// The build: a kept session, the levels of instantiations on the emitters' threads, assembling the
// modules (generateLLVM, rebuildLLVM). A translation unit of its own since 2026-11-05: what
// codegen.cpp held after the emitters, and its prologue (codegen.cpp split, stage 2), moved without
// a change.
#include "Emitter.hpp"
#include "host.hpp"
#ifdef _WIN32
// SEH wrapper: catches stack overflow that C++ try/catch can't.
// Returns 0 on success, 1 on stack overflow.
static int callWithStackGuard(void(*fn)(void*), void* arg)
{
  __try { fn(arg); return 0; }
  __except(GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
  { _resetstkoflw(); return 1; }
}
#endif

// Object model of the v0 code generator:
// - Scalars are SSA values. Class objects always live in memory; a Value of class type holds the
//   object's address.
// - A class parameter is passed as a pointer to a caller-made copy that the callee owns and
//   destroys. A class return value is constructed into a hidden `sret` slot provided by the
//   caller.
// - Locals (and temporaries) of class types that need destruction are destroyed at the end of
//   their scope, in reverse order; `return`, `break` and `continue` destroy the scopes they leave.
// - Templates: a struct layout exists per instantiation type; an instantiated function is
//   emitted on first use from a worklist, after the type checker re-typed its body for the
//   concrete arguments. All declaration types are read through declType(), which applies the
//   arguments of the instantiation being emitted (or of the class whose metadata is inspected).

using namespace codegen;

// What a build keeps for the next (CodegenSessionPtr): the shared state the emitters wrote, the
// emitters, and what assembling a module needs.
struct CodegenSession
{
  CodegenResult result; // the build's counts (and the result Shared refers to)
  std::unique_ptr<Shared> shared;
  std::vector<std::unique_ptr<Emitter>> emitters;
  std::string prefix;   // a module's first lines
  unsigned threads = 1;
  bool usable = false;  // a build of several modules without errors, its texts kept
};

void CodegenSessionDeleter::operator()(CodegenSession* session) const
{
  delete session;
}

static CodegenResult generate(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols,
                              const CodegenOptions& options, CodegenSessionPtr* keep, PieceStream* stream);

// Runs work(i) for i in [0, n): the calling thread runs work(0), n - 1 threads with 1 GB stacks
// (reserved, committed as used: the emitter recurses as deep as the code it walks) the rest.
// (on every host: a Linux one's default 8 MB stacks are less than the game's deepest code needs)
template<typename F>
static void onThreads(unsigned n, F& work)
{
  struct Arg
  {
    F* work;
    unsigned i;
  };
  std::vector<Arg> args(n);
  std::vector<host::LargeStackThread> threads;
  std::vector<unsigned> unstarted; // (run here after work(0): never left out)
  for (unsigned i = 1; i < n; ++i)
  {
    args[i] = {&work, i};
    host::LargeStackThread t;
    if (host::startOnLargeStack(size_t(1) << 30, [](void* p)
    {
      auto* a = static_cast<Arg*>(p);
      (*a->work)(a->i);
    }, &args[i], t))
      threads.push_back(t);
    else
      unstarted.push_back(i);
  }
  work(0);
  for (unsigned i : unstarted)
    work(i);
  for (host::LargeStackThread& t : threads)
    host::joinThread(t);
}

// --stub-missing: the definition of a function that was left out, which reports it and aborts when
// called (ret and params as its declaration has them, or `void` and `()` for one known by name only:
// a call names it with its own type). In every module that uses it: linkonce_odr.
std::string codegen::stubText(const std::string& name, const std::string& ret, const std::string& params, const std::string& message)
{
  std::string inner = name.size() > 2 && name[1] == '"' ? name.substr(2, name.size() - 3) : name.substr(1);
  std::string bytes = message + '\0', escaped;
  for (unsigned char c : bytes)
    if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
      escaped += char(c);
    else
    {
      char buf[8];
      snprintf(buf, sizeof buf, "\\%02X", c);
      escaped += buf;
    }
  std::string constant = "@\"kpp.missing:" + inner + "\"";
  return "$" + name.substr(1) + " = comdat any\n" + constant + " = private unnamed_addr constant [" + std::to_string(bytes.size()) + " x i8] c\"" +
         escaped + "\"\ndefine linkonce_odr " + ret + " " + name + params + " comdat {\nentry:\n  call void @kpp_missing(ptr " + constant +
         ")\n  unreachable\n}\n";
}

// The global names a module's text uses without a definition or declaration (functions an error
// left out, with --keep-going), each declared as `declare void @name()`: with opaque pointers a
// call may name a function of another type, so the module compiles and the link reports the name.
// With stubs (--stub-missing), each is defined as a stub instead (stubText), its message the one
// stubs gives it or its name.
static std::string declareUndefined(const std::string& text, const std::unordered_map<std::string, std::string>* stubs = nullptr,
                                    std::unordered_set<std::string>* stubbed = nullptr)
{
  auto nameAt = [&](size_t i) -> std::string
  {
    size_t end;
    if (i + 1 < text.size() && text[i + 1] == '"')
    {
      end = text.find('"', i + 2);
      end = end == std::string::npos ? text.size() : end + 1;
    }
    else
    {
      end = i + 1;
      while (end < text.size() && (isalnum((unsigned char)text[end]) || strchr("-$._", text[end])))
        ++end;
    }
    return text.substr(i, end - i);
  };
  std::unordered_set<std::string> known, used;
  for (size_t line = 0; line < text.size();)
  {
    size_t next = text.find('\n', line);
    next = next == std::string::npos ? text.size() : next + 1;
    if (text.compare(line, 7, "define ") == 0 || text.compare(line, 8, "declare ") == 0)
    {
      size_t at = text.find(" @", line);
      if (at != std::string::npos && at < next)
        known.insert(nameAt(at + 1));
    }
    else if (text[line] == '@')
      known.insert(nameAt(line)); // `@name = ... global ...`
    line = next;
  }
  // A quoted name or string (`@"f::__lambda_1_2@10834::operator()()"`, c"name@factorio.com") and a
  // comment are skipped whole: an '@' inside them names nothing.
  for (size_t i = text.find_first_of("\"@;"); i != std::string::npos; i = text.find_first_of("\"@;", i + 1))
  {
    if (text[i] == '"' || text[i] == ';')
    {
      i = text.find(text[i] == '"' ? '"' : '\n', i + 1);
      if (i == std::string::npos)
        break;
      continue;
    }
    std::string name = nameAt(i);
    if (name.size() > 1 && name.rfind("@llvm.", 0) != 0 && !known.count(name))
      used.insert(name);
    i += name.size() - 1;
  }
  // The stubs' runtime is a shared function, which a debug build of several modules defines in one
  // home module: the others declare it for their stubs (made after their declarations were, or in
  // their text already: a program piece's), never stub it. Taken for a function left out, it was
  // defined twice there, `declare void @kpp_missing(ptr)` and a stub `define ... @kpp_missing()`, and
  // clang rejected the module (spec language/stub_missing_in_several_modules).
  bool usesRuntime = used.erase("@kpp_missing") > 0;
  std::vector<std::string> sorted(used.begin(), used.end());
  std::sort(sorted.begin(), sorted.end());
  std::string out = sorted.empty() && !usesRuntime ? "" : "\n; left out by codegen errors (--keep-going)\n";
  if (((stubs && !sorted.empty()) || usesRuntime) && !known.count("@kpp_missing"))
    out += "declare void @kpp_missing(ptr)\n";
  for (const std::string& name : sorted)
    if (stubs)
    {
      auto it = stubs->find(name);
      out += stubText(name, "void", "()", it != stubs->end() ? it->second : name);
      if (stubbed)
        stubbed->insert(name);
    }
    else
      out += "declare void " + name + "()\n";
  return out;
}

// A module's own definitions of string literals the shared declarations (which every module carries)
// came to define too, after the module's: dropped (Emitter::constantName).
static void dropSharedLiterals(Shared& sh, std::vector<std::string>& entries)
{
  std::erase_if(entries, [&](const std::string& e) {
    if (!e.starts_with("$.str.") && !e.starts_with("$.wstr.") && !e.starts_with("$.data."))
      return false;
    std::string name = "@" + e.substr(1, e.find(" = comdat") - 1);
    return sh.literalShard(name).shared.count(name) != 0;
  });
}

unsigned codegenOutputModules(unsigned moduleCount, bool debug, bool optimize)
{
  return moduleCount > 1 && debug && !optimize ? 2 * moduleCount : moduleCount;
}

CodegenResult generateLLVM(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols,
                           const CodegenOptions& options, CodegenSessionPtr* keep, PieceStream* stream)
{
  // The emitter recurses as deep as the code it walks (long else-if chains, nested expressions),
  // so it runs on a thread with a 1 GB stack, reserved and committed only as it is used (on every
  // host: Linux's 8 MB main stack is less than the game needs).
  struct Job
  {
    std::vector<std::unique_ptr<ParsedFile>>* files;
    GlobalScope* global;
    const SymbolTable* symbols;
    const CodegenOptions* options;
    CodegenSessionPtr* keep;
    PieceStream* stream;
    CodegenResult result;
  } job{&files, &global, &symbols, &options, keep, stream, {}};
  host::LargeStackThread thread;
  if (host::startOnLargeStack(size_t(1) << 30, [](void* p)
  {
    auto* j = static_cast<Job*>(p);
    j->result = generate(*j->files, *j->global, *j->symbols, *j->options, j->keep, j->stream);
  }, &job, thread))
  {
    host::joinThread(thread);
    return std::move(job.result);
  }
  return generate(files, global, symbols, options, keep, stream);
}

// --stub-missing: the functions used without a definition that declsFor defines as stubs.
static size_t countUndefinedStubs(Shared& sh)
{
  size_t n = 0;
  if (sh.stubMissing)
    for (auto& [name, f] : sh.undefinedUses)
      n += !sh.definedIn.count(name);
  return n;
}

// emitFunction with the stack overflow caught. With a 1 GB stack an overflow means recursion
// without end; the fault may have struck inside the allocator, leaving the heap mid-update (a
// later allocation then spins forever), so nothing is allocated after it: the function's name
// goes out through a fixed buffer and the process ends.
static void emitInstantiation(Emitter& em, const Instantiation& item)
{
  if (item.depth > Emitter::maxInstantiationDepth)
  {
    em.reportTooDeep(item);
    return;
  }
  em.currentModule = item.module;
  em.currentRequestedBy = item.requestedBy;
  em.currentRequestedByContext = item.requestedByContext;
  em.currentDepth = item.depth;
  struct EmitCall { Emitter* e; const Instantiation* item; };
  EmitCall call{&em, &item};
#ifdef _WIN32
  if (callWithStackGuard([](void* p) {
    auto* c = static_cast<EmitCall*>(p);
    c->e->emitFunction(c->item->f, c->item->args, c->item->classTy, c->item->name);
  }, &call))
  {
    static char message[1024];
    int n = snprintf(message, sizeof message, "codegen: out of stack (recursion without end) in %.900s\n", em.currentFunctionName.c_str());
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, DWORD(n > 0 ? std::min(n, int(sizeof message) - 1) : 0), &written, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
  }
#else
  em.emitFunction(item.f, item.args, item.classTy, item.name);
#endif
  em.currentRequestedBy.clear();
  em.currentDepth = 0;
}

// The next level: what the emitters asked for (each function once: the names were claimed), in
// the order of their names, each with its first requester by name over all emitters
// (Emitter::levelRequests). A large one is collected on the emitters' threads (collectInParallel).
static bool collectInParallel(std::vector<std::unique_ptr<Emitter>>& emitters, std::vector<Instantiation>& level);
static void collectNextLevel(std::vector<std::unique_ptr<Emitter>>& emitters, std::vector<Instantiation>& level)
{
  level.clear();
  for (auto& em : emitters)
  {
    for (Instantiation& item : em->worklist)
      level.push_back(std::move(item));
    em->worklist.clear();
  }
  if (collectInParallel(emitters, level))
    return;
  std::unordered_map<std::string, Request*> first;
  for (const Instantiation& item : level)
    first.emplace(item.name, nullptr);
  for (auto& em : emitters)
    for (auto& [name, r] : em->levelRequests)
      if (auto it = first.find(name); it != first.end() && (!it->second || r.by < it->second->by))
        it->second = &r;
  for (Instantiation& item : level)
    if (Request* r = first[item.name])
    {
      item.requestedBy = r->by;
      item.requestedByContext = r->byContext;
      item.depth = r->depth;
    }
  for (auto& em : emitters)
    em->levelRequests.clear();
  std::sort(level.begin(), level.end(), [](const Instantiation& a, const Instantiation& b) { return a.name < b.name; });
}

// collectNextLevel's work for a level of at least 4,096 functions, on as many threads as there are emitters, with
// its result: the emitters' requests scanned and freed each on its emitter's thread, the level's chunks given their
// items' first requesters and sorted each on a thread of its own, then merged. On one thread the game's collections
// took 513 to 537 ms between its levels, with the other 31 cores idle (42,420 items: 157 ms, 103 ms of it a map of
// the names copied). This way costs about 5 ms of its own (three thread starts, the index, the merge), the serial
// one 2.5 us an item. False (nothing done) for a smaller level, and for a name twice or an empty one, whose order
// only the serial way gives.
static bool collectInParallel(std::vector<std::unique_ptr<Emitter>>& emitters, std::vector<Instantiation>& level)
{
  const size_t n = level.size();
  const unsigned threads = unsigned(emitters.size());
  if (threads < 2 || n < 4096 || n >= UINT32_MAX)
    return false;
  std::unordered_map<std::string_view, uint32_t> index; // the names, each item's (views into level: it stays put until the merge)
  index.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
    if (level[i].name.empty() || !index.emplace(level[i].name, i).second)
      return false;
  // Chunk c is level[bounds[c], bounds[c + 1]).
  std::vector<size_t> bounds(threads + 1);
  for (unsigned c = 0; c <= threads; ++c)
    bounds[c] = size_t(c) * n / threads;
  auto chunkOf = [&](uint32_t i) { return unsigned(std::upper_bound(bounds.begin(), bounds.end(), size_t(i)) - bounds.begin() - 1); };
  // Each emitter's requests of the level's names, by the chunk of the item named.
  std::vector<std::vector<std::vector<std::pair<uint32_t, const Request*>>>> found(threads, std::vector<std::vector<std::pair<uint32_t, const Request*>>>(threads));
  auto scan = [&](unsigned e)
  {
    for (auto& [name, r] : emitters[e]->levelRequests)
      if (auto it = index.find(name); it != index.end())
        found[e][chunkOf(it->second)].push_back({it->second, &r});
  };
  onThreads(threads, scan);
  // Each chunk: its items' first requesters by name (over the emitters in their order, as the serial way takes
  // them: of two equal ones, the first emitter's), then its items sorted by name.
  auto byName = [](const Instantiation& a, const Instantiation& b) { return a.name < b.name; };
  auto merge = [&](unsigned c)
  {
    std::vector<const Request*> best(bounds[c + 1] - bounds[c], nullptr);
    for (unsigned e = 0; e < threads; ++e)
      for (auto [i, r] : found[e][c])
        if (const Request*& b = best[i - bounds[c]]; !b || r->by < b->by)
          b = r;
    for (size_t i = bounds[c]; i < bounds[c + 1]; ++i)
      if (const Request* r = best[i - bounds[c]])
      {
        level[i].requestedBy = r->by;
        level[i].requestedByContext = r->byContext;
        level[i].depth = r->depth;
      }
    std::sort(level.begin() + bounds[c], level.begin() + bounds[c + 1], byName);
  };
  onThreads(threads, merge);
  // The requests and what pointed into them, freed on the emitters' threads.
  auto release = [&](unsigned e)
  {
    emitters[e]->levelRequests.clear();
    std::vector<std::vector<std::pair<uint32_t, const Request*>>>().swap(found[e]);
  };
  onThreads(threads, release);
  // The sorted chunks merged (the names are unique: one order, the serial sort's).
  std::vector<Instantiation> merged;
  merged.reserve(n);
  std::vector<size_t> at(bounds.begin(), bounds.end() - 1);
  auto later = [&](unsigned a, unsigned b) { return level[at[a]].name > level[at[b]].name; };
  std::priority_queue<unsigned, std::vector<unsigned>, decltype(later)> heads(later);
  for (unsigned c = 0; c < threads; ++c)
    if (at[c] < bounds[c + 1])
      heads.push(c);
  while (!heads.empty())
  {
    unsigned c = heads.top();
    heads.pop();
    merged.push_back(std::move(level[at[c]]));
    if (++at[c] < bounds[c + 1])
      heads.push(c);
  }
  level = std::move(merged);
  return true;
}

// Every name the build's texts define so far, each with the line defining it (a global's) or empty
// (a function's): the functions defined (definedIn), and the globals the shared declarations, the
// shared definitions and each module's own declarations define (not `external` ones). For the
// barrier check (Shared::checkBarrier), at the barrier and at the end.
static std::unordered_map<std::string, std::string> definedNames(const Shared& sh)
{
  std::unordered_map<std::string, std::string> names;
  for (const auto& [name, module] : sh.definedIn)
    names.emplace(name, std::string());
  auto globalsIn = [&](std::string_view text)
  {
    for (size_t pos = 0; pos < text.size();)
    {
      size_t end = text.find('\n', pos);
      if (end == std::string_view::npos)
        end = text.size();
      std::string_view line = text.substr(pos, end - pos);
      pos = end + 1;
      size_t eq = line.find(" = ");
      if (line.empty() || line[0] != '@' || eq == std::string_view::npos || line.substr(eq + 3).starts_with("external "))
        continue;
      names.emplace(std::string(line.substr(0, eq)), std::string(line));
    }
  };
  for (const std::string& entry : sh.decls.entries)
    globalsIn(entry);
  for (const std::string& entry : sh.shared.entries)
    globalsIn(entry);
  for (const SharedText& own : sh.moduleDecls)
    for (const std::string& entry : own.entries)
      globalsIn(entry);
  for (const std::vector<Shared::OwnedEntry>& owned : sh.ownedEntries)
    for (const Shared::OwnedEntry& o : owned)
      globalsIn(o.text);
  return names;
}

// The function an entry of the shared definitions defines, when that is all it holds (its comdat
// line, then one linkonce_odr definition: a destructor helper `@kpp.dtor.<class>`, a runtime
// function such as `@kpp_throw`), with its header; an empty name for any other entry.
static std::string sharedFunctionIn(const std::string& entry, std::string& header)
{
  size_t at = entry.starts_with("define ") ? 0 : entry.find("\ndefine ");
  if (at == std::string::npos)
    return {};
  at += entry[at] == '\n';
  if (at != 0 && !(entry[0] == '$' && entry.find('\n') + 1 == at))
    return {}; // (something before the definition besides its comdat line)
  if (!std::string_view(entry).substr(at).starts_with("define linkonce_odr ") || entry.find("\ndefine ", at) != std::string::npos ||
      entry.find("\n@", at) != std::string::npos)
    return {};
  header = entry.substr(at, entry.find('\n', at) - at);
  size_t name = header.find(" @");
  if (name == std::string::npos)
    return {};
  size_t nameEnd = header[name + 2] == '"' ? header.find('"', name + 3) + 1 : header.find('(', name);
  return nameEnd == std::string::npos ? std::string() : header.substr(name + 1, nameEnd - name - 1);
}

// The program functions the emitters emitted since they were last gathered (Shared::programFunctions).
static void gatherProgramFunctions(Shared& sh, std::vector<std::unique_ptr<Emitter>>& emitters)
{
  for (auto& em : emitters)
  {
    sh.programFunctions.insert(em->programFunctionsNow.begin(), em->programFunctionsNow.end());
    em->programFunctionsNow.clear();
  }
}

// Module k's owned entries for its declarations (a program function's go in its program piece), each
// text once: a shared static local's definition is an entry of every function naming it
// (Emitter::addSharedStatic).
static void appendOwnedEntries(const Shared& sh, unsigned k, std::vector<std::string>& entries)
{
  std::unordered_set<std::string_view> seen;
  for (const Shared::OwnedEntry& o : sh.ownedEntries[k])
    if (!sh.programOwned(o.owner) && seen.insert(o.text).second)
      entries.push_back(o.text);
}

// What program pieces are assembled from besides their module's own texts (programPieceText): the
// units (a line, or a definition with its body: Emitter::forEachLine) of the shared declarations and
// of the shared definitions every module carries, by the name each defines or declares, and the type
// lines by type name. At level 0's barrier from the entries written so far, less the shared functions
// the end homes in one module (pieces declare those); at the end from the finished texts. A unit is
// one text in both.
struct PieceSources
{
  std::unordered_map<std::string_view, std::vector<std::string_view>> decls, shared;
  std::unordered_map<std::string_view, std::string_view> types;
  bool atBarrier = false;
};

static PieceSources pieceSources(const Shared& sh, bool atBarrier)
{
  PieceSources sources;
  sources.atBarrier = atBarrier;
  auto add = [&](std::unordered_map<std::string_view, std::vector<std::string_view>>& index, std::string_view text)
  {
    Emitter::forEachLine(text, [&](std::string_view unit)
    {
      if (Emitter::isTypeLine(unit))
        sources.types.emplace(unit.substr(0, unit.find(" = type ")), unit);
      else if (std::string_view name = Emitter::definedBy(unit); !name.empty())
        index[name].push_back(unit);
    });
  };
  if (atBarrier)
  {
    for (const std::string& entry : sh.decls.entries)
      add(sources.decls, entry);
    for (const std::string& entry : sh.shared.entries)
      if (std::string header; sharedFunctionIn(entry, header).empty())
        add(sources.shared, entry);
  }
  else
  {
    add(sources.decls, sh.decls.text);
    add(sources.shared, sh.shared.text);
  }
  return sources;
}

// Module k's program piece (Shared::splitPieces): the definitions its program functions own and their
// owned entries (with sources.atBarrier, those written before level 0's barrier), and what they name,
// closed over what that names in turn, each from a text that does not change once written:
// - a string literal: a copy of its definition from its contents, whether the module or the shared
//   declarations define it (a module's literal goes to those at the end when another module uses it);
// - a unit of the module's own declarations, of the shared definitions or of the shared declarations:
//   copied (a vtable and its thunks, a type descriptor, a declare, an internal constant, `@kpp.dyncast`),
//   except a global the shared declarations home in one module (an instance piece), declared external;
// - a function defined in another text (another module, its instance piece, a later level, a shared
//   function homed at the end) or the runtime's global: its declaration as its definition gives it
//   (declareLines) or its first request did (requestDeclarations), the same by the barrier check.
// Then the type lines all of it names, closed over theirs. Each part sorted: one text at the barrier,
// at the end and in a server's build. A name none of these has is left alone (an '@' inside a string
// names nothing; LLVM reports a real one). errors gets a name private to the instance piece (one of its
// static locals) that the piece names; complete is false when a type it names has no line in sources
// (at the barrier: one laid out only later, Shared::latePieces).
static std::string programPieceText(const Shared& sh, unsigned k, const std::string& prefix, const PieceSources& sources, std::string* errors,
                                    bool* complete = nullptr)
{
  const bool atBarrier = sources.atBarrier;
  std::vector<const Shared::Definition*> definitions;
  const std::vector<Shared::Definition>& all = sh.definitions[k];
  for (size_t i = 0, n = atBarrier ? sh.barrierCounts[k] : all.size(); i < n; ++i)
    if (sh.programOwned(all[i].owner))
      definitions.push_back(&all[i]);
  std::sort(definitions.begin(), definitions.end(), [](const Shared::Definition* a, const Shared::Definition* b) { return a->text < b->text; });
  std::vector<std::string_view> owned;
  const std::vector<Shared::OwnedEntry>& entries = sh.ownedEntries[k];
  for (size_t i = 0, n = atBarrier ? sh.barrierOwned[k] : entries.size(); i < n; ++i)
    if (sh.programOwned(entries[i].owner))
      owned.push_back(entries[i].text);
  std::sort(owned.begin(), owned.end());
  owned.erase(std::unique(owned.begin(), owned.end()), owned.end()); // (a shared static local's, one per function naming it)
  // What the piece defines (by name, without the sigil), and the module's own declarations by name.
  std::unordered_set<std::string_view> defined;
  for (const Shared::Definition* d : definitions)
    if (!d->name.empty())
      defined.insert(std::string_view(d->name).substr(1));
  for (std::string_view entry : owned)
    Emitter::forEachLine(entry, [&](std::string_view unit)
    {
      if (std::string_view name = Emitter::definedBy(unit); !name.empty())
        defined.insert(name);
    });
  std::unordered_map<std::string_view, std::vector<std::string_view>> own;
  auto addOwn = [&](std::string_view text)
  {
    Emitter::forEachLine(text, [&](std::string_view unit)
    {
      if (std::string_view name = Emitter::definedBy(unit); !name.empty())
        own[name].push_back(unit);
    });
  };
  if (atBarrier)
    for (const std::string& entry : sh.moduleDecls[k].entries)
      addOwn(entry);
  else
    addOwn(sh.moduleDecls[k].text);
  std::vector<std::string_view> work, typeWork, lines, declarations;
  std::deque<std::string> made; // the texts written here (literals, external declarations)
  auto uses = [&](std::string_view text)
  {
    irNamesIn(text, [&](size_t at, size_t length) { work.push_back(text.substr(at, length)); });
    Emitter::typeNamesIn(text, [&](std::string_view name) { typeWork.push_back(name); });
  };
  for (const Shared::Definition* d : definitions)
  {
    for (auto [at, length] : d->names)
      work.push_back(std::string_view(d->text).substr(at, length));
    for (auto [at, length] : d->types)
      typeWork.push_back(std::string_view(d->text).substr(at, length));
  }
  for (std::string_view entry : owned)
    uses(entry);
  std::unordered_set<std::string_view> seen;
  while (!work.empty())
  {
    std::string_view name = work.back(); // `@name` or `@"name"`
    work.pop_back();
    if (name.size() < 2 || !seen.insert(name).second || defined.count(name.substr(1)))
      continue;
    std::string key(name);
    const Shared::LiteralShard& literals = sh.literalShard(key);
    if (auto literal = literals.contents.find(key); literal != literals.contents.end())
    {
      std::string_view text = made.emplace_back(Emitter::literalDefinition(key, literal->second));
      lines.push_back(text.substr(0, text.size() - 1)); // (its comdat and its definition)
      uses(literal->second);                            // (a constant array's pointers and types)
      continue;
    }
    std::string_view bare = name.substr(1);
    const std::vector<std::string_view>* units = nullptr;
    bool fromOwn = false, fromDecls = false;
    if (auto it = own.find(bare); it != own.end())
    {
      units = &it->second;
      fromOwn = true;
    }
    else if (auto it = sources.shared.find(bare); it != sources.shared.end())
      units = &it->second;
    else if (auto it = sources.decls.find(bare); it != sources.decls.end())
    {
      units = &it->second;
      fromDecls = true;
    }
    if (units)
    {
      std::string external; // a homed global of the shared declarations
      if (fromDecls)
        for (std::string_view unit : *units)
          if (std::string_view ignored; external.empty())
            external = Emitter::externalDeclarationOf(unit, ignored);
      if (!external.empty())
      {
        std::string_view text = made.emplace_back(std::move(external));
        declarations.push_back(text);
        Emitter::typeNamesIn(text, [&](std::string_view type) { typeWork.push_back(type); });
        continue;
      }
      for (std::string_view unit : *units)
      {
        // (a private vtable in a comdat named after its MSVC symbol, Layout.cpp, is a copy: the linker keeps one)
        if (fromOwn && errors && Emitter::localLine(unit) && unit.find(", comdat($\"??_7") == std::string_view::npos)
          *errors += "codegen: module " + std::to_string(k) + "'s program piece names " + key + ", private to its instance piece (step 4's pieces)\n";
        lines.push_back(unit);
        uses(unit);
      }
      continue;
    }
    const std::string* declaration = nullptr;
    if (auto it = sh.declareLines.find(key); it != sh.declareLines.end())
      declaration = &it->second;
    else if (auto it = sh.requestDeclarations.find(key); it != sh.requestDeclarations.end())
      declaration = &it->second;
    if (declaration)
    {
      declarations.push_back(*declaration);
      Emitter::typeNamesIn(*declaration, [&](std::string_view type) { typeWork.push_back(type); });
    }
  }
  std::vector<std::string_view> types;
  std::unordered_set<std::string_view> typeSeen;
  while (!typeWork.empty())
  {
    std::string_view name = typeWork.back();
    typeWork.pop_back();
    if (!typeSeen.insert(name).second)
      continue;
    auto it = sources.types.find(name);
    if (it == sources.types.end())
    {
      if (complete)
        *complete = false;
      continue;
    }
    types.push_back(it->second);
    Emitter::typeNamesIn(it->second.substr(it->second.find(" = type ")), [&](std::string_view type) { typeWork.push_back(type); });
  }
  for (std::vector<std::string_view>* part : {&types, &lines, &declarations})
  {
    std::sort(part->begin(), part->end());
    part->erase(std::unique(part->begin(), part->end()), part->end());
  }
  size_t size = prefix.size() + 1 + (sh.debugInfo ? 1024 : 0); // (and the tail)
  for (const std::vector<std::string_view>* part : {&types, &lines, &owned, &declarations})
    for (std::string_view text : *part)
      size += text.size() + 1;
  for (const Shared::Definition* d : definitions)
    size += d->text.size();
  std::string out;
  out.reserve(size);
  out += prefix;
  for (const std::vector<std::string_view>* part : {&types, &lines})
    for (std::string_view text : *part)
    {
      out += text;
      out += '\n';
    }
  for (std::string_view text : owned)
    out += text;
  for (std::string_view text : declarations)
  {
    out += text;
    out += '\n';
  }
  out += '\n';
  uint32_t nextDebugId = debugFirstId;
  size_t definitionsAt = out.size();
  for (const Shared::Definition* d : definitions)
  {
    size_t at = out.size();
    if (sh.debugInfo)
    {
      appendDefinitionMetadataFirst(out, d->text);
      nextDebugId += renumberDebugIds(out.data() + at, d->text.size(), nextDebugId);
    }
    else
      out += d->text;
  }
  size_t metadataAt = out.size();
  DebugLists lists;
  if (sh.fullDebugInfo)
    lists = appendDebugTypes(sh, out, definitionsAt, nextDebugId);
  if (sh.debugInfo)
  {
    out += debugModuleTailFor(sh.target, sh.fullDebugInfo, lists);
    moveMetadataBeforeDefinitions(out, definitionsAt, metadataAt);
  }
  return out;
}

// A text's FNV-1a hash: step 4's P2 checks that a program piece handed to the stream is the end's text.
static uint64_t textHash(std::string_view text)
{
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : text)
    h = (h ^ c) * 1099511628211ull;
  return h;
}

// Output module i of a build (Shared::outputModules): module i's text, or with pieces, the program piece
// of module i (i < M) or the instance piece of module i - M.
static std::string outputText(Emitter& em, unsigned i, const std::string& prefix, const PieceSources* sources, std::string* errors)
{
  Shared& sh = em.sh;
  if (!sh.splitPieces)
    return em.moduleText(i, prefix);
  if (i < sh.moduleCount)
    return programPieceText(sh, i, prefix, *sources, errors);
  return em.moduleText(i - sh.moduleCount, prefix, true, errors);
}

static CodegenResult generate(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols,
                              const CodegenOptions& options, CodegenSessionPtr* keep, PieceStream* stream)
{
  bool debug = options.debug, despiteErrors = options.despiteErrors, stubMissing = options.stubMissing;
  bool inlineTrivial = options.inlineTrivial, debugInfo = options.debugInfo, optimize = options.optimize;
  unsigned moduleCount = options.moduleCount, threads = options.threads;
  const Target* target = options.target;
  // The build's state lives in a session (Shared refers to its result), kept for the next build
  // when asked (keep) or freed.
  CodegenSessionPtr session(new CodegenSession);
  CodegenResult& result = session->result;
  auto shared = std::make_unique<Shared>(result);
  Shared& sh = *shared;
  sh.debug = debug;
  sh.stubMissing = despiteErrors && stubMissing;
  sh.despiteErrors = despiteErrors;
  if (target)
    sh.target = *target;
  sh.inlineTrivial = inlineTrivial;
  sh.debugInfo = debugInfo;
  sh.fullDebugInfo = debugInfo && !optimize; // (an optimized build keeps its line tables alone)
  sh.setModules(moduleCount);
  // A release build (ThinLTO) puts its short definitions in modules of their own (Emitter::placeShortDefinitions).
  if (!debug && sh.moduleCount >= 2)
    sh.importableModules = sh.moduleCount / 2;
  // A debug build of several modules, not optimized, has each in two pieces (Shared::splitPieces).
  sh.splitPieces = codegenOutputModules(sh.moduleCount, debug, optimize) != sh.moduleCount;
  // Every module's first lines, with the attribute group its calls name (#0: the post-invoke nop's,
  // Emitter::emitCall).
  std::string prefix = "; kpp v0: generated by kpp\n"
                       "target datalayout = \"" + sh.target.dataLayout + "\"\n"
                       "target triple = \"" + sh.target.triple + "\"\n"
                       "attributes #0 = { memory(inaccessiblemem: readwrite) }\n\n";
  for (size_t i = 0; i < files.size(); ++i)
    sh.fileModule[files[i].get()] = sh.emissionModule(i);
  // One emitter per thread; the first one also finishes the runtime and assembles the modules.
  threads = std::max(1u, threads);
  std::vector<std::unique_ptr<Emitter>> emitters;
  for (unsigned i = 0; i < threads; ++i)
    emitters.push_back(std::make_unique<Emitter>(global, symbols, sh));
  session->threads = threads;
  // (Each emitter where it is for as long as sh is: a kept session keeps both, and a build frees
  // the emitters before sh.)
  std::vector<Emitter*> each;
  for (const std::unique_ptr<Emitter>& em : emitters)
    each.push_back(em.get());
  sh.onEmitters = [each](const std::function<void(Emitter&)>& work)
  {
    auto run = [&](unsigned i) { work(*each[i]); };
    onThreads(unsigned(each.size()), run);
  };
  // The emitters' and the shared state are freed on a thread of their own once the result is
  // returned (about 3 s for the game, on the build's critical path before): a build in its own
  // process exits without waiting for it, the compile server answers meanwhile. Their destructors
  // touch nothing but their own containers (not the result, not the program they point into).
  // A session kept for the next build takes them instead.
  struct FreeLater
  {
    ~FreeLater()
    {
      if (keep && session->usable)
      {
        session->shared = std::move(shared);
        session->emitters = std::move(emitters);
        *keep = std::move(session);
        return;
      }
      std::thread([shared = std::move(shared), emitters = std::move(emitters), session = std::move(session)]() mutable
      {
        emitters.clear();
        shared.reset();
        session.reset();
      }).detach();
    }

    std::unique_ptr<Shared>& shared;
    std::vector<std::unique_ptr<Emitter>>& emitters;
    CodegenSessionPtr& session;
    CodegenSessionPtr* keep;
  } freeLater{shared, emitters, session, keep};
  Emitter& e = *emitters[0];
  std::vector<FunctionDecl*> functions;
  std::vector<VariableDecl*> startupGlobals;
  for (auto& file : files)
  {
    e.settleDefinitionFlags(file->decls);
    e.collectFunctions(file->decls, functions);
    if (!file->isImported)
      e.collectStartupGlobals(file->decls, startupGlobals);
  }
  // `template struct Registry<int>;`: the instance's static data members too (the Lua bindings'
  // LuaStruct<T> register their metamethods so), built where the explicit instantiation is
  // written, in their definitions' order.
  std::vector<std::pair<VariableDecl*, const Ty*>> instanceStartupGlobals;
  std::vector<Shared::Startup> instanceStartupAt;
  for (auto& file : files)
    if (!file->isImported)
      for (auto& [cls, token] : global.explicitInstantiationsOf(*file))
      {
        e.collectInstanceStartupGlobals(cls, instanceStartupGlobals);
        instanceStartupAt.resize(instanceStartupGlobals.size(), Shared::Startup{file->index, token, 0, ""});
      }
  // Every global built at startup is defined and built, read or not (C++ initialises them all);
  // what their initializers ask for joins the next level's requests. They are the first items of
  // level 0's queue, on all threads with the program's functions: one thread took 4.5 s of the
  // game's 9 s of codegen defining them before any function, nearly all of it evaluating
  // initializers (a few take a second each). The order they run in at startup is ctorsText's (where
  // each is defined), not the order they are defined in here.
  size_t startupCount = startupGlobals.size() + instanceStartupGlobals.size();
  auto emitStartup = [&](Emitter& em, size_t k)
  {
    if (k < startupGlobals.size())
      em.emitStartupGlobal(startupGlobals[k]);
    else
    {
      size_t j = k - startupGlobals.size();
      em.emitStartupGlobal(instanceStartupGlobals[j].first, instanceStartupGlobals[j].second, instanceStartupAt[j]);
    }
  };
  // Level 0: the functions the program writes. Each later level: the instantiations the previous
  // one asked for. The functions of a level are emitted on all threads at once.
  std::vector<Instantiation> level;
  for (FunctionDecl* f : functions)
  {
    auto it = sh.fileModule.find(f->file);
    level.push_back({f, {}, nullptr, it == sh.fileModule.end() ? 0 : it->second, "", "", 0});
  }
  // definedIn and declareLines get every definition's name, and requestDeclarations level 0's requests, under
  // definitionsMutex (Emitter::addDefinition): a rehash of one stopped every emitter that added a definition
  // meanwhile, 156.5 ms in the game's level 12 as the maps passed 262,144 names (docs/codegen-speed.md, step 6).
  // Reserved for what the program will define: its own functions and startup globals and the function instances
  // the resolver made, twice over (their lambdas' operators, thunks, constructors: the game's 162,165 make 266,292
  // names, in the 524,288 buckets reserved for 324,330), at most 2 million names. A program past the estimate
  // rehashes as before (docs/codegen-speed.md, step 7).
  {
    size_t own = level.size() + startupCount;
    size_t expected = std::min<size_t>(2 * (own + global.instantiationStats().functions), size_t(1) << 21);
    sh.definedIn.reserve(expected);
    sh.declareLines.reserve(expected);
    sh.requestDeclarations.reserve(std::min<size_t>(own, size_t(1) << 21));
  }
  auto emitOne = [&](Emitter& em, const Instantiation& item) { emitInstantiation(em, item); };
  auto nextLevel = [&]() { collectNextLevel(emitters, level); };
  std::atomic<bool> outOfMemory{false};
  size_t emitted = 0;
  std::set<std::pair<StaticAssertDecl*, std::string>> layoutAssertsChecked;
  // Emits `level` and every level it asks for, on all threads (also what an incremental build
  // emits again: KPP_CHECK_REBUILD below).
  auto emitLevels = [&]()
  {
    while (!outOfMemory)
    {
      if (level.empty() && startupCount == 0)
      {
        // Everything is emitted: the static_asserts on a class's size or alignment, which the
        // checker leaves to the layouts; those of class template instances once the functions
        // that name the instances are emitted (and one in an instance's function if that is
        // emitted). A layout they build can ask for functions (its vtable's), which go on.
        result.instanceAssertsChecked = global.checkInstanceStaticAsserts(result.instanceAssertsChecked); // (only those named since)
        bool checked = false;
        for (const GlobalScope::LayoutStaticAssert& a : global.layoutStaticAsserts())
          if (layoutAssertsChecked.insert({a.decl, a.instance}).second)
          {
            e.checkLayoutAssert(a);
            checked = true;
          }
        if (!checked)
          break;
        nextLevel();
        continue;
      }
      std::atomic<size_t> next{0};
      size_t head = startupCount; // the startup globals, before level 0's functions (once)
      startupCount = 0;
      auto work = [&](unsigned i) {
        try
        {
          for (size_t k; !outOfMemory && (k = next++) < head + level.size();)
            if (k < head)
              emitStartup(*emitters[i], k);
            else
              emitOne(*emitters[i], level[k - head]);
        }
        catch (const std::bad_alloc&)
        {
          outOfMemory = true;
        }
      };
      onThreads(head + level.size() < 64 ? 1 : threads, work);
      emitted += level.size();
      nextLevel();
      if (!sh.barrierTaken && sh.moduleCount > 1 && sh.importableModules == 0 && !outOfMemory)
      {
        // Level 0's barrier (Shared::requestDeclarations): what is defined at it, by name and per
        // module (the definitions are only appended to until the end sorts them).
        sh.barrierTaken = true;
        sh.barrierCounts.resize(sh.moduleCount);
        sh.barrierOwned.resize(sh.moduleCount);
        for (unsigned k = 0; k < sh.moduleCount; ++k)
        {
          sh.barrierCounts[k] = sh.definitions[k].size();
          sh.barrierOwned[k] = sh.ownedEntries[k].size();
        }
        gatherProgramFunctions(sh, emitters);
        // The shared definitions written so far (destructor helpers, the runtimes' functions): a
        // program piece declares them, never takes their text (homed later, among the instance
        // pieces), by the declarations of their own headers.
        for (const std::string& entry : sh.shared.entries)
          for (size_t at = entry.starts_with("define ") ? 0 : entry.find("\ndefine "); at != std::string::npos; at = entry.find("\ndefine ", at + 1))
          {
            size_t start = entry[at] == '\n' ? at + 1 : at;
            std::string name, declaration = Emitter::sharedDeclarationOf(entry.substr(start, entry.find('\n', start) - start), &name);
            sh.requestDeclarations.emplace(name, std::move(declaration));
          }
        // The runtime's functions and global written after the levels, from the headers and the
        // line they are written from.
        for (const std::string& header : Emitter::runtimeHeaders())
        {
          std::string name, declaration = Emitter::sharedDeclarationOf(header, &name);
          sh.requestDeclarations.emplace(name, std::move(declaration));
        }
        {
          std::string_view name;
          std::string declaration = Emitter::externalDeclarationOf(Emitter::kppEhCurrentLine, name);
          sh.requestDeclarations.emplace(std::string(name), std::move(declaration));
        }
        if (sh.checkBarrier)
          for (const auto& [name, line] : definedNames(sh))
            sh.barrierDefined.insert(name);
        // The program pieces as the barrier has them: each one it has whole handed to the stream as soon
        // as it is assembled (step 4's P2), when no error came before (an error's --keep-going
        // declarations come at the end); and kept under KPP_CHECK_BARRIER for the check at the end that
        // each is the end's.
        bool streaming = stream && sh.splitPieces &&
                         std::all_of(emitters.begin(), emitters.end(), [](const std::unique_ptr<Emitter>& em) { return em->errors.empty() && em->hiddenErrors == 0; });
        if ((sh.checkBarrier || streaming) && sh.splitPieces)
        {
          PieceSources sources = pieceSources(sh, true);
          if (sh.checkBarrier)
            sh.barrierPieces.assign(sh.moduleCount, {});
          if (streaming)
            sh.streamedPieces.assign(sh.moduleCount, {});
          // The largest pieces first, as the stream compiles them (BackEndPool): by the text of the
          // definitions each module has at the barrier, nearly all of them the program functions'.
          std::vector<unsigned> order(sh.moduleCount);
          std::vector<size_t> written(sh.moduleCount);
          for (unsigned k = 0; k < sh.moduleCount; ++k)
          {
            order[k] = k;
            for (const Shared::Definition& d : sh.definitions[k])
              written[k] += d.text.size();
          }
          std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) { return written[a] > written[b]; });
          std::atomic<unsigned> nextPiece{0};
          auto assemblePieces = [&](unsigned)
          {
            for (unsigned i; (i = nextPiece++) < sh.moduleCount;)
            {
              unsigned k = order[i];
              bool complete = true;
              std::string errors; // (the end assembles the piece again and reports them)
              std::string text = programPieceText(sh, k, prefix, sources, &errors, &complete);
              sh.latePieces[k] = !complete;
              if (sh.checkBarrier)
                sh.barrierPieces[k] = text;
              if (streaming && complete && errors.empty())
              {
                sh.streamedPieces[k] = {true, textHash(text), text.size()};
                stream->piece(k, std::move(text));
              }
            }
          };
          onThreads(threads, assemblePieces);
        }
      }
    }
  };
  emitLevels();
  e.finishAssertRuntime();
  e.finishThreadExitRuntime();
  e.finishProgramExitRuntime();
  e.finishExceptionRuntime();
  if (sh.stubMissing)
  {
    // --stub-missing: what each stub says, the function and where it is (a failed function's own
    // line, a function used without a definition its declaration's).
    for (auto& em : emitters)
      for (auto& [name, where] : em->failedAt)
        sh.stubMessages.emplace(name, where);
    for (auto& [name, f] : sh.undefinedUses)
      if (!sh.definedIn.count(name))
        sh.stubMessages.emplace(name, e.qualifiedName(f) + " at " + e.programLocationOf(f, f->file) + " (used, but no definition of it was found)");
    e.finishStubRuntime();
  }
  // A closure that functions name but whose lambda was never laid out: the function it is written
  // in failed before it (an error), and so did those functions, but with --keep-going the modules
  // have to parse, so the type gets a placeholder. Without any error it is a closure nobody built.
  bool placeholders = false;
  {
    bool anyErrors = false;
    for (auto& em : emitters)
      anyErrors = anyErrors || !em->errors.empty() || em->hiddenErrors;
    for (auto& [t, info] : sh.closures)
      if (!info.laidOut && !info.structName.empty())
      {
        if (!anyErrors)
        {
          e.currentFile = t->decl->file;
          e.error(t->decl, "the closure of " + e.irString(t) + " is used, but its lambda was never emitted");
        }
        sh.decls += info.structName + " = type { i8 } ; left out by codegen errors\n";
        placeholders = true;
      }
  }
  // Functions of the program used where no definition of theirs was found (an out-of-line member
  // the resolver did not attach to its declaration, a template whose definition it did not find):
  // each an error at its declaration, never only a name the link misses.
  for (auto& [name, f] : sh.undefinedUses)
    if (!sh.definedIn.count(name))
    {
      e.currentFile = f->file;
      e.error(f, e.qualifiedName(f) + " is used, but no definition of it was found (" + name + ")");
    }
  // A lambda's call operator is emitted with the function the lambda is written in; one called
  // that never was is reported, unless that function failed (its own error says why).
  {
    std::unordered_set<std::string> failed;
    for (auto& em : emitters)
      failed.insert(em->failedFunctions.begin(), em->failedFunctions.end());
    for (auto& [name, lambda] : sh.closureCalls)
      if (!sh.definedIn.count(name))
      {
        std::string writer;
        if (auto recorded = sh.closureWriters.find(lambda); recorded != sh.closureWriters.end())
          writer = recorded->second.writtenIn;
        if (!writer.empty() && failed.count(writer))
          continue;
        e.currentFile = lambda->decl->file;
        e.error(lambda->decl, "the call operator of a lambda written in " + (writer.empty() ? std::string("a function never emitted") : writer) +
                                  " is used, but it was never emitted (" + name + ")");
      }
  }
  // The errors, in the same order in every run.
  for (auto& em : emitters)
  {
    result.errors.insert(result.errors.end(), em->errors.begin(), em->errors.end());
    result.failedFunctions.insert(result.failedFunctions.end(), em->failedFunctions.begin(), em->failedFunctions.end());
    result.hiddenErrors += em->hiddenErrors;
  }
  sortDiagnostics(result.errors);
  std::sort(result.failedFunctions.begin(), result.failedFunctions.end());
  if (outOfMemory)
    result.errors.push_back("codegen: out of memory after " + std::to_string(emitted) + " functions");
  // Whether anything failed, shown or not: what an error left out is left out either way.
  const bool failed = result.failed();
  result.functions = sh.functions;
  // Each shared function (a destructor helper, a runtime function) is defined in one home module,
  // hash(name) % moduleCount like a function, and declared by the modules that use it (moduleText's
  // declares): in every module, the destructor helpers were 843 of the game's 1,760 MB of IR. The
  // home depends only on the function's name and the module count, so it does not move when other
  // code changes. (Streaming's program pieces declare them, never define them: docs/codegen-speed.md.)
  if (sh.moduleCount > 1)
  {
    std::vector<std::string> everywhere;
    for (std::string& entry : sh.shared.entries)
    {
      std::string header;
      std::string name = sharedFunctionIn(entry, header);
      // (A release build's ThinLTO has every other shared function in every module, as before.)
      if (name.empty() || (sh.importableModules != 0 && !name.starts_with("@kpp.dtor.")))
      {
        everywhere.push_back(std::move(entry));
        continue;
      }
      unsigned home = sh.emissionModule(std::hash<std::string>()(name));
      sh.definedIn[name] = home;
      sh.declareLines[name] = Emitter::sharedDeclarationOf(header);
      // weak_odr, not linkonce_odr: the home module's copy is the only one, and a release build's
      // optimizer drops a linkonce_odr definition its own module does not call (the link then
      // missed the helpers the other modules declare).
      if (size_t at = entry.find("define linkonce_odr "); at != std::string::npos)
        entry.replace(at, std::string_view("define linkonce_odr").size(), "define weak_odr");
      sh.definitions[home].push_back(Shared::definitionOf(std::move(entry)));
    }
    sh.shared.entries = std::move(everywhere);
  }
  // Streaming's barrier check (Shared::checkBarrier), every text written (the shared functions homed
  // just above): each name a level-0 definition uses that only a text written after level 0
  // defines (an instance, a destructor helper, the runtime's functions and global) has a barrier
  // declaration, and each barrier declaration is its definition's (a function's declare line, a
  // global's external declaration). A difference is kpp's error, never a program piece assembled from it.
  if (sh.barrierTaken && sh.checkBarrier && result.errors.empty())
  {
    std::unordered_map<std::string, std::string> defined = definedNames(sh);
    std::map<std::string, std::string> unknown; // a name -> the first level-0 definition naming it
    for (unsigned k = 0; k < sh.moduleCount; ++k)
      for (size_t i = 0; i < sh.barrierCounts[k] && i < sh.definitions[k].size(); ++i)
      {
        const Shared::Definition& d = sh.definitions[k][i];
        for (auto [at, length] : d.names)
        {
          std::string name = d.text.substr(at, length);
          if (defined.count(name) && !sh.barrierDefined.count(name) && !sh.requestDeclarations.count(name))
            unknown.emplace(name, d.name);
        }
      }
    for (auto& [name, by] : unknown)
      result.errors.push_back("codegen: " + name + " is named in level 0 (by " + by + ") and defined after it, but has no declaration from the barrier (step 4's barrier)");
    std::vector<std::string> different;
    for (auto& [name, declaration] : sh.requestDeclarations)
    {
      auto it = defined.find(name);
      if (it == defined.end())
        continue; // (never defined: a function left out by its failure, reported where it failed)
      std::string_view ignored;
      std::string actual;
      if (!it->second.empty())
        actual = Emitter::externalDeclarationOf(it->second, ignored);
      else if (auto line = sh.declareLines.find(name); line != sh.declareLines.end())
        actual = line->second;
      if (actual != declaration)
        different.push_back("codegen: the barrier's declaration of " + name + ", `" + declaration + "`, is not its definition's, `" + actual + "` (step 4's barrier)");
    }
    std::sort(different.begin(), different.end());
    result.errors.insert(result.errors.end(), different.begin(), different.end());
  }
  sh.decls.finish();
  sh.shared.finish();
  sh.body.finish();
  // KPP_CHECK_REBUILD=n, a check of the incremental path (docs/compile-speed.md, step 5): every
  // n-th function of the program is emitted again in the kept state, as an edit of its body would
  // be, and the modules assembled again must be byte for byte the ones just built.
  const char* checkRebuild = getenv("KPP_CHECK_REBUILD");
  // A compile server's session is kept across errors when each came from a program function's own
  // emission (Emitter::error): the next build emits those functions again, as it does edited ones,
  // and assembles the modules once it has no errors. Not when a failure is remembered for every use
  // (a layout's, a closure's, a global's), a lambda got a placeholder, or the IR is assembled
  // despite the errors (--keep-going).
  bool keptAcrossErrors = keep && failed && !despiteErrors && !outOfMemory && !placeholders && sh.failuresRemembered == 0 &&
                          std::none_of(emitters.begin(), emitters.end(), [](const std::unique_ptr<Emitter>& em) { return em->unattributedErrors; });
  if (keptAcrossErrors)
    for (auto& em : emitters)
      sh.erroring.insert(em->erroring.begin(), em->erroring.end());
  bool keepTexts = ((checkRebuild && !failed) || (keep && (!failed || keptAcrossErrors))) && sh.moduleCount > 1;
  gatherProgramFunctions(sh, emitters);
  // Each module's own texts finished, on the emitters' threads: a module's step touches only its own data, and
  // reads the program's functions and the shared literals. On one thread this was 130 ms of the game's codegen,
  // on the threads 12 ms (docs/codegen-speed.md, steps 6 and 7).
  std::atomic<unsigned> nextFinished{0};
  std::atomic<bool> finishOutOfMemory{false};
  auto finishModules = [&](unsigned)
  {
    try
    {
      for (unsigned k; (k = nextFinished++) < sh.moduleCount;)
      {
        // A program piece the barrier does not have whole (Shared::latePieces).
        if (sh.splitPieces && sh.barrierTaken)
        {
          bool late = false;
          for (size_t i = sh.barrierCounts[k]; i < sh.definitions[k].size() && !late; ++i)
            late = sh.programOwned(sh.definitions[k][i].owner);
          for (size_t i = sh.barrierOwned[k]; i < sh.ownedEntries[k].size() && !late; ++i)
            late = sh.programOwned(sh.ownedEntries[k][i].owner);
          sh.latePieces[k] = sh.latePieces[k] || late;
        }
        // The definitions in the order SharedText puts entries: by their text.
        std::sort(sh.definitions[k].begin(), sh.definitions[k].end(),
                  [](const Shared::Definition& a, const Shared::Definition& b) { return a.text < b.text; });
        dropSharedLiterals(sh, sh.moduleDecls[k].entries);
        if (keepTexts)
          sh.keptModuleEntries[k] = sh.moduleDecls[k].entries;
        appendOwnedEntries(sh, k, sh.moduleDecls[k].entries);
        sh.moduleDecls[k].finish();
      }
    }
    catch (const std::bad_alloc&)
    {
      finishOutOfMemory = true; // (thrown again below, on this thread, as the loop on one thread threw it)
    }
  };
  onThreads(std::max(1u, std::min(unsigned(emitters.size()), sh.moduleCount)), finishModules);
  if (finishOutOfMemory)
    throw std::bad_alloc();
  // Skip IR assembly when there are errors: the IR would be invalid anyway, and assembling
  // gigabytes of text just to discard it wastes memory and time. With despiteErrors the IR is
  // assembled from what was emitted, and what an error left out is declared (declareUndefined).
  if (failed && !despiteErrors)
  {
    if (keepTexts)
    {
      // Kept across the errors: placed and prepared as for assembly, and no module assembled (the
      // build that has no errors assembles them all).
      e.placeShortDefinitions(threads);
      e.prepareModuleTexts();
      session->prefix = prefix;
      sh.unassembled.assign(sh.moduleCount, true);
      session->usable = !checkRebuild;
    }
    return std::move(result);
  }
  if (e.moduleCount <= 1)
  {
    // Pre-compute pieces and reserve to avoid temporary-string OOM on large programs.
    std::string decls = e.declsFor(0);
    std::string ctors = e.ctorsText(0);
    size_t total = prefix.size() + decls.size() + e.moduleDecls[0].text.size() + ctors.size() + 1 + e.shared.text.size() + e.body.text.size();
    result.ir.reserve(total);
    result.ir.append(prefix);
    result.ir.append(decls);
    result.ir.append(e.moduleDecls[0].text);
    result.ir.append(ctors);
    result.ir.push_back('\n');
    result.ir.append(e.shared.text);
    uint32_t nextDebugId = debugFirstId;
    if (sh.debugInfo)
    {
      // One definition after another, each numbering its metadata from 0 (Emitter::debugLines).
      std::string& body = e.body.text;
      for (size_t at = 0; at < body.size();)
      {
        size_t next = body.find("\ndefine ", at);
        next = next == std::string::npos ? body.size() : next + 1;
        nextDebugId += renumberDebugIds(body.data() + at, next - at, nextDebugId);
        at = next;
      }
    }
    size_t bodyAt = result.ir.size();
    if (sh.fullDebugInfo)
      e.homeDebugClasses(); // (one module: every class is defined in it)
    result.ir.append(std::move(e.body.text));
    DebugLists lists;
    if (sh.fullDebugInfo)
      lists = appendDebugTypes(sh, result.ir, bodyAt, nextDebugId, &sh.debugHomes[0]);
    if (sh.debugInfo)
      result.ir += debugModuleTailFor(sh.target, sh.fullDebugInfo, lists);
    if (failed)
    {
      std::unordered_set<std::string> stubbed;
      result.ir += declareUndefined(result.ir, sh.stubMissing ? &sh.stubMessages : nullptr, &stubbed);
      result.stubs = stubbed.size() + countUndefinedStubs(sh);
    }
    return std::move(result);
  }
  // The modules are independent once the texts are finished: assembled on all the emitters'
  // threads at once (on one thread this was 4.5 s of the game's codegen), each module's own texts
  // freed as soon as it is assembled.
  e.placeShortDefinitions(threads);
  e.prepareModuleTexts();
  const unsigned outputs = sh.outputModules();
  result.modules.resize(outputs);
  std::vector<std::unordered_set<std::string>> stubbedIn(outputs);
  std::vector<std::string> pieceErrors(e.moduleCount);
  std::optional<PieceSources> sources;
  if (sh.splitPieces)
    sources = pieceSources(sh, false);
  std::atomic<unsigned> nextModule{0};
  // (A module's definitions may be placed in another (placeShortDefinitions): they are freed once
  // every module is assembled; its own declarations as soon as it is, both its pieces with it.)
  bool placed = sh.importableModules != 0;
  std::vector<char> asStreamed(e.moduleCount); // (step 4's P2: a program piece's text is the one the stream has)
  auto assemble = [&](unsigned thread)
  {
    Emitter& em = *emitters[thread];
    for (unsigned k = nextModule++; k < em.moduleCount; k = nextModule++)
    {
      for (unsigned i : {k, k + em.moduleCount})
        if (i < outputs)
          result.modules[i] = outputText(em, i, prefix, sources ? &*sources : nullptr, &pieceErrors[k]);
      if (!keepTexts)
      {
        if (!placed)
          std::vector<Shared::Definition>().swap(sh.definitions[k]);
        std::string().swap(sh.moduleDecls[k].text);
      }
      if (failed)
        for (unsigned i : {k, k + em.moduleCount})
          if (i < outputs)
            result.modules[i] += declareUndefined(result.modules[i], sh.stubMissing ? &sh.stubMessages : nullptr, &stubbedIn[i]);
      if (k < sh.streamedPieces.size() && sh.streamedPieces[k].handed)
        asStreamed[k] = result.modules[k].size() == sh.streamedPieces[k].size && textHash(result.modules[k]) == sh.streamedPieces[k].hash;
    }
  };
  onThreads(unsigned(emitters.size()), assemble);
  // Step 4's P2: the build links the stream's object of a program piece handed over at the barrier when
  // its text is the end's (CodegenResult::streamed). Otherwise the driver compiles the end's: as it may
  // differ (--keep-going's declarations of what failed, a program function's lambda emitted at a later
  // level), or, in a build without errors, as kpp's error, never silently (KPP_CHECK_BARRIER shows the
  // first difference).
  if (!sh.streamedPieces.empty())
  {
    result.streamed.assign(outputs, false);
    for (unsigned k = 0; k < e.moduleCount; ++k)
    {
      result.streamed[k] = asStreamed[k] != 0;
      if (sh.streamedPieces[k].handed && !asStreamed[k] && !failed && !sh.latePieces[k] && sh.barrierPieces.empty())
        pieceErrors[k] += "codegen: module " + std::to_string(k) + "'s program piece handed to the back end at level 0's barrier is not the end's "
                          "(step 4's pieces; KPP_CHECK_BARRIER=1 shows the first difference)\n";
    }
    sh.streamedPieces.clear();
  }
  // KPP_CHECK_BARRIER: each program piece is the one level 0's barrier assembled, unless it is late.
  if (sh.splitPieces && !sh.barrierPieces.empty() && !failed)
    for (unsigned k = 0; k < e.moduleCount; ++k)
      if (!sh.latePieces[k] && sh.barrierPieces[k] != result.modules[k])
      {
        const std::string& early = sh.barrierPieces[k];
        const std::string& end = result.modules[k];
        size_t at = std::mismatch(early.begin(), early.end(), end.begin(), end.end()).first - early.begin();
        auto lineAt = [&](const std::string& text)
        {
          size_t start = text.rfind('\n', at);
          start = start == std::string::npos ? 0 : start + 1;
          return text.substr(start, std::min(text.find('\n', start), start + 160) - start);
        };
        pieceErrors[k] += "codegen: module " + std::to_string(k) + "'s program piece at level 0's barrier is not the end's (step 4's pieces): `" + lineAt(early) +
                          "` at the barrier, `" + lineAt(end) + "` at the end\n";
        if (const char* dump = getenv("KPP_CHECK_BARRIER_DUMP")) // a directory: each such piece as the barrier and the end assembled it
          for (auto [what, text] : {std::pair<const char*, const std::string*>{"barrier", &early}, {"end", &end}})
            if (FILE* out = fopen((std::string(dump) + "/" + what + "-" + std::to_string(k) + ".ll").c_str(), "wb"))
            {
              fwrite(text->data(), 1, text->size(), out);
              fclose(out);
            }
      }
  sh.barrierPieces.clear();
  for (const std::string& errors : pieceErrors)
    for (size_t at = 0; at < errors.size();)
    {
      size_t end = errors.find('\n', at);
      result.errors.push_back(errors.substr(at, end - at));
      at = end + 1;
    }
  if (placed && !keepTexts)
    for (unsigned k = 0; k < e.moduleCount; ++k)
      std::vector<Shared::Definition>().swap(sh.definitions[k]);
  std::unordered_set<std::string> stubbed;
  for (const std::unordered_set<std::string>& names : stubbedIn)
    stubbed.insert(names.begin(), names.end());
  if (sh.stubMissing)
    result.stubs = stubbed.size() + countUndefinedStubs(sh);
  session->prefix = prefix;
  if (keepTexts && checkRebuild)
  {
    // Every n-th function of the program emitted again through rebuildLLVM, as the compile server
    // will after an edit of their bodies; the modules it assembles again must be the ones just built.
    size_t every = std::max<size_t>(1, strtoul(checkRebuild, nullptr, 10));
    std::vector<FunctionDecl*> picked;
    for (size_t i = 0; i < functions.size(); i += every)
      picked.push_back(functions[i]);
    session->shared = std::move(shared);
    session->emitters = std::move(emitters);
    session->usable = true;
    auto start = std::chrono::steady_clock::now();
    bool rebuilt = false;
    CodegenResult again = rebuildLLVM(*session, picked, {}, {}, {}, {}, {}, {}, {}, {}, rebuilt);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    shared = std::move(session->shared);
    emitters = std::move(session->emitters);
    session->usable = false;
    // The modules it assembled (none, when every function came out the same), then all the others
    // assembled from the state it left: each must be the build's.
    std::vector<std::string> texts(rebuilt ? again.changedModules.size() : 0);
    std::optional<PieceSources> sourcesNow;
    if (sh.splitPieces && rebuilt)
      sourcesNow = pieceSources(sh, false);
    std::atomic<unsigned> nextCheck{0};
    auto assembleAll = [&](unsigned t)
    {
      for (unsigned k; (k = nextCheck++) < texts.size();)
        texts[k] = again.changedModules[k] ? std::move(again.modules[k]) : outputText(*emitters[t], k, prefix, sourcesNow ? &*sourcesNow : nullptr, nullptr);
    };
    onThreads(unsigned(emitters.size()), assembleAll);
    size_t changed = 0, differ = 0;
    std::string first;
    for (size_t k = 0; k < texts.size(); ++k)
    {
      changed += again.changedModules[k];
      if (texts[k] != result.modules[k])
      {
        ++differ;
        if (first.empty())
        {
          size_t at = std::mismatch(texts[k].begin(), texts[k].end(), result.modules[k].begin(), result.modules[k].end()).first - texts[k].begin();
          size_t line = texts[k].rfind('\n', at);
          first = "module " + std::to_string(k) + ": " + texts[k].substr(line == std::string::npos ? 0 : line + 1, 160);
        }
        if (const char* dump = getenv("KPP_CHECK_REBUILD_DUMP")) // a directory: each such module as built and as assembled again
          for (auto [what, text] : {std::pair<const char*, const std::string*>{"built", &result.modules[k]}, {"rebuilt", &texts[k]}})
            if (FILE* out = fopen((std::string(dump) + "/" + what + "-" + std::to_string(k) + ".ll").c_str(), "wb"))
            {
              fwrite(text->data(), 1, text->size(), out);
              fclose(out);
            }
      }
    }
    fprintf(stderr,
            "rebuild check: %zu functions given to rebuildLLVM, %zu emitted again in %.3f s (%s), %zu modules assembled again; of all %zu "
            "from its state %zu differ, %zu functions (the build's %zu)%s%s\n",
            picked.size(), again.emittedAgain, seconds, rebuilt ? "rebuilt" : "declined", changed, texts.size(), differ, again.functions,
            result.functions, first.empty() ? "" : "; first: ", first.c_str());
  }
  session->usable = keep && keepTexts && !checkRebuild && result.errors.empty();
  return std::move(result);
}

// IR text with the numbers of its debug metadata's lines left out (`line: 12`, `scopeLine: 12` give
// `line: `, `scopeLine: `), to tell texts that differ in their lines alone.
static std::string withoutLines(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  size_t from = 0;
  for (size_t at = text.find("ine: "); at != std::string_view::npos; at = text.find("ine: ", at + 5))
  {
    if (at == 0 || (text[at - 1] != 'l' && text[at - 1] != 'L'))
      continue;
    size_t digits = at + 5;
    while (digits < text.size() && isdigit((unsigned char)text[digits]))
      ++digits;
    out.append(text.substr(from, at + 5 - from));
    from = digits;
  }
  out.append(text.substr(from));
  // An assert's line constant (Emitter::lineOperand: `@"f.line.0" = private constant i32 42, section ".kpplin$..."`), whose
  // value the patch rewrites as it does the line tables.
  static constexpr std::string_view slot = "private constant i32 ", section = ", section \".kpplin$";
  if (out.find(section) == std::string::npos)
    return out;
  std::string result;
  result.reserve(out.size());
  from = 0;
  for (size_t at = out.find(slot); at != std::string::npos; at = out.find(slot, at + slot.size()))
  {
    size_t digits = at + slot.size(), end = digits;
    while (end < out.size() && isdigit((unsigned char)out[end]))
      ++end;
    if (end == digits || out.compare(end, section.size(), section) != 0)
      continue;
    result.append(out, from, digits - from);
    from = end;
  }
  result.append(out, from, std::string::npos);
  return result;
}

// Whether two lists of texts (each sorted) are the same but for their debug lines: their texts with the
// lines left out, sorted again, equal.
template<class It> static bool sameButLines(const std::vector<std::string>& before, It from, It to)
{
  std::vector<std::string> a, b;
  for (const std::string& t : before)
    a.push_back(withoutLines(t));
  for (; from != to; ++from)
  {
    if constexpr (std::is_same_v<std::decay_t<decltype(*from)>, std::string>)
      b.push_back(withoutLines(*from));
    else
      b.push_back(withoutLines(from->text));
  }
  if (a.size() != b.size())
    return false;
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return a == b;
}

static bool sameButLines(const std::vector<std::string>& before, const std::vector<std::string>& after)
{
  return sameButLines(before, after.begin(), after.end());
}

// A constant global whose initializer read an edited function (no constructor to emit again). One
// codegen never defined: its value reached the code only through folds, each of which ran its
// initializer, the edited function too, and recorded what it ran (the interpreter's `ran` hook,
// Shared::Definition::inlined): those functions are emitted again, and the global has nothing of its
// own. One it defined is constant data in the shared declarations, every module's: its value made
// again with the new bodies (globalForOnce, what it appends then taken back), the same definition as
// the kept one changes nothing. Another value, a definition with more beside it, or none (it fails, or
// wants a constructor now), and a template's member (one global per instance), are codegen in full, as
// before 2026-11-10 every one was (Util/Vector.kpp's constructor read by CargoPod::processionSpawnOffset,
// stdlib Vector's by UtilityConstants::colorFilters).
static bool keptConstantGlobal(Shared& sh, Emitter& e, const Decl* g)
{
  if (g->kind != NodeKind::VariableDecl)
    return false;
  auto* v = const_cast<VariableDecl*>(static_cast<const VariableDecl*>(g));
  if (e.global.isTemplateContext(v))
    return false;
  if (Decl* owner = e.global.owningClass(v); owner && (!owner->templateParams.empty() || e.global.isTemplateContext(owner)))
    return false;
  // (the map's key is the declaration a use named: a static member's in its class, whose definition the
  // reader is)
  auto named = sh.globals.find(v);
  for (auto it = sh.globals.begin(); named == sh.globals.end() && it != sh.globals.end(); ++it)
    if (it->first->kind == NodeKind::VariableDecl && e.variableDefinition(static_cast<VariableDecl*>(it->first)) == v)
      named = it;
  if (named == sh.globals.end())
    return true;
  v = static_cast<VariableDecl*>(named->first);
  const std::string name = named->second;
  // The kept definition: `$name = comdat any` and `name = linkonce_odr ...`, one line each.
  std::string_view text = sh.decls.text;
  auto lineOf = [&](const std::string& head) -> std::string_view
  {
    for (size_t at = text.find(head); at != std::string_view::npos; at = text.find(head, at + 1))
      if (at == 0 || text[at - 1] == '\n')
        return text.substr(at, text.find('\n', at) - at + 1);
    return {};
  };
  std::string_view comdat = lineOf("$" + name.substr(1) + " = comdat any\n");
  std::string_view kept = lineOf(name + " = linkonce_odr ");
  if (comdat.empty() || kept.empty())
    return false;
  auto lock = e.sharedLock();
  size_t entriesBefore = sh.decls.entries.size(), errorsBefore = e.errors.size(), inlinedBefore = e.inlinedNow.size();
  size_t hiddenBefore = e.hiddenErrors;
  bool same = false;
  sh.globals.erase(named);
  std::vector<const FunctionDecl*> ran; // (what the new evaluation runs: its record anew, Shared::constantGlobals)
  Restore<std::vector<const FunctionDecl*>*> ranRestore(e.globalRan);
  e.globalRan = &ran;
  try
  {
    std::string again = e.globalForOnce(v, v, nullptr);
    std::vector<std::string>& added = sh.decls.entries;
    same = again == name && added.size() == entriesBefore + 2 && e.errors.size() == errorsBefore && e.hiddenErrors == hiddenBefore &&
           ((added[entriesBefore] == comdat && added[entriesBefore + 1] == kept) || (added[entriesBefore] == kept && added[entriesBefore + 1] == comdat));
  }
  catch (Emitter::Unsupported&)
  {
  }
  sh.decls.entries.resize(std::min(sh.decls.entries.size(), entriesBefore));
  e.errors.resize(std::min(e.errors.size(), errorsBefore));
  e.hiddenErrors = hiddenBefore;
  if (e.inlinedNow.size() > inlinedBefore)
    e.inlinedNow.resize(inlinedBefore);
  sh.globals[v] = name;
  return same;
}

static CodegenResult rebuildModules(CodegenSession& session, const std::vector<FunctionDecl*>& changedBodies, const std::vector<FunctionDecl*>& movedLines,
                                    const std::vector<FunctionDecl*>& replaced, const std::vector<FunctionDecl*>& added,
                                    const std::vector<FunctionDecl*>& removed, const std::vector<const Decl*>& changedLayouts,
                                    const std::vector<const Decl*>& instancesOf, const std::vector<const Decl*>& changedGlobals,
                                    const std::vector<const Ty*>& droppedClasses, bool& rebuilt);

CodegenResult rebuildLLVM(CodegenSession& session, const std::vector<FunctionDecl*>& changedBodies, const std::vector<FunctionDecl*>& movedLines,
                          const std::vector<FunctionDecl*>& replaced, const std::vector<FunctionDecl*>& added, const std::vector<FunctionDecl*>& removed,
                          const std::vector<const Decl*>& changedLayouts, const std::vector<const Decl*>& instancesOf,
                          const std::vector<const Decl*>& changedGlobals, const std::vector<const Ty*>& droppedClasses, bool& rebuilt)
{
  CodegenResult out =
    rebuildModules(session, changedBodies, movedLines, replaced, added, removed, changedLayouts, instancesOf, changedGlobals, droppedClasses, rebuilt);
  // The classes every module defines (the session's, made again by homeDebugClasses when this
  // build named a class), whatever path the rebuild took: one place, so that no return of it
  // gives the natvis an empty list. (A build from scratch's homeDebugClasses fills its result.)
  if (rebuilt && session.shared)
    out.debugClasses = session.shared->debugClasses;
  return out;
}

void unassembledAgain(CodegenSession& session, const std::vector<bool>& changedModules)
{
  if (!session.shared)
    return;
  Shared& sh = *session.shared;
  sh.unassembled.resize(sh.moduleCount);
  for (unsigned k = 0; k < sh.moduleCount; ++k)
    if (changedModules.empty())
      sh.unassembled[k] = true;
    else if (changedModules[k] || (sh.splitPieces && k + sh.moduleCount < changedModules.size() && changedModules[k + sh.moduleCount]))
      sh.unassembled[k] = true;
}

static CodegenResult rebuildModules(CodegenSession& session, const std::vector<FunctionDecl*>& changedBodies, const std::vector<FunctionDecl*>& movedLines,
                                    const std::vector<FunctionDecl*>& replaced, const std::vector<FunctionDecl*>& added,
                                    const std::vector<FunctionDecl*>& removed, const std::vector<const Decl*>& changedLayouts,
                                    const std::vector<const Decl*>& instancesOf, const std::vector<const Decl*>& changedGlobals,
                                    const std::vector<const Ty*>& droppedClasses, bool& rebuilt)
{
  rebuilt = false;
  CodegenResult out;
  if (!session.usable || !session.shared || session.emitters.empty())
  {
    out.declined = "no session kept from the last build";
    return out;
  }
  session.usable = false; // until this rebuild is done: one that fails leaves the session to a build from scratch
  Shared& sh = *session.shared;
  ++sh.debugGeneration; // (--debug-info's types are made again as this build names them)
  std::vector<std::unique_ptr<Emitter>>& emitters = session.emitters;
  Emitter& e = *emitters[0];
  for (auto& em : emitters)
    em->lineStarts.clear(); // the edited files' lines moved
  // Work on each emitter's own answers, the emitters on threads (one pass over the game's 32 on one thread was 0.23 s of an
  // edit of ElectricNetworkValues::computeUsage, whose old local struct's member functions are released).
  auto onEachEmitter = [&](const auto& fn)
  {
    std::atomic<size_t> next{0};
    auto work = [&](unsigned)
    {
      for (size_t i; (i = next++) < emitters.size();)
        fn(*emitters[i]);
    };
    onThreads(std::max(1u, std::min(session.threads, unsigned(emitters.size()))), work);
  };
  // What the changed and removed functions' emissions added goes (their definitions, their
  // lambdas' operators, their static locals); the changed and the added ones are emitted again,
  // each in its file's module as a program function is. A template's function (or a local class's)
  // among them, a caller the server passes, is no program function: what it is in the program are
  // its instances, emitted again below when they hold a changed body.
  std::unordered_set<std::string> owners;
  std::vector<Instantiation> level;
  auto programFunction = [&](FunctionDecl* f) -> Instantiation
  {
    auto it = sh.fileModule.find(f->file);
    return {f, {}, nullptr, it == sh.fileModule.end() ? 0 : it->second, "", "", 0};
  };
  for (FunctionDecl* f : changedBodies)
    if (e.isProgramFunction(f))
    {
      owners.insert(e.functionName(f, {}, nullptr));
      level.push_back(programFunction(f));
    }
  for (FunctionDecl* f : removed)
    if (e.isProgramFunction(f))
    {
      owners.insert(e.functionName(f, {}, nullptr));
      sh.programFunctions.erase(e.functionName(f, {}, nullptr)); // (an added one's name comes back with its emission)
    }
  for (FunctionDecl* f : added)
    if (e.isProgramFunction(f))
      level.push_back(programFunction(f));
  // Kept functions whose lines moved (--debug-info): emitted again for their line tables; their callers stay.
  for (FunctionDecl* f : movedLines)
    if (e.isProgramFunction(f) && owners.insert(e.functionName(f, {}, nullptr)).second)
      level.push_back(programFunction(f));
  // The program functions emitted again with the bodies they had (readers of an edited name, moved lines) and the global
  // constructors below: their lambdas' closures are the types they were, so what was made for those stays (until 2026-11-10 it
  // went as an old closure's, and a polymorphic local class of a template's instance made for one lost its virtual functions,
  // its vtable kept: thread F's game did not link after an edit of Entity::getDump, which EntityWithOwner's constructor reads).
  std::unordered_set<std::string> sameClosures;
  {
    std::unordered_set<const FunctionDecl*> replacedSet(replaced.begin(), replaced.end());
    for (const std::vector<FunctionDecl*>* list : {&changedBodies, &movedLines})
      for (FunctionDecl* f : *list)
        if (!replacedSet.count(f) && e.isProgramFunction(f))
          sameClosures.insert(e.functionName(f, {}, nullptr));
  }
  // The functions whose emission raised the last build's errors (a session kept across them): emitted
  // again, so that each error is given again at its place now, or goes. Those emitted again for that
  // alone must fail again (an error that goes without a change is one that a mark of something
  // emitted once hid: codegen in full).
  std::vector<std::string> erroredBefore;
  const bool hadErrors = !sh.erroring.empty();
  {
    std::unordered_set<const FunctionDecl*> gone(removed.begin(), removed.end());
    std::map<std::string, FunctionDecl*> byName(sh.erroring.begin(), sh.erroring.end());
    for (auto& [name, f] : byName)
      if (!gone.count(f) && owners.insert(name).second)
      {
        if (!e.isProgramFunction(f))
        {
          out.declined = "the function with an error " + name + " is no program function now";
          return out;
        }
        level.push_back(programFunction(f));
        erroredBefore.push_back(name);
      }
    sh.erroring.clear();
  }
  // The globals whose initializers read an edited function: their constructors (with their lambdas
  // and static InitializerList arrays) go and are emitted again, as their first use emitted them. One
  // with no constructor (a constant: folded, or in the shared declarations) stays when its value is the
  // one it had (keptConstantGlobal), else codegen is in full.
  std::vector<Shared::GlobalInit> globalsAgain;
  for (const Decl* g : changedGlobals)
  {
    auto it = sh.globalInitsOf.find(g);
    if (it == sh.globalInitsOf.end() && keptConstantGlobal(sh, e, g))
      continue;
    if (it == sh.globalInitsOf.end())
    {
      out.declined = "the global " + e.global.qualifiedName(const_cast<Decl*>(g)) + " read an edited function, and it has no constructor to emit again";
      return out;
    }
    for (const std::string& fnName : it->second)
      if (owners.insert(fnName).second)
      {
        globalsAgain.push_back(sh.globalInits.at(fnName));
        sameClosures.insert(fnName); // (its initializer is the one it had)
      }
  }
  // Each emitter's verdicts on which callees are trivial (inlined) were taken on the old bodies:
  // taken again. (A changed body that is trivial now is inlined by callers that only called it
  // before, which codegen does not know: the server passes an edited function's callers too.)
  for (auto& em : emitters)
    em->trivialDefinitions.clear();
  // The functions whose emission inlined a changed or removed body, or ran it in a constant fold
  // (Shared::Definition::inlined), hold it: emitted again too, each as it was emitted (an instance
  // in the module that asked for it).
  std::unordered_set<const FunctionDecl*> edited(changedBodies.begin(), changedBodies.end());
  edited.insert(removed.begin(), removed.end());
  // The constant globals whose evaluation ran a changed or removed body: their data made again, kept
  // when it is the same (keptConstantGlobal), else codegen in full. No walk records that a global's
  // initializer read a constructor, so the readers above miss them: until 2026-11-10 a constant `const V
  // scale = V(1.5, 2)` kept the value of the constructor before its edit, silently (`3 5` where a build
  // from scratch printed `4 6`, kpp/server_constant_global_reads_an_edited_constructor).
  {
    std::vector<std::string> ranEdited;
    for (const auto& [name, c] : sh.constantGlobals)
      if (std::any_of(c.ran.begin(), c.ran.end(), [&](const FunctionDecl* f) { return edited.count(f) != 0; }))
        ranEdited.push_back(name);
    std::sort(ranEdited.begin(), ranEdited.end());
    for (const std::string& name : ranEdited)
    {
      const Shared::ConstantGlobal c = sh.constantGlobals.at(name);
      if (c.classTy || !keptConstantGlobal(sh, e, c.v))
      {
        out.declined = "the constant global " + name + " ran an edited function, and its value is not the one kept";
        return out;
      }
    }
  }
  // An instance whose template's body (the definition it is emitted from, as written: an
  // out-of-line one, an explicit specialisation) changed is emitted again too, from the copy the
  // resolver checked again.
  // So is every instance of a template one of whose instances read a change (instancesOf: the
  // resolver checked it again, or took it out to be made anew by its first use here), and a
  // function that inlined one (its body as written is the template's).
  std::unordered_set<const Decl*> rechecked(instancesOf.begin(), instancesOf.end());
  std::unordered_set<const FunctionDecl*> linesMoved(movedLines.begin(), movedLines.end()); // (a template's: its instances again, for their lines)
  auto instanceOfEdited = [&](const Shared::Definition& d)
  {
    if (d.name != d.owner || (d.emitted->args.empty() && !d.emitted->classTy))
      return false;
    Decl* defined = e.global.definitionOf(d.emitted->f, d.emitted->args);
    const FunctionDecl* written = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<const FunctionDecl*>(defined) : d.emitted->f;
    if (edited.count(written) != 0 || edited.count(d.emitted->f) != 0 || rechecked.count(written) != 0 || rechecked.count(d.emitted->f) != 0 ||
        linesMoved.count(written) != 0 || linesMoved.count(d.emitted->f) != 0)
      return true;
    // A member function of a class declared in a function: its walk is that function's (the server names the function when the
    // member read an edited name: one that became trivial is inlined now), and its lines are in that function's.
    for (const Decl* p = d.emitted->f->parent; p; p = p->parent)
      if (p->kind == NodeKind::FunctionDecl &&
          (edited.count(static_cast<const FunctionDecl*>(p)) != 0 || linesMoved.count(static_cast<const FunctionDecl*>(p)) != 0))
        return true;
    return false;
  };
  // Changed class layouts (the resolver's changedLayouts: data members of non-template classes
  // added, removed or changed): those layouts, and every one holding one of them by value or as a
  // base (transitively), are dropped and laid out again by their first use; every function whose
  // emission used one of them (Shared::Definition::layouts) is emitted again. Declined (codegen in
  // full): a polymorphic one (its vtables and thunks hold its offsets), one that a text other than a
  // function's names (a destructor helper, a descriptor, a global's definition), a closure holding
  // one by value.
  std::unordered_set<const StructInfo*> droppedLayouts;
  std::vector<const Ty*> droppedKeys;
  std::unordered_map<const Ty*, const StructInfo*> layoutOf; // a noted type -> its layout (memo)
  auto layoutFor = [&](const Ty* t) -> const StructInfo*
  {
    t = e.types.stripRef(t);
    while (t && t->kind == TyKind::Array)
      t = t->inner;
    if (!t || !e.isClass(t))
      return nullptr;
    auto [it, isNew] = layoutOf.emplace(t, nullptr);
    if (!isNew)
      return it->second;
    if (auto known = sh.structsByType.find(t); known != sh.structsByType.end())
      return it->second = known->second;
    try
    {
      if (auto own = sh.structs.find(e.canonicalClass(t, nullptr)); own != sh.structs.end())
        it->second = &own->second;
    }
    catch (Emitter::Unsupported&)
    {
    }
    return it->second;
  };
  if (!changedLayouts.empty())
  {
    std::unordered_set<const Decl*> changedClasses(changedLayouts.begin(), changedLayouts.end());
    for (auto& [key, info] : sh.structs)
      if (key->decl && changedClasses.count(key->decl))
        droppedLayouts.insert(&info);
    for (bool grew = true; grew;)
    {
      grew = false;
      for (auto& [key, info] : sh.structs)
        if (!droppedLayouts.count(&info))
          for (const Ty* ft : info.fieldTypes)
            if (ft && !ft->isReference() && droppedLayouts.count(layoutFor(ft)))
            {
              droppedLayouts.insert(&info);
              grew = true;
              break;
            }
    }
    for (auto& [key, info] : sh.structs)
      if (droppedLayouts.count(&info))
        droppedKeys.push_back(key);
    auto decline = [&](const std::string& why)
    {
      out.declined = why;
      return out;
    };
    std::vector<std::string> droppedNames;
    for (const Ty* key : droppedKeys)
    {
      const StructInfo& info = sh.structs[key];
      if (info.polymorphic)
        return decline("a changed layout of a polymorphic class (" + e.types.toString(key, &e.symbols) + "): its vtables");
      droppedNames.push_back(info.name);
    }
    // A text other than a function's that names one of them.
    auto namesOne = [&](std::string_view text, bool typeLinesToo) -> std::string
    {
      for (const std::string& name : droppedNames)
        for (size_t at = text.find(name); at != std::string_view::npos; at = text.find(name, at + 1))
        {
          size_t after = at + name.size();
          if (after < text.size() && (std::isalnum((unsigned char)text[after]) || text[after] == '.' || text[after] == '_'))
            continue; // (a longer name)
          size_t lineStart = text.rfind('\n', at);
          lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
          if (!typeLinesToo && text.substr(lineStart, 8) == "%struct." && text.find(" = type ", lineStart) < text.find('\n', lineStart))
            continue; // a type line (the layouts holding it are dropped too)
          return name;
        }
      return {};
    };
    for (auto& [name, done] : sh.emittedShared)
      for (const std::string& dropped : droppedNames)
        if (name.find(dropped.substr(8)) != std::string::npos)
          return decline("a changed layout that " + name + " was made for");
    if (std::string named = namesOne(sh.decls.text, false); !named.empty())
      return decline("a changed layout that the shared declarations name: " + named);
    if (std::string named = namesOne(sh.shared.text, true); !named.empty())
      return decline("a changed layout that a shared definition names: " + named);
    for (unsigned k = 0; k < sh.moduleCount; ++k)
      if (std::string named = namesOne(sh.moduleDecls[k].text, true); !named.empty())
        return decline("a changed layout that module " + std::to_string(k) + "'s declarations name: " + named);
    for (auto& [t, closure] : sh.closures)
      if (closure.laidOut)
        for (size_t i = 0; i < closure.fieldTypes.size(); ++i)
          if (!closure.byRef[i] && closure.captured[i] && droppedLayouts.count(layoutFor(closure.fieldTypes[i])))
            return decline("a changed layout that a lambda holds by value");
  }
  std::map<unsigned, std::vector<Instantiation>> holding; // by depth
  auto usesDropped = [&](const Shared::Definition& d)
  {
    for (const Ty* t : d.layouts)
      if (droppedLayouts.count(layoutFor(t)))
        return true;
    return false;
  };
  // What a replaced or removed body declared (a local class or enumeration, a lambda's closure, and what those declare) no
  // code names now: a new body's are new declarations, whose functions and instances are made by their first use.
  std::unordered_set<const Decl*> goneBodies(replaced.begin(), replaced.end());
  goneBodies.insert(removed.begin(), removed.end());
  std::unordered_map<const Decl*, bool> goneDeclIn;
  auto goneDecl = [&](const Decl* d)
  {
    if (!d || goneBodies.empty())
      return false;
    auto [it, isNew] = goneDeclIn.emplace(d, false);
    if (isNew)
      for (const Decl* p = d->parent; p; p = p->parent)
        if (goneBodies.count(p))
        {
          it->second = true;
          break;
        }
    return it->second;
  };
  // An instance made for the closure of a lambda whose writer is emitted again with a new body (`applyTo<closure>`):
  // the old lambda's, which no one calls now; dropped, not emitted again (the new lambda's instance is
  // made by its first use). Not a writer emitted again with the body it had (sameClosures).
  // So is a member function of a class the old body declared, and an instance made for one of its types (`Vector<Category>`):
  // emitted again from the old declarations, they kept their names from the new ones', whose first use found them emitted (until
  // 2026-11-10 a body declaring a class was not incremental; on the way, an edit of a local struct's member function with a line
  // added before it printed what the old member gave, kpp/server_local_struct_member_edited_with_a_line_added).
  // (Every type the instance is made of, each once: the types form a DAG, a lambda nested in holders
  // to any depth; none is cut off, which would keep an instance for a closure that no longer exists.)
  std::unordered_map<const Ty*, bool> oldClosureIn;
  std::function<bool(const Ty*)> oldClosure = [&](const Ty* t)
  {
    if (!t)
      return false;
    if (auto known = oldClosureIn.find(t); known != oldClosureIn.end())
      return known->second;
    oldClosureIn[t] = false; // (a cycle, if any, holds no old closure through itself)
    bool found = false;
    if (e.isLambda(t))
      if (auto writer = sh.closureWriters.find(e.types.unqualified(t));
          writer != sh.closureWriters.end() && owners.count(writer->second.writtenIn) && !sameClosures.count(writer->second.writtenIn))
        found = true;
    found = found || goneDecl(t->decl) || oldClosure(t->inner);
    for (const Ty* a : t->args)
      found = found || oldClosure(a);
    return oldClosureIn[t] = found;
  };
  // Its name released: the new closure's instance has it, and is asked for by its first use (every
  // emitter's answers forget it too, all at once below: one pass over each emitter's answers, not one
  // per name, which an edit of the game's fmain made 7 s).
  std::unordered_set<std::string> releasedNames;
  auto releaseName = [&](const std::string& name)
  {
    sh.requested.release(name);
    releasedNames.insert(name);
  };
  auto forOldClosure = [&](const Instantiation& inst)
  {
    if (goneDecl(inst.f) || oldClosure(inst.classTy))
      return true;
    for (const auto& pair : inst.args.pairs)
      if (oldClosure(pair.second))
        return true;
    return false;
  };
  // An instance made for the closure of a lambda whose writer is emitted again for its moved lines alone (the same closure,
  // sameClosures): it owns the instances of the lambda's generic call operator it asked for (`f::__lambda_..._0::operator()<int32>`,
  // asked for by `applyAll<closure>`'s calls), whose lines are the writer's: emitted again with them, as their owner's emission
  // asks for them again (until 2026-11-10 they kept the lines of the build before: a line added above SegmentedUnitAI::isValidEnemy
  // left the debugger at the old line of its lambda's operator()<UniquePtr<SpiderLegSpecification>>).
  // (the lambda by its declaration, written in a function emitted for its moved lines, in a lambda's body there too: the generic
  // lambda SegmentedUnitAI::isValidEnemy's canTargetAnyLeg passes to Util::anyOf)
  std::unordered_set<const Decl*> movedBodies;
  for (FunctionDecl* f : movedLines)
    if (!goneBodies.count(f))
      movedBodies.insert(f);
  std::unordered_map<const Decl*, bool> movedDeclIn;
  auto movedDecl = [&](const Decl* d)
  {
    if (!d || movedBodies.empty())
      return false;
    auto [it, isNew] = movedDeclIn.emplace(d, false);
    if (isNew)
      for (const Decl* p = d->parent; p; p = p->parent)
        if (movedBodies.count(p))
        {
          it->second = true;
          break;
        }
    return it->second;
  };
  std::unordered_map<const Ty*, bool> movedClosureIn;
  std::function<bool(const Ty*)> movedClosure = [&](const Ty* t)
  {
    if (!t || movedBodies.empty())
      return false;
    if (auto known = movedClosureIn.find(t); known != movedClosureIn.end())
      return known->second;
    movedClosureIn[t] = false;
    bool found = e.isLambda(t) && movedDecl(t->decl);
    found = found || movedClosure(t->inner);
    for (const Ty* a : t->args)
      found = found || movedClosure(a);
    return movedClosureIn[t] = found;
  };
  auto forMovedClosure = [&](const Instantiation& inst)
  {
    if (movedBodies.empty())
      return false;
    if (movedClosure(inst.classTy))
      return true;
    for (const auto& pair : inst.args.pairs)
      if (movedClosure(pair.second))
        return true;
    return false;
  };
  // The classes of the members dropped for an old closure whose layouts are polymorphic: a vtable, in the modules that used it,
  // names those members (checked below: a layout kept is a vtable no one asks for its members again).
  std::vector<std::pair<const Ty*, std::string>> polymorphicDropped;
  // An instance whose own body is the edited one (its template's): emitted again below, or gone when its body is trivial now
  // (madeTrivial).
  std::vector<const Shared::Definition*> editedInstances;
  for (unsigned k = 0; k < sh.moduleCount; ++k)
    for (const Shared::Definition& d : sh.definitions[k])
    {
      if (!d.emitted || owners.count(d.owner))
        continue;
      bool forOld = forOldClosure(*d.emitted);
      bool inlinesEdited = std::any_of(d.inlined.begin(), d.inlined.end(), [&](const FunctionDecl* body) { return edited.count(body) != 0 || rechecked.count(body) != 0; });
      bool layoutDropped = !droppedLayouts.empty() && usesDropped(d);
      if (forOld || inlinesEdited || instanceOfEdited(d) || layoutDropped || forMovedClosure(*d.emitted))
      {
        owners.insert(d.owner);
        bool ownBodyEdited = false;
        if (!forOld && !inlinesEdited && !layoutDropped && d.name == d.owner && (!d.emitted->args.empty() || d.emitted->classTy))
        {
          Decl* defined = e.global.definitionOf(d.emitted->f, d.emitted->args);
          const FunctionDecl* written = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<const FunctionDecl*>(defined) : d.emitted->f;
          ownBodyEdited = edited.count(written) != 0 || edited.count(d.emitted->f) != 0;
        }
        if (ownBodyEdited)
          editedInstances.push_back(&d);
        else if (!forOld)
          holding[d.emitted->depth].push_back(*d.emitted);
        else
        {
          releaseName(d.name);
          if (const StructInfo* info = d.emitted->classTy ? layoutFor(d.emitted->classTy) : nullptr; info && info->polymorphic)
            polymorphicDropped.push_back({e.types.unqualified(d.emitted->classTy), d.name});
        }
      }
    }
  if (!droppedLayouts.empty())
  {
    // A function that used a dropped layout without a record of how it was emitted cannot be emitted
    // again: declined.
    for (unsigned k = 0; k < sh.moduleCount; ++k)
      for (const Shared::Definition& d : sh.definitions[k])
        if (!d.emitted && !owners.count(d.owner) && usesDropped(d))
        {
          out.declined = "a changed layout that " + d.name + " used, with no record of its emission";
          return out;
        }
    // The layouts go, with every answer computed from them (each emitter's and the published ones).
    for (auto it = sh.structsByType.begin(); it != sh.structsByType.end();)
      it = droppedLayouts.count(it->second) ? sh.structsByType.erase(it) : std::next(it);
    for (const Ty* key : droppedKeys)
    {
      if (sh.structs[key].complete)
        --sh.result.structs; // (counted again when laid out again: the count is a build from scratch's)
      sh.structs.erase(key);
    }
    onEachEmitter([](Emitter& em) { em.forgetLayoutAnswers(); });
    sh.finalLayouts.clear();
    sh.finalSizes.clear();
    sh.finalDestruction.clear();
  }
  // An edited function that a build from scratch inlines into every caller now (trivialDefinition, trivialConstructor) and that
  // no kept code inlined before (the definitions' records name what each inlined): the kept code that calls it is emitted again,
  // to inline it (a constructor's callers name its class, not it, so its walk's readers are not they), and an instance of an
  // edited template goes, its name released, unless code that cannot be emitted again names it (a caller's emission asks for it
  // again where the caller still calls it, an address taken). Until 2026-11-10 identity gaps: a kept caller still called a
  // constructor made trivial, and a template's instance made trivial stayed defined beside the calls that inlined it (the
  // program the same, its code not a build from scratch's).
  {
    struct MadeTrivial
    {
      std::string name;
      const Shared::Definition* instance; // (null: a program function, which is defined in any case)
      bool kept = false;                  // named where it cannot be emitted again: emitted again itself, as before
    };
    std::vector<MadeTrivial> madeTrivial;
    std::unordered_set<const FunctionDecl*> inlinedBefore;
    bool inlinedKnown = false;
    auto wasInlined = [&](const FunctionDecl* f)
    {
      if (!inlinedKnown)
      {
        for (unsigned k = 0; k < sh.moduleCount; ++k)
          for (const Shared::Definition& d : sh.definitions[k])
            inlinedBefore.insert(d.inlined.begin(), d.inlined.end());
        inlinedKnown = true;
      }
      return inlinedBefore.count(f) != 0;
    };
    auto trivialNow = [&](FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      try
      {
        if (f->flags & DF_Constructor)
          return classTy && e.trivialConstructor(f, args, classTy, f) != nullptr;
        return e.trivialDefinition(f, args, classTy) != nullptr;
      }
      catch (Emitter::Unsupported&)
      {
        return false;
      }
    };
    for (FunctionDecl* f : replaced)
      if ((f->flags & DF_Constructor) && e.isProgramFunction(f) && !wasInlined(f))
        if (Decl* cls = e.global.owningClass(f); cls && cls->kind == NodeKind::ClassDecl && cls->templateParams.empty())
          if (const Ty* classTy = e.canonicalClass(e.types.classType(cls, {}), f); trivialNow(f, {}, classTy))
            madeTrivial.push_back({e.functionName(f, {}, nullptr), nullptr});
    for (const Shared::Definition* d : editedInstances)
    {
      Decl* defined = e.global.definitionOf(d->emitted->f, d->emitted->args);
      const FunctionDecl* written = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<const FunctionDecl*>(defined) : d->emitted->f;
      if (!(d->emitted->f->flags & DF_Virtual) && !wasInlined(written) && trivialNow(d->emitted->f, d->emitted->args, d->emitted->classTy))
        madeTrivial.push_back({d->name, d});
      else
        holding[d->emitted->depth].push_back(*d->emitted);
    }
    if (!madeTrivial.empty())
    {
      std::unordered_map<std::string_view, size_t> byName;
      for (size_t i = 0; i < madeTrivial.size(); ++i)
        byName.emplace(madeTrivial[i].name, i);
      for (unsigned k = 0; k < sh.moduleCount; ++k)
        for (const Shared::Definition& d : sh.definitions[k])
        {
          if (owners.count(d.owner))
            continue;
          for (auto [at, length] : d.names)
            if (auto it = byName.find(std::string_view(d.text).substr(at, length)); it != byName.end())
            {
              if (d.emitted)
              {
                owners.insert(d.owner);
                holding[d.emitted->depth].push_back(*d.emitted);
                break;
              }
              madeTrivial[it->second].kept = true;
            }
        }
      // (a text that is no function's names it otherwise than in a declaration: a vtable, a static local's or a global's
      // initializer)
      auto namedIn = [](std::string_view text, std::string_view name)
      {
        for (size_t at = text.find(name); at != std::string_view::npos; at = text.find(name, at + 1))
        {
          size_t line = text.rfind('\n', at);
          line = line == std::string_view::npos ? 0 : line + 1;
          if (text.substr(line, 8) != "declare ")
            return true;
        }
        return false;
      };
      for (MadeTrivial& m : madeTrivial)
        if (m.instance && !m.kept)
        {
          m.kept = namedIn(sh.shared.text, m.name) || namedIn(sh.decls.text, m.name);
          for (unsigned k = 0; !m.kept && k < sh.moduleCount; ++k)
          {
            m.kept = namedIn(sh.moduleDecls[k].text, m.name);
            for (const Shared::OwnedEntry& o : sh.ownedEntries[k])
              m.kept = m.kept || namedIn(o.text, m.name);
          }
        }
      for (const MadeTrivial& m : madeTrivial)
        if (m.instance && m.kept)
          holding[m.instance->emitted->depth].push_back(*m.instance->emitted);
        else if (m.instance)
          releaseName(m.name);
    }
  }
  // Per module, the texts dropped (sorted) and how many definitions and owned entries stayed: the
  // ones emitted now follow those, and a module whose new texts are the dropped ones again (an
  // edited function's callers, emitted again for what they inline, mostly are) is not assembled again.
  struct Dropped
  {
    std::vector<std::string> definitions, owned;
    size_t keptDefinitions = 0, keptOwned = 0;
  };
  std::vector<Dropped> dropped(sh.moduleCount);
  std::unordered_set<std::string> droppedNames;
  std::vector<const Ty*> droppedUses; // the layouts the dropped definitions used (unusedLayouts, below)
  size_t droppedFunctions = 0;
  // The call operators emitted on demand whose writer is emitted again: emitted again on their own
  // (the writer may never make their closure itself).
  std::unordered_set<std::string> removedNames;
  for (FunctionDecl* f : removed)
    removedNames.insert(e.functionName(f, {}, nullptr));
  struct Operator
  {
    std::string name;
    const Ty* closure;
    const Ty* callType;
  };
  std::vector<Operator> operators;
  for (unsigned k = 0; k < sh.moduleCount; ++k)
  {
    Dropped& was = dropped[k];
    std::vector<Shared::Definition>& defs = sh.definitions[k];
    size_t kept = 0;
    for (size_t i = 0; i < defs.size(); ++i)
    {
      if (owners.count(defs[i].owner))
      {
        droppedNames.insert(defs[i].name);
        droppedUses.insert(droppedUses.end(), defs[i].layouts.begin(), defs[i].layouts.end());
        droppedFunctions += defs[i].counted;
        if (defs[i].emitted && forOldClosure(*defs[i].emitted))
          releaseName(defs[i].name); // (an instance for the old lambda's closure)
        if (defs[i].closure && !removedNames.count(defs[i].owner))
        {
          // (not the old lambda's of a writer emitted again: the new lambda's operator is emitted by its first use)
          auto writer = sh.closureWriters.find(defs[i].closure);
          if (writer == sh.closureWriters.end() || !owners.count(writer->second.writtenIn))
            operators.push_back({defs[i].name, defs[i].closure, defs[i].callType});
        }
        was.definitions.push_back(std::move(defs[i].text));
        continue;
      }
      if (kept != i)
        defs[kept] = std::move(defs[i]);
      ++kept;
    }
    defs.erase(defs.begin() + kept, defs.end()); // the kept ones still in order
    was.keptDefinitions = kept;
    std::vector<Shared::OwnedEntry>& owned = sh.ownedEntries[k];
    kept = 0;
    for (size_t i = 0; i < owned.size(); ++i)
    {
      if (owners.count(owned[i].owner))
      {
        was.owned.push_back(std::move(owned[i].text));
        continue;
      }
      if (kept != i)
        owned[kept] = std::move(owned[i]);
      ++kept;
    }
    owned.erase(owned.begin() + kept, owned.end());
    was.keptOwned = kept;
    std::sort(was.definitions.begin(), was.definitions.end());
    std::sort(was.owned.begin(), was.owned.end());
  }
  if (!releasedNames.empty())
    onEachEmitter([&](Emitter& em)
    {
      std::erase_if(em.usedFunctions, [&](const auto& used) { return releasedNames.count(used.first) || releasedNames.count(used.second.name); });
    });
  // A closure of a function emitted again: its name goes to the closure the function's new walk makes
  // (the same lambda: irTypeName would give it `.2`), whose type line, when its layout is the same, is
  // already in the shared declarations (below).
  // So do the types made from such a closure (`Holder<closure>`, a std::function's implementation).
  // (A writer emitted again with the body it had keeps its closures' types and what was made of them: its call operators alone
  // go, emitted again by its emission.)
  std::unordered_set<const Ty*> droppedTypes;
  for (auto& [t, info] : sh.closures)
    if (auto writer = sh.closureWriters.find(t);
        droppedNames.count(info.functionName) || (writer != sh.closureWriters.end() && owners.count(writer->second.writtenIn))) // (a generic lambda's operators are instances: by its writer)
    {
      sh.closuresEmitted.erase(t);
      if (writer == sh.closureWriters.end() || !sameClosures.count(writer->second.writtenIn))
        droppedTypes.insert(t);
    }
  // So do the class instances the resolver took out (no one asks for them now: `Boxed<int>` after an
  // edit made it `Boxed<long long>`): their layouts and names go (a build from scratch has neither).
  for (const Ty* t : droppedClasses)
  {
    droppedTypes.insert(t);
    droppedTypes.insert(e.types.unqualified(t));
  }
  if (!droppedTypes.empty())
  {
    // (each type once, to any depth: a type that keeps a name it should give back gets `.2` beside it)
    std::unordered_map<const Ty*, bool> mentionsIn;
    std::function<bool(const Ty*)> mentions = [&](const Ty* t)
    {
      if (!t)
        return false;
      if (auto known = mentionsIn.find(t); known != mentionsIn.end())
        return known->second;
      mentionsIn[t] = false;
      bool found = droppedTypes.count(t) || droppedTypes.count(e.types.unqualified(t)) || goneDecl(t->decl) || mentions(t->inner);
      for (const Ty* a : t->args)
        found = found || mentions(a);
      return mentionsIn[t] = found;
    };
    // A member of a polymorphic class dropped for an old closure whose layout stays (a local class of the instance made for it:
    // its type names no closure): the class's vtable names the member and nothing asks for it again: codegen in full.
    for (auto& [cls, name] : polymorphicDropped)
      if (!mentions(cls))
      {
        out.declined = "a member function of a polymorphic class made for an edited function's lambda, which its kept vtable names (" + name + ")";
        return out;
      }
    std::erase_if(sh.irTypeNames, [&](const auto& named) { return mentions(named.second); });
    // Their layouts go too (`Hold<closure>`, laid out for the old closure): a build from scratch never
    // lays them out, nor counts them; every answer computed from them with them.
    std::unordered_set<const StructInfo*> goneLayouts;
    std::vector<const Ty*> goneKeys;
    for (auto& [key, info] : sh.structs)
      if (mentions(key))
      {
        goneLayouts.insert(&info);
        goneKeys.push_back(key);
      }
    if (!goneKeys.empty())
    {
      for (auto it = sh.structsByType.begin(); it != sh.structsByType.end();)
        it = goneLayouts.count(it->second) ? sh.structsByType.erase(it) : std::next(it);
      for (const Ty* key : goneKeys)
      {
        if (sh.structs[key].complete)
          --sh.result.structs;
        sh.structs.erase(key);
      }
      onEachEmitter([](Emitter& em) { em.forgetLayoutAnswers(); });
      sh.finalLayouts.clear();
      sh.finalSizes.clear();
      sh.finalDestruction.clear();
    }
  }
  for (const std::string& name : droppedNames)
  {
    sh.lambdaInstances.erase(name);
    sh.definedIn.erase(name);
    sh.declareLines.erase(name);
  }
  // After errors, a lambda's operator that failed is marked emitted without a definition: emitted
  // again by its writer (a function with an error, emitted again above).
  if (hadErrors)
  {
    for (auto& [t, info] : sh.closures)
      if (!sh.definedIn.count(info.functionName))
        sh.closuresEmitted.erase(t);
    std::erase_if(sh.lambdaInstances, [&](const std::string& name) { return !sh.definedIn.count(name); });
  }
  sh.functions -= std::min<size_t>(sh.functions, droppedFunctions);
  size_t sharedBefore = sh.decls.entries.size() + sh.shared.entries.size();
  // (the exception runtime is written once, after a build from scratch emitted every function
  // (finishExceptionRuntime): a first throw, or on Windows a first noexcept function whose catchpad
  // names kpp.eh.filter.noexcept, needs it written again, which a rebuild does not do)
  bool exceptionsBefore = sh.usesExceptions, noexceptFilterBefore = sh.usesNoexceptFilter;
  for (auto& em : emitters)
  {
    em->errors.clear();
    em->failedFunctions.clear();
    em->failedAt.clear();
    em->erroring.clear();
    em->unattributedErrors = false;
    em->hiddenErrors = 0;
  }
  // Level by level as the build emitted them: the functions of depth d with the first uses those of
  // depth d - 1 asked for, after all of those (a closure's call operator goes to the function that
  // uses it first, as the build decided it level by level).
  for (unsigned depth = 0;; ++depth)
  {
    if (auto it = holding.find(depth); it != holding.end())
    {
      for (Instantiation& item : it->second)
        level.push_back(std::move(item));
      holding.erase(it);
    }
    if (level.empty() && (depth > 0 || operators.empty()))
    {
      if (holding.empty())
        break;
      depth = holding.begin()->first - 1; // nothing asked for in between
      continue;
    }
    out.emittedAgain += level.size();
    std::atomic<size_t> next{0};
    auto work = [&](unsigned i)
    {
      for (size_t k; (k = next++) < level.size();)
        emitInstantiation(*emitters[i], level[k]);
    };
    onThreads(level.size() < 64 ? 1u : session.threads, work);
    if (depth == 0)
      for (const Shared::GlobalInit& g : globalsAgain)
      {
        Emitter& em = *emitters[0];
        try
        {
          em.startupAt = g.startupAt;
          em.emitGlobalConstructor(g.v, g.name, g.t, g.init, g.at, g.perThread, g.definition, g.build);
          em.startupAt.reset();
        }
        catch (Emitter::Unsupported&)
        {
          em.startupAt.reset();
          out.declined = "the constructor of " + g.name + " could not be emitted";
          return out;
        }
        ++out.emittedAgain;
      }
    if (depth == 0)
      for (const Operator& o : operators) // (none made again by its writer meanwhile)
      {
        try
        {
          emitters[0]->emitCallOperatorOnDemand(o.closure, o.callType, o.name);
        }
        catch (Emitter::Unsupported&)
        {
          out.declined = "the call operator " + o.name + " could not be emitted";
          return out;
        }
        ++out.emittedAgain;
      }
    collectNextLevel(emitters, level);
  }
  gatherProgramFunctions(sh, emitters);
  // The program's entry names main's line in its debug information (emitEntry): written again with main.
  if (sh.debugInfo)
    for (const char* name : {"@kpp.main", "@kpp.wmain"})
      if (owners.count(name))
        e.emitEntry(name);
  // An error, or a first use that added to the shared declarations (in every module): from scratch.
  // What it was goes to the server's log (the first one, cut).
  auto cut = [](const std::string& text) { return text.substr(0, std::min(text.find('\n'), size_t(240))); };
  // The dropped layouts not laid out again by a function emitted again: laid out now (a type line
  // names them). Each new type line replaces the old one in the shared declarations' text, where it
  // sorts the same (its name), and the modules are assembled again with them.
  bool layoutsReplaced = false;
  if (!droppedKeys.empty())
  {
    for (const Ty* key : droppedKeys)
      if (!sh.structs.count(key))
        try
        {
          emitters[0]->structFor(key, key->decl);
        }
        catch (Emitter::Unsupported&)
        {
        }
    std::vector<std::string> remaining;
    for (std::string& entry : sh.decls.entries)
    {
      size_t is = entry.find(" = type ");
      if (!entry.starts_with("%struct.") || is == std::string::npos || entry.find('\n') + 1 != entry.size())
      {
        remaining.push_back(std::move(entry));
        continue;
      }
      std::string head = entry.substr(0, is + 8);
      size_t at = sh.decls.text.starts_with(head) ? 0 : sh.decls.text.find("\n" + head);
      if (at == std::string::npos)
      {
        remaining.push_back(std::move(entry)); // a new type: a first use (declined below)
        continue;
      }
      at += at ? 1 : 0;
      sh.decls.text.replace(at, sh.decls.text.find('\n', at) + 1 - at, entry);
      layoutsReplaced = true;
    }
    sh.decls.entries = std::move(remaining);
    if (layoutsReplaced)
    {
      sh.typeLines.clear();
      sh.typeIndex.clear();
      e.prepareModuleTexts();
    }
  }
  // Errors keep the session when they came from program functions' own emissions (as generate keeps
  // it), each of those emitted again only for its last error failing again.
  bool errors = false;
  std::unordered_map<std::string, FunctionDecl*> erroring;
  for (auto& em : emitters)
    if (!em->errors.empty() || em->unattributedErrors || em->hiddenErrors)
    {
      errors = true;
      if (em->unattributedErrors || sh.despiteErrors || sh.failuresRemembered)
      {
        out.declined = "an error: " + cut(!em->errors.empty() ? em->errors.front() : em->hiddenErrors ? std::string("(one following from a checker error)") : std::string("(gone)"));
        return out;
      }
      erroring.insert(em->erroring.begin(), em->erroring.end());
    }
  for (const std::string& name : erroredBefore)
    if (!erroring.count(name))
    {
      out.declined = "the error of " + name + " went without a change to it";
      return out;
    }
  // The layouts only the dropped definitions used, which no definition uses now (a local `Boxed
  // extra(2.5)` taken out: Boxed<double>), nor another layout or a closure holds: a build from scratch
  // never lays them out, nor counts them. (One needed for a use no definition notes is laid out again
  // by that use.)
  if (!droppedUses.empty())
  {
    std::unordered_set<const Ty*> unusedLayouts;
    for (const Ty* t : droppedUses)
      if (sh.structs.count(t))
        unusedLayouts.insert(t);
    for (unsigned k = 0; k < sh.moduleCount && !unusedLayouts.empty(); ++k)
      for (const Shared::Definition& d : sh.definitions[k])
        for (const Ty* t : d.layouts)
          unusedLayouts.erase(t);
    // (a field's class, also one held in an array: `RowCell cells[2]`)
    auto held = [&](const Ty* f)
    {
      while (f && f->kind == TyKind::Array)
        f = f->inner;
      if (f)
        unusedLayouts.erase(e.types.unqualified(f));
    };
    for (auto& [key, info] : sh.structs)
      for (const Ty* f : info.fieldTypes)
        held(f);
    for (auto& [t, info] : sh.closures)
      for (const Ty* f : info.fieldTypes)
        held(f);
    if (!unusedLayouts.empty())
    {
      std::unordered_set<const StructInfo*> gone;
      for (const Ty* t : unusedLayouts)
        gone.insert(&sh.structs[t]);
      for (auto it = sh.structsByType.begin(); it != sh.structsByType.end();)
        it = gone.count(it->second) ? sh.structsByType.erase(it) : std::next(it);
      for (const Ty* t : unusedLayouts)
      {
        if (sh.structs[t].complete)
          --sh.result.structs;
        sh.structs.erase(t);
      }
      onEachEmitter([](Emitter& em) { em.forgetLayoutAnswers(); });
      sh.finalLayouts.clear();
      sh.finalSizes.clear();
      sh.finalDestruction.clear();
    }
  }
  // A type line the shared declarations hold as it is (a closure laid out again by its function,
  // emitted again) adds nothing.
  std::erase_if(sh.decls.entries, [&](const std::string& entry)
  {
    size_t is = entry.find(" = type ");
    if (is == std::string::npos || entry.find('\n') + 1 != entry.size())
      return false;
    auto it = sh.typeIndex.find(std::string_view(entry).substr(0, is));
    return it != sh.typeIndex.end() && sh.typeLines[it->second].line == std::string_view(entry).substr(0, entry.size() - 1);
  });
  // A type line of a new name (a class instance laid out first by the edit, `Boxed<long long>`): into
  // the shared declarations' text where it sorts among the type lines (each one entry of one line),
  // the type index made again. A module takes the type lines it names: only those naming it change.
  std::vector<std::string> newTypes;
  std::erase_if(sh.decls.entries, [&](std::string& entry)
  {
    size_t is = entry.find(" = type ");
    if (is == std::string::npos || entry.find('\n') + 1 != entry.size() || !(entry.starts_with("%struct.") || entry.starts_with("%closure.")) ||
        sh.typeLines.empty() || sh.typeIndex.count(std::string_view(entry).substr(0, is)))
      return false;
    newTypes.push_back(std::move(entry));
    return true;
  });
  if (!newTypes.empty())
  {
    std::sort(newTypes.begin(), newTypes.end());
    std::string_view text = sh.decls.text;
    std::vector<size_t> at; // per new line: the offset it goes in at
    for (const std::string& line : newTypes)
    {
      std::string_view own = std::string_view(line).substr(0, line.size() - 1);
      auto after = std::upper_bound(sh.typeLines.begin(), sh.typeLines.end(), own, [](std::string_view l, const Shared::TypeLine& t) { return l < t.line; });
      at.push_back(after != sh.typeLines.end() ? size_t(after->line.data() - text.data())
                                               : size_t(sh.typeLines.back().line.data() + sh.typeLines.back().line.size() - text.data()) + 1);
    }
    std::string merged;
    merged.reserve(text.size() + newTypes.size() * 64);
    size_t from = 0;
    for (size_t i = 0; i < newTypes.size(); ++i)
    {
      merged.append(text.substr(from, at[i] - from));
      merged += newTypes[i];
      from = at[i];
    }
    merged.append(text.substr(from));
    sh.decls.text = std::move(merged);
    sh.typeLines.clear();
    sh.typeIndex.clear();
    e.prepareModuleTexts();
  }
  if ((!exceptionsBefore && sh.usesExceptions) || (!noexceptFilterBefore && sh.usesNoexceptFilter))
  {
    out.declined = !exceptionsBefore && sh.usesExceptions ? "a first throw: the exception runtime is written again"
                                                         : "a first noexcept function's filter (kpp.eh.filter.noexcept): the exception runtime is written again";
    return out;
  }
  if (sh.decls.entries.size() + sh.shared.entries.size() != sharedBefore)
  {
    std::vector<std::string> added = sh.decls.entries;
    added.insert(added.end(), sh.shared.entries.begin(), sh.shared.entries.end());
    std::sort(added.begin(), added.end());
    out.declined = std::to_string(added.size()) + " first use(s) added to the shared declarations, the first: " + cut(added.front());
    return out;
  }
  std::vector<unsigned> which;
  std::vector<bool> linesOnly(sh.moduleCount, false); // (texts changed in their debug lines alone: linesOnlyModules)
  for (unsigned k = 0; k < sh.moduleCount; ++k)
  {
    Dropped& was = dropped[k];
    std::vector<Shared::Definition>& defs = sh.definitions[k];
    std::vector<Shared::OwnedEntry>& owned = sh.ownedEntries[k];
    bool declared = !sh.moduleDecls[k].entries.empty(); // a first use declared something in this module
    if (was.definitions.empty() && was.owned.empty() && defs.size() == was.keptDefinitions && owned.size() == was.keptOwned && !declared &&
        !layoutsReplaced)
      continue;
    // The new definitions sorted and merged in among the kept ones (as from scratch, all in order).
    auto byText = [](const Shared::Definition& a, const Shared::Definition& b) { return a.text < b.text; };
    std::sort(defs.begin() + was.keptDefinitions, defs.end(), byText);
    bool same = !declared && defs.size() - was.keptDefinitions == was.definitions.size() &&
                std::equal(was.definitions.begin(), was.definitions.end(), defs.begin() + was.keptDefinitions,
                           [](const std::string& a, const Shared::Definition& b) { return a == b.text; });
    // (or the same but for its debug lines: a function emitted again because an edit moved its lines,
    // its code the one it had; the server patches the object's line tables, LinePatch.hpp)
    bool linesCandidate = !same && !declared && !layoutsReplaced && sh.fullDebugInfo && !sh.importableModules &&
                          defs.size() - was.keptDefinitions == was.definitions.size();
    if (same || linesCandidate)
    {
      std::vector<std::string> ownedNow;
      for (size_t i = was.keptOwned; i < owned.size(); ++i)
        ownedNow.push_back(owned[i].text);
      std::sort(ownedNow.begin(), ownedNow.end());
      if (same)
        same = ownedNow == was.owned;
      else
        linesOnly[k] = sameButLines(was.definitions, defs.begin() + was.keptDefinitions, defs.end()) && sameButLines(was.owned, ownedNow);
    }
    std::inplace_merge(defs.begin(), defs.begin() + was.keptDefinitions, defs.end(), byText);
    if (same && !layoutsReplaced)
      continue; // its text would be the one it has
    which.push_back(k);
    // The module's declarations again: those kept, those a first use added now (kept from here on),
    // and its functions' own.
    std::vector<std::string>& kept = sh.keptModuleEntries[k];
    kept.insert(kept.end(), sh.moduleDecls[k].entries.begin(), sh.moduleDecls[k].entries.end());
    dropSharedLiterals(sh, kept);
    std::vector<std::string> entries = kept;
    appendOwnedEntries(sh, k, entries);
    sh.moduleDecls[k].entries = std::move(entries);
    sh.moduleDecls[k].text.clear();
    sh.moduleDecls[k].finish();
  }
  // --debug-info: a global defined in a file whose lines an edit moved has its DIGlobalVariable's line made again
  // (Emitter::debugGlobal); one that changed changes its home module's text, in its debug metadata alone (a
  // DIGlobalVariable's line is in no CodeView record: its object stays the one it has, patched). Until
  // 2026-11-10 it kept the line of the build before (--verify-incremental: the module's text not a build from
  // scratch's).
  if (sh.fullDebugInfo && sh.homeGlobals && !movedLines.empty())
  {
    std::unordered_set<const ParsedFile*> movedFiles;
    for (const FunctionDecl* f : movedLines)
      movedFiles.insert(f->file);
    for (auto& [decl, name] : sh.globals)
    {
      if (decl->kind != NodeKind::VariableDecl)
        continue;
      VariableDecl* def = e.variableDefinition(static_cast<VariableDecl*>(decl));
      if (!def || !movedFiles.count(def->file))
        continue;
      auto home = sh.homedGlobals.find(name);
      const Ty* t = e.global.typeOfDecl(decl);
      if (home == sh.homedGlobals.end() || !t)
        continue;
      uint64_t key = debugGlobalKey(name);
      auto kept = sh.debugGlobals.find(key);
      if (kept == sh.debugGlobals.end())
        continue;
      std::string before = kept->second;
      e.debugGlobal(name, def, t);
      if (sh.debugGlobals[key] != before && std::find(which.begin(), which.end(), home->second.home) == which.end())
      {
        which.push_back(home->second.home);
        linesOnly[home->second.home] = true;
      }
    }
    std::sort(which.begin(), which.end());
  }
  // Placed again (Emitter::placeShortDefinitions): a definition placed elsewhere than before changes
  // both modules, and a module whose definitions changed changes those its definitions are placed in.
  if (sh.importableModules)
  {
    std::unordered_map<std::string, unsigned> before = sh.placedIn;
    e.placeShortDefinitions(session.threads);
    auto at = [&](const std::unordered_map<std::string, unsigned>& placed, const std::string& name)
    {
      if (auto it = placed.find(name); it != placed.end())
        return it->second;
      auto in = sh.definedIn.find(name);
      return in == sh.definedIn.end() ? 0u : in->second;
    };
    std::vector<unsigned> also;
    for (const auto* placed : {&before, &sh.placedIn})
      for (auto& [name, module] : *placed)
        if (at(before, name) != at(sh.placedIn, name))
        {
          also.push_back(at(before, name));
          also.push_back(at(sh.placedIn, name));
        }
    for (unsigned k : which)
      for (const Shared::Definition& d : sh.definitions[k])
        if (std::string_view header = std::string_view(d.text).substr(0, d.text.find('\n')); header.find('@') != std::string_view::npos)
        {
          size_t from = header.find('@');
          size_t end = header[from + 1] == '"' ? header.find('"', from + 2) + 1 : header.find('(', from);
          std::string name(header.substr(from, end - from));
          also.push_back(at(before, name));
          also.push_back(at(sh.placedIn, name));
        }
    which.insert(which.end(), also.begin(), also.end());
    for (unsigned k : also)
      if (k < linesOnly.size())
        linesOnly[k] = false;
    std::sort(which.begin(), which.end());
    which.erase(std::unique(which.begin(), which.end()), which.end());
  }
  sh.erroring = std::move(erroring);
  sh.unassembled.resize(sh.moduleCount);
  if (errors)
  {
    // Nothing assembled (a build with errors writes no module): the modules that changed are
    // assembled by the first build without errors.
    for (unsigned k : which)
      sh.unassembled[k] = true;
    for (auto& em : emitters)
    {
      out.errors.insert(out.errors.end(), em->errors.begin(), em->errors.end());
      out.failedFunctions.insert(out.failedFunctions.end(), em->failedFunctions.begin(), em->failedFunctions.end());
      out.hiddenErrors += em->hiddenErrors;
    }
    sortDiagnostics(out.errors);
    std::sort(out.failedFunctions.begin(), out.failedFunctions.end());
    out.functions = sh.functions;
    out.structs = session.result.structs;
    session.usable = true;
    rebuilt = true;
    return out;
  }
  for (unsigned k = 0; k < sh.moduleCount; ++k)
    if (sh.unassembled[k])
    {
      which.push_back(k);
      linesOnly[k] = false; // (what an earlier build left unassembled changed otherwise)
    }
  std::sort(which.begin(), which.end());
  which.erase(std::unique(which.begin(), which.end()), which.end());
  sh.unassembled.assign(sh.moduleCount, false);
  // A module changed: both its pieces assembled again (the one whose text is the same keeps its object).
  const unsigned outputs = sh.outputModules();
  std::vector<unsigned> assembled = which;
  if (sh.splitPieces)
    for (unsigned k : which)
      assembled.push_back(k + sh.moduleCount);
  out.modules.resize(outputs);
  out.changedModules.assign(outputs, false);
  std::optional<PieceSources> sources;
  if (sh.splitPieces)
    sources = pieceSources(sh, false);
  std::vector<std::string> pieceErrors(assembled.size());
  std::atomic<size_t> nextModule{0};
  auto assemble = [&](unsigned t)
  {
    for (size_t i; (i = nextModule++) < assembled.size();)
      out.modules[assembled[i]] = outputText(*emitters[t], assembled[i], session.prefix, sources ? &*sources : nullptr, &pieceErrors[i]);
  };
  onThreads(std::max(1u, std::min(session.threads, unsigned(assembled.size()))), assemble);
  for (const std::string& errors : pieceErrors)
    if (!errors.empty())
    {
      out.declined = errors.substr(0, errors.find('\n')); // (the build from scratch reports them all)
      return out;
    }
  for (unsigned i : assembled)
    out.changedModules[i] = true;
  out.linesOnlyModules.assign(outputs, false);
  for (unsigned k : which)
    if (linesOnly[k])
    {
      out.linesOnlyModules[k] = true;
      if (sh.splitPieces)
        out.linesOnlyModules[k + sh.moduleCount] = true;
    }
  out.functions = sh.functions;
  out.structs = session.result.structs;
  session.usable = true;
  rebuilt = true;
  return out;
}
