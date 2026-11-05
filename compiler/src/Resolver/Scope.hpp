#pragma once
// Part of the resolver: a namespace's, class's or enumeration's scope, its names and its relations
// (GlobalScopeImpl.hpp includes it where Entry and Dependencies are declared).

inline std::atomic<uint32_t> nextScopeId{0};

struct Scope
{
  // Symbol table, sharded for the heavily shared namespace scopes.
  struct Shard
  {
    std::mutex mutex;
    std::unordered_map<uint32_t, std::vector<Entry>> map;
  };
  Decl* findAbsent(uint32_t symbol) const
  {
    if (absent.empty())
      return nullptr;
    auto it = absent.find(symbol);
    return it == absent.end() ? nullptr : it->second;
  }

  Scope(ScopeKind kind, Scope* parent, Decl* owner, uint32_t shardCount)
    : kind(kind), parent(parent), owner(owner), shards(new Shard[shardCount]), shardCount(shardCount)
  {
  }

  Shard& shardFor(uint32_t symbol) { return shards[symbol % shardCount]; }

  void add(uint32_t symbol, Decl* decl, Scope* scope)
  {
    Shard& shard = shardFor(symbol);
    std::lock_guard lock(shard.mutex);
    shard.map[symbol].push_back({decl, scope});
  }

  // Read-only lookup for phase 2.
  const std::vector<Entry>* find(uint32_t symbol)
  {
    Shard& shard = shardFor(symbol);
    auto it = shard.map.find(symbol);
    const std::vector<Entry>* found = it == shard.map.end() ? nullptr : &it->second;
    if (!recordingDependencies || (!recordingInto && !usesCollector))
      return found;
    // A class's lookup is recorded with the class; one that finds a using-declaration (its target is
    // looked up elsewhere, and cached) by the name alone too, as every other scope's. (With the class
    // as well: an edit of a member is read only through its class and the classes derived from it,
    // and a using-declaration in a class names a base's member.)
    bool coarse = kind != ScopeKind::Class;
    bool viaUsing = false;
    if (found && !coarse)
      for (const Entry& e : *found)
        viaUsing = viaUsing || (e.decl && e.decl->kind == NodeKind::UsingNameDecl);
    if (coarse || viaUsing)
      recordName(symbol);
    if (!coarse)
      recordMember(id, symbol);
    return found;
  }

  // Finds an existing child namespace or creates it (merging all NamespaceDecls of that name).
  Scope* getOrCreateNamespace(uint32_t symbol, Decl* owner)
  {
    Shard& shard = shardFor(symbol);
    std::lock_guard lock(shard.mutex);
    std::vector<Entry>& entries = shard.map[symbol];
    for (const Entry& e : entries)
      if (e.scope && e.scope->kind == ScopeKind::Namespace)
        return e.scope;
    Scope* ns = new Scope(ScopeKind::Namespace, this, owner, 64);
    ns->name = symbol;
    entries.push_back({owner, ns});
    return ns;
  }

  ScopeKind kind;
  Scope* parent;
  Decl* owner; // Null for the global namespace; one of possibly several NamespaceDecls otherwise.
  uint32_t name = UINT32_MAX;
  const ParsedFile* privateTo = nullptr; // a file's private names (design.md 15): that file
  bool library = false; // a scoped library's names, the namespace of its name (design.md 16)
  uint32_t id = nextScopeId.fetch_add(1, std::memory_order_relaxed); // what a lookup in it is recorded with (memberName)
  std::unique_ptr<Shard[]> shards;
  uint32_t shardCount;

  // Lazily resolved relations (phase 2).
  std::vector<Type*> baseTypes;          // From the class definition.
  std::vector<Scope*> baseScopes;        // Resolved base class scopes.
  std::atomic<int> baseState{0};         // 0 untouched, 1 resolving, 2 done
  std::thread::id baseResolver;
  std::vector<UsingDirectiveDecl*> usingDirectives;
  std::vector<Scope*> usingScopes;
  // A namespace's named inline namespaces ([namespace.def] 7): their names are its names too, for
  // qualified and unqualified lookup alike (lookupInScope; filled by the index under shards[0]'s mutex).
  std::vector<Scope*> inlineScopes;
  std::atomic<int> usingState{0};
  std::vector<UsingNameDecl*> usingEnums; // `using enum E;` inside a class or namespace.
  std::vector<Scope*> usingEnumScopes;
  std::atomic<int> usingEnumState{0};
  Span<Decl*> templateParams; // Of the owning class template.
  std::vector<FunctionDecl*> outOfLineMembers; // `void Foo<T>::f() {}` definitions, attached by finishIndex.
  // The names the false `when` declarations of this scope declare (design.md 19), which the parser
  // dropped: a lookup stops at one (Lookup::absent), never reaching an outer scope's or a base's name
  // of that spelling. Filled by the index (a namespace's under deferredMutex), read after it.
  std::unordered_map<uint32_t, Decl*> absent;
  Dependencies relationDeps; // what resolving the bases and using-directives read
};
