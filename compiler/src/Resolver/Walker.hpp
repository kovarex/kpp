// GlobalScope::Impl, phase 2, walking: the Walker, which types and checks bodies: part of the class's body, included in it in GlobalScopeImpl.hpp,
// its definitions in Walker.cpp (resolver.cpp split, 2026-11-05, moved without a change).
  // ---------------- phase 2: walking ----------------

  struct Walker
  {
    Impl& impl;
    TypeTable& types;
    ResolveStats& stats;
    const SymbolTable& symbols;
    const std::vector<Token>& tokens;
    std::string_view text;
    LocalStack locals;
    Scope* scope;            // Current lexical/semantic scope.
    const Ty* thisType = nullptr;
    const Subst* activeSubst = nullptr; // Set while checking a template body for an instantiation.
    int instantiationDepth = 0;
    const ParsedFile* file = nullptr; // The file the tokens belong to (for --why sites).
    Expr* current = nullptr;          // The expression being typed (for --why sites).
    // Of an instance's walk: where it was first asked for (the asking walk's expression), for the
    // messages about it.
    const ParsedFile* requestFile = nullptr;
    uint32_t requestToken = UINT32_MAX;
    FunctionDecl* bodyOf = nullptr;   // The function whose body is being walked.
    int instantiatedBaseDepth = 0;    // memberOfInstantiatedBases nesting
    Expr* const* walkedArgs = nullptr; // see WalkedArgs
    std::vector<const Ty*> walkedArgTypes;
    const Ty* deducedLocal = nullptr;  // the `auto` type walkDecl just deduced, for walkLocalDecl
    size_t blockLocals = 0;            // where the innermost block's locals start (a `when` clash: whenHolds)
    std::vector<size_t> blockStarts;   // where each enclosing block's locals start, the innermost last (whenUse)
    int undecidedBranches = 0;         // inside an instance's `if constexpr` branch this walk could not decide
    // Under --no-implicit-fallthrough, of the switches checkCaseEnds saw: their labels (any other is nested in a
    // statement) and each `fallthrough;` ending a group with another after it (any other is misplaced).
    std::unordered_set<const Stmt*> switchLabels, placedFallthroughs;
    // The code generator's re-typing of an instance's copy with a type pack bound to one element
    // (a fold over the pack, GlobalScope::retypeFor): its static_asserts were checked with the
    // whole pack, and a fold over the pack cannot be evaluated against one element.
    bool retypingElements = false;
    // Of that re-typing for one element of a parameter pack (to emit its expansion's pattern): an
    // if constexpr keeps the branch the instance took with the whole pack, which one element
    // would decide anew (`isConstructible<T, Input&, Args&&...>`, false for each element alone).
    // Not for a `for constexpr` element, whose body's if constexpr depends on it.
    bool keepConstexprBranches = false;
    // Of that re-typing: the packs whose mentions name one element (TemplateArgs::elementPacks),
    // activeSubst binding them whole; null otherwise.
    const std::vector<Decl*>* elementPacks = nullptr;
    uint32_t elementIndex = 0;
    // Return statements inside lambdas / auto functions record their type here.
    std::vector<const Ty*> returnFrames;
    // Set on the scratch walkers that probe a requires-expression's requirements: an explicit
    // conversion of a class to a type that is no class (`bool(instance)`) is typed only when
    // the class has an operator for it, since a requirement holds exactly when it is valid.
    bool probing = false;
    bool walkingQueued = false; // a queued walk (Impl::DeclWalk): its own declaration is walked here, not queued again

    // The type of an explicit conversion of operand to t (a cast) while probing: unknown when the
    // operand is a class with no conversion operator to t (explicit ones count), t otherwise.
    const Ty* probedCast(const Ty* t, const Ty* operand);

    int forConstexprDepth = 0; // for constexpr bodies this walk is inside (typed with the variable unbound)

    // The elements of a for constexpr list under the current substitution: the types of
    // `types<A, B>`, the constants of `{1, 2}`, the descriptors of `alternatives(U)` and of
    // `members(T)` (a class without bases). False when one cannot be told yet.
    bool forConstexprElements(ForConstexprStmt* f, std::vector<const Ty*>& out);

    // Per lambda / auto function this walk entered: whether it returns `decltype(auto)`, so that
    // its returns record their expression's decltype (`f(x)` returning int& stays int&).
    std::vector<bool> decltypeReturns;
    // The variables the lambdas this walk is in capture by copy without being `mutable`: const in
    // their bodies (a const local item over each), which a write's error names.
    std::vector<Decl*> constCopyCaptures;

    // The type `decltype(e)` names, for a `decltype(auto)` return, given e's type t: a variable
    // or member named without parentheses has its declared type, any other lvalue is a reference
    // to its type, a call or cast keeps the reference its type has.
    const Ty* decltypeOf(Expr* e, const Ty* t);

    // A typed union (`union enum`, design.md 8.5), given its class type; null for anything else.
    static ClassDecl* typedUnionOf(const Ty* t);

    // `types<A, B, C>`: the compile-time list of types of design.md 8.1 (a builtin name).
    bool isTypesList(const NamedType* n) const;

    // `members(T)` (design.md 8.3), `alternatives(U)` (8.5): the type reflected on, or null when
    // the node is no such list.
    Expr* reflectionSubject(Node* list) const;

    // The enumerator of union u's tag that an index written as a bare name names, when the name
    // means nothing where it is written (`p[Integer]` outside the union: design.md 8.5).
    EnumeratorDecl* alternativeNamed(ClassDecl* u, Expr* index);

    // The typed union whose alternative a descriptor names (`for constexpr (auto a :
    // alternatives(U))`), with the alternative's index; null for a member descriptor.
    static ClassDecl* alternativeOf(const Ty* descriptor, size_t& index);

    TokenScope tokenScope; // this walker's file is where literals are read from, lookups included
    Walker(Impl& impl, ResolveStats& stats, const std::vector<Token>& tokens, std::string_view text);

    std::string_view spelling(uint32_t token) const;
    // A literal's spelling, from the file it was parsed from (the walk may be another file's).
    std::string_view spelling(const LiteralExpr* l) const;

    // --- statistics helpers ---

    void countUnresolved(const QualifiedName& name, uint32_t token);

    // A name nothing declares: an error at the name. In a template, as C++'s two phases have it
    // ([temp.res]): in its own walk when the name does not depend on its parameters, and a call
    // whose argument does (argument-dependent lookup with the instance's types), or a name a base
    // that is a parameter may declare, in each instance whose function the program emits.
    // dependentCall: the name is called with an argument whose type depends on a parameter (or is
    // not known).
    const NameExpr* undeclaredLater = nullptr; // a subscript's lone index name, judged by the subscript (a typed union's tag)
    // A scoped library's names are written NAME::name (design.md 16): no using-directive or
    // namespace alias brings them in.
    void checkNotLibrary(const QualifiedName& name, uint32_t token);
    void checkNotLibrary(const Type* type, uint32_t token);

    void reportUndeclared(const QualifiedName& name, uint32_t token, const NameExpr* at = nullptr, bool dependentCall = false);

    // An error in an instance's body: counts once its function is emitted (an `if constexpr` branch
    // the walk could not decide may be the discarded one, and a member of a class template's
    // instance that nothing calls is never instantiated in C++: Array<const char, N>::fill).
    void reportInInstance(uint32_t token, std::string message);
    void checkTestStatementName(const AssertStmt* a);
    void checkTestDeclaration(FunctionDecl* f);

    // A warning at token: in an instance's body one that counts once its function is emitted
    // (Impl::instanceWarnings), none in an `if constexpr` branch the walk could not decide.
    void reportWarning(uint32_t token, const std::string& message);

    // A built-in assignment (`=`, `+=` and the others on a scalar), increment or decrement of a const
    // object is an error, as in C++ ([expr.ass]/1, [expr.pre.incr]: the operand is a modifiable
    // lvalue): through a `const T&`, a const local or member, a const getter's result. Not in a
    // template's own walk (its instances say). what: "assignment to", "increment of", "decrement of".
    // operand: the expression written to, which names a lambda's capture by copy in the message.
    void checkConstModification(uint32_t token, const Ty* object, const char* what, const Expr* operand = nullptr);

    // A checker error at token: in a template's instance once its function is emitted, never in the
    // template's own walk (its instances say), nowhere while probing or in imported C.
    void reportCheck(uint32_t token, const std::string& message);
    // A non-const lvalue reference (declared) bound to a const object of its type or of a class
    // derived from it (given: the initializer as an argument presents it, an lvalue's a T&): an
    // error naming what is bound (`r`, `the result of get`), as C++'s [dcl.init.ref] 5 has it.
    void checkConstDropped(uint32_t token, const std::string& what, const Ty* declared, const Ty* given);
    // A static_cast, reinterpret_cast or dynamic_cast (c, its operand of type from, to type to)
    // that casts away const: to a reference or a pointer to a non-const object from a const one
    // (a pointer's or array's elements), which only const_cast may ([expr.static.cast] 1).
    void checkCastConst(NamedCastExpr* c, const Ty* from, const Ty* to);
    // A built-in assignment's left operand, an increment's or a decrement's operand (of type
    // type): an lvalue, else an error naming what (`f() = 5`, `x++ = 5`, `f()++`); a class's
    // operator function is the class's to judge.
    void checkAssignable(uint32_t token, const char* what, const Expr* operand, const Ty* type);
    // A non-const lvalue reference (declared) bound to a value (given: no T&, a prvalue or an
    // xvalue) it cannot take: `int& r = value();`, `return 5;` from a function returning int&;
    // an error naming what is bound, as C++'s [dcl.init.ref] 5 (a class whose conversion operator
    // gives an lvalue of the type is taken: conversionRank's).
    void checkTemporaryBound(uint32_t token, const std::string& what, const Ty* declared, const Ty* given);

    // Whether f, a member function, is a non-const one called on a const object (objectTy): no
    // candidate in C++, whose implicit object parameter `C&` a const object does not bind
    // ([over.match.funcs]/4-5). A static member, a constructor or a destructor takes any object.
    bool nonConstOnConstObject(FunctionDecl* f, const Ty* objectTy);

    void checkConstObjectCall(FunctionDecl* f, const Ty* objectTy, uint32_t token);
    // A call on a const object (objectTy) whose overloads (pickOverload's arguments) took none: the
    // one a non-const object would have taken, a non-const member function, is the error
    // checkConstObjectCall reports (`items.push_back(1)` in a const member function, push_back's
    // two overloads both non-const), where the call was left unresolved, codegen's error.
    void checkConstObjectOverloads(Decl* primary, const std::vector<Entry>* overloads, const std::vector<const Ty*>& argTypes, const Subst* classSubst,
                                   const Ty* objectTy, const NamePart* namePart, Span<Expr*> callArgs, uint32_t token);
    // A member call none of whose overloads took the arguments, left unresolved (codegen could only
    // say it does not support it): "no overload of String::find takes (const char[7], int32, int32)",
    // when the object's and the arguments' types are all known and none is dependent, and every
    // candidate is a plain function (a template's deduction pickOverload does not make, codegen may).
    // A call no candidate takes because each is a function template whose deduction conflicts, one parameter deduced as
    // two types by two arguments (`refConv(1, 2.0)` for `template<class T> int refConv(const T&, T)`): the checker's
    // error, where it was codegen's "a call that did not resolve". Only where it is sure. objectTy: a member's object,
    // whose class names the function in the error (null for a function's call).
    void reportDeductionConflict(Decl* primary, const std::vector<Entry>* overloads, const std::vector<const Ty*>& argTypes, const NamePart& name,
                                 const Ty* objectTy, uint32_t token);
    void reportNoMatchingMember(Decl* primary, const std::vector<Entry>* overloads, const Ty* objectTy, uint32_t symbol,
                                const std::vector<const Ty*>& argTypes, uint32_t token);

    // A function defined twice (two bodies of one function: `int f(int)` and `int f(int)`, or
    // `f(long)` beside `f(int)`, one function in kpp, design.md "Types") is an error at the later
    // definition, as in C++ ([basic.def.odr]); the code generator would emit both. Not templates
    // (their specialisations and instances have bodies of their own) nor a `conditional(...)` one
    // (alternatives of which one exists).
    void checkDefinedOnce(FunctionDecl* f);
    // A ref-qualified member function called on an object its implicit object parameter does not
    // bind ([over.match.funcs] 4): `f() &&` on an lvalue, a non-const `f() &` on an rvalue, is an
    // error at the call, as in C++. objectTy: the object with its value category (T& or T&&).
    void checkRefQualifiedCall(FunctionDecl* f, const Ty* objectTy, uint32_t token);
    // Overloads of one name and parameter list of which some have a ref qualifier and some none
    // are an error ([over.load] 2.3), reported at the later declaration.
    void checkRefQualifierMix(FunctionDecl* f);

    // An object's array bound (a variable's, a data member's) that is no constant, or is negative,
    // is an error at the bound, as in C++ ([dcl.array] 1), never an array of no elements. Checked
    // where its template parameters are known (not in a template's own walk: in its instances);
    // `int a[] = {1, 2}` has its bound from the list, `new T[n]` a run-time one.
    void checkArrayBounds(VariableDecl* v);

    // An expression statement that discards a call's result where the function, or the class or
    // enumeration it returns, is [[nodiscard]]: a warning, as C++ compilers give (a cast to void
    // says it is meant).
    void checkDiscarded(Expr* e);

    // A call of a [[deprecated]] function: a warning naming it, with the reason it was given.
    void noteDeprecatedCall(FunctionDecl* f, Node* at);

    // An array of unknown bound whose values elide an element's braces: the bound the declaration's
    // type counted, typing the values as a signature's (elementsFromValues), must be the one the
    // walked values' types give (`In a[] = {x, y, z, w}` of locals, which a signature cannot type):
    // an error otherwise, never a wrong bound.
    void checkElidedBound(VariableDecl* v, const Ty* declared);

    // Whether a class the walk is in has a base no scope stands for until it is instantiated (a
    // template parameter, `class Wrapper : public T`), which may declare any name.
    bool inClassWithParameterBase();

    enum class Why : uint8_t { None, Name, Base, Member, Overload, Deduction, AutoReturn, Unsupported, Operator, Auto };

    const Ty* unknown(Why why);

    // Records the type of an expression node and counts it.
    // A name of an `auto`-returning function (not a template, nor a member of one) has the type its
    // body deduces, deduced now if its own file's walk has not done it yet: otherwise the name was
    // `auto(...)` or the deduced type depending on which thread walked which file first.
    const Ty* settledCalleeType(Expr* e, const Ty* t);

    const Ty* record(Expr* e, const Ty* t);

    // --why-at FILE:LINE,...: every expression typed on those lines, with its type (innermost first).
    void traceAt(Expr* e, const Ty* t);

    TypeContext ctx() const;
    // A name written in the walked code, looked up as this walk types it: under activeSubst, and in a pack
    // expansion's pattern typed for one element (elementPacks) naming that element.
    Lookup lookupHere(const QualifiedName& name) { return impl.lookupQualified(name, scope, &locals, 0, activeSubst, elementPacks, elementIndex); }

    // --- template instantiation: check bodies once per (template, arguments) ---

    static bool argsConcrete(TypeTable& types, const Ty* t);

    // Runs `body` as a sub-check with the given substitution, scope and this-type, in the
    // context of the template's own file. Expression statistics are merged into ours.
    template<typename F>
    void subCheck(Decl* templ, const Subst& subst, Scope* bodyScope, const Ty* bodyThis, F&& body, const void* owner = nullptr)
    {
      ParsedFile* file = templ->file;
      if (!file || !file->tokens)
        return;
      Impl::OrdinaryLayout ordinary;
      ResolveStats local;
      // The places it records are the template's file's tokens (a statistic's "first at" names them):
      // that file's, not the requester's.
      local.fileIndex = file->index;
      local.collectWhy = stats.collectWhy;
      Walker sub(impl, local, *file->tokens, file->text);
      sub.file = file;
      FileOfLookups walkedFile(file);
      sub.scope = bodyScope;
      sub.thisType = bodyThis;
      sub.activeSubst = &subst;
      sub.instantiationDepth = instantiationDepth + 1;
      if (current && this->file)
      {
        sub.requestFile = this->file;
        sub.requestToken = current->token;
      }
      for (Decl* p : templ->templateParams)
        sub.locals.push(static_cast<TemplateParamDecl*>(p)->name, p);
      body(sub);
      local.instExprsTyped += local.exprsTyped;
      local.instExprsDependent += local.exprsDependent;
      local.instExprsUnknown += local.exprsUnknown;
      impl.addInstanceStats(local, owner); // counted once, whoever asked (Impl::instanceStatsTotal)
    }

    // Types the instantiation of member f for the class's arguments, in a copy of f's nodes kept
    // as that instantiation's body (unless it exists already: an `auto` member asked for its
    // return type first).
    void checkMemberBody(Walker& sub, FunctionDecl* f, const Subst& classSubst);

    // Checks every member function body of a class template for one instantiation, once.
    void instantiateClass(const Ty* classTy);

    // Checks a function template body for one set of deduced arguments, once. Returns the
    // deduced return type when the function returns `auto` (waiting for another producer).
    // record: the instance's record, when one was claimed or found (complete unless this thread
    // is walking it).
    const Ty* instantiateFunction(FunctionDecl* f, const Subst& subst, bool needReturn, const Subst* classSubst = nullptr, const Ty* objectType = nullptr, Impl::InstanceRecord** record = nullptr);

    // Checks the function instance inst describes (claimed by this walk, or a queued one this walk
    // took over) on this thread, and publishes its body and deduced return.
    const Ty* runFunctionInstance(Impl::InstanceRecord* inst, const Impl::FunctionInstanceTask& t);

    // --- names ---

    // A name two entities answer equally (a namespace's class and a using-directive's): an error,
    // as in C++, where the first was taken silently.
    void reportAmbiguity(const Lookup& l, const QualifiedName& name, uint32_t token);

    // A member two bases of a class have ([class.member.lookup] 6): an error where it is used.
    void reportBaseAmbiguity(const Lookup& l, uint32_t symbol, uint32_t token);

    void resolveTypeName(NamedType* t);

    // `UniquePtr<Thing, CountingDeleter>` with a one-parameter UniquePtr: more template arguments
    // than the class (or alias) template has parameters, and no pack to take the rest, is a
    // compile error, as in C++ (the extra argument was dropped silently).
    void checkTemplateArgumentCount(NamedType* t, const Lookup& l);

    // `InfiniteRectangle<double>` with `template<ConvertibleToBool T> class InfiniteRectangle`: a
    // class template's constrained parameter checked for the arguments written, as C++ checks it
    // where the specialization is named ([temp.names] 8); one its argument does not satisfy is a
    // compile error (it was instantiated as if unconstrained).
    void checkClassConstraints(NamedType* t, const Lookup& l);

    // `S<Box<int>, int>` with `S<T, int>` and `S<Box<T>, U>` both matching and neither more
    // specialized: ambiguous, a compile error as C++ has it ([temp.spec.partial.match] 1); the first
    // declared was taken.
    void checkSpecializationTie(NamedType* t, const Lookup& l);
    // Each part of a name's qualifier that names a class template's specialization (`S<const int,
    // int*>::v()`, `S<A, B>::Inner::f`): its partial specializations' tie, as checkSpecializationTie
    // checks a type name. A call's callee reached none: it was taken silently, the first declared.
    void checkQualifierTies(const QualifiedName& name, uint32_t token);

    // Converts a type written in the current context.
    const Ty* typeOf(Type* t) { return impl.convertType(t, ctx(), 0); }

    // Type of a name expression from the declaration it resolved to.
    // The hidden object of `auto [a, b] = e;` (v) as this walk deduced it; null when not known.
    const Ty* hiddenObjectType(VariableDecl* v);

    // `auto [a, b] = pair;`: the type of the binding named `symbol`, the matching data member (or
    // array element) of the hidden object the declaration names; null when symbol is no binding.
    // (A tuple-like binding names a variable of its own: tupleBindings.)
    const Ty* bindingType(VariableDecl* v, uint32_t symbol);

    const Ty* typeOfResolvedName(const Lookup& l, const NamePart& lastPart, Expr* e);

    // The type of a template parameter named as a value or a type: what activeSubst binds it to,
    // a constant parameter's value as its type, and in the pattern of one element's expansion
    // (elementPacks) that element (`values[I]...` over `int... I`, `func(Args())...`).
    const Ty* dependentNameType(Decl* d);

    const Ty* resolveName(NameExpr* e);
    // The type of a data member a name finds without an object (`v`, `Base::b`, l's) in a member
    // function, `this->v`: t made const where `this` points at const (a const member function's,
    // also in a lambda in it), as memberOf makes a const object's members; a mutable, static or
    // reference member's t as it is.
    const Ty* memberThroughThis(const Lookup& l, const Ty* t);
    // `a?->b` of a value of type value (design.md 23a): Optional<value>, or null when the result stays value
    // (a pointer, a class with operator-> and a bool conversion, a bool, void, or not known yet).
    const Ty* safeNavigationOptional(const Ty* value, const Expr* at);

    // --this-warnings (design.md 23c): an unqualified name that finds a data member or a member function
    // of a class, which required `this.` writes `this.x` (a static one `This::x`). One beside a name of its
    // spelling around the class says so: a bare name will be that one.
    void noteImplicitMember(const NameExpr* n, const Lookup& l);
    // --this-reference-warnings (design.md 23d): what e does with `this` that changes once it is a reference,
    // noted before e's operands are walked; a `this` no parent claimed is its address (noteThisAddress).
    void noteThisUse(Expr* e);
    void noteThisCondition(Node* cond);
    void noteThisAddress(Expr* thisExpr);
    void noteThisReference(uint32_t token, const std::string& message);
    // --plain-enum-warnings: a plain enum declared in kpp code, and the uses of its enumerators e makes, noted
    // once e's operands are walked (their names resolved): `kind` the use's (bits, integer, index, size).
    const Ty* walkExprDispatch(Expr* e);
    void notePlainEnumDecl(EnumDecl* e);
    void notePlainEnumUses(Expr* e);
    void notePlainEnumUse(const Expr* operand, const char* kind);
    void noteDefaultCaptureOfThis(LambdaExpr* l);
    std::unordered_set<const Expr*> thisUsesNoted; // the `this` nodes a parent has spoken for
    // A declaration in kpp code named `This`, the keyword for the class around the code: an error (design.md 23c).
    void checkNotThis(uint32_t symbol, uint32_t token);
    // The name of a scoped enum the scopes around declare with an enumerator spelled symbol, for "write
    // Direction::North"; empty when there is none.
    std::string scopedEnumeratorNamed(uint32_t symbol);
    // Inside a parameter's, a data member's or a function's leading return type, where `this` is no
    // value and a name needs no `this.` (design.md 23c).
    int declaredTypes = 0;
    // How the class cls reaches the class scope target, itself or a base's: 0 not at all, 1 through bases
    // that do not depend on a template parameter, 2 only through one that does.
    int reachThroughBases(Scope* cls, Scope* target, int depth);

    // `Accumulator::kind` (`&T::kind` in an instance) with kind a member of a base that an
    // instantiation's arguments choose (`class BehaviorTemplate : public Parent`, Parent =
    // Behavior): the class template's scope links no such base, so the name is looked up as a
    // member of the qualifier's type, through its instantiated bases.
    Lookup memberOfQualifierInstance(NameExpr* e);

    // --- member access through the object's type ---

    struct MemberResult
    {
      const Ty* type = nullptr;
      Decl* decl = nullptr;
      const std::vector<Entry>* overloads = nullptr;
      std::shared_ptr<std::vector<Entry>> merged; // what overloads points to when built here (a using-declaration's set joined)
      Subst subst;
      Why why = Why::None;
    };

    // Follows operator-> on class types for `a->b`.
    const Ty* objectForArrow(const Ty* t, int depth = 0);

    // Looks a member up in the class of `objectTy` (after stripping references and const).
    // The instantiation a member found in scope `in` belongs to, seen through an object of type t
    // whose class scope is cls: t itself, or the base (as written in cls, under t's substitution)
    // on the way to `in`. `v.x` with x declared in Vector2<T, V> and v a Vec: Vector2<double, Vec>.
    const Ty* instantiationOfBase(const Ty* t, Scope* cls, Scope* in, int depth);

    // `symbol` in the bases of the instantiation t (class scope cls) as its arguments make them.
    std::optional<MemberResult> memberOfInstantiatedBases(const Ty* t, Scope* cls, uint32_t symbol, uint32_t token);

    // `using Vector2::save;` beside the class's own save overloads (l found in a class scope; t the
    // class as the object or `this` has it, cls its scope): both sets as candidates, own first,
    // with the arguments of the base instances the using-declarations reach in baseArgs; null when
    // l has no such mix.
    std::shared_ptr<std::vector<Entry>> withUsingOverloads(const Lookup& l, const Ty* t, Scope* cls, Subst& baseArgs);

    MemberResult memberOf(const Ty* objectTy, uint32_t symbol, const QualifiedName* name, uint32_t token, bool countMissing = true);

    // True if the class or one of its bases has a base type that could not be resolved.
    bool hasUnknownBase(Scope* cls, int depth);

    const Ty* typeOfMember(MemberExpr* m);

    // --- calls ---

    // Whether target is cls or one of its bases, directly or not.
    bool reachesClassScope(Scope* cls, Scope* target, int depth);

    // Picks a function among overloads by argument count and conversion ranking.
    // An ambiguous call (tied: one of each function no other beats), a checker error when the
    // types are concrete (a template's own walk decides nothing; its instances do), C++'s
    // `call is ambiguous`; behind Impl::ambiguousCallsAreErrors.
    void reportAmbiguousCall(CallExpr* c, const NamePart& name, const std::vector<const Ty*>& argTypes, const std::vector<FunctionDecl*>& tied, const Subst* classSubst);

    // A new-expression's allocation function (C++ [expr.new] 9-12, 24): the class's own operator new
    // for (size, the placement arguments...) when the class has one and the expression is not
    // `::new`, recorded on n with the operator delete that gives the memory back when its constructor
    // throws (usualDeallocation's, or the placement one whose parameters after the first are the
    // operator new's). t is the class allocated. An operator new that takes no such arguments is an
    // error, as in C++.
    // array: `new T[n]`, of the class's operator new[] and operator delete[]. Without a class's own
    // (or with `::new`): chooseGlobalAllocation.
    void chooseAllocation(NewExpr* n, const Ty* t, bool array);

    // The global operator new (array: new[]) for (size, the placement arguments...) when the program
    // declares its own allocation functions (GlobalScope::programAllocationFunctions), recorded on n
    // with a placement form's operator delete; a placement at an address no global one takes stays
    // the library's. Otherwise nothing: new allocates with malloc.
    void chooseGlobalAllocation(NewExpr* n, const Ty* t, bool array);

    // The operator delete a delete-expression of a class t object calls ([expr.delete] 10): the class's
    // own `operator delete(void*)`, else its `operator delete(void*, size_t)` (sized). Null when the
    // class (or a base) declares none (declared false) or only other forms (declared true).
    // array: its operator delete[] (delete[]).
    FunctionDecl* usualDeallocation(const Ty* t, uint32_t token, bool& sized, bool& declared, bool array);

    FunctionDecl* pickOverload(Decl* primary, const std::vector<Entry>* overloads, const std::vector<const Ty*>& argTypes, const Subst* classSubst, const Ty* objectTy, bool& ambiguous, const NamePart* namePart = nullptr, Span<Expr*> callArgs = {}, std::vector<FunctionDecl*>* tied = nullptr);

    // Argument types of a call; a pack expansion `args...` contributes one type per element.
    // Lvalues present themselves as lvalue references (argumentType).
    // The parameter pack an expansion pattern expands (the first pack parameter named in it), or null.
    Decl* packParamIn(Node* n, int depth = 0);

    // `f(forward<Args>(args)...)` under an instantiation with an empty Args: no argument at all.
    bool isEmptyExpansion(Expr* a);

    std::vector<const Ty*> argTypesOf(const Span<Expr*>& args);

    // The argument list of the call being typed and the types this walk gave its expressions, which
    // argTypesOf uses for it instead of the types on the AST nodes: the AST is shared, and another
    // thread checking another instantiation of the same body may have rewritten them meanwhile.
    struct WalkedArgs
    {
      Walker& w;
      Expr* const* savedArgs;
      std::vector<const Ty*> savedTypes;
      WalkedArgs(Walker& w, const Span<Expr*>& args, std::vector<const Ty*>&& types)
        : w(w), savedArgs(w.walkedArgs), savedTypes(std::move(w.walkedArgTypes))
      {
        w.walkedArgs = args.data;
        w.walkedArgTypes = std::move(types);
      }
      ~WalkedArgs()
      {
        w.walkedArgs = savedArgs;
        w.walkedArgTypes = std::move(savedTypes);
      }
    };

    // The explicit template arguments of a call (`count<int, int, int>()`): one per parameter;
    // a trailing pack takes the rest, and is empty when the list leaves it nothing and there is
    // no call argument to deduce it from. A pack expansion in the list (`load<V, Z, Args...>`)
    // gives one argument per element of the pack, none for an empty one. Returns how many
    // arguments the list names: an expansion still dependent counts as none, since it may name
    // none.
    size_t bindExplicitArgs(Span<Decl*> templateParams, const NamePart& namePart, Span<Expr*> args, Subst& s, bool emptyPackWithoutArgs = true);
    size_t bindExplicitArgs(FunctionDecl* f, const NamePart& namePart, Span<Expr*> args, Subst& s, bool emptyPackWithoutArgs = true);

    // Whether an explicit template argument is of its parameter's kind: a type for a type
    // parameter, a constant for a constant one. One not known yet, or dependent on the
    // enclosing template in a way that does not tell, fits.
    bool argumentKindFits(const Ty* t, const TemplateParamDecl* p);

    // An explicit template argument of a call (`forward<Args>(args)`): under an instantiation that
    // binds the pack Args to one element, a non-pack parameter of the callee gets that element,
    // not a pack of one. An expansion (`typeIndex<T, Tail...>`) stays the pack it expands.
    const Ty* explicitArgument(Node* written, Decl* param);

    // Records f's deduced template arguments on the call for the code generator: built aside and
    // stored with the count last, since instantiations checked on other threads type the same call
    // (the count was incremented in place and could pass the 8 slots).
    static void recordTemplateArgs(CallExpr* call, FunctionDecl* f, const Subst& s);

    // Return type of calling `f` with the given arguments, deducing its template parameters. An
    // operator expression has no argument list: its operand types (operands) are deduced from.
    const Ty* returnTypeOfCall(FunctionDecl* f, const Subst* classSubst, const Span<Expr*>& args, const NamePart* namePart, CallExpr* call = nullptr, const std::vector<const Ty*>* operands = nullptr);

    // l, an unqualified call's lookup, with the functions of the name that the namespaces
    // associated with the argument types declare (Impl::associatedNamespaces) added, and the
    // friend functions their classes define (Impl::hiddenFriendLookup), whose classes' template
    // arguments go into friendArgs.
    void addArgumentDependentCandidates(Lookup& l, uint32_t symbol, const std::vector<const Ty*>& argTypes, Subst& friendArgs, uint32_t token);

    // An error of the code being walked: in an instance, said with the instance; in a template's
    // own walk, left to its instances.
    void reportHere(uint32_t token, const std::string& message);

    // Whether d is the only function of its name there (overloads: the entries found with it)
    // and no template.
    static bool onlyPlainCandidate(Decl* d, const std::vector<Entry>* overloads);

    // Whether e, walked (under parentheses and a `&`), names a function template without template
    // arguments, or several functions: with no target type to choose one by, C++ has no expression
    // there ([over.over]), so a requirement on it is not satisfied (`requires() { &T::f; }` with
    // T's f a member template it inherits: LuaBinder's luaDebugChildren). Nor is a non-static
    // member function an object's member names without calling it (`requires(T t) { t.f; }`).
    bool namesFunctionsWithoutTarget(Expr* e);

    // An argument naming several functions, alone or under `&` (`Expression::computeConstant` with
    // an overload for one constant and one for a Vector of them): C++ takes the overload whose type
    // the parameter names, once the callee is chosen, so meanwhile the argument decides nothing.
    struct OverloadSetArgument
    {
      size_t index;
      NameExpr* name;
      Expr* address; // the `&name` around it, or null
      Lookup set;
    };
    const std::vector<OverloadSetArgument>* callSets = nullptr; // the overload sets among the arguments of the call being typed

    // Whether a function of overload set `set` has exactly the function type target (what a
    // pointer to it as the argument needs: C++ [over.over]).
    bool setHasFunctionOf(const OverloadSetArgument& set, const Ty* target);

    // Whether candidate f (its class's and explicit arguments in s) can take each overload set
    // among the call's arguments: a parameter that is a pointer to a function or a member function
    // of a type no function of the set has makes it no candidate. One of a type still dependent
    // (`template<class F> add(int, F)`) is decided when the set is bound.
    bool takesCallSets(FunctionDecl* f, Subst& s);

    bool overloadSetArgument(Expr* a, size_t index, OverloadSetArgument& out);

    // `construct<Neg>`, a function template named with some of its arguments (or none), where a
    // pointer to a function of a known type is wanted: its other arguments, a trailing pack's
    // included, deduced from that type's parameters as from a call's, and the name typed as that
    // instance's function (which the code generator takes the address of).
    void bindTemplateAddress(Expr* a, const Ty* target);

    // The function template names among what initializes an object of type target, bound to the
    // function pointer types they initialize (bindTemplateAddress): through brace lists, an
    // InitializerList's elements, an array's and an aggregate's members in order
    // (`InitializerList<OperationTestCase<...>>{{"abs", constructExpression<Abs>, ...}}`).
    void bindTemplateAddresses(Expr* e, const Ty* target, int depth = 0);

    // Binds overload set s to its overload of the type that pt, what it initializes, takes: a function pointer's or
    // reference's function, or a member pointer's (C++ [over.over]). Several or none of that type leave it as it is.
    void bindOverloadSet(const OverloadSetArgument& s, const Ty* pt);

    // Binds each overload-set argument to the overload of the type the chosen callee's parameter
    // takes (fnTy: the callee's function type), as bindOverloadSet does.
    void bindOverloadSets(const std::vector<OverloadSetArgument>& sets, const Ty* fnTy);

    // A class value where a scalar is wanted, an argument for a parameter that is no class or a copy-initialized
    // variable's initializer (`takeInt(item)`, `Item* raw = item;` with a UniquePtr<Item>): C++ converts it only through
    // a conversion operator of the class. Without any it can take the checker's error, where the code generator stopped
    // at it as at a construct not supported yet. What the code generator takes is never one: a non-explicit conversion
    // operator to a type that is no class, an explicit one for a bool (chooseConversion), a lambda without captures as
    // a pointer to function. Only in code walked where it is written, not in a template's instance: there the walk's
    // candidate need not be the code generator's callee (a CRTP base's `triggered(x)`, which the derived class's
    // overload answers), and the code generator reports what it cannot convert, as before.
    // ambiguousOnly: an ambiguous conversion alone is the error (the chosen function's argument, which an
    // ambiguous conversion kept viable, C++ [over.best.ics] 10).
    void checkClassToScalar(const Ty* from, const Ty* to, uint32_t token, bool ambiguousOnly = false);
    // A construction of class t from args (a call of the class, a functional cast, `new T(args)`, a
    // variable's direct initialization) that no constructor takes, or takes ambiguously: an error, by
    // the rule codegen and the interpreter follow (chooseConstructor; design.md 24). Only in code
    // walked where it is written (not a template's instance), and not from a class with a conversion
    // operator (which may make the T the copy constructor takes): codegen still reports those.
    void checkConstruction(const Ty* t, Span<Expr*> args, uint32_t token);
    // C++20's parentheses for an array ([dcl.init.general] 16.5: `int a[3](1, 2)`, a member's `values(4, 5)`,
    // `new int[3](8, 9)`): each value copy-initializes an element, narrowing allowed and no braces elided, as
    // many values as elements at most, and the rest value-initialized. An error for what does not fit.
    void checkParenthesizedArray(Span<Expr*> args, const Ty* array, uint32_t token);

    // Each argument of a call of f (fnTy: its type), the single plain candidate whose parameters the arguments do not
    // fit (pickOverload chose none), that is a class where its parameter is a scalar, checked (checkClassToScalar). Not
    // for a function template's call, nor one with an expansion among its arguments.
    void checkScalarArguments(FunctionDecl* f, const Ty* fnTy, const Span<Expr*>& args, bool ambiguousOnly = false);
    // The same call's arguments for its reference parameters (checkReferenceBindings), f named by
    // callee if given (`sink(d)` of a class's operator()).
    void checkReferenceArguments(FunctionDecl* f, const Ty* fnTy, const Span<Expr*>& args, const std::string& callee = {});
    // A call's arguments for its reference parameters (paramTypes: the parameters' types, params:
    // their declarations, empty through a pointer to a function), callee its name in the message. Each
    // non-const lvalue reference must be given an lvalue of its type or of a class derived from it
    // ([dcl.init.ref] 5), never a value a temporary would hold (an int for a `double&`, "abc" for a
    // `String&`, `{1, 2}`), which the function would change in the argument's stead; an rvalue
    // reference never an lvalue of its type or of a class derived from it (5.4.4: move() says the
    // object may be moved from, the object itself never does). Either is an error.
    void checkReferenceBindings(const std::string& callee, Span<Decl*> params, Span<const Ty*> paramTypes, const Span<Expr*>& args);

    // Copy-initialisation of a class from a value of another type takes no explicit constructor
    // ([over.match.copy]): with `explicit Strict(int)`, `Strict s = 5;`, an argument 5 for a Strict
    // or const Strict& parameter, `return 5;` and `s = 5;` are errors, where `Strict s(5)` and
    // `Strict(5)` take it. Reported where the class takes the value by no implicit conversion (another
    // constructor, a conversion operator of the value's class) and its constructor for it is explicit.
    // Only in code walked where it is written, as checkClassToScalar.
    void checkExplicitCopy(const Ty* from, const Ty* to, uint32_t token);
    // `Strict s = {5};`, `return {5};`: copy-list-initialisation whose chosen constructor is explicit
    // ([over.match.list]).
    void checkExplicitListCopy(BraceInitExpr* b, const Ty* to);
    // `switch (flag)` on a class: it converts implicitly to an integer or an enum ([stmt.switch] 2, the conversion
    // codegen's chooseConversion takes; under --explicit-by-default a marked one, design.md "Explicit by default",
    // rule 4). A typed union switches on its tag.
    void checkSwitchOnClass(SwitchStmt* w);
    // Under --explicit-by-default, a list of two or more values copy-initializing a class no constructor takes, its
    // InitializerList constructor's for want of an element's conversion: each such conversion's error
    // (checkExplicitCopy), as C++'s `std::vector<Strict> v = {5, 6};` for an explicit Strict(int).
    void checkUnmarkedListElements(BraceInitExpr* b, const Ty* to);
    // Whether converting e (of type from) to type to in a brace initialization narrows it
    // ([dcl.init.list] 7): a floating value to an integer; a floating one to a floating type of
    // fewer bits, or an integer to a floating type, unless e is a constant the target holds (and,
    // for an integer, gives back); an integer to an integer type that cannot hold all its type's
    // values unless e is a constant that fits; a pointer to bool. why: what the message says. A
    // value the evaluator cannot tell is taken to fit unless e is surely no constant.
    bool narrows(Expr* e, const Ty* from, const Ty* to, std::string& why);
    // Whether e's value is surely no constant: it reads a parameter, a variable that is neither
    // const nor constexpr, a data member, or calls a function that is not constexpr.
    bool surelyNotConstant(const Expr* e, int depth = 0);
    // The elements of a brace list initializing an object of type to, each checked for narrowing
    // against what it initializes: a scalar's one element, an array's or an aggregate's elements in
    // order (a nested list the same way), an InitializerList's elements, the chosen constructor's
    // parameters; an error for each that narrows (a template's instance's too: `T{d}` with T int).
    // Not where brace elision leaves the targets unclear.
    void checkListNarrowing(Span<Expr*> elements, const Ty* to, int depth = 0);
    // Each argument of a call of f (fnTy: its type) for a class parameter, checked (checkExplicitCopy).
    void checkExplicitArguments(FunctionDecl* f, const Ty* fnTy, const Span<Expr*>& args);
    // --explicit-by-default (design.md "Explicit by default"), its errors. A constructor's or conversion
    // operator's plain `explicit` ("remove `explicit`") and `explicit(cond)` ("write `implicit(...)`, not
    // `explicit(cond)`", the condition turned round), at the word.
    void checkConversionMark(FunctionDecl* f);
    // A function argument's outermost braces that name no type, of a call of f (fnTy: its type): `draw({1, 0, 0})`,
    // "write `Color{1, 0, 0}`, not `{1, 0, 0}`", whatever they reach (a constructor, an aggregate, a container,
    // `{}`); not for an InitializerList parameter, nor one a template parameter would be deduced from (no C++
    // either). refused: set at the arguments reported.
    void checkBracedArguments(FunctionDecl* f, const Ty* fnTy, const Span<Expr*>& args, std::vector<bool>* refused = nullptr);
    // A construction's arguments of class t, as checkBracedArguments, by the constructor they choose.
    void checkBracedConstruction(const Ty* t, const Span<Expr*>& args);
    // The error of braced list b given for a parameter of type to (p: as declared, or null for an implicit
    // constructor's), naming the fix: `T{...}`, `nullptr` or the value for a pointer.
    void refuseBracedArgument(BraceInitExpr* b, const Ty* to, ParamDecl* p);
    // Whether braced list b, a function argument for a parameter of type to, must name its type: any but an
    // InitializerList parameter's (rule 1), and not the checker's own text lists.
    bool bracedArgumentNamesNoType(BraceInitExpr* b, const Ty* to);
    // The type a fix names for parameter p of type to (`Color` of `const Color& c`): as written, when it names
    // no template parameter and no member of a class unqualified (the call may be outside it); else a class of
    // no template arguments by its namespaces and classes; else empty (a printed type is not always kpp:
    // `int32`).
    std::string spelledParameterType(ParamDecl* p, const Ty* to);
    // A type as kpp code writes it (`Pair<BlueprintRecordID, f32>`, `geo::Point*`), or empty where it cannot
    // (a specialization's own arguments, a class template's member, a function type); and the namespaces and
    // classes before a name declared in d (`CircuitNetworkHelper::`), none where they cannot be written.
    std::string kppSpelling(const Ty* t, int depth = 0);
    std::optional<std::string> scopePath(Decl* d);
    // An explicit check's error: as reportHere, and under --explicit-by-default in template code too, a
    // template's own walk reporting it at once (the check's types are not dependent: every instance would say
    // the same), an instance's walk as that instance's.
    void reportExplicitError(uint32_t token, const std::string& message);

    // `NamedFunction(move(parameters), create, Expression::computeConstant)`: the constructor of
    // class type t the arguments choose, its parameters binding the overload sets among them.
    void bindOverloadSetsToConstructor(const std::vector<OverloadSetArgument>& sets, const Ty* t, Span<Expr*> args);

    // `EditForceGui gui(0, &EditorAction::SetForceFriend, ...)`, `new EditForceGui(...)`: the
    // overload sets among a construction's arguments, bound once the constructor is chosen.
    void bindOverloadSetsOfConstruction(const Ty* t, Span<Expr*> args);

    // `(ImU32)(255)`: a parenthesised name of a type before a parenthesised operand, which the
    // parser reads as a call (it cannot tell types from functions). C++ reads a cast. The name
    // inside, or null when it names no type.
    NameExpr* parenthesisedTypeCallee(Expr* e);

    // `(T)(x).m`, `(T)(x)[i]`, `(T)(x)(y)`: C++ casts the whole postfix expression, the parser read
    // the cast as a call the postfix applies to; said, not compiled as the other one.
    void checkPostfixAfterParenthesisedType(Expr* base, uint32_t token);

    // Whether this walk owns the nodes it types, so that it may rewrite them: an instance's copy, a
    // function that is no template, a global's initializer. A template's own nodes are shared
    // with the walks copying them for its instances (and with probes), which rewrite their own.
    bool ownsNodes() const;

    // A lambda whose body names a Text (or a closure capturing one) of the function around it: its
    // closure holds the Text, so it is valid during the call only (holdsText).
    void markTextCapture(LambdaExpr* l, size_t mark);

    // A Text's lifetime rule broken (design.md 14): reported where the program asks for it, the
    // request's site for an instance of the library's code (`later = [=] { ... }` making a Function
    // that allocates the closure).
    void reportTextError(uint32_t token, const std::string& message);
    // A closure capturing a Text given where another type is wanted (a Function, a class's
    // parameter): it would be kept as that type, past the call (design.md 14). As its own type it
    // goes on (a template's parameter, a local), the rule following it.
    void checkClosureTo(const Expr* value, const Ty* to, uint32_t token);
    static std::string textStored(const std::string& as);
    // The Text parameters of f: the last one, without a default, not from C; f's result holds none.
    void checkTextSignature(FunctionDecl* f);
    // A member function marked override must override a virtual function of a base (C++
    // [class.virtual]): an error where it overrides none, in a class or a class template's instance.
    void checkOverride(FunctionDecl* f);
    // A variable whose type is an abstract class (or an array of one): an error, as in C++.
    void checkAbstractObject(VariableDecl* v, const Ty* declared);
    // A variable holding a Text: a parameter's local copy only, never a data member or a global.
    void checkTextVariable(VariableDecl* v, const Ty* declared);

    // A call of f whose last parameter is a Text (design.md 14): the arguments from its position on
    // become one brace list typed Text (BraceInitExpr::text), which code generation and the
    // interpreter build the Text from; each must have an appendTo, an error naming its type
    // otherwise. A call given one Text there passes it on as it is.
    void packText(Span<Expr*>& args, FunctionDecl* f, uint32_t token);
    // The same for a call through a lambda or a function pointer whose type takes a Text.
    void packTextThrough(Span<Expr*>& args, const Ty* callee, uint32_t token);
    void packTextAt(Span<Expr*>& args, size_t k, uint32_t token);

    // A call of a consteval function, or of a consteval constructor converting an argument (C++20's
    // immediate invocation): a constant where it is written, evaluated here; an error naming the
    // function otherwise. Inside a consteval function's own body it is evaluated with the call of
    // that function (its parameters are constants there).
    FunctionDecl* initializersOf = nullptr; // the constructor whose member initializers are walked
    bool insideConsteval() const;
    // Whether an immediate evaluation fails here (why: the evaluator's reason); not when it needs a
    // class layout, which is code generation's to know: not decided here.
    bool immediateFails(const std::function<std::optional<ConstValue>()>& evaluate, std::string& why);
    void checkImmediate(FunctionDecl* f, uint32_t token, const std::function<std::optional<ConstValue>()>& evaluate);
    // An argument converted to its class parameter by a consteval constructor (`format("{}", x)`
    // with format's `FormatString(const char*)`, design.md 14): that conversion is an immediate
    // invocation.
    void checkImmediateConversion(Expr* arg, const Ty* param);

    // The piece count a call gives its Text parameter at k (a pack expansion its elements); SIZE_MAX
    // when it is not known here (the Text passed through, a pack's element in a pattern).
    size_t pieceCount(Span<Expr*> args, size_t k);

    // design.md 14: a parameter whose class has `consteval void checkPieces(ContainerSize pieces)
    // const`, right before a Text parameter (format's FormatString), is checked against the pieces
    // at every call: checkPieces runs on the argument's constant value with the call's piece count,
    // and a failure (a throw) is a compile error at the call. An argument that is no constant here
    // (a FormatString parameter passed on) is not checked: its own call was.
    void checkTextPieces(FunctionDecl* f, const Ty* ft, Span<Expr*> args);
    uint32_t checkPiecesSymbol = UINT32_MAX;

    // The constructor of cls a call with these arguments runs when one of cls's takes a Text, else null.
    FunctionDecl* textConstructor(const Ty* cls, Span<Expr*> args);

    const Ty* typeOfCall(CallExpr* c);

    // `x.appendTo(out)` on a built-in type or an enumeration (design.md 14: the member of every
    // type): an enumeration's own appendTo method if it has one, else the standard library's
    // `void appendTo(T value, String& out)` for the type, which the program does not write.
    bool isBuiltinAppendTo(const Ty* t, uint32_t symbol);
    // The call written as the library function's (`appendTo(x, out)`), so that code generation and
    // the interpreter see a plain call; typed as such. A walk that does not own the nodes (a
    // template's own) types it without rewriting: its instances rewrite their copies.
    const Ty* builtinMemberCall(CallExpr* c, MemberExpr* m, const Ty* base, const Ty* builtin);

    const Ty* typeOfCallUnpacked(CallExpr* c);

    // A call to a generic lambda: its template parameters are deduced from the arguments, its
    // body typed for them once per set of arguments (in the environment it was written in), and
    // the call has the result type found there. The deduced arguments are recorded on the call
    // for the code generator, as for function templates. Written arguments (`f.operator()<T>(x)`)
    // come first, the parameters deduce the rest.
    const Ty* genericLambdaCall(const Ty* t, CallExpr* c, const NamePart* written = nullptr);

    // Result of calling a value of type t: function pointers, lambdas, functors (written: the
    // template arguments of an explicit `operator()<...>`).
    const Ty* callResultOfValue(const Ty* t, CallExpr* c, const NamePart* written = nullptr);

    // --- operators ---

    const Ty* promote(const Ty* t);
    bool reversedSubscript(const Ty* b, const Ty* base); // an operand of type b (base, as written) before a pointer's `[`
    // An arithmetic operand e's type t, a bit-field's promoted by its width (Impl::bitFieldPromotion:
    // `u3 - 2` with an `unsigned u3 : 3` is an int); promote and arithmetic then do the rest.
    const Ty* operandType(Expr* e, const Ty* t);

    const Ty* arithmetic(const Ty* a, const Ty* b);

    // Overloaded operator lookup: member of the left operand's class, then free functions in
    // the class's namespace and the current scope chain.
    // The function `lhs op rhs` (or `op lhs`) calls: the class's member operator, or a free one
    // from the namespaces of both operand classes or the current scope, ranked together as C++
    // does ([over.match.oper]): the member, unless a free one takes the operands strictly better.
    // For a member, subst is the substitution of its class and isMember is set.
    FunctionDecl* operatorFunction(P op, const Ty* lhs, const Ty* rhs, uint32_t token, size_t arity, Subst& subst, bool& isMember);

    // Whether `lhs <=> rhs` is a defaulted operator<=>, which gives the class `==` as well.
    bool threeWayDefaulted(const Ty* lhs, const Ty* rhs, uint32_t token);

    // The `==` a defaulted operator<=> declares implicitly takes the class itself, so `alertType ==
    // AlertType::ALERTS_COUNT` converts the enumerator to an AlertType (whatever `<=>` the class
    // also has for it): whether lhs's class has it and rhs converts to the class.
    bool implicitEquality(const Ty* lhs, const Ty* rhs, uint32_t token);

    // Whether `lhs op rhs`, a comparison, is a rewritten candidate over operator f, which the lookup
    // found: `(lhs <=> rhs) op 0` or the reversed `0 op (rhs <=> lhs)` for a relational operator,
    // `!(lhs == rhs)` or `!(rhs == lhs)` for `!=`, `rhs == lhs` for `==`. One wins where it takes
    // the operands strictly better than f ([over.match.best] 2.1), or as well where f is a template's
    // specialization and it is not (2.4, before the non-rewritten candidate's tie-breaker, 2.8).
    bool rewrittenBeats(P op, FunctionDecl* f, const Subst& subst, bool member, const Ty* lhs, const Ty* rhs, uint32_t token);

    // The type of `lhs op rhs` (or `op lhs`) through an operator function, or for what every class
    // has; null when there is none (counted as unknown here only when countFailure). lhsOperand:
    // the left operand as an argument (an lvalue a reference), for a template's deduction.
    FunctionDecl* operatorChosen = nullptr; // overloadedOperator's operator function, null for none
    bool operatorIsMember = false;
    // overloadedOperator's decision (design.md 24): Function (operatorChosen), Rewritten, Builtin, or
    // Undecided when it found nothing.
    OperatorCall operatorCall = OperatorCall::Undecided;
    const Ty* overloadedOperator(P op, const Ty* lhs, const Ty* rhs, uint32_t token, size_t arity, bool countFailure = true, const Ty* lhsOperand = nullptr);

    static bool isAssignmentOp(P op);
    // Why C++ defines the implicit assignment of cls as deleted ([class.copy.assign] 2, 4, 7), or
    // nothing: a const or reference non-static data member (an array of const elements too), a base
    // or member whose class's assignment is deleted, or one that declares a move constructor or a
    // move assignment and so has a deleted copy assignment (cls itself too: the walk found no
    // operator= of its own to take the operand). move: an assignment from an rvalue, which a class
    // whose move assignment is declared or implicit moves by, and copies by where that one is
    // implicit or defaulted and C++ defines it as deleted. level 0 is the assigned class; above it,
    // prefix names the base or member in the reason (`its member f's member c is const`).
    // defaulted: cls's own operator= the walk calls is defaulted, and what it assigns is checked.
    std::string deletedAssignment(const Ty* cls, bool move, int level, const std::string& prefix, bool defaulted = false);
    // Why C++ defines cls's defaulted operator= f (`S& operator=(const S&) = default;`, the one an
    // assignment calls) as deleted, as it would the implicit one ([class.copy.assign] 7), or nothing.
    // A defaulted move assignment defined as deleted leaves the rvalue to the copy assignment.
    std::string deletedDefaultedAssignment(const Ty* cls, FunctionDecl* f);
    // A requirement's probe of `a = b` that no operator function takes (l: a's type without its reference; lhsOperand:
    // a as an argument, a reference when an lvalue, or null): whether C++ allows it. A scalar must be a modifiable
    // lvalue. A class assigns by its implicit copy or move assignment ([class.copy.assign]): none for a const object,
    // none when it declares a copy or move assignment of its own (none of which took b), and only for a b that
    // converts to the class implicitly and when C++ does not define it as deleted (deletedAssignment). True while
    // either type is not known.
    bool probedAssignment(const Ty* l, const Ty* lhsOperand, const Ty* rhs);
    // A requirement's probe of `a = b` of a scalar a (target): whether b (value, of type given) converts to a's type
    // ([expr.ass] 3), as an argument would: `requires(int*& p, int i) { p = i; }` is not met. A literal (`p = 0`, a null
    // pointer constant), a braced list, a class's value (its conversion operators) and a function pointer target (an
    // overload set's address) are not decided here: true.
    bool probedScalarConversion(Expr* value, const Ty* given, const Ty* target);

    // C++'s built-in candidates for an operator no function defines: a class operand takes part
    // through its conversion operator to a non-class type (`area < smallBlueprintAreaSize` with a
    // Const<float>). The operand's converted type, or null when it has no such conversion; an
    // operand that is no class stays as it is.
    const Ty* builtinOperand(const Ty* t);

    // A user-defined operator on an enumeration operand (`Flags operator|(Flags, Flags)`) beats the
    // built-in one, which a scoped enum does not even have. Only a free non-template function whose
    // parameters take the enum operands as they are (not through a conversion to some class).
    const Ty* enumOperator(P op, const Ty* lhs, const Ty* rhs, uint32_t token, FunctionDecl** chosen = nullptr);
    // The same for a unary operator on an enumeration operand (`!flags`, `~flags`, `-p`, `++d`;
    // postfix `d++` the `operator++(E&, int)`), which C++ calls where it is declared and kpp took
    // as the built-in operator (language/enum_operand_calls_user_defined_operator).
    const Ty* enumUnaryOperator(P op, bool postfix, const Ty* operand, uint32_t token, FunctionDecl** chosen = nullptr);

    // Two rules of design.md 14 on text. A string literal plus or minus an integer is C++'s pointer
    // arithmetic, never text: an error (`&"abc"[1]` writes the pointer). A `<<` chain into a String
    // mixed with `&`, `|` or `^` without parentheses binds as `(out << a) & b`: an error.
    static Expr* unparenthesized(Expr* e);
    void checkTextOperators(BinaryExpr* b, P op, const Ty* l, const Ty* r);

    // The type of `lhs op rhs`, and what the operator calls recorded on b (design.md 24).
    const Ty* typeOfBinary(BinaryExpr* b);

    // Records on an operator's node what it calls: call as the class branch decided it, else the
    // built-in operator when both operands are known and not dependent (a template's own walk leaves
    // its dependent operators undecided). A probe of shared nodes records nothing.
    template<class E>
    void recordOperatorCall(E* e, const Ty* t, Expr* lhs, Expr* rhs, OperatorCall call, FunctionDecl* callee, bool member)
    {
      if (probesSharedNodes())
        return;
      if (call == OperatorCall::Undecided && t && t->isKnown() && !types.isDependent(t))
      {
        auto plain = [&](Expr* x) { return !x || (x->ty && x->ty->isKnown() && !types.isDependent(x->ty)); };
        if (plain(lhs) && plain(rhs))
          call = OperatorCall::Builtin;
      }
      e->call = call;
      e->callee = call == OperatorCall::Function ? callee : nullptr;
      e->calleeMember = call == OperatorCall::Function && member;
    }

    const Ty* typeOfBinaryOperator(BinaryExpr* b, OperatorCall& call, FunctionDecl*& callee, bool& member);

    // `forward<Args>(args)...` under an instantiation: the pattern typed once per element with the
    // packs bound to that element, as C++ instantiates an expansion; its type the pack of the
    // results (Args = (int, String&): (int&&, String&)). Null when the pattern names no pack
    // parameter bound to a pack here.
    const Ty* typeOfExpansion(UnaryExpr* u);

    // The type of `op operand`, and what the operator calls recorded on u (design.md 24).
    const Ty* typeOfUnary(UnaryExpr* u);

    const Ty* typeOfUnaryOperator(UnaryExpr* u, OperatorCall& call, FunctionDecl*& callee, bool& member);

    // --- literals ---

    const Ty* typeOfLiteral(LiteralExpr* l);

    // --- element type for range-for ---

    // The iterator type of the free begin(range) found for a class without a begin() member (in
    // the class's namespace or from here), the call a range-for makes then; null if none.
    const Ty* freeBeginType(const Ty* t, Expr* rangeExpr);
    // The free function `symbol(range)` a range-for over class t calls (begin or end), or null.
    FunctionDecl* freeRangeFunction(const Ty* range, uint32_t symbol);
    // Records on f the begin and end a range-for over class range calls (design.md 24).
    void recordRangeFunctions(RangeForStmt* f, const Ty* range);

    // A data member declared as an array without a length (`int items[];`: a flexible array
    // member), as e names it (`s->items`, `items` in a member function), or a lone name in sizeof's
    // type names it; else null. (`extern int table[];` takes its length from its definition.)
    VariableDecl* unboundedArray(Node* e);

    // The variable declared const (a local, a parameter by value, a global or a static) whose
    // object, or a member or element of it, a placement new's place visibly names: `&limit`,
    // `&limit.x`, `&table[i]`, `table` (an array); else null. Through a pointer (`p->x`, `*p`, a
    // cast, const_cast included) nothing is visible (design.md 21).
    Decl* constObjectPlaced(Expr* place);

    // That f, not noexcept, overrides no noexcept function (an error).
    void checkOverrideNoexcept(FunctionDecl* f);
    void checkCaseEnds(Stmt* body);
    // A switch's body under --no-implicit-fallthrough: each group of statements after a label a scope of its own
    // (design.md "No implicit fallthrough", rule 4); else as any block.
    void walkSwitchBody(Stmt* body);
    // `fallthrough;`, a label: an error under --no-implicit-fallthrough where checkCaseEnds did not place it.
    void checkPlacedInSwitch(Stmt* s);
    void checkDestructorNoexcept(ClassDecl* c);
    void checkDestructorRedeclared(FunctionDecl* f);
    // Whether e can throw, for the noexcept operator ([expr.unary.noexcept]): a call of a function
    // not noexcept (its noexcept condition evaluated under the walk's substitution), a throw, a new
    // that allocates; scalar arithmetic, names, literals and the operators of scalars cannot. What it
    // does not decide yet (an overloaded operator, a constructor, a call through a pointer) is named
    // in unsupported, an error, never a guess.
    bool canThrow(Expr* e, std::string& unsupported);
    // What an expression calls without naming it ([except.spec] 6), for canThrow.
    bool functionThrows(FunctionDecl* f, const Subst* subst, std::string& unsupported);
    bool destroyingThrows(const Ty* t, std::string& unsupported);
    bool constructingThrows(const Ty* t, const std::vector<const Ty*>& argTypes, std::string& unsupported, int depth = 0);
    bool defaultMembersThrow(const Ty* t, size_t given, std::string& unsupported, int depth);
    bool convertingThrows(Expr* from, const Ty* to, bool explicitCast, std::string& unsupported);
    bool callArgumentsThrow(FunctionDecl* f, const Ty* signature, const Subst* subst, Span<Expr*> args, std::string& unsupported);

    // kpp requires nullptr (kovarex, 2026-11-04; design.md 4): e, the literal 0 of kpp's own sources (in
    // parentheses or not), made a pointer or member pointer of type target without a cast, is an error
    // at the literal's place.
    void noteZeroNull(Expr* e, const Ty* target);

    // rangeExpr: the range written, for a free begin(range).
    const Ty* elementTypeOf(const Ty* range, Expr* rangeExpr);

    // --- generic walking ---

    void walkNode(Node* n);

    void walkType(Type* t);

    // A declaration's type that names a variable (`a * b;`, `a b;`, a parameter `Box box(four)` at
    // namespace scope): an error (design.md 22), never the variable's type, which is the reading only
    // sizeof's operand has (`sizeof(prefixes)`).
    void checkTypeNamesNoVariable(Node* base, uint32_t token);

    void walkParam(ParamDecl* p);

    void pushTemplateParams(Span<Decl*> params);

    // Scope in which the body of a (possibly out-of-line) member is resolved.
    Scope* semanticScopeFor(const QualifiedName& name);

    // A probe of a requires-expression under arguments walks the template's own nodes, which other
    // walks share: nothing it finds is cached on them. (An instance's walk has its own copy.)
    bool probesSharedNodes() const { return probing && activeSubst; }

    // A template's own walk: a template parameter in scope that this walk binds to nothing.
    // Whether a function's signature is typed by its walk: a decltype or a deduced auto in its
    // return type or a parameter's.
    static bool signatureFromWalk(FunctionDecl* f);

    bool inGenericTemplate() const;

    // Whether d is in an instance of a function template that an explicit specialisation replaces
    // for these arguments (`template<> SpriteDrawOrder* DrawOrderProvider::allocate()`): that body
    // is not the one the program runs.
    bool replacedBySpecialization(Decl* d);

    // The instance this walk types, for messages: `T = int32, N = 3`; empty outside templates.
    std::string instanceDescription() const;

    const Ty* computeThisType();

    void walkDecls(Span<Decl*> decls);

    void walkDecl(Decl* d);

    // Declarations inside function bodies: add their names to the local stack.
    void declareLocal(Decl* d);

    // `auto [a, b] = e;` of a class E whose TupleSize<E> (the library's std::tuple_size) has a
    // member value binds tuple-like (design.md 4, structured bindings): binding i names a hidden
    // `auto&& a = get<i>(object)`, or `object.get<i>()` when E has a member template get whose
    // first parameter is a constant, the object passed as an lvalue. The binding's own name node
    // in v's list records that variable (resolved), for declareLocal and the code generator. Any
    // other object (an array, a class without TupleSize<E>::value) binds member by member
    // (bindingType).
    void tupleBindings(VariableDecl* v, const Ty* object);

    // A `when` declaration's condition as written (design.md 19), for a message: its tokens from the first
    // to the `)` that closes `when (`.
    std::string conditionText(Decl* d);

    // Where the walk is, for a message about what exists: "here", or which instance.
    std::string whenWhere();

    // A use of a local a `when` declares (design.md 19), found by lookup as found (its block's last
    // declaration of the name), checked in the configuration and instance being built: the declaration
    // the use names, or null after an error, or where this walk cannot tell (undecided: a template's own
    // walk, a condition on its parameters, of several declarations). The candidates are the declarations
    // of the name in found's block, a plain one existing always. The use names the one that exists here;
    // none existing is an error here, as a `when` that does not exist still hides an outer name (never a
    // fallback to it); two existing are the clash, reported where the later is declared. A use inside a
    // discarded branch is not walked, as any discarded code.
    Decl* whenUse(uint32_t symbol, uint32_t token, Decl* found, bool& undecided);

    // A lookup that found a local a `when` declares (design.md 19): its declaration made the one the use
    // names (whenUse); false after an error, or where this walk cannot tell which.
    bool whenLookup(Lookup& l, NameExpr* n);

    // An unqualified use, in a member of a class template's instance, of a name that several `when` data members
    // of the class declare (design.md 19): the instance's own, whose condition holds under its arguments, as memberOf
    // takes it for `object.name`; none or two is an error there. As before where this walk does not bind the class's
    // arguments or the name was found in a base.
    bool whenMemberLookup(Lookup& l, NameExpr* n);

    // A condition made of config constants and literals (names in the `config` namespace, already
    // resolved: a walked condition): the same in every instance of a template, so its own walk decides it
    // as a function that is no template does (design.md 7.4 and 19).
    bool configCondition(Node* n, int depth = 0);

    // `when (condition)` on a local (design.md 19): decided as if constexpr's condition is, in an
    // instance and in a function that is no template; false, the variable does not exist: nothing of
    // it is checked or instantiated, as a discarded branch (walkLocalDecl declares it absent). A
    // template's own walk decides a condition of config constants and literals so too, and checks any
    // other as an undecided branch. True when there is no condition.
    bool whenHolds(Decl* d);

    void walkWhenCondition(Decl* d);
    void walkLocalDecl(Decl* d);

    void walkStmt(Stmt* s);

    void walkExprs(Span<Expr*> exprs);

    // Resolves names in and computes the type of an expression.
    const Ty* walkExpr(Expr* e);
  };

  // The call operator of a generic lambda for one set of its own template arguments: a copy of
  // the lambda's parameters and body (InstanceCopier::lambdaInstance) typed in the environment the
  // lambda was written in, once per (lambda type, arguments) and kept; body gets the copy.
  // Returns the operator's function type (parameters and result), or null when the environment
  // is unknown, the result has no type, or the lambda is asked for while it is being typed.
  const Ty* checkLambdaInstance(const Ty* lambdaTy, const Subst& own, Subst* fullOut, ResolveStats* statsInto, LambdaExpr** body = nullptr)
  {
    LambdaEnv env;
    {
      std::lock_guard lock(lambdaEnvMutex);
      auto it = lambdaEnvs.find(lambdaTy);
      if (it == lambdaEnvs.end())
        return nullptr;
      env = it->second;
    }
    LambdaExpr* original = env.lambda;
    if (!original || !env.tokens || env.depth > 16)
      return nullptr;
    Subst full = env.subst;
    for (const Subst::Pair& p : own.pairs)
      if (!full.find(p.param))
        full.pairs.push_back(p);
    if (fullOut)
      *fullOut = full;
    bool producer;
    InstanceRecord* inst = claimInstance(original->closure, own, lambdaTy, producer);
    std::optional<RecordInto> into; // what typing the instance reads is its own
    if (producer)
      into.emplace(&inst->deps);
    if (!producer)
    {
      if (inst->state.load(std::memory_order_acquire) != 2)
      {
        if (inst->owner == std::this_thread::get_id())
          return nullptr; // the lambda calling itself through a capture
        ++instWaits;
        for (int seen = inst->state.load(std::memory_order_acquire); seen != 2; seen = inst->state.load(std::memory_order_acquire))
          inst->state.wait(seen, std::memory_order_acquire); // blocks (C++20 atomic wait) instead of spinning on yield
      }
      if (body)
        *body = inst->lambda;
      return inst->deducedReturn; // for a lambda: the call operator's type for these arguments
    }
    if (recordingDependencies)
    {
      // How to check it again (recheckInstancesOf): its closure type and own arguments.
      inst->recheck = std::make_unique<InstanceRecord::Recheck>();
      inst->recheck->lambdaTy = lambdaTy;
      inst->recheck->subst = own;
    }
    return runLambdaInstance(inst, env, full, statsInto, body);
  }

  // A generic lambda's call operator for the arguments full: its body copied and walked into inst.
  const Ty* runLambdaInstance(InstanceRecord* inst, const LambdaEnv& env, const Subst& full, ResolveStats* statsInto, LambdaExpr** body = nullptr)
  {
    LambdaExpr* original = env.lambda;
    LambdaExpr* l = InstanceCopier(instanceArena()).lambdaInstance(original);
    inst->lambda = l;
    ++instBodies;
    inst->counted |= 1;
    ResolveStats local;
    if (statsInto)
    {
      local.fileIndex = env.file ? env.file->index : statsInto->fileIndex; // the lambda's file's tokens are what it records
      local.collectWhy = statsInto->collectWhy;
    }
    Walker sub(*this, local, *env.tokens, env.text);
    sub.file = env.file;
    FileOfLookups walkedFile(env.file);
    sub.scope = env.scope;
    sub.thisType = env.thisType;
    sub.activeSubst = &full;
    sub.instantiationDepth = env.depth + 1;
    sub.locals.items = env.locals;
    for (const LambdaCapture& cap : l->captures)
      if (cap.decl)
        sub.walkLocalDecl(cap.decl);
    sub.pushTemplateParams(l->templateParams);
    std::vector<const Ty*> params;
    for (Decl* p : l->params)
    {
      sub.walkParam(static_cast<ParamDecl*>(p));
      const Ty* pt = declaredTypeOf(p, &full, 0);
      if (!(static_cast<ParamDecl*>(p)->isPack && spliceIfPack(pt, params)))
        params.push_back(pt);
    }
    sub.walkType(l->returnType);
    const Ty* result = nullptr;
    if (l->returnType)
      result = sub.typeOf(l->returnType);
    sub.returnFrames.push_back(nullptr);
    sub.walkStmt(l->body);
    const Ty* deduced = sub.returnFrames.back();
    sub.returnFrames.pop_back();
    if (!result)
      result = deduced ? types.decay(deduced) : types.voidType();
    local.instExprsTyped += local.exprsTyped;
    local.instExprsDependent += local.exprsDependent;
    local.instExprsUnknown += local.exprsUnknown;
    addInstanceStats(local, inst); // counted once, whoever asked (instanceStatsTotal)
    const Ty* callTy = nullptr;
    bool known = result->isKnown() && !types.isDependent(result);
    for (const Ty* pt : params)
      known = known && pt && pt->isKnown();
    if (known)
      callTy = types.function(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, false);
    inst->deducedReturn = callTy;
    inst->state.store(2, std::memory_order_release);
    inst->state.notify_all();
    if (body)
      *body = l;
    return callTy;
  }

  // Types l, a copy of a generic lambda's parameters and body (GlobalScope::privateLambdaInstance),
  // for full in the environment the lambda was written in, as runLambdaInstance types an instance's
  // own copy; element: again, for one element of a pack bound (its elementPacks and index, the
  // `if constexpr` branches the whole instance took kept when keepBranches), as typeFunctionFor
  // retypes a function's copy (GlobalScope::retypeLambdaFor).
  void typeLambdaCopy(LambdaExpr* l, const LambdaEnv& env, const Subst& full, const TemplateArgs* element, bool keepBranches)
  {
    ResolveStats scratch;
    Walker sub(*this, scratch, *env.tokens, env.text);
    sub.file = env.file;
    FileOfLookups walkedFile(env.file);
    sub.scope = env.scope;
    sub.thisType = env.thisType;
    sub.activeSubst = &full;
    sub.instantiationDepth = env.depth + 1;
    sub.locals.items = env.locals;
    if (element)
    {
      sub.retypingElements = true;
      sub.keepConstexprBranches = keepBranches;
      if (!element->elementPacks.empty())
      {
        sub.elementPacks = &element->elementPacks;
        sub.elementIndex = element->elementIndex;
      }
    }
    for (const LambdaCapture& cap : l->captures)
      if (cap.decl)
        sub.walkLocalDecl(cap.decl);
    sub.pushTemplateParams(l->templateParams);
    for (Decl* p : l->params)
      sub.walkParam(static_cast<ParamDecl*>(p));
    sub.walkType(l->returnType);
    sub.returnFrames.push_back(nullptr);
    sub.walkStmt(l->body);
    sub.returnFrames.pop_back();
  }

  // The environment a generic lambda of type lambdaTy was written in (its enclosing walk's), or
  // false when none is known.
  bool lambdaEnvOf(const Ty* lambdaTy, LambdaEnv& out)
  {
    std::lock_guard lock(lambdaEnvMutex);
    auto it = lambdaEnvs.find(lambdaTy);
    if (it == lambdaEnvs.end() || !it->second.tokens)
      return false;
    out = it->second;
    return true;
  }

  // The deduced return type of an auto-returning function template for one instantiation,
  // checking its body now if no call has yet.
  const Ty* deducedReturnOf(FunctionDecl* f, const Subst& subst)
  {
    if (!f->file || !f->file->tokens)
      return nullptr;
    ResolveStats scratch;
    Walker w(*this, scratch, *f->file->tokens, f->file->text);
    for (Decl* p = f; p; p = p->parent)
      for (Decl* tp : p->templateParams)
        w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
    w.scope = lexicalScopeOf(f);
    if (f->name.parts.size() > 1)
      w.scope = w.semanticScopeFor(f->name);
    return w.instantiateFunction(f, subst, true);
  }

  // The body a constant evaluation runs for def (a function's definition) under subst: the copy
  // its instance's walk typed, made now or waited for, through the record the checker's instance
  // checks use, never def's own nodes, which its file's walk may not have reached yet (kpp's order
  // independence: a constant above the function, or in a file walked first) or may be writing on
  // another thread meanwhile. For a template's function (its own, or its class's) the instance's;
  // there a call with a type-dependent argument names what the instance's types choose (C++
  // [temp.dep.candidate]). For a plain function the one walk of it with no arguments, which an
  // `auto` function's return deduced before its file's walk shares. def itself when an argument is
  // not known, or this thread is walking the copy (a call in its own body).
  FunctionDecl* constInstanceBody(FunctionDecl* def, const Subst* subst)
  {
    if (!def->body || !def->file || !def->file->tokens)
      return def;
    Decl* cls = owningClassOf(def);
    Span<Decl*> classParams = cls ? effectiveParams(cls) : Span<Decl*>{};
    if (!subst && (!def->templateParams.empty() || !classParams.empty()))
      return def;
    Subst classSubst, own;
    for (Decl* p : classParams)
    {
      const Ty* t = subst->find(p);
      if (!t)
        return def;
      classSubst.pairs.push_back({p, t});
    }
    // An out-of-line member's own copy of its class's parameter list takes the class's arguments
    // in order, as the class's check has it (checkMemberBody).
    bool outOfLine = cls && def->parent != cls && def->templateParams.size() == cls->templateParams.size();
    for (size_t i = 0; i < def->templateParams.size(); ++i)
    {
      const Ty* t = outOfLine ? classSubst.find(cls->templateParams[i]) : subst->find(def->templateParams[i]);
      if (!t)
        return def;
      own.pairs.push_back({def->templateParams[i], t});
    }
    ResolveStats scratch;
    Walker w(*this, scratch, *def->file->tokens, def->file->text);
    for (Decl* p = def; p; p = p->parent)
      for (Decl* tp : p->templateParams)
        w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
    w.scope = lexicalScopeOf(def);
    if (def->name.parts.size() > 1)
      w.scope = w.semanticScopeFor(def->name);
    InstanceRecord* record = nullptr;
    w.instantiateFunction(def, own, true, classSubst.empty() ? nullptr : &classSubst, nullptr, &record);
    if (!record || record->state.load(std::memory_order_acquire) != 2 || !record->body)
      return def;
    return record->body;
  }

  // The initializer a constant evaluation reads for v (a namespace-scope or static constant's
  // definition, a trait's static value, a data member's default) under subst (the arguments of the
  // class template it is a member of, or none): a copy of it as a walk of its own typed it, made now
  // or waited for, through the kind of record GlobalScope::instanceInitializer keeps for variable
  // templates; never v's own nodes, which its file's walk may not have reached yet or may be writing
  // on another thread (a constant of another file read the overload set's first function, an array's
  // bound 100 or 4 by the order the files were walked in). v->init itself when an argument is not
  // known, or this thread is typing the copy.
  Expr* constInitializerOf(VariableDecl* v, const Subst* subst)
  {
    if (!v->init)
      return nullptr;
    Scope* scope = lexicalScopeOf(v);
    if (v->name.parts.size() > 1)
    {
      ResolveStats scratch;
      if (v->file && v->file->tokens)
        scope = Walker(*this, scratch, *v->file->tokens, v->file->text).semanticScopeFor(v->name);
    }
    return constWalkedCopy(v, v->init, v->file, scope, v, subst);
  }

  // e (an expression of key's: a constant's initializer, an enumerator's value) as constInitializerOf
  // has it: a copy typed by a walk of its own in scope, with the template parameters of paramsOf and
  // its parents, under subst; the record of key keeps it.
  Expr* constWalkedCopy(const Decl* key, Expr* e, const ParsedFile* file, Scope* scope, Decl* paramsOf, const Subst* subst)
  {
    if (!e || !file || !file->tokens)
      return e;
    Subst s;
    if (subst)
      for (const Subst::Pair& p : subst->pairs)
      {
        if (!p.type || !p.type->isKnown() || types.isDependent(p.type) || mentionsUnknown(p.type))
          return e;
        s.pairs.push_back(p);
      }
    bool producer;
    InstanceRecord* record = claimInstance(key, s, nullptr, producer);
    if (!producer)
    {
      if (record->state.load(std::memory_order_acquire) != 2)
      {
        if (record->owner == std::this_thread::get_id())
          return e; // (its own expression, while this thread types the copy)
        for (int seen = record->state.load(std::memory_order_acquire); seen != 2; seen = record->state.load(std::memory_order_acquire))
          record->state.wait(seen, std::memory_order_acquire);
      }
      return record->init ? record->init : e;
    }
    RecordInto into(&record->deps); // what typing the copy reads is its own
    InstanceCopier copier(instanceArena());
    // With no arguments, the copy is the plain walk's expression: its lambdas are the types that
    // walk gives them (as a function's no-argument instance's are, runFunctionInstance), so that a
    // namespace-scope `auto` variable initialized by a lambda has its own closure type wherever it
    // is asked for (computeDeclaredType).
    copier.keepClosures = s.empty();
    Expr* copy = copier.expression(e);
    record->init = copy;
    ResolveStats scratch;
    Walker w(*this, scratch, *file->tokens, file->text);
    for (Decl* p = paramsOf; p; p = p->parent)
      for (Decl* tp : p->templateParams)
        w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
    w.scope = scope;
    if (!s.empty())
      w.activeSubst = &s;
    if (Decl* cls = enclosingClassDecl(w.scope))
      w.thisType = types.pointer(s.empty() ? selfType(cls) : substitute(selfType(cls), s));
    w.walkExpr(copy);
    record->state.store(2, std::memory_order_release);
    record->state.notify_all();
    return copy;
  }

