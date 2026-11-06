// struct Emitter's members: assignments, operator calls, arithmetic, comparison and call
// expressions: the declarations, part of the struct's body, included in it in Codegen/Emitter.hpp;
// the definitions in Operators.cpp (codegen.cpp split, stage 2, 2026-11-05, moved without a
// change).
    static bool isAssignment(P op);

    static P baseOp(P op);

    // Whether evaluating e may write what another operand of its expression reads: it calls (a
    // function, a class's operator or constructor), assigns, steps (++, --), or news, deletes or
    // throws. (A lambda's body runs when it is called, not here.)
    bool mayWrite(Expr* e);

    // Whether e designates a place no other operand's evaluation can move: a variable (a reference
    // one is bound once), `this`, or a data member of one of those reached by `.` or `this->`, an
    // element of an array among them at a constant index, or one's typed union's alternative
    // (`target[a] = a.type(input)`: built in place, as before).
    bool fixedPlace(Expr* e);
    bool fixedName(Expr* e);

    // design.md 24 (decisions are made once): with KPP_VERIFY_DECISIONS=1 in the environment, what
    // codegen chooses for an operator is compared with what the checker recorded on its node (call,
    // callee, calleeMember), and a difference is an error naming both. A node the checker left
    // Undecided or Rewritten is not compared. This is the cross-check before codegen reads the
    // recorded choice instead of choosing again.
    static bool verifyDecisions();
    static const char* operatorCallName(OperatorCall c);
    template<class E>
    void verifyOperatorCall(const E* e, OperatorCall call, FunctionDecl* f = nullptr, bool member = false)
    {
      if (!verifyDecisions() || e->call == OperatorCall::Undecided || e->call == OperatorCall::Rewritten)
        return;
      if (e->call == call && (call != OperatorCall::Function || (e->callee == f && e->calleeMember == member)))
        return;
      auto describe = [&](OperatorCall c, FunctionDecl* g, bool m)
      {
        if (c != OperatorCall::Function || !g)
          return std::string(operatorCallName(c));
        return (m ? "the member " : "the free ") + qualifiedName(g) + " (" + locationOf(g, g->file) + ")";
      };
      std::string where = currentFunctionName.empty() ? std::string() : " (in " + currentFunctionName + ")";
      error(const_cast<E*>(e), "decision check: codegen calls " + describe(call, f, member) + ", the checker chose " +
                                   describe(e->call, e->callee, e->calleeMember) + where);
    }

    // A range-for's begin and end as codegen chose them (members when members) against the checker's.
    void verifyRangeFunctions(RangeForStmt* f, bool members, FunctionDecl* begin, FunctionDecl* end, const Ty* range);

    // Finds the best member with the given name (searching bases, nearest class wins) for the
    // argument types and the object's constness. Returns the class type it was found in.
    FunctionDecl* findMemberFunction(const Ty* classTy, const std::string& name, const std::vector<const Ty*>& argTypes, const Ty* objectTy, Node* at, const Ty** foundIn, bool templates = false);

    FunctionDecl* findOperator(const Ty* classTy, const std::string& name, size_t argCount, Node* at, const Ty** foundIn);

    // Calls an operator member on an object. The result of a reference-returning operator is
    // the referenced address (ty keeps the reference). sret: where a class result is built (the
    // variable `Chunk c = maker(5)` initialises), or empty for a temporary.
    // An object's value with the const the checker gave its expression (`this->byChunk` in a
    // const member function is const even where the field's own type is not), so that an
    // operator chosen on it is the one a const object calls (`T* operator[](int) const`).
    Value withCheckedConst(Value v, Expr* e);

    // Whether e names a const object: as the checker types it, or as its path makes it (a data
    // member of a const object is const unless mutable: `this->byChunk` in a const member
    // function, `p->field` with p a pointer to const).
    bool constObject(Expr* e, int depth = 0);

    Value operatorCall(Value object, const std::string& name, Span<Expr*> args, Node* at, bool required, const std::string& sret = "");

    // An expression naming a place code generation already has, for the length of this object.
    struct PlaceName
    {
      PlaceName(Emitter& emitter, const std::string& at, const Ty* t, uint32_t token);
      PlaceName(const PlaceName&) = delete;
      ~PlaceName();

      Emitter& e;
      VariableDecl variable{};
      NameExpr name{};
    };

    // An operand evaluated before the expression it is in, here where this object is made:
    // standIn stands for it there, as the place it designates, evaluating nothing again.
    struct EvaluatedOperand
    {
      EvaluatedOperand(Emitter& e, Expr* rhs, Node* at);

      PlaceName place;
      NamedCastExpr rvalueCast{};
      Expr* standIn = nullptr;

    private:
      static std::string placeOf(Emitter& e, Expr* rhs, Node* at);
    };

    // The operator function the checker chose for e (design.md 24), when codegen calls the recorded
    // choice: a function, in a body whose nodes its own walk typed (choicesRecorded). Null: codegen
    // chooses itself (the built-in operator, a rewritten comparison, an undecided node).
    template<class E>
    FunctionDecl* recordedOperator(const E* e) const
    {
      return choicesRecorded && e->call == OperatorCall::Function ? e->callee : nullptr;
    }

    // The class that declares member f, objectTy's own class or one of its bases (as instantiated for
    // objectTy): where a member the checker chose is called. Null when none does.
    const Ty* declaringClass(const Ty* objectTy, FunctionDecl* f, Node* at);

    // Calls member operator f, declared in foundIn (the object's class or a base), on object.
    Value memberCall(Value object, FunctionDecl* f, const Ty* foundIn, Span<Expr*> args, Node* at, const std::string& sret = "");

    Value arithmetic(P op, Value l, Value r, Node* at);

    Value compare(P op, Value l, Value r, Node* at);

    void emitBranch(const std::string& target);

    void startBlock(const std::string& name);

    // Call expression. sret: where to construct a class result (empty: a temporary).
    Value callValue(CallExpr* c, const std::string& sret);

    // A call through a function pointer, with the same calling convention as a declared kpp
    // function: the object first for a member function (thisArg), the caller's result slot for a
    // class result, classes by pointer (a copy the callee owns), references as addresses.
    Value callIndirect(const std::string& fnText, const Ty* ft, const std::string& thisArg, Span<Expr*> callArgs, const std::string& sret, Node* c);

    // va_start, va_end and va_copy on a C va_list (a char* on x64 Windows): the LLVM intrinsics.
    // They are not declared anywhere; a function defined with `...` uses them like C does.
    std::optional<Value> vaBuiltin(NameExpr* n, CallExpr* c);

