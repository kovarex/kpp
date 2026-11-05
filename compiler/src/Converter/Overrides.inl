// Overrides.inl: the override files' members.
// Part of convert.cpp, included where it stood in its anonymous namespace (a file of its own since 2026-11-05, moved without a change).

  // An override file's `class [[members]] Name { ... }`: members that join the converted class Name,
  // whose other members stay as converted (the game's SerialiserHelper with the typed unions' save).
  struct MemberOverride
  {
    std::string members; // the text between the braces
    uint32_t begin = 0, end = 0; // the class's text in the override file, from its `template` or class keyword
    bool used = false;   // a converted class took them
    std::set<std::string> replaces; // the names of members marked `[[replaces]]`: the converted ones of that name go
  };

  // The `class [[members]]` classes of an override file (lexed into tk), by qualified name.
  // The name a `[[replaces]]` member of an override class replaces: a function's (every overload)
  // or a member alias's (`using StringType = ...;`); empty for any other member.
  std::string replaceableName(Decl* member, const SymbolTable& symbols)
  {
    if (member->kind == NodeKind::FunctionDecl)
      return nameText(static_cast<FunctionDecl*>(member)->name, symbols);
    if (member->kind == NodeKind::UsingAliasDecl)
      return std::string(symbols.name(static_cast<UsingAliasDecl*>(member)->name));
    return std::string();
  }

  void collectMemberOverrides(Span<Decl*> decls, const std::string& prefix, const std::vector<Token>& tk, std::string_view text,
                              const SymbolTable& symbols, std::map<std::string, MemberOverride>& out)
  {
    for (Decl* d : decls)
    {
      if (d->kind == NodeKind::NamespaceDecl)
      {
        auto* n = static_cast<NamespaceDecl*>(d);
        std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
        collectMemberOverrides(n->decls, prefix + name + "::", tk, text, symbols, out);
        continue;
      }
      auto* c = d->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(d) : nullptr;
      if (!c || !c->isDefinition || c->name.empty() || c->endToken <= c->token || c->endToken > tk.size())
        continue;
      // The head: from the `template` keyword (or the class keyword) to the body's `{`. (For a
      // template, the keyword before its parameters: a parameter's own `class` is no head.)
      uint32_t head = c->token;
      if (!c->templateParams.empty())
      {
        head = c->templateParams[0]->token;
        while (head > 0 && !tk[head].is(Kw::template_))
          --head;
      }
      else
        while (head > 0 && !tk[head].is(Kw::class_) && !tk[head].is(Kw::struct_) && !tk[head].is(P::Semi) && !tk[head].is(P::RBrace))
          --head;
      uint32_t open = head;
      while (open < c->endToken && !tk[open].is(P::LBrace))
        ++open;
      bool marked = false;
      for (uint32_t k = head; k + 4 < open && !marked; ++k)
        marked = tk[k].is(P::LBracket) && tk[k + 1].is(P::LBracket) && text.substr(tk[k + 2].offset, tk[k + 2].length) == "members" &&
                 tk[k + 3].is(P::RBracket) && tk[k + 4].is(P::RBracket);
      uint32_t close = c->endToken;
      while (close > open && !tk[close - 1].is(P::RBrace))
        --close;
      if (!marked || open >= c->endToken || close <= open + 1)
        continue;
      MemberOverride m;
      m.members = std::string(text.substr(tk[open].offset + 1, tk[close - 1].offset - tk[open].offset - 1));
      m.begin = tk[head].offset;
      uint32_t last = close - 1;
      if (last + 1 < tk.size() && tk[last + 1].is(P::Semi))
        ++last;
      m.end = tk[last].offset + tk[last].length;
      // A member marked `[[replaces]]` (before its template header, type or `using`) takes the
      // place of the converted members of its name, every overload; the others are added.
      for (Decl* member : c->members)
      {
        if (replaceableName(member, symbols).empty() || member->token <= open || member->token >= close)
          continue;
        uint32_t k = member->token;
        while (k > open + 1 && !tk[k - 1].is(P::Semi) && !tk[k - 1].is(P::RBrace) && !tk[k - 1].is(P::LBrace) && !tk[k - 1].is(P::Colon))
          --k;
        for (; k + 4 < member->token; ++k)
          if (tk[k].is(P::LBracket) && tk[k + 1].is(P::LBracket) && text.substr(tk[k + 2].offset, tk[k + 2].length) == "replaces" &&
              tk[k + 3].is(P::RBracket) && tk[k + 4].is(P::RBracket))
          {
            m.replaces.insert(replaceableName(member, symbols));
            break;
          }
      }
      out[prefix + nameText(c->name, symbols)] = std::move(m);
    }
  }

  void collectOverrideKeys(Span<Decl*> decls, const std::string& prefix, const SymbolTable& symbols, OverrideKeys& out,
                           const std::map<std::string, MemberOverride>* memberClasses = nullptr)
  {
    for (Decl* d : decls)
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
        {
          auto* n = static_cast<NamespaceDecl*>(d);
          std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
          collectOverrideKeys(n->decls, prefix + name + "::", symbols, out, memberClasses);
          break;
        }
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          if (isFunctionDefinition(f))
          {
            out.functions.insert(prefix + nameText(f->name, symbols) + "/" + std::to_string(f->params.size()));
            ++out.functionCounts[prefix + nameText(f->name, symbols)];
          }
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (c->isDefinition && !c->name.empty() && !(memberClasses && memberClasses->count(prefix + nameText(c->name, symbols))))
            out.classes.insert(prefix + nameText(c->name, symbols));
          break;
        }
        case NodeKind::VariableDecl: out.variables.insert(prefix + nameText(static_cast<VariableDecl*>(d)->name, symbols)); break;
        default: break;
      }
  }

