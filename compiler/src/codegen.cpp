#include "codegen.hpp"
#include "consteval.hpp"
#include <map>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
// SEH wrapper: catches stack overflow that C++ try/catch can't.
// Returns 0 on success, 1 on stack overflow.
static int callWithStackGuard(void(*fn)(void*), void* arg)
{
  __try { fn(arg); return 0; }
  __except(GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
  { _resetstkoflw(); return 1; }
}
#endif

// Object model of the v0 code generator:
// - Scalars are SSA values. Class objects always live in memory; a Value of class type holds the
//   object's address.
// - A class parameter is passed as a pointer to a caller-made copy that the callee owns and
//   destroys. A class return value is constructed into a hidden `sret` slot provided by the
//   caller.
// - Locals (and temporaries) of class types that need destruction are destroyed at the end of
//   their scope, in reverse order; `return`, `break` and `continue` destroy the scopes they leave.
// - Templates: a struct layout exists per instantiation type; an instantiated function is
//   emitted on first use from a worklist, after the type checker re-typed its body for the
//   concrete arguments. All declaration types are read through declType(), which applies the
//   arguments of the instantiation being emitted (or of the class whose metadata is inspected).
static std::string stubText(const std::string& name, const std::string& ret, const std::string& params, const std::string& message);

namespace
{
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
    std::unordered_map<Decl*, std::pair<uint32_t, uint32_t>> bitfields; // member -> (shift, width) inside its storage field
    bool complete = false;                    // every field is laid out (structFor returned it once)
    uint64_t size = 0, align = 0;             // computed on first use once complete (align 0: not yet)
    // The alignment LLVM gives the struct type (its fields' largest); alignas raises the class's own
    // (align) above it: then its padding is written out as fields, and its objects get `align`.
    uint64_t llvmAlign = 1;
    bool overAligned = false;                 // align > llvmAlign
    uint64_t tailPadding = 0;                 // the `[n x i8]` the struct's text ends with (after its byte of its own, when empty)
    bool sizing = false;                      // its size is being computed (a class that contains itself comes back here)
    bool failed = false;                      // laying it out failed: every use is an error, never the partial layout
    std::string failedWhy;                    // what made it fail, which every use reports
  };
  // The field index of a member, 0 when the layout has no such member.
  uint32_t fieldIndexOf(const StructInfo& info, Decl* member)
  {
    auto it = info.fieldIndex.find(member);
    return it == info.fieldIndex.end() ? 0 : it->second;
  }
  struct BitfieldInfo
  {
    uint32_t shift, width;
    const Ty* unit; // the storage unit's (integer) type
  };

  // A lambda's closure: one struct per lambda, fields for the captures, one function.
  struct ClosureInfo
  {
    std::string structName;
    std::vector<Decl*> captured;       // enclosing local per field (null for `this`)
    std::vector<bool> byRef;
    std::vector<Expr*> inits;          // init-captures: the initializer (null for other captures)
    std::vector<const Ty*> fieldTypes; // pointee type for references and `this`
    const Ty* enclosingClass = nullptr;
    std::string functionName;
    std::string writtenIn; // the function the lambda is written in (its call operator's requester in diagnostics)
    bool laidOut = false; // captures and fields known (read where the lambda is written); before that only the struct's name exists
    std::string failure;  // why laying it out failed where it is written, if it did
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
  };

  // IR text several emitters append to (the shared declarations, a module's), one whole
  // declaration or definition per append, in whatever order the emitters reach them.
  struct SharedText
  {
    std::recursive_mutex* mutex = nullptr;
    std::vector<std::string> entries;
    std::string text; // the entries, once finish() put them in order
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
  };

  // A set of IR names several emitters mark, in shards with a lock each (marking is frequent).
  struct NameSet
  {
    struct Shard
    {
      std::mutex mutex;
      std::unordered_set<std::string> names;
    };
    Shard shards[64];
    Shard& shardOf(const std::string& name) { return shards[std::hash<std::string>()(name) % std::size(shards)]; }
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
  };

  // A request for an instantiation: by which function (its IR name), and the instantiation depth
  // through it (the requested one included).
  struct Request
  {
    std::string by;
    unsigned depth = 0;
  };

  // What the emitters of one program share, under `mutex` (recursive: laying out a class lays out
  // the classes of its fields): class layouts, closures, globals, the instantiations asked for,
  // and the output. Everything else an emitter has is its own (the function it is emitting, and
  // caches of what only depends on the program).
  struct Shared
  {
    std::recursive_mutex mutex;
    CodegenResult& result;
    unsigned moduleCount = 1;
    bool debug = true; // config::debug: asserts are compiled in
    SharedText decls;  // struct types, globals, string constants, declares
    SharedText body;   // function definitions (single module), or unused when split
    SharedText shared; // definitions every module carries (linkonce_odr): enum str(), the assert handler
    std::vector<SharedText> bodies;                         // per module
    std::vector<SharedText> moduleDecls;                    // per module: its string constants and vtables
    // The startup functions of the globals built at startup, each with where its global is
    // defined (ctorsText runs them in that order).
    // (An explicit instantiation's static data member is at the `template class` line, then in
    // its definitions' order: member is the definition's token there, 0 for any other global.)
    struct Startup { uint32_t file; uint32_t token; uint32_t member; std::string function; };
    std::vector<Startup> startups;
    std::unordered_map<std::string, std::string> threadLocalInits; // a thread_local global built per thread -> the function building it
    std::vector<std::unordered_set<std::string>> vtablesIn; // per module: the vtables it already has
    std::unordered_map<ParsedFile*, unsigned> fileModule;   // file -> module
    std::unordered_map<std::string, unsigned> definedIn;    // function name -> module
    std::unordered_map<std::string, std::string> declareLines; // function name -> its declare line
    std::unordered_map<std::string, const Ty*> irTypeNames; // irTypeName(): a struct type's IR name -> its type
    std::unordered_map<std::string, std::string> sharedConstants; // constantName(): a private constant's name -> its contents
    std::vector<std::unordered_map<std::string, std::string>> moduleConstants;
    std::unordered_map<const Ty*, StructInfo> structs;
    std::unordered_map<const Ty*, StructInfo*> structsByType; // any spelling of a class type (const, a reference, a declaration) -> its layout
    std::unordered_map<Decl*, std::string> globals;
    std::map<std::pair<Decl*, const Ty*>, std::string> failedGlobals; // globals whose definition failed -> what their uses report
    std::unordered_map<Decl*, BitfieldInfo> bitfieldOf; // bit-field members of every laid-out class
    bool dyncastEmitted = false;                        // the @kpp.dyncast helper is in `decls`
    std::unordered_map<std::string, std::string> instanceGlobals; // static members of class template instantiations, by name
    NameSet declaredNames;                               // IR names with a declare/define line
    std::atomic<size_t> functions{0};                    // functions defined (result.functions once done)
    std::unordered_map<const Ty*, ClosureInfo> closures; // keyed by the lambda type (one per enclosing instantiation)
    std::unordered_set<const Ty*> closuresEmitted;       // lambdas whose call operator is emitted (or being emitted)
    std::unordered_set<std::string> lambdaInstances;     // call operators of generic lambdas emitted so far, by IR name
    NameSet requested;                                   // instantiated names queued or emitted
    bool mallocDeclared = false;
    std::atomic<bool> usesAssert{false};                 // some assert was emitted: the module needs kpp_assert_failed
    std::atomic<bool> usesExceptions{false};             // the program needs the exception runtime
    std::unordered_map<std::string, bool> emittedShared; // typeinfo and destructor thunks emitted
    std::string exceptionTypeinfo;                       // typeinfo of the library's Exception, once seen
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
    };
    static Definition definitionOf(std::string text);
    std::vector<std::vector<Definition>> definitions;    // per module, sorted by text at the end (as SharedText sorts)
    // A module declaration a function's emission adds every time it runs (a static local and its
    // guard, a static InitializerList's array), not once at a first use: it belongs to that function
    // (owner), and an incremental build replaces it with the function. Joins the module's
    // declarations when they are finished.
    struct OwnedEntry
    {
      std::string owner;
      std::string text;
    };
    std::vector<std::vector<OwnedEntry>> ownedEntries;   // per module
    std::vector<std::vector<std::string>> keptModuleEntries; // KPP_CHECK_REBUILD: the module declarations, before they were finished
    std::unordered_set<std::string> sharedDeclared;      // functions with a declare line in `decls`, found once
    std::set<std::string> sharedReferences;              // program functions `decls` names (sorted: the same declares in every run)
    struct TypeLine
    {
      std::string_view line;      // `%struct.<hash> = type {...}`, in the finished `decls`
      std::vector<uint32_t> uses; // the type lines it names
    };
    std::vector<TypeLine> typeLines;                     // the type definitions, in `decls` order (prepareModuleTexts)
    std::unordered_map<std::string_view, uint32_t> typeIndex; // a type's name -> its line
    std::map<std::string, FunctionDecl*> undefinedUses;  // Kpp functions used whose definition was not found (only declared)
    std::map<std::string, const Ty*> closureCalls;       // call operators of (non-generic) lambdas called, by name -> the lambda's type
    bool stubMissing = false;                            // --stub-missing: what is left out becomes stubs that abort (generateLLVM)
    bool inlineTrivial = true;                           // trivial callees emitted in their callers (--no-inline: false)
    std::unordered_map<std::string, std::string> stubMessages; // a stubbed function -> "qualified name at file:line"

    explicit Shared(CodegenResult& result) : result(result)
    {
      decls.mutex = body.mutex = shared.mutex = &mutex;
    }
    // The per-module parts, for n modules.
    void setModules(unsigned n)
    {
      moduleCount = std::max(1u, n);
      bodies.resize(moduleCount);
      moduleDecls.resize(moduleCount);
      for (unsigned k = 0; k < moduleCount; ++k)
        bodies[k].mutex = moduleDecls[k].mutex = &mutex;
      vtablesIn.resize(moduleCount);
      moduleConstants.resize(moduleCount);
      definitions.resize(moduleCount);
      ownedEntries.resize(moduleCount);
      keptModuleEntries.resize(moduleCount);
    }
  };

  // Calls found(offset, length) for each `@name` a text names, as module assembly has always scanned
  // them: every '@', a quoted name to its closing quote, an unquoted one over [A-Za-z0-9-$._].
  template<typename F>
  static void irNamesIn(std::string_view text, F&& found)
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

  Shared::Definition Shared::definitionOf(std::string text)
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

  struct Emitter
  {
    GlobalScope& global;
    TypeTable& types;
    const SymbolTable& symbols;
    Shared& sh;
    // The shared state, under the names the emitter has always used (see Shared). What the
    // emitter changes there it changes under sharedLock().
    CodegenResult& result = sh.result;
    SharedText& decls = sh.decls;
    SharedText& body = sh.body;
    SharedText& shared = sh.shared;
    unsigned& moduleCount = sh.moduleCount;
    std::vector<SharedText>& bodies = sh.bodies;
    std::vector<SharedText>& moduleDecls = sh.moduleDecls;
    std::vector<Shared::Startup>& startups = sh.startups;
    std::unordered_map<std::string, std::string>& threadLocalInits = sh.threadLocalInits;
    std::vector<std::unordered_set<std::string>>& vtablesIn = sh.vtablesIn;
    std::unordered_map<ParsedFile*, unsigned>& fileModule = sh.fileModule;
    std::unordered_map<std::string, unsigned>& definedIn = sh.definedIn;
    std::unordered_map<std::string, std::string>& declareLines = sh.declareLines;
    bool& debug = sh.debug;
    std::atomic<bool>& usesAssert = sh.usesAssert;
    std::unordered_map<std::string, const Ty*>& irTypeNames = sh.irTypeNames;
    std::unordered_map<std::string, std::string>& sharedConstants = sh.sharedConstants;
    std::vector<std::unordered_map<std::string, std::string>>& moduleConstants = sh.moduleConstants;
    std::unordered_map<const Ty*, StructInfo>& structs = sh.structs;
    std::unordered_map<const Ty*, StructInfo*>& structsByType = sh.structsByType;
    std::unordered_map<Decl*, std::string>& globals = sh.globals;
    std::unordered_map<Decl*, BitfieldInfo>& bitfieldOf = sh.bitfieldOf;
    bool& dyncastEmitted = sh.dyncastEmitted;
    std::unordered_map<std::string, std::string>& instanceGlobals = sh.instanceGlobals;
    NameSet& declaredNames = sh.declaredNames;
    std::unordered_map<const Ty*, ClosureInfo>& closures = sh.closures;
    std::unordered_set<std::string>& lambdaInstances = sh.lambdaInstances;
    NameSet& requested = sh.requested;
    // What this emitter asked for: the next level's functions (the driver gathers them).
    std::vector<Instantiation> worklist;
    // Its errors and the functions they left out, gathered once all are emitted.
    std::vector<std::string> errors, failedFunctions;
    std::unordered_map<std::string, std::string> failedAt; // failed function's IR name -> "qualified name at file:line"
    bool& mallocDeclared = sh.mallocDeclared;
    std::atomic<bool>& usesExceptions = sh.usesExceptions;
    std::unordered_map<std::string, bool>& emittedShared = sh.emittedShared;
    std::string& exceptionTypeinfo = sh.exceptionTypeinfo;
    std::unordered_set<std::string>& sharedDeclared = sh.sharedDeclared;
    std::unique_lock<std::recursive_mutex> sharedLock() { return std::unique_lock<std::recursive_mutex>(sh.mutex); }
    void countFunction() { ++sh.functions; }

    unsigned currentModule = 0;  // where the function being emitted goes
    bool emittingShared = false; // string constants go to the shared decls
    std::string fn;      // current function text (after the allocas)
    std::string allocas; // entry-block allocas of the current function
    uint32_t temp = 0;
    uint32_t label = 0;
    bool blockTerminated = false;
    std::string currentBlock = "entry";
    FunctionDecl* currentFunction = nullptr;
    const Ty* currentReturn = nullptr;
    std::string sretAddr; // Hidden return slot when returning a class.
    Decl* constContext = nullptr; // The declaration whose initializer evalConst is folding (a variable template's): the context of the traits in it.
    struct ConstContextScope // restores constContext when an Unsupported leaves the fold
    {
      Emitter& e;
      Decl* saved;
      ConstContextScope(Emitter& e, Decl* d) : e(e), saved(e.constContext) { e.constContext = d; }
      ~ConstContextScope() { e.constContext = saved; }
    };
    ParsedFile* currentFile = nullptr;
    int recursionDepth = 0; // guards rvalue/lvalue/statement against stack overflow

    // Template arguments in effect: of the instantiation being emitted, or temporarily of a class
    // whose constructors/fields are being inspected.
    const TemplateArgs* currentArgs = nullptr;
    const Ty* currentClassTy = nullptr; // Instantiated class of the method being emitted.
    struct ArgsScope
    {
      Emitter& e;
      const TemplateArgs* saved;
      ArgsScope(Emitter& e, const TemplateArgs* args) : e(e), saved(e.currentArgs) { e.currentArgs = args; }
      ~ArgsScope() { e.currentArgs = saved; }
    };

    struct Local
    {
      std::string addr; // Address of the variable (or of the pointer slot for references).
      const Ty* ty;
      bool isRef;
    };
    struct Value
    {
      std::string text;         // IR operand; for class types the object's address
      const Ty* ty;             // Value type (never a reference)
      bool isTemporary = false; // Class temporary owned by the current full-expression
    };
    std::unordered_map<Decl*, Local> locals;
    std::unordered_map<Decl*, std::vector<Local>> packLocals; // one local per element of a parameter pack
    // The `__range` of a range-for that calls free begin(__range) and end(__range): a variable of
    // the compiler's and the name of it passed to the calls (one each per such loop).
    std::deque<VariableDecl> rangeVariables;
    std::deque<NameExpr> rangeNames;
    // The objects a defaulted comparison compares by their own operators (`a.dir < b.dir` for a
    // member with no <=>): a variable of the compiler's for each, names of them, the comparison.
    std::deque<VariableDecl> comparedVariables;
    std::deque<NameExpr> comparedNames;
    std::deque<BinaryExpr> comparisons;
    std::unordered_map<uint32_t, std::string> labelBlocks; // goto labels of the current function -> their blocks
    Span<uint32_t> pendingDesignators;                        // of the brace list constructInto is about to consume
    std::unordered_map<Decl*, int64_t> enumeratorValues;
    std::unordered_map<Decl*, std::string> qualifiedNames; // qualifiedName() per declaration

    unsigned currentDepth = 0;       // of the function being emitted: 0 for one that is not an instantiation
    std::string currentFunctionName; // LLVM name of the function being emitted
    std::string currentRequestedBy;  // for an instantiation: the function that first used it

    static bool isLambda(const Ty* t) { return t && t->kind == TyKind::Lambda && t->decl; }
    static bool isAggregate(const Ty* t) { return isClass(t) || isLambda(t); }
    // A result that comes back through the caller's hidden result slot: a class, and a closure (a
    // lambda returned by value lives in memory like every closure; returned as an LLVM value, its
    // address and its bytes were confused: `ret %closure %o3`).
    static bool returnsInMemory(const Ty* t) { return isAggregate(t); }
    static LambdaExpr* lambdaOf(const Ty* t) { return isLambda(t) && t->decl->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(t->decl)->lambda : nullptr; }
    static bool isGenericLambda(const Ty* t) { LambdaExpr* l = lambdaOf(t); return l && !l->templateParams.empty(); }

    struct Object
    {
      std::string addr;
      const Ty* ty;
      bool exception = false; // addr holds a caught exception record: ending it destroys the object
      // Destroyed only by an exception leaving its scope (cleanup pads), never on a normal exit: a
      // constructor's bases and members as it builds them, which outlive it when it returns.
      bool unwindOnly = false;
      bool freeMemory = false; // `new`'s allocation while its constructor runs: freed, not destroyed
    };
    std::vector<std::vector<Object>> scopes; // Objects to destroy, per open scope.
    std::vector<Object> statementTemps;       // Temporaries of the current full-expression.
    struct Loop
    {
      std::string breakLabel, continueLabel;
      size_t scopeDepth;
    };
    std::vector<Loop> loops;

    // Exceptions (docs/design.md section 9) on Windows SEH: `throw` raises through kpp_throw, one
    // filter function per try statement matches the thrown type against the handlers, and the
    // destructors of the objects between a call and its handler run in cleanup pads.
    struct EHFrame
    {
      bool isHandler;            // false: an open try body; true: an open catch handler
      std::string dispatchLabel; // try: the block of its catchswitch
      std::string parentFunclet; // the funclet token the try or handler is in ("" at top level)
      std::string funclet;       // handler: its catchpad token
      std::string exceptionSlot; // handler: the alloca holding the caught exception record
      size_t scopeDepth;         // scopes.size() when entered
      bool used = false;         // try: some call in the body unwinds to the dispatch
    };
    std::vector<EHFrame> ehFrames;
    std::string ehBlocks;                                     // cleanup pads of the current function
    std::unordered_map<std::string, std::string> cleanupPads; // destruction state -> cleanup block
    std::string funcletOverride;                              // the pad being emitted, for its calls
    bool usesEH = false;                                      // the current function has invokes or pads
    std::string returnLabel; // a destructor's: where a `return` goes, the members and bases destroyed after the body

    Emitter(GlobalScope& global, const SymbolTable& symbols, Shared& sh) : global(global), types(global.types()), symbols(symbols), sh(sh) {}

    // --- diagnostics ---

    std::unordered_map<const ParsedFile*, std::vector<uint32_t>> lineStarts; // per file: the offset each line starts at

    void error(Node* at, const std::string& message)
    {
      errors.push_back(locationOf(at, currentFile) + ": " + message);
    }

    // "file:line" of a node in a file, "?" when unknown.
    std::string locationOf(Node* at, ParsedFile* file)
    {
      if (!file || !file->tokens || !at || file->tokens->empty())
        return "?";
      const std::vector<Token>& toks = *file->tokens;
      const Token& t = toks[std::min<size_t>(at->token, toks.size() - 1)];
      std::vector<uint32_t>& starts = lineStarts[file];
      if (starts.empty())
      {
        starts.push_back(0);
        for (uint32_t i = 0; i < file->text.size(); ++i)
          if (file->text[i] == '\n')
            starts.push_back(i + 1);
      }
      size_t line = size_t(std::upper_bound(starts.begin(), starts.end(), t.offset) - starts.begin()); // lines starting at or before the token
      return file->path + ":" + std::to_string(line);
    }

    struct Unsupported
    {
    };
    std::string lastFailure; // the message of the last fail(), without its place (a failed layout or global repeats it at every use)
    // An error that abandons what is being emitted (the function, layout or global it is in).
    [[noreturn]] void fail(Node* at, const std::string& message)
    {
      std::string where;
      if (!currentFunctionName.empty())
        where = " (in " + currentFunctionName + (currentRequestedBy.empty() ? "" : ", requested by " + currentRequestedBy) + ")";
      lastFailure = message;
      error(at, message + where);
      throw Unsupported{};
    }
    // A construct the code generator cannot translate yet.
    [[noreturn]] void unsupported(Node* at, const std::string& what) { fail(at, "codegen does not support " + what + " yet"); }

    // --- names and types ---

    // GlobalScope::qualifiedName, computed once per declaration.
    const std::string& qualifiedName(Decl* d)
    {
      auto it = qualifiedNames.find(d);
      if (it == qualifiedNames.end())
        it = qualifiedNames.emplace(d, global.qualifiedName(d)).first;
      return it->second;
    }

    std::string sym(uint32_t s) const { return s == UINT32_MAX ? "?" : std::string(symbols.name(s)); }
    // A type as IR names print it: every class and enum with its full path (and a local class with
    // where it is declared), so that two types never give one name (DebugInfoRenderer::LineInfo
    // and DebugVisualizationRenderer::LineInfo as template arguments made one RbTree<...>::newNode
    // for both). Messages print types short (types.toString).
    std::string irString(const Ty* t) const { return types.toString(t, &symbols, true); }

    // The variable whose initializer is being emitted (a global's or static member's in its
    // startup function, a default member initializer in a constructor), or null: names written
    // in it are typed from its declaration (the class's member aliases are in scope there), not
    // from the function that happens to be emitted (none, for a startup function).
    Decl* initializerOf = nullptr;
    // The closure of the lambda whose body is being emitted, or null: names written in the body
    // are typed from it (the lambda's template parameters, the aliases its body declares), not
    // from the function that happens to emit it (a generic lambda's is the one calling it).
    Decl* lambdaContext = nullptr;
    Decl* typingContext() const { return initializerOf ? initializerOf : lambdaContext ? lambdaContext : static_cast<Decl*>(currentFunction); }

    // A class declared in a function body, or a member of one: the main pass does not reach its
    // member functions (they are emitted when used), and its name alone is not unique.
    static bool declaredInFunction(const Decl* d)
    {
      for (const Decl* p = d ? d->parent : nullptr; p; p = p->parent)
        if (p->kind == NodeKind::FunctionDecl)
          return true;
      return false;
    }
    std::string newTemp() { return "%t" + std::to_string(temp++); }
    std::string newLabel(const char* hint) { return std::string(hint) + std::to_string(label++); }

    static bool isClass(const Ty* t) { return t && t->kind == TyKind::Class; }

    const Ty* declType(Decl* d) { return currentArgs ? global.typeOfDeclIn(d, *currentArgs) : global.typeOfDecl(d); }

    const Ty* classKey(const Ty* t) { return types.unqualified(types.stripRef(t)); }

    std::string llType(const Ty* t, Node* at = nullptr)
    {
      if (!t)
        unsupported(at, "an expression without a type");
      switch (t->kind)
      {
        case TyKind::Void: return "void";
        case TyKind::Bool: return "i1";
        case TyKind::Char: case TyKind::Int: return "i" + std::to_string(t->bits);
        case TyKind::Float: return t->bits == 32 ? "float" : "double";
        case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Function:
          return "ptr";
        case TyKind::MemberPointer: return t->inner && t->inner->kind == TyKind::Function ? "ptr" : "i64"; // a function, or a data member's offset within its class
        case TyKind::Lambda: return t->decl ? closureStructName(t) : "ptr";
        case TyKind::Array:
          return "[" + std::to_string(t->count) + " x " + llType(t->inner, at) + "]";
        case TyKind::Vector:
          return "<" + std::to_string(t->count) + " x " + llType(t->inner, at) + ">";
        case TyKind::Class: return structFor(t, at).name;
        case TyKind::Enum: return llType(enumUnderlying(t->decl), at);
        default:
          unsupported(at, "the type " + types.toString(t, &symbols) + (t->kind == TyKind::Pack ? " (a parameter pack)" : t->kind == TyKind::Param ? " (a template parameter)" : "") +
                            (at ? std::string(" of ") + nodeKindName(at->kind) : std::string()));
      }
    }

    const Ty* enumUnderlying(Decl* d)
    {
      Decl* def = global.definitionOf(d);
      auto* e = static_cast<EnumDecl*>(def ? def : d);
      if (e->underlying)
        if (const Ty* u = global.typeOfAst(e->underlying, e))
          if (u->isKnown())
            return u;
      return types.intType(32);
    }

    // Zeroes an object of type t at addr (value initialization): a struct or an array with a memset of
    // its size, anything else with a store. The instruction selector splits `store [1048576 x i8]
    // zeroinitializer` into a node per element and crashed on a member `char buffer[1 << 20]`
    // (BufferedReadStream); the size is LLVM's own (`getelementptr (T, ptr null, i32 1)`).
    void storeZero(const Ty* t, const std::string& addr, Node* at)
    {
      // An empty class has no state to clear, and as a base it may have no bytes of its own (laid
      // out with no size: its byte would be the next member's, or past the object).
      if (isClass(t) && structFor(t, at).empty)
        return;
      std::string ty = llType(t, at);
      if (ty[0] == '%' || ty[0] == '[' || ty.compare(0, 2, "<{") == 0)
      {
        if (declaredNames.claim("@llvm.memset.p0.i64"))
          decls += "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n";
        fn += "  call void @llvm.memset.p0.i64(ptr " + addr + ", i8 0, i64 ptrtoint (ptr getelementptr (" + ty + ", ptr null, i32 1) to i64), i1 false)\n";
        return;
      }
      fn += "  store " + ty + " zeroinitializer, ptr " + addr + "\n";
    }

    uint64_t sizeOf(const Ty* t, Node* at = nullptr)
    {
      switch (t->kind)
      {
        case TyKind::Bool: return 1;
        case TyKind::Char: case TyKind::Int: case TyKind::Float: return t->bits / 8;
        case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Function:
        case TyKind::MemberPointer:
          return 8;
        case TyKind::Lambda:
        {
          // The closure struct's own size: its fields where LLVM puts them (a reference capture is
          // a pointer; fieldTypes holds what it points to), rounded to the largest alignment; an
          // empty closure is one byte.
          if (!t->decl)
            return 8;
          const ClosureInfo& info = closureFor(t, at);
          uint64_t size = 0, align = 1;
          for (size_t i = 0; i < info.fieldTypes.size(); ++i)
          {
            uint64_t s = info.byRef[i] ? 8 : sizeOf(info.fieldTypes[i], at), a = info.byRef[i] ? 8 : llvmAlignOf(info.fieldTypes[i], at);
            size = (size + a - 1) / a * a + s;
            align = std::max(align, a);
          }
          return std::max<uint64_t>((size + align - 1) / align * align, 1);
        }
        case TyKind::Array: return t->count * sizeOf(t->inner, at);
        case TyKind::Vector: return t->count * sizeOf(t->inner, at);
        case TyKind::Enum: return sizeOf(enumUnderlying(t->decl), at);
        case TyKind::Class: return classSizeAndAlign(t, at).first;
        default:
          unsupported(at, "sizeof " + types.toString(t, &symbols));
      }
    }

    // A class's size and alignment from its fields, remembered once its layout is complete.
    std::unordered_map<const Ty*, std::pair<uint64_t, uint64_t>> sizeCache; // this emitter's view of the remembered sizes
    std::pair<uint64_t, uint64_t> classSizeAndAlign(const Ty* t, Node* at)
    {
      if (auto mine = sizeCache.find(t); mine != sizeCache.end())
        return mine->second;
      auto lock = sharedLock(); // the size is remembered in the shared layout
      StructInfo& info = structFor(t, at);
      if (info.align)
      {
        sizeCache.emplace(t, std::pair{info.size, info.align});
        return {info.size, info.align};
      }
      if (info.sizing)
        unsupported(at, "the class " + types.toString(t, &symbols) + ", which contains itself (a member or base of its own type)");
      struct Sizing
      {
        bool& flag;
        Sizing(bool& flag) : flag(flag) { flag = true; }
        ~Sizing() { flag = false; }
      } sizing(info.sizing);
      uint64_t size = 0, align = 1;
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
      {
        if (i < info.zeroSize.size() && info.zeroSize[i])
          continue; // an empty base: no storage of its own
        const Ty* f = info.fieldTypes[i];
        uint64_t a = info.packed ? 1 : alignOf(f, at);
        size = (size + a - 1) / a * a + sizeOf(f, at);
        align = std::max(align, a);
      }
      align = std::max(align, declaredAlignment(t, at)); // `struct alignas(16) X`
      size = std::max<uint64_t>((size + align - 1) / align * align, 1);
      if (info.complete)
        info.size = size, info.align = align;
      return {size, align};
    }

    uint64_t alignOf(const Ty* t, Node* at = nullptr)
    {
      switch (t->kind)
      {
        case TyKind::Array: return alignOf(t->inner, at);
        case TyKind::Vector: return t->count * sizeOf(t->inner, at); // SSE vectors are 16-byte aligned
        case TyKind::Class: return classSizeAndAlign(t, at).second;
        case TyKind::Lambda:
        {
          if (!t->decl)
            return 8;
          const ClosureInfo& info = closureFor(t, at);
          uint64_t align = 1;
          for (size_t i = 0; i < info.fieldTypes.size(); ++i)
            align = std::max(align, info.byRef[i] ? uint64_t(8) : alignOf(info.fieldTypes[i], at));
          return align;
        }
        default: return std::max<uint64_t>(1, sizeOf(t, at));
      }
    }

    // The alignment a class declares with alignas(N), or 1; N under the instantiation's arguments
    // (`template<size_t Align> struct alignas(Align) AlignedBytes`).
    uint64_t declaredAlignment(const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (!c->alignment)
        return 1;
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      return alignmentValue(c->alignment, c, args, types.toString(t, &symbols), at);
    }

    // An alignas(N) written in owner (a class, a function), N under args: a power of two.
    uint64_t alignmentValue(Expr* alignment, Decl* owner, const TemplateArgs& args, const std::string& what, Node* at)
    {
      ArgsScope scope(*this, &args);
      ConstContextScope context(*this, owner);
      ParsedFile* saved = currentFile;
      currentFile = owner && owner->file ? owner->file : currentFile;
      std::optional<int64_t> v = evalConst(alignment);
      currentFile = saved;
      if (!v || *v <= 0 || (*v & (*v - 1)))
        unsupported(at, "the alignment of " + what + ", which is not a constant power of two");
      return uint64_t(*v);
    }

    // The alignment LLVM gives a value of type t from the type alone (its ABI alignment): what a
    // class's alignas adds on top of its fields' it only gets from padding fields and `align`.
    uint64_t llvmAlignOf(const Ty* t, Node* at)
    {
      switch (t->kind)
      {
        case TyKind::Array: return llvmAlignOf(t->inner, at);
        case TyKind::Class: return structFor(t, at).llvmAlign;
        default: return alignOf(t, at);
      }
    }

    // `, align N` for an object of type t (and an alignas(N) of the variable itself, or 0) whose
    // alignment is more than LLVM gives its type: an alloca's, a global's.
    std::string alignSuffix(const Ty* t, uint64_t declared, Node* at)
    {
      t = types.unqualified(types.stripRef(t));
      if (!declared && !overAlignedType(t, at))
        return "";
      uint64_t want = std::max(declared, alignOf(t, at));
      return want > llvmAlignOf(t, at) ? ", align " + std::to_string(want) : std::string();
    }

    // Whether an object of type t needs more alignment than LLVM gives its type (a class with
    // alignas, or holding a member that has it).
    bool overAlignedType(const Ty* t, Node* at)
    {
      t = types.unqualified(t);
      while (t->kind == TyKind::Array)
        t = types.unqualified(t->inner);
      return t->kind == TyKind::Class && structFor(t, at).overAligned;
    }

    // The alignment of a class (info, laid out but for its LLVM text) and of each field: LLVM aligns
    // a field to its type's ABI alignment only, so where alignas asks for more (on the class, on a
    // member, or a member's class has it) the bytes C++ puts before a field become a `[n x i8]` field
    // of their own, and the class gets the size and alignment sizeof and alignof give. Returns the
    // padding to write at the end of the struct (after its byte of its own, when empty).
    uint64_t alignPadding(StructInfo& info, ClassDecl* c, const Ty* key, const TemplateArgs& args, Node* at)
    {
      uint64_t natural = 1;
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
        if (!info.zeroSize[i] && !info.packed)
          natural = std::max(natural, llvmAlignOf(info.fieldTypes[i], at));
      info.llvmAlign = natural;
      if (info.packed)
        return 0;
      std::vector<uint64_t> declaredAt(info.fieldTypes.size(), 0); // a member's own alignas
      bool needed = false;
      for (Decl* m : c->members)
        if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->alignment && info.fieldIndex.count(m))
        {
          uint32_t i = info.fieldIndex[m];
          declaredAt[i] = std::max(declaredAt[i], alignmentValue(static_cast<VariableDecl*>(m)->alignment, c, args,
                                                                 "the member " + sym(static_cast<VariableDecl*>(m)->name.last().symbol), at));
          needed = needed || declaredAt[i] > llvmAlignOf(info.fieldTypes[i], at);
        }
      for (size_t i = 0; i < info.fieldTypes.size() && !needed; ++i)
        needed = !info.zeroSize[i] && overAlignedType(info.fieldTypes[i], at);
      uint64_t declaredClass = c->alignment ? declaredAlignment(key, at) : 1;
      if (!needed && declaredClass <= natural)
        return 0;
      std::vector<const Ty*> fields;
      std::vector<bool> zero;
      std::vector<uint32_t> newIndex(info.fieldTypes.size());
      uint64_t offset = 0, align = 1;
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
      {
        bool none = info.zeroSize[i];
        uint64_t want = none ? 1 : std::max(alignOf(info.fieldTypes[i], at), declaredAt[i]);
        uint64_t llvmWant = none ? 1 : llvmAlignOf(info.fieldTypes[i], at);
        uint64_t place = (offset + want - 1) / want * want;
        if (place != (offset + llvmWant - 1) / llvmWant * llvmWant)
        {
          fields.push_back(types.array(types.intType(8), uint32_t(place - offset)));
          zero.push_back(false);
        }
        newIndex[i] = uint32_t(fields.size());
        fields.push_back(info.fieldTypes[i]);
        zero.push_back(none);
        offset = place + (none ? 0 : sizeOf(info.fieldTypes[i], at));
        align = std::max(align, want);
      }
      if (info.empty)
        offset = std::max<uint64_t>(offset, 1); // its byte of its own
      align = std::max(align, declaredClass);
      uint64_t size = std::max<uint64_t>((offset + align - 1) / align * align, 1);
      info.fieldTypes = std::move(fields);
      info.zeroSize = std::move(zero);
      for (auto& [d, index] : info.fieldIndex)
        index = newIndex[index];
      for (auto& [d, index] : info.bases)
        index = newIndex[index];
      for (auto& [d, index] : info.anonymous)
        index = newIndex[index];
      info.size = size;
      info.align = align;
      info.overAligned = align > natural;
      uint64_t llvmSize = (offset + natural - 1) / natural * natural;
      return size > llvmSize ? size - offset : 0;
    }

    // A local's or a global's own alignas(N), or 0: N in the context it is written in (the function
    // being emitted for a local, the variable itself for a global), under the current arguments.
    uint64_t variableAlignment(VariableDecl* v, Decl* context, Node* at)
    {
      if (!v || !v->alignment)
        return 0;
      return alignmentValue(v->alignment, context, currentArgs ? *currentArgs : TemplateArgs{}, sym(v->name.last().symbol), at);
    }

    ClassDecl* classDef(Decl* decl, Node* at)
    {
      Decl* def = global.definitionOf(decl);
      if (!def || def->kind != NodeKind::ClassDecl)
        unsupported(at, "a class without a definition (" + qualifiedName(decl) + ")");
      return static_cast<ClassDecl*>(def);
    }

    // The instantiation type with the definition as its decl (forward declarations map here).
    const Ty* canonicalClass(const Ty* t, Node* at)
    {
      t = classKey(t);
      ClassDecl* def = classDef(t->decl, at);
      std::vector<const Ty*> args;
      bool changed = def != t->decl;
      for (const Ty* a : t->args)
      {
        args.push_back(definitionsIn(a));
        changed = changed || args.back() != a;
      }
      return changed ? types.classType(def, Span<const Ty*>{args.data(), uint32_t(args.size())}) : t;
    }

    // Type t with every class in it named by its definition, a forward declaration's decl and the
    // definition's being one class: the key of a class's layout (and IR name) is then the same however
    // its arguments were reached (`ReverseIterator<const agui::Widget**>` with Widget declared ahead in
    // one file and defined in another was two layouts, the second named `.2` in whatever order the
    // threads met them, so the IR differed from run to run).
    std::unordered_map<const Ty*, const Ty*> definitionsCache;
    const Ty* definitionsIn(const Ty* t, int depth = 0)
    {
      if (!t || depth > 32)
        return t;
      if (auto it = definitionsCache.find(t); it != definitionsCache.end())
        return it->second;
      const Ty* u = types.unqualified(t);
      const Ty* result = u;
      switch (u->kind)
      {
        case TyKind::Pointer: result = types.pointer(definitionsIn(u->inner, depth + 1)); break;
        case TyKind::LRef: result = types.lref(definitionsIn(u->inner, depth + 1)); break;
        case TyKind::RRef: result = types.rref(definitionsIn(u->inner, depth + 1)); break;
        case TyKind::Array: result = u->decl ? u : types.array(definitionsIn(u->inner, depth + 1), u->count); break;
        case TyKind::Class:
          if (!u->isBraceList())
          {
            Decl* def = u->decl ? global.definitionOf(u->decl) : nullptr;
            std::vector<const Ty*> args;
            bool changed = def && def != u->decl && def->kind == NodeKind::ClassDecl;
            for (const Ty* a : u->args)
            {
              args.push_back(definitionsIn(a, depth + 1));
              changed = changed || args.back() != a;
            }
            if (changed)
              result = types.classType(def && def->kind == NodeKind::ClassDecl ? def : u->decl, Span<const Ty*>{args.data(), uint32_t(args.size())});
          }
          break;
        default: break;
      }
      if (result != u && (t->isConst || t->isVolatile))
        result = types.qualified(result, t->isConst, t->isVolatile);
      else if (result == u)
        result = t;
      definitionsCache.emplace(t, result);
      return result;
    }

    // The layouts this emitter has used: complete (or failed) ones, which do not change any more,
    // so they are read without the shared lock.
    std::unordered_map<const Ty*, StructInfo*> layoutCache;

    StructInfo& structFor(const Ty* classTy, Node* at)
    {
      if (auto mine = layoutCache.find(classTy); mine != layoutCache.end())
        return checkedLayout(*mine->second, classTy, at);
      auto lock = sharedLock();
      StructInfo* info = nullptr;
      if (auto known = structsByType.find(classTy); known != structsByType.end())
        info = known->second;
      else
      {
        const Ty* key = canonicalClass(classTy, at);
        auto it = structs.find(key);
        if (it != structs.end())
        {
          structsByType.emplace(classTy, &it->second);
          info = &it->second;
        }
        else
        {
          size_t errorsBefore = errors.size();
          try
          {
            info = &buildStruct(key, at);
          }
          catch (Unsupported&)
          {
            // Every use reports what made it fail, at the use: the first use like the others
            // (its own report came from inside the layout, wherever that was), so that the lines
            // do not depend on which use comes first.
            structs[key].failed = true;
            structs[key].failedWhy = lastFailure;
            layoutCache.emplace(classTy, &structs[key]);
            errors.resize(errorsBefore);
            fail(at, structs[key].failedWhy);
          }
        }
      }
      if (info->complete || info->failed)
        layoutCache.emplace(classTy, info);
      return checkedLayout(*info, classTy, at);
    }

    // A failed layout gives every use the error its first use got (the same lines whichever use
    // comes first, so that they do not depend on the order functions are emitted in).
    StructInfo& checkedLayout(StructInfo& info, const Ty* classTy, Node* at)
    {
      if (info.failed)
        fail(at, info.failedWhy.empty() ? "codegen does not support the class " + types.toString(classKey(classTy), &symbols) + ", whose layout failed yet" : info.failedWhy);
      return info;
    }

    // The placeholder class of a type name declared nowhere (a draft) that a value of type t is or
    // holds by value (through arrays), or null.
    static const Ty* draftHeld(const Ty* t)
    {
      for (; t; t = t->kind == TyKind::Array ? t->inner : nullptr)
        if (t->kind == TyKind::Class && t->decl && (t->decl->attributes & ATTR_Draft))
          return t;
      return nullptr;
    }

    // Lays out a class: bases first, then its data members; every member and base must be known
    // (a guess would be a wrong layout that compiles).
    StructInfo& buildStruct(const Ty* key, Node* at)
    {
      ClassDecl* c = static_cast<ClassDecl*>(key->decl);
      auto layoutError = [&](const std::string& why) { unsupported(at, "the layout of " + types.toString(key, &symbols) + ": " + why); };
      // A member as written (`Tick lastCheck`), for the error naming what has no known type.
      auto memberName = [&](Decl* m)
      {
        std::string name = m->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(m)->name.empty() ? sym(static_cast<VariableDecl*>(m)->name.last().symbol) : std::string("?");
        if (!m->file || !m->file->tokens || m->endToken <= m->token || m->endToken > m->file->tokens->size())
          return name;
        const std::vector<Token>& toks = *m->file->tokens;
        size_t from = toks[m->token].offset, to = toks[m->endToken - 1].offset + toks[m->endToken - 1].length;
        std::string written;
        for (char ch : m->file->text.substr(from, std::min<size_t>(to - from, 160)))
          if (ch != '\n' && ch != '\r' && !(ch == ' ' && !written.empty() && written.back() == ' '))
            written += ch == '\t' ? ' ' : ch;
        return name + " (`" + written + "`)";
      };
      // A type name declared nowhere is the resolver's placeholder class (a draft, so that checking
      // goes on): an error wherever a layout needs it, never an empty class that compiles.
      if (c->attributes & ATTR_Draft)
        fail(at, "error: the type '" + global.draftName(c) + "' is not declared");
      auto undeclared = [&](const std::string& what, const Ty* draft)
      {
        fail(at, "error: the layout of " + types.toString(key, &symbols) + ": " + what + " names '" + global.draftName(draft->decl) + "', which is not declared");
      };
      // `Imm<strlen("x")>`: an argument the checker could not work out (a constant it could not
      // evaluate) is no argument to lay out with; taken for 0, `char data[N]` was an empty array
      // that writes ran past.
      for (size_t i = 0; i < key->args.size(); ++i)
        if (!key->args[i] || !key->args[i]->isKnown())
        {
          Span<Decl*> params = key->decl->templateParams;
          auto* p = i < params.size() ? static_cast<TemplateParamDecl*>(params[i]) : nullptr;
          std::string which = p && p->name != UINT32_MAX ? "the argument for " + sym(p->name) : "argument " + std::to_string(i + 1);
          fail(at, "error: the layout of " + types.toString(key, &symbols) + ": " + which +
                       (p && p->isType ? " is a type the compiler could not resolve" : " is a constant the compiler could not evaluate"));
        }
      TemplateArgs args = global.classArgsOf(key);
      if (c->keyword == Kw::union_)
      {
        // A union is one block of storage; every member lives at its start.
        StructInfo& info = structs[key];
        info.name = irTypeName("%struct.", key);
        info.isUnion = true;
        uint64_t size = 1, align = 1;
        for (Decl* m : c->members)
        {
          const Ty* ft = nullptr;
          if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
            ft = global.typeOfDeclIn(m, args);
          else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
            ft = types.classType(m, key->args); // an anonymous struct takes the enclosing instantiation's arguments
          else
            continue;
          info.fieldIndex[m] = 0;
          info.fieldDecls.push_back(m);
          if (!ft || !ft->isKnown() || types.isDependent(ft))
            layoutError("the member " + memberName(m) + " has an unknown type");
          if (const Ty* draft = draftHeld(ft))
            undeclared("the member " + memberName(m), draft);
          if (m->kind == NodeKind::ClassDecl)
            info.anonymous.push_back({m, 0});
          size = std::max(size, sizeOf(ft, at));
          align = std::max(align, alignOf(ft, at));
          if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->alignment)
            align = std::max(align, alignmentValue(static_cast<VariableDecl*>(m)->alignment, c, args, "the member " + memberName(m), at));
        }
        // The storage: integers of the union's alignment, up to i128 (LLVM's most aligned integer);
        // a member aligned to more (OptionalOwningStorage of an alignas(64) class) makes the union
        // over-aligned, its objects then aligned with `align`.
        uint64_t unit = std::min<uint64_t>(align, 16), total = (size + align - 1) / align * align;
        info.llvmAlign = unit;
        info.fieldTypes.push_back(types.array(types.intType(uint8_t(unit * 8)), uint32_t(total / unit)));
        if (align > unit)
        {
          info.size = total;
          info.align = align;
          info.overAligned = true;
        }
        decls += info.name + " = type { " + llType(info.fieldTypes[0], c) + " }\n";
        ++result.structs;
        info.complete = true;
        return structs[key];
      }
      StructInfo& info = structs[key];
      info.name = irTypeName("%struct.", key);
      // Bases first (resolved before deciding on the vtable pointer).
      std::vector<const Ty*> baseKeys;
      for (const ClassDecl::Base& b : c->bases)
      {
        const Ty* bt = global.typeOfAstIn(b.type, c, args);
        if (!bt || bt->kind != TyKind::Class || types.isDependent(bt))
          layoutError("a base class whose type is unknown (" + (bt ? types.toString(bt, &symbols) : std::string("?")) + ")");
        if (const Ty* draft = draftHeld(bt))
          undeclared("a base class", draft);
        const Ty* baseKey = canonicalClass(bt, at);
        structFor(baseKey, at);
        baseKeys.push_back(baseKey);
      }
      // The primary base is the first polymorphic one: at offset 0, its vtable pointer the class's
      // own. A polymorphic class without a polymorphic base has a vtable pointer of its own there.
      // The polymorphic bases are laid out first, then the others (MSVC's layout, design.md section
      // 9); info.bases keeps declaration order, the order bases are built in.
      int primary = -1;
      for (size_t i = 0; i < baseKeys.size() && primary < 0; ++i)
        if (structs[baseKeys[i]].polymorphic)
          primary = int(i);
      bool ownVptr = false;
      if (primary < 0)
        for (Decl* m : c->members)
          if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override)) && !(m->flags & (DF_Static | DF_Constructor)) &&
              m->templateParams.empty())
            ownVptr = true;
      info.polymorphic = primary >= 0 || ownVptr;
      info.primaryBase = primary >= 0 ? baseKeys[primary] : nullptr;
      if (ownVptr)
        info.fieldTypes.push_back(types.pointer(types.voidType()));
      std::vector<uint32_t> baseIndex(baseKeys.size());
      for (bool polymorphicPass : {true, false})
        for (size_t i = 0; i < baseKeys.size(); ++i)
          if (structs[baseKeys[i]].polymorphic == polymorphicPass)
          {
            baseIndex[i] = uint32_t(info.fieldTypes.size());
            info.fieldTypes.push_back(baseKeys[i]);
          }
      for (size_t i = 0; i < baseKeys.size(); ++i)
        info.bases.push_back({baseKeys[i]->decl, baseIndex[i]});
      int bitUnitIndex = -1;    // the storage field the current run of bit fields shares
      uint32_t bitUnitBits = 0; // its size in bits
      uint32_t bitUsed = 0;
      for (Decl* m : c->members)
      {
        if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
        {
          // Anonymous struct/union member: a field whose own members are reachable by name, laid
          // out under the enclosing instantiation's arguments (ImmutableStringTemplate<N>'s union
          // holding a LongStr<N>).
          const Ty* ft = types.classType(m, key->args);
          structFor(ft, at);
          info.anonymous.push_back({m, uint32_t(info.fieldTypes.size())});
          info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
          info.fieldDecls.push_back(m);
          info.fieldTypes.push_back(ft);
          bitUnitIndex = -1;
          continue;
        }
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef | DF_Friend)))
          continue;
        const Ty* ft = global.typeOfDeclIn(m, args);
        if (!ft || !ft->isKnown() || types.isDependent(ft))
          layoutError("the member " + memberName(m) + " has an unknown type");
        if (const Ty* draft = draftHeld(ft))
          undeclared("the member " + memberName(m), draft);
        // `char data[N]` with N no constant the checker could evaluate has the length 0; only a C
        // struct's last member may be such an array (`char data[];`, a flexible array member).
        for (const Ty* a = types.unqualified(ft); a && a->kind == TyKind::Array; a = types.unqualified(a->inner))
          if (a->count == 0 && !(c->file && c->file->isImported))
            fail(at, "error: the layout of " + types.toString(key, &symbols) + ": the member " + memberName(m) +
                         " is an array whose length is no constant the compiler could evaluate");
        // A lambda held by value (FunctionImpl<F>'s `F f`, TransformView's): its closure, laid out
        // from the lambda's AST whoever asks first; one that cannot be laid out fails this class.
        for (const Ty* held = types.unqualified(ft); held; held = held->kind == TyKind::Array ? types.unqualified(held->inner) : nullptr)
          if (isLambda(held))
            closureFor(held, at);
        if (Expr* widthExpr = static_cast<VariableDecl*>(m)->bitfield)
        {
          // `unsigned level : 3;`: consecutive bit fields share a storage unit while their types
          // have one size and their widths fit (MSVC: `uint8_t x : 5; bool on : 1;` share a byte,
          // `int a : 3; unsigned b : 4;` four bytes); each remembers its shift and width.
          // The width under the class's arguments, in its context: not the arguments of whichever
          // function asked for the layout first.
          ParsedFile* savedFile = currentFile;
          currentFile = c->file ? c->file : currentFile;
          std::optional<int64_t> w;
          {
            ArgsScope widthArgs(*this, &args);
            ConstContextScope widthContext(*this, c);
            w = evalConst(widthExpr);
          }
          currentFile = savedFile;
          if (!w || *w <= 0)
            unsupported(m, "a bit field whose width is not a constant");
          const Ty* unit = types.unqualified(ft);
          if (unit->kind == TyKind::Enum)
            unit = enumUnderlying(unit->decl);
          if (unit->kind == TyKind::Bool)
            unit = types.intType(8, true);
          uint32_t unitBits = uint32_t(sizeOf(unit, m) * 8), width = uint32_t(*w);
          if (bitUnitIndex < 0 || bitUnitBits != unitBits || bitUsed + width > unitBits)
          {
            bitUnitIndex = int(info.fieldTypes.size());
            bitUnitBits = unitBits;
            bitUsed = 0;
            info.fieldTypes.push_back(unit);
            info.fieldDecls.push_back(m); // the first field of the unit stands for the unit in memberwise work
          }
          info.fieldIndex[m] = uint32_t(bitUnitIndex);
          info.bitfields[m] = {bitUsed, width};
          bitfieldOf[m] = {bitUsed, width, unit};
          bitUsed += width;
          continue;
        }
        bitUnitIndex = -1;
        info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
        info.fieldDecls.push_back(m);
        info.fieldTypes.push_back(ft);
      }
      // Empty bases take no storage, unless the first member is of the same class (two objects of
      // one type at one address: C++ gives the base a byte then).
      info.zeroSize.assign(info.fieldTypes.size(), false);
      const Ty* firstMember = info.fieldDecls.empty() ? nullptr : classKey(info.fieldTypes[fieldIndexOf(info, info.fieldDecls[0])]);
      bool allBasesEmpty = true;
      for (auto& [baseDef, index] : info.bases)
      {
        bool emptyBase = structs[info.fieldTypes[index]].empty;
        allBasesEmpty = allBasesEmpty && emptyBase;
        info.zeroSize[index] = emptyBase && classKey(info.fieldTypes[index]) != firstMember;
      }
      info.empty = !info.polymorphic && info.fieldDecls.empty() && allBasesEmpty;
      info.packed = (c->attributes & ATTR_Packed) != 0;
      uint64_t tailPadding = alignPadding(info, c, key, args, at);
      info.tailPadding = tailPadding;
      std::string line = info.name + " = type " + (info.packed ? "<{ " : "{ ");
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
        line += (i ? ", " : "") + (info.zeroSize[i] ? std::string("[0 x i8]") : llType(info.fieldTypes[i], c));
      if (info.empty)
        line += info.fieldTypes.empty() ? "i8" : ", i8"; // one byte of its own as an object
      if (tailPadding)
        line += ", [" + std::to_string(tailPadding) + " x i8]";
      line += info.packed ? " }>\n" : " }\n";
      decls += line;
      ++result.structs;
      info.complete = true;
      if (structs[key].polymorphic)
      {
        // The base fields are final now (alignPadding may have moved them).
        std::vector<uint32_t> finalIndex;
        for (auto& [baseDef, index] : structs[key].bases)
          finalIndex.push_back(index);
        structs[key].vtables = collectVtables(key, c, args, baseKeys, finalIndex, primary);
        emitVtables(key, at);
      }
      return structs[key];
    }

    // The vtables of class key (design.md section 9): the primary one extends the primary base's
    // (or starts empty, the class's own); then every vtable of the other polymorphic bases, and those
    // of the primary base after its first, as the bases have them, at the bases' places. The class's
    // virtual functions take the slots they override in all of them; one that overrides no slot of
    // the primary vtable also gets a slot there, so that a call through the class needs no thunk.
    std::vector<Vtable> collectVtables(const Ty* key, ClassDecl* c, const TemplateArgs& args, const std::vector<const Ty*>& baseKeys,
                                       const std::vector<uint32_t>& baseIndex, int primary)
    {
      std::vector<Vtable> tables(1);
      for (size_t i = 0; i < baseKeys.size(); ++i)
      {
        std::vector<Vtable> inherited = structs[baseKeys[i]].vtables;
        for (size_t j = 0; j < inherited.size(); ++j)
        {
          Vtable& t = inherited[j];
          for (VtableSlot& s : t.slots)
            s.ownerPath.insert(s.ownerPath.begin(), baseIndex[i]);
          if (int(i) == primary && j == 0)
          {
            tables[0].slots = std::move(t.slots);
            continue;
          }
          t.path.insert(t.path.begin(), baseIndex[i]);
          t.name.clear();
          t.vptrValue.clear();
          tables.push_back(std::move(t));
        }
      }
      auto addToPrimary = [&](const VtableSlot& slot)
      {
        for (const VtableSlot& s : tables[0].slots)
          if (s.key == slot.key)
            return;
        tables[0].slots.push_back(slot);
      };
      bool declaresDestructor = false;
      {
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || (m->flags & (DF_Static | DF_Constructor)) || !m->templateParams.empty())
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          declaresDestructor = declaresDestructor || (f->flags & DF_Destructor);
          VtableSlot own{methodKey(f), f, key, {}};
          bool overrides = false;
          for (Vtable& t : tables)
            for (VtableSlot& s : t.slots)
              if (s.key == own.key)
              {
                s = own;
                overrides = true;
              }
          if (overrides || (f->flags & (DF_Virtual | DF_Override)))
            addToPrimary(own);
        }
      }
      if (!declaresDestructor)
      {
        // A class that declares no destructor has an implicit one, virtual when a base's is: it
        // destroys this class's members too, so it takes the destructor slots.
        VtableSlot implicit{"~", nullptr, key, {}};
        bool virtualDestructor = false;
        for (Vtable& t : tables)
          for (VtableSlot& s : t.slots)
            if (s.key == "~")
            {
              s = implicit;
              virtualDestructor = true;
            }
        if (virtualDestructor)
          addToPrimary(implicit);
      }
      return tables;
    }

    // The address an object's subobject at path (base fields down from class cls) has in an object
    // placed at address 0, which is its offset: a constant expression, "null" for the object itself.
    std::string subobjectAtNull(const Ty* cls, const std::vector<uint32_t>& path, Node* at)
    {
      std::string p = "null";
      const Ty* t = cls;
      for (uint32_t index : path)
      {
        StructInfo& info = structFor(t, at);
        p = "getelementptr (" + info.name + ", ptr " + p + ", i32 0, i32 " + std::to_string(index) + ")";
        t = info.fieldTypes[index];
      }
      return p;
    }

    // Whether two subobjects at these paths start at the same address: a field 0 is at its
    // object's start, so trailing zeros go (the primary base's subobject, and its primary base's).
    static bool samePlace(std::vector<uint32_t> a, std::vector<uint32_t> b)
    {
      while (!a.empty() && a.back() == 0)
        a.pop_back();
      while (!b.empty() && b.back() == 0)
        b.pop_back();
      return a == b;
    }

    // The definitions of class key's vtables, with a thunk for each slot whose function belongs to a
    // class at another place than the subobject whose vtable pointer leads to the slot. A vtable
    // pointer points at the first function's slot, the functions following in declaration order
    // (MSVC's layout and COM's: a C object's table of functions is called as a Kpp class's, and the
    // reverse). Slot -1 is the class's descriptor (typeid, dynamic_cast), slot -2 the offset of the
    // subobject holding the pointer within the whole object (delete and dynamic_cast find the whole
    // object with it).
    // The name a class's vtables, thunks and typeinfo are called by: its printed name, and for a class
    // whose printed name another class shares, the number its IR type got (irTypeName's ".N"). A
    // local class of a function template has one name for all instances
    // (`LambdaIDConnector::make::InnerResolver@73` for every Callback): their vtables were one
    // linkonce_odr vtable, which the linker kept for all, so every InnerResolver ran one callback.
    std::string classIrName(const Ty* key, const StructInfo& info)
    {
      std::string name = irString(key);
      const size_t hashed = std::string("%struct.").size() + 16; // "%struct." and 16 hex digits
      if (info.name.size() > hashed && info.name[hashed] == '.')
        name += "#" + info.name.substr(hashed + 1);
      return name;
    }

    void emitVtables(const Ty* key, Node* at)
    {
      StructInfo& info = structs[key];
      std::string ti = descriptorFor(key, at);
      std::vector<Vtable> tables = info.vtables;
      std::string text, thunks;
      // The vtable asks for its functions, not whichever function happened to need the layout first
      // (the requester the diagnostics name, the same in every run).
      struct NameRestore { std::string& name; std::string saved; ~NameRestore() { name = std::move(saved); } } restore{currentFunctionName, currentFunctionName};
      currentFunctionName = "the vtable of " + types.toString(key, &symbols);
      for (size_t t = 0; t < tables.size(); ++t)
      {
        Vtable& table = tables[t];
        table.name = "@\"vtable." + classIrName(key, info) + (t ? "." + std::to_string(t) : std::string()) + "\"";
        std::string entries = "ptr " + subobjectAtNull(key, table.path, at) + ", ptr " + ti;
        for (size_t s = 0; s < table.slots.size(); ++s)
        {
          const VtableSlot& slot = table.slots[s];
          if (slot.function && (slot.function->flags & DF_Pure))
          {
            entries += ", ptr null";
            continue;
          }
          std::string target = slot.function ? useFunction(slot.function, global.classArgsOf(slot.owner), slot.owner)
                                             : implicitDestructor(slot.owner, at); // a class declaring none still destroys its own members
          if (!samePlace(slot.ownerPath, table.path))
          {
            std::string thunk = "@\"thunk." + classIrName(key, info) + "." + std::to_string(t) + "." + std::to_string(s) + "\"";
            thunks += adjustingThunk(thunk, target, slot, subobjectAtNull(key, table.path, at), subobjectAtNull(key, slot.ownerPath, at), at);
            target = thunk;
          }
          entries += ", ptr " + target;
        }
        std::string tableType = "[" + std::to_string(table.slots.size() + 2) + " x ptr]";
        text += "$" + table.name.substr(1) + " = comdat any\n" + table.name + " = linkonce_odr constant " + tableType + " [" + entries + "], comdat\n";
        table.vptrValue = "getelementptr inbounds (" + tableType + ", ptr " + table.name + ", i64 0, i64 2)";
      }
      StructInfo& done = structs[key];
      done.vtables = std::move(tables);
      done.vtableName = done.vtables[0].name;
      done.vptrValue = done.vtables[0].vptrValue;
      done.vtableText = text + thunks;
    }

    // A function that moves `this` from the subobject at `from` to the one at `to` (offsets as
    // subobjectAtNull gives them) and calls target with the arguments it was given.
    std::string adjustingThunk(const std::string& name, const std::string& target, const VtableSlot& slot, const std::string& from, const std::string& to, Node* at)
    {
      Signature s;
      if (slot.function)
      {
        TemplateArgs args = global.classArgsOf(slot.owner);
        ArgsScope scope(*this, &args);
        s = signatureOf(slot.function);
      }
      else
      {
        s.returnType = "void";
        s.paramTypes = {"ptr"};
        s.hasThis = true;
      }
      if (s.variadic)
        unsupported(at, "the variadic virtual function " + qualifiedName(slot.function) + " reached through another base of " +
                            types.toString(slot.owner, &symbols) + " (its thunk cannot pass the arguments on)");
      std::string params, args;
      for (size_t i = 1; i < s.paramTypes.size(); ++i)
      {
        params += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
        args += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
      }
      std::string body = "  %to = ptrtoint ptr " + to + " to i64\n  %from = ptrtoint ptr " + from + " to i64\n  %d = sub i64 %to, %from\n"
                         "  %this1 = getelementptr i8, ptr %this, i64 %d\n";
      if (s.returnType == "void")
        body += "  tail call void " + target + "(ptr %this1" + args + ")\n  ret void\n";
      else
        body += "  %r = tail call " + s.returnType + " " + target + "(ptr %this1" + args + ")\n  ret " + s.returnType + " %r\n";
      return "$" + name.substr(1) + " = comdat any\ndefine linkonce_odr " + s.returnType + " " + name + "(ptr %this" + params + ") comdat {\nentry:\n" + body + "}\n";
    }

    // What a pointer to a member function of class cls holds when calling f with the object is not
    // enough: a virtual f calls the object's override (through the vtable pointer of owner, the class
    // declaring f), and an f of a base at another place in cls gets `this` moved to that base (path,
    // from cls down to owner). One function per class and member, in every module.
    std::string memberFunctionThunk(const Ty* cls, const Ty* owner, const std::vector<std::pair<std::string, uint32_t>>& path, FunctionDecl* f, int slot, Node* at)
    {
      Signature s;
      std::string key;
      {
        TemplateArgs args = global.classArgsOf(owner);
        ArgsScope scope(*this, &args);
        s = signatureOf(f);
        key = methodKey(f);
      }
      if (s.variadic)
        unsupported(at, "a pointer to the variadic member function " + qualifiedName(f) + " (its thunk cannot pass the arguments on)");
      std::string target = slot < 0 ? useFunction(f, global.classArgsOf(owner), owner) : std::string();
      return sharedThunk("@\"pmf." + irString(cls) + "." + irString(owner) + "::" + key + "\"", s, pathAtNull(path), target, slot);
    }

    // The offset of the subobject a path of base fields leads to, as a pointer constant (see
    // subobjectAtNull).
    static std::string pathAtNull(const std::vector<std::pair<std::string, uint32_t>>& path)
    {
      std::string offset = "null";
      for (auto& [structName, index] : path)
        offset = "getelementptr (" + structName + ", ptr " + offset + ", i32 0, i32 " + std::to_string(index) + ")";
      return offset;
    }

    // The descriptor of a class (what slot -1 of its vtables points at, what dynamic_cast searches):
    // { name, number of direct bases, [{ base's descriptor, base's offset }] }, for any class (a
    // polymorphic class's bases need not be polymorphic). Built once, used per module (useDescriptor).
    std::string descriptorFor(const Ty* cls, Node* at)
    {
      auto lock = sharedLock();
      StructInfo& info = structFor(cls, at);
      if (!info.typeInfoName.empty())
        return info.typeInfoName;
      const Ty* key = canonicalClass(cls, at);
      std::string name = "@\"typeinfo." + classIrName(key, info) + "\"";
      info.typeInfoName = name;
      bool savedShared = emittingShared;
      emittingShared = true; // the descriptor goes into every module that uses it
      std::string nameConstant = cStringConstant(types.toString(key, &symbols));
      emittingShared = savedShared;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      std::string structName = info.name, list;
      for (auto& [baseDef, index] : bases)
        list += (list.empty() ? "" : ", ") + std::string("{ ptr, i64 } { ptr ") + descriptorFor(fieldTypes[index], at) +
                ", i64 ptrtoint (ptr getelementptr (" + structName + ", ptr null, i32 0, i32 " + std::to_string(index) + ") to i64) }";
      std::string arrayType = "[" + std::to_string(bases.size()) + " x { ptr, i64 }]";
      structFor(cls, at).descriptorText = "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr constant { ptr, i64, " + arrayType + " } { ptr " +
                                          nameConstant + ", i64 " + std::to_string(bases.size()) + ", " + arrayType + " " +
                                          (bases.empty() ? std::string("zeroinitializer") : "[" + list + "]") + " }, comdat\n";
      return name;
    }

    // A class's descriptor and its bases' in the module being emitted (each once).
    void useDescriptor(const StructInfo& info)
    {
      auto lock = sharedLock();
      if (info.descriptorText.empty() || !vtablesIn[currentModule].insert(info.typeInfoName).second)
        return;
      moduleDecls[currentModule] += info.descriptorText;
      for (auto& [baseDef, index] : std::vector<std::pair<Decl*, uint32_t>>(info.bases))
        useDescriptor(structFor(info.fieldTypes[index], nullptr));
    }

    // Identity of a method for override matching: name, parameter types and constness.
    std::string methodKey(FunctionDecl* f)
    {
      if (f->flags & DF_Destructor)
        return "~";
      std::string full = qualifiedName(f);
      size_t pos = full.rfind("::");
      std::string k = pos == std::string::npos ? full : full.substr(pos + 2);
      const Ty* ft = declType(f);
      k += "(";
      for (size_t i = 0; ft && ft->kind == TyKind::Function && i < ft->args.size(); ++i)
        k += (i ? ", " : "") + types.toString(ft->args[i], &symbols);
      k += ")";
      if (f->flags & DF_Const)
        k += " const";
      return k;
    }

    // Slot of f in the primary vtable of classTy, or -1 if f is not virtual there. A call goes
    // through the vtable pointer of the class that declares f (every virtual function of a class has
    // a slot in its primary vtable), with `this` moved to that class's subobject.
    int slotOf(const Ty* classTy, FunctionDecl* f, Node* at)
    {
      StructInfo& info = structFor(classTy, at);
      if (!info.polymorphic)
        return -1;
      TemplateArgs args = global.classArgsOf(canonicalClass(classTy, at));
      ArgsScope scope(*this, &args);
      return slotKeyed(structFor(classTy, at), methodKey(f));
    }
    static int slotKeyed(const StructInfo& info, const std::string& key)
    {
      if (info.vtables.empty())
        return -1;
      for (size_t i = 0; i < info.vtables[0].slots.size(); ++i)
        if (info.vtables[0].slots[i].key == key)
          return int(i);
      return -1;
    }

    // Loads a vtable slot of the object at addr: a function's (from 0), or the class descriptor (-1).
    std::string loadSlot(const std::string& addr, int slot)
    {
      std::string vptr = newTemp();
      fn += "  " + vptr + " = load ptr, ptr " + addr + "\n";
      std::string entry = newTemp();
      fn += "  " + entry + " = getelementptr inbounds ptr, ptr " + vptr + ", i64 " + std::to_string(slot) + "\n";
      std::string fp = newTemp();
      fn += "  " + fp + " = load ptr, ptr " + entry + "\n";
      return fp;
    }

    std::string gep(const std::string& structName, const std::string& addr, uint32_t index)
    {
      std::string r = newTemp();
      fn += "  " + r + " = getelementptr inbounds " + structName + ", ptr " + addr + ", i32 0, i32 " + std::to_string(index) + "\n";
      return r;
    }

    // The type a data member has in the object's class as laid out (under the class's arguments:
    // Holder<int&>'s `A value` is an int&), found in the class, its bases or its anonymous members;
    // null when it is none of them.
    const Ty* fieldTypeIn(const Ty* classTy, Decl* member, Node* at, int depth = 0)
    {
      if (!isClass(classTy) || depth > 16)
        return nullptr;
      StructInfo& info = structFor(classTy, at);
      if (auto it = info.fieldIndex.find(member); it != info.fieldIndex.end())
        return info.isUnion ? nullptr : info.fieldTypes[it->second];
      for (auto& [base, index] : std::vector<std::pair<Decl*, uint32_t>>(info.bases))
        if (const Ty* t = fieldTypeIn(structFor(classTy, at).fieldTypes[index], member, at, depth + 1))
          return t;
      for (auto& [anon, index] : std::vector<std::pair<Decl*, uint32_t>>(info.anonymous))
        if (const Ty* t = fieldTypeIn(structFor(classTy, at).fieldTypes[index], member, at, depth + 1))
          return t;
      return nullptr;
    }

    bool fieldAddress(const Ty* classTy, Decl* member, const std::string& objectAddr, std::string& out, Node* at)
    {
      StructInfo& info = structFor(classTy, at);
      auto it = info.fieldIndex.find(member);
      if (it != info.fieldIndex.end())
      {
        out = info.isUnion ? objectAddr : gep(info.name, objectAddr, it->second);
        return true;
      }
      // Fallback: the resolver may have a different Decl* for the same field (e.g. an out-of-line
      // definition or a using-declaration). Match by name when the pointer lookup fails.
      if (member->kind == NodeKind::VariableDecl)
      {
        auto* vd = static_cast<VariableDecl*>(member);
        if (!vd->name.empty())
        {
          uint32_t nameSymbol = vd->name.last().symbol;
          for (auto& [d, idx] : info.fieldIndex)
          {
            if (d->kind == NodeKind::VariableDecl)
            {
              auto* fd = static_cast<VariableDecl*>(d);
              if (!fd->name.empty() && fd->name.last().symbol == nameSymbol)
              {
                out = info.isUnion ? objectAddr : gep(info.name, objectAddr, idx);
                return true;
              }
            }
          }
        }
      }
      std::vector<std::pair<Decl*, uint32_t>> anonymous = info.anonymous;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      bool isUnion = info.isUnion;
      std::string name = info.name;
      Span<const Ty*> classArgs = canonicalClass(classTy, at)->args; // an anonymous member's: its class's arguments (buildStruct)
      for (auto& [anon, index] : anonymous)
      {
        std::string addr = isUnion ? objectAddr : gep(name, objectAddr, index);
        if (fieldAddress(types.classType(anon, classArgs), member, addr, out, at))
          return true;
      }
      for (auto& [baseDef, index] : bases)
      {
        std::string baseAddr = gep(name, objectAddr, index);
        if (fieldAddress(fieldTypes[index], member, baseAddr, out, at))
          return true;
      }
      return false;
    }

    // Look up a field by symbol name in a class's own members and its bases (used when the
    // resolver left MemberExpr::resolved null).  Searches the AST directly, without building
    // struct layouts, so it's safe for types that structFor would choke on.
    Decl* findFieldByName(const Ty* classTy, uint32_t nameSymbol, Node* at, int depth = 0)
    {
      if (depth > 16 || !classTy || classTy->kind != TyKind::Class || !classTy->decl)
        return nullptr;
      Decl* def = global.definitionOf(classTy->decl);
      if (!def || def->kind != NodeKind::ClassDecl)
        return nullptr;
      auto* c = static_cast<ClassDecl*>(def);
      TemplateArgs args = global.classArgsOf(classTy);
      for (Decl* m : c->members)
      {
        if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
        {
          auto* vd = static_cast<VariableDecl*>(m);
          if (!vd->name.empty() && vd->name.last().symbol == nameSymbol)
            return m;
        }
        // Search anonymous struct/union members.
        if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
          if (Decl* found = findFieldByName(types.classType(m, classTy->args), nameSymbol, at, depth + 1))
            return found;
      }
      // Search base classes.
      for (const ClassDecl::Base& b : c->bases)
      {
        const Ty* bt = global.typeOfAstIn(b.type, c, args);
        if (bt && bt->kind == TyKind::Class)
          if (Decl* found = findFieldByName(bt, nameSymbol, at, depth + 1))
            return found;
      }
      return nullptr;
    }

    // --- class metadata: constructors, destructors ---

    // The constructor overload resolution picks for a class and argument types, chosen once.
    struct ConstructorKey
    {
      const Ty* classTy;
      std::vector<const Ty*> argTypes;
      bool operator==(const ConstructorKey&) const = default;
    };
    struct ConstructorKeyHash
    {
      size_t operator()(const ConstructorKey& k) const
      {
        size_t h = std::hash<const void*>()(k.classTy);
        for (const Ty* a : k.argTypes)
          h = (h * 1000003) ^ std::hash<const void*>()(a);
        return h;
      }
    };
    std::unordered_map<ConstructorKey, FunctionDecl*, ConstructorKeyHash> constructors;

    FunctionDecl* findConstructor(const Ty* classTy, const std::vector<const Ty*>& argTypes, Node* at)
    {
      const Ty* canonical = canonicalClass(classTy, at);
      ConstructorKey key{canonical, argTypes};
      if (auto it = constructors.find(key); it != constructors.end())
        return it->second;
      auto* c = static_cast<ClassDecl*>(canonical->decl);
      TemplateArgs args = global.classArgsOf(canonical);
      std::vector<FunctionDecl*> candidates;
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
          candidates.push_back(static_cast<FunctionDecl*>(m));
      // `using Text::Text;`: the base's constructors compete too, its copy and move ones aside (C++
      // [over.match.funcs]/8) and one with an own constructor's parameters hidden by it. When one of
      // them wins, null: constructInto builds that base from the arguments (`MoveOnly direct("abc")`
      // takes Text(const char*), not the move constructor through a conversion by it).
      size_t own = candidates.size();
      auto parameterTypes = [&](FunctionDecl* f, const TemplateArgs& of)
      {
        ArgsScope scope(*this, &of);
        std::vector<const Ty*> out;
        for (Decl* p : f->params)
          out.push_back(declType(p));
        return out;
      };
      std::vector<std::vector<const Ty*>> ownParameters;
      for (FunctionDecl* f : candidates)
        ownParameters.push_back(f->templateParams.empty() ? parameterTypes(f, args) : std::vector<const Ty*>{});
      std::vector<const Ty*> chain{canonical}; // the class, then each base whose constructors it inherits
      for (int level = 0; level < 8; ++level)
      {
        std::optional<QualifiedName> base = inheritedConstructorBase(classDef(chain.back()->decl, at));
        if (!base)
          break;
        FunctionDecl::CtorInit init{};
        init.member = *base;
        const Ty* bt = initializerClass(chain.back(), init, at);
        if (!bt)
          break;
        chain.push_back(bt);
        TemplateArgs baseArgs = global.classArgsOf(bt);
        for (Decl* m : classDef(bt->decl, at)->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          std::vector<const Ty*> params = f->templateParams.empty() ? parameterTypes(f, baseArgs) : std::vector<const Ty*>{};
          const Ty* first = !params.empty() && params[0] ? types.unqualified(types.stripRef(params[0])) : nullptr;
          if (argTypes.size() == 1 && first && isClass(first) && !types.isDependent(first) &&
              std::find(chain.begin(), chain.end(), canonicalClass(first, at)) != chain.end())
            continue; // a copy or move constructor of the base, or one taking the class itself
          if (!params.empty() && std::find(ownParameters.begin(), ownParameters.end(), params) != ownParameters.end())
            continue;
          candidates.push_back(f);
        }
        args.pairs.insert(args.pairs.end(), baseArgs.pairs.begin(), baseArgs.pairs.end());
      }
      FunctionDecl* chosen = global.chooseOverload(candidates, argTypes, args, nullptr);
      if (chosen && std::find(candidates.begin() + own, candidates.end(), chosen) != candidates.end())
        chosen = nullptr;
      constructors.emplace(std::move(key), chosen);
      return chosen;
    }

    std::vector<const Ty*> argTypesOf(Span<Expr*> args)
    {
      std::vector<const Ty*> out;
      for (Expr* a : args)
      {
        if (a && isPackExpansion(a))
        {
          // `args...`: one argument per element of the pack, under the current instantiation: the
          // parameter's own element types, or for a longer pattern the elements of its first pack.
          Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
          Decl* param = packIn(a);
          if (param && pattern->kind == NodeKind::NameExpr && static_cast<NameExpr*>(pattern)->resolved == param)
          {
            for (const Local& element : packLocals[param])
              out.push_back(argumentType(pattern, element.ty, types));
            continue;
          }
          std::vector<Decl*> packs = param ? templatePacksOf(param) : std::vector<Decl*>{};
          const Ty* pack = nullptr;
          if (!packs.empty() && currentArgs)
            for (auto& [d, t] : currentArgs->pairs)
              if (d == packs[0])
                pack = t;
          if (pack && pack->kind == TyKind::Pack)
          {
            for (const Ty* element : pack->args)
              out.push_back(argumentType(a, element, types));
            continue;
          }
          // `values[I]...` over a template parameter pack: the pattern's type, once per element.
          if (Decl* valuePack = !param && currentArgs ? typePackIn(pattern) : nullptr)
            if (const Ty* bound = boundPack(valuePack); bound && bound->kind == TyKind::Pack)
            {
              for (size_t i = 0; i < bound->args.size(); ++i)
                out.push_back(argumentType(pattern, pattern->ty, types));
              continue;
            }
        }
        if (a && a->kind == NodeKind::BraceInitExpr)
        {
          out.push_back(braceListType(a, global.initializerListDecl(), types));
          continue;
        }
        out.push_back(a ? argumentType(a, a->ty, types) : nullptr);
      }
      return out;
    }

    bool isInitializerListType(const Ty* t)
    {
      Decl* d = global.initializerListDecl();
      return d && t && t->kind == TyKind::Class && t->decl == d;
    }

    // `{a, b, c}` for an InitializerList<E> parameter: the elements in a temporary array, the list
    // object (a pointer and a count) pointing at them; both live to the end of the statement.
    // variable: the address of a variable the list initializes (`InitializerList<int> v = {1, 2}`,
    // `auto names = {"a", "b"}`), whose array lives as long as it does, as C++ extends it: in the
    // function's frame for a local, in static storage for a global or a static local.
    std::string initializerListTemporary(const Ty* listTy, BraceInitExpr* b, Node* at, const std::string& variable = "")
    {
      if (listTy->args.empty())
        unsupported(at, "an InitializerList without its element type");
      const Ty* element = listTy->args[0];
      // A pack expansion among the elements (`{args...}`) gives one element per element of the pack.
      uint32_t n = 0;
      for (Expr* a : b->args)
        n += isPackExpansion(a) ? uint32_t(packElementCount(static_cast<UnaryExpr*>(a)->operand, at)) : 1;
      const Ty* arrayTy = types.array(element, n);
      bool staticArray = !variable.empty() && variable[0] == '@';
      std::string array;
      if (staticArray)
      {
        if (needsDestruction(arrayTy, at))
          unsupported(at, "an InitializerList in static storage of elements that need destruction");
        std::string owner = variable.substr(0, variable.size() - (variable.back() == '"' ? 1 : 0));
        array = owner + ".list." + std::to_string(b->token) + (variable.back() == '"' ? "\"" : "");
        addOwnedEntry(array + " = internal global " + llType(arrayTy, at) + " zeroinitializer\n");
      }
      else
        array = newObjectSlot(arrayTy, at);
      uint32_t i = 0;
      auto initializeElement = [&](Expr* value)
      {
        std::string slot = newTemp();
        fn += "  " + slot + " = getelementptr inbounds " + llType(arrayTy, at) + ", ptr " + array + ", i64 0, i64 " + std::to_string(i++) + "\n";
        initializeFrom(slot, element, value, at);
      };
      for (Expr* a : b->args)
      {
        if (isPackExpansion(a))
        {
          Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
          forEachPackElement(pattern, at, [&](size_t) { initializeElement(pattern); });
        }
        else
          initializeElement(a);
      }
      if (!variable.empty() && !staticArray && needsDestruction(arrayTy, at))
        scopes.back().push_back({array, arrayTy}); // destroyed with the variable's scope
      else if (variable.empty() && needsDestruction(arrayTy, at))
        statementTemps.push_back({array, arrayTy});
      std::string list = variable.empty() ? newObjectSlot(listTy, at) : variable;
      const StructInfo& info = structFor(listTy, at);
      if (info.fieldDecls.size() < 2)
        unsupported(at, "an InitializerList class without its two fields");
      fn += "  store ptr " + array + ", ptr " + gep(info.name, list, fieldIndexOf(info, info.fieldDecls[0])) + "\n";
      fn += "  store i32 " + std::to_string(n) + ", ptr " + gep(info.name, list, fieldIndexOf(info, info.fieldDecls[1])) + "\n";
      return list;
    }

    // Default-constructs every element of an array of objects at addr: one after the other for a
    // few, in a loop for more (an array of a thousand would otherwise be a thousand copies).
    void constructArray(const std::string& addr, const Ty* t, Node* at)
    {
      auto constructAt = [&](const std::string& index)
      {
        std::string element = newTemp();
        fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + index + "\n";
        if (isClass(t->inner))
          defaultConstruct(element, t->inner, at);
        else if (t->inner->kind == TyKind::Array)
          constructArray(element, t->inner, at);
        else
          storeZero(t->inner, element, at);
      };
      if (t->count <= 8)
      {
        for (uint32_t i = 0; i < t->count; ++i)
          constructAt(std::to_string(i));
        return;
      }
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i64\n";
      fn += "  store i64 0, ptr " + slot + "\n";
      std::string loopL = newLabel("array.loop"), bodyL = newLabel("array.body"), endL = newLabel("array.end");
      emitBranch(loopL);
      startBlock(loopL);
      std::string index = newTemp(), done = newTemp();
      fn += "  " + index + " = load i64, ptr " + slot + "\n";
      fn += "  " + done + " = icmp eq i64 " + index + ", " + std::to_string(t->count) + "\n";
      fn += "  br i1 " + done + ", label %" + endL + ", label %" + bodyL + "\n";
      startBlock(bodyL);
      constructAt(index);
      std::string next = newTemp();
      fn += "  " + next + " = add i64 " + index + ", 1\n";
      fn += "  store i64 " + next + ", ptr " + slot + "\n";
      emitBranch(loopL);
      startBlock(endL);
    }

    // A destructor written by the user; `~T() = default` is the compiler's memberwise one.
    FunctionDecl* findDestructor(ClassDecl* c)
    {
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) && !(m->flags & DF_Default))
          return static_cast<FunctionDecl*>(m);
      return nullptr;
    }

    // Constructors written by the user (`= default` ones are the compiler's memberwise ones).
    bool hasUserConstructors(ClassDecl* c)
    {
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Default))
          return true;
      return false;
    }

    // Default construction does something: user constructors, a vtable pointer, default member
    // initializers, or a base/member class that needs constructing (a zeroed global is not enough).
    bool needsConstruction(const Ty* t, Node* at, int depth = 0)
    {
      t = types.stripRef(t);
      if (t->kind == TyKind::Array)
        return needsConstruction(t->inner, at, depth);
      if (!isClass(t) || !t->decl || depth > 16)
        return false;
      if (hasUserConstructors(classDef(t->decl, at)))
        return true;
      const StructInfo& info = structFor(t, at);
      if (info.polymorphic)
        return true;
      for (Decl* m : info.fieldDecls)
        if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->init)
          return true;
      for (const Ty* ft : info.fieldTypes)
        if (needsConstruction(ft, at, depth + 1))
          return true;
      return false;
    }

    bool needsDestruction(const Ty* t, Node* at)
    {
      t = types.stripRef(t);
      if (t->kind == TyKind::Array)
        return needsDestruction(t->inner, at);
      if (isLambda(t))
      {
        for (auto& [index, ft] : closureValueFields(t, at))
          if (needsDestruction(ft, at))
            return true;
        return false;
      }
      if (!isClass(t))
        return false;
      if (auto mine = destructionCache.find(t); mine != destructionCache.end())
        return mine->second;
      auto lock = sharedLock(); // remembered in the shared layout, computed under the lock
      StructInfo& info = structFor(t, at);
      if (info.needsDestruction)
      {
        // Final unless this emitter is in the middle of computing it (the recursion guard below):
        // nobody else computes while the lock is held.
        if (destructionDepth == 0)
          destructionCache.emplace(t, *info.needsDestruction);
        return *info.needsDestruction;
      }
      info.needsDestruction = false; // Guard against recursion.
      struct Depth { int& d; Depth(int& d) : d(d) { ++d; } ~Depth() { --d; } } depth(destructionDepth);
      ClassDecl* c = classDef(t->decl, at);
      if (c->isTypedUnion)
      {
        // A typed union destroys whichever alternative it holds.
        bool any = false;
        for (size_t i = 0; i < c->alternatives.size(); ++i)
          any = any || needsDestruction(alternativeType(c, t, i, at), at);
        structFor(t, at).needsDestruction = any;
        return any;
      }
      bool result = findDestructor(c) != nullptr;
      for (const Ty* ft : std::vector<const Ty*>(info.fieldTypes))
        result = result || (!ft->isReference() && needsDestruction(ft, at)); // a reference member owns nothing
      structFor(t, at).needsDestruction = result;
      return result;
    }
    std::unordered_map<const Ty*, bool> destructionCache; // this emitter's view of the answers above
    int destructionDepth = 0;

    // Destroys the object at addr: user destructor if any (it destroys members itself), else
    // members and bases that need it, in reverse order.
    void destroy(const std::string& addr, const Ty* t, Node* at, bool dynamic = false)
    {
      t = types.stripRef(t);
      if (t->kind == TyKind::Array)
      {
        if (!needsDestruction(t->inner, at))
          return;
        for (uint32_t i = t->count; i-- > 0;)
        {
          std::string e = newTemp();
          fn += "  " + e + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(i) + "\n";
          destroy(e, t->inner, at);
        }
        return;
      }
      if (isLambda(t))
      {
        std::vector<std::pair<uint32_t, const Ty*>> fields = closureValueFields(t, at);
        std::string structName = closureFor(t, at).structName;
        for (size_t i = fields.size(); i-- > 0;)
          if (needsDestruction(fields[i].second, at))
            destroy(gep(structName, addr, fields[i].first), fields[i].second, at);
        return;
      }
      if (!isClass(t))
        return;
      if (dynamic)
      {
        // A virtual destructor: the dynamic class's, also when this class has nothing to destroy
        // (`virtual ~Shape() = default`) and the derived one does.
        if (int slot = slotKeyed(structFor(t, at), "~"); slot >= 0)
        {
          std::string fp = loadSlot(addr, slot);
          emitCall("", "void " + fp + "(ptr " + addr + ")", false);
          return;
        }
      }
      if (!needsDestruction(t, at))
        return;
      ClassDecl* c = classDef(t->decl, at);
      if (c->isTypedUnion)
      {
        destroyAlternative(c, t, addr, at);
        return;
      }
      if (FunctionDecl* dtor = findDestructor(c))
      {
        TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
        emitCall("", "void " + useFunction(dtor, args, canonicalClass(t, at)) + "(ptr " + addr + ")", false);
        return;
      }
      destroyMembers(addr, t, at);
    }

    void destroyMembers(const std::string& addr, const Ty* t, Node* at)
    {
      StructInfo& info = structFor(t, at);
      std::string name = info.name;
      std::vector<Decl*> fieldDecls = info.fieldDecls;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      std::unordered_map<Decl*, uint32_t> fieldIndex = info.fieldIndex;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      for (size_t i = fieldDecls.size(); i-- > 0;)
      {
        const Ty* ft = fieldTypes[fieldIndex[fieldDecls[i]]];
        if (!ft->isReference() && needsDestruction(ft, at))
          destroy(gep(name, addr, fieldIndex[fieldDecls[i]]), ft, at);
      }
      for (size_t i = bases.size(); i-- > 0;)
      {
        const Ty* bt = fieldTypes[bases[i].second];
        if (needsDestruction(bt, at))
          destroy(gep(name, addr, bases[i].second), bt, at);
      }
    }

    // Default construction: zero, then default member initializers, then class members' and
    // bases' constructors.
    void defaultConstruct(const std::string& addr, const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (c->isTypedUnion)
      {
        // `Property p;`: the first alternative, default-constructed.
        storeZero(t, addr, at);
        if (!c->alternatives.empty())
          constructAlternative(c, t, addr, 0, nullptr, at);
        return;
      }
      if (hasUserConstructors(c))
      {
        FunctionDecl* ctor = findConstructor(t, {}, at);
        if (!ctor && !inheritedConstructorBase(c))
          unsupported(at, "default construction of " + qualifiedName(c) + ", which has no default constructor");
        if (ctor && !(ctor->flags & DF_Default))
        {
          // Through callFunction: `Style(const Style* parent = nullptr)` wants its default argument.
          TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
          if (!ctor->templateParams.empty())
            global.deduceArgs(ctor, {}, args); // `template<typename... Args> OnHeap(Args&&...)`: Args is empty
          callFunction(ctor, args, canonicalClass(t, at), addr, "", {}, at);
          return;
        }
        // `T() = default;` next to other constructors: the memberwise default construction below.
      }
      storeZero(t, addr, at);
      initializeMembers(addr, t, {}, at);
    }

    // The class a constructor initializer of class t names (`super(v)` with `using super = ...`,
    // `Chain<Tail...>(tail...)`): its name converted as a type in the class under the class's
    // arguments; null for a member's name or one that names no class there.
    const Ty* initializerClass(const Ty* t, const FunctionDecl::CtorInit& init, Node* at)
    {
      if (init.member.empty())
        return nullptr;
      const Ty* cls = canonicalClass(t, at);
      ClassDecl* c = classDef(cls->decl, at);
      NamedType named{};
      named.kind = NodeKind::NamedType;
      named.token = at ? at->token : 0;
      named.name = init.member;
      // Looked up from inside the class: from the constructor when it is the one being emitted.
      Decl* context = currentFunction && global.owningClass(currentFunction) && classDef(global.owningClass(currentFunction), at) == c ? static_cast<Decl*>(currentFunction) : c;
      const Ty* nt = global.typeOfAstIn(&named, context, global.classArgsOf(cls));
      return nt && isClass(nt) && !types.isDependent(nt) ? canonicalClass(nt, at) : nullptr;
    }

    // Whether a constructor initializer written with template arguments names another class than
    // cls: `Chain<Tail...>(tail...)` in Chain<Head, Tail...> (its base, an instance of the same
    // template), `Holder<1, T>(b)` beside a base Holder<0, H>.
    bool initializerNamesOther(const Ty* t, const FunctionDecl::CtorInit& init, const Ty* cls, Node* at)
    {
      // Only a name with template arguments or a qualified one can name another class than its
      // last part spells: `Chain<Tail...>(...)`, `UnitTest::TestRunner(reporter)` in a TestRunner.
      if (init.member.empty() || (!init.member.last().hasTemplateArgs && init.member.parts.size() == 1))
        return false;
      const Ty* named = initializerClass(t, init, at);
      return named && named != canonicalClass(cls, at);
    }

    // The constructor initializer that names base `baseTy` of class t by its type (through an
    // alias, `using super = Conditional<...>; Const(const T& v) : super(v) {}`, or one of several
    // instances of a template); null if none does.
    const FunctionDecl::CtorInit* initializerNamingBase(const Ty* t, const Ty* baseTy, Span<FunctionDecl::CtorInit> inits, Node* at)
    {
      const Ty* base = canonicalClass(baseTy, at);
      for (const FunctionDecl::CtorInit& init : inits)
        if (initializerClass(t, init, at) == base)
          return &init;
      return nullptr;
    }

    // Runs emit with the default member initializer of v, a member of the class t whose object is
    // at addr: the initializer as typed for t's instantiation, with `this` the object, t the class
    // of the moment and its arguments the current ones. A base's `Hook* next = this;` refers to the
    // base subobject, also when a derived class's implicit constructor is emitted into a caller.
    template<typename F>
    void withMemberInitializer(const std::string& addr, const Ty* t, VariableDecl* v, F&& emit)
    {
      const Ty* cls = canonicalClass(t, v);
      TemplateArgs classArgs = global.classArgsOf(cls);
      Expr* init = global.instanceInitializer(v, classArgs);
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca ptr\n";
      fn += "  store ptr " + addr + ", ptr " + slot + "\n";
      struct Restore
      {
        Emitter& e;
        std::optional<Local> self;
        const Ty* classTy;
        const TemplateArgs* args;
        ParsedFile* file;
        Decl* initializer;
        ~Restore()
        {
          if (self)
            e.locals[nullptr] = *self;
          else
            e.locals.erase(nullptr);
          e.currentClassTy = classTy;
          e.currentArgs = args;
          e.currentFile = file;
          e.initializerOf = initializer;
        }
      } restore{*this, locals.count(nullptr) ? std::optional<Local>(locals[nullptr]) : std::nullopt, currentClassTy, currentArgs, currentFile, initializerOf};
      locals[nullptr] = {slot, nullptr, false};
      currentClassTy = cls;
      currentArgs = &classArgs;
      currentFile = v->file;
      initializerOf = v;
      emit(init);
    }

    // A reference member (of type refTy) bound to what source names: the field holds its address, of
    // its base subobject for a reference to a base (EntityAndIndex's `UpdatableEntity& entity` from an
    // InfinityContainer, whose UpdatableEntity comes after its LogisticContainer).
    void bindReferenceMember(const std::string& fieldAddr, const Ty* refTy, Expr* source, Node* at)
    {
      withSingleElement(source, at, [&](Expr* one)
      {
        std::string target;
        if (isClass(types.stripRef(one->ty)))
        {
          Value object = rvalue(one);
          target = object.text;
          const Ty* referenced = refTy ? types.unqualified(types.stripRef(refTy)) : nullptr;
          if (referenced && isClass(referenced) && !types.isDependent(referenced) && isClass(object.ty) &&
              canonicalClass(object.ty, at) != canonicalClass(referenced, at))
            target = convert(object, referenced, at).text; // to the base, or through a conversion operator
        }
        else
          target = lvalue(one);
        fn += "  store ptr " + target + ", ptr " + fieldAddr + "\n";
      });
    }

    // Points the vtable pointer of the object at addr at its class's vtable: once its bases are
    // built (their constructors pointed it at theirs), and after its bytes were copied from an
    // object that may be of a class derived from it.
    void storeVptrs(const std::string& addr, const StructInfo& info)
    {
      if (!info.polymorphic)
        return;
      useVtable(info);
      std::vector<std::string> where = vptrAddresses(addr, info);
      for (size_t i = 0; i < where.size(); ++i)
        fn += "  store ptr " + info.vtables[i].vptrValue + ", ptr " + where[i] + "\n";
    }

    // The vtable pointers of the object at addr, loaded before an assignment copies its bytes and
    // stored back after it: an object keeps its own class's vtables (it may be of a class derived
    // from the one assigned).
    std::vector<std::string> loadVptrs(const std::string& addr, const StructInfo& info)
    {
      std::vector<std::string> saved; // pairs: the value, then where it goes back
      if (!info.polymorphic)
        return saved;
      for (const std::string& at : vptrAddresses(addr, info))
      {
        saved.push_back(newTemp());
        fn += "  " + saved.back() + " = load ptr, ptr " + at + "\n";
        saved.push_back(at);
      }
      return saved;
    }
    void restoreVptrs(const std::vector<std::string>& saved)
    {
      for (size_t i = 0; i + 1 < saved.size(); i += 2)
        fn += "  store ptr " + saved[i] + ", ptr " + saved[i + 1] + "\n";
    }

    // Where the vtable pointers of the object at addr are, in the order of its class's vtables.
    std::vector<std::string> vptrAddresses(const std::string& addr, const StructInfo& info)
    {
      std::vector<std::string> out;
      for (const Vtable& t : info.vtables)
      {
        std::string a = addr;
        const StructInfo* s = &info;
        for (uint32_t index : t.path)
        {
          a = gep(s->name, a, index);
          s = &structFor(s->fieldTypes[index], nullptr);
        }
        out.push_back(a);
      }
      return out;
    }

    // Initialises bases and members of the object at addr from constructor initializers (or
    // default member initializers / default construction when absent).
    // built: a constructor's own initialization: each base and member that needs destruction is
    // destroyed if an exception leaves the constructor after it was built (a later member's
    // initializer or the body throwing), as C++ does.
    void initializeMembers(const std::string& addr, const Ty* t, Span<FunctionDecl::CtorInit> inits, Node* at, bool built = false)
    {
      const StructInfo& info = structFor(t, at);
      auto builtPart = [&](const std::string& part, const Ty* pt)
      {
        if (built && needsDestruction(pt, at))
        {
          Object o{part, pt};
          o.unwindOnly = true;
          scopes.back().push_back(o);
        }
      };
      auto initFor = [&](uint32_t symbol) -> const FunctionDecl::CtorInit*
      {
        for (const FunctionDecl::CtorInit& init : inits)
          if (!init.member.empty() && init.member.last().symbol == symbol)
            return &init;
        return nullptr;
      };
      if (isClass(t) && t->decl)
      {
        // `Frame(x) : Frame(x, "") {}`: a delegating constructor builds the whole object (not
        // `Chain<Tail...>(tail...)` in Chain<Head, Tail...>, which initializes its base).
        const QualifiedName& own = classDef(t->decl, at)->name;
        if (const FunctionDecl::CtorInit* self = own.empty() ? nullptr : initFor(own.last().symbol); self && !initializerNamesOther(t, *self, t, at))
        {
          constructInto(addr, t, self->args, at);
          return;
        }
      }
      for (auto& [baseDef, index] : info.bases)
      {
        const Ty* bt = info.fieldTypes[index];
        std::string baseAddr = gep(info.name, addr, index);
        const QualifiedName& bn = static_cast<ClassDecl*>(baseDef)->name;
        const FunctionDecl::CtorInit* init = bn.empty() ? nullptr : initFor(bn.last().symbol);
        if (init && initializerNamesOther(t, *init, bt, at))
          init = nullptr; // `Holder<0, H>(a)` met first for the base Holder<1, T>: by the type each names
        if (!init)
          init = initializerNamingBase(t, bt, inits, at);
        if (init)
          constructInto(baseAddr, bt, init->args, at);
        else if (hasUserConstructors(static_cast<ClassDecl*>(baseDef)))
          defaultConstruct(baseAddr, bt, at);
        else
          initializeMembers(baseAddr, bt, {}, at);
        builtPart(baseAddr, bt);
      }
      storeVptrs(addr, info);
      if (info.isUnion)
      {
        // A union with no member named by a constructor initializer: its member with a default
        // member initializer is the one initialized, as in C++ (`union { int64_t raw = 0; ... }`).
        bool named = false;
        for (Decl* d : info.fieldDecls)
          if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
            named = named || initFor(static_cast<VariableDecl*>(d)->name.last().symbol);
        // The member a constructor initializer names, at the union's start (UpgradableEntity's
        // `WrappedValue(EntityWithHealth& wrapped) : wrappedEntityWithHealth(&wrapped) {}`).
        if (named)
          for (Decl* d : info.fieldDecls)
            if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
              if (const FunctionDecl::CtorInit* init = initFor(static_cast<VariableDecl*>(d)->name.last().symbol))
              {
                const Ty* ft = unionMemberType(t, d, at);
                if (isClass(ft))
                  constructInto(addr, ft, init->args, at);
                else if (init->args.empty())
                  storeZero(ft, addr, at); // `: raw()`: value-initialized
                else if (init->args.size() == 1)
                  withSingleElement(init->args[0], at, [&](Expr* one) { initializeFrom(addr, ft, one, at); });
                else
                  unsupported(at, "a union member initializer of several values");
                break;
              }
        if (!named)
          for (Decl* d : info.fieldDecls)
            if (d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->init && !bitfieldFor(d))
            {
              auto* v = static_cast<VariableDecl*>(d);
              const Ty* ft = unionMemberType(t, d, at);
              if (const Ty* declared = global.typeOfDecl(v); !ft->isReference() && !(declared && declared->isReference()))
                withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(addr, ft, value, v); });
              break;
            }
        return;
      }
      std::set<size_t> zeroedUnits; // the storage units of bit fields, each zeroed at its first member
      for (Decl* d : info.fieldDecls)
      {
        if (d->kind == NodeKind::ClassDecl)
        {
          // An anonymous struct or union member (`union { SharedString sharedStr; LongStr longStr;
          // ... }` with `: longStr{nullptr, 0}`): its members are initialised as the class's own,
          // by the same constructor initializers.
          initializeAnonymous(gep(info.name, addr, fieldIndexOf(info, d)), info.fieldTypes[fieldIndexOf(info, d)], static_cast<ClassDecl*>(d),
                              inits, at);
          continue;
        }
        if (d->kind != NodeKind::VariableDecl)
          continue;
        auto* v = static_cast<VariableDecl*>(d);
        const Ty* ft = info.fieldTypes[fieldIndexOf(info, d)];
        std::string fieldAddr = gep(info.name, addr, fieldIndexOf(info, d));
        const FunctionDecl::CtorInit* init = v->name.empty() ? nullptr : initFor(v->name.last().symbol);
        if (bitfieldFor(d))
        {
          // The unit's first bit field stands for the unit: each bit field in it is initialised.
          for (Decl* m : unitMembers(info, classDef(t->decl, at), d))
          {
            auto* bv = static_cast<VariableDecl*>(m);
            const FunctionDecl::CtorInit* bitInit = bv->name.empty() ? nullptr : initFor(bv->name.last().symbol);
            if (bitInit && bitInit->args.size() > 1)
              unsupported(at, "this member initializer");
            initializeBitfield(addr, t, info, bv, *bitfieldFor(m), bitInit != nullptr, bitInit && bitInit->args.size() == 1 ? bitInit->args[0] : nullptr,
                               zeroedUnits, at);
          }
          continue;
        }
        if (const Ty* declared = global.typeOfDecl(v); ft->isReference() || (declared && declared->isReference()))
        {
          // A reference member: the field holds the address of what it refers to.
          if (!init && v->init)
          {
            withMemberInitializer(addr, t, v, [&](Expr* source) { bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, source, at); });
            continue;
          }
          Expr* source = init && init->args.size() == 1 ? init->args[0] : nullptr;
          if (!source)
            unsupported(at, "a reference member without an initializer");
          bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, source, at);
          continue;
        }
        if (init)
        {
          if (isClass(ft))
            constructInto(fieldAddr, ft, init->args, at);
          else if (isLambda(ft) && init->args.size() == 1)
            withSingleElement(init->args[0], at, [&](Expr* one) { initializeFrom(fieldAddr, ft, one, at); }); // `f(f)`: the closure's bytes, not its address
          else if (size_t values = expandedCount(init->args, at); values == 0)
            storeZero(ft, fieldAddr, at); // `value()`, `value(forward<Args>(args)...)` with Args empty: value-initialized
          else if (values == 1)
            forEachExpanded(init->args, at, [&](Expr* one)
            {
              // A member initializer is direct-initialization: a class value converts through an
              // explicit conversion operator too (Pair's `second(forward<U2>(second))` with
              // ParsedNoiseExpression's `explicit operator const NoiseExpression*()`).
              Value value = rvalue(one);
              if (isClass(value.ty) && !isClass(ft) && ft->kind != TyKind::Lambda && !ft->isReference())
                value = convert(value, ft, at, true);
              store(value, fieldAddr, ft, at);
            }); // `value(first, args...)`, args empty
          else
            unsupported(at, "this member initializer");
        }
        else if (v->init)
          withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(fieldAddr, ft, value, v); });
        else if (isClass(ft) && hasUserConstructors(classDef(ft->decl, at)))
          defaultConstruct(fieldAddr, ft, at);
        else if (isClass(ft))
          initializeMembers(fieldAddr, ft, {}, at);
        else if (ft->kind == TyKind::Array && (needsDestruction(ft, at) || needsConstruction(ft, at))) // elements with constructors or default member initializers
          constructArray(fieldAddr, ft, at);
        else
          storeZero(ft, fieldAddr, at);
        builtPart(fieldAddr, ft);
      }
    }

    // Whether a constructor initializer names a data member of the anonymous struct or union c (or
    // of one nested in it).
    static bool namesMemberOf(ClassDecl* c, Span<FunctionDecl::CtorInit> inits)
    {
      for (Decl* m : c->members)
      {
        if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
        {
          if (namesMemberOf(static_cast<ClassDecl*>(m), inits))
            return true;
          continue;
        }
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)) || static_cast<VariableDecl*>(m)->name.empty())
          continue;
        for (const FunctionDecl::CtorInit& init : inits)
          if (!init.member.empty() && init.member.last().symbol == static_cast<VariableDecl*>(m)->name.last().symbol)
            return true;
      }
      return false;
    }

    // An anonymous struct or union member of a class being constructed, at addr, of type t (c its
    // declaration). A struct: its members as the class's own (initializeMembers). A union: zeroed
    // (members are zero-initialised in Kpp), then the member a constructor initializer names, or
    // else the one with a default member initializer, is initialised: C++ initialises one member.
    void initializeAnonymous(const std::string& addr, const Ty* t, ClassDecl* c, Span<FunctionDecl::CtorInit> inits, Node* at)
    {
      if (!structFor(t, at).isUnion)
      {
        initializeMembers(addr, t, inits, at);
        return;
      }
      storeZero(t, addr, at);
      Decl* chosen = nullptr;
      for (Decl* m : c->members)
        if (((m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef))) ||
             (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)) &&
            !chosen)
        {
          if (m->kind == NodeKind::ClassDecl ? namesMemberOf(static_cast<ClassDecl*>(m), inits) : false)
            chosen = m;
          else if (m->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(m)->name.empty())
            for (const FunctionDecl::CtorInit& init : inits)
              if (!init.member.empty() && init.member.last().symbol == static_cast<VariableDecl*>(m)->name.last().symbol)
                chosen = m;
        }
      if (!chosen)
        for (Decl* m : c->members)
          if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)) && static_cast<VariableDecl*>(m)->init && !chosen)
            chosen = m;
      if (!chosen)
        return;
      const Ty* mt = unionMemberType(t, chosen, at);
      if (chosen->kind == NodeKind::ClassDecl)
      {
        initializeAnonymous(addr, mt, static_cast<ClassDecl*>(chosen), inits, at); // a struct inside the union, at its start
        return;
      }
      auto* v = static_cast<VariableDecl*>(chosen);
      if (bitfieldFor(v))
        unsupported(at, "a bit field of an anonymous union initialised by a constructor");
      const FunctionDecl::CtorInit* init = nullptr;
      for (const FunctionDecl::CtorInit& i : inits)
        if (!i.member.empty() && i.member.last().symbol == v->name.last().symbol)
          init = &i;
      if (const Ty* declared = global.typeOfDecl(v); mt->isReference() || (declared && declared->isReference()))
        unsupported(at, "a reference member of an anonymous union");
      if (!init)
      {
        withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(addr, mt, value, v); });
        return;
      }
      if (isClass(mt))
        constructInto(addr, mt, init->args, at);
      else if (size_t values = expandedCount(init->args, at); values == 0)
        storeZero(mt, addr, at);
      else if (values == 1)
        forEachExpanded(init->args, at, [&](Expr* one) { initializeFrom(addr, mt, one, at); });
      else
        unsupported(at, "this member initializer");
    }

    // The type of member m of union t (every member at the union's start, field 0 standing for none
    // of them): a data member's, or an anonymous struct's with the union's arguments.
    const Ty* unionMemberType(const Ty* t, Decl* m, Node* at)
    {
      const Ty* key = canonicalClass(t, at);
      if (m->kind == NodeKind::VariableDecl)
        return global.typeOfDeclIn(m, global.classArgsOf(key));
      return types.classType(m, key->args);
    }

    // Whether every element of the brace list converts to the element type of the InitializerList
    // constructor ctor of class t: only then is ctor the one, otherwise the other constructors take
    // the elements (C++'s two phases: `Vector<int*> v{set.begin(), set.end()}` is the range). An
    // element of unknown type, or a list itself, counts as converting.
    bool listConvertsTo(FunctionDecl* ctor, BraceInitExpr* list, const Ty* t, Node* at)
    {
      TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &classArgs);
      const Ty* pt = declType(ctor->params[0]);
      pt = pt ? types.unqualified(types.stripRef(pt)) : nullptr;
      if (!pt || pt->args.empty())
        return true;
      const Ty* element = pt->args[0];
      for (Expr* a : list->args)
      {
        // A list as an element (`Vector<ID> ids{{ID(1), ID(2), ID(3)}}`): one only when the element
        // type is list-initialized from it; otherwise the list is the InitializerList itself, the
        // argument of the second phase.
        if (a && a->kind == NodeKind::BraceInitExpr && !types.isDependent(element))
          if (const Ty* inner = braceListType(a, global.initializerListDecl(), types); inner && !global.converts(inner, element))
            return false;
        if (!a || a->kind == NodeKind::BraceInitExpr || isPackExpansion(a) || !a->ty || !a->ty->isKnown())
          continue;
        const Ty* at = withCurrentArgs(a->ty);
        if (types.isDependent(at) || types.isDependent(element))
          continue;
        if (!global.converts(argumentType(a, at, types), element))
          return false;
      }
      return true;
    }

    // The constructor of class t that takes an InitializerList (the rest defaulted), or null.
    FunctionDecl* initializerListConstructor(ClassDecl* c, const Ty* t, Node* at)
    {
      TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &classArgs);
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || (m->flags & DF_Delete))
          continue;
        auto* f = static_cast<FunctionDecl*>(m);
        if (f->params.empty() || !f->templateParams.empty())
          continue;
        bool othersDefaulted = true;
        for (size_t i = 1; i < f->params.size(); ++i)
          othersDefaulted = othersDefaulted && static_cast<ParamDecl*>(f->params[i])->defaultValue != nullptr;
        if (!othersDefaulted)
          continue;
        const Ty* pt = declType(f->params[0]);
        if (pt && isInitializerListType(types.stripRef(pt)))
          return f;
      }
      return nullptr;
    }

    // Constructs an object of class type t at addr from constructor arguments.
    // (class, argument) of the one-argument constructions under way: a constructor whose parameter
    // needs the same conversion again would otherwise recurse until the stack runs out.
    std::vector<std::pair<const Ty*, Expr*>> constructing;
    struct ConstructingScope
    {
      std::vector<std::pair<const Ty*, Expr*>>& list;
      ConstructingScope(std::vector<std::pair<const Ty*, Expr*>>& list, const Ty* t, Expr* arg) : list(list) { list.push_back({t, arg}); }
      ~ConstructingScope() { list.pop_back(); }
    };

    // `T(x)` of class type t into addr: the constructors take the arguments; and, as
    // direct-initialization lets the copy or move constructor take what an explicit conversion
    // operator gives (C++'s rule for a constructor's first parameter), `Constant(parsed)` with
    // Parsed's `explicit operator const Constant&()` and `AchievementId(id)` with `explicit operator
    // AchievementId()` when no constructor takes the object itself.
    void directInitialize(const std::string& addr, const Ty* t, Span<Expr*> args, bool braced, Node* at, bool listElements)
    {
      if (braced && isInitializerListType(types.unqualified(t)))
      {
        // `InitializerList<TestCase>{{...}, {...}}` (a range-for's range in the tests): the list
        // over its elements' array, which lives as long as the object made here does.
        if (!listElements && args.size() == 1 && args[0] && args[0]->kind == NodeKind::BraceInitExpr)
        {
          initializerListTemporary(types.unqualified(t), static_cast<BraceInitExpr*>(args[0]), at, addr);
          return;
        }
        BraceInitExpr list{};
        list.kind = NodeKind::BraceInitExpr;
        list.token = at ? at->token : 0;
        list.args = args;
        initializerListTemporary(types.unqualified(t), &list, at, addr);
        return;
      }
      Expr* arg = args.size() == 1 && !braced ? args[0] : nullptr;
      const Ty* argTy = arg && arg->ty && arg->ty->isKnown() ? withCurrentArgs(types.unqualified(types.stripRef(arg->ty))) : nullptr;
      if (argTy && isClass(argTy) && !types.isDependent(argTy) && classKey(argTy) != classKey(t) && !baseInstantiation(argTy, classDef(t->decl, at), at) &&
          !findConstructor(t, argTypesOf(args), at) && !global.converts(argumentType(arg, arg->ty, types), types.unqualified(t)))
      {
        // The operator gives the class (`operator AchievementId()`) or a reference to it (`operator
        // const Constant&()`): either way the copy or move constructor's argument.
        const Ty* value = types.unqualified(t);
        const Ty* reference = types.lref(types.qualified(value, true, false));
        const Ty* target = conversionOperator(argTy, value, at, nullptr, true) ? value : conversionOperator(argTy, reference, at, nullptr, true) ? reference : nullptr;
        if (target)
        {
          Value converted = convert(rvalue(arg), target, at, true);
          if (converted.isTemporary)
            moveTemporary(addr, converted, value, at);
          else
            copyConstruct(addr, converted.text, value, at);
          return;
        }
      }
      constructInto(addr, t, args, at, listElements);
    }

    // listElements: args are a brace list's elements (the list taken apart once already).
    void constructInto(const std::string& addr, const Ty* t, Span<Expr*> args, Node* at, bool listElements = false)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (args.empty())
      {
        defaultConstruct(addr, t, at);
        return;
      }
      std::optional<ConstructingScope> guard;
      if (args.size() == 1)
      {
        const Ty* key = canonicalClass(t, at);
        for (auto& [k, a] : constructing)
          if (k == key && a == args[0])
            unsupported(at, "converting " + (args[0]->ty ? types.toString(args[0]->ty, &symbols) : std::string("a value")) + " to " +
                                types.toString(key, &symbols) + ": the constructor chosen needs the same conversion for its own parameter");
        guard.emplace(constructing, key, args[0]);
      }
      // (`return {{a, b}};` for an Optional<Pair<int, int>>: the inner list, one element of the
      // outer one, is an argument of a constructor, not the class's own list again; nor is the
      // one value of `{.effectTexture = {bitmap}}`, which is the designated member's.)
      if (args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr && !(listElements && hasUserConstructors(c)) &&
          !(listElements && !pendingDesignators.empty()))
      {
        // `T x{y}` with y a T, or of a class derived from T: a copy or a move of y, before any
        // initializer-list constructor (C++ DR 1467): `[names{::move(names)}]` moves the Vector.
        auto* list = static_cast<BraceInitExpr*>(args[0]);
        if (list->args.size() == 1 && list->designators.empty() && list->args[0]->kind != NodeKind::BraceInitExpr && list->args[0]->ty)
        {
          const Ty* et = withCurrentArgs(types.stripRef(list->args[0]->ty));
          std::vector<std::pair<std::string, uint32_t>> path;
          if (et && isClass(et) && !types.isDependent(et) &&
              (canonicalClass(et, at) == canonicalClass(t, at) || basePath(et, t, path, at)))
          {
            constructInto(addr, t, list->args, at);
            return;
          }
        }
        // `Vector<Item> items = {}` of a class with a default constructor: value-initialisation (C++
        // [dcl.init.list] 3.5), not the InitializerList constructor, whose body copies elements (a
        // Vector of a move-only class: CommandProcessor's `Vector<SavedSpecialItemReference> ... = {}`).
        bool valueInitialised = list->args.empty() && findConstructor(t, {}, at);
        if (FunctionDecl* ctor = valueInitialised ? nullptr : initializerListConstructor(c, t, at); ctor && listConvertsTo(ctor, list, t, at))
        {
          // `Vector<int> v = {1, 2, 3}`, `Poly p({1, 2, 3, 4})`: the list as one InitializerList argument.
          TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
          callFunction(ctor, classArgs, canonicalClass(t, at), addr, "", args, at);
          return;
        }
        pendingDesignators = static_cast<BraceInitExpr*>(args[0])->designators;
        constructInto(addr, t, static_cast<BraceInitExpr*>(args[0])->args, at, true);
        return;
      }
      Span<uint32_t> designators = pendingDesignators;
      pendingDesignators = {};
      if (c->isTypedUnion && args.size() == 1 && args[0]->ty && args[0]->ty->isKnown() && !isPackExpansion(args[0])) // an expansion: its element, below
      {
        // `Shape a(s)`, `Shape(7)`, a member initializer `shape(s)`, `new (slot) Shape(x)`: as
        // copy-initialization, a value of the union itself copies or moves it, any other the
        // alternative of its type (design.md 8.5).
        const Ty* argTy = types.stripRef(args[0]->ty);
        if (!(isClass(argTy) && classKey(argTy) == classKey(t)))
        {
          storeZero(t, addr, at);
          constructAlternative(c, t, addr, alternativeForValue(c, t, args[0]->ty, at), args[0], at);
          return;
        }
        initializeFrom(addr, t, args[0], at);
        return;
      }
      // `using Base::Base;`: the base's constructors, when none of the class's own takes the
      // arguments (a copy stays a copy).
      bool copyLike = args.size() == 1 && args[0]->ty && isClass(types.stripRef(args[0]->ty)) && classKey(types.stripRef(args[0]->ty)) == classKey(t);
      // (A value whose class converts to t itself takes that conversion: `return connector;` for a
      // SignalID through SignalIDConnector's `operator SignalID()`, not a base's constructors.)
      bool converts = args.size() == 1 && !copyLike && args[0]->ty && isClass(types.stripRef(args[0]->ty)) &&
                      !types.isDependent(types.stripRef(args[0]->ty)) && conversionOperator(types.stripRef(args[0]->ty), t, at);
      if (!copyLike && !converts && (!hasUserConstructors(c) || !findConstructor(t, argTypesOf(args), at)))
        if (std::optional<QualifiedName> base = inheritedConstructorBase(c))
        {
          // Inherited constructors: the arguments build that base, everything else is default.
          FunctionDecl::CtorInit init{};
          init.member = *base;
          init.args = args;
          initializeMembers(addr, t, Span<FunctionDecl::CtorInit>{&init, 1}, at);
          return;
        }
      if (args.size() == 1 && isPackExpansion(args[0]))
      {
        // `T(forward<Args>(args)...)`: the elements are the arguments. With one element the
        // constructor choice (copy, move, converting) sees that element's type; several elements
        // of a class without constructors fill its fields in order; otherwise the constructor
        // call below expands the pack itself.
        Expr* pattern = static_cast<UnaryExpr*>(args[0])->operand;
        size_t count = packElementCount(pattern, at);
        if (count == 0)
        {
          defaultConstruct(addr, t, at);
          return;
        }
        if (count == 1)
        {
          forEachPackElement(pattern, at, [&](size_t)
          {
            Expr* one = pattern;
            constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          });
          return;
        }
        if (!hasUserConstructors(c))
        {
          const StructInfo& info = structFor(t, at);
          storeZero(t, addr, at);
          size_t i = 0;
          forEachPackElement(pattern, at, [&](size_t)
          {
            if (i >= info.fieldDecls.size())
              unsupported(at, "more constructor arguments than " + qualifiedName(c) + " has fields");
            Decl* d = info.fieldDecls[i++];
            initializeFrom(gep(info.name, addr, fieldIndexOf(info, d)), info.fieldTypes[fieldIndexOf(info, d)], pattern, at);
          });
          return;
        }
      }
      const Ty* firstTy = args.size() == 1 ? args[0]->ty : nullptr;
      FunctionDecl* ctor = findConstructor(t, argTypesOf(args), at);
      if (ctor && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) == classKey(t) && !ctor->params.empty())
      {
        // The same class: a copy, unless the chosen constructor is a copy constructor itself (a
        // converting one, deleted or not, reached through a conversion of the class, loses to
        // the implicit copy: `Ref(const int&&) = delete` beside Ref's `operator const int&()`;
        // `ConvertingElement(const Tree&, uint32_t index = 0)` beside Tree's converting template).
        TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
        if (!ctor->templateParams.empty())
          global.deduceArgs(ctor, argTypesOf(args), classArgs); // `template<typename... Args> Str(Args&&...)` as Str(Str&): as deduced
        const Ty* pt;
        {
          ArgsScope scope(*this, &classArgs);
          pt = declType(ctor->params[0]);
        }
        if (pt && pt->kind == TyKind::Pack)
          pt = pt->args.empty() ? nullptr : pt->args[0];
        if (!pt || !isClass(types.stripRef(pt)) || classKey(pt) != classKey(t))
          ctor = nullptr; // a converting constructor of another class: not for a copy
        else if (!ctor->templateParams.empty() && implicitCopyOrMoveTakes(c, t, pt, at))
          ctor = nullptr; // as good as the implicit copy or move constructor, which wins as no template
      }
      if (ctor && (ctor->flags & DF_Delete))
        unsupported(at, "constructing " + types.toString(t, &symbols) + " with a deleted constructor (copying a move-only object?)");
      if (ctor && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) != classKey(t) && ctor->params.size() == 1)
      {
        TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
        const Ty* pt;
        {
          ArgsScope scope(*this, &classArgs);
          pt = declType(ctor->params[0]);
        }
        if (pt && isClass(types.stripRef(pt)) && classKey(pt) == classKey(t))
        {
          // The copy constructor, chosen for a value of another class: that class converts to t
          // first (a base, or its conversion operator), the constructor is not called with it.
          Value src = convert(rvalue(args[0]), types.unqualified(t), at);
          if (src.isTemporary)
            moveTemporary(addr, src, t, at);
          else if (isXvalue(args[0]))
            moveConstruct(addr, src.text, t, at);
          else
            copyConstruct(addr, src.text, t, at);
          return;
        }
      }
      if (ctor)
      {
        TemplateArgs ctorArgs = global.classArgsOf(canonicalClass(t, at));
        if (!ctor->templateParams.empty())
          global.deduceArgs(ctor, argTypesOf(args), ctorArgs); // `template<typename F> Function(F)`: F from the argument
        callFunction(ctor, ctorArgs, canonicalClass(t, at), addr, "", args, at);
        return;
      }
      if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) != classKey(t))
      {
        // A value of a class derived from t, no constructor of t taking it as written (t's copy
        // and move constructors are implicit, or t is an aggregate): its t part, copied or moved
        // (`Name(const Name& n) : Text(n)`, `: Plain(d)`).
        std::vector<std::pair<std::string, uint32_t>> path;
        if (const Ty* from = withCurrentArgs(types.unqualified(types.stripRef(firstTy))); !types.isDependent(from) && basePath(from, t, path, at))
        {
          Value src = convert(rvalue(args[0]), types.unqualified(t), at);
          if (isXvalue(args[0]))
            moveConstruct(addr, src.text, t, at);
          else
            copyConstruct(addr, src.text, t, at);
          return;
        }
      }
      if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) != classKey(t) &&
          conversionOperator(types.stripRef(firstTy), t, at))
      {
        // No constructor of t takes the value, its class converts to t: `ProtoID copy = connector;`
        // and `h.chosen = connector;` through IDConnector's `implicit operator IDType()`.
        Value src = convert(rvalue(args[0]), types.unqualified(t), at);
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else
          copyConstruct(addr, src.text, t, at);
        return;
      }
      if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) == classKey(t))
      {
        if (args[0]->kind == NodeKind::CallExpr && !firstTy->isReference())
        {
          // `: simulations(loadSimulations(input))`: the result built in the object itself (C++17),
          // as `T x = f();` is; a Map moved bitwise afterwards kept its header pointing at the dead
          // temporary.
          Value v = callValue(static_cast<CallExpr*>(args[0]), addr);
          if (!isClass(v.ty) || v.text == addr)
            return;
          if (v.isTemporary)
            moveTemporary(addr, v, t, at);
          else
            copyConstruct(addr, v.text, t, at);
          return;
        }
        Value src = rvalue(args[0]);
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else if (isXvalue(args[0]))
          moveConstruct(addr, src.text, t, at);
        else
          copyConstruct(addr, src.text, t, at);
        return;
      }
      if (!hasUserConstructors(c))
      {
        // An aggregate: the values fill the fields in order; the fields left over get their
        // default member initializers (or default construction), as in C++.
        const StructInfo& info = structFor(t, at);
        // `Item {load, ifNoPrototype, ::forward<Args>(args)...}` with Args empty: the expansion of an
        // empty pack is no value (InstanceLoader::registerLoader).
        std::vector<Expr*> values;
        bool expanded = false;
        for (Expr* a : args)
        {
          if (!isPackExpansion(a))
          {
            values.push_back(a);
            continue;
          }
          expanded = true;
          if (expandedCount(Span<Expr*>{&a, 1}, at) != 0)
            unsupported(at, "a pack expanded into the values of an aggregate");
        }
        if (expanded)
          args = Span<Expr*>{values.data(), uint32_t(values.size())};
        storeZero(t, addr, at);
        storeVptrs(addr, info);
        if (info.isUnion && designators.empty() && args.size() == 1 && !info.fieldDecls.empty())
        {
          // `{nullptr}` for a union: its first member, at the union's start (Lua's TValue's Value).
          initializeFrom(addr, unionMemberType(t, info.fieldDecls[0], at), args[0], at);
          return;
        }
        if (info.fieldDecls.size() == 1 && info.bases.empty() && designators.empty() && args.size() > 1)
        {
          // Brace elision: `Array<int, 3> a = {1, 2, 3}` fills the one array field element by element.
          const Ty* ft = info.fieldTypes[fieldIndexOf(info, info.fieldDecls[0])];
          if (ft->kind == TyKind::Array)
          {
            std::string fieldAddr = gep(info.name, addr, fieldIndexOf(info, info.fieldDecls[0]));
            if (args.size() > ft->count)
              unsupported(at, "more values than the array of " + qualifiedName(c) + " holds");
            for (size_t i = 0; i < args.size(); ++i)
            {
              std::string element = newTemp();
              fn += "  " + element + " = getelementptr inbounds " + llType(ft, at) + ", ptr " + fieldAddr + ", i64 0, i64 " + std::to_string(i) + "\n";
              initializeFrom(element, ft->inner, args[i], at);
            }
            return;
          }
        }
        // C++17 aggregate init: base sub-objects consume the first args, then data members.
        size_t argIdx = 0;
        if (!info.bases.empty() && designators.empty())
        {
          for (auto& [baseDef, baseFieldIdx] : info.bases)
          {
            if (argIdx >= args.size())
              break;
            std::string baseAddr = gep(info.name, addr, baseFieldIdx);
            const Ty* bt = info.fieldTypes[baseFieldIdx];
            initializeFrom(baseAddr, bt, args[argIdx++], at);
          }
          storeVptrs(addr, info); // the bases built pointed it at theirs
        }
        // The members the values go to: the fields, and each bit field of a storage unit on its
        // own (the unit's first bit field stands for the unit among the fields).
        std::vector<Decl*> members;
        for (Decl* d : info.fieldDecls)
          if (d->kind == NodeKind::VariableDecl && bitfieldFor(d))
            for (Decl* m : unitMembers(info, c, d))
              members.push_back(m);
          else
            members.push_back(d);
        // Which value goes to which member: in order, or by name for `.x = 1` designators.
        std::vector<Expr*> valueOf(members.size(), nullptr);
        std::vector<std::vector<Expr*>> elided(members.size()); // an array member's values without their braces
        size_t next = 0;
        for (size_t k = argIdx; k < args.size(); ++k)
        {
          uint32_t designator = k < designators.size() ? designators[k] : UINT32_MAX;
          if (designator != UINT32_MAX)
          {
            size_t found = members.size();
            for (size_t j = 0; j < members.size(); ++j)
              if (members[j]->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(members[j])->name.empty() &&
                  static_cast<VariableDecl*>(members[j])->name.last().symbol == designator)
                found = j;
            if (found == members.size())
              unsupported(at, "a designator naming no field of " + qualifiedName(c) + " (" + sym(designator) + ")");
            valueOf[found] = args[k];
            next = found + 1;
            continue;
          }
          if (next >= valueOf.size())
          {
            if (valueOf.empty())
              break; // anonymous/incomplete class with no known fields — already zeroed
            unsupported(at, "more values than " + qualifiedName(c) + " has fields");
          }
          // Brace elision: an array member takes its elements' values from the list itself when the
          // value there is no list and no array (`GUID g = { 0x429b8c22, 0x9188, 0x4b0c, 0x87, ... }`
          // fills Data4[8] from the last eight).
          if (members[next]->kind == NodeKind::VariableDecl && !bitfieldFor(members[next]))
            if (const Ty* ft = types.unqualified(info.fieldTypes[fieldIndexOf(info, members[next])]); ft->kind == TyKind::Array && ft->count > 0)
              if (Expr* a = args[k]; a->kind != NodeKind::BraceInitExpr && a->ty && a->ty->isKnown() &&
                                     !(a->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(a)->literalKind == TokenKind::String) &&
                                     types.unqualified(types.stripRef(a->ty))->kind != TyKind::Array)
              {
                size_t take = std::min<size_t>(ft->count, args.size() - k);
                elided[next] = std::vector<Expr*>(args.begin() + k, args.begin() + k + take);
                k += take - 1;
                ++next;
                continue;
              }
          valueOf[next++] = args[k];
        }
        size_t i = 0;
        std::set<size_t> zeroedUnits; // the storage units of bit fields, each zeroed at its first member
        // A union holds one member: the one a value is for (`{.effectTexture = {bitmap}}`), else
        // the one with a default member initializer, else its first (C++'s rule), each at the
        // union's start with its own type.
        Decl* unionMember = nullptr;
        if (info.isUnion)
        {
          for (size_t j = 0; j < members.size() && !unionMember; ++j)
            if (valueOf[j] || !elided[j].empty())
              unionMember = members[j];
          for (size_t j = 0; j < members.size() && !unionMember; ++j)
            if (members[j]->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(members[j])->init)
              unionMember = members[j];
          if (!unionMember && !members.empty())
            unionMember = members[0];
        }
        for (Decl* d : members)
        {
          if (info.isUnion && d != unionMember)
          {
            ++i;
            continue;
          }
          if (!elided[i].empty())
          {
            const Ty* ft = types.unqualified(info.fieldTypes[fieldIndexOf(info, d)]);
            std::string fieldAddr = gep(info.name, addr, fieldIndexOf(info, d));
            for (size_t e = 0; e < elided[i].size(); ++e)
            {
              std::string element = newTemp();
              fn += "  " + element + " = getelementptr inbounds " + llType(ft, at) + ", ptr " + fieldAddr + ", i64 0, i64 " + std::to_string(e) + "\n";
              initializeFrom(element, ft->inner, elided[i][e], at);
            }
            ++i;
            continue;
          }
          Expr* value = valueOf[i++];
          if (d->kind == NodeKind::VariableDecl)
            if (const BitfieldInfo* bits = bitfieldFor(d))
            {
              initializeBitfield(addr, t, info, static_cast<VariableDecl*>(d), *bits, value != nullptr, value, zeroedUnits, at);
              continue;
            }
          std::string fieldAddr = info.isUnion ? addr : gep(info.name, addr, fieldIndexOf(info, d));
          const Ty* ft = info.isUnion ? unionMemberType(t, d, at) : info.fieldTypes[fieldIndexOf(info, d)];
          const Ty* declared = d->kind == NodeKind::VariableDecl ? global.typeOfDecl(d) : nullptr;
          bool reference = ft->isReference() || (declared && declared->isReference());
          if (value)
          {
            if (reference)
              bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, value, at); // `Ref r{x}`: the member refers to x
            else
              initializeFrom(fieldAddr, ft, value, at);
            continue;
          }
          if (d->kind != NodeKind::VariableDecl)
            continue;
          auto* v = static_cast<VariableDecl*>(d);
          if (v->init && reference)
            withMemberInitializer(addr, t, v, [&](Expr* value) { bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, value, v); });
          else if (v->init)
            withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(fieldAddr, ft, value, v); });
          else if (isClass(ft))
            defaultConstruct(fieldAddr, ft, at);
          else if (ft->kind == TyKind::Array && (needsDestruction(ft, at) || needsConstruction(ft, at))) // elements with constructors or default member initializers
            constructArray(fieldAddr, ft, at);
        }
        return;
      }
      unsupported(at, "a constructor of " + qualifiedName(c) + " taking " + std::to_string(args.size()) + " arguments");
    }

    // `auto [a, b] = pair;`: e names a binding of the hidden local v; out gets the address of the
    // matching data member (or array element). False when e is not one of v's bindings (the
    // nameless name of v itself, which a tuple-like binding's get<i>(v) passes, included).
    bool bindingAddress(VariableDecl* v, Node* e, std::string& out)
    {
      if (v->structuredBindings.empty() || !e || e->kind != NodeKind::NameExpr || static_cast<NameExpr*>(e)->name.empty())
        return false;
      uint32_t symbol = static_cast<NameExpr*>(e)->name.last().symbol;
      size_t index = v->structuredBindings.size();
      for (size_t i = 0; i < v->structuredBindings.size(); ++i)
        if (static_cast<NameExpr*>(v->structuredBindings[i])->name.last().symbol == symbol)
          index = i;
      if (index == v->structuredBindings.size())
        return false;
      auto it = locals.find(v);
      if (it == locals.end())
        unsupported(e, "a structured binding outside its function");
      std::string base = it->second.addr;
      if (it->second.isRef)
      {
        std::string r = newTemp();
        fn += "  " + r + " = load ptr, ptr " + base + "\n";
        base = r;
      }
      const Ty* object = types.unqualified(types.stripRef(it->second.ty));
      if (object->kind == TyKind::Array)
      {
        std::string r = newTemp();
        fn += "  " + r + " = getelementptr inbounds " + llType(object->inner, e) + ", ptr " + base + ", i64 " + std::to_string(index) + "\n";
        out = r;
        return true;
      }
      if (!isClass(object))
        unsupported(e, "a structured binding of something that is neither a class nor an array");
      // The class whose direct members the bindings name: the object's own class, or, when that
      // declares none, the one base that has them (C++'s rule: Vector2D's x and y are its CRTP
      // base Vector2's).
      const Ty* owner = object;
      for (int depth = 0; depth < 16 && structFor(owner, e).fieldDecls.empty(); ++depth)
      {
        const Ty* withMembers = nullptr;
        for (auto& [baseDef, baseIndex] : std::vector<std::pair<Decl*, uint32_t>>(structFor(owner, e).bases))
        {
          const Ty* bt = structFor(owner, e).fieldTypes[baseIndex];
          if (!structFor(bt, e).fieldDecls.empty() || !structFor(bt, e).bases.empty())
          {
            if (withMembers)
              unsupported(e, "a structured binding of a class whose data members are in several bases");
            withMembers = bt;
          }
        }
        if (!withMembers)
          break;
        std::string part;
        if (!baseAddress(owner, withMembers, base, part, e))
          unsupported(e, "a structured binding through a base the layout does not have");
        base = part;
        owner = withMembers;
      }
      const StructInfo& info = structFor(owner, e);
      if (index >= info.fieldDecls.size() || !fieldAddress(owner, info.fieldDecls[index], base, out, e))
        unsupported(e, "a structured binding of a class without that many data members");
      // A reference member's binding names the object the member refers to, as `pair.second` does
      // (TestBuildabilityChecks' `[testedLocation, surface]` over Pair<Locations, Surface&> read the
      // member's own storage as the Surface).
      if (const Ty* ft = fieldTypeIn(owner, info.fieldDecls[index], e); ft && ft->isReference())
      {
        std::string referenced = newTemp();
        fn += "  " + referenced + " = load ptr, ptr " + out + "\n";
        out = referenced;
      }
      return true;
    }

    // `return result;` naming a local object or a by-value parameter of the returned class: it is
    // moved into the result, not copied (C++'s implicit move on return).
    // Whether a loop variable of type vt, given an element of elementTy, is of another class than
    // the element (`for (const Label& l : wrappers)`), which the element converts to.
    bool elementOfOtherClass(const Ty* elementTy, const Ty* vt, Node* at)
    {
      const Ty* target = types.unqualified(types.stripRef(vt));
      const Ty* et = elementTy ? types.unqualified(types.stripRef(elementTy)) : nullptr;
      return et && isClass(target) && isClass(et) && !types.isDependent(et) && canonicalClass(et, at) != canonicalClass(target, at);
    }

    // What a loop variable of type vt refers to or copies, from the element at addr: the element,
    // or the element converted to vt's class (to its base, or through its conversion operator:
    // a Vector of reference_wrapper read as references), never the element read as that class.
    std::string elementAs(const std::string& addr, const Ty* elementTy, const Ty* vt, Node* at)
    {
      if (!elementOfOtherClass(elementTy, vt, at))
        return addr;
      return convert(Value{addr, types.unqualified(types.stripRef(elementTy)), false}, types.unqualified(types.stripRef(vt)), at).text;
    }

    // Whether a loop variable of type vt (or referring to one) is of a class and the element is no
    // object (`for (String name : {"stone-wall", "transport-belt"})`): the variable is made from it.
    bool elementMadeInto(const Ty* elementTy, const Ty* vt)
    {
      return isClass(types.unqualified(types.stripRef(vt))) && elementTy && !isAggregate(types.unqualified(types.stripRef(elementTy)));
    }

    // Builds the loop variable's object of class cls at slot from the element at elementAddr as
    // `String name = element;` would (a converting constructor), never from the element's bytes:
    // TestDragBuilding's `for (std::string blockingEntity : {"stone-wall", ...})` read each
    // `const char*` as a String, and every name came out empty.
    void makeFromElement(const std::string& slot, const Ty* cls, const std::string& elementAddr, const Ty* elementTy, Node* at)
    {
      VariableDecl element{};
      element.kind = NodeKind::VariableDecl;
      element.token = at->token;
      NameExpr name{};
      name.kind = NodeKind::NameExpr;
      name.token = at->token;
      name.resolved = &element;
      name.ty = types.stripRef(elementTy);
      locals[&element] = {elementAddr, types.stripRef(elementTy), false};
      struct Forget { std::unordered_map<Decl*, Local>& locals; Decl* d; ~Forget() { locals.erase(d); } } forget{locals, &element};
      initializeFrom(slot, cls, &name, at);
    }

    // The class of the local object (not a reference) e names, when it is not the function's
    // result class (whose own `return obj;` is a move: returnedLocalObject); null otherwise.
    const Ty* localObjectOfOtherClass(Expr* e)
    {
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      Decl* d = static_cast<NameExpr*>(e)->resolved;
      if (!d || (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::ParamDecl) || (d->flags & DF_Static))
        return nullptr;
      auto it = locals.find(d);
      if (it == locals.end() || it->second.isRef || !isClass(it->second.ty) || classKey(it->second.ty) == classKey(currentReturn))
        return nullptr;
      return types.unqualified(it->second.ty);
    }

    Decl* returnedLocalObject(Expr* e)
    {
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      Decl* d = static_cast<NameExpr*>(e)->resolved;
      if (!d || (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::ParamDecl) || (d->flags & DF_Static))
        return nullptr;
      if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->structuredBindings.empty())
        return nullptr;
      auto it = locals.find(d);
      if (it == locals.end() || it->second.isRef || !isClass(it->second.ty) || classKey(it->second.ty) != classKey(currentReturn))
        return nullptr;
      return d;
    }

    // A bit field's value: the storage unit shifted and masked (sign-extended for signed types).
    Value loadBitfield(const std::string& addr, const BitfieldInfo& bits, const Ty* memberTy, Node* at)
    {
      std::string unitLl = llType(bits.unit, at);
      uint32_t unitBits = uint32_t(sizeOf(bits.unit, at) * 8);
      std::string unit = newTemp(), r;
      fn += "  " + unit + " = load " + unitLl + ", ptr " + addr + "\n";
      const Ty* valueTy = types.unqualified(memberTy);
      bool isSigned = (valueTy->kind == TyKind::Int || valueTy->kind == TyKind::Char) && !valueTy->isUnsigned;
      if (isSigned)
      {
        std::string left = newTemp();
        fn += "  " + left + " = shl " + unitLl + " " + unit + ", " + std::to_string(unitBits - bits.shift - bits.width) + "\n";
        r = newTemp();
        fn += "  " + r + " = ashr " + unitLl + " " + left + ", " + std::to_string(unitBits - bits.width) + "\n";
      }
      else
      {
        std::string shifted = newTemp();
        fn += "  " + shifted + " = lshr " + unitLl + " " + unit + ", " + std::to_string(bits.shift) + "\n";
        r = newTemp();
        uint64_t mask = bits.width >= 64 ? ~0ull : ((1ull << bits.width) - 1);
        fn += "  " + r + " = and " + unitLl + " " + shifted + ", " + std::to_string(mask) + "\n";
      }
      Value asUnit{r, bits.unit};
      if (valueTy->kind == TyKind::Bool)
      {
        std::string b = newTemp();
        fn += "  " + b + " = icmp ne " + unitLl + " " + r + ", 0\n";
        return {b, valueTy};
      }
      return convert(asUnit, valueTy, at);
    }

    // Writes a bit field: the other bits of the storage unit stay as they are.
    void storeBitfield(Value v, const std::string& addr, const BitfieldInfo& bits, const Ty* memberTy, Node* at)
    {
      std::string unitLl = llType(bits.unit, at);
      Value asUnit = convert(v, bits.unit, at);
      uint64_t mask = bits.width >= 64 ? ~0ull : ((1ull << bits.width) - 1);
      std::string old = newTemp(), cleared = newTemp(), masked = newTemp(), shifted = newTemp(), merged = newTemp();
      fn += "  " + old + " = load " + unitLl + ", ptr " + addr + "\n";
      fn += "  " + cleared + " = and " + unitLl + " " + old + ", " + std::to_string(~(mask << bits.shift)) + "\n";
      fn += "  " + masked + " = and " + unitLl + " " + asUnit.text + ", " + std::to_string(mask) + "\n";
      fn += "  " + shifted + " = shl " + unitLl + " " + masked + ", " + std::to_string(bits.shift) + "\n";
      fn += "  " + merged + " = or " + unitLl + " " + cleared + ", " + shifted + "\n";
      fn += "  store " + unitLl + " " + merged + ", ptr " + addr + "\n";
    }

    // The bit fields that share first's storage unit, in declaration order (first among them).
    std::vector<Decl*> unitMembers(const StructInfo& info, ClassDecl* c, Decl* first)
    {
      std::vector<Decl*> out;
      uint32_t index = fieldIndexOf(info, first);
      if (c)
        for (Decl* m : c->members)
          if (info.bitfields.count(m) && fieldIndexOf(info, m) == index)
            out.push_back(m);
      if (out.empty())
        out.push_back(first);
      return out;
    }

    // A bit field initialised: from value (an aggregate's element, a constructor initializer's
    // argument; `: flag()` is explicit with none, a zero), else from its default member
    // initializer (`bool known : 1 = true;`). It shares its storage unit with its neighbours: the
    // unit is zeroed once, at its first member (zeroed), and each value stored into its own bits.
    void initializeBitfield(const std::string& addr, const Ty* t, const StructInfo& info, VariableDecl* v, const BitfieldInfo& bits,
                            bool explicitInit, Expr* value, std::set<size_t>& zeroed, Node* at)
    {
      size_t index = fieldIndexOf(info, v);
      std::string fieldAddr = gep(info.name, addr, index);
      if (zeroed.insert(index).second)
        fn += "  store " + llType(info.fieldTypes[index], at) + " 0, ptr " + fieldAddr + "\n";
      const Ty* memberTy = types.unqualified(types.stripRef(declType(v)));
      auto storeInto = [&](Expr* e, Node* where)
      {
        if (e && e->kind == NodeKind::BraceInitExpr)
        {
          auto* list = static_cast<BraceInitExpr*>(e);
          if (list->args.empty())
            return; // `{}`: zero, as the unit already is
          if (list->args.size() != 1)
            unsupported(where, "a bit field initialised with a list of several values");
          e = list->args[0];
        }
        storeBitfield(convert(rvalue(e), memberTy, where), fieldAddr, bits, memberTy, where);
      };
      if (value)
        withSingleElement(value, at, [&](Expr* one) { storeInto(one, at); });
      else if (explicitInit)
        storeBitfield(Value{"0", bits.unit, false}, fieldAddr, bits, memberTy, at);
      else if (v->init)
        withMemberInitializer(addr, t, v, [&](Expr* init) { storeInto(init, v); });
    }

    // The address of the object a variable or parameter declaration names (what lvalue gives a
    // name expression): a local, a field of this, or a global.
    std::string addressOfDecl(Decl* d, Node* e)
    {
      if (d->kind == NodeKind::VariableDecl)
        if (std::string bound; bindingAddress(static_cast<VariableDecl*>(d), e, bound))
          return bound;
      auto it = locals.find(d);
      if (it != locals.end())
      {
        if (it->second.isRef)
        {
          std::string r = newTemp();
          fn += "  " + r + " = load ptr, ptr " + it->second.addr + "\n";
          return r;
        }
        return it->second.addr;
      }
      if (d->kind == NodeKind::VariableDecl)
      {
        if (Decl* cls = global.owningClass(d); cls && !(d->flags & DF_Static))
        {
          if (!currentClassTy)
            unsupported(e, "a member used outside a method");
          return memberObject(currentClassTy, d, thisPointer(e), e);
        }
        return globalAddress(static_cast<VariableDecl*>(d), e, qualifierClass(e, e));
      }
      unsupported(e, "taking the address of " + qualifiedName(d) + " (" + nodeKindName(d->kind) + (packLocals.count(d) ? ", a parameter pack outside its expansion" : "") + ")");
    }

    // The object a data member of the object at `object` (of class classTy) is: its field, or for a
    // reference member the object the field refers to (its address is what the field holds), also
    // when a template argument makes the member a reference (Holder<int&>'s `A value`).
    std::string memberObject(const Ty* classTy, Decl* d, const std::string& object, Node* e)
    {
      std::string addr;
      if (!fieldAddress(classTy, d, object, addr, e))
        unsupported(e, "a member that is not a field of the object's class");
      const Ty* ft = fieldTypeIn(classTy, d, e);
      if (!ft)
        ft = d->kind == NodeKind::VariableDecl ? declType(d) : nullptr;
      if (ft && ft->isReference())
      {
        std::string referenced = newTemp();
        fn += "  " + referenced + " = load ptr, ptr " + addr + "\n";
        return referenced;
      }
      return addr;
    }

    // Whether an expression is an xvalue: an rvalue reference to an object that may be moved from
    // (`move(x)`); a named rvalue reference is an lvalue.
    static bool isXvalue(Expr* e) { return e && e->ty && e->ty->kind == TyKind::RRef && !isLvalueExpression(e); }

    // Move-constructs the object at src into dst: through the move constructor where the class has
    // one, as a copy otherwise.
    // Whether class c (of type t) has an implicit copy or move constructor taking a parameter of
    // type pt (T&& or const T&) as well as a constructor template given the same argument does:
    // the tie goes to the implicit one, no template ([over.match.best]). The implicit move
    // constructor is declared when c declares no copy constructor, copy or move assignment and no
    // destructor; the implicit copy constructor when c declares none ([class.copy.ctor] 6 and 8).
    bool implicitCopyOrMoveTakes(ClassDecl* c, const Ty* t, const Ty* pt, Node* at)
    {
      if (!pt || !pt->isReference() || !isClass(types.stripRef(pt)) || classKey(types.stripRef(pt)) != classKey(t))
        return false;
      bool copyCtor = false, moveCtor = false, assignment = false, destructor = false;
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &args);
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl)
          continue;
        auto* f = static_cast<FunctionDecl*>(m);
        if (f->flags & DF_Destructor)
          destructor = true;
        else if (!f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign)
          assignment = true;
        else if ((f->flags & DF_Constructor) && f->templateParams.empty() && f->params.size() == 1)
        {
          const Ty* q = declType(f->params[0]);
          if (q && q->isReference() && isClass(types.stripRef(q)) && classKey(types.stripRef(q)) == classKey(t))
            (q->kind == TyKind::RRef ? moveCtor : copyCtor) = true;
        }
      }
      if (pt->kind == TyKind::RRef)
        return !copyCtor && !moveCtor && !assignment && !destructor;
      return !copyCtor && types.stripRef(pt)->isConst; // `const T&`; a non-const T& binds better than the copy's
    }

    void moveConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      FunctionDecl* move = nullptr;
      bool defaultedMove = false;
      {
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          if (f->params.size() != 1)
            continue;
          const Ty* pt = declType(f->params[0]);
          bool ownMove = pt && pt->kind == TyKind::RRef && isClass(pt->inner) && classKey(pt->inner) == classKey(t);
          if (f->flags & DF_Default)
          {
            defaultedMove = defaultedMove || ownMove; // `T(T&&) = default`: the memberwise move below
            continue;
          }
          if (ownMove)
            move = f;
        }
      }
      if (move && !(move->flags & DF_Delete))
      {
        emitCall("", "void " + useFunction(move, args, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
        return;
      }
      if (!move && !defaultedMove && !implicitCopyOrMoveTakes(c, t, types.rref(types.unqualified(t)), at))
      {
        // No move constructor, not even an implicit one: a constructor template taking the rvalue
        // as a T&& is the one C++ chooses, over a copy constructor's const T& (`template<typename...
        // Args> Str(Args&&...)` beside an explicit copy constructor: ImmutableStringTemplate's moves
        // were copies).
        const Ty* rvalueTy = types.rref(types.unqualified(t));
        FunctionDecl* chosen = findConstructor(t, {rvalueTy}, at);
        if (chosen && !chosen->templateParams.empty() && !(chosen->flags & DF_Delete) && !global.isExplicit(chosen, args))
        {
          TemplateArgs ctorArgs = args;
          global.deduceArgs(chosen, {rvalueTy}, ctorArgs);
          const Ty* pt;
          {
            ArgsScope scope(*this, &ctorArgs);
            pt = declType(chosen->params[0]);
          }
          if (pt && pt->kind == TyKind::Pack)
            pt = pt->args.size() == 1 ? pt->args[0] : nullptr;
          if (pt && pt->kind == TyKind::RRef && isClass(pt->inner) && classKey(pt->inner) == classKey(t))
          {
            emitCall("", "void " + useFunction(chosen, ctorArgs, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
            return;
          }
        }
      }
      if (defaultedMove && !c->isTypedUnion)
      {
        // `explicit Animation(Animation&&) = default` beside a deleted copy constructor: the
        // members moved, the copy constructor not looked at (a move is no copy).
        memberwiseConstruct(dst, src, t, at, true);
        return;
      }
      copyConstruct(dst, src, t, at, true); // the user copy constructor if any, else memberwise moves
    }

    // Copy-constructs the object at src into dst (user copy constructor or bitwise copy); with
    // `move`, a class without a user copy constructor moves its members instead.
    void copyConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move = false)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (c->isTypedUnion)
      {
        copyAlternative(c, t, dst, src, move, at);
        return;
      }
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      FunctionDecl* copy = nullptr;
      bool hasMove = false, defaultedCopy = false;
      {
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          if (f->params.size() != 1)
            continue;
          const Ty* pt = declType(f->params[0]);
          if (!pt || !pt->isReference() || !isClass(pt->inner) || classKey(pt->inner) != classKey(t))
            continue;
          if (f->flags & DF_Default)
          {
            // `T(const T&) = default`: the memberwise copy below, whatever else the class declares
            // (BuildResult: a move constructor and a destructor of its own besides).
            defaultedCopy = defaultedCopy || pt->kind == TyKind::LRef;
            continue;
          }
          if (pt->kind == TyKind::LRef)
            copy = f; // `T(const T&)`; the move constructor `T(T&&)` would empty the source
          else
            hasMove = true;
        }
      }
      if (copy && (copy->flags & DF_Delete))
        unsupported(at, "copying " + qualifiedName(c) + ", whose copy constructor is deleted (move it instead)");
      if (!copy && hasMove && !defaultedCopy)
        unsupported(at, "copying " + qualifiedName(c) + ", which has a move constructor but no copy constructor");
      if (copy)
      {
        emitCall("", "void " + useFunction(copy, args, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
        return;
      }
      // A destructor of its own leaves the class its implicit copy constructor (C++ only deprecates
      // it): the members copied (Achievement's `new Achievement(*this)`, InputAction's ActionData).
      // A declared move assignment deletes it; and a union with a destructor of its own manages its
      // member's lifetime (an Optional's storage), whose bytes a copy would share.
      if (!defaultedCopy && declaresMoveMember(c, t, true, at))
        unsupported(at, "copying " + qualifiedName(c) + ", which declares a move assignment but no copy constructor");
      if (!defaultedCopy && c->keyword == Kw::union_ && findDestructor(c))
        unsupported(at, "copying " + qualifiedName(c) + ", a union with a destructor but no copy constructor");
      memberwiseConstruct(dst, src, t, at, move);
    }

    // Whether class c (of type t) declares a move assignment (assignment) or a move constructor
    // (`= default` and `= delete` ones included): what deletes C++'s implicit copy members.
    bool declaresMoveMember(ClassDecl* c, const Ty* t, bool assignment, Node* at)
    {
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &args);
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl)
          continue;
        auto* f = static_cast<FunctionDecl*>(m);
        bool isAssignment = !f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign;
        if ((assignment ? !isAssignment : !(f->flags & DF_Constructor)) || f->params.size() != 1)
          continue;
        const Ty* pt = declType(f->params[0]);
        if (pt && pt->kind == TyKind::RRef && isClass(pt->inner) && classKey(pt->inner) == classKey(t))
          return true;
      }
      return false;
    }

    // Constructs the object at dst from the one at src base by base and member by member
    // (bitwise when nothing needs destruction): copies, or moves with `move`.
    void memberwiseConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move)
    {
      // Bitwise when nothing needs destruction and no base or member has a copy or move constructor
      // of its own (agui::GenericTargetable's copy targets nothing: a widget copied bitwise shared
      // the original's targeters).
      if (!needsDestruction(t, at) && relocatesBitwise(t, at))
      {
        if (isClass(t) && structFor(t, at).empty)
          return; // no bytes to copy (and as a base, none of its own: its byte is the next member's)
        std::string v = newTemp();
        fn += "  " + v + " = load " + llType(t, at) + ", ptr " + src + "\n";
        fn += "  store " + llType(t, at) + " " + v + ", ptr " + dst + "\n";
        if (isClass(t))
          storeVptrs(dst, structFor(t, at)); // the source may be of a derived class (a slicing copy)
        return;
      }
      // Memberwise: bases and members in order, each with its own copy semantics.
      const StructInfo& info = structFor(t, at);
      for (auto& [baseDef, index] : info.bases)
      {
        if (move)
          moveConstruct(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at);
        else
          copyConstruct(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at);
      }
      storeVptrs(dst, info); // after the bases, whose constructors pointed it at theirs
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = fieldIndexOf(info, d);
        const Ty* ft = info.fieldTypes[index];
        std::string to = gep(info.name, dst, index), from = gep(info.name, src, index);
        if (isClass(ft) && move)
          moveConstruct(to, from, ft, at);
        else if (isClass(ft))
          copyConstruct(to, from, ft, at);
        else if (isLambda(ft))
          copyClosure(to, from, ft, at, move); // FunctionImpl<F>'s `F f`
        else if (ft->kind == TyKind::Array)
          transferArray(to, from, ft, at, move, false);
        else
        {
          std::string v = newTemp();
          fn += "  " + v + " = load " + llType(ft, at) + ", ptr " + from + "\n";
          fn += "  store " + llType(ft, at) + " " + v + ", ptr " + to + "\n";
        }
      }
    }

    // Copies or moves the array of type t at src into the one at dst, constructing (assign: assigning)
    // element by element, each through its own copy or move: a member `T items[N]` of a class whose
    // copy or move is memberwise (the library's Array<Vector<T>, N> returned by value; bitwise, the
    // vectors were freed twice). One after the other for a few, in a loop for more; an array of what
    // needs no destruction is copied bitwise.
    void transferArray(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move, bool assign)
    {
      t = types.unqualified(t);
      if (!needsDestruction(t, at) || t->count == 0)
      {
        std::string v = newTemp();
        fn += "  " + v + " = load " + llType(t, at) + ", ptr " + src + "\n";
        fn += "  store " + llType(t, at) + " " + v + ", ptr " + dst + "\n";
        return;
      }
      auto transferAt = [&](const std::string& index)
      {
        std::string to = newTemp(), from = newTemp();
        fn += "  " + to + " = getelementptr inbounds " + llType(t, at) + ", ptr " + dst + ", i64 0, i64 " + index + "\n";
        fn += "  " + from + " = getelementptr inbounds " + llType(t, at) + ", ptr " + src + ", i64 0, i64 " + index + "\n";
        const Ty* element = types.unqualified(t->inner);
        if (element->kind == TyKind::Array)
          transferArray(to, from, element, at, move, assign);
        else if (assign)
          assignObject(to, from, element, at, move);
        else if (move)
          moveConstruct(to, from, element, at);
        else
          copyConstruct(to, from, element, at);
      };
      if (t->count <= 8)
      {
        for (uint32_t i = 0; i < t->count; ++i)
          transferAt(std::to_string(i));
        return;
      }
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i64\n";
      fn += "  store i64 0, ptr " + slot + "\n";
      std::string loopL = newLabel("array.loop"), bodyL = newLabel("array.body"), endL = newLabel("array.end");
      emitBranch(loopL);
      startBlock(loopL);
      std::string index = newTemp(), done = newTemp();
      fn += "  " + index + " = load i64, ptr " + slot + "\n";
      fn += "  " + done + " = icmp eq i64 " + index + ", " + std::to_string(t->count) + "\n";
      fn += "  br i1 " + done + ", label %" + endL + ", label %" + bodyL + "\n";
      startBlock(bodyL);
      transferAt(index);
      std::string next = newTemp();
      fn += "  " + next + " = add i64 " + index + ", 1\n";
      fn += "  store i64 " + next + ", ptr " + slot + "\n";
      emitBranch(loopL);
      startBlock(endL);
    }

    // Implicit copy assignment: user operator= where one exists, memberwise otherwise.
    void assignObject(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move = false)
    {
      if (ClassDecl* c = typedUnionOf(t, at))
      {
        // The alternative held goes, the source's alternative and tag come.
        destroyAlternative(c, t, dst, at);
        copyAlternative(c, t, dst, src, move, at);
        return;
      }
      const Ty* foundIn = nullptr;
      // From an lvalue: operator=(const T&). From a temporary or `move(x)`: operator=(T&&) if there is one.
      std::vector<const Ty*> argTypes{move ? types.rref(types.unqualified(t)) : types.lref(types.qualified(t, true, false))};
      FunctionDecl* op = declaresAssignment(t, at) ? findMemberFunction(t, "operator=", argTypes, t, at, &foundIn) : nullptr;
      bool defaulted = op && (op->flags & DF_Default);
      if (defaulted)
        op = nullptr; // `operator=(const T&) = default`: the memberwise assignment below
      if (op)
      {
        if (op->flags & DF_Delete)
          unsupported(at, "assignment to " + qualifiedName(t->decl) + " through its deleted operator=");
        std::string thisArg = dst;
        if (canonicalClass(t, at) != foundIn)
          baseAddress(t, foundIn, dst, thisArg, at);
        std::string name = useFunction(op, global.classArgsOf(foundIn), foundIn);
        Signature s;
        {
          TemplateArgs args = global.classArgsOf(foundIn);
          ArgsScope scope(*this, &args);
          s = signatureOf(op);
        }
        if (s.returnType == "void")
          emitCall("", "void " + name + "(ptr " + thisArg + ", ptr " + src + ")");
        else
          emitCall(newTemp() + " = ", s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + src + ")");
        return;
      }
      if (!needsDestruction(t, at) && assignsBitwise(t, at))
      {
        if (isClass(t) && structFor(t, at).empty)
          return; // no state, and as a base maybe no bytes of its own (the next member's)
        std::vector<std::string> vptrs = loadVptrs(dst, structFor(t, at));
        std::string v = newTemp();
        fn += "  " + v + " = load " + llType(t, at) + ", ptr " + src + "\n";
        fn += "  store " + llType(t, at) + " " + v + ", ptr " + dst + "\n";
        restoreVptrs(vptrs);
        return;
      }
      // A destructor of its own leaves the class its implicit copy assignment (C++ only deprecates
      // it), a defaulted one is memberwise anyway (Trigger); a declared move constructor deletes the
      // implicit copy assignment.
      if (!defaulted && !move && declaresMoveMember(classDef(t->decl, at), t, false, at))
        unsupported(at, "assignment to " + qualifiedName(t->decl) + ", which declares a move constructor but no operator=");
      if (!defaulted && classDef(t->decl, at)->keyword == Kw::union_ && findDestructor(classDef(t->decl, at)))
        unsupported(at, "assignment to " + qualifiedName(t->decl) + ", a union with a destructor but no operator=");
      memberwiseAssign(dst, src, t, at, move);
    }

    // Whether class t declares an operator= of its own (`using Base::operator=;` included): one that
    // does not has implicit ones (memberwise), which hide its bases'.
    bool declaresAssignment(const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      for (Decl* m : c->members)
      {
        if (m->kind == NodeKind::FunctionDecl)
        {
          auto* f = static_cast<FunctionDecl*>(m);
          if (!f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign)
            return true;
        }
        else if (m->kind == NodeKind::UsingNameDecl)
        {
          const QualifiedName& name = static_cast<UsingNameDecl*>(m)->name;
          if (!name.empty() && name.last().special == 2 && P(name.last().opToken) == P::Assign)
            return true;
        }
      }
      return false;
    }

    // Assigns the object at src to the one at dst base by base and member by member.
    void memberwiseAssign(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move)
    {
      const StructInfo& info = structFor(t, at);
      for (auto& [baseDef, index] : info.bases)
        assignObject(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at, move);
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = fieldIndexOf(info, d);
        const Ty* ft = info.fieldTypes[index];
        std::string to = gep(info.name, dst, index), from = gep(info.name, src, index);
        if (isClass(ft))
          assignObject(to, from, ft, at, move);
        else if (ft->kind == TyKind::Array)
          transferArray(to, from, ft, at, move, true);
        else
        {
          std::string v = newTemp();
          fn += "  " + v + " = load " + llType(ft, at) + ", ptr " + from + "\n";
          fn += "  store " + llType(ft, at) + " " + v + ", ptr " + to + "\n";
        }
      }
    }

    // Initialises the object/scalar at addr of type t from an initializer expression,
    // constructing in place where the expression is a temporary.
    void initializeFrom(const std::string& addr, const Ty* t, Expr* init, Node* at)
    {
      t = types.stripRef(t);
      // `[callback{::move(callback)}]`, `int x{5}`: one value in braces initialises what is not a
      // class as the value itself would.
      if (init && init->kind == NodeKind::BraceInitExpr && !isClass(t) && t->kind != TyKind::Array)
        if (auto* list = static_cast<BraceInitExpr*>(init); list->args.size() == 1 && list->designators.empty() &&
                                                            list->args[0]->kind != NodeKind::BraceInitExpr)
          init = list->args[0];
      if (isLambda(t))
      {
        initializeClosure(addr, t, init, at);
        return;
      }
      if (isClass(t) && init && init->kind == NodeKind::BraceInitExpr && isInitializerListType(t))
      {
        // `InitializerList<int> numbers = {1, 2, 3}`, `const auto entityNames = {"pipe", "tank"}`:
        // the list over an array that lives as long as the variable.
        initializerListTemporary(t, static_cast<BraceInitExpr*>(init), at, addr);
        return;
      }
      if (isClass(t))
      {
        // `T a = T(x)` and `T a = makeT()` build the object in place; an initializer of another
        // class (`Target a = make();` returning a Source) goes through the converting constructor below.
        const Ty* initTy = withCurrentArgs(init->ty);
        bool initIsClass = initTy && isClass(types.stripRef(initTy));
        // A template parameter the type checker left in (`shared_from_this()` of a base class
        // template, typed SharedPtr<T>): the value's own class decides once it is emitted.
        bool dependent = initIsClass && types.isDependent(initTy);
        bool sameClass = initIsClass && !dependent && canonicalClass(initTy, at) == canonicalClass(t, at);
        if (ClassDecl* u = typedUnionOf(t, at); u && !sameClass && !dependent && init->kind != NodeKind::BraceInitExpr && initTy && initTy->isKnown())
        {
          // `AIState s = Patrolling{1};`, `return Attacking{n};`: the alternative of the value's
          // type, constructed from it (design.md 8.5).
          size_t index = alternativeForValue(u, t, initTy, at);
          storeZero(t, addr, at);
          constructAlternative(u, t, addr, index, init, at);
          return;
        }
        if (init->kind == NodeKind::FunctionalCastExpr && sameClass)
        {
          // `T(list)`: the list is a constructor's argument; `T{...}` has its list as the one argument.
          directInitialize(addr, t, static_cast<FunctionalCastExpr*>(init)->args, static_cast<FunctionalCastExpr*>(init)->braced, at,
                           !static_cast<FunctionalCastExpr*>(init)->braced);
          return;
        }
        if (init->kind == NodeKind::BraceInitExpr)
        {
          Expr* one = init; // constructInto sees the whole list: an InitializerList constructor, designators, or the fields
          constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          return;
        }
        if (init->kind == NodeKind::CallExpr && sameClass && !init->ty->isReference())
        {
          // The result is built in place; a call that put it anywhere else would leave the
          // variable unset (`Chunk c = maker(5)` through operator() once did).
          Value v = callValue(static_cast<CallExpr*>(init), addr);
          if (isClass(v.ty) && v.text != addr)
            unsupported(at, "a call that built its " + types.toString(t, &symbols) + " result somewhere else than the object it initialises");
          return;
        }
        if (init->kind == NodeKind::CallExpr && dependent && !init->ty->isReference())
        {
          Value v = callValue(static_cast<CallExpr*>(init), "");
          if (!isClass(v.ty) || !v.isTemporary || canonicalClass(v.ty, at) != canonicalClass(t, at))
            unsupported(at, "initialising a " + types.toString(t, &symbols) + " from a call returning " + (v.ty ? types.toString(v.ty, &symbols) : std::string("?")));
          moveTemporary(addr, v, t, at);
          return;
        }
        if (init->kind == NodeKind::ParenExpr)
        {
          initializeFrom(addr, t, static_cast<ParenExpr*>(init)->inner, at);
          return;
        }
        // An SSE operation the checker could not type (one intrin.h has as a macro, _mm_shuffle_ps)
        // initialising a wrapper class (float32x4): the vector stored into its first field.
        if (init->kind == NodeKind::CallExpr && (!init->ty || !init->ty->isKnown()))
        {
          Value v = rvalue(init);
          if (v.ty && v.ty->kind == TyKind::Vector)
          {
            StructInfo& info = structFor(t, at);
            if (info.fieldTypes.empty() || llType(info.fieldTypes[0], at) != llType(v.ty, at))
              unsupported(at, "initialising " + types.toString(t, &symbols) + ", whose first member is not that vector, from a vector");
            fn += "  store " + llType(v.ty, at) + " " + v.text + ", ptr " + gep(info.name, addr, 0) + "\n";
            return;
          }
          // Not a vector — fall through to the non-class converting constructor path.
        }
        if (init->ty && init->ty->isKnown() && !isClass(types.stripRef(init->ty)) && isClass(t))
        {
          // A non-class value: through a converting constructor (`String s = "text"`).
          Expr* one = init;
          constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          return;
        }
        if (initIsClass && !dependent && !sameClass)
        {
          // Another class: through a converting constructor, a base, or its conversion operator
          // (`String text = vec;` with `operator String()`), never by copying it as a t.
          Expr* one = init;
          constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          return;
        }
        Value src = rvalue(init);
        if (!isClass(src.ty))
          unsupported(at, "initialising a class from a non-class value");
        if (canonicalClass(src.ty, at) != canonicalClass(t, at))
          unsupported(at, "initialising a " + types.toString(t, &symbols) + " from a " + types.toString(src.ty, &symbols) + " whose type the checker left open");
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else if (isXvalue(init))
          moveConstruct(addr, src.text, t, at); // `T tmp = move(a);`
        else
          copyConstruct(addr, src.text, t, at);
        return;
      }
      // `char text[16] = "edit me"` (also in braces or parentheses), `wchar_t name[8] = L"x"`: the
      // literal's characters and its terminating zero, the rest of the array zero.
      Expr* literal = init;
      while (literal && literal->kind == NodeKind::ParenExpr)
        literal = static_cast<ParenExpr*>(literal)->inner;
      if (literal && literal->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(literal)->args.size() == 1)
        literal = static_cast<BraceInitExpr*>(literal)->args[0];
      if (t->kind == TyKind::Array && (types.unqualified(t->inner)->kind == TyKind::Char || types.unqualified(t->inner)->kind == TyKind::Int) &&
          literal && literal->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(literal)->literalKind == TokenKind::String)
      {
        auto* l = static_cast<LiteralExpr*>(literal);
        uint32_t unitBytes = 1;
        size_t units = stringLiteralUnits(l, unitBytes);
        if (sizeOf(t->inner, at) != unitBytes)
          fail(at, "error: an array of " + types.toString(t->inner, &symbols) + " initialised from a string literal of " + std::to_string(unitBytes * 8) + "-bit characters");
        if (units > t->count)
          fail(at, "error: a string literal of " + std::to_string(units) + " characters initialises an array of " + std::to_string(t->count));
        storeZero(t, addr, at);
        if (declaredNames.claim("@llvm.memcpy.p0.p0.i64"))
          decls += "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n";
        fn += "  call void @llvm.memcpy.p0.p0.i64(ptr " + addr + ", ptr " + stringConstant(l) + ", i64 " +
              std::to_string(std::min<uint64_t>(units + 1, t->count) * unitBytes) + ", i1 false)\n";
        return;
      }
      if (init->kind == NodeKind::BraceInitExpr)
      {
        auto* bi = static_cast<BraceInitExpr*>(init);
        if (bi->args.empty())
        {
          storeZero(t, addr, at);
          return;
        }
        if (t->kind == TyKind::Array)
        {
          // Element by element (nested lists for arrays of arrays); the rest stays zero.
          storeZero(t, addr, at);
          for (size_t i = 0; i < bi->args.size() && i < t->count; ++i)
          {
            std::string element = newTemp();
            fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(i) + "\n";
            initializeFrom(element, t->inner, bi->args[i], at);
          }
          return;
        }
        if (bi->args.size() != 1)
          unsupported(at, "this brace initializer");
        init = bi->args[0];
      }
      store(rvalue(init), addr, t, at);
    }

    // t with the template parameters of the instantiation being emitted replaced by their
    // arguments, for an expression the type checker typed with a parameter left in.
    const Ty* withCurrentArgs(const Ty* t, int depth = 0)
    {
      if (!t || !currentArgs || depth > 16 || !t->isKnown() || !types.isDependent(t))
        return t;
      switch (t->kind)
      {
        case TyKind::Param:
          for (auto& [d, a] : currentArgs->pairs)
            if (d == t->decl && a)
            {
              const Ty* bound = a;
              // (of one element's expansion: the pattern's mention of the pack, that element)
              if (a->kind == TyKind::Pack && currentArgs->elementIndex < a->args.size() &&
                  std::find(currentArgs->elementPacks.begin(), currentArgs->elementPacks.end(), d) != currentArgs->elementPacks.end())
                bound = a->args[currentArgs->elementIndex];
              return types.qualified(bound, t->isConst || bound->isConst, t->isVolatile || bound->isVolatile);
            }
          return t;
        case TyKind::Pointer: return types.qualified(types.pointer(withCurrentArgs(t->inner, depth + 1)), t->isConst, t->isVolatile);
        case TyKind::LRef: return types.lref(withCurrentArgs(t->inner, depth + 1));
        case TyKind::RRef: return types.rref(withCurrentArgs(t->inner, depth + 1));
        case TyKind::Class:
        {
          std::vector<const Ty*> args;
          for (const Ty* a : t->args)
            args.push_back(withCurrentArgs(a, depth + 1));
          return types.qualified(types.classType(t->decl, Span<const Ty*>{args.data(), uint32_t(args.size())}), t->isConst, t->isVolatile);
        }
        default: return t;
      }
    }

    // Whether a temporary of class t may move to the object it initialises bitwise, as if it had been
    // built there (C++17 builds a prvalue in its object; kppc may build it in a slot of its own first):
    // no user-provided copy or move constructor in the class, its bases or its members. A class with
    // one may keep its own address somewhere: IDConnector hands `this` to its resolver, and its move
    // constructor gives the resolver the new address (a bitwise move left the resolver writing to the
    // dead temporary: assignPrototypeIDs then deleted garbage).
    std::unordered_map<const Ty*, bool> bitwiseRelocation; // this emitter's answers
    bool relocatesBitwise(const Ty* t, Node* at, int depth = 0)
    {
      t = types.unqualified(types.stripRef(t));
      while (t->kind == TyKind::Array && t->inner)
        t = types.unqualified(t->inner);
      if (isLambda(t) && depth <= 32)
      {
        for (auto& [index, ft] : closureValueFields(t, at))
          if (!relocatesBitwise(ft, at, depth + 1))
            return false;
        return true;
      }
      if (!isClass(t) || depth > 32)
        return true;
      const Ty* key = canonicalClass(t, at);
      if (auto it = bitwiseRelocation.find(key); it != bitwiseRelocation.end())
        return it->second;
      bitwiseRelocation[key] = true; // (a member pointing back at its class is no member by value)
      ClassDecl* c = classDef(key->decl, at);
      bool bitwise = !c->isTypedUnion;
      if (bitwise)
      {
        TemplateArgs args = global.classArgsOf(key);
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || (m->flags & (DF_Default | DF_Delete)) ||
              !m->templateParams.empty() || static_cast<FunctionDecl*>(m)->params.size() != 1)
            continue;
          const Ty* pt = declType(static_cast<FunctionDecl*>(m)->params[0]);
          if (pt && pt->isReference() && isClass(pt->inner) && classKey(pt->inner) == classKey(key))
            bitwise = false; // `IDConnector(IDConnector&& other)`, `T(const T&)` of its own
        }
      }
      if (bitwise)
      {
        const StructInfo& info = structFor(key, at);
        std::vector<const Ty*> fields = info.fieldTypes;
        for (const Ty* f : fields)
          if (!relocatesBitwise(f, at, depth + 1))
            bitwise = false;
      }
      bitwiseRelocation[key] = bitwise;
      return bitwise;
    }

    // Whether objects of type t are assigned bitwise: no copy or move assignment of its own (written,
    // not defaulted) in the class, its bases or its members (GenericTargetable's `operator=` keeps
    // the object's own targeters).
    std::unordered_map<const Ty*, bool> bitwiseAssignment;
    bool assignsBitwise(const Ty* t, Node* at, int depth = 0)
    {
      t = types.unqualified(types.stripRef(t));
      while (t->kind == TyKind::Array && t->inner)
        t = types.unqualified(t->inner);
      if (!isClass(t) || depth > 32)
        return true;
      const Ty* key = canonicalClass(t, at);
      if (auto it = bitwiseAssignment.find(key); it != bitwiseAssignment.end())
        return it->second;
      bitwiseAssignment[key] = true; // (a member pointing back at its class is no member by value)
      ClassDecl* c = classDef(key->decl, at);
      bool bitwise = !c->isTypedUnion;
      if (bitwise)
      {
        TemplateArgs args = global.classArgsOf(key);
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || (m->flags & (DF_Default | DF_Delete)) || !m->templateParams.empty())
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          bool isAssignment = !f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign;
          if (!isAssignment || f->params.size() != 1)
            continue;
          const Ty* pt = declType(f->params[0]);
          if (pt && pt->isReference() && isClass(pt->inner) && classKey(pt->inner) == classKey(key))
            bitwise = false;
        }
      }
      if (bitwise)
      {
        const StructInfo& info = structFor(key, at);
        std::vector<const Ty*> fields = info.fieldTypes;
        for (const Ty* f : fields)
          if (!assignsBitwise(f, at, depth + 1))
            bitwise = false;
      }
      bitwiseAssignment[key] = bitwise;
      return bitwise;
    }

    // Whether class t has a move or copy constructor that is not deleted (an implicit one counts).
    bool movableByConstructor(const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &args);
      bool declared = false, usable = false;
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || !m->templateParams.empty() ||
            static_cast<FunctionDecl*>(m)->params.size() != 1)
          continue;
        const Ty* pt = declType(static_cast<FunctionDecl*>(m)->params[0]);
        if (!pt || !pt->isReference() || !isClass(pt->inner) || classKey(pt->inner) != classKey(t))
          continue;
        declared = true;
        usable = usable || !(m->flags & DF_Delete);
      }
      return !declared || usable;
    }

    // A temporary that is used to initialise an object is moved there and no longer destroyed:
    // bitwise when its class allows it (relocatesBitwise), else by its move (or copy) constructor,
    // and the temporary destroyed then (a class that can be neither moved nor copied: bitwise, as
    // C++17 builds such a prvalue in its object).
    void moveTemporary(const std::string& dst, Value src, const Ty* t, Node* at)
    {
      for (size_t i = statementTemps.size(); i-- > 0;)
        if (statementTemps[i].addr == src.text)
        {
          statementTemps.erase(statementTemps.begin() + i);
          break;
        }
      if (isClass(t) && structFor(t, at).empty)
        return; // no state, and as a base maybe no bytes of its own (the next member's)
      if (isClass(t) && !relocatesBitwise(t, at) && !(src.ty && classKey(types.stripRef(src.ty)) != classKey(t)) && movableByConstructor(t, at))
      {
        // (Where the object could be built in place, it is: a call's result, an argument. Here it was
        // built elsewhere first; a class that may know its own address is moved by its constructor.)
        moveConstruct(dst, src.text, t, at);
        destroy(src.text, t, at);
        return;
      }
      std::string v = newTemp();
      fn += "  " + v + " = load " + llType(t, at) + ", ptr " + src.text + "\n";
      fn += "  store " + llType(t, at) + " " + v + ", ptr " + dst + "\n";
      if (isClass(t) && src.ty && classKey(types.stripRef(src.ty)) != classKey(t))
        storeVptrs(dst, structFor(t, at)); // a temporary of a derived class, sliced
    }

    std::string newObjectSlot(const Ty* t, Node* at)
    {
      std::string slot = "%o" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca " + llType(t, at) + alignSuffix(t, 0, at) + "\n";
      return slot;
    }

    // --- parameter packs ---

    // The pack parameter an expression refers to (the first one found), or null.
    Decl* packIn(Node* n, int depth = 0)
    {
      if (!n || depth > 32)
        return nullptr;
      switch (n->kind)
      {
        case NodeKind::NameExpr:
        {
          Decl* d = static_cast<NameExpr*>(n)->resolved;
          return d && packLocals.count(d) ? d : nullptr;
        }
        case NodeKind::ParenExpr: return packIn(static_cast<ParenExpr*>(n)->inner, depth + 1);
        case NodeKind::UnaryExpr: return packIn(static_cast<UnaryExpr*>(n)->operand, depth + 1);
        case NodeKind::CastExpr: return packIn(static_cast<CastExpr*>(n)->operand, depth + 1);
        case NodeKind::NamedCastExpr: return packIn(static_cast<NamedCastExpr*>(n)->operand, depth + 1);
        case NodeKind::MemberExpr: return packIn(static_cast<MemberExpr*>(n)->base, depth + 1);
        case NodeKind::BinaryExpr:
          if (Decl* d = packIn(static_cast<BinaryExpr*>(n)->lhs, depth + 1))
            return d;
          return packIn(static_cast<BinaryExpr*>(n)->rhs, depth + 1);
        case NodeKind::CallExpr:
        {
          auto* c = static_cast<CallExpr*>(n);
          if (Decl* d = packIn(c->callee, depth + 1))
            return d;
          for (Expr* a : c->args)
            if (Decl* d = packIn(a, depth + 1))
              return d;
          return nullptr;
        }
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(n);
          if (Decl* d = packIn(i->base, depth + 1))
            return d;
          for (Expr* a : i->args)
            if (Decl* d = packIn(a, depth + 1))
              return d;
          return nullptr;
        }
        case NodeKind::FunctionalCastExpr:
          for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
            if (Decl* d = packIn(a, depth + 1))
              return d;
          return nullptr;
        default:
          return nullptr;
      }
    }

    // The template parameter packs a pack parameter's declared type names, in order: `Args&&... args`
    // names Args; `auto PrototypeT::*... members` names PrototypeT and the pack invented for the
    // `auto`. An expansion binds each of them to its i-th element together.
    std::vector<Decl*> templatePacksOf(Decl* param)
    {
      std::vector<Decl*> packs;
      collectPacks(global.typeOfDecl(param), packs, 0);
      return packs;
    }

    static void collectPacks(const Ty* t, std::vector<Decl*>& packs, int depth)
    {
      if (!t || depth > 16)
        return;
      if (t->kind == TyKind::Param && t->decl && t->decl->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(t->decl)->isPack)
      {
        if (std::find(packs.begin(), packs.end(), t->decl) == packs.end())
          packs.push_back(t->decl);
        return;
      }
      collectPacks(t->inner, packs, depth + 1);
      for (const Ty* a : t->args)
        collectPacks(a, packs, depth + 1);
    }

    // The element types each pack named by param's type is bound to in the instantiation being
    // emitted, all of `count` elements; unsupported when one is not bound to such a pack.
    std::vector<const Ty*> boundPacks(const std::vector<Decl*>& packs, size_t count, Node* at)
    {
      std::vector<const Ty*> bound;
      for (Decl* tp : packs)
      {
        const Ty* packTy = nullptr;
        if (currentArgs)
          for (auto& [d, t] : currentArgs->pairs)
            if (d == tp)
              packTy = t;
        if (!packTy || packTy->kind != TyKind::Pack || packTy->args.size() != count)
          unsupported(at, "a pack whose elements are unknown");
        bound.push_back(packTy);
      }
      return bound;
    }

    // Runs body(i) for each element of the pack that `pattern` expands over, with the pack
    // parameter bound to that element and the function body re-typed for it.
    // Runs body on an expression, or, for a pack expansion `args...` of exactly one element, on its
    // pattern with that element bound (a scalar member initialised with `forward<Args>(args)...`).
    template<typename F>
    void withSingleElement(Expr* e, Node* at, F&& body)
    {
      if (!isPackExpansion(e))
      {
        body(e);
        return;
      }
      Expr* pattern = static_cast<UnaryExpr*>(e)->operand;
      if (packElementCount(pattern, at) != 1)
        unsupported(at, "a pack of " + std::to_string(packElementCount(pattern, at)) + " elements where one value is expected");
      forEachPackElement(pattern, at, [&](size_t) { body(pattern); });
    }

    // How many values arguments stand for once their pack expansions are expanded.
    size_t expandedCount(Span<Expr*> args, Node* at)
    {
      size_t n = 0;
      for (Expr* a : args)
        n += isPackExpansion(a) ? packElementCount(static_cast<UnaryExpr*>(a)->operand, at) : 1;
      return n;
    }

    // Runs body on each value arguments stand for: an expression as it is, a pack expansion's
    // pattern once per element, the element bound.
    template<typename F>
    void forEachExpanded(Span<Expr*> args, Node* at, F&& body)
    {
      for (Expr* a : args)
        if (isPackExpansion(a))
        {
          Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
          forEachPackElement(pattern, at, [&](size_t) { body(pattern); });
        }
        else
          body(a);
    }

    size_t packElementCount(Expr* pattern, Node* at)
    {
      Decl* param = packIn(pattern);
      if (!param)
        if (Decl* pack = currentArgs ? typePackIn(pattern) : nullptr)
          return boundPack(pack)->args.size(); // `values[I]...` over `int... I`
      if (!param)
        unsupported(at, "a pack expansion that does not name a parameter pack");
      return packLocals[param].size();
    }

    // What the instantiation binds a template parameter pack to (typePackIn found it bound).
    const Ty* boundPack(Decl* pack)
    {
      for (auto& [d, t] : currentArgs->pairs)
        if (d == pack)
          return t;
      return nullptr;
    }

    template<typename F>
    void forEachPackElement(Expr* pattern, Node* at, F&& body)
    {
      Decl* param = packIn(pattern);
      if (!param && currentArgs)
        if (Decl* pack = typePackIn(pattern))
        {
          // `values[I]...` over a template parameter pack (`int... I` of the index-sequence idiom):
          // the pattern once per element, with I bound to that element.
          forEachTypePackElement(pack, body);
          return;
        }
      if (!param)
        unsupported(at, "a pack expansion that does not name a parameter pack");
      std::vector<Decl*> packs = templatePacksOf(param);
      if (packs.empty() || !currentArgs)
        unsupported(at, "a pack expansion outside an instantiation");
      std::vector<Local> elements = packLocals[param];
      boundPacks(packs, elements.size(), at); // (each as long as the parameter pack)
      TemplateArgs original = *currentArgs;
      for (size_t i = 0; i < elements.size(); ++i)
      {
        // The pattern's mentions of the packs name element i; the rest of the body keeps them whole.
        TemplateArgs elementArgs = original;
        elementArgs.elementPacks = packs;
        elementArgs.elementIndex = uint32_t(i);
        global.retypeFor(currentFunction, elementArgs, currentClassTy, true);
        locals[param] = elements[i];
        body(i);
      }
      locals.erase(param);
      global.retypeFor(currentFunction, original, currentClassTy);
    }

    // A template type pack that a pattern names in a type (`(func(Args()), ...)`,
    // `(... || isSame<T, Args>)`, `sizeof(Args)`) and the instantiation binds to a pack, or null.
    Decl* typePackIn(Node* n, int depth = 0)
    {
      if (!n || depth > 32 || !currentArgs)
        return nullptr;
      auto inName = [&](const QualifiedName& name) -> Decl*
      {
        for (const NamePart& part : name.parts)
          for (Node* a : part.templateArgs)
            if (Decl* d = typePackIn(a, depth + 1))
              return d;
        return nullptr;
      };
      auto either = [&](Node* a, Node* b) -> Decl* { Decl* d = typePackIn(a, depth + 1); return d ? d : typePackIn(b, depth + 1); };
      switch (n->kind)
      {
        case NodeKind::NamedType:
        {
          auto* t = static_cast<NamedType*>(n);
          if (Decl* d = t->resolved; d && d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isPack)
            for (auto& [p, a] : currentArgs->pairs)
              if (p == d && a && a->kind == TyKind::Pack)
                return d;
          return inName(t->name);
        }
        case NodeKind::PointerType: return typePackIn(static_cast<PointerType*>(n)->pointee, depth + 1);
        case NodeKind::LRefType: return typePackIn(static_cast<LRefType*>(n)->referenced, depth + 1);
        case NodeKind::RRefType: return typePackIn(static_cast<RRefType*>(n)->referenced, depth + 1);
        case NodeKind::ConstType: return typePackIn(static_cast<ConstType*>(n)->inner, depth + 1);
        case NodeKind::NameExpr:
        {
          // `Args()` parsed as a call of the name Args.
          if (Decl* d = static_cast<NameExpr*>(n)->resolved; d && d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isPack)
            for (auto& [p, a] : currentArgs->pairs)
              if (p == d && a && a->kind == TyKind::Pack)
                return d;
          return inName(static_cast<NameExpr*>(n)->name);
        }
        case NodeKind::ParenExpr: return typePackIn(static_cast<ParenExpr*>(n)->inner, depth + 1);
        case NodeKind::UnaryExpr: return typePackIn(static_cast<UnaryExpr*>(n)->operand, depth + 1);
        case NodeKind::MemberExpr: return typePackIn(static_cast<MemberExpr*>(n)->base, depth + 1);
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(n);
          if (Decl* d = typePackIn(i->base, depth + 1))
            return d;
          for (Expr* a : i->args)
            if (Decl* d = typePackIn(a, depth + 1))
              return d;
          return nullptr;
        }
        case NodeKind::CastExpr: return either(static_cast<CastExpr*>(n)->type, static_cast<CastExpr*>(n)->operand);
        case NodeKind::NamedCastExpr: return either(static_cast<NamedCastExpr*>(n)->type, static_cast<NamedCastExpr*>(n)->operand);
        case NodeKind::BinaryExpr: return either(static_cast<BinaryExpr*>(n)->lhs, static_cast<BinaryExpr*>(n)->rhs);
        case NodeKind::SizeofExpr:
        {
          auto* s = static_cast<SizeofExpr*>(n);
          return s->isPack ? nullptr : either(s->type, s->operand); // `sizeof...(Args)` counts the pack, it expands nothing
        }
        case NodeKind::FunctionalCastExpr:
        {
          auto* c = static_cast<FunctionalCastExpr*>(n);
          if (Decl* d = typePackIn(c->type, depth + 1))
            return d;
          for (Expr* a : c->args)
            if (Decl* d = typePackIn(a, depth + 1))
              return d;
          return nullptr;
        }
        case NodeKind::CallExpr:
        {
          auto* c = static_cast<CallExpr*>(n);
          if (Decl* d = typePackIn(c->callee, depth + 1))
            return d;
          for (Expr* a : c->args)
            if (Decl* d = typePackIn(a, depth + 1))
              return d;
          return nullptr;
        }
        default:
          return nullptr;
      }
    }

    // Runs body once per element of the type pack `pack` (typePackIn), the function re-typed with
    // the pack bound to that element (a private copy: retypedPerElement), and the element's
    // arguments in effect.
    template<class F>
    void forEachTypePackElement(Decl* pack, F&& body)
    {
      TemplateArgs original = *currentArgs;
      const Ty* bound = nullptr;
      for (auto& [d, t] : original.pairs)
        if (d == pack)
          bound = t;
      for (size_t i = 0; bound && i < bound->args.size(); ++i)
      {
        // The pattern's mentions of the pack name element i; the parameters, the class instance
        // and its members keep the whole pack (`lists` an `Array<Entry, sizeof...(Ts)>&`).
        TemplateArgs elementArgs = original;
        elementArgs.elementPacks = {pack};
        elementArgs.elementIndex = uint32_t(i);
        global.retypeFor(currentFunction, elementArgs, currentClassTy, true);
        ArgsScope scope(*this, &elementArgs);
        body(i);
      }
      global.retypeFor(currentFunction, original, currentClassTy);
    }

    bool isPackExpansion(Expr* e)
    {
      return e && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->isKeyword &&
             P(static_cast<UnaryExpr*>(e)->op) == P::Ellipsis && static_cast<UnaryExpr*>(e)->postfix;
    }

    // --- lambdas ---

    static bool localNamed(Decl* d, uint32_t symbol)
    {
      if (d && d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
        return static_cast<VariableDecl*>(d)->name.last().symbol == symbol;
      return d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->name == symbol;
    }

    // A variable of a function (a parameter, a local of automatic storage): what a lambda captures.
    static bool functionLocal(const Decl* d)
    {
      return d && (d->kind == NodeKind::ParamDecl ||
                   (d->kind == NodeKind::VariableDecl && !(d->flags & DF_Static) && d->parent && d->parent->kind == NodeKind::FunctionDecl));
    }

    // What a lambda's body names, from its AST alone: the locals of the functions around it (each
    // once, in the order first written: `used`) and whether it uses `this`. inner gathers what is
    // declared inside it (its parameters, its locals, nested lambdas' own), which is no capture;
    // C++ declares a name before its uses, so it is in inner before a use is reached.
    // useTypes, when given, gets the type of each used local's first name expression.
    // keptBranches: an `if constexpr` its instance decided names only what the kept branch names
    // (not in a generic lambda, whose one closure serves every call's instance of the body).
    void captureNames(Node* n, std::unordered_set<Decl*>& inner, std::vector<Decl*>& used, bool& usesThis,
                      std::unordered_map<Decl*, const Ty*>* useTypes = nullptr, int depth = 0, bool keptBranches = false)
    {
      if (!n || depth > 96)
        return;
      auto visit = [&](Node* c) { captureNames(c, inner, used, usesThis, useTypes, depth + 1, keptBranches); };
      auto visitExprs = [&](Span<Expr*> es) { for (Expr* e : es) visit(e); };
      switch (n->kind)
      {
        case NodeKind::NameExpr:
        {
          Decl* d = static_cast<NameExpr*>(n)->resolved;
          if (!d)
            break;
          if (functionLocal(d))
          {
            if (!inner.count(d) && std::find(used.begin(), used.end(), d) == used.end())
            {
              used.push_back(d);
              if (useTypes)
                (*useTypes)[d] = static_cast<NameExpr*>(n)->ty;
            }
          }
          else if ((d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) && global.owningClass(d) && !(d->flags & DF_Static))
            usesThis = true; // a member named without `this->`
          break;
        }
        case NodeKind::VariableDecl: // a condition that declares (`if (auto* p = f())`)
          inner.insert(static_cast<Decl*>(n));
          visit(static_cast<VariableDecl*>(n)->init);
          break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(n)->decls)
            if (d->kind == NodeKind::VariableDecl)
            {
              inner.insert(d);
              visit(static_cast<VariableDecl*>(d)->init);
            }
          break;
        case NodeKind::ThisExpr: usesThis = true; break;
        case NodeKind::CallExpr: visit(static_cast<CallExpr*>(n)->callee); visitExprs(static_cast<CallExpr*>(n)->args); break;
        case NodeKind::IndexExpr: visit(static_cast<IndexExpr*>(n)->base); visitExprs(static_cast<IndexExpr*>(n)->args); break;
        case NodeKind::MemberExpr: visit(static_cast<MemberExpr*>(n)->base); visit(static_cast<MemberExpr*>(n)->dynamicMember); break;
        case NodeKind::UnaryExpr: visit(static_cast<UnaryExpr*>(n)->operand); break;
        case NodeKind::BinaryExpr: visit(static_cast<BinaryExpr*>(n)->lhs); visit(static_cast<BinaryExpr*>(n)->rhs); break;
        case NodeKind::ConditionalExpr:
          visit(static_cast<ConditionalExpr*>(n)->cond);
          visit(static_cast<ConditionalExpr*>(n)->then);
          visit(static_cast<ConditionalExpr*>(n)->else_);
          break;
        case NodeKind::CastExpr: visit(static_cast<CastExpr*>(n)->operand); break;
        case NodeKind::NamedCastExpr: visit(static_cast<NamedCastExpr*>(n)->operand); break;
        case NodeKind::FunctionalCastExpr: visitExprs(static_cast<FunctionalCastExpr*>(n)->args); break;
        case NodeKind::SizeofExpr: visit(static_cast<SizeofExpr*>(n)->operand); break;
        case NodeKind::NewExpr: visitExprs(static_cast<NewExpr*>(n)->placement); visitExprs(static_cast<NewExpr*>(n)->args); break;
        case NodeKind::DeleteExpr: visit(static_cast<DeleteExpr*>(n)->operand); break;
        case NodeKind::BraceInitExpr: visitExprs(static_cast<BraceInitExpr*>(n)->args); break;
        case NodeKind::ThrowExpr: visit(static_cast<ThrowExpr*>(n)->operand); break;
        case NodeKind::ParenExpr: visit(static_cast<ParenExpr*>(n)->inner); break;
        case NodeKind::NameofExpr: visit(static_cast<NameofExpr*>(n)->operand); break;
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(n);
          for (const LambdaCapture& c : l->captures)
          {
            visit(c.init);
            if (c.decl)
              inner.insert(c.decl);
          }
          for (Decl* p : l->params)
            inner.insert(p);
          visit(l->body);
          break;
        }
        case NodeKind::BlockStmt: for (Stmt* s : static_cast<BlockStmt*>(n)->stmts) visit(s); break;
        case NodeKind::ExprStmt: visit(static_cast<ExprStmt*>(n)->expr); break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(n);
          visit(i->init);
          if (keptBranches && i->isConstexpr && i->constexprKnown >= 0)
          {
            // An instance's `if constexpr`: only the branch it keeps, as C++ captures what the
            // instantiated body uses (FluidBox::findOtherUndergroundEnd<BlueprintSurface>'s lambda
            // names `surface` only in the branch it discards, where nothing types it).
            visit(i->constexprKnown ? i->then : i->else_);
            break;
          }
          visit(i->cond);
          visit(i->then);
          visit(i->else_);
          break;
        }
        case NodeKind::ForStmt:
          visit(static_cast<ForStmt*>(n)->init);
          visit(static_cast<ForStmt*>(n)->cond);
          visit(static_cast<ForStmt*>(n)->step);
          visit(static_cast<ForStmt*>(n)->body);
          break;
        case NodeKind::RangeForStmt:
          visit(static_cast<RangeForStmt*>(n)->range);
          if (Decl* v = static_cast<RangeForStmt*>(n)->var)
            inner.insert(v);
          visit(static_cast<RangeForStmt*>(n)->body);
          break;
        case NodeKind::ForConstexprStmt: visit(static_cast<ForConstexprStmt*>(n)->body); break;
        case NodeKind::WhileStmt: visit(static_cast<WhileStmt*>(n)->cond); visit(static_cast<WhileStmt*>(n)->body); break;
        case NodeKind::DoStmt: visit(static_cast<DoStmt*>(n)->body); visit(static_cast<DoStmt*>(n)->cond); break;
        case NodeKind::SwitchStmt:
          visit(static_cast<SwitchStmt*>(n)->init);
          visit(static_cast<SwitchStmt*>(n)->cond);
          visit(static_cast<SwitchStmt*>(n)->body);
          break;
        case NodeKind::CaseStmt: visit(static_cast<CaseStmt*>(n)->value); break;
        case NodeKind::ReturnStmt: visit(static_cast<ReturnStmt*>(n)->value); break;
        case NodeKind::AssertStmt: visit(static_cast<AssertStmt*>(n)->cond); break;
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(n);
          visit(t->body);
          for (const TryStmt::Handler& h : t->handlers)
          {
            if (h.param)
              inner.insert(h.param);
            visit(h.body);
          }
          break;
        }
        default: break;
      }
    }

    // A lambda's captures from its AST alone, in a fixed order: its capture list, then what a
    // default capture picks up in the order first written, then `this`. The same list wherever the
    // closure is laid out or built, whichever function is emitted first (the body names the
    // instantiation's own locals: a capture's position, not its declaration, matches them up).
    struct CaptureSlot
    {
      Decl* decl = nullptr; // the captured variable (null for `this`)
      bool byRef = false;
      Expr* init = nullptr; // an init-capture's initializer
    };
    std::vector<CaptureSlot> captureList(LambdaExpr* l, std::unordered_map<Decl*, const Ty*>* useTypes = nullptr)
    {
      std::unordered_set<Decl*> inner;
      for (Decl* p : l->params)
        inner.insert(p);
      for (const LambdaCapture& cap : l->captures)
        if (cap.decl)
          inner.insert(cap.decl);
      std::vector<Decl*> used;
      bool usesThis = false;
      captureNames(l->body, inner, used, usesThis, useTypes, 0, l->templateParams.empty());
      std::vector<CaptureSlot> out;
      bool thisCaptured = false;
      for (const LambdaCapture& cap : l->captures)
      {
        if (cap.init)
        {
          // `[x = expr]`: a field of the closure, initialised from expr when the closure is made;
          // `[&x = expr]`: one pointing at what expr names.
          if (!cap.decl)
            unsupported(l, "an init-capture without its variable");
          out.push_back({cap.decl, cap.byRef, cap.init});
          continue;
        }
        if (cap.isThis)
        {
          thisCaptured = true;
          continue;
        }
        Decl* d = nullptr;
        for (Decl* u : used)
          if (localNamed(u, cap.symbol) || bindsName(u, cap.symbol))
            d = d ? d : u;
        if (!d)
          d = unusedCapture(l, cap.symbol);
        // `[mixingConnection]` of a structured binding: the object it names a part of, captured
        // the same way (the body reaches the part through it, as outside the lambda); once for
        // several of its bindings.
        if (std::none_of(out.begin(), out.end(), [&](const CaptureSlot& s) { return s.decl == d && !s.init; }))
          out.push_back({d, cap.byRef, nullptr});
      }
      if (l->defaultCapture != 0)
      {
        for (Decl* d : used)
          if (std::none_of(out.begin(), out.end(), [&](const CaptureSlot& s) { return s.decl == d; }))
            out.push_back({d, l->defaultCapture == 2, nullptr});
        thisCaptured = thisCaptured || usesThis;
      }
      if (thisCaptured)
        out.push_back({nullptr, true, nullptr});
      return out;
    }

    // Whether d is the hidden object of a structured binding declaration that names symbol.
    static bool bindsName(Decl* d, uint32_t symbol)
    {
      if (!d || d->kind != NodeKind::VariableDecl)
        return false;
      for (Node* b : static_cast<VariableDecl*>(d)->structuredBindings)
        if (b && b->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(b)->name.empty() && static_cast<NameExpr*>(b)->name.last().symbol == symbol)
          return true;
      return false;
    }

    // An explicit capture the body does not use (`[guard]` kept alive): the one local of that name
    // declared before the lambda in the function around it; with several, which one is in scope
    // the code generator cannot tell from the names alone.
    Decl* unusedCapture(LambdaExpr* l, uint32_t symbol)
    {
      Decl* f = l->closure ? l->closure->parent : nullptr;
      while (f && f->kind != NodeKind::FunctionDecl)
        f = f->parent;
      if (!f)
        unsupported(l, "a capture that is not a local variable (" + sym(symbol) + ")");
      auto* fn = static_cast<FunctionDecl*>(f);
      std::unordered_set<Decl*> declared(fn->params.begin(), fn->params.end());
      std::vector<Decl*> ignored;
      bool ignoredThis = false;
      captureNames(fn->body, declared, ignored, ignoredThis);
      Decl* only = nullptr;
      for (Decl* d : declared)
        if (localNamed(d, symbol) && d->token < l->token)
        {
          if (only)
            unsupported(l, "a capture of " + sym(symbol) + " that the lambda does not use, with several locals of that name in the function");
          only = d;
        }
      if (!only)
        unsupported(l, "a capture that is not a local variable (" + sym(symbol) + ")");
      return only;
    }

    // The type a captured variable has in the closure (references stripped): the variable's declared
    // type where the declaration alone gives it, an init-capture's as its initializer deduces it,
    // else as the lambda's body first names it (useTypes: an instantiation's local of a dependent
    // type, typed in place).
    static bool mentionsAuto(const Ty* t, int depth = 0)
    {
      if (!t || depth > 16)
        return false;
      if (t->kind == TyKind::Auto || mentionsAuto(t->inner, depth + 1))
        return true;
      for (const Ty* a : t->args)
        if (mentionsAuto(a, depth + 1))
          return true;
      return false;
    }

    const Ty* captureType(Decl* d, Expr* init, const std::unordered_map<Decl*, const Ty*>& useTypes, LambdaExpr* l)
    {
      auto exact = [&](const Ty* t) { return t && t->isKnown() && !types.isDependent(t) && !mentionsAuto(t); };
      if (const Ty* cached = d->cachedType.load(std::memory_order_acquire); exact(cached))
        return types.stripRef(cached); // as the instantiation's walk typed its own copy of the local
      const Ty* t = global.typeOfDecl(d);
      if (exact(t))
        return types.stripRef(t);
      if (init && init->ty && init->ty->isKnown() && !types.isDependent(init->ty))
        return types.decay(types.stripRef(init->ty)); // `[x = expr]`: as auto deduces it
      if (auto it = useTypes.find(d); it != useTypes.end() && it->second && it->second->isKnown() && !types.isDependent(it->second))
        return types.stripRef(it->second);
      unsupported(l, "a capture of " + (d->kind == NodeKind::VariableDecl ? sym(static_cast<VariableDecl*>(d)->name.last().symbol) : std::string("a parameter")) +
                         " whose type the code generator could not work out");
    }

    // The closure of a lambda type, created from its capture list in the current function.
    // The IR name of a lambda's closure struct: known as soon as the type is mentioned (a slot
    // for the variable holding it is allocated before its expression is reached); the struct is
    // defined once the captures are known.
    std::unordered_map<const Ty*, std::string> closureNames; // this emitter's view of the names below
    std::string closureStructName(const Ty* t)
    {
      t = types.unqualified(t); // `const auto f = [..]{..}`: the const variable holds the same closure
      if (auto mine = closureNames.find(t); mine != closureNames.end())
        return mine->second;
      auto lock = sharedLock();
      ClosureInfo& info = closures[t];
      if (info.structName.empty())
        info.structName = irTypeName("%closure.", t);
      closureNames.emplace(t, info.structName);
      return info.structName;
    }

    // The closure of a lambda type with its captures laid out, from the lambda's AST (captureList),
    // by whichever function needs it first: the function the lambda is written in, or one that
    // holds or calls a closure it returned (an instance of a template that returns a lambda is
    // emitted after its caller). The layout is the same either way.
    std::unordered_map<const Ty*, ClosureInfo*> closureCache; // laid-out closures this emitter used
    ClosureInfo& closureFor(const Ty* t, Node* at, bool = false)
    {
      t = types.unqualified(t);
      if (auto mine = closureCache.find(t); mine != closureCache.end())
        return *mine->second;
      auto lock = sharedLock(); // a laid-out closure does not change, so its reference may be read after
      auto it = closures.find(t);
      if (it != closures.end() && it->second.laidOut)
      {
        closureCache.emplace(t, &it->second);
        return it->second;
      }
      if (it != closures.end() && !it->second.failure.empty())
        fail(at, "the closure of " + irString(t) + " could not be laid out (" + it->second.failure + ")");
      size_t errorsBefore = errors.size();
      try
      {
        return layOutClosure(t, at);
      }
      catch (Unsupported&)
      {
        // Every use, the first one like the others, reports the closure and why: the same lines
        // whichever function asks first.
        std::string why = lastFailure;
        closures[t].failure = why;
        errors.resize(errorsBefore);
        fail(at, "the closure of " + irString(t) + " could not be laid out (" + why + ")");
      }
    }

    // closureFor's layout, from the lambda expression being emitted (at) or the closure's own.
    ClosureInfo& layOutClosure(const Ty* t, Node* at)
    {
      auto* c = static_cast<ClassDecl*>(t->decl);
      LambdaExpr* l = c->lambda; // the instantiation's own lambda: the same whoever lays it out
      if (!l)
        unsupported(at, "a lambda without its expression");
      // Laid out aside and published whole: a capture that fails (an unsupported one, a field whose
      // class cannot be laid out) leaves the closure unbuilt for every later use, not half built.
      ClosureInfo info;
      info.structName = closureStructName(t);
      std::unordered_map<Decl*, const Ty*> useTypes;
      for (const CaptureSlot& s : captureList(l, &useTypes))
      {
        info.captured.push_back(s.decl);
        info.byRef.push_back(s.byRef);
        info.inits.push_back(s.init);
        // `this`: a pointer field; the class it points to is the writer's (enclosingClass).
        info.fieldTypes.push_back(s.decl ? captureType(s.decl, s.init, useTypes, l) : types.pointer(types.voidType()));
      }
      std::string line = info.structName + " = type { ";
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
        line += (i ? ", " : "") + (info.byRef[i] ? std::string("ptr") : llType(info.fieldTypes[i], at));
      line += info.fieldTypes.empty() ? "i8 }\n" : " }\n";
      const Ty* ft = t;
      std::string params;
      for (size_t i = 0; i < ft->args.size(); ++i)
        params += (i ? ", " : "") + irString(ft->args[i]);
      info.functionName = "@\"" + irString(t) + "::operator()(" + params + ")\"";
      info.laidOut = true;
      decls += line;
      ClosureInfo& published = closures[t];
      info.enclosingClass = published.enclosingClass; // set by the writer (makeClosure), whenever it comes
      info.writtenIn = published.writtenIn;
      published = std::move(info);
      return published;
    }

    // Lays out the closure of every lambda written in n (not in a lambda's body), before n is
    // emitted: a slot for a class holding one (`auto h = hold([base](int x) { ... })` and its
    // Holder<closure>, a call's result) is laid out before the expression that builds the closure
    // is reached. The captures are the locals in scope where the statement starts.
    void layOutClosuresIn(Node* n)
    {
      if (!n)
        return;
      switch (n->kind)
      {
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(n);
          for (const LambdaCapture& c : l->captures)
            layOutClosuresIn(c.init);
          if (isLambda(l->ty))
            closureFor(l->ty, l, true);
          return;
        }
        case NodeKind::CallExpr:
          layOutClosuresIn(static_cast<CallExpr*>(n)->callee);
          for (Expr* a : static_cast<CallExpr*>(n)->args)
            layOutClosuresIn(a);
          return;
        case NodeKind::MemberExpr: layOutClosuresIn(static_cast<MemberExpr*>(n)->base); return;
        case NodeKind::IndexExpr:
          layOutClosuresIn(static_cast<IndexExpr*>(n)->base);
          for (Expr* a : static_cast<IndexExpr*>(n)->args)
            layOutClosuresIn(a);
          return;
        case NodeKind::UnaryExpr: layOutClosuresIn(static_cast<UnaryExpr*>(n)->operand); return;
        case NodeKind::BinaryExpr:
          layOutClosuresIn(static_cast<BinaryExpr*>(n)->lhs);
          layOutClosuresIn(static_cast<BinaryExpr*>(n)->rhs);
          return;
        case NodeKind::ConditionalExpr:
          layOutClosuresIn(static_cast<ConditionalExpr*>(n)->cond);
          layOutClosuresIn(static_cast<ConditionalExpr*>(n)->then);
          layOutClosuresIn(static_cast<ConditionalExpr*>(n)->else_);
          return;
        case NodeKind::CastExpr: layOutClosuresIn(static_cast<CastExpr*>(n)->operand); return;
        case NodeKind::NamedCastExpr: layOutClosuresIn(static_cast<NamedCastExpr*>(n)->operand); return;
        case NodeKind::FunctionalCastExpr:
          for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
            layOutClosuresIn(a);
          return;
        case NodeKind::NewExpr:
          for (Expr* a : static_cast<NewExpr*>(n)->placement)
            layOutClosuresIn(a);
          for (Expr* a : static_cast<NewExpr*>(n)->args)
            layOutClosuresIn(a);
          return;
        case NodeKind::BraceInitExpr:
          for (Expr* a : static_cast<BraceInitExpr*>(n)->args)
            layOutClosuresIn(a);
          return;
        case NodeKind::ParenExpr: layOutClosuresIn(static_cast<ParenExpr*>(n)->inner); return;
        case NodeKind::ThrowExpr: layOutClosuresIn(static_cast<ThrowExpr*>(n)->operand); return;
        default: return;
      }
    }

    // A closure's captures by value, as (field index, type): what copying, moving and destroying
    // the closure copy, move and destroy one by one (a capture by reference, and `this`, is a pointer).
    std::vector<std::pair<uint32_t, const Ty*>> closureValueFields(const Ty* t, Node* at)
    {
      ClosureInfo& info = closureFor(t, at);
      std::vector<std::pair<uint32_t, const Ty*>> fields;
      for (size_t i = 0; i < info.captured.size(); ++i)
        if (info.captured[i] && !info.byRef[i])
          fields.push_back({uint32_t(i), info.fieldTypes[i]});
      return fields;
    }

    // Copies (move: moves) the closure at src into dst capture by capture, each through its own copy
    // or move constructor: EntityWithOwner's `[entity = Targeter<EntityWithOwner>(this)]` copied
    // bitwise into a load helper left the copy out of the entity's targeters and the stack's closure
    // in them, never destroyed.
    void copyClosure(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move)
    {
      ClosureInfo& info = closureFor(t, at);
      for (size_t i = 0; i < info.captured.size(); ++i)
      {
        std::string to = gep(info.structName, dst, uint32_t(i)), from = gep(info.structName, src, uint32_t(i));
        const Ty* ft = info.fieldTypes[i];
        bool byValue = info.captured[i] && !info.byRef[i];
        if (byValue && isClass(ft))
        {
          if (move)
            moveConstruct(to, from, ft, at);
          else
            copyConstruct(to, from, ft, at);
          continue;
        }
        if (byValue && isLambda(ft))
        {
          copyClosure(to, from, ft, at, move);
          continue;
        }
        if (byValue && ft->kind == TyKind::Array)
        {
          transferArray(to, from, ft, at, move, false);
          continue;
        }
        std::string llt = byValue ? llType(ft, at) : std::string("ptr"); // a scalar, or a pointer: by reference, `this`
        std::string v = newTemp();
        fn += "  " + v + " = load " + llt + ", ptr " + from + "\n";
        fn += "  store " + llt + " " + v + ", ptr " + to + "\n";
      }
    }

    // Initialises the closure at addr from init, a closure value: a lambda expression is built in
    // place, a temporary moved from (and destroyed with the statement), anything else copied (moved
    // for `move(f)`).
    void initializeClosure(const std::string& addr, const Ty* t, Expr* init, Node* at)
    {
      Expr* inner = init;
      while (inner && inner->kind == NodeKind::ParenExpr)
        inner = static_cast<ParenExpr*>(inner)->inner;
      if (inner && inner->kind == NodeKind::LambdaExpr && types.unqualified(inner->ty) == types.unqualified(t))
      {
        makeClosure(static_cast<LambdaExpr*>(inner), addr);
        return;
      }
      Value v = rvalue(init);
      if (!isLambda(v.ty))
        unsupported(at, "initialising a lambda variable from something else");
      if (v.isTemporary && relocatesBitwise(t, at))
      {
        // Its bytes are the closure now: the statement no longer destroys the temporary.
        for (size_t i = statementTemps.size(); i-- > 0;)
          if (statementTemps[i].addr == v.text)
            statementTemps.erase(statementTemps.begin() + i);
        std::string tmp = newTemp();
        fn += "  " + tmp + " = load " + llType(t, at) + ", ptr " + v.text + "\n";
        fn += "  store " + llType(t, at) + " " + tmp + ", ptr " + addr + "\n";
        return;
      }
      copyClosure(addr, v.text, t, at, v.isTemporary || isXvalue(init));
    }

    // Builds the closure object of a lambda expression in a fresh slot (into: in that object) and
    // emits its function. One in a fresh slot is a temporary: the statement destroys it unless it
    // is moved out (a parameter by value, a variable), when its captures need destruction.
    Value makeClosure(LambdaExpr* l, const std::string& into = "")
    {
      const Ty* t = l->ty;
      if (!isLambda(t))
        unsupported(l, "a lambda whose type is unknown");
      ClosureInfo info = closureFor(t, l, true);
      {
        // The function the lambda is written in: its class is what `this` in the body points to,
        // its name the requester the operator's diagnostics give.
        auto lock = sharedLock();
        ClosureInfo& shared = closures[types.unqualified(t)];
        if (shared.writtenIn.empty())
        {
          shared.writtenIn = currentFunctionName;
          shared.enclosingClass = currentClassTy;
        }
        info.writtenIn = shared.writtenIn;
        info.enclosingClass = shared.enclosingClass;
      }
      // This lambda's own captures, field by field (in a private copy of an instantiation they name
      // the copy's locals; the layout was read from the instantiation's).
      std::vector<CaptureSlot> slots = captureList(l);
      if (slots.size() != info.captured.size())
        unsupported(l, "a lambda whose captures differ from its closure's");
      std::string slot = into.empty() ? newObjectSlot(t, l) : into;
      for (size_t i = 0; i < info.captured.size(); ++i)
      {
        std::string field = gep(info.structName, slot, uint32_t(i));
        Decl* d = slots[i].decl;
        if (!d)
        {
          fn += "  store ptr " + thisPointer(l) + ", ptr " + field + "\n";
          continue;
        }
        if (slots[i].init && slots[i].byRef)
        {
          // `[&entities = this->entities]`: the field points at what the initializer names.
          Expr* init = slots[i].init;
          std::string target = isClass(types.stripRef(init->ty)) ? rvalue(init).text : lvalue(init);
          fn += "  store ptr " + target + ", ptr " + field + "\n";
          continue;
        }
        if (slots[i].init)
        {
          initializeFrom(field, info.fieldTypes[i], slots[i].init, l); // `[x = expr]`
          continue;
        }
        if (!locals.count(d))
          unsupported(l, "a capture of a variable the function being emitted does not have");
        Local& local = locals[d];
        std::string addr = local.addr;
        if (local.isRef)
        {
          addr = newTemp();
          fn += "  " + addr + " = load ptr, ptr " + local.addr + "\n";
        }
        if (info.byRef[i])
          fn += "  store ptr " + addr + ", ptr " + field + "\n";
        else if (isAggregate(info.fieldTypes[i]))
        {
          if (isClass(info.fieldTypes[i]))
            copyConstruct(field, addr, info.fieldTypes[i], l);
          else
            copyClosure(field, addr, info.fieldTypes[i], l, false); // `[f]` of a closure
        }
        else
          store(load(addr, info.fieldTypes[i], l), field, info.fieldTypes[i], l);
      }
      bool first = false; // this emitter emits the operator (a generic lambda's is emitted per call, once its types are known)
      {
        auto lock = sharedLock();
        if (!isGenericLambda(t))
          first = sh.closuresEmitted.insert(t).second;
      }
      if (first)
        emitLambda(l, t, types.function(t->inner ? t->inner : types.voidType(), t->args, false), info.functionName, nullptr);
      if (!into.empty())
        return {slot, t, false};
      if (needsDestruction(t, l))
        statementTemps.push_back({slot, t});
      return {slot, t, true};
    }

    struct FunctionState
    {
      std::string fn, allocas, currentBlock, sretAddr;
      std::unordered_map<Decl*, Local> locals;
      std::unordered_map<uint32_t, std::string> labelBlocks; // a lambda emitted between a goto and its label keeps the label's block
      std::vector<Loop> loops;
      std::vector<std::vector<Object>> scopes;
      std::vector<Object> statementTemps;
      uint32_t temp, label;
      bool blockTerminated;
      FunctionDecl* currentFunction;
      const Ty* currentReturn;
      const Ty* currentClassTy;
      std::vector<EHFrame> ehFrames;
      std::string ehBlocks;
      std::unordered_map<std::string, std::string> cleanupPads;
      bool usesEH;
      std::string returnLabel;
      std::unordered_map<std::string, std::string> inlineSlots;
    };

    FunctionState saveState()
    {
      return {fn, allocas, currentBlock, sretAddr, locals, labelBlocks, loops, scopes, statementTemps, temp, label, blockTerminated,
              currentFunction, currentReturn, currentClassTy, ehFrames, ehBlocks, cleanupPads, usesEH, returnLabel, inlineSlots};
    }

    void restoreState(FunctionState& s)
    {
      fn = s.fn;
      allocas = s.allocas;
      currentBlock = s.currentBlock;
      sretAddr = s.sretAddr;
      locals = s.locals;
      labelBlocks = s.labelBlocks;
      loops = s.loops;
      scopes = s.scopes;
      statementTemps = s.statementTemps;
      temp = s.temp;
      label = s.label;
      blockTerminated = s.blockTerminated;
      currentFunction = s.currentFunction;
      currentReturn = s.currentReturn;
      currentClassTy = s.currentClassTy;
      ehFrames = s.ehFrames;
      ehBlocks = s.ehBlocks;
      cleanupPads = s.cleanupPads;
      usesEH = s.usesEH;
      returnLabel = s.returnLabel;
      inlineSlots = s.inlineSlots;
    }

    void resetFunctionState()
    {
      returnLabel.clear();
      fn.clear();
      allocas.clear();
      locals.clear();
      labelBlocks.clear();
      loops.clear();
      scopes.clear();
      statementTemps.clear();
      ehFrames.clear();
      ehBlocks.clear();
      cleanupPads.clear();
      usesEH = false;
      inlineSlots.clear();
      temp = 0;
      label = 0;
      blockTerminated = false;
      currentBlock = "entry";
      recursionDepth = 0;
    }

    // A function's definition text: the personality and the cleanup pads when it uses exceptions.
    std::string functionText(std::string header)
    {
      if (usesEH)
        header.insert(header.rfind("{\nentry:"), "personality ptr @__C_specific_handler ");
      return header + allocas + fn + ehBlocks + "}\n\n";
    }

    // Emits the lambda's function while another function is being emitted: the enclosing
    // function's state is saved and restored around it. callTy: the operator's type (parameters
    // and result); name: its IR name; args: for an instantiation of a generic lambda, the
    // enclosing instantiation's arguments plus its own, which declarations in the body are typed under.
    void emitLambda(LambdaExpr* l, const Ty* t, const Ty* callTy, const std::string& name, const TemplateArgs* args)
    {
      ClosureInfo info = closureFor(t, l);
      FunctionState saved = saveState();
      const TemplateArgs* savedArgs = currentArgs;
      ParsedFile* savedFile = currentFile;
      std::string savedName = currentFunctionName;
      struct NameRestore { std::string& name; std::string saved; ~NameRestore() { name = saved; } } nameRestore{currentFunctionName, savedName};
      currentFunctionName = name; // what the operator's own statics and filters are named after
      // Its diagnostics name the function it is written in as the requester, not the function that
      // happened to emit it first.
      NameRestore requesterRestore{currentRequestedBy, currentRequestedBy};
      currentRequestedBy = info.writtenIn;
      struct ContextRestore
      {
        Decl*& lambda;
        Decl*& initializer;
        Decl* savedLambda;
        Decl* savedInitializer;
        ~ContextRestore()
        {
          lambda = savedLambda;
          initializer = savedInitializer;
        }
      } contextRestore{lambdaContext, initializerOf, lambdaContext, initializerOf};
      // One written in a function body (whose declarations the closure's context reaches); one in
      // a constructor's initializer list or a member's initializer is typed as before, from the
      // function or the initializer being emitted.
      if (l->closure && l->closure->parent && l->closure->parent->kind == NodeKind::FunctionDecl)
      {
        lambdaContext = l->closure;
        initializerOf = nullptr;
      }
      resetFunctionState();
      if (args)
        currentArgs = args;
      if (l->closure && l->closure->file)
        currentFile = l->closure->file;
      currentReturn = callTy->inner ? callTy->inner : types.voidType();
      if (!currentReturn->isKnown())
        currentReturn = types.pointer(types.voidType());
      sretAddr.clear();
      currentClassTy = info.enclosingClass;
      try
      {
        std::string params = "ptr %closure";
        bool hasSret = returnsInMemory(currentReturn);
        if (hasSret)
        {
          params += ", ptr %sret";
          sretAddr = "%sret";
        }
        std::vector<std::string> paramNames;
        for (size_t i = 0; i < callTy->args.size(); ++i)
        {
          const Ty* pt = callTy->args[i];
          std::string pn = "%p" + std::to_string(i);
          paramNames.push_back(pn);
          params += ", " + std::string(isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, l)) + " " + pn;
        }
        std::string retType = hasSret ? "void" : currentReturn->isReference() ? "ptr" : llType(currentReturn, l);
        std::string header = "define " + retType + " " + name + "(" + params + ") {\nentry:\n";
        pushScope();
        // Captures become locals: by-value captures live in the closure, references point out.
        // The body's own names for them, field by field (captureList of the lambda emitted).
        std::vector<CaptureSlot> slots = captureList(l);
        if (slots.size() != info.captured.size())
          unsupported(l, "a lambda whose captures differ from its closure's");
        for (size_t i = 0; i < info.captured.size(); ++i)
        {
          std::string field = gep(info.structName, "%closure", uint32_t(i));
          Decl* d = slots[i].decl;
          if (!d)
          {
            std::string thisSlot = "%v" + std::to_string(temp++);
            allocas += "  " + thisSlot + " = alloca ptr\n";
            std::string loaded = newTemp();
            fn += "  " + loaded + " = load ptr, ptr " + field + "\n";
            fn += "  store ptr " + loaded + ", ptr " + thisSlot + "\n";
            locals[nullptr] = {thisSlot, nullptr, false};
            continue;
          }
          locals[d] = {field, info.byRef[i] ? types.lref(info.fieldTypes[i]) : info.fieldTypes[i], info.byRef[i]};
        }
        for (size_t i = 0; i < l->params.size() && i < callTy->args.size(); ++i)
        {
          const Ty* pt = callTy->args[i];
          if (isAggregate(pt))
          {
            locals[l->params[i]] = {paramNames[i], pt, false};
            if (needsDestruction(pt, l))
              scopes.back().push_back({paramNames[i], pt});
            continue;
          }
          bool isRef = pt->isReference();
          std::string ps = "%v" + std::to_string(temp++);
          allocas += "  " + ps + " = alloca " + (isRef ? std::string("ptr") : llType(pt, l)) + "\n";
          fn += "  store " + (isRef ? std::string("ptr") : llType(pt, l)) + " " + paramNames[i] + ", ptr " + ps + "\n";
          locals[l->params[i]] = {ps, pt, isRef};
        }
        statement(l->body);
        if (!blockTerminated)
        {
          popScope();
          if (currentReturn->kind == TyKind::Void || hasSret)
            fn += "  ret void\n";
          else
            fn += "  ret " + (currentReturn->isReference() ? std::string("ptr null") : llType(currentReturn, l) + " zeroinitializer") + "\n";
        }
        addDefinition(name, functionText(header));
        countFunction();
      }
      catch (Unsupported&)
      {
        restoreState(saved);
        currentArgs = savedArgs;
        currentFile = savedFile;
        throw;
      }
      restoreState(saved);
      currentArgs = savedArgs;
      currentFile = savedFile;
    }

    // The call operator of a (non-generic) lambda a call needs, emitted from the lambda's AST by
    // whoever needs it first when its writer has not (a lambda a function returns, whose caller may
    // be emitted before it; a private copy's lambda type the writer never makes: the callers of
    // RenderParameters::getScreenPositionFunction). Only for a lambda written outside templates:
    // an instance's arguments, which its body may need, are known to its writer alone.
    void emitCallOperatorOnDemand(const Ty* t, const Ty* callTy, const std::string& name)
    {
      LambdaExpr* l = lambdaOf(t);
      if (!l || !l->closure)
        return;
      Decl* writer = l->closure->parent;
      while (writer && writer->kind != NodeKind::FunctionDecl)
      {
        if (!writer->templateParams.empty())
          return;
        writer = writer->parent;
      }
      if (!writer || !writer->templateParams.empty())
        return;
      Decl* owner = global.owningClass(writer);
      for (Decl* o = owner; o; o = o->parent)
        if (!o->templateParams.empty() || (o->kind == NodeKind::FunctionDecl && o != writer))
          return; // a member of a class template, or of a local class (inside a function that may be one)
      {
        auto lock = sharedLock();
        if (!sh.closuresEmitted.insert(t).second)
          return;
        ClosureInfo& shared = closures[types.unqualified(t)];
        if (!shared.enclosingClass && owner && owner->kind == NodeKind::ClassDecl)
          shared.enclosingClass = types.classType(owner, {}); // what `this` in the body points to
      }
      TemplateArgs none;
      emitLambda(l, t, callTy, name, &none);
    }

    // A lambda without captures used as a pointer to function: a function with the pointer's
    // signature that calls the lambda's operator with no closure (nothing is captured), defined
    // once per lambda.
    std::string lambdaThunk(const Ty* t, const Ty* fnTy, Node* at)
    {
      t = types.unqualified(t);
      LambdaExpr* l = lambdaOf(t);
      ClosureInfo info = closureFor(t, at);
      if (!info.captured.empty())
        unsupported(at, "a lambda with captures used as a pointer to function");
      // The operator the pointer calls: the lambda's, or for a generic one (`[](const auto&
      // parameters, MapDeserialiser& input) -> ElemType*` as InstanceLoader's LoadFunction) the
      // instance whose parameters the pointer's give, each `auto` the pointer's type there.
      const Ty* callTy = types.function(t->inner ? t->inner : types.voidType(), t->args, false);
      std::string functionName = info.functionName;
      std::string suffix;
      if (l && !l->templateParams.empty())
      {
        if (fnTy->args.size() != t->args.size())
          unsupported(at, "a lambda used as a pointer to a function of another signature");
        TemplateArgs own;
        for (Decl* p : l->templateParams)
        {
          const Ty* found = nullptr;
          for (size_t i = 0; i < t->args.size() && !found; ++i)
            if (const Ty* pt = types.unqualified(types.stripRef(t->args[i])); pt->kind == TyKind::Param && pt->decl == p)
              found = types.unqualified(types.stripRef(fnTy->args[i]));
          if (!found)
            unsupported(at, "a generic lambda used as a pointer to function, its parameters not deduced from the pointer's");
          own.pairs.push_back({p, found});
        }
        TemplateArgs instanceArgs;
        LambdaExpr* genericBody = nullptr;
        callTy = global.lambdaInstance(t, own, &instanceArgs, &genericBody);
        if (!callTy || !genericBody)
          unsupported(at, "a generic lambda whose body could not be typed for the pointer's parameters");
        std::string params;
        for (size_t i = 0; i < callTy->args.size(); ++i)
          params += (i ? ", " : "") + irString(callTy->args[i]);
        functionName = genericOperatorName(t, own, callTy);
        suffix = "(" + params + ")";
        bool firstUse;
        {
          auto lock = sharedLock();
          firstUse = lambdaInstances.insert(functionName).second;
        }
        if (firstUse)
          emitLambda(genericBody, t, callTy, functionName, &instanceArgs);
      }
      const Ty* ret = fnTy->inner;
      const Ty* lambdaRet = callTy->inner ? callTy->inner : types.voidType();
      if (types.unqualified(types.stripRef(ret)) != types.unqualified(types.stripRef(lambdaRet)) || fnTy->args.size() != callTy->args.size())
        unsupported(at, "a lambda used as a pointer to a function of another signature");
      for (size_t i = 0; i < fnTy->args.size(); ++i)
      {
        const Ty* pt = fnTy->args[i];
        if (types.unqualified(types.stripRef(pt)) != types.unqualified(types.stripRef(callTy->args[i])))
          unsupported(at, "a lambda used as a pointer to a function of another signature");
        if (isClass(pt) && needsDestruction(pt, at))
          unsupported(at, "a lambda used as a pointer to function taking an object with a destructor by value");
      }
      std::string name = "@\"" + irString(t) + "::thunk" + suffix + "\"";
      {
        auto lock = sharedLock();
        if (!lambdaInstances.insert(name).second)
          return name;
      }
      bool hasSret = returnsInMemory(ret);
      std::string retType = hasSret ? "void" : (!ret || !ret->isKnown()) ? "ptr" : ret->isReference() ? "ptr" : llType(ret, at);
      std::string params, args = "ptr null";
      if (hasSret)
      {
        params = "ptr %sret";
        args += ", ptr %sret";
      }
      for (size_t i = 0; i < fnTy->args.size(); ++i)
      {
        const Ty* pt = fnTy->args[i];
        std::string ty = isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, at);
        params += (params.empty() ? "" : ", ") + ty + " %a" + std::to_string(i);
        args += ", " + ty + " %a" + std::to_string(i);
      }
      std::string text = "define " + retType + " " + name + "(" + params + ") {\nentry:\n";
      if (hasSret || ret->kind == TyKind::Void)
        text += "  call void " + functionName + "(" + args + ")\n  ret void\n";
      else
        text += "  %r = call " + retType + " " + functionName + "(" + args + ")\n  ret " + retType + " %r\n";
      text += "}\n\n";
      addDefinition(name, text, true); // emitted once, at its first use: its own, not the user's
      countFunction();
      return name;
    }

    // Calls a closure value with arguments.
    // The call operator of generic lambda t for its own arguments, one per set of them: named by
    // those arguments and the parameter types they give (a template parameter no parameter
    // mentions tells instances apart only by its argument: `[]<class T>(const char* name)`, called
    // `runTest.operator()<CraftingMachine>(name)` and `runTest.operator()<RocketSilo>(name)`).
    std::string genericOperatorName(const Ty* t, const TemplateArgs& own, const Ty* callTy)
    {
      std::string args, params;
      for (size_t i = 0; i < own.pairs.size(); ++i)
        args += (i ? ", " : "") + irString(own.pairs[i].second);
      for (size_t i = 0; i < callTy->args.size(); ++i)
        params += (i ? ", " : "") + irString(callTy->args[i]);
      return "@\"" + irString(t) + "::operator()<" + args + ">(" + params + ")\"";
    }

    Value callClosure(Value closure, Span<Expr*> callArgs, const std::string& sret, Node* at)
    {
      const Ty* t = types.unqualified(closure.ty); // a const lambda variable calls the same operator
      ClosureInfo info = closureFor(t, at);
      const Ty* callTy = types.function(t->inner ? t->inner : types.voidType(), t->args, false);
      std::string functionName = info.functionName;
      TemplateArgs instanceArgs;
      LambdaExpr* genericBody = nullptr; // a generic lambda's copy of its parameters and body for this call's arguments
      LambdaExpr* genericLambda = lambdaOf(t);
      if (genericLambda && genericLambda->templateParams.empty())
        genericLambda = nullptr;
      if (LambdaExpr* l = genericLambda)
      {
        // A generic lambda: the call's deduced arguments (recorded by the type checker, as for a
        // function template) pick the instantiation of its call operator, emitted at first use.
        CallExpr* call = at && at->kind == NodeKind::CallExpr ? static_cast<CallExpr*>(at) : nullptr;
        if (!call || call->templateArgCount != l->templateParams.size())
          unsupported(at, "a call to a generic lambda whose parameter types were not deduced");
        TemplateArgs own;
        for (size_t i = 0; i < l->templateParams.size(); ++i)
          own.pairs.push_back({l->templateParams[i], call->templateArgs[i]});
        callTy = global.lambdaInstance(t, own, &instanceArgs, &genericBody);
        if (!callTy || !genericBody)
          unsupported(at, "a generic lambda whose body could not be typed for this call");
        functionName = genericOperatorName(t, own, callTy);
      }
      else
      {
        // Emitted with the function the lambda is written in: checked once generation is done.
        {
          auto lock = sharedLock();
          sh.closureCalls.emplace(functionName, t);
        }
        emitCallOperatorOnDemand(t, callTy, functionName);
      }
      const Ty* ret = callTy->inner ? callTy->inner : types.voidType();
      std::string argText = "ptr " + closure.text;
      std::string resultAddr = sret;
      bool hasSret = returnsInMemory(ret);
      bool resultTemp = false; // owned by the statement once the call returned it
      if (hasSret)
      {
        if (resultAddr.empty())
        {
          resultAddr = newObjectSlot(ret, at);
          resultTemp = true;
        }
        argText += ", ptr " + resultAddr;
      }
      std::string sig = (hasSret ? "void" : ret->isReference() ? "ptr" : llType(ret, at)) + std::string(" (ptr") + (hasSret ? ", ptr" : "");
      size_t position = 0;
      for (Expr* arg : callArgs)
      {
        if (isPackExpansion(arg))
        {
          Expr* pattern = static_cast<UnaryExpr*>(arg)->operand;
          forEachPackElement(pattern, at, [&](size_t)
          {
            const Ty* pt = position < callTy->args.size() ? callTy->args[position] : nullptr;
            if (!pt)
              unsupported(at, "too many arguments to a lambda");
            sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
            argText += ", " + argumentText(pt, pattern, at);
            ++position;
          });
          continue;
        }
        const Ty* pt = position < callTy->args.size() ? callTy->args[position] : nullptr;
        if (!pt)
          unsupported(at, "too many arguments to a lambda");
        sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
        argText += ", " + argumentText(pt, arg, at);
        ++position;
      }
      // Parameters the call left out take their default arguments: GuiTargetItemBase's loader calls
      // `[&](auto& value, OpenGuiType initIfType, bool load = true)` with two, and the operator read
      // whatever its third register held (no targeter was read, and the map loading lost its place).
      LambdaExpr* declared = genericBody ? genericBody : lambdaOf(t);
      for (; position < callTy->args.size(); ++position)
      {
        auto* pd = declared && position < declared->params.size() ? static_cast<ParamDecl*>(declared->params[position]) : nullptr;
        if (!pd || pd->isPack)
          break;
        if (!pd->defaultValue)
          unsupported(at, "a call to a lambda without an argument for its parameter " + (pd->name != UINT32_MAX ? sym(pd->name) : std::to_string(position + 1)));
        const Ty* pt = callTy->args[position];
        sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
        if (pd->defaultValue->kind == NodeKind::CallerExpr)
          argText += ", ptr " + callerLocation(pt, at); // `CallerLocation here = caller`: this call's location, as for a function
        else
          argText += ", " + argumentText(pt, pd->defaultValue, pd->defaultValue);
      }
      sig += ")";
      // The instantiation's operator is emitted after the arguments: a lambda argument's closure
      // (its capture list) is built here, in the function that writes it, before the operator's
      // body calls it.
      bool firstUse = false;
      if (genericLambda)
      {
        auto lock = sharedLock();
        firstUse = lambdaInstances.insert(functionName).second;
      }
      if (firstUse)
        emitLambda(genericBody, t, callTy, functionName, &instanceArgs);
      if (hasSret || ret->kind == TyKind::Void)
      {
        emitCall("", sig + " " + functionName + "(" + argText + ")");
        if (resultTemp)
          statementTemps.push_back({resultAddr, ret}); // not before: a call that throws built no result to destroy
        return hasSret ? Value{resultAddr, ret, sret.empty()} : Value{"", types.voidType(), false};
      }
      std::string r = newTemp();
      emitCall(r + " = ", sig + " " + functionName + "(" + argText + ")");
      return {r, ret, false};
    }

    // --- functions ---

    // IR name of a function (for an instantiation: with the class and template arguments),
    // computed once per function, class and arguments.
    struct NameKey
    {
      FunctionDecl* f;
      const Ty* classTy;
      std::vector<std::pair<Decl*, const Ty*>> args;
      bool operator==(const NameKey&) const = default;
    };
    struct NameKeyHash
    {
      size_t operator()(const NameKey& k) const
      {
        size_t h = std::hash<const void*>()(k.f) * 31 + std::hash<const void*>()(k.classTy);
        for (auto& [d, t] : k.args)
          h = (h * 1000003) ^ (std::hash<const void*>()(d) * 31 + std::hash<const void*>()(t));
        return h;
      }
    };
    std::unordered_map<NameKey, std::string, NameKeyHash> functionNames;

    const std::string& functionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      NameKey key{f, classTy, args.pairs};
      auto it = functionNames.find(key);
      if (it == functionNames.end())
        it = functionNames.emplace(std::move(key), computeFunctionName(f, args, classTy)).first;
      return it->second;
    }

    // q, the qualified name of a member d, with the arguments written for the full specialisation
    // owning d (`template<> struct Limits<int>`) before the member's own name, so that the members
    // of two specialisations of one template differ (`Flag<true>::value`, `Flag<false>::value`).
    // Unchanged for a member of any other class.
    std::string withSpecialisationArgs(std::string q, Decl* d)
    {
      Decl* owner = global.owningClass(d);
      if (!owner || owner->kind != NodeKind::ClassDecl || !(owner->flags & DF_Specialization))
        return q;
      const NamePart& last = static_cast<ClassDecl*>(owner)->name.last();
      std::string spelled;
      for (Node* a : last.templateArgs)
      {
        const Ty* t = nullptr;
        switch (a->kind)
        {
          case NodeKind::NamedType: case NodeKind::PointerType: case NodeKind::LRefType: case NodeKind::RRefType:
          case NodeKind::ConstType: case NodeKind::ArrayType: case NodeKind::FunctionType: case NodeKind::BuiltinType:
          case NodeKind::DecltypeType: case NodeKind::AutoType: case NodeKind::MemberPointerType:
            t = global.typeOfAst(static_cast<Type*>(a), owner);
            break;
          default: break;
        }
        std::string one;
        if (t && t->isKnown())
          one = irString(t);
        else if (owner->file && owner->file->tokens && a->token < owner->file->tokens->size())
        {
          const Token& tok = (*owner->file->tokens)[a->token];
          one = std::string(owner->file->text.substr(tok.offset, tok.length));
        }
        spelled += (spelled.empty() ? "" : ", ") + one;
      }
      size_t pos = q.rfind("::");
      if (pos != std::string::npos)
        q.insert(pos, "<" + spelled + ">");
      return q;
    }

    std::string computeFunctionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      if (global.isExternC(f))
        return "@" + sym(f->name.last().symbol);
      if (f->name.parts.size() == 1 && (sym(f->name.parts[0].symbol) == "main" || sym(f->name.parts[0].symbol) == "wmain") && !global.owningClass(f))
        return "@kpp." + sym(f->name.parts[0].symbol); // called by the generated @main (@wmain), which reports uncaught exceptions
      ArgsScope scope(*this, &args);
      std::string q;
      if (classTy && !classTy->args.empty())
      {
        // Class path with the instantiation's arguments, then the member's own name.
        std::string full = qualifiedName(f);
        std::string cls = qualifiedName(classTy->decl);
        q = irString(classTy);
        size_t pos = full.rfind("::");
        q += pos == std::string::npos ? "::" + full : full.substr(pos);
      }
      else
      {
        q = qualifiedName(f);
        if (f->name.parts.size() > 1 && !global.owningClass(f))
        {
          // `int M::twice(int) {}` outside its namespace: the name the declaration in M has.
          std::string prefix;
          for (size_t i = 0; i + 1 < f->name.parts.size(); ++i)
            if (f->name.parts[i].symbol != UINT32_MAX)
              prefix += sym(f->name.parts[i].symbol) + "::";
          size_t pos = q.rfind("::");
          std::string head = pos == std::string::npos ? "" : q.substr(0, pos + 2), tail = pos == std::string::npos ? q : q.substr(pos + 2);
          if (!prefix.empty() && head.find(prefix) == std::string::npos)
            q = head + prefix + tail;
        }
        q = withSpecialisationArgs(q, f);
        if (Decl* owner = global.owningClass(f); owner && owner->kind == NodeKind::ClassDecl && declaredInFunction(owner))
        {
          // A local class's member: the class as types print it (its function and place), with
          // the arguments of the function template instance it is in, then the member's own name;
          // two local classes named Visitor are two classes, and so is one in two instances.
          std::string instance;
          std::vector<std::string> printed;
          for (auto& [d, ty] : args.pairs)
            if (d && d->kind == NodeKind::TemplateParamDecl)
              printed.push_back(sym(static_cast<TemplateParamDecl*>(d)->name) + "=" + (ty ? irString(ty) : std::string("?")));
          std::sort(printed.begin(), printed.end());
          for (const std::string& p : printed)
            instance += (instance.empty() ? "<" : ", ") + p;
          if (!instance.empty())
            instance += ">";
          size_t pos = q.rfind("::");
          q = irString(classTy ? classTy : types.classType(owner, {})) + instance + (pos == std::string::npos ? "::" + q : q.substr(pos));
        }
      }
      if (!f->templateParams.empty())
      {
        q += "<";
        for (size_t i = 0; i < f->templateParams.size(); ++i)
        {
          const Ty* t = nullptr;
          for (auto& [d, ty] : args.pairs)
            if (d == f->templateParams[i])
              t = ty;
          q += (i ? ", " : "") + (t ? irString(t) : std::string("?"));
        }
        q += ">";
      }
      else if ((f->flags & DF_Specialization) && f->isTemplate)
      {
        // `template<> Vector<String> splitWords(StringView)`: the explicit arguments, or the return
        // type when the parameter appears only there, tell the specialisations of one template apart.
        std::string spelled;
        for (Node* a : f->name.last().templateArgs)
        {
          const Ty* t = uint8_t(a->kind) <= uint8_t(NodeKind::PackType) ? global.typeOfAst(static_cast<Type*>(a), f) : nullptr;
          spelled += (spelled.empty() ? "" : ", ") + (t && t->isKnown() ? irString(t) : std::string("?"));
        }
        if (spelled.empty())
          if (const Ty* ft = declType(f); ft && ft->kind == TyKind::Function && ft->inner)
            spelled = irString(ft->inner);
        q += "<" + spelled + ">";
      }
      const Ty* ft = declType(f);
      std::string params;
      for (size_t i = 0; ft && i < ft->args.size(); ++i)
        params += (i ? ", " : "") + irString(ft->args[i]);
      // A const method is a different function from its non-const overload.
      return "@\"" + q + "(" + params + ")" + ((f->flags & DF_Const) && !(f->flags & DF_Static) ? " const" : "") + "\"";
    }

    struct Signature
    {
      std::string returnType;
      std::vector<std::string> paramTypes;
      std::vector<const Ty*> paramTys; // null for this/sret
      bool hasThis = false, hasSret = false, variadic = false;
    };

    // A function (in every module) that moves `this` by the offset `at` (a pointer constant) and calls
    // callee, or the function in slot `slot` of the vtable `this` then has, with its other arguments.
    std::string sharedThunk(const std::string& name, const Signature& s, const std::string& at, const std::string& callee, int slot)
    {
      auto lock = sharedLock();
      if (emittedShared[name])
        return name;
      emittedShared[name] = true;
      std::string params, args;
      for (size_t i = 1; i < s.paramTypes.size(); ++i)
      {
        params += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
        args += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
      }
      std::string body = "  %off = ptrtoint ptr " + at + " to i64\n  %self = getelementptr i8, ptr %this, i64 %off\n";
      std::string target = callee;
      if (slot >= 0)
      {
        body += "  %vt = load ptr, ptr %self\n  %sp = getelementptr inbounds ptr, ptr %vt, i64 " + std::to_string(slot) + "\n  %fp = load ptr, ptr %sp\n";
        target = "%fp";
      }
      if (s.returnType == "void")
        body += "  tail call void " + target + "(ptr %self" + args + ")\n  ret void\n";
      else
        body += "  %r = tail call " + s.returnType + " " + target + "(ptr %self" + args + ")\n  ret " + s.returnType + " %r\n";
      shared += sharedComdat(name) + "define linkonce_odr " + s.returnType + " " + name + "(ptr %this" + params + ") comdat {\nentry:\n" + body + "}\n";
      return name;
    }

    // The calling convention of a member function of type ft, as signatureOf gives it for a
    // declaration: `this`, the result's slot for a class result, then the parameters.
    Signature methodSignature(const Ty* ft, Node* at)
    {
      Signature s;
      const Ty* ret = ft->inner;
      if (returnsInMemory(ret))
      {
        s.hasSret = true;
        s.returnType = "void";
      }
      else
        s.returnType = !ret || !ret->isKnown() || ret->isReference() ? "ptr" : llType(ret, at);
      s.hasThis = true;
      s.paramTypes.push_back("ptr");
      s.paramTys.push_back(nullptr);
      if (s.hasSret)
      {
        s.paramTypes.push_back("ptr");
        s.paramTys.push_back(nullptr);
      }
      for (const Ty* pt : ft->args)
      {
        s.paramTypes.push_back(isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, at));
        s.paramTys.push_back(pt);
      }
      s.variadic = ft->variadic;
      return s;
    }

    // Signature under the arguments currently in effect.
    Signature signatureOf(FunctionDecl* f)
    {
      const Ty* ft = declType(f);
      if (!ft || ft->kind != TyKind::Function)
        unsupported(f, "a function whose type is unknown");
      Signature s;
      bool method = global.owningClass(f) && !(f->flags & DF_Static) && !global.isExternC(f);
      const Ty* ret = ft->inner;
      if (returnsInMemory(ret))
      {
        s.hasSret = true;
        s.returnType = "void";
      }
      else if (!ret || !ret->isKnown())
        s.returnType = "ptr"; // unknown return type: treat as opaque pointer
      else
        s.returnType = ret->isReference() ? "ptr" : llType(ret, f);
      if (method)
      {
        s.hasThis = true;
        s.paramTypes.push_back("ptr");
        s.paramTys.push_back(nullptr);
      }
      if (s.hasSret)
      {
        s.paramTypes.push_back("ptr");
        s.paramTys.push_back(nullptr);
      }
      for (size_t i = 0; i < ft->args.size(); ++i)
      {
        const Ty* pt = ft->args[i];
        if (pt->kind == TyKind::Pack)
        {
          // A deduced pack still in one piece (`Args&&...` of a member template): one parameter per element.
          for (const Ty* e : pt->args)
          {
            s.paramTypes.push_back(isAggregate(e) || e->isReference() || !e->isKnown() ? "ptr" : llType(e, f));
            s.paramTys.push_back(e);
          }
          continue;
        }
        s.paramTypes.push_back(isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, f));
        s.paramTys.push_back(pt);
      }
      s.variadic = ft->variadic;
      return s;
    }

    std::string signatureText(const Signature& s)
    {
      std::string t = s.returnType + " (";
      for (size_t i = 0; i < s.paramTypes.size(); ++i)
        t += (i ? ", " : "") + s.paramTypes[i];
      if (s.variadic)
        t += s.paramTypes.empty() ? "..." : ", ...";
      t += ")";
      return t;
    }

    // An instantiation requested through a longer chain of instantiations is an error: a template
    // that instantiates itself with ever larger arguments would otherwise never stop, its names
    // growing with every step. The whole Factorio source needs 12. See docs/design.md.
    static constexpr unsigned maxInstantiationDepth = 64;

    // The instantiations this emitter asked for while the current level was emitted, each with
    // its first requester by name: the driver takes the first over all emitters as the
    // instantiation's requester (which the diagnostics name, and whose depth it inherits), the same
    // in every run, where the first to ask depends on timing. Every request counts, cached or not.
    std::unordered_map<std::string, Request> levelRequests;
    void noteRequest(const std::string& name)
    {
      auto [it, first] = levelRequests.try_emplace(name, Request{currentFunctionName, currentDepth + 1});
      if (!first && currentFunctionName < it->second.by)
        it->second = {currentFunctionName, currentDepth + 1};
    }
    bool lastUseInstantiates = false; // the last useFunctionOnce asked for an instantiation

    // Asks for the instantiation `name`: true for the first request, which queues it.
    bool claimInstantiation(const std::string& name)
    {
      noteRequest(name);
      lastUseInstantiates = true;
      return requested.claim(name);
    }

    // An instantiation whose chain of requests is longer than the limit is an error, reported
    // instead of emitting it (with its requester as the level settled it, the same in every run).
    void reportTooDeep(const Instantiation& item)
    {
      auto shortened = [](const std::string& s) { return s.size() <= 300 ? s : s.substr(0, 300) + "..."; };
      currentFile = item.f->file;
      error(item.f, "template instantiations nested more than " + std::to_string(maxInstantiationDepth) + " deep: " + shortened(item.name) +
                        " requested by " + shortened(item.requestedBy));
    }

    void queueInstantiation(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& name)
    {
      // In the module its name hashes to: spread evenly, and the same module whoever asks first (the
      // first requester's module piled most of the library's instantiations into one module that
      // clang then took 7 s at -O0 and 224 s at -O2 on, alone).
      unsigned module = moduleCount <= 1 ? 0 : unsigned(std::hash<std::string>()(name) % moduleCount);
      worklist.push_back({f, args, classTy, module, currentFunctionName, name, currentDepth + 1});
    }

    // Makes a function usable from the current function: declares external ones, queues
    // instantiations. Returns its IR name.
    struct UsedFunction
    {
      std::string name;
      bool instantiation; // asked for as an instantiation: each use is a request (noteRequest)
    };
    std::unordered_map<std::string, UsedFunction> usedFunctions; // what useFunction answered this emitter (its shared part done)
    std::string useFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      if (f->builtin == BUILTIN_EnumFromStr)
        return useFunctionOnce(f, args, classTy);
      std::string key = functionName(f, args, classTy);
      if (auto mine = usedFunctions.find(key); mine != usedFunctions.end())
      {
        if (mine->second.instantiation)
          noteRequest(mine->second.name);
        return mine->second.name;
      }
      lastUseInstantiates = false;
      std::string name = useFunctionOnce(f, args, classTy);
      usedFunctions.emplace(std::move(key), UsedFunction{name, lastUseInstantiates});
      return name;
    }


    // The arguments of f's instantiation as its definition def names them: a definition separate
    // from the declaration has its own parameter declarations (`template<typename Policy>
    // ScrollBar<Policy>::ScrollBar(...)` names the class's parameters first, then the member's own).
    TemplateArgs definitionArguments(FunctionDecl* f, FunctionDecl* def, const TemplateArgs& args)
    {
      TemplateArgs defArgs = args;
      if (def == f)
        return defArgs;
      Decl* owner = global.owningClass(f);
      Span<Decl*> classParams; // a nested class's are the enclosing template's (`Outer<K, V>::Inner::load`)
      for (Decl* o = owner; o && (o->kind == NodeKind::ClassDecl || o->kind == NodeKind::EnumDecl) && classParams.empty(); o = o->parent)
        classParams = o->templateParams;
      size_t classCount = def->templateParams.size() > f->templateParams.size() ? def->templateParams.size() - f->templateParams.size() : 0;
      if (classCount == classParams.size() && classCount + f->templateParams.size() <= def->templateParams.size())
      {
        std::vector<std::pair<Decl*, const Ty*>> added;
        for (auto& [d, t] : defArgs.pairs)
        {
          for (size_t i = 0; i < classCount; ++i)
            if (d == classParams[i])
              added.push_back({def->templateParams[i], t});
          for (size_t i = 0; i < f->templateParams.size(); ++i)
            if (d == f->templateParams[i])
              d = def->templateParams[classCount + i];
        }
        for (auto& p : added)
          defArgs.pairs.push_back(p);
      }
      return defArgs;
    }

    std::string useFunctionOnce(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      if (f->builtin == BUILTIN_EnumFromStr)
      {
        // The generated `E::fromStr(text)` of a Kpp enum: the library's enumFromStr<E>(text),
        // which has the same signature (an Optional<E> result slot, the text).
        Decl* lib = global.enumFromStrDecl();
        if (!lib || lib->templateParams.size() != 1 || !f->parent || f->parent->kind != NodeKind::EnumDecl)
          unsupported(f, "E::fromStr without the library's enumFromStr<E>");
        TemplateArgs libArgs;
        libArgs.pairs.push_back({lib->templateParams[0], types.enumType(f->parent)});
        return useFunction(static_cast<FunctionDecl*>(lib), libArgs, nullptr);
      }
      std::string name = functionName(f, args, classTy);
      Decl* defDecl = global.definitionOf(f, args);
      FunctionDecl* def = defDecl && defDecl->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defDecl) : f;
      bool templated = !args.empty();
      if (def->file && def->file->isImported)
        def = f, templated = false; // inline C functions are declared, the library provides them
      if (def != f && (def->flags & DF_Default) && !def->body && !(f->flags & DF_Default))
        f->flags |= DF_Default; // `Flow::~Flow() = default;` out of line: the declaration is defaulted too (settled before emitting)
      if ((f->flags & DF_Default) && !f->body)
      {
        // `= default`: the compiler's memberwise function, defined where first needed (a vtable
        // slot or a function pointer wants a real function; calls inline the body).
        if (claimInstantiation(name))
          queueInstantiation(f, args, classTy, name);
        return name;
      }
      if (def->body && templated)
      {
        if (claimInstantiation(name))
        {
          queueInstantiation(def, definitionArguments(f, def, args), classTy, name);
        }
        return name;
      }
      if (def->body && !(def->file && def->file->isImported))
      {
        // Defined by the main pass, except a local class's member, which is emitted when used.
        if (Decl* owner = global.owningClass(def); owner && declaredInFunction(owner) && claimInstantiation(name))
          queueInstantiation(def, args, classTy, name);
        return name;
      }
      if (!declaredNames.claim(name))
        return name;
      {
        static bool debugCalls = getenv("KPP_DEBUG_CALLS") != nullptr;
        if (debugCalls)
          fprintf(stderr, "declare %s: def %s, def flags %x, def body %d, f flags %x, f body %d\n", name.c_str(), def == f ? "self" : "other", def->flags,
                  def->body != nullptr, f->flags, f->body != nullptr);
      }
      if (f->builtin == BUILTIN_EnumStr)
      {
        emitEnumStr(f, name);
        return name;
      }
      // A function of the program's own (not C's) with no definition found: only declared, it would
      // be missing from the link with no error. Reported once generation is done (generateLLVM).
      if (!global.isExternC(f) && !(f->file && f->file->isImported) && !(f->flags & (DF_Delete | DF_Pure)))
      {
        auto lock = sharedLock();
        sh.undefinedUses.emplace(name, f);
      }
      ArgsScope scope(*this, &args);
      Signature s = signatureOf(f);
      std::string params;
      for (size_t i = 0; i < s.paramTypes.size(); ++i)
        params += (i ? ", " : "") + s.paramTypes[i];
      if (s.variadic)
        params += params.empty() ? "..." : ", ...";
      decls += "declare " + s.returnType + " " + name + "(" + params + ")\n";
      return name;
    }

    static size_t enumeratorCount(Decl* e)
    {
      size_t n = 0;
      for (Decl* d : static_cast<EnumDecl*>(e)->enumerators)
        n += d->kind == NodeKind::EnumeratorDecl;
      return n;
    }

    // The generated `const char* str() const` of a Kpp enum: a switch over the enumerators.
    void emitEnumStr(FunctionDecl* f, const std::string& name)
    {
      emittingShared = true;
      auto* e = static_cast<EnumDecl*>(f->parent);
      std::string k = llType(enumUnderlying(e), f), cases, blocks;
      int i = 0;
      for (Decl* d : e->enumerators)
        if (d->kind == NodeKind::EnumeratorDecl)
        {
          auto* en = static_cast<EnumeratorDecl*>(d);
          std::string l = "e" + std::to_string(i++);
          cases += "    " + k + " " + std::to_string(enumeratorValue(en)) + ", label %" + l + "\n";
          blocks += l + ":\n  ret ptr " + cStringConstant(sym(en->name)) + "\n";
        }
      shared += "$" + name.substr(1) + " = comdat any\n"
                "define linkonce_odr ptr " + name + "(ptr %this) comdat {\nentry:\n  %v = load " + k + ", ptr %this\n  switch " + k +
                " %v, label %other [\n" + cases + "  ]\n" + blocks + "other:\n  ret ptr " + cStringConstant("?") + "\n}\n";
      emittingShared = false;
    }

    // Template arguments for calling f on an object of classTy, with the call's deduced arguments.
    TemplateArgs calleeArgs(FunctionDecl* f, const Ty* classTy, CallExpr* call, Node* at)
    {
      TemplateArgs args;
      if (classTy)
        args = global.classArgsOf(canonicalClass(classTy, at));
      if (!f->templateParams.empty())
      {
        if (!call || call->templateArgCount != f->templateParams.size())
          unsupported(at, "a call to " + qualifiedName(f) + " whose template arguments could not be deduced");
        bool hasUnknown = false;
        for (size_t i = 0; i < f->templateParams.size(); ++i)
        {
          const Ty* ta = call->templateArgs[i];
          args.pairs.push_back({f->templateParams[i], ta});
          if (!ta || !ta->isKnown())
            hasUnknown = true;
          else if (ta->kind == TyKind::Pack)
            for (const Ty* e : ta->args)
              if (!e || !e->isKnown())
              { hasUnknown = true; break; }
        }
        if (hasUnknown)
        {
          // The type checker left some template args unresolved (e.g. a pack forwarded into
          // another template). Remove them so deduceArgs can re-add with concrete types.
          auto isUnresolved = [](const Ty* t) -> bool
          {
            if (!t || !t->isKnown()) return true;
            if (t->kind == TyKind::Pack)
              for (const Ty* e : t->args)
                if (!e || !e->isKnown()) return true;
            return false;
          };
          args.pairs.erase(
            std::remove_if(args.pairs.begin(), args.pairs.end(),
              [&](const auto& p) { return isUnresolved(p.second); }),
            args.pairs.end());
          std::vector<const Ty*> argTypes;
          for (Expr* a : call->args)
          {
            if (isPackExpansion(a))
            {
              Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
              forEachPackElement(pattern, at, [&](size_t)
              {
                argTypes.push_back(pattern->ty ? argumentType(pattern, pattern->ty, types) : nullptr);
              });
            }
            else
              argTypes.push_back(a ? argumentType(a, a->ty, types) : nullptr);
          }
          global.deduceArgs(f, argTypes, args);
        }
      }
      return args;
    }

    // Emits a call. thisArg/sret may be empty. For class returns the result is the sret address.
    // `kpp_atomic_add32(p, v)` and friends (stdlib/core/Thread.kpp): atomic instructions, not calls.
    std::optional<Value> atomicBuiltin(FunctionDecl* f, Span<Expr*> callArgs, Node* at)
    {
      std::string name = sym(f->name.last().symbol);
      if (name.rfind("kpp_atomic_", 0) != 0)
        return std::nullopt;
      // The operation, then the width in bits: kpp_atomic_load8, kpp_atomic_cas64.
      std::string op = name.substr(11);
      size_t digits = op.find_first_of("0123456789");
      std::string width = digits == std::string::npos ? "" : op.substr(digits);
      op = op.substr(0, digits);
      if (width != "8" && width != "16" && width != "32" && width != "64")
        unsupported(at, "the atomic builtin " + name + " (widths are 8, 16, 32 and 64)");
      int bits = std::stoi(width);
      std::string ty = "i" + width;
      std::string align = std::to_string(bits / 8);
      if (callArgs.empty())
        unsupported(at, "an atomic builtin without its pointer");
      std::string p = convert(rvalue(callArgs[0]), types.pointer(types.voidType()), at).text;
      const Ty* valueTy = types.intType(bits, false);
      auto arg = [&](size_t i) { return convert(rvalue(callArgs[i]), valueTy, at).text; };
      std::string r = newTemp();
      if (op == "load")
      {
        fn += "  " + r + " = load atomic " + ty + ", ptr " + p + " seq_cst, align " + align + "\n";
        return Value{r, valueTy};
      }
      if (op == "store")
      {
        fn += "  store atomic " + ty + " " + arg(1) + ", ptr " + p + " seq_cst, align " + align + "\n";
        return Value{"", types.voidType()};
      }
      if (op == "add" || op == "exchange")
      {
        fn += "  " + r + " = atomicrmw " + (op == "add" ? "add" : "xchg") + " ptr " + p + ", " + ty + " " + arg(1) + " seq_cst, align " + align + "\n";
        return Value{r, valueTy};
      }
      if (op == "cas")
      {
        std::string exchange = arg(1), comparand = arg(2), pair = newTemp();
        fn += "  " + pair + " = cmpxchg ptr " + p + ", " + ty + " " + comparand + ", " + ty + " " + exchange + " seq_cst seq_cst, align " + align + "\n";
        fn += "  " + r + " = extractvalue { " + ty + ", i1 } " + pair + ", 0\n";
        return Value{r, valueTy};
      }
      unsupported(at, "the atomic builtin " + name);
    }

    // The library's `move` and `forward` called with their one argument: identity casts, a reference
    // in and the same reference out, so the call is the argument's address as the parameter would
    // receive it (the same conversions and temporaries) without the call (docs/compile-speed.md,
    // "Design: inlining trivial functions in kppc", step 1). Null for any other call.
    std::optional<Value> inlinedIdentity(FunctionDecl* f, const TemplateArgs& args, Span<Expr*> callArgs, Node* at)
    {
      if (!f->file || !f->file->isLibrary || f->name.empty() || f->name.parts.size() != 1 || f->templateParams.empty() || f->params.size() != 1 ||
          callArgs.size() != 1 || !callArgs[0] || isPackExpansion(callArgs[0]) || global.owningClass(f))
        return std::nullopt;
      std::string_view name = symbols.name(f->name.last().symbol);
      if (name != "move" && name != "forward")
        return std::nullopt;
      Signature s;
      const Ty* ret;
      {
        ArgsScope scope(*this, &args);
        s = signatureOf(f);
        ret = declType(f)->inner;
      }
      if (s.hasThis || s.hasSret || s.paramTys.size() != 1 || !s.paramTys[0] || !s.paramTys[0]->isReference() || !ret || !ret->isReference())
        return std::nullopt;
      std::string text = argumentText(s.paramTys[0], callArgs[0], at);
      if (text.rfind("ptr ", 0) != 0)
        unsupported(at, "an inlined " + std::string(name) + " given no address (" + text + ")"); // (a reference argument is always one)
      return Value{text.substr(4), ret, false};
    }

    std::unordered_map<FunctionDecl*, FunctionDecl*> trivialDefinitions; // a function's typed definition -> it, when trivial (or null)
    std::unordered_set<FunctionDecl*> inlining;                            // the definitions being emitted in a caller now
    std::unordered_map<std::string, std::string> inlineSlots;              // (inlining depth, parameter, type) -> its slot in this function

    // Whether a value of type t is a scalar, a pointer or a reference: what a trivial function
    // takes and gives (a class by value needs a copy and a destruction; a class result a slot).
    bool inlineScalar(const Ty* t)
    {
      if (!t || !t->isKnown() || types.isDependent(t))
        return false;
      if (t->isReference())
        return types.stripRef(t)->isKnown();
      return t->isArithmetic() || t->kind == TyKind::Enum || t->kind == TyKind::Pointer || t->kind == TyKind::Nullptr;
    }

    // Whether e may stand in a trivial function's body: its parameters, `this` and fields reached
    // from them, constants and enumerators, combined by built-in operators on scalars and pointers,
    // built-in subscripts and casts. No call, no overloaded operator, nothing that makes a temporary
    // to destroy or can throw; a parameter is only read.
    bool trivialExpression(Expr* e, FunctionDecl* def, int depth = 0)
    {
      if (!e || depth > 16 || !e->ty || !e->ty->isKnown() || types.isDependent(e->ty))
        return false;
      auto scalar = [&](Expr* x) { return x && x->ty && inlineScalar(types.unqualified(types.stripRef(x->ty))); };
      auto parameter = [&](Expr* x) {
        while (x && x->kind == NodeKind::ParenExpr)
          x = static_cast<ParenExpr*>(x)->inner;
        return x && x->kind == NodeKind::NameExpr && static_cast<NameExpr*>(x)->resolved &&
               static_cast<NameExpr*>(x)->resolved->kind == NodeKind::ParamDecl;
      };
      switch (e->kind)
      {
        case NodeKind::ParenExpr:
          return trivialExpression(static_cast<ParenExpr*>(e)->inner, def, depth + 1);
        case NodeKind::LiteralExpr:
        case NodeKind::ThisExpr:
          return true;
        case NodeKind::NameExpr:
        {
          Decl* d = static_cast<NameExpr*>(e)->resolved;
          if (!d)
            return false;
          if (d->kind == NodeKind::ParamDecl)
            return std::find(def->params.begin(), def->params.end(), d) != def->params.end() && !static_cast<ParamDecl*>(d)->isPack;
          if (d->kind == NodeKind::EnumeratorDecl)
            return true;
          return d->kind == NodeKind::VariableDecl && !(d->flags & (DF_Static | DF_Typedef)) && global.owningClass(d) &&
                 static_cast<VariableDecl*>(d)->structuredBindings.empty(); // `items`: this->items
        }
        case NodeKind::MemberExpr:
        {
          auto* m = static_cast<MemberExpr*>(e);
          if (m->dynamicMember || !m->resolved || m->resolved->kind != NodeKind::VariableDecl || (m->resolved->flags & (DF_Static | DF_Typedef)) ||
              m->member.parts.size() != 1)
            return false;
          if ((m->op == P::Arrow || m->op == P::SafeArrow) &&
              (m->op == P::SafeArrow || !m->base->ty || types.unqualified(types.stripRef(m->base->ty))->kind != TyKind::Pointer))
            return false; // `->` of a class (its operator->), or `?->`
          return trivialExpression(m->base, def, depth + 1);
        }
        case NodeKind::UnaryExpr:
        {
          auto* u = static_cast<UnaryExpr*>(e);
          if (u->isKeyword || !scalar(u->operand))
            return false;
          P op = P(u->op);
          if (op == P::PlusPlus || op == P::MinusMinus)
            return !parameter(u->operand) && trivialExpression(u->operand, def, depth + 1); // `++this->count`
          if (op == P::Amp)
            return !parameter(u->operand) && trivialExpression(u->operand, def, depth + 1); // `&this->items[i]`
          if (op != P::Bang && op != P::Minus && op != P::Plus && op != P::Tilde && op != P::Star)
            return false;
          return trivialExpression(u->operand, def, depth + 1);
        }
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          if (b->isKeyword || !scalar(b->lhs) || !scalar(b->rhs))
            return false;
          P op = P(b->op);
          if (op == P::DotStar || op == P::ArrowStar || op == P::Comma)
            return false;
          if (isAssignment(op) && parameter(b->lhs))
            return false; // a parameter is only read
          return trivialExpression(b->lhs, def, depth + 1) && trivialExpression(b->rhs, def, depth + 1);
        }
        case NodeKind::IndexExpr:
        {
          auto* x = static_cast<IndexExpr*>(e);
          if (x->args.size() != 1 || !x->base->ty || !scalar(x->args[0]))
            return false;
          const Ty* bt = types.unqualified(types.stripRef(x->base->ty));
          if (bt->kind != TyKind::Pointer && bt->kind != TyKind::Array)
            return false; // a class's operator[]
          return trivialExpression(x->base, def, depth + 1) && trivialExpression(x->args[0], def, depth + 1);
        }
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          return c->then && scalar(c->cond) && scalar(c->then) && scalar(c->else_) && trivialExpression(c->cond, def, depth + 1) &&
                 trivialExpression(c->then, def, depth + 1) && trivialExpression(c->else_, def, depth + 1);
        }
        case NodeKind::CastExpr:
        case NodeKind::NamedCastExpr:
        {
          Expr* operand = e->kind == NodeKind::CastExpr ? static_cast<CastExpr*>(e)->operand : static_cast<NamedCastExpr*>(e)->operand;
          if (e->kind == NodeKind::NamedCastExpr && static_cast<NamedCastExpr*>(e)->castKind == Kw::dynamic_cast_)
            return false;
          return inlineScalar(types.unqualified(e->ty)) && scalar(operand) && trivialExpression(operand, def, depth + 1);
        }
        default:
          return false;
      }
    }

    // The typed definition of the call's callee when it is trivial: one statement, `return E;` or
    // `E;`, with E a trivialExpression, the parameters and the result scalars, pointers or
    // references. Decided once per definition (it depends on the definition alone). Null otherwise.
    FunctionDecl* trivialDefinition(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      if ((f->flags & (DF_Constructor | DF_Destructor | DF_Variadic | DF_Default | DF_Delete)) || (f->attributes & ATTR_NoInline) ||
          f->builtin || global.isExternC(f))
        return nullptr;
      // The definition the call would emit (useFunctionOnce): an explicit specialisation for these
      // arguments (`template<> const char* name<bool>()`), an out-of-line definition, the instance
      // of either under its own parameters.
      Decl* defined = global.definitionOf(f, args);
      auto* chosen = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defined) : f;
      if (!chosen->body || (chosen->file && chosen->file->isImported))
        return nullptr;
      FunctionDecl* def = args.empty() ? chosen : global.instanceBody(chosen, definitionArguments(f, chosen, args), classTy);
      if (!def || !def->body || def->body->kind != NodeKind::BlockStmt || inlining.count(def) || retypedPerElement(def))
        return nullptr;
      if (auto it = trivialDefinitions.find(def); it != trivialDefinitions.end())
        return it->second;
      FunctionDecl* result = nullptr;
      auto* block = static_cast<BlockStmt*>(def->body);
      Expr* e = nullptr;
      if (block->stmts.size() == 1 && block->stmts[0]->kind == NodeKind::ReturnStmt)
        e = static_cast<ReturnStmt*>(block->stmts[0])->value;
      else if (block->stmts.size() == 1 && block->stmts[0]->kind == NodeKind::ExprStmt)
        e = static_cast<ExprStmt*>(block->stmts[0])->expr;
      if (e && def->params.size() == f->params.size() && trivialExpression(e, def))
      {
        ArgsScope scope(*this, &args);
        Signature s = signatureOf(f);
        const Ty* ret = declType(f)->inner;
        bool ok = !s.hasSret && !s.variadic && ret && (ret->kind == TyKind::Void || inlineScalar(ret));
        if (ok && block->stmts[0]->kind == NodeKind::ReturnStmt && ret->kind != TyKind::Void && ret->isReference())
        {
          // A reference result: the lvalue E is, of the referred type itself (no base adjustment).
          const Ty* want = types.unqualified(types.stripRef(ret));
          const Ty* got = types.unqualified(types.stripRef(e->ty));
          ok = want == got || (isClass(want) && isClass(got) && classKey(want) == classKey(got));
        }
        for (size_t i = 0, slot = (s.hasThis ? 1 : 0); ok && i < f->params.size(); ++i, ++slot)
          ok = slot < s.paramTys.size() && inlineScalar(s.paramTys[slot]) && !static_cast<ParamDecl*>(def->params[i])->isPack;
        if (ok)
          result = def;
      }
      trivialDefinitions.emplace(def, result);
      return result;
    }

    // A call of a trivial function (trivialDefinition) emitted as its body: the arguments as the call
    // passes them, in order, bound to the definition's parameters as its own prologue binds them (in
    // slots this function's inlined calls share), `this` likewise, and E emitted under the callee's
    // arguments, class and file (an error in it is the callee's line), its result as the callee's
    // return statement makes it (docs/compile-speed.md, step 2). Null for any other call.
    std::optional<Value> inlinedBody(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& thisArg,
                                     const std::string& sret, Span<Expr*> callArgs, Node* at)
    {
      if (!sret.empty() || callArgs.size() != f->params.size())
        return std::nullopt;
      for (Expr* a : callArgs)
        if (!a || isPackExpansion(a) || a->kind == NodeKind::BraceInitExpr)
          return std::nullopt;
      FunctionDecl* def = trivialDefinition(f, args, classTy);
      if (!def)
        return std::nullopt;
      Signature s;
      const Ty* ret;
      {
        ArgsScope scope(*this, &args);
        s = signatureOf(f);
        ret = declType(f)->inner;
      }
      if (s.hasThis && thisArg.empty())
        return std::nullopt;
      size_t first = s.hasThis ? 1 : 0;
      std::vector<std::string> texts;
      for (size_t i = 0; i < callArgs.size(); ++i)
        texts.push_back(argumentText(s.paramTys[first + i], callArgs[i], at));
      auto slotFor = [&](const std::string& key, const std::string& type) {
        std::string& slot = inlineSlots[key + " " + type];
        if (slot.empty())
        {
          slot = "%inl" + std::to_string(temp++);
          allocas += "  " + slot + " = alloca " + type + "\n";
        }
        return slot;
      };
      std::vector<std::pair<Decl*, std::optional<Local>>> saved;
      auto bind = [&](Decl* d, Local local) {
        auto it = locals.find(d);
        saved.push_back({d, it == locals.end() ? std::nullopt : std::optional<Local>(it->second)});
        locals[d] = local;
      };
      size_t depth = inlining.size();
      if (s.hasThis)
      {
        std::string slot = slotFor(std::to_string(depth) + ".this", "ptr");
        fn += "  store ptr " + thisArg + ", ptr " + slot + "\n";
        bind(nullptr, Local{slot, nullptr, false});
      }
      for (size_t i = 0; i < callArgs.size(); ++i)
      {
        const Ty* pt = s.paramTys[first + i];
        bool isRef = pt->isReference();
        std::string type = isRef ? std::string("ptr") : llType(pt, at);
        size_t space = texts[i].find(' ');
        std::string slot = slotFor(std::to_string(depth) + "." + std::to_string(i), type);
        fn += "  store " + type + " " + texts[i].substr(space + 1) + ", ptr " + slot + "\n";
        bind(def->params[i], Local{slot, pt, isRef});
      }
      inlining.insert(def);
      ArgsScope argsScope(*this, &args);
      struct Restore
      {
        Emitter& e;
        const Ty* classTy;
        Decl* initializerOf;
        Decl* lambdaContext;
        ParsedFile* file;
        ~Restore() { e.currentClassTy = classTy; e.initializerOf = initializerOf; e.lambdaContext = lambdaContext; e.currentFile = file; }
      } restore{*this, currentClassTy, initializerOf, lambdaContext, currentFile};
      currentClassTy = classTy;
      initializerOf = def;
      lambdaContext = nullptr;
      currentFile = def->file;
      auto* statement = static_cast<BlockStmt*>(def->body)->stmts[0];
      Value result{"", types.voidType(), false};
      if (statement->kind == NodeKind::ExprStmt)
        rvalue(static_cast<ExprStmt*>(statement)->expr);
      else
      {
        Expr* e = static_cast<ReturnStmt*>(statement)->value;
        if (ret->kind == TyKind::Void)
          rvalue(e);
        else if (ret->isReference())
          result = {isAggregate(types.stripRef(ret)) ? rvalue(e).text : lvalue(e), ret, false};
        else
          result = {convert(rvalue(e), ret, e).text, ret, false};
      }
      inlining.erase(def);
      for (auto it = saved.rbegin(); it != saved.rend(); ++it)
        if (it->second)
          locals[it->first] = *it->second;
        else
          locals.erase(it->first);
      return result;
    }

    Value callFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& thisArg,
                       const std::string& sret, Span<Expr*> callArgs, Node* at, const std::string& indirect = "")
    {
      if ((f->flags & DF_Delete) && global.definitionOf(f, args) == f) // a deleted template's explicit specialisation is no deleted function
        unsupported(at, "a call to the deleted function " + qualifiedName(f));
      if (global.isExternC(f))
        if (std::optional<Value> v = atomicBuiltin(f, callArgs, at))
          return *v;
      if (f->flags & DF_Default)
        return defaultedCall(f, classTy, thisArg, callArgs, at);
      if (!conditionHolds(f))
        return {"", types.voidType(), false}; // conditional(false): no call, arguments not evaluated
      if (indirect.empty() && sh.inlineTrivial)
      {
        if (std::optional<Value> v = inlinedIdentity(f, args, callArgs, at))
          return *v;
        if (std::optional<Value> v = inlinedBody(f, args, classTy, thisArg, sret, callArgs, at))
          return *v;
      }
      std::string name = indirect.empty() ? useFunction(f, args, classTy) : indirect;
      Signature s;
      const Ty* ret;
      {
        ArgsScope scope(*this, &args);
        s = signatureOf(f);
        ret = declType(f)->inner;
      }
      std::string argText;
      size_t slot = 0;
      if (s.hasThis)
      {
        if (thisArg.empty())
          unsupported(at, "a method call without an object");
        argText += "ptr " + thisArg;
        ++slot;
      }
      std::string resultAddr = sret;
      bool resultTemp = false; // a temporary for the result, owned by the statement once the call returned it
      if (s.hasSret)
      {
        if (resultAddr.empty())
        {
          resultAddr = newObjectSlot(ret, at);
          resultTemp = true;
        }
        argText += std::string(argText.empty() ? "" : ", ") + "ptr " + resultAddr;
        ++slot;
      }
      size_t position = slot;
      for (Expr* arg : callArgs)
      {
        if (isPackExpansion(arg))
        {
          Expr* pattern = static_cast<UnaryExpr*>(arg)->operand;
          std::vector<std::string> texts;
          forEachPackElement(pattern, at, [&](size_t)
          {
            const Ty* pt = position < s.paramTys.size() ? s.paramTys[position] : nullptr;
            texts.push_back(argumentText(pt, pattern, at));
            ++position;
          });
          for (const std::string& t : texts)
            argText += std::string(argText.empty() ? "" : ", ") + t;
          continue;
        }
        const Ty* pt = position < s.paramTys.size() ? s.paramTys[position] : nullptr;
        argText += std::string(argText.empty() ? "" : ", ") + argumentText(pt, arg, at);
        ++position;
      }
      size_t provided = position - slot; // argument values emitted (pack expansions counted element by element)
      if (provided < f->params.size())
      {
        // Parameters the call left out take their default arguments, evaluated at the call site
        // (the expression's literals belong to the callee's file; a `caller` location to this one).
        // A template's are the ones of the callee's own instantiation, typed under its arguments
        // (`ScrollBar(const Style* s = &Policy::defaultStyle)` with Policy = VerticalPolicy).
        ParsedFile* callerFile = currentFile;
        FunctionDecl* instance = nullptr;
        for (size_t i = provided; i < f->params.size(); ++i)
        {
          auto* pd = static_cast<ParamDecl*>(f->params[i]);
          if (pd->isPack)
            break;
          if (!pd->defaultValue)
            unsupported(at, "a call to " + qualifiedName(f) + " without an argument for its parameter " +
                              (pd->name != UINT32_MAX ? sym(pd->name) : std::to_string(i + 1)) + " (and the overload resolution let it through)");
          if (!instance)
            instance = args.empty() ? f : global.instanceBody(f, args, classTy);
          Expr* value = i < instance->params.size() ? static_cast<ParamDecl*>(instance->params[i])->defaultValue : nullptr;
          if (!value)
            unsupported(at, "the default argument of " + qualifiedName(f) + " missing from its instantiation");
          const Ty* pt = position < s.paramTys.size() ? s.paramTys[position] : nullptr;
          std::string text;
          if (value->kind == NodeKind::CallerExpr)
            text = "ptr " + callerLocation(pt, at);
          else
          {
            // Emitted from the callee's declaration: its own node (and file) for what it reports,
            // not the caller's token read in the callee's file.
            struct FileRestore { ParsedFile*& f; ParsedFile* saved; ~FileRestore() { f = saved; } } fileRestore{currentFile, callerFile};
            currentFile = f->file;
            // Names in it are the callee's (`Limits<ModifierValueType>::max()` with the class's
            // `using ModifierValueType = double;`), not those of the caller's scope.
            struct ContextRestore { Decl*& d; Decl* saved; ~ContextRestore() { d = saved; } } contextRestore{initializerOf, initializerOf};
            initializerOf = instance;
            ArgsScope calleeArgs(*this, args.empty() ? currentArgs : &args); // CharacterModifier<double>'s ModifierValueType
            text = argumentText(pt, value, value);
          }
          argText += std::string(argText.empty() ? "" : ", ") + text;
          ++position;
        }
      }
      std::string sig = (s.variadic || !indirect.empty()) ? signatureText(s) + " " : s.returnType + " ";
      bool mayThrow = !global.isExternC(f); // C functions do not throw Kpp exceptions
      if (s.returnType == "void")
      {
        emitCall("", sig + name + "(" + argText + ")", mayThrow);
        if (resultTemp)
          statementTemps.push_back({resultAddr, ret}); // not before: a call that throws built no result to destroy
        if (s.hasSret)
          return {resultAddr, ret, sret.empty()};
        return {"", types.voidType(), false};
      }
      std::string r = newTemp();
      emitCall(r + " = ", sig + name + "(" + argText + ")", mayThrow);
      return {r, ret, false};
    }

    // A member declared `= default`: the memberwise constructor, destructor, assignment or
    // comparison the compiler writes for it, on the object at thisArg.
    Value defaultedCall(FunctionDecl* f, const Ty* classTy, const std::string& thisArg, Span<Expr*> args, Node* at)
    {
      if (!classTy || thisArg.empty())
        unsupported(at, "a defaulted function without its object");
      const Ty* t = types.unqualified(types.stripRef(classTy));
      std::string src;
      if (!args.empty())
      {
        if (args.size() != 1)
          unsupported(at, "a defaulted function with " + std::to_string(args.size()) + " arguments");
        // A value of another type, neither t nor a class derived from it, for an assignment or a
        // comparison (`n = "abc"`, `m = String("def")` through Name's implicit constructors,
        // `fixedPoint == 0`): made a temporary of t first, the value evaluated once.
        const Ty* argTy = args[0]->ty ? withCurrentArgs(types.unqualified(types.stripRef(args[0]->ty))) : nullptr;
        std::vector<std::pair<std::string, uint32_t>> path;
        bool related = argTy && isClass(argTy) && !types.isDependent(argTy) &&
                       (canonicalClass(argTy, at) == canonicalClass(t, at) || basePath(argTy, t, path, at));
        // (`data.previewIcons = {Signal(4), Signal(5)}` through a defaulted `operator=(Icons&&)`:
        // the list makes the temporary, as list-initialization of t.)
        bool list = args[0]->kind == NodeKind::BraceInitExpr;
        if (((argTy && argTy->isKnown() && !related) || list) && !(f->flags & (DF_Constructor | DF_Destructor)) && isClass(t))
        {
          std::string tmp = newObjectSlot(t, at);
          Expr* one = args[0];
          constructInto(tmp, t, Span<Expr*>{&one, 1}, at);
          if (needsDestruction(t, at))
            statementTemps.push_back({tmp, t}); // destroyed with the statement's other temporaries
          return defaultedOperation(f, t, thisArg, tmp, at);
        }
        Value v = rvalue(args[0]);
        if (!isClass(v.ty))
        {
          // A vector the checker could not type (an SSE operation intrin.h has as a macro,
          // _mm_shuffle_ps) assigned or copied into a class that wraps it (float32x4 wrapping
          // __m128): stored into the wrapper's first field. A typed one goes through the class's
          // converting constructor above.
          if (v.ty && v.ty->kind == TyKind::Vector && isClass(t))
          {
            StructInfo& info = structFor(t, at);
            if (info.fieldTypes.empty() || llType(info.fieldTypes[0], at) != llType(v.ty, at))
              unsupported(at, "assigning a vector to " + types.toString(t, &symbols) + ", whose first member is not that vector");
            std::string field0 = gep(info.name, thisArg, 0);
            fn += "  store " + llType(v.ty, at) + " " + v.text + ", ptr " + field0 + "\n";
            bool isAssign = (f->flags & DF_Constructor) == 0;
            if (isAssign)
              return {thisArg, classTy, false};
            return {"", types.voidType(), false};
          }
          // For defaulted comparisons (operator==, operator<=>), convert the scalar to
          // a temporary of the class type through its converting constructor, e.g. `fixedPoint == 0`.
          if (!(f->flags & (DF_Constructor | DF_Destructor)) && isClass(t))
          {
            std::string tmp = newObjectSlot(t, at);
            Expr* one = args[0];
            constructInto(tmp, t, Span<Expr*>{&one, 1}, at);
            src = tmp;
          }
          else
            unsupported(at, "a defaulted function given a non-class");
        }
        if (src.empty())
        {
          src = v.text;
          if (classKey(v.ty) != classKey(t) && !baseAddress(v.ty, t, v.text, src, at))
            unsupported(at, "a defaulted function given an unrelated class");
        }
      }
      return defaultedOperation(f, t, thisArg, src, at);
    }

    // The memberwise body of a member declared `= default`, on the object at thisArg (and the one
    // at src: the other operand of a copy, move, assignment or comparison).
    Value defaultedOperation(FunctionDecl* f, const Ty* t, const std::string& thisArg, const std::string& src, Node* at)
    {
      bool move = false;
      if (!f->params.empty())
      {
        TemplateArgs cargs = global.classArgsOf(canonicalClass(t, at));
        ArgsScope scope(*this, &cargs);
        const Ty* pt = declType(f->params[0]);
        move = pt && pt->kind == TyKind::RRef;
      }
      if (f->flags & DF_Constructor)
      {
        if (src.empty())
        {
          storeZero(t, thisArg, at);
          initializeMembers(thisArg, t, {}, at);
          return {"", types.voidType(), false};
        }
        memberwiseConstruct(thisArg, src, t, at, move);
        return {"", types.voidType(), false};
      }
      if (f->flags & DF_Destructor)
      {
        destroyMembers(thisArg, t, at);
        return {"", types.voidType(), false};
      }
      if (!f->name.empty() && f->name.last().special == 2 && !src.empty())
      {
        P op = P(f->name.last().opToken);
        if (op == P::Assign)
        {
          memberwiseAssign(thisArg, src, t, at, move);
          return {thisArg, types.lref(t), false};
        }
        if (op == P::EqEq || op == P::NotEq)
        {
          Value eq = memberwiseEqual(thisArg, src, t, at);
          if (op == P::EqEq)
            return eq;
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + eq.text + ", true\n";
          return {r, types.boolType(), false};
        }
        if (op == P::Spaceship)
        {
          // `operator<=>(const T&) const = default`: the order of the bases and members as the
          // value of `a <=> b` (inlined at a call; emitFunction moves it to the result slot).
          return orderingObject(memberwiseOrder(thisArg, src, t, at), at);
        }
      }
      unsupported(at, "the defaulted function " + qualifiedName(f));
    }

    // a == b for the objects at those addresses, base by base and member by member.
    Value memberwiseEqual(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      const StructInfo& info = structFor(t, at);
      std::string result = "true";
      auto combine = [&](const std::string& v)
      {
        if (result == "true")
        {
          result = v;
          return;
        }
        std::string r = newTemp();
        fn += "  " + r + " = and i1 " + result + ", " + v + "\n";
        result = r;
      };
      for (auto& [baseDef, index] : info.bases)
        combine(objectsEqual(gep(info.name, a, index), gep(info.name, b, index), info.fieldTypes[index], at).text);
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = fieldIndexOf(info, d);
        const Ty* ft = info.fieldTypes[index];
        std::string fa = gep(info.name, a, index), fb = gep(info.name, b, index);
        combine(partsEqual(fa, fb, ft, at).text);
      }
      return {result, types.boolType(), false};
    }

    // a == b of two parts of objects (members, elements) at those addresses.
    Value partsEqual(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      if (isClass(t))
        return objectsEqual(a, b, t, at);
      if (t->kind == TyKind::Array)
        return arraysEqual(a, b, t, at);
      return compare(P::EqEq, load(a, t, at), load(b, t, at), at);
    }

    // a == b of two arrays at those addresses, element by element (the first that differs decides).
    Value arraysEqual(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      if (!t->count || !t->inner)
        unsupported(at, "a defaulted comparison of an array member without its length");
      std::string slot = "%equal" + std::to_string(temp++), index = "%index" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i1\n  " + index + " = alloca i64\n";
      fn += "  store i1 true, ptr " + slot + "\n  store i64 0, ptr " + index + "\n";
      std::string arrayType = llType(t, at);
      std::string cond = newLabel("arrayequal.cond"), body = newLabel("arrayequal.body"), next = newLabel("arrayequal.next"), done = newLabel("arrayequal.done");
      emitBranch(cond);
      startBlock(cond);
      std::string i = newTemp(), more = newTemp();
      fn += "  " + i + " = load i64, ptr " + index + "\n";
      fn += "  " + more + " = icmp ult i64 " + i + ", " + std::to_string(t->count) + "\n";
      fn += "  br i1 " + more + ", label %" + body + ", label %" + done + "\n";
      blockTerminated = true;
      startBlock(body);
      std::string ea = newTemp(), eb = newTemp();
      fn += "  " + ea + " = getelementptr inbounds " + arrayType + ", ptr " + a + ", i64 0, i64 " + i + "\n";
      fn += "  " + eb + " = getelementptr inbounds " + arrayType + ", ptr " + b + ", i64 0, i64 " + i + "\n";
      Value same = partsEqual(ea, eb, t->inner, at);
      fn += "  store i1 " + same.text + ", ptr " + slot + "\n";
      fn += "  br i1 " + same.text + ", label %" + next + ", label %" + done + "\n";
      blockTerminated = true;
      startBlock(next);
      std::string i2 = newTemp();
      fn += "  " + i2 + " = add i64 " + i + ", 1\n  store i64 " + i2 + ", ptr " + index + "\n";
      emitBranch(cond);
      startBlock(done);
      return load(slot, types.boolType(), at);
    }

    // `x OP y` of the objects at a and b, emitted as that expression would be: the class's operator,
    // a free one, or a built-in one through a conversion (a Direction's to its Enum).
    Value comparedAt(P op, const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      VariableDecl& va = comparedVariables.emplace_back();
      VariableDecl& vb = comparedVariables.emplace_back();
      NameExpr& na = comparedNames.emplace_back();
      NameExpr& nb = comparedNames.emplace_back();
      BinaryExpr& cmp = comparisons.emplace_back();
      const Ty* operand = types.lref(types.qualified(t, true, false));
      for (VariableDecl* v : {&va, &vb})
      {
        v->kind = NodeKind::VariableDecl;
        v->token = at ? at->token : 0;
        v->file = currentFile;
      }
      for (auto [n, v] : {std::pair{&na, &va}, std::pair{&nb, &vb}})
      {
        n->kind = NodeKind::NameExpr;
        n->token = at ? at->token : 0;
        n->resolved = v;
        n->ty = operand;
      }
      cmp.kind = NodeKind::BinaryExpr;
      cmp.token = at ? at->token : 0;
      cmp.lhs = &na;
      cmp.rhs = &nb;
      cmp.op = uint32_t(op);
      cmp.isKeyword = false;
      cmp.ty = op == P::Spaceship ? orderingType() : types.boolType();
      locals[&va] = Local{a, t, false};
      locals[&vb] = Local{b, t, false};
      struct Forget
      {
        std::unordered_map<Decl*, Local>& locals;
        Decl* x;
        Decl* y;
        ~Forget()
        {
          locals.erase(x);
          locals.erase(y);
        }
      } forget{locals, &va, &vb};
      return rvalue(&cmp);
    }

    // a == b for class objects: the class's own operator== unless it is defaulted, else memberwise.
    Value objectsEqual(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      const Ty* foundIn = nullptr;
      std::vector<const Ty*> argTypes{types.lref(types.qualified(t, true, false))};
      FunctionDecl* op = findMemberFunction(t, "operator==", argTypes, t, at, &foundIn);
      if (op && !(op->flags & DF_Default))
      {
        TemplateArgs args = global.classArgsOf(foundIn);
        Signature s;
        {
          ArgsScope scope(*this, &args);
          s = signatureOf(op);
        }
        std::string name = useFunction(op, args, foundIn);
        std::string thisArg = a;
        if (canonicalClass(t, at) != foundIn)
          baseAddress(t, foundIn, a, thisArg, at);
        std::string r = newTemp();
        emitCall(r + " = ", s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + b + ")");
        if (s.returnType == "i1")
          return {r, types.boolType(), false};
        return convert(Value{r, types.intType(32), false}, types.boolType(), at);
      }
      if (!op)
        if (const Ty* operand = types.lref(types.qualified(t, true, false)); global.findFreeOperator(uint32_t(P::EqEq), operand, operand, currentFunction))
          return convert(comparedAt(P::EqEq, a, b, t, at), types.boolType(), at); // SHA1Digest's `operator==(const SHA1Digest&, const SHA1Digest&)`
      if (ClassDecl* u = typedUnionOf(t, at))
        return typedUnionEqual(u, a, b, t, at); // the alternative held, then its values (design.md 8.5)
      return memberwiseEqual(a, b, t, at);
    }

    // A brace list as a value of a type that is no class (`x = {}`, `return {};`, an argument `{}`):
    // empty, the type's zero (value initialization); one value, that value converted.
    Value braceScalar(BraceInitExpr* list, const Ty* t, Node* at)
    {
      t = types.unqualified(types.stripRef(t));
      if (!t->isKnown() || isAggregate(t))
        unsupported(at, "a brace list as a value of " + types.toString(t, &symbols));
      if (list->args.empty())
      {
        if (t->kind == TyKind::Pointer || t->kind == TyKind::Nullptr)
          return {"null", t};
        if (t->kind == TyKind::MemberPointer)
          return convert(Value{"null", types.nullptrType()}, t, at);
        return {t->kind == TyKind::Float ? floatConstant(0, t->bits) : t->kind == TyKind::Vector ? "zeroinitializer" : "0", t};
      }
      if (list->args.size() == 1 && list->designators.empty())
        return convert(rvalue(list->args[0]), t, at);
      unsupported(at, "a brace list of " + std::to_string(list->args.size()) + " values as a value of " + types.toString(t, &symbols));
    }

    // Whether e is the literal 0, a null pointer constant.
    static bool nullPointerConstant(Expr* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e || e->kind != NodeKind::LiteralExpr || static_cast<LiteralExpr*>(e)->literalKind != TokenKind::Number)
        return false;
      auto* l = static_cast<LiteralExpr*>(e);
      if (!l->file || !l->file->tokens || l->token >= l->file->tokens->size())
        return false;
      const Token& tok = (*l->file->tokens)[l->token];
      std::string_view text = l->file->text.substr(tok.offset, tok.length);
      return text == "0" || text == "0u" || text == "0U" || text == "0l" || text == "0L" || text == "0ll" || text == "0LL" || text == "0ull" || text == "0ULL";
    }

    // One call argument as "<type> <operand>", converted to the parameter type (null: variadic).
    std::string argumentText(const Ty* pt, Expr* arg, Node* at)
    {
      if (pt && pt->isReference())
      {
        if (isAggregate(types.stripRef(pt)))
        {
          const Ty* cls = types.stripRef(pt);
          if (arg->kind == NodeKind::BraceInitExpr && isInitializerListType(cls))
            return "ptr " + initializerListTemporary(cls, static_cast<BraceInitExpr*>(arg), at); // `max({a, b, c})`
          if (arg->kind == NodeKind::BraceInitExpr && isClass(cls))
            return "ptr " + convertedTemporary(cls, arg, at); // `f({a, b, c})` with `f(const Rect&)`
          const Ty* argTy = arg->ty && arg->ty->isKnown() && types.stripRef(arg->ty)->isKnown() && !types.isDependent(arg->ty) ? types.stripRef(arg->ty) : nullptr;
          if (argTy && isClass(cls) && !isClass(argTy))
            return "ptr " + convertedTemporary(cls, arg, at); // `f("text")` with `f(const String&)`, a lambda with `f(const Function<void()>&)`
          if (argTy && isClass(cls) && isClass(argTy) && classKey(argTy) != classKey(cls) && !baseInstantiation(argTy, classDef(cls->decl, at), at))
          {
            if (typedUnionOf(cls, at))
              return "ptr " + convertedTemporary(cls, arg, at); // `report(Patrolling{3})` with `report(const AIState&)`
            if (conversionOperator(argTy, types.unqualified(cls), at))
              return "ptr " + convert(rvalue(arg), types.unqualified(cls), at).text; // `f(lowercase)` through `operator const String&()`
            // (the argument as overload resolution sees it: an lvalue binds a constructor template's `T&`;
            // a constructor inherited with `using Base::Base;` converts too, as the resolver ranks it)
            if (findConstructor(cls, argTypesOf(Span<Expr*>{&arg, 1}), at) || global.converts(argumentType(arg, arg->ty, types), types.unqualified(cls)))
              return "ptr " + convertedTemporary(cls, arg, at); // `f(view)` with `f(const String&)`
          }
          Value o = rvalue(arg);
          if (!isAggregate(o.ty))
            unsupported(at, "binding a non-class to a class reference");
          std::string addr = o.text;
          if (isClass(o.ty) && classKey(o.ty) != classKey(pt))
          {
            if (baseAddress(o.ty, types.stripRef(pt), o.text, addr, at))
              return "ptr " + addr;
            // Fallback: try implicit constructor of the target class (e.g. Optional(const T&)
            // when passing T where const Optional<T>& is expected).
            if (findConstructor(cls, argTypesOf(Span<Expr*>{&arg, 1}), at))
              return "ptr " + convertedTemporary(cls, arg, at);
            if (conversionOperator(o.ty, types.unqualified(cls), at))
              return "ptr " + convert(o, types.unqualified(cls), at).text;
            unsupported(at, "binding an unrelated class to a reference (" + types.toString(o.ty, &symbols) + " to " + types.toString(pt, &symbols) + ")");
          }
          return "ptr " + addr;
        }
        const Ty* referenced = types.stripRef(pt);
        if (referenced->kind == TyKind::Pointer && referenced->inner && referenced->inner->kind == TyKind::Function && arg->ty &&
            isLambda(types.stripRef(arg->ty)))
        {
          // `Pair(key, lambda)` with `Pair(const K&, const Fn*&)`: the function pointer, in a slot
          // the reference can bind to.
          Value fp = convert(rvalue(arg), referenced, at);
          std::string slot = "%v" + std::to_string(temp++);
          allocas += "  " + slot + " = alloca ptr\n";
          fn += "  store ptr " + fp.text + ", ptr " + slot + "\n";
          return "ptr " + slot;
        }
        // `const double&` given an int (`Constant(const double&)` built from 2): the reference
        // binds a temporary of its own type holding the converted value, never the int itself.
        // A bit-field (UnitTest's `const Actual&` given TreeData's `progress : 4`) the same: it has no
        // address of its own, and its storage unit's address read the neighbouring fields with it.
        const Ty* argTy = arg->ty && arg->ty->isKnown() && !types.isDependent(arg->ty) ? types.unqualified(types.stripRef(arg->ty)) : nullptr;
        if (argTy && argTy->isKnown() && (referenced->isConst || pt->kind == TyKind::RRef) && types.unqualified(referenced)->isKnown() &&
            !types.isDependent(referenced) && (argTy != types.unqualified(referenced) || namesBitField(arg)) && arg->kind != NodeKind::BraceInitExpr)
        {
          const Ty* to = types.unqualified(referenced);
          Value v = convert(rvalue(arg), to, at);
          std::string slot = "%v" + std::to_string(temp++);
          allocas += "  " + slot + " = alloca " + llType(to, at) + "\n";
          fn += "  store " + llType(to, at) + " " + v.text + ", ptr " + slot + "\n";
          return "ptr " + slot;
        }
        return "ptr " + lvalue(arg);
      }
      if (pt && isAggregate(pt))
      {
        if (arg->kind == NodeKind::BraceInitExpr && isInitializerListType(pt))
          return "ptr " + initializerListTemporary(pt, static_cast<BraceInitExpr*>(arg), at); // `max({a, b, c})`
        if (arg->kind == NodeKind::BraceInitExpr && isClass(pt))
        {
          // `f({a, b, c})`; the callee destroys it. The list as a whole, as a variable's: an
          // InitializerList constructor first (`take({Caps::Color, Caps::Light})`), then the fields.
          std::string copy = newObjectSlot(pt, at);
          Expr* one = arg;
          constructInto(copy, pt, Span<Expr*>{&one, 1}, at);
          return "ptr " + copy;
        }
        const Ty* argTy = arg->ty && arg->ty->isKnown() && !types.isDependent(arg->ty) ? types.stripRef(arg->ty) : nullptr;
        if (argTy && isClass(pt) && (!isClass(argTy) || (classKey(argTy) != classKey(pt) &&
                                                         !baseInstantiation(argTy, classDef(pt->decl, at), at) &&
                                                         !conversionOperator(argTy, types.unqualified(pt), at))))
        {
          // Through a converting constructor (a typed union: from an alternative's value); the
          // callee destroys the parameter.
          std::string copy = newObjectSlot(pt, at);
          Expr* one = arg;
          if (typedUnionOf(pt, at))
            initializeFrom(copy, pt, arg, at);
          else
            constructInto(copy, pt, Span<Expr*>{&one, 1}, at);
          return "ptr " + copy;
        }
        Value v = rvalue(arg);
        if (!isAggregate(v.ty))
        {
          // A Vector value being passed to a class parameter that wraps it (e.g. float32x4
          // wrapping __m128): construct the wrapper and store the vector into its first field.
          if (v.ty && v.ty->kind == TyKind::Vector && isClass(pt))
          {
            std::string copy = newObjectSlot(pt, at);
            storeZero(pt, copy, at);
            StructInfo& info = structFor(pt, at);
            std::string field0 = gep(info.name, copy, 0);
            std::string tmp = newTemp();
            fn += "  " + tmp + " = load " + llType(v.ty, at) + ", ptr " + v.text + "\n";
            fn += "  store " + llType(v.ty, at) + " " + tmp + ", ptr " + field0 + "\n";
            return "ptr " + copy;
          }
          unsupported(at, "passing a non-class to a class parameter");
        }
        if (isClass(v.ty) && isClass(pt) && classKey(v.ty) != classKey(pt))
          v = convert(v, types.unqualified(pt), at); // a base, or a conversion operator
        if (isLambda(pt) && v.isTemporary)
        {
          // The closure the lambda expression built is the parameter (the callee destroys it).
          for (size_t i = statementTemps.size(); i-- > 0;)
            if (statementTemps[i].addr == v.text)
            {
              statementTemps.erase(statementTemps.begin() + i);
              break;
            }
          return "ptr " + v.text;
        }
        std::string copy = newObjectSlot(pt, at);
        if (isLambda(pt))
          copyClosure(copy, v.text, pt, at, isXvalue(arg));
        else if (v.isTemporary && isClass(v.ty) && classKey(v.ty) == classKey(pt) && !isLambda(pt))
        {
          // `byValue(Tracker(5))`, `f(makeT())`: the temporary is the parameter, not moved (C++17
          // builds the prvalue in the parameter); the callee destroys it.
          for (size_t i = statementTemps.size(); i-- > 0;)
            if (statementTemps[i].addr == v.text)
            {
              statementTemps.erase(statementTemps.begin() + i);
              break;
            }
          return "ptr " + v.text;
        }
        else if (v.isTemporary)
          moveTemporary(copy, v, pt, at);
        else if (isXvalue(arg))
          moveConstruct(copy, v.text, pt, at); // `f(move(ptr))` with `f(UniquePtr<T> ptr)`
        else
          copyConstruct(copy, v.text, pt, at);
        return "ptr " + copy;
      }
      if (arg->kind == NodeKind::BraceInitExpr && pt)
      {
        Value a = braceScalar(static_cast<BraceInitExpr*>(arg), pt, at); // `deduceSelectedEntity(surface, position, allow, {}, {})`
        return llType(a.ty, at) + " " + a.text;
      }
      if (pt && pt->kind == TyKind::Pointer && nullPointerConstant(arg))
        return "ptr null"; // `Entity* secondaryEntity = 0`
      Value a = rvalue(arg);
      if (isClass(a.ty) && pt && !isAggregate(pt) && conversionOperator(a.ty, types.unqualified(pt), at))
        a = convert(a, types.unqualified(pt), at); // `draw(image)` with `operator const Image*()`
      if (isLambda(a.ty) && pt && pt->kind == TyKind::Pointer && pt->inner && pt->inner->kind == TyKind::Function)
        a = convert(a, pt, at); // a lambda without captures as a pointer to function
      if (isAggregate(a.ty) && !pt)
      {
        // Variadic C argument with no declared type: try the class's implicit conversion operator
        // to a scalar (e.g. String's operator const char*() for lua_pushstring(L, str)).
        FunctionDecl* conv = conversionOperator(a.ty, types.intType(32), at);
        if (conv)
        {
          const Ty* ft = declType(conv);
          const Ty* ret = types.unqualified(types.stripRef(ft->inner));
          a = convert(a, ret, at);
        }
      }
      if (isAggregate(a.ty) && !pt && isClass(a.ty))
      {
        // A class through C's `...` with no conversion to a scalar, as MSVC passes one: a copy, of 1,
        // 2, 4 or 8 bytes as an integer of its size, of any other size by its address
        // (PlatformBackdropCloud's `ssprintf("\"%s\" is unknown ...", input)` with a StringView:
        // a wrong argument on the game's error path, which MSVC compiles).
        std::string copy = newObjectSlot(a.ty, at);
        copyConstruct(copy, a.text, a.ty, at);
        if (needsDestruction(a.ty, at))
          statementTemps.push_back({copy, a.ty});
        uint64_t size = sizeOf(a.ty, at);
        if (size == 1 || size == 2 || size == 4 || size == 8)
        {
          std::string bits = newTemp(), ty = "i" + std::to_string(size * 8);
          fn += "  " + bits + " = load " + ty + ", ptr " + copy + "\n";
          return ty + " " + bits;
        }
        return "ptr " + copy;
      }
      if (isAggregate(a.ty))
        unsupported(at, "passing a class (" + types.toString(a.ty, &symbols) + ") where a scalar is expected");
      if (pt && a.ty->kind == TyKind::Pointer && (pt->kind == TyKind::Int || pt->kind == TyKind::Char || pt->kind == TyKind::Enum))
        unsupported(at, "passing a pointer (" + types.toString(a.ty, &symbols) + ") where " + types.toString(pt, &symbols) + " is expected");
      if (pt && pt->kind == TyKind::Pointer && (a.ty->kind == TyKind::Int || a.ty->kind == TyKind::Char || a.ty->kind == TyKind::Enum))
        unsupported(at, "passing " + types.toString(a.ty, &symbols) + " where a pointer is expected");
      if (pt)
        a = convert(a, pt, at);
      else if (a.ty->kind == TyKind::Float && a.ty->bits == 32)
        a = convert(a, types.floatType(64), at);
      else if ((a.ty->kind == TyKind::Int || a.ty->kind == TyKind::Char) && a.ty->bits < 32)
        a = convert(a, types.intType(32), at);
      else if (a.ty->kind == TyKind::Bool)
        a = convert(a, types.intType(32), at);
      return llType(a.ty, at) + " " + a.text;
    }

    // What a local reference to a scalar binds to: the value itself, or, for a const or rvalue
    // reference given a value of another type, a temporary of the referenced type holding the
    // converted value, as C++ binds it and as argumentText binds a parameter (`const double& d =
    // count;`, a `const B* const&` given a `C*`): the value's own storage holds another
    // representation, and a double read through an int's address is 4 bytes of garbage.
    // Whether e names a bit-field (`data.progress` with `uint8_t progress : 4`), to which a reference
    // binds a temporary holding its value.
    static bool namesBitField(Expr* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      Decl* d = !e ? nullptr : e->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(e)->resolved
                             : e->kind == NodeKind::NameExpr    ? static_cast<NameExpr*>(e)->resolved
                                                                : nullptr;
      return d && d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->bitfield;
    }

    std::string scalarReferenceTarget(const Ty* refTy, Expr* value, Node* at)
    {
      auto scalar = [](const Ty* t)
      {
        return t->kind == TyKind::Bool || t->kind == TyKind::Char || t->kind == TyKind::Int || t->kind == TyKind::Float ||
               t->kind == TyKind::Enum || t->kind == TyKind::Pointer || t->kind == TyKind::Nullptr;
      };
      const Ty* referenced = types.stripRef(refTy);
      const Ty* want = types.unqualified(referenced);
      const Ty* have = value->ty && value->ty->isKnown() && !types.isDependent(value->ty) ? types.unqualified(types.stripRef(value->ty)) : nullptr;
      if (namesBitField(value) && want->isKnown() && !types.isDependent(want) && scalar(want))
      {
        // `const uint8_t& bound = data.progress;`: a temporary holding the bit-field's value, as C++
        // binds it (the field has no address; its storage unit's holds its neighbours too).
        Value converted = convert(rvalue(value), want, at);
        std::string slot = newObjectSlot(want, at);
        fn += "  store " + llType(want, at) + " " + converted.text + ", ptr " + slot + "\n";
        return slot;
      }
      if (!have || have == want || !have->isKnown() || !want->isKnown() || types.isDependent(want) || !scalar(want) || !scalar(have))
        return lvalue(value);
      if (refTy->kind == TyKind::LRef && !referenced->isConst)
      {
        // (C++ binds no other type to a non-const lvalue reference; `long&` to an int is one
        // representation.)
        if (llType(have, at) != llType(want, at))
          unsupported(at, "binding " + types.toString(refTy, &symbols) + " to a value of type " + types.toString(have, &symbols));
        return lvalue(value);
      }
      Value converted = convert(rvalue(value), want, at);
      std::string slot = newObjectSlot(want, at);
      fn += "  store " + llType(want, at) + " " + converted.text + ", ptr " + slot + "\n";
      return slot;
    }

    // A temporary of class type cls built from a non-class value through a converting
    // constructor, alive until the end of the full expression: what a reference parameter binds to.
    std::string convertedTemporary(const Ty* cls, Expr* arg, Node* at)
    {
      const Ty* source = arg->ty ? types.stripRef(arg->ty) : nullptr;
      if (arg->kind != NodeKind::BraceInitExpr && (!source || !source->isKnown()))
        unsupported(at, "converting a value of unknown type to " + types.toString(cls, &symbols)); // (not the copy constructor, again and again)
      Expr* one = arg;
      // The resolver already enforces explicit-ness during overload resolution; codegen trusts it.
      std::string tmp = newObjectSlot(cls, at);
      if (typedUnionOf(cls, at) && arg->kind != NodeKind::BraceInitExpr)
        initializeFrom(tmp, cls, arg, at); // a typed union from an alternative's value (design.md 8.5)
      else
        constructInto(tmp, cls, Span<Expr*>{&one, 1}, at);
      if (needsDestruction(cls, at))
        statementTemps.push_back({tmp, cls});
      return tmp;
    }

    // --- constants ---

    std::string_view spelling(uint32_t token) const
    {
      const std::vector<Token>& toks = *currentFile->tokens;
      const Token& t = toks[std::min<size_t>(token, toks.size() - 1)];
      return currentFile->text.substr(t.offset, t.length);
    }

    // A literal's own file (the parser's): a default argument, a default member initializer or an
    // instance's body is emitted while another file is the current one.
    ParsedFile* literalFile(const LiteralExpr* l) const { return l->file && l->file->tokens ? l->file : currentFile; }
    std::string_view literalSpelling(const LiteralExpr* l, uint32_t token) const
    {
      const ParsedFile* f = literalFile(l);
      const std::vector<Token>& toks = *f->tokens;
      const Token& t = toks[std::min<size_t>(token, toks.size() - 1)];
      return f->text.substr(t.offset, t.length);
    }

    // `(Name)` where Name is a type: a C-style cast the parser could not tell from a parenthesis.
    bool isParenthesizedTypeName(Expr* e)
    {
      if (!e || e->kind != NodeKind::ParenExpr)
        return false;
      Expr* inner = static_cast<ParenExpr*>(e)->inner;
      if (!inner || inner->kind != NodeKind::NameExpr)
        return false;
      Decl* d = static_cast<NameExpr*>(inner)->resolved;
      return d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                   (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)));
    }

    // The type of a constant template parameter's values (`int N`), int when not written.
    const Ty* valueParamType(Decl* param)
    {
      auto* p = param && param->kind == NodeKind::TemplateParamDecl ? static_cast<TemplateParamDecl*>(param) : nullptr;
      if (p && !p->isType && p->valueType)
        if (const Ty* t = global.typeOfAst(p->valueType, p); t && t->isKnown() && !types.isDependent(t))
          return types.unqualified(t);
      return types.intType();
    }

    // The arguments of a variable template's use (`canBuild<String, const char*>`): the written
    // ones, read where the use is written (a function, or another variable template's
    // initializer), a trailing pack taking the rest; then the defaults, read in the template.
    TemplateArgs variableTemplateArgs(NameExpr* n, VariableDecl* v, Node* at)
    {
      TemplateArgs args, none;
      Decl* context = constContext ? constContext : typingContext();
      const NamePart& last = n->name.last();
      std::vector<const Ty*> packElements;
      size_t count = v->templateParams.size();
      auto* lastParam = count ? static_cast<TemplateParamDecl*>(v->templateParams[count - 1]) : nullptr;
      for (size_t i = 0; i < last.templateArgs.size(); ++i)
      {
        Node* arg = last.templateArgs[i];
        const Ty* t = nullptr;
        if (uint8_t(arg->kind) <= uint8_t(NodeKind::PackType))
          t = global.typeOfAstIn(static_cast<Type*>(arg), context, currentArgs ? *currentArgs : none);
        else if (std::optional<int64_t> c = evalConst(static_cast<Expr*>(arg)))
          t = types.valueType(valueParamType(i < count ? v->templateParams[i] : nullptr), *c);
        if (!t || !t->isKnown())
          unsupported(at, "an argument of " + qualifiedName(v) + " that is not a type or a constant");
        if (lastParam && lastParam->isPack && i + 1 >= count)
          packElements.push_back(t);
        else if (i < count)
          args.pairs.push_back({v->templateParams[i], t});
        else
          unsupported(at, "more arguments than " + qualifiedName(v) + " has parameters");
      }
      if (lastParam && lastParam->isPack)
        args.pairs.push_back({lastParam, types.pack(Span<const Ty*>{packElements.data(), uint32_t(packElements.size())})});
      // Fill in default arguments for parameters not covered by the written arguments.
      for (size_t i = args.pairs.size(); i < count; ++i)
      {
        auto* param = static_cast<TemplateParamDecl*>(v->templateParams[i]);
        if (param->defaultArg)
        {
          const Ty* t = nullptr;
          if (param->isType && uint8_t(param->defaultArg->kind) <= uint8_t(NodeKind::PackType))
            t = global.typeOfAstIn(static_cast<Type*>(param->defaultArg), v, args); // `class U = T`: the parameters before it
          else
          {
            ParsedFile* saved = currentFile;
            ConstContextScope inTemplate(*this, v);
            ArgsScope withArgs(*this, &args);
            currentFile = v->file ? v->file : currentFile;
            std::optional<int64_t> c = evalConst(static_cast<Expr*>(param->defaultArg));
            currentFile = saved;
            if (c)
              t = types.valueType(valueParamType(param), *c);
          }
          if (t && t->isKnown())
          {
            args.pairs.push_back({param, t});
            continue;
          }
        }
        unsupported(at, "the variable template " + qualifiedName(v) + " used without all its arguments");
      }
      return args;
    }

    // The code generator's constants (design.md, "Compile-time evaluation"): the checker's
    // evaluator (GlobalScope::evaluate) with this code generator's hooks: signed arithmetic wraps,
    // as the emitted code does, an enumeration is the integer type it is stored as here, and
    // sizeof and alignof are these layouts.
    ConstArith foldArith()
    {
      return ConstArith{types, ConstMode::Fold, nullptr, [this](const Ty* e) { return e->decl ? enumUnderlying(e->decl) : nullptr; }};
    }

    // layoutFailed: set when a layout the evaluation needs fails (it has reported itself).
    ConstHooks foldHooks(Node* at, bool& layoutFailed)
    {
      ConstHooks hooks;
      hooks.mode = ConstMode::Fold;
      hooks.underlying = [this](const Ty* e) { return e->decl ? enumUnderlying(e->decl) : nullptr; };
      hooks.layout = [this, at, &layoutFailed](const Ty* t, bool alignment) -> std::optional<int64_t>
      {
        try
        {
          t = types.stripRef(t);
          return int64_t(alignment ? alignOf(t, at) : sizeOf(t, at));
        }
        catch (Unsupported&)
        {
          layoutFailed = true; // not thrown through the checker's evaluator
          return std::nullopt;
        }
      };
      hooks.offset = [this, at, &layoutFailed](const Ty* t, Decl* m) -> std::optional<int64_t>
      {
        try
        {
          std::optional<uint64_t> o = memberOffset(t, m, at);
          return o ? std::optional<int64_t>(int64_t(*o)) : std::nullopt;
        }
        catch (Unsupported&)
        {
          layoutFailed = true;
          return std::nullopt;
        }
      };
      return hooks;
    }

    // The byte offset of data member m in an object of class t as laid out (offsetof), m declared
    // in t or in one of its bases; nullopt when it is in neither.
    std::optional<uint64_t> memberOffset(const Ty* t, Decl* m, Node* at, int depth = 0)
    {
      t = types.unqualified(types.stripRef(t));
      if (!isClass(t) || depth > 16)
        return std::nullopt;
      classSizeAndAlign(t, at); // laid out, the fields' sizes known
      auto lock = sharedLock();
      StructInfo& info = structFor(t, at);
      auto start = [&](uint32_t index)
      {
        uint64_t offset = 0;
        for (uint32_t i = 0; i < index && i < info.fieldTypes.size(); ++i)
        {
          if (i < info.zeroSize.size() && info.zeroSize[i])
            continue; // an empty base: no storage
          uint64_t a = info.packed ? 1 : alignOf(info.fieldTypes[i], at);
          offset = (offset + a - 1) / a * a + sizeOf(info.fieldTypes[i], at);
        }
        if (index < info.fieldTypes.size() && !(index < info.zeroSize.size() && info.zeroSize[index]))
        {
          uint64_t a = info.packed ? 1 : alignOf(info.fieldTypes[index], at);
          offset = (offset + a - 1) / a * a;
        }
        return offset;
      };
      if (auto it = info.fieldIndex.find(m); it != info.fieldIndex.end())
        return info.isUnion ? 0 : start(it->second);
      for (const auto& [base, index] : info.bases)
        if (index < info.fieldTypes.size())
          if (std::optional<uint64_t> inner = memberOffset(info.fieldTypes[index], m, at, depth + 1))
            return start(index) + *inner;
      return std::nullopt;
    }

    std::optional<int64_t> evalConst(Expr* e, int = 0)
    {
      std::optional<ConstValue> v = foldConst(e);
      if (!v || v->kind != ConstValue::Kind::Int)
        return std::nullopt; // a floating value, an object, a pointer: no integer constant
      return v->i;
    }

    // A condition folded at compile time (if constexpr, static_assert): any scalar constant, a
    // floating one included.
    std::optional<bool> evalCondition(Expr* e)
    {
      std::optional<ConstValue> v = foldConst(e);
      return v && (v->isScalar() || v->isPointer()) ? std::optional<bool>(v->truthy()) : std::nullopt;
    }

    // A folded value given type t (a declaration's): converted when t is an arithmetic or
    // enumeration type; kept for any other type, and for one not known here.
    std::optional<ConstValue> foldAs(std::optional<ConstValue> v, const Ty* t)
    {
      if (!v || !t || !t->isKnown() || types.isDependent(t) || !v->isScalar())
        return v;
      t = types.unqualified(types.stripRef(t));
      return t->isArithmetic() || t->kind == TyKind::Enum ? foldArith().convert(*v, t) : v;
    }

    // A constant expression written in the declaration being folded (constContext) or the function
    // being emitted, read from currentFile, under the current arguments. A number or character
    // literal, the commonest, needs no context.
    std::optional<ConstValue> foldConst(Expr* e)
    {
      if (!e)
        return std::nullopt;
      if (e->kind == NodeKind::LiteralExpr)
      {
        auto* l = static_cast<LiteralExpr*>(e);
        if (l->literalKind == TokenKind::Number || l->literalKind == TokenKind::Char)
          return foldArith().literal(l->literalKind, literalSpelling(l, l->token)); // from its own file's tokens
      }
      TemplateArgs none;
      bool layoutFailed = false;
      std::optional<ConstValue> v =
        global.evaluate(e, currentFile, constContext ? constContext : typingContext(), currentArgs ? *currentArgs : none, foldHooks(e, layoutFailed));
      if (!v && layoutFailed)
        throw Unsupported{}; // the failed layout reported itself; what needs the constant fails with it
      return v;
    }

    int64_t enumeratorValue(EnumeratorDecl* en)
    {
      auto it = enumeratorValues.find(en);
      if (it != enumeratorValues.end())
        return it->second;
      auto* e = static_cast<EnumDecl*>(en->parent);
      int64_t next = 0;
      ParsedFile* saved = currentFile;
      if (e && e->file)
        currentFile = e->file;
      ConstContextScope context(*this, e); // `B = A + 1` names the enumeration's own A
      if (e)
        for (Decl* d : e->enumerators)
        {
          if (d->kind != NodeKind::EnumeratorDecl)
            continue;
          auto* cur = static_cast<EnumeratorDecl*>(d);
          if (cur->value)
            if (auto v = evalConst(cur->value))
              next = *v;
          enumeratorValues[cur] = next;
          ++next;
        }
      currentFile = saved;
      it = enumeratorValues.find(en);
      return it == enumeratorValues.end() ? 0 : it->second;
    }

    // A string literal's bytes with its escapes decoded, UTF-8 as the source is: a universal character
    // name (`é`, `\U0001F600`) is its code point's UTF-8; an octal escape (up to three digits,
    // `\101`) or a hex one is one byte in a narrow literal and one code unit in a wide one (its UTF-8
    // here, which wideStringConstant decodes back). `L"T"` was the five units u0054 and `"\101"`
    // the three characters 101.
    static std::string decodeString(std::string_view s, bool wide = false)
    {
      size_t q = s.find_first_of("\"'");
      if (q == std::string_view::npos)
        return {};
      char quote = s[q];
      bool raw = q > 0 && s[q - 1] == 'R';
      std::string out;
      auto utf8 = [&](uint32_t cp)
      {
        if (cp < 0x80)
          out += char(cp);
        else if (cp < 0x800)
        {
          out += char(0xC0 | (cp >> 6));
          out += char(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
          out += char(0xE0 | (cp >> 12));
          out += char(0x80 | ((cp >> 6) & 0x3F));
          out += char(0x80 | (cp & 0x3F));
        }
        else
        {
          out += char(0xF0 | (cp >> 18));
          out += char(0x80 | ((cp >> 12) & 0x3F));
          out += char(0x80 | ((cp >> 6) & 0x3F));
          out += char(0x80 | (cp & 0x3F));
        }
      };
      auto unit = [&](uint32_t v)
      {
        if (wide)
          utf8(v);
        else
          out += char(v);
      };
      auto hexValue = [](char c) { return uint32_t(isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10); };
      if (raw)
      {
        size_t open = s.find('(', q);
        size_t close = s.rfind(')');
        if (open != std::string_view::npos && close != std::string_view::npos && close > open)
          out.assign(s.substr(open + 1, close - open - 1));
        return out;
      }
      for (size_t i = q + 1; i < s.size() && s[i] != quote; ++i)
      {
        char c = s[i];
        if (c != '\\')
        {
          out += c;
          continue;
        }
        if (++i >= s.size())
          break;
        char e = s[i];
        switch (e)
        {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'a': out += '\a'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'v': out += '\v'; break;
          case 'x':
          {
            uint32_t v = 0;
            while (i + 1 < s.size() && isxdigit((unsigned char)s[i + 1]))
              v = v * 16 + hexValue(s[++i]);
            unit(v);
            break;
          }
          case 'u':
          case 'U':
          {
            uint32_t v = 0;
            for (int digits = e == 'u' ? 4 : 8; digits > 0 && i + 1 < s.size() && isxdigit((unsigned char)s[i + 1]); --digits)
              v = v * 16 + hexValue(s[++i]);
            utf8(v);
            break;
          }
          default:
            if (e >= '0' && e <= '7')
            {
              uint32_t v = uint32_t(e - '0');
              for (int digits = 2; digits > 0 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7'; --digits)
                v = v * 8 + uint32_t(s[++i] - '0');
              unit(v);
            }
            else
              out += e;
            break;
        }
      }
      return out;
    }

    std::string stringConstant(LiteralExpr* l)
    {
      std::string_view first = literalSpelling(l, l->token);
      bool wide16 = first.starts_with("L") || (first.starts_with("u") && !first.starts_with("u8"));
      bool wide32 = first.starts_with("U");
      std::string bytes;
      const std::vector<Token>& toks = *literalFile(l)->tokens;
      for (size_t i = l->token; i < toks.size() && toks[i].getKind() == TokenKind::String; ++i)
        bytes += decodeString(literalSpelling(l, uint32_t(i)), wide16 || wide32);
      if (wide16 || wide32)
        return wideStringConstant(bytes, wide16 ? 16 : 32);
      return cStringConstant(bytes);
    }

    // The code units of a string literal (adjacent ones joined) without its terminating zero, as
    // stringConstant makes them, and their size: chars, or UTF-16/32 units for L"..", u"..", U"..".
    size_t stringLiteralUnits(LiteralExpr* l, uint32_t& unitBytes)
    {
      std::string_view first = literalSpelling(l, l->token);
      bool wide16 = first.starts_with("L") || (first.starts_with("u") && !first.starts_with("u8"));
      unitBytes = wide16 ? 2 : first.starts_with("U") ? 4 : 1;
      std::string bytes;
      const std::vector<Token>& toks = *literalFile(l)->tokens;
      for (size_t i = l->token; i < toks.size() && toks[i].getKind() == TokenKind::String; ++i)
        bytes += decodeString(literalSpelling(l, uint32_t(i)), unitBytes != 1);
      if (unitBytes == 1)
        return bytes.size();
      size_t units = 0;
      for (size_t i = 0; i < bytes.size();)
      {
        unsigned char c = bytes[i];
        size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        units += unitBytes == 2 && n == 4 ? 2 : 1; // a surrogate pair in UTF-16
        i += n;
      }
      return units;
    }

    // `L"text"` / `U"text"`: the UTF-8 bytes decoded and re-encoded as UTF-16 or UTF-32 code units.
    std::string wideStringConstant(const std::string& utf8, int bits)
    {
      std::vector<uint32_t> points;
      for (size_t i = 0; i < utf8.size();)
      {
        unsigned char c = utf8[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (size_t k = 1; k < n && i + k < utf8.size(); ++k)
          cp = (cp << 6) | (utf8[i + k] & 0x3F);
        points.push_back(cp);
        i += n;
      }
      std::vector<uint32_t> units;
      for (uint32_t cp : points)
      {
        if (bits == 16 && cp >= 0x10000)
        {
          cp -= 0x10000;
          units.push_back(0xD800 + (cp >> 10));
          units.push_back(0xDC00 + (cp & 0x3FF));
        }
        else
          units.push_back(cp);
      }
      units.push_back(0);
      std::string unit = bits == 16 ? "i16" : "i32", elements;
      for (size_t i = 0; i < units.size(); ++i)
        elements += std::string(i ? ", " : "") + unit + " " + std::to_string(units[i]);
      std::string contents = "[" + std::to_string(units.size()) + " x " + unit + "] [" + elements + "]";
      auto [name, isNew] = constantName(emittingShared ? "@.swstr." : "@.wstr.", contents);
      if (isNew)
        (emittingShared ? decls : moduleDecls[currentModule]) += name + " = private unnamed_addr constant " + contents + "\n";
      return name;
    }

    // The name of a private constant with these contents in the output being written (the shared
    // declarations, or the current module): its contents decide it, the same in every run whatever
    // order the functions are emitted in; isNew when the caller has to define it there.
    std::unordered_map<std::string, std::string> constantCache; // where + prefix + contents -> the name this emitter got
    std::pair<std::string, bool> constantName(const char* prefix, const std::string& contents)
    {
      std::string key = (emittingShared ? std::string("*") : std::to_string(currentModule)) + "|" + prefix + "|" + contents;
      if (auto mine = constantCache.find(key); mine != constantCache.end())
        return {mine->second, false};
      uint64_t h = 1469598103934665603ull;
      for (unsigned char c : contents)
        h = (h ^ c) * 1099511628211ull;
      char hex[17];
      snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
      std::string base = std::string(prefix) + hex, name = base;
      auto lock = sharedLock();
      auto& defined = emittingShared ? sharedConstants : moduleConstants[currentModule];
      for (int n = 2;; ++n)
      {
        auto [it, inserted] = defined.emplace(name, contents);
        if (inserted || it->second == contents)
        {
          constantCache.emplace(std::move(key), name);
          return {name, inserted};
        }
        name = base + "." + std::to_string(n); // two contents with one hash
      }
    }

    // The IR name of a class's or closure's struct type: a hash of the type as printed, so that it
    // is the same in every run whatever order the types are laid out in (a short name: it is
    // written in every access), a suffix telling apart two types that print alike.
    std::string irTypeName(const char* prefix, const Ty* t)
    {
      auto lock = sharedLock();
      std::string printed = irString(t);
      uint64_t h = 1469598103934665603ull;
      for (unsigned char c : printed)
        h = (h ^ c) * 1099511628211ull;
      char hex[17];
      snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
      std::string base = std::string(prefix) + hex, name = base;
      for (int n = 2;; ++n)
      {
        auto [it, inserted] = irTypeNames.emplace(name, t);
        if (inserted || it->second == t)
          return name;
        name = base + "." + std::to_string(n);
      }
    }

    // A NUL-terminated string constant; returns its global name.
    std::string cStringConstant(std::string bytes)
    {
      bytes += '\0';
      std::string escaped;
      for (unsigned char c : bytes)
      {
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
          escaped += char(c);
        else
        {
          char buf[8];
          snprintf(buf, sizeof buf, "\\%02X", c);
          escaped += buf;
        }
      }
      std::string contents = "[" + std::to_string(bytes.size()) + " x i8] c\"" + escaped + "\"";
      auto [name, isNew] = constantName(emittingShared ? "@.sstr." : "@.str.", contents);
      if (isNew)
        (emittingShared ? decls : moduleDecls[currentModule]) += name + " = private unnamed_addr constant " + contents + "\n";
      return name;
    }

    // The vtables of a class are defined in every module that stores them (linkonce_odr), with the
    // descriptors they point at.
    void useVtable(const StructInfo& info)
    {
      auto lock = sharedLock();
      if (info.vtableText.empty() || !vtablesIn[currentModule].insert(info.vtableName).second)
        return;
      moduleDecls[currentModule] += info.vtableText;
      useDescriptor(info);
    }

    // The name a class descriptor (slot -1 of a vtable) starts with.
    std::string descriptorName(const std::string& descriptor)
    {
      std::string n = newTemp();
      fn += "  " + n + " = load ptr, ptr " + descriptor + "\n";
      return n;
    }

    // dynamic_cast<T*>(p) / dynamic_cast<T&>(x) from a polymorphic class: the whole object (found
    // with the offset in slot -2 of the vtable p's pointer leads to) has its class's descriptor in
    // slot -1, which is searched, bases and their bases, for T's: the result is the whole object
    // moved by the offset T's subobject has in it (a downcast, or a cross-cast to another base), or
    // null for a pointer when T is none of them. Upcasts and non-polymorphic operands fall back to
    // the static conversion (nullopt).
    std::optional<Value> dynamicCast(NamedCastExpr* c, Expr* e)
    {
      const Ty* target = types.stripRef(e->ty);
      if (!target || !target->isKnown())
        return std::nullopt;
      bool pointer = target->kind == TyKind::Pointer;
      const Ty* tcls = pointer ? target->inner : target;
      if (!isClass(tcls))
        return std::nullopt;
      Value o = rvalue(c->operand);
      const Ty* scls = o.ty && o.ty->kind == TyKind::Pointer ? o.ty->inner : types.stripRef(o.ty);
      if (!isClass(scls) || !structFor(scls, e).polymorphic)
        return std::nullopt;
      std::vector<std::pair<std::string, uint32_t>> path;
      if (classKey(scls) == classKey(tcls) || basePath(scls, tcls, path, e))
        return std::nullopt; // the same class or a base of it: the static conversion
      std::string descriptor = descriptorFor(tcls, e);
      useDescriptor(structFor(tcls, e));
      auto lock = sharedLock();
      if (!dyncastEmitted)
      {
        dyncastEmitted = true;
        decls += "$kpp.dyncast = comdat any\n"
                 "define linkonce_odr ptr @kpp.dyncast(ptr %obj, ptr %target) comdat {\n"
                 "entry:\n  %isnull = icmp eq ptr %obj, null\n  br i1 %isnull, label %fail, label %start\n"
                 "start:\n  %vt = load ptr, ptr %obj\n  %tp = getelementptr inbounds ptr, ptr %vt, i64 -2\n  %top = load ptr, ptr %tp\n"
                 "  %topi = ptrtoint ptr %top to i64\n  %dp = getelementptr inbounds ptr, ptr %vt, i64 -1\n  %d = load ptr, ptr %dp\n"
                 "  %off = call i64 @kpp.basesearch(ptr %d, ptr %target)\n  %found = icmp sge i64 %off, 0\n  br i1 %found, label %ok, label %fail\n"
                 "ok:\n  %delta = sub i64 %off, %topi\n  %r = getelementptr i8, ptr %obj, i64 %delta\n  ret ptr %r\n"
                 "fail:\n  ret ptr null\n}\n" +
                 baseSearchFunction("@kpp.basesearch");
      }
      std::string r = newTemp();
      fn += "  " + r + " = call ptr @kpp.dyncast(ptr " + o.text + ", ptr " + descriptor + ")\n";
      if (pointer)
        return Value{r, e->ty, false};
      // `dynamic_cast<const T&>(x)`: the T object itself (its members are called on it); an x that
      // is no T is a wrong program (C++ throws bad_cast), stopped here rather than read through null.
      if (declaredNames.claim("@llvm.trap"))
        decls += "declare void @llvm.trap()\n";
      std::string bad = newTemp(), okL = newLabel("dyncast.ok"), badL = newLabel("dyncast.bad");
      fn += "  " + bad + " = icmp eq ptr " + r + ", null\n";
      fn += "  br i1 " + bad + ", label %" + badL + ", label %" + okL + "\n";
      startBlock(badL);
      fn += "  call void @llvm.trap()\n  unreachable\n";
      blockTerminated = true;
      startBlock(okL);
      return Value{r, target, false};
    }

    // The offset of the class whose descriptor is %target within one whose descriptor is %d (both
    // { name, n, [n x { base descriptor, offset }] }), searching the bases and theirs in order, or -1.
    static std::string baseSearchFunction(const std::string& name)
    {
      return "$" + name.substr(1) + " = comdat any\n"
             "define linkonce_odr i64 " + name + "(ptr %d, ptr %target) comdat {\n"
             "entry:\n  %same = icmp eq ptr %d, %target\n  br i1 %same, label %yes, label %scan\n"
             "scan:\n  %np = getelementptr inbounds i8, ptr %d, i64 8\n  %n = load i64, ptr %np\n  br label %loop\n"
             "loop:\n  %i = phi i64 [ 0, %scan ], [ %i1, %next ]\n  %more = icmp slt i64 %i, %n\n  br i1 %more, label %body, label %no\n"
             "body:\n  %at = mul i64 %i, 16\n  %at1 = add i64 %at, 16\n  %e = getelementptr inbounds i8, ptr %d, i64 %at1\n  %b = load ptr, ptr %e\n"
             "  %op = getelementptr inbounds i8, ptr %e, i64 8\n  %o = load i64, ptr %op\n  %r = call i64 " + name + "(ptr %b, ptr %target)\n"
             "  %hit = icmp sge i64 %r, 0\n  br i1 %hit, label %found, label %next\n"
             "found:\n  %sum = add i64 %r, %o\n  ret i64 %sum\n"
             "next:\n  %i1 = add i64 %i, 1\n  br label %loop\n"
             "yes:\n  ret i64 0\nno:\n  ret i64 -1\n}\n";
    }

    // A floating-point constant in IR: the exact bits, rounded through float for 32-bit values.
    static std::string floatConstant(double value, uint8_t bits)
    {
      double exact = bits == 32 ? double(float(value)) : value;
      uint64_t raw;
      memcpy(&raw, &exact, sizeof raw);
      char buf[32];
      snprintf(buf, sizeof buf, "0x%016llX", (unsigned long long)raw);
      return buf;
    }

    // Line and column (1-based) of a token of the current file.
    std::pair<uint32_t, uint32_t> lineColumnOf(uint32_t token)
    {
      const std::vector<Token>& toks = *currentFile->tokens;
      const Token& t = toks[std::min<size_t>(token, toks.size() - 1)];
      uint32_t line = 1, lineStart = 0;
      for (uint32_t i = 0; i < t.offset && i < currentFile->text.size(); ++i)
        if (currentFile->text[i] == '\n')
        {
          ++line;
          lineStart = i + 1;
        }
      return {line, t.offset - lineStart + 1};
    }

    // The source text from one token up to (not including) another.
    std::string sourceText(uint32_t fromToken, uint32_t toToken)
    {
      const std::vector<Token>& toks = *currentFile->tokens;
      if (fromToken >= toks.size() || toToken <= fromToken)
        return "";
      const Token& first = toks[fromToken];
      const Token& last = toks[std::min<size_t>(toToken - 1, toks.size() - 1)];
      return std::string(currentFile->text.substr(first.offset, last.offset + last.length - first.offset));
    }

    // Builds a CallerLocation {file, line, column, function} for the call at `at`, in a fresh slot.
    std::string callerLocation(const Ty* pt, Node* at)
    {
      const Ty* t = types.stripRef(pt);
      if (!t || !isClass(t))
        unsupported(at, "a caller location parameter whose type is not a class");
      StructInfo& info = structFor(t, at);
      if (info.fieldTypes.size() < 4)
        unsupported(at, "a caller location class without its four fields (file, line, column, function)");
      std::string addr = newObjectSlot(t, at);
      uint32_t token = at->kind == NodeKind::CallExpr ? static_cast<CallExpr*>(at)->callee->token : at->token; // the callee's name
      auto [line, column] = lineColumnOf(token);
      std::string function = currentFunction ? qualifiedName(currentFunction) : std::string();
      fn += "  store ptr " + cStringConstant(currentFile->path) + ", ptr " + gep(info.name, addr, 0) + "\n";
      fn += "  store i32 " + std::to_string(line) + ", ptr " + gep(info.name, addr, 1) + "\n";
      fn += "  store i32 " + std::to_string(column) + ", ptr " + gep(info.name, addr, 2) + "\n";
      fn += "  store ptr " + cStringConstant(function) + ", ptr " + gep(info.name, addr, 3) + "\n";
      return addr;
    }

    // conditional(expr) on a function: false means the function and every call to it vanish.
    bool conditionHolds(FunctionDecl* f)
    {
      if (!f->condition)
        return true;
      ParsedFile* saved = currentFile;
      currentFile = f->file;
      ConstContextScope context(*this, f);
      std::optional<int64_t> v = evalConst(f->condition);
      currentFile = saved;
      if (!v)
        unsupported(f, "a conditional(...) whose condition is not a compile-time constant");
      return *v != 0;
    }

    // LLVM function attributes for [[noinline]] and [[forceinline]].
    static std::string attributeText(Decl* d)
    {
      std::string s;
      if (d->attributes & ATTR_NoInline)
        s += "noinline ";
      if (d->attributes & ATTR_ForceInline)
        s += "alwaysinline ";
      return s;
    }

    // A finished function definition: into the single body, or into the current module. ownItself:
    // one emitted once at its first use (a global's constructor, a lambda's function-pointer thunk),
    // which belongs to no user; otherwise it belongs to the function being emitted.
    void addDefinition(const std::string& name, const std::string& text, bool ownItself = false)
    {
      if (moduleCount <= 1)
      {
        body += text;
        auto lock = sharedLock();
        definedIn[name] = 0; // (what generateLLVM's check of the functions used without a definition reads)
        return;
      }
      std::string declare = declareFromHeader(text);
      Shared::Definition definition = Shared::definitionOf(text);
      definition.name = name;
      definition.owner = ownItself || owner.empty() ? name : owner;
      auto lock = sharedLock();
      sh.definitions[currentModule].push_back(std::move(definition));
      definedIn[name] = currentModule;
      declareLines[name] = std::move(declare);
    }
    // The function whose emission is running (set by emitFunction at the outermost level): what its
    // emission adds (its definition, its lambdas' call operators) belongs to it.
    std::string owner;

    // A module declaration that belongs to the function being emitted (Shared::OwnedEntry).
    void addOwnedEntry(const std::string& text)
    {
      if (moduleCount <= 1)
      {
        moduleDecls[currentModule] += text;
        return;
      }
      auto lock = sharedLock();
      sh.ownedEntries[currentModule].push_back({owner.empty() ? currentFunctionName : owner, text});
    }

    // "declare RET NAME(TYPES)" from a "define RET NAME(TYPE %name, ...) attrs {" header.
    static std::string declareFromHeader(const std::string& text)
    {
      size_t at = text.find(" @");
      size_t nameEnd = text[at + 2] == '"' ? text.find('"', at + 3) + 1 : text.find('(', at);
      size_t open = text.find('(', nameEnd);
      size_t close = text.find(") ", open);
      if (close == std::string::npos || text.find('{', open) < close)
        close = text.find(")", open);
      std::string params;
      size_t p = open + 1;
      while (p < close)
      {
        size_t comma = text.find(", ", p);
        if (comma == std::string::npos || comma > close)
          comma = close;
        std::string one = text.substr(p, comma - p);
        size_t pct = one.rfind(" %");
        if (pct != std::string::npos)
          one = one.substr(0, pct);
        params += (params.empty() ? "" : ", ") + one;
        p = comma + 2;
      }
      return "declare " + text.substr(7, nameEnd - 7) + "(" + params + ")";
    }

    // --stub-missing: a stub for a function used without a definition (ret and params from its
    // declaration), saying which function and where it is declared.
    std::string stubDefinition(const std::string& name, const std::string& ret, const std::string& params)
    {
      auto it = sh.stubMessages.find(name);
      return stubText(name, ret, params, it != sh.stubMessages.end() ? it->second : name);
    }

    // The runtime of --stub-missing's stubs: what was left out, on stderr, and abort.
    void finishStubRuntime()
    {
      if (!sh.stubMissing || !declaredNames.claim("@kpp_missing"))
        return;
      emittingShared = true;
      std::string format = cStringConstant("kppc left this function out (it did not compile; see the compile's errors): %s\n");
      emittingShared = false;
      if (declaredNames.claim("@fprintf"))
        decls += "declare i32 @fprintf(ptr, ptr, ...)\n";
      if (declaredNames.claim("@__acrt_iob_func"))
        decls += "declare ptr @__acrt_iob_func(i32)\n";
      if (declaredNames.claim("@fflush"))
        decls += "declare i32 @fflush(ptr)\n";
      if (declaredNames.claim("@abort"))
        decls += "declare void @abort()\n";
      shared += sharedComdat("@kpp_missing") +
                "define linkonce_odr void @kpp_missing(ptr %what) noinline comdat {\nentry:\n"
                "  %f = call i32 @fflush(ptr null)\n" // (every stream first: the program's own output comes before the message)
                "  %err = call ptr @__acrt_iob_func(i32 2)\n  %r = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + format + ", ptr %what)\n"
                "  %f2 = call i32 @fflush(ptr %err)\n  call void @abort()\n  unreachable\n}\n";
    }

    // Module k: the shared declarations, then declares for what it uses from other modules, then its bodies.
    // The shared declarations without those of functions this module defines: a `declare` next to
    // the `define` is a redefinition to LLVM (a call site whose definition was not found declares
    // the function as external).
    // The shared declarations as module k carries them; without the type definitions when types is
    // false (moduleText writes those the module uses).
    std::string declsFor(unsigned k, bool types = true)
    {
      std::string out;
      const std::string& decls = this->decls.text;
      size_t pos = 0;
      while (pos < decls.size())
      {
        size_t end = decls.find('\n', pos);
        if (end == std::string::npos)
          end = decls.size();
        std::string_view line(decls.data() + pos, end - pos);
        bool skip = !types && isTypeLine(line);
        if (line.rfind("declare ", 0) == 0)
        {
          size_t at = line.find('@');
          if (at != std::string_view::npos)
          {
            size_t nameEnd = line[at + 1] == '"' ? line.find('"', at + 2) + 1 : line.find('(', at);
            if (nameEnd != std::string_view::npos)
            {
              std::string name(line.substr(at, nameEnd - at));
              auto it = definedIn.find(name);
              skip = it != definedIn.end() && it->second == k;
              // --stub-missing: a function of the program used without a definition is defined
              // here as a stub (in every module: linkonce_odr), from its declaration.
              if (!skip && sh.stubMissing && sh.undefinedUses.count(name) && !definedIn.count(name))
              {
                out += stubDefinition(name, std::string(line.substr(8, at - 8)), std::string(line.substr(nameEnd)));
                skip = true;
              }
            }
          }
        }
        if (!skip)
        {
          out.append(line);
          out += '\n';
        }
        pos = end + 1;
      }
      return out;
    }

    // The functions a text has `declare` lines for.
    static void declaredIn(std::string_view text, std::unordered_set<std::string>& out)
    {
      for (size_t pos = 0; pos < text.size();)
      {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
          end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        if (line.rfind("declare ", 0) == 0)
          if (size_t at = line.find('@'); at != std::string_view::npos)
          {
            size_t nameEnd = line[at + 1] == '"' ? line.find('"', at + 2) + 1 : line.find('(', at);
            if (nameEnd != std::string_view::npos && nameEnd > at)
              out.insert(std::string(line.substr(at, nameEnd - at)));
          }
        pos = end + 1;
      }
    }

    // A type definition line of the shared declarations (`%struct.<hash> = type {...}`, `%closure...`).
    static bool isTypeLine(std::string_view line) { return !line.empty() && line[0] == '%' && line.find(" = type ") != std::string_view::npos; }

    // Calls found(name) for each class or closure type a text names (`%struct.<hash>`, `%closure.<hash>`).
    template<typename F>
    static void typeNamesIn(std::string_view text, F&& found)
    {
      for (size_t i = text.find('%'); i != std::string_view::npos; i = text.find('%', i + 1))
      {
        std::string_view rest = text.substr(i + 1);
        if (!rest.starts_with("struct.") && !rest.starts_with("closure."))
          continue;
        size_t end = i + 1;
        while (end < text.size() && (isalnum((unsigned char)text[end]) || text[end] == '.' || text[end] == '_'))
          ++end;
        found(text.substr(i, end - i));
        i = end - 1;
      }
    }

    // What every module's assembly reads, filled once before the modules are assembled on all
    // threads: the functions the shared declarations declare and those they name (a global's table
    // of function pointers), and the type definitions with the types each names.
    void prepareModuleTexts()
    {
      declaredIn(decls.text, sharedDeclared);
      const std::string& shared = decls.text;
      for (size_t i = shared.find("@\""); i != std::string::npos; i = shared.find("@\"", i + 1))
      {
        size_t end = shared.find('"', i + 2);
        if (end == std::string::npos)
          break;
        std::string name = shared.substr(i, end + 1 - i);
        if (definedIn.count(name) && !sharedDeclared.count(name))
          sh.sharedReferences.insert(name);
        i = end;
      }
      std::string_view text = decls.text;
      for (size_t pos = 0; pos < text.size();)
      {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
          end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        if (isTypeLine(line))
        {
          sh.typeIndex.emplace(line.substr(0, line.find(" = type ")), uint32_t(sh.typeLines.size()));
          sh.typeLines.push_back({line, {}});
        }
        pos = end + 1;
      }
      for (Shared::TypeLine& t : sh.typeLines)
        typeNamesIn(t.line.substr(t.line.find(" = type ")), [&](std::string_view name)
        {
          if (auto it = sh.typeIndex.find(name); it != sh.typeIndex.end())
            t.uses.push_back(it->second);
        });
    }

    // Module k's text. Reads only what is finished (the texts) and prepareModuleTexts' lookups, so
    // the modules are assembled on all threads at once.
    std::string moduleText(unsigned k, const std::string& prefix)
    {
      std::string ctors = ctorsText(k); // module 0 runs the startup functions of every module
      std::string decls = declsFor(k, false);
      std::unordered_set<std::string> already; // declarations the module carries (shared, or its own)
      declaredIn(moduleDecls[k].text, already);
      std::string declares;
      std::unordered_set<std::string> seen;
      auto declare = [&](const std::string& name)
      {
        auto line = declareLines.find(name);
        if (line != declareLines.end())
          declares += line->second + "\n";
      };
      // The names the module's texts use, in the order they appear (its declarations, its
      // definitions as sorted, the shared definitions, the startup list): each function's names were
      // found when it was emitted; the other texts are scanned here.
      auto consider = [&](std::string_view nameView)
      {
        std::string name(nameView);
        if (!seen.insert(name).second)
          return;
        auto it = definedIn.find(name);
        if (it != definedIn.end() && it->second != k && !sharedDeclared.count(name) && !already.count(name))
          declare(name); // not when a declaration is in the module's text already
      };
      auto scan = [&](std::string_view text) { irNamesIn(text, [&](size_t at, size_t length) { consider(text.substr(at, length)); }); };
      const std::vector<Shared::Definition>& definitions = sh.definitions[k];
      scan(moduleDecls[k].text);
      for (const Shared::Definition& d : definitions)
        for (auto [at, length] : d.names)
          consider(std::string_view(d.text).substr(at, length));
      scan(shared.text);
      scan(ctors);
      // The functions the shared declarations name: every module but the defining one declares them.
      for (const std::string& name : sh.sharedReferences)
        if (auto it = definedIn.find(name); it != definedIn.end() && it->second != k && !already.count(name) && seen.insert(name).second)
          declare(name);
      // The type definitions the module names, and those they name in turn (every module had all of
      // the game's 24 k), in the shared declarations' order.
      std::vector<char> used(sh.typeLines.size(), 0);
      std::vector<uint32_t> work;
      auto mark = [&](uint32_t t)
      {
        if (!used[t])
        {
          used[t] = 1;
          work.push_back(t);
        }
      };
      auto markName = [&](std::string_view name)
      {
        if (auto it = sh.typeIndex.find(name); it != sh.typeIndex.end())
          mark(it->second);
      };
      typeNamesIn(moduleDecls[k].text, markName);
      for (const Shared::Definition& d : definitions)
        for (auto [at, length] : d.types)
          markName(std::string_view(d.text).substr(at, length));
      typeNamesIn(shared.text, markName);
      typeNamesIn(ctors, markName);
      typeNamesIn(decls, markName);
      typeNamesIn(declares, markName);
      while (!work.empty())
      {
        uint32_t t = work.back();
        work.pop_back();
        for (uint32_t u : sh.typeLines[t].uses)
          mark(u);
      }
      std::string types;
      for (size_t t = 0; t < sh.typeLines.size(); ++t)
        if (used[t])
        {
          types.append(sh.typeLines[t].line);
          types += '\n';
        }
      size_t size = prefix.size() + types.size() + decls.size() + moduleDecls[k].text.size() + ctors.size() + 1 + shared.text.size() + declares.size() + 1;
      for (const Shared::Definition& d : definitions)
        size += d.text.size();
      std::string out;
      out.reserve(size);
      out += prefix;
      out += types;
      out += decls;
      out += moduleDecls[k].text;
      out += ctors;
      out += '\n';
      out += shared.text;
      out += declares;
      out += '\n';
      for (const Shared::Definition& d : definitions)
        out += d.text;
      return out;
    }

    // The default assert handler, unless the program defines `extern "C" kpp_assert_failed`.
    void finishAssertRuntime()
    {
      if (!usesAssert || !declaredNames.claim("@kpp_assert_failed"))
        return;
      emittingShared = true;
      std::string format = cStringConstant("%s:%u:%u: assertion failed: %s\n");
      if (declaredNames.claim("@fprintf"))
        decls += "declare i32 @fprintf(ptr, ptr, ...)\n";
      if (declaredNames.claim("@__acrt_iob_func"))
        decls += "declare ptr @__acrt_iob_func(i32)\n";
      if (declaredNames.claim("@abort"))
        decls += "declare void @abort()\n";
      shared += "$kpp_assert_failed = comdat any\n"
                "define linkonce_odr void @kpp_assert_failed(ptr %file, i32 %line, i32 %column, ptr %expression) noinline comdat {\nentry:\n"
              "  %err = call ptr @__acrt_iob_func(i32 2)\n"
              "  %0 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + format + ", ptr %file, i32 %line, i32 %column, ptr %expression)\n"
              "  call void @abort()\n  unreachable\n}\n";
      emittingShared = false;
    }

    // --- values ---

    // The class's `operator T()` converting to `to` (exactly, or any scalar one when none is exact),
    // its own or inherited (`operator bool` of a BoolConstant base); explicit ones for bool, where
    // conditions allow them, and in an explicit cast when they yield the type itself
    // (`double(b)`, `static_cast<double>(b)` with `explicit operator const double&()`: direct
    // initialisation's rule). foundIn: the class (or base) declaring it.
    // opArgs: the arguments to call it with (the class's, and for an operator template the one its
    // result is deduced to: `template<class T> operator T()` converting to double has T = double).
    FunctionDecl* conversionOperator(const Ty* from, const Ty* to, Node* at, const Ty** foundIn = nullptr, bool explicitCast = false,
                                     TemplateArgs* opArgs = nullptr)
    {
      return findConversion(from, at, foundIn, [&](FunctionDecl* f, const TemplateArgs& args, const Ty* ret) -> int
      {
        bool exact = ret == types.unqualified(to) || (isClass(ret) && isClass(to) && classKey(ret) == classKey(to));
        // A class derived from `to` serves too: its `to` part (SignalIDConnector's `operator
        // SignalID()` where an IDWithQuality, SignalID's base, is wanted), after the conversion to
        // `to` itself if there is one (C++ compares the derived-to-base conversion after it: a
        // RailBuilder made a RailSignalBase& takes `operator RailSignalBase&()`, not `operator
        // RailChainSignal&()`).
        std::vector<std::pair<std::string, uint32_t>> path;
        bool derived = !exact && isClass(ret) && isClass(to) && !types.isDependent(ret) && basePath(ret, to, path, at);
        if (global.isExplicit(f, args) && to->kind != TyKind::Bool && (!explicitCast || !(exact || derived)))
          return 0;
        // (A scalar is no way to a class: from it a converting constructor would be a second
        // user-defined conversion. Direction's `operator Enum()` is not how a Direction becomes a
        // RealOrientation; RealOrientation's constructor from a Direction is.)
        return exact ? 2 : derived ? 1 : isClass(ret) || isClass(to) ? 0 : 1;
      }, to, opArgs);
    }

    // A class value through its (non-explicit) conversion operator to a pointer, own or inherited,
    // or nothing when it has none: what `*` and `delete` of such an object work on.
    std::optional<Value> toPointer(const Value& object, Node* at)
    {
      const Ty* owner = nullptr;
      TemplateArgs opArgs;
      FunctionDecl* op = findConversion(object.ty, at, &owner, [&](FunctionDecl* f, const TemplateArgs& args, const Ty* ret) -> int
      {
        return !global.isExplicit(f, args) && ret->kind == TyKind::Pointer ? 2 : 0;
      }, nullptr, &opArgs);
      if (!op)
        return std::nullopt;
      return callConversion(op, object.ty, owner, object.text, at, &opArgs);
    }

    // A class's (non-explicit) conversion operator to an enum or an integer, own or inherited, or
    // null: what a switch on the object switches on.
    FunctionDecl* integralConversionOperator(const Ty* from, Node* at, const Ty** foundIn = nullptr)
    {
      return findConversion(from, at, foundIn, [&](FunctionDecl* f, const TemplateArgs& args, const Ty* ret) -> int
      {
        if (global.isExplicit(f, args))
          return 0;
        return ret->kind == TyKind::Enum || ret->kind == TyKind::Int || ret->kind == TyKind::Char || ret->kind == TyKind::Bool ? 2 : 0;
      });
    }

    // The conversion operator of `from` or its bases (the class first, then each base level) that
    // score rates best: 2 at once, else the first rated 1; 0 is not a candidate.
    // to: the type converted to, when known, which a conversion operator template deduces its
    // result from; opArgs: the arguments the one found is called with.
    template<typename Score>
    FunctionDecl* findConversion(const Ty* from, Node* at, const Ty** foundIn, Score&& score, const Ty* to = nullptr, TemplateArgs* opArgs = nullptr)
    {
      FunctionDecl* best = nullptr;
      const Ty* bestIn = nullptr;
      TemplateArgs bestArgs;
      int bestScore = 0;
      std::vector<const Ty*> classes{canonicalClass(from, at)};
      for (size_t i = 0; i < classes.size() && i < 64 && bestScore < 2; ++i)
      {
        const Ty* cls = classes[i];
        ClassDecl* c = classDef(cls->decl, at);
        TemplateArgs args = global.classArgsOf(cls);
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          const Ty* ft = declType(m);
          if (!ft || ft->kind != TyKind::Function || !ft->inner || !ft->inner->isKnown())
            continue;
          TemplateArgs fargs = args;
          const Ty* ret = types.unqualified(types.stripRef(ft->inner));
          if (!f->templateParams.empty())
          {
            // `template<class T> operator T()` (or `operator const T&()`): T is the type converted
            // to, and the operator a candidate only when its constraints hold for it.
            if (!to || ret->kind != TyKind::Param || std::find(f->templateParams.begin(), f->templateParams.end(), ret->decl) == f->templateParams.end())
              continue;
            Decl* param = ret->decl;
            ret = types.unqualified(types.stripRef(to));
            fargs.pairs.push_back({param, ret});
            if (!global.constraintsHold(f, fargs))
              continue;
          }
          int s = score(f, fargs, ret);
          if (s > bestScore)
          {
            best = f;
            bestIn = cls;
            bestArgs = fargs;
            bestScore = s;
            if (s >= 2)
              break;
          }
        }
        for (const ClassDecl::Base& b : c->bases)
          if (const Ty* bt = global.typeOfAstIn(b.type, c, args); bt && isClass(bt) && !types.isDependent(bt))
            classes.push_back(canonicalClass(bt, at));
      }
      if (foundIn)
        *foundIn = bestIn;
      if (opArgs)
        *opArgs = bestArgs;
      return best;
    }

    // Calls a conversion operator found in `owner` (the object's class or one of its bases) on
    // the object at addr.
    Value callConversion(FunctionDecl* op, const Ty* objectTy, const Ty* owner, const std::string& addr, Node* at, const TemplateArgs* opArgs = nullptr)
    {
      std::string self = addr;
      if (owner != canonicalClass(objectTy, at) && !baseAddress(objectTy, owner, addr, self, at))
        unsupported(at, "a conversion operator of the base " + types.toString(owner, &symbols) + ", which the layout of " +
                            types.toString(objectTy, &symbols) + " does not have");
      // A virtual one is the object's override (ScriptRenderBase's pure `virtual explicit operator
      // bool() const`, which was called directly: a link error, or the base's body for an impure one).
      std::string indirect;
      if (op->templateParams.empty() && isClass(owner) && structFor(owner, at).polymorphic)
        if (int slot = slotOf(owner, op, at); slot >= 0)
          indirect = loadSlot(self, slot);
      return callRvalue(callFunction(op, opArgs && !op->templateParams.empty() ? *opArgs : global.classArgsOf(owner), owner, self, "", {}, at, indirect), at);
    }

    // explicitCast: the conversion an explicit cast asks for, which may use explicit conversion
    // operators.
    Value convert(Value v, const Ty* to, Node* at, bool explicitCast = false)
    {
      const Ty* from = v.ty;
      to = types.stripRef(to);
      if (!to || !to->isKnown())
        unsupported(at, "a conversion to an unknown type");
      if (from == to || types.unqualified(from) == types.unqualified(to))
        return {v.text, to, v.isTemporary};
      // A value whose type the checker did not find is no pointer to compare with null or pass on.
      if (!from->isKnown())
        unsupported(at, "a conversion from a value of unknown type to " + types.toString(to, &symbols));
      auto kindOf = [&](const Ty* t) { return t->kind == TyKind::Enum ? TyKind::Int : t->kind; };
      TyKind fk = kindOf(from), tk = kindOf(to);
      auto bitsOf = [&](const Ty* t) -> uint8_t { return t->kind == TyKind::Enum ? uint8_t(enumUnderlying(t->decl)->bits) : t->bits; };
      auto unsignedOf = [&](const Ty* t) { return t->kind == TyKind::Enum ? enumUnderlying(t->decl)->isUnsigned : t->isUnsigned; };
      if (isLambda(from) && tk == TyKind::Pointer && to->inner && to->inner->kind == TyKind::Function)
        return {lambdaThunk(from, to->inner, at), to}; // a lambda without captures as a pointer to function
      if (fk == TyKind::Class || tk == TyKind::Class)
      {
        if (fk == TyKind::Class && tk == TyKind::Class)
        {
          std::string addr;
          if (baseAddress(from, to, v.text, addr, at))
            return {addr, to, v.isTemporary && addr == v.text};
        }
        if (fk == TyKind::Class)
        {
          const Ty* owner = nullptr;
          TemplateArgs opArgs;
          if (FunctionDecl* op = conversionOperator(from, to, at, &owner, explicitCast, &opArgs))
            return convert(callConversion(op, from, owner, v.text, at, &opArgs), to, at);
        }
        // (A class without a conversion to bool is no condition: never its address compared with null.)
        unsupported(at, "a conversion from " + types.toString(from, &symbols) + " to " + types.toString(to, &symbols));
      }
      // The result's name, taken when an instruction defines it: a conversion that emits nothing (the
      // same bits, a pointer to a pointer) takes no number. Which of two equivalent types a value
      // carries can differ from run to run (`const Chunk*` through forward<const Chunk*&> or through
      // ChunkSet's own argument), and a number taken for nothing renumbered the rest of the function
      // (Vector<ChunkSet<...>>::emplace_back, one run in four at -j 32).
      std::string r;
      auto result = [&]() -> const std::string& { if (r.empty()) r = newTemp(); return r; };
      // A pointer to a member function is the function (null when none), one to a data member the
      // member's offset, -1 when null (offset 0 is the first member's), as MSVC has them.
      if (tk == TyKind::MemberPointer && fk == TyKind::Nullptr)
        return {to->inner && to->inner->kind == TyKind::Function ? "null" : "-1", to};
      if (fk == TyKind::MemberPointer && tk == TyKind::Bool)
      {
        bool function = from->inner && from->inner->kind == TyKind::Function;
        fn += "  " + result() + " = icmp ne " + (function ? "ptr " + v.text + ", null" : "i64 " + v.text + ", -1") + "\n";
        return {r, to};
      }
      // `int (Both::*)() const = &Named::getId`: a pointer to a member of a base, as one of a class
      // derived from it. A data member's offset grows by the base's place; a member function gets a
      // thunk moving `this` to the base, for a function known here (a pointer read at run time
      // carries nothing to move by).
      if (fk == TyKind::MemberPointer && tk == TyKind::MemberPointer && !from->args.empty() && !to->args.empty() && isClass(from->args[0]) &&
          isClass(to->args[0]) && classKey(from->args[0]) != classKey(to->args[0]))
      {
        const Ty* base = withCurrentArgs(from->args[0]);
        const Ty* derived = withCurrentArgs(to->args[0]);
        std::vector<std::pair<std::string, uint32_t>> path;
        if (basePath(derived, base, path, at))
        {
          bool moved = false;
          for (auto& [structName, index] : path)
            moved = moved || index != 0;
          if (!moved)
            return {v.text, to};
          if (from->inner && from->inner->kind == TyKind::Function)
          {
            if (v.text.empty() || v.text[0] != '@')
              unsupported(at, "a pointer to a member function of " + types.toString(base, &symbols) + " read at run time, as one of " +
                                  types.toString(derived, &symbols) + ", which has that base at an offset");
            std::string function = v.text[1] == '"' ? v.text.substr(2, v.text.size() - 3) : v.text.substr(1);
            return {sharedThunk("@\"pmf." + irString(derived) + "." + function + "\"", methodSignature(from->inner, at), pathAtNull(path), v.text, -1), to};
          }
          std::string offset = newTemp(), sum = newTemp(), isNull = newTemp();
          fn += "  " + offset + " = ptrtoint ptr " + pathAtNull(path) + " to i64\n";
          fn += "  " + sum + " = add i64 " + v.text + ", " + offset + "\n";
          fn += "  " + isNull + " = icmp eq i64 " + v.text + ", -1\n";
          fn += "  " + result() + " = select i1 " + isNull + ", i64 -1, i64 " + sum + "\n";
          return {r, to};
        }
      }
      std::string fromT = llType(from, at), toT = llType(to, at);
      if (fromT == toT && !(fk == TyKind::Pointer && tk == TyKind::Pointer))
        return {v.text, to, v.isTemporary}; // the same bits: bool to `enum class E : bool`, uint8 to an int8 enum
      // (two class pointers are both `ptr`, yet Derived* -> Base* may move to the base subobject)
      if ((fk == TyKind::Int || fk == TyKind::Char) && (tk == TyKind::Int || tk == TyKind::Char))
      {
        uint8_t fb = bitsOf(from), tb = bitsOf(to);
        if (fb == tb)
          return {v.text, to};
        fn += "  " + result() + " = " + (fb > tb ? "trunc" : unsignedOf(from) ? "zext" : "sext") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if (fk == TyKind::Bool && (tk == TyKind::Int || tk == TyKind::Char))
      {
        fn += "  " + result() + " = zext i1 " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if ((fk == TyKind::Int || fk == TyKind::Char) && tk == TyKind::Bool)
      {
        fn += "  " + result() + " = icmp ne " + fromT + " " + v.text + ", 0\n";
        return {r, to};
      }
      if ((fk == TyKind::Pointer || fk == TyKind::Nullptr) && tk == TyKind::Bool)
      {
        fn += "  " + result() + " = icmp ne ptr " + v.text + ", null\n";
        return {r, to};
      }
      if (fk == TyKind::Float && tk == TyKind::Bool)
      {
        fn += "  " + result() + " = fcmp une " + fromT + " " + v.text + ", 0.0\n";
        return {r, to};
      }
      if (fk == TyKind::Float && (tk == TyKind::Int || tk == TyKind::Char))
      {
        fn += "  " + result() + " = " + (unsignedOf(to) ? "fptoui" : "fptosi") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if ((fk == TyKind::Int || fk == TyKind::Char || fk == TyKind::Bool) && tk == TyKind::Float)
      {
        bool u = fk == TyKind::Bool || unsignedOf(from);
        fn += "  " + result() + " = " + (u ? "uitofp" : "sitofp") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if (fk == TyKind::Float && tk == TyKind::Float)
      {
        fn += "  " + result() + " = " + (from->bits > to->bits ? "fptrunc" : "fpext") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if (tk == TyKind::Pointer && (fk == TyKind::Pointer || fk == TyKind::Nullptr || fk == TyKind::Function || fk == TyKind::Array))
      {
        if (fk == TyKind::Pointer && isClass(from->inner) && isClass(to->inner) && classKey(from->inner) != classKey(to->inner))
        {
          std::string addr;
          if (baseAddress(from->inner, to->inner, v.text, addr, at))
            return {keepNull(v.text, addr), to};
        }
        return {fk == TyKind::Nullptr ? "null" : v.text, to};
      }
      if (tk == TyKind::Pointer && fk == TyKind::Int)
      {
        // A signed integer narrower than a pointer is widened as a signed value first, as through
        // intptr_t: `(HANDLE)-1` is all ones (INVALID_HANDLE_VALUE); inttoptr alone zero-extends.
        std::string value = v.text, valueT = fromT;
        if (!unsignedOf(from) && bitsOf(from) < 64)
        {
          value = newTemp();
          fn += "  " + value + " = sext " + fromT + " " + v.text + " to i64\n";
          valueT = "i64";
        }
        fn += "  " + result() + " = inttoptr " + valueT + " " + value + " to ptr\n";
        return {r, to};
      }
      if (fk == TyKind::Pointer && tk == TyKind::Int)
      {
        // Only a cast turns a pointer into an integer (`(uintptr_t)p`, reinterpret_cast); `int y =
        // &x;` is an error in C++, and was a silently truncated address here.
        if (!explicitCast)
          fail(at, "error: a pointer (" + types.toString(from, &symbols) + ") does not convert to " + types.toString(to, &symbols) + " without a cast");
        fn += "  " + result() + " = ptrtoint ptr " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      // SSE vector to scalar: extract lane 0.
      // __m128 -> float, __m128d -> double, __m128i -> int32/int64.
      if (fk == TyKind::Vector && (tk == TyKind::Float || tk == TyKind::Int || tk == TyKind::Char))
      {
        // If element types differ, bitcast the vector first.
        const Ty* elemTy = from->inner;
        if (elemTy && elemTy->kind != tk)
        {
          // e.g. <4 x i32> -> double: bitcast <4 x i32> to <2 x double> then extract.
          // or <4 x float> -> int32: bitcast <4 x float> to <4 x i32> then extract.
          uint32_t totalBits = from->count * (elemTy->bits ? elemTy->bits : 32);
          uint32_t newCount = totalBits / to->bits;
          const Ty* newVec = types.vectorType(to, newCount);
          std::string t1 = newTemp();
          fn += "  " + t1 + " = bitcast " + fromT + " " + v.text + " to " + llType(newVec, at) + "\n";
          fn += "  " + result() + " = extractelement " + llType(newVec, at) + " " + t1 + ", i32 0\n";
        }
        else
        {
          fn += "  " + result() + " = extractelement " + fromT + " " + v.text + ", i32 0\n";
        }
        return {r, to};
      }
      // Scalar to SSE vector: splat (broadcast to all lanes).
      if (tk == TyKind::Vector && (fk == TyKind::Float || fk == TyKind::Int || fk == TyKind::Char || fk == TyKind::Bool))
      {
        const Ty* elemTy = to->inner;
        // Convert the scalar to the element type first if needed.
        Value scalar = (from != elemTy && types.unqualified(from) != types.unqualified(elemTy))
                        ? convert(v, elemTy, at) : v;
        std::string t1 = newTemp(), t2 = newTemp();
        fn += "  " + t1 + " = insertelement " + toT + " undef, " + llType(elemTy, at) + " " + scalar.text + ", i32 0\n";
        // Shuffle to splat lane 0 to all lanes.
        std::string mask = "<";
        for (uint32_t i = 0; i < to->count; ++i)
          mask += (i ? ", " : "") + std::string("i32 0");
        mask += ">";
        fn += "  " + t2 + " = shufflevector " + toT + " " + t1 + ", " + toT + " undef, <" + std::to_string(to->count) + " x i32> " + mask + "\n";
        return {t2, to};
      }
      // Vector to vector bitcast (same total size, different element types).
      if (fk == TyKind::Vector && tk == TyKind::Vector)
      {
        fn += "  " + result() + " = bitcast " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      unsupported(at, "a conversion from " + types.toString(from, &symbols) + " to " + types.toString(to, &symbols));
    }

    // Address of the `base` subobject inside the `derived` object at addr (false if not a base).
    // An explicit cast: what convert does, plus the downcast to a derived class (pointer or
    // reference) that an implicit conversion never makes.
    Value explicitConvert(Value v, const Ty* to, Node* at)
    {
      const Ty* target = types.stripRef(to);
      const Ty* from = v.ty;
      bool pointers = from && target && from->kind == TyKind::Pointer && target->kind == TyKind::Pointer && isClass(from->inner) && isClass(target->inner);
      bool objects = isClass(from) && isClass(target);
      if (pointers || objects)
      {
        const Ty* fromCls = pointers ? from->inner : from;
        const Ty* toCls = pointers ? target->inner : target;
        std::vector<std::pair<std::string, uint32_t>> path;
        if (classKey(fromCls) != classKey(toCls) && basePath(toCls, fromCls, path, at))
        {
          std::string addr = derivedAddress(path, v.text);
          return {pointers ? keepNull(v.text, addr) : addr, pointers ? target : types.unqualified(toCls), false}; // `static_cast<VerticalFlow&>(layout)`
        }
      }
      return convert(v, to, at, true);
    }

    // A pointer moved to a base or derived subobject: null stays null (C++'s rule for pointer conversions).
    std::string keepNull(const std::string& original, const std::string& adjusted)
    {
      if (adjusted == original)
        return adjusted;
      std::string isNull = newTemp(), r = newTemp();
      fn += "  " + isNull + " = icmp eq ptr " + original + ", null\n";
      fn += "  " + r + " = select i1 " + isNull + ", ptr null, ptr " + adjusted + "\n";
      return r;
    }

    // The chain of base-class fields from derived down to base: (struct name, field index) pairs.
    bool basePath(const Ty* derived, const Ty* base, std::vector<std::pair<std::string, uint32_t>>& path, Node* at)
    {
      if (canonicalClass(derived, at) == canonicalClass(base, at))
        return true;
      StructInfo& info = structFor(derived, at);
      std::string name = info.name;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      for (auto& [baseDef, index] : bases)
      {
        path.push_back({name, index});
        if (basePath(fieldTypes[index], base, path, at))
          return true;
        path.pop_back();
      }
      return false;
    }

    // The derived object's address from its base subobject's, back along the path of base fields.
    std::string derivedAddress(const std::vector<std::pair<std::string, uint32_t>>& path, const std::string& baseAddr)
    {
      if (path.empty())
        return baseAddr;
      std::string offset = "ptr null";
      for (auto& [name, index] : path)
        offset = "ptr getelementptr (" + name + ", " + offset + ", i32 0, i32 " + std::to_string(index) + ")";
      std::string off = newTemp(), neg = newTemp(), r = newTemp();
      fn += "  " + off + " = ptrtoint " + offset + " to i64\n";
      fn += "  " + neg + " = sub i64 0, " + off + "\n";
      fn += "  " + r + " = getelementptr i8, ptr " + baseAddr + ", i64 " + neg + "\n";
      return r;
    }

    // The type of the object a conditional's operand x names when the conditional, of type t, can
    // be that object (C++'s lvalue conditional): x an lvalue or a reference of type t or, for a
    // class, of a class derived from it (whose t part it is). Null when x converts to t, so that
    // the conditional is a new value (`p ? p->name : Mod::CORE_MOD`, a String and a `const char*`).
    const Ty* conditionalObject(Expr* x, const Ty* t, Node* at)
    {
      if (!x || !x->ty || !(isLvalueExpression(x) || x->ty->isReference()))
        return nullptr;
      const Ty* xt = withCurrentArgs(types.unqualified(types.stripRef(x->ty)));
      const Ty* want = withCurrentArgs(types.unqualified(t));
      if (!xt || !want || types.isDependent(xt))
        return nullptr;
      std::vector<std::pair<std::string, uint32_t>> path;
      if (isClass(xt) && isClass(want))
        return basePath(xt, want, path, at) ? xt : nullptr;
      return xt == want ? xt : nullptr;
    }

    // The address of the object the operand x names (of type xt, conditionalObject's), as an
    // object of t: its t part for a class derived from t.
    std::string conditionalAddress(Expr* x, const Ty* xt, const Ty* t, Node* at)
    {
      std::string addr = lvalue(x);
      std::string part = addr;
      if (isClass(xt) && canonicalClass(xt, at) != canonicalClass(t, at) && !baseAddress(xt, t, addr, part, at))
        unsupported(at, "a conditional operand of a class derived from the result's, without the base");
      return part;
    }

    bool baseAddress(const Ty* derived, const Ty* base, const std::string& addr, std::string& out, Node* at)
    {
      if (canonicalClass(derived, at) == canonicalClass(base, at))
      {
        out = addr;
        return true;
      }
      StructInfo& info = structFor(derived, at);
      std::string name = info.name;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      for (auto& [baseDef, index] : bases)
      {
        std::string sub = gep(name, addr, index);
        if (baseAddress(fieldTypes[index], base, sub, out, at))
          return true;
      }
      return false;
    }

    // An arithmetic operand's type after the integral promotions.
    const Ty* promoted(const Ty* t)
    {
      t = types.unqualified(types.stripRef(t));
      if (t->kind == TyKind::Enum)
        return enumUnderlying(t->decl);
      if (t->kind == TyKind::Bool || t->kind == TyKind::Char || (t->kind == TyKind::Int && t->bits < 32))
        return types.intType(32);
      return t;
    }

    const Ty* commonArithmetic(const Ty* a, const Ty* b)
    {
      a = promoted(a);
      b = promoted(b);
      if (a->kind == TyKind::Float || b->kind == TyKind::Float)
        return types.floatType(std::max<uint8_t>(a->kind == TyKind::Float ? a->bits : 0, b->kind == TyKind::Float ? b->bits : 0));
      if (a->kind != TyKind::Int || b->kind != TyKind::Int)
        return a->kind == TyKind::Int ? a : b;
      if (a->bits != b->bits)
        return a->bits > b->bits ? a : b;
      return types.intType(a->bits, a->isUnsigned || b->isUnsigned);
    }

    Value load(const std::string& addr, const Ty* ty, Node* at)
    {
      ty = types.stripRef(ty);
      if (ty->kind == TyKind::Array)
      {
        std::string r = newTemp();
        fn += "  " + r + " = getelementptr inbounds " + llType(ty, at) + ", ptr " + addr + ", i64 0, i64 0\n";
        return {r, types.pointer(ty->inner)};
      }
      if (ty->kind == TyKind::Function)
        return {addr, types.pointer(ty)}; // a function (through a reference, `F&& f`): its address, nothing to load
      if (isAggregate(ty))
        return {addr, types.unqualified(ty)};
      std::string r = newTemp();
      fn += "  " + r + " = load " + llType(ty, at) + ", ptr " + addr + "\n";
      return {r, types.unqualified(ty)};
    }

    void store(Value v, const std::string& addr, const Ty* ty, Node* at)
    {
      ty = types.stripRef(ty);
      if (isClass(ty))
        unsupported(at, "storing a class value directly");
      if (ty->kind == TyKind::Array)
      {
        // A scalar stored into an array: zero-init the array, then store the scalar
        // converted to the element type in element 0 (C's `T a[N] = {value}` semantics).
        storeZero(ty, addr, at);
        auto isNumeric = [](TyKind k) { return k == TyKind::Int || k == TyKind::Bool || k == TyKind::Char || k == TyKind::Float; };
        if (v.ty && ty->inner && isNumeric(v.ty->kind) && isNumeric(ty->inner->kind))
        {
          Value elem = convert(v, ty->inner, at);
          std::string elemAddr = newTemp();
          fn += "  " + elemAddr + " = getelementptr inbounds " + llType(ty, at) + ", ptr " + addr + ", i64 0, i64 0\n";
          fn += "  store " + llType(ty->inner, at) + " " + elem.text + ", ptr " + elemAddr + "\n";
        }
        return;
      }
      Value c = convert(v, ty, at);
      fn += "  store " + llType(ty, at) + " " + c.text + ", ptr " + addr + "\n";
    }

    std::string thisPointer(Node* at)
    {
      auto it = locals.find(nullptr);
      if (it == locals.end())
        unsupported(at, "a member used outside a method");
      std::string r = newTemp();
      fn += "  " + r + " = load ptr, ptr " + it->second.addr + "\n";
      return r;
    }

    std::string operatorName(P op)
    {
      return op == P::LBracket ? "operator[]" : op == P::LParen ? "operator()" : "operator" + std::string(punctuatorSpelling(op));
    }

    // The address of the object a call's result names: a returned reference's referent, a class
    // result's object (its slot), and a scalar returned by value (a prvalue) in a temporary of its
    // own. Never the scalar itself: Targeter's `T* operator*() const` bound to Map's
    // `operator[](const K&)` passed the rail pointer as the key's address.
    std::string resultAddress(const Value& v, Node* at)
    {
      if (isClass(v.ty) || isLambda(v.ty) || (v.ty && v.ty->isReference()))
        return v.text;
      std::string slot = newObjectSlot(v.ty, at);
      fn += "  store " + llType(v.ty, at) + " " + v.text + ", ptr " + slot + "\n";
      return slot;
    }

    std::string lvalue(Expr* e)
    {
      if (++recursionDepth > 128)
      {
        recursionDepth = 0;
        fail(e, "expression nested too deeply");
      }
      struct DepthGuard { int& d; ~DepthGuard() { --d; } } dg{recursionDepth};
      switch (e->kind)
      {
        case NodeKind::NameExpr:
        {
          auto* n = static_cast<NameExpr*>(e);
          Decl* d = n->resolved;
          if (!d)
            unsupported(e, "an unresolved name (" + sym(n->name.last().symbol) + ")");
          if (d->kind == NodeKind::VariableDecl)
            if (std::string bound; bindingAddress(static_cast<VariableDecl*>(d), e, bound))
              return bound;
          auto it = locals.find(d);
          if (it != locals.end())
          {
            if (it->second.isRef)
            {
              std::string r = newTemp();
              fn += "  " + r + " = load ptr, ptr " + it->second.addr + "\n";
              return r;
            }
            return it->second.addr;
          }
          if (d->kind == NodeKind::VariableDecl)
          {
            if (Decl* cls = global.owningClass(d); cls && !(d->flags & DF_Static))
            {
              if (!currentClassTy)
                unsupported(e, "a member used outside a method");
              return memberObject(currentClassTy, d, thisPointer(e), e); // `r = v` with r a reference member
            }
            return globalAddress(static_cast<VariableDecl*>(d), e, qualifierClass(e, e));
          }
          if (d->kind == NodeKind::EnumeratorDecl)
            break; // an enumerator bound to a reference: its value goes to a temporary (below)
          if (d->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(d)->isType)
            break; // a constant template parameter bound to a reference (`take(K)`): the same
          if (d->kind == NodeKind::FunctionDecl && !(global.owningClass(d) && !(d->flags & DF_Static)))
          {
            // A function bound to a reference (`call(add, 2, 3)` with `F&& f`): the function itself,
            // whose address the reference holds, as `&add` gives it.
            auto* f = static_cast<FunctionDecl*>(d);
            if (!f->templateParams.empty() || global.isTemplateContext(f))
              unsupported(e, "a function template bound to a reference (" + sym(n->name.last().symbol) + ")");
            return useFunction(f, {}, nullptr);
          }
          unsupported(e, "taking the address of " + sym(n->name.last().symbol));
        }
        case NodeKind::MemberExpr:
        {
          auto* m = static_cast<MemberExpr*>(e);
          Decl* d = m->resolved;
          std::string object;
          const Ty* baseTy;
          if (m->op == P::Arrow)
          {
            Value p = arrowOperand(rvalue(m->base), e);
            if (p.ty->kind != TyKind::Pointer)
              unsupported(e, "-> on something that is not a pointer");
            object = p.text;
            baseTy = p.ty->inner;
          }
          else
          {
            Value o = rvalue(m->base);
            if (!isClass(o.ty))
              unsupported(e, "a member access on a non-class object");
            object = o.text;
            baseTy = o.ty;
          }
          baseTy = types.stripRef(baseTy);
          if (!baseTy || !isClass(baseTy))
            unsupported(e, "a member access on a non-class object");
          if ((!d || d->kind != NodeKind::VariableDecl) && !m->member.empty())
            d = findFieldByName(baseTy, m->member.last().symbol, e);
          if (!d || d->kind != NodeKind::VariableDecl)
            unsupported(e, "a member access that did not resolve to a field");
          if (d->flags & DF_Static)
            return globalAddress(static_cast<VariableDecl*>(d), e, baseTy);
          if (const Ty* qualifier = memberQualifier(m, e))
          {
            // `this->Hook<Robot, TagB>::id`: the field of the instance of its class that the
            // qualifier is or derives from (the first base holding a field of that declaration was
            // another instance of Hook). The field's own class, not the qualifier: `b.Left::id`
            // with id in a virtual base is the one shared subobject's.
            Decl* owner = global.owningClass(d);
            const Ty* instance = owner ? instantiationIn(qualifier, owner, e) : nullptr;
            if (instance && instance != canonicalClass(baseTy, e))
            {
              std::string addr;
              if (!baseAddress(baseTy, instance, object, addr, e))
                unsupported(e, "a member of " + types.toString(instance, &symbols) + ", which is no base of " + types.toString(baseTy, &symbols));
              object = addr;
              baseTy = instance;
            }
          }
          return memberObject(baseTy, d, object, e);
        }
        case NodeKind::UnaryExpr:
        {
          auto* u = static_cast<UnaryExpr*>(e);
          if (!u->isKeyword && P(u->op) == P::Star)
          {
            Value p = rvalue(u->operand);
            if (isClass(p.ty))
            {
              // No operator* of its own: the class's conversion to a pointer (`&*context.undoFiller`).
              const Ty* foundIn = nullptr;
              if (!findMemberFunction(p.ty, operatorName(P::Star), {}, p.ty, e, &foundIn, true))
                if (std::optional<Value> pointer = toPointer(p, e))
                  return pointer->text;
              return resultAddress(operatorCall(p, operatorName(P::Star), {}, e, true), e);
            }
            if (p.ty->kind != TyKind::Pointer)
              unsupported(e, "* on something that is not a pointer");
            return p.text;
          }
          if (!u->isKeyword && !u->postfix && (P(u->op) == P::PlusPlus || P(u->op) == P::MinusMinus))
          {
            rvalue(e);
            return lvalue(u->operand);
          }
          break;
        }
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(e);
          if (i->args.size() != 1)
            unsupported(e, "multi-argument indexing");
          Value base = withCheckedConst(rvalue(i->base), i->base);
          if (ClassDecl* c = typedUnionOf(base.ty, e))
            return alternativeLvalue(c, base, i, e, true); // p[Integer]: the alternative, if held
          if (isClass(base.ty))
            return resultAddress(operatorCall(base, operatorName(P::LBracket), i->args, e, true), e);
          Value index = convert(rvalue(i->args[0]), types.intType(64), e);
          if (base.ty->kind != TyKind::Pointer)
            unsupported(e, "indexing a non-pointer");
          std::string r = newTemp();
          fn += "  " + r + " = getelementptr inbounds " + llType(base.ty->inner, e) + ", ptr " + base.text + ", i64 " + index.text + "\n";
          return r;
        }
        case NodeKind::ParenExpr: return lvalue(static_cast<ParenExpr*>(e)->inner);
        case NodeKind::CastExpr:
        {
          // `(T&&)value` in forward, `(Base&)derived`: a cast to a reference names the operand
          // itself; a copy in a temporary would be a dangling reference once returned.
          auto* c = static_cast<CastExpr*>(e);
          if (e->ty && e->ty->isReference())
            return lvalue(c->operand);
          break;
        }
        case NodeKind::NamedCastExpr:
        {
          auto* c = static_cast<NamedCastExpr*>(e);
          // A reference to the operand's own type, or a reinterpret_cast or const_cast: the operand's
          // address. Another class (a base or a derived one, or one the operand converts to with
          // `operator TrainStop&()`) is the cast's value, as explicitConvert makes it.
          const Ty* from = c->operand && c->operand->ty ? types.unqualified(types.stripRef(c->operand->ty)) : nullptr;
          const Ty* to = e->ty ? types.unqualified(types.stripRef(e->ty)) : nullptr;
          if (e->ty && e->ty->isReference() && c->castKind != Kw::dynamic_cast_ &&
              (c->castKind == Kw::reinterpret_cast_ || c->castKind == Kw::const_cast_ || !from || !to || !isClass(from) || !isClass(to) ||
               classKey(from) == classKey(to)))
            return lvalue(c->operand);
          break;
        }
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          if (!b->isKeyword && (P(b->op) == P::DotStar || P(b->op) == P::ArrowStar))
            return memberPointerAddress(b);
          if (!b->isKeyword && isAssignment(P(b->op)))
          {
            Value v = rvalue(e);
            if (isClass(v.ty))
              return v.text;
            return lvalue(b->lhs);
          }
          if (!b->isKeyword && P(b->op) == P::Comma)
          {
            rvalue(b->lhs);
            return lvalue(b->rhs);
          }
          break;
        }
        case NodeKind::CallExpr: return resultAddress(callValue(static_cast<CallExpr*>(e), ""), e);
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          const Ty* t = e->ty ? types.stripRef(e->ty) : nullptr;
          const Ty* thenObject = c->then && t ? conditionalObject(c->then, t, e) : nullptr;
          const Ty* elseObject = thenObject ? conditionalObject(c->else_, t, e) : nullptr;
          if (thenObject && elseObject)
          {
            // `first ? a : b` with two lvalues is an lvalue: the address of the chosen one.
            Value cond = convert(rvalue(c->cond), types.boolType(), e);
            std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
            fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
            size_t temps = statementTemps.size();
            startBlock(tL);
            std::string a = conditionalAddress(c->then, thenObject, t, e);
            destroyTempsFrom(temps);
            std::string aEnd = currentBlock;
            emitBranch(jL);
            startBlock(eL);
            std::string b = conditionalAddress(c->else_, elseObject, t, e);
            destroyTempsFrom(temps);
            std::string bEnd = currentBlock;
            emitBranch(jL);
            startBlock(jL);
            std::string r = newTemp();
            fn += "  " + r + " = phi ptr [ " + a + ", %" + aEnd + " ], [ " + b + ", %" + bEnd + " ]\n";
            return r;
          }
          break;
        }
        default:
          break;
      }
      Value v = rvalue(e);
      if (isClass(v.ty) || isLambda(v.ty))
        return v.text; // an object in memory already (a closure too: `auto&& add = [&](int x) {...};`)
      std::string slot = newObjectSlot(v.ty, e);
      fn += "  store " + llType(v.ty, e) + " " + v.text + ", ptr " + slot + "\n";
      return slot;
    }

    // A global built at startup (a class with a constructor, or an initializer that is not a
    // constant): a function constructing it, run from llvm.global_ctors in definition order (see
    // ctorsText); destructors at exit do not run yet.
    // init: the global's initializer (for a static member of a class template, its instantiation's
    // copy), or null.
    // perThread: a thread_local global, built by each thread at its first use, not at startup: the
    // function checks a thread_local guard, and globalFor calls it before every use (C++'s
    // thread_local wrapper). Destroyed at no thread's exit yet.
    void emitGlobalConstructor(VariableDecl* v, const std::string& name, const Ty* t, Expr* init, Node* at, bool perThread = false,
                               VariableDecl* definition = nullptr)
    {
      FunctionState saved = saveState();
      std::string savedOverride = funcletOverride;
      FunctionDecl* savedFunction = currentFunction;
      ParsedFile* savedFile = currentFile;
      bool savedShared = emittingShared;
      funcletOverride.clear();
      resetFunctionState();
      currentFunction = nullptr;
      VariableDecl* def = definition ? definition : variableDefinition(v);
      currentFile = def->file;
      // Names in the initializer are typed from the declaration it belongs to (a member alias of
      // the class: `Limits<IDType::IndexType>::max()`), not from the root scope.
      struct InitializerRestore { Decl*& d; Decl* saved; ~InitializerRestore() { d = saved; } } initializerRestore{initializerOf, initializerOf};
      initializerOf = def;
      emittingShared = false;
      std::string fnName = std::string(perThread ? "@\"kpp.tlsinit." : "@\"kpp.init.") + name.substr(2, name.size() - 3) + "\""; // per instantiation for class template members
      std::string guard = name.substr(0, name.size() - 1) + ".tlsguard\"";
      // In the module its name hashes to, whoever uses the global first (the same in every run),
      // and its errors are its own, not of the function that happened to use it first.
      struct ModuleRestore { unsigned& m; unsigned saved; ~ModuleRestore() { m = saved; } } moduleRestore{currentModule, currentModule};
      currentModule = moduleCount <= 1 ? 0 : unsigned(std::hash<std::string>()(fnName) % moduleCount);
      // What it adds (its static InitializerList arrays) belongs to it, not to its first user.
      struct OwnerRestore { std::string& owner; std::string saved; ~OwnerRestore() { owner = saved; } } ownerRestore{owner, owner};
      owner = fnName;
      struct NameRestore
      {
        std::string& name;
        std::string& requestedBy;
        std::string savedName, savedRequestedBy;
        ~NameRestore() { name = savedName, requestedBy = savedRequestedBy; }
      } nameRestore{currentFunctionName, currentRequestedBy, currentFunctionName, currentRequestedBy};
      currentFunctionName = fnName;
      currentRequestedBy.clear();
      try
      {
        if (perThread)
        {
          {
            auto lock = sharedLock();
            if (declaredNames.claim(guard))
              decls += "$" + guard.substr(1) + " = comdat any\n" + guard + " = linkonce_odr thread_local global i1 false, comdat\n";
          }
          fn += "  %tls.built = load i1, ptr " + guard + "\n  br i1 %tls.built, label %tls.done, label %tls.build\ntls.build:\n";
          fn += "  store i1 true, ptr " + guard + "\n";
        }
        pushScope();
        if (t->isReference() && init)
        {
          // The address of what the reference is bound to.
          std::string target = isClass(types.stripRef(init->ty)) ? rvalue(init).text : lvalue(init);
          fn += "  store ptr " + target + ", ptr " + name + "\n";
        }
        else if (init)
          initializeFrom(name, t, init, at);
        else if (isClass(t))
          defaultConstruct(name, t, at);
        else if (t->kind == TyKind::Array)
          constructArray(name, t, at); // `static Style styles[3];`
        destroyTemps();
        popScope();
        if (perThread)
          fn += "  br label %tls.done\ntls.done:\n";
        fn += "  ret void\n";
        addDefinition(fnName, functionText("define void " + fnName + "() {\nentry:\n"), true); // the global's, not its first user's
        auto lock = sharedLock();
        if (perThread)
          threadLocalInits[name] = fnName;
        else
          startups.push_back(startupAt ? Shared::Startup{startupAt->file, startupAt->token, def->token, fnName}
                                       : Shared::Startup{def->file ? def->file->index : 0, def->token, 0, fnName});
      }
      catch (Unsupported&)
      {
        restoreState(saved);
        funcletOverride = savedOverride;
        currentFunction = savedFunction;
        currentFile = savedFile;
        emittingShared = savedShared;
        throw;
      }
      restoreState(saved);
      funcletOverride = savedOverride;
      currentFunction = savedFunction;
      currentFile = savedFile;
      emittingShared = savedShared;
    }

    // The llvm.global_ctors array, all of it in module 0: the globals built at startup in
    // definition order within a file (design.md section 2: a global's initializer may read the ones
    // defined before it in its file), the files in the driver's order, the library first. Not by
    // module: which module a startup function lands in says nothing about the order.
    std::string ctorsText(unsigned k)
    {
      if (k != 0 || startups.empty())
        return "";
      std::vector<Shared::Startup> order = startups;
      std::sort(order.begin(), order.end(), [](const Shared::Startup& a, const Shared::Startup& b) {
        return std::tie(a.file, a.token, a.member, a.function) < std::tie(b.file, b.token, b.member, b.function);
      });
      std::string s = "@llvm.global_ctors = appending global [" + std::to_string(order.size()) + " x { i32, ptr, ptr }] [";
      for (size_t i = 0; i < order.size(); ++i)
        s += std::string(i ? ", " : "") + "{ i32, ptr, ptr } { i32 65535, ptr " + order[i].function + ", ptr null }";
      return s + "]\n";
    }

    // The declaration carrying a global's initializer: the out-of-line definition of a static data
    // member (`double Gui::scale = 1.0;`), or the declaration itself.
    VariableDecl* variableDefinition(VariableDecl* v)
    {
      Decl* d = global.definitionOf(v);
      return d && d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : v;
    }

    // The definition of static member v in the class instance classTy: the explicit specialisation
    // written for that instance (`template<> const uint32_t ElectricNetworkStatistics::smoothingTicks
    // = 0;`), or the template's definition; v's own definition without an instance.
    VariableDecl* variableDefinition(VariableDecl* v, const Ty* classTy)
    {
      if (!classTy)
        return variableDefinition(v);
      Decl* d = global.definitionOf(v, global.classArgsOf(classTy));
      return d && d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : v;
    }

    // The type the qualifier of a qualified name names, under the current arguments: `Counted<int>`
    // of `Counted<int>::instances`, the class `decltype(check<T>(nullptr))` names of
    // `decltype(check<T>(nullptr))::value`; null for an unqualified name.
    const Ty* qualifierOf(NameExpr* n)
    {
      TemplateArgs none;
      if (n->name.parts.size() == 1 && n->name.typeQualifier)
        return global.typeOfAstIn(n->name.typeQualifier, typingContext(), currentArgs ? *currentArgs : none);
      if (n->name.parts.size() <= 1)
        return nullptr;
      NamedType tmp{};
      tmp.kind = NodeKind::NamedType;
      tmp.token = n->token;
      tmp.name = n->name;
      tmp.name.parts.count -= 1;
      const Ty* q = global.typeOfAstIn(&tmp, typingContext(), currentArgs ? *currentArgs : none);
      if (!q || !q->isKnown() || types.isDependent(q))
        if (const Ty* bound = boundParameter(tmp.name))
          return bound;
      return q;
    }

    // A template parameter written as a qualifier (`T::loadExplicit(input)` in a generic lambda
    // `[&]<class T>(T) { ... }`): its argument in the instance being emitted, by name. The lambda's
    // own parameters are in no scope that a re-typing from the enclosing function looks in.
    const Ty* boundParameter(const QualifiedName& q)
    {
      if (!currentArgs || q.parts.size() != 1 || q.parts[0].hasTemplateArgs || q.parts[0].symbol == UINT32_MAX)
        return nullptr;
      for (const auto& [param, type] : currentArgs->pairs)
        if (param && param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(param)->name == q.parts[0].symbol &&
            type && type->isKnown() && !types.isDependent(type))
          return type;
      return nullptr;
    }

    // The class a member's name is qualified with (`Hook<Robot, TagB>` of `r.Hook<Robot, TagB>::getId()`),
    // canonical; null for an unqualified member.
    // The implicit assignment of class target (objectTy itself or a base of it), member by member,
    // into the object of class objectTy at addr from arg (`this->Base::operator=(other)` and
    // `super::operator=(other)` of a class declaring none); the target object as an lvalue.
    Value implicitAssignment(const std::string& addr, const Ty* objectTy, const Ty* target, Expr* arg, Node* at)
    {
      std::string dst = addr;
      if (canonicalClass(target, at) != canonicalClass(objectTy, at) && !baseAddress(objectTy, target, addr, dst, at))
        unsupported(at, "operator= of " + types.toString(target, &symbols) + ", which is no base of " + types.toString(objectTy, &symbols));
      Value src = rvalue(arg);
      const Ty* srcTy = types.unqualified(types.stripRef(src.ty));
      std::string from = src.text;
      if (!isClass(srcTy))
        unsupported(at, "assigning a value that is no object to " + types.toString(target, &symbols));
      if (canonicalClass(srcTy, at) != canonicalClass(target, at) && !baseAddress(srcTy, target, src.text, from, at))
        unsupported(at, "assigning " + types.toString(srcTy, &symbols) + " to " + types.toString(target, &symbols));
      assignObject(dst, from, target, at, isXvalue(arg));
      return {dst, types.lref(target)};
    }

    const Ty* memberQualifier(MemberExpr* m, Node* at)
    {
      if (m->member.parts.size() < 2)
        return nullptr;
      NamedType written{};
      written.kind = NodeKind::NamedType;
      written.token = m->token;
      written.name = m->member;
      written.name.parts.count -= 1;
      TemplateArgs none;
      const Ty* q = global.typeOfAstIn(&written, typingContext(), currentArgs ? *currentArgs : none);
      return q && isClass(types.stripRef(q)) ? canonicalClass(types.unqualified(types.stripRef(q)), at) : nullptr;
    }

    // `Counted<int>::instances`: the instantiation a qualified static member belongs to (null for
    // an unqualified name or a class that is not a template).
    const Ty* qualifierClass(Node* e, Node* at)
    {
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      const Ty* q = qualifierOf(static_cast<NameExpr*>(e));
      q = q ? types.unqualified(types.stripRef(q)) : nullptr;
      return q && isClass(q) && !q->args.empty() ? canonicalClass(q, at) : nullptr;
    }

    // The instantiation a static member of a class template is used with: the qualifier written,
    // or the class of the method being emitted, or the base of either that is an instantiation of
    // the member's class (`MyLimits<int>::digits` with digits declared in the base
    // IntegerLimits<int, true, 32>).
    const Ty* staticMemberClass(VariableDecl* v, Node* e, Node* at, const Ty* classTy)
    {
      Decl* owner = global.owningClass(v);
      if (!owner || owner->templateParams.empty())
        return nullptr;
      if (classTy)
      {
        // `IsPointerLike<int*>::value` with value declared in IsPointerLike<T*>'s base Yes
        // (Constant<bool, true>): the base's instantiation.
        const Ty* c = canonicalClass(classTy, at);
        if (const Ty* found = instantiationIn(c, owner, at))
          return found;
        return c;
      }
      if (const Ty* q = qualifierType(e, at))
        if (const Ty* found = instantiationIn(q, owner, at))
          return found;
      if (currentClassTy)
        if (const Ty* found = instantiationIn(currentClassTy, owner, at))
          return found;
      if (const Ty* q = qualifierClass(e, at))
        return q;
      return nullptr; // no instantiation in sight (an alias used as the qualifier): one shared object, as for a plain class
    }

    // The class a qualified name's qualifier names (`MyLimits<int>` of `MyLimits<int>::digits`),
    // any class; null for an unqualified name.
    const Ty* qualifierType(Node* e, Node* at)
    {
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      const Ty* q = qualifierOf(static_cast<NameExpr*>(e));
      q = q ? types.unqualified(types.stripRef(q)) : nullptr;
      return q && isClass(q) && !types.isDependent(q) ? canonicalClass(q, at) : nullptr;
    }

    // The instantiation of the class template owner that cls is or derives from; null if none.
    const Ty* instantiationIn(const Ty* cls, Decl* owner, Node* at, int depth = 0)
    {
      if (!cls || !isClass(cls) || depth > 32)
        return nullptr;
      const Ty* c = canonicalClass(cls, at);
      ClassDecl* def = classDef(c->decl, at);
      if (def == classDef(owner, at))
        return c;
      TemplateArgs args = global.classArgsOf(c);
      for (const ClassDecl::Base& b : def->bases)
        if (const Ty* bt = global.typeOfAstIn(b.type, def, args); bt && isClass(bt) && !types.isDependent(bt))
          if (const Ty* found = instantiationIn(bt, owner, at, depth + 1))
            return found;
      return nullptr;
    }

    // `using EmptyWidget::EmptyWidget;` in a class without constructors of its own: the base whose
    // constructors are inherited (its name, as a constructor initializer would spell it); null if none.
    std::optional<QualifiedName> inheritedConstructorBase(ClassDecl* c)
    {
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::UsingNameDecl)
          continue;
        const QualifiedName& q = static_cast<UsingNameDecl*>(m)->name;
        if (q.parts.size() < 2 || q.parts[q.parts.size() - 1].symbol != q.parts[q.parts.size() - 2].symbol)
          continue;
        QualifiedName base = q;
        base.parts.count -= 1;
        return base;
      }
      return std::nullopt;
    }

    // The address of the object a global names: the global itself, or, for a reference global,
    // what it was bound to. classTy: the instantiation, for a static member of a class template.
    std::string globalAddress(VariableDecl* v, Node* at, const Ty* classTy = nullptr)
    {
      classTy = staticMemberClass(v, at, at, classTy);
      std::string g = globalFor(v, at, classTy);
      const Ty* t = classTy ? global.typeOfDeclIn(v, global.classArgsOf(classTy)) : global.typeOfDecl(v);
      if (!t || !t->isReference())
        return g;
      std::string p = newTemp();
      fn += "  " + p + " = load ptr, ptr " + g + "\n";
      return p;
    }

    // --- constant data (design.md, "Compile-time evaluation: one interpreter", step 4b) ---

    static bool zeroText(const std::string& s) { return s == "0" || s == "false" || s == "null" || s == "zeroinitializer" || s == "0x0000000000000000"; }

    // The data members of class c as the interpreter's Object parts list them (after the bases).
    static std::vector<Decl*> valueMembers(ClassDecl* c)
    {
      std::vector<Decl*> members;
      for (Decl* m : c->members)
        if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
          members.push_back(m);
      return members;
    }

    // The LLVM constant value v of type t is (a global's initializer, ConstValue's contract in
    // consteval.hpp), or nothing when a part is none the IR can hold here: a polymorphic class, a
    // union, a class with an anonymous member or a reference member, a member pointer, a part of a
    // kind its type cannot take. Then the global is built at startup, as before.
    std::optional<std::string> constantText(const ConstValue& v, const Ty* t, Node* at, int depth = 0)
    {
      if (!t || depth > 64)
        return std::nullopt;
      t = types.unqualified(t);
      bool unset = v.kind == ConstValue::Kind::Unset; // static storage starts zero
      switch (t->kind)
      {
        case TyKind::Bool:
          if (unset)
            return std::string("false");
          return v.kind == ConstValue::Kind::Int ? std::optional<std::string>(v.i ? "true" : "false") : std::nullopt;
        case TyKind::Char: case TyKind::Int: case TyKind::Enum:
          if (unset)
            return std::string("0");
          return v.kind == ConstValue::Kind::Int ? std::optional<std::string>(std::to_string(v.i)) : std::nullopt;
        case TyKind::Float:
          if (unset)
            return floatConstant(0.0, t->bits);
          if (v.kind == ConstValue::Kind::Float)
            return floatConstant(v.f, t->bits);
          return v.kind == ConstValue::Kind::Int ? std::optional<std::string>(floatConstant(double(v.i), t->bits)) : std::nullopt;
        case TyKind::Pointer:
          // A null pointer only when no initializer set it: the interpreter's Int 0 of a pointer type
          // is also what a function pointer template argument (`FOLD = fold` with fold = &add) comes
          // as, so such a part is built at startup. (`T* p = nullptr;` itself is a null constant
          // before constant data is asked for.)
          if (unset)
            return std::string("null");
          if (v.kind == ConstValue::Kind::Pointer)
            return evaluatedArrayElement(v, t, at, depth);
          if (v.kind == ConstValue::Kind::Address && v.target)
            return addressConstant(*v.target, at);
          return std::nullopt;
        case TyKind::Array:
        {
          if (unset)
            return std::string("zeroinitializer");
          if (v.kind != ConstValue::Kind::Object || !v.parts || v.parts->size() != t->count)
            return std::nullopt;
          std::string element = llType(t->inner, at), out = "[";
          bool allZero = true;
          for (size_t i = 0; i < v.parts->size(); ++i)
          {
            std::optional<std::string> e = constantText((*v.parts)[i], t->inner, at, depth + 1);
            if (!e)
              return std::nullopt;
            allZero = allZero && zeroText(*e);
            out += (i ? ", " : "") + element + " " + *e;
          }
          return allZero ? std::string("zeroinitializer") : out + "]";
        }
        case TyKind::Class:
          return classConstant(v, t, at, depth);
        default:
          return std::nullopt;
      }
    }

    std::optional<std::string> classConstant(const ConstValue& v, const Ty* t, Node* at, int depth)
    {
      StructInfo& info = structFor(t, at);
      ClassDecl* c = classDef(t->decl, at);
      if (info.isUnion || c->isTypedUnion || info.polymorphic || !info.anonymous.empty())
        return std::nullopt;
      if (v.kind == ConstValue::Kind::Unset)
        return std::string("zeroinitializer");
      std::vector<Decl*> members = valueMembers(c);
      if (v.kind != ConstValue::Kind::Object || !v.parts || v.parts->size() != info.bases.size() + members.size())
        return std::nullopt;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      std::vector<bool> zeroSize = info.zeroSize;
      std::vector<std::string> fields(fieldTypes.size());
      std::vector<uint64_t> units(fieldTypes.size(), 0); // bit fields' storage units
      std::vector<bool> unit(fieldTypes.size(), false);
      for (size_t k = 0; k < info.bases.size(); ++k)
      {
        uint32_t index = info.bases[k].second;
        if (zeroSize[index])
          continue; // an empty base: no storage of its own
        std::optional<std::string> b = constantText((*v.parts)[k], fieldTypes[index], at, depth + 1);
        if (!b)
          return std::nullopt;
        fields[index] = *b;
      }
      for (size_t k = 0; k < members.size(); ++k)
      {
        const ConstValue& part = (*v.parts)[info.bases.size() + k];
        auto it = info.fieldIndex.find(members[k]);
        if (it == info.fieldIndex.end())
          return std::nullopt;
        uint32_t index = it->second;
        if (auto bits = info.bitfields.find(members[k]); bits != info.bitfields.end())
        {
          if (part.kind != ConstValue::Kind::Int && part.kind != ConstValue::Kind::Unset)
            return std::nullopt;
          auto [shift, width] = bits->second;
          uint64_t mask = width >= 64 ? ~0ull : (1ull << width) - 1;
          units[index] |= ((part.kind == ConstValue::Kind::Int ? uint64_t(part.i) : 0) & mask) << shift;
          unit[index] = true;
          continue;
        }
        if (fieldTypes[index]->isReference())
          return std::nullopt; // the interpreter holds the referred value there, not an address
        std::optional<std::string> f = constantText(part, fieldTypes[index], at, depth + 1);
        if (!f)
          return std::nullopt;
        fields[index] = *f;
      }
      std::string out = info.packed ? "<{ " : "{ ";
      bool allZero = true;
      for (size_t i = 0; i < fieldTypes.size(); ++i)
      {
        std::string value = unit[i] ? std::to_string(int64_t(units[i])) : fields[i].empty() ? "zeroinitializer" : fields[i]; // padding and empty bases: zero
        allZero = allZero && zeroText(value);
        out += (i ? ", " : "") + (zeroSize[i] ? std::string("[0 x i8]") : llType(fieldTypes[i], at)) + " " + value;
      }
      if (info.empty)
        out += std::string(fieldTypes.empty() ? "" : ", ") + "i8 0"; // its byte of its own
      if (info.tailPadding)
        out += ", [" + std::to_string(info.tailPadding) + " x i8] zeroinitializer";
      out += info.packed ? " }>" : " }";
      return allZero ? std::string("zeroinitializer") : out;
    }

    // A Pointer value: into an array the evaluation made (a string literal's chars, a brace list's
    // elements), which becomes a private constant (one per contents), and the element's address.
    std::optional<std::string> evaluatedArrayElement(const ConstValue& v, const Ty* pointerTy, Node* at, int depth)
    {
      const Ty* element = types.unqualified(pointerTy->inner);
      if (!v.parts || element->kind == TyKind::Void || !element->isKnown())
        return std::nullopt;
      const Ty* arrayTy = types.array(element, uint32_t(v.parts->size()));
      std::optional<std::string> body = constantText(ConstValue::object(arrayTy, *v.parts), arrayTy, at, depth + 1);
      if (!body)
        return std::nullopt;
      std::string contents = llType(arrayTy, at) + " " + *body;
      auto [name, isNew] = constantName(emittingShared ? "@.sdata." : "@.data.", contents);
      if (isNew)
        (emittingShared ? decls : moduleDecls[currentModule]) += name + " = private unnamed_addr constant " + contents + "\n";
      return "getelementptr inbounds (" + llType(arrayTy, at) + ", ptr " + name + ", i64 0, i64 " + std::to_string(v.i) + ")";
    }

    // An Address value: a function, or a global (a part of its value along the path).
    std::optional<std::string> addressConstant(const ConstTarget& target, Node* at)
    {
      if (!target.decl)
        return std::nullopt;
      if (target.decl->kind == NodeKind::FunctionDecl)
      {
        auto* f = static_cast<FunctionDecl*>(target.decl);
        if (!target.path.empty() || (global.owningClass(f) && !(f->flags & DF_Static)))
          return std::nullopt; // a member function's address is a member pointer
        TemplateArgs args;
        for (auto& p : target.args)
          args.pairs.push_back(p);
        return useFunction(f, args, target.owner);
      }
      if (target.decl->kind != NodeKind::VariableDecl || (target.decl->parent && target.decl->parent->kind == NodeKind::FunctionDecl))
        return std::nullopt; // a static local is laid out where its function is emitted
      auto* var = static_cast<VariableDecl*>(target.decl);
      std::string name = globalFor(var, at, target.owner);
      if (target.path.empty())
        return name;
      const Ty* type = target.owner ? global.typeOfDeclIn(var, global.classArgsOf(target.owner)) : global.typeOfDecl(var);
      std::string gep = "getelementptr inbounds (" + llType(types.stripRef(type), at) + ", ptr " + name + ", i64 0";
      for (size_t step : target.path)
      {
        type = types.unqualified(types.stripRef(type));
        if (type->kind == TyKind::Array)
        {
          gep += ", i64 " + std::to_string(step);
          type = type->inner;
          continue;
        }
        if (!isClass(type))
          return std::nullopt;
        StructInfo& info = structFor(type, at);
        if (info.isUnion || !info.anonymous.empty())
          return std::nullopt;
        uint32_t index;
        if (step < info.bases.size())
          index = info.bases[step].second;
        else
        {
          std::vector<Decl*> members = valueMembers(classDef(type->decl, at));
          size_t k = step - info.bases.size();
          if (k >= members.size() || !info.fieldIndex.count(members[k]) || info.bitfields.count(members[k]))
            return std::nullopt;
          index = info.fieldIndex[members[k]];
        }
        gep += ", i32 " + std::to_string(index);
        type = info.fieldTypes[index];
      }
      return gep + ")";
    }

    // A class (or an array of them) with a mutable member: no constant memory for a const global.
    bool hasMutableMember(const Ty* t, Node* at, int depth = 0)
    {
      t = types.unqualified(t);
      while (t->kind == TyKind::Array)
        t = types.unqualified(t->inner);
      if (!isClass(t) || depth > 16)
        return false;
      StructInfo& info = structFor(t, at);
      for (Decl* m : info.fieldDecls)
        if (m->flags & DF_Mutable)
          return true;
      for (const Ty* ft : std::vector<const Ty*>(info.fieldTypes))
        if (!ft->isReference() && hasMutableMember(ft, at, depth + 1))
          return true;
      return false;
    }

    // A global's initializer as constant data: the value the interpreter computes for it as type t,
    // lowered (constantText), or nothing (it is built at startup).
    std::optional<std::string> constantData(const Ty* t, Expr* value, VariableDecl* def, const TemplateArgs& args, Node* at)
    {
      bool layoutFailed = false;
      std::optional<ConstValue> v;
      {
        ConstContextScope context(*this, def);
        v = global.evaluateInitializer(t, value, def->file, def, args, foldHooks(at, layoutFailed));
      }
      if (!v)
        return std::nullopt;
      return constantText(*v, t, at);
    }

    std::map<std::pair<Decl*, const Ty*>, std::string> globalCache; // globalFor's answers this emitter had
    std::string globalFor(VariableDecl* v, Node* at, const Ty* classTy = nullptr)
    {
      // A type name read as a value (`(HANDLE) -1` taken for `HANDLE - 1`, `(LPWSTR)&msg` for
      // `LPWSTR & msg`): no variable to define, never a global of that name.
      if (v->flags & DF_Typedef)
        unsupported(at, "the type " + (v->name.empty() ? std::string("?") : sym(v->name.last().symbol)) + " used as a value");
      if (!classTy && (v->flags & DF_Extern))
        v = variableDefinition(v); // `extern T x;` names the variable its namespace defines, if it does
      if (auto mine = globalCache.find({v, classTy}); mine != globalCache.end())
      {
        buildThreadLocal(mine->second);
        return mine->second;
      }
      std::string name;
      {
        auto lock = sharedLock(); // a global is defined (and its startup function emitted) once
        std::pair<Decl*, const Ty*> key{v, classTy};
        if (auto failed = sh.failedGlobals.find(key); failed != sh.failedGlobals.end())
          fail(at, failed->second);
        size_t errorsBefore = errors.size();
        // Defined by whichever function uses it first, which depends on the threads' timing: the
        // temporaries it numbers (building a constant initializer) are not that function's, or its
        // IR would differ from run to run.
        uint32_t tempBefore = temp;
        size_t fnBefore = fn.size(), allocasBefore = allocas.size();
        try
        {
          name = globalForOnce(v, at, classTy);
          if (fn.size() == fnBefore && allocas.size() == allocasBefore)
            temp = tempBefore;
        }
        catch (Unsupported&)
        {
          // Every use, the first one like the others, reports the global unusable and why (the
          // same lines whichever use comes first).
          std::string why = "a use of the global " + qualifiedName(v) + ", whose definition failed (" + lastFailure + ")";
          sh.failedGlobals[key] = why;
          errors.resize(errorsBefore);
          fail(at, why);
        }
      }
      globalCache.emplace(std::pair<Decl*, const Ty*>{v, classTy}, name);
      buildThreadLocal(name);
      return name;
    }

    // A thread_local global built per thread: its function, called before the use being emitted
    // (it returns at once once this thread has built it).
    void buildThreadLocal(const std::string& name)
    {
      std::string init;
      {
        auto lock = sharedLock();
        if (auto it = threadLocalInits.find(name); it != threadLocalInits.end())
          init = it->second;
      }
      if (!init.empty() && !currentFunctionName.empty() && currentFunctionName != init)
        fn += "  call void " + init + "()\n";
    }

    std::string globalForOnce(VariableDecl* v, Node* at, const Ty* classTy)
    {
      TemplateArgs classArgs;
      std::string instanceName;
      if (classTy)
      {
        // A static member of a class template: one global per instantiation, its initializer
        // evaluated under the instantiation's arguments (also an out-of-class definition's own
        // declarations of the class's parameters).
        classArgs = global.definitionArgs(variableDefinition(v, classTy), global.classArgsOf(classTy));
        instanceName = "@\"" + irString(classTy) + "::" + sym(v->name.last().symbol) + "\"";
        auto found = instanceGlobals.find(instanceName);
        if (found != instanceGlobals.end())
          return found->second;
      }
      else
      {
        auto it = globals.find(v);
        if (it != globals.end())
          return it->second;
      }
      // The global's initializer under its own arguments and class, not those of the function that
      // happened to use it first.
      ArgsScope argsScope(*this, classTy ? &classArgs : nullptr);
      struct ClassRestore { const Ty*& c; const Ty* saved; ~ClassRestore() { c = saved; } } classRestore{currentClassTy, currentClassTy};
      currentClassTy = classTy;
      struct SharedScope
      {
        bool& flag;
        bool saved;
        SharedScope(bool& f) : flag(f), saved(f) { flag = true; }
        ~SharedScope() { flag = saved; }
      } sharedScope(emittingShared); // a global's initializer belongs to every module
      const Ty* t = classTy ? global.typeOfDeclIn(v, classArgs) : global.typeOfDecl(v);
      if (!t || !t->isKnown())
        unsupported(at, "a global of unknown type");
      std::string name = classTy ? instanceName : "@\"" + withSpecialisationArgs(qualifiedName(v), v) + "\"";
      if (classTy)
        instanceGlobals[instanceName] = name;
      else
        globals[v] = name;
      if (v->builtin == BUILTIN_EnumValues && v->parent)
      {
        // E::values: a constant array of the enumerators in declaration order.
        auto* e = static_cast<EnumDecl*>(v->parent);
        std::string element = llType(enumUnderlying(e), at), elements;
        for (Decl* d : e->enumerators)
          if (d->kind == NodeKind::EnumeratorDecl)
            elements += std::string(elements.empty() ? "" : ", ") + element + " " + std::to_string(enumeratorValue(static_cast<EnumeratorDecl*>(d)));
        decls += name + " = internal constant " + llType(t, at) + " [" + elements + "]\n";
        return name;
      }
      std::string init = "zeroinitializer";
      bool constructed = false;
      const Ty* st = types.stripRef(t);
      VariableDecl* def = variableDefinition(v, classTy); // `static double scale;` in the class, `double Gui::scale = 1.0;` outside
      if (def == v && (v->flags & DF_Extern) && !v->init && !classTy && !t->isReference())
      {
        // `extern int counter;` that no Kpp source defines: a C library's variable (its header
        // declares it), defined and initialised there. A definition here would be a second one
        // (a duplicate in the link) or, where the library's is not linked, a zero read as its value.
        // Declared once, however many declarations name it.
        if (declaredNames.claim(name))
          decls += name + " = external global " + llType(st, at) + "\n";
        return name;
      }
      // `static constexpr T ValuesPerOne = T(1) << Bits`: the initializer typed for this instantiation.
      Expr* value = classTy ? global.instanceInitializer(def, classArgs) : def->init;
      ParsedFile* saved = currentFile;
      currentFile = def->file;
      // `thread_local IntrusiveList<CrashHookBase> CrashHooks::hooks;`: one per thread (TLS), built by
      // each thread at its first use (emitGlobalConstructor's perThread).
      bool threadLocal = ((v->flags | def->flags) & DF_ThreadLocal) != 0;
      if (t->isReference())
      {
        // `static const Style& defaultWidgetStyle = getStyleStatic();`: a pointer, bound at startup.
        currentFile = saved;
        if (threadLocal)
          unsupported(at, "a thread_local reference");
        decls += "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr global ptr null, comdat\n";
        if (value)
          emitGlobalConstructor(v, name, t, value, at, false, def);
        return name;
      }
      if (value)
      {
        std::optional<ConstValue> c;
        {
          ConstContextScope context(*this, def); // traits in the initializer resolve their names in the declaration's context
          c = foldAs(foldConst(value), st);       // the value the emitted initialization would store
          if (c && !c->isScalar())
            c.reset(); // an object or a pointer: built at startup (constant data comes later)
        }
        if (c && (st->kind == TyKind::Int || st->kind == TyKind::Char || st->kind == TyKind::Enum) && !c->isFloat())
          init = std::to_string(c->i);
        else if (c && st->kind == TyKind::Float)
          init = floatConstant(c->isFloat() ? c->f : double(c->i), st->bits);
        else if (c && st->kind == TyKind::Bool)
          init = c->truthy() ? "true" : "false";
        else if (st->kind == TyKind::Pointer && value->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(value)->keyword == Kw::nullptr_)
          init = "null";
        else if (value->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(value)->literalKind == TokenKind::String &&
                 st->kind == TyKind::Pointer)
          init = stringConstant(static_cast<LiteralExpr*>(value));
        else if (std::optional<std::string> data =
                   isClass(st) || st->kind == TyKind::Array || st->kind == TyKind::Pointer ? constantData(st, value, def, classArgs, at) : std::nullopt)
          init = *data; // constant data: the object as the interpreter computes it
        else
          constructed = true; // built at startup, see emitGlobalConstructor
      }
      else if (needsConstruction(st, at))
        constructed = true;
      currentFile = saved;
      // Defined in every module, merged by the linker (one instance per program). Two entries: the
      // shared text is sorted, and the definition (an `@` line) has to come after the struct types
      // (`%` lines) a constant initializer names; the comdat line (`$`) would sort first.
      decls += "$" + name.substr(1) + " = comdat any\n";
      decls += name + " = linkonce_odr " + (threadLocal ? "thread_local " : "") +
               (t->isConst && !constructed && !hasMutableMember(st, at) ? "constant " : "global ") + llType(st, at) + " " + init + ", comdat" +
               alignSuffix(st, variableAlignment(def, def, at), at) + "\n";
      if (constructed)
        emitGlobalConstructor(v, name, st, value, at, threadLocal, def);
      return name;
    }

    static bool isAssignment(P op)
    {
      switch (op)
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign: case P::PercentAssign:
        case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign: case P::ShrAssign:
          return true;
        default:
          return false;
      }
    }

    static P baseOp(P op)
    {
      switch (op)
      {
        case P::PlusAssign: return P::Plus;
        case P::MinusAssign: return P::Minus;
        case P::StarAssign: return P::Star;
        case P::SlashAssign: return P::Slash;
        case P::PercentAssign: return P::Percent;
        case P::AmpAssign: return P::Amp;
        case P::PipeAssign: return P::Pipe;
        case P::CaretAssign: return P::Caret;
        case P::ShlAssign: return P::Shl;
        case P::ShrAssign: return P::Shr;
        default: return op;
      }
    }

    // Finds the best member with the given name (searching bases, nearest class wins) for the
    // argument types and the object's constness. Returns the class type it was found in.
    FunctionDecl* findMemberFunction(const Ty* classTy, const std::string& name, const std::vector<const Ty*>& argTypes,
                                     const Ty* objectTy, Node* at, const Ty** foundIn, bool templates = false)
    {
      FunctionDecl* found = nullptr;
      std::function<void(const Ty*)> search = [&](const Ty* cls)
      {
        ClassDecl* c = classDef(cls->decl, at);
        std::vector<FunctionDecl*> candidates;
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || (!templates && !m->templateParams.empty()))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          const std::string& q = qualifiedName(f); // ends with "::" + name
          if (q.size() > name.size() + 1 && q.ends_with(name) && q.compare(q.size() - name.size() - 2, 2, "::") == 0)
            candidates.push_back(f);
        }
        if (!candidates.empty())
        {
          const Ty* obj = objectTy ? objectTy : cls;
          found = global.chooseOverload(candidates, argTypes, global.classArgsOf(cls), obj);
          if (found)
          {
            if (foundIn)
              *foundIn = cls;
            return;
          }
        }
        StructInfo& info = structFor(cls, at);
        std::vector<const Ty*> baseTypes;
        for (auto& [baseDef, index] : info.bases)
          baseTypes.push_back(info.fieldTypes[index]);
        for (const Ty* b : baseTypes)
          if (!found)
            search(b);
      };
      search(canonicalClass(classTy, at));
      return found;
    }

    FunctionDecl* findOperator(const Ty* classTy, const std::string& name, size_t argCount, Node* at, const Ty** foundIn)
    {
      std::vector<const Ty*> neutral(argCount, nullptr);
      return findMemberFunction(classTy, name, neutral, nullptr, at, foundIn);
    }

    // Calls an operator member on an object. The result of a reference-returning operator is
    // the referenced address (ty keeps the reference). sret: where a class result is built (the
    // variable `Chunk c = maker(5)` initialises), or empty for a temporary.
    // An object's value with the const the checker gave its expression (`this->byChunk` in a
    // const member function is const even where the field's own type is not), so that an
    // operator chosen on it is the one a const object calls (`T* operator[](int) const`).
    Value withCheckedConst(Value v, Expr* e)
    {
      if (v.ty && !v.ty->isConst && isClass(v.ty) && constObject(e))
        v.ty = types.qualified(v.ty, true, v.ty->isVolatile);
      return v;
    }

    // Whether e names a const object: as the checker types it, or as its path makes it (a data
    // member of a const object is const unless mutable: `this->byChunk` in a const member
    // function, `p->field` with p a pointer to const).
    bool constObject(Expr* e, int depth = 0)
    {
      if (!e || depth > 16)
        return false;
      if (e->ty && types.stripRef(e->ty)->isConst)
        return true;
      if (e->kind == NodeKind::ParenExpr)
        return constObject(static_cast<ParenExpr*>(e)->inner, depth + 1);
      if (e->kind != NodeKind::MemberExpr)
        return false;
      auto* m = static_cast<MemberExpr*>(e);
      if (!m->resolved || m->resolved->kind != NodeKind::VariableDecl || (m->resolved->flags & (DF_Mutable | DF_Static)))
        return false;
      // A reference member names the object it refers to, const only as that object's type is (the
      // checker's type, above): MapGenerator's `this->surface.hiddenTiles[position] = tile` in a
      // const member function, with `Surface& surface`, called the const operator[] and assigned
      // to its copy.
      if (const Ty* ft = declType(m->resolved); ft && ft->isReference())
        return false;
      if (m->op != P::Arrow)
        return constObject(m->base, depth + 1);
      if (m->base && m->base->kind == NodeKind::ThisExpr && currentFunction && (currentFunction->flags & DF_Const))
        return true;
      const Ty* bt = m->base && m->base->ty ? types.stripRef(m->base->ty) : nullptr;
      return bt && bt->kind == TyKind::Pointer && bt->inner && bt->inner->isConst;
    }

    Value operatorCall(Value object, const std::string& name, Span<Expr*> args, Node* at, bool required, const std::string& sret = "")
    {
      const Ty* foundIn = nullptr;
      FunctionDecl* f = findMemberFunction(object.ty, name, argTypesOf(args), object.ty, at, &foundIn, true);
      if (!f)
      {
        if (required)
          unsupported(at, qualifiedName(object.ty->decl) + " has no " + name);
        return {"", nullptr};
      }
      std::string thisArg = object.text;
      if (canonicalClass(object.ty, at) != foundIn)
      {
        std::string addr;
        if (baseAddress(object.ty, foundIn, object.text, addr, at))
          thisArg = addr;
      }
      TemplateArgs fnArgs = global.classArgsOf(foundIn);
      if (!f->templateParams.empty())
        global.deduceArgs(f, argTypesOf(args), fnArgs); // `template<typename U> bool operator==(const U*) const`
      std::string indirect;
      if (isClass(foundIn) && f->templateParams.empty() && structFor(foundIn, at).polymorphic)
        if (int slot = slotOf(foundIn, f, at); slot >= 0)
          indirect = loadSlot(thisArg, slot); // `*a == *b` on images: the dynamic type's operator==, through the declaring class's vtable pointer
      return callFunction(f, fnArgs, foundIn, thisArg, sret, args, at, indirect);
    }

    Value arithmetic(P op, Value l, Value r, Node* at)
    {
      bool lp = l.ty->kind == TyKind::Pointer, rp = r.ty->kind == TyKind::Pointer;
      std::string res = newTemp();
      if (op == P::Plus || op == P::Minus)
      {
        if (lp && rp && op == P::Minus)
        {
          std::string a = newTemp(), b = newTemp(), d = newTemp();
          fn += "  " + a + " = ptrtoint ptr " + l.text + " to i64\n";
          fn += "  " + b + " = ptrtoint ptr " + r.text + " to i64\n";
          fn += "  " + d + " = sub i64 " + a + ", " + b + "\n";
          fn += "  " + res + " = sdiv exact i64 " + d + ", " + std::to_string(std::max<uint64_t>(1, sizeOf(l.ty->inner, at))) + "\n";
          return {res, types.ptrdiffType()};
        }
        if (lp || rp)
        {
          if (rp)
            std::swap(l, r);
          // C++ has no arithmetic on a pointer to void or to a function (no element size): an error,
          // never a `getelementptr inbounds void` LLVM rejects.
          if (!l.ty->inner || l.ty->inner->kind == TyKind::Void || l.ty->inner->kind == TyKind::Function || !l.ty->inner->isKnown())
            unsupported(at, "arithmetic on a pointer to " + (l.ty->inner ? types.toString(l.ty->inner, &symbols) : std::string("?")));
          Value idx = convert(r, types.intType(64), at);
          std::string i = idx.text;
          if (op == P::Minus)
          {
            std::string neg = newTemp();
            fn += "  " + neg + " = sub i64 0, " + i + "\n";
            i = neg;
          }
          fn += "  " + res + " = getelementptr inbounds " + llType(l.ty->inner, at) + ", ptr " + l.text + ", i64 " + i + "\n";
          return {res, l.ty};
        }
      }
      // C++ has no other arithmetic on pointers (`(LPWSTR)&msg` with LPWSTR read as a variable is
      // `LPWSTR & msg`): an error, not an instruction LLVM rejects.
      if (lp || rp)
      {
        std::string spelling(punctuatorSpelling(op));
        unsupported(at, spelling + " on a pointer (" + types.toString(l.ty, &symbols) + " " + spelling + " " + types.toString(r.ty, &symbols) + ")");
      }
      // A shift has its left operand's promoted type, the count converted to it (LLVM shifts two
      // operands of one type), never a type common to both: `position.x >> sideBitShift` with an
      // int32 x and a uint32 count is an arithmetic shift of an int32 (ChunkPosition(TilePosition)).
      const Ty* ct = op == P::Shl || op == P::Shr ? promoted(l.ty) : commonArithmetic(l.ty, r.ty);
      l = convert(l, ct, at);
      r = convert(r, ct, at);
      bool isFloat = ct->kind == TyKind::Float;
      bool isUnsigned = ct->isUnsigned;
      const char* ins = nullptr;
      switch (op)
      {
        case P::Plus: ins = isFloat ? "fadd" : "add"; break;
        case P::Minus: ins = isFloat ? "fsub" : "sub"; break;
        case P::Star: ins = isFloat ? "fmul" : "mul"; break;
        case P::Slash: ins = isFloat ? "fdiv" : isUnsigned ? "udiv" : "sdiv"; break;
        case P::Percent: ins = isFloat ? "frem" : isUnsigned ? "urem" : "srem"; break;
        case P::Amp: ins = "and"; break;
        case P::Pipe: ins = "or"; break;
        case P::Caret: ins = "xor"; break;
        case P::Shl: ins = "shl"; break;
        case P::Shr: ins = isUnsigned ? "lshr" : "ashr"; break;
        default: unsupported(at, "this operator");
      }
      fn += "  " + res + " = " + ins + " " + llType(ct, at) + " " + l.text + ", " + r.text + "\n";
      return {res, ct};
    }

    Value compare(P op, Value l, Value r, Node* at)
    {
      std::string res = newTemp();
      if (l.ty->kind == TyKind::Pointer || r.ty->kind == TyKind::Pointer || l.ty->kind == TyKind::Nullptr || r.ty->kind == TyKind::Nullptr)
      {
        const Ty* pt = l.ty->kind == TyKind::Pointer ? l.ty : r.ty->kind == TyKind::Pointer ? r.ty : types.pointer(types.voidType());
        l = convert(l, pt, at);
        r = convert(r, pt, at);
        const char* pred = op == P::EqEq ? "eq" : op == P::NotEq ? "ne" : op == P::Less ? "ult" : op == P::LessEq ? "ule" : op == P::Greater ? "ugt" : "uge";
        fn += "  " + res + " = icmp " + pred + " ptr " + l.text + ", " + r.text + "\n";
        return {res, types.boolType()};
      }
      const Ty* ct = (l.ty->kind == TyKind::Bool && r.ty->kind == TyKind::Bool) ? types.boolType() : commonArithmetic(l.ty, r.ty);
      l = convert(l, ct, at);
      r = convert(r, ct, at);
      if (ct->kind == TyKind::Float)
      {
        const char* pred = op == P::EqEq ? "oeq" : op == P::NotEq ? "une" : op == P::Less ? "olt" : op == P::LessEq ? "ole" : op == P::Greater ? "ogt" : "oge";
        fn += "  " + res + " = fcmp " + pred + " " + llType(ct, at) + " " + l.text + ", " + r.text + "\n";
      }
      else
      {
        bool u = ct->isUnsigned || ct->kind == TyKind::Bool;
        const char* pred = op == P::EqEq ? "eq" : op == P::NotEq ? "ne" : op == P::Less ? (u ? "ult" : "slt") : op == P::LessEq ? (u ? "ule" : "sle")
                         : op == P::Greater ? (u ? "ugt" : "sgt") : (u ? "uge" : "sge");
        fn += "  " + res + " = icmp " + pred + " " + llType(ct, at) + " " + l.text + ", " + r.text + "\n";
      }
      return {res, types.boolType()};
    }

    void emitBranch(const std::string& target)
    {
      if (!blockTerminated)
        fn += "  br label %" + target + "\n";
      blockTerminated = true;
    }

    void startBlock(const std::string& name)
    {
      fn += name + ":\n";
      blockTerminated = false;
      currentBlock = name;
    }

    // Call expression. sret: where to construct a class result (empty: a temporary).
    Value callValue(CallExpr* c, const std::string& sret)
    {
      Expr* callee = c->callee;
      if (callee->kind == NodeKind::NameExpr)
      {
        auto* n = static_cast<NameExpr*>(callee);
        Decl* d = n->resolved;
        if (!d && n->name.parts.size() > 1 && n->name.last().special == 2 && P(n->name.last().opToken) == P::Assign && c->args.size() == 1)
        {
          // `super::operator=(other)` of a class declaring none: its implicit assignment, as the
          // walker typed it, on this object (`this->super::operator=(other)`).
          const Ty* q = qualifierOf(n);
          q = q ? types.unqualified(types.stripRef(q)) : nullptr;
          if (!q || !isClass(q) || !currentClassTy)
            unsupported(c, "operator= qualified with no class of this object");
          return implicitAssignment(thisPointer(c), currentClassTy, canonicalClass(q, c), c->args[0], c);
        }
        if (d && d->kind == NodeKind::FunctionDecl && d->file && d->file->isImported)
        {
          // An intrinsic declared by intrin.h (its static inline body is not emitted, or there is
          // none): the code generator's own IR.
          if (std::optional<Value> v = sseBuiltin(n, c))
            return *v;
          if (std::optional<Value> v = msvcBuiltin(n, c))
            return *v;
        }
        if (d && d->kind == NodeKind::FunctionDecl)
        {
          auto* f = static_cast<FunctionDecl*>(d);
          std::string thisArg;
          const Ty* classTy = nullptr;
          if (Decl* owner = global.owningClass(f); owner && !(f->flags & DF_Static))
          {
            if (!currentClassTy)
              unsupported(c, "a method call outside a method");
            thisArg = thisPointer(c);
            classTy = isClass(currentClassTy) ? currentClassTy : nullptr;
            if (owner->kind == NodeKind::ClassDecl && classTy && classDef(owner, c) != classDef(currentClassTy->decl, c))
            {
              // Inherited method: find the base subobject of the right class.
              const Ty* base = baseInstantiation(currentClassTy, owner, c);
              std::string addr;
              if (base && baseAddress(currentClassTy, base, thisArg, addr, c))
              {
                thisArg = addr;
                classTy = base;
              }
            }
            else if (owner->kind == NodeKind::ClassDecl && classTy && n->name.parts.size() > 1)
              if (const Ty* q = qualifierType(n, c); q && classDef(q->decl, c) == classDef(owner, c) && q != canonicalClass(currentClassTy, c))
              {
                // `Chain<Tail...>::count()` in Chain<Head, Tail...>: the member of the base the
                // qualifier names, an instance of the same template as this class.
                std::string addr;
                if (!baseAddress(currentClassTy, q, thisArg, addr, c))
                  unsupported(c, "a call of " + qualifiedName(f) + " through " + types.toString(q, &symbols) + ", which is no base of " +
                                     types.toString(currentClassTy, &symbols));
                thisArg = addr;
                classTy = q;
              }
          }
          std::string indirect;
          if (classTy && n->name.parts.size() == 1 && !thisArg.empty())
            if (int slot = slotOf(classTy, f, c); slot >= 0)
              indirect = loadSlot(thisArg, slot);
          if (!classTy && (f->flags & DF_Static) && n->name.parts.size() == 1 && currentClassTy && isClass(currentClassTy))
            if (Decl* owner = global.owningClass(f); owner && owner->kind == NodeKind::ClassDecl)
            {
              // `listOptions()` inside Item<Mode>: a static member of the class being emitted (or
              // of a base), with that instantiation's arguments.
              if (classDef(owner, c) == classDef(currentClassTy->decl, c))
                classTy = currentClassTy;
              else if (const Ty* base = baseInstantiation(currentClassTy, owner, c))
                classTy = base;
            }
          if (!classTy && (f->flags & DF_Static) && n->name.parts.size() > 1 && global.owningClass(f))
          {
            // `Box<T>::get()`: the instantiation comes from the qualifier.
            QualifiedName qualifier = n->name;
            qualifier.parts.count -= 1;
            NamedType tmp{};
            tmp.kind = NodeKind::NamedType;
            tmp.token = n->token;
            tmp.name = qualifier;
            TemplateArgs none;
            const Ty* q = global.typeOfAstIn(&tmp, typingContext(), currentArgs ? *currentArgs : none);
            if (!q || !q->isKnown() || types.isDependent(q))
              if (const Ty* bound = boundParameter(qualifier))
                q = bound;
            // `T::computeHash(args...)` with T a class deriving from the template that declares it
            // (ComplexExpressionWithOperation<BasisNoise> from ComplexExpression<BasisNoise, 2, 6>):
            // the member of that base instance, with its arguments.
            const Ty* declaring = q && isClass(q) ? instantiationIn(q, global.owningClass(f), c) : nullptr;
            if (declaring && !declaring->args.empty())
              classTy = declaring;
            else if (q && isClass(q) && !q->args.empty())
              classTy = canonicalClass(q, c);
          }
          // move(x) and forward(x): identity casts (lref -> rref in the type system,
          // but both are just a pointer in LLVM IR).
          if (!classTy && f->params.size() == 1 && c->args.size() == 1 &&
              !f->templateParams.empty() && c->templateArgCount != f->templateParams.size())
          {
            std::string_view fname = f->name.empty() ? "" : symbols.name(f->name.last().symbol);
            if (fname == "move" || fname == "forward")
            {
              std::string addr = lvalue(c->args[0]);
              const Ty* argTy = c->args[0]->ty;
              if (argTy) argTy = types.stripRef(argTy);
              return {addr, argTy ? argTy : types.unknown(), false};
            }
          }
          return callFunction(f, calleeArgs(f, classTy, c, c), classTy, thisArg, sret, c->args, c, indirect);
        }
        if (!d)
          if (std::optional<Value> v = vaBuiltin(n, c))
            return *v;
        if (!d)
          if (std::optional<Value> v = sseBuiltin(n, c))
            return *v;
        if (!d || d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
            (d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isType) || // `func(a, b)` with a function as a constant parameter: a call
            (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)))
        {
          const Ty* t = c->ty;
          if (!t || !t->isKnown())
          {
            // Try to derive the type from the resolved declaration.
            if (d && d->kind == NodeKind::ClassDecl)
            {
              NamedType tmp{};
              tmp.kind = NodeKind::NamedType;
              tmp.name = n->name;
              TemplateArgs none;
              t = global.typeOfAstIn(&tmp, typingContext(), currentArgs ? *currentArgs : none);
              if (t) t = types.unqualified(types.stripRef(t));
              if (!t || !t->isKnown())
                t = types.classType(static_cast<ClassDecl*>(d), {});
            }
            else if (d && d->kind == NodeKind::EnumDecl)
              t = types.enumType(static_cast<ClassDecl*>(d));
            else if (d && (d->kind == NodeKind::UsingAliasDecl || d->kind == NodeKind::TemplateParamDecl))
            {
              t = declType(d);
              if (t) t = types.unqualified(types.stripRef(t));
            }
            if (!t || !t->isKnown())
              unsupported(c, "a call that did not resolve");
          }
          if (isClass(t))
          {
            std::string addr = sret.empty() ? newObjectSlot(t, c) : sret;
            directInitialize(addr, t, c->args, false, c, true); // `Widths({{120, 0}})`: the list is the constructor's argument
            if (sret.empty())
              statementTemps.push_back({addr, t});
            return {addr, t, sret.empty()};
          }
          if (c->args.size() == 1 && isPackExpansion(c->args[0]))
          {
            // `T(forward<Args>(args)...)` with a scalar T: the pack's one element, re-typed for it.
            Expr* pattern = static_cast<UnaryExpr*>(c->args[0])->operand;
            size_t count = packElementCount(pattern, c);
            if (count == 0)
              return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
            if (count != 1)
              unsupported(c, "a scalar built from a pack of several elements");
            Value v;
            forEachPackElement(pattern, c, [&](size_t) { v = explicitConvert(rvalue(pattern), t, c); });
            return v;
          }
          if (c->args.size() == 1)
            return explicitConvert(rvalue(c->args[0]), t, c); // `uintptr_t(&x)` is the cast `(uintptr_t)&x`
          if (c->args.empty())
            return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
          unsupported(c, "this functional cast");
        }
      }
      else if (callee->kind == NodeKind::MemberExpr)
      {
        auto* m = static_cast<MemberExpr*>(callee);
        if (!m->member.empty() && m->member.last().special == 1)
        {
          // Explicit destructor call.
          std::string addr;
          const Ty* objectTy;
          if (m->op == P::Arrow)
          {
            Value p = arrowOperand(rvalue(m->base), c);
            if (p.ty->kind != TyKind::Pointer)
              unsupported(c, "-> on something that is not a pointer");
            addr = p.text;
            objectTy = p.ty->inner;
          }
          else
          {
            Value o = rvalue(m->base);
            addr = o.text;
            objectTy = o.ty;
          }
          if (isClass(objectTy))
            destroy(addr, objectTy, c);
          return {"", types.voidType()};
        }
        if (m->member.parts.size() == 1 && m->member.last().special == 2 && P(m->member.last().opToken) == P::LParen && isLambda(m->ty))
        {
          // `runTest.operator()<CraftingMachine>(name)`: a call of the lambda, its template arguments
          // on the call as the type checker recorded them.
          Value closure;
          if (m->op == P::Arrow)
          {
            Value p = arrowOperand(rvalue(m->base), c);
            if (p.ty->kind != TyKind::Pointer)
              unsupported(c, "-> on something that is not a pointer");
            closure = {p.text, p.ty->inner};
          }
          else
            closure = rvalue(m->base);
          return callClosure(closure, c->args, sret, c);
        }
        if ((!m->resolved || m->resolved->kind != NodeKind::FunctionDecl) && !m->member.empty() && m->member.last().special == 2 &&
            P(m->member.last().opToken) == P::Assign && c->args.size() == 1)
        {
          // `this->CollisionMask::operator=(other)` of a class declaring none: its implicit
          // assignment, member by member, of the base the name is qualified with; the object.
          std::string addr;
          const Ty* objectTy;
          if (m->op == P::Arrow)
          {
            Value p = arrowOperand(rvalue(m->base), c);
            if (p.ty->kind != TyKind::Pointer)
              unsupported(c, "-> on something that is not a pointer");
            addr = p.text;
            objectTy = p.ty->inner;
          }
          else
          {
            Value o = rvalue(m->base);
            addr = o.text;
            objectTy = o.ty;
          }
          objectTy = types.unqualified(types.stripRef(objectTy));
          if (!isClass(objectTy))
            unsupported(c, "operator= on a non-class object");
          const Ty* target = objectTy;
          if (const Ty* q = memberQualifier(m, c))
            target = q;
          return implicitAssignment(addr, objectTy, target, c->args[0], c);
        }
        if (m->resolved && m->resolved->kind == NodeKind::FunctionDecl)
        {
          auto* f = static_cast<FunctionDecl*>(m->resolved);
          std::string thisArg;
          const Ty* objectTy = nullptr;
          if (!(f->flags & DF_Static))
          {
            if (m->op == P::Arrow)
            {
              Value p = arrowOperand(rvalue(m->base), c);
              if (p.ty->kind != TyKind::Pointer)
                unsupported(c, "-> on something that is not a pointer");
              thisArg = p.text;
              objectTy = p.ty->inner;
            }
            else
            {
              Value o = rvalue(m->base);
              if (isClass(o.ty))
              {
                thisArg = o.text;
                objectTy = o.ty;
              }
              else if (o.ty && types.stripRef(o.ty)->kind == TyKind::Enum)
              {
                // A method of an enum: `this` is the value, passed by its address.
                const Ty* et = types.stripRef(o.ty);
                std::string slot = "%v" + std::to_string(temp++);
                allocas += "  " + slot + " = alloca " + llType(et, c) + "\n";
                fn += "  store " + llType(et, c) + " " + o.text + ", ptr " + slot + "\n";
                thisArg = slot;
                objectTy = et;
              }
              else
                unsupported(c, "a method call on a non-class object");
            }
            if (isClass(objectTy))
            {
              objectTy = canonicalClass(objectTy, c);
              Decl* owner = global.owningClass(f);
              if (owner && classDef(owner, c) != static_cast<ClassDecl*>(objectTy->decl))
              {
                // `r.Hook<Robot, TagB>::getId()`: the instance of the method's class template that
                // the qualifier is or derives from, not the first instance among the bases.
                const Ty* qualifier = memberQualifier(m, c);
                const Ty* base = qualifier ? instantiationIn(qualifier, owner, c) : nullptr;
                if (!base)
                  base = baseInstantiation(objectTy, owner, c);
                std::string addr;
                if (base && baseAddress(objectTy, base, thisArg, addr, c))
                {
                  thisArg = addr;
                  objectTy = base;
                }
              }
            }
            else
              objectTy = nullptr; // an enum: no class arguments, no bases
          }
          else if (m->base)
          {
            const Ty* bt = types.stripRef(m->base->ty);
            if (bt && bt->kind == TyKind::Pointer)
              bt = bt->inner;
            if (isClass(bt))
            {
              objectTy = canonicalClass(bt, c);
              // `object.staticType()` with staticType a static member of a base class template
              // (ComplexExpression<WithOperation<7, 2, 0>, 2, 0>): that base instance's member,
              // as for a call through the derived class's name.
              Decl* owner = global.owningClass(f);
              if (owner && classDef(owner, c) != static_cast<ClassDecl*>(objectTy->decl))
                if (const Ty* base = baseInstantiation(objectTy, owner, c))
                  objectTy = base;
            }
          }
          std::string indirect;
          if (objectTy && !thisArg.empty() && m->member.parts.size() == 1)
            if (int slot = slotOf(objectTy, f, c); slot >= 0)
              indirect = loadSlot(thisArg, slot);
          return callFunction(f, calleeArgs(f, objectTy, c, c), objectTy, thisArg, sret, c->args, c, indirect);
        }
      }
      // `(obj.*pmf)(args)` / `(p->*pmf)(args)`: a call through a pointer to a member function, the
      // object first.
      {
        Expr* inner = callee;
        while (inner && inner->kind == NodeKind::ParenExpr)
          inner = static_cast<ParenExpr*>(inner)->inner;
        if (inner && inner->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(inner)->isKeyword &&
            (P(static_cast<BinaryExpr*>(inner)->op) == P::DotStar || P(static_cast<BinaryExpr*>(inner)->op) == P::ArrowStar))
        {
          auto* b = static_cast<BinaryExpr*>(inner);
          const Ty* mp = b->rhs && b->rhs->ty ? types.stripRef(b->rhs->ty) : nullptr;
          if (mp && mp->kind == TyKind::MemberPointer && mp->inner && mp->inner->kind == TyKind::Function)
          {
            Value o = P(b->op) == P::ArrowStar ? arrowOperand(rvalue(b->lhs), c) : rvalue(b->lhs);
            std::string object = o.text;
            // The object as the pointer's class: an object of a class derived from it moves to that base.
            const Ty* objectTy = P(b->op) == P::ArrowStar ? (o.ty && o.ty->kind == TyKind::Pointer ? o.ty->inner : nullptr) : types.stripRef(o.ty);
            const Ty* memberOf = mp->args.empty() ? nullptr : withCurrentArgs(mp->args[0]);
            if (isClass(objectTy) && isClass(memberOf) && classKey(objectTy) != classKey(memberOf))
              baseAddress(objectTy, memberOf, o.text, object, c);
            Value fp = rvalue(b->rhs);
            return callIndirect(fp.text, mp->inner, object, c->args, sret, c);
          }
        }
      }
      // Call through a closure or a function pointer.
      Value fnValue = rvalue(callee);
      if (isLambda(fnValue.ty))
        return callClosure(fnValue, c->args, sret, c);
      if (isClass(fnValue.ty))
        return operatorCall(fnValue, "operator()", c->args, c, true, sret); // a functor: object(args)
      const Ty* ft = fnValue.ty->kind == TyKind::Pointer ? fnValue.ty->inner : fnValue.ty;
      if (!ft || ft->kind != TyKind::Function)
        unsupported(c, "a call that did not resolve to a function (the callee has type " + types.toString(fnValue.ty, &symbols) + ")");
      return callIndirect(fnValue.text, ft, "", c->args, sret, c);
    }

    // A call through a function pointer, with the same calling convention as a declared Kpp
    // function: the object first for a member function (thisArg), the caller's result slot for a
    // class result, classes by pointer (a copy the callee owns), references as addresses.
    Value callIndirect(const std::string& fnText, const Ty* ft, const std::string& thisArg, Span<Expr*> callArgs, const std::string& sret, Node* c)
    {
      Signature s;
      const Ty* ret = ft->inner;
      if (returnsInMemory(ret))
      {
        s.hasSret = true;
        s.returnType = "void";
      }
      else if (!ret || !ret->isKnown())
        s.returnType = "ptr";
      else
        s.returnType = ret->isReference() ? "ptr" : llType(ret, c);
      std::string args, resultAddr;
      if (!thisArg.empty())
      {
        args = "ptr " + thisArg;
        s.paramTypes.push_back("ptr");
      }
      if (s.hasSret)
      {
        resultAddr = sret.empty() ? newObjectSlot(ret, c) : sret;
        args += (args.empty() ? "" : ", ") + std::string("ptr ") + resultAddr;
        s.paramTypes.push_back("ptr");
      }
      size_t position = 0;
      auto addArgument = [&](Expr* arg)
      {
        const Ty* pt = position < ft->args.size() ? ft->args[position] : nullptr;
        std::string text = argumentText(pt, arg, c);
        args += (args.empty() ? "" : ", ") + text;
        ++position;
      };
      for (Expr* arg : callArgs)
      {
        if (isPackExpansion(arg))
        {
          Expr* pattern = static_cast<UnaryExpr*>(arg)->operand;
          forEachPackElement(pattern, c, [&](size_t) { addArgument(pattern); });
        }
        else
          addArgument(arg);
      }
      for (const Ty* pt : ft->args)
        s.paramTypes.push_back(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, c));
      s.variadic = ft->variadic;
      std::string sig = signatureText(s);
      if (s.returnType == "void")
      {
        emitCall("", sig + " " + fnText + "(" + args + ")");
        if (s.hasSret)
          return {resultAddr, ret, sret.empty()};
        return {"", types.voidType()};
      }
      std::string r = newTemp();
      emitCall(r + " = ", sig + " " + fnText + "(" + args + ")");
      return {r, ret, false};
    }

    // va_start, va_end and va_copy on a C va_list (a char* on x64 Windows): the LLVM intrinsics.
    // They are not declared anywhere; a function defined with `...` uses them like C does.
    std::optional<Value> vaBuiltin(NameExpr* n, CallExpr* c)
    {
      if (n->name.parts.size() != 1)
        return std::nullopt;
      std::string name = sym(n->name.parts[0].symbol);
      if (name == "va_arg" && c->args.size() == 2 && c->args[1]->kind == NodeKind::SizeofExpr && c->ty)
      {
        // `va_arg(list, T)` (Parser::parseVaArg): x64 Windows passes each variadic argument in an
        // 8-byte slot, one larger than 8 bytes or of another size than 1, 2, 4, 8 by its address.
        const Ty* t = withCurrentArgs(types.unqualified(types.stripRef(c->ty)));
        std::string list = lvalue(c->args[0]);
        std::string slot = newTemp(), next = newTemp();
        fn += "  " + slot + " = load ptr, ptr " + list + "\n";
        fn += "  " + next + " = getelementptr inbounds i8, ptr " + slot + ", i64 8\n";
        fn += "  store ptr " + next + ", ptr " + list + "\n";
        uint64_t size = sizeOf(t, c);
        if (size > 8 || (size != 1 && size != 2 && size != 4 && size != 8))
        {
          std::string address = newTemp();
          fn += "  " + address + " = load ptr, ptr " + slot + "\n";
          return load(address, t, c);
        }
        return load(slot, t, c);
      }
      if (name != "va_start" && name != "va_end" && name != "va_copy")
        return std::nullopt;
      if (c->args.size() < (name == "va_start" ? 2u : name == "va_copy" ? 2u : 1u))
        unsupported(c, name + " with too few arguments");
      std::string list = lvalue(c->args[0]);
      std::string intrinsic = name == "va_start" ? "@llvm.va_start.p0" : name == "va_end" ? "@llvm.va_end.p0" : "@llvm.va_copy.p0";
      if (declaredNames.claim(intrinsic))
        decls += "declare void " + intrinsic + (name == "va_copy" ? "(ptr, ptr)\n" : "(ptr)\n");
      if (name == "va_copy")
        fn += "  call void " + intrinsic + "(ptr " + list + ", ptr " + lvalue(c->args[1]) + ")\n";
      else
        fn += "  call void " + intrinsic + "(ptr " + list + ")\n";
      return Value{"", types.voidType(), false};
    }

    // SSE intrinsics: _mm_* functions and _MM_SHUFFLE macros.
    // They are not declared; the codegen emits LLVM IR directly for each one.

    // The LLVM vector type for an SSE suffix: ps = <4 x float>, pd = <2 x double>,
    // si128/epi32/epu32/epi8 = <4 x i32> (or <2 x i64>, <16 x i8> as needed).
    const Ty* sseTypeByName(const std::string& name)
    {
      if (name.ends_with("_ps")) return types.vectorType(types.floatType(32), 4);
      if (name.ends_with("_pd")) return types.vectorType(types.floatType(64), 2);
      if (name.ends_with("_si128")) return types.vectorType(types.intType(32), 4);
      if (name.ends_with("_epi32") || name.ends_with("_epu32")) return types.vectorType(types.intType(32), 4);
      if (name.ends_with("_epi64")) return types.vectorType(types.intType(64), 2);
      if (name.ends_with("_epi8")) return types.vectorType(types.intType(8), 16);
      return nullptr;
    }

    // A SIMD vector argument as a register value of vecTy. A vector value is a register like a
    // scalar (bitcast when its lanes differ: an __m128i of <4 x i32> where <16 x i8> is wanted);
    // an SSE wrapper class (float32x4, float64x2, int32x4) gives its first field.
    std::string sseArg(Expr* arg, const Ty* vecTy, Node* at)
    {
      Value v = rvalue(arg);
      const Ty* vt = v.ty ? types.unqualified(types.stripRef(v.ty)) : nullptr;
      if (vt && vt->kind == TyKind::Class)
      {
        StructInfo& info = structFor(vt, at);
        if (info.fieldDecls.empty())
          unsupported(at, "an SSE operation on " + types.toString(vt, &symbols) + ", a class without a vector member");
        std::string r = newTemp();
        fn += "  " + r + " = load " + llType(vecTy, at) + ", ptr " + gep(info.name, v.text, 0) + "\n"; // the __m128/__m128d/__m128i member
        return r;
      }
      if (!vt || vt->kind != TyKind::Vector)
        unsupported(at, "an SSE operation on " + (vt ? types.toString(vt, &symbols) : std::string("an operand without a type")) + ", which is not a vector");
      if (llType(vt, at) == llType(vecTy, at))
        return v.text;
      std::string r = newTemp();
      fn += "  " + r + " = bitcast " + llType(vt, at) + " " + v.text + " to " + llType(vecTy, at) + "\n";
      return r;
    }

    // Evaluate a scalar argument (float, double, int32_t) as a register value.
    std::string sseScalar(Expr* arg, const Ty* scalarTy, Node* at)
    {
      Value v = rvalue(arg);
      return convert(v, scalarTy, at).text;
    }

    // Declare an LLVM intrinsic if not already declared, and return its name.
    std::string sseIntrinsic(const std::string& name, const std::string& retTy, const std::string& paramTys)
    {
      std::string full = "@" + name;
      if (declaredNames.claim(full))
        decls += "declare " + retTy + " " + full + "(" + paramTys + ")\n";
      return full;
    }

    // Emit a binary SSE operation: fadd, fsub, fmul, fdiv, add, sub for vectors.
    std::string sseBinOp(const std::string& op, const std::string& tyStr, const std::string& a, const std::string& b)
    {
      std::string r = newTemp();
      fn += "  " + r + " = " + op + " " + tyStr + " " + a + ", " + b + "\n";
      return r;
    }

    // Emit an LLVM call.
    std::string sseCall(const std::string& intrinsic, const std::string& retTy, const std::string& args)
    {
      std::string r = newTemp();
      fn += "  " + r + " = call " + retTy + " " + intrinsic + "(" + args + ")\n";
      return r;
    }

    // A vector result: its register, as any value of a scalar type is (it was a slot's address, which
    // a local or a parameter of type __m128 then stored as the vector itself).
    Value sseResult(const std::string& reg, const Ty* vecTy, Node*)
    {
      return {reg, vecTy, false};
    }

    // An SSE operation's immediate operand: a constant, or the macros `_MM_SHUFFLE(z, y, x, w)` and
    // `_MM_SHUFFLE2(x, y)`, which the import does not have (function-like) and evalConst cannot call.
    std::optional<int64_t> sseImmediate(Expr* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (e && e->kind == NodeKind::CallExpr && static_cast<CallExpr*>(e)->callee->kind == NodeKind::NameExpr)
      {
        auto* call = static_cast<CallExpr*>(e);
        auto* callee = static_cast<NameExpr*>(call->callee);
        std::string name = callee->name.parts.size() == 1 ? sym(callee->name.parts[0].symbol) : std::string();
        if ((name == "_MM_SHUFFLE" && call->args.size() == 4) || (name == "_MM_SHUFFLE2" && call->args.size() == 2))
        {
          int64_t value = 0;
          for (Expr* a : call->args)
          {
            std::optional<int64_t> part = sseImmediate(a);
            if (!part)
              return std::nullopt;
            value = (value << (name == "_MM_SHUFFLE" ? 2 : 1)) | *part;
          }
          return value;
        }
      }
      return evalConst(e, 0);
    }

    std::optional<Value> sseBuiltin(NameExpr* n, CallExpr* c)
    {
      if (n->name.parts.size() != 1)
        return std::nullopt;
      std::string name = sym(n->name.parts[0].symbol);

      // _MM_SHUFFLE(z, y, x, w) = ((z<<6)|(y<<4)|(x<<2)|w)
      if (name == "_MM_SHUFFLE" && c->args.size() == 4)
      {
        auto e = [&](int i) { return evalConst(c->args[i], 0); };
        auto z = e(0), y = e(1), x = e(2), w = e(3);
        if (z && y && x && w)
          return Value{std::to_string((*z << 6) | (*y << 4) | (*x << 2) | *w), types.intType(32)};
        return std::nullopt;
      }
      if (name == "_MM_SHUFFLE2" && c->args.size() == 2)
      {
        auto a = evalConst(c->args[0], 0), b = evalConst(c->args[1], 0);
        if (a && b)
          return Value{std::to_string((*a << 1) | *b), types.intType(32)};
        return std::nullopt;
      }
      // _MM_GET_ROUNDING_MODE() = (_mm_getcsr() & _MM_ROUND_MASK), a function-like macro
      if (name == "_MM_GET_ROUNDING_MODE" && c->args.empty())
      {
        std::optional<Value> csr = sseMore("_mm_getcsr", c);
        std::string r = newTemp();
        fn += "  " + r + " = and i32 " + csr->text + ", 24576\n"; // 0x6000
        return Value{r, types.intType(32, true)};
      }

      if (!name.starts_with("_mm_"))
        return std::nullopt;

      // Determine the vector type from the name suffix.
      const Ty* v4f = types.vectorType(types.floatType(32), 4);
      const Ty* v2d = types.vectorType(types.floatType(64), 2);
      const Ty* v4i = types.vectorType(types.intType(32), 4);
      const Ty* v2i64 = types.vectorType(types.intType(64), 2);
      const Ty* v16i8 = types.vectorType(types.intType(8), 16);

      auto ll = [&](const Ty* t) { return llType(t, c); };

      // --- undefined (zero args) ---
      if (name == "_mm_undefined_ps")  return sseResult("undef", v4f, c);
      if (name == "_mm_undefined_pd")  return sseResult("undef", v2d, c);
      if (name == "_mm_undefined_si128") return sseResult("undef", v4i, c);
      if (name == "_mm_setzero_si128") return sseResult("zeroinitializer", v4i, c);

      // --- arithmetic: binary float ---
      auto floatBinOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        return sseResult(sseBinOp(op, ll(vt), a, b), vt, c);
      };

      if (name == "_mm_add_ps")  return floatBinOp("fadd", v4f);
      if (name == "_mm_sub_ps")  return floatBinOp("fsub", v4f);
      if (name == "_mm_mul_ps")  return floatBinOp("fmul", v4f);
      if (name == "_mm_div_ps")  return floatBinOp("fdiv", v4f);
      if (name == "_mm_add_pd")  return floatBinOp("fadd", v2d);
      if (name == "_mm_sub_pd")  return floatBinOp("fsub", v2d);
      if (name == "_mm_mul_pd")  return floatBinOp("fmul", v2d);
      if (name == "_mm_div_pd")  return floatBinOp("fdiv", v2d);

      // --- arithmetic: binary integer ---
      auto intBinOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        return sseResult(sseBinOp(op, ll(vt), a, b), vt, c);
      };

      if (name == "_mm_add_epi32") return intBinOp("add", v4i);
      if (name == "_mm_sub_epi32") return intBinOp("sub", v4i);

      // --- bitwise: implemented as integer and/or/xor then bitcast back ---
      auto bitwiseOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        // Load as the native type, bitcast to i128 for the operation, bitcast back.
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        // For float types, bitcast to <4 x i32> for the integer operation.
        bool needsCast = (vt == v4f || vt == v2d);
        const Ty* intVt = (vt == v2d) ? v2i64 : v4i;
        std::string ai = a, bi = b;
        if (needsCast)
        {
          ai = newTemp(); fn += "  " + ai + " = bitcast " + ll(vt) + " " + a + " to " + ll(intVt) + "\n";
          bi = newTemp(); fn += "  " + bi + " = bitcast " + ll(vt) + " " + b + " to " + ll(intVt) + "\n";
        }
        std::string ri = sseBinOp(op, ll(intVt), ai, bi);
        std::string res = ri;
        if (needsCast)
        {
          res = newTemp(); fn += "  " + res + " = bitcast " + ll(intVt) + " " + ri + " to " + ll(vt) + "\n";
        }
        return sseResult(res, vt, c);
      };

      if (name == "_mm_and_ps")    return bitwiseOp("and", v4f);
      if (name == "_mm_or_ps")     return bitwiseOp("or", v4f);
      if (name == "_mm_xor_ps")    return bitwiseOp("xor", v4f);
      if (name == "_mm_and_pd")    return bitwiseOp("and", v2d);
      if (name == "_mm_or_pd")     return bitwiseOp("or", v2d);
      if (name == "_mm_xor_pd")    return bitwiseOp("xor", v2d);
      if (name == "_mm_and_si128") return bitwiseOp("and", v4i);
      if (name == "_mm_or_si128")  return bitwiseOp("or", v4i);
      if (name == "_mm_xor_si128") return bitwiseOp("xor", v4i);

      // andnot: ~a & b
      auto andnotOp = [&](const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        bool needsCast = (vt == v4f || vt == v2d);
        const Ty* intVt = (vt == v2d) ? v2i64 : v4i;
        std::string ai = a, bi = b;
        if (needsCast)
        {
          ai = newTemp(); fn += "  " + ai + " = bitcast " + ll(vt) + " " + a + " to " + ll(intVt) + "\n";
          bi = newTemp(); fn += "  " + bi + " = bitcast " + ll(vt) + " " + b + " to " + ll(intVt) + "\n";
        }
        // NOT a
        std::string notA = newTemp();
        fn += "  " + notA + " = xor " + ll(intVt) + " " + ai + ", <";
        for (uint32_t i = 0; i < intVt->count; ++i) fn += (i ? ", " : "") + std::string(intVt->inner->bits == 64 ? "i64 -1" : "i32 -1");
        fn += ">\n";
        std::string ri = sseBinOp("and", ll(intVt), notA, bi);
        std::string res = ri;
        if (needsCast)
        {
          res = newTemp(); fn += "  " + res + " = bitcast " + ll(intVt) + " " + ri + " to " + ll(vt) + "\n";
        }
        return sseResult(res, vt, c);
      };

      if (name == "_mm_andnot_ps")    return andnotOp(v4f);
      if (name == "_mm_andnot_pd")    return andnotOp(v2d);
      if (name == "_mm_andnot_si128") return andnotOp(v4i);

      // --- float comparisons: fcmp, then select to produce all-ones or all-zeros mask ---
      auto cmpOp = [&](const std::string& pred, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        std::string cmp = newTemp();
        std::string boolVt = "<" + std::to_string(vt->count) + " x i1>";
        fn += "  " + cmp + " = fcmp " + pred + " " + ll(vt) + " " + a + ", " + b + "\n";
        // Select: true → all-ones, false → all-zeros (as float bit pattern)
        const Ty* intVt = (vt == v2d) ? v2i64 : v4i;
        std::string allOnes = "<";
        for (uint32_t i = 0; i < intVt->count; ++i) allOnes += (i ? ", " : "") + std::string(intVt->inner->bits == 64 ? "i64 -1" : "i32 -1");
        allOnes += ">";
        std::string sel = newTemp();
        fn += "  " + sel + " = select " + boolVt + " " + cmp + ", " + ll(intVt) + " " + allOnes + ", " + ll(intVt) + " zeroinitializer\n";
        std::string res = newTemp();
        fn += "  " + res + " = bitcast " + ll(intVt) + " " + sel + " to " + ll(vt) + "\n";
        return sseResult(res, vt, c);
      };

      if (name == "_mm_cmpeq_ps")  return cmpOp("oeq", v4f);
      if (name == "_mm_cmpge_ps")  return cmpOp("oge", v4f);
      if (name == "_mm_cmpgt_ps")  return cmpOp("ogt", v4f);
      if (name == "_mm_cmple_ps")  return cmpOp("ole", v4f);
      if (name == "_mm_cmplt_ps")  return cmpOp("olt", v4f);
      if (name == "_mm_cmpneq_ps") return cmpOp("une", v4f);
      if (name == "_mm_cmpeq_pd")  return cmpOp("oeq", v2d);
      if (name == "_mm_cmpge_pd")  return cmpOp("oge", v2d);
      if (name == "_mm_cmpgt_pd")  return cmpOp("ogt", v2d);
      if (name == "_mm_cmple_pd")  return cmpOp("ole", v2d);
      if (name == "_mm_cmplt_pd")  return cmpOp("olt", v2d);
      if (name == "_mm_cmpneq_pd") return cmpOp("une", v2d);

      // --- integer comparisons ---
      auto icmpOp = [&](const std::string& pred, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        std::string cmp = newTemp();
        std::string boolVt = "<" + std::to_string(vt->count) + " x i1>";
        fn += "  " + cmp + " = icmp " + pred + " " + ll(vt) + " " + a + ", " + b + "\n";
        std::string sel = newTemp();
        std::string allOnes = "<";
        for (uint32_t i = 0; i < vt->count; ++i) allOnes += (i ? ", " : "") + std::string(vt->inner->bits == 64 ? "i64 -1" : "i32 -1");
        allOnes += ">";
        fn += "  " + sel + " = select " + boolVt + " " + cmp + ", " + ll(vt) + " " + allOnes + ", " + ll(vt) + " zeroinitializer\n";
        return sseResult(sel, vt, c);
      };

      if (name == "_mm_cmpeq_epi32") return icmpOp("eq", v4i);
      if (name == "_mm_cmpgt_epi32") return icmpOp("sgt", v4i);
      if (name == "_mm_cmplt_epi32") return icmpOp("slt", v4i);

      // --- set (splat / build) ---
      if (name == "_mm_set1_ps" && c->args.size() >= 1)
      {
        std::string s = sseScalar(c->args[0], types.floatType(32), c);
        std::string v = newTemp();
        fn += "  " + v + " = insertelement " + ll(v4f) + " undef, float " + s + ", i32 0\n";
        std::string splat = newTemp();
        fn += "  " + splat + " = shufflevector " + ll(v4f) + " " + v + ", " + ll(v4f) + " undef, <4 x i32> zeroinitializer\n";
        return sseResult(splat, v4f, c);
      }
      if (name == "_mm_set1_pd" && c->args.size() >= 1)
      {
        std::string s = sseScalar(c->args[0], types.floatType(64), c);
        std::string v = newTemp();
        fn += "  " + v + " = insertelement " + ll(v2d) + " undef, double " + s + ", i32 0\n";
        std::string splat = newTemp();
        fn += "  " + splat + " = shufflevector " + ll(v2d) + " " + v + ", " + ll(v2d) + " undef, <2 x i32> zeroinitializer\n";
        return sseResult(splat, v2d, c);
      }
      if (name == "_mm_set1_epi32" && c->args.size() >= 1)
      {
        std::string s = sseScalar(c->args[0], types.intType(32), c);
        std::string v = newTemp();
        fn += "  " + v + " = insertelement " + ll(v4i) + " undef, i32 " + s + ", i32 0\n";
        std::string splat = newTemp();
        fn += "  " + splat + " = shufflevector " + ll(v4i) + " " + v + ", " + ll(v4i) + " undef, <4 x i32> zeroinitializer\n";
        return sseResult(splat, v4i, c);
      }
      // _mm_set_ps(d, c, b, a) builds <a, b, c, d> — note reversed order
      if (name == "_mm_set_ps" && c->args.size() >= 4)
      {
        std::string vec = "undef";
        for (int i = 3; i >= 0; --i)
        {
          std::string s = sseScalar(c->args[i], types.floatType(32), c);
          std::string next = newTemp();
          fn += "  " + next + " = insertelement " + ll(v4f) + " " + vec + ", float " + s + ", i32 " + std::to_string(3 - i) + "\n";
          vec = next;
        }
        return sseResult(vec, v4f, c);
      }
      if (name == "_mm_set_pd" && c->args.size() >= 2)
      {
        std::string s0 = sseScalar(c->args[1], types.floatType(64), c);
        std::string s1 = sseScalar(c->args[0], types.floatType(64), c);
        std::string v = newTemp();
        fn += "  " + v + " = insertelement " + ll(v2d) + " undef, double " + s0 + ", i32 0\n";
        std::string v2 = newTemp();
        fn += "  " + v2 + " = insertelement " + ll(v2d) + " " + v + ", double " + s1 + ", i32 1\n";
        return sseResult(v2, v2d, c);
      }
      if (name == "_mm_set_epi32" && c->args.size() >= 4)
      {
        std::string vec = "undef";
        for (int i = 3; i >= 0; --i)
        {
          std::string s = sseScalar(c->args[i], types.intType(32), c);
          std::string next = newTemp();
          fn += "  " + next + " = insertelement " + ll(v4i) + " " + vec + ", i32 " + s + ", i32 " + std::to_string(3 - i) + "\n";
          vec = next;
        }
        return sseResult(vec, v4i, c);
      }

      // --- load (unaligned) ---
      if (name == "_mm_loadu_ps" && c->args.size() >= 1)
      {
        std::string p = rvalue(c->args[0]).text;
        std::string r = newTemp();
        fn += "  " + r + " = load " + ll(v4f) + ", ptr " + p + ", align 1\n";
        return sseResult(r, v4f, c);
      }
      if (name == "_mm_loadu_pd" && c->args.size() >= 1)
      {
        std::string p = rvalue(c->args[0]).text;
        std::string r = newTemp();
        fn += "  " + r + " = load " + ll(v2d) + ", ptr " + p + ", align 1\n";
        return sseResult(r, v2d, c);
      }
      if (name == "_mm_loadu_si128" && c->args.size() >= 1)
      {
        std::string p = rvalue(c->args[0]).text;
        std::string r = newTemp();
        fn += "  " + r + " = load " + ll(v4i) + ", ptr " + p + ", align 1\n";
        return sseResult(r, v4i, c);
      }

      // --- store (unaligned) — returns void ---
      if (name == "_mm_storeu_ps" && c->args.size() >= 2)
      {
        std::string p = rvalue(c->args[0]).text;
        std::string v = sseArg(c->args[1], v4f, c);
        fn += "  store " + ll(v4f) + " " + v + ", ptr " + p + ", align 1\n";
        return Value{"", types.voidType(), false};
      }
      if (name == "_mm_storeu_pd" && c->args.size() >= 2)
      {
        std::string p = rvalue(c->args[0]).text;
        std::string v = sseArg(c->args[1], v2d, c);
        fn += "  store " + ll(v2d) + " " + v + ", ptr " + p + ", align 1\n";
        return Value{"", types.voidType(), false};
      }

      // --- cast (bitcast between vector types, no conversion) ---
      auto castOp = [&](const Ty* from, const Ty* to) -> std::optional<Value>
      {
        if (c->args.size() < 1) return std::nullopt;
        std::string a = sseArg(c->args[0], from, c);
        std::string r = newTemp();
        fn += "  " + r + " = bitcast " + ll(from) + " " + a + " to " + ll(to) + "\n";
        return sseResult(r, to, c);
      };

      if (name == "_mm_castps_si128") return castOp(v4f, v4i);
      if (name == "_mm_castsi128_ps") return castOp(v4i, v4f);
      if (name == "_mm_castps_pd")    return castOp(v4f, v2d);
      if (name == "_mm_castpd_ps")    return castOp(v2d, v4f);
      if (name == "_mm_castsi128_pd") return castOp(v4i, v2d);
      if (name == "_mm_castpd_si128") return castOp(v2d, v4i);

      // --- convert (type conversion) ---
      if (name == "_mm_cvtepi32_ps" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v4i, c);
        std::string r = newTemp();
        fn += "  " + r + " = sitofp " + ll(v4i) + " " + a + " to " + ll(v4f) + "\n";
        return sseResult(r, v4f, c);
      }
      if (name == "_mm_cvtps_epi32" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v4f, c);
        std::string intr = sseIntrinsic("llvm.x86.sse2.cvtps2dq", ll(v4i), ll(v4f));
        return sseResult(sseCall(intr, ll(v4i), ll(v4f) + " " + a), v4i, c);
      }
      if (name == "_mm_cvtps_pd" && c->args.size() >= 1)
      {
        // Low 2 floats → 2 doubles
        std::string a = sseArg(c->args[0], v4f, c);
        const Ty* v2f = types.vectorType(types.floatType(32), 2);
        std::string lo = newTemp();
        fn += "  " + lo + " = shufflevector " + ll(v4f) + " " + a + ", " + ll(v4f) + " undef, <2 x i32> <i32 0, i32 1>\n";
        std::string r = newTemp();
        fn += "  " + r + " = fpext " + ll(v2f) + " " + lo + " to " + ll(v2d) + "\n";
        return sseResult(r, v2d, c);
      }
      if (name == "_mm_cvtpd_ps" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v2d, c);
        const Ty* v2f = types.vectorType(types.floatType(32), 2);
        std::string lo = newTemp();
        fn += "  " + lo + " = fptrunc " + ll(v2d) + " " + a + " to " + ll(v2f) + "\n";
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v2f) + " " + lo + ", " + ll(v2f) + " zeroinitializer, <4 x i32> <i32 0, i32 1, i32 2, i32 3>\n";
        return sseResult(r, v4f, c);
      }

      // --- min / max / sqrt / rcp / rsqrt ---
      auto unaryIntr = [&](const std::string& intr, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 1) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string i = sseIntrinsic(intr, ll(vt), ll(vt));
        return sseResult(sseCall(i, ll(vt), ll(vt) + " " + a), vt, c);
      };
      auto binaryIntr = [&](const std::string& intr, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        std::string i = sseIntrinsic(intr, ll(vt), ll(vt) + ", " + ll(vt));
        return sseResult(sseCall(i, ll(vt), ll(vt) + " " + a + ", " + ll(vt) + " " + b), vt, c);
      };

      if (name == "_mm_min_ps")  return binaryIntr("llvm.x86.sse.min.ps", v4f);
      if (name == "_mm_max_ps")  return binaryIntr("llvm.x86.sse.max.ps", v4f);
      if (name == "_mm_min_pd")  return binaryIntr("llvm.x86.sse2.min.pd", v2d);
      if (name == "_mm_max_pd")  return binaryIntr("llvm.x86.sse2.max.pd", v2d);
      if (name == "_mm_sqrt_ps") return unaryIntr("llvm.sqrt.v4f32", v4f);
      if (name == "_mm_sqrt_pd") return unaryIntr("llvm.sqrt.v2f64", v2d);
      if (name == "_mm_rcp_ps")  return unaryIntr("llvm.x86.sse.rcp.ps", v4f);
      if (name == "_mm_rsqrt_ps") return unaryIntr("llvm.x86.sse.rsqrt.ps", v4f);

      // --- movemask (returns i32) ---
      if (name == "_mm_movemask_ps" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v4f, c);
        std::string i = sseIntrinsic("llvm.x86.sse.movmsk.ps", "i32", ll(v4f));
        return Value{sseCall(i, "i32", ll(v4f) + " " + a), types.intType(32)};
      }
      if (name == "_mm_movemask_pd" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v2d, c);
        std::string i = sseIntrinsic("llvm.x86.sse2.movmsk.pd", "i32", ll(v2d));
        return Value{sseCall(i, "i32", ll(v2d) + " " + a), types.intType(32)};
      }
      if (name == "_mm_movemask_epi8" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v4i, c);
        std::string ab = newTemp();
        fn += "  " + ab + " = bitcast " + ll(v4i) + " " + a + " to " + ll(v16i8) + "\n";
        std::string i = sseIntrinsic("llvm.x86.sse2.pmovmskb.128", "i32", ll(v16i8));
        return Value{sseCall(i, "i32", ll(v16i8) + " " + ab), types.intType(32)};
      }

      // --- shuffle ---
      if (name == "_mm_shuffle_ps" && c->args.size() >= 3)
      {
        std::string a = sseArg(c->args[0], v4f, c);
        std::string b = sseArg(c->args[1], v4f, c);
        auto imm = sseImmediate(c->args[2]);
        if (!imm) return std::nullopt;
        int m = int(*imm);
        int i0 = m & 3, i1 = (m >> 2) & 3, i2 = ((m >> 4) & 3) + 4, i3 = ((m >> 6) & 3) + 4;
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v4f) + " " + a + ", " + ll(v4f) + " " + b +
              ", <4 x i32> <i32 " + std::to_string(i0) + ", i32 " + std::to_string(i1) + ", i32 " + std::to_string(i2) + ", i32 " + std::to_string(i3) + ">\n";
        return sseResult(r, v4f, c);
      }
      if (name == "_mm_shuffle_pd" && c->args.size() >= 3)
      {
        std::string a = sseArg(c->args[0], v2d, c);
        std::string b = sseArg(c->args[1], v2d, c);
        auto imm = sseImmediate(c->args[2]);
        if (!imm) return std::nullopt;
        int m = int(*imm);
        int i0 = m & 1, i1 = ((m >> 1) & 1) + 2;
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v2d) + " " + a + ", " + ll(v2d) + " " + b +
              ", <2 x i32> <i32 " + std::to_string(i0) + ", i32 " + std::to_string(i1) + ">\n";
        return sseResult(r, v2d, c);
      }
      if (name == "_mm_shuffle_epi32" && c->args.size() >= 2)
      {
        std::string a = sseArg(c->args[0], v4i, c);
        auto imm = sseImmediate(c->args[1]);
        if (!imm) return std::nullopt;
        int m = int(*imm);
        int i0 = m & 3, i1 = (m >> 2) & 3, i2 = (m >> 4) & 3, i3 = (m >> 6) & 3;
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v4i) + " " + a + ", " + ll(v4i) + " undef, <4 x i32> <i32 " +
              std::to_string(i0) + ", i32 " + std::to_string(i1) + ", i32 " + std::to_string(i2) + ", i32 " + std::to_string(i3) + ">\n";
        return sseResult(r, v4i, c);
      }

      // --- unpack (interleave) ---
      auto unpackOp = [&](const Ty* vt, bool high) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        uint32_t half = vt->count / 2;
        uint32_t start = high ? half : 0;
        std::string mask = "<" + std::to_string(vt->count) + " x i32> <";
        for (uint32_t i = 0; i < half; ++i)
        {
          if (i) mask += ", ";
          mask += "i32 " + std::to_string(start + i) + ", i32 " + std::to_string(vt->count + start + i);
        }
        mask += ">";
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(vt) + " " + a + ", " + ll(vt) + " " + b + ", " + mask + "\n";
        return sseResult(r, vt, c);
      };

      if (name == "_mm_unpacklo_ps")    return unpackOp(v4f, false);
      if (name == "_mm_unpackhi_ps")    return unpackOp(v4f, true);
      if (name == "_mm_unpacklo_pd")    return unpackOp(v2d, false);
      if (name == "_mm_unpackhi_pd")    return unpackOp(v2d, true);
      if (name == "_mm_unpacklo_epi32") return unpackOp(v4i, false);
      if (name == "_mm_unpackhi_epi32") return unpackOp(v4i, true);
      if (name == "_mm_unpacklo_epi64") return unpackOp(v2i64, false);
      if (name == "_mm_unpackhi_epi64") return unpackOp(v2i64, true);

      // --- shifts ---
      // Immediate shifts: slli, srli, srai
      auto shiftImmOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string imm = sseScalar(c->args[1], types.intType(32), c);
        // Splat the immediate into a vector
        std::string splat = newTemp();
        fn += "  " + splat + " = insertelement " + ll(vt) + " zeroinitializer, i32 " + imm + ", i32 0\n";
        std::string splat2 = newTemp();
        fn += "  " + splat2 + " = shufflevector " + ll(vt) + " " + splat + ", " + ll(vt) + " undef, <" + std::to_string(vt->count) + " x i32> zeroinitializer\n";
        return sseResult(sseBinOp(op, ll(vt), a, splat2), vt, c);
      };

      if (name == "_mm_slli_epi32") return shiftImmOp("shl", v4i);
      if (name == "_mm_srli_epi32") return shiftImmOp("lshr", v4i);
      if (name == "_mm_srai_epi32") return shiftImmOp("ashr", v4i);

      // Variable shifts (by __m128i count, low 64 bits):
      auto shiftVarOp = [&](const std::string& intr, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], v4i, c);
        std::string i = sseIntrinsic(intr, ll(vt), ll(vt) + ", " + ll(v4i));
        return sseResult(sseCall(i, ll(vt), ll(vt) + " " + a + ", " + ll(v4i) + " " + b), vt, c);
      };

      if (name == "_mm_sll_epi32") return shiftVarOp("llvm.x86.sse2.psll.d", v4i);
      if (name == "_mm_srl_epi32") return shiftVarOp("llvm.x86.sse2.psrl.d", v4i);
      if (name == "_mm_sra_epi32") return shiftVarOp("llvm.x86.sse2.psra.d", v4i);

      // _mm_srli_si128: byte shift right (psrldq)
      if (name == "_mm_srli_si128" && c->args.size() >= 2)
      {
        std::string a = sseArg(c->args[0], v16i8, c);
        auto imm = sseImmediate(c->args[1]);
        if (!imm || *imm < 0 || *imm > 16) return std::nullopt;
        int shift = int(*imm);
        // Build shuffle mask: shift bytes right, fill with zeros
        std::string mask = "<16 x i32> <";
        for (int i = 0; i < 16; ++i)
        {
          if (i) mask += ", ";
          int src = i + shift;
          mask += "i32 " + std::to_string(src < 16 ? src : 16); // 16+ maps to the zero vector
        }
        mask += ">";
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v16i8) + " " + a + ", " + ll(v16i8) + " zeroinitializer, " + mask + "\n";
        // Bitcast back to v4i
        std::string res = newTemp();
        fn += "  " + res + " = bitcast " + ll(v16i8) + " " + r + " to " + ll(v4i) + "\n";
        return sseResult(res, v4i, c);
      }

      // --- widening multiply ---
      if (name == "_mm_mul_epu32" && c->args.size() >= 2)
      {
        std::string a = sseArg(c->args[0], v4i, c);
        std::string b = sseArg(c->args[1], v4i, c);
        std::string i = sseIntrinsic("llvm.x86.sse2.pmulu.dq", ll(v2i64), ll(v4i) + ", " + ll(v4i));
        return sseResult(sseCall(i, ll(v2i64), ll(v4i) + " " + a + ", " + ll(v4i) + " " + b), v2i64, c);
      }

      return sseMore(name, c);
    }

    // The SSE operations sseBuiltin does not have: on 8-, 16- and 64-bit lanes, on the lowest lane
    // only (_ss, _sd), packs, saturating and horizontal ones, the set, load and store forms and the
    // conversions. An integer result is an __m128i (<4 x i32>), its lanes bitcast back; nothing for
    // a name that is none of them.
    std::optional<Value> sseMore(const std::string& name, CallExpr* c)
    {
      const Ty* v4f = types.vectorType(types.floatType(32), 4);
      const Ty* v2d = types.vectorType(types.floatType(64), 2);
      const Ty* v4i = types.vectorType(types.intType(32), 4);
      const Ty* v2i64 = types.vectorType(types.intType(64), 2);
      const Ty* v8i16 = types.vectorType(types.intType(16), 8);
      const Ty* v16i8 = types.vectorType(types.intType(8), 16);
      const Ty* f32 = types.floatType(32);
      const Ty* f64 = types.floatType(64);
      auto ll = [&](const Ty* t) { return llType(t, c); };
      auto need = [&](size_t n)
      {
        if (c->args.size() < n)
          unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
      };
      auto arg = [&](size_t i, const Ty* vt) { return sseArg(c->args[i], vt, c); };
      auto asInt = [&](const std::string& r, const Ty* lanes) -> Value
      {
        if (lanes == v4i)
          return sseResult(r, v4i, c);
        std::string b = newTemp();
        fn += "  " + b + " = bitcast " + ll(lanes) + " " + r + " to " + ll(v4i) + "\n";
        return sseResult(b, v4i, c);
      };
      auto vecName = [&](const Ty* vt) { return "v" + std::to_string(vt->count) + (vt->inner->kind == TyKind::Float ? "f" : "i") + std::to_string(vt->inner->bits); };
      auto call2 = [&](const std::string& intrinsic, const Ty* ret, const Ty* in, const std::string& a, const std::string& b)
      {
        std::string i = sseIntrinsic(intrinsic, ll(ret), ll(in) + ", " + ll(in));
        return sseCall(i, ll(ret), ll(in) + " " + a + ", " + ll(in) + " " + b);
      };
      auto constArg = [&](size_t i) -> int64_t
      {
        std::optional<int64_t> v = sseImmediate(c->args[i]);
        if (!v)
          unsupported(c, name + " with an operand that is no constant");
        return *v;
      };
      auto laneScalar = [&](const Ty* vt) { return vt->inner; };
      auto build = [&](const Ty* vt, const std::vector<size_t>& order) // lane i from argument order[i]
      {
        std::string vec = "undef";
        for (size_t i = 0; i < order.size(); ++i)
        {
          std::string s = sseScalar(c->args[order[i]], laneScalar(vt), c);
          std::string next = newTemp();
          fn += "  " + next + " = insertelement " + ll(vt) + " " + vec + ", " + ll(laneScalar(vt)) + " " + s + ", i32 " + std::to_string(i) + "\n";
          vec = next;
        }
        return vec;
      };
      auto splat = [&](const Ty* vt)
      {
        std::string s = sseScalar(c->args[0], laneScalar(vt), c);
        std::string one = newTemp(), all = newTemp();
        fn += "  " + one + " = insertelement " + ll(vt) + " undef, " + ll(laneScalar(vt)) + " " + s + ", i32 0\n";
        fn += "  " + all + " = shufflevector " + ll(vt) + " " + one + ", " + ll(vt) + " undef, <" + std::to_string(vt->count) + " x i32> zeroinitializer\n";
        return all;
      };
      auto lowLane = [&](const Ty* vt, const std::string& scalar) // `scalar` in lane 0, the other lanes zero
      {
        std::string r = newTemp();
        fn += "  " + r + " = insertelement " + ll(vt) + " zeroinitializer, " + ll(laneScalar(vt)) + " " + scalar + ", i32 0\n";
        return r;
      };
      auto extract = [&](const Ty* vt, const std::string& v, int lane)
      {
        std::string r = newTemp();
        fn += "  " + r + " = extractelement " + ll(vt) + " " + v + ", i32 " + std::to_string(lane) + "\n";
        return r;
      };
      std::string suffix = name.substr(name.rfind('_') + 1);
      const Ty* lanes = suffix == "epi8" || suffix == "epu8" ? v16i8 : suffix == "epi16" || suffix == "epu16" ? v8i16
                      : suffix == "epi32" || suffix == "epu32" ? v4i : suffix == "epi64" || suffix == "epi64x" ? v2i64 : nullptr;
      bool unsignedLanes = suffix.size() > 2 && suffix[2] == 'u';

      // --- lane arithmetic (epi32 add/sub are sseBuiltin's) ---
      if ((name.starts_with("_mm_add_ep") || name.starts_with("_mm_sub_ep") || name == "_mm_mullo_epi16" || name == "_mm_mullo_epi32") && lanes)
      {
        need(2);
        std::string op = name.starts_with("_mm_add") ? "add" : name.starts_with("_mm_sub") ? "sub" : "mul";
        return asInt(sseBinOp(op, ll(lanes), arg(0, lanes), arg(1, lanes)), lanes);
      }
      if ((name.starts_with("_mm_adds_ep") || name.starts_with("_mm_subs_ep")) && lanes)
      {
        need(2);
        std::string op = std::string(unsignedLanes ? "u" : "s") + (name.starts_with("_mm_adds") ? "add" : "sub") + ".sat";
        return asInt(call2("llvm." + op + "." + vecName(lanes), lanes, lanes, arg(0, lanes), arg(1, lanes)), lanes);
      }
      if ((name.starts_with("_mm_min_ep") || name.starts_with("_mm_max_ep")) && lanes)
      {
        need(2);
        std::string op = std::string(unsignedLanes ? "u" : "s") + (name.starts_with("_mm_min") ? "min" : "max");
        return asInt(call2("llvm." + op + "." + vecName(lanes), lanes, lanes, arg(0, lanes), arg(1, lanes)), lanes);
      }
      if (name == "_mm_mulhi_epi16" || name == "_mm_mulhi_epu16")
      {
        need(2);
        return asInt(call2(name == "_mm_mulhi_epi16" ? "llvm.x86.sse2.pmulh.w" : "llvm.x86.sse2.pmulhu.w", v8i16, v8i16, arg(0, v8i16), arg(1, v8i16)), v8i16);
      }
      if (name == "_mm_madd_epi16")
      {
        need(2);
        return asInt(call2("llvm.x86.sse2.pmadd.wd", v4i, v8i16, arg(0, v8i16), arg(1, v8i16)), v4i);
      }
      if (name == "_mm_sad_epu8")
      {
        need(2);
        return asInt(call2("llvm.x86.sse2.psad.bw", v2i64, v16i8, arg(0, v16i8), arg(1, v16i8)), v2i64);
      }

      // --- packs ---
      if (name == "_mm_packs_epi32")
      {
        need(2);
        return asInt(call2("llvm.x86.sse2.packssdw.128", v8i16, v4i, arg(0, v4i), arg(1, v4i)), v8i16);
      }
      if (name == "_mm_packs_epi16" || name == "_mm_packus_epi16")
      {
        need(2);
        return asInt(call2(name == "_mm_packs_epi16" ? "llvm.x86.sse2.packsswb.128" : "llvm.x86.sse2.packuswb.128", v16i8, v8i16, arg(0, v8i16), arg(1, v8i16)), v16i8);
      }
      if (name == "_mm_packus_epi32")
      {
        need(2);
        return asInt(call2("llvm.x86.sse41.packusdw", v8i16, v4i, arg(0, v4i), arg(1, v4i)), v8i16);
      }

      // --- lane compares: all ones where true (epi32 ones are sseBuiltin's) ---
      if ((name.starts_with("_mm_cmpeq_ep") || name.starts_with("_mm_cmpgt_ep") || name.starts_with("_mm_cmplt_ep")) && lanes && lanes != v4i)
      {
        need(2);
        std::string pred = name.starts_with("_mm_cmpeq") ? "eq" : name.starts_with("_mm_cmpgt") ? "sgt" : "slt";
        std::string cmp = newTemp(), r = newTemp();
        fn += "  " + cmp + " = icmp " + pred + " " + ll(lanes) + " " + arg(0, lanes) + ", " + arg(1, lanes) + "\n";
        fn += "  " + r + " = sext <" + std::to_string(lanes->count) + " x i1> " + cmp + " to " + ll(lanes) + "\n";
        return asInt(r, lanes);
      }

      // --- shifts of 16- and 64-bit lanes by a count (the hardware's: a count past the width gives 0, or the sign) ---
      if ((name == "_mm_slli_epi16" || name == "_mm_srli_epi16" || name == "_mm_srai_epi16" || name == "_mm_slli_epi64" || name == "_mm_srli_epi64"))
      {
        need(2);
        std::string kind = name.substr(4, 4) == "slli" ? "pslli" : name.substr(4, 4) == "srli" ? "psrli" : "psrai";
        std::string intrinsic = "llvm.x86.sse2." + kind + (lanes == v8i16 ? ".w" : ".q");
        std::string i = sseIntrinsic(intrinsic, ll(lanes), ll(lanes) + ", i32");
        return asInt(sseCall(i, ll(lanes), ll(lanes) + " " + arg(0, lanes) + ", i32 " + sseScalar(c->args[1], types.intType(32), c)), lanes);
      }
      if (name == "_mm_slli_si128" || name == "_mm_bslli_si128")
      {
        need(2);
        int64_t shift = constArg(1);
        std::string mask = "<16 x i32> <";
        for (int i = 0; i < 16; ++i)
          mask += (i ? ", i32 " : "i32 ") + std::to_string(i - shift >= 0 && shift <= 15 ? 16 + (i - shift) : 0); // zero vector first, then the bytes moved up
        mask += ">";
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v16i8) + " zeroinitializer, " + ll(v16i8) + " " + arg(0, v16i8) + ", " + mask + "\n";
        return asInt(r, v16i8);
      }

      // --- shuffles and unpacks on 8- and 16-bit lanes ---
      if (name == "_mm_shufflelo_epi16" || name == "_mm_shufflehi_epi16")
      {
        need(2);
        int64_t imm = constArg(1);
        bool high = name == "_mm_shufflehi_epi16";
        std::string mask = "<8 x i32> <";
        for (int i = 0; i < 8; ++i)
        {
          int lane = i;
          if (high == (i >= 4))
            lane = (high ? 4 : 0) + int((imm >> (2 * (i % 4))) & 3);
          mask += (i ? ", i32 " : "i32 ") + std::to_string(lane);
        }
        mask += ">";
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v8i16) + " " + arg(0, v8i16) + ", " + ll(v8i16) + " undef, " + mask + "\n";
        return asInt(r, v8i16);
      }
      if ((name.starts_with("_mm_unpacklo_ep") || name.starts_with("_mm_unpackhi_ep")) && (lanes == v16i8 || lanes == v8i16))
      {
        need(2);
        bool high = name.starts_with("_mm_unpackhi");
        uint32_t n = lanes->count, start = high ? n / 2 : 0;
        std::string mask = "<" + std::to_string(n) + " x i32> <";
        for (uint32_t i = 0; i < n / 2; ++i)
          mask += (i ? ", i32 " : "i32 ") + std::to_string(start + i) + ", i32 " + std::to_string(n + start + i);
        mask += ">";
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(lanes) + " " + arg(0, lanes) + ", " + ll(lanes) + " " + arg(1, lanes) + ", " + mask + "\n";
        return asInt(r, lanes);
      }
      if (name == "_mm_insert_epi16")
      {
        need(3);
        std::string r = newTemp();
        fn += "  " + r + " = insertelement " + ll(v8i16) + " " + arg(0, v8i16) + ", i16 " + sseScalar(c->args[1], types.intType(16), c) + ", i32 " +
              std::to_string(constArg(2) & 7) + "\n";
        return asInt(r, v8i16);
      }
      if (name == "_mm_extract_epi16")
      {
        need(2);
        std::string lane = extract(v8i16, arg(0, v8i16), int(constArg(1) & 7)), r = newTemp();
        fn += "  " + r + " = zext i16 " + lane + " to i32\n";
        return Value{r, types.intType(32)};
      }

      // --- conversions ---
      if (name == "_mm_cvtsi128_si32")
      {
        need(1);
        return Value{extract(v4i, arg(0, v4i), 0), types.intType(32)};
      }
      if (name == "_mm_cvtsi128_si64" || name == "_mm_cvtsi128_si64x")
      {
        need(1);
        return Value{extract(v2i64, arg(0, v2i64), 0), types.intType(64)};
      }
      if (name == "_mm_cvtsi32_si128")
      {
        need(1);
        return asInt(lowLane(v4i, sseScalar(c->args[0], types.intType(32), c)), v4i);
      }
      if (name == "_mm_cvtsi64_si128" || name == "_mm_cvtsi64x_si128")
      {
        need(1);
        return asInt(lowLane(v2i64, sseScalar(c->args[0], types.intType(64), c)), v2i64);
      }
      if (name == "_mm_cvtss_f32")
      {
        need(1);
        return Value{extract(v4f, arg(0, v4f), 0), f32};
      }
      if (name == "_mm_cvtsd_f64")
      {
        need(1);
        return Value{extract(v2d, arg(0, v2d), 0), f64};
      }
      if (name == "_mm_cvttps_epi32" || name == "_mm_cvtpd_epi32" || name == "_mm_cvttpd_epi32")
      {
        need(1);
        const Ty* in = name == "_mm_cvttps_epi32" ? v4f : v2d;
        std::string intrinsic = name == "_mm_cvttps_epi32" ? "llvm.x86.sse2.cvttps2dq" : name == "_mm_cvtpd_epi32" ? "llvm.x86.sse2.cvtpd2dq" : "llvm.x86.sse2.cvttpd2dq";
        std::string i = sseIntrinsic(intrinsic, ll(v4i), ll(in));
        return asInt(sseCall(i, ll(v4i), ll(in) + " " + arg(0, in)), v4i);
      }
      if (name == "_mm_cvtepi32_pd")
      {
        need(1);
        std::string low = newTemp(), r = newTemp();
        fn += "  " + low + " = shufflevector " + ll(v4i) + " " + arg(0, v4i) + ", " + ll(v4i) + " undef, <2 x i32> <i32 0, i32 1>\n";
        fn += "  " + r + " = sitofp <2 x i32> " + low + " to " + ll(v2d) + "\n";
        return sseResult(r, v2d, c);
      }

      // --- set ---
      if (name == "_mm_setzero_ps")
        return sseResult("zeroinitializer", v4f, c);
      if (name == "_mm_setzero_pd")
        return sseResult("zeroinitializer", v2d, c);
      if ((name == "_mm_set1_epi8" || name == "_mm_set1_epi16" || name == "_mm_set1_epi64x") && lanes)
      {
        need(1);
        return asInt(splat(lanes), lanes);
      }
      if (name == "_mm_set1_ps1" || name == "_mm_set_ps1")
      {
        need(1);
        return sseResult(splat(v4f), v4f, c);
      }
      if (name == "_mm_set_ss" || name == "_mm_set_sd")
      {
        need(1);
        const Ty* vt = name == "_mm_set_ss" ? v4f : v2d;
        return sseResult(lowLane(vt, sseScalar(c->args[0], laneScalar(vt), c)), vt, c);
      }
      if (name == "_mm_setr_ps" || name == "_mm_setr_pd" || name == "_mm_setr_epi32" || name == "_mm_setr_epi16" || name == "_mm_setr_epi8" ||
          name == "_mm_set_epi16" || name == "_mm_set_epi8")
      {
        const Ty* vt = name == "_mm_setr_ps" ? v4f : name == "_mm_setr_pd" ? v2d : lanes;
        need(vt->count);
        std::vector<size_t> order(vt->count);
        for (size_t i = 0; i < order.size(); ++i)
          order[i] = name.starts_with("_mm_setr") ? i : order.size() - 1 - i; // _mm_set_ lists the highest lane first
        std::string v = build(vt, order);
        return vt->inner->kind == TyKind::Float ? sseResult(v, vt, c) : asInt(v, vt);
      }

      // --- loads and stores ---
      if (name == "_mm_load_ps" || name == "_mm_load_pd" || name == "_mm_load_si128")
      {
        need(1);
        const Ty* vt = name == "_mm_load_ps" ? v4f : name == "_mm_load_pd" ? v2d : v4i;
        std::string r = newTemp();
        fn += "  " + r + " = load " + ll(vt) + ", ptr " + rvalue(c->args[0]).text + ", align 16\n";
        return sseResult(r, vt, c);
      }
      if (name == "_mm_load_ss" || name == "_mm_load_sd" || name == "_mm_loadl_epi64" || name == "_mm_loadu_si32" || name == "_mm_loadu_si64")
      {
        need(1);
        const Ty* vt = name == "_mm_load_ss" ? v4f : name == "_mm_load_sd" ? v2d : name == "_mm_loadu_si32" ? v4i : v2i64;
        std::string s = newTemp();
        fn += "  " + s + " = load " + ll(laneScalar(vt)) + ", ptr " + rvalue(c->args[0]).text + ", align 1\n";
        std::string v = lowLane(vt, s);
        return vt->inner->kind == TyKind::Float ? sseResult(v, vt, c) : asInt(v, vt);
      }
      if (name == "_mm_store_ps" || name == "_mm_store_pd" || name == "_mm_store_si128" || name == "_mm_storeu_si128")
      {
        need(2);
        const Ty* vt = name == "_mm_store_ps" ? v4f : name == "_mm_store_pd" ? v2d : v4i;
        std::string p = rvalue(c->args[0]).text, v = arg(1, vt);
        fn += "  store " + ll(vt) + " " + v + ", ptr " + p + (name == "_mm_storeu_si128" ? ", align 1\n" : ", align 16\n");
        return Value{"", types.voidType(), false};
      }
      if (name == "_mm_store_ss" || name == "_mm_store_sd" || name == "_mm_storel_epi64" || name == "_mm_storeu_si32" || name == "_mm_storeu_si64")
      {
        need(2);
        const Ty* vt = name == "_mm_store_ss" ? v4f : name == "_mm_store_sd" ? v2d : name == "_mm_storeu_si32" ? v4i : v2i64;
        std::string p = rvalue(c->args[0]).text, lane = extract(vt, arg(1, vt), 0);
        fn += "  store " + ll(laneScalar(vt)) + " " + lane + ", ptr " + p + ", align 1\n";
        return Value{"", types.voidType(), false};
      }

      // --- the lowest lane only (the others from the first operand) ---
      if (name == "_mm_add_ss" || name == "_mm_sub_ss" || name == "_mm_mul_ss" || name == "_mm_div_ss" ||
          name == "_mm_add_sd" || name == "_mm_sub_sd" || name == "_mm_mul_sd" || name == "_mm_div_sd")
      {
        need(2);
        const Ty* vt = suffix == "ss" ? v4f : v2d;
        std::string a = arg(0, vt), b = arg(1, vt);
        std::string x = extract(vt, a, 0), y = extract(vt, b, 0);
        std::string op = name.substr(4, 3) == "add" ? "fadd" : name.substr(4, 3) == "sub" ? "fsub" : name.substr(4, 3) == "mul" ? "fmul" : "fdiv";
        std::string v = sseBinOp(op, ll(laneScalar(vt)), x, y), r = newTemp();
        fn += "  " + r + " = insertelement " + ll(vt) + " " + a + ", " + ll(laneScalar(vt)) + " " + v + ", i32 0\n";
        return sseResult(r, vt, c);
      }
      if (name == "_mm_max_ss" || name == "_mm_min_ss" || name == "_mm_max_sd" || name == "_mm_min_sd")
      {
        need(2);
        const Ty* vt = suffix == "ss" ? v4f : v2d;
        std::string intrinsic = std::string("llvm.x86.") + (suffix == "ss" ? "sse." : "sse2.") + name.substr(4, 3) + "." + suffix;
        return sseResult(call2(intrinsic, vt, vt, arg(0, vt), arg(1, vt)), vt, c);
      }
      if (name == "_mm_rcp_ss" || name == "_mm_rsqrt_ss")
      {
        need(1);
        std::string intrinsic = "llvm.x86.sse." + name.substr(4, name.size() - 7) + ".ss";
        std::string i = sseIntrinsic(intrinsic, ll(v4f), ll(v4f));
        return sseResult(sseCall(i, ll(v4f), ll(v4f) + " " + arg(0, v4f)), v4f, c);
      }

      // --- the control register and cache hints ---
      if (name == "_mm_getcsr")
      {
        std::string slot = "%v" + std::to_string(temp++), r = newTemp();
        allocas += "  " + slot + " = alloca i32\n";
        fn += "  call void " + sseIntrinsic("llvm.x86.sse.stmxcsr", "void", "ptr") + "(ptr " + slot + ")\n";
        fn += "  " + r + " = load i32, ptr " + slot + "\n";
        return Value{r, types.intType(32, true)};
      }
      if (name == "_mm_prefetch")
      {
        need(2);
        int64_t hint = constArg(1); // _MM_HINT_T0 3 ... _MM_HINT_NTA 0: the locality
        std::string i = sseIntrinsic("llvm.prefetch.p0", "void", "ptr, i32, i32, i32");
        fn += "  call void " + i + "(ptr " + convert(rvalue(c->args[0]), types.pointer(types.voidType()), c).text + ", i32 0, i32 " +
              std::to_string(hint & 3) + ", i32 1)\n";
        return Value{"", types.voidType(), false};
      }
      return std::nullopt; // not an SSE operation the code generator has
    }

    // MSVC's intrin.h functions the game calls that are instructions: _BitScanReverse and
    // _BitScanForward (32 and 64 bits), __cpuid and __cpuidex. Nothing for other names.
    std::optional<Value> msvcBuiltin(NameExpr* n, CallExpr* c)
    {
      if (n->name.parts.size() != 1)
        return std::nullopt;
      std::string name = sym(n->name.parts[0].symbol);
      if (name == "_alloca" || name == "alloca")
      {
        // Stack memory for the rest of the function (StackWalker's buffers): an alloca where the
        // call is, aligned as malloc's (MSVC's _alloca gives 16).
        if (c->args.size() != 1)
          unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
        std::string size = convert(rvalue(c->args[0]), types.intType(64, true), c).text, r = newTemp();
        fn += "  " + r + " = alloca i8, i64 " + size + ", align 16\n";
        return Value{r, types.pointer(types.voidType()), false};
      }
      if (name == "_BitScanReverse" || name == "_BitScanForward" || name == "_BitScanReverse64" || name == "_BitScanForward64")
      {
        if (c->args.size() != 2)
          unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
        bool wide = name.ends_with("64"), reverse = name.starts_with("_BitScanReverse");
        std::string iN = wide ? "i64" : "i32";
        std::string index = convert(rvalue(c->args[0]), types.pointer(types.intType(32, true)), c).text;
        std::string mask = convert(rvalue(c->args[1]), types.intType(wide ? 64 : 32, true), c).text;
        std::string count = reverse ? "llvm.ctlz." + iN : "llvm.cttz." + iN;
        if (declaredNames.claim("@" + count))
          decls += "declare " + iN + " @" + count + "(" + iN + ", i1)\n";
        std::string zero = newTemp(), bits = newTemp(), at = newTemp(), at32 = newTemp(), found = newTemp(), r = newTemp();
        fn += "  " + zero + " = icmp eq " + iN + " " + mask + ", 0\n";
        fn += "  " + bits + " = call " + iN + " @" + count + "(" + iN + " " + mask + ", i1 false)\n";
        if (reverse)
          fn += "  " + at + " = sub " + iN + " " + (wide ? "63" : "31") + ", " + bits + "\n";
        else
          fn += "  " + at + " = add " + iN + " " + bits + ", 0\n";
        fn += "  " + at32 + " = " + (wide ? "trunc i64 " : "add i32 ") + at + (wide ? " to i32\n" : ", 0\n");
        // With mask 0 *index is left as it was (MSVC calls it undefined; the instruction leaves it).
        std::string old = newTemp(), kept = newTemp();
        fn += "  " + old + " = load i32, ptr " + index + "\n";
        fn += "  " + kept + " = select i1 " + zero + ", i32 " + old + ", i32 " + at32 + "\n";
        fn += "  store i32 " + kept + ", ptr " + index + "\n";
        fn += "  " + found + " = xor i1 " + zero + ", true\n";
        fn += "  " + r + " = zext i1 " + found + " to i8\n";
        return Value{r, types.intType(8, true)};
      }
      if (name == "__cpuid" || name == "__cpuidex")
      {
        if (c->args.size() != (name == "__cpuid" ? 2u : 3u))
          unsupported(c, name + " with " + std::to_string(c->args.size()) + " arguments");
        std::string info = convert(rvalue(c->args[0]), types.pointer(types.intType(32)), c).text;
        std::string leaf = convert(rvalue(c->args[1]), types.intType(32), c).text;
        std::string sub = name == "__cpuidex" ? convert(rvalue(c->args[2]), types.intType(32), c).text : "0";
        std::string regs = newTemp();
        fn += "  " + regs + " = call { i32, i32, i32, i32 } asm sideeffect \"cpuid\", \"={ax},={bx},={cx},={dx},{ax},{cx},~{dirflag},~{fpsr},~{flags}\"(i32 " +
              leaf + ", i32 " + sub + ")\n";
        for (int i = 0; i < 4; ++i)
        {
          std::string v = newTemp(), slot = newTemp();
          fn += "  " + v + " = extractvalue { i32, i32, i32, i32 } " + regs + ", " + std::to_string(i) + "\n";
          fn += "  " + slot + " = getelementptr inbounds i32, ptr " + info + ", i64 " + std::to_string(i) + "\n";
          fn += "  store i32 " + v + ", ptr " + slot + "\n";
        }
        return Value{"", types.voidType(), false};
      }
      return std::nullopt;
    }

    // The pointer an `->` applies to: the value itself, or what the class's operator-> returns
    // (repeatedly, for wrappers of wrappers).
    Value arrowOperand(Value p, Node* at)
    {
      for (int depth = 0; depth < 8 && isClass(p.ty); ++depth)
      {
        const Ty* foundIn = nullptr;
        if (!findMemberFunction(p.ty, "operator->", {}, p.ty, at, &foundIn))
          break;
        p = callRvalue(operatorCall(p, "operator->", {}, at, true), at);
      }
      return p;
    }

    // The instantiated base type of `derived` whose definition is `baseDecl`, or null.
    const Ty* baseInstantiation(const Ty* derived, Decl* baseDecl, Node* at)
    {
      ClassDecl* target = classDef(baseDecl, at);
      StructInfo& info = structFor(derived, at);
      std::vector<const Ty*> candidates;
      for (auto& [baseDef, index] : info.bases)
        candidates.push_back(info.fieldTypes[index]);
      for (const Ty* b : candidates)
      {
        if (static_cast<ClassDecl*>(canonicalClass(b, at)->decl) == target)
          return canonicalClass(b, at);
        if (const Ty* deeper = baseInstantiation(b, baseDecl, at))
          return deeper;
      }
      return nullptr;
    }

    // Value of a call result for use as an rvalue (reference results are loaded).
    Value callRvalue(Value v, Node* at)
    {
      if (v.ty && v.ty->isReference())
      {
        if (isClass(types.stripRef(v.ty)))
          return {v.text, types.unqualified(types.stripRef(v.ty)), false};
        return load(v.text, v.ty, at);
      }
      return v;
    }

    Value rvalue(Expr* e)
    {
      if (++recursionDepth > 128)
      {
        recursionDepth = 0;
        fail(e, "expression nested too deeply");
      }
      struct DepthGuard { int& d; ~DepthGuard() { --d; } } dg{recursionDepth};
      switch (e->kind)
      {
        case NodeKind::LiteralExpr:
        {
          auto* l = static_cast<LiteralExpr*>(e);
          const Ty* t = e->ty;
          if (l->literalKind == TokenKind::String)
            return {stringConstant(l), t};
          if (l->literalKind == TokenKind::Keyword)
            return {l->keyword == Kw::nullptr_ ? "null" : l->keyword == Kw::true_ ? "true" : "false", t};
          if (t->kind == TyKind::Float)
          {
            std::string digits;
            for (char c : literalSpelling(l, l->token))
              if (c != '\'' && c != 'f' && c != 'F' && c != 'l' && c != 'L')
                digits += c;
            double d = strtod(digits.c_str(), nullptr);
            // LLVM wants the exact value: the bits of the double (rounded through float first for f32).
            double exact = t->bits == 32 ? double(float(d)) : d;
            uint64_t bits;
            memcpy(&bits, &exact, sizeof bits);
            char buf[32];
            snprintf(buf, sizeof buf, "0x%016llX", (unsigned long long)bits);
            return {buf, t};
          }
          auto v = evalConst(e);
          if (!v)
            unsupported(e, "this literal");
          return {std::to_string(*v), t};
        }
        case NodeKind::NameExpr:
        {
          auto* n = static_cast<NameExpr*>(e);
          Decl* d = n->resolved;
          if (!d)
            if (std::optional<int64_t> c = evalConst(e)) // a builtin trait used as a value
              return {e->ty && e->ty->kind == TyKind::Bool ? std::string(*c ? "true" : "false") : std::to_string(*c), e->ty};
          if (d && d->kind == NodeKind::EnumeratorDecl)
            return {std::to_string(enumeratorValue(static_cast<EnumeratorDecl*>(d))), e->ty};
          if (d && d->kind == NodeKind::TemplateParamDecl)
          {
            // A non-type template parameter is a constant of the instantiation; a function given
            // for `float (*func)(float, float)` is that function's address.
            if (currentArgs)
              for (auto& [p, bound] : currentArgs->pairs)
                if (p == d && bound && bound->kind == TyKind::Value && bound->decl && bound->decl->kind == NodeKind::FunctionDecl)
                  return {useFunction(static_cast<FunctionDecl*>(bound->decl), {}, nullptr), bound->inner};
            std::optional<int64_t> v = evalConst(e);
            const Ty* t = e->ty ? types.stripRef(e->ty) : nullptr;
            if (t && t->kind == TyKind::Value && t->inner)
              t = t->inner;
            if (!v || !t || !t->isKnown())
              unsupported(e, "a template parameter whose value is not known here");
            if (t->kind == TyKind::Pointer && *v == 0)
              return {"null", t}; // `fold = nullptr` for a function pointer parameter
            return {t->kind == TyKind::Bool ? std::string(*v ? "true" : "false") : std::to_string(*v), t};
          }
          if (d && d->kind == NodeKind::FunctionDecl)
          {
            auto* f = static_cast<FunctionDecl*>(d);
            if (f->templateParams.empty() && !global.isTemplateContext(f))
              return {useFunction(f, {}, nullptr), types.pointer(declType(d))};
            // `&FunctionImpl<R, Args...>::call<F>`: a member of an instantiation, with explicit arguments.
            TemplateArgs fargs;
            const Ty* classTy = nullptr;
            if (global.owningClass(f))
            {
              if (n->name.parts.size() > 1)
              {
                NamedType qualifier;
                qualifier.kind = NodeKind::NamedType;
                qualifier.token = n->token;
                qualifier.name = n->name;
                qualifier.name.parts.count -= 1;
                TemplateArgs none;
                const Ty* q = global.typeOfAstIn(&qualifier, typingContext(), currentArgs ? *currentArgs : none);
                if (q && isClass(q))
                  classTy = canonicalClass(q, e);
              }
              else if (currentClassTy)
                classTy = canonicalClass(currentClassTy, e);
              // `Expression::computeConstant` with Expression deriving from the class template
              // instance that declares it (ComplexExpressionWithOperation<QuickMultioctaveNoise, 2,
              // 10>): that instance's member, with its arguments.
              if (classTy)
                if (const Ty* declaring = instantiationIn(classTy, global.owningClass(f), e); declaring && !declaring->args.empty())
                  classTy = declaring;
              if (!classTy)
                unsupported(e, "taking the address of a member of an unknown instantiation");
              fargs = global.classArgsOf(classTy);
            }
            const NamePart& last = n->name.last();
            size_t given = 0;
            // A trailing pack takes the rest (`getOrCreate<Expression, const NoiseExpression&, const
            // NoiseExpression&>`), and is empty when nothing is left for it.
            bool packLast = !f->templateParams.empty() && static_cast<TemplateParamDecl*>(f->templateParams[f->templateParams.size() - 1])->isPack;
            std::vector<const Ty*> packElements;
            for (size_t i = 0; i < last.templateArgs.size(); ++i)
            {
              Node* arg = last.templateArgs[i];
              TemplateArgs none;
              const Ty* t = nullptr;
              if (uint8_t(arg->kind) > uint8_t(NodeKind::PackType))
              {
                // `checkArray<16>`: a constant argument, its parameter's type.
                Decl* param = i < f->templateParams.size() ? f->templateParams[i] : nullptr;
                std::optional<int64_t> v = evalConst(static_cast<Expr*>(arg));
                if (!v)
                  unsupported(e, "a constant argument the compiler could not evaluate in the address of a function template");
                t = types.valueType(valueParamType(param), *v);
              }
              else
                t = global.typeOfAstIn(static_cast<Type*>(arg), typingContext(), currentArgs ? *currentArgs : none);
              if (packLast && i + 1 >= f->templateParams.size())
              {
                if (t && t->kind == TyKind::Pack)
                  packElements.insert(packElements.end(), t->args.begin(), t->args.end()); // `Args...` of the enclosing instance
                else
                  packElements.push_back(t);
                continue;
              }
              if (i >= f->templateParams.size())
                break;
              fargs.pairs.push_back({f->templateParams[i], t});
              ++given;
            }
            // The checker typed the name as the instance a function pointer of a known type wants
            // (`construct<Neg>` for a `const Expr& (*)(const Expr&)`, the trailing pack deduced
            // from its parameters): the arguments not written, deduced the same way.
            const Ty* typed = e->ty ? withCurrentArgs(types.unqualified(types.stripRef(e->ty))) : nullptr;
            if (typed && typed->kind == TyKind::Pointer)
              typed = typed->inner;
            if (given < f->templateParams.size() && packElements.empty() && typed && typed->kind == TyKind::Function && typed->isKnown() &&
                !types.isDependent(typed))
            {
              std::vector<const Ty*> paramTypes(typed->args.begin(), typed->args.end());
              global.deduceArgs(f, paramTypes, fargs);
              given = 0;
              for (Decl* p : f->templateParams)
                for (auto& [d, t] : fargs.pairs)
                  if (d == p)
                  {
                    ++given;
                    break;
                  }
            }
            if (packLast && given + 1 == f->templateParams.size())
            {
              fargs.pairs.push_back({f->templateParams[f->templateParams.size() - 1], types.pack(Span<const Ty*>{packElements.data(), uint32_t(packElements.size())})});
              ++given;
            }
            if (given < f->templateParams.size())
              unsupported(e, "taking the address of a function template without all its arguments");
            std::string name = useFunction(f, fargs, classTy);
            ArgsScope scope(*this, &fargs);
            return {name, types.pointer(declType(f))};
          }
          if (d && d->kind == NodeKind::VariableDecl && !locals.count(d))
          {
            const Ty* t = types.stripRef(declType(d));
            if (!d->templateParams.empty())
            {
              // A variable template: its value for these arguments, a constant (`pi_v<double>` a
              // floating one), of the type the use has; while that is still the template's (`T`),
              // of the type the evaluator gave the value, the declared one for these arguments.
              auto usable = [this](const Ty* x) { return x && x->isKnown() && !types.isDependent(x) ? types.unqualified(types.stripRef(x)) : nullptr; };
              const Ty* vt = usable(e->ty) ? usable(e->ty) : usable(t);
              if (std::optional<ConstValue> c = foldAs(foldConst(e), vt); c && c->isScalar())
              {
                const Ty* ct = vt ? vt : usable(c->type);
                if (c->isFloat() || (ct && ct->kind == TyKind::Float))
                {
                  const Ty* ft = ct && ct->kind == TyKind::Float ? ct : types.floatType(64);
                  return {floatConstant(c->isFloat() ? c->f : double(c->i), ft->bits), ft};
                }
                return {ct && ct->kind == TyKind::Bool ? (c->i ? "true" : "false") : std::to_string(c->i), ct ? ct : types.intType()};
              }
              if (getenv("KPP_DEBUG_VT"))
                fprintf(stderr, "DEBUG vt file=%s ctx=%s args=%d constContext=%s\n", currentFile ? currentFile->path.c_str() : "null",
                        typingContext() ? nodeKindName(typingContext()->kind) : "null", currentArgs ? int(currentArgs->pairs.size()) : -1,
                        constContext ? nodeKindName(constContext->kind) : "null");
              unsupported(e, "a variable template whose value is not a constant");
            }
            if (t && t->isConst && (t->kind == TyKind::Int || t->kind == TyKind::Enum || t->kind == TyKind::Char || t->kind == TyKind::Bool))
              if (auto c = evalConst(e))
                return {t->kind == TyKind::Bool ? (*c ? "true" : "false") : std::to_string(*c), types.unqualified(t)};
          }
          std::string addr = lvalue(e);
          const Ty* type = e->ty;
          if ((!type || !type->isKnown()) && d)
            if (auto local = locals.find(d); local != locals.end() && local->second.ty && local->second.ty->isKnown())
              type = local->second.ty; // `auto xmm0 = _mm_loadu_si128(...)`: the type the code generator gave the local
          return load(addr, type ? type : (d ? declType(d) : nullptr), e);
        }
        case NodeKind::ThisExpr:
        {
          std::string p = thisPointer(e);
          const Ty* t = types.stripRef(e->ty);
          if (t && t->kind == TyKind::Enum)
          {
            std::string r = newTemp(); // in an enum's methods `this` is the value
            fn += "  " + r + " = load " + llType(t, e) + ", ptr " + p + "\n";
            return {r, t};
          }
          return {p, e->ty};
        }
        case NodeKind::ParenExpr: return rvalue(static_cast<ParenExpr*>(e)->inner);
        case NodeKind::MemberExpr:
        {
          auto* m = static_cast<MemberExpr*>(e);
          if (m->resolved && m->resolved->kind == NodeKind::EnumeratorDecl)
            return {std::to_string(enumeratorValue(static_cast<EnumeratorDecl*>(m->resolved))), e->ty};
          if (const Ty* bt = m->base->ty ? types.stripRef(m->base->ty) : nullptr; bt && bt->kind == TyKind::Member && !m->dynamicMember)
          {
            // A reflection descriptor: m.name is the member's name, m.offset its offset in the class;
            // an alternative's (alternatives(U)) a.name is the alternative's, a.tag its tag value.
            auto* field = static_cast<VariableDecl*>(bt->decl);
            std::string which = m->member.empty() ? std::string() : sym(m->member.last().symbol);
            if (ClassDecl* u = bt->args.empty() ? nullptr : typedUnionOf(bt->args[0], e))
              if (int64_t index = descriptorAlternative(u, bt); index >= 0)
              {
                auto* en = static_cast<EnumeratorDecl*>(static_cast<EnumDecl*>(u->tagEnum)->enumerators[size_t(index)]);
                if (which == "name")
                  return {cStringConstant(sym(en->name)), e->ty};
                if (which == "tag")
                  return {std::to_string(enumeratorValue(en)), tagType(u)};
                unsupported(e, "an alternative descriptor has name, type and tag only");
              }
            if (which == "name")
              return {cStringConstant(field->name.empty() ? std::string() : sym(field->name.last().symbol)), e->ty};
            if (which == "offset")
            {
              const Ty* cls = bt->args.empty() ? nullptr : bt->args[0];
              std::string addr;
              if (!cls || !fieldAddress(cls, field, "null", addr, e))
                unsupported(e, "the offset of a member that is not a field");
              std::string r = newTemp();
              fn += "  " + r + " = ptrtoint ptr " + addr + " to i64\n";
              return {r, e->ty};
            }
            unsupported(e, "a member descriptor has name and offset only");
          }
          std::string addr = lvalue(e);
          // Use the field's declared type when the expression type is unknown.
          const Ty* memberTy = e->ty;
          const Ty* memberTyStripped = memberTy ? types.stripRef(memberTy) : nullptr;
          if ((!memberTyStripped || !memberTyStripped->isKnown()) && m->resolved && m->resolved->kind == NodeKind::VariableDecl)
            memberTy = declType(m->resolved);
          memberTyStripped = memberTy ? types.stripRef(memberTy) : nullptr;
          if ((!memberTyStripped || !memberTyStripped->isKnown()) && !m->member.empty())
          {
            // Try finding the field by name for its declared type.
            const Ty* bt = m->base->ty ? types.stripRef(m->base->ty) : nullptr;
            if (m->op == P::Arrow && bt && bt->kind == TyKind::Pointer)
              bt = types.stripRef(bt->inner);
            if (bt && isClass(bt))
              if (Decl* fd = findFieldByName(bt, m->member.last().symbol, e))
                memberTy = declType(fd);
          }
          if (const BitfieldInfo* bits = m->resolved ? bitfieldFor(m->resolved) : nullptr)
            return loadBitfield(addr, *bits, types.stripRef(memberTy), e);
          return load(addr, memberTy, e);
        }
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(e);
          Value base = withCheckedConst(rvalue(i->base), i->base);
          if (ClassDecl* c = typedUnionOf(base.ty, e))
          {
            std::string addr = alternativeLvalue(c, base, i, e, true);
            const Ty* altTy = types.stripRef(e->ty);
            return isClass(altTy) ? Value{addr, altTy, false} : load(addr, altTy, e);
          }
          if (isClass(base.ty))
            return callRvalue(operatorCall(base, operatorName(P::LBracket), i->args, e, true), e);
          Value index = convert(rvalue(i->args[0]), types.intType(64), e);
          if (base.ty->kind != TyKind::Pointer)
            unsupported(e, "indexing a non-pointer");
          std::string r = newTemp();
          fn += "  " + r + " = getelementptr inbounds " + llType(base.ty->inner, e) + ", ptr " + base.text + ", i64 " + index.text + "\n";
          return load(r, base.ty->inner, e);
        }
        case NodeKind::CallExpr: return callRvalue(callValue(static_cast<CallExpr*>(e), ""), e);
        case NodeKind::TypeidExpr:
        {
          auto* t = static_cast<TypeidExpr*>(e);
          const Ty* infoTy = e->ty ? types.stripRef(e->ty) : nullptr;
          if (!isClass(infoTy))
            unsupported(e, "typeid without the library's TypeInfo class");
          std::string namePtr;
          const Ty* staticTy = nullptr;
          if (t->operandDecl)
          {
            // typeid(q): the parser read the variable as a type name; the resolver found it.
            const Ty* xt = t->operandType ? types.stripRef(t->operandType) : nullptr;
            if (isClass(xt) && structFor(xt, e).polymorphic)
              namePtr = descriptorName(loadSlot(addressOfDecl(t->operandDecl, e), -1));
            else
              staticTy = xt;
          }
          else if (t->operand && uint8_t(t->operand->kind) <= uint8_t(NodeKind::PackType))
            staticTy = t->operandType ? types.stripRef(t->operandType) : nullptr; // typeid(Widget): typed by the resolver
          else if (t->operand)
          {
            auto* x = static_cast<Expr*>(t->operand);
            Decl* named = x->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(x)->resolved : nullptr;
            const Ty* xt = x->ty ? types.stripRef(x->ty) : nullptr;
            if (named && named->kind == NodeKind::ClassDecl)
              staticTy = canonicalClass(types.classType(named, {}), e); // typeid(Base): the parser read the type as a name
            else if (named && named->kind == NodeKind::EnumDecl)
              staticTy = types.enumType(named);
            else if (isClass(xt) && structFor(xt, e).polymorphic)
              namePtr = descriptorName(loadSlot(lvalue(x), -1)); // the dynamic type: slot -1 of the object's vtable
            else
              staticTy = xt;
          }
          if (namePtr.empty())
          {
            if (!staticTy || !staticTy->isKnown() || types.isDependent(staticTy))
              unsupported(e, "typeid of an unknown type (operand: " + std::string(t->operand ? nodeKindName(t->operand->kind) : "none") + ", " +
                                 (staticTy ? types.toString(staticTy, &symbols) : std::string("no type")) + ")");
            namePtr = cStringConstant(types.toString(types.unqualified(staticTy), &symbols));
          }
          std::string slot = newObjectSlot(infoTy, e);
          const StructInfo& info = structFor(infoTy, e);
          if (info.fieldDecls.empty())
            unsupported(e, "a TypeInfo class without its name field");
          fn += "  store ptr " + namePtr + ", ptr " + gep(info.name, slot, fieldIndexOf(info, info.fieldDecls[0])) + "\n";
          return {slot, infoTy, true};
        }
        case NodeKind::NameofExpr:
        {
          Expr* o = static_cast<NameofExpr*>(e)->operand;
          std::string text;
          if (o->kind == NodeKind::NameExpr)
            text = sym(static_cast<NameExpr*>(o)->name.last().symbol);
          else if (o->kind == NodeKind::MemberExpr)
            text = sym(static_cast<MemberExpr*>(o)->member.last().symbol);
          else
            text = std::string(spelling(o->token));
          return {cStringConstant(text), e->ty};
        }
        case NodeKind::CastExpr:
          if (e->ty && e->ty->kind == TyKind::Void)
          {
            rvalue(static_cast<CastExpr*>(e)->operand); // `(void)x`: evaluated, result dropped
            return {"", types.voidType()};
          }
          return explicitConvert(rvalue(static_cast<CastExpr*>(e)->operand), e->ty, e);
        case NodeKind::NamedCastExpr:
        {
          auto* c = static_cast<NamedCastExpr*>(e);
          if (c->castKind == Kw::dynamic_cast_)
            if (std::optional<Value> v = dynamicCast(c, e))
              return *v;
          return explicitConvert(rvalue(c->operand), e->ty, e);
        }
        case NodeKind::FunctionalCastExpr:
        {
          auto* c = static_cast<FunctionalCastExpr*>(e);
          const Ty* t = types.stripRef(e->ty);
          if (isClass(t))
          {
            std::string addr = newObjectSlot(t, e);
            directInitialize(addr, t, c->args, c->braced, e, !c->braced); // `T(list)`: the list is a constructor's argument
            statementTemps.push_back({addr, t});
            return {addr, t, true};
          }
          // `value_type{forward<Args>(args)...}` with a scalar value_type: as `T(...)` below.
          Span<Expr*> args = c->args;
          if (args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(args[0])->args.size() == 1 &&
              isPackExpansion(static_cast<BraceInitExpr*>(args[0])->args[0]))
            args = static_cast<BraceInitExpr*>(args[0])->args;
          if (args.size() == 1 && isPackExpansion(args[0]))
          {
            // `T(forward<Args>(args)...)` with a scalar T: the pack's one element, re-typed for it
            // (so that `forward<Args>` means forward<that element>).
            Expr* pattern = static_cast<UnaryExpr*>(args[0])->operand;
            size_t count = packElementCount(pattern, e);
            if (count == 0)
              return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
            if (count != 1)
              unsupported(e, "a scalar built from a pack of several elements");
            Value v;
            forEachPackElement(pattern, e, [&](size_t) { v = explicitConvert(rvalue(pattern), t, e); });
            return v;
          }
          if (c->args.size() == 1)
          {
            Expr* a = c->args[0];
            if (a->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(a)->args.empty())
              return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t}; // `mapped_type{}`: zero
            if (a->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(a)->args.size() == 1)
              return convert(rvalue(static_cast<BraceInitExpr*>(a)->args[0]), t, e, true); // `T{x}`: direct-list-initialisation, no cast
            return explicitConvert(rvalue(a), t, e); // `T(x)` is the cast `(T)x`
          }
          if (c->args.empty())
            return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
          unsupported(e, "this functional cast");
        }
        case NodeKind::RequiresExpr:
          return {static_cast<RequiresExpr*>(e)->satisfied ? "true" : "false", types.boolType()};
        case NodeKind::SizeofExpr:
        {
          auto* s = static_cast<SizeofExpr*>(e);
          if (s->isPack)
          {
            Decl* param = nullptr;
            if (s->type && s->type->kind == NodeKind::NamedType)
              param = static_cast<NamedType*>(s->type)->resolved;
            if (!param && s->operand)
              param = packIn(s->operand);
            if (param && param->kind == NodeKind::TemplateParamDecl && currentArgs)
              for (auto& [p, t] : currentArgs->pairs)
                if (p == param && t && t->kind == TyKind::Pack)
                  return {std::to_string(t->args.size()), types.sizeType()}; // `sizeof...(Args)` of a template pack
            if (!param || !packLocals.count(param))
              unsupported(e, "sizeof... of something that is not a parameter pack");
            return {std::to_string(packLocals[param].size()), types.sizeType()};
          }
          auto v = evalConst(e);
          if (!v)
            unsupported(e, "this sizeof");
          return {std::to_string(*v), types.sizeType()};
        }
        case NodeKind::FoldExpr:
        {
          auto* f = static_cast<FoldExpr*>(e);
          P op = P(f->op);
          // `(args op ...)` and `(args op ... op init)` fold to the right: E1 op (E2 op (... op init));
          // `(... op args)` and `(init op ... op args)` to the left: ((init op E1) op E2) op ...
          bool right = f->lhs && (packIn(f->lhs) || typePackIn(f->lhs));
          Expr* pattern = right ? f->lhs : f->rhs;
          Expr* init = right ? f->rhs : f->lhs;
          // A function parameter pack (`(args + ...)`), or a type pack the pattern names in a type
          // (`(func(Args()), ...)`, `(... || isSame<T, Args>)`).
          Decl* typePack = pattern && !packIn(pattern) ? typePackIn(pattern) : nullptr;
          if (!pattern || (!packIn(pattern) && !typePack))
            unsupported(e, "a fold expression that expands no parameter pack");
          // Operands in evaluation order: the elements, with init last (right fold) or first (left).
          auto eachOperand = [&](auto&& visit)
          {
            if (init && !right)
              visit(init);
            if (typePack)
              forEachTypePackElement(typePack, [&](size_t) { visit(pattern); });
            else
              forEachPackElement(pattern, e, [&](size_t) { visit(pattern); });
            if (init && right)
              visit(init);
          };
          if (op == P::Comma)
          {
            eachOperand([&](Expr* x)
            {
              rvalue(x);
              destroyTemps();
            });
            return {"", types.voidType()};
          }
          if (op == P::AndAnd || op == P::OrOr)
          {
            // Short-circuit: each operand that decides the result ends the fold (an empty pack
            // without init is true for &&, false for ||).
            bool isAnd = op == P::AndAnd;
            std::string endL = newLabel(isAnd ? "fold.and.end" : "fold.or.end");
            std::string phi;
            eachOperand([&](Expr* x)
            {
              size_t temps = statementTemps.size();
              Value v = convert(rvalue(x), types.boolType(), e);
              destroyTempsFrom(temps); // an operand runs only when the ones before did not decide
              std::string next = newLabel("fold.next");
              fn += "  br i1 " + v.text + ", label %" + (isAnd ? next : endL) + ", label %" + (isAnd ? endL : next) + "\n";
              phi += std::string(phi.empty() ? "" : ", ") + "[ " + (isAnd ? "false" : "true") + ", %" + currentBlock + " ]";
              startBlock(next);
            });
            phi += std::string(phi.empty() ? "" : ", ") + "[ " + (isAnd ? "true" : "false") + ", %" + currentBlock + " ]";
            emitBranch(endL);
            startBlock(endL);
            std::string r = newTemp();
            fn += "  " + r + " = phi i1 " + phi + "\n";
            return {r, types.boolType()};
          }
          std::vector<Value> values;
          eachOperand([&](Expr* x) { values.push_back(rvalue(x)); });
          if (values.empty())
            unsupported(e, "a fold over an empty pack without an initial value");
          auto combine = [&](const Value& a, const Value& b)
          {
            if (op == P::EqEq || op == P::NotEq || op == P::Less || op == P::LessEq || op == P::Greater || op == P::GreaterEq)
              return compare(op, a, b, e);
            return arithmetic(op, a, b, e);
          };
          Value acc = right ? values.back() : values.front();
          if (right)
            for (size_t i = values.size() - 1; i-- > 0;)
              acc = combine(values[i], acc);
          else
            for (size_t i = 1; i < values.size(); ++i)
              acc = combine(acc, values[i]);
          return acc;
        }
        case NodeKind::UnaryExpr: return unary(static_cast<UnaryExpr*>(e));
        case NodeKind::BinaryExpr: return binary(static_cast<BinaryExpr*>(e));
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          if (!c->then)
            unsupported(e, "the ?: elvis operator");
          const Ty* t = types.stripRef(e->ty);
          Value cond = convert(rvalue(c->cond), types.boolType(), e);
          std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
          fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
          const Ty* thenObject = isClass(t) ? conditionalObject(c->then, t, e) : nullptr;
          const Ty* elseObject = isClass(t) ? conditionalObject(c->else_, t, e) : nullptr;
          if (thenObject && elseObject)
          {
            // Both branches name objects (`cond ? a : b` with lvalues or references): the result
            // is one of them, not a copy.
            size_t temps = statementTemps.size();
            startBlock(tL);
            std::string a = conditionalAddress(c->then, thenObject, t, e);
            destroyTempsFrom(temps);
            std::string aEnd = currentBlock;
            emitBranch(jL);
            startBlock(eL);
            std::string b = conditionalAddress(c->else_, elseObject, t, e);
            destroyTempsFrom(temps);
            std::string bEnd = currentBlock;
            emitBranch(jL);
            startBlock(jL);
            std::string r = newTemp();
            fn += "  " + r + " = phi ptr [ " + a + ", %" + aEnd + " ], [ " + b + ", %" + bEnd + " ]\n";
            return {r, t, false};
          }
          if (isClass(t))
          {
            // Both branches build the one temporary (a copy of an lvalue, or the value itself).
            std::string slot = newObjectSlot(t, e);
            size_t temps = statementTemps.size();
            startBlock(tL);
            initializeFrom(slot, t, c->then, e);
            destroyTempsFrom(temps);
            emitBranch(jL);
            startBlock(eL);
            initializeFrom(slot, t, c->else_, e);
            destroyTempsFrom(temps);
            emitBranch(jL);
            startBlock(jL);
            if (needsDestruction(t, e))
              statementTemps.push_back({slot, t});
            return {slot, t, true};
          }
          size_t temps = statementTemps.size();
          startBlock(tL);
          Value a = t->kind == TyKind::Void ? rvalue(c->then) : convert(rvalue(c->then), t, e);
          destroyTempsFrom(temps); // `number(isNumber ? strtodouble(String(data, length)) : 0)`: the String exists only here
          std::string aEnd = currentBlock;
          emitBranch(jL);
          startBlock(eL);
          Value b = t->kind == TyKind::Void ? rvalue(c->else_) : convert(rvalue(c->else_), t, e);
          destroyTempsFrom(temps);
          std::string bEnd = currentBlock;
          emitBranch(jL);
          startBlock(jL);
          if (t->kind == TyKind::Void)
            return {"", t};
          std::string r = newTemp();
          fn += "  " + r + " = phi " + llType(t, e) + " [ " + a.text + ", %" + aEnd + " ], [ " + b.text + ", %" + bEnd + " ]\n";
          return {r, t};
        }
        case NodeKind::NewExpr:
        {
          auto* n = static_cast<NewExpr*>(e);
          const Ty* t = e->ty && e->ty->kind == TyKind::Pointer ? e->ty->inner : nullptr;
          if (!t)
            unsupported(e, "new of an unknown type");
          if (n->type && n->type->kind == NodeKind::ArrayType && static_cast<ArrayType*>(n->type)->size)
          {
            if (!n->placement.empty())
              unsupported(e, "placement new of an array");
            return {arrayNew(n, t, static_cast<ArrayType*>(n->type)->size), e->ty};
          }
          std::string r;
          if (n->placement.size() == 1)
            r = convert(rvalue(n->placement[0]), types.pointer(types.voidType()), e).text;
          else if (!n->placement.empty())
            unsupported(e, "placement new with several arguments");
          else
          {
            ensureMalloc();
            r = newTemp();
            emitCall(r + " = ", "ptr @malloc(i64 " + std::to_string(sizeOf(t, e)) + ")", false);
          }
          if (isClass(t) && n->placement.empty())
          {
            // A constructor that throws gives the memory back, as C++'s new does: freed while the
            // constructor runs if an exception leaves it, and only then.
            Object allocation{r, t};
            allocation.unwindOnly = true;
            allocation.freeMemory = true;
            statementTemps.push_back(allocation);
            size_t at = statementTemps.size() - 1;
            constructInto(r, t, n->args, e);
            statementTemps.erase(statementTemps.begin() + at);
          }
          else if (isClass(t))
            constructInto(r, t, n->args, e);
          else if (isLambda(t) && n->args.size() == 1)
            initializeFrom(r, t, n->args[0], e); // `new F(callable)` with F a lambda type: the closure, copied
          else if (n->args.size() == 1 && isPackExpansion(n->args[0]))
          {
            // `new (p) T(forward<Args>(args)...)` with a scalar T: the pack's one element.
            Expr* pattern = static_cast<UnaryExpr*>(n->args[0])->operand;
            size_t count = packElementCount(pattern, e);
            if (count == 0)
              storeZero(t, r, e);
            else if (count != 1)
              unsupported(e, "a scalar built from a pack of several elements");
            else
              forEachPackElement(pattern, e, [&](size_t) { store(rvalue(pattern), r, t, e); });
          }
          else if (!n->args.empty())
          {
            Expr* a = n->args[0];
            if (a->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(a)->args.size() == 1)
              a = static_cast<BraceInitExpr*>(a)->args[0];
            store(rvalue(a), r, t, e);
          }
          else
            storeZero(t, r, e);
          return {r, e->ty};
        }
        case NodeKind::LambdaExpr: return makeClosure(static_cast<LambdaExpr*>(e));
        case NodeKind::ThrowExpr:
          emitThrow(static_cast<ThrowExpr*>(e));
          return {"", types.voidType()};
        case NodeKind::DeleteExpr:
        {
          auto* d = static_cast<DeleteExpr*>(e);
          Value p = rvalue(d->operand);
          if (isClass(p.ty))
            if (std::optional<Value> pointer = toPointer(p, e))
              p = *pointer; // `delete image` of a ValueComparePointer: what its conversion to a pointer gives, as C++ has it
          if (p.ty->kind != TyKind::Pointer)
            unsupported(e, "delete of a non-pointer");
          ensureMalloc();
          if (d->isArray)
          {
            arrayDelete(p.text, p.ty->inner, e);
            return {"", types.voidType()};
          }
          if (isClass(p.ty->inner) && (needsDestruction(p.ty->inner, e) || structFor(p.ty->inner, e).polymorphic))
          {
            // `delete p` with p null does nothing (C++): no destructor runs on address 0.
            std::string isNull = newTemp(), objectL = newLabel("delete.object"), endL = newLabel("delete.end");
            fn += "  " + isNull + " = icmp eq ptr " + p.text + ", null\n";
            fn += "  br i1 " + isNull + ", label %" + endL + ", label %" + objectL + "\n";
            blockTerminated = true;
            startBlock(objectL);
            // Through a base with a virtual destructor the object may be of a class that has this
            // base at an offset (a second base): the block starts at the whole object, which slot -2
            // of the vtable gives (read before the destructor runs).
            std::string block = p.text;
            if (slotKeyed(structFor(p.ty->inner, e), "~") >= 0)
            {
              std::string vptr = newTemp(), slot = newTemp(), offset = newTemp(), bytes = newTemp(), back = newTemp();
              block = newTemp();
              fn += "  " + vptr + " = load ptr, ptr " + p.text + "\n";
              fn += "  " + slot + " = getelementptr inbounds ptr, ptr " + vptr + ", i64 -2\n";
              fn += "  " + offset + " = load ptr, ptr " + slot + "\n";
              fn += "  " + bytes + " = ptrtoint ptr " + offset + " to i64\n";
              fn += "  " + back + " = sub i64 0, " + bytes + "\n";
              fn += "  " + block + " = getelementptr i8, ptr " + p.text + ", i64 " + back + "\n";
            }
            destroy(p.text, p.ty->inner, e, true);
            emitCall("", "void @free(ptr " + block + ")", false);
            emitBranch(endL);
            startBlock(endL);
            return {"", types.voidType()};
          }
          if (isLambda(p.ty->inner) && needsDestruction(p.ty->inner, e))
          {
            // FunctionImpl's `delete (F*)target` of a closure: its captures by value are destroyed.
            std::string isNull = newTemp(), objectL = newLabel("delete.object"), endL = newLabel("delete.end");
            fn += "  " + isNull + " = icmp eq ptr " + p.text + ", null\n";
            fn += "  br i1 " + isNull + ", label %" + endL + ", label %" + objectL + "\n";
            blockTerminated = true;
            startBlock(objectL);
            destroy(p.text, p.ty->inner, e);
            emitCall("", "void @free(ptr " + p.text + ")", false);
            emitBranch(endL);
            startBlock(endL);
            return {"", types.voidType()};
          }
          emitCall("", "void @free(ptr " + p.text + ")", false); // free(null) does nothing
          return {"", types.voidType()};
        }
        default:
          unsupported(e, std::string("the expression kind ") + nodeKindName(e->kind));
      }
    }

    // The bit-field layout of member d (a bit field of some laid-out class), or null.
    // (Only asked after the member's class was laid out, so the answer, none included, is final:
    // remembered by this emitter.)
    std::unordered_map<Decl*, const BitfieldInfo*> bitfieldCache;
    const BitfieldInfo* bitfieldFor(Decl* d)
    {
      if (auto mine = bitfieldCache.find(d); mine != bitfieldCache.end())
        return mine->second;
      auto lock = sharedLock();
      auto it = bitfieldOf.find(d);
      const BitfieldInfo* bits = it == bitfieldOf.end() ? nullptr : &it->second; // map nodes stay put: the pointer outlives the lock
      bitfieldCache.emplace(d, bits);
      return bits;
    }

    void ensureMalloc()
    {
      auto lock = sharedLock();
      if (mallocDeclared)
        return;
      mallocDeclared = true;
      if (declaredNames.claim("@malloc"))
        decls += "declare ptr @malloc(i64)\n";
      if (declaredNames.claim("@free"))
        decls += "declare void @free(ptr)\n";
    }

    // The bytes before the elements of `new T[n]`: a cookie whose last 8 hold n, when T needs
    // destroying (as MSVC's runtime lays it out), so that delete[] destroys all n; 16 for an element
    // aligned to 16, malloc's alignment. A type that needs no destroying has none (delete[] frees).
    uint64_t arrayCookie(const Ty* t, Node* at)
    {
      if (!needsDestruction(t, at))
        return 0;
      const Ty* inner = t;
      while (inner->kind == TyKind::Array)
        inner = inner->inner;
      uint64_t align = isClass(inner) ? classSizeAndAlign(inner, at).second : sizeOf(inner, at);
      if (align > 16)
        unsupported(at, "new[] of " + types.toString(t, &symbols) + ", aligned to " + std::to_string(align) + " bytes (more than malloc's 16)");
      return align > 8 ? 16 : 8;
    }

    // A loop over the elements of type t at base with an index in an i64 slot: from first up to
    // count, or from count down to 0 (backwards: the last element first).
    template<typename F>
    void forEachElement(const std::string& base, const Ty* t, const std::string& first, const std::string& count, bool backwards, Node* at, F&& body)
    {
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i64\n";
      fn += "  store i64 " + (backwards ? count : first) + ", ptr " + slot + "\n";
      std::string loopL = newLabel("array.loop"), bodyL = newLabel("array.body"), endL = newLabel("array.end");
      emitBranch(loopL);
      startBlock(loopL);
      std::string index = newTemp(), done = newTemp();
      fn += "  " + index + " = load i64, ptr " + slot + "\n";
      fn += "  " + done + " = icmp " + (backwards ? "eq i64 " + index + ", 0" : "uge i64 " + index + ", " + count) + "\n";
      fn += "  br i1 " + done + ", label %" + endL + ", label %" + bodyL + "\n";
      startBlock(bodyL);
      std::string i = index;
      if (backwards)
      {
        i = newTemp();
        fn += "  " + i + " = sub i64 " + index + ", 1\n";
        fn += "  store i64 " + i + ", ptr " + slot + "\n";
      }
      std::string element = newTemp();
      fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + base + ", i64 " + i + "\n";
      body(element);
      if (!backwards)
      {
        std::string next = newTemp();
        fn += "  " + next + " = add i64 " + index + ", 1\n";
        fn += "  store i64 " + next + ", ptr " + slot + "\n";
      }
      emitBranch(loopL);
      startBlock(endL);
    }

    // `new T[n]`: n elements of T, each built by its default constructor in order (a scalar left as
    // it is, zero with `new T[n]()`); `new T[n]{a, b}` builds the first ones from the list and
    // value-initializes the rest. Returns the address of the first element.
    std::string arrayNew(NewExpr* n, const Ty* t, Expr* sizeExpr)
    {
      std::string count = convert(rvalue(sizeExpr), types.sizeType(), n).text;
      std::string stride = "ptrtoint (ptr getelementptr (" + llType(t, n) + ", ptr null, i32 1) to i64)"; // LLVM's own element size
      uint64_t cookie = arrayCookie(t, n);
      ensureMalloc();
      std::string bytes = newTemp();
      fn += "  " + bytes + " = mul i64 " + count + ", " + stride + "\n";
      std::string total = bytes;
      if (cookie)
      {
        total = newTemp();
        fn += "  " + total + " = add i64 " + bytes + ", " + std::to_string(cookie) + "\n";
      }
      std::string block = newTemp();
      emitCall(block + " = ", "ptr @malloc(i64 " + total + ")", false);
      std::string base = block;
      if (cookie)
      {
        base = newTemp();
        fn += "  " + base + " = getelementptr inbounds i8, ptr " + block + ", i64 " + std::to_string(cookie) + "\n";
        std::string countAddr = newTemp();
        fn += "  " + countAddr + " = getelementptr inbounds i8, ptr " + base + ", i64 -8\n";
        fn += "  store i64 " + count + ", ptr " + countAddr + "\n";
      }
      Span<Expr*> list;
      bool valueInit = n->parenthesized;
      if (n->braced)
      {
        list = static_cast<BraceInitExpr*>(n->args[0])->args;
        valueInit = true;
      }
      else if (!n->args.empty())
        unsupported(n, "new[] with constructor arguments");
      bool scalar = !isClass(t) && t->kind != TyKind::Array;
      if (scalar && valueInit)
      {
        if (declaredNames.claim("@llvm.memset.p0.i64"))
          decls += "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n";
        fn += "  call void @llvm.memset.p0.i64(ptr " + base + ", i8 0, i64 " + bytes + ", i1 false)\n";
      }
      for (size_t i = 0; i < list.size(); ++i)
      {
        std::string element = newTemp();
        fn += "  " + element + " = getelementptr inbounds " + llType(t, n) + ", ptr " + base + ", i64 " + std::to_string(i) + "\n";
        initializeFrom(element, t, list[i], n);
      }
      if (!scalar && (valueInit || needsConstruction(t, n)))
        forEachElement(base, t, std::to_string(list.size()), count, false, n, [&](const std::string& element)
        {
          if (isClass(t))
            defaultConstruct(element, t, n);
          else
            constructArray(element, t, n);
        });
      return base;
    }

    // `delete[] p`: with a cookie (arrayCookie), the n elements destroyed, the last first, then the
    // block freed; nothing for null. Without one, the block freed.
    void arrayDelete(const std::string& p, const Ty* t, Node* at)
    {
      uint64_t cookie = arrayCookie(t, at);
      if (!cookie)
      {
        emitCall("", "void @free(ptr " + p + ")", false); // free(null) does nothing
        return;
      }
      std::string isNull = newTemp(), arrayL = newLabel("delete.array"), endL = newLabel("delete.end");
      fn += "  " + isNull + " = icmp eq ptr " + p + ", null\n";
      fn += "  br i1 " + isNull + ", label %" + endL + ", label %" + arrayL + "\n";
      blockTerminated = true;
      startBlock(arrayL);
      std::string countAddr = newTemp(), count = newTemp();
      fn += "  " + countAddr + " = getelementptr inbounds i8, ptr " + p + ", i64 -8\n";
      fn += "  " + count + " = load i64, ptr " + countAddr + "\n";
      forEachElement(p, t, "0", count, true, at, [&](const std::string& element) { destroy(element, t, at); });
      std::string block = newTemp();
      fn += "  " + block + " = getelementptr inbounds i8, ptr " + p + ", i64 -" + std::to_string(cookie) + "\n";
      emitCall("", "void @free(ptr " + block + ")", false);
      emitBranch(endL);
      startBlock(endL);
    }

    Value unary(UnaryExpr* u)
    {
      if (u->isKeyword)
      {
        if (Kw(u->op) == Kw::not_)
        {
          Value v = convert(rvalue(u->operand), types.boolType(), u);
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + v.text + ", true\n";
          return {r, types.boolType()};
        }
        unsupported(u, "this keyword operator");
      }
      P op = P(u->op);
      if (op == P::Amp && u->ty && types.stripRef(u->ty)->kind == TyKind::MemberPointer)
      {
        // `&Style::field`: the field's offset within the class.
        auto* n = static_cast<NameExpr*>(u->operand);
        Decl* d = n->resolved;
        const Ty* mp = types.stripRef(u->ty);
        // (`&Proto::read` in a member of the class template: this instance's class, not the
        // template's own parameters the shared expression was typed with.)
        const Ty* cls = mp->args.empty() ? nullptr : withCurrentArgs(mp->args[0]);
        if (cls && types.isDependent(cls) && n->name.parts.size() > 1)
        {
          // `&T::luaNewIndex` in LuaBinder<T>: typed with the declaring template's own parameters
          // (LuaPrototypeTemplate<TemplateType, ...>), which this instance does not bind; the
          // qualifier under this instance's arguments names the class.
          QualifiedName qualifier = n->name;
          qualifier.parts.count -= 1;
          NamedType tmp{};
          tmp.kind = NodeKind::NamedType;
          tmp.token = n->token;
          tmp.name = qualifier;
          TemplateArgs none;
          if (const Ty* q = global.typeOfAstIn(&tmp, typingContext(), currentArgs ? *currentArgs : none); q && isClass(q) && !types.isDependent(q))
            cls = q;
        }
        if (d && d->kind == NodeKind::FunctionDecl && cls && isClass(cls))
        {
          // `&Calc::mul`: the function itself; a call through it passes the object first. A virtual
          // function, or one of a base at another place in the object, goes through a thunk that
          // calls the object's override, or moves `this` to that base.
          auto* f = static_cast<FunctionDecl*>(d);
          const Ty* key = canonicalClass(cls, u);
          const Ty* owner = key;
          if (Decl* ownerDecl = global.owningClass(f); ownerDecl && ownerDecl->kind == NodeKind::ClassDecl && classDef(ownerDecl, u) != classDef(key->decl, u))
            if (const Ty* base = baseInstantiation(key, ownerDecl, u))
              owner = base;
          std::vector<std::pair<std::string, uint32_t>> path;
          if (owner != key)
            basePath(key, owner, path, u);
          bool moved = false;
          for (auto& [structName, index] : path)
            moved = moved || index != 0;
          int slot = f->templateParams.empty() ? slotOf(owner, f, u) : -1;
          if (slot < 0 && !moved)
            return {useFunction(f, global.classArgsOf(owner), owner), mp};
          return {memberFunctionThunk(key, owner, path, f, slot, u), mp};
        }
        if (!d || d->kind != NodeKind::VariableDecl || !cls || !isClass(cls))
          unsupported(u, "a pointer to a member that is neither a field nor a function");
        std::string addr;
        if (!fieldAddress(cls, d, "null", addr, u))
          unsupported(u, "a pointer to a member that is not a field");
        std::string r = newTemp();
        fn += "  " + r + " = ptrtoint ptr " + addr + " to i64\n";
        return {r, mp};
      }
      if (op == P::Amp && u->operand->kind == NodeKind::NameExpr && static_cast<NameExpr*>(u->operand)->resolved &&
          static_cast<NameExpr*>(u->operand)->resolved->kind == NodeKind::FunctionDecl)
        return {rvalue(u->operand).text, u->ty}; // `&function`: the function itself is the pointer
      if (op == P::Amp)
        return {lvalue(u->operand), u->ty};
      if (op == P::Star)
      {
        Value p = rvalue(u->operand);
        if (isClass(p.ty))
        {
          // No operator* of its own: the class's conversion to a pointer, dereferenced
          // (`*this->undoFiller` through UndoItemFiller's `operator UndoItem*()`).
          const Ty* foundIn = nullptr;
          if (!findMemberFunction(p.ty, operatorName(P::Star), {}, p.ty, u, &foundIn, true))
            if (std::optional<Value> pointer = toPointer(p, u))
              p = *pointer;
          if (isClass(p.ty))
            return callRvalue(operatorCall(p, operatorName(P::Star), {}, u, true), u);
        }
        if (p.ty->kind != TyKind::Pointer)
          unsupported(u, "* on something that is not a pointer");
        return load(p.text, p.ty->inner, u);
      }
      if (op == P::PlusPlus || op == P::MinusMinus)
      {
        const Ty* ot = types.stripRef(u->operand->ty);
        if (isClass(ot))
        {
          Value o = rvalue(u->operand);
          std::string opName = operatorName(op);
          if (u->postfix)
          {
            const Ty* foundIn = nullptr;
            std::vector<const Ty*> intArg = {types.intType(32)};
            FunctionDecl* pf = findMemberFunction(o.ty, opName, intArg, o.ty, u, &foundIn, true);
            if (!pf)
            {
              // No postfix operator++(int): fall back to prefix operator++()
              return callRvalue(operatorCall(o, opName, {}, u, true), u);
            }
            std::string thisArg = o.text;
            if (canonicalClass(o.ty, u) != foundIn)
            {
              std::string addr;
              if (baseAddress(o.ty, foundIn, o.text, addr, u))
                thisArg = addr;
            }
            TemplateArgs fnArgs = global.classArgsOf(foundIn);
            std::string fname = useFunction(pf, fnArgs, foundIn);
            Signature s;
            const Ty* ret;
            {
              ArgsScope scope(*this, &fnArgs);
              s = signatureOf(pf);
              ret = declType(pf)->inner;
            }
            std::string argText = "ptr " + thisArg;
            std::string resultAddr;
            if (s.hasSret)
            {
              resultAddr = newObjectSlot(ret, u);
              argText += ", ptr " + resultAddr;
            }
            argText += ", i32 0";
            std::string sig = s.returnType + " ";
            if (s.returnType == "void")
            {
              emitCall("", sig + fname + "(" + argText + ")");
              if (s.hasSret)
                statementTemps.push_back({resultAddr, ret}); // not before: a call that throws built no result to destroy
              return s.hasSret ? callRvalue({resultAddr, ret, true}, u) : Value{"", types.voidType(), false};
            }
            std::string r = newTemp();
            emitCall(r + " = ", sig + fname + "(" + argText + ")");
            return callRvalue({r, ret, false}, u);
          }
          return callRvalue(operatorCall(o, opName, {}, u, true), u);
        }
        std::string addr = lvalue(u->operand);
        Value old = load(addr, ot, u);
        Value updated = arithmetic(op == P::PlusPlus ? P::Plus : P::Minus, old, Value{"1", types.intType(32)}, u);
        store(updated, addr, ot, u);
        return u->postfix ? old : convert(updated, ot, u);
      }
      Value v = rvalue(u->operand);
      if (isClass(v.ty))
      {
        const Ty* in = nullptr;
        if (op == P::Bang && !findMemberFunction(v.ty, "operator!", {}, v.ty, u, &in))
        {
          // No operator!: the class's conversion to bool, negated.
          Value b = convert(v, types.boolType(), u);
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + b.text + ", true\n";
          return {r, types.boolType()};
        }
        // No operator function: the built-in operator on the class's conversion to a scalar
        // (`-limit` with a Const<double>), as the resolver typed it.
        const Ty* scalar = nullptr;
        if (!findMemberFunction(v.ty, operatorName(op), {}, v.ty, u, &in) &&
            !(u->operand->ty && global.findFreeOperator(uint32_t(op), argumentType(u->operand, u->operand->ty, types), nullptr, currentFunction)))
          scalar = global.scalarConversionOf(v.ty);
        if (!scalar)
          return callRvalue(operatorCall(v, operatorName(op), {}, u, true), u);
        v = convert(v, scalar, u);
      }
      switch (op)
      {
        case P::Bang:
        {
          Value b = convert(v, types.boolType(), u);
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + b.text + ", true\n";
          return {r, types.boolType()};
        }
        case P::Minus:
        {
          Value c = convert(v, u->ty, u);
          std::string r = newTemp();
          if (c.ty->kind == TyKind::Float)
            fn += "  " + r + " = fneg " + llType(c.ty, u) + " " + c.text + "\n";
          else
            fn += "  " + r + " = sub " + llType(c.ty, u) + " 0, " + c.text + "\n";
          return {r, c.ty};
        }
        case P::Plus: return convert(v, u->ty, u);
        case P::Tilde:
        {
          Value c = convert(v, u->ty, u);
          std::string r = newTemp();
          fn += "  " + r + " = xor " + llType(c.ty, u) + " " + c.text + ", -1\n";
          return {r, c.ty};
        }
        default:
          unsupported(u, "this unary operator");
      }
    }

    Value binary(BinaryExpr* b)
    {
      if (b->isKeyword)
      {
        Kw k = Kw(b->op);
        if (k == Kw::and_ || k == Kw::or_)
          return logical(k == Kw::and_, b->lhs, b->rhs, b);
        unsupported(b, "this keyword operator");
      }
      P op = P(b->op);
      if (op == P::AndAnd || op == P::OrOr)
        return logical(op == P::AndAnd, b->lhs, b->rhs, b);
      if (op == P::Comma)
      {
        rvalue(b->lhs);
        return rvalue(b->rhs);
      }
      if (op == P::DotStar || op == P::ArrowStar)
      {
        std::string addr = memberPointerAddress(b);
        const Ty* mt = b->ty ? types.stripRef(b->ty) : nullptr;
        if (!mt || !mt->isKnown())
          unsupported(b, "a member pointer of unknown type");
        return load(addr, mt, b);
      }
      const Ty* lt = types.stripRef(b->lhs->ty);
      const Ty* rt = b->rhs && b->rhs->ty ? types.stripRef(b->rhs->ty) : nullptr;
      if (op == P::Assign && b->lhs->kind == NodeKind::IndexExpr && static_cast<IndexExpr*>(b->lhs)->base->ty &&
          typedUnionOf(types.stripRef(static_cast<IndexExpr*>(b->lhs)->base->ty), b))
        return assignAlternative(static_cast<IndexExpr*>(b->lhs), b->rhs, b); // `p[Integer] = 5`: the tag and the value
      if ((op == P::EqEq || op == P::NotEq) &&
          ((typedUnionOf(lt, b) && rt && rt->kind == TyKind::Enum) || (typedUnionOf(rt, b) && lt && lt->kind == TyKind::Enum)))
        return compareTag(b, op == P::NotEq);
      if (op == P::Assign && rt && rt->isKnown() && !types.isDependent(rt))
        if (ClassDecl* u = typedUnionOf(lt, b); u && !(isClass(rt) && canonicalClass(rt, b) == canonicalClass(lt, b)))
        {
          // `s = Attacking{7};`: the alternative held goes, the value's comes (design.md 8.5).
          std::string obj = lvalue(b->lhs);
          size_t index = alternativeForValue(u, lt, rt, b);
          destroyAlternative(u, lt, obj, b);
          constructAlternative(u, lt, obj, index, b->rhs, b);
          return {obj, lt, false};
        }
      // `a | b` on an enum with its own `Flags operator|(Flags, Flags)`: the user's function, as the
      // resolver typed it (a scoped enum has no built-in one).
      if (op != P::Assign && !isClass(lt) && !isClass(rt) && ((lt && lt->kind == TyKind::Enum) || (rt && rt->kind == TyKind::Enum)) &&
          lt && lt->isKnown() && rt && rt->isKnown())
        if (FunctionDecl* f = global.enumOperator(uint32_t(op), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types),
                                                  currentFunction))
        {
          Expr* args[2] = {b->lhs, b->rhs};
          return callRvalue(callFunction(f, {}, nullptr, "", "", Span<Expr*>{args, 2}, b), b);
        }
      // `a == b`, `a < b` of two typed unions of one type with no operator of their own: as
      // std::variant's, by the alternative held, then its values (design.md 8.5).
      if (ClassDecl* u = lt && rt ? typedUnionOf(lt, b) : nullptr;
          u && typedUnionOf(rt, b) && canonicalClass(lt, b) == canonicalClass(rt, b) &&
          (op == P::EqEq || op == P::NotEq || op == P::Less || op == P::Greater || op == P::LessEq || op == P::GreaterEq || op == P::Spaceship))
      {
        Expr* rhsExpr = b->rhs;
        Span<Expr*> one{&rhsExpr, 1};
        auto own = [&](P which) {
          const Ty* foundIn = nullptr;
          return findMemberFunction(types.unqualified(lt), operatorName(which), argTypesOf(one), types.unqualified(lt), b, &foundIn, true) ||
                 global.findFreeOperator(uint32_t(which), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types), currentFunction);
        };
        bool equality = op == P::EqEq || op == P::NotEq;
        if (!own(op) && !(equality ? own(P::EqEq) : own(P::Spaceship)))
        {
          Value l = rvalue(b->lhs), r = rvalue(b->rhs);
          if (equality)
          {
            Value eq = typedUnionEqual(u, l.text, r.text, lt, b);
            if (op == P::EqEq)
              return eq;
            std::string ne = newTemp();
            fn += "  " + ne + " = xor i1 " + eq.text + ", true\n";
            return {ne, types.boolType(), false};
          }
          Value order = typedUnionOrder(u, l.text, r.text, lt, b);
          if (op == P::Spaceship)
            return orderingObject(order, b);
          const char* predicate = op == P::Less ? "slt" : op == P::Greater ? "sgt" : op == P::LessEq ? "sle" : "sge";
          std::string res = newTemp();
          fn += "  " + res + " = icmp " + predicate + " i8 " + order.text + ", 0\n";
          return {res, types.boolType(), false};
        }
      }
      // `area += limit` with a Const<float> and no function for it: the scalar assignment below, on
      // the right operand's conversion to a scalar; `recipeCopied = setupForCrafting(...)`, an
      // ActionResult converting to bool and to its Enum: the conversion to the left side's type.
      bool scalarAssignment = isAssignment(op) && lt && lt->isKnown() && !isClass(lt) && isClass(rt) &&
                              (global.scalarConversionOf(rt) || (op == P::Assign && conversionOperator(rt, lt, b))) &&
                              !global.findFreeOperator(uint32_t(op), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types), currentFunction);
      if ((isClass(lt) || isClass(rt)) && !scalarAssignment)
      {
        std::string name = operatorName(op);
        Expr* rhsExpr = b->rhs;
        Span<Expr*> one{&rhsExpr, 1};
        const Ty* foundIn = nullptr;
        // `a = move(b)` of a class declaring no operator=: its implicit one (memberwise, below), not
        // a base's, which it hides.
        bool implicitAssignment = op == P::Assign && isClass(lt) && !declaresAssignment(types.unqualified(lt), b);
        if (isClass(lt) && !implicitAssignment && findMemberFunction(types.unqualified(lt), name, argTypesOf(one), types.unqualified(lt), b, &foundIn, true))
          return callRvalue(operatorCall(rvalue(b->lhs), name, one, b, true), b);
        // A free `operator+(const char*, const String&)` and the like, templates among them
        // (`template<class B, class E> bool operator!=(const Opt<B, E>&, const Opt<B, E>&)`).
        if (b->lhs->ty && b->rhs && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
          if (FunctionDecl* f = global.findFreeOperator(uint32_t(op), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types), currentFunction))
          {
            TemplateArgs fnArgs;
            if (!f->templateParams.empty())
            {
              global.deduceArgs(f, {argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types)}, fnArgs);
              if (fnArgs.pairs.size() < f->templateParams.size())
                unsupported(b, "a call to " + qualifiedName(f) + " whose template arguments could not be deduced");
            }
            Expr* args[2] = {b->lhs, b->rhs};
            return callRvalue(callFunction(f, fnArgs, nullptr, "", "", Span<Expr*>{args, 2}, b), b);
          }
        if (std::optional<Value> rewritten = rewrittenComparison(b, op, lt, rt))
          return *rewritten;
        // No operator function: C++'s built-in operator on the operands' conversions to a scalar
        // (`area < smallBlueprintAreaSize` with a Const<float>), as the resolver typed it.
        if (!isAssignment(op))
        {
          const Ty* lc = isClass(lt) ? global.scalarConversionOf(lt) : lt;
          const Ty* rc = isClass(rt) ? global.scalarConversionOf(rt) : rt;
          if (lc && rc && lc->isKnown() && rc->isKnown())
          {
            Value l = convert(rvalue(b->lhs), lc, b);
            Value r = convert(rvalue(b->rhs), rc, b);
            switch (op)
            {
              case P::EqEq: case P::NotEq: case P::Less: case P::LessEq: case P::Greater: case P::GreaterEq:
                return compare(op, l, r, b);
              case P::Spaceship:
                return orderingObject(orderOfScalars(l, r, b), b);
              default:
                return arithmetic(op, l, r, b);
            }
          }
        }
        if (!isClass(lt))
          unsupported(b, "an operator with a class on the right that no function defines");
        Value l = rvalue(b->lhs);
        if (op == P::Assign)
        {
          const Ty* lt0 = types.unqualified(types.stripRef(l.ty));
          const Ty* rt0 = b->rhs->ty && b->rhs->ty->isKnown() ? types.stripRef(b->rhs->ty) : nullptr;
          if (rt0 && !isClass(rt0) && findConstructor(lt0, {argumentType(b->rhs, b->rhs->ty, types)}, b))
          {
            // `flag = InvertColors::True`: the converting constructor makes a temporary to assign from.
            std::string tmp = convertedTemporary(lt0, b->rhs, b);
            assignObject(l.text, tmp, l.ty, b, true);
            return l;
          }
          if (rt0 && isClass(rt0) && classKey(rt0) != classKey(lt0) && !baseInstantiation(rt0, classDef(lt0->decl, b), b) &&
              (conversionOperator(rt0, lt0, b) || findConstructor(lt0, {argumentType(b->rhs, b->rhs->ty, types)}, b) ||
               global.converts(argumentType(b->rhs, b->rhs->ty, types), lt0)))
          {
            // `view = string`: StringView's converting constructor makes the temporary to assign from;
            // also one inherited with `using Base::Base;` (`states[i] = createPipelineStateObject(spec)`
            // into a SpritePipelineState built from PipelineStateHandle's UniquePtr constructor).
            std::string tmp = convertedTemporary(lt0, b->rhs, b);
            assignObject(l.text, tmp, l.ty, b, true);
            return l;
          }
          if (b->rhs->kind == NodeKind::BraceInitExpr)
          {
            // `*this = {}`, `this->data = {a, b}`, `mode = {Mode::MinLinear, Mode::MagLinear}`: a
            // temporary of the left side's class, list-initialised as `T tmp = {...};` would be (an
            // InitializerList constructor first), moved from, and destroyed at the statement's end.
            std::string tmp = newObjectSlot(lt0, b);
            initializeFrom(tmp, lt0, b->rhs, b);
            if (needsDestruction(lt0, b))
              statementTemps.push_back({tmp, lt0});
            assignObject(l.text, tmp, lt0, b, true);
            return l;
          }
          Value r = rvalue(b->rhs);
          if (!isClass(r.ty))
            unsupported(b, "assigning a non-class to a class");
          std::string src = r.text;
          if (classKey(r.ty) != classKey(l.ty))
            if (!baseAddress(r.ty, l.ty, r.text, src, b))
              unsupported(b, "assigning an unrelated class");
          assignObject(l.text, src, l.ty, b, r.isTemporary || isXvalue(b->rhs));
          return l;
        }
        unsupported(b, qualifiedName(l.ty->decl) + " has no " + name);
      }
      if (isAssignment(op))
      {
        std::string addr = lvalue(b->lhs);
        const Ty* t = types.stripRef(b->lhs->ty);
        if ((!t || !t->isKnown()) && b->lhs->kind == NodeKind::MemberExpr)
        {
          auto* mx = static_cast<MemberExpr*>(b->lhs);
          if (mx->resolved && mx->resolved->kind == NodeKind::VariableDecl)
            t = types.stripRef(declType(mx->resolved));
          if ((!t || !t->isKnown()) && !mx->member.empty())
          {
            const Ty* bt = mx->base->ty ? types.stripRef(mx->base->ty) : nullptr;
            if (mx->op == P::Arrow && bt && bt->kind == TyKind::Pointer)
              bt = types.stripRef(bt->inner);
            if (bt && isClass(bt))
              if (Decl* fd = findFieldByName(bt, mx->member.last().symbol, b))
                t = types.stripRef(declType(fd));
          }
        }
        if ((!t || !t->isKnown()) && b->lhs->kind == NodeKind::NameExpr)
          if (auto local = locals.find(static_cast<NameExpr*>(b->lhs)->resolved); local != locals.end() && local->second.ty && local->second.ty->isKnown())
            t = types.stripRef(local->second.ty); // `xmm7 = _mm_shufflelo_epi16(...)` into an SSE `auto` local
        if (!t || !t->isKnown())
        {
          // Fall back to the RHS type when the LHS type is unknown.
          const Ty* rhsTy = b->rhs->ty ? types.stripRef(b->rhs->ty) : nullptr;
          if (rhsTy && rhsTy->isKnown())
            t = rhsTy;
          else
            unsupported(b, "an assignment to an expression of unknown type");
        }
        Value rhs = op == P::Assign && b->rhs->kind == NodeKind::BraceInitExpr ? braceScalar(static_cast<BraceInitExpr*>(b->rhs), t, b) // `mod->id.crc = {};`
                                                                                : rvalue(b->rhs);
        if (scalarAssignment)
          rhs = convert(rhs, global.scalarConversionOf(rhs.ty) ? global.scalarConversionOf(rhs.ty) : t, b);
        const BitfieldInfo* bits = nullptr;
        if (b->lhs->kind == NodeKind::MemberExpr)
          if (Decl* d = static_cast<MemberExpr*>(b->lhs)->resolved)
            bits = bitfieldFor(d);
        if (op != P::Assign)
        {
          Value old = bits ? loadBitfield(addr, *bits, t, b) : load(addr, t, b);
          rhs = arithmetic(baseOp(op), old, rhs, b);
        }
        if (bits)
          storeBitfield(convert(rhs, t, b), addr, *bits, t, b);
        else
          store(rhs, addr, t, b);
        return convert(rhs, t, b);
      }
      Value l = rvalue(b->lhs);
      if (isClass(l.ty))
      {
        // Late resolution: the AST type was Unknown but codegen resolved a class.
        std::string name = operatorName(op);
        Expr* rhsExpr = b->rhs;
        Span<Expr*> one{&rhsExpr, 1};
        Value result = operatorCall(l, name, one, b, false);
        if (result.ty)
          return callRvalue(result, b);
        if (std::optional<Value> rewritten = rewrittenComparison(b, op, l.ty, nullptr))
          return *rewritten;
        unsupported(b, qualifiedName(l.ty->decl) + " has no " + name);
      }
      Value r = rvalue(b->rhs);
      switch (op)
      {
        case P::EqEq: case P::NotEq: case P::Less: case P::LessEq: case P::Greater: case P::GreaterEq:
          return compare(op, l, r, b);
        case P::Spaceship:
          return orderingObject(orderOfScalars(l, r, b), b);
        default:
          return arithmetic(op, l, r, b);
      }
    }

    // --- three-way comparison ---

    // The library's strong_ordering (what `a <=> b` yields), or null without the library.
    const Ty* orderingType()
    {
      Decl* d = global.strongOrderingDecl();
      return d ? types.classType(d, {}) : nullptr;
    }

    // The order of two scalars as an i8: -1, 0 or 1.
    Value orderOfScalars(Value l, Value r, Node* at)
    {
      Value less = compare(P::Less, l, r, at);
      Value greater = compare(P::Greater, l, r, at);
      std::string g = newTemp(), res = newTemp();
      fn += "  " + g + " = select i1 " + greater.text + ", i8 1, i8 0\n";
      fn += "  " + res + " = select i1 " + less.text + ", i8 -1, i8 " + g + "\n";
      return {res, types.intType(8)};
    }

    // An order (i8) as the value of `a <=> b`: a strong_ordering object, or an int without the library.
    Value orderingObject(Value order, Node* at)
    {
      const Ty* ot = orderingType();
      if (!ot)
        return convert(order, types.intType(), at);
      std::string slot = newObjectSlot(ot, at);
      fn += "  store i8 " + order.text + ", ptr " + gep(structFor(ot, at).name, slot, 0) + "\n";
      return {slot, ot, true};
    }

    // The i8 inside the value of `a <=> b`.
    Value orderOf(Value ordering, Node* at)
    {
      const Ty* t = types.stripRef(ordering.ty);
      if (isClass(t))
        return load(gep(structFor(t, at).name, ordering.text, 0), types.intType(8), at);
      return convert(ordering, types.intType(8), at);
    }

    // `a <=> b` of two objects at those addresses: the class's operator<=> (memberwise when
    // defaulted); the order as an i8.
    Value orderOfObjects(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      const Ty* foundIn = nullptr;
      std::vector<const Ty*> argTypes{types.lref(types.qualified(t, true, false))};
      FunctionDecl* op = findMemberFunction(t, "operator<=>", argTypes, t, at, &foundIn);
      if (ClassDecl* u = typedUnionOf(t, at); u && (!op || (op->flags & DF_Default)))
        return typedUnionOrder(u, a, b, t, at); // the alternative held, then its values (design.md 8.5)
      if (!op)
      {
        const Ty* operand = types.lref(types.qualified(t, true, false));
        if (global.findFreeOperator(uint32_t(P::Spaceship), operand, operand, currentFunction))
          return orderOf(comparedAt(P::Spaceship, a, b, t, at), at);
        // C++'s synthesized three-way comparison (what a defaulted <=> declaring its order type does
        // with a member that has no <=>): equal by ==, else less by <, else greater; a Direction
        // compares by its conversion to its Enum.
        Value equal = convert(comparedAt(P::EqEq, a, b, t, at), types.boolType(), at);
        Value less = convert(comparedAt(P::Less, a, b, t, at), types.boolType(), at);
        std::string ordered = newTemp(), order = newTemp();
        fn += "  " + ordered + " = select i1 " + less.text + ", i8 -1, i8 1\n";
        fn += "  " + order + " = select i1 " + equal.text + ", i8 0, i8 " + ordered + "\n";
        return {order, types.intType(8)};
      }
      if (op->flags & DF_Default)
        return memberwiseOrder(a, b, t, at);
      TemplateArgs args = global.classArgsOf(foundIn);
      Signature s;
      {
        ArgsScope scope(*this, &args);
        s = signatureOf(op);
      }
      std::string name = useFunction(op, args, foundIn);
      std::string thisArg = a;
      if (canonicalClass(t, at) != foundIn)
        baseAddress(t, foundIn, a, thisArg, at);
      if (s.returnType == "void")
      {
        const Ty* ot = orderingType();
        if (!ot)
          unsupported(at, "operator<=> returning an object without the library's strong_ordering");
        std::string tmp = newObjectSlot(ot, at); // the result object: this, sret, other
        emitCall("", "void " + name + "(ptr " + thisArg + ", ptr " + tmp + ", ptr " + b + ")");
        return orderOf(Value{tmp, ot, true}, at);
      }
      std::string r = newTemp();
      emitCall(r + " = ", s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + b + ")");
      return convert(Value{r, types.intType(32), false}, types.intType(8), at);
    }

    // The order of two objects base by base and member by member: the first that differs decides.
    Value memberwiseOrder(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      const StructInfo& info = structFor(t, at);
      std::string slot = "%order" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i8\n";
      fn += "  store i8 0, ptr " + slot + "\n";
      std::string done = newLabel("order.done");
      auto part = [&](Value order)
      {
        fn += "  store i8 " + order.text + ", ptr " + slot + "\n";
        std::string ne = newTemp(), next = newLabel("order.next");
        fn += "  " + ne + " = icmp ne i8 " + order.text + ", 0\n";
        fn += "  br i1 " + ne + ", label %" + done + ", label %" + next + "\n";
        blockTerminated = true;
        startBlock(next);
      };
      for (auto& [baseDef, index] : info.bases)
        part(orderOfObjects(gep(info.name, a, index), gep(info.name, b, index), info.fieldTypes[index], at));
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = fieldIndexOf(info, d);
        const Ty* ft = info.fieldTypes[index];
        std::string fa = gep(info.name, a, index), fb = gep(info.name, b, index);
        part(orderOfParts(fa, fb, ft, at));
      }
      emitBranch(done);
      startBlock(done);
      return load(slot, types.intType(8), at);
    }

    // The order (i8) of two parts of objects (members, elements) at those addresses.
    Value orderOfParts(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      if (isClass(t))
        return orderOfObjects(a, b, t, at);
      if (t->kind == TyKind::Array)
        return orderOfArrays(a, b, t, at);
      return orderOfScalars(load(a, t, at), load(b, t, at), at);
    }

    // The order of two arrays at those addresses element by element: the first that differs decides.
    Value orderOfArrays(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      if (!t->count || !t->inner)
        unsupported(at, "a defaulted comparison of an array member without its length");
      std::string slot = "%order" + std::to_string(temp++), index = "%index" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i8\n  " + index + " = alloca i64\n";
      fn += "  store i8 0, ptr " + slot + "\n  store i64 0, ptr " + index + "\n";
      std::string arrayType = llType(t, at);
      std::string cond = newLabel("arrayorder.cond"), body = newLabel("arrayorder.body"), next = newLabel("arrayorder.next"), done = newLabel("arrayorder.done");
      emitBranch(cond);
      startBlock(cond);
      std::string i = newTemp(), more = newTemp();
      fn += "  " + i + " = load i64, ptr " + index + "\n";
      fn += "  " + more + " = icmp ult i64 " + i + ", " + std::to_string(t->count) + "\n";
      fn += "  br i1 " + more + ", label %" + body + ", label %" + done + "\n";
      blockTerminated = true;
      startBlock(body);
      std::string ea = newTemp(), eb = newTemp();
      fn += "  " + ea + " = getelementptr inbounds " + arrayType + ", ptr " + a + ", i64 0, i64 " + i + "\n";
      fn += "  " + eb + " = getelementptr inbounds " + arrayType + ", ptr " + b + ", i64 0, i64 " + i + "\n";
      Value order = orderOfParts(ea, eb, t->inner, at);
      std::string differs = newTemp();
      fn += "  store i8 " + order.text + ", ptr " + slot + "\n";
      fn += "  " + differs + " = icmp ne i8 " + order.text + ", 0\n";
      fn += "  br i1 " + differs + ", label %" + done + ", label %" + next + "\n";
      blockTerminated = true;
      startBlock(next);
      std::string i2 = newTemp();
      fn += "  " + i2 + " = add i64 " + i + ", 1\n  store i64 " + i2 + ", ptr " + index + "\n";
      emitBranch(cond);
      startBlock(done);
      return load(slot, types.intType(8), at);
    }

    // The comparisons C++ rewrites when the class defines no operator for them: `a != b` as
    // `!(a == b)`, `a < b` and the other orderings as `(a <=> b) < 0`, and `==` of a class whose
    // operator<=> is defaulted as `(a <=> b) == 0`. Null when the class has none of those either.
    std::optional<Value> rewrittenComparison(BinaryExpr* b, P op, const Ty* lt, const Ty* rt)
    {
      Expr* rhsExpr = b->rhs;
      Span<Expr*> one{&rhsExpr, 1};
      auto reversed = [&]() -> std::optional<Value>
      {
        // `3 == id` through Id::operator==(int): the operands the other way round.
        if (!rt || !isClass(rt))
          return std::nullopt;
        Expr* lhsExpr = b->lhs;
        Span<Expr*> left{&lhsExpr, 1};
        const Ty* foundIn = nullptr;
        if (findMemberFunction(types.unqualified(rt), "operator==", argTypesOf(left), types.unqualified(rt), b, &foundIn, true))
          return callRvalue(operatorCall(rvalue(b->rhs), "operator==", left, b, true), b);
        return std::nullopt;
      };
      auto call = [&](P which) -> std::optional<Value>
      {
        std::string name = operatorName(which);
        const Ty* foundIn = nullptr;
        if (isClass(lt) && findMemberFunction(types.unqualified(lt), name, argTypesOf(one), types.unqualified(lt), b, &foundIn, true))
          return callRvalue(operatorCall(rvalue(b->lhs), name, one, b, true), b);
        if (b->lhs->ty && b->rhs && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
          if (FunctionDecl* f = global.findFreeOperator(uint32_t(which), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types), currentFunction); f && f->templateParams.empty())
          {
            Expr* args[2] = {b->lhs, b->rhs};
            return callRvalue(callFunction(f, {}, nullptr, "", "", Span<Expr*>{args, 2}, b), b);
          }
        return std::nullopt;
      };
      auto threeWayDefaulted = [&]()
      {
        const Ty* foundIn = nullptr;
        FunctionDecl* f = isClass(lt) ? findMemberFunction(types.unqualified(lt), "operator<=>", argTypesOf(one), types.unqualified(lt), b, &foundIn, true) : nullptr;
        return f && (f->flags & DF_Default);
      };
      auto orderCompare = [&](P cmp) -> std::optional<Value>
      {
        std::optional<Value> ordering = call(P::Spaceship);
        if (!ordering)
          return std::nullopt;
        return compare(cmp, orderOf(*ordering, b), Value{"0", types.intType(8)}, b);
      };
      // `scale < ScreenshotRequest::MinScale` with a double on the left: the right operand's
      // `operator<=>(const double&)`, the operands the other way round (`0 < (b <=> a)`).
      auto reversedOrder = [&](P cmp) -> std::optional<Value>
      {
        // (Also with a class on the left whose own <=> does not take the right operand: C++20's
        // reversed candidate, `current > latest` with Optional's `operator<=>(const U&)`.)
        if (!rt || !isClass(rt))
          return std::nullopt;
        Expr* lhsExpr = b->lhs;
        Span<Expr*> left{&lhsExpr, 1};
        const Ty* foundIn = nullptr;
        if (!findMemberFunction(types.unqualified(rt), "operator<=>", argTypesOf(left), types.unqualified(rt), b, &foundIn, true))
          return std::nullopt;
        Value ordering = callRvalue(operatorCall(rvalue(b->rhs), "operator<=>", left, b, true), b);
        P swapped = cmp == P::Less ? P::Greater : cmp == P::Greater ? P::Less : cmp == P::LessEq ? P::GreaterEq : P::LessEq;
        return compare(swapped, orderOf(ordering, b), Value{"0", types.intType(8)}, b);
      };
      // C++20: a class whose operator<=> is defaulted has a defaulted operator==(const T&) too, which
      // takes a value of another type through T's converting constructor (`alertType ==
      // AlertType::ALERTS_COUNT` beside AlertType's `operator<=>(const Enum&)`, which is no ==).
      // classOnRight: `this->value != MAX_TICK`, the scalar on the left converted to the MapTick on
      // the right (C++20's reversed candidate of the implicit ==).
      auto implicitEquality = [&](bool classOnRight) -> std::optional<Value>
      {
        Expr* classSide = classOnRight ? b->rhs : b->lhs;
        Expr* otherSide = classOnRight ? b->lhs : b->rhs;
        const Ty* classSideTy = classOnRight ? rt : lt;
        if (!isClass(classSideTy) || !otherSide->ty || !otherSide->ty->isKnown())
          return std::nullopt;
        const Ty* cls = canonicalClass(types.unqualified(classSideTy), b);
        bool defaulted = false;
        for (Decl* m : classDef(cls->decl, b)->members)
          if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Default) && !static_cast<FunctionDecl*>(m)->name.empty() &&
              static_cast<FunctionDecl*>(m)->name.last().special == 2 && P(static_cast<FunctionDecl*>(m)->name.last().opToken) == P::Spaceship)
            defaulted = true;
        if (!defaulted)
          return std::nullopt;
        const Ty* other = withCurrentArgs(types.unqualified(types.stripRef(otherSide->ty)));
        bool sameClass = isClass(other) && classKey(other) == classKey(cls);
        if (!sameClass && !findConstructor(cls, {argumentType(otherSide, otherSide->ty, types)}, b))
          return std::nullopt;
        auto otherValue = [&]() -> std::string
        {
          if (sameClass)
            return rvalue(otherSide).text;
          std::string r = newObjectSlot(cls, b);
          Expr* one = otherSide;
          constructInto(r, cls, Span<Expr*>{&one, 1}, b);
          if (needsDestruction(cls, b))
            statementTemps.push_back({r, cls});
          return r;
        };
        // The operands in the order they are written.
        if (classOnRight)
        {
          std::string l = otherValue();
          return objectsEqual(l, rvalue(classSide).text, cls, b);
        }
        Value l = rvalue(classSide);
        return objectsEqual(l.text, otherValue(), cls, b);
      };
      switch (op)
      {
        case P::NotEq:
        {
          std::optional<Value> eq = call(P::EqEq);
          if (!eq && threeWayDefaulted())
            return orderCompare(P::NotEq);
          if (!eq)
            eq = reversed();
          if (!eq)
            eq = implicitEquality(false);
          if (!eq)
            eq = implicitEquality(true);
          if (!eq)
            return std::nullopt;
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + convert(*eq, types.boolType(), b).text + ", true\n";
          return Value{r, types.boolType()};
        }
        case P::EqEq:
          if (threeWayDefaulted())
            return orderCompare(P::EqEq);
          if (std::optional<Value> eq = reversed())
            return eq;
          if (std::optional<Value> eq = implicitEquality(false))
            return eq;
          return implicitEquality(true);
        case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
          if (std::optional<Value> ordered = orderCompare(op))
            return ordered;
          return reversedOrder(op);
        case P::Spaceship:
        {
          // `minimum <=> scale`: the reversed `0 <=> (scale <=> minimum)`, its order negated, of the
          // type that call gives (as the resolver types it).
          if (!rt || !isClass(rt) || (lt && isClass(lt)))
            return std::nullopt;
          Expr* lhsExpr = b->lhs;
          Span<Expr*> left{&lhsExpr, 1};
          const Ty* foundIn = nullptr;
          if (!findMemberFunction(types.unqualified(rt), "operator<=>", argTypesOf(left), types.unqualified(rt), b, &foundIn, true))
            return std::nullopt;
          Value ordering = callRvalue(operatorCall(rvalue(b->rhs), "operator<=>", left, b, true), b);
          std::string negated = newTemp();
          fn += "  " + negated + " = sub i8 0, " + orderOf(ordering, b).text + "\n";
          Value order{negated, types.intType(8)};
          const Ty* ot = types.unqualified(types.stripRef(ordering.ty));
          return isClass(ot) ? orderingObject(order, b) : convert(order, ot, b);
        }
        default:
          return std::nullopt;
      }
    }

    // The address `object.*member` / `pointer->*member` denotes: the object plus the offset.
    std::string memberPointerAddress(BinaryExpr* b)
    {
      Value object = rvalue(b->lhs);
      std::string base = object.text;
      if (P(b->op) == P::ArrowStar)
      {
        if (object.ty->kind != TyKind::Pointer)
          unsupported(b, "->* on something that is not a pointer");
      }
      else if (!isClass(object.ty))
        unsupported(b, ".* on something that is not an object");
      Value offset = rvalue(b->rhs);
      if (!offset.ty || types.stripRef(offset.ty)->kind != TyKind::MemberPointer)
        unsupported(b, "a member pointer of unknown type");
      std::string r = newTemp();
      fn += "  " + r + " = getelementptr inbounds i8, ptr " + base + ", i64 " + offset.text + "\n";
      return r;
    }

    Value logical(bool isAnd, Expr* lhs, Expr* rhs, Node* at)
    {
      Value l = convert(rvalue(lhs), types.boolType(), at);
      std::string rL = newLabel(isAnd ? "and.rhs" : "or.rhs"), eL = newLabel(isAnd ? "and.end" : "or.end");
      std::string lEnd = currentBlock;
      if (isAnd)
        fn += "  br i1 " + l.text + ", label %" + rL + ", label %" + eL + "\n";
      else
        fn += "  br i1 " + l.text + ", label %" + eL + ", label %" + rL + "\n";
      startBlock(rL);
      size_t temps = statementTemps.size();
      Value r = convert(rvalue(rhs), types.boolType(), at);
      destroyTempsFrom(temps); // `flag && Noisy(2).value() > 5`: the Noisy exists only here
      std::string rEnd = currentBlock;
      emitBranch(eL);
      startBlock(eL);
      std::string res = newTemp();
      fn += "  " + res + " = phi i1 [ " + std::string(isAnd ? "false" : "true") + ", %" + lEnd + " ], [ " + r.text + ", %" + rEnd + " ]\n";
      return {res, types.boolType()};
    }

    // --- statements and scopes ---

    void destroyTemps()
    {
      for (size_t i = statementTemps.size(); i-- > 0;)
        destroyOnExit(statementTemps[i]);
      statementTemps.clear();
    }

    // Destroys the temporaries made since mark: those of one branch of `?:` or of the right
    // operand of `&&`/`||`, which exist only when that branch ran, so they die inside it.
    void destroyTempsFrom(size_t mark)
    {
      for (size_t i = statementTemps.size(); i-- > mark;)
        destroyOnExit(statementTemps[i]);
      statementTemps.resize(mark);
    }

    void destroyObject(const Object& o)
    {
      if (o.exception)
        fn += "  call void @kpp_eh_end(ptr " + o.addr + ")" + bundle() + "\n";
      else if (o.freeMemory)
      {
        ensureMalloc();
        fn += "  call void @free(ptr " + o.addr + ")" + bundle() + "\n";
      }
      else
        destroy(o.addr, o.ty, nullptr);
    }

    // What a normal exit (the end of a scope or a statement, a return, a break) destroys: not what
    // only an exception leaving does (Object::unwindOnly).
    void destroyOnExit(const Object& o)
    {
      if (!o.unwindOnly)
        destroyObject(o);
    }

    void pushScope() { scopes.emplace_back(); }

    void popScope()
    {
      if (!blockTerminated)
        for (size_t i = scopes.back().size(); i-- > 0;)
          destroyOnExit(scopes.back()[i]);
      scopes.pop_back();
    }

    // Destroys the objects of the innermost scopes down to (not including) depth.
    void unwindTo(size_t depth) { unwindRange(depth, scopes.size()); }

    // Destroys the objects of scopes [low, high), innermost first; the scopes stay open.
    void unwindRange(size_t low, size_t high)
    {
      for (size_t s = high; s-- > low;)
        for (size_t i = scopes[s].size(); i-- > 0;)
          destroyOnExit(scopes[s][i]);
    }

    // Destroys the scopes above depth for a `return`, `break` or `continue`, leaving the catch
    // handlers in between through catchret: a funclet cannot branch out directly.
    void leaveScopes(size_t depth)
    {
      std::vector<EHFrame> saved = ehFrames;
      size_t top = scopes.size();
      for (size_t i = ehFrames.size(); i-- > 0;)
      {
        if (!ehFrames[i].isHandler)
          continue;
        if (ehFrames[i].scopeDepth < depth)
          break; // the target is inside this handler
        unwindRange(ehFrames[i].scopeDepth, top);
        top = ehFrames[i].scopeDepth;
        std::string next = newLabel("catchret");
        fn += "  catchret from " + ehFrames[i].funclet + " to label %" + next + "\n";
        ehFrames.resize(i);
        startBlock(next);
      }
      unwindRange(depth, top);
      ehFrames = saved;
    }

    // --- exceptions ---

    // The catchpad token of the handler the code being emitted belongs to, or "" outside handlers.
    std::string currentFunclet()
    {
      if (!funcletOverride.empty())
        return funcletOverride;
      for (size_t i = ehFrames.size(); i-- > 0;)
        if (ehFrames[i].isHandler)
          return ehFrames[i].funclet;
      return "";
    }

    // The operand bundle a call inside a funclet carries.
    std::string bundle()
    {
      std::string f = currentFunclet();
      return f.empty() ? "" : " [ \"funclet\"(token " + f + ") ]";
    }

    bool handlerDeclared = false; // this emitter saw __C_specific_handler declared
    void markEH()
    {
      usesEH = true;
      if (handlerDeclared)
        return;
      handlerDeclared = true;
      if (declaredNames.claim("@__C_specific_handler"))
        decls += "declare i32 @__C_specific_handler(...)\n";
    }

    // Emits `call` or, when an exception leaving the call has to run destructors or reach a handler
    // of this function, `invoke`. prefix is "" or "%r = "; callee is the text after `call`.
    void emitCall(const std::string& prefix, const std::string& callee, bool mayThrow = true)
    {
      std::string dest = mayThrow && funcletOverride.empty() ? unwindDestination() : std::string();
      if (dest.empty())
      {
        fn += "  " + prefix + "call " + callee + bundle() + "\n";
        return;
      }
      markEH();
      std::string cont = newLabel("invoke.cont");
      fn += "  " + prefix + "invoke " + callee + bundle() + " to label %" + cont + " unwind label %" + dest + "\n";
      startBlock(cont);
    }

    // Where an exception thrown at this point unwinds to: a cleanup pad destroying the objects
    // constructed since the innermost try or handler began (shared by all calls with the same
    // objects), the try's dispatch, or "" when nothing in this function has to happen.
    std::string unwindDestination()
    {
      size_t limit = ehFrames.empty() ? 0 : ehFrames.back().scopeDepth;
      std::vector<Object> objects;
      for (size_t i = statementTemps.size(); i-- > 0;)
        objects.push_back(statementTemps[i]);
      for (size_t s = scopes.size(); s-- > limit;)
        for (size_t i = scopes[s].size(); i-- > 0;)
          objects.push_back(scopes[s][i]);
      std::string next; // "" = out of the function or funclet
      if (!ehFrames.empty() && !ehFrames.back().isHandler)
      {
        next = ehFrames.back().dispatchLabel;
        if (objects.empty())
          return ehFrames.back().used = true, next;
      }
      if (objects.empty())
        return "";
      markEH();
      if (!next.empty())
        ehFrames.back().used = true;
      return cleanupChain(objects, 0, next, currentFunclet());
    }

    // The pad destroying objects[from..] (the newest first) and then unwinding to next ("": out of
    // the function or funclet): it destroys objects[from] and continues into the pad of the rest,
    // which every call whose live objects end the same way shares. A constructor's members so cost
    // one destructor call each, not one per member built before them: ControlSettings' constructor
    // was 875 k lines of IR with 482 k calls of ~String, a quarter of clang's critical path.
    std::string cleanupChain(const std::vector<Object>& objects, size_t from, const std::string& next, const std::string& parent)
    {
      if (from == objects.size())
        return next;
      std::string key = next + "|" + parent;
      for (size_t i = from; i < objects.size(); ++i)
        key += "|" + objects[i].addr + (objects[i].freeMemory ? "~" : "") + (objects[i].exception ? "!" : "");
      auto it = cleanupPads.find(key);
      if (it != cleanupPads.end())
        return it->second;
      std::string rest = cleanupChain(objects, from + 1, next, parent);
      std::string padLabel = newLabel("cleanup"), pad = newTemp();
      cleanupPads[key] = padLabel;
      // The pad is emitted aside, after the function body.
      std::string savedFn = std::move(fn), savedBlock = currentBlock, savedOverride = funcletOverride;
      bool savedTerminated = blockTerminated;
      fn.clear();
      startBlock(padLabel);
      fn += "  " + pad + " = cleanuppad within " + (parent.empty() ? "none" : parent) + " []\n";
      funcletOverride = pad;
      destroyObject(objects[from]);
      funcletOverride = savedOverride;
      fn += "  cleanupret from " + pad + " unwind " + (rest.empty() ? "to caller" : "label %" + rest) + "\n";
      ehBlocks += fn;
      fn = std::move(savedFn);
      currentBlock = savedBlock;
      blockTerminated = savedTerminated;
      return padLabel;
    }

    std::string sharedComdat(const std::string& name) { return "$" + name.substr(1) + " = comdat any\n"; }

    // The type descriptor matched against catch clauses, as a class descriptor is (descriptorFor):
    // { name, number of direct bases, [{ base's descriptor, base's offset }] }, so that a handler
    // takes any base of the thrown class, with that base's offset.
    std::string typeinfoFor(const Ty* t, Node* at)
    {
      t = types.unqualified(types.stripRef(t));
      auto lock = sharedLock(); // the descriptors emitted, the library's Exception's
      std::string name, display, list;
      size_t count = 0;
      if (isClass(t))
      {
        const StructInfo& info = structFor(t, at);
        name = "@kpp.ti." + info.name.substr(8);
        display = types.toString(t, &symbols);
        std::string structName = info.name;
        std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
        std::vector<const Ty*> fieldTypes = info.fieldTypes;
        for (auto& [baseDef, index] : bases)
          list += (count++ ? ", " : "") + std::string("{ ptr, i64 } { ptr ") + typeinfoFor(fieldTypes[index], at) +
                  ", i64 ptrtoint (ptr getelementptr (" + structName + ", ptr null, i32 0, i32 " + std::to_string(index) + ") to i64) }";
        if (display == "Exception" && exceptionTypeinfo.empty())
          exceptionTypeinfo = name;
      }
      else
      {
        display = types.toString(t, &symbols);
        name = "@\"kpp.ti." + display + "\""; // one descriptor per scalar type, pointers by pointee
      }
      if (emittedShared[name])
        return name;
      emittedShared[name] = true;
      bool was = emittingShared;
      emittingShared = true;
      std::string str = cStringConstant(display);
      emittingShared = was;
      std::string arrayType = "[" + std::to_string(count) + " x { ptr, i64 }]";
      shared += sharedComdat(name) + name + " = linkonce_odr constant { ptr, i64, " + arrayType + " } { ptr " + str + ", i64 " +
                std::to_string(count) + ", " + arrayType + " " + (count ? "[" + list + "]" : std::string("zeroinitializer")) + " }, comdat\n";
      return name;
    }

    // The destructor slot of a class that declares no destructor: a function destroying its members
    // and bases (destructorThunk), or one doing nothing when none of them needs it.
    std::string implicitDestructor(const Ty* t, Node* at)
    {
      std::string thunk = destructorThunk(t, at);
      if (thunk != "null")
        return thunk;
      auto lock = sharedLock();
      if (!emittedShared["@kpp.nodtor"])
      {
        emittedShared["@kpp.nodtor"] = true;
        shared += sharedComdat("@kpp.nodtor") + "define linkonce_odr void @kpp.nodtor(ptr %o) comdat {\nentry:\n  ret void\n}\n";
      }
      return "@kpp.nodtor";
    }

    // A function destroying an object of type t through a pointer (for the exception runtime), or null.
    std::string destructorThunk(const Ty* t, Node* at)
    {
      if (!needsDestruction(t, at))
        return "null";
      std::string name = "@kpp.dtor." + structFor(t, at).name.substr(8);
      auto lock = sharedLock(); // held while the thunk is emitted: one emitter writes it
      if (emittedShared[name])
        return name;
      emittedShared[name] = true;
      FunctionState saved = saveState();
      std::string savedOverride = funcletOverride;
      funcletOverride.clear();
      resetFunctionState();
      try
      {
        destroy("%o", t, at);
        fn += "  ret void\n";
        shared += sharedComdat(name) + "define linkonce_odr void " + name + "(ptr %o) comdat {\nentry:\n" + allocas + fn + "}\n";
      }
      catch (Unsupported&)
      {
        restoreState(saved);
        funcletOverride = savedOverride;
        throw;
      }
      restoreState(saved);
      funcletOverride = savedOverride;
      return name;
    }

    // `throw expr`: the object is built on the heap and raised with its descriptor and destructor.
    // `throw;` raises the innermost handler's exception again, which then no longer owns it. The
    // runtime's raise functions are declared as returning (they never do), see below.
    void emitThrow(ThrowExpr* t)
    {
      usesExceptions = true;
      if (!t->operand)
      {
        const EHFrame* handler = nullptr;
        for (size_t i = ehFrames.size(); i-- > 0 && !handler;)
          if (ehFrames[i].isHandler)
            handler = &ehFrames[i];
        std::string slot;
        if (handler)
          slot = handler->exceptionSlot;
        else
        {
          // In a function a handler calls (an exception dispatcher's `try { throw; } catch ...`):
          // the exception that handler holds, kpp.eh.current; with none, a wrong program, stopped.
          slot = newTemp();
          fn += "  " + slot + " = load ptr, ptr @kpp.eh.current\n";
          if (declaredNames.claim("@llvm.trap"))
            decls += "declare void @llvm.trap()\n";
          std::string none = newTemp(), okL = newLabel("rethrow"), noneL = newLabel("rethrow.none");
          fn += "  " + none + " = icmp eq ptr " + slot + ", null\n";
          fn += "  br i1 " + none + ", label %" + noneL + ", label %" + okL + "\n";
          startBlock(noneL);
          fn += "  call void @llvm.trap()\n  unreachable\n";
          blockTerminated = true;
          startBlock(okL);
        }
        destroyTemps();
        emitCall("", "void @kpp_eh_rethrow(ptr " + slot + ")");
      }
      else
      {
        const Ty* ty = t->operand->ty ? types.unqualified(types.stripRef(t->operand->ty)) : nullptr;
        if (!ty || !ty->isKnown() || types.isDependent(ty))
          unsupported(t, "throwing a value of unknown type");
        if (ty->kind == TyKind::Array)
          ty = types.pointer(ty->inner);
        ensureMalloc();
        std::string obj = newTemp();
        emitCall(obj + " = ", "ptr @malloc(i64 " + std::to_string(sizeOf(ty, t)) + ")", false);
        if (isClass(ty))
          initializeFrom(obj, ty, t->operand, t);
        else
          store(convert(rvalue(t->operand), ty, t), obj, ty, t);
        std::string ti = typeinfoFor(ty, t), dtor = destructorThunk(ty, t);
        destroyTemps();
        emitCall("", "void @kpp_throw(ptr " + obj + ", ptr " + ti + ", ptr " + dtor + ")");
      }
      // No `unreachable` here, on purpose: the code after a throw stays, so that the optimizer never
      // proves a function noreturn. The SEH scope tables need an instruction after every throwing
      // call; a catch handler placed right at a call's return address would make the runtime skip
      // the cleanups between them, taking the unwind for a jump within the scope.
    }

    // try { body } catch (T1 a) {...} catch (...) {...}: the body's calls unwind to a catchswitch
    // whose filter function (run by the OS during the search phase) matches the thrown type against
    // the handlers in order and records the exception for the one that takes it. The handler code
    // is a catchpad funclet reading that record; leaving it normally ends the exception.
    void tryStatement(TryStmt* t)
    {
      usesExceptions = true;
      markEH();
      std::string endL = newLabel("try.end"), dispatchL = newLabel("catch.dispatch"), handlerL = newLabel("catch.pad");
      ehFrames.push_back({false, dispatchL, currentFunclet(), "", "", scopes.size()});
      statement(t->body);
      emitBranch(endL);
      EHFrame frame = ehFrames.back();
      ehFrames.pop_back();
      if (!frame.used)
      {
        startBlock(endL); // nothing in the body can throw to the handlers
        return;
      }
      // The filter function, named by the function and where the try is written (the same in every
      // run, whatever order the functions are emitted in).
      std::string owner = currentFunctionName.size() > 2 && currentFunctionName[1] == '"' ? currentFunctionName.substr(2, currentFunctionName.size() - 3)
                                                                                          : currentFunctionName.substr(currentFunctionName.empty() ? 0 : 1);
      std::string filter = "@\"kpp.filter." + owner + "." + std::to_string(t->token) + "\"";
      std::string ft = "define i32 " + filter + "(ptr %pointers, ptr %frame) {\nentry:\n"
                       "  %rec = load ptr, ptr %pointers\n  %code = load i32, ptr %rec\n"
                       "  %tip = getelementptr inbounds i8, ptr %rec, i64 40\n  %ti = load ptr, ptr %tip\n"
                       "  %is = icmp eq i32 %code, -536870911\n  br i1 %is, label %check0, label %no\n";
      for (size_t i = 0; i < t->handlers.size(); ++i)
      {
        std::string n = std::to_string(i), nextL = i + 1 < t->handlers.size() ? "check" + std::to_string(i + 1) : "no";
        ft += "check" + n + ":\n";
        if (!t->handlers[i].param)
        {
          ft += "  call void @kpp_eh_push(ptr %rec, i32 " + n + ", i64 0)\n  ret i32 1\n";
          break;
        }
        const Ty* pt = declType(t->handlers[i].param);
        if (!pt || !pt->isKnown())
          unsupported(t->handlers[i].param, "a catch parameter of unknown type");
        ft += "  %off" + n + " = call i64 @kpp_eh_offset(ptr %ti, ptr " + typeinfoFor(pt, t) + ")\n"
              "  %ok" + n + " = icmp sge i64 %off" + n + ", 0\n  br i1 %ok" + n + ", label %yes" + n + ", label %" + nextL + "\n"
              "yes" + n + ":\n  call void @kpp_eh_push(ptr %rec, i32 " + n + ", i64 %off" + n + ")\n  ret i32 1\n";
      }
      ft += "no:\n  ret i32 0\n}\n\n";
      addDefinition(filter, ft);
      // The dispatch and the handlers.
      std::string outer = unwindDestination(); // objects outside the try, then the enclosing try
      startBlock(dispatchL);
      std::string cs = newTemp(), pad = newTemp();
      fn += "  " + cs + " = catchswitch within " + (frame.parentFunclet.empty() ? "none" : frame.parentFunclet) + " [label %" + handlerL +
            "] unwind " + (outer.empty() ? "to caller" : "label %" + outer) + "\n";
      startBlock(handlerL);
      fn += "  " + pad + " = catchpad within " + cs + " [ptr " + filter + "]\n";
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca [6 x i64]\n"; // object, typeinfo, destructor, handler, adjusted object, the handler around this one (kpp.eh.current)
      ehFrames.push_back({true, "", frame.parentFunclet, pad, slot, scopes.size()});
      fn += "  call void @kpp_eh_begin(ptr " + slot + ")" + bundle() + "\n";
      std::vector<std::string> handlerLabels;
      for (size_t i = 0; i < t->handlers.size(); ++i)
        handlerLabels.push_back(newLabel("catch.body"));
      if (t->handlers.size() == 1)
        emitBranch(handlerLabels[0]);
      else
      {
        std::string hp = newTemp(), h = newTemp();
        fn += "  " + hp + " = getelementptr inbounds i8, ptr " + slot + ", i64 24\n  " + h + " = load i64, ptr " + hp + "\n";
        fn += "  switch i64 " + h + ", label %" + handlerLabels[0] + " [";
        for (size_t i = 1; i < t->handlers.size(); ++i)
          fn += " i64 " + std::to_string(i) + ", label %" + handlerLabels[i];
        fn += " ]\n";
        blockTerminated = true;
      }
      for (size_t i = 0; i < t->handlers.size(); ++i)
      {
        const TryStmt::Handler& h = t->handlers[i];
        startBlock(handlerLabels[i]);
        pushScope();
        scopes.back().push_back({slot, nullptr, true});
        if (h.param)
        {
          const Ty* pt = declType(h.param);
          std::string ap = newTemp(), obj = newTemp();
          fn += "  " + ap + " = getelementptr inbounds i8, ptr " + slot + ", i64 32\n  " + obj + " = load ptr, ptr " + ap + "\n";
          if (pt->isReference())
          {
            std::string ref = "%v" + std::to_string(temp++);
            allocas += "  " + ref + " = alloca ptr\n";
            fn += "  store ptr " + obj + ", ptr " + ref + "\n";
            locals[h.param] = {ref, pt, true};
          }
          else if (isClass(pt))
          {
            std::string copy = newObjectSlot(pt, h.param);
            copyConstruct(copy, obj, pt, h.param);
            locals[h.param] = {copy, pt, false};
            if (needsDestruction(pt, h.param))
              scopes.back().push_back({copy, pt});
          }
          else
          {
            std::string v = "%v" + std::to_string(temp++), loaded = newTemp();
            allocas += "  " + v + " = alloca " + llType(pt, h.param) + "\n";
            fn += "  " + loaded + " = load " + llType(pt, h.param) + ", ptr " + obj + "\n  store " + llType(pt, h.param) + " " + loaded + ", ptr " + v + "\n";
            locals[h.param] = {v, pt, false};
          }
        }
        statement(h.body);
        popScope();
        if (!blockTerminated)
        {
          fn += "  catchret from " + pad + " to label %" + endL + "\n";
          blockTerminated = true;
        }
      }
      ehFrames.pop_back();
      startBlock(endL);
    }

    // The exception runtime (linkonce_odr, in every module) and the real @main, which calls the
    // program's main and reports an exception nobody caught.
    void finishExceptionRuntime()
    {
      auto declareOnce = [&](const char* name, const std::string& line)
      {
        if (declaredNames.claim(name))
          decls += line + "\n";
      };
      if (usesExceptions)
      {
        ensureMalloc();
        declareOnce("@RaiseException", "declare dllimport void @RaiseException(i32, i32, i32, ptr)");
        declareOnce("@fprintf", "declare i32 @fprintf(ptr, ptr, ...)");
        declareOnce("@__acrt_iob_func", "declare ptr @__acrt_iob_func(i32)");
        declareOnce("@abort", "declare void @abort()");
        markEH();
        emittingShared = true;
        std::string fmt1 = cStringConstant("unhandled exception of type %s"), fmt2 = cStringConstant(": %s"), fmt3 = cStringConstant("\n");
        emittingShared = false;
        shared += sharedComdat("@kpp.eh.stack") + "@kpp.eh.stack = linkonce_odr thread_local global [160 x i64] zeroinitializer, comdat\n" +
                  sharedComdat("@kpp.eh.depth") + "@kpp.eh.depth = linkonce_odr thread_local global i64 0, comdat\n";
        shared += sharedComdat("@kpp_throw") +
                  "define linkonce_odr void @kpp_throw(ptr %object, ptr %typeinfo, ptr %dtor) noinline comdat {\nentry:\n"
                  "  %args = alloca [3 x i64]\n  %a1 = getelementptr inbounds i64, ptr %args, i64 1\n  %a2 = getelementptr inbounds i64, ptr %args, i64 2\n"
                  "  store ptr %object, ptr %args\n  store ptr %typeinfo, ptr %a1\n  store ptr %dtor, ptr %a2\n"
                  "  call void @RaiseException(i32 -536870911, i32 1, i32 3, ptr %args)\n  ret void\n}\n";
        shared += baseSearchFunction("@kpp_eh_offset");
        shared += sharedComdat("@kpp_eh_push") +
                  "define linkonce_odr void @kpp_eh_push(ptr %record, i32 %handler, i64 %offset) comdat {\nentry:\n"
                  "  %d = load i64, ptr @kpp.eh.depth\n  %i = mul i64 %d, 5\n  %slot = getelementptr inbounds i64, ptr @kpp.eh.stack, i64 %i\n"
                  "  %info = getelementptr inbounds i8, ptr %record, i64 32\n  %obj = load i64, ptr %info\n"
                  "  %tip = getelementptr inbounds i8, ptr %record, i64 40\n  %ti = load i64, ptr %tip\n"
                  "  %dp = getelementptr inbounds i8, ptr %record, i64 48\n  %dtor = load i64, ptr %dp\n"
                  "  %h = zext i32 %handler to i64\n  %adj = add i64 %obj, %offset\n"
                  "  %s1 = getelementptr inbounds i64, ptr %slot, i64 1\n  %s2 = getelementptr inbounds i64, ptr %slot, i64 2\n"
                  "  %s3 = getelementptr inbounds i64, ptr %slot, i64 3\n  %s4 = getelementptr inbounds i64, ptr %slot, i64 4\n"
                  "  store i64 %obj, ptr %slot\n  store i64 %ti, ptr %s1\n  store i64 %dtor, ptr %s2\n  store i64 %h, ptr %s3\n  store i64 %adj, ptr %s4\n"
                  "  %d1 = add i64 %d, 1\n  store i64 %d1, ptr @kpp.eh.depth\n  ret void\n}\n";
        // A handler's record is the current exception while it runs (kpp.eh.current, the record of
        // the handler around it kept in its sixth word): what `throw;` in a function it calls raises.
        shared += sharedComdat("@kpp.eh.current") + "@kpp.eh.current = linkonce_odr thread_local global ptr null, comdat\n";
        shared += sharedComdat("@kpp_eh_begin") +
                  "define linkonce_odr void @kpp_eh_begin(ptr %out) comdat {\nentry:\n"
                  "  %d = load i64, ptr @kpp.eh.depth\n  %d1 = sub i64 %d, 1\n  store i64 %d1, ptr @kpp.eh.depth\n"
                  "  %i = mul i64 %d1, 5\n  %slot = getelementptr inbounds i64, ptr @kpp.eh.stack, i64 %i\n"
                  "  call void @llvm.memcpy.p0.p0.i64(ptr %out, ptr %slot, i64 40, i1 false)\n"
                  "  %outer = load ptr, ptr @kpp.eh.current\n  %op = getelementptr inbounds i8, ptr %out, i64 40\n  store ptr %outer, ptr %op\n"
                  "  store ptr %out, ptr @kpp.eh.current\n  ret void\n}\n";
        shared += sharedComdat("@kpp_eh_end") +
                  "define linkonce_odr void @kpp_eh_end(ptr %out) comdat {\nentry:\n"
                  "  %op = getelementptr inbounds i8, ptr %out, i64 40\n  %outer = load ptr, ptr %op\n  store ptr %outer, ptr @kpp.eh.current\n"
                  "  %obj = load ptr, ptr %out\n  %null = icmp eq ptr %obj, null\n  br i1 %null, label %done, label %live\n"
                  "live:\n  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n"
                  "  %nodtor = icmp eq ptr %dtor, null\n  br i1 %nodtor, label %release, label %destroy\n"
                  "destroy:\n  call void %dtor(ptr %obj)\n  br label %release\n"
                  "release:\n  call void @free(ptr %obj)\n  store ptr null, ptr %out\n  br label %done\n"
                  "done:\n  ret void\n}\n";
        shared += sharedComdat("@kpp_eh_rethrow") +
                  "define linkonce_odr void @kpp_eh_rethrow(ptr %out) noinline comdat {\nentry:\n"
                  "  %obj = load ptr, ptr %out\n  %tp = getelementptr inbounds i8, ptr %out, i64 8\n  %ti = load ptr, ptr %tp\n"
                  "  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n  store ptr null, ptr %out\n"
                  "  call void @kpp_throw(ptr %obj, ptr %ti, ptr %dtor)\n  ret void\n}\n";
        shared += sharedComdat("@kpp.eh.filter.any") +
                  "define linkonce_odr i32 @kpp.eh.filter.any(ptr %pointers, ptr %frame) comdat {\nentry:\n"
                  "  %rec = load ptr, ptr %pointers\n  %code = load i32, ptr %rec\n  %is = icmp eq i32 %code, -536870911\n"
                  "  br i1 %is, label %yes, label %no\nyes:\n  call void @kpp_eh_push(ptr %rec, i32 0, i64 0)\n  ret i32 1\nno:\n  ret i32 0\n}\n";
        std::string message;
        if (!exceptionTypeinfo.empty())
          message = "  %off = call i64 @kpp_eh_offset(ptr %typeinfo, ptr " + exceptionTypeinfo + ")\n  %isExc = icmp sge i64 %off, 0\n"
                    "  br i1 %isExc, label %msg, label %end\n"
                    "msg:\n  %ep = getelementptr inbounds i8, ptr %object, i64 %off\n  %data = load ptr, ptr %ep\n"
                    "  %nodata = icmp eq ptr %data, null\n  br i1 %nodata, label %end, label %print\n"
                    "print:\n  %r2 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt2 + ", ptr %data)\n  br label %end\n";
        else
          message = "  br label %end\n";
        shared += sharedComdat("@kpp_eh_unhandled") +
                  "define linkonce_odr void @kpp_eh_unhandled(ptr %object, ptr %typeinfo) noinline comdat {\nentry:\n"
                  "  %err = call ptr @__acrt_iob_func(i32 2)\n  %name = load ptr, ptr %typeinfo\n"
                  "  %r1 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt1 + ", ptr %name)\n" + message +
                  "end:\n  %r3 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt3 + ")\n  call void @abort()\n  unreachable\n}\n";
        declareOnce("@llvm.memcpy.p0.p0.i64", "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)");
      }
      // @main, or @wmain for a program whose entry takes wide arguments (the game's, on Windows: the
      // C runtime calls whichever the program defines).
      for (const char* entry : {"main", "wmain"})
        emitEntry(entry, "@kpp." + std::string(entry));
    }

    void emitEntry(const std::string& entry, const std::string& mainName)
    {
      std::string declare = declareLines[mainName];
      if (moduleCount <= 1)
      {
        std::string definition = body.fromFirst("define i32 " + mainName + "(");
        if (definition.empty())
          return;
        declare = definition.substr(0, definition.find(")") + 1);
      }
      if (declare.empty())
        return;
      bool args = declare.find("(i32, ptr)") != std::string::npos || declare.find("(i32 %p0, ptr %p1)") != std::string::npos;
      std::string call = "i32 " + mainName + (args ? "(i32 %argc, ptr %argv)" : "()");
      std::string text = "define i32 @" + entry + "(i32 %argc, ptr %argv) " + std::string(usesExceptions ? "personality ptr @__C_specific_handler " : "") + "{\nentry:\n";
      if (!usesExceptions)
        text += "  %r = call " + call + "\n  ret i32 %r\n}\n\n";
      else
        text += "  %out = alloca [6 x i64]\n  %r = invoke " + call + " to label %done unwind label %dispatch\n"
                "done:\n  ret i32 %r\n"
                "dispatch:\n  %cs = catchswitch within none [label %handler] unwind to caller\n"
                "handler:\n  %pad = catchpad within %cs [ptr @kpp.eh.filter.any]\n"
                "  call void @kpp_eh_begin(ptr %out) [ \"funclet\"(token %pad) ]\n"
                "  %obj = load ptr, ptr %out\n  %tp = getelementptr inbounds i8, ptr %out, i64 8\n  %ti = load ptr, ptr %tp\n"
                "  call void @kpp_eh_unhandled(ptr %obj, ptr %ti) [ \"funclet\"(token %pad) ]\n"
                "  catchret from %pad to label %failed\n"
                "failed:\n  ret i32 3\n}\n\n";
      if (moduleCount <= 1)
        body += text;
      else
      {
        Shared::Definition definition = Shared::definitionOf(text);
        auto lock = sharedLock();
        sh.definitions[definedIn[mainName]].push_back(std::move(definition));
      }
    }

    // `static T x = ...;` inside a function: a global of the program (in the module of the function),
    // constructed on the first pass through the declaration, never destroyed. Not thread-safe yet.
    // `thread_local T x;` (static or not): one per thread, the guard too, so each thread builds its
    // own on its first pass (destroyed at no thread's exit yet).
    void staticLocal(VariableDecl* v, const Ty* t)
    {
      // `static const Map<String, SDL_Scancode>& keyStrings = generate();`: a pointer, bound on the
      // first pass to what the initializer names (an lvalue: a reference bound to a temporary would
      // need the temporary kept for the program).
      bool isRef = t->isReference();
      if (isRef && (!v->init || !(isLvalueExpression(v->init) || (v->init->ty && v->init->ty->isReference()))))
        unsupported(v, "a static local reference bound to a temporary");
      std::string owner = currentFunctionName;
      if (owner.rfind("@\"", 0) == 0)
        owner = owner.substr(2, owner.size() - 3);
      else if (!owner.empty() && owner[0] == '@')
        owner = owner.substr(1);
      // Numbered by where the variable is written (its token): the same name in every run.
      std::string name = "@\"" + owner + "::" + sym(v->name.last().symbol) + "." + std::to_string(v->token) + "\"";
      std::string guard = name.substr(0, name.size() - 1) + ".guard\"";
      std::string storage = (v->flags & DF_ThreadLocal) ? " = internal thread_local global " : " = internal global ";
      if (isRef)
        addOwnedEntry(name + storage + "ptr null\n");
      else
        addOwnedEntry(name + storage + llType(t, v) + " zeroinitializer" + alignSuffix(t, variableAlignment(v, typingContext(), v), v) + "\n");
      addOwnedEntry(guard + storage + "i1 false\n");
      {
        auto lock = sharedLock();
        globals[v] = name;
      }
      locals[v] = {name, t, isRef};
      std::string done = newLabel("static.done"), init = newLabel("static.init");
      std::string flag = newTemp();
      fn += "  " + flag + " = load i1, ptr " + guard + "\n";
      fn += "  br i1 " + flag + ", label %" + done + ", label %" + init + "\n";
      startBlock(init);
      fn += "  store i1 true, ptr " + guard + "\n";
      if (isRef)
        fn += "  store ptr " + lvalue(v->init) + ", ptr " + name + "\n";
      else if (v->init)
        initializeFrom(name, t, v->init, v);
      else if (isClass(t))
        defaultConstruct(name, t, v);
      else if (t->kind == TyKind::Array && needsConstruction(t, v))
        constructArray(name, t, v);
      destroyTemps();
      emitBranch(done);
      startBlock(done);
    }

    // `auto [a, b] = e;` binding tuple-like: after the object, the hidden variables `auto&& a =
    // get<0>(e)` and so on that the bindings' names name (the resolver's tupleBindings).
    void declareTupleBindings(VariableDecl* v)
    {
      for (Expr* b : v->structuredBindings)
        if (Decl* own = static_cast<NameExpr*>(b)->resolved; own && own->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(own)->tupleBinding)
        {
          declareLocal(static_cast<VariableDecl*>(own));
          destroyTemps();
        }
    }

    void declareLocal(VariableDecl* v)
    {
      const Ty* t = declType(v);
      if ((!t || !t->isKnown() || types.isDependent(t)) && v->init && v->init->ty)
      {
        // Fall back to the initializer's type when the declared type is unknown (e.g. auto).
        const Ty* initTy = types.stripRef(v->init->ty);
        if (initTy && initTy->isKnown() && !types.isDependent(initTy))
          t = initTy;
      }
      if ((!t || !t->isKnown() || types.isDependent(t)) && v->init && v->init->kind == NodeKind::NameExpr)
        if (auto local = locals.find(static_cast<NameExpr*>(v->init)->resolved); local != locals.end() && local->second.ty && local->second.ty->kind == TyKind::Vector)
          t = local->second.ty; // `auto xmm3 = xmm6;` of an SSE `auto` local, which the code generator typed
      if ((!t || !t->isKnown() || types.isDependent(t)) && v->init && v->init->kind == NodeKind::CallExpr && v->type &&
          v->type->kind == NodeKind::AutoType && !(v->flags & (DF_Static | DF_ThreadLocal)))
      {
        // `auto xmm0 = _mm_loadu_si128(...)`: an SSE operation, which the code generator types (the
        // type checker does not know them): the local has the vector type of its value.
        auto* call = static_cast<CallExpr*>(v->init);
        Decl* callee = call->callee && call->callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(call->callee)->resolved : nullptr;
        if (call->callee && call->callee->kind == NodeKind::NameExpr && (!callee || (callee->kind == NodeKind::FunctionDecl && callee->file && callee->file->isImported)))
          if (std::optional<Value> value = sseBuiltin(static_cast<NameExpr*>(call->callee), call); value && value->ty && value->ty->kind == TyKind::Vector)
          {
            std::string slot = "%v" + std::to_string(temp++);
            allocas += "  " + slot + " = alloca " + llType(value->ty, v) + "\n";
            fn += "  store " + llType(value->ty, v) + " " + value->text + ", ptr " + slot + "\n";
            locals[v] = {slot, value->ty, false};
            return;
          }
      }
      if (!t || !t->isKnown() || types.isDependent(t))
        unsupported(v, "a local of unknown type (" + (v->name.empty() ? std::string("[bindings]") : sym(v->name.last().symbol)) + ")");
      // `char buffer[N]` with N no constant the checker could evaluate has the length 0 (writes
      // would run past it).
      for (const Ty* a = types.unqualified(t); a && a->kind == TyKind::Array; a = types.unqualified(a->inner))
        if (a->count == 0)
          fail(v, "error: the local " + (v->name.empty() ? std::string("[bindings]") : sym(v->name.last().symbol)) +
                      " is an array whose length is no constant the compiler could evaluate");
      // (`thread_local SignalsVector signals;` is static too: the function returns a reference to it.)
      if ((v->flags & (DF_Static | DF_ThreadLocal)) && !(v->flags & DF_Typedef))
      {
        staticLocal(v, t);
        return;
      }
      bool isRef = t->isReference();
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca " + (isRef ? std::string("ptr") : llType(t, v) + alignSuffix(t, variableAlignment(v, typingContext(), v), v)) + "\n";
      locals[v] = {slot, t, isRef};
      if (isRef)
      {
        if (!v->init)
          unsupported(v, "a reference without an initializer");
        // `const Sprite& s(expr);`, `T& r{expr};`: a reference initialised directly binds to the one
        // value, as `= expr` does (a copy into a temporary needs a copy constructor, Sprite's is
        // deleted).
        Expr* init = v->init;
        if (init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(init)->type == v->type &&
            static_cast<FunctionalCastExpr*>(init)->args.size() == 1)
          init = static_cast<FunctionalCastExpr*>(init)->args[0];
        if (init->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(init)->args.size() == 1 &&
            static_cast<BraceInitExpr*>(init)->designators.empty())
          init = static_cast<BraceInitExpr*>(init)->args[0];
        std::string addr;
        if (isClass(types.stripRef(t)))
        {
          // `const Sorted& r = path;`, `const String& s = "text";`: a value of another type that is
          // neither of a derived class nor converts by an operator binds to a temporary the class's
          // converting constructor makes, which lives as long as the reference.
          const Ty* target = types.unqualified(types.stripRef(t));
          const Ty* initTy = init->ty ? withCurrentArgs(types.unqualified(types.stripRef(init->ty))) : nullptr;
          if (initTy && initTy->isKnown() && !types.isDependent(initTy) && (!isClass(initTy) || canonicalClass(initTy, v) != canonicalClass(target, v)))
          {
            std::vector<std::pair<std::string, uint32_t>> path;
            const Ty* owner = nullptr;
            TemplateArgs opArgs;
            bool converts = isClass(initTy) && (basePath(initTy, target, path, v) || conversionOperator(initTy, target, v, &owner, false, &opArgs));
            if (!converts && findConstructor(target, {init->ty}, v))
            {
              std::string tmp = newObjectSlot(target, v);
              Expr* one = init;
              constructInto(tmp, target, Span<Expr*>{&one, 1}, v);
              if (needsDestruction(target, v))
                scopes.back().push_back({tmp, target});
              fn += "  store ptr " + tmp + ", ptr " + slot + "\n";
              return;
            }
          }
          Value o = rvalue(init);
          // `const Label& l = wrapper;`: an object of another class is converted, to its base or
          // through its conversion operator (reference_wrapper's `operator T&()`), never bound as
          // if it were the referenced class.
          if (const Ty* target = types.unqualified(types.stripRef(t)); isClass(o.ty) && !types.isDependent(o.ty) && canonicalClass(o.ty, v) != canonicalClass(target, v))
            o = convert(o, target, v);
          addr = o.text;
          if (o.isTemporary)
          {
            for (size_t i = statementTemps.size(); i-- > 0;)
              if (statementTemps[i].addr == addr)
                statementTemps.erase(statementTemps.begin() + i);
            scopes.back().push_back({addr, types.stripRef(t)});
          }
        }
        else
          addr = scalarReferenceTarget(t, init, v);
        fn += "  store ptr " + addr + ", ptr " + slot + "\n";
        return;
      }
      if (v->init)
        initializeFrom(slot, t, v->init, v);
      else if (isClass(t))
        defaultConstruct(slot, t, v);
      else if (t->kind == TyKind::Array && needsConstruction(t, v))
        constructArray(slot, t, v); // `StringView text[2];`
      else
        storeZero(t, slot, v);
      if (needsDestruction(t, v))
        scopes.back().push_back({slot, t});
    }

    void statement(Stmt* s)
    {
      if (!s)
        return;
      if (++recursionDepth > 128)
      {
        recursionDepth = 0;
        fail(s, "statement nested too deeply");
      }
      struct DepthGuard { int& d; ~DepthGuard() { --d; } } dg{recursionDepth};
      if (blockTerminated)
        startBlock(newLabel("dead"));
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          pushScope();
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            statement(inner);
          popScope();
          break;
        case NodeKind::ExprStmt:
          if (static_cast<ExprStmt*>(s)->expr)
          {
            layOutClosuresIn(static_cast<ExprStmt*>(s)->expr);
            rvalue(static_cast<ExprStmt*>(s)->expr);
          }
          destroyTemps();
          break;
        case NodeKind::GotoStmt:
        {
          // A jump to a label of this function (no destructors run on the way: the labels Factorio
          // uses jump forward out of loops, past nothing that needs destroying).
          uint32_t symbol = static_cast<GotoStmt*>(s)->label;
          if (!labelBlocks.count(symbol))
            labelBlocks[symbol] = newLabel("goto");
          fn += "  br label %" + labelBlocks[symbol] + "\n";
          blockTerminated = true;
          break;
        }
        case NodeKind::LabelStmt:
        {
          uint32_t symbol = static_cast<LabelStmt*>(s)->label;
          if (!labelBlocks.count(symbol))
            labelBlocks[symbol] = newLabel("goto");
          emitBranch(labelBlocks[symbol]);
          startBlock(labelBlocks[symbol]);
          break;
        }
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
          {
            if (d->kind == NodeKind::VariableDecl)
            {
              layOutClosuresIn(static_cast<VariableDecl*>(d)->init); // the variable's slot may hold one
              declareLocal(static_cast<VariableDecl*>(d));
              destroyTemps();
              declareTupleBindings(static_cast<VariableDecl*>(d));
            }
            else if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                     d->kind == NodeKind::UsingDirectiveDecl || d->kind == NodeKind::UsingNameDecl ||
                     d->kind == NodeKind::EmptyDecl || d->kind == NodeKind::StaticAssertDecl)
              continue; // names only (`using namespace gui;` in a body): resolved, nothing to emit
            else
              unsupported(d, "this local declaration");
          }
          break;
        case NodeKind::EmptyStmt:
          break;
        case NodeKind::ReturnStmt:
        {
          auto* r = static_cast<ReturnStmt*>(s);
          layOutClosuresIn(r->value);
          std::string retText;
          if (currentReturn->kind == TyKind::Void)
          {
            if (r->value)
              rvalue(r->value);
          }
          else if (returnsInMemory(currentReturn))
          {
            if (!r->value)
              unsupported(s, "return without a value");
            if (returnedLocalObject(r->value))
              moveConstruct(sretAddr, lvalue(r->value), currentReturn, s); // `return result;`: the local is moved, as in C++
            else if (const Ty* local = localObjectOfOtherClass(r->value); local && findConstructor(currentReturn, {types.rref(local)}, s))
            {
              // `return obj;` with obj a local of another class (UniquePtr<Derived> returned as
              // UniquePtr<Base>): C++ takes the local as an rvalue first, for the converting
              // constructor that takes one.
              NamedCastExpr moved{};
              moved.kind = NodeKind::NamedCastExpr;
              moved.token = r->value->token;
              moved.castKind = Kw::static_cast_;
              moved.operand = r->value;
              moved.ty = types.rref(local);
              initializeFrom(sretAddr, currentReturn, &moved, s);
            }
            else
              initializeFrom(sretAddr, currentReturn, r->value, s);
          }
          else if (currentReturn->isReference())
          {
            if (!r->value)
              unsupported(s, "return without a value in a function returning a reference");
            if (isAggregate(types.stripRef(currentReturn)))
            {
              Value v = rvalue(r->value);
              if (v.text.empty() || !v.ty || v.ty->kind == TyKind::Void)
                fail(r->value, "error: a function returning " + types.toString(currentReturn, &symbols) + " returns an expression without a value (of type void)");
              // A derived object returned as a base's reference is that base's subobject:
              // `PendingRobotsSetHook& asPendingRobotsHook() { return *this; }` returned the robot's
              // start, and unlinking that "hook" walked the vtable as its tree.
              const Ty* want = types.unqualified(types.stripRef(currentReturn));
              if (isClass(v.ty) && isClass(want) && classKey(v.ty) != classKey(want))
                v = convert(v, want, s);
              retText = v.text;
            }
            else
              retText = lvalue(r->value);
          }
          else
          {
            if (!r->value)
              unsupported(s, "return without a value in a non-void function");
            if (r->value->kind == NodeKind::BraceInitExpr)
              retText = braceScalar(static_cast<BraceInitExpr*>(r->value), currentReturn, s).text; // `return {};` of an enum
            else
              retText = convert(rvalue(r->value), currentReturn, s).text;
          }
          destroyTemps();
          std::string retLl = currentReturn->kind == TyKind::Void || returnsInMemory(currentReturn) ? std::string()
                              : currentReturn->isReference()                                ? std::string("ptr")
                                                                                             : llType(currentReturn, s);
          if (!retLl.empty() && !currentFunclet().empty())
          {
            // Leaving a catch handler: the value crosses a funclet boundary through memory.
            std::string slot = "%v" + std::to_string(temp++);
            allocas += "  " + slot + " = alloca " + retLl + "\n";
            fn += "  store " + retLl + " " + retText + ", ptr " + slot + "\n";
            leaveScopes(0);
            retText = newTemp();
            fn += "  " + retText + " = load " + retLl + ", ptr " + slot + "\n";
          }
          else
            leaveScopes(0);
          if (retLl.empty() && !returnLabel.empty())
            fn += "  br label %" + returnLabel + "\n"; // a destructor's: its members and bases are destroyed there
          else if (retLl.empty())
            fn += "  ret void\n";
          else
            fn += "  ret " + retLl + " " + retText + "\n";
          blockTerminated = true;
          break;
        }
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          if (i->isConstexpr && i->constexprKnown >= 0)
          {
            // The resolver already folded this if constexpr during instantiation.
            statement(i->constexprKnown ? i->then : i->else_);
            break;
          }
          if (i->isConstexpr && !i->init && i->cond && i->cond->kind != NodeKind::VariableDecl && i->cond->kind != NodeKind::DeclStmt)
            if (std::optional<bool> known = evalCondition(static_cast<Expr*>(i->cond)))
            {
              // if constexpr with a known condition: only the taken branch exists.
              statement(*known ? i->then : i->else_);
              break;
            }
          pushScope();
          if (i->init)
            statement(i->init);
          Value cond = condition(i->cond, s);
          destroyTemps();
          std::string tL = newLabel("if.then"), eL = newLabel("if.else"), jL = newLabel("if.end");
          fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + (i->else_ ? eL : jL) + "\n";
          startBlock(tL);
          statement(i->then);
          emitBranch(jL);
          if (i->else_)
          {
            startBlock(eL);
            statement(i->else_);
            emitBranch(jL);
          }
          startBlock(jL);
          popScope();
          break;
        }
        case NodeKind::AssertStmt:
        {
          auto* a = static_cast<AssertStmt*>(s);
          if (!a->release && !debug)
            break; // assert compiles to nothing outside debug builds
          Value cond = condition(a->cond, s);
          destroyTemps();
          std::string okL = newLabel("assert.ok"), failL = newLabel("assert.fail");
          fn += "  br i1 " + cond.text + ", label %" + okL + ", label %" + failL + "\n";
          startBlock(failL);
          auto [line, column] = lineColumnOf(a->token);
          emitCall("", "void @kpp_assert_failed(ptr " + cStringConstant(currentFile->path) + ", i32 " + std::to_string(line) + ", i32 " +
                           std::to_string(column) + ", ptr " + cStringConstant(sourceText(a->startToken, a->endToken)) + ")", false);
          fn += "  unreachable\n";
          usesAssert = true;
          startBlock(okL);
          break;
        }
        case NodeKind::WhileStmt:
        {
          auto* w = static_cast<WhileStmt*>(s);
          std::string cL = newLabel("while.cond"), bL = newLabel("while.body"), eL = newLabel("while.end");
          emitBranch(cL);
          startBlock(cL);
          Value cond = condition(w->cond, s);
          destroyTemps();
          fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
          startBlock(bL);
          loops.push_back({eL, cL, scopes.size()});
          statement(w->body);
          loops.pop_back();
          emitBranch(cL);
          startBlock(eL);
          break;
        }
        case NodeKind::DoStmt:
        {
          auto* d = static_cast<DoStmt*>(s);
          std::string bL = newLabel("do.body"), cL = newLabel("do.cond"), eL = newLabel("do.end");
          emitBranch(bL);
          startBlock(bL);
          loops.push_back({eL, cL, scopes.size()});
          statement(d->body);
          loops.pop_back();
          emitBranch(cL);
          startBlock(cL);
          Value cond = convert(rvalue(d->cond), types.boolType(), s);
          destroyTemps();
          fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
          startBlock(eL);
          break;
        }
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          pushScope();
          if (f->init)
            statement(f->init);
          std::string cL = newLabel("for.cond"), bL = newLabel("for.body"), sL = newLabel("for.step"), eL = newLabel("for.end");
          emitBranch(cL);
          startBlock(cL);
          if (f->cond)
          {
            Value cond = condition(f->cond, s);
            destroyTemps();
            fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
          }
          else
            fn += "  br label %" + bL + "\n";
          startBlock(bL);
          loops.push_back({eL, sL, scopes.size()});
          statement(f->body);
          loops.pop_back();
          emitBranch(sL);
          startBlock(sL);
          if (f->step)
          {
            rvalue(f->step);
            destroyTemps();
          }
          emitBranch(cL);
          startBlock(eL);
          popScope();
          break;
        }
        case NodeKind::BreakStmt:
          if (loops.empty())
            unsupported(s, "break outside a loop or switch");
          leaveScopes(loops.back().scopeDepth);
          emitBranch(loops.back().breakLabel);
          break;
        case NodeKind::ContinueStmt:
        {
          for (size_t i = loops.size(); i-- > 0;)
            if (!loops[i].continueLabel.empty())
            {
              leaveScopes(loops[i].scopeDepth);
              emitBranch(loops[i].continueLabel);
              return;
            }
          unsupported(s, "continue outside a loop");
        }
        case NodeKind::SwitchStmt:
          switchStatement(static_cast<SwitchStmt*>(s));
          break;
        case NodeKind::TryStmt:
          tryStatement(static_cast<TryStmt*>(s));
          break;
        case NodeKind::RangeForStmt:
          rangeFor(static_cast<RangeForStmt*>(s));
          break;
        case NodeKind::ForConstexprStmt:
          forConstexpr(static_cast<ForConstexprStmt*>(s));
          break;
        case NodeKind::CaseStmt:
        case NodeKind::DefaultStmt:
          unsupported(s, "a case label outside a switch body");
        case NodeKind::FallthroughStmt:
          break;
        default:
          unsupported(s, std::string("the statement kind ") + nodeKindName(s->kind));
      }
    }

    // Finds a zero-argument member function by name (searching bases), with the class it lives in.
    FunctionDecl* findMember(const Ty* classTy, const std::string& name, Node* at, const Ty** foundIn)
    {
      return findOperator(classTy, name, 0, at, foundIn);
    }

    // A range whose begin()/end() return iterator objects: `it != end`, `*it` and `++it` go through
    // the iterator class's operators. Both iterators live in the loop's scope (already open).
    void iteratorLoop(RangeForStmt* f, Value begin, Value end)
    {
      auto own = [&](const Value& v)
      {
        for (size_t i = statementTemps.size(); i-- > 0;)
          if (statementTemps[i].addr == v.text)
            statementTemps.erase(statementTemps.begin() + i);
        if (needsDestruction(v.ty, f))
          scopes.back().push_back({v.text, v.ty});
      };
      own(begin);
      own(end);
      destroyTemps();
      // `it != end`, or `!(it == end)`; end may be of another type than the iterator (a sentinel:
      // CircleIterator's `nullptr_t end()` and `bool operator!=(nullptr_t)`).
      const Ty* in = nullptr;
      std::vector<const Ty*> argTypes{isClass(end.ty) ? types.lref(types.qualified(end.ty, true, false)) : end.ty};
      bool negate = false;
      FunctionDecl* compare = findMemberFunction(begin.ty, "operator!=", argTypes, begin.ty, f, &in);
      if (!compare)
      {
        compare = findMemberFunction(begin.ty, "operator==", argTypes, begin.ty, f, &in);
        negate = true;
      }
      if (!compare)
        unsupported(f, "an iterator without operator!= or operator==");
      TemplateArgs compareArgs = global.classArgsOf(in);
      std::string compareName = useFunction(compare, compareArgs, in);
      Signature compareSig;
      {
        ArgsScope scope(*this, &compareArgs);
        compareSig = signatureOf(compare);
      }
      std::string cL = newLabel("iter.cond"), bL = newLabel("iter.body"), sL = newLabel("iter.step"), eL = newLabel("iter.end");
      emitBranch(cL);
      startBlock(cL);
      std::string thisArg = begin.text;
      if (canonicalClass(begin.ty, f) != in)
        baseAddress(begin.ty, in, begin.text, thisArg, f);
      std::string endArg = "ptr " + end.text;
      if (!isClass(end.ty) && compareSig.paramTys.size() > 1 && compareSig.paramTys[1])
      {
        const Ty* pt = compareSig.paramTys[1];
        if (pt->isReference())
        {
          std::string slot = newObjectSlot(end.ty, f);
          store(end, slot, end.ty, f);
          endArg = "ptr " + slot;
        }
        else
          endArg = compareSig.paramTypes[1] + " " + convert(end, pt, f).text;
      }
      std::string cond = newTemp();
      emitCall(cond + " = ", compareSig.returnType + " " + compareName + "(ptr " + thisArg + ", " + endArg + ")");
      if (compareSig.returnType != "i1")
        cond = convert(Value{cond, types.intType(32)}, types.boolType(), f).text;
      if (negate)
      {
        std::string flipped = newTemp();
        fn += "  " + flipped + " = xor i1 " + cond + ", true\n";
        cond = flipped;
      }
      fn += "  br i1 " + cond + ", label %" + bL + ", label %" + eL + "\n";
      startBlock(bL);
      pushScope();
      // The loop variable from `*it`: a reference binds to the element, a value copies it.
      auto* v = static_cast<VariableDecl*>(f->var);
      const Ty* vt = declType(v);
      if (!vt || !vt->isKnown() || types.isDependent(vt))
        unsupported(v, "a loop variable of unknown type");
      Value element = operatorCall(begin, "operator*", {}, f, true);
      std::string slot = "%v" + std::to_string(temp++);
      if (vt->isReference())
      {
        std::string addr = element.text;
        if (!element.ty->isReference() && !isClass(element.ty))
        {
          addr = newObjectSlot(element.ty, v);
          fn += "  store " + llType(element.ty, v) + " " + element.text + ", ptr " + addr + "\n";
        }
        if (elementMadeInto(element.ty, vt))
        {
          // `for (const String& name : names)` over a Vector<const char*>: a String made from the
          // element, the reference's (it lives for the iteration).
          const Ty* cls = types.unqualified(types.stripRef(vt));
          std::string made = newObjectSlot(cls, v);
          makeFromElement(made, cls, addr, element.ty, v);
          if (needsDestruction(cls, v))
            scopes.back().push_back({made, cls});
          addr = made;
        }
        else
          addr = elementAs(addr, element.ty, vt, v);
        allocas += "  " + slot + " = alloca ptr\n";
        fn += "  store ptr " + addr + ", ptr " + slot + "\n";
        locals[v] = {slot, vt, true};
      }
      else
      {
        allocas += "  " + slot + " = alloca " + llType(vt, v) + "\n";
        if (elementMadeInto(element.ty, vt))
        {
          // `for (String name : names)` over a Vector<const char*>: made from the element.
          std::string addr = resultAddress(element, v);
          makeFromElement(slot, vt, addr, element.ty, v);
        }
        else if (isClass(vt) && elementOfOtherClass(element.ty, vt, v))
          copyConstruct(slot, elementAs(element.text, element.ty, vt, v), vt, v); // an element of another class, converted
        else if (isClass(vt))
        {
          if (element.ty->isReference() || !element.isTemporary)
            copyConstruct(slot, element.text, vt, v);
          else
            moveTemporary(slot, element, vt, v);
        }
        else
          store(callRvalue(element, v), slot, vt, v);
        locals[v] = {slot, vt, false};
        if (needsDestruction(vt, v))
          scopes.back().push_back({slot, vt});
      }
      destroyTemps();
      declareTupleBindings(v); // `for (auto& [a, b] : tuples)`: each element's get<i>
      loops.push_back({eL, sL, scopes.size()});
      statement(f->body);
      loops.pop_back();
      popScope();
      emitBranch(sL);
      startBlock(sL);
      operatorCall(begin, "operator++", {}, f, true);
      destroyTemps();
      emitBranch(cL);
      startBlock(eL);
    }

    // --- typed unions (design.md 8.5): a tag and the storage of one alternative ---

    ClassDecl* typedUnionOf(const Ty* t, Node* at)
    {
      t = t ? types.unqualified(types.stripRef(t)) : nullptr;
      if (!isClass(t))
        return nullptr;
      ClassDecl* c = classDef(t->decl, at);
      return c && c->isTypedUnion ? c : nullptr;
    }
    const Ty* tagType(ClassDecl* c) { return types.enumType(c->tagEnum); }
    std::string tagAddress(ClassDecl* c, const Ty* t, const std::string& obj, Node* at)
    {
      std::string addr;
      if (!fieldAddress(types.unqualified(types.stripRef(t)), c->tagField, obj, addr, at))
        unsupported(at, "a typed union without its tag");
      return addr;
    }
    std::string loadTag(ClassDecl* c, const Ty* t, const std::string& obj, Node* at)
    {
      return load(tagAddress(c, t, obj, at), tagType(c), at).text;
    }
    const Ty* alternativeType(ClassDecl* c, const Ty* t, size_t index, Node* at)
    {
      const Ty* cls = canonicalClass(types.unqualified(types.stripRef(t)), at);
      const Ty* alt = global.typeOfDeclIn(c->alternatives[index], global.classArgsOf(cls));
      if (!alt || !alt->isKnown())
        unsupported(at, "an alternative of a typed union whose type is unknown");
      return alt;
    }
    std::string alternativeAddress(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Node* at)
    {
      std::string addr;
      if (!fieldAddress(types.unqualified(types.stripRef(t)), c->alternatives[index], obj, addr, at))
        unsupported(at, "a typed union without its storage");
      return addr;
    }
    // The alternative of typed union c (of type t) a value of type valueTy sets (design.md 8.5):
    // the one whose type is valueTy without references and const. None, or two of that type, is
    // a compile error (two: set it by name, `u[Text] = x`).
    size_t alternativeForValue(ClassDecl* c, const Ty* t, const Ty* valueTy, Node* at)
    {
      const Ty* v = types.unqualified(types.stripRef(valueTy));
      std::vector<size_t> found;
      for (size_t i = 0; i < c->alternatives.size(); ++i)
        if (types.unqualified(alternativeType(c, t, i, at)) == v)
          found.push_back(i);
      auto name = [&](size_t i) { return sym(static_cast<EnumeratorDecl*>(static_cast<EnumDecl*>(c->tagEnum)->enumerators[i])->name); };
      if (found.empty())
        fail(at, "the typed union " + qualifiedName(c) + " has no alternative of type " + types.toString(v, &symbols));
      if (found.size() > 1)
        fail(at, "a " + types.toString(v, &symbols) + " is ambiguous for the typed union " + qualifiedName(c) + ": its alternatives " +
                     name(found[0]) + " and " + name(found[1]) + " both have that type (set one by name: u[" + name(found[0]) + "] = value)");
      return found[0];
    }
    // The index among c's alternatives of the one a descriptor of `alternatives(U)` names, or -1.
    static int64_t descriptorAlternative(ClassDecl* c, const Ty* descriptor)
    {
      if (!descriptor || descriptor->kind != TyKind::Member)
        return -1;
      for (size_t i = 0; i < c->alternatives.size(); ++i)
        if (c->alternatives[i] == descriptor->decl)
          return int64_t(i);
      return -1;
    }
    // The alternative an index names: `p[Property::Integer]`, a constant enumerator of the tag, or
    // `p[a]` with a descriptor of alternatives(U).
    size_t alternativeIndex(ClassDecl* c, IndexExpr* i, Node* at)
    {
      if (i->args.size() != 1)
        unsupported(at, "a typed union indexed with more than one alternative");
      const Ty* indexTy = i->args[0]->ty ? types.stripRef(i->args[0]->ty) : nullptr;
      std::optional<int64_t> v = indexTy && indexTy->kind == TyKind::Member ? descriptorAlternative(c, indexTy) : evalConst(i->args[0]);
      if (!v || *v < 0 || size_t(*v) >= c->alternatives.size())
        unsupported(at, "a typed union indexed with something other than one of its alternatives");
      return size_t(*v);
    }
    // The address of the alternative p[E] names; with check, a release_assert that p holds it.
    std::string alternativeLvalue(ClassDecl* c, Value base, IndexExpr* i, Node* at, bool check)
    {
      size_t index = alternativeIndex(c, i, at);
      if (check)
      {
        std::string tag = loadTag(c, base.ty, base.text, at);
        std::string same = newTemp();
        fn += "  " + same + " = icmp eq " + llType(tagType(c), at) + " " + tag + ", " + std::to_string(index) + "\n";
        std::string okL = newLabel("alt.ok"), failL = newLabel("alt.fail");
        fn += "  br i1 " + same + ", label %" + okL + ", label %" + failL + "\n";
        startBlock(failL);
        auto [line, column] = lineColumnOf(at->token);
        emitCall("", "void @kpp_assert_failed(ptr " + cStringConstant(currentFile->path) + ", i32 " + std::to_string(line) + ", i32 " +
                         std::to_string(column) + ", ptr " + cStringConstant("the typed union holds another alternative") + ")", false);
        fn += "  unreachable\n";
        usesAssert = true;
        startBlock(okL);
      }
      return alternativeAddress(c, base.ty, base.text, index, at);
    }
    void setTag(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Node* at)
    {
      fn += "  store " + llType(tagType(c), at) + " " + std::to_string(index) + ", ptr " + tagAddress(c, t, obj, at) + "\n";
    }
    // Sets the tag and builds the alternative from init (default-constructed without one).
    void constructAlternative(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Expr* init, Node* at)
    {
      setTag(c, t, obj, index, at);
      const Ty* altTy = alternativeType(c, t, index, at);
      std::string addr = alternativeAddress(c, t, obj, index, at);
      if (init)
        initializeFrom(addr, altTy, init, at);
      else if (isClass(altTy))
        defaultConstruct(addr, altTy, at);
      else if (altTy->kind == TyKind::Array && needsDestruction(altTy, at))
        constructArray(addr, altTy, at);
      else
        storeZero(altTy, addr, at);
    }
    // Runs body(index) for the alternative the object holds, among those `wanted` selects: a
    // switch on the tag.
    template<typename Wanted, typename Body>
    void forHeldAlternative(ClassDecl* c, const Ty* t, const std::string& obj, Node* at, Wanted&& wanted, Body&& body)
    {
      std::vector<size_t> indices;
      for (size_t i = 0; i < c->alternatives.size(); ++i)
        if (wanted(i))
          indices.push_back(i);
      if (indices.empty())
        return;
      std::string tag = loadTag(c, t, obj, at);
      std::string k = llType(tagType(c), at);
      std::string endL = newLabel("alt.end");
      std::vector<std::string> labels;
      std::string cases;
      for (size_t i : indices)
      {
        labels.push_back(newLabel("alt"));
        cases += "    " + k + " " + std::to_string(i) + ", label %" + labels.back() + "\n";
      }
      fn += "  switch " + k + " " + tag + ", label %" + endL + " [\n" + cases + "  ]\n";
      blockTerminated = true;
      for (size_t n = 0; n < indices.size(); ++n)
      {
        startBlock(labels[n]);
        body(indices[n]);
        emitBranch(endL);
      }
      startBlock(endL);
    }
    void destroyAlternative(ClassDecl* c, const Ty* t, const std::string& obj, Node* at)
    {
      forHeldAlternative(c, t, obj, at,
                         [&](size_t i) { return needsDestruction(alternativeType(c, t, i, at), at); },
                         [&](size_t i) { destroy(alternativeAddress(c, t, obj, i, at), alternativeType(c, t, i, at), at); });
    }
    // Copies (or moves) the alternative src holds into dst, tag included.
    void copyAlternative(ClassDecl* c, const Ty* t, const std::string& dst, const std::string& src, bool move, Node* at)
    {
      std::string tag = loadTag(c, t, src, at);
      fn += "  store " + llType(tagType(c), at) + " " + tag + ", ptr " + tagAddress(c, t, dst, at) + "\n";
      forHeldAlternative(c, t, src, at, [&](size_t) { return true; }, [&](size_t i)
      {
        const Ty* altTy = alternativeType(c, t, i, at);
        std::string from = alternativeAddress(c, t, src, i, at), to = alternativeAddress(c, t, dst, i, at);
        if (isClass(altTy))
        {
          if (move)
            moveConstruct(to, from, altTy, at);
          else
            copyConstruct(to, from, altTy, at);
        }
        else
        {
          std::string v = newTemp();
          fn += "  " + v + " = load " + llType(altTy, at) + ", ptr " + from + "\n";
          fn += "  store " + llType(altTy, at) + " " + v + ", ptr " + to + "\n";
        }
      });
    }
    // `p[Integer] = 5`: destroys the alternative held, sets the tag, constructs the new value.
    Value assignAlternative(IndexExpr* i, Expr* rhs, Node* at)
    {
      Value base = rvalue(i->base);
      ClassDecl* c = typedUnionOf(base.ty, at);
      size_t index = alternativeIndex(c, i, at);
      destroyAlternative(c, base.ty, base.text, at);
      constructAlternative(c, base.ty, base.text, index, rhs, at);
      const Ty* altTy = alternativeType(c, base.ty, index, at);
      std::string addr = alternativeAddress(c, base.ty, base.text, index, at);
      return isClass(altTy) ? Value{addr, altTy, false} : load(addr, altTy, at);
    }
    // The tag values of a typed union's alternatives, in declaration order (the cases of a switch).
    std::vector<int64_t> alternativeTags(ClassDecl* c)
    {
      std::vector<int64_t> tags;
      if (auto* e = static_cast<EnumDecl*>(c->tagEnum))
        for (Decl* d : e->enumerators)
          if (d->kind == NodeKind::EnumeratorDecl)
            tags.push_back(enumeratorValue(static_cast<EnumeratorDecl*>(d)));
      return tags;
    }

    // Runs part(k) in a block of its own for the alternative k both typed unions at a and b hold,
    // their tags known equal; the blocks go on to done.
    template<typename F>
    void forEachHeldAlternative(ClassDecl* c, const Ty* t, const Value& tag, const std::string& done, Node* at, F&& part)
    {
      std::vector<int64_t> tags = alternativeTags(c);
      if (tags.size() != c->alternatives.size())
        unsupported(at, "a typed union whose tags are not its alternatives'");
      const Ty* underlying = enumUnderlying(c->tagEnum);
      std::string type = llType(underlying, at);
      std::vector<std::string> labels;
      std::string sw = "  switch " + type + " " + tag.text + ", label %" + done + " [";
      for (size_t k = 0; k < tags.size(); ++k)
      {
        labels.push_back(newLabel("union.alternative"));
        sw += " " + type + " " + std::to_string(tags[k]) + ", label %" + labels.back();
      }
      fn += sw + " ]\n";
      blockTerminated = true;
      for (size_t k = 0; k < labels.size(); ++k)
      {
        startBlock(labels[k]);
        part(k);
        emitBranch(done);
      }
    }

    // a == b of two typed unions at those addresses (design.md 8.5, as std::variant's): the same
    // alternative held, and equal values of it.
    Value typedUnionEqual(ClassDecl* c, const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      const Ty* underlying = enumUnderlying(c->tagEnum);
      Value ta = convert(Value{loadTag(c, t, a, at), tagType(c), false}, underlying, at);
      Value tb = convert(Value{loadTag(c, t, b, at), tagType(c), false}, underlying, at);
      std::string slot = "%equal" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i1\n";
      fn += "  store i1 false, ptr " + slot + "\n";
      std::string same = newTemp(), held = newLabel("unionequal.held"), done = newLabel("unionequal.done");
      fn += "  " + same + " = icmp eq " + llType(underlying, at) + " " + ta.text + ", " + tb.text + "\n";
      fn += "  br i1 " + same + ", label %" + held + ", label %" + done + "\n";
      blockTerminated = true;
      startBlock(held);
      forEachHeldAlternative(c, t, ta, done, at, [&](size_t k) {
        Value e = partsEqual(alternativeAddress(c, t, a, k, at), alternativeAddress(c, t, b, k, at), alternativeType(c, t, k, at), at);
        fn += "  store i1 " + e.text + ", ptr " + slot + "\n";
      });
      startBlock(done);
      return load(slot, types.boolType(), at);
    }

    // The order (i8) of two typed unions at those addresses (design.md 8.5, as std::variant's): by
    // the alternative held, in declaration order, then by the values of it.
    Value typedUnionOrder(ClassDecl* c, const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      const Ty* underlying = enumUnderlying(c->tagEnum);
      Value ta = convert(Value{loadTag(c, t, a, at), tagType(c), false}, underlying, at);
      Value tb = convert(Value{loadTag(c, t, b, at), tagType(c), false}, underlying, at);
      std::string slot = "%order" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i8\n";
      std::string type = llType(underlying, at);
      std::string less = newTemp(), byTag = newTemp(), same = newTemp();
      fn += "  " + less + " = icmp ult " + type + " " + ta.text + ", " + tb.text + "\n";
      fn += "  " + byTag + " = select i1 " + less + ", i8 -1, i8 1\n";
      fn += "  store i8 " + byTag + ", ptr " + slot + "\n";
      std::string held = newLabel("unionorder.held"), done = newLabel("unionorder.done");
      fn += "  " + same + " = icmp eq " + type + " " + ta.text + ", " + tb.text + "\n";
      fn += "  br i1 " + same + ", label %" + held + ", label %" + done + "\n";
      blockTerminated = true;
      startBlock(held);
      fn += "  store i8 0, ptr " + slot + "\n";
      forEachHeldAlternative(c, t, ta, done, at, [&](size_t k) {
        Value o = orderOfParts(alternativeAddress(c, t, a, k, at), alternativeAddress(c, t, b, k, at), alternativeType(c, t, k, at), at);
        fn += "  store i8 " + o.text + ", ptr " + slot + "\n";
      });
      startBlock(done);
      return load(slot, types.intType(8), at);
    }

    // `p == Property::Integer`: the tag against the enumerator.
    Value compareTag(BinaryExpr* b, bool negate)
    {
      bool unionLeft = typedUnionOf(types.stripRef(b->lhs->ty), b) != nullptr;
      Expr* unionExpr = unionLeft ? b->lhs : b->rhs;
      Expr* tagExpr = unionLeft ? b->rhs : b->lhs;
      Value obj = rvalue(unionExpr);
      ClassDecl* c = typedUnionOf(obj.ty, b);
      const Ty* underlying = enumUnderlying(c->tagEnum);
      Value tag = convert(Value{loadTag(c, obj.ty, obj.text, b), tagType(c), false}, underlying, b);
      Value other = convert(rvalue(tagExpr), underlying, b);
      std::string r = newTemp();
      fn += "  " + r + " = icmp " + (negate ? "ne " : "eq ") + llType(underlying, b) + " " + tag.text + ", " + other.text + "\n";
      return {r, types.boolType(), false};
    }

    // `types<A, B, C>`: the compile-time list of types (design.md 8.1).
    bool isTypesList(const NamedType* n)
    {
      return n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && sym(n->name.parts[0].symbol) == "types";
    }

    // `members(T)` (design.md 8.3), `alternatives(U)` (8.5): the type reflected on, or null when
    // the node is no such list; alternatives set when it is the latter.
    Expr* reflectionSubject(Node* list, bool* alternatives = nullptr)
    {
      if (!list || list->kind != NodeKind::CallExpr)
        return nullptr;
      auto* c = static_cast<CallExpr*>(list);
      if (!c->callee || c->callee->kind != NodeKind::NameExpr || c->args.size() != 1)
        return nullptr;
      const QualifiedName& name = static_cast<NameExpr*>(c->callee)->name;
      if (name.parts.size() != 1 || name.parts[0].hasTemplateArgs)
        return nullptr;
      std::string which = sym(name.parts[0].symbol);
      if (which != "members" && which != "alternatives")
        return nullptr;
      if (alternatives)
        *alternatives = which == "alternatives";
      return c->args[0];
    }

    // The descriptors of a class's non-static data members, bases first, in declaration order.
    void collectMembers(const Ty* cls, std::vector<const Ty*>& out, Node* at, int depth)
    {
      if (depth > 16)
        unsupported(at, "a class nested too deep for members(T)");
      ClassDecl* def = classDef(cls->decl, at);
      if (!def)
        unsupported(at, "members(T) of a class without a definition");
      TemplateArgs classArgs = global.classArgsOf(cls);
      for (const ClassDecl::Base& b : def->bases)
      {
        const Ty* bt = global.typeOfAstIn(b.type, def, classArgs);
        bt = bt ? types.unqualified(types.stripRef(bt)) : nullptr;
        if (!isClass(bt))
          unsupported(at, "members(T) with a base class of unknown type");
        collectMembers(canonicalClass(bt, at), out, at, depth + 1);
      }
      for (Decl* d : def->members)
      {
        if (d->kind != NodeKind::VariableDecl || (d->flags & (DF_Static | DF_Typedef)))
          continue;
        auto* v = static_cast<VariableDecl*>(d);
        if (v->name.empty())
          continue;
        const Ty* t = global.typeOfDeclIn(v, classArgs);
        if (!t || !t->isKnown())
          unsupported(at, "members(T) with a member of unknown type (" + sym(v->name.last().symbol) + ")");
        out.push_back(types.member(t, v, cls));
      }
    }

    // `for constexpr (typename T : types<A, B>) body` / `for constexpr (int i : {1, 2}) body`: the
    // body once per element, re-typed and emitted with the loop variable bound to the element (a
    // type, or a constant), the way a pack expansion binds each element in turn.
    void forConstexpr(ForConstexprStmt* f)
    {
      forEachConstexprElement(f, [&](size_t)
      {
        pushScope();
        statement(f->body);
        popScope();
      });
    }

    // The statements of a for constexpr body: those of its block, or the statement itself.
    static std::vector<Stmt*> bodyStatements(Stmt* body)
    {
      if (!body)
        return {};
      if (body->kind == NodeKind::BlockStmt)
        return std::vector<Stmt*>(static_cast<BlockStmt*>(body)->stmts.begin(), static_cast<BlockStmt*>(body)->stmts.end());
      return {body};
    }

    // Runs body(index) once per element of a for constexpr list, with the function re-typed for
    // the element and currentArgs binding the loop variable to it; the original typing is
    // restored afterwards.
    void forEachConstexprElement(ForConstexprStmt* f, const std::function<void(size_t)>& body)
    {
      auto* var = static_cast<TemplateParamDecl*>(f->var);
      if (!currentFunction)
        unsupported(f, "for constexpr outside a function");
      TemplateArgs none;
      TemplateArgs original = currentArgs ? *currentArgs : none;
      std::vector<const Ty*> elements;
      Node* list = f->list;
      if (list && list->kind == NodeKind::NamedType && isTypesList(static_cast<NamedType*>(list)))
      {
        if (!var->isType)
          unsupported(f, "a list of types for a for constexpr value variable");
        for (Node* arg : static_cast<NamedType*>(list)->name.parts[0].templateArgs)
        {
          if (!arg || uint8_t(arg->kind) > uint8_t(NodeKind::PackType))
            unsupported(f, "an element of types<...> that is not a type");
          const Ty* t = global.typeOfAstIn(static_cast<Type*>(arg), currentFunction, original);
          if (!t || !t->isKnown())
            unsupported(f, "an element of types<...> whose type is unknown");
          elements.push_back(t);
        }
      }
      else if (list && list->kind == NodeKind::BraceInitExpr)
      {
        if (var->isType)
          unsupported(f, "a list of values for a for constexpr type variable");
        const Ty* elementTy = var->valueType ? global.typeOfAstIn(var->valueType, currentFunction, original) : nullptr;
        if (!elementTy || !elementTy->isKnown() || elementTy->kind == TyKind::Auto)
          unsupported(f, "a for constexpr value variable without a written type");
        for (Expr* e : static_cast<BraceInitExpr*>(list)->args)
        {
          std::optional<int64_t> v = evalConst(e);
          if (!v)
            unsupported(e, "a for constexpr element that is not a constant");
          elements.push_back(types.valueType(elementTy, *v));
        }
      }
      else if (bool alternatives = false; Expr* subject = reflectionSubject(list, &alternatives))
      {
        // `members(T)`: T's non-static data members, bases first, in declaration order;
        // `alternatives(U)`: a typed union's alternatives, in declaration order.
        if (var->isType)
          unsupported(f, "members(T) or alternatives(U) for a for constexpr type variable");
        const Ty* cls = nullptr;
        if (subject->kind == NodeKind::NameExpr)
        {
          NamedType tmp{};
          tmp.kind = NodeKind::NamedType;
          tmp.name = static_cast<NameExpr*>(subject)->name;
          cls = global.typeOfAstIn(&tmp, currentFunction, original);
        }
        cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
        if (!isClass(cls))
          unsupported(f, alternatives ? "alternatives(U) where U is not a class" : "members(T) where T is not a class");
        cls = canonicalClass(cls, f);
        if (alternatives)
        {
          ClassDecl* u = typedUnionOf(cls, f);
          if (!u)
            unsupported(f, "alternatives(U) where U is not a typed union");
          for (size_t i = 0; i < u->alternatives.size(); ++i)
            elements.push_back(types.member(alternativeType(u, cls, i, f), u->alternatives[i], cls));
        }
        else
          collectMembers(cls, elements, f, 0);
      }
      else
        unsupported(f, "a for constexpr list that is neither types<...>, members(T), alternatives(U) nor a brace list of constants");
      const TemplateArgs* savedArgs = currentArgs;
      for (size_t index = 0; index < elements.size(); ++index)
      {
        TemplateArgs elementArgs = original;
        bool bound = false;
        for (auto& [d, t] : elementArgs.pairs)
          if (d == var)
            t = elements[index], bound = true;
        if (!bound)
          elementArgs.pairs.push_back({var, elements[index]});
        global.retypeFor(currentFunction, elementArgs, currentClassTy);
        currentArgs = &elementArgs;
        body(index);
        currentArgs = savedArgs;
      }
      if (!elements.empty())
        global.retypeFor(currentFunction, original, currentClassTy);
    }

    void rangeFor(RangeForStmt* f)
    {
      pushScope();
      const Ty* rt = types.stripRef(f->range->ty);
      if (!rt || !rt->isKnown())
        unsupported(f, "a range of unknown type");
      std::string beginV, endV;
      const Ty* elementTy;
      if (rt->kind == TyKind::Array)
      {
        std::string base;
        if (f->range->kind == NodeKind::BraceInitExpr)
        {
          // `for (X* kids : {&a, &b})`: the elements in a temporary array.
          auto* b = static_cast<BraceInitExpr*>(f->range);
          base = newObjectSlot(rt, f);
          for (size_t i = 0; i < b->args.size(); ++i)
          {
            std::string slot = newTemp();
            fn += "  " + slot + " = getelementptr inbounds " + llType(rt, f) + ", ptr " + base + ", i64 0, i64 " + std::to_string(i) + "\n";
            initializeFrom(slot, rt->inner, b->args[i], f);
          }
        }
        else
          base = lvalue(f->range);
        beginV = newTemp();
        fn += "  " + beginV + " = getelementptr inbounds " + llType(rt, f) + ", ptr " + base + ", i64 0, i64 0\n";
        endV = newTemp();
        fn += "  " + endV + " = getelementptr inbounds " + llType(rt, f) + ", ptr " + base + ", i64 0, i64 " + std::to_string(rt->count) + "\n";
        elementTy = rt->inner;
      }
      else if (isClass(rt))
      {
        Value range = rvalue(f->range);
        if (range.isTemporary)
        {
          for (size_t i = statementTemps.size(); i-- > 0;)
            if (statementTemps[i].addr == range.text)
              statementTemps.erase(statementTemps.begin() + i);
          scopes.back().push_back({range.text, range.ty});
        }
        const Ty* beginIn = nullptr;
        const Ty* endIn = nullptr;
        FunctionDecl* begin = findMember(range.ty, "begin", f, &beginIn);
        FunctionDecl* end = findMember(range.ty, "end", f, &endIn);
        Value b, e;
        if (begin && end)
        {
          // On the base that declares them, where it is (`for (T* t : *this)` in TriggerList, a
          // Vector<T*> after a polymorphic base: not at the object's address).
          auto on = [&](const Ty* in)
          {
            std::string object = range.text;
            if (canonicalClass(range.ty, f) != in)
              baseAddress(range.ty, in, range.text, object, f);
            return object;
          };
          b = callFunction(begin, global.classArgsOf(beginIn), beginIn, on(beginIn), "", {}, f);
          e = callFunction(end, global.classArgsOf(endIn), endIn, on(endIn), "", {}, f);
        }
        else
        {
          // No such members: begin(__range) and end(__range), the free functions found for the
          // range's class, called with the range evaluated once (C++'s rule).
          const Ty* rangeRef = types.lref(types.unqualified(range.ty));
          FunctionDecl* freeBegin = global.findFreeFunction("begin", {rangeRef}, currentFunction);
          FunctionDecl* freeEnd = global.findFreeFunction("end", {rangeRef}, currentFunction);
          if (!freeBegin || !freeEnd)
            unsupported(f, "a range without begin()/end()");
          VariableDecl& var = rangeVariables.emplace_back();
          var.kind = NodeKind::VariableDecl;
          var.token = f->range->token;
          var.file = currentFile;
          std::string slot = "%v" + std::to_string(temp++);
          allocas += "  " + slot + " = alloca ptr\n";
          fn += "  store ptr " + range.text + ", ptr " + slot + "\n";
          locals[&var] = {slot, rangeRef, true};
          NameExpr& name = rangeNames.emplace_back();
          name.kind = NodeKind::NameExpr;
          name.token = f->range->token;
          name.resolved = &var;
          name.ty = types.unqualified(range.ty);
          Expr* arg = &name;
          auto call = [&](FunctionDecl* callee) -> Value
          {
            TemplateArgs calleeArgs;
            if (!callee->templateParams.empty())
            {
              global.deduceArgs(callee, {rangeRef}, calleeArgs);
              if (calleeArgs.pairs.size() < callee->templateParams.size())
                unsupported(f, "a call to " + qualifiedName(callee) + " whose template arguments could not be deduced");
            }
            return callFunction(callee, calleeArgs, nullptr, "", "", Span<Expr*>{&arg, 1}, f);
          };
          b = call(freeBegin);
          e = call(freeEnd);
        }
        // `Iterator& begin()`, `const Iterator& end()`: the loop works on copies, C++'s `auto __begin
        // = range.begin()` (HeuristicEntitySearch hands out its own iterator member).
        auto byValue = [&](Value v) -> Value
        {
          if (!v.ty || !v.ty->isReference())
            return v;
          const Ty* t = types.unqualified(types.stripRef(v.ty));
          if (t->kind == TyKind::Pointer)
            return load(v.text, t, f);
          if (!isClass(t))
            return v;
          std::string copy = newObjectSlot(t, f);
          copyConstruct(copy, v.text, t, f);
          return {copy, t, true};
        };
        b = byValue(b);
        e = byValue(e);
        if (b.ty && isClass(b.ty))
        {
          iteratorLoop(f, b, e);
          popScope();
          return;
        }
        if (!b.ty || b.ty->kind != TyKind::Pointer)
          unsupported(f, "a range whose iterators are not pointers");
        beginV = b.text;
        endV = e.text;
        elementTy = b.ty->inner;
      }
      else
        unsupported(f, "this kind of range");
      std::string itSlot = "%v" + std::to_string(temp++);
      allocas += "  " + itSlot + " = alloca ptr\n";
      fn += "  store ptr " + beginV + ", ptr " + itSlot + "\n";
      destroyTemps();
      std::string cL = newLabel("range.cond"), bL = newLabel("range.body"), sL = newLabel("range.step"), eL = newLabel("range.end");
      emitBranch(cL);
      startBlock(cL);
      std::string it = newTemp();
      fn += "  " + it + " = load ptr, ptr " + itSlot + "\n";
      std::string done = newTemp();
      fn += "  " + done + " = icmp eq ptr " + it + ", " + endV + "\n";
      fn += "  br i1 " + done + ", label %" + eL + ", label %" + bL + "\n";
      startBlock(bL);
      pushScope();
      // The loop variable: a reference binds to the element, a value copies it.
      auto* v = static_cast<VariableDecl*>(f->var);
      const Ty* vt = declType(v);
      if (!vt || !vt->isKnown() || types.isDependent(vt))
        unsupported(v, "a loop variable of unknown type");
      std::string element = newTemp();
      fn += "  " + element + " = load ptr, ptr " + itSlot + "\n";
      std::string slot = "%v" + std::to_string(temp++);
      if (vt->isReference())
      {
        std::string target = elementAs(element, elementTy, vt, v);
        if (elementMadeInto(elementTy, vt))
        {
          // `for (const String& name : {"ref"})`: a String made from the element, the reference's
          // (it lives for the iteration).
          const Ty* cls = types.unqualified(types.stripRef(vt));
          target = newObjectSlot(cls, v);
          makeFromElement(target, cls, element, elementTy, v);
          if (needsDestruction(cls, v))
            scopes.back().push_back({target, cls});
        }
        allocas += "  " + slot + " = alloca ptr\n";
        fn += "  store ptr " + target + ", ptr " + slot + "\n";
        locals[v] = {slot, vt, true};
      }
      else
      {
        allocas += "  " + slot + " = alloca " + llType(vt, v) + "\n";
        if (elementMadeInto(elementTy, vt))
          makeFromElement(slot, vt, element, elementTy, v);
        else if (isClass(vt))
          copyConstruct(slot, elementAs(element, elementTy, vt, v), vt, v);
        else
          store(load(element, elementTy, v), slot, vt, v);
        locals[v] = {slot, vt, false};
        if (needsDestruction(vt, v))
          scopes.back().push_back({slot, vt});
      }
      declareTupleBindings(v); // `for (auto& [a, b] : tuples)`: each element's get<i>
      loops.push_back({eL, sL, scopes.size()});
      statement(f->body);
      loops.pop_back();
      popScope();
      emitBranch(sL);
      startBlock(sL);
      std::string cur = newTemp();
      fn += "  " + cur + " = load ptr, ptr " + itSlot + "\n";
      std::string next = newTemp();
      fn += "  " + next + " = getelementptr inbounds " + llType(elementTy, f) + ", ptr " + cur + ", i64 1\n";
      fn += "  store ptr " + next + ", ptr " + itSlot + "\n";
      emitBranch(cL);
      startBlock(eL);
      popScope();
    }

    Value condition(Node* cond, Node* at)
    {
      if (!cond)
        unsupported(at, "a missing condition");
      if (cond->kind == NodeKind::DeclStmt)
      {
        auto* d = static_cast<DeclStmt*>(cond);
        if (d->decls.size() != 1 || d->decls[0]->kind != NodeKind::VariableDecl)
          unsupported(at, "this condition declaration");
        auto* v = static_cast<VariableDecl*>(d->decls[0]);
        declareLocal(v);
        Local& l = locals[v];
        if (l.isRef || l.ty->isReference())
        {
          // `if (const IDConnector<ItemID>& spoilResultID = itemID->spoilResult)`: the object the
          // reference names is the condition (its operator bool), not the reference's address.
          std::string object = newTemp();
          fn += "  " + object + " = load ptr, ptr " + l.addr + "\n";
          const Ty* objectTy = types.stripRef(l.ty);
          return convert(isAggregate(objectTy) ? Value{object, objectTy} : load(object, objectTy, at), types.boolType(), at);
        }
        return convert(load(l.addr, l.ty, at), types.boolType(), at);
      }
      return convert(rvalue(static_cast<Expr*>(cond)), types.boolType(), at);
    }

    void switchStatement(SwitchStmt* sw)
    {
      pushScope();
      if (sw->init)
        statement(sw->init);
      if (!sw->cond)
        unsupported(sw, "a switch without a condition");
      Value cond;
      const Ty* switchedTy = nullptr;
      if (sw->cond->kind == NodeKind::DeclStmt)
      {
        // `switch (const int type = table.get<uint32_t>("type"))`: the variable, in the switch's
        // scope, and its value switched on.
        auto* d = static_cast<DeclStmt*>(sw->cond);
        if (d->decls.size() != 1 || d->decls[0]->kind != NodeKind::VariableDecl)
          unsupported(sw, "this switch declaration");
        auto* v = static_cast<VariableDecl*>(d->decls[0]);
        declareLocal(v);
        Local& l = locals[v];
        cond = load(l.addr, l.ty, sw);
        switchedTy = types.stripRef(l.ty);
      }
      else
      {
        cond = rvalue(static_cast<Expr*>(sw->cond));
        switchedTy = types.stripRef(static_cast<Expr*>(sw->cond)->ty); // the written type: a Kpp enum's switch names every value
      }
      if (ClassDecl* c = typedUnionOf(cond.ty, sw))
      {
        // `switch (p)` on a typed union: its tag.
        cond = Value{loadTag(c, cond.ty, cond.text, sw), tagType(c), false};
        switchedTy = cond.ty;
      }
      else if (isClass(cond.ty))
      {
        // `switch (direction)` on an object: its conversion to an enum or integer (Direction's `operator Enum()`).
        const Ty* owner = nullptr;
        FunctionDecl* op = integralConversionOperator(cond.ty, sw, &owner);
        if (!op)
          unsupported(sw, "a switch on a class without a conversion to an enum or an integer");
        cond = callConversion(op, cond.ty, owner, cond.text, sw);
        switchedTy = cond.ty;
      }
      const Ty* ct = cond.ty->kind == TyKind::Enum ? enumUnderlying(cond.ty->decl) : cond.ty;
      if (ct->kind != TyKind::Int && ct->kind != TyKind::Char && ct->kind != TyKind::Bool)
        unsupported(sw, "a switch on a non-integer");
      cond = convert(cond, ct, sw);
      destroyTemps();
      if (!sw->body || sw->body->kind != NodeKind::BlockStmt)
        unsupported(sw, "a switch whose body is not a block");
      auto* body = static_cast<BlockStmt*>(sw->body);
      std::string endL = newLabel("switch.end");
      std::string defaultL;
      std::map<std::pair<const Stmt*, size_t>, std::string> caseLabels; // (label statement, for constexpr element) -> block
      std::set<int64_t> covered;
      std::string cases;
      constexpr size_t topLevel = SIZE_MAX;
      auto addLabel = [&](Stmt* st, size_t element)
      {
        if (st->kind == NodeKind::CaseStmt)
        {
          auto* c = static_cast<CaseStmt*>(st);
          if (c->rangeEnd)
            unsupported(st, "case ranges");
          auto v = evalConst(c->value);
          if (!v)
            unsupported(st, "a case label that is not a constant");
          std::string l = newLabel("case");
          caseLabels[{st, element}] = l;
          covered.insert(*v);
          cases += "    " + llType(ct, sw) + " " + std::to_string(*v) + ", label %" + l + "\n";
        }
        else if (st->kind == NodeKind::DefaultStmt)
        {
          defaultL = newLabel("default");
          caseLabels[{st, element}] = defaultL;
        }
      };
      for (Stmt* st : body->stmts)
      {
        if (st->kind == NodeKind::ForConstexprStmt)
        {
          // `for constexpr (typename T : kinds) case T::kind: ...`: one case per element.
          auto* fc = static_cast<ForConstexprStmt*>(st);
          forEachConstexprElement(fc, [&](size_t element)
          {
            for (Stmt* inner : bodyStatements(fc->body))
              addLabel(inner, element);
          });
          continue;
        }
        addLabel(st, topLevel);
      }
      std::string invalidL;
      const Ty* condTy = switchedTy; // before the conversion to the underlying type
      if (condTy && condTy->kind == TyKind::Enum && defaultL.empty())
      {
        Decl* def = global.definitionOf(condTy->decl);
        auto* e = def && def->kind == NodeKind::EnumDecl ? static_cast<EnumDecl*>(def) : nullptr;
        if (e && e->isKpp)
        {
          // A Kpp enum: every value needs a case, and a value outside them aborts.
          std::string missing;
          for (Decl* d : e->enumerators)
            if (d->kind == NodeKind::EnumeratorDecl && !covered.count(enumeratorValue(static_cast<EnumeratorDecl*>(d))))
              missing += (missing.empty() ? "" : ", ") + sym(static_cast<EnumeratorDecl*>(d)->name);
          if (!missing.empty())
            error(sw, "switch on " + qualifiedName(e) + " does not handle " + missing);
          invalidL = newLabel("switch.invalid");
        }
      }
      fn += "  switch " + llType(ct, sw) + " " + cond.text + ", label %" + (invalidL.empty() ? (defaultL.empty() ? endL : defaultL) : invalidL) + " [\n" + cases + "  ]\n";
      blockTerminated = true;
      if (!invalidL.empty())
      {
        startBlock(invalidL);
        auto [line, column] = lineColumnOf(sw->token);
        emitCall("", "void @kpp_assert_failed(ptr " + cStringConstant(currentFile->path) + ", i32 " + std::to_string(line) + ", i32 " +
                         std::to_string(column) + ", ptr " + cStringConstant("invalid value in switch on " + qualifiedName(condTy->decl)) + ")", false);
        fn += "  unreachable\n";
        usesAssert = true;
        blockTerminated = true;
      }
      loops.push_back({endL, "", scopes.size()});
      pushScope();
      auto emitLabelled = [&](Stmt* st, size_t element)
      {
        if (st->kind == NodeKind::CaseStmt || st->kind == NodeKind::DefaultStmt)
        {
          const std::string& l = caseLabels[{st, element}];
          emitBranch(l);
          startBlock(l);
          return;
        }
        if (st->kind == NodeKind::FallthroughStmt)
          return;
        statement(st);
      };
      for (Stmt* st : body->stmts)
      {
        if (st->kind == NodeKind::ForConstexprStmt)
        {
          auto* fc = static_cast<ForConstexprStmt*>(st);
          forEachConstexprElement(fc, [&](size_t element)
          {
            pushScope();
            for (Stmt* inner : bodyStatements(fc->body))
              emitLabelled(inner, element);
            popScope();
          });
          continue;
        }
        emitLabelled(st, topLevel);
      }
      popScope();
      loops.pop_back();
      emitBranch(endL);
      startBlock(endL);
      popScope();
    }

    // --- function definitions ---

    // Whether emitting f re-types its nodes in place, once per element of a pack expansion (f or
    // its class has a parameter pack) or of a `for constexpr` (design.md 8.2).
    static bool retypedPerElement(FunctionDecl* f)
    {
      for (Decl* p = f; p; p = p->parent)
        for (Decl* tp : p->templateParams)
          if (tp->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(tp)->isPack)
            return true;
      return containsForConstexpr(f->body);
    }
    static bool containsForConstexpr(Stmt* s)
    {
      if (!s)
        return false;
      switch (s->kind)
      {
        case NodeKind::ForConstexprStmt: return true;
        case NodeKind::BlockStmt:
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            if (containsForConstexpr(inner))
              return true;
          return false;
        case NodeKind::IfStmt: return containsForConstexpr(static_cast<IfStmt*>(s)->then) || containsForConstexpr(static_cast<IfStmt*>(s)->else_);
        case NodeKind::ForStmt: return containsForConstexpr(static_cast<ForStmt*>(s)->body);
        case NodeKind::RangeForStmt: return containsForConstexpr(static_cast<RangeForStmt*>(s)->body);
        case NodeKind::WhileStmt: return containsForConstexpr(static_cast<WhileStmt*>(s)->body);
        case NodeKind::DoStmt: return containsForConstexpr(static_cast<DoStmt*>(s)->body);
        case NodeKind::SwitchStmt: return containsForConstexpr(static_cast<SwitchStmt*>(s)->body);
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(s);
          if (containsForConstexpr(t->body))
            return true;
          for (const TryStmt::Handler& h : t->handlers)
            if (containsForConstexpr(h.body))
              return true;
          return false;
        }
        default: return false;
      }
    }

    void emitFunction(FunctionDecl* original, const TemplateArgs& args, const Ty* classTy, const std::string& nameOverride = "")
    {
      // An instantiation is emitted from its own copy of the template's nodes, typed for args once
      // (GlobalScope::instanceBody); its IR name is the template's with the arguments. One that is
      // re-typed per pack element while it is emitted gets a copy of its own for that, so that
      // nobody reading the instantiation's copy meanwhile (a caller's default arguments, on another
      // thread) sees the element's types.
      FunctionDecl* f = args.empty() ? original : global.instanceBody(original, args, classTy);
      FunctionDecl* checked = f; // the body the checker typed, whose static_asserts count now (GlobalScope::functionEmitted)
      if (retypedPerElement(f))
        f = global.privateInstance(original, args, classTy);
      if (!conditionHolds(f))
        return; // conditional(false): the function does not exist in this build
      global.functionEmitted(checked);
      if (f != checked)
        global.functionEmitted(f);
      currentFile = f->file;
      currentArgs = args.empty() ? nullptr : &args;
      currentClassTy = classTy;
      if (!currentClassTy)
        if (Decl* cls = global.owningClass(f))
          currentClassTy = cls->kind == NodeKind::EnumDecl ? types.enumType(cls) : canonicalClass(types.classType(cls, {}), f);
      // The type callers see: the template's own declaration under args. An `auto` instantiation's
      // return type is the one its body (the copy emitted here) deduced; asking for the copy's type
      // would type a copy of the copy, whose lambdas are other closure types than the body's.
      const Ty* ft = declType(original);
      if (!ft || ft->kind != TyKind::Function)
      {
        error(f, "function type unknown, skipped");
        currentArgs = nullptr;
        return;
      }
      resetFunctionState();
      currentFunction = f;
      currentReturn = ft->inner;
      // If the return type is unknown, pretend it's ptr so the function body can be emitted;
      // return statements will try to convert their value to a pointer (and throw if they can't).
      if (!currentReturn || !currentReturn->isKnown())
        currentReturn = types.pointer(types.voidType());
      sretAddr.clear();
      currentFunctionName = nameOverride.empty() ? functionName(original, args, classTy) : nameOverride;
      struct OwnerScope
      {
        std::string& owner;
        bool outermost;
        ~OwnerScope()
        {
          if (outermost)
            owner.clear();
        }
      } ownerScope{owner, owner.empty()};
      if (ownerScope.outermost)
        owner = currentFunctionName;
      try
      {
        Signature s = signatureOf(original);
        std::string params;
        std::vector<std::string> paramNames;
        for (size_t i = 0; i < s.paramTypes.size(); ++i)
        {
          std::string pn = "%p" + std::to_string(i);
          paramNames.push_back(pn);
          params += (i ? ", " : "") + s.paramTypes[i] + " " + pn;
        }
        if (s.variadic)
          params += params.empty() ? "..." : ", ...";
        std::string name = currentFunctionName;
        declaredNames.claim(name);
        std::string header = "define " + s.returnType + " " + name + "(" + params + ") " + attributeText(f) + "{\nentry:\n";
        pushScope();
        size_t slot = 0;
        if (s.hasThis)
        {
          std::string thisSlot = "%v" + std::to_string(temp++);
          allocas += "  " + thisSlot + " = alloca ptr\n";
          fn += "  store ptr " + paramNames[slot] + ", ptr " + thisSlot + "\n";
          locals[nullptr] = {thisSlot, nullptr, false};
          ++slot;
        }
        if (s.hasSret)
          sretAddr = paramNames[slot++];
        packLocals.clear();
        for (size_t i = 0; i < f->params.size(); ++i)
        {
          auto* pd = static_cast<ParamDecl*>(f->params[i]);
          size_t count = 1;
          if (pd->isPack)
          {
            const Ty* packTy = declType(pd);
            if (!packTy || packTy->kind != TyKind::Pack)
              unsupported(f, "a parameter pack that was not deduced");
            count = packTy->args.size();
            packLocals[pd] = {};
          }
          for (size_t k = 0; k < count; ++k, ++slot)
          {
            if (slot >= s.paramTys.size())
              unsupported(f, "parameter " + std::to_string(i + 1) + " (" + types.toString(declType(pd), &symbols) +
                                 ") beyond the parameters of the function's type " + types.toString(ft, &symbols));
            const Ty* pt = s.paramTys[slot];
            Local local;
            if (isAggregate(pt))
            {
              local = {paramNames[slot], pt, false};
              if (needsDestruction(pt, f))
                scopes.back().push_back({paramNames[slot], pt});
            }
            else
            {
              bool isRef = pt->isReference();
              std::string ps = "%v" + std::to_string(temp++);
              allocas += "  " + ps + " = alloca " + (isRef ? std::string("ptr") : llType(pt, f)) + "\n";
              fn += "  store " + (isRef ? std::string("ptr") : llType(pt, f)) + " " + paramNames[slot] + ", ptr " + ps + "\n";
              local = {ps, pt, isRef};
            }
            if (pd->isPack)
              packLocals[pd].push_back(local);
            else
              locals[pd] = local;
          }
        }
        if ((f->flags & DF_Default) && !f->body)
        {
          // `= default`: the memberwise body; the source object, if any, is the parameter after this.
          size_t sourceIndex = s.hasSret ? 2 : 1;
          std::string source = paramNames.size() > sourceIndex ? paramNames[sourceIndex] : std::string();
          Value r = defaultedOperation(f, types.unqualified(types.stripRef(currentClassTy)), thisPointer(f), source, f);
          destroyTemps();
          popScope();
          if (isClass(currentReturn))
          {
            // A class result (a defaulted operator<=>'s strong_ordering): into the result slot.
            if (!sretAddr.empty() && r.ty && isClass(r.ty))
              fn += "  store i8 " + orderOf(r, f).text + ", ptr " + gep(structFor(currentReturn, f).name, sretAddr, 0) + "\n";
            fn += "  ret void\n";
          }
          else if (currentReturn->kind == TyKind::Void)
            fn += "  ret void\n";
          else if (currentReturn->isReference())
            fn += "  ret ptr " + r.text + "\n";
          else
            fn += "  ret " + llType(currentReturn, f) + " " + r.text + "\n";
          addDefinition(name, functionText(header));
          countFunction();
          currentArgs = nullptr;
          currentClassTy = nullptr;
          return;
        }
        if (f->flags & DF_Constructor)
        {
          initializeMembers(thisPointer(f), currentClassTy, f->ctorInits, f, true);
          destroyTemps();
        }
        bool destructor = (f->flags & DF_Destructor) && isClass(currentClassTy);
        if (destructor)
        {
          storeVptrs(thisPointer(f), structFor(currentClassTy, f)); // virtual calls while it is destroyed reach this class's functions, as in C++
          // The members and bases are destroyed after the body however it ends: falling off its end
          // or at a `return` (CargoBayResizeHelper's `if (!this->surface) return;` left its targeter
          // member in the hub's list, and destroying the hub faulted).
          returnLabel = newLabel("dtor.members");
        }
        statement(f->body);
        if (destructor)
        {
          popScope();
          emitBranch(returnLabel);
          startBlock(returnLabel);
          returnLabel.clear();
          destroyMembers(thisPointer(f), currentClassTy, f);
          fn += "  ret void\n";
        }
        else if (!blockTerminated)
        {
          popScope();
          if (currentReturn->kind == TyKind::Void || returnsInMemory(currentReturn))
            fn += "  ret void\n";
          else if (name == "@kpp.main" || name == "@kpp.wmain")
            fn += "  ret i32 0\n";
          else // falling off the end (after a throw, usually): a zero rather than `unreachable`, see emitThrow
            fn += "  ret " + (currentReturn->isReference() ? std::string("ptr null") : llType(currentReturn, f) + " zeroinitializer") + "\n";
        }
        addDefinition(name, functionText(header));
        countFunction();
      }
      catch (Unsupported&)
      {
        failedFunctions.push_back(currentFunctionName);
        failedAt[currentFunctionName] = qualifiedName(original) + " at " + locationOf(original, original->file); // what --stub-missing's stub says
      }
      currentArgs = nullptr;
      currentClassTy = nullptr;
    }

    // A static_assert the checker left to the layouts (`static_assert(sizeof(Pair) == 16)`): its
    // condition under the instance's arguments, with sizes and alignments as laid out. A false one is
    // the checker's error ("static assertion failed"); a layout that fails reports itself.
    void checkLayoutAssert(const GlobalScope::LayoutStaticAssert& a)
    {
      StaticAssertDecl* s = a.decl;
      if (!s || !s->cond)
        return;
      ArgsScope scope(*this, &a.args);
      ConstContextScope context(*this, s);
      currentFile = s->file;
      currentFunctionName.clear();
      currentRequestedBy.clear();
      std::optional<bool> v;
      try
      {
        v = evalCondition(s->cond);
      }
      catch (Unsupported&)
      {
        return;
      }
      if (v && *v)
        return;
      std::string message = v ? "static assertion failed" : "static assertion whose condition the compiler cannot evaluate yet";
      if (s->message && s->message->kind == NodeKind::LiteralExpr && s->file && s->file->tokens && !s->file->tokens->empty())
      {
        const Token& t = (*s->file->tokens)[std::min<size_t>(s->message->token, s->file->tokens->size() - 1)];
        message += ": " + std::string(s->file->text.substr(t.offset, t.length));
      }
      if (v && !a.instance.empty())
        message += " (in " + a.instance + ")";
      error(s, "error: " + message);
    }

    // Out-of-line member definitions (`void Text::split(...) {}`, `Flow::~Flow() = default;`, those
    // of class templates too) take `static`, `virtual` and `= default` from their declarations, and
    // the declarations `= default` from them, before any function is emitted: emitters on several
    // threads read these flags.
    void settleDefinitionFlags(Span<Decl*> decls)
    {
      for (Decl* d : decls)
      {
        if (d->kind == NodeKind::NamespaceDecl)
          settleDefinitionFlags(static_cast<NamespaceDecl*>(d)->decls);
        if (d->kind == NodeKind::ClassDecl)
        {
          // `Flow(Flow&&) noexcept;` in the class and `Flow::Flow(Flow&&) noexcept = default;`
          // outside: the declaration is defaulted too. Found from the declaration, as useFunction
          // finds the definition (declarationOf finds no constructor or destructor), so that
          // useFunction never has to write the flag while other threads read it.
          for (Decl* m : static_cast<ClassDecl*>(d)->members)
            if (m->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(m)->body && !(m->flags & DF_Default))
              if (Decl* def = global.definitionOf(m);
                  def && def != m && def->kind == NodeKind::FunctionDecl && (def->flags & DF_Default) && !static_cast<FunctionDecl*>(def)->body)
                m->flags |= DF_Default;
          settleDefinitionFlags(static_cast<ClassDecl*>(d)->members); // nested classes
          continue;
        }
        if (d->kind != NodeKind::FunctionDecl || static_cast<FunctionDecl*>(d)->name.parts.size() <= 1)
          continue;
        auto* def = static_cast<FunctionDecl*>(d);
        FunctionDecl* declaration = global.declarationOf(def);
        if (declaration && (def->flags & DF_Default) && !def->body && !(declaration->flags & DF_Default))
          declaration->flags |= DF_Default;
        // `template<> void LuaDefines::setKey<const String&>(lua_State*, ...) {...}`: an explicit
        // specialisation of a member function template is static when the template is (`static` is
        // written in the class only, and the declaration lookup matches no template). Without it the
        // definition took a `this` its callers never pass, and read every argument one off.
        if (!declaration && def->isTemplate && def->templateParams.empty() && !(def->flags & DF_Static))
          if (Decl* owner = global.owningClass(def); owner && owner->kind == NodeKind::ClassDecl)
            if (ClassDecl* cls = classDef(owner, def))
            {
              const NamePart& name = def->name.last();
              bool anyTemplate = false, allStatic = true;
              for (Decl* m : cls->members)
                if (m->kind == NodeKind::FunctionDecl && !m->templateParams.empty() && !static_cast<FunctionDecl*>(m)->name.empty())
                  if (const NamePart& mn = static_cast<FunctionDecl*>(m)->name.last(); mn.symbol == name.symbol && mn.special == name.special &&
                                                                                       mn.opToken == name.opToken)
                  {
                    anyTemplate = true;
                    allStatic = allStatic && (m->flags & DF_Static);
                  }
              if (anyTemplate && allStatic)
                def->flags |= DF_Static;
            }
      }
    }

    // The program's globals that C++ builds at startup whether anything reads them or not: a static
    // member whose initializer registers something (`bool FontBank::loaderRegistered =
    // PrototypeLoader::registerPlainLoader(...)`, read by nobody: the game's prototype loaders were
    // never registered). Variables of namespaces and static data members of classes that are no
    // templates, with an initializer that is no literal, or of a class type without one; for an
    // out-of-line definition, the declaration in the class (the one uses name).
    void collectStartupGlobals(Span<Decl*> decls, std::vector<VariableDecl*>& out)
    {
      for (Decl* d : decls)
      {
        if (d->kind == NodeKind::NamespaceDecl)
          collectStartupGlobals(static_cast<NamespaceDecl*>(d)->decls, out);
        else if (d->kind == NodeKind::ClassDecl)
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (!c->templateParams.empty() || !c->isDefinition || c->isClosure || global.isTemplateContext(c))
            continue;
          for (Decl* m : c->members)
          {
            // `static inline T x = ...;` in the class, or `static bool registered;` defined outside
            // (`bool FontBank::loaderRegistered = ...;`, whose initializer considerStartupGlobal reads).
            if (m->kind == NodeKind::VariableDecl && (m->flags & DF_Static))
              considerStartupGlobal(static_cast<VariableDecl*>(m), out);
            else if (m->kind == NodeKind::ClassDecl)
              collectStartupGlobals(Span<Decl*>{&m, 1}, out); // a nested class
          }
        }
        else if (d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->name.parts.size() == 1)
          considerStartupGlobal(static_cast<VariableDecl*>(d), out); // (an out-of-line static member: through its class)
      }
    }

    void considerStartupGlobal(VariableDecl* v, std::vector<VariableDecl*>& out)
    {
      VariableDecl* def = variableDefinition(v);
      if ((v->flags & (DF_Typedef | DF_Constexpr | DF_ThreadLocal)) || (def->flags & (DF_Constexpr | DF_ThreadLocal)) || v->isTemplate ||
          !v->structuredBindings.empty() || (v->file && v->file->isImported) || global.isTemplateContext(v))
        return;
      if ((v->flags & DF_Extern) && !def->init && def == v)
        return; // a C library's variable
      if (def->init)
      {
        if (def->init->kind == NodeKind::LiteralExpr)
          return; // a constant: built by nobody
      }
      else
      {
        const Ty* t = global.typeOfDecl(v);
        const Ty* element = t;
        while (element && element->kind == TyKind::Array)
          element = element->inner;
        if (!element || !isClass(element))
          return; // zero
      }
      out.push_back(v);
    }

    // The static data members of class instance cls that an explicit instantiation builds at
    // startup (`template struct Registry<int>;`, as C++ instantiates every member): those with an
    // initializer that is no literal, or of a class type that needs constructing.
    void collectInstanceStartupGlobals(const Ty* cls, std::vector<std::pair<VariableDecl*, const Ty*>>& out)
    {
      Decl* d = global.definitionOf(cls->decl);
      if (!d || d->kind != NodeKind::ClassDecl)
        return;
      for (Decl* m : static_cast<ClassDecl*>(d)->members)
      {
        if (m->kind != NodeKind::VariableDecl || !(m->flags & DF_Static))
          continue;
        auto* v = static_cast<VariableDecl*>(m);
        VariableDecl* def = variableDefinition(v);
        if ((v->flags & (DF_Typedef | DF_Constexpr | DF_ThreadLocal)) || (def->flags & (DF_Constexpr | DF_ThreadLocal)) || v->isTemplate)
          continue;
        if (def->init ? def->init->kind == NodeKind::LiteralExpr : !needsConstruction(global.typeOfDeclIn(v, global.classArgsOf(cls)), v))
          continue;
        out.push_back({v, cls});
      }
    }

    // A global collectStartupGlobals found (or a static data member of the class instance classTy
    // collectInstanceStartupGlobals did): defined, and its startup function registered, as if a
    // function had read it. Its errors are its own (globalFor reports them).
    std::optional<Shared::Startup> startupAt; // where the startup function of the global being defined runs, when not where it is defined
    void emitStartupGlobal(VariableDecl* v, const Ty* classTy = nullptr, std::optional<Shared::Startup> at = std::nullopt)
    {
      struct AtRestore { std::optional<Shared::Startup>& a; ~AtRestore() { a.reset(); } } atRestore{startupAt};
      startupAt = at;
      auto it = fileModule.find(v->file);
      currentModule = it == fileModule.end() ? 0 : it->second;
      currentFunctionName = "@\"kpp.init." + (classTy ? irString(classTy) + "::" + sym(v->name.last().symbol) : qualifiedName(v)) + "\"";
      currentRequestedBy.clear();
      currentFile = v->file; // its errors point at its declaration
      try
      {
        globalFor(v, v, classTy);
      }
      catch (Unsupported&)
      {
      }
      currentFunctionName.clear();
      currentFile = nullptr;
    }

    void collectFunctions(Span<Decl*> decls, std::vector<FunctionDecl*>& out)
    {
      for (Decl* d : decls)
      {
        switch (d->kind)
        {
          case NodeKind::NamespaceDecl: collectFunctions(static_cast<NamespaceDecl*>(d)->decls, out); break;
          case NodeKind::ClassDecl:
            if (static_cast<ClassDecl*>(d)->templateParams.empty())
              collectFunctions(static_cast<ClassDecl*>(d)->members, out);
            break;
          case NodeKind::EnumDecl: collectFunctions(static_cast<EnumDecl*>(d)->members, out); break;
          case NodeKind::FunctionDecl:
          {
            auto* f = static_cast<FunctionDecl*>(d);
            if (f->body && f->templateParams.empty() && !global.isTemplateContext(f) && !(f->file && f->file->isImported))
              out.push_back(f);
            break;
          }
          default: break;
        }
      }
    }
  };
}

// What a build keeps for the next (CodegenSessionPtr): the shared state the emitters wrote, the
// emitters, and what assembling a module needs.
struct CodegenSession
{
  CodegenResult result; // the build's counts (and the result Shared refers to)
  std::unique_ptr<Shared> shared;
  std::vector<std::unique_ptr<Emitter>> emitters;
  std::string prefix;   // a module's first lines
  unsigned threads = 1;
  bool usable = false;  // a build of several modules without errors, its texts kept
};

void CodegenSessionDeleter::operator()(CodegenSession* session) const
{
  delete session;
}

static CodegenResult generate(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols, bool debug,
                              unsigned moduleCount, bool despiteErrors, unsigned threads, bool stubMissing, bool inlineTrivial,
                              CodegenSessionPtr* keep);

// Runs work(i) for i in [0, n): the calling thread runs work(0), n - 1 threads with 1 GB stacks
// (reserved, committed as used: the emitter recurses as deep as the code it walks) the rest.
template<typename F>
static void onThreads(unsigned n, F& work)
{
#ifdef _WIN32
  struct Arg
  {
    F* work;
    unsigned i;
  };
  std::vector<Arg> args(n);
  std::vector<HANDLE> handles;
  for (unsigned i = 1; i < n; ++i)
  {
    args[i] = {&work, i};
    HANDLE h = CreateThread(nullptr, size_t(1) << 30, [](void* p) -> DWORD
    {
      auto* a = static_cast<Arg*>(p);
      (*a->work)(a->i);
      return 0;
    }, &args[i], STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h)
      handles.push_back(h);
  }
  work(0);
  for (HANDLE h : handles)
  {
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
  }
#else
  std::vector<std::thread> pool;
  for (unsigned i = 1; i < n; ++i)
    pool.emplace_back([&work, i] { work(i); });
  work(0);
  for (std::thread& t : pool)
    t.join();
#endif
}

// --stub-missing: the definition of a function that was left out, which reports it and aborts when
// called (ret and params as its declaration has them, or `void` and `()` for one known by name only:
// a call names it with its own type). In every module that uses it: linkonce_odr.
static std::string stubText(const std::string& name, const std::string& ret, const std::string& params, const std::string& message)
{
  std::string inner = name.size() > 2 && name[1] == '"' ? name.substr(2, name.size() - 3) : name.substr(1);
  std::string bytes = message + '\0', escaped;
  for (unsigned char c : bytes)
    if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
      escaped += char(c);
    else
    {
      char buf[8];
      snprintf(buf, sizeof buf, "\\%02X", c);
      escaped += buf;
    }
  std::string constant = "@\"kpp.missing:" + inner + "\"";
  return "$" + name.substr(1) + " = comdat any\n" + constant + " = private unnamed_addr constant [" + std::to_string(bytes.size()) + " x i8] c\"" +
         escaped + "\"\ndefine linkonce_odr " + ret + " " + name + params + " comdat {\nentry:\n  call void @kpp_missing(ptr " + constant +
         ")\n  unreachable\n}\n";
}

// The global names a module's text uses without a definition or declaration (functions an error
// left out, with --keep-going), each declared as `declare void @name()`: with opaque pointers a
// call may name a function of another type, so the module compiles and the link reports the name.
// With stubs (--stub-missing), each is defined as a stub instead (stubText), its message the one
// stubs gives it or its name.
static std::string declareUndefined(const std::string& text, const std::unordered_map<std::string, std::string>* stubs = nullptr,
                                    std::unordered_set<std::string>* stubbed = nullptr)
{
  auto nameAt = [&](size_t i) -> std::string
  {
    size_t end;
    if (i + 1 < text.size() && text[i + 1] == '"')
    {
      end = text.find('"', i + 2);
      end = end == std::string::npos ? text.size() : end + 1;
    }
    else
    {
      end = i + 1;
      while (end < text.size() && (isalnum((unsigned char)text[end]) || strchr("-$._", text[end])))
        ++end;
    }
    return text.substr(i, end - i);
  };
  std::unordered_set<std::string> known, used;
  for (size_t line = 0; line < text.size();)
  {
    size_t next = text.find('\n', line);
    next = next == std::string::npos ? text.size() : next + 1;
    if (text.compare(line, 7, "define ") == 0 || text.compare(line, 8, "declare ") == 0)
    {
      size_t at = text.find(" @", line);
      if (at != std::string::npos && at < next)
        known.insert(nameAt(at + 1));
    }
    else if (text[line] == '@')
      known.insert(nameAt(line)); // `@name = ... global ...`
    line = next;
  }
  // A quoted name or string (`@"f::__lambda_1_2@10834::operator()()"`, c"name@factorio.com") and a
  // comment are skipped whole: an '@' inside them names nothing.
  for (size_t i = text.find_first_of("\"@;"); i != std::string::npos; i = text.find_first_of("\"@;", i + 1))
  {
    if (text[i] == '"' || text[i] == ';')
    {
      i = text.find(text[i] == '"' ? '"' : '\n', i + 1);
      if (i == std::string::npos)
        break;
      continue;
    }
    std::string name = nameAt(i);
    if (name.size() > 1 && name.rfind("@llvm.", 0) != 0 && !known.count(name))
      used.insert(name);
    i += name.size() - 1;
  }
  std::vector<std::string> sorted(used.begin(), used.end());
  std::sort(sorted.begin(), sorted.end());
  std::string out = sorted.empty() ? "" : "\n; left out by codegen errors (--keep-going)\n";
  for (const std::string& name : sorted)
    if (stubs)
    {
      auto it = stubs->find(name);
      out += stubText(name, "void", "()", it != stubs->end() ? it->second : name);
      if (stubbed)
        stubbed->insert(name);
    }
    else
      out += "declare void " + name + "()\n";
  return out;
}

CodegenResult generateLLVM(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols, bool debug,
                           unsigned moduleCount, bool despiteErrors, unsigned threads, bool stubMissing, bool inlineTrivial,
                           CodegenSessionPtr* keep)
{
#ifdef _WIN32
  // The emitter recurses as deep as the code it walks (long else-if chains, nested expressions),
  // so it runs on a thread with a 1 GB stack, reserved and committed only as it is used.
  struct Job
  {
    std::vector<std::unique_ptr<ParsedFile>>* files;
    GlobalScope* global;
    const SymbolTable* symbols;
    bool debug;
    unsigned moduleCount;
    bool despiteErrors;
    unsigned threads;
    bool stubMissing;
    bool inlineTrivial;
    CodegenSessionPtr* keep;
    CodegenResult result;
  } job{&files, &global, &symbols, debug, moduleCount, despiteErrors, threads, stubMissing, inlineTrivial, keep, {}};
  HANDLE thread = CreateThread(nullptr, size_t(1) << 30, [](void* p) -> DWORD
  {
    auto* j = static_cast<Job*>(p);
    j->result = generate(*j->files, *j->global, *j->symbols, j->debug, j->moduleCount, j->despiteErrors, j->threads, j->stubMissing, j->inlineTrivial,
                         j->keep);
    return 0;
  }, &job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (thread)
  {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return std::move(job.result);
  }
#endif
  return generate(files, global, symbols, debug, moduleCount, despiteErrors, threads, stubMissing, inlineTrivial, keep);
}

// --stub-missing: the functions used without a definition that declsFor defines as stubs.
static size_t countUndefinedStubs(Shared& sh)
{
  size_t n = 0;
  if (sh.stubMissing)
    for (auto& [name, f] : sh.undefinedUses)
      n += !sh.definedIn.count(name);
  return n;
}

// emitFunction with the stack overflow caught. With a 1 GB stack an overflow means recursion
// without end; the fault may have struck inside the allocator, leaving the heap mid-update (a
// later allocation then spins forever), so nothing is allocated after it: the function's name
// goes out through a fixed buffer and the process ends.
static void emitInstantiation(Emitter& em, const Instantiation& item)
{
  if (item.depth > Emitter::maxInstantiationDepth)
  {
    em.reportTooDeep(item);
    return;
  }
  em.currentModule = item.module;
  em.currentRequestedBy = item.requestedBy;
  em.currentDepth = item.depth;
  struct EmitCall { Emitter* e; const Instantiation* item; };
  EmitCall call{&em, &item};
#ifdef _WIN32
  if (callWithStackGuard([](void* p) {
    auto* c = static_cast<EmitCall*>(p);
    c->e->emitFunction(c->item->f, c->item->args, c->item->classTy, c->item->name);
  }, &call))
  {
    static char message[1024];
    int n = snprintf(message, sizeof message, "codegen: out of stack (recursion without end) in %.900s\n", em.currentFunctionName.c_str());
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, DWORD(n > 0 ? std::min(n, int(sizeof message) - 1) : 0), &written, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
  }
#else
  em.emitFunction(item.f, item.args, item.classTy, item.name);
#endif
  em.currentRequestedBy.clear();
  em.currentDepth = 0;
}

// The next level: what the emitters asked for (each function once: the names were claimed), in
// the order of their names, each with its first requester by name over all emitters
// (Emitter::levelRequests).
static void collectNextLevel(std::vector<std::unique_ptr<Emitter>>& emitters, std::vector<Instantiation>& level)
{
  level.clear();
  for (auto& em : emitters)
  {
    for (Instantiation& item : em->worklist)
      level.push_back(std::move(item));
    em->worklist.clear();
  }
  std::unordered_map<std::string, Request*> first;
  for (const Instantiation& item : level)
    first.emplace(item.name, nullptr);
  for (auto& em : emitters)
    for (auto& [name, r] : em->levelRequests)
      if (auto it = first.find(name); it != first.end() && (!it->second || r.by < it->second->by))
        it->second = &r;
  for (Instantiation& item : level)
    if (Request* r = first[item.name])
    {
      item.requestedBy = r->by;
      item.depth = r->depth;
    }
  for (auto& em : emitters)
    em->levelRequests.clear();
  std::sort(level.begin(), level.end(), [](const Instantiation& a, const Instantiation& b) { return a.name < b.name; });
}

// The destructor helper an entry of the shared definitions defines (`@kpp.dtor.<class>`, from
// destructorThunk), or empty for any other entry.
static std::string destructorHelperIn(const std::string& entry)
{
  const std::string_view mark = "define linkonce_odr void @kpp.dtor.";
  size_t at = entry.find(mark);
  if (at == std::string::npos)
    return {};
  size_t start = at + mark.size() - std::string_view("@kpp.dtor.").size();
  size_t end = entry.find('(', start);
  return end == std::string::npos ? std::string() : entry.substr(start, end - start);
}

static CodegenResult generate(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols, bool debug,
                              unsigned moduleCount, bool despiteErrors, unsigned threads, bool stubMissing, bool inlineTrivial,
                              CodegenSessionPtr* keep)
{
  // The build's state lives in a session (Shared refers to its result), kept for the next build
  // when asked (keep) or freed.
  CodegenSessionPtr session(new CodegenSession);
  CodegenResult& result = session->result;
  auto shared = std::make_unique<Shared>(result);
  Shared& sh = *shared;
  sh.debug = debug;
  sh.stubMissing = despiteErrors && stubMissing;
  sh.inlineTrivial = inlineTrivial;
  sh.setModules(moduleCount);
  for (size_t i = 0; i < files.size(); ++i)
    sh.fileModule[files[i].get()] = unsigned(i % sh.moduleCount);
  // One emitter per thread; the first one also finishes the runtime and assembles the modules.
  threads = std::max(1u, threads);
  std::vector<std::unique_ptr<Emitter>> emitters;
  for (unsigned i = 0; i < threads; ++i)
    emitters.push_back(std::make_unique<Emitter>(global, symbols, sh));
  session->threads = threads;
  // The emitters' and the shared state are freed on a thread of their own once the result is
  // returned (about 3 s for the game, on the build's critical path before): a build in its own
  // process exits without waiting for it, the compile server answers meanwhile. Their destructors
  // touch nothing but their own containers (not the result, not the program they point into).
  // A session kept for the next build takes them instead.
  struct FreeLater
  {
    std::unique_ptr<Shared>& shared;
    std::vector<std::unique_ptr<Emitter>>& emitters;
    CodegenSessionPtr& session;
    CodegenSessionPtr* keep;
    ~FreeLater()
    {
      if (keep && session->usable)
      {
        session->shared = std::move(shared);
        session->emitters = std::move(emitters);
        *keep = std::move(session);
        return;
      }
      std::thread([shared = std::move(shared), emitters = std::move(emitters), session = std::move(session)]() mutable
      {
        emitters.clear();
        shared.reset();
        session.reset();
      }).detach();
    }
  } freeLater{shared, emitters, session, keep};
  Emitter& e = *emitters[0];
  std::vector<FunctionDecl*> functions;
  std::vector<VariableDecl*> startupGlobals;
  for (auto& file : files)
  {
    e.settleDefinitionFlags(file->decls);
    e.collectFunctions(file->decls, functions);
    if (!file->isImported)
      e.collectStartupGlobals(file->decls, startupGlobals);
  }
  // `template struct Registry<int>;`: the instance's static data members too (the Lua bindings'
  // LuaStruct<T> register their metamethods so), built where the explicit instantiation is
  // written, in their definitions' order.
  std::vector<std::pair<VariableDecl*, const Ty*>> instanceStartupGlobals;
  std::vector<Shared::Startup> instanceStartupAt;
  for (auto& file : files)
    if (!file->isImported)
      for (auto& [cls, token] : global.explicitInstantiationsOf(*file))
      {
        e.collectInstanceStartupGlobals(cls, instanceStartupGlobals);
        instanceStartupAt.resize(instanceStartupGlobals.size(), Shared::Startup{file->index, token, 0, ""});
      }
  // Every global built at startup is defined and built, read or not (C++ initialises them all);
  // what their initializers ask for joins the first level's requests.
  for (VariableDecl* v : startupGlobals)
    e.emitStartupGlobal(v);
  for (size_t i = 0; i < instanceStartupGlobals.size(); ++i)
    e.emitStartupGlobal(instanceStartupGlobals[i].first, instanceStartupGlobals[i].second, instanceStartupAt[i]);
  // Level 0: the functions the program writes. Each later level: the instantiations the previous
  // one asked for. The functions of a level are emitted on all threads at once.
  std::vector<Instantiation> level;
  for (FunctionDecl* f : functions)
  {
    auto it = sh.fileModule.find(f->file);
    level.push_back({f, {}, nullptr, it == sh.fileModule.end() ? 0 : it->second, "", "", 0});
  }
  auto emitOne = [&](Emitter& em, const Instantiation& item) { emitInstantiation(em, item); };
  auto nextLevel = [&]() { collectNextLevel(emitters, level); };
  std::atomic<bool> outOfMemory{false};
  size_t emitted = 0;
  std::set<std::pair<StaticAssertDecl*, std::string>> layoutAssertsChecked;
  // Emits `level` and every level it asks for, on all threads (also what an incremental build
  // emits again: KPP_CHECK_REBUILD below).
  auto emitLevels = [&]()
  {
    while (!outOfMemory)
    {
      if (level.empty())
      {
        // Everything is emitted: the static_asserts on a class's size or alignment, which the
        // checker leaves to the layouts; those of class template instances once the functions
        // that name the instances are emitted (and one in an instance's function if that is
        // emitted). A layout they build can ask for functions (its vtable's), which go on.
        global.checkInstanceStaticAsserts();
        bool checked = false;
        for (const GlobalScope::LayoutStaticAssert& a : global.layoutStaticAsserts())
          if (layoutAssertsChecked.insert({a.decl, a.instance}).second)
          {
            e.checkLayoutAssert(a);
            checked = true;
          }
        if (!checked)
          break;
        nextLevel();
        continue;
      }
      std::atomic<size_t> next{0};
      auto work = [&](unsigned i) {
        try
        {
          for (size_t k; !outOfMemory && (k = next++) < level.size();)
            emitOne(*emitters[i], level[k]);
        }
        catch (const std::bad_alloc&)
        {
          outOfMemory = true;
        }
      };
      onThreads(level.size() < 64 ? 1 : threads, work);
      emitted += level.size();
      nextLevel();
    }
  };
  emitLevels();
  e.finishAssertRuntime();
  e.finishExceptionRuntime();
  if (sh.stubMissing)
  {
    // --stub-missing: what each stub says, the function and where it is (a failed function's own
    // line, a function used without a definition its declaration's).
    for (auto& em : emitters)
      for (auto& [name, where] : em->failedAt)
        sh.stubMessages.emplace(name, where);
    for (auto& [name, f] : sh.undefinedUses)
      if (!sh.definedIn.count(name))
        sh.stubMessages.emplace(name, e.qualifiedName(f) + " at " + e.locationOf(f, f->file) + " (used, but no definition of it was found)");
    e.finishStubRuntime();
  }
  // A closure that functions name but whose lambda was never laid out: the function it is written
  // in failed before it (an error), and so did those functions, but with --keep-going the modules
  // have to parse, so the type gets a placeholder. Without any error it is a closure nobody built.
  {
    bool anyErrors = false;
    for (auto& em : emitters)
      anyErrors = anyErrors || !em->errors.empty();
    for (auto& [t, info] : sh.closures)
      if (!info.laidOut && !info.structName.empty())
      {
        if (!anyErrors)
        {
          e.currentFile = t->decl->file;
          e.error(t->decl, "error: the closure of " + e.irString(t) + " is used, but its lambda was never emitted");
        }
        sh.decls += info.structName + " = type { i8 } ; left out by codegen errors\n";
      }
  }
  // Functions of the program used where no definition of theirs was found (an out-of-line member
  // the resolver did not attach to its declaration, a template whose definition it did not find):
  // each an error at its declaration, never only a name the link misses.
  for (auto& [name, f] : sh.undefinedUses)
    if (!sh.definedIn.count(name))
    {
      e.currentFile = f->file;
      e.error(f, "error: " + e.qualifiedName(f) + " is used, but no definition of it was found (" + name + ")");
    }
  // A lambda's call operator is emitted with the function the lambda is written in; one called
  // that never was is reported, unless that function failed (its own error says why).
  {
    std::unordered_set<std::string> failed;
    for (auto& em : emitters)
      failed.insert(em->failedFunctions.begin(), em->failedFunctions.end());
    for (auto& [name, lambda] : sh.closureCalls)
      if (!sh.definedIn.count(name))
      {
        const std::string& writer = sh.closures[lambda].writtenIn;
        if (!writer.empty() && failed.count(writer))
          continue;
        e.currentFile = lambda->decl->file;
        e.error(lambda->decl, "error: the call operator of a lambda written in " + (writer.empty() ? std::string("a function never emitted") : writer) +
                                  " is used, but it was never emitted (" + name + ")");
      }
  }
  // The errors, in the same order in every run.
  for (auto& em : emitters)
  {
    result.errors.insert(result.errors.end(), em->errors.begin(), em->errors.end());
    result.failedFunctions.insert(result.failedFunctions.end(), em->failedFunctions.begin(), em->failedFunctions.end());
  }
  std::sort(result.errors.begin(), result.errors.end());
  std::sort(result.failedFunctions.begin(), result.failedFunctions.end());
  if (outOfMemory)
    result.errors.push_back("codegen: out of memory after " + std::to_string(emitted) + " functions");
  result.functions = sh.functions;
  // Each destructor helper is defined in one home module, hash(name) % moduleCount like a function,
  // and declared by the modules that use it (moduleText's declares): in every module, they were 843
  // of the game's 1,760 MB of IR. The home depends only on the helper's name (its class's IR type
  // name) and the module count, so a helper does not move when other code changes.
  if (sh.moduleCount > 1)
  {
    std::vector<std::string> everywhere;
    for (std::string& entry : sh.shared.entries)
    {
      std::string name = destructorHelperIn(entry);
      if (name.empty())
      {
        everywhere.push_back(std::move(entry));
        continue;
      }
      unsigned home = unsigned(std::hash<std::string>()(name) % sh.moduleCount);
      sh.definedIn[name] = home;
      sh.declareLines[name] = "declare void " + name + "(ptr)";
      // weak_odr, not linkonce_odr: the home module's copy is the only one, and a release build's
      // optimizer drops a linkonce_odr definition its own module does not call (the link then
      // missed the helpers the other modules declare).
      if (size_t at = entry.find("define linkonce_odr void " + name + "("); at != std::string::npos)
        entry.replace(at, std::string_view("define linkonce_odr").size(), "define weak_odr");
      sh.definitions[home].push_back(Shared::definitionOf(std::move(entry)));
    }
    sh.shared.entries = std::move(everywhere);
  }
  sh.decls.finish();
  sh.shared.finish();
  sh.body.finish();
  // KPP_CHECK_REBUILD=n, a check of the incremental path (docs/compile-speed.md, step 5): every
  // n-th function of the program is emitted again in the kept state, as an edit of its body would
  // be, and the modules assembled again must be byte for byte the ones just built.
  const char* checkRebuild = getenv("KPP_CHECK_REBUILD");
  bool keepTexts = (checkRebuild || keep) && result.errors.empty() && sh.moduleCount > 1;
  for (unsigned k = 0; k < sh.moduleCount; ++k)
  {
    // The definitions in the order SharedText puts entries: by their text.
    std::sort(sh.definitions[k].begin(), sh.definitions[k].end(),
              [](const Shared::Definition& a, const Shared::Definition& b) { return a.text < b.text; });
    if (keepTexts)
      sh.keptModuleEntries[k] = sh.moduleDecls[k].entries;
    for (const Shared::OwnedEntry& o : sh.ownedEntries[k])
      sh.moduleDecls[k].entries.push_back(o.text);
    sh.moduleDecls[k].finish();
  }
  // Skip IR assembly when there are errors: the IR would be invalid anyway, and assembling
  // gigabytes of text just to discard it wastes memory and time. With despiteErrors the IR is
  // assembled from what was emitted, and what an error left out is declared (declareUndefined).
  if (!result.errors.empty() && !despiteErrors)
    return std::move(result);
  std::string prefix = "; Kpp v0: generated by kppc\n"
                       "target datalayout = \"e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n"
                       "target triple = \"x86_64-pc-windows-msvc\"\n\n";
  if (e.moduleCount <= 1)
  {
    // Pre-compute pieces and reserve to avoid temporary-string OOM on large programs.
    std::string decls = e.declsFor(0);
    std::string ctors = e.ctorsText(0);
    size_t total = prefix.size() + decls.size() + e.moduleDecls[0].text.size() + ctors.size() + 1 + e.shared.text.size() + e.body.text.size();
    result.ir.reserve(total);
    result.ir.append(prefix);
    result.ir.append(decls);
    result.ir.append(e.moduleDecls[0].text);
    result.ir.append(ctors);
    result.ir.push_back('\n');
    result.ir.append(e.shared.text);
    result.ir.append(std::move(e.body.text));
    if (!result.errors.empty())
    {
      std::unordered_set<std::string> stubbed;
      result.ir += declareUndefined(result.ir, sh.stubMissing ? &sh.stubMessages : nullptr, &stubbed);
      result.stubs = stubbed.size() + countUndefinedStubs(sh);
    }
    return std::move(result);
  }
  // The modules are independent once the texts are finished: assembled on all the emitters'
  // threads at once (on one thread this was 4.5 s of the game's codegen), each module's own texts
  // freed as soon as it is assembled.
  e.prepareModuleTexts();
  result.modules.resize(e.moduleCount);
  std::vector<std::unordered_set<std::string>> stubbedIn(e.moduleCount);
  std::atomic<unsigned> nextModule{0};
  auto assemble = [&](unsigned thread)
  {
    Emitter& em = *emitters[thread];
    for (unsigned k = nextModule++; k < em.moduleCount; k = nextModule++)
    {
      result.modules[k] = em.moduleText(k, prefix);
      if (!keepTexts)
      {
        std::vector<Shared::Definition>().swap(sh.definitions[k]);
        std::string().swap(sh.moduleDecls[k].text);
      }
      if (!result.errors.empty())
        result.modules[k] += declareUndefined(result.modules[k], sh.stubMissing ? &sh.stubMessages : nullptr, &stubbedIn[k]);
    }
  };
  onThreads(unsigned(emitters.size()), assemble);
  std::unordered_set<std::string> stubbed;
  for (const std::unordered_set<std::string>& names : stubbedIn)
    stubbed.insert(names.begin(), names.end());
  if (sh.stubMissing)
    result.stubs = stubbed.size() + countUndefinedStubs(sh);
  session->prefix = prefix;
  if (keepTexts && checkRebuild)
  {
    // Every n-th function of the program emitted again through rebuildLLVM, as the compile server
    // will after an edit of their bodies; the modules it assembles again must be the ones just built.
    size_t every = std::max<size_t>(1, strtoul(checkRebuild, nullptr, 10));
    std::vector<FunctionDecl*> picked;
    for (size_t i = 0; i < functions.size(); i += every)
      picked.push_back(functions[i]);
    session->shared = std::move(shared);
    session->emitters = std::move(emitters);
    session->usable = true;
    auto start = std::chrono::steady_clock::now();
    bool rebuilt = false;
    CodegenResult again = rebuildLLVM(*session, picked, {}, {}, rebuilt);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    shared = std::move(session->shared);
    emitters = std::move(session->emitters);
    session->usable = false;
    size_t changed = 0, differ = 0;
    std::string first;
    for (size_t k = 0; rebuilt && k < again.changedModules.size(); ++k)
      if (again.changedModules[k])
      {
        ++changed;
        if (again.modules[k] != result.modules[k])
        {
          ++differ;
          if (first.empty())
          {
            size_t at = std::mismatch(again.modules[k].begin(), again.modules[k].end(), result.modules[k].begin(), result.modules[k].end()).first -
                        again.modules[k].begin();
            size_t line = again.modules[k].rfind('\n', at);
            first = "module " + std::to_string(k) + ": " + again.modules[k].substr(line == std::string::npos ? 0 : line + 1, 160);
          }
        }
      }
    fprintf(stderr, "rebuild check: %zu functions emitted again by rebuildLLVM in %.3f s (%s), %zu modules assembled again, %zu of them differ%s%s\n",
            picked.size(), seconds, rebuilt ? "rebuilt" : "declined", changed, differ, first.empty() ? "" : "; first: ", first.c_str());
  }
  session->usable = keep && keepTexts && !checkRebuild;
  return std::move(result);
}

CodegenResult rebuildLLVM(CodegenSession& session, const std::vector<FunctionDecl*>& changedBodies, const std::vector<FunctionDecl*>& added,
                          const std::vector<FunctionDecl*>& removed, bool& rebuilt)
{
  rebuilt = false;
  CodegenResult out;
  if (!session.usable || !session.shared || session.emitters.empty())
    return out;
  session.usable = false; // until this rebuild is done: one that fails leaves the session to a build from scratch
  Shared& sh = *session.shared;
  std::vector<std::unique_ptr<Emitter>>& emitters = session.emitters;
  Emitter& e = *emitters[0];
  for (auto& em : emitters)
    em->lineStarts.clear(); // the edited files' lines moved
  // What the changed and removed functions' emissions added goes (their definitions, their
  // lambdas' operators, their static locals); the changed and the added ones are emitted again,
  // each in its file's module as a program function is.
  std::unordered_set<std::string> owners;
  std::vector<Instantiation> level;
  auto programFunction = [&](FunctionDecl* f) -> Instantiation
  {
    auto it = sh.fileModule.find(f->file);
    return {f, {}, nullptr, it == sh.fileModule.end() ? 0 : it->second, "", "", 0};
  };
  for (FunctionDecl* f : changedBodies)
  {
    owners.insert(e.functionName(f, {}, nullptr));
    level.push_back(programFunction(f));
  }
  for (FunctionDecl* f : removed)
    owners.insert(e.functionName(f, {}, nullptr));
  for (FunctionDecl* f : added)
    level.push_back(programFunction(f));
  std::vector<char> touched(sh.moduleCount, 0);
  std::unordered_set<std::string> droppedNames;
  for (unsigned k = 0; k < sh.moduleCount; ++k)
  {
    std::vector<Shared::Definition>& defs = sh.definitions[k];
    size_t before = defs.size();
    for (const Shared::Definition& d : defs)
      if (owners.count(d.owner))
        droppedNames.insert(d.name);
    defs.erase(std::remove_if(defs.begin(), defs.end(), [&](const Shared::Definition& d) { return owners.count(d.owner) != 0; }), defs.end());
    std::vector<Shared::OwnedEntry>& owned = sh.ownedEntries[k];
    size_t ownedBefore = owned.size();
    owned.erase(std::remove_if(owned.begin(), owned.end(), [&](const Shared::OwnedEntry& o) { return owners.count(o.owner) != 0; }), owned.end());
    touched[k] = before != defs.size() || ownedBefore != owned.size();
  }
  for (auto& [t, info] : sh.closures)
    if (droppedNames.count(info.functionName))
      sh.closuresEmitted.erase(t);
  for (const std::string& name : droppedNames)
  {
    sh.lambdaInstances.erase(name);
    sh.definedIn.erase(name);
    sh.declareLines.erase(name);
  }
  sh.functions -= std::min<size_t>(sh.functions, droppedNames.size());
  std::vector<size_t> counts(sh.moduleCount);
  for (unsigned k = 0; k < sh.moduleCount; ++k)
    counts[k] = sh.definitions[k].size() + sh.ownedEntries[k].size() + sh.moduleDecls[k].entries.size();
  size_t sharedBefore = sh.decls.entries.size() + sh.shared.entries.size();
  for (auto& em : emitters)
  {
    em->errors.clear();
    em->failedFunctions.clear();
  }
  while (!level.empty())
  {
    std::atomic<size_t> next{0};
    auto work = [&](unsigned i)
    {
      for (size_t k; (k = next++) < level.size();)
        emitInstantiation(*emitters[i], level[k]);
    };
    onThreads(level.size() < 64 ? 1u : session.threads, work);
    collectNextLevel(emitters, level);
  }
  // An error, or a first use that added to the shared declarations (in every module): from scratch.
  for (auto& em : emitters)
    if (!em->errors.empty())
      return out;
  if (sh.decls.entries.size() + sh.shared.entries.size() != sharedBefore)
    return out;
  std::vector<unsigned> which;
  for (unsigned k = 0; k < sh.moduleCount; ++k)
  {
    if (counts[k] != sh.definitions[k].size() + sh.ownedEntries[k].size() + sh.moduleDecls[k].entries.size())
      touched[k] = 1;
    if (!touched[k])
      continue;
    which.push_back(k);
    std::sort(sh.definitions[k].begin(), sh.definitions[k].end(),
              [](const Shared::Definition& a, const Shared::Definition& b) { return a.text < b.text; });
    // The module's declarations again: those kept, those a first use added now (kept from here on),
    // and its functions' own.
    std::vector<std::string>& kept = sh.keptModuleEntries[k];
    kept.insert(kept.end(), sh.moduleDecls[k].entries.begin(), sh.moduleDecls[k].entries.end());
    std::vector<std::string> entries = kept;
    for (const Shared::OwnedEntry& o : sh.ownedEntries[k])
      entries.push_back(o.text);
    sh.moduleDecls[k].entries = std::move(entries);
    sh.moduleDecls[k].text.clear();
    sh.moduleDecls[k].finish();
  }
  out.modules.resize(sh.moduleCount);
  out.changedModules.assign(sh.moduleCount, false);
  std::atomic<size_t> nextModule{0};
  auto assemble = [&](unsigned t)
  {
    for (size_t i; (i = nextModule++) < which.size();)
      out.modules[which[i]] = emitters[t]->moduleText(which[i], session.prefix);
  };
  onThreads(std::max(1u, std::min(session.threads, unsigned(which.size()))), assemble);
  for (unsigned k : which)
    out.changedModules[k] = true;
  out.functions = sh.functions;
  out.structs = session.result.structs;
  session.usable = true;
  rebuilt = true;
  return out;
}
