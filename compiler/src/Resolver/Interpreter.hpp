// GlobalScope::Impl, the compile-time evaluator (design.md, "Compile-time evaluation: one interpreter"): part of the class's
// body, included in it in GlobalScopeImpl.hpp, its definitions in Interpreter.cpp (resolver.cpp split, 2026-11-05, moved
// without a change; docs/design.md, "Compilation model").
  // ---- compile-time evaluation (design.md, "Compile-time evaluation: one interpreter") ----
  // Step 1: expressions with C++'s types (consteval.hpp computes them, for the code generator's
  // folding too), and function bodies of returns, if, if constexpr and locals initialized once.
  // A name's evaluation starts its calls' depth afresh (NameEvaluation); calls nest at most 512
  // deep, an evaluation takes at most a million steps, and the stack left bounds both.

  // Why the last evaluation that failed stopped (a signed overflow, a division by zero): the first
  // reason since whoever reports it cleared it (checkStaticAssert).
  static std::string& constWhy();

  // The code generator's hooks while it has a constant evaluated (GlobalScope::evaluate), else
  // null: the checker's own evaluations are Required, with its own layouts.
  static const ConstHooks*& constHooks();

  // The evaluator runs a function's body (def: the definition it is written in): what the current
  // unit computes depends on that body now (USE_RanBody), as does the code generator's text when it
  // asked (its hooks' ran: an incremental build emits that text again after an edit of the body).
  // A cached answer's bodies are replayed through here for each asker it answers.
  void ranBody(const FunctionDecl* def);

  // The cached answers whose collected reads meet a change (the evaluator's, the trait answers,
  // the specialisation cache): forgotten, computed again by whoever asks next (every unit that read
  // one had them replayed, and is found by its own record).
  void forgetCachedAnswers(const std::function<bool(const CollectedUse&)>& changed);

  // A cached answer's collected reads replayed (replayCollected), its body runs through ranBody.
  void replayUses(const std::vector<CollectedUse>& uses);

  ConstArith constArith();

  // The underlying type an enumeration type was declared with (`enum class Tag : uint8_t`), null
  // for one declared without, unknown when the written one cannot be converted.
  std::mutex underlyingMutex;
  std::unordered_map<const Ty*, const Ty*> underlyingTypes;
  const Ty* declaredUnderlying(const Ty* t);

  // depth: nesting of the evaluation, calls included (`ratioGcd(b, a % b)` recursing a few times
  // inside a static member's initializer inside a requires clause).
  std::optional<int64_t> constEval(Expr* e, const TypeContext& ctx, int depth = 0);

  // A condition (static_assert, if constexpr, a requires clause): any scalar constant, a floating
  // one included.
  std::optional<bool> constCondition(Expr* e, const TypeContext& ctx, int depth = 0);

  // A value converted to type t as a cast converts it (consteval.hpp): to an arithmetic or
  // enumeration type; a null pointer constant stays one for a pointer; nothing else is a constant
  // (a class, a type the checker does not know).
  std::optional<ConstValue> constConverted(std::optional<ConstValue> v, const Ty* t);

  // A value as a pointer of type t: the null pointer constant stays one; a pointer, or an array
  // (decayed: its first element), of t's element type, ignoring const (a cast between unrelated
  // types would read an element as what it is not).
  std::optional<ConstValue> constPointerTo(const ConstValue& v, const Ty* t);

  // A value given the type it was declared with (a variable's, a parameter's, a function's
  // result): converted to an arithmetic or enumeration type; kept for a type the checker leaves
  // open (auto, a dependent type); no constant for an object (step 1 has none) or a pointer other
  // than the null one.
  std::optional<ConstValue> constAsDeclared(std::optional<ConstValue> v, const Ty* t);

  // A scalar's initializer: `= value`, `{value}`, `{}` (zero).
  std::optional<ConstValue> constInitValue(Expr* init, const TypeContext& ctx, int depth);

  // The class type of an object a member is named through (`layers.size()`, `p->value`): the
  // walk's type when it holds for every instance, else the expression typed under ctx; null when
  // it is no class type the checker knows.
  const Ty* constObjectType(Expr* object, const TypeContext& ctx, bool arrow);

  // Bounds of one evaluation: the steps it takes and the calls nested in it.
  static uint64_t& constSteps();
  static int& constCalls();
  static int& constActive();

  // An expression's value at compile time under ctx, typed as C++ types it, or nullopt when it is
  // no constant the checker can evaluate.
  std::optional<ConstValue> constValue(Expr* e, const TypeContext& ctx, int depth = 0);

  // One evaluation, run by evaluate, whichever entry starts it (constValue, constInitialize): the
  // outermost one's own objects, memory and temporaries begin and end with it, memory it leaves
  // allocated is an error, and a result that points at one of its objects is no constant. One
  // nested in it is part of it.
  static int& constEvaluating();
  // The const globals' values an evaluation read (constValueOfDecl), by the definition and its
  // class's arguments: each initializer runs once per outermost evaluation, as C++ runs it once,
  // not at every read (a loop reading a table built by a function call rebuilt it per element: one
  // of the game's globals took 1.6 s so). Begin and end with the evaluation, as its memory does.
  static std::map<std::pair<Decl*, std::vector<const Ty*>>, ConstValue>& constGlobalValues();
  // The named return variables' candidates of the bodies an evaluation ran (constNamedReturn), by
  // body: cleared with the evaluation, as a compile server's next build may put another body at the
  // same address.
  static std::unordered_map<const Stmt*, VariableDecl*>& constNamedReturns();
  template<class Evaluate>
  std::optional<ConstValue> constOutermost(Evaluate&& evaluate, int depth)
  {
    if (constEvaluating() > 0)
      return evaluate();
    struct Level
    {
      Level() { ++constEvaluating(); }
      ~Level() { --constEvaluating(); }
    } level;
    constStorage().clear();
    constHeap().clear();
    constAllocations().clear();
    constGlobalValues().clear();
    constNamedReturns().clear();
    constFailedAt() = ConstWhere{};
    std::optional<ConstValue> v;
    {
      ConstFrame heap(&constHeap());
      {
        ConstTemps temps;
        v = evaluate();
        if (!constEndTemps(temps, depth, v.has_value()))
          v.reset();
      }
      for (const ConstAllocation& a : constAllocations())
        if (v && !a.isFreed)
        {
          if (constWhy().empty())
            constWhy() = "memory allocated at " + constWhereText(a.allocated) + " is never freed";
          v.reset();
        }
    }
    constHeap().clear();
    constAllocations().clear();
    constGlobalValues().clear();
    if (v)
      if (const ConstStorage* escaped = constHeldStorage(*v))
      {
        if (constWhy().empty())
          constWhy() = "a pointer to " + constStorageName(*escaped) + " in the result, whose lifetime ends with the evaluation";
        return std::nullopt;
      }
    return v;
  }

  std::optional<ConstValue> constValueIn(Expr* e, const TypeContext& ctx, int depth);

  // A template parameter pack s binds that n names (`Ts` in `isFundamental<RemoveReference<Ts>>`,
  // `Ns` in `Ns * 2`), or null.
  Decl* packNamedIn(Node* n, const Subst& s, int depth = 0);

  // `(isFundamental<RemoveReference<Ts>> && ...)`, `(Ns + ... + 0)`, `(0 + ... + Ns)`: the pattern,
  // the side that names a pack ctx binds (the two binary folds parse alike), evaluated once per
  // element with the pack bound to it, the values combined in the fold's order; `&&` and `||`
  // stop at the operand that decides. A pattern naming a second pack, or a pack of the values a
  // function was given (`args...`), is no constant of step 1's.
  std::optional<ConstValue> constFold(FoldExpr* f, const TypeContext& ctx, int depth);

  std::optional<ConstValue> constName(NameExpr* n, const TypeContext& ctx);

  // `args...`, `forward<Args>(args)...`: a pack expansion (the parser's postfix `...`).
  static bool constIsExpansion(const Expr* e);

  // The names a pattern mentions (its NameExprs' single symbols), for the function parameter packs
  // it expands.
  static void constNamesIn(Node* n, std::vector<uint32_t>& out, int depth = 0);

  // The values a pack expansion's pattern gives (`forward<Args>(args)...`, a fold's operands): the
  // pattern once per element, each template parameter pack it names bound to that element's type
  // and each function parameter pack (a pack binding of the run) to that element's value. nullopt
  // when it names no pack, or packs of different lengths.
  std::optional<std::vector<ConstValue>> constExpand(Expr* pattern, const TypeContext& ctx, int depth, std::vector<std::optional<ConstPlace>>* places = nullptr, std::vector<uint8_t>* categories = nullptr);

  // The values of an argument list, pack expansions expanded; false when one is no constant.
  // places (when given) gets, for each value, the place of an argument written alone.
  bool constArguments(Span<Expr*> args, const TypeContext& ctx, int depth, std::vector<ConstValue>& values, std::vector<std::optional<ConstPlace>>* places = nullptr, std::vector<uint8_t>* categories = nullptr);

  // A function template's own parameters bound for a call (constCall's, constMemberCall's): the
  // written arguments (given), then what the arguments' types deduce, a pack the arguments from its
  // position on, a default for one neither written nor deduced. False when one stays unbound.
  bool constBindTemplate(FunctionDecl* f, const std::vector<const Ty*>& given, CallExpr* c, const std::vector<ConstValue>& values, const TypeContext& ctx, int depth, Subst& bound, LocalStack& params);

  // A call at compile time: a type's name converting a value (`size_t(5)`, `Tag(0)`), a trait
  // object as bool (`isOptional<T>()`), or a function run with its arguments bound (constRun).
  // returnPlace (a reference result asked for as a place, constPlaceOf): a named function's call only,
  // its result the place the function returns (`return add(uses, name);` of a Deque element).
  std::optional<ConstValue> constCall(CallExpr* c, const TypeContext& ctx, int depth, std::optional<ConstPlace>* returnPlace = nullptr);

  // `layers.size()`: a member function called on an object, run with the arguments of the
  // object's class instance bound. Step 1 has no objects: a body that reads a data member makes
  // no constant (constValueOfDecl), so only what the type decides is computed (Array::size).
  std::optional<ConstValue> constMemberCall(CallExpr* c, const TypeContext& ctx, int depth);

  // Function f's body run at compile time, its parameters bound to the arguments' values (and the
  // missing ones to their defaults) converted to the parameters' types; what it returns converted
  // to f's declared result type. A function returning void gives a value of type void.
  // ---- step 3: objects, arrays and pointers into them (design.md, "Compile-time evaluation") ----

  // ---- step 3, second part: class operators, conversions, virtual calls, references ----

  // The path from an object of class from to its base subobject of class to (`OptionalMapPosition`
  // given where a `MapPosition` is wanted: slicing), or nullopt when to is no base of from.
  std::optional<std::vector<size_t>> constBasePath(const Ty* from, const Ty* to, int depth);

  // The candidates a class offers for a member function or operator named symbol, its own or a
  // base's, and the arguments of the class they are found in for an object of type t.
  // templates: member function templates too (their own parameters the caller's to bind).
  std::vector<FunctionDecl*> constMembersNamed(const Ty* t, uint32_t symbol, Subst& subst, int depth, bool templates = false);

  // A free function named symbol (a free operator) visible from the scope and the arguments'
  // classes' namespaces, the arguments choosing among them (GlobalScope::findFree's search).
  // templates: function templates too, their arguments deduced.
  FunctionDecl* constFreeFunction(uint32_t symbol, const std::vector<const Ty*>& argTypes, Scope* scope, bool templates = false);

  // Set when constCallPlace runs a function (constArguments: a call whose result was no place is
  // not run again for its value).
  static thread_local bool constCallPlaceRan;

  // A member function returning a reference, run on the object at self for the place its result
  // designates (`result[i]` through operator[], `front()`): `this` refers to self, so that the place
  // is in the caller's object (a write through it reaches the object); nullopt for a result that
  // is no part of it.
  // ran: set to the call's value when its result is no place the run holds.
  std::optional<ConstPlace> constCallPlace(FunctionDecl* f, const Subst& subst, const std::vector<ConstValue>& args, const ConstPlace& self, int depth, const std::vector<std::optional<ConstPlace>>* places = nullptr, const std::vector<uint8_t>* categories = nullptr, std::optional<ConstValue>* ran = nullptr);

  // The member function of an object's class named symbol that a call with these arguments runs
  // for a place: one taking that many arguments, the non-const one where the object may be written.
  // Of several that take that many arguments: the one the walk chose (preferred), else the one the
  // arguments' types choose (argTypes, each known), else one of a pair that differs only in being
  // const; never one picked by the count alone (`config(StringView, i32)` beside
  // `config(StringView, bool)`).
  FunctionDecl* constPlaceMember(const Ty* t, uint32_t symbol, size_t args, bool writable, Subst& subst, int depth, Decl* preferred = nullptr, const std::vector<const Ty*>* argTypes = nullptr);

  // The declaration whose body a call of f runs: f, or its definition outside the class
  // (`Rectangle::Rectangle(int x, ...)` after the class: the game's .cpp half); null when none
  // has a body the evaluator sees (a C function, a declaration defined in no kpp source).
  // A call the evaluator cannot run, the reason kept (why): the function has no body it sees (a C
  // function: `printf`, `puts`; one declared and defined nowhere in the program).
  std::optional<ConstValue> constNoBody(FunctionDecl* f);

  FunctionDecl* constDefinition(FunctionDecl* f);

  // A function chosen for an object (or for none): run with these arguments, `this` the object.
  std::optional<ConstValue> constCallOn(FunctionDecl* f, const Subst& subst, const std::vector<ConstValue>& args, std::optional<ConstValue>* self, int depth, const ConstPlace* selfPlace = nullptr, const std::vector<std::optional<ConstPlace>>* places = nullptr, const std::vector<uint8_t>* categories = nullptr, std::optional<ConstPlace>* returnPlace = nullptr);

  // Whether an object is true in a condition (`if (optional)`, `!*this`): its conversion to bool
  // (or to an arithmetic type), explicit or not, its own or a base's.
  std::optional<bool> constTruth(const ConstValue& v, int depth);
  // A class object v converted to the scalar type t outside a condition (`int x = result;`, an
  // argument, a return): the conversion operator chooseConversion takes run on it (no explicit
  // one), its result converted to t.
  std::optional<ConstValue> constByConversionOperator(const ConstValue& v, const Ty* t, int depth);

  // Whether the operator of a class is `= default` (`operator<=>`, `operator==`).
  static bool constDefaulted(FunctionDecl* f) { return f && (f->flags & DF_Default) && !f->body; }

  // An operator a class declares for op, or `= default` (a defaulted `<=>` implies `==`).
  FunctionDecl* constClassOperator(const Ty* t, P op, const std::vector<const Ty*>& argTypes, Subst& subst, int depth);

  // `a == b` of two objects of one class compared member by member (a defaulted `==`, or the one a
  // defaulted `<=>` implies): bases, then members, each by its own `==`.
  std::optional<bool> constMemberwiseEqual(const ConstValue& l, const ConstValue& r, const TypeContext& ctx, int depth);

  // `a <=> b` of two objects of one class with a defaulted `<=>`: the first part that differs
  // decides, as -1, 0 or 1; nullopt where a part's order is no number's (a class with an
  // operator<=> of its own).
  std::optional<int> constMemberwiseOrder(const ConstValue& l, const ConstValue& r, const TypeContext& ctx, int depth);

  // An operator applied to values at least one of which is an object or a pointer: pointer
  // arithmetic; a class's member operator, a free one, or what C++20 rewrites (`a != b` as `!(a
  // == b)`, `b == a` reversed, `<` and the others by a defaulted `<=>`, `==` by memberwise
  // comparison where `=` default).
  // Whether f returns an lvalue reference (`String& operator<<(String&, Text)`).
  bool constReturnsLRef(FunctionDecl* f, const Subst& subst);
  // The value a call run for its reference result (ran: its outcome, place: where the result
  // refers to) reads there; nullopt when it did not run or refers to nothing.
  std::optional<ConstValue> constReferred(const std::optional<ConstValue>& ran, std::optional<ConstPlace>& place);

  // A chain of binary operators (`1 + 1 + ... + 0`, `out << a << b`), evaluated down the left
  // operands in a loop, not nested once per term. An operator returning a reference (String's
  // `<<`) refers to an object: the next operator runs on that object, never on a copy, and place
  // (when asked for) is where the last one's result refers to.
  std::optional<ConstValue> constBinaryChain(BinaryExpr* e, const TypeContext& ctx, int depth, std::optional<ConstPlace>* place = nullptr);

  struct ConstOperand; // (after ConstPlace)
  // node: the expression evaluated, whose operator function the checker recorded (design.md 24), or null.
  std::optional<ConstValue> constBinaryValues(P op, const ConstValue& l, const ConstValue& r, const TypeContext& ctx, int depth, const ConstOperand* lo = nullptr, const ConstOperand* ro = nullptr, std::optional<ConstPlace>* returned = nullptr, const BinaryExpr* node = nullptr);

  // The function a virtual call on an object of dynamic type t runs: the final overrider of f
  // (the most derived class's function of its name and parameter types), or f.
  FunctionDecl* constOverrider(FunctionDecl* f, const Ty* t, int depth);


  // The binding a member function or constructor runs with as `this`: the object it works on.
  static constexpr uint32_t constThisSymbol = 0xFFFFFFFEu;
  // The binding of a structured binding declaration's hidden object (`auto [a, b] = e;`), which
  // its names refer into (constBindNames); ConstBinding::hiddenOf says whose.
  static constexpr uint32_t constHiddenSymbol = 0xFFFFFFFCu;

  // Values made by one evaluation (objects' parts, arrays' elements): bounded like its steps.
  static uint64_t& constValuesMade();
  bool constMayMake(size_t n);

  // The parts of a class's objects as the interpreter holds them: its bases, then its non-static
  // data members in declaration order (a bit field one of its own), with their types under the
  // class's arguments.
  struct ConstClassLayout
  {
    ClassDecl* decl = nullptr;
    Subst subst;
    std::vector<const Ty*> bases;
    std::vector<VariableDecl*> fields;
    std::vector<const Ty*> fieldTypes;
    std::vector<uint8_t> fieldBits; // a bit field's width, 0 for a whole member
    std::vector<FunctionDecl*> constructors;
    bool ok = false;
    // A member is an anonymous struct or union: its field is null, its type that class's, its
    // members reached through it (constFieldPath). No aggregate initialization goes through one.
    bool anonymous = false;
    std::vector<CollectedUse> uses; // what computing it read (a recording build's): replayed to each asker
  };

  // A field no initializer of an aggregate names: an unnamed bit field (`int : 0;`), padding.
  static bool constUnnamedField(const ConstClassLayout& layout, size_t i) { return !layout.fields[i] || layout.fields[i]->name.empty(); }

  // `using Vector2::Vector2;`: a class with a base's constructors (no aggregate, C++20).
  static bool constInheritsConstructors(const ConstClassLayout& layout);

  // An integer stored in a bit field of the given width: its low bits, read back sign-extended for
  // a signed field type (an enumeration's by its underlying type), as the generated code reads it.
  ConstValue constFitBits(ConstValue v, const Ty* fieldType, unsigned bits);

  // The parts of an object of a class with this layout, each bit field's part fitted to its width.
  void constFitBitFields(const ConstClassLayout& layout, std::vector<ConstValue>& parts);
  std::mutex constLayoutsMutex;
  std::unordered_map<const Ty*, std::shared_ptr<ConstClassLayout>> constLayouts;
  const ConstClassLayout* constLayoutOf(const Ty* t, int depth);

  // The parts to follow from an object of class t to its data member field (through the bases it
  // is declared in), or nullopt.
  std::optional<std::vector<size_t>> constFieldPath(const Ty* t, Decl* field, int depth);

  // The value at path inside v, or nullopt when a part on the way is no object or is unset.
  static std::optional<ConstValue> constPart(const ConstValue& v, const std::vector<size_t>& path);

  // v with the part at path replaced by part: new parts along the path, the rest shared. Parts no
  // other value shares (the binding's own) are written in place: an element written into a long
  // array costs no copy of the array.
  static bool constSetPart(ConstValue& v, const std::vector<size_t>& path, size_t from, const ConstValue& part);

  // The type an argument of this value has, for choosing a constructor or an operator.
  const Ty* constTypeOfValue(const ConstValue& v);

  // A string literal's bytes (adjacent ones joined, escapes decoded), read from the file it is in
  // (or `in`); false when its tokens are not at hand.
  bool literalBytes(LiteralExpr* l, const ParsedFile* in, std::string& bytes);

  // A string literal (adjacent ones joined) as an array of its chars and a terminating zero.
  std::optional<ConstValue> constString(LiteralExpr* l, const TypeContext& ctx);

  // An object of type t, initialized empty: value-initialization (zero: scalars zero, `T{}`, an
  // aggregate's missing elements) or default-initialization (a scalar unset, `T x;`), a class's
  // members from their default member initializers either way, and its default constructor
  // run when it has a user-provided one. target: where the object lives (a local's binding), the
  // constructor run there, its `this` that place (constRunConstructor).
  std::optional<ConstValue> constEmpty(const Ty* t, bool zero, int depth, const ConstPlace* target = nullptr);

  // A field's value where no initializer of the object's names it: its default member
  // initializer, under the class's arguments, else empty.
  std::optional<ConstValue> constFieldDefault(const ConstClassLayout& layout, size_t i, bool zero, int depth);

  // The value an initializer gives an object of type t: a brace list (an aggregate's elements, a
  // constructor's arguments), an expression converted to t (a converting constructor for a
  // class, the array of a string literal for a char array, a scalar's conversions).
  // Whether copying an object of class t (or an array of them) runs code: a copy or move constructor
  // that is not trivial (a user-provided one, a member's or a base's). Otherwise a copy is the value.
  bool constCopyRunsCode(const Ty* t);

  // An expression's value category for the copy of a class object it initialises: 0 a prvalue (the
  // object itself), 1 an lvalue (copied), 2 an xvalue (moved). source: what `move(x)`, a
  // `forward<T>(x)` of an rvalue or `static_cast<T&&>(x)` names, else e itself.
  uint8_t constCategory(Expr* e, Expr*& source);

  // An object of class t copied (moving: moved) from value v, whose place (if any) is at: t's own copy
  // or move constructor run, the source's place given to it (a move empties its source); an implicit
  // or defaulted one member by member, each part copied as its own type copies.
  std::optional<ConstValue> constCopyConstruct(const Ty* t, const ConstValue& v, const std::optional<ConstPlace>& at, bool moving, int depth, const ConstPlace* target = nullptr);

  // Whether assigning an object of type t (an array: its elements) runs code: an operator= of its
  // own or of a part's. Else the assignment writes the value.
  bool constAssignRunsCode(const Ty* t);

  // The object of type t at place assigned value v, which is at from (if anywhere) with value category
  // category (0 a prvalue, 1 an lvalue, 2 an xvalue): t's own operator= run on it there, an implicit
  // or defaulted one member by member, each part assigned as its own type assigns
  // ([class.copy.assign]: a String member's operator= copies its characters, never its pointer).
  bool constAssignObject(const ConstPlace& place, const Ty* t, const ConstValue& v, const std::optional<ConstPlace>& from, uint8_t category, int depth);

  // An object of class t initialised from expression init, a value of t itself, as C++ initialises one:
  // a prvalue is the object (elided); an lvalue is copied and an xvalue (`move(x)`) moved by t's copy
  // or move constructor (constCopyConstruct). The outer nullopt: init is not such a copy, or copying t
  // runs no code. evaluated: set to init's value when it was evaluated and is of another class (a
  // converting constructor's argument), which the caller then takes rather than evaluating init
  // again (`C c = *it++;` advanced it twice). An xvalue whose place is not known is no constant:
  // its move would leave the object it names as it was.
  std::optional<std::optional<ConstValue>> constCopyInit(Expr* init, const Ty* t, const TypeContext& ctx, int depth, bool moving = false,
                                                         std::optional<ConstValue>* evaluated = nullptr, const ConstPlace* target = nullptr);

  // Whether v is an object of class u itself (cv-qualification and references aside).
  bool constIsObjectOf(const ConstValue& v, const Ty* u);

  // The object of class u made from v, an object of u itself, as the category of the expression
  // that gave it chooses (0 a prvalue, 1 an lvalue, 2 an xvalue; at: where it is, when known): a
  // prvalue is the object itself, an lvalue is copied and an xvalue moved by u's own constructor
  // (design.md "Objects of the run"); an xvalue whose place is not known is no constant.
  std::optional<ConstValue> constFromObject(const Ty* u, const ConstValue& v, const std::optional<ConstPlace>& at, uint8_t category, int depth, const ConstPlace* target = nullptr);

  // `T(args)`, a functional cast or a call of the class's name (`T b(a)` too, which the parser
  // writes as `T b = T(a)`): the object the constructor the arguments choose makes, one argument
  // that is an object of the class itself copied or moved by the class's constructor
  // (constFromObject), which constConstruct, given values without their categories, takes as it is.
  std::optional<ConstValue> constConstructFrom(const Ty* t, Span<Expr*> args, const TypeContext& ctx, int depth, const ConstPlace* target = nullptr);
  // `T(args)`, `T{args}`, `T()` of a class type t (a FunctionalCastExpr, also `T x(args)`'s): the
  // object made (none when it is no constant), or nullopt when t is no class. target: where it
  // lives, its constructor run there (constRunConstructor).
  std::optional<std::optional<ConstValue>> constClassCast(FunctionalCastExpr* f, const Ty* t, const TypeContext& ctx, int depth,
                                                          const ConstPlace* target = nullptr);

  std::optional<ConstValue> constInitialize(const Ty* t, Expr* init, const TypeContext& ctx, int depth, const ConstPlace* target = nullptr);

  std::optional<ConstValue> constInitializeIn(const Ty* t, Expr* init, const TypeContext& ctx, int depth, const ConstPlace* target = nullptr);

  // `char name[8] = "pipe";`, `char text[] = "1.57";`: the literal's characters and its terminating
  // zero, the rest of the array zero ([dcl.init.string]); a literal longer than the array is no
  // constant. Nullopt when t is no char array or init no string literal.
  std::optional<ConstValue> constCharArrayFromLiteral(const Ty* t, Expr* init, const TypeContext& ctx, int depth);

  // `{1, 2, 3}` for an array: its elements in order, the rest value-initialized; brace elision
  // lets an element that is itself an array or aggregate take several values.
  std::optional<ConstValue> constArrayInit(const Ty* t, Span<Expr*> values, const TypeContext& ctx, int depth);

  // The object of type t (an array or an aggregate class) that values from next on initialize,
  // as many as it takes (brace elision), next moved past them.
  std::optional<ConstValue> constElided(const Ty* t, Span<Expr*> values, size_t& next, const TypeContext& ctx, int depth);

  // An aggregate (C++20): a class without user-declared constructors, virtual functions or
  // virtual bases (its bases aggregates too, as far as the interpreter needs).
  bool constIsAggregate(const Ty* t, int depth);

  // `T{...}` and `T x = {...}` for a class: an aggregate's members in order (designators
  // naming them), or the arguments of the constructor they choose.
  std::optional<ConstValue> constBraceInit(const Ty* t, BraceInitExpr* b, const TypeContext& ctx, int depth, const ConstPlace* target = nullptr);

  // A value given type t where an initialization converts implicitly: an object of t as it is; to
  // a class, by a constructor marked implicit taking the value; else the scalar conversions.
  std::optional<ConstValue> constConvertTo(std::optional<ConstValue> v, const Ty* t, int depth);

  // `T(args)`: the object the constructor the arguments choose makes (only one marked implicit
  // when the construction is an implicit conversion); an object of T given alone is taken as it
  // is, its category not known here (a caller that knows it copies or moves it by T's constructor
  // first: constFromObject); an aggregate takes the arguments as its members (C++20).
  // written: the argument expressions, when the construction has them: where the values' types leave
  // two constructors equally good (`Pair(A&&, B&&)` and `Pair(const A&, B&&)` for a string literal
  // and an enumerator), the types the walk gave them, value categories included, decide.
  // A typed union (design.md 8.5) made from a value of exactly one alternative's type, or from a
  // constant tag and that alternative's arguments: the tag, and that alternative alive in the
  // storage (the others not). Nullopt for anything else.
  std::optional<ConstValue> constTypedUnion(const Ty* t, const ConstClassLayout& layout, const std::vector<ConstValue>& args, int depth);

  std::optional<ConstValue> constConstruct(const Ty* t, const std::vector<ConstValue>& args, bool implicitOnly, int depth, const Span<Expr*>* written = nullptr, const ConstPlace* target = nullptr,
                                           const std::vector<std::optional<ConstPlace>>* places = nullptr);

  // ---- step 4: brace lists, InitializerList, range-for (design.md, "Compile-time evaluation") ----

  // A brace list's value before it meets the type it initializes (an argument, `f({1, 2})`; an
  // element of another list, `{{A, B}, 7}`): an object of the brace list type overload resolution
  // sees (TypeTable::braceList of its first typed element's type), its parts the elements; `{}` an
  // object of no type. A designated list is no value without its type.
  static bool constIsBraceList(const ConstValue& v) { return v.isObject() && (!v.type || v.type->isBraceList()); }

  // places, categories: the elements' places and value categories, as constArguments gives them.
  std::optional<ConstValue> constBraceList(BraceInitExpr* b, const TypeContext& ctx, int depth, std::vector<std::optional<ConstPlace>>* places = nullptr,
                                           std::vector<uint8_t>* categories = nullptr);

  // A brace list value given the type it initializes: an InitializerList of its elements, an
  // array's elements, a class's InitializerList constructor (C++'s list-initialization tries
  // those first) or else the constructor the elements choose (an aggregate's members), a scalar's
  // one element or zero. With the elements' places and categories (constBraceList's), one element
  // that is an object of the class itself is copied or moved (constFromObject).
  std::optional<ConstValue> constFromBraceList(const ConstValue& list, const Ty* t, int depth, const Span<Expr*>* written = nullptr,
                                               const std::vector<std::optional<ConstPlace>>* places = nullptr,
                                               const std::vector<uint8_t>* categories = nullptr, const ConstPlace* target = nullptr);

  // An InitializerList<E> of these elements: a constant array of them converted to E (what the
  // brace list makes) and the list's two fields, a pointer to its first element and the count
  // (InitializerList.kpp keeps them first); an empty list's pointer is null.
  std::optional<ConstValue> constInitializerList(const Ty* u, const std::vector<ConstValue>& elements, int depth);

  // `Flags f = {A, B};`: the object a class's constructor taking an InitializerList (its other
  // parameters defaulted) makes of the elements; nullopt when the class has none, or the elements
  // do not convert to its element type (the other constructors are tried then).
  std::optional<ConstValue> constByInitializerList(const Ty* u, const std::vector<ConstValue>& elements, int depth, const ConstPlace* target = nullptr);

  // ---- step 4: addresses of declarations and member pointers ----

  // Whether a variable lives as long as the program (a global, a static member, a static local),
  // so that its address is a constant; a reference variable's address is what it refers to's. A
  // global declared `extern` is one too, wherever it is defined (Lua's `&luaO_nilobject_` after
  // lobject.h's `extern const TValue luaO_nilobject_;`, a constant in C).
  static bool constStaticStorage(Decl* d);

  // The class template instance a member is named through: the qualifier's (`Table<int>::rows`), else
  // the context's instance of the member's class; null for no member, a class that is no template,
  // or no instance in sight.
  const Ty* constInstanceOf(Decl* d, const QualifiedName& name, const TypeContext& ctx, int depth);

  // The type of the part of an object of type t that path leads to.
  const Ty* constPartType(const Ty* t, const std::vector<size_t>& path, size_t count, int depth);

  // The declaration's type of what an Address names, its arguments applied.
  const Ty* constTargetType(const ConstTarget& t, int depth);

  // What an lvalue of static storage designates (`origin`, `table[1]`, `box.leftTop`, `*where`,
  // `where->y`): the target and the type of the object there.
  struct ConstDesignated
  {
    ConstTarget target;
    const Ty* type = nullptr;
  };
  std::optional<ConstDesignated> constDesignated(Expr* x, const TypeContext& ctx, int depth);

  // A function as a value (`&twice`, `thrice` where a pointer is wanted): its address; a member
  // function that is not static named through its class (`&Calc::mul`) is a pointer to member.
  std::optional<ConstValue> constFunctionAddress(FunctionDecl* f, const QualifiedName& name, const TypeContext& ctx, int depth);

  // `&x`: the address of what x designates (a variable of static storage, a part of one), of a
  // function, or a pointer to a member (`&Point::y`); nullopt for the address of a local of the run.
  std::optional<ConstValue> constAddressOf(Expr* operand, const TypeContext& ctx, int depth);

  // An Address moved by n elements along the array it points into (`second - 1`); nullopt outside
  // the array, one past its end allowed.
  std::optional<ConstValue> constAddressMove(const ConstValue& p, int64_t n, int depth);

  // Whether an Address names an object (not one past an array's end).
  bool constAddressDereferenceable(const ConstValue& p, int depth);

  // The value an Address points at: the variable's constant value (a constexpr or const one; any
  // other is no constant at compile time), the part its path leads to.
  std::optional<ConstValue> constReadTarget(const ConstValue& p, int depth);

  // `==` and `!=` of addresses (the same declaration and part, or not), their order within one
  // array, `p + n` and `p - n`, and an address given where a pointer is compared with null.
  std::optional<ConstValue> constAddressArith(P op, const ConstValue& l, const ConstValue& r, int depth);

  // A lambda called where it is written (`constexpr Shape shape = []() -> Shape { ... }();`): its
  // body run as a function's, in the context it is written in. One that captures, or a generic
  // one, is no constant yet.
  std::optional<ConstValue> constCallLambda(LambdaExpr* l, CallExpr* c, const TypeContext& ctx, int depth);
  // What a lambda captures, in its closure's order: the captures written (`[a, &b, x = e, this]`),
  // then for `[=]` or `[&]` each variable of the functions around that the body names, and `this`
  // where the body uses it.
  struct ConstCapture
  {
    uint32_t symbol;
    bool byRef;
    bool isThis;
    Expr* init; // an init-capture's
    Decl* decl; // an init-capture's variable
  };
  bool constClosureCaptures(LambdaExpr* l, std::vector<ConstCapture>& out);
  // A lambda as a value (`auto twice = [k](int v) { return v * k; };`): its closure, of type type,
  // whose parts are the captures (a copy, or for one by reference and `this` the place's address).
  std::optional<ConstValue> constClosure(LambdaExpr* l, const Ty* type, const TypeContext& ctx, int depth);
  // A closure called: the lambda's body run where it is written, its captures bound by name (a copy
  // read only, one by reference referring to its object), its parameters to the arguments.
  std::optional<ConstValue> constCallClosure(LambdaExpr* l, const ConstValue& closure, CallExpr* c, const TypeContext& ctx, int depth);

  // A function pointer's call (`f(v)` with f a parameter holding `&twice`, `ops[0](5)`): the
  // function it points at, run with the arguments.
  std::optional<ConstValue> constCallThrough(const ConstValue& fn, CallExpr* c, const TypeContext& ctx, int depth);

  // `object.*member`, `pointer->*member` of a pointer to a data member: that member of the object.
  std::optional<ConstValue> constMemberPointerRead(BinaryExpr* b, const TypeContext& ctx, int depth);

  // The value of a range's begin() or end() (`for (auto v : flags)`), run on the object.
  // at: the range's place, where begin() runs (its result may point into it); write: the loop
  // writes the elements (`for (auto& v : table)`), so the begin() that is not const.
  std::optional<ConstValue> constRangeEnd(const ConstValue& range, uint32_t symbol, int depth, const ConstPlace* at = nullptr, bool write = false);

  // Constructor ctor of class t run at compile time: the bases and members its initializers name
  // from them (a delegating one makes the whole object), the others from their defaults, then
  // its body with `this` the object.
  // own: a constructor template's arguments (with the class's), when ctor is one.
  // declared: the declaration in the class when ctor is its definition after the class (its
  // default arguments, its scope).
  // target: the place the object is made in (a local's binding), written with the members before
  // the body runs on it there, so that what the body points at the object (a List's sentinel at
  // itself) points at the object where it lives, never at a binding of the constructor's own.
  std::optional<ConstValue> constRunConstructor(FunctionDecl* ctor, const Ty* t, const std::vector<ConstValue>& args, int depth, const Subst* own = nullptr, FunctionDecl* declared = nullptr, const std::vector<std::optional<ConstPlace>>* places = nullptr, const ConstPlace* target = nullptr);

  // A constructor's body run with `this` the object its initializers made: the object as the
  // body leaves it; with target, written there first and run on it there.
  std::optional<ConstValue> constRunBody(FunctionDecl* ctor, const Ty* t, const ConstValue& object, TypeContext& fctx, std::vector<ConstBinding>& env, int depth, const ConstPlace* target = nullptr);

  // The `this` binding of the member function being run, or null.
  static ConstBinding* constThis(const TypeContext& ctx);

  // Where an assignment stores and a reference refers to: a local (or parameter) of a function
  // being run, or a part of one (`point.x`, `items[i]`, `this->count`, a member named alone in a
  // member function); a reference's place is what it refers to.
  struct ConstPlace
  {
    std::vector<ConstBinding>* env = nullptr; // by index: the locals may grow meanwhile
    size_t index = 0;
    std::vector<size_t> path;
    const Ty* type = nullptr; // the part's type, which a stored value is converted to
    bool readOnly = false;    // reached through something const (a reference to const)
    uint8_t bits = 0;         // a bit field's width, which a stored value is fitted to
    ConstBinding& binding() const { return (*env)[index]; }
    bool writable() const { return !readOnly && binding().writable; }
  };

  // An operand of a binary operator: where its object is (an lvalue's place, a prvalue's
  // temporary) and its value category, for a class's operator to run on it there.
  struct ConstOperand
  {
    std::optional<ConstPlace> place;
    uint8_t category = 1;
  };
  // An operand's type as overload resolution sees it (argumentType): an lvalue as `T&`, `const T&`
  // where it is reached as const, so that `a << b` of an lvalue b takes `const Acc&` over `Acc&&`.
  const Ty* constOperandType(const ConstValue& v, const ConstOperand* o);

  ConstOperand constOperandOf(Expr* e, const ConstValue& v, const TypeContext& ctx, int depth);

  // A place with the references on the way followed to what they refer to.
  static ConstPlace constResolve(ConstPlace p);

  // The place of the local or parameter an expression names (`count`, `(count)`), or nullopt.
  // The evaluation's own objects whose address it took (`&x`), by serial: where each is while it
  // lives. A run's locals are a frame, listed while it runs (ConstFrame), so that an Address into
  // one that ended is known as such, never read as what took its place.
  struct ConstStorage
  {
    std::vector<ConstBinding>* env = nullptr;
    size_t index = 0;
    uint32_t symbol = 0;
    const Ty* type = nullptr; // the whole object's
  };
  static std::unordered_map<uint64_t, ConstStorage>& constStorage();
  static std::vector<const void*>& constFrames();
  struct ConstFrame
  {
    explicit ConstFrame(const std::vector<ConstBinding>* env) { constFrames().push_back(env); }
    ~ConstFrame() { constFrames().pop_back(); }
  };

  // The file of the function running, for where the evaluation allocates and frees.
  static const ParsedFile*& constRunningFile();
  struct ConstRunning
  {
    const ParsedFile* outer = constRunningFile();
    explicit ConstRunning(const ParsedFile* file)
    {
      if (file)
        constRunningFile() = file;
    }
    ~ConstRunning() { constRunningFile() = outer; }
  };
  struct ConstWhere
  {
    const ParsedFile* file = nullptr;
    uint32_t token = 0;
  };
  ConstWhere constHere(const Expr* e) { return ConstWhere{constRunningFile(), e->token}; }
  // The innermost statement of the outermost evaluation that failed (runBuild's "build.kpp:12").
  static ConstWhere& constFailedAt();
  // "Build.kpp:12": the file's name and the line.
  static std::string constWhereText(const ConstWhere& w);

  // Memory the outermost evaluation allocated (C++20's transient allocation): one binding per
  // allocation, which nothing destroys on its own (an Address names it by serial, as a local's),
  // and where it was allocated and freed. All of it is freed before the evaluation ends.
  struct ConstAllocation
  {
    ConstWhere allocated, freed;
    bool array = false; // by new[]
    bool isFreed = false;
    // By malloc, calloc or realloc: bytes, untyped until a cast or a placement new gives them a
    // type (k objects of T, uninitialized; zero for calloc's).
    bool raw = false;
    bool zero = false;
    int64_t bytes = 0;
  };
  static std::vector<ConstBinding>& constHeap();
  static std::vector<ConstAllocation>& constAllocations();

  // An Address of the object at place (a binding of a running frame, a part of it): the serial
  // its binding is named by from now on.
  ConstTarget constStorageTarget(const ConstPlace& place);

  std::string constStorageName(const ConstStorage& s);

  // The object an Address of storage names, while it lives; else nullopt, why naming it ("a read
  // of x, whose lifetime ended").
  const ConstStorage* constLiveStorage(const ConstTarget& t, const char* use);

  // The place an Address of storage points at (p of a pointer type, its pointee the place's type).
  std::optional<ConstPlace> constStoragePlace(const ConstValue& p, const char* use, int depth);

  // The first storage a value points at, in itself or its parts; null for none.
  const ConstStorage* constHeldStorage(const ConstValue& v, int depth = 0);

  // `malloc(n)`, `calloc(k, n)`, `realloc(p, n)`, `free(p)` at compile time: bytes from the
  // evaluation's own memory (which it must free before it ends, as new's), typed by the first cast
  // to T* (constTypeAllocation); free of what malloc did not allocate, twice, or of a pointer into
  // a block is an error, as delete's are.
  std::optional<ConstValue> constCAllocation(std::string_view name, const std::vector<ConstValue>& args, CallExpr* c, int depth);

  // Memory malloc allocated, untyped, given type t by the first cast to t* (or placement new of a
  // t into it): as many objects of t as fit, uninitialized (zero for calloc's); the pointer to the
  // first of them. Nullopt when p is no such memory (already typed: no new type for it).
  // retype: also bytes already typed (as chars), at their start, when no element was written yet.
  std::optional<ConstValue> constTypeAllocation(const ConstValue& p, const Ty* t, int depth, bool retype = false);

  // An object of t in malloc's bytes, as C++20 creates one there implicitly (an object of an
  // implicit-lifetime type: a scalar, an array, a class that is no union and has no virtual
  // function), run by no constructor: each scalar part unset, or zero for calloc's, so that a
  // member is written, addressed or made by placement new (`new (&node->value) T(...)` in a List
  // node); a class of another kind is one unset value, none of its members a constant.
  std::optional<ConstValue> constRawObject(const Ty* t, bool zero, int depth);

  // The type of the object an Address points at (its target's part, whatever the pointer's own
  // type says: `void*`), or null.
  const Ty* constPointedType(const ConstValue& p, int depth);

  // A scalar's value read as another scalar type of the same size (the bits kept: `memcpy` of a
  // uint32_t into a float), or nullopt.
  std::optional<ConstValue> constBitCast(const ConstValue& v, const Ty* to, int depth);

  // `memcpy(to, from, n)` and `memmove` of the evaluation's own objects, by their types: the whole
  // object when n is its size and both are of one type (its value copied), a scalar into another of
  // its size (its bits), or n bytes of elements of one type along two arrays (copied as if through
  // a buffer). Anything else is no constant: the evaluator has no bytes, only typed values.
  std::optional<ConstValue> constMemcpy(const ConstValue& to, const ConstValue& from, const ConstValue& count, int depth);

  // `memcmp(a, b, n)` of chars (or other bytes) through any pointer the evaluation holds (a string
  // literal's, its own arrays', a constant's): the first difference's sign, as unsigned bytes.
  // Elements of a wider type are compared by no bytes the evaluator has: no constant.
  std::optional<ConstValue> constMemcmp(const ConstValue& a, const ConstValue& b, const ConstValue& count, int depth);

  // `memset(to, c, n)` of the evaluation's own objects: n bytes of chars, or zero over whole objects
  // or elements (each value-initialized to zero). Anything else is no constant.
  std::optional<ConstValue> constMemset(const ConstValue& to, const ConstValue& byte, const ConstValue& count, int depth);

  // Whether the object at a resolved place may have its address taken: one the run made (a local,
  // a parameter, a temporary), the object a function runs on, or one in memory the evaluation
  // allocated. Not a copy the run holds of something else's (a reference it could not bind).
  static bool constAddressable(const ConstPlace& p);

  // The Text the arguments from `from` on make for a Text parameter (design.md 14), as code
  // generation builds it: each piece the address of its argument (a temporary of the full
  // expression for one that has no place) and the library's function appending it, the Text a
  // pointer to the pieces and their count. Nullopt for an argument that is no piece.
  std::optional<ConstValue> constTextOf(const std::vector<ConstValue>& values, const std::vector<std::optional<ConstPlace>>* places, size_t from, int depth);

  // The temporaries of the full expression being evaluated (constTemps), or null outside any.
  static std::vector<ConstBinding>*& constTempList();

  // The temporaries of one full expression (a statement's, a condition's, the outermost
  // evaluation's): objects the run made that no variable holds (a prvalue a reference parameter
  // binds, the object a member function runs on, a value discarded), destroyed when it ends
  // (constEndTemps), the last made first.
  struct ConstTemps
  {
    std::vector<ConstBinding> list;
    ConstFrame frame{&list};
    std::vector<ConstBinding>* outer = constTempList();
    ConstTemps() { constTempList() = &list; }
    ~ConstTemps() { constTempList() = outer; }
  };

  // What a throw in a constant evaluation throws, and where, as the reason it is no constant:
  // `it throws RuntimeError("a negative value") (Html.kpp:12)`, the message read through the
  // object's what(); a string literal's text; `it rethrows` for `throw;`.
  std::string constThrown(ThrowExpr* t, const TypeContext& ctx, int depth);

  // A temporary of class t holding v, destroyed when the full expression ends: its place, or
  // nullopt where no full expression is being evaluated.
  std::optional<ConstPlace> constMaterialize(const ConstValue& v, const Ty* t);

  // Whether destroying an object of type t runs code (a destructor of its own, a member's, a base's).
  bool constDestroyRunsCode(const Ty* t);

  // The object of type t whose value is v destroyed as the program destroys it: its destructor's
  // body run on it, then its members and its bases, the last first; an array's elements, the last
  // first. false: that is no constant (a destructor the evaluator cannot run). at: where the object
  // is (a local's binding, a heap block): its destructor runs on it there, so that the object's
  // pointers at itself (a List's sentinel, made in place) compare equal to its own addresses.
  bool constDestroy(const Ty* t, const ConstValue& v, int depth, const ConstPlace* at = nullptr);

  // The bindings env holds past mark ended, the last first: each object the run owns there
  // destroyed (run: false for a run that failed, which destroys nothing). false: a destructor is no
  // constant.
  bool constEndLocals(std::vector<ConstBinding>& env, size_t mark, int depth, bool run = true);

  bool constEndTemps(ConstTemps& temps, int depth, bool run = true) { return constEndLocals(temps.list, 0, depth, run); }

  // The object `new T...` makes, of type t, from the new-expression's initializer: `T` default-
  // initialized, `T()` value-initialized, `T(a, b)` and `T{a, b}` from them.
  std::optional<ConstValue> constNewObject(NewExpr* n, const Ty* t, const TypeContext& ctx, int depth);

  // `new T(args)`, `new T[n]{...}`: an object, or n of them, in memory the evaluation allocates
  // (C++20's transient allocation: freed before it ends); a pointer to it, to the first of the n.
  // `new (p) T(args)`: the object made where p points.
  std::optional<ConstValue> constNew(NewExpr* n, const TypeContext& ctx, int depth);

  // `delete p`, `delete[] p` of memory `new` allocated: the object (each element, the last first)
  // destroyed, the memory freed. Null: nothing. Anything else is an error, as in C++20.
  std::optional<ConstValue> constDelete(DeleteExpr* d, const TypeContext& ctx, int depth);

  // build.kpp's description (GlobalScope::runBuild, design.md 12): a Project default-constructed,
  // its member configuration (when it has one: projectlib's has none, its build() describes every
  // configuration) the enumerator of that name, build(project) run on it, then describe(project),
  // whose String's characters are the answer; both destroyed after, so memory left allocated is an
  // error as in any constant. Nullopt with error, "Build.kpp:12: why", when that is no constant.
  std::optional<std::string> runBuild(std::string_view configuration, std::string& error);

  std::optional<ConstPlace> constLocal(Expr* e, const TypeContext& ctx);

  // Whether e names, alone, an object of the running function itself: a local or a by-value
  // parameter, not a reference (what `return e;` moves).
  bool constOwnLocal(Expr* e, const TypeContext& ctx);

  std::optional<ConstPlace> constThisPlace(const TypeContext& ctx);

  // The data member a member access or a name alone in a member function names: the walk's
  // binding, or, in a body no walk has typed yet (a constexpr member function of a class declared
  // further down, BoundingBox::isZeroSize called from a file walked first), the non-static data
  // member of that name in the class of the object the evaluation holds.
  Decl* constFieldNamed(Decl* resolved, const QualifiedName& name, const Ty* objectType);

  // calls: through member functions' reference results too (`result[i]` by an Array's operator[],
  // `front()`), which runs them: only where the expression is not evaluated for its value as well.
  // Whether expression e, a pointer, may point at an object of the evaluation (an Address of
  // storage): its type is a pointer and the run has locals. Only those are evaluated for a place.
  bool constPointsIntoStorage(Expr* e);

  // Whether evaluating e changes nothing (names, members, indexes, arithmetic, casts): what a place
  // is looked for by evaluating, and then evaluated again where it is no place, may be only that.
  bool constPure(Expr* e, int depth = 0);

  // `it->member` of an iterator object (value v of expression e): its class's operator-> run on it
  // where it is (an lvalue's place, else a temporary), the pointer it gives; nullopt for no object.
  std::optional<ConstValue> constArrowOf(Expr* e, const ConstValue& v, const TypeContext& ctx, int depth);

  // `hasher(key)`, `this->compare(a, b)`: an object of a class called, its operator() run on it where
  // it is (an lvalue's place, else a temporary), with the arguments where they are.
  // field: the data member a member call names (`this->hasher(key)`, whose name the walk may have
  // bound to the operator() it calls), found in the object it is a member of.
  std::optional<ConstValue> constCallObject(Expr* object, CallExpr* c, const TypeContext& ctx, int depth, Decl* field = nullptr);

  // ran: where e is a call it runs (calls) whose reference result is no place the run holds (a
  // reference into a global constant), that call's value, so that the caller takes it rather than
  // running the call again.
  std::optional<ConstPlace> constPlaceOf(Expr* e, const TypeContext& ctx, int depth, bool calls = false, std::optional<ConstValue>* ran = nullptr);

  // An operand of class type that a call gives as an lvalue or an xvalue (`v[i]` through a class's
  // operator[], `v.front()`, `get(v)`), for an operator's function that takes it by reference: the
  // call run once, on the object where it is, for the place its result designates, the operand's
  // value read there (place: set), so that `this != &other` in String's operator= compares; a
  // reference result the run holds no place of gives the value the call computed. The outer
  // nullopt: e is no such operand (the caller evaluates it as before); the inner: no constant (the
  // call ran, and is never run again).
  std::optional<std::optional<ConstValue>> constPlacedOperand(Expr* e, const TypeContext& ctx, int depth, std::optional<ConstPlace>& place);

  // The value at a place, unset included.
  static std::optional<ConstValue> constPartOrUnset(const ConstPlace& place);

  // Whether p (resolved) is in memory the evaluation freed, the reason kept (a reference into a
  // Vector's block that a push_back reallocated: what it refers to is gone).
  static bool constFreedPlace(const ConstPlace& p, const char* use);

  // The value at a place, nullopt while unset.
  static std::optional<ConstValue> constRead(const ConstPlace& place);

  // A value stored at a place (its parts on the way copied).
  bool constWrite(const ConstPlace& place, const ConstValue& v);

  // The width of field (a bit field's, else 0) in an object of class t, its bases searched.
  uint8_t constBitsOf(const Ty* t, Decl* field, int depth = 0);

  // The element a pointer into an array points at (nullopt one past the end).
  static std::optional<ConstValue> constDeref(const ConstValue& p);

  // `p + n`, `p - n`, `p - q`, `p == q`, `p < q` with p and q pointers into one array (one past its
  // end included), `p == nullptr` (never: a pointer into an array is not null).
  std::optional<ConstValue> constPointerArith(P op, const ConstValue& l, const ConstValue& r);

  // `strlen(text)` on a pointer into a char array (a string literal's): the chars before the
  // first zero. The interpreter's one function of the C library so far: the library's
  // StringView(const char*) counts with it.
  std::optional<ConstValue> constStrlen(const ConstValue& p);

  // `point.x`, `this->count`, `items[i].size`: the data member of an object the evaluation holds
  // (a local, `this`, a constant's value, a temporary).
  std::optional<ConstValue> constMemberRead(MemberExpr* m, const TypeContext& ctx, int depth);
  // A reference member's part is the address of what it refers to (`View(const int& r) : ref(r)`:
  // r's place, a global's address), so that reading or writing it later reaches that object as it
  // is then, where a copy of its value was kept (a silent wrong answer). constReferenceTo makes it
  // from what the initializer names (nullopt for a prvalue, kept as a copy as before; a call that ran
  // gives its value), constRefersTo tells it from such a copy, and constThroughReference takes a
  // member's place to the referred object's (nullopt for a global's, read by constMemberRead).
  std::optional<std::optional<ConstValue>> constReferenceTo(Expr* x, const Ty* reference, const TypeContext& ctx, int depth);
  bool constRefersTo(const ConstValue& part, const Ty* declared);
  std::optional<ConstPlace> constThroughReference(const ConstPlace& member, int depth);
  // Whether x designates what a variable of static storage duration holds (a global, a static
  // member, a part of one), which no evaluation writes: a reference to it may keep its value, where
  // one to an object of the evaluation must keep its place. x without side effects (constPure).
  bool constDesignatesConstant(Expr* x, const TypeContext& ctx, int depth);

  // `items[i]` of an array the evaluation holds, `text[i]` through a pointer into one, `names[i]`
  // of a class by its operator[]: the element, its index checked.
  std::optional<ConstValue> constIndex(IndexExpr* x, const TypeContext& ctx, int depth);

  // A member operator of a class (`names[i]`: operator[]) run on an object the evaluation holds.
  std::optional<ConstValue> constOperatorCall(const ConstValue& object, uint32_t name, Span<Expr*> args, const TypeContext& ctx, int depth);

  // self: the object a member function runs on (`this`), the object it leaves after.
  // places: the arguments' places, for reference parameters (a reference parameter given none
  // gets a copy it may not write).
  std::optional<ConstValue> constRun(FunctionDecl* f, const std::vector<ConstValue>& args, TypeContext fctx, int depth, std::optional<ConstValue>* self = nullptr, const std::vector<std::optional<ConstPlace>>* places = nullptr, const std::vector<uint8_t>* categories = nullptr, const ConstPlace* selfPlace = nullptr, std::optional<ConstPlace>* returnPlace = nullptr);

  // A parameter's or a local's binding: its declared type (the value's own for auto), and whether
  // it refers to the caller's object (a reference other than to const), which step 2 cannot
  // write through.
  ConstBinding constBinding(uint32_t symbol, const ConstValue& v, const Ty* declared);

  // `total += i`, `a = b`: the value stored in a local of the function being run, converted to its
  // type, which is the expression's value. A global, an object's member or what a reference
  // parameter refers to is no local of the run: no constant. where: the left operand's place, which
  // the expression designates (const int& r = (a = 3);), when it was assigned.
  std::optional<ConstValue> constAssign(BinaryExpr* b, const TypeContext& ctx, int depth, std::optional<ConstPlace>* where = nullptr);

  // `++i`, `count--`: the local stepped by one in its type; the new value, or the old one for the
  // postfix forms. An object of a class: its operator++ or operator-- run on it where it is. where:
  // the operand's place, which a prefix one designates (const int& r = ++a;).
  std::optional<ConstValue> constIncrement(UnaryExpr* u, bool increment, const TypeContext& ctx, int depth, std::optional<ConstPlace>* where = nullptr);
  // A unary operator on an enumeration operand that calls an operator function (`!flags`, `++d`):
  // that call's result (none when it is no constant), or nullopt for the built-in operator.
  std::optional<std::optional<ConstValue>> constEnumUnary(UnaryExpr* u, P op, const TypeContext& ctx, int depth);

  static bool isAssignmentOperator(P op);

  enum class ConstFlow : uint8_t { Next, Returned, Break, Continue, Failed };

  // A case or default label of the switch body s that is not one of its statements, but inside
  // one (a label in a nested block, Duff's device): the runner finds labels among the body's own
  // statements only.
  static bool hasNestedLabel(Stmt* s, int depth = 0);

  // `for (auto v : range)` run at compile time: the body once per element, v declared as a local of
  // it would be (by value a copy, by reference the element). The range is an array (its elements
  // are places: `for (int& v : items)` writes them, and each is read as the body reaches it), a
  // brace list, or an object whose begin() and end() point into one array (InitializerList, Array,
  // StringView); such an object changed by the body makes no constant, since what its pointers
  // read is the array as it was.
  static constexpr uint32_t constRangeSymbol = 0xFFFFFFFDu;
  // A range-for over a range whose begin() and end() are iterator objects (a Deque's, a Map's): the
  // iterator and the end held as locals of the loop, `it != end`, `*it` and `++it` run by the
  // iterator's class on them where they are; the variable refers to what `*it` refers to (or is a
  // copy of it).
  ConstFlow constRangeForIterators(RangeForStmt* r, const TypeContext& ctx, std::vector<ConstBinding>& env, std::optional<ConstValue>& result, int depth, const ConstValue& begin, const ConstValue& end, const Ty* declared, uint32_t name);

  ConstFlow constRangeFor(RangeForStmt* r, const TypeContext& ctx, std::vector<ConstBinding>& env, std::optional<ConstValue>& result, int depth);

  // A local's declaration run: its binding pushed on env (a reference's to the place it refers to),
  // a structured binding's hidden object and names (constBindNames).
  ConstFlow constDeclare(VariableDecl* v, const TypeContext& ctx, std::vector<ConstBinding>& env, int depth);
  // The names of `auto [a, b] = e;` once its hidden object is bound at env[hidden]: each refers to
  // its part (an element, a data member of the class holding them), a tuple-like one is
  // `auto&& a = get<i>(e)` declared (Walker::tupleBindings). The binding at hidden becomes the
  // hidden object (constHiddenSymbol, hiddenOf v). False when that is no constant.
  bool constBindNames(VariableDecl* v, std::vector<ConstBinding>& env, size_t hidden, const TypeContext& ctx, int depth);

  // One statement of a body run at compile time (design.md, "Compile-time evaluation", steps 1 to
  // 4): blocks, returns, if and if constexpr, locals, assignments and ++/-- of them as expression
  // statements, for, while, do and range-for loops with break and continue, switch with
  // fallthrough and default, static_assert (checked where it is written) and assert. Anything else
  // (goto, try, a static local) makes the call no constant.
  ConstFlow constStatement(Stmt* s, const TypeContext& ctx, std::vector<ConstBinding>& env, std::optional<ConstValue>& result, int depth);

  ConstFlow constStatementIn(Stmt* s, const TypeContext& ctx, std::vector<ConstBinding>& env, std::optional<ConstValue>& result, int depth);

