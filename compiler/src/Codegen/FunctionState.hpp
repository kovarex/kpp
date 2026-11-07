// What the emission of one function holds (J's refactoring list, item 3): every member of struct
// Emitter that is the current function's, declared once, here. Emitter derives from FunctionState, so
// its code names them as members; saveState and restoreState copy the struct whole around a lambda's
// call operator emitted in the middle of a function, and resetFunctionState sets FunctionBody anew
// for each function. A member added here is saved, restored and reset with the rest, never left out
// of one of the three by hand (NRVO's namedReturn and keptOnReturn needed an edit in each). Part of
// codegen.cpp, included in its namespace by Codegen/Emitter.hpp.

  // What the caller sets before emitting a function's body (emitFunction, emitLambda), kept by
  // resetFunctionState.
  struct FunctionContext
  {
    FunctionDecl* currentFunction = nullptr;
    const Ty* currentReturn = nullptr;
    std::string sretAddr; // Hidden return slot when returning a class.
    const Ty* currentClassTy = nullptr; // Instantiated class of the method being emitted.
    // design.md 24: the body's nodes were typed by a walk for this function alone (a plain function's,
    // an instantiation's own copy or a private copy retyped per element, a lambda in either, a
    // global's initializer: its own or its instance's copy, a generic lambda's instance's copy or its
    // private copy), so what the checker recorded on them (an operator's callee) is this function's
    // and codegen calls it instead of choosing again. False for a lambda's call operator emitted on
    // demand and a destructor thunk: codegen chooses there itself.
    bool choicesRecorded = false;
    // A generic lambda's call operator emitted from a private copy of its instance's body (one with
    // pack expansions or a `for constexpr` over a pack, retyped per element): that copy and the
    // lambda's type, which retypeForElement retypes instead of currentFunction. Null otherwise.
    LambdaExpr* retypedLambda = nullptr;
    const Ty* retypedLambdaTy = nullptr;
  };

  // What emitting a function's body builds up, from nothing for each function.
  struct FunctionBody
  {
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
      bool indirect = false; // its address holds the variable's address (a class parameter passed by pointer): DW_OP_deref
      uint64_t offset = 0;   // the variable is that many bytes into what the address gives (a structured binding's member)
    };
    struct DebugBlock
    {
      uint32_t parent = 0;
      size_t line = 0;
    };
    struct Local
    {
      std::string addr; // Address of the variable (or of the pointer slot for references).
      const Ty* ty;
      bool isRef;
    };
    // An argument of the call a check tests (design.md 8.7): its address, or a scalar's value and
    // LLVM type. No type: it has no value to show.
    struct CheckedArgument
    {
      const Ty* type = nullptr;
      std::string value;
      std::string scalar;
    };
    struct Object
    {
      std::string addr;
      const Ty* ty;
      bool exception = false; // addr holds a caught exception record: ending it destroys the object
      // Destroyed only by an exception leaving its scope (cleanup pads), never on a normal exit: a
      // constructor's bases and members as it builds them, which outlive it when it returns.
      bool unwindOnly = false;
      bool freeMemory = false; // `new`'s allocation while its constructor runs: freed, not destroyed
      std::string freeCall;    // freeMemory's call of the class's operator delete instead of free ("void @f(ptr %a, i64 8)")
      // An array built in a loop (InlineParts::builtCounted): only its first elements are built,
      // as many as the i64 at countSlot says, and only those are destroyed, the last first.
      std::string countSlot;
      // A static local's guard (staticLocal) while its initializer runs: an exception leaving gives
      // the claim back (the guard to 0, the next pass tries again), never a destructor.
      std::string guardReset;
      // A temporary of one operand of ?:, && or || (keepOperandTemps): it lives to the end of the
      // full-expression and is destroyed there only when its operand ran, as the i1 at builtFlag
      // says (false from the function's entry, true once the operand ran, false again once destroyed).
      std::string builtFlag;
    };
    struct LabelPlace
    {
      std::vector<uint32_t> ids;
      std::vector<size_t> counts;
      std::vector<uint32_t> inits; // scopeInits at the label
    };
    // A point of a forward goto's way out (leaveForward): reaching it with the scopes from `low` to
    // `high` still open, the jump goes on to the label (`stop` branches there) when the label's depth
    // is among them, else to `next`, which destroys more.
    struct GotoStop
    {
      size_t low, high;
      std::string stop, next;
    };
    struct PendingGoto
    {
      std::vector<GotoStop> stops;
      std::vector<uint32_t> ids;
      std::vector<uint32_t> inits; // scopeInits at the goto
      Node* at = nullptr;
    };
    // The operator expression whose result initializes an object, and that object: its operator
    // function's result is built there (initializeFrom; operatorResult gives it to the call).
    const Node* inPlaceOperator = nullptr;
    std::string inPlaceSlot;
    struct Loop
    {
      std::string breakLabel, continueLabel;
      size_t scopeDepth;
      size_t continueDepth = size_t(-1); // where continue leaves scopes to, when not at scopeDepth
    };
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

    std::string fn;      // current function text (after the allocas)
    std::string allocas; // entry-block allocas of the current function
    // --debug-info: the declaration (a function or a lambda) whose definition is being emitted and its
    // file, or null for a definition without line information (a global's startup function).
    Node* debugAt = nullptr;
    ParsedFile* debugFile = nullptr;
    std::vector<DebugVariable> debugVariables;
    std::vector<DebugBlock> debugBlocks;
    uint32_t debugBlock = 0;
    uint32_t temp = 0;
    uint32_t label = 0;
    bool blockTerminated = false;
    std::string currentBlock = "entry";
    // The body being emitted: the function's, or the lambda's whose call operator is (its static
    // locals are declared in it, staticLocal).
    Node* bodyNow = nullptr;
    Node* noexceptLambdaBody = nullptr; // bodyNow when it is a noexcept lambda's (emittingNoexcept)
    bool noexceptDestructorHelper = false; // emitting the helper of a class whose destructor is noexcept (destructorThunk)
    // Named return elision (design.md 23): the local the function's every return returns, built in
    // the caller's result (sretAddr) instead of a slot of its own; null when there is none.
    VariableDecl* namedReturn = nullptr;
    // While a return of namedReturn leaves its scopes: the result's address, whose object (that
    // local) they do not destroy (destroyOnExit). Empty otherwise.
    std::string keptOnReturn;
    // A C-callable function's struct result that the target's C returns in registers (CPassing's
    // parts, at their offsets): built in a slot of its own (sretAddr), then each `return` loads the
    // parts from it and returns them (emitReturnVoid). Empty otherwise.
    std::vector<std::string> cReturnParts;
    std::vector<uint64_t> cReturnOffsets;
    std::unordered_map<Decl*, Local> locals;
    // One local per element of a parameter pack: the function's own parameters', or in a lambda's
    // call operator the closure's fields that hold the elements of a pack of the function it is
    // written in (the function's locals are other IR names: a lambda that read them printed its
    // own parameters, templates/lambda_uses_parameter_pack_of_its_function).
    std::unordered_map<Decl*, std::vector<Local>> packLocals;
    // In a lambda's call operator, the structured bindings its closure captured, each on its own
    // (CaptureSlot::binding): the field per hidden object and binding name, by copy holding the
    // value, by reference the place (isRef). A use of the binding reads it (bindingPlace), never the
    // hidden object, which the closure does not hold.
    std::map<std::pair<Decl*, uint32_t>, Local> bindingCaptures;
    std::unordered_map<uint32_t, std::string> labelBlocks; // goto labels of the current function -> their blocks
    std::vector<std::vector<Object>> scopes; // Objects to destroy, per open scope.
    // Each open scope's id (pushScope), and the goto labels' places: the scopes' ids and their
    // objects' counts at the label. A goto destroys what it leaves ([stmt.jump] 2): a backward one by
    // its label's place, a forward one through a trampoline emitted at the label (GotoStmt, LabelStmt).
    std::vector<uint32_t> scopeIds;
    // Per scope (beside scopeIds), its automatic variables declared so far whose initialization a
    // jump into their scope would skip (jumpBypassesInitialization).
    std::vector<uint32_t> scopeInits;
    // Per scope (beside scopeIds), the closing brace of the block it is (0: no block of the source):
    // its objects are destroyed at that line (debugScopeEnd).
    std::vector<uint32_t> scopeEnds;
    uint32_t nextScopeId = 0;
    std::unordered_map<uint32_t, LabelPlace> labelPlaces;
    std::unordered_map<uint32_t, std::vector<PendingGoto>> pendingGotos;
    std::vector<Object> statementTemps;       // Temporaries of the current full-expression.
    Node* checkedCall = nullptr;              // The call a check statement tests, and its arguments.
    std::vector<CheckedArgument> checkedArguments;
    // While a variable is initialised with braces (`Holder h{T(1)}`, `= {...}`, `auto h = Holder{...}`):
    // the elements of its brace lists, nested ones included. A temporary one of them makes that a
    // reference member binds lives as long as the variable ([class.temporary] 6), never only to the
    // end of the declaration (bindReferenceMember records it in extendedTemporaries).
    const std::vector<const Expr*>* extendingElements = nullptr;
    std::vector<std::string> extendedTemporaries;
    // A variable of static storage so initialised (a global, a static local): such a temporary is an
    // object of static storage of its own (staticTemporary: this name's ".temp<n>", thread_local for
    // one per thread), since the function initialising the variable returns; destroyed after the
    // variable (staticTemporaries).
    std::string extendingStatic;
    // While an operator converts its operands to their common type (arithmetic, compare): an
    // enumeration's value is a number there, which convert's enumeration rule leaves to the operator.
    bool operandConversion = false;
    bool extendingThreadLocal = false;
    uint32_t extendingCount = 0;
    // The temporaries a static or thread_local variable's initializer extended that need destroying
    // (staticTemporary): destroyed at the program's or the thread's exit after the variable
    // (registerProgramExits, registerThreadExits).
    std::vector<std::pair<std::string, const Ty*>> staticTemporaries;
    std::vector<Loop> loops;
    std::vector<EHFrame> ehFrames;
    std::string ehBlocks;                                     // cleanup pads of the current function
    std::unordered_map<std::string, std::string> cleanupPads; // destruction state -> cleanup block
    bool itaniumCleanup = false;                              // off Windows: a cleanup block is being emitted, for its calls
    bool usesEH = false;                                      // the current function has invokes or pads
    std::string returnLabel; // a destructor's: where a `return` goes, the members and bases destroyed after the body
    std::unordered_map<std::string, std::string> inlineSlots;              // (inlining depth, parameter, type) -> its slot in this function
  };

  struct FunctionState : FunctionContext, FunctionBody
  {
  };
