// struct Emitter's members: local variables and statements: the declarations, part of the struct's
// body, included in it in Codegen/Emitter.hpp; the definitions in Statements.cpp (codegen.cpp
// split, stage 2, 2026-11-05, moved without a change).
    // A static local: `static T x` or `thread_local T x` in a function body (staticLocal).
    static bool isStaticLocal(const VariableDecl* v);

    // A parameter, or a variable of a function body that lives in its frame (not static, extern or
    // thread_local), which only its function's own code reaches.
    static bool isAutomaticLocal(const Decl* d);

    // The error for a parameter or automatic local of another function than the one emitted (a
    // member function of a local class, or a lambda, naming one it cannot reach): `value, a local of
    // counted, is used outside its function...`; notConstant when it is const, but its value no
    // constant (a constant of it is constant data, which every function reads alike).
    [[noreturn]] void refuseOutsideItsFunction(Decl* d, Node* at, bool notConstant = false);

    // What the bodies nested in a body name (its lambdas', its local classes' member functions':
    // code that other emissions than the body's may write), and the variables declared in the body
    // itself, outside those.
    struct NestedNames
    {
      std::unordered_set<const Decl*> named;
      std::unordered_set<const Decl*> direct;
    };
    std::unordered_map<const Node*, NestedNames> nestedNamesByBody; // (emitFunction clears it: nodes go between builds)
    const NestedNames& nestedNamesOf(Node* body);

    // The function a static local is written in, when the variable is named after that function alone:
    // a function no template makes or holds, the variable declared in its own body (not in a lambda's,
    // whose operator names it). Null for any other. A copy of such a function is the function (the
    // checker types an `auto` one's body in a copy, whose lambdas its callers get: `auto g = make();`).
    FunctionDecl* plainWriter(VariableDecl* v);

    // A local's type: its declaration's, or for `auto` its initializer's.
    const Ty* localType(VariableDecl* v);

    // A static local's storage, named after the function it belongs to (an IR name, `@"..."`) and
    // numbered in that function (VariableDecl::number): the same name in every run, wherever the function is written.
    std::string staticLocalName(const std::string& function, VariableDecl* v);

    // The definition of a static local that code other emissions write names too (staticLocal's
    // shared): linkonce_odr in a comdat of its own, every module naming it carrying this text, and the
    // link keeping one object.
    std::string sharedStaticText(const std::string& name, VariableDecl* v, const Ty* t, uint64_t alignment);

    // A shared static local's definition in this module, belonging to the function being emitted (its
    // own static, or a copy of another's it names): once per function and module, and a build of one
    // module has it once.
    void addSharedStatic(const std::string& name, const std::string& text);

    // A static local named by code its function's emission does not write (a lambda's call operator,
    // which a generic lambda's first caller emits; a local class's member function): the storage its
    // function defines shared (staticLocal), with a copy of the definition in this module. Before the
    // function is emitted, a plain function's (plainWriter) has the name and the definition that
    // function will give it; another's is not known yet.
    std::string staticLocalElsewhere(VariableDecl* v, Node* at);

    // A const static local's initializer as a constant of type t, lowered as globalForOnce lowers a
    // global's (a scalar folded, a string literal, an object or array the interpreter computes), or
    // nothing (it is built on the first pass).
    std::optional<std::string> staticConstant(VariableDecl* v, const Ty* t);

    // `static T x = ...;` inside a function: a global of the program (in the module of the function),
    // constructed on the first pass through the declaration, never destroyed. Not thread-safe yet.
    // `thread_local T x;` (static or not): one per thread, the guard too, so each thread builds its
    // own on its first pass (destroyed at no thread's exit yet). One that code in a lambda or a local
    // class of the body names is shared: that code's operator or member function may be emitted
    // elsewhere (a generic lambda's by its first caller, before or after the function, in another
    // module or piece), which names it too (staticLocalElsewhere); the guard stays the function's.
    void staticLocal(VariableDecl* v, const Ty* t);

    // Variable v of class or array type t initialised from init at slot, a temporary that a reference member
    // binds in its brace lists kept as long as v ([class.temporary] 6: `Holder h{T(1)}`, `= {...}`,
    // nested, `= Holder{...}`: extendedElementsOf): a local's destroyed when v's scope ends, after v;
    // one of a variable of static storage (staticName: v's global) an object of static storage of its
    // own (staticTemporary).
    // An array's initialization takes arrayContext (its caller's: a local's `T a[2]{...}` is direct).
    void initializeExtending(const std::string& slot, const Ty* t, VariableDecl* v, Expr* init, const std::string& staticName, bool threadLocal,
                             ConversionContext arrayContext = ConversionContext::Unknown);
    // The elements of init's brace lists whose temporaries a reference member binds and that live as
    // long as the object of class cls init initializes ([class.temporary] 6): its own list's
    // (`Holder h{T(1)}`, `= {...}`), or those of a list naming cls, an aggregate (`Holder h =
    // Holder{T(1)}`, `auto h = Holder{...}`, a reference's `const Holder& r = Holder{T(1)}`: the
    // prvalue initializes the object itself, [dcl.init.general] 17.6.1); a nested list and its
    // elements too (`Holder h{{1, 2}}`: the list a reference member binds), never those of a list
    // inside an element (a call's argument `f(Holder{T(2)})`, whose temporary dies with the
    // statement). Empty for any other initializer.
    std::vector<const Expr*> extendedElementsOf(Expr* init, const Ty* cls, Node* at);
    // While an object is initialised from a list whose elements' temporaries live as long as it
    // (extendedElementsOf): those elements (extendingElements), and how such a temporary of static
    // storage is named (staticName's ".temp<n>", n from count on, thread_local for one per thread:
    // staticTemporary); what was there before back at the end. keepInScope moves the temporaries a
    // local's initializer made that live as long as it from the statement's to the scope's, where
    // they are destroyed after whatever is pushed there next (the object itself).
    struct ExtendingScope
    {
      ExtendingScope(Emitter& e, const std::vector<const Expr*>& elements, const std::string& staticName, bool threadLocal, uint32_t count);
      ~ExtendingScope();
      void keepInScope();

      Emitter& e;
      const std::vector<const Expr*>* outerElements;
      std::vector<std::string> outerExtended;
      std::string outerStatic;
      bool outerThreadLocal;
      uint32_t outerCount;
    };
    // An object of static storage for a temporary that a reference of static storage keeps
    // (extendingStatic's name and a number), zero-initialised, never destroyed.
    std::string staticTemporary(const Ty* type, Node* at);
    // The address a reference of static storage (a global, a static member, a static local; name
    // its global) is bound to, the initialization emitted, as a local's reference binds: a base's
    // part, a conversion operator's lvalue, or a temporary of static storage of its own, which
    // lives as long as the reference ([class.temporary] 6) and is never destroyed.
    std::string staticReferenceTarget(VariableDecl* v, const std::string& name, const Ty* t, Expr* init, Node* at, bool perThread);
    // The one value a reference's `(expr)` or `{expr}` initializer binds, as `= expr` does; refTy is
    // the reference's type (a reference to an array keeps `{9}`, its array's first element).
    Expr* referenceInitializer(VariableDecl* v, Expr* init, const Ty* refTy);
    // The temporary of class type a reference bound to a subobject of it keeps alive
    // ([class.temporary] 6): what `make().inner`, `make().items[1]` and
    // `static_cast<const T&>(make().t)` designate a part of, through `.`, a built-in subscript of an
    // array and a cast to a reference; null for an initializer that is no such part.
    Expr* temporaryOfSubobject(Expr* init) const;

    // `auto [a, b] = e;` binding tuple-like: after the object, the hidden variables `auto&& a =
    // get<0>(e)` and so on that the bindings' names name (the resolver's tupleBindings).
    void declareTupleBindings(VariableDecl* v);
    // --debug-info: a structured binding of a class's member or an array's element, which is no
    // local of its own (its uses read the hidden object's member): a variable at that member's
    // offset in the hidden object, as clang describes one. Not a bit-field's or a reference member's.
    void debugAggregateBindings(VariableDecl* v);

    void declareLocal(VariableDecl* v);

    // --debug-info: the instructions from here on are at the line `at` starts on (debugLines makes
    // the marker their location): a statement's, a loop's step or condition emitted after its body.
    // Not in a body inlined into its caller: those instructions are the call's. With variables
    // (Shared::fullDebugInfo) the marker names the lexical block too, `  ;@LINE:BLOCK`.
    void debugLine(Node* at);

    // The same at the line of a token, a block's closing brace.
    void debugLineAt(uint32_t token);

    // --debug-info with variables: a block statement's lexical block (not the function's body, whose
    // variables are the function's own, as clang's), which the variables declared in it belong to; the
    // previous block again when it ends (debugLine names it at the next statement).
    uint32_t openDebugBlock(Node* at);

    void statement(Stmt* s);
    // A run-time if and the plain ifs of its `else if` chain, without a recursion per link.
    void ifChain(IfStmt* first);
    // The branch an if constexpr takes, when its condition is decided (the resolver's, or evaluated here); none
    // for a run-time if, or a condition that is no constant expression.
    std::optional<bool> constexprTaken(IfStmt* i);
    // The end of a loop's body, back to condLabel. A variable its condition declared (declares)
    // ends here each time; one with a destructor also at falseLabel, the way out when the condition
    // is false, which goes on to endLabel. The scope of such a variable closes here.
    void endIteration(bool declares, size_t outside, const std::string& condLabel, const std::string& falseLabel, const std::string& endLabel);

    // Finds a zero-argument member function by name (searching bases), with the class it lives in,
    // for an object of classTy (a const one: its const overload).
    FunctionDecl* findMember(const Ty* classTy, const std::string& name, Node* at, const Ty** foundIn);

    // A range whose begin()/end() return iterator objects: `it != end`, `*it` and `++it` go through
    // the iterator class's operators. Both iterators live in the loop's scope (already open).
    void iteratorLoop(RangeForStmt* f, Value begin, Value end);

