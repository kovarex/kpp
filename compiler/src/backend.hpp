#pragma once
// LLVM's back end inside kpp (docs/codegen-speed.md, "The integration in three steps", step 2): a
// module's IR compiled to an object in this process, on the calling thread, as `clang -O0 -c` compiles
// it (the same -O0 pipeline, the same target options), without writing the IR to a file or starting a
// process. Built only when CMake is given LLVM (KPP_LLVM_DIR); otherwise inProcessBackend() is false
// and the driver runs clang per module as before.
#include <string>
#include <string_view>

struct BackendOptions
{
  std::string path;    // the module's IR file, where clang would have read it: its file name is the object's
                       // `.file` symbol, and the text is written there only when LLVM fails on it
  unsigned module = 0; // its number, for the messages
  bool functionSections = true; // -ffunction-sections -fdata-sections: not in an --incremental-link build (main.cpp)
};

// Whether this kpp was built with LLVM's back end in it.
bool inProcessBackend();

// The object file for module text ir (textual LLVM IR), in object; false with error set when LLVM
// rejects the IR (the parser or the verifier), the target, or what the back end makes of it (its
// diagnostics' errors; its warnings go to stderr). A fatal error of LLVM's ends the process after
// naming the module. Thread-safe: each call has an LLVMContext and a TargetMachine of its own.
bool compileModule(std::string_view ir, const BackendOptions& options, std::string& object, std::string& error);
