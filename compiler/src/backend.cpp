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
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

bool inProcessBackend() { return true; }

namespace
{
  // The module this thread is compiling, for LLVM's fatal-error handler.
  struct Compiling
  {
    std::string_view ir;
    const BackendOptions* options;
  };
  thread_local const Compiling* compiling = nullptr;

  // A report_fatal_error in LLVM ends the process when this handler returns (LLVM's rule: it cannot be
  // recovered from), on whichever thread it happened. Before that, it names the module and writes its
  // text where clang would have read it, so the failure is never kppc exiting with no module named.
  void fatalError(void*, const char* reason, bool)
  {
    if (compiling)
    {
      std::ofstream(compiling->options->path, std::ios::binary).write(compiling->ir.data(), std::streamsize(compiling->ir.size()));
      fprintf(stderr, "%s: error: LLVM's back end failed on module %u, which ends kppc: %s\n", compiling->options->path.c_str(),
              compiling->options->module, reason);
    }
    else
      fprintf(stderr, "kppc: error: LLVM failed outside a module, which ends kppc: %s\n", reason);
    fflush(stderr);
  }

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
      llvm::install_fatal_error_handler(fatalError);
    });
  }

  // What LLVM reports while it compiles a module (inline asm the assembler rejects, a call it cannot
  // lower, ...). Without a handler of the context's, an error prints and exit(1)s the whole process
  // from a worker thread: here it fails the module instead, as clang's exit code did. Warnings are
  // printed, never dropped; a note goes with the error before it.
  struct Diagnostics
  {
    const BackendOptions* options;
    std::string errors;

    static void handle(const llvm::DiagnosticInfo* diagnostic, void* self)
    {
      const llvm::DiagnosticInfo& info = *diagnostic;
      Diagnostics& d = *static_cast<Diagnostics*>(self);
      std::string text;
      llvm::raw_string_ostream out(text);
      llvm::DiagnosticPrinterRawOStream printer(out);
      info.print(printer);
      out.flush();
      switch (info.getSeverity())
      {
      case llvm::DS_Error:
        d.errors += "error: " + text + "\n";
        break;
      case llvm::DS_Note:
        if (!d.errors.empty())
        {
          d.errors += "note: " + text + "\n";
          break;
        }
        fprintf(stderr, "%s: note: %s\n", d.options->path.c_str(), text.c_str());
        break;
      case llvm::DS_Warning:
        fprintf(stderr, "%s: warning: %s\n", d.options->path.c_str(), text.c_str());
        break;
      case llvm::DS_Remark:
        break; // (only the remarks asked for arrive, and kppc asks for none)
      }
    }
  };
}

bool compileModule(std::string_view ir, const BackendOptions& options, std::string& object, std::string& error)
{
  initializeTargets();
  Compiling current{ir, &options};
  compiling = &current;
  struct Done
  {
    ~Done() { compiling = nullptr; }
  } done;
  llvm::LLVMContext context;
  Diagnostics diagnostics{&options, {}};
  context.setDiagnosticHandlerCallBack(&Diagnostics::handle, &diagnostics);
  llvm::SMDiagnostic diagnostic;
  std::string sourceName = std::filesystem::path(options.path).filename().string(); // (the `.file` symbol clang writes)
  std::unique_ptr<llvm::Module> module =
    llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(ir.data(), ir.size()), sourceName), diagnostic, context);
  if (!module)
  {
    std::string text;
    llvm::raw_string_ostream out(text);
    diagnostic.print("kppc", out);
    error = out.str();
    return false;
  }
  // The IR checked before the back end sees it: an IR bug of kppc's is the verifier's message here,
  // never a crash in the back end or an object built from broken IR.
  {
    std::string text;
    llvm::raw_string_ostream out(text);
    if (llvm::verifyModule(*module, &out))
    {
      error = "LLVM's verifier: " + out.str();
      return false;
    }
  }
  // The target is the module's own (kppc's target triple and data layout); LLVM's must agree with it,
  // never replace it: a data layout of kppc's that is not LLVM's for the triple is kppc's error.
  std::string triple = module->getTargetTriple();
  if (triple.empty())
  {
    error = "the module names no target triple";
    return false;
  }
  std::string lookupError;
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupError);
  if (!target)
  {
    error = lookupError;
    return false;
  }
  // As clang -O0 -c -ffunction-sections -fdata-sections -ffp-contract=off sets them for its back end
  // (the driver's own compile line; `clang -###` shows the cc1 flags): sections per function and per
  // global (not in an --incremental-link build, whose compile line has neither), address-significance
  // tables, strict floating-point contraction, position-independent code, the generic x86-64 CPU; and
  // per target what Visual Studio's clang (windows) and llvm-mingw's (windows-gnu, the same LLVM
  // 19.1.7) set apart.
  llvm::Triple parsed(triple);
  llvm::TargetOptions targetOptions;
  targetOptions.FunctionSections = options.functionSections;
  targetOptions.DataSections = options.functionSections;
  targetOptions.EmitAddrsig = true;
  targetOptions.AllowFPOpFusion = llvm::FPOpFusion::Strict;
  if (parsed.isWindowsGNUEnvironment())
  {
    // llvm-mingw's: -fno-use-init-array (MinGW-w64's runtime runs the .ctors section, which LLVM
    // writes in reverse so that it runs in llvm.global_ctors' order), -exception-model=seh, and no
    // -mincremental-linker-compatible (lld links it, not link.exe /INCREMENTAL).
    targetOptions.UseInitArray = false;
    targetOptions.ExceptionModel = llvm::ExceptionHandling::WinEH;
  }
  else
  {
    // Visual Studio's: objects link.exe /INCREMENTAL can patch, and constructors run in the order
    // llvm.global_ctors lists them (.CRT$XCU). LLVM's default (UseInitArray false: the legacy .ctors
    // section) emitted the list reversed, so the globals were built at startup in the opposite order
    // (found comparing the game's objects with clang's).
    targetOptions.MCOptions.MCIncrementalLinkerCompatible = true;
    targetOptions.UseInitArray = true;
  }
  const char* cpu = parsed.getArch() == llvm::Triple::x86_64 ? "x86-64" : parsed.isAArch64() ? "generic" : "";
  std::unique_ptr<llvm::TargetMachine> machine(
    target->createTargetMachine(triple, cpu, "", targetOptions, llvm::Reloc::PIC_, std::nullopt, llvm::CodeGenOptLevel::None));
  if (!machine)
  {
    error = "no target machine for " + triple;
    return false;
  }
  std::string layout = machine->createDataLayout().getStringRepresentation();
  if (module->getDataLayoutStr() != layout)
  {
    error = "kppc's data layout \"" + module->getDataLayoutStr() + "\" is not LLVM's for " + triple + ": \"" + layout + "\"";
    return false;
  }
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
  // (DisableVerify: the module was verified above.)
  if (machine->addPassesToEmitFile(codegen, out, nullptr, llvm::CodeGenFileType::ObjectFile, /*DisableVerify*/ true))
  {
    error = "the target cannot emit an object file";
    return false;
  }
  codegen.run(*module);
  if (!diagnostics.errors.empty())
  {
    error = diagnostics.errors;
    return false;
  }
  object.assign(buffer.data(), buffer.size());
  return true;
}

#endif
