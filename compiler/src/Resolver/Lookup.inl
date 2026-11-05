// GlobalScope::Impl, phase 2, lookup: part of resolver.cpp, included in the class's body there (a file of its
// own since 2026-11-05, moved without a change; docs/design.md, "Compilation model").
  // ---------------- phase 2: lookup ----------------

  // Declaration order across the program: the driver's file order (the library first), then the
  // position in the file.
  static bool declaredBefore(const Decl* a, const Decl* b)
  {
    uint32_t fa = a && a->file ? a->file->index : 0, fb = b && b->file ? b->file->index : 0;
    if (fa != fb)
      return fa < fb;
    return (a ? a->token : 0) < (b ? b->token : 0);
  }

  // Puts what the parallel index phase appended in declaration order: the entries of each name
  // (so that pick() and overload ties do not depend on which thread got there first), the
  // using-directives, `using enum`s and out-of-line members.
  void sortEntries(Scope* scope, std::unordered_set<Scope*>& seen)
  {
    if (!seen.insert(scope).second)
      return;
    for (uint32_t i = 0; i < scope->shardCount; ++i)
      for (auto& [symbol, entries] : scope->shards[i].map)
      {
        if (entries.size() > 1)
          std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return declaredBefore(a.decl, b.decl); });
        if (entries.size() > 1 && scope->kind == ScopeKind::Namespace)
        {
          // `extern T name;` and `T name;` in one namespace are one variable: the declaration's
          // definition is the other (definitionOf), so that both name one global and the
          // declaration is not taken for a C library's variable.
          VariableDecl* definition = nullptr;
          for (const Entry& e : entries)
            if (!definition && e.decl && e.decl->kind == NodeKind::VariableDecl && !(e.decl->flags & DF_Extern))
              definition = static_cast<VariableDecl*>(e.decl);
          if (definition)
            for (const Entry& e : entries)
              if (e.decl && e.decl->kind == NodeKind::VariableDecl && (e.decl->flags & DF_Extern) && !static_cast<VariableDecl*>(e.decl)->init)
                variableDefinitions[e.decl] = definition;
          // `extern "C" TValue* index2addr(lua_State*, int);` then `TValue* index2addr(...) {...}`:
          // a later declaration of a function declared extern "C" with the same parameter types
          // (finishIndex compares them) is that function, with its C linkage (as C++ has it). So is
          // an earlier one: `double trio_nan();` then `extern "C" double trio_nan() {...}` (trio
          // converted as C, where nothing has a linkage) is one function, C's, where C++ calls two
          // linkages of one function ill-formed ([dcl.link]/5); calls to the prototype named a
          // mangled function nothing defined.
          for (const Entry& c : entries)
            if (c.decl && c.decl->kind == NodeKind::FunctionDecl && (c.decl->flags & DF_ExternC))
              for (const Entry& e : entries)
                if (e.decl && e.decl != c.decl && e.decl->kind == NodeKind::FunctionDecl && !(e.decl->flags & DF_ExternC) &&
                    e.decl->templateParams.empty() &&
                    static_cast<FunctionDecl*>(e.decl)->params.size() == static_cast<FunctionDecl*>(c.decl)->params.size())
                  externCRedeclarations.push_back({static_cast<FunctionDecl*>(c.decl), static_cast<FunctionDecl*>(e.decl)});
        }
        if (scope->kind == ScopeKind::Namespace)
          for (const Entry& e : entries)
            if (e.decl && e.decl->kind == NodeKind::FunctionDecl && e.decl->templateParams.empty() && isCFunction(static_cast<FunctionDecl*>(e.decl)))
              cFunctions[symbol].push_back(static_cast<FunctionDecl*>(e.decl));
        bool specialized = false; // a class template's specialisations among them: its instances may be one (resolveSpecialization)
        for (const Entry& e : entries)
        {
          specialized = specialized || (e.decl && e.decl->kind == NodeKind::ClassDecl && (e.decl->flags & DF_Specialization));
          if (e.scope && e.scope->kind != ScopeKind::Enum)
            sortEntries(e.scope, seen);
        }
        if (specialized)
          for (const Entry& e : entries)
            if (e.decl && e.decl->kind == NodeKind::ClassDecl && !(e.decl->flags & DF_Specialization))
              e.decl->attributes |= ATTR_Specialized;
      }
    std::stable_sort(scope->usingDirectives.begin(), scope->usingDirectives.end(), declaredBefore);
    std::stable_sort(scope->usingEnums.begin(), scope->usingEnums.end(), declaredBefore);
    std::stable_sort(scope->outOfLineMembers.begin(), scope->outOfLineMembers.end(), declaredBefore);
  }

  // Entries that open a scope are preferred (definition over forward declaration, primary
  // template over specialisation).
  static Entry pick(const std::vector<Entry>& entries)
  {
    const Entry* best = &entries[0];
    for (const Entry& e : entries)
    {
      bool primary = !(e.decl->flags & DF_Specialization);
      bool bestPrimary = !(best->decl->flags & DF_Specialization);
      if ((e.scope && !best->scope) || (e.scope && best->scope && primary && !bestPrimary))
        best = &e;
    }
    return *best;
  }

  // The private names (design.md 15) of the file a lookup from context is made for: that of the
  // innermost private scope, class or enum around context, else the thread's (lookupFile: a
  // namespace's scope is every file's).
  Scope* privateScopeFor(Scope* context)
  {
    const ParsedFile* file = nullptr;
    for (Scope* s = context; !file && s; s = s->parent)
      if (s->privateTo)
        file = s->privateTo;
      else if (s->owner && s->owner->kind != NodeKind::NamespaceDecl)
        file = s->owner->file;
    if (!file)
      file = lookupFile;
    return file ? file->privateScope : nullptr;
  }

  // Another file than from's that declares symbol private (design.md 15), the first in file order,
  // for the error of naming it (null: none).
  const ParsedFile* filePrivateTo(uint32_t symbol, const ParsedFile* from)
  {
    for (auto& [file, own] : privateScopes)
      if (file != from && own->find(symbol))
        return file;
    return nullptr;
  }

  Scope* lexicalScopeOf(Decl* d)
  {
    for (Decl* p = d ? d->parent : nullptr; p; p = p->parent)
      if (p->ownScope)
        return p == p->file->libraryNamespace && p->file->privateScope ? p->file->privateScope : p->ownScope;
    // A top-level declaration of a file with private names is in the file's view of the global
    // scope: those names first (design.md 15), whichever file's walk asks. (A scoped library's file
    // sees its private names before the library's, design.md 16: the namespace above.)
    return d && d->file && d->file->privateScope ? d->file->privateScope : &root;
  }

  // Resolves a type to the scope it denotes (class/enum/namespace scope), following aliases.
  Scope* scopeOfType(Type* type, Scope* context, const LocalStack* locals, int depth)
  {
    if (!type || depth > 16)
      return nullptr;
    switch (type->kind)
    {
      case NodeKind::ConstType: return scopeOfType(static_cast<ConstType*>(type)->inner, context, locals, depth + 1);
      case NodeKind::PointerType: return scopeOfType(static_cast<PointerType*>(type)->pointee, context, locals, depth + 1);
      case NodeKind::LRefType: return scopeOfType(static_cast<LRefType*>(type)->referenced, context, locals, depth + 1);
      case NodeKind::RRefType: return scopeOfType(static_cast<RRefType*>(type)->referenced, context, locals, depth + 1);
      case NodeKind::NamedType:
      {
        const QualifiedName& name = static_cast<NamedType*>(type)->name;
        Lookup l = lookupQualified(name, context, locals, depth + 1);
        // `struct S : IntegralConstant<bool, true>`: arguments that pick a specialisation name
        // its scope, not the primary template's.
        if (l.decl && l.decl->kind == NodeKind::ClassDecl && !name.empty() && name.last().hasTemplateArgs && l.overloads)
          for (const Entry& e : *l.overloads)
            if (e.decl && e.decl->kind == NodeKind::ClassDecl && (e.decl->flags & DF_Specialization))
            {
              const Ty* t = typeOfTypeDecl(l.decl, l, name.last(), TypeContext{context, locals, nullptr}, depth + 1);
              if (t && t->kind == TyKind::Class && t->decl && t->decl->ownScope && !types.isDependent(t))
                return t->decl->ownScope;
              break;
            }
        return l.scope;
      }
      default:
        return nullptr;
    }
  }

  Scope* scopeOfEntry(const Entry& e, int depth)
  {
    if (e.scope)
      return e.scope;
    Decl* d = e.decl;
    if (d->kind == NodeKind::UsingAliasDecl)
      return scopeOfType(static_cast<UsingAliasDecl*>(d)->type, lexicalScopeOf(d), nullptr, depth + 1);
    if (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
      return scopeOfType(static_cast<VariableDecl*>(d)->type, lexicalScopeOf(d), nullptr, depth + 1);
    if (d->kind == NodeKind::UsingNameDecl)
    {
      Lookup l = lookupQualified(static_cast<UsingNameDecl*>(d)->name, lexicalScopeOf(d), nullptr, depth + 1);
      return l.scope;
    }
    if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl)
    {
      // Forward declaration: find the definition through the lexical scope.
      Scope* s = lexicalScopeOf(d);
      uint32_t symbol = d->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(d)->name.parts[0].symbol
                                                       : static_cast<EnumDecl*>(d)->name.parts[0].symbol;
      if (const std::vector<Entry>* entries = s->find(symbol))
      {
        Entry best = pick(*entries);
        if (best.scope)
          return best.scope;
      }
    }
    return nullptr;
  }

  void noteRelations(Scope* s)
  {
    std::lock_guard lock(relationScopesMutex);
    relationScopes.push_back(s);
  }

  void ensureBases(Scope* cls, int depth)
  {
    recordUse(USE_ScopeRelations, cls);
    int state = cls->baseState.load(std::memory_order_acquire);
    if (state == 2)
      return;
    if (state == 0 && cls->baseState.compare_exchange_strong(state, 1))
    {
      RecordInto into(&cls->relationDeps);
      noteRelations(cls);
      cls->baseResolver = std::this_thread::get_id();
      std::vector<Scope*> bases;
      LocalStack params;
      for (Decl* p : cls->templateParams)
        params.push(static_cast<TemplateParamDecl*>(p)->name, p);
      for (Type* t : cls->baseTypes)
        if (Scope* s = scopeOfType(t, cls->parent, &params, depth + 1))
          if (s != cls)
            bases.push_back(s);
      cls->baseScopes = std::move(bases);
      cls->baseState.store(2, std::memory_order_release);
      cls->baseState.notify_all();
      return;
    }
    // Another thread is resolving; if it is us (a cycle through aliases), give up on bases.
    if (cls->baseResolver == std::this_thread::get_id())
      return;
    for (int seen = cls->baseState.load(std::memory_order_acquire); seen != 2; seen = cls->baseState.load(std::memory_order_acquire))
      cls->baseState.wait(seen, std::memory_order_acquire); // blocks (C++20 atomic wait) instead of spinning on yield
  }

  void ensureUsing(Scope* ns, int depth)
  {
    recordUse(USE_ScopeRelations, ns);
    int state = ns->usingState.load(std::memory_order_acquire);
    if (state == 2)
      return;
    if (state == 0 && ns->usingState.compare_exchange_strong(state, 1))
    {
      RecordInto into(&ns->relationDeps);
      noteRelations(ns);
      std::vector<Scope*> scopes;
      for (UsingDirectiveDecl* u : ns->usingDirectives)
      {
        Lookup l = lookupQualified(u->name, lexicalScopeOf(u), nullptr, depth + 1);
        if (l.scope && l.scope != ns)
          scopes.push_back(l.scope);
      }
      ns->usingScopes = std::move(scopes);
      ns->usingState.store(2, std::memory_order_release);
      return;
    }
    // Being resolved elsewhere: proceed without using-directives rather than risk a deadlock.
  }

  void ensureUsingEnums(Scope* cls, int depth)
  {
    recordUse(USE_ScopeRelations, cls);
    int state = cls->usingEnumState.load(std::memory_order_acquire);
    if (state == 2)
      return;
    if (state == 0 && cls->usingEnumState.compare_exchange_strong(state, 1))
    {
      RecordInto into(&cls->relationDeps);
      noteRelations(cls);
      std::vector<Scope*> scopes;
      for (UsingNameDecl* u : cls->usingEnums)
      {
        Lookup l = lookupQualified(u->name, cls, nullptr, depth + 1);
        if (l.scope)
          scopes.push_back(l.scope);
      }
      cls->usingEnumScopes = std::move(scopes);
      cls->usingEnumState.store(2, std::memory_order_release);
    }
    // Otherwise being resolved: finishIndex resolves them all before any lookup that runs in
    // parallel, so only a lookup made while resolving them (the enum's name, in this scope) gets
    // here, and goes on without them.
  }

  // Whether a member is one of each object (a non-static data member or member function): reached
  // through two base subobjects, it is two members.
  static bool isInstanceMember(const Decl* d)
  {
    if (!d || (d->flags & DF_Static))
      return false;
    return d->kind == NodeKind::FunctionDecl || (d->kind == NodeKind::VariableDecl && !(d->flags & DF_Typedef));
  }

  // Looks a symbol up in one scope: its own entries, template parameters, using-enums, bases.
  bool lookupInScope(Scope* s, uint32_t symbol, Lookup& out, int depth, bool withUsing = true)
  {
    if (depth > 24)
      return false;
    if (const std::vector<Entry>* entries = s->find(symbol))
    {
      Entry e = pick(*entries);
      out.decl = e.decl;
      out.scope = e.scope ? e.scope : scopeOfEntry(e, depth + 1);
      out.found = s->kind == ScopeKind::Namespace ? Found::Global : Found::Member;
      out.overloads = entries;
      out.foundIn = s;
      return true;
    }
    if (Decl* a = s->findAbsent(symbol))
    {
      out.absent = a; // (the name is a false `when`'s here: no base's, no using's)
      out.foundIn = s;
      return false;
    }
    if (s->kind == ScopeKind::Class)
    {
      for (Decl* p : s->templateParams)
        if (static_cast<TemplateParamDecl*>(p)->name == symbol)
        {
          out.decl = p;
          out.found = Found::Dependent;
          return true;
        }
      // The injected class name: in a class, and in the classes derived from it, its own name
      // names it (`ToolTip::defaultLabelStyle` in a member of a class deriving agui::ToolTip).
      if (s->owner && s->owner->kind == NodeKind::ClassDecl && !(s->owner->flags & DF_Specialization))
        if (const QualifiedName& cn = static_cast<ClassDecl*>(s->owner)->name; !cn.empty() && cn.last().symbol == symbol)
        {
          out.decl = s->owner;
          out.scope = s;
          out.found = Found::Member;
          out.overloads = nullptr;
          out.foundIn = s;
          return true;
        }
      if (!s->usingEnums.empty())
      {
        ensureUsingEnums(s, depth);
        if (s->usingEnumState.load(std::memory_order_acquire) == 2) // not while another thread fills them
          for (Scope* e : s->usingEnumScopes)
            if (lookupInScope(e, symbol, out, depth + 1))
              return true;
      }
      if (!s->baseTypes.empty())
      {
        // Every base is searched ([class.member.lookup]): one member reached through two bases is
        // one name for a static member, a type or an enumerator; anything else two bases have is
        // ambiguous (ambiguousIn, reported where the name is used), never the first base's. (No
        // base is virtual, so two bases are two subobjects and neither's members dominate the
        // other's; hiding happens inside each base's own lookup.)
        ensureBases(s, depth);
        bool any = false;
        Lookup absentInBase;
        for (Scope* b : s->baseScopes)
        {
          Lookup r;
          if (!lookupInScope(b, symbol, r, depth + 1))
          {
            if (r.absent && !absentInBase.absent)
              absentInBase = r;
            continue;
          }
          if (!any)
          {
            out = r;
            any = true;
            continue;
          }
          // (operator=: every class declares a copy assignment of its own, implicitly if not
          // written, which hides the bases' ([class.copy.assign] 1): never ambiguous)
          if (out.ambiguousWith || r.found == Found::Dependent || out.found == Found::Dependent || symbol == opSymbols[size_t(P::Assign)])
            continue;
          if (r.foundIn == out.foundIn && r.decl == out.decl && !isInstanceMember(r.decl))
            continue;
          out.ambiguousWith = r.decl;
          out.ambiguousIn = r.foundIn;
        }
        if (any)
          return true;
        if (absentInBase.absent)
        {
          out.absent = absentInBase.absent;
          out.foundIn = absentInBase.foundIn;
          return false;
        }
      }
    }
    else if (s->kind == ScopeKind::Namespace)
    {
      // `namespace TileRenderLayer { enum class Enum {...}; using enum Enum; }`, then
      // `TileRenderLayer::GROUND_NATURAL`: the enumerators are the namespace's names.
      if (!s->usingEnums.empty())
      {
        ensureUsingEnums(s, depth);
        if (s->usingEnumState.load(std::memory_order_acquire) == 2)
          for (Scope* e : s->usingEnumScopes)
            if (lookupInScope(e, symbol, out, depth + 1))
              return true;
      }
      if (withUsing && !s->usingDirectives.empty())
      {
        ensureUsing(s, depth);
        for (Scope* u : s->usingScopes)
          if (lookupInScope(u, symbol, out, depth + 1, false))
            return true;
      }
    }
    return false;
  }

  // Whether a declaration can name a scope, as the part before `::` must: a namespace, a class,
  // an enumeration, an alias, a template parameter (C++ looks only at those there).
  static bool namesScope(const Decl* d)
  {
    if (!d)
      return false;
    switch (d->kind)
    {
      case NodeKind::NamespaceDecl: case NodeKind::ClassDecl: case NodeKind::EnumDecl: case NodeKind::UsingAliasDecl:
      case NodeKind::TemplateParamDecl:
        return true;
      case NodeKind::VariableDecl: return (d->flags & DF_Typedef) != 0;
      default: return false;
    }
  }

  // qualifierOnly: the name is followed by `::` (`thisThread::get_id()` with a local variable
  // thisThread in scope): variables and functions of the name are passed over.
  // A lookup the name of a false `when` declaration stopped (design.md 19): nothing found, and why.
  static Lookup absentStop(const Lookup& l)
  {
    Lookup out;
    out.absent = l.absent;
    out.foundIn = l.foundIn;
    return out;
  }

  Lookup lookupUnqualified(uint32_t symbol, Scope* context, const LocalStack* locals, int depth, bool qualifierOnly = false)
  {
    Lookup out;
    if (locals)
      for (size_t i = locals->items.size(); i-- > 0;)
      {
        const LocalStack::Item& item = locals->items[i];
        if (item.symbol != symbol || (qualifierOnly && !namesScope(item.decl)))
          continue;
        Decl* d = item.decl;
        out.decl = d;
        out.found = d->kind == NodeKind::TemplateParamDecl ? Found::Dependent : Found::Local;
        if (d->ownScope)
          out.scope = d->ownScope;
        return out;
      }
    // A scope's entry of the name, or (qualifierOnly) the one of them that names a scope.
    auto found = [&](Lookup& l)
    {
      if (!qualifierOnly || namesScope(l.decl))
        return true;
      if (l.overloads)
        for (const Entry& e : *l.overloads)
          if (namesScope(e.decl))
          {
            l.decl = e.decl;
            l.scope = e.scope ? e.scope : scopeOfEntry(e, depth + 1);
            return true;
          }
      l = Lookup{};
      return false;
    };
    // A function's `using namespace Helpers;` makes Helpers' names visible as if declared in the
    // nearest enclosing namespace: tried where the search first reaches a namespace.
    bool usingsTried = !locals;
    std::vector<std::pair<Scope*, Scope*>> nominated; // (the namespace their names are visible in, the nominated namespace)
    for (Scope* s = context; s; s = s->parent)
    {
      // The file's private names (design.md 15) hide the global ones of the same spelling (in a
      // scoped library's file, the library's: design.md 16).
      if (s == &root || s->library)
        if (Scope* own = privateScopeFor(context); own && own->parent == s)
        {
          if (lookupInScope(own, symbol, out, depth, false) && found(out))
            return out;
          if (out.absent)
            return absentStop(out);
        }
      if (!usingsTried && s->kind == ScopeKind::Namespace)
      {
        usingsTried = true;
        Lookup viaUsing;
        if (lookupInLocalUsings(symbol, *locals, viaUsing, depth) && found(viaUsing))
        {
          // Functions of the name in both places are one overload set (`twice(x)` with
          // ::twice(double) and Helpers::twice(int)).
          if (lookupInScope(s, symbol, out, depth) && found(out))
            return sameNameTogether(out, viaUsing, depth);
          return viaUsing;
        }
      }
      if (s->kind != ScopeKind::Namespace)
      {
        if (lookupInScope(s, symbol, out, depth) && found(out))
          return out;
        if (out.absent)
          return absentStop(out);
        continue;
      }
      // A namespace's using-directive makes the nominated namespace's names visible as if declared
      // in the nearest namespace enclosing both ([namespace.udir]): found there, beside its own
      // names (a name both declare is ambiguous, functions of it are one overload set), and hidden
      // by what the namespaces in between declare.
      if (!s->usingDirectives.empty())
      {
        ensureUsing(s, depth);
        for (Scope* u : s->usingScopes)
          nominated.push_back({enclosingBoth(s, u), u});
      }
      Lookup own;
      bool ownFound = lookupInScope(s, symbol, own, depth, false) && found(own);
      Lookup via;
      bool viaFound = false;
      for (auto& [at, u] : nominated)
      {
        Lookup one;
        if (at != s || !lookupInScope(u, symbol, one, depth + 1) || !found(one))
          continue;
        via = viaFound ? sameNameTogether(via, one, depth) : one;
        viaFound = true;
      }
      if (ownFound && viaFound)
        return sameNameTogether(own, via, depth);
      if (ownFound)
        return own;
      if (viaFound)
        return via;
      if (own.absent)
        return absentStop(own);
    }
    if (!usingsTried && lookupInLocalUsings(symbol, *locals, out, depth) && found(out))
      return out;
    return out;
  }

  // The nearest namespace enclosing both a and b (the global one at worst).
  static Scope* enclosingBoth(Scope* a, Scope* b)
  {
    for (Scope* x = a; x; x = x->parent)
      if (x->kind == ScopeKind::Namespace)
        for (Scope* y = b; y; y = y->parent)
          if (y == x)
            return x;
    Scope* root = a;
    while (root && root->parent)
      root = root->parent;
    return root;
  }

  // The entity a namespace's using-declaration names (`using Colors::Red;`), else d itself.
  Decl* throughUsing(Decl* d, int depth)
  {
    if (!d || d->kind != NodeKind::UsingNameDecl || static_cast<UsingNameDecl*>(d)->isEnum || depth > 8)
      return d;
    Lookup target = lookupQualified(static_cast<UsingNameDecl*>(d)->name, lexicalScopeOf(d), nullptr, depth + 1);
    return target.decl ? throughUsing(target.decl, depth + 1) : d;
  }

  // One name found twice at one level (a namespace's own declaration and a using-directive's, or
  // two directives'): the same entity (also when one of them is a using-declaration of it, `using
  // Colors::Red;` beside `using namespace Colors;`), or functions (one overload set), or ambiguous.
  Lookup sameNameTogether(const Lookup& a0, const Lookup& b0, int depth)
  {
    if (a0.decl == b0.decl || (a0.scope && a0.scope == b0.scope))
      return a0;
    if (Decl* entity = throughUsing(a0.decl, depth); entity && entity == throughUsing(b0.decl, depth))
      return a0;
    // (a using-declaration of functions: the functions it names)
    Lookup a = withNamespaceUsings(a0, depth);
    Lookup b = withNamespaceUsings(b0, depth);
    auto onlyFunctions = [](const Lookup& l)
    {
      if (!l.decl || l.decl->kind != NodeKind::FunctionDecl)
        return false;
      if (l.overloads)
        for (const Entry& e : *l.overloads)
          if (!e.decl || e.decl->kind != NodeKind::FunctionDecl)
            return false;
      return true;
    };
    if (onlyFunctions(a) && onlyFunctions(b))
      return mergeOverloads(a, b);
    Lookup out = a0;
    out.ambiguousWith = b0.decl;
    return out;
  }

  // A name in an alias's own definition, `using Option = Option<DataType>;` in a class deriving the
  // template: not yet the alias (C++ declares it after its type), but what the class's bases or
  // the enclosing scopes declare.
  Lookup lookupPastAlias(UsingAliasDecl* alias, uint32_t symbol, int depth)
  {
    Lookup out;
    Scope* s = lexicalScopeOf(alias);
    if (!s || depth > 24)
      return out;
    if (s->kind == ScopeKind::Class && !s->baseTypes.empty())
    {
      ensureBases(s, depth);
      for (Scope* b : s->baseScopes)
        if (lookupInScope(b, symbol, out, depth + 1))
          return out;
    }
    return lookupUnqualified(symbol, s->parent, nullptr, depth + 1);
  }

  // The namespaces argument-dependent lookup searches for an argument of type t: a class's or an
  // enumeration's innermost enclosing namespace (not the global one, which ordinary lookup
  // searches anyway), and those of a class instance's type arguments.
  void associatedNamespaces(const Ty* t, std::vector<Scope*>& out, int depth = 0)
  {
    if (!t || depth > 4)
      return;
    t = types.unqualified(types.stripRef(t));
    while (t && (t->kind == TyKind::Pointer || t->kind == TyKind::Array))
      t = t->inner ? types.unqualified(t->inner) : nullptr;
    if (!t || !t->decl || (t->kind != TyKind::Class && t->kind != TyKind::Enum))
      return;
    Scope* own = t->kind == TyKind::Class ? classScopeOf(t) : t->decl->ownScope;
    Scope* s = own ? own->parent : lexicalScopeOf(t->decl);
    while (s && s->kind != ScopeKind::Namespace)
      s = s->parent;
    if (s && s != &root && std::find(out.begin(), out.end(), s) == out.end())
      out.push_back(s);
    if (t->kind == TyKind::Class)
      for (const Ty* a : t->args)
        associatedNamespaces(a, out, depth + 1);
  }

  // The friend functions defined in a class (hidden friends: static members to the compiler, the
  // parser's DF_Static) named symbol, which no lookup finds but argument-dependent lookup through an
  // argument of the class (or of one deriving from it, or of an instance with it as an argument).
  // Kept per class definition and symbol; a compile server's build that changed the index makes
  // them again (forgetMergedOverloads).
  std::mutex hiddenFriendsMutex;
  std::map<std::pair<const Decl*, uint32_t>, std::unique_ptr<std::vector<Entry>>> hiddenFriendSets;
  std::vector<ClassDecl*> hiddenFriendsDecided; // the classes whose hiddenFriendState is set (forgetMergedOverloads)
  const std::vector<Entry>* hiddenFriendsOf(ClassDecl* def, uint32_t symbol)
  {
    recordName(symbol); // (a hidden friend's changed name is read by name: GlobalScope::changedNameOf)
    // Most classes define no friend function: said without the lock, which argument-dependent lookup
    // took for every class it visits (47 to 62% of the resolve phase's waits at -j 32, 2026-11-03).
    int8_t state = def->hiddenFriendState.load(std::memory_order_acquire);
    if (state == 0)
    {
      bool any = false;
      for (Decl* m : def->members)
        any = any || (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Friend) && (m->flags & DF_Static));
      state = any ? 2 : 1;
      if (def->hiddenFriendState.exchange(state, std::memory_order_acq_rel) == 0)
      {
        std::lock_guard lock(hiddenFriendsMutex); // (once per class)
        hiddenFriendsDecided.push_back(def);
      }
    }
    if (state == 1)
      return nullptr;
    std::lock_guard lock(hiddenFriendsMutex);
    auto [it, added] = hiddenFriendSets.try_emplace({def, symbol});
    if (added)
    {
      std::vector<Entry> found;
      for (Decl* m : def->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Friend) && (m->flags & DF_Static) &&
            functionSymbol(static_cast<FunctionDecl*>(m)->name) == symbol)
          found.push_back({m, nullptr});
      if (!found.empty())
        it->second = std::make_unique<std::vector<Entry>>(std::move(found));
    }
    return it->second.get();
  }

  // The hidden friends named symbol that argument-dependent lookup finds through an argument of
  // type t (its class, the class's bases, the classes of an instance's arguments), each set with
  // its class's template arguments.
  struct HiddenFriends
  {
    const std::vector<Entry>* entries;
    const Ty* cls;
  };
  void hiddenFriends(const Ty* t, uint32_t symbol, std::vector<HiddenFriends>& out, int depth = 0)
  {
    if (!t || depth > 4)
      return;
    t = types.unqualified(types.stripRef(t));
    while (t && (t->kind == TyKind::Pointer || t->kind == TyKind::Array))
      t = t->inner ? types.unqualified(t->inner) : nullptr;
    // An enumeration's associated class is the class it is a member of (NotificationData's `Flags
    // operator|` for its nested `enum class Flags`), a nested class's the one around it too (C++'s
    // [basic.lookup.argdep] 3), with the instance's arguments.
    auto enclosing = [&](const Decl* member)
    {
      if (member && member->parent && member->parent->kind == NodeKind::ClassDecl && !types.isDependent(t))
        hiddenFriends(types.classType(member->parent, t->args), symbol, out, depth + 1);
    };
    if (t && t->kind == TyKind::Enum && t->decl)
    {
      enclosing(t->decl);
      return;
    }
    ClassDecl* def = t && t->kind == TyKind::Class && !types.isDependent(t) ? classDefinition(t) : nullptr;
    if (!def)
      return;
    if (const std::vector<Entry>* entries = hiddenFriendsOf(def, symbol))
      if (std::none_of(out.begin(), out.end(), [&](const HiddenFriends& h) { return h.entries == entries && h.cls == t; }))
        out.push_back({entries, t});
    enclosing(def);
    std::vector<const Ty*> bases;
    directBases(t, def, bases);
    for (const Ty* b : bases)
      hiddenFriends(b, symbol, out, depth + 1);
    for (const Ty* a : t->args)
      hiddenFriends(a, symbol, out, depth + 1);
  }

  // The hidden friends named symbol the arguments find (hiddenFriends) as one lookup, and in subst
  // their classes' template arguments; the classes of two found sets being instances of one template
  // (`Box<int> == Box<long>` with Box's friend operator==), whose parameters would take two
  // arguments, is said in conflict and nothing is found.
  Lookup hiddenFriendLookup(uint32_t symbol, const std::vector<const Ty*>& argTypes, Subst& subst, std::string* conflict = nullptr)
  {
    Lookup out;
    std::vector<HiddenFriends> sets;
    for (const Ty* a : argTypes)
      hiddenFriends(a, symbol, sets);
    if (sets.empty())
      return out;
    for (const HiddenFriends& h : sets)
      for (const Subst::Pair& p : substFor(h.cls).pairs)
        if (const Ty* had = subst.find(p.param); !had)
          subst.pairs.push_back(p);
        else if (had != p.type)
        {
          if (conflict)
            for (const HiddenFriends& other : sets)
              if (other.cls != h.cls && substFor(other.cls).find(p.param) == had)
              {
                *conflict = "the friend functions " + std::string(symbols.name(symbol)) + " of two instances of one class template, " +
                            types.toString(other.cls, &symbols) + " and " + types.toString(h.cls, &symbols) + ", are not supported in one call";
                break;
              }
          subst = {};
          return out;
        }
    for (const HiddenFriends& h : sets)
    {
      Lookup one;
      one.found = Found::Global;
      one.decl = h.entries->front().decl;
      one.overloads = h.entries;
      Scope* own = classScopeOf(h.cls);
      for (Scope* s = own ? own->parent : nullptr; s; s = s->parent)
        if (s->kind == ScopeKind::Namespace)
        {
          one.foundIn = s; // (a friend is a function of the class's namespace, which no lookup there finds)
          break;
        }
      out = out.decl ? mergeOverloads(out, one) : one;
    }
    return out;
  }

  // Two lookups' function overloads as one set (kept for the run: a lookup's overloads outlive it);
  // `a` alone when either found something other than functions.
  std::mutex mergedOverloadsMutex;
  std::map<std::pair<const void*, const void*>, std::unique_ptr<std::vector<Entry>>> mergedOverloads;
  // A compile server's build that changed the index (step 4): the merged sets are made again from
  // the entries as they are now; the old ones stay allocated (a lookup result may still point at one).
  std::vector<std::unique_ptr<std::vector<Entry>>> retiredOverloads;
  void forgetMergedOverloads()
  {
    {
      std::lock_guard lock(mergedOverloadsMutex);
      for (auto& [key, set] : mergedOverloads)
        retiredOverloads.push_back(std::move(set));
      mergedOverloads.clear();
    }
    {
      std::lock_guard lock(hiddenFriendsMutex);
      for (auto& [key, set] : hiddenFriendSets)
        if (set)
          retiredOverloads.push_back(std::move(set));
      hiddenFriendSets.clear();
      for (ClassDecl* c : hiddenFriendsDecided)
        c->hiddenFriendState.store(0, std::memory_order_relaxed); // (an edit may have added or removed one)
      hiddenFriendsDecided.clear();
    }
    std::lock_guard lock(usingOverloadsMutex);
    for (auto& [key, set] : usingOverloads)
      retiredOverloads.push_back(std::move(set));
    usingOverloads.clear();
  }
  Lookup mergeOverloads(const Lookup& a, const Lookup& b)
  {
    auto functions = [](const Lookup& l)
    {
      if (!l.decl || l.decl->kind != NodeKind::FunctionDecl)
        return false;
      if (l.overloads)
        for (const Entry& e : *l.overloads)
          if (!e.decl || e.decl->kind != NodeKind::FunctionDecl)
            return false;
      return true;
    };
    if (!functions(a) || !functions(b) || !a.overloads || !b.overloads || a.overloads == b.overloads)
      return a;
    std::lock_guard lock(mergedOverloadsMutex);
    std::unique_ptr<std::vector<Entry>>& merged = mergedOverloads[{a.overloads, b.overloads}];
    if (!merged)
    {
      merged = std::make_unique<std::vector<Entry>>(*a.overloads);
      merged->insert(merged->end(), b.overloads->begin(), b.overloads->end());
    }
    Lookup out = a;
    out.overloads = merged.get();
    return out;
  }

  // A class and a function of one name in one scope: a C header's struct and function, which C keeps
  // apart (a struct's name is its tag: `struct stat` and stat(), statvfs), and a nested class and a
  // member function (or data member) of one class, which C++ allows. Where a name is a value (a
  // call's callee, an identifier, the operand of &, a member access), it is the function, or the
  // variable or enumerator, as C++ hides the class there ([basic.scope.hiding]/2); as a type it stays
  // the class (pick: no `struct` needed). At namespace scope only the imported headers': two of the
  // program's declarations of one name are the global clash, and an imported and a program's one
  // too, so kpp's own names never meet this there.
  static void preferValue(Lookup& l)
  {
    if (!l.decl || !l.overloads || (l.decl->kind != NodeKind::ClassDecl && l.decl->kind != NodeKind::EnumDecl))
      return;
    bool imported = l.decl->file && l.decl->file->isImported;
    const Decl* owner = l.decl->parent && l.decl->parent->kind == NodeKind::ClassDecl ? l.decl->parent : nullptr;
    if (!imported && !owner)
      return;
    for (const Entry& e : *l.overloads)
      if ((owner ? e.decl->parent == owner : e.decl->file && e.decl->file->isImported) &&
          (e.decl->kind == NodeKind::FunctionDecl || e.decl->kind == NodeKind::EnumeratorDecl ||
           (e.decl->kind == NodeKind::VariableDecl && !(e.decl->flags & DF_Typedef))))
      {
        l.decl = e.decl;
        l.scope = nullptr;
        return;
      }
  }

  // `using lib::size;` in namespace wrap beside wrap's own size(int) (Filesystem's `using
  // __fs::file_size;`): the functions the namespace's using-declarations of the name stand for and
  // its own, as one overload set (kept for the run); l itself when none of its entries is such a
  // using-declaration, or when one of them is no function.
  std::mutex usingOverloadsMutex;
  std::map<const void*, std::unique_ptr<std::vector<Entry>>> usingOverloads;
  Lookup withNamespaceUsings(const Lookup& l, int depth)
  {
    if (!l.overloads || !l.foundIn || l.foundIn->kind != ScopeKind::Namespace || depth > 8)
      return l;
    auto isUsing = [](const Decl* d) { return d && d->kind == NodeKind::UsingNameDecl && !static_cast<const UsingNameDecl*>(d)->isEnum; };
    if (std::none_of(l.overloads->begin(), l.overloads->end(), [&](const Entry& e) { return isUsing(e.decl); }))
      return l;
    std::vector<Entry> merged;
    for (const Entry& e : *l.overloads)
    {
      if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
        merged.push_back(e);
      else if (isUsing(e.decl))
      {
        Lookup target = lookupQualified(static_cast<UsingNameDecl*>(e.decl)->name, lexicalScopeOf(e.decl), nullptr, depth + 1);
        if (!target.decl || target.decl->kind != NodeKind::FunctionDecl)
          return l;
        if (target.overloads)
        {
          for (const Entry& te : *target.overloads)
            if (te.decl && te.decl->kind == NodeKind::FunctionDecl)
              merged.push_back(te);
        }
        else
          merged.push_back(Entry{target.decl, nullptr});
      }
      else
        return l;
    }
    if (merged.empty())
      return l;
    std::lock_guard lock(usingOverloadsMutex);
    std::unique_ptr<std::vector<Entry>>& kept = usingOverloads[l.overloads];
    if (!kept)
      kept = std::make_unique<std::vector<Entry>>(std::move(merged));
    Lookup out = l;
    out.decl = kept->front().decl;
    out.overloads = kept.get();
    return out;
  }

  // The using-directives among a body's locals (LocalStack::usingDirective items), innermost first.
  bool lookupInLocalUsings(uint32_t symbol, const LocalStack& locals, Lookup& out, int depth)
  {
    for (size_t i = locals.items.size(); i-- > 0;)
    {
      const LocalStack::Item& item = locals.items[i];
      if (item.symbol != LocalStack::usingDirective || !item.decl || item.decl->kind != NodeKind::UsingDirectiveDecl || depth > 16)
        continue;
      auto* u = static_cast<UsingDirectiveDecl*>(item.decl);
      Lookup ns = lookupQualified(u->name, lexicalScopeOf(u), nullptr, depth + 1);
      if (ns.scope && ns.scope->kind == ScopeKind::Namespace && lookupInScope(ns.scope, symbol, out, depth + 1))
        return true;
    }
    return false;
  }

  // Whether two template arguments are the same: the same type, or constants of the same value
  // (`IntegralConstant<bool, isSigned<T>>` folds to a bool true, the specialisation's written
  // `true` is typed by its parameter `T V`: the same argument).
  static bool sameArgument(const Ty* a, const Ty* b)
  {
    if (a == b)
      return true;
    return a && b && a->kind == TyKind::Value && b->kind == TyKind::Value && a->value == b->value && a->decl == b->decl;
  }

  // Among a class template's entries, its primary template: the declaration that is no
  // specialisation, its definition when it has one. Null when the program declares none.
  static Decl* primaryOf(const std::vector<Entry>& entries)
  {
    Decl* primary = nullptr;
    for (const Entry& e : entries)
      if (e.decl && e.decl->kind == NodeKind::ClassDecl && !(e.decl->flags & DF_Specialization))
        if (!primary || (static_cast<ClassDecl*>(e.decl)->isDefinition && !static_cast<ClassDecl*>(primary)->isDefinition))
          primary = e.decl;
    return primary;
  }

  // The primary template's parameter that template argument i is for: its pack takes the rest.
  static Decl* parameterFor(Decl* primary, size_t i)
  {
    if (!primary || primary->templateParams.empty())
      return nullptr;
    if (i < primary->templateParams.size())
      return primary->templateParams[i];
    Decl* last = primary->templateParams[primary->templateParams.size() - 1];
    return static_cast<TemplateParamDecl*>(last)->isPack ? last : nullptr;
  }

  static bool isPackParam(const Ty* t)
  {
    return t && t->kind == TyKind::Param && t->args.empty() && t->decl && t->decl->kind == NodeKind::TemplateParamDecl &&
           static_cast<TemplateParamDecl*>(t->decl)->isPack;
  }

  // A partial specialisation's patterns (`<T, T, R...>`) as types, its own parameters standing for
  // themselves; a pack expansion is its pack parameter. False when one does not convert.
  bool specializationPatterns(ClassDecl* c, Decl* primary, std::vector<const Ty*>& out, int depth)
  {
    const NamePart& part = c->name.last();
    LocalStack own;
    for (Decl* p : c->templateParams)
      own.push(static_cast<TemplateParamDecl*>(p)->name, p);
    TypeContext sctx{lexicalScopeOf(c), &own, nullptr, c->file ? c->file->tokens : nullptr, c->file ? c->file->text : std::string_view()};
    for (size_t i = 0; i < part.templateArgs.size(); ++i)
    {
      Node* a = part.templateArgs[i];
      const Ty* t = a && a->kind == NodeKind::PackType ? convertType(static_cast<PackType*>(a)->pattern, sctx, depth + 1)
                                                       : templateArgument(a, parameterFor(primary, i), sctx, depth);
      if (!t || !t->isKnown())
        return false;
      out.push_back(t);
    }
    bool expansion = !part.templateArgs.empty() && part.templateArgs[part.templateArgs.size() - 1] &&
                     part.templateArgs[part.templateArgs.size() - 1]->kind == NodeKind::PackType;
    if (!expansion)
      appendDefaultPatterns(primary, out, depth);
    return true;
  }

  // A specialisation written with fewer arguments than its primary has parameters
  // (`is_optional<Optional<T>>` of `template<typename T, typename Enable = void>`): the rest are
  // the primary's defaults, read with the arguments before them, as C++ completes it.
  void appendDefaultPatterns(Decl* primary, std::vector<const Ty*>& out, int depth)
  {
    Span<Decl*> params = primary ? effectiveParams(primary) : Span<Decl*>{};
    for (size_t i = out.size(); i < params.size(); ++i)
    {
      auto* p = static_cast<TemplateParamDecl*>(params[i]);
      if (!p->defaultArg || p->isPack)
        return;
      LocalStack own;
      Subst s;
      for (size_t k = 0; k < params.size(); ++k)
        own.push(static_cast<TemplateParamDecl*>(params[k])->name, params[k]);
      for (size_t k = 0; k < i; ++k)
        s.pairs.push_back({params[k], out[k]});
      TypeContext dctx{lexicalScopeOf(primary), &own, &s, primary->file ? primary->file->tokens : nullptr,
                       primary->file ? primary->file->text : std::string_view()};
      const Ty* t = templateArgument(p->defaultArg, p, dctx, depth + 1);
      if (!t || !t->isKnown())
        return;
      out.push_back(t);
    }
  }

  // Deduces partial specialisation c's parameters from the arguments: `<T, T, R...>` against
  // <B, B, C> makes T = B and R = {C}, against <C, A, B> it does not match. A pack expansion
  // among the patterns (the last one, as C++ requires) takes the rest of the arguments. An
  // argument that is itself a pack parameter (the patterns of another specialisation, when
  // ordering them) matches only a pack expansion. False when c does not match.
  bool matchPatterns(ClassDecl* c, Decl* primary, const std::vector<const Ty*>& args, std::vector<const Ty*>& deduced, int depth)
  {
    const NamePart& part = c->name.last();
    size_t written = part.templateArgs.size();
    bool expansion = written > 0 && part.templateArgs[written - 1] && part.templateArgs[written - 1]->kind == NodeKind::PackType;
    std::vector<const Ty*> patterns;
    if (!specializationPatterns(c, primary, patterns, depth))
      return false;
    size_t n = patterns.size(); // the written ones, then the primary's defaults
    if (expansion ? args.size() < n - 1 : args.size() != n)
      return false;
    Subst s;
    for (size_t i = 0; i < n; ++i)
    {
      const Ty* pattern = patterns[i];
      if (expansion && i == n - 1)
      {
        if (!isPackParam(pattern))
          return false; // `Ts*...`: a pattern around the pack, not deduced yet
        std::vector<const Ty*> rest(args.begin() + i, args.end());
        const Ty* pack = types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())});
        if (const Ty* bound = s.find(pattern->decl))
        {
          if (bound != pack)
            return false;
        }
        else
          s.pairs.push_back({pattern->decl, pack});
        break;
      }
      if (isPackParam(args[i]))
        return false;
      if (types.isDependent(pattern))
      {
        deduceArgument(pattern, args[i], s, 1);
        if (!sameArgument(substitute(pattern, s), args[i]))
          return false;
      }
      else if (!sameArgument(pattern, args[i]))
        return false;
    }
    for (Decl* p : c->templateParams)
    {
      const Ty* t = s.find(p);
      if (!t)
        return false;
      deduced.push_back(t);
    }
    return true;
  }

  // Whether partial specialisation a is at least as specialised as b: b's patterns match a's,
  // a's parameters standing for unique types (`<T, T, R...>` is, `<T, F, R...>` is not, either way).
  bool atLeastAsSpecialized(ClassDecl* a, ClassDecl* b, Decl* primary, int depth)
  {
    std::vector<const Ty*> patterns;
    if (!specializationPatterns(a, primary, patterns, depth))
      return false;
    std::vector<const Ty*> deduced;
    return matchPatterns(b, primary, patterns, deduced, depth);
  }

  bool argsDependent(const std::vector<const Ty*>& args)
  {
    for (const Ty* a : args)
      if (!a || !a->isKnown() || types.isDependent(a))
        return true;
    return false;
  }

  // Among the entries a class template's name resolved to, the specialisation for `args`: a full
  // one whose arguments are exactly `args` (its own class, without arguments), or else the most
  // specialised partial one (`template<typename T> struct Describe<T*>`) whose patterns deduce
  // from them (its class with the deduced arguments of its own parameters). A pack among `args`
  // stands for its elements. Null when there is none or an argument is still dependent.
  const Ty* matchSpecialization(const Lookup& l, const std::vector<const Ty*>& written, int depth)
  {
    if (!l.overloads || written.empty() || depth > 24)
      return nullptr;
    std::vector<const Ty*> args;
    for (const Ty* a : written)
    {
      if (!a || !a->isKnown() || types.isDependent(a))
        return nullptr;
      if (a->kind == TyKind::Pack)
        args.insert(args.end(), a->args.begin(), a->args.end());
      else
        args.push_back(a);
    }
    Decl* primary = primaryOf(*l.overloads);
    // `Table<char*>::kind` names the arguments it writes; the instance has the defaults too.
    appendDefaultPatterns(primary, args, depth);
    for (const Ty* a : args)
      if (!a || !a->isKnown() || types.isDependent(a))
        return nullptr;
    struct Candidate
    {
      ClassDecl* decl;
      std::vector<const Ty*> deduced;
    };
    std::vector<Candidate> partials;
    for (const Entry& e : *l.overloads)
    {
      if (!e.decl || e.decl->kind != NodeKind::ClassDecl || !(e.decl->flags & DF_Specialization))
        continue;
      auto* c = static_cast<ClassDecl*>(e.decl);
      if (!c->isDefinition || c->name.empty())
        continue;
      const NamePart& part = c->name.last();
      if (c->templateParams.empty())
      {
        // `template<> struct is_optional<int>`: the written arguments, then the primary's defaults.
        TypeContext sctx{lexicalScopeOf(c), nullptr, nullptr, c->file ? c->file->tokens : nullptr, c->file ? c->file->text : std::string_view()};
        std::vector<const Ty*> full;
        for (size_t i = 0; i < part.templateArgs.size(); ++i)
          full.push_back(templateArgument(part.templateArgs[i], parameterFor(primary, i), sctx, depth));
        appendDefaultPatterns(primary, full, depth);
        if (full.size() != args.size())
          continue;
        bool same = true;
        for (size_t i = 0; i < args.size() && same; ++i)
          same = sameArgument(full[i], args[i]);
        if (same)
          return types.classType(c, {});
        continue;
      }
      Candidate m{c, {}};
      if (matchPatterns(c, primary, args, m.deduced, depth))
        partials.push_back(std::move(m));
    }
    if (partials.empty())
      return nullptr;
    // The most specialised: at least as specialised as every other match (`<T, T, R...>` over
    // `<T, F, R...>` for <B, B, C>). When none is, the first (declaration order).
    size_t best = 0;
    if (partials.size() > 1)
      for (size_t i = 0; i < partials.size(); ++i)
      {
        bool all = true;
        for (size_t k = 0; k < partials.size() && all; ++k)
          all = k == i || atLeastAsSpecialized(partials[i].decl, partials[k].decl, primary, depth);
        if (all)
        {
          best = i;
          break;
        }
      }
    Candidate& m = partials[best];
    return types.classType(m.decl, Span<const Ty*>{m.deduced.data(), uint32_t(m.deduced.size())});
  }

  // A name as written, found from context: what lookupWritten finds, a using-declaration of an
  // enumerator taken for that enumerator (usingEnumerator).
  Lookup lookupQualified(const QualifiedName& name, Scope* context, const LocalStack* locals, int depth, const Subst* subst = nullptr)
  {
    Lookup l = lookupWritten(name, context, locals, depth, subst);
    if (Decl* en = usingEnumerator(l.decl, depth))
      l.decl = en, l.overloads = nullptr, l.scope = nullptr;
    return l;
  }

  // A lookup in s, and in the global scope (`::helper`) first in the private names of the file the
  // lookup from context is made for (design.md 15), which hide the global ones there.
  bool lookupInScopeOrPrivate(Scope* s, Scope* context, uint32_t symbol, Lookup& out, int depth)
  {
    if (s == &root || s->library)
      if (Scope* own = privateScopeFor(context); own && own->parent == s && lookupInScope(own, symbol, out, depth, false))
        return true;
    return lookupInScope(s, symbol, out, depth);
  }

  Lookup lookupWritten(const QualifiedName& name, Scope* context, const LocalStack* locals, int depth, const Subst* subst = nullptr)
  {
    Lookup out;
    if (name.empty() || depth > 24)
      return out;
    Scope* current = name.global ? &root : nullptr;
    const Ty* currentTy = nullptr; // the class current is the scope of, when its arguments are known
    if (name.typeQualifier)
    {
      // `decltype(this->inventories)::mapped_type`: the name inside the class the type names.
      const Ty* q = convertType(name.typeQualifier, TypeContext{context, locals, subst}, depth + 1);
      q = q ? types.unqualified(types.stripRef(q)) : nullptr;
      if (!q || !q->isKnown())
        return out;
      if (types.isDependent(q))
      {
        out.found = Found::Dependent;
        return out;
      }
      if (q->kind == TyKind::Class)
        currentTy = q;
      current = q->kind == TyKind::Class ? classScopeOf(q) : q->kind == TyKind::Enum && q->decl ? q->decl->ownScope : nullptr;
      if (!current)
        return out;
    }
    for (size_t i = 0; i < name.parts.size(); ++i)
    {
      const NamePart& part = name.parts[i];
      bool last = i + 1 == name.parts.size();
      if (part.special == 1)
      {
        // Destructor name: resolves to the class itself.
        if (!last)
          return {};
        if (current)
          lookupInScopeOrPrivate(current, context, part.symbol, out, depth);
        else
          out = lookupUnqualified(part.symbol, context, locals, depth);
        return out;
      }
      if (part.special == 2 && part.opToken < uint32_t(P::Count))
      {
        if (!last)
          return {};
        // (Not found is not found: `super::operator!=` of a class declaring none names no class.)
        uint32_t symbol = opSymbols[part.opToken];
        out = Lookup{};
        if (current)
          lookupInScopeOrPrivate(current, context, symbol, out, depth);
        else
          out = lookupUnqualified(symbol, context, locals, depth);
        return out;
      }
      if (part.special != 0 || part.symbol == UINT32_MAX)
      {
        // conversion operators and `decltype(...)` components: the class scope is the result.
        if (current)
        {
          out.found = current->kind == ScopeKind::Namespace ? Found::Global : Found::Member;
          out.decl = current->owner;
          out.scope = nullptr;
          return out;
        }
        return {};
      }
      Lookup step;
      if (current)
      {
        if (!lookupInScopeOrPrivate(current, context, part.symbol, step, depth))
          return {};
      }
      else
        step = lookupUnqualified(part.symbol, context, locals, depth, !last);
      if (step.found == Found::No)
        return {};
      if (step.found == Found::Dependent)
      {
        if (!last && subst && step.decl && step.decl->kind == NodeKind::TemplateParamDecl)
        {
          const Ty* sub = subst->find(step.decl);
          if (sub && sub->kind == TyKind::Class && sub->decl)
            if (Scope* inside = classScopeOf(sub))
            {
              current = inside; // `Policy::getCoordinate` with Policy = VerticalPolicy: the rest inside that class
              currentTy = sub;
              continue;
            }
          if (sub && sub->kind == TyKind::Enum && sub->decl && sub->decl->ownScope)
          {
            current = sub->decl->ownScope; // `E::values` with E = Direction: the enum's members
            currentTy = nullptr;
            continue;
          }
        }
        out = step;
        out.found = Found::Dependent;
        return out; // `T::type`: dependent on a template parameter.
      }
      if (last)
        return step;
      const Ty* stepTy = nullptr; // the class step names, when its arguments are known
      if (part.hasTemplateArgs && step.decl && step.decl->kind == NodeKind::ClassDecl)
      {
        // `Limits<int>::max`: the specialisation for these arguments, when there is one.
        std::vector<const Ty*> args;
        TypeContext actx{context, locals, subst};
        Decl* primary = step.overloads ? primaryOf(*step.overloads) : step.decl;
        for (size_t i = 0; i < part.templateArgs.size(); ++i)
          args.push_back(templateArgument(part.templateArgs[i], parameterFor(primary, i), actx, depth + 1)); // types, and constants for value parameters
        // `typename Nest<I - 1, T*>::Type` in a member of Nest's primary: the injected class name,
        // written with arguments, names the template, whose specialisations they may pick.
        Lookup entries = step;
        if (!entries.overloads && primary && (primary->attributes & ATTR_Specialized))
          if (Lookup home; lookupInScope(lexicalScopeOf(primary), part.symbol, home, depth + 1, false) && home.overloads)
          {
            entries = home;
            if (primary->kind == NodeKind::ClassDecl)
              normalizeValueArgs(primary->templateParams, args, primary, depth + 1);
          }
        if (const Ty* spec = matchSpecialization(entries, args, depth))
        {
          step.decl = spec->decl;
          step.scope = spec->decl->ownScope;
        }
        else if ((step.decl->flags & DF_Specialization) && !argsDependent(args))
        {
          // The name picked a specialisation for its scope, the primary template having no
          // definition (`template<typename...> struct IndexInTemplateList;`), and none matches
          // these arguments: an incomplete class, whose members are not found.
          step.decl = primary;
          step.scope = primary ? primary->ownScope : nullptr;
          if (!primary)
            return {};
        }
        if (const Ty* t = typeOfTypeDecl(step.decl, step, part, actx, depth + 1); t && t->kind == TyKind::Class && !types.isDependent(t))
          stepTy = t;
      }
      else if (step.scope && step.scope->kind == ScopeKind::Class && step.scope->owner && effectiveParams(step.scope->owner).empty())
        stepTy = types.classType(step.scope->owner, {});
      else if (step.scope && step.decl && step.decl->kind == NodeKind::UsingAliasDecl && step.decl->templateParams.empty())
      {
        // `using HorizontalScrollBar = ScrollBar<HorizontalPolicy>;` then
        // `HorizontalScrollBar::PolicyType::defaultStyle`: the instantiation the alias names, so
        // that a member alias of ScrollBar after it has HorizontalPolicy for Policy.
        auto* alias = static_cast<UsingAliasDecl*>(step.decl);
        bool local = step.found == Found::Local;
        const Ty* at = convertType(alias->type, TypeContext{local ? context : lexicalScopeOf(alias), local ? locals : nullptr, subst}, depth + 1);
        if (at && at->kind == TyKind::Class && at->decl && !at->args.empty() && !(at->decl->attributes & ATTR_Draft) && !types.isDependent(at) &&
            classScopeOf(at) == step.scope)
          stepTy = at;
      }
      if (!step.scope && step.decl && step.decl->kind == NodeKind::UsingAliasDecl && step.decl->templateParams.empty())
      {
        auto* alias = static_cast<UsingAliasDecl*>(step.decl);
        // The rest of the name inside the class or enum the alias stands for (`ResultType::Nothing`
        // with `using ResultType = R;` and R an enum).
        auto enterAliased = [&](const Ty* at)
        {
          if (!at || !at->decl || (at->decl->attributes & ATTR_Draft) || types.isDependent(at))
            return false;
          if (at->kind == TyKind::Class)
            if (Scope* inside = classScopeOf(at))
            {
              current = inside;
              currentTy = at;
              out = step;
              return true;
            }
          if (at->kind == TyKind::Enum && at->decl->ownScope)
          {
            current = at->decl->ownScope;
            currentTy = nullptr;
            out = step;
            return true;
          }
          return false;
        };
        // An alias a base class template declares, `ItemPrototype::IDType::IndexType` with
        // `using IDType = IDTypeParameter;` in Prototype<ItemID>: its type under that base's
        // arguments, then the rest of the name inside it.
        if (currentTy && step.foundIn && step.foundIn->kind == ScopeKind::Class)
        {
          Subst ms = memberSubst(currentTy, step.foundIn, depth);
          if (enterAliased(ms.empty() ? nullptr : declaredTypeOf(alias, &ms, depth + 1)))
            continue;
        }
        // The same alias inherited and named unqualified, `IDType::IndexType` in CollisionLayerPrototype
        // (no template) deriving from Prototype<CollisionLayerID>: under the arguments of the base it
        // was found in.
        if (!currentTy && i == 0 && step.found == Found::Member)
        {
          Subst bs = baseSubstForUnqualified(step, TypeContext{context, locals, subst}, depth);
          if (enterAliased(bs.empty() ? nullptr : declaredTypeOf(alias, &bs, depth + 1)))
            continue;
        }
        // `using X = T::IDType;` in a template body, or `using IDType = IDTypeParameter;` in the class
        // being instantiated, then `X::IndexType` / `IDType::PrototypeType`: the alias's type under the
        // instantiation's arguments (a member alias in its own class's scope).
        if (subst && (step.found == Found::Local || step.found == Found::Member))
        {
          bool local = step.found == Found::Local;
          if (enterAliased(convertType(alias->type, TypeContext{local ? context : lexicalScopeOf(alias), local ? locals : nullptr, subst}, depth + 1)))
            continue;
        }
        // `using HorizontalScrollBar = ScrollBar<HorizontalPolicy>;` then
        // `HorizontalScrollBar::PolicyType::defaultStyle`: the instantiation the alias names, so
        // that a member alias of ScrollBar after it has HorizontalPolicy for Policy.
        if (!subst)
        {
          bool local = step.found == Found::Local;
          if (enterAliased(convertType(alias->type, TypeContext{local ? context : lexicalScopeOf(alias), local ? locals : nullptr, nullptr}, depth + 1)))
            continue;
        }
        // `using Kind = Token::Special;` (a local alias, say), then `Kind::None`: the rest of the
        // name inside the type the alias stands for.
        if (Scope* inside = scopeOfType(alias->type, step.found == Found::Local ? context : lexicalScopeOf(alias), locals, depth + 1))
        {
          current = inside;
          currentTy = nullptr;
          out = step;
          continue;
        }
        // `using IDType = IDTypeParameter;` and then `IDType::PrototypeType` in the class template:
        // dependent on the parameter the alias names, the same type as `IDTypeParameter::PrototypeType`
        // (which an out-of-line definition may write instead).
        if (!subst && step.found != Found::Local && alias->type && alias->type->kind == NodeKind::NamedType)
        {
          Lookup target = lookupQualified(static_cast<NamedType*>(alias->type)->name, lexicalScopeOf(alias), nullptr, depth + 1);
          if (target.found == Found::Dependent && target.decl && target.decl->kind == NodeKind::TemplateParamDecl)
            return target;
        }
      }
      if (!step.scope)
      {
        // A variable/function used as a qualifier, or an alias we could not follow.
        return {};
      }
      current = step.scope;
      currentTy = stepTy;
      out = step;
    }
    return out;
  }

