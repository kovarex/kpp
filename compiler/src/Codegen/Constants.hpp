// struct Emitter's members: constants: the declarations, part of the struct's body, included in it
// in Codegen/Emitter.hpp; the definitions in Constants.cpp (codegen.cpp split, stage 2, 2026-11-05,
// moved without a change).
    // --- constants ---

    std::string_view spelling(uint32_t token) const;

    // A literal's own file (the parser's): a default argument, a default member initializer or an
    // instance's body is emitted while another file is the current one.
    ParsedFile* literalFile(const LiteralExpr* l) const { return l->file && l->file->tokens ? l->file : currentFile; }
    // (its line splices taken out: in buffer, for a literal that has one)
    std::string_view literalSpelling(const LiteralExpr* l, uint32_t token, std::string& buffer) const;

    // `(Name)` where Name is a type: a C-style cast the parser could not tell from a parenthesis.
    bool isParenthesizedTypeName(Expr* e);

    // The type of a constant template parameter's values (`int N`), int when not written.
    const Ty* valueParamType(Decl* param);

    // The arguments of a variable template's use (`canBuild<String, const char*>`): the written
    // ones, read where the use is written (a function, or another variable template's
    // initializer), a trailing pack taking the rest; then the defaults, read in the template.
    TemplateArgs variableTemplateArgs(NameExpr* n, VariableDecl* v, Node* at);

    // The code generator's constants (design.md, "Compile-time evaluation"): the checker's
    // evaluator (GlobalScope::evaluate) with this code generator's hooks: signed arithmetic wraps,
    // as the emitted code does, an enumeration is the integer type it is stored as here, and
    // sizeof and alignof are these layouts.
    ConstArith foldArith();

    // layoutFailed: set when a layout the evaluation needs fails (it has reported itself).
    ConstHooks foldHooks(Node* at, bool& layoutFailed);

    // The byte offset of data member m in an object of class t as laid out (offsetof), m declared
    // in t or in one of its bases; nullopt when it is in neither.
    std::optional<uint64_t> memberOffset(const Ty* t, Decl* m, Node* at, int depth = 0);

    std::optional<int64_t> evalConst(Expr* e, int = 0);

    // A condition folded at compile time (if constexpr, static_assert): any scalar constant, a
    // floating one included.
    std::optional<bool> evalCondition(Expr* e);

    // A folded value given type t (a declaration's): converted when t is an arithmetic or
    // enumeration type; kept for any other type, and for one not known here.
    std::optional<ConstValue> foldAs(std::optional<ConstValue> v, const Ty* t);

    // A constant expression written in the declaration being folded (constContext) or the function
    // being emitted, read from currentFile, under the current arguments. A number or character
    // literal, the commonest, needs no context.
    std::optional<ConstValue> foldConst(Expr* e);

    int64_t enumeratorValue(EnumeratorDecl* en);

    // A string literal's bytes with its escapes decoded, UTF-8 as the source is: a universal character
    // name (`é`, `\U0001F600`) is its code point's UTF-8; an octal escape (up to three digits,
    // `\101`) or a hex one is one byte in a narrow literal and one code unit in a wide one (its UTF-8
    // here, which wideStringConstant decodes back). `L"T"` was the five units u0054 and `"\101"`
    // the three characters 101.
    static std::string decodeString(std::string_view s, bool wide = false);

    std::string stringConstant(LiteralExpr* l);

    // The code units of a string literal (adjacent ones joined) without its terminating zero, as
    // stringConstant makes them, and their size: chars, or UTF-16/32 units for L"..", u"..", U"..".
    size_t stringLiteralUnits(LiteralExpr* l, uint32_t& unitBytes);

    // `L"text"` / `U"text"`: the UTF-8 bytes decoded and re-encoded as UTF-16 or UTF-32 code units.
    std::string wideStringConstant(const std::string& utf8, int bits);

    // A string literal is one object in the whole program, whichever module names it (as C++'s
    // literal pooling makes it, and as a constexpr pointer to one must be: a module that folds the
    // pointer's value and the module whose definition of the variable the linker keeps name the
    // same object). Its name comes from its contents, the same in every run whatever order the
    // functions are emitted in; isNew when the caller has to define it in the output being written
    // (the shared declarations, which every module carries, or the current module, unless the
    // shared ones define it: a module's own definition of one the shared declarations define later
    // is dropped when its declarations are finished, dropSharedLiterals).
    std::unordered_map<std::string, std::string> constantCache; // where + prefix + contents -> the name this emitter got
    std::pair<std::string, bool> constantName(const char* prefix, const std::string& contents);

    // A string literal's definition: one object in the program (the linker keeps one of the
    // modules' copies).
    static std::string literalDefinition(const std::string& name, const std::string& contents);

    // The IR name of a class's or closure's struct type: a hash of the type as printed, so that it
    // is the same in every run whatever order the types are laid out in (a short name: it is
    // written in every access), a suffix telling apart two types that print alike.
    std::string irTypeName(const char* prefix, const Ty* t);

    // A NUL-terminated string constant; returns its global name.
    std::string cStringConstant(std::string bytes);

    // The vtables of a class are defined in every module that stores them (linkonce_odr), with the
    // descriptors they point at.
    void useVtable(const StructInfo& info);

    // The name a class descriptor (slot -1 of a vtable) starts with.
    std::string descriptorName(const std::string& descriptor);

    // dynamic_cast<T*>(p) / dynamic_cast<T&>(x) from a polymorphic class: the whole object (found
    // with the offset in slot -2 of the vtable p's pointer leads to) has its class's descriptor in
    // slot -1, which is searched, bases and their bases, for T's: the result is the whole object
    // moved by the offset T's subobject has in it (a downcast, or a cross-cast to another base), or
    // null for a pointer when T is none of them. Upcasts and non-polymorphic operands fall back to
    // the static conversion (nullopt).
    std::optional<Value> dynamicCast(NamedCastExpr* c, Expr* e);

    // The offset of the class whose descriptor is %target within one whose descriptor is %d (both
    // { name, n, [n x { base descriptor, offset }] }), searching the bases and theirs in order, or -1.
    static std::string baseSearchFunction(const std::string& name);

    // A floating-point constant in IR: the exact bits, rounded through float for 32-bit values.
    static std::string floatConstant(double value, uint8_t bits);

    // Line and column (1-based) of a token of the current file.
    std::pair<uint32_t, uint32_t> lineColumnOf(uint32_t token);

    // The source text from one token up to (not including) another.
    std::string sourceText(uint32_t fromToken, uint32_t toToken);

    // Builds a CallerLocation {file, line, column, function} for the call at `at`, in a fresh slot.
    std::string callerLocation(const Ty* pt, Node* at);

    // conditional(expr) on a function: false means the function and every call to it vanish.
    bool conditionHolds(FunctionDecl* f);

    // LLVM function attributes for [[noinline]] and [[forceinline]].
    static std::string attributeText(Decl* d);

    // A finished function definition: into the single body, or into the current module. ownItself:
    // one emitted once at its first use (a global's constructor, a lambda's function-pointer thunk),
    // which belongs to no user; otherwise it belongs to the function being emitted.
    void addDefinition(const std::string& name, const std::string& text, bool ownItself = false, bool counted = false);
    // The function whose emission is running (set by emitFunction at the outermost level): what its
    // emission adds (its definition, its lambdas' call operators) belongs to it.
    std::string owner;
    Instantiation emittingNow{}; // how that function is emitted (what rebuildLLVM emits again)
    std::string emittingName;    // its IR name (the owner while it is emitted)
    std::vector<const FunctionDecl*> inlinedNow; // the bodies its emission inlined so far
    std::vector<const Ty*> layoutsNow;           // the class layouts its emission used so far (noteLayout)
    struct OnDemand
    {
      std::string name; // the call operator emitCallOperatorOnDemand is emitting, or empty
      const Ty* closure = nullptr;
      const Ty* callType = nullptr;
      FunctionDecl* writer = nullptr;
    } onDemandNow;
    // A call emitted as the callee's body (inlinedBody; any other inlining of a body calls this
    // too): `written` is the definition the body is written in, an instance's template's. The
    // function being emitted then depends on that body (Shared::Definition::inlined).
    void noteInlined(const FunctionDecl* written)
    {
      if (moduleCount > 1)
        inlinedNow.push_back(written);
    }

    // A changed layout (rebuildLLVM): every answer this emitter remembers from layouts goes.
    void forgetLayoutAnswers();

    // A class type whose layout (or an answer computed from it) the function being emitted uses
    // (Shared::Definition::layouts).
    void noteLayout(const Ty* classTy);

    // A module declaration that belongs to the function being emitted (Shared::OwnedEntry).
    void addOwnedEntry(const std::string& text);

    // A shared or runtime function's declaration from the header of its definition (declareFromHeader,
    // without its linkonce_odr: a declaration has no linkage), and its name.
    static std::string sharedDeclarationOf(const std::string& header, std::string* name = nullptr);

    // The runtime's functions that are written only once every level is emitted (finishExceptionRuntime,
    // finishAssertRuntime, finishStubRuntime), by the headers of their definitions: each definition
    // is written from its header here, and level 0's barrier declares them all from the same headers
    // (Shared::requestDeclarations), for a program piece that calls one.
    static constexpr const char* kppThrowHeader = "define linkonce_odr void @kpp_throw(ptr %object, ptr %typeinfo, ptr %dtor) noinline comdat {";
    static constexpr const char* kppEhRethrowHeader = "define linkonce_odr void @kpp_eh_rethrow(ptr %out) noinline comdat {";
    static constexpr const char* kppEhPushHeader = "define linkonce_odr void @kpp_eh_push(ptr %record, i32 %handler, i64 %offset) comdat {";
    static constexpr const char* kppEhBeginHeader = "define linkonce_odr void @kpp_eh_begin(ptr %out) comdat {";
    static constexpr const char* kppEhEndHeader = "define linkonce_odr void @kpp_eh_end(ptr %out) comdat {";
    static constexpr const char* kppEhFilterAnyHeader = "define linkonce_odr i32 @kpp.eh.filter.any(ptr %pointers, ptr %frame) comdat {";
    static constexpr const char* kppEhFilterNoexceptHeader = "define linkonce_odr i32 @kpp.eh.filter.noexcept(ptr %pointers, ptr %frame) comdat {";
    // (why: 1 nobody caught it, 2 it left a noexcept function; as off Windows' kpp_eh_terminate)
    static constexpr const char* kppEhUnhandledHeader = "define linkonce_odr void @kpp_eh_unhandled(ptr %object, ptr %typeinfo, i32 %why) noinline comdat {";
    // (off Windows: main.cpp's personality calls kpp_eh_terminate too)
    static constexpr const char* kppEhEnterHeader = "define linkonce_odr void @kpp_eh_enter(ptr %out, ptr %h, i32 %handler, ptr %target) comdat {";
    static constexpr const char* kppEhTerminateHeader = "define linkonce_odr void @kpp_eh_terminate(ptr %h, i32 %why) noinline comdat {";
    static constexpr const char* kppAssertFailedHeader =
      "define linkonce_odr void @kpp_assert_failed(ptr %file, i32 %line, i32 %column, ptr %expression) noinline comdat {";
    static constexpr const char* kppMissingHeader = "define linkonce_odr void @kpp_missing(ptr %what) noinline comdat {";
    static std::string baseSearchHeader(const std::string& name) { return "define linkonce_odr i64 " + name + "(ptr %d, ptr %target) comdat {"; }
    // The runtime's global that function bodies read (a `throw;` outside a handler), written after the
    // levels too: one line, in the shared declarations (homed in a debug build), declared at the barrier.
    static constexpr const char* kppEhCurrentLine = "@kpp.eh.current = linkonce_odr thread_local global ptr null, comdat";
    static std::vector<std::string> runtimeHeaders();

    // "declare RET NAME(TYPES)" from a "define RET NAME(TYPE %name, ...) attrs {" header.
    static std::string declareFromHeader(const std::string& text);

    // --stub-missing: a stub for a function used without a definition (ret and params from its
    // declaration), saying which function and where it is declared.
    std::string stubDefinition(const std::string& name, const std::string& ret, const std::string& params);

    // The runtime of --stub-missing's stubs: what was left out, on stderr, and abort.
    void finishStubRuntime();

    // Module k: the shared declarations, then declares for what it uses from other modules, then its bodies.
    // The shared declarations without those of functions this module defines: a `declare` next to
    // the `define` is a redefinition to LLVM (a call site whose definition was not found declares
    // the function as external).
    // The shared declarations as module k carries them; without the type definitions when types is
    // false (moduleText writes those the module uses). An instance piece's (programNames: what its
    // program piece defines) keep the declarations of those.
    std::string declsFor(unsigned k, bool types = true, const std::unordered_set<std::string_view>* programNames = nullptr);

    // The functions a text has `declare` lines for.
    static void declaredIn(std::string_view text, std::unordered_set<std::string>& out);

    // The length of the LLVM type at the start of text: a bracketed aggregate (`[4 x i32]`, `{ i32,
    // ptr }`, `<{ ... }>`) to its matching close, else one word (`i32`, `ptr`, `%struct.<hash>`).
    static size_t typeLengthIn(std::string_view text);
    // A shared `@name = linkonce_odr [unnamed_addr] [thread_local] global|constant TYPE INIT, comdat
    // [, align N]` line as the external declaration of the same global; empty for any other line.
    static std::string externalDeclarationOf(std::string_view line, std::string_view& name);
    // The stable home of a global: FNV-1a of its name.
    static unsigned homeOf(std::string_view name, unsigned modules);

    // A type definition line of the shared declarations (`%struct.<hash> = type {...}`, `%closure...`).
    static bool isTypeLine(std::string_view line) { return !line.empty() && line[0] == '%' && line.find(" = type ") != std::string_view::npos; }

    // The name a line defines or declares (`@name = ...`, `$name = comdat any`, `declare ... @name(`,
    // `define ... @name(`), without its sigil; empty for any other line.
    static std::string_view definedBy(std::string_view line);
    // Each line of a module's declarations; a definition (a thunk's) whole, its body with it.
    template<typename F>
    static void forEachLine(std::string_view text, F&& f)
    {
      for (size_t pos = 0; pos < text.size();)
      {
        bool definition = text.substr(pos).starts_with("define ");
        size_t end = definition ? text.find("\n}", pos) : text.find('\n', pos);
        if (definition && end != std::string_view::npos)
          end += 2;
        if (end == std::string_view::npos)
          end = text.size();
        f(text.substr(pos, end - pos));
        pos = end + 1;
      }
    }
    // A (non-empty) line that defines a name private to its module: an internal or private global or function.
    static bool localLine(std::string_view line);

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
    void prepareModuleTexts();

    // The shared declarations of module k (declsFor) with the homed globals as module k has them:
    // defined where k is their home, declared where k names them (its texts, as moduleText found
    // them in seen; its homed globals' initializers; the lines every module keeps), else left out.
    std::string homedDeclsFor(unsigned k, const std::string& decls, const std::unordered_set<std::string>& seen);

    // A release build's placement (Shared::importableModules). ThinLTO imports short functions into
    // their callers' modules, and its cache keeps a module's back end only while every module it
    // imports from is unchanged: with functions spread by file and name, every module imported from
    // every other, and an edit of one function ran all 64 back ends again (docs/compile-speed.md,
    // "The release edit"). So each short function goes where its callers are: the modules whose
    // long functions call it, directly or through other short functions (what ThinLTO imports it
    // into). Called from one module, it is defined in that module; from several, in one of the
    // importable modules (first in the link); from none (only through a vtable or a pointer), it
    // stays where it was emitted. An edit of a long function (or of a short one called from one
    // module) then changes one module, whose back end alone runs again; of a short one called from
    // several, an importable module, which nearly every module imports from (all of them again).
    // (A linkonce_odr copy in each calling module, as C++ has its inline functions, does not help:
    // ThinLTO imports the copy the link keeps into every module with another.) A definition moves
    // with copies of what it names from its module's declarations (string literals, vtables, type
    // descriptors, thunks: linkonce_odr, one object whichever copy the link keeps; declarations),
    // and with the names only its module has (a static local and its guard, a private function)
    // when every definition naming them moves with it to the same place. Placed again from the
    // definitions as they are before every assembly: a server's build places as a build from
    // scratch does.
    static constexpr size_t shortLines = 300;
    void placeShortDefinitions(unsigned threads);

    // Module k's text. Reads only what is finished (the texts) and prepareModuleTexts' lookups, so
    // the modules are assembled on all threads at once. instancePiece: its instance piece
    // (Shared::splitPieces), without what its program piece holds, which it declares; errors gets a
    // name private to the program piece (a static local's) that it names.
    std::string moduleText(unsigned k, const std::string& prefix, bool instancePiece = false, std::string* errors = nullptr);

    // The default assert handler, unless the program defines `extern "C" kpp_assert_failed`.
    // The C runtime's stderr stream, loaded into %err (the lines that do it; the declaration it needs
    // joins the shared ones): the UCRT's __acrt_iob_func(2), glibc's `stderr`, FreeBSD's `__stderrp`.
    std::string stderrStream();

    void finishAssertRuntime();

