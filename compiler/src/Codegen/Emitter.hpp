// struct Emitter, one per codegen thread: its state, and its members in the files it includes, each
// where it stood in codegen.cpp. Part of codegen.cpp, included in its namespace there (a file of
// its own since 2026-11-05, moved without a change). What is the current function's is its base,
// FunctionState.
#include "FunctionState.hpp"

  struct Emitter : FunctionState
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
    // The program's own functions it emitted (level 0), by IR name: gathered into
    // Shared::programFunctions at level 0's barrier and after a server's build.
    std::vector<std::string> programFunctionsNow;
    // Its errors and the functions they left out, gathered once all are emitted.
    std::vector<std::string> errors, failedFunctions;
    std::unordered_map<std::string, std::string> failedAt; // failed function's IR name -> "qualified name at file:line"
    // Where its errors came from, for a compile server's session kept across builds with errors
    // (rebuildLLVM): the program functions (level 0) whose own emission raised them, by IR name, and
    // whether any came from elsewhere (an instance, a global's constructor, a shared text, a check
    // after the emission), which a later build cannot emit again on its own.
    std::unordered_map<std::string, FunctionDecl*> erroring;
    bool unattributedErrors = false;
    // The failures not shown (error's shown false: each follows from an error the checker reported):
    // failures all the same, for every decision that asks whether codegen failed (errors holds the
    // shown ones only).
    size_t hiddenErrors = 0;
    bool& mallocDeclared = sh.mallocDeclared;
    std::atomic<bool>& usesExceptions = sh.usesExceptions;
    std::unordered_map<std::string, bool>& emittedShared = sh.emittedShared;
    std::string& exceptionTypeinfo = sh.exceptionTypeinfo;
    uint64_t& exceptionMessageMark = sh.exceptionMessageMark;
    std::unordered_set<std::string>& sharedDeclared = sh.sharedDeclared;
    // The shared lock, and how many times this emitter holds it. An answer is published for the
    // other emitters (FinalAnswers) only while it holds the lock once: then no layout of its own is
    // half built (buildStruct marks a layout complete before it makes its vtables, and a layout
    // published from there had another emitter call a virtual function directly), and nobody else
    // is building one (that is done under the lock).
    int sharedDepth = 0;
    struct SharedLock
    {
      std::unique_lock<SpinRecursiveMutex> lock;
      int& depth;
      SharedLock(SpinRecursiveMutex& m, int& depth) : lock(m), depth(depth) { ++depth; }
      ~SharedLock() { --depth; }
    };
    SharedLock sharedLock() { return SharedLock(sh.mutex, sharedDepth); }
    bool mayPublish() const { return sharedDepth == 1; }
    // A function's definition (addDefinition), counted in result.functions; an incremental build
    // takes the count of the ones it drops off again.
    void addFunction(const std::string& name, const std::string& text, bool ownItself = false)
    {
      addDefinition(name, text, ownItself, true);
      ++sh.functions;
    }

    unsigned currentModule = 0;  // where the function being emitted goes
    // What a first use emits, whichever user is first (a generic lambda's call operator for its
    // arguments, a lambda's function-pointer thunk; a lambda's call operator its closure's first writer
    // emits goes to lambdaHome's), goes to the module its name hashes to, as an
    // instantiation does (queueInstantiation): users in two modules emitted at once (callers of one
    // level) would otherwise move it between runs. Its owner stays the user's.
    struct NameHome
    {
      Emitter& e;
      unsigned module;
      NameHome(Emitter& e, const std::string& name) : e(e), module(e.currentModule)
      {
        if (e.moduleCount > 1)
          e.currentModule = e.sh.emissionModule(std::hash<std::string>()(name));
      }
      NameHome(Emitter& e, unsigned home) : e(e), module(e.currentModule)
      {
        if (e.moduleCount > 1)
          e.currentModule = home;
      }
      ~NameHome() { e.currentModule = module; }
    };
    bool emittingShared = false; // string constants go to the shared decls
    // The function emitFunction emits, as the checker typed it (an instance's copy; fail).
    const FunctionDecl* checkerChecked = nullptr;
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
    struct ArgsScope
    {
      Emitter& e;
      const TemplateArgs* saved;
      ArgsScope(Emitter& e, const TemplateArgs* args) : e(e), saved(e.currentArgs) { e.currentArgs = args; }
      ~ArgsScope() { e.currentArgs = saved; }
    };

    struct Value
    {
      std::string text;         // IR operand; for class types the object's address
      const Ty* ty;             // Value type (never a reference)
      bool isTemporary = false; // Class temporary owned by the current full-expression
    };
    // The last scalar assignment emitted and the place it stored to: an assignment used as an
    // lvalue (`int& r = (a[i++] = v);`) is that place, its left operand not evaluated again.
    const BinaryExpr* assignedAt = nullptr;
    std::string assignedPlace;
    const Expr* evaluatedFirst = nullptr; // the stand-in for an assignment's right operand evaluated before its left
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
    Span<uint32_t> pendingDesignators;                        // of the brace list constructInto is about to consume
    std::unordered_map<Decl*, int64_t> enumeratorValues;
    std::unordered_map<Decl*, std::string> qualifiedNames; // qualifiedName() per declaration

    unsigned currentDepth = 0;       // of the function being emitted: 0 for one that is not an instantiation
    std::string currentFunctionName; // LLVM name of the function being emitted
    std::string currentRequestedBy;  // for an instantiation: the function that first used it
    // What an error's `(in ...)` calls them (fail): the function as errors print types
    // (displayFunctionName: `a(Item^&)`, `Probe::Reading(int32)`, `sum<int32>(const Vector<int32>&)`), a
    // lambda by the function it is written in and its line, a vtable or a global's initializer; the
    // LLVM names above are the program's, never shown. Empty: those names are shown.
    std::string currentContext;
    std::string currentRequestedByContext;

    static bool isLambda(const Ty* t) { return t && t->kind == TyKind::Lambda && t->decl; }
    static bool isAggregate(const Ty* t) { return isClass(t) || isLambda(t); }
    // A result that comes back through the caller's hidden result slot: a class, and a closure (a
    // lambda returned by value lives in memory like every closure; returned as an LLVM value, its
    // address and its bytes were confused: `ret %closure %o3`).
    static bool returnsInMemory(const Ty* t) { return isAggregate(t); }
    static LambdaExpr* lambdaOf(const Ty* t) { return isLambda(t) && t->decl->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(t->decl)->lambda : nullptr; }
    static bool isGenericLambda(const Ty* t) { LambdaExpr* l = lambdaOf(t); return l && !l->templateParams.empty(); }


    std::string funcletOverride;                              // the pad being emitted, for its calls

    Emitter(GlobalScope& global, const SymbolTable& symbols, Shared& sh) : global(global), types(global.types()), symbols(symbols), sh(sh) {}

#include "Diagnostics.inl"
#include "EmitterNames.inl"
#include "Layout.inl"
#include "ClassMetadata.inl"
#include "ParameterPacks.inl"
#include "Lambdas.inl"
#include "DebugInfo.inl"
#include "Functions.inl"
#include "CCallingConvention.inl"
#include "Calls.inl"
#include "Constants.inl"
#include "Values.inl"
#include "ConstantData.inl"
#include "Operators.inl"
#include "SseBuiltins.inl"
#include "Expressions.inl"
#include "Scopes.inl"
#include "Exceptions.inl"
#include "ItaniumExceptions.inl"
#include "Statements.inl"
#include "TypedUnions.inl"
#include "ControlFlow.inl"
#include "FunctionDefinitions.inl"
  };
