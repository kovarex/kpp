// struct Emitter's members: parameter packs. Part of codegen.cpp, included in the struct's body in
// Codegen/Emitter.hpp (a file of its own since 2026-11-05, moved without a change).
    // --- parameter packs ---

    // The pack parameter an expression refers to (the first one found), or null.
    Decl* packIn(Node* n, int depth = 0)
    {
      if (!n || depth > 32)
        return nullptr;
      switch (n->kind)
      {
        case NodeKind::NameExpr:
        {
          Decl* d = static_cast<NameExpr*>(n)->resolved;
          return d && packLocals.count(d) ? d : nullptr;
        }
        case NodeKind::ParenExpr: return packIn(static_cast<ParenExpr*>(n)->inner, depth + 1);
        case NodeKind::UnaryExpr: return packIn(static_cast<UnaryExpr*>(n)->operand, depth + 1);
        case NodeKind::CastExpr: return packIn(static_cast<CastExpr*>(n)->operand, depth + 1);
        case NodeKind::NamedCastExpr: return packIn(static_cast<NamedCastExpr*>(n)->operand, depth + 1);
        case NodeKind::MemberExpr: return packIn(static_cast<MemberExpr*>(n)->base, depth + 1);
        case NodeKind::BinaryExpr:
          if (Decl* d = packIn(static_cast<BinaryExpr*>(n)->lhs, depth + 1))
            return d;
          return packIn(static_cast<BinaryExpr*>(n)->rhs, depth + 1);
        case NodeKind::CallExpr:
        {
          auto* c = static_cast<CallExpr*>(n);
          if (Decl* d = packIn(c->callee, depth + 1))
            return d;
          for (Expr* a : c->args)
            if (Decl* d = packIn(a, depth + 1))
              return d;
          return nullptr;
        }
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(n);
          if (Decl* d = packIn(i->base, depth + 1))
            return d;
          for (Expr* a : i->args)
            if (Decl* d = packIn(a, depth + 1))
              return d;
          return nullptr;
        }
        case NodeKind::FunctionalCastExpr:
          for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
            if (Decl* d = packIn(a, depth + 1))
              return d;
          return nullptr;
        default:
          return nullptr;
      }
    }

    // The template parameter packs a pack parameter's declared type names, in order: `Args&&... args`
    // names Args; `auto PrototypeT::*... members` names PrototypeT and the pack invented for the
    // `auto`. An expansion binds each of them to its i-th element together.
    std::vector<Decl*> templatePacksOf(Decl* param)
    {
      std::vector<Decl*> packs;
      collectPacks(global.typeOfDecl(param), packs, 0);
      return packs;
    }

    static void collectPacks(const Ty* t, std::vector<Decl*>& packs, int depth)
    {
      if (!t || depth > 16)
        return;
      if (t->kind == TyKind::Param && t->decl && t->decl->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(t->decl)->isPack)
      {
        if (std::find(packs.begin(), packs.end(), t->decl) == packs.end())
          packs.push_back(t->decl);
        return;
      }
      collectPacks(t->inner, packs, depth + 1);
      for (const Ty* a : t->args)
        collectPacks(a, packs, depth + 1);
    }

    // The element types each pack named by param's type is bound to in the instantiation being
    // emitted, all of `count` elements; unsupported when one is not bound to such a pack.
    std::vector<const Ty*> boundPacks(const std::vector<Decl*>& packs, size_t count, Node* at)
    {
      std::vector<const Ty*> bound;
      for (Decl* tp : packs)
      {
        const Ty* packTy = nullptr;
        if (currentArgs)
          for (auto& [d, t] : currentArgs->pairs)
            if (d == tp)
              packTy = t;
        if (!packTy || packTy->kind != TyKind::Pack || packTy->args.size() != count)
          unsupported(at, "a pack whose elements are unknown");
        bound.push_back(packTy);
      }
      return bound;
    }

    // Runs body(i) for each element of the pack that `pattern` expands over, with the pack
    // parameter bound to that element and the function body re-typed for it.
    // Runs body on an expression, or, for a pack expansion `args...` of exactly one element, on its
    // pattern with that element bound (a scalar member initialised with `forward<Args>(args)...`).
    template<typename F>
    void withSingleElement(Expr* e, Node* at, F&& body)
    {
      if (!isPackExpansion(e))
      {
        body(e);
        return;
      }
      Expr* pattern = static_cast<UnaryExpr*>(e)->operand;
      if (packElementCount(pattern, at) != 1)
        unsupported(at, "a pack of " + std::to_string(packElementCount(pattern, at)) + " elements where one value is expected");
      forEachPackElement(pattern, at, [&](size_t) { body(pattern); });
    }

    // How many values arguments stand for once their pack expansions are expanded.
    size_t expandedCount(Span<Expr*> args, Node* at)
    {
      size_t n = 0;
      for (Expr* a : args)
        n += isPackExpansion(a) ? packElementCount(static_cast<UnaryExpr*>(a)->operand, at) : 1;
      return n;
    }

    // Runs body on each value arguments stand for: an expression as it is, a pack expansion's
    // pattern once per element, the element bound.
    template<typename F>
    void forEachExpanded(Span<Expr*> args, Node* at, F&& body)
    {
      for (Expr* a : args)
        if (isPackExpansion(a))
        {
          Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
          forEachPackElement(pattern, at, [&](size_t) { body(pattern); });
        }
        else
          body(a);
    }

    size_t packElementCount(Expr* pattern, Node* at)
    {
      Decl* param = packIn(pattern);
      if (!param)
        if (Decl* pack = currentArgs ? typePackIn(pattern) : nullptr)
          return boundPack(pack)->args.size(); // `values[I]...` over `int... I`
      if (!param)
        unsupported(at, "a pack expansion that does not name a parameter pack");
      return packLocals[param].size();
    }

    // What the instantiation binds a template parameter pack to (typePackIn found it bound).
    const Ty* boundPack(Decl* pack)
    {
      for (auto& [d, t] : currentArgs->pairs)
        if (d == pack)
          return t;
      return nullptr;
    }

    template<typename F>
    void forEachPackElement(Expr* pattern, Node* at, F&& body)
    {
      Decl* param = packIn(pattern);
      if (!param && currentArgs)
        if (Decl* pack = typePackIn(pattern))
        {
          // `values[I]...` over a template parameter pack (`int... I` of the index-sequence idiom):
          // the pattern once per element, with I bound to that element.
          forEachTypePackElement(pack, body);
          return;
        }
      if (!param)
        unsupported(at, "a pack expansion that does not name a parameter pack");
      std::vector<Decl*> packs = templatePacksOf(param);
      if (packs.empty() || !currentArgs)
        unsupported(at, "a pack expansion outside an instantiation");
      std::vector<Local> elements = packLocals[param];
      boundPacks(packs, elements.size(), at); // (each as long as the parameter pack)
      TemplateArgs original = *currentArgs;
      for (size_t i = 0; i < elements.size(); ++i)
      {
        // The pattern's mentions of the packs name element i; the rest of the body keeps them whole.
        TemplateArgs elementArgs = original;
        elementArgs.elementPacks = packs;
        elementArgs.elementIndex = uint32_t(i);
        global.retypeFor(currentFunction, elementArgs, currentClassTy, true);
        locals[param] = elements[i];
        body(i);
      }
      locals.erase(param);
      global.retypeFor(currentFunction, original, currentClassTy);
    }

    // A template type pack that a pattern names in a type (`(func(Args()), ...)`,
    // `(... || isSame<T, Args>)`, `sizeof(Args)`) and the instantiation binds to a pack, or null.
    Decl* typePackIn(Node* n, int depth = 0)
    {
      if (!n || depth > 32 || !currentArgs)
        return nullptr;
      auto inName = [&](const QualifiedName& name) -> Decl*
      {
        for (const NamePart& part : name.parts)
          for (Node* a : part.templateArgs)
            if (Decl* d = typePackIn(a, depth + 1))
              return d;
        return nullptr;
      };
      auto either = [&](Node* a, Node* b) -> Decl* { Decl* d = typePackIn(a, depth + 1); return d ? d : typePackIn(b, depth + 1); };
      switch (n->kind)
      {
        case NodeKind::NamedType:
        {
          auto* t = static_cast<NamedType*>(n);
          if (Decl* d = t->resolved; d && d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isPack)
            for (auto& [p, a] : currentArgs->pairs)
              if (p == d && a && a->kind == TyKind::Pack)
                return d;
          return inName(t->name);
        }
        case NodeKind::PointerType: return typePackIn(static_cast<PointerType*>(n)->pointee, depth + 1);
        case NodeKind::LRefType: return typePackIn(static_cast<LRefType*>(n)->referenced, depth + 1);
        case NodeKind::RRefType: return typePackIn(static_cast<RRefType*>(n)->referenced, depth + 1);
        case NodeKind::ConstType: return typePackIn(static_cast<ConstType*>(n)->inner, depth + 1);
        case NodeKind::NameExpr:
        {
          // `Args()` parsed as a call of the name Args.
          if (Decl* d = static_cast<NameExpr*>(n)->resolved; d && d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isPack)
            for (auto& [p, a] : currentArgs->pairs)
              if (p == d && a && a->kind == TyKind::Pack)
                return d;
          return inName(static_cast<NameExpr*>(n)->name);
        }
        case NodeKind::ParenExpr: return typePackIn(static_cast<ParenExpr*>(n)->inner, depth + 1);
        case NodeKind::UnaryExpr: return typePackIn(static_cast<UnaryExpr*>(n)->operand, depth + 1);
        case NodeKind::MemberExpr: return typePackIn(static_cast<MemberExpr*>(n)->base, depth + 1);
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(n);
          if (Decl* d = typePackIn(i->base, depth + 1))
            return d;
          for (Expr* a : i->args)
            if (Decl* d = typePackIn(a, depth + 1))
              return d;
          return nullptr;
        }
        case NodeKind::CastExpr: return either(static_cast<CastExpr*>(n)->type, static_cast<CastExpr*>(n)->operand);
        case NodeKind::NamedCastExpr: return either(static_cast<NamedCastExpr*>(n)->type, static_cast<NamedCastExpr*>(n)->operand);
        case NodeKind::BinaryExpr: return either(static_cast<BinaryExpr*>(n)->lhs, static_cast<BinaryExpr*>(n)->rhs);
        case NodeKind::SizeofExpr:
        {
          auto* s = static_cast<SizeofExpr*>(n);
          return s->isPack ? nullptr : either(s->type, s->operand); // `sizeof...(Args)` counts the pack, it expands nothing
        }
        case NodeKind::FunctionalCastExpr:
        {
          auto* c = static_cast<FunctionalCastExpr*>(n);
          if (Decl* d = typePackIn(c->type, depth + 1))
            return d;
          for (Expr* a : c->args)
            if (Decl* d = typePackIn(a, depth + 1))
              return d;
          return nullptr;
        }
        case NodeKind::CallExpr:
        {
          auto* c = static_cast<CallExpr*>(n);
          if (Decl* d = typePackIn(c->callee, depth + 1))
            return d;
          for (Expr* a : c->args)
            if (Decl* d = typePackIn(a, depth + 1))
              return d;
          return nullptr;
        }
        default:
          return nullptr;
      }
    }

    // Runs body once per element of the type pack `pack` (typePackIn), the function re-typed with
    // the pack bound to that element (a private copy: retypedPerElement), and the element's
    // arguments in effect.
    template<class F>
    void forEachTypePackElement(Decl* pack, F&& body)
    {
      TemplateArgs original = *currentArgs;
      const Ty* bound = nullptr;
      for (auto& [d, t] : original.pairs)
        if (d == pack)
          bound = t;
      for (size_t i = 0; bound && i < bound->args.size(); ++i)
      {
        // The pattern's mentions of the pack name element i; the parameters, the class instance
        // and its members keep the whole pack (`lists` an `Array<Entry, sizeof...(Ts)>&`).
        TemplateArgs elementArgs = original;
        elementArgs.elementPacks = {pack};
        elementArgs.elementIndex = uint32_t(i);
        global.retypeFor(currentFunction, elementArgs, currentClassTy, true);
        ArgsScope scope(*this, &elementArgs);
        body(i);
      }
      global.retypeFor(currentFunction, original, currentClassTy);
    }

    bool isPackExpansion(Expr* e)
    {
      return e && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->isKeyword &&
             P(static_cast<UnaryExpr*>(e)->op) == P::Ellipsis && static_cast<UnaryExpr*>(e)->postfix;
    }

