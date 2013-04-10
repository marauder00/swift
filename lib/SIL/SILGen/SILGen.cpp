//===--- SILGen.cpp - Implements Lowering of ASTs -> SIL ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGen.h"
#include "llvm/ADT/Optional.h"
#include "swift/AST/AST.h"
#include "swift/SIL/SILArgument.h"
#include "swift/Subsystems.h"
#include "llvm/Support/Debug.h"
using namespace swift;
using namespace Lowering;

//===--------------------------------------------------------------------===//
// SILGenFunction Class implementation
//===--------------------------------------------------------------------===//

// TODO: more accurately port the result schema logic from
// IRGenFunction::emitEpilogue to handle all cases where a default void return
// is needed
static bool isVoidableType(Type type) {
  return type->isEqual(type->getASTContext().TheEmptyTupleType);
}

SILGenFunction::SILGenFunction(SILGenModule &SGM, SILFunction &F,
                               bool hasVoidReturn)
  : SGM(SGM), F(F), B(new (F.getModule()) SILBasicBlock(&F), F),
    Cleanups(*this),
    hasVoidReturn(hasVoidReturn),
    epilogBB(nullptr) {
}

/// SILGenFunction destructor - called after the entire function's AST has been
/// visited.  This handles "falling off the end of the function" logic.
SILGenFunction::~SILGenFunction() {
  // If the end of the function isn't reachable (e.g. it ended in an explicit
  // return), then we're done.
  if (!B.hasValidInsertionPoint())
    return;
  
  // If we have an unterminated block, it is either an implicit return of an
  // empty tuple, or a dynamically unreachable location.
  if (hasVoidReturn) {
    assert(!epilogBB && "epilog bb not terminated?!");
    SILValue emptyTuple = emitEmptyTuple(SILLocation());
    Cleanups.emitReturnAndCleanups(SILLocation(), emptyTuple);
  } else {
    B.createUnreachable();
  }
}

//===--------------------------------------------------------------------===//
// SILGenModule Class implementation
//===--------------------------------------------------------------------===//

SILGenModule::SILGenModule(SILModule &M)
  : M(M), Types(M.Types), TopLevelSGF(nullptr) {
  if (M.toplevel) {
    TopLevelSGF = new SILGenFunction(*this, *M.toplevel,
                                     /*hasVoidReturn=*/true);
  }
}

SILGenModule::~SILGenModule() {
  delete TopLevelSGF;
  if (M.toplevel) {
    DEBUG(llvm::dbgs() << "lowered toplevel sil:\n";
          M.toplevel->print(llvm::dbgs()));
    M.toplevel->verify();
  }
}

SILType SILGenModule::getConstantType(SILConstant constant) {
  return Types.getConstantType(constant);
}

void SILGenModule::visitFuncDecl(FuncDecl *fd) {
  emitFunction(fd, fd->getBody());
}

template<typename T>
SILFunction *SILGenModule::preEmitFunction(SILConstant constant, T *astNode) {
  assert(!M.hasFunction(constant) &&
         "already generated function for constant!");
  
  DEBUG(llvm::dbgs() << "lowering ";
        constant.print(llvm::dbgs());
        llvm::dbgs() << " : $";
        getConstantType(constant).print(llvm::dbgs());
        llvm::dbgs() << '\n';
        if (astNode) {
          astNode->print(llvm::dbgs());
          llvm::dbgs() << '\n';
        });
  
  return new (M) SILFunction(M, getConstantType(constant));
}

void SILGenModule::postEmitFunction(SILConstant constant,
                                    SILFunction *F) {

  DEBUG(llvm::dbgs() << "lowered sil:\n";
        F->print(llvm::dbgs()));
  F->verify();
  M.functions[constant] = F;
}

SILFunction *SILGenModule::emitFunction(SILConstant::Loc decl, FuncExpr *fe) {
  // Ignore prototypes.
  if (fe->getBody() == nullptr) return nullptr;
  
  SILConstant constant(decl);
  SILFunction *f = preEmitFunction(constant, fe);
  bool hasVoidReturn = isVoidableType(fe->getResultType(f->getContext()));
  SILGenFunction(*this, *f, hasVoidReturn).emitFunction(fe);
  postEmitFunction(constant, f);
  
  return f;
}

void SILGenModule::addGlobalVariable(VarDecl *global) {
  M.globals.insert(global);
}

SILFunction *SILGenModule::emitConstructor(ConstructorDecl *decl) {
  // Ignore prototypes.
  // FIXME: generate default constructor, which appears in the AST as a
  // prototype
  if (decl->getBody() == nullptr) return nullptr;

  SILConstant constant(decl);
  SILFunction *f = preEmitFunction(constant, decl);
  
  if (decl->getImplicitThisDecl()->getType()->hasReferenceSemantics()) {
    // Class constructors have separate entry points for allocation and
    // initialization.
    SILGenFunction(*this, *f, /*hasVoidReturn=*/true)
      .emitClassConstructorAllocator(decl);
    postEmitFunction(constant, f);
    
    SILConstant initConstant(decl, SILConstant::Kind::Initializer);
    SILFunction *initF = preEmitFunction(initConstant, decl);
    SILGenFunction(*this, *initF, /*hasVoidReturn=*/true)
      .emitClassConstructorInitializer(decl);
    postEmitFunction(initConstant, initF);
  } else {
    // Struct constructors do everything in a single function.
    SILGenFunction(*this, *f, /*hasVoidReturn=*/true)
      .emitValueConstructor(decl);
    postEmitFunction(constant, f);
  }
  
  return f;
}

SILFunction *SILGenModule::emitClosure(ClosureExpr *ce) {
  SILConstant constant(ce);
  SILFunction *f = preEmitFunction(constant, ce);
  SILGenFunction(*this, *f, /*hasVoidReturn=*/false).emitClosure(ce);
  postEmitFunction(constant, f);
  
  return f;
}

SILFunction *SILGenModule::emitDestructor(ClassDecl *cd,
                                       DestructorDecl /*nullable*/ *dd) {
  SILConstant constant(cd, SILConstant::Kind::Destructor);
  
  SILFunction *f = preEmitFunction(constant, dd);
  SILGenFunction(*this, *f, /*hasVoidReturn=*/true).emitDestructor(cd, dd);
  postEmitFunction(constant, f);
  
  return f;
}

void SILGenModule::visitPatternBindingDecl(PatternBindingDecl *pd) {
  // Emit initializers for variables in top-level code.
  if (TopLevelSGF) {
    if (!TopLevelSGF->B.hasValidInsertionPoint())
      return;
    
    TopLevelSGF->visit(pd);
  }
  
  // FIXME: generate accessor functions for global variables
}

//===--------------------------------------------------------------------===//
// SILModule::constructSIL method implementation
//===--------------------------------------------------------------------===//


SILModule *SILModule::constructSIL(TranslationUnit *tu, unsigned startElem) {
  bool hasTopLevel = true;
  switch (tu->Kind) {
  case TranslationUnit::Library:
    hasTopLevel = false;
    break;
  case TranslationUnit::Main:
  case TranslationUnit::Repl:
    hasTopLevel = true;
    break;
  }
  SILModule *m = new SILModule(tu->getASTContext(), hasTopLevel);
  SILGenModule sgm(*m);
  for (Decl *D : llvm::makeArrayRef(tu->Decls).slice(startElem))
    sgm.visit(D);
  
  // Emit external definitions from Clang modules.
  // FIXME: O(n^2), since the same Clang module gets seen through multiple TUs
  for (auto mod : tu->getASTContext().LoadedClangModules) {
    for (auto &def : mod->getExternalDefinitions()) {
      switch (def.getStage()) {
        case ExternalDefinition::NameBound:
          llvm_unreachable("external definition not type-checked");
          
        case ExternalDefinition::TypeChecked:
          // FIXME: We should emit this definition only if it's actually needed.
          sgm.emitExternalDefinition(def.getDecl());
          break;
      }
    }
  }

  return m;
}

SILModule *swift::performSILGeneration(TranslationUnit *tu,
                                       unsigned startElem) {
  return SILModule::constructSIL(tu, startElem);
}
