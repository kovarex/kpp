// GlobalScope::Impl, phase 2, types: part of the class's body, included in it in GlobalScopeImpl.hpp,
// its definitions in TypeOfAst.cpp (resolver.cpp split, 2026-11-05, moved without a change).
  // ---------------- phase 2: types ----------------

  struct ConstPlace;
  struct ConstBinding // a parameter's or a local's value while a function runs at compile time
  {
    uint32_t symbol;
    ConstValue value;
    const Ty* type = nullptr; // what an assignment converts to
    bool writable = true;     // false for a const one, a reference to const, a copy for a reference
    // A reference bound to a place: the locals it is in (this function's or a caller's, which
    // outlive it), the binding and the part; value then unused.
    std::vector<ConstBinding>* aliasEnv = nullptr;
    size_t aliasIndex = 0;
    std::vector<size_t> aliasPath;
    // A function parameter pack (`Args&&... args`): value's parts are its elements, read only one
    // at a time, by a pack expansion (constExpand). packPlaces: where each element's argument is
    // (env null: nowhere the run holds), which a reference element refers to.
    bool pack = false;
    std::vector<ConstPlace> packPlaces;
    // Its address was taken (`&x`): the serial an Address names it by (constStorage).
    uint64_t serial = 0;
    // An object the run made and destroys when the binding ends (constEndLocals): a local, a
    // by-value parameter, a temporary, a reference's that extends its life. Not a reference's that
    // copies an lvalue it cannot alias, nor `this`.
    bool owns = false;
  };
  struct TypeContext
  {
    Scope* scope;
    const LocalStack* locals;
    const Subst* subst;
    const std::vector<Token>* tokens = nullptr; // For evaluating array bounds written as literals.
    std::string_view text;
    std::vector<ConstBinding>* env = nullptr; // The parameters and locals of a function running at compile time.
    const Ty* returnType = nullptr;          // That function's declared result type (`return {a, b};`).
    // That function's reference result wanted as a place (`result[i] = x` through operator[]).
    std::optional<ConstPlace>* returnPlace = nullptr;
    // That function's named return variable (NRVO, constNamedReturn): `return v;` of it is the result.
    const VariableDecl* namedReturn = nullptr;
    FunctionDecl* function = nullptr; // Whose parameters a trailing return type names (`-> decltype(callable())`).
    // The variable template whose initializer is evaluated under subst (`hasBegin<Vector<int>>`):
    // a requires-expression in it holds or not for these arguments, whatever its last typing left.
    Decl* variableTemplate = nullptr;
    // One element of a pack expansion typed (TemplateArgs::elementPacks): the packs whose mentions
    // here name the element at elementIndex, subst binding them whole. Only the pattern's own
    // nodes see it: a context made for a declaration they name starts without it.
    const std::vector<Decl*>* elementPacks = nullptr;
    uint32_t elementIndex = 0;
  };

  // What a mention of template parameter d names, subst binding it to bound: under ctx's pack
  // element (an expansion's pattern), the element of a pack it expands; bound otherwise.
  static const Ty* elementOf(const Decl* d, const Ty* bound, const TypeContext& ctx);

  // A written template argument that names only types that exist under ctx's substitution,
  // the arguments of an alias template included: `VoidT<T::force_memcpy_save>` is void for any T
  // but well-formed only for a T with that member (the detection idiom's substitution failure).
  bool wellFormedArgument(Node* n, const TypeContext& ctx, int depth = 0);

  // The specialisation of a variable template a use with these arguments (one per parameter of
  // the primary) takes its value from, as C++ chooses it: the matching partial and full
  // specialisations, the most specialised of them (its parameters bound in out); the primary
  // when none matches. Null when the matches are ambiguous.
  VariableDecl* variableSpecialization(VariableDecl* primary, const std::vector<const Ty*>& args, Subst& out);

  // --- builtin type traits ---

  static bool isTypeNode(Node* n);

  // The argument types of a trait, or empty when one is not a type or is still dependent.
  std::vector<const Ty*> traitArgs(const NamePart& part, const TypeContext& ctx, int depth);

  // The type an enumeration is declared with, read where the enumeration is declared, not where
  // UnderlyingType<E> is written (`enum class Enum : InternalType` with AlertType's `using
  // InternalType = uint8_t;`, met in DeserialiserHelper's loadEnum); int when it names none.
  const Ty* enumUnderlyingType(const Ty* e, const TypeContext& ctx, int depth);

  bool isTypeTraitName(uint32_t symbol);

  // RemoveConst<T> and the other type-valued traits; null when `part` is not one.
  const Ty* typeTrait(const NamePart& part, const TypeContext& ctx, int depth);

  static bool isArithmeticKind(const Ty* t);

  // C++'s composite pointer type of two pointers: to their pointee when that is one type up to
  // qualifiers, to void when one is void, to the base when one points to a class derived from the
  // other's; qualified as either pointee is. Null when the pointers have none.
  const Ty* compositePointer(const Ty* a, const Ty* b);

  // Whether `derived` is `base` or has it among its bases (classes only).
  bool derivesFrom(const Ty* derived, const Ty* base, int depth = 0);

  // Whether a constructor or conversion operator is explicit here: `explicit`, or `explicit(cond)`
  // with cond true under the substitution (FixedPointNumberTemplate's `explicit(Explicit)`).
  bool isExplicit(FunctionDecl* f, const Subst* subst);

  // Whether a function is noexcept here: `noexcept`, or `noexcept(cond)` with cond true under the
  // substitution (`noexcept(false)` is not; FlatMap's `noexcept(isNothrowMoveConstructible<T>)` as T
  // is). nullopt: the condition is no constant here.
  std::optional<bool> isNoexcept(FunctionDecl* f, const Subst* subst);

  // Trait answers by (name, argument types): a trait of known types is a fact about them, asked
  // again and again (isTriviallyCopyable of a class walks its special members and bases each time,
  // about a fifth of the front end before this cache). Only definite answers are kept.
  struct TraitShard
  {
    std::shared_mutex mutex;
    std::unordered_map<std::string, bool, PointerKeyHash> answers;
    // What computing an answer read (a recording build's, when it read anything): replayed to each
    // asker it answers.
    std::unordered_map<std::string, std::vector<CollectedUse>, PointerKeyHash> uses;
  };
  TraitShard traitShards[64];

  // isArray<T>, isSame<A, B>, ... ; nullopt when `name` is not a trait.
  std::optional<bool> traitValue(std::string_view name, const std::vector<const Ty*>& a);

  std::optional<bool> traitValueUncached(std::string_view name, const std::vector<const Ty*>& a);

  bool isBoolTraitName(uint32_t symbol);

  // --- what the class traits read: definitions, bases, members, special members ---

  // The definition of a class type (the declaration it names may be a forward one), or null.
  ClassDecl* classDefinition(const Ty* t);

  // The direct bases of a class type, with its template arguments substituted; false when one
  // is not known.
  bool directBases(const Ty* t, ClassDecl* def, std::vector<const Ty*>& out);

  // The types of the non-static data members, with the class's template arguments substituted;
  // false when one is not known.
  bool fieldTypes(const Ty* t, ClassDecl* def, std::vector<const Ty*>& out);

  // What an override has to match: the name, the parameter types under the class's template
  // arguments, const. Every destructor overrides every other.
  std::string overrideKey(FunctionDecl* f, const Ty* cls);

  // isPolymorphic: the class or a base declares a virtual function.
  std::optional<bool> polymorphicClass(const Ty* t, int depth = 0);

  // isAbstract: a pure virtual function of the class or a base that no class from the base down
  // to t overrides. `overridden` holds the keys the classes below (more derived) declare.
  std::optional<bool> abstractClass(const Ty* t, std::unordered_set<std::string> overridden, int depth = 0);

  // The copy and move constructors and assignment operators a class declares (none of them is a
  // template), and whether it declares a destructor: what decides its implicit ones.
  struct SpecialMembers
  {
    FunctionDecl* copyCtor = nullptr;         // C(const C&)
    FunctionDecl* copyCtorNonConst = nullptr; // C(C&)
    FunctionDecl* moveCtor = nullptr;         // C(C&&)
    FunctionDecl* copyAssign = nullptr;       // operator=(const C&) or operator=(C)
    FunctionDecl* copyAssignNonConst = nullptr;
    FunctionDecl* moveAssign = nullptr;       // operator=(C&&)
    bool destructor = false;
  };
  enum class TraitForm { Plain, Trivially, Nothrow };

  // Whether a parameter's type is the class itself: 1 `const C&`, 2 `C&`, 3 `C&&`, 4 `C`; 0 not.
  int selfParameter(Decl* p, ClassDecl* def);

  SpecialMembers specialMembersOf(ClassDecl* def);

  // A user-declared special member chosen for the trait: deleted is false; a user-provided one
  // is not trivial, and nothrow as declared; a defaulted one is as the implicit one (nullopt:
  // decide by the bases and members).
  std::optional<bool> declaredSpecial(FunctionDecl* f, TraitForm form, const Ty* of);

  // isMoveConstructible (move) and isCopyConstructible, in their plain, trivial and nothrow
  // forms: whether T is built from an rvalue (from a const lvalue) of itself by the constructor
  // C++ chooses, the implicit or defaulted ones by the class's bases and members.
  std::optional<bool> constructibleFromSelf(const Ty* t, bool move, TraitForm form, int depth = 0);

  // isMoveAssignable (move) and isCopyAssignable, likewise.
  std::optional<bool> assignableFromSelf(const Ty* t, bool move, TraitForm form, int depth = 0);

  // The implicit (or defaulted) special member of a class exists, is trivial or nothrow, when
  // every base and member's does; an assignment also needs no reference or const member, and a
  // trivial one no virtual function or base.
  std::optional<bool> membersAllow(const Ty* t, ClassDecl* def, TraitForm form, const std::function<std::optional<bool>(const Ty*, int)>& each, bool assignment, int depth);

  // The class an object of type t is made of for the triviality traits (arrays by element), or
  // the answer itself for a type that is no class: nullopt result means "not decided here".
  struct TrivialSubject
  {
    const Ty* cls = nullptr;
    std::optional<bool> answer;
  };
  TrivialSubject trivialSubject(const Ty* t);

  // isTriviallyDestructible: no user-provided, deleted or virtual destructor, and every base and
  // member trivially destructible.
  std::optional<bool> triviallyDestructible(const Ty* t, int depth = 0);

  // isTriviallyCopyable: trivially destructible, and every copy and move constructor and
  // assignment the class has is trivial, at least one of them existing.
  std::optional<bool> triviallyCopyable(const Ty* t);

  // isTriviallyDefaultConstructible: a default constructor that is neither user-provided nor
  // deleted, no virtual function or base, no default member initializer, and every base and member
  // trivially default constructible.
  // Whether T() builds a T (std::is_default_constructible): a scalar or a pointer does; a reference
  // does not; a class by its declared default constructor (not a deleted one), none if it declares
  // others, else by the implicit one, which needs every base and member default-constructible (or
  // initialised in the class), no reference or const scalar left uninitialised, and for a typed
  // union its first alternative (design.md 8.5: default construction builds that one).
  std::optional<bool> defaultConstructible(const Ty* t, int depth = 0);

  std::optional<bool> triviallyDefaultConstructible(const Ty* t, int depth = 0);

  // isInvocable: whether a value of type f can be called with arguments of these types (each a
  // reference type for an lvalue, as in C++'s declval). result, when given, is the call's type
  // where it is known without typing a body.
  std::optional<bool> invocable(const Ty* f, const std::vector<const Ty*>& args, const Ty** result);

  // Walk a class's own members and its bases to find `static constexpr bool value`.
  // Returns the VariableDecl if found, nullptr otherwise.  Used by constEval to fold
  // `SomeTrait<T>()` (a zero-arg constructor of a TrueType/FalseType–derived struct)
  // and `SomeTrait<T>::value` (a qualified access to the inherited static member).
  // A trait object converted to bool (`HasSize<T>()`, `HasSize<T>{}`): the class's static
  // constexpr `value`, its own or a base's, read for the instance its arguments make, a base's
  // with that base's arguments (`HasSize<double> : Constant<bool, false>`: V is false).
  std::optional<ConstValue> traitObjectValue(const Ty* cls, int depth);

  // Whether e names a tuple-like structured binding (its hidden `auto&&` variable, Walker::tupleBindings).
  static bool namesTupleBinding(const Expr* e);

  // `auto [a, b] = e;` of class type t: whether the library's TupleSize<t> (std::tuple_size) has
  // a member named value, which makes the binding tuple-like (design.md 4, structured bindings);
  // size gets that value, nothing when it is no constant.
  bool tupleSizeOf(const Ty* t, std::optional<int64_t>& size, int depth);

  // Whether class t has a member function template get whose first template parameter is a
  // constant (`template<int I> auto& get()`): its tuple-like bindings call e.get<i>(), as C++'s do.
  bool hasMemberGet(const Ty* t, int depth);

  VariableDecl* findStaticBoolValue(ClassDecl* c, int depth);

  // `path:line` of a token, `path:line:column` with --diagnostic-columns (sourceLocation).
  static std::string locationOf(const ParsedFile* file, uint32_t token);

  void reportCheckerError(const ParsedFile* file, uint32_t token, const std::string& message);

  void reportCheckerErrorText(const std::string& text);

  // The errors the units given reported (and the layout asserts they met), taken back: the units
  // are walked or checked again and report what they still find. An error another unit reported
  // too stays.
  void forgetErrorsOf(const std::unordered_set<const void*>& units);

  // A declaration removed from a kept build (a compile server's, step 4: unindexFunction,
  // unindexMember): its walk's recorded reads go, and the errors its walk and its declared type
  // reported with them. Its walk is never run again to take them back, as a changed body's is, so a
  // removed function's error stayed in every later build until the server ended (E, 2026-11-04).
  void forgetUnitsOf(const Decl* d);

