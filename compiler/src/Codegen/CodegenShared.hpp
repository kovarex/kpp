// What codegen's emitters share (Shared) and the records they keep (StructInfo, Vtable, Request,
// ...). Included by Codegen/Emitter.hpp, in namespace codegen (a file of its own since 2026-11-05,
// moved without a change).
#pragma once
#include "codegen.hpp"
#include "ast_walk.hpp"
#include "consteval.hpp"
#include "source.hpp"
#include "Restore.hpp"
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
  struct VtableSlot
  {
    std::string key;                  // name(param types) [const], "~" for the destructor
    FunctionDecl* function = nullptr; // null: pure virtual, or (key "~") the owner's implicit destructor
    const Ty* owner = nullptr;
    std::vector<uint32_t> ownerPath;
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
    const Ty* unit; // the storage unit's (integer) type
  };

  struct StructInfo
  {
    std::string name;                        // %struct.N
    std::vector<const Ty*> fieldTypes;       // In layout order: bases first, then data members.
    std::unordered_map<Decl*, uint32_t> fieldIndex;
    std::vector<std::pair<Decl*, uint32_t>> bases; // base class definition -> field index
    std::vector<Decl*> fieldDecls;           // data members in layout order
    // An empty class (no data members, no vtable, only empty bases) is one byte as an object of
    // its own (`{ i8 }`) and none as a base: such a base's field is `[0 x i8]` (C++'s empty-base
    // optimisation, which the game's size static_asserts count on).
    bool empty = false;
    std::vector<bool> zeroSize;              // per field: an empty base laid out without storage
    bool packed = false;                     // [[packed]]: `<{ ... }>`, every member at alignment 1
    // Its last data member is a flexible array member (`T items[];`, C99's): a `[0 x T]` field, so
    // the size ends where the array starts (rounded up to the alignment); the elements are past it.
    bool flexible = false;
    std::optional<bool> needsDestruction;
    // Virtual functions: a polymorphic class has a vtable pointer at offset 0 (its own, or its
    // primary base's: the first polymorphic one), whose slots extend the primary base's in
    // declaration order, and one in each other polymorphic base's subobject (vtables[1] and on).
    bool polymorphic = false;
    std::vector<Vtable> vtables;              // the primary first
    std::string vtableName;                   // @"vtable.Class", the primary's: which modules have the vtables
    std::string vtableText;                   // the vtables and their thunks, emitted into every module that stores them
    std::string vptrValue;                    // what the primary vtable pointer holds
    std::string typeInfoName;                 // @"typeinfo.Class", the descriptor slot -1 points to (descriptorFor)
    std::string descriptorText;               // its definition, emitted into every module that uses it
    const Ty* primaryBase = nullptr;          // the polymorphic base at offset 0
    bool isUnion = false;                     // all members at offset 0, storage sized for the largest
    std::vector<std::pair<Decl*, uint32_t>> anonymous; // anonymous struct/union members -> field index
    // Bit-field member -> its place inside its storage field: this layout's own, never looked up by
    // the member alone (an instance's members are its template's declarations, and Bits<3> and
    // Bits<5> place `uint32_t a : N` differently).
    std::unordered_map<Decl*, BitfieldInfo> bitfields;
    bool complete = false;                    // every field is laid out (structFor returned it once)
    uint64_t size = 0, align = 0;             // set when the layout completes (align 0: not yet), or by alignPadding
    // The alignment LLVM gives the struct type (its fields' largest); alignas raises the class's own
    // (align) above it: then its padding is written out as fields, and its objects get `align`.
    uint64_t llvmAlign = 1;
    bool overAligned = false;                 // align > llvmAlign
    uint64_t tailPadding = 0;                 // the `[n x i8]` the struct's text ends with (after its byte of its own, when empty)
    bool failed = false;                      // laying it out failed: every use is an error, never the partial layout
    std::string failedWhy;                    // what made it fail, which every use reports
    bool failedShown = true;                  // false: it follows from a checker error (fail's shown), at every use
  };
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
    std::string functionName;
    bool laidOut = false; // captures and fields known (read where the lambda is written); before that only the struct's name exists
    std::string failure;  // why laying it out failed where it is written, if it did
  };
  // The function a lambda is written in, recorded whenever it comes, often after the closure was laid
  // out and read by others: by its closure's writer (makeClosure) or by its call operator's emission on
  // demand. Kept apart from the ClosureInfo and read under the lock: written into the laid-out closure,
  // a copy racing with the write read a string being assigned (kppc crashed, 0xC0000005, in 6 to 8 of
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

  // IR text several emitters append to (the shared declarations, a module's), one whole
  // declaration or definition per append, in whatever order the emitters reach them. Under a lock of
  // its own (only appended to while functions are emitted, read once they all are).
  struct SharedText
  {
    SharedText& operator+=(const std::string& s)
    {
      std::lock_guard lock(*mutex);
      entries.push_back(s);
      return *this;
    }
    // The text the module is assembled from, the same in every run: the entries sorted (LLVM takes
    // declarations and definitions in any order), each freed once copied.
    void finish()
    {
      std::sort(entries.begin(), entries.end());
      size_t total = text.size();
      for (const std::string& e : entries)
        total += e.size();
      text.reserve(total);
      for (std::string& e : entries)
      {
        text += e;
        std::string().swap(e);
      }
      entries.clear();
    }
    // The text from `needle` to the end of the entry holding it (before finish()), or empty.
    std::string fromFirst(const std::string& needle) const
    {
      for (const std::string& e : entries)
        if (size_t at = e.find(needle); at != std::string::npos)
          return e.substr(at);
      return {};
    }

    std::unique_ptr<std::mutex> mutex = std::make_unique<std::mutex>();
    std::vector<std::string> entries;
    std::string text; // the entries, once finish() put them in order
  };

  // A mutex for the short holds that many emitters take (the shared lock, the definitions', the shards
  // below): a waiter spins a while before it sleeps. With std::mutex a waiter slept at once and was
  // woken by the kernel, which takes longer than the holds it waits for (2.3 us on average in the
  // game's codegen), so 32 threads formed a convoy: 66.7 s waited on 3.05 s held, 12 threads blocked on
  // average (docs/codegen-speed.md, step 5). The state is Drepper's: 0 free, 1 held, 2 held with a
  // sleeper (std::atomic's wait and notify: WaitOnAddress).
  class SpinMutex
  {
    std::atomic<uint32_t> state{0};

  public:
    static constexpr int spins = 1000;
    void lock()
    {
      for (int i = 0; i < spins; ++i)
      {
        uint32_t free = 0;
        if (state.load(std::memory_order_relaxed) == 0 && state.compare_exchange_weak(free, 1, std::memory_order_acquire, std::memory_order_relaxed))
          return;
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
#endif
      }
      while (state.exchange(2, std::memory_order_acquire) != 0)
        state.wait(2, std::memory_order_relaxed);
    }
    bool try_lock()
    {
      uint32_t free = 0;
      return state.compare_exchange_strong(free, 1, std::memory_order_acquire, std::memory_order_relaxed);
    }
    void unlock()
    {
      if (state.exchange(0, std::memory_order_release) == 2)
        state.notify_one();
    }
  };
  // std::recursive_mutex's semantics on a SpinMutex: the thread holding it may take it again.
  class SpinRecursiveMutex
  {
  public:
    void lock()
    {
      std::thread::id me = std::this_thread::get_id();
      if (owner.load(std::memory_order_relaxed) != me)
      {
        mutex.lock();
        owner.store(me, std::memory_order_relaxed);
      }
      ++count;
    }
    bool try_lock()
    {
      std::thread::id me = std::this_thread::get_id();
      if (owner.load(std::memory_order_relaxed) != me)
      {
        if (!mutex.try_lock())
          return false;
        owner.store(me, std::memory_order_relaxed);
      }
      ++count;
      return true;
    }
    void unlock()
    {
      if (--count == 0)
      {
        owner.store(std::thread::id(), std::memory_order_relaxed);
        mutex.unlock();
      }
    }
  private:
    SpinMutex mutex;
    std::atomic<std::thread::id> owner{};
    unsigned count = 0;
  };

  // A set of IR names several emitters mark, in shards with a lock each (marking is frequent).
  struct NameSet
  {
    struct Shard
    {
      SpinMutex mutex;
      std::unordered_set<std::string> names;
    };
    // By the hash's top bits: a shard's set buckets by its low bits (a power-of-two bucket count),
    // and picked by those too, its names all fell into one bucket in 64 (11.7% of codegen's time
    // at -j 1 was spent walking those chains).
    Shard& shardOf(const std::string& name) { return shards[(std::hash<std::string>()(name) >> 58) % std::size(shards)]; }
    // Marks name; true for whoever marked it first.
    bool claim(const std::string& name)
    {
      Shard& s = shardOf(name);
      std::lock_guard lock(s.mutex);
      return s.names.insert(name).second;
    }
    bool contains(const std::string& name)
    {
      Shard& s = shardOf(name);
      std::lock_guard lock(s.mutex);
      return s.names.count(name) != 0;
    }
    // Unmarks name: its next claim is a first again.
    void release(const std::string& name)
    {
      Shard& s = shardOf(name);
      std::lock_guard lock(s.mutex);
      s.names.erase(name);
    }

    Shard shards[64];
  };

  // Answers that do not change once given (a complete layout, a class's size, whether it needs
  // destroying, a member's bit-field), published for every emitter in shards with a lock each. Each
  // emitter keeps its own copy of the answers it used; one it has not used yet is looked up here, not
  // under the shared lock: 32 emitters, each taking the shared lock for every layout and answer it
  // used the first time (1.3 million times for the game), waited 103 s of their 172 s on it.
  template<typename K, typename V>
  struct FinalAnswers
  {
    struct Shard
    {
      SpinMutex mutex;
      std::unordered_map<K, V> answers;
    };
    Shard& shardOf(const K& k) { return shards[(std::hash<K>()(k) >> 58) % std::size(shards)]; } // (by the top bits, as NameSet's)
    bool find(const K& k, V& v)
    {
      Shard& s = shardOf(k);
      std::lock_guard lock(s.mutex);
      auto it = s.answers.find(k);
      if (it == s.answers.end())
        return false;
      v = it->second;
      return true;
    }
    void publish(const K& k, const V& v)
    {
      Shard& s = shardOf(k);
      std::lock_guard lock(s.mutex);
      s.answers.emplace(k, v);
    }
    void clear()
    {
      for (Shard& s : shards)
      {
        std::lock_guard lock(s.mutex);
        s.answers.clear();
      }
    }

    Shard shards[64];
  };

  // A request for an instantiation: by which function (its IR name), and the instantiation depth
  // through it (the requested one included).
  struct Request
  {
    std::string by;
    unsigned depth = 0;
    std::string byContext; // by, as diagnostics show it (Emitter::currentContext)
  };

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
      moduleLiterals.resize(moduleCount);
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
    std::unordered_map<std::string, std::string> threadLocalInits; // a thread_local global built per thread -> the function building it
    std::vector<std::unordered_set<std::string>> vtablesIn; // per module: the vtables it already has
    std::unordered_map<ParsedFile*, unsigned> fileModule;   // file -> module
    std::unordered_map<std::string, unsigned> definedIn;    // function name -> module
    std::unordered_map<std::string, std::string> declareLines; // function name -> its declare line
    std::unordered_map<std::string, const Ty*> irTypeNames; // irTypeName(): a struct type's IR name -> its type
    SpinMutex irTypeNamesMutex;                              // (irTypeNames' own: taken without the shared lock)
    // String literals (constantName): each name's contents, one name per contents for the whole
    // program; the names the shared declarations define, and those each module defines itself.
    std::unordered_map<std::string, std::string> literalContents;
    std::unordered_set<std::string> sharedLiterals;
    std::vector<std::unordered_set<std::string>> moduleLiterals;
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
    bool mallocDeclared = false;
    std::atomic<bool> usesAssert{false};                 // some assert was emitted: the module needs kpp_assert_failed
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
    SpinMutex definitionsMutex, constantsMutex;
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
    std::unordered_map<uint64_t, DebugTypeText> debugTypes;
    // --debug-info's globals (Emitter::debugGlobal), by their IR names' hash: the metadata of each,
    // which the module whose text defines it gets (appendDebugTypes).
    std::unordered_map<uint64_t, std::string> debugGlobals;
    uint32_t debugGeneration = 0;
    std::vector<std::vector<uint64_t>> debugHomes; // per module, the classes it defines (their keys, in order)
    std::vector<DebugClass> debugClasses;          // those classes with their names (CodegenResult::debugClasses), made with debugHomes
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
    const std::unordered_map<uint64_t, Shared::DebugTypeText>& debugTypes = sh.debugTypes;
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
      if (auto it = debugTypes.find(key); it != debugTypes.end())
        collect(textOf(it->second, key));
    }
    std::unordered_map<uint64_t, uint32_t> ids;
    for (uint64_t key : keys)
      if (auto it = debugTypes.find(key); it != debugTypes.end() && !textOf(it->second, key).empty())
      {
        const std::string& text = textOf(it->second, key);
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
      if (auto it = ids.find(key); it != ids.end() && defined(key) && !debugTypes.at(key).definition.empty())
        retained.push_back(it->second);
    lists.retained = listOf(retained);
    lists.globals = listOf(globalList);
    return lists;
  }

  // --debug-info: what every module's line tables refer to (the definitions' `unit: !0`, `type: !2`).
  // The functions' own ids start after these.
  inline const char* const debugModuleTail = "!llvm.dbg.cu = !{!0}\n"
                                             "!llvm.module.flags = !{!3, !4}\n"
                                             "!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, producer: \"kppc\", "
                                             "isOptimized: false, runtimeVersion: 0, emissionKind: LineTablesOnly)\n"
                                             "!1 = !DIFile(filename: \"kpp\", directory: \"\")\n"
                                             "!2 = !DISubroutineType(types: !{})\n"
                                             "!3 = !{i32 2, !\"CodeView\", i32 1}\n"
                                             "!4 = !{i32 2, !\"Debug Info Version\", i32 3}\n";
  // The tail for a target: DWARF instead of CodeView off Windows. The producer (CodeView's S_COMPILE3,
  // DWARF's DW_AT_producer) is kppc with kpp's release version, without the commit and "+dirty": the
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
    const std::string producer = "producer: \"kppc\"";
    tail.replace(tail.find(producer), producer.size(), "producer: \"kppc " + version + "\"");
    if (!target.windows())
    {
      const std::string codeView = "!3 = !{i32 2, !\"CodeView\", i32 1}";
      tail.replace(tail.find(codeView), codeView.size(), "!3 = !{i32 7, !\"Dwarf Version\", i32 5}");
    }
    return tail;
  }
  inline const uint32_t debugFirstId = 16;

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
