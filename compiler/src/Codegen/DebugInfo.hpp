// struct Emitter's members: debug information (--debug-info): metadata ids, files, types,
// variables, the classes' composites and the line tables: the declarations, part of the struct's
// body, included in it in Codegen/Emitter.hpp; the definitions in DebugInfo.cpp (codegen.cpp split,
// stage 2, 2026-11-05, moved without a change).
    // --debug-info: a metadata id in a function's text. Each function numbers its own from 0 (its
    // DISubprogram), always ten digits wide, and module assembly (renumberDebugIds) makes them the
    // module's, in place: the text does not depend on where the function lands.
    static std::string debugId(uint32_t local);

    std::unordered_map<const ParsedFile*, std::string> debugFiles; // a file's DIFile, its absolute path
    std::unordered_map<const ParsedFile*, std::string> programPaths; // a file's path as the program's text holds it

    // The path an assertion's message and a CallerLocation give for a file: --file-prefix-map's
    // (programPath), the debug info keeping the path itself (debugFileOf).
    const std::string& programPathOf(const ParsedFile* file);

    const std::string& debugFileOf(const ParsedFile* file);

    // A metadata string's text: `\` and `"` and control characters as \XX.
    static std::string metadataString(std::string_view s);

    // --debug-info: the placeholder that names a type's metadata in a definition's text (`!T` and the
    // key's 16 hex digits, as wide as module assembly's id and its padding: appendDebugTypes).
    static std::string debugTypePlaceholder(uint64_t key);

    // --debug-info with variables: the placeholder of type t's metadata, made the first time any
    // emitter names the type (Shared::debugTypes). Its key is its IR spelling's hash (irString tells
    // every type apart: a class's path, its template's arguments, where a local class is declared).
    std::string debugType(const Ty* t);

    // A type's name in the debug information (and so in the .natvis, which takes the classes' from
    // CodegenResult::debugClasses): kppc's (TypeTable::toString, qualified) with closing angle
    // brackets kept apart, `RbNode<Pair<String, int32> >`, as MSVC's PDBs write them. Visual Studio's
    // expression evaluator reads `>>` as a shift, so it could not name, cast to or match a natvis
    // rule to a type whose name ends two template argument lists at once.
    static std::string debuggerTypeName(std::string name);

    // A class's name there: debuggerTypeName of its qualified name, or `Item^` for the library's
    // UniquePtr<Item> with its default deleter (design.md 20), as the program writes it and errors
    // print it (TypeTable::toString's unqualified form). Visual Studio's Watch and Locals show it as
    // the type. A class declared in a class template has each class's arguments after it,
    // `IntrusiveListImplementation<T, Tag, true, false>::List`, as C++ names it.
    std::string debuggerClassName(const Ty* t) const;

    // Type t's metadata: node 0 the type, the nodes it needs after it, other types by placeholder.
    // Basic types as CodeView names them (its simple types have no names of their own: int32 is
    // `int`), each of kpp's integers a typedef of its name too (int32, uint8: an S_UDT, so that a
    // natvis rule's $T1 parses). A class or enum is named as kppc's IR names it (game::Entity,
    // Vector<int32>), the whole spelling, its closing brackets apart (debuggerTypeName), and
    // identified by it.
    std::string debugTypeText(const Ty* t, const std::string& spelling);

    // --debug-info with variables (Shared::fullDebugInfo): a local or parameter (arg: its number from
    // 1) held at address (its alloca, or the pointer a parameter passed by address is), whose value is
    // of type t (a reference's own: its slot holds the pointer), in the current block. Not one of a
    // body inlined into its caller (its instructions are the call's).
    void debugVariable(std::string name, Node* at, const std::string& address, const Ty* t, uint32_t arg = 0, bool artificial = false, bool defined = false,
                       bool indirect = false);

    // --debug-info with variables: the attachment of global `name`'s definition line (`, !dbg ` and a
    // placeholder, `!G` and the hash of its IR name), its metadata made here into Shared::debugGlobals:
    // a DIGlobalVariableExpression of a DIGlobalVariable named as the IR names it (game::counter,
    // Vector<int32>::empty: a static member's storage is Class::member, where the debugger reads its
    // value; its class's closing brackets apart, debuggerTypeName), at its definition's line, of
    // type t. Module assembly gives it to the module whose text defines the global
    // (appendDebugTypes), its home in a debug build; a declaration has none
    // (externalDeclarationOf).
    std::string debugGlobal(const std::string& name, VariableDecl* def, const Ty* t);

    // A variable's or parameter's name for the debugger, or "" for one without (a structured binding's).
    std::string debugName(Decl* d);

    // Class ct's layout as codegen made it, or null for one never laid out (a class the program only
    // points to): the debugger gets its declaration alone.
    const StructInfo* laidOutClass(const Ty* ct);

    // --debug-info with variables: the composite node `self` of class ct as laid out (info), its
    // members' nodes appended to nodes with ids from next on: its vtable pointer (a class with its
    // own, at offset 0), its bases, its data members at their offsets (a bit field at its bits, an
    // anonymous struct's or union's in an unnamed member of that class's own composite, made here:
    // LLVM's CodeView writer lists them as the enclosing class's, as MSVC's PDBs do). Named (its
    // spelling, which identifies it) or, for an anonymous member's class, not.
    void debugComposite(const Ty* ct, const StructInfo& info, uint32_t self, const std::string* spelling, std::string& nodes, uint32_t& next, int depth = 0);
    // A static constexpr data member's value as its member node's extraData (`, extraData: i8 16`;
    // CodeView's S_CONSTANT Class::NAME, which a watch and a natvis rule read: `flags & FILTER_WHITELIST`),
    // for an integer, character or enumeration whose initializer the evaluator folds without running
    // a function; empty otherwise.
    std::string staticConstant(VariableDecl* v, const Ty* t, const TemplateArgs& args);

    // --debug-info with variables, once the emission is done (prepareModuleTexts; a build of one
    // module before it assembles its text): every class a definition or a type named (each one
    // Shared::debugTypes has: a debug build's program pieces are assembled, and their texts freed,
    // before this), and the classes their definitions name, each defined (debugComposite) for its
    // home module alone (Shared::debugHomes, the key's remainder by the module count: where it lands
    // does not depend on who named it first); every other module declares it. A compile server's
    // build makes the definitions of the classes it named itself (their types are its own) and keeps
    // the others'.
    void homeDebugClasses();

    // The definition with line information: every instruction gets the line of the statement whose
    // marker (`  ;@LINE`, from statement()) precedes it, those before the first the declaration's
    // line; then the function's DISubprogram and DILocations (one per line). A switch's case lines
    // belong to the instruction they end with (`  ]`). With variables (Shared::fullDebugInfo) a marker
    // names its lexical block too (`  ;@LINE:BLOCK`, a location per line and block), and after the
    // allocas a #dbg_declare gives each variable its address. Ids: 0 the DISubprogram; with blocks or
    // variables 1 the file's node, then the blocks (DILexicalBlock), then the variables
    // (DILocalVariable, their types by placeholder); then the locations.
    std::string debugLines(std::string header, size_t declLine);

