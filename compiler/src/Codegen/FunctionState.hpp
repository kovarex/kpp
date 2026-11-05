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
  };

  // What emitting a function's body builds up, from nothing for each function.
  struct FunctionBody
  {
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
    // The body being emitted: the function's, or the lambda's whose call operator is (its static
    // locals are declared in it, staticLocal).
    Node* bodyNow = nullptr;
    // Named return elision (design.md 23): the local the function's every return returns, built in
    // the caller's result (sretAddr) instead of a slot of its own; null when there is none.
    VariableDecl* namedReturn = nullptr;
    // While a return of namedReturn leaves its scopes: the result's address, whose object (that
    // local) they do not destroy (destroyOnExit). Empty otherwise.
    std::string keptOnReturn;
    struct Local
    {
      std::string addr; // Address of the variable (or of the pointer slot for references).
      const Ty* ty;
      bool isRef;
    };
    std::unordered_map<Decl*, Local> locals;
    std::unordered_map<uint32_t, std::string> labelBlocks; // goto labels of the current function -> their blocks
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
    bool itaniumCleanup = false;                              // off Windows: a cleanup block is being emitted, for its calls
    bool usesEH = false;                                      // the current function has invokes or pads
    std::string returnLabel; // a destructor's: where a `return` goes, the members and bases destroyed after the body
    std::unordered_map<std::string, std::string> inlineSlots;              // (inlining depth, parameter, type) -> its slot in this function
  };

  struct FunctionState : FunctionContext, FunctionBody
  {
  };
