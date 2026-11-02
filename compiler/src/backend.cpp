#include "backend.hpp"

#ifndef KPP_WITH_LLVM

bool inProcessBackend() { return false; }

bool compileModule(std::string_view, const BackendOptions&, std::string&, std::string& error)
{
  error = "this kppc was built without LLVM's back end (CMake's KPP_LLVM_DIR)";
  return false;
}

#else

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include <memory>
#include <mutex>

bool inProcessBackend() { return true; }

namespace
{
  // The targets kppc builds for (design.md, "Targets and hosts"): x86-64 and AArch64.
  void initializeTargets()
  {
    static std::once_flag once;
    std::call_once(once, []
    {
      LLVMInitializeX86TargetInfo();
      LLVMInitializeX86Target();
      LLVMInitializeX86TargetMC();
      LLVMInitializeX86AsmPrinter();
      LLVMInitializeX86AsmParser(); // (inline asm in the modules)
      LLVMInitializeAArch64TargetInfo();
      LLVMInitializeAArch64Target();
      LLVMInitializeAArch64TargetMC();
      LLVMInitializeAArch64AsmPrinter();
      LLVMInitializeAArch64AsmParser();
    });
  }
}

bool compileModule(std::string_view ir, const BackendOptions& options, std::string& object, std::string& error)
{
  initializeTargets();
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  std::unique_ptr<llvm::Module> module =
    llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(ir.data(), ir.size()), options.sourceName.empty() ? "kpp-module" : options.sourceName),
                  diagnostic, context);
  if (!module)
  {
    std::string text;
    llvm::raw_string_ostream out(text);
    diagnostic.print("kppc", out);
    error = out.str();
    return false;
  }
  std::string triple = options.triple.empty() ? module->getTargetTriple() : options.triple;
  module->setTargetTriple(triple);
  std::string lookupError;
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupError);
  if (!target)
  {
    error = lookupError;
    return false;
  }
  // As clang -O0 -c -ffunction-sections -fdata-sections -ffp-contract=off sets them for its back end
  // (the driver's own compile line; `clang -###` shows the cc1 flags): sections per function and per
  // global, address-significance tables, strict floating-point contraction, objects an incremental
  // link can patch, position-independent code, the generic x86-64 CPU.
  llvm::TargetOptions targetOptions;
  targetOptions.FunctionSections = true;
  targetOptions.DataSections = true;
  targetOptions.EmitAddrsig = true;
  targetOptions.AllowFPOpFusion = llvm::FPOpFusion::Strict;
  targetOptions.MCOptions.MCIncrementalLinkerCompatible = true;
  // Constructors run in the order llvm.global_ctors lists them, as clang sets it: LLVM's default
  // (false: the legacy .ctors section, run backwards) emitted the list reversed, so the globals were
  // built at startup in the opposite order (found comparing the game's objects with clang's).
  targetOptions.UseInitArray = true;
  llvm::Triple parsed(triple);
  const char* cpu = parsed.getArch() == llvm::Triple::x86_64 ? "x86-64" : parsed.isAArch64() ? "generic" : "";
  std::unique_ptr<llvm::TargetMachine> machine(
    target->createTargetMachine(triple, cpu, "", targetOptions, llvm::Reloc::PIC_, std::nullopt, llvm::CodeGenOptLevel::None));
  if (!machine)
  {
    error = "no target machine for " + triple;
    return false;
  }
  module->setDataLayout(machine->createDataLayout());
  // clang's -O0 pipeline: the always-inliner (kppc's alwaysinline functions) and what else it runs.
  {
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager cgscc;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder builder(machine.get());
    builder.registerModuleAnalyses(modules);
    builder.registerCGSCCAnalyses(cgscc);
    builder.registerFunctionAnalyses(functions);
    builder.registerLoopAnalyses(loops);
    builder.crossRegisterProxies(loops, functions, cgscc, modules);
    llvm::ModulePassManager pipeline = builder.buildO0DefaultPipeline(llvm::OptimizationLevel::O0);
    pipeline.run(*module, modules);
  }
  llvm::SmallVector<char, 0> buffer;
  llvm::raw_svector_ostream out(buffer);
  llvm::legacy::PassManager codegen;
  codegen.add(new llvm::TargetLibraryInfoWrapperPass(llvm::TargetLibraryInfoImpl(parsed)));
  if (machine->addPassesToEmitFile(codegen, out, nullptr, llvm::CodeGenFileType::ObjectFile, /*DisableVerify*/ true))
  {
    error = "the target cannot emit an object file";
    return false;
  }
  codegen.run(*module);
  object.assign(buffer.data(), buffer.size());
  return true;
}

#endif
