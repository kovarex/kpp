// struct Emitter's members: typed unions: the declarations, part of the struct's body, included in
// it in Codegen/Emitter.hpp; the definitions in TypedUnions.cpp (codegen.cpp split, stage 2,
// 2026-11-05, moved without a change).
    // --- typed unions (design.md 8.5): a tag and the storage of one alternative ---

    ClassDecl* typedUnionOf(const Ty* t, Node* at);
    const Ty* tagType(ClassDecl* c) { return types.enumType(c->tagEnum); }
    std::string tagAddress(ClassDecl* c, const Ty* t, const std::string& obj, Node* at);
    std::string loadTag(ClassDecl* c, const Ty* t, const std::string& obj, Node* at);
    const Ty* alternativeType(ClassDecl* c, const Ty* t, size_t index, Node* at);
    std::string alternativeAddress(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Node* at);
    // The alternative of typed union c (of type t) a value of type valueTy sets (design.md 8.5):
    // the one whose type is valueTy without references and const. None, or two of that type, is
    // a compile error (two: set it by name, `u[Text] = x`).
    size_t alternativeForValue(ClassDecl* c, const Ty* t, const Ty* valueTy, Node* at);
    // The alternative a constant tag names: an enumerator of U::Tag (`U::Text`), a constant of it, or
    // the tag of a descriptor of `alternatives(U)` in a `for constexpr` body (`a.tag`).
    std::optional<int64_t> constantTag(ClassDecl* c, Expr* e);

    // The index among c's alternatives of the one a descriptor of `alternatives(U)` names, or -1.
    static int64_t descriptorAlternative(ClassDecl* c, const Ty* descriptor);
    // The alternative an index names: `p[Property::Integer]`, a constant enumerator of the tag, or
    // `p[a]` with a descriptor of alternatives(U).
    size_t alternativeIndex(ClassDecl* c, IndexExpr* i, Node* at);
    // The address of the alternative p[E] names; with check, a release_assert that p holds it.
    std::string alternativeLvalue(ClassDecl* c, Value base, IndexExpr* i, Node* at, bool check);
    void setTag(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Node* at);
    // Sets the tag and builds the alternative from init (default-constructed without one).
    void constructAlternative(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Expr* init, Node* at);
    // Runs body(index) for the alternative the object holds, among those `wanted` selects: a
    // switch on the tag.
    template<typename Wanted, typename Body>
    void forHeldAlternative(ClassDecl* c, const Ty* t, const std::string& obj, Node* at, Wanted&& wanted, Body&& body)
    {
      std::vector<size_t> indices;
      for (size_t i = 0; i < c->alternatives.size(); ++i)
        if (wanted(i))
          indices.push_back(i);
      if (indices.empty())
        return;
      std::string tag = loadTag(c, t, obj, at);
      std::string k = llType(tagType(c), at);
      std::string endL = newLabel("alt.end");
      std::vector<std::string> labels;
      std::string cases;
      for (size_t i : indices)
      {
        labels.push_back(newLabel("alt"));
        cases += "    " + k + " " + std::to_string(i) + ", label %" + labels.back() + "\n";
      }
      fn += "  switch " + k + " " + tag + ", label %" + endL + " [\n" + cases + "  ]\n";
      blockTerminated = true;
      for (size_t n = 0; n < indices.size(); ++n)
      {
        startBlock(labels[n]);
        body(indices[n]);
        emitBranch(endL);
      }
      startBlock(endL);
    }
    void destroyAlternative(ClassDecl* c, const Ty* t, const std::string& obj, Node* at);
    // Copies (or moves) the alternative src holds into dst, tag included.
    void copyAlternative(ClassDecl* c, const Ty* t, const std::string& dst, const std::string& src, bool move, Node* at);
    // dst holds another alternative than src: its copy assignment (design.md 8.5).
    void copyOverAlternative(ClassDecl* c, const Ty* t, const std::string& dst, const std::string& src, Node* at);
    // Whether an alternative of this type is a class with an operator= to call.
    bool alternativeAssignable(const Ty* altTy, Node* at);
    // dst and src hold one alternative: src's assigned to dst's, by its operator=.
    void assignHeldAlternative(ClassDecl* c, const Ty* t, const std::string& dst, const std::string& src, bool move, Node* at);
    // `p[Integer] = 5`: setAlternative for the alternative the left operand names.
    Value assignAlternative(BinaryExpr* b);
    // rhs assigned to alternative index of the union at obj (`u[Name] = rhs`, named being that
    // assignment, or `u = rhs`, null): as std::variant's assignment (design.md 8.5).
    Value setAlternative(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Expr* rhs, BinaryExpr* named, Node* at);
    // Whether operator= f takes a cls (its copy or move assignment), no value of another type.
    bool isAssignmentOf(FunctionDecl* f, const Ty* cls, Node* at);
    // The tag values of a typed union's alternatives, in declaration order (the cases of a switch).
    std::vector<int64_t> alternativeTags(ClassDecl* c);

    // Runs part(k) in a block of its own for the alternative k both typed unions at a and b hold,
    // their tags known equal; the blocks go on to done.
    template<typename F>
    void forEachHeldAlternative(ClassDecl* c, const Value& tag, const std::string& done, Node* at, F&& part)
    {
      std::vector<int64_t> tags = alternativeTags(c);
      if (tags.size() != c->alternatives.size())
        unsupported(at, "a typed union whose tags are not its alternatives'");
      const Ty* underlying = enumUnderlying(c->tagEnum);
      std::string type = llType(underlying, at);
      std::vector<std::string> labels;
      std::string sw = "  switch " + type + " " + tag.text + ", label %" + done + " [";
      for (size_t k = 0; k < tags.size(); ++k)
      {
        labels.push_back(newLabel("union.alternative"));
        sw += " " + type + " " + std::to_string(tags[k]) + ", label %" + labels.back();
      }
      fn += sw + " ]\n";
      blockTerminated = true;
      for (size_t k = 0; k < labels.size(); ++k)
      {
        startBlock(labels[k]);
        part(k);
        emitBranch(done);
      }
    }

    // a == b of two typed unions at those addresses (design.md 8.5, as std::variant's): the same
    // alternative held, and equal values of it.
    Value typedUnionEqual(ClassDecl* c, const std::string& a, const std::string& b, const Ty* t, Node* at);

    // The order (i8) of two typed unions at those addresses (design.md 8.5, as std::variant's): by
    // the alternative held, in declaration order, then by the values of it.
    Value typedUnionOrder(ClassDecl* c, const std::string& a, const std::string& b, const Ty* t, Node* at);

    // `p == Property::Integer`: the tag against the enumerator.
    Value compareTag(BinaryExpr* b, bool negate);

