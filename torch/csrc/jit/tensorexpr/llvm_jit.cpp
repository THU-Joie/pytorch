#ifdef TORCH_ENABLE_LLVM

#include <torch/csrc/jit/tensorexpr/intrinsic_symbols.h>
#include <torch/csrc/jit/tensorexpr/llvm_jit.h>

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/SymbolStringPool.h>
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Mangler.h>
#include <llvm/Support/CFGUpdate.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <torch/csrc/jit/tensorexpr/external_functions_registry.h>

#include <c10/util/Half.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace torch::jit::tensorexpr;

template <typename T>
static llvm::JITTargetAddress toAddress(T* Ptr) {
  return static_cast<llvm::JITTargetAddress>(reinterpret_cast<uintptr_t>(Ptr));
}

// Get subtarget features for the host.
static llvm::SubtargetFeatures getHostSubtargetFeatures() {
  llvm::SubtargetFeatures subtargetFeatures;
  llvm::StringMap<bool> featureMap;
  llvm::sys::getHostCPUFeatures(featureMap);
  for (auto& feature : featureMap) {
    subtargetFeatures.AddFeature(feature.first(), feature.second);
  }
  return subtargetFeatures;
}

// Create a JTMB using the host's triple.  CPU and attrs default to the host
// unless they are supplied.
static llvm::orc::JITTargetMachineBuilder makeJTMBFromHost(
    c10::optional<std::string> cpu,
    c10::optional<std::string> attrs) {
  llvm::orc::JITTargetMachineBuilder JTMB(
      (llvm::Triple(llvm::sys::getProcessTriple())));
  JTMB.setCPU(cpu.value_or(llvm::sys::getHostCPUName().str()));
  if (attrs) {
    std::vector<std::string> features;
    llvm::SubtargetFeatures::Split(features, *attrs);
    JTMB.addFeatures(features);
  } else {
    JTMB.addFeatures(getHostSubtargetFeatures().getFeatures());
  }
  return JTMB;
}

// Create a JTMB using a given triple.  Do not set cpu or attrs if not supplied.
static llvm::orc::JITTargetMachineBuilder makeJTMBFromTriple(
    const std::string& triple,
    c10::optional<std::string> cpu,
    c10::optional<std::string> attrs) {
  llvm::orc::JITTargetMachineBuilder JTMB((llvm::Triple(triple)));
  if (cpu) {
    JTMB.setCPU(*cpu);
  }
  if (attrs) {
    std::vector<std::string> features;
    llvm::SubtargetFeatures::Split(features, *attrs);
    JTMB.addFeatures(features);
  }
  return JTMB;
}

static llvm::orc::JITTargetMachineBuilder makeTargetMachineBuilder(
    c10::optional<std::string> triple,
    c10::optional<std::string> cpu,
    c10::optional<std::string> attrs) {
  auto JTMB = triple ? makeJTMBFromTriple(*triple, cpu, attrs)
                     : makeJTMBFromHost(cpu, attrs);
  JTMB.setCodeGenOptLevel(llvm::CodeGenOpt::Default);
  JTMB.getOptions().AllowFPOpFusion = llvm::FPOpFusion::Fast;
  return JTMB;
}

static void registerIntrinsics(
    llvm::orc::JITDylib& JD,
    llvm::orc::MangleAndInterner& Mangle,
    std::unordered_set<std::string>& intrinsics) {
  using namespace llvm;
  using namespace llvm::orc;

  auto entry = [&](const char* name, auto ptr) -> SymbolMap::value_type {
    return {Mangle(name), {toAddress(ptr), JITSymbolFlags::None}};
  };

  SymbolMap symbols;
  for (auto const& sym : getIntrinsicSymbols()) {
    symbols.insert(entry(sym.symbol, sym.address));
    intrinsics.insert(sym.symbol);
  }
  assertSuccess(JD.define(absoluteSymbols(symbols)));

  for (auto& kv : getNNCFunctionRegistry()) {
    assertSuccess(
        JD.define(absoluteSymbols({entry(kv.first.c_str(), kv.second)})));
  }
  assertSuccess(JD.define(
      absoluteSymbols({entry("DispatchParallel", DispatchParallel)})));
}

namespace llvm {
namespace orc {

// Lightly modified implementation from LLVM's Kaleidoscope JIT tutorial:
// https://llvm.org/docs/tutorial/BuildingAJIT1.html
#if LLVM_VERSION_MAJOR >= 9
class TORCH_API PytorchLLVMJITImpl {
 private:
  std::unique_ptr<TargetMachine> TM;
  std::unique_ptr<LLJIT> LLJ;
  std::unordered_set<std::string> intrinsics;

 public:
  PytorchLLVMJITImpl(
      c10::optional<std::string> triple,
      c10::optional<std::string> cpu,
      c10::optional<std::string> attrs)
      : TM(assertSuccess(makeTargetMachineBuilder(triple, cpu, attrs)
                             .createTargetMachine())),
        LLJ(assertSuccess(LLJITBuilder()
                              .setJITTargetMachineBuilder(
                                  makeTargetMachineBuilder(triple, cpu, attrs))
                              .create())) {
    auto ProcSymbolsGenerator =
        assertSuccess(DynamicLibrarySearchGenerator::GetForCurrentProcess(
            LLJ->getDataLayout().getGlobalPrefix()));
    auto& JD = LLJ->getMainJITDylib();
#if LLVM_VERSION_MAJOR == 9
    JD.setGenerator(std::move(ProcSymbolsGenerator));
#else
    JD.addGenerator(std::move(ProcSymbolsGenerator));
#endif

    // Handle platform-specific symbol mangling
    MangleAndInterner Mangle(LLJ->getExecutionSession(), LLJ->getDataLayout());

    // Register implementations of intrinsics
    registerIntrinsics(JD, Mangle, intrinsics);
  }

  void addModule(std::unique_ptr<Module> M, std::unique_ptr<LLVMContext> C) {
    assertSuccess(
        LLJ->addIRModule(ThreadSafeModule(std::move(M), std::move(C))),
        "Failed to add module to compile layer");
  }

  JITSymbol findSymbol(const std::string Name) {
    return assertSuccess(LLJ->lookup(Name));
  }

  bool hasSymbol(const std::string& Name) {
    return intrinsics.find(Name) != intrinsics.end();
  }

  TargetMachine& getTargetMachine() {
    return *TM;
  }

  const DataLayout& getDataLayout() {
    return LLJ->getDataLayout();
  }
};

#elif LLVM_VERSION_MAJOR == 8 && LLVM_VERSION_PATCH == 20181009

class TORCH_API PytorchLLVMJITImpl {
 private:
  ExecutionSession ES;
  std::shared_ptr<SymbolResolver> Resolver;
  std::unique_ptr<TargetMachine> TM;
  const DataLayout DL;
  RTDyldObjectLinkingLayer ObjectLayer;
  IRCompileLayer<decltype(ObjectLayer), SimpleCompiler> CompileLayer;
  std::unordered_set<std::string> intrinsics;

 public:
  PytorchLLVMJITImpl(
      c10::optional<std::string> triple,
      c10::optional<std::string> cpu,
      c10::optional<std::string> attrs)
      : Resolver(createLegacyLookupResolver(
            ES,
            [this](const std::string& Name) -> JITSymbol {
              if (auto Sym = CompileLayer.findSymbol(Name, false)) {
                return Sym;
              } else if (auto Err = Sym.takeError()) {
                return std::move(Err);
              }
              if (auto SymAddr =
                      RTDyldMemoryManager::getSymbolAddressInProcess(Name)) {
                return JITSymbol(SymAddr, JITSymbolFlags::Exported);
              }
              MangleAndInterner Mangle(ES, DL);
              return assertSuccess(
                  lookup({&ES.getMainJITDylib()}, Mangle(Name)));
            },
            [](Error Err) {
              assertSuccess(std::move(Err), "lookupFlags failed");
            })),
        TM(assertSuccess(makeTargetMachineBuilder(triple, cpu, attrs)
                             .createTargetMachine())),
        DL(TM->createDataLayout()),
        ObjectLayer(
            ES,
            [this](VModuleKey) {
              return RTDyldObjectLinkingLayer::Resources{
                  std::make_shared<SectionMemoryManager>(), Resolver};
            }),
        CompileLayer(ObjectLayer, SimpleCompiler(*TM)) {
    auto& JD = ES.getMainJITDylib();
    MangleAndInterner Mangle(ES, DL);
    registerIntrinsics(JD, Mangle, intrinsics);
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  }

  TargetMachine& getTargetMachine() {
    return *TM;
  }

  void addModule(std::unique_ptr<Module> M, std::unique_ptr<LLVMContext> C) {
    // Add the module to the JIT with a new VModuleKey.
    auto K = ES.allocateVModule();
    assertSuccess(
        CompileLayer.addModule(K, std::move(M)),
        "Failed to add module to compile layer");
  }

  JITSymbol findSymbol(const std::string Name) {
    std::string MangledName;
    raw_string_ostream MangledNameStream(MangledName);
    Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
    return CompileLayer.findSymbol(MangledNameStream.str(), true);
  }

  bool hasSymbol(const std::string& Name) {
    return intrinsics.find(Name) != intrinsics.end();
  }

  JITTargetAddress getSymbolAddress(const std::string Name) {
    return assertSuccess(findSymbol(Name).getAddress());
  }

  void removeModule(VModuleKey K) {
    assertSuccess(CompileLayer.removeModule(K));
  }

  const DataLayout& getDataLayout() {
    return DL;
  }
};

#else // LLVM_VERSION_MAJOR
#error Only LLVM versions 8 and above are supported.
#endif

PytorchLLVMJIT::PytorchLLVMJIT(
    c10::optional<std::string> triple,
    c10::optional<std::string> cpu,
    c10::optional<std::string> attrs)
    : impl_(std::make_unique<PytorchLLVMJITImpl>(triple, cpu, attrs)) {}

PytorchLLVMJIT::~PytorchLLVMJIT() = default;

void PytorchLLVMJIT::addModule(
    std::unique_ptr<Module> M,
    std::unique_ptr<LLVMContext> C) {
  impl_->addModule(std::move(M), std::move(C));
}

JITSymbol PytorchLLVMJIT::findSymbol(const std::string Name) {
  return impl_->findSymbol(std::move(Name));
}

bool PytorchLLVMJIT::hasSymbol(const std::string& Name) {
  return impl_->hasSymbol(Name);
}

TargetMachine& PytorchLLVMJIT::getTargetMachine() {
  return impl_->getTargetMachine();
}

const DataLayout& PytorchLLVMJIT::getDataLayout() {
  return impl_->getDataLayout();
}

#if !defined(NDEBUG)
void dumpCFG(const llvm::cfg::Update<llvm::BasicBlock*>& update) {
  // XXX: This method call is only here to placate gcov builds.  The `dump`
  // method is conditionally defined when NDEBUG is unset, so if you try to
  // link a debug-mode pytorch with an opt-mode llvm, the symbol is undefined.
  update.dump();
}
#endif

std::string PytorchLLVMJIT::getUniqueFunctionName(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = existing_functions_.find(name);
  if (it == existing_functions_.end()) {
    existing_functions_[name] = 0;
    return name;
  }
  existing_functions_[name] = it->second + 1;
  std::string unique_name = name + "_" + std::to_string(it->second + 1);
  return unique_name;
}

std::unordered_map<std::string, std::unique_ptr<PytorchLLVMJIT>>
    PytorchLLVMJITCache::jit_cache_;
std::mutex PytorchLLVMJITCache::mutex_;

PytorchLLVMJIT* PytorchLLVMJITCache::getPytorchLLVMJITInstance(
    c10::optional<std::string> triple,
    c10::optional<std::string> cpu,
    c10::optional<std::string> attrs) {
  std::string cacheKey = getCacheKey(triple, cpu, attrs);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = jit_cache_.find(cacheKey);
  if (it != jit_cache_.end()) {
    return it->second.get();
  }
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  auto jit = std::make_unique<PytorchLLVMJIT>(triple, cpu, attrs);
  auto jit_to_return = jit.get();
  jit_cache_[cacheKey] = std::move(jit);
  return jit_to_return;
}

std::string PytorchLLVMJITCache::getCacheKey(
    c10::optional<std::string> triple,
    c10::optional<std::string> cpu,
    c10::optional<std::string> attrs) {
  return "triple:" + std::string(triple ? *triple : "") +
      "cpu:" + std::string(cpu ? *cpu : "") +
      "attrs:" + std::string(attrs ? *attrs : "");
}

} // end namespace orc
} // end namespace llvm

#endif // TORCH_ENABLE_LLVM
