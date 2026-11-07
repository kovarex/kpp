// struct Emitter's members: lambdas and their closures: the declarations, part of the struct's
// body, included in it in Codegen/Emitter.hpp; the definitions in Lambdas.cpp (codegen.cpp split,
// stage 2, 2026-11-05, moved without a change).
    // --- lambdas ---

    static bool localNamed(Decl* d, uint32_t symbol);

    // A variable of a function (a parameter, a local of automatic storage): what a lambda captures.
    static bool functionLocal(const Decl* d);

    // What a lambda's body names, from its AST alone: the locals of the functions around it (each
    // once, in the order first written: `used`) and whether it uses `this`. inner gathers what is
    // declared inside it (its parameters, its locals, nested lambdas' own), which is no capture;
    // C++ declares a name before its uses, so it is in inner before a use is reached.
    // useTypes, when given, gets the type of each used local's first name expression.
    // keptBranches: an `if constexpr` its instance decided names only what the kept branch names
    // (not in a generic lambda, whose one closure serves every call's instance of the body).
    // bindingUses, when given, gets each structured binding the body names (its hidden object, its
    // name and the type of its first use), once, in the order first written. (The checker's
    // GlobalScope::lambdaNames, which its view of what a closure holds is made from too.)
    using BindingUse = GlobalScope::BindingUse;
    void captureNames(Node* n, std::unordered_set<Decl*>& inner, std::vector<Decl*>& used, bool& usesThis, std::unordered_map<Decl*, const Ty*>* useTypes = nullptr, int depth = 0, bool keptBranches = false,
                      std::vector<BindingUse>* bindingUses = nullptr);

    // A lambda's captures from its AST alone, in a fixed order: its capture list, then what a
    // default capture picks up in the order first written, then `this`. The same list wherever the
    // closure is laid out or built, whichever function is emitted first (the body names the
    // instantiation's own locals: a capture's position, not its declaration, matches them up).
    // A parameter pack of the function around the lambda is captured element by element, a slot
    // (a field of the closure) each, in order.
    // A structured binding is captured on its own, as C++20 captures it ([x, &y] of `auto [x, y] =
    // p`: x's value, y's place), its slot holding that binding alone: decl its hidden object, binding
    // its name.
    struct CaptureSlot
    {
      Decl* decl = nullptr; // the captured variable (null for `this`)
      bool byRef = false;
      Expr* init = nullptr; // an init-capture's initializer
      int element = -1;     // decl a parameter pack: the element this slot holds
      const Ty* elementType = nullptr; // and that element's type (references stripped)
      uint32_t binding = UINT32_MAX;   // decl a structured binding's hidden object: the binding's name
      const Ty* bindingType = nullptr; // and the binding's type, as the body names it
    };
    std::vector<CaptureSlot> captureList(LambdaExpr* l, std::unordered_map<Decl*, const Ty*>* useTypes = nullptr);

    // Whether a generic lambda's call operator is retyped per element while it is emitted (from a
    // private copy, emitLambda): it, the function it is written in or a class around has a template
    // parameter pack, or its body has a `for constexpr`.
    bool genericLambdaRetypedPerElement(LambdaExpr* l);

    // Whether d is the hidden object of a structured binding declaration that names symbol.
    static bool bindsName(Decl* d, uint32_t symbol);

    // The address of what structured binding `name` of hidden object v names in the function being
    // emitted: the closure's field where a lambda captured it (bindingCaptures, the field's address by
    // copy, the place it holds by reference), else the part of v's object; bits gets a bit-field's.
    std::string bindingPlace(VariableDecl* v, uint32_t name, Node* at, const BitfieldInfo** bits = nullptr);

    // An explicit capture the body does not use (`[guard]` kept alive): the one local of that name
    // declared before the lambda in the function around it; with several, which one is in scope
    // the code generator cannot tell from the names alone.
    Decl* unusedCapture(LambdaExpr* l, uint32_t symbol);

    // The type a captured variable has in the closure (references stripped): the variable's declared
    // type where the declaration alone gives it, an init-capture's as its initializer deduces it,
    // else as the lambda's body first names it (useTypes: an instantiation's local of a dependent
    // type, typed in place).
    static bool mentionsAuto(const Ty* t, int depth = 0);

    const Ty* captureType(Decl* d, Expr* init, const std::unordered_map<Decl*, const Ty*>& useTypes, LambdaExpr* l);

    // The closure of a lambda type, created from its capture list in the current function.
    // The IR name of a lambda's closure struct: known as soon as the type is mentioned (a slot
    // for the variable holding it is allocated before its expression is reached); the struct is
    // defined once the captures are known.
    std::unordered_map<const Ty*, std::string> closureNames; // this emitter's view of the names below
    std::string closureStructName(const Ty* t);

    // The closure of a lambda type with its captures laid out, from the lambda's AST (captureList),
    // by whichever function needs it first: the function the lambda is written in, or one that
    // holds or calls a closure it returned (an instance of a template that returns a lambda is
    // emitted after its caller). The layout is the same either way.
    std::unordered_map<const Ty*, ClosureInfo*> closureCache; // laid-out closures this emitter used
    ClosureInfo& closureFor(const Ty* t, Node* at, bool = false);

    // closureFor's layout, from the lambda expression being emitted (at) or the closure's own.
    ClosureInfo& layOutClosure(const Ty* t, Node* at);

    // Lays out the closure of every lambda written in n (not in a lambda's body), before n is
    // emitted: a slot for a class holding one (`auto h = hold([base](int x) { ... })` and its
    // Holder<closure>, a call's result) is laid out before the expression that builds the closure
    // is reached. The captures are the locals in scope where the statement starts.
    void layOutClosuresIn(Node* n);

    // A closure's captures by value, as (field index, type): what copying, moving and destroying
    // the closure copy, move and destroy one by one (a capture by reference, and `this`, is a pointer).
    std::vector<std::pair<uint32_t, const Ty*>> closureValueFields(const Ty* t, Node* at);

    // Copies (move: moves) the closure at src into dst capture by capture, each through its own copy
    // or move constructor: EntityWithOwner's `[entity = Targeter<EntityWithOwner>(this)]` copied
    // bitwise into a load helper left the copy out of the entity's targeters and the stack's closure
    // in them, never destroyed.
    void copyClosure(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move);

    // Initialises the closure at addr from init, a closure value: a lambda expression is built in
    // place, a temporary moved from (and destroyed with the statement), anything else copied (moved
    // for `move(f)`).
    void initializeClosure(const std::string& addr, const Ty* t, Expr* init, Node* at);

    // Fails when given, a closure, is another lambda's than closure (the type a closure variable, a
    // parameter or a reference wants): C++ has no conversion between two lambdas' closures, the
    // checker lets one through, and taking one's bytes for the other's ran the wrong lambda's body.
    // reference: closure is what a reference binds.
    void requireSameClosure(const Ty* given, const Ty* closure, bool reference, Node* at);

    // Builds the closure object of a lambda expression in a fresh slot (into: in that object) and
    // emits its function. One in a fresh slot is a temporary: the statement destroys it unless it
    // is moved out (a parameter by value, a variable), when its captures need destruction.
    Value makeClosure(LambdaExpr* l, const std::string& into = "");

    // The current function's state, all of FunctionState, copied out before a lambda's call operator
    // is emitted in the middle of it and back after.
    FunctionState saveState() { return static_cast<const FunctionState&>(*this); }
    void restoreState(FunctionState& s) { static_cast<FunctionState&>(*this) = s; }
    // A new function's state: FunctionBody's members as they start (FunctionContext's, the function,
    // its result and its class, are the caller's to set).
    void resetFunctionState();

    // A function's definition text: the personality and the cleanup pads when it uses exceptions.
    std::string functionText(std::string header);

    // Emits the lambda's function while another function is being emitted: the enclosing
    // function's state is saved and restored around it. callTy: the operator's type (parameters
    // and result); name: its IR name; args: for an instantiation of a generic lambda, the
    // enclosing instantiation's arguments plus its own, which declarations in the body are typed under.
    // The function a lambda is written in, as far as it is known yet (Shared::closureWriters).
    ClosureWriter closureWriterOf(const Ty* t);

    void emitLambda(LambdaExpr* l, const Ty* t, const Ty* callTy, const std::string& name, const TemplateArgs* args);

    // The plain function lambda l is written in: none for a lambda in a template (a function template,
    // a member of a class template), in a member of a local class (inside a function that may be one),
    // or outside any function (a default member initializer's, a default argument's).
    FunctionDecl* lambdaWriter(LambdaExpr* l);

    // Where lambda l's call operator goes (named name; not a generic lambda's instance, which is one by
    // name): beside its plain writer, in its file's module, so that a lambda stays with the function it
    // is written in; else (a template's lambda; a default member initializer's, written by each
    // constructor; a default argument's, by each call site) the module its name hashes to. Its
    // closure's first writer and a caller emitting it on demand both take it from the lambda alone, so
    // whichever is first, it is in one module.
    unsigned lambdaHome(LambdaExpr* l, const std::string& name);

    // The call operator of a (non-generic) lambda a call needs, emitted from the lambda's AST by
    // whoever needs it first when its writer has not (a lambda a function returns, whose caller may
    // be emitted before it; a private copy's lambda type the writer never makes: the callers of
    // RenderParameters::getScreenPositionFunction). Only for a lambda written outside templates:
    // an instance's arguments, which its body may need, are known to its writer alone.
    void emitCallOperatorOnDemand(const Ty* t, const Ty* callTy, const std::string& name);

    // A lambda without captures used as a pointer to function: a function with the pointer's
    // signature that calls the lambda's operator with no closure (nothing is captured), defined
    // once per lambda.
    std::string lambdaThunk(const Ty* t, const Ty* fnTy, Node* at);

    // Calls a closure value with arguments.
    // The call operator of generic lambda t for its own arguments, one per set of them: named by
    // those arguments and the parameter types they give (a template parameter no parameter
    // mentions tells instances apart only by its argument: `[]<class T>(const char* name)`, called
    // `runTest.operator()<CraftingMachine>(name)` and `runTest.operator()<RocketSilo>(name)`).
    std::string genericOperatorName(const Ty* t, const TemplateArgs& own, const Ty* callTy);

    Value callClosure(Value closure, Span<Expr*> callArgs, const std::string& sret, Node* at);

