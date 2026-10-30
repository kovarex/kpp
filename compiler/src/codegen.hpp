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
  size_t stubs = 0;                 // with stubMissing: functions left out that are stubs aborting when called
  size_t functions = 0;
  size_t structs = 0;
  std::vector<bool> changedModules; // rebuildLLVM: which modules it assembled again (the others' texts are empty here)
};

// Codegen's state kept between builds by the compile server (docs/compile-speed.md, step 5b): the
// shared texts, layouts and names, each module's definitions with their owners, the emitters. It
// points into the program (GlobalScope, types, the parsed files): destroy it before them.
struct CodegenSession;
struct CodegenSessionDeleter
{
  void operator()(CodegenSession* session) const;
};
using CodegenSessionPtr = std::unique_ptr<CodegenSession, CodegenSessionDeleter>;

// debug: config::debug; `assert` statements compile to nothing when it is false.
// moduleCount > 1 splits the output into that many modules (functions by file, instantiations
// by the hash of their name) so that LLVM can compile them in parallel.
// despiteErrors: assemble the IR even when there are errors (--keep-going): the functions with
// errors are left out and only declared, so that what remains can be compiled and linked.
// threads: how many threads emit functions at once (the output does not depend on it).
// stubMissing (with despiteErrors; a bring-up tool, never the default): every function an error
// left out, and every one used without a definition, is defined as a stub that aborts with its
// name and line when called, so that the program links and runs up to the first one it reaches.
// inlineTrivial: calls of trivial functions are emitted as the callees' bodies (docs/compile-speed.md,
// "Design: inlining trivial functions in kppc"); false (--no-inline) keeps every call.
// keep: hand the session back (a build of several modules without errors) instead of freeing it.
CodegenResult generateLLVM(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols,
                           bool debug = true, unsigned moduleCount = 1, bool despiteErrors = false, unsigned threads = 1,
                           bool stubMissing = false, bool inlineTrivial = true, CodegenSessionPtr* keep = nullptr);

// An incremental build after the resolver checked the changed bodies again (its steps 3b and 4):
// emits again the functions whose declarations got new bodies (or were walked again), emits the
// added ones (plain program functions), drops what the removed ones' and the old bodies' emissions
// added (their definitions, their lambdas' operators, their static locals), and assembles again the
// modules that changed: result.changedModules says which, the others' texts are left empty. Only
// program FunctionDecls, never instances. rebuilt is false when it cannot (an error, a change to
// the shared declarations): the session is then unusable, and the caller builds from scratch.
CodegenResult rebuildLLVM(CodegenSession& session, const std::vector<FunctionDecl*>& changedBodies, const std::vector<FunctionDecl*>& added,
                          const std::vector<FunctionDecl*>& removed, bool& rebuilt);
