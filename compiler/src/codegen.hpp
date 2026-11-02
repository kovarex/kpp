#pragma once
// Code generation for the kpp v0 subset: textual LLVM IR for the whole program, compiled to
// machine code by the bundled clang. See docs/design.md, "kpp v0".
#include "ast.hpp"
#include "resolver.hpp"
#include "target.hpp"
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
  size_t emittedAgain = 0;          // rebuildLLVM: the functions it emitted (the changed and added ones, those that inlined a changed body, first uses)
  std::string declined;             // rebuildLLVM: why it did not rebuild (rebuilt false), for the server's log
  size_t instanceAssertsChecked = 0; // the class template instances whose static_asserts it checked (GlobalScope::checkerErrors)
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
// keep: hand the session back (a build of several modules) instead of freeing it: one with errors only
// when each came from a program function's own emission (none assembled then).
// debugInfo (--debug-info): line tables, each statement's instructions at its line, so that a
// debugger steps through the kpp source (docs/debugging.md).
// target: the platform the IR is for (--target; null: Windows x64).
CodegenResult generateLLVM(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols,
                           bool debug = true, unsigned moduleCount = 1, bool despiteErrors = false, unsigned threads = 1,
                           bool stubMissing = false, bool inlineTrivial = true, CodegenSessionPtr* keep = nullptr,
                           bool debugInfo = false, const Target* target = nullptr);

// An incremental build after the resolver checked the changed bodies again (its steps 3b and 4):
// emits again the functions whose declarations got new bodies (or were walked again), emits the
// added ones (plain program functions), drops what the removed ones' and the old bodies' emissions
// added (their definitions, their lambdas' operators, their static locals), and assembles again the
// modules that changed: result.changedModules says which, the others' texts are left empty. The
// lists hold FunctionDecls, never instances: those of a template (or of a local class) are skipped,
// what is emitted of them being their instances. The functions (instances included) whose emission
// inlined a changed or removed body, or ran it in a constant fold, are emitted again too. The
// callers of an edited function come in changedBodies (the server's walksReading): codegen knows
// only the callers that inline, and one that only called it inlines it once it is trivial. Errors of
// program functions' own emissions keep the session (those functions are emitted again by every
// later build; no module is assembled until a build has none: it assembles every module changed
// since). rebuilt is false when it cannot (another error, a change to the shared declarations;
// result.declined says which): the session is then unusable, and the caller builds from scratch. changedLayouts: the
// classes whose data members changed (non-template ones): their layouts and those holding them are
// laid out again, and every function that used one of them is emitted again (declined for a
// polymorphic one, or one a destructor helper, a descriptor, a global or a lambda's capture holds).
// instancesOf: templates one of whose instances read a change (the resolver checked it again or
// took it out): all their instances, and the functions that inlined one, are emitted again.
CodegenResult rebuildLLVM(CodegenSession& session, const std::vector<FunctionDecl*>& changedBodies, const std::vector<FunctionDecl*>& added,
                          const std::vector<FunctionDecl*>& removed, const std::vector<const Decl*>& changedLayouts,
                          const std::vector<const Decl*>& instancesOf, const std::vector<const Decl*>& changedGlobals, bool& rebuilt);
