// GlobalScope::Impl, overload resolution: part of the class's body, included in it in GlobalScopeImpl.hpp, its
// definitions in OverloadResolution.cpp (resolver.cpp split, 2026-11-05, moved without a change; docs/design.md,
// "Compilation model").
  // ---------------- overload resolution ----------------

  // Is class `base` a (transitive) base of class `derived`? By scope, and for an instantiation also
  // by its arguments: a base they choose (`Const<Color> : Conditional<c, Value<T>, T>`) is not
  // linked to the class template's scope.
  bool isBaseClass(const Ty* base, const Ty* derived);

  // Whether a base of class t (directly or deeper) declares a virtual function whose override key
  // (overrideKey) is key: what a member function marked `override` must override (C++
  // [class.virtual]). Null when a base is not known.
  std::optional<bool> overridesVirtual(const Ty* t, const std::string& key, int depth = 0);

  // Whether a class's bases, as its scope and its bases' scopes link them (once, without
  // arguments), leave one out: a base its arguments decide, or one outside the program.
  bool hasUnlinkedBase(Scope* cls, int depth = 0);

  // Is `base` a (transitive) base class scope of `derived`?
  bool isBaseScope(Scope* base, Scope* derived, int depth = 0);

  Scope* classScopeOf(const Ty* t);

  static constexpr int NotViable = 1000;

  // Rank of list-initializing an object of type p from a brace list's elements (C++'s
  // [over.ics.list]): a scalar from its one element (from none: value-initialized); a class from
  // one element of its own type (or of a derived one), else by a constructor taking the elements
  // (an InitializerList one taking each of them), else as an aggregate whose bases and data
  // members take them in order, a user-defined conversion. 2 when the elements or the class are
  // not known; an aggregate that brace elision may fill is not followed and taken as viable. A
  // list nested in another's elements is not judged: the conversions under it would run out of
  // conversionRank's depth and read as not viable (`{"x", {"input_scale", 1}}` for a
  // Vector<NoiseFunctionParameter>).
  // The base classes (as instances, under p's arguments) whose constructors class def of type p
  // inherits with `using Vector2::Vector2;`, and those they inherit in turn.
  void inheritedConstructorSources(ClassDecl* def, const Ty* p, std::vector<const Ty*>& out, int depth, int level = 0);
  // The one rule for which constructor a construction runs (design.md 24; GlobalScope::chooseConstructor).
  FunctionDecl* chooseConstructor(const Ty* classType, const std::vector<const Ty*>& argTypes, const std::vector<bool>* nullConstants,
                                  const Ty** inheritedFrom, bool implicitOnly = false, bool* ambiguous = nullptr);
  // The one rule for which conversion operator a conversion runs (design.md 24; GlobalScope::chooseConversion).
  // contextualBool: a conversion to bool may use an explicit operator bool (a condition); false for a
  // copy-initialization, an argument (C++ [class.conv.fct] 2).
  // ambiguous, when given: two operators were equally good (null returned).
  FunctionDecl* chooseConversion(const Ty* from, const Ty* to, ConversionKind kind, bool explicitCast, const Ty** foundIn, Subst* opArgs,
                                 bool contextualBool = true, bool* ambiguous = nullptr);

  int listInitRank(const Ty* list, const Ty* p, int depth);

  // Whether constructor f (of a class whose arguments s binds) takes these arguments: their count,
  // its template's deduction, and each converts to its parameter (one not known is taken as
  // converting).
  bool constructorTakes(FunctionDecl* f, const std::vector<const Ty*>& argTypes, const Subst& s, int depth);

  // Rank of converting an argument of type `arg` to a parameter of type `param`: 0 exact,
  // 1 qualification, 2 promotion, 3 standard conversion, 4 user-defined, NotViable.
  int conversionRank(const Ty* arg, const Ty* param, int depth = 0);

  // Whether an object of type t is const: t's own const, an array's its elements' ([basic.type.qualifier]
  // 3: `const int (&)[3]` is a reference to const, a brace list or a temporary binds it).
  static bool isConstObject(const Ty* t);
  // t without its const and volatile, an array's elements' included (`const int[3]` is an int[3]).
  const Ty* unqualifiedDeep(const Ty* t);

  // The type a class operand of a built-in operator converts to (`area < smallBlueprintAreaSize`
  // with a Const<float>): the result of its one non-explicit conversion operator to a non-class
  // type, its own or inherited (under its arguments); null when it has none or several.
  const Ty* scalarConversionOf(const Ty* cls);
  // Whether a built-in operator op ([over.built]) takes operands of these types (no class: a class
  // operand is what it converts to; r null for a unary operator): arithmetic types and unscoped
  // enumerations for the arithmetic operators, integral ones for %, the bitwise and the shift
  // operators, a pointer and an integer for + and -, two of one enumeration (a scoped one too) or
  // two pointers for a comparison, what converts to bool for ! && and ||. A scoped enumeration has
  // no built-in `|` (Factorio's `status | EntityStatus::Working`, the member operator| alone).
  bool builtinOperatorTakes(P op, const Ty* l, const Ty* r);
  // A free operator function f (subst: its class's arguments, for a friend of an instance) against
  // the built-in operator on the class operands' conversions to scalars, as C++ ranks the two
  // ([over.match.oper]; the class has no member operator): operand by operand, the one that takes
  // it without a user-defined conversion is the better there. 1: f is chosen, 0: the built-in is,
  // -1: neither is better (`-n` with Num's operator double beside `operator-(const Other&)` and
  // Other's constructor from a Num), an ambiguous call.
  int freeOperatorAgainstBuiltin(FunctionDecl* f, const Subst* subst, P op, const std::vector<const Ty*>& operands);
  // A class's member operator f (subst: the class's arguments) on lhs and rhs against the built-in
  // operator on the operands' conversions, ranked as above: the member takes its object as it is,
  // the built-in through the class's conversion operator, so the member is the better on the left;
  // the built-in is on the right where the member takes it by a user-defined conversion and it is
  // no class (`n < 2.0` with Num's operator<(Num), `implicit Num(double)` and `operator double()`,
  // which clang rejects as ambiguous). 1: f is chosen, -1: an ambiguous call. Arithmetic,
  // comparison, bitwise, shift and logical operators only (a built-in assignment converts no left
  // operand, [over.match.oper] 4). On a SIMD vector too (`float32x4 < __m128`, which clang rejects:
  // classes/member_operator_against_builtin_vector_is_ambiguous), which Factorio's SSE.hpp answers
  // with exact free operators, converted for every platform.
  int memberOperatorAgainstBuiltin(FunctionDecl* f, const Subst* subst, P op, const Ty* lhs, const Ty* rhs);

  void collectScalarConversions(const Ty* cls, const Ty*& found, bool& ambiguous, int depth);

  // Whether a non-explicit conversion operator of class a, or of one of its bases (under a's
  // arguments: `Const<float> : Value<float>` with Value's `operator const T&()`), gives a value
  // that converts to param by a standard conversion. With rvalues, only one that gives a value or
  // an rvalue reference counts (what an rvalue reference binds), not one that gives an lvalue.
  bool convertsByOperator(const Ty* a, const Ty* param, int depth, int baseDepth = 0, bool allowExplicit = false, bool rvalues = false);


  // Whether data member m of a class template, a `when` member on its parameters (design.md 19), exists in the
  // instance whose arguments subst binds: its condition folded there, in the class's scope, as the layout folds it
  // (codegen's whenMemberHolds). Nothing when the condition is no constant here.
  std::optional<bool> whenMemberHolds(Decl* m, const Subst& subst, int depth = 0);

  // Of the data members a class template declares with m's name, m a `when` one decided per instance (design.md 19:
  // `when (isTriviallyCopyable<T>) T copy; when (!isTriviallyCopyable<T>) short copy;`), the one that exists in the
  // instance whose arguments subst binds (holding 1), or how many do (holding 0 or 2 and more: an error where it is
  // named). Null with undecided when a condition is no constant here.
  Decl* whenMemberOf(Decl* m, const Subst& subst, int& holding, bool& undecided, int depth = 0);

  // The named return variable's candidate in a function's body (NRVO, kovarex 2026-11-04;
  // GlobalScope::namedReturnVariable): the one automatic local every `return` of the body names
  // (nested lambdas' and local classes' returns aside), declared by a declaration statement or a
  // range-for, and not static, thread_local, extern, a structured binding, absent by its `when` or
  // declared alignas (the caller's result has its class's alignment alone); null with why
  // otherwise. Whether its type is the result's class (and it is not volatile) is the caller's to
  // compare, under the arguments it runs with: an instance's template arguments for codegen
  // (GlobalScope::namedReturnVariable's args), an evaluation's for the interpreter (a template's
  // body under its arguments).
  static VariableDecl* namedReturnCandidate(const Stmt* body, std::string* why);

  // The named return variable of the function whose body runs under ctx (the interpreter's call),
  // result its declared result: the candidate (namedReturnCandidate, kept per body during the
  // outermost evaluation: constNamedReturns), when its type under ctx's arguments is that class and
  // it is not volatile; else null.
  const VariableDecl* constNamedReturn(const Stmt* body, const Ty* result, const TypeContext& ctx, int depth);

  // The conversion function of class a (or of a base, whose arguments subst gets) to exactly the
  // type target, as a conversion-function-id names it: `commands.operator AllowedCommands::Enum()`.
  FunctionDecl* conversionFunctionTo(const Ty* a, const Ty* target, Subst& subst, int baseDepth = 0);

  struct Candidate
  {
    FunctionDecl* f;
    std::vector<int> ranks;
    int thisRank; // 0 exact constness, 1 const method on a non-const object, NotViable if const object/non-const method
    std::vector<bool> refs; // per rank: the parameter is a reference
    std::vector<const Ty*> params; // per rank: the parameter's type (null for a variadic tail)
    bool emptyText = false; // its Text parameter takes no pieces (design.md 14)
  };

  // The template parameters of d and of the templates around it, as locals of a context in d.
  static void pushEnclosingTemplateParams(Decl* d, LocalStack& own);

  // The value of a requires clause under the deduced arguments: `!`, `&&`, `||` and parentheses
  // over its parts, each a requires-expression (its requirements probed for the arguments), a
  // concept (its definition for them), a boolean trait (`isIntegral<T>`), or any other constant
  // expression: a boolean variable template (`hasBegin<C>`), a comparison of deduced constants
  // (`N1 * D2 <= N2 * D1`). nullopt when it cannot be told (an argument still dependent, an
  // expression the constant evaluation does not know). f is the function (or the concept) whose
  // template parameters s binds.
  std::optional<bool> constraintValue(Expr* e, Decl* f, const Subst& s, int depth = 0);

  // The primary of the variable template a lookup found (its declaration without written
  // arguments), or null when the name is no variable template.
  static VariableDecl* primaryVariableTemplate(const Lookup& l);

  // The specialisation of variable template v the arguments part writes pick, read under ctx
  // (variableSpecialization), and its arguments: a trailing pack takes the rest, defaults fill the
  // end. Null while an argument is unknown, or dependent unless dependent is allowed (typing a
  // template's own body), or when two specialisations match equally well.
  VariableDecl* variableTemplateSource(VariableDecl* v, const NamePart& part, const TypeContext& ctx, int depth, bool dependent, Subst& out);

  // The value of variable template v for the arguments part writes, read under ctx: the
  // initializer of the specialisation they pick, evaluated with them bound. nullopt while an
  // argument is dependent or unknown, or two specialisations match equally well.
  std::optional<ConstValue> variableTemplateValue(VariableDecl* v, const NamePart& part, const TypeContext& ctx, int depth);

  // The type of `two_v<int>`: the declared type of the specialisation the arguments pick, with them
  // bound (`T` is int); null when they pick none.
  const Ty* variableTemplateType(VariableDecl* v, const NamePart& part, const TypeContext& ctx);

  // The concept d declares (`template<class T> concept C = ...;`, a variable template with no
  // type), or null.
  static VariableDecl* conceptOf(Decl* d);

  // Whether the concept holds for the arguments part writes (`has_str<T>` under ctx): its
  // definition evaluated for them; nullopt when that cannot be told (an argument still
  // dependent, a pack).
  std::optional<bool> conceptValue(VariableDecl* concept_, const NamePart& part, const TypeContext& ctx, int depth);

  // The concept's definition evaluated for the arguments given (a trailing pack parameter takes
  // the rest); nullopt when that cannot be told.
  std::optional<bool> conceptHolds(VariableDecl* concept_, const std::vector<const Ty*>& given, int depth);

  // The concept a constrained template parameter of owner names (`template<SmallInt T>`,
  // `template<SameAs<U> T>`), or null (an unconstrained one, or the type of a constant parameter:
  // `template<Mode::Type M>`).
  VariableDecl* typeConstraintOf(const TemplateParamDecl* p, Decl* owner);

  // Whether p's type-constraint holds for its argument under s ([temp.param] 4: `C<T, X...>` for
  // `template<C<X...> T>`; for a constrained pack, each element's): true for an unconstrained p,
  // nullopt when it cannot be told (an argument not known yet).
  std::optional<bool> typeConstraintHolds(TemplateParamDecl* p, Decl* owner, const Subst& s, int depth);

  // Whether f has associated constraints: a requires clause or a constrained template parameter.
  bool constrained(const FunctionDecl* f);

  // Whether every requirement of `requires (params) { ... }` in owner (a concept, a variable
  // template, a constrained function) is well-typed under s, probed on a scratch walker; nullopt
  // while one still depends on a parameter.
  std::optional<bool> requirementsHold(RequiresExpr* r, Decl* owner, const Subst& s);

  // An expression in a function's signature (`decltype(callable())` in its trailing return type)
  // typed under ctx's substitution, with the function's parameters in scope: a copy of it is
  // walked, so that the template's own nodes keep their generic types. Null when it cannot be.
  const Ty* typeOfSignatureExpression(Expr* e, const TypeContext& ctx);

  // The first name in a requires clause that names nothing (`requires (noSuchTrait<T>)`), or "".
  std::string undeclaredNameIn(Expr* e, Decl* f, const LocalStack& own, int depth = 0);

  // Whether f's requires clause holds for the deduced arguments (no clause holds). One that
  // names something not declared (`requires (noSuchTrait<T>)`) is a compile error, and does not
  // hold. One that cannot be told otherwise holds for now: while an argument is still dependent,
  // and (until static constexpr members of class template instances evaluate, which the
  // library's Duration constraints need) for known arguments too.
  bool requiresHolds(FunctionDecl* f, const Subst& deduced);

  // Whether f's associated constraints hold for the deduced arguments: its template parameters'
  // type-constraints (typeConstraintHolds), then its requires clause (requiresHolds). Until
  // 2026-11-05 a type-constraint was parsed and never checked: `template<SmallInt T> int g(T)`
  // took g(2.5).
  bool constraintsHold(FunctionDecl* f, const Subst& deduced);

  // Whether each argument deduces what the call deduced ([temp.deduct.type] 2: a template
  // parameter deduced from two arguments gets the same type from each, else deduction fails):
  // deduceCall keeps a parameter's first deduction, so `f(1, 2.5)` of `template<class T> T f(T, T)`
  // took T = int and converted 2.5 to 2 where C++ has no such call. given: deduced's leading pairs
  // that are no deduction (the class's arguments, explicit ones), which an argument converts to.
  bool deductionsAgree(FunctionDecl* f, const Ty* ft, const std::vector<const Ty*>& argTypes, const Subst& deduced, size_t given);

  // A constraint in normal form ([temp.constr.normal]): atomic constraints under && and ||. An
  // atom is an expression with its parameter mapping, the arguments its owner's template
  // parameters stand for there; a concept-id is replaced by its concept's definition under the
  // arguments it names, so that the atoms a concept defines are one and the same wherever it is
  // named, and two atoms are identical only when they are the same expression with the same
  // mapping ([temp.constr.atomic] 2): `requires integral<T>` written in two templates are two atoms.
  struct ConstraintAtom
  {
    const Node* e = nullptr;
    std::vector<const Ty*> mapping;
  };
  struct ConstraintTerm
  {
    uint8_t kind = 0; // 0 an atom, 1 a conjunction, 2 a disjunction
    int left = -1, right = -1, atom = -1;
  };
  struct NormalConstraint
  {
    std::vector<ConstraintAtom> atoms;
    std::vector<ConstraintTerm> terms;
  };

  // e normalized into n under s (its owner's template parameters bound to what they stand for):
  // the index of its term, or -1 when it cannot be (too deep, a concept-id whose arguments do not
  // form).
  int normalizeConstraint(NormalConstraint& n, const Node* e, Decl* owner, const Subst& s, int depth);

  // f's associated constraints normalized under s ([temp.constr.decl] 3: its template parameters'
  // type-constraints, then its requires clause, conjoined): the root term, -1 for none (true), -2
  // when they cannot be normalized.
  int associatedConstraints(NormalConstraint& n, FunctionDecl* f, const Subst& s);

  // The clauses of term t (-1: none, true): its disjunctive normal form (disjunction), or its
  // conjunctive one: each clause a list of atom indices; false when they grow past a bound.
  static bool constraintClauses(const NormalConstraint& n, int t, bool disjunction, std::vector<std::vector<int>>& out);

  // Whether constraint p (root pt in pn) subsumes q (root qt in qn) ([temp.constr.order] 1): each
  // disjunctive clause of p's has an atom identical to one of each conjunctive clause of q's. No
  // constraint (-1) is subsumed by every one and subsumes only another none.
  bool subsumes(const NormalConstraint& pn, int pt, const NormalConstraint& qn, int qt);

  // Whether a is more constrained than b ([temp.func.order] 6.2.2, [over.match.best] 2.6): their
  // template parameters taken position by position, a's associated constraints subsume b's and not
  // the other way. false when either's cannot be normalized, or their template parameter counts
  // differ.
  bool moreConstrained(FunctionDecl* a, FunctionDecl* b, const Subst* classSubst);

  // Whether a and b take the same parameter types, b's template parameters taken as a's position by
  // position (the condition under which constraints order two functions).
  bool equivalentParameters(FunctionDecl* a, FunctionDecl* b, const Subst* classSubst);

  // The parameter type of f that a call's argument i binds, for partial ordering: a trailing
  // pack's pattern for each argument it takes (fromPack set); null when there is none, or f has
  // a pack before its last parameter (not ordered here).
  const Ty* orderingParam(FunctionDecl* f, const Ty* ft, size_t i, bool& fromPack);

  // One step of partial ordering: whether parameter type p of one template deduces from the
  // other's type a at the same argument, within s, the deduction for the whole call so far.
  // References and top-level qualifiers are removed and a's template parameters stand in as
  // unique types. A pack's pattern deduces each argument afresh (its packs bind per element);
  // a type from a pack deduces only a pack's pattern.
  bool orderingDeduces(const Ty* p, bool pFromPack, const Ty* a, bool aFromPack, Subst& s);

  // Whether type x is more cv-qualified than y.
  static bool moreQualified(const Ty* x, const Ty* y);

  // C++'s partial ordering of function templates ([temp.deduct.partial], [temp.func.order]):
  // whether a is more specialised than b for a call with `count` arguments. a is at least as
  // specialised as b when b's parameter types deduce from a's (orderingDeduces), and more when
  // not the other way round. Two references that deduce both ways prefer the lvalue reference
  // (`reverse(T&)` over `reverse(T&&)`), then the more qualified one (`begin(const C&)` over
  // `begin(C&)`); equal otherwise, a template with no trailing pack beats one whose pack takes
  // no argument (`emplace(it, key)` over `emplace(it, key, values...)`).
  bool moreSpecialized(FunctionDecl* a, FunctionDecl* b, size_t count, const Subst* classSubst);

  // Whether t names one of f's template parameters that deduction binds (not one the first
  // `given` pairs of s, the class's and the explicit arguments, already bound; not under `T::x`).
  bool namesDeducedParam(const Ty* t, FunctionDecl* f, const Subst& s, size_t given, int depth = 0);

  // A parameter whose type is an instantiation pattern of a class template that deduction reads
  // (`const GameOptional<B>&`, `Vector<T>*`) accepts as argument that template's instantiation
  // (the deduced one) or a class derived from it, nothing converted to it. written: the declared
  // parameter type, substituted: with the deduced arguments, arg: the argument's type.
  bool deducedClassFits(const Ty* written, const Ty* substituted, const Ty* arg, FunctionDecl* f, const Subst& s, size_t given);

  // Picks the best candidate by C++'s rule (better in one argument, not worse in any); on a tie
  // the non-template, then the first one. Null when no candidate is viable. bindExplicit, when
  // given, binds the call's explicit template arguments for a candidate (false: the list does not
  // fit it); a template candidate then has to have every parameter bound or deduced
  // (`T load(Args&&...)` is no candidate for `load(x)`, whose T nothing gives).
  // nullConstants: per argument, whether it is the literal 0, which converts to any pointer (C++'s
  // null pointer constant; design.md 4).
  // bitFieldPromotions: per argument, the type a bit-field argument promotes to by its width
  // ([conv.prom] 5: an `unsigned u3 : 3` to int), or null: its conversion to that type, by value or
  // to a const reference, is a promotion, as C++ ranks it (f(int) over f(i64) for s.u3).
  // ambiguousCallsAreErrors: a call that two functions take equally well is a checker error, as in
  // C++ (expressions/ambiguous_call_is_error). Two declarations of one C function with two types are
  // an error of their own (checkCFunctionTypes).
  static constexpr bool ambiguousCallsAreErrors = true;

  // The scope a function belongs to: its class's, or the namespace its name names.
  Scope* homeScope(FunctionDecl* f);

  static bool isCFunction(const FunctionDecl* f) { return (f->flags & DF_ExternC) || (f->file && f->file->isImported); }

  // Every C function (cFunctions) declared with the type of its first declaration: its result,
  // its parameters (their own const aside) and whether it is variadic. A declaration whose type is
  // not known yet is not compared.
  void checkCFunctionTypes();

  // Whether two candidates are one function declared twice: a declaration and its definition, a
  // member declared in its class and defined outside it, a function declared in two places of its
  // namespace (of one name, scope, type and qualifiers), or a C function of one name and type, which
  // a header and the library both declare.
  bool sameFunction(FunctionDecl* a, FunctionDecl* b);

  FunctionDecl* chooseAmong(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes, const Subst* classSubst, const Ty* objectTy, bool& ambiguous, const std::function<bool(FunctionDecl*, Subst&)>* bindExplicit = nullptr, const std::vector<bool>* nullConstants = nullptr, std::vector<FunctionDecl*>* tiedOut = nullptr, const std::vector<const Ty*>* bitFieldPromotions = nullptr);

  // C++ ranks a class's member operator and the free ones together ([over.match.oper]); the checker
  // (Walker::operatorFunction) and the interpreter's own lookup rank them so: the member, unless a
  // free one takes the operands strictly better (classes/operator_member_and_free_ranked_together).
  // The ranks of member operator f (of the class subst instantiates): the object's binding to its
  // implicit object parameter as no conversion, then rhs to its parameter (arity 2), exact and
  // qualification as one rank; empty for a template or a type not known (not ranked then).
  std::vector<int> memberOperatorRanks(FunctionDecl* f, const Subst& subst, const Ty* rhs, size_t arity);
  // Whether member operator f takes the operands with no conversion (no free one can be better).
  bool memberOperatorExact(FunctionDecl* f, const Subst& subst, const Ty* rhs, size_t arity);
  // Whether free operator free (freeSubst: its class's arguments, a friend's) takes lhs and rhs
  // strictly better than member operator member: no worse for either operand, better for one. Not a
  // template, nor a unary operator (a member is kept there).
  bool freeOperatorBeatsMember(FunctionDecl* free, const Subst& freeSubst, FunctionDecl* member, const Subst& memberSubst, const Ty* lhs,
                               const Ty* rhs, size_t arity);

  // Whether the free operator f takes the enumeration operands of `lhs op rhs` as they are, so that
  // it is the user's operator for that enum (`Flags operator|(Flags, Flags)`) rather than one
  // reached through a conversion to some class; f is a non-template function.
  bool takesEnumOperands(FunctionDecl* f, const Ty* lhs, const Ty* rhs);

