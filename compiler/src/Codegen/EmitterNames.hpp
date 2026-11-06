// struct Emitter's members: names and IR types: the declarations, part of the struct's body,
// included in it in Codegen/Emitter.hpp; the definitions in EmitterNames.cpp (codegen.cpp split,
// stage 2, 2026-11-05, moved without a change).
    // --- names and types ---

    // GlobalScope::qualifiedName, computed once per declaration.
    const std::string& qualifiedName(Decl* d);

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
    // The class whose implicit construction is being emitted where its object is made (not in a
    // constructor of its own): the constructors it calls are its to call (--access-warnings).
    Decl* implicitConstructionOf = nullptr;
    // The closure of the lambda whose body is being emitted, or null: names written in the body
    // are typed from it (the lambda's template parameters, the aliases its body declares), not
    // from the function that happens to emit it (a generic lambda's is the one calling it).
    Decl* lambdaContext = nullptr;
    Decl* typingContext() const { return initializerOf ? initializerOf : lambdaContext ? lambdaContext : static_cast<Decl*>(currentFunction); }

    // A class declared in a function body, or a member of one: the main pass does not reach its
    // member functions (they are emitted when used), and its name alone is not unique.
    static bool declaredInFunction(const Decl* d);
    std::string newTemp() { return "%t" + std::to_string(temp++); }
    std::string newLabel(const char* hint) { return std::string(hint) + std::to_string(label++); }

    static bool isClass(const Ty* t) { return t && t->kind == TyKind::Class; }

    const Ty* declType(Decl* d) { return currentArgs ? global.typeOfDeclIn(d, *currentArgs) : global.typeOfDecl(d); }

    const Ty* classKey(const Ty* t) { return types.unqualified(types.stripRef(t)); }
    // The IR type of a pointer to a data member, the member's offset in its class: an i32 in MSVC's
    // ABI (4 bytes, as MSVC and clang for Windows have it), an i64 in Itanium's (the resolver's
    // constSizeOf says the same: GlobalScope::setMsvcAbi). Null is -1 in both.
    std::string memberOffsetType() const { return sh.target.msvc() ? "i32" : "i64"; }

    std::string llType(const Ty* t, Node* at = nullptr);

    const Ty* enumUnderlying(Decl* d);

    // Zeroes an object of type t at addr (value initialization): a struct or an array with a memset of
    // its size, anything else with a store. The instruction selector splits `store [1048576 x i8]
    // zeroinitializer` into a node per element and crashed on a member `char buffer[1 << 20]`
    // (BufferedReadStream); the size is LLVM's own (`getelementptr (T, ptr null, i32 1)`).
    void storeZero(const Ty* t, const std::string& addr, Node* at);

    // Copies the bytes of an object of type t from src to dst: a struct or an array with a memcpy of
    // its size, anything else with a load and a store. A struct loaded as one value crashed the
    // instruction selector as storeZero's did (minizip's `*zi = ziinit;`, a struct holding a 64 KB
    // buffer). src and dst are the same object or apart (`*p = *p`), as memcpy allows.
    void copyBytes(const std::string& dst, const std::string& src, const Ty* t, Node* at);

