//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines functionality to support driver managed builds for
/// compilations which use Clang modules or standard C++20 named modules.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_DRIVER_MODULESDRIVER_H
#define LLVM_CLANG_DRIVER_MODULESDRIVER_H

#include "clang/Basic/LLVM.h"
#include "clang/Driver/Types.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystemFwd.h"

namespace clang {
class DiagnosticsEngine;
namespace driver {
class Compilation;
} // namespace driver
} // namespace clang

namespace clang::driver::modules {

/// The parsed Standard library module manifest.
struct StdModuleManifest {
  struct Module {
    struct LocalArguments {
      std::vector<std::string> SystemIncludeDirs;
    };

    bool IsStdlib = false;
    std::string LogicalName;
    std::string SourcePath;
    std::optional<LocalArguments> LocalArgs;
  };

  std::vector<Module> Modules;
};

/// Reads the Standard library module manifest at \p ManifestPath.
///
/// Assumes that all file paths specified in the manifest are relative to
/// \p ManifestPath and converts them to absolute.
///
/// \returns The parsed manifest on success; otherwise, a \c llvm::FileError
/// or \c llvm::json::ParseError.
llvm::Expected<StdModuleManifest>
readStdModuleManifest(llvm::StringRef ManifestPath, llvm::vfs::FileSystem &VFS);

/// Scans the compilation inputs for module dependencies and adjusts the
/// compilation to build and supply those modules as required.
///
/// \param C The Compilation being built.
/// \param ManifestEntries The entries of all module manifests. The modules they
/// describe are built on demand if imported by any input.
void runModulesDriver(Compilation &C,
                      ArrayRef<StdModuleManifest::Module> ManifestEntries);

} // namespace clang::driver::modules

#endif // LLVM_CLANG_DRIVER_MODULESDRIVER_H
