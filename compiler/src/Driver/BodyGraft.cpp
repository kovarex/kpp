// The driver: a compile server's incremental builds: bodies and functions grafted into the kept
// build (steps 3b and 4); its declarations in BodyGraft.hpp (Driver.hpp). A translation unit of its
// own since 2026-11-05 (main.cpp split, stage 2), moved without a change.
#include "Driver.hpp"

namespace driver
{
bool sameToken(const Token& a, std::string_view ta, const Token& b, std::string_view tb)
{
  if (a.kind != b.kind)
    return false;
  switch (a.getKind())
  {
    case TokenKind::Identifier: case TokenKind::Keyword: case TokenKind::Punctuator: case TokenKind::End:
      return a.id == b.id;
    default:
      return ta.substr(a.offset, a.length) == tb.substr(b.offset, b.length);
  }
}

// A false `when`'s dropped declarations (and the declaration-scope `if constexpr` branches the parser
// skipped) are the same in both versions: the same token ranges' tokens, in order. The names a false
// `when` keeps for a lookup to stop at (ParsedFile::absentDecls, ClassDecl::absentMembers) then stay
// the kept file's, their tokens remapped with the rest (remapKeptFile).
bool sameDropped(const ParsedFile& a, const std::vector<Token>& ta, std::string_view xa, const ParsedFile& b, const std::vector<Token>& tb,
                 std::string_view xb)
{
  if (a.droppedTokens.size() != b.droppedTokens.size() || a.droppedWhen != b.droppedWhen)
    return false;
  for (size_t i = 0; i < a.droppedTokens.size(); ++i)
  {
    auto [as, ae] = a.droppedTokens[i];
    auto [bs, be] = b.droppedTokens[i];
    if (ae - as != be - bs || ae > ta.size() || be > tb.size())
      return false;
    for (uint32_t k = 0; k < ae - as; ++k)
      if (!sameToken(ta[as + k], xa, tb[bs + k], xb))
        return false;
  }
  return true;
}

bool collectBodies(Span<Decl*> decls, const std::vector<Token>& tokens, bool inTemplate, std::vector<Body>& out)
{
  for (Decl* d : decls)
  {
    if (!d)
      continue;
    bool templated = inTemplate || d->isTemplate || !d->templateParams.empty();
    switch (d->kind)
    {
      case NodeKind::NamespaceDecl:
        if (!collectBodies(static_cast<NamespaceDecl*>(d)->decls, tokens, inTemplate, out))
          return false;
        break;
      case NodeKind::ClassDecl:
        if (!collectBodies(static_cast<ClassDecl*>(d)->members, tokens, templated, out))
          return false;
        break;
      case NodeKind::EnumDecl:
        if (!collectBodies(static_cast<EnumDecl*>(d)->members, tokens, templated, out))
          return false;
        break;
      case NodeKind::FunctionDecl:
      {
        auto* f = static_cast<FunctionDecl*>(d);
        if (!f->body)
          break;
        uint32_t open = f->body->token;
        if (open >= tokens.size() || !tokens[open].is(P::LBrace))
          return false;
        uint32_t close = open;
        for (int depth = 0; close < tokens.size(); ++close)
          if (tokens[close].is(P::LBrace))
            ++depth;
          else if (tokens[close].is(P::RBrace) && --depth == 0)
            break;
        if (close >= tokens.size())
          return false;
        bool eligible = true;
        for (uint32_t k = f->token; eligible && k < open; ++k)
          if (tokens[k].is(Kw::auto_) || tokens[k].is(Kw::decltype_))
            eligible = false; // a deduced return type: callers elsewhere are typed by the body
        out.push_back({f, open, close, eligible, templated, bool(f->flags & (DF_Constexpr | DF_Consteval))});
        break;
      }
      default:
        break;
    }
  }
  return true;
}

bool errorsInBodies(const ParsedFile& parsed, const std::vector<Body>& bodies)
{
  for (const Diagnostic& d : parsed.diagnostics)
  {
    auto after = std::upper_bound(bodies.begin(), bodies.end(), d.token, [](uint32_t t, const Body& b) { return t < b.open; });
    if (after == bodies.begin())
      return false;
    const Body& b = *std::prev(after);
    if (d.token > b.close || b.f->endToken != b.close + 1)
      return false;
  }
  return true;
}

bool bodiesOnly(Build& kept, size_t i, std::string text, ChangedFile& out, std::string& why)
{
  const ParsedFile& old = *kept.parsed[i];
  const std::vector<Token>& oldTokens = kept.lexed[i].tokens;
  std::string_view oldText = kept.files[i].text;
  if (kept.files[i].path.extension() != ".kpp" || kept.lexed[i].errorCount)
    return why = "not a .kpp file, or lexical errors", false;
  out.index = i;
  out.text = std::move(text);
  out.lexed = lex(out.text, *kept.symbols);
  out.tokens = out.lexed.tokens;
  out.parsed = std::make_unique<ParsedFile>();
  out.parsed->path = old.path;
  out.parsed->index = old.index;
  out.parsed->lambdaTag = old.lambdaTag;
  out.parsed->isLibrary = old.isLibrary;
  out.parsed->library = old.library;
  out.parsed->libraryScope = old.libraryScope;
  parseFile(out.tokens, out.text, *kept.symbols, *out.parsed, dialectOfPath(out.parsed->path), &kept.options.configValues);
  if (out.lexed.errorCount)
    return why = "the new text has lexical errors", false;
  if ((old.droppedWhen || out.parsed->droppedWhen) && !sameDropped(old, oldTokens, oldText, *out.parsed, out.tokens, out.text))
    return why = "a false when's dropped declarations changed (their names are kept in the index)", false;
  if (!collectBodies(old.decls, oldTokens, false, out.oldBodies))
    return why = "the function bodies cannot be paired", false;
  if (!collectBodies(out.parsed->decls, out.tokens, false, out.newBodies) || out.oldBodies.size() != out.newBodies.size())
    return why = "the function bodies cannot be paired", false;
  auto byOpen = [](const Body& a, const Body& b) { return a.open < b.open; };
  std::sort(out.oldBodies.begin(), out.oldBodies.end(), byOpen);
  std::sort(out.newBodies.begin(), out.newBodies.end(), byOpen);
  if (!errorsInBodies(old, out.oldBodies))
    return why = "parse errors outside function bodies", false;
  if (!errorsInBodies(*out.parsed, out.newBodies))
    return why = "the new text has parse errors outside function bodies", false;
  // Outside the bodies the token streams must be the same; a body that differs is a change.
  size_t a = 0, b = 0;
  for (size_t k = 0; k <= out.oldBodies.size(); ++k)
  {
    size_t aEnd = k < out.oldBodies.size() ? out.oldBodies[k].open : oldTokens.size();
    size_t bEnd = k < out.newBodies.size() ? out.newBodies[k].open : out.tokens.size();
    if (aEnd < a || bEnd < b || aEnd - a != bEnd - b)
      return why = "a change outside function bodies", false;
    for (; a < aEnd; ++a, ++b)
      if (!sameToken(oldTokens[a], oldText, out.tokens[b], out.text))
        return why = "a change outside function bodies", false;
    if (k == out.oldBodies.size())
      break;
    const Body& ob = out.oldBodies[k];
    const Body& nb = out.newBodies[k];
    bool same = ob.close - ob.open == nb.close - nb.open;
    for (uint32_t x = 0; same && x <= ob.close - ob.open; ++x)
      same = sameToken(oldTokens[ob.open + x], oldText, out.tokens[nb.open + x], out.text);
    if (!same)
    {
      if (!ob.eligible || !nb.eligible)
        return why = "a changed body with a deduced return type", false;
      // A class or enumeration declared in a body gets its scope when the file is indexed: one in
      // a new body would have none (a lambda's closure is no such declaration).
      bool localType = false;
      auto findLocalTypes = [&](Node* n)
      {
        if ((n->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(n)->lambda) || n->kind == NodeKind::EnumDecl)
          localType = true;
      };
      forEachNode(nb.f->body, findLocalTypes);
      forEachNode(ob.f->body, findLocalTypes); // (and the old body's would stay in the index)
      if (localType)
        return why = "a changed body declares a class or an enumeration", false;
      out.changed.push_back(k);
    }
    a = ob.close + 1;
    b = nb.close + 1;
  }
  return true;
}

bool tokenMap(const std::vector<Token>& a, std::string_view ta, const std::vector<Token>& b, std::string_view tb, std::vector<uint32_t>& map)
{
  size_t n = a.size(), m = b.size();
  map.assign(n, UINT32_MAX);
  size_t prefix = 0;
  while (prefix < n && prefix < m && sameToken(a[prefix], ta, b[prefix], tb))
    map[prefix] = uint32_t(prefix), ++prefix;
  size_t suffix = 0;
  while (suffix < n - prefix && suffix < m - prefix && sameToken(a[n - 1 - suffix], ta, b[m - 1 - suffix], tb))
    map[n - 1 - suffix] = uint32_t(m - 1 - suffix), ++suffix;
  int64_t N = int64_t(n - prefix - suffix), M = int64_t(m - prefix - suffix);
  if (N == 0 || M == 0)
    return true;
  auto same = [&](int64_t x, int64_t y) { return sameToken(a[prefix + x], ta, b[prefix + y], tb); };
  const int64_t maxD = std::min<int64_t>(N + M, 4000);
  int64_t offset = maxD + 1;
  std::vector<int64_t> v(size_t(2 * offset + 1), 0);
  std::vector<std::vector<int64_t>> trace;
  int64_t found = -1;
  for (int64_t d = 0; d <= maxD && found < 0; ++d)
  {
    if (int64_t(trace.size()) * (2 * offset + 1) > 64'000'000)
      return false;
    trace.push_back(v);
    for (int64_t k = -d; k <= d; k += 2)
    {
      int64_t x = (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1])) ? v[offset + k + 1] : v[offset + k - 1] + 1;
      int64_t y = x - k;
      while (x < N && y < M && same(x, y))
        ++x, ++y;
      v[offset + k] = x;
      if (x >= N && y >= M)
      {
        found = d;
        break;
      }
    }
  }
  if (found < 0)
    return false;
  // Back through the trace: the diagonal moves are the tokens the two versions share.
  int64_t x = N, y = M;
  for (int64_t d = found; d > 0; --d)
  {
    const std::vector<int64_t>& pv = trace[size_t(d)];
    int64_t k = x - y;
    int64_t prevK = (k == -d || (k != d && pv[offset + k - 1] < pv[offset + k + 1])) ? k + 1 : k - 1;
    int64_t prevX = pv[offset + prevK], prevY = prevX - prevK;
    while (x > prevX && y > prevY)
    {
      --x, --y;
      map[prefix + size_t(x)] = uint32_t(prefix + size_t(y));
    }
    x = prevX, y = prevY;
  }
  while (x > 0 && y > 0)
  {
    --x, --y;
    map[prefix + size_t(x)] = uint32_t(prefix + size_t(y));
  }
  return true;
}

void collectFunctions(Span<Decl*> decls, Decl* container, bool inTemplate, std::vector<DeclaredFunction>& out,
                      std::vector<Decl*>& containers, std::vector<DeclaredMember>* members)
{
  for (Decl* d : decls)
  {
    if (!d)
      continue;
    bool templated = inTemplate || d->isTemplate || !d->templateParams.empty();
    switch (d->kind)
    {
      case NodeKind::NamespaceDecl:
        containers.push_back(d);
        collectFunctions(static_cast<NamespaceDecl*>(d)->decls, d, inTemplate, out, containers, members);
        break;
      case NodeKind::ClassDecl:
        containers.push_back(d);
        collectFunctions(static_cast<ClassDecl*>(d)->members, d, templated, out, containers, members);
        break;
      case NodeKind::EnumDecl:
        containers.push_back(d);
        collectFunctions(static_cast<EnumDecl*>(d)->members, d, templated, out, containers, members);
        break;
      case NodeKind::FunctionDecl:
        out.push_back({static_cast<FunctionDecl*>(d), container, templated});
        break;
      case NodeKind::VariableDecl:
        if (members && container && container->kind == NodeKind::ClassDecl)
          members->push_back({static_cast<VariableDecl*>(d), static_cast<ClassDecl*>(container), inTemplate});
        break;
      default:
        break;
    }
  }
}

bool remapKeptFile(Build& kept, ParsedFile& file, const std::vector<uint32_t>& map, const std::unordered_set<Node*>& skip,
                   const std::unordered_set<const Decl*>& staleSources, const ParsedFile& fresh)
{
  // Names the code gives by position: a function's local types (`f::Local@2045`; its lambdas'
  // closures by their file's tag and the parser's numbers in the file's order,
  // `f::__lambda_6c1f0e2a_1`), its static locals, catch filters and static initializer lists (by
  // their tokens). A build from scratch of the new text names them by their new places, so a kept
  // function holding one that moves is emitted again (in full, for a local type: codegen's session
  // keeps the types' names; and for a template's, whose instances it cannot emit again alone), and a
  // kept closure takes the name the new parse gives the lambda at its new place.
  std::unordered_map<uint32_t, ClassDecl*> freshClosures; // new token -> the new parse's closure there
  forEachNode(fresh.decls, [&](Node* n)
  {
    if (n->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(n)->lambda)
      freshClosures[n->token] = static_cast<ClassDecl*>(n);
  });
  auto moved = [&](uint32_t t) { return t >= map.size() || map[t] != t; };
  // A kept closure takes the name the new parse gives the lambda at its new place: true when that
  // name is another.
  auto renameClosure = [&](ClassDecl* c)
  {
    if (!c->lambda || c->name.empty() || c->token >= map.size() || map[c->token] == UINT32_MAX)
      return false;
    auto it = freshClosures.find(map[c->token]);
    if (it == freshClosures.end() || it->second->name.empty() || it->second->name.last().symbol == c->name.last().symbol)
      return false;
    const_cast<NamePart&>(c->name.parts[c->name.parts.size() - 1]).symbol = it->second->name.last().symbol;
    return true;
  };
  std::vector<DeclaredFunction> functions;
  std::vector<Decl*> containers;
  collectFunctions(file.decls, nullptr, false, functions, containers);
  for (const DeclaredFunction& d : functions)
  {
    if (!d.f->body || skip.count(d.f->body))
      continue;
    bool localType = false, positional = false;
    forEachNode(d.f->body, [&](Node* n)
    {
      if (skip.count(n))
        return;
      if (n->kind == NodeKind::ClassDecl || n->kind == NodeKind::EnumDecl)
      {
        // (A lambda's closure is named by its file's number, not its place: renamed only.)
        bool renamed = n->kind == NodeKind::ClassDecl && renameClosure(static_cast<ClassDecl*>(n));
        bool closure = n->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(n)->lambda;
        localType = localType || renamed || (!closure && moved(n->token));
      }
      else if ((n->kind == NodeKind::VariableDecl && (static_cast<Decl*>(n)->flags & DF_Static)) || n->kind == NodeKind::TryStmt ||
               n->kind == NodeKind::BraceInitExpr)
        positional = positional || moved(n->token);
    });
    if (localType || (positional && d.templated))
      kept.positionalCodegenInFull = true;
    else if (positional)
      kept.changedBodies.push_back(d.f);
  }
  // The closures outside function bodies, named alike (a global's initializer `{ [](int v) {...}, ...
  // }`, a default member initializer, a default argument): the instances made from them carry their
  // names (`FunctionImpl<int32, int32>::call<__lambda_6c1f0e2a_2>`), so a renamed one is emitted in
  // full too. (Only renamed: outside a function their names carry no token. The ones in bodies again:
  // renamed already.)
  forEachNode(file.decls, [&](Node* n)
  {
    if (!skip.count(n) && n->kind == NodeKind::ClassDecl && renameClosure(static_cast<ClassDecl*>(n)))
      kept.positionalCodegenInFull = true;
  });
  bool lost = false;
  std::unordered_set<Node*> done;
  auto remap = [&](Node* n)
  {
    if (skip.count(n) || !done.insert(n).second)
      return;
    remapNodeTokens(n, [&](uint32_t t)
    {
      if (t < map.size() && map[t] != UINT32_MAX)
        return map[t];
      lost = true;
      return t;
    });
  };
  forEachNode(file.decls, remap);
  // A false `when`'s absent declarations, which the walk over the tree does not reach: their tokens
  // move too (the names stay in the index; sameDropped made sure they are the same).
  for (Decl* d : file.absentDecls)
    forEachNode(d, remap);
  forEachNode(file.decls, [&](Node* n)
  {
    if (n->kind == NodeKind::ClassDecl)
      for (Decl* m : static_cast<ClassDecl*>(n)->absentMembers)
        forEachNode(m, remap);
  });
  kept.global->forEachInstanceCopy(file, [&](Node* copy, const Decl* source)
  {
    if (!staleSources.count(source))
      forEachNode(copy, remap);
  });
  return !lost;
}

bool graftBodies(Build& kept, ChangedFile& c, std::string& why)
{
  size_t i = c.index;
  ParsedFile& file = *kept.parsed[i];
  std::vector<uint32_t> map;
  if (!tokenMap(kept.lexed[i].tokens, kept.files[i].text, c.tokens, c.text, map))
    return why = "the two versions differ too much to pair their tokens", false;
  std::unordered_set<Node*> replaced; // the old bodies' nodes: dropped, not pointed anywhere
  std::unordered_set<const Decl*> stale; // templates whose instances are copied anew
  for (size_t k : c.changed)
  {
    forEachNode(c.oldBodies[k].f->body, [&](Node* n) { replaced.insert(n); });
    stale.insert(c.oldBodies[k].f);
  }
  if (!remapKeptFile(kept, file, map, replaced, stale, *c.parsed))
    return why = "a kept node's token has no place in the new version", false;
  for (auto& [b, where] : c.parsed->castCandidates)
    if (where && where->kind == NodeKind::FunctionDecl)
      for (size_t k : c.changed)
        if (where == c.newBodies[k].f)
          return why = "a new body has a cast to rewrite", false;
  // The new bodies, their declarations (locals, lambdas' closures) now the kept file's and their
  // parent the kept function where it was the new parse's.
  for (size_t k : c.changed)
  {
    FunctionDecl* keptF = c.oldBodies[k].f;
    FunctionDecl* newF = c.newBodies[k].f;
    keptF->body = newF->body;
    forEachNode(keptF->body, [&](Node* n)
    {
      if (n->kind > NodeKind::PackType && n->kind <= NodeKind::ImportDecl)
      {
        auto* d = static_cast<Decl*>(n);
        d->file = &file;
        if (d->parent == newF)
          d->parent = keptF;
      }
      else if (n->kind == NodeKind::LiteralExpr)
        static_cast<LiteralExpr*>(n)->file = &file; // its spelling is read from its file's tokens
    });
  }
  kept.files[i].text = std::move(c.text);
  kept.lexed[i].tokens = std::move(c.tokens);
  kept.lexed[i].lineCount = c.lexed.lineCount;
  kept.lexed[i].errorCount = c.lexed.errorCount;
  file.text = kept.files[i].text;
  file.tokens = &kept.lexed[i].tokens;
  file.diagnostics = c.parsed->diagnostics; // (inside bodies: errorsInBodies)
  file.declCount = c.parsed->declCount;
  file.functionBodies = c.parsed->functionBodies;
  file.droppedTokens = c.parsed->droppedTokens; // (the language server's inactive code, by the new tokens)
  file.nodeCount = c.parsed->nodeCount;
  file.arenaBytesReported = c.parsed->arena.bytesAllocated();
  kept.grafts.push_back(std::move(c.parsed)); // its arena holds the new bodies
  // The kept instance statistics' places in this file (an instance of a template it declares)
  // move with its tokens (and each instance walk's own, from which the total may be made again);
  // one in a replaced body cannot be told, and the build is from scratch.
  bool lost = false;
  auto moved = [&](uint32_t& token)
  {
    if (token < map.size() && map[token] != UINT32_MAX)
      token = map[token];
    else
      lost = true;
  };
  for (auto& [symbol, where] : kept.instanceStats.unresolvedFirst)
    if (where.file == i)
      moved(where.token);
  for (auto& [symbol, where] : kept.instanceStats.missingMembersFirst)
    if (where.file == i)
      moved(where.token);
  for (auto& [key, site] : kept.instanceStats.why)
    if (site.file == &file)
      moved(site.token);
  kept.global->forEachInstanceStatsPlace(uint32_t(i), &file, moved);
  if (lost)
    return why = "an instance's statistic names a place in a replaced body", false;
  return true;
}

uint32_t memberEnd(const VariableDecl* v, const std::vector<Token>& tokens)
{
  uint32_t e = v->endToken;
  if (e < tokens.size() && tokens[e].is(P::Semi))
    ++e;
  return e;
}

bool plainDataMember(const DeclaredMember& d)
{
  VariableDecl* v = d.v;
  ClassDecl* c = d.container;
  return !(d.templated || c->isTemplate || !c->templateParams.empty() || c->keyword == Kw::union_ || c->isTypedUnion || c->lambda ||
           !c->isDefinition || c->name.empty() || (v->flags & (DF_Static | DF_Typedef | DF_Friend)) || v->name.parts.size() != 1 ||
           !v->structuredBindings.empty());
}

bool plainFunction(const DeclaredFunction& d, const std::vector<Token>& tokens)
{
  FunctionDecl* f = d.f;
  const uint32_t excluded = DF_Virtual | DF_Override | DF_Pure | DF_Constructor | DF_Destructor | DF_Conversion | DF_Friend | DF_Constexpr |
                            DF_Consteval | DF_Specialization | DF_ExternC;
  if (d.templated || f->isTemplate || !f->templateParams.empty() || (f->flags & excluded) || f->name.empty() || f->name.last().special ||
      !f->objectArgs.empty())
    return false;
  uint32_t end = f->body ? f->body->token : f->endToken;
  for (uint32_t k = f->token; k < end && k < tokens.size(); ++k)
    if (tokens[k].is(Kw::auto_) || tokens[k].is(Kw::decltype_))
      return false;
  bool localType = false;
  forEachNode(f->body, [&](Node* n)
  {
    if ((n->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(n)->lambda) || n->kind == NodeKind::EnumDecl)
      localType = true;
  });
  return !localType;
}

bool functionsOnly(Build& kept, size_t i, std::string text, FunctionChange& out, std::string& why)
{
  const ParsedFile& old = *kept.parsed[i];
  const std::vector<Token>& oldTokens = kept.lexed[i].tokens;
  std::string_view oldText = kept.files[i].text;
  if (!old.diagnostics.empty() || kept.files[i].path.extension() != ".kpp")
    return why = "parse errors, or not a .kpp file", false;
  out.index = i;
  out.text = std::move(text);
  out.lexed = lex(out.text, *kept.symbols);
  out.tokens = out.lexed.tokens;
  out.parsed = std::make_unique<ParsedFile>();
  out.parsed->path = old.path;
  out.parsed->index = old.index;
  out.parsed->lambdaTag = old.lambdaTag;
  out.parsed->isLibrary = old.isLibrary;
  out.parsed->library = old.library;
  out.parsed->libraryScope = old.libraryScope;
  parseFile(out.tokens, out.text, *kept.symbols, *out.parsed, dialectOfPath(out.parsed->path), &kept.options.configValues);
  if (!out.parsed->diagnostics.empty() || out.lexed.errorCount)
    return why = "the new text has errors", false;
  if ((old.droppedWhen || out.parsed->droppedWhen) && !sameDropped(old, oldTokens, oldText, *out.parsed, out.tokens, out.text))
    return why = "a false when's dropped declarations changed (their names are kept in the index)", false;
  if (!tokenMap(oldTokens, oldText, out.tokens, out.text, out.map))
    return why = "the two versions differ too much to pair their tokens", false;
  std::vector<DeclaredFunction> oldFunctions, newFunctions;
  std::vector<Decl*> oldContainers, newContainers;
  std::vector<DeclaredMember> oldMembers, newMembers;
  collectFunctions(old.decls, nullptr, false, oldFunctions, oldContainers, &oldMembers);
  collectFunctions(out.parsed->decls, nullptr, false, newFunctions, newContainers, &newMembers);
  // A kept function whose tokens all have places, side by side, at a new function's: the same.
  std::unordered_map<uint32_t, const DeclaredFunction*> newByToken;
  for (const DeclaredFunction& d : newFunctions)
    newByToken[d.f->token] = &d;
  std::unordered_set<const FunctionDecl*> pairedNew;
  std::vector<bool> oldCovered(oldTokens.size(), false), newCovered(out.tokens.size(), false);
  std::vector<const DeclaredFunction*> removed;
  for (const DeclaredFunction& d : oldFunctions)
  {
    uint32_t s = d.f->token, e = d.f->endToken;
    bool same = e > s && e <= out.map.size() && out.map[s] != UINT32_MAX;
    for (uint32_t k = s; same && k < e; ++k)
      same = out.map[k] == out.map[s] + (k - s);
    auto it = same ? newByToken.find(out.map[s]) : newByToken.end();
    if (it != newByToken.end() && it->second->f->endToken == out.map[s] + (e - s))
    {
      pairedNew.insert(it->second->f);
      continue;
    }
    removed.push_back(&d);
    for (uint32_t k = s; k < e && k < oldCovered.size(); ++k)
      oldCovered[k] = true;
  }
  std::vector<const DeclaredFunction*> added;
  for (const DeclaredFunction& d : newFunctions)
    if (!pairedNew.count(d.f))
    {
      added.push_back(&d);
      for (uint32_t k = d.f->token; k < d.f->endToken && k < newCovered.size(); ++k)
        newCovered[k] = true;
    }
  // Data members likewise (a class's layout edit), a declaration at a time: its declarators all
  // start at its first token and share its type's nodes (`MapPosition start, goal;`), so they are
  // kept, removed or added together. A kept declaration whose tokens all have places side by side
  // at a new one's is the same, the others removed, the new ones not paired added.
  struct MemberDeclaration
  {
    std::vector<const DeclaredMember*> declarators;
    uint32_t end = 0; // one past its `;`
  };
  auto declarationsOf = [](const std::vector<DeclaredMember>& members, const std::vector<Token>& tokens)
  {
    std::map<uint32_t, MemberDeclaration> byToken;
    for (const DeclaredMember& d : members)
    {
      MemberDeclaration& m = byToken[d.v->token];
      m.declarators.push_back(&d);
      m.end = std::max(m.end, memberEnd(d.v, tokens));
    }
    return byToken;
  };
  std::map<uint32_t, MemberDeclaration> oldDeclarations = declarationsOf(oldMembers, oldTokens), newDeclarations = declarationsOf(newMembers, out.tokens);
  std::unordered_set<uint32_t> pairedNewDeclarations;
  std::vector<const DeclaredMember*> removedMembers, addedMembers;
  for (auto& [s, m] : oldDeclarations)
  {
    uint32_t e = m.end;
    bool same = e > s && e <= out.map.size() && out.map[s] != UINT32_MAX;
    for (uint32_t k = s; same && k < e; ++k)
      same = out.map[k] == out.map[s] + (k - s);
    auto it = same ? newDeclarations.find(out.map[s]) : newDeclarations.end();
    if (it != newDeclarations.end() && it->second.end == out.map[s] + (e - s) && it->second.declarators.size() == m.declarators.size())
    {
      pairedNewDeclarations.insert(it->first);
      continue;
    }
    removedMembers.insert(removedMembers.end(), m.declarators.begin(), m.declarators.end());
    for (uint32_t k = s; k < e && k < oldCovered.size(); ++k)
      oldCovered[k] = true;
  }
  for (auto& [s, m] : newDeclarations)
    if (!pairedNewDeclarations.count(s))
    {
      addedMembers.insert(addedMembers.end(), m.declarators.begin(), m.declarators.end());
      for (uint32_t k = s; k < m.end && k < newCovered.size(); ++k)
        newCovered[k] = true;
    }
  if (removed.empty() && added.empty() && removedMembers.empty() && addedMembers.empty())
    return why = "no function or data member changed", false;
  // Everything else the same: every token gone is a removed function's, every new one an added one's.
  std::vector<bool> reached(out.tokens.size(), false);
  for (size_t k = 0; k < out.map.size(); ++k)
    if (out.map[k] != UINT32_MAX)
      reached[out.map[k]] = true;
    else if (!oldCovered[k])
      return why = "a change outside whole functions and data members", false;
  for (size_t k = 0; k < reached.size(); ++k)
    if (!reached[k] && !newCovered[k])
      return why = "a change outside whole functions and data members", false;
  for (const DeclaredMember* d : removedMembers)
    if (!plainDataMember(*d))
      return why = "a removed or changed data member of a template, a union, or static, an alias", false;
  for (const DeclaredMember* d : addedMembers)
    if (!plainDataMember(*d))
      return why = "an added or changed data member of a template, a union, or static, an alias", false;
  for (const DeclaredFunction* d : removed)
    if (!plainFunction(*d, oldTokens))
      return why = "a removed or changed function others may read without its name (a template, virtual, special, constexpr)", false;
  for (const DeclaredFunction* d : added)
    if (!plainFunction(*d, out.tokens))
      return why = "an added or changed function others may read without its name (a template, virtual, special, constexpr)", false;
  // A declaration taken out while its out-of-line definition stays: a build from scratch reports it.
  for (const DeclaredFunction* d : removed)
    if (!d->f->body && d->container && d->container->kind == NodeKind::ClassDecl && d->container->ownScope)
      for (FunctionDecl* def : kept.global->outOfLineDefinitions(d->container))
        if (kept.global->functionSymbolOf(def) == kept.global->functionSymbolOf(d->f) &&
            std::none_of(removed.begin(), removed.end(), [&](const DeclaredFunction* r) { return r->f == def; }))
          return why = "a declaration removed whose definition stays", false;
  // Each added function's kept declaration to go into: the new one's, by its first token's place.
  std::unordered_map<uint32_t, Decl*> keptContainerAt; // new token -> kept container there
  for (Decl* c : oldContainers)
    if (c->token < out.map.size() && out.map[c->token] != UINT32_MAX)
      keptContainerAt[out.map[c->token]] = c;
  for (const DeclaredFunction* d : added)
  {
    Decl* keptContainer = nullptr;
    if (d->container)
    {
      auto it = keptContainerAt.find(d->container->token);
      if (it == keptContainerAt.end() || it->second->kind != d->container->kind)
        return why = "an added function's class or namespace is new", false;
      keptContainer = it->second;
    }
    out.added.push_back({d->f, keptContainer});
  }
  for (auto& [b, where] : out.parsed->castCandidates)
    for (const DeclaredFunction* d : added)
      if (where == d->f)
        return why = "an added function has a cast to rewrite", false;
  // A private one's name is in its file's private scope, which only a whole index makes (a name
  // made private or global is one of these: removed as the one, added as the other).
  for (const DeclaredFunction* d : added)
    if (d->f->filePrivate)
      return why = "an added function is private to its file", false;
  for (const DeclaredFunction* d : removed)
    if (d->f->filePrivate)
      return why = "a removed function is private to its file", false;
  // A scoped library's top-level function is in the library's namespace, whose declaration only a
  // whole index makes (design.md 16).
  if (out.parsed->libraryScope != UINT32_MAX)
    for (const auto* list : {&added, &removed})
      for (const DeclaredFunction* d : *list)
        if (!d->container)
          return why = "an added or removed function is a top-level one of a scoped library", false;
  for (const DeclaredFunction* d : removed)
    out.removed.push_back({d->f, d->container});
  for (const DeclaredMember* d : addedMembers)
  {
    auto it = keptContainerAt.find(d->container->token);
    if (it == keptContainerAt.end() || it->second->kind != NodeKind::ClassDecl)
      return why = "an added data member's class is new", false;
    out.addedMembers.push_back({d->v, static_cast<ClassDecl*>(it->second)});
  }
  for (const DeclaredMember* d : removedMembers)
    out.removedMembers.push_back({d->v, d->container});
  for (auto& [b, where] : out.parsed->castCandidates)
    for (const DeclaredMember* d : addedMembers)
      if (where == d->v)
        return why = "an added data member has a cast to rewrite", false;
  return true;
}

Span<Decl*> withFunctions(Span<Decl*> list, const std::unordered_set<const Decl*>& removed, const std::vector<Decl*>& added,
                          Arena& arena)
{
  std::vector<Decl*> out;
  for (Decl* d : list)
    if (!removed.count(d))
      out.push_back(d);
  out.insert(out.end(), added.begin(), added.end());
  std::stable_sort(out.begin(), out.end(), [](const Decl* a, const Decl* b) { return a->token < b->token; });
  Span<Decl*> span;
  span.count = uint32_t(out.size());
  span.data = static_cast<Decl**>(arena.allocate(sizeof(Decl*) * std::max<size_t>(1, out.size()), alignof(Decl*)));
  std::copy(out.begin(), out.end(), span.data);
  return span;
}

bool graftFunctions(Build& kept, FunctionChange& c, std::string& why)
{
  size_t i = c.index;
  ParsedFile& file = *kept.parsed[i];
  GlobalScope& global = *kept.global;
  std::unordered_set<const Decl*> removed;
  std::unordered_set<Node*> dropped;
  for (auto& [f, container] : c.removed)
  {
    removed.insert(f);
    global.unindexFunction(f);
    forEachNode(f, [&](Node* n) { dropped.insert(n); });
  }
  for (auto& [v, container] : c.removedMembers)
  {
    removed.insert(v);
    global.unindexMember(v);
    forEachNode(v, [&](Node* n) { dropped.insert(n); });
  }
  if (!remapKeptFile(kept, file, c.map, dropped, {}, *c.parsed))
    return why = "a kept node's token has no place in the new version", false;
  // The added functions: the kept file's, members of the kept declarations.
  std::unordered_map<Decl*, std::vector<Decl*>> into;
  std::vector<FunctionDecl*> added;
  std::vector<VariableDecl*> addedMembers;
  auto ownNodes = [&](Node* root)
  {
    forEachNode(root, [&](Node* n)
    {
      if (n->kind > NodeKind::PackType && n->kind <= NodeKind::ImportDecl)
        static_cast<Decl*>(n)->file = &file;
      else if (n->kind == NodeKind::LiteralExpr)
        static_cast<LiteralExpr*>(n)->file = &file;
    });
  };
  for (auto& [f, container] : c.added)
  {
    ownNodes(f);
    f->parent = container;
    into[container].push_back(f);
    added.push_back(f);
  }
  for (auto& [v, container] : c.addedMembers)
  {
    ownNodes(v);
    v->parent = container;
    into[container].push_back(v);
    addedMembers.push_back(v);
  }
  std::unordered_set<Decl*> touched;
  for (auto& [f, container] : c.removed)
    touched.insert(container);
  for (auto& [v, container] : c.removedMembers)
    touched.insert(container);
  for (auto& [container, list] : into)
    touched.insert(container);
  for (Decl* container : touched)
  {
    const std::vector<Decl*>& in = into[container];
    if (!container)
      file.decls = withFunctions(file.decls, removed, in, file.arena);
    else if (container->kind == NodeKind::NamespaceDecl)
      static_cast<NamespaceDecl*>(container)->decls = withFunctions(static_cast<NamespaceDecl*>(container)->decls, removed, in, file.arena);
    else if (container->kind == NodeKind::ClassDecl)
      static_cast<ClassDecl*>(container)->members = withFunctions(static_cast<ClassDecl*>(container)->members, removed, in, file.arena);
    else if (container->kind == NodeKind::EnumDecl)
      static_cast<EnumDecl*>(container)->members = withFunctions(static_cast<EnumDecl*>(container)->members, removed, in, file.arena);
  }
  kept.files[i].text = std::move(c.text);
  kept.lexed[i].tokens = std::move(c.tokens);
  kept.lexed[i].lineCount = c.lexed.lineCount;
  kept.lexed[i].errorCount = c.lexed.errorCount;
  file.text = kept.files[i].text;
  file.tokens = &kept.lexed[i].tokens;
  file.declCount = c.parsed->declCount;
  file.functionBodies = c.parsed->functionBodies;
  file.droppedTokens = c.parsed->droppedTokens; // (the language server's inactive code, by the new tokens)
  file.nodeCount = c.parsed->nodeCount;
  file.arenaBytesReported = c.parsed->arena.bytesAllocated();
  kept.grafts.push_back(std::move(c.parsed));
  for (auto& [symbol, where] : kept.instanceStats.unresolvedFirst)
    if (where.file == i)
      return why = "an instance's statistic names a place in the file", false;
  for (auto& [symbol, where] : kept.instanceStats.missingMembersFirst)
    if (where.file == i)
      return why = "an instance's statistic names a place in the file", false;
  global.indexMembers(addedMembers);
  if (!global.indexFunctions(added))
    return why = "an added function needs the whole index (a definition matching no declaration, an object declaration)", false;
  return true;
}

bool incrementalBuild(Build& kept, std::vector<SourceFile>& files, double collectTime, bool headersSame, bool& keptChanged)
{
  using Clock = std::chrono::steady_clock;
  keptChanged = false;
  std::string why;
  if (!kept.keepable || files.size() + 1 < kept.files.size() || !headersSame)
    return false;
  size_t sources = 0; // the kept files without the C import, which the request's files have not yet
  for (const auto& p : kept.parsed)
    sources += !p->isImported;
  if (files.size() != sources)
    return false;
  for (size_t i = 0; i < files.size(); ++i)
    if (files[i].path != kept.files[i].path)
      return false;
  auto start = Clock::now();
  std::vector<ChangedFile> changed;         // bodies only (3b)
  std::vector<FunctionChange> functionEdits; // whole functions (step 4)
  for (size_t i = 0; i < files.size(); ++i)
    if (files[i].text != kept.files[i].text)
    {
      // A config:: value changed (a default declared in source: design.md, "config:: defaults in
      // source"): every file's parse-time choices read it.
      if (files[i].path == "<config>")
      {
        logBuildNote("not incremental: <config>: a config:: value changed");
        return false;
      }
      changed.emplace_back();
      if (bodiesOnly(kept, i, files[i].text, changed.back(), why))
        continue;
      changed.pop_back();
      std::string bodiesWhy = why;
      functionEdits.emplace_back();
      if (!functionsOnly(kept, i, files[i].text, functionEdits.back(), why))
      {
        logBuildNote("not incremental: " + files[i].path.string() + ": " + bodiesWhy + "; " + why);
        return false;
      }
    }
  double parseTime = seconds(Clock::now() - start);
  keptChanged = true;
  // The last build generated no code (parse errors, a clash): what it changed, codegen's kept
  // session has not seen, and this build emits it too. What codegen cannot take as changed bodies
  // and globals (functions added or removed, layouts, instances), or could not follow then, makes
  // it run in full.
  std::vector<FunctionDecl*> carriedBodies;
  std::vector<const Decl*> carriedGlobals;
  bool carriedInFull = false;
  if (kept.codegenBehind)
  {
    carriedBodies = kept.changedBodies;
    carriedGlobals = kept.changedGlobals;
    carriedInFull = !kept.rebuildCodegen || !kept.addedFunctions.empty() || !kept.removedFunctions.empty() || !kept.changedLayouts.empty() ||
                    !kept.instancesOf.empty() || kept.positionalCodegenInFull;
  }
  kept.changedBodies.clear();
  kept.addedFunctions.clear();
  kept.removedFunctions.clear();
  kept.changedLayouts.clear();
  kept.instancesOf.clear();
  kept.changedGlobals.clear();
  kept.droppedClasses.clear();
  kept.positionalCodegenInFull = false;
  bool codegenCanFollow = true; // what changed is functions' code only (rebuildLLVM's input)
  std::string codegenInFullBecause; // the first reason it is not (the server's log names it)
  size_t codegenInFullOthers = 0;   // and how many more there are
  auto inFull = [&](std::string why)
  {
    if (codegenCanFollow)
      codegenInFullBecause = std::move(why);
    else
      ++codegenInFullOthers;
    codegenCanFollow = false;
  };
  auto initializerOf = [&](const Decl* d)
  {
    std::string what = d->kind == NodeKind::StaticAssertDecl ? std::string("a static_assert")
                       : d->kind == NodeKind::UsingNameDecl ? "a using-declaration in " + kept.global->qualifiedName(d->parent)
                       : d->kind == NodeKind::VariableDecl  ? "the initializer of " + kept.global->qualifiedName(const_cast<Decl*>(d))
                                                            : "the walk of " + kept.global->qualifiedName(const_cast<Decl*>(d));
    return what + " (" + (d->file ? kept.files[d->file->index].path.filename().string() : std::string("?")) + ")";
  };
  std::vector<FunctionDecl*> templateBodies; // changed bodies of templates: their instances are checked again
  std::vector<const FunctionDecl*> constantBodies; // changed constexpr bodies: what the evaluator ran them for is checked again
  std::vector<FunctionDecl*> plainBodies; // changed bodies of the others: for withBodyCopies
  for (size_t n = 0; n < changed.size(); ++n)
  {
    if (!graftBodies(kept, changed[n], why))
    {
      logBuildNote("not incremental: " + kept.files[changed[n].index].path.string() + ": " + why);
      return false;
    }
    for (size_t k : changed[n].changed)
    {
      kept.changedBodies.push_back(changed[n].oldBodies[k].f);
      if (changed[n].oldBodies[k].templated)
        templateBodies.push_back(changed[n].oldBodies[k].f);
      else
        plainBodies.push_back(changed[n].oldBodies[k].f);
      if (changed[n].oldBodies[k].constant)
        constantBodies.push_back(changed[n].oldBodies[k].f);
    }
  }
  // A changed function that is no template but has a copy of its body in an instance record: the
  // evaluator ran it from that copy (a constexpr one or not: a static_assert, --run-build) or its
  // `auto` return was typed before its file's walk. The copy is made anew, and what the evaluator
  // ran it for is found, as for a constexpr one (it was kept, the old body's answer: silently).
  for (FunctionDecl* f : kept.global->withBodyCopies(plainBodies))
  {
    templateBodies.push_back(f);
    if (std::find(constantBodies.begin(), constantBodies.end(), f) == constantBodies.end())
      constantBodies.push_back(f);
  }
  GlobalScope& global = *kept.global;
  GlobalScope::recordDependencies(true);
  start = Clock::now();
  // The files to walk again: the changed ones, and (whole functions changed) every file a walk
  // of which read one of the functions' names or the declared type of a removed one.
  std::set<size_t> rewalk;
  for (const ChangedFile& c : changed)
    rewalk.insert(c.index);
  std::vector<GlobalScope::ChangedName> names;
  std::vector<const Decl*> gone;
  std::vector<const void*> readingInstances; // instances that read a changed name, type or body: checked again
  std::vector<const Decl*> typedByWalk;      // reading walks' declarations whose type the walk gives: compared after it
  size_t entriesBefore = global.entryCount();
  for (FunctionChange& c : functionEdits)
  {
    rewalk.insert(c.index);
    for (auto& [f, container] : c.removed)
    {
      names.push_back(global.changedNameOf(f));
      gone.push_back(f);
      kept.removedFunctions.push_back(f);
    }
    for (auto& [f, container] : c.added)
      kept.addedFunctions.push_back(f);
    // A data member removed or changed: its name in its class, its declared type, its class's
    // member list (read by position) are what readers read.
    for (auto& [v, cls] : c.removedMembers)
    {
      names.push_back(global.changedNameOf(v));
      gone.push_back(v);
      kept.changedLayouts.push_back(cls);
    }
    if (!graftFunctions(kept, c, why))
    {
      logBuildNote("not incremental: " + kept.files[c.index].path.string() + ": " + why);
      return false;
    }
    // (once grafted: an added member's class is then the kept one, its lookups' scope)
    for (auto& [f, container] : c.added)
      names.push_back(global.changedNameOf(f));
    for (auto& [v, cls] : c.addedMembers)
    {
      names.push_back(global.changedNameOf(v));
      kept.changedLayouts.push_back(cls);
    }
  }
  std::sort(kept.changedLayouts.begin(), kept.changedLayouts.end());
  kept.changedLayouts.erase(std::unique(kept.changedLayouts.begin(), kept.changedLayouts.end()), kept.changedLayouts.end());
  // Codegen takes changed layouts (rebuildLLVM lays them out again, or declines); it runs in full
  // for local types that moved (remapKeptFile).
  if (kept.positionalCodegenInFull)
    inFull("a local type moved (its name is its place)");
  if (carriedInFull)
    inFull("the last build generated no code and changed what codegen does not follow");
  if (!names.empty() || !constantBodies.empty())
  {
    std::vector<const ParsedFile*> readers;
    std::vector<const Decl*> readingWalks;
    if (!global.invalidateReaders(names, gone, readers, &readingWalks, constantBodies, &why, &readingInstances, &typedByWalk, kept.changedLayouts))
    {
      logBuildNote("not incremental: " + why);
      return false;
    }
    for (const ParsedFile* p : readers)
      if (p && !p->isImported)
        rewalk.insert(p->index);
    // Their functions' code changes with what they read (a call bound to a new overload); a
    // global's initializer that read one is no function codegen can emit again on its own.
    std::unordered_set<const Decl*> removedSet(gone.begin(), gone.end());
    for (const Decl* d : readingWalks)
      if (d->kind == NodeKind::FunctionDecl && static_cast<const FunctionDecl*>(d)->body)
      {
        if (!removedSet.count(d))
          kept.changedBodies.push_back(const_cast<FunctionDecl*>(static_cast<const FunctionDecl*>(d)));
      }
      else if (!removedSet.count(d) && d->kind == NodeKind::VariableDecl)
        kept.changedGlobals.push_back(d); // (its constructor emitted again)
      else if (!removedSet.count(d))
        inFull(initializerOf(d) + " read a changed name");
    // The index line counts the entries as the index has them now: what the functions added and
    // took out (scopes none: no function with a class in its body is grafted).
    kept.indexEntries += global.entryCount() - entriesBefore;
    char line[256];
    snprintf(line, sizeof line, "index:   %.3f s  (+%.3f s for out-of-line class definitions): %zu scopes, %zu entries\n", kept.indexTime,
             kept.finishTime, kept.indexScopes, kept.indexEntries);
    kept.indexLine = line;
    if (kept.options.convertDir.empty())
      kept.clashes = global.topLevelClashes();
  }
  // Walked again: what they read recorded anew, and nothing instantiated that a resolve from
  // scratch would count differently.
  std::vector<size_t> walked(rewalk.begin(), rewalk.end());
  // A walk takes back the errors its units reported (forgetWalks) and reports what it finds; one
  // reported outside any unit (the index's) it cannot, and a file with one is not walked again here.
  for (size_t index : walked)
    if (const std::string& path = kept.parsed[index]->path; global.unitlessErrorIn(path))
    {
      logBuildNote("not incremental: " + path + " is walked again and has an error reported outside its walks");
      return false;
    }
  std::vector<std::vector<const void*>> before(walked.size());
  for (size_t n = 0; n < walked.size(); ++n)
  {
    before[n] = global.instancesWalksUse(*kept.parsed[walked[n]]);
    global.forgetWalks(*kept.parsed[walked[n]]);
  }
  // A reading walk's declaration typed by that walk (decltype, auto): its type before, compared
  // with what the walk gives now (changed, what read it is out of date: from scratch).
  std::vector<const Ty*> typesBefore = global.forgetTypesFromWalks(typedByWalk);
  global.beginRewalk();
  // A template's changed body: its instances copied anew and checked again, their statistics
  // replacing theirs (the kept total is made again from every instance walk's own). Before the
  // walks: one that runs the function in the evaluator (a static_assert) runs the instance's copy,
  // which must be the new body's (after them, it ran the old one's and kept its answer).
  size_t droppedInstances = 0; // codegen's, taken out (codegen makes them anew)
  std::vector<const Decl*> instancesOfBodies, instancesOfReaders;
  bool rechecked = templateBodies.empty() || global.recheckInstancesOf(templateBodies, {}, why, &droppedInstances, &instancesOfBodies);
  for (size_t index : walked)
  {
    ResolveStats s;
    s.fileIndex = uint32_t(index);
    s.collectWhy = kept.options.showWhy > 0;
    global.resolveFile(*kept.parsed[index], kept.lexed[index].tokens, kept.files[index].text, s);
    kept.stats[index] = std::move(s);
  }
  // The instances that read a change (invalidateReaders), checked again once the walks are done.
  rechecked = rechecked && (readingInstances.empty() || global.recheckInstancesOf({}, readingInstances, why, &droppedInstances, &instancesOfReaders));
  {
    // (both lists in their files' order, as recheckInstancesOf gives each)
    std::set<const Decl*> seen;
    for (const std::vector<const Decl*>* list : {&instancesOfBodies, &instancesOfReaders})
      for (const Decl* d : *list)
        if (seen.insert(d).second)
          kept.instancesOf.push_back(d);
    std::stable_sort(kept.instancesOf.begin(), kept.instancesOf.end(), [](const Decl* a, const Decl* b)
    {
      uint32_t fa = a->file ? a->file->index : UINT32_MAX, fb = b->file ? b->file->index : UINT32_MAX;
      return fa != fb ? fa < fb : a->token < b->token;
    });
  }
  // Codegen emits again the functions given, those that inlined one of them, the instances of a
  // changed template body, and every instance of a template one of whose instances read a change
  // (instancesOf: those checked again and those taken out, which it makes anew) (rebuildLLVM).
  bool typesSame = true;
  for (size_t n = 0; n < typedByWalk.size(); ++n)
    typesSame = typesSame && global.typeOfDecl(const_cast<Decl*>(typedByWalk[n])) == typesBefore[n];
  bool walkedSame = global.endRewalk();
  bool same = walkedSame && rechecked && typesSame;
  if (!typesSame)
    why = "a declaration typed by its walk (decltype, auto) got another type";
  if ((!templateBodies.empty() || !readingInstances.empty()) && rechecked)
    kept.instanceStats = global.instanceStatsAtResolveEnd();
  if (!walkedSame)
    why = "the new code instantiates what the old did not (or what only codegen had)";
  // What the old walks asked for and no one asks for now (an old lambda's closure in a template's
  // instance): taken out, as a build from scratch never makes it; the instances the rewalk made
  // counted in. The report's counts and the instances' statistics follow.
  GlobalScope::InstantiationStats droppedCounts{};
  std::vector<const Decl*> droppedTemplates;
  for (size_t n = 0; same && n < walked.size(); ++n)
  {
    const ParsedFile& file = *kept.parsed[walked[n]];
    std::vector<const void*> after = global.instancesWalksUse(file), dropped;
    std::set_difference(before[n].begin(), before[n].end(), after.begin(), after.end(), std::back_inserter(dropped));
    global.dropUnusedInstances(dropped, file, droppedCounts, droppedTemplates, kept.droppedClasses); // (its declarations' typing too)
  }
  GlobalScope::InstantiationStats made = global.rewalkCounts();
  if (same && (made.classes || made.functions || made.bodies || !droppedTemplates.empty()))
  {
    kept.instantiation.classes += made.classes - droppedCounts.classes;
    kept.instantiation.functions += made.functions - droppedCounts.functions;
    kept.instantiation.bodies += made.bodies - droppedCounts.bodies;
    kept.instanceStats = global.instanceStatsAtResolveEnd();
    // (codegen drops what it emitted for the old closures itself, rebuildLLVM: not every instance of their templates)
  }
  if (!same)
  {
    logBuildNote("not incremental: " + why);
    return false;
  }
  kept.resolveTime = seconds(Clock::now() - start);
  // The reports a build from scratch prints. The lexer's counts come from lexing every file again
  // into a table of its own (the kept table holds the old texts' identifiers too); with --quiet
  // there is no report, and nothing is lexed again (an editor's check, kppc --lsp).
  start = Clock::now();
  if (!quietOutput)
  {
    SymbolTable fresh;
    std::vector<LexResult> relexed(kept.files.size());
    parallelFor(kept.files.size(), kept.options.threads, [&](size_t i)
    {
      if (!kept.parsed[i]->isImported)
        relexed[i] = lex(kept.files[i].text, fresh);
    });
    double lexTime = seconds(Clock::now() - start);
    FileStats total;
    for (size_t i = 0; i < kept.files.size(); ++i)
      if (!kept.parsed[i]->isImported)
      {
        total.bytes += kept.files[i].text.size();
        total.tokens += relexed[i].tokens.size();
        total.lines += relexed[i].lineCount;
        total.errors += relexed[i].errorCount;
      }
    reportLex(sources, total, fresh.size(), kept.options.threads, collectTime, lexTime);
  }
  double lexTime = seconds(Clock::now() - start);
  report("%s", kept.importsLine.c_str());
  reportParse(kept, parseTime);
  report("%s", kept.indexLine.c_str());
  for (const std::string& clash : kept.clashes)
  {
    fprintf(stderr, "%s\n", clash.c_str());
    codegenExit = 1;
  }
  logBuildNote("incremental: " + std::to_string(changed.size() + functionEdits.size()) + " file(s) changed (" + std::to_string(functionEdits.size()) + " with whole functions), " + std::to_string(walked.size()) + " file(s) walked again" +
               (readingInstances.empty() ? std::string() : ", " + std::to_string(readingInstances.size()) + " instance(s) that read a change checked again") +
               (droppedInstances ? " (" + std::to_string(droppedInstances) + " made by codegen dropped, to be made anew)" : std::string()) +
               (kept.changedLayouts.empty() ? std::string() : ", " + std::to_string(kept.changedLayouts.size()) + " class layout(s) changed"));
  {
    // (the server's own part of the build's time, which the report's lines, a build from scratch's, do not show)
    char times[160];
    snprintf(times, sizeof times, "incremental: read the sources %.3f s, parse the changes %.3f s, walks and checks again %.3f s, lexed again for the report %.3f s",
             collectTime, parseTime, kept.resolveTime, lexTime);
    logBuildNote(times);
  }
  // Codegen follows through its kept session when every module's object is where the last build
  // left it (an unchanged module is not assembled again, so it has no text to compile now).
  // The functions whose walks read an edited function's name are emitted again with it: an edit
  // that makes the function trivial makes its callers inline it (codegen's record knows only the
  // callers that inline it already).
  {
    std::vector<GlobalScope::ChangedName> edited;
    for (FunctionDecl* f : kept.changedBodies)
      edited.push_back(global.changedNameOf(f));
    // (An instance whose code holds an edited body, inlined or constant-folded, codegen finds itself:
    // its inlined-body record, cef267b.)
    for (const Decl* d : global.walksReading(edited))
      if (d->kind == NodeKind::FunctionDecl && static_cast<const FunctionDecl*>(d)->body)
        kept.changedBodies.push_back(const_cast<FunctionDecl*>(static_cast<const FunctionDecl*>(d)));
      else if (d->kind == NodeKind::VariableDecl)
        kept.changedGlobals.push_back(d); // (its constructor emitted again)
      else
        inFull(initializerOf(d) + " read an edited function");
  }
  // (and what the last build changed that generated no code: none of it removed since, as a removal
  // there makes codegen run in full)
  {
    std::unordered_set<const FunctionDecl*> removed(kept.removedFunctions.begin(), kept.removedFunctions.end());
    for (FunctionDecl* f : carriedBodies)
      if (!removed.count(f))
        kept.changedBodies.push_back(f);
    for (const Decl* d : carriedGlobals)
      if (std::find(kept.changedGlobals.begin(), kept.changedGlobals.end(), d) == kept.changedGlobals.end())
        kept.changedGlobals.push_back(d);
  }
  std::sort(kept.changedBodies.begin(), kept.changedBodies.end());
  kept.changedBodies.erase(std::unique(kept.changedBodies.begin(), kept.changedBodies.end()), kept.changedBodies.end());
  // An added function is emitted as added, never as a changed one too (its walk may read an edited
  // name: an overload that calls the function it overloads; codegen then emitted it twice).
  {
    std::unordered_set<const FunctionDecl*> added(kept.addedFunctions.begin(), kept.addedFunctions.end());
    std::erase_if(kept.changedBodies, [&](FunctionDecl* f) { return added.count(f) != 0; });
  }
  for (size_t k = 0; codegenCanFollow && k < kept.moduleBytes.size(); ++k)
  {
    std::error_code ec;
    std::string base = kept.options.exe + "." + std::to_string(k);
    if (!kept.options.check && (kept.options.exe.empty() || !std::filesystem::exists(base + ".o", ec) || !std::filesystem::exists(base + ".hash", ec)))
      inFull("module " + std::to_string(k) + "'s object is missing");
    // Written since by another build of the same outputs (another server: Visual Studio's and a
    // terminal's): its objects hold that build's program, never this one's (2026-11-04: a build
    // after the other server's linked the other's code, silently).
    else if (!kept.options.check && (!leftAsKept(base + ".o") || !leftAsKept(base + ".hash")))
      inFull("module " + std::to_string(k) + "'s object was written by another build");
  }
  kept.rebuildCodegen = codegenCanFollow && kept.codegen && (!kept.options.exe.empty() || kept.options.check);
  if (!kept.rebuildCodegen)
    logBuildNote(std::string("codegen in full: ") + (!kept.codegen ? "no kept session" : !codegenCanFollow ? codegenInFullBecause + (codegenInFullOthers ? " (and " + std::to_string(codegenInFullOthers) + " more)" : std::string()) : "no executable"));
  noteIncrementalBuild();
  reportResolve(kept);
  kept.retiredCodegen.reset(); // (the client has its answer: reportResolve ends with buildDone)
  return true;
}
}
