// struct Emitter's members: constructors, destructors, member initialisation and copies: the
// declarations, part of the struct's body, included in it in Codegen/Emitter.hpp; the definitions
// in ClassMetadata.cpp (codegen.cpp split, stage 2, 2026-11-05, moved without a change).
    // --- class metadata: constructors, destructors ---

    // The constructor overload resolution picks for a class and argument types, chosen once.
    struct ConstructorKey
    {
      const Ty* classTy;
      std::vector<const Ty*> argTypes;
      std::vector<bool> nulls;
      bool operator==(const ConstructorKey&) const = default;
    };
    struct ConstructorKeyHash
    {
      size_t operator()(const ConstructorKey& k) const
      {
        size_t h = std::hash<const void*>()(k.classTy);
        for (const Ty* a : k.argTypes)
          h = (h * 1000003) ^ std::hash<const void*>()(a);
        return h ^ std::hash<std::vector<bool>>()(k.nulls);
      }
    };
    std::unordered_map<ConstructorKey, FunctionDecl*, ConstructorKeyHash> constructors;

    // The constructor for these arguments: their types, and which are the literal 0 (a null pointer
    // constant, which reaches `FromText(const char*)` as the checker ranks it).
    FunctionDecl* constructorFor(const Ty* classTy, Span<Expr*> args, Node* at);

    // nulls: per argument, whether it is the literal 0; empty when none is.
    FunctionDecl* findConstructor(const Ty* classTy, const std::vector<const Ty*>& argTypes, Node* at, const std::vector<bool>& nulls = {});

    std::vector<const Ty*> argTypesOf(Span<Expr*> args);

    bool isInitializerListType(const Ty* t);

    // `{a, b, c}` for an InitializerList<E> parameter: the elements in a temporary array, the list
    // object (a pointer and a count) pointing at them; both live to the end of the statement.
    // variable: the address of a variable the list initializes (`InitializerList<int> v = {1, 2}`,
    // `auto names = {"a", "b"}`), whose array lives as long as it does, as C++ extends it: in the
    // function's frame for a local, in static storage for a global or a static local.
    std::string initializerListTemporary(const Ty* listTy, BraceInitExpr* b, Node* at, const std::string& variable = "");

    // A call's arguments packed for its Text parameter (design.md 14, BraceInitExpr::text): the
    // pieces in a temporary array, each the address of its argument (a temporary of the statement
    // for one that is no lvalue, bound as a const reference binds it) and the library's function
    // that appends it (resolver: textPieceOf); the Text a pointer to them and their count.
    std::string textTemporary(BraceInitExpr* b, Node* at);

    // The parts of an object built inline (no constructor function of their own: an implicit
    // constructor's bases and members, a memberwise copy's, a braced aggregate's, a closure's
    // captures, an array's elements), each destroyed if a later part's construction throws
    // ([except.ctor] 3): unwind-only cleanups that go with the statement's temporaries (so that
    // unwinding destroys them before a new-expression frees the memory), dropped when this ends,
    // the object whole (its owner destroys it then). Until 2026-11-05 only a written constructor's
    // body had them: `class Implicit { M a{3}; M b{fail(1)}; }` left a undestroyed.
    struct InlineParts
    {
      InlineParts(Emitter& emitter, Node* where) : e(emitter), at(where) {}
      InlineParts(const InlineParts&) = delete;
      void built(const std::string& addr, const Ty* t);
      // An array's elements built in a loop: the first *countSlot of them (an i64 the loop keeps).
      void builtCounted(const std::string& addr, const Ty* arrayTy, const std::string& countSlot);
      ~InlineParts();

      Emitter& e;
      Node* at;
      std::vector<std::string> parts;
    };

    // Default-constructs every element of an array of objects at addr: one after the other for a
    // few, in a loop for more (an array of a thousand would otherwise be a thousand copies).
    void constructArray(const std::string& addr, const Ty* t, Node* at);

    // A destructor written by the user; `~T() = default` is the compiler's memberwise one.
    FunctionDecl* findDestructor(ClassDecl* c);

    // Constructors written by the user (`= default` ones are the compiler's memberwise ones).
    bool hasUserConstructors(ClassDecl* c);

    // Default construction does something: user constructors, a vtable pointer, default member
    // initializers, or a base/member class that needs constructing (a zeroed global is not enough).
    bool needsConstruction(const Ty* t, Node* at, int depth = 0);

    bool needsDestruction(const Ty* t, Node* at);
    std::unordered_map<const Ty*, bool> destructionCache; // this emitter's final answers (its copy of sh.finalDestruction's)
    std::unordered_set<const Ty*> destructionPending;     // the classes whose answers this emitter is computing
    bool destructionGuessed = false;                      // one of them was reached again inside its own answer

    // Destroys the object at addr: user destructor if any (it destroys members itself), else
    // members and bases that need it, in reverse order.
    void destroy(const std::string& addr, const Ty* t, Node* at, bool dynamic = false);

    // Whether destroying an object of type t may throw: its destructor is declared `noexcept(false)`
    // (or noexcept with a condition that is false), or has no specifier and a member's or base's
    // destruction may throw (C++ [except.spec]/8: such a destructor is noexcept unless one of
    // theirs is potentially-throwing). The game has eleven that throw by design (ChartedChunksEventHelper,
    // RollingStock::CarriageGuard, ForceManager, MapSerialiser, ...); every other destructor call
    // stays a plain call.
    std::unordered_map<const Ty*, bool> throwingDestructionCache;
    bool destructionMayThrow(const Ty* t, Node* at, int depth = 0);

    // A destructor's call (dtor: the function, object: its address). One that may throw is invoked:
    // its exception unwinds through what was built before the object (the caller keeps that out of
    // the lists: popScope). While an exception is already unwinding (in a cleanup pad) its exception
    // ends the program, C++'s std::terminate ([except.terminate]): the call goes through
    // @kpp_destroy_unwinding, which invokes it with abort as the unwind (a cleanup funclet of SEH's
    // personality can hold no invoke itself).
    void callDestructor(const std::string& dtor, const std::string& object, bool mayThrow);

    // How many calls destroying an object of class type t spells out inline (destroy without a
    // helper), at most 1000.
    std::unordered_map<const Ty*, size_t> destructionCallCounts;
    size_t destructionCalls(const Ty* t, Node* at, int depth = 0);

    void destroyMembers(const std::string& addr, const Ty* t, Node* at);

    // Default construction: zero, then default member initializers, then class members' and
    // bases' constructors.
    void defaultConstruct(const std::string& addr, const Ty* t, Node* at);

    // The class a constructor initializer of class t names (`super(v)` with `using super = ...`,
    // `Chain<Tail...>(tail...)`): its name converted as a type in the class under the class's
    // arguments (args: under those instead); null for a member's name or one that names no class there.
    const Ty* initializerClass(const Ty* t, const FunctionDecl::CtorInit& init, Node* at, const TemplateArgs* args = nullptr);

    // Whether a constructor initializer written with template arguments names another class than
    // cls: `Chain<Tail...>(tail...)` in Chain<Head, Tail...> (its base, an instance of the same
    // template), `Holder<1, T>(b)` beside a base Holder<0, H>.
    bool initializerNamesOther(const Ty* t, const FunctionDecl::CtorInit& init, const Ty* cls, Node* at);

    // The constructor initializer that names base `baseTy` of class t by its type (through an
    // alias, `using super = Conditional<...>; Const(const T& v) : super(v) {}`, or one of several
    // instances of a template); null if none does.
    const FunctionDecl::CtorInit* initializerNamingBase(const Ty* t, const Ty* baseTy, Span<FunctionDecl::CtorInit> inits, Node* at);

    // The pack expansion of constructor initializers (`Leaf<A>(a)...`, `Ts(ts)...`, [temp.variadic] 5)
    // whose element names base `baseTy` of class t, and that element (element): the initializer's
    // name converted with the class's packs bound to each element in turn. Null if none does.
    const FunctionDecl::CtorInit* packInitializerOf(const Ty* t, const Ty* baseTy, Span<FunctionDecl::CtorInit> inits, size_t& element, Node* at);

    // Runs emit with the default member initializer of v, a member of the class t whose object is
    // at addr: the initializer as typed for t's instantiation, with `this` the object, t the class
    // of the moment and its arguments the current ones. A base's `Hook* next = this;` refers to the
    // base subobject, also when a derived class's implicit constructor is emitted into a caller.
    template<typename F>
    void withMemberInitializer(const std::string& addr, const Ty* t, VariableDecl* v, F&& emit)
    {
      const Ty* cls = canonicalClass(t, v);
      TemplateArgs classArgs = global.classArgsOf(cls);
      Expr* init = global.instanceInitializer(v, classArgs);
      std::string slot;
      if (!inlining.empty())
        slot = inlineSlot(std::to_string(inlining.size()) + ".member", "ptr"); // in a constructor inlined here: one slot for its members' initializers
      else
      {
        slot = "%v" + std::to_string(temp++);
        allocas += "  " + slot + " = alloca ptr\n";
      }
      fn += "  store ptr " + addr + ", ptr " + slot + "\n";
      struct Restore
      {
        ~Restore()
        {
          if (self)
            e.locals[nullptr] = *self;
          else
            e.locals.erase(nullptr);
          e.currentClassTy = classTy;
          e.currentArgs = args;
          e.currentFile = file;
          e.initializerOf = initializer;
        }

        Emitter& e;
        std::optional<Local> self;
        const Ty* classTy;
        const TemplateArgs* args;
        ParsedFile* file;
        Decl* initializer;
      } restore{*this, locals.count(nullptr) ? std::optional<Local>(locals[nullptr]) : std::nullopt, currentClassTy, currentArgs, currentFile, initializerOf};
      locals[nullptr] = {slot, nullptr, false};
      currentClassTy = cls;
      currentArgs = &classArgs;
      currentFile = v->file;
      initializerOf = v;
      emit(init);
    }

    // A reference member (of type refTy) bound to what source names: the field holds its address, of
    // its base subobject for a reference to a base (EntityAndIndex's `UpdatableEntity& entity` from an
    // InfinityContainer, whose UpdatableEntity comes after its LogisticContainer).
    void bindReferenceMember(const std::string& fieldAddr, const Ty* refTy, Expr* source, Node* at);

    // Points the vtable pointer of the object at addr at its class's vtable: once its bases are
    // built (their constructors pointed it at theirs), and after its bytes were copied from an
    // object that may be of a class derived from it.
    void storeVptrs(const std::string& addr, const StructInfo& info);

    // The vtable pointers of the object at addr, loaded before an assignment copies its bytes and
    // stored back after it: an object keeps its own class's vtables (it may be of a class derived
    // from the one assigned).
    std::vector<std::string> loadVptrs(const std::string& addr, const StructInfo& info);
    void restoreVptrs(const std::vector<std::string>& saved);

    // Where the vtable pointers of the object at addr are, in the order of its class's vtables.
    std::vector<std::string> vptrAddresses(const std::string& addr, const StructInfo& info);

    // Initialises bases and members of the object at addr from constructor initializers (or
    // default member initializers / default construction when absent).
    // built: a constructor's own initialization: each base and member that needs destruction is
    // destroyed if an exception leaves the constructor after it was built (a later member's
    // initializer or the body throwing), as C++ does.
    void initializeMembers(const std::string& addr, const Ty* t, Span<FunctionDecl::CtorInit> inits, Node* at, bool built = false);

    // Whether a constructor initializer names a data member of the anonymous struct or union c (or
    // of one nested in it).
    static bool namesMemberOf(ClassDecl* c, Span<FunctionDecl::CtorInit> inits);

    // An anonymous struct or union member of a class being constructed, at addr, of type t (c its
    // declaration). A struct: its members as the class's own (initializeMembers). A union: zeroed
    // (members are zero-initialised in kpp), then the member a constructor initializer names, or
    // else the one with a default member initializer, is initialised: C++ initialises one member.
    void initializeAnonymous(const std::string& addr, const Ty* t, ClassDecl* c, Span<FunctionDecl::CtorInit> inits, Node* at);

    // The type of member m of union t (every member at the union's start, field 0 standing for none
    // of them): a data member's, or an anonymous struct's with the union's arguments.
    const Ty* unionMemberType(const Ty* t, Decl* m, Node* at);

    // Whether every element of the brace list converts to the element type of the InitializerList
    // constructor ctor of class t: only then is ctor the one, otherwise the other constructors take
    // the elements (C++'s two phases: `Vector<int*> v{set.begin(), set.end()}` is the range). An
    // element of unknown type, or a list itself, counts as converting.
    bool listConvertsTo(FunctionDecl* ctor, BraceInitExpr* list, const Ty* t, Node* at);

    // The constructor of class t that takes an InitializerList (the rest defaulted), or null.
    FunctionDecl* initializerListConstructor(ClassDecl* c, const Ty* t, Node* at);

    // Constructs an object of class type t at addr from constructor arguments.
    // (class, argument) of the one-argument constructions under way: a constructor whose parameter
    // needs the same conversion again would otherwise recurse until the stack runs out.
    std::vector<std::pair<const Ty*, Expr*>> constructing;
    struct ConstructingScope
    {
      ConstructingScope(std::vector<std::pair<const Ty*, Expr*>>& list, const Ty* t, Expr* arg) : list(list) { list.push_back({t, arg}); }
      ~ConstructingScope() { list.pop_back(); }

      std::vector<std::pair<const Ty*, Expr*>>& list;
    };

    // `T(x)` of class type t into addr: the constructors take the arguments; and, as
    // direct-initialization lets the copy or move constructor take what an explicit conversion
    // operator gives (C++'s rule for a constructor's first parameter), `Constant(parsed)` with
    // Parsed's `explicit operator const Constant&()` and `AchievementId(id)` with `explicit operator
    // AchievementId()` when no constructor takes the object itself.
    void directInitialize(const std::string& addr, const Ty* t, Span<Expr*> args, bool braced, Node* at, bool listElements);

    // listElements: args are a brace list's elements (the list taken apart once already).
    void constructInto(const std::string& addr, const Ty* t, Span<Expr*> args, Node* at, bool listElements = false);

    // `auto [a, b] = pair;`: e names a binding of the hidden local v; out gets the address of the
    // matching data member (or array element). False when e is not one of v's bindings (the
    // nameless name of v itself, which a tuple-like binding's get<i>(v) passes, included).
    bool bindingAddress(VariableDecl* v, Node* e, std::string& out);
    // The bit-field a structured binding e names (`a` of `auto [a, b] = s;` over a bit-field), or
    // null: the member bindingAddress finds, without emitting anything. A reference given one binds
    // a temporary, as one given the field does (bindsBitField).
    const BitfieldInfo* bindingBitfield(Expr* e);
    // Whether e is a bit-field lvalue: namesBitField's, or a structured binding of a bit-field.
    bool bindsBitField(Expr* e) { return namesBitField(e) || bindingBitfield(e); }
    // The data member of owner's class that binding number index names: C++'s numbering
    // ([dcl.struct.bind] 5), the named non-static data members in declaration order (an unnamed bit
    // field is no member), as this layout has them; null when there are fewer. Bit fields sharing a
    // storage unit are one field of the layout (StructInfo::fieldDecls), each its own binding here.
    Decl* bindingMember(const Ty* owner, size_t index, Node* at);

    // `return result;` naming a local object or a by-value parameter of the returned class: it is
    // moved into the result, not copied (C++'s implicit move on return).
    // Whether a loop variable of type vt, given an element of elementTy, is of another class than
    // the element (`for (const Label& l : wrappers)`), which the element converts to.
    bool elementOfOtherClass(const Ty* elementTy, const Ty* vt, Node* at);

    // What a loop variable of type vt refers to or copies, from the element at addr: the element,
    // or the element converted to vt's class (to its base, or through its conversion operator:
    // a Vector of reference_wrapper read as references), never the element read as that class.
    std::string elementAs(const std::string& addr, const Ty* elementTy, const Ty* vt, Node* at);

    // Whether a loop variable of type vt (or referring to one) is of a class and the element is no
    // object (`for (String name : {"stone-wall", "transport-belt"})`): the variable is made from it.
    bool elementMadeInto(const Ty* elementTy, const Ty* vt);

    // Builds the loop variable's object of class cls at slot from the element at elementAddr as
    // `String name = element;` would (a converting constructor), never from the element's bytes:
    // TestDragBuilding's `for (std::string blockingEntity : {"stone-wall", ...})` read each
    // `const char*` as a String, and every name came out empty.
    void makeFromElement(const std::string& slot, const Ty* cls, const std::string& elementAddr, const Ty* elementTy, Node* at);

    // A vector the checker could not type (an SSE operation clang's intrin.h has as a macro,
    // _mm_shuffle_ps: the walker leaves its name to codegen, which builds it by name) made into an
    // object of class cls at addr, by cls's constructor from that vector (float32x4's from __m128)
    // as a typed vector is: the value stands in as a local of its own type. copyInit: `T x = v`, a
    // parameter, a return value, an assignment's operand, where an explicit constructor is no way.
    // Until 2026-11-05 the vector was stored into the class's first field, which skipped the
    // constructor: a silent wrong value where it does more than store.
    void constructFromVector(const std::string& addr, const Ty* cls, Value v, bool copyInit, Node* at);

    // The local e names that `return e;` takes as an rvalue first, C++'s implicit move
    // ([class.copy.elision] 3): a (possibly parenthesized) name of an automatic object of class type
    // or of an rvalue reference to one, declared in the function (not a lambda's capture), not const
    // (its `const T&&` binds the copy constructor); null otherwise. Its class in cls.
    Decl* implicitlyMovableLocal(Expr* e, const Ty*& cls);

    // The class of that local when it is not the function's result class (whose own `return obj;`
    // is a move: returnedLocalObject); null otherwise.
    const Ty* localObjectOfOtherClass(Expr* e);

    Decl* returnedLocalObject(Expr* e);

    // Whether init, an lvalue of the type t refers to (or of a class derived from it), is given to the
    // rvalue reference t ([dcl.init.ref] 5.4.4: an error, never the object itself bound).
    bool rvalueReferenceGivenLvalue(const Ty* t, Expr* init, Node* at);

    // `return value;` in a function returning ret, an rvalue reference: an error where value is an
    // lvalue of its type, in the function's own body and in a call inlined from it (inlinedBody).
    void checkRvalueReferenceResult(const Ty* ret, Expr* value);

    // Named return elision, guaranteed (design.md 23, kovarex 2026-11-04): the variable every return of
    // the function returns, by the resolver's rule (GlobalScope::namedReturnVariable, given the body,
    // the result type and the template arguments codegen emits it under: an instance's typed copy, a
    // lambda's operator), is built in the caller's result (sretAddr) instead of a slot of its own, so
    // that no return copies or moves it and its class needs neither constructor. Not when the result
    // comes back in registers (no sretAddr: a class copied as bits, with no constructor to skip). A
    // variable of another type than the result's would be the rule's error: never built there, a
    // compile error.
    VariableDecl* builtInResult(VariableDecl* v, Node* at);

    // Whether e (a return's value) names the function's namedReturn.
    bool returnsNamedReturn(Expr* e);

    // A bit field's value: the storage unit shifted and masked (sign-extended for signed types).
    Value loadBitfield(const std::string& addr, const BitfieldInfo& bits, const Ty* memberTy, Node* at);

    // Writes a bit field v, already of the member's type: the other bits of the storage unit stay
    // as they are.
    void storeBitfield(Value v, const std::string& addr, const BitfieldInfo& bits, Node* at);

    // The bit fields that share first's storage unit, in declaration order (first among them).
    std::vector<Decl*> unitMembers(const StructInfo& info, ClassDecl* c, Decl* first);

    // The members an aggregate's values go to after its bases, in order: the layout's fields, each
    // named bit field of a storage unit on its own (the unit's first bit field stands for the unit
    // among the fields, StructInfo::fieldDecls); an unnamed bit field is no member and takes no value
    // ([class.bit] 2: `{1, 2.5f}` for `int a : 3; int : 2; float f;` is a and f, where 2.5f went
    // into the unnamed bits until 2026-11-06).
    std::vector<Decl*> aggregateMembers(const StructInfo& info, ClassDecl* c);

    // Member d of the aggregate of class t at addr initialized: from value (a brace list's value, a
    // pack's element), a reference member bound to it; or, with none, value-initialized as from `{}`
    // (its default member initializer, its default constructor; a bit field's unit zeroed once:
    // zeroedUnits). Each part made goes to parts. The brace list's and the pack's paths of
    // constructInto.
    void initializeAggregateMember(const std::string& addr, const Ty* t, const StructInfo& info, Decl* d, Expr* value,
                                   std::set<size_t>& zeroedUnits, InlineParts& parts, Node* at);

    // The type a value for member d of the aggregate t goes to: a bit field's declared type (a
    // scalar: one value), a reference member's reference type, a union member's, a field's.
    const Ty* aggregateMemberType(const Ty* t, const StructInfo& info, Decl* d, Node* at);

    // Whether value initializes an element of type et as it is ([dcl.init.aggr] 16: "if the
    // assignment-expression can initialize an element, the element is initialized"): a list, a value
    // for a scalar, a reference or a class with constructors, a string literal or an array for an
    // array, a value of the element's own class, of a class derived from it or converting to it.
    // Otherwise the element is a subaggregate whose braces are elided: its own elements take the
    // values in turn (a number for an aggregate member: `Out o{1, 2}` over `In in; int c;`).
    bool initializesWhole(const Ty* et, Expr* value, Node* at);

    // How many of values, from start, the aggregate or array t takes with its braces elided: one
    // for each of its elements (an array's; a class's bases, then its aggregateMembers; a union's
    // first member alone), or as many as an element takes itself when the value there does not
    // initialize it whole; fewer when the values run out.
    size_t elidedValues(const Ty* t, Span<Expr*> values, size_t start, Node* at, int depth = 0);

    // An element of type et at addr (a member, a base, an array's element) initialized from the
    // values its braces were elided from: an array's elements (initializeArrayElements), an
    // aggregate class's parts (constructInto, as from a list of the values).
    void initializeFromElided(const std::string& addr, const Ty* et, Span<Expr*> values, Node* at);

    // The elements of the array of type t at addr (already zeroed) from values, each from the next
    // value, or from as many as it takes when that does not initialize it whole (its braces elided:
    // `In arr[2] = {1, 2, 3, 4}` is {{1, 2}, {3, 4}}, D, 2026-11-06: it was {1, 0}, {2, 0} with 3
    // and 4 dropped); the elements left out value-initialized, as from `{}`; each element built
    // destroyed if a later one's initializer throws ([except.ctor] 3). More values than the array
    // takes is an error, as in C++ (they were dropped).
    void initializeArrayElements(const std::string& addr, const Ty* t, Span<Expr*> values, Node* at);

    // A bit field initialised: from value (an aggregate's element, a constructor initializer's
    // argument; `: flag()` is explicit with none, a zero), else from its default member
    // initializer (`bool known : 1 = true;`). It shares its storage unit with its neighbours: the
    // unit is zeroed once, at its first member (zeroed), and each value stored into its own bits.
    void initializeBitfield(const std::string& addr, const Ty* t, const StructInfo& info, VariableDecl* v, const BitfieldInfo& bits, bool explicitInit, Expr* value, std::set<size_t>& zeroed, Node* at);

    // The address of the object a variable or parameter declaration names (what lvalue gives a
    // name expression): a local, a field of this, or a global.
    std::string addressOfDecl(Decl* d, Node* e);

    // The object a data member of the object at `object` (of class classTy) is: its field, or for a
    // reference member the object the field refers to (its address is what the field holds), also
    // when a template argument makes the member a reference (Holder<int&>'s `A value`). bits, when
    // given, gets the member's bit-field place in the layout that holds it (fieldAddress).
    std::string memberObject(const Ty* classTy, Decl* d, const std::string& object, Node* e, const BitfieldInfo** bits = nullptr);

    // Whether an expression is an xvalue: an rvalue reference to an object that may be moved from
    // (`move(x)`); a named rvalue reference is an lvalue.
    static bool isXvalue(Expr* e) { return e && e->ty && e->ty->kind == TyKind::RRef && !isLvalueExpression(e); }
    // An xvalue a move may take: not a const one (`move(constObject)`), which a `T&&` does not bind,
    // so the copy constructor or copy assignment takes it and the source keeps its value.
    static bool movable(Expr* e) { return isXvalue(e) && e->ty->inner && !e->ty->inner->isConst; }

    // Move-constructs the object at src into dst: through the move constructor where the class has
    // one, as a copy otherwise.
    // Whether class c (of type t) has an implicit copy or move constructor taking a parameter of
    // type pt (T&& or const T&) as well as a constructor template given the same argument does:
    // the tie goes to the implicit one, no template ([over.match.best]). The implicit move
    // constructor is declared when c declares no copy constructor, copy or move assignment and no
    // destructor; the implicit copy constructor when c declares none ([class.copy.ctor] 6 and 8).
    bool implicitCopyOrMoveTakes(ClassDecl* c, const Ty* t, const Ty* pt, Node* at);

    void moveConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at);

    // Copy-constructs the object at src into dst (user copy constructor or bitwise copy); with
    // `move`, a class without a user copy constructor moves its members instead.
    void copyConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move = false);

    // Whether class c (of type t) declares a move assignment (assignment) or a move constructor
    // (`= default` and `= delete` ones included): what deletes C++'s implicit copy members.
    bool declaresMoveMember(ClassDecl* c, const Ty* t, bool assignment, Node* at);

    // Why the implicit copy constructor of the class t is deleted ([class.copy.ctor] 10): a base or
    // member whose class declares its copy constructor deleted, or has an implicit one deleted so;
    // empty when it is not.
    std::string implicitCopyDeletedBy(const Ty* t, Node* at, int depth = 0);

    // Constructs the object at dst from the one at src base by base and member by member
    // (bitwise when nothing needs destruction): copies, or moves with `move`.
    void memberwiseConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move);

    // Copies or moves the array of type t at src into the one at dst, constructing (assign: assigning)
    // element by element, each through its own copy or move: a member `T items[N]` of a class whose
    // copy or move is memberwise (the library's Array<Vector<T>, N> returned by value; bitwise, the
    // vectors were freed twice). One after the other for a few, in a loop for more; an array of what
    // needs no destruction and copies (assigns) bitwise is copied bitwise (a `Counted cs[2]` whose
    // copy constructor counts is copied by it).
    void transferArray(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move, bool assign);

    // Implicit copy assignment: user operator= where one exists, memberwise otherwise.
    void assignObject(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move = false);

    // Whether class t declares an operator= of its own (`using Base::operator=;` included): one that
    // does not has implicit ones (memberwise), which hide its bases'.
    // Whether class t declares an operator= of its own, or brings a base's in by a using-declaration;
    // usingOnly: only the latter.
    bool declaresAssignment(const Ty* t, Node* at, bool usingOnly = false);

    // Whether assigning an object of class t from an rvalue (move) or a const lvalue resolves to a deleted
    // operator=: its own deleted one, the implicit copy assignment a declared move member deletes, or an
    // implicit (or defaulted) one whose base or member cannot be assigned so ([class.copy.assign] 7). An
    // implicit or defaulted move assignment C++ defines as deleted is no candidate ([over.match.funcs] 8):
    // the copy assignment takes the rvalue. (Reference and const members, which delete both, are the
    // checker's errors: Walker::deletedAssignment decides the same rules for them.)
    bool assignmentDeleted(const Ty* t, bool move, Node* at, int depth = 0);

    // Whether a base or member (an array's element) of class t cannot be assigned from an rvalue (move) or
    // a const lvalue (assignmentDeleted): what defines t's implicit move (copy) assignment as deleted.
    bool partAssignmentDeleted(const Ty* t, bool move, Node* at, int depth = 0);

    // Assigns the object at src to the one at dst base by base and member by member.
    void memberwiseAssign(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move);

    // Initialises the object/scalar at addr of type t from an initializer expression,
    // constructing in place where the expression is a temporary.
    // Whether both branches of conditional c are of class t itself, one at least a prvalue
    // (`c ? T(1) : make(2)`, `c ? make() : lvalue`): the result is a prvalue, each branch building
    // the object initialised from c in place. Not two lvalues (one is copied) or another class
    // (converted).
    bool conditionalOfPrvalues(ConditionalExpr* c, const Ty* t, Node* at);
    // e as a built-in comma, in parentheses or not (`(x, T(4))`), as binary() evaluates it: its left
    // operand, then its right one, which is the result. Null for anything else, among them a comma
    // that calls the comma operator of a class the checker chose.
    BinaryExpr* builtinComma(Expr* e) const;

    void initializeFrom(const std::string& addr, const Ty* t, Expr* init, Node* at);

    // t with the template parameters of the instantiation being emitted replaced by their
    // arguments, for an expression the type checker typed with a parameter left in.
    const Ty* withCurrentArgs(const Ty* t, int depth = 0);
    // t when it is an enumeration of a class template's instance (`S<long long>::Dir`), as the code
    // emitted now binds its arguments, unqualified; otherwise null. Its methods are that instance's.
    const Ty* enumInstance(const Ty* t);

    // Whether a temporary of class t may move to the object it initialises bitwise, as if it had been
    // built there (C++17 builds a prvalue in its object; kpp may build it in a slot of its own first):
    // no user-provided copy or move constructor in the class, its bases or its members. A class with
    // one may keep its own address somewhere: IDConnector hands `this` to its resolver, and its move
    // constructor gives the resolver the new address (a bitwise move left the resolver writing to the
    // dead temporary: assignPrototypeIDs then deleted garbage).
    std::unordered_map<const Ty*, bool> bitwiseRelocation; // this emitter's answers
    bool relocatesBitwise(const Ty* t, Node* at, int depth = 0);

    // Whether objects of type t are assigned bitwise: no copy or move assignment of its own (written,
    // not defaulted) in the class, its bases or its members (GenericTargetable's `operator=` keeps
    // the object's own targeters).
    std::unordered_map<const Ty*, bool> bitwiseAssignment;
    bool assignsBitwise(const Ty* t, Node* at, int depth = 0);

    // Whether class t has a move or copy constructor that is not deleted (an implicit one counts).
    bool movableByConstructor(const Ty* t, Node* at);

    // A temporary that is used to initialise an object is moved there and no longer destroyed:
    // bitwise when its class allows it (relocatesBitwise), else by its move (or copy) constructor,
    // and the temporary destroyed then (a class that can be neither moved nor copied: bitwise, as
    // C++17 builds such a prvalue in its object).
    void moveTemporary(const std::string& dst, Value src, const Ty* t, Node* at);

    std::string newObjectSlot(const Ty* t, Node* at);

