// struct Emitter's members: function definitions and startup globals: the declarations, part of the
// struct's body, included in it in Codegen/Emitter.hpp; the definitions in FunctionDefinitions.cpp
// (codegen.cpp split, stage 2, 2026-11-05, moved without a change).
    // --- function definitions ---

    // Whether emitting f re-types its nodes in place, once per element of a pack expansion (f or
    // its class has a parameter pack) or of a `for constexpr` (design.md 8.2).
    static bool retypedPerElement(FunctionDecl* f);
    static bool containsForConstexpr(Stmt* s);

    void emitFunction(FunctionDecl* original, const TemplateArgs& args, const Ty* classTy, const std::string& nameOverride = "");

    // A static_assert the checker left to the layouts (`static_assert(sizeof(Pair) == 16)`): its
    // condition under the instance's arguments, with sizes and alignments as laid out. A false one is
    // the checker's error ("static assertion failed"); a layout that fails reports itself.
    void checkLayoutAssert(const GlobalScope::LayoutStaticAssert& a);

    // Out-of-line member definitions (`void Text::split(...) {}`, `Flow::~Flow() = default;`, those
    // of class templates too) take `static`, `virtual` and `= default` from their declarations, and
    // the declarations `= default` from them, before any function is emitted: emitters on several
    // threads read these flags.
    void settleDefinitionFlags(Span<Decl*> decls);

    // The program's globals that C++ builds at startup whether anything reads them or not: a static
    // member whose initializer registers something (`bool FontBank::loaderRegistered =
    // PrototypeLoader::registerPlainLoader(...)`, read by nobody: the game's prototype loaders were
    // never registered). Variables of namespaces and static data members of classes that are no
    // templates, with an initializer that is no literal, or of a class type without one; for an
    // out-of-line definition, the declaration in the class (the one uses name).
    void collectStartupGlobals(Span<Decl*> decls, std::vector<VariableDecl*>& out);

    void considerStartupGlobal(VariableDecl* v, std::vector<VariableDecl*>& out);

    // The static data members of class instance cls that an explicit instantiation builds at
    // startup (`template struct Registry<int>;`, as C++ instantiates every member): those with an
    // initializer that is no literal, or of a class type that needs constructing.
    void collectInstanceStartupGlobals(const Ty* cls, std::vector<std::pair<VariableDecl*, const Ty*>>& out);

    // A global collectStartupGlobals found (or a static data member of the class instance classTy
    // collectInstanceStartupGlobals did): defined, and its startup function registered, as if a
    // function had read it. Its errors are its own (globalFor reports them).
    std::optional<Shared::Startup> startupAt; // where the startup function of the global being defined runs, when not where it is defined
    void emitStartupGlobal(VariableDecl* v, const Ty* classTy = nullptr, std::optional<Shared::Startup> at = std::nullopt);

    void collectFunctions(Span<Decl*> decls, std::vector<FunctionDecl*>& out);

    // Whether the build emits f as one of the program's own functions (collectFunctions reaches it):
    // a definition outside templates and outside function bodies (a local class's functions are
    // emitted where they are used).
    bool isProgramFunction(FunctionDecl* f);
