// NameMap.inl: the game's class renames, std names, aliases and scope names.
// Part of convert.cpp, included where it stood in its anonymous namespace (a file of its own since 2026-11-05, moved without a change).

  // Text of the game's C++ carried into the output by hand (a default argument a declaration that
  // goes had, a type written again): std:: names mapped as the token pass maps them
  // (`std::atomic<bool>` is std::Atomic<bool>).
  std::string mapStdText(std::string value)
  {
    for (size_t at = value.find("std::"); at != std::string::npos; at = value.find("std::", at))
    {
      size_t nameEnd = at + 5;
      while (nameEnd < value.size() && (isalnum((unsigned char)value[nameEnd]) || value[nameEnd] == '_'))
        ++nameEnd;
      std::string name = value.substr(at + 5, nameEnd - at - 5);
      auto it = stdNames.find(name);
      std::string to = it == stdNames.end() ? name : it->second;
      value.replace(at, nameEnd - at, to);
      at += to.size();
    }
    return value;
  }

  // The aliases the program declares (`using X = ...;` and typedefs), at any scope: their name,
  // the file, and whether at namespace scope (in C++ a .cpp file's namespace-scope alias is its
  // own; in kpp every namespace-scope name is the program's).
  struct AliasDeclaration
  {
    std::string name;
    const void* file;
    bool namespaceScope;
  };
  void collectAliasesIn(Stmt* s, const void* file, const SymbolTable& symbols, std::vector<AliasDeclaration>& out);
  void collectAliases(Span<Decl*> decls, const void* file, bool namespaceScope, const SymbolTable& symbols, std::vector<AliasDeclaration>& out)
  {
    for (Decl* d : decls)
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl: collectAliases(static_cast<NamespaceDecl*>(d)->decls, file, true, symbols, out); break;
        case NodeKind::ClassDecl: collectAliases(static_cast<ClassDecl*>(d)->members, file, false, symbols, out); break;
        case NodeKind::UsingAliasDecl: out.push_back({std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name)), file, namespaceScope}); break;
        case NodeKind::VariableDecl:
          if ((d->flags & DF_Typedef) && static_cast<VariableDecl*>(d)->name.parts.size() == 1)
            out.push_back({std::string(symbols.name(static_cast<VariableDecl*>(d)->name.parts[0].symbol)), file, namespaceScope});
          break;
        case NodeKind::FunctionDecl: collectAliasesIn(static_cast<FunctionDecl*>(d)->body, file, symbols, out); break;
        default: break;
      }
  }
  void collectAliasesIn(Stmt* s, const void* file, const SymbolTable& symbols, std::vector<AliasDeclaration>& out)
  {
    if (!s)
      return;
    switch (s->kind)
    {
      case NodeKind::BlockStmt:
        for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
          collectAliasesIn(inner, file, symbols, out);
        break;
      case NodeKind::DeclStmt: collectAliases(static_cast<DeclStmt*>(s)->decls, file, false, symbols, out); break;
      case NodeKind::IfStmt:
        collectAliasesIn(static_cast<IfStmt*>(s)->then, file, symbols, out);
        collectAliasesIn(static_cast<IfStmt*>(s)->else_, file, symbols, out);
        break;
      case NodeKind::ForStmt: collectAliasesIn(static_cast<ForStmt*>(s)->body, file, symbols, out); break;
      case NodeKind::RangeForStmt: collectAliasesIn(static_cast<RangeForStmt*>(s)->body, file, symbols, out); break;
      case NodeKind::WhileStmt: collectAliasesIn(static_cast<WhileStmt*>(s)->body, file, symbols, out); break;
      case NodeKind::DoStmt: collectAliasesIn(static_cast<DoStmt*>(s)->body, file, symbols, out); break;
      case NodeKind::SwitchStmt: collectAliasesIn(static_cast<SwitchStmt*>(s)->body, file, symbols, out); break;
      case NodeKind::TryStmt:
        collectAliasesIn(static_cast<TryStmt*>(s)->body, file, symbols, out);
        for (const TryStmt::Handler& h : static_cast<TryStmt*>(s)->handlers)
          collectAliasesIn(h.body, file, symbols, out);
        break;
      default: break;
    }
  }

  // --- hand-written overrides ---

  // What an override file defines, by qualified name: those definitions of the converted file
  // are dropped and the override file's text is appended.
  struct OverrideKeys
  {
    std::set<std::string> functions;           // "ns::name/arity", or "ns::name/*" for every overload
    std::map<std::string, int> functionCounts; // definitions per qualified name
    std::set<std::string> classes;             // "ns::Class" (its out-of-line members go too)
    std::set<std::string> variables;           // "ns::name"
  };

  std::string nameText(const QualifiedName& name, const SymbolTable& symbols)
  {
    std::string s;
    for (size_t i = 0; i < name.parts.size(); ++i)
    {
      const NamePart& p = name.parts[i];
      if (i)
        s += "::";
      if (p.special == 1)
        s += "~";
      if (p.special == 2)
        s += "operator#" + std::to_string(p.opToken);
      else if (p.special == 3)
        s += "operator-conversion";
      else if (p.symbol != UINT32_MAX)
        s += std::string(symbols.name(p.symbol));
    }
    return s;
  }

  // ScopeNames from a scope's declarations; prefix is the scope's qualified name and `::` ("" at
  // global scope). The global namespace's names are under "(global)" ("::" is an unnamed
  // namespace's): its classes, aliases and variables, which a name a using-directive finds is
  // ambiguous with, not its functions (one overload set with the directive's).
  void collectScopeNames(Span<Decl*> decls, const std::string& prefix, const SymbolTable& symbols, ScopeNames& out)
  {
    auto declare = [&](std::string_view name)
    {
      if (!name.empty())
        out[prefix.empty() ? "(global)" : prefix].insert(std::string(name));
    };
    for (Decl* d : decls)
    {
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
        {
          auto* n = static_cast<NamespaceDecl*>(d);
          std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
          collectScopeNames(n->decls, prefix + name + "::", symbols, out);
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (c->name.empty() || c->name.last().symbol == UINT32_MAX)
            break;
          std::string_view own = symbols.name(c->name.last().symbol);
          declare(own);
          if (c->isDefinition)
          {
            std::string inner = prefix + nameText(c->name, symbols) + "::";
            out[inner].insert(std::string(own)); // the injected class name
            collectScopeNames(c->members, inner, symbols, out);
          }
          break;
        }
        case NodeKind::EnumDecl:
        {
          auto* e = static_cast<EnumDecl*>(d);
          if (!e->name.empty() && e->name.last().symbol != UINT32_MAX)
            declare(symbols.name(e->name.last().symbol));
          if (!e->isClass)
            for (Decl* v : e->enumerators)
              declare(symbols.name(static_cast<EnumeratorDecl*>(v)->name));
          break;
        }
        case NodeKind::UsingAliasDecl:
          declare(symbols.name(static_cast<UsingAliasDecl*>(d)->name));
          break;
        case NodeKind::VariableDecl:
        {
          auto* v = static_cast<VariableDecl*>(d);
          if (v->name.parts.size() == 1 && v->name.last().symbol != UINT32_MAX)
            declare(symbols.name(v->name.last().symbol));
          break;
        }
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          if (!prefix.empty() && f->name.parts.size() == 1 && f->name.last().special == 0 && f->name.last().symbol != UINT32_MAX)
            declare(symbols.name(f->name.last().symbol));
          break;
        }
        default: break;
      }
    }
  }

  // The bases of the classes, as written (`ControllerProperties`, `agui::Widget`), by the class's
  // ScopeNames key, with the scope the class is declared in (where a base's name is looked up).
  struct ClassBases
  {
    std::string enclosing;
    std::vector<std::string> bases;
  };
  void collectClassBases(Span<Decl*> decls, const std::string& prefix, const SymbolTable& symbols, std::map<std::string, ClassBases>& out)
  {
    for (Decl* d : decls)
    {
      if (d->kind == NodeKind::NamespaceDecl)
      {
        auto* n = static_cast<NamespaceDecl*>(d);
        std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
        collectClassBases(n->decls, prefix + name + "::", symbols, out);
      }
      else if (d->kind == NodeKind::ClassDecl)
      {
        auto* c = static_cast<ClassDecl*>(d);
        if (c->name.empty() || c->name.last().symbol == UINT32_MAX || !c->isDefinition)
          continue;
        std::string key = prefix + nameText(c->name, symbols) + "::";
        ClassBases& entry = out[key];
        entry.enclosing = prefix;
        for (const ClassDecl::Base& b : c->bases)
          if (b.type && b.type->kind == NodeKind::NamedType && !static_cast<NamedType*>(b.type)->name.empty())
            entry.bases.push_back(nameText(static_cast<NamedType*>(b.type)->name, symbols));
        collectClassBases(c->members, key, symbols, out);
      }
      else if (d->kind == NodeKind::UsingAliasDecl)
      {
        // An alias of a class (`template<class T> using IntrusiveListHook = typename
        // IntrusiveListImplementation<T, ...>::Hook;`) is an entry whose one base is that class, so
        // a class deriving from the alias sees the class's names (UpdatableEntity's `next`).
        auto* a = static_cast<UsingAliasDecl*>(d);
        if (a->type && a->type->kind == NodeKind::NamedType && !static_cast<NamedType*>(a->type)->name.empty())
        {
          ClassBases& entry = out[prefix + std::string(symbols.name(a->name)) + "::"];
          entry.enclosing = prefix;
          entry.bases.push_back(nameText(static_cast<NamedType*>(a->type)->name, symbols));
        }
      }
    }
  }
  // Adds to every class's names the names its bases declare (transitively), which an unqualified
  // name in the class finds before a global one: a class deriving from ControllerProperties sees
  // its `TimePoint`.
  void addInheritedNames(ScopeNames& names, const std::map<std::string, ClassBases>& bases)
  {
    // The base's key: looked up from the class's enclosing scope outwards, as C++ finds it.
    auto keyOf = [&](const std::string& base, std::string scope) -> std::string
    {
      std::string written = base.rfind("::", 0) == 0 ? base.substr(2) : base;
      while (true)
      {
        std::string key = scope + written + "::";
        if (names.count(key) || bases.count(key))
          return key;
        if (scope.empty())
          return "";
        // "A::B::" -> "A::", "A::" (or an anonymous namespace's "::") -> "".
        size_t cut = scope.size() > 2 ? scope.rfind("::", scope.size() - 3) : std::string::npos;
        scope = cut == std::string::npos ? std::string() : scope.substr(0, cut + 2);
      }
    };
    std::map<std::string, std::set<std::string>> inherited;
    std::set<std::string> visited;
    std::function<void(const std::string&, std::set<std::string>&)> gather = [&](const std::string& key, std::set<std::string>& into)
    {
      auto it = bases.find(key);
      if (it == bases.end() || !visited.insert(key).second)
        return;
      for (const std::string& base : it->second.bases)
      {
        std::string baseKey = keyOf(base, it->second.enclosing);
        if (baseKey.empty())
          continue;
        auto own = names.find(baseKey);
        if (own != names.end())
          into.insert(own->second.begin(), own->second.end());
        gather(baseKey, into);
      }
    };
    for (const auto& [key, entry] : bases)
    {
      visited.clear();
      gather(key, inherited[key]);
    }
    for (auto& [key, extra] : inherited)
      names[key].insert(extra.begin(), extra.end());
  }

  // --- std::variant: typed unions (`union enum`, design.md 8.5) ---

