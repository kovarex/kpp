// GlobalScope::Impl, phase 2, lookup (its declarations in Lookup.hpp, part of GlobalScopeImpl.hpp): a translation unit
// of its own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"

auto GlobalScope::Impl::declaredBefore(const Decl* a, const Decl* b) -> bool
{
  uint32_t fa = a && a->file ? a->file->index : 0, fb = b && b->file ? b->file->index : 0;
  if (fa != fb)
    return fa < fb;
  return (a ? a->token : 0) < (b ? b->token : 0);
}

auto GlobalScope::Impl::sortEntries(Scope* scope, std::unordered_set<Scope*>& seen) -> void
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

auto GlobalScope::Impl::pick(const std::vector<Entry>& entries) -> Entry
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

auto GlobalScope::Impl::privateScopeFor(Scope* context) -> Scope*
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

auto GlobalScope::Impl::filePrivateTo(uint32_t symbol, const ParsedFile* from) -> const ParsedFile*
{
  for (auto& [file, own] : privateScopes)
    if (file != from && own->find(symbol))
      return file;
  return nullptr;
}

auto GlobalScope::Impl::lexicalScopeOf(Decl* d) -> Scope*
{
  for (Decl* p = d ? d->parent : nullptr; p; p = p->parent)
    if (p->ownScope)
      return p == p->file->libraryNamespace && p->file->privateScope ? p->file->privateScope : p->ownScope;
  // A top-level declaration of a file with private names is in the file's view of the global
  // scope: those names first (design.md 15), whichever file's walk asks. (A scoped library's file
  // sees its private names before the library's, design.md 16: the namespace above.)
  return d && d->file && d->file->privateScope ? d->file->privateScope : &root;
}

auto GlobalScope::Impl::scopeOfType(Type* type, Scope* context, const LocalStack* locals, int depth) -> Scope*
{
  if (!type || depth > 16)
    return nullptr;
  switch (type->kind)
  {
    case NodeKind::ConstType: return scopeOfType(static_cast<ConstType*>(type)->inner, context, locals, depth + 1);
    case NodeKind::PointerType: return scopeOfType(static_cast<PointerType*>(type)->pointee, context, locals, depth + 1);
    case NodeKind::LRefType: return scopeOfType(static_cast<LRefType*>(type)->referenced, context, locals, depth + 1);
    case NodeKind::RRefType: return scopeOfType(static_cast<RRefType*>(type)->referenced, context, locals, depth + 1);
    // A pack of bases (`struct Many : Leaf<A>...`): the class template its elements are instances of.
    case NodeKind::PackType: return scopeOfType(static_cast<PackType*>(type)->pattern, context, locals, depth + 1);
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

auto GlobalScope::Impl::scopeOfEntry(const Entry& e, int depth) -> Scope*
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

auto GlobalScope::Impl::noteRelations(Scope* s) -> void
{
  std::lock_guard lock(relationScopesMutex);
  relationScopes.push_back(s);
}

auto GlobalScope::Impl::ensureBases(Scope* cls, int depth) -> void
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
    std::vector<uint8_t> dependent;
    LocalStack params;
    for (Decl* p : cls->templateParams)
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    LocalStack allParams = params; // (and an enclosing template's: a nested class's base `B<T>` of the outer T)
    if (cls->owner)
      pushEnclosingTemplateParams(cls->owner->parent, allParams);
    for (Type* t : cls->baseTypes)
      if (Scope* s = scopeOfType(t, cls->parent, &params, depth + 1))
        if (s != cls)
        {
          bases.push_back(s);
          const Ty* bt = allParams.items.empty() ? nullptr : convertType(t, TypeContext{cls->parent, &allParams, nullptr}, depth + 1);
          dependent.push_back(bt && types.isDependent(bt));
        }
    cls->baseScopes = std::move(bases);
    cls->baseScopeDependent = std::move(dependent);
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

auto GlobalScope::Impl::ensureUsing(Scope* ns, int depth) -> void
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

auto GlobalScope::Impl::ensureUsingEnums(Scope* cls, int depth) -> void
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

auto GlobalScope::Impl::isInstanceMember(const Decl* d) -> bool
{
  if (!d || (d->flags & DF_Static))
    return false;
  return d->kind == NodeKind::FunctionDecl || (d->kind == NodeKind::VariableDecl && !(d->flags & DF_Typedef));
}

auto GlobalScope::Impl::lookupInScope(Scope* s, uint32_t symbol, Lookup& out, int depth, bool withUsing, bool skipDependentBases) -> bool
{
  if (depth > maxNestedLevels)
    return false;
  if (const std::vector<Entry>* entries = s->find(symbol))
  {
    Entry e = pick(*entries);
    out.decl = e.decl;
    out.scope = e.scope ? e.scope : scopeOfEntry(e, depth + 1);
    out.found = s->kind == ScopeKind::Namespace ? Found::Global : Found::Member;
    out.overloads = entries;
    out.foundIn = s;
    if (!s->inlineScopes.empty())
      withInlineNamespaces(s, symbol, out, true, depth);
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
      for (size_t i = 0; i < s->baseScopes.size(); ++i)
      {
        Scope* b = s->baseScopes[i];
        if (skipDependentBases && i < s->baseScopeDependent.size() && s->baseScopeDependent[i])
          continue; // (an unqualified name: never a dependent base's member, [temp.dep] 3)
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
    // `Geo::version()` of `namespace Geo { inline namespace V2 { int version(); } }` ([namespace.def] 7;
    // until 2026-11-05 it did not resolve).
    if (!s->inlineScopes.empty() && withInlineNamespaces(s, symbol, out, false, depth))
      return true;
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

auto GlobalScope::Impl::withInlineNamespaces(Scope* s, uint32_t symbol, Lookup& out, bool found, int depth) -> bool
{
  for (Scope* in : s->inlineScopes)
  {
    Lookup r;
    if (depth > 16 || !lookupInScope(in, symbol, r, depth + 1, false))
      continue;
    out = found ? sameNameTogether(out, r, depth + 1) : r;
    found = true;
  }
  return found;
}

auto GlobalScope::Impl::namesScope(const Decl* d) -> bool
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

auto GlobalScope::Impl::absentStop(const Lookup& l) -> Lookup
{
  Lookup out;
  out.absent = l.absent;
  out.foundIn = l.foundIn;
  return out;
}

auto GlobalScope::Impl::lookupTypeName(const NamedType* t, Scope* context, const LocalStack* locals, int depth, const Subst* subst,
                                       const std::vector<Decl*>* elementPacks, uint32_t elementIndex) -> Lookup
{
  if (t->elaborated && t->name.parts.size() == 1 && !t->name.global && !t->name.parts[0].hasTemplateArgs)
    return lookupUnqualified(t->name.parts[0].symbol, context, locals, depth, true);
  // `This` in a class's own base clause (`class Widget : public Counter<This>`), which the indexer marked with the
  // class (markThisInBaseClause): the class, as the lookup inside it would have it (a class template's current
  // instance), and never what the lookup around the class finds. Around a nested class that is the outer class, which
  // the walk, member lookup through the bases and the evaluator took, and codegen's layout (which looks in the class)
  // did not: `Outer::Inner`'s `Pick<This>` was two types (until 2026-11-09, a silent wrong answer). Elsewhere `resolved`
  // is the class the walk found the same way.
  if (t->resolved && t->resolved->kind == NodeKind::ClassDecl && t->name.parts.size() == 1 && !t->name.global && t->name.parts[0].symbol == symThisType &&
      !t->name.parts[0].hasTemplateArgs)
    if (auto* c = static_cast<ClassDecl*>(t->resolved); c->ownScope && !(c->file && c->file->isImported))
    {
      Lookup l;
      l.decl = c;
      l.scope = c->ownScope;
      l.found = Found::Member;
      l.foundIn = l.scope;
      return l;
    }
  return lookupQualified(t->name, context, locals, depth, subst, elementPacks, elementIndex); // (`typename IsRef<Ts>::Type` in a pattern)
}

auto GlobalScope::Impl::lookupUnqualified(uint32_t symbol, Scope* context, const LocalStack* locals, int depth, bool qualifierOnly) -> Lookup
{
  Lookup out;
  // `This` in kpp code (design.md 23c): the class lexically around the code, as its injected name would be (a
  // class template's current instance, a base's member its base), or the enum around an enum's method. Nothing
  // may be named `This` there (Walker::checkNotThis); imported C keeps it a name.
  if (symbol == symThisType && !(lookupFile && lookupFile->isImported))
  {
    for (Scope* s = context; s; s = s->parent)
      if ((s->kind == ScopeKind::Class || s->kind == ScopeKind::Enum) && s->owner &&
          (s->owner->kind == NodeKind::ClassDecl || s->owner->kind == NodeKind::EnumDecl))
      {
        out.decl = s->owner;
        out.scope = s;
        out.found = Found::Member;
        out.foundIn = s;
        return out;
      }
    return out;
  }
  // The locals below `from` down to `to`, the innermost first.
  auto inLocals = [&](size_t from, size_t to)
  {
    for (size_t i = from; i-- > to;)
    {
      const LocalStack::Item& item = locals->items[i];
      if (item.symbol != symbol || (qualifierOnly && !namesScope(item.decl)))
        continue;
      Decl* d = item.decl;
      if (d->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(d)->isEnum)
      {
        // A block's using-declaration of functions (Walker::declareLocal): the overload set it
        // names, which hides the outer scopes' functions of the name in the block
        // ([namespace.udecl]; until 2026-11-05 the global function of the name was called).
        Lookup target = lookupQualified(static_cast<UsingNameDecl*>(d)->name, lexicalScopeOf(d), nullptr, depth + 1);
        if (!target.decl)
          continue;
        out = target;
        return true;
      }
      out = Lookup{}; // (nothing of a scope searched before)
      out.decl = d;
      out.found = d->kind == NodeKind::TemplateParamDecl ? Found::Dependent : Found::Local;
      if (d->ownScope)
        out.scope = d->ownScope;
      return true;
    }
    return false;
  };
  // In a member function of a class declared in a function body, the function's locals come after
  // the class's scope, its members and bases ([basic.lookup.unqual]): `int value = 3; class Local {
  // int value = 7; int get() { return value; } };` is the member (until 2026-11-05 the local, which
  // codegen built as a global). The locals of each such class's function are searched once the
  // search has passed the class's scope (LocalStack::classFrames); where the search does not pass
  // the innermost class's (a lookup from another context), every local comes first, as before.
  size_t frames = 0;
  if (locals && !locals->classFrames.empty())
    for (Scope* s = context; s && !frames; s = s->parent)
      if (s == locals->classFrames.back().classScope)
        frames = locals->classFrames.size();
  if (locals && inLocals(locals->items.size(), frames ? locals->classFrames[frames - 1].floor : 0))
    return out;
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
  for (Scope* s = context, *passed = nullptr; s; passed = s, s = s->parent)
  {
    // Past a local class's scope: the locals of the function it is declared in (down to the
    // floor of the class around that function, if it is a member function of a local class too).
    if (frames && passed == locals->classFrames[frames - 1].classScope)
    {
      size_t from = locals->classFrames[--frames].floor;
      if (inLocals(from, frames ? locals->classFrames[frames - 1].floor : 0))
        return out;
    }
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
      // A class around the name: its own members and its bases', but not those of a base that depends
      // on its template parameters, which C++ looks for where the template is written ([temp.dep] 3;
      // design.md 23c: `this->x` reaches them). (Until 2026-11-07 the primary template's member was
      // taken, ahead of the global C++ takes: J's silent wrong answer.)
      if (lookupInScope(s, symbol, out, depth, true, true) && found(out))
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
    if (s == &root && stdScope)
    {
      Lookup bridged = own;
      if (bridgedToStd(symbol, bridged, ownFound, depth) && found(bridged))
      {
        own = bridged;
        ownFound = true;
      }
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
  // (Classes the search never passed, which a scope chain without a namespace would leave: their
  // functions' locals all the same.)
  if (frames && inLocals(locals->classFrames[frames - 1].floor, 0))
    return out;
  if (!usingsTried && lookupInLocalUsings(symbol, *locals, out, depth) && found(out))
    return out;
  return out;
}

auto GlobalScope::Impl::enclosingBoth(Scope* a, Scope* b) -> Scope*
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

auto GlobalScope::Impl::throughUsing(Decl* d, int depth) -> Decl*
{
  if (!d || d->kind != NodeKind::UsingNameDecl || static_cast<UsingNameDecl*>(d)->isEnum || depth > 8)
    return d;
  Lookup target = lookupQualified(static_cast<UsingNameDecl*>(d)->name, lexicalScopeOf(d), nullptr, depth + 1);
  return target.decl ? throughUsing(target.decl, depth + 1) : d;
}

auto GlobalScope::Impl::sameNameTogether(const Lookup& a0, const Lookup& b0, int depth) -> Lookup
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

auto GlobalScope::Impl::lookupPastAlias(UsingAliasDecl* alias, uint32_t symbol, int depth) -> Lookup
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

auto GlobalScope::Impl::associatedNamespaces(const Ty* t, std::vector<Scope*>& out, int depth, std::vector<const Ty*>* visited) -> void
{
  if (!t || depth > maxNestedLevels)
    return;
  t = types.unqualified(types.stripRef(t));
  while (t && (t->kind == TyKind::Pointer || t->kind == TyKind::Array))
    t = t->inner ? types.unqualified(t->inner) : nullptr;
  if (!t || !t->decl || (t->kind != TyKind::Class && t->kind != TyKind::Enum))
    return;
  // (The associated classes are a graph, not a tree: `class Robot : Hook<Robot>` is its own base's argument.)
  std::vector<const Ty*> own;
  if (!visited)
    visited = &own;
  if (std::find(visited->begin(), visited->end(), t) != visited->end())
    return;
  visited->push_back(t);
  Scope* scope = t->kind == TyKind::Class ? classScopeOf(t) : t->decl->ownScope;
  Scope* s = scope ? scope->parent : lexicalScopeOf(t->decl);
  while (s && s->kind != ScopeKind::Namespace)
    s = s->parent;
  if (s && (s != &root || stdScope) && std::find(out.begin(), out.end(), s) == out.end()) // (the global scope: only a code in std misses it by ordinary lookup)
    out.push_back(s);
  if (t->kind == TyKind::Class)
  {
    for (const Ty* a : t->args)
      associatedNamespaces(a, out, depth + 1, visited);
    // A base class's namespace is associated as the class's own is ([basic.lookup.argdep] 2.2).
    if (ClassDecl* def = types.isDependent(t) ? nullptr : classDefinition(t))
    {
      std::vector<const Ty*> bases;
      directBases(t, def, bases);
      for (const Ty* b : bases)
        associatedNamespaces(b, out, depth + 1, visited);
    }
  }
}

auto GlobalScope::Impl::hiddenFriendsOf(ClassDecl* def, uint32_t symbol) -> const std::vector<Entry>*
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

auto GlobalScope::Impl::hiddenFriends(const Ty* t, uint32_t symbol, std::vector<HiddenFriends>& out, int depth, std::vector<const Ty*>* visited) -> void
{
  if (!t || depth > maxNestedLevels)
    return;
  t = types.unqualified(types.stripRef(t));
  while (t && (t->kind == TyKind::Pointer || t->kind == TyKind::Array))
    t = t->inner ? types.unqualified(t->inner) : nullptr;
  // (The associated classes are a graph, not a tree: `class Robot : Hook<Robot>` is its own base's argument.)
  std::vector<const Ty*> own;
  if (!visited)
    visited = &own;
  if (!t || std::find(visited->begin(), visited->end(), t) != visited->end())
    return;
  visited->push_back(t);
  // An enumeration's associated class is the class it is a member of (NotificationData's `Flags
  // operator|` for its nested `enum class Flags`), a nested class's the one around it too (C++'s
  // [basic.lookup.argdep] 3), with the instance's arguments.
  auto enclosing = [&](const Decl* member)
  {
    if (member && member->parent && member->parent->kind == NodeKind::ClassDecl && !types.isDependent(t))
      hiddenFriends(types.classType(member->parent, t->args), symbol, out, depth + 1, visited);
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
    hiddenFriends(b, symbol, out, depth + 1, visited);
  for (const Ty* a : t->args)
    hiddenFriends(a, symbol, out, depth + 1, visited);
}

auto GlobalScope::Impl::hiddenFriendLookup(uint32_t symbol, const std::vector<const Ty*>& argTypes, Subst& subst, std::string* conflict) -> Lookup
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

auto GlobalScope::Impl::forgetMergedOverloads() -> void
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

auto GlobalScope::Impl::mergeOverloads(const Lookup& a, const Lookup& b) -> Lookup
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

auto GlobalScope::Impl::preferValue(Lookup& l) -> void
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

auto GlobalScope::Impl::withNamespaceUsings(const Lookup& l, int depth) -> Lookup
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

auto GlobalScope::Impl::lookupInLocalUsings(uint32_t symbol, const LocalStack& locals, Lookup& out, int depth) -> bool
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

auto GlobalScope::Impl::sameArgument(const Ty* a, const Ty* b) -> bool
{
  if (a == b)
    return true;
  if (!a || !b || a->kind != TyKind::Value || b->kind != TyKind::Value || a->value != b->value || a->decl != b->decl)
    return false;
  // (a floating value's bits (TypeTable::floatBits) are one with another's of its type only: 0.0's are 0's)
  bool floatA = a->inner && a->inner->kind == TyKind::Float, floatB = b->inner && b->inner->kind == TyKind::Float;
  return floatA == floatB && (!floatA || a->inner->bits == b->inner->bits);
}

auto GlobalScope::Impl::primaryOf(const std::vector<Entry>& entries) -> Decl*
{
  Decl* primary = nullptr;
  for (const Entry& e : entries)
    if (e.decl && e.decl->kind == NodeKind::ClassDecl && !(e.decl->flags & DF_Specialization))
      if (!primary || (static_cast<ClassDecl*>(e.decl)->isDefinition && !static_cast<ClassDecl*>(primary)->isDefinition))
        primary = e.decl;
  return primary;
}

auto GlobalScope::Impl::parameterFor(Decl* primary, size_t i) -> Decl*
{
  if (!primary || primary->templateParams.empty())
    return nullptr;
  if (i < primary->templateParams.size())
    return primary->templateParams[i];
  Decl* last = primary->templateParams[primary->templateParams.size() - 1];
  return static_cast<TemplateParamDecl*>(last)->isPack ? last : nullptr;
}

auto GlobalScope::Impl::isPackParam(const Ty* t) -> bool
{
  return t && t->kind == TyKind::Param && t->args.empty() && t->decl && t->decl->kind == NodeKind::TemplateParamDecl &&
         static_cast<TemplateParamDecl*>(t->decl)->isPack;
}

auto GlobalScope::Impl::specializationPatterns(ClassDecl* c, Decl* primary, std::vector<const Ty*>& out, int depth) -> bool
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

auto GlobalScope::Impl::appendDefaultPatterns(Decl* primary, std::vector<const Ty*>& out, int depth, const Subst* enclosing) -> void
{
  // (The template's own parameters, as out has its own arguments, parameterFor's: a member
  // template's effective ones begin with the enclosing class template's, so that `Pick<char*>` of
  // `template<typename U, typename = void> struct Pick` in Wrapper<T> looked for U's default at
  // the second place, found none, and never matched `Pick<V*, void>`.)
  Span<Decl*> params = !primary ? Span<Decl*>{} : !primary->templateParams.empty() ? primary->templateParams : effectiveParams(primary);
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
    if (enclosing)
      for (const Subst::Pair& p : enclosing->pairs)
        s.pairs.push_back(p);
    TypeContext dctx{lexicalScopeOf(primary), &own, &s, primary->file ? primary->file->tokens : nullptr,
                     primary->file ? primary->file->text : std::string_view()};
    const Ty* t = templateArgument(p->defaultArg, p, dctx, depth + 1);
    if (!t || !t->isKnown())
      return;
    out.push_back(t);
  }
}

// The decltype types a template argument writes (a type, or an expression's: none), through every
// type and template argument list it holds: `void_t<decltype(static_cast<T*>(nullptr)->size())>`.
static void decltypesIn(Node* n, std::vector<DecltypeType*>& out, int depth = 0)
{
  if (!n || depth > 32)
    return;
  switch (n->kind)
  {
    case NodeKind::DecltypeType: out.push_back(static_cast<DecltypeType*>(n)); return;
    case NodeKind::NamedType:
      for (const NamePart& part : static_cast<NamedType*>(n)->name.parts)
        for (Node* a : part.templateArgs)
          decltypesIn(a, out, depth + 1);
      return;
    case NodeKind::PointerType: decltypesIn(static_cast<PointerType*>(n)->pointee, out, depth + 1); return;
    case NodeKind::LRefType: decltypesIn(static_cast<LRefType*>(n)->referenced, out, depth + 1); return;
    case NodeKind::RRefType: decltypesIn(static_cast<RRefType*>(n)->referenced, out, depth + 1); return;
    case NodeKind::ConstType: decltypesIn(static_cast<ConstType*>(n)->inner, out, depth + 1); return;
    case NodeKind::ArrayType: decltypesIn(static_cast<ArrayType*>(n)->element, out, depth + 1); return;
    case NodeKind::FunctionType: decltypesIn(static_cast<FunctionType*>(n)->result, out, depth + 1); return;
    case NodeKind::MemberPointerType:
      decltypesIn(static_cast<MemberPointerType*>(n)->classType, out, depth + 1);
      decltypesIn(static_cast<MemberPointerType*>(n)->pointee, out, depth + 1);
      return;
    case NodeKind::PackType: decltypesIn(static_cast<PackType*>(n)->pattern, out, depth + 1); return;
    default: return;
  }
}

auto GlobalScope::Impl::matchPatterns(ClassDecl* c, Decl* primary, const std::vector<const Ty*>& args, std::vector<const Ty*>& deduced, int depth) -> bool
{
  const NamePart& part = c->name.last();
  size_t written = part.templateArgs.size();
  bool expansion = written > 0 && part.templateArgs[written - 1] && part.templateArgs[written - 1]->kind == NodeKind::PackType;
  std::vector<const Ty*> patterns;
  if (!specializationPatterns(c, primary, patterns, depth))
    return false;
  size_t n = patterns.size(); // the written ones, then the primary's defaults
  // The primary's defaults that name the specialization's parameters (`int N = sizeof(U)` for
  // `N3<T, T>`) form only once those are deduced: fewer patterns than arguments, the rest checked
  // after the loop. Until 2026-11-07 such a specialization matched nothing (N3<int, int> took the
  // primary).
  bool defaultsLater = !expansion && args.size() > n;
  if (expansion ? args.size() < n - 1 : args.size() != n && !defaultsLater)
    return false;
  Subst s;
  for (size_t i = 0; i < n; ++i)
  {
    const Ty* pattern = patterns[i];
    if (expansion && i == n - 1)
    {
      if (!isPackParam(pattern))
      {
        // `V<Box<Ts>...>`, `V<Ts*...>`: each remaining argument matches the pattern, deducing one
        // element of each pack the pattern names ([temp.deduct.type] 9), as a call's `const
        // Vector<Args>&...` does (deduceParams). Until 2026-11-07 such a specialization matched
        // nothing: V<Box<int>, Box<char>> took the primary.
        std::vector<Decl*> packs;
        packParamsIn(pattern, packs);
        if (packs.empty())
          return false;
        auto isPack = [&](Decl* d) { return std::find(packs.begin(), packs.end(), d) != packs.end(); };
        std::vector<std::vector<const Ty*>> elems(packs.size());
        for (size_t j = i; j < args.size(); ++j)
        {
          if (isPackParam(args[j]))
            return false; // (partial ordering: another specialization's bare `Us...` is no `Box<Ts>`)
          Subst one;
          for (const Subst::Pair& q : s.pairs)
            if (!isPack(q.param))
              one.pairs.push_back(q);
          deduceArgument(pattern, args[j], one, 1);
          if (one.failed || !sameArgument(substitute(pattern, one), args[j]))
            return false;
          for (size_t k = 0; k < packs.size(); ++k)
          {
            const Ty* e = one.find(packs[k]);
            if (!e)
              return false;
            elems[k].push_back(e);
          }
          for (const Subst::Pair& q : one.pairs)
            if (!isPack(q.param) && !s.find(q.param))
              s.pairs.push_back(q); // (`Two<T, Ts>...`: T the same in every element, the first deducing it)
        }
        for (size_t k = 0; k < packs.size(); ++k)
        {
          const Ty* pack = types.pack(Span<const Ty*>{elems[k].data(), uint32_t(elems[k].size())});
          if (const Ty* bound = s.find(packs[k]))
          {
            if (bound != pack)
              return false;
          }
          else
            s.pairs.push_back({packs[k], pack});
        }
        break;
      }
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
      // Each argument deduces on its own, and a parameter two of them deduce must be the same type
      // from both ([temp.deduct.type] 2): `S<T, const T>` does not match S<const int, const int>
      // (T const int, then int). Until 2026-11-07 the first deduction stood and only the
      // substitution was compared, which a const already there absorbed.
      Subst one;
      deduceArgument(pattern, args[i], one, 1);
      // `S<T, const T>` given S<int(char), int(char)>: deducing `const T` from the function fails,
      // though T from the first argument substitutes back to it (const on a function is dropped).
      if (one.failed)
        return false;
      for (const Subst::Pair& q : one.pairs)
      {
        if (const Ty* had = s.find(q.param))
        {
          if (!sameArgument(had, q.type))
            return false;
        }
        else
          s.pairs.push_back(q);
      }
      if (!sameArgument(substitute(pattern, s), args[i]))
        return false;
    }
    else if (!sameArgument(pattern, args[i]))
      return false;
  }
  if (defaultsLater)
  {
    std::vector<const Ty*> full;
    for (const Ty* pattern : patterns)
    {
      const Ty* t = substitute(pattern, s);
      if (!t || !t->isKnown() || types.isDependent(t))
        return false;
      full.push_back(t);
    }
    appendDefaultPatterns(primary, full, depth);
    if (full.size() != args.size())
      return false;
    for (size_t i = n; i < args.size(); ++i)
      if (!sameArgument(full[i], args[i]))
        return false;
  }
  for (Decl* p : c->templateParams)
  {
    const Ty* t = s.find(p);
    if (!t)
      return false;
    deduced.push_back(t);
  }
  // C++'s substitution failure ([temp.class.spec.match] 2, [temp.deduct] 8): a decltype the
  // arguments write must form under the arguments deduced, or the specialization does not match.
  // `HasSize<T, void_t<decltype(static_cast<T*>(nullptr)->size())>>` (the detection idiom) made
  // its second pattern void, the alias dropping its argument, so that it matched every T: D,
  // 2026-11-06, HasSize<B>::value was true for a B without size(). Only for arguments all known
  // (an instance's, not another specialization's patterns in partial ordering).
  bool concrete = true;
  for (const Subst::Pair& q : s.pairs)
    concrete = concrete && q.type && q.type->isKnown() && !types.isDependent(q.type);
  if (concrete)
  {
    std::vector<DecltypeType*> decltypes;
    for (Node* a : part.templateArgs)
      decltypesIn(a, decltypes);
    if (!decltypes.empty())
    {
      LocalStack own;
      for (Decl* p : c->templateParams)
        own.push(static_cast<TemplateParamDecl*>(p)->name, p);
      TypeContext sctx{lexicalScopeOf(c), &own, &s, c->file ? c->file->tokens : nullptr, c->file ? c->file->text : std::string_view()};
      for (DecltypeType* d : decltypes)
        if (const Ty* t = convertType(d, sctx, depth + 1); !t || !t->isKnown() || types.isDependent(t) || mentionsUnknown(t))
          return false;
    }
  }
  return true;
}

// The number of arguments a partial specialization writes when the last is a pack expansion
// (`V<T*, Ts...>`: 2), else 0.
static size_t trailingExpansionAt(ClassDecl* c)
{
  const NamePart& part = c->name.last();
  size_t written = part.templateArgs.size();
  return written > 0 && part.templateArgs[written - 1] && part.templateArgs[written - 1]->kind == NodeKind::PackType ? written : 0;
}

auto GlobalScope::Impl::atLeastAsSpecialized(ClassDecl* a, ClassDecl* b, Decl* primary, int depth) -> bool
{
  std::vector<const Ty*> patterns;
  if (!specializationPatterns(a, primary, patterns, depth))
    return false;
  // a's last argument a pack expansion that nothing of b's corresponds to (`V<T*, Us...>` against
  // `V<T>`): it is ignored ([temp.deduct.type] 9), so `V<T*, Ts...>` is at least as specialized as
  // `V<T>` (and more: V<int**> takes it, as clang has it; g++ calls it ambiguous). Until 2026-11-08
  // neither was at least as specialized as the other and the first declared was taken.
  if (size_t at = trailingExpansionAt(a); at && at == patterns.size() && !trailingExpansionAt(b))
  {
    std::vector<const Ty*> own;
    if (specializationPatterns(b, primary, own, depth) && own.size() == at - 1)
    {
      patterns.pop_back();
      std::vector<const Ty*> deduced;
      return matchPatterns(b, primary, patterns, deduced, depth);
    }
  }
  // a's last argument a pack expansion around its packs (`V<Box<Us>...>`): its pattern stands for
  // any number of arguments, so only an expansion of b at the same place matches it (`V<Ts...>`,
  // `V<Box<Ts>...>`); b's fixed arguments there never do ([temp.deduct.type] 9: a P that is no
  // expansion against an A that is one fails), so `V<T, Ts...>` and `V<Box<Us>...>` stay unordered.
  if (size_t at = trailingExpansionAt(a); at && !isPackParam(patterns[at - 1]) && trailingExpansionAt(b) != at)
    return false;
  std::vector<const Ty*> deduced;
  return matchPatterns(b, primary, patterns, deduced, depth);
}

auto GlobalScope::Impl::argsDependent(const std::vector<const Ty*>& args) -> bool
{
  for (const Ty* a : args)
    if (!a || !a->isKnown() || types.isDependent(a))
      return true;
  return false;
}

auto GlobalScope::Impl::matchSpecialization(const Lookup& l, const std::vector<const Ty*>& written, int depth, std::vector<ClassDecl*>* tied,
                                             const Subst* enclosing) -> const Ty*
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
  appendDefaultPatterns(primary, args, depth, enclosing);
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
    if (!matchPatterns(c, primary, args, m.deduced, depth))
      continue;
    // Its constrained template parameters' constraints, for the deduced arguments ([temp.spec.partial.match]
    // 2: a partial specialisation whose constraints are not satisfied does not match; until 2026-11-06
    // they were never checked: `template<Floating T> struct Traits<T>` matched an int and a class).
    bool satisfied = true;
    Subst own;
    for (size_t i = 0; i < c->templateParams.size() && i < m.deduced.size(); ++i)
      own.pairs.push_back({c->templateParams[i], m.deduced[i]});
    for (Decl* tp : c->templateParams)
    {
      if (tp->kind != NodeKind::TemplateParamDecl)
        continue;
      std::optional<bool> holds = typeConstraintHolds(static_cast<TemplateParamDecl*>(tp), c, own, depth + 1);
      if (holds && !*holds)
        satisfied = false;
      else if (!holds)
      {
        const Ty* t = own.find(tp);
        if (t && t->isKnown() && !types.isDependent(t) && !mentionsUnknown(t))
        {
          reportCheckerError(c->file, tp->token, "the constraint on " + std::string(symbols.name(static_cast<TemplateParamDecl*>(tp)->name)) +
                                                     " of a partial specialization cannot be evaluated for " + types.toString(t, &symbols));
          satisfied = false;
        }
      }
    }
    // Its requires clause (`template<class T> requires Small<T> struct S<T>`), which C++ conjoins after
    // them ([temp.constr.decl] 3); until 2026-11-08 it was never checked: S<double> took it.
    if (satisfied && c->requiresClause)
    {
      std::optional<bool> holds = constraintValue(c->requiresClause, c, own);
      if (holds && !*holds)
        satisfied = false;
      else if (!holds && !argsDependent(m.deduced))
      {
        reportCheckerError(c->file, c->requiresClause->token, "the requires clause of a partial specialization of " + debugName(c->name.last().symbol) +
                                                                   " cannot be evaluated for its arguments");
        satisfied = false;
      }
    }
    if (satisfied)
      partials.push_back(std::move(m));
  }
  if (partials.empty())
    return nullptr;
  // At least as specialised: by the patterns, and where the patterns are the same both ways, by the
  // constraints, the one whose constraints subsume the other's ([temp.func.order] 6.2.2 as
  // [temp.spec.partial.order] applies it: `template<Signed T>` over `template<Integral T>`).
  auto atLeastAsSpecialized = [&](ClassDecl* a, ClassDecl* b, Decl* p, int d) -> bool
  {
    bool ab = this->atLeastAsSpecialized(a, b, p, d);
    if (!ab || !this->atLeastAsSpecialized(b, a, p, d))
      return ab;
    // Each at least as specialized as the other: the one without a trailing pack expansion, when
    // the other has one ([temp.func.order] 5 as clang and g++ apply it to partial specializations:
    // A<int> takes `A<T>` over `A<T, Ts...>`), then the constraints.
    bool aExpands = trailingExpansionAt(a) != 0;
    if (aExpands != (trailingExpansionAt(b) != 0))
      return !aExpands;
    return constraintsSubsume(a, b);
  };
  // The most specialised: at least as specialised as every other match (`<T, T, R...>` over
  // `<T, F, R...>` for <B, B, C>). When none is, the first (declaration order).
  size_t best = 0;
  bool found = partials.size() == 1;
  if (partials.size() > 1)
    for (size_t i = 0; i < partials.size(); ++i)
    {
      bool all = true;
      for (size_t k = 0; k < partials.size() && all; ++k)
        all = k == i || atLeastAsSpecialized(partials[i].decl, partials[k].decl, primary, depth);
      if (all)
      {
        best = i;
        found = true;
        break;
      }
    }
  if (!found && tied)
    for (size_t i = 0; i < partials.size(); ++i)
    {
      bool beaten = false;
      for (size_t k = 0; k < partials.size() && !beaten; ++k)
        beaten = k != i && atLeastAsSpecialized(partials[k].decl, partials[i].decl, primary, depth) &&
                 !atLeastAsSpecialized(partials[i].decl, partials[k].decl, primary, depth);
      if (!beaten)
        tied->push_back(partials[i].decl);
    }
  Candidate& m = partials[best];
  return types.classType(m.decl, Span<const Ty*>{m.deduced.data(), uint32_t(m.deduced.size())});
}

auto GlobalScope::Impl::lookupQualified(const QualifiedName& name, Scope* context, const LocalStack* locals, int depth, const Subst* subst,
                                        const std::vector<Decl*>* elementPacks, uint32_t elementIndex) -> Lookup
{
  Lookup l = lookupWritten(name, context, locals, depth, subst, elementPacks, elementIndex);
  if (Decl* en = usingEnumerator(l.decl, depth))
    l.decl = en, l.overloads = nullptr, l.scope = nullptr;
  // Required `this.` (design.md 23c): here, for every reader of a marked name, the walk, the evaluator, a template
  // argument or a constant the parser read as a type (`Box<limit>`, `sizeof(limit)`).
  return name.requiresThis ? bareNameOutside(name, l, context) : l;
}

auto GlobalScope::Impl::lookupInScopeOrPrivate(Scope* s, Scope* context, uint32_t symbol, Lookup& out, int depth) -> bool
{
  if (s == &root || s->library)
    if (Scope* own = privateScopeFor(context); own && own->parent == s && lookupInScope(own, symbol, out, depth, false))
      return true;
  return lookupInScope(s, symbol, out, depth);
}

auto GlobalScope::Impl::bridgedToStd(uint32_t symbol, Lookup& out, bool found, int depth) -> bool
{
  Lookup library;
  if (!stdScope || !lookupInScope(stdScope, symbol, library, depth))
    return found;
  if (!found)
    out = library;
  else if (out.decl && out.decl->kind == NodeKind::FunctionDecl && library.decl && library.decl->kind == NodeKind::FunctionDecl)
    out = sameNameTogether(out, library, depth);
  return true;
}

auto GlobalScope::Impl::moveBareSpecializationsToStd() -> void
{
  if (!stdScope)
    return;
  for (uint32_t i = 0; i < root.shardCount; ++i)
  {
    std::vector<uint32_t> emptied;
    for (auto& [symbol, entries] : root.shards[i].map)
    {
      auto isSpecialization = [](const Entry& e) { return e.decl && e.decl->kind == NodeKind::ClassDecl && (e.decl->flags & DF_Specialization); };
      if (std::none_of(entries.begin(), entries.end(), isSpecialization) || !stdScope->find(symbol) ||
          std::any_of(entries.begin(), entries.end(), [](const Entry& e) { return e.decl && e.decl->kind == NodeKind::ClassDecl && !(e.decl->flags & DF_Specialization); }))
        continue;
      for (const Entry& e : entries)
        if (isSpecialization(e))
          stdScope->add(symbol, e.decl, e.scope);
      entries.erase(std::remove_if(entries.begin(), entries.end(), isSpecialization), entries.end());
      if (entries.empty())
        emptied.push_back(symbol);
    }
    for (uint32_t symbol : emptied)
      root.shards[i].map.erase(symbol);
  }
}

auto GlobalScope::Impl::lookupWritten(const QualifiedName& name, Scope* context, const LocalStack* locals, int depth, const Subst* subst,
                                      const std::vector<Decl*>* elementPacks, uint32_t elementIndex) -> Lookup
{
  Lookup out;
  if (name.empty() || depth > 24)
    return out;
  // The name's own nodes (its type qualifier, its template arguments) as a pack expansion's pattern typed
  // for one element sees them: `(IsRef<Ts>::v + ...)` over <int&, int&> is IsRef<int&>'s v, a partial
  // specialization's, for each element (it took the primary's for every one: IsRef<Pack> matched none).
  TypeContext own{context, locals, subst};
  own.elementPacks = elementPacks;
  own.elementIndex = elementIndex;
  Scope* current = name.global ? &root : nullptr;
  const Ty* currentTy = nullptr; // the class current is the scope of, when its arguments are known
  if (name.typeQualifier)
  {
    // `decltype(this->inventories)::mapped_type`: the name inside the class the type names.
    const Ty* q = convertType(name.typeQualifier, own, depth + 1);
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
      lookupInScopeOrPrivate(current, context, part.symbol, step, depth);
      if (i == 0 && current == &root && stdScope)
        bridgedToStd(part.symbol, step, step.found != Found::No, depth);
      if (currentTy)
        lookupThroughBaseInstances(currentTy, part.symbol, step, depth); // `G<0>::type` with G<N> : E<N> and E<0> a specialisation
      if (step.found == Found::No)
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
        const Ty* sub = elementOf(step.decl, subst->find(step.decl), own); // (`Ts::value` in a pattern: the element's)
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
      const TypeContext& actx = own;
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
      Subst outer = currentTy && currentTy->kind == TyKind::Class ? substFor(currentTy) : Subst{}; // (Wrapper<int>::Pick3<int>: T is int)
      if (const Ty* spec = matchSpecialization(entries, args, depth, nullptr, outer.empty() ? nullptr : &outer))
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
    else if (subst && !part.hasTemplateArgs && step.scope && step.scope->kind == ScopeKind::Class && step.scope->owner &&
             step.decl == step.scope->owner && step.decl->kind == NodeKind::ClassDecl)
    {
      // `typename G::type` in a member of G<N>, instantiated: the injected class name is the
      // instance whose body this is, G<0>, so that its bases are the ones its arguments pick.
      if (const Ty* self = selfType(step.decl))
        if (const Ty* sub = types.unqualified(substitute(self, *subst, depth + 1)); sub && sub->kind == TyKind::Class && !types.isDependent(sub))
          stepTy = sub;
    }
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
