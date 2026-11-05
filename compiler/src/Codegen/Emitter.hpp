// struct Emitter, one per codegen thread: its state, and its members in the files it includes, each
// where it stood in codegen.cpp. Part of codegen.cpp, included in its namespace there (a file of
// its own since 2026-11-05, moved without a change).
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
    std::string fn;      // current function text (after the allocas)
    std::string allocas; // entry-block allocas of the current function
    // --debug-info: the declaration (a function or a lambda) whose definition is being emitted and its
    // file, or null for a definition without line information (a global's startup function).
    Node* debugAt = nullptr;
    ParsedFile* debugFile = nullptr;
    // --debug-info unoptimized (Shared::fullDebugInfo): the definition's variables (the address that
    // holds one, its name, its type's placeholder, its line and block, its parameter number from 1),
    // its lexical blocks (the enclosing block, 0 for the function's own scope, and the line) and the
    // block the code being emitted is in; debugLines makes them metadata.
    struct DebugVariable
    {
      std::string address, name, type;
      size_t line = 0;
      uint32_t block = 0, arg = 0;
      bool artificial = false;
      bool defined = false; // its address is an instruction's in the body (a lambda's capture: its field), declared after it
    };
    std::vector<DebugVariable> debugVariables;
    struct DebugBlock
    {
      uint32_t parent = 0;
      size_t line = 0;
    };
    std::vector<DebugBlock> debugBlocks;
    uint32_t debugBlock = 0;
    uint32_t temp = 0;
    uint32_t label = 0;
    bool blockTerminated = false;
    std::string currentBlock = "entry";
    FunctionDecl* currentFunction = nullptr;
    // The body being emitted: the function's, or the lambda's whose call operator is (its static
    // locals are declared in it, staticLocal).
    Node* bodyNow = nullptr;
    // The function emitFunction emits, as the checker typed it (an instance's copy; fail).
    const FunctionDecl* checkerChecked = nullptr;
    const Ty* currentReturn = nullptr;
    std::string sretAddr; // Hidden return slot when returning a class.
    // Named return elision (design.md 23): the local the function's every return returns, built in
    // the caller's result (sretAddr) instead of a slot of its own; null when there is none.
    VariableDecl* namedReturn = nullptr;
    // While a return of namedReturn leaves its scopes: the result's address, whose object (that
    // local) they do not destroy (destroyOnExit). Empty otherwise.
    std::string keptOnReturn;
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
    std::unordered_map<uint32_t, std::string> labelBlocks; // goto labels of the current function -> their blocks
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
    // destructors of the objects between a call and its handler run in cleanup pads. Off Windows, the
    // Itanium model's landing pads (itaniumUnwindDestination, itaniumTry): the frames are the same.
    struct EHFrame
    {
      bool isHandler;            // false: an open try body; true: an open catch handler
      std::string dispatchLabel; // try: the block of its catchswitch; handler: where an exception leaving
                                 // it unwinds, its catchswitch's unwind destination ("": the caller)
      std::string parentFunclet; // the funclet token the try or handler is in ("" at top level)
      std::string funclet;       // handler: its catchpad token
      std::string exceptionSlot; // handler: the alloca holding the caught exception record
      size_t scopeDepth;         // scopes.size() when entered
      bool used = false;         // try: some call in the body unwinds to the dispatch
      std::vector<std::string> catchTypes; // off Windows, a try: its handlers' descriptors in order ("null": catch (...))
    };
    std::vector<EHFrame> ehFrames;
    std::string ehBlocks;                                     // cleanup pads of the current function
    std::unordered_map<std::string, std::string> cleanupPads; // destruction state -> cleanup block
    std::string funcletOverride;                              // the pad being emitted, for its calls
    bool itaniumCleanup = false;                              // off Windows: a cleanup block is being emitted, for its calls
    bool usesEH = false;                                      // the current function has invokes or pads
    std::string returnLabel; // a destructor's: where a `return` goes, the members and bases destroyed after the body

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
