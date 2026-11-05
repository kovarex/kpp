// GlobalScope::Impl, phase 1, indexing: part of resolver.cpp, included in the class's body there (a file of its
// own since 2026-11-05, moved without a change; docs/design.md, "Compilation model").
  // ---------------- phase 1: indexing ----------------

  static uint32_t simpleName(const QualifiedName& name)
  {
    return name.parts.size() == 1 && name.parts[0].special == 0 ? name.parts[0].symbol : UINT32_MAX;
  }

  // Symbol under which a (possibly operator) function is indexed, or UINT32_MAX.
  // The symbol a function is known by: its own name, the last part of `Class<T>::name`.
  uint32_t functionSymbol(const QualifiedName& name) const
  {
    const NamePart& part = name.parts[name.parts.size() - 1];
    if (part.special == 0)
      return part.symbol;
    if (part.special == 2 && part.opToken < uint32_t(P::Count))
      return opSymbols[part.opToken];
    return UINT32_MAX;
  }

  // The out-of-line member function definitions indexing deferred (deferredFunctions), attached to
  // their classes (outOfLineMembers; out-of-line in a namespace: indexed there). finishIndex runs it,
  // and a compile server's build that adds such definitions (step 4).
  std::unordered_map<const FunctionDecl*, Scope*> outOfLineScope; // the class each out-of-line definition was attached to
  void attachOutOfLine(std::vector<FunctionDecl*>& attached)
  {
    for (auto& [f, context] : deferredFunctions)
    {
      FileOfLookups ofFile(f->file);
      QualifiedName qualifier = f->name;
      qualifier.parts.count -= 1;
      LocalStack params;
      for (Decl* p : f->templateParams)
        params.push(static_cast<TemplateParamDecl*>(p)->name, p);
      Lookup l = lookupQualified(qualifier, context, &params, 0);
      if (l.scope && l.scope->kind == ScopeKind::Class)
      {
        if (Scope* special = fullSpecializationScope(qualifier, context, &params, f->file, f->token))
        {
          l.scope = special; // `int Flag<true>::value() {...}`: a member of the specialisation
          specializationMembers[f] = special;
        }
        else if (Scope* partial = partialSpecializationScope(qualifier, l, context, f->templateParams, f->file))
        {
          l.scope = partial; // `template<typename T> int Holder<T, true>::load(int)`: of the partial specialisation
          specializationMembers[f] = partial;
        }
        else if ((f->flags & DF_Specialization) && f->isTemplate && f->templateParams.empty() && !isMemberSpecialization(f) &&
                 l.scope->owner && !effectiveParams(l.scope->owner).empty())
        {
          // `template<> Vector<int> UpdateBuffer::generate()` with `using UpdateBuffer =
          // Buffer<Update>;`: that instance's member, as `Buffer<Update>::generate` writes it (not
          // a definition of the template's member, which every instance would run).
          TokenScope tokenScope(f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view());
          if (const Ty* written = specializedClassOf(f); written && !written->args.empty())
            f->attributes |= ATTR_MemberSpecialization;
        }
      }
      if (l.scope && (l.scope->kind == ScopeKind::Class || l.scope->kind == ScopeKind::Namespace))
        if (auto it = localTypesOfOutOfLine.find(f); it != localTypesOfOutOfLine.end())
          for (Scope* local : it->second)
            local->parent = l.scope; // a class declared in `void A::f() {...}` sees A's members
      if (l.scope && l.scope->kind == ScopeKind::Class)
      {
        l.scope->outOfLineMembers.push_back(f);
        outOfLineScope[f] = l.scope; // (a compile server's later build may take it out again)
        attached.push_back(f);
      }
      else if (l.scope && l.scope->kind == ScopeKind::Namespace)
      {
        // `void StringUtil::f() {}` outside its namespace: the same as written inside it (its name
        // is found there, its body sees the namespace), as C++ has it.
        QualifiedName simple;
        simple.parts.data = f->name.parts.data + (f->name.parts.size() - 1);
        simple.parts.count = 1;
        f->name = simple;
        if (l.scope->owner)
          f->parent = l.scope->owner;
        indexDecl(f, l.scope);
      }
    }
    deferredFunctions.clear();
    localTypesOfOutOfLine.clear();
  }

  // The name a declaration a false `when` dropped declares in its scope (design.md 19), or none.
  uint32_t absentName(const Decl* d)
  {
    switch (d->kind)
    {
      case NodeKind::VariableDecl:
        return static_cast<const VariableDecl*>(d)->name.parts.size() == 1 ? static_cast<const VariableDecl*>(d)->name.parts[0].symbol : UINT32_MAX;
      case NodeKind::FunctionDecl:
      {
        auto* f = static_cast<const FunctionDecl*>(d);
        return f->name.parts.size() == 1 && !(f->flags & (DF_Constructor | DF_Destructor | DF_Conversion | DF_Friend)) ? functionSymbol(f->name) : UINT32_MAX;
      }
      case NodeKind::ClassDecl:
        return static_cast<const ClassDecl*>(d)->name.parts.size() == 1 ? static_cast<const ClassDecl*>(d)->name.parts[0].symbol : UINT32_MAX;
      case NodeKind::EnumDecl:
        return static_cast<const EnumDecl*>(d)->name.parts.size() == 1 ? static_cast<const EnumDecl*>(d)->name.parts[0].symbol : UINT32_MAX;
      case NodeKind::UsingAliasDecl: return static_cast<const UsingAliasDecl*>(d)->name;
      default: return UINT32_MAX;
    }
  }

  void indexDecls(Span<Decl*> decls, Scope* scope)
  {
    for (Decl* d : decls)
      indexDecl(d, scope);
  }

  void indexDecl(Decl* d, Scope* scope)
  {
    switch (d->kind)
    {
      case NodeKind::NamespaceDecl:
      {
        auto* ns = static_cast<NamespaceDecl*>(d);
        Scope* target = scope;
        if (ns->name != UINT32_MAX)
        {
          for (const NamePart& part : ns->nested.parts)
          {
            target = target->getOrCreateNamespace(part.symbol, ns);
            if (target->owner == ns)
              ++scopes;
          }
        }
        // Anonymous and inline namespaces are flattened into the parent.
        ns->ownScope = target;
        indexDecls(ns->decls, target);
        break;
      }
      case NodeKind::ClassDecl:
      {
        auto* c = static_cast<ClassDecl*>(d);
        if (c->flags & DF_Friend)
          break; // `friend class Other;` names no nested class and no member
        if (c->name.parts.size() > 1)
        {
          // `struct A::B { ... }`: attach once A exists.
          if (c->isDefinition)
          {
            std::lock_guard lock(deferredMutex);
            deferredQualified.push_back({c, scope});
          }
          break;
        }
        Scope* own = nullptr;
        if (c->isDefinition)
        {
          own = new Scope(ScopeKind::Class, scope, c, 1);
          own->templateParams = c->templateParams;
          for (const ClassDecl::Base& b : c->bases)
            own->baseTypes.push_back(b.type);
          c->ownScope = own;
          ++scopes;
          indexDecls(c->members, own);
          for (Decl* a : c->absentMembers) // (the scope is this thread's until the index is done)
            if (uint32_t symbol = absentName(a); symbol != UINT32_MAX)
              own->absent.emplace(symbol, a);
        }
        if (c->name.empty())
        {
          // Anonymous struct/union: its members belong to the enclosing scope.
          if (own)
            indexDecls(c->members, scope);
        }
        else
        {
          scope->add(c->name.parts[0].symbol, c, own);
          ++entries;
        }
        break;
      }
      case NodeKind::EnumDecl:
      {
        auto* e = static_cast<EnumDecl*>(d);
        if (e->name.parts.size() > 1)
        {
          if (e->isDefinition)
          {
            std::lock_guard lock(deferredMutex);
            deferredQualified.push_back({e, scope});
          }
          break;
        }
        Scope* own = nullptr;
        if (e->isDefinition)
        {
          own = new Scope(ScopeKind::Enum, scope, e, 1);
          e->ownScope = own;
          ++scopes;
          for (Decl* en : e->enumerators)
            if (en->kind == NodeKind::EnumeratorDecl)
            {
              uint32_t name = static_cast<EnumeratorDecl*>(en)->name;
              own->add(name, en, nullptr);
              ++entries;
              if (!e->isClass)
                scope->add(name, en, nullptr); // Unscoped enumerators leak into the enclosing scope.
            }
          indexDecls(e->members, own);
        }
        if (!e->name.empty())
        {
          scope->add(e->name.parts[0].symbol, e, own);
          ++entries;
        }
        break;
      }
      case NodeKind::FunctionDecl:
      {
        auto* f = static_cast<FunctionDecl*>(d);
        // Out-of-line definitions (`void A::f()`) are not new names. Constructors/destructors
        // are reachable through the class name.
        if (f->name.parts.size() == 1 && !(f->flags & (DF_Constructor | DF_Destructor | DF_Conversion)) &&
            !(f->flags & DF_Friend))
        {
          uint32_t symbol = functionSymbol(f->name);
          if (symbol != UINT32_MAX)
          {
            scope->add(symbol, f, nullptr);
            ++entries;
          }
          if (!f->objectArgs.empty() && !f->body)
          {
            std::lock_guard lock(deferredMutex);
            objectCandidates.push_back({f, scope}); // `Pair<int, Mode> five(a, m);`: decided by finishIndex
          }
        }
        else if (f->name.parts.size() > 1 && (f->body || (f->flags & (DF_Default | DF_Delete))))
        {
          // An out-of-line definition, `Flow::~Flow() = default;` included: attached to its class by finishIndex.
          std::lock_guard lock(deferredMutex);
          deferredFunctions.push_back({f, scope});
        }
        if (f->body)
          indexLocalTypes(f->body, scope, f);
        break;
      }
      case NodeKind::VariableDecl:
      {
        auto* v = static_cast<VariableDecl*>(d);
        if (v->name.parts.size() == 1)
        {
          uint32_t symbol = simpleName(v->name);
          if (symbol != UINT32_MAX)
          {
            scope->add(symbol, v, nullptr);
            ++entries;
          }
        }
        else if (v->name.parts.size() > 1)
        {
          // `const Style& Widget::defaultWidgetStyle = Widget::getStyleStatic();`: the definition
          // of a static data member, attached to its declaration by finishIndex.
          std::lock_guard lock(deferredMutex);
          deferredVariables.push_back({v, scope});
        }
        for (Expr* b : v->structuredBindings)
        {
          scope->add(simpleName(static_cast<NameExpr*>(b)->name), v, nullptr);
          ++entries;
        }
        break;
      }
      case NodeKind::DeductionGuideDecl:
      {
        // Not a name of its own: the class template it guides is found by finishIndex.
        std::lock_guard lock(deferredMutex);
        deferredGuides.push_back({static_cast<DeductionGuideDecl*>(d), scope});
        break;
      }
      case NodeKind::UsingAliasDecl:
      {
        auto* u = static_cast<UsingAliasDecl*>(d);
        scope->add(u->name, u, nullptr);
        ++entries;
        break;
      }
      case NodeKind::UsingNameDecl:
      {
        auto* u = static_cast<UsingNameDecl*>(d);
        if (u->isEnum)
        {
          {
            std::lock_guard lock(scope->shards[0].mutex);
            scope->usingEnums.push_back(u);
          }
          std::lock_guard lock(deferredMutex);
          scopesWithUsingEnums.push_back(scope); // resolved by finishIndex
        }
        else if (!u->name.empty() && u->name.last().special == 0 &&
                 !(u->name.parts.size() >= 2 && u->name.last().symbol == u->name.parts[u->name.parts.size() - 2].symbol))
        {
          // (`using Base::Base;` inherits Base's constructors and names no member: `Base::` inside
          // the class still reaches the base class.)
          scope->add(u->name.last().symbol, u, nullptr);
          ++entries;
        }
        break;
      }
      case NodeKind::UsingDirectiveDecl:
      {
        std::lock_guard lock(scope->shards[0].mutex);
        scope->usingDirectives.push_back(static_cast<UsingDirectiveDecl*>(d));
        break;
      }
      default:
        break;
    }
  }

  // Classes and enums declared in a function body get their scopes as nested ones do, but their
  // names stay out of the enclosing scope: the function's locals carry them (declareLocal). The
  // scope's parent is the function's, its class for a member function (finishIndex moves an
  // out-of-line member's ones under its class).
  // The aliases and typedefs of a function body in scope at token `at` (`using nl = Limits<T1>;`
  // before `nl::min()`): those declared before it in the blocks around it, in declaration order,
  // so that an inner one comes later and hides an outer one of its name.
  // What a block-scope `using Graphics::Architecture;` brings in: the declaration it names, unless
  // that is a function (whose overloads the using-declaration would take together).
  Decl* localUsingTarget(UsingNameDecl* u, Scope* scope, const LocalStack* locals)
  {
    Lookup l = lookupQualified(u->name, scope, locals, 0);
    return l.decl && l.decl->kind != NodeKind::FunctionDecl ? l.decl : nullptr;
  }

  // `using Enum::True;` in a class or namespace: the enumerator it names, which the name means as
  // that enumerator's value (a constant, never an object); null for any other using-declaration.
  Decl* usingEnumerator(Decl* d, int depth)
  {
    if (!d || d->kind != NodeKind::UsingNameDecl)
      return nullptr;
    auto* u = static_cast<UsingNameDecl*>(d);
    if (u->isEnum || u->name.parts.size() < 2)
      return nullptr;
    Lookup l = lookupQualified(u->name, lexicalScopeOf(u), nullptr, depth + 1);
    return l.decl && l.decl->kind == NodeKind::EnumeratorDecl ? l.decl : nullptr;
  }

  // Those visible at token `at` in the body of the lambda whose closure is context (the code
  // generator types a lambda's body from its closure): a body's declarations belong to the function
  // around the lambda, whose statements do not reach into it.
  void collectLambdaBodyAliases(Decl* context, uint32_t at, LocalStack& out)
  {
    if (!context || context->kind != NodeKind::ClassDecl || !static_cast<ClassDecl*>(context)->isClosure)
      return;
    LambdaExpr* l = static_cast<ClassDecl*>(context)->lambda;
    if (l && l->body && l->body->token < at)
      collectVisibleLocalAliases(l->body, at, out);
  }

  // `using enum UndoActionDescription::Type;` in a block: the enumeration's enumerators, as locals
  // of the block (C++20's using-enum-declaration).
  void pushLocalUsingEnum(UsingNameDecl* u, Scope* scope, LocalStack& locals)
  {
    if (!u || u->name.empty())
      return;
    Lookup l = lookupQualified(u->name, scope, &locals, 0);
    Decl* e = l.scope && l.scope->owner && l.scope->owner->kind == NodeKind::EnumDecl ? l.scope->owner : l.decl;
    if (!e || e->kind != NodeKind::EnumDecl)
      return;
    for (Decl* en : static_cast<EnumDecl*>(e)->enumerators)
      if (en->kind == NodeKind::EnumeratorDecl)
        locals.push(static_cast<EnumeratorDecl*>(en)->name, en);
  }

  void collectVisibleLocalAliases(Stmt* s, uint32_t at, LocalStack& out, int depth = 0)
  {
    if (!s || s->token > at || depth > 64)
      return;
    auto declared = [&](Stmt* st)
    {
      if (st && st->kind == NodeKind::DeclStmt)
        for (Decl* d : static_cast<DeclStmt*>(st)->decls)
          if (d->endToken && d->endToken <= at)
          {
            if (d->kind == NodeKind::UsingAliasDecl)
              out.push(static_cast<UsingAliasDecl*>(d)->name, d);
            else if (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef) && !static_cast<VariableDecl*>(d)->name.empty())
              out.push(static_cast<VariableDecl*>(d)->name.last().symbol, d);
            else if (d->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(d)->name.parts.size() == 1)
              out.push(static_cast<ClassDecl*>(d)->name.parts[0].symbol, d); // `struct Point { float x, y; };` then `sizeof(Point)`
            else if (d->kind == NodeKind::EnumDecl && static_cast<EnumDecl*>(d)->name.parts.size() == 1)
              out.push(static_cast<EnumDecl*>(d)->name.parts[0].symbol, d);
            else if (d->kind == NodeKind::UsingNameDecl && static_cast<UsingNameDecl*>(d)->isEnum)
              pushLocalUsingEnum(static_cast<UsingNameDecl*>(d), lexicalScopeOf(d), out); // `using enum Desc::Type;`
            else if (d->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(d)->isEnum && !static_cast<UsingNameDecl*>(d)->name.empty())
              if (Decl* target = localUsingTarget(static_cast<UsingNameDecl*>(d), lexicalScopeOf(d), &out))
                out.push(static_cast<UsingNameDecl*>(d)->name.last().symbol, target); // `using Graphics::Architecture;`
          }
    };
    // Of statements in source order, the one `at` is in: the last that starts at or before it.
    auto enclosing = [&](std::initializer_list<Stmt*> candidates)
    {
      Stmt* found = nullptr;
      for (Stmt* c : candidates)
        if (c && c->token <= at)
          found = c;
      return found;
    };
    auto inner = [&](Stmt* st) { collectVisibleLocalAliases(st, at, out, depth + 1); };
    switch (s->kind)
    {
      case NodeKind::BlockStmt:
      {
        Span<Stmt*> stmts = static_cast<BlockStmt*>(s)->stmts;
        for (size_t i = 0; i < stmts.size(); ++i)
        {
          if (!stmts[i] || stmts[i]->token > at)
            break;
          declared(stmts[i]);
          if (i + 1 == stmts.size() || !stmts[i + 1] || stmts[i + 1]->token > at)
            inner(stmts[i]);
        }
        break;
      }
      case NodeKind::IfStmt:
      {
        auto* i = static_cast<IfStmt*>(s);
        declared(i->init);
        inner(enclosing({i->then, i->else_}));
        break;
      }
      case NodeKind::ForStmt:
        declared(static_cast<ForStmt*>(s)->init);
        inner(static_cast<ForStmt*>(s)->body);
        break;
      case NodeKind::RangeForStmt: inner(static_cast<RangeForStmt*>(s)->body); break;
      case NodeKind::ForConstexprStmt: inner(static_cast<ForConstexprStmt*>(s)->body); break;
      case NodeKind::WhileStmt: inner(static_cast<WhileStmt*>(s)->body); break;
      case NodeKind::DoStmt: inner(static_cast<DoStmt*>(s)->body); break;
      case NodeKind::SwitchStmt:
        declared(static_cast<SwitchStmt*>(s)->init);
        inner(static_cast<SwitchStmt*>(s)->body);
        break;
      case NodeKind::TryStmt:
      {
        auto* t = static_cast<TryStmt*>(s);
        Stmt* found = t->body && t->body->token <= at ? t->body : nullptr;
        for (const TryStmt::Handler& h : t->handlers)
          if (h.body && h.body->token <= at)
            found = h.body;
        inner(found);
        break;
      }
      default:
        break;
    }
  }

  void indexLocalTypes(Stmt* s, Scope* scope, FunctionDecl* f, std::unordered_map<uint32_t, std::vector<ClassDecl*>>* forward = nullptr)
  {
    if (!s)
      return;
    std::unordered_map<uint32_t, std::vector<ClassDecl*>> declaredAhead; // `struct Support;` before its definition
    if (!forward)
      forward = &declaredAhead;
    auto indexLocalTypes = [&](Stmt* inner, Scope* sc, FunctionDecl* fn) { this->indexLocalTypes(inner, sc, fn, forward); };
    switch (s->kind)
    {
      case NodeKind::BlockStmt:
        for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
          indexLocalTypes(inner, scope, f);
        break;
      case NodeKind::DeclStmt:
        for (Decl* d : static_cast<DeclStmt*>(s)->decls)
        {
          if (d->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(d)->isDefinition && static_cast<ClassDecl*>(d)->name.parts.size() == 1 &&
              !(d->flags & DF_Friend))
            (*forward)[static_cast<ClassDecl*>(d)->name.parts[0].symbol].push_back(static_cast<ClassDecl*>(d));
          Scope* own = (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl) && !d->ownScope ? indexLocalType(d, scope) : nullptr;
          // The body's earlier declarations of the class are the same class: they name its scope.
          if (own && d->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(d)->name.parts.size() == 1)
            if (auto it = forward->find(static_cast<ClassDecl*>(d)->name.parts[0].symbol); it != forward->end())
            {
              for (ClassDecl* ahead : it->second)
                ahead->ownScope = own;
              forward->erase(it);
            }
          if (own && f->name.parts.size() > 1)
          {
            std::lock_guard lock(deferredMutex);
            localTypesOfOutOfLine[f].push_back(own);
          }
          if (d->kind == NodeKind::VariableDecl)
            forEachLambda(static_cast<VariableDecl*>(d)->init, [&](LambdaExpr* l) { indexLocalTypes(l->body, scope, f); });
        }
        break;
      case NodeKind::ExprStmt:
        forEachLambda(static_cast<ExprStmt*>(s)->expr, [&](LambdaExpr* l) { indexLocalTypes(l->body, scope, f); });
        break;
      case NodeKind::ReturnStmt:
        forEachLambda(static_cast<ReturnStmt*>(s)->value, [&](LambdaExpr* l) { indexLocalTypes(l->body, scope, f); });
        break;
      case NodeKind::IfStmt:
      {
        auto* i = static_cast<IfStmt*>(s);
        indexLocalTypes(i->init, scope, f);
        indexLocalTypes(i->then, scope, f);
        indexLocalTypes(i->else_, scope, f);
        break;
      }
      case NodeKind::ForStmt:
        indexLocalTypes(static_cast<ForStmt*>(s)->init, scope, f);
        indexLocalTypes(static_cast<ForStmt*>(s)->body, scope, f);
        break;
      case NodeKind::RangeForStmt: indexLocalTypes(static_cast<RangeForStmt*>(s)->body, scope, f); break;
      case NodeKind::ForConstexprStmt: indexLocalTypes(static_cast<ForConstexprStmt*>(s)->body, scope, f); break;
      case NodeKind::WhileStmt: indexLocalTypes(static_cast<WhileStmt*>(s)->body, scope, f); break;
      case NodeKind::DoStmt: indexLocalTypes(static_cast<DoStmt*>(s)->body, scope, f); break;
      case NodeKind::SwitchStmt:
        indexLocalTypes(static_cast<SwitchStmt*>(s)->init, scope, f);
        indexLocalTypes(static_cast<SwitchStmt*>(s)->body, scope, f);
        break;
      case NodeKind::TryStmt:
      {
        auto* t = static_cast<TryStmt*>(s);
        indexLocalTypes(t->body, scope, f);
        for (const TryStmt::Handler& h : t->handlers)
          indexLocalTypes(h.body, scope, f);
        break;
      }
      default:
        break;
    }
  }

  // Calls body with each lambda in expression n, not looking into the lambdas themselves (local
  // types in a lambda's body are indexed with the body: GraphicsInterfaceDX11::logDeviceInfo's
  // createDriverDescription declares an unnamed union).
  template<typename F>
  static void forEachLambda(Node* n, F&& body, int depth = 0)
  {
    if (!n || depth > 64)
      return;
    auto each = [&](Span<Expr*> list)
    {
      for (Expr* e : list)
        forEachLambda(e, body, depth + 1);
    };
    switch (n->kind)
    {
      case NodeKind::LambdaExpr: body(static_cast<LambdaExpr*>(n)); break;
      case NodeKind::CallExpr: forEachLambda(static_cast<CallExpr*>(n)->callee, body, depth + 1); each(static_cast<CallExpr*>(n)->args); break;
      case NodeKind::MemberExpr: forEachLambda(static_cast<MemberExpr*>(n)->base, body, depth + 1); break;
      case NodeKind::IndexExpr: forEachLambda(static_cast<IndexExpr*>(n)->base, body, depth + 1); each(static_cast<IndexExpr*>(n)->args); break;
      case NodeKind::UnaryExpr: forEachLambda(static_cast<UnaryExpr*>(n)->operand, body, depth + 1); break;
      case NodeKind::BinaryExpr:
        forEachLambda(static_cast<BinaryExpr*>(n)->lhs, body, depth + 1);
        forEachLambda(static_cast<BinaryExpr*>(n)->rhs, body, depth + 1);
        break;
      case NodeKind::ConditionalExpr:
        forEachLambda(static_cast<ConditionalExpr*>(n)->cond, body, depth + 1);
        forEachLambda(static_cast<ConditionalExpr*>(n)->then, body, depth + 1);
        forEachLambda(static_cast<ConditionalExpr*>(n)->else_, body, depth + 1);
        break;
      case NodeKind::CastExpr: forEachLambda(static_cast<CastExpr*>(n)->operand, body, depth + 1); break;
      case NodeKind::NamedCastExpr: forEachLambda(static_cast<NamedCastExpr*>(n)->operand, body, depth + 1); break;
      case NodeKind::FunctionalCastExpr: each(static_cast<FunctionalCastExpr*>(n)->args); break;
      case NodeKind::BraceInitExpr: each(static_cast<BraceInitExpr*>(n)->args); break;
      case NodeKind::ParenExpr: forEachLambda(static_cast<ParenExpr*>(n)->inner, body, depth + 1); break;
      case NodeKind::ThrowExpr: forEachLambda(static_cast<ThrowExpr*>(n)->operand, body, depth + 1); break;
      default: break;
    }
  }

  // The scope of a class or enum defined in a function body, its members indexed in it.
  Scope* indexLocalType(Decl* d, Scope* scope)
  {
    Scope* own = nullptr;
    if (d->kind == NodeKind::ClassDecl)
    {
      auto* c = static_cast<ClassDecl*>(d);
      if (!c->isDefinition || c->name.parts.size() > 1 || (c->flags & DF_Friend))
        return nullptr;
      own = new Scope(ScopeKind::Class, scope, c, 1);
      own->templateParams = c->templateParams;
      for (const ClassDecl::Base& b : c->bases)
        own->baseTypes.push_back(b.type);
      c->ownScope = own;
      // The class's name inside it (`bool operator<(const Key&) const`): the enclosing scope does
      // not have it.
      if (!c->name.empty())
        own->add(c->name.parts[0].symbol, c, own);
      indexDecls(c->members, own);
      for (Decl* a : c->absentMembers)
        if (uint32_t symbol = absentName(a); symbol != UINT32_MAX)
          own->absent.emplace(symbol, a);
    }
    else
    {
      auto* e = static_cast<EnumDecl*>(d);
      if (!e->isDefinition || e->name.parts.size() > 1)
        return nullptr;
      own = new Scope(ScopeKind::Enum, scope, e, 1);
      e->ownScope = own;
      for (Decl* en : e->enumerators)
        if (en->kind == NodeKind::EnumeratorDecl)
          own->add(static_cast<EnumeratorDecl*>(en)->name, en, nullptr);
      indexDecls(e->members, own);
    }
    ++scopes;
    return own;
  }

