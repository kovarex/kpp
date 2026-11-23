#pragma once
// kpp's C++ back end (--via-cpp; docs/cpp-backend.md): a module's IR written as one C++ translation unit,
// for a platform whose own C++ compiler builds the program (kovarex, 2026-11-23: Nintendo's). The C++ is
// what the code generator already lowered, operation by operation: flat storage, one statement per
// operation, gotos. It is an intermediate build step, not source to read. Built only when CMake is given
// LLVM (KPP_LLVM_DIR), which reads the module.
#include <string>
#include <string_view>

struct CppWriterOptions
{
  std::string path;    // the module's IR file, for the messages
  unsigned module = 0; // its number, for the messages
};

// Whether this kpp can write C++ (it was built with LLVM in it).
bool cppWriterAvailable();

// The C++ translation unit for module text ir (textual LLVM IR of a target whose exceptions are the Itanium
// model's landing pads: Target::seh() false), in cpp; false with error set when LLVM rejects the IR or the
// module holds what the writer does not translate (the first such construct named: never a guess).
// Thread-safe: each call has an LLVMContext of its own.
bool writeCppModule(std::string_view ir, const CppWriterOptions& options, std::string& cpp, std::string& error);

// The C++ source of what the translation units call besides the program (kpp_cpp_raise, kpp_cpp_resume,
// kpp_cpp_uncaught): a kpp exception travels as a C++ exception of one type.
const char* cppRuntimeSource();
