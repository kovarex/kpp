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

    Value rvalue(Expr* e);

    // The bit-field an lvalue names, or null: `x.f`, `this->data.f`, `f` in a member function, in
    // parentheses or not (what an assignment, a compound assignment or an increment writes, its own
    // bits of the storage unit alone). Asked right after lvalue(e), which records the place of the
    // member it found in the layout holding it (lvalueBitfield): that instance's own. A member alone
    // does not tell it: an instance's members are its template's declarations, and Bits<3> and
    // Bits<5> place `uint32_t a : N` differently (kppc read one instance's bits with the other's).
    std::pair<const Expr*, const BitfieldInfo*> lvalueBitfield{nullptr, nullptr}; // the member lvalue computed last, its bit field
    const BitfieldInfo* bitfieldNamed(Expr* e);

    bool mallocKnown = false; // this emitter saw malloc declared (it stays declared)
    void ensureMalloc();

    // The bytes before the elements of `new T[n]`: a cookie whose last 8 hold n, when T needs
    // destroying (as MSVC's runtime lays it out), so that delete[] destroys all n; 16 for an element
    // aligned to 16, malloc's alignment. A type that needs no destroying has none (delete[] frees).
    uint64_t arrayCookie(const Ty* t, Node* at);

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
    void arrayDelete(const std::string& p, const Ty* t, Node* at);

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

