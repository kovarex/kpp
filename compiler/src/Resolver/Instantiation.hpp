// GlobalScope::Impl, templates: class arguments, substitutions, declared types, deduction: part of the class's body,
// included in it in GlobalScopeImpl.hpp, its definitions in Instantiation.cpp (resolver.cpp split, 2026-11-05, moved without
// a change; docs/design.md, "Compilation model").
  static Decl* enclosingClassDecl(Scope* s);

  static size_t enumeratorCount(Decl* e);

  // Type of a class or enum declaration as written inside it: template parameters stay dependent.
  // The template parameters a class is instantiated with: its own, or, for a class nested in a
  // class template (`Holder<T>::Node`), the enclosing template's, or, for a class template nested
  // in one (`Heap<V, Tag>::IteratorBase<Value>`), the enclosing template's and then its own.
  static Span<Decl*> effectiveParams(Decl* cls);

  // How many of a class's effectiveParams are an enclosing template's, ahead of its own: those of
  // `Heap<V, Tag>` for its member template IteratorBase<Value>; 0 for any class that is not a
  // template declared in a class template.
  static size_t enclosingParamCount(Decl* cls);

  // The type of an enum declaration. One declared in a class template is a type of each instance
  // (`Flag<A>::Enum` is not `Flag<B>::Enum`), so it carries the enclosing template's arguments as
  // `subst` binds them (its parameters where it binds none: the enum as the template writes it).
  const Ty* enumTypeIn(Decl* e, const Subst* subst);

  // A typed union's tag type (`Property::Tag`) for the union type `of`: with that instance's
  // arguments when the union is declared in a class template.
  const Ty* tagTypeOf(ClassDecl* u, const Ty* of);

  const Ty* selfType(Decl* d);
  // `this` in a member function called on an object of type objectType: a pointer to it, in an
  // enumeration's method the value (a reference to it).
  const Ty* thisTypeOf(const Ty* objectType);

  // Maps the template parameters of `cls` to the arguments of an instantiation type.
  Subst substFor(const Ty* classTy);

  // The member type named symbol of class type t, with t's arguments (or those of the base it is
  // declared in); null when t has none. A base that is a template parameter (`template<class T>
  // struct Const : T` over a Map) is searched as the class it is for t: its scope has no such base.
  const Ty* memberTypeOf(const Ty* t, uint32_t symbol, int depth);

  // The instantiation of the base class whose scope is `in` that class type t (scope cls)
  // derives from (`Prototype<ItemID>` for ItemPrototype and Prototype's scope), t itself when in
  // is cls; null when in is no base of it.
  const Ty* instantiationOfBase(const Ty* t, Scope* cls, Scope* in, int depth);

  // l, the lookup of symbol in the scope of class type t (t's arguments known), made right for
  // t's bases as its arguments pick them: a member found in a base class template's primary
  // scope (the static lookup's bases) is the member of the specialisation t's base is, when it is
  // one (`template<int N> struct G : E<N>` with `template<> struct E<0>`: G<0>'s `type` is
  // E<0>'s); a member the static lookup did not find is looked for in t's base instances
  // (`template<int N> struct M : M<N - 1>`, whose own scope has no base, and M<0> a
  // specialisation: M<1>'s `type` is M<0>'s). Unchanged otherwise.
  void lookupThroughBaseInstances(const Ty* t, uint32_t symbol, Lookup& l, int depth);
  // The lookup of symbol in t's base instances (lookupThroughBaseInstances applied to each): the
  // first that has it, ambiguousWith set when another has another member of the name; false when
  // none does.
  bool lookupInBaseInstances(const Ty* t, Scope* cls, uint32_t symbol, Lookup& out, int depth);

  // Every instantiation of the base class whose scope is `in` that class type t (scope cls) is or
  // derives from, directly or not, each once: Holder<2, int>, Holder<1, double> and Holder<0,
  // String> of a tuple built from such bases.
  void instantiationsOfBase(const Ty* t, Scope* cls, Scope* in, int depth, std::vector<const Ty*>& out);

  // The class whose own non-static data members `auto [a, b] = e` binds for class type t: t, or
  // the one base (direct or not) that declares them when t declares none, as C++ has it (Vector2D's
  // x and y in its base Vector2<double, Vector2D>); null when no class or several do.
  const Ty* bindingMembersClass(const Ty* t, int depth = 0);

  // The classes a base written as b names under ctx: itself, or each element of the pack a base
  // like `Ts...` expands to (`struct Overload : Ts...` with Ts = (F, G) has the bases F and G).
  std::vector<const Ty*> baseInstances(Type* b, const TypeContext& ctx, int depth);

  // A constant argument has its parameter's type, read with the arguments before it (`template<
  // class T, T V>`: V's type is the first argument), so that `Constant<bool, true>` and
  // `Constant<bool, isSigned<T>>` with a signed T are one class whichever way the constant was
  // computed (a literal is typed by the parameter's type unbound, a trait as bool).
  void normalizeValueArgs(Span<Decl*> params, std::vector<const Ty*>& args, Decl* def, int depth);

  // Whether class type t (scope cls) is, or derives from, the class instance `base` exactly,
  // through any of its bases as their arguments make them.
  bool derivesFromInstance(const Ty* t, Scope* cls, const Ty* base, int depth);

  // The substitution a member found in scope foundIn sees through class type t: that of the
  // base instantiation the member is declared in (`ItemPrototype::IDType` with `using IDType =
  // IDTypeParameter;` in the base Prototype<ItemID>), else t's own.
  Subst memberSubst(const Ty* t, Scope* foundIn, int depth);

  // A name written unqualified inside a class and found in the scope of one of its base classes
  // (`const_iterator(*this)` in a member of ParticleSurface : InfiniteRectangle<ParticleChunk*>,
  // the iterator a nested class of the base template): that base's substitution, so the name has
  // its arguments; empty when the name was found elsewhere or the base is no template.
  Subst baseSubstForUnqualified(const Lookup& l, const TypeContext& ctx, int depth);

  // For `Vector<int>::iterator`: the substitution of the instantiation the qualifier names
  // (its innermost part with template arguments), or of the base the member was found in
  // (foundIn); empty when there is none. qualifierOut: the qualifier's type, when it was converted.
  Subst qualifierSubst(const QualifiedName& name, const TypeContext& ctx, int depth, Scope* foundIn = nullptr, const Ty** qualifierOut = nullptr);

  // A written template argument: a type, or a constant expression for a non-type parameter
  // (a Value type of the parameter's declared type), or unknown.
  // A function named as a constant template argument, for a parameter `float (*func)(float,
  // float)`: the function value (TypeTable::functionValue) of the overload whose type the
  // parameter points to; unknown when none fits or the parameter takes no function.
  const Ty* functionArgument(const Lookup& l, Decl* param, int depth);

  const Ty* templateArgument(Node* arg, Decl* param, const TypeContext& ctx, int depth);
  // A class object's value as a template argument (TypeTable::objectValue) and back: its parts
  // integers (bool, char, integer, enumeration) and objects of such parts; null (nullopt) for any
  // other part (a floating value, a pointer, a reference), which no argument holds yet.
  const Ty* objectArgument(const ConstValue& v, int depth = 0);
  std::optional<ConstValue> argumentObject(const Ty* t, int depth = 0);

  uint32_t functionSymbol_ = UINT32_MAX;
  uint32_t functionSymbolId();
  uint32_t functionImplSymbol_ = UINT32_MAX;
  uint32_t initializerListSymbol_ = UINT32_MAX;
  uint32_t functionImplSymbolId();
  // The library's `Function` (stdlib/core/Function.kpp's `template<typename Signature> struct
  // Function;`, a name for FunctionImpl<R, Args...>) and its FunctionImpl: whether d is that one.
  bool isLibraryFunction(const Decl* d, bool impl);
  // `Function<T>` written with T a parameter (`template<class T> struct is_std_function<Function<T>>`)
  // and T now a signature: the FunctionImpl<R, Args...> it means, as `Function<void()>` written is.
  const Ty* functionOfSignature(const Ty* signature);

  Decl* initializerListDecl();
  bool isInitializerList(const Ty* t);
  // The library's Text (design.md 14): a parameter of the type takes a call's remaining arguments.
  uint32_t textSymbol_ = UINT32_MAX;
  Decl* textDecl();
  bool isText(const Ty* t);
  // The standard library's compiler intrinsics (stdlib/core/Text.kpp), declared without a body:
  // 1 `kppEnumeratorName(E value)`, the name of the enumerator value is (null for a value that is
  // none); 2 `kppEnumTypeName<E>()`, the enumeration's name (design.md 14: an enum's appendTo).
  int intrinsicOf(const FunctionDecl* f);
  // The library's move, both forwards and asConst (GlobalScope::callIntrinsicOf): its top-level function
  // templates of those names with one type parameter and one reference parameter, returning a reference,
  // found in the root scope once (never by a call's name: a namespace's own move is its function).
  CallIntrinsic callIntrinsicOf(const FunctionDecl* f);
  std::once_flag callIntrinsicsOnce;
  std::vector<std::pair<const FunctionDecl*, CallIntrinsic>> callIntrinsics;
  // An intrinsic's value at compile time: args the call's, subst binding E.
  std::optional<ConstValue> constIntrinsic(int which, FunctionDecl* f, const std::vector<ConstValue>& args, const Subst* subst, int depth);

  // The closures of lambdas that capture a Text, or such a closure (Walker's lambda walk): valid,
  // as a Text is, during the call that made them only.
  std::mutex textClosuresMutex;
  std::unordered_set<const Decl*> textClosures;
  void markTextClosure(const Decl* closure);
  bool isTextClosure(const Ty* t);
  // Whether t is, refers to or holds a Text or a closure capturing one (design.md 14): a pointer to
  // one, an array of them, a class template instance with one among its arguments.
  bool holdsText(const Ty* t, int depth = 0);
  // Whether f's last parameter is written as Text (`Text text`, `const Text& text`): the cheap test
  // before its type is looked at.
  bool namesTextLast(FunctionDecl* f);
  // The position of a function type's Text parameter, its last; SIZE_MAX when it has none.
  size_t textParameterOf(const Ty* ft);
  // A function or class the library declares at the top level (Text.kpp's), or null.
  Decl* libraryDecl(std::string_view name, NodeKind kind);
  const Ty* textType();

  // How one argument packed into a Text (design.md 14) appends itself, by its type: the library's
  // function the Text calls for it (Text.kpp) and the type it is instantiated for (none for C
  // text), or why the argument can be no piece, an error naming the type. An argument whose type
  // is not known yet (a template's own walk) has neither: its instance decides.
  TextPieceKind textPieceOf(const Expr* e, const Ty* given);
  uint32_t enumFromStrSymbol_ = UINT32_MAX;
  Decl* enumFromStrDecl();
  // The standard library's Optional (its class template), for `a?->b` of a value (design.md 23a); null
  // without the standard library.
  uint32_t optionalSymbol_ = UINT32_MAX;
  Decl* optionalDecl();
  uint32_t strongOrderingSymbol_ = UINT32_MAX;
  Decl* strongOrderingDecl();
  // What `a <=> b` yields: the library's strong_ordering, or int without the library.
  const Ty* orderingType();
  uint32_t typeInfoSymbol_ = UINT32_MAX;
  Decl* typeInfoDecl();
  bool isFunctionImpl(const Ty* t);
  // Whether a callable of type `a` takes the arguments of FunctionImpl<R, Args...> `impl`: a
  // lambda or function (pointer) by its parameters; anything else is not checked.
  bool callableWith(const Ty* a, const Ty* impl, int depth);

  // `ScopedSetter guard(x, true)`: a class template named without arguments is typed as its own
  // parameters (what typeOfTypeDecl gives for the primary); the arguments come from a constructor.
  bool isUndeducedClassTemplate(const Ty* t);
  // Whether a function's parameters can take this many arguments (defaults and a pack counted).
  static bool takesArgumentCount(const FunctionDecl* f, size_t count);

  // The class's arguments a deduction guide gives for these argument types (finishIndex made each
  // guide a function returning the class): `Overload{f, g}` with `template<typename... Ts>
  // Overload(Ts...) -> Overload<Ts...>;` is Overload<F, G>. ranks: each argument's conversion to
  // the chosen guide's parameter. Null when no guide takes them; of several, the one with the
  // better conversions (C++'s ranking), the first on a tie. An explicit guide is none for
  // copy-initialisation (`Overload o = {f, g};`).
  const Ty* deduceFromGuides(ClassDecl* cls, const std::vector<const Ty*>& argTypes, bool copyInit, std::vector<int>& ranks);
  // Whether the constraints of class template cls (its parameters' and its requires clause) hold for
  // the arguments d gives; true where they cannot be told (dependent arguments).
  bool classConstraintsHold(ClassDecl* cls, const Subst& d);

  // Whether conversions a are better than b: better for one argument, worse for none.
  static bool strictlyBetter(const std::vector<int>& a, const std::vector<int>& b);

  // The class's arguments deduced from a constructor or a deduction guide taking these argument
  // types, or null. Both are C++'s candidates: the constructor wins only with better conversions,
  // the user's guide on a tie.
  const Ty* deduceClassArgs(const Ty* primary, const std::vector<const Ty*>& argTypes, bool copyInit = false);

  // The arguments of a class template without constructors deduced from a brace list, as C++20
  // does for an aggregate: its bases, then its data members take the elements in order, a base
  // that is a pack (`struct Overload : Ts...`) all the remaining ones, one base each. A last member
  // that is an array with a parameter bound (`T items[N]`) takes all the remaining elements, N
  // their count, as std::array's deduction guide has it: `Array a = {x, y, z}`, `Pair{name,
  // &Record::field}`, `Overload{f, g}`.
  const Ty* deduceAggregateArgs(const Ty* primary, const std::vector<const Ty*>& argTypes);

  // The canonical type a resolved name denotes when used as a type.
  const Ty* typeOfTypeDecl(Decl* d, const Lookup& l, const NamePart& lastPart, const TypeContext& ctx, int depth);

  // AST type -> canonical type.
  const Ty* convertType(Type* t, const TypeContext& ctx, int depth);

  // Context in which a declaration's own type is written: its lexical scope plus the template
  // parameters of the declaration and of its enclosing templates.
  struct DeclContext
  {
    LocalStack params;
    Scope* scope;
  };
  void declContextFor(Decl* d, DeclContext& out);

  // Declared type of a declaration, seen through an optional substitution (for members of
  // template instantiations). Non-dependent results are cached on the declaration.
  const Ty* declaredTypeOf(Decl* d, const Subst* subst, int depth);

  // t with C's calling convention on each function type in it (through pointers, references and
  // arrays; nested: in a function type's parameters and result too) that takes or returns a class
  // by value (TypeTable::function's cConvention): an imported C header's declaration's type, and an
  // extern "C" function's own (nested false: its parameters are spelled in kpp). Thread B,
  // 2026-11-05 (design.md, "Targets and hosts").
  const Ty* withCConvention(const Ty* t, bool nested, int depth = 0);

  // The substitution each instantiation's copy of a function is typed under, as its walk recorded
  // it: a class declared in that body (its members, its bases) sees the instance's arguments.
  // Sharded by the copy's address: every instance walk records one, on every thread (one map
  // under one lock was a fifth of the parallel front end's waiting).
  struct InstanceSubstShard
  {
    std::mutex mutex;
    std::unordered_map<const Decl*, Subst> substs;
  };
  InstanceSubstShard instanceSubstShards[64];
  InstanceSubstShard& instanceSubstShard(const Decl* copy);
  void recordInstanceSubst(const Decl* copy, const Subst& s);
  // The substitution of the instance copy whose body declares the class d is (or is in), or null.
  // Only classes: the copy's own parameters and locals keep their written types, which the code
  // generator substitutes itself (packs included).
  const Subst* instanceSubstOf(const Decl* d);
  // subst with the pairs of the instance d is declared in added (those subst binds stay).
  Subst withInstanceSubst(const Decl* d, const Subst* subst);

  // The class template t's arguments for variable v from its initializer, as the walk deduces them
  // (CTAD: a constructor or deduction guide, else an aggregate's members); null when its values'
  // types are not known here.
  const Ty* deducedFromInitializer(const Ty* t, VariableDecl* v, const TypeContext& ctx);

  const Ty* computeDeclaredType(Decl* d, const Subst* subst, int depth);

  bool containsAuto(const Ty* t) const;

  // Whether an unknown type or a draft (an unresolved name's placeholder class) is anywhere in t
  // (`Map<?, int>`: an argument a declaration's context could not resolve).
  static bool mentionsUnknown(const Ty* t, int depth = 0);

  // Replaces the Auto placeholder in `pattern` with the type deduced from an initializer.
  // `auto names = {"pipe", "pipe-to-ground"}`: the InitializerList<T> a brace list copy-initializing
  // an `auto` variable makes, T each element's decayed type, the same for all (null when they
  // differ, or one is not known, or a list is nested).
  const Ty* initializerListOfElements(const BraceInitExpr* list);

  // initExpr: the initializer whose type init is, when there is one, for its value category: an
  // `auto&&` is a forwarding reference, an lvalue reference for an lvalue (`auto&& value =
  // this->state;` an AIState&, as C++ deduces it), an rvalue reference for an rvalue.
  const Ty* deduceAuto(const Ty* pattern, const Ty* init, const Expr* initExpr = nullptr);

  // Variable v's type deduced from its initializer's type init (initExpr), as deduceAuto does, but
  // for a structured binding's hidden object ([dcl.struct.bind] 1): an array initializer with no
  // ref-qualifier makes it an array of the same type, its elements copied (`auto [a, b, c] = arr;`;
  // cv from the declaration, `const auto [a, b] = arr;`), where an `auto` variable's decays to a
  // pointer.
  const Ty* deduceVariable(const VariableDecl* v, const Ty* pattern, const Ty* init, const Expr* initExpr = nullptr);

  // The type a function returning `auto`, `auto&`, `const auto*`... deduces from a return
  // statement's expression type (an lvalue's an lvalue reference, the walk's): the written shape
  // around the deduced type.
  const Ty* deducedShape(const Ty* pattern, const Ty* deduced);

  // Whether a written type is `auto`, or `auto` under qualifiers, references or pointers.
  static bool astMentionsAuto(const Type* t);

  // Whether a written type has a `decltype(expr)` anywhere in it (`Future<decltype(callable())>`).
  static bool astMentionsDecltype(Node* n, int depth = 0);

  // Whether expression or type n names a template parameter that s binds (by its name, in a name,
  // a template argument or a type written in it); true for a node it cannot look into.
  bool mentionsBound(Node* n, const Subst& s, int depth = 0);

  // Whether expression n has a lambda in it (true for a node it cannot look into).
  static bool hasLambda(Node* n, int depth = 0);

  // Whether a pack expansion's pattern is the pack alone (`Args...`).
  static bool isBarePack(const Type* pattern);

  // A function parameter pack's pattern (`const Box<Ts>&... boxes`) under ctx's substitution: a Pack of it typed once per
  // element of the packs it names; null when they are not bound to packs, its pattern is a bare pack, or an element is
  // not known.
  const Ty* expandedPattern(ParamDecl* pd, const TypeContext& ctx, int depth);

  // The parameter packs that s binds to packs and the pattern n names (by name, in its names and
  // template arguments, in types and expressions written in it), each once.
  void packsNamedIn(Node* n, const Subst& s, std::vector<Decl*>& out, int depth = 0);

  // Whether a written type names something its generic conversion cannot keep for the
  // substitution to fill in later: a decltype, a built-in type trait (`RemoveReference<T>`), a
  // template argument computed from the parameters (`Conditional<(sizeof(A) < sizeof(B)), B,
  // A>::Type`, `Frac<gcd(N, M)>`), or an alias template defined with one (`Wider<A, B>`). A signature written so is converted again
  // with a call's deduced arguments. scope and locals: where t is written.
  bool astNeedsArguments(Node* n, Scope* scope, const LocalStack* locals, int depth = 0);

  // Does `t` mention one of the given template parameters?
  static bool dependsOn(const Ty* t, Span<Decl*> params);

  // A class's argument list against an instance's: element by element, a trailing parameter
  // pack taking the rest as a pack (`TList<Ts...>` given TList<int, float>: Ts = int, float), as
  // an applied template template parameter's does below.
  void deduceClassArgs(Span<const Ty*> patterns, Span<const Ty*> args, Subst& out, int depth);

  // Deduction from a template argument (a specialisation's pattern, an argument of a class
  // template's instance): a parameter takes the argument as it is, a reference type included
  // (`Row<H, T...>` given Row<double&> binds H = double&, Wrapped<T> given Wrapped<int&> T =
  // int&), where a function parameter's T takes the type referred to.
  void deduceArgument(const Ty* pattern, const Ty* arg, Subst& out, int depth);

  // Structural template argument deduction: binds parameters of `pattern` from `arg`.
  // An array's qualifiers are its elements' ([basic.type.qualifier] 3): what a pattern's const (or
  // volatile) takes off its argument comes off the elements, so `const T&` against a const char[2]
  // lvalue (a string literal) makes T char[2], as C++ deduces.
  const Ty* arrayQualifiersOff(const Ty* a, bool isConst, bool isVolatile);

  void deduce(const Ty* pattern, const Ty* arg, Subst& out, int depth = 0);

  // The template parameter a parameter pack's type is, under references (`const Args&...` -> Args).
  static Decl* packParamOf(const Ty* t);

  // The template parameter packs a type names anywhere (`auto C::*...`: C and the invented one).
  static void packParamsIn(const Ty* t, std::vector<Decl*>& out, int depth = 0);

  // Deduces f's template parameters from the argument types of a call, including a trailing
  // parameter pack, which binds to the remaining arguments.
  // The declared type of candidate with its template parameters renamed to those of f: a
  // declaration and its separate definition each have their own, and signatures must compare equal.
  // The template parameters deduction left unbound take their defaults (`typename = EnableIf<...>`);
  // a default that is no type (EnableIf of a false condition) makes the candidate drop out.
  bool fillDefaultTemplateArgs(FunctionDecl* f, Subst& s);

  // `template<> const char* Button<RecipeID>::tooltip() {...}`: an explicit specialisation of a
  // member of a class template, the body that one instantiation's member has (not a definition
  // of the template's member).
  static bool isMemberSpecialization(const FunctionDecl* f);

  // The class instantiation an explicit member specialisation is written for (`Button<RecipeID>`
  // with its defaults), or null.
  const Ty* specializedClassOf(FunctionDecl* f) { return specializedClassOf(f, f->name); }
  const Ty* specializedClassOf(Decl* d, const QualifiedName& name);

  // The definition of static data member v for the instance of its class with arguments s: the
  // explicit specialisation written for that instance (`template<> const uint32_t
  // ElectricNetworkStatistics::smoothingTicks = 0;`), or the template's definition (v itself when
  // it has none out of the class).
  VariableDecl* variableDefinitionFor(VariableDecl* v, const Subst* s);

  // `int Flag<true>::counter = 100;`, `int Flag<true>::value() {...}`: the scope of the full
  // specialisation the qualifier names, whose own members these define (C++ writes them without
  // `template<>`); null when the qualifier names the template or no full specialisation.
  // locals: the definition's own template parameters (`template<bool B> int Flag<B>::value()`
  // names the template).
  Scope* fullSpecializationScope(const QualifiedName& qualifier, Scope* context, const LocalStack* locals, const ParsedFile* file, uint32_t token);

  // `template<typename T> int Holder<T, true>::load(int x) {...}`: the scope of the partial
  // specialisation the qualifier names, its arguments written as the specialisation's pattern
  // with the definition's own parameters in place of the specialisation's; null when it names
  // the primary template or no partial specialisation. l: the lookup of the qualifier.
  Scope* partialSpecializationScope(const QualifiedName& qualifier, const Lookup& l, Scope* context, Span<Decl*> defParams, const ParsedFile* file);

  // Whether explicit member specialisation f is written for class instantiation classTy (the
  // same arguments for the class template's parameters).
  bool memberSpecializationIsFor(FunctionDecl* f, const Ty* classTy);

  // `template<> ForceID parse<ForceID>(lua_State*, int32_t, const char*)`: an explicit
  // specialisation of a function template. It is no candidate of overload resolution (C++
  // chooses among the templates), only the body a call of its template runs with its arguments.
  static bool isFunctionSpecialization(const FunctionDecl* f);

  // The explicit specialisation of function template f written for these arguments of f's
  // parameters (`parse<ForceID>`, or deduced from its parameter types as in `template<> const
  // char* kind(bool)`) whose signature is f's with them; null when there is none.
  FunctionDecl* functionSpecializationFor(FunctionDecl* f, const Subst& args);

  const Ty* typeAs(FunctionDecl* candidate, FunctionDecl* f);

  // f's declared type as typeAs gives a definition's, for comparing the two: through the same
  // substitution (its parameters as themselves), so that a member of a dependent instance
  // (`Vector<VertexT*>::iterator`) is resolved alike on both sides.
  const Ty* matchedType(FunctionDecl* f);

  void deduceCall(FunctionDecl* f, const Ty* fnTy, const std::vector<const Ty*>& argTypes, Subst& out);

  // Deduction of a call's template arguments from its arguments: params are the parameter
  // declarations (for packs), patterns their types (a function's, or a generic lambda's).
  void deduceParams(Span<Decl*> params, Span<const Ty*> patterns, const std::vector<const Ty*>& argTypes, Subst& out);

  // Applies a substitution to a canonical type.
  // An instantiation of a primary class template whose arguments pick a specialisation: that
  // specialisation's type; otherwise the type as given.
  std::mutex specializationCacheMutex;
  struct Specialization
  {
    const Ty* type;
    std::vector<CollectedUse> uses; // what matching it read (a constant argument's bodies run, ...)
    Nesting needed;                 // how much deeper matching it nested (CachedEvaluation)
  };
  std::unordered_map<const Ty*, Specialization> specializationCache; // resolveSpecialization's answers for known arguments

  const Ty* resolveSpecialization(const Ty* classTy, int depth);

  // The primary template's arguments of a specialisation's instance: `Owner<T, Del<T>>` with T =
  // int is Owner<int, Del<int>>. Empty when t is no instance of a specialisation of `primary`.
  std::vector<const Ty*> primaryArgumentsOf(const Ty* t, Decl* primary, int depth);

  // A parameter whose type became a pack (`Args...` -> int, String), also under a reference or a
  // pointer (`Args&&...` -> int&&, String&&): appends one type per element and returns true.
  bool spliceIfPack(const Ty* sub, std::vector<const Ty*>& out);

  const Ty* substitute(const Ty* t, const Subst& s, int depth = 0);

