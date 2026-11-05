// struct Emitter's members: typed unions. Part of codegen.cpp, included in the struct's body in
// Codegen/Emitter.hpp (a file of its own since 2026-11-05, moved without a change).
    // --- typed unions (design.md 8.5): a tag and the storage of one alternative ---

    ClassDecl* typedUnionOf(const Ty* t, Node* at)
    {
      t = t ? types.unqualified(types.stripRef(t)) : nullptr;
      if (!isClass(t))
        return nullptr;
      ClassDecl* c = classDef(t->decl, at);
      return c && c->isTypedUnion ? c : nullptr;
    }
    const Ty* tagType(ClassDecl* c) { return types.enumType(c->tagEnum); }
    std::string tagAddress(ClassDecl* c, const Ty* t, const std::string& obj, Node* at)
    {
      std::string addr;
      if (!fieldAddress(types.unqualified(types.stripRef(t)), c->tagField, obj, addr, at))
        unsupported(at, "a typed union without its tag");
      return addr;
    }
    std::string loadTag(ClassDecl* c, const Ty* t, const std::string& obj, Node* at)
    {
      return load(tagAddress(c, t, obj, at), tagType(c), at).text;
    }
    const Ty* alternativeType(ClassDecl* c, const Ty* t, size_t index, Node* at)
    {
      const Ty* cls = canonicalClass(types.unqualified(types.stripRef(t)), at);
      const Ty* alt = global.typeOfDeclIn(c->alternatives[index], global.classArgsOf(cls));
      if (!alt || !alt->isKnown())
        unsupported(at, "an alternative of a typed union whose type is unknown");
      return alt;
    }
    std::string alternativeAddress(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Node* at)
    {
      std::string addr;
      if (!fieldAddress(types.unqualified(types.stripRef(t)), c->alternatives[index], obj, addr, at))
        unsupported(at, "a typed union without its storage");
      return addr;
    }
    // The alternative of typed union c (of type t) a value of type valueTy sets (design.md 8.5):
    // the one whose type is valueTy without references and const. None, or two of that type, is
    // a compile error (two: set it by name, `u[Text] = x`).
    size_t alternativeForValue(ClassDecl* c, const Ty* t, const Ty* valueTy, Node* at)
    {
      const Ty* v = types.unqualified(types.stripRef(valueTy));
      std::vector<size_t> found;
      for (size_t i = 0; i < c->alternatives.size(); ++i)
        if (types.unqualified(alternativeType(c, t, i, at)) == v)
          found.push_back(i);
      auto name = [&](size_t i) { return sym(static_cast<EnumeratorDecl*>(static_cast<EnumDecl*>(c->tagEnum)->enumerators[i])->name); };
      if (found.empty())
        fail(at, "the typed union " + qualifiedName(c) + " has no alternative of type " + types.toString(v, &symbols));
      if (found.size() > 1)
        fail(at, "a " + types.toString(v, &symbols) + " is ambiguous for the typed union " + qualifiedName(c) + ": its alternatives " +
                     name(found[0]) + " and " + name(found[1]) + " both have that type (name one: " + qualifiedName(c) + "(" + qualifiedName(c) +
                     "::" + name(found[0]) + ", value), or u[" + name(found[0]) + "] = value)");
      return found[0];
    }
    // The alternative a constant tag names: an enumerator of U::Tag (`U::Text`), a constant of it, or
    // the tag of a descriptor of `alternatives(U)` in a `for constexpr` body (`a.tag`).
    std::optional<int64_t> constantTag(ClassDecl* c, Expr* e)
    {
      Expr* x = e;
      while (x && x->kind == NodeKind::ParenExpr)
        x = static_cast<ParenExpr*>(x)->inner;
      if (x && x->kind == NodeKind::MemberExpr)
      {
        auto* m = static_cast<MemberExpr*>(x);
        const Ty* bt = m->base && m->base->ty ? types.stripRef(m->base->ty) : nullptr;
        if (bt && bt->kind == TyKind::Member && !m->member.empty() && sym(m->member.last().symbol) == "tag")
          if (int64_t index = descriptorAlternative(c, bt); index >= 0)
            return index;
      }
      return evalConst(e);
    }

    // The index among c's alternatives of the one a descriptor of `alternatives(U)` names, or -1.
    static int64_t descriptorAlternative(ClassDecl* c, const Ty* descriptor)
    {
      if (!descriptor || descriptor->kind != TyKind::Member)
        return -1;
      for (size_t i = 0; i < c->alternatives.size(); ++i)
        if (c->alternatives[i] == descriptor->decl)
          return int64_t(i);
      return -1;
    }
    // The alternative an index names: `p[Property::Integer]`, a constant enumerator of the tag, or
    // `p[a]` with a descriptor of alternatives(U).
    size_t alternativeIndex(ClassDecl* c, IndexExpr* i, Node* at)
    {
      if (i->args.size() != 1)
        unsupported(at, "a typed union indexed with more than one alternative");
      const Ty* indexTy = i->args[0]->ty ? types.stripRef(i->args[0]->ty) : nullptr;
      std::optional<int64_t> v = indexTy && indexTy->kind == TyKind::Member ? descriptorAlternative(c, indexTy) : evalConst(i->args[0]);
      if (!v || *v < 0 || size_t(*v) >= c->alternatives.size())
        unsupported(at, "a typed union indexed with something other than one of its alternatives");
      return size_t(*v);
    }
    // The address of the alternative p[E] names; with check, a release_assert that p holds it.
    std::string alternativeLvalue(ClassDecl* c, Value base, IndexExpr* i, Node* at, bool check)
    {
      size_t index = alternativeIndex(c, i, at);
      if (check)
      {
        std::string tag = loadTag(c, base.ty, base.text, at);
        std::string same = newTemp();
        fn += "  " + same + " = icmp eq " + llType(tagType(c), at) + " " + tag + ", " + std::to_string(index) + "\n";
        std::string okL = newLabel("alt.ok"), failL = newLabel("alt.fail");
        fn += "  br i1 " + same + ", label %" + okL + ", label %" + failL + "\n";
        startBlock(failL);
        auto [line, column] = lineColumnOf(at->token);
        emitCall("", "void @kpp_assert_failed(ptr " + cStringConstant(programPathOf(currentFile)) + ", i32 " + std::to_string(line) + ", i32 " +
                         std::to_string(column) + ", ptr " + cStringConstant("the typed union holds another alternative") + ")", false);
        fn += "  unreachable\n";
        usesAssert = true;
        startBlock(okL);
      }
      return alternativeAddress(c, base.ty, base.text, index, at);
    }
    void setTag(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Node* at)
    {
      fn += "  store " + llType(tagType(c), at) + " " + std::to_string(index) + ", ptr " + tagAddress(c, t, obj, at) + "\n";
    }
    // Sets the tag and builds the alternative from init (default-constructed without one).
    void constructAlternative(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Expr* init, Node* at)
    {
      setTag(c, t, obj, index, at);
      const Ty* altTy = alternativeType(c, t, index, at);
      std::string addr = alternativeAddress(c, t, obj, index, at);
      if (init)
        initializeFrom(addr, altTy, init, at);
      else if (isClass(altTy))
        defaultConstruct(addr, altTy, at);
      else if (altTy->kind == TyKind::Array && needsDestruction(altTy, at))
        constructArray(addr, altTy, at);
      else
        storeZero(altTy, addr, at);
    }
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
    void destroyAlternative(ClassDecl* c, const Ty* t, const std::string& obj, Node* at)
    {
      forHeldAlternative(c, t, obj, at,
                         [&](size_t i) { return needsDestruction(alternativeType(c, t, i, at), at); },
                         [&](size_t i) { destroy(alternativeAddress(c, t, obj, i, at), alternativeType(c, t, i, at), at); });
    }
    // Copies (or moves) the alternative src holds into dst, tag included.
    void copyAlternative(ClassDecl* c, const Ty* t, const std::string& dst, const std::string& src, bool move, Node* at)
    {
      std::string tag = loadTag(c, t, src, at);
      fn += "  store " + llType(tagType(c), at) + " " + tag + ", ptr " + tagAddress(c, t, dst, at) + "\n";
      forHeldAlternative(c, t, src, at, [&](size_t) { return true; }, [&](size_t i)
      {
        const Ty* altTy = alternativeType(c, t, i, at);
        std::string from = alternativeAddress(c, t, src, i, at), to = alternativeAddress(c, t, dst, i, at);
        if (isClass(altTy))
        {
          if (move)
            moveConstruct(to, from, altTy, at);
          else
            copyConstruct(to, from, altTy, at);
        }
        else
        {
          copyBytes(to, from, altTy, at);
        }
      });
    }
    // `p[Integer] = 5`: destroys the alternative held, sets the tag, constructs the new value.
    Value assignAlternative(IndexExpr* i, Expr* rhs, Node* at)
    {
      Value base = rvalue(i->base);
      ClassDecl* c = typedUnionOf(base.ty, at);
      size_t index = alternativeIndex(c, i, at);
      destroyAlternative(c, base.ty, base.text, at);
      constructAlternative(c, base.ty, base.text, index, rhs, at);
      const Ty* altTy = alternativeType(c, base.ty, index, at);
      std::string addr = alternativeAddress(c, base.ty, base.text, index, at);
      return isClass(altTy) ? Value{addr, altTy, false} : load(addr, altTy, at);
    }
    // The tag values of a typed union's alternatives, in declaration order (the cases of a switch).
    std::vector<int64_t> alternativeTags(ClassDecl* c)
    {
      std::vector<int64_t> tags;
      if (auto* e = static_cast<EnumDecl*>(c->tagEnum))
        for (Decl* d : e->enumerators)
          if (d->kind == NodeKind::EnumeratorDecl)
            tags.push_back(enumeratorValue(static_cast<EnumeratorDecl*>(d)));
      return tags;
    }

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
    Value typedUnionEqual(ClassDecl* c, const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      const Ty* underlying = enumUnderlying(c->tagEnum);
      Value ta = convert(Value{loadTag(c, t, a, at), tagType(c), false}, underlying, at);
      Value tb = convert(Value{loadTag(c, t, b, at), tagType(c), false}, underlying, at);
      std::string slot = "%equal" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i1\n";
      fn += "  store i1 false, ptr " + slot + "\n";
      std::string same = newTemp(), held = newLabel("unionequal.held"), done = newLabel("unionequal.done");
      fn += "  " + same + " = icmp eq " + llType(underlying, at) + " " + ta.text + ", " + tb.text + "\n";
      fn += "  br i1 " + same + ", label %" + held + ", label %" + done + "\n";
      blockTerminated = true;
      startBlock(held);
      forEachHeldAlternative(c, ta, done, at, [&](size_t k) {
        Value e = partsEqual(alternativeAddress(c, t, a, k, at), alternativeAddress(c, t, b, k, at), alternativeType(c, t, k, at), at);
        fn += "  store i1 " + e.text + ", ptr " + slot + "\n";
      });
      startBlock(done);
      return load(slot, types.boolType(), at);
    }

    // The order (i8) of two typed unions at those addresses (design.md 8.5, as std::variant's): by
    // the alternative held, in declaration order, then by the values of it.
    Value typedUnionOrder(ClassDecl* c, const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      const Ty* underlying = enumUnderlying(c->tagEnum);
      Value ta = convert(Value{loadTag(c, t, a, at), tagType(c), false}, underlying, at);
      Value tb = convert(Value{loadTag(c, t, b, at), tagType(c), false}, underlying, at);
      std::string slot = "%order" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i8\n";
      std::string type = llType(underlying, at);
      std::string less = newTemp(), byTag = newTemp(), same = newTemp();
      fn += "  " + less + " = icmp ult " + type + " " + ta.text + ", " + tb.text + "\n";
      fn += "  " + byTag + " = select i1 " + less + ", i8 -1, i8 1\n";
      fn += "  store i8 " + byTag + ", ptr " + slot + "\n";
      std::string held = newLabel("unionorder.held"), done = newLabel("unionorder.done");
      fn += "  " + same + " = icmp eq " + type + " " + ta.text + ", " + tb.text + "\n";
      fn += "  br i1 " + same + ", label %" + held + ", label %" + done + "\n";
      blockTerminated = true;
      startBlock(held);
      fn += "  store i8 0, ptr " + slot + "\n";
      forEachHeldAlternative(c, ta, done, at, [&](size_t k) {
        Value o = orderOfParts(alternativeAddress(c, t, a, k, at), alternativeAddress(c, t, b, k, at), alternativeType(c, t, k, at), at);
        fn += "  store i8 " + o.text + ", ptr " + slot + "\n";
      });
      startBlock(done);
      return load(slot, types.intType(8), at);
    }

    // `p == Property::Integer`: the tag against the enumerator.
    Value compareTag(BinaryExpr* b, bool negate)
    {
      bool unionLeft = typedUnionOf(types.stripRef(b->lhs->ty), b) != nullptr;
      Expr* unionExpr = unionLeft ? b->lhs : b->rhs;
      Expr* tagExpr = unionLeft ? b->rhs : b->lhs;
      Value obj = rvalue(unionExpr);
      ClassDecl* c = typedUnionOf(obj.ty, b);
      const Ty* underlying = enumUnderlying(c->tagEnum);
      Value tag = convert(Value{loadTag(c, obj.ty, obj.text, b), tagType(c), false}, underlying, b);
      Value other = convert(rvalue(tagExpr), underlying, b);
      std::string r = newTemp();
      fn += "  " + r + " = icmp " + (negate ? "ne " : "eq ") + llType(underlying, b) + " " + tag.text + ", " + other.text + "\n";
      return {r, types.boolType(), false};
    }

