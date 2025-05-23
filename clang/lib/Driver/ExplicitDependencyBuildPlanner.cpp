//===--- ExplicitModuelBuildPlanner.cpp - Modules Aware Build Planner
//------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===-----------------------------------------------------------------------===//

#include "clang/Driver/ExplicitDependencyBuildPlanner.h"
#include "clang/Driver/Compilation.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang::driver;

void ExplicitModuleBuildPlanner::CheckInputsForModulePresence(
    const InputList &Inputs) {
  llvm::errs() << "remark: entered module check\n";
}

void ExplicitModuleBuildPlanner::BuildActions(Compilation &C,
                                              const InputList &Inputs) {
  llvm::errs() << "remark: using explicit module build planner\n";
  CheckInputsForModulePresence(Inputs);
}
