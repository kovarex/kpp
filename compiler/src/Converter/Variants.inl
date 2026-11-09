// Variants.inl: std::variant as typed unions.
// Part of convert.cpp, included where it stood in its anonymous namespace (a file of its own since 2026-11-05, moved without a change).

  // A std::variant the program declares. An alias (`using AIState = std::variant<A, B>;`) names
  // its union; a member declared as a std::variant gets a union named after it (`data` ->
  // DataVariant), declared just before it.
  struct VariantAlternative
  {
    std::string spelling; // the C++ type as written, without spaces and a leading `::`
    std::string name;     // the alternative's name in the union
  };
  struct VariantInfo
  {
    Decl* decl = nullptr;  // the UsingAliasDecl, or the member's VariableDecl
    std::string unionName; // AIState, DataVariant
    std::string qualified; // ::SegmentedUnitAI::AIState
    std::vector<VariantAlternative> alternatives;
    std::set<std::string> localTypes; // a block's union: the classes declared in its blocks, which it names as written
  };
  struct Variants
  {
    std::vector<VariantInfo> all;
    std::map<Decl*, size_t> byDecl;
  };

  // Whether a written type is `std::variant<...>`.
  bool isStdVariant(Type* t, const SymbolTable& symbols)
  {
    if (!t || t->kind != NodeKind::NamedType)
      return false;
    const QualifiedName& n = static_cast<NamedType*>(t)->name;
    return n.parts.size() == 2 && n.parts[0].symbol != UINT32_MAX && n.parts[1].symbol != UINT32_MAX &&
           symbols.name(n.parts[0].symbol) == "std" && symbols.name(n.parts[1].symbol) == "variant" && n.parts[1].hasTemplateArgs;
  }

  // The token ranges [first, last) of the arguments of the first `std::variant<...>` in tokens
  // [from, to), and in variantClose the index of its closing `>`.
  std::vector<std::pair<uint32_t, uint32_t>> variantArgumentTokens(const std::vector<Token>& tk, std::string_view text, uint32_t from, uint32_t to,
                                                                   uint32_t& variantStart, uint32_t& variantClose)
  {
    std::vector<std::pair<uint32_t, uint32_t>> args;
    for (uint32_t k = from; k + 3 < to; ++k)
    {
      if (text.substr(tk[k].offset, tk[k].length) != "std" || !tk[k + 1].is(P::ColonColon) ||
          text.substr(tk[k + 2].offset, tk[k + 2].length) != "variant" || !tk[k + 3].is(P::Less))
        continue;
      int depth = 0;
      uint32_t start = k + 4;
      for (uint32_t j = k + 4; j < to; ++j)
      {
        if (tk[j].is(P::Less) || tk[j].is(P::LParen))
          ++depth;
        else if (tk[j].is(P::RParen))
          --depth;
        else if (tk[j].is(P::Greater) || tk[j].is(P::Shr))
        {
          if (depth == 0)
          {
            args.push_back({start, j});
            variantStart = k;
            variantClose = j;
            return args;
          }
          depth -= tk[j].is(P::Shr) ? 2 : 1;
        }
        else if (tk[j].is(P::Comma) && depth == 0)
        {
          args.push_back({start, j});
          start = j + 1;
        }
      }
      return {};
    }
    return {};
  }

  // A type as written, for comparing: no whitespace, no leading `::`.
  std::string normalizedSpelling(std::string_view text)
  {
    std::string s;
    for (char c : text)
      if (!isspace((unsigned char)c))
        s += c;
    if (s.rfind("::", 0) == 0)
      s.erase(0, 2);
    return s;
  }

  // The last component of a spelled type's name, outside template arguments
  // (`SegmentedUnitAI::Patrolling` -> `Patrolling`, `ID<A::B>` -> `ID<A::B>`).
  std::string lastComponent(const std::string& spelling)
  {
    int depth = 0;
    size_t cut = 0;
    for (size_t i = 0; i + 1 < spelling.size(); ++i)
    {
      if (spelling[i] == '<')
        ++depth;
      else if (spelling[i] == '>')
        --depth;
      else if (depth == 0 && spelling[i] == ':' && spelling[i + 1] == ':')
        cut = i + 2;
    }
    return spelling.substr(cut);
  }

  // An alternative's name from its type: the identifiers of the type's last component and its
  // template arguments, the first letter upper case (`IDWithQuality<FluidID>` -> IDWithQualityFluidID,
  // `int` -> Int, `std::string` -> String).
  std::string alternativeName(const std::string& spelling)
  {
    std::string last = lastComponent(spelling), name;
    for (size_t i = 0; i < last.size();)
    {
      if (isalpha((unsigned char)last[i]) || last[i] == '_')
      {
        size_t j = i;
        while (j < last.size() && isIdentChar(last[j]))
          ++j;
        std::string word = last.substr(i, j - i);
        auto mapped = stdNames.find(word);
        if (word != "std" && word != "const")
          name += (i >= 5 && last.compare(i - 5, 5, "std::") == 0 && mapped != stdNames.end()) ? lastComponent(mapped->second) : word;
        i = j;
      }
      else
        ++i;
    }
    if (!name.empty())
      name[0] = (char)toupper((unsigned char)name[0]);
    return name;
  }

  void collectVariants(Span<Decl*> decls, const std::string& prefix, const std::vector<Token>& tk, std::string_view text,
                       const SymbolTable& symbols, Variants& out);
  // A function body's block-local aliases (`using VariantType = std::variant<...>;` in a test):
  // the union is declared in the block, named as written, unqualified.
  void collectLocalVariants(Stmt* s, const std::vector<Token>& tk, std::string_view text, const SymbolTable& symbols, Variants& out,
                            std::set<std::string> localClasses)
  {
    if (!s)
      return;
    switch (s->kind)
    {
      case NodeKind::BlockStmt:
        for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
        {
          // A class declared in the block is in scope for what follows it there.
          if (inner->kind == NodeKind::DeclStmt)
            for (Decl* d : static_cast<DeclStmt*>(inner)->decls)
              if (d->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(d)->name.empty())
                localClasses.insert(nameText(static_cast<ClassDecl*>(d)->name, symbols));
          collectLocalVariants(inner, tk, text, symbols, out, localClasses);
        }
        break;
      case NodeKind::DeclStmt:
      {
        size_t before = out.all.size();
        collectVariants(static_cast<DeclStmt*>(s)->decls, "", tk, text, symbols, out);
        for (size_t i = before; i < out.all.size(); ++i)
        {
          out.all[i].qualified = out.all[i].unionName;
          out.all[i].localTypes = localClasses;
        }
        break;
      }
      case NodeKind::IfStmt:
        collectLocalVariants(static_cast<IfStmt*>(s)->then, tk, text, symbols, out, localClasses);
        collectLocalVariants(static_cast<IfStmt*>(s)->else_, tk, text, symbols, out, localClasses);
        break;
      case NodeKind::ForStmt: collectLocalVariants(static_cast<ForStmt*>(s)->body, tk, text, symbols, out, localClasses); break;
      case NodeKind::RangeForStmt: collectLocalVariants(static_cast<RangeForStmt*>(s)->body, tk, text, symbols, out, localClasses); break;
      case NodeKind::WhileStmt: collectLocalVariants(static_cast<WhileStmt*>(s)->body, tk, text, symbols, out, localClasses); break;
      case NodeKind::DoStmt: collectLocalVariants(static_cast<DoStmt*>(s)->body, tk, text, symbols, out, localClasses); break;
      case NodeKind::SwitchStmt: collectLocalVariants(static_cast<SwitchStmt*>(s)->body, tk, text, symbols, out, localClasses); break;
      default: break;
    }
  }

  // Records the program's std::variant aliases and members (prefix: the enclosing scopes and `::`),
  // and the block-local aliases of its functions' bodies.
  void collectVariants(Span<Decl*> decls, const std::string& prefix, const std::vector<Token>& tk, std::string_view text,
                       const SymbolTable& symbols, Variants& out)
  {
    for (Decl* d : decls)
    {
      Type* type = nullptr;
      std::string unionName;
      if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body)
      {
        collectLocalVariants(static_cast<FunctionDecl*>(d)->body, tk, text, symbols, out, {});
        continue;
      }
      if (d->kind == NodeKind::NamespaceDecl)
      {
        auto* n = static_cast<NamespaceDecl*>(d);
        std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
        collectVariants(n->decls, prefix + name + "::", tk, text, symbols, out);
        continue;
      }
      if (d->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(d)->isDefinition && !static_cast<ClassDecl*>(d)->name.empty())
      {
        collectVariants(static_cast<ClassDecl*>(d)->members, prefix + nameText(static_cast<ClassDecl*>(d)->name, symbols) + "::", tk, text, symbols, out);
        continue;
      }
      if (d->kind == NodeKind::UsingAliasDecl && isStdVariant(static_cast<UsingAliasDecl*>(d)->type, symbols))
      {
        type = static_cast<UsingAliasDecl*>(d)->type;
        unionName = std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name));
      }
      else if (d->kind == NodeKind::VariableDecl && isStdVariant(static_cast<VariableDecl*>(d)->type, symbols) &&
               static_cast<VariableDecl*>(d)->name.parts.size() == 1 && d->parent && d->parent->kind == NodeKind::ClassDecl)
      {
        type = static_cast<VariableDecl*>(d)->type;
        unionName = std::string(symbols.name(static_cast<VariableDecl*>(d)->name.last().symbol));
        unionName[0] = (char)toupper((unsigned char)unionName[0]);
        unionName += "Variant";
      }
      if (!type || d->endToken <= d->token)
        continue;
      uint32_t variantStart = 0, variantClose = 0;
      auto args = variantArgumentTokens(tk, text, type->token, d->endToken, variantStart, variantClose);
      if (args.empty())
        continue;
      VariantInfo info;
      info.decl = d;
      info.unionName = unionName;
      info.qualified = "::" + prefix + unionName;
      std::set<std::string> used;
      for (auto [a0, a1] : args)
      {
        VariantAlternative alternative;
        alternative.spelling = normalizedSpelling(text.substr(tk[a0].offset, tk[a1 - 1].offset + tk[a1 - 1].length - tk[a0].offset));
        alternative.name = alternativeName(alternative.spelling);
        if (alternative.name.empty())
          alternative.name = "Alternative";
        std::string base = alternative.name;
        for (int n = 2; !used.insert(alternative.name).second; ++n)
          alternative.name = base + std::to_string(n);
        info.alternatives.push_back(alternative);
      }
      out.byDecl[d] = out.all.size();
      out.all.push_back(std::move(info));
    }
  }

  bool isFunctionDefinition(FunctionDecl* f) { return f->body != nullptr || (f->flags & (DF_Default | DF_Delete)); }

