//===--- ModulesDriver.cpp - Driver managed module builds -----------------===//
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

#include "clang/Driver/ModulesDriver.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/LLVM.h"
#include "clang/DependencyScanning/DependencyScanningUtils.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Driver/Types.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/StandaloneDiagnostic.h"
#include "clang/Lex/DependencyDirectivesScanner.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/DirectedGraph.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVectorExtras.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <utility>

namespace deps = clang::dependencies;

using namespace llvm::opt;
using namespace clang;
using namespace driver;
using namespace modules;

namespace clang::driver::modules {
static bool fromJSON(const llvm::json::Value &Params,
                     StdModuleManifest::Module::LocalArguments &LocalArgs,
                     llvm::json::Path P) {
  llvm::json::ObjectMapper O(Params, P);
  return O.mapOptional("system-include-directories",
                       LocalArgs.SystemIncludeDirs);
}

static bool fromJSON(const llvm::json::Value &Params,
                     StdModuleManifest::Module &ModuleEntry,
                     llvm::json::Path P) {
  llvm::json::ObjectMapper O(Params, P);
  return O.map("is-std-library", ModuleEntry.IsStdlib) &&
         O.map("logical-name", ModuleEntry.LogicalName) &&
         O.map("source-path", ModuleEntry.SourcePath) &&
         O.mapOptional("local-arguments", ModuleEntry.LocalArgs);
}

static bool fromJSON(const llvm::json::Value &Params,
                     StdModuleManifest &Manifest, llvm::json::Path P) {
  llvm::json::ObjectMapper O(Params, P);
  return O.map("modules", Manifest.Modules);
}
} // namespace clang::driver::modules

/// Parses the Standard library module manifest from \p Buffer.
static Expected<StdModuleManifest> parseManifest(StringRef Buffer) {
  auto ParsedOrErr = llvm::json::parse(Buffer);
  if (!ParsedOrErr)
    return ParsedOrErr.takeError();

  StdModuleManifest Manifest;
  llvm::json::Path::Root Root;
  if (!fromJSON(*ParsedOrErr, Manifest, Root))
    return Root.getError();

  return Manifest;
}

/// Converts each file path in manifest from relative to absolute.
///
/// Each file path in the manifest is expected to be relative the manifest's
/// location \p ManifestPath itself.
static void makeManifestPathsAbsolute(
    MutableArrayRef<StdModuleManifest::Module> ManifestEntries,
    StringRef ManifestPath) {
  StringRef ManifestDir = llvm::sys::path::parent_path(ManifestPath);
  SmallString<256> TempPath;

  auto PrependManifestDir = [&](StringRef Path) {
    TempPath = ManifestDir;
    llvm::sys::path::append(TempPath, Path);
    return std::string(TempPath);
  };

  for (auto &Entry : ManifestEntries) {
    Entry.SourcePath = PrependManifestDir(Entry.SourcePath);
    if (!Entry.LocalArgs)
      continue;

    for (auto &IncludeDir : Entry.LocalArgs->SystemIncludeDirs)
      IncludeDir = PrependManifestDir(IncludeDir);
  }
}

Expected<StdModuleManifest>
driver::modules::readStdModuleManifest(StringRef ManifestPath,
                                       llvm::vfs::FileSystem &VFS) {
  auto MemBufOrErr = VFS.getBufferForFile(ManifestPath);
  if (!MemBufOrErr)
    return llvm::createFileError(ManifestPath, MemBufOrErr.getError());

  auto ManifestOrErr = parseManifest((*MemBufOrErr)->getBuffer());
  if (!ManifestOrErr)
    return ManifestOrErr.takeError();
  auto Manifest = std::move(*ManifestOrErr);

  makeManifestPathsAbsolute(Manifest.Modules, ManifestPath);
  return Manifest;
}

static bool isCC1Job(const Command &Job) {
  return StringRef(Job.getCreator().getName()) == "clang";
}

/// Computes the -fmodule-cache-path for this compilation.
static std::optional<std::string>
getModuleCachePath(llvm::opt::DerivedArgList &Args) {
  if (const Arg *A = Args.getLastArg(options::OPT_fmodules_cache_path))
    return A->getValue();

  if (SmallString<128> Path; Driver::getDefaultModuleCachePath(Path))
    return std::string(Path);

  return std::nullopt;
}

/// Returns true if a dependency scan can be performed using \p Job.
static bool isDependencyScannableJob(const Command &Job) {
  if (!isCC1Job(Job))
    return false;
  const auto &InputInfos = Job.getInputInfos();
  return !InputInfos.empty() && types::isSrcFile(InputInfos.front().getType());
}

namespace {
/// Pool of reusable dependency scanning workers and their contexts with
/// RAII-based acquire/release.
class ScanningWorkerPool {
public:
  ScanningWorkerPool(size_t NumWorkers,
                     deps::DependencyScanningService &ScanningService) {
    for (size_t I = 0; I < NumWorkers; ++I)
      Slots.emplace_back(ScanningService);

    AvailableSlots.resize(NumWorkers);
    std::iota(AvailableSlots.begin(), AvailableSlots.end(), 0);
  }

  /// Acquires a unique pointer to a dependency scanning worker and its
  /// context.
  ///
  /// The worker bundle automatically released back to the pool when the
  /// pointer is destroyed. The pool has to outlive the leased worker bundle.
  [[nodiscard]] auto scopedAcquire() {
    std::unique_lock<std::mutex> UL(Lock);
    CV.wait(UL, [&] { return !AvailableSlots.empty(); });
    const size_t Index = AvailableSlots.pop_back_val();
    auto ReleaseHandle = [this, Index](WorkerBundle *) { release(Index); };
    return std::unique_ptr<WorkerBundle, decltype(ReleaseHandle)>(
        &Slots[Index], ReleaseHandle);
  }

private:
  /// Releases the worker bundle at \c Index back into the pool.
  void release(size_t Index) {
    {
      std::scoped_lock<std::mutex> SL(Lock);
      AvailableSlots.push_back(Index);
    }
    CV.notify_one();
  }

  /// A scanning worker with its associated context.
  struct WorkerBundle {
    WorkerBundle(deps::DependencyScanningService &ScanningService)
        : Worker(std::make_unique<deps::DependencyScanningWorker>(
              ScanningService)) {}

    std::unique_ptr<deps::DependencyScanningWorker> Worker;
  };

  std::mutex Lock;
  std::condition_variable CV;
  SmallVector<size_t> AvailableSlots;
  SmallVector<WorkerBundle, 0> Slots;
};
} // anonymous namespace

// Creates a ThreadPool and a corresponding ScanningWorkerPool for scanning
// \p NumScanInputs inputs.
static std::pair<std::unique_ptr<llvm::ThreadPoolInterface>,
                 std::unique_ptr<ScanningWorkerPool>>
createOptimalThreadAndWorkerPool(
    size_t NumScanInputs, deps::DependencyScanningService &ScanningService) {
  // TODO: Benchmark: Determine the optimal number of worker threads for a
  // given number of inputs. How many inputs are required for multi-threading
  // to be beneficial? How many inputs should each thread scan at least?
#if LLVM_ENABLE_THREADS
  std::unique_ptr<llvm::ThreadPoolInterface> ThreadPool;
  size_t WorkerCount;

  if (NumScanInputs <= 1) {
    auto S = llvm::optimal_concurrency(1);
    ThreadPool = std::make_unique<llvm::SingleThreadExecutor>(std::move(S));
    WorkerCount = 1;
  } else {
    ThreadPool = std::make_unique<llvm::DefaultThreadPool>(
        llvm::optimal_concurrency(NumScanInputs));
    WorkerCount =
        std::min<size_t>(ThreadPool->getMaxConcurrency(), NumScanInputs);
  }
#else
  auto ThreadPool = std::make_unique<llvm::SingleThreadExecutor>();
  size_t WorkerCount = 1;
#endif

  return {std::move(ThreadPool),
          std::make_unique<ScanningWorkerPool>(WorkerCount, ScanningService)};
}

static StringRef getTriple(const Command &Job) {
  return Job.getCreator().getToolChain().getTriple().getTriple();
}

using ModuleNameAndTriple = std::pair<StringRef, StringRef>;

namespace {
/// Collects diagnostics in a form that can be retained until after their
/// associated SourceManager is destroyed.
class StandaloneDiagCollector : public DiagnosticConsumer {
public:
  void BeginSourceFile(const LangOptions &LangOpts,
                       const Preprocessor *PP = nullptr) override {
    this->LangOpts = &LangOpts;
  }

  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    StoredDiagnostic StoredDiag(Level, Info);
    StandaloneDiags.emplace_back(*LangOpts, StoredDiag);
    DiagnosticConsumer::HandleDiagnostic(Level, Info);
  }

  SmallVector<StandaloneDiagnostic, 0> takeDiagnostics() {
    return std::move(StandaloneDiags);
  }

private:
  const LangOptions *LangOpts = nullptr;
  SmallVector<StandaloneDiagnostic, 0> StandaloneDiags;
};

/// RAII utility to report collected StandaloneDiagnostic through a
/// DiagnosticsEngine.
///
/// The driver's DiagnosticsEngine usually does not have a SourceManager at
/// this point of building the compilation, in which case the
/// StandaloneDiagReporter supplies its own.
class StandaloneDiagReporter {
public:
  explicit StandaloneDiagReporter(DiagnosticsEngine &Diags) : Diags(Diags) {
    if (!Diags.hasSourceManager()) {
      FileSystemOptions Opts;
      Opts.WorkingDir = ".";
      OwnedFileMgr = llvm::makeIntrusiveRefCnt<FileManager>(std::move(Opts));
      OwnedSrcMgr =
          llvm::makeIntrusiveRefCnt<SourceManager>(Diags, *OwnedFileMgr);
    }
  }

  /// Emits all diagnostics in \c StandaloneDiags using the associated
  /// DiagnosticsEngine.
  void Report(ArrayRef<StandaloneDiagnostic> StandaloneDiags) const {
    llvm::StringMap<SourceLocation> SrcLocCache;
    Diags.getClient()->BeginSourceFile(LangOptions(), nullptr);
    for (const auto &StandaloneDiag : StandaloneDiags) {
      const auto StoredDiag = translateStandaloneDiag(
          getFileManager(), getSourceManager(), StandaloneDiag, SrcLocCache);
      Diags.Report(StoredDiag);
    }
    Diags.getClient()->EndSourceFile();
  }

private:
  DiagnosticsEngine &Diags;
  IntrusiveRefCntPtr<FileManager> OwnedFileMgr;
  IntrusiveRefCntPtr<SourceManager> OwnedSrcMgr;

  FileManager &getFileManager() const {
    if (OwnedFileMgr)
      return *OwnedFileMgr;
    return Diags.getSourceManager().getFileManager();
  }

  SourceManager &getSourceManager() const {
    if (OwnedSrcMgr)
      return *OwnedSrcMgr;
    return Diags.getSourceManager();
  }
};
} // anonymous namespace

/// Report the diagnostics collected during each dependency scan.
static void reportAllScanDiagnostics(
    SmallVectorImpl<SmallVector<StandaloneDiagnostic, 0>> &&AllScanDiags,
    const StandaloneDiagReporter &Reporter) {
  for (auto &SingleScanDiags : AllScanDiags)
    Reporter.Report(SingleScanDiags);
}

/// Construct a path for the explicitly built PCM.
static std::string constructPCMPath(const deps::ModuleID &ID,
                                    StringRef OutputDir) {
  assert(!ID.ModuleName.empty() && !ID.ContextHash.empty() &&
         "Invalid ModuleID!");
  // Sanitize the ':' included in partition names. It is illegal for filenames
  // on Windows.
  SmallString<32> SanitizedModuleName(ID.ModuleName);
  llvm::replace(SanitizedModuleName, ':', '-');
  SmallString<256> ExplicitPCMPath(OutputDir);
  llvm::sys::path::append(ExplicitPCMPath, ID.ContextHash,
                          SanitizedModuleName + "-" + ID.ContextHash + ".pcm");
  return std::string(ExplicitPCMPath);
}

namespace {
/// Describes where a C++20 named module that inputs of this compilation may
/// import is defined.
struct NamedModuleProvider {
  /// The interface source of the module, used to build the module implicitly
  /// while scanning.
  HeaderSearchOptions::NamedModuleSource Source;

  /// If the module is defined by a command-line input, the path of the BMI
  /// used by importers. Otherwise, the module is built on demand with a
  /// dedicated BMI-only job, and its object code is expected to be in a
  /// prebuilt library.
  std::string OwnedBMIPath;

  /// If the module is defined by a command-line input, the path of the full
  /// BMI that the input's object file is compiled from. With Reduced BMIs,
  /// \c OwnedBMIPath is emitted as a by-product of building this BMI.
  std::string CodeGenBMIPath;

  bool isOwnedByInput() const { return !OwnedBMIPath.empty(); }
};

/// The named module providers available to inputs, by module name.
using NamedModuleProviderMap = std::map<std::string, NamedModuleProvider>;

/// The dependency action controller used by the modules driver.
///
/// Provides the output paths of discovered modules and makes the named module
/// providers known to the dependency scanner, so that imported named modules
/// are built implicitly during the scan.
class ModulesDriverActionController : public deps::DependencyActionController {
public:
  ModulesDriverActionController(StringRef OutputDir,
                                const NamedModuleProviderMap *Providers)
      : OutputDir(OutputDir), Providers(Providers) {}

  std::string lookupModuleOutput(const deps::ModuleDeps &MD,
                                 deps::ModuleOutputKind Kind) override {
    if (Kind == deps::ModuleOutputKind::ModuleFile) {
      if (const auto *Provider = getOwnedProvider(MD))
        return Provider->OwnedBMIPath;
      return constructPCMPath(MD.ID, OutputDir);
    }

    // Driver command lines that trigger lookups for unsupported
    // ModuleOutputKinds are not supported by the modules driver. Those
    // command lines should probably be adjusted or rejected in
    // Driver::handleArguments or Driver::HandleImmediateArgs.
    llvm::reportFatalInternalError(
        "call to lookupModuleOutput with unexpected ModuleOutputKind");
  }

  std::string
  lookupNamedModuleCodeGenOutput(const deps::ModuleDeps &MD) override {
    if (const auto *Provider = getOwnedProvider(MD))
      return Provider->CodeGenBMIPath;
    return {};
  }

  void initializeScanInvocation(CompilerInvocation &ScanInvocation) override {
    if (!Providers || Providers->empty())
      return;

    HeaderSearchOptions &HSOpts = ScanInvocation.getHeaderSearchOpts();
    for (const auto &[Name, Provider] : *Providers)
      HSOpts.NamedModuleSources[Name] = Provider.Source;
    // Implicitly built named modules are stored in the module cache.
    if (HSOpts.ModuleCachePath.empty())
      HSOpts.ModuleCachePath = OutputDir.str();
  }

  std::unique_ptr<DependencyActionController> clone() const override {
    return std::make_unique<ModulesDriverActionController>(OutputDir,
                                                           Providers);
  }

  /// Returns the provider of \p MD if it is a named module defined by a
  /// command-line input.
  const NamedModuleProvider *
  getOwnedProvider(const deps::ModuleDeps &MD) const {
    if (!MD.IsCXXNamedModule || !Providers)
      return nullptr;
    const auto It = Providers->find(MD.ID.ModuleName);
    if (It == Providers->end() || !It->second.isOwnedByInput())
      return nullptr;
    return &It->second;
  }

private:
  StringRef OutputDir;
  const NamedModuleProviderMap *Providers;
};

/// The full dependencies for a specific command-line input.
struct InputDependencies {
  /// The name of the C++20 module provided by this translation unit.
  std::string ModuleName;

  /// A list of modules this translation unit directly depends on, not including
  /// transitive dependencies.
  ///
  /// This may include modules with a different context hash when it can be
  /// determined that the differences are benign for this compilation.
  std::vector<deps::ModuleID> ClangModuleDeps;

  /// A list of the C++20 named modules this translation unit depends on.
  ///
  /// These correspond only to modules built with compatible compiler
  /// invocations.
  std::vector<std::string> NamedModuleDeps;

  /// A collection of absolute paths to files that this translation unit
  /// directly depends on, not including transitive dependencies.
  std::vector<std::string> FileDeps;

  /// The compiler invocation with modifications to properly import all Clang
  /// module dependencies. Does not include argv[0].
  std::vector<std::string> BuildArgs;
};
} // anonymous namespace

static InputDependencies makeInputDeps(deps::TranslationUnitDeps &&TUDeps) {
  InputDependencies InputDeps;
  InputDeps.ModuleName = std::move(TUDeps.ID.ModuleName);
  InputDeps.NamedModuleDeps = std::move(TUDeps.NamedModuleDeps);
  InputDeps.ClangModuleDeps = std::move(TUDeps.ClangModuleDeps);
  InputDeps.FileDeps = std::move(TUDeps.FileDeps);
  assert(TUDeps.Commands.size() == 1 && "Expected exactly one command");
  InputDeps.BuildArgs = std::move(TUDeps.Commands.front().Arguments);
  return InputDeps;
}

/// Makes the input dependencies of a job building the named module
/// \p ModuleName, from the by-name scan result \p TUDeps of that module.
static std::optional<InputDependencies>
makeInputDepsForOwnedModule(const deps::TranslationUnitDeps &TUDeps,
                            StringRef ModuleName) {
  const auto IsModule = [&](const deps::ModuleDeps &MD) {
    return MD.IsCXXNamedModule && MD.ID.ModuleName == ModuleName;
  };
  const auto It = llvm::find_if(TUDeps.ModuleGraph, IsModule);
  if (It == TUDeps.ModuleGraph.end())
    return std::nullopt;

  InputDependencies InputDeps;
  InputDeps.ModuleName = ModuleName.str();
  InputDeps.ClangModuleDeps = It->ClangModuleDeps;
  for (const deps::ModuleID &Dep : It->ClangModuleDeps)
    if (llvm::any_of(TUDeps.ModuleGraph, [&](const deps::ModuleDeps &MD) {
          return MD.ID == Dep && MD.IsCXXNamedModule;
        }))
      InputDeps.NamedModuleDeps.push_back(Dep.ModuleName);
  InputDeps.BuildArgs = It->getBuildArguments();
  return InputDeps;
}

/// Constructs the full command line, including the executable, for \p Job.
static SmallVector<std::string, 0> buildCommandLine(const Command &Job) {
  const auto &JobArgs = Job.getArguments();
  SmallVector<std::string, 0> CommandLine;
  CommandLine.reserve(JobArgs.size() + 1);
  CommandLine.emplace_back(Job.getExecutable());
  for (const char *Arg : JobArgs)
    CommandLine.emplace_back(Arg);
  return CommandLine;
}

namespace {
/// A dependency scan of a job.
struct ScanRequest {
  const Command *Job;

  /// If set, the job builds this named module, which is scanned by name
  /// instead of scanning the job as a translation unit.
  std::optional<std::string> ModuleName;
};

/// Collects the dependencies of a single by-name query.
class ByNameDependencyConsumer : public deps::FullDependencyConsumer {
public:
  using FullDependencyConsumer::FullDependencyConsumer;

  void finishQuery(StringRef ModuleName, bool Success) override {
    Succeeded = Success;
  }

  bool Succeeded = false;
};
} // anonymous namespace

/// Performs a dependency scan for a single request.
///
/// \returns a pair containing TranslationUnitDeps on success, or std::nullopt
/// on failure, along with any diagnostics produced. For by-name scans, the
/// TranslationUnitDeps contain only the module graph.
static std::pair<std::optional<deps::TranslationUnitDeps>,
                 SmallVector<StandaloneDiagnostic, 0>>
scanDependenciesForJob(const ScanRequest &Request,
                       ScanningWorkerPool &WorkerPool,
                       StringRef WorkingDirectory,
                       deps::DependencyActionController &Controller) {
  StandaloneDiagCollector DiagConsumer;
  std::optional<deps::TranslationUnitDeps> MaybeTUDeps;

  {
    auto CC1CommandLine = buildCommandLine(*Request.Job);
    auto WorkerBundleHandle = WorkerPool.scopedAcquire();
    llvm::DenseSet<deps::ModuleID> AlreadySeen;

    if (!Request.ModuleName) {
      deps::FullDependencyConsumer DepConsumer(AlreadySeen);
      if (WorkerBundleHandle->Worker->computeDependencies(
              WorkingDirectory, {CC1CommandLine}, DepConsumer, Controller,
              DiagConsumer))
        MaybeTUDeps = DepConsumer.takeTranslationUnitDeps();
    } else {
      // Use the job's own command line, without its input, as the context to
      // build the module in.
      StringRef Input = Request.Job->getInputInfos().front().getFilename();
      if (auto It = llvm::find(llvm::reverse(CC1CommandLine), Input);
          It != CC1CommandLine.rend())
        CC1CommandLine.erase(std::next(It).base());
      auto [OverlayFS, ByNameCommandLine] =
          deps::initVFSForByNameScanning(CC1CommandLine);

      ByNameDependencyConsumer DepConsumer(AlreadySeen);
      bool Queried = false;
      auto GetNextName = [&]() -> std::optional<std::string> {
        if (std::exchange(Queried, true))
          return std::nullopt;
        return *Request.ModuleName;
      };
      if (WorkerBundleHandle->Worker->computeDependenciesByName(
              WorkingDirectory, ByNameCommandLine, std::move(OverlayFS),
              DiagConsumer, Controller, GetNextName, DepConsumer) &&
          DepConsumer.Succeeded)
        MaybeTUDeps = DepConsumer.takeTranslationUnitDeps();
    }
  }

  return {std::move(MaybeTUDeps), DiagConsumer.takeDiagnostics()};
}

/// Performs the scans \p Requests, in parallel.
///
/// \p GetController returns the action controller to use for a given job.
///
/// \returns the scan results in the order of \p Requests, or std::nullopt
/// on failure, with diagnostics reported via \p DiagReporter in both cases.
static std::optional<SmallVector<deps::TranslationUnitDeps, 0>>
scanJobs(ArrayRef<ScanRequest> Requests,
         deps::DependencyScanningService &ScanningService,
         llvm::function_ref<deps::DependencyActionController &(const Command &)>
             GetController,
         StringRef WorkingDirectory,
         const StandaloneDiagReporter &DiagReporter) {
  llvm::PrettyStackTraceString CrashInfo("Performing module dependency scan.");

  const size_t NumScanInputs = Requests.size();
  std::unique_ptr<llvm::ThreadPoolInterface> ThreadPool;
  std::unique_ptr<ScanningWorkerPool> WorkerPool;
  std::tie(ThreadPool, WorkerPool) =
      createOptimalThreadAndWorkerPool(NumScanInputs, ScanningService);

  SmallVector<std::optional<deps::TranslationUnitDeps>, 0> AllScanResults(
      NumScanInputs);
  SmallVector<SmallVector<StandaloneDiagnostic, 0>, 0> AllScanDiags(
      NumScanInputs);
  std::atomic<bool> HasError{false};

  for (size_t ScanIndex = 0; ScanIndex < NumScanInputs; ++ScanIndex) {
    const ScanRequest &Request = Requests[ScanIndex];
    deps::DependencyActionController &Controller = GetController(*Request.Job);
    ThreadPool->async([&, ScanIndex]() {
      auto [MaybeTUDeps, ScanDiags] = scanDependenciesForJob(
          Request, *WorkerPool, WorkingDirectory, Controller);
      AllScanDiags[ScanIndex] = std::move(ScanDiags);
      if (!MaybeTUDeps)
        HasError.store(true, std::memory_order_relaxed);
      AllScanResults[ScanIndex] = std::move(MaybeTUDeps);
    });
  }
  ThreadPool->wait();

  reportAllScanDiagnostics(std::move(AllScanDiags), DiagReporter);
  if (HasError.load(std::memory_order_relaxed))
    return std::nullopt;

  SmallVector<deps::TranslationUnitDeps, 0> Results;
  Results.reserve(NumScanInputs);
  for (auto &MaybeTUDeps : AllScanResults)
    Results.push_back(std::move(*MaybeTUDeps));
  return Results;
}

namespace {
class CGNode;
class CGEdge;
using CGNodeBase = llvm::DGNode<CGNode, CGEdge>;
using CGEdgeBase = llvm::DGEdge<CGNode, CGEdge>;
using CGBase = llvm::DirectedGraph<CGNode, CGEdge>;

/// Compilation Graph Node
class CGNode : public CGNodeBase {
public:
  enum class NodeKind {
    ClangModuleCC1Job,
    NamedModuleCC1Job,
    NonModuleCC1Job,
    MiscJob,
    ImageJob,
    Root,
  };

  CGNode(const NodeKind K) : Kind(K) {}
  CGNode(const CGNode &) = delete;
  CGNode(CGNode &&) = delete;
  CGNode &operator=(const CGNode &) = delete;
  CGNode &operator=(CGNode &&) = delete;
  virtual ~CGNode() = 0;

  NodeKind getKind() const { return Kind; }

private:
  NodeKind Kind;
};
CGNode::~CGNode() = default;

/// Subclass of CGNode representing the root node of the graph.
///
/// The root node is a special node that connects to all other nodes with
/// no incoming edges, so that there is always a path from it to any node
/// in the graph.
///
/// There should only be one such node in a given graph.
class RootNode : public CGNode {
public:
  RootNode() : CGNode(NodeKind::Root) {}
  ~RootNode() override = default;

  static bool classof(const CGNode *N) {
    return N->getKind() == NodeKind::Root;
  }
};

/// Base class for any CGNode type that represents a job.
class JobNode : public CGNode {
public:
  JobNode(std::unique_ptr<Command> &&Job, NodeKind Kind)
      : CGNode(Kind), Job(std::move(Job)) {
    assert(this->Job && "Expected valid job!");
  }
  virtual ~JobNode() override = 0;

  std::unique_ptr<Command> Job;

  static bool classof(const CGNode *N) {
    return N->getKind() != NodeKind::Root;
  }
};
JobNode::~JobNode() = default;

/// Subclass of CGNode representing a -cc1 job which produces a Clang module.
class ClangModuleJobNode : public JobNode {
public:
  ClangModuleJobNode(std::unique_ptr<Command> &&Job, deps::ModuleDeps &&MD)
      : JobNode(std::move(Job), NodeKind::ClangModuleCC1Job),
        MD(std::move(MD)) {}
  ~ClangModuleJobNode() override = default;

  deps::ModuleDeps MD;

  static bool classof(const CGNode *N) {
    return N->getKind() == NodeKind::ClangModuleCC1Job;
  }
};

/// Base class for any CGNode type that represents any scanned -cc1 job.
class ScannedJobNode : public JobNode {
public:
  ScannedJobNode(std::unique_ptr<Command> &&Job, InputDependencies &&InputDeps,
                 NodeKind Kind)
      : JobNode(std::move(Job), Kind), InputDeps(std::move(InputDeps)) {}
  ~ScannedJobNode() override = default;

  InputDependencies InputDeps;

  static bool classof(const CGNode *N) {
    return N->getKind() == NodeKind::NamedModuleCC1Job ||
           N->getKind() == NodeKind::NonModuleCC1Job;
  }
};

/// Subclass of CGNode representing a -cc1 job which produces a C++20 named
/// module.
class NamedModuleJobNode : public ScannedJobNode {
public:
  NamedModuleJobNode(std::unique_ptr<Command> &&Job,
                     InputDependencies &&InputDeps)
      : ScannedJobNode(std::move(Job), std::move(InputDeps),
                       NodeKind::NamedModuleCC1Job) {}
  ~NamedModuleJobNode() override = default;

  static bool classof(const CGNode *N) {
    return N->getKind() == NodeKind::NamedModuleCC1Job;
  }
};

/// Subclass of CGNode representing a -cc1 job which does not produce any
/// module, but might still have module imports.
class NonModuleTUJobNode : public ScannedJobNode {
public:
  NonModuleTUJobNode(std::unique_ptr<Command> &&Job,
                     InputDependencies &&InputDeps)
      : ScannedJobNode(std::move(Job), std::move(InputDeps),
                       NodeKind::NonModuleCC1Job) {}
  ~NonModuleTUJobNode() override = default;

  static bool classof(const CGNode *N) {
    return N->getKind() == NodeKind::NonModuleCC1Job;
  }
};

/// Subclass of CGNode representing a job which produces an image file, such as
/// a linker or interface stub merge job.
class ImageJobNode : public JobNode {
public:
  ImageJobNode(std::unique_ptr<Command> &&Job)
      : JobNode(std::move(Job), NodeKind::ImageJob) {}
  ~ImageJobNode() override = default;

  static bool classof(const CGNode *N) {
    return N->getKind() == NodeKind::ImageJob;
  }
};

/// Subclass of CGNode representing any job not covered by the other node types.
///
/// Jobs represented by this node type are not modified by the modules driver.
class MiscJobNode : public JobNode {
public:
  MiscJobNode(std::unique_ptr<Command> &&Job)
      : JobNode(std::move(Job), NodeKind::MiscJob) {}
  ~MiscJobNode() override = default;

  static bool classof(const CGNode *N) {
    return N->getKind() == NodeKind::MiscJob;
  }
};

/// Compilation Graph Edge
///
/// Edges connect the producer of an output to its consumer, except for edges
/// stemming from the root node.
class CGEdge : public CGEdgeBase {
public:
  enum class EdgeKind {
    Regular,
    ModuleDependency,
    Rooted,
  };

  CGEdge(CGNode &N, EdgeKind K) : CGEdgeBase(N), Kind(K) {}
  CGEdge(const CGEdge &) = delete;
  CGEdge &operator=(const CGEdge &) = delete;
  CGEdge(CGEdge &&) = delete;
  CGEdge &operator=(CGEdge &&) = delete;

  EdgeKind getKind() const { return Kind; }

private:
  EdgeKind Kind;
};

/// Compilation Graph
///
/// The graph owns all of its components.
/// All nodes and edges created by the graph have the same livetime as the
/// graph, even if removed from the graph's node list.
class CompilationGraph : public CGBase {
public:
  CompilationGraph() = default;
  CompilationGraph(const CompilationGraph &) = delete;
  CompilationGraph &operator=(const CompilationGraph &) = delete;
  CompilationGraph(CompilationGraph &&G) = default;
  CompilationGraph &operator=(CompilationGraph &&) = default;
  ~CompilationGraph() = default;

  CGNode &getRoot() const {
    assert(Root && "Root node has not yet been created!");
    return *Root;
  }

  RootNode &createRoot() {
    assert(!Root && "Root node has already been created!");
    auto &RootRef = createNodeImpl<RootNode>();
    Root = &RootRef;
    return RootRef;
  }

  template <typename T, typename... Args> T &createJobNode(Args &&...Arg) {
    static_assert(std::is_base_of<JobNode, T>::value,
                  "T must be derived from JobNode");
    return createNodeImpl<T>(std::forward<Args>(Arg)...);
  }

  CGEdge &createEdge(CGEdge::EdgeKind Kind, CGNode &Src, CGNode &Dst) {
    auto Edge = std::make_unique<CGEdge>(Dst, Kind);
    CGEdge &EdgeRef = *Edge;
    AllEdges.push_back(std::move(Edge));
    connect(Src, Dst, EdgeRef);
    return EdgeRef;
  }

private:
  using CGBase::addNode;
  using CGBase::connect;

  template <typename T, typename... Args> T &createNodeImpl(Args &&...Arg) {
    auto Node = std::make_unique<T>(std::forward<Args>(Arg)...);
    T &NodeRef = *Node;
    AllNodes.push_back(std::move(Node));
    addNode(NodeRef);
    return NodeRef;
  }

  CGNode *Root = nullptr;
  SmallVector<std::unique_ptr<CGNode>> AllNodes;
  SmallVector<std::unique_ptr<CGEdge>> AllEdges;
};
} // anonymous namespace

static StringRef getFirstInputFilename(const Command &Job) {
  return Job.getInputInfos().front().getFilename();
}

namespace llvm {
/// Non-const versions of the GraphTraits specializations for CompilationGraph.
template <> struct GraphTraits<CGNode *> {
  using NodeRef = CGNode *;

  static NodeRef CGGetTargetNode(CGEdge *E) { return &E->getTargetNode(); }

  using ChildIteratorType =
      mapped_iterator<CGNode::iterator, decltype(&CGGetTargetNode)>;
  using ChildEdgeIteratorType = CGNode::iterator;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N->begin(), &CGGetTargetNode);
  }

  static ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType(N->end(), &CGGetTargetNode);
  }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef N) { return N->end(); }
};

template <> struct GraphTraits<CompilationGraph *> : GraphTraits<CGNode *> {
  using GraphRef = CompilationGraph *;
  using NodeRef = CGNode *;

  using nodes_iterator = CompilationGraph::iterator;

  static NodeRef getEntryNode(GraphRef G) { return &G->getRoot(); }

  static nodes_iterator nodes_begin(GraphRef G) { return G->begin(); }

  static nodes_iterator nodes_end(GraphRef G) { return G->end(); }
};

/// Const versions of the GraphTraits specializations for CompilationGraph.
template <> struct GraphTraits<const CGNode *> {
  using NodeRef = const CGNode *;

  static NodeRef CGGetTargetNode(const CGEdge *E) {
    return &E->getTargetNode();
  }

  using ChildIteratorType =
      mapped_iterator<CGNode::const_iterator, decltype(&CGGetTargetNode)>;
  using ChildEdgeIteratorType = CGNode::const_iterator;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N->begin(), &CGGetTargetNode);
  }

  static ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType(N->end(), &CGGetTargetNode);
  }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->begin();
  }

  static ChildEdgeIteratorType child_edge_end(NodeRef N) { return N->end(); }
};

template <>
struct GraphTraits<const CompilationGraph *> : GraphTraits<const CGNode *> {
  using GraphRef = const CompilationGraph *;
  using NodeRef = const CGNode *;

  using nodes_iterator = CompilationGraph::const_iterator;

  static NodeRef getEntryNode(GraphRef G) { return &G->getRoot(); }

  static nodes_iterator nodes_begin(GraphRef G) { return G->begin(); }

  static nodes_iterator nodes_end(GraphRef G) { return G->end(); }
};

template <>
struct DOTGraphTraits<const CompilationGraph *> : DefaultDOTGraphTraits {
  explicit DOTGraphTraits(bool IsSimple = false)
      : DefaultDOTGraphTraits(IsSimple) {}
  using GraphRef = const CompilationGraph *;
  using NodeRef = const CGNode *;

  static std::string getGraphName(GraphRef) {
    return "Module Dependency Graph";
  }

  static std::string getGraphProperties(GraphRef) {
    return "\tnode [shape=Mrecord, colorscheme=set23, style=filled];\n";
  }

  static bool renderGraphFromBottomUp() { return true; }

  static bool isNodeHidden(NodeRef N, GraphRef) {
    // Only show nodes with module dependency relations.
    return !isa<ClangModuleJobNode, ScannedJobNode>(N);
  }

  static std::string getNodeIdentifier(NodeRef N, GraphRef) {
    return llvm::TypeSwitch<NodeRef, std::string>(N)
        .Case([](const ClangModuleJobNode *ClangModuleNode) {
          const auto &ID = ClangModuleNode->MD.ID;
          return llvm::formatv("{0}-{1}", ID.ModuleName, ID.ContextHash).str();
        })
        .Case([](const NamedModuleJobNode *NamedModuleNode) {
          return llvm::formatv("{0}-{1}", NamedModuleNode->InputDeps.ModuleName,
                               getTriple(*NamedModuleNode->Job))
              .str();
        })
        .Case([](const NonModuleTUJobNode *NonModuleTUNode) {
          const auto &Job = *NonModuleTUNode->Job;
          return llvm::formatv("{0}-{1}", getFirstInputFilename(Job),
                               getTriple(Job))
              .str();
        })
        .DefaultUnreachable("Unexpected node kind! Is this node hidden?");
  }

  static std::string getNodeLabel(NodeRef N, GraphRef) {
    return llvm::TypeSwitch<NodeRef, std::string>(N)
        .Case([](const ClangModuleJobNode *ClangModuleNode) {
          const auto &MD = ClangModuleNode->MD;
          if (MD.IsCXXNamedModule)
            return llvm::formatv(
                       "Filename: {0} \\| Module type: Named module \\| "
                       "Module name: {1} \\| Hash: {2}",
                       MD.CXXNamedModuleSource, MD.ID.ModuleName,
                       MD.ID.ContextHash)
                .str();
          return llvm::formatv("Module type: Clang module \\| Module name: {0} "
                               "\\| Hash: {1}",
                               MD.ID.ModuleName, MD.ID.ContextHash)
              .str();
        })
        .Case([](const NamedModuleJobNode *NamedModuleNode) {
          const auto &Job = *NamedModuleNode->Job;
          return llvm::formatv(
                     "Filename: {0} \\| Module type: Named module \\| "
                     "Module name: {1} \\| Triple: {2}",
                     getFirstInputFilename(Job),
                     NamedModuleNode->InputDeps.ModuleName, getTriple(Job))
              .str();
        })
        .Case([](const NonModuleTUJobNode *NonModuleTUNode) {
          const auto &Job = *NonModuleTUNode->Job;
          return llvm::formatv("Filename: {0} \\| Triple: {1}",
                               getFirstInputFilename(Job), getTriple(Job))
              .str();
        })
        .DefaultUnreachable("Unexpected node kind! Is this node hidden?");
  }

  static std::string getNodeAttributes(NodeRef N, GraphRef) {
    switch (N->getKind()) {
    case CGNode::NodeKind::ClangModuleCC1Job:
      return cast<ClangModuleJobNode>(N)->MD.IsCXXNamedModule ? "fillcolor=2"
                                                              : "fillcolor=1";
    case CGNode::NodeKind::NamedModuleCC1Job:
      return "fillcolor=2";
    case CGNode::NodeKind::NonModuleCC1Job:
      return "fillcolor=3";
    default:
      llvm_unreachable("Unexpected node kind! Is this node hidden?");
    }
  }
};

/// GraphWriter specialization for CompilationGraph that emits a more
/// human-readable DOT graph.
template <>
class GraphWriter<const CompilationGraph *>
    : public GraphWriterBase<const CompilationGraph *,
                             GraphWriter<const CompilationGraph *>> {
public:
  using GraphType = const CompilationGraph *;
  using Base = GraphWriterBase<GraphType, GraphWriter<GraphType>>;

  GraphWriter(llvm::raw_ostream &O, const GraphType &G, bool IsSimple)
      : Base(O, G, IsSimple), EscapedIDByNodeRef(G->size()) {}

  void writeNodes() {
    auto IsNodeVisible = [&](NodeRef N) { return !DTraits.isNodeHidden(N, G); };
    auto VisibleNodes = llvm::filter_to_vector(nodes(G), IsNodeVisible);

    writeNodeDefinitions(VisibleNodes);
    O << "\n";
    writeNodeRelations(VisibleNodes);
  }

private:
  using Base::DOTTraits;
  using Base::GTraits;
  using Base::NodeRef;

  void writeNodeDefinitions(ArrayRef<NodeRef> VisibleNodes) {
    for (NodeRef Node : VisibleNodes) {
      std::string EscapedNodeID =
          DOT::EscapeString(DTraits.getNodeIdentifier(Node, G));
      const std::string NodeLabel = DTraits.getNodeLabel(Node, G);
      const std::string NodeAttrs = DTraits.getNodeAttributes(Node, G);
      O << '\t' << '"' << EscapedNodeID << "\" [" << NodeAttrs << ", label=\"{ "
        << DOT::EscapeString(NodeLabel) << " }\"];\n";
      EscapedIDByNodeRef.try_emplace(Node, std::move(EscapedNodeID));
    }
  }

  void writeNodeRelations(ArrayRef<NodeRef> VisibleNodes) {
    auto IsNodeVisible = [&](NodeRef N) { return !DTraits.isNodeHidden(N, G); };
    for (NodeRef Node : VisibleNodes) {
      auto DstNodes = llvm::make_range(GTraits::child_begin(Node),
                                       GTraits::child_end(Node));
      auto VisibleDstNodes = llvm::make_filter_range(DstNodes, IsNodeVisible);
      StringRef EscapedSrcNodeID = EscapedIDByNodeRef.at(Node);
      for (NodeRef DstNode : VisibleDstNodes) {
        StringRef EscapedTgtNodeID = EscapedIDByNodeRef.at(DstNode);
        O << '\t' << '"' << EscapedSrcNodeID << "\" -> \"" << EscapedTgtNodeID
          << "\";\n";
      }
    }
  }

  DenseMap<NodeRef, std::string> EscapedIDByNodeRef;
};
} // namespace llvm

/// Validates that each module-defining source is of type \c TY_CXXModule.
///
/// \returns false on error, with diagnostics emitted via \p Diags.
static bool validateScannedJobInputKinds(
    ArrayRef<std::unique_ptr<Command>> ScannedJobs,
    ArrayRef<InputDependencies> InputDepsForScannedJobs,
    DiagnosticsEngine &Diags) {
  for (const auto &&[Job, InputDeps] : llvm::zip_equal(
           llvm::make_pointee_range(ScannedJobs), InputDepsForScannedJobs)) {
    const auto &MainInput = Job.getInputInfos().front();
    const bool DefinesNamedModule = !InputDeps.ModuleName.empty();

    if (DefinesNamedModule && MainInput.getType() != types::TY_CXXModule &&
        MainInput.getType() != types::TY_PP_CXXModule) {
      Diags.Report(diag::err_module_defined_outside_of_module_source)
          << InputDeps.ModuleName << MainInput.getFilename();
      return false;
    }
  }
  return true;
}

static SmallVector<std::unique_ptr<Command>>
takeJobsAtIndices(SmallVectorImpl<std::unique_ptr<Command>> &Jobs,
                  ArrayRef<size_t> Indices) {
  SmallVector<std::unique_ptr<Command>> Out;
  for (const auto JobIndex : Indices) {
    assert(Jobs[JobIndex] && "Expected valid job!");
    Out.push_back(std::move(Jobs[JobIndex]));
  }
  return Out;
}

/// Creates nodes for all jobs that could not be scanned (e.g. image jobs, ...).
static void createNodesForNonScannableJobs(
    CompilationGraph &Graph,
    SmallVectorImpl<std::unique_ptr<Command>> &&NonScannableJobs) {
  for (auto &Job : NonScannableJobs) {
    if (Job->getCreator().isLinkJob())
      Graph.createJobNode<ImageJobNode>(std::move(Job));
    else
      Graph.createJobNode<MiscJobNode>(std::move(Job));
  }
}

// Returns the derived argument list for the tool chain responsible
// for creating \p Job.
static const DerivedArgList &getToolChainArgs(Compilation &C,
                                              const Command &Job) {
  const auto &TC = Job.getCreator().getToolChain();
  const auto &SourceAction = Job.getSource();
  return C.getArgsForToolChain(&TC, SourceAction.getOffloadingArch(),
                               SourceAction.getOffloadingDeviceKind());
}

/// Creates a job for the Clang module described by \p MD.
static std::unique_ptr<Command>
createClangModulePrecompileJob(Compilation &C, const Command &ImportingJob,
                               const deps::ModuleDeps &MD, StringRef PCMPath) {
  DerivedArgList &Args = C.getArgs();
  const OptTable &Opts = C.getDriver().getOpts();
  Arg *InputArg =
      makeInputArg(Args, Opts,
                   MD.IsCXXNamedModule ? "<discovered named module>"
                                       : "<discovered clang module>");
  Action *IA = C.MakeAction<InputAction>(*InputArg, types::ID::TY_ModuleFile);
  Action *PA = C.MakeAction<PrecompileJobAction>(IA, types::ID::TY_ModuleFile);
  PA->propagateOffloadInfo(&ImportingJob.getSource());

  const auto &TCArgs = getToolChainArgs(C, ImportingJob);

  const auto &BuildArgs = MD.getBuildArguments();
  ArgStringList JobArgs;
  JobArgs.reserve(BuildArgs.size());
  for (const auto &Arg : BuildArgs)
    JobArgs.push_back(TCArgs.MakeArgString(Arg));

  const auto &D = C.getDriver();
  const char *PCMPathArg = TCArgs.MakeArgString(PCMPath);
  const InputInfo Output(types::TY_ModuleFile, PCMPathArg, PCMPathArg);
  if (D.CC1Main && !D.CCGenDiagnostics)
    return std::make_unique<CC1Command>(
        *PA, ImportingJob.getCreator(), ResponseFileSupport::AtFileUTF8(),
        D.getDriverProgramPath(), JobArgs,
        /*Inputs=*/ArrayRef<InputInfo>{}, Output, D.getPrependArg());
  return std::make_unique<Command>(
      *PA, ImportingJob.getCreator(), ResponseFileSupport::AtFileUTF8(),
      D.getDriverProgramPath(), JobArgs,
      /*Inputs=*/ArrayRef<InputInfo>{}, Output, D.getPrependArg());
}

/// Creates a \c ClangModuleJobNode with associated job for each unique module
/// in \p ModuleDepGraphsForScannedJobs.
///
/// Named modules defined by command-line inputs are not built by a separate
/// job. Their IDs are instead recorded in \p OwnedNamedModules, mapped to the
/// module name and triple.
static void createModuleJobsAndNodes(
    CompilationGraph &Graph, Compilation &C,
    ArrayRef<std::unique_ptr<Command>> ImportingJobs,
    SmallVectorImpl<deps::ModuleDepsGraph> &&ModuleDepGraphsForScannedJobs,
    llvm::function_ref<ModulesDriverActionController &(const Command &)>
        GetController,
    llvm::DenseMap<deps::ModuleID, ModuleNameAndTriple> &OwnedNamedModules) {
  llvm::DenseSet<deps::ModuleID> AlreadySeen;
  for (auto &&[ImportingJob, ModuleDepsGraph] :
       llvm::zip_equal(llvm::make_pointee_range(ImportingJobs),
                       ModuleDepGraphsForScannedJobs)) {
    auto &Controller = GetController(ImportingJob);
    for (auto &MD : ModuleDepsGraph) {
      const auto Inserted = AlreadySeen.insert(MD.ID).second;
      if (!Inserted)
        continue;

      if (Controller.getOwnedProvider(MD)) {
        const ModuleNameAndTriple NameAndTriple{
            C.getArgs().MakeArgString(MD.ID.ModuleName),
            getTriple(ImportingJob)};
        OwnedNamedModules.try_emplace(MD.ID, NameAndTriple);
        continue;
      }

      const std::string PCMPath =
          Controller.lookupModuleOutput(MD, deps::ModuleOutputKind::ModuleFile);
      auto ModuleJob =
          createClangModulePrecompileJob(C, ImportingJob, MD, PCMPath);
      Graph.createJobNode<ClangModuleJobNode>(std::move(ModuleJob),
                                              std::move(MD));
    }
  }
}

/// Installs the command lines produced by the dependency scan into
/// \p ScannedJobs.
static void
installScanCommandLines(Compilation &C,
                        MutableArrayRef<std::unique_ptr<Command>> ScannedJobs,
                        ArrayRef<InputDependencies> InputDepsForScannedJobs) {
  for (auto &&[Job, InputDeps] : llvm::zip_equal(
           llvm::make_pointee_range(ScannedJobs), InputDepsForScannedJobs)) {
    const auto &BuildArgs = InputDeps.BuildArgs;
    ArgStringList JobArgs;
    JobArgs.reserve(BuildArgs.size());

    auto &TCArgs = getToolChainArgs(C, Job);
    for (const auto &Arg : BuildArgs)
      JobArgs.push_back(TCArgs.MakeArgString(Arg));

    Job.replaceArguments(std::move(JobArgs));
  }
}

/// Creates nodes for all jobs which were scanned for dependencies.
///
/// The updated command lines produced by the dependency scan are installed at a
/// later point.
static void createNodesForScannedJobs(
    CompilationGraph &Graph,
    SmallVectorImpl<std::unique_ptr<Command>> &&ScannedJobs,
    SmallVectorImpl<InputDependencies> &&InputDepsForScannedJobs) {
  for (auto &&[Job, InputDeps] :
       llvm::zip_equal(ScannedJobs, InputDepsForScannedJobs)) {
    if (InputDeps.ModuleName.empty())
      Graph.createJobNode<NonModuleTUJobNode>(std::move(Job),
                                              std::move(InputDeps));
    else
      Graph.createJobNode<NamedModuleJobNode>(std::move(Job),
                                              std::move(InputDeps));
  }
}

template <typename LookupT, typename KeyRangeT>
static void connectEdgesViaLookup(CompilationGraph &Graph, CGNode &TgtNode,
                                  const LookupT &SrcNodeLookup,
                                  const KeyRangeT &SrcNodeLookupKeys,
                                  CGEdge::EdgeKind Kind) {
  for (const auto &Key : SrcNodeLookupKeys) {
    const auto It = SrcNodeLookup.find(Key);
    if (It == SrcNodeLookup.end())
      continue;

    auto &SrcNode = *It->second;
    Graph.createEdge(Kind, SrcNode, TgtNode);
  }
}

/// Create edges for regular (non-module) dependencies in \p Graph.
static void createRegularEdges(CompilationGraph &Graph) {
  llvm::DenseMap<StringRef, CGNode *> NodeByOutputFiles;
  for (auto *Node : Graph) {
    for (const auto &Output : cast<JobNode>(Node)->Job->getOutputFilenames()) {
      [[maybe_unused]] const bool Inserted =
          NodeByOutputFiles.try_emplace(Output, Node).second;
      assert(Inserted &&
             "Driver should not produce multiple jobs with identical outputs!");
    }
  }

  for (auto *Node : Graph) {
    const auto &InputInfos = cast<JobNode>(Node)->Job->getInputInfos();
    auto InputFilenames = llvm::map_range(
        InputInfos, [](const auto &II) { return II.getFilename(); });

    connectEdgesViaLookup(Graph, *Node, NodeByOutputFiles, InputFilenames,
                          CGEdge::EdgeKind::Regular);
  }
}

/// Create edges for module dependencies in \p Graph.
///
/// \returns false if there are multiple definitions for a named module, with
/// diagnostics reported to \p Diags; otherwise returns true.
static bool createModuleDependencyEdges(
    CompilationGraph &Graph,
    const llvm::DenseMap<deps::ModuleID, ModuleNameAndTriple>
        &OwnedNamedModules,
    DiagnosticsEngine &Diags) {
  llvm::DenseMap<deps::ModuleID, CGNode *> ModuleNodeByID;
  llvm::DenseMap<ModuleNameAndTriple, CGNode *> NamedModuleNodeByID;

  // Map each module to the job that produces it.
  bool HasDuplicateModuleError = false;
  for (auto *Node : Graph) {
    llvm::TypeSwitch<CGNode *>(Node)
        .Case([&](ClangModuleJobNode *ClangModuleNode) {
          [[maybe_unused]] const bool Inserted =
              ModuleNodeByID.try_emplace(ClangModuleNode->MD.ID, Node).second;
          assert(Inserted && "Multiple module nodes with the same module ID!");
        })
        .Case([&](NamedModuleJobNode *NamedModuleNode) {
          StringRef ModuleName = NamedModuleNode->InputDeps.ModuleName;
          ModuleNameAndTriple ID{ModuleName, getTriple(*NamedModuleNode->Job)};
          const auto [It, Inserted] = NamedModuleNodeByID.try_emplace(ID, Node);
          if (!Inserted) {
            // For scan input jobs, their first input is always a filename and
            // the scanned source.
            // We don't use InputDeps.FileDeps here because diagnostics should
            // refer to the filename as specified on the command line, not the
            // canonical absolute path.
            StringRef PrevFile =
                getFirstInputFilename(*cast<JobNode>(It->second)->Job);
            StringRef CurFile = getFirstInputFilename(*NamedModuleNode->Job);
            Diags.Report(diag::err_modules_driver_named_module_redefinition)
                << ModuleName << PrevFile << CurFile;
            HasDuplicateModuleError = true;
          }
        });
  }
  if (HasDuplicateModuleError)
    return false;

  // Named modules defined by command-line inputs are produced by those inputs'
  // jobs.
  for (const auto &[ID, NameAndTriple] : OwnedNamedModules)
    if (auto *Node = NamedModuleNodeByID.lookup(NameAndTriple))
      ModuleNodeByID.try_emplace(ID, Node);

  // Create edges from the module nodes to their importers.
  for (auto *Node : Graph) {
    llvm::TypeSwitch<CGNode *>(Node)
        .Case([&](ClangModuleJobNode *ClangModuleNode) {
          connectEdgesViaLookup(Graph, *ClangModuleNode, ModuleNodeByID,
                                ClangModuleNode->MD.ClangModuleDeps,
                                CGEdge::EdgeKind::ModuleDependency);
        })
        .Case([&](ScannedJobNode *NodeWithInputDeps) {
          connectEdgesViaLookup(Graph, *NodeWithInputDeps, ModuleNodeByID,
                                NodeWithInputDeps->InputDeps.ClangModuleDeps,
                                CGEdge::EdgeKind::ModuleDependency);
        });
  }

  return true;
}

/// Creates the root node and connects it to all nodes with no incoming edges
/// ensuring that every node in the graph is reachable from the root.
static void createAndConnectRoot(CompilationGraph &Graph) {
  llvm::SmallPtrSet<CGNode *, 16> HasIncomingEdge;
  for (auto *Node : Graph)
    for (auto *Edge : Node->getEdges())
      HasIncomingEdge.insert(&Edge->getTargetNode());

  auto AllNonRootNodes = llvm::iterator_range(Graph);
  auto &Root = Graph.createRoot();

  for (auto *Node : AllNonRootNodes) {
    if (HasIncomingEdge.contains(Node))
      continue;
    Graph.createEdge(CGEdge::EdgeKind::Rooted, Root, *Node);
  }
}

/// Creates a temporary output path for \p ModuleName.
static std::string createModuleOutputPath(const Compilation &C,
                                          StringRef ModuleName) {
  // Sanitize the ':' included in parition names. It is illegal for filenames on
  // Windows.
  SmallString<32> SanitizedModuleName(ModuleName);
  llvm::replace(SanitizedModuleName, ':', '-');
  auto ModuleOutputPath = C.getDriver().GetTemporaryPath(
      SanitizedModuleName, types::getTypeTempSuffix(types::TY_ModuleFile));
  return ModuleOutputPath;
}

/// Configures the jobs of command-line inputs which define named modules.
///
/// Such inputs are compiled in two phases: a precompile job builds the BMI,
/// using the module build command produced by the dependency scan, and a
/// second job compiles the BMI into an object file. That second job needs the
/// same module file mappings as the precompile job.
static void configureOwnedNamedModuleJobs(CompilationGraph &Graph) {
  for (auto *Node : Graph) {
    auto *NamedModuleNode = dyn_cast<NamedModuleJobNode>(Node);
    if (!NamedModuleNode ||
        !isa<PrecompileJobAction>(NamedModuleNode->Job->getSource()))
      continue;

    SmallVector<const char *> ModuleFileArgs;
    for (const char *Arg : NamedModuleNode->Job->getArguments())
      if (StringRef(Arg).starts_with("-fmodule-file="))
        ModuleFileArgs.push_back(Arg);

    for (auto *Edge : Node->getEdges()) {
      auto *Consumer = dyn_cast<JobNode>(&Edge->getTargetNode());
      if (!Consumer || Edge->getKind() != CGEdge::EdgeKind::Regular ||
          !isCC1Job(*Consumer->Job))
        continue;
      auto Args = Consumer->Job->getArguments();
      llvm::append_range(Args, ModuleFileArgs);
      Consumer->Job->replaceArguments(std::move(Args));
    }
  }
}

/// Makes each job's action depend on the actions of the jobs producing the
/// modules it imports.
///
/// This way, jobs are not executed if any job they depend on failed, and the
/// job dependencies can be determined from the actions alone.
static void addModuleDependenciesToActions(CompilationGraph &Graph) {
  for (auto *Node : Graph) {
    auto *SrcNode = dyn_cast<JobNode>(Node);
    if (!SrcNode)
      continue;
    for (auto *Edge : Node->getEdges()) {
      if (Edge->getKind() != CGEdge::EdgeKind::ModuleDependency)
        continue;
      auto &DstNode = cast<JobNode>(Edge->getTargetNode());
      auto &DstAction = const_cast<Action &>(DstNode.Job->getSource());
      auto *SrcAction = const_cast<Action *>(&SrcNode->Job->getSource());
      if (!llvm::is_contained(DstAction.getInputs(), SrcAction))
        DstAction.getInputs().push_back(SrcAction);
    }
  }
}

/// Reports an error for each named module imported by a scanned job that is
/// neither defined by any input nor provided by any module manifest.
///
/// \returns false if any errors were reported.
static bool diagnoseUnresolvedNamedModuleImports(
    ArrayRef<std::unique_ptr<Command>> ScannedJobs,
    ArrayRef<InputDependencies> InputDepsForScannedJobs,
    llvm::function_ref<const NamedModuleProviderMap &(const Command &)>
        GetProviders,
    const llvm::StringMap<std::string> &MissingManifestSources,
    DiagnosticsEngine &Diags) {
  bool Success = true;
  for (const auto &&[Job, InputDeps] : llvm::zip_equal(
           llvm::make_pointee_range(ScannedJobs), InputDepsForScannedJobs)) {
    const auto &Providers = GetProviders(Job);
    for (const auto &ModuleName : InputDeps.NamedModuleDeps) {
      if (Providers.count(ModuleName))
        continue;
      if (const auto It = MissingManifestSources.find(ModuleName);
          It != MissingManifestSources.end())
        Diags.Report(diag::err_modules_driver_manifest_source_not_found)
            << ModuleName << getFirstInputFilename(Job) << It->second;
      else
        Diags.Report(diag::err_modules_driver_named_module_not_found)
            << ModuleName << getFirstInputFilename(Job);
      Success = false;
    }
  }
  return Success;
}

/// Returns the path of the BMI used by importers of the named module
/// \p ModuleName, which is defined by the command-line input of the precompile
/// job \p Job.
static std::string getOwnedBMIPath(Compilation &C, const Command &Job,
                                   StringRef ModuleName, bool ReducedBMI) {
  // With full BMIs, importers use the main output of the precompile job.
  if (!ReducedBMI)
    return Job.getOutputFilenames().front();

  // Respect an explicitly requested module output.
  if (C.getArgs().hasArg(options::OPT_fmodule_output,
                         options::OPT_fmodule_output_EQ))
    for (StringRef Arg : llvm::reverse(Job.getArguments()))
      if (Arg.consume_front("-fmodule-output="))
        return Arg.str();

  auto ModuleOutputPath = createModuleOutputPath(C, ModuleName);
  C.addTempFile(C.getArgs().MakeArgString(ModuleOutputPath));
  return ModuleOutputPath;
}

/// Returns true if \p Job compiles a C++ module unit, which may define a named
/// module.
static bool isModuleUnitJob(const Command &Job) {
  const auto Type = Job.getInputInfos().front().getType();
  return Type == types::TY_CXXModule || Type == types::TY_PP_CXXModule;
}

/// Moves jobs from \p Graph into \p C in the graph's topological order.
static void feedJobsBackIntoCompilation(Compilation &C,
                                        CompilationGraph &&Graph) {
  llvm::ReversePostOrderTraversal<CompilationGraph *> TopologicallySortedNodes(
      &Graph);
  assert(isa<RootNode>(*TopologicallySortedNodes.begin()) &&
         "First node in topological order must be the root!");
  auto TopologicallySortedJobNodes = llvm::map_range(
      llvm::drop_begin(TopologicallySortedNodes), llvm::CastTo<JobNode>);
  for (auto *JN : TopologicallySortedJobNodes)
    C.addCommand(std::move(JN->Job));
}

void driver::modules::runModulesDriver(
    Compilation &C, ArrayRef<StdModuleManifest::Module> ManifestEntries) {
  llvm::PrettyStackTraceString CrashInfo("Running modules driver.");

  auto Jobs = C.getJobs().takeJobs();
  DiagnosticsEngine &Diags = C.getDriver().getDiags();

  const auto MaybeModuleCachePath = getModuleCachePath(C.getArgs());
  if (!MaybeModuleCachePath) {
    Diags.Report(diag::err_default_modules_cache_not_available);
    return;
  }
  const std::string &ModuleCachePath = *MaybeModuleCachePath;

  auto MaybeCWD = C.getDriver().getVFS().getCurrentWorkingDirectory();
  const auto CWD = MaybeCWD ? std::move(*MaybeCWD) : ".";

  const llvm::opt::Arg *LogPathArg =
      C.getArgs().getLastArg(options::OPT_fdepscan_log_path);
  StringRef DepScanLogPath =
      LogPathArg ? StringRef(LogPathArg->getValue()).trim() : StringRef();
  if (LogPathArg && DepScanLogPath.empty()) {
    Diags.Report(diag::err_drv_depscan_log_path_empty);
    return;
  }

  // The number of parallel jobs is used when executing the compilation.
  if (const Arg *A =
          C.getArgs().getLastArg(options::OPT_fmodules_driver_jobs_EQ)) {
    unsigned NumJobs;
    if (StringRef(A->getValue()).getAsInteger(10, NumJobs) || NumJobs == 0) {
      Diags.Report(diag::err_drv_modules_driver_invalid_jobs) << A->getValue();
      return;
    }
  }

  deps::DependencyScanningServiceOptions Opts;
  Opts.LogPath = DepScanLogPath.str();
  deps::DependencyScanningService ScanningService(std::move(Opts));

  // Reports the diagnostics of all scans. A single reporter is used, since it
  // may install its own SourceManager into Diags.
  StandaloneDiagReporter DiagReporter(Diags);

  // Classify the jobs based on scan eligibility.
  SmallVector<size_t> ScannableJobIndices;
  SmallVector<size_t> NonScannableJobIndices;
  for (const auto &&[Index, Job] : llvm::enumerate(Jobs)) {
    if (isDependencyScannableJob(*Job))
      ScannableJobIndices.push_back(Index);
    else
      NonScannableJobIndices.push_back(Index);
  }

  // Collect the named modules provided by the module manifests. These are
  // built on demand, with their object code in a prebuilt library.
  const bool ReducedBMI =
      C.getArgs().hasFlag(options::OPT_fmodules_reduced_bmi,
                          options::OPT_fno_modules_reduced_bmi, true);
  // Entries from later manifests take precedence, so that manifests passed
  // explicitly with -fmodules-manifest= override the Standard library
  // manifest.
  NamedModuleProviderMap ManifestProviders;
  llvm::StringMap<std::string> MissingManifestSources;
  for (const auto &Entry : ManifestEntries) {
    if (!C.getDriver().getVFS().exists(Entry.SourcePath)) {
      MissingManifestSources[Entry.LogicalName] = Entry.SourcePath;
      ManifestProviders.erase(Entry.LogicalName);
      continue;
    }
    MissingManifestSources.erase(Entry.LogicalName);
    NamedModuleProvider Provider;
    Provider.Source.SourcePath = Entry.SourcePath;
    if (Entry.LocalArgs)
      Provider.Source.SystemIncludeDirs = Entry.LocalArgs->SystemIncludeDirs;
    Provider.Source.IsStdLibrary = Entry.IsStdlib;
    Provider.Source.ReducedBMI = ReducedBMI;
    ManifestProviders.insert_or_assign(Entry.LogicalName, std::move(Provider));
  }

  // The named modules available to inputs depend on their target, since the
  // command-line inputs defining named modules are compiled per target.
  llvm::StringMap<NamedModuleProviderMap> ProvidersByTriple;
  auto GetProviders = [&](const Command &Job) -> NamedModuleProviderMap & {
    auto [It, Inserted] = ProvidersByTriple.try_emplace(getTriple(Job));
    if (Inserted)
      It->second = ManifestProviders;
    return It->second;
  };

  // Determine which named modules the command-line inputs define, by reading
  // the module declarations of the module units. This needs neither the
  // preprocessor nor the -cc1 command lines.
  llvm::DenseMap<const Command *, std::string> OwnedModuleNameByJob;
  {
    SmallVector<std::pair<const Command *, std::string>> ModuleUnitJobs;
    for (const size_t Index : ScannableJobIndices) {
      const Command &Job = *Jobs[Index];
      if (!isModuleUnitJob(Job))
        continue;
      // Standard input can only be read once, by the job itself.
      StringRef Input = getFirstInputFilename(Job);
      if (Input == "-")
        continue;
      auto Buffer = C.getDriver().getVFS().getBufferForFile(Input);
      if (!Buffer)
        continue;
      auto Decl = scanInputForCXX20ModuleDeclaration((*Buffer)->getBuffer());
      if (Decl && Decl->providesModule())
        ModuleUnitJobs.emplace_back(&Job, std::move(Decl->Name));
    }

    llvm::DenseMap<ModuleNameAndTriple, const Command *> DefiningJobs;
    bool HasDuplicateModuleError = false;
    for (const auto &[Job, ModuleName] : ModuleUnitJobs) {
      const auto [It, Inserted] = DefiningJobs.try_emplace(
          {C.getArgs().MakeArgString(ModuleName), getTriple(*Job)}, Job);
      if (!Inserted) {
        Diags.Report(diag::err_modules_driver_named_module_redefinition)
            << ModuleName << getFirstInputFilename(*It->second)
            << getFirstInputFilename(*Job);
        HasDuplicateModuleError = true;
        continue;
      }

      // Modules defined by command-line inputs take precedence over modules
      // from the manifests.
      NamedModuleProvider Provider;
      Provider.Source.SourcePath = getFirstInputFilename(*Job).str();
      Provider.Source.ReducedBMI = ReducedBMI;
      // If the input's job does not produce a BMI (e.g. with -fsyntax-only),
      // the module is built by a dedicated BMI-only job instead.
      if (isa<PrecompileJobAction>(Job->getSource()) &&
          !Job->getOutputFilenames().empty()) {
        Provider.OwnedBMIPath =
            getOwnedBMIPath(C, *Job, ModuleName, ReducedBMI);
        Provider.CodeGenBMIPath = Job->getOutputFilenames().front();
        OwnedModuleNameByJob[Job] = ModuleName;
      }
      GetProviders(*Job).insert_or_assign(ModuleName, std::move(Provider));
    }
    if (HasDuplicateModuleError)
      return;
  }

  // Scan all inputs. Imported named modules are built implicitly during the
  // scan and reported like Clang modules.
  llvm::StringMap<std::unique_ptr<ModulesDriverActionController>>
      ControllersByTriple;
  auto GetController =
      [&](const Command &Job) -> ModulesDriverActionController & {
    auto &Controller = ControllersByTriple[getTriple(Job)];
    if (!Controller)
      Controller = std::make_unique<ModulesDriverActionController>(
          ModuleCachePath, &GetProviders(Job));
    return *Controller;
  };
  // Create all controllers before scanning concurrently.
  for (const size_t Index : ScannableJobIndices)
    GetController(*Jobs[Index]);

  // Modules defined by inputs are scanned by name, which builds them through
  // the same implicit build as imports from other inputs. All other inputs are
  // scanned as translation units.
  SmallVector<ScanRequest> Requests;
  for (const size_t Index : ScannableJobIndices) {
    ScanRequest &Request = Requests.emplace_back();
    Request.Job = Jobs[Index].get();
    if (auto It = OwnedModuleNameByJob.find(Request.Job);
        It != OwnedModuleNameByJob.end())
      Request.ModuleName = It->second;
  }
  auto MaybeScanResults = scanJobs(
      Requests, ScanningService,
      [&](const Command &Job) -> deps::DependencyActionController & {
        return GetController(Job);
      },
      CWD, DiagReporter);
  if (!MaybeScanResults) {
    Diags.Report(diag::err_dependency_scan_failed);
    return;
  }

  SmallVector<deps::ModuleDepsGraph, 0> ModuleDepGraphsForScannedJobs;
  SmallVector<InputDependencies, 0> InputDepsForScannedJobs;
  for (auto &&[Request, TUDeps] :
       llvm::zip_equal(Requests, *MaybeScanResults)) {
    if (Request.ModuleName) {
      auto InputDeps = makeInputDepsForOwnedModule(TUDeps, *Request.ModuleName);
      if (!InputDeps) {
        Diags.Report(diag::err_dependency_scan_failed);
        return;
      }
      InputDepsForScannedJobs.push_back(std::move(*InputDeps));
    } else {
      InputDepsForScannedJobs.push_back(makeInputDeps(std::move(TUDeps)));
    }
    ModuleDepGraphsForScannedJobs.push_back(std::move(TUDeps.ModuleGraph));
  }

  // Build the compilation graph.
  CompilationGraph Graph;
  createNodesForNonScannableJobs(
      Graph, takeJobsAtIndices(Jobs, NonScannableJobIndices));

  auto ScannedJobs = takeJobsAtIndices(Jobs, ScannableJobIndices);
  if (!validateScannedJobInputKinds(ScannedJobs, InputDepsForScannedJobs,
                                    Diags))
    return;
  if (!diagnoseUnresolvedNamedModuleImports(
          ScannedJobs, InputDepsForScannedJobs,
          [&](const Command &Job) -> const NamedModuleProviderMap & {
            return GetProviders(Job);
          },
          MissingManifestSources, Diags))
    return;
  installScanCommandLines(C, ScannedJobs, InputDepsForScannedJobs);

  llvm::DenseMap<deps::ModuleID, ModuleNameAndTriple> OwnedNamedModules;
  createModuleJobsAndNodes(Graph, C, /*ImportingJobs*/ ScannedJobs,
                           std::move(ModuleDepGraphsForScannedJobs),
                           GetController, OwnedNamedModules);
  createNodesForScannedJobs(Graph, std::move(ScannedJobs),
                            std::move(InputDepsForScannedJobs));

  createRegularEdges(Graph);
  if (!createModuleDependencyEdges(Graph, OwnedNamedModules, Diags))
    return;
  createAndConnectRoot(Graph);

  Diags.Report(diag::remark_printing_module_graph);
  if (!Diags.isLastDiagnosticIgnored())
    llvm::WriteGraph<const CompilationGraph *>(llvm::errs(), &Graph);

  configureOwnedNamedModuleJobs(Graph);
  addModuleDependenciesToActions(Graph);
  feedJobsBackIntoCompilation(C, std::move(Graph));
}
