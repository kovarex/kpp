// struct Emitter's members: lambdas and their closures. Part of codegen.cpp, included in the
// struct's body in Codegen/Emitter.hpp (a file of its own since 2026-11-05, moved without a
// change).
    // --- lambdas ---

    static bool localNamed(Decl* d, uint32_t symbol)
    {
      if (d && d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
        return static_cast<VariableDecl*>(d)->name.last().symbol == symbol;
      return d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->name == symbol;
    }

    // A variable of a function (a parameter, a local of automatic storage): what a lambda captures.
    static bool functionLocal(const Decl* d)
    {
      return d && (d->kind == NodeKind::ParamDecl ||
                   (d->kind == NodeKind::VariableDecl && !(d->flags & DF_Static) && d->parent && d->parent->kind == NodeKind::FunctionDecl));
    }

    // What a lambda's body names, from its AST alone: the locals of the functions around it (each
    // once, in the order first written: `used`) and whether it uses `this`. inner gathers what is
    // declared inside it (its parameters, its locals, nested lambdas' own), which is no capture;
    // C++ declares a name before its uses, so it is in inner before a use is reached.
    // useTypes, when given, gets the type of each used local's first name expression.
    // keptBranches: an `if constexpr` its instance decided names only what the kept branch names
    // (not in a generic lambda, whose one closure serves every call's instance of the body).
    void captureNames(Node* n, std::unordered_set<Decl*>& inner, std::vector<Decl*>& used, bool& usesThis,
                      std::unordered_map<Decl*, const Ty*>* useTypes = nullptr, int depth = 0, bool keptBranches = false)
    {
      if (!n || depth > 96)
        return;
      auto visit = [&](Node* c) { captureNames(c, inner, used, usesThis, useTypes, depth + 1, keptBranches); };
      auto visitExprs = [&](Span<Expr*> es) { for (Expr* e : es) visit(e); };
      switch (n->kind)
      {
        case NodeKind::NameExpr:
        {
          Decl* d = static_cast<NameExpr*>(n)->resolved;
          if (!d)
            break;
          if (functionLocal(d))
          {
            if (!inner.count(d) && std::find(used.begin(), used.end(), d) == used.end())
            {
              used.push_back(d);
              if (useTypes)
                (*useTypes)[d] = static_cast<NameExpr*>(n)->ty;
            }
          }
          else if ((d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) && global.owningClass(d) && !(d->flags & DF_Static))
            usesThis = true; // a member named without `this->`
          break;
        }
        case NodeKind::VariableDecl: // a condition that declares (`if (auto* p = f())`)
          inner.insert(static_cast<Decl*>(n));
          visit(static_cast<VariableDecl*>(n)->init);
          break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(n)->decls)
            if (d->kind == NodeKind::VariableDecl && !(keptBranches && d->whenKnown == 0)) // (a `when` its instance decided false names nothing)
            {
              inner.insert(d);
              visit(static_cast<VariableDecl*>(d)->init);
            }
          break;
        case NodeKind::ThisExpr: usesThis = true; break;
        case NodeKind::CallExpr: visit(static_cast<CallExpr*>(n)->callee); visitExprs(static_cast<CallExpr*>(n)->args); break;
        case NodeKind::IndexExpr: visit(static_cast<IndexExpr*>(n)->base); visitExprs(static_cast<IndexExpr*>(n)->args); break;
        case NodeKind::MemberExpr: visit(static_cast<MemberExpr*>(n)->base); visit(static_cast<MemberExpr*>(n)->dynamicMember); break;
        case NodeKind::UnaryExpr: visit(static_cast<UnaryExpr*>(n)->operand); break;
        case NodeKind::BinaryExpr: visit(static_cast<BinaryExpr*>(n)->lhs); visit(static_cast<BinaryExpr*>(n)->rhs); break;
        case NodeKind::ConditionalExpr:
          visit(static_cast<ConditionalExpr*>(n)->cond);
          visit(static_cast<ConditionalExpr*>(n)->then);
          visit(static_cast<ConditionalExpr*>(n)->else_);
          break;
        case NodeKind::CastExpr: visit(static_cast<CastExpr*>(n)->operand); break;
        case NodeKind::NamedCastExpr: visit(static_cast<NamedCastExpr*>(n)->operand); break;
        case NodeKind::FunctionalCastExpr: visitExprs(static_cast<FunctionalCastExpr*>(n)->args); break;
        case NodeKind::SizeofExpr: visit(static_cast<SizeofExpr*>(n)->operand); break;
        case NodeKind::NewExpr: visitExprs(static_cast<NewExpr*>(n)->placement); visitExprs(static_cast<NewExpr*>(n)->args); break;
        case NodeKind::DeleteExpr: visit(static_cast<DeleteExpr*>(n)->operand); break;
        case NodeKind::BraceInitExpr: visitExprs(static_cast<BraceInitExpr*>(n)->args); break;
        case NodeKind::ThrowExpr: visit(static_cast<ThrowExpr*>(n)->operand); break;
        case NodeKind::ParenExpr: visit(static_cast<ParenExpr*>(n)->inner); break;
        case NodeKind::NameofExpr: visit(static_cast<NameofExpr*>(n)->operand); break;
        case NodeKind::NoexceptExpr: break; // (its operand is not evaluated)
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(n);
          for (const LambdaCapture& c : l->captures)
          {
            visit(c.init);
            if (c.decl)
              inner.insert(c.decl);
          }
          for (Decl* p : l->params)
            inner.insert(p);
          visit(l->body);
          break;
        }
        case NodeKind::BlockStmt: for (Stmt* s : static_cast<BlockStmt*>(n)->stmts) visit(s); break;
        case NodeKind::ExprStmt: visit(static_cast<ExprStmt*>(n)->expr); break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(n);
          visit(i->init);
          if (i->ifConsteval)
          {
            visit(i->ifConsteval == 1 ? i->else_ : i->then); // `if consteval`: the program's branch alone
            break;
          }
          if (keptBranches && i->isConstexpr && i->constexprKnown >= 0)
          {
            // An instance's `if constexpr`: only the branch it keeps, as C++ captures what the
            // instantiated body uses (FluidBox::findOtherUndergroundEnd<BlueprintSurface>'s lambda
            // names `surface` only in the branch it discards, where nothing types it).
            visit(i->constexprKnown ? i->then : i->else_);
            break;
          }
          visit(i->cond);
          visit(i->then);
          visit(i->else_);
          break;
        }
        case NodeKind::ForStmt:
          visit(static_cast<ForStmt*>(n)->init);
          visit(static_cast<ForStmt*>(n)->cond);
          visit(static_cast<ForStmt*>(n)->step);
          visit(static_cast<ForStmt*>(n)->body);
          break;
        case NodeKind::RangeForStmt:
          visit(static_cast<RangeForStmt*>(n)->range);
          if (Decl* v = static_cast<RangeForStmt*>(n)->var)
            inner.insert(v);
          visit(static_cast<RangeForStmt*>(n)->body);
          break;
        case NodeKind::ForConstexprStmt: visit(static_cast<ForConstexprStmt*>(n)->body); break;
        case NodeKind::WhileStmt: visit(static_cast<WhileStmt*>(n)->cond); visit(static_cast<WhileStmt*>(n)->body); break;
        case NodeKind::DoStmt: visit(static_cast<DoStmt*>(n)->body); visit(static_cast<DoStmt*>(n)->cond); break;
        case NodeKind::SwitchStmt:
          visit(static_cast<SwitchStmt*>(n)->init);
          visit(static_cast<SwitchStmt*>(n)->cond);
          visit(static_cast<SwitchStmt*>(n)->body);
          break;
        case NodeKind::CaseStmt: visit(static_cast<CaseStmt*>(n)->value); break;
        case NodeKind::ReturnStmt: visit(static_cast<ReturnStmt*>(n)->value); break;
        case NodeKind::AssertStmt: visit(static_cast<AssertStmt*>(n)->cond); break;
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(n);
          visit(t->body);
          for (const TryStmt::Handler& h : t->handlers)
          {
            if (h.param)
              inner.insert(h.param);
            visit(h.body);
          }
          break;
        }
        default: break;
      }
    }

    // A lambda's captures from its AST alone, in a fixed order: its capture list, then what a
    // default capture picks up in the order first written, then `this`. The same list wherever the
    // closure is laid out or built, whichever function is emitted first (the body names the
    // instantiation's own locals: a capture's position, not its declaration, matches them up).
    struct CaptureSlot
    {
      Decl* decl = nullptr; // the captured variable (null for `this`)
      bool byRef = false;
      Expr* init = nullptr; // an init-capture's initializer
    };
    std::vector<CaptureSlot> captureList(LambdaExpr* l, std::unordered_map<Decl*, const Ty*>* useTypes = nullptr)
    {
      std::unordered_set<Decl*> inner;
      for (Decl* p : l->params)
        inner.insert(p);
      for (const LambdaCapture& cap : l->captures)
        if (cap.decl)
          inner.insert(cap.decl);
      std::vector<Decl*> used;
      bool usesThis = false;
      captureNames(l->body, inner, used, usesThis, useTypes, 0, l->templateParams.empty());
      std::vector<CaptureSlot> out;
      bool thisCaptured = false;
      for (const LambdaCapture& cap : l->captures)
      {
        if (cap.init)
        {
          // `[x = expr]`: a field of the closure, initialised from expr when the closure is made;
          // `[&x = expr]`: one pointing at what expr names.
          if (!cap.decl)
            unsupported(l, "an init-capture without its variable");
          out.push_back({cap.decl, cap.byRef, cap.init});
          continue;
        }
        if (cap.isThis)
        {
          thisCaptured = true;
          continue;
        }
        Decl* d = nullptr;
        for (Decl* u : used)
          if (localNamed(u, cap.symbol) || bindsName(u, cap.symbol))
            d = d ? d : u;
        if (!d)
          d = unusedCapture(l, cap.symbol);
        // `[mixingConnection]` of a structured binding: the object it names a part of, captured
        // the same way (the body reaches the part through it, as outside the lambda); once for
        // several of its bindings.
        if (std::none_of(out.begin(), out.end(), [&](const CaptureSlot& s) { return s.decl == d && !s.init; }))
          out.push_back({d, cap.byRef, nullptr});
      }
      if (l->defaultCapture != 0)
      {
        for (Decl* d : used)
          if (std::none_of(out.begin(), out.end(), [&](const CaptureSlot& s) { return s.decl == d; }))
            out.push_back({d, l->defaultCapture == 2, nullptr});
        thisCaptured = thisCaptured || usesThis;
      }
      if (thisCaptured)
        out.push_back({nullptr, true, nullptr});
      return out;
    }

    // Whether d is the hidden object of a structured binding declaration that names symbol.
    static bool bindsName(Decl* d, uint32_t symbol)
    {
      if (!d || d->kind != NodeKind::VariableDecl)
        return false;
      for (Node* b : static_cast<VariableDecl*>(d)->structuredBindings)
        if (b && b->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(b)->name.empty() && static_cast<NameExpr*>(b)->name.last().symbol == symbol)
          return true;
      return false;
    }

    // An explicit capture the body does not use (`[guard]` kept alive): the one local of that name
    // declared before the lambda in the function around it; with several, which one is in scope
    // the code generator cannot tell from the names alone.
    Decl* unusedCapture(LambdaExpr* l, uint32_t symbol)
    {
      Decl* f = l->closure ? l->closure->parent : nullptr;
      while (f && f->kind != NodeKind::FunctionDecl)
        f = f->parent;
      if (!f)
        unsupported(l, "a capture that is not a local variable (" + sym(symbol) + ")");
      auto* fn = static_cast<FunctionDecl*>(f);
      std::unordered_set<Decl*> declared(fn->params.begin(), fn->params.end());
      std::vector<Decl*> ignored;
      bool ignoredThis = false;
      captureNames(fn->body, declared, ignored, ignoredThis);
      Decl* only = nullptr;
      for (Decl* d : declared)
        if (localNamed(d, symbol) && d->token < l->token)
        {
          if (only)
            unsupported(l, "a capture of " + sym(symbol) + " that the lambda does not use, with several locals of that name in the function");
          only = d;
        }
      if (!only)
        unsupported(l, "a capture that is not a local variable (" + sym(symbol) + ")");
      return only;
    }

    // The type a captured variable has in the closure (references stripped): the variable's declared
    // type where the declaration alone gives it, an init-capture's as its initializer deduces it,
    // else as the lambda's body first names it (useTypes: an instantiation's local of a dependent
    // type, typed in place).
    static bool mentionsAuto(const Ty* t, int depth = 0)
    {
      if (!t || depth > 16)
        return false;
      if (t->kind == TyKind::Auto || mentionsAuto(t->inner, depth + 1))
        return true;
      for (const Ty* a : t->args)
        if (mentionsAuto(a, depth + 1))
          return true;
      return false;
    }

    const Ty* captureType(Decl* d, Expr* init, const std::unordered_map<Decl*, const Ty*>& useTypes, LambdaExpr* l)
    {
      auto exact = [&](const Ty* t) { return t && t->isKnown() && !types.isDependent(t) && !mentionsAuto(t); };
      if (const Ty* cached = d->cachedType.load(std::memory_order_acquire); exact(cached))
        return types.stripRef(cached); // as the instantiation's walk typed its own copy of the local
      const Ty* t = global.typeOfDecl(d);
      if (exact(t))
        return types.stripRef(t);
      if (init && init->ty && init->ty->isKnown() && !types.isDependent(init->ty))
        return types.decay(types.stripRef(init->ty)); // `[x = expr]`: as auto deduces it
      if (auto it = useTypes.find(d); it != useTypes.end() && it->second && it->second->isKnown() && !types.isDependent(it->second))
        return types.stripRef(it->second);
      unsupported(l, "a capture of " + (d->kind == NodeKind::VariableDecl ? sym(static_cast<VariableDecl*>(d)->name.last().symbol) : std::string("a parameter")) +
                         " whose type the code generator could not work out");
    }

    // The closure of a lambda type, created from its capture list in the current function.
    // The IR name of a lambda's closure struct: known as soon as the type is mentioned (a slot
    // for the variable holding it is allocated before its expression is reached); the struct is
    // defined once the captures are known.
    std::unordered_map<const Ty*, std::string> closureNames; // this emitter's view of the names below
    std::string closureStructName(const Ty* t)
    {
      t = types.unqualified(t); // `const auto f = [..]{..}`: the const variable holds the same closure
      if (auto mine = closureNames.find(t); mine != closureNames.end())
        return mine->second;
      auto lock = sharedLock();
      ClosureInfo& info = closures[t];
      if (info.structName.empty())
        info.structName = irTypeName("%closure.", t);
      closureNames.emplace(t, info.structName);
      return info.structName;
    }

    // The closure of a lambda type with its captures laid out, from the lambda's AST (captureList),
    // by whichever function needs it first: the function the lambda is written in, or one that
    // holds or calls a closure it returned (an instance of a template that returns a lambda is
    // emitted after its caller). The layout is the same either way.
    std::unordered_map<const Ty*, ClosureInfo*> closureCache; // laid-out closures this emitter used
    ClosureInfo& closureFor(const Ty* t, Node* at, bool = false)
    {
      t = types.unqualified(t);
      if (auto mine = closureCache.find(t); mine != closureCache.end())
        return *mine->second;
      if (ClosureInfo* known; sh.finalClosures.find(t, known))
        return *closureCache.emplace(t, known).first->second;
      auto lock = sharedLock(); // a laid-out closure does not change, so its reference may be read after
      auto it = closures.find(t);
      if (it != closures.end() && it->second.laidOut)
      {
        closureCache.emplace(t, &it->second);
        if (mayPublish())
          sh.finalClosures.publish(t, &it->second);
        return it->second;
      }
      if (it != closures.end() && !it->second.failure.empty())
        fail(at, "the closure of " + irString(t) + " could not be laid out (" + it->second.failure + ")");
      size_t errorsBefore = errors.size();
      try
      {
        return layOutClosure(t, at);
      }
      catch (Unsupported&)
      {
        // Every use, the first one like the others, reports the closure and why: the same lines
        // whichever function asks first.
        std::string why = lastFailure;
        closures[t].failure = why;
        ++sh.failuresRemembered;
        errors.resize(errorsBefore);
        fail(at, "the closure of " + irString(t) + " could not be laid out (" + why + ")");
      }
    }

    // closureFor's layout, from the lambda expression being emitted (at) or the closure's own.
    ClosureInfo& layOutClosure(const Ty* t, Node* at)
    {
      auto* c = static_cast<ClassDecl*>(t->decl);
      LambdaExpr* l = c->lambda; // the instantiation's own lambda: the same whoever lays it out
      if (!l)
        unsupported(at, "a lambda without its expression");
      // Laid out aside and published whole: a capture that fails (an unsupported one, a field whose
      // class cannot be laid out) leaves the closure unbuilt for every later use, not half built.
      ClosureInfo info;
      info.structName = closureStructName(t);
      std::unordered_map<Decl*, const Ty*> useTypes;
      for (const CaptureSlot& s : captureList(l, &useTypes))
      {
        info.captured.push_back(s.decl);
        info.byRef.push_back(s.byRef);
        info.inits.push_back(s.init);
        // `this`: a pointer field; the class it points to is the writer's (enclosingClass).
        info.fieldTypes.push_back(s.decl ? captureType(s.decl, s.init, useTypes, l) : types.pointer(types.voidType()));
      }
      std::string line = info.structName + " = type { ";
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
        line += (i ? ", " : "") + (info.byRef[i] ? std::string("ptr") : llType(info.fieldTypes[i], at));
      line += info.fieldTypes.empty() ? "i8 }\n" : " }\n";
      const Ty* ft = t;
      std::string params;
      for (size_t i = 0; i < ft->args.size(); ++i)
        params += (i ? ", " : "") + irString(ft->args[i]);
      info.functionName = "@\"" + irString(t) + "::operator()(" + params + ")\"";
      info.laidOut = true;
      decls += line;
      ClosureInfo& published = closures[t];
      published = std::move(info);
      return published;
    }

    // Lays out the closure of every lambda written in n (not in a lambda's body), before n is
    // emitted: a slot for a class holding one (`auto h = hold([base](int x) { ... })` and its
    // Holder<closure>, a call's result) is laid out before the expression that builds the closure
    // is reached. The captures are the locals in scope where the statement starts.
    void layOutClosuresIn(Node* n)
    {
      if (!n)
        return;
      switch (n->kind)
      {
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(n);
          for (const LambdaCapture& c : l->captures)
            layOutClosuresIn(c.init);
          if (isLambda(l->ty))
            closureFor(l->ty, l, true);
          return;
        }
        case NodeKind::CallExpr:
          layOutClosuresIn(static_cast<CallExpr*>(n)->callee);
          for (Expr* a : static_cast<CallExpr*>(n)->args)
            layOutClosuresIn(a);
          return;
        case NodeKind::MemberExpr: layOutClosuresIn(static_cast<MemberExpr*>(n)->base); return;
        case NodeKind::IndexExpr:
          layOutClosuresIn(static_cast<IndexExpr*>(n)->base);
          for (Expr* a : static_cast<IndexExpr*>(n)->args)
            layOutClosuresIn(a);
          return;
        case NodeKind::UnaryExpr: layOutClosuresIn(static_cast<UnaryExpr*>(n)->operand); return;
        case NodeKind::BinaryExpr:
          layOutClosuresIn(static_cast<BinaryExpr*>(n)->lhs);
          layOutClosuresIn(static_cast<BinaryExpr*>(n)->rhs);
          return;
        case NodeKind::ConditionalExpr:
          layOutClosuresIn(static_cast<ConditionalExpr*>(n)->cond);
          layOutClosuresIn(static_cast<ConditionalExpr*>(n)->then);
          layOutClosuresIn(static_cast<ConditionalExpr*>(n)->else_);
          return;
        case NodeKind::CastExpr: layOutClosuresIn(static_cast<CastExpr*>(n)->operand); return;
        case NodeKind::NamedCastExpr: layOutClosuresIn(static_cast<NamedCastExpr*>(n)->operand); return;
        case NodeKind::FunctionalCastExpr:
          for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
            layOutClosuresIn(a);
          return;
        case NodeKind::NewExpr:
          for (Expr* a : static_cast<NewExpr*>(n)->placement)
            layOutClosuresIn(a);
          for (Expr* a : static_cast<NewExpr*>(n)->args)
            layOutClosuresIn(a);
          return;
        case NodeKind::BraceInitExpr:
          for (Expr* a : static_cast<BraceInitExpr*>(n)->args)
            layOutClosuresIn(a);
          return;
        case NodeKind::ParenExpr: layOutClosuresIn(static_cast<ParenExpr*>(n)->inner); return;
        case NodeKind::ThrowExpr: layOutClosuresIn(static_cast<ThrowExpr*>(n)->operand); return;
        default: return;
      }
    }

    // A closure's captures by value, as (field index, type): what copying, moving and destroying
    // the closure copy, move and destroy one by one (a capture by reference, and `this`, is a pointer).
    std::vector<std::pair<uint32_t, const Ty*>> closureValueFields(const Ty* t, Node* at)
    {
      ClosureInfo& info = closureFor(t, at);
      std::vector<std::pair<uint32_t, const Ty*>> fields;
      for (size_t i = 0; i < info.captured.size(); ++i)
        if (info.captured[i] && !info.byRef[i])
          fields.push_back({uint32_t(i), info.fieldTypes[i]});
      return fields;
    }

    // Copies (move: moves) the closure at src into dst capture by capture, each through its own copy
    // or move constructor: EntityWithOwner's `[entity = Targeter<EntityWithOwner>(this)]` copied
    // bitwise into a load helper left the copy out of the entity's targeters and the stack's closure
    // in them, never destroyed.
    void copyClosure(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move)
    {
      ClosureInfo& info = closureFor(t, at);
      for (size_t i = 0; i < info.captured.size(); ++i)
      {
        std::string to = gep(info.structName, dst, uint32_t(i)), from = gep(info.structName, src, uint32_t(i));
        const Ty* ft = info.fieldTypes[i];
        bool byValue = info.captured[i] && !info.byRef[i];
        if (byValue && isClass(ft))
        {
          if (move)
            moveConstruct(to, from, ft, at);
          else
            copyConstruct(to, from, ft, at);
          continue;
        }
        if (byValue && isLambda(ft))
        {
          copyClosure(to, from, ft, at, move);
          continue;
        }
        if (byValue && ft->kind == TyKind::Array)
        {
          transferArray(to, from, ft, at, move, false);
          continue;
        }
        if (byValue)
        {
          copyBytes(to, from, ft, at); // a scalar
          continue;
        }
        std::string v = newTemp(); // a pointer: by reference, `this`
        fn += "  " + v + " = load ptr, ptr " + from + "\n";
        fn += "  store ptr " + v + ", ptr " + to + "\n";
      }
    }

    // Initialises the closure at addr from init, a closure value: a lambda expression is built in
    // place, a temporary moved from (and destroyed with the statement), anything else copied (moved
    // for `move(f)`).
    void initializeClosure(const std::string& addr, const Ty* t, Expr* init, Node* at)
    {
      Expr* inner = init;
      while (inner && inner->kind == NodeKind::ParenExpr)
        inner = static_cast<ParenExpr*>(inner)->inner;
      if (inner && inner->kind == NodeKind::LambdaExpr && types.unqualified(inner->ty) == types.unqualified(t))
      {
        makeClosure(static_cast<LambdaExpr*>(inner), addr);
        return;
      }
      Value v = rvalue(init);
      if (!isLambda(v.ty))
        unsupported(at, "initialising a lambda variable from something else");
      if (v.isTemporary && relocatesBitwise(t, at))
      {
        // Its bytes are the closure now: the statement no longer destroys the temporary.
        for (size_t i = statementTemps.size(); i-- > 0;)
          if (statementTemps[i].addr == v.text)
            statementTemps.erase(statementTemps.begin() + i);
        copyBytes(addr, v.text, t, at);
        return;
      }
      copyClosure(addr, v.text, t, at, v.isTemporary || movable(init));
    }

    // Builds the closure object of a lambda expression in a fresh slot (into: in that object) and
    // emits its function. One in a fresh slot is a temporary: the statement destroys it unless it
    // is moved out (a parameter by value, a variable), when its captures need destruction.
    Value makeClosure(LambdaExpr* l, const std::string& into = "")
    {
      const Ty* t = l->ty;
      if (!isLambda(t))
        unsupported(l, "a lambda whose type is unknown");
      ClosureInfo info = closureFor(t, l, true);
      {
        // The function the lambda is written in: its class is what `this` in the body points to,
        // its name the requester the operator's diagnostics give.
        auto lock = sharedLock();
        ClosureWriter& writer = sh.closureWriters[types.unqualified(t)];
        if (writer.writtenIn.empty())
        {
          writer.writtenIn = currentFunctionName;
          writer.writtenInContext = currentContext;
          writer.enclosingClass = currentClassTy;
        }
      }
      // This lambda's own captures, field by field (in a private copy of an instantiation they name
      // the copy's locals; the layout was read from the instantiation's).
      std::vector<CaptureSlot> slots = captureList(l);
      if (slots.size() != info.captured.size())
        unsupported(l, "a lambda whose captures differ from its closure's");
      std::string slot = into.empty() ? newObjectSlot(t, l) : into;
      for (size_t i = 0; i < info.captured.size(); ++i)
      {
        std::string field = gep(info.structName, slot, uint32_t(i));
        Decl* d = slots[i].decl;
        if (!d)
        {
          fn += "  store ptr " + thisPointer(l) + ", ptr " + field + "\n";
          continue;
        }
        if (slots[i].init && slots[i].byRef)
        {
          // `[&entities = this->entities]`: the field points at what the initializer names.
          Expr* init = slots[i].init;
          std::string target = isClass(types.stripRef(init->ty)) ? rvalue(init).text : lvalue(init);
          fn += "  store ptr " + target + ", ptr " + field + "\n";
          continue;
        }
        if (slots[i].init)
        {
          initializeFrom(field, info.fieldTypes[i], slots[i].init, l); // `[x = expr]`
          continue;
        }
        if (!locals.count(d))
          unsupported(l, "a capture of a variable the function being emitted does not have");
        Local& local = locals[d];
        std::string addr = local.addr;
        if (local.isRef)
        {
          addr = newTemp();
          fn += "  " + addr + " = load ptr, ptr " + local.addr + "\n";
        }
        if (info.byRef[i])
          fn += "  store ptr " + addr + ", ptr " + field + "\n";
        else if (isAggregate(info.fieldTypes[i]))
        {
          if (isClass(info.fieldTypes[i]))
            copyConstruct(field, addr, info.fieldTypes[i], l);
          else
            copyClosure(field, addr, info.fieldTypes[i], l, false); // `[f]` of a closure
        }
        else
          store(load(addr, info.fieldTypes[i], l), field, info.fieldTypes[i], l);
      }
      bool first = false; // this emitter emits the operator (a generic lambda's is emitted per call, once its types are known)
      {
        auto lock = sharedLock();
        if (!isGenericLambda(t))
          first = sh.closuresEmitted.insert(t).second;
      }
      if (first)
      {
        // (Several functions write one closure: a default member initializer's lambda each constructor,
        // a default argument's each call site: lambdaHome is the lambda's, whoever writes it.)
        NameHome home(*this, lambdaHome(l, info.functionName));
        emitLambda(l, t, types.function(t->inner ? t->inner : types.voidType(), t->args, false), info.functionName, nullptr);
      }
      if (!into.empty())
        return {slot, t, false};
      if (needsDestruction(t, l))
        statementTemps.push_back({slot, t});
      return {slot, t, true};
    }

    struct FunctionState
    {
      std::string fn, allocas, currentBlock, sretAddr;
      std::unordered_map<Decl*, Local> locals;
      std::unordered_map<uint32_t, std::string> labelBlocks; // a lambda emitted between a goto and its label keeps the label's block
      std::vector<Loop> loops;
      std::vector<std::vector<Object>> scopes;
      std::vector<Object> statementTemps;
      uint32_t temp, label;
      bool blockTerminated;
      FunctionDecl* currentFunction;
      const Ty* currentReturn;
      const Ty* currentClassTy;
      std::vector<EHFrame> ehFrames;
      std::string ehBlocks;
      std::unordered_map<std::string, std::string> cleanupPads;
      bool itaniumCleanup;
      bool usesEH;
      std::string returnLabel;
      std::unordered_map<std::string, std::string> inlineSlots;
      Node* debugAt;
      ParsedFile* debugFile;
      Node* bodyNow;
      std::vector<DebugVariable> debugVariables;
      std::vector<DebugBlock> debugBlocks;
      uint32_t debugBlock;
      VariableDecl* namedReturn;
      std::string keptOnReturn;
    };

    FunctionState saveState()
    {
      return {fn, allocas, currentBlock, sretAddr, locals, labelBlocks, loops, scopes, statementTemps, temp, label, blockTerminated,
              currentFunction, currentReturn, currentClassTy, ehFrames, ehBlocks, cleanupPads, itaniumCleanup, usesEH, returnLabel, inlineSlots, debugAt, debugFile,
              bodyNow, debugVariables, debugBlocks, debugBlock, namedReturn, keptOnReturn};
    }

    void restoreState(FunctionState& s)
    {
      fn = s.fn;
      allocas = s.allocas;
      currentBlock = s.currentBlock;
      sretAddr = s.sretAddr;
      locals = s.locals;
      labelBlocks = s.labelBlocks;
      loops = s.loops;
      scopes = s.scopes;
      statementTemps = s.statementTemps;
      temp = s.temp;
      label = s.label;
      blockTerminated = s.blockTerminated;
      currentFunction = s.currentFunction;
      currentReturn = s.currentReturn;
      currentClassTy = s.currentClassTy;
      ehFrames = s.ehFrames;
      ehBlocks = s.ehBlocks;
      cleanupPads = s.cleanupPads;
      itaniumCleanup = s.itaniumCleanup;
      usesEH = s.usesEH;
      returnLabel = s.returnLabel;
      inlineSlots = s.inlineSlots;
      debugAt = s.debugAt;
      debugFile = s.debugFile;
      bodyNow = s.bodyNow;
      debugVariables = s.debugVariables;
      debugBlocks = s.debugBlocks;
      debugBlock = s.debugBlock;
      namedReturn = s.namedReturn;
      keptOnReturn = s.keptOnReturn;
    }

    void resetFunctionState()
    {
      bodyNow = nullptr;
      namedReturn = nullptr;
      keptOnReturn.clear();
      returnLabel.clear();
      fn.clear();
      allocas.clear();
      locals.clear();
      labelBlocks.clear();
      loops.clear();
      scopes.clear();
      statementTemps.clear();
      ehFrames.clear();
      ehBlocks.clear();
      cleanupPads.clear();
      itaniumCleanup = false;
      usesEH = false;
      inlineSlots.clear();
      debugAt = nullptr;
      debugFile = nullptr;
      debugVariables.clear();
      debugBlocks.clear();
      debugBlock = 0;
      temp = 0;
      label = 0;
      blockTerminated = false;
      currentBlock = "entry";
      recursionDepth = 0;
    }

    // A function's definition text: the personality and the cleanup pads when it uses exceptions.
    std::string functionText(std::string header)
    {
      if (usesEH)
        header.insert(header.rfind("{\nentry:"), "personality ptr " + personality() + " ");
      if (size_t line = debugFile ? lineOf(debugAt, debugFile) : 0)
        return debugLines(header, line);
      return header + allocas + fn + ehBlocks + "}\n\n";
    }

    // --debug-info: a metadata id in a function's text. Each function numbers its own from 0 (its
    // DISubprogram), always ten digits wide, and module assembly (renumberDebugIds) makes them the
    // module's, in place: the text does not depend on where the function lands.
    static std::string debugId(uint32_t local)
    {
      char digits[16];
      snprintf(digits, sizeof digits, "!%010u", local);
      return digits;
    }

    std::unordered_map<const ParsedFile*, std::string> debugFiles; // a file's DIFile, its absolute path
    std::unordered_map<const ParsedFile*, std::string> programPaths; // a file's path as the program's text holds it

    // The path an assertion's message and a CallerLocation give for a file: --file-prefix-map's
    // (programPath), the debug info keeping the path itself (debugFileOf).
    const std::string& programPathOf(const ParsedFile* file)
    {
      auto [it, added] = programPaths.try_emplace(file);
      if (added)
        it->second = programPath(file->path);
      return it->second;
    }

    const std::string& debugFileOf(const ParsedFile* file)
    {
      std::string& text = debugFiles[file];
      if (text.empty())
      {
        std::error_code ec;
        std::string path = std::filesystem::absolute(file->path, ec).string();
        if (ec)
          path = file->path;
        std::string escaped;
        for (char c : path)
          if (c == '\\' || c == '"' || (unsigned char)c < 0x20)
          {
            char hex[4];
            snprintf(hex, sizeof hex, "\\%02X", (unsigned char)c);
            escaped += hex;
          }
          else
            escaped += c;
        text = "!DIFile(filename: \"" + escaped + "\", directory: \"\")";
      }
      return text;
    }

    // A metadata string's text: `\` and `"` and control characters as \XX.
    static std::string metadataString(std::string_view s)
    {
      std::string out = "\"";
      for (char c : s)
        if (c == '\\' || c == '"' || (unsigned char)c < 0x20)
        {
          char hex[4];
          snprintf(hex, sizeof hex, "\\%02X", (unsigned char)c);
          out += hex;
        }
        else
          out += c;
      return out + "\"";
    }

    // --debug-info: the placeholder that names a type's metadata in a definition's text (`!T` and the
    // key's 16 hex digits, as wide as module assembly's id and its padding: appendDebugTypes).
    static std::string debugTypePlaceholder(uint64_t key)
    {
      char text[24];
      snprintf(text, sizeof text, "!T%016llx", (unsigned long long)key);
      return text;
    }

    // --debug-info with variables: the placeholder of type t's metadata, made the first time any
    // emitter names the type (Shared::debugTypes). Its key is its IR spelling's hash (irString tells
    // every type apart: a class's path, its template's arguments, where a local class is declared).
    std::string debugType(const Ty* t)
    {
      std::string spelling = irString(t);
      uint64_t key = 14695981039346656037ull;
      for (unsigned char c : spelling)
        key = (key ^ c) * 1099511628211ull;
      std::string placeholder = debugTypePlaceholder(key);
      {
        auto lock = sharedLock();
        auto [it, isNew] = sh.debugTypes.try_emplace(key);
        if (!isNew && it->second.generation == sh.debugGeneration)
          return placeholder; // (made, or being made: a type that names itself goes through a pointer)
        it->second.generation = sh.debugGeneration;
      }
      std::string text = debugTypeText(t, spelling);
      bool isClass = (t->kind == TyKind::Class || t->kind == TyKind::Lambda) && !t->isConst && !t->isVolatile;
      auto lock = sharedLock();
      Shared::DebugTypeText& made = sh.debugTypes[key];
      made.text = std::move(text);
      made.type = isClass ? t : nullptr;
      return placeholder;
    }

    // Type t's metadata: node 0 the type, the nodes it needs after it, other types by placeholder.
    // Basic types as CodeView names them (its simple types have no names of their own: int32 is
    // `int`), each of kpp's integers a typedef of its name too (int32, uint8: an S_UDT, so that a
    // natvis rule's $T1 parses). A class or enum is named as kppc's IR names it (game::Entity,
    // Vector<int32>), the whole spelling, its closing brackets apart (debuggerTypeName), and
    // identified by it.
    std::string debugTypeText(const Ty* t, const std::string& spelling)
    {
      auto basic = [](const char* name, uint64_t bits, const char* encoding)
      { return std::string("!DIBasicType(name: \"") + name + "\", size: " + std::to_string(bits) + ", encoding: " + encoding + ")"; };
      auto one = [](const std::string& node) { return debugId(0) + " = " + node + "\n"; };
      // (`T* const`'s qualifier is the pointer's: its unqualified type is the pointer.)
      if (const Ty* bare = types.unqualified(t); bare != t)
        return one(std::string("!DIDerivedType(tag: ") + (t->isConst ? "DW_TAG_const_type" : "DW_TAG_volatile_type") + ", baseType: " + debugType(bare) + ")");
      switch (t->kind)
      {
        case TyKind::Bool: return one(basic("bool", 8, "DW_ATE_boolean"));
        case TyKind::Char:
          if (t->bits == 8)
            return one(t->isUnsigned ? basic("unsigned char", 8, "DW_ATE_unsigned_char") : basic("char", 8, "DW_ATE_signed_char"));
          return one(basic(t->bits == 16 ? "char16_t" : "char32_t", t->bits, "DW_ATE_UTF"));
        case TyKind::Float: return one(basic(t->bits == 32 ? "float" : "double", t->bits, "DW_ATE_float"));
        case TyKind::Nullptr: return one("!DIBasicType(tag: DW_TAG_unspecified_type, name: \"decltype(nullptr)\")");
        case TyKind::Int:
        {
          if (t->isLong())
            return one(basic(t->isUnsigned ? "unsigned long" : "long", t->bits, t->isUnsigned ? "DW_ATE_unsigned" : "DW_ATE_signed"));
          static const char* const names[2][4] = {{"signed char", "short", "int", "long long"}, {"unsigned char", "unsigned short", "unsigned int", "unsigned long long"}};
          int size = t->bits == 8 ? 0 : t->bits == 16 ? 1 : t->bits == 32 ? 2 : 3;
          const char* encoding = t->isUnsigned ? (size == 0 ? "DW_ATE_unsigned_char" : "DW_ATE_unsigned") : (size == 0 ? "DW_ATE_signed_char" : "DW_ATE_signed");
          return debugId(0) + " = !DIDerivedType(tag: DW_TAG_typedef, name: " + metadataString(spelling) + ", baseType: " + debugId(1) + ")\n" + debugId(1) + " = " +
                 basic(names[t->isUnsigned][size], t->bits, encoding) + "\n";
        }
        case TyKind::Pointer:
          return one("!DIDerivedType(tag: DW_TAG_pointer_type, baseType: " + (types.unqualified(t->inner)->kind == TyKind::Void ? std::string("null") : debugType(t->inner)) +
                     ", size: 64)");
        case TyKind::LRef:
        case TyKind::RRef:
          return one(std::string("!DIDerivedType(tag: ") + (t->kind == TyKind::LRef ? "DW_TAG_reference_type" : "DW_TAG_rvalue_reference_type") +
                     ", baseType: " + debugType(t->inner) + ", size: 64)");
        case TyKind::Array:
          if (t->count)
            return one("!DICompositeType(tag: DW_TAG_array_type, baseType: " + debugType(t->inner) + ", size: " + std::to_string(sizeOf(t) * 8) +
                       ", elements: " + debugId(1) + ")") +
                   debugId(1) + " = !{ " + debugId(2) + "}\n" + debugId(2) + " = !DISubrange(count: " + std::to_string(t->count) + ")\n";
          break;
        case TyKind::Enum:
          if (t->decl && t->decl->kind == NodeKind::EnumDecl)
          {
            auto* e = static_cast<EnumDecl*>(t->decl);
            const Ty* underlying = enumUnderlying(e);
            std::string elements, enumerators;
            uint32_t next = 2;
            for (Decl* d : e->enumerators)
            {
              if (d->kind != NodeKind::EnumeratorDecl)
                continue;
              auto* en = static_cast<EnumeratorDecl*>(d);
              elements += (elements.empty() ? "" : ", ") + debugId(next);
              // An unsigned enumeration's value as its type holds it (`Top = ~0ull`, `None = -1`: LLVM
              // rejects an unsigned enumerator with a negative value).
              int64_t value = enumeratorValue(en);
              std::string valueText = std::to_string(value);
              if (underlying && underlying->isUnsigned)
              {
                uint64_t bits = uint64_t(value);
                if (underlying->bits && underlying->bits < 64)
                  bits &= (uint64_t(1) << underlying->bits) - 1;
                valueText = std::to_string(bits);
              }
              enumerators += debugId(next++) + " = !DIEnumerator(name: " + metadataString(sym(en->name)) + ", value: " + valueText +
                             (underlying && underlying->isUnsigned ? ", isUnsigned: true" : "") + ")\n";
            }
            return one("!DICompositeType(tag: DW_TAG_enumeration_type, name: " + metadataString(debuggerTypeName(spelling)) +
                       (underlying ? ", baseType: " + debugType(underlying) : std::string()) + ", size: " + std::to_string(sizeOf(t) * 8) +
                       (e->isClass ? ", flags: DIFlagEnumClass" : "") + ", elements: " + debugId(1) + ", identifier: " + metadataString(spelling) + ")") +
                   debugId(1) + " = !{ " + elements + "}\n" + enumerators; // (`!{ `: renumberDebugIds takes an id after a blank)
          }
          break;
        case TyKind::Class:
        case TyKind::Lambda:
        {
          // Its declaration, with the definition's tag (debugComposite): the definition is its home's
          // alone (homeDebugClasses), every other module's reference resolved by the identifier.
          auto* cd = t->decl && t->decl->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(t->decl) : nullptr;
          const char* tag = cd && cd->keyword == Kw::union_ ? "DW_TAG_union_type" : cd && cd->keyword == Kw::class_ ? "DW_TAG_class_type" : "DW_TAG_structure_type";
          return one(std::string("!DICompositeType(tag: ") + tag + ", name: " + metadataString(debuggerClassName(types, t, symbols)) + ", flags: DIFlagFwdDecl, identifier: " +
                     metadataString(spelling) + ")");
        }
        case TyKind::Function:
          return one("!DISubroutineType(types: !{})");
        default:
          break;
      }
      // What a debugger has no type for (a pointer to a member): an unsigned integer of its size.
      return one(basic("unsigned long long", 64, "DW_ATE_unsigned"));
    }

    // --debug-info with variables (Shared::fullDebugInfo): a local or parameter (arg: its number from
    // 1) held at address (its alloca, or the pointer a parameter passed by address is), whose value is
    // of type t (a reference's own: its slot holds the pointer), in the current block. Not one of a
    // body inlined into its caller (its instructions are the call's).
    void debugVariable(std::string name, Node* at, const std::string& address, const Ty* t, uint32_t arg = 0, bool artificial = false, bool defined = false)
    {
      if (!sh.fullDebugInfo || !debugFile || debugFile != currentFile || !inlining.empty() || !t || !t->isKnown() || types.isDependent(t))
        return;
      debugVariables.push_back({address, std::move(name), debugType(t), at ? lineOf(at, currentFile) : 0, arg ? 0 : debugBlock, arg, artificial, defined});
    }

    // --debug-info with variables: the attachment of global `name`'s definition line (`, !dbg ` and a
    // placeholder, `!G` and the hash of its IR name), its metadata made here into Shared::debugGlobals:
    // a DIGlobalVariableExpression of a DIGlobalVariable named as the IR names it (game::counter,
    // Vector<int32>::empty: a static member's storage is Class::member, where the debugger reads its
    // value; its class's closing brackets apart, debuggerTypeName), at its definition's line, of
    // type t. Module assembly gives it to the module whose text defines the global
    // (appendDebugTypes), its home in a debug build; a declaration has none
    // (externalDeclarationOf).
    std::string debugGlobal(const std::string& name, VariableDecl* def, const Ty* t)
    {
      if (!sh.fullDebugInfo || !def || !def->file || !t || !t->isKnown() || types.isDependent(t))
        return std::string();
      std::string bare = name.rfind("@\"", 0) == 0 ? name.substr(2, name.size() - 3) : name.substr(1);
      uint64_t key = 14695981039346656037ull;
      for (unsigned char c : bare)
        key = (key ^ c) * 1099511628211ull;
      char placeholder[24];
      snprintf(placeholder, sizeof placeholder, "!G%016llx", (unsigned long long)key);
      std::string text = debugId(0) + " = !DIGlobalVariableExpression(var: " + debugId(1) + ", expr: !DIExpression())\n" + debugId(1) +
                         " = distinct !DIGlobalVariable(name: " + metadataString(debuggerTypeName(bare)) + ", scope: !0, file: " + debugFileOf(def->file) +
                         ", line: " + std::to_string(lineOf(def, def->file)) + ", type: " + debugType(t) + ", isLocal: false, isDefinition: true)\n";
      auto lock = sharedLock();
      sh.debugGlobals[key] = std::move(text);
      return std::string(", !dbg ") + placeholder;
    }

    // A variable's or parameter's name for the debugger, or "" for one without (a structured binding's).
    std::string debugName(Decl* d)
    {
      if (d && d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
        return sym(static_cast<VariableDecl*>(d)->name.last().symbol);
      if (d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->name != UINT32_MAX)
        return sym(static_cast<ParamDecl*>(d)->name);
      return std::string();
    }

    // Class ct's layout as codegen made it, or null for one never laid out (a class the program only
    // points to): the debugger gets its declaration alone.
    const StructInfo* laidOutClass(const Ty* ct)
    {
      auto lock = sharedLock();
      const StructInfo* info = nullptr;
      if (auto it = sh.structsByType.find(ct); it != sh.structsByType.end())
        info = it->second;
      else if (auto s = sh.structs.find(ct); s != sh.structs.end())
        info = &s->second;
      return info && info->complete && !info->failed ? info : nullptr;
    }

    // --debug-info with variables: the composite node `self` of class ct as laid out (info), its
    // members' nodes appended to nodes with ids from next on: its vtable pointer (a class with its
    // own, at offset 0), its bases, its data members at their offsets (a bit field at its bits, an
    // anonymous struct's or union's in an unnamed member of that class's own composite, made here:
    // LLVM's CodeView writer lists them as the enclosing class's, as MSVC's PDBs do). Named (its
    // spelling, which identifies it) or, for an anonymous member's class, not.
    void debugComposite(const Ty* ct, const StructInfo& info, uint32_t self, const std::string* spelling, std::string& nodes, uint32_t& next, int depth = 0)
    {
      auto* c = static_cast<ClassDecl*>(ct->decl);
      TemplateArgs args = global.classArgsOf(ct);
      // Each field's offset, as memberOffset computes it (an empty base's: no storage).
      std::vector<uint64_t> offsets(info.fieldTypes.size(), 0);
      uint64_t at = 0;
      for (size_t i = 0; i < info.fieldTypes.size() && !info.isUnion; ++i)
      {
        if (i < info.zeroSize.size() && info.zeroSize[i])
        {
          offsets[i] = at;
          continue;
        }
        uint64_t a = info.packed ? 1 : alignOf(info.fieldTypes[i], nullptr);
        at = (at + a - 1) / a * a;
        offsets[i] = at;
        at += sizeOf(info.fieldTypes[i], nullptr);
      }
      std::string elements;
      auto element = [&](uint32_t id) { elements += (elements.empty() ? "" : ", ") + debugId(id); };
      // A member's access as written (`public:` in a class, `private:` in a struct); none for the
      // default, which the tag gives (a class's members private, a struct's public).
      auto access = [](Access a) -> std::string
      {
        return a == Access::Public ? "DIFlagPublic" : a == Access::Protected ? "DIFlagProtected" : a == Access::Private ? "DIFlagPrivate" : "";
      };
      auto flagsOf = [](std::initializer_list<std::string> flags)
      {
        std::string text;
        for (const std::string& f : flags)
          if (!f.empty())
            text += (text.empty() ? ", flags: " : " | ") + f;
        return text;
      };
      auto member = [&](const std::string& name, const std::string& type, uint64_t bits, uint64_t offset, const std::string& flags, const std::string& extra = "")
      {
        uint32_t id = next++;
        nodes += debugId(id) + " = !DIDerivedType(tag: DW_TAG_member" + (name.empty() ? std::string() : ", name: " + metadataString(name)) +
                 ", scope: " + debugId(self) + ", baseType: " + type + ", size: " + std::to_string(bits) + ", offset: " + std::to_string(offset) + flags + extra + ")\n";
        element(id);
      };
      if (info.polymorphic && !info.primaryBase && !info.isUnion && !info.fieldTypes.empty())
      {
        // Its own vtable pointer: a pointer to the vtable's shape (CodeView's LF_VTSHAPE, one entry a slot).
        uint64_t slots = info.vtables.empty() ? 1 : std::max<uint64_t>(1, info.vtables[0].slots.size());
        uint32_t shape = next++, pointer = next++;
        nodes += debugId(shape) + " = !DIDerivedType(tag: DW_TAG_pointer_type, name: \"__vtbl_ptr_type\", baseType: null, size: " + std::to_string(slots * 64) + ")\n";
        nodes += debugId(pointer) + " = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: " + debugId(shape) + ", size: 64)\n";
        member("_vptr$" + (c->name.empty() ? std::string("?") : sym(c->name.last().symbol)), debugId(pointer), 64, 0, flagsOf({"DIFlagArtificial"}));
      }
      // The bases in the order written, their access too where the layout's are the written ones.
      for (size_t b = 0; b < info.bases.size(); ++b)
        if (uint32_t index = info.bases[b].second; index < info.fieldTypes.size())
        {
          uint32_t id = next++;
          nodes += debugId(id) + " = !DIDerivedType(tag: DW_TAG_inheritance, scope: " + debugId(self) + ", baseType: " + debugType(info.fieldTypes[index]) +
                   ", offset: " + std::to_string(offsets[index] * 8) + flagsOf({info.bases.size() == c->bases.size() ? access(c->bases[b].access) : ""}) +
                   ", extraData: i32 0)\n";
          element(id);
        }
      for (Decl* m : info.fieldDecls)
      {
        auto found = info.fieldIndex.find(m);
        if (found == info.fieldIndex.end())
          continue;
        uint32_t index = found->second;
        uint64_t offset = info.isUnion || index >= offsets.size() ? 0 : offsets[index];
        if (m->kind == NodeKind::ClassDecl)
        {
          // An anonymous struct or union: its own composite here, with no name, an unnamed member.
          const Ty* inner = types.classType(m, ct->args);
          const StructInfo* innerInfo = depth < 8 ? laidOutClass(inner) : nullptr;
          if (!innerInfo)
            continue;
          uint32_t composite = next++;
          debugComposite(inner, *innerInfo, composite, nullptr, nodes, next, depth + 1);
          member("", debugId(composite), sizeOf(inner, nullptr) * 8, offset * 8, flagsOf({access(m->access)}));
          continue;
        }
        auto* v = static_cast<VariableDecl*>(m);
        std::string name = v->name.empty() ? std::string() : sym(v->name.last().symbol);
        if (auto bits = info.bitfields.find(m); bits != info.bitfields.end())
        {
          const Ty* declared = global.typeOfDeclIn(m, args);
          if (declared && declared->isKnown() && !types.isDependent(declared))
            member(name, debugType(declared), bits->second.second, offset * 8 + bits->second.first, flagsOf({"DIFlagBitField", access(m->access)}),
                   ", extraData: i64 " + std::to_string(offset * 8));
          continue;
        }
        const Ty* mt = info.isUnion ? global.typeOfDeclIn(m, args) : info.fieldTypes[index];
        if (!mt || !mt->isKnown() || types.isDependent(mt))
          continue;
        member(name, debugType(mt), sizeOf(mt, nullptr) * 8, offset * 8, flagsOf({access(m->access)}));
      }
      // Its static data members (CodeView's LF_STMEMBER): their storage is a global of the program,
      // named Class::member (debugGlobal), where the debugger reads their values.
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::VariableDecl || !(m->flags & DF_Static) || (m->flags & (DF_Typedef | DF_Friend)) || m->builtin)
          continue;
        auto* v = static_cast<VariableDecl*>(m);
        const Ty* st = v->name.empty() ? nullptr : global.typeOfDeclIn(m, args);
        if (!st || !st->isKnown() || types.isDependent(st))
          continue;
        uint32_t id = next++;
        nodes += debugId(id) + " = !DIDerivedType(tag: DW_TAG_member, name: " + metadataString(sym(v->name.last().symbol)) + ", scope: " + debugId(self) +
                 ", baseType: " + debugType(st) + flagsOf({"DIFlagStaticMember", access(m->access)}) + ")\n";
        element(id);
      }
      std::string tag = info.isUnion || c->keyword == Kw::union_ ? "DW_TAG_union_type" : c->keyword == Kw::class_ ? "DW_TAG_class_type" : "DW_TAG_structure_type";
      uint32_t list = next++;
      nodes += debugId(list) + " = !{ " + elements + "}\n";
      nodes += debugId(self) + " = distinct !DICompositeType(tag: " + tag + (spelling ? ", name: " + metadataString(debuggerClassName(types, ct, symbols)) : std::string()) +
               ", size: " + std::to_string(sizeOf(ct, nullptr) * 8) + ", elements: " + debugId(list) +
               (spelling && info.polymorphic && !info.primaryBase ? ", vtableHolder: " + debugId(self) : std::string()) +
               (spelling ? ", identifier: " + metadataString(*spelling) : std::string()) + ")\n";
    }

    // --debug-info with variables, once the emission is done (prepareModuleTexts; a build of one
    // module before it assembles its text): every class a definition or a type named (each one
    // Shared::debugTypes has: a debug build's program pieces are assembled, and their texts freed,
    // before this), and the classes their definitions name, each defined (debugComposite) for its
    // home module alone (Shared::debugHomes, the key's remainder by the module count: where it lands
    // does not depend on who named it first); every other module declares it. A compile server's
    // build makes the definitions of the classes it named itself (their types are its own) and keeps
    // the others'.
    void homeDebugClasses()
    {
      std::set<uint64_t> done;
      for (bool more = true; more;)
      {
        more = false;
        std::vector<std::pair<uint64_t, Shared::DebugTypeText*>> entries;
        {
          auto lock = sharedLock();
          for (auto& [key, entry] : sh.debugTypes)
            if (!done.count(key))
              entries.push_back({key, &entry}); // (an element stays where it is as others are added)
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& [key, entry] : entries)
        {
          done.insert(key);
          more = true;
          if (!entry->type || entry->generation != sh.debugGeneration || entry->definitionGeneration == sh.debugGeneration)
            continue;
          entry->definitionGeneration = sh.debugGeneration;
          std::string definition;
          if (const StructInfo* info = laidOutClass(entry->type))
          {
            std::string spelling = irString(entry->type);
            uint32_t next = 1;
            debugComposite(entry->type, *info, 0, &spelling, definition, next); // (the types its members name join debugTypes)
          }
          // (A compile server's build, rebuildLLVM's generation, assembles the modules it changed: a
          // class's new definition is its home's change too.)
          if (sh.debugGeneration > 0 && definition != entry->definition && sh.moduleCount)
          {
            sh.unassembled.resize(sh.moduleCount);
            sh.unassembled[key % sh.moduleCount] = true;
          }
          entry->definition = std::move(definition);
        }
      }
      sh.debugHomes.assign(std::max(1u, sh.moduleCount), {});
      for (uint64_t key : done)
        if (auto it = sh.debugTypes.find(key); it != sh.debugTypes.end() && !it->second.definition.empty())
          sh.debugHomes[key % sh.debugHomes.size()].push_back(key);
    }

    // The definition with line information: every instruction gets the line of the statement whose
    // marker (`  ;@LINE`, from statement()) precedes it, those before the first the declaration's
    // line; then the function's DISubprogram and DILocations (one per line). A switch's case lines
    // belong to the instruction they end with (`  ]`). With variables (Shared::fullDebugInfo) a marker
    // names its lexical block too (`  ;@LINE:BLOCK`, a location per line and block), and after the
    // allocas a #dbg_declare gives each variable its address. Ids: 0 the DISubprogram; with blocks or
    // variables 1 the file's node, then the blocks (DILexicalBlock), then the variables
    // (DILocalVariable, their types by placeholder); then the locations.
    std::string debugLines(std::string header, size_t declLine)
    {
      size_t name = header.find('@');
      size_t nameEnd = header[name + 1] == '"' ? header.find('"', name + 2) + 1 : header.find('(', name);
      std::string spName = header[name + 1] == '"' ? header.substr(name + 2, nameEnd - name - 3) : header.substr(name + 1, nameEnd - name - 1);
      header.insert(header.rfind("{\nentry:"), "!dbg " + debugId(0) + " ");
      std::string out = std::move(header);
      out.reserve(out.size() + (allocas.size() + fn.size() + ehBlocks.size()) * 5 / 4);
      bool variables = !debugBlocks.empty() || !debugVariables.empty();
      const uint32_t fileId = 1, blocksBase = 1, variablesBase = 2 + uint32_t(debugBlocks.size());
      const uint32_t firstLocation = variables ? variablesBase + uint32_t(debugVariables.size()) : 1;
      auto scope = [&](uint32_t block) { return debugId(block ? blocksBase + block : 0); };
      std::vector<std::pair<size_t, uint32_t>> locations; // id - firstLocation -> its line and block
      std::map<std::pair<size_t, uint32_t>, uint32_t> ids;
      std::pair<size_t, uint32_t> current{declLine, 0};
      std::string currentRef;
      auto idFor = [&](std::pair<size_t, uint32_t> location)
      {
        auto [it, isNew] = ids.emplace(location, firstLocation + uint32_t(ids.size()));
        if (isNew)
          locations.push_back(location);
        return it->second;
      };
      currentRef = ", !dbg " + debugId(idFor(current));
      // A variable's declaration, located at its own line and in its block: after the allocas, or for
      // one whose address an instruction of the body computes (a capture's field), before the
      // instruction after that one (a record goes before an instruction).
      auto declare = [&](size_t i)
      {
        const DebugVariable& v = debugVariables[i];
        return "    #dbg_declare(ptr " + v.address + ", " + debugId(variablesBase + uint32_t(i)) + ", !DIExpression(), " +
               debugId(idFor({v.line ? v.line : declLine, v.block})) + ")\n";
      };
      std::vector<size_t> defined, pending;
      for (size_t i = 0; i < debugVariables.size(); ++i)
        if (debugVariables[i].defined)
          defined.push_back(i);
      for (const std::string* part : {&allocas, &fn, &ehBlocks})
      {
        std::string_view text = *part;
        for (size_t pos = 0; pos < text.size();)
        {
          size_t end = text.find('\n', pos);
          if (end == std::string_view::npos)
            end = text.size();
          std::string_view l = text.substr(pos, end - pos);
          pos = end + 1;
          if (l.starts_with("  ;@"))
          {
            std::string marker(l.substr(4));
            char* rest = nullptr;
            std::pair<size_t, uint32_t> location{strtoull(marker.c_str(), &rest, 10), 0};
            if (rest && *rest == ':')
              location.second = uint32_t(strtoul(rest + 1, nullptr, 10));
            if (location != current)
            {
              current = location;
              currentRef = ", !dbg " + debugId(idFor(location));
            }
            continue;
          }
          bool instruction = l.starts_with("  ") && !l.starts_with("    ") && !l.starts_with("  ;");
          if (instruction && !pending.empty())
          {
            for (size_t i : pending)
              out += declare(i);
            pending.clear();
          }
          out.append(l);
          if (instruction && !l.ends_with("["))
            out += currentRef;
          out += '\n';
          if (instruction)
            for (size_t i : defined)
              if (l.size() > debugVariables[i].address.size() + 5 && l.substr(2, debugVariables[i].address.size()) == debugVariables[i].address &&
                  l.substr(2 + debugVariables[i].address.size(), 3) == " = ")
                pending.push_back(i);
        }
        if (part == &allocas)
          for (size_t i = 0; i < debugVariables.size(); ++i)
            if (!debugVariables[i].defined)
              out += declare(i);
      }
      out += "}\n";
      const std::string& file = debugFileOf(debugFile);
      out += debugId(0) + " = distinct !DISubprogram(name: \"" + spName + "\", scope: " + file + ", file: " + file + ", line: " +
             std::to_string(declLine) + ", type: !2, scopeLine: " + std::to_string(declLine) + ", spFlags: DISPFlagDefinition, unit: !0)\n";
      if (variables)
      {
        out += debugId(fileId) + " = " + file + "\n";
        for (uint32_t b = 1; b <= debugBlocks.size(); ++b)
          out += debugId(blocksBase + b) + " = distinct !DILexicalBlock(scope: " + scope(debugBlocks[b - 1].parent) + ", file: " + debugId(fileId) +
                 ", line: " + std::to_string(debugBlocks[b - 1].line ? debugBlocks[b - 1].line : declLine) + ")\n";
        for (size_t i = 0; i < debugVariables.size(); ++i)
        {
          const DebugVariable& v = debugVariables[i];
          out += debugId(variablesBase + uint32_t(i)) + " = !DILocalVariable(name: " + metadataString(v.name) +
                 (v.arg ? ", arg: " + std::to_string(v.arg) : std::string()) + ", scope: " + scope(v.block) + ", file: " + debugId(fileId) +
                 ", line: " + std::to_string(v.line ? v.line : declLine) + ", type: " + v.type +
                 (v.artificial ? ", flags: DIFlagArtificial | DIFlagObjectPointer" : "") + ")\n";
        }
      }
      for (size_t k = 0; k < locations.size(); ++k)
        out += debugId(firstLocation + uint32_t(k)) + " = !DILocation(line: " + std::to_string(locations[k].first) + ", scope: " + scope(locations[k].second) + ")\n";
      out += '\n';
      return out;
    }

    // Emits the lambda's function while another function is being emitted: the enclosing
    // function's state is saved and restored around it. callTy: the operator's type (parameters
    // and result); name: its IR name; args: for an instantiation of a generic lambda, the
    // enclosing instantiation's arguments plus its own, which declarations in the body are typed under.
    // The function a lambda is written in, as far as it is known yet (Shared::closureWriters).
    ClosureWriter closureWriterOf(const Ty* t)
    {
      auto lock = sharedLock();
      auto it = sh.closureWriters.find(types.unqualified(t));
      return it == sh.closureWriters.end() ? ClosureWriter{} : it->second;
    }

    void emitLambda(LambdaExpr* l, const Ty* t, const Ty* callTy, const std::string& name, const TemplateArgs* args)
    {
      ClosureInfo info = closureFor(t, l);
      ClosureWriter writer = closureWriterOf(t);
      FunctionState saved = saveState();
      const TemplateArgs* savedArgs = currentArgs;
      ParsedFile* savedFile = currentFile;
      std::string savedName = currentFunctionName;
      struct NameRestore { std::string& name; std::string saved; ~NameRestore() { name = saved; } } nameRestore{currentFunctionName, savedName};
      currentFunctionName = name; // what the operator's own statics and filters are named after
      // Its diagnostics name the function it is written in as the requester, not the function that
      // happened to emit it first.
      NameRestore requesterRestore{currentRequestedBy, currentRequestedBy};
      currentRequestedBy = writer.writtenIn;
      // As an error says it: the function it is written in and its line, which a lambda's closure
      // tag would not tell a reader (A, 2026-11-04).
      NameRestore contextRestore1{currentContext, currentContext};
      NameRestore contextRestore2{currentRequestedByContext, currentRequestedByContext};
      {
        size_t line = lineOf(l, l->closure && l->closure->file ? l->closure->file : currentFile);
        currentContext = "a lambda" + (writer.writtenInContext.empty() ? std::string() : " in " + writer.writtenInContext) +
                         (line ? ", line " + std::to_string(line) : std::string());
        currentRequestedByContext.clear();
      }
      struct ContextRestore
      {
        Decl*& lambda;
        Decl*& initializer;
        Decl* savedLambda;
        Decl* savedInitializer;
        ~ContextRestore()
        {
          lambda = savedLambda;
          initializer = savedInitializer;
        }
      } contextRestore{lambdaContext, initializerOf, lambdaContext, initializerOf};
      // One written in a function body (whose declarations the closure's context reaches); one in
      // a constructor's initializer list or a member's initializer is typed as before, from the
      // function or the initializer being emitted.
      if (l->closure && l->closure->parent && l->closure->parent->kind == NodeKind::FunctionDecl)
      {
        lambdaContext = l->closure;
        initializerOf = nullptr;
      }
      resetFunctionState();
      bodyNow = l->body;
      // The function it is written in is the current function while its body is emitted (not the
      // one that happened to emit it first, a generic lambda's first caller): its operators and free
      // functions are looked up from there, `this` is that function's, and its nodes are that
      // function's to retype (a pack expansion, a for constexpr). Restored with the saved state.
      if (l->closure && l->closure->parent && l->closure->parent->kind == NodeKind::FunctionDecl)
        currentFunction = static_cast<FunctionDecl*>(l->closure->parent);
      if (args)
        currentArgs = args;
      if (l->closure && l->closure->file)
        currentFile = l->closure->file;
      if (sh.debugInfo)
      {
        debugAt = l;
        debugFile = currentFile;
      }
      currentReturn = callTy->inner ? callTy->inner : types.voidType();
      if (!currentReturn->isKnown())
        currentReturn = types.pointer(types.voidType());
      sretAddr.clear();
      currentClassTy = writer.enclosingClass;
      try
      {
        Signature s = lambdaSignatureOf(callTy, l);
        std::string params = "ptr %closure";
        bool hasSret = s.hasSret;
        if (hasSret)
        {
          params += ", ptr %sret";
          sretAddr = "%sret";
        }
        std::vector<std::string> paramNames;
        for (size_t i = 0; i < callTy->args.size(); ++i)
        {
          std::string pn = "%p" + std::to_string(i);
          paramNames.push_back(pn);
          params += ", " + s.paramTypes[i + (hasSret ? 2 : 1)] + " " + pn;
        }
        std::string retType = s.returnType;
        std::string header = "define " + retType + " " + name + "(" + params + ") {\nentry:\n";
        pushScope();
        // Captures become locals: by-value captures live in the closure, references point out.
        // The body's own names for them, field by field (captureList of the lambda emitted).
        std::vector<CaptureSlot> slots = captureList(l);
        if (slots.size() != info.captured.size())
          unsupported(l, "a lambda whose captures differ from its closure's");
        for (size_t i = 0; i < info.captured.size(); ++i)
        {
          std::string field = gep(info.structName, "%closure", uint32_t(i));
          Decl* d = slots[i].decl;
          if (!d)
          {
            std::string thisSlot = "%v" + std::to_string(temp++);
            allocas += "  " + thisSlot + " = alloca ptr\n";
            std::string loaded = newTemp();
            fn += "  " + loaded + " = load ptr, ptr " + field + "\n";
            fn += "  store ptr " + loaded + ", ptr " + thisSlot + "\n";
            locals[nullptr] = {thisSlot, nullptr, false};
            if (currentClassTy)
              debugVariable("this", l, thisSlot, types.pointer(currentClassTy));
            continue;
          }
          locals[d] = {field, info.byRef[i] ? types.lref(info.fieldTypes[i]) : info.fieldTypes[i], info.byRef[i]};
          // (by value the capture is its field; by reference the field holds its address: a reference)
          if (std::string name = debugName(d); !name.empty())
            debugVariable(name, d, field, info.byRef[i] ? types.lref(info.fieldTypes[i]) : info.fieldTypes[i], 0, false, true);
        }
        for (size_t i = 0; i < l->params.size() && i < callTy->args.size(); ++i)
        {
          const Ty* pt = callTy->args[i];
          auto* pd = l->params[i]->kind == NodeKind::ParamDecl ? static_cast<ParamDecl*>(l->params[i]) : nullptr;
          if (isAggregate(pt))
          {
            locals[l->params[i]] = {paramNames[i], pt, false};
            if (needsDestruction(pt, l))
              scopes.back().push_back({paramNames[i], pt});
            if (pd && pd->name != UINT32_MAX)
              debugVariable(sym(pd->name), pd, paramNames[i], pt, uint32_t(i + 1));
            continue;
          }
          bool isRef = pt->isReference();
          std::string ps = "%v" + std::to_string(temp++);
          allocas += "  " + ps + " = alloca " + (isRef ? std::string("ptr") : llType(pt, l)) + "\n";
          fn += "  store " + (isRef ? std::string("ptr") : llType(pt, l)) + " " + paramNames[i] + ", ptr " + ps + "\n";
          locals[l->params[i]] = {ps, pt, isRef};
          if (pd && pd->name != UINT32_MAX)
            debugVariable(sym(pd->name), pd, ps, pt, uint32_t(i + 1));
        }
        namedReturn = namedReturnVariable(l->body);
        statement(l->body);
        if (!blockTerminated)
        {
          popScope();
          if (currentReturn->kind == TyKind::Void || hasSret)
            fn += "  ret void\n";
          else
            fn += "  ret " + (currentReturn->isReference() ? std::string("ptr null") : llType(currentReturn, l) + " zeroinitializer") + "\n";
        }
        addFunction(name, functionText(header));
      }
      catch (Unsupported&)
      {
        restoreState(saved);
        currentArgs = savedArgs;
        currentFile = savedFile;
        throw;
      }
      restoreState(saved);
      currentArgs = savedArgs;
      currentFile = savedFile;
    }

    // The plain function lambda l is written in: none for a lambda in a template (a function template,
    // a member of a class template), in a member of a local class (inside a function that may be one),
    // or outside any function (a default member initializer's, a default argument's).
    FunctionDecl* lambdaWriter(LambdaExpr* l)
    {
      if (!l || !l->closure)
        return nullptr;
      Decl* writer = l->closure->parent;
      while (writer && writer->kind != NodeKind::FunctionDecl)
      {
        if (!writer->templateParams.empty())
          return nullptr;
        writer = writer->parent;
      }
      if (!writer || !writer->templateParams.empty())
        return nullptr;
      for (Decl* o = global.owningClass(writer); o; o = o->parent)
        if (!o->templateParams.empty() || (o->kind == NodeKind::FunctionDecl && o != writer))
          return nullptr;
      return static_cast<FunctionDecl*>(writer);
    }

    // Where lambda l's call operator goes (named name; not a generic lambda's instance, which is one by
    // name): beside its plain writer, in its file's module, so that a lambda stays with the function it
    // is written in; else (a template's lambda; a default member initializer's, written by each
    // constructor; a default argument's, by each call site) the module its name hashes to. Its
    // closure's first writer and a caller emitting it on demand both take it from the lambda alone, so
    // whichever is first, it is in one module.
    unsigned lambdaHome(LambdaExpr* l, const std::string& name)
    {
      if (FunctionDecl* writer = lambdaWriter(l))
        if (auto it = sh.fileModule.find(writer->file); it != sh.fileModule.end())
          return it->second;
      return moduleCount > 1 ? sh.emissionModule(std::hash<std::string>()(name)) : currentModule;
    }

    // The call operator of a (non-generic) lambda a call needs, emitted from the lambda's AST by
    // whoever needs it first when its writer has not (a lambda a function returns, whose caller may
    // be emitted before it; a private copy's lambda type the writer never makes: the callers of
    // RenderParameters::getScreenPositionFunction). Only for a lambda written outside templates:
    // an instance's arguments, which its body may need, are known to its writer alone.
    void emitCallOperatorOnDemand(const Ty* t, const Ty* callTy, const std::string& name)
    {
      LambdaExpr* l = lambdaOf(t);
      FunctionDecl* writer = lambdaWriter(l);
      if (!writer)
        return;
      Decl* ownerClass = global.owningClass(writer);
      {
        auto lock = sharedLock();
        if (!sh.closuresEmitted.insert(t).second)
          return;
        ClosureWriter& recorded = sh.closureWriters[types.unqualified(t)];
        if (!recorded.enclosingClass && ownerClass && ownerClass->kind == NodeKind::ClassDecl)
          recorded.enclosingClass = types.classType(ownerClass, {}); // what `this` in the body points to
      }
      // Placed and owned as if its writer had made it, whichever caller needs it first (callers of one
      // level are emitted at once): where its closure's first writer puts it too (lambdaHome: the two
      // share closuresEmitted), belonging to the writer. An edit of the writer drops it, and rebuildLLVM
      // emits it again on its own (Shared::Definition::closure); what it inlines is its own, not its
      // first caller's.
      auto* writerFunction = static_cast<FunctionDecl*>(writer);
      struct Restore
      {
        Emitter& e;
        unsigned module;
        std::string owner;
        std::vector<const FunctionDecl*> inlined;
        OnDemand onDemand;
        ~Restore()
        {
          e.currentModule = module;
          e.owner = std::move(owner);
          e.inlinedNow = std::move(inlined);
          e.onDemandNow = std::move(onDemand);
        }
      } restore{*this, currentModule, owner, std::move(inlinedNow), onDemandNow};
      inlinedNow.clear();
      if (moduleCount > 1)
        currentModule = lambdaHome(l, name);
      owner = functionName(writerFunction, {}, nullptr);
      onDemandNow = {name, t, callTy, writerFunction};
      TemplateArgs none;
      emitLambda(l, t, callTy, name, &none);
    }

    // A lambda without captures used as a pointer to function: a function with the pointer's
    // signature that calls the lambda's operator with no closure (nothing is captured), defined
    // once per lambda.
    std::string lambdaThunk(const Ty* t, const Ty* fnTy, Node* at)
    {
      t = types.unqualified(t);
      LambdaExpr* l = lambdaOf(t);
      ClosureInfo info = closureFor(t, at);
      if (!info.captured.empty())
        unsupported(at, "a lambda with captures used as a pointer to function");
      // The operator the pointer calls: the lambda's, or for a generic one (`[](const auto&
      // parameters, MapDeserialiser& input) -> ElemType*` as InstanceLoader's LoadFunction) the
      // instance whose parameters the pointer's give, each `auto` the pointer's type there.
      const Ty* callTy = types.function(t->inner ? t->inner : types.voidType(), t->args, false);
      std::string functionName = info.functionName;
      std::string suffix;
      if (l && !l->templateParams.empty())
      {
        if (fnTy->args.size() != t->args.size())
          unsupported(at, "a lambda used as a pointer to a function of another signature");
        TemplateArgs own;
        for (Decl* p : l->templateParams)
        {
          const Ty* found = nullptr;
          for (size_t i = 0; i < t->args.size() && !found; ++i)
            if (const Ty* pt = types.unqualified(types.stripRef(t->args[i])); pt->kind == TyKind::Param && pt->decl == p)
              found = types.unqualified(types.stripRef(fnTy->args[i]));
          if (!found)
            unsupported(at, "a generic lambda used as a pointer to function, its parameters not deduced from the pointer's");
          own.pairs.push_back({p, found});
        }
        TemplateArgs instanceArgs;
        LambdaExpr* genericBody = nullptr;
        callTy = global.lambdaInstance(t, own, &instanceArgs, &genericBody);
        if (!callTy || !genericBody)
          unsupported(at, "a generic lambda whose body could not be typed for the pointer's parameters");
        std::string params;
        for (size_t i = 0; i < callTy->args.size(); ++i)
          params += (i ? ", " : "") + irString(callTy->args[i]);
        functionName = genericOperatorName(t, own, callTy);
        suffix = "(" + params + ")";
        bool firstUse;
        {
          auto lock = sharedLock();
          firstUse = lambdaInstances.insert(functionName).second;
        }
        if (firstUse)
        {
          NameHome home(*this, functionName);
          emitLambda(genericBody, t, callTy, functionName, &instanceArgs);
        }
      }
      const Ty* ret = fnTy->inner;
      const Ty* lambdaRet = callTy->inner ? callTy->inner : types.voidType();
      if (types.unqualified(types.stripRef(ret)) != types.unqualified(types.stripRef(lambdaRet)) || fnTy->args.size() != callTy->args.size())
        unsupported(at, "a lambda used as a pointer to a function of another signature");
      for (size_t i = 0; i < fnTy->args.size(); ++i)
      {
        const Ty* pt = fnTy->args[i];
        if (types.unqualified(types.stripRef(pt)) != types.unqualified(types.stripRef(callTy->args[i])))
          unsupported(at, "a lambda used as a pointer to a function of another signature");
        if (isClass(pt) && needsDestruction(pt, at))
          unsupported(at, "a lambda used as a pointer to function taking an object with a destructor by value");
      }
      std::string name = "@\"" + irString(t) + "::thunk" + suffix + "\"";
      {
        auto lock = sharedLock();
        if (!lambdaInstances.insert(name).second)
          return name;
      }
      bool hasSret = returnsInMemory(ret);
      std::string retType = hasSret ? "void" : (!ret || !ret->isKnown()) ? "ptr" : ret->isReference() ? "ptr" : llType(ret, at);
      std::string params, args = "ptr null";
      if (hasSret)
      {
        params = "ptr %sret";
        args += ", ptr %sret";
      }
      for (size_t i = 0; i < fnTy->args.size(); ++i)
      {
        const Ty* pt = fnTy->args[i];
        std::string ty = isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, at);
        params += (params.empty() ? "" : ", ") + ty + " %a" + std::to_string(i);
        args += ", " + ty + " %a" + std::to_string(i);
      }
      std::string text = "define " + retType + " " + name + "(" + params + ") {\nentry:\n";
      if (hasSret || ret->kind == TyKind::Void)
        text += "  call void " + functionName + "(" + args + ")\n  ret void\n";
      else
        text += "  %r = call " + retType + " " + functionName + "(" + args + ")\n  ret " + retType + " %r\n";
      text += "}\n\n";
      NameHome home(*this, name);
      addFunction(name, text, true); // emitted once, at its first use: its own, not the user's
      return name;
    }

    // Calls a closure value with arguments.
    // The call operator of generic lambda t for its own arguments, one per set of them: named by
    // those arguments and the parameter types they give (a template parameter no parameter
    // mentions tells instances apart only by its argument: `[]<class T>(const char* name)`, called
    // `runTest.operator()<CraftingMachine>(name)` and `runTest.operator()<RocketSilo>(name)`).
    std::string genericOperatorName(const Ty* t, const TemplateArgs& own, const Ty* callTy)
    {
      std::string args, params;
      for (size_t i = 0; i < own.pairs.size(); ++i)
        args += (i ? ", " : "") + irString(own.pairs[i].second);
      for (size_t i = 0; i < callTy->args.size(); ++i)
        params += (i ? ", " : "") + irString(callTy->args[i]);
      return "@\"" + irString(t) + "::operator()<" + args + ">(" + params + ")\"";
    }

    Value callClosure(Value closure, Span<Expr*> callArgs, const std::string& sret, Node* at)
    {
      const Ty* t = types.unqualified(closure.ty); // a const lambda variable calls the same operator
      ClosureInfo info = closureFor(t, at);
      const Ty* callTy = types.function(t->inner ? t->inner : types.voidType(), t->args, false);
      std::string functionName = info.functionName;
      TemplateArgs instanceArgs;
      LambdaExpr* genericBody = nullptr; // a generic lambda's copy of its parameters and body for this call's arguments
      LambdaExpr* genericLambda = lambdaOf(t);
      if (genericLambda && genericLambda->templateParams.empty())
        genericLambda = nullptr;
      if (LambdaExpr* l = genericLambda)
      {
        // A generic lambda: the call's deduced arguments (recorded by the type checker, as for a
        // function template) pick the instantiation of its call operator, emitted at first use.
        CallExpr* call = at && at->kind == NodeKind::CallExpr ? static_cast<CallExpr*>(at) : nullptr;
        if (!call || call->templateArgCount != l->templateParams.size())
          unsupported(at, "a call to a generic lambda whose parameter types were not deduced");
        TemplateArgs own;
        for (size_t i = 0; i < l->templateParams.size(); ++i)
          own.pairs.push_back({l->templateParams[i], call->templateArgs[i]});
        callTy = global.lambdaInstance(t, own, &instanceArgs, &genericBody);
        if (!callTy || !genericBody)
          unsupported(at, "a generic lambda whose body could not be typed for this call");
        functionName = genericOperatorName(t, own, callTy);
      }
      else
      {
        // Emitted with the function the lambda is written in: checked once generation is done.
        {
          auto lock = sharedLock();
          sh.closureCalls.emplace(functionName, t);
        }
        // Declared by the call, for a program piece whose function calls an operator its writer (an
        // instance) emits at a later level (Shared::requestDeclarations).
        if (recordingRequests())
          withoutFailing([&] { recordRequestDeclaration(functionName, declarationText(lambdaSignatureOf(callTy, at), functionName)); });
        emitCallOperatorOnDemand(t, callTy, functionName);
      }
      const Ty* ret = callTy->inner ? callTy->inner : types.voidType();
      std::string argText = "ptr " + closure.text;
      std::string resultAddr = sret;
      bool hasSret = returnsInMemory(ret);
      bool resultTemp = false; // owned by the statement once the call returned it
      if (hasSret)
      {
        if (resultAddr.empty())
        {
          resultAddr = newObjectSlot(ret, at);
          resultTemp = true;
        }
        argText += ", ptr " + resultAddr;
      }
      std::string sig = (hasSret ? "void" : ret->isReference() ? "ptr" : llType(ret, at)) + std::string(" (ptr") + (hasSret ? ", ptr" : "");
      size_t position = 0;
      for (Expr* arg : callArgs)
      {
        if (isPackExpansion(arg))
        {
          Expr* pattern = static_cast<UnaryExpr*>(arg)->operand;
          forEachPackElement(pattern, at, [&](size_t)
          {
            const Ty* pt = position < callTy->args.size() ? callTy->args[position] : nullptr;
            if (!pt)
              unsupported(at, "too many arguments to a lambda");
            sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
            argText += ", " + argumentText(pt, pattern, at);
            ++position;
          });
          continue;
        }
        const Ty* pt = position < callTy->args.size() ? callTy->args[position] : nullptr;
        if (!pt)
          unsupported(at, "too many arguments to a lambda");
        sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
        argText += ", " + argumentText(pt, arg, at);
        ++position;
      }
      // Parameters the call left out take their default arguments: GuiTargetItemBase's loader calls
      // `[&](auto& value, OpenGuiType initIfType, bool load = true)` with two, and the operator read
      // whatever its third register held (no targeter was read, and the map loading lost its place).
      LambdaExpr* declared = genericBody ? genericBody : lambdaOf(t);
      for (; position < callTy->args.size(); ++position)
      {
        auto* pd = declared && position < declared->params.size() ? static_cast<ParamDecl*>(declared->params[position]) : nullptr;
        if (!pd || pd->isPack)
          break;
        if (!pd->defaultValue)
          unsupported(at, "a call to a lambda without an argument for its parameter " + (pd->name != UINT32_MAX ? sym(pd->name) : std::to_string(position + 1)));
        const Ty* pt = callTy->args[position];
        sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
        if (pd->defaultValue->kind == NodeKind::CallerExpr)
          argText += ", ptr " + callerLocation(pt, at); // `CallerLocation here = caller`: this call's location, as for a function
        else
          argText += ", " + argumentText(pt, pd->defaultValue, pd->defaultValue);
      }
      sig += ")";
      // The instantiation's operator is emitted after the arguments: a lambda argument's closure
      // (its capture list) is built here, in the function that writes it, before the operator's
      // body calls it.
      bool firstUse = false;
      if (genericLambda)
      {
        auto lock = sharedLock();
        firstUse = lambdaInstances.insert(functionName).second;
      }
      if (firstUse)
      {
        NameHome home(*this, functionName);
        emitLambda(genericBody, t, callTy, functionName, &instanceArgs);
      }
      if (hasSret || ret->kind == TyKind::Void)
      {
        emitCall("", sig + " " + functionName + "(" + argText + ")");
        if (resultTemp)
          statementTemps.push_back({resultAddr, ret}); // not before: a call that throws built no result to destroy
        return hasSret ? Value{resultAddr, ret, sret.empty()} : Value{"", types.voidType(), false};
      }
      std::string r = newTemp();
      emitCall(r + " = ", sig + " " + functionName + "(" + argText + ")");
      return {r, ret, false};
    }

