// GlobalScope::Impl, its data: the tables, the instantiation records, the generic lambdas: part of resolver.cpp, included in the class's body there (a file of its
// own since 2026-11-05, moved without a change; docs/design.md, "Compilation model").
  SymbolTable& symbols;
  TypeTable types;
  // The tokens of the file being resolved on this thread: constants in template arguments
  // (`Num<1>::Type`) are read from them when a lookup's own TypeContext carries none.
  static thread_local const std::vector<Token>* fallbackTokens;
  static thread_local std::string_view fallbackText;
  struct TokenScope
  {
    TokenScope(const std::vector<Token>* tokens, std::string_view text) : savedTokens(fallbackTokens), savedText(fallbackText)
    {
      fallbackTokens = tokens;
      fallbackText = text;
    }
    ~TokenScope()
    {
      fallbackTokens = savedTokens;
      fallbackText = savedText;
    }

    const std::vector<Token>* savedTokens;
    std::string_view savedText;
  };
  Scope root{ScopeKind::Namespace, nullptr, nullptr, 256};
  std::mutex deferredMutex;
  std::vector<std::pair<const ParsedFile*, Scope*>> privateScopes; // each file's private names (design.md 15), as indexFile made them
  Scope* stdScope = nullptr;          // the standard library's names, std (design.md 16); null without a standard library (--no-std)
  // Where the compiler finds what it names in the standard library: std, or the global scope without one.
  Scope* stdlibScope() { return stdScope ? stdScope : &root; }
  std::vector<Scope*> libraryScopes; // each scoped library's names (design.md 16), by name once the index is finished
  std::vector<NamespaceDecl*> programNamespaces; // the top-level namespace blocks of the files of no scoped library
  std::unordered_set<uint32_t> unscopedLibraries; // the unscoped libraries' names, which qualify nothing (design.md 16)
  std::vector<std::pair<Decl*, Scope*>> deferredQualified; // Out-of-line class/enum definitions.
  std::vector<std::pair<FunctionDecl*, Scope*>> deferredFunctions; // Out-of-line member function definitions.
  std::vector<std::pair<VariableDecl*, Scope*>> deferredVariables; // Out-of-line static data member definitions.
  std::vector<std::pair<DeductionGuideDecl*, Scope*>> deferredGuides; // Deduction guides, attached to their class templates by finishIndex.
  std::vector<std::pair<FunctionDecl*, Scope*>> objectCandidates; // `T five(a, m);`: an object when a and m are variables (finishIndex).
  std::vector<ParsedFile*> castFiles; // files with `(name) op x` candidates (ParsedFile::castCandidates), for finishIndex
  std::vector<ParsedFile*> splitDefaultFiles; // files with ParsedFile::splitDefaults, for finishIndex
  std::vector<std::pair<FunctionDecl*, FunctionDecl*>> externCRedeclarations; // (extern "C" declaration, a later one): sortEntries, decided by finishIndex
  std::unordered_map<uint32_t, std::vector<FunctionDecl*>> cFunctions; // C functions by name, in every namespace: sortEntries, compared by finishIndex

  // `(HANDLE) -1` (also `(T) +x`, `(T) *p`, `(T) &x`), which the parser read as `(HANDLE) - 1`:
  // when the name is a type where it is written, the cast of the unary expression it reads as in
  // C++, `(HANDLE)(-1)`, rewritten in place before any walk. The cast takes the leftmost operand
  // of what the parser took for the right side (`(T) -a * b` is `((T)(-a)) * b`). A parameter of
  // the function of the same name hides the type, so stays a subtraction. Run by finishIndex.
  void castFromBinary(BinaryExpr* b, Decl* where)
  {
    Expr** paren = &b->lhs; // `(long long)(HANDLE) -2`: the parenthesised name under the casts before it
    while ((*paren)->kind == NodeKind::CastExpr)
      paren = &static_cast<CastExpr*>(*paren)->operand;
    if ((*paren)->kind != NodeKind::ParenExpr || !static_cast<ParenExpr*>(*paren)->inner ||
        static_cast<ParenExpr*>(*paren)->inner->kind != NodeKind::NameExpr)
      return;
    auto* name = static_cast<NameExpr*>(static_cast<ParenExpr*>(*paren)->inner);
    LocalStack locals;
    Scope* scope = lexicalScopeOf(where);
    if (where && where->kind == NodeKind::FunctionDecl)
    {
      auto* f = static_cast<FunctionDecl*>(where);
      if (f->name.parts.size() > 1)
        if (Decl* cls = owningClassOf(f); cls && cls->ownScope)
          scope = cls->ownScope; // `void A::f() { (IndexType) -1; }`: A's members are visible
      if (f->parent && f->parent->kind == NodeKind::ClassDecl)
        for (Decl* p : effectiveParams(f->parent))
          locals.push(static_cast<TemplateParamDecl*>(p)->name, p);
      for (Decl* p : f->templateParams)
        locals.push(static_cast<TemplateParamDecl*>(p)->name, p);
      for (Decl* p : f->params)
        if (static_cast<ParamDecl*>(p)->name != UINT32_MAX)
          locals.push(static_cast<ParamDecl*>(p)->name, p);
    }
    Decl* d = lookupQualified(name->name, scope, &locals, 0).decl;
    bool type = d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                      (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)) ||
                      (d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isType));
    if (!type)
      return;
    Arena& arena = instanceArena();
    Expr** slot = &b->rhs;
    while ((*slot)->kind == NodeKind::BinaryExpr)
      slot = &static_cast<BinaryExpr*>(*slot)->lhs;
    auto* u = arena.make<UnaryExpr>();
    u->kind = NodeKind::UnaryExpr;
    u->token = b->token;
    u->operand = *slot;
    u->op = b->op;
    u->isKeyword = false;
    u->postfix = false;
    auto* t = arena.make<NamedType>();
    t->kind = NodeKind::NamedType;
    t->token = name->token;
    t->name = name->name;
    auto* c = arena.make<CastExpr>();
    c->kind = NodeKind::CastExpr;
    c->token = (*paren)->token;
    c->type = t;
    c->operand = u;
    *paren = c;
    if (slot == &b->rhs)
    {
      static_assert(sizeof(CastExpr) <= sizeof(BinaryExpr) && alignof(CastExpr) <= alignof(BinaryExpr));
      new (b) CastExpr(*static_cast<CastExpr*>(b->lhs)); // the node its parent points to becomes the (outermost) cast
      return;
    }
    *slot = b->lhs;
    *b = *static_cast<BinaryExpr*>(b->rhs); // `(T) -a * b`: the node becomes the multiplication, the cast its left operand
  }

  // `constexpr Pair<int, Mode> five(a, m);` at namespace scope, which the parser read as a function
  // with unnamed parameters of the types a and m: when every name is a variable (or an enumerator
  // or a function) instead, an object constructed from them (design.md rule 2: no most vexing
  // parse), which replaces the function in its scope and among its file's or namespace's
  // declarations. Run by finishIndex, when every name is known.
  void objectFromFunctionDeclaration(FunctionDecl* f, Scope* scope)
  {
    for (Expr* a : f->objectArgs)
    {
      Decl* d = lookupQualified(static_cast<NameExpr*>(a)->name, scope, nullptr, 0).decl;
      bool value = d && ((d->kind == NodeKind::VariableDecl && !(d->flags & DF_Typedef)) || d->kind == NodeKind::EnumeratorDecl ||
                         d->kind == NodeKind::FunctionDecl);
      if (!value)
        return; // a type (or nothing known): the function stands
    }
    Arena& arena = instanceArena();
    auto* v = arena.make<VariableDecl>();
    v->kind = NodeKind::VariableDecl;
    v->token = f->token;
    v->endToken = f->endToken;
    v->file = f->file;
    v->parent = f->parent;
    v->access = f->access;
    v->attributes = f->attributes;
    v->flags = f->flags & (DF_Constexpr | DF_Static | DF_Extern | DF_Inline | DF_ThreadLocal);
    v->type = f->returnType;
    v->name = f->name;
    auto* init = arena.make<FunctionalCastExpr>();
    init->kind = NodeKind::FunctionalCastExpr;
    init->token = f->token;
    init->type = f->returnType; // the parser's `T x(args)`
    init->args = f->objectArgs;
    init->braced = false;
    v->init = init;
    uint32_t symbol = functionSymbol(f->name);
    if (const std::vector<Entry>* entries = scope->find(symbol))
      for (Entry& e : const_cast<std::vector<Entry>&>(*entries))
      {
        if (e.decl == f)
          e.decl = v;
        else if (scope->kind == ScopeKind::Namespace && e.decl && e.decl->kind == NodeKind::VariableDecl &&
                 (e.decl->flags & DF_Extern) && !static_cast<VariableDecl*>(e.decl)->init && !variableDefinitions.count(e.decl))
          variableDefinitions[e.decl] = v; // `extern Box box;` before it: its definition (as sortEntries links)
      }
    Span<Decl*> siblings = f->parent && f->parent->kind == NodeKind::NamespaceDecl ? static_cast<NamespaceDecl*>(f->parent)->decls
                           : f->file ? f->file->decls : Span<Decl*>{};
    for (Decl*& sibling : siblings)
      if (sibling == f)
        sibling = v;
  }
  // A class template's deduction guides, each as a function (template) returning the class it
  // deduces: built by finishIndex (single-threaded), only read after.
  std::unordered_map<Decl*, std::vector<FunctionDecl*>> guidesOf;
  std::vector<std::unique_ptr<FunctionDecl>> guideFunctions;
  std::unordered_map<FunctionDecl*, std::vector<Scope*>> localTypesOfOutOfLine; // Scopes of classes and enums declared in their bodies.
  std::vector<Scope*> scopesWithUsingEnums; // `using enum E;` in them: resolved once by finishIndex, before parallel lookups
  std::unordered_map<Decl*, VariableDecl*> variableDefinitions;    // Static data member -> its `T C::x = ...;` definition.
  // A class template's static data member -> its explicit specialisations for single instances
  // (`template<> const uint32_t ElectricNetworkStatistics::smoothingTicks = 0;`, variableDefinitionFor).
  std::unordered_map<Decl*, std::vector<VariableDecl*>> variableSpecializations;
  std::unordered_map<Decl*, Scope*> specializationMembers;         // `int Flag<true>::value() {...}` -> Flag<true>'s scope (finishIndex).
  std::mutex checkerErrorsMutex;
  // Each error the checker reported (its file, token and message; rendered as text only when the
  // errors are asked for, checkerErrors), with the units that reported it (reportingUnit; null: none,
  // or a build that records no units).
  std::unordered_map<CheckerError, std::vector<const void*>, CheckerError::Hash> checkerErrorSet;
  // The warnings it reported likewise ([[nodiscard]] discarded, [[deprecated]] called): no error,
  // forgotten with their units as errors are (forgetErrorsOf), rendered by checkerWarnings.
  std::unordered_map<CheckerError, std::vector<const void*>, CheckerError::Hash> checkerWarningSet;
  std::unordered_set<CheckerError, CheckerError::Hash> reportedErrors; // what a collected USE_ReportedError points at (never erased)
  struct LayoutAssert                                              // a static_assert on a class's size or alignment
  {
    StaticAssertDecl* decl = nullptr;
    Subst subst;                                                   // the instance's arguments, in a template
    std::string instance;
    const FunctionDecl* inFunction = nullptr;                      // the instance's function body it is in: counts if emitted
    const void* unit = nullptr;                                    // the unit that met it (reportingUnit)
  };
  std::vector<LayoutAssert> layoutAsserts;                          // (under checkerErrorsMutex) for the code generator
  // (under checkerErrorsMutex) Failed static_asserts in template instances' function bodies, by
  // that body, errors once the code generator emits it (GlobalScope::functionEmitted).
  std::unordered_map<const FunctionDecl*, std::vector<CheckerError>> instanceAssertErrors;
  // (under checkerErrorsMutex) Names a template instance's function body uses that its arguments'
  // lookup does not find either, by that body: errors once the code generator emits it.
  std::unordered_map<const FunctionDecl*, std::vector<CheckerError>> instanceNameErrors;
  // (under checkerErrorsMutex) Warnings in template instances' function bodies, by that body: printed
  // once the code generator emits it, as C++ warns in the instances it makes only (the checker walks
  // some that C++ never makes: a primary template's body for arguments an explicit specialization takes).
  std::unordered_map<const FunctionDecl*, std::vector<CheckerError>> instanceWarnings;
  std::unordered_set<const FunctionDecl*> emittedFunctions;
  bool codeGenerated = true; // GlobalScope::setCodeGenerated: false, none of emittedFunctions counts
  bool converting = false;   // GlobalScope::setConverting: both branches of a plain function's if constexpr walked
  bool debug = true;         // GlobalScope::setDebug: config::debug (false: an assert's expression is discarded code)
  bool cLong64 = false;      // GlobalScope::setCLong64: the target's C long is 64 bits (printf's %ld)
  bool msvcAbi = false;      // GlobalScope::setMsvcAbi: MSVC's C++ ABI (a pointer to a data member is 4 bytes)
  bool emitted(const FunctionDecl* f) const { return codeGenerated && emittedFunctions.count(f); }
  std::atomic<size_t> scopes{1};
  // Draft types: created during Phase 2 for unresolved type names, stored separately from the
  // scope tree so Phase 2 lookups remain lock-free. Keyed by (scope*, symbol).
  struct DraftKey
  {
    bool operator==(const DraftKey&) const = default;

    Scope* scope;
    uint32_t symbol;
  };
  struct DraftHash { size_t operator()(const DraftKey& k) const { return std::hash<uintptr_t>()(reinterpret_cast<uintptr_t>(k.scope)) ^ (k.symbol * 2654435761u); } };
  static constexpr uint32_t draftShardCount = 16;
  struct DraftShard
  {
    std::mutex mutex;
    std::unordered_map<DraftKey, ClassDecl*, DraftHash> map;
  };
  DraftShard draftShards[draftShardCount];
  std::mutex draftStorageMutex;
  std::vector<std::unique_ptr<ClassDecl>> draftClasses;
  // Classes an elaborated type specifier declares (`class ScaleTag` in a template argument), made
  // the same way, one per (namespace scope, name): real classes with a name, not drafts.
  DraftShard tagShards[draftShardCount];
  std::deque<NamePart> tagNames; // their names' storage (under draftStorageMutex)
  std::unordered_set<uint32_t> templateParamNames;

  void collectTemplateParamNames(Scope* scope)
  {
    for (Decl* p : scope->templateParams)
      templateParamNames.insert(static_cast<TemplateParamDecl*>(p)->name);
    // A class's constructors and conversion functions are no names of its scope (they are reached
    // through the class): their template parameters are classified here, or `template<Size N>
    // View(T (&a)[N])` kept N a constrained type parameter and the bound of its array no bound.
    if (scope->kind == ScopeKind::Class && scope->owner && scope->owner->kind == NodeKind::ClassDecl)
      for (Decl* m : static_cast<ClassDecl*>(scope->owner)->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Constructor | DF_Conversion)))
          for (Decl* p : m->templateParams)
          {
            templateParamNames.insert(static_cast<TemplateParamDecl*>(p)->name);
            classifyTemplateParam(static_cast<TemplateParamDecl*>(p), scope, m->templateParams);
          }
    for (uint32_t i = 0; i < scope->shardCount; ++i)
      for (auto& [sym, entries] : scope->shards[i].map)
        for (const Entry& e : entries)
        {
          for (Decl* p : e.decl->templateParams)
          {
            templateParamNames.insert(static_cast<TemplateParamDecl*>(p)->name);
            classifyTemplateParam(static_cast<TemplateParamDecl*>(p), scope, e.decl->templateParams);
          }
          if (e.scope && e.scope != scope)
            collectTemplateParamNames(e.scope); // not a local class's own name in its scope
        }
    for (FunctionDecl* f : scope->outOfLineMembers)
      for (Decl* p : f->templateParams)
        classifyTemplateParam(static_cast<TemplateParamDecl*>(p), lexicalScopeOf(f), f->templateParams);
  }

  // `template<Kind K>`: the parser cannot tell a constant parameter of a named type from a
  // constrained type parameter (`template<Concept T>`) and takes it for the latter; a name that
  // turns out to be a type, not a concept, makes it a constant parameter of that type.
  void classifyTemplateParam(TemplateParamDecl* p, Scope* scope, Span<Decl*> siblings)
  {
    if (!p->isType || !p->valueType || p->valueType->kind != NodeKind::NamedType)
      return;
    const QualifiedName& name = static_cast<NamedType*>(p->valueType)->name;
    if (name.empty() || name.last().hasTemplateArgs)
      return; // `Concept<X> T`
    // `template<typename T, T V>`: an earlier type parameter of the same list.
    if (name.parts.size() == 1)
      for (Decl* sibling : siblings)
      {
        if (sibling == p)
          break;
        auto* s = static_cast<TemplateParamDecl*>(sibling);
        if (s->isType && s->name == name.parts[0].symbol)
        {
          p->isType = false;
          return;
        }
      }
    Lookup l = lookupQualified(name, scope, nullptr, 0);
    Decl* d = l.decl;
    if (d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
              (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))))
      p->isType = false;
    else if (!d && name.parts.size() == 1 && builtinByName(name.parts[0].symbol))
      p->isType = false; // `template<typename T, size_t N>`: size_t is a type by its name
  }

  // The class `class ScaleTag` declares where nothing named ScaleTag is visible: one per namespace
  // scope and name, an empty class (only its identity matters to a tag) with its name and
  // namespace, so that it mangles and prints as written.
  ClassDecl* getOrCreateTag(Scope* scope, uint32_t symbol)
  {
    DraftKey key{scope, symbol};
    DraftShard& shard = tagShards[symbol % draftShardCount];
    {
      std::lock_guard lock(shard.mutex);
      if (auto it = shard.map.find(key); it != shard.map.end())
        return it->second;
    }
    auto cd = std::make_unique<ClassDecl>();
    cd->kind = NodeKind::ClassDecl;
    cd->keyword = Kw::class_;
    cd->isDefinition = true;
    cd->parent = scope->owner;
    cd->ownScope = new Scope(ScopeKind::Class, scope, cd.get(), 1);
    ClassDecl* raw = cd.get();
    {
      std::lock_guard lock(draftStorageMutex);
      NamePart& part = tagNames.emplace_back();
      part.symbol = symbol;
      cd->name.parts = Span<NamePart>{&part, 1};
      draftClasses.push_back(std::move(cd));
    }
    std::lock_guard lock(shard.mutex);
    auto [it, inserted] = shard.map.emplace(key, raw);
    return it->second; // If another thread beat us, theirs.
  }

  ClassDecl* getOrCreateDraft(Scope* scope, uint32_t symbol)
  {
    DraftKey key{scope, symbol};
    DraftShard& shard = draftShards[symbol % draftShardCount];
    {
      std::lock_guard lock(shard.mutex);
      auto it = shard.map.find(key);
      if (it != shard.map.end())
        return it->second;
    }
    auto cd = std::make_unique<ClassDecl>();
    cd->kind = NodeKind::ClassDecl;
    cd->attributes = ATTR_Draft;
    cd->name.parts = {};
    cd->ownScope = new Scope(ScopeKind::Class, scope, cd.get(), 4);
    cd->ownScope->name = symbol;
    ClassDecl* raw = cd.get();
    {
      std::lock_guard lock(draftStorageMutex);
      draftClasses.push_back(std::move(cd));
    }
    {
      std::lock_guard lock(shard.mutex);
      auto [it, inserted] = shard.map.emplace(key, raw);
      return it->second; // If another thread beat us, return theirs.
    }
  }

  // The hash of an instance key (pointers appended as bytes): eight bytes at a time, where
  // std::hash<std::string> takes them one by one (the instance maps were 8% of the front end).
  struct PointerKeyHash
  {
    using is_transparent = void; // (claimInstance looks a key on its stack up: std::string_view)
    size_t operator()(const std::string& key) const { return (*this)(std::string_view(key)); }
    size_t operator()(std::string_view key) const
    {
      uint64_t h = 0x9E3779B97F4A7C15ull ^ key.size();
      size_t i = 0;
      for (; i + 8 <= key.size(); i += 8)
      {
        uint64_t w;
        memcpy(&w, key.data() + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
      }
      for (; i < key.size(); ++i)
        h = (h ^ (unsigned char)key[i]) * 0x100000001B3ull;
      return size_t(h ^ (h >> 29));
    }
  };

  // ---- instantiations' own bodies (docs/design.md, "Instantiations own their typed bodies") ----
  // One record per (function, arguments, `this` type): the copy of the function's nodes typed for
  // them, built once by whichever asks first (the checker's instantiation checks, or codegen for an
  // instantiation the checker did not reach) and read by everyone after.
  // A function instance's check queued while the files are resolved (Walker::instantiateFunction):
  // what running it later needs.
  struct FunctionInstanceTask
  {
    FunctionDecl* f;
    FunctionDecl* def;
    Subst full, defFull;
    Scope* bodyScope;
    const Ty* bodyThis;
    const ParsedFile* requestFile;
    uint32_t requestToken;
    int depth;
    const ParsedFile* rootFile = nullptr; // Walker::rootFile
    uint32_t rootToken = UINT32_MAX;
  };
  struct InstanceRecord
  {
    // How it was checked, for checking it again when its template's body changes (a compile
    // server's build keeps it: step 4). A member of a class instance (member) or a function instance.
    struct Recheck
    {
      bool member = false;
      FunctionDecl* source = nullptr;          // the declaration whose body was copied
      std::unique_ptr<FunctionInstanceTask> task; // a function instance's
      // a member's walker, as set up
      Scope* scope = nullptr;
      const Ty* thisType = nullptr;
      Subst subst;
      std::vector<LocalStack::Item> locals;
      const ParsedFile* file = nullptr; // the template's: walked in its tokens and text as they are now (a graft replaces them)
      int depth = 0;
      const ParsedFile* requestFile = nullptr;
      uint32_t requestToken = UINT32_MAX;
      const ParsedFile* rootFile = nullptr; // Walker::rootFile
      uint32_t rootToken = UINT32_MAX;
      uint32_t fileIndex = 0;
      bool collectWhy = false;
      const Ty* lambdaTy = nullptr; // a generic lambda's call operator: its closure type (subst: its own arguments)
    };

    std::atomic<int> state{0}; // 1 being built (by owner), 2 built
    std::atomic<std::thread::id> owner; // who builds it (whoever takes a queued check over)
    std::atomic<int> queued{0};         // 1: its check is queued, not started; taken by whoever exchanges it to 0 first
    bool late = false;                  // made after the resolve (by codegen): a clean resolve may not make it (step 3b)
    std::unique_ptr<Recheck> recheck;
    const Decl* of = nullptr; // what it is an instance of (claimInstance's f)
    // Where the program's own code asked for it in the end, through the instances that asked for one
    // another (Walker::rootFile; null when no walk of the program's code did: codegen's)
    const ParsedFile* rootFile = nullptr;
    uint32_t rootToken = UINT32_MAX;
    std::unique_ptr<FunctionInstanceTask> task;
    const Ty* memberOf = nullptr; // a member's queued check (no task): its class instance (demandMember)
    FunctionDecl* body = nullptr;
    const Ty* deducedReturn = nullptr; // what the body's returns deduced, for an `auto` function
    Expr* init = nullptr;              // for a variable: its initializer's copy (GlobalScope::instanceInitializer)
    LambdaExpr* lambda = nullptr;      // for a generic lambda's call operator: the copy of its body (checkLambdaInstance)
    Dependencies deps;                 // what its walk read (step 2 of the persistent compiler)
    uint8_t counted = 0;               // the report's counters it added to: 1 instBodies, 2 instFunctions (a server's edit that drops it takes them off)
  };
  struct InstanceShard
  {
    std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<InstanceRecord>, PointerKeyHash, std::equal_to<>> map; // key: f, this type, sorted argument pointers
  };
  InstanceShard instanceShards[64];
  // A thread's last answers of claimInstance by key (direct-mapped): an instance asked for again (92%
  // of the game's 1,288,637 requests) is found without its shard's lock and without allocating. An
  // entry holds for one generation of the table: one is a GlobalScope's life (a new one may reuse
  // addresses) until a record leaves the table (recheckInstancesOf, dropUnusedInstances).
  static inline std::atomic<uint64_t> instanceGeneration{1};
  struct InstanceCacheEntry
  {
    uint64_t generation = 0;
    size_t hash = 0;
    uint32_t size = 0;
    char key[16 + 16 * 6];
    InstanceRecord* record = nullptr;
  };
  static constexpr size_t instanceCacheSize = 1024;
  // Instances codegen made that a compile server's edit took out of the table (recheckInstancesOf):
  // kept, not freed, so that no address the recorded reads mention is ever another record's.
  std::vector<std::unique_ptr<InstanceRecord>> droppedInstances;

  // The record of f's instantiation for subst with `this` of thisType (null: none), whatever the
  // order of subst's pairs; producer: the caller just claimed it and builds it. f: a function, or
  // a variable whose initializer is instantiated.
  InstanceRecord* claimInstance(const Decl* f, const Subst& subst, const Ty* thisType, bool& producer)
  {
    // The key, on the stack for up to 8 arguments: f, the this type, the argument pairs sorted.
    size_t n = subst.pairs.size();
    std::pair<const void*, const void*> fewPairs[8];
    std::vector<std::pair<const void*, const void*>> manyPairs;
    std::pair<const void*, const void*>* pairs = n <= std::size(fewPairs) ? fewPairs : (manyPairs.resize(n), manyPairs.data());
    for (size_t i = 0; i < n; ++i)
      pairs[i] = {subst.pairs[i].param, subst.pairs[i].type};
    std::sort(pairs, pairs + n);
    char fewKey[16 + 16 * 8];
    std::string manyKey;
    size_t size = 16 + 16 * n;
    char* key = size <= sizeof fewKey ? fewKey : (manyKey.resize(size), manyKey.data());
    memcpy(key, &f, 8);
    memcpy(key + 8, &thisType, 8);
    for (size_t i = 0; i < n; ++i)
    {
      memcpy(key + 16 + 16 * i, &pairs[i].first, 8);
      memcpy(key + 24 + 16 * i, &pairs[i].second, 8);
    }
    std::string_view keyView(key, size);
    size_t hash = PointerKeyHash()(keyView);
    auto found = [&](InstanceRecord* record)
    {
      producer = false;
      if (rewalking.load(std::memory_order_relaxed) && record->late)
        rewalkTouchedLate = true;
      recordUse(USE_Instance, record);
      return record;
    };
    // This thread's cache: a record the table held for this key in this generation is still its.
    thread_local std::unique_ptr<InstanceCacheEntry[]> cache;
    uint64_t generation = instanceGeneration.load(std::memory_order_acquire);
    InstanceCacheEntry* cached = nullptr;
    if (size <= sizeof(InstanceCacheEntry::key))
    {
      if (!cache)
        cache = std::make_unique<InstanceCacheEntry[]>(instanceCacheSize);
      cached = &cache[hash % instanceCacheSize];
      if (cached->generation == generation && cached->hash == hash && cached->size == size && !memcmp(cached->key, key, size))
        return found(cached->record);
    }
    auto remember = [&](InstanceRecord* record)
    {
      if (!cached)
        return;
      cached->generation = generation;
      cached->hash = hash;
      cached->size = uint32_t(size);
      memcpy(cached->key, key, size);
      cached->record = record;
    };
    InstanceShard& shard = instanceShards[(hash >> 58) % std::size(instanceShards)]; // the top bits: the map inside buckets by the low ones
    std::lock_guard lock(shard.mutex);
    if (auto it = shard.map.find(keyView); it != shard.map.end())
    {
      remember(it->second.get());
      return found(it->second.get());
    }
    auto& slot = shard.map[std::string(keyView)];
    slot = std::make_unique<InstanceRecord>();
    slot->late = afterResolve.load(std::memory_order_relaxed) && !rewalking.load(std::memory_order_relaxed);
    slot->of = f;
    // (Made by a rewalk: a build from scratch makes it too; what the old walks asked for and no one
    // uses now goes, GlobalScope::dropUnusedInstances.)
    if (rewalking.load(std::memory_order_relaxed))
    {
      std::lock_guard made(rewalkMadeMutex);
      rewalkRecords.push_back(slot.get());
    }
    slot->owner = std::this_thread::get_id();
    slot->state.store(1, std::memory_order_relaxed);
    producer = true;
    recordUse(USE_Instance, slot.get());
    return slot.get();
  }

  // A copy of f's own nodes for one instantiation, its local classes and enums given scopes of
  // their own under scope (the copy is typed next). An out-of-line definition takes `static` and
  // `virtual` from its declaration first, so that the copy has them.
  FunctionDecl* copyInstance(FunctionDecl* f, Scope* scope, const Subst& subst, bool keepClosures = false)
  {
    declarationOfDefinition(f);
    InstanceCopier copier(instanceArena());
    copier.keepClosures = keepClosures;
    FunctionDecl* copy = copier.function(f);
    copy->instanceMark = instanceMarkOf(subst);
    if (copy->body)
      indexLocalTypes(copy->body, scope, copy);
    return copy;
  }

  // An instantiation's mark: a hash of its arguments as printed (with their classes' full paths), in
  // parameter-name order, the same in every run; 0 for none. A lambda type in it carries the same
  // (its instance), and the names of its local classes too (FunctionDecl::instanceMark).
  uint64_t instanceMarkOf(const Subst& subst)
  {
    if (subst.pairs.empty())
      return 0;
    std::vector<std::string> printed;
    for (auto& [d, ty] : subst.pairs)
      if (d && d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->name != UINT32_MAX)
        printed.push_back(std::string(symbols.name(static_cast<TemplateParamDecl*>(d)->name)) + "=" + (ty ? types.toString(ty, &symbols, true) : std::string("?")));
    std::sort(printed.begin(), printed.end());
    uint64_t h = 1469598103934665603ull;
    for (const std::string& p : printed)
      for (unsigned char c : p + ";")
        h = (h ^ c) * 1099511628211ull;
    return h ? h : 1;
  }

  // GlobalScope::programAllocationFunctions, replacedAllocation, globalDeallocation.
  bool programAllocationFunctions()
  {
    for (uint32_t s : {symOperatorNew, symOperatorNewArray, symOperatorDelete, symOperatorDeleteArray})
    {
      Lookup l;
      if (!lookupInScope(&root, s, l, 0))
        continue;
      auto own = [](const Decl* d) { return d && d->kind == NodeKind::FunctionDecl && d->file && !d->file->isLibrary && !d->file->isImported; };
      if (l.overloads)
      {
        for (const Entry& e : *l.overloads)
          if (own(e.decl))
            return true;
      }
      else if (own(l.decl))
        return true;
    }
    return false;
  }

  bool replacedAllocation(const FunctionDecl* f)
  {
    if (!f || !f->file || !f->file->isLibrary || f->name.parts.size() != 1 || f->name.last().special != 0 || (f->parent && f->parent->kind != NodeKind::NamespaceDecl))
      return false;
    uint32_t s = f->name.last().symbol;
    if (s != symOperatorNew && s != symOperatorNewArray && s != symOperatorDelete && s != symOperatorDeleteArray)
      return false;
    Lookup l;
    if (!lookupInScope(&root, s, l, 0) || !l.overloads)
      return false;
    const Ty* mine = declaredTypeOf(const_cast<FunctionDecl*>(f), nullptr, 0);
    for (const Entry& e : *l.overloads)
    {
      if (!e.decl || e.decl == f || e.decl->kind != NodeKind::FunctionDecl || !e.decl->file || e.decl->file->isLibrary || e.decl->file->isImported)
        continue;
      const Ty* theirs = declaredTypeOf(e.decl, nullptr, 0);
      if (mine && theirs && mine->kind == TyKind::Function && theirs->kind == TyKind::Function && mine->args.size() == theirs->args.size() &&
          std::equal(mine->args.begin(), mine->args.end(), theirs->args.begin()))
        return true;
    }
    return false;
  }

  FunctionDecl* globalDeallocation(bool array, bool sized)
  {
    Lookup l;
    if (!lookupInScope(&root, array ? symOperatorDeleteArray : symOperatorDelete, l, 0))
      return nullptr;
    std::vector<Decl*> all;
    if (l.overloads)
      for (const Entry& e : *l.overloads)
        all.push_back(e.decl);
    else
      all.push_back(l.decl);
    for (Decl* d : all)
    {
      if (!d || d->kind != NodeKind::FunctionDecl || replacedAllocation(static_cast<FunctionDecl*>(d)))
        continue;
      auto* f = static_cast<FunctionDecl*>(d);
      if (f->params.size() == (sized ? 2u : 1u) && f->templateParams.empty())
        return f;
    }
    return nullptr;
  }

  // The class a member (possibly defined out of line) belongs to (GlobalScope::owningClass).
  Decl* owningClassOf(Decl* d)
  {
    if (!d)
      return nullptr;
    if (d->parent && (d->parent->kind == NodeKind::ClassDecl || d->parent->kind == NodeKind::EnumDecl))
      return d->parent;
    if (d->kind == NodeKind::FunctionDecl)
    {
      auto* f = static_cast<FunctionDecl*>(d);
      if (f->name.parts.size() > 1)
      {
        if (auto it = specializationMembers.find(f); it != specializationMembers.end())
          return it->second->owner; // `int Flag<true>::value() {...}`: the specialisation's member
        QualifiedName qualifier = f->name;
        qualifier.parts.count -= 1;
        LocalStack params;
        for (Decl* p : f->templateParams)
          params.push(static_cast<TemplateParamDecl*>(p)->name, p);
        Lookup l = lookupQualified(qualifier, lexicalScopeOf(f), &params, 0);
        if (l.scope && l.scope->kind == ScopeKind::Class)
          return l.scope->owner;
      }
    }
    return nullptr;
  }

  // The in-class declaration of an out-of-line member definition, whose `static` and `virtual`
  // def takes (GlobalScope::declarationOf); null when there is none.
  FunctionDecl* declarationOfDefinition(FunctionDecl* def)
  {
    if (!def || def->name.parts.size() <= 1)
      return nullptr;
    Decl* cls = owningClassOf(def);
    if (!cls || !cls->ownScope)
      return nullptr;
    uint32_t symbol = functionSymbol(def->name);
    if (const std::vector<Entry>* entries = cls->ownScope->find(symbol))
      for (const Entry& e : *entries)
      {
        if (!e.decl || e.decl->kind != NodeKind::FunctionDecl || e.decl == def)
          continue;
        auto* decl = static_cast<FunctionDecl*>(e.decl);
        if (decl->body)
          continue;
        if (decl->refQualifier == def->refQualifier && typeAs(def, decl) == matchedType(decl)) // (`f() &` and `f() &&`: two)
        {
          uint32_t taken = decl->flags & (DF_Static | DF_Virtual);
          if ((def->flags & taken) != taken)
            def->flags |= taken; // written once (codegen settles it before emitting on several threads)
          return decl;
        }
      }
    return nullptr;
  }
  std::mutex instanceArenasMutex;
  std::vector<std::unique_ptr<Arena>> instanceArenas;
  // The arena this thread copies instantiations' bodies into (one per thread: no locking per node).
  Arena& instanceArena()
  {
    thread_local Arena* mine = nullptr;
    thread_local const void* owner = nullptr;
    if (!mine || owner != this)
    {
      std::lock_guard lock(instanceArenasMutex);
      instanceArenas.push_back(std::make_unique<Arena>());
      mine = instanceArenas.back().get();
      owner = this;
    }
    return *mine;
  }

  // ---- template instantiations: each (template, arguments) pair is checked exactly once ----
  struct Instantiation
  {
    std::atomic<int> state{0}; // 0 claimed-not-started is never visible: created in state 1 by the producer.
    std::thread::id owner;
    bool late = false; // made after the resolve (by codegen)
    const Ty* deducedReturn = nullptr; // For function templates with `auto` return.
    Dependencies deps; // what checking it read
    const Ty* classTy = nullptr; // a class instance's type (its template's declaration: classTy->decl)
    bool counted = false;        // added to instClasses
  };
  struct InstShard
  {
    std::mutex mutex;
    std::unordered_map<const Ty*, std::unique_ptr<Instantiation>> classes;
    std::unordered_map<std::string, std::unique_ptr<Instantiation>, PointerKeyHash> functions; // key: decl + arg pointers
  };
  static constexpr uint32_t instShardCount = 64;
  InstShard instShards[instShardCount];
  std::atomic<size_t> instClasses{0}, instFunctions{0}, instBodies{0}, instRequests{0}, instWaits{0};
  std::atomic<uint64_t> instNanos{0};
  // Step 3b (the compile server's fast path): the resolve is over (what is made from now on is
  // codegen's), and a changed file's walk is being redone (what it makes or reuses of codegen's).
  std::atomic<bool> afterResolve{false}, rewalking{false}, rewalkTouchedLate{false};
  // The instances a rewalk made (a build from scratch makes them too; the report counts them in):
  // GlobalScope::rewalkCounts.
  std::mutex rewalkMadeMutex;
  std::vector<const InstanceRecord*> rewalkRecords;
  std::vector<const Instantiation*> rewalkClasses;
  // The ones a server's edit took out of the tables (dropUnusedInstances): kept, not freed, so that no
  // address the recorded reads mention is ever another record's.
  std::vector<std::unique_ptr<Instantiation>> droppedClasses;
  // Every instance walk's statistics (GlobalScope::instanceStats), not its asker's: an instance
  // first asked for by a probe (a requires-expression's, whose statistics are dropped) would
  // otherwise go uncounted in some runs and counted in others.
  std::mutex instanceStatsMutex;
  ResolveStats instanceStatsTotal;
  // A compile server's build keeps each walk's own statistics too, with the instance it typed (null:
  // a class instance's check apart from its members'), so that one instance's can be replaced and
  // the total made again (step 4: a template's body changed).
  struct InstanceContribution
  {
    const void* owner;
    std::unique_ptr<ResolveStats> stats;
    bool late; // added after the resolve (codegen's instances): not in the resolve's report
  };
  std::vector<InstanceContribution> instanceContributions;
  bool rechecking = false; // an instance checked again counts as the resolve's (step 4)
  void addInstanceStats(const ResolveStats& s, const void* owner = nullptr)
  {
    // (the copy a compile server keeps is made before the lock: copying the statistics' maps under
    // it made a server's resolve 3 times as long at 32 threads, the instance checks waiting in line)
    std::unique_ptr<ResolveStats> kept = recordingDependencies ? std::make_unique<ResolveStats>(s) : nullptr;
    std::lock_guard lock(instanceStatsMutex);
    instanceStatsTotal.add(s);
    if (kept)
      instanceContributions.push_back({owner, std::move(kept), afterResolve.load() && !rechecking && !rewalking.load()}); // (a rewalk is the resolve's)
  }

  // The dependencies of the units that are declarations: a function's or a variable's walk (its
  // body, its initializer) and a declaration's declared type, keyed by the declaration and the
  // kind. Instances keep theirs in their records, scopes theirs in the scope.
  enum : uint8_t { UNIT_Walk = 1 };
  struct DeclDependencyShard
  {
    std::mutex mutex;
    std::unordered_map<uintptr_t, std::unique_ptr<Dependencies>> map;
  };
  DeclDependencyShard declDependencies[64];
  Dependencies* dependenciesOf(const Decl* d, uint8_t unit)
  {
    uintptr_t key = reinterpret_cast<uintptr_t>(d) + unit; // a declaration is larger than the kinds
    DeclDependencyShard& shard = declDependencies[(key >> 6) % std::size(declDependencies)];
    std::lock_guard lock(shard.mutex);
    std::unique_ptr<Dependencies>& slot = shard.map[key];
    if (!slot)
      slot = std::make_unique<Dependencies>();
    return slot.get();
  }
  // A declaration's declared type's: on the declaration, made once (asked for constantly).
  std::mutex typeDependenciesMutex;
  std::vector<std::unique_ptr<Dependencies>> typeDependencies;
  std::vector<std::pair<const Decl*, Dependencies*>> typeDependencyOwners; // the same, with their declarations
  Dependencies* typeDependenciesOf(Decl* d)
  {
    if (Dependencies* own = d->typeDependencies.load(std::memory_order_acquire))
      return own;
    std::lock_guard lock(typeDependenciesMutex);
    if (Dependencies* own = d->typeDependencies.load(std::memory_order_acquire))
      return own;
    typeDependencies.push_back(std::make_unique<Dependencies>());
    typeDependencyOwners.push_back({d, typeDependencies.back().get()});
    d->typeDependencies.store(typeDependencies.back().get(), std::memory_order_release);
    return typeDependencies.back().get();
  }
  std::mutex relationScopesMutex;
  std::vector<const Scope*> relationScopes; // the scopes whose relations were resolved (their relationDeps)
  // The bodies the checker's evaluator ran outside any unit while recording (nothing records that
  // it did): an edit of one is built from scratch.
  std::mutex ranOutsideMutex;
  std::unordered_set<const FunctionDecl*> ranOutside;
  std::unordered_set<const void*> membersReadOutside; // (under membersOutsideMutex) see membersOutsideSink

  // ---- generic lambdas: where each was written, so that its body can be typed for every call ----
  // A lambda with template parameters (written, or invented for `auto` parameters) is a template
  // whose call operator is instantiated per call; its body is typed in the environment of the
  // enclosing function at the point the lambda appears: its locals, scope, this, instantiation.
  struct LambdaEnv
  {
    LambdaExpr* lambda = nullptr;
    std::vector<LocalStack::Item> locals; // the enclosing locals, template parameters included
    Scope* scope = nullptr;
    const Ty* thisType = nullptr;
    Subst subst;                          // the enclosing instantiation's arguments
    const std::vector<Token>* tokens = nullptr;
    std::string_view text;
    const ParsedFile* file = nullptr;
    int depth = 0;
  };
  std::mutex lambdaEnvMutex;
  std::unordered_map<const Ty*, LambdaEnv> lambdaEnvs; // keyed by the lambda's type (one per enclosing instantiation)

  // A closure type's call operator as a member function (closureCall, OverloadResolution.hpp): the
  // declaration, the one overload a member lookup of operator() gives, the closure type and its
  // signature. Made at first use, kept for the build; by the closure type and by the declaration.
  struct ClosureCall
  {
    FunctionDecl decl;
    NamePart name; // its name's one part, operator()
    std::vector<Entry> entries;
    const Ty* closure = nullptr;
    const Ty* signature = nullptr;
  };
  std::mutex closureCallMutex;
  std::unordered_map<const Ty*, std::unique_ptr<ClosureCall>> closureCalls;
  std::unordered_map<const FunctionDecl*, ClosureCall*> closureCallsByDecl;

  static LambdaExpr* lambdaOf(const Ty* t)
  {
    if (!t || t->kind != TyKind::Lambda || !t->decl || t->decl->kind != NodeKind::ClassDecl)
      return nullptr;
    return static_cast<ClassDecl*>(t->decl)->lambda;
  }

  // A symbol's spelling for the KPP_DEBUG_CALLS trace; operators and unnamed parameters have none.
  std::string debugName(uint32_t symbol) const
  {
    return symbol == UINT32_MAX || symbol >= symbols.size() ? std::string("?") : std::string(symbols.name(symbol));
  }
  static bool isGenericLambda(const Ty* t)
  {
    LambdaExpr* l = lambdaOf(t);
    return l && !l->templateParams.empty();
  }

  // Returns the record and whether the caller just became its producer.
  Instantiation* claimClass(const Ty* classTy, bool& producer)
  {
    ++instRequests;
    InstShard& shard = instShards[(reinterpret_cast<uintptr_t>(classTy) >> 4) % instShardCount];
    std::lock_guard lock(shard.mutex);
    auto it = shard.classes.find(classTy);
    if (it != shard.classes.end())
    {
      producer = false;
      if (rewalking.load(std::memory_order_relaxed) && it->second->late)
        rewalkTouchedLate = true;
      recordUse(USE_ClassInstance, it->second.get());
      return it->second.get();
    }
    auto inst = std::make_unique<Instantiation>();
    inst->state.store(1);
    inst->owner = std::this_thread::get_id();
    inst->late = afterResolve.load(std::memory_order_relaxed) && !rewalking.load(std::memory_order_relaxed); // (a rewalk is the resolve's)
    inst->classTy = classTy;
    inst->counted = true;
    Instantiation* result = inst.get();
    if (rewalking.load(std::memory_order_relaxed))
    {
      std::lock_guard made(rewalkMadeMutex);
      rewalkClasses.push_back(result);
    }
    shard.classes.emplace(classTy, std::move(inst));
    producer = true;
    ++instClasses;
    recordUse(USE_ClassInstance, result);
    return result;
  }

  // f: a function template, or the closure of a generic lambda (then extra is the lambda's type,
  // which differs per enclosing instantiation).
  Instantiation* claimFunction(Decl* f, const Subst& subst, bool& producer, const void* extra = nullptr)
  {
    ++instRequests;
    std::string key(reinterpret_cast<const char*>(&f), sizeof(f));
    if (extra)
      key.append(reinterpret_cast<const char*>(&extra), sizeof(extra));
    // In parameter order whatever order the pairs were added in (as claimInstance's key): the same
    // instantiation reached by two paths was otherwise checked twice, by whichever paths ran.
    std::vector<std::pair<const void*, const void*>> pairs;
    pairs.reserve(subst.pairs.size());
    for (const Subst::Pair& pair : subst.pairs)
      pairs.push_back({pair.param, pair.type});
    std::sort(pairs.begin(), pairs.end());
    for (auto& [param, type] : pairs)
    {
      key.append(reinterpret_cast<const char*>(&param), sizeof(param));
      key.append(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    InstShard& shard = instShards[(PointerKeyHash()(key) >> 58) % instShardCount]; // the top bits: the map inside buckets by the low ones
    std::lock_guard lock(shard.mutex);
    auto it = shard.functions.find(key);
    if (it != shard.functions.end())
    {
      producer = false;
      return it->second.get();
    }
    auto inst = std::make_unique<Instantiation>();
    inst->state.store(1);
    inst->owner = std::this_thread::get_id();
    Instantiation* result = inst.get();
    shard.functions.emplace(std::move(key), std::move(inst));
    producer = true;
    ++instFunctions;
    return result;
  }

  std::atomic<size_t> entries{0};
  uint32_t symStd, symValue;
  uint32_t opSymbols[size_t(P::Count)]; // "operator+" etc., so operators can be looked up by name.
  // The allocation and deallocation functions' names ("operator new", "operator delete[]"): a
  // new-expression and a delete-expression look up the class's own (Walker::chooseAllocation).
  uint32_t symOperatorNew, symOperatorDelete, symOperatorNewArray, symOperatorDeleteArray;
  uint32_t symThisType; // `This`, the class around the code (design.md 23c): lookupUnqualified's
  uint32_t symBegin, symEnd, symFirst, symSecond;
  uint32_t symTupleSize, symGet;
  // The numbers 0 to 63 spelled in a file of their own: the explicit template argument of the
  // get<i>(e) a tuple-like structured binding calls (Walker::tupleBindings), which no source spells.
  std::string indexText;
  std::vector<Token> indexTokens;
  ParsedFile indexFile;
  std::vector<std::pair<std::string, uint32_t>> whyAt; // --why-at: trace the expressions typed on these (file suffix, line)s
  struct BuiltinName
  {
    uint32_t symbol;
    const Ty* type;
  };
  std::vector<BuiltinName> builtinNames; // uint32_t, size_t... which come from headers outside the corpus.

  explicit Impl(SymbolTable& symbols) : symbols(symbols)
  {
    symStd = symbols.intern("std");
    symBegin = symbols.intern("begin");
    symEnd = symbols.intern("end");
    symFirst = symbols.intern("first");
    symSecond = symbols.intern("second");
    symValue = symbols.intern("value");
    symTupleSize = symbols.intern("TupleSize");
    symGet = symbols.intern("get");
    for (uint32_t i = 0; i < 64; ++i)
    {
      std::string n = std::to_string(i);
      Token t{};
      t.offset = uint32_t(indexText.size());
      t.length = uint32_t(n.size());
      t.kind = uint32_t(TokenKind::Number);
      indexTokens.push_back(t);
      indexText += n + " ";
    }
    indexFile.tokens = &indexTokens;
    indexFile.text = indexText;
    indexFile.path = "<structured binding index>";
    for (uint32_t i = 0; i < uint32_t(P::Count); ++i)
      opSymbols[i] = symbols.intern("operator" + std::string(punctuatorSpelling(P(i))));
    symOperatorNew = symbols.intern("operator new");
    symOperatorDelete = symbols.intern("operator delete");
    symOperatorNewArray = symbols.intern("operator new[]");
    symOperatorDeleteArray = symbols.intern("operator delete[]");
    symThisType = symbols.intern("This");
    auto add = [&](const char* name, const Ty* t) { builtinNames.push_back({symbols.intern(name), t}); };
    add("int8_t", types.intType(8));
    add("i8", types.intType(8));
    add("i16", types.intType(16));
    add("i32", types.intType(32));
    add("i64", types.intType(64));
    add("u8", types.intType(8, true));
    add("u16", types.intType(16, true));
    add("u32", types.intType(32, true));
    add("u64", types.intType(64, true));
    add("usize", types.sizeType());
    add("f32", types.floatType(32));
    add("f64", types.floatType(64));
    add("int16_t", types.intType(16));
    add("int32_t", types.intType(32));
    add("int64_t", types.intType(64));
    add("uint8_t", types.intType(8, true));
    add("uint16_t", types.intType(16, true));
    add("uint32_t", types.intType(32, true));
    add("uint64_t", types.intType(64, true));
    add("size_t", types.sizeType());
    add("__builtin_va_list", types.pointer(types.charType())); // clang's <stdarg.h> va_list: a char* on x64 Windows, as MSVC's
    add("ptrdiff_t", types.ptrdiffType());
    add("intptr_t", types.intType(64));
    add("uintptr_t", types.intType(64, true));
    add("__int64", types.intType(64));
    add("__int32", types.intType(32));
    add("__int16", types.intType(16));
    add("__int8", types.charType(8)); // MSVC's __int8 is char
    add("ssize_t", types.ptrdiffType());
    add("int_fast32_t", types.intType(32));
    add("uint_fast32_t", types.intType(32, true));
    add("nullptr_t", types.nullptrType());
    add("_Bool", types.boolType());
    add("wchar_t", types.wcharType()); // (GlobalScope::setLP64: the target's size)
    add("__m128", types.vectorType(types.floatType(32), 4));
    add("__m128d", types.vectorType(types.floatType(64), 2));
    add("__m128i", types.vectorType(types.intType(32), 4));
    add("__m256", types.vectorType(types.floatType(32), 8));
    add("__m256d", types.vectorType(types.floatType(64), 4));
    add("__m256i", types.vectorType(types.intType(32), 8));
  }

  const Ty* builtinByName(uint32_t symbol) const
  {
    for (const BuiltinName& b : builtinNames)
      if (b.symbol == symbol)
        return b.type;
    return nullptr;
  }

