#pragma once
// LLVM's back end inside kppc (docs/codegen-speed.md, "The integration in three steps", step 2): a
// module's IR compiled to an object in this process, on the calling thread, as `clang -O0 -c` compiles
// it (the same -O0 pipeline, the same target options), without writing the IR to a file or starting a
// process. Built only when CMake is given LLVM (KPP_LLVM_DIR); otherwise inProcessBackend() is false
// and the driver runs clang per module as before.
#include <string>
#include <string_view>

struct BackendOptions
{
  std::string triple;  // the target's (the module's own when empty)
  bool optimize = false; // -O0 only so far: the release build's ThinLTO stays clang's and lld's
  std::string sourceName; // the object's `.file` symbol: the name of the IR file clang would have read
};

// Whether this kppc was built with LLVM's back end in it.
bool inProcessBackend();

// The object file for module text ir (textual LLVM IR), in object; false with error set when LLVM
// rejects the IR or the target. Thread-safe: each call has an LLVMContext and a TargetMachine of its own.
bool compileModule(std::string_view ir, const BackendOptions& options, std::string& object, std::string& error);
