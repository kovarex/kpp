# Patches a scratch copy of the kpp sources with a per-template profile (front-end exclusive time and
# typed expressions per template; codegen emit time per template), written at the end of a run to
# %KPROF_OUT% (default kprof.txt) as a tab-separated table. Never apply it to the repository's sources.
# Usage: copy compiler\ somewhere, then python -X utf8 -I prof_patch.py <copy>\src and build the copy
# with its build.cmd. Written against master 89058a6; an anchor that no longer matches stops the patch.
import sys, os

src = sys.argv[1]

def patch(name, edits):
    path = os.path.join(src, name)
    data = open(path, 'rb').read()
    crlf = b'\r\n' in data
    text = data.decode('utf-8').replace('\r\n', '\n')
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            raise SystemExit(f'{name}: anchor found {n} times: {old[:80]!r}')
        text = text.replace(old, new)
    if crlf:
        text = text.replace('\n', '\r\n')
    open(path, 'wb').write(text.encode('utf-8'))
    print('patched', name)

PROF = r'''#include <unordered_map>
#include <vector>
// PROFILE (scratch copy, not for the repository): exclusive front-end time and typed expressions per template.
std::atomic<uint64_t> g_kprofResolveNanos{0};
std::unordered_map<const void*, std::pair<uint64_t, uint64_t>> g_codegenProf; // key -> (nanos, functions), from codegen
namespace kprof
{
  struct Entry { uint64_t nanos = 0, exprs = 0, count = 0, cgNanos = 0, cgCount = 0; };
  struct Frame { const void* key; std::chrono::steady_clock::time_point start; uint64_t child; };
  struct Local { std::unordered_map<const void*, Entry> map; std::vector<Frame> stack; };
  std::mutex mutex;
  std::vector<Local*> all;
  Local& local()
  {
    thread_local Local* l = nullptr;
    if (!l)
    {
      l = new Local();
      std::lock_guard g(mutex);
      all.push_back(l);
    }
    return *l;
  }
  void push(const void* key) { local().stack.push_back({key, std::chrono::steady_clock::now(), 0}); }
  void pop()
  {
    Local& l = local();
    Frame f = l.stack.back();
    l.stack.pop_back();
    uint64_t elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - f.start).count());
    Entry& e = l.map[f.key];
    e.nanos += elapsed > f.child ? elapsed - f.child : 0;
    ++e.count;
    if (!l.stack.empty())
      l.stack.back().child += elapsed;
  }
  void expr()
  {
    Local& l = local();
    ++l.map[l.stack.empty() ? nullptr : l.stack.back().key].exprs;
  }
}
'''

DUMP = r'''
void GlobalScope::profileDump()
{
  std::unordered_map<const void*, kprof::Entry> total;
  for (kprof::Local* l : kprof::all)
    for (auto& [k, e] : l->map)
    {
      kprof::Entry& t = total[k];
      t.nanos += e.nanos;
      t.exprs += e.exprs;
      t.count += e.count;
    }
  for (auto& [k, v] : g_codegenProf)
  {
    kprof::Entry& t = total[k];
    t.cgNanos += v.first;
    t.cgCount += v.second;
  }
  uint64_t feNanos = 0, feExprs = 0, cgNanos = 0, cgCount = 0;
  for (auto& [k, e] : total)
  {
    feNanos += e.nanos;
    feExprs += e.exprs;
    cgNanos += e.cgNanos;
    cgCount += e.cgCount;
  }
  std::vector<std::pair<const void*, kprof::Entry>> rows(total.begin(), total.end());
  std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second.nanos + a.second.cgNanos > b.second.nanos + b.second.cgNanos; });
  const char* path = getenv("KPROF_OUT") ? getenv("KPROF_OUT") : "kprof.txt";
  FILE* out = fopen(path, "w");
  if (!out)
    return;
  fprintf(out, "resolveFile CPU (all threads, inclusive of instantiation checks) %.3f s\n", g_kprofResolveNanos.load() / 1e9);
  fprintf(out, "instantiation checks, exclusive CPU %.3f s; expressions typed %llu (all, incl. outside instantiations)\n", feNanos / 1e9,
          (unsigned long long)feExprs);
  fprintf(out, "codegen emitFunction %.3f s for %llu functions\n", cgNanos / 1e9, (unsigned long long)cgCount);
  fprintf(out, "key\tinstantiations\tfe_seconds\tfe_pct\texprs\texprs_pct\tcg_seconds\tcg_pct\tcg_functions\n");
  for (auto& [k, e] : rows)
  {
    std::string name = k ? qualifiedName((Decl*)k) : std::string("(not in an instantiation / not a template)");
    fprintf(out, "%s\t%llu\t%.4f\t%.3f\t%llu\t%.3f\t%.4f\t%.3f\t%llu\n", name.c_str(), (unsigned long long)e.count, e.nanos / 1e9,
            100.0 * e.nanos / std::max<uint64_t>(1, feNanos), (unsigned long long)e.exprs, 100.0 * e.exprs / std::max<uint64_t>(1, feExprs),
            e.cgNanos / 1e9, 100.0 * e.cgNanos / std::max<uint64_t>(1, cgNanos), (unsigned long long)e.cgCount);
  }
  fclose(out);
}
'''

patch('resolver.cpp', [
    ('#include <unordered_set>\n\nvoid ResolveStats::add', '#include <unordered_set>\n' + PROF + '\nvoid ResolveStats::add'),
    ('      e->ty = t;\n      if (!t->isKnown())\n        ++stats.exprsUnknown;',
     '      e->ty = t;\n      kprof::expr();\n      if (!t->isKnown())\n        ++stats.exprsUnknown;'),
    ('        return; // Done or being done by someone else; nothing here needs its result.\n      auto start = std::chrono::steady_clock::now();\n',
     '        return; // Done or being done by someone else; nothing here needs its result.\n      kprof::push(classTy->decl);\n      auto start = std::chrono::steady_clock::now();\n'),
    ('      impl.instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());\n      inst->state.store(2, std::memory_order_release);\n    }\n\n    // Checks a function template body',
     '      impl.instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());\n      kprof::pop();\n      inst->state.store(2, std::memory_order_release);\n    }\n\n    // Checks a function template body'),
    ('      ++impl.instFunctions;\n      auto start = std::chrono::steady_clock::now();\n',
     '      ++impl.instFunctions;\n      { Decl* c = impl.owningClassOf(f); kprof::push(c && c->isTemplate ? (const void*)c : (const void*)f); }\n      auto start = std::chrono::steady_clock::now();\n'),
    ('      inst->deducedReturn = deduced ? impl.deducedShape(pattern, deduced) : nullptr;\n',
     '      inst->deducedReturn = deduced ? impl.deducedShape(pattern, deduced) : nullptr;\n      kprof::pop();\n'),
    ('  record->deducedReturn = typeFunctionFor(f, copy, args, objectType);\n',
     '  { Decl* c = owningClass(f); kprof::push(c && c->isTemplate ? (const void*)c : (const void*)f); }\n  record->deducedReturn = typeFunctionFor(f, copy, args, objectType);\n  kprof::pop();\n'),
    ('GlobalScope::InstantiationStats GlobalScope::instantiationStats() const\n',
     DUMP + '\nGlobalScope::InstantiationStats GlobalScope::instantiationStats() const\n'),
])

patch('resolver.hpp', [
    ('  InstantiationStats instantiationStats() const;\n', '  InstantiationStats instantiationStats() const;\n  void profileDump();\n'),
])

patch('main.cpp', [
    ('    parallelFor(parsed.size(), options.threads, [&](size_t i) { global.resolveFile(*parsed[i], lexed[i].tokens, files[i].text, stats[i]); });\n',
     '    parallelFor(parsed.size(), options.threads, [&](size_t i) {\n      auto kstart = std::chrono::steady_clock::now();\n      global.resolveFile(*parsed[i], lexed[i].tokens, files[i].text, stats[i]);\n      g_kprofResolveNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - kstart).count());\n    });\n'),
    ('      runCodegen(parsed, global, symbols, options);\n    if (!options.convertDir.empty())',
     '      runCodegen(parsed, global, symbols, options);\n    global.profileDump();\n    if (!options.convertDir.empty())'),
    ('#include <vector>\n\nnamespace\n{\n', '#include <vector>\n\nextern std::atomic<uint64_t> g_kprofResolveNanos;\nnamespace\n{\n'),
])

patch('codegen.cpp', [
    ('    EmitCall call{&e, f, &args, classTy, &name};\n', '    EmitCall call{&e, f, &args, classTy, &name};\n    auto kprofStart = std::chrono::steady_clock::now();\n'),
    ('    e.emitFunction(f, args, classTy, name);\n#endif\n  };\n',
     '    e.emitFunction(f, args, classTy, name);\n#endif\n    {\n      Decl* c = global.owningClass(f);\n      const void* key = c && c->isTemplate ? (const void*)c : f->isTemplate ? (const void*)f : nullptr;\n      auto& slot = g_codegenProf[key];\n      slot.first += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - kprofStart).count());\n      ++slot.second;\n    }\n  };\n'),
    ('static CodegenResult generate(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols, bool debug,\n                              unsigned moduleCount, bool despiteErrors);\n',
     '#include <unordered_map>\n#include <chrono>\nextern std::unordered_map<const void*, std::pair<uint64_t, uint64_t>> g_codegenProf;\nstatic CodegenResult generate(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols, bool debug,\n                              unsigned moduleCount, bool despiteErrors);\n'),
])
