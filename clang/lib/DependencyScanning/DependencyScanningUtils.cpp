//===- DependencyScanningUtils.cpp - Common Scanning Utilities ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/DependencyScanning/DependencyScanningUtils.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang;
using namespace dependencies;

std::pair<IntrusiveRefCntPtr<llvm::vfs::FileSystem>, std::vector<std::string>>
dependencies::initVFSForByNameScanning(ArrayRef<std::string> CommandLine) {
  // The fake input buffer is read-only, and it is used to produce unique source
  // locations for the diagnostics. Therefore, sharing this global buffer across
  // threads is ok.
  static const std::string FakeInput(
      DependencyScanningWorker::MaxNumOfByNameQueries, ' ');

  StringRef InputPath =
      llvm::sys::path::is_style_windows(llvm::sys::path::Style::native)
          ? "Z:\\module-include.input"
          : "/module-include.input";
  auto InputBuf = llvm::MemoryBuffer::getMemBuffer(FakeInput, InputPath);

  auto FS = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
  FS->addFile(InputPath, 0, std::move(InputBuf));

  std::vector<std::string> ModifiedCommandLine(CommandLine);
  ModifiedCommandLine.emplace_back(InputPath);

  return std::make_pair(std::move(FS), ModifiedCommandLine);
}

TranslationUnitDeps FullDependencyConsumer::takeTranslationUnitDeps() {
  TranslationUnitDeps TU;

  TU.ID.ContextHash = std::move(ContextHash);
  TU.ID.ModuleName = std::move(ModuleName);
  TU.NamedModuleDeps = std::move(NamedModuleDeps);
  TU.FileDeps = std::move(Dependencies);
  TU.PrebuiltModuleDeps = std::move(PrebuiltModuleDeps);
  TU.VisibleModules = std::move(VisibleModules);
  TU.Commands = std::move(Commands);

  for (auto &&M : ClangModuleDeps) {
    auto &MD = M.second;
    // TODO: Avoid handleModuleDependency even being called for modules
    //   we've already seen.
    if (AlreadySeen.count(M.first))
      continue;
    TU.ModuleGraph.push_back(std::move(MD));
  }
  TU.ClangModuleDeps = std::move(DirectModuleDeps);

  return TU;
}

CallbackActionController::~CallbackActionController() {}
