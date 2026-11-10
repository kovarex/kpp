#pragma once
// Code generation for the kpp v0 subset: textual LLVM IR for the whole program, compiled to
// machine code by the bundled clang. See docs/design.md, "kpp v0".
#include "Ast/Ast.hpp"
#include "resolver.hpp"
#include "target.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

// A class the debug information defines (--debug-info with variables), by the name its definition
// gives it (debuggerClassName, Codegen/DebugInfo.inl): what the .natvis's per-instance rules name and
// cast to (makeNatvis), so that the two agree by construction.
struct DebugClass
{
  const struct Ty* type;
  std::string name;
};

struct CodegenResult
{
  // Whether codegen failed, its failures shown or not: a function a hidden one left out is left out
  // too (declared or stubbed with --keep-going, emitted again by a compile server's next build).
  bool failed() const { return !errors.empty() || hiddenErrors != 0; }

  std::string ir;                   // the whole program as one module (moduleCount == 1)
  std::vector<std::string> modules; // or one module per group of files, each complete on its own (codegenOutputModules)
  std::vector<std::string> errors; // "file:line: message"
  size_t hiddenErrors = 0;         // failures not in errors: each follows from an error the checker reported (not shown again)
  std::vector<std::string> failedFunctions; // IR names of the functions an error left out
  size_t stubs = 0;                 // with stubMissing: functions left out that are stubs aborting when called
  size_t functions = 0;
  size_t structs = 0;
  std::vector<bool> changedModules; // rebuildLLVM: which modules it assembled again (the others' texts are empty here)
  // With a PieceStream: the program pieces whose object the stream compiles, its text being modules[k]
  // (handed over at level 0's barrier, the same at the end).
  std::vector<bool> streamed;
  size_t emittedAgain = 0;          // rebuildLLVM: the functions it emitted (the changed and added ones, those that inlined a changed body, first uses)
  std::string declined;             // rebuildLLVM: why it did not rebuild (rebuilt false), for the server's log
  size_t instanceAssertsChecked = 0; // the class template instances whose static_asserts it checked (GlobalScope::checkerErrors)
  std::vector<DebugClass> debugClasses; // --debug-info: the classes the program's modules define, sorted by name
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

// Step 4's P2 (docs/codegen-speed.md): a debug build's program pieces handed over at level 0's barrier,
// so that the back end compiles them while codegen emits the instances. piece(k, text) is called on
// codegen's threads, once for each program piece the barrier has whole (k < the module count, its index
// in CodegenResult::modules), when no error came before, and takes the text. The end assembles every
// piece again: CodegenResult::streamed says which are the text handed over.
struct PieceStream
{
  std::function<void(unsigned k, std::string&& text)> piece;
};

// What a build asks of codegen (generateLLVM), each by its name (J's refactoring list, item 14: the
// entry took them as nine positional parameters, seven of them bools).
struct CodegenOptions
{
  bool debug = true; // config::debug; `assert` statements compile to nothing when it is false.
  // > 1 splits the output into that many modules (functions by file, instantiations by the hash of
  // their name) so that LLVM can compile them in parallel.
  unsigned moduleCount = 1;
  // Assemble the IR even when there are errors (--keep-going): the functions with errors are left out
  // and only declared, so that what remains can be compiled and linked.
  bool despiteErrors = false;
  unsigned threads = 1; // how many threads emit functions at once (the output does not depend on it)
  // With despiteErrors (a bring-up tool, never the default): every function an error left out, and
  // every one used without a definition, is defined as a stub that aborts with its name and line when
  // called, so that the program links and runs up to the first one it reaches.
  bool stubMissing = false;
  // Calls of trivial functions are emitted as the callees' bodies (docs/compile-speed.md, "Design:
  // inlining trivial functions in kpp"); false (--no-inline) keeps every call.
  bool inlineTrivial = true;
  // --debug-info: line tables, each statement's instructions at its line, so that a debugger steps
  // through the kpp source (docs/debugging.md).
  bool debugInfo = false;
  const Target* target = nullptr; // the platform the IR is for (--target; null: Windows x64)
  bool optimize = false; // --optimize: the modules are optimized (ThinLTO), and not split (codegenOutputModules)
};

// keep: hand the session back (a build of several modules) instead of freeing it: one with errors only
// when each came from a program function's own emission (none assembled then).
// stream: the program pieces handed over at level 0's barrier (PieceStream).
CodegenResult generateLLVM(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols,
                           const CodegenOptions& options, CodegenSessionPtr* keep = nullptr, PieceStream* stream = nullptr);

// How many modules a build of moduleCount modules writes (CodegenResult::modules): a debug build of
// several, not optimized, has each in two, its program piece (what the functions the program writes
// own) and its instance piece (the rest), the program pieces first (docs/codegen-speed.md, step 4).
// (The pieces are for kpp's -O0 back end; an optimized build's is ThinLTO's link, where twice the
// modules only cost.)
unsigned codegenOutputModules(unsigned moduleCount, bool debug, bool optimize);

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
// replaced: the bodies the edit replaced (changedBodies holds their readers too): only their lambdas' closures are new types, so
// only what was made for those goes; a function emitted again with the body it had keeps what was made for its closures.
// movedLines (--debug-info): functions kept as they were whose lines an edit moved: emitted again for their line tables
// (a template's: its instances), and nothing else with them, not their callers (inlined code carries no lines of its callee).
// A build that assembled modules and compiled none of them (it stopped at an error outside codegen: the
// checker's): those modules (changedModules, a build's result; empty: all of them, a build in full)
// are assembled again by the next build, whose own changes may leave their texts as this build had
// them, which the objects never got (an unchanged module keeps its object: the server's build after a
// fixed static_assert linked the object from before the edit, silently; thread K, 2026-11-07).
void unassembledAgain(CodegenSession& session, const std::vector<bool>& changedModules);

CodegenResult rebuildLLVM(CodegenSession& session, const std::vector<FunctionDecl*>& changedBodies, const std::vector<FunctionDecl*>& movedLines,
                          const std::vector<FunctionDecl*>& replaced, const std::vector<FunctionDecl*>& added, const std::vector<FunctionDecl*>& removed, const std::vector<const Decl*>& changedLayouts,
                          const std::vector<const Decl*>& instancesOf, const std::vector<const Decl*>& changedGlobals,
                          const std::vector<const Ty*>& droppedClasses, bool& rebuilt);
