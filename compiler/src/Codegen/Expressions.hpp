// struct Emitter's members: expressions: rvalues, unary and binary operators, three-way comparison:
// the declarations, part of the struct's body, included in it in Codegen/Emitter.hpp; the
// definitions in Expressions.cpp (codegen.cpp split, stage 2, 2026-11-05, moved without a change).
    // The pointer an `->` applies to: the value itself, or what the class's operator-> returns
    // (repeatedly, for wrappers of wrappers).
    Value arrowOperand(Value p, Node* at);

    // The instantiated base type of `derived` whose definition is `baseDecl`, or null.
    const Ty* baseInstantiation(const Ty* derived, Decl* baseDecl, Node* at);

    // The arguments of function template f whose address n (in e) takes, added to fargs (its
    // class's): those written, then those the type the checker gave the address deduces, a
    // trailing pack the rest. All of them, or the address is unsupported.
    void addressedTemplateArgs(FunctionDecl* f, NameExpr* n, Expr* e, TemplateArgs& fargs);

    // Value of a call result for use as an rvalue (reference results are loaded).
    Value callRvalue(Value v, Node* at);
    // Where the operator function called for expression e builds its result: the object it
    // initializes (inPlaceOperator), else nowhere yet ("": a temporary).
    std::string operatorResult(const Node* e) const { return e == inPlaceOperator ? inPlaceSlot : ""; }

    Value rvalue(Expr* e);

    // The bit-field an lvalue names, or null: `x.f`, `this->data.f`, `f` in a member function, in
    // parentheses or not (what an assignment, a compound assignment or an increment writes, its own
    // bits of the storage unit alone). Asked right after lvalue(e), which records the place of the
    // member it found in the layout holding it (lvalueBitfield): that instance's own. A member alone
    // does not tell it: an instance's members are its template's declarations, and Bits<3> and
    // Bits<5> place `uint32_t a : N` differently (kpp read one instance's bits with the other's).
    std::pair<const Expr*, const BitfieldInfo*> lvalueBitfield{nullptr, nullptr}; // the member lvalue computed last, its bit field
    const BitfieldInfo* bitfieldNamed(Expr* e);
    // An arithmetic operand's value v (just computed from e) as C++'s [conv.prom] 5 promotes it
    // (kovarex 2026-11-05: as the standard, where MSVC keeps the declared type): a bit-field of an
    // integer type of 32 bits or more converted to int when int holds every value of its width, to
    // unsigned int when only that does (32 bits of an unsigned type), by its layout's width
    // (lvalueBitfield); any other v as it is. `u3 - 2` with an `unsigned u3 : 3` is the int -1. The
    // checker types the operators by the same rule (GlobalScope's bitFieldPromotion).
    Value bitfieldOperand(Expr* e, Value v);
    // A conditional whose type the checker could not tell (a branch it could not type, as a
    // recursive generic lambda's `self(self, n - 1)` in its generic body): its type is the one C++
    // gives the branches as this instance emits them, the same type, their usual arithmetic
    // conversions or the pointer one converts to; a class branch is an error, never a guess.
    Value conditionalOfBranches(ConditionalExpr* c);
    Expr* enterBranchBesideThrow(ConditionalExpr* c); // (emits the condition and the throw; null, nothing emitted, when no branch alone throws)

    bool mallocKnown = false; // this emitter saw malloc declared (it stays declared)
    int programAllocates = -1; // GlobalScope::programAllocationFunctions, asked once (-1: not yet)

    // The object a class-type template parameter names in this instantiation (`template<Fixed F>`
    // given Fixed{3, 14}, C++20's template parameter object): the address of a constant of that
    // value, one per value (linkonce_odr); empty when d is bound to no class object here.
    std::string templateParameterObject(Decl* d, Node* at);
    const Ty* templateParameterClass(Decl* d); // its class (null: no class object)
    bool programAllocationFunctions();

    // The call (for emitCall) that gives back the block a new-expression allocated without a class's
    // own operator new: free, or, when the program declares its own allocation functions, the global
    // operator delete (array: delete[]), the one that takes the size when size is given and there is
    // one ([expr.delete] 10; as clang's sized deallocation).
    std::string globalFreeCall(const std::string& block, const std::string& size, bool array, Node* at);
    void ensureMalloc();
    // A new-expression that cannot have its block, as C++'s throws std::bad_alloc (or its
    // bad_array_new_length, [expr.new] 8): BadAlloc thrown (the library's throwBadAlloc), and
    // without the library the program stopped saying `stop`. Ends the current block.
    void allocationFailure(Node* at, const std::string& stop);
    // Calls the library's function of that name, which throws (with no library the program stops
    // there, saying `stop`); nothing runs after it.
    void throwFromLibrary(const char* name, Node* at, const std::string& stop);
    // A block malloc gave a new-expression: allocationFailure when it is null, never an object built there.
    void checkAllocation(const std::string& block, Node* at);
    // `(a <=> b) cmp 0` of an order (an i8: -1, 0, 1, or 2 for unordered, a NaN's, which only != holds),
    // what a rewritten `a >= b` tests: until 2026-11-06 unordered counted as greater (E's battery).
    Value orderHolds(P cmp, Value order, Node* at);
    // An expression that needs an operator class cls has none of (`name`, "operator+="): an error.
    // The library's String and WString have no `+=` on purpose (`<<` appends: docs/stdlib/text.md,
    // design.md 14), which it says, with what to write.
    [[noreturn]] void missingOperator(Node* at, const Ty* cls, const std::string& name);

    // The bytes before the elements of `new T[n]`: a cookie whose last 8 hold n, when T needs
    // destroying (as MSVC's runtime lays it out), so that delete[] destroys all n; 16 for an element
    // aligned to 16, malloc's alignment. A type that needs no destroying has none (delete[] frees).
    // sizedDelete: the class's usual operator delete[] takes the size (the type checker's
    // sizedDeallocation), which needs the count however t is destroyed (at least 8 then).
    uint64_t arrayCookie(const Ty* t, Node* at, bool sizedDelete = false);

    // A loop over the elements of type t at base with an index in an i64 slot: from first up to
    // count, or from count down to 0 (backwards: the last element first).
    template<typename F>
    void forEachElement(const std::string& base, const Ty* t, const std::string& first, const std::string& count, bool backwards, Node* at, F&& body)
    {
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i64\n";
      fn += "  store i64 " + (backwards ? count : first) + ", ptr " + slot + "\n";
      std::string loopL = newLabel("array.loop"), bodyL = newLabel("array.body"), endL = newLabel("array.end");
      emitBranch(loopL);
      startBlock(loopL);
      std::string index = newTemp(), done = newTemp();
      fn += "  " + index + " = load i64, ptr " + slot + "\n";
      fn += "  " + done + " = icmp " + (backwards ? "eq i64 " + index + ", 0" : "uge i64 " + index + ", " + count) + "\n";
      fn += "  br i1 " + done + ", label %" + endL + ", label %" + bodyL + "\n";
      startBlock(bodyL);
      std::string i = index;
      if (backwards)
      {
        i = newTemp();
        fn += "  " + i + " = sub i64 " + index + ", 1\n";
        fn += "  store i64 " + i + ", ptr " + slot + "\n";
      }
      std::string element = newTemp();
      fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + base + ", i64 " + i + "\n";
      body(element);
      if (!backwards)
      {
        std::string next = newTemp();
        fn += "  " + next + " = add i64 " + index + ", 1\n";
        fn += "  store i64 " + next + ", ptr " + slot + "\n";
      }
      emitBranch(loopL);
      startBlock(endL);
    }

    // `new T[n]`: n elements of T, each built by its default constructor in order (a scalar left as
    // it is, zero with `new T[n]()`); `new T[n]{a, b}` builds the first ones from the list and
    // value-initializes the rest. One that throws destroys those built, the last first, and frees
    // the block. A count below the list's length, or negative, is an error when known here and
    // stops the program at run time. Returns the address of the first element.
    std::string arrayNew(NewExpr* n, const Ty* t, Expr* sizeExpr);

    // `delete[] p`: with a cookie (arrayCookie), the n elements destroyed, the last first, then the
    // block freed; nothing for null. Without one, the block freed.
    // dealloc: the class's own operator delete[] the type checker chose (DeleteExpr::deallocator), in
    // place of free, with the block's size after it when sized.
    void arrayDelete(const std::string& p, const Ty* t, Node* at, FunctionDecl* dealloc = nullptr, bool sized = false);

    // The call (for emitCall: "void @f(ptr %b, i64 8)") of a class's own operator delete f, which
    // the type checker chose for an object of class t (DeleteExpr::deallocator, NewExpr::deallocator),
    // on the block at block: with its size after it (an i64's text) when size is not empty, then
    // extra (a placement form's arguments, as its operator new took them). t: the class.
    std::string deallocationCall(FunctionDecl* f, const std::string& size, const Ty* t, const std::string& block, const std::vector<std::string>& extra, Node* at);

    // The usual operator delete of class cls, as a delete-expression's lookup finds it ([expr.delete]
    // 10, [class.free]): the class's own, else its bases' (one of them; two is an error), its
    // `operator delete(void*)` before `operator delete(void*, size_t)` (sized). owner: the class that
    // declares it. Null when none declares one.
    FunctionDecl* classDeallocation(const Ty* cls, const Ty*& owner, bool& sized, Node* at, int depth = 0);

    // Slot -3 of class key's vtables (emitVtables): a function `void(ptr block)` calling key's
    // operator delete (classDeallocation) on the whole object's block, its definition added to
    // definitions, named after irName; "null" when key has none (delete frees).
    std::string deleterFor(const Ty* key, const std::string& irName, std::string& definitions, Node* at);

    Value unary(UnaryExpr* u);

    // Whether `a && b` or `a || b` calls an operator function (a member of the left operand's class
    // or a free one), as the class-operand path of binary() finds it.
    bool userLogicalOperator(BinaryExpr* b, P op);
    // ... or an enumeration's own (agui's `PropertyStatus operator||(PropertyStatus, PropertyStatus)`),
    // which binary() calls as it calls `Flags operator|(Flags, Flags)`.
    bool userEnumLogicalOperator(BinaryExpr* b, P op);

    // Calls f, the operator function the checker chose for e (design.md 24), on operands (the left one
    // first): a member on the first operand's object (the class or the base declaring it), a free
    // function with them all, a template's arguments deduced from them, a friend's class's arguments
    // from the operands' classes.
    Value callChosenOperator(Expr* e, FunctionDecl* f, bool member, Span<Expr*> operands);

    Value binary(BinaryExpr* b);

    // --- three-way comparison ---

    // The library's strong_ordering (what `a <=> b` yields), or null without the library.
    const Ty* orderingType();

    // The order of two scalars as an i8: -1, 0 or 1.
    Value orderOfScalars(Value l, Value r, Node* at);

    // An order (i8) as the value of `a <=> b`: a strong_ordering object, or an int without the library.
    Value orderingObject(Value order, Node* at);

    // The i8 inside the value of `a <=> b`.
    Value orderOf(Value ordering, Node* at);

    // `a <=> b` of two objects at those addresses: the class's operator<=> (memberwise when
    // defaulted); the order as an i8.
    Value orderOfObjects(const std::string& a, const std::string& b, const Ty* t, Node* at);

    // The order of two objects base by base and member by member: the first that differs decides.
    Value memberwiseOrder(const std::string& a, const std::string& b, const Ty* t, Node* at);

    // The order (i8) of two parts of objects (members, elements) at those addresses.
    Value orderOfParts(const std::string& a, const std::string& b, const Ty* t, Node* at);

    // The order of two arrays at those addresses element by element: the first that differs decides.
    Value orderOfArrays(const std::string& a, const std::string& b, const Ty* t, Node* at);

    // The comparisons C++ rewrites when the class defines no operator for them: `a != b` as
    // `!(a == b)`, `a < b` and the other orderings as `(a <=> b) < 0`, and `==` of a class whose
    // operator<=> is defaulted as `(a <=> b) == 0`. Null when the class has none of those either.
    std::optional<Value> rewrittenComparison(BinaryExpr* b, P op, const Ty* lt, const Ty* rt);

    // The address `object.*member` / `pointer->*member` denotes: the object plus the offset.
    std::string memberPointerAddress(BinaryExpr* b);

    Value logical(bool isAnd, Expr* lhs, Expr* rhs, Node* at);

