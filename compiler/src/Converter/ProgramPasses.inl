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
    bodies[id] = {text.substr(body + bodyMark.size(), end - body - bodyMark.size()), lineIndent(at)};
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
    out.append(text, from, at - from);
    from = idEnd + 2;
    auto body = bodies.find(id);
    if (body == bodies.end() || from >= text.size() || text[from] != ';' || inGroup(at))
      continue; // (the `;` stays)
    ++from; // (the `;` goes: the body follows the declarator)
    std::string indent = lineIndent(at);
    if (const std::vector<std::string>& comment = comments[id]; !comment.empty())
    {
      // (above the declaration's line, after the comment it may have of its own)
      size_t lineStart = text.rfind('\n', at);
      lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
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
          else if (b->name != UINT32_MAX && a->name != b->name)
            why = a->name == UINT32_MAX ? "a parameter unnamed in the declaration" : "a parameter named otherwise in the declaration";
        }
      if (!why && def->body && def->body->kind != NodeKind::BlockStmt)
        why = "a function-try-block";
      if (!why && def->file && def->file->tokens)
        for (uint32_t k = def->token; k < def->body->token && k < def->file->tokens->size(); ++k)
          if ((*def->file->tokens)[k].is(Kw::try_))
            why = "a function-try-block"; // (`X::X() try : a(b) { ... } catch (...) { ... }`)
      if (auto autos = out.autoVariables.find(def->file); !why && autos != out.autoVariables.end() && def->file->tokens)
        for (uint32_t k = def->body->token; k < def->endToken && k < def->file->tokens->size() && !why; ++k)
          if ((*def->file->tokens)[k].getKind() == TokenKind::Identifier && autos->second.count((*def->file->tokens)[k].id))
            why = "using a namespace-scope auto variable of its file";
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
// parsed as kppc parses them.
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
    parseFile(lexed.tokens, text, symbols, parsed);
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

