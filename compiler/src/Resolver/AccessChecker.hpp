// GlobalScope::Impl, access checking (design.md, "struct and class"): part of the class's body, included in it in GlobalScopeImpl.hpp,
// its definitions in AccessChecker.cpp (resolver.cpp split, 2026-11-05, moved without a change).
  // ---------------- access checking, warnings only (design.md, "struct and class") ----------------

  bool accessWarnings = false; // kpp --access-warnings: checkAccessIn after every walked function and variable
  bool accessErrors = false;   // design.md "struct and class": a denied use is an error (the default; not a conversion's)
  bool structRuleErrors = false; // kpp --struct-rule: its breaks are errors (the parser's, a struct deriving from a polymorphic class)
  std::mutex accessMutex;
  // A use of a private or protected member that its code may not name, by its warning (`path:line:
  // warning: ...`), each once: a template's instances share the uses of their template.
  struct AccessViolation
  {
    std::string group; // where the use is: tests, stdlib or game
    bool isProtected = false;
    const char* kind = "";
  };
  std::map<std::string, AccessViolation> accessViolations;
  std::set<std::string> accessFriendUses;      // the uses a friend declaration allows, `path:line: note: ...`, each once
  std::unordered_set<Decl*> accessFriendsUsed; // the friend declarations that allowed one

  // The classes whose private members the code of d may name: the class d is a member of and the
  // classes around it, also through the function d is local to (a local class's members, like a
  // lambda's body, belong to their function), each by its scope.
  void accessContext(Decl* d, std::vector<Scope*>& out);

  static bool sameNamePart(const NamePart& a, const NamePart& b);

  // The friend declaration of class owner that lets the code of fn, or of the classes whose scopes
  // context holds, name owner's private members: a friend class one of them is, by name, or a
  // friend function fn is, by name (one qualified by a class, `friend void A::f();`, names a member
  // of a class named A, or a function outside classes; an unqualified one no member). Null if none
  // does.
  Decl* grantingFriend(Decl* owner, FunctionDecl* fn, const std::vector<Scope*>& context);

  std::string namePartText(const NamePart& p) const;

  // A declaration's name for an access warning, with the classes around it (`Surface::Chunk::tiles`).
  std::string accessName(Decl* d) const;

  // Where a use is, for the counts: the game's tests (and UnitTest++), the standard library, or the game.
  static const char* accessGroup(const ParsedFile* file);

  // Whether the code of fn (null: code outside functions) in the classes whose scopes context holds
  // may name used, C++ [class.access]: used is no private or protected member, or the code is in its
  // class (or one around the code), in a class derived from it (protected), or a friend of its class
  // or, for a protected member, of the class naming names it through (`stock->driver` with a
  // RollingStock* names Vehicle's driver as RollingStock's, [class.access.base] 5); self: the class
  // template instance the code is in, whose bases its arguments choose. member is set when the
  // answer took the friends (allowed then by grantedBy, else denied), for a warning's message.
  struct AccessDecision
  {
    bool allowed = true;
    Decl* member = nullptr; // what used is a member as (an enumerator: its enum)
    Decl* owner = nullptr;  // its class
    bool isProtected = false;
    Decl* grantedBy = nullptr; // the friend declaration that allows it
  };
  AccessDecision accessOf(Decl* used, FunctionDecl* fn, const std::vector<Scope*>& context, Scope* naming, const Ty* self);

  // A use of the declaration used at node at, in the code of d (fn: the function it is in, if any)
  // whose context classes are context: what accessOf denies (`Account::balance is private, named in
  // the function thief`), else empty; a warning of the report too unless access errors are on, a
  // note when a friend declaration allows it.
  std::string checkAccessUse(Decl* used, Node* at, Decl* d, FunctionDecl* fn, const std::vector<Scope*>& context, Scope* naming, const Ty* self = nullptr);

  // The object `a->b` names b of, for the access checks: a pointer's pointee, or what a class's
  // operator-> gives, followed (`handle->forceID` with a Handle<Character> names Character's);
  // null when not known (an operator-> returning auto).
  const Ty* arrowObject(const Ty* t, int depth = 0);

  // What node n names, for the access checks: a name's, a member's or a type name's declaration, and
  // for a member the class it is named through (naming: its object's, a pointer's pointee, what an
  // operator-> gives).
  Decl* usedAt(Node* n, Scope*& naming);

  // Whether every declaration a typed requirement (n: a private copy, typed by its probe) names is
  // one the code of fn in the classes of context may name. C++ checks access in a requires-expression
  // where it is written ([expr.prim.req.general] 5, [temp.deduct] 8): a requirement naming a private
  // member from elsewhere is an invalid expression, and the requires-expression false
  // (LuaBinder<T>'s `requires() { &T::luaIndex; }` with LuaDebugAdapter's luaIndex private).
  bool namesOnlyAccessible(Node* n, FunctionDecl* fn, const std::vector<Scope*>& context, const Ty* self);

  // After the walk of d, a function or a variable: every name, member and type name in it (in a
  // function also its signature, member initializers and the lambdas and local classes of its
  // body) checked against the access of what it names.
  void checkAccessIn(Decl* d, const Ty* self = nullptr);

  // The struct rule, counted over the program's class definitions: a struct is a public value type,
  // so a private or protected member, a base that is not public or a virtual function makes it one
  // that would have to be a class.
  struct StructCounts
  {
    std::map<std::string, std::array<size_t, 6>> byGroup; // structs, classes, with a private member, a private base, a virtual function, polymorphic by a base only
    size_t friends = 0;                                    // friend declarations
  };

  // record: false for the struct rule's question, which no data member's edit changes (an edit that
  // adds or removes a virtual function is a build from scratch): its reads, outside any unit, would
  // make every layout edit of the class one.
  static bool declaresVirtual(ClassDecl* c, bool record = true);

  bool polymorphicBase(ClassDecl* c, int depth) { return polymorphicBaseOf(c, depth); }

  // The base of c (a direct one) that declares a virtual function or has a base that does, or null.
  ClassDecl* polymorphicBaseOf(ClassDecl* c, int depth, bool record = true);

  void structRule(Span<Decl*> decls, std::vector<std::string>& out, StructCounts& counts);

  // A static_assert under ctx (an instance's arguments, when it is in a template): an error when
  // its condition is false, or is no constant the checker can evaluate (never a silent pass).
  // instance: which instance, for the message. One that needs a class's size or alignment waits
  // for the code generator's layout (layoutAsserts). inFunction: the template instance's function
  // body it is in, whose use decides whether it counts (instanceAssertErrors).
  void checkStaticAssert(StaticAssertDecl* s, const TypeContext& ctx, const std::string& instance, const FunctionDecl* inFunction = nullptr);

  // A constexpr variable's initializer is evaluated as C++ requires ([dcl.constexpr] 10): one that is
  // no constant, or that the interpreter cannot evaluate yet, is an error, never a variable
  // initialized at run time. One whose value needs the code generator's layouts (a sizeof the
  // checker's own layouts do not know exactly) is the code generator's to evaluate.
  // inFunction: the template instance's function it is in, whose errors count once it is emitted.
  void checkConstexprVariable(VariableDecl* v, const Ty* t, const TypeContext& ctx, const FunctionDecl* inFunction, const ParsedFile* in);

  // The value of a declaration used as a constant: a const/constexpr variable with an initializer
  // (converted to its declared type), an enumerator, a kpp enum's count, or a non-type template
  // parameter under the substitution. classSubst: the arguments of the class template
  // instantiation a static member was named through (`Bits<uint64_t>::count`).
  std::optional<int64_t> constOfDecl(Decl* d, const TypeContext& ctx, int depth, const Subst* classSubst = nullptr);

  // A variable's type as written, or, for a class template written without its arguments
  // (`constexpr Array names = {...}`, CTAD), the type the walk deduced for it, unless that depends
  // on an instance's arguments.
  const Ty* constDeducedType(VariableDecl* v, const Ty* written);

  // The enumerators' values found so far by the enum evaluations running on this thread
  // (constValueOfDecl), innermost first.
  struct EnumeratorValues
  {
    const EnumDecl* e;
    const Ty* type;
    std::vector<std::pair<const Decl*, ConstValue>> values;
    const EnumeratorValues* outer;
  };
  static thread_local const EnumeratorValues* enumeratorValues;
  // The values of an enumeration's enumerators (not an instance's), all of them, by its type: computed
  // once by constValueOfDecl when one is first asked for, and kept as enumValueRange's answers are (for
  // this GlobalScope's life, a thread's own copy of what it asked: no lock again). Each was computed again
  // from the first enumerator whenever one was asked: Windows' SDK headers write each message id from the
  // one before (`D3D11_MESSAGE_ID_X = (D3D11_MESSAGE_ID_W + 1)`), and the debug type of D3D11_MESSAGE
  // asking for the thousands of D3D11_MESSAGE_ID took codegen 0.5 s on one thread at its end.
  using EnumeratorTable = std::unordered_map<const Decl*, ConstValue>;
  std::mutex enumeratorTablesMutex;
  std::unordered_map<const Ty*, std::shared_ptr<const EnumeratorTable>> enumeratorTables;
  std::shared_ptr<const EnumeratorTable> keptEnumerators(const Ty* enumType);

  std::optional<ConstValue> constValueOfDecl(Decl* d, const TypeContext& ctx, int depth, const Subst* classSubst = nullptr);

  // Compile-time sizeof/alignof for constEval — mirrors the codegen's sizeOf/alignOf but returns
  // nullopt instead of throwing when a type can't be sized (the codegen may still handle it later).
  // Set while a static_assert is checked: a class's size or alignment is then left to the code
  // generator, whose layout is the exact one (bit fields, empty bases), and layoutNeeded says so.
  static thread_local bool exactLayoutOnly;
  static thread_local bool layoutNeeded;
  // The layout flags hold for a static_assert's own condition only: a walk or a typing nested in
  // its evaluation (an instance asked for on the way, a call's argument typed) sizes classes as
  // always, and what it computes is kept.
  struct OrdinaryLayout
  {
    OrdinaryLayout()
    {
      exactLayoutOnly = false;
      layoutNeeded = false;
      constHooks() = nullptr;
    }
    ~OrdinaryLayout()
    {
      exactLayoutOnly = exact;
      layoutNeeded = needed;
      constHooks() = hooks;
    }

    bool exact = exactLayoutOnly, needed = layoutNeeded;
    const ConstHooks* hooks = constHooks(); // and the code generator's hooks are not its
  };

  // An alignas(N) written in class cls (on it, or on one of its data members), N under the class's
  // arguments; 1 when there is none, nothing when N is no constant.
  std::optional<int64_t> constDeclaredAlign(ClassDecl* cls, Expr* alignment, const Subst* sp, int depth);

  std::optional<int64_t> constSizeOf(const Ty* t, int depth);
  // GlobalScope::hasBaseAtOffset, and the declaration-based facts it rests on (structFor's: a class
  // with a virtual function of its own or a polymorphic base is polymorphic; one with no data member,
  // not polymorphic, and only empty bases is empty).
  bool classHasBaseAtOffset(const Ty* cls, int depth);
  bool classStartsWithBase(const Ty* cls, const Ty* base, int depth);
  bool classDeclaredPolymorphic(const Ty* cls, int depth);
  bool classDeclaredEmpty(const Ty* cls, int depth);
  std::vector<const Ty*> classBasesOf(const Ty* cls, ClassDecl*& def, int depth);

  std::optional<int64_t> constAlignOf(const Ty* t, int depth);

  // `static_cast<size_t>(value)` with value an enumerator or a constant parameter (a template
  // argument `bits.test<static_cast<size_t>(value)>()`), `(uint8_t)x`: the operand's value
  // converted to the written type.
  // The byte offset of data member m in an object of class t, in a base of it included: the code
  // generator's layout when it lends it (exact), else, where only that one decides (a
  // static_assert), none yet (layoutNeeded), else the ordinary one sizeof computes with.
  std::optional<int64_t> constFieldOffset(const Ty* t, Decl* m, int depth);

  // `&((T*)nullptr)->a.b[2]`, what the parser reads offsetof(T, a.b[2]) as: the member's offset in
  // a T, a number; nullopt for any other operand of `&`.
  std::optional<int64_t> constMemberOffset(Expr* e, const TypeContext& ctx, int depth);

  std::optional<ConstValue> constCastValue(Type* type, Expr* operand, const TypeContext& ctx, int depth);

  // Whether this thread's stack has room for more nested evaluation (a megabyte kept spare). Every
  // thread of the front end has the same large stack (host::compilerStack), so the bounds below
  // come first; running out of it anyway is a trip of its own (evaluationTrip), never an answer.
  static bool stackRoomLeft();

  // Evaluations nest: a name's value can need another constant's, a template recursion (`N +
  // Sum<N - 1>::value`, `1 + IndexInTemplateList<T, R...>::value` in a base's argument) once per
  // level, and a call another call. Each name's evaluation starts its expressions' depth afresh; how
  // many names and calls nest is bounded: 1024 and 512 ([implimits]' nested template instantiations
  // and constexpr calls, clang's defaults). Counted as if nothing were cached: a cached answer
  // (CachedEvaluation) keeps how much deeper its computation nested, and an asker it answers nests
  // that much deeper (cachedNestingFits). So what another thread happened to compute first changes
  // no answer (a template recursion 1100 deep compiled or not by the order of the walks, its errors
  // changed with -j; thread C's trial, an error whose instance changed with --jobs). Past a bound, or
  // past the stack's end, the evaluation trips (evaluationTrips, a count that only grows): no answer,
  // and nothing computed while it tripped is cached.
  // A cached computation is computed as if asked from the top (its depth 0), whoever asks it from how
  // deep: a partial specialisation's match asked by a type nested 20 deep picked the primary template
  // (resolveSpecialization's own depth bound), and one cached by a deep asker stayed for every asker.
  // Cached computations nested in one another are counted as names are (levels, 4096), which bounds a
  // recursion through them that no name evaluation is part of.
  static constexpr int maxNestedNames = 1024, maxNestedCalls = 512, maxNestedLevels = 4096;
  static constexpr int maxArrowChain = 256; // operator-> returning a class with an operator-> (clang's -foperator-arrow-depth)
  static thread_local int nameEvaluations, cachedLevels;
  static thread_local int deepestNames, deepestCalls, deepestLevels; // the most nested so far in the innermost CachedEvaluation
  static thread_local uint64_t evaluationTrips;
  static void evaluationTrip(const char* why);
  static bool enterName(); // ++nameEvaluations; false (a trip) past the bound or the stack
  static bool enterCall(); // ++constCalls(); likewise
  struct NameEvaluation
  {
    NameEvaluation() : ok(enterName()) {}
    ~NameEvaluation() { --nameEvaluations; }

    bool ok;
  };
  struct EvaluationCall
  {
    EvaluationCall() : ok(enterCall()) {}
    ~EvaluationCall() { --constCalls(); }

    bool ok;
  };
  // A cached answer's computation (memberConstant, constLayoutOf, resolveSpecialization,
  // traitValue, declaredTypeOf, an alias template's type): how much deeper it nested (needed),
  // whether it tripped (then it is not cached), and its own budget of steps and values and its own
  // const globals' values (constGlobalValues), as if nothing had been evaluated before it: an answer,
  // and what it needed, are the same whoever asked first.
  struct CachedEvaluation
  {
    CachedEvaluation()
      : names(nameEvaluations), calls(constCalls()), levels(cachedLevels), outerNames(deepestNames), outerCalls(deepestCalls), outerLevels(deepestLevels),
        trips(evaluationTrips), steps(constSteps()), made(constValuesMade())
    {
      deepestNames = names;
      deepestCalls = calls;
      deepestLevels = ++cachedLevels;
      ok = cachedLevels <= maxNestedLevels;
      if (!ok)
        evaluationTrip(levelsTooDeep());
      constSteps() = 0;
      constValuesMade() = 0;
      if (!constGlobalValues().empty()) // (an empty map is no allocation: most computations evaluate no global)
      {
        globals.emplace();
        std::swap(*globals, constGlobalValues());
      }
    }
    ~CachedEvaluation()
    {
      --cachedLevels;
      deepestNames = std::max(outerNames, deepestNames);
      deepestCalls = std::max(outerCalls, deepestCalls);
      deepestLevels = std::max(outerLevels, deepestLevels);
      constSteps() = steps;
      constValuesMade() = made;
      if (globals)
        std::swap(*globals, constGlobalValues());
      else
        constGlobalValues().clear();
    }
    CachedEvaluation(const CachedEvaluation&) = delete;

    Nesting needed() const { return {deepestNames - names, deepestCalls - calls, deepestLevels - levels}; }
    bool tripped() const { return evaluationTrips != trips; }

    bool ok; // false: past the bound of levels (a trip); compute nothing
    int names;
    int calls;
    int levels;
    int outerNames;
    int outerCalls;
    int outerLevels;
    uint64_t trips;
    uint64_t steps;
    uint64_t made;
    std::optional<std::map<std::pair<Decl*, std::vector<const Ty*>>, ConstValue>> globals; // the asker's, none when it had none
    CachedEvaluation& operator=(const CachedEvaluation&) = delete;
  };
  // A cached answer whose computation nested `needed` deeper, asked here: false (a trip) when that
  // is past a bound, as computing it here would have been.
  static bool cachedNestingFits(Nesting needed);
  // A Nesting in 32 bits, for a cache kept in a declaration (Decl::typeNeeded): names in 11 bits,
  // calls in 10, levels in 11; none when it does not fit (an answer that needed more levels is not
  // kept, and computed anew is counted as it is).
  static std::optional<uint32_t> packNesting(Nesting n)
  {
    if (n.names < 0 || n.names >= 2048 || n.calls < 0 || n.calls >= 1024 || n.levels < 0 || n.levels >= 2048)
      return std::nullopt;
    return uint32_t(n.names) | uint32_t(n.calls) << 11 | uint32_t(n.levels) << 21;
  }
  static Nesting unpackNesting(uint32_t p) { return {int(p & 2047), int(p >> 11 & 1023), int(p >> 21)}; }
  static const char* levelsTooDeep();
  // Declared types under a substitution (a class template's member alias given its instance's
  // arguments) are not kept in their declaration; within one outermost such computation each one
  // is computed once (`using A2 = P<A1, A1>;` in a class template computed A1 twice per level: time
  // doubling with each alias), and an asker it answers nests as much deeper as it did.
  struct SubstTypeMemo
  {
    struct Entry
    {
      const Ty* type;
      Nesting needed;
    };
    std::optional<std::map<std::vector<const void*>, Entry>> types; // made at the first entry (most computations keep none)
    int rootLevel = 0; // the outermost computation's level
    // Kept from this many levels below it: a hit answers as computing it again would (its nesting
    // included), so which are kept changes no answer; shallow ones are rarely asked again (the game:
    // 1.8 million nested computations, a tenth of them asked twice) and cost a key and an entry each.
    static constexpr int fromLevel = 4;
  };
  static thread_local SubstTypeMemo* substTypeMemo; // the outermost computation's, none outside one
  // Declarations whose types this thread is computing (without a substitution), each with its
  // computation's level, and the lowest level a cycle was cut at inside the current computation.
  struct TypeInProgress
  {
    Decl* decl;
    int level;
  };
  static thread_local std::vector<TypeInProgress> typesInProgress;
  static thread_local int cycleCutAt;

  // A static member's value for a class instance's arguments, kept: a template recursion asks for
  // the same ones over and over. Not while a static_assert is checked (its class layouts are the
  // code generator's, see exactLayoutOnly), and only values that were found.
  std::mutex memberConstantsMutex;
  struct MemberConstant
  {
    ConstValue value;
    std::vector<CollectedUse> uses; // what computing it read: an asker it answers read it too
    Nesting needed;
  };
  std::map<std::vector<const void*>, MemberConstant> memberConstants;
  std::optional<ConstValue> memberConstant(Decl* d, const TypeContext& ctx, const Subst& classSubst);

