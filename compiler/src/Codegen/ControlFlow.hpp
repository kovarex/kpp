// struct Emitter's members: for constexpr, range-for, conditions and switch: the declarations, part
// of the struct's body, included in it in Codegen/Emitter.hpp; the definitions in ControlFlow.cpp
// (codegen.cpp split, stage 2, 2026-11-05, moved without a change).
    // `types<A, B, C>`: the compile-time list of types (design.md 8.1).
    bool isTypesList(const NamedType* n);

    // `members(T)` (design.md 8.3), `alternatives(U)` (8.5): the type reflected on, or null when
    // the node is no such list; alternatives set when it is the latter.
    Expr* reflectionSubject(Node* list, bool* alternatives = nullptr);

    // The descriptors of a class's non-static data members, bases first, in declaration order.
    void collectMembers(const Ty* cls, std::vector<const Ty*>& out, Node* at, int depth);

    // `for constexpr (typename T : types<A, B>) body` / `for constexpr (int i : {1, 2}) body`: the
    // body once per element, re-typed and emitted with the loop variable bound to the element (a
    // type, or a constant), the way a pack expansion binds each element in turn.
    void forConstexpr(ForConstexprStmt* f);
    // An element of `for constexpr (auto v : {...})`: the constant as a template argument of its own
    // type (an integer, enumeration or floating one); null for any other value.
    const Ty* ownElement(const std::optional<ConstValue>& v);
    // `for constexpr (typename T : kinds) case T::kind: ...` directly in a switch's body: its elements
    // are the switch's cases (design.md 8.2), and `break` in it leaves the switch; any other one is a
    // statement of its own, which `break` leaves.
    bool labelsCases(ForConstexprStmt* f);

    // The statements of a for constexpr body: those of its block, or the statement itself.
    static std::vector<Stmt*> bodyStatements(Stmt* body);

    // Runs body(index) once per element of a for constexpr list, with the function re-typed for
    // the element and currentArgs binding the loop variable to it; the original typing is
    // restored afterwards.
    void forEachConstexprElement(ForConstexprStmt* f, const std::function<void(size_t)>& body);

    // The temporary a range-for's range is a member subobject of (`Duo{...}.a`, `makeHolder().items[1]`):
    // the base of a chain of `.member` and array elements, when it is a class prvalue. The reference
    // the range binds extends its whole lifetime to the loop's end ([class.temporary]/6), not the
    // other temporaries of the range's expression.
    const Ty* rangeTemporaryBase(Expr* e);

    // Moves the statement's temporary of class cls made since `since` (the last one: the base the
    // range is a member of) to the loop's scope, which destroys it when the loop ends.
    void extendRangeTemporary(const Ty* cls, size_t since, Node* at);

    void rangeFor(RangeForStmt* f);

    Value condition(Node* cond, Node* at);

    void switchStatement(SwitchStmt* sw);

