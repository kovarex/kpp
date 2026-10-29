#pragma once
// Code generation for the Kpp v0 subset: textual LLVM IR for the whole program, compiled to
// machine code by the bundled clang. See docs/design.md, "Kpp v0".
#include "ast.hpp"
#include "resolver.hpp"
#include <memory>
#include <string>
#include <vector>

struct CodegenResult
{
  std::string ir;                   // the whole program as one module (moduleCount == 1)
  std::vector<std::string> modules; // or one module per group of files, each complete on its own
  std::vector<std::string> errors; // "file:line: message"
  std::vector<std::string> failedFunctions; // IR names of the functions an error left out
  size_t functions = 0;
  size_t structs = 0;
};

// debug: config::debug; `assert` statements compile to nothing when it is false.
// moduleCount > 1 splits the output into that many modules (functions by file, instantiations
// by the hash of their name) so that LLVM can compile them in parallel.
// despiteErrors: assemble the IR even when there are errors (--keep-going): the functions with
// errors are left out and only declared, so that what remains can be compiled and linked.
// threads: how many threads emit functions at once (the output does not depend on it).
CodegenResult generateLLVM(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols,
                           bool debug = true, unsigned moduleCount = 1, bool despiteErrors = false, unsigned threads = 1);
