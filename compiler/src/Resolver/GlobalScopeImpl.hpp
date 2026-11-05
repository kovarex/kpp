#pragma once
// GlobalScope::Impl, the resolver's state and work (resolver.hpp's GlobalScope), and what its parts share: a
// header since 2026-11-05 (resolver.cpp split, stage 2), the parts each a translation unit of its own as they convert.
#include "resolver.hpp"
#include "consteval.hpp"
#include "instance.hpp"
#include "ast_walk.hpp"
#include "source.hpp"
#include "Restore.hpp"
#include <array>
#include <cctype>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_set>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void __stdcall GetCurrentThreadStackLimits(unsigned long long* lowLimit, unsigned long long* highLimit);
#endif


enum class ScopeKind : uint8_t { Namespace, Class, Enum };

struct Entry
{
  Decl* decl;
  Scope* scope; // Non-null when the declaration opens a scope (namespace, class or enum definition).
};

// --- Dependencies (docs/compile-speed.md, "Design: the persistent compiler", step 2) ----------
// What one unit of the front end's work read: a function body's walk, an instance's, a
// declaration's declared type, a scope's bases and using-directives. Every name it looked up,
// found or not, in whichever scope (a new declaration of that name could change the answer, from
// any file: invalidating by the name alone is coarser than by (scope, name) and recording it is
// much cheaper, 1.3 -> 2.1 s of resolve with the scopes), and what it used that other units computed and cached (a declared type, a scope's relations, a
// class or function instance), and the function bodies the compile-time evaluator ran for it
// (a constexpr function's body edit changes what they computed). A later edit invalidates the
// units that read what it changed.
enum : uint8_t { USE_DeclaredType = 1, USE_ScopeRelations, USE_ClassInstance, USE_Instance, USE_RanBody, USE_ClassMembers };
// Not a use: a checker error a cached answer's computation reported (its text), collected
// (CollectUses) so that the answer's later askers report it too (Impl::replayUses).
enum : uint8_t { USE_ReportedError = 64 };

// A recorded name: a symbol looked up in a namespace's or an enumeration's scope (or coarsely, by
// the symbol alone), or a symbol looked up in a class's scope with that scope (memberName): an edit
// of one class's member is read by the lookups in that class and the classes derived from it, not by
// every lookup of the name. One vector of both keeps a unit's record the size it was (a build in its
// own process records nothing and pays nothing for it).
using RecordedName = uint64_t;
inline RecordedName memberName(uint32_t scopeId, uint32_t symbol) { return (RecordedName(scopeId) + 1) << 32 | symbol; }
inline uint32_t recordedSymbol(RecordedName n) { return uint32_t(n); }
inline bool isMemberName(RecordedName n) { return (n >> 32) != 0; }

struct DependencyList
{
  void tidy()
  {
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::sort(uses.begin(), uses.end());
    uses.erase(std::unique(uses.begin(), uses.end()), uses.end());
  }

  std::vector<RecordedName> names;
  std::vector<std::pair<uint8_t, const void*>> uses;
};

// A unit's dependencies. Several threads may record into one (a declared type two threads compute
// at once): each records into a list of its own, merged when its part of the work ends.
struct Dependencies
{
  // A spin lock: a std::mutex is 80 bytes, in each of 400 k instance records, and the merges are short.
  struct Lock
  {
    void lock() { while (held.exchange(true, std::memory_order_acquire)) std::this_thread::yield(); }
    void unlock() { held.store(false, std::memory_order_release); }

    std::atomic<bool> held{false};
  } mutex;
  void add(DependencyList& part)
  {
    if (part.names.empty() && part.uses.empty())
      return;
    std::lock_guard lock(mutex);
    list.names.insert(list.names.end(), part.names.begin(), part.names.end());
    list.uses.insert(list.uses.end(), part.uses.begin(), part.uses.end());
    tidied = false; // tidied when read (a declared type nobody caches is computed, and merged, again and again)
  }
  DependencyList& read()
  {
    if (!tidied)
      list.tidy();
    tidied = true;
    return list;
  }

  DependencyList list;
  bool tidied = true;
};

// Whether this build records dependencies: a compile server's builds do (the incremental builds
// after them use them), a build in its own process does not (it would only pay for them: 10 to 17%
// of a single thread's resolve).
inline bool recordingDependencies = false;

// The list this thread's lookups are recorded into (null: none).
inline thread_local DependencyList* recordingInto = nullptr;

// The unit whose checker errors this thread reports (its dependencies; null: none): a unit walked or
// checked again takes back what it reported (Impl::forgetErrorsOf), one that is not keeps it.
inline thread_local const void* reportingUnit = nullptr;

// The file this thread's lookups are made for (null: none known): its private names (design.md 15)
// are found before the global ones. Set by what walks or types a declaration (FileOfLookups).
inline thread_local const ParsedFile* lookupFile = nullptr;

namespace resolver_detail
{
  struct FileOfLookups
  {
    explicit FileOfLookups(const ParsedFile* file) : saved(lookupFile)
    {
      if (file)
        lookupFile = file;
    }
    ~FileOfLookups() { lookupFile = saved; }
    FileOfLookups(const FileOfLookups&) = delete;

    const ParsedFile* saved;
    FileOfLookups& operator=(const FileOfLookups&) = delete;
  };
}
using namespace resolver_detail;

// A cached computation's reads, collected (CollectUses): kind 0 is a name (its RecordedName in
// what's bits), else a use.
using CollectedUse = std::pair<uint8_t, const void*>;
inline thread_local std::vector<CollectedUse>* usesCollector = nullptr;

// A lookup visits a chain of scopes with one symbol, and a walk asks for one declaration's type
// many times in a row: what was recorded last is not recorded again (tidy() drops the other
// repeats). A unit starting or ending forgets it, so that nothing is taken as recorded that the
// current unit did not record.
inline thread_local uint32_t lastName = UINT32_MAX;
inline thread_local const void* lastUse = nullptr;

inline void collectName(RecordedName n)
{
  if (usesCollector)
    usesCollector->push_back({0, reinterpret_cast<const void*>(uintptr_t(n))});
}

inline void recordName(uint32_t symbol)
{
  if (!recordingDependencies) // (a build in its own process: nothing to look at, not even a thread's state)
    return;
  collectName(symbol);
  DependencyList* r = recordingInto;
  if (!r || symbol == lastName)
    return;
  lastName = symbol;
  r->names.push_back(symbol);
}

// A lookup in a class's scope: recorded with the scope (memberName). The last one is not recorded
// again (a walk looks one member up in one class many times in a row).
inline thread_local RecordedName lastMember = 0;
inline void recordMember(uint32_t scopeId, uint32_t symbol)
{
  if (!recordingDependencies)
    return;
  DependencyList* r = recordingInto;
  RecordedName n = memberName(scopeId, symbol);
  collectName(n);
  if (!r || n == lastMember)
    return;
  lastMember = n;
  r->names.push_back(n);
}

// Uses repeat less in a row than names (a walk asks for the types of the declarations it meets in
// turn): the last 4096 the current unit recorded are remembered, by address and kind (a function's
// declared type and its body run are two uses of one address).
struct RecentUses
{
  const void* what[4096] = {};
  uint32_t unit[4096] = {};
};
inline thread_local std::unique_ptr<RecentUses> recentUses;
inline thread_local uint32_t recordingUnit = 0, unitCounter = 0;

inline void recordUse(uint8_t kind, const void* what)
{
  if (!recordingDependencies)
    return;
  if (usesCollector)
    usesCollector->push_back({kind, what});
  DependencyList* r = recordingInto;
  // (what is a declaration, a scope or a record: aligned, so the kind fits in its low bits)
  const void* key = reinterpret_cast<const char*>(what) + kind;
  if (!r || key == lastUse)
    return;
  lastUse = key;
  size_t slot = (reinterpret_cast<uintptr_t>(key) >> 4) % 4096;
  if (recentUses->what[slot] == key && recentUses->unit[slot] == recordingUnit)
    return;
  recentUses->what[slot] = key;
  recentUses->unit[slot] = recordingUnit;
  r->uses.push_back({kind, what});
}

#include "RecordInto.hpp"

// What a cached answer's computation recorded (the evaluator's caches, the trait answers, the
// specialisation cache): a later asker the cache answers read it too, so it is replayed into the
// asker's unit (replayUses). Nested: what an inner collection gathers belongs to the outer one as
// well. Only while recording.
struct CollectUses
{
  CollectUses()
  {
    if (recordingDependencies)
      usesCollector = &uses;
  }
  ~CollectUses()
  {
    if (!recordingDependencies)
      return;
    usesCollector = saved;
    if (saved)
      saved->insert(saved->end(), uses.begin(), uses.end());
  }
  CollectUses(const CollectUses&) = delete;

  std::vector<CollectedUse> uses;
  std::vector<CollectedUse>* saved = usesCollector;
  CollectUses& operator=(const CollectUses&) = delete;
};

// A cached answer's collected reads, replayed into the asker's unit (and an outer collection).
// (Impl::replayUses: a body run tells the code generator's hooks too.)
inline void replayCollected(const std::vector<CollectedUse>& uses)
{
  if (!recordingInto && !usesCollector)
    return;
  for (const auto& [kind, what] : uses)
    if (kind == 0)
    {
      RecordedName n = RecordedName(reinterpret_cast<uintptr_t>(what));
      collectName(n);
      if (recordingInto)
        recordingInto->names.push_back(n);
    }
    else if (kind != USE_ReportedError) // (Impl::replayUses reports that, which collects it again)
      recordUse(kind, what);
}

// A class's member list read by position (aggregate initialisation, traits, layouts, reflection,
// implicit members), not through a name: recorded (USE_ClassMembers), so that an edit of the class's
// data members finds what read it.
// The classes whose member lists were read outside any unit while the files were resolved (or a
// compile server's edit was checked again): the build's own set (Impl::membersReadOutside) while
// its resolve runs, else none. An edit of one of their data members is built from scratch.
inline std::unordered_set<const void*>* membersOutsideSink = nullptr;
inline std::mutex membersOutsideMutex;

template<typename C>
inline Span<Decl*> membersRead(C* c)
{
  if (!recordingDependencies)
    return c->members;
  recordUse(USE_ClassMembers, c);
  if (!recordingInto && membersOutsideSink && recordingDependencies)
  {
    std::lock_guard lock(membersOutsideMutex);
    membersOutsideSink->insert(c);
  }
  return c->members;
}

#include "Scope.hpp"

namespace resolver_detail
{
#include "LocalStack.hpp"

  enum class Found : uint8_t { No, Local, Member, Global, Dependent };

  struct Lookup
  {
    Found found = Found::No;
    Decl* decl = nullptr;
    Scope* scope = nullptr;                        // Scope opened by decl, if any.
    const std::vector<Entry>* overloads = nullptr; // All entries with that name in the scope where it was found.
    Scope* foundIn = nullptr;                      // Scope the name was found in.
    Decl* ambiguousWith = nullptr; // another entity the name equally names (through a using-directive): an error
    Scope* ambiguousIn = nullptr;  // set with ambiguousWith when two bases of a class have the name: the other base's scope
    Decl* absent = nullptr;        // not found: the name is a false `when` declaration's in foundIn (design.md 19)
  };

#include "InlineVector.hpp"

  // Template parameter -> type, for members seen through an instantiation like Vector<int>.
  struct Subst
  {
    struct Pair
    {
      Decl* param;
      const Ty* type;
    };
    const Ty* find(Decl* param) const
    {
      for (const Pair& p : pairs)
        if (p.param == param)
          return p.type;
      return nullptr;
    }
    bool empty() const { return pairs.empty(); }

    InlineVector<Pair, 4> pairs;
  };

  inline bool isAllCaps(std::string_view s)
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
using namespace resolver_detail;

struct GlobalScope::Impl
{
#include "GlobalScopeImpl.inl"
#include "Indexer.hpp"
#include "Lookup.hpp"
#include "TypeOfAst.hpp"
#include "AccessChecker.hpp"
#include "Interpreter.hpp"
#include "Instantiation.hpp"
#include "OverloadResolution.hpp"
#include "Walker.hpp"
#include "InstanceTasks.hpp"
};
