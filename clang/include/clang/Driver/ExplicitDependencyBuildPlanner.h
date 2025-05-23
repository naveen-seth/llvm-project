//===- ExplicitModuleBuildPlanner.h - Modules Aware Build Planner  ----*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------------===//

#ifndef LLVM_CLANG_DRIVER_EXPLICIT_MODULE_BUILD_PLANNER_H
#define LLVM_CLANG_DRIVER_EXPLICIT_MODULE_BUILD_PLANNER_H

#include "clang/Driver/Compilation.h"
#include "clang/Driver/Types.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Option/Arg.h"
#include <utility>

namespace clang {
namespace driver {

class ExplicitModuleBuildPlanner {
  /// An input type and its arguments.
  using InputTy = std::pair<types::ID, const llvm::opt::Arg *>;

  /// A list of inputs and their types for the given arguments.
  using InputList = llvm::SmallVector<InputTy, 16>;

  /// List of inputs which don't make use of C++20 modules
  InputList NonModuleInputs;

  /// List of inputs which use C++20 modules
  InputList ModuleInputs;

  /// Checks for C++20 module presence and populates `NonModuleInputs` and
  /// `ModuleInputs` accordingly.
  void CheckInputsForModulePresence(const InputList &Inputs);

public:
  /// BuildActions - Constructs a list of actions to perform for the given
  void BuildActions(Compilation &C, const InputList &Inputs);
};

} // namespace driver
} // namespace clang

#endif // LLVM_CLANG_DRIVER_EXPLICIT_MODULE_BUILD_PLANNER_H
