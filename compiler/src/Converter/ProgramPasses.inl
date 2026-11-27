// ProgramPasses.inl: whole-program passes: library names, inline bodies, macro classes, scoped names.
// Part of convert.cpp, included where it stood (a file of its own since 2026-11-05, moved without a change).

// The names of the standard library's free functions: a top-level declaration line (no
// indentation) with a name before its parameter list, `void sort(It first, It last, F less)`.
std::set<std::string> libraryFunctionNames(const std::filesystem::path& libraryDir)
{
  std::set<std::string> names;
  std::error_code ec;
  if (libraryDir.empty() || !std::filesystem::is_directory(libraryDir, ec))
    return names;
  static const std::set<std::string> notNames = {"if", "for", "while", "switch", "return", "sizeof", "decltype", "requires", "operator",
                                                "alignof", "static_assert", "template"};
  for (const auto& entry : std::filesystem::recursive_directory_iterator(libraryDir, ec))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".kpp")
      continue;
    std::string text;
    if (!readFile(entry.path(), text))
      continue;
    for (size_t pos = 0; pos < text.size();)
    {
      size_t end = text.find('\n', pos);
      if (end == std::string::npos)
        end = text.size();
      std::string_view line(text.data() + pos, end - pos);
      pos = end + 1;
      if (line.empty() || !(isalpha((unsigned char)line[0]) || line[0] == '_') || line.rfind("//", 0) == 0)
        continue;
      size_t paren = line.find('(');
      if (paren == std::string_view::npos || paren == 0)
        continue;
      size_t e = paren;
      while (e > 0 && line[e - 1] == ' ')
        --e;
      size_t b = e;
      while (b > 0 && (isalnum((unsigned char)line[b - 1]) || line[b - 1] == '_'))
        --b;
      if (b == e || b == 0 || line[b - 1] == ':' || line[b - 1] == '.')
        continue; // no name, or a qualified one (an out-of-line member)
      std::string name(line.substr(b, e - b));
      if (!notNames.count(name))
        names.insert(name);
    }
  }
  return names;
}

// --convert-library (setConvertLibraries): the scoped libraries the converted program uses.
// The program's classes that braces construct as parentheses do (defined, no InitializerList
// constructor), by qualified name: a macro's expansion text names them (UnitTest++'s TEST, its
// `UnitTest::ListAdder adder##Fixture##Name (List, &test##Fixture##Name##Instance);`).
static void collectBraceableClasses(Span<Decl*> decls, GlobalScope& global, const SymbolTable& symbols, std::set<std::string>& out)
{
  for (Decl* d : decls)
    if (d->kind == NodeKind::NamespaceDecl)
      collectBraceableClasses(static_cast<NamespaceDecl*>(d)->decls, global, symbols, out);
    else if (d->kind == NodeKind::ClassDecl)
    {
      auto* c = static_cast<ClassDecl*>(d);
      if (c->isDefinition && !c->name.empty() && c->templateParams.empty() && !hasListConstructor(c, symbols))
        out.insert(global.qualifiedName(c));
      collectBraceableClasses(c->members, global, symbols, out);
    }
}

// `a != b` is only ever `!(a == b)` (design.md 4, kovarex 2026-11-09): a declared operator!= that returns bool goes
// where it provably is the negation of the class's ==, so that the checker's coming error finds none and no call
// changes meaning. On an output file's text, after resolveInlineBodies (an out-of-line body is at its declaration):
// - dropped: `= default`; a body `return !(*this == p);`, `!(p1 == p2)`, `!operator==(...)`, `!this->operator==(p)`
//   (whatever == it calls is what kpp's `!(a == b)` calls); a body `return x != y || ...;` (`|` between
//   parenthesized terms too) whose scope's == with the same parameter types is `return x == y && ...;` over the same
//   pairs (each `x != y` of a bool comparison is `!(x == y)`, IEEE's NaN too);
// - made the ==, where the scope has no == of those parameter types: `!=` written `==` and its body negated,
//   `return !x;` as `return x;`, `return x != y || ...;` as `return x == y && ...;` (an iterator's
//   `position != other.position`, a sentinel's `!this->ended()`);
// - kept, each a TODO line in CONVERSION.md for review: a virtual or override one (its chain spans files), one
//   whose == is written through !=, one with no body here, one returning another type than bool, and any other body.
// Single `&` and `|` count only between parenthesized terms (`(x != o.x) | (y != o.y)`), never a type's `&`.
static std::string withNotEqualsDropped(const std::string& text, Counts& counts, std::vector<std::string>& review)
{
  // (also written `operator !=`: Unreal's FIoChunkId and FObjectThumbnail, until 2026-11-27 their files never read)
  bool any = false;
  for (size_t at = text.find("operator"); at != std::string::npos && !any; at = text.find("operator", at + 8))
  {
    size_t next = text.find_first_not_of(" \t\r\n", at + 8);
    any = next != std::string::npos && text.compare(next, 2, "!=") == 0;
  }
  if (!any)
    return text;
  SymbolTable symbols;
  LexResult lexed = lex(text, symbols);
  const std::vector<Token>& toks = lexed.tokens;
  ParsedFile parsed;
  parseFile(lexed.tokens, text, symbols, parsed, Dialect::KppSource);
  auto spell = [&](uint32_t t) { return std::string_view(text).substr(toks[t].offset, toks[t].length); };
  // the tokens [from, to) as text without spaces (an operand's identity)
  auto joined = [&](uint32_t from, uint32_t to)
  {
    std::string s;
    for (uint32_t t = from; t < to; ++t)
      s += spell(t);
    return s;
  };
  // The same, each parameter of fn named by its position (#0, #1), not a member of that name (`o.#0`): two functions'
  // operands compared whatever they name their parameters (`ColorB.R` of == and `Other.R` of != are `#0.R`)
  auto joinedOf = [&](uint32_t from, uint32_t to, FunctionDecl* fn)
  {
    std::string s;
    for (uint32_t t = from; t < to; ++t)
    {
      size_t k = fn ? fn->params.size() : 0;
      if (toks[t].getKind() == TokenKind::Identifier && !(t > from && (toks[t - 1].is(P::Dot) || toks[t - 1].is(P::Arrow) || toks[t - 1].is(P::ColonColon))))
        for (k = 0; k < fn->params.size(); ++k)
          if (static_cast<ParamDecl*>(fn->params[k])->name != UINT32_MAX && toks[t].id == static_cast<ParamDecl*>(fn->params[k])->name)
            break;
      s += fn && k < fn->params.size() ? "#" + std::to_string(k) : std::string(spell(t));
    }
    return s;
  };
  // An expression of the return statement, read as a tree of the operators that matter here.
  struct Node { char kind; uint32_t from, to, op; std::vector<Node> kids; }; // kind: '|' or, '&' and, '!' not, '=' ==, 'n' !=, 'a' atom
  std::function<bool(uint32_t, uint32_t, Node&)> parse = [&](uint32_t from, uint32_t to, Node& out) -> bool
  {
    while (from + 1 < to && toks[from].is(P::LParen))
    {
      // (strip parentheses around the whole)
      int depth = 0;
      uint32_t t = from;
      for (; t < to; ++t)
      {
        if (toks[t].is(P::LParen) || toks[t].is(P::LBracket) || toks[t].is(P::LBrace))
          ++depth;
        else if (toks[t].is(P::RParen) || toks[t].is(P::RBracket) || toks[t].is(P::RBrace))
          --depth;
        if (depth == 0)
          break;
      }
      if (t != to - 1)
        break;
      ++from;
      --to;
    }
    if (from >= to)
      return false;
    // the lowest-precedence top-level operator: || then && then | then & then == !=, a leading ! last
    auto find = [&](auto test) -> uint32_t
    {
      int depth = 0;
      uint32_t found = UINT32_MAX;
      for (uint32_t t = from; t < to; ++t)
      {
        if (toks[t].is(P::LParen) || toks[t].is(P::LBracket) || toks[t].is(P::LBrace))
          ++depth;
        else if (toks[t].is(P::RParen) || toks[t].is(P::RBracket) || toks[t].is(P::RBrace))
          --depth;
        else if (depth == 0 && test(t) && !(t > 0 && toks[t - 1].is(Kw::operator_)))
          found = t; // (the last: left associative)
      }
      return found;
    };
    auto bitwise = [&](uint32_t t, P p) { return toks[t].is(p) && t > from && t + 1 < to && toks[t - 1].is(P::RParen) && toks[t + 1].is(P::LParen); };
    for (auto [kind, test] : std::initializer_list<std::pair<char, std::function<bool(uint32_t)>>>{
           {'|', [&](uint32_t t) { return toks[t].is(P::OrOr); }}, {'&', [&](uint32_t t) { return toks[t].is(P::AndAnd); }},
           {'|', [&](uint32_t t) { return bitwise(t, P::Pipe); }}, {'&', [&](uint32_t t) { return bitwise(t, P::Amp); }}})
      if (uint32_t t = find(test); t != UINT32_MAX)
      {
        out = {kind, from, to, t, {}};
        Node l, r;
        if (!parse(from, t, l) || !parse(t + 1, to, r))
          return false;
        out.kids = {l, r};
        return true;
      }
    // (a top-level `?`, `<`, `,`, an assignment: no form this reads)
    if (find([&](uint32_t t) { return toks[t].is(P::Question) || toks[t].is(P::Comma) || toks[t].is(P::Less) || toks[t].is(P::Greater) || toks[t].is(P::Assign); }) !=
        UINT32_MAX)
    {
      out = {'a', from, to, 0, {}};
      return true;
    }
    if (uint32_t t = find([&](uint32_t t) { return toks[t].is(P::EqEq) || toks[t].is(P::NotEq); }); t != UINT32_MAX)
    {
      out = {toks[t].is(P::EqEq) ? '=' : 'n', from, to, t, {}};
      // An operand in parentheses is one whatever it holds (FGuid's `((X.A ^ Y.A) | (X.B ^ Y.B)) != 0`); unparenthesized
      // (`(x) | (y) != z`), the comparison binds tighter than the `|`: no form this reads.
      auto wrapped = [&](uint32_t f, uint32_t e)
      {
        if (f + 1 >= e || !toks[f].is(P::LParen) || !toks[e - 1].is(P::RParen))
          return false;
        int depth = 0;
        for (uint32_t k = f; k < e; ++k)
        {
          if (toks[k].is(P::LParen) || toks[k].is(P::LBracket) || toks[k].is(P::LBrace))
            ++depth;
          else if (toks[k].is(P::RParen) || toks[k].is(P::RBracket) || toks[k].is(P::RBrace))
            --depth;
          if (depth == 0 && k + 1 < e)
            return false;
        }
        return true;
      };
      Node l, r;
      if (!parse(from, t, l) || !parse(t + 1, to, r))
        return false;
      if (l.kind != 'a')
      {
        if (!wrapped(from, t))
          return false;
        l = {'a', from, t, 0, {}};
      }
      if (r.kind != 'a')
      {
        if (!wrapped(t + 1, to))
          return false;
        r = {'a', t + 1, to, 0, {}};
      }
      out.kids = {l, r};
      return true;
    }
    if (toks[from].is(P::Bang))
    {
      out = {'!', from, to, from, {}};
      Node x;
      if (!parse(from + 1, to, x))
        return false;
      out.kids = {x};
      return true;
    }
    out = {'a', from, to, 0, {}};
    return true;
  };
  // The terms of an or-tree of != (kind '|') or an and-tree of == (kind '&'), each an unordered operand pair.
  // (fn: the function whose parameters the operands name, by position: joinedOf)
  std::function<bool(const Node&, char, char, std::multiset<std::pair<std::string, std::string>>&, FunctionDecl*)> terms =
    [&](const Node& n, char tree, char comparison, std::multiset<std::pair<std::string, std::string>>& out, FunctionDecl* fn) -> bool
  {
    if (n.kind == tree)
      return terms(n.kids[0], tree, comparison, out, fn) && terms(n.kids[1], tree, comparison, out, fn);
    if (n.kind != comparison)
      return false;
    std::string a = joinedOf(n.kids[0].from, n.kids[0].to, fn), b = joinedOf(n.kids[1].from, n.kids[1].to, fn);
    out.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
    return true;
  };
  struct Function { FunctionDecl* f; Decl* scope; std::string params; bool isConst; };
  std::vector<Function> equals, notEquals, namespaceDefinitions;
  auto paramText = [&](FunctionDecl* f)
  {
    std::string s;
    for (Decl* p : f->params)
    {
      auto* pd = static_cast<ParamDecl*>(p);
      // (the type: from its first token up to the name, or the parameter's end, a `,` or `)` outside brackets)
      uint32_t from = pd->type ? std::min(pd->type->token, pd->token) : pd->token;
      std::string t;
      int depth = 0;
      for (uint32_t k = from; k < toks.size(); ++k)
      {
        if (toks[k].getKind() == TokenKind::Identifier && pd->name != UINT32_MAX && toks[k].id == pd->name && depth == 0)
          break;
        if (toks[k].is(P::LParen) || toks[k].is(P::LBracket) || toks[k].is(P::Less))
          ++depth;
        else if (toks[k].is(P::RParen) || toks[k].is(P::RBracket) || toks[k].is(P::Greater))
        {
          if (depth == 0)
            break;
          --depth;
        }
        else if (depth == 0 && (toks[k].is(P::Comma) || toks[k].is(P::Assign)))
          break;
        t += spell(k);
      }
      s += t + ",";
    }
    return s;
  };
  // The file's classes by name, for a member defined below its class (a class template's, whose body stays outside:
  // `template<typename T> bool Range<T>::operator!=(const Sentinel&) const {...}`); a name two classes have is none.
  std::map<uint32_t, Decl*> classesByName;
  std::function<void(Span<Decl*>)> collectClasses = [&](Span<Decl*> decls)
  {
    for (Decl* d : decls)
      if (d->kind == NodeKind::NamespaceDecl)
        collectClasses(static_cast<NamespaceDecl*>(d)->decls);
      else if (d->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(d)->isDefinition)
      {
        auto* c = static_cast<ClassDecl*>(d);
        if (!c->name.empty() && c->name.last().symbol != UINT32_MAX)
        {
          auto [it, fresh] = classesByName.insert({c->name.last().symbol, c});
          if (!fresh)
            it->second = nullptr;
        }
        collectClasses(c->members);
      }
  };
  collectClasses(parsed.decls);
  std::function<void(Span<Decl*>, Decl*)> collect = [&](Span<Decl*> decls, Decl* scope)
  {
    for (Decl* d : decls)
    {
      if (d->kind == NodeKind::NamespaceDecl)
        collect(static_cast<NamespaceDecl*>(d)->decls, d);
      else if (d->kind == NodeKind::ClassDecl)
        collect(static_cast<ClassDecl*>(d)->members, d);
      else if (d->kind == NodeKind::FunctionDecl)
      {
        auto* f = static_cast<FunctionDecl*>(d);
        if (f->name.parts.size() > 1 && f->name.last().special == 2 && f->body)
        {
          // (a member defined below its class: its class's, when the file has one class of that name)
          auto c = classesByName.find(f->name.parts[f->name.parts.size() - 2].symbol);
          uint32_t op = f->name.last().opToken;
          if (c != classesByName.end() && c->second && (op == uint32_t(P::NotEq) || op == uint32_t(P::EqEq)))
            (op == uint32_t(P::NotEq) ? notEquals : equals).push_back({f, c->second, paramText(f), bool(f->flags & DF_Const)});
          else if (c == classesByName.end() && (op == uint32_t(P::NotEq) || op == uint32_t(P::EqEq)))
            namespaceDefinitions.push_back({f, nullptr, paramText(f), bool(f->flags & DF_Const)}); // (its friend's class: below)
          continue;
        }
        if (f->name.parts.size() != 1 || f->name.last().special != 2)
          continue;
        uint32_t op = f->name.last().opToken;
        if (op == uint32_t(P::NotEq) || op == uint32_t(P::EqEq))
          (op == uint32_t(P::NotEq) ? notEquals : equals).push_back({f, scope, paramText(f), bool(f->flags & DF_Const)});
      }
    }
  };
  collect(parsed.decls, nullptr);
  // A friend declared in a class and defined below it qualified by its namespace (Unreal's FVersePath: `friend bool
  // operator!=(const FVersePath&, const FVersePath&);` and `bool UE::Core::operator!=(...) { return !(Lhs == Rhs); }`):
  // the definition is the friend's, of its class, as the converter decides both (until 2026-11-27 the declaration was
  // kept for review, "no body here")
  for (Function& d : namespaceDefinitions)
  {
    bool notEqual = d.f->name.last().opToken == uint32_t(P::NotEq);
    for (const Function& x : notEqual ? notEquals : equals)
      if (!x.f->body && (x.f->flags & DF_Friend) && x.params == d.params && x.isConst == d.isConst && x.scope)
      {
        d.scope = x.scope;
        break;
      }
    if (d.scope)
      (notEqual ? notEquals : equals).push_back(d);
  }
  if (notEquals.empty())
    return text;
  struct Edit { uint32_t offset, length; std::string text; };
  std::vector<Edit> edits;
  auto declStart = [&](FunctionDecl* f)
  {
    // back from the return type to just after what ends the declaration before it (`;`, a brace, an access label):
    // its specifiers, attributes, template header and requires-clause with it
    uint32_t t = f->returnType ? f->returnType->token : f->token;
    while (t > 0)
    {
      uint32_t p = t - 1;
      if (toks[p].is(P::Semi) || toks[p].is(P::LBrace) || toks[p].is(P::RBrace) ||
          (toks[p].is(P::Colon) && p > 0 && (toks[p - 1].is(Kw::public_) || toks[p - 1].is(Kw::private_) || toks[p - 1].is(Kw::protected_))))
        break;
      t = p;
    }
    return t;
  };
  // the `!=` of `operator!=` (f->endToken when not found)
  auto operatorToken = [&](FunctionDecl* f)
  {
    uint32_t t = declStart(f);
    while (t + 1 < f->endToken && !(toks[t].is(Kw::operator_) && toks[t + 1].is(P::NotEq)))
      ++t;
    return t + 1 < f->endToken ? t + 1 : f->endToken;
  };
  auto removeDeclaration = [&](FunctionDecl* f)
  {
    uint32_t start = toks[declStart(f)].offset;
    uint32_t end = toks[f->endToken - 1].offset + toks[f->endToken - 1].length;
    uint32_t lineStart = start;
    while (lineStart > 0 && (text[lineStart - 1] == ' ' || text[lineStart - 1] == '\t'))
      --lineStart;
    uint32_t lineEnd = end;
    while (lineEnd < text.size() && (text[lineEnd] == ' ' || text[lineEnd] == '\t' || text[lineEnd] == '\r'))
      ++lineEnd;
    if ((lineStart == 0 || text[lineStart - 1] == '\n') && (lineEnd == text.size() || text[lineEnd] == '\n'))
      edits.push_back({lineStart, uint32_t(lineEnd + (lineEnd < text.size() ? 1 : 0) - lineStart), ""});
    else
      edits.push_back({start, end - start, ""});
  };
  auto keep = [&](const Function& n, const std::string& why)
  {
    // (its class, and its declaration as written up to its body, blanks collapsed)
    std::string owner;
    if (n.scope && n.scope->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(n.scope)->name.empty())
      owner = std::string(symbols.name(static_cast<ClassDecl*>(n.scope)->name.last().symbol)) + ": ";
    uint32_t from = toks[declStart(n.f)].offset, to = toks[n.f->endToken - 1].offset + toks[n.f->endToken - 1].length;
    std::string written;
    for (uint32_t k = from; k < to && written.size() < 160; ++k)
      if (!isspace((unsigned char)text[k]))
        written += text[k];
      else if (!written.empty() && written.back() != ' ')
        written += ' ';
    review.push_back("operator!= kept (" + why + "): " + owner + "`" + written + "`");
    counts.add("operator!= kept for review: " + why);
  };
  // (the definitions first: a declaration whose definition is below it in the file follows what was decided for it)
  std::stable_partition(notEquals.begin(), notEquals.end(), [](const Function& x) { return x.f->body != nullptr; });
  auto keyOf = [](const Function& x) { return std::to_string(uintptr_t(x.scope)) + "|" + x.params + "|" + (x.isConst ? "c" : ""); };
  std::map<std::string, char> decided; // 'd' dropped, 'm' made the ==
  for (const Function& n : notEquals)
  {
    FunctionDecl* f = n.f;
    if (f->returnType && spell(f->returnType->token) != "bool")
    {
      keep(n, "it returns " + std::string(spell(f->returnType->token)) + ", not bool");
      continue;
    }
    if (f->flags & (DF_Virtual | DF_Override))
    {
      keep(n, "virtual");
      continue;
    }
    const Function* eq = nullptr;
    for (const Function& e : equals)
      if (e.scope == n.scope && e.params == n.params && e.isConst == n.isConst)
        eq = &e;
    if (f->flags & DF_Default)
    {
      removeDeclaration(f);
      decided[keyOf(n)] = 'd';
      counts.add("operator!= dropped: defaulted");
      continue;
    }
    auto onlyReturn = [&](FunctionDecl* g, uint32_t& from, uint32_t& to) -> bool
    {
      if (!g->body || g->body->kind != NodeKind::BlockStmt)
        return false;
      auto* b = static_cast<BlockStmt*>(g->body);
      if (b->stmts.size() != 1 || !b->stmts[0] || b->stmts[0]->kind != NodeKind::ReturnStmt)
        return false;
      from = b->stmts[0]->token + 1;
      int depth = 0;
      for (to = from; to < toks.size(); ++to)
      {
        if (toks[to].is(P::LParen) || toks[to].is(P::LBracket) || toks[to].is(P::LBrace))
          ++depth;
        else if (toks[to].is(P::RParen) || toks[to].is(P::RBracket) || toks[to].is(P::RBrace))
          --depth;
        else if (depth == 0 && toks[to].is(P::Semi))
          break;
      }
      return to < toks.size() && from < to;
    };
    // The body's last statement a return, and nothing before it returning or throwing (`ensure(...); return a != b;`,
    // Unreal's TRangedForIterator): its expression, which an operator!= made the == negates alone; never for one
    // dropped, whose statements before would go with it.
    auto lastReturn = [&](FunctionDecl* g, uint32_t& from, uint32_t& to) -> bool
    {
      if (!g->body || g->body->kind != NodeKind::BlockStmt)
        return false;
      auto* b = static_cast<BlockStmt*>(g->body);
      Stmt* last = b->stmts.empty() ? nullptr : b->stmts[b->stmts.size() - 1];
      if (!last || last->kind != NodeKind::ReturnStmt)
        return false;
      // (a lambda's own return is none of the function's: `ensure(c, [&]() { return "..."; });`, the message of Unreal's
      // ensureMsgf before TRangedForIterator's return)
      bool leaves = false;
      for (size_t k = 0; k + 1 < b->stmts.size(); ++k)
        forEachNodeExcept(
          b->stmts[k],
          [&](::Node* x) { leaves = leaves || x->kind == NodeKind::ReturnStmt || x->kind == NodeKind::ThrowExpr || x->kind == NodeKind::GotoStmt; },
          [](::Node* x) { return x->kind == NodeKind::LambdaExpr; });
      if (leaves)
        return false;
      from = last->token + 1;
      int depth = 0;
      for (to = from; to < toks.size(); ++to)
      {
        if (toks[to].is(P::LParen) || toks[to].is(P::LBracket) || toks[to].is(P::LBrace))
          ++depth;
        else if (toks[to].is(P::RParen) || toks[to].is(P::RBracket) || toks[to].is(P::RBrace))
          --depth;
        else if (depth == 0 && toks[to].is(P::Semi))
          break;
      }
      return to < toks.size() && from < to;
    };
    uint32_t from = 0, to = 0;
    if (f->flags & DF_Delete)
    {
      // `friend bool operator!=(const TSet&, const TSet&) = delete;` beside a deleted == (Unreal's containers): kpp's
      // `a != b` is `!(a == b)`, which the deleted == refuses already
      if (eq && (eq->f->flags & DF_Delete))
      {
        removeDeclaration(f);
        decided[keyOf(n)] = 'd';
        counts.add("operator!= dropped: deleted, as its == is");
        continue;
      }
      keep(n, "deleted, its == not");
      continue;
    }
    if (!f->body)
    {
      if (auto d = decided.find(keyOf(n)); d != decided.end() && d->second == 'd')
      {
        removeDeclaration(f);
        counts.add("operator!= declaration dropped with its definition");
        continue;
      }
      if (!eq)
      {
        // A declaration whose body is elsewhere (a class template's member defined below its class, Unreal's
        // TDirectoryTree::TIterator): with no == of its parameters, its definition is made the == where it is (below),
        // so the declaration is too (a definition kept for review leaves the two apart: an error, never another call)
        uint32_t opTok = operatorToken(f);
        if (opTok >= f->endToken)
        {
          keep(n, "its operator token not found");
          continue;
        }
        edits.push_back({toks[opTok].offset, toks[opTok].length, "=="});
        counts.add("operator!= declaration made the == its definition is: no == of its parameters");
        continue;
      }
      keep(n, "no body here");
      continue;
    }
    Node e;
    if (!(onlyReturn(f, from, to) || (!eq && lastReturn(f, from, to))) || !parse(from, to, e))
    {
      // Statement by statement the =='s, each return its negation (Unreal's TRange: `if (IsEmpty() && Other.IsEmpty())
      // return false; return (LowerBound != Other.LowerBound) || ...;` beside == returning true and the `==`s and `&&`s)
      auto closing = [&](uint32_t open) -> uint32_t
      {
        int depth = 0;
        for (uint32_t k = open; k < toks.size(); ++k)
        {
          if (toks[k].is(P::LParen) || toks[k].is(P::LBracket) || toks[k].is(P::LBrace))
            ++depth;
          else if ((toks[k].is(P::RParen) || toks[k].is(P::RBracket) || toks[k].is(P::RBrace)) && --depth == 0)
            return k;
        }
        return UINT32_MAX;
      };
      auto semicolon = [&](uint32_t start) -> uint32_t // (the `;` that ends a simple statement, at its depth)
      {
        int depth = 0;
        for (uint32_t k = start; k < toks.size(); ++k)
        {
          if (toks[k].is(P::LParen) || toks[k].is(P::LBracket) || toks[k].is(P::LBrace))
            ++depth;
          else if (toks[k].is(P::RParen) || toks[k].is(P::RBracket) || toks[k].is(P::RBrace))
            --depth;
          else if (depth == 0 && toks[k].is(P::Semi))
            return k;
        }
        return UINT32_MAX;
      };
      auto negates = [&](uint32_t xf, uint32_t xt, FunctionDecl* fx, uint32_t ef, uint32_t et, FunctionDecl* fe) -> bool
      {
        while (xt > xf + 1 && toks[xf].is(P::LParen) && closing(xf) == xt - 1)
          ++xf, --xt;
        while (et > ef + 1 && toks[ef].is(P::LParen) && closing(ef) == et - 1)
          ++ef, --et;
        if (xt == xf + 1 && et == ef + 1)
          return (spell(xf) == "false" && spell(ef) == "true") || (spell(xf) == "true" && spell(ef) == "false");
        Node x, y;
        if (!parse(xf, xt, x) || !parse(ef, et, y))
          return false;
        if (x.kind == '!' && joinedOf(x.kids[0].from, x.kids[0].to, fx) == joinedOf(ef, et, fe))
          return true;
        if (y.kind == '!' && joinedOf(y.kids[0].from, y.kids[0].to, fe) == joinedOf(xf, xt, fx))
          return true;
        std::multiset<std::pair<std::string, std::string>> xs, ys;
        return terms(x, '|', 'n', xs, fx) && terms(y, '&', '=', ys, fe) && xs == ys;
      };
      std::function<bool(Stmt*, Stmt*)> negatedStatement = [&](Stmt* a, Stmt* b) -> bool
      {
        if (!a || !b || a->kind != b->kind)
          return false;
        switch (a->kind)
        {
          case NodeKind::BlockStmt:
          {
            auto* x = static_cast<BlockStmt*>(a);
            auto* y = static_cast<BlockStmt*>(b);
            if (x->stmts.size() != y->stmts.size())
              return false;
            for (uint32_t k = 0; k < x->stmts.size(); ++k)
              if (!negatedStatement(x->stmts[k], y->stmts[k]))
                return false;
            return true;
          }
          case NodeKind::ReturnStmt:
          {
            uint32_t xe = semicolon(a->token + 1), ye = semicolon(b->token + 1);
            return xe != UINT32_MAX && ye != UINT32_MAX && xe > a->token + 1 && ye > b->token + 1 &&
                   negates(a->token + 1, xe, f, b->token + 1, ye, eq->f);
          }
          case NodeKind::IfStmt:
          {
            auto* x = static_cast<IfStmt*>(a);
            auto* y = static_cast<IfStmt*>(b);
            if (x->init || y->init || x->ifConsteval || y->ifConsteval || x->isConstexpr != y->isConstexpr || !x->cond || !y->cond ||
                !toks[a->token + 1].is(P::LParen) || !toks[b->token + 1].is(P::LParen))
              return false;
            uint32_t xc = closing(a->token + 1), yc = closing(b->token + 1);
            if (xc == UINT32_MAX || yc == UINT32_MAX || joinedOf(a->token + 2, xc, f) != joinedOf(b->token + 2, yc, eq->f))
              return false;
            if (!negatedStatement(x->then, y->then))
              return false;
            return (!x->else_ && !y->else_) || negatedStatement(x->else_, y->else_);
          }
          default:
          {
            // (any other statement: the same, returning nothing)
            uint32_t xe = semicolon(a->token), ye = semicolon(b->token);
            if (xe == UINT32_MAX || ye == UINT32_MAX)
              return false;
            for (uint32_t k = a->token; k < xe; ++k)
              if (toks[k].is(Kw::return_) || toks[k].is(Kw::throw_))
                return false;
            return joinedOf(a->token, xe, f) == joinedOf(b->token, ye, eq->f);
          }
        }
      };
      if (eq && eq->f->body && f->body && eq->f->params.size() == f->params.size() && negatedStatement(f->body, eq->f->body))
      {
        removeDeclaration(f);
        decided[keyOf(n)] = 'd';
        counts.add("operator!= dropped: statement by statement the negation of ==");
        continue;
      }
      keep(n, "a body other than one return");
      continue;
    }
    // the names `*this` and the parameters, whose == is the class's or the pair's
    std::set<std::string> selves;
    if (((f->parent && f->parent->kind == NodeKind::ClassDecl) || f->name.parts.size() > 1) && !(f->flags & (DF_Static | DF_Friend)))
      selves.insert("*this"); // (a member, also one defined below its class)
    for (Decl* p : f->params)
      if (static_cast<ParamDecl*>(p)->name != UINT32_MAX)
        selves.insert(std::string(symbols.name(static_cast<ParamDecl*>(p)->name)));
    auto isSelfPair = [&](const Node& c)
    {
      std::string a = joined(c.kids[0].from, c.kids[0].to), b = joined(c.kids[1].from, c.kids[1].to);
      return a != b && selves.count(a) && selves.count(b);
    };
    // == written through != (`return !(*this != other);`): this != is the real one
    bool eqThroughNotEqual = false;
    if (eq)
    {
      uint32_t ef = 0, et = 0;
      Node ee;
      if (eq->f->body && onlyReturn(eq->f, ef, et))
      {
        std::string body = joined(ef, et);
        eqThroughNotEqual = body.find("operator!=") != std::string::npos ||
                            (parse(ef, et, ee) && ee.kind == '!' && ee.kids[0].kind == 'n' && isSelfPair(ee.kids[0]));
      }
    }
    if (eqThroughNotEqual)
    {
      keep(n, "its == is written through !=");
      continue;
    }
    if (e.kind == '!')
    {
      const Node& x = e.kids[0];
      std::string inner = joined(x.from, x.to);
      // (`!(FConfigValue::operator==(Other))`: the class's own ==, named through the class)
      std::string ownQualified;
      if (n.scope && n.scope->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(n.scope)->name.empty())
        ownQualified = std::string(symbols.name(static_cast<ClassDecl*>(n.scope)->name.last().symbol)) + "::operator==(";
      bool callsEquals = (x.kind == '=' && isSelfPair(x)) || inner.rfind("operator==(", 0) == 0 || inner.rfind("this->operator==(", 0) == 0 ||
                         (!ownQualified.empty() && inner.rfind(ownQualified, 0) == 0);
      // `return !this->Equals(other);` beside an == whose body is `return this->Equals(other);`, its parameters named alike
      // or not (Unreal's TBigInt IsEqual, TMemoryView and FCbPackage Equals): the same test, negated.
      if (!callsEquals && eq && eq->f->body && eq->f->params.size() == f->params.size())
      {
        uint32_t ef = 0, et = 0;
        if (onlyReturn(eq->f, ef, et))
        {
          // the tokens' spellings, each parameter's name written as its position
          auto normalized = [&](uint32_t from, uint32_t to, FunctionDecl* fn)
          {
            std::string out;
            for (uint32_t t = from; t < to; ++t)
            {
              std::string_view word = spell(t);
              std::string piece(word);
              for (size_t k = 0; k < fn->params.size(); ++k)
                if (static_cast<ParamDecl*>(fn->params[k])->name != UINT32_MAX && symbols.name(static_cast<ParamDecl*>(fn->params[k])->name) == word)
                  piece = "#" + std::to_string(k);
              out += piece + " ";
            }
            return out;
          };
          uint32_t xf = x.from, xt = x.to;
          // (`!(expression)`: the expression inside the parentheses)
          while (xt > xf + 1 && toks[xf].is(P::LParen) && toks[xt - 1].is(P::RParen))
            ++xf, --xt;
          while (et > ef + 1 && toks[ef].is(P::LParen) && toks[et - 1].is(P::RParen))
            ++ef, --et;
          callsEquals = normalized(xf, xt, f) == normalized(ef, et, eq->f);
        }
      }
      if (callsEquals)
      {
        removeDeclaration(f);
        decided[keyOf(n)] = 'd';
        counts.add("operator!= dropped: the negation of ==");
        continue;
      }
      if (!eq)
      {
        // (made the ==: `return !x;` is `return x;`)
        uint32_t opTok = operatorToken(f);
        if (opTok >= f->endToken)
        {
          keep(n, "its operator token not found");
          continue;
        }
        edits.push_back({toks[opTok].offset, toks[opTok].length, "=="});
        edits.push_back({toks[e.from].offset, toks[e.from].length, ""});
        counts.add("operator!= made the == it negates: no == of its parameters");
        decided[keyOf(n)] = 'm';
        continue;
      }
      keep(n, "a negation of something else than its ==");
      continue;
    }
    std::multiset<std::pair<std::string, std::string>> ne;
    if (terms(e, '|', 'n', ne, f))
    {
      if (eq)
      {
        uint32_t ef = 0, et = 0;
        Node ee;
        std::multiset<std::pair<std::string, std::string>> eqTerms;
        // (the parameters by position: FLinearColor's == names its ColorB, its != Other)
        if (eq->f->body && eq->f->params.size() == f->params.size() && onlyReturn(eq->f, ef, et) && parse(ef, et, ee) &&
            terms(ee, '&', '=', eqTerms, eq->f) && eqTerms == ne)
        {
          removeDeclaration(f);
          decided[keyOf(n)] = 'd';
          counts.add("operator!= dropped: its terms the negation of =='s");
          continue;
        }
        // (a defaulted == compares the bases and each data member: the same when the class has no base and the terms
        // are `this->m != p.m`, each data member once)
        if ((eq->f->flags & DF_Default) && n.scope && n.scope->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(n.scope)->bases.empty() &&
            f->params.size() == 1 && static_cast<ParamDecl*>(f->params[0])->name != UINT32_MAX)
        {
          std::string other = "#0"; // (its parameter, by position: joinedOf)
          std::multiset<std::pair<std::string, std::string>> members;
          for (Decl* m : static_cast<ClassDecl*>(n.scope)->members)
            if (m->kind == NodeKind::VariableDecl && !(m->flags & DF_Static) && static_cast<VariableDecl*>(m)->name.parts.size() == 1)
            {
              // (an array member: a defaulted == compares its elements, `this->a != other.a` its addresses)
              for (uint32_t k = m->token; k < m->endToken && k < toks.size(); ++k)
                if (toks[k].is(P::LBracket))
                  members.insert({"[", "array"});
              std::string name(symbols.name(static_cast<VariableDecl*>(m)->name.last().symbol));
              std::string a = "this->" + name, b = other + "." + name;
              members.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
            }
          if (!members.empty() && members == ne)
          {
            removeDeclaration(f);
            decided[keyOf(n)] = 'd';
            counts.add("operator!= dropped: its terms the negation of the defaulted =='s");
            continue;
          }
        }
        keep(n, "its terms are not its =='s negated");
        continue;
      }
      // (made the ==: each != written ==, each || &&, each | &)
      uint32_t opTok = operatorToken(f);
      if (opTok >= f->endToken)
      {
        keep(n, "its operator token not found");
        continue;
      }
      edits.push_back({toks[opTok].offset, toks[opTok].length, "=="});
      std::function<void(const Node&)> flip = [&](const Node& x)
      {
        if (x.kind == 'n')
          edits.push_back({toks[x.op].offset, toks[x.op].length, "=="});
        else if (x.kind == '|')
        {
          edits.push_back({toks[x.op].offset, toks[x.op].length, toks[x.op].is(P::OrOr) ? "&&" : "&"});
          flip(x.kids[0]);
          flip(x.kids[1]);
        }
      };
      flip(e);
      counts.add("operator!= made the == it negates: no == of its parameters");
      decided[keyOf(n)] = 'm';
      continue;
    }
    if (eq && eq->f->body && eq->f->params.size() == f->params.size())
    {
      // `return x;` beside an == whose body is `return !x;` (`operator!=(nullptr_t)` returning `IsValid()` beside an ==
      // returning `!IsValid()`, Unreal's TPimplPtr and TCbFieldIterator; `(bool)*this` beside `!*this`, TFunction), and
      // `return false;` beside `return true;` (FNull): the same test, negated
      uint32_t ef = 0, et = 0;
      Node ee;
      uint32_t xf = e.from, xt = e.to;
      if (xt > xf + 3 && toks[xf].is(P::LParen) && spell(xf + 1) == "bool" && toks[xf + 2].is(P::RParen))
        xf += 3; // (a value tested as a bool: `(bool)x` is x)
      std::string tested = joinedOf(xf, xt, f);
      if (onlyReturn(eq->f, ef, et) && parse(ef, et, ee) &&
          ((ee.kind == '!' && joinedOf(ee.kids[0].from, ee.kids[0].to, eq->f) == tested) ||
           (xt == xf + 1 && et == ef + 1 && ((tested == "false" && spell(ef) == "true") || (tested == "true" && spell(ef) == "false")))))
      {
        removeDeclaration(f);
        decided[keyOf(n)] = 'd';
        counts.add("operator!= dropped: the negation of ==");
        continue;
      }
    }
    if (!eq)
    {
      // (made the ==: `return x;` is `return !(x);`)
      uint32_t opTok = operatorToken(f);
      if (opTok >= f->endToken)
      {
        keep(n, "its operator token not found");
        continue;
      }
      edits.push_back({toks[opTok].offset, toks[opTok].length, "=="});
      edits.push_back({toks[from].offset, 0, "!("});
      edits.push_back({toks[to - 1].offset + toks[to - 1].length, 0, ")"});
      counts.add("operator!= made the == it negates: no == of its parameters");
      decided[keyOf(n)] = 'm';
      continue;
    }
    {
      // The =='s expression negated by De Morgan's laws, its operands in either order (Unreal's TRangeBound: `(Type !=
      // Other.Type) || (!IsOpen() && (Value != Other.Value))` beside `(Type == Other.Type) && (IsOpen() || (Value ==
      // Other.Value))`; FMD5Hash's the same with the validity first): kept for review until 2026-11-27
      uint32_t ef = 0, et = 0;
      Node ee;
      std::function<bool(const Node&, const Node&)> same, negation;
      auto pairOf = [&](const Node& x, FunctionDecl* fx)
      {
        std::string a = joinedOf(x.kids[0].from, x.kids[0].to, fx), b = joinedOf(x.kids[1].from, x.kids[1].to, fx);
        return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
      };
      // (x of the !=, y of the ==)
      same = [&](const Node& x, const Node& y) -> bool
      {
        if (x.kind != y.kind)
          return false;
        switch (x.kind)
        {
          case 'a':
            return joinedOf(x.from, x.to, f) == joinedOf(y.from, y.to, eq->f);
          case '=':
          case 'n':
            return pairOf(x, f) == pairOf(y, eq->f);
          case '!':
            return same(x.kids[0], y.kids[0]);
          default:
            return toks[x.op].is(P::OrOr) == toks[y.op].is(P::OrOr) && toks[x.op].is(P::AndAnd) == toks[y.op].is(P::AndAnd) &&
                   ((same(x.kids[0], y.kids[0]) && same(x.kids[1], y.kids[1])) || (same(x.kids[0], y.kids[1]) && same(x.kids[1], y.kids[0])));
        }
      };
      negation = [&](const Node& x, const Node& y) -> bool
      {
        if (x.kind == '!' && same(x.kids[0], y))
          return true;
        if (y.kind == '!' && same(x, y.kids[0]))
          return true;
        if ((x.kind == 'n' && y.kind == '=') || (x.kind == '=' && y.kind == 'n'))
          return pairOf(x, f) == pairOf(y, eq->f);
        // (|| against &&, never the bitwise | and &: their operands are not bools)
        if (((x.kind == '|' && y.kind == '&') || (x.kind == '&' && y.kind == '|')) && toks[x.op].is(x.kind == '|' ? P::OrOr : P::AndAnd) &&
            toks[y.op].is(y.kind == '|' ? P::OrOr : P::AndAnd))
          return (negation(x.kids[0], y.kids[0]) && negation(x.kids[1], y.kids[1])) ||
                 (negation(x.kids[0], y.kids[1]) && negation(x.kids[1], y.kids[0]));
        return false;
      };
      if (eq && eq->f->body && eq->f->params.size() == f->params.size() && onlyReturn(eq->f, ef, et) && parse(ef, et, ee) && negation(e, ee))
      {
        removeDeclaration(f);
        decided[keyOf(n)] = 'd';
        counts.add("operator!= dropped: the negation of =='s expression");
        continue;
      }
    }
    keep(n, "a body of another form");
  }
  if (edits.empty())
    return text;
  std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.offset > b.offset; });
  std::string out = text;
  for (const Edit& e : edits)
    out.replace(e.offset, e.length, e.text);
  return out;
}

// The marks markInlineBody left in an output file's text, resolved: each marked definition's body
// (from where its declarator ends) replaces its declaration's `;`, indented as the declaration is,
// and the definition goes. A declaration whose definition this text has not (it is in a part a
// configuration leaves out) keeps its `;`; a definition whose declaration is not here stays.
static std::string resolveInlineBodies(std::string text, Counts& counts)
{
  if (text.find("/*kpp:") == std::string::npos)
    return text;
  auto lineIndent = [&](size_t at)
  {
    size_t lineStart = text.rfind('\n', at);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    size_t end = lineStart;
    while (end < text.size() && (text[end] == ' ' || text[end] == '\t'))
      ++end;
    return text.substr(lineStart, end - lineStart);
  };
  // (marks that coincide, `*//*kpp:def:`, come from declarations or definitions one macro's use
  // makes, which share its tokens: none of them moves)
  std::set<std::string> shared;
  for (const char* kind : {"/*kpp:def:", "/*kpp:inline:"})
    for (size_t at = text.find(kind); at != std::string::npos; at = text.find(kind, at + 1))
    {
      size_t idStart = at + strlen(kind), idEnd = text.find_first_not_of("0123456789", idStart);
      size_t previous = at >= 2 ? text.rfind("/*", at - 2) : std::string::npos;
      bool before = previous != std::string::npos && text.compare(at - 2, 2, "*/") == 0 && text.compare(previous, 6, "/*kpp:") == 0 &&
                    text.compare(previous, 9, "/*kpp:if ") != 0 && text.compare(previous, 12, "/*kpp:endif ") != 0;
      size_t close = text.find("*/", idStart);
      bool after = close != std::string::npos && text.compare(close + 2, 6, "/*kpp:") == 0 && text.compare(close + 2, 9, "/*kpp:if ") != 0 &&
                   text.compare(close + 2, 12, "/*kpp:endif ") != 0;
      if (before || after)
        shared.insert(text.substr(idStart, idEnd - idStart));
    }
  // The bodies, by id, with the indentation of their definition's first line.
  std::map<std::string, std::pair<std::string, std::string>> bodies;
  std::map<std::string, std::pair<size_t, size_t>> definitionRanges;
  // The `//` lines right above a definition (no blank line between): its comment, which goes with
  // the body, above the declaration.
  auto commentAbove = [&](size_t at, size_t& from)
  {
    size_t lineStart = text.rfind('\n', at);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    from = lineStart;
    std::vector<std::string> lines;
    while (from > 0)
    {
      size_t prevEnd = from - 1, prevStart = text.rfind('\n', prevEnd == 0 ? 0 : prevEnd - 1);
      prevStart = prevStart == std::string::npos || prevEnd == 0 ? 0 : prevStart + 1;
      std::string line = text.substr(prevStart, prevEnd - prevStart);
      size_t first = line.find_first_not_of(" \t");
      if (first == std::string::npos || line.compare(first, 2, "//") != 0)
        break;
      lines.insert(lines.begin(), line.substr(first));
      from = prevStart;
    }
    return lines;
  };
  std::map<std::string, std::vector<std::string>> comments;
  std::map<std::string, size_t> commentStarts;
  // (a definition or declaration inside an #if group, `/*kpp:if FILE:N*/` to its `/*kpp:endif*/` in a
  // conversion the configurations merge, stays: the other configuration may have neither, and the
  // merge aligns the two texts outside the groups)
  std::vector<std::pair<size_t, int>> groupMarks;
  for (size_t at = text.find("/*kpp:"); at != std::string::npos; at = text.find("/*kpp:", at + 1))
    if (text.compare(at, 9, "/*kpp:if ") == 0)
      groupMarks.push_back({at, 1});
    else if (text.compare(at, 12, "/*kpp:endif ") == 0)
      groupMarks.push_back({at, -1});
  auto inGroup = [&](size_t at)
  {
    int depth = 0;
    for (const auto& [mark, step] : groupMarks)
      if (mark < at)
        depth += step;
    return depth > 0;
  };
  for (size_t at = text.find("/*kpp:def:"); at != std::string::npos; at = text.find("/*kpp:def:", at + 1))
  {
    size_t idEnd = text.find("{*/", at);
    if (idEnd == std::string::npos)
      break;
    std::string id = text.substr(at + 10, idEnd - at - 10);
    std::string bodyMark = "/*kpp:body:" + id + "*/", endMark = "/*}kpp:def:" + id + "*/";
    size_t body = text.find(bodyMark, idEnd), end = text.find(endMark, idEnd);
    if (body == std::string::npos || end == std::string::npos || body > end || inGroup(at) || shared.count(id))
      continue;
    // (one definition between the marks: a declarator without `{` or `;`, and a body whose braces
    // close only at its end; a macro's use making several definitions shares one range of text)
    std::string_view declarator(text.data() + idEnd + 3, body - idEnd - 3);
    if (declarator.find_first_of("{;") != std::string_view::npos)
      continue;
    int depth = 0;
    bool closedEarly = false, opened = false;
    for (size_t k = body + bodyMark.size(); k < end; ++k)
      if (text[k] == '{')
      {
        ++depth;
        opened = true;
      }
      else if (text[k] == '}' && --depth == 0 && text.find_first_not_of(" \t\r\n", k + 1) < end)
        closedEarly = true;
    if (!opened || depth != 0 || closedEarly)
      continue;
    // (the definition's indentation: its mark stands before that, at the line's start, markInlineBody's declStart)
    size_t indentEnd = idEnd + 3;
    while (indentEnd < text.size() && (text[indentEnd] == ' ' || text[indentEnd] == '	'))
      ++indentEnd;
    bodies[id] = {text.substr(body + bodyMark.size(), end - body - bodyMark.size()), lineIndent(at) + text.substr(idEnd + 3, indentEnd - idEnd - 3)};
    definitionRanges[id] = {at, end + endMark.size()};
    size_t commentFrom = 0;
    comments[id] = commentAbove(at, commentFrom);
    commentStarts[id] = commentFrom;
  }
  // The declarations that take them.
  std::set<std::string> moved;
  std::string out;
  out.reserve(text.size());
  size_t from = 0;
  for (size_t at = text.find("/*kpp:inline:"); at != std::string::npos; at = text.find("/*kpp:inline:", at + 1))
  {
    size_t idEnd = text.find("*/", at);
    std::string id = text.substr(at + 13, idEnd - at - 13);
    size_t copied = from;
    out.append(text, from, at - from);
    from = idEnd + 2;
    auto body = bodies.find(id);
    if (body == bodies.end() || from >= text.size() || text[from] != ';' || inGroup(at))
      continue; // (the `;` stays)
    ++from; // (the `;` goes: the body follows the declarator)
    // The declaration's first line, whose indentation the body takes and above which its comment goes: the
    // mark is on the line of its `;`, which for a declaration over several lines is a continuation line
    // (`int padded(int left,` / `             int right = 1) const;`). Each `(` left open before a line's start
    // takes the search to its own line.
    size_t lineStart = text.rfind('\n', at);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    for (size_t k = at, open = 0; k > copied;)
    {
      --k;
      if (text[k] == ')')
        ++open;
      else if (text[k] == '(' && open > 0)
        --open;
      else if (text[k] == '\n' && open == 0)
        break;
      if (open > 0 && k < lineStart)
      {
        size_t previous = k == 0 ? std::string::npos : text.rfind('\n', k - 1);
        lineStart = previous == std::string::npos || previous < copied ? copied : previous + 1;
      }
    }
    std::string indent = lineIndent(lineStart);
    if (const std::vector<std::string>& comment = comments[id]; !comment.empty())
    {
      // (above the declaration's first line, after the comment it may have of its own)
      std::string lines;
      for (const std::string& line : comment)
        lines += indent + line + "\n";
      out.insert(out.size() - (at - lineStart), lines);
    }
    const std::string& defIndent = body->second.second;
    std::string moving;
    const std::string& b = body->second.first;
    for (size_t i = 0; i < b.size(); ++i)
    {
      moving += b[i];
      if (b[i] == '\n')
      {
        size_t skip = 0;
        while (skip < defIndent.size() && i + 1 + skip < b.size() && b[i + 1 + skip] == defIndent[skip])
          ++skip;
        i += skip;
        if (i + 1 < b.size() && b[i + 1] != '\n' && b[i + 1] != '\r')
          moving += indent;
      }
    }
    out += moving;
    moved.insert(id);
  }
  out.append(text, from, std::string::npos);
  text = std::move(out);
  // The declarations' parameters markInlineBody marked: the definition's names where its body moved,
  // their own where it stayed.
  for (size_t at = text.find("/*kpp:p:"); at != std::string::npos; at = text.find("/*kpp:p:", at))
  {
    size_t idEnd = text.find(':', at + 8), newEnd = text.find("*/", idEnd), close = text.find("/*kpp:p*/", newEnd);
    if (idEnd == std::string::npos || newEnd == std::string::npos || close == std::string::npos)
      break;
    std::string id = text.substr(at + 8, idEnd - at - 8);
    bool movedHere = moved.count(id) != 0;
    std::string written = movedHere ? text.substr(idEnd + 1, newEnd - idEnd - 1) : text.substr(newEnd + 2, close - newEnd - 2);
    if (movedHere)
      counts.add(written.empty() ? "parameters left unnamed as the definition leaves them, its body moved into the class"
                                 : "parameters named as the definition names them, its body moved into the class");
    text.replace(at, close + 9 - at, written);
    at += written.size();
  }
  // The moved definitions go (with the line they stood on and one blank line before them); the
  // others keep their text, the marks gone.
  for (const std::string& id : moved)
  {
    std::string start = "/*kpp:def:" + id + "{*/", endMark = "/*}kpp:def:" + id + "*/";
    size_t at = text.find(start), end = text.find(endMark);
    if (at == std::string::npos || end == std::string::npos)
      continue;
    end += endMark.size();
    size_t lineStart = text.rfind('\n', at);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    if (text.find_first_not_of(" \t", lineStart) == at)
    {
      at = lineStart;
      size_t commentFrom = at;
      if (!commentAbove(at, commentFrom).empty())
        at = commentFrom;
    }
    if (end < text.size() && text[end] == '\n')
      ++end;
    if (at >= 2 && text[at - 1] == '\n' && text[at - 2] == '\n' && (end >= text.size() || text[end] == '\n'))
      --at;
    text.erase(at, end - at);
    counts.add("member function bodies moved into their classes");
  }
  for (const char* mark : {"/*kpp:def:", "/*kpp:body:", "/*}kpp:def:"})
    for (size_t at = text.find(mark); at != std::string::npos; at = text.find(mark, at))
      text.erase(at, text.find("*/", at) + 2 - at);
  return text;
}

// The member functions whose bodies go inside their classes (kovarex, 2026-11-04: "convert the
// hpp+cpp contents in a way, that functions are inlined always"): a member declared in a class and
// defined outside it, in the same output file (X.hpp's class, X.cpp's `void X::f() { ... }`), whose
// definition's body the conversion writes at the declaration instead. Both get one id: the
// declaration's `;` is marked `/*kpp:inline:ID*/`, the definition `/*kpp:def:ID{*/`, its body's start
// `/*kpp:body:ID*/` and its end `/*}kpp:def:ID*/` (resolveInlineBodies). Out-of-class definitions stay
// where moving is not plain: a template (the class's or the member's), a specialization, a
// definition in another file, parameters named otherwise than the declaration names them.
// (InlineBodies: at the file's start)

static bool sameOutputFile(const ParsedFile* a, const ParsedFile* b)
{
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  std::filesystem::path pa(a->path), pb(b->path);
  return pa.parent_path() == pb.parent_path() && pa.stem() == pb.stem();
}

// A parameter type as written, for comparing a declaration's with its definition's (the resolver's
// match of the two is not taken alone: two overloads whose types are aliases it cannot tell apart,
// LogisticSupply's sumCounts of ItemWithQualityCountsDouble& and of ItemWithQualityCountsBig&).
static bool isTypeNode(const Node* n)
{
  switch (n->kind)
  {
    case NodeKind::NamedType: case NodeKind::BuiltinType: case NodeKind::PointerType: case NodeKind::LRefType:
    case NodeKind::RRefType: case NodeKind::ConstType: case NodeKind::ArrayType: case NodeKind::FunctionType:
    case NodeKind::DecltypeType: case NodeKind::AutoType: case NodeKind::MemberPointerType: case NodeKind::PackType:
      return true;
    default: return false;
  }
}

static std::string writtenType(const Type* t, const SymbolTable& symbols)
{
  if (!t)
    return "?";
  switch (t->kind)
  {
    case NodeKind::NamedType:
    {
      auto* n = static_cast<const NamedType*>(t);
      std::string out = n->name.global ? "::" : "";
      for (const NamePart& part : n->name.parts)
      {
        out += (part.symbol == UINT32_MAX ? std::string("?") : std::string(symbols.name(part.symbol))) + "::";
        if (part.hasTemplateArgs)
        {
          out += "<";
          for (Node* a : part.templateArgs)
            out += (a && isTypeNode(a) ? writtenType(static_cast<const Type*>(a), symbols) : std::string("expr")) + ",";
          out += ">";
        }
      }
      return out;
    }
    case NodeKind::BuiltinType:
    {
      auto* b = static_cast<const BuiltinType*>(t);
      return "builtin" + std::to_string(uint32_t(b->keyword)) + "/" + std::to_string(b->longCount) + (b->isUnsigned ? "u" : "") + (b->isSigned ? "s" : "");
    }
    case NodeKind::PointerType: return writtenType(static_cast<const PointerType*>(t)->pointee, symbols) + "*";
    case NodeKind::LRefType: return writtenType(static_cast<const LRefType*>(t)->referenced, symbols) + "&";
    case NodeKind::RRefType: return writtenType(static_cast<const RRefType*>(t)->referenced, symbols) + "&&";
    case NodeKind::ConstType: return "const " + writtenType(static_cast<const ConstType*>(t)->inner, symbols);
    case NodeKind::ArrayType: return writtenType(static_cast<const ArrayType*>(t)->element, symbols) + "[]";
    default: return "kind" + std::to_string(int(t->kind));
  }
}

// Whether alias names the class template instance base does, its template arguments written out to
// the template's defaults (`IDButton<IDType, void>` of `IDButton<IDType>`, its ElemType's default void).
static bool sameButDefaults(const Type* alias, const Type* base, const SymbolTable& symbols)
{
  if (!alias || !base || alias->kind != NodeKind::NamedType || base->kind != NodeKind::NamedType)
    return false;
  auto* a = static_cast<const NamedType*>(alias);
  auto* b = static_cast<const NamedType*>(base);
  if (a->name.empty() || a->name.parts.size() != b->name.parts.size() || a->name.global != b->name.global || !b->resolved ||
      b->resolved->kind != NodeKind::ClassDecl)
    return false;
  for (size_t i = 0; i + 1 < a->name.parts.size(); ++i)
    if (a->name.parts[i].symbol != b->name.parts[i].symbol || a->name.parts[i].hasTemplateArgs || b->name.parts[i].hasTemplateArgs)
      return false;
  const NamePart& al = a->name.last();
  const NamePart& bl = b->name.last();
  Span<Decl*> params = b->resolved->templateParams;
  if (al.symbol != bl.symbol || !al.hasTemplateArgs || al.templateArgs.size() <= bl.templateArgs.size() || al.templateArgs.size() > params.size())
    return false;
  auto written = [&](const Node* n) { return n && isTypeNode(n) ? writtenType(static_cast<const Type*>(n), symbols) : std::string(); };
  for (size_t i = 0; i < al.templateArgs.size(); ++i)
  {
    const Node* given = i < bl.templateArgs.size() ? bl.templateArgs[i] : static_cast<const TemplateParamDecl*>(params[i])->defaultArg;
    std::string x = written(al.templateArgs[i]), y = written(given);
    if (x.empty() || x != y)
      return false;
  }
  return true;
}

// Each class's own `using super = X;` (SuperAlias). X is the first base when it is written as the base's
// type is (`using super = gui::Frame<Style>;` of `: public gui::Frame<Style>`), or as its last name alone,
// in the class the injected class name of that base (`using super = LuaPrototypeTemplate;` of `: public
// LuaPrototypeTemplate<LuaAchievementPrototype, ...>`, `using super = HorizontalFlow;` of `: public
// agui::HorizontalFlow`), or with the template's default arguments written out (sameButDefaults). Anything
// else is another class (a second base, a grandparent).
static void collectSuperAliases(Span<Decl*> decls, const ConvertInput& in, const SymbolTable& symbols, SuperAliases& out, int depth = 0)
{
  if (depth > 64)
    return;
  const std::vector<Token>& toks = *in.tokens;
  auto spell = [&](uint32_t k) { return in.text.substr(toks[k].offset, toks[k].length); };
  for (Decl* d : decls)
  {
    if (d->kind == NodeKind::NamespaceDecl)
    {
      collectSuperAliases(static_cast<NamespaceDecl*>(d)->decls, in, symbols, out, depth + 1);
      continue;
    }
    if (d->kind != NodeKind::ClassDecl)
      continue;
    auto* c = static_cast<ClassDecl*>(d);
    collectSuperAliases(c->members, in, symbols, out, depth + 1);
    for (Decl* m : c->members)
    {
      if (m->kind != NodeKind::UsingAliasDecl || !m->templateParams.empty())
        continue;
      auto* a = static_cast<UsingAliasDecl*>(m);
      if (a->name == UINT32_MAX || symbols.name(a->name) != "super" || !a->type || a->type->token >= toks.size())
        continue;
      uint32_t from = a->type->token, semi = from;
      while (semi < toks.size() && !toks[semi].is(P::Semi))
        ++semi;
      if (semi >= toks.size() || semi == from)
        continue;
      SuperAlias alias;
      alias.written = std::string(in.text.substr(toks[from].offset, toks[semi - 1].offset + toks[semi - 1].length - toks[from].offset));
      alias.last = a->type->kind == NodeKind::NamedType && !static_cast<NamedType*>(a->type)->name.empty()
                     ? std::string(symbols.name(static_cast<NamedType*>(a->type)->name.last().symbol))
                     : alias.written;
      if (!c->bases.empty() && !c->bases[0].isPack && c->bases[0].type && c->bases[0].type->token < toks.size())
      {
        uint32_t base = c->bases[0].type->token, n = 0;
        while (from + n < semi && base + n < toks.size() && spell(from + n) == spell(base + n))
          ++n;
        bool baseEnds = base + n < toks.size() && (toks[base + n].is(P::Comma) || toks[base + n].is(P::LBrace));
        if (from + n == semi && baseEnds)
          alias.firstBase = true;
        else if (semi == from + 1 && toks[from].getKind() == TokenKind::Identifier && c->bases[0].type->kind == NodeKind::NamedType)
        {
          const QualifiedName& bn = static_cast<NamedType*>(c->bases[0].type)->name;
          alias.firstBase = !bn.empty() && symbols.name(bn.last().symbol) == spell(from) && (bn.last().hasTemplateArgs || bn.parts.size() > 1);
        }
        if (!alias.firstBase)
          alias.firstBase = sameButDefaults(a->type, c->bases[0].type, symbols);
      }
      out[c] = alias;
    }
  }
}

// Whether a declaration says [[noreturn]]: `[[noreturn]]` among its tokens before its name, back to the
// previous declaration's end.
static bool saysNoreturn(const Decl* d)
{
  if (!d->file || !d->file->tokens || d->token >= d->file->tokens->size())
    return false;
  const std::vector<Token>& toks = *d->file->tokens;
  for (uint32_t k = d->token; k > 0;)
  {
    --k;
    if (toks[k].is(P::Semi) || toks[k].is(P::LBrace) || toks[k].is(P::RBrace))
      return false;
    if (k >= 2 && toks[k].getKind() == TokenKind::Identifier && d->file->text.substr(toks[k].offset, toks[k].length) == "noreturn" &&
        toks[k - 1].is(P::LBracket) && toks[k - 2].is(P::LBracket))
      return true;
  }
  return false;
}

// The definitions of the functions a declaration of which says [[noreturn]] where the definition does
// not (LuaExceptionHelper.hpp's 15 functions, defined in its .cpp): the declaration of a namespace's
// function goes, and the definition takes the attribute (thread D's check of functions that do not
// return, which needs it on the function the program has).
static void collectNoreturn(Span<Decl*> decls, GlobalScope& global, InlineBodies& out)
{
  for (Decl* d : decls)
  {
    // (a namespace's functions alone: a member function's declaration stays in its class, saying it)
    if (d->kind == NodeKind::NamespaceDecl)
      collectNoreturn(static_cast<NamespaceDecl*>(d)->decls, global, out);
    if (d->kind != NodeKind::FunctionDecl || static_cast<FunctionDecl*>(d)->body || !saysNoreturn(d))
      continue;
    Decl* def = global.definitionOf(d);
    if (def && def != d && def->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(def)->body && !saysNoreturn(def))
      out.noreturn.insert(def);
  }
}

static void collectInlineBodies(Span<Decl*> decls, bool inTemplate, GlobalScope& global, const SymbolTable& symbols, InlineBodies& out)
{
  for (Decl* d : decls)
  {
    if (d->kind == NodeKind::NamespaceDecl)
      collectInlineBodies(static_cast<NamespaceDecl*>(d)->decls, inTemplate, global, symbols, out);
    if (d->kind != NodeKind::ClassDecl)
      continue;
    auto* c = static_cast<ClassDecl*>(d);
    if (!c->isDefinition)
      continue;
    bool classTemplate = inTemplate || !c->templateParams.empty();
    collectInlineBodies(c->members, classTemplate, global, symbols, out);
    for (Decl* m : c->members)
    {
      if (m->kind != NodeKind::FunctionDecl)
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      if (f->body || (f->flags & (DF_Friend | DF_Default | DF_Delete)))
        continue;
      Decl* defDecl = global.definitionOf(f);
      if (!defDecl || defDecl == f || defDecl->kind != NodeKind::FunctionDecl || !static_cast<FunctionDecl*>(defDecl)->body)
        continue;
      auto* def = static_cast<FunctionDecl*>(defDecl);
      const char* why = nullptr;
      if (classTemplate || !f->templateParams.empty() || !def->templateParams.empty())
        why = "a template's";
      else if (def->flags & DF_Specialization)
        why = "a specialization";
      else if (!sameOutputFile(f->file, def->file))
        why = "defined in another file";
      else if (f->params.size() != def->params.size())
        why = "parameters differ";
      else
        for (size_t i = 0; i < f->params.size() && !why; ++i)
        {
          auto* a = static_cast<ParamDecl*>(f->params[i]);
          auto* b = static_cast<ParamDecl*>(def->params[i]);
          const Type* at = a->type;
          const Type* bt = b->type;
          bool aConst = at && at->kind == NodeKind::ConstType, bConst = bt && bt->kind == NodeKind::ConstType;
          if (aConst)
            at = static_cast<const ConstType*>(at)->inner;
          if (bConst)
            bt = static_cast<const ConstType*>(bt)->inner;
          if (writtenType(at, symbols) != writtenType(bt, symbols))
            why = "parameter types written otherwise";
          else if (aConst && !bConst)
            why = "a parameter const in the declaration, not in the definition";
          else if (b->name != UINT32_MAX && a->name != b->name && !paramRenameable(a, symbols))
            why = a->name == UINT32_MAX ? "a parameter unnamed in the declaration" : "a parameter named otherwise in the declaration";
        }
      // (a declaration's parameters renamed as the definition names them: none may be a name its
      // default arguments use, a member's, which the parameter would hide there)
      if (!why && f->file && f->file->tokens && f->params.size() == def->params.size())
      {
        std::set<std::string_view> renamed;
        for (size_t i = 0; i < f->params.size(); ++i)
        {
          auto* a = static_cast<ParamDecl*>(f->params[i]);
          auto* b = static_cast<ParamDecl*>(def->params[i]);
          if (b->name != UINT32_MAX && a->name != b->name)
            renamed.insert(symbols.name(b->name));
        }
        const std::vector<Token>& ftoks = *f->file->tokens;
        for (size_t i = 0; i < f->params.size() && !renamed.empty() && !why; ++i)
        {
          auto* a = static_cast<ParamDecl*>(f->params[i]);
          if (!a->defaultValue)
            continue;
          for (uint32_t k = a->token, e = paramEndToken(a); k < e && k < ftoks.size() && !why; ++k)
            if (ftoks[k].getKind() == TokenKind::Identifier && renamed.count(f->file->text.substr(ftoks[k].offset, ftoks[k].length)))
              why = "a parameter named otherwise in the declaration, a name its default arguments use";
        }
      }
      if (!why && def->body && def->body->kind != NodeKind::BlockStmt)
        why = "a function-try-block";
      if (!why && def->file && def->file->tokens)
        for (uint32_t k = def->token; k < def->body->token && k < def->file->tokens->size(); ++k)
          if ((*def->file->tokens)[k].is(Kw::try_))
            why = "a function-try-block"; // (`X::X() try : a(b) { ... } catch (...) { ... }`)
      if (why)
      {
        ++out.kept[why];
        continue;
      }
      uint32_t id = uint32_t(out.declarations.size());
      out.declarations.emplace(f, id);
      out.definitions.emplace(def, id);
    }
  }
}

// The body of Factorio's `#define BITFIELD(Name, ...)` (Util/Container/BitField.hpp) among the inputs, its
// continuation lines joined and its spaces one each: the BITFIELD expansion writes the members this macro
// has, so a source of another version (kpp-2.1.24's, without operator^, ~ and ^=) gets its own (thread P,
// 2026-11-13: the members of master's hard-coded were 14 errors there). Empty when no input defines it.
static std::string collectBitfieldMacro(const std::vector<ConvertInput>& inputs)
{
  for (const ConvertInput& in : inputs)
  {
    std::string_view text = in.text;
    for (size_t at = text.find("BITFIELD("); at != std::string_view::npos; at = text.find("BITFIELD(", at + 1))
    {
      size_t lineStart = text.rfind('\n', at);
      lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
      std::string head(text.substr(lineStart, at - lineStart));
      head.erase(std::remove_if(head.begin(), head.end(), [](char c) { return isspace((unsigned char)c); }), head.end());
      if (head != "#define")
        continue;
      std::string body;
      size_t p = at;
      while (p < text.size())
      {
        size_t end = text.find('\n', p);
        if (end == std::string_view::npos)
          end = text.size();
        std::string_view line = text.substr(p, end - p);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
          line.remove_suffix(1);
        bool continued = !line.empty() && line.back() == '\\';
        if (continued)
          line.remove_suffix(1);
        body += std::string(line) + " ";
        p = end + 1;
        if (!continued)
          break;
      }
      std::string squeezed;
      for (char c : body)
        if (!isspace((unsigned char)c) || (!squeezed.empty() && squeezed.back() != ' '))
          squeezed += isspace((unsigned char)c) ? ' ' : c;
      return squeezed;
    }
  }
  return std::string();
}

// The classes Factorio's ENUM(Name, ...) and BITFIELD(Name, ...) define at namespace scope, by
// qualified name (`ns::Name`). The resolver sees the macro's use, not the class the converter writes
// for it, so a forward declaration of one (Entity.hpp's `class DrawingFlags;`) finds no definition;
// with this set it goes as any class's forward declaration does.
static std::set<std::string> collectMacroClasses(const std::vector<ConvertInput>& inputs)
{
  std::set<std::string> classes;
  for (const ConvertInput& in : inputs)
  {
    if (!in.parsed || in.parsed->isImported || !in.tokens)
      continue;
    const std::vector<Token>& toks = *in.tokens;
    auto spelling = [&](size_t k) { return in.text.substr(toks[k].offset, toks[k].length); };
    std::vector<std::pair<std::string, int>> namespaces; // a namespace's name ("" anonymous) and the brace depth inside it
    int depth = 0;
    for (size_t k = 0; k < toks.size(); ++k)
    {
      if (toks[k].is(P::LBrace))
        ++depth;
      else if (toks[k].is(P::RBrace))
      {
        if (!namespaces.empty() && namespaces.back().second == depth)
          namespaces.pop_back();
        --depth;
      }
      else if (toks[k].getKind() == TokenKind::Keyword && spelling(k) == "namespace")
      {
        // `namespace a::b {`, `namespace {`; an alias (`namespace fs = ...;`) opens nothing
        std::string name;
        size_t j = k + 1;
        while (j < toks.size() && (toks[j].getKind() == TokenKind::Identifier || toks[j].is(P::ColonColon)))
          name += spelling(j++);
        if (j < toks.size() && toks[j].is(P::LBrace))
        {
          namespaces.push_back({name, depth + 1});
          ++depth;
          k = j;
        }
      }
      else if (toks[k].getKind() == TokenKind::Identifier && (spelling(k) == "ENUM" || spelling(k) == "BITFIELD") && k + 2 < toks.size() &&
               toks[k + 1].is(P::LParen) && toks[k + 2].getKind() == TokenKind::Identifier &&
               depth == (namespaces.empty() ? 0 : namespaces.back().second))
      {
        std::string qualified;
        for (const auto& [name, inside] : namespaces)
          if (!name.empty())
            qualified += name + "::";
        classes.insert(qualified + std::string(spelling(k + 2)));
      }
    }
  }
  return classes;
}

static std::vector<std::pair<std::string, std::filesystem::path>> convertLibraries;
void setConvertLibraries(std::vector<std::pair<std::string, std::filesystem::path>> libraries) { convertLibraries = std::move(libraries); }

// The line a converted interface header's file starts with.
static const char* const scopeInterfaceLine =
  "// The library's interface (interface.txt): its users write its names qualified (design.md 16).";
// The line after it that lists the interface's functions, whose declarations the conversion removes
// (they are defined in the library's files), continued on `//  ` lines, the names comma-separated.
static const char* const scopeFunctionsLine = "// Its functions, defined in the library's files:";

// A scoped library's names, as its users write them qualified (design.md 16): the top-level
// declarations of its interface files under dir (scopeInterfaceLine: what C's users include, zstd.h,
// not its internal mem.h's BYTE, which Windows' headers declare too) that are not private
// (functions, classes, enumerations and their enumerators, aliases, variables and constants),
// parsed as kpp parses them.
static std::set<std::string> scopedNamesOf(const std::filesystem::path& dir, std::set<std::string>& headers)
{
  std::set<std::string> names;
  std::error_code ec;
  SymbolTable symbols;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".kpp")
      continue;
    std::string text;
    if (!readFile(entry.path(), text) || text.find(scopeInterfaceLine) == std::string::npos)
      continue;
    // (its C header's name: zlib.kpp is zlib.h's, stb_image.kpp stb_image.hpp's)
    headers.insert(entry.path().stem().string() + ".h");
    headers.insert(entry.path().stem().string() + ".hpp");
    if (size_t list = text.find(scopeFunctionsLine); list != std::string::npos)
    {
      size_t at = list + strlen(scopeFunctionsLine);
      for (;;)
      {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        for (size_t k = 0; k < line.size();)
          if (isIdentChar(line[k]))
          {
            size_t e = k;
            while (e < line.size() && isIdentChar(line[e]))
              ++e;
            names.insert(line.substr(k, e - k));
            k = e;
          }
          else
            ++k;
        if (end == std::string::npos || text.compare(end + 1, 4, "//  ") != 0)
          break;
        at = end + 5;
      }
    }
    LexResult lexed = lex(text, symbols);
    ParsedFile parsed;
    parseFile(lexed.tokens, text, symbols, parsed, Dialect::Cpp);
    auto add = [&](const QualifiedName& name)
    {
      if (name.parts.size() == 1 && name.last().symbol != UINT32_MAX)
        names.insert(std::string(symbols.name(name.last().symbol)));
    };
    for (Decl* d : parsed.decls)
    {
      if (d->filePrivate)
        continue;
      if (d->kind == NodeKind::FunctionDecl)
        add(static_cast<FunctionDecl*>(d)->name);
      else if (d->kind == NodeKind::ClassDecl)
        add(static_cast<ClassDecl*>(d)->name);
      else if (d->kind == NodeKind::VariableDecl)
        add(static_cast<VariableDecl*>(d)->name);
      else if (d->kind == NodeKind::UsingAliasDecl)
        names.insert(std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name)));
      else if (d->kind == NodeKind::EnumDecl)
      {
        add(static_cast<EnumDecl*>(d)->name);
        if (!static_cast<EnumDecl*>(d)->isClass)
          for (Decl* e : static_cast<EnumDecl*>(d)->enumerators)
            names.insert(std::string(symbols.name(static_cast<EnumeratorDecl*>(e)->name)));
      }
    }
  }
  return names;
}

// The names of the standard library's global classes, unions, enums and aliases: a top-level
// declaration line (no indentation), after a template header on it if any, `struct Set`,
// `template<> struct AtomicWord<1> ...`, `using Byte = uint8_t;`.
std::set<std::string> libraryTypeNames(const std::filesystem::path& libraryDir)
{
  std::set<std::string> names;
  std::error_code ec;
  if (libraryDir.empty() || !std::filesystem::is_directory(libraryDir, ec))
    return names;
  auto word = [](std::string_view line, size_t& at)
  {
    while (at < line.size() && line[at] == ' ')
      ++at;
    size_t b = at;
    while (at < line.size() && (isalnum((unsigned char)line[at]) || line[at] == '_'))
      ++at;
    return line.substr(b, at - b);
  };
  for (const auto& entry : std::filesystem::recursive_directory_iterator(libraryDir, ec))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".kpp")
      continue;
    std::string text;
    if (!readFile(entry.path(), text))
      continue;
    for (size_t pos = 0; pos < text.size();)
    {
      size_t end = text.find('\n', pos);
      if (end == std::string::npos)
        end = text.size();
      std::string_view line(text.data() + pos, end - pos);
      pos = end + 1;
      if (line.empty() || !isalpha((unsigned char)line[0]))
        continue; // not at the top level
      size_t at = 0;
      if (line.rfind("template<", 0) == 0)
      {
        int depth = 0;
        for (; at < line.size(); ++at)
          if (line[at] == '<')
            ++depth;
          else if (line[at] == '>' && --depth == 0)
            break;
        ++at;
      }
      std::string_view first = word(line, at);
      if (first == "using")
      {
        std::string_view name = word(line, at);
        while (at < line.size() && line[at] == ' ')
          ++at;
        if (!name.empty() && name != "namespace" && at < line.size() && line[at] == '=')
          names.insert(std::string(name));
        continue;
      }
      if (first == "union" || first == "enum")
      {
        size_t next = at;
        std::string_view second = word(line, next);
        if (second == "class" || second == "struct" || second == "enum")
          at = next;
      }
      else if (first != "class" && first != "struct")
        continue;
      std::string_view name = word(line, at);
      if (!name.empty() && !isdigit((unsigned char)name[0]))
        names.insert(std::string(name));
    }
  }
  return names;
}

static int convertErrors = 0;
int convertErrorCount()
{
  return convertErrors;
}

