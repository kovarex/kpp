#include "resolver.hpp"
#include "instance.hpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_set>

void ResolveStats::add(const ResolveStats& o)
{
  typesResolved += o.typesResolved;
  typesDependent += o.typesDependent;
  typesUnresolved += o.typesUnresolved;
  typesBuiltinByName += o.typesBuiltinByName;
  namesLocal += o.namesLocal;
  namesMember += o.namesMember;
  namesGlobal += o.namesGlobal;
  namesDependent += o.namesDependent;
  namesUnresolved += o.namesUnresolved;
  qualifiedUnresolvedStd += o.qualifiedUnresolvedStd;
  unresolvedMacroLike += o.unresolvedMacroLike;
  for (const auto& [symbol, count] : o.unresolvedSymbols)
    unresolvedSymbols[symbol] += count;
  for (const auto& [symbol, where] : o.unresolvedFirst)
    unresolvedFirst.emplace(symbol, where);
  membersBound += o.membersBound;
  membersDependent += o.membersDependent;
  membersUnknownBase += o.membersUnknownBase;
  membersNotFound += o.membersNotFound;
  for (const auto& [symbol, count] : o.missingMembers)
    missingMembers[symbol] += count;
  for (const auto& [symbol, where] : o.missingMembersFirst)
    missingMembersFirst.emplace(symbol, where);
  for (const auto& [symbol, count] : o.unknownBaseMembers)
    unknownBaseMembers[symbol] += count;
  exprsTyped += o.exprsTyped;
  exprsDependent += o.exprsDependent;
  exprsUnknown += o.exprsUnknown;
  unknownName += o.unknownName;
  unknownBase += o.unknownBase;
  unknownMember += o.unknownMember;
  unknownOverload += o.unknownOverload;
  unknownDeduction += o.unknownDeduction;
  unknownAutoReturn += o.unknownAutoReturn;
  unknownUnsupported += o.unknownUnsupported;
  unknownOperator += o.unknownOperator;
  unknownAuto += o.unknownAuto;
  callsResolved += o.callsResolved;
  callsAmbiguousArity += o.callsAmbiguousArity;
  instExprsTyped += o.instExprsTyped;
  instExprsDependent += o.instExprsDependent;
  instExprsUnknown += o.instExprsUnknown;
  for (const auto& [key, site] : o.why)
  {
    auto [it, inserted] = why.emplace(key, site);
    if (!inserted)
      it->second.count += site.count;
  }
}

std::string whyReport(const ResolveStats& total, SymbolTable& symbols, int perCause)
{
  static const char* causes[] = {"none", "name", "object type", "member", "overload", "deduction", "auto return",
                                 "unsupported construct", "operator", "auto"};
  std::vector<std::pair<uint64_t, ResolveStats::WhySite>> sites(total.why.begin(), total.why.end());
  std::sort(sites.begin(), sites.end(), [](auto& a, auto& b)
  {
    if ((a.first >> 55) != (b.first >> 55))
      return (a.first >> 55) < (b.first >> 55);
    return a.second.count != b.second.count ? a.second.count > b.second.count : a.first < b.first;
  });
  std::string out;
  char line[512];
  for (size_t i = 0; i < sites.size();)
  {
    uint64_t group = sites[i].first >> 55;
    uint64_t cause = group >> 1;
    uint64_t sum = 0;
    size_t end = i;
    while (end < sites.size() && (sites[end].first >> 55) == group)
      sum += sites[end++].second.count;
    snprintf(line, sizeof line, "  unknown because of %s %s (%llu), most common constructs:\n",
             cause < std::size(causes) ? causes[cause] : "?", group & 1 ? "inside template instantiations" : "in plain code",
             (unsigned long long)sum);
    out += line;
    for (size_t k = i; k < end && int(k - i) < perCause; ++k)
    {
      const auto& [key, site] = sites[k];
      auto kind = NodeKind(uint8_t(key >> 32));
      uint32_t symbol = uint32_t(key);
      std::string_view name = symbol == UINT32_MAX ? std::string_view("-") : symbols.name(symbol);
      uint32_t lineNo = 0;
      if (site.file && site.file->tokens && !site.file->tokens->empty())
      {
        const Token& t = (*site.file->tokens)[std::min<size_t>(site.token, site.file->tokens->size() - 1)];
        lineNo = 1 + uint32_t(std::count(site.file->text.begin(), site.file->text.begin() + std::min<size_t>(t.offset, site.file->text.size()), '\n'));
      }
      snprintf(line, sizeof line, "    %6u  %-14s %-32.*s first at %s:%u\n", site.count, nodeKindName(kind), int(std::min<size_t>(name.size(), 60)),
               name.data(), site.file ? site.file->path.c_str() : "?", lineNo);
      out += line;
    }
    i = end;
  }
  return out;
}

// --- Scope tree ---------------------------------------------------------------------------

enum class ScopeKind : uint8_t { Namespace, Class, Enum };

struct Entry
{
  Decl* decl;
  Scope* scope; // Non-null when the declaration opens a scope (namespace, class or enum definition).
};

struct Scope
{
  ScopeKind kind;
  Scope* parent;
  Decl* owner; // Null for the global namespace; one of possibly several NamespaceDecls otherwise.
  uint32_t name = UINT32_MAX;

  // Symbol table, sharded for the heavily shared namespace scopes.
  struct Shard
  {
    std::mutex mutex;
    std::unordered_map<uint32_t, std::vector<Entry>> map;
  };
  std::unique_ptr<Shard[]> shards;
  uint32_t shardCount;

  // Lazily resolved relations (phase 2).
  std::vector<Type*> baseTypes;          // From the class definition.
  std::vector<Scope*> baseScopes;        // Resolved base class scopes.
  std::atomic<int> baseState{0};         // 0 untouched, 1 resolving, 2 done
  std::thread::id baseResolver;
  std::vector<UsingDirectiveDecl*> usingDirectives;
  std::vector<Scope*> usingScopes;
  std::atomic<int> usingState{0};
  std::vector<UsingNameDecl*> usingEnums; // `using enum E;` inside a class or namespace.
  std::vector<Scope*> usingEnumScopes;
  std::atomic<int> usingEnumState{0};
  Span<Decl*> templateParams; // Of the owning class template.
  std::vector<FunctionDecl*> outOfLineMembers; // `void Foo<T>::f() {}` definitions, attached by finishIndex.

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
    return it == shard.map.end() ? nullptr : &it->second;
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

};

namespace
{
  // Local declarations inside a function body, template parameter lists and lambdas, kept as a
  // stack that is searched backwards.
  struct LocalStack
  {
    struct Item
    {
      uint32_t symbol;
      Decl* decl;
      // The type this walk deduced for a local whose declared type has no initializer to deduce
      // from (a range-for variable): per walk, since one template body is typed concurrently for
      // several instantiations.
      const Ty* type = nullptr;
    };
    std::vector<Item> items;
    size_t mark() const { return items.size(); }
    void pop(size_t mark) { items.resize(mark); }
    void push(uint32_t symbol, Decl* decl)
    {
      if (symbol != UINT32_MAX)
        items.push_back({symbol, decl});
    }
    Decl* find(uint32_t symbol) const
    {
      for (size_t i = items.size(); i-- > 0;)
        if (items[i].symbol == symbol)
          return items[i].decl;
      return nullptr;
    }
    const Ty* deducedTypeOf(const Decl* decl) const
    {
      for (size_t i = items.size(); i-- > 0;)
        if (items[i].decl == decl)
          return items[i].type;
      return nullptr;
    }
  };

  enum class Found : uint8_t { No, Local, Member, Global, Dependent };

  struct Lookup
  {
    Found found = Found::No;
    Decl* decl = nullptr;
    Scope* scope = nullptr;                        // Scope opened by decl, if any.
    const std::vector<Entry>* overloads = nullptr; // All entries with that name in the scope where it was found.
    Scope* foundIn = nullptr;                      // Scope the name was found in.
  };

  // Template parameter -> type, for members seen through an instantiation like Vector<int>.
  struct Subst
  {
    struct Pair
    {
      Decl* param;
      const Ty* type;
    };
    std::vector<Pair> pairs;
    const Ty* find(Decl* param) const
    {
      for (const Pair& p : pairs)
        if (p.param == param)
          return p.type;
      return nullptr;
    }
    bool empty() const { return pairs.empty(); }
  };

  bool isAllCaps(std::string_view s)
  {
    if (s.size() < 2)
      return false;
    bool letter = false;
    for (char c : s)
    {
      if (c >= 'a' && c <= 'z')
        return false;
      if (c >= 'A' && c <= 'Z')
        letter = true;
    }
    return letter;
  }
}

struct GlobalScope::Impl
{
  SymbolTable& symbols;
  TypeTable types;
  // The tokens of the file being resolved on this thread: constants in template arguments
  // (`Num<1>::Type`) are read from them when a lookup's own TypeContext carries none.
  static thread_local const std::vector<Token>* fallbackTokens;
  static thread_local std::string_view fallbackText;
  struct TokenScope
  {
    const std::vector<Token>* savedTokens;
    std::string_view savedText;
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
  };
  Scope root{ScopeKind::Namespace, nullptr, nullptr, 256};
  std::mutex deferredMutex;
  std::vector<std::pair<Decl*, Scope*>> deferredQualified; // Out-of-line class/enum definitions.
  std::vector<std::pair<FunctionDecl*, Scope*>> deferredFunctions; // Out-of-line member function definitions.
  std::vector<std::pair<VariableDecl*, Scope*>> deferredVariables; // Out-of-line static data member definitions.
  std::unordered_map<FunctionDecl*, std::vector<Scope*>> localTypesOfOutOfLine; // Scopes of classes and enums declared in their bodies.
  std::vector<Scope*> scopesWithUsingEnums; // `using enum E;` in them: resolved once by finishIndex, before parallel lookups
  std::unordered_map<Decl*, VariableDecl*> variableDefinitions;    // Static data member -> its `T C::x = ...;` definition.
  std::unordered_map<Decl*, Scope*> specializationMembers;         // `int Flag<true>::value() {...}` -> Flag<true>'s scope (finishIndex).
  std::mutex checkerErrorsMutex;
  std::unordered_set<std::string> checkerErrorSet;                 // `path:line: error: ...`, each once (a failed static_assert)
  struct LayoutAssert                                              // a static_assert on a class's size or alignment
  {
    StaticAssertDecl* decl = nullptr;
    Subst subst;                                                   // the instance's arguments, in a template
    std::string instance;
  };
  std::vector<LayoutAssert> layoutAsserts;                          // (under checkerErrorsMutex) for the code generator
  std::atomic<size_t> scopes{1};
  // Draft types: created during Phase 2 for unresolved type names, stored separately from the
  // scope tree so Phase 2 lookups remain lock-free. Keyed by (scope*, symbol).
  struct DraftKey { Scope* scope; uint32_t symbol; bool operator==(const DraftKey& o) const { return scope == o.scope && symbol == o.symbol; } };
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

  // ---- instantiations' own bodies (docs/design.md, "Instantiations own their typed bodies") ----
  // One record per (function, arguments, `this` type): the copy of the function's nodes typed for
  // them, built once by whichever asks first (the checker's instantiation checks, or codegen for an
  // instantiation the checker did not reach) and read by everyone after.
  struct InstanceRecord
  {
    std::atomic<int> state{0}; // 1 being built (by owner), 2 built
    std::thread::id owner;
    FunctionDecl* body = nullptr;
    const Ty* deducedReturn = nullptr; // what the body's returns deduced, for an `auto` function
    Expr* init = nullptr;              // for a variable: its initializer's copy (GlobalScope::instanceInitializer)
    LambdaExpr* lambda = nullptr;      // for a generic lambda's call operator: the copy of its body (checkLambdaInstance)
  };
  struct InstanceShard
  {
    std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<InstanceRecord>> map; // key: f, this type, sorted argument pointers
  };
  InstanceShard instanceShards[64];

  // The record of f's instantiation for subst with `this` of thisType (null: none), whatever the
  // order of subst's pairs; producer: the caller just claimed it and builds it. f: a function, or
  // a variable whose initializer is instantiated.
  InstanceRecord* claimInstance(const Decl* f, const Subst& subst, const Ty* thisType, bool& producer)
  {
    std::vector<std::pair<const void*, const void*>> pairs;
    pairs.reserve(subst.pairs.size());
    for (const Subst::Pair& p : subst.pairs)
      pairs.push_back({p.param, p.type});
    std::sort(pairs.begin(), pairs.end());
    std::string key(reinterpret_cast<const char*>(&f), sizeof f);
    key.append(reinterpret_cast<const char*>(&thisType), sizeof thisType);
    for (auto& [d, t] : pairs)
    {
      key.append(reinterpret_cast<const char*>(&d), sizeof d);
      key.append(reinterpret_cast<const char*>(&t), sizeof t);
    }
    InstanceShard& shard = instanceShards[std::hash<std::string>()(key) % std::size(instanceShards)];
    std::lock_guard lock(shard.mutex);
    auto& slot = shard.map[key];
    if (slot)
    {
      producer = false;
      return slot.get();
    }
    slot = std::make_unique<InstanceRecord>();
    slot->owner = std::this_thread::get_id();
    slot->state.store(1, std::memory_order_relaxed);
    producer = true;
    return slot.get();
  }

  // A copy of f's own nodes for one instantiation, its local classes and enums given scopes of
  // their own under scope (the copy is typed next). An out-of-line definition takes `static` and
  // `virtual` from its declaration first, so that the copy has them.
  FunctionDecl* copyInstance(FunctionDecl* f, Scope* scope)
  {
    declarationOfDefinition(f);
    InstanceCopier copier(instanceArena());
    FunctionDecl* copy = copier.function(f);
    if (copy->body)
      indexLocalTypes(copy->body, scope, copy);
    return copy;
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
        if (typeAs(def, decl) == declaredTypeOf(decl, nullptr, 0))
        {
          def->flags |= decl->flags & (DF_Static | DF_Virtual);
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
    const Ty* deducedReturn = nullptr; // For function templates with `auto` return.
  };
  struct InstShard
  {
    std::mutex mutex;
    std::unordered_map<const Ty*, std::unique_ptr<Instantiation>> classes;
    std::unordered_map<std::string, std::unique_ptr<Instantiation>> functions; // key: decl + arg pointers
  };
  static constexpr uint32_t instShardCount = 64;
  InstShard instShards[instShardCount];
  std::atomic<size_t> instClasses{0}, instFunctions{0}, instBodies{0}, instRequests{0}, instWaits{0};
  std::atomic<uint64_t> instNanos{0};

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
      return it->second.get();
    }
    auto inst = std::make_unique<Instantiation>();
    inst->state.store(1);
    inst->owner = std::this_thread::get_id();
    Instantiation* result = inst.get();
    shard.classes.emplace(classTy, std::move(inst));
    producer = true;
    ++instClasses;
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
    for (const Subst::Pair& pair : subst.pairs)
    {
      key.append(reinterpret_cast<const char*>(&pair.param), sizeof(pair.param));
      key.append(reinterpret_cast<const char*>(&pair.type), sizeof(pair.type));
    }
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key)
      h = (h ^ c) * 1099511628211ull;
    InstShard& shard = instShards[h % instShardCount];
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
  uint32_t symBegin, symFirst, symSecond;
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
    symFirst = symbols.intern("first");
    symSecond = symbols.intern("second");
    symValue = symbols.intern("value");
    for (uint32_t i = 0; i < uint32_t(P::Count); ++i)
      opSymbols[i] = symbols.intern("operator" + std::string(punctuatorSpelling(P(i))));
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
    add("int16_t", types.intType(16));
    add("int32_t", types.intType(32));
    add("int64_t", types.intType(64));
    add("uint8_t", types.intType(8, true));
    add("uint16_t", types.intType(16, true));
    add("uint32_t", types.intType(32, true));
    add("uint64_t", types.intType(64, true));
    add("size_t", types.sizeType());
    add("ptrdiff_t", types.ptrdiffType());
    add("intptr_t", types.intType(64));
    add("uintptr_t", types.intType(64, true));
    add("__int64", types.intType(64));
    add("__int32", types.intType(32));
    add("__int16", types.intType(16));
    add("__int8", types.intType(8));
    add("ssize_t", types.ptrdiffType());
    add("int_fast32_t", types.intType(32));
    add("uint_fast32_t", types.intType(32, true));
    add("nullptr_t", types.nullptrType());
    add("_Bool", types.boolType());
    add("wchar_t", types.charType(16, true));
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
  void indexLocalTypes(Stmt* s, Scope* scope, FunctionDecl* f)
  {
    if (!s)
      return;
    switch (s->kind)
    {
      case NodeKind::BlockStmt:
        for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
          indexLocalTypes(inner, scope, f);
        break;
      case NodeKind::DeclStmt:
        for (Decl* d : static_cast<DeclStmt*>(s)->decls)
          if (Scope* own = (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl) && !d->ownScope ? indexLocalType(d, scope) : nullptr;
              own && f->name.parts.size() > 1)
          {
            std::lock_guard lock(deferredMutex);
            localTypesOfOutOfLine[f].push_back(own);
          }
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
        for (const Entry& e : entries)
          if (e.scope && e.scope->kind != ScopeKind::Enum)
            sortEntries(e.scope, seen);
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

  Scope* lexicalScopeOf(Decl* d)
  {
    for (Decl* p = d ? d->parent : nullptr; p; p = p->parent)
      if (p->ownScope)
        return p->ownScope;
    return &root;
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
        Lookup l = lookupQualified(static_cast<NamedType*>(type)->name, context, locals, depth + 1);
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

  void ensureBases(Scope* cls, int depth)
  {
    int state = cls->baseState.load(std::memory_order_acquire);
    if (state == 2)
      return;
    if (state == 0 && cls->baseState.compare_exchange_strong(state, 1))
    {
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
      return;
    }
    // Another thread is resolving; if it is us (a cycle through aliases), give up on bases.
    if (cls->baseResolver == std::this_thread::get_id())
      return;
    while (cls->baseState.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
  }

  void ensureUsing(Scope* ns, int depth)
  {
    int state = ns->usingState.load(std::memory_order_acquire);
    if (state == 2)
      return;
    if (state == 0 && ns->usingState.compare_exchange_strong(state, 1))
    {
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
    int state = cls->usingEnumState.load(std::memory_order_acquire);
    if (state == 2)
      return;
    if (state == 0 && cls->usingEnumState.compare_exchange_strong(state, 1))
    {
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
        ensureBases(s, depth);
        for (Scope* b : s->baseScopes)
          if (lookupInScope(b, symbol, out, depth + 1))
            return true;
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

  Lookup lookupUnqualified(uint32_t symbol, Scope* context, const LocalStack* locals, int depth)
  {
    Lookup out;
    if (locals)
      if (Decl* d = locals->find(symbol))
      {
        out.decl = d;
        out.found = d->kind == NodeKind::TemplateParamDecl ? Found::Dependent : Found::Local;
        if (d->ownScope)
          out.scope = d->ownScope;
        return out;
      }
    for (Scope* s = context; s; s = s->parent)
      if (lookupInScope(s, symbol, out, depth))
        return out;
    return out;
  }

  // Among the entries a class template's name resolved to, the specialisation for `args`: a full
  // one whose arguments are exactly `args` (its own class, without arguments), or else a partial
  // one (`template<typename T> struct Describe<T*>`) whose patterns deduce from them (its class
  // with the deduced arguments of its own parameters). Null when there is none or an argument is
  // still dependent.
  // Whether two template arguments are the same: the same type, or constants of the same value
  // (`IntegralConstant<bool, isSigned<T>>` folds to a bool true, the specialisation's written
  // `true` is typed by its parameter `T V`: the same argument).
  static bool sameArgument(const Ty* a, const Ty* b)
  {
    if (a == b)
      return true;
    return a && b && a->kind == TyKind::Value && b->kind == TyKind::Value && a->value == b->value && a->decl == b->decl;
  }

  const Ty* matchSpecialization(const Lookup& l, const std::vector<const Ty*>& args, int depth)
  {
    if (!l.overloads || args.empty() || depth > 24)
      return nullptr;
    for (const Ty* a : args)
      if (!a || !a->isKnown() || types.isDependent(a))
        return nullptr;
    const Ty* partial = nullptr;
    for (const Entry& e : *l.overloads)
    {
      if (!e.decl || e.decl->kind != NodeKind::ClassDecl || !(e.decl->flags & DF_Specialization))
        continue;
      auto* c = static_cast<ClassDecl*>(e.decl);
      if (!c->isDefinition || c->name.empty())
        continue;
      const NamePart& part = c->name.last();
      if (part.templateArgs.size() != args.size())
        continue;
      LocalStack own;
      for (Decl* p : c->templateParams)
        own.push(static_cast<TemplateParamDecl*>(p)->name, p);
      TypeContext sctx{lexicalScopeOf(c), c->templateParams.empty() ? nullptr : &own, nullptr,
                       c->file ? c->file->tokens : nullptr, c->file ? c->file->text : std::string_view()};
      if (c->templateParams.empty())
      {
        bool same = true;
        for (size_t i = 0; i < args.size() && same; ++i)
        {
          Node* a = part.templateArgs[i];
          Decl* param = l.decl && i < l.decl->templateParams.size() ? l.decl->templateParams[i] : nullptr;
          same = sameArgument(templateArgument(a, param, sctx, depth), args[i]);
        }
        if (same)
          return types.classType(c, {});
        continue;
      }
      if (partial)
        continue; // the first matching partial specialisation wins (no ordering yet)
      Subst s;
      bool ok = true;
      for (size_t i = 0; i < args.size() && ok; ++i)
      {
        Node* a = part.templateArgs[i];
        Decl* param = l.decl && i < l.decl->templateParams.size() ? l.decl->templateParams[i] : nullptr;
        const Ty* pattern = templateArgument(a, param, sctx, depth);
        if (!pattern || !pattern->isKnown())
          ok = false;
        else if (types.isDependent(pattern))
        {
          deduce(pattern, args[i], s, depth + 1);
          ok = sameArgument(substitute(pattern, s), args[i]);
        }
        else
          ok = sameArgument(pattern, args[i]);
      }
      std::vector<const Ty*> deduced;
      for (Decl* p : c->templateParams)
      {
        const Ty* t = ok ? s.find(p) : nullptr;
        if (!t)
          ok = false;
        else
          deduced.push_back(t);
      }
      if (ok)
        partial = types.classType(c, Span<const Ty*>{deduced.data(), uint32_t(deduced.size())});
    }
    return partial;
  }

  Lookup lookupQualified(const QualifiedName& name, Scope* context, const LocalStack* locals, int depth, const Subst* subst = nullptr)
  {
    Lookup out;
    if (name.empty() || depth > 24)
      return out;
    Scope* current = name.global ? &root : nullptr;
    const Ty* currentTy = nullptr; // the class current is the scope of, when its arguments are known
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
          lookupInScope(current, part.symbol, out, depth);
        else
          out = lookupUnqualified(part.symbol, context, locals, depth);
        return out;
      }
      if (part.special == 2 && part.opToken < uint32_t(P::Count))
      {
        if (!last)
          return {};
        uint32_t symbol = opSymbols[part.opToken];
        if (current)
          lookupInScope(current, symbol, out, depth);
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
        if (!lookupInScope(current, part.symbol, step, depth))
          return {};
      }
      else
        step = lookupUnqualified(part.symbol, context, locals, depth);
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
        for (size_t i = 0; i < part.templateArgs.size(); ++i)
        {
          Decl* param = i < step.decl->templateParams.size() ? step.decl->templateParams[i] : nullptr;
          args.push_back(templateArgument(part.templateArgs[i], param, actx, depth + 1)); // types, and constants for value parameters
        }
        if (const Ty* spec = matchSpecialization(step, args, depth))
        {
          step.decl = spec->decl;
          step.scope = spec->decl->ownScope;
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

  // ---------------- phase 2: types ----------------

  struct ConstBinding // a parameter's value while a constexpr function call is evaluated
  {
    uint32_t symbol;
    int64_t value;
  };
  struct TypeContext
  {
    Scope* scope;
    const LocalStack* locals;
    const Subst* subst;
    const std::vector<Token>* tokens = nullptr; // For evaluating array bounds written as literals.
    std::string_view text;
    const std::vector<ConstBinding>* env = nullptr; // Parameters bound to values inside a constexpr call.
    FunctionDecl* function = nullptr; // Whose parameters a trailing return type names (`-> decltype(callable())`).
    // The variable template whose initializer is evaluated under subst (`hasBegin<Vector<int>>`):
    // a requires-expression in it holds or not for these arguments, whatever its last typing left.
    Decl* variableTemplate = nullptr;
  };

  // A written template argument that names only types that exist under ctx's substitution,
  // the arguments of an alias template included: `VoidT<T::force_memcpy_save>` is void for any T
  // but well-formed only for a T with that member (the detection idiom's substitution failure).
  bool wellFormedArgument(Node* n, const TypeContext& ctx, int depth = 0)
  {
    if (!n || depth > 16)
      return false;
    if (uint8_t(n->kind) > uint8_t(NodeKind::PackType))
      return true; // a constant
    switch (n->kind)
    {
      case NodeKind::NamedType:
        for (const NamePart& part : static_cast<NamedType*>(n)->name.parts)
          for (Node* a : part.templateArgs)
            if (!wellFormedArgument(a, ctx, depth + 1))
              return false;
        break;
      case NodeKind::PointerType: return wellFormedArgument(static_cast<PointerType*>(n)->pointee, ctx, depth + 1);
      case NodeKind::LRefType: return wellFormedArgument(static_cast<LRefType*>(n)->referenced, ctx, depth + 1);
      case NodeKind::RRefType: return wellFormedArgument(static_cast<RRefType*>(n)->referenced, ctx, depth + 1);
      case NodeKind::ConstType: return wellFormedArgument(static_cast<ConstType*>(n)->inner, ctx, depth + 1);
      case NodeKind::PackType: return wellFormedArgument(static_cast<PackType*>(n)->pattern, ctx, depth + 1);
      default: break;
    }
    const Ty* t = convertType(static_cast<Type*>(n), ctx, depth + 1);
    return t && t->isKnown();
  }

  // The specialisation of a variable template a use with these arguments (one per parameter of
  // the primary) takes its value from, as C++ chooses it: the matching partial and full
  // specialisations, the most specialised of them (its parameters bound in out); the primary
  // when none matches. Null when the matches are ambiguous.
  VariableDecl* variableSpecialization(VariableDecl* primary, const std::vector<const Ty*>& args, Subst& out)
  {
    if (primary->name.empty() || args.empty())
      return primary;
    for (const Ty* a : args)
      if (!a || !a->isKnown() || types.isDependent(a))
        return primary;
    Lookup l;
    if (!lookupInScope(lexicalScopeOf(primary), primary->name.last().symbol, l, 0) || !l.overloads)
      return primary;
    struct Match { VariableDecl* v; Subst s; std::vector<const Ty*> patterns; };
    std::vector<Match> matches;
    for (const Entry& e : *l.overloads)
    {
      if (!e.decl || e.decl == primary || e.decl->kind != NodeKind::VariableDecl)
        continue;
      auto* v = static_cast<VariableDecl*>(e.decl);
      if (v->name.empty() || !v->name.last().hasTemplateArgs || v->name.last().templateArgs.size() != args.size())
        continue;
      Span<Node*> written = v->name.last().templateArgs;
      LocalStack own;
      for (Decl* p : v->templateParams)
        own.push(static_cast<TemplateParamDecl*>(p)->name, p);
      TypeContext sctx{lexicalScopeOf(v), v->templateParams.empty() ? nullptr : &own, nullptr,
                       v->file ? v->file->tokens : nullptr, v->file ? v->file->text : std::string_view()};
      Match m{v, {}, {}};
      bool ok = true;
      for (size_t i = 0; i < args.size() && ok; ++i)
      {
        Decl* param = i < primary->templateParams.size() ? primary->templateParams[i] : nullptr;
        const Ty* pattern = templateArgument(written[i], param, sctx, 0);
        m.patterns.push_back(pattern);
        if (!pattern || !pattern->isKnown())
          ok = false;
        else if (types.isDependent(pattern))
        {
          deduce(pattern, args[i], m.s);
          ok = substitute(pattern, m.s) == args[i];
        }
        else
          ok = pattern == args[i];
      }
      for (Decl* p : v->templateParams)
        ok = ok && m.s.find(p);
      if (!ok)
        continue;
      TypeContext dctx{sctx.scope, sctx.locals, &m.s, sctx.tokens, sctx.text};
      for (Node* w : written)
        ok = ok && wellFormedArgument(w, dctx);
      if (ok)
        matches.push_back(std::move(m));
    }
    if (matches.empty())
      return primary;
    // a is at least as specialised as b when b's arguments deduce from a's.
    auto atLeastAs = [&](const Match& a, const Match& b)
    {
      Subst t;
      for (size_t i = 0; i < a.patterns.size(); ++i)
      {
        if (!types.isDependent(b.patterns[i]))
        {
          if (b.patterns[i] != a.patterns[i])
            return false;
          continue;
        }
        deduce(b.patterns[i], a.patterns[i], t);
        if (substitute(b.patterns[i], t) != a.patterns[i])
          return false;
      }
      return true;
    };
    for (const Match& m : matches)
    {
      bool best = true;
      for (const Match& o : matches)
        if (&o != &m && !(atLeastAs(m, o) && !atLeastAs(o, m)))
          best = false;
      if (best)
      {
        out = m.s;
        return m.v;
      }
    }
    return nullptr;
  }

  // --- builtin type traits ---

  static bool isTypeNode(Node* n)
  {
    switch (n->kind)
    {
      case NodeKind::NamedType: case NodeKind::PointerType: case NodeKind::LRefType: case NodeKind::RRefType: case NodeKind::ConstType:
      case NodeKind::ArrayType: case NodeKind::FunctionType: case NodeKind::DecltypeType: case NodeKind::AutoType: case NodeKind::BuiltinType:
      case NodeKind::MemberPointerType: case NodeKind::PackType:
        return true;
      default: return false;
    }
  }

  // The argument types of a trait, or empty when one is not a type or is still dependent.
  std::vector<const Ty*> traitArgs(const NamePart& part, const TypeContext& ctx, int depth)
  {
    std::vector<const Ty*> args;
    for (Node* a : part.templateArgs)
    {
      if (!isTypeNode(a))
        return {};
      const Ty* t = convertType(static_cast<Type*>(a), ctx, depth + 1);
      if (t && t->kind == TyKind::Pack)
      {
        for (const Ty* element : t->args)
          args.push_back(element); // `Args...` bound to a pack: its elements
        continue;
      }
      if (!t || !t->isKnown() || types.isDependent(t))
        return {};
      args.push_back(t);
    }
    return args;
  }

  const Ty* enumUnderlyingType(const Ty* e, const TypeContext& ctx, int depth)
  {
    Decl* def = e->decl;
    if (def && def->kind == NodeKind::EnumDecl && static_cast<EnumDecl*>(def)->underlying)
      return convertType(static_cast<EnumDecl*>(def)->underlying, ctx, depth + 1);
    return types.intType();
  }

  bool isTypeTraitName(uint32_t symbol)
  {
    std::string_view name = symbols.name(symbol);
    return name == "RemoveConst" || name == "RemoveCv" || name == "RemoveReference" || name == "RemoveExtent" || name == "RemovePointer" ||
           name == "Decay" || name == "UnderlyingType" || name == "AddConst" || name == "AddPointer" || name == "RemoveCvRef" ||
           name == "Conditional" || name == "EnableIf";
  }

  // RemoveConst<T> and the other type-valued traits; null when `part` is not one.
  const Ty* typeTrait(const NamePart& part, const TypeContext& ctx, int depth)
  {
    std::string_view name = symbols.name(part.symbol);
    if (!isTypeTraitName(part.symbol))
      return nullptr;
    if (name == "Conditional" || name == "EnableIf")
    {
      // Conditional<c, A, B> is A or B; EnableIf<c, T = void> is T when c holds (and no type at all
      // otherwise, which removes an overload the way std::enable_if does).
      if (part.templateArgs.empty())
        return types.unknown();
      const Ty* c = templateArgument(part.templateArgs[0], nullptr, ctx, depth);
      if (!c || c->kind != TyKind::Value)
        return types.unknown(); // dependent still: settled under the instantiation
      if (name == "Conditional")
      {
        if (part.templateArgs.size() != 3 || !isTypeNode(part.templateArgs[1]) || !isTypeNode(part.templateArgs[2]))
          return types.unknown();
        return convertType(static_cast<Type*>(part.templateArgs[c->value ? 1 : 2]), ctx, depth + 1);
      }
      if (!c->value)
        return types.unknown();
      if (part.templateArgs.size() == 1)
        return types.voidType();
      return isTypeNode(part.templateArgs[1]) ? convertType(static_cast<Type*>(part.templateArgs[1]), ctx, depth + 1) : types.unknown();
    }
    if (part.templateArgs.size() != 1 || !isTypeNode(part.templateArgs[0]))
      return types.unknown();
    const Ty* t = convertType(static_cast<Type*>(part.templateArgs[0]), ctx, depth + 1);
    if (!t || !t->isKnown() || types.isDependent(t))
      return t; // settled when the instantiation substitutes the argument
    if (name == "RemoveConst" || name == "RemoveCv")
      return types.unqualified(t);
    if (name == "RemoveReference")
      return types.stripRef(t);
    if (name == "RemoveCvRef")
      return types.unqualified(types.stripRef(t));
    if (name == "RemoveExtent")
      return t->kind == TyKind::Array ? t->inner : t;
    if (name == "RemovePointer")
      return t->kind == TyKind::Pointer ? t->inner : t;
    if (name == "AddConst")
      return types.qualified(t, true, false);
    if (name == "AddPointer")
      return types.pointer(types.stripRef(t));
    if (name == "Decay")
      return types.decay(t);
    if (name == "UnderlyingType")
      return t->kind == TyKind::Enum ? enumUnderlyingType(t, ctx, depth) : t;
    return nullptr;
  }

  static bool isArithmeticKind(const Ty* t)
  {
    return t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Bool || t->kind == TyKind::Float || t->kind == TyKind::Enum;
  }

  // Whether `derived` is `base` or has it among its bases (classes only).
  bool derivesFrom(const Ty* derived, const Ty* base, int depth = 0)
  {
    if (!derived || !base || depth > 16)
      return false;
    if (derived->kind != TyKind::Class || base->kind != TyKind::Class)
      return false;
    if (derived->decl == base->decl)
      return true;
    Decl* def = derived->decl;
    if (def && def->kind == NodeKind::ClassDecl && !def->ownScope)
    {
      Entry e{def, nullptr};
      Scope* s = scopeOfEntry(e, 0);
      def = s ? s->owner : def;
    }
    if (!def || def->kind != NodeKind::ClassDecl)
      return false;
    DeclContext dc;
    declContextFor(def, dc);
    // The bases as the instantiation's arguments make them (`Const<Color> : Conditional<..., Value<T>, T>`
    // derives from Color, which the class template alone cannot tell).
    Subst own = derived->args.empty() ? Subst{} : substFor(derived);
    for (const ClassDecl::Base& b : static_cast<ClassDecl*>(def)->bases)
    {
      const Ty* bt = convertType(b.type, TypeContext{dc.scope, &dc.params, own.pairs.empty() ? nullptr : &own, def->file ? def->file->tokens : nullptr,
                                                     def->file ? def->file->text : std::string_view()}, 0);
      if (bt && derivesFrom(bt, base, depth + 1))
        return true;
    }
    return false;
  }

  // Whether a constructor or conversion operator is explicit here: `explicit`, or `explicit(cond)`
  // with cond true under the substitution (FixedPointNumberTemplate's `explicit(Explicit)`).
  bool isExplicit(FunctionDecl* f, const Subst* subst)
  {
    if (!(f->flags & DF_Explicit))
      return false;
    if (!f->explicitCondition)
      return true;
    DeclContext dc;
    declContextFor(f, dc);
    ParsedFile* file = f->file;
    TypeContext ctx{dc.scope, &dc.params, subst, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
    std::optional<int64_t> v = constEval(f->explicitCondition, ctx, 0);
    return !v || *v != 0; // unknown: explicit, the safe side
  }

  // isArray<T>, isSame<A, B>, ... ; nullopt when `name` is not a trait.
  std::optional<bool> traitValue(std::string_view name, const std::vector<const Ty*>& a)
  {
    auto one = [&]() { return a.size() == 1 ? a[0] : nullptr; };
    if (name == "isArray") return one() && one()->kind == TyKind::Array;
    if (name == "isPointer") return one() && one()->kind == TyKind::Pointer;
    if (name == "isEnum") return one() && one()->kind == TyKind::Enum;
    if (name == "isClass") return one() && one()->kind == TyKind::Class;
    if (name == "isTypedUnion") // a `union enum` (design.md 8.5)
    {
      const Ty* t = one() ? types.unqualified(one()) : nullptr;
      return t && t->kind == TyKind::Class && t->decl && t->decl->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(t->decl)->isTypedUnion;
    }
    if (name == "isVoid") return one() && one()->kind == TyKind::Void;
    if (name == "isReference") return one() && one()->isReference();
    if (name == "isConst") return one() && one()->isConst;
    if (name == "isNullPointer") return one() && one() == types.nullptrType();
    if (name == "isIntegral") return one() && (one()->kind == TyKind::Int || one()->kind == TyKind::Char || one()->kind == TyKind::Bool);
    if (name == "isFloatingPoint") return one() && one()->kind == TyKind::Float;
    if (name == "isArithmetic") return one() && isArithmeticKind(one()) && one()->kind != TyKind::Enum;
    if (name == "isSigned")
      return one() && (one()->kind == TyKind::Float || ((one()->kind == TyKind::Int || one()->kind == TyKind::Char) && !one()->isUnsigned));
    if (name == "isUnsigned")
      return one() && ((one()->kind == TyKind::Int || one()->kind == TyKind::Char || one()->kind == TyKind::Bool) && (one()->isUnsigned || one()->kind == TyKind::Bool));
    if (name == "isFunction") return one() && one()->kind == TyKind::Function;
    if (name == "isTriviallyCopyable") return one() ? triviallyCopyable(one()) : std::nullopt;
    if (name == "isConstructible")
    {
      // isConstructible<T, Args...>: T has a constructor taking Args (a class without constructors
      // takes none or a copy; a scalar takes one convertible value).
      if (a.empty())
        return false;
      const Ty* t = types.unqualified(types.stripRef(a[0]));
      std::vector<const Ty*> args(a.begin() + 1, a.end());
      bool copy = args.size() == 1 && types.unqualified(types.stripRef(args[0])) == t;
      if (t->kind == TyKind::Class)
      {
        Decl* def = t->decl && t->decl->ownScope ? t->decl : (classScopeOf(t) ? classScopeOf(t)->owner : nullptr);
        if (!def || def->kind != NodeKind::ClassDecl)
          return false;
        std::vector<FunctionDecl*> ctors;
        for (Decl* m : static_cast<ClassDecl*>(def)->members)
          if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Delete))
            ctors.push_back(static_cast<FunctionDecl*>(m));
        if (copy || ctors.empty())
          return copy || args.empty();
        Subst s = substFor(t);
        bool ambiguous = false;
        return chooseAmong(ctors, args, s.pairs.empty() ? nullptr : &s, nullptr, ambiguous) != nullptr;
      }
      if (args.empty())
        return t->kind != TyKind::Void && t->kind != TyKind::Function;
      return args.size() == 1 && traitValue("isConvertible", {args[0], t}).value_or(false);
    }
    if (name == "isFundamental")
      return one() && (isArithmeticKind(one()) || one()->kind == TyKind::Void || one() == types.nullptrType());
    if (name == "isTriviallyDestructible") return one() ? triviallyDestructible(one()) : std::nullopt;
    if (name == "isTriviallyCopyConstructible") return one() ? constructibleFromSelf(one(), false, TraitForm::Trivially) : std::nullopt;
    if (name == "isTriviallyCopyAssignable") return one() ? assignableFromSelf(one(), false, TraitForm::Trivially) : std::nullopt;
    if (name == "isTriviallyDefaultConstructible") return one() ? triviallyDefaultConstructible(one()) : std::nullopt;
    if (name == "isSame") return a.size() == 2 && a[0] == a[1];
    if (name == "isBaseOf") return a.size() == 2 && derivesFrom(a[1], a[0]);
    if (name == "isConvertible")
    {
      // isConvertible<From, To>: an rvalue of From (an lvalue for a reference From, as declval
      // gives) converts to To implicitly, by the conversions overload resolution knows: standard
      // ones, derived to base, converting constructors and conversion operators.
      if (a.size() != 2)
        return false;
      const Ty* from = a[0];
      const Ty* to = a[1];
      if (!from->isKnown() || !to->isKnown() || types.isDependent(from) || types.isDependent(to))
        return std::nullopt;
      if (types.unqualified(types.stripRef(to))->kind == TyKind::Void)
        return types.unqualified(types.stripRef(from))->kind == TyKind::Void;
      if (types.unqualified(types.stripRef(from))->kind == TyKind::Void)
        return false;
      if (to->kind == TyKind::LRef && !to->inner->isConst && from->kind != TyKind::LRef)
        return false; // an rvalue binds no non-const lvalue reference
      return conversionRank(from, to) < NotViable;
    }
    // The traits that need a class's members, answered as C++'s intrinsics answer them; nullopt
    // where that cannot be decided here (unknown or dependent types, a class without definition).
    if (name == "isPolymorphic") return one() ? polymorphicClass(one()) : std::nullopt;
    if (name == "isAbstract") return one() ? abstractClass(one(), {}) : std::nullopt;
    if (name == "isFinal")
    {
      if (!one() || !one()->isKnown() || types.isDependent(one()))
        return std::nullopt;
      if (one()->kind != TyKind::Class)
        return false;
      ClassDecl* def = classDefinition(one());
      return def ? std::optional<bool>(def->isFinal) : std::nullopt;
    }
    if (name == "isMoveConstructible") return one() ? constructibleFromSelf(one(), true, TraitForm::Plain) : std::nullopt;
    if (name == "isCopyConstructible") return one() ? constructibleFromSelf(one(), false, TraitForm::Plain) : std::nullopt;
    if (name == "isNothrowMoveConstructible") return one() ? constructibleFromSelf(one(), true, TraitForm::Nothrow) : std::nullopt;
    if (name == "isTriviallyMoveConstructible") return one() ? constructibleFromSelf(one(), true, TraitForm::Trivially) : std::nullopt;
    if (name == "isMoveAssignable") return one() ? assignableFromSelf(one(), true, TraitForm::Plain) : std::nullopt;
    if (name == "isCopyAssignable") return one() ? assignableFromSelf(one(), false, TraitForm::Plain) : std::nullopt;
    if (name == "isNothrowMoveAssignable") return one() ? assignableFromSelf(one(), true, TraitForm::Nothrow) : std::nullopt;
    if (name == "isTriviallyMoveAssignable") return one() ? assignableFromSelf(one(), true, TraitForm::Trivially) : std::nullopt;
    if (name == "isMemberPointer" || name == "isMemberFunctionPointer" || name == "isMemberObjectPointer")
    {
      if (!one() || !one()->isKnown() || types.isDependent(one()))
        return std::nullopt;
      const Ty* t = types.unqualified(one());
      if (t->kind != TyKind::MemberPointer)
        return false;
      bool function = t->inner && t->inner->kind == TyKind::Function;
      return name == "isMemberPointer" || function == (name == "isMemberFunctionPointer");
    }
    if (name == "isInvocable")
    {
      if (a.empty())
        return std::nullopt;
      std::vector<const Ty*> args(a.begin() + 1, a.end());
      return invocable(a[0], args, nullptr);
    }
    if (name == "isInvocableR")
    {
      // isInvocableR<R, F, Args...>: callable, and the result converts to R (anything to void).
      if (a.size() < 2)
        return std::nullopt;
      std::vector<const Ty*> args(a.begin() + 2, a.end());
      const Ty* result = nullptr;
      std::optional<bool> callable = invocable(a[1], args, &result);
      if (!callable || !*callable)
        return callable;
      if (types.unqualified(a[0])->kind == TyKind::Void)
        return true;
      if (!result || !result->isKnown() || types.isDependent(result))
        return std::nullopt;
      return conversionRank(result, a[0]) < NotViable;
    }
    return std::nullopt;
  }

  bool isBoolTraitName(uint32_t symbol)
  {
    std::string_view n = symbols.name(symbol);
    return n == "isArray" || n == "isPointer" || n == "isEnum" || n == "isClass" || n == "isTypedUnion" || n == "isVoid" || n == "isReference" || n == "isConst" ||
           n == "isNullPointer" || n == "isIntegral" || n == "isFloatingPoint" || n == "isArithmetic" || n == "isSigned" ||
           n == "isUnsigned" || n == "isFunction" || n == "isTriviallyCopyable" || n == "isSame" || n == "isBaseOf" || n == "isConvertible" ||
           n == "isConstructible" || n == "isFundamental" || n == "isTriviallyDestructible" || n == "isTriviallyCopyAssignable" ||
           n == "isTriviallyCopyConstructible" || n == "isTriviallyDefaultConstructible" || n == "isPolymorphic" || n == "isAbstract" ||
           n == "isFinal" || n == "isMoveConstructible" || n == "isCopyConstructible" || n == "isNothrowMoveConstructible" ||
           n == "isTriviallyMoveConstructible" || n == "isMoveAssignable" || n == "isCopyAssignable" || n == "isNothrowMoveAssignable" ||
           n == "isTriviallyMoveAssignable" || n == "isMemberPointer" || n == "isMemberFunctionPointer" || n == "isMemberObjectPointer" ||
           n == "isInvocable" || n == "isInvocableR";
  }

  // --- what the class traits read: definitions, bases, members, special members ---

  // The definition of a class type (the declaration it names may be a forward one), or null.
  ClassDecl* classDefinition(const Ty* t)
  {
    if (!t || t->kind != TyKind::Class || !t->decl)
      return nullptr;
    Decl* def = t->decl;
    if (def->kind == NodeKind::ClassDecl && !def->ownScope)
    {
      Entry e{def, nullptr};
      Scope* s = scopeOfEntry(e, 0);
      def = s ? s->owner : def;
    }
    if (!def || def->kind != NodeKind::ClassDecl || !static_cast<ClassDecl*>(def)->isDefinition)
      return nullptr;
    return static_cast<ClassDecl*>(def);
  }

  // The direct bases of a class type, with its template arguments substituted; false when one
  // is not known.
  bool directBases(const Ty* t, ClassDecl* def, std::vector<const Ty*>& out)
  {
    DeclContext dc;
    declContextFor(def, dc);
    Subst s = substFor(t);
    TypeContext ctx{dc.scope, &dc.params, s.pairs.empty() ? nullptr : &s, def->file ? def->file->tokens : nullptr,
                    def->file ? def->file->text : std::string_view()};
    for (const ClassDecl::Base& b : def->bases)
    {
      const Ty* bt = b.isPack ? nullptr : convertType(b.type, ctx, 0);
      if (!bt || !bt->isKnown() || types.isDependent(bt))
        return false;
      out.push_back(bt);
    }
    return true;
  }

  // The types of the non-static data members, with the class's template arguments substituted;
  // false when one is not known.
  bool fieldTypes(const Ty* t, ClassDecl* def, std::vector<const Ty*>& out)
  {
    Subst s = substFor(t);
    for (Decl* m : def->members)
    {
      if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)))
        continue;
      const Ty* ft = declaredTypeOf(m, s.pairs.empty() ? nullptr : &s, 0);
      if (!ft || !ft->isKnown() || types.isDependent(ft))
        return false;
      out.push_back(ft);
    }
    return true;
  }

  // What an override has to match: the name, the parameter types under the class's template
  // arguments, const. Every destructor overrides every other.
  std::string overrideKey(FunctionDecl* f, const Ty* cls)
  {
    if (f->flags & DF_Destructor)
      return "~";
    const NamePart& n = f->name.last();
    std::string k = n.special == 2 ? "operator#" + std::to_string(n.opToken) : n.special == 3 ? "conversion" : debugName(n.symbol);
    Subst s = substFor(cls);
    const Ty* ft = declaredTypeOf(f, s.pairs.empty() ? nullptr : &s, 0);
    k += "(";
    for (size_t i = 0; ft && ft->kind == TyKind::Function && i < ft->args.size(); ++i)
      k += (i ? ", " : "") + types.toString(ft->args[i], &symbols);
    k += ")";
    if (n.special == 3 && ft && ft->kind == TyKind::Function && ft->inner)
      k += types.toString(ft->inner, &symbols);
    return (f->flags & DF_Const) ? k + " const" : k;
  }

  // isPolymorphic: the class or a base declares a virtual function.
  std::optional<bool> polymorphicClass(const Ty* t, int depth = 0)
  {
    if (!t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->kind != TyKind::Class)
      return false;
    ClassDecl* def = classDefinition(t);
    if (!def)
      return std::nullopt;
    for (Decl* m : def->members)
      if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override | DF_Pure)))
        return true;
    std::vector<const Ty*> bases;
    if (!directBases(t, def, bases))
      return std::nullopt;
    for (const Ty* b : bases)
    {
      std::optional<bool> p = polymorphicClass(b, depth + 1);
      if (!p || *p)
        return p;
    }
    return false;
  }

  // isAbstract: a pure virtual function of the class or a base that no class from the base down
  // to t overrides. `overridden` holds the keys the classes below (more derived) declare.
  std::optional<bool> abstractClass(const Ty* t, std::unordered_set<std::string> overridden, int depth = 0)
  {
    if (!t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->kind != TyKind::Class)
      return false;
    ClassDecl* def = classDefinition(t);
    if (!def)
      return std::nullopt;
    std::vector<FunctionDecl*> pure;
    for (Decl* m : def->members)
      if (m->kind == NodeKind::FunctionDecl && !(m->flags & (DF_Static | DF_Constructor)) && m->templateParams.empty())
      {
        auto* f = static_cast<FunctionDecl*>(m);
        if (f->flags & DF_Pure)
          pure.push_back(f);
        else
          overridden.insert(overrideKey(f, t));
      }
    for (FunctionDecl* f : pure)
      if (!overridden.count(overrideKey(f, t)))
        return true;
    overridden.insert("~"); // the class's destructor, declared or implicit, overrides a pure one of a base
    std::vector<const Ty*> bases;
    if (!directBases(t, def, bases))
      return std::nullopt;
    for (const Ty* b : bases)
    {
      std::optional<bool> a = abstractClass(b, overridden, depth + 1);
      if (!a || *a)
        return a;
    }
    return false;
  }

  // The copy and move constructors and assignment operators a class declares (none of them is a
  // template), and whether it declares a destructor: what decides its implicit ones.
  struct SpecialMembers
  {
    FunctionDecl* copyCtor = nullptr;         // C(const C&)
    FunctionDecl* copyCtorNonConst = nullptr; // C(C&)
    FunctionDecl* moveCtor = nullptr;         // C(C&&)
    FunctionDecl* copyAssign = nullptr;       // operator=(const C&) or operator=(C)
    FunctionDecl* copyAssignNonConst = nullptr;
    FunctionDecl* moveAssign = nullptr;       // operator=(C&&)
    bool destructor = false;
  };
  enum class TraitForm { Plain, Trivially, Nothrow };

  // Whether a parameter's type is the class itself: 1 `const C&`, 2 `C&`, 3 `C&&`, 4 `C`; 0 not.
  static int selfParameter(Decl* p, ClassDecl* def)
  {
    Type* t = static_cast<ParamDecl*>(p)->type;
    int kind = 4;
    if (t && t->kind == NodeKind::LRefType)
    {
      t = static_cast<LRefType*>(t)->referenced;
      kind = 2;
    }
    else if (t && t->kind == NodeKind::RRefType)
    {
      t = static_cast<RRefType*>(t)->referenced;
      kind = 3;
    }
    bool isConst = t && t->kind == NodeKind::ConstType;
    if (isConst)
      t = static_cast<ConstType*>(t)->inner;
    if (!t || t->kind != NodeKind::NamedType || static_cast<NamedType*>(t)->name.empty() || def->name.empty() ||
        static_cast<NamedType*>(t)->name.last().symbol != def->name.last().symbol)
      return 0;
    if (isConst)
      return kind == 2 ? 1 : kind == 4 ? 4 : 0;
    return kind;
  }

  static SpecialMembers specialMembersOf(ClassDecl* def)
  {
    SpecialMembers s;
    for (Decl* m : def->members)
    {
      if (m->kind != NodeKind::FunctionDecl || !m->templateParams.empty())
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      if (f->flags & DF_Destructor)
      {
        s.destructor = true;
        continue;
      }
      size_t required = 0;
      for (Decl* p : f->params)
        required += static_cast<ParamDecl*>(p)->defaultValue ? 0 : 1;
      if (f->params.empty() || required > 1)
        continue;
      int k = selfParameter(f->params[0], def);
      if (f->flags & DF_Constructor)
      {
        if (k == 1)
          s.copyCtor = f;
        else if (k == 2)
          s.copyCtorNonConst = f;
        else if (k == 3)
          s.moveCtor = f;
      }
      else if (!f->name.empty() && f->name.last().special == 2 && f->name.last().opToken == uint32_t(P::Assign))
      {
        if (k == 1 || k == 4)
          s.copyAssign = f;
        else if (k == 2)
          s.copyAssignNonConst = f;
        else if (k == 3)
          s.moveAssign = f;
      }
    }
    return s;
  }

  // A user-declared special member chosen for the trait: deleted is false; a user-provided one
  // is not trivial, and nothrow as declared; a defaulted one is as the implicit one (nullopt:
  // decide by the bases and members).
  static std::optional<bool> declaredSpecial(FunctionDecl* f, TraitForm form)
  {
    if (f->flags & DF_Delete)
      return false;
    if (f->flags & DF_Default)
      return std::nullopt;
    if (form == TraitForm::Trivially)
      return false;
    if (form == TraitForm::Nothrow)
      return (f->flags & DF_Noexcept) != 0;
    return true;
  }

  // isMoveConstructible (move) and isCopyConstructible, in their plain, trivial and nothrow
  // forms: whether T is built from an rvalue (from a const lvalue) of itself by the constructor
  // C++ chooses, the implicit or defaulted ones by the class's bases and members.
  std::optional<bool> constructibleFromSelf(const Ty* t, bool move, TraitForm form, int depth = 0)
  {
    if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->isReference())
      return true;
    if (t->isConst)
      move = false; // `const T&&` binds to `const T&`: the copy constructor
    const Ty* u = types.unqualified(t);
    switch (u->kind)
    {
      case TyKind::Void: case TyKind::Function: case TyKind::Array: return false;
      case TyKind::Class: break;
      case TyKind::Lambda:
      {
        LambdaExpr* l = lambdaOf(u);
        if (!l)
          return std::nullopt;
        return form != TraitForm::Trivially || l->captures.empty() ? std::optional<bool>(true) : std::nullopt;
      }
      case TyKind::Unknown: case TyKind::Param: case TyKind::DependentMember: case TyKind::Auto: case TyKind::Pack: return std::nullopt;
      default: return true; // scalars, pointers, enums
    }
    ClassDecl* def = classDefinition(u);
    if (!def)
      return std::nullopt;
    std::optional<bool> isAbstract = abstractClass(u, {});
    if (!isAbstract || *isAbstract)
      return isAbstract ? std::optional<bool>(false) : std::nullopt;
    SpecialMembers s = specialMembersOf(def);
    FunctionDecl* chosen = move && s.moveCtor ? s.moveCtor : s.copyCtor;
    if (chosen)
    {
      if (std::optional<bool> d = declaredSpecial(chosen, form))
        return d;
    }
    else
    {
      // None declared fits: a declared one that does not (C(C&) given an rvalue, only a move
      // constructor given a const lvalue) means there is no implicit one either.
      if (s.copyCtorNonConst || s.moveCtor || s.moveAssign)
        return false; // a declared move operation deletes the implicit copy constructor
      move = move && !s.copyAssign && !s.copyAssignNonConst && !s.destructor; // else the implicit copy constructor
    }
    return membersAllow(u, def, form, [&](const Ty* m, int d) { return constructibleFromSelf(m, move, form, d); }, false, depth);
  }

  // isMoveAssignable (move) and isCopyAssignable, likewise.
  std::optional<bool> assignableFromSelf(const Ty* t, bool move, TraitForm form, int depth = 0)
  {
    if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->isReference())
    {
      t = types.stripRef(t); // `T& = T&`: a copy assignment of T
      move = false;
    }
    if (t->isConst)
      return false;
    const Ty* u = types.unqualified(t);
    switch (u->kind)
    {
      case TyKind::Void: case TyKind::Function: case TyKind::Array: return false;
      case TyKind::Class: break;
      case TyKind::Lambda:
      {
        LambdaExpr* l = lambdaOf(u);
        if (!l)
          return std::nullopt;
        return l->captures.empty() && !l->defaultCapture; // a closure with captures has a deleted assignment
      }
      case TyKind::Unknown: case TyKind::Param: case TyKind::DependentMember: case TyKind::Auto: case TyKind::Pack: return std::nullopt;
      default: return true;
    }
    ClassDecl* def = classDefinition(u);
    if (!def)
      return std::nullopt;
    SpecialMembers s = specialMembersOf(def);
    FunctionDecl* chosen = move && s.moveAssign ? s.moveAssign : s.copyAssign;
    if (chosen)
    {
      if (std::optional<bool> d = declaredSpecial(chosen, form))
        return d;
    }
    else
    {
      if (s.copyAssignNonConst || s.moveAssign || s.moveCtor)
        return false; // a declared move operation deletes the implicit copy assignment
      move = move && !s.copyCtor && !s.copyCtorNonConst && !s.destructor;
    }
    return membersAllow(u, def, form, [&](const Ty* m, int d) { return assignableFromSelf(m, move, form, d); }, true, depth);
  }

  // The implicit (or defaulted) special member of a class exists, is trivial or nothrow, when
  // every base and member's does; an assignment also needs no reference or const member, and a
  // trivial one no virtual function or base.
  std::optional<bool> membersAllow(const Ty* t, ClassDecl* def, TraitForm form, const std::function<std::optional<bool>(const Ty*, int)>& each,
                                   bool assignment, int depth)
  {
    if (form == TraitForm::Trivially)
    {
      for (const ClassDecl::Base& b : def->bases)
        if (b.isVirtual)
          return false;
      std::optional<bool> p = polymorphicClass(t);
      if (!p || *p)
        return p ? std::optional<bool>(false) : std::nullopt;
    }
    std::vector<const Ty*> parts;
    if (!directBases(t, def, parts) || !fieldTypes(t, def, parts))
      return std::nullopt;
    size_t baseCount = def->bases.size();
    for (size_t i = 0; i < parts.size(); ++i)
    {
      const Ty* m = parts[i];
      bool field = i >= baseCount;
      if (field && assignment && (m->isReference() || types.stripRef(m)->isConst))
        return false;
      if (field && m->isReference())
        continue; // constructing binds it
      while (m->kind == TyKind::Array)
        m = m->inner; // arrays as members are copied element by element
      std::optional<bool> r = each(m, depth + 1);
      if (!r || !*r)
        return r;
    }
    return true;
  }

  // The class an object of type t is made of for the triviality traits (arrays by element), or
  // the answer itself for a type that is no class: nullopt result means "not decided here".
  struct TrivialSubject
  {
    const Ty* cls = nullptr;
    std::optional<bool> answer;
  };
  TrivialSubject trivialSubject(const Ty* t)
  {
    const Ty* u = types.unqualified(t);
    while (u->kind == TyKind::Array)
      u = types.unqualified(u->inner);
    switch (u->kind)
    {
      case TyKind::Class: return {u, std::nullopt};
      case TyKind::Void: case TyKind::Function: return {nullptr, false};
      case TyKind::Lambda:
      {
        LambdaExpr* l = lambdaOf(u);
        return {nullptr, l && l->captures.empty() && !l->defaultCapture ? std::optional<bool>(true) : std::nullopt};
      }
      case TyKind::Unknown: case TyKind::Param: case TyKind::DependentMember: case TyKind::Auto: case TyKind::Pack:
        return {nullptr, std::nullopt};
      default: return {nullptr, true}; // scalars, pointers, enums, member pointers
    }
  }

  // isTriviallyDestructible: no user-provided, deleted or virtual destructor, and every base and
  // member trivially destructible.
  std::optional<bool> triviallyDestructible(const Ty* t, int depth = 0)
  {
    if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->isReference())
      return true;
    TrivialSubject s = trivialSubject(t);
    if (!s.cls)
      return s.answer;
    ClassDecl* def = classDefinition(s.cls);
    if (!def)
      return std::nullopt;
    for (Decl* m : def->members)
      if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) &&
          ((m->flags & DF_Delete) || !(m->flags & DF_Default) || (m->flags & (DF_Virtual | DF_Override))))
        return false;
    std::vector<const Ty*> parts;
    if (!directBases(s.cls, def, parts) || !fieldTypes(s.cls, def, parts))
      return std::nullopt;
    for (const Ty* part : parts)
    {
      std::optional<bool> r = triviallyDestructible(part, depth + 1);
      if (!r || !*r)
        return r;
    }
    return true;
  }

  // isTriviallyCopyable: trivially destructible, and every copy and move constructor and
  // assignment the class has is trivial, at least one of them existing.
  std::optional<bool> triviallyCopyable(const Ty* t)
  {
    if (!t || !t->isKnown() || types.isDependent(t))
      return std::nullopt;
    if (t->isReference())
      return false;
    TrivialSubject s = trivialSubject(t);
    if (!s.cls)
      return s.answer;
    std::optional<bool> destructible = triviallyDestructible(s.cls);
    if (!destructible || !*destructible)
      return destructible;
    bool any = false;
    for (int op = 0; op < 4; ++op)
    {
      bool move = op & 1, assign = op & 2;
      auto test = [&](TraitForm form)
      {
        return assign ? assignableFromSelf(s.cls, move, form) : constructibleFromSelf(s.cls, move, form);
      };
      std::optional<bool> exists = test(TraitForm::Plain);
      if (!exists)
        return std::nullopt;
      if (!*exists)
        continue;
      std::optional<bool> trivial = test(TraitForm::Trivially);
      if (!trivial || !*trivial)
        return trivial;
      any = true;
    }
    return any;
  }

  // isTriviallyDefaultConstructible: a default constructor that is neither user-provided nor
  // deleted, no virtual function or base, no default member initializer, and every base and member
  // trivially default constructible.
  std::optional<bool> triviallyDefaultConstructible(const Ty* t, int depth = 0)
  {
    if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->isReference())
      return false;
    TrivialSubject s = trivialSubject(t);
    if (!s.cls)
      return s.answer;
    ClassDecl* def = classDefinition(s.cls);
    if (!def)
      return std::nullopt;
    bool anyConstructor = false, defaultConstructor = false;
    for (Decl* m : def->members)
    {
      if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)) && static_cast<VariableDecl*>(m)->init)
        return false; // `int count = 0;`: the implicit constructor runs it
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
        continue;
      anyConstructor = true;
      auto* f = static_cast<FunctionDecl*>(m);
      bool takesNone = true;
      for (Decl* p : f->params)
        takesNone = takesNone && static_cast<ParamDecl*>(p)->defaultValue;
      if (!takesNone)
        continue;
      if ((f->flags & DF_Delete) || !(f->flags & DF_Default))
        return false;
      defaultConstructor = true;
    }
    if (anyConstructor && !defaultConstructor)
      return false; // a declared constructor and no default one: none is implicit
    for (const ClassDecl::Base& b : def->bases)
      if (b.isVirtual)
        return false;
    std::optional<bool> polymorphic = polymorphicClass(s.cls);
    if (!polymorphic || *polymorphic)
      return polymorphic ? std::optional<bool>(false) : std::nullopt;
    std::vector<const Ty*> parts;
    if (!directBases(s.cls, def, parts) || !fieldTypes(s.cls, def, parts))
      return std::nullopt;
    for (const Ty* part : parts)
    {
      if (part->isReference())
        return false; // a reference member has no default
      std::optional<bool> r = triviallyDefaultConstructible(part, depth + 1);
      if (!r || !*r)
        return r;
    }
    return true;
  }

  // isInvocable: whether a value of type f can be called with arguments of these types (each a
  // reference type for an lvalue, as in C++'s declval). result, when given, is the call's type
  // where it is known without typing a body.
  std::optional<bool> invocable(const Ty* f, const std::vector<const Ty*>& args, const Ty** result)
  {
    for (const Ty* a : args)
      if (!a || !a->isKnown() || types.isDependent(a))
        return std::nullopt;
    if (!f || !f->isKnown() || types.isDependent(f))
      return std::nullopt;
    const Ty* c = types.unqualified(types.stripRef(f));
    if (c->kind == TyKind::Pointer && c->inner && c->inner->kind == TyKind::Function)
      c = c->inner;
    auto accepts = [&](Span<const Ty*> params, bool variadic) -> std::optional<bool>
    {
      if (args.size() < params.size() || (!variadic && args.size() > params.size()))
        return false;
      for (size_t i = 0; i < params.size(); ++i)
      {
        if (!params[i] || !params[i]->isKnown() || types.isDependent(params[i]))
          return std::nullopt;
        if (conversionRank(args[i], params[i]) >= NotViable)
          return false;
      }
      return true;
    };
    if (c->kind == TyKind::Function)
    {
      if (result)
        *result = c->inner;
      return accepts(c->args, c->variadic);
    }
    if (c->kind == TyKind::Lambda)
    {
      LambdaExpr* l = lambdaOf(c);
      if (!l)
        return std::nullopt;
      if (l->templateParams.empty())
      {
        if (result)
          *result = c->inner;
        return accepts(c->args, false);
      }
      // A generic lambda: its `auto` parameters deduce from the arguments; its result needs the
      // body typed for them, which is left to the call.
      Subst deduced;
      deduceParams(l->params, c->args, args, deduced);
      std::vector<const Ty*> params;
      bool pack = false;
      for (const Ty* p : c->args)
      {
        const Ty* s = substitute(p, deduced);
        if (s->kind == TyKind::Pack)
        {
          pack = true;
          for (const Ty* e : s->args)
            params.push_back(e);
        }
        else
          params.push_back(s);
      }
      if (!pack && args.size() != params.size())
        return false;
      return accepts(Span<const Ty*>{params.data(), uint32_t(params.size())}, pack);
    }
    if (c->kind == TyKind::Class)
    {
      // A function object: its call operators, its bases' too, by overload resolution.
      std::vector<FunctionDecl*> candidates;
      std::vector<const Ty*> classes{c};
      for (size_t i = 0; i < classes.size() && i < 32; ++i)
      {
        ClassDecl* def = classDefinition(classes[i]);
        if (!def)
          return std::nullopt;
        for (Decl* m : def->members)
          if (m->kind == NodeKind::FunctionDecl)
            if (auto* fn = static_cast<FunctionDecl*>(m); !fn->name.empty() && fn->name.last().special == 2 && fn->name.last().opToken == uint32_t(P::LParen))
              candidates.push_back(fn);
        if (!directBases(classes[i], def, classes))
          return std::nullopt;
      }
      if (candidates.empty())
        return false;
      Subst s = substFor(c);
      bool ambiguous = false;
      FunctionDecl* chosen = chooseAmong(candidates, args, s.pairs.empty() ? nullptr : &s, f, ambiguous);
      if (!chosen || ambiguous)
        return false;
      if (result)
      {
        const Ty* ft = declaredTypeOf(chosen, s.pairs.empty() ? nullptr : &s, 0);
        *result = ft && ft->kind == TyKind::Function ? ft->inner : nullptr;
      }
      return true;
    }
    return std::nullopt; // member pointers and the rest: not decided here
  }

  // Walk a class's own members and its bases to find `static constexpr bool value`.
  // Returns the VariableDecl if found, nullptr otherwise.  Used by constEval to fold
  // `SomeTrait<T>()` (a zero-arg constructor of a TrueType/FalseType–derived struct)
  // and `SomeTrait<T>::value` (a qualified access to the inherited static member).
  VariableDecl* findStaticBoolValue(ClassDecl* c, int depth)
  {
    if (!c || depth > 8)
      return nullptr;
    for (Decl* m : c->members)
    {
      if (m->kind != NodeKind::VariableDecl || !(m->flags & DF_Static))
        continue;
      auto* v = static_cast<VariableDecl*>(m);
      if (v->name.empty() || v->name.last().symbol != symValue)
        continue;
      if ((v->flags & DF_Constexpr) && v->init)
        return v;
    }
    if (c->ownScope)
    {
      ensureBases(c->ownScope, depth);
      for (Scope* base : c->ownScope->baseScopes)
        if (base->owner && base->owner->kind == NodeKind::ClassDecl)
          if (VariableDecl* found = findStaticBoolValue(static_cast<ClassDecl*>(base->owner), depth + 1))
            return found;
    }
    return nullptr;
  }

  // Minimal constant evaluation for array bounds: literals and arithmetic on them.
  // The value of a declaration used as a constant: a const/constexpr variable with an
  // initializer, an enumerator, or a non-type template parameter under the substitution.
  void reportCheckerError(const ParsedFile* file, uint32_t token, const std::string& message)
  {
    std::string where = file ? file->path : std::string("?");
    if (file && file->tokens && !file->tokens->empty())
    {
      const Token& t = (*file->tokens)[std::min<size_t>(token, file->tokens->size() - 1)];
      where += ":" + std::to_string(1 + std::count(file->text.begin(), file->text.begin() + std::min<size_t>(t.offset, file->text.size()), '\n'));
    }
    std::lock_guard lock(checkerErrorsMutex);
    checkerErrorSet.insert(where + ": error: " + message);
  }

  // A static_assert under ctx (an instance's arguments, when it is in a template): an error when
  // its condition is false, or is no constant the checker can evaluate (never a silent pass).
  // instance: which instance, for the message. One that needs a class's size or alignment waits
  // for the code generator's layout (layoutAsserts).
  void checkStaticAssert(StaticAssertDecl* s, const TypeContext& ctx, const std::string& instance)
  {
    if (!s->cond)
      return;
    TokenScope tokenScope(ctx.tokens, ctx.text);
    // An instance whose arguments the checker does not know (`EnumType = ?`) decides nothing.
    bool knownInstance = true;
    if (ctx.subst)
      for (const Subst::Pair& p : ctx.subst->pairs)
        knownInstance = knownInstance && p.type && !mentionsUnknown(p.type);
    exactLayoutOnly = true;
    layoutNeeded = false;
    std::optional<int64_t> v = knownInstance ? constEval(s->cond, ctx, 0) : std::nullopt;
    exactLayoutOnly = false;
    if (!v && layoutNeeded)
    {
      std::lock_guard lock(checkerErrorsMutex);
      LayoutAssert& a = layoutAsserts.emplace_back();
      a.decl = s;
      if (ctx.subst)
        a.subst = *ctx.subst;
      a.instance = instance;
      return;
    }
    if (v && *v)
      return;
    std::string message = v ? "static assertion failed" : "static assertion whose condition the compiler cannot evaluate yet";
    if (s->message && s->message->kind == NodeKind::LiteralExpr && s->file && s->file->tokens && !s->file->tokens->empty())
    {
      const Token& t = (*s->file->tokens)[std::min<size_t>(s->message->token, s->file->tokens->size() - 1)];
      message += ": " + std::string(s->file->text.substr(t.offset, t.length));
    }
    // A failure names its instance; a condition the checker cannot evaluate is its gap at that
    // place, one error however many instances meet it.
    if (v && !instance.empty())
      message += " (in " + instance + ")";
    reportCheckerError(s->file, s->token, message);
  }

  // classSubst: the arguments of the class template instantiation a static member was named
  // through (`Bits<uint64_t>::count`).
  std::optional<int64_t> constOfDecl(Decl* d, const TypeContext& ctx, int depth, const Subst* classSubst = nullptr)
  {
    if (!d || depth > 16)
      return std::nullopt;
    if (d->kind == NodeKind::TemplateParamDecl)
    {
      if (ctx.subst)
        if (const Ty* t = ctx.subst->find(d); t && t->kind == TyKind::Value)
          return t->value;
      return std::nullopt;
    }
    if (d->kind == NodeKind::VariableDecl)
    {
      auto* v = static_cast<VariableDecl*>(d);
      VariableDecl* def = v; // `static const int LIMIT;` in the class, `const int Gui::LIMIT = 21;` outside
      if (!v->init)
        if (auto it = variableDefinitions.find(v); it != variableDefinitions.end())
          def = it->second;
      if (!def->init)
        return std::nullopt;
      TypeContext vctx{lexicalScopeOf(v), nullptr, nullptr, v->file ? v->file->tokens : nullptr, v->file ? v->file->text : std::string_view()};
      const Ty* t = convertType(v->type, vctx, depth + 1);
      if (!(v->flags & DF_Constexpr) && !(t && t->isConst))
        return std::nullopt;
      // The definition's initializer, read from its file, evaluated where the declaration is (the class).
      // A constant local to a function body (`constexpr bool sign = isSigned<T>;`): its initializer
      // under the context it is used in, which has the instantiation's arguments.
      if (v->parent && v->parent->kind == NodeKind::FunctionDecl && v->name.parts.size() == 1)
        return constEval(def->init, ctx, depth + 1);
      // A member of a class template: under the instantiation's arguments, the ones it was named
      // through or the context's when they bind the class's parameters (`count` read in another
      // member's initializer).
      Span<Decl*> classParams = v->parent ? effectiveParams(v->parent) : Span<Decl*>{};
      const Subst* memberArgs = classSubst;
      if (!memberArgs && ctx.subst)
        for (Decl* p : classParams)
          if (ctx.subst->find(p))
          {
            memberArgs = ctx.subst;
            break;
          }
      LocalStack params;
      for (Decl* p : classParams)
        params.push(static_cast<TemplateParamDecl*>(p)->name, p);
      TypeContext ictx{lexicalScopeOf(v), classParams.empty() ? nullptr : &params, memberArgs, def->file ? def->file->tokens : nullptr,
                       def->file ? def->file->text : std::string_view()};
      return constEval(def->init, ictx, depth + 1);
    }
    if (d->kind == NodeKind::EnumeratorDecl)
    {
      // Enumerators count up from zero, or from the last written value.
      auto* e = static_cast<EnumDecl*>(d->parent);
      if (!e || e->kind != NodeKind::EnumDecl)
        return std::nullopt;
      // In the enum's own scope: `GENERIC = NOTHING` names an earlier enumerator of an enum class.
      TypeContext ectx{e->ownScope ? e->ownScope : lexicalScopeOf(e), nullptr, nullptr, e->file ? e->file->tokens : nullptr,
                       e->file ? e->file->text : std::string_view()};
      int64_t next = 0;
      for (Decl* m : e->enumerators)
      {
        if (m->kind != NodeKind::EnumeratorDecl)
          continue;
        auto* cur = static_cast<EnumeratorDecl*>(m);
        if (cur->value)
        {
          std::optional<int64_t> v = constEval(cur->value, ectx, depth + 1);
          if (!v)
            return std::nullopt;
          next = *v;
        }
        if (cur == d)
          return next;
        ++next;
      }
    }
    return std::nullopt;
  }

  // Compile-time sizeof/alignof for constEval — mirrors the codegen's sizeOf/alignOf but returns
  // nullopt instead of throwing when a type can't be sized (the codegen may still handle it later).
  // Set while a static_assert is checked: a class's size or alignment is then left to the code
  // generator, whose layout is the exact one (bit fields, empty bases), and layoutNeeded says so.
  static thread_local bool exactLayoutOnly;
  static thread_local bool layoutNeeded;

  std::optional<int64_t> constSizeOf(const Ty* t, int depth)
  {
    if (!t || depth > 24)
      return std::nullopt;
    t = types.unqualified(t);
    if (t->kind == TyKind::Class && exactLayoutOnly)
    {
      layoutNeeded = true;
      return std::nullopt;
    }
    switch (t->kind)
    {
      case TyKind::Bool: return 1;
      case TyKind::Char: case TyKind::Int: case TyKind::Float:
        return t->bits / 8;
      case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef:
      case TyKind::Function: case TyKind::MemberPointer:
        return 8;
      case TyKind::Array:
      {
        auto inner = constSizeOf(t->inner, depth + 1);
        if (!inner) return std::nullopt;
        return int64_t(t->count) * *inner;
      }
      case TyKind::Enum:
      {
        Decl* def = t->decl;
        if (def && !static_cast<EnumDecl*>(def)->ownScope)
        {
          Entry e{def, nullptr};
          if (Scope* s = scopeOfEntry(e, 0))
            def = s->owner;
        }
        if (def && def->kind == NodeKind::EnumDecl && static_cast<EnumDecl*>(def)->underlying)
        {
          DeclContext dc;
          declContextFor(def, dc);
          TypeContext ctx2{dc.scope, &dc.params, nullptr, def->file ? def->file->tokens : nullptr,
                          def->file ? def->file->text : std::string_view()};
          if (const Ty* u = convertType(static_cast<EnumDecl*>(def)->underlying, ctx2, depth + 1))
            if (u->isKnown())
              return constSizeOf(u, depth + 1);
        }
        return 4; // default: int-sized
      }
      case TyKind::Class:
      {
        Decl* def = t->decl;
        if (def && !def->ownScope)
        {
          Entry e{def, nullptr};
          if (Scope* s = scopeOfEntry(e, 0))
            def = s->owner;
        }
        if (!def || def->kind != NodeKind::ClassDecl || !static_cast<ClassDecl*>(def)->isDefinition)
          return std::nullopt;
        auto* cls = static_cast<ClassDecl*>(def);
        Subst subst = substFor(t);
        const Subst* sp = subst.empty() ? nullptr : &subst;
        bool isUnion = cls->keyword == Kw::union_;
        int64_t size = 0, align = 1;
        // Base classes contribute their fields before own members.
        if (!isUnion)
          for (const ClassDecl::Base& b : cls->bases)
          {
            DeclContext dc;
            declContextFor(cls, dc);
            TypeContext ctx2{dc.scope, &dc.params, sp, cls->file ? cls->file->tokens : nullptr,
                            cls->file ? cls->file->text : std::string_view()};
            const Ty* bt = convertType(b.type, ctx2, depth + 1);
            if (!bt || !bt->isKnown() || types.isDependent(bt))
              return std::nullopt;
            auto bs = constSizeOf(bt, depth + 1);
            auto ba = constAlignOf(bt, depth + 1);
            if (!bs || !ba) return std::nullopt;
            size = (size + *ba - 1) / *ba * *ba + *bs;
            align = std::max(align, *ba);
          }
        for (Decl* m : cls->members)
        {
          const Ty* ft = nullptr;
          if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
            ft = declaredTypeOf(m, sp, depth + 1);
          else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.parts.empty() &&
                   static_cast<ClassDecl*>(m)->isDefinition)
            ft = types.classType(m, {});
          else
            continue;
          if (!ft || !ft->isKnown() || types.isDependent(ft))
            return std::nullopt;
          auto fs = constSizeOf(ft, depth + 1);
          auto fa = constAlignOf(ft, depth + 1);
          if (!fs || !fa) return std::nullopt;
          if (isUnion)
          {
            size = std::max(size, *fs);
            align = std::max(align, *fa);
          }
          else
          {
            size = (size + *fa - 1) / *fa * *fa + *fs;
            align = std::max(align, *fa);
          }
        }
        size = (size + align - 1) / align * align;
        return std::max<int64_t>(size, 1);
      }
      default:
        return std::nullopt;
    }
  }

  std::optional<int64_t> constAlignOf(const Ty* t, int depth)
  {
    if (!t || depth > 24) return std::nullopt;
    t = types.unqualified(t);
    if (t->kind == TyKind::Array)
      return constAlignOf(t->inner, depth + 1);
    if (t->kind == TyKind::Class && exactLayoutOnly)
    {
      layoutNeeded = true; // only the code generator's layout is exact (bit fields, empty bases)
      return std::nullopt;
    }
    if (t->kind == TyKind::Class)
    {
      Decl* def = t->decl;
      if (def && !def->ownScope)
      {
        Entry e{def, nullptr};
        if (Scope* s = scopeOfEntry(e, 0))
          def = s->owner;
      }
      if (!def || def->kind != NodeKind::ClassDecl || !static_cast<ClassDecl*>(def)->isDefinition)
        return std::nullopt;
      auto* cls = static_cast<ClassDecl*>(def);
      Subst subst = substFor(t);
      const Subst* sp = subst.empty() ? nullptr : &subst;
      int64_t align = 1;
      for (const ClassDecl::Base& b : cls->bases)
      {
        DeclContext dc;
        declContextFor(cls, dc);
        TypeContext ctx2{dc.scope, &dc.params, sp, cls->file ? cls->file->tokens : nullptr,
                        cls->file ? cls->file->text : std::string_view()};
        const Ty* bt = convertType(b.type, ctx2, depth + 1);
        if (!bt || !bt->isKnown()) return std::nullopt;
        auto ba = constAlignOf(bt, depth + 1);
        if (!ba) return std::nullopt;
        align = std::max(align, *ba);
      }
      for (Decl* m : cls->members)
      {
        const Ty* ft = nullptr;
        if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
          ft = declaredTypeOf(m, sp, depth + 1);
        else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.parts.empty() &&
                 static_cast<ClassDecl*>(m)->isDefinition)
          ft = types.classType(m, {});
        else
          continue;
        if (!ft || !ft->isKnown()) return std::nullopt;
        auto fa = constAlignOf(ft, depth + 1);
        if (!fa) return std::nullopt;
        align = std::max(align, *fa);
      }
      return align;
    }
    auto s = constSizeOf(t, depth);
    if (!s) return std::nullopt;
    return std::max<int64_t>(1, *s);
  }

  // `static_cast<size_t>(value)` with value an enumerator or a constant parameter (a template
  // argument `bits.test<static_cast<size_t>(value)>()`): the operand's value in the target type.
  std::optional<int64_t> constCast(Type* type, Expr* operand, const TypeContext& ctx, int depth)
  {
    std::optional<int64_t> v = constEval(operand, ctx, depth + 1);
    if (!v || !type)
      return v;
    const Ty* t = types.unqualified(convertType(type, ctx, depth + 1));
    if (t->kind == TyKind::Bool)
      return *v != 0 ? 1 : 0;
    if ((t->kind == TyKind::Int || t->kind == TyKind::Char) && t->bits > 0 && t->bits < 64)
    {
      uint64_t mask = (uint64_t(1) << t->bits) - 1, bits = uint64_t(*v) & mask;
      if (!t->isUnsigned && (bits >> (t->bits - 1)))
        return int64_t(bits | ~mask); // a negative value of a narrower signed type
      return int64_t(bits);
    }
    return v;
  }

  std::optional<int64_t> constEval(Expr* e, const TypeContext& ctx, int depth = 0)
  {
    if (!e || depth > 16)
      return std::nullopt;
    switch (e->kind)
    {
      case NodeKind::NameExpr:
      {
        auto* n = static_cast<NameExpr*>(e);
        if (ctx.env && n->name.parts.size() == 1)
          for (const ConstBinding& b : *ctx.env)
            if (b.symbol == n->name.parts[0].symbol)
              return b.value; // a parameter of the constexpr function being evaluated
        Lookup l = lookupQualified(n->name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
        if (l.found == Found::No && !n->resolved && n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs &&
            isBoolTraitName(n->name.parts[0].symbol))
        {
          // `isSame<T, int>` as a constant (an if constexpr condition under the substitution).
          std::vector<const Ty*> a = traitArgs(n->name.parts[0], ctx, depth + 1);
          if (a.empty())
            return std::nullopt;
          std::optional<bool> v = traitValue(symbols.name(n->name.parts[0].symbol), a);
          return v ? std::optional<int64_t>(*v ? 1 : 0) : std::nullopt;
        }
        // `if constexpr (has_get_entity<OwnerType>)`: a concept for these arguments, not the value its
        // requires-expression was last typed to.
        if (n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs)
          if (VariableDecl* concept_ = conceptOf(l.decl))
          {
            std::optional<bool> v = conceptValue(concept_, n->name.parts[0], ctx, depth + 1);
            return v ? std::optional<int64_t>(*v ? 1 : 0) : std::nullopt;
          }
        // `hasBegin<Vector<int>>`, `bytes<T>` under an instantiation: a variable template for these
        // arguments, not its initializer as written.
        if (n->name.last().hasTemplateArgs)
          if (VariableDecl* primary = primaryVariableTemplate(l))
            return variableTemplateValue(primary, n->name.last(), ctx, depth + 1);
        // `Bits<uint64_t>::count`: a static member of a class template, under that instantiation's
        // arguments.
        if (n->name.parts.size() > 1 && l.decl && l.decl->kind == NodeKind::VariableDecl)
          if (Subst qs = qualifierSubst(n->name, ctx, depth + 1, l.foundIn); !qs.pairs.empty())
            return constOfDecl(l.decl, ctx, depth, &qs);
        // A name the context cannot see (a for constexpr variable in `int buffer[i + 1]`): the
        // declaration the type checker bound where the name is written.
        return constOfDecl(l.decl ? l.decl : n->resolved, ctx, depth);
      }
      case NodeKind::MemberExpr:
      {
        // `Enum::Value` is parsed as a name; `object.member` is rarely constant, but
        // `object.value` where the member is a static constexpr variable is (e.g. trait structs).
        Decl* d = static_cast<MemberExpr*>(e)->resolved;
        if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Static))
          return constOfDecl(d, ctx, depth + 1);
        return std::nullopt;
      }
      case NodeKind::CallExpr:
      {
        // `cells()`: a call to a function whose body is one `return expression;`, evaluated with
        // the arguments bound to its parameters (constexpr functions as bounds and constants).
        auto* c = static_cast<CallExpr*>(e);
        if (!c->callee || c->callee->kind != NodeKind::NameExpr)
          return std::nullopt;
        auto* callee = static_cast<NameExpr*>(c->callee);
        Decl* d = callee->resolved;
        if (!d || d->kind == NodeKind::TemplateParamDecl)
          d = lookupQualified(callee->name, ctx.scope, ctx.locals, depth + 1, ctx.subst).decl;
        if (!d)
          return std::nullopt;
        if (d->kind == NodeKind::EnumDecl && c->args.size() == 1)
          return constEval(c->args[0], ctx, depth + 1); // `U::Tag(0)`: a conversion to an enumeration
        if (d->kind == NodeKind::ClassDecl && c->args.empty())
        {
          // `is_optional<T>()`: a zero-arg constructor of a type-trait struct used as bool.
          // Return its static constexpr `value` member (inherited from TrueType/FalseType).
          if (VariableDecl* v = findStaticBoolValue(static_cast<ClassDecl*>(d), depth + 1))
          {
            TypeContext vctx{lexicalScopeOf(v), nullptr, nullptr, v->file ? v->file->tokens : nullptr, v->file ? v->file->text : std::string_view()};
            return constEval(v->init, vctx, depth + 1);
          }
          return std::nullopt;
        }
        if (d->kind != NodeKind::FunctionDecl)
          return std::nullopt;
        auto* f = static_cast<FunctionDecl*>(d);
        if (!f->body || f->body->kind != NodeKind::BlockStmt || f->params.size() != c->args.size())
          return std::nullopt;
        auto* block = static_cast<BlockStmt*>(f->body);
        if (block->stmts.size() != 1 || !block->stmts[0] || block->stmts[0]->kind != NodeKind::ReturnStmt)
          return std::nullopt;
        std::vector<ConstBinding> env;
        for (size_t i = 0; i < c->args.size(); ++i)
        {
          auto v = constEval(c->args[i], ctx, depth + 1);
          if (!v)
            return std::nullopt;
          env.push_back({static_cast<ParamDecl*>(f->params[i])->name, *v});
        }
        TypeContext fctx{lexicalScopeOf(f), nullptr, nullptr, f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
        fctx.env = &env;
        return constEval(static_cast<ReturnStmt*>(block->stmts[0])->value, fctx, depth + 1);
      }
      case NodeKind::LiteralExpr:
      {
        auto* l = static_cast<LiteralExpr*>(e);
        if (l->literalKind == TokenKind::Keyword) // nullptr: the null pointer constant (`fold = nullptr` as a template argument)
          return l->keyword == Kw::true_ ? 1 : l->keyword == Kw::false_ || l->keyword == Kw::nullptr_ ? 0 : std::optional<int64_t>();
        const std::vector<Token>* toks = ctx.tokens ? ctx.tokens : fallbackTokens;
        std::string_view text = ctx.tokens ? ctx.text : fallbackText;
        if (l->literalKind != TokenKind::Number || !toks)
          return std::nullopt;
        const Token& t = (*toks)[std::min<size_t>(l->token, toks->size() - 1)];
        std::string digits;
        for (char c : text.substr(t.offset, t.length))
          if (c != '\'')
            digits += c;
        // A floating literal (`0.25`, `1e-9`, `2.f`) is no integer: not cut to one (`0.25 < 0.45`
        // would read 0 < 0).
        bool hex = digits.size() > 1 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X');
        if (digits.find('.') != std::string::npos || (!hex && digits.find_first_of("eEfF") != std::string::npos))
          return std::nullopt;
        try
        {
          if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B'))
            return std::stoll(digits.substr(2), nullptr, 2); // 0b1010
          return std::stoll(digits, nullptr, 0);
        }
        catch (...)
        {
          return std::nullopt;
        }
      }
      case NodeKind::ParenExpr: return constEval(static_cast<ParenExpr*>(e)->inner, ctx, depth + 1);
      case NodeKind::CastExpr: return constCast(static_cast<CastExpr*>(e)->type, static_cast<CastExpr*>(e)->operand, ctx, depth);
      case NodeKind::NamedCastExpr: return constCast(static_cast<NamedCastExpr*>(e)->type, static_cast<NamedCastExpr*>(e)->operand, ctx, depth);
      case NodeKind::FunctionalCastExpr:
      {
        auto* f = static_cast<FunctionalCastExpr*>(e);
        return f->args.size() == 1 ? constCast(f->type, f->args[0], ctx, depth) : std::nullopt;
      }
      case NodeKind::RequiresExpr:
        if (ctx.variableTemplate && ctx.subst)
        {
          // In `hasBegin<Vector<int>>`'s initializer: the requirements for these arguments.
          std::optional<bool> b = requirementsHold(static_cast<RequiresExpr*>(e), ctx.variableTemplate, *ctx.subst);
          return b ? std::optional<int64_t>(*b ? 1 : 0) : std::nullopt;
        }
        return static_cast<RequiresExpr*>(e)->satisfied ? 1 : 0; // set where the expression was typed
      case NodeKind::ConditionalExpr:
      {
        // `isSigned<T> ? count - 1 : count`: only the branch taken has to be a constant.
        auto* c = static_cast<ConditionalExpr*>(e);
        auto cond = constEval(c->cond, ctx, depth + 1);
        if (!cond)
          return std::nullopt;
        if (!c->then)
          return *cond ? cond : constEval(c->else_, ctx, depth + 1); // `a ?: b`
        return constEval(*cond ? c->then : c->else_, ctx, depth + 1);
      }
      case NodeKind::UnaryExpr:
      {
        auto* u = static_cast<UnaryExpr*>(e);
        auto v = constEval(u->operand, ctx, depth + 1);
        if (!v || u->isKeyword)
          return std::nullopt;
        if (P(u->op) == P::Minus) return -*v;
        if (P(u->op) == P::Plus) return *v;
        if (P(u->op) == P::Bang) return !*v ? 1 : 0;
        if (P(u->op) == P::Tilde) return ~*v;
        return std::nullopt;
      }
      case NodeKind::BinaryExpr:
      {
        auto* b = static_cast<BinaryExpr*>(e);
        auto l = constEval(b->lhs, ctx, depth + 1);
        // Short-circuit: `true || <unknown>` is 1, `false && <unknown>` is 0.
        if (l && P(b->op) == P::OrOr && *l)
          return 1;
        if (l && P(b->op) == P::AndAnd && !*l)
          return 0;
        auto r = constEval(b->rhs, ctx, depth + 1);
        if (l && r && P(b->op) == P::OrOr)
          return (*l || *r) ? 1 : 0;
        if (l && r && P(b->op) == P::AndAnd)
          return (*l && *r) ? 1 : 0;
        if (!l || !r || b->isKeyword)
          return std::nullopt;
        switch (P(b->op))
        {
          case P::Plus: return *l + *r;
          case P::Minus: return *l - *r;
          case P::Star: return *l * *r;
          case P::Slash: return *r ? *l / *r : std::optional<int64_t>();
          case P::Percent: return *r ? *l % *r : std::optional<int64_t>();
          case P::Shl: return *l << *r;
          case P::Shr: return *l >> *r;
          case P::Amp: return *l & *r; // `BOTH = ONE | 2` inside an enum: the underlying values
          case P::Pipe: return *l | *r;
          case P::Caret: return *l ^ *r;
          case P::Less: return *l < *r;
          case P::Greater: return *l > *r;
          case P::LessEq: return *l <= *r;
          case P::GreaterEq: return *l >= *r;
          case P::EqEq: return *l == *r;
          case P::NotEq: return *l != *r;
          default: return std::nullopt;
        }
      }
      case NodeKind::SizeofExpr:
      {
        auto* s = static_cast<SizeofExpr*>(e);
        if (s->isPack)
        {
          // `sizeof...(Renderers)` (a template argument: StaticVector<T, sizeof...(Renderers)>):
          // the number of elements the pack, a type pack or a function parameter pack, is bound to.
          const QualifiedName* name = s->type && s->type->kind == NodeKind::NamedType ? &static_cast<NamedType*>(s->type)->name
                                    : s->operand && s->operand->kind == NodeKind::NameExpr ? &static_cast<NameExpr*>(s->operand)->name : nullptr;
          if (!name || name->parts.size() != 1 || !ctx.subst)
            return std::nullopt;
          Lookup l = lookupQualified(*name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
          Decl* d = l.decl;
          if (!d) // `sizeof...(args)` where the context has no locals (the code generator's): what the walk found
            d = s->type && s->type->kind == NodeKind::NamedType ? static_cast<NamedType*>(s->type)->resolved
              : s->operand && s->operand->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(s->operand)->resolved : nullptr;
          const Ty* pack = nullptr;
          if (d && d->kind == NodeKind::TemplateParamDecl)
            pack = ctx.subst->find(d);
          else if (d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->isPack)
            pack = declaredTypeOf(d, ctx.subst, depth + 1);
          return pack && pack->kind == TyKind::Pack ? std::optional<int64_t>(int64_t(pack->args.size())) : std::nullopt;
        }
        const Ty* t = nullptr;
        if (s->type)
          t = convertType(s->type, ctx, depth + 1);
        else if (s->operand && s->operand->ty)
          t = s->operand->ty;
        if (!t || !t->isKnown() || types.isDependent(t))
          return std::nullopt;
        return constSizeOf(types.stripRef(t), depth + 1);
      }
      default:
        return std::nullopt;
    }
  }

  static Decl* enclosingClassDecl(Scope* s)
  {
    for (; s; s = s->parent)
      if (s->kind == ScopeKind::Class || s->kind == ScopeKind::Enum)
        return s->owner;
    return nullptr;
  }

  static size_t enumeratorCount(Decl* e)
  {
    size_t n = 0;
    for (Decl* d : static_cast<EnumDecl*>(e)->enumerators)
      n += d->kind == NodeKind::EnumeratorDecl;
    return n;
  }

  // Type of a class or enum declaration as written inside it: template parameters stay dependent.
  // The template parameters a class is instantiated with: its own, or, for a class nested in a
  // class template (`Holder<T>::Node`), the enclosing template's.
  static Span<Decl*> effectiveParams(Decl* cls)
  {
    for (Decl* d = cls; d; d = d->parent)
    {
      if (d->kind != NodeKind::ClassDecl && d->kind != NodeKind::EnumDecl)
        break;
      if (!d->templateParams.empty())
        return d->templateParams;
    }
    return {};
  }

  const Ty* selfType(Decl* d)
  {
    if (d->kind == NodeKind::EnumDecl)
      return types.enumType(d);
    std::vector<const Ty*> args;
    for (Decl* p : effectiveParams(d))
      args.push_back(types.param(p));
    return types.classType(d, Span<const Ty*>{args.data(), uint32_t(args.size())});
  }

  // Maps the template parameters of `cls` to the arguments of an instantiation type.
  Subst substFor(const Ty* classTy)
  {
    Subst s;
    if (!classTy || classTy->kind != TyKind::Class || !classTy->decl)
      return s;
    Span<Decl*> params = effectiveParams(classTy->decl);
    for (size_t i = 0; i < params.size() && i < classTy->args.size(); ++i)
    {
      if (params[i]->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(params[i])->isPack)
      {
        // `template<typename R, typename... Args>`: the pack takes the rest of the arguments.
        std::vector<const Ty*> rest(classTy->args.begin() + i, classTy->args.end());
        if (rest.size() == 1 && rest[0]->kind == TyKind::Pack)
          s.pairs.push_back({params[i], rest[0]});
        else
          s.pairs.push_back({params[i], types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
        break;
      }
      s.pairs.push_back({params[i], classTy->args[i]});
    }
    if (!params.empty() && classTy->args.size() == params.size() - 1)
    {
      Decl* lastParam = params[params.size() - 1];
      if (lastParam->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(lastParam)->isPack)
        s.pairs.push_back({lastParam, types.pack({})}); // `Function<void()>`: an empty pack
    }
    return s;
  }

  // The instantiation of the base class whose scope is `in` that class type t (scope cls)
  // derives from (`Prototype<ItemID>` for ItemPrototype and Prototype's scope), t itself when in
  // is cls; null when in is no base of it.
  const Ty* instantiationOfBase(const Ty* t, Scope* cls, Scope* in, int depth)
  {
    if (cls == in)
      return t;
    if (!cls || depth > 16 || cls->baseTypes.empty() || !cls->owner)
      return nullptr;
    Subst own = substFor(t);
    LocalStack params;
    for (Decl* p : cls->templateParams)
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    ParsedFile* file = cls->owner->file;
    TypeContext ctx{cls->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
    for (Type* b : cls->baseTypes)
    {
      const Ty* bt = convertType(b, ctx, depth + 1);
      if (!bt || bt->kind != TyKind::Class || !bt->decl)
        continue;
      Scope* bs = bt->decl->ownScope;
      if (!bs)
      {
        Entry e{bt->decl, nullptr};
        bs = scopeOfEntry(e, 0);
      }
      if (const Ty* found = instantiationOfBase(bt, bs, in, depth + 1))
        return found;
    }
    return nullptr;
  }

  // A constant argument has its parameter's type, read with the arguments before it (`template<
  // class T, T V>`: V's type is the first argument), so that `Constant<bool, true>` and
  // `Constant<bool, isSigned<T>>` with a signed T are one class whichever way the constant was
  // computed (a literal is typed by the parameter's type unbound, a trait as bool).
  void normalizeValueArgs(Span<Decl*> params, std::vector<const Ty*>& args, Decl* def, int depth)
  {
    for (size_t i = 0; i < args.size() && i < params.size(); ++i)
    {
      auto* p = static_cast<TemplateParamDecl*>(params[i]);
      const Ty* a = args[i];
      if (p->isType || p->isPack || !p->valueType || !a || a->kind != TyKind::Value || a->decl)
        continue; // a function as a constant keeps its own type
      LocalStack own;
      Subst s;
      for (size_t k = 0; k < params.size(); ++k)
        own.push(static_cast<TemplateParamDecl*>(params[k])->name, params[k]);
      for (size_t k = 0; k < i; ++k)
        s.pairs.push_back({params[k], args[k]});
      TypeContext vctx{lexicalScopeOf(def), &own, s.pairs.empty() ? nullptr : &s, def->file ? def->file->tokens : nullptr,
                       def->file ? def->file->text : std::string_view()};
      const Ty* of = types.unqualified(convertType(p->valueType, vctx, depth + 1));
      if (of && of->isKnown() && !types.isDependent(of) && of != a->inner)
        args[i] = types.valueType(of, of->kind == TyKind::Bool ? int64_t(a->value != 0) : a->value);
    }
  }

  // Whether class type t (scope cls) is, or derives from, the class instance `base` exactly,
  // through any of its bases as their arguments make them.
  bool derivesFromInstance(const Ty* t, Scope* cls, const Ty* base, int depth)
  {
    if (!t || !cls || depth > 16)
      return false;
    if (types.unqualified(t) == base)
      return true;
    if (cls->baseTypes.empty() || !cls->owner)
      return false;
    Subst own = substFor(t);
    LocalStack params;
    for (Decl* p : cls->templateParams)
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    ParsedFile* file = cls->owner->file;
    TypeContext ctx{cls->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
    for (Type* b : cls->baseTypes)
    {
      const Ty* bt = convertType(b, ctx, depth + 1);
      if (!bt || bt->kind != TyKind::Class || !bt->decl)
        continue;
      Scope* bs = bt->decl->ownScope;
      if (!bs)
      {
        Entry e{bt->decl, nullptr};
        bs = scopeOfEntry(e, 0);
      }
      if (derivesFromInstance(bt, bs, base, depth + 1))
        return true;
    }
    return false;
  }

  // The substitution a member found in scope foundIn sees through class type t: that of the
  // base instantiation the member is declared in (`ItemPrototype::IDType` with `using IDType =
  // IDTypeParameter;` in the base Prototype<ItemID>), else t's own.
  Subst memberSubst(const Ty* t, Scope* foundIn, int depth)
  {
    Scope* cls = t && t->decl ? t->decl->ownScope : nullptr;
    if (cls && foundIn && foundIn != cls && foundIn->kind == ScopeKind::Class)
      if (const Ty* base = instantiationOfBase(t, cls, foundIn, depth + 1))
        return substFor(base);
    return substFor(t);
  }

  // A name written unqualified inside a class and found in the scope of one of its base classes
  // (`const_iterator(*this)` in a member of ParticleSurface : InfiniteRectangle<ParticleChunk*>,
  // the iterator a nested class of the base template): that base's substitution, so the name has
  // its arguments; empty when the name was found elsewhere or the base is no template.
  Subst baseSubstForUnqualified(const Lookup& l, const TypeContext& ctx, int depth)
  {
    if (l.found != Found::Member || !l.foundIn || l.foundIn->kind != ScopeKind::Class || depth > 16)
      return {};
    Scope* cls = ctx.scope;
    while (cls && cls->kind != ScopeKind::Class)
      cls = cls->parent;
    if (!cls || cls == l.foundIn || !cls->owner || cls->baseTypes.empty())
      return {};
    const Ty* self = selfType(cls->owner);
    if (self && ctx.subst)
      self = substitute(self, *ctx.subst);
    if (!self || self->kind != TyKind::Class)
      return {};
    const Ty* base = instantiationOfBase(self, cls, l.foundIn, depth + 1);
    if (!base || base == self || types.isDependent(base))
      return {};
    return substFor(base);
  }

  // For `Vector<int>::iterator`: the substitution of the instantiation the qualifier names
  // (its innermost part with template arguments), or of the base the member was found in
  // (foundIn); empty when there is none.
  Subst qualifierSubst(const QualifiedName& name, const TypeContext& ctx, int depth, Scope* foundIn = nullptr)
  {
    Subst s;
    if (name.parts.size() < 2 || depth > 20)
      return s;
    // The qualifier as a type: `Vector<int>::`, or `VT::` with `using VT = Vector<int>`.
    QualifiedName prefix = name;
    prefix.parts.count -= 1;
    Lookup l = lookupQualified(prefix, ctx.scope, ctx.locals, depth + 1, ctx.subst);
    if (!l.decl)
      return s;
    if (l.decl->kind == NodeKind::TemplateParamDecl && ctx.subst)
    {
      // `T::value_type` where T is a template parameter: the substituted class carries the
      // arguments that resolve the member (`using value_type = T;` inside Vec<int> needs
      // Vec's T mapped to int).
      const Ty* sub = ctx.subst->find(l.decl);
      if (sub && sub->kind == TyKind::Class)
        return memberSubst(sub, foundIn, depth);
      return s;
    }
    if (l.decl->kind != NodeKind::ClassDecl && l.decl->kind != NodeKind::UsingAliasDecl)
      return s;
    const Ty* qualifier = nullptr;
    if (prefix.parts.size() > 1)
    {
      // `PrototypeType::IDType::IndexType`: the qualifier converted as the type it names, so that
      // an alias it ends in has the arguments of the base declaring it (ID<EntityPrototype, uint16>).
      NamedType written{};
      written.kind = NodeKind::NamedType;
      written.name = prefix;
      qualifier = convertType(&written, ctx, depth + 1);
    }
    else
      qualifier = typeOfTypeDecl(l.decl, l, prefix.last(), ctx, depth + 1);
    if (!qualifier || qualifier->kind != TyKind::Class)
      return s;
    return memberSubst(qualifier, foundIn, depth);
  }

  // A written template argument: a type, or a constant expression for a non-type parameter
  // (a Value type of the parameter's declared type), or unknown.
  // A function named as a constant template argument, for a parameter `float (*func)(float,
  // float)`: the function value (TypeTable::functionValue) of the overload whose type the
  // parameter points to; unknown when none fits or the parameter takes no function.
  const Ty* functionArgument(const Lookup& l, Decl* param, int depth)
  {
    auto* p = param && param->kind == NodeKind::TemplateParamDecl ? static_cast<TemplateParamDecl*>(param) : nullptr;
    if (!p || p->isType || !p->valueType)
      return types.unknown();
    const Ty* pt = types.unqualified(convertType(p->valueType, TypeContext{lexicalScopeOf(param), nullptr, nullptr}, depth + 1));
    const Ty* wanted = pt && pt->kind == TyKind::Pointer ? pt->inner : pt;
    if (!wanted || wanted->kind != TyKind::Function)
      return types.unknown();
    std::vector<Decl*> candidates;
    if (l.overloads)
      for (const Entry& e : *l.overloads)
        if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
          candidates.push_back(e.decl);
    if (candidates.empty())
      candidates.push_back(l.decl);
    for (Decl* f : candidates)
      if (declaredTypeOf(f, nullptr, depth + 1) == wanted)
        return types.functionValue(types.pointer(wanted), f);
    return types.unknown();
  }

  const Ty* templateArgument(Node* arg, Decl* param, const TypeContext& ctx, int depth)
  {
    if (!arg)
      return types.unknown();
    if (arg->kind == NodeKind::FunctionType && static_cast<FunctionType*>(arg)->asCall && param && param->kind == NodeKind::TemplateParamDecl &&
        !static_cast<TemplateParamDecl*>(param)->isType)
      arg = static_cast<FunctionType*>(arg)->asCall; // `Frac<gcd(N, M)>`: a call, for a constant parameter
    std::optional<int64_t> v;
    if (arg->kind == NodeKind::NamedType)
    {
      const QualifiedName& an = static_cast<NamedType*>(arg)->name;
      if (an.parts.size() == 1 && an.parts[0].hasTemplateArgs && isBoolTraitName(an.parts[0].symbol))
      {
        // `EnableIf<isIntegral<T>>`: a trait's value, once its arguments are known.
        std::vector<const Ty*> targs = traitArgs(an.parts[0], ctx, depth + 1);
        std::optional<bool> b = targs.empty() ? std::nullopt : traitValue(symbols.name(an.parts[0].symbol), targs);
        return b ? types.valueType(types.boolType(), *b ? 1 : 0) : types.unknown();
      }
      // `Array<char, bufferSize>`: a bare name is a type to the parser; it may be a constant.
      Lookup l = lookupQualified(an, ctx.scope, ctx.locals, depth + 1, ctx.subst);
      if (!l.decl && an.parts.size() == 1)
        if (Decl* r = static_cast<NamedType*>(arg)->resolved; r && r->kind == NodeKind::VariableDecl && r->parent && r->parent->kind == NodeKind::FunctionDecl)
          l.decl = r; // `Flag<sign>` with a local constant, where the context has no locals: what the walk found
      if (l.decl && l.decl->kind == NodeKind::FunctionDecl)
        return functionArgument(l, param, depth); // `BinaryOperation<Add, Functions::add>`
      bool constant = l.decl && ((l.decl->kind == NodeKind::VariableDecl && !(l.decl->flags & DF_Typedef)) || l.decl->kind == NodeKind::EnumeratorDecl ||
                                 (l.decl->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(l.decl)->isType));
      if (constant && l.decl->kind == NodeKind::TemplateParamDecl && ctx.subst)
        if (const Ty* bound = ctx.subst->find(l.decl); bound && bound->kind == TyKind::Value && bound->decl)
          return bound; // a function passed on (`Op<func>` inside a template given one)
      if (constant)
      {
        Subst qs; // `Array<int, Bits<int16_t>::valueBits>`: the member under the instantiation's arguments
        if (an.parts.size() > 1 && l.decl->kind == NodeKind::VariableDecl)
          qs = qualifierSubst(an, ctx, depth + 1, l.foundIn);
        v = constOfDecl(l.decl, ctx, depth, qs.pairs.empty() ? nullptr : &qs);
        if (!v && l.decl->kind == NodeKind::TemplateParamDecl)
          return types.param(l.decl); // `Num<I>` in a template: the argument is still the parameter (dependent)
      }
      else
        return convertType(static_cast<Type*>(arg), ctx, depth + 1);
    }
    else if (uint8_t(arg->kind) <= uint8_t(NodeKind::PackType))
      return convertType(static_cast<Type*>(arg), ctx, depth + 1);
    else if (arg->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(arg)->isKeyword && P(static_cast<UnaryExpr*>(arg)->op) == P::Amp &&
             static_cast<UnaryExpr*>(arg)->operand && static_cast<UnaryExpr*>(arg)->operand->kind == NodeKind::NameExpr)
    {
      // `BinaryOperation<Mul, &Functions::mul>`: the address of a function.
      Lookup l = lookupQualified(static_cast<NameExpr*>(static_cast<UnaryExpr*>(arg)->operand)->name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
      if (l.decl && l.decl->kind == NodeKind::FunctionDecl)
        return functionArgument(l, param, depth);
      v = constEval(static_cast<Expr*>(arg), ctx, depth + 1);
    }
    else
      v = constEval(static_cast<Expr*>(arg), ctx, depth + 1);
    if (!v)
      return types.unknown();
    const Ty* of = types.intType();
    if (param && param->kind == NodeKind::TemplateParamDecl)
      if (auto* p = static_cast<TemplateParamDecl*>(param); !p->isType && p->valueType)
        of = convertType(p->valueType, TypeContext{lexicalScopeOf(param), nullptr, nullptr}, depth + 1);
    return types.valueType(of, *v);
  }

  uint32_t functionSymbol_ = UINT32_MAX;
  uint32_t functionSymbolId()
  {
    if (functionSymbol_ == UINT32_MAX)
      functionSymbol_ = symbols.intern("Function");
    return functionSymbol_;
  }
  uint32_t functionImplSymbol_ = UINT32_MAX;
  uint32_t initializerListSymbol_ = UINT32_MAX;
  uint32_t functionImplSymbolId()
  {
    if (functionImplSymbol_ == UINT32_MAX)
      functionImplSymbol_ = symbols.intern("FunctionImpl");
    return functionImplSymbol_;
  }
  Decl* initializerListDecl()
  {
    if (initializerListSymbol_ == UINT32_MAX)
      initializerListSymbol_ = symbols.intern("InitializerList");
    Lookup l;
    if (lookupInScope(&root, initializerListSymbol_, l, 0) && l.scope && l.scope->owner)
      return l.scope->owner;
    return nullptr;
  }
  bool isInitializerList(const Ty* t)
  {
    return t && t->kind == TyKind::Class && t->decl && t->decl == initializerListDecl();
  }
  uint32_t enumFromStrSymbol_ = UINT32_MAX;
  Decl* enumFromStrDecl()
  {
    if (enumFromStrSymbol_ == UINT32_MAX)
      enumFromStrSymbol_ = symbols.intern("enumFromStr");
    Lookup l;
    if (lookupInScope(&root, enumFromStrSymbol_, l, 0) && l.decl && l.decl->kind == NodeKind::FunctionDecl)
      return l.decl;
    return nullptr;
  }
  uint32_t strongOrderingSymbol_ = UINT32_MAX;
  Decl* strongOrderingDecl()
  {
    if (strongOrderingSymbol_ == UINT32_MAX)
      strongOrderingSymbol_ = symbols.intern("strong_ordering");
    Lookup l;
    if (lookupInScope(&root, strongOrderingSymbol_, l, 0) && l.scope && l.scope->owner)
      return l.scope->owner;
    return nullptr;
  }
  // What `a <=> b` yields: the library's strong_ordering, or int without the library.
  const Ty* orderingType()
  {
    Decl* d = strongOrderingDecl();
    return d ? types.classType(d, {}) : types.intType();
  }
  uint32_t typeInfoSymbol_ = UINT32_MAX;
  Decl* typeInfoDecl()
  {
    if (typeInfoSymbol_ == UINT32_MAX)
      typeInfoSymbol_ = symbols.intern("TypeInfo");
    Lookup l;
    if (lookupInScope(&root, typeInfoSymbol_, l, 0) && l.scope && l.scope->owner)
      return l.scope->owner;
    return nullptr;
  }
  bool isFunctionImpl(const Ty* t)
  {
    if (!t || t->kind != TyKind::Class || !t->decl || t->decl->kind != NodeKind::ClassDecl)
      return false;
    const QualifiedName& n = static_cast<ClassDecl*>(t->decl)->name;
    return !n.empty() && n.last().symbol == functionImplSymbolId();
  }
  // Whether a callable of type `a` takes the arguments of FunctionImpl<R, Args...> `impl`: a
  // lambda or function (pointer) by its parameters; anything else is not checked.
  bool callableWith(const Ty* a, const Ty* impl, int depth)
  {
    a = types.decay(a);
    const Ty* fn = nullptr;
    if (a->kind == TyKind::Lambda || a->kind == TyKind::Function)
      fn = a;
    else if (a->kind == TyKind::Pointer && a->inner->kind == TyKind::Function)
      fn = a->inner;
    else
      return true;
    if (impl->args.empty())
      return true;
    size_t n = impl->args.size() - 1;
    if (fn->args.size() != n)
      return false;
    for (size_t i = 0; i < n; ++i)
      if (conversionRank(impl->args[i + 1], fn->args[i], depth + 1) >= NotViable)
        return false;
    return true;
  }

  // `ScopedSetter guard(x, true)`: a class template named without arguments is typed as its own
  // parameters (what typeOfTypeDecl gives for the primary); the arguments come from a constructor.
  bool isUndeducedClassTemplate(const Ty* t)
  {
    if (!t || t->kind != TyKind::Class || !t->decl)
      return false;
    Span<Decl*> params = t->decl->templateParams;
    if (params.empty() || t->args.size() != params.size())
      return false;
    for (size_t i = 0; i < params.size(); ++i)
      if (t->args[i] != types.param(params[i]))
        return false;
    return true;
  }
  // The class's arguments deduced from a constructor taking these argument types, or null.
  const Ty* deduceClassArgs(const Ty* primary, const std::vector<const Ty*>& argTypes)
  {
    Scope* s = classScopeOf(primary);
    if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
      return nullptr;
    auto* cls = static_cast<ClassDecl*>(s->owner);
    // `Pairish p = Pairish<int, double>(1, 2.5);`, `Duration budget = Milliseconds(5);`: a value of
    // an instantiation of the same template gives its arguments (C++'s copy deduction candidate).
    if (argTypes.size() == 1 && argTypes[0])
      if (const Ty* a = types.unqualified(types.stripRef(argTypes[0])); a && a->kind == TyKind::Class && a->decl == cls && !a->args.empty())
        return a;
    for (Decl* m : cls->members)
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
        continue;
      auto* ctor = static_cast<FunctionDecl*>(m);
      size_t required = 0;
      bool pack = false;
      for (Decl* p : ctor->params)
      {
        auto* pd = static_cast<ParamDecl*>(p);
        if (pd->isPack)
          pack = true;
        else if (!pd->defaultValue)
          ++required;
      }
      if (argTypes.size() < required || (!pack && argTypes.size() > ctor->params.size()))
        continue;
      const Ty* ft = declaredTypeOf(ctor, nullptr, 0);
      if (!ft || ft->kind != TyKind::Function)
        continue;
      Subst d;
      deduceCall(ctor, ft, argTypes, d);
      std::vector<const Ty*> args;
      bool all = true;
      LocalStack own; // the parameters by name, for defaults that name earlier ones
      for (Decl* p : cls->templateParams)
        own.push(static_cast<TemplateParamDecl*>(p)->name, p);
      for (Decl* p : cls->templateParams)
      {
        const Ty* t = d.find(p);
        if (!t && static_cast<TemplateParamDecl*>(p)->defaultArg)
        {
          // `Tuple(mask, gap, id)` of `template<A, B = Nothing, C = Nothing, D = Nothing>`: what the
          // constructor does not deduce takes its default.
          TypeContext dctx{lexicalScopeOf(cls), &own, &d, cls->file ? cls->file->tokens : nullptr, cls->file ? cls->file->text : std::string_view()};
          t = templateArgument(static_cast<TemplateParamDecl*>(p)->defaultArg, p, dctx, 0);
          if (t && t->isKnown())
            d.pairs.push_back({p, t});
          else
            t = nullptr;
        }
        if (!t)
          all = false;
        else
          args.push_back(t);
      }
      if (!all)
        continue;
      bool ok = true;
      for (size_t i = 0; i < argTypes.size() && i < ft->args.size() && ok; ++i)
        if (conversionRank(argTypes[i], substitute(ft->args[i], d)) >= NotViable)
          ok = false;
      if (ok)
        return types.classType(cls, Span<const Ty*>{args.data(), uint32_t(args.size())});
    }
    return nullptr;
  }

  // The arguments of a class template without constructors deduced from a brace list, as C++20
  // does for an aggregate: the data members take the elements in order. A last member that is an
  // array with a parameter bound (`T items[N]`) takes all the remaining elements, N their count, as
  // std::array's deduction guide has it: `Array a = {x, y, z}`, `Pair{name, &Record::field}`.
  const Ty* deduceAggregateArgs(const Ty* primary, const std::vector<const Ty*>& argTypes)
  {
    Scope* s = classScopeOf(primary);
    if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl || argTypes.empty())
      return nullptr;
    auto* cls = static_cast<ClassDecl*>(s->owner);
    if (!cls->bases.empty())
      return nullptr;
    std::vector<VariableDecl*> fields;
    for (Decl* m : cls->members)
    {
      if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
        return nullptr; // not an aggregate: its constructors decide
      if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)))
        fields.push_back(static_cast<VariableDecl*>(m));
    }
    Subst d;
    std::vector<std::pair<const Ty*, const Ty*>> checks; // (argument, the member type it initializes)
    size_t next = 0;
    for (size_t i = 0; i < fields.size() && next < argTypes.size(); ++i)
    {
      const Ty* fieldTy = declaredTypeOf(fields[i], nullptr, 0);
      if (!fieldTy || !fieldTy->isKnown())
        return nullptr;
      Decl* bound = nullptr;
      if (i + 1 == fields.size() && fieldTy->kind == TyKind::Array && fields[i]->type && fields[i]->type->kind == NodeKind::ArrayType)
        if (Expr* size = static_cast<ArrayType*>(fields[i]->type)->size; size && size->kind == NodeKind::NameExpr)
          if (const QualifiedName& n = static_cast<NameExpr*>(size)->name; n.parts.size() == 1)
            for (Decl* p : cls->templateParams)
              if (auto* tp = static_cast<TemplateParamDecl*>(p); !tp->isType && !tp->isPack && tp->name == n.parts[0].symbol)
                bound = p;
      if (!bound)
      {
        deduce(fieldTy, argTypes[next], d);
        checks.push_back({argTypes[next++], fieldTy});
        continue;
      }
      // `T items[N]`: the elements go into the array (brace elision).
      int64_t count = int64_t(argTypes.size() - next);
      for (; next < argTypes.size(); ++next)
      {
        deduce(fieldTy->inner, argTypes[next], d);
        checks.push_back({argTypes[next], fieldTy->inner});
      }
      auto* tp = static_cast<TemplateParamDecl*>(bound);
      const Ty* of = tp->valueType ? convertType(tp->valueType, TypeContext{lexicalScopeOf(bound), nullptr, nullptr}, 1) : types.intType();
      if (!d.find(bound))
        d.pairs.push_back({bound, types.valueType(of, count)});
    }
    if (next < argTypes.size())
      return nullptr; // more elements than members
    std::vector<const Ty*> args;
    for (Decl* p : cls->templateParams)
    {
      const Ty* t = d.find(p);
      if (!t || !t->isKnown() || types.isDependent(t))
        return nullptr;
      args.push_back(t);
    }
    for (const auto& [arg, member] : checks)
      if (conversionRank(arg, substitute(member, d)) >= NotViable)
        return nullptr;
    return types.classType(cls, Span<const Ty*>{args.data(), uint32_t(args.size())});
  }

  // The canonical type a resolved name denotes when used as a type.
  const Ty* typeOfTypeDecl(Decl* d, const Lookup& l, const NamePart& lastPart, const TypeContext& ctx, int depth)
  {
    switch (d->kind)
    {
      case NodeKind::ClassDecl:
      {
        // Use the definition (the scope owner) so that all mentions share one decl.
        Decl* def = l.scope && l.scope->owner && l.scope->kind == ScopeKind::Class ? l.scope->owner : d;
        std::vector<const Ty*> args;
        Span<Decl*> params = effectiveParams(def);
        if (lastPart.templateArgs.size() == 1 && lastPart.symbol == functionSymbolId())
        {
          // The library's `Function<R(Args...)>`: the compiler takes the signature apart, the
          // class behind it is FunctionImpl<R, Args...> (stdlib/core/Function.kpp).
          const Ty* signature = templateArgument(lastPart.templateArgs[0], nullptr, ctx, depth);
          if (signature && signature->kind == TyKind::Function)
          {
            Lookup implLookup;
            if (lookupInScope(&root, symbols.intern("FunctionImpl"), implLookup, 0) && implLookup.scope && implLookup.scope->owner)
            {
              std::vector<const Ty*> implArgs{signature->inner};
              for (const Ty* a : signature->args)
                implArgs.push_back(a);
              return types.classType(implLookup.scope->owner, Span<const Ty*>{implArgs.data(), uint32_t(implArgs.size())});
            }
          }
        }
        for (size_t i = 0; i < lastPart.templateArgs.size(); ++i)
        {
          const Ty* a = templateArgument(lastPart.templateArgs[i], i < params.size() ? params[i] : nullptr, ctx, depth);
          if (a && a->kind == TyKind::Pack)
            for (const Ty* e : a->args)
              args.push_back(e); // `FunctionImpl<R, Args...>` inside the template: the elements
          else
            args.push_back(a);
        }
        if (!args.empty() || (lastPart.hasTemplateArgs && !params.empty()))
        {
          // Parameters not written take their defaults, which may use the earlier arguments
          // (`Less<>`: all of them).
          for (size_t i = args.size(); i < params.size(); ++i)
          {
            auto* p = static_cast<TemplateParamDecl*>(params[i]);
            if (!p->defaultArg)
              break;
            Subst s;
            LocalStack own; // the parameters by name: `= Less<K>` names an earlier one
            for (size_t k = 0; k < i; ++k)
            {
              s.pairs.push_back({params[k], args[k]});
              own.push(static_cast<TemplateParamDecl*>(params[k])->name, params[k]);
            }
            TypeContext dctx{lexicalScopeOf(def), &own, &s, def->file ? def->file->tokens : nullptr, def->file ? def->file->text : std::string_view()};
            args.push_back(templateArgument(p->defaultArg, p, dctx, depth));
          }
        }
        // `Signal<>` of `template<typename... Args>`: written, its pack empty (no arguments at all).
        bool emptyPack = lastPart.hasTemplateArgs && params.size() == 1 && static_cast<TemplateParamDecl*>(params[0])->isPack;
        if (args.empty() && !params.empty() && !emptyPack && ctx.subst)
        {
          // Injected class name inside its own template, or a dependent mention: keep parameters.
          for (Decl* p : params)
            args.push_back(ctx.subst->find(p) ? ctx.subst->find(p) : types.param(p));
        }
        else if (args.empty() && !params.empty() && !emptyPack)
          for (Decl* p : params)
            args.push_back(types.param(p));
        normalizeValueArgs(params, args, def, depth);
        if (!params.empty())
          if (const Ty* spec = matchSpecialization(l, args, depth))
            return spec; // a full specialisation is its own class; a partial one keeps its own parameters' arguments
        return types.classType(def, Span<const Ty*>{args.data(), uint32_t(args.size())});
      }
      case NodeKind::EnumDecl:
      {
        Decl* def = l.scope && l.scope->owner && l.scope->kind == ScopeKind::Enum ? l.scope->owner : d;
        return types.enumType(def);
      }
      case NodeKind::TemplateParamDecl:
        if (ctx.subst)
          if (const Ty* t = ctx.subst->find(d))
            return t;
        return types.param(d);
      case NodeKind::UsingAliasDecl:
      {
        auto* u = static_cast<UsingAliasDecl*>(d);
        if (!u->templateParams.empty())
        {
          // An alias template: its type with the written arguments (and defaults) substituted.
          Span<Decl*> params = u->templateParams;
          Subst s;
          std::vector<const Ty*> args;
          for (size_t i = 0; i < lastPart.templateArgs.size() && i < params.size(); ++i)
          {
            args.push_back(templateArgument(lastPart.templateArgs[i], params[i], ctx, depth));
            s.pairs.push_back({params[i], args.back()});
          }
          LocalStack own; // the alias's parameters, found by name while its defaults and type are converted
          for (Decl* p : params)
            own.push(static_cast<TemplateParamDecl*>(p)->name, p);
          for (size_t i = args.size(); i < params.size(); ++i)
          {
            auto* p = static_cast<TemplateParamDecl*>(params[i]);
            if (!p->defaultArg)
              break;
            TypeContext dctx{lexicalScopeOf(u), &own, &s, u->file ? u->file->tokens : nullptr, u->file ? u->file->text : std::string_view()};
            args.push_back(templateArgument(p->defaultArg, p, dctx, depth)); // `= Less<K>`: K is an earlier parameter, bound in s
            s.pairs.push_back({params[i], args.back()});
          }
          if (ctx.subst)
            for (const Subst::Pair& p : ctx.subst->pairs)
              s.pairs.push_back(p);
          TypeContext actx{lexicalScopeOf(u), &own, &s, u->file ? u->file->tokens : nullptr, u->file ? u->file->text : std::string_view()};
          return convertType(u->type, actx, depth + 1);
        }
        return declaredTypeOf(d, ctx.subst, depth + 1); // under the instantiation's arguments, if any
      }
      case NodeKind::VariableDecl:
        if (d->flags & DF_Typedef)
          return declaredTypeOf(d, ctx.subst, depth + 1);
        return types.unknown();
      case NodeKind::UsingNameDecl:
      {
        Lookup inner = lookupQualified(static_cast<UsingNameDecl*>(d)->name, lexicalScopeOf(d), nullptr, depth + 1);
        if (inner.decl && inner.decl != d)
          return typeOfTypeDecl(inner.decl, inner, static_cast<UsingNameDecl*>(d)->name.last(), TypeContext{lexicalScopeOf(d), nullptr, nullptr}, depth + 1);
        return types.unknown();
      }
      default:
        return types.unknown();
    }
  }

  // AST type -> canonical type.
  const Ty* convertType(Type* t, const TypeContext& ctx, int depth)
  {
    if (!t || depth > 40)
      return types.unknown();
    switch (t->kind)
    {
      case NodeKind::BuiltinType:
      {
        auto* b = static_cast<BuiltinType*>(t);
        switch (b->keyword)
        {
          case Kw::void_: return types.voidType();
          case Kw::bool_: return types.boolType();
          case Kw::char_: return types.charType(8, b->isUnsigned);
          case Kw::char8_t_: return types.charType(8, true);
          case Kw::char16_t_: return types.charType(16, true);
          case Kw::char32_t_: return types.charType(32, true);
          case Kw::wchar_t_: return types.charType(16, true);
          case Kw::float_: return types.floatType(32);
          case Kw::double_: return types.floatType(64);
          case Kw::short_: return types.intType(16, b->isUnsigned);
          default:
            // int, long, long long, unsigned, signed: Windows LLP64.
            return types.intType(b->longCount >= 2 ? 64 : 32, b->isUnsigned);
        }
      }
      case NodeKind::NamedType:
      {
        auto* n = static_cast<NamedType*>(t);
        if (n->name.empty())
          return types.unknown();
        // An alias local to a function body (`using T = VectorType::value_type;`), as the walk
        // found it: also where the context has no locals (the code generator's typeOfAstIn).
        if (n->name.parts.size() == 1 && n->resolved && n->resolved->kind == NodeKind::UsingAliasDecl &&
            n->resolved->parent && n->resolved->parent->kind == NodeKind::FunctionDecl && n->resolved->templateParams.empty())
          return convertType(static_cast<UsingAliasDecl*>(n->resolved)->type, ctx, depth + 1);
        if (n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs)
          if (const Ty* trait = typeTrait(n->name.parts[0], ctx, depth))
            return trait;
        // Under an instantiation the qualifier's arguments are known: `Pick<1, A, B>::Type` with
        // A = int finds the member in the specialisation those arguments pick.
        Lookup l = lookupQualified(n->name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
        if (n->name.parts.size() == 2 && !n->name.parts[1].hasTemplateArgs && n->name.parts[0].hasTemplateArgs)
        {
          // `Num<I>::Type` with I still a parameter: the member of whichever specialisation the
          // arguments pick once they are known; the primary's member must not stand in for it.
          QualifiedName prefix = n->name;
          prefix.parts.count -= 1;
          Lookup pl = lookupQualified(prefix, ctx.scope, ctx.locals, depth + 1, ctx.subst);
          if (pl.decl && pl.decl->kind == NodeKind::ClassDecl)
          {
            const Ty* q = typeOfTypeDecl(pl.decl, pl, prefix.last(), ctx, depth + 1);
            if (q && q->kind == TyKind::Class && types.isDependent(q))
              return types.dependentMember(q, n->name.parts[1].symbol);
          }
        }
        if (l.found == Found::No)
        {
          if (n->name.parts.size() >= 1)
          {
            const NamePart& first = n->name.parts[0];
            if (n->name.parts.size() == 1 && !first.hasTemplateArgs)
              if (const Ty* b = builtinByName(first.symbol))
                return b;
            uint32_t sym = first.symbol;
            if (sym != UINT32_MAX && !templateParamNames.count(sym))
            {
              Scope* target = ctx.scope ? ctx.scope : &root;
              while (target->parent && target->kind != ScopeKind::Namespace)
                target = target->parent;
              // `StrongTypedef<double, class ScaleTag, PositiveDoubleValidator>`: the elaborated
              // type specifier declares the class ScaleTag in the namespace, as C++ has it.
              if (n->elaborated && n->name.parts.size() == 1 && !first.hasTemplateArgs)
                return types.classType(getOrCreateTag(target, sym), {});
              ClassDecl* draft = getOrCreateDraft(target, sym);
              // The arguments live until the type is interned: a span into a vector destroyed
              // earlier read freed memory, which other threads' allocations overwrote (the
              // intermittent crash in isDependent on a garbage argument).
              std::vector<const Ty*> args;
              if (first.hasTemplateArgs)
                for (Node* a : first.templateArgs)
                  args.push_back(templateArgument(a, nullptr, ctx, depth + 1));
              const Ty* base = types.classType(draft, Span<const Ty*>{args.data(), uint32_t(args.size())});
              for (size_t i = 1; i < n->name.parts.size(); ++i)
                base = types.dependentMember(base, n->name.parts[i].symbol);
              return base;
            }
          }
          return types.unknown();
        }
        if (l.found == Found::Dependent && l.decl && l.decl->kind == NodeKind::TemplateParamDecl)
        {
          if (n->name.parts.size() > 1)
          {
            // `T::Unit`, `T::Option::BuilderMapping`: a member of a member of T, each step resolved
            // once the one before is a class (substitute), under the instantiation's arguments
            // right away when T is bound.
            bool plain = true;
            for (size_t i = 1; i < n->name.parts.size(); ++i)
              plain = plain && !n->name.parts[i].hasTemplateArgs && n->name.parts[i].special == 0;
            if (plain)
            {
              const Ty* t = types.param(l.decl);
              for (size_t i = 1; i < n->name.parts.size(); ++i)
                t = types.dependentMember(t, n->name.parts[i].symbol);
              if (const Ty* bound = ctx.subst ? ctx.subst->find(l.decl) : nullptr)
              {
                const Ty* sub = substitute(t, *ctx.subst, depth + 1);
                // A step left unresolved with T known names no member (`T::force_memcpy_save` with
                // T = int*): no type, which is what makes a detection idiom's argument ill-formed.
                if (!sub || (sub->kind == TyKind::DependentMember && !types.isDependent(bound)))
                  return types.unknown();
                return sub;
              }
              return t;
            }
            if (ctx.subst)
              if (const Ty* sub = ctx.subst->find(l.decl))
              {
                if (sub->kind == TyKind::Class && sub->decl && sub->decl->ownScope)
                {
                  // Resolve the rest of the name inside the substituted class.
                  QualifiedName rest = n->name;
                  rest.parts.data += 1;
                  rest.parts.count -= 1;
                  Lookup inner = lookupQualified(rest, sub->decl->ownScope, nullptr, depth + 1);
                  if (inner.decl && inner.found != Found::No)
                  {
                    Subst s2 = memberSubst(sub, inner.foundIn, depth); // a member of a base: that base's arguments
                    TypeContext c2{sub->decl->ownScope, nullptr, &s2};
                    return typeOfTypeDecl(inner.decl, inner, rest.last(), c2, depth + 1);
                  }
                }
                return types.unknown();
              }
            return types.param(l.decl);
          }
          if (n->name.parts[0].hasTemplateArgs && !static_cast<TemplateParamDecl*>(l.decl)->innerParams.empty())
          {
            // `Container<T>` with Container a template template parameter: the parameter applied
            // to the written arguments; under an instantiation that binds Container to a class
            // template, that template with the arguments.
            std::vector<const Ty*> args;
            for (Node* a : n->name.parts[0].templateArgs)
              args.push_back(templateArgument(a, nullptr, ctx, depth + 1));
            Span<const Ty*> span{args.data(), uint32_t(args.size())};
            if (ctx.subst)
              if (const Ty* sub = ctx.subst->find(l.decl); sub && sub->kind == TyKind::Class && sub->decl)
                return resolveSpecialization(types.classType(sub->decl, span), depth + 1);
            return types.param(l.decl, span);
          }
          if (ctx.subst)
            if (const Ty* sub = ctx.subst->find(l.decl))
              return sub;
          return types.param(l.decl);
        }
        if (!l.decl)
          return types.unknown();
        if (n->name.parts.size() > 1 &&
            (l.decl->kind == NodeKind::UsingAliasDecl || (l.decl->kind == NodeKind::VariableDecl && (l.decl->flags & DF_Typedef)) ||
             (l.decl->kind == NodeKind::ClassDecl && l.decl->templateParams.empty() && !effectiveParams(l.decl).empty())))
        {
          // `Vector<int>::iterator`: the alias as the instantiation named by the qualifier has it;
          // `Holder<int>::Node`: the nested class with the enclosing instantiation's arguments;
          // `ItemPrototype::IDType`: the alias as the base it is declared in has it.
          Subst qs = qualifierSubst(n->name, ctx, depth, l.foundIn);
          if (!qs.empty())
          {
            if (ctx.subst)
              for (const Subst::Pair& p : ctx.subst->pairs)
                qs.pairs.push_back(p);
            TypeContext c2{ctx.scope, ctx.locals, &qs, ctx.tokens, ctx.text};
            return typeOfTypeDecl(l.decl, l, n->name.last(), c2, depth);
          }
        }
        if (n->name.parts.size() == 1)
        {
          // `const_iterator it(*this);` in a member of ParticleSurface : InfiniteRectangle<ParticleChunk*>:
          // the base's nested class with the base's arguments.
          Subst bs = baseSubstForUnqualified(l, ctx, depth);
          if (!bs.empty())
          {
            if (ctx.subst)
              for (const Subst::Pair& p : ctx.subst->pairs)
                bs.pairs.push_back(p);
            TypeContext c2{ctx.scope, ctx.locals, &bs, ctx.tokens, ctx.text};
            return typeOfTypeDecl(l.decl, l, n->name.last(), c2, depth);
          }
        }
        return typeOfTypeDecl(l.decl, l, n->name.last(), ctx, depth);
      }
      case NodeKind::PointerType: return types.pointer(convertType(static_cast<PointerType*>(t)->pointee, ctx, depth + 1));
      case NodeKind::LRefType: return types.lref(convertType(static_cast<LRefType*>(t)->referenced, ctx, depth + 1));
      case NodeKind::RRefType: return types.rref(convertType(static_cast<RRefType*>(t)->referenced, ctx, depth + 1));
      case NodeKind::ConstType:
      {
        auto* c = static_cast<ConstType*>(t);
        return types.qualified(convertType(c->inner, ctx, depth + 1), true, c->isVolatile);
      }
      case NodeKind::ArrayType:
      {
        auto* a = static_cast<ArrayType*>(t);
        auto n = constEval(a->size, ctx);
        const Ty* element = convertType(a->element, ctx, depth + 1);
        if (!n && a->size && a->size->kind == NodeKind::NameExpr)
        {
          // `const T (&array)[N]` with N a constant parameter not yet bound: the bound is N.
          Lookup l = lookupQualified(static_cast<NameExpr*>(a->size)->name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
          if (l.decl && l.decl->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(l.decl)->isType &&
              !(ctx.subst && ctx.subst->find(l.decl)))
            return types.dependentArray(element, l.decl);
        }
        return types.array(element, n && *n > 0 ? uint32_t(*n) : 0);
      }
      case NodeKind::FunctionType:
      {
        auto* f = static_cast<FunctionType*>(t);
        std::vector<const Ty*> params;
        for (Decl* p : f->params)
        {
          const Ty* pt = convertType(static_cast<ParamDecl*>(p)->type, ctx, depth + 1);
          if (pt && pt->kind == TyKind::Pack)
            for (const Ty* e : pt->args)
              params.push_back(e); // `R (*)(Args...)` under an instantiation: one parameter per element
          else
            params.push_back(pt);
        }
        return types.function(convertType(f->result, ctx, depth + 1), Span<const Ty*>{params.data(), uint32_t(params.size())}, false);
      }
      case NodeKind::AutoType: return static_cast<AutoType*>(t)->isDecltypeAuto ? types.decltypeAutoType() : types.autoType();
      case NodeKind::DecltypeType:
      {
        auto* d = static_cast<DecltypeType*>(t);
        const Ty* generic = d->expr && d->expr->ty ? d->expr->ty : types.unknown();
        // `-> Future<decltype(callable())>` under an instantiation's arguments: the generic type of
        // a dependent expression (F) stands in for no call's result, so the expression is typed
        // anew with them.
        if ((ctx.function || ctx.tokens) && ctx.subst && !ctx.subst->empty() && d->expr && (!generic->isKnown() || types.isDependent(generic)))
          if (const Ty* typed = typeOfSignatureExpression(d->expr, ctx); typed && typed->isKnown() && !types.isDependent(typed))
            return typed;
        return generic;
      }
      case NodeKind::MemberPointerType:
      {
        auto* m = static_cast<MemberPointerType*>(t);
        const Ty* cls = convertType(m->classType, ctx, depth + 1);
        return types.memberPointer(convertType(m->pointee, ctx, depth + 1), cls);
      }
      case NodeKind::PackType: return convertType(static_cast<PackType*>(t)->pattern, ctx, depth + 1);
      default: return types.unknown();
    }
  }

  // Context in which a declaration's own type is written: its lexical scope plus the template
  // parameters of the declaration and of its enclosing templates.
  struct DeclContext
  {
    LocalStack params;
    Scope* scope;
  };
  void declContextFor(Decl* d, DeclContext& out)
  {
    out.scope = lexicalScopeOf(d);
    for (Decl* p = d; p; p = p->parent)
      if (p->kind == NodeKind::FunctionDecl)
      {
        // `int Box::f(const Terminal t) const` outside its class: the class scope, as C++ has it,
        // for the function and everything declared inside it.
        auto* f = static_cast<FunctionDecl*>(p);
        if (auto it = specializationMembers.find(f); it != specializationMembers.end())
          out.scope = it->second; // `int Flag<true>::f(Inner i)`: the specialisation's scope
        else if (f->name.parts.size() > 1)
        {
          QualifiedName qualifier = f->name;
          qualifier.parts.count -= 1;
          LocalStack params;
          for (Decl* tp : f->templateParams)
            params.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
          Lookup l = lookupQualified(qualifier, lexicalScopeOf(f), &params, 0);
          if (l.scope && l.scope->kind == ScopeKind::Class)
            out.scope = l.scope;
        }
        break;
      }
    std::vector<Decl*> chain;
    for (Decl* p = d; p; p = p->parent)
      chain.push_back(p);
    for (size_t i = chain.size(); i-- > 0;)
      for (Decl* tp : chain[i]->templateParams)
        out.params.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
  }

  // Declared type of a declaration, seen through an optional substitution (for members of
  // template instantiations). Non-dependent results are cached on the declaration.
  const Ty* declaredTypeOf(Decl* d, const Subst* subst, int depth)
  {
    if (!d || depth > 40)
      return types.unknown();
    bool useCache = !subst || subst->empty();
    if (useCache)
      if (const Ty* cached = d->cachedType.load(std::memory_order_acquire))
        return cached;
    const Ty* result = computeDeclaredType(d, subst, depth);
    if (useCache && result->kind != TyKind::Auto)
      d->cachedType.store(result, std::memory_order_release);
    return result;
  }

  const Ty* computeDeclaredType(Decl* d, const Subst* subst, int depth)
  {
    DeclContext dc;
    declContextFor(d, dc);
    TypeContext ctx{dc.scope, &dc.params, subst, d->file ? d->file->tokens : nullptr, d->file ? d->file->text : std::string_view()};
    switch (d->kind)
    {
      case NodeKind::VariableDecl:
      {
        auto* v = static_cast<VariableDecl*>(d);
        if (v->builtin == BUILTIN_EnumValues && v->parent && v->parent->kind == NodeKind::EnumDecl)
          return types.array(types.enumType(v->parent), uint32_t(enumeratorCount(v->parent))); // E::values
        const Ty* t = convertType(v->type, ctx, depth + 1);
        if (t && t->kind == TyKind::Array && t->count == 0 && v->init && v->init->kind == NodeKind::BraceInitExpr)
          t = types.array(t->inner, uint32_t(static_cast<BraceInitExpr*>(v->init)->args.size())); // `double a[] = {1, 2}`
        if (types.isDependent(t) && containsAuto(t))
          return t; // Deduced from the initializer by the walker when it reaches the declaration.
        return t;
      }
      case NodeKind::ParamDecl:
      {
        const Ty* t = convertType(static_cast<ParamDecl*>(d)->type, ctx, depth + 1);
        return t && t->kind == TyKind::Array ? types.decay(t) : t; // `double m[3][3]` is a pointer parameter
      }
      case NodeKind::EnumeratorDecl:
        return d->parent && d->parent->kind == NodeKind::EnumDecl ? types.enumType(d->parent) : types.intType();
      case NodeKind::FunctionDecl:
      {
        auto* f = static_cast<FunctionDecl*>(d);
        const Ty* result;
        if (f->flags & (DF_Constructor | DF_Destructor))
          result = types.voidType();
        else if (f->flags & DF_Conversion)
          result = convertType(f->name.last().convType, ctx, depth + 1);
        else
        {
          TypeContext rctx = ctx;
          rctx.function = f; // a decltype in the return type is typed anew under an instantiation's arguments
          result = convertType(f->trailingReturn ? f->trailingReturn : f->returnType, rctx, depth + 1);
        }
        std::vector<const Ty*> params;
        bool variadic = (f->flags & DF_Variadic) != 0;
        for (Decl* p : f->params)
        {
          auto* pd = static_cast<ParamDecl*>(p);
          const Ty* pt = convertType(pd->type, ctx, depth + 1);
          if (pt && pt->kind == TyKind::Array)
            pt = types.decay(pt); // `f(double m[3][3])` takes a pointer, as in C
          if (pt && !pt->isReference() && pt->kind != TyKind::Pack)
            pt = types.unqualified(pt); // `f(const int x)` is f(int): the const is the definition's business
          if (pd->isPack && spliceIfPack(pt, params))
          {
            // deduced pack (`Args...`, `Args&&...`): one parameter per element
          }
          else
          {
            params.push_back(pt);
            if (pd->isPack)
              variadic = true; // still a pack
          }
        }
        return types.function(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, variadic);
      }
      case NodeKind::ClassDecl:
      case NodeKind::EnumDecl:
        return selfType(d);
      case NodeKind::TemplateParamDecl:
      {
        auto* p = static_cast<TemplateParamDecl*>(d);
        if (subst)
          if (const Ty* t = subst->find(d))
            return p->isType || t->kind != TyKind::Value ? t : t->inner; // a value parameter has its type
        if (p->isType)
          return types.param(d);
        return p->valueType ? convertType(p->valueType, ctx, depth + 1) : types.intType();
      }
      case NodeKind::UsingAliasDecl:
        return convertType(static_cast<UsingAliasDecl*>(d)->type, ctx, depth + 1);
      default:
        return types.unknown();
    }
  }

  bool containsAuto(const Ty* t) const
  {
    for (; t; t = t->inner)
      if (t->kind == TyKind::Auto)
        return true;
    return false;
  }

  // Whether an unknown type or a draft (an unresolved name's placeholder class) is anywhere in t
  // (`Map<?, int>`: an argument a declaration's context could not resolve).
  static bool mentionsUnknown(const Ty* t, int depth = 0)
  {
    if (!t || depth > 16)
      return false;
    if (!t->isKnown() || (t->kind == TyKind::Class && t->decl && (t->decl->attributes & ATTR_Draft)))
      return true;
    if (mentionsUnknown(t->inner, depth + 1))
      return true;
    for (const Ty* a : t->args)
      if (mentionsUnknown(a, depth + 1))
        return true;
    return false;
  }

  // Replaces the Auto placeholder in `pattern` with the type deduced from an initializer.
  const Ty* deduceAuto(const Ty* pattern, const Ty* init)
  {
    if (!pattern || !init || !init->isKnown())
      return types.unknown();
    if (pattern == types.decltypeAutoType())
      return init; // `decltype(auto)`: the expression's decltype, references kept
    if (pattern->kind == TyKind::Auto)
      return pattern->isConst ? types.qualified(types.decay(init), true, false) : types.decay(init);
    auto leaf = [&](const Ty* p) -> const Ty*
    {
      const Ty* base = types.stripRef(init);
      return types.qualified(base, p->isConst, p->isVolatile);
    };
    switch (pattern->kind)
    {
      case TyKind::LRef:
        return pattern->inner->kind == TyKind::Auto ? types.lref(leaf(pattern->inner)) : types.lref(deduceAuto(pattern->inner, init));
      case TyKind::RRef:
        return pattern->inner->kind == TyKind::Auto ? (init->isReference() ? init : types.rref(leaf(pattern->inner)))
                                                    : types.rref(deduceAuto(pattern->inner, init));
      case TyKind::Pointer:
      {
        const Ty* base = types.stripRef(init);
        if (pattern->inner->kind == TyKind::Auto && base->kind == TyKind::Pointer)
          return types.qualified(types.pointer(types.qualified(base->inner, pattern->inner->isConst, false)), pattern->isConst, false);
        if (base->kind == TyKind::Pointer)
          return types.pointer(deduceAuto(pattern->inner, base->inner));
        return types.unknown();
      }
      default:
        return types.unknown();
    }
  }

  // The type a function returning `auto`, `auto&`, `const auto*`... deduces from a return
  // statement's expression type: the written shape around the deduced type.
  const Ty* deducedShape(const Ty* pattern, const Ty* deduced)
  {
    if (pattern && containsAuto(pattern) && deduced && deduced->isKnown())
      return deduceAuto(pattern, deduced);
    return types.decay(deduced);
  }

  // Whether a written type is `auto`, or `auto` under qualifiers, references or pointers.
  static bool astMentionsAuto(const Type* t)
  {
    for (;;)
    {
      if (!t)
        return false;
      switch (t->kind)
      {
        case NodeKind::AutoType: return true;
        case NodeKind::DecltypeType: return static_cast<const DecltypeType*>(t)->isAuto;
        case NodeKind::ConstType: t = static_cast<const ConstType*>(t)->inner; break;
        case NodeKind::PointerType: t = static_cast<const PointerType*>(t)->pointee; break;
        case NodeKind::LRefType: t = static_cast<const LRefType*>(t)->referenced; break;
        case NodeKind::RRefType: t = static_cast<const RRefType*>(t)->referenced; break;
        default: return false;
      }
    }
  }

  // Whether a written type has a `decltype(expr)` anywhere in it (`Future<decltype(callable())>`).
  static bool astMentionsDecltype(Node* n, int depth = 0)
  {
    if (!n || depth > 16 || !isTypeNode(n))
      return false;
    switch (n->kind)
    {
      case NodeKind::DecltypeType: return !static_cast<DecltypeType*>(n)->isAuto;
      case NodeKind::ConstType: return astMentionsDecltype(static_cast<ConstType*>(n)->inner, depth + 1);
      case NodeKind::PointerType: return astMentionsDecltype(static_cast<PointerType*>(n)->pointee, depth + 1);
      case NodeKind::LRefType: return astMentionsDecltype(static_cast<LRefType*>(n)->referenced, depth + 1);
      case NodeKind::RRefType: return astMentionsDecltype(static_cast<RRefType*>(n)->referenced, depth + 1);
      case NodeKind::NamedType:
        for (const NamePart& part : static_cast<NamedType*>(n)->name.parts)
          for (Node* arg : part.templateArgs)
            if (astMentionsDecltype(arg, depth + 1))
              return true;
        return false;
      default: return false;
    }
  }

  // Whether a written type names something its generic conversion cannot keep for the
  // substitution to fill in later: a decltype, a template argument computed from the
  // parameters (`Conditional<(sizeof(A) < sizeof(B)), B, A>::Type`, `Frac<gcd(N, M)>`), or an
  // alias template defined with one (`Wider<A, B>`). A signature written so is converted again
  // with a call's deduced arguments. scope and locals: where t is written.
  bool astNeedsArguments(Node* n, Scope* scope, const LocalStack* locals, int depth = 0)
  {
    if (!n || depth > 12)
      return false;
    switch (n->kind)
    {
      case NodeKind::DecltypeType: return !static_cast<DecltypeType*>(n)->isAuto;
      case NodeKind::ConstType: return astNeedsArguments(static_cast<ConstType*>(n)->inner, scope, locals, depth + 1);
      case NodeKind::PointerType: return astNeedsArguments(static_cast<PointerType*>(n)->pointee, scope, locals, depth + 1);
      case NodeKind::LRefType: return astNeedsArguments(static_cast<LRefType*>(n)->referenced, scope, locals, depth + 1);
      case NodeKind::RRefType: return astNeedsArguments(static_cast<RRefType*>(n)->referenced, scope, locals, depth + 1);
      case NodeKind::NamedType:
      {
        auto* named = static_cast<NamedType*>(n);
        bool hasArgs = false;
        for (const NamePart& part : named->name.parts)
          for (Node* arg : part.templateArgs)
          {
            hasArgs = true;
            if (!arg)
              continue;
            if (isTypeNode(arg))
            {
              if (astNeedsArguments(arg, scope, locals, depth + 1))
                return true;
            }
            else if (arg->kind != NodeKind::LiteralExpr &&
                     !(arg->kind == NodeKind::NameExpr && static_cast<NameExpr*>(arg)->name.parts.size() == 1 &&
                       !static_cast<NameExpr*>(arg)->name.parts[0].hasTemplateArgs))
              return true; // a value computed from the parameters
          }
        if (!hasArgs)
          return false;
        Lookup l = lookupQualified(named->name, scope, locals, depth + 1);
        if (l.decl && l.decl->kind == NodeKind::UsingAliasDecl && !l.decl->templateParams.empty())
        {
          auto* alias = static_cast<UsingAliasDecl*>(l.decl);
          LocalStack own;
          for (Decl* p : alias->templateParams)
            own.push(static_cast<TemplateParamDecl*>(p)->name, p);
          return astNeedsArguments(alias->type, lexicalScopeOf(alias), &own, depth + 1);
        }
        return false;
      }
      default: return false;
    }
  }

  // Does `t` mention one of the given template parameters?
  static bool dependsOn(const Ty* t, Span<Decl*> params)
  {
    if (!t)
      return false;
    switch (t->kind)
    {
      case TyKind::Param:
        for (Decl* p : params)
          if (p == t->decl)
            return true;
        return false;
      case TyKind::Array:
        if (t->decl)
          for (Decl* p : params)
            if (p == t->decl)
              return true; // `T[N]`
        return dependsOn(t->inner, params);
      case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::MemberPointer:
      case TyKind::DependentMember:
        return dependsOn(t->inner, params);
      case TyKind::Function: case TyKind::Class: case TyKind::Lambda:
        if (dependsOn(t->inner, params))
          return true;
        for (const Ty* a : t->args)
          if (dependsOn(a, params))
            return true;
        return false;
      default:
        return false;
    }
  }

  // Structural template argument deduction: binds parameters of `pattern` from `arg`.
  void deduce(const Ty* pattern, const Ty* arg, Subst& out, int depth = 0)
  {
    if (!pattern || !arg || depth > 16 || !arg->isKnown())
      return;
    if (pattern->kind == TyKind::DependentMember)
      return; // `typename T::Unit` is not a deduced context
    if (arg->isBraceList())
    {
      // `{a, b}` deduces only an InitializerList<P> parameter (its P from the elements); for
      // any other parameter, `const T&` included, it is a non-deduced context.
      const Ty* p = types.stripRef(pattern);
      if (p->kind != TyKind::Class || p->decl != arg->decl)
        return;
    }
    if (pattern->kind == TyKind::Param && !pattern->args.empty())
    {
      // `Container<T>` (Container a template template parameter) against `Vector<int>`: the
      // parameter binds to the template itself (its class type without arguments), its written
      // arguments to the instantiation's.
      const Ty* a = types.unqualified(types.stripRef(arg));
      if (a->kind != TyKind::Class || !a->decl)
        return;
      if (!out.find(pattern->decl))
        out.pairs.push_back({pattern->decl, types.classType(a->decl, {})});
      for (size_t i = 0; i < pattern->args.size() && i < a->args.size(); ++i)
        deduce(pattern->args[i], a->args[i], out, depth + 1);
      return;
    }
    if (pattern->kind == TyKind::Param)
    {
      // `T value` takes the decayed argument; inside a pointer or a class (`T*`, `Vector<T>`) T
      // keeps the argument's qualifiers: `const int*` against `T*` makes T `const int`.
      if (!out.find(pattern->decl))
        out.pairs.push_back({pattern->decl, depth == 0 ? types.decay(arg) : types.stripRef(arg)});
      return;
    }
    if (pattern->isReference())
    {
      // `const T&` binds T to the argument without its reference and top-level const; `T&`
      // keeps the argument's const (`forward(const Event&)` deduces T = const Event, as C++
      // does); the forwarding reference `T&&` given an lvalue of type U binds T to U& (which
      // collapses the parameter to U&), so that the callee sees an lvalue and copies rather
      // than moves.
      const Ty* a = types.stripRef(arg);
      if (pattern->inner->kind == TyKind::Param && pattern->inner->args.empty())
      {
        bool forwarding = pattern->kind == TyKind::RRef && !pattern->inner->isConst && depth == 0;
        if (!out.find(pattern->inner->decl))
        {
          const Ty* bound = pattern->inner->isConst ? types.unqualified(a) : a;
          if (forwarding && arg->kind == TyKind::LRef)
            bound = types.lref(a);
          out.pairs.push_back({pattern->inner->decl, bound});
        }
        return;
      }
      deduce(pattern->inner, a, out, depth + 1);
      return;
    }
    const Ty* a = types.stripRef(arg);
    switch (pattern->kind)
    {
      case TyKind::Pointer:
        if (a->kind == TyKind::Pointer)
          deduce(pattern->inner, a->inner, out, depth + 1);
        else if (a->kind == TyKind::Array)
          deduce(pattern->inner, a->inner, out, depth + 1);
        break;
      case TyKind::Array:
        if (a->kind == TyKind::Array || a->kind == TyKind::Pointer)
          deduce(pattern->inner, a->inner, out, depth + 1);
        if (pattern->decl && a->kind == TyKind::Array && a->count > 0 && !out.find(pattern->decl))
        {
          // `const T (&array)[N]` given an int[5]: N = 5, a constant of N's declared type.
          auto* p = static_cast<TemplateParamDecl*>(pattern->decl);
          const Ty* of = p->valueType ? types.unqualified(convertType(p->valueType, TypeContext{lexicalScopeOf(p), nullptr, nullptr}, depth + 1))
                                      : types.sizeType();
          out.pairs.push_back({pattern->decl, types.valueType(of && of->isKnown() ? of : types.sizeType(), int64_t(a->count))});
        }
        break;
      case TyKind::MemberPointer:
        if (a->kind == TyKind::MemberPointer)
        {
          deduce(pattern->inner, a->inner, out, depth + 1);
          if (!pattern->args.empty() && !a->args.empty())
            deduce(pattern->args[0], a->args[0], out, depth + 1);
        }
        break;
      case TyKind::Class:
        if (a->kind == TyKind::Class && a->decl == pattern->decl)
          for (size_t i = 0; i < pattern->args.size() && i < a->args.size(); ++i)
            deduce(pattern->args[i], a->args[i], out, depth + 1);
        else if (a->kind == TyKind::Class && a->decl && pattern->decl && pattern->decl->ownScope && !pattern->args.empty())
        {
          // `const Opt<T>&` given a class derived from Opt<int>: T from that base, as C++ has it.
          if (const Ty* base = instantiationOfBase(a, classScopeOf(a), pattern->decl->ownScope, depth + 1); base && base != a)
            for (size_t i = 0; i < pattern->args.size() && i < base->args.size(); ++i)
              deduce(pattern->args[i], base->args[i], out, depth + 1);
        }
        break;
      case TyKind::Function:
        if (a->kind == TyKind::Function || a->kind == TyKind::Lambda)
        {
          deduce(pattern->inner, a->inner, out, depth + 1);
          for (size_t i = 0; i < pattern->args.size() && i < a->args.size(); ++i)
            deduce(pattern->args[i], a->args[i], out, depth + 1);
        }
        break;
      default:
        break;
    }
  }

  // The template parameter a parameter pack's type is built from (`const Args&...` -> Args).
  static Decl* packParamOf(const Ty* t)
  {
    for (int i = 0; t && i < 8; ++i)
    {
      if (t->kind == TyKind::Param)
        return t->decl && t->decl->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(t->decl)->isPack ? t->decl : nullptr;
      if (t->kind == TyKind::Pointer || t->isReference())
        t = t->inner;
      else
        return nullptr;
    }
    return nullptr;
  }

  // The template parameter packs a type names anywhere (`auto C::*...`: C and the invented one).
  static void packParamsIn(const Ty* t, std::vector<Decl*>& out, int depth = 0)
  {
    if (!t || depth > 16)
      return;
    if (t->kind == TyKind::Param && t->decl && t->decl->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(t->decl)->isPack)
    {
      if (std::find(out.begin(), out.end(), t->decl) == out.end())
        out.push_back(t->decl);
      return;
    }
    packParamsIn(t->inner, out, depth + 1);
    for (const Ty* a : t->args)
      packParamsIn(a, out, depth + 1);
  }

  // Deduces f's template parameters from the argument types of a call, including a trailing
  // parameter pack, which binds to the remaining arguments.
  // The declared type of candidate with its template parameters renamed to those of f: a
  // declaration and its separate definition each have their own, and signatures must compare equal.
  // The template parameters deduction left unbound take their defaults (`typename = EnableIf<...>`);
  // a default that is no type (EnableIf of a false condition) makes the candidate drop out.
  bool fillDefaultTemplateArgs(FunctionDecl* f, Subst& s)
  {
    LocalStack own;
    for (Decl* p : f->templateParams)
      own.push(static_cast<TemplateParamDecl*>(p)->name, p);
    for (Decl* d : f->templateParams)
    {
      auto* p = static_cast<TemplateParamDecl*>(d);
      if (s.find(d) || !p->defaultArg || p->isPack)
        continue;
      TypeContext ctx{lexicalScopeOf(f), &own, &s, f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
      const Ty* t = templateArgument(p->defaultArg, d, ctx, 0);
      if (!t || !t->isKnown())
        return false;
      s.pairs.push_back({d, t});
    }
    // C++: a trailing pack nothing deduces is empty (`assignIf<Index + 1, Tail...>(widget)` with
    // Tail empty names `assignIf<Index + 1>`, whose `template<size_t Index, class Head, class...
    // Tail>` has an empty Tail). One a parameter takes is the call's to deduce.
    if (!f->templateParams.empty())
      if (Decl* last = f->templateParams[f->templateParams.size() - 1]; static_cast<TemplateParamDecl*>(last)->isPack && !s.find(last))
      {
        bool deducible = false;
        if (const Ty* ft = declaredTypeOf(f, nullptr, 0); ft && ft->kind == TyKind::Function)
          for (const Ty* pt : ft->args)
          {
            std::vector<Decl*> packs;
            packParamsIn(pt, packs);
            deducible = deducible || std::find(packs.begin(), packs.end(), last) != packs.end();
          }
        if (!deducible)
          s.pairs.push_back({last, types.pack({})});
      }
    return true;
  }

  // `template<> const char* Button<RecipeID>::tooltip() {...}`: an explicit specialisation of a
  // member of a class template, the body that one instantiation's member has (not a definition
  // of the template's member).
  static bool isMemberSpecialization(const FunctionDecl* f)
  {
    if (!(f->flags & DF_Specialization) || !f->templateParams.empty() || f->name.parts.size() < 2 || f->name.last().hasTemplateArgs)
      return false;
    for (size_t i = 0; i + 1 < f->name.parts.size(); ++i)
      if (f->name.parts[i].hasTemplateArgs)
        return true;
    return false;
  }

  // The class instantiation an explicit member specialisation is written for (`Button<RecipeID>`
  // with its defaults), or null.
  const Ty* specializedClassOf(FunctionDecl* f)
  {
    QualifiedName qualifier = f->name;
    qualifier.parts.count -= 1;
    NamedType written{};
    written.kind = NodeKind::NamedType;
    written.token = f->token;
    written.name = qualifier;
    TypeContext ctx{lexicalScopeOf(f), nullptr, nullptr, f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
    const Ty* t = convertType(&written, ctx, 0);
    return t && t->kind == TyKind::Class && !types.isDependent(t) ? t : nullptr;
  }

  // `int Flag<true>::counter = 100;`, `int Flag<true>::value() {...}`: the scope of the full
  // specialisation the qualifier names, whose own members these define (C++ writes them without
  // `template<>`); null when the qualifier names the template or no full specialisation.
  // locals: the definition's own template parameters (`template<bool B> int Flag<B>::value()`
  // names the template).
  Scope* fullSpecializationScope(const QualifiedName& qualifier, Scope* context, const LocalStack* locals, const ParsedFile* file,
                                 uint32_t token)
  {
    if (qualifier.empty() || !qualifier.last().hasTemplateArgs)
      return nullptr;
    NamedType written{};
    written.kind = NodeKind::NamedType;
    written.token = token;
    written.name = qualifier;
    TokenScope tokenScope(file ? file->tokens : nullptr, file ? file->text : std::string_view());
    TypeContext ctx{context, locals, nullptr, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
    const Ty* t = convertType(&written, ctx, 0);
    if (!t || t->kind != TyKind::Class || !t->decl || !(t->decl->flags & DF_Specialization) || !t->decl->templateParams.empty())
      return nullptr;
    return t->decl->ownScope;
  }

  // Whether explicit member specialisation f is written for class instantiation classTy (the
  // same arguments for the class template's parameters).
  bool memberSpecializationIsFor(FunctionDecl* f, const Ty* classTy)
  {
    TokenScope tokenScope(f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view());
    const Ty* written = specializedClassOf(f);
    if (!written || !classTy || !classTy->decl)
      return false;
    Subst ws = substFor(written), cs = substFor(classTy);
    for (Decl* p : effectiveParams(classTy->decl))
      if (!ws.find(p) || ws.find(p) != cs.find(p))
        return false;
    return true;
  }

  // `template<> ForceID parse<ForceID>(lua_State*, int32_t, const char*)`: an explicit
  // specialisation of a function template. It is no candidate of overload resolution (C++
  // chooses among the templates), only the body a call of its template runs with its arguments.
  static bool isFunctionSpecialization(const FunctionDecl* f)
  {
    return (f->flags & DF_Specialization) && f->isTemplate && f->templateParams.empty() && !isMemberSpecialization(f);
  }

  // The explicit specialisation of function template f written for these arguments of f's
  // parameters (`parse<ForceID>`, or deduced from its parameter types as in `template<> const
  // char* kind(bool)`) whose signature is f's with them; null when there is none.
  FunctionDecl* functionSpecializationFor(FunctionDecl* f, const Subst& args)
  {
    if (f->templateParams.empty() || f->name.empty())
      return nullptr;
    uint32_t symbol = functionSymbol(f->name);
    std::vector<FunctionDecl*> candidates;
    auto consider = [&](Decl* d)
    {
      if (d && d != f && d->kind == NodeKind::FunctionDecl && isFunctionSpecialization(static_cast<FunctionDecl*>(d)) &&
          static_cast<FunctionDecl*>(d)->body && functionSymbol(static_cast<FunctionDecl*>(d)->name) == symbol)
        candidates.push_back(static_cast<FunctionDecl*>(d));
    };
    if (Scope* s = lexicalScopeOf(f))
      if (const std::vector<Entry>* entries = s->find(symbol))
        for (const Entry& e : *entries)
          consider(e.decl);
    if (f->parent && f->parent->kind == NodeKind::ClassDecl && f->parent->ownScope)
      for (FunctionDecl* m : f->parent->ownScope->outOfLineMembers) // `template<> void Box::put<int>(int)`
        consider(m);
    const Ty* primary = declaredTypeOf(f, nullptr, 0);
    if (!primary || primary->kind != TyKind::Function)
      return nullptr;
    for (FunctionDecl* spec : candidates)
    {
      TokenScope tokenScope(spec->file ? spec->file->tokens : nullptr, spec->file ? spec->file->text : std::string_view());
      const Ty* st = declaredTypeOf(spec, nullptr, 0);
      if (!st || st->kind != TyKind::Function)
        continue;
      Subst written;
      TypeContext ctx{lexicalScopeOf(spec), nullptr, nullptr, spec->file ? spec->file->tokens : nullptr, spec->file ? spec->file->text : std::string_view()};
      const NamePart& last = spec->name.last();
      for (size_t i = 0; i < last.templateArgs.size() && i < f->templateParams.size(); ++i)
        written.pairs.push_back({f->templateParams[i], templateArgument(last.templateArgs[i], f->templateParams[i], ctx, 0)});
      deduceCall(f, primary, std::vector<const Ty*>(st->args.begin(), st->args.end()), written);
      bool same = true;
      for (Decl* p : f->templateParams)
        same = same && written.find(p) && written.find(p) == args.find(p);
      if (same && substitute(primary, written) == st)
        return spec;
    }
    return nullptr;
  }

  const Ty* typeAs(FunctionDecl* candidate, FunctionDecl* f)
  {
    const Ty* t = declaredTypeOf(candidate, nullptr, 0);
    if (candidate == f)
      return t;
    // The definition names the class's parameters first (`template<typename T, int N> void
    // Ring<T, N>::push(...)`), then the member's own; the declaration inside the class has only
    // its own, the class's being in scope.
    Span<Decl*> classParams; // a nested class's are the enclosing template's (`Outer<K, V>::Inner::load`)
    if (f->parent && (f->parent->kind == NodeKind::ClassDecl || f->parent->kind == NodeKind::EnumDecl))
      classParams = effectiveParams(f->parent);
    if (candidate->templateParams.size() != classParams.size() + f->templateParams.size())
      return t;
    Subst s;
    for (size_t i = 0; i < classParams.size(); ++i)
      s.pairs.push_back({candidate->templateParams[i], types.param(classParams[i])});
    for (size_t i = 0; i < f->templateParams.size(); ++i)
      s.pairs.push_back({candidate->templateParams[classParams.size() + i], types.param(f->templateParams[i])});
    return s.pairs.empty() ? t : substitute(t, s);
  }

  void deduceCall(FunctionDecl* f, const Ty* fnTy, const std::vector<const Ty*>& argTypes, Subst& out)
  {
    if (!fnTy || fnTy->kind != TyKind::Function)
      return;
    deduceParams(f->params, fnTy->args, argTypes, out);
  }

  // Deduction of a call's template arguments from its arguments: params are the parameter
  // declarations (for packs), patterns their types (a function's, or a generic lambda's).
  void deduceParams(Span<Decl*> params, Span<const Ty*> patterns, const std::vector<const Ty*>& argTypes, Subst& out)
  {
    size_t i = 0;
    for (size_t p = 0; p < params.size() && i < patterns.size(); ++p)
    {
      auto* pd = static_cast<ParamDecl*>(params[p]);
      const Ty* pattern = patterns[i];
      if (pd->isPack)
      {
        if (Decl* packDecl = packParamOf(pattern))
        {
          if (!out.find(packDecl))
          {
            bool forwarding = pattern->kind == TyKind::RRef && pattern->inner->kind == TyKind::Param && !pattern->inner->isConst;
            std::vector<const Ty*> elems;
            for (size_t j = i; j < argTypes.size(); ++j)
            {
              const Ty* at = argTypes[j];
              if (at && at->isBraceList())
                return; // `const Args&...` given `{a, b}`: a non-deduced context, the pack stays unbound
              if (!at)
                elems.push_back(types.unknown());
              else if (forwarding && at->kind == TyKind::LRef)
                elems.push_back(types.lref(types.stripRef(at))); // `emplace_back(name)`: Args = String&
              else
                elems.push_back(types.decay(at));
            }
            out.pairs.push_back({packDecl, types.pack(Span<const Ty*>{elems.data(), uint32_t(elems.size())})});
          }
        }
        else
        {
          // A pattern built around the packs (`auto C::*... members`, `const Vector<Args>&...`):
          // every remaining argument deduces one element of each, the elements explicit
          // arguments gave staying as given.
          std::vector<Decl*> packs;
          packParamsIn(pattern, packs);
          std::vector<std::vector<const Ty*>> elems(packs.size());
          for (size_t j = i; j < argTypes.size() && !packs.empty(); ++j)
          {
            const Ty* at = argTypes[j];
            if (!at || !at->isKnown() || at->isBraceList())
              return;
            Subst one;
            for (Decl* pack : packs)
              if (const Ty* given = out.find(pack); given && given->kind == TyKind::Pack && j - i < given->args.size())
                one.pairs.push_back({pack, given->args[j - i]});
            deduce(pattern, at, one);
            for (size_t k = 0; k < packs.size(); ++k)
            {
              const Ty* e = one.find(packs[k]);
              if (!e)
                return;
              elems[k].push_back(e);
            }
          }
          for (size_t k = 0; k < packs.size(); ++k)
            if (!out.find(packs[k]))
              out.pairs.push_back({packs[k], types.pack(Span<const Ty*>{elems[k].data(), uint32_t(elems[k].size())})});
        }
        return;
      }
      if (i < argTypes.size())
        deduce(pattern, argTypes[i], out);
      ++i;
    }
  }

  // Applies a substitution to a canonical type.
  // An instantiation of a primary class template whose arguments pick a specialisation: that
  // specialisation's type; otherwise the type as given.
  const Ty* resolveSpecialization(const Ty* classTy, int depth)
  {
    Decl* d = classTy ? classTy->decl : nullptr;
    if (!d || d->kind != NodeKind::ClassDecl || d->templateParams.empty() || (d->flags & DF_Specialization) || classTy->args.empty() || depth > 24)
      return classTy;
    const QualifiedName& name = static_cast<ClassDecl*>(d)->name;
    if (name.empty())
      return classTy;
    Lookup l;
    if (!lookupInScope(lexicalScopeOf(d), name.last().symbol, l, depth + 1) || !l.overloads)
      return classTy;
    std::vector<const Ty*> args(classTy->args.begin(), classTy->args.end());
    if (const Ty* spec = matchSpecialization(l, args, depth + 1))
      return spec;
    return classTy;
  }

  // A parameter whose type became a pack (`Args...` -> int, String), also under a reference or a
  // pointer (`Args&&...` -> int&&, String&&): appends one type per element and returns true.
  bool spliceIfPack(const Ty* sub, std::vector<const Ty*>& out)
  {
    if (!sub)
      return false;
    if (sub->kind == TyKind::Pack)
    {
      for (const Ty* e : sub->args)
        out.push_back(e);
      return true;
    }
    if ((sub->kind == TyKind::LRef || sub->kind == TyKind::RRef || sub->kind == TyKind::Pointer) && sub->inner && sub->inner->kind == TyKind::Pack)
    {
      for (const Ty* e : sub->inner->args)
        out.push_back(sub->kind == TyKind::LRef ? types.lref(e) : sub->kind == TyKind::RRef ? types.rref(e)
                                                : types.qualified(types.pointer(e), sub->isConst, sub->isVolatile));
      return true;
    }
    return false;
  }

  const Ty* substitute(const Ty* t, const Subst& s, int depth = 0)
  {
    if (!t || s.empty() || depth > 16)
      return t;
    switch (t->kind)
    {
      case TyKind::Param:
        if (const Ty* r = s.find(t->decl))
        {
          if (!t->args.empty() && r->kind == TyKind::Class && r->decl)
          {
            // `Container<T>` with Container bound to a class template: that template with the arguments.
            std::vector<const Ty*> args;
            for (const Ty* a : t->args)
              args.push_back(substitute(a, s, depth + 1));
            const Ty* applied = resolveSpecialization(types.classType(r->decl, Span<const Ty*>{args.data(), uint32_t(args.size())}), depth + 1);
            return types.qualified(applied, t->isConst, t->isVolatile);
          }
          return types.qualified(r, t->isConst || r->isConst, t->isVolatile || r->isVolatile); // T = const int keeps its const
        }
        if (!t->args.empty())
        {
          std::vector<const Ty*> args;
          for (const Ty* a : t->args)
            args.push_back(substitute(a, s, depth + 1));
          return types.qualified(types.param(t->decl, Span<const Ty*>{args.data(), uint32_t(args.size())}), t->isConst, t->isVolatile);
        }
        return t;
      case TyKind::DependentMember:
      {
        // `T::Unit` with T now a class, `Num<I>::Type` with I now known: the member type inside
        // it (inside the specialisation the arguments pick, when there is one).
        const Ty* sub = substitute(t->inner, s, depth + 1);
        if (sub && sub->kind == TyKind::Class && !types.isDependent(sub))
          sub = resolveSpecialization(sub, depth + 1);
        if (sub && sub->kind == TyKind::Class && sub->decl && !types.isDependent(sub))
          if (Scope* inside = classScopeOf(sub))
          {
            Lookup inner;
            if (lookupInScope(inside, uint32_t(t->value), inner, depth + 1) && inner.decl)
            {
              Subst s2 = memberSubst(sub, inner.foundIn, depth + 1); // a member of a base: that base's arguments
              TypeContext c2{inside, nullptr, &s2};
              NamePart part{};
              part.symbol = uint32_t(t->value);
              const Ty* r = typeOfTypeDecl(inner.decl, inner, part, c2, depth + 1);
              return types.qualified(r, t->isConst || (r && r->isConst), t->isVolatile);
            }
          }
        return sub == t->inner ? t : types.qualified(types.dependentMember(sub, uint32_t(t->value)), t->isConst, t->isVolatile);
      }
      case TyKind::Pointer: return types.qualified(types.pointer(substitute(t->inner, s, depth + 1)), t->isConst, t->isVolatile);
      case TyKind::LRef: return types.lref(substitute(t->inner, s, depth + 1));
      case TyKind::RRef: return types.rref(substitute(t->inner, s, depth + 1));
      case TyKind::Array:
        if (t->decl) // `T[N]`: N's value once bound
        {
          const Ty* bound = s.find(t->decl);
          if (bound && bound->kind == TyKind::Value && !bound->decl && bound->value > 0)
            return types.array(substitute(t->inner, s, depth + 1), uint32_t(bound->value));
          return types.dependentArray(substitute(t->inner, s, depth + 1), t->decl);
        }
        return types.array(substitute(t->inner, s, depth + 1), t->count);
      case TyKind::MemberPointer:
        return types.memberPointer(substitute(t->inner, s, depth + 1), t->args.empty() ? nullptr : substitute(t->args[0], s, depth + 1));
      case TyKind::Member:
        return types.member(substitute(t->inner, s, depth + 1), t->decl, t->args.empty() ? nullptr : substitute(t->args[0], s, depth + 1));
      case TyKind::Function:
      case TyKind::Class:
      case TyKind::Lambda:
      case TyKind::Pack:
      {
        std::vector<const Ty*> args;
        for (const Ty* a : t->args)
        {
          const Ty* sub = substitute(a, s, depth + 1);
          if (t->kind != TyKind::Pack && spliceIfPack(sub, args))
            continue;
          args.push_back(sub);
        }
        Span<const Ty*> span{args.data(), uint32_t(args.size())};
        if (t->kind == TyKind::Pack)
          return types.pack(span);
        if (t->kind == TyKind::Function)
          return types.function(substitute(t->inner, s, depth + 1), span, t->variadic);
        if (t->kind == TyKind::Lambda)
          return types.lambda(substitute(t->inner, s, depth + 1), span, t->decl, t->value);
        return types.qualified(types.classType(t->decl, span), t->isConst, t->isVolatile);
      }
      default:
        return t;
    }
  }

  // ---------------- overload resolution ----------------

  // Is class `base` a (transitive) base of class `derived`? By scope, and for an instantiation also
  // by its arguments: a base they choose (`Const<Color> : Conditional<c, Value<T>, T>`) is not
  // linked to the class template's scope.
  bool isBaseClass(const Ty* base, const Ty* derived)
  {
    if (isBaseScope(classScopeOf(base), classScopeOf(derived)))
    {
      // The scopes link a class template's instances as one: `IsPtr<int> : Constant<bool, false>`
      // derives from no Constant<bool, true>. An instance is a base only as one the bases make
      // (when they can be told).
      const Ty* b = types.unqualified(types.stripRef(base));
      const Ty* d = types.unqualified(types.stripRef(derived));
      if (b->kind != TyKind::Class || b->args.empty() || !b->decl || !b->decl->ownScope || types.isDependent(b) || types.isDependent(d))
        return true;
      if (derivesFromInstance(d, classScopeOf(d), b, 0))
        return true;
      const Ty* actual = instantiationOfBase(d, classScopeOf(d), b->decl->ownScope, 0);
      return !actual || !actual->isKnown() || types.isDependent(actual) || mentionsUnknown(actual);
    }
    derived = types.unqualified(types.stripRef(derived));
    base = types.unqualified(types.stripRef(base));
    if (derived->decl == base->decl)
      return false;
    // An instantiation, or a class whose chain of bases, as the scopes link them (without
    // arguments), loses one (`class UtilitySprite : public UtilitySpriteInstance<Sprite>` with
    // `template<class T> class UtilitySpriteInstance : public T`): the bases as types.
    return (!derived->args.empty() || hasUnlinkedBase(classScopeOf(derived))) && derivesFrom(derived, base);
  }

  // Whether a class's bases, as its scope and its bases' scopes link them (once, without
  // arguments), leave one out: a base its arguments decide, or one outside the program.
  bool hasUnlinkedBase(Scope* cls, int depth = 0)
  {
    if (!cls || depth > 16 || cls->baseTypes.empty())
      return false;
    ensureBases(cls, depth);
    if (cls->baseScopes.size() < cls->baseTypes.size())
      return true;
    for (Scope* b : cls->baseScopes)
      if (hasUnlinkedBase(b, depth + 1))
        return true;
    return false;
  }

  // Is `base` a (transitive) base class scope of `derived`?
  bool isBaseScope(Scope* base, Scope* derived, int depth = 0)
  {
    if (!base || !derived || depth > 16)
      return false;
    ensureBases(derived, depth);
    for (Scope* b : derived->baseScopes)
      if (b == base || isBaseScope(base, b, depth + 1))
        return true;
    return false;
  }

  Scope* classScopeOf(const Ty* t)
  {
    t = types.unqualified(types.stripRef(t));
    if (!t || t->kind != TyKind::Class || !t->decl)
      return nullptr;
    if (t->decl->ownScope)
      return t->decl->ownScope;
    Entry e{t->decl, nullptr};
    return scopeOfEntry(e, 0);
  }

  static constexpr int NotViable = 1000;

  // Rank of converting an argument of type `arg` to a parameter of type `param`: 0 exact,
  // 1 qualification, 2 promotion, 3 standard conversion, 4 user-defined, NotViable.
  int conversionRank(const Ty* arg, const Ty* param, int depth = 0)
  {
    if (!arg || !param || !arg->isKnown() || !param->isKnown() || types.isDependent(arg) || types.isDependent(param))
      return 2; // Unknown types never decide an overload.
    if (depth > 4)
      return NotViable;
    const Ty* a = types.stripRef(arg);
    if (a->isBraceList() && !isInitializerList(types.stripRef(param)) && param->kind == TyKind::LRef)
    {
      // `take({1, makeUnique<int>(5)})`: the brace list makes a temporary, which a `T&&` binds
      // better than a `const T&` (C++'s rvalue-reference rule) and a `T&` not at all.
      return param->inner->isConst ? 3 : NotViable;
    }
    if (isInitializerList(a) && !isInitializerList(types.stripRef(param)))
      return 2; // a brace list: it converts to anything else through its elements, which the code generator checks
    if (a->isBraceList())
    {
      // `{"ab", text}` for an InitializerList<StringView>: as its elements convert (the first
      // one's type stands for them all).
      const Ty* p = types.unqualified(types.stripRef(param));
      if (p->args.empty() || a->args.empty())
        return 2;
      int r = conversionRank(a->args[0], p->args[0], depth + 1);
      return r >= NotViable ? NotViable : r;
    }
    if (param->isReference())
    {
      const Ty* pt = param->inner;
      bool lvalueToRvalueRef = param->kind == TyKind::RRef && arg->kind == TyKind::LRef; // `T&&` takes no lvalue directly
      if (param->kind == TyKind::LRef && !pt->isConst && !arg->isReference())
        return NotViable; // `T&` binds no prvalue (a temporary, a lambda): lvalues present themselves as references (argumentType)
      if (types.unqualified(a) == types.unqualified(pt))
      {
        if (lvalueToRvalueRef || (!pt->isConst && a->isConst))
          return NotViable;
        return pt->isConst && !a->isConst ? 1 : 0;
      }
      if (a->kind == TyKind::Class && pt->kind == TyKind::Class && isBaseClass(pt, a))
        return lvalueToRvalueRef || (!pt->isConst && a->isConst) ? NotViable : 3;
      if (a->kind == TyKind::Array && pt->kind == TyKind::Array && a->count == pt->count && a->inner && pt->inner &&
          types.unqualified(a->inner) == types.unqualified(pt->inner) && (pt->inner->isConst || !a->inner->isConst))
        return lvalueToRvalueRef ? NotViable : 1; // `const int (&)[5]` given an int[5]: the elements made const
      if (param->kind == TyKind::LRef && !pt->isConst)
        return NotViable; // A non-const lvalue reference binds only to the same type.
      // The argument as it is (an lvalue stays one): a converting constructor taking `T&`
      // binds it (`implicit EntityAndIndex(T& entity)` given a Tower lvalue).
      int r = conversionRank(arg, pt, depth + 1);
      if (r >= NotViable)
        return NotViable;
      if (r >= 4 && param->kind == TyKind::LRef)
        return 5; // the converted temporary binds better to `T&&` than to `const T&`
      return std::max(r, 2);
    }
    a = types.decay(a);
    const Ty* p = types.unqualified(param);
    if (a == p)
      return 0;
    auto isInt = [](const Ty* t) { return t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Bool; };
    switch (p->kind)
    {
      case TyKind::Bool:
        if (isInt(a) || a->kind == TyKind::Float || a->kind == TyKind::Pointer || a->kind == TyKind::Nullptr)
          return 3;
        if (a->kind == TyKind::Enum && !static_cast<EnumDecl*>(a->decl)->isClass)
          return 3;
        break;
      case TyKind::Int:
      case TyKind::Char:
        if (isInt(a))
        {
          bool promotion = p->kind == TyKind::Int && p->bits == 32 && !p->isUnsigned &&
                           (a->kind == TyKind::Bool || a->kind == TyKind::Char || a->bits < 32);
          return promotion ? 2 : 3;
        }
        if (a->kind == TyKind::Float)
          return 3;
        if (a->kind == TyKind::Enum)
          return static_cast<EnumDecl*>(a->decl)->isClass ? NotViable : (p->kind == TyKind::Int && p->bits == 32 ? 2 : 3);
        break;
      case TyKind::Float:
        if (a->kind == TyKind::Float)
          return p->bits > a->bits ? 2 : 3;
        if (isInt(a))
          return 3;
        if (a->kind == TyKind::Enum && !static_cast<EnumDecl*>(a->decl)->isClass)
          return 3;
        break;
      case TyKind::Enum:
        if (a->kind == TyKind::Enum && a->decl == p->decl)
          return 0;
        break;
      case TyKind::MemberPointer:
        if (a->kind == TyKind::MemberPointer && types.unqualified(a->inner) == types.unqualified(p->inner) && a->decl == p->decl)
          return 0;
        // `&LuaControlBehavior::luaReadEntity` for an `int32 (T::*)(lua_State*)` with T derived from
        // it: a pointer to a member of a base converts to one of the derived class.
        if (a->kind == TyKind::MemberPointer && types.unqualified(a->inner) == types.unqualified(p->inner) && !a->args.empty() &&
            !p->args.empty() && a->args[0] && p->args[0] && a->args[0]->kind == TyKind::Class && p->args[0]->kind == TyKind::Class &&
            isBaseClass(a->args[0], p->args[0]))
          return 3;
        if (a->kind == TyKind::Nullptr)
          return 3;
        break;
      case TyKind::Pointer:
        if (a->kind == TyKind::Nullptr)
          return 3;
        if (a->kind == TyKind::Pointer)
        {
          const Ty* ai = a->inner;
          const Ty* pi = p->inner;
          if (types.unqualified(ai) == types.unqualified(pi))
            return pi->isConst == ai->isConst ? 0 : (pi->isConst ? 1 : NotViable);
          if (pi->kind == TyKind::Void)
            return 3;
          if (ai->kind == TyKind::Class && pi->kind == TyKind::Class && isBaseClass(pi, ai))
            return !pi->isConst && ai->isConst ? NotViable : 3;
          return NotViable;
        }
        if (a->kind == TyKind::Lambda && p->inner->kind == TyKind::Function)
          return 3;
        break;
      case TyKind::Class:
      {
        if (a->kind == TyKind::Class)
        {
          if (types.unqualified(a)->decl == p->decl && a->args.size() == p->args.size())
          {
            bool same = true;
            for (size_t i = 0; i < a->args.size(); ++i)
              same = same && a->args[i] == p->args[i];
            if (same)
              return 0;
          }
          if (isBaseClass(p, a))
            return 3;
        }
        // A typed union from a value of an alternative's type, references and const removed
        // (design.md 8.5): a user-defined conversion, as through a converting constructor. Two
        // alternatives of that type make it ambiguous, which the code generator reports.
        if (p->decl && p->decl->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(p->decl)->isTypedUnion)
        {
          auto* u = static_cast<ClassDecl*>(p->decl);
          Subst s = substFor(p);
          for (Decl* alt : u->alternatives)
            if (types.unqualified(declaredTypeOf(alt, s.pairs.empty() ? nullptr : &s, 0)) == types.unqualified(a))
              return 4;
          return NotViable; // no other conversion makes a union
        }
        // User-defined: a non-explicit single-argument constructor of p accepting a.
        if (Decl* def = p->decl && p->decl->ownScope ? p->decl : (classScopeOf(p) ? classScopeOf(p)->owner : nullptr))
        {
          Subst s = substFor(p);
          for (Decl* m : static_cast<ClassDecl*>(def)->members)
          {
            if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || isExplicit(static_cast<FunctionDecl*>(m), &s))
              continue;
            auto* f = static_cast<FunctionDecl*>(m);
            if (f->params.size() != 1)
              continue;
            Subst d = s;
            if (!f->templateParams.empty())
            {
              // `template<typename F> Function(F callable)`: F from the argument (as it is: an
              // lvalue for `template<class T> EntityAndIndex(T& entity)`).
              const Ty* ft = declaredTypeOf(f, &s, 0);
              deduceCall(f, ft, std::vector<const Ty*>{arg}, d);
              if (isFunctionImpl(p) && !callableWith(a, p, depth))
                continue; // Function<void(double)> takes no `[]() {}`
            }
            const Ty* pt = declaredTypeOf(f->params[0], &d, 0);
            if (pt && types.stripRef(pt)->kind == TyKind::Class && types.unqualified(types.stripRef(pt)) == types.unqualified(p))
              continue; // The copy constructor is not a conversion (ReverseIterator<const T> from ReverseIterator<T> is).
            if (conversionRank(arg, pt, depth + 1) < 4)
              return 4;
          }
        }
        break;
      }
      default:
        break;
    }
    // Conversion operator of the argument's class, or one it inherits.
    if (a->kind == TyKind::Class && convertsByOperator(a, param, depth))
      return 4;
    return NotViable;
  }

  // The type a class operand of a built-in operator converts to (`area < smallBlueprintAreaSize`
  // with a Const<float>): the result of its one non-explicit conversion operator to a non-class
  // type, its own or inherited (under its arguments); null when it has none or several.
  const Ty* scalarConversionOf(const Ty* cls)
  {
    const Ty* found = nullptr;
    bool ambiguous = false;
    collectScalarConversions(types.unqualified(types.stripRef(cls)), found, ambiguous, 0);
    return ambiguous ? nullptr : found;
  }
  void collectScalarConversions(const Ty* cls, const Ty*& found, bool& ambiguous, int depth)
  {
    if (!cls || cls->kind != TyKind::Class || depth > 8)
      return;
    Scope* s = classScopeOf(cls);
    if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
      return;
    Subst subst = substFor(cls);
    for (Decl* m : static_cast<ClassDecl*>(s->owner)->members)
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion) || isExplicit(static_cast<FunctionDecl*>(m), &subst))
        continue;
      const Ty* ft = declaredTypeOf(m, &subst, 0);
      if (!ft || ft->kind != TyKind::Function || !ft->inner || !ft->inner->isKnown() || types.isDependent(ft->inner))
        continue;
      const Ty* t = types.unqualified(types.stripRef(ft->inner));
      if (t->kind == TyKind::Class || t->kind == TyKind::Void || t->kind == TyKind::Lambda)
        continue;
      if (found && found != t)
        ambiguous = true;
      found = t;
    }
    ClassDecl* def = classDefinition(cls);
    std::vector<const Ty*> bases;
    if (def && directBases(cls, def, bases))
      for (const Ty* b : bases)
        collectScalarConversions(b, found, ambiguous, depth + 1);
  }

  // Whether a non-explicit conversion operator of class a, or of one of its bases (under a's
  // arguments: `Const<float> : Value<float>` with Value's `operator const T&()`), gives a value
  // that converts to param by a standard conversion.
  bool convertsByOperator(const Ty* a, const Ty* param, int depth, int baseDepth = 0, bool allowExplicit = false)
  {
    if (baseDepth > 8)
      return false;
    Scope* s = classScopeOf(a);
    if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
      return false;
    Subst subst = substFor(a);
    for (Decl* m : static_cast<ClassDecl*>(s->owner)->members)
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion) || (!allowExplicit && isExplicit(static_cast<FunctionDecl*>(m), &subst)))
        continue;
      const Ty* ft = declaredTypeOf(m, &subst, 0);
      if (ft && ft->kind == TyKind::Function && conversionRank(ft->inner, param, depth + 1) < 4)
        return true;
    }
    ClassDecl* def = classDefinition(a);
    std::vector<const Ty*> bases;
    if (def && directBases(a, def, bases))
      for (const Ty* b : bases)
        if (b->kind == TyKind::Class && convertsByOperator(b, param, depth, baseDepth + 1, allowExplicit))
          return true;
    return false;
  }

  struct Candidate
  {
    FunctionDecl* f;
    std::vector<int> ranks;
    int thisRank; // 0 exact constness, 1 const method on a non-const object, NotViable if const object/non-const method
    std::vector<bool> refs; // per rank: the parameter is a reference
  };

  // The template parameters of d and of the templates around it, as locals of a context in d.
  static void pushEnclosingTemplateParams(Decl* d, LocalStack& own)
  {
    for (Decl* p = d; p; p = p->parent)
      for (Decl* tp : p->templateParams)
        own.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
  }

  // The value of a requires clause under the deduced arguments: `!`, `&&`, `||` and parentheses
  // over its parts, each a requires-expression (its requirements probed for the arguments), a
  // concept (its definition for them), a boolean trait (`isIntegral<T>`), or any other constant
  // expression: a boolean variable template (`hasBegin<C>`), a comparison of deduced constants
  // (`N1 * D2 <= N2 * D1`). nullopt when it cannot be told (an argument still dependent, an
  // expression the constant evaluation does not know). f is the function (or the concept) whose
  // template parameters s binds.
  std::optional<bool> constraintValue(Expr* e, Decl* f, const Subst& s, int depth = 0)
  {
    if (!e || depth > 16)
      return std::nullopt;
    switch (e->kind)
    {
      case NodeKind::ParenExpr: return constraintValue(static_cast<ParenExpr*>(e)->inner, f, s, depth + 1);
      case NodeKind::UnaryExpr:
      {
        auto* u = static_cast<UnaryExpr*>(e);
        if (u->isKeyword ? Kw(u->op) == Kw::not_ : P(u->op) == P::Bang)
        {
          std::optional<bool> v = constraintValue(u->operand, f, s, depth + 1);
          return v ? std::optional<bool>(!*v) : std::nullopt;
        }
        break;
      }
      case NodeKind::BinaryExpr:
      {
        auto* b = static_cast<BinaryExpr*>(e);
        bool andOp = b->isKeyword ? Kw(b->op) == Kw::and_ : P(b->op) == P::AndAnd;
        bool orOp = b->isKeyword ? Kw(b->op) == Kw::or_ : P(b->op) == P::OrOr;
        if (!andOp && !orOp)
          break;
        std::optional<bool> l = constraintValue(b->lhs, f, s, depth + 1), r = constraintValue(b->rhs, f, s, depth + 1);
        if (andOp)
          return (l && !*l) || (r && !*r) ? std::optional<bool>(false) : (l && r ? std::optional<bool>(true) : std::nullopt);
        return (l && *l) || (r && *r) ? std::optional<bool>(true) : (l && r ? std::optional<bool>(false) : std::nullopt);
      }
      case NodeKind::RequiresExpr:
        // `requires requires (const T& value, PropertyTree& ptree) { value.save(ptree); }`
        return requirementsHold(static_cast<RequiresExpr*>(e), f, s);
      default:
        break;
    }
    LocalStack own;
    pushEnclosingTemplateParams(f, own);
    TypeContext ctx{lexicalScopeOf(f), &own, &s, f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
    if (e->kind == NodeKind::NameExpr)
    {
      auto* n = static_cast<NameExpr*>(e);
      if (n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && isBoolTraitName(n->name.parts[0].symbol))
      {
        std::vector<const Ty*> args = traitArgs(n->name.parts[0], ctx, depth + 1);
        if (args.empty())
          return std::nullopt;
        return traitValue(symbols.name(n->name.parts[0].symbol), args);
      }
      // `has_str<T>` with `template<class T> concept has_str = requires(const T& value) { value.str(); };`.
      if (n->name.last().hasTemplateArgs)
        if (VariableDecl* concept_ = conceptOf(lookupQualified(n->name, ctx.scope, &own, depth + 1).decl))
          return conceptValue(concept_, n->name.last(), ctx, depth + 1);
    }
    std::optional<int64_t> v = constEval(e, ctx, depth + 1);
    return v ? std::optional<bool>(*v != 0) : std::nullopt;
  }

  // The primary of the variable template a lookup found (its declaration without written
  // arguments), or null when the name is no variable template.
  static VariableDecl* primaryVariableTemplate(const Lookup& l)
  {
    auto isPrimary = [](Decl* d) {
      return d && d->kind == NodeKind::VariableDecl && !d->templateParams.empty() && static_cast<VariableDecl*>(d)->init &&
             !static_cast<VariableDecl*>(d)->name.empty() && !static_cast<VariableDecl*>(d)->name.last().hasTemplateArgs;
    };
    if (isPrimary(l.decl))
      return static_cast<VariableDecl*>(l.decl);
    if (l.decl && l.decl->kind == NodeKind::VariableDecl && l.overloads)
      for (const Entry& e : *l.overloads)
        if (isPrimary(e.decl))
          return static_cast<VariableDecl*>(e.decl);
    return nullptr;
  }

  // The value of variable template v for the arguments part writes, read under ctx: the
  // initializer of the specialisation they pick (variableSpecialization), evaluated with them
  // bound; a trailing pack takes the rest, defaults fill the end. nullopt while an argument is
  // dependent or unknown, or two specialisations match equally well.
  std::optional<int64_t> variableTemplateValue(VariableDecl* v, const NamePart& part, const TypeContext& ctx, int depth)
  {
    size_t n = v->templateParams.size();
    if (n == 0 || depth > 16)
      return std::nullopt;
    auto* lastParam = static_cast<TemplateParamDecl*>(v->templateParams[n - 1]);
    std::vector<const Ty*> given, packElements;
    for (Node* written : part.templateArgs)
    {
      size_t i = given.size() + packElements.size();
      Decl* param = v->templateParams[std::min(i, n - 1)];
      const Ty* a = templateArgument(written, param, ctx, depth + 1);
      if (!a || !a->isKnown() || types.isDependent(a))
        return std::nullopt;
      std::vector<const Ty*> elements;
      if (written && written->kind == NodeKind::PackType && a->kind == TyKind::Pack)
        elements.assign(a->args.begin(), a->args.end());
      else
        elements.push_back(a);
      for (const Ty* t : elements)
        (lastParam->isPack && given.size() + 1 >= n ? packElements : given).push_back(t);
    }
    if (given.size() > n || (!lastParam->isPack && !packElements.empty()))
      return std::nullopt;
    Subst bound;
    for (size_t i = 0; i < given.size(); ++i)
      bound.pairs.push_back({v->templateParams[i], given[i]});
    if (lastParam->isPack)
      bound.pairs.push_back({lastParam, types.pack(Span<const Ty*>{packElements.data(), uint32_t(packElements.size())})});
    LocalStack own;
    pushEnclosingTemplateParams(v, own);
    for (size_t i = bound.pairs.size(); i < n; ++i)
    {
      auto* p = static_cast<TemplateParamDecl*>(v->templateParams[i]);
      TypeContext dctx{lexicalScopeOf(v), &own, &bound, v->file ? v->file->tokens : nullptr, v->file ? v->file->text : std::string_view()};
      const Ty* t = p->defaultArg ? templateArgument(p->defaultArg, p, dctx, depth + 1) : nullptr;
      if (!t || !t->isKnown() || types.isDependent(t))
        return std::nullopt; // `isX<A>` naming fewer arguments than the template has, and no default
      bound.pairs.push_back({p, t});
    }
    std::vector<const Ty*> args;
    for (const Subst::Pair& q : bound.pairs)
      args.push_back(q.type);
    Subst specialised;
    VariableDecl* source = lastParam->isPack ? v : variableSpecialization(v, args, specialised);
    if (!source)
      return std::nullopt;
    const Subst& sourceSubst = source == v ? bound : specialised;
    if (!source->init)
      return std::nullopt;
    LocalStack sourceOwn;
    pushEnclosingTemplateParams(source, sourceOwn);
    TypeContext vctx{lexicalScopeOf(source), &sourceOwn, &sourceSubst, source->file ? source->file->tokens : nullptr,
                     source->file ? source->file->text : std::string_view()};
    vctx.variableTemplate = source;
    return constEval(source->init, vctx, depth + 1);
  }

  // The concept d declares (`template<class T> concept C = ...;`, a variable template with no
  // type), or null.
  static VariableDecl* conceptOf(Decl* d)
  {
    auto* v = d && d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : nullptr;
    return v && !v->type && v->init && !v->templateParams.empty() ? v : nullptr;
  }

  // Whether the concept holds for the arguments part writes (`has_str<T>` under ctx): its
  // definition evaluated for them; nullopt when that cannot be told (an argument still
  // dependent, a pack).
  std::optional<bool> conceptValue(VariableDecl* concept_, const NamePart& part, const TypeContext& ctx, int depth)
  {
    if (!concept_ || depth > 16 || part.templateArgs.size() != concept_->templateParams.size())
      return std::nullopt;
    Subst cs;
    for (size_t i = 0; i < concept_->templateParams.size(); ++i)
    {
      auto* param = static_cast<TemplateParamDecl*>(concept_->templateParams[i]);
      const Ty* a = param->isPack ? nullptr : templateArgument(part.templateArgs[i], param, ctx, depth + 1);
      if (!a || !a->isKnown() || types.isDependent(a))
        return std::nullopt;
      cs.pairs.push_back({param, a});
    }
    if (concept_->init->kind == NodeKind::RequiresExpr)
      return requirementsHold(static_cast<RequiresExpr*>(concept_->init), concept_, cs);
    return constraintValue(concept_->init, concept_, cs, depth + 1);
  }

  // Whether every requirement of `requires (params) { ... }` in owner (a concept, a variable
  // template, a constrained function) is well-typed under s, probed on a scratch walker; nullopt
  // while one still depends on a parameter.
  std::optional<bool> requirementsHold(RequiresExpr* r, Decl* owner, const Subst& s)
  {
    // Not for arguments that are not known: the requirements would be probed on unknown types,
    // and `requires (const T::value_type& v) { toPtree(v); }` would ask the same of toPtree again.
    for (const Subst::Pair& q : s.pairs)
      if (mentionsUnknown(q.type))
        return std::nullopt;
    // A constraint whose satisfaction depends on itself (ill-formed in C++), or a chain of
    // probes too deep to follow: not satisfied.
    struct InProgress { RequiresExpr* r; std::vector<Subst::Pair> pairs; };
    static thread_local std::vector<InProgress> inProgress;
    for (const InProgress& p : inProgress)
      if (p.r == r && p.pairs.size() == s.pairs.size() &&
          std::equal(p.pairs.begin(), p.pairs.end(), s.pairs.begin(), [](const Subst::Pair& x, const Subst::Pair& y) { return x.param == y.param && x.type == y.type; }))
        return false;
    if (inProgress.size() >= 16)
      return false;
    inProgress.push_back({r, s.pairs});
    struct Pop { ~Pop() { inProgress.pop_back(); } } pop;
    ParsedFile* file = owner->file;
    if (!file || !file->tokens)
      return std::nullopt;
    ResolveStats scratch;
    Walker probe(*this, scratch, *file->tokens, file->text);
    probe.file = file;
    probe.scope = lexicalScopeOf(owner);
    probe.activeSubst = &s;
    probe.probing = true;
    pushEnclosingTemplateParams(owner, probe.locals);
    for (Decl* p : r->params)
      probe.locals.push(static_cast<ParamDecl*>(p)->name, p);
    bool dependent = false;
    for (Type* t : r->typeRequirements)
    {
      const Ty* ty = probe.typeOf(t);
      if (!ty || !ty->isKnown())
        return false;
      dependent = dependent || types.isDependent(ty);
    }
    for (Expr* requirement : r->requirements)
    {
      const Ty* ty = probe.walkExpr(requirement);
      if (!ty || !ty->isKnown())
        return false;
      dependent = dependent || types.isDependent(ty);
    }
    return dependent ? std::nullopt : std::optional<bool>(true);
  }

  // An expression in a function's signature (`decltype(callable())` in its trailing return type)
  // typed under ctx's substitution, with the function's parameters in scope: a copy of it is
  // walked, so that the template's own nodes keep their generic types. Null when it cannot be.
  const Ty* typeOfSignatureExpression(Expr* e, const TypeContext& ctx)
  {
    static thread_local int nesting = 0;
    FunctionDecl* f = ctx.function;
    const std::vector<Token>* tokens = f && f->file ? f->file->tokens : ctx.tokens; // an alias template's: its own
    std::string_view text = f && f->file ? f->file->text : ctx.text;
    if (!tokens || nesting > 8)
      return nullptr;
    ++nesting;
    InstanceCopier copier(instanceArena());
    Expr* copy = copier.expression(e);
    ResolveStats scratch;
    Walker probe(*this, scratch, *tokens, text);
    probe.file = f ? f->file : nullptr;
    probe.scope = ctx.scope;
    probe.activeSubst = ctx.subst;
    if (ctx.locals)
      probe.locals = *ctx.locals;
    if (f)
      for (Decl* p : f->params)
        probe.locals.push(static_cast<ParamDecl*>(p)->name, p);
    const Ty* t = probe.walkExpr(copy);
    --nesting;
    return t;
  }

  bool requiresHolds(FunctionDecl* f, const Subst& deduced)
  {
    std::optional<bool> v = constraintValue(f->requiresClause, f, deduced);
    return !v || *v;
  }

  // The parameter type of f that a call's argument i binds, for partial ordering: a trailing
  // pack's pattern for each argument it takes (fromPack set); null when there is none, or f has
  // a pack before its last parameter (not ordered here).
  const Ty* orderingParam(FunctionDecl* f, const Ty* ft, size_t i, bool& fromPack)
  {
    fromPack = false;
    size_t n = std::min<size_t>(f->params.size(), ft->args.size());
    for (size_t k = 0; k < n; ++k)
      if (static_cast<ParamDecl*>(f->params[k])->isPack)
      {
        if (k + 1 != f->params.size())
          return nullptr;
        if (i >= k)
        {
          fromPack = true;
          return ft->args[k];
        }
        break;
      }
    return i < n ? ft->args[i] : nullptr;
  }

  // One step of partial ordering: whether parameter type p of one template deduces from the
  // other's type a at the same argument, within s, the deduction for the whole call so far.
  // References and top-level qualifiers are removed and a's template parameters stand in as
  // unique types. A pack's pattern deduces each argument afresh (its packs bind per element);
  // a type from a pack deduces only a pack's pattern.
  bool orderingDeduces(const Ty* p, bool pFromPack, const Ty* a, bool aFromPack, Subst& s)
  {
    if (aFromPack && !pFromPack)
      return false;
    const Ty* pt = types.unqualified(types.stripRef(p));
    const Ty* at = types.unqualified(types.stripRef(a));
    std::vector<Decl*> packs;
    if (pFromPack)
      packParamsIn(pt, packs);
    auto isPack = [&](Decl* d) { return std::find(packs.begin(), packs.end(), d) != packs.end(); };
    Subst trial;
    for (const Subst::Pair& q : s.pairs)
      if (!isPack(q.param))
        trial.pairs.push_back(q);
    deduce(pt, at, trial, 1);
    if (substitute(pt, trial) != at)
      return false;
    s.pairs.clear();
    for (const Subst::Pair& q : trial.pairs)
      if (!isPack(q.param))
        s.pairs.push_back(q);
    return true;
  }

  // Whether type x is more cv-qualified than y.
  static bool moreQualified(const Ty* x, const Ty* y)
  {
    return (x->isConst || !y->isConst) && (x->isVolatile || !y->isVolatile) && (x->isConst != y->isConst || x->isVolatile != y->isVolatile);
  }

  // C++'s partial ordering of function templates ([temp.deduct.partial], [temp.func.order]):
  // whether a is more specialised than b for a call with `count` arguments. a is at least as
  // specialised as b when b's parameter types deduce from a's (orderingDeduces), and more when
  // not the other way round. Two references that deduce both ways prefer the lvalue reference
  // (`reverse(T&)` over `reverse(T&&)`), then the more qualified one (`begin(const C&)` over
  // `begin(C&)`); equal otherwise, a template with no trailing pack beats one whose pack takes
  // no argument (`emplace(it, key)` over `emplace(it, key, values...)`).
  bool moreSpecialized(FunctionDecl* a, FunctionDecl* b, size_t count, const Subst* classSubst)
  {
    const Ty* at = declaredTypeOf(a, classSubst, 0);
    const Ty* bt = declaredTypeOf(b, classSubst, 0);
    if (!at || !bt || at->kind != TyKind::Function || bt->kind != TyKind::Function)
      return false;
    Subst fromA, fromB; // b's parameters deduced from a's types, a's from b's
    bool aAtLeast = true, bAtLeast = true;
    for (size_t i = 0; i < count; ++i)
    {
      bool aPack = false, bPack = false;
      const Ty* ta = orderingParam(a, at, i, aPack);
      const Ty* tb = orderingParam(b, bt, i, bPack);
      if (!ta || !tb)
        return false;
      bool aOk = aAtLeast && orderingDeduces(tb, bPack, ta, aPack, fromA);
      bool bOk = bAtLeast && orderingDeduces(ta, aPack, tb, bPack, fromB);
      if (aOk && bOk && ta->isReference() && tb->isReference())
      {
        if (ta->kind == TyKind::LRef && tb->kind == TyKind::RRef)
          bOk = false;
        else if (tb->kind == TyKind::LRef && ta->kind == TyKind::RRef)
          aOk = false;
        else if (moreQualified(ta->inner, tb->inner))
          bOk = false;
        else if (moreQualified(tb->inner, ta->inner))
          aOk = false;
      }
      aAtLeast = aOk;
      bAtLeast = bOk;
      if (!aAtLeast)
        return false;
    }
    if (aAtLeast && bAtLeast)
    {
      // [temp.func.order]/5: b's trailing pack has no parameter of a's to match (it takes no argument here).
      bool aPack = false, bPack = false;
      bool aHasPack = !a->params.empty() && static_cast<ParamDecl*>(a->params[a->params.size() - 1])->isPack;
      bool bHasPack = !b->params.empty() && static_cast<ParamDecl*>(b->params[b->params.size() - 1])->isPack;
      return !aHasPack && bHasPack && !orderingParam(a, at, count, aPack) && orderingParam(b, bt, count, bPack) && bPack;
    }
    return !bAtLeast;
  }

  // Whether t names one of f's template parameters that deduction binds (not one the first
  // `given` pairs of s, the class's and the explicit arguments, already bound; not under `T::x`).
  bool namesDeducedParam(const Ty* t, FunctionDecl* f, const Subst& s, size_t given, int depth = 0)
  {
    if (!t || depth > 16 || t->kind == TyKind::DependentMember)
      return false;
    if (t->kind == TyKind::Param)
    {
      bool own = std::find(f->templateParams.begin(), f->templateParams.end(), t->decl) != f->templateParams.end();
      for (size_t i = 0; own && i < given && i < s.pairs.size(); ++i)
        own = s.pairs[i].param != t->decl;
      return own;
    }
    if (namesDeducedParam(t->inner, f, s, given, depth + 1))
      return true;
    for (const Ty* a : t->args)
      if (namesDeducedParam(a, f, s, given, depth + 1))
        return true;
    return false;
  }

  // A parameter whose type is an instantiation pattern of a class template that deduction reads
  // (`const GameOptional<B>&`, `Vector<T>*`) accepts as argument that template's instantiation
  // (the deduced one) or a class derived from it, nothing converted to it. written: the declared
  // parameter type, substituted: with the deduced arguments, arg: the argument's type.
  bool deducedClassFits(const Ty* written, const Ty* substituted, const Ty* arg, FunctionDecl* f, const Subst& s, size_t given)
  {
    const Ty* w = types.unqualified(types.stripRef(written));
    const Ty* p = types.unqualified(types.stripRef(substituted));
    const Ty* a = types.unqualified(types.stripRef(arg));
    if (w->kind == TyKind::Pointer && p->kind == TyKind::Pointer)
    {
      if (a->kind != TyKind::Pointer && a->kind != TyKind::Array)
        return true; // conversions to a pointer are ranked elsewhere
      w = types.unqualified(w->inner);
      p = types.unqualified(p->inner);
      a = types.unqualified(a->inner);
    }
    if (w->kind != TyKind::Class || !w->decl || w->args.empty() || !namesDeducedParam(w, f, s, given))
      return true;
    if (a->isBraceList() || a == p)
      return true;
    if (a->kind != TyKind::Class || !a->decl || p->kind != TyKind::Class)
      return false;
    return a->decl == p->decl || isBaseScope(classScopeOf(p), classScopeOf(a));
  }

  // Picks the best candidate by C++'s rule (better in one argument, not worse in any); on a tie
  // the non-template, then the first one. Null when no candidate is viable. bindExplicit, when
  // given, binds the call's explicit template arguments for a candidate (false: the list does not
  // fit it); a template candidate then has to have every parameter bound or deduced
  // (`T load(Args&&...)` is no candidate for `load(x)`, whose T nothing gives).
  FunctionDecl* chooseAmong(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes,
                            const Subst* classSubst, const Ty* objectTy, bool& ambiguous,
                            const std::function<bool(FunctionDecl*, Subst&)>* bindExplicit = nullptr)
  {
    bool argsConcrete = true;
    for (const Ty* at : argTypes)
      argsConcrete = argsConcrete && at && at->isKnown() && !types.isDependent(at);
    std::vector<Candidate> viable;
    bool objectConst = objectTy && types.stripRef(objectTy)->isConst;
    bool single = candidates.size() == 1; // One candidate: arity decides, no ranking needed.
    for (FunctionDecl* f : candidates)
    {
      if (isFunctionSpecialization(f))
        continue; // `template<> ForceID parse<ForceID>(...)`: its template is the candidate
      size_t required = 0, total = f->params.size();
      bool variadic = (f->flags & DF_Variadic) != 0;
      for (Decl* p : f->params)
      {
        auto* param = static_cast<ParamDecl*>(p);
        if (param->isPack)
          variadic = true;
        else if (!param->defaultValue)
          ++required;
      }
      if (argTypes.size() < required || (!variadic && argTypes.size() > total))
        continue;
      Candidate c{f, {}, 0, {}};
      bool constMethod = (f->flags & DF_Const) != 0;
      if (objectTy && !(f->flags & DF_Static) && !single)
      {
        if (objectConst && !constMethod)
          continue;
        c.thisRank = (!objectConst && constMethod) ? 1 : 0;
      }
      const Ty* ft = declaredTypeOf(f, classSubst, 0);
      Subst deduced = classSubst ? *classSubst : Subst{};
      if (bindExplicit && !(*bindExplicit)(f, deduced))
        continue;
      size_t givenCount = deduced.pairs.size(); // the class's and the explicit arguments: not deduced
      if (!f->templateParams.empty() && ft && ft->kind == TyKind::Function)
      {
        deduceCall(f, ft, argTypes, deduced);
        if (!fillDefaultTemplateArgs(f, deduced))
          continue; // a default template argument that is no type: not a candidate
        if (argsConcrete)
        {
          bool complete = true;
          for (Decl* p : f->templateParams)
            complete = complete && (deduced.find(p) || static_cast<TemplateParamDecl*>(p)->isPack);
          if (!complete)
            continue;
        }
      }
      bool ok = true;
      for (size_t i = 0; i < argTypes.size() && ok; ++i)
      {
        const Ty* pt = ft && ft->kind == TyKind::Function && i < ft->args.size() ? ft->args[i] : nullptr;
        if (!pt)
        {
          c.ranks.push_back(3); // variadic tail
          c.refs.push_back(false);
          continue;
        }
        if (!f->templateParams.empty())
        {
          const Ty* written = pt;
          pt = substitute(pt, deduced);
          const Ty* at = argTypes[i];
          if (at && at->isKnown() && !types.isDependent(at) && dependsOn(pt, f->templateParams))
          {
            // Deduction failed (`operator<<(UniquePtr<T>&)` given a Label&): not a candidate.
            ok = false;
            break;
          }
          if (at && at->isKnown() && !types.isDependent(at) && !deducedClassFits(written, pt, at, f, deduced, givenCount))
          {
            // `Direction != Direction` against `operator!=(const B&, const GameOptional<B>&)`: a
            // parameter deduction reads takes its template's instantiations (and classes derived
            // from one) only; C++ converts no argument to make it deducible.
            ok = false;
            break;
          }
        }
        if (pt->kind == TyKind::Pack)
        {
          // A deduced parameter pack: the remaining arguments match its elements one by one.
          for (size_t j = i; j < argTypes.size() && ok; ++j)
          {
            const Ty* et = j - i < pt->args.size() ? pt->args[j - i] : nullptr;
            int r = et ? conversionRank(argTypes[j], et) : 3;
            if (r >= NotViable)
              ok = false;
            c.ranks.push_back(r);
            c.refs.push_back(et && et->isReference());
          }
          break;
        }
        int r = conversionRank(argTypes[i], pt);
        if (r >= NotViable)
          ok = false;
        c.ranks.push_back(r);
        c.refs.push_back(pt->isReference());
      }
      if (ok && f->requiresClause && !requiresHolds(f, deduced))
        ok = false; // `requires isIntegral<T>`: the constraint fails for these arguments
      if (ok)
        viable.push_back(std::move(c));
    }
    ambiguous = false;
    if (viable.empty())
      return nullptr;
    auto better = [&](const Candidate& a, const Candidate& b)
    {
      bool someBetter = false;
      for (size_t i = 0; i < a.ranks.size(); ++i)
      {
        // Exact and const-qualified-reference bindings differ only between two references
        // (`T&` beats `const T&`); against a by-value parameter they are the same match.
        if (a.ranks[i] + b.ranks[i] == 1 && !(a.refs[i] && b.refs[i]))
          continue;
        if (a.ranks[i] > b.ranks[i])
          return false;
        if (a.ranks[i] < b.ranks[i])
          someBetter = true;
      }
      if (a.thisRank > b.thisRank)
        return false;
      if (a.thisRank < b.thisRank)
        someBetter = true;
      if (!someBetter && a.f->templateParams.empty() && !b.f->templateParams.empty())
        return true;
      if (!someBetter && !a.f->templateParams.empty() && !b.f->templateParams.empty() &&
          moreSpecialized(a.f, b.f, argTypes.size(), classSubst))
        return true; // `f(Vector<T>)` over `f(T&)`: C++'s partial ordering of function templates
      return someBetter;
    };
    for (const Candidate& c : viable)
    {
      bool best = true;
      for (const Candidate& o : viable)
        if (&o != &c && !better(c, o))
          best = false;
      if (best)
        return c.f;
    }
    ambiguous = viable.size() > 1;
    // No clear winner: the smallest total rank, then non-template, then declaration order.
    const Candidate* pick = &viable[0];
    auto total = [](const Candidate& c) { int t = c.thisRank; for (int r : c.ranks) t += r; return t; };
    for (const Candidate& c : viable)
      if (total(c) < total(*pick) || (total(c) == total(*pick) && c.f->templateParams.empty() && !pick->f->templateParams.empty()))
        pick = &c;
    return pick->f;
  }

  // Whether the free operator f takes the enumeration operands of `lhs op rhs` as they are, so that
  // it is the user's operator for that enum (`Flags operator|(Flags, Flags)`) rather than one
  // reached through a conversion to some class; f is a non-template function.
  bool takesEnumOperands(FunctionDecl* f, const Ty* lhs, const Ty* rhs)
  {
    if (!f || !f->templateParams.empty())
      return false;
    const Ty* ft = declaredTypeOf(f, nullptr, 0);
    if (!ft || ft->kind != TyKind::Function || ft->args.size() != 2 || !lhs || !rhs)
      return false;
    const Ty* operands[2] = {types.unqualified(types.stripRef(lhs)), types.unqualified(types.stripRef(rhs))};
    for (int i = 0; i < 2; ++i)
      if (operands[i]->kind == TyKind::Enum && types.unqualified(types.stripRef(ft->args[i])) != operands[i])
        return false;
    return true;
  }

  // ---------------- phase 2: walking ----------------

  struct Walker
  {
    Impl& impl;
    TypeTable& types;
    ResolveStats& stats;
    const SymbolTable& symbols;
    const std::vector<Token>& tokens;
    std::string_view text;
    LocalStack locals;
    Scope* scope;            // Current lexical/semantic scope.
    const Ty* thisType = nullptr;
    const Subst* activeSubst = nullptr; // Set while checking a template body for an instantiation.
    int instantiationDepth = 0;
    const ParsedFile* file = nullptr; // The file the tokens belong to (for --why sites).
    Expr* current = nullptr;          // The expression being typed (for --why sites).
    int instantiatedBaseDepth = 0;    // memberOfInstantiatedBases nesting
    Expr* const* walkedArgs = nullptr; // see WalkedArgs
    std::vector<const Ty*> walkedArgTypes;
    const Ty* deducedLocal = nullptr;  // the `auto` type walkDecl just deduced, for walkLocalDecl
    int undecidedBranches = 0;         // inside an instance's `if constexpr` branch this walk could not decide
    // Return statements inside lambdas / auto functions record their type here.
    std::vector<const Ty*> returnFrames;
    // Set on the scratch walkers that probe a requires-expression's requirements: an explicit
    // conversion of a class to a type that is no class (`bool(instance)`) is typed only when
    // the class has an operator for it, since a requirement holds exactly when it is valid.
    bool probing = false;

    // The type of an explicit conversion of operand to t (a cast) while probing: unknown when the
    // operand is a class with no conversion operator to t (explicit ones count), t otherwise.
    const Ty* probedCast(const Ty* t, const Ty* operand)
    {
      if (!probing || !t || !operand || !t->isKnown() || !operand->isKnown() || types.isDependent(t) || types.isDependent(operand))
        return t;
      const Ty* from = types.unqualified(types.stripRef(operand));
      const Ty* to = types.unqualified(types.stripRef(t));
      if (from->kind != TyKind::Class || to->kind == TyKind::Class || to->kind == TyKind::Void || from->isBraceList())
        return t;
      return impl.convertsByOperator(from, to, 0, 0, true) ? t : unknown(Why::Unsupported);
    }

    int forConstexprDepth = 0; // for constexpr bodies this walk is inside (typed with the variable unbound)

    // The elements of a for constexpr list under the current substitution: the types of
    // `types<A, B>`, the constants of `{1, 2}`, the descriptors of `alternatives(U)` and of
    // `members(T)` (a class without bases). False when one cannot be told yet.
    bool forConstexprElements(ForConstexprStmt* f, std::vector<const Ty*>& out)
    {
      auto* v = static_cast<TemplateParamDecl*>(f->var);
      Node* list = f->list;
      if (list && list->kind == NodeKind::NamedType && isTypesList(static_cast<NamedType*>(list)))
      {
        for (Node* arg : static_cast<NamedType*>(list)->name.parts[0].templateArgs)
        {
          const Ty* t = arg && uint8_t(arg->kind) <= uint8_t(NodeKind::PackType) ? typeOf(static_cast<Type*>(arg)) : nullptr;
          if (!t || !t->isKnown() || types.isDependent(t))
            return false;
          out.push_back(t);
        }
        return v->isType;
      }
      if (list && list->kind == NodeKind::BraceInitExpr)
      {
        const Ty* elementTy = v->valueType ? typeOf(v->valueType) : nullptr;
        if (v->isType || !elementTy || !elementTy->isKnown() || types.isDependent(elementTy) || elementTy->kind == TyKind::Auto)
          return false;
        for (Expr* e : static_cast<BraceInitExpr*>(list)->args)
        {
          std::optional<int64_t> c = impl.constEval(e, ctx());
          if (!c)
            return false;
          out.push_back(types.valueType(elementTy, *c));
        }
        return true;
      }
      bool alternatives = list && list->kind == NodeKind::CallExpr && static_cast<CallExpr*>(list)->callee &&
                          static_cast<CallExpr*>(list)->callee->kind == NodeKind::NameExpr &&
                          symbols.name(static_cast<NameExpr*>(static_cast<CallExpr*>(list)->callee)->name.last().symbol) == "alternatives";
      Expr* subject = reflectionSubject(list);
      if (!subject || subject->kind != NodeKind::NameExpr || v->isType)
        return false;
      NamedType named{};
      named.kind = NodeKind::NamedType;
      named.token = subject->token;
      named.name = static_cast<NameExpr*>(subject)->name;
      const Ty* cls = typeOf(&named);
      cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
      if (!cls || cls->kind != TyKind::Class || types.isDependent(cls))
        return false;
      Subst s = cls->args.empty() ? Subst{} : impl.substFor(cls);
      if (alternatives)
      {
        ClassDecl* u = typedUnionOf(cls);
        if (!u)
          return false;
        for (Decl* alt : u->alternatives)
          out.push_back(types.member(impl.declaredTypeOf(alt, s.pairs.empty() ? nullptr : &s, 0), alt, cls));
        return true;
      }
      ClassDecl* def = impl.classDefinition(cls);
      if (!def || !def->bases.empty())
        return false;
      for (Decl* d : def->members)
        if (d->kind == NodeKind::VariableDecl && !(d->flags & (DF_Static | DF_Typedef)) && !static_cast<VariableDecl*>(d)->name.empty())
          out.push_back(types.member(impl.declaredTypeOf(d, s.pairs.empty() ? nullptr : &s, 0), d, cls));
      return true;
    }

    // Per lambda / auto function this walk entered: whether it returns `decltype(auto)`, so that
    // its returns record their expression's decltype (`f(x)` returning int& stays int&).
    std::vector<bool> decltypeReturns;

    // The type `decltype(e)` names, for a `decltype(auto)` return, given e's type t: a variable
    // or member named without parentheses has its declared type, any other lvalue is a reference
    // to its type, a call or cast keeps the reference its type has.
    const Ty* decltypeOf(Expr* e, const Ty* t)
    {
      if (!e || !t || !t->isKnown() || t->isReference() || e->kind == NodeKind::NameExpr || e->kind == NodeKind::MemberExpr)
        return t;
      return isLvalueExpression(e) ? types.lref(t) : t;
    }

    // A typed union (`union enum`, design.md 8.5), given its class type; null for anything else.
    static ClassDecl* typedUnionOf(const Ty* t)
    {
      if (!t || t->kind != TyKind::Class || !t->decl || t->decl->kind != NodeKind::ClassDecl)
        return nullptr;
      auto* c = static_cast<ClassDecl*>(t->decl);
      return c->isTypedUnion && c->isDefinition ? c : nullptr;
    }

    // `types<A, B, C>`: the compile-time list of types of design.md 8.1 (a builtin name).
    bool isTypesList(const NamedType* n) const
    {
      return n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && symbols.name(n->name.parts[0].symbol) == "types";
    }

    // `members(T)` (design.md 8.3), `alternatives(U)` (8.5): the type reflected on, or null when
    // the node is no such list.
    Expr* reflectionSubject(Node* list) const
    {
      if (!list || list->kind != NodeKind::CallExpr)
        return nullptr;
      auto* c = static_cast<CallExpr*>(list);
      if (!c->callee || c->callee->kind != NodeKind::NameExpr || c->args.size() != 1)
        return nullptr;
      const QualifiedName& name = static_cast<NameExpr*>(c->callee)->name;
      if (name.parts.size() != 1 || name.parts[0].hasTemplateArgs)
        return nullptr;
      std::string_view which = symbols.name(name.parts[0].symbol);
      return which == "members" || which == "alternatives" ? c->args[0] : nullptr;
    }

    // The typed union whose alternative a descriptor names (`for constexpr (auto a :
    // alternatives(U))`), with the alternative's index; null for a member descriptor.
    static ClassDecl* alternativeOf(const Ty* descriptor, size_t& index)
    {
      if (!descriptor || descriptor->kind != TyKind::Member || descriptor->args.empty())
        return nullptr;
      ClassDecl* u = typedUnionOf(descriptor->args[0]);
      for (size_t i = 0; u && i < u->alternatives.size(); ++i)
        if (u->alternatives[i] == descriptor->decl)
        {
          index = i;
          return u;
        }
      return nullptr;
    }

    TokenScope tokenScope; // this walker's file is where literals are read from, lookups included
    Walker(Impl& impl, ResolveStats& stats, const std::vector<Token>& tokens, std::string_view text)
      : impl(impl), types(impl.types), stats(stats), symbols(impl.symbols), tokens(tokens), text(text), scope(&impl.root), tokenScope(&tokens, text)
    {
    }

    std::string_view spelling(uint32_t token) const
    {
      const Token& t = tokens[std::min<size_t>(token, tokens.size() - 1)];
      return text.substr(t.offset, t.length);
    }

    // --- statistics helpers ---

    void countUnresolved(const QualifiedName& name, uint32_t token)
    {
      uint32_t first = name.parts[0].symbol;
      if (first == impl.symStd && name.parts.size() > 1)
      {
        ++stats.qualifiedUnresolvedStd;
        return;
      }
      uint32_t key = name.last().symbol;
      if (key == UINT32_MAX)
        return;
      if (name.parts.size() == 1 && isAllCaps(symbols.name(key)))
      {
        ++stats.unresolvedMacroLike;
        return;
      }
      ++stats.unresolvedSymbols[key];
      stats.unresolvedFirst.emplace(key, ResolveStats::Where{stats.fileIndex, token});
    }

    enum class Why : uint8_t { None, Name, Base, Member, Overload, Deduction, AutoReturn, Unsupported, Operator, Auto };

    const Ty* unknown(Why why)
    {
      if (stats.collectWhy && current && file)
      {
        uint32_t symbol = UINT32_MAX;
        switch (current->kind)
        {
          case NodeKind::NameExpr:
            if (!static_cast<NameExpr*>(current)->name.empty())
              symbol = static_cast<NameExpr*>(current)->name.last().symbol;
            break;
          case NodeKind::MemberExpr:
            if (!static_cast<MemberExpr*>(current)->member.empty())
              symbol = static_cast<MemberExpr*>(current)->member.last().symbol;
            break;
          case NodeKind::CallExpr:
            if (Expr* callee = static_cast<CallExpr*>(current)->callee)
            {
              if (callee->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(callee)->name.empty())
                symbol = static_cast<NameExpr*>(callee)->name.last().symbol;
              else if (callee->kind == NodeKind::MemberExpr && !static_cast<MemberExpr*>(callee)->member.empty())
                symbol = static_cast<MemberExpr*>(callee)->member.last().symbol;
            }
            break;
          case NodeKind::BinaryExpr:
            if (!static_cast<BinaryExpr*>(current)->isKeyword && static_cast<BinaryExpr*>(current)->op < uint32_t(P::Count))
              symbol = impl.opSymbols[static_cast<BinaryExpr*>(current)->op];
            break;
          case NodeKind::UnaryExpr:
            if (!static_cast<UnaryExpr*>(current)->isKeyword && static_cast<UnaryExpr*>(current)->op < uint32_t(P::Count))
              symbol = impl.opSymbols[static_cast<UnaryExpr*>(current)->op];
            break;
          default:
            break;
        }
        uint64_t key = uint64_t(why) << 56 | uint64_t(activeSubst ? 1 : 0) << 55 | uint64_t(uint8_t(current->kind)) << 32 | symbol;
        auto [it, inserted] = stats.why.emplace(key, ResolveStats::WhySite{file, current->token, 0});
        ++it->second.count;
      }
      switch (why)
      {
        case Why::Name: ++stats.unknownName; break;
        case Why::Base: ++stats.unknownBase; break;
        case Why::Member: ++stats.unknownMember; break;
        case Why::Overload: ++stats.unknownOverload; break;
        case Why::Deduction: ++stats.unknownDeduction; break;
        case Why::AutoReturn: ++stats.unknownAutoReturn; break;
        case Why::Unsupported: ++stats.unknownUnsupported; break;
        case Why::Operator: ++stats.unknownOperator; break;
        case Why::Auto: ++stats.unknownAuto; break;
        default: break;
      }
      return types.unknown();
    }

    // Records the type of an expression node and counts it.
    const Ty* record(Expr* e, const Ty* t)
    {
      if (!t)
        t = types.unknown();
      e->ty = t;
      if (!t->isKnown())
        ++stats.exprsUnknown;
      else if (types.isDependent(t))
        ++stats.exprsDependent;
      else
        ++stats.exprsTyped;
      if (!impl.whyAt.empty() && file)
        traceAt(e, t);
      return t;
    }

    // --why-at FILE:LINE,...: every expression typed on those lines, with its type (innermost first).
    void traceAt(Expr* e, const Ty* t)
    {
      const std::string& path = file->path;
      auto inFile = [&](const std::string& want) { return path.size() >= want.size() && path.compare(path.size() - want.size(), want.size(), want) == 0; };
      bool anyInFile = false;
      for (const auto& [want, wantLine] : impl.whyAt)
        anyInFile = anyInFile || inFile(want);
      if (!anyInFile)
        return;
      const Token& tk = tokens[std::min<size_t>(e->token, tokens.size() - 1)];
      uint32_t line = 1 + uint32_t(std::count(text.begin(), text.begin() + std::min<size_t>(tk.offset, text.size()), '\n'));
      bool wanted = false;
      for (const auto& [want, wantLine] : impl.whyAt)
        wanted = wanted || (wantLine == line && inFile(want));
      if (!wanted)
        return;
      // An operator's or a destructor's name part has no symbol of its own (`operator-(a, b)`).
      auto partName = [&](const NamePart& part)
      {
        if (part.special == 2 && part.opToken < uint32_t(P::Count))
          return std::string(symbols.name(impl.opSymbols[part.opToken]));
        if (part.special == 3)
          return std::string("operator <conversion>");
        std::string name = part.symbol == UINT32_MAX ? "?" : std::string(symbols.name(part.symbol));
        return part.special == 1 ? "~" + name : name;
      };
      std::string what = nodeKindName(e->kind);
      if (e->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(e)->name.empty())
        what += " " + partName(static_cast<NameExpr*>(e)->name.last());
      else if (e->kind == NodeKind::MemberExpr && !static_cast<MemberExpr*>(e)->member.empty())
        what += " ." + partName(static_cast<MemberExpr*>(e)->member.last());
      std::string inst;
      if (activeSubst)
        for (const Subst::Pair& p : activeSubst->pairs)
          inst += (inst.empty() ? " [" : ", ") + types.toString(p.type, &symbols);
      fprintf(stderr, "why-at %s:%u %s at '%.*s' -> %s%s%s\n", path.c_str(), line, what.c_str(), int(std::min<uint32_t>(tk.length, 40)),
              text.data() + tk.offset, types.toString(t, &symbols).c_str(), inst.c_str(), inst.empty() ? "" : "]");
    }

    TypeContext ctx() const { return TypeContext{scope, &locals, activeSubst, &tokens, text}; }

    // --- template instantiation: check bodies once per (template, arguments) ---

    static bool argsConcrete(TypeTable& types, const Ty* t)
    {
      if (!t || t->kind != TyKind::Class || t->args.empty())
        return false;
      for (const Ty* a : t->args)
        if (!a->isKnown() || types.isDependent(a))
          return false;
      return true;
    }

    // Runs `body` as a sub-check with the given substitution, scope and this-type, in the
    // context of the template's own file. Expression statistics are merged into ours.
    template<typename F>
    void subCheck(Decl* templ, const Subst& subst, Scope* bodyScope, const Ty* bodyThis, F&& body)
    {
      ParsedFile* file = templ->file;
      if (!file || !file->tokens)
        return;
      ResolveStats local;
      local.fileIndex = stats.fileIndex;
      local.collectWhy = stats.collectWhy;
      Walker sub(impl, local, *file->tokens, file->text);
      sub.file = file;
      sub.scope = bodyScope;
      sub.thisType = bodyThis;
      sub.activeSubst = &subst;
      sub.instantiationDepth = instantiationDepth + 1;
      for (Decl* p : templ->templateParams)
        sub.locals.push(static_cast<TemplateParamDecl*>(p)->name, p);
      body(sub);
      local.instExprsTyped += local.exprsTyped;
      local.instExprsDependent += local.exprsDependent;
      local.instExprsUnknown += local.exprsUnknown;
      stats.add(local);
    }

    // Types the instantiation of member f for the class's arguments, in a copy of f's nodes kept
    // as that instantiation's body (unless it exists already: an `auto` member asked for its
    // return type first).
    void checkMemberBody(Walker& sub, FunctionDecl* f, const Subst& classSubst)
    {
      // Out-of-line members carry their own template parameter list; map it positionally.
      Subst merged = classSubst;
      Decl* cls = sub.scope->owner;
      if (cls && f->templateParams.size() == cls->templateParams.size() && f->parent != cls)
        for (size_t i = 0; i < f->templateParams.size(); ++i)
          if (const Ty* t = classSubst.find(cls->templateParams[i]))
            merged.pairs.push_back({f->templateParams[i], t});
      bool producer;
      Impl::InstanceRecord* inst = impl.claimInstance(f, merged, sub.thisType, producer);
      if (!producer)
        return;
      FunctionDecl* copy = impl.copyInstance(f, sub.scope);
      inst->body = copy;
      sub.activeSubst = &merged;
      size_t mark = sub.locals.mark();
      if (f->parent != cls)
        for (Decl* p : f->templateParams)
          sub.locals.push(static_cast<TemplateParamDecl*>(p)->name, p);
      sub.returnFrames.push_back(nullptr);
      sub.walkDecl(copy);
      const Ty* deduced = sub.returnFrames.back();
      sub.returnFrames.pop_back();
      if (deduced)
        inst->deducedReturn = impl.deducedShape(copy->returnType && !copy->trailingReturn ? sub.typeOf(copy->returnType) : nullptr, deduced);
      sub.locals.pop(mark);
      sub.activeSubst = &classSubst;
      ++impl.instBodies;
      inst->state.store(2, std::memory_order_release);
    }

    // Checks every member function body of a class template for one instantiation, once.
    void instantiateClass(const Ty* classTy)
    {
      if (instantiationDepth > 16 || !argsConcrete(types, classTy) || !classTy->decl || !classTy->decl->ownScope)
        return;
      if (!classTy->decl->isTemplate || classTy->decl->templateParams.empty())
        return;
      bool producer;
      Impl::Instantiation* inst = impl.claimClass(classTy, producer);
      if (!producer)
        return; // Done or being done by someone else; nothing here needs its result.
      auto start = std::chrono::steady_clock::now();
      Subst subst = impl.substFor(classTy);
      Scope* cls = classTy->decl->ownScope;
      subCheck(classTy->decl, subst, cls, types.pointer(classTy), [&](Walker& sub)
      {
        auto* c = static_cast<ClassDecl*>(classTy->decl);
        for (Decl* m : c->members)
          if (m->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(m)->body && m->templateParams.empty())
            checkMemberBody(sub, static_cast<FunctionDecl*>(m), subst);
        for (FunctionDecl* f : cls->outOfLineMembers)
          if (f->body && (!Impl::isMemberSpecialization(f) || impl.memberSpecializationIsFor(f, classTy)))
            checkMemberBody(sub, f, subst); // `template<> R Button<RecipeID>::f()`: for its instantiation only
      });
      impl.instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      inst->state.store(2, std::memory_order_release);
    }

    // Checks a function template body for one set of deduced arguments, once. Returns the
    // deduced return type when the function returns `auto` (waiting for another producer).
    const Ty* instantiateFunction(FunctionDecl* f, const Subst& subst, bool needReturn, const Subst* classSubst = nullptr,
                                  const Ty* objectType = nullptr)
    {
      for (Decl* p : f->templateParams)
        if (!subst.find(p) || types.isDependent(subst.find(p)) || !subst.find(p)->isKnown())
          return nullptr;
      if (!f->body || instantiationDepth > 16)
        return nullptr;
      if (classSubst)
        for (const Subst::Pair& pair : classSubst->pairs)
          if (!pair.type->isKnown() || types.isDependent(pair.type))
            return nullptr;
      Subst full = subst;
      if (classSubst)
        for (const Subst::Pair& pair : classSubst->pairs)
          if (!full.find(pair.param))
            full.pairs.push_back(pair);
      Scope* bodyScope = f->name.parts.size() > 1 ? semanticScopeFor(f->name) : impl.lexicalScopeOf(f);
      const Ty* bodyThis = nullptr;
      if (objectType)
        bodyThis = types.pointer(types.unqualified(types.stripRef(objectType)));
      else if (Decl* cls = enclosingClassDecl(bodyScope))
        bodyThis = types.pointer(impl.substitute(impl.selfType(cls), full));
      ++impl.instRequests;
      bool producer;
      Impl::InstanceRecord* inst = impl.claimInstance(f, full, bodyThis, producer);
      if (!producer)
      {
        if (!needReturn)
          return nullptr;
        if (inst->state.load(std::memory_order_acquire) != 2)
        {
          if (inst->owner == std::this_thread::get_id())
            return nullptr; // Recursive auto function.
          ++impl.instWaits;
          while (inst->state.load(std::memory_order_acquire) != 2)
            std::this_thread::yield();
        }
        return inst->deducedReturn;
      }
      ++impl.instFunctions;
      auto start = std::chrono::steady_clock::now();
      // The instantiation's own copy of f's nodes, typed here and kept as its body.
      FunctionDecl* copy = impl.copyInstance(f, bodyScope);
      inst->body = copy;
      const Ty* deduced = nullptr;
      const Ty* pattern = nullptr; // the written `auto&`, `const auto*`...: the deduced type takes its shape
      subCheck(f, full, bodyScope, bodyThis, [&](Walker& sub)
      {
        sub.returnFrames.push_back(nullptr);
        sub.walkDecl(copy);
        deduced = sub.returnFrames.back();
        sub.returnFrames.pop_back();
        if (copy->returnType && !copy->trailingReturn)
          pattern = sub.typeOf(copy->returnType);
        ++impl.instBodies;
      });
      inst->deducedReturn = deduced ? impl.deducedShape(pattern, deduced) : nullptr;
      impl.instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      inst->state.store(2, std::memory_order_release);
      return inst->deducedReturn;
    }

    // --- names ---

    void resolveTypeName(NamedType* t)
    {
      if (t->name.empty())
        return;
      Lookup l = impl.lookupQualified(t->name, scope, &locals, 0, activeSubst);
      t->resolved = l.decl;
      if (l.found == Found::No)
      {
        if (t->name.parts.size() == 1 && impl.builtinByName(t->name.parts[0].symbol))
          ++stats.typesBuiltinByName;
        else if (t->name.parts.size() == 1 && t->name.parts[0].hasTemplateArgs && impl.isTypeTraitName(t->name.parts[0].symbol))
          ++stats.typesResolved; // RemoveConst<T> and friends
        else
        {
          ++stats.typesUnresolved;
          countUnresolved(t->name, t->token);
        }
      }
      else if (l.found == Found::Dependent)
        ++stats.typesDependent;
      else
        ++stats.typesResolved;
      for (const NamePart& part : t->name.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
    }

    // Converts a type written in the current context.
    const Ty* typeOf(Type* t) { return impl.convertType(t, ctx(), 0); }

    // Type of a name expression from the declaration it resolved to.
    // `auto [a, b] = pair;`: the type of the binding named `symbol`, the matching data member (or
    // array element) of the hidden object the declaration names; null when symbol is no binding.
    const Ty* bindingType(VariableDecl* v, uint32_t symbol)
    {
      size_t index = v->structuredBindings.size();
      for (size_t i = 0; i < v->structuredBindings.size(); ++i)
        if (static_cast<NameExpr*>(v->structuredBindings[i])->name.last().symbol == symbol)
          index = i;
      if (index == v->structuredBindings.size())
        return nullptr;
      const Ty* t = impl.declaredTypeOf(v, activeSubst, 0);
      if (t && impl.containsAuto(t))
      {
        if (const Ty* deduced = locals.deducedTypeOf(v))
          t = deduced; // the object as this walk deduced it (also a range-for variable's)
        else if (v->init && v->init->ty)
          t = impl.deduceAuto(t, v->init->ty);
        else if (const Ty* cached = v->cachedType.load(std::memory_order_acquire); cached && !impl.containsAuto(cached))
          t = cached;
        else
          t = nullptr;
      }
      if (!t || !t->isKnown())
        return unknown(Why::Auto);
      bool isConst = types.stripRef(t)->isConst;
      const Ty* object = types.unqualified(types.stripRef(t));
      if (object->kind == TyKind::Array)
        return isConst ? types.qualified(object->inner, true, false) : object->inner;
      if (object->kind == TyKind::Param)
        return object; // the element of a range that depends on a template parameter
      if (object->kind != TyKind::Class)
        return unknown(Why::Unsupported);
      Scope* cs = impl.classScopeOf(object);
      if (!cs || !cs->owner || cs->owner->kind != NodeKind::ClassDecl)
        return unknown(Why::Unsupported);
      size_t seen = 0;
      for (Decl* m : static_cast<ClassDecl*>(cs->owner)->members)
      {
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)))
          continue;
        if (seen++ == index)
        {
          Subst s = object->args.empty() ? Subst{} : impl.substFor(object);
          const Ty* mt = impl.declaredTypeOf(m, object->args.empty() ? activeSubst : &s, 0);
          return mt && isConst ? types.qualified(mt, true, false) : mt;
        }
      }
      return unknown(Why::Unsupported);
    }

    const Ty* typeOfResolvedName(const Lookup& l, const NamePart& lastPart, Expr* e)
    {
      Decl* d = l.decl;
      if (!d)
        return unknown(Why::Name);
      switch (d->kind)
      {
        case NodeKind::VariableDecl:
        {
          if (auto* v = static_cast<VariableDecl*>(d); !v->structuredBindings.empty())
            if (const Ty* bt = bindingType(v, lastPart.symbol))
              return bt;
          const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
          if (impl.containsAuto(t))
          {
            // A local whose initializer was typed already has its deduced type cached by the
            // walker; otherwise (members/globals in other files) deduce from the initializer now.
            auto* v = static_cast<VariableDecl*>(d);
            if (const Ty* deduced = locals.deducedTypeOf(v))
              return deduced; // a local (or range-for variable) as this walk deduced it
            if (v->init && v->init->ty)
              return impl.deduceAuto(t, v->init->ty);
            return unknown(Why::Auto);
          }
          return t;
        }
        case NodeKind::ParamDecl:
        {
          const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
          return impl.containsAuto(t) ? unknown(Why::Auto) : t;
        }
        case NodeKind::EnumeratorDecl:
          return impl.declaredTypeOf(d, nullptr, 0);
        case NodeKind::FunctionDecl:
          return impl.declaredTypeOf(d, activeSubst, 0);
        case NodeKind::ClassDecl:
        case NodeKind::EnumDecl:
        case NodeKind::UsingAliasDecl:
        {
          // A type used as an expression: constructor call `Foo(x)`, `Foo{}` or `Enum(x)`.
          TypeContext c = ctx();
          // `const_iterator(*this)` in a member of ParticleSurface : InfiniteRectangle<ParticleChunk*>:
          // the base's nested class with the base's arguments.
          Subst bs;
          if (e && e->kind == NodeKind::NameExpr && static_cast<NameExpr*>(e)->name.parts.size() == 1)
            bs = impl.baseSubstForUnqualified(l, c, 0);
          if (!bs.empty())
          {
            if (activeSubst)
              for (const Subst::Pair& p : activeSubst->pairs)
                bs.pairs.push_back(p);
            c.subst = &bs;
          }
          return impl.typeOfTypeDecl(d, l, lastPart, c, 0);
        }
        case NodeKind::TemplateParamDecl:
        {
          const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
          return t && t->kind == TyKind::Value && t->inner ? t->inner : t; // `M == Mode::Fast`: the value's type
        }
        case NodeKind::NamespaceDecl:
          return unknown(Why::Unsupported);
        default:
          (void)e;
          return unknown(Why::Unsupported);
      }
    }

    const Ty* resolveName(NameExpr* e)
    {
      if (e->name.empty())
        return unknown(Why::Unsupported);
      if (e->name.parts.size() == 1 && e->name.parts[0].hasTemplateArgs && impl.isBoolTraitName(e->name.parts[0].symbol))
      {
        // A builtin trait: a bool the code generator evaluates once the arguments are known.
        for (Node* arg : e->name.parts[0].templateArgs)
          walkNode(arg);
        e->resolved = nullptr;
        ++stats.namesGlobal;
        return types.boolType();
      }
      Lookup l = impl.lookupQualified(e->name, scope, &locals, 0, activeSubst);
      e->resolved = l.decl;
      for (const NamePart& part : e->name.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
      switch (l.found)
      {
        case Found::No:
          if (e->name.parts.size() == 1)
            if (const Ty* b = impl.builtinByName(e->name.parts[0].symbol))
              return b; // `uint32_t(x)`
          ++stats.namesUnresolved;
          countUnresolved(e->name, e->token);
          return unknown(Why::Name);
        case Found::Local: ++stats.namesLocal; break;
        case Found::Member: ++stats.namesMember; break;
        case Found::Global: ++stats.namesGlobal; break;
        case Found::Dependent: ++stats.namesDependent; break;
      }
      if (l.found == Found::Dependent)
        return l.decl ? impl.declaredTypeOf(l.decl, activeSubst, 0) : types.unknown();
      // Members of the current class template keep their dependent types; members reached
      // through a qualified name on an instantiation get substituted.
      if (e->name.parts.size() > 1 && l.found == Found::Member && l.foundIn && l.foundIn->owner &&
          !l.foundIn->owner->templateParams.empty())
      {
        // Qualifier like `Vector<int>::npos`: substitute from the qualifier type when it is an instantiation.
        QualifiedName qualifier = e->name;
        qualifier.parts.count -= 1;
        NamedType tmp{};
        tmp.kind = NodeKind::NamedType;
        tmp.name = qualifier;
        const Ty* q = typeOf(&tmp);
        // `IsSmall::value` with `struct IsSmall : Constant<bool, true> {}`: a member of a template
        // base takes that base instantiation's arguments.
        if (q->kind == TyKind::Class && (!q->args.empty() || (q->decl && q->decl->ownScope != l.foundIn)))
          if (Subst s = impl.memberSubst(q, l.foundIn, 0); !s.pairs.empty())
            return impl.declaredTypeOf(l.decl, &s, 0);
      }
      return typeOfResolvedName(l, e->name.last(), e);
    }

    // --- member access through the object's type ---

    struct MemberResult
    {
      const Ty* type = nullptr;
      Decl* decl = nullptr;
      const std::vector<Entry>* overloads = nullptr;
      std::shared_ptr<std::vector<Entry>> merged; // what overloads points to when built here (a using-declaration's set joined)
      Subst subst;
      Why why = Why::None;
    };

    // Follows operator-> on class types for `a->b`.
    const Ty* objectForArrow(const Ty* t, int depth = 0)
    {
      t = types.stripRef(t);
      if (!t || depth > 4)
        return t;
      if (t->kind == TyKind::Pointer)
        return t->inner;
      if (t->kind == TyKind::Class)
      {
        MemberResult arrow = memberOf(t, impl.opSymbols[uint32_t(P::Arrow)], nullptr, 0, false);
        if (arrow.type && arrow.type->kind == TyKind::Function)
          return objectForArrow(arrow.type->inner, depth + 1);
        return types.unknown();
      }
      return t;
    }

    // Looks a member up in the class of `objectTy` (after stripping references and const).
    // The instantiation a member found in scope `in` belongs to, seen through an object of type t
    // whose class scope is cls: t itself, or the base (as written in cls, under t's substitution)
    // on the way to `in`. `v.x` with x declared in Vector2<T, V> and v a Vec: Vector2<double, Vec>.
    const Ty* instantiationOfBase(const Ty* t, Scope* cls, Scope* in, int depth)
    {
      return impl.instantiationOfBase(t, cls, in, depth);
    }

    // `symbol` in the bases of the instantiation t (class scope cls) as its arguments make them.
    std::optional<MemberResult> memberOfInstantiatedBases(const Ty* t, Scope* cls, uint32_t symbol, uint32_t token)
    {
      if (!cls->owner || instantiatedBaseDepth > 16)
        return std::nullopt;
      Subst own = impl.substFor(t);
      LocalStack params;
      for (Decl* p : cls->templateParams)
        params.push(static_cast<TemplateParamDecl*>(p)->name, p);
      ParsedFile* file = cls->owner->file;
      TypeContext ctx{cls->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
      std::optional<MemberResult> result;
      ++instantiatedBaseDepth;
      for (Type* b : cls->baseTypes)
      {
        const Ty* bt = impl.convertType(b, ctx, 1);
        if (!bt || bt->kind != TyKind::Class || !bt->decl || types.isDependent(bt))
          continue;
        MemberResult r = memberOf(bt, symbol, nullptr, token, false);
        if (r.decl)
        {
          result = std::move(r);
          break;
        }
      }
      --instantiatedBaseDepth;
      return result;
    }

    // `using Vector2::save;` beside the class's own save overloads (l found in a class scope; t the
    // class as the object or `this` has it, cls its scope): both sets as candidates, own first,
    // with the arguments of the base instances the using-declarations reach in baseArgs; null when
    // l has no such mix.
    std::shared_ptr<std::vector<Entry>> withUsingOverloads(const Lookup& l, const Ty* t, Scope* cls, Subst& baseArgs)
    {
      if (!l.overloads || l.overloads->size() < 2 || !l.foundIn || l.foundIn->kind != ScopeKind::Class)
        return nullptr;
      auto isUsing = [](Decl* d) { return d && d->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(d)->isEnum; };
      bool hasUsing = false, hasOwn = false;
      for (const Entry& e : *l.overloads)
      {
        hasUsing = hasUsing || isUsing(e.decl);
        hasOwn = hasOwn || (e.decl && e.decl->kind == NodeKind::FunctionDecl);
      }
      if (!hasUsing || !hasOwn)
        return nullptr;
      auto merged = std::make_shared<std::vector<Entry>>();
      for (const Entry& e : *l.overloads)
        if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
          merged->push_back(e);
      for (const Entry& e : *l.overloads)
      {
        if (!isUsing(e.decl))
          continue;
        Lookup target = impl.lookupQualified(static_cast<UsingNameDecl*>(e.decl)->name, l.foundIn, nullptr, 0);
        if (target.overloads)
          for (const Entry& te : *target.overloads)
            if (te.decl && te.decl->kind == NodeKind::FunctionDecl)
              merged->push_back(te);
        if (t && cls && target.foundIn && target.foundIn != l.foundIn && target.foundIn->kind == ScopeKind::Class)
          if (const Ty* base = instantiationOfBase(t, cls, target.foundIn, 0))
            for (const Subst::Pair& p : impl.substFor(base).pairs)
              if (!baseArgs.find(p.param))
                baseArgs.pairs.push_back(p);
      }
      return merged;
    }

    MemberResult memberOf(const Ty* objectTy, uint32_t symbol, const QualifiedName* name, uint32_t token, bool countMissing = true)
    {
      MemberResult r;
      const Ty* t = types.stripRef(objectTy);
      if (!t || !t->isKnown())
      {
        r.why = Why::Base;
        return r;
      }
      if (t->kind == TyKind::Param || (t->kind == TyKind::Class && types.isDependent(t) && !t->decl))
      {
        r.type = types.param(t->decl);
        return r;
      }
      if (t->kind == TyKind::DependentMember)
      {
        // `typename T::X` object: known once T is.
        r.type = types.param(nullptr);
        return r;
      }
      if (t->kind != TyKind::Class && t->kind != TyKind::Enum)
      {
        r.why = Why::Base;
        return r;
      }
      Scope* cls = t->decl ? t->decl->ownScope : nullptr;
      if (!cls)
      {
        // Only a forward declaration was seen.
        Entry e{t->decl, nullptr};
        cls = impl.scopeOfEntry(e, 0);
        if (!cls)
        {
          r.why = Why::Base;
          return r;
        }
      }
      Lookup l;
      bool found = true;
      const Ty* foundInBase = nullptr; // the instantiation of the base l was found in, once computed
      if (name && name->parts.size() > 1)
      {
        // `obj.Base::member`: Base as the object's class sees it, else as the expression's context
        // does (`prototype->Super::addValues(result)` with Super a template parameter).
        l = impl.lookupQualified(*name, cls, nullptr, 0);
        if (l.found == Found::No || !l.decl)
        {
          QualifiedName qualifier = *name;
          qualifier.parts.count -= 1;
          NamedType written{};
          written.kind = NodeKind::NamedType;
          written.token = token;
          written.name = qualifier;
          const Ty* q = typeOf(&written);
          if (q && q->isKnown() && types.isDependent(q))
          {
            r.type = types.param(nullptr); // known once Super is
            return r;
          }
          if (Scope* qs = q && q->kind == TyKind::Class ? impl.classScopeOf(q) : nullptr)
          {
            Lookup inQualifier;
            if (impl.lookupInScope(qs, symbol, inQualifier, 0))
              l = inQualifier;
          }
        }
      }
      else
      {
        found = impl.lookupInScope(cls, symbol, l, 0);
        // A class template's bases are linked to its scope once, without its arguments, so a base
        // the arguments choose (`struct X : BoolConstant<B>`, `Conditional<c, Value<T>, T>`) is
        // missing there or is some other class: an instantiation's members come from its own bases.
        if (!t->args.empty() && !types.isDependent(t) && !cls->baseTypes.empty())
        {
          bool viaOtherClass = false;
          if (found && l.foundIn && l.foundIn != cls && l.foundIn->kind == ScopeKind::Class)
          {
            foundInBase = instantiationOfBase(t, cls, l.foundIn, 0);
            viaOtherClass = !foundInBase;
          }
          if (!found || viaOtherClass)
            if (std::optional<MemberResult> viaBase = memberOfInstantiatedBases(t, cls, symbol, token))
              return *viaBase;
        }
        else if (!found && !types.isDependent(t) && !cls->baseTypes.empty())
        {
          // A plain class deriving from such an instantiation (`struct Item : H<Item, true>`,
          // whose Hook takes a sibling nested class as its base by the arguments): the member
          // through the bases as types.
          if (std::optional<MemberResult> viaBase = memberOfInstantiatedBases(t, cls, symbol, token))
            return *viaBase;
        }
      }
      if (!found)
      {
        if (types.isDependent(t))
        {
          r.type = types.unknown();
          r.why = Why::None; // Member of a dependent base: cannot know yet.
          if (countMissing)
            ++stats.membersDependent;
          return r;
        }
        if (hasUnknownBase(cls, 0))
        {
          // A base class outside the corpus (std::vector...) may declare it.
          r.why = Why::Base;
          if (countMissing)
          {
            ++stats.membersUnknownBase;
            ++stats.unknownBaseMembers[symbol];
          }
          return r;
        }
        r.why = Why::Member;
        if (countMissing)
        {
          ++stats.membersNotFound;
          ++stats.missingMembers[symbol];
          stats.missingMembersFirst.emplace(symbol, ResolveStats::Where{stats.fileIndex, token});
        }
        return r;
      }
      Subst usingArgs;
      std::shared_ptr<std::vector<Entry>> merged = withUsingOverloads(l, t, cls, usingArgs);
      if (merged)
      {
        l.decl = merged->front().decl;
        l.overloads = merged.get();
      }
      if (l.decl && l.decl->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(l.decl)->isEnum)
      {
        // `using Base::f;` in the class: f means the base's overloads, as members of that base
        // (`using super::front;` with `using super = CompactDeque<T>;`: front's T is CompactDeque's).
        Lookup target = impl.lookupQualified(static_cast<UsingNameDecl*>(l.decl)->name, l.foundIn ? l.foundIn : cls, nullptr, 0);
        if (target.found != Found::No && target.decl)
        {
          l.decl = target.decl;
          l.overloads = target.overloads;
          if (target.foundIn && target.foundIn->kind == ScopeKind::Class)
          {
            l.foundIn = target.foundIn;
            foundInBase = nullptr;
          }
        }
      }
      if (l.found == Found::No || !l.decl)
      {
        r.why = Why::Member;
        return r;
      }
      r.decl = l.decl;
      r.overloads = l.overloads;
      r.merged = merged;
      const Ty* owner = t;
      if (l.foundIn && l.foundIn != cls && l.foundIn->kind == ScopeKind::Class)
        if (const Ty* base = foundInBase ? foundInBase : instantiationOfBase(t, cls, l.foundIn, 0))
          owner = base; // `v.x` with x declared in the base Vector2<T, V>: that base's instantiation binds T
      r.subst = impl.substFor(owner);
      if (!r.subst.empty())
        instantiateClass(types.unqualified(owner));
      for (const Subst::Pair& p : usingArgs.pairs)
        if (!r.subst.find(p.param))
          r.subst.pairs.push_back(p);
      if (l.found == Found::Dependent)
      {
        r.type = types.param(l.decl);
        return r;
      }
      const Ty* declared = impl.declaredTypeOf(l.decl, &r.subst, 0);
      if (impl.containsAuto(declared))
      {
        auto* v = l.decl->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(l.decl) : nullptr;
        declared = v && v->init && v->init->ty ? impl.deduceAuto(declared, v->init->ty) : types.unknown();
        if (!declared->isKnown())
          r.why = Why::Auto;
      }
      r.type = declared;
      return r;
    }

    // True if the class or one of its bases has a base type that could not be resolved.
    bool hasUnknownBase(Scope* cls, int depth)
    {
      if (!cls || depth > 16 || cls->baseTypes.empty())
        return false;
      impl.ensureBases(cls, 0);
      if (cls->baseScopes.size() < cls->baseTypes.size())
        return true;
      for (Scope* b : cls->baseScopes)
        if (hasUnknownBase(b, depth + 1))
          return true;
      return false;
    }

    const Ty* typeOfMember(MemberExpr* m)
    {
      const Ty* base = walkExpr(m->base);
      if (m->dynamicMember)
      {
        // `object.[m]`: the data member the descriptor m names (reflection, design.md 8.3).
        const Ty* d = types.stripRef(walkExpr(m->dynamicMember));
        const Ty* object = m->op == P::Arrow || m->op == P::SafeArrow ? objectForArrow(base) : base;
        if (!d || !d->isKnown())
          return unknown(Why::Base);
        if (d->kind != TyKind::Member || !d->decl)
        {
          m->resolved = nullptr;
          ++stats.membersDependent;
          return types.isDependent(d) ? d : unknown(Why::Unsupported); // in a template body: dependent until the loop binds m
        }
        m->resolved = d->decl;
        ++stats.membersBound;
        const Ty* t = d->inner ? d->inner : types.unknown();
        const Ty* obj = object ? types.stripRef(object) : nullptr;
        if (obj && obj->isConst && t->isKnown() && !t->isReference())
          t = types.qualified(t, true, false);
        return t;
      }
      for (const NamePart& part : m->member.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
      if (m->member.empty())
        return unknown(Why::Unsupported);
      const NamePart& last = m->member.last();
      uint32_t symbol = last.special == 2 && last.opToken < uint32_t(P::Count) ? impl.opSymbols[last.opToken] : last.symbol;
      if (symbol == UINT32_MAX)
      {
        if (last.special == 1)
        {
          // obj.~Foo()
          ++stats.membersBound;
          return types.function(types.voidType(), {}, false);
        }
        return unknown(Why::Unsupported);
      }
      const Ty* object = base;
      if (m->op == P::Arrow || m->op == P::SafeArrow)
        object = objectForArrow(base);
      if (!object || !object->isKnown())
      {
        ++stats.membersUnknownBase;
        ++stats.unknownBaseMembers[symbol];
        return unknown(Why::Base);
      }
      if (const Ty* descriptor = types.stripRef(object); descriptor->kind == TyKind::Member)
      {
        // A reflection descriptor (`for constexpr (auto m : members(T))`): m.name, m.offset, m.type
        // (written as a type: `a.type value;`, read as decltype(a.type)); an alternative's
        // (`alternatives(U)`) also a.tag, its U::Tag value.
        m->resolved = nullptr;
        ++stats.membersBound;
        std::string_view which = symbols.name(symbol);
        if (which == "name")
          return types.pointer(types.qualified(types.charType(), true, false));
        if (which == "type")
          return descriptor->inner ? descriptor->inner : types.unknown();
        size_t index = 0;
        if (ClassDecl* u = alternativeOf(descriptor, index))
          return which == "tag" && u->tagEnum ? types.enumType(u->tagEnum) : unknown(Why::Member);
        if (which == "offset")
          return types.sizeType();
        return unknown(Why::Member);
      }
      MemberResult r = memberOf(object, symbol, &m->member, m->token);
      m->resolved = r.decl;
      if (r.decl)
      {
        if (r.type && types.isDependent(r.type))
          ++stats.membersDependent;
        else
          ++stats.membersBound;
      }
      else if (r.why == Why::Base)
        ++stats.membersUnknownBase;
      if (r.why != Why::None)
        return unknown(r.why);
      if (r.type && r.type->kind == TyKind::Function && last.special == 0)
      {
        // `obj.method` used as a value is only meaningful in a call, which handles overloads.
      }
      return r.type ? r.type : types.unknown();
    }

    // --- calls ---

    // Picks a function among overloads by argument count and conversion ranking.
    FunctionDecl* pickOverload(Decl* primary, const std::vector<Entry>* overloads, const std::vector<const Ty*>& argTypes,
                               const Subst* classSubst, const Ty* objectTy, bool& ambiguous, const NamePart* namePart = nullptr,
                               Span<Expr*> callArgs = {})
    {
      std::vector<FunctionDecl*> candidates;
      if (overloads)
      {
        for (const Entry& e : *overloads)
          if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
            candidates.push_back(static_cast<FunctionDecl*>(e.decl));
      }
      else if (primary && primary->kind == NodeKind::FunctionDecl)
        candidates.push_back(static_cast<FunctionDecl*>(primary));
      if (!namePart)
        return impl.chooseAmong(candidates, argTypes, classSubst, objectTy, ambiguous);
      // The explicit template arguments (`load<bool>()`): a candidate with fewer parameters than
      // the list names (`typeIndex<T, Tail...>` with Tail known), and no pack to take the rest,
      // is not one, nor one whose parameter is of another kind than its argument.
      std::function<bool(FunctionDecl*, Subst&)> bind = [&](FunctionDecl* f, Subst& s)
      {
        if (!namePart->hasTemplateArgs || namePart->templateArgs.empty())
          return true;
        size_t n = f->templateParams.size();
        size_t before = s.pairs.size();
        size_t given = bindExplicitArgs(f, *namePart, callArgs, s);
        if (given > n && !(n && static_cast<TemplateParamDecl*>(f->templateParams[n - 1])->isPack))
          return false;
        for (size_t i = before; i < s.pairs.size(); ++i)
          if (!argumentKindFits(s.pairs[i].type, static_cast<TemplateParamDecl*>(s.pairs[i].param)))
            return false; // `create<Abs>` names no `template<VoronoiType TYPE> create`
        return true;
      };
      return impl.chooseAmong(candidates, argTypes, classSubst, objectTy, ambiguous, &bind);
    }

    // Argument types of a call; a pack expansion `args...` contributes one type per element.
    // Lvalues present themselves as lvalue references (argumentType).
    // The parameter pack an expansion pattern expands (the first pack parameter named in it), or null.
    Decl* packParamIn(Node* n, int depth = 0)
    {
      if (!n || depth > 32)
        return nullptr;
      switch (n->kind)
      {
        case NodeKind::NameExpr:
        {
          Decl* d = static_cast<NameExpr*>(n)->resolved;
          return d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->isPack ? d : nullptr;
        }
        case NodeKind::ParenExpr: return packParamIn(static_cast<ParenExpr*>(n)->inner, depth + 1);
        case NodeKind::UnaryExpr: return packParamIn(static_cast<UnaryExpr*>(n)->operand, depth + 1);
        case NodeKind::CastExpr: return packParamIn(static_cast<CastExpr*>(n)->operand, depth + 1);
        case NodeKind::NamedCastExpr: return packParamIn(static_cast<NamedCastExpr*>(n)->operand, depth + 1);
        case NodeKind::MemberExpr: return packParamIn(static_cast<MemberExpr*>(n)->base, depth + 1);
        case NodeKind::CallExpr:
        {
          auto* c = static_cast<CallExpr*>(n);
          if (Decl* d = packParamIn(c->callee, depth + 1))
            return d;
          for (Expr* a : c->args)
            if (Decl* d = packParamIn(a, depth + 1))
              return d;
          return nullptr;
        }
        default: return nullptr;
      }
    }

    // `f(forward<Args>(args)...)` under an instantiation with an empty Args: no argument at all.
    bool isEmptyExpansion(Expr* a)
    {
      if (!a || a->kind != NodeKind::UnaryExpr || static_cast<UnaryExpr*>(a)->isKeyword ||
          P(static_cast<UnaryExpr*>(a)->op) != P::Ellipsis || !static_cast<UnaryExpr*>(a)->postfix)
        return false;
      Decl* pd = packParamIn(static_cast<UnaryExpr*>(a)->operand);
      if (!pd)
        return false;
      const Ty* pt = impl.declaredTypeOf(pd, activeSubst, 0);
      return pt && pt->kind == TyKind::Pack && pt->args.empty();
    }

    std::vector<const Ty*> argTypesOf(const Span<Expr*>& args)
    {
      bool walked = walkedArgs && args.data == walkedArgs && args.size() == walkedArgTypes.size();
      std::vector<const Ty*> out;
      for (size_t i = 0; i < args.size(); ++i)
      {
        Expr* a = args[i];
        if (isEmptyExpansion(a))
          continue;
        const Ty* t = walked ? walkedArgTypes[i] : a ? a->ty : nullptr;
        if (a && t && t->kind == TyKind::Pack)
          for (const Ty* e : t->args)
            out.push_back(argumentType(a, e, types));
        else if (a && a->kind == NodeKind::BraceInitExpr)
          out.push_back(braceListType(a, impl.initializerListDecl(), types));
        else
          out.push_back(a ? argumentType(a, t, types) : nullptr);
      }
      return out;
    }

    // The argument list of the call being typed and the types this walk gave its expressions, which
    // argTypesOf uses for it instead of the types on the AST nodes: the AST is shared, and another
    // thread checking another instantiation of the same body may have rewritten them meanwhile.
    struct WalkedArgs
    {
      Walker& w;
      Expr* const* savedArgs;
      std::vector<const Ty*> savedTypes;
      WalkedArgs(Walker& w, const Span<Expr*>& args, std::vector<const Ty*>&& types)
        : w(w), savedArgs(w.walkedArgs), savedTypes(std::move(w.walkedArgTypes))
      {
        w.walkedArgs = args.data;
        w.walkedArgTypes = std::move(types);
      }
      ~WalkedArgs()
      {
        w.walkedArgs = savedArgs;
        w.walkedArgTypes = std::move(savedTypes);
      }
    };

    // The explicit template arguments of a call (`count<int, int, int>()`): one per parameter;
    // a trailing pack takes the rest, and is empty when the list leaves it nothing and there is
    // no call argument to deduce it from. A pack expansion in the list (`load<V, Z, Args...>`)
    // gives one argument per element of the pack, none for an empty one. Returns how many
    // arguments the list names: an expansion still dependent counts as none, since it may name
    // none.
    size_t bindExplicitArgs(FunctionDecl* f, const NamePart& namePart, Span<Expr*> args, Subst& s)
    {
      size_t n = f->templateParams.size(), named = 0;
      std::vector<const Ty*> given;
      for (Node* written : namePart.templateArgs)
      {
        Decl* param = n ? f->templateParams[std::min(given.size(), n - 1)] : nullptr;
        const Ty* t = explicitArgument(written, param);
        bool expansion = written && written->kind == NodeKind::PackType;
        if (expansion && t && t->kind == TyKind::Pack)
        {
          given.insert(given.end(), t->args.begin(), t->args.end());
          named += t->args.size();
        }
        else
        {
          given.push_back(t);
          named += !expansion;
        }
      }
      if (n == 0)
        return named;
      for (size_t i = 0; i < given.size() && i < n; ++i)
      {
        auto* param = static_cast<TemplateParamDecl*>(f->templateParams[i]);
        if (param->isPack && i + 1 == n)
        {
          std::vector<const Ty*> rest(given.begin() + i, given.end());
          s.pairs.push_back({param, types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
          return named;
        }
        s.pairs.push_back({param, given[i]});
      }
      if (namePart.hasTemplateArgs && args.empty())
      {
        auto* last = static_cast<TemplateParamDecl*>(f->templateParams[n - 1]);
        if (last->isPack && !s.find(last))
          s.pairs.push_back({last, types.pack({})}); // `count<int>()`: nothing left for Args
      }
      return named;
    }

    // Whether an explicit template argument is of its parameter's kind: a type for a type
    // parameter, a constant for a constant one. One not known yet, or dependent on the
    // enclosing template in a way that does not tell, fits.
    bool argumentKindFits(const Ty* t, const TemplateParamDecl* p)
    {
      if (!t || !t->isKnown() || !p->innerParams.empty())
        return true; // template template parameters take templates: not checked here
      if (t->kind == TyKind::Pack)
      {
        for (const Ty* e : t->args)
          if (!argumentKindFits(e, p))
            return false;
        return true;
      }
      bool constantParam = t->kind == TyKind::Param && t->decl && !static_cast<TemplateParamDecl*>(t->decl)->isType;
      if (p->isType)
        return t->kind != TyKind::Value && !constantParam;
      return t->kind == TyKind::Value || constantParam || t->kind == TyKind::DependentMember; // `T::value` may be either
    }

    // An explicit template argument of a call (`forward<Args>(args)`): under an instantiation that
    // binds the pack Args to one element, a non-pack parameter of the callee gets that element,
    // not a pack of one. An expansion (`typeIndex<T, Tail...>`) stays the pack it expands.
    const Ty* explicitArgument(Node* written, Decl* param)
    {
      const Ty* t = impl.templateArgument(written, param, ctx(), 0);
      bool packParam = param && param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(param)->isPack;
      if (t && t->kind == TyKind::Pack && t->args.size() == 1 && !packParam && !(written && written->kind == NodeKind::PackType))
        return t->args[0];
      return t;
    }

    // Records f's deduced template arguments on the call for the code generator: built aside and
    // stored with the count last, since instantiations checked on other threads type the same call
    // (the count was incremented in place and could pass the 8 slots).
    static void recordTemplateArgs(CallExpr* call, FunctionDecl* f, const Subst& s)
    {
      const Ty* found[8] = {};
      uint8_t n = 0;
      for (Decl* p : f->templateParams)
      {
        const Ty* t = s.find(p);
        if (!t || n >= 8)
          break;
        found[n++] = t;
      }
      for (uint8_t i = 0; i < n; ++i)
        call->templateArgs[i] = found[i];
      call->templateArgCount = n;
    }

    // Return type of calling `f` with the given arguments, deducing its template parameters. An
    // operator expression has no argument list: its operand types (operands) are deduced from.
    const Ty* returnTypeOfCall(FunctionDecl* f, const Subst* classSubst, const Span<Expr*>& args, const NamePart* namePart,
                               CallExpr* call = nullptr, const std::vector<const Ty*>* operands = nullptr)
    {
      auto argTypes = [&] { return operands ? *operands : argTypesOf(args); };
      if (f->body == nullptr && f->returnType == nullptr && !(f->flags & (DF_Constructor | DF_Destructor | DF_Conversion)))
        return unknown(Why::Unsupported);
      bool autoReturn = false;
      if (f->returnType && Impl::astMentionsAuto(f->returnType) && !f->trailingReturn)
        autoReturn = true; // `auto`, `auto&`, `const auto*`: the body decides
      if (autoReturn)
      {
        if (!f->templateParams.empty())
        {
          Subst s = classSubst ? *classSubst : Subst{};
          const Ty* fnTy = impl.declaredTypeOf(f, classSubst, 0);
          if (namePart)
            bindExplicitArgs(f, *namePart, args, s);
          impl.deduceCall(f, fnTy, argTypes(), s);
          impl.fillDefaultTemplateArgs(f, s);
          if (call)
            recordTemplateArgs(call, f, s);
          if (const Ty* r = instantiateFunction(f, s, true, classSubst))
            return r;
          return unknown(Why::AutoReturn);
        }
        if (const Ty* cached = f->cachedType.load(std::memory_order_acquire))
          if (cached->kind == TyKind::Function && !impl.containsAuto(cached->inner))
            return classSubst ? impl.substitute(cached->inner, *classSubst) : cached->inner;
        // `renderParameters.getScreenPositionFunction()` before the file defining it was walked:
        // its body is typed now, once, for the return type.
        if (const Ty* r = instantiateFunction(f, Subst{}, true, classSubst))
          return r;
        return unknown(Why::AutoReturn);
      }
      Subst subst = classSubst ? *classSubst : Subst{};
      // The callee's type for deduction: the enclosing instantiation's arguments apply, but never
      // to the callee's own parameters (a template calling itself, `sum(rest...)` inside
      // `sum<int, int, int>`, deduces them afresh from the arguments).
      Subst outer;
      const Subst* forType = classSubst ? classSubst : activeSubst;
      if (!classSubst && activeSubst && !f->templateParams.empty())
      {
        for (const Subst::Pair& p : activeSubst->pairs)
        {
          bool own = false;
          for (Decl* tp : f->templateParams)
            own = own || tp == p.param;
          if (!own)
            outer.pairs.push_back(p);
        }
        forType = outer.pairs.empty() ? nullptr : &outer;
      }
      const Ty* fnTy = impl.declaredTypeOf(f, forType, 0);
      if (!f->templateParams.empty())
      {
        // Explicit template arguments first, then deduction from the arguments.
        if (namePart)
          bindExplicitArgs(f, *namePart, args, subst);
        impl.deduceCall(f, fnTy, argTypes(), subst);
        impl.fillDefaultTemplateArgs(f, subst);
        if (call)
          recordTemplateArgs(call, f, subst);
        {
          static bool debugCalls = getenv("KPP_DEBUG_CALLS") != nullptr;
          if (debugCalls)
          {
            std::string line = std::string(call ? "call " : "call(no expr) ") + impl.debugName(f->name.last().symbol) + ":";
            for (size_t i = 0; call && i < call->templateArgCount; ++i)
              line += " " + types.toString(call->templateArgs[i], &impl.symbols);
            line += " | args:";
            for (const Ty* a : argTypes())
              line += " " + types.toString(a, &impl.symbols);
            line += " | explicit:";
            if (namePart)
              for (size_t i = 0; i < namePart->templateArgs.size() && i < f->templateParams.size(); ++i)
                line += " " + types.toString(impl.templateArgument(namePart->templateArgs[i], f->templateParams[i], ctx(), 0), &impl.symbols);
            line += " | subst:";
            if (activeSubst)
              for (auto& [d, ty] : activeSubst->pairs)
                line += " " + impl.debugName(static_cast<TemplateParamDecl*>(d)->name) + "=" + types.toString(ty, &impl.symbols);
            fprintf(stderr, "%s\n", line.c_str());
          }
        }
        const Ty* result = impl.substitute(fnTy->inner, subst);
        Type* written = f->trailingReturn ? f->trailingReturn : f->returnType;
        LocalStack own;
        Impl::pushEnclosingTemplateParams(f, own);
        if ((f->trailingReturn && Impl::astMentionsDecltype(f->trailingReturn)) || impl.astNeedsArguments(written, impl.lexicalScopeOf(f), &own))
        {
          // `-> Future<decltype(callable())>`, `typename Conditional<(sizeof(A) < sizeof(B)), B,
          // A>::Type`: typed again with the deduced arguments, since the generic type substituted
          // keeps neither what calling the closure gives nor which specialisation the condition picks.
          Subst all = subst;
          if (forType)
            for (const Subst::Pair& p : forType->pairs)
              if (!all.find(p.param))
                all.pairs.push_back(p);
          if (const Ty* full = impl.declaredTypeOf(f, &all, 0); full && full->kind == TyKind::Function && full->inner->isKnown())
            result = full->inner;
        }
        if (impl.dependsOn(result, f->templateParams))
          return unknown(Why::Deduction);
        if (!types.isDependent(result))
          instantiateFunction(f, subst, false, classSubst);
        return result;
      }
      return fnTy->inner;
    }

    // Whether d is the only function of its name there (overloads: the entries found with it)
    // and no template.
    static bool onlyPlainCandidate(Decl* d, const std::vector<Entry>* overloads)
    {
      if (!d || d->kind != NodeKind::FunctionDecl || !d->templateParams.empty())
        return false;
      size_t functions = 0;
      if (overloads)
        for (const Entry& e : *overloads)
          functions += e.decl->kind == NodeKind::FunctionDecl;
      return functions <= 1;
    }

    const Ty* typeOfCall(CallExpr* c)
    {
      std::vector<const Ty*> walked;
      walked.reserve(c->args.size());
      for (Expr* a : c->args)
        walked.push_back(walkExpr(a));
      WalkedArgs walkedArgsScope(*this, c->args, std::move(walked));
      Expr* callee = c->callee;
      bool ambiguous = false;
      if (callee->kind == NodeKind::NameExpr)
      {
        auto* n = static_cast<NameExpr*>(callee);
        if (n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && impl.isTypeTraitName(n->name.parts[0].symbol))
          if (const Ty* trait = impl.typeTrait(n->name.parts[0], ctx(), 0); trait && trait->isKnown())
          {
            // `Conditional<isSigned<U>, int64_t, uint64_t>(value)`: a functional cast to the type the
            // built-in trait gives, as the same name gives in a declaration (not the library's
            // Conditional struct).
            n->resolved = nullptr;
            for (Node* arg : n->name.parts[0].templateArgs)
              walkNode(arg);
            ++stats.callsResolved;
            record(n, trait);
            return trait;
          }
        Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
        n->resolved = l.decl;
        for (const NamePart& part : n->name.parts)
          for (Node* arg : part.templateArgs)
            walkNode(arg);
        switch (l.found)
        {
          case Found::No:
            if (n->name.parts.size() == 1)
              if (const Ty* b = impl.builtinByName(n->name.parts[0].symbol))
              {
                record(n, b); // `uint32_t(x)`: a functional cast, not an unresolved name
                return b;
              }
            ++stats.namesUnresolved;
            countUnresolved(n->name, n->token);
            record(n, unknown(Why::Name));
            return types.unknown();
          case Found::Local: ++stats.namesLocal; break;
          case Found::Member: ++stats.namesMember; break;
          case Found::Global: ++stats.namesGlobal; break;
          case Found::Dependent: ++stats.namesDependent; break;
        }
        if (l.found == Found::Dependent)
        {
          const Ty* t = l.decl ? impl.declaredTypeOf(l.decl, activeSubst, 0) : types.unknown();
          record(n, t);
          if (l.decl && l.decl->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(l.decl)->isType)
            return callResultOfValue(t && t->kind == TyKind::Value && t->inner ? t->inner : t, c); // `func(a, b)`: a call through a constant parameter
          return t;
        }
        // `save(t)` in a member of a class with `using Vector2::save;` beside its own save overloads.
        Subst usingArgs;
        std::shared_ptr<std::vector<Entry>> usingMerged;
        if (n->name.parts.size() == 1 && l.found == Found::Member && l.foundIn && l.foundIn->kind == ScopeKind::Class)
        {
          const Ty* self = thisType && thisType->kind == TyKind::Pointer ? types.unqualified(thisType->inner) : nullptr;
          if ((usingMerged = withUsingOverloads(l, self, self ? impl.classScopeOf(self) : nullptr, usingArgs)))
          {
            l.decl = usingMerged->front().decl;
            l.overloads = usingMerged.get();
          }
        }
        Decl* d = l.decl;
        if (d && d->kind == NodeKind::FunctionDecl)
        {
          // `DamageTypeID::fromIndex(i)` with `using DamageTypeID = ID<DamageType, uint8_t>`: a static
          // member named through an instantiation has that instantiation's arguments.
          Subst qualifier = usingArgs;
          if (n->name.parts.size() > 1 && l.foundIn && l.foundIn->kind == ScopeKind::Class)
            qualifier = impl.qualifierSubst(n->name, ctx(), 0, l.foundIn);
          else if (n->name.parts.size() == 1 && l.found == Found::Member && activeSubst && l.foundIn && l.foundIn->kind == ScopeKind::Class &&
                   l.foundIn->owner)
          {
            // `construct(...)` in a member of UniquePointer<T>: the member of this instantiation (or of
            // the base it was found in), whose parameter types and constraints need T.
            for (const Subst::Pair& p : impl.baseSubstForUnqualified(l, ctx(), 0).pairs)
              if (!qualifier.find(p.param))
                qualifier.pairs.push_back(p);
            // With the enclosing instantiation's arguments, except a candidate's own parameters,
            // which the call deduces afresh (a member template calling itself).
            auto ownOfCandidate = [&](Decl* param)
            {
              auto owns = [&](Decl* f) { return std::find(f->templateParams.begin(), f->templateParams.end(), param) != f->templateParams.end(); };
              if (!l.overloads)
                return owns(d);
              for (const Entry& e : *l.overloads)
                if (e.decl && e.decl->kind == NodeKind::FunctionDecl && owns(e.decl))
                  return true;
              return false;
            };
            for (const Subst::Pair& p : activeSubst->pairs)
              if (!qualifier.find(p.param) && !ownOfCandidate(p.param))
                qualifier.pairs.push_back(p);
          }
          const Subst* classSubst = qualifier.empty() ? nullptr : &qualifier;
          FunctionDecl* f = pickOverload(d, l.overloads, argTypesOf(c->args), classSubst, nullptr, ambiguous, &n->name.last(), c->args);
          if (!f)
          {
            // No callee the code generator could trust: not the first of several overloads, nor a
            // template with the arguments an earlier walk of this call recorded (the generic
            // one's). A single plain candidate stays: the code generator converts the arguments
            // itself and says so when it cannot.
            c->templateArgCount = 0;
            if (!onlyPlainCandidate(d, l.overloads))
              n->resolved = nullptr;
            record(n, types.unknown());
            return unknown(Why::Overload);
          }
          if (ambiguous)
            ++stats.callsAmbiguousArity;
          ++stats.callsResolved;
          n->resolved = f;
          const Ty* fnTy = impl.declaredTypeOf(f, classSubst, 0);
          record(n, fnTy);
          return returnTypeOfCall(f, classSubst, c->args, &n->name.last(), c);
        }
        // Constructor call, functional cast, callable variable...
        const Ty* t = typeOfResolvedName(l, n->name.last(), n);
        record(n, t);
        if (d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                  (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))))
        {
          ++stats.callsResolved; // a functional cast, `int64_t(x)` included when int64_t is an imported typedef
          if (n->name.parts.size() > 1 &&
              (d->kind == NodeKind::UsingAliasDecl || d->kind == NodeKind::VariableDecl ||
               (d->kind == NodeKind::ClassDecl && d->templateParams.empty() && !impl.effectiveParams(d).empty())))
          {
            // `Table::value_type(65)`: the alias as the type path reads it, with the qualifier's
            // instantiation substituted (Array<unsigned char, 4>::value_type is unsigned char);
            // `Surface::const_iterator(surface)`: the nested class of the base
            // InfiniteRectangle<Chunk*> with that base's arguments.
            NamedType written{};
            written.kind = NodeKind::NamedType;
            written.token = n->token;
            written.name = n->name;
            if (const Ty* asType = typeOf(&written); asType && asType->isKnown())
              t = asType;
          }
          if (impl.isUndeducedClassTemplate(t))
            if (const Ty* deduced = impl.deduceClassArgs(t, argTypesOf(c->args)))
            {
              record(n, deduced);
              return deduced; // `Pair(a, b)`, `Span(p, n)`: the class's arguments from a constructor (CTAD)
            }
          return t;
        }
        return callResultOfValue(t, c);
      }
      if (callee->kind == NodeKind::MemberExpr)
      {
        auto* m = static_cast<MemberExpr*>(callee);
        const Ty* base = walkExpr(m->base);
        for (const NamePart& part : m->member.parts)
          for (Node* arg : part.templateArgs)
            walkNode(arg);
        const Ty* object = (m->op == P::Arrow || m->op == P::SafeArrow) ? objectForArrow(base) : base;
        if (m->member.empty())
        {
          record(m, types.unknown());
          return unknown(Why::Unsupported);
        }
        const NamePart& last = m->member.last();
        if (!object || !object->isKnown())
        {
          ++stats.membersUnknownBase;
          if (last.symbol != UINT32_MAX)
            ++stats.unknownBaseMembers[last.symbol];
          record(m, types.unknown());
          return unknown(Why::Base);
        }
        if (last.special == 1)
        {
          ++stats.membersBound;
          record(m, types.function(types.voidType(), {}, false));
          return types.voidType();
        }
        uint32_t symbol = last.special == 2 && last.opToken < uint32_t(P::Count) ? impl.opSymbols[last.opToken] : last.symbol;
        if (symbol == UINT32_MAX)
        {
          record(m, types.unknown());
          return unknown(Why::Unsupported);
        }
        MemberResult r = memberOf(object, symbol, &m->member, m->token);
        m->resolved = r.decl;
        if (!r.decl)
        {
          if (r.why == Why::Base)
            ++stats.membersUnknownBase;
          record(m, types.unknown());
          return r.why == Why::None ? types.unknown() : unknown(r.why);
        }
        if (r.decl->kind == NodeKind::FunctionDecl)
        {
          FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypesOf(c->args), &r.subst, object, ambiguous, &last, c->args);
          if (!f)
          {
            ++stats.membersBound;
            c->templateArgCount = 0; // as for a name
            if (!onlyPlainCandidate(r.decl, r.overloads))
              m->resolved = nullptr;
            record(m, types.unknown());
            return unknown(Why::Overload);
          }
          if (ambiguous)
            ++stats.callsAmbiguousArity;
          ++stats.callsResolved;
          m->resolved = f;
          const Ty* fnTy = impl.declaredTypeOf(f, &r.subst, 0);
          record(m, fnTy);
          if (types.isDependent(fnTy))
            ++stats.membersDependent;
          else
            ++stats.membersBound;
          return returnTypeOfCall(f, &r.subst, c->args, &last, c);
        }
        if (r.type && types.isDependent(r.type))
          ++stats.membersDependent;
        else
          ++stats.membersBound;
        record(m, r.type ? r.type : types.unknown());
        if (r.why != Why::None)
          return unknown(r.why);
        return callResultOfValue(r.type, c);
      }
      const Ty* t = walkExpr(callee);
      return callResultOfValue(t, c);
    }

    // A call to a generic lambda: its template parameters are deduced from the arguments, its
    // body typed for them once per set of arguments (in the environment it was written in), and
    // the call has the result type found there. The deduced arguments are recorded on the call
    // for the code generator, as for function templates.
    const Ty* genericLambdaCall(const Ty* t, CallExpr* c)
    {
      LambdaExpr* l = Impl::lambdaOf(t);
      std::vector<const Ty*> argTypes = argTypesOf(c->args);
      Subst deduced;
      impl.deduceParams(l->params, t->args, argTypes, deduced);
      Subst own; // in parameter order: the instantiation's key
      for (Decl* p : l->templateParams)
      {
        const Ty* a = deduced.find(p);
        if (!a || !a->isKnown() || types.isDependent(a))
          return unknown(Why::Deduction);
        own.pairs.push_back({p, a});
      }
      if (own.pairs.size() > 8)
        return unknown(Why::Deduction);
      for (size_t i = 0; i < own.pairs.size(); ++i)
        c->templateArgs[i] = own.pairs[i].type;
      c->templateArgCount = uint8_t(own.pairs.size()); // the count last: other threads type the same call
      // The instantiation's own copy of the body, typed once (null while the lambda calls itself
      // through a capture).
      const Ty* callTy = instantiationDepth > 16 ? nullptr : impl.checkLambdaInstance(t, own, nullptr, &stats);
      return callTy && callTy->kind == TyKind::Function ? callTy->inner : unknown(Why::AutoReturn);
    }

    // Result of calling a value of type t: function pointers, lambdas, functors.
    const Ty* callResultOfValue(const Ty* t, CallExpr* c)
    {
      t = types.stripRef(t);
      if (!t || !t->isKnown())
        return types.unknown();
      if (t->kind == TyKind::Function)
        return t->inner;
      if (t->kind == TyKind::Pointer && t->inner->kind == TyKind::Function)
        return t->inner->inner;
      if (t->kind == TyKind::Lambda)
      {
        if (c && Impl::isGenericLambda(t))
          return genericLambdaCall(t, c);
        return t->inner ? t->inner : unknown(Why::AutoReturn);
      }
      if (t->kind == TyKind::Class)
      {
        MemberResult r = memberOf(t, impl.opSymbols[uint32_t(P::LParen)], nullptr, c->token, false);
        if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
        {
          bool ambiguous;
          if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypesOf(c->args), &r.subst, t, ambiguous))
            return returnTypeOfCall(f, &r.subst, c->args, nullptr);
          return unknown(Why::Overload);
        }
        return r.why == Why::Member ? unknown(Why::Member) : types.unknown();
      }
      if (t->kind == TyKind::Param)
        return t;
      return unknown(Why::Unsupported);
    }

    // --- operators ---

    const Ty* promote(const Ty* t)
    {
      t = types.unqualified(types.stripRef(t));
      switch (t->kind)
      {
        case TyKind::Bool: case TyKind::Char: return types.intType(32);
        case TyKind::Int: return t->bits < 32 ? types.intType(32) : t;
        case TyKind::Enum: return types.intType(32);
        default: return t;
      }
    }

    const Ty* arithmetic(const Ty* a, const Ty* b)
    {
      a = promote(a);
      b = promote(b);
      if (a->kind == TyKind::Float || b->kind == TyKind::Float)
      {
        uint8_t bits = std::max<uint8_t>(a->kind == TyKind::Float ? a->bits : 0, b->kind == TyKind::Float ? b->bits : 0);
        return types.floatType(bits);
      }
      if (a->kind != TyKind::Int || b->kind != TyKind::Int)
        return a->kind == TyKind::Int ? a : b;
      if (a->bits != b->bits)
        return a->bits > b->bits ? a : b;
      return types.intType(a->bits, a->isUnsigned || b->isUnsigned);
    }

    // Overloaded operator lookup: member of the left operand's class, then free functions in
    // the class's namespace and the current scope chain.
    // The function `lhs op rhs` (or `op lhs`) calls: the class's member operator, else a free one
    // from the namespaces of both operand classes or the current scope. For a member, subst is
    // the substitution of its class and isMember is set.
    FunctionDecl* operatorFunction(P op, const Ty* lhs, const Ty* rhs, uint32_t token, size_t arity, Subst& subst, bool& isMember)
    {
      uint32_t symbol = impl.opSymbols[uint32_t(op)];
      const Ty* l = types.stripRef(lhs);
      isMember = false;
      if (l && l->kind == TyKind::Class)
      {
        MemberResult r = memberOf(l, symbol, nullptr, token, false);
        if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
        {
          bool ambiguous;
          std::vector<const Ty*> argTypes;
          if (arity == 2)
            argTypes.push_back(rhs);
          if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypes, &r.subst, lhs, ambiguous))
          {
            subst = r.subst;
            isMember = true;
            return f;
          }
        }
      }
      // Free function: look in the namespaces of both operand classes and enums and the current
      // scope; members are not candidates here (a class's own operator== hides no free one).
      std::vector<Scope*> places;
      for (const Ty* t : {l, types.stripRef(rhs)})
        if (t && (t->kind == TyKind::Class || t->kind == TyKind::Enum) && t->decl)
          places.push_back(impl.lexicalScopeOf(t->decl));
      places.push_back(scope);
      for (Scope* place : places)
      {
        for (Scope* s = place; s; s = s->parent)
        {
          if (s->kind == ScopeKind::Class)
            continue;
          Lookup look;
          if (!impl.lookupInScope(s, symbol, look, 0))
            continue;
          if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
          {
            bool ambiguous;
            std::vector<const Ty*> argTypes{lhs};
            if (arity == 2)
              argTypes.push_back(rhs);
            if (FunctionDecl* f = pickOverload(look.decl, look.overloads, argTypes, nullptr, nullptr, ambiguous))
              return f;
          }
          break;
        }
      }
      return nullptr;
    }

    // Whether `lhs <=> rhs` is a defaulted operator<=>, which gives the class `==` as well.
    bool threeWayDefaulted(const Ty* lhs, const Ty* rhs, uint32_t token)
    {
      Subst subst;
      bool isMember;
      FunctionDecl* f = operatorFunction(P::Spaceship, lhs, rhs, token, 2, subst, isMember);
      return f && (f->flags & DF_Default);
    }

    // The type of `lhs op rhs` (or `op lhs`) through an operator function, or for what every class
    // has; null when there is none (counted as unknown here only when countFailure). lhsOperand:
    // the left operand as an argument (an lvalue a reference), for a template's deduction.
    const Ty* overloadedOperator(P op, const Ty* lhs, const Ty* rhs, uint32_t token, size_t arity, bool countFailure = true,
                                 const Ty* lhsOperand = nullptr)
    {
      const Ty* l = types.stripRef(lhs);
      Subst subst;
      bool isMember;
      Span<Expr*> none;
      if (FunctionDecl* f = operatorFunction(op, lhs, rhs, token, arity, subst, isMember))
      {
        // The operands a template deduces from: a member's object is not one of them.
        std::vector<const Ty*> operands;
        if (!isMember)
          operands.push_back(lhsOperand ? lhsOperand : lhs);
        if (arity == 2)
          operands.push_back(rhs);
        return returnTypeOfCall(f, isMember ? &subst : nullptr, none, nullptr, nullptr, &operands);
      }
      if (arity == 2)
      {
        // The comparisons C++ rewrites when the class defines no operator for them: `a != b` is
        // `!(a == b)`, `a < b` and the other orderings are `(a <=> b) < 0`, and a defaulted
        // operator<=> gives `==` too.
        switch (op)
        {
          case P::NotEq:
            if (operatorFunction(P::EqEq, lhs, rhs, token, 2, subst, isMember) || threeWayDefaulted(lhs, rhs, token) ||
                operatorFunction(P::EqEq, rhs, lhs, token, 2, subst, isMember))
              return types.boolType(); // `3 != id` through Id::operator==(int): the operands reversed
            break;
          case P::EqEq:
            if (threeWayDefaulted(lhs, rhs, token) || operatorFunction(P::EqEq, rhs, lhs, token, 2, subst, isMember))
              return types.boolType();
            break;
          case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
            if (operatorFunction(P::Spaceship, lhs, rhs, token, 2, subst, isMember))
              return types.boolType();
            break;
          default:
            break;
        }
      }
      // Operators every class has or that always yield bool.
      switch (op)
      {
        case P::Assign: return lhs;                       // implicit copy/move assignment
        case P::AndAnd: case P::OrOr: case P::Bang: return types.boolType();
        case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
          if (l && l->kind == TyKind::Enum)
            return types.boolType();
          break;
        default:
          break;
      }
      return countFailure ? unknown(Why::Operator) : nullptr;
    }

    static bool isAssignmentOp(P op)
    {
      switch (op)
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign: case P::PercentAssign:
        case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign: case P::ShrAssign:
          return true;
        default:
          return false;
      }
    }

    // C++'s built-in candidates for an operator no function defines: a class operand takes part
    // through its conversion operator to a non-class type (`area < smallBlueprintAreaSize` with a
    // Const<float>). The operand's converted type, or null when it has no such conversion; an
    // operand that is no class stays as it is.
    const Ty* builtinOperand(const Ty* t)
    {
      const Ty* s = types.stripRef(t);
      if (!s || s->kind != TyKind::Class)
        return t;
      return impl.scalarConversionOf(s);
    }

    // A user-defined operator on an enumeration operand (`Flags operator|(Flags, Flags)`) beats the
    // built-in one, which a scoped enum does not even have. Only a free non-template function whose
    // parameters take the enum operands as they are (not through a conversion to some class).
    const Ty* enumOperator(P op, const Ty* lhs, const Ty* rhs, uint32_t token)
    {
      Subst subst;
      bool isMember;
      FunctionDecl* f = operatorFunction(op, lhs, rhs, token, 2, subst, isMember);
      if (!f || isMember || !impl.takesEnumOperands(f, lhs, rhs))
        return nullptr;
      Span<Expr*> none;
      return returnTypeOfCall(f, nullptr, none, nullptr);
    }

    const Ty* typeOfBinary(BinaryExpr* b)
    {
      const Ty* l = walkExpr(b->lhs);
      const Ty* r = walkExpr(b->rhs);
      if (b->isKeyword)
      {
        switch (Kw(b->op))
        {
          case Kw::and_: case Kw::or_: case Kw::not_eq_: return types.boolType();
          default: return arithmetic(l, r);
        }
      }
      P op = P(b->op);
      if (op == P::Comma)
        return r;
      const Ty* ls = types.stripRef(l);
      const Ty* rs = types.stripRef(r);
      if (!ls || !rs)
        return types.unknown();
      if (op == P::DotStar || op == P::ArrowStar)
        return rs->kind == TyKind::MemberPointer ? types.lref(rs->inner) : types.unknown();
      if ((op == P::EqEq || op == P::NotEq) &&
          ((typedUnionOf(ls) && rs->kind == TyKind::Enum) || (typedUnionOf(rs) && ls->kind == TyKind::Enum)))
        return types.boolType(); // `p == Property::Integer`: the typed union's tag (design.md 8.5)
      if (ls->kind == TyKind::Class || rs->kind == TyKind::Class)
      {
        if (!ls->isKnown() || !rs->isKnown())
          return types.unknown();
        if (types.isDependent(ls) || types.isDependent(rs))
          return types.param(ls->kind == TyKind::Param ? ls->decl : rs->decl);
        if (const Ty* t = overloadedOperator(op, l, argumentType(b->rhs, r, types), b->token, 2, false, argumentType(b->lhs, l, types)))
          return t;
        // No operator function: the built-in operator on the operands' scalar conversions (an
        // assignment's left operand is the object itself and converts to nothing).
        const Ty* lc = isAssignmentOp(op) ? (ls->kind == TyKind::Class ? nullptr : l) : builtinOperand(l);
        const Ty* rc = builtinOperand(r);
        if (!lc || !rc)
          return unknown(Why::Operator);
        l = lc;
        r = rc;
        ls = types.stripRef(l);
        rs = types.stripRef(r);
      }
      if (!ls->isKnown() || !rs->isKnown())
      {
        // Comparisons and logic have a known result regardless of the operands.
        switch (op)
        {
          case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
          case P::AndAnd: case P::OrOr:
            return types.boolType();
          default:
            if (op == P::Assign || op == P::PlusAssign || op == P::MinusAssign || op == P::StarAssign ||
                op == P::SlashAssign || op == P::PercentAssign || op == P::AmpAssign || op == P::PipeAssign ||
                op == P::CaretAssign || op == P::ShlAssign || op == P::ShrAssign)
              return ls->isKnown() ? l : types.unknown();
            return types.unknown();
        }
      }
      if (ls->kind == TyKind::Param || rs->kind == TyKind::Param)
        return ls->kind == TyKind::Param ? ls : rs;
      if ((ls->kind == TyKind::Enum || rs->kind == TyKind::Enum) && op != P::Assign)
        if (const Ty* user = enumOperator(op, argumentType(b->lhs, l, types), argumentType(b->rhs, r, types), b->token))
          return user;
      switch (op)
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign:
        case P::PercentAssign: case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign:
        case P::ShrAssign:
          return l;
        case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
        case P::AndAnd: case P::OrOr:
          return types.boolType();
        case P::Spaceship:
          return impl.orderingType();
        case P::Shl: case P::Shr:
          return promote(l);
        case P::Plus: case P::Minus:
        {
          bool lp = ls->kind == TyKind::Pointer || ls->kind == TyKind::Array;
          bool rp = rs->kind == TyKind::Pointer || rs->kind == TyKind::Array;
          if (lp && rp)
            return types.ptrdiffType();
          if (lp)
            return types.decay(ls);
          if (rp)
            return types.decay(rs);
          return arithmetic(l, r);
        }
        case P::Star: case P::Slash: case P::Percent: case P::Amp: case P::Pipe: case P::Caret:
          return arithmetic(l, r);
        case P::DotStar: case P::ArrowStar:
          return rs->kind == TyKind::MemberPointer ? types.lref(rs->inner) : types.unknown();
        default:
          return unknown(Why::Unsupported);
      }
    }

    // `forward<Args>(args)...` under an instantiation: the pattern typed once per element with the
    // packs bound to that element, as C++ instantiates an expansion; its type the pack of the
    // results (Args = (int, String&): (int&&, String&)). Null when the pattern names no pack
    // parameter bound to a pack here.
    const Ty* typeOfExpansion(UnaryExpr* u)
    {
      Decl* pd = activeSubst ? packParamIn(u->operand) : nullptr;
      const Ty* pt = pd ? impl.declaredTypeOf(pd, activeSubst, 0) : nullptr;
      if (!pt || pt->kind != TyKind::Pack)
        return nullptr;
      size_t n = pt->args.size();
      const Subst* outer = activeSubst;
      std::vector<const Ty*> elements;
      for (size_t i = 0; i < n; ++i)
      {
        Subst one;
        for (const Subst::Pair& p : outer->pairs)
        {
          bool pack = p.param && p.param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(p.param)->isPack;
          one.pairs.push_back({p.param, pack && p.type && p.type->kind == TyKind::Pack && p.type->args.size() == n ? p.type->args[i] : p.type});
        }
        activeSubst = &one;
        const Ty* e = walkExpr(u->operand);
        activeSubst = outer;
        elements.push_back(e ? e : types.unknown());
      }
      return types.pack(Span<const Ty*>{elements.data(), uint32_t(elements.size())});
    }

    const Ty* typeOfUnary(UnaryExpr* u)
    {
      if (!u->isKeyword && P(u->op) == P::Ellipsis && u->postfix)
        if (const Ty* pack = typeOfExpansion(u))
          return pack;
      const Ty* t = walkExpr(u->operand);
      const Ty* s = types.stripRef(t);
      if (u->isKeyword)
        return Kw(u->op) == Kw::not_ ? types.boolType() : promote(t);
      P op = P(u->op);
      if (op == P::Ellipsis)
        return t;
      if (!s || !s->isKnown())
        return op == P::Bang ? types.boolType() : types.unknown();
      if (op == P::Amp && u->operand->kind == NodeKind::NameExpr)
      {
        // `&Style::field`: a pointer to a member of the class, not to the field of some object.
        auto* n = static_cast<NameExpr*>(u->operand);
        Decl* d = n->resolved;
        if (n->name.parts.size() > 1 && d && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) &&
            !(d->flags & DF_Static) && d->parent && d->parent->kind == NodeKind::ClassDecl)
          return types.memberPointer(s, impl.selfType(d->parent));
      }
      if (s->kind == TyKind::Param)
        return op == P::Bang ? types.boolType() : op == P::Amp ? types.pointer(s) : s;
      if (s->kind == TyKind::Class)
      {
        if (op == P::Amp)
          return types.pointer(s);
        if (types.isDependent(s))
          return types.unknown();
        if (const Ty* r = overloadedOperator(op, t, nullptr, u->token, 1, false, argumentType(u->operand, t, types)))
          return r;
        // No operator function: the built-in one on the operand's scalar conversion (`-speed`
        // with a Const<double>); ++ and -- change the object itself.
        const Ty* c = op == P::PlusPlus || op == P::MinusMinus ? nullptr : builtinOperand(t);
        if (!c)
          return unknown(Why::Operator);
        t = c;
        s = types.stripRef(c);
      }
      switch (op)
      {
        case P::Star:
          if (s->kind == TyKind::Pointer || s->kind == TyKind::Array)
            return types.lref(s->inner);
          return unknown(Why::Operator);
        case P::Amp: return types.pointer(s);
        case P::Bang: return types.boolType();
        case P::Minus: case P::Plus: case P::Tilde: return promote(t);
        case P::PlusPlus: case P::MinusMinus: return u->postfix ? types.unqualified(s) : t;
        default: return unknown(Why::Unsupported);
      }
    }

    // --- literals ---

    const Ty* typeOfLiteral(LiteralExpr* l)
    {
      switch (l->literalKind)
      {
        case TokenKind::Keyword:
          return l->keyword == Kw::nullptr_ ? types.nullptrType() : types.boolType();
        case TokenKind::Char:
        {
          std::string_view s = spelling(l->token);
          if (s.starts_with("u8")) return types.charType(8, true);
          if (s.starts_with("u")) return types.charType(16, true);
          if (s.starts_with("U")) return types.charType(32, true);
          if (s.starts_with("L")) return types.charType(16, true);
          return types.charType(8, false);
        }
        case TokenKind::String:
        {
          std::string_view s = spelling(l->token);
          const Ty* ch = types.charType(8, false);
          if (s.starts_with("u8")) ch = types.charType(8, true);
          else if (s.starts_with("u")) ch = types.charType(16, true);
          else if (s.starts_with("U")) ch = types.charType(32, true);
          else if (s.starts_with("L")) ch = types.charType(16, true);
          return types.pointer(types.qualified(ch, true, false));
        }
        case TokenKind::Number:
        {
          std::string_view s = spelling(l->token);
          bool hex = s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
          bool isFloat = false;
          bool isUnsigned = false;
          int longs = 0;
          bool f32 = false;
          for (size_t i = hex ? 2 : 0; i < s.size(); ++i)
          {
            char c = s[i];
            if (c == '.' || (!hex && (c == 'e' || c == 'E')) || (hex && (c == 'p' || c == 'P')))
              isFloat = true;
          }
          size_t end = s.size();
          for (const char* ms : {"i64", "i32", "i16", "i8"}) // MSVC: 1i64, 1ui64
            if (!isFloat && end > strlen(ms) && s.compare(end - strlen(ms), strlen(ms), ms) == 0)
            {
              if (ms[1] == '6')
                longs = 2;
              end -= strlen(ms);
              break;
            }
          while (end > 0)
          {
            char c = s[end - 1];
            if (c == 'u' || c == 'U') { isUnsigned = true; --end; }
            else if (c == 'l' || c == 'L') { ++longs; --end; }
            else if ((c == 'f' || c == 'F') && !hex) { f32 = true; isFloat = true; --end; }
            else if (c == 'z' || c == 'Z') { --end; }
            else break;
          }
          if (isFloat)
            return types.floatType(f32 ? 32 : 64);
          // The first type the value fits: int, then (for hex and octal) unsigned int, then the
          // 64-bit ones, as C++ has it; 0xffffffff is unsigned, 4294967296 is 64 bits wide.
          std::string digits;
          for (size_t i = 0; i < end; ++i)
            if (s[i] != '\'')
              digits += s[i];
          bool binary = digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B');
          unsigned long long value = binary ? strtoull(digits.c_str() + 2, nullptr, 2) : strtoull(digits.c_str(), nullptr, 0);
          bool decimal = !hex && !(digits.size() > 1 && digits[0] == '0');
          if (longs < 2)
          {
            if (!isUnsigned && value <= 0x7fffffffull)
              return types.intType(32, false);
            if ((isUnsigned || !decimal) && value <= 0xffffffffull)
              return types.intType(32, true);
          }
          if (!isUnsigned && value <= 0x7fffffffffffffffull)
            return types.intType(64, false);
          return types.intType(64, true);
        }
        default:
          return types.unknown();
      }
    }

    // --- element type for range-for ---

    // The iterator type of the free begin(range) found for a class without a begin() member (in
    // the class's namespace or from here), the call a range-for makes then; null if none.
    const Ty* freeBeginType(const Ty* t, Expr* rangeExpr)
    {
      std::vector<const Ty*> argTypes{types.lref(t)};
      for (Scope* place : {impl.lexicalScopeOf(t->decl), scope})
        for (Scope* s = place; s; s = s->parent)
        {
          if (s->kind == ScopeKind::Class)
            continue;
          Lookup look;
          if (!impl.lookupInScope(s, impl.symBegin, look, 0))
            continue;
          if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
          {
            bool ambiguous;
            if (FunctionDecl* f = pickOverload(look.decl, look.overloads, argTypes, nullptr, nullptr, ambiguous))
              return returnTypeOfCall(f, nullptr, Span<Expr*>{&rangeExpr, 1}, nullptr);
          }
          break;
        }
      return nullptr;
    }

    // rangeExpr: the range written, for a free begin(range).
    const Ty* elementTypeOf(const Ty* range, Expr* rangeExpr)
    {
      const Ty* t = types.stripRef(range);
      if (!t || !t->isKnown())
        return types.unknown();
      if (t->kind == TyKind::Array || t->kind == TyKind::Pointer)
        return types.lref(t->inner);
      if (t->kind == TyKind::Class)
      {
        if (types.isDependent(t))
          return types.unknown();
        MemberResult begin = memberOf(t, impl.symBegin, nullptr, 0, false);
        const Ty* iterator = nullptr;
        if (begin.decl && begin.decl->kind == NodeKind::FunctionDecl)
        {
          bool ambiguous;
          Span<Expr*> none;
          if (FunctionDecl* f = pickOverload(begin.decl, begin.overloads, {}, &begin.subst, t, ambiguous))
            iterator = returnTypeOfCall(f, &begin.subst, none, nullptr);
        }
        else if (!begin.decl && rangeExpr)
          iterator = freeBeginType(t, rangeExpr);
        if (!iterator || !iterator->isKnown())
          return types.unknown();
        const Ty* it = types.stripRef(iterator);
        if (it->kind == TyKind::Pointer)
          return types.lref(it->inner);
        if (it->kind == TyKind::Class && !types.isDependent(it))
        {
          MemberResult deref = memberOf(it, impl.opSymbols[uint32_t(P::Star)], nullptr, 0, false);
          if (deref.decl && deref.decl->kind == NodeKind::FunctionDecl)
          {
            bool ambiguous;
            Span<Expr*> none;
            if (FunctionDecl* f = pickOverload(deref.decl, deref.overloads, {}, &deref.subst, it, ambiguous))
              return returnTypeOfCall(f, &deref.subst, none, nullptr);
          }
        }
        return types.unknown();
      }
      return types.unknown();
    }

    // --- generic walking ---

    void walkNode(Node* n)
    {
      if (!n)
        return;
      if (uint8_t(n->kind) <= uint8_t(NodeKind::PackType))
        walkType(static_cast<Type*>(n));
      else if (uint8_t(n->kind) >= uint8_t(NodeKind::NameExpr))
        walkExpr(static_cast<Expr*>(n));
      else if (uint8_t(n->kind) >= uint8_t(NodeKind::BlockStmt))
        walkStmt(static_cast<Stmt*>(n));
      else
        walkDecl(static_cast<Decl*>(n));
    }

    void walkType(Type* t)
    {
      if (!t)
        return;
      switch (t->kind)
      {
        case NodeKind::NamedType: resolveTypeName(static_cast<NamedType*>(t)); break;
        case NodeKind::PointerType: walkType(static_cast<PointerType*>(t)->pointee); break;
        case NodeKind::LRefType: walkType(static_cast<LRefType*>(t)->referenced); break;
        case NodeKind::RRefType: walkType(static_cast<RRefType*>(t)->referenced); break;
        case NodeKind::ConstType: walkType(static_cast<ConstType*>(t)->inner); break;
        case NodeKind::PackType: walkType(static_cast<PackType*>(t)->pattern); break;
        case NodeKind::ArrayType:
          walkType(static_cast<ArrayType*>(t)->element);
          walkExpr(static_cast<ArrayType*>(t)->size);
          break;
        case NodeKind::FunctionType:
        {
          auto* f = static_cast<FunctionType*>(t);
          walkType(f->result);
          size_t mark = locals.mark();
          for (Decl* p : f->params)
            walkParam(static_cast<ParamDecl*>(p));
          locals.pop(mark);
          break;
        }
        case NodeKind::DecltypeType: walkExpr(static_cast<DecltypeType*>(t)->expr); break;
        case NodeKind::MemberPointerType:
          walkType(static_cast<MemberPointerType*>(t)->classType);
          walkType(static_cast<MemberPointerType*>(t)->pointee);
          break;
        default: break;
      }
    }

    void walkParam(ParamDecl* p)
    {
      walkType(p->type);
      walkExpr(p->defaultValue);
      locals.push(p->name, p);
    }

    void pushTemplateParams(Span<Decl*> params)
    {
      for (Decl* d : params)
      {
        auto* p = static_cast<TemplateParamDecl*>(d);
        walkType(p->valueType);
        walkNode(p->defaultArg);
        locals.push(p->name, p);
      }
    }

    // Scope in which the body of a (possibly out-of-line) member is resolved.
    Scope* semanticScopeFor(const QualifiedName& name)
    {
      if (name.parts.size() <= 1)
        return scope;
      QualifiedName qualifier = name;
      qualifier.parts.count -= 1;
      Lookup l = impl.lookupQualified(qualifier, scope, &locals, 0);
      if (l.scope && l.scope->kind == ScopeKind::Class)
        if (Scope* special = impl.fullSpecializationScope(qualifier, scope, &locals, file, 0))
          return special; // `int Flag<true>::value() {...}`: the body of the specialisation's member
      return l.scope ? l.scope : scope;
    }

    // A template's own walk: a template parameter in scope that this walk binds to nothing.
    bool inGenericTemplate() const
    {
      for (const LocalStack::Item& item : locals.items)
        if (item.decl && item.decl->kind == NodeKind::TemplateParamDecl && (!activeSubst || !activeSubst->find(item.decl)))
          return true;
      return false;
    }

    // The instance this walk types, for messages: `T = int32, N = 3`; empty outside templates.
    std::string instanceDescription() const
    {
      std::string out;
      if (activeSubst)
        for (const Subst::Pair& p : activeSubst->pairs)
          if (p.param && p.param->kind == NodeKind::TemplateParamDecl)
          {
            uint32_t name = static_cast<TemplateParamDecl*>(p.param)->name;
            out += (out.empty() ? "" : ", ") + std::string(name == UINT32_MAX ? "?" : impl.symbols.name(name)) + " = " +
                   types.toString(p.type, &impl.symbols);
          }
      return out;
    }

    const Ty* computeThisType()
    {
      Decl* cls = enclosingClassDecl(scope);
      if (!cls)
        return nullptr;
      if (cls->kind == NodeKind::EnumDecl)
        return types.lref(types.enumType(cls)); // in an enum's methods, `this` is the value
      return types.pointer(impl.selfType(cls));
    }

    void walkDecls(Span<Decl*> decls)
    {
      for (Decl* d : decls)
        walkDecl(d);
    }

    void walkDecl(Decl* d)
    {
      if (!d)
        return;
      size_t mark = locals.mark();
      if (!d->templateParams.empty())
        pushTemplateParams(d->templateParams);
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
        {
          auto* ns = static_cast<NamespaceDecl*>(d);
          Scope* saved = scope;
          if (ns->ownScope)
            scope = ns->ownScope;
          walkDecls(ns->decls);
          scope = saved;
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          Scope* saved = scope;
          const Ty* savedThis = thisType;
          for (const ClassDecl::Base& b : c->bases)
            walkType(b.type);
          if (c->ownScope)
            scope = c->ownScope;
          else if (c->name.parts.size() > 1)
            scope = semanticScopeFor(c->name);
          if (!activeSubst || c->ownScope != scope)
            thisType = computeThisType();
          walkDecls(c->members);
          scope = saved;
          thisType = savedThis;
          break;
        }
        case NodeKind::EnumDecl:
        {
          auto* e = static_cast<EnumDecl*>(d);
          walkType(e->underlying);
          Scope* saved = scope;
          const Ty* savedThis = thisType;
          if (e->ownScope)
          {
            scope = e->ownScope;
            thisType = computeThisType(); // the enum's methods see `this` as the value
          }
          for (Decl* en : e->enumerators)
            if (en->kind == NodeKind::EnumeratorDecl)
              walkExpr(static_cast<EnumeratorDecl*>(en)->value);
          walkDecls(e->members);
          scope = saved;
          thisType = savedThis;
          break;
        }
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          Scope* saved = scope;
          const Ty* savedThis = thisType;
          walkType(f->returnType);
          if (f->name.parts.size() > 1)
          {
            scope = semanticScopeFor(f->name);
            if (!activeSubst)
              thisType = computeThisType();
          }
          else if (f->parent && f->parent->kind == NodeKind::NamespaceDecl && f->parent->ownScope)
          {
            // finishIndex simplified the qualified name but set the parent to the namespace decl.
            scope = f->parent->ownScope;
          }
          for (const NamePart& part : f->name.parts)
          {
            for (Node* arg : part.templateArgs)
              walkNode(arg);
            if (part.special == 3)
              walkType(part.convType);
          }
          for (Decl* p : f->params)
            walkParam(static_cast<ParamDecl*>(p));
          walkType(f->trailingReturn);
          walkExpr(f->requiresClause);
          walkExpr(f->condition);
          for (const FunctionDecl::CtorInit& init : f->ctorInits)
          {
            Lookup l = impl.lookupQualified(init.member, scope, &locals, 0);
            if (l.found == Found::No)
            {
              ++stats.namesUnresolved;
              countUnresolved(init.member, f->token);
            }
            else
              ++stats.namesMember;
            for (Expr* a : init.args)
              walkExpr(a);
          }
          bool autoReturn = f->returnType && Impl::astMentionsAuto(f->returnType) && !f->trailingReturn;
          if (autoReturn)
          {
            returnFrames.push_back(nullptr);
            decltypeReturns.push_back(f->returnType->kind == NodeKind::AutoType && static_cast<AutoType*>(f->returnType)->isDecltypeAuto);
          }
          if (f->flags & DF_Static)
            thisType = nullptr;
          else if ((f->flags & DF_Const) && thisType && thisType->kind == TyKind::Pointer && !thisType->inner->isConst)
            thisType = impl.types.pointer(impl.types.qualified(thisType->inner, true, false)); // `const C* this` in a const method
          walkStmt(f->body);
          if (autoReturn)
          {
            const Ty* deduced = returnFrames.back();
            returnFrames.pop_back();
            decltypeReturns.pop_back();
            if (deduced && !returnFrames.empty() && !returnFrames.back())
              returnFrames.back() = deduced; // an instantiation's frame around the body takes it
            if (deduced && deduced->isKnown() && !types.isDependent(deduced))
            {
              // Cache the deduced signature for callers in other files.
              const Ty* fnTy = impl.declaredTypeOf(f, nullptr, 0);
              std::vector<const Ty*> params(fnTy->args.begin(), fnTy->args.end());
              f->cachedType.store(types.function(impl.deducedShape(fnTy->inner, deduced), Span<const Ty*>{params.data(), uint32_t(params.size())}, fnTy->variadic),
                                  std::memory_order_release);
            }
          }
          scope = saved;
          thisType = savedThis;
          break;
        }
        case NodeKind::VariableDecl:
        {
          auto* v = static_cast<VariableDecl*>(d);
          walkType(v->type);
          walkExpr(v->bitfield);
          Scope* saved = scope;
          if (v->name.parts.size() > 1)
            scope = semanticScopeFor(v->name);
          const Ty* initTy = walkExpr(v->init);
          scope = saved;
          // Deduce `auto` now that the initializer is typed, and cache it on the declaration.
          const Ty* declared = impl.declaredTypeOf(v, activeSubst, 0);
          bool isDraft = declared && declared->kind == TyKind::Class && declared->decl && (declared->decl->attributes & ATTR_Draft);
          if ((!declared || isDraft || Impl::mentionsUnknown(declared)) && v->type && !activeSubst)
          {
            // A type only the function's locals know (`using Match = Pair<int, int>;` in the body,
            // `Map<Key, int>` with a local class Key): resolved here, where the locals are, and
            // cached for everyone asking later.
            const Ty* local = typeOf(v->type);
            if (local && local->isKnown() && (!declared || !declared->isKnown() || isDraft || !Impl::mentionsUnknown(local)))
            {
              v->cachedType.store(local, std::memory_order_release);
              declared = local;
            }
          }
          if (v->init && !activeSubst && impl.isUndeducedClassTemplate(declared))
          {
            // `ScopedSetter guard(x, true)`: the class's arguments from a constructor (CTAD);
            // `Array names = {a, b}`: from an aggregate's members; `Duration budget =
            // Milliseconds(5)`: from the initializer's value, one argument.
            Span<Expr*> args;
            bool copyInit = false;
            if (v->init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(v->init)->type == v->type)
              args = static_cast<FunctionalCastExpr*>(v->init)->args; // the parser's `T x(args)`
            else if (v->init->kind == NodeKind::BraceInitExpr)
              args = static_cast<BraceInitExpr*>(v->init)->args;
            else
              copyInit = initTy && initTy->isKnown();
            std::vector<const Ty*> argTypes = copyInit ? std::vector<const Ty*>{argumentType(v->init, initTy, types)} : argTypesOf(args);
            const Ty* deduced = impl.deduceClassArgs(declared, argTypes);
            if (!deduced && v->init->kind == NodeKind::BraceInitExpr)
              deduced = impl.deduceAggregateArgs(declared, argTypes);
            if (deduced)
            {
              v->cachedType.store(deduced, std::memory_order_release);
              if (v->init->kind == NodeKind::FunctionalCastExpr)
                record(v->init, deduced);
              declared = deduced;
            }
          }
          if (impl.containsAuto(declared))
          {
            // From the type this walk gave the initializer, not the one on its (shared) node.
            if (declared == types.decltypeAutoType())
              initTy = decltypeOf(v->init, initTy); // `decltype(auto) r = (x);` is a reference
            const Ty* deduced = v->init && initTy ? impl.deduceAuto(declared, initTy) : types.unknown();
            if (deduced->isKnown())
            {
              deducedLocal = deduced; // kept with the local by walkLocalDecl, per walk
              if (!activeSubst)
                v->cachedType.store(deduced, std::memory_order_release);
            }
            else
              ++stats.unknownAuto;
          }
          break;
        }
        case NodeKind::ParamDecl: walkParam(static_cast<ParamDecl*>(d)); break;
        case NodeKind::UsingAliasDecl: walkType(static_cast<UsingAliasDecl*>(d)->type); break;
        case NodeKind::UsingNameDecl:
        {
          auto* u = static_cast<UsingNameDecl*>(d);
          Lookup l = impl.lookupQualified(u->name, scope, &locals, 0);
          if (l.found == Found::No)
          {
            ++stats.typesUnresolved;
            countUnresolved(u->name, u->token);
          }
          else
            ++stats.typesResolved;
          break;
        }
        case NodeKind::UsingDirectiveDecl:
        {
          auto* u = static_cast<UsingDirectiveDecl*>(d);
          Lookup l = impl.lookupQualified(u->name, scope, &locals, 0);
          if (l.found == Found::No)
          {
            ++stats.typesUnresolved;
            countUnresolved(u->name, u->token);
          }
          else
            ++stats.typesResolved;
          break;
        }
        case NodeKind::StaticAssertDecl:
        {
          auto* s = static_cast<StaticAssertDecl*>(d);
          walkExpr(s->cond);
          // Checked where its condition is known: not in a template's own walk (its instances
          // are), nor in a branch of an instance's `if constexpr` left undecided (it may be the
          // discarded one).
          if (undecidedBranches == 0 && !inGenericTemplate())
            impl.checkStaticAssert(s, ctx(), instanceDescription());
          break;
        }
        case NodeKind::FriendDecl: walkType(static_cast<FriendDecl*>(d)->type); break;
        default: break;
      }
      locals.pop(mark);
    }

    // Declarations inside function bodies: add their names to the local stack.
    void declareLocal(Decl* d)
    {
      switch (d->kind)
      {
        case NodeKind::VariableDecl:
        {
          auto* v = static_cast<VariableDecl*>(d);
          if (v->name.parts.size() == 1)
            locals.push(v->name.parts[0].symbol, v);
          for (Expr* b : v->structuredBindings)
            locals.push(static_cast<NameExpr*>(b)->name.parts[0].symbol, v);
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (c->name.parts.size() == 1)
            locals.push(c->name.parts[0].symbol, c);
          break;
        }
        case NodeKind::EnumDecl:
        {
          auto* e = static_cast<EnumDecl*>(d);
          if (e->name.parts.size() == 1)
            locals.push(e->name.parts[0].symbol, e);
          for (Decl* en : e->enumerators)
            if (en->kind == NodeKind::EnumeratorDecl)
              locals.push(static_cast<EnumeratorDecl*>(en)->name, en);
          break;
        }
        case NodeKind::UsingAliasDecl: locals.push(static_cast<UsingAliasDecl*>(d)->name, d); break;
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          if (f->name.parts.size() == 1 && f->name.parts[0].special == 0)
            locals.push(f->name.parts[0].symbol, f);
          break;
        }
        default: break;
      }
    }

    void walkLocalDecl(Decl* d)
    {
      // Resolve the declaration first (its type/initializer cannot see the name being declared,
      // except for classes), then make the name visible.
      if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl)
      {
        declareLocal(d);
        walkDecl(d);
      }
      else
      {
        deducedLocal = nullptr;
        walkDecl(d);
        const Ty* deduced = d->kind == NodeKind::VariableDecl ? deducedLocal : nullptr;
        deducedLocal = nullptr;
        size_t declaredAt = locals.mark();
        declareLocal(d);
        if (deduced)
          for (size_t i = declaredAt; i < locals.items.size(); ++i)
            locals.items[i].type = deduced; // `auto x = e;`: the type this walk deduced
      }
    }

    void walkStmt(Stmt* s)
    {
      if (!s)
        return;
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
        {
          size_t mark = locals.mark();
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            walkStmt(inner);
          locals.pop(mark);
          break;
        }
        case NodeKind::ExprStmt: walkExpr(static_cast<ExprStmt*>(s)->expr); break;
        case NodeKind::AssertStmt: walkExpr(static_cast<AssertStmt*>(s)->cond); break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
            walkLocalDecl(d);
          break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          size_t mark = locals.mark();
          walkStmt(i->init);
          walkNode(i->cond);
          if (i->isConstexpr && activeSubst && i->cond && uint8_t(i->cond->kind) > uint8_t(NodeKind::PackType) &&
              uint8_t(i->cond->kind) >= uint8_t(NodeKind::NameExpr))
            if (std::optional<int64_t> known = impl.constEval(static_cast<Expr*>(i->cond), ctx(), 0))
            {
              // if constexpr in an instantiation: the discarded branch is not checked (nor are
              // its calls instantiated), as in C++.  Store the result so codegen can read it
              // without re-evaluating (codegen's TypeContext lacks function-body locals).
              i->constexprKnown = int8_t(*known ? 1 : 0);
              walkStmt(*known ? i->then : i->else_);
              locals.pop(mark);
              break;
            }
          bool undecided = i->isConstexpr && activeSubst;
          undecidedBranches += undecided;
          walkStmt(i->then);
          walkStmt(i->else_);
          undecidedBranches -= undecided;
          locals.pop(mark);
          break;
        }
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          size_t mark = locals.mark();
          walkStmt(f->init);
          walkNode(f->cond);
          walkExpr(f->step);
          walkStmt(f->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::RangeForStmt:
        {
          auto* f = static_cast<RangeForStmt*>(s);
          size_t mark = locals.mark();
          const Ty* range = walkExpr(f->range);
          if (f->range->kind == NodeKind::BraceInitExpr)
          {
            // `for (X* kids : {&a, &b})`: an array of the first element's type.
            auto* b = static_cast<BraceInitExpr*>(f->range);
            const Ty* first = !b->args.empty() && b->args[0]->ty ? types.decay(b->args[0]->ty) : nullptr;
            if (first && first->isKnown())
              range = record(f->range, types.array(first, uint32_t(b->args.size())));
          }
          auto* v = static_cast<VariableDecl*>(f->var);
          walkType(v->type);
          const Ty* declared = impl.declaredTypeOf(v, activeSubst, 0);
          const Ty* loopVarType = nullptr;
          if (impl.containsAuto(declared))
          {
            const Ty* element = elementTypeOf(range, f->range);
            const Ty* deduced = element->isKnown() ? impl.deduceAuto(declared, element) : types.unknown();
            if (deduced->isKnown())
            {
              v->cachedType.store(deduced, std::memory_order_release);
              loopVarType = deduced;
            }
            else if (range && range->isKnown() && types.isDependent(range))
              loopVarType = impl.deduceAuto(declared, types.lref(types.param(nullptr))); // typed per instantiation
            else
              ++stats.unknownAuto;
          }
          size_t declaredAt = locals.mark();
          declareLocal(v);
          if (loopVarType)
            for (size_t i = declaredAt; i < locals.items.size(); ++i)
              locals.items[i].type = loopVarType;
          walkStmt(f->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::ForConstexprStmt:
        {
          // The loop variable is a template parameter of the body: in the generic pass the body
          // depends on it; the code generator re-types it per element (as a pack expansion).
          auto* f = static_cast<ForConstexprStmt*>(s);
          size_t mark = locals.mark();
          auto* v = static_cast<TemplateParamDecl*>(f->var);
          walkType(v->valueType);
          if (f->list && f->list->kind == NodeKind::NamedType && isTypesList(static_cast<NamedType*>(f->list)))
            for (Node* arg : static_cast<NamedType*>(f->list)->name.parts[0].templateArgs)
              walkNode(arg); // `types<A, B>`: the elements; the list itself is the compiler's
          else if (Expr* subject = reflectionSubject(f->list))
            walkExpr(subject); // `members(T)`: the type reflected on
          else
            walkNode(f->list);
          locals.push(v->name, v);
          ++forConstexprDepth;
          walkStmt(f->body);
          --forConstexprDepth;
          // In an instantiation of an auto function whose return type is still open (`visit(f, u)`
          // returning f's result from inside the loop): the body once per element, the variable
          // bound to it, as the code generator emits it, until a return gives the type.
          std::vector<const Ty*> elements;
          if (activeSubst && !returnFrames.empty() && !returnFrames.back() && forConstexprElements(f, elements))
          {
            Subst bound = *activeSubst;
            bound.pairs.push_back({v, nullptr});
            const Subst* saved = activeSubst;
            activeSubst = &bound;
            for (const Ty* element : elements)
            {
              bound.pairs.back().type = element;
              size_t inner = locals.mark();
              walkStmt(f->body);
              locals.pop(inner);
              if (returnFrames.back())
                break;
            }
            activeSubst = saved;
          }
          locals.pop(mark);
          break;
        }
        case NodeKind::WhileStmt:
        {
          auto* w = static_cast<WhileStmt*>(s);
          size_t mark = locals.mark();
          walkNode(w->cond);
          walkStmt(w->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::DoStmt:
          walkStmt(static_cast<DoStmt*>(s)->body);
          walkExpr(static_cast<DoStmt*>(s)->cond);
          break;
        case NodeKind::SwitchStmt:
        {
          auto* w = static_cast<SwitchStmt*>(s);
          size_t mark = locals.mark();
          walkStmt(w->init);
          walkNode(w->cond);
          walkStmt(w->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::CaseStmt:
          walkExpr(static_cast<CaseStmt*>(s)->value);
          walkExpr(static_cast<CaseStmt*>(s)->rangeEnd);
          break;
        case NodeKind::ReturnStmt:
        {
          const Ty* t = walkExpr(static_cast<ReturnStmt*>(s)->value);
          if (!decltypeReturns.empty() && decltypeReturns.back())
            t = decltypeOf(static_cast<ReturnStmt*>(s)->value, t);
          // Inside a for constexpr body of an instantiation, before the loop binds its variable,
          // a return that depends on it gives no type (the per-element walk does).
          bool open = forConstexprDepth > 0 && activeSubst && t && (types.isDependent(t) || impl.containsAuto(t));
          if (!returnFrames.empty() && !returnFrames.back() && t && t->isKnown() && !open)
            returnFrames.back() = t;
          break;
        }
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(s);
          walkStmt(t->body);
          for (const TryStmt::Handler& h : t->handlers)
          {
            size_t mark = locals.mark();
            if (h.param)
              walkParam(static_cast<ParamDecl*>(h.param));
            walkStmt(h.body);
            locals.pop(mark);
          }
          break;
        }
        default: break;
      }
    }

    void walkExprs(Span<Expr*> exprs)
    {
      for (Expr* e : exprs)
        walkExpr(e);
    }

    // Resolves names in and computes the type of an expression.
    const Ty* walkExpr(Expr* e)
    {
      if (!e)
        return nullptr;
      struct Current
      {
        Expr*& slot;
        Expr* saved;
        Current(Expr*& slot, Expr* e) : slot(slot), saved(slot) { slot = e; }
        ~Current() { slot = saved; }
      } guard(current, e);
      switch (e->kind)
      {
        case NodeKind::NameExpr: return record(e, resolveName(static_cast<NameExpr*>(e)));
        case NodeKind::CallerExpr: return record(e, types.unknown()); // typed by the parameter it defaults
        case NodeKind::NameofExpr:
          walkExpr(static_cast<NameofExpr*>(e)->operand);
          return record(e, types.pointer(types.qualified(types.charType(), true, false)));
        case NodeKind::LiteralExpr: return record(e, typeOfLiteral(static_cast<LiteralExpr*>(e)));
        case NodeKind::ThisExpr: return record(e, thisType ? thisType : unknown(Why::Unsupported));
        case NodeKind::CallExpr: return record(e, typeOfCall(static_cast<CallExpr*>(e)));
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(e);
          const Ty* base = walkExpr(i->base);
          const Ty* firstIndex = nullptr;
          for (Expr* a : i->args)
          {
            const Ty* at = walkExpr(a);
            if (a == i->args[0])
              firstIndex = at;
          }
          const Ty* b = types.stripRef(base);
          if (!b || !b->isKnown())
            return record(e, types.unknown());
          if (b->kind == TyKind::Pointer || b->kind == TyKind::Array)
            return record(e, types.lref(b->inner));
          if (b->kind == TyKind::Param)
            return record(e, b);
          if (b->kind == TyKind::Class)
          {
            if (ClassDecl* u = typedUnionOf(b); u && i->args.size() == 1)
            {
              // `p[Property::Integer]`: the alternative the enumerator names (design.md 8.5); `p[a]`
              // with a descriptor of alternatives(U): the one it describes.
              std::optional<int64_t> v;
              size_t index = 0;
              if (const Ty* it = firstIndex ? types.stripRef(firstIndex) : nullptr; it && it->kind == TyKind::Member)
              {
                if (alternativeOf(it, index) != u)
                  return record(e, unknown(Why::Operator)); // a descriptor of another union, or of a member
                v = int64_t(index);
              }
              else if (firstIndex && types.isDependent(firstIndex))
                return record(e, types.unknown()); // `u[a]` before the loop binds a
              else
                v = impl.constEval(i->args[0], ctx());
              if (!v || *v < 0 || size_t(*v) >= u->alternatives.size())
                return record(e, unknown(Why::Operator));
              Subst s = b->args.empty() ? Subst{} : impl.substFor(b);
              const Ty* alt = impl.declaredTypeOf(u->alternatives[size_t(*v)], s.pairs.empty() ? nullptr : &s, 0);
              if (!alt || !alt->isKnown())
                return record(e, types.unknown());
              return record(e, types.lref(b->isConst ? types.qualified(alt, true, false) : alt));
            }
            if (types.isDependent(b))
              return record(e, types.unknown());
            MemberResult r = memberOf(b, impl.opSymbols[uint32_t(P::LBracket)], nullptr, i->token, false);
            if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
            {
              bool ambiguous;
              if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypesOf(i->args), &r.subst, b, ambiguous))
                return record(e, returnTypeOfCall(f, &r.subst, i->args, nullptr));
            }
            return record(e, unknown(Why::Operator));
          }
          return record(e, unknown(Why::Operator));
        }
        case NodeKind::MemberExpr: return record(e, typeOfMember(static_cast<MemberExpr*>(e)));
        case NodeKind::UnaryExpr: return record(e, typeOfUnary(static_cast<UnaryExpr*>(e)));
        case NodeKind::BinaryExpr: return record(e, typeOfBinary(static_cast<BinaryExpr*>(e)));
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          const Ty* cond = walkExpr(c->cond);
          const Ty* then = walkExpr(c->then);
          const Ty* other = walkExpr(c->else_);
          if (!c->then)
            return record(e, cond); // Elvis
          const Ty* a = types.stripRef(then);
          const Ty* b = types.stripRef(other);
          if (!a || !b || !a->isKnown())
            return record(e, b && b->isKnown() ? other : types.unknown());
          if (!b->isKnown())
            return record(e, then);
          if (a->kind == TyKind::Nullptr)
            return record(e, other);
          if (b->kind == TyKind::Nullptr)
            return record(e, then);
          if (a->isArithmetic() && b->isArithmetic() && a != b)
            return record(e, arithmetic(a, b));
          // `cond ? "_" : someString`: the class branch's type; the other converts to it.
          if (b->kind == TyKind::Class && a->kind != TyKind::Class)
            return record(e, types.unqualified(b));
          if (a->kind == TyKind::Class && b->kind != TyKind::Class)
            return record(e, types.unqualified(a));
          return record(e, then);
        }
        case NodeKind::CastExpr:
        {
          auto* c = static_cast<CastExpr*>(e);
          walkType(c->type);
          const Ty* operand = walkExpr(c->operand);
          return record(e, probedCast(typeOf(c->type), operand));
        }
        case NodeKind::NamedCastExpr:
        {
          auto* c = static_cast<NamedCastExpr*>(e);
          walkType(c->type);
          const Ty* operand = walkExpr(c->operand);
          const Ty* t = typeOf(c->type);
          return record(e, c->castKind == Kw::static_cast_ ? probedCast(t, operand) : t);
        }
        case NodeKind::SizeofExpr:
          walkType(static_cast<SizeofExpr*>(e)->type);
          walkExpr(static_cast<SizeofExpr*>(e)->operand);
          return record(e, types.sizeType());
        case NodeKind::NewExpr:
        {
          auto* n = static_cast<NewExpr*>(e);
          walkExprs(n->placement);
          walkType(n->type);
          walkExprs(n->args);
          const Ty* t = typeOf(n->type);
          if (t->kind == TyKind::Array)
            t = t->inner;
          return record(e, t->isKnown() ? types.pointer(t) : t);
        }
        case NodeKind::DeleteExpr:
          walkExpr(static_cast<DeleteExpr*>(e)->operand);
          return record(e, types.voidType());
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(e);
          bool generic = !l->templateParams.empty(); // a template: its call operator is typed per call
          std::vector<LocalStack::Item> environment;
          if (generic)
            environment = locals.items; // what the body can see, kept for typing it for each call
          size_t mark = locals.mark();
          for (const LambdaCapture& cap : l->captures)
            if (cap.decl)
              walkLocalDecl(cap.decl); // init-capture: a variable of the closure, typed from its initializer
          pushTemplateParams(l->templateParams);
          std::vector<const Ty*> params;
          for (Decl* p : l->params)
          {
            walkParam(static_cast<ParamDecl*>(p));
            params.push_back(impl.declaredTypeOf(p, activeSubst, 0));
          }
          walkType(l->returnType);
          const Ty* result = nullptr;
          if (l->returnType)
            result = typeOf(l->returnType);
          returnFrames.push_back(nullptr);
          decltypeReturns.push_back(l->returnType && l->returnType->kind == NodeKind::AutoType && static_cast<AutoType*>(l->returnType)->isDecltypeAuto);
          walkStmt(l->body);
          const Ty* deduced = returnFrames.back();
          returnFrames.pop_back();
          decltypeReturns.pop_back();
          if (!result)
            result = deduced ? types.decay(deduced) : types.voidType();
          else if (impl.containsAuto(result))
            result = deduced ? impl.deducedShape(result, deduced) : types.voidType(); // `-> decltype(auto)`, `-> auto&`
          if (generic && (!result->isKnown() || types.isDependent(result)))
            result = nullptr; // deduced per call
          locals.pop(mark);
          // In a template, the lambda is a different type in each instantiation (its body differs).
          // The mark telling them apart is a hash of the arguments as printed, in parameter-name
          // order, so that it (and every name printed with it) is the same in every run.
          int64_t instance = 0;
          if (activeSubst)
          {
            std::vector<std::string> printed;
            for (auto& [d, ty] : activeSubst->pairs)
              printed.push_back(std::string(symbols.name(static_cast<TemplateParamDecl*>(d)->name)) + "=" + types.toString(ty, &symbols));
            std::sort(printed.begin(), printed.end());
            uint64_t h = 1469598103934665603ull;
            for (const std::string& p : printed)
              for (unsigned char c : p + ";")
                h = (h ^ c) * 1099511628211ull;
            instance = int64_t(h ? h : 1);
          }
          const Ty* lambdaTy = types.lambda(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, l->closure, instance);
          if (generic)
          {
            Impl::LambdaEnv env;
            env.lambda = l;
            env.locals = std::move(environment);
            env.scope = scope;
            env.thisType = thisType;
            if (activeSubst)
              env.subst = *activeSubst;
            env.tokens = &tokens;
            env.text = text;
            env.file = file;
            env.depth = instantiationDepth;
            std::lock_guard lock(impl.lambdaEnvMutex);
            impl.lambdaEnvs[lambdaTy] = std::move(env);
          }
          return record(e, lambdaTy);
        }
        case NodeKind::BraceInitExpr:
          walkExprs(static_cast<BraceInitExpr*>(e)->args);
          return record(e, unknown(Why::Unsupported));
        case NodeKind::ThrowExpr:
          walkExpr(static_cast<ThrowExpr*>(e)->operand);
          return record(e, types.voidType());
        case NodeKind::FunctionalCastExpr:
        {
          auto* c = static_cast<FunctionalCastExpr*>(e);
          walkType(c->type);
          const Ty* operand = nullptr;
          for (Expr* a : c->args)
          {
            const Ty* at = walkExpr(a);
            if (a == c->args[0])
              operand = at;
          }
          const Ty* t = typeOf(c->type);
          if (t->kind == TyKind::Auto && c->args.size() == 1 && operand)
            t = types.decay(operand); // auto(x)
          if (c->args.size() == 1 && !c->braced)
            t = probedCast(t, operand); // `bool(instance)` in a requirement
          if (impl.isUndeducedClassTemplate(t))
          {
            // `Pair(a, b)`, `Span(p, n)`, `Pair{a, b}` in an expression: the class's arguments from a
            // constructor, or from an aggregate's members (CTAD).
            Span<Expr*> args = c->args;
            if (c->braced && args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr)
              args = static_cast<BraceInitExpr*>(args[0])->args;
            std::vector<const Ty*> argTypes = argTypesOf(args);
            if (const Ty* deduced = impl.deduceClassArgs(t, argTypes))
              t = deduced;
            else if (const Ty* aggregate = c->braced ? impl.deduceAggregateArgs(t, argTypes) : nullptr)
              t = aggregate;
          }
          return record(e, t);
        }
        case NodeKind::TypeidExpr:
        {
          auto* t = static_cast<TypeidExpr*>(e);
          walkNode(t->operand);
          if (t->operand && uint8_t(t->operand->kind) <= uint8_t(NodeKind::PackType))
          {
            // typeid(Widget), or typeid(q) with q a variable the parser read as a type name.
            auto* n = t->operand->kind == NodeKind::NamedType ? static_cast<NamedType*>(t->operand) : nullptr;
            Lookup l = n ? impl.lookupQualified(n->name, scope, &locals, 0, activeSubst) : Lookup{};
            if (l.decl && (l.decl->kind == NodeKind::VariableDecl || l.decl->kind == NodeKind::ParamDecl) && !(l.decl->flags & DF_Typedef))
            {
              t->operandDecl = l.decl;
              t->operandType = impl.declaredTypeOf(l.decl, activeSubst, 0);
            }
            else
              t->operandType = typeOf(static_cast<Type*>(t->operand));
          }
          if (Decl* info = impl.typeInfoDecl())
            return record(e, types.classType(info, {})); // the library's TypeInfo; the code generator fills in the name
          return record(e, unknown(Why::Unsupported));
        }
        case NodeKind::FoldExpr:
        {
          auto* f = static_cast<FoldExpr*>(e);
          const Ty* l = walkExpr(f->lhs);
          const Ty* r = walkExpr(f->rhs);
          if (f->op == uint32_t(P::Comma))
            return record(e, types.voidType());
          const Ty* pattern = l ? l : r;
          if (pattern && pattern->kind == TyKind::Pack)
            pattern = pattern->args.empty() ? types.voidType() : pattern->args[0];
          return record(e, pattern ? pattern : types.unknown());
        }
        case NodeKind::RequiresExpr:
        {
          // `requires { t.save(); }`: true when every requirement is well-typed for the types at
          // hand, probed on a scratch walker so that the misses count in no statistic.
          auto* r = static_cast<RequiresExpr*>(e);
          ResolveStats scratch;
          Walker probe(impl, scratch, tokens, text);
          probe.scope = scope;
          probe.locals = locals;
          probe.activeSubst = activeSubst;
          probe.thisType = thisType;
          probe.probing = true;
          for (Decl* p : r->params)
            probe.locals.push(static_cast<ParamDecl*>(p)->name, p);
          bool satisfied = true;
          for (Type* t : r->typeRequirements)
            if (const Ty* ty = probe.typeOf(t); !ty || !ty->isKnown())
              satisfied = false;
          for (Expr* requirement : r->requirements)
            if (const Ty* ty = probe.walkExpr(requirement); !ty || !ty->isKnown())
              satisfied = false;
          r->satisfied = satisfied;
          return record(e, types.boolType());
        }
        case NodeKind::ParenExpr:
          return record(e, walkExpr(static_cast<ParenExpr*>(e)->inner));
        default:
          return record(e, unknown(Why::Unsupported));
      }
    }
  };

  // The call operator of a generic lambda for one set of its own template arguments: a copy of
  // the lambda's parameters and body (InstanceCopier::lambdaInstance) typed in the environment the
  // lambda was written in, once per (lambda type, arguments) and kept; body gets the copy.
  // Returns the operator's function type (parameters and result), or null when the environment
  // is unknown, the result has no type, or the lambda is asked for while it is being typed.
  const Ty* checkLambdaInstance(const Ty* lambdaTy, const Subst& own, Subst* fullOut, ResolveStats* statsInto, LambdaExpr** body = nullptr)
  {
    LambdaEnv env;
    {
      std::lock_guard lock(lambdaEnvMutex);
      auto it = lambdaEnvs.find(lambdaTy);
      if (it == lambdaEnvs.end())
        return nullptr;
      env = it->second;
    }
    LambdaExpr* original = env.lambda;
    if (!original || !env.tokens || env.depth > 16)
      return nullptr;
    Subst full = env.subst;
    for (const Subst::Pair& p : own.pairs)
      if (!full.find(p.param))
        full.pairs.push_back(p);
    if (fullOut)
      *fullOut = full;
    bool producer;
    InstanceRecord* inst = claimInstance(original->closure, own, lambdaTy, producer);
    if (!producer)
    {
      if (inst->state.load(std::memory_order_acquire) != 2)
      {
        if (inst->owner == std::this_thread::get_id())
          return nullptr; // the lambda calling itself through a capture
        ++instWaits;
        while (inst->state.load(std::memory_order_acquire) != 2)
          std::this_thread::yield();
      }
      if (body)
        *body = inst->lambda;
      return inst->deducedReturn; // for a lambda: the call operator's type for these arguments
    }
    LambdaExpr* l = InstanceCopier(instanceArena()).lambdaInstance(original);
    inst->lambda = l;
    ++instBodies;
    ResolveStats local;
    if (statsInto)
    {
      local.fileIndex = statsInto->fileIndex;
      local.collectWhy = statsInto->collectWhy;
    }
    Walker sub(*this, local, *env.tokens, env.text);
    sub.file = env.file;
    sub.scope = env.scope;
    sub.thisType = env.thisType;
    sub.activeSubst = &full;
    sub.instantiationDepth = env.depth + 1;
    sub.locals.items = env.locals;
    for (const LambdaCapture& cap : l->captures)
      if (cap.decl)
        sub.walkLocalDecl(cap.decl);
    sub.pushTemplateParams(l->templateParams);
    std::vector<const Ty*> params;
    for (Decl* p : l->params)
    {
      sub.walkParam(static_cast<ParamDecl*>(p));
      const Ty* pt = declaredTypeOf(p, &full, 0);
      if (!(static_cast<ParamDecl*>(p)->isPack && spliceIfPack(pt, params)))
        params.push_back(pt);
    }
    sub.walkType(l->returnType);
    const Ty* result = nullptr;
    if (l->returnType)
      result = sub.typeOf(l->returnType);
    sub.returnFrames.push_back(nullptr);
    sub.walkStmt(l->body);
    const Ty* deduced = sub.returnFrames.back();
    sub.returnFrames.pop_back();
    if (!result)
      result = deduced ? types.decay(deduced) : types.voidType();
    if (statsInto)
    {
      local.instExprsTyped += local.exprsTyped;
      local.instExprsDependent += local.exprsDependent;
      local.instExprsUnknown += local.exprsUnknown;
      statsInto->add(local);
    }
    const Ty* callTy = nullptr;
    bool known = result->isKnown() && !types.isDependent(result);
    for (const Ty* pt : params)
      known = known && pt && pt->isKnown();
    if (known)
      callTy = types.function(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, false);
    inst->deducedReturn = callTy;
    inst->state.store(2, std::memory_order_release);
    if (body)
      *body = l;
    return callTy;
  }

  // The deduced return type of an auto-returning function template for one instantiation,
  // checking its body now if no call has yet.
  const Ty* deducedReturnOf(FunctionDecl* f, const Subst& subst)
  {
    if (!f->file || !f->file->tokens)
      return nullptr;
    ResolveStats scratch;
    Walker w(*this, scratch, *f->file->tokens, f->file->text);
    for (Decl* p = f; p; p = p->parent)
      for (Decl* tp : p->templateParams)
        w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
    w.scope = lexicalScopeOf(f);
    if (f->name.parts.size() > 1)
      w.scope = w.semanticScopeFor(f->name);
    return w.instantiateFunction(f, subst, true);
  }
};

GlobalScope::GlobalScope(SymbolTable& symbols) : impl(new Impl(symbols)) {}
GlobalScope::~GlobalScope() { delete impl; }

void GlobalScope::indexFile(ParsedFile& file)
{
  impl->indexDecls(file.decls, &impl->root);
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
  for (auto& [f, context] : impl->deferredFunctions)
  {
    QualifiedName qualifier = f->name;
    qualifier.parts.count -= 1;
    LocalStack params;
    for (Decl* p : f->templateParams)
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    Lookup l = impl->lookupQualified(qualifier, context, &params, 0);
    if (l.scope && l.scope->kind == ScopeKind::Class)
      if (Scope* special = impl->fullSpecializationScope(qualifier, context, &params, f->file, f->token))
      {
        l.scope = special; // `int Flag<true>::value() {...}`: a member of the specialisation
        impl->specializationMembers[f] = special;
      }
    if (l.scope && (l.scope->kind == ScopeKind::Class || l.scope->kind == ScopeKind::Namespace))
      if (auto it = impl->localTypesOfOutOfLine.find(f); it != impl->localTypesOfOutOfLine.end())
        for (Scope* local : it->second)
          local->parent = l.scope; // a class declared in `void A::f() {...}` sees A's members
    if (l.scope && l.scope->kind == ScopeKind::Class)
      l.scope->outOfLineMembers.push_back(f);
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
      impl->indexDecl(f, l.scope);
    }
  }
  impl->deferredFunctions.clear();
  impl->localTypesOfOutOfLine.clear();
  // Attach out-of-line static data member definitions to the declarations in their classes.
  for (auto& [v, context] : impl->deferredVariables)
  {
    QualifiedName qualifier = v->name;
    qualifier.parts.count -= 1;
    LocalStack params;
    for (Decl* p : v->templateParams)
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    Lookup l = impl->lookupQualified(qualifier, context, &params, 0);
    if (l.scope && l.scope->kind == ScopeKind::Class)
      if (Scope* special = impl->fullSpecializationScope(qualifier, context, &params, v->file, v->token))
        l.scope = special; // `int Flag<true>::counter = 100;`: the specialisation's member
    if (l.scope && l.scope->kind == ScopeKind::Class)
    {
      if (const std::vector<Entry>* entries = l.scope->find(v->name.last().symbol))
        for (const Entry& e : *entries)
          if (e.decl && e.decl->kind == NodeKind::VariableDecl)
          {
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
  std::unordered_set<Scope*> seen;
  impl->sortEntries(&impl->root, seen);
  impl->collectTemplateParamNames(&impl->root);
  // `using enum E;` in classes and namespaces: resolved here, single-threaded, so that the
  // parallel lookups only read them.
  for (Scope* s : impl->scopesWithUsingEnums)
    impl->ensureUsingEnums(s, 0);
  impl->scopesWithUsingEnums.clear();
}

std::vector<GlobalScope::LayoutStaticAssert> GlobalScope::layoutStaticAsserts()
{
  std::lock_guard lock(impl->checkerErrorsMutex);
  std::vector<LayoutStaticAssert> out;
  std::unordered_set<std::string> seen; // an instance's assert met by several walks once
  for (const Impl::LayoutAssert& a : impl->layoutAsserts)
  {
    std::string key = std::to_string(reinterpret_cast<uintptr_t>(a.decl)) + "|" + a.instance;
    if (!seen.insert(key).second)
      continue;
    LayoutStaticAssert& l = out.emplace_back();
    l.decl = a.decl;
    for (const Subst::Pair& p : a.subst.pairs)
      l.args.pairs.push_back({p.param, p.type});
    l.instance = a.instance;
  }
  return out;
}

std::vector<std::string> GlobalScope::checkerErrors()
{
  // The static_asserts in the bodies of class templates, once per instance the program names (a
  // template's own walk leaves them to its instances).
  for (const Ty* cls : impl->types.classInstances())
  {
    if (!cls->decl || cls->decl->kind != NodeKind::ClassDecl || cls->isConst || cls->isVolatile || !cls->isKnown() ||
        impl->types.isDependent(cls) || Impl::mentionsUnknown(cls))
      continue;
    auto* c = static_cast<ClassDecl*>(cls->decl);
    Subst subst = impl->substFor(cls);
    LocalStack params;
    for (Decl* p : Impl::effectiveParams(c))
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    Impl::TypeContext ctx{c->ownScope ? c->ownScope : impl->lexicalScopeOf(c), &params, subst.empty() ? nullptr : &subst,
                          c->file ? c->file->tokens : nullptr, c->file ? c->file->text : std::string_view()};
    for (Decl* m : c->members)
      if (m->kind == NodeKind::StaticAssertDecl)
        impl->checkStaticAssert(static_cast<StaticAssertDecl*>(m), ctx, impl->types.toString(cls, &impl->symbols));
  }
  // By file, then line number, then text.
  auto key = [](const std::string& e)
  {
    size_t error = e.find(": error: ");
    size_t colon = e.rfind(':', error == std::string::npos ? std::string::npos : error - 1);
    if (error == std::string::npos || colon == std::string::npos || colon == 0)
      return std::make_tuple(e, 0L, e);
    return std::make_tuple(e.substr(0, colon), std::strtol(e.c_str() + colon + 1, nullptr, 10), e);
  };
  std::vector<std::string> out(impl->checkerErrorSet.begin(), impl->checkerErrorSet.end());
  std::sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) { return key(a) < key(b); });
  return out;
}

void GlobalScope::resolveFile(ParsedFile& file, const std::vector<Token>& tokens, std::string_view text, ResolveStats& stats)
{
  Impl::Walker walker(*impl, stats, tokens, text);
  walker.file = &file;
  walker.walkDecls(file.decls);
}

TypeTable& GlobalScope::types() { return impl->types; }


FunctionDecl* GlobalScope::findFreeOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context)
{
  std::vector<const Ty*> argTypes{lhs};
  if (rhs)
    argTypes.push_back(rhs);
  return findFree(impl->opSymbols[punctuator], argTypes, context);
}

FunctionDecl* GlobalScope::findFreeFunction(std::string_view name, const std::vector<const Ty*>& argTypes, Decl* context)
{
  return findFree(impl->symbols.intern(name), argTypes, context);
}

FunctionDecl* GlobalScope::findFree(uint32_t symbol, const std::vector<const Ty*>& argTypes, Decl* context)
{
  std::vector<Scope*> places;
  for (const Ty* a : argTypes)
    if (const Ty* t = a ? impl->types.stripRef(a) : nullptr; t && (t->kind == TyKind::Class || t->kind == TyKind::Enum) && t->decl)
      places.push_back(impl->lexicalScopeOf(t->decl));
  places.push_back(impl->lexicalScopeOf(context));
  for (Scope* place : places)
    for (Scope* s = place; s; s = s->parent)
    {
      if (s->kind == ScopeKind::Class)
        continue; // members are not free operators, and do not hide them
      Lookup look;
      if (!impl->lookupInScope(s, symbol, look, 0))
        continue;
      if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
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
        if (FunctionDecl* f = impl->chooseAmong(candidates, argTypes, nullptr, nullptr, ambiguous))
          return f;
      }
      break;
    }
  return nullptr;
}

const Ty* GlobalScope::scalarConversionOf(const Ty* classType) { return impl->scalarConversionOf(classType); }

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
    switch (d->kind)
    {
      case NodeKind::ClassDecl: return static_cast<const ClassDecl*>(d)->isDefinition;
      case NodeKind::EnumDecl: return static_cast<const EnumDecl*>(d)->isDefinition;
      case NodeKind::FunctionDecl: case NodeKind::VariableDecl: case NodeKind::UsingAliasDecl: return true;
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
    return (d->file ? d->file->path : std::string("?")) + ":" + std::to_string(line);
  }
}

std::vector<std::string> GlobalScope::topLevelClashes()
{
  std::vector<std::string> out;
  Scope& root = impl->root;
  for (uint32_t i = 0; i < root.shardCount; ++i)
    for (const auto& [symbol, entries] : root.shards[i].map)
    {
      const Decl* library = nullptr;
      for (const Entry& e : entries)
        if (!library && e.decl->file && e.decl->file->isLibrary && claimsTopLevelName(e.decl) && !fileLocal(e.decl))
          library = e.decl;
      if (!library)
        continue;
      for (const Entry& e : entries)
      {
        const Decl* d = e.decl;
        if (!d->file || d->file->isLibrary || d->file->isImported || !claimsTopLevelName(d) || fileLocal(d))
          continue;
        if (library->kind == NodeKind::FunctionDecl && d->kind == NodeKind::FunctionDecl)
          continue; // overloads of one function
        out.push_back(whereIs(d) + ": error: '" + std::string(impl->symbols.name(symbol)) + "' is declared here and in the standard library (" +
                      whereIs(library) + "); top-level names are global, rename one of them");
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

thread_local const std::vector<Token>* GlobalScope::Impl::fallbackTokens = nullptr;
thread_local bool GlobalScope::Impl::exactLayoutOnly = false;
thread_local bool GlobalScope::Impl::layoutNeeded = false;
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
Decl* GlobalScope::typeInfoDecl() { return impl->typeInfoDecl(); }
Decl* GlobalScope::enumFromStrDecl() { return impl->enumFromStrDecl(); }
Decl* GlobalScope::strongOrderingDecl() { return impl->strongOrderingDecl(); }

Decl* GlobalScope::definitionOf(Decl* d, const TemplateArgs& classArgs)
{
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
  Decl* cls = d && d->kind == NodeKind::FunctionDecl && !classArgs.empty() ? owningClass(d) : nullptr;
  if (cls && cls->ownScope && !cls->templateParams.empty())
  {
    auto* f = static_cast<FunctionDecl*>(d);
    uint32_t symbol = impl->functionSymbol(f->name);
    Subst s;
    for (auto& [p, t] : classArgs.pairs)
      s.pairs.push_back({p, t});
    const Ty* ft = nullptr;
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
      for (Decl* p : cls->templateParams)
        same = same && ws.find(p) && ws.find(p) == s.find(p);
      if (!same)
        continue;
      if (!ft)
        ft = impl->substitute(impl->declaredTypeOf(f, nullptr, 0), s);
      if (impl->declaredTypeOf(candidate, nullptr, 0) != ft)
        continue; // another overload's specialisation
      candidate->flags |= f->flags & (DF_Static | DF_Virtual);
      return candidate;
    }
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
      const Ty* ft = impl->declaredTypeOf(f, nullptr, 0);
      for (FunctionDecl* candidate : cls->ownScope->outOfLineMembers)
        if (impl->functionSymbol(candidate->name) == symbol && !Impl::isMemberSpecialization(candidate) && impl->typeAs(candidate, f) == ft)
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
      const Ty* ft = impl->declaredTypeOf(f, nullptr, 0);
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
  std::string result;
  for (size_t i = parts.size(); i-- > 0;)
    result += parts[i] + "::";
  return result + own(d);
}

bool GlobalScope::isExternC(FunctionDecl* f) { return (f->flags & DF_ExternC) != 0; }

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

const Ty* GlobalScope::typeOfDeclIn(Decl* d, const TemplateArgs& args)
{
  if (args.empty())
    return typeOfDecl(d);
  Subst s = toSubst(args);
  const Ty* t = impl->declaredTypeOf(d, &s, 0);
  if (d->kind == NodeKind::FunctionDecl && t && t->kind == TyKind::Function && impl->containsAuto(t->inner))
  {
    // `template<typename T> auto f(T)`: the return type its body deduced for these arguments.
    if (const Ty* r = impl->deducedReturnOf(static_cast<FunctionDecl*>(d), s))
      return impl->types.function(r, t->args, t->variadic);
  }
  if (impl->containsAuto(t) && d->kind == NodeKind::VariableDecl)
  {
    auto* v = static_cast<VariableDecl*>(d);
    if (v->init && v->init->ty)
      return impl->deduceAuto(t, v->init->ty);
    if (const Ty* cached = v->cachedType.load(std::memory_order_acquire); cached && !impl->containsAuto(cached))
      return cached; // a range-for variable: the element type the loop deduced
  }
  return t;
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
  // A parameter args binds that context does not declare: a for constexpr variable
  // (`sizeof(E)` in `for constexpr (typename E : types<A, B>)`), found by its name too.
  for (const Subst::Pair& p : s.pairs)
    if (p.param && p.param->kind == NodeKind::TemplateParamDecl && !dc.params.find(static_cast<TemplateParamDecl*>(p.param)->name))
      dc.params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  ParsedFile* file = context ? context->file : nullptr;
  Impl::TokenScope tokenScope(file ? file->tokens : nullptr, file ? file->text : std::string_view());
  return impl->convertType(t, Impl::TypeContext{dc.scope, &dc.params, args.empty() ? nullptr : &s, file ? file->tokens : nullptr,
                                                file ? file->text : std::string_view()}, 0);
}

std::optional<bool> GlobalScope::typeTraitValue(NameExpr* e, Decl* context, const TemplateArgs& args)
{
  if (e->name.parts.size() != 1 || !e->name.parts[0].hasTemplateArgs || !impl->isBoolTraitName(e->name.parts[0].symbol))
    return std::nullopt;
  Impl::DeclContext dc;
  impl->declContextFor(context, dc);
  Subst s = toSubst(args);
  ParsedFile* file = context ? context->file : nullptr;
  Impl::TypeContext ctx{dc.scope, &dc.params, args.empty() ? nullptr : &s, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  std::vector<const Ty*> a = impl->traitArgs(e->name.parts[0], ctx, 0);
  bool hasPack = false;
  for (Node* arg : e->name.parts[0].templateArgs)
    hasPack = hasPack || arg->kind == NodeKind::PackType; // `isConstructible<T, Args...>`: any number of types
  if (a.empty() || (!hasPack && a.size() != e->name.parts[0].templateArgs.size()))
    return std::nullopt;
  return impl->traitValue(impl->symbols.name(e->name.parts[0].symbol), a);
}

void GlobalScope::deduceArgs(FunctionDecl* f, const std::vector<const Ty*>& argTypes, TemplateArgs& args)
{
  Subst s = toSubst(args);
  const Ty* ft = impl->declaredTypeOf(f, s.empty() ? nullptr : &s, 0);
  impl->deduceCall(f, ft, argTypes, s);
  for (Decl* p : f->templateParams)
    if (const Ty* t = s.find(p))
    {
      bool known = false;
      for (auto& [d, ty] : args.pairs)
        known = known || d == p;
      if (!known)
        args.pairs.push_back({p, t});
    }
}

FunctionDecl* GlobalScope::chooseOverload(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes,
                                          const TemplateArgs& classArgs, const Ty* objectType, bool* ambiguous)
{
  Subst s = toSubst(classArgs);
  bool amb = false;
  FunctionDecl* f = impl->chooseAmong(candidates, argTypes, classArgs.empty() ? nullptr : &s, objectType, amb);
  if (ambiguous)
    *ambiguous = amb;
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
const Ty* GlobalScope::typeFunctionFor(FunctionDecl* f, FunctionDecl* nodes, const TemplateArgs& args, const Ty* objectType)
{
  Impl& impl = *this->impl;
  if (!f->file || !f->file->tokens)
    return nullptr;
  Subst subst = toSubst(args);
  ResolveStats scratch;
  Impl::Walker w(impl, scratch, *f->file->tokens, f->file->text);
  for (Decl* p = f; p; p = p->parent)
    for (Decl* tp : p->templateParams)
      w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
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
    w.thisType = impl.types.pointer(impl.types.unqualified(impl.types.stripRef(objectType)));
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

void GlobalScope::retypeFor(FunctionDecl* f, const TemplateArgs& args, const Ty* objectType)
{
  typeFunctionFor(f, f, args, objectType);
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
    thisType = impl.types.pointer(impl.types.unqualified(impl.types.stripRef(objectType)));
  else if (Decl* cls = owningClass(f))
    thisType = impl.types.pointer(impl.substitute(impl.selfType(cls), subst));
  bool producer;
  Impl::InstanceRecord* record = impl.claimInstance(f, subst, thisType, producer);
  if (!producer)
  {
    if (record->state.load(std::memory_order_acquire) != 2)
    {
      if (record->owner == std::this_thread::get_id())
        return record->body ? record->body : f; // asked for again while being typed: what exists so far
      while (record->state.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    }
    return record->body ? record->body : f;
  }
  impl.declarationOfDefinition(f); // `static` and `virtual` onto an out-of-line definition before it is copied
  InstanceCopier copier(impl.instanceArena());
  FunctionDecl* copy = copier.function(f);
  record->body = copy;
  record->deducedReturn = typeFunctionFor(f, copy, args, objectType);
  record->state.store(2, std::memory_order_release);
  return copy;
}

bool GlobalScope::isExplicit(FunctionDecl* f, const TemplateArgs& args)
{
  Subst s = toSubst(args);
  return impl->isExplicit(f, args.empty() ? nullptr : &s);
}

Expr* GlobalScope::instanceInitializer(VariableDecl* v, const TemplateArgs& args)
{
  if (args.empty() || !v->init || !v->file || !v->file->tokens)
    return v->init;
  Impl& impl = *this->impl;
  Subst subst = toSubst(args);
  bool producer;
  Impl::InstanceRecord* record = impl.claimInstance(v, subst, nullptr, producer);
  if (!producer)
  {
    if (record->state.load(std::memory_order_acquire) != 2 && record->owner != std::this_thread::get_id())
      while (record->state.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    return record->init ? record->init : v->init; // (on the producing thread while it types: the copy so far)
  }
  InstanceCopier copier(impl.instanceArena());
  Expr* copy = copier.expression(v->init);
  record->init = copy;
  ResolveStats scratch;
  Impl::Walker w(impl, scratch, *v->file->tokens, v->file->text);
  for (Decl* p = v; p; p = p->parent)
    for (Decl* tp : p->templateParams)
      w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
  w.scope = impl.lexicalScopeOf(v);
  if (v->name.parts.size() > 1)
    w.scope = w.semanticScopeFor(v->name);
  w.activeSubst = &subst;
  if (Decl* cls = Impl::enclosingClassDecl(w.scope))
    w.thisType = impl.types.pointer(impl.substitute(impl.selfType(cls), subst));
  w.walkExpr(copy);
  record->state.store(2, std::memory_order_release);
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

size_t GlobalScope::scopeCount() const { return impl->scopes.load(); }
size_t GlobalScope::entryCount() const { return impl->entries.load(); }
size_t GlobalScope::typeCount() const { return impl->types.size(); }

GlobalScope::InstantiationStats GlobalScope::instantiationStats() const
{
  return {impl->instClasses.load(), impl->instFunctions.load(), impl->instBodies.load(), impl->instRequests.load(),
          impl->instWaits.load(), impl->instNanos.load() / 1e9};
}
