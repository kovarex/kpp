// GlobalScope's own methods (resolver.hpp), over GlobalScope::Impl (GlobalScopeImpl.hpp): a translation unit of its
// own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"

GlobalScope::GlobalScope(SymbolTable& symbols) : impl(new Impl(symbols))
{
  std::lock_guard lock(membersOutsideMutex);
  membersOutsideSink = &impl->membersReadOutside;
}

GlobalScope::~GlobalScope()
{
  Impl::instanceGeneration.fetch_add(1, std::memory_order_acq_rel); // (another GlobalScope's records may get these addresses)
  {
    std::lock_guard lock(membersOutsideMutex);
    if (membersOutsideSink == &impl->membersReadOutside)
      membersOutsideSink = nullptr;
  }
  delete impl;
}

void GlobalScope::indexFile(ParsedFile& file)
{
  // A file's private names (design.md 15) are a scope of their own, which the lookups made for the
  // file reach before the global one (privateScopeFor); every other declaration is global.
  // A scoped library's file (design.md 16) is as if its top level were in a namespace of the
  // library's name, which its top-level declarations get for their parent: the library's names are
  // NAME::name to everyone else, and bare inside it.
  Scope* top = &impl->root;
  if (file.libraryScope != UINT32_MAX)
  {
    if (!file.libraryNamespace)
    {
      NamespaceDecl* ns = file.arena.make<NamespaceDecl>();
      ns->kind = NodeKind::NamespaceDecl;
      ns->token = 0;
      ns->file = &file;
      ns->name = file.libraryScope;
      NamePart* part = file.arena.make<NamePart>();
      part->symbol = file.libraryScope;
      ns->nested.parts.data = part;
      ns->nested.parts.count = 1;
      file.libraryNamespace = ns;
    }
    NamespaceDecl* ns = file.libraryNamespace;
    ns->decls = file.decls;
    for (Decl* d : file.decls)
      d->parent = ns;
    top = impl->root.getOrCreateNamespace(file.libraryScope, ns);
    ns->ownScope = top;
    std::lock_guard lock(impl->deferredMutex);
    if (!top->library)
    {
      top->library = true; // (whichever namespace of the name came first: the program's is an error, topLevelClashes)
      impl->libraryScopes.push_back(top);
    }
    if (file.isLibrary)
      impl->stdScope = top;
  }
  else
  {
    if (!file.library.empty())
    {
      uint32_t name = impl->symbols.intern(file.library);
      std::lock_guard lock(impl->deferredMutex);
      impl->unscopedLibraries.insert(name);
    }
    std::vector<NamespaceDecl*> blocks;
    for (Decl* d : file.decls)
      if (d->kind == NodeKind::NamespaceDecl && static_cast<NamespaceDecl*>(d)->name != UINT32_MAX)
        blocks.push_back(static_cast<NamespaceDecl*>(d));
    if (!blocks.empty())
    {
      std::lock_guard lock(impl->deferredMutex);
      impl->programNamespaces.insert(impl->programNamespaces.end(), blocks.begin(), blocks.end());
    }
  }
  file.privateScope = nullptr;
  for (Decl* d : file.decls)
    if (d->filePrivate && !file.privateScope)
    {
      file.privateScope = new Scope(ScopeKind::Namespace, top, nullptr, 8);
      file.privateScope->privateTo = &file;
      std::lock_guard lock(impl->deferredMutex);
      impl->privateScopes.push_back({&file, file.privateScope});
    }
  for (Decl* d : file.decls)
    impl->indexDecl(d, d->filePrivate ? file.privateScope : top);
  if (!file.absentDecls.empty())
  {
    // The namespace-scope declarations a false `when` dropped (design.md 19): names lookups stop at.
    std::lock_guard lock(impl->deferredMutex);
    for (Decl* a : file.absentDecls)
      if (uint32_t symbol = impl->absentName(a); symbol != UINT32_MAX)
      {
        Scope* in = a->parent && a->parent->kind == NodeKind::NamespaceDecl && a->parent->ownScope ? a->parent->ownScope : top;
        in->absent.emplace(symbol, a);
      }
  }
  if (!file.castCandidates.empty())
  {
    std::lock_guard lock(impl->deferredMutex);
    impl->castFiles.push_back(&file);
  }
  if (!file.splitDefaults.empty())
  {
    std::lock_guard lock(impl->deferredMutex);
    impl->splitDefaultFiles.push_back(&file);
  }
}

void GlobalScope::finishIndex()
{
  // The index phase appended to these from several threads: in declaration order, what follows
  // is the same in every run.
  auto byDecl = [](const auto& a, const auto& b) { return Impl::declaredBefore(a.first, b.first); };
  std::stable_sort(impl->deferredQualified.begin(), impl->deferredQualified.end(), byDecl);
  std::stable_sort(impl->deferredFunctions.begin(), impl->deferredFunctions.end(), byDecl);
  std::stable_sort(impl->deferredVariables.begin(), impl->deferredVariables.end(), byDecl);
  // `struct A::B { ... }`: resolve A, then index B inside it. Repeat for nested chains.
  for (int round = 0; round < 4 && !impl->deferredQualified.empty(); ++round)
  {
    std::vector<std::pair<Decl*, Scope*>> pending;
    pending.swap(impl->deferredQualified);
    for (auto& [d, context] : pending)
    {
      FileOfLookups ofFile(d->file);
      const QualifiedName& name = d->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(d)->name
                                                                 : static_cast<EnumDecl*>(d)->name;
      QualifiedName qualifier = name;
      qualifier.parts.count -= 1;
      Lookup l = impl->lookupQualified(qualifier, context, nullptr, 0);
      if (!l.scope)
      {
        impl->deferredQualified.push_back({d, context});
        continue;
      }
      // Re-index as a simple name inside the found scope.
      QualifiedName simple;
      simple.parts.data = name.parts.data + (name.parts.size() - 1);
      simple.parts.count = 1;
      if (d->kind == NodeKind::ClassDecl)
        static_cast<ClassDecl*>(d)->name = simple;
      else
        static_cast<EnumDecl*>(d)->name = simple;
      impl->indexDecl(d, l.scope);
    }
  }
  // Attach out-of-line member function definitions to their class scope.
  std::vector<FunctionDecl*> attached; // to a class: each must match a declaration (checked below)
  impl->attachOutOfLine(attached);
  // Attach out-of-line static data member definitions to the declarations in their classes.
  for (auto& [v, context] : impl->deferredVariables)
  {
    FileOfLookups ofFile(v->file);
    QualifiedName qualifier = v->name;
    qualifier.parts.count -= 1;
    LocalStack params;
    for (Decl* p : v->templateParams)
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    Lookup l = impl->lookupQualified(qualifier, context, &params, 0);
    if (l.scope && l.scope->kind == ScopeKind::Class)
    {
      if (Scope* special = impl->fullSpecializationScope(qualifier, context, &params, v->file, v->token))
        l.scope = special; // `int Flag<true>::counter = 100;`: the specialisation's member
      else if (Scope* partial = impl->partialSpecializationScope(qualifier, l, context, v->templateParams, v->file))
        l.scope = partial; // `template<class T> int Box<T*>::made = 20;`: the partial specialisation's, never the primary's
    }
    if (l.scope && l.scope->kind == ScopeKind::Class)
    {
      // `template<> const uint32_t ElectricNetworkStatistics::smoothingTicks = 0;` (an instance of
      // a class template, through an alias or with its arguments): that instance's definition
      // only, not the template's, which every other instance keeps (variableDefinitionFor).
      bool instanceOnly = false;
      if ((v->flags & DF_Specialization) && v->templateParams.empty() && l.scope->owner && !Impl::effectiveParams(l.scope->owner).empty())
      {
        Impl::TokenScope tokenScope(v->file ? v->file->tokens : nullptr, v->file ? v->file->text : std::string_view());
        const Ty* written = impl->specializedClassOf(v, v->name);
        instanceOnly = written && !written->args.empty();
      }
      if (const std::vector<Entry>* entries = l.scope->find(v->name.last().symbol))
        for (const Entry& e : *entries)
          if (e.decl && e.decl->kind == NodeKind::VariableDecl)
          {
            if (instanceOnly)
              impl->variableSpecializations[e.decl].push_back(v);
            else
              impl->variableDefinitions[e.decl] = v;
            break;
          }
    }
    else if (l.scope && l.scope->kind == ScopeKind::Namespace)
    {
      // `int Config::limit = 5;` outside its namespace: the same as written inside it.
      QualifiedName simple;
      simple.parts.data = v->name.parts.data + (v->name.parts.size() - 1);
      simple.parts.count = 1;
      v->name = simple;
      if (l.scope->owner)
        v->parent = l.scope->owner;
      impl->indexDecl(v, l.scope);
    }
  }
  impl->deferredVariables.clear();
  impl->moveBareSpecializationsToStd();
  std::unordered_set<Scope*> seen;
  impl->sortEntries(&impl->root, seen);
  std::sort(impl->privateScopes.begin(), impl->privateScopes.end(), [](const auto& a, const auto& b) { return a.first->index < b.first->index; });
  std::sort(impl->libraryScopes.begin(), impl->libraryScopes.end(),
            [&](const Scope* a, const Scope* b) { return impl->symbols.name(a->name) < impl->symbols.name(b->name); });
  std::sort(impl->programNamespaces.begin(), impl->programNamespaces.end(),
            [](const NamespaceDecl* a, const NamespaceDecl* b) { return std::make_pair(a->file->index, a->token) < std::make_pair(b->file->index, b->token); });
  for (auto& [file, own] : impl->privateScopes)
    impl->sortEntries(own, seen);
  impl->collectTemplateParamNames(&impl->root);
  // `using enum E;` in classes and namespaces: resolved here, single-threaded, so that the
  // parallel lookups only read them.
  for (Scope* s : impl->scopesWithUsingEnums)
    impl->ensureUsingEnums(s, 0);
  impl->scopesWithUsingEnums.clear();
  // `using namespace X;` in namespaces likewise: resolved lazily, a lookup on another thread while
  // they were being resolved went on without them (a name found in one run and not the next).
  std::vector<Scope*> withUsings;
  for (Scope* s : seen)
    if (s->kind == ScopeKind::Namespace && !s->usingDirectives.empty())
      withUsings.push_back(s);
  std::sort(withUsings.begin(), withUsings.end(), [](Scope* a, Scope* b)
  {
    return a->owner && b->owner ? Impl::declaredBefore(a->owner, b->owner) : !a->owner && b->owner;
  });
  for (Scope* s : withUsings)
    impl->ensureUsing(s, 0);
  impl->breakBaseCycles(seen);
  // Deduction guides, by the class template each names: a function (template) of the guide's
  // parameters returning the class it gives, for class template argument deduction to try.
  std::stable_sort(impl->deferredGuides.begin(), impl->deferredGuides.end(), byDecl);
  for (auto& [g, context] : impl->deferredGuides)
  {
    FileOfLookups ofFile(g->file);
    Lookup l = impl->lookupQualified(g->name, context, nullptr, 0);
    Decl* cls = l.overloads ? Impl::primaryOf(*l.overloads) : l.decl;
    if (!cls || cls->kind != NodeKind::ClassDecl || cls->templateParams.empty() || !g->target)
      continue;
    auto f = std::make_unique<FunctionDecl>();
    f->kind = NodeKind::FunctionDecl;
    f->token = g->token;
    f->endToken = g->endToken;
    f->file = g->file;
    f->parent = g->parent;
    f->templateParams = g->templateParams;
    f->isTemplate = g->isTemplate;
    f->flags = g->flags & DF_Explicit;
    f->name = g->name;
    f->params = g->params;
    f->trailingReturn = g->target;
    f->requiresClause = g->requiresClause; // `template<class U> requires Small<U> G(U) -> G<char>;`
    impl->guidesOf[cls].push_back(f.get());
    impl->guideFunctions.push_back(std::move(f));
  }
  impl->deferredGuides.clear();
  // `constexpr Pair<int, Mode> five(a, m);`: an object when a and m are variables.
  std::stable_sort(impl->objectCandidates.begin(), impl->objectCandidates.end(), byDecl);
  for (auto& [f, context] : impl->objectCandidates)
  {
    FileOfLookups ofFile(f->file);
    impl->objectFromFunctionDeclaration(f, context);
  }
  impl->objectCandidates.clear();
  // `(HANDLE) -1`: a cast when HANDLE is a type (every type name is known now, no walk has begun).
  std::stable_sort(impl->castFiles.begin(), impl->castFiles.end(), [](ParsedFile* a, ParsedFile* b) { return a->index < b->index; });
  for (ParsedFile* file : impl->castFiles)
  {
    FileOfLookups ofFile(file);
    for (auto& [b, where] : file->castCandidates)
      impl->castFromBinary(b, where);
  }
  impl->castFiles.clear();
  // A later declaration of an extern "C" function with its parameter types is that function, with
  // C linkage; an overload (`float sqrtf2(float)` beside C's `double sqrtf2(double)`) is not.
  for (auto& [c, e] : impl->externCRedeclarations)
  {
    const Ty* ct = impl->declaredTypeOf(c, nullptr, 0);
    const Ty* et = impl->declaredTypeOf(e, nullptr, 0);
    if (ct && et && ct->isKnown() && ct->kind == TyKind::Function && et->kind == TyKind::Function && ct->args.size() == et->args.size())
    {
      bool same = true;
      for (size_t i = 0; i < ct->args.size(); ++i)
        same = same && impl->types.unqualified(ct->args[i]) == impl->types.unqualified(et->args[i]);
      if (same)
        e->flags |= DF_ExternC;
    }
  }
  impl->externCRedeclarations.clear();
  // A C function is one function whatever namespace declares it (C++ [dcl.link] 6), so two
  // declarations of one name with two types (a header's and the library's) are an error, never two
  // overloads a call chooses between.
  impl->checkCFunctionTypes();
  // An out-of-line member definition that matches no declaration of its class is an error, as in
  // C++: never a definition silently dropped (its calls would fail only at link time).
  for (FunctionDecl* f : attached)
  {
    if (Impl::isMemberSpecialization(f) || f->name.last().hasTemplateArgs || ((f->flags & DF_Specialization) && f->templateParams.empty()))
      continue; // `template<> R Button<RecipeID>::f()`, `template<> T* Pool::get()`: matched by their arguments
    Impl::TokenScope tokenScope(f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view());
    if (impl->declarationOfDefinition(f))
      continue;
    Decl* cls = impl->owningClassOf(f);
    if (!cls || cls->kind != NodeKind::ClassDecl)
      continue;
    // Constructors, destructors and conversions are no entries of the class's scope: its members.
    // (Not a read of the member list: the answer is the special members', whose edits a compile
    // server never builds from the kept program, so a data member's edit cannot change it.)
    const uint32_t special = DF_Constructor | DF_Destructor | DF_Conversion;
    bool declared = false;
    if (f->flags & special)
      for (Decl* m : static_cast<ClassDecl*>(cls)->members)
        if (!declared && m->kind == NodeKind::FunctionDecl && m != f && (m->flags & special) == (f->flags & special) &&
            !static_cast<FunctionDecl*>(m)->body)
          declared = (f->flags & DF_Destructor) ||
                     impl->typeAs(f, static_cast<FunctionDecl*>(m)) == impl->matchedType(static_cast<FunctionDecl*>(m));
    if (!declared)
      impl->reportCheckerError(f->file, f->token,
                               "the out-of-line definition of " + qualifiedName(f) + " matches no declaration in " + qualifiedName(cls));
  }
  std::stable_sort(impl->splitDefaultFiles.begin(), impl->splitDefaultFiles.end(), [](ParsedFile* a, ParsedFile* b) { return a->index < b->index; });
  for (ParsedFile* file : impl->splitDefaultFiles)
  {
    FileOfLookups ofFile(file);
    Impl::TokenScope tokenScope(file->tokens, file->text);
    for (FunctionDecl* f : file->splitDefaults)
      mergeDefaultArguments(f);
  }
  impl->splitDefaultFiles.clear();
}

// `int f(int a, int b = 2); int f(int a = 1, int b) { ... }`: C++ adds a declaration's default arguments
// to the ones before it. Here every declaration of the function, in the one file (a default's tokens are
// read in its function's), has them all; symbols are order-independent, so a call before the declaration
// that adds one sees it too (and C++ rejects that call). Until 2026-11-06 no declaration took `f()`.
void GlobalScope::mergeDefaultArguments(FunctionDecl* f)
{
  Decl* definition = definitionOf(f);
  std::vector<FunctionDecl*> declarations;
  auto add = [&](Decl* d)
  {
    if (d->kind != NodeKind::FunctionDecl || d->file != f->file || static_cast<FunctionDecl*>(d)->params.size() != f->params.size())
      return;
    if (definitionOf(d) == definition && std::find(declarations.begin(), declarations.end(), d) == declarations.end())
      declarations.push_back(static_cast<FunctionDecl*>(d));
  };
  add(f);
  add(definition);
  const uint32_t special = DF_Constructor | DF_Destructor | DF_Conversion;
  uint32_t symbol = impl->functionSymbol(f->name);
  if (Decl* cls = impl->owningClassOf(f); cls && cls->kind == NodeKind::ClassDecl)
  {
    for (Decl* m : static_cast<ClassDecl*>(cls)->members)
      if (m->kind == NodeKind::FunctionDecl && (m->flags & special) == (f->flags & special) &&
          ((f->flags & special) || impl->functionSymbol(static_cast<FunctionDecl*>(m)->name) == symbol))
        add(m);
  }
  else if (f->name.parts.size() == 1)
  {
    if (const std::vector<Entry>* entries = impl->lexicalScopeOf(f)->find(symbol))
      for (const Entry& e : *entries)
        add(e.decl);
  }
  // (As in C++ ([dcl.fct.default] 4): an instance's defaults are its declaration's, in terms of that
  // declaration's template parameters.)
  if (declarations.size() > 1 && (!f->templateParams.empty() || isTemplateContext(f)))
  {
    impl->reportCheckerError(f->file, f->token, "a redeclaration of the template " + qualifiedName(f) +
                                                    " adds a default argument: give them all on its first declaration");
    return;
  }
  for (size_t i = 0; i < f->params.size(); ++i)
  {
    Expr* value = nullptr;
    for (FunctionDecl* d : declarations)
      if (auto* p = static_cast<ParamDecl*>(d->params[i]); !value && p->defaultValue)
        value = p->defaultValue;
    for (FunctionDecl* d : declarations)
      if (auto* p = static_cast<ParamDecl*>(d->params[i]); value && !p->defaultValue)
      {
        p->defaultValue = value;
        p->inheritedDefault = true;
      }
  }
}

std::vector<GlobalScope::LayoutStaticAssert> GlobalScope::layoutStaticAsserts()
{
  std::lock_guard lock(impl->checkerErrorsMutex);
  std::vector<LayoutStaticAssert> out;
  std::unordered_set<std::string> seen; // an instance's assert met by several walks once
  for (const Impl::LayoutAssert& a : impl->layoutAsserts)
  {
    if (a.inFunction && !impl->emitted(a.inFunction))
      continue; // in a template instance's function the program does not use (functionEmitted)
    std::string key = std::to_string(reinterpret_cast<uintptr_t>(a.decl)) + "|" + a.instance;
    if (!seen.insert(key).second)
      continue;
    LayoutStaticAssert& l = out.emplace_back();
    l.decl = a.decl;
    for (const Subst::Pair& p : a.subst.pairs)
      l.args.pairs.push_back({p.param, p.type});
    l.instance = a.instance;
  }
  // (in a fixed order: checkInstanceStaticAsserts adds them from every thread)
  std::sort(out.begin(), out.end(), [](const LayoutStaticAssert& a, const LayoutStaticAssert& b)
  {
    const std::string& pa = a.decl && a.decl->file ? a.decl->file->path : std::string();
    const std::string& pb = b.decl && b.decl->file ? b.decl->file->path : std::string();
    if (pa != pb)
      return pa < pb;
    uint32_t ta = a.decl ? a.decl->token : 0, tb = b.decl ? b.decl->token : 0;
    if (ta != tb)
      return ta < tb;
    return a.instance < b.instance;
  });
  return out;
}

size_t GlobalScope::checkInstanceStaticAsserts(size_t from)
{
  // The static_asserts in the bodies of class templates, once per instance the program names (a
  // template's own walk leaves them to its instances).
  std::vector<const Ty*> instances = impl->types.classInstances(from);
  auto checkOne = [&](const Ty* cls)
  {
    if (!cls->decl || cls->decl->kind != NodeKind::ClassDecl || cls->isConst || cls->isVolatile || !cls->isKnown() ||
        impl->types.isDependent(cls) || Impl::mentionsUnknown(cls))
      return;
    // The primary's arguments that pick a specialisation (`Atomic<SharedPtr<Config>>`) instantiate
    // the specialisation's body, not the primary's: its own instance is walked by itself.
    if (impl->resolveSpecialization(cls, 0) != cls)
      return;
    auto* c = static_cast<ClassDecl*>(cls->decl);
    // Required `override` per instance (design.md 23b, Q7): an instance overrides through a member nothing
    // calls by name, which no walk reaches (a virtual call does not), so each instance says what its members
    // override, an error naming the instance. Reported outside any walk, as this loop's static_asserts are: a
    // compile server walks a file with such an error again from scratch (BodyGraft, unitlessErrorIn).
    if (c->file && !c->file->isImported)
    {
      Impl::TokenScope tokenScope(c->file->tokens, c->file->text);
      // Only an instance with a virtual function, its own or a base's, or a member that says `override` (of
      // nothing, a mixin over a plain base) has anything to check: most (Vector<int>) have neither, and the
      // check's overrideKey types each member (0.7 s of a game build on one thread, B's profile 2026-11-07).
      bool anySays = false;
      for (Decl* m : membersRead(c))
        anySays = anySays || (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Override | DF_Final)));
      bool check = anySays || impl->polymorphicClass(cls) == std::optional<bool>(true);
      for (Decl* m : membersRead(c))
        if (check && m->kind == NodeKind::FunctionDecl && m->templateParams.empty() && !(m->flags & (DF_Static | DF_Constructor)) &&
            (!(m->flags & DF_Destructor) || (m->flags & DF_Override))) // (a destructor need not say it, Q5, but one that does overrides)
        {
          bool says = (m->flags & (DF_Override | DF_Final)) != 0;
          if (says && !(m->flags & DF_Override))
            continue; // (`final` alone may end a new virtual function)
          std::optional<bool> overrides = impl->overridesVirtual(cls, impl->overrideKey(static_cast<FunctionDecl*>(m), cls), 0);
          if (!overrides || *overrides == says)
            continue;
          const NamePart& n = static_cast<FunctionDecl*>(m)->name.last();
          std::string name = (m->flags & DF_Destructor) ? std::string("the destructor") : n.special ? std::string("this operator") : impl->debugName(n.symbol);
          // (an instance whose base is its parameter overrides or not by that argument: a mixin over a plain base)
          uint32_t overrideToken = UINT32_MAX;
          std::string old = Impl::spelledTokens(c->file, m->token, Impl::declaratorEnd(c->file, m->token, &overrideToken));
          std::string instance = impl->types.toString(cls, &impl->symbols);
          if (says)
            impl->reportCheckerError(c->file, overrideToken != UINT32_MAX ? overrideToken : m->token,
                                     name + " says override and overrides no virtual function in " + instance + ": remove `override`");
          else
            impl->reportCheckerError(c->file, m->token, name + " overrides a virtual function in " + instance + ": write `" + old + " override`, not `" + old + "`");
        }
    }
    if (std::none_of(membersRead(c).begin(), membersRead(c).end(), [](Decl* m) { return m->kind == NodeKind::StaticAssertDecl; }))
      return; // (most have none: no substitution and context to build for them)
    Subst subst = impl->substFor(cls);
    LocalStack params;
    for (Decl* p : Impl::effectiveParams(c))
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    Impl::TypeContext ctx{c->ownScope ? c->ownScope : impl->lexicalScopeOf(c), &params, subst.empty() ? nullptr : &subst,
                          c->file ? c->file->tokens : nullptr, c->file ? c->file->text : std::string_view()};
    for (Decl* m : membersRead(c))
      if (m->kind == NodeKind::StaticAssertDecl)
        impl->checkStaticAssert(static_cast<StaticAssertDecl*>(m), ctx, impl->types.toString(cls, &impl->symbols));
  };
  // On every thread (the code generator's levels are done when it runs: 87,252 instances in the game, 1.7 s on
  // one thread, B's profile 2026-11-07): what an instance reports goes into sets and layoutAsserts, which
  // layoutStaticAsserts puts in a fixed order, so nothing depends on which thread took which instance.
  unsigned threads = unsigned(std::min<size_t>(std::max(1u, std::thread::hardware_concurrency()), std::max<size_t>(1, instances.size() / 256)));
  std::atomic<size_t> next{0};
  auto worker = [&]
  {
    for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < instances.size();)
      checkOne(instances[i]);
  };
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < threads; ++t)
    pool.emplace_back(worker);
  worker();
  for (std::thread& t : pool)
    t.join();
  return from + instances.size();
}

std::vector<std::string> GlobalScope::checkerErrors(size_t instancesChecked)
{
  checkInstanceStaticAsserts(instancesChecked);
  // By file, then line number, then text: as a build outside a compile server writes them, a
  // server's marked places too (plainDiagnostic, source.hpp).
  auto key = [](const std::string& marked)
  {
    std::string e = plainDiagnostic(marked);
    size_t error = e.find(": error: ");
    size_t colon = e.rfind(':', error == std::string::npos ? std::string::npos : error - 1);
    if (error == std::string::npos || colon == std::string::npos || colon == 0)
      return std::make_tuple(e, 0L, e);
    return std::make_tuple(e.substr(0, colon), std::strtol(e.c_str() + colon + 1, nullptr, 10), e);
  };
  std::lock_guard lock(impl->checkerErrorsMutex);
  std::unordered_set<std::string> all; // (rendered: two errors that read the same are one)
  std::unordered_set<std::string> messages;
  for (const auto& [e, units] : impl->checkerErrorSet)
  {
    std::string text = e.text();
    messages.insert(text);
    all.insert(std::move(text));
  }
  for (const auto& [f, errors] : impl->instanceAssertErrors)
    if (impl->emitted(f))
      for (const CheckerError& e : errors)
        all.insert(e.text());
  // A function argument's braces in template code, which its instances decide: one error where they agree on the fix
  // (no instance named, as the same at every thread count); where it differs by instance
  // (`ScriptObjectHelper<T>::SelectAction({a, b})`, a specialization's parameter in each) no one fix fits the line, so
  // none is offered, each instance naming its type (the editor's fix and C's converter wrote one instance's type).
  static const std::string bracedFix = "the braces of a function argument name their type: write `";
  std::map<std::pair<const ParsedFile*, uint32_t>, std::set<std::string>> bracedFixes;
  for (const auto& [f, errors] : impl->instanceNameErrors)
    if (impl->emitted(f))
      for (const CheckerError& e : errors)
        if (size_t at = e.message.find(bracedFix); at != std::string::npos)
          if (size_t end = e.message.find('`', at + bracedFix.size()); end != std::string::npos)
            bracedFixes[{e.file, e.token}].insert(e.message.substr(at + bracedFix.size(), end - at - bracedFix.size()));
  // (one the template's own walk reported at the definition already is that error, whichever walk
  // ran first)
  for (const auto& [f, errors] : impl->instanceNameErrors)
    if (impl->emitted(f))
      for (const CheckerError& e : errors)
      {
        CheckerError shown = e;
        if (auto fixes = bracedFixes.find({e.file, e.token}); fixes != bracedFixes.end() && fixes->second.size() == 1)
          if (size_t in = shown.message.find(" (in "); in != std::string::npos && shown.message.find(bracedFix) != std::string::npos)
            shown.message.erase(in); // (every instance's: one error)
        if (auto fixes = bracedFixes.find({e.file, e.token}); fixes != bracedFixes.end() && fixes->second.size() > 1)
          if (size_t at = shown.message.find(bracedFix); at != std::string::npos)
            if (size_t fixEnd = shown.message.find('`', at + bracedFix.size()); fixEnd != std::string::npos)
              if (size_t oldStart = shown.message.find(", not `", fixEnd); oldStart == fixEnd + 1)
                if (size_t oldEnd = shown.message.find('`', oldStart + 7); oldEnd != std::string::npos)
                {
                  std::string fix = shown.message.substr(at + bracedFix.size(), fixEnd - at - bracedFix.size());
                  shown.message = shown.message.substr(0, at) + "the braces of a function argument name their type, here " + fix +
                                  ", another in another instance, so no fix fits the line" + shown.message.substr(oldEnd + 1);
                }
        if (std::string text = shown.text(); !messages.count(text.substr(0, text.find(" (in "))))
          all.insert(std::move(text));
      }
  std::vector<std::string> out(all.begin(), all.end());
  std::sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) { return key(a) < key(b); });
  return out;
}

std::vector<std::string> GlobalScope::checkerWarnings()
{
  std::lock_guard lock(impl->checkerErrorsMutex);
  std::vector<std::pair<std::pair<std::string, uint32_t>, std::string>> keyed;
  for (const auto& [w, units] : impl->checkerWarningSet)
    keyed.push_back({{w.file ? w.file->path : std::string(), w.token}, Impl::locationOf(w.file, w.token) + ": warning: " + w.message});
  for (const auto& [f, warnings] : impl->instanceWarnings)
    if (impl->emitted(f))
      for (const CheckerError& w : warnings)
        keyed.push_back({{w.file ? w.file->path : std::string(), w.token}, Impl::locationOf(w.file, w.token) + ": warning: " + w.message});
  std::sort(keyed.begin(), keyed.end());
  std::vector<std::string> out;
  for (auto& [key, text] : keyed)
    if (out.empty() || out.back() != text)
      out.push_back(std::move(text));
  return out;
}

bool GlobalScope::hasCheckerErrors()
{
  std::lock_guard lock(impl->checkerErrorsMutex);
  return !impl->checkerErrorSet.empty();
}

void GlobalScope::functionEmitted(const FunctionDecl* f)
{
  std::lock_guard lock(impl->checkerErrorsMutex);
  impl->emittedFunctions.insert(f);
}

size_t GlobalScope::unusedFailedAsserts()
{
  std::lock_guard lock(impl->checkerErrorsMutex);
  size_t n = 0;
  for (const auto& [f, errors] : impl->instanceAssertErrors)
    if (!impl->emitted(f))
      n += errors.size();
  return n;
}

void GlobalScope::setCodeGenerated(bool generated)
{
  std::lock_guard lock(impl->checkerErrorsMutex);
  impl->codeGenerated = generated;
}

std::string GlobalScope::checkConstructorAccess(FunctionDecl* ctor, Decl* in, Node* at)
{
  if ((!impl->accessWarnings && !impl->accessErrors) || !ctor || !in || !at || !in->file)
    return {};
  std::vector<Scope*> context;
  impl->accessContext(in, context);
  std::string denied = impl->checkAccessUse(ctor, at, in, in->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(in) : nullptr, context, nullptr);
  return impl->accessErrors ? denied : std::string();
}

void GlobalScope::enableAccessErrors() { impl->accessErrors = true; }

void GlobalScope::setConverting(bool converting) { impl->converting = converting; }

void GlobalScope::setDebug(bool debug) { impl->debug = debug; }

void GlobalScope::setMsvcAbi(bool msvc) { impl->msvcAbi = msvc; }

void GlobalScope::setLP64(bool lp64, bool wcharUnsigned)
{
  impl->cLong64 = lp64;
  impl->types.setDataModel(lp64, wcharUnsigned);
  for (auto& [symbol, t] : impl->builtinNames)
    if (impl->symbols.name(symbol) == "wchar_t")
      t = impl->types.wcharType();
}

bool GlobalScope::unitlessErrorIn(const std::string& path)
{
  std::lock_guard lock(impl->checkerErrorsMutex);
  for (const auto& [e, units] : impl->checkerErrorSet)
    if (e.file && e.file->path == path && std::find(units.begin(), units.end(), nullptr) != units.end())
      return true;
  return false;
}

bool GlobalScope::checkerErrorIn(const FunctionDecl* f)
{
  if (!f)
    return false;
  std::lock_guard lock(impl->checkerErrorsMutex);
  if (impl->instanceNameErrors.count(f) || impl->instanceAssertErrors.count(f))
    return true; // (an instance's copy: its own errors)
  for (const auto& [e, units] : impl->checkerErrorSet)
    if (e.file == f->file && e.token >= f->token && e.token < f->endToken)
      return true;
  return false;
}

bool GlobalScope::checkerErrorWithin(const ParsedFile* file, uint32_t from, uint32_t to)
{
  std::lock_guard lock(impl->checkerErrorsMutex);
  for (const auto& [e, units] : impl->checkerErrorSet)
    if (e.file == file && e.token >= from && e.token <= to)
      return true;
  return false;
}

bool GlobalScope::undeclaredReported(const std::string& name)
{
  std::string said = "'" + name + "' is not declared";
  std::lock_guard lock(impl->checkerErrorsMutex);
  for (const auto& [e, units] : impl->checkerErrorSet)
    if (e.message.size() >= said.size() && e.message.compare(e.message.size() - said.size(), said.size(), said) == 0)
      return true;
  return false;
}


void GlobalScope::resolveAll(size_t count, unsigned threads, const std::function<void(size_t)>& resolveOne)
{
  Impl& im = *impl;
  im.queueMemberBodies.store(true);
  std::atomic<size_t> next{0};
  // Running work: files being resolved and tasks being checked; either may queue tasks. A
  // worker takes the next file while there is one, then tasks, and is done when nothing is
  // queued and nothing runs.
  im.memberTasksRunning = 0;
  auto finishOne = [&]
  {
    bool finished;
    {
      std::lock_guard lock(im.memberTaskMutex);
      --im.memberTasksRunning;
      finished = im.memberTasks.empty() && im.memberTasksRunning == 0 && next.load() >= count;
    }
    if (finished)
      im.memberTaskCv.notify_all(); // the waiting workers see that nothing is left
  };
  auto worker = [&]
  {
    for (;;)
    {
      {
        std::lock_guard lock(im.memberTaskMutex);
        ++im.memberTasksRunning; // counted before taking a file: a worker waiting for tasks must not see zero meanwhile
      }
      size_t i = next.fetch_add(1);
      if (i < count)
      {
        resolveOne(i);
        finishOne();
        continue;
      }
      finishOne();
      Impl::MemberTask task;
      {
        std::unique_lock lock(im.memberTaskMutex);
        im.memberTaskCv.wait(lock, [&] { return !im.memberTasks.empty() || im.memberTasksRunning == 0; });
        if (im.memberTasks.empty())
        {
          im.memberTaskCv.notify_all(); // none left and none running: every worker is done
          return;
        }
        task = im.memberTasks.front();
        im.memberTasks.pop_front();
        ++im.memberTasksRunning;
      }
      im.runMemberTask(task); // may queue more (each push wakes a waiting worker)
      finishOne();
    }
  };
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < threads; ++t)
    pool.emplace_back(worker);
  worker();
  for (auto& t : pool)
    t.join();
  im.queueMemberBodies.store(false);
  for (auto& [into, stats] : im.deferredStats)
    into->add(*stats);
  im.deferredStats.clear();
}

void GlobalScope::resolveFile(ParsedFile& file, const std::vector<Token>& tokens, std::string_view text, ResolveStats& stats)
{
  Impl::Walker walker(*impl, stats, tokens, text);
  walker.file = &file;
  FileOfLookups walkedFile(&file);
  if (file.libraryNamespace && file.libraryNamespace->ownScope)
    walker.scope = file.libraryNamespace->ownScope; // a scoped library's top level (design.md 16)
  if (impl->structRuleErrors)
    for (const Diagnostic& d : file.structRule)
      impl->reportCheckerError(&file, d.token, d.message);
  walker.walkDecls(file.decls);
  for (Decl* a : file.absentDecls) // (a false `when`'s dropped declarations: their condition's names resolve too)
    walker.walkWhenCondition(a);
  // `template struct Registry<int>;`: every member of the instance, used or not ([temp.explicit] 10),
  // where other instances check the members used (Walker::demandMember).
  for (auto& [cls, token] : explicitInstantiationsOf(file))
    walker.instantiateClass(cls, true);
}

TypeTable& GlobalScope::types() { return impl->types; }


FunctionDecl* GlobalScope::findFreeOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context)
{
  std::vector<const Ty*> argTypes{lhs};
  if (rhs)
    argTypes.push_back(rhs);
  FunctionDecl* f = findFree(impl->opSymbols[punctuator], argTypes, context);
  if (!f)
    return nullptr;
  // None when the built-in operator on the operands' conversions is the better (the resolver's
  // operatorFunction; an ambiguous one it reported).
  const Ty* friendOf = hiddenFriendClass(f, argTypes);
  Subst friendArgs = friendOf ? impl->substFor(friendOf) : Subst{};
  return impl->freeOperatorAgainstBuiltin(f, friendArgs.empty() ? nullptr : &friendArgs, P(punctuator), argTypes) == 0 ? nullptr : f;
}

FunctionDecl* GlobalScope::findFreeFunction(std::string_view name, const std::vector<const Ty*>& argTypes, Decl* context)
{
  return findFree(impl->symbols.intern(name), argTypes, context);
}

std::string GlobalScope::stdUsesText()
{
  std::set<std::string> lines;
  for (const Impl::StdUse& use : impl->stdUses)
  {
    char kind = use.kind;
    if (use.call)
    {
      // What the call took: a program's own function (a hidden friend, by argument-dependent lookup) or C's (clib/) stays bare.
      const Decl* chosen = use.call->resolved;
      if (chosen && chosen->kind == NodeKind::FunctionDecl && !(chosen->file && chosen->file->libraryScope != UINT32_MAX))
        continue;
      if (kind == 'M' && !chosen)
        kind = 'U'; // (decided per instance: for a reader)
    }
    lines.insert(std::string(1, kind) + "\t" + use.file->path + "\t" + std::to_string((*use.file->tokens)[use.token].offset) + "\t" +
                 std::string(impl->symbols.name(use.symbol)) + "\n");
  }
  std::string out;
  for (const std::string& line : lines)
    out += line;
  return out;
}

FunctionDecl* GlobalScope::libraryFunction(std::string_view name)
{
  return static_cast<FunctionDecl*>(impl->libraryDecl(name, NodeKind::FunctionDecl));
}

bool GlobalScope::isLibraryClass(const Ty* t, std::string_view name)
{
  return t && t->kind == TyKind::Class && t->decl && t->decl == impl->libraryDecl(name, NodeKind::ClassDecl);
}

FunctionDecl* GlobalScope::findFree(uint32_t symbol, const std::vector<const Ty*>& argTypes, Decl* context)
{
  // The arguments' classes' friend functions are candidates beside the namespaces' (hiddenFriendClass
  // gives the class instance of the one chosen).
  Subst friendArgs;
  Lookup friends = impl->hiddenFriendLookup(symbol, argTypes, friendArgs);
  auto choose = [&](const Lookup& look) -> FunctionDecl*
  {
    std::vector<FunctionDecl*> candidates;
    if (look.overloads)
    {
      for (const Entry& e : *look.overloads)
        if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
          candidates.push_back(static_cast<FunctionDecl*>(e.decl));
    }
    else
      candidates.push_back(static_cast<FunctionDecl*>(look.decl));
    bool ambiguous = false;
    return impl->chooseAmong(candidates, argTypes, friendArgs.empty() ? nullptr : &friendArgs, nullptr, ambiguous);
  };
  std::vector<Scope*> places;
  for (const Ty* a : argTypes)
    if (const Ty* t = a ? impl->types.stripRef(a) : nullptr; t && (t->kind == TyKind::Class || t->kind == TyKind::Enum) && t->decl)
    {
      places.push_back(impl->lexicalScopeOf(t->decl));
      impl->associatedNamespaces(t, places); // (its bases' and its arguments' too, [basic.lookup.argdep] 2)
    }
  places.push_back(impl->lexicalScopeOf(context));
  // Every place's functions ranked together, as C++ ranks argument-dependent lookup's and ordinary
  // lookup's: from each place the innermost scope declaring the name, and the friends (the walker's
  // operatorFunction does the same: language/operator_from_every_associated_namespace).
  std::vector<Entry> all;
  std::unordered_set<Decl*> seen;
  auto add = [&](const Lookup& look) {
    if (look.overloads)
    {
      for (const Entry& e : *look.overloads)
        if (e.decl && e.decl->kind == NodeKind::FunctionDecl && seen.insert(e.decl).second)
          all.push_back(e);
    }
    else if (look.decl && look.decl->kind == NodeKind::FunctionDecl && seen.insert(look.decl).second)
      all.push_back(Entry{look.decl, nullptr});
  };
  for (Scope* place : places)
    for (Scope* s = place; s; s = s->parent)
    {
      if (s->kind == ScopeKind::Class)
        continue; // members are not free operators, and do not hide them
      Lookup look;
      if (!impl->lookupInScope(s, symbol, look, 0))
        continue;
      if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
        add(look);
      break;
    }
  if (friends.decl)
    add(friends);
  if (all.empty())
    return nullptr;
  Lookup merged;
  merged.decl = all[0].decl;
  merged.overloads = &all;
  return choose(merged);
}

const Ty* GlobalScope::hiddenFriendClass(FunctionDecl* f, const std::vector<const Ty*>& argTypes)
{
  if (!f || !(f->flags & DF_Friend) || !(f->flags & DF_Static) || f->name.empty())
    return nullptr;
  std::vector<Impl::HiddenFriends> sets;
  uint32_t symbol = impl->functionSymbol(f->name);
  for (const Ty* a : argTypes)
    impl->hiddenFriends(a, symbol, sets);
  const Ty* out = nullptr;
  for (const Impl::HiddenFriends& h : sets)
    for (const Entry& e : *h.entries)
      if (e.decl == f)
      {
        if (out && out != h.cls)
          return nullptr; // (two instances: the resolver said so)
        out = h.cls;
      }
  return out;
}

const Ty* GlobalScope::scalarConversionOf(const Ty* classType) { return impl->scalarConversionOf(classType); }
bool GlobalScope::builtinOperands(uint32_t op, const Ty* l, const Ty* r, const Ty** lc, const Ty** rc, bool* ambiguousOut)
{
  return impl->builtinOperands(P(op), l, r, lc, rc, ambiguousOut);
}

std::vector<std::pair<const Ty*, uint32_t>> GlobalScope::explicitInstantiationsOf(ParsedFile& file)
{
  std::vector<std::pair<const Ty*, uint32_t>> out;
  for (auto& [named, where] : file.explicitInstantiations)
  {
    // Written at namespace scope: the name looked up from that namespace (the file's top level
    // when it is in none).
    Impl::DeclContext dc;
    dc.scope = where && where->ownScope ? where->ownScope : where ? impl->lexicalScopeOf(where) : &impl->root;
    Impl::TokenScope tokenScope(file.tokens, file.text);
    Impl::TypeContext ctx{dc.scope, &dc.params, nullptr, file.tokens, file.text};
    const Ty* t = impl->convertType(named, ctx, 0);
    t = t ? impl->types.unqualified(t) : nullptr;
    if (t && t->kind == TyKind::Class && t->decl && !t->args.empty() && !impl->types.isDependent(t))
      out.push_back({t, named->token});
  }
  return out;
}

void GlobalScope::traceLine(const std::string& fileSuffix, uint32_t line)
{
  impl->whyAt.push_back({fileSuffix, line});
}

namespace
{
  // Whether d gives its name a new entity at the top level: not a forward declaration, an extern
  // declaration of something defined elsewhere, or a specialisation of a template declared
  // elsewhere (`template<> struct Hash<ItemID>` extends the library's Hash).
  bool claimsTopLevelName(const Decl* d)
  {
    if (d->flags & (DF_Specialization | DF_Friend | DF_Extern))
      return false;
    // (a variable template's partial specialisation, `constexpr bool isSaved<T, VoidT<...>> = true`)
    if (d->kind == NodeKind::VariableDecl && !static_cast<const VariableDecl*>(d)->name.empty() && static_cast<const VariableDecl*>(d)->name.last().hasTemplateArgs)
      return false;
    switch (d->kind)
    {
      case NodeKind::ClassDecl: return static_cast<const ClassDecl*>(d)->isDefinition;
      case NodeKind::EnumDecl: return static_cast<const EnumDecl*>(d)->isDefinition;
      // (an unscoped enum's enumerator is a name of the scope around its enum, `COPY` beside a
      // constant COPY: two entities)
      case NodeKind::FunctionDecl: case NodeKind::VariableDecl: case NodeKind::UsingAliasDecl: case NodeKind::EnumeratorDecl: return true;
      default: return false;
    }
  }

  // A name only its own file sees: `static` at namespace scope, or in an unnamed namespace.
  bool fileLocal(const Decl* d)
  {
    if ((d->kind == NodeKind::FunctionDecl || d->kind == NodeKind::VariableDecl) && (d->flags & DF_Static))
      return true;
    for (const Decl* p = d->parent; p; p = p->parent)
      if (p->kind == NodeKind::NamespaceDecl && static_cast<const NamespaceDecl*>(p)->name == UINT32_MAX &&
          !static_cast<const NamespaceDecl*>(p)->isInline)
        return true;
    return false;
  }

  std::string whereIs(const Decl* d)
  {
    uint32_t line = 0;
    if (d->file && d->file->tokens && d->token < d->file->tokens->size())
    {
      std::string_view text = d->file->text;
      size_t offset = std::min<size_t>((*d->file->tokens)[d->token].offset, text.size());
      line = 1 + uint32_t(std::count(text.begin(), text.begin() + offset, '\n'));
    }
    return sourceLocation(d->file ? d->file->path : std::string("?"), line);
  }
}

std::vector<std::string> GlobalScope::topLevelClashes()
{
  std::vector<std::string> out;
  Scope& root = impl->root;
  // Whether two free functions take the same parameters (the same function declared twice): the
  // same types, a template's parameters matched by position; never when a parameter type is
  // unknown or either is constrained (constraints make overloads).
  auto sameParameters = [&](FunctionDecl* a, FunctionDecl* b)
  {
    // Declaring a function again is fine (`int printf(const char*, ...);`); defining it twice is not.
    if (!a->body || !b->body)
      return false;
    if (a->templateParams.size() != b->templateParams.size() || a->params.size() != b->params.size() || a->requiresClause ||
        b->requiresClause)
      return false;
    const Ty* ta = impl->declaredTypeOf(a, nullptr, 0);
    const Ty* tb = impl->declaredTypeOf(b, nullptr, 0);
    if (!ta || !tb || ta->kind != TyKind::Function || tb->kind != TyKind::Function || ta->variadic != tb->variadic ||
        ta->args.size() != tb->args.size())
      return false;
    Subst s;
    for (size_t i = 0; i < a->templateParams.size(); ++i)
      s.pairs.push_back({b->templateParams[i], impl->types.param(a->templateParams[i])});
    for (size_t i = 0; i < ta->args.size(); ++i)
    {
      const Ty* pb = s.empty() ? tb->args[i] : impl->substitute(tb->args[i], s);
      if (!ta->args[i] || !ta->args[i]->isKnown() || Impl::mentionsUnknown(ta->args[i]) || pb != ta->args[i])
        return false;
    }
    return true;
  };
  // The type a top-level declaration names (a class, an enum, an alias), or null: two declarations
  // naming one type are one name (`typedef struct LoadF {...} LoadF;`, an alias written in two files).
  auto typeNamed = [&](const Decl* d) -> const Ty*
  {
    Decl* m = const_cast<Decl*>(d);
    const Ty* t = nullptr;
    if (!d->templateParams.empty())
      return nullptr;
    if (d->kind == NodeKind::ClassDecl)
      t = impl->types.classType(m, {});
    else if (d->kind == NodeKind::EnumDecl)
      t = impl->enumTypeIn(m, nullptr);
    else if (d->kind == NodeKind::UsingAliasDecl || (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)))
      t = impl->declaredTypeOf(m, nullptr, 0);
    return t && t->isKnown() && !Impl::mentionsUnknown(t) ? t : nullptr;
  };
  // A constant's declaration spelled token by token, when it reads no name but its own (literals,
  // keywords, punctuation: nothing in it means something else in another file), or nothing.
  auto constantSpelling = [&](const Decl* d)
  {
    std::vector<std::string> out;
    if (d->kind != NodeKind::VariableDecl || (d->flags & DF_Typedef) || !d->file->tokens || static_cast<const VariableDecl*>(d)->name.parts.size() != 1)
      return std::vector<std::string>{};
    const std::vector<Token>& tokens = *d->file->tokens;
    uint32_t own = static_cast<const VariableDecl*>(d)->name.last().symbol;
    bool constant = false;
    int depth = 0;
    std::string buffer;
    for (uint32_t t = d->token; t < tokens.size(); ++t)
    {
      const Token& token = tokens[t];
      if (token.getKind() == TokenKind::End || (token.getKind() == TokenKind::Identifier && token.id != own) || (depth == 0 && token.is(P::Comma)))
        return std::vector<std::string>{};
      constant = constant || token.is(Kw::const_) || token.is(Kw::constexpr_);
      depth += token.is(P::LParen) || token.is(P::LBracket) || token.is(P::LBrace);
      depth -= token.is(P::RParen) || token.is(P::RBracket) || token.is(P::RBrace);
      if (depth == 0 && token.is(P::Semi))
        return constant ? out : std::vector<std::string>{};
      out.push_back(std::string(tokenSpelling(d->file->text, token, buffer)));
    }
    return std::vector<std::string>{};
  };
  // Two declarations of one name that are one entity, or one overload set: functions, two names of
  // one type, or one constant written alike in two files (`const auto M_1_2_PI = 0.159154943091895335769;`
  // from a macro of the game's and one of Lua's; in C++ each file had its own, of the same value).
  auto together = [&](const Decl* a, const Decl* b)
  {
    if (a->kind == NodeKind::FunctionDecl && b->kind == NodeKind::FunctionDecl)
      return true;
    if (const Ty* ta = typeNamed(a); ta && ta == typeNamed(b))
      return true;
    std::vector<std::string> sa = constantSpelling(a);
    return !sa.empty() && sa == constantSpelling(b);
  };
  // A file-local name (`static`, or in an unnamed namespace) is local to its file in C++ and in
  // design.md; kpp gives every top-level name one entity, so one that another declaration also
  // claims is two entities merged: an error until names local to their file are supported.
  auto clash = [&](const Decl* d, uint32_t symbol, const Decl* other)
  {
    std::string text = whereIs(d) + ": error: '" + std::string(impl->symbols.name(symbol)) + "' is declared here and " +
                       (other->file->isLibrary  ? "in the standard library (" + whereIs(other) + ")"
                        : other->file->isImported ? "in the imported C headers (" + whereIs(other) + ")"
                        : !other->file->library.empty() && other->file->library != d->file->library
                          ? "in the library " + other->file->library + " (" + whereIs(other) + ")"
                          : "at " + whereIs(other));
    if (other->file->isImported)
      text += "; a C header's names are the program's, rename the program's";
    else if (fileLocal(d) || fileLocal(other))
      text += "; a name local to its file (static, or in an unnamed namespace) is not supported yet (kpp gives every top-level name one entity), rename one of them";
    else
      text += "; top-level names are global, rename one of them";
    out.push_back(text);
  };
  // A scoped library's name is what its names are qualified with (design.md 16): no other
  // declaration has it at the top level, and no namespace of the program opens it (its names would
  // join the library's).
  auto libraryNameTaken = [&](const Decl* d, const Scope* library)
  {
    std::string name(impl->symbols.name(library->name));
    out.push_back(whereIs(d) + ": error: '" + name + "' is the name of the library " + name +
                  ", which qualifies its names (design.md 16); rename the program's");
  };
  for (const Scope* library : impl->libraryScopes)
    if (const std::vector<Entry>* entries = root.find(library->name))
      for (const Entry& e : *entries)
        if (e.decl->kind != NodeKind::NamespaceDecl)
          libraryNameTaken(e.decl, library);
  for (const NamespaceDecl* ns : impl->programNamespaces)
    for (const Scope* s = ns->ownScope; s && s != &root; s = s->parent)
      if (s->library)
      {
        libraryNameTaken(ns, s);
        break;
      }
  // The global namespace and every namespace in it (each one scope however many blocks open it):
  // a namespace's members are names of the program as the global ones are.
  std::vector<Scope*> namespaces{&root};
  std::unordered_set<Scope*> listed{&root};
  for (size_t n = 0; n < namespaces.size(); ++n)
    for (uint32_t i = 0; i < namespaces[n]->shardCount; ++i)
      for (const auto& [symbol, entries] : namespaces[n]->shards[i].map)
        for (const Entry& e : entries)
          if (e.scope && e.scope->kind == ScopeKind::Namespace && e.scope->parent == namespaces[n] && listed.insert(e.scope).second)
            namespaces.push_back(e.scope);
  // Each file's private names (design.md 15), among themselves: they hide the global ones and never
  // meet another file's.
  for (auto& [file, own] : impl->privateScopes)
    namespaces.push_back(own);
  for (Scope* scope : namespaces)
    for (uint32_t i = 0; i < scope->shardCount; ++i)
      for (const auto& [symbol, entries] : scope->shards[i].map)
      {
        if (entries.size() < 2)
          continue;
        const Decl* library = nullptr;
        for (const Entry& e : entries)
          if (!library && e.decl->file && e.decl->file->isLibrary && claimsTopLevelName(e.decl))
            library = e.decl;
        // The program's own declarations of the name, in source order (the order of indexing varies).
        std::vector<const Decl*> program;
        for (const Entry& e : entries)
          if (e.decl->file && !e.decl->file->isLibrary && !e.decl->file->isImported && claimsTopLevelName(e.decl))
            program.push_back(e.decl);
        // (an unscoped library's first: the program's is the one in the error, design.md 16)
        std::sort(program.begin(), program.end(), [](const Decl* a, const Decl* b)
                  { return std::make_tuple(a->file->library.empty(), std::string_view(a->file->path), a->token) <
                           std::make_tuple(b->file->library.empty(), std::string_view(b->file->path), b->token); });
        // The imported C headers' (`extern "C" import`) of the name, which every file sees as the
        // program's own are.
        std::vector<const Decl*> imported;
        for (const Entry& e : entries)
          if (e.decl->file && e.decl->file->isImported && claimsTopLevelName(e.decl))
            imported.push_back(e.decl);
        for (size_t k = 0; k < program.size(); ++k)
        {
          const Decl* d = program[k];
          if (library)
          {
            if (library->kind == NodeKind::FunctionDecl && d->kind == NodeKind::FunctionDecl)
            {
              // Overloads of one function; but the parameters of one of the library's is that function
              // declared again (`gcd(long long, long long)`), which the program cannot have twice.
              // (a replacement of the library's allocation functions, `void* operator new(size_t)`, is no
              // second definition: the program's replaces it, design.md "Allocation functions")
              for (const Entry& l : entries)
                if (l.decl->kind == NodeKind::FunctionDecl && l.decl->file && l.decl->file->isLibrary && claimsTopLevelName(l.decl) &&
                    !impl->replacedAllocation(static_cast<FunctionDecl*>(l.decl)) &&
                    sameParameters(static_cast<FunctionDecl*>(l.decl), static_cast<FunctionDecl*>(const_cast<Decl*>(d))))
                {
                  out.push_back(whereIs(d) + ": error: '" + std::string(impl->symbols.name(symbol)) +
                                "' with these parameters is defined here and in the standard library (" + whereIs(l.decl) +
                                "); top-level names are global, rename one of them");
                  break;
                }
              continue;
            }
            clash(d, symbol, library);
            continue;
          }
          // A C header's name is the program's unless the program's is that entity too (a function
          // the header declares, or overloads of it; an alias of the header's type; a constant
          // written alike); otherwise one of the two silently stood for the other in every file (a
          // struct of another layout, whose sizeof the header's own users then got). Every one of
          // the header's: its `typedef struct X X;` names the program's X when both define X, so
          // that one alone would excuse the header's own definition (a forward declaration's
          // typedef, kpp_directx.h's COM interfaces, is the only one there).
          if (!imported.empty() && !std::all_of(imported.begin(), imported.end(), [&](const Decl* c) { return together(c, d); }))
          {
            clash(d, symbol, imported[0]);
            continue;
          }
          // Two of the program's: the second is the error, at the first that is another entity.
          for (size_t j = 0; j < k; ++j)
            if (!together(program[j], d))
            {
              clash(d, symbol, program[j]);
              break;
            }
        }
      }
  std::sort(out.begin(), out.end());
  return out;
}

FunctionDecl* GlobalScope::enumOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context)
{
  FunctionDecl* f = findFreeOperator(punctuator, lhs, rhs, context);
  return impl->takesEnumOperands(f, lhs, rhs) ? f : nullptr;
}

const Ty* GlobalScope::typeOfDecl(Decl* d) { return impl->declaredTypeOf(d, nullptr, 0); }

std::vector<const Ty*> GlobalScope::forgetTypesFromWalks(const std::vector<const Decl*>& decls)
{
  std::vector<const Ty*> before;
  for (const Decl* d : decls)
  {
    before.push_back(impl->declaredTypeOf(const_cast<Decl*>(d), nullptr, 0));
    const_cast<Decl*>(d)->cachedType.store(nullptr, std::memory_order_release);
    if (Dependencies* deps = d->typeDependencies.load(std::memory_order_acquire))
    {
      {
        std::lock_guard lock(deps->mutex);
        deps->list = DependencyList{};
        deps->tidied = true;
      }
      impl->forgetErrorsOf({deps});
    }
  }
  return before;
}

thread_local const std::vector<Token>* GlobalScope::Impl::fallbackTokens = nullptr;
thread_local bool GlobalScope::Impl::exactLayoutOnly = false;
thread_local bool GlobalScope::Impl::layoutNeeded = false;
thread_local int GlobalScope::Impl::nameEvaluations = 0;
thread_local int GlobalScope::Impl::deepestNames = 0;
thread_local int GlobalScope::Impl::deepestCalls = 0;
thread_local int GlobalScope::Impl::deepestLevels = 0;
thread_local int GlobalScope::Impl::cachedLevels = 0;
thread_local uint64_t GlobalScope::Impl::evaluationTrips = 0;
thread_local GlobalScope::Impl::SubstTypeMemo* GlobalScope::Impl::substTypeMemo = nullptr;
thread_local std::vector<GlobalScope::Impl::TypeInProgress> GlobalScope::Impl::typesInProgress;
thread_local int GlobalScope::Impl::cycleCutAt = INT_MAX;
thread_local bool GlobalScope::Impl::constCallPlaceRan = false;
thread_local const GlobalScope::Impl::EnumeratorValues* GlobalScope::Impl::enumeratorValues = nullptr;
thread_local std::string_view GlobalScope::Impl::fallbackText;

const Ty* GlobalScope::typeOfAst(Type* t, Decl* context)
{
  Impl::DeclContext dc;
  impl->declContextFor(context, dc);
  ParsedFile* file = context ? context->file : nullptr;
  Impl::TokenScope tokenScope(file ? file->tokens : nullptr, file ? file->text : std::string_view());
  return impl->convertType(t, Impl::TypeContext{dc.scope, &dc.params, nullptr, file ? file->tokens : nullptr, file ? file->text : std::string_view()}, 0);
}

FunctionDecl* GlobalScope::declarationOf(FunctionDecl* def) { return impl->declarationOfDefinition(def); }

Decl* GlobalScope::owningClass(Decl* d) { return impl->owningClassOf(d); }
bool GlobalScope::programAllocationFunctions() { return impl->programAllocationFunctions(); }
std::optional<ConstValue> GlobalScope::argumentObject(const Ty* t) { return impl->argumentObject(t); }
bool GlobalScope::replacedAllocation(const FunctionDecl* f) { return impl->replacedAllocation(f); }
FunctionDecl* GlobalScope::globalDeallocation(bool array, bool sized) { return impl->globalDeallocation(array, sized); }

TemplateArgs GlobalScope::definitionArgs(Decl* def, const TemplateArgs& classArgs)
{
  // `template<typename T> const typename Search<T>::Iterator Search<T>::endIterator = Iter<T>();`:
  // the out-of-class definition declares the class's parameters anew; they take its arguments.
  TemplateArgs out = classArgs;
  if (!def || def->kind != NodeKind::VariableDecl || def->templateParams.empty() || static_cast<VariableDecl*>(def)->name.parts.size() < 2)
    return out;
  auto* v = static_cast<VariableDecl*>(def);
  QualifiedName qualifier = v->name;
  qualifier.parts.count -= 1;
  LocalStack params;
  for (Decl* p : v->templateParams)
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  Lookup l = impl->lookupQualified(qualifier, impl->lexicalScopeOf(v), &params, 0);
  Decl* cls = l.scope && l.scope->kind == ScopeKind::Class ? l.scope->owner : nullptr;
  // (`template<class T> int Box<T*>::size = sizeof(T);`: the partial specialisation's parameters,
  // which its instances' arguments name: T is Box<double*>'s double)
  if (cls)
    if (Scope* partial = impl->partialSpecializationScope(qualifier, l, impl->lexicalScopeOf(v), v->templateParams, v->file))
      cls = partial->owner;
  if (!cls || cls->templateParams.size() > v->templateParams.size())
    return out;
  for (size_t i = 0; i < cls->templateParams.size(); ++i)
    for (auto& [p, t] : classArgs.pairs)
      if (p == cls->templateParams[i])
      {
        out.pairs.push_back({v->templateParams[i], t});
        break;
      }
  return out;
}

Decl* GlobalScope::initializerListDecl() { return impl->initializerListDecl(); }
const Ty* GlobalScope::textType() { return impl->textType(); }
int GlobalScope::intrinsicOf(const FunctionDecl* f) { return impl->intrinsicOf(f); }
GlobalScope::CallIntrinsic GlobalScope::callIntrinsicOf(const FunctionDecl* f) { return impl->callIntrinsicOf(f); }
TextPieceKind GlobalScope::textPieceOf(const Expr* e, const Ty* t) { return impl->textPieceOf(e, t); }
Decl* GlobalScope::typeInfoDecl() { return impl->typeInfoDecl(); }
Decl* GlobalScope::enumFromStrDecl() { return impl->enumFromStrDecl(); }
Decl* GlobalScope::strongOrderingDecl() { return impl->strongOrderingDecl(); }

Decl* GlobalScope::definitionOf(Decl* d, const TemplateArgs& classArgs)
{
  // A static data member of a class template instance: the explicit specialisation written for
  // that instance (`template<> const uint32_t ElectricNetworkStatistics::smoothingTicks = 0;`), if any.
  if (d && d->kind == NodeKind::VariableDecl && !classArgs.empty())
  {
    Subst s;
    for (auto& [p, t] : classArgs.pairs)
      s.pairs.push_back({p, t});
    return impl->variableDefinitionFor(static_cast<VariableDecl*>(d), &s);
  }
  // A function template with its arguments: the explicit specialisation written for them
  // (`template<> ForceID parse<ForceID>(...)`), when there is one.
  if (d && d->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(d)->templateParams.empty() && !classArgs.empty())
  {
    Subst s;
    for (auto& [p, t] : classArgs.pairs)
      s.pairs.push_back({p, t});
    if (FunctionDecl* spec = impl->functionSpecializationFor(static_cast<FunctionDecl*>(d), s))
      return spec;
  }
  // A member of a class template instantiation: the explicit specialisation written for that
  // instantiation (`template<> const char* Button<RecipeID>::tooltip()`), when there is one.
  // (A member of a class nested in one, `template<> int Outer<int>::Inner::f()`, too: the
  // nested class has the enclosing template's parameters.)
  Decl* cls = d && d->kind == NodeKind::FunctionDecl && !classArgs.empty() ? owningClass(d) : nullptr;
  if (cls && cls->ownScope && !Impl::effectiveParams(cls).empty())
  {
    auto* f = static_cast<FunctionDecl*>(d);
    uint32_t symbol = impl->functionSymbol(f->name);
    Subst s;
    for (auto& [p, t] : classArgs.pairs)
      s.pairs.push_back({p, t});
    const Ty* ft = nullptr;
    FunctionDecl* declared = nullptr; // a specialisation declared without its body (`template<> void Buffer<Update>::f();`)
    for (FunctionDecl* candidate : cls->ownScope->outOfLineMembers)
    {
      if (!Impl::isMemberSpecialization(candidate) || impl->functionSymbol(candidate->name) != symbol)
        continue;
      Impl::TokenScope tokenScope(candidate->file ? candidate->file->tokens : nullptr, candidate->file ? candidate->file->text : std::string_view());
      const Ty* written = impl->specializedClassOf(candidate);
      if (!written)
        continue;
      Subst ws = impl->substFor(written);
      bool same = true;
      for (Decl* p : Impl::effectiveParams(cls))
        same = same && ws.find(p) && ws.find(p) == s.find(p);
      if (!same)
        continue;
      if (!ft)
        ft = impl->substitute(impl->declaredTypeOf(f, nullptr, 0), s);
      // Its type read for the instance it specialises (`Nanoseconds Item::*` with the class's
      // `using Item = T;`), as the template's member's is.
      if (impl->declaredTypeOf(candidate, &ws, 0) != ft && impl->declaredTypeOf(candidate, nullptr, 0) != ft)
        continue; // another overload's specialisation
      candidate->flags |= f->flags & (DF_Static | DF_Virtual);
      if (!candidate->body)
      {
        // Declared first, defined later (through an alias of the instance, `template<> void
        // UpdateBuffer::TimeChecker::addTime(...) {...}`): the definition is the one with the body.
        if (!declared)
          declared = candidate;
        continue;
      }
      return candidate;
    }
    if (declared)
      return declared;
  }
  return definitionOf(d);
}

Decl* GlobalScope::definitionOf(Decl* d)
{
  if (!d)
    return nullptr;
  if (d->kind == NodeKind::VariableDecl)
  {
    // A static data member: the out-of-line definition carries the initializer.
    auto it = impl->variableDefinitions.find(d);
    return it != impl->variableDefinitions.end() ? it->second : d;
  }
  if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl)
  {
    if (d->ownScope)
      return d;
    Entry e{d, nullptr};
    Scope* s = impl->scopeOfEntry(e, 0);
    return s ? s->owner : nullptr;
  }
  if (d->kind == NodeKind::FunctionDecl)
  {
    auto* f = static_cast<FunctionDecl*>(d);
    if (f->body)
      return f;
    Decl* cls = owningClass(f);
    if (cls && cls->ownScope)
    {
      uint32_t symbol = impl->functionSymbol(f->name);
      const Ty* ft = impl->matchedType(f);
      // A declaration returning `auto` against a definition whose type has its return deduced:
      // the parameters and the method's constness decide.
      auto matches = [&](FunctionDecl* candidate)
      {
        if (candidate->refQualifier != f->refQualifier || (candidate->flags & (DF_Const | DF_Volatile)) != (f->flags & (DF_Const | DF_Volatile)))
          return false; // (`int Box::out() &` and `int Box::out() &&`: two functions, as with const and volatile)
        const Ty* ct = impl->typeAs(candidate, f);
        if (ct == ft)
          return true;
        if (!ft || !ct || ft->kind != TyKind::Function || ct->kind != TyKind::Function || !impl->containsAuto(ft->inner) ||
            ct->args.size() != ft->args.size() || ct->variadic != ft->variadic)
          return false;
        for (size_t i = 0; i < ct->args.size(); ++i)
          if (ct->args[i] != ft->args[i])
            return false;
        return true;
      };
      for (FunctionDecl* candidate : cls->ownScope->outOfLineMembers)
        if (impl->functionSymbol(candidate->name) == symbol && !Impl::isMemberSpecialization(candidate) && matches(candidate))
        {
          // `static` and `virtual` are written on the declaration only; the definition is emitted
          // (with or without `this`) by what the declaration says.
          candidate->flags |= f->flags & (DF_Static | DF_Virtual);
          return candidate;
        }
      static bool debugCalls = getenv("KPP_DEBUG_CALLS") != nullptr;
      if (debugCalls)
      {
        std::string line = "definitionOf " + qualifiedName(f) + " : " + impl->types.toString(ft, &impl->symbols) + " candidates:";
        for (FunctionDecl* candidate : cls->ownScope->outOfLineMembers)
          if (impl->functionSymbol(candidate->name) == symbol)
            line += " " + impl->types.toString(impl->typeAs(candidate, f), &impl->symbols);
        line += " (" + std::to_string(cls->ownScope->outOfLineMembers.size()) + " out-of-line members)";
        fprintf(stderr, "%s\n", line.c_str());
      }
    }
    else if (!cls && f->name.parts.size() == 1)
    {
      // A free function declared and defined separately: find the entry with a body.
      Scope* s = impl->lexicalScopeOf(f);
      uint32_t symbol = impl->functionSymbol(f->name);
      const Ty* ft = impl->matchedType(f);
      if (const std::vector<Entry>* entries = s->find(symbol))
        for (const Entry& e : *entries)
          if (e.decl->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(e.decl)->body &&
              impl->typeAs(static_cast<FunctionDecl*>(e.decl), f) == ft)
            return e.decl;
    }
    return f;
  }
  return d;
}

std::string GlobalScope::draftName(const Decl* d)
{
  if (!d || !(d->attributes & ATTR_Draft) || !d->ownScope || d->ownScope->name == UINT32_MAX)
    return "?";
  return std::string(impl->symbols.name(d->ownScope->name));
}

std::string GlobalScope::qualifiedName(Decl* d)
{
  auto own = [&](Decl* x) -> std::string
  {
    switch (x->kind)
    {
      case NodeKind::NamespaceDecl:
      {
        uint32_t n = static_cast<NamespaceDecl*>(x)->name;
        return n == UINT32_MAX ? std::string() : std::string(impl->symbols.name(n));
      }
      case NodeKind::ClassDecl: case NodeKind::EnumDecl:
      {
        const QualifiedName& q = x->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(x)->name : static_cast<EnumDecl*>(x)->name;
        return q.empty() || q.last().symbol == UINT32_MAX ? std::string("(anonymous)") : std::string(impl->symbols.name(q.last().symbol));
      }
      case NodeKind::FunctionDecl:
      {
        const NamePart& p = static_cast<FunctionDecl*>(x)->name.last();
        if (p.special == 1)
          return "~" + std::string(impl->symbols.name(p.symbol));
        if (p.special == 2 && p.opToken < uint32_t(P::Count))
        {
          P op = P(p.opToken);
          return op == P::LBracket ? "operator[]" : op == P::LParen ? "operator()" : "operator" + std::string(punctuatorSpelling(op));
        }
        if (p.special == 3)
        {
          // A conversion operator: named by what it converts to (`operator bool`, `operator String`).
          const Ty* ft = impl->declaredTypeOf(x, nullptr, 0);
          if (ft && ft->kind == TyKind::Function && ft->inner)
            return "operator " + impl->types.toString(ft->inner, &impl->symbols);
        }
        if (p.special != 0 || p.symbol == UINT32_MAX)
          return "operator?";
        return std::string(impl->symbols.name(p.symbol));
      }
      case NodeKind::VariableDecl:
      {
        const QualifiedName& q = static_cast<VariableDecl*>(x)->name;
        return q.empty() ? std::string("(unnamed)") : std::string(impl->symbols.name(q.last().symbol));
      }
      case NodeKind::EnumeratorDecl: return std::string(impl->symbols.name(static_cast<EnumeratorDecl*>(x)->name));
      default: return "?";
    }
  };
  std::vector<std::string> parts;
  Decl* start = owningClass(d);
  if (!start || (d->parent == start))
    start = d->parent;
  for (Decl* p = start; p; p = p->parent)
  {
    if (p->kind == NodeKind::NamespaceDecl || p->kind == NodeKind::ClassDecl || p->kind == NodeKind::EnumDecl)
    {
      std::string n = own(p);
      if (!n.empty())
        parts.push_back(n);
    }
  }
  // (a member defined out of line, `int Packer::add(int)`, is its class's: private when it is)
  std::string result = filePrivatePrefix(owningClass(d) ? owningClass(d) : d);
  for (size_t i = parts.size(); i-- > 0;)
    result += parts[i] + "::";
  return result + own(d);
}

bool GlobalScope::isExternC(FunctionDecl* f) { return (f->flags & DF_ExternC) != 0; }

VariableDecl* GlobalScope::namedReturnVariable(const FunctionDecl* f, std::string* why)
{
  if (!f || !f->body)
  {
    if (why)
      *why = "no body";
    return nullptr;
  }
  const Ty* ft = typeOfDecl(const_cast<FunctionDecl*>(f));
  return namedReturnVariable(f->body, ft && ft->kind == TyKind::Function ? ft->inner : nullptr, why);
}

VariableDecl* GlobalScope::namedReturnVariable(const Stmt* body, const Ty* result, std::string* why, const TemplateArgs* args)
{
  auto no = [&](std::string reason) -> VariableDecl*
  {
    if (why)
      *why = std::move(reason);
    return nullptr;
  };
  if (!body)
    return no("no body");
  if (!result || result->kind != TyKind::Class)
    return no("the function returns no class by value"); // (a reference, a scalar, void, unknown)
  VariableDecl* v = Impl::namedReturnCandidate(body, why);
  if (!v)
    return nullptr;
  const Ty* vt = args ? typeOfDeclIn(v, *args) : typeOfDecl(v);
  if (vt && vt->isVolatile)
    return no("the variable is volatile");
  if (!vt || vt->kind != TyKind::Class || impl->types.unqualified(vt) != impl->types.unqualified(result))
    return no(std::string("the variable's type, ") + (vt ? impl->types.toString(vt, &impl->symbols) : std::string("?")) + ", is not the returned " +
              impl->types.toString(result, &impl->symbols) + ": a copy or a conversion");
  return v;
}

std::vector<std::string> GlobalScope::implicitUses(const std::vector<ParsedFile*>& files)
{
  std::map<std::pair<const void*, uint32_t>, ImplicitRecord> records;
  for (ImplicitRecord& r : takeImplicitRecords())
    records[{r.file, r.token}] = std::move(r);
  std::vector<std::string> out;
  for (ParsedFile* file : files)
  {
    if (!file || file->isImported || !file->tokens)
      continue;
    LineStarts starts(file->text);
    forEachNode(file->decls, [&](Node* n)
    {
      if (n->kind != NodeKind::FunctionDecl)
        return;
      auto* f = static_cast<FunctionDecl*>(n);
      if (!(f->flags & DF_Implicit) || !(f->flags & (DF_Constructor | DF_Conversion)) || f->file != file || f->token >= file->tokens->size())
        return;
      auto it = records.find({file, f->token});
      uint64_t asked = it == records.end() ? 0 : it->second.asked;
      size_t uses = it == records.end() ? 0 : it->second.places.size();
      std::string line = sourceLocation(file->path, starts.line((*file->tokens)[f->token].offset)) + "	" +
                         ((f->flags & DF_Constructor) ? "constructor" : "conversion") + "	" + qualifiedName(f) + "	uses " + std::to_string(uses) +
                         "	asked " + std::to_string(asked) + (f->explicitCondition ? "	conditional" : "");
      out.push_back(line);
      if (it != records.end())
        for (const std::string& place : it->second.places)
          out.push_back("	at " + place);
    });
  }
  return out;
}

std::vector<std::string> GlobalScope::returnCopyWarnings(const std::vector<ParsedFile*>& files)
{
  std::vector<std::string> out;
  for (ParsedFile* file : files)
  {
    if (!file || file->isImported || file->isLibrary)
      continue;
    forEachNode(file->decls, [&](Node* n)
    {
      if (n->kind != NodeKind::FunctionDecl)
        return;
      auto* f = static_cast<FunctionDecl*>(n);
      if (!f->body || !f->templateParams.empty() || isTemplateContext(f))
        return; // (a template's: decided per instance, not warned of yet)
      std::string why;
      if (namedReturnVariable(f, &why))
        return;
      // Each `return x;` of a local of class type: copied or moved, not built in place.
      std::unordered_set<const Node*> nested;
      forEachNode(f->body, [&](Node* m)
      {
        if ((m->kind == NodeKind::LambdaExpr || m->kind == NodeKind::ClassDecl) && !nested.count(m))
          forEachNode(m, [&](Node* inner) { if (inner != m) nested.insert(inner); });
        if (m->kind != NodeKind::ReturnStmt || nested.count(m))
          return;
        const Expr* e = static_cast<ReturnStmt*>(m)->value;
        const Decl* d = e && e->kind == NodeKind::NameExpr ? static_cast<const NameExpr*>(e)->resolved : nullptr;
        if (!d || (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::ParamDecl) || (d->flags & (DF_Static | DF_Extern)))
          return;
        const Ty* t = typeOfDecl(const_cast<Decl*>(d));
        if (!t || t->kind != TyKind::Class)
          return;
        uint32_t name = d->kind == NodeKind::ParamDecl ? static_cast<const ParamDecl*>(d)->name : static_cast<const VariableDecl*>(d)->name.last().symbol;
        out.push_back(Impl::locationOf(f->file, m->token) + ": warning: the return of '" + std::string(impl->symbols.name(name)) +
                      "' copies or moves it, not built in the result's place: " + why);
      });
    });
  }
  sortDiagnostics(out);
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool GlobalScope::isTemplateContext(Decl* d)
{
  for (Decl* p = d; p; p = p->parent)
    if (!p->templateParams.empty())
      return true;
  // An explicit specialisation of a class template's member or of a function template: emitted
  // as that instantiation (definitionOf with its arguments), never on its own.
  return d && d->kind == NodeKind::FunctionDecl &&
         (Impl::isMemberSpecialization(static_cast<FunctionDecl*>(d)) || Impl::isFunctionSpecialization(static_cast<FunctionDecl*>(d)));
}

namespace
{
  Subst toSubst(const TemplateArgs& args)
  {
    Subst s;
    for (auto& [d, t] : args.pairs)
      s.pairs.push_back({d, t});
    return s;
  }
}

bool GlobalScope::freeOperatorBeatsMember(FunctionDecl* f, const TemplateArgs& freeArgs, FunctionDecl* member, const TemplateArgs& memberArgs,
                                          const Ty* lhs, const Ty* rhs)
{
  return impl->freeOperatorBeatsMember(f, toSubst(freeArgs), member, toSubst(memberArgs), lhs, rhs, 2);
}

const Ty* GlobalScope::typeInInstanceOf(Decl* d, const Decl* in)
{
  const Subst* s = impl->instanceSubstOf(in); // (an entry is kept once recorded)
  return s ? impl->declaredTypeOf(d, s, 0) : nullptr;
}

const Ty* GlobalScope::typeOfDeclIn(Decl* d, const TemplateArgs& args)
{
  if (args.empty())
    return typeOfDecl(d);
  Subst s = toSubst(args);
  const Ty* t = impl->declaredTypeOf(d, &s, 0);
  if (d->kind == NodeKind::VariableDecl && d->parent && d->parent->kind == NodeKind::FunctionDecl && Impl::mentionsUnknown(t))
    if (const Ty* cached = d->cachedType.load(std::memory_order_acquire); cached && !Impl::mentionsUnknown(cached) && !impl->containsAuto(cached))
      return cached; // `Vector<ToCall> calls;` with ToCall a class of the instance's own body: as its walk typed it
  if (d->kind == NodeKind::VariableDecl && d->parent && d->parent->kind == NodeKind::FunctionDecl && impl->isUndeducedClassTemplate(t))
    if (const Ty* cached = d->cachedType.load(std::memory_order_acquire);
        cached && cached->kind == TyKind::Class && cached->decl == t->decl && !impl->types.isDependent(cached))
      return cached; // `Failed result = this->canSet(id)` in an instance's copy: the arguments its walk deduced
  if (d->kind == NodeKind::FunctionDecl && t && t->kind == TyKind::Function && impl->containsAuto(t->inner))
  {
    // `template<typename T> auto f(T)`: the return type its body deduced for these arguments.
    if (const Ty* r = impl->deducedReturnOf(static_cast<FunctionDecl*>(d), s))
      return impl->types.functionLike(t, r, t->args);
  }
  if (impl->containsAuto(t) && d->kind == NodeKind::VariableDecl)
  {
    auto* v = static_cast<VariableDecl*>(d);
    // A braced initializer (`auto x{e}`, an init-capture `[callback{::move(callback)}]`) is no
    // value of its own: the type the walk deduced from its element.
    bool braced = v->init && v->init->kind == NodeKind::BraceInitExpr;
    if (v->init && v->init->ty && !braced)
      return impl->deduceVariable(v, t, v->init->ty, v->init);
    if (const Ty* cached = v->cachedType.load(std::memory_order_acquire); cached && !impl->containsAuto(cached))
      return cached; // a range-for variable: the element type the loop deduced
  }
  return t;
}

std::optional<ConstValue> GlobalScope::evaluate(Expr* e, ParsedFile* file, Decl* context, const TemplateArgs& args, const ConstHooks& hooks)
{
  return evaluateInitializer(nullptr, e, file, context, args, hooks);
}

// t null: the value of e as an expression (evaluate).
const Ty* GlobalScope::surrogateFunction(const Ty* object, const std::vector<const Ty*>& args)
{
  bool ambiguous = false;
  const Ty* t = impl->surrogateFunction(object, args, ambiguous);
  return ambiguous ? nullptr : t;
}

const Ty* GlobalScope::underlyingOf(const Ty* enumeration)
{
  ParsedFile* file = enumeration->decl ? enumeration->decl->file : nullptr;
  Impl::TokenScope tokenScope(file ? file->tokens : nullptr, file ? file->text : std::string_view());
  return impl->declaredUnderlying(enumeration);
}

std::optional<ConstValue> GlobalScope::evaluateEnumerator(Decl* enumerator, const Ty* enumType, const ConstHooks& hooks)
{
  struct Lent
  {
    explicit Lent(const ConstHooks& h) { Impl::constHooks() = &h; }
    ~Lent() { Impl::constHooks() = saved; }

    const ConstHooks* saved = Impl::constHooks();
  };
  ParsedFile* file = enumerator->parent ? enumerator->parent->file : nullptr;
  Impl::TokenScope tokenScope(file ? file->tokens : nullptr, file ? file->text : std::string_view());
  Lent lent(hooks);
  Subst s = impl->substFor(enumType);
  Impl::TypeContext ctx{impl->lexicalScopeOf(enumerator->parent), nullptr, nullptr, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  return impl->constValueOfDecl(enumerator, ctx, 0, s.empty() ? nullptr : &s);
}

std::optional<ConstValue> GlobalScope::evaluateInitializer(const Ty* t, Expr* e, ParsedFile* file, Decl* context, const TemplateArgs& args,
                                                           const ConstHooks& hooks)
{
  struct Lent
  {
    explicit Lent(const ConstHooks& h) { Impl::constHooks() = &h; }
    ~Lent() { Impl::constHooks() = saved; }

    const ConstHooks* saved = Impl::constHooks();
  };
  if (!e)
  {
    // `constexpr P p;`, a global no initializer initializes: default-initialized, as a local is (a
    // user-provided default constructor run, members from their defaults); one nothing sets is
    // unset, which static storage holds as zero (constantText). Thread B's init-order bug: built
    // at startup, p read zero in a dynamic initializer that ran first.
    if (!t)
      return std::nullopt;
    if (!file)
      file = context ? context->file : nullptr;
    Impl::TokenScope tokenScope(file ? file->tokens : nullptr, file ? file->text : std::string_view());
    Lent lent(hooks);
    return impl->constEmpty(t, false, 0);
  }
  // The context as typeOfAstIn makes it for a type written there.
  Impl::DeclContext dc;
  impl->declContextFor(context, dc);
  if (context && (context->kind == NodeKind::ClassDecl || context->kind == NodeKind::EnumDecl) && context->ownScope)
    dc.scope = context->ownScope;
  Subst s = toSubst(args);
  if (impl->instanceSubstOf(context))
    s = impl->withInstanceSubst(context, &s);
  for (const Subst::Pair& p : s.pairs)
    if (p.param && p.param->kind == NodeKind::TemplateParamDecl && !dc.params.find(static_cast<TemplateParamDecl*>(p.param)->name))
      dc.params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  if (!file)
    file = context ? context->file : nullptr;
  if (context && context->kind == NodeKind::FunctionDecl && file == context->file)
    if (Stmt* body = static_cast<FunctionDecl*>(context)->body; body && body->token < e->token && (!context->endToken || e->token < context->endToken))
      impl->collectVisibleLocalAliases(body, e->token, dc.params);
  if (context && file == context->file)
  {
    // In a lambda's body (the closure as the context): the function around it, then the body.
    for (Decl* p = context->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(context)->isClosure ? context->parent : nullptr; p; p = p->parent)
      if (p->kind == NodeKind::FunctionDecl)
      {
        if (Stmt* body = static_cast<FunctionDecl*>(p)->body; body && body->token < e->token && (!p->endToken || e->token < p->endToken))
          impl->collectVisibleLocalAliases(body, e->token, dc.params);
        break;
      }
    impl->collectLambdaBodyAliases(context, e->token, dc.params);
  }
  Impl::TokenScope tokenScope(file ? file->tokens : nullptr, file ? file->text : std::string_view());
  Impl::TypeContext ctx{dc.scope, &dc.params, s.empty() ? nullptr : &s, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  if (!args.elementPacks.empty())
  {
    ctx.elementPacks = &args.elementPacks;
    ctx.elementIndex = args.elementIndex;
  }
  Lent lent(hooks);
  return t ? impl->constInitialize(t, e, ctx, 0) : impl->constValue(e, ctx, 0);
}

std::string GlobalScope::constantReason()
{
  return Impl::constWhy();
}

void GlobalScope::clearConstantReason()
{
  Impl::constWhy().clear();
}

std::optional<std::string> GlobalScope::runBuild(std::string_view configuration, std::string& error)
{
  return impl->runBuild(configuration, error);
}

const Ty* GlobalScope::typeOfAstIn(Type* t, Decl* context, const TemplateArgs& args)
{
  Impl::DeclContext dc;
  impl->declContextFor(context, dc);
  // A type written inside a class (`super` in `using super::super;`, the base initializer of an
  // inherited constructor): its members and the injected names of its bases are in scope.
  if (context && (context->kind == NodeKind::ClassDecl || context->kind == NodeKind::EnumDecl) && context->ownScope)
    dc.scope = context->ownScope;
  Subst s = toSubst(args);
  if (impl->instanceSubstOf(context))
    s = impl->withInstanceSubst(context, &s); // `ElemProvider<ElemType>` based on by a class of an instance's body
  // A parameter args binds that context does not declare: a for constexpr variable
  // (`sizeof(E)` in `for constexpr (typename E : types<A, B>)`), found by its name too.
  for (const Subst::Pair& p : s.pairs)
    if (p.param && p.param->kind == NodeKind::TemplateParamDecl && !dc.params.find(static_cast<TemplateParamDecl*>(p.param)->name))
      dc.params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  // A type written in the function's body (`nl::min()` after `using nl = Limits<T1>;`, `List::forEach`
  // after `using List = TypeList<int, Small>;`): the body's aliases in scope there, which the walk
  // had as locals and which hide any other declaration of their name (the library's List).
  // Those of the functions it is nested in too (a lambda's parameter `const Layers::value_type&`
  // with `using Layers = ...;` in the function around the lambda), outermost first, so that an
  // inner alias hides an outer one of its name.
  if (t)
  {
    std::vector<FunctionDecl*> around;
    for (Decl* p = context; p; p = p->parent)
      if (p->kind == NodeKind::FunctionDecl)
        if (Stmt* body = static_cast<FunctionDecl*>(p)->body; body && body->token < t->token && (!p->endToken || t->token < p->endToken))
          around.push_back(static_cast<FunctionDecl*>(p));
    for (size_t i = around.size(); i-- > 0;)
      impl->collectVisibleLocalAliases(around[i]->body, t->token, dc.params);
    impl->collectLambdaBodyAliases(context, t->token, dc.params); // innermost: they hide the function's
  }
  ParsedFile* file = context ? context->file : nullptr;
  Impl::TokenScope tokenScope(file ? file->tokens : nullptr, file ? file->text : std::string_view());
  Impl::TypeContext ctx{dc.scope, &dc.params, s.empty() ? nullptr : &s, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  if (!args.elementPacks.empty())
  {
    ctx.elementPacks = &args.elementPacks;
    ctx.elementIndex = args.elementIndex;
  }
  return impl->convertType(t, ctx, 0);
}

std::optional<bool> GlobalScope::typeTraitValue(NameExpr* e, Decl* context, const TemplateArgs& args)
{
  Impl::DeclContext dc;
  impl->declContextFor(context, dc);
  const NamePart* trait = impl->boolTraitPart(e->name, dc.scope, &dc.params, 0);
  if (!trait)
    return std::nullopt;
  Subst s = toSubst(args);
  ParsedFile* file = context ? context->file : nullptr;
  Impl::TypeContext ctx{dc.scope, &dc.params, args.empty() ? nullptr : &s, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  std::vector<const Ty*> a = impl->traitArgs(*trait, ctx, 0);
  bool hasPack = false;
  for (Node* arg : trait->templateArgs)
    hasPack = hasPack || arg->kind == NodeKind::PackType; // `isConstructible<T, Args...>`: any number of types
  if (a.empty() || (!hasPack && a.size() != trait->templateArgs.size()))
    return std::nullopt;
  return impl->traitValue(impl->symbols.name(trait->symbol), a);
}

void GlobalScope::deduceArgs(FunctionDecl* f, const std::vector<const Ty*>& argTypes, TemplateArgs& args)
{
  Subst s = toSubst(args);
  const Ty* ft = impl->declaredTypeOf(f, s.empty() ? nullptr : &s, 0);
  impl->deduceCall(f, ft, argTypes, s);
  impl->fillDefaultTemplateArgs(f, s); // `template<typename = EnableIf<...>>`: the default, as C++ fills it
  for (Decl* p : f->templateParams)
  {
    const Ty* t = s.find(p);
    if (!t && static_cast<TemplateParamDecl*>(p)->isPack)
      t = impl->types.pack({}); // a pack no argument deduces is empty (`OnHeap(Args&&...)` default-constructing)
    if (t)
    {
      bool known = false;
      for (auto& [d, ty] : args.pairs)
        known = known || d == p;
      if (!known)
        args.pairs.push_back({p, t});
    }
  }
}

bool GlobalScope::constraintsHold(FunctionDecl* f, const TemplateArgs& args)
{
  return impl->constraintsHold(f, toSubst(args));
}

FunctionDecl* GlobalScope::chooseOverload(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes,
                                          const TemplateArgs& classArgs, const Ty* objectType, bool* ambiguous,
                                          const std::vector<bool>* nullConstants)
{
  Subst s = toSubst(classArgs);
  bool amb = false;
  FunctionDecl* f = impl->chooseAmong(candidates, argTypes, classArgs.empty() ? nullptr : &s, objectType, amb, nullptr, nullConstants);
  if (ambiguous)
    *ambiguous = amb;
  return f;
}

FunctionDecl* GlobalScope::closureCallOperator(const Ty* lambdaTy)
{
  Impl::ClosureCall* c = impl->closureCall(lambdaTy);
  return c ? &c->decl : nullptr;
}

const Ty* GlobalScope::closureOfCallOperator(const FunctionDecl* f)
{
  Impl::ClosureCall* c = impl->closureCallOf(f);
  return c ? c->closure : nullptr;
}

FunctionDecl* GlobalScope::chooseConstructor(const Ty* classType, const std::vector<const Ty*>& argTypes,
                                            const std::vector<bool>* nullConstants, const Ty** inheritedFrom, bool implicitOnly)
{
  return impl->chooseConstructor(classType, argTypes, nullConstants, inheritedFrom, implicitOnly);
}

FunctionDecl* GlobalScope::chooseConversion(const Ty* from, const Ty* to, ConversionKind kind, bool explicitCast, const Ty** foundIn, TemplateArgs* opArgs,
                                            bool contextualBool)
{
  Subst args;
  FunctionDecl* f = impl->chooseConversion(from, to, kind, explicitCast, foundIn, &args, contextualBool);
  if (opArgs)
  {
    opArgs->pairs.clear();
    for (const Subst::Pair& p : args.pairs)
      opArgs->pairs.push_back({p.param, p.type});
  }
  return f;
}

TemplateArgs GlobalScope::classArgsOf(const Ty* classType)
{
  TemplateArgs out;
  Subst s = impl->substFor(classType);
  for (const Subst::Pair& p : s.pairs)
    out.pairs.push_back({p.param, p.type});
  return out;
}

// f's template parameters bound to args, `this` of objectType.
// What is walked in a class of one element's copy of a `for constexpr` body: the loops' variables, which no declaration
// around it has as its template parameters do, as locals of the walk.
template<typename Locals>
static void pushLoopVariables(Locals& locals, const Subst& subst)
{
  for (const Subst::Pair& p : subst.pairs)
    if (p.param && p.param->kind == NodeKind::TemplateParamDecl && !locals.find(static_cast<TemplateParamDecl*>(p.param)->name))
      locals.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
}

const Ty* GlobalScope::typeFunctionFor(FunctionDecl* f, FunctionDecl* nodes, const TemplateArgs& args, const Ty* objectType, bool retypingElements,
                                       bool keepBranches)
{
  Impl& impl = *this->impl;
  if (!f->file || !f->file->tokens)
    return nullptr;
  Subst subst = toSubst(args);
  for (Decl* p = f; p; p = p->parent) // (`L::size()` of such a class: called with no arguments of its own)
    if (p->elementMark && (p->kind == NodeKind::ClassDecl || p->kind == NodeKind::EnumDecl))
    {
      subst = impl.withInstanceSubst(f, &subst);
      break;
    }
  ResolveStats scratch;
  Impl::Walker w(impl, scratch, *f->file->tokens, f->file->text);
  FileOfLookups walkedFile(f->file);
  w.retypingElements = retypingElements;
  w.keepConstexprBranches = keepBranches;
  if (!args.elementPacks.empty())
  {
    w.elementPacks = &args.elementPacks;
    w.elementIndex = args.elementIndex;
    w.elementPattern = args.elementPattern;
  }
  for (Decl* p = f; p; p = p->parent)
    for (Decl* tp : p->templateParams)
      w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
  pushLoopVariables(w.locals, subst);
  // The qualifier of an out-of-line definition (`ScrollBar<Policy>::ScrollBar`) is looked up
  // from the definition's own namespace, where the class is visible.
  w.scope = impl.lexicalScopeOf(f);
  if (f->name.parts.size() > 1)
    w.scope = w.semanticScopeFor(f->name);
  w.activeSubst = subst.pairs.empty() ? nullptr : &subst; // no arguments: the generic pass again (lambda types keep their identity)
  static bool debugCalls = getenv("KPP_DEBUG_CALLS") != nullptr;
  if (debugCalls)
  {
    std::string line = "retype " + impl.debugName(f->name.last().symbol) + ":";
    for (auto& [d, ty] : subst.pairs)
      line += " " + impl.debugName(static_cast<TemplateParamDecl*>(d)->name) + "=" + impl.types.toString(ty, &impl.symbols);
    fprintf(stderr, "%s\n", line.c_str());
  }
  if (objectType)
    w.thisType = impl.thisTypeOf(objectType);
  else if (Decl* cls = Impl::enclosingClassDecl(w.scope))
    w.thisType = impl.types.pointer(impl.substitute(impl.selfType(cls), subst));
  if (nodes != f && nodes->body)
    impl.indexLocalTypes(nodes->body, w.scope, nodes); // the copy's local classes and enums: scopes of their own members
  w.returnFrames.push_back(nullptr);
  w.walkDecl(nodes);
  const Ty* deduced = w.returnFrames.back();
  w.returnFrames.pop_back();
  if (!deduced)
    return nullptr;
  return impl.deducedShape(nodes->returnType && !nodes->trailingReturn ? w.typeOf(nodes->returnType) : nullptr, deduced);
}

void GlobalScope::retypeFor(FunctionDecl* f, const TemplateArgs& args, const Ty* objectType, bool packElement)
{
  typeFunctionFor(f, f, args, objectType, true, packElement);
}

FunctionDecl* GlobalScope::instanceBody(FunctionDecl* f, const TemplateArgs& args, const Ty* objectType)
{
  if (args.empty() || !f->file || !f->file->tokens)
    return f;
  // The record the checker's instantiation checks use too (the same `this` type as theirs), so
  // that what they typed is used as it is.
  Impl& impl = *this->impl;
  Subst subst = toSubst(args);
  const Ty* thisType = nullptr;
  if (objectType)
    thisType = impl.thisTypeOf(objectType);
  else if (Decl* cls = owningClass(f))
    thisType = impl.types.pointer(impl.substitute(impl.selfType(cls), subst));
  bool producer;
  Impl::InstanceRecord* record = impl.claimInstance(f, subst, thisType, producer);
  std::optional<RecordInto> into; // what typing the instance reads is its own
  if (producer)
    into.emplace(&record->deps);
  if (!producer)
  {
    if (record->state.load(std::memory_order_acquire) != 2)
    {
      if (record->owner == std::this_thread::get_id())
        return record->body ? record->body : f; // asked for again while being typed: what exists so far
      for (int seen = record->state.load(std::memory_order_acquire); seen != 2; seen = record->state.load(std::memory_order_acquire))
        record->state.wait(seen, std::memory_order_acquire); // blocks (C++20 atomic wait) instead of spinning on yield
    }
    return record->body ? record->body : f;
  }
  impl.declarationOfDefinition(f); // `static` and `virtual` onto an out-of-line definition before it is copied
  InstanceCopier copier(impl.instanceArena());
  FunctionDecl* copy = copier.function(f);
  copy->instanceMark = impl.instanceMarkOf(subst);
  record->body = copy;
  record->deducedReturn = typeFunctionFor(f, copy, args, objectType);
  record->state.store(2, std::memory_order_release);
  record->state.notify_all();
  return copy;
}

FunctionDecl* GlobalScope::privateInstance(FunctionDecl* f, const TemplateArgs& args, const Ty* objectType)
{
  if (!f->file || !f->file->tokens)
    return f;
  FunctionDecl* shared = instanceBody(f, args, objectType);
  impl->declarationOfDefinition(f);
  InstanceCopier copier(impl->instanceArena());
  copier.keepClosures = true; // its lambdas are the instantiation's types
  FunctionDecl* copy = copier.function(shared);
  copy->instanceMark = shared->instanceMark;
  typeFunctionFor(f, copy, args, objectType);
  return copy;
}

bool GlobalScope::converts(const Ty* from, const Ty* to)
{
  return from && to && impl->conversionRank(from, to) < Impl::NotViable;
}

bool GlobalScope::isExplicit(FunctionDecl* f, const TemplateArgs& args)
{
  Subst s = toSubst(args);
  return impl->isExplicit(f, args.empty() ? nullptr : &s);
}

bool GlobalScope::isNoexcept(FunctionDecl* f, const TemplateArgs& args)
{
  Subst s = toSubst(args);
  std::optional<bool> v = impl->isNoexcept(f, args.empty() ? nullptr : &s);
  return !v || *v; // (a condition that is no constant: as declared, noexcept)
}

bool GlobalScope::constructsWithoutThrowing(const Ty* t, bool move)
{
  return impl->constructibleFromSelf(t, move, Impl::TraitForm::Nothrow).value_or(false);
}

std::optional<bool> GlobalScope::destructorMayThrow(const Ty* t)
{
  return impl->destructorMayThrow(t);
}

void GlobalScope::lambdaNames(Node* n, std::unordered_set<Decl*>& inner, std::vector<Decl*>& used, bool& usesThis,
                              std::unordered_map<Decl*, const Ty*>* useTypes, int depth, bool keptBranches, std::vector<BindingUse>* bindingUses)
{
  impl->lambdaNames(n, inner, used, usesThis, useTypes, depth, keptBranches, bindingUses);
}

std::optional<std::vector<GlobalScope::ClosurePart>> GlobalScope::closureValueParts(const Ty* lambdaTy)
{
  return impl->closureValueParts(lambdaTy);
}

const Ty* GlobalScope::destructorLoosens(const Ty* t)
{
  return impl->destructorLoosens(t);
}

bool GlobalScope::hasVirtualDestructor(const Ty* t)
{
  return impl->hasVirtualDestructor(t);
}

Expr* GlobalScope::instanceInitializer(VariableDecl* v, const TemplateArgs& args)
{
  if (args.empty() || !v->init || !v->file || !v->file->tokens)
    return v->init;
  Impl& impl = *this->impl;
  Subst subst = toSubst(args);
  bool producer;
  Impl::InstanceRecord* record = impl.claimInstance(v, subst, nullptr, producer);
  std::optional<RecordInto> into; // what typing the instance reads is its own
  if (producer)
    into.emplace(&record->deps);
  if (!producer)
  {
    if (record->state.load(std::memory_order_acquire) != 2 && record->owner != std::this_thread::get_id())
      for (int seen = record->state.load(std::memory_order_acquire); seen != 2; seen = record->state.load(std::memory_order_acquire))
        record->state.wait(seen, std::memory_order_acquire); // blocks (C++20 atomic wait) instead of spinning on yield
    return record->init ? record->init : v->init; // (on the producing thread while it types: the copy so far)
  }
  InstanceCopier copier(impl.instanceArena());
  Expr* copy = copier.expression(v->init);
  record->init = copy;
  ResolveStats scratch;
  Impl::Walker w(impl, scratch, *v->file->tokens, v->file->text);
  FileOfLookups walkedFile(v->file);
  for (Decl* p = v; p; p = p->parent)
    for (Decl* tp : p->templateParams)
      w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
  pushLoopVariables(w.locals, subst);
  w.scope = impl.lexicalScopeOf(v);
  if (v->name.parts.size() > 1)
    w.scope = w.semanticScopeFor(v->name);
  w.activeSubst = &subst;
  if (Decl* cls = Impl::enclosingClassDecl(w.scope))
    w.thisType = impl.types.pointer(impl.substitute(impl.selfType(cls), subst));
  w.walkExpr(copy);
  record->state.store(2, std::memory_order_release);
  record->state.notify_all();
  return copy;
}

std::optional<bool> GlobalScope::requirementsHold(RequiresExpr* r, Decl* owner, const TemplateArgs& args)
{
  Subst s = toSubst(args);
  return impl->requirementsHold(r, owner, s);
}

VariableDecl* GlobalScope::variableTemplateFor(VariableDecl* primary, const TemplateArgs& args, TemplateArgs& specArgs)
{
  specArgs = args;
  std::vector<const Ty*> given;
  for (Decl* p : primary->templateParams)
  {
    if (static_cast<TemplateParamDecl*>(p)->isPack)
      return primary; // specialisations over a pack are not matched (none in the library or the game)
    const Ty* t = nullptr;
    for (auto& [d, ty] : args.pairs)
      if (d == p)
        t = ty;
    given.push_back(t);
  }
  Subst s;
  VariableDecl* v = impl->variableSpecialization(primary, given, s);
  if (v && v != primary)
  {
    specArgs.pairs.clear();
    for (const Subst::Pair& p : s.pairs)
      specArgs.pairs.push_back({p.param, p.type});
  }
  return v;
}

const Ty* GlobalScope::lambdaInstance(const Ty* lambdaTy, const TemplateArgs& own, TemplateArgs* fullArgs, LambdaExpr** body)
{
  Subst s = toSubst(own), full;
  const Ty* callTy = impl->checkLambdaInstance(lambdaTy, s, &full, nullptr, body);
  if (fullArgs)
  {
    fullArgs->pairs.clear();
    for (const auto& p : full.pairs)
      fullArgs->pairs.push_back({p.param, p.type});
  }
  return callTy;
}

LambdaExpr* GlobalScope::privateLambdaInstance(const Ty* lambdaTy, LambdaExpr* body, const TemplateArgs& args)
{
  Impl::LambdaEnv env;
  if (!body || !impl->lambdaEnvOf(lambdaTy, env))
    return nullptr;
  LambdaExpr* copy = InstanceCopier(impl->instanceArena()).lambdaInstance(body);
  impl->typeLambdaCopy(copy, env, toSubst(args), nullptr, false);
  return copy;
}

void GlobalScope::retypeLambdaFor(const Ty* lambdaTy, LambdaExpr* body, const TemplateArgs& args, bool packElement)
{
  Impl::LambdaEnv env;
  if (body && impl->lambdaEnvOf(lambdaTy, env))
    impl->typeLambdaCopy(body, env, toSubst(args), &args, packElement);
}

size_t GlobalScope::scopeCount() const { return impl->scopes.load(); }
size_t GlobalScope::entryCount() const { return impl->entries.load(); }
size_t GlobalScope::typeCount() const { return impl->types.size(); }
size_t GlobalScope::typeCountWithoutUnknown() const { return impl->types.sizeWithoutUnknown(); }

void GlobalScope::recordDependencies(bool on) { recordingDependencies = on; }

void GlobalScope::resolveDone()
{
  impl->afterResolve = true;
  std::lock_guard lock(membersOutsideMutex);
  if (membersOutsideSink == &impl->membersReadOutside)
    membersOutsideSink = nullptr; // (codegen's reads: a layout edit runs it in full)
}

void GlobalScope::beginRewalk()
{
  {
    std::lock_guard lock(membersOutsideMutex);
    membersOutsideSink = &impl->membersReadOutside;
  }
  {
    std::lock_guard made(impl->rewalkMadeMutex);
    impl->rewalkRecords.clear();
    impl->rewalkClasses.clear();
  }
  impl->rewalkTouchedLate = false;
  impl->rewalking = true;
}

bool GlobalScope::endRewalk()
{
  {
    std::lock_guard lock(membersOutsideMutex);
    if (membersOutsideSink == &impl->membersReadOutside)
      membersOutsideSink = nullptr;
  }
  impl->rewalking = false;
  return !impl->rewalkTouchedLate;
}

namespace
{
  // The walk unit's declaration, from its key in Impl::declDependencies (its address and the
  // unit's kind in the low bits a declaration's alignment leaves free).
  const Decl* walkUnitDecl(uintptr_t key) { return reinterpret_cast<const Decl*>(key & ~uintptr_t(7)); }
}

void GlobalScope::forgetWalks(const ParsedFile& file)
{
  std::unordered_set<const void*> units;
  for (Impl::DeclDependencyShard& shard : impl->declDependencies)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [key, deps] : shard.map)
      if (walkUnitDecl(key)->file == &file)
      {
        std::lock_guard l(deps->mutex);
        deps->list = DependencyList{};
        deps->tidied = true;
        units.insert(deps.get());
      }
  }
  // (and the errors they reported: the walk reports what it still finds)
  impl->forgetErrorsOf(units);
  // The types the old walk cached on its functions' locals (a deduced auto, a class template's
  // arguments from its initializer): kept, the walk again took them for the answer instead of
  // deducing them from what they read now (a return type edited: `Boxed b(unitDepth())` kept
  // Boxed<int>, its initializer Boxed<long long>).
  forEachNode(file.decls, [](Node* n)
  {
    if (n->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(n)->body)
      forEachNode(static_cast<FunctionDecl*>(n)->body, [](Node* m)
      {
        if (m->kind == NodeKind::VariableDecl)
          static_cast<VariableDecl*>(m)->cachedType.store(nullptr, std::memory_order_release);
      });
  });
}

std::vector<const void*> GlobalScope::instancesWalksUse(const ParsedFile& file)
{
  std::vector<const void*> out;
  for (Impl::DeclDependencyShard& shard : impl->declDependencies)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [key, deps] : shard.map)
      if (walkUnitDecl(key)->file == &file)
      {
        std::lock_guard l(deps->mutex);
        for (auto& [kind, what] : deps->list.uses)
          if (kind == USE_Instance || kind == USE_ClassInstance)
            out.push_back(what);
      }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

uint32_t GlobalScope::functionSymbolOf(const FunctionDecl* f) { return impl->functionSymbol(f->name); }

GlobalScope::ChangedName GlobalScope::changedNameOf(const VariableDecl* v)
{
  ChangedName out{v->name.empty() ? UINT32_MAX : v->name.last().symbol, nullptr};
  if (v->parent && v->parent->kind == NodeKind::ClassDecl)
    out.classScope = v->parent->ownScope ? static_cast<const void*>(v->parent->ownScope) : ChangedName::anyClass;
  out.memberOnly = out.classScope != nullptr;
  return out;
}

GlobalScope::ChangedName GlobalScope::changedNameOf(const FunctionDecl* f)
{
  ChangedName out{impl->functionSymbol(f->name), nullptr};
  if (f->parent && f->parent->kind == NodeKind::ClassDecl)
    out.classScope = f->parent->ownScope ? static_cast<const void*>(f->parent->ownScope) : ChangedName::anyClass;
  else if (auto it = impl->outOfLineScope.find(f); it != impl->outOfLineScope.end())
    out.classScope = it->second;
  else if (f->name.parts.size() > 1)
    out.classScope = ChangedName::anyClass; // `X::f` whose class is not known here: every class's f
  out.memberOnly = out.classScope && !(f->flags & DF_Friend); // (a friend defined in its class is found by ADL, recorded by name)
  return out;
}

namespace
{
  // Who read one of the changed names: a lookup of the symbol recorded by name alone (a namespace's
  // scope, a using-declaration's), or in a changed member's class or a class derived from it (the
  // lookups there reach the base), or in any class for a member whose class is not known.
  struct NameReaders
  {
    bool reads(const DependencyList& l) const
    {
      if (symbols.empty() && members.empty() && anyClass.empty())
        return false; // (no name changed: a body edit's; a list of millions is not walked for nothing)
      for (RecordedName n : l.names)
        if (!isMemberName(n) ? symbols.count(recordedSymbol(n)) != 0 : members.count(n) || anyClass.count(recordedSymbol(n)))
          return true;
      return false;
    }

    std::unordered_set<uint32_t> symbols;
    std::unordered_set<RecordedName> members;
    std::unordered_set<uint32_t> anyClass;
  };

  // byClassOnly: a member's name is read only through its class or a class derived from it: a lookup
  // by name alone (a namespace's) never reaches a member, whose readers look it up in a class, and a
  // class's lookup that finds a using-declaration is recorded with the class too (Scope::find). A
  // friend defined in its class is found by ADL, recorded by name: it is no memberOnly name.
  // f(i) for every i in [0, count) on the machine's threads. A compile server's edit scans every
  // unit's recorded dependencies (millions of names and uses; 0.8 s on one thread for the game): each
  // scan keeps what it finds per shard, joined in shard order after, as one thread finds it.
  template<typename F>
  void forEachParallel(size_t count, F&& f)
  {
    unsigned threads = unsigned(std::min<size_t>(std::max(1u, std::thread::hardware_concurrency()), count));
    std::atomic<size_t> next{0};
    auto worker = [&]
    {
      for (size_t i; (i = next.fetch_add(1)) < count;)
        f(i);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t)
      pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool)
      t.join();
  }

  NameReaders nameReaders(const std::vector<GlobalScope::ChangedName>& changed, const std::vector<const Scope*>& relationScopes, bool byClassOnly = false)
  {
    NameReaders out;
    std::unordered_map<const Scope*, std::vector<const Scope*>> derived; // a class scope -> the classes naming it as a base
    bool derivedKnown = false;
    for (const GlobalScope::ChangedName& c : changed)
    {
      if (!byClassOnly || !c.memberOnly)
        out.symbols.insert(c.symbol);
      if (c.classScope == GlobalScope::ChangedName::anyClass)
        out.anyClass.insert(c.symbol);
      else if (c.classScope)
      {
        if (!derivedKnown)
        {
          for (const Scope* s : relationScopes)
            if (s->baseState.load(std::memory_order_acquire) == 2)
              for (const Scope* b : s->baseScopes)
                derived[b].push_back(s);
          derivedKnown = true;
        }
        std::vector<const Scope*> pending{static_cast<const Scope*>(c.classScope)};
        std::unordered_set<const Scope*> seen;
        while (!pending.empty())
        {
          const Scope* s = pending.back();
          pending.pop_back();
          if (!seen.insert(s).second)
            continue;
          out.members.insert(memberName(s->id, c.symbol));
          if (auto it = derived.find(s); it != derived.end())
            pending.insert(pending.end(), it->second.begin(), it->second.end());
        }
      }
    }
    return out;
  }
}

std::vector<FunctionDecl*> GlobalScope::outOfLineDefinitions(const Decl* cls)
{
  return cls && cls->ownScope ? cls->ownScope->outOfLineMembers : std::vector<FunctionDecl*>{};
}

void GlobalScope::forEachInstanceCopy(const ParsedFile& file, const std::function<void(Node* copy, const Decl* source)>& visit)
{
  // (the shards scanned on every thread, the full game's in a body edit; the copies visited in the
  // shards' order after)
  std::vector<std::vector<std::pair<Node*, const Decl*>>> copies(std::size(impl->instanceShards));
  forEachParallel(std::size(impl->instanceShards), [&](size_t i)
  {
    Impl::InstanceShard& shard = impl->instanceShards[i];
    std::lock_guard lock(shard.mutex);
    for (auto& [key, record] : shard.map)
    {
      const Decl* source = record->recheck ? record->recheck->source : record->of;
      if (record->body && record->body->file == &file)
        copies[i].push_back({record->body, source});
      if (record->init && record->of && record->of->file == &file)
        copies[i].push_back({record->init, source});
      if (record->lambda && record->of && record->of->file == &file)
        copies[i].push_back({record->lambda, source});
    }
  });
  for (const auto& shard : copies)
    for (auto [copy, source] : shard)
      visit(copy, source);
}

void GlobalScope::forEachInstanceCopy(const std::function<void(Node* copy, const Decl* source, const ParsedFile& file)>& visit)
{
  for (Impl::InstanceShard& shard : impl->instanceShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [key, record] : shard.map)
    {
      const Decl* source = record->recheck ? record->recheck->source : record->of;
      if (record->body && record->body->file)
        visit(record->body, source, *record->body->file);
      if (record->init && record->of && record->of->file)
        visit(record->init, source, *record->of->file);
      if (record->lambda && record->of && record->of->file)
        visit(record->lambda, source, *record->of->file);
    }
  }
}

void GlobalScope::forEachInstanceRoot(const std::function<void(Node* copy, const ParsedFile& file, const ParsedFile* rootFile, uint32_t rootToken)>& visit)
{
  for (Impl::InstanceShard& shard : impl->instanceShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [key, record] : shard.map)
      if (record->body && record->body->file)
        visit(record->body, *record->body->file, record->rootFile, record->rootToken);
  }
}

void GlobalScope::forEachInstanceStatsPlace(uint32_t fileIndex, const ParsedFile* file, const std::function<void(uint32_t& token)>& visit)
{
  std::lock_guard lock(impl->instanceStatsMutex);
  for (Impl::InstanceContribution& c : impl->instanceContributions)
  {
    for (auto& [symbol, where] : c.stats->unresolvedFirst)
      if (where.file == fileIndex)
        visit(where.token);
    for (auto& [symbol, where] : c.stats->missingMembersFirst)
      if (where.file == fileIndex)
        visit(where.token);
    for (auto& [key, site] : c.stats->why)
      if (site.file == file)
        visit(site.token);
  }
}

ResolveStats GlobalScope::instanceStatsAtResolveEnd()
{
  std::lock_guard lock(impl->instanceStatsMutex);
  ResolveStats total;
  for (const Impl::InstanceContribution& c : impl->instanceContributions)
    if (!c.late)
      total.add(*c.stats);
  return total;
}

std::vector<FunctionDecl*> GlobalScope::withBodyCopies(const std::vector<FunctionDecl*>& functions)
{
  std::unordered_set<const void*> wanted(functions.begin(), functions.end());
  if (wanted.empty())
    return {};
  // (the shards scanned on every thread: the full game's, in every body edit)
  std::vector<std::unordered_set<const void*>> found(std::size(impl->instanceShards));
  forEachParallel(std::size(impl->instanceShards), [&](size_t i)
  {
    Impl::InstanceShard& shard = impl->instanceShards[i];
    std::lock_guard lock(shard.mutex);
    for (auto& [key, record] : shard.map)
    {
      if (record->recheck && wanted.count(record->recheck->source))
        found[i].insert(record->recheck->source);
      if (wanted.count(record->of))
        found[i].insert(record->of);
    }
  });
  std::vector<FunctionDecl*> out;
  for (FunctionDecl* f : functions)
    if (std::any_of(found.begin(), found.end(), [&](const auto& set) { return set.count(f) != 0; }))
      out.push_back(f);
  return out;
}

bool GlobalScope::recheckInstancesOf(const std::vector<FunctionDecl*>& sources, const std::vector<const void*>& readers, std::string& why, size_t* dropped,
                                     std::vector<const Decl*>* instancesOf)
{
  std::unordered_set<const void*> changed(sources.begin(), sources.end());
  std::vector<Impl::InstanceRecord*> stale;
  // An instance codegen made (late) is taken out of the table instead of checked again, with the
  // instances codegen made whose checks used it: codegen asks for them again and they are made anew
  // from the bodies as they are, as in a build from scratch (where codegen makes them). Among them
  // the copy of a plain function the evaluator ran for codegen (a constant a global's initializer
  // or a body folds): until 2026-11-02 such a copy made every edit of the function a build from
  // scratch (the game's MapPosition::operator+, the wide edit: 1.9 s -> 20 to 60 s).
  std::unordered_set<const Impl::InstanceRecord*> drop;
  for (Impl::InstanceShard& shard : impl->instanceShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [key, record] : shard.map)
      if ((record->recheck && changed.count(record->recheck->source)) || changed.count(record->of))
      {
        if (record->state.load() != 2)
          return why = "an instance of a changed function not finished" +
                       (record->of ? ": " + impl->accessName(const_cast<Decl*>(record->of)) + " (" + Impl::locationOf(record->of->file, record->of->token) + ")"
                                   : std::string()),
                 false;
        if (record->late)
          drop.insert(record.get());
        else if (!record->recheck)
          return why = "an instance of a changed function with no record of how it was checked" +
                       (record->of ? ": " + impl->accessName(const_cast<Decl*>(record->of)) : std::string()),
                 false;
        else
          stale.push_back(record.get());
      }
  }
  // The instances whose recorded reads met the change (invalidateReaders), checked again from their
  // templates' bodies as they are; a late one dropped, as above.
  size_t unrecorded = 0, unfinished = 0;
  for (const void* r : readers)
  {
    auto* record = static_cast<Impl::InstanceRecord*>(const_cast<void*>(r));
    if (record->state.load() != 2)
      ++unfinished;
    else if (record->late)
      drop.insert(record);
    else if (!record->recheck)
      ++unrecorded;
    else
      stale.push_back(record);
  }
  if (unrecorded || unfinished)
    return why = "instances read a change and cannot be checked again: " + std::to_string(unrecorded) + " with no record of how they were checked, " +
                 std::to_string(unfinished) + " unfinished (of " + std::to_string(readers.size()) + ")",
           false;
  if (!drop.empty())
  {
    // The late instances whose checks used a dropped one, transitively (only late ones can: an
    // instance of the resolve that used a late one would be late itself).
    std::unordered_map<const void*, std::vector<Impl::InstanceRecord*>> users;
    for (Impl::InstanceShard& shard : impl->instanceShards)
    {
      std::lock_guard lock(shard.mutex);
      for (auto& [key, record] : shard.map)
        if (record->late)
        {
          std::lock_guard l(record->deps.mutex);
          for (auto& [kind, what] : record->deps.list.uses)
            if (kind == USE_Instance)
              users[what].push_back(record.get());
        }
    }
    std::vector<const Impl::InstanceRecord*> pending(drop.begin(), drop.end());
    while (!pending.empty())
    {
      const Impl::InstanceRecord* r = pending.back();
      pending.pop_back();
      if (auto it = users.find(r); it != users.end())
        for (Impl::InstanceRecord* u : it->second)
          if (drop.insert(u).second)
            pending.push_back(u);
    }
    for (Impl::InstanceShard& shard : impl->instanceShards)
    {
      std::lock_guard lock(shard.mutex);
      for (auto it = shard.map.begin(); it != shard.map.end();)
        if (drop.count(it->second.get()))
        {
          impl->droppedInstances.push_back(std::move(it->second));
          it = shard.map.erase(it);
          Impl::instanceGeneration.fetch_add(1, std::memory_order_acq_rel); // (no thread's claimInstance answers with it now)
          if (dropped)
            ++*dropped;
        }
        else
          ++it;
    }
    {
      std::lock_guard lock(impl->instanceStatsMutex);
      auto& list = impl->instanceContributions;
      list.erase(std::remove_if(list.begin(), list.end(), [&](const Impl::InstanceContribution& c) { return drop.count(static_cast<const Impl::InstanceRecord*>(c.owner)); }),
                 list.end());
    }
    // (and the static_asserts their copies failed, which count once the copy is emitted: the new
    // copies check theirs)
    std::lock_guard lock(impl->checkerErrorsMutex);
    for (const Impl::InstanceRecord* r : drop)
      if (r->body)
      {
        impl->instanceAssertErrors.erase(r->body);
        impl->instanceNameErrors.erase(r->body);
        impl->instanceWarnings.erase(r->body);
      }
  }
  std::sort(stale.begin(), stale.end());
  stale.erase(std::unique(stale.begin(), stale.end()), stale.end());
  if (instancesOf)
  {
    // (the dropped records are kept in droppedInstances: their pointers still hold)
    std::unordered_set<const Decl*> templates;
    auto add = [&](const Impl::InstanceRecord* r)
    {
      if (r->of)
        templates.insert(r->of);
      if (r->recheck && r->recheck->source)
        templates.insert(r->recheck->source);
    };
    for (const Impl::InstanceRecord* r : stale)
      add(r);
    for (const Impl::InstanceRecord* r : drop)
      add(r);
    instancesOf->assign(templates.begin(), templates.end());
    std::sort(instancesOf->begin(), instancesOf->end(), [](const Decl* a, const Decl* b)
    {
      uint32_t fa = a->file ? a->file->index : UINT32_MAX, fb = b->file ? b->file->index : UINT32_MAX;
      return fa != fb ? fa < fb : a->token < b->token;
    });
  }
  impl->rechecking = true;
  Restore<bool> done(impl->rechecking, false);
  std::unordered_set<const void*> requestedBefore; // the instances the old checks asked for
  for (Impl::InstanceRecord* r : stale)
  {
    {
      std::lock_guard lock(r->deps.mutex);
      for (auto& [kind, what] : r->deps.list.uses)
        if (kind == USE_Instance || kind == USE_ClassInstance)
          requestedBefore.insert(what);
    }
    {
      // Its old statistics go; what it reads is recorded anew.
      std::lock_guard lock(impl->instanceStatsMutex);
      auto& list = impl->instanceContributions;
      list.erase(std::remove_if(list.begin(), list.end(), [&](const Impl::InstanceContribution& c) { return c.owner == r; }), list.end());
    }
    {
      std::lock_guard lock(r->deps.mutex);
      r->deps.list = DependencyList{};
      r->deps.tidied = true;
    }
    // Its errors go too: the check again reports what it still finds (in the new copy's body).
    impl->forgetErrorsOf({&r->deps});
    if (r->body)
    {
      std::lock_guard lock(impl->checkerErrorsMutex);
      impl->instanceAssertErrors.erase(r->body);
      impl->instanceNameErrors.erase(r->body);
      impl->instanceWarnings.erase(r->body);
    }
    const Ty* deducedBefore = r->deducedReturn;
    Impl::InstanceRecord::Recheck& rc = *r->recheck;
    if (rc.member)
    {
      RecordInto into(&r->deps);
      FunctionDecl* copy = impl->copyInstance(rc.source, rc.scope, rc.subst);
      r->body = copy;
      r->rootFile = rc.rootFile;
      r->rootToken = rc.rootToken;
      ResolveStats mine;
      mine.fileIndex = rc.fileIndex;
      mine.collectWhy = rc.collectWhy;
      Impl::Walker member(*impl, mine, *rc.file->tokens, rc.file->text);
      member.file = rc.file;
      FileOfLookups walkedFile(rc.file);
      member.scope = rc.scope;
      member.thisType = rc.thisType;
      member.activeSubst = &rc.subst;
      member.instantiationDepth = rc.depth;
      member.requestFile = rc.requestFile;
      member.requestToken = rc.requestToken;
      member.rootFile = rc.rootFile;
      member.rootToken = rc.rootToken;
      member.locals.items = rc.locals;
      member.returnFrames.push_back(nullptr);
      member.walkDecl(copy);
      const Ty* deduced = member.returnFrames.back();
      member.returnFrames.pop_back();
      r->deducedReturn = deduced ? impl->deducedShape(copy->returnType && !copy->trailingReturn ? member.typeOf(copy->returnType) : nullptr, deduced)
                                 : nullptr;
      mine.instExprsTyped += mine.exprsTyped;
      mine.instExprsDependent += mine.exprsDependent;
      mine.instExprsUnknown += mine.exprsUnknown;
      impl->addInstanceStats(mine, r);
    }
    else if (rc.lambdaTy)
    {
      // A generic lambda's call operator: copied from the lambda as written and walked again (the
      // lambda's own walk, if its file was walked again, registered it anew).
      Impl::LambdaEnv env;
      {
        std::lock_guard lock(impl->lambdaEnvMutex);
        auto it = impl->lambdaEnvs.find(rc.lambdaTy);
        if (it == impl->lambdaEnvs.end())
          return why = "a generic lambda's instance whose lambda is gone", false;
        env = it->second;
      }
      if (!env.lambda || !env.tokens || !env.file || env.lambda->closure != r->of)
        return why = "a generic lambda's instance whose lambda is gone", false;
      Subst full = env.subst;
      for (const Subst::Pair& p : rc.subst.pairs)
        if (!full.find(p.param))
          full.pairs.push_back(p);
      RecordInto into(&r->deps);
      ResolveStats stats;
      stats.fileIndex = env.file->index;
      impl->runLambdaInstance(r, env, full, &stats);
    }
    else
    {
      Impl::FunctionInstanceTask task = *rc.task;
      const ParsedFile* f = task.def ? task.def->file : nullptr;
      if (!f || !f->tokens)
        return why = "an instance of a changed template without its file", false;
      ResolveStats scratch;
      Impl::Walker w(*impl, scratch, *f->tokens, f->text);
      w.file = f;
      FileOfLookups walkedFile(f);
      w.runFunctionInstance(r, task); // (the record keeps a new Recheck, the same)
    }
    if (r->deducedReturn != deducedBefore)
      return why = "an instance of a changed template deduces another return type", false;
  }
  // An instance the old checks asked for and nothing asks for now would not be in a build from scratch.
  if (!requestedBefore.empty())
  {
    auto drop = [&](Dependencies& d)
    {
      std::lock_guard lock(d.mutex);
      for (auto& [kind, what] : d.list.uses)
        requestedBefore.erase(what);
    };
    for (Impl::DeclDependencyShard& shard : impl->declDependencies)
    {
      std::lock_guard lock(shard.mutex);
      for (auto& [key, deps] : shard.map)
        drop(*deps);
    }
    {
      std::lock_guard lock(impl->typeDependenciesMutex);
      for (auto& deps : impl->typeDependencies)
        drop(*deps);
    }
    for (Impl::InstanceShard& shard : impl->instanceShards)
    {
      std::lock_guard lock(shard.mutex);
      for (auto& [key, record] : shard.map)
        drop(record->deps);
    }
    for (Impl::InstShard& shard : impl->instShards)
    {
      std::lock_guard lock(shard.mutex);
      for (auto& [key, inst] : shard.classes)
        drop(inst->deps);
    }
    if (!requestedBefore.empty())
      return why = "an instance an old template body asked for that nothing asks for now", false;
  }
  return true;
}

namespace
{
  // Whether a declaration's declared type, changed, changes only what reads it through the recorded
  // uses: a function's or a parameter's, a template parameter's, an alias's (whose readers are
  // followed in turn), a local variable's of a function (not of a lambda or a local class). A data
  // member's is a class layout, a global's or a static member's a global codegen defines: what
  // uses those reads no declared type it records.
  bool typeChangeFollowed(const Decl* d)
  {
    switch (d->kind)
    {
      case NodeKind::FunctionDecl:
      case NodeKind::ParamDecl:
      case NodeKind::TemplateParamDecl:
      case NodeKind::UsingAliasDecl:
        return true;
      case NodeKind::VariableDecl:
        return (d->flags & DF_Typedef) || (d->parent && d->parent->kind == NodeKind::FunctionDecl);
      default:
        return false;
    }
  }

  // Whether a declaration's declared type is taken from its own walk: a `decltype(expr)` (the type
  // the walk gave expr) or a deduced `auto` in a variable's type or a function's signature. Its
  // declared-type unit records nothing of what the walk read, so a change its walk read cannot be
  // followed to its declared type (and from there to a layout or to the callers' types).
  bool typeFromWalk(const Decl* d)
  {
    bool found = false;
    auto look = [&](Node* t)
    {
      if (t)
        forEachNode(t, [&](Node* n) { found = found || n->kind == NodeKind::DecltypeType || n->kind == NodeKind::AutoType; });
    };
    if (d->kind == NodeKind::VariableDecl)
    {
      auto* v = static_cast<const VariableDecl*>(d);
      if (!v->type)
        return true;
      look(v->type);
    }
    else if (d->kind == NodeKind::UsingAliasDecl)
      look(static_cast<const UsingAliasDecl*>(d)->type);
    else if (d->kind == NodeKind::FunctionDecl)
    {
      auto* f = static_cast<const FunctionDecl*>(d);
      look(f->returnType);
      look(f->trailingReturn);
      for (Decl* p : f->params)
        look(static_cast<ParamDecl*>(p)->type);
    }
    return found;
  }

  // The scope a declaration's members are indexed in: its own (a namespace, a class, an enum), or
  // the global one for the file's top level.
  Scope* containerScope(Decl* container, Scope& root) { return container && container->ownScope ? container->ownScope : &root; }
}

void GlobalScope::unindexFunction(FunctionDecl* f)
{
  auto attachedTo = impl->outOfLineScope.find(f);
  if (attachedTo != impl->outOfLineScope.end())
  {
    std::vector<FunctionDecl*>& members = attachedTo->second->outOfLineMembers;
    members.erase(std::remove(members.begin(), members.end(), f), members.end());
    impl->outOfLineScope.erase(attachedTo);
  }
  else if (f->name.parts.size() == 1)
  {
    uint32_t symbol = impl->functionSymbol(f->name);
    Scope* scope = containerScope(f->parent, impl->root);
    Scope::Shard& shard = scope->shardFor(symbol);
    std::lock_guard lock(shard.mutex);
    if (auto it = shard.map.find(symbol); it != shard.map.end())
    {
      size_t before = it->second.size();
      it->second.erase(std::remove_if(it->second.begin(), it->second.end(), [&](const Entry& e) { return e.decl == f; }), it->second.end());
      impl->entries -= before - it->second.size();
      if (it->second.empty())
        shard.map.erase(it);
    }
  }
  impl->forgetUnitsOf(f);
  impl->forgetMergedOverloads();
}

void GlobalScope::unindexMember(VariableDecl* v)
{
  if (v->name.parts.size() == 1)
  {
    uint32_t symbol = v->name.parts[0].symbol;
    Scope* scope = containerScope(v->parent, impl->root);
    Scope::Shard& shard = scope->shardFor(symbol);
    std::lock_guard lock(shard.mutex);
    if (auto it = shard.map.find(symbol); it != shard.map.end())
    {
      size_t before = it->second.size();
      it->second.erase(std::remove_if(it->second.begin(), it->second.end(), [&](const Entry& e) { return e.decl == v; }), it->second.end());
      impl->entries -= before - it->second.size();
      if (it->second.empty())
        shard.map.erase(it);
    }
  }
  impl->forgetUnitsOf(v); // (its initializer's walk)
}

void GlobalScope::indexMembers(const std::vector<VariableDecl*>& added)
{
  for (VariableDecl* v : added)
    impl->indexDecl(v, containerScope(v->parent, impl->root));
  std::unordered_set<Scope*> seen;
  impl->sortEntries(&impl->root, seen);
  impl->forgetMergedOverloads();
}

bool GlobalScope::membersReadOutsideUnits(const Decl* cls)
{
  std::lock_guard lock(membersOutsideMutex);
  return impl->membersReadOutside.count(cls) != 0;
}

bool GlobalScope::indexFunctions(const std::vector<FunctionDecl*>& added)
{
  for (FunctionDecl* f : added)
    impl->indexDecl(f, containerScope(f->parent, impl->root));
  std::vector<FunctionDecl*> attached;
  impl->attachOutOfLine(attached);
  bool whole = impl->objectCandidates.empty() && impl->deferredQualified.empty() && impl->deferredVariables.empty();
  impl->objectCandidates.clear();
  for (FunctionDecl* f : attached)
  {
    Impl::TokenScope tokenScope(f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view());
    whole = whole && impl->declarationOfDefinition(f);
  }
  std::unordered_set<Scope*> seen;
  impl->sortEntries(&impl->root, seen); // in declaration order, as finishIndex leaves them
  impl->cFunctions.clear();
  impl->forgetMergedOverloads();
  return whole;
}

std::vector<const Decl*> GlobalScope::walksReading(const std::vector<ChangedName>& changed)
{
  std::vector<const Scope*> relations;
  {
    std::lock_guard lock(impl->relationScopesMutex);
    relations = impl->relationScopes;
  }
  NameReaders names = nameReaders(changed, relations, true);
  std::vector<std::vector<const Decl*>> found(std::size(impl->declDependencies));
  forEachParallel(std::size(impl->declDependencies), [&](size_t i)
  {
    Impl::DeclDependencyShard& shard = impl->declDependencies[i];
    std::lock_guard lock(shard.mutex);
    for (auto& [key, deps] : shard.map)
    {
      std::lock_guard l(deps->mutex);
      if (names.reads(deps->list))
        found[i].push_back(reinterpret_cast<const Decl*>(key & ~uintptr_t(7)));
    }
  });
  std::vector<const Decl*> out;
  for (const std::vector<const Decl*>& f : found)
    out.insert(out.end(), f.begin(), f.end());
  return out;
}

bool GlobalScope::invalidateReaders(const std::vector<ChangedName>& changed, const std::vector<const Decl*>& decls, std::vector<const ParsedFile*>& files,
                                    std::vector<const Decl*>* walks, const std::vector<const FunctionDecl*>& bodies, std::string* why,
                                    std::vector<const void*>* instances, std::vector<const Decl*>* typedByWalk,
                                    const std::vector<const Decl*>& classes)
{
  std::unordered_set<const void*> lists(classes.begin(), classes.end()); // classes whose member lists changed
  std::vector<const Scope*> relations;
  {
    std::lock_guard lock(impl->relationScopesMutex);
    relations = impl->relationScopes;
  }
  NameReaders names = nameReaders(changed, relations, true);
  std::unordered_set<const void*> types(decls.begin(), decls.end()); // declarations whose declared types are no more what was read
  std::unordered_set<const void*> ran(bodies.begin(), bodies.end());  // changed bodies the evaluator may have run
  auto fail = [&](const char* reason)
  {
    if (why)
      *why = reason;
    return false;
  };
  if (!ran.empty())
  {
    std::lock_guard lock(impl->ranOutsideMutex);
    for (const FunctionDecl* f : bodies)
      if (impl->ranOutside.count(f))
        return fail("the evaluator ran a changed body outside any recorded unit");
  }
  auto reads = [&](Dependencies& d)
  {
    std::lock_guard lock(d.mutex);
    if (names.reads(d.list))
      return true;
    for (auto& [kind, what] : d.list.uses)
      if ((kind == USE_DeclaredType && types.count(what)) || (kind == USE_RanBody && ran.count(what)) || (kind == USE_ClassMembers && lists.count(what)))
        return true;
    return false;
  };
  for (const Decl* c : classes)
    if (membersReadOutsideUnits(c))
      return fail("a changed class's member list was read outside any recorded unit");
  // The declared types that read them, and what read those: one pass for the direct readers, then
  // (only when there are some to follow) one for who reads what, followed from them.
  {
    std::lock_guard lock(impl->typeDependenciesMutex);
    std::unordered_map<const void*, std::vector<std::pair<const Decl*, Dependencies*>>> readersOf; // declaration -> declared types that read its type
    std::vector<std::pair<const Decl*, Dependencies*>> pending;
    {
      // (in chunks on all threads, each its direct readers and its first reason to fail, joined in order)
      const auto& owners = impl->typeDependencyOwners;
      constexpr size_t chunks = 256;
      std::vector<std::pair<const Decl*, Dependencies*>> direct[chunks];
      const char* failed[chunks] = {};
      forEachParallel(chunks, [&](size_t c)
      {
        for (size_t k = owners.size() * c / chunks, end = owners.size() * (c + 1) / chunks; k < end && !failed[c]; ++k)
        {
          auto [owner, deps] = owners[k];
          std::lock_guard l(deps->mutex);
          bool reads = names.reads(deps->list);
          for (auto& [kind, what] : deps->list.uses)
            if (kind == USE_DeclaredType)
              reads = reads || types.count(what);
            else if (kind == USE_RanBody && ran.count(what))
              failed[c] = "a declared type ran a changed body (an array bound, a constant template argument)";
            else if (kind == USE_ClassMembers && lists.count(what))
              failed[c] = "a declared type read a changed class's member list";
          if (reads)
            direct[c].push_back({owner, deps});
        }
      });
      for (size_t c = 0; c < chunks; ++c)
      {
        if (failed[c])
          return fail(failed[c]);
        pending.insert(pending.end(), direct[c].begin(), direct[c].end());
      }
    }
    if (!pending.empty())
      for (auto& [owner, deps] : impl->typeDependencyOwners)
      {
        std::lock_guard l(deps->mutex);
        for (auto& [kind, what] : deps->list.uses)
          if (kind == USE_DeclaredType)
            readersOf[what].push_back({owner, deps});
      }
    while (!pending.empty())
    {
      auto [owner, deps] = pending.back();
      pending.pop_back();
      if (!types.insert(owner).second)
        continue;
      if (!typeChangeFollowed(owner))
        return fail("the declared type of a data member, a global or a class read a changed function (a layout may change)");
      const_cast<Decl*>(owner)->cachedType.store(nullptr, std::memory_order_release);
      {
        std::lock_guard l(deps->mutex);
        deps->list = DependencyList{};
      }
      impl->forgetErrorsOf({deps});
      if (auto it = readersOf.find(owner); it != readersOf.end())
        pending.insert(pending.end(), it->second.begin(), it->second.end());
    }
  }
  {
    std::vector<std::vector<const void*>> reading(std::size(impl->instanceShards));
    std::vector<const char*> failed(std::size(impl->instanceShards), nullptr);
    forEachParallel(std::size(impl->instanceShards), [&](size_t i)
    {
      Impl::InstanceShard& shard = impl->instanceShards[i];
      std::lock_guard lock(shard.mutex);
      for (auto& [key, record] : shard.map)
        if (!failed[i] && reads(record->deps))
        {
          if (!instances)
            failed[i] = "an instance read a changed function's name or ran a changed body";
          // One that cannot be checked again fails the build here, before any file is walked again
          // (recheckInstancesOf would refuse it after the walks).
          else if (record->state.load() != 2 || (!record->late && !record->recheck))
            failed[i] = "an instance read a change and cannot be checked again (no record of how it was checked, or unfinished)";
          else
            reading[i].push_back(record.get()); // checked again (recheckInstancesOf)
        }
    });
    for (size_t i = 0; i < std::size(impl->instanceShards); ++i)
    {
      if (failed[i])
        return fail(failed[i]);
      if (instances)
        instances->insert(instances->end(), reading[i].begin(), reading[i].end());
    }
  }
  for (Impl::InstShard& shard : impl->instShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [key, inst] : shard.classes)
      if (reads(inst->deps))
        return fail("a class instance read a changed function's name or ran a changed body");
  }
  {
    std::lock_guard lock(impl->relationScopesMutex);
    for (const Scope* s : impl->relationScopes)
      if (reads(const_cast<Scope*>(s)->relationDeps))
        return fail("a scope's relations read a changed function's name or ran a changed body");
  }
  // The cached answers that ran a changed body are computed again by whoever asks next (every
  // unit that read one recorded its bodies, and is found above or below).
  if (!ran.empty() || !lists.empty() || !decls.empty())
  {
    impl->forgetCachedAnswers([&](const CollectedUse& u)
                              { return (u.first == USE_RanBody && ran.count(u.second)) || (u.first == USE_ClassMembers && lists.count(u.second)) ||
                                       (u.first == USE_DeclaredType && types.count(u.second)); });
  }
  std::unordered_set<const ParsedFile*> found;
  std::vector<std::vector<const Decl*>> reading(std::size(impl->declDependencies));
  forEachParallel(std::size(impl->declDependencies), [&](size_t i)
  {
    Impl::DeclDependencyShard& shard = impl->declDependencies[i];
    std::lock_guard lock(shard.mutex);
    for (auto& [key, deps] : shard.map)
      if (reads(*deps))
        reading[i].push_back(reinterpret_cast<const Decl*>(key & ~uintptr_t(7)));
  });
  for (const std::vector<const Decl*>& shardReading : reading)
    for (const Decl* d : shardReading)
    {
      if (typeFromWalk(d))
      {
        if (!typedByWalk)
          return fail("a declaration whose type its walk gives (decltype, auto) read a changed function");
        typedByWalk->push_back(d); // its type compared after the walk (forgetTypesFromWalks, typeOfDecl)
      }
      found.insert(d->file);
      if (walks)
        walks->push_back(d);
    }
  files.assign(found.begin(), found.end());
  std::sort(files.begin(), files.end(), [](const ParsedFile* a, const ParsedFile* b) { return a->index < b->index; });
  return true;
}

GlobalScope::InstantiationStats GlobalScope::rewalkCounts() const
{
  InstantiationStats out{};
  std::lock_guard made(impl->rewalkMadeMutex);
  for (const Impl::Instantiation* c : impl->rewalkClasses)
    out.classes += c->counted;
  for (const Impl::InstanceRecord* r : impl->rewalkRecords)
  {
    out.bodies += (r->counted & 1) != 0;
    out.functions += (r->counted & 2) != 0;
  }
  return out;
}

void GlobalScope::dropUnusedInstances(const std::vector<const void*>& instances, const ParsedFile& file, InstantiationStats& dropped,
                                      std::vector<const Decl*>& templates, std::vector<const Ty*>& droppedClasses)
{
  // The instances the old walks asked for that no one asks for now, and what only they asked for: a
  // build from scratch never makes them. One pass over every unit's recorded uses (in parallel, by
  // shard) gives the roots (every walk, file's new ones too, and the declarations' typing) and what
  // each instance asked for; an instance is used when a root reaches it.
  auto isInstance = [](uint8_t kind) { return kind == USE_Instance || kind == USE_ClassInstance; };
  // (A declaration's typing whose declaration the file no longer has, a local of a body replaced by
  // its edit, is no root: what it asked for is the old body's.)
  std::unordered_set<const Node*> current;
  forEachNode(file.decls, [&](Node* n) { current.insert(n); });
  std::vector<std::vector<const void*>> rootParts(std::size(impl->declDependencies) + 1);
  std::vector<const void*> staleUses = instances; // the old walks' requests, and those of the declarations they made
  {
    std::lock_guard lock(impl->typeDependenciesMutex);
    for (auto& [owner, deps] : impl->typeDependencyOwners)
    {
      bool stale = owner->file == &file && !current.count(owner);
      std::lock_guard l(deps->mutex);
      for (auto& [kind, what] : deps->list.uses)
        if (isInstance(kind))
          (stale ? staleUses : rootParts.back()).push_back(what); // (a removed local's: `Boxed extra(2.5)`'s Boxed<double>)
      if (stale)
      {
        deps->list = DependencyList{}; // (taken now: its declaration is gone, a later edit has nothing to find in it)
        deps->tidied = true;
      }
    }
  }
  if (staleUses.empty())
    return; // nothing the old walks or their declarations asked for is gone
  forEachParallel(std::size(impl->declDependencies), [&](size_t s)
  {
    Impl::DeclDependencyShard& shard = impl->declDependencies[s];
    std::lock_guard lock(shard.mutex);
    for (auto& [key, deps] : shard.map)
    {
      std::lock_guard l(deps->mutex);
      for (auto& [kind, what] : deps->list.uses)
        if (isInstance(kind))
          rootParts[s].push_back(what);
    }
  });
  using Edges = std::vector<std::pair<const void*, std::vector<const void*>>>;
  std::vector<Edges> edgeParts(std::size(impl->instanceShards) + std::size(impl->instShards));
  forEachParallel(edgeParts.size(), [&](size_t s)
  {
    auto add = [&](const void* owner, Dependencies& d)
    {
      std::lock_guard l(d.mutex);
      std::vector<const void*> asked;
      for (auto& [kind, what] : d.list.uses)
        if (isInstance(kind))
          asked.push_back(what);
      if (!asked.empty())
        edgeParts[s].push_back({owner, std::move(asked)});
    };
    if (s < std::size(impl->instanceShards))
    {
      Impl::InstanceShard& shard = impl->instanceShards[s];
      std::lock_guard lock(shard.mutex);
      for (auto& [key, record] : shard.map)
        add(record.get(), record->deps);
    }
    else
    {
      Impl::InstShard& shard = impl->instShards[s - std::size(impl->instanceShards)];
      std::lock_guard lock(shard.mutex);
      for (auto& [classTy, inst] : shard.classes)
        add(inst.get(), inst->deps);
    }
  });
  std::unordered_map<const void*, const std::vector<const void*>*> asks;
  for (const Edges& part : edgeParts)
    for (const auto& [owner, asked] : part)
      asks[owner] = &asked;
  auto spread = [&](std::vector<const void*> pending, std::unordered_set<const void*>& into)
  {
    for (const void* p : pending)
      into.insert(p);
    while (!pending.empty())
    {
      const void* p = pending.back();
      pending.pop_back();
      if (auto it = asks.find(p); it != asks.end())
        for (const void* q : *it->second)
          if (into.insert(q).second)
            pending.push_back(q);
    }
  };
  std::unordered_set<const void*> reached, candidates;
  for (const std::vector<const void*>& part : rootParts)
    spread(part, reached);
  spread(staleUses, candidates);
  std::unordered_set<const void*> unused;
  for (const void* p : candidates)
    if (!reached.count(p))
      unused.insert(p);
  // And every instance made for a closure whose lambda the file no longer has (the edit replaced its
  // body): nothing can name that closure, whether or not a recorded request led to the instance (a
  // class instance's, `Hold<Hold<...<closure>>>` ten deep, is no one's recorded request).
  std::unordered_map<const Ty*, bool> goneIn;
  std::function<bool(const Ty*)> gone = [&](const Ty* t)
  {
    if (!t)
      return false;
    if (auto known = goneIn.find(t); known != goneIn.end())
      return known->second;
    goneIn[t] = false;
    bool found = t->kind == TyKind::Lambda && t->decl && t->decl->file == &file && t->decl->kind == NodeKind::ClassDecl &&
                 static_cast<const ClassDecl*>(t->decl)->lambda && !current.count(static_cast<const ClassDecl*>(t->decl)->lambda);
    found = found || gone(t->inner);
    for (const Ty* a : t->args)
      found = found || gone(a);
    return goneIn[t] = found;
  };
  for (Impl::InstanceShard& shard : impl->instanceShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [key, r] : shard.map)
    {
      bool forGone = r->of && r->of->kind == NodeKind::ClassDecl && static_cast<const ClassDecl*>(r->of)->lambda &&
                     r->of->file == &file && !current.count(static_cast<const ClassDecl*>(r->of)->lambda);
      if (!forGone && r->recheck)
      {
        forGone = gone(r->recheck->thisType) || gone(r->recheck->lambdaTy);
        for (const Subst::Pair& p : r->recheck->subst.pairs)
          forGone = forGone || gone(p.type);
        if (r->recheck->task)
          for (const Subst::Pair& p : r->recheck->task->full.pairs)
            forGone = forGone || gone(p.type);
      }
      if (forGone)
        unused.insert(r.get());
    }
  }
  for (Impl::InstShard& shard : impl->instShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [classTy, inst] : shard.classes)
      if (gone(classTy))
        unused.insert(inst.get());
  }
  // A class instance's members' bodies go with it (they are not among its recorded requests).
  std::unordered_set<const Ty*> droppedClassTypes;
  for (Impl::InstShard& shard : impl->instShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [classTy, inst] : shard.classes)
      if (unused.count(inst.get()))
        droppedClassTypes.insert(classTy);
  }
  if (!droppedClassTypes.empty())
    for (Impl::InstanceShard& shard : impl->instanceShards)
    {
      std::lock_guard lock(shard.mutex);
      for (auto& [key, r] : shard.map)
        if (r->recheck && r->recheck->member && r->recheck->thisType && r->recheck->thisType->kind == TyKind::Pointer &&
            droppedClassTypes.count(impl->types.unqualified(r->recheck->thisType->inner)))
          unused.insert(r.get());
    }
  // Out of the tables (kept: their addresses stay theirs), with their statistics and errors; the
  // report's counts lose what each added (not codegen's own, which the report never counted).
  std::unordered_set<const void*> drop;
  for (Impl::InstanceShard& shard : impl->instanceShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto it = shard.map.begin(); it != shard.map.end();)
      if (unused.count(it->second.get()))
      {
        Impl::InstanceRecord* r = it->second.get();
        if (!r->late)
        {
          dropped.bodies += (r->counted & 1) != 0;
          dropped.functions += (r->counted & 2) != 0;
        }
        if (r->of)
          templates.push_back(r->of);
        if (r->recheck && r->recheck->source)
          templates.push_back(r->recheck->source);
        drop.insert(r);
        impl->droppedInstances.push_back(std::move(it->second));
        it = shard.map.erase(it);
        Impl::instanceGeneration.fetch_add(1, std::memory_order_acq_rel); // (no thread's claimInstance answers with it now)
      }
      else
        ++it;
  }
  for (Impl::InstShard& shard : impl->instShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto it = shard.classes.begin(); it != shard.classes.end();)
      if (unused.count(it->second.get()))
      {
        Impl::Instantiation* c = it->second.get();
        if (!c->late && c->counted)
          ++dropped.classes;
        if (c->classTy && c->classTy->decl)
          templates.push_back(c->classTy->decl);
        if (c->classTy)
          droppedClasses.push_back(c->classTy);
        drop.insert(c);
        impl->droppedClasses.push_back(std::move(it->second));
        it = shard.classes.erase(it);
      }
      else
        ++it;
  }
  {
    std::lock_guard lock(impl->instanceStatsMutex);
    auto& list = impl->instanceContributions;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Impl::InstanceContribution& c) { return drop.count(c.owner) != 0; }), list.end());
  }
  std::unordered_set<const void*> units;
  for (auto& r : impl->droppedInstances)
    if (drop.count(r.get()))
      units.insert(&r->deps);
  for (auto& c : impl->droppedClasses)
    if (drop.count(c.get()))
      units.insert(&c->deps);
  impl->forgetErrorsOf(units);
  std::lock_guard lock(impl->checkerErrorsMutex);
  for (auto& r : impl->droppedInstances)
    if (drop.count(r.get()) && r->body)
    {
      impl->instanceAssertErrors.erase(r->body);
      impl->instanceNameErrors.erase(r->body);
      impl->instanceWarnings.erase(r->body);
    }
}

GlobalScope::DependencyStats GlobalScope::dependencyStats()
{
  DependencyStats out;
  auto count = [&](Dependencies& d, size_t& units)
  {
    std::lock_guard lock(d.mutex);
    DependencyList& list = d.list; // as recorded: tidying all of it would cost a second of resolve
    if (list.names.empty() && list.uses.empty())
      return;
    ++units;
    out.names += list.names.size();
    out.uses += list.uses.size();
  };
  for (Impl::DeclDependencyShard& shard : impl->declDependencies)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [key, deps] : shard.map)
      count(*deps, out.walks);
  }
  {
    std::lock_guard lock(impl->typeDependenciesMutex);
    for (auto& deps : impl->typeDependencies)
      count(*deps, out.declaredTypes);
  }
  for (Impl::InstanceShard& shard : impl->instanceShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [key, record] : shard.map)
      count(record->deps, out.instances);
  }
  for (Impl::InstShard& shard : impl->instShards)
  {
    std::lock_guard lock(shard.mutex);
    for (auto& [key, inst] : shard.classes)
      count(inst->deps, out.classInstances);
  }
  std::lock_guard lock(impl->relationScopesMutex);
  std::vector<const Scope*> scopes = impl->relationScopes;
  std::sort(scopes.begin(), scopes.end());
  scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
  for (const Scope* s : scopes)
    count(const_cast<Scope*>(s)->relationDeps, out.scopes);
  return out;
}

ResolveStats GlobalScope::instanceStats()
{
  std::lock_guard lock(impl->instanceStatsMutex);
  return impl->instanceStatsTotal;
}

GlobalScope::InstantiationStats GlobalScope::instantiationStats() const
{
  return {impl->instClasses.load(), impl->instFunctions.load(), impl->instBodies.load(), impl->instRequests.load(),
          impl->instWaits.load(), impl->instNanos.load() / 1e9};
}

void GlobalScope::enableAccessWarnings() { impl->accessWarnings = true; }
void GlobalScope::enableStructRule() { impl->structRuleErrors = true; }

std::vector<std::string> GlobalScope::accessReport(const std::vector<ParsedFile*>& files, std::string& summary)
{
  std::vector<std::string> out;
  Impl::StructCounts counts;
  for (ParsedFile* f : files)
    if (f)
      impl->structRule(f->decls, out, counts);
  size_t structWarnings = std::count_if(out.begin(), out.end(), [](const std::string& w) { return w.find(": warning: struct ") != std::string::npos; });
  std::map<std::string, size_t> byGroup, byKind;
  size_t protectedUses = 0;
  {
    std::lock_guard lock(impl->accessMutex);
    out.insert(out.end(), impl->accessFriendUses.begin(), impl->accessFriendUses.end());
    for (const auto& [message, v] : impl->accessViolations)
    {
      out.push_back(message);
      ++byGroup[v.group];
      ++byKind[v.kind];
      protectedUses += v.isProtected;
    }
    auto list = [](const std::map<std::string, size_t>& m)
    {
      std::string text;
      for (const auto& [name, n] : m)
        text += (text.empty() ? "" : ", ") + std::to_string(n) + " " + name;
      return text.empty() ? std::string("none") : text;
    };
    summary = "access: " + std::to_string(impl->accessViolations.size()) + " uses of private or protected members their code may not name (" +
              list(byGroup) + "; " + std::to_string(impl->accessViolations.size() - protectedUses) + " private, " + std::to_string(protectedUses) +
              " protected; " + list(byKind) + "), " + std::to_string(impl->accessFriendUses.size()) + " more that " +
              std::to_string(impl->accessFriendsUsed.size()) + " of the " + std::to_string(counts.friends) + " friend declarations allow\n";
  }
  for (const auto& [group, n] : counts.byGroup)
    summary += "structs (" + group + "): " + std::to_string(n[0]) + " structs and " + std::to_string(n[1]) + " classes; " +
               std::to_string(n[2]) + " structs with a private or protected member, " + std::to_string(n[3]) + " with a base that is not public, " +
               std::to_string(n[4]) + " with a virtual function, " + std::to_string(n[5]) + " more polymorphic through a base only\n";
  summary += "  (" + std::to_string(structWarnings) + " structs would have to become classes; constructors are checked where code calls them)";
  // By file, then line number, then text, as checkerErrors.
  auto key = [](const std::string& marked)
  {
    std::string w = plainDiagnostic(marked);
    size_t kind = w.find(": warning: ") != std::string::npos ? w.find(": warning: ") : w.find(": note: ");
    size_t colon = kind == std::string::npos || kind == 0 ? std::string::npos : w.rfind(':', kind - 1);
    if (colon == std::string::npos || colon == 0)
      return std::make_tuple(w, 0L, w);
    return std::make_tuple(w.substr(0, colon), std::strtol(w.c_str() + colon + 1, nullptr, 10), w);
  };
  std::sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) { return key(a) < key(b); });
  return out;
}
