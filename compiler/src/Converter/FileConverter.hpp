// FileConverter.hpp: FileConverter, the conversion of one file.
// Part of convert.cpp, included where it stood in its anonymous namespace (a file of its own since 2026-11-05, moved without a change).

  struct OverrideFile
  {
    std::string text;
    OverrideKeys keys;
    std::map<std::string, MemberOverride> memberClasses;
    std::string appended; // text without the member classes, which go into the converted classes
  };

  // Whether a program's global declaration only redeclares a scoped library's name (design.md 16): a
  // class declared, not defined (`struct unz_file_info64_s;`), or an alias (`using unzFile = void*;`,
  // `using unz_file_info64 = unz_file_info64_s;`) of a name the library has, as C++ code writes them so
  // as not to include the library's header. They are the library's own (C++ allows a typedef again
  // only of the same type), so they go and the uses are qualified (FileConverter::convertDecls).
  bool redeclaresScoped(Decl* d, const SymbolTable& symbols, const std::map<std::string, std::string>& names)
  {
    std::string name;
    if (d->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(d)->isDefinition && static_cast<ClassDecl*>(d)->name.parts.size() == 1 &&
        static_cast<ClassDecl*>(d)->name.last().symbol != UINT32_MAX && d->templateParams.empty())
      name = symbols.name(static_cast<ClassDecl*>(d)->name.last().symbol);
    else if (d->kind == NodeKind::UsingAliasDecl && d->templateParams.empty())
      name = symbols.name(static_cast<UsingAliasDecl*>(d)->name);
    else if (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef) && static_cast<VariableDecl*>(d)->name.parts.size() == 1 &&
             static_cast<VariableDecl*>(d)->name.last().symbol != UINT32_MAX)
      name = symbols.name(static_cast<VariableDecl*>(d)->name.last().symbol);
    return !name.empty() && names.count(name);
  }

  // Whether a class has an initializer-list constructor (C++'s: its first parameter a
  // std::initializer_list, by value or reference, the others defaulted): kpp's braces choose it
  // first (design.md: list-initialization), so `T x(3, 4)` is not `T x{3, 4}` for it. A list later
  // among the parameters (UnitTest++'s SuiteModpackAdder(Suite*, initializer_list)) is no such one.
  bool hasListConstructor(const ClassDecl* cls, const SymbolTable& symbols)
  {
    for (Decl* m : cls->members)
    {
      if (m->kind != NodeKind::FunctionDecl || static_cast<FunctionDecl*>(m)->params.empty())
        continue;
      Span<Decl*> params = static_cast<FunctionDecl*>(m)->params;
      bool othersDefaulted = true;
      for (size_t k = 1; k < params.size(); ++k)
        othersDefaulted = othersDefaulted && params[k]->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(params[k])->defaultValue;
      Type* t = params[0]->kind == NodeKind::ParamDecl ? static_cast<ParamDecl*>(params[0])->type : nullptr;
      while (t && (t->kind == NodeKind::ConstType || t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType))
        t = t->kind == NodeKind::ConstType  ? static_cast<ConstType*>(t)->inner
            : t->kind == NodeKind::LRefType ? static_cast<LRefType*>(t)->referenced
                                            : static_cast<RRefType*>(t)->referenced;
      if (othersDefaulted && t && t->kind == NodeKind::NamedType)
        for (const NamePart& part : static_cast<NamedType*>(t)->name.parts)
          if (part.symbol != UINT32_MAX && symbols.name(part.symbol) == "initializer_list")
            return true;
    }
    return false;
  }

  // The classes a program's types name, by their qualified names, and the last names of the types the resolver
  // left unresolved (convertToKpp; a declaration of a class defined nowhere goes when neither names it).
  struct ProgramNames
  {
    std::set<std::string> classes;
    std::set<uint32_t> unresolved;
  };

  struct FileConverter
  {
    bool isHeader() const
    {
      std::string ext = in.path.extension().string();
      return ext != ".cpp" && ext != ".cc" && ext != ".c";
    }
    // A std::variant declared in this file, which becomes a union enum (run() writes it once the
    // edits inside its alternatives' types are known).
    struct VariantDeclaration
    {
      const VariantInfo* info;
      uint32_t begin, end;                             // the alias declaration, or the member's type
      bool member;                                     // a member: the union goes before it, its type becomes the union
      uint32_t memberStart;                            // where the member's declaration starts (its line)
      std::vector<std::pair<uint32_t, uint32_t>> args; // the alternatives' types, as offsets
    };
    // std::get<T>(x), std::holds_alternative<T>(x), std::get_if<T>(p), std::visit(f, x) on a variant.
    struct VariantUse
    {
      std::string kind;
      uint32_t begin, end, calleeToken;
      std::vector<std::pair<uint32_t, uint32_t>> args; // the call's arguments, as offsets
      std::string typeSpelling;                        // T, normalized
      uint32_t typeBegin = 0, typeEnd = 0;             // T, as offsets
      bool noAlternative = false;                      // T is no variant's alternative (a template's parameter)
    };
    // This file's namespaces and classes (and the class scope of out-of-line member definitions),
    // by text range: where a name would find something before the global library name.
    struct ScopeRange
    {
      uint32_t begin, end;
      std::string key; // as ScopeNames has it
      bool classBody = false; // a class's definition (not a namespace, not an out-of-line member's scope)
    };
    void collectScopeRanges(Span<Decl*> decls, const std::string& prefix)
    {
      for (Decl* d : decls)
      {
        if (d->endToken <= d->token || d->endToken > toks.size())
          continue;
        uint32_t begin = toks[d->token].offset, end = endOffset(d->endToken - 1);
        if (d->kind == NodeKind::NamespaceDecl)
        {
          auto* n = static_cast<NamespaceDecl*>(d);
          std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
          scopeRanges.push_back({begin, end, prefix + name + "::"});
          collectScopeRanges(n->decls, prefix + name + "::");
        }
        else if (d->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(d)->isDefinition && !static_cast<ClassDecl*>(d)->name.empty())
        {
          std::string key = prefix + nameText(static_cast<ClassDecl*>(d)->name, symbols) + "::";
          scopeRanges.push_back({begin, end, key, true});
          collectScopeRanges(static_cast<ClassDecl*>(d)->members, key);
        }
        else if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->name.parts.size() > 1)
        {
          // `void NoiseExpressions::String::f(...) {...}`: the parameters and body see the class.
          QualifiedName qualifier = static_cast<FunctionDecl*>(d)->name;
          qualifier.parts.count -= 1;
          std::string scope = prefix;
          for (size_t i = 0; i < qualifier.parts.size(); ++i)
          {
            if (qualifier.parts[i].symbol == UINT32_MAX)
              continue; // the leading `::`
            scope += std::string(symbols.name(qualifier.parts[i].symbol)) + "::";
            scopeRanges.push_back({begin, end, scope});
          }
        }
      }
    }
    // The file's using-directives of the program's namespaces, by text range: from each to the end
    // of the block or namespace it is written in, the names of the namespace it names are found as
    // if declared in the global namespace, beside the library's. So a library name the converter
    // writes there is ambiguous unless qualified (TestMigrations' `std::set<std::string>` under
    // `using namespace TestMigrationHelpers;`, whose own Set class template it would find: `::Set`),
    // and so is the namespace's own name of the library's (that Set, written `Set`:
    // `TestMigrationHelpers::Set`). A directive outside any block of a source file (not a header)
    // goes, its namespace's names written qualified instead: C++ limits it to its translation
    // unit, kpp has none, and the global namespace it is in is every file's (DeferredTestReporter's
    // `using namespace UnitTest;` put UnitTest's TestReporter beside the tests' own everywhere).
    struct DirectiveRange
    {
      uint32_t begin, end;  // the directive, to the end of its block (or of the file)
      uint32_t lineEnd;     // the end of the directive's line (a removed one goes with it)
      std::string key;      // the namespace's ScopeNames key
      bool removed = false; // a source file's directive outside any block
    };
    void collectDirectiveRanges()
    {
      if (!scopeNames)
        return;
      std::vector<std::vector<size_t>> pending(1); // per open block: the directives written directly in it
      for (uint32_t i = 0; i < toks.size(); ++i)
      {
        if (toks[i].is(P::LBrace))
          pending.emplace_back();
        else if (toks[i].is(P::RBrace) && pending.size() > 1)
        {
          for (size_t d : pending.back())
            directiveRanges[d].end = toks[i].offset;
          pending.pop_back();
        }
        else if (toks[i].is(Kw::using_) && i + 2 < toks.size() && toks[i + 1].is(Kw::namespace_))
        {
          std::string name;
          uint32_t j = i + 2;
          for (; j < toks.size() && !toks[j].is(P::Semi); ++j)
            name += spelling(j);
          if (name.empty() || name == "std" || name.compare(0, 5, "std::") == 0 || j == toks.size())
            continue;
          if (name.compare(0, 2, "::") == 0)
            name = name.substr(2);
          // A name written in a namespace may be one of that namespace's own (or an enclosing
          // one's): the innermost that the program declares names in.
          std::vector<std::string> keys;
          for (const ScopeRange& sr : scopeRanges)
            if (!sr.classBody && sr.begin <= toks[i].offset && toks[i].offset < sr.end)
              keys.push_back(sr.key + name + "::");
          std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
          keys.push_back(name + "::");
          for (const std::string& key : keys)
            if (scopeNames->count(key))
            {
              pending.back().push_back(directiveRanges.size());
              directiveRanges.push_back({toks[i].offset, UINT32_MAX, lineEndAfter(endOffset(j)), key, pending.size() == 1 && !isHeader()});
              break;
            }
          i = j;
        }
      }
    }
    // Whether the program declares a class, alias or variable of the name in the global namespace.
    bool globalNamed(const std::string& name) const
    {
      auto it = scopeNames->find("(global)");
      return it != scopeNames->end() && it->second.count(name);
    }
    // The namespace (its ScopeNames key) through whose using-directive an unqualified `name`
    // written at offset finds a declaration, and the directive, or none.
    const DirectiveRange* directiveFinding(const std::string& name, uint32_t offset) const
    {
      for (const DirectiveRange& r : directiveRanges)
        if (r.begin <= offset && offset < r.end)
          if (auto it = scopeNames->find(r.key); it != scopeNames->end() && it->second.count(name))
            return &r;
      return nullptr;
    }
    // Whether an unqualified `name` written at offset finds a declaration of the program before a
    // global one: a local or parameter of the function around it declared before, or a member of
    // a namespace or class around it (an out-of-line member's class, `void
    // DeferredTestReporter::f()` with that class UnitTest's through a directive, among them).
    bool declaredCloser(const std::string& name, uint32_t offset) const
    {
      if (auto l = localNames.find(name); l != localNames.end())
        for (auto [declared, end] : l->second)
          if (declared <= offset && offset < end)
            return true;
      if (!scopeNames)
        return false;
      for (const ScopeRange& r : scopeRanges)
        if (r.begin <= offset && offset < r.end)
        {
          auto it = scopeNames->find(r.key);
          if (it != scopeNames->end() && it->second.count(name))
            return true;
          // (Not the class's own name, which its qualified declarator name is written with: the
          // class it names is the directive's namespace's either way.)
          if (it == scopeNames->end() && r.key.size() > name.size() + 2 && r.key.compare(r.key.size() - name.size() - 2, name.size(), name) != 0)
            for (const DirectiveRange& d : directiveRanges)
              if (d.begin <= offset && offset < d.end)
                if (auto via = scopeNames->find(d.key + r.key); via != scopeNames->end() && via->second.count(name))
                  return true;
        }
      return false;
    }
    // Whether an unqualified `name` written at offset finds a declaration of the program before the
    // global (library) one: a namespace's or class's member, a member of a namespace a
    // using-directive names (as ambiguous: the library's needs `::` as well), or a local or
    // parameter of the function around it declared before (`Layer::const_iterator next = std::next(current);`, whose
    // initializer sees the variable being declared).
    bool shadowedAt(const std::string& name, uint32_t offset) const
    {
      return declaredCloser(name, offset) || (scopeNames && directiveFinding(name, offset));
    }
    // The function definitions of the file: where each starts and ends, and its qualified name.
    struct FunctionRange
    {
      uint32_t begin, end;
      std::string name;
    };
    void collectLocals(Span<Decl*> decls, const std::string& prefix = "")
    {
      for (Decl* d : decls)
        if (d->kind == NodeKind::NamespaceDecl)
        {
          auto* n = static_cast<NamespaceDecl*>(d);
          std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
          collectLocals(n->decls, name.empty() ? prefix : prefix + name + "::");
        }
        else if (d->kind == NodeKind::ClassDecl)
        {
          auto* c = static_cast<ClassDecl*>(d);
          collectLocals(c->members, c->name.empty() ? prefix : prefix + nameText(c->name, symbols) + "::");
        }
        else if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body && d->endToken > 0 && d->endToken <= toks.size())
        {
          auto* f = static_cast<FunctionDecl*>(d);
          uint32_t end = endOffset(d->endToken - 1);
          functionRanges.push_back({toks[d->token].offset, end, prefix + nameText(f->name, symbols)});
          for (Decl* p : f->params)
            if (auto* pd = static_cast<ParamDecl*>(p); pd->name != UINT32_MAX)
              localNames[std::string(symbols.name(pd->name))].push_back({toks[p->token].offset, end});
          collectLocalsIn(f->body, end);
        }
    }
    // A declaration among a statement's parts (`if (auto* x = ...)`, a range-for's variable).
    void collectLocal(Node* n, uint32_t end)
    {
      if (n && n->kind == NodeKind::VariableDecl)
      {
        auto* v = static_cast<VariableDecl*>(n);
        if (v->name.parts.size() == 1 && v->name.parts[0].symbol != UINT32_MAX)
          localNames[std::string(symbols.name(v->name.parts[0].symbol))].push_back({toks[v->token].offset, end});
        // A local named `_`, which the parser makes unnamed (`_<offset>`), as kpp does.
        for (uint32_t k = v->token; k < v->endToken && k < toks.size() && !toks[k].is(P::Semi); ++k)
          if (toks[k].getKind() == TokenKind::Identifier && spelling(k) == "_")
          {
            underscoreLocals.push_back({toks[v->token].offset, end, k});
            break;
          }
      }
    }
    // The locals named `_` (where each declaration starts, where its function ends, its name's
    // token): in kpp `_` is an unnamed variable, so one the code reads after all gets a name.
    struct UnderscoreLocal
    {
      uint32_t declared, end, nameToken;
    };
    // `Filesystem::ErrorCode _; Filesystem::RemoveAll(path, _);` (WriteFileGuard): the `_` the call
    // reads is the latest `_` declared before it in the function; both become `ignored`.
    bool nameUnderscoreUse(uint32_t token)
    {
      uint32_t at = toks[token].offset;
      const UnderscoreLocal* found = nullptr;
      for (const UnderscoreLocal& u : underscoreLocals)
        if (u.declared < at && at < u.end && toks[u.nameToken].offset < at && (!found || u.declared > found->declared))
          found = &u;
      if (!found)
        return false;
      replace(at, toks[token].length, "ignored");
      if (namedUnderscores.insert(found->nameToken).second)
        replace(toks[found->nameToken].offset, toks[found->nameToken].length, "ignored");
      counts.add("read `_` locals named");
      return true;
    }
    void collectLocalsIn(Stmt* s, uint32_t end)
    {
      if (!s)
        return;
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            collectLocalsIn(inner, end);
          break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
            collectLocal(d, end);
          break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          collectLocalsIn(i->init, end);
          collectLocal(i->cond, end);
          collectLocalsIn(i->then, end);
          collectLocalsIn(i->else_, end);
          break;
        }
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          collectLocalsIn(f->init, end);
          collectLocal(f->cond, end);
          collectLocalsIn(f->body, end);
          break;
        }
        case NodeKind::RangeForStmt:
        {
          auto* f = static_cast<RangeForStmt*>(s);
          collectLocal(f->var, end);
          collectLocalsIn(f->body, end);
          break;
        }
        case NodeKind::WhileStmt:
          collectLocal(static_cast<WhileStmt*>(s)->cond, end);
          collectLocalsIn(static_cast<WhileStmt*>(s)->body, end);
          break;
        case NodeKind::DoStmt: collectLocalsIn(static_cast<DoStmt*>(s)->body, end); break;
        case NodeKind::SwitchStmt:
        {
          auto* sw = static_cast<SwitchStmt*>(s);
          collectLocalsIn(sw->init, end);
          collectLocal(sw->cond, end);
          collectLocalsIn(sw->body, end);
          break;
        }
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(s);
          collectLocalsIn(t->body, end);
          for (const TryStmt::Handler& h : t->handlers)
            collectLocalsIn(h.body, end);
          break;
        }
        default: break;
      }
    }
    struct LocalMacro
    {
      std::vector<std::string> params;
      std::string body;
      uint32_t from = 0, to = UINT32_MAX; // the text where this definition holds: from its #define to an #undef or the next #define
    };
    // The game's release_assert (Util/ReleaseAssert.hpp), `do { if (!(value)) F(__FILE__, __LINE__,
    // #value); } while (false)`: kpp's own release_assert statement, which takes the file, the line
    // and the expression's text itself (design.md section 2), so its uses stay as written and are no
    // macro expansions. Its definition becomes the hook a failure calls, kpp_assert_failed, calling
    // F as the macro did. Returns F, or nothing when the macro is not of that form (then it is
    // expanded as any other).
    static std::optional<std::string> kppReleaseAssert(const std::string& name, const std::vector<std::string>& params, const std::string& body)
    {
      if (name != "release_assert" || params.size() != 1)
        return std::nullopt;
      std::string text;
      for (char c : body)
        if (!isspace((unsigned char)c) && c != '\\')
          text += c;
      const std::string& value = params[0];
      std::string head = "do{if(!(" + value + "))", tail = "(__FILE__,__LINE__,#" + value + ");}while(";
      if (text.compare(0, head.size(), head) != 0)
        return std::nullopt;
      size_t call = text.find('(', head.size());
      if (call == std::string::npos || call == head.size())
        return std::nullopt;
      std::string failure = text.substr(head.size(), call - head.size());
      for (char c : failure)
        if (!isIdentChar(c) && c != ':')
          return std::nullopt;
      std::string rest = text.substr(call);
      if (rest != tail + "false)" && rest != tail + "0)")
        return std::nullopt;
      return failure;
    }
    bool isScopeHeader() const { return in.c && cScopeHeaders && cScopeHeaders->count(in.path.filename().string()); }
    bool redeclaresScopedName(Decl* d) const
    {
      return scopedLibraryNames && !scopedLibraryNames->empty() && redeclaresScoped(d, symbols, *scopedLibraryNames);
    }
    // A macro whose uses in the code write its name: this file's constant, or one an interface header
    // made. (Inside another macro's expansion an interface constant is its value, as the preprocessor
    // rescans: ZSTD_VERSION_STRING stringizes ZSTD_LIB_VERSION's numbers, "1.4.0".)
    bool cNamedConstant(const std::string& name)
    {
      return cOwnConstant(name) || (cScopeConstants && cScopeConstants->count(name) && !isScopeHeader());
    }
    struct SuiteBlock
    {
      std::string name;
      uint32_t open = 0, close = 0;
    };
    // The `}` token closing the `{` token open (0: none).
    uint32_t closingBrace(uint32_t open) const
    {
      int depth = 0;
      for (uint32_t k = open; k < toks.size(); ++k)
        if (toks[k].is(P::LBrace))
          ++depth;
        else if (toks[k].is(P::RBrace) && --depth == 0)
          return k;
      return 0;
    }
    // Whether name is an iterator type's, whose member types (value_type, ...) the library's
    // IteratorTraits gives: an alias of an iterator (`using ListIterator = ...::const_iterator;`; a
    // Vector's iterator is a pointer, which has no members), or, naming no class or alias of the
    // converted files, a template parameter named as std names iterators (RandomIt, InputIt, Iter).
    bool namesIteratorType(const std::string& name) const
    {
      if (classesByName)
        if (auto found = classesByName->find(name); found != classesByName->end() && !found->second.empty())
        {
          for (Decl* d : found->second)
          {
            Type* aliased = d->kind == NodeKind::UsingAliasDecl ? static_cast<UsingAliasDecl*>(d)->type : nullptr;
            if (!aliased || aliased->kind != NodeKind::NamedType || static_cast<NamedType*>(aliased)->name.empty() ||
                static_cast<NamedType*>(aliased)->name.last().symbol == UINT32_MAX)
              return false;
            std::string last(symbols.name(static_cast<NamedType*>(aliased)->name.last().symbol));
            if (last.size() < 8 || last.compare(last.size() - 8, 8, "iterator") != 0)
              return false;
          }
          return true;
        }
      auto endsWith = [&](std::string_view suffix) { return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0; };
      return endsWith("It") || endsWith("Iter") || endsWith("Iterator");
    }
    // A fixture test's body (UnitTest++'s TEST_FIXTURE, whose expansion ends with the
    // `Fixture##Name##Helper::Run()` of a class deriving from the fixture): the parse does not see it
    // (a macro's use with a block), so the names it uses of the fixture's members get their `this->`
    // here, which kpp requires (TestArithmeticCombinator's `combinator->setParameters(params);`). From
    // its `{` token to its `}`, the fixture's member names, and the locals declared in it (a name, the
    // token declaring it, the end of its block), which hide members of their names.
    struct FixtureBody
    {
      uint32_t open = 0, close = 0;
      std::set<std::string> members;
      std::vector<std::tuple<std::string, uint32_t, uint32_t>> locals;
    };
    const FixtureBody* fixtureBodyAt(uint32_t token) const
    {
      for (const FixtureBody& b : fixtureBodies)
        if (b.open < token && token < b.close)
          return &b;
      return nullptr;
    }
    static bool isFixtureLocal(const FixtureBody& b, const std::string& name, uint32_t token)
    {
      for (const auto& [local, declared, end] : b.locals)
        if (local == name && declared <= token && token < end)
          return true;
      return false;
    }
    // The body of the fixture test whose expansion is text and whose body begins at token open:
    // its fixture's members (through an alias, and the bases'), and its locals by their declarations.
    void noteFixtureBody(const std::string& expansion, uint32_t open)
    {
      std::string text = expansion;
      while (!text.empty() && isspace((unsigned char)text.back()))
        text.pop_back();
      // (--convert-native-tests: `test Name : Fixture`, `test Name : Fixture(Parameter)`)
      if (classesByName && open < toks.size() && toks[open].is(P::LBrace) && text.rfind("test ", 0) == 0 && text.find(" : ") != std::string::npos)
      {
        std::string fixture = text.substr(text.find(" : ") + 3);
        fixture = fixture.substr(0, fixture.find('('));
        while (!fixture.empty() && isspace((unsigned char)fixture.back()))
          fixture.pop_back();
        noteFixtureBodyOf(fixture.substr(fixture.rfind(':') == std::string::npos ? 0 : fixture.rfind(':') + 1), open);
        return;
      }
      if (!classesByName || open >= toks.size() || !toks[open].is(P::LBrace) || text.size() < 8 || text.compare(text.size() - 7, 7, "::Run()") != 0)
        return;
      size_t nameEnd = text.size() - 7, nameStart = nameEnd;
      while (nameStart > 0 && isIdentChar(text[nameStart - 1]))
        --nameStart;
      std::string helper = text.substr(nameStart, nameEnd - nameStart);
      size_t at = text.find("class " + helper + " : public ");
      if (helper.empty() || at == std::string::npos)
        return;
      size_t baseStart = at + helper.size() + 16, baseEnd = baseStart;
      while (baseEnd < text.size() && text[baseEnd] != ',' && text[baseEnd] != '{')
        ++baseEnd;
      std::string base = text.substr(baseStart, baseEnd - baseStart);
      while (!base.empty() && isspace((unsigned char)base.back()))
        base.pop_back();
      size_t last = base.rfind(':');
      std::string fixture = last == std::string::npos ? base : base.substr(last + 1);
      while (!fixture.empty() && isspace((unsigned char)fixture.front()))
        fixture.erase(fixture.begin());
      noteFixtureBodyOf(fixture, open);
    }
    // The body from token open of a test of fixture class fixture (by its unqualified name).
    void noteFixtureBodyOf(const std::string& fixture, uint32_t open)
    {
      auto found = classesByName->find(fixture);
      if (found == classesByName->end() || found->second.empty())
        return;
      Decl* cls = found->second.front();
      if (cls->kind == NodeKind::UsingAliasDecl)
      {
        const Ty* aliased = global.typeOfAst(static_cast<UsingAliasDecl*>(cls)->type, cls);
        if (!aliased || !aliased->decl)
          return;
        cls = aliased->decl;
      }
      FixtureBody body;
      body.open = open;
      body.close = closingBrace(open);
      if (body.close == 0)
        return;
      // (A class's constructors are among its members by the class's name, which names a type here:
      // TestAchievements' `AchievementFixture fixture("look-at-my-shiny-rare-armor");`.)
      for (uint32_t symbol : memberNames(cls))
        if (std::string name(symbols.name(symbol)); !classesByName->count(name))
          body.members.insert(name);
      // The locals: a name after a type's last token (a name, `>`, `*`, `&`, `&&`, a type word) and
      // before what a declarator ends with, or the names of a structured binding; each until the end
      // of the block it is declared in.
      static const std::set<std::string_view> typeWords = {"auto", "bool", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "void", "const", "volatile"};
      std::vector<uint32_t> blocks; // the `{` of the blocks open at k
      auto blockEnd = [&](uint32_t from)
      {
        int d = 0;
        for (uint32_t k = from; k <= body.close; ++k)
          if (toks[k].is(P::LBrace))
            ++d;
          else if (toks[k].is(P::RBrace) && --d == 0)
            return k;
        return body.close;
      };
      for (uint32_t k = open; k < body.close; ++k)
      {
        if (toks[k].is(P::LBrace))
          blocks.push_back(k);
        else if (toks[k].is(P::RBrace) && !blocks.empty())
          blocks.pop_back();
        uint32_t end = blocks.empty() ? body.close : blockEnd(blocks.back());
        if (toks[k].is(Kw::auto_) || (toks[k].getKind() == TokenKind::Keyword && spelling(k) == "auto"))
        {
          uint32_t b = k + 1;
          while (b < body.close && (toks[b].is(P::Amp) || toks[b].is(P::AndAnd)))
            ++b;
          if (b < body.close && toks[b].is(P::LBracket))
            for (++b; b < body.close && !toks[b].is(P::RBracket); ++b)
              if (toks[b].getKind() == TokenKind::Identifier)
                body.locals.emplace_back(std::string(spelling(b)), b, end);
        }
        if (toks[k].getKind() != TokenKind::Identifier || k == 0)
          continue;
        const Token& before = toks[k - 1];
        const Token& after = toks[k + 1];
        bool typeBefore = before.getKind() == TokenKind::Identifier || before.is(P::Greater) || before.is(P::Star) || before.is(P::Amp) ||
                          before.is(P::AndAnd) || (before.getKind() == TokenKind::Keyword && typeWords.count(spelling(k - 1)));
        bool declaratorAfter = after.is(P::Assign) || after.is(P::Semi) || after.is(P::LParen) || after.is(P::LBrace) || after.is(P::LBracket) ||
                               after.is(P::Comma) || after.is(P::RParen) || after.is(P::Colon);
        bool qualified = k >= 2 && (toks[k - 2].is(P::Dot) || toks[k - 2].is(P::Arrow) || toks[k - 2].is(P::ColonColon));
        if (typeBefore && declaratorAfter && !(before.getKind() == TokenKind::Identifier && qualified))
          body.locals.emplace_back(std::string(spelling(k)), k, end);
      }
      counts.add("fixture test bodies");
      fixtureBodies.push_back(std::move(body));
    }
    void notePack(const std::string& rest, uint32_t offset)
    {
      size_t p = rest.find("pack");
      if (p == std::string::npos || rest.find('(', p) == std::string::npos)
        return;
      std::string args = rest.substr(rest.find('(', p) + 1);
      args = args.substr(0, args.find(')'));
      std::vector<std::string> parts;
      for (size_t start = 0; start <= args.size();)
      {
        size_t comma = args.find(',', start);
        std::string part = args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t a = part.find_first_not_of(" \t"), b = part.find_last_not_of(" \t");
        if (a != std::string::npos)
          parts.push_back(part.substr(a, b - a + 1));
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
      int before = pack;
      if (!parts.empty() && parts[0] == "push")
      {
        packStack.push_back(pack);
        if (parts.size() > 1)
          pack = atoi(parts[1].c_str());
      }
      else if (!parts.empty() && parts[0] == "pop")
      {
        pack = packStack.empty() ? 0 : packStack.back();
        if (!packStack.empty())
          packStack.pop_back();
      }
      else
        pack = parts.empty() ? 0 : atoi(parts[0].c_str());
      if (pack != 0 && pack != 1 && pack != 8 && pack != 16)
        todo(offset, "#pragma pack(" + args + "): only a pack of 1 ([[packed]]) is written out");
      if (before != 1 && pack == 1)
        packedSince = offset;
      else if (before == 1 && pack != 1)
        packedRanges.push_back({packedSince, offset});
    }
    bool packedAt(uint32_t offset) const
    {
      for (auto [from, to] : packedRanges)
        if (from <= offset && offset < to)
          return true;
      return pack == 1 && packedSince <= offset; // an unclosed push lasts to the end of the file
    }
    void seedHeaderMacros()
    {
      if (headerMacros)
        for (const auto& [name, macro] : *headerMacros)
        {
          LocalMacro m;
          m.params = macro.first;
          m.body = macro.second;
          localMacros[name].push_back(m);
        }
      auto replaced = [&](const std::string& name, const std::string& body) -> const std::string&
      {
        if (convertNativeTests)
          if (auto n = nativeTestMacroBodies().find(name); n != nativeTestMacroBodies().end())
            return n->second;
        auto r = replacedMacroBodies.find(name);
        return r == replacedMacroBodies.end() ? body : r->second;
      };
      for (const auto* macros : includedMacros)
        for (const auto& [name, macro] : *macros)
        {
          // The macros the converter rewrites by name keep their rewrite (convertTokens writes
          // Util/Enums.hpp's ENUM as a readable class, ...); the first header to define a name gives
          // its definition.
          static const std::set<std::string> rewrittenByName = {"ENUM", "ENUM_IMPL", "ENUM_IMPL_NOSTRING", "BITFIELD", "MAKE_OPTIONAL"};
          if (knownMacros.count(name) || knownFunctionMacros.count(name) || argumentMacros.count(name) || rewrittenByName.count(name) ||
              localMacros.count(name) || kppReleaseAssert(name, macro.first, macro.second))
            continue;
          LocalMacro m;
          m.params = macro.first;
          m.body = replaced(name, macro.second);
          localMacros[name].push_back(m);
        }
    }
    const LocalMacro* localMacroAt(const std::string& name, uint32_t offset) const
    {
      auto it = localMacros.find(name);
      if (it != localMacros.end())
        for (const LocalMacro& m : it->second)
          if (m.from <= offset && offset < m.to)
            return &m;
      return nullptr;
    }
    void endLocalMacro(const std::string& name, uint32_t offset)
    {
      auto it = localMacros.find(name);
      if (it != localMacros.end() && !it->second.empty() && it->second.back().to == UINT32_MAX)
        it->second.back().to = offset;
    }
    // A use of an argument macro, expanded in run() once the edits inside its arguments are known.
    struct ArgumentMacroUse
    {
      uint32_t begin, end; // the use, from the name to the closing parenthesis
      std::vector<std::pair<uint32_t, uint32_t>> args; // offsets
      const std::string* replacement;
    };
    // A use of a local macro (this file's, or a header's it includes), expanded in run() once the
    // edits inside its arguments are known, as argument macros are: the arguments' tokens stay in
    // the stream, so the rewrites of code apply in them too (a renamed alias, a chrono literal,
    // this->: TestLatencyCarWhiteBox's `CHECK_EQUAL(State(...), ...)`, TestUDPSocket's
    // `CHECK(wait_for(50ms))`).
    struct LocalMacroUse
    {
      uint32_t begin, end; // the use, from the name to the closing parenthesis
      std::vector<std::pair<uint32_t, uint32_t>> args; // offsets; an empty argument is an empty range
      const LocalMacro* macro;
    };
    void importC(const std::string& header)
    {
      if (std::find(cImports.begin(), cImports.end(), header) == cImports.end())
        cImports.push_back(header);
    }

    FileConverter(const ConvertInput& in, GlobalScope& global, const SymbolTable& symbols)
      : in(in), global(global), symbols(symbols), text(in.text), toks(*in.tokens)
    {
    }

    uint32_t lineOf(uint32_t offset) const
    {
      uint32_t line = 1;
      for (uint32_t i = 0; i < offset && i < text.size(); ++i)
        if (text[i] == '\n')
          ++line;
      return line;
    }

    void todo(uint32_t offset, const std::string& what)
    {
      todos.push_back(std::to_string(lineOf(offset)) + ": " + what);
      counts.add("todo");
    }

    void remove(uint32_t offset, uint32_t length) { edits.push_back({offset, length, ""}); }
    void insert(uint32_t offset, const std::string& s) { edits.push_back({offset, 0, s}); }
    void replace(uint32_t offset, uint32_t length, const std::string& s) { edits.push_back({offset, length, s}); }

    // s with __FILE__ and __LINE__ replaced by this file's name and the line of offset.
    std::string withFileAndLine(std::string s, uint32_t offset) const
    {
      for (size_t p = s.find("__FILE__"); p != std::string::npos; p = s.find("__FILE__", p))
      {
        std::string literal = "\"" + in.path.filename().string() + "\"";
        s.replace(p, 8, literal);
        p += literal.size();
      }
      for (size_t p = s.find("__LINE__"); p != std::string::npos; p = s.find("__LINE__", p))
      {
        std::string line = std::to_string(1 + std::count(text.begin(), text.begin() + std::min<size_t>(offset, text.size()), '\n'));
        s.replace(p, 8, line);
        p += line.size();
      }
      // __FUNCTION__ and __func__: the name of the function around the use, qualified as MSVC's
      // __FUNCTION__ is (`FramebufferOpenGL::generateFramebufferObject`; a template's without its
      // arguments). Outside a function they stay, and the compiler reports them.
      for (const char* macro : {"__FUNCTION__", "__func__"})
        for (size_t p = s.find(macro); p != std::string::npos; p = s.find(macro, p))
        {
          size_t length = strlen(macro);
          bool word = (p == 0 || !isIdentChar(s[p - 1])) && (p + length >= s.size() || !isIdentChar(s[p + length]));
          const std::string* name = functionNameAt(offset);
          if (!word || !name)
          {
            p += length;
            continue;
          }
          std::string literal = "\"" + *name + "\"";
          s.replace(p, length, literal);
          p += literal.size();
        }
      return s;
    }
    // The qualified name of the innermost function whose definition holds offset, or null.
    const std::string* functionNameAt(uint32_t offset) const
    {
      const FunctionRange* best = nullptr;
      for (const FunctionRange& f : functionRanges)
        if (f.begin <= offset && offset < f.end && (!best || f.begin >= best->begin))
          best = &f;
      return best ? &best->name : nullptr;
    }

    // Whether token is a keyword that a parenthesis after it belongs to, which makes `(NAME)` no
    // cast: a functional cast to a builtin type (`float(M_PI) * x`), sizeof, alignof, decltype, noexcept.
    bool callsWithParenthesis(uint32_t token) const
    {
      static const std::set<std::string_view> keywords = {"bool", "char", "char8_t", "char16_t", "char32_t", "wchar_t", "short", "int", "long", "signed",
                                                          "unsigned", "float", "double", "sizeof", "alignof", "decltype", "noexcept"};
      return toks[token].getKind() == TokenKind::Keyword && keywords.count(this->spelling(token));
    }
    std::string_view spelling(uint32_t token) const
    {
      const Token& t = toks[token];
      return text.substr(t.offset, t.length);
    }
    uint32_t endOffset(uint32_t token) const { return toks[token].offset + toks[token].length; }

    // --- preprocessor lines ---

    struct Lines
    {
      std::vector<std::pair<uint32_t, uint32_t>> lines; // offset, length (without the newline)
    };

    // An interface header's macro (isScopeHeader, design.md 16) is the library's interface: a number
    // is a constant of its scope (macroToKppText), a function-like macro computing a value from its
    // arguments a function (`template<typename T> constexpr auto ZSTD_COMPRESSBOUND(T srcSize)`,
    // whose arithmetic is the macro's on the argument's type), and one that is neither (a stringized
    // version, `ZSTD_LIB_VERSION`'s `1.4.0`) a TODO of the report, never left out silently. A guard,
    // an empty one and a linkage word (ZSTDLIB_API) are no value.
    // A macro's text that is one name of a scoped library (Lua's override_printf.h, `#define sprintf
    // trio_sprintf`), written qualified as the name is where the code spells it: `trio::trio_sprintf`.
    std::string scopedQualified(const std::string& text) const
    {
      if (!scopedLibraryNames || text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return isIdentChar(c); }))
        return text;
      auto library = scopedLibraryNames->find(text);
      return library == scopedLibraryNames->end() ? text : library->second + "::" + text;
    }

    // Whether a position of the file is at its scope: outside every function's, class's and
    // initializer's braces (a header's `extern "C" {` block is the file's scope), comments, literals
    // and preprocessor lines skipped. A header-only library's implementation defines macros in its
    // functions (stb_image's STBI__COMBO, stb_truetype's STBTT__CSERR): those are no interface names.
    bool atFileScope(uint32_t offset) const
    {
      std::vector<bool> counted; // each open brace: whether it is a scope's (not extern "C"'s)
      int depth = 0;
      bool lineStart = true;
      for (size_t i = 0; i < offset && i < text.size(); ++i)
      {
        char c = text[i];
        if (c == '\n')
        {
          lineStart = true;
          continue;
        }
        if (isspace((unsigned char)c))
          continue;
        if (lineStart && c == '#')
        {
          for (; i < text.size() && text[i] != '\n'; ++i)
            if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r'))
              i += text[i + 1] == '\r' && i + 2 < text.size() && text[i + 2] == '\n' ? 2 : 1;
          lineStart = true;
          continue;
        }
        lineStart = false;
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
          while (i < text.size() && text[i] != '\n')
            ++i;
          lineStart = true;
        }
        else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
        {
          size_t e = text.find("*/", i + 2);
          i = e == std::string_view::npos ? text.size() : e + 1;
        }
        else if (c == '"' || c == '\'')
        {
          for (++i; i < text.size() && text[i] != c && text[i] != '\n'; ++i)
            i += text[i] == '\\';
        }
        else if (c == '{')
        {
          size_t k = i;
          while (k > 0 && isspace((unsigned char)text[k - 1]))
            --k;
          bool externC = k >= 3 && text.substr(k - 3, 3) == "\"C\"";
          counted.push_back(!externC);
          depth += !externC;
        }
        else if (c == '}' && !counted.empty())
        {
          depth -= counted.back();
          counted.pop_back();
        }
      }
      return depth == 0;
    }

#include "FileConverterPreprocessorLines.inl"
#include "FileConverterTokens.inl"
#include "FileConverterDeclarations.inl"
#include "FileConverterBodies.inl"
#include "FileConverterC.inl"
    std::string run()
    {
      collectScopeRanges(in.parsed->decls, "");
      collectLocals(in.parsed->decls);
      collectDirectiveRanges();
      seedHeaderMacros();
      convertPreprocessor();
      convertBlanked();
      convertTokens();
      convertDecls(in.parsed->decls, true);
      dropExplicitInstantiationsWithoutEffect();
      if (in.c)
      {
        convertC(in.parsed->decls);
        cWestConst();
        cArrayParameters(in.parsed->decls);
        cFunctionTypedefs();
        cCastChains();
      }
      removeUnparsedDefinitions();
      // (the printf rewrite's closings after its arguments: after every other edit there)
      edits.insert(edits.end(), backEdits.begin(), backEdits.end());
      // By offset; at the same offset the longest first, so that a removal cancels the edits inside it.
      auto byOffset = [](const Edit& a, const Edit& b) { return a.offset != b.offset ? a.offset < b.offset : a.length > b.length; };
      std::stable_sort(edits.begin(), edits.end(), byOffset);
      // Local macros, now that their arguments' rewrites are known: each use becomes one replacement,
      // which cancels the edits inside it; the innermost first, as a use inside another's argument
      // is part of that argument. (The uses are in source order: one inside another comes after it.)
      if (!localMacroUses.empty())
      {
        std::vector<int> depth(localMacroUses.size(), 0);
        std::vector<size_t> open;
        int deepest = 0;
        for (size_t u = 0; u < localMacroUses.size(); ++u)
        {
          while (!open.empty() && localMacroUses[open.back()].end <= localMacroUses[u].begin)
            open.pop_back();
          depth[u] = int(open.size());
          deepest = std::max(deepest, depth[u]);
          open.push_back(u);
        }
        for (int level = deepest; level >= 0; --level)
        {
          for (size_t u = 0; u < localMacroUses.size(); ++u)
            if (depth[u] == level)
            {
              const LocalMacroUse& use = localMacroUses[u];
              std::vector<std::string> args, spellings;
              for (auto [b, e] : use.args)
              {
                args.push_back(applyEdits(edits, b, e, true));
                spellings.push_back(std::string(text.substr(b, e - b)));
              }
              std::string expansion = rewritePrintfText(expandMacro(*use.macro, args, use.begin, &spellings));
              // A statement macro in the do/while form, used as a statement (`blueprintAssert(x);`):
              // its body without the wrapper, which only made the use one statement. The use's `;`
              // goes with it; a body of several statements keeps braces, and so does one before an
              // `else` (`if (a) M(x); else`, where an `if` of its own would take the else).
              uint32_t end = use.end;
              if (level == 0)
                if (std::optional<std::string> body = doWhileBody(expansion))
                {
                  size_t semicolon = text.find_first_not_of(" \t", use.end);
                  size_t next = semicolon == std::string_view::npos ? semicolon : text.find_first_not_of(" \t\r\n", semicolon + 1);
                  bool elseNext = next != std::string_view::npos && text.compare(next, 4, "else") == 0 &&
                                  (next + 4 == text.size() || !isIdentChar(text[next + 4]));
                  if (semicolon != std::string_view::npos && text[semicolon] == ';')
                  {
                    expansion = body->empty() ? std::string(elseNext ? "{}" : "") : oneStatement(*body) && !elseNext ? *body : "{ " + *body + " }";
                    // (one that is nothing in this configuration, the final build's ASSERT_RENDER_THREAD,
                    // keeps its `;` after the marks: the merge writes the other configurations' text
                    // as `if constexpr (!config::finalRelease) { release_assert(...); }`)
                    if (!body->empty() || !convertMarks || elseNext)
                      end = uint32_t(semicolon + 1);
                    counts.add("do/while statement macros written without the wrapper");
                  }
                }
              if (level == 0)
              {
                bool endsCase = false, takesSemicolon = false;
                size_t next = text.find_first_not_of(" \t\r\n", end);
                bool semicolonAfter = end == use.end && next != std::string_view::npos && text[next] == ';';
                expansion = dropTrailingBreaksInExpansion(expansion, labelAfterUse(end), semicolonAfter, &endsCase, &takesSemicolon);
                if (endsCase)
                  withdrawFallthrough(use.begin);
                if (takesSemicolon)
                  end = uint32_t(next + 1); // (`HANDLE_CASE(...);`: the use's `;` was its break's)
              }
              // (marked: the macro may be defined otherwise in the other configuration, Lua's api_check
              // an assert in one and a check that aborts in the other; the use's offset is its id)
              if (convertMarks)
              {
                std::string id = in.path.filename().string() + ":@" + std::to_string(use.begin);
                expansion = "/*kpp:if " + id + "*/" + expansion + "/*kpp:endif " + id + "*/";
              }
              edits.push_back({use.begin, end - use.begin, expansion});
            }
          std::stable_sort(edits.begin(), edits.end(), byOffset);
        }
      }
      // Argument macros, now that their arguments' rewrites (this->, renames) are known: each use
      // becomes one replacement, which cancels the edits inside it.
      if (!argumentMacroUses.empty())
      {
        for (const ArgumentMacroUse& use : argumentMacroUses)
        {
          std::vector<std::string> args, spellings;
          for (auto [b, e] : use.args)
          {
            args.push_back(applyEdits(edits, b, e));
            spellings.push_back(std::string(text.substr(b, e - b)));
          }
          bool endsCase = false, takesSemicolon = false;
          size_t next = text.find_first_not_of(" \t\r\n", use.end);
          bool semicolonAfter = next != std::string_view::npos && text[next] == ';';
          std::string expansion = dropTrailingBreaksInExpansion(rewritePrintfText(expandArguments(*use.replacement, args, spellings)), labelAfterUse(use.end),
                                                                semicolonAfter, &endsCase, &takesSemicolon);
          if (endsCase)
            withdrawFallthrough(use.begin);
          edits.push_back({use.begin, (takesSemicolon ? uint32_t(next + 1) : use.end) - use.begin, expansion});
        }
        std::stable_sort(edits.begin(), edits.end(), byOffset);
      }
      // std::variant declarations and uses, now that the edits inside their types and arguments are known.
      if (!variantDeclarations.empty() || !variantUses.empty())
      {
        for (const VariantDeclaration& v : variantDeclarations)
        {
          size_t lineStart = v.memberStart;
          std::string indent;
          for (size_t p = lineStart; p < text.size() && (text[p] == ' ' || text[p] == '\t'); ++p)
            indent += text[p];
          std::string list;
          for (size_t a = 0; a < v.args.size() && a < v.info->alternatives.size(); ++a)
          {
            std::string type = applyEdits(edits, v.args[a].first, v.args[a].second);
            // A block's union names its blocks' classes as written (they have no qualified name),
            // the others qualified as any union's (an alternative named String hides the type).
            if (!v.info->localTypes.count(normalizedSpelling(type)))
              type = qualifiedAlternativeType(type, *v.info, v.memberStart);
            list += std::string(list.empty() ? "" : ", ") + "{" + type + ", " + v.info->alternatives[a].name + "}";
          }
          // std::variant's ==, the same alternative with equal values; a template, so that as for
          // std::variant an alternative without == matters only where two unions are compared.
          const std::string& u = v.info->unionName;
          std::string equality = indent + "  template<typename Other> requires (std::isSame<Other, " + u + ">)\n" + indent +
                                 "  bool operator==(const Other& other) const\n" + indent + "  {\n" + indent + "    for constexpr (auto a : alternatives(" + u +
                                 "))\n" + indent + "      if (*this == a.tag)\n" + indent + "        return other == a.tag && (*this)[a] == other[a];\n" + indent +
                                 "    return false;\n" + indent + "  }\n";
          // std::get<T>, std::holds_alternative<T> and std::get_if<T> for a T only an instantiation
          // knows (a template's parameter): the alternative of type T, by the union's alternatives.
          std::string loop = "for constexpr (auto a : alternatives(" + u + ")) if constexpr (std::isSame<a.type, T>) ";
          std::string generic = indent + "  template<typename T>\n" + indent + "  T& get() { " + loop + "return (*this)[a]; }\n" + indent +
                                "  template<typename T>\n" + indent + "  const T& get() const { " + loop + "return (*this)[a]; }\n" + indent +
                                "  template<typename T>\n" + indent + "  bool holds() const { " + loop + "return *this == a.tag; return false; }\n" + indent +
                                "  template<typename T>\n" + indent + "  T* getIf() { " + loop + "return *this == a.tag ? &(*this)[a] : nullptr; return nullptr; }\n" +
                                indent + "  template<typename T>\n" + indent + "  const T* getIf() const { " + loop +
                                "return *this == a.tag ? &(*this)[a] : nullptr; return nullptr; }\n" +
                                // std::variant's emplace<T>(args...), built where it stays (`=` would
                                // move it: design.md 8.5), and index(), a size_t as std's (a `%zu`
                                // reads 8 bytes, a << writes them).
                                indent + "  template<typename T, typename... Args>\n" + indent + "  T& emplace(Args&&... args) { " + loop +
                                "{ this->~" + u + "(); new (this) " + u + "(a.tag, T(std::forward<Args>(args)...)); return (*this)[a]; } }\n" + indent +
                                "  size_t index() const { return size_t(this->tag()); }\n";
          std::string declaration = indent + "union enum " + u + "\n" + indent + "{\n" + indent + "  { " + list + " }\n" + equality + generic + indent + "};";
          if (v.member)
          {
            edits.push_back({v.memberStart, 0, declaration + "\n"});
            edits.push_back({v.begin, v.end - v.begin, v.info->unionName});
          }
          else
            edits.push_back({v.begin, v.end - v.begin, declaration});
        }
        for (const VariantUse& use : variantUses)
        {
          auto known = callVariant.find(use.calleeToken);
          const VariantInfo* info = known != callVariant.end() ? known->second : nullptr;
          if (use.noAlternative)
          {
            // T is no alternative's spelling: through the union's generic accessors, when the
            // argument is known to be one of the converted variants; left as written otherwise
            // (std::get<T> of something else).
            if (!info || use.args.empty() || use.typeSpelling.empty() || isdigit((unsigned char)use.typeSpelling[0]))
              continue;
            std::string arg = applyEdits(edits, use.args[0].first, use.args[0].second);
            std::string type = applyEdits(edits, use.typeBegin, use.typeEnd);
            std::string replacement = use.kind == "get"                 ? "(" + arg + ").template get<" + type + ">()"
                                    : use.kind == "holds_alternative" ? "(" + arg + ").template holds<" + type + ">()"
                                                                      : "((" + arg + ") ? (" + arg + ")->template getIf<" + type + ">() : nullptr)";
            edits.push_back({use.begin, use.end - use.begin, replacement});
            counts.add("std::" + use.kind + " of a template's type on a union enum");
            continue;
          }
          if (!info && use.kind != "visit")
            for (const VariantInfo& v : variants->all)
              if (alternativeFor(v, use.typeSpelling))
              {
                if (info)
                {
                  info = nullptr; // several variants have this alternative: the argument's declaration has to tell
                  break;
                }
                info = &v;
              }
          const VariantAlternative* alternative = info && use.kind != "visit" ? alternativeFor(*info, use.typeSpelling) : nullptr;
          if (!info || (use.kind != "visit" && !alternative) || (use.kind == "visit" && use.args.size() != 2))
          {
            todo(use.begin, "std::" + use.kind + " on a variant the converter could not tell: left as written");
            continue;
          }
          std::vector<std::string> args;
          for (auto [b, e] : use.args)
            args.push_back(applyEdits(edits, b, e));
          std::string u = info->qualified + "::";
          std::string replacement;
          if (use.kind == "holds_alternative")
            replacement = "((" + args[0] + ") == " + u + alternative->name + ")";
          else if (use.kind == "get")
            replacement = "(" + args[0] + ")[" + u + alternative->name + "]";
          else if (use.kind == "get_if")
            replacement = "(*(" + args[0] + ") == " + u + alternative->name + " ? &(*(" + args[0] + "))[" + u + alternative->name + "] : nullptr)";
          else
          {
            // std::visit(f, x): f called with the alternative x holds, by a switch over the alternatives.
            std::string cases;
            for (const VariantAlternative& a : info->alternatives)
              cases += "case " + u + a.name + ": return visitor(value[" + u + a.name + "]); ";
            // (The visitor is held by value: `auto&&` bound to a lambda does not compile yet, spec
            // language/forwarding_reference_to_lambda.)
            replacement = "[&]() { auto visitor = " + args[0] + "; auto&& value = " + args[1] + "; switch (value) { " + cases + "} }()";
          }
          edits.push_back({use.begin, use.end - use.begin, replacement});
          counts.add("std::" + use.kind + " on a union enum");
        }
        std::stable_sort(edits.begin(), edits.end(), byOffset);
      }
      // (continuation lines keep their alignment where the edits change the length before it: AlignContinuations.inl)
      if (std::vector<Edit> moves = alignmentEdits(std::string(text), edits); !moves.empty())
      {
        counts.add("continuation lines moved to keep their alignment", int(moves.size()));
        edits.insert(edits.end(), moves.begin(), moves.end());
        std::stable_sort(edits.begin(), edits.end(), byOffset);
      }
      std::string out = applyEdits(edits, 0, uint32_t(text.size()));
      // At most two consecutive blank lines where includes and directives used to be.
      std::string squeezed;
      squeezed.reserve(out.size());
      int blank = 0;
      size_t lineStart = 0;
      for (size_t i = 0; i <= out.size(); ++i)
      {
        if (i == out.size() || out[i] == '\n')
        {
          std::string_view line(out.data() + lineStart, i - lineStart);
          bool isBlank = line.find_first_not_of(" \t\r") == std::string_view::npos;
          blank = isBlank ? blank + 1 : 0;
          if (blank <= 2)
          {
            squeezed.append(line);
            if (i < out.size())
              squeezed += '\n';
          }
          lineStart = i + 1;
        }
      }
      // `std::name` in text the parser never tokenized (the other branch of an #if, a macro's
      // expansion, where an X-macro list's `PREFIX TYPE` makes `std:: string`): the same renames
      // by text.
      for (size_t p = squeezed.find("std::"); p != std::string::npos; p = squeezed.find("std::", p + 1))
      {
        if (p > 0 && (isalnum((unsigned char)squeezed[p - 1]) || squeezed[p - 1] == '_'))
          continue;
        size_t s = p + 5;
        while (s < squeezed.size() && (squeezed[s] == ' ' || squeezed[s] == '\t'))
          ++s;
        size_t e = s;
        while (e < squeezed.size() && (isalnum((unsigned char)squeezed[e]) || squeezed[e] == '_'))
          ++e;
        auto it = stdNames.find(squeezed.substr(s, e - s));
        if (it != stdNames.end())
        {
          squeezed.replace(p, e - p, it->second);
          counts.add("std renamed");
        }
      }
      std::string named = in.c ? cMathNames(squeezed) : kppNumberNames(cMathNames(squeezed));
      return alignedAsBefore(squeezed, named);
    }

    // C's <math.h> classifications, macros C writes bare (Lua's .c files, converted as C++ too), are the library's
    // functions (std::isnan: core/Math.kpp); so are C's math functions a macro's text calls, which the token pass does not see.
    std::string cMathNames(const std::string& text)
    {
      std::string out;
      for (size_t i = 0; i < text.size();)
      {
        char c = text[i];
        if (c == '"' || (c == '\'' && !isDigitSeparator(text, i)))
        {
          size_t j = i + 1;
          while (j < text.size() && text[j] != c)
            j += text[j] == '\\' ? 2 : 1;
          j = std::min(j + 1, text.size());
          out.append(text, i, j - i);
          i = j;
          continue;
        }
        if (c == '/' && i + 1 < text.size() && (text[i + 1] == '/' || text[i + 1] == '*'))
        {
          size_t j = text[i + 1] == '/' ? text.find('\n', i) : text.find("*/", i + 2);
          j = j == std::string::npos ? text.size() : text[i + 1] == '/' ? j : j + 2;
          out.append(text, i, j - i);
          i = j;
          continue;
        }
        if (!isIdentChar(c))
        {
          out += c;
          ++i;
          continue;
        }
        size_t j = i;
        while (j < text.size() && isIdentChar(text[j]))
          ++j;
        std::string word = text.substr(i, j - i);
        bool member = !out.empty() && (out.back() == '.' || out.back() == ':' || (out.back() == '>' && out.size() > 1 && out[out.size() - 2] == '-'));
        size_t after = text.find_first_not_of(" \t", j);
        // (a macro's text writes the rest: stb_truetype's STBTT_sqrt; not a declaration's name, after its type)
        static const std::set<std::string> math = {"isnan", "isinf", "isfinite", "signbit", "abs", "fabs", "floor", "ceil", "round", "trunc", "sqrt",
                                                   "pow", "fmod", "exp", "log", "sin", "cos", "atan2", "lround", "nextafter"};
        size_t last = out.find_last_not_of(" \t");
        size_t wordStart = last;
        while (wordStart != std::string::npos && wordStart > 0 && isIdentChar(out[wordStart - 1]))
          --wordStart;
        bool declared = last != std::string::npos && isIdentChar(out[last]) && out.compare(wordStart, last + 1 - wordStart, "return") != 0 &&
                        out.compare(wordStart, last + 1 - wordStart, "else") != 0 && out.compare(wordStart, last + 1 - wordStart, "case") != 0;
        bool classification = word == "isnan" || word == "isinf" || word == "isfinite" || word == "signbit";
        if (!member && !declared && after != std::string::npos && text[after] == '(' && math.count(word) && (classification || in.c))
          out += "std::";
        out += word;
        i = j;
      }
      return out;
    }

    // kpp's names of the integer and floating types (design.md 13, "What the converter does"):
    // `uint32_t` is `u32`, `int` `i32`, `unsigned char` `u8`, `size_t` `usize`, `double` `f64`. Each is
    // the same type under kpp's name, so nothing else changes. Done on the converted C++ file's text,
    // so that the names the converter writes itself (a printf piece's `uint32_t(x)`) and the macros'
    // expansions get them too; literals and comments keep their text (a check's `#value`). C's names
    // stay inside `extern "C"` declarations and blocks, which meet C, and `long` stays: C's long is
    // a type of its own (32 bits on Windows) that kpp has no other name for. Plain char is the text
    // character, and stays.
    std::string kppNumberNames(const std::string& text)
    {
      static const std::map<std::string_view, std::string_view> names = {
        {"uint8_t", "u8"},  {"uint16_t", "u16"}, {"uint32_t", "u32"}, {"uint64_t", "u64"}, {"int8_t", "i8"},     {"int16_t", "i16"},
        {"int32_t", "i32"}, {"int64_t", "i64"},  {"size_t", "usize"}, {"ptrdiff_t", "i64"}, {"intptr_t", "i64"}, {"uintptr_t", "u64"}};
      static const std::set<std::string_view> words = {"signed", "unsigned", "short", "long", "int", "char", "float", "double"};
      auto identStart = [](char c) { return isalpha((unsigned char)c) || c == '_'; };
      std::string out;
      out.reserve(text.size());
      // The last three characters of code written (not of comments): `.` or `->` before a name makes it
      // a member's (a comment ending with a period before a declaration does not).
      char last = 0, beforeLast = 0, third = 0;
      auto code = [&](std::string_view written)
      {
        out.append(written);
        for (char ch : written)
          if (!isspace((unsigned char)ch))
            third = beforeLast, beforeLast = last, last = ch;
      };
      for (size_t i = 0; i < text.size();)
      {
        char c = text[i];
        if (size_t raw = rawStringEnd(text, i); raw != std::string_view::npos)
        {
          code(std::string_view(text).substr(i, raw - i));
          i = raw;
          continue;
        }
        if (c == '"' || (c == '\'' && !isDigitSeparator(text, i)))
        {
          size_t e = i + 1;
          for (; e < text.size() && text[e] != c && text[e] != '\n'; ++e)
            e += text[e] == '\\';
          e = std::min(e + 1, text.size());
          code(std::string_view(text).substr(i, e - i));
          i = e;
          continue;
        }
        if (text.compare(i, 2, "//") == 0)
        {
          size_t e = text.find('\n', i);
          e = e == std::string::npos ? text.size() : e;
          out.append(text, i, e - i);
          i = e;
          continue;
        }
        if (text.compare(i, 2, "/*") == 0)
        {
          size_t e = text.find("*/", i + 2);
          e = e == std::string::npos ? text.size() : e + 2;
          out.append(text, i, e - i);
          i = e;
          continue;
        }
        if (isdigit((unsigned char)c))
        {
          // (a number's suffix is no name: `1ull`, `0x1fu`)
          size_t e = i;
          while (e < text.size() && (isIdentChar(text[e]) || (text[e] == '\'' && isDigitSeparator(text, e)) || text[e] == '.'))
            ++e;
          code(std::string_view(text).substr(i, e - i));
          i = e;
          continue;
        }
        if (!identStart(c))
        {
          code(std::string_view(&text[i], 1));
          ++i;
          continue;
        }
        size_t e = i;
        while (e < text.size() && isIdentChar(text[e]))
          ++e;
        std::string_view word(text.data() + i, e - i);
        if (word == "extern")
        {
          // `extern "C"`: the declaration, or the block, as it is.
          size_t q = e;
          while (q < text.size() && isspace((unsigned char)text[q]))
            ++q;
          if (text.compare(q, 3, "\"C\"") == 0)
          {
            size_t end = q + 3;
            int depth = 0;
            for (; end < text.size(); ++end)
            {
              if (size_t raw = rawStringEnd(text, end); raw != std::string_view::npos)
                end = raw - 1;
              else if (text[end] == '"' || (text[end] == '\'' && !isDigitSeparator(text, end)))
              {
                char quote = text[end];
                for (++end; end < text.size() && text[end] != quote && text[end] != '\n'; ++end)
                  end += text[end] == '\\';
              }
              else if (text.compare(end, 2, "//") == 0)
                end = std::min(text.find('\n', end), text.size()) - 1;
              else if (text.compare(end, 2, "/*") == 0)
                end = std::min(text.find("*/", end + 2), text.size() - 2) + 1;
              else if (text[end] == '{' || text[end] == '(' || text[end] == '[')
                ++depth;
              else if (text[end] == '}' || text[end] == ')' || text[end] == ']')
              {
                if (--depth == 0 && text[end] == '}')
                  break;
              }
              else if (text[end] == ';' && depth == 0)
                break;
            }
            end = std::min(end + 1, text.size());
            code(std::string_view(text).substr(i, end - i));
            i = end;
            continue;
          }
        }
        if (words.count(word))
        {
          // A run of C's type words: `unsigned long long`, `short int`, `signed char`.
          int isSigned = 0, isUnsigned = 0, shorts = 0, longs = 0, ints = 0, chars = 0, floats = 0, doubles = 0;
          size_t runEnd = i, q = i;
          std::string spelled;
          while (q < text.size() && identStart(text[q]))
          {
            size_t we = q;
            while (we < text.size() && isIdentChar(text[we]))
              ++we;
            std::string_view w(text.data() + q, we - q);
            if (!words.count(w))
              break;
            isSigned += w == "signed", isUnsigned += w == "unsigned", shorts += w == "short", longs += w == "long", ints += w == "int";
            chars += w == "char", floats += w == "float", doubles += w == "double";
            spelled += (spelled.empty() ? "" : " ") + std::string(w);
            runEnd = we;
            q = we;
            while (q < text.size() && (text[q] == ' ' || text[q] == '\t'))
              ++q;
          }
          std::string kpp;
          if (floats == 1 && runEnd == i + 5)
            kpp = "f32";
          else if (doubles == 1 && runEnd == i + 6)
            kpp = "f64";
          else if (floats || doubles || longs == 1)
            kpp = ""; // long, long double: as they are
          else if (chars)
            kpp = isUnsigned ? "u8" : isSigned ? "i8" : "";
          else if (shorts)
            kpp = isUnsigned ? "u16" : "i16";
          else if (longs == 2)
            kpp = isUnsigned ? "u64" : "i64";
          else
            kpp = isUnsigned ? "u32" : "i32";
          if (!kpp.empty())
          {
            counts.add("C type names written as kpp's: " + spelled + " as " + kpp);
            code(kpp);
          }
          else
          {
            if (longs == 1 && !doubles)
              counts.add("C type names kept: " + spelled);
            code(std::string_view(text).substr(i, runEnd - i));
          }
          i = runEnd;
          continue;
        }
        auto name = names.find(word);
        char before = last;
        // (`) -> T`, a trailing return type, is no member)
        bool member = before == '.' || (before == '>' && beforeLast == '-' && third != ')');
        // `::` before the name (one colon is an access specifier's, an enum's base, a bit-field's or
        // `?:`'s, after which the name is renamed as anywhere).
        bool qualified = before == ':' && beforeLast == ':';
        if (name != names.end() && !member)
        {
          if (qualified)
          {
            // `::size_t`: kpp's name is a built-in, which no `::` qualifies; `glm::uint32_t` (a
            // qualified one, another namespace's) stays.
            size_t colons = out.find_last_not_of(" \t\r\n");
            size_t prior = colons >= 2 ? out.find_last_not_of(" \t\r\n", colons - 2) : std::string::npos;
            bool global = colons >= 1 && out[colons - 1] == ':' &&
                          (prior == std::string::npos || !(isIdentChar(out[prior]) || out[prior] == '>'));
            if (!global)
            {
              code(word);
              i = e;
              continue;
            }
            out.erase(colons - 1);
          }
          counts.add("C type names written as kpp's: " + std::string(word) + " as " + std::string(name->second));
          code(name->second);
          i = e;
          continue;
        }
        code(word);
        i = e;
      }
      return out;
    }

    const ConvertInput& in;
    GlobalScope& global;
    const SymbolTable& symbols;
    const OverrideKeys* overrides = nullptr; // definitions a hand-written file replaces
    std::map<std::string, MemberOverride>* memberOverrides = nullptr; // members a hand-written file adds to classes
    const CarriedDefaults* carriedDefaults = nullptr;
    const CarriedTemplateDefaults* carriedTemplateDefaults = nullptr;
    const ScopeNames* scopeNames = nullptr;
    const std::set<std::string>* libraryTypes = nullptr; // the library's global classes and aliases (libraryTypeNames)
    const ProgramNames* programNames = nullptr; // what the program's types name (convertToKpp)
    std::set<std::string>* opaqueKept = nullptr; // classes declared and defined nowhere, kept as declarations (once in the program)
    const std::set<std::string>* macroClasses = nullptr; // the classes ENUM and BITFIELD define (collectMacroClasses)
    const std::set<std::string>* braceableClasses = nullptr; // collectBraceableClasses
    const InlineBodies* inlineBodies = nullptr; // collectInlineBodies
    const SuperAliases* superAliases = nullptr; // collectSuperAliases
    // The files the conversion leaves out: a class defined only in one of them is defined nowhere
    // (GraphicsInterface.hpp's `class ShaderProgram;` stays, as the game's files see it).
    const std::set<const ParsedFile*>* leftOutFiles = nullptr;
    const Variants* variants = nullptr;
    std::vector<VariantDeclaration> variantDeclarations;
    std::vector<VariantUse> variantUses;
    std::map<uint32_t, const VariantInfo*> callVariant; // a use's callee token -> the variant its argument is declared as
    std::vector<ScopeRange> scopeRanges;
    std::vector<DirectiveRange> directiveRanges;
    // The locals and parameters of the file's functions by name: where each declaration starts and
    // where its function ends (the block it is in ends there or sooner: a `::` too many is harmless).
    std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> localNames;
    std::vector<FunctionRange> functionRanges;
    std::vector<UnderscoreLocal> underscoreLocals;
    std::set<uint32_t> namedUnderscores;
    // Function-like macros defined in this file, expanded at their uses; a name can be defined,
    // undefined and defined again (`#define INCREMENT(NAME) 1+` then with four parameters).
    std::map<std::string, std::vector<LocalMacro>> localMacros;
    // Function-like macros of the C headers the program imports (Lua's lua_pop and the rest, from the
    // headers expand-macros.txt in the overrides lists), expanded like this file's own: the imported
    // headers give kpp their functions and constants, not their macros.
    const std::map<std::string, std::pair<std::vector<std::string>, std::string>>* headerMacros = nullptr;
    // The imported C headers' object-like macros that are another name (glad's glGenTextures is
    // glad_glGenTextures): a use is written as that name.
    const std::map<std::string, std::string>* headerAliases = nullptr;
    // This .cpp file's namespace-scope aliases that another file declares too, with their own
    // names (`using MyBinder = LuaBinder<LuaRocketSiloControlBehavior>;` is
    // MyBinderOfLuaRocketSiloControlBehavior): file-local in C++, program-wide in kpp.
    std::map<std::string, std::string> fileAliasRenames;
    // The function-like macros of the game's headers this file includes, directly or through other
    // headers, that they leave defined (the X-macro lists a header defines and its .cpp expands), in
    // include order.
    std::vector<const std::map<std::string, std::pair<std::vector<std::string>, std::string>>*> includedMacros;
    // The includes of this file that name a converted file beside it (siblingIncludes): removed.
    const std::set<std::string>* siblingIncludes = nullptr;
    const std::set<std::string>* cInterfaceHeaders = nullptr; // overrides/c-headers.txt: included, imported
    const std::map<std::string, std::set<size_t>>* cInterfaceNames = nullptr; // the functions those headers declare: their parameter counts
    const std::map<std::string, std::string>* cStaticRenames = nullptr; // C: this file's statics another file declares too
    bool cGivenHeader = false; // C: a header given alone, which holds a header-only library's implementation (stb_truetype.h)
    // C: a header given alone that c-headers.txt keeps as C, which the program imports (fontstash.h):
    // its text as the import configures it. What that leaves in is the import's, and goes.
    const std::string* cInterfaceText = nullptr;
    const std::set<std::string>* cPrivateNames = nullptr; // C: the library's private-names.txt (zlib's inflate.h OS)
    // C: the names this .c file (or C header given alone) keeps to itself, written `private`
    // (design.md 15), and the declaration starts that have it already (`typedef struct {...} X;` is
    // two declarations)
    const std::set<std::string>* cPrivate = nullptr;
    std::set<uint32_t> cPrivateAt;
    const std::map<std::string, std::string>* cConfiguredImports = nullptr; // and what an include of one imports instead
    const CMacros* cMacros = nullptr; // C: the macros the parse saw through, also inside macro expansions
    const std::set<std::string>* cWrittenOutMacros = nullptr; // C: macro constants written out at their uses, not made constants ("*": all)
    // C: the library's interface headers converted into its scope (overrides/interface.txt, design.md
    // 16: zstd's zstd.h), whose number macros are constants of the scope and whose function-like
    // macros of values are functions; and the constants they made, by name, which every file's uses
    // write by name.
    const std::set<std::string>* cScopeHeaders = nullptr;
    std::map<std::string, std::string>* cScopeConstants = nullptr;
    std::vector<std::string> scopeFunctions; // an interface header's functions, in its order
    // --convert-library: the scoped libraries this program uses, their names by library (design.md
    // 16): a use of one is written qualified (zstd::ZSTD_compress), and an include of its headers
    // (zstd/zstd.h) imports nothing.
    const std::map<std::string, std::string>* scopedLibraryNames = nullptr; // name -> its library
    const std::set<std::string>* scopedLibraries = nullptr;
    const std::set<std::string>* scopedHeaders = nullptr; // their interface headers' names (zlib.h), included as "zlib.h" too
    const std::map<std::string, std::vector<std::string>>* scopedHeaderImports = nullptr; // what an include of one still imports
    // The std names this file brought in with a using-declaration (XmlTestReporter.cpp's `using
    // std::ostream;`), by their kpp names: its later unqualified uses are the library's.
    std::map<std::string, std::string> stdUsingNames;
    // Every function-like macro's name (callsFunction): an object-like macro that calls something else
    // is written out at its uses.
    const std::set<std::string>* functionMacroNames = nullptr;
    // The converted files' classes and aliases by their own name (convertToKpp), for the fixture of a
    // fixture test.
    const std::map<std::string, std::vector<Decl*>>* classesByName = nullptr;
    // The tests the conversion leaves out (overrides/left-out-tests.txt: `Suite/Test` -> why), those
    // found and the suites converted: the block of one goes, a comment in its place says why, and its
    // suite runs its other tests (a stub would stop the suite where the test reaches it). The SUITE
    // blocks seen so far tell a test's suite: the suite's name and its block's `{` and `}` tokens.
    const std::map<std::string, std::string>* leftOutTests = nullptr;
    std::set<std::string>* leftOutTestsFound = nullptr;
    // The `implicit` marks this file goes without (overrides/implicit-dropped.txt, its entries for this
    // file): a converting constructor or conversion operator no build of the game asks about, by
    // implicitKey, and those found.
    const std::set<std::string>* implicitDropped = nullptr;
    std::set<std::string>* implicitDroppedFound = nullptr;
    // A converting constructor's or conversion operator's key in implicit-dropped.txt: `Class::Class(a, b)`,
    // `Class::operator()`, its class's own name and its parameters' names (`_` for one unnamed),
    // which the converted declaration keeps (programs/factorio/implicit-dropped.py reads them there).
    std::string implicitKey(FunctionDecl* f, const FunctionDecl* named)
    {
      Decl* owner = global.owningClass(f);
      if (!owner || owner->kind != NodeKind::ClassDecl || static_cast<ClassDecl*>(owner)->name.empty() ||
          static_cast<ClassDecl*>(owner)->name.last().symbol == UINT32_MAX)
        return std::string();
      std::string cls(symbols.name(static_cast<ClassDecl*>(owner)->name.last().symbol));
      std::string key = cls + "::" + ((f->flags & DF_Conversion) ? std::string("operator") : cls) + "(";
      for (size_t i = 0; i < named->params.size(); ++i)
      {
        const Decl* p = named->params[i];
        uint32_t name = p->kind == NodeKind::ParamDecl ? static_cast<const ParamDecl*>(p)->name : UINT32_MAX;
        key += (i ? ", " : "") + (name == UINT32_MAX ? std::string("_") : std::string(symbols.name(name)));
      }
      return key + ")";
    }
    bool implicitDroppedKey(const std::string& key)
    {
      if (!implicitDropped || key.empty() || !implicitDropped->count(key))
        return false;
      implicitDroppedFound->insert(key);
      counts.add("implicit left out: no build asks about it (implicit-dropped.txt)");
      return true;
    }
    // (A class the converter writes itself: a BITFIELD's.)
    std::string implicitWord(const std::string& key) { return implicitDroppedKey(key) ? std::string() : std::string("implicit "); }
    // (By the parameter names the converted declaration has: its own, or, where the body moves into the
    // class, the definition's. An out-of-line definition that stays one has a mark of its own.)
    bool implicitDroppedFor(FunctionDecl* f)
    {
      if (!implicitDropped)
        return false;
      if (inlineBodies && inlineBodies->declarations.count(f))
      {
        Decl* def = global.definitionOf(f);
        if (def && def != f && def->kind == NodeKind::FunctionDecl)
          return implicitDroppedKey(implicitKey(f, static_cast<FunctionDecl*>(def)));
      }
      return implicitDroppedKey(implicitKey(f, f));
    }
    std::set<std::string>* suitesConverted = nullptr;
    std::vector<SuiteBlock> suiteBlocks;
    std::vector<FixtureBody> fixtureBodies;
    // The C libraries this file's library keeps calling, by include path prefix, imported where it
    // includes them (Lua's override_printf.h includes trio.h, CommonUtil's StringUtil.cpp too; the
    // includes of trio.h elsewhere, which call none of it, go).
    const std::set<std::string>* keptCLibraries = nullptr;
    // The object-like macros of this file and the headers it reaches that are written out at their
    // uses: those whose body is string literals (UTF8_BULLET; constants in the conversion) where
    // C++ concatenates them with a literal (`"\n" UTF8_BULLET " "`, since only literals concatenate),
    // and those whose body is a qualified name everywhere (`#define vkCreateInstance
    // VulkanImpl::CreateInstanceWrapper`: Vulkan through the function pointers the game loads).
    std::map<std::string, std::string> objectMacros;
    // Where this file's own #define of a name holds: from the #define to its #undef (a header's,
    // seen through an include, holds everywhere).
    std::map<std::string, std::pair<uint32_t, uint32_t>> ownMacroRanges;
    // Which lines the configuration keeps (null: the file's own lines decide, without its includes).
    const Preprocessed* preprocessed = nullptr;
    // `#pragma pack(push, 1)` to its `#pragma pack(pop)`: the classes defined there are [[packed]]
    // (Tile, ImmutableStringTemplate, PropertyTree). A pack of 8, the largest alignment there is,
    // changes nothing; another value gets a TODO.
    std::vector<std::pair<uint32_t, uint32_t>> packedRanges;
    std::vector<int> packStack;
    int pack = 0; // 0: the default
    uint32_t packedSince = 0;
    std::set<std::string> replacedFunctions;  // qualified names whose definitions were found and removed
    std::set<std::string> replacedVariables;  // the same, of variables
    std::vector<Edit> edits;
    std::vector<ArgumentMacroUse> argumentMacroUses;
    std::vector<LocalMacroUse> localMacroUses;
    // The `fallthrough;` walkSwitchBody inserted before a label (its offset) after the statement at an offset: a
    // macro's use there ends its case when its expansion ends with the case's break (see withdrawFallthrough).
    std::map<uint32_t, uint32_t> fallthroughAfterUse;
    std::vector<ClassDecl*> classStack; // the classes whose members convertDecls is in, outermost first
    // The identifiers of allocator arguments dropped from std container types (convertTokens), by
    // offset: a template parameter used only there goes from its header too (dropAllocatorParameters).
    std::map<uint32_t, std::string> droppedAllocators;
    Counts counts;
    // This file's #if groups (convertMarks): the next one's number, and those open, each with whether
    // its #if line is written out.
    uint32_t nextIfGroup = 0;
    std::vector<std::pair<uint32_t, bool>> openIfGroups;
    // The C headers this file imports, in its include order: written once per library, in its
    // c-imports.kpp (writeLibraryImports), not in the file.
    std::vector<std::string> cImports;
    std::vector<std::string> todos; // "line: text"
    std::string_view text;
    const std::vector<Token>& toks;
  };
