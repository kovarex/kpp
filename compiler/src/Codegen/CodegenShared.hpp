// What codegen's emitters share (Shared) and the records they keep (StructInfo, Vtable, Request,
// ...). Included by Codegen/Emitter.hpp, in namespace codegen (a file of its own since 2026-11-05,
// moved without a change).
#pragma once
#include "codegen.hpp"
#include "Ast/AstWalk.hpp"
#include "consteval.hpp"
#include "source.hpp"
#include "Restore.hpp"
#include "LineStarts.hpp"
#include <map>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <condition_variable>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#include <numeric>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif

namespace codegen
{
  // A stub's definition (--stub-missing): the function `name`, aborting with message when called
  // (CodegenSession.inl).
  std::string stubText(const std::string& name, const std::string& ret, const std::string& params, const std::string& message);

  // One slot of a vtable: the function it calls (the final overrider), the class that function is
  // a member of, and the base fields from the vtable's class down to that class's subobject, where
  // `this` has to point (a thunk moves it there from the subobject whose vtable pointer was used).
  // A key has several slots in a table where a covariant override's result does not start with the
  // class its callers so far expect: the override takes a slot of its own, the last of its key, and
  // the earlier ones keep their callers, each with the result they expect (design.md section 9).
  struct VtableSlot
  {
    std::string key;                  // name(param types) [const], "~" for the destructor
    FunctionDecl* function = nullptr; // null: pure virtual, or (key "~") the owner's implicit destructor
    const Ty* owner = nullptr;
    std::vector<uint32_t> ownerPath;
    bool overridesTest = false; // over a test function, whose call notes its site (Emitter::forgetTestCall)
    const Ty* result = nullptr;       // the class whose pointer or reference this slot's callers expect back: the function's that made the slot, whichever overrides it
  };
  // One vtable pointer in the objects of a class, and the table it points at (design.md section 9).
  struct Vtable
  {
    std::vector<uint32_t> path;       // base fields from the class down to the subobject holding the pointer (empty: offset 0)
    std::vector<VtableSlot> slots;
    std::string name;                 // @"vtable.C" for the primary, @"vtable.C.1" and on for the others
    std::string vptrValue;            // what the pointer holds: the address of the first function's slot
  };

  // A bit field's place in its class's layout: its bits of a storage unit.
  struct BitfieldInfo
  {
    uint32_t shift, width;
    const Ty* unit;     // the storage unit's (integer) type, as it is read and written
    bool bytes = false; // the unit is a byte array (SysV's layout off Windows): read and written align 1
  };

#include "StructInfo.hpp"
  // The field index of a member, 0 when the layout has no such member.
  inline uint32_t fieldIndexOf(const StructInfo& info, Decl* member)
  {
    auto it = info.fieldIndex.find(member);
    return it == info.fieldIndex.end() ? 0 : it->second;
  }
  // The place of bit-field member d in the layout info, or null when d is no bit field of it.
  inline const BitfieldInfo* bitfieldIn(const StructInfo& info, Decl* d)
  {
    auto it = info.bitfields.find(d);
    return it == info.bitfields.end() ? nullptr : &it->second;
  }
  // A lambda's closure: one struct per lambda, fields for the captures, one function. It does not
  // change once laid out: other emitters copy it without the lock (Shared::finalClosures, closureFor's
  // callers). What is known only later, the function the lambda is written in, is a ClosureWriter.
  struct ClosureInfo
  {
    std::string structName;
    std::vector<Decl*> captured;       // enclosing local per field (null for `this`)
    std::vector<bool> byRef;
    std::vector<Expr*> inits;          // init-captures: the initializer (null for other captures)
    std::vector<const Ty*> fieldTypes; // pointee type for references and `this`
    std::vector<uint32_t> bindings;    // a structured binding's name (captured is its hidden object), UINT32_MAX for any other capture
    std::string functionName;
    bool laidOut = false; // captures and fields known (read where the lambda is written); before that only the struct's name exists
    std::string failure;  // why laying it out failed where it is written, if it did
  };
  // The function a lambda is written in, recorded whenever it comes, often after the closure was laid
  // out and read by others: by its closure's writer (makeClosure) or by its call operator's emission on
  // demand. Kept apart from the ClosureInfo and read under the lock: written into the laid-out closure,
  // a copy racing with the write read a string being assigned (kpp crashed, 0xC0000005, in 6 to 8 of
  // 160 builds of 1,000 functions returning lambdas, each called beside it; tests\concurrency.ps1).
  struct ClosureWriter
  {
    const Ty* enclosingClass = nullptr; // what `this` in its body points to
    std::string writtenIn;              // the function it is written in (its call operator's requester in diagnostics)
    std::string writtenInContext;       // and as diagnostics show it (`a lambda in main(), line 12`)
  };

  struct Instantiation
  {
    FunctionDecl* f;
    TemplateArgs args;
    const Ty* classTy;
    unsigned module;         // the module of the function that asked for it
    std::string requestedBy; // the name of that function, for diagnostics
    std::string name;        // the LLVM name to define it under (the declaration's), or empty
    unsigned depth;          // instantiations in the chain that asked for it, itself included
    std::string requestedByContext; // requestedBy as diagnostics show it (Emitter::currentContext)
  };

#include "SharedText.hpp"

#include "SpinMutex.hpp"
#include "SpinRecursiveMutex.hpp"

#include "NameSet.hpp"

#include "FinalAnswers.hpp"
  // A defined global as its uses name it (Emitter::globalFor): its IR name, and the function that
  // builds a thread_local one per thread (empty for any other global).
  struct GlobalAnswer
  {
    std::string name;
    std::string threadLocalInit;
  };
  // A global's key (the variable, and the class instance of a template's static member), hashed
  // for FinalAnswers' shards, which take the top bits.
  struct GlobalKeyHash
  {
    size_t operator()(const std::pair<Decl*, const Ty*>& k) const
    {
      uint64_t h = uint64_t(uintptr_t(k.first)) * 0x9E3779B97F4A7C15ull ^ uint64_t(uintptr_t(k.second));
      h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull; // (splitmix64's finalizer)
      h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
      return size_t(h ^ (h >> 31));
    }
  };

  // A request for an instantiation: by which function (its IR name), and the instantiation depth
  // through it (the requested one included).
  struct Request
  {
    std::string by;
    unsigned depth = 0;
    std::string byContext; // by, as diagnostics show it (Emitter::currentContext)
  };

  struct Emitter;

  // What the emitters of one program share, under `mutex` (recursive: laying out a class lays out
  // the classes of its fields): class layouts, closures, globals, the instantiations asked for,
  // and the output. Everything else an emitter has is its own (the function it is emitting, and
  // caches of what only depends on the program).
  struct Shared
  {
    // The startup functions of the globals built at startup, each with where its global is
    // defined (ctorsText runs them in that order).
    // (An explicit instantiation's static data member is at the `template class` line, then in
    // its definitions' order: member is the definition's token there, 0 for any other global.)
    struct Startup { uint32_t file; uint32_t token; uint32_t member; std::string function; };
    // What a global's startup function does: builds it and registers its destruction; builds it alone
    // (one built before everything else, whose destruction is registered in its place: ExitOnly); or
    // registers it alone (one of constant data).
    enum class GlobalBuild { BuildAndExit, BuildOnly, ExitOnly };
    // How each global's constructor (`kpp.init.<name>`) was emitted (Emitter::emitGlobalConstructor's
    // arguments), by its name, and the names by the global: a server's build emits one again when
    // its initializer read an edited function (rebuildLLVM's changedGlobals).
    struct GlobalInit
    {
      VariableDecl* v;
      std::string name;
      const Ty* t;
      Expr* init;
      Node* at;
      bool perThread;
      VariableDecl* definition;
      std::optional<Startup> startupAt;
      GlobalBuild build;
    };
    // A definition in a module (a function's, a helper's in its home module) with the names its text
    // uses, found once on the thread that emitted it: module assembly reads these instead of scanning
    // the module's text again (and they are what an incremental build will keep per function).
    struct Definition
    {
      std::string name;  // the IR name it defines (empty for a helper placed in its home module)
      std::string owner; // the function whose emission added it: itself, or the function a lambda is written in
      std::string text;
      std::vector<std::pair<uint32_t, uint32_t>> names; // the `@names` in the text (offset, length), in order, as moduleText scans them
      std::vector<std::pair<uint32_t, uint32_t>> types; // the `%struct...`/`%closure...` it names
      bool counted = false; // one of the build's functions (result.functions): not a catch filter, nor a global's constructor
      // The definitions (as written: an instance's template's) of the trivial callees whose bodies
      // this function's emission holds, its lambdas' included (Emitter::noteInlined): an edit of one
      // of those bodies changes this text, and rebuildLLVM emits it again as `emitted` says.
      std::vector<const FunctionDecl*> inlined;
      // How it was emitted: set with inlined, and for every instance's own definition (an edit of its
      // template's body, as written, emits it again).
      std::unique_ptr<Instantiation> emitted;
      // The class types whose layouts (or answers computed from them: size, destruction, bitwise
      // copy) its emission and its lambdas' used (Emitter::noteLayout), sorted: a changed layout
      // emits it again (rebuildLLVM's changedLayouts).
      std::vector<const Ty*> layouts;
      // A lambda's call operator its first caller emitted (Emitter::emitCallOperatorOnDemand): its
      // closure and call types, to emit it again on its own once its writer's edit dropped it.
      const Ty* closure = nullptr;
      const Ty* callType = nullptr;
    };
    static Definition definitionOf(std::string text);
    bool programOwned(const std::string& owner) const { return programFunctions.count(owner) != 0; }
    unsigned outputModules() const { return splitPieces ? 2 * moduleCount : moduleCount; }
    // Step 4's P2: per module, its program piece as handed to the stream at the barrier (PieceStream),
    // by its size and hash, for the end's check that it is the end's text.
    struct StreamedPiece
    {
      bool handed = false;
      uint64_t hash = 0;
      size_t size = 0;
    };
    // A debug build's linkonce_odr globals of the shared declarations (constants, string literals,
    // static data), each defined in one module, its home (a hash of its name), and declared
    // `external` in the other modules that name it (prepareModuleTexts, moduleText). Every module
    // defined every one before: 1.57 million definitions of the game's 64,779, a quarter of its IR,
    // compiled 128 times and thrown away by the linker 127 times.
    struct HomedGlobal
    {
      unsigned home;
      std::string declaration; // `@name = external [thread_local] global|constant TYPE[, align N]`
    };
    // The module emission puts item x in (x: a file's index, a name's hash): one of the modules after
    // the importable ones.
    unsigned emissionModule(size_t x) const { return importableModules + unsigned(x % (moduleCount - importableModules)); }
    // The module that defines a function: where it was placed, else where it was emitted; null if none.
    const unsigned* moduleDefining(const std::string& name) const
    {
      if (auto it = placedIn.find(name); it != placedIn.end())
        return &it->second;
      auto it = definedIn.find(name);
      return it == definedIn.end() ? nullptr : &it->second;
    }
    // A module declaration a function's emission adds every time it runs (a static local and its
    // guard, a static InitializerList's array), not once at a first use: it belongs to that function
    // (owner), and an incremental build replaces it with the function. Joins the module's
    // declarations when they are finished.
    struct OwnedEntry
    {
      std::string owner;
      std::string text;
    };
    struct TypeLine
    {
      std::string_view line;      // `%struct.<hash> = type {...}`, in the finished `decls`
      std::vector<uint32_t> uses; // the type lines it names
    };
    // --debug-info's types, each made once per build by its key (Emitter::debugType: its IR spelling's
    // hash): its metadata text, numbered from 0 as a function's, naming other types by placeholders
    // (`!T` and a key's 16 hex digits) that module assembly resolves (appendDebugTypes). A compile
    // server's build (rebuildLLVM, a generation of its own) makes a type's text again when it first
    // names the type, an edited enum's enumerators then the new ones; the definitions it kept name the
    // types their own build made.
    // A class's text is its declaration (DIFlagFwdDecl, by its identifier); its definition, made once
    // the emission is done (Emitter::homeDebugClasses), is in its home's text alone (debugHomes).
    struct DebugTypeText
    {
      std::string text;
      uint32_t generation = 0;
      const Ty* type = nullptr;       // a class's, as its build named it
      std::string definition;         // a class's members, bases and vtable pointer
      uint32_t definitionGeneration = UINT32_MAX;
    };

    explicit Shared(CodegenResult& result) : result(result) {}
    // The per-module parts, for n modules.
    void setModules(unsigned n)
    {
      moduleCount = std::max(1u, n);
      bodies.resize(moduleCount);
      moduleDecls.resize(moduleCount);
      vtablesIn.resize(moduleCount);
      for (LiteralShard& s : literals)
        s.modules.resize(moduleCount);
      definitions.resize(moduleCount);
      ownedEntries.resize(moduleCount);
      keptModuleEntries.resize(moduleCount);
      placedHere.resize(moduleCount);
      placedDecls.resize(moduleCount);
      movedOut.resize(moduleCount);
      latePieces.resize(moduleCount);
    }

    SpinRecursiveMutex mutex;
    CodegenResult& result;
    unsigned moduleCount = 1;
    bool debug = true; // config::debug: asserts are compiled in
    SharedText decls;  // struct types, globals, string constants, declares
    SharedText body;   // function definitions (single module), or unused when split
    SharedText shared; // definitions every module carries (linkonce_odr): enum str(), the assert handler
    std::vector<SharedText> bodies;                         // per module
    std::vector<SharedText> moduleDecls;                    // per module: its string constants and vtables
    std::vector<Startup> startups;
    std::unordered_set<std::string> startupFunctions; // the functions startups names (a server's build emits one again)
    std::unordered_map<std::string, GlobalInit> globalInits;
    std::unordered_map<const Decl*, std::vector<std::string>> globalInitsOf;
    // A global defined with no constructor (constant data, globalForOnce), by its IR name: the declaration
    // its use named, its class (a template's instance, else null) and the bodies its evaluation ran (as
    // written: noteInlined's). A server's build checks it again when one of them changed
    // (rebuildModules: keptConstantGlobal), as no walk records that it read a constructor.
    struct ConstantGlobal
    {
      VariableDecl* v = nullptr;
      const Ty* classTy = nullptr;
      std::vector<const FunctionDecl*> ran;
    };
    std::unordered_map<std::string, ConstantGlobal> constantGlobals;
    std::unordered_map<std::string, std::string> threadLocalInits; // a thread_local global built per thread -> the function building it
    std::vector<std::unordered_set<std::string>> vtablesIn; // per module: the vtables it already has
    std::unordered_map<ParsedFile*, unsigned> fileModule;   // file -> module
    std::unordered_map<std::string, unsigned> definedIn;    // function name -> module
    std::unordered_map<std::string, std::string> declareLines; // function name -> its declare line
    std::unordered_map<std::string, const Ty*> irTypeNames; // irTypeName(): a struct type's IR name -> its type
    SpinMutex irTypeNamesMutex;                              // (irTypeNames' own: taken without the shared lock)
    // String literals (constantName): each name's contents, one name per contents for the whole
    // program; the names the shared declarations define, and those each module defines itself. In
    // shards by the name, a lock each (the emitters queued on one lock for them all).
    struct LiteralShard
    {
      SpinMutex mutex;
      std::unordered_map<std::string, std::string> contents;
      std::unordered_set<std::string> shared;
      std::vector<std::unordered_set<std::string>> modules;
    };
    LiteralShard literals[64];
    LiteralShard& literalShard(const std::string& name) { return literals[(std::hash<std::string>()(name) >> 58) % std::size(literals)]; }
    const LiteralShard& literalShard(const std::string& name) const { return literals[(std::hash<std::string>()(name) >> 58) % std::size(literals)]; }
    // The layouts, published whole (vtables included) by the emitter that built one first: each
    // builds a layout without the shared lock, as its own copy (Emitter::structFor), and the first
    // copy published is the class's. Under structsMutex, held only to look up and publish.
    std::unordered_map<const Ty*, StructInfo> structs;
    std::unordered_map<const Ty*, StructInfo*> structsByType; // any spelling of a class type (const, a reference, a declaration) -> its layout
    SpinMutex structsMutex;
    SpinMutex descriptorsMutex; // a published layout's typeInfoName and descriptorText (descriptorFor makes them on demand)
    std::unordered_map<Decl*, std::string> globals;
    // A static local that code its function's emission does not write names too (Emitter::staticLocal):
    // its definition by name, which every module naming it has a copy of; a build of one module
    // defines it once.
    std::unordered_map<std::string, std::string> sharedStatics;
    std::unordered_set<std::string> sharedStaticsDefined;
    std::map<std::pair<Decl*, const Ty*>, std::string> failedGlobals; // globals whose definition failed -> what their uses report
    // Globals whose initializer an emitter evaluates before taking the lock (Emitter::globalFor):
    // another emitter that needs one waits for it to be defined instead of evaluating it again.
    std::mutex claimsMutex;
    std::condition_variable claimsChanged;
    std::set<std::pair<Decl*, const Ty*>> evaluating;
    std::atomic<size_t> failuresRemembered{0}; // failed layouts, closures and globals, whose uses fail with them (no session kept)
    bool dyncastEmitted = false;                        // the @kpp.dyncast helper is in `decls`
    std::unordered_map<std::string, std::string> instanceGlobals; // static members of class template instantiations, by name
    NameSet declaredNames;                               // IR names with a declare/define line
    std::atomic<size_t> functions{0};                    // functions defined (result.functions once done)
    std::unordered_map<const Ty*, ClosureInfo> closures; // keyed by the lambda type (one per enclosing instantiation)
    std::unordered_map<const Ty*, ClosureWriter> closureWriters; // by the (unqualified) lambda type
    std::unordered_set<const Ty*> closuresEmitted;       // lambdas whose call operator is emitted (or being emitted)
    std::unordered_set<std::string> lambdaInstances;     // call operators of generic lambdas emitted so far, by IR name
    NameSet requested;                                   // instantiated names queued or emitted
    FinalAnswers<const Ty*, StructInfo*> finalLayouts;   // complete (or failed) layouts, by any spelling of the class type
    FinalAnswers<const Ty*, std::pair<uint64_t, uint64_t>> finalSizes; // classSizeAndAlign
    FinalAnswers<const Ty*, bool> finalDestruction;      // needsDestruction of a class
    FinalAnswers<const Ty*, ClosureInfo*> finalClosures; // laid-out closures (closureFor)
    // Globals defined for good (globalFor; never dropped, as `globals` keeps their names).
    FinalAnswers<std::pair<Decl*, const Ty*>, GlobalAnswer, GlobalKeyHash> finalGlobals;
    bool mallocDeclared = false;
    std::atomic<bool> usesAssert{false};                 // some assert was emitted: the module needs kpp_assert_failed
    std::atomic<bool> usesProgramExit{false};            // an object of static storage is destroyed at the program's end: kpp_atexit
    std::atomic<bool> usesThreadExit{false};             // a thread_local is destroyed at its thread's exit: kpp_thread_atexit
    std::atomic<bool> usesExceptions{false};             // the program needs the exception runtime
    std::atomic<bool> usesNoexceptFilter{false};         // Windows: a noexcept function's catchpad names kpp.eh.filter.noexcept
    std::unordered_map<std::string, bool> emittedShared; // typeinfo and destructor thunks emitted
    std::string exceptionTypeinfo;                       // typeinfo of the library's Exception, once seen
    std::string exceptionMessageOffset = "0"; // where the message is in an Exception, after its vtable pointer
    uint64_t exceptionMessageMark = 0;                   // the byte of Exception's message (a String) that says heap or small
    std::vector<std::vector<Definition>> definitions;    // per module, sorted by text at the end (as SharedText sorts)
    // Streaming's barrier (docs/codegen-speed.md, step 4): a debug build's module is cut into a
    // program piece, which can be assembled once level 0 is emitted, and an instance piece with the
    // rest (splitPieces). A program piece declares the functions its definitions name that are defined only later
    // (instances, destructor helpers, lambdas' call operators emitted on demand, the entry point)
    // from requestDeclarations: each one's declaration as its signature gives it when it is first
    // asked for, never as a call site spells it (taking a function's address names no type).
    // barrierDefined and barrierCounts are level 0's barrier, for the check at the end that every
    // such name has a declaration and that it is its definition's (checkRequestDeclarations).
    std::unordered_map<std::string, std::string> requestDeclarations; // under definitionsMutex
    std::unordered_set<std::string> barrierDefined; // every name a text defines at the barrier (only with checkBarrier)
    std::vector<size_t> barrierCounts; // per module: its definitions at level 0's barrier
    bool barrierTaken = false;
    // KPP_CHECK_BARRIER (set by run_tests and spec until program pieces are assembled): the check at
    // the end that every name level 0 uses and only a later text defines has a barrier declaration,
    // and that it is its definition's (B's review: test-only until the pieces rely on it).
    bool checkBarrier = getenv("KPP_CHECK_BARRIER") != nullptr;
    // The pieces: a debug build of several modules (the builds that home their globals) has each
    // module's text in two, the output's modules being the program pieces (0..M-1), then the instance
    // pieces (M..2M-1). A module's program piece holds what the functions the program writes (level
    // 0, by IR name in programFunctions) own (Definition::owner, OwnedEntry::owner): their
    // definitions, their lambdas' call operators, their catch filters, their static locals. It is
    // assembled from texts that do not change once written (programPieceText), so it is one text at
    // level 0's barrier, at the end and in a server's build. The instance piece holds the rest
    // (moduleText's instancePiece) and declares what the program piece defines.
    bool splitPieces = false;
    std::unordered_set<std::string> programFunctions; // (rebuildLLVM: less the removed, with the added)
    std::vector<size_t> barrierOwned; // per module: its owned entries at level 0's barrier
    // KPP_CHECK_BARRIER: each module's program piece as level 0's barrier assembles it, compared with
    // the one the end assembles; and whether the barrier's is not the whole piece (late): a definition
    // or owned entry of a program function came after the barrier (a lambda's call operator a caller of
    // a later level emitted; none in the game), or a type it names had no line yet (a closure a program
    // function holds, laid out by the lambda's writer at a later level: FusionGeneratorPrototype::
    // addToDescription's, in 3 of the game's 128 modules).
    std::vector<std::string> barrierPieces;
    std::vector<char> latePieces;
    std::vector<StreamedPiece> streamedPieces;
    // A release build's modules [0, importableModules) hold the short definitions emitted in the
    // others (Emitter::placeShortDefinitions); 0 in a debug build, whose modules are as emitted.
    unsigned importableModules = 0;
    bool homeGlobals = false;
    std::unordered_map<std::string_view, HomedGlobal> homedGlobals; // by `@name`, a view into decls
    std::unordered_map<std::string_view, unsigned> homedComdats;     // `$name` -> its global's home
    std::vector<std::vector<std::string_view>> homeNames;            // per module: the names its homed globals' initializers use
    std::vector<std::string_view> namedByShared;                     // the names the shared lines every module keeps use
    std::vector<std::vector<const Definition*>> placedHere; // per module: the definitions placed in it
    std::vector<std::string> placedDecls;                    // per module: what they name from where they were emitted
    std::unordered_map<std::string, unsigned> placedIn;      // a placed definition's name -> its module
    std::unordered_set<const Definition*> placedAway;        // the definitions placed in another module than their own
    std::vector<std::vector<std::string_view>> movedOut;     // per module: its declarations' lines placed in another (static locals)
    // Held to add a definition (definitions, definedIn, declareLines: only added to while functions
    // are emitted, read once they all are), and to name a constant (constantName).
    SpinMutex definitionsMutex;
    std::vector<std::vector<OwnedEntry>> ownedEntries;   // per module
    std::vector<std::vector<std::string>> keptModuleEntries; // KPP_CHECK_REBUILD: the module declarations, before they were finished
    std::unordered_set<std::string> sharedDeclared;      // functions with a declare line in `decls`, found once
    std::set<std::string> sharedReferences;              // program functions `decls` names (sorted: the same declares in every run)
    std::vector<TypeLine> typeLines;                     // the type definitions, in `decls` order (prepareModuleTexts)
    std::unordered_map<std::string_view, uint32_t> typeIndex; // a type's name -> its line
    std::map<std::string, FunctionDecl*> undefinedUses;  // kpp functions used whose definition was not found (only declared)
    std::map<std::string, const Ty*> closureCalls;       // call operators of (non-generic) lambdas called, by name -> the lambda's type
    bool stubMissing = false;                            // --stub-missing: what is left out becomes stubs that abort (generateLLVM)
    bool despiteErrors = false;                          // --keep-going: the IR assembled despite errors
    Target target;                                       // the platform the IR is for (--target)
    // A session kept across errors (generate): the program functions whose emission raised them, by
    // IR name (emitted again by every later build), and the modules not assembled since they changed.
    std::unordered_map<std::string, FunctionDecl*> erroring;
    std::vector<bool> unassembled;
    bool inlineTrivial = true;                           // trivial callees emitted in their callers (--no-inline: false)
    bool debugInfo = false;                              // --debug-info: line tables for the debugger (Emitter::debugLines)
    bool fullDebugInfo = false;                          // --debug-info unoptimized: variables and types too (Emitter::debugVariable)
    // Those types, in shards by key, each under a lock of its own: under the shared lock, which every
    // emitter's other work takes too, 32 emitters asking for types and member functions at once spun or
    // slept on it in 60% of homeDebugClasses' samples in the game's debug build. Each shard also has the
    // keys of the classes as a generation takes their entries (Emitter::debugType), in the order they
    // come, which homeDebugClasses takes level by level (it scanned every entry once a level: about 0.1 s
    // apiece, every core but one idle). Read without the locks once the emission and homeDebugClasses
    // are done (findDebugType: appendDebugTypes).
    struct DebugTypeShard
    {
      SpinMutex mutex;
      std::unordered_map<uint64_t, DebugTypeText> types;
      std::vector<uint64_t> classesNamed;
    };
    static constexpr size_t debugTypeShardCount = 64;
    DebugTypeShard debugTypeShards[debugTypeShardCount];
    DebugTypeShard& debugTypeShard(uint64_t key) { return debugTypeShards[(key * 0x9E3779B97F4A7C15ull) >> 58]; }
    const DebugTypeText* findDebugType(uint64_t key) const
    {
      const DebugTypeShard& shard = debugTypeShards[(key * 0x9E3779B97F4A7C15ull) >> 58];
      auto it = shard.types.find(key);
      return it == shard.types.end() ? nullptr : &it->second;
    }
    // The classes that have a definition (homeDebugClasses alone changes one), kept with the entries
    // across a compile server's builds.
    std::unordered_set<uint64_t> debugClassesDefined;
    // --debug-info's globals (Emitter::debugGlobal), by their IR names' hash: the metadata of each,
    // which the module whose text defines it gets (appendDebugTypes).
    std::unordered_map<uint64_t, std::string> debugGlobals;
    uint32_t debugGeneration = 0;
    std::vector<std::vector<uint64_t>> debugHomes; // per module, the classes it defines (their keys, in order)
    std::vector<DebugClass> debugClasses;          // those classes with their names (CodegenResult::debugClasses), made with debugHomes
    // Work after the emission on every emitter of the build at once, each on a thread of its own
    // (the caller's thread runs the first): homeDebugClasses' levels. Set by the session that made
    // the emitters, which outlive the build with this state; unset, the caller does the work alone.
    std::function<void(const std::function<void(Emitter&)>& work)> onEmitters;
    // --debug-info: the program's main and wmain (their IR names), each with its DIFile and line, where
    // the entry that calls it is in the debug information (Emitter::emitEntry).
    std::unordered_map<std::string, std::pair<std::string, size_t>> entryDebug;
    std::unordered_map<std::string, std::string> stubMessages; // a stubbed function -> "qualified name at file:line"
  };

  // --debug-info: a definition's own metadata ids (Emitter::debugId: `!` and ten digits, after a
  // space or at a line's start) become base + its own id, in place. Returns how many it used.
  inline uint32_t renumberDebugIds(char* text, size_t size, uint32_t base)
  {
    uint32_t used = 0;
    for (char* at = (char*)memchr(text, '!', size); at; at = (char*)memchr(at + 1, '!', text + size - at - 1))
    {
      if (at + 11 > text + size || (at > text && at[-1] != ' ' && at[-1] != '\n') || (at + 11 < text + size && isdigit((unsigned char)at[11])))
        continue;
      uint32_t local = 0;
      bool digits = true;
      for (int k = 1; k <= 10 && digits; ++k)
        if (isdigit((unsigned char)at[k]))
          local = local * 10 + uint32_t(at[k] - '0');
        else
          digits = false;
      if (!digits)
        continue;
      char buffer[16];
      snprintf(buffer, sizeof buffer, "%010u", base + local);
      memcpy(at + 1, buffer, 10);
      used = std::max(used, local + 1);
    }
    return used;
  }

  // --debug-info with variables: the key of a placeholder at text[at] (`!` and its letter, `T` a
  // type's, `G` a global's, and 16 hex digits, after a blank, before a delimiter:
  // Emitter::debugTypePlaceholder, debugGlobal), or nothing.
  inline std::optional<uint64_t> debugTypeKeyAt(std::string_view text, size_t at, char letter = 'T')
  {
    if (at == 0 || text[at - 1] != ' ' || at + 18 > text.size() || text[at] != '!' || text[at + 1] != letter ||
        (at + 18 < text.size() && !strchr(",)} \n", text[at + 18])))
      return std::nullopt;
    uint64_t key = 0;
    for (size_t i = at + 2; i < at + 18; ++i)
    {
      char c = text[i];
      if (c >= '0' && c <= '9')
        key = key * 16 + uint64_t(c - '0');
      else if (c >= 'a' && c <= 'f')
        key = key * 16 + uint64_t(c - 'a' + 10);
      else
        return std::nullopt;
    }
    return key;
  }

  // --debug-info with variables: the types a module's definitions (out from `from` on) name by
  // placeholder (Emitter::debugType), with the types those name, and the classes homed here (their
  // definitions: homed, Shared::debugHomes; every other class its declaration), each once: appended
  // to out with ids from nextDebugId on, in their keys' order (the text does not depend on the order
  // of emission), and every placeholder in out from `from` on replaced in place by its type's id,
  // padded with blanks to the placeholder's width. Before them the globals the module's text defines
  // (a definition's `!dbg` placeholder, `!G` and a key, anywhere in out: Emitter::debugGlobal), each
  // one's metadata once. Returns the ids of the list of the classes defined here, the compile unit's
  // retainedTypes (LLVM's CodeView writer emits a type only when something in its module names it,
  // and a class's home may not), and of the list of those globals, its globals (0: none), for
  // debugModuleTailFor.
  struct DebugLists
  {
    uint32_t retained = 0, globals = 0;
  };
  inline DebugLists appendDebugTypes(const Shared& sh, std::string& out, size_t from, uint32_t& nextDebugId, const std::vector<uint64_t>* homed = nullptr)
  {
    DebugLists lists;
    auto listOf = [&](const std::vector<uint32_t>& ids) -> uint32_t
    {
      if (ids.empty())
        return 0;
      char head[24];
      snprintf(head, sizeof head, "!%010u = !{", nextDebugId);
      std::string line = head;
      for (size_t i = 0; i < ids.size(); ++i)
      {
        char id[16];
        snprintf(id, sizeof id, "!%010u", ids[i]);
        line += std::string(i ? ", " : " ") + id;
      }
      out += line + "}\n";
      return nextDebugId++;
    };
    std::unordered_map<uint64_t, uint32_t> globalIds;
    std::vector<uint32_t> globalList;
    size_t globalsEnd = out.size();
    for (size_t at = out.find("!G"); at != std::string::npos && at < globalsEnd; at = out.find("!G", at + 2))
      if (std::optional<uint64_t> key = debugTypeKeyAt(out, at, 'G'); key && !globalIds.count(*key))
        if (auto it = sh.debugGlobals.find(*key); it != sh.debugGlobals.end() && !it->second.empty())
        {
          globalIds[*key] = nextDebugId;
          globalList.push_back(nextDebugId);
          size_t start = out.size();
          out += it->second;
          nextDebugId += renumberDebugIds(out.data() + start, it->second.size(), nextDebugId);
        }
    for (size_t at = out.find("!G"); at != std::string::npos && at < globalsEnd; at = out.find("!G", at + 2))
      if (std::optional<uint64_t> key = debugTypeKeyAt(out, at, 'G'))
      {
        if (auto it = globalIds.find(*key); it != globalIds.end())
        {
          char text[24];
          snprintf(text, sizeof text, "!%010u       ", it->second);
          memcpy(&out[at], text, 18);
        }
        else if (at >= 7 && out.compare(at - 7, 7, ", !dbg ") == 0)
          memset(&out[at - 7], ' ', 25); // (a global no emitter described: no attachment)
      }
    std::set<uint64_t> keys;
    std::vector<uint64_t> work;
    auto defined = [&](uint64_t key) { return homed && std::binary_search(homed->begin(), homed->end(), key); };
    auto textOf = [&](const Shared::DebugTypeText& t, uint64_t key) -> const std::string& { return defined(key) && !t.definition.empty() ? t.definition : t.text; };
    auto collect = [&](std::string_view text)
    {
      for (size_t at = text.find("!T"); at != std::string_view::npos; at = text.find("!T", at + 2))
        if (std::optional<uint64_t> key = debugTypeKeyAt(text, at); key && keys.insert(*key).second)
          work.push_back(*key);
    };
    collect(std::string_view(out).substr(from));
    if (homed)
      for (uint64_t key : *homed)
        if (keys.insert(key).second)
          work.push_back(key);
    while (!work.empty())
    {
      uint64_t key = work.back();
      work.pop_back();
      if (const Shared::DebugTypeText* type = sh.findDebugType(key))
        collect(textOf(*type, key));
    }
    std::unordered_map<uint64_t, uint32_t> ids;
    for (uint64_t key : keys)
      if (const Shared::DebugTypeText* type = sh.findDebugType(key); type && !textOf(*type, key).empty())
      {
        const std::string& text = textOf(*type, key);
        ids[key] = nextDebugId;
        size_t at = out.size();
        out += text;
        nextDebugId += renumberDebugIds(out.data() + at, text.size(), nextDebugId);
      }
    for (size_t at = out.find("!T", from); at != std::string::npos; at = out.find("!T", at + 2))
      if (std::optional<uint64_t> key = debugTypeKeyAt(out, at))
      {
        char text[24];
        auto it = ids.find(*key);
        if (it != ids.end())
          snprintf(text, sizeof text, "!%010u       ", it->second);
        else
          snprintf(text, sizeof text, "%-18s", "null"); // (a type no emitter made: none)
        memcpy(&out[at], text, 18);
      }
    std::vector<uint32_t> retained;
    for (uint64_t key : keys)
      if (auto it = ids.find(key); it != ids.end() && defined(key) && !sh.findDebugType(key)->definition.empty())
        retained.push_back(it->second);
    lists.retained = listOf(retained);
    lists.globals = listOf(globalList);
    return lists;
  }

  // --debug-info: what every module's line tables refer to (the definitions' `unit: !0`, `type: !2`).
  // The functions' own ids start after these.
  inline const char* const debugModuleTail = "!llvm.dbg.cu = !{!0}\n"
                                             "!llvm.module.flags = !{!3, !4}\n"
                                             "!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, producer: \"kpp\", "
                                             "isOptimized: false, runtimeVersion: 0, emissionKind: LineTablesOnly)\n"
                                             "!1 = !DIFile(filename: \"kpp\", directory: \"\")\n"
                                             "!2 = !DISubroutineType(types: !{})\n"
                                             "!3 = !{i32 2, !\"CodeView\", i32 1}\n"
                                             "!4 = !{i32 2, !\"Debug Info Version\", i32 3}\n";
  // The tail for a target: DWARF instead of CodeView off Windows. The producer (CodeView's S_COMPILE3,
  // DWARF's DW_AT_producer) is kpp with its release version, without the commit and "+dirty": the
  // objects of one version are the same (docs/build.md, "Versions"). An unoptimized build's has
  // variables and types (Shared::fullDebugInfo): FullDebug.
  inline std::string debugModuleTailFor(const Target& target, bool full, DebugLists lists = {})
  {
    std::string tail = debugModuleTail;
    if (full)
    {
      const std::string kind = "emissionKind: LineTablesOnly";
      char types[40] = "", globals[40] = "";
      if (lists.retained)
        snprintf(types, sizeof types, ", retainedTypes: !%010u", lists.retained);
      if (lists.globals)
        snprintf(globals, sizeof globals, ", globals: !%010u", lists.globals);
      tail.replace(tail.find(kind), kind.size(), std::string("emissionKind: FullDebug") + types + globals);
    }
    std::string version = kppVersion();
    version.erase(std::min(version.find('+'), version.size()));
    const std::string producer = "producer: \"kpp\"";
    tail.replace(tail.find(producer), producer.size(), "producer: \"kpp " + version + "\"");
    if (!target.windows())
    {
      const std::string codeView = "!3 = !{i32 2, !\"CodeView\", i32 1}";
      tail.replace(tail.find(codeView), codeView.size(), "!3 = !{i32 7, !\"Dwarf Version\", i32 5}");
    }
    return tail;
  }
  inline const uint32_t debugFirstId = 16;

  // --debug-info: metadata before what names it. To LLVM's parser a `!N` it has not read yet is a temporary
  // node and later a replaceAllUsesWith, and every `!dbg` in a function body was one, each function's metadata
  // following its body and the shared metadata ending the module: 3 to 4% of a module's compile. The order of
  // the text does not change the object. A definition is kept as Emitter::debugLines writes it (its body, then
  // its own metadata), so appended with the metadata first; the shared metadata moves when it is known, after
  // the definitions (appendDebugTypes reads what they name).
  inline void appendDefinitionMetadataFirst(std::string& out, std::string_view definition)
  {
    size_t bodyEnd = definition.rfind("\n}\n");
    bodyEnd = bodyEnd == std::string_view::npos ? 0 : bodyEnd + 3;
    out += definition.substr(bodyEnd);
    out += definition.substr(0, bodyEnd);
  }

  // out's text from metadataAt on, moved to definitionsAt.
  inline void moveMetadataBeforeDefinitions(std::string& out, size_t definitionsAt, size_t metadataAt)
  {
    const std::string metadata = out.substr(metadataAt);
    out.resize(metadataAt);
    out.insert(definitionsAt, metadata);
  }

  // Calls found(offset, length) for each `@name` a text names, as module assembly has always scanned
  // them: every '@', a quoted name to its closing quote, an unquoted one over [A-Za-z0-9-$._].
  template<typename F>
  inline void irNamesIn(std::string_view text, F&& found)
  {
    for (size_t i = text.find('@'); i != std::string_view::npos; i = text.find('@', i + 1))
    {
      size_t end;
      if (i + 1 < text.size() && text[i + 1] == '"')
        end = text.find('"', i + 2) + 1;
      else
      {
        end = i + 1;
        while (end < text.size() && (isalnum((unsigned char)text[end]) || strchr("-$._", text[end])))
          ++end;
      }
      found(i, std::min(end, text.size()) - i);
    }
  }

  // The key of global `name`'s debug metadata (Shared::debugGlobals, its `!G` placeholder): the hash of its
  // IR name without the `@` and the quotes.
  inline uint64_t debugGlobalKey(const std::string& name)
  {
    std::string_view bare = name.rfind("@\"", 0) == 0 ? std::string_view(name).substr(2, name.size() - 3) : std::string_view(name).substr(1);
    uint64_t key = 14695981039346656037ull;
    for (unsigned char c : bare)
      key = (key ^ c) * 1099511628211ull;
    return key;
  }

  inline Shared::Definition Shared::definitionOf(std::string text)
  {
    Definition d;
    d.text = std::move(text);
    std::string_view t = d.text;
    std::unordered_set<std::string_view> seen;
    irNamesIn(t, [&](size_t at, size_t length)
    {
      if (seen.insert(t.substr(at, length)).second)
        d.names.push_back({uint32_t(at), uint32_t(length)});
    });
    std::unordered_set<std::string_view> types;
    for (size_t i = t.find('%'); i != std::string_view::npos; i = t.find('%', i + 1))
    {
      std::string_view rest = t.substr(i + 1);
      if (!rest.starts_with("struct.") && !rest.starts_with("closure."))
        continue;
      size_t end = i + 1;
      while (end < t.size() && (isalnum((unsigned char)t[end]) || t[end] == '.' || t[end] == '_'))
        ++end;
      if (types.insert(t.substr(i, end - i)).second)
        d.types.push_back({uint32_t(i), uint32_t(end - i)});
      i = end - 1;
    }
    return d;
  }
}
