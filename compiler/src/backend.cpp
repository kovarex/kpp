#include "backend.hpp"

#ifndef KPP_WITH_LLVM

bool inProcessBackend() { return false; }

bool compileModule(std::string_view, const BackendOptions&, std::string&, std::string& error)
{
  error = "this kpp was built without LLVM's back end (CMake's KPP_LLVM_DIR)";
  return false;
}

#else

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
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
#include <vector>

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
  // text where clang would have read it, so the failure is never kpp exiting with no module named.
  void fatalError(void*, const char* reason, bool)
  {
    if (compiling)
    {
      std::ofstream(compiling->options->path, std::ios::binary).write(compiling->ir.data(), std::streamsize(compiling->ir.size()));
      fprintf(stderr, "%s: error: LLVM's back end failed on module %u, which ends kpp: %s\n", compiling->options->path.c_str(),
              compiling->options->module, reason);
    }
    else
      fprintf(stderr, "kpp: error: LLVM failed outside a module, which ends kpp: %s\n", reason);
    fflush(stderr);
  }

  // The targets kpp builds for (design.md, "Targets and hosts"): x86-64 and AArch64.
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
      // compileModule verifies a module once. LLVM's parser would run the verifier a second time, for debug
      // information of older versions than kpp writes (UpgradeDebugInfo: 3.4% of a debug build's back end).
      const char* arguments[] = {"kpp", "-disable-auto-upgrade-debug-info"};
      llvm::cl::ParseCommandLineOptions(2, arguments);
    });
  }

  // What LLVM reports while it compiles a module (inline asm the assembler rejects, a call it cannot
  // lower, ...). Without a handler of the context's, an error prints and exit(1)s the whole process
  // from a worker thread: here it fails the module instead, as clang's exit code did. Warnings are
  // printed, never dropped; a note goes with the error before it.
  struct Diagnostics
  {
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
        break; // (only the remarks asked for arrive, and kpp asks for none)
      }
    }

    const BackendOptions* options;
    std::string errors;
  };

  // Each `asm "nop"` in a function (Emitter::emitCall: two per invoke, 631,606 in the game's debug build) as
  // the instruction it is: the asm printer makes a new asm parser for every inline asm, 3.9% of that build's
  // back end. The object is the same, byte for byte, so this runs right before the printer (the passes
  // before it tell an inline asm from an instruction: on Win64, whether a call is a function's last
  // instruction), and a nop between a call and an epilogue stays an inline asm: the printer's own nop after a
  // call before an epilogue looks back over pseudo instructions, and an inline asm is one, an instruction not.
  struct NopsAsInstructions : llvm::MachineFunctionPass
  {
    static char ID;

    NopsAsInstructions() : llvm::MachineFunctionPass(ID) {}

    llvm::StringRef getPassName() const override { return "kpp's nops as instructions"; }

    void getAnalysisUsage(llvm::AnalysisUsage& usage) const override
    {
      usage.setPreservesAll();
      llvm::MachineFunctionPass::getAnalysisUsage(usage);
    }

    bool runOnMachineFunction(llvm::MachineFunction& function) override
    {
      const llvm::TargetInstrInfo& instructions = *function.getSubtarget().getInstrInfo();
      const unsigned nop = instructions.getNop().getOpcode();
      bool changed = false;
      auto replace = [&](llvm::MachineInstr& asmNop)
      {
        llvm::BuildMI(*asmNop.getParent(), asmNop, asmNop.getDebugLoc(), instructions.get(nop));
        asmNop.eraseFromParent();
        changed = true;
      };
      // (the nops after the last call with only pseudo instructions since it, across blocks as the printer looks)
      std::vector<llvm::MachineInstr*> sinceCall;
      auto replaceThose = [&]
      {
        for (llvm::MachineInstr* asmNop : sinceCall)
          replace(*asmNop);
        sinceCall.clear();
      };
      bool afterCall = false;
      for (llvm::MachineBasicBlock& block : function)
        for (llvm::MachineInstr& instruction : llvm::make_early_inc_range(block))
          if (instruction.isCall())
          {
            replaceThose();
            afterCall = true;
          }
          else if (instruction.isInlineAsm() && llvm::StringRef(instruction.getOperand(llvm::InlineAsm::MIOp_AsmString).getSymbolName()) == "nop")
          {
            if (afterCall)
              sinceCall.push_back(&instruction);
            else
              replace(instruction);
          }
          else if (!instruction.isPseudo())
          {
            replaceThose();
            afterCall = false;
          }
          else if (!sinceCall.empty() && instructions.getName(instruction.getOpcode()) == "SEH_Epilogue")
          {
            sinceCall.clear();
            afterCall = false;
          }
      replaceThose();
      return changed;
    }
  };
  char NopsAsInstructions::ID = 0;

  // The passes LLVM's target machine adds to write an object, with kpp's before its asm printer (x86-64's;
  // the nops of other targets go through the asm parser).
  struct ObjectPasses : llvm::legacy::PassManagerBase
  {
    explicit ObjectPasses(llvm::legacy::PassManager& passes) : passes(passes) {}

    void add(llvm::Pass* pass) override
    {
      if (pass->getPassName() == "X86 Assembly Printer")
        passes.add(new NopsAsInstructions);
      passes.add(pass);
    }

    llvm::legacy::PassManager& passes;
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
    diagnostic.print("kpp", out);
    error = out.str();
    return false;
  }
  // The IR checked before the back end sees it: an IR bug of kpp's is the verifier's message here,
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
  // The target is the module's own (kpp's target triple and data layout); LLVM's must agree with it,
  // never replace it: a data layout of kpp's that is not LLVM's for the triple is kpp's error.
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
    error = "kpp's data layout \"" + module->getDataLayoutStr() + "\" is not LLVM's for " + triple + ": \"" + layout + "\"";
    return false;
  }
  // clang's -O0 pipeline: the always-inliner (kpp's alwaysinline functions) and what else it runs.
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
  ObjectPasses passes(codegen);
  if (machine->addPassesToEmitFile(passes, out, nullptr, llvm::CodeGenFileType::ObjectFile, /*DisableVerify*/ true))
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
