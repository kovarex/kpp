// FileConverterDeclarations.inl: FileConverter's members: declarations: classes, functions, variables, variants, std types.
// Part of convert.cpp, included in struct FileConverter's body in Converter/FileConverter.hpp (a file of its own since 2026-11-05, moved without a change).

    static bool isDefinition(FunctionDecl* f) { return f->body != nullptr || (f->flags & (DF_Default | DF_Delete)); }

    // Whether a hand-written override replaces this namespace-scope definition.
    bool overridden(Decl* d, const std::string& prefix)
    {
      if (!overrides)
        return false;
      if (d->kind == NodeKind::FunctionDecl)
      {
        auto* f = static_cast<FunctionDecl*>(d);
        if (!isFunctionDefinition(f))
          return false;
        std::string q = prefix + nameText(f->name, symbols);
        std::string resolved = global.qualifiedName(f); // the namespace the resolver put it in
        size_t cut = resolved.rfind("::");
        std::string alt = (cut == std::string::npos ? std::string() : resolved.substr(0, cut + 2)) + nameText(f->name, symbols);
        for (const std::string& key : {q, alt})
          if (overrides->functions.count(key + "/" + std::to_string(f->params.size())) || overrides->functions.count(key + "/*"))
          {
            replacedFunctions.insert(key);
            return true;
          }
        for (const std::string& c : overrides->classes)
          if (q.size() > c.size() + 2 && q.compare(0, c.size(), c) == 0 && q.compare(c.size(), 2, "::") == 0)
            return true; // a member of a replaced class, defined out of line
        return false;
      }
      if (d->kind == NodeKind::ClassDecl)
      {
        auto* c = static_cast<ClassDecl*>(d);
        return c->isDefinition && !c->name.empty() && overrides->classes.count(prefix + nameText(c->name, symbols));
      }
      if (d->kind == NodeKind::VariableDecl)
      {
        std::string q = prefix + nameText(static_cast<VariableDecl*>(d)->name, symbols);
        if (!overrides->variables.count(q))
          return false;
        replacedVariables.insert(q);
        return true;
      }
      return false;
    }

    void convertDecls(Span<Decl*> decls, bool namespaceScope, const std::string& prefix = "")
    {
      if (!in.c)
        for (Decl* d : decls)
          if (isFormatFunction(d))
            convertFormatFunction(static_cast<FunctionDecl*>(d));
      for (Decl* d : decls)
      {
        if (namespaceScope && prefix.empty() && redeclaresScopedName(d))
        {
          // (ZipPackage.hpp's `struct unz_file_info64_s;` and `using unz_file_info64 = ...;`, written
          // so as not to include unzip.h: they declare minizip's own, now minizip::unz_file_info64)
          removeDecl(d);
          counts.add("forward declarations and aliases of a scoped library's names removed (the library's)");
          continue;
        }
        if (namespaceScope && overridden(d, prefix))
        {
          removeDefinition(d);
          counts.add("replaced by the override file");
          continue;
        }
        if ((d->flags & DF_Friend) && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::FunctionDecl))
        {
          // kpp keeps friend (design.md "struct and class", kovarex 2026-11-01): a friend class or
          // function as the C++ declares it, and a hidden friend, defined in its class, as C++ has it
          // (found by argument-dependent lookup, through a nested enum or class too).
          bool hidden = d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body;
          counts.add(hidden ? "hidden friends kept" : "friend declarations kept");
          if (!hidden)
            continue;
        }
        if (d->kind == NodeKind::DeductionGuideDecl)
        {
          removeDecl(d); // kpp deduces class arguments from constructors
          counts.add("deduction guides removed");
          continue;
        }
        // C++: a source file's names of its own, a `static` at the global namespace's scope and what
        // an anonymous namespace there declares, are `private` (design.md 15: a file's names; not in
        // a named namespace, where private is no kpp, nor in a header, which other files include).
        if (!in.c && namespaceScope && !isHeader() && (d->flags & DF_Static) && prefix.empty() &&
            (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) && cDeclName(d).size() &&
            !(d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->name.parts.size() > 1))
          writePrivate(d);
        if (!in.c && namespaceScope && !isHeader() && prefix == "::" &&
            (d->kind == NodeKind::FunctionDecl || d->kind == NodeKind::VariableDecl || d->kind == NodeKind::ClassDecl ||
             d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl) &&
            !(d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->name.parts.size() > 1))
          writePrivate(d);
        if (namespaceScope && (d->flags & DF_Static) && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) && isHeader() &&
            !cGivenHeader)
        {
          // `static` at namespace scope in a header gives every C++ file that includes it a copy
          // of its own; in kpp a header and its source are one file and a `static` name stays in
          // it, so the other files would not find it (EMPTY_CARGO_STATION_ID): the `static` goes.
          // (Not in a C header given alone: it is the implementation's one file, and its statics
          // are its own, stb_truetype.h's fallback stbrp_init_target and stbrp_pack_rects beside
          // the game's stb_rect_pack.)
          uint32_t start = declStart(d);
          uint32_t first = d->token;
          while (first > 0 && toks[first - 1].offset >= start)
            --first;
          for (uint32_t k = first; k < toks.size(); ++k)
          {
            if (toks[k].is(P::LParen) || toks[k].is(P::Assign) || toks[k].is(P::LBrace) || toks[k].is(P::Semi))
              break;
            if (toks[k].is(Kw::static_))
            {
              uint32_t end = endOffset(k);
              while (end < text.size() && (text[end] == ' ' || text[end] == '\t'))
                ++end;
              remove(toks[k].offset, end - toks[k].offset);
              counts.add("static dropped from a header declaration");
              break;
            }
          }
        }
        if (isGenericVariantCode(d))
        {
          // Code for any std::variant<T...> (index recursion over variant_size_v): a std::variant
          // becomes a typed union, which takes no such code; the game's generic save and load of
          // them are hand-written with alternatives(U) (overrides, `[[members]]` classes).
          removeDecl(d);
          counts.add("generic std::variant code removed");
          continue;
        }
        dropAllocatorParameters(d);
        switch (d->kind)
        {
          case NodeKind::NamespaceDecl:
          {
            auto* n = static_cast<NamespaceDecl*>(d);
            std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
            if (!in.c && name.empty() && prefix.empty() && !isHeader() && !n->isInline && cUnwrapAnonymousNamespace(n))
            {
              // its declarations private, each (convertDecls with the prefix "::")
              convertDecls(n->decls, true, "::");
              break;
            }
            convertDecls(n->decls, true, prefix + name + "::");
            break;
          }
          case NodeKind::ClassDecl:
          {
            auto* c = static_cast<ClassDecl*>(d);
            if (c->isDefinition && packedAt(toks[c->token].offset))
            {
              // The class keyword, after any `template<...>`.
              uint32_t k = c->token;
              if (toks[k].is(Kw::template_) && k + 1 < toks.size() && toks[k + 1].is(P::Less))
              {
                for (int depth = 0; ++k < toks.size();)
                {
                  if (toks[k].is(P::Less))
                    ++depth;
                  else if (toks[k].is(P::Greater) && --depth == 0)
                    break;
                  else if (toks[k].is(P::Shr) && (depth -= 2) <= 0)
                    break;
                }
                ++k;
              }
              if (k < toks.size() && (toks[k].is(Kw::class_) || toks[k].is(Kw::struct_) || toks[k].is(Kw::union_)))
              {
                insert(endOffset(k), " [[packed]]");
                counts.add("#pragma pack(push, 1) classes [[packed]]");
              }
            }
            if (!c->isDefinition)
            {
              Decl* def = global.definitionOf(c);
              // A class template whose primary is only declared, defined through its partial
              // specializations (`template<typename...> struct IndexInTemplateList;`): the
              // declaration is the primary template, which the specializations need.
              bool specializedOnly = def && def != c && def->kind == NodeKind::ClassDecl && !c->templateParams.empty() &&
                                     !static_cast<ClassDecl*>(def)->name.empty() && static_cast<ClassDecl*>(def)->name.last().hasTemplateArgs;
              bool definedInLeftOutFile = def && leftOutFiles && leftOutFiles->count(def->file);
              // (a class ENUM or BITFIELD defines has a definition the resolver does not see: its
              // declaration goes as any other's)
              bool macroDefined = !def && macroClasses && macroClasses->count(global.qualifiedName(c));
              // (a class defined nowhere that no type of the program names, `class Region;` of a class the game
              // removed long ago, goes: by the resolver's names, a word elsewhere is no use of it)
              bool namedNowhere = programNames && (!def || def == c) && !macroDefined && c->templateParams.empty() && !c->name.empty() &&
                                  c->name.last().symbol != UINT32_MAX && !programNames->classes.count(global.qualifiedName(c)) &&
                                  !programNames->unresolved.count(c->name.last().symbol);
              if (namespaceScope && namedNowhere)
              {
                removeDecl(d);
                counts.add("declarations of classes defined nowhere and named by no type removed");
              }
              else if (namespaceScope && opaqueKept && !macroDefined && (!def || def == c || specializedOnly || definedInLeftOutFile) &&
                  opaqueKept->insert(global.qualifiedName(c)).second)
              {
                // No definition anywhere (`namespace TaskBar { class Interface; }`, used only through
                // pointers; the tags only template arguments name, `class ElectricityTag;`): the
                // declaration is the class, an opaque type; kept once in the program. At global scope
                // the name may also be a C library's type (SDL_Cursor, which the C import defines too)
                // or a class a macro defines (BITFIELD): a declaration next to a definition is fine.
                counts.add(specializedOnly ? "primary templates only specialized kept" : "declarations of classes defined nowhere kept");
              }
              else if (namespaceScope)
              {
                removeDecl(d);
                counts.add("forward declarations removed");
              }
              break;
            }
            structAsClass(c);
            if (carriedTemplateDefaults)
              if (auto cd = carriedTemplateDefaults->find(c); cd != carriedTemplateDefaults->end())
                for (auto& [index, value] : cd->second)
                  if (index < c->templateParams.size())
                    if (uint32_t end = templateParameterEnd(c->templateParams[index]->token); end != UINT32_MAX)
                    {
                      insert(endOffset(end - 1), " = " + value);
                      counts.add("default template arguments carried from forward declarations");
                    }
            if (memberOverrides)
              if (auto mo = memberOverrides->find(prefix + nameText(c->name, symbols)); mo != memberOverrides->end() && !mo->second.replaces.empty())
                for (Decl* member : c->members)
                  if (std::string name = replaceableName(member, symbols); !name.empty() && mo->second.replaces.count(name))
                  {
                    removeDecl(member);
                    counts.add("members replaced by hand-written ones");
                  }
            classStack.push_back(c);
            convertDecls(c->members, false, prefix + nameText(c->name, symbols) + "::");
            classStack.pop_back();
            if (memberOverrides)
              if (auto mo = memberOverrides->find(prefix + nameText(c->name, symbols)); mo != memberOverrides->end())
              {
                // An override file's `class [[members]] Name { ... }`: its members join this class,
                // before the closing brace.
                uint32_t close = d->endToken;
                while (close > d->token && !toks[close - 1].is(P::RBrace))
                  --close;
                if (close > d->token)
                {
                  insert(toks[close - 1].offset, "\n  // --- Hand-written members, from the override file ---\n" + mo->second.members + "\n");
                  mo->second.used = true;
                  counts.add("hand-written members added");
                }
              }
            break;
          }
          case NodeKind::EnumDecl:
          {
            auto* e = static_cast<EnumDecl*>(d);
            if (!e->isDefinition && namespaceScope)
            {
              removeDecl(d);
              counts.add("forward declarations removed");
            }
            break;
          }
          case NodeKind::FunctionDecl:
          {
            auto* f = static_cast<FunctionDecl*>(d);
            if (objectOfMacroArguments(f))
              break;
            // (before markInlineBody: `override` goes before the mark on the declaration's `;`, which must
            // stay right before it for the body to move in)
            if (!(f->flags & (DF_Override | DF_Final | DF_Static | DF_Constructor | DF_Destructor | DF_Conversion)) && f->templateParams.empty())
              if (Decl* owner = global.owningClass(f); owner && owner->kind == NodeKind::ClassDecl && overridesBaseVirtual(f, owner))
              {
                if (uint32_t at = overrideToken(f); at != UINT32_MAX)
                {
                  insert(endOffset(at - 1), " override");
                  counts.add("override added");
                }
                else
                  todo(toks[f->token].offset, "a function that overrides a virtual one, whose place for override was not found");
              }
            markInlineBody(f);
            // (an interface header's function, defined in the library's files: listed in its file,
            // the interface's own record of it once the declaration goes)
            if (isScopeHeader() && namespaceScope && !f->body && f->name.parts.size() == 1 && f->name.last().symbol != UINT32_MAX)
              scopeFunctions.push_back(std::string(symbols.name(f->name.last().symbol)));
            if (f->templateParams.empty() && !f->body && !f->name.empty() && f->name.last().hasTemplateArgs && !(f->flags & DF_Specialization))
            {
              removeDecl(d); // an explicit instantiation: templates compile from source in kpp
              counts.add("explicit instantiations removed");
              break;
            }
            if (!isDefinition(f) && namespaceScope && !(f->flags & DF_ExternC))
            {
              Decl* def = global.definitionOf(f);
              if (def && def != f && def->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(def)->body)
              {
                removeDecl(d);
                counts.add("forward declarations removed");
                break;
              }
            }
            if (f->body && carriedTemplateDefaults)
              if (auto cd = carriedTemplateDefaults->find(f); cd != carriedTemplateDefaults->end())
                for (auto& [index, value] : cd->second)
                  if (index < f->templateParams.size())
                    if (uint32_t end = templateParameterEnd(f->templateParams[index]->token); end != UINT32_MAX)
                    {
                      insert(endOffset(end - 1), " = " + value);
                      counts.add("default template arguments carried from forward declarations");
                    }
            if (f->body && carriedDefaults)
              if (auto cd = carriedDefaults->find(f); cd != carriedDefaults->end())
                for (auto& [index, value] : cd->second)
                  if (index < f->params.size())
                    if (uint32_t end = parameterEndIn(toks, f->params[index]->token); end != UINT32_MAX)
                    {
                      insert(toks[end].offset, " = " + value);
                      counts.add("default arguments carried from declarations");
                    }
            if ((f->flags & DF_Constructor) && !(f->flags & (DF_Explicit | DF_Implicit)) && convertingConstructor(f) && !implicitDroppedFor(f))
            {
              insert(toks[f->token].offset, "implicit ");
              counts.add("implicit added");
            }
            if ((f->flags & DF_Conversion) && !(f->flags & (DF_Explicit | DF_Implicit)) && !implicitDroppedFor(f))
            {
              insert(toks[f->token].offset, "implicit ");
              counts.add("implicit added");
            }
            if ((f->flags & (DF_Constructor | DF_Conversion)) && ((f->flags & DF_Explicit) || f->explicitCondition) && f->name.parts.size() == 1)
              writeExplicitByDefault(f);
            if (f->body)
            {
              Decl* owner = global.owningClass(f);
              walkStmt(f->body, owner);
            }
            for (const FunctionDecl::CtorInit& init : f->ctorInits)
              for (Expr* a : init.args)
                walkExpr(a, global.owningClass(f));
            break;
          }
          case NodeKind::VariableDecl:
          {
            auto* v = static_cast<VariableDecl*>(d);
            recordVariantDeclaration(d, true);
            walkExpr(v->init, nullptr);
            directInitialization(v);
            break;
          }
          case NodeKind::UsingAliasDecl:
          {
            recordVariantDeclaration(d, false);
            // `using path = path;` in a namespace that a using-directive opens to the other path
            // (Filesystem.hpp: `using namespace __fs;`): in C++ the alias's own name is not in
            // scope in its type, in kpp it would name the alias itself; the type is qualified
            // with the namespace the directive names (with `::` when there is none).
            auto* a = static_cast<UsingAliasDecl*>(d);
            Type* t = a->type;
            bool ownName = t && t->token + 1 < toks.size() && toks[t->token].getKind() == TokenKind::Identifier &&
                           spelling(t->token) == symbols.name(a->name) && toks[t->token + 1].is(P::Semi) &&
                           !(t->token > 0 && toks[t->token - 1].is(P::ColonColon));
            // Unless the scope declared a class or enum of that name before it (InfinityPipeSettings's
            // `class Filter { ... }; using Filter = Filter;`): the alias then names that member,
            // which MSVC takes as a redeclaration, so it goes (there is no outer one to qualify).
            bool memberOfThatName = false;
            for (Decl* other : decls)
            {
              if (other == d)
                break;
              const QualifiedName* n = other->kind == NodeKind::ClassDecl  ? &static_cast<ClassDecl*>(other)->name
                                       : other->kind == NodeKind::EnumDecl ? &static_cast<EnumDecl*>(other)->name
                                                                           : nullptr;
              if (n && !n->empty() && n->last().symbol == a->name)
                memberOfThatName = true;
            }
            if (ownName && memberOfThatName && t->token >= 3 && spelling(t->token - 3) == "using")
            {
              uint32_t from = toks[t->token - 3].offset, to = endOffset(t->token + 1);
              while (from > 0 && (text[from - 1] == ' ' || text[from - 1] == '\t'))
                --from;
              if (to < text.size() && text[to] == '\r')
                ++to;
              if (to < text.size() && text[to] == '\n')
                ++to;
              remove(from, to - from);
              counts.add("alias of a member of its own name removed");
            }
            else if (ownName)
            {
              std::string qualifier;
              for (Decl* other : decls)
                if (other->kind == NodeKind::UsingDirectiveDecl)
                  qualifier = nameText(static_cast<UsingDirectiveDecl*>(other)->name, symbols);
              insert(toks[t->token].offset, qualifier + "::");
              counts.add("alias of its own name qualified");
            }
            break;
          }
          default: break;
        }
      }
    }

    // A namespace- or class-scope object initialized by parentheses, `Widget w(Size(x));`: at those
    // scopes kpp reads `T f(...)` as a function always (design.md 4, most vexing parse), so the
    // object is written with braces, `Widget w{Size(x)};`, or, where braces would choose another
    // constructor or say something else, with `=`:
    // - a class of the program without an InitializerList constructor (an enumeration too): braces;
    // - a pointer, a reference or a built-in type, one argument: `T* p = x;`, `i32 n = x;`;
    // - anything else (a class with an InitializerList constructor, whose list kpp's braces would
    //   choose, as Vector's, a library class, an alias, a template parameter): `T x = T(args);`.
    // A member function whose body goes inside its class (collectInlineBodies): its declaration's
    // `;` and its out-of-class definition marked for resolveInlineBodies, which moves the body (with
    // a constructor's initializers) to the declaration once the whole output file is written.
    void markInlineBody(FunctionDecl* f)
    {
      if (!inlineBodies || f->endToken == 0 || f->endToken > toks.size())
        return;
      if (auto it = inlineBodies->declarations.find(f); it != inlineBodies->declarations.end())
      {
        if (toks[f->endToken - 1].is(P::Semi))
        {
          insert(toks[f->endToken - 1].offset, "/*kpp:inline:" + std::to_string(it->second) + "*/");
          // (its parameters named as the definition names them, which the body it gets uses;
          // collectInlineBodies checked each can be)
          Decl* defDecl = global.definitionOf(f);
          auto* def = defDecl && defDecl->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defDecl) : nullptr;
          for (size_t i = 0; def && i < f->params.size() && i < def->params.size(); ++i)
          {
            auto* a = static_cast<ParamDecl*>(f->params[i]);
            auto* b = static_cast<ParamDecl*>(def->params[i]);
            if (a->name == b->name)
              continue;
            ParamNamePlace place = paramNamePlace(a, symbols);
            // (marked, `/*kpp:p:ID:NEW*/OLD/*kpp:p*/`: resolveInlineBodies writes NEW where the body
            // moves and OLD where it stays, a definition in an #if group, which may differ per platform)
            std::string open = "/*kpp:p:" + std::to_string(it->second) + ":";
            if (b->name == UINT32_MAX)
            {
              // (unnamed in the definition, as an empty body's or an unused parameter's: unnamed here
              // too, `upgradeModules(ItemStack&, ItemCounts&, InventoryBuffer&) {}`)
              if (place.nameToken != UINT32_MAX && place.nameToken > a->token)
              {
                uint32_t from = endOffset(place.nameToken - 1), to = endOffset(place.nameToken);
                replace(from, to - from, open + "*/" + std::string(text.substr(from, to - from)) + "/*kpp:p*/");
              }
              continue;
            }
            std::string name(symbols.name(b->name));
            if (place.nameToken != UINT32_MAX)
              replace(toks[place.nameToken].offset, toks[place.nameToken].length,
                      open + name + "*/" + std::string(spelling(place.nameToken)) + "/*kpp:p*/");
            else if (place.insertBefore != UINT32_MAX)
              insert(endOffset(place.insertBefore - 1), open + " " + name + "*//*kpp:p*/"); // (after the type's last token)
          }
        }
        return;
      }
      auto it = inlineBodies->definitions.find(f);
      if (it == inlineBodies->definitions.end() || !f->body || f->body->token >= toks.size())
        return;
      // (the body's start: its `{`, or a constructor's `:` before its initializers; the text moved
      // starts where the declarator ends, its line break included)
      uint32_t start = f->body->token;
      if (!f->ctorInits.empty())
      {
        int depth = 0;
        uint32_t k = start;
        while (k > f->token)
        {
          --k;
          if (toks[k].is(P::RParen) || toks[k].is(P::RBrace) || toks[k].is(P::RBracket))
            ++depth;
          else if (toks[k].is(P::LParen) || toks[k].is(P::LBrace) || toks[k].is(P::LBracket))
            --depth;
          else if (depth == 0 && toks[k].is(P::Colon))
            break;
        }
        if (!toks[k].is(P::Colon))
          return; // (not found: the definition stays where it is)
        start = k;
      }
      if (start == 0)
        return;
      // (a definition a macro's use makes, InputAction.cpp's CALL_ON_EVERY_INPUT_ACTION_TYPE: its
      // tokens are the use's, several definitions' alike; it stays where the use is)
      if (f->name.empty())
        return;
      const NamePart& last = f->name.last();
      std::string spelled = last.special == 2 || last.special == 3 ? "::operator"
                            : last.symbol == UINT32_MAX          ? std::string()
                            : (last.special == 1 ? "::~" : "::") + std::string(symbols.name(last.symbol));
      std::string_view written = text.substr(declStart(f), toks[start].offset - declStart(f));
      if (spelled.empty() || written.find(spelled) == std::string_view::npos)
        return;
      std::string id = std::to_string(it->second);
      insert(declStart(f), "/*kpp:def:" + id + "{*/");
      insert(endOffset(start - 1), "/*kpp:body:" + id + "*/");
      insert(endOffset(f->endToken - 1), "/*}kpp:def:" + id + "*/");
    }

    // Whether an explicit instantiation of the class has an effect in kpp, where the whole program is compiled
    // together and what is used is instantiated: the instance's own static data members that run code at
    // startup are built (design.md, codegen's collectInstanceStartupGlobals, which reads no base class). Told
    // as it does, but for a member with no initializer, whose type's construction is codegen's to know: it stays.
    bool instanceRunsAtStartup(const Ty* cls)
    {
      Decl* d = global.definitionOf(cls->decl);
      if (!d || d->kind != NodeKind::ClassDecl)
        return true;
      for (Decl* m : static_cast<ClassDecl*>(d)->members)
      {
        if (m->kind != NodeKind::VariableDecl || !(m->flags & DF_Static) || (m->flags & (DF_Typedef | DF_Constexpr | DF_ThreadLocal)) || m->isTemplate)
          continue;
        Decl* definition = global.definitionOf(m);
        auto* v = definition && definition->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(definition) : static_cast<VariableDecl*>(m);
        if ((v->flags & (DF_Constexpr | DF_ThreadLocal)) || (v->init && v->init->kind == NodeKind::LiteralExpr))
          continue;
        return true;
      }
      return false;
    }

    // `template class X<int>;` lines that do nothing go (kovarex 2026-11-09); one whose class is not known to the
    // converter stays, as does a line a macro's use writes (its tokens are not a line of their own).
    void dropExplicitInstantiationsWithoutEffect()
    {
      if (in.c || !in.parsed)
        return;
      std::map<uint32_t, const Ty*> known;
      for (auto& [cls, token] : global.explicitInstantiationsOf(*in.parsed))
        known[token] = cls;
      for (auto& [named, where] : in.parsed->explicitInstantiations)
      {
        // (the record's token is the class keyword's: `template class X<int>;`)
        uint32_t keyword = named->token;
        auto cls = known.find(keyword);
        if (keyword < 1 || keyword >= toks.size() || !(toks[keyword].is(Kw::class_) || toks[keyword].is(Kw::struct_)) || !toks[keyword - 1].is(Kw::template_) ||
            (keyword >= 2 && toks[keyword - 2].is(Kw::extern_)))
          continue;
        uint32_t end = keyword;
        while (end < toks.size() && !toks[end].is(P::Semi) && !toks[end].is(P::LBrace) && toks[end].getKind() != TokenKind::End)
          ++end;
        if (end >= toks.size() || !toks[end].is(P::Semi))
          continue;
        if (cls == known.end() || instanceRunsAtStartup(cls->second))
        {
          counts.add("explicit instantiations of classes kept: a static data member runs code at startup, or the class is not known here");
          continue;
        }
        removeWholeLine(toks[keyword - 1].offset, endOffset(end));
        counts.add("explicit instantiations of classes removed: no effect in kpp");
      }
    }

    void directInitialization(VariableDecl* v)
    {
      // (C has no `T x(args)` object: its parentheses after a name are a function's, `double
      // trio_nzero(void)`)
      if (in.c || !v->init || v->init->kind != NodeKind::FunctionalCastExpr || !v->type)
        return;
      auto* c = static_cast<FunctionalCastExpr*>(v->init);
      if (c->braced || c->type != v->type || c->token >= toks.size() || v->name.empty())
        return;
      // (the resolver's object from a declaration the parser read as a function, `Color color(Color::Green);`
      // with only names in the parentheses: its initializer starts at the declaration; the
      // parenthesis is the one after the name)
      directInitialization(v->type, v->name, v->token, v->endToken, c->token, c->args.size());
    }

    // A declaration the parser read as a function with unnamed parameters, `const ApplicationVersion
    // ApplicationVersion::currentVersion(VERSION_STRING, BUILD_VERSION, PLATFORM);`, whose names are
    // macros the conversion writes out (string or number macros: values, no types): C++ preprocessed
    // it into an object, and so is it written.
    bool objectOfMacroArguments(FunctionDecl* f)
    {
      if (in.c || f->objectArgs.empty() || f->body || !f->returnType || f->name.empty() || objectMacros.empty())
        return false;
      // (one value is enough: a literal is no parameter's type; the other names, BUILD_VERSION, are
      // then values too)
      bool value = false;
      for (Expr* a : f->objectArgs)
        if (a->kind == NodeKind::NameExpr && a->token < toks.size())
          if (auto macro = objectMacros.find(std::string(spelling(a->token)));
              macro != objectMacros.end() && macroHoldsAt(macro->first, toks[a->token].offset) && !isQualifiedName(macro->second) &&
              !isAliasMacro(macro->first, macro->second))
            value = true;
      if (!value)
        return false;
      directInitialization(f->returnType, f->name, f->token, f->endToken, f->token, f->objectArgs.size());
      return true;
    }

    void directInitialization(Type* type, const QualifiedName& declName, uint32_t declToken, uint32_t declEnd, uint32_t open, size_t argCount)
    {
      if (!toks[open].is(P::LParen))
      {
        uint32_t name = declName.last().symbol;
        uint32_t at = open;
        open = UINT32_MAX;
        for (uint32_t k = declToken; k + 1 < declEnd && k + 1 < toks.size(); ++k)
          if (toks[k].getKind() == TokenKind::Identifier && toks[k].id == name && toks[k + 1].is(P::LParen))
          {
            open = k + 1;
            break;
          }
        if (open == UINT32_MAX)
        {
          todo(toks[at].offset, "a namespace- or class-scope object initialized by parentheses whose parenthesis was not found: written as it was");
          return;
        }
      }
      uint32_t close = open;
      for (int depth = 0; close < toks.size(); ++close)
        if (toks[close].is(P::LParen))
          ++depth;
        else if (toks[close].is(P::RParen) && --depth == 0)
          break;
      if (close >= toks.size())
        return;
      Type* t = type;
      while (t->kind == NodeKind::ConstType)
        t = static_cast<ConstType*>(t)->inner;
      auto braces = [&](const char* what)
      {
        replace(toks[open].offset, 1, "{");
        replace(toks[close].offset, 1, "}");
        counts.add(std::string("namespace- and class-scope objects initialized by parentheses written with braces: ") + what);
      };
      if (t->kind == NodeKind::PointerType || t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType ||
          t->kind == NodeKind::BuiltinType || t->kind == NodeKind::MemberPointerType)
      {
        if (argCount == 1)
        {
          replace(toks[open].offset, 1, " = ");
          remove(toks[close].offset, 1);
          counts.add("namespace- and class-scope objects initialized by parentheses written with =: a pointer, reference or built-in type");
        }
        else
          todo(toks[open].offset, "an object of a pointer, reference or built-in type initialized by parentheses with other than one argument");
        return;
      }
      if (t->kind == NodeKind::NamedType)
      {
        Decl* resolved = static_cast<NamedType*>(t)->resolved;
        if (resolved && resolved->kind == NodeKind::EnumDecl)
          return braces("an enumeration");
        if (resolved && resolved->kind == NodeKind::ClassDecl)
        {
          Decl* def = global.definitionOf(resolved);
          auto* cls = static_cast<ClassDecl*>(def && def->kind == NodeKind::ClassDecl ? def : resolved);
          bool listConstructor = hasListConstructor(cls, symbols);
          if (!listConstructor && cls->isDefinition)
            return braces("a class of the program");
        }
      }
      // `T x = T(args)`: the type written again (its C++ text, mapped as the token pass maps it),
      // without the declaration's own const.
      uint32_t nameStart = open;
      if (nameStart > 0 && toks[nameStart - 1].getKind() == TokenKind::Identifier)
        --nameStart;
      while (nameStart >= 2 && toks[nameStart - 1].is(P::ColonColon) && toks[nameStart - 2].getKind() == TokenKind::Identifier)
        nameStart -= 2;
      uint32_t typeStart = t->token;
      std::string typeText = typeStart < nameStart ? std::string(text.substr(toks[typeStart].offset, endOffset(nameStart - 1) - toks[typeStart].offset)) : "";
      while (!typeText.empty() && isspace((unsigned char)typeText.back()))
        typeText.pop_back();
      if (typeText.size() > 6 && typeText.compare(typeText.size() - 6, 6, " const") == 0)
        typeText.resize(typeText.size() - 6);
      if (typeText.empty() || typeText.find_first_of("&*(") != std::string::npos)
      {
        todo(toks[open].offset, "a namespace- or class-scope object initialized by parentheses whose type could not be written again: written as it was");
        return;
      }
      insert(toks[open].offset, " = " + mapStdText(typeText));
      counts.add("namespace- and class-scope objects initialized by parentheses written `= T(args)` (a library class, an alias, a class with an InitializerList constructor)");
    }

    // A std::variant alias or member of this file (one of `variants`): what run() turns into a union enum.
    void recordVariantDeclaration(Decl* d, bool member)
    {
      if (!variants || d->endToken <= d->token)
        return;
      auto it = variants->byDecl.find(d);
      if (it == variants->byDecl.end())
        return;
      Type* type = member ? static_cast<VariableDecl*>(d)->type : static_cast<UsingAliasDecl*>(d)->type;
      uint32_t variantStart = 0, variantClose = 0;
      auto args = variantArgumentTokens(toks, text, type->token, d->endToken, variantStart, variantClose);
      if (args.empty())
        return;
      VariantDeclaration v;
      v.info = &variants->all[it->second];
      v.member = member;
      v.memberStart = declStart(d);
      if (member)
      {
        v.begin = toks[variantStart].offset;
        v.end = endOffset(variantClose);
      }
      else
      {
        v.begin = declStart(d);
        uint32_t last = d->endToken - 1;
        if (!toks[last].is(P::Semi) && last + 1 < toks.size() && toks[last + 1].is(P::Semi))
          ++last;
        v.end = endOffset(last);
      }
      for (auto [a0, a1] : args)
        v.args.push_back({toks[a0].offset, endOffset(a1 - 1)});
      variantDeclarations.push_back(std::move(v));
      counts.add("std::variant to union enum");
    }

    // A std::variant's construction `V(...)` as the typed union's from a tag (design.md 8.5,
    // `U(U::Tag, args...)`): `V(std::in_place_index<I>, args...)` names the I-th alternative,
    // `V(std::in_place_type<T>, args...)` T's; and the C++ converting constructions the union's
    // from-a-value does not make, where std::variant selects an alternative by conversion: a
    // string literal (the std::string alternative) and std::nullopt (the std::optional one), when
    // one alternative is of that kind. Any other construction stays as written (from a value of
    // one alternative's type; kpp rejects one it cannot decide).
    // (A cast `V(...)` or, in a function body, a call of the alias's name: the parse takes a name
    // it does not know as a type for a function.)
    void variantConstruction(const VariantInfo& info, uint32_t typeToken, Span<Expr*> args)
    {
      if (args.empty())
        return;
      uint32_t open = typeToken;
      while (open < toks.size() && !toks[open].is(P::LParen))
        ++open;
      if (open >= toks.size())
        return;
      std::vector<std::pair<uint32_t, uint32_t>> ranges = argumentRanges(open + 1);
      if (ranges.size() != args.size())
        return;
      auto [first, end] = ranges[0];
      // `std::in_place_index<I>` / `std::in_place_type<T>` (the `std::` optional).
      uint32_t k = first;
      if (k + 2 < end && spelling(k) == "std" && toks[k + 1].is(P::ColonColon))
        k += 2;
      std::string_view marker = k < end ? spelling(k) : std::string_view();
      if ((marker == "in_place_index" || marker == "in_place_type") && k + 2 < end && toks[k + 1].is(P::Less) && toks[end - 1].is(P::Greater))
      {
        std::string argument = normalizedSpelling(text.substr(toks[k + 2].offset, toks[end - 1].offset - toks[k + 2].offset));
        const VariantAlternative* chosen = nullptr;
        if (marker == "in_place_index")
        {
          size_t index = size_t(atoi(argument.c_str()));
          chosen = index < info.alternatives.size() ? &info.alternatives[index] : nullptr;
        }
        else
        {
          int matches = 0;
          for (const VariantAlternative& a : info.alternatives)
            if (a.spelling == argument)
            {
              chosen = &a;
              ++matches;
            }
          if (matches != 1)
            chosen = nullptr;
        }
        if (chosen)
        {
          replace(toks[first].offset, endOffset(end - 1) - toks[first].offset, info.qualified + "::" + chosen->name);
          counts.add("std::variant constructions in an alternative written U(U::Tag, ...)");
        }
        return;
      }
      if (args.size() != 1)
        return;
      Expr* argument = args[0];
      bool literal = argument->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(argument)->literalKind == TokenKind::String;
      bool nullopt = end - first <= 3 && spelling(end - 1) == "nullopt";
      if (!literal && !nullopt)
        return;
      const VariantAlternative* chosen = nullptr;
      int matches = 0;
      for (const VariantAlternative& a : info.alternatives)
        if ((literal && (a.spelling == "std::string" || a.spelling == "String")) || (nullopt && a.spelling.rfind("std::optional<", 0) == 0))
        {
          chosen = &a;
          ++matches;
        }
      if (matches != 1)
        return;
      // In place of the call's `(`: an insert at the argument would be taken by the argument's own
      // rewrite starting there (`std::nullopt`).
      replace(toks[open].offset, toks[open].length, "(" + info.qualified + "::" + chosen->name + ", ");
      counts.add("std::variant converting constructions written U(U::Tag, value)");
    }

    // The variant an expression is declared as: a variable, parameter or member whose type is one of
    // the variant aliases (or a variant member itself), or a call of a function returning one.
    const VariantInfo* variantOfType(Type* t)
    {
      while (t && (t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType || t->kind == NodeKind::ConstType))
        t = t->kind == NodeKind::LRefType ? static_cast<LRefType*>(t)->referenced
          : t->kind == NodeKind::RRefType ? static_cast<RRefType*>(t)->referenced
                                          : static_cast<ConstType*>(t)->inner;
      if (!t || t->kind != NodeKind::NamedType || !static_cast<NamedType*>(t)->resolved)
        return nullptr;
      auto it = variants->byDecl.find(static_cast<NamedType*>(t)->resolved);
      return it == variants->byDecl.end() ? nullptr : &variants->all[it->second];
    }
    // An argument that is a variable of a GUID type (GUID, IID, CLSID, WIC's pixel format GUID), by
    // name: C++ passes it to a REFGUID-like parameter, a reference, which C's (and so the import's
    // and the COM classes') is a pointer to it.
    bool isGuidVariable(Expr* e) const
    {
      Decl* d = e && e->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(e)->resolved : nullptr;
      Type* t = !d ? nullptr : d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d)->type
              : d->kind == NodeKind::ParamDecl                   ? static_cast<ParamDecl*>(d)->type
                                                                 : nullptr;
      while (t && t->kind == NodeKind::ConstType)
        t = static_cast<ConstType*>(t)->inner;
      if (!t || t->kind != NodeKind::NamedType || static_cast<NamedType*>(t)->name.parts.size() != 1)
        return false;
      uint32_t symbol = static_cast<NamedType*>(t)->name.parts[0].symbol;
      if (symbol == UINT32_MAX)
        return false;
      std::string_view name = symbols.name(symbol);
      return name == "GUID" || name == "IID" || name == "CLSID" || name == "WICPixelFormatGUID";
    }
    // `output << this->tagsByTagNumber.size()`: a std container's size() is a size_t in C++ and an
    // int in kpp (docs/stdlib/common.md), which a serialiser's operator<< writes in 4 bytes instead
    // of 8: the game's complete-mode saves (its CRCs, desync reports) came out other than the C++
    // build's (TagManager::save, 4 bytes short per force). Written `size_t(...)` where it is the
    // right operand of a <<, the place where C++ code sends a size on by its type (a printf-style
    // argument is widenFormatArguments's, by its format; the game's
    // saveContainerSize and saveSpaceOptimized take it as a FullType). length() and count(k) alike,
    // and wherever the declarations say the container's type (typeOfExpr): CustomGui::save's
    // `item.second.size()` of a map's std::set put the heuristic CRC 4 bytes short per mod.
    // A std container, written so or through an alias of one, or a class derived from one
    // (ScheduleRecords : std::vector<ScheduleRecord>): whose size() is the library's int.
    static const std::set<std::string_view>& stdContainerNames()
    {
      static const std::set<std::string_view> containers = {
        "vector", "deque", "list", "map", "multimap", "set", "multiset", "unordered_map", "unordered_multimap", "unordered_set",
        "unordered_multiset", "string", "wstring", "u16string", "u32string", "string_view", "wstring_view", "array", "bitset", "span",
        "queue", "stack", "priority_queue"};
      return containers;
    }
    bool isStdContainerType(Type* type)
    {
      NamedType* named = stdTemplateOf(type, 0);
      return named && stdContainerNames().count(stdTemplateName(named)) != 0;
    }
    // The std container (or std::pair) a type names, written so, through aliases or as a base class:
    // the NamedType whose last part carries its template arguments. Null for any other type.
    NamedType* stdTemplateOf(Type* type, int depth)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType || type->kind == NodeKind::RRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner
             : type->kind == NodeKind::LRefType  ? static_cast<LRefType*>(type)->referenced
                                                 : static_cast<RRefType*>(type)->referenced;
      if (!type || type->kind != NodeKind::NamedType || depth > 8)
        return nullptr;
      auto* named = static_cast<NamedType*>(type);
      const QualifiedName& name = named->name;
      if (name.parts.size() == 2 && name.parts[0].symbol != UINT32_MAX && name.parts[1].symbol != UINT32_MAX &&
          symbols.name(name.parts[0].symbol) == "std")
        return named;
      Decl* d = named->resolved;
      if (!d)
        return nullptr;
      if (d->kind == NodeKind::UsingAliasDecl)
        return stdTemplateOf(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return stdTemplateOf(static_cast<VariableDecl*>(d)->type, depth + 1);
      if (d->kind == NodeKind::ClassDecl)
        for (const ClassDecl::Base& b : static_cast<ClassDecl*>(d)->bases)
          if (NamedType* base = stdTemplateOf(b.type, depth + 1))
            return base;
      return nullptr;
    }
    std::string_view stdTemplateName(NamedType* named) { return symbols.name(named->name.parts[1].symbol); }
    Type* stdTemplateArg(NamedType* named, uint32_t i)
    {
      const NamePart& part = named->name.parts[1];
      Node* arg = i < part.templateArgs.size() ? part.templateArgs[i] : nullptr;
      return arg && arg->kind <= NodeKind::PackType ? static_cast<Type*>(arg) : nullptr;
    }
    // What an expression is, as far as the declarations say: a type, or a std::pair's (a map element's)
    // two types; an iterator's element (begin(), find()) is the same with `iterator` set. The type of
    // an auto variable is its initialiser's or, for a range-for's, its range's element.
    struct SeenType
    {
      bool known() const { return type || first; }

      Type* type = nullptr;
      Type* first = nullptr;
      Type* second = nullptr;
      bool iterator = false;
    };
    std::map<Decl*, Expr*> rangeForRanges; // a range-for's variable: its range
    // A std container's element: a map's key and value pair, an array's or a sequence's T.
    SeenType elementOf(const SeenType& of)
    {
      SeenType element;
      if (!of.type)
        return element;
      Type* t = of.type;
      while (t && (t->kind == NodeKind::ConstType || t->kind == NodeKind::LRefType))
        t = t->kind == NodeKind::ConstType ? static_cast<ConstType*>(t)->inner : static_cast<LRefType*>(t)->referenced;
      if (t && t->kind == NodeKind::ArrayType)
      {
        element.type = static_cast<ArrayType*>(t)->element;
        return element;
      }
      NamedType* named = stdTemplateOf(t, 0);
      if (!named)
        return element;
      std::string_view name = stdTemplateName(named);
      if (name == "map" || name == "multimap" || name == "unordered_map" || name == "unordered_multimap")
      {
        element.first = stdTemplateArg(named, 0);
        element.second = stdTemplateArg(named, 1);
      }
      else if (name == "vector" || name == "deque" || name == "list" || name == "set" || name == "multiset" || name == "unordered_set" ||
               name == "unordered_multiset" || name == "array" || name == "span")
        element.type = stdTemplateArg(named, 0);
      return element;
    }
    SeenType typeOfDecl(Decl* d, int depth)
    {
      SeenType seen;
      if (!d || depth > 16)
        return seen;
      Type* type = d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d)->type
                 : d->kind == NodeKind::ParamDecl    ? static_cast<ParamDecl*>(d)->type
                                                     : nullptr;
      Type* t = type;
      while (t && (t->kind == NodeKind::ConstType || t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType))
        t = t->kind == NodeKind::ConstType ? static_cast<ConstType*>(t)->inner
          : t->kind == NodeKind::LRefType  ? static_cast<LRefType*>(t)->referenced
                                           : static_cast<RRefType*>(t)->referenced;
      if (!t || t->kind != NodeKind::AutoType)
      {
        seen.type = type;
        return seen;
      }
      if (auto it = rangeForRanges.find(d); it != rangeForRanges.end())
        return elementOf(typeOfExpr(it->second, depth + 1));
      if (d->kind == NodeKind::VariableDecl)
        if (Expr* init = static_cast<VariableDecl*>(d)->init)
          return typeOfExpr(init, depth + 1);
      return seen;
    }
    SeenType typeOfExpr(Expr* e, int depth)
    {
      SeenType seen;
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e || depth > 16)
        return seen;
      switch (e->kind)
      {
        case NodeKind::NameExpr: return typeOfDecl(static_cast<NameExpr*>(e)->resolved, depth + 1);
        case NodeKind::MemberExpr:
        {
          auto* member = static_cast<MemberExpr*>(e);
          if (member->resolved)
            return typeOfDecl(member->resolved, depth + 1);
          if (member->member.parts.size() != 1 || member->member.parts[0].symbol == UINT32_MAX)
            return seen;
          // `item.second` of a map element, `it->first` of a map iterator, `pair.first` of a std::pair.
          SeenType base = typeOfExpr(member->base, depth + 1);
          if ((member->op == P::Arrow) != base.iterator)
            return seen;
          if (!base.first && base.type)
            if (NamedType* named = stdTemplateOf(base.type, 0); named && stdTemplateName(named) == "pair")
            {
              base.first = stdTemplateArg(named, 0);
              base.second = stdTemplateArg(named, 1);
            }
          std::string_view name = symbols.name(member->member.parts[0].symbol);
          seen.type = name == "first" ? base.first : name == "second" ? base.second : nullptr;
          return seen;
        }
        case NodeKind::IndexExpr:
        {
          auto* index = static_cast<IndexExpr*>(e);
          SeenType base = typeOfExpr(index->base, depth + 1);
          SeenType element = elementOf(base);
          if (element.first) // a map's [] is its value
            seen.type = element.second;
          else
            seen = element;
          return seen;
        }
        case NodeKind::UnaryExpr:
        {
          auto* unary = static_cast<UnaryExpr*>(e);
          if (P(unary->op) != P::Star)
            return seen;
          SeenType operand = typeOfExpr(unary->operand, depth + 1);
          if (!operand.iterator)
            return seen;
          operand.iterator = false;
          return operand;
        }
        case NodeKind::CallExpr:
        {
          // A std container's front() / back() / at(i): its element; begin() / find(k): an iterator to one.
          auto* call = static_cast<CallExpr*>(e);
          if (call->callee->kind != NodeKind::MemberExpr)
            return seen;
          auto* member = static_cast<MemberExpr*>(call->callee);
          if (member->op == P::Arrow || member->member.parts.size() != 1 || member->member.parts[0].symbol == UINT32_MAX)
            return seen;
          std::string_view name = symbols.name(member->member.parts[0].symbol);
          bool access = name == "front" || name == "back" || name == "at";
          bool iterator = name == "begin" || name == "cbegin" || name == "find" || name == "lower_bound" || name == "upper_bound";
          if (!access && !iterator)
            return seen;
          SeenType base = typeOfExpr(member->base, depth + 1);
          if (!base.type || base.iterator || !stdTemplateOf(base.type, 0))
            return seen;
          seen = elementOf(base);
          if (name == "at" && seen.first)
            seen = SeenType{seen.second};
          seen.iterator = iterator;
          return seen;
        }
        default: return seen;
      }
    }
    // A class's member type alias (`using size_type = ...`, `typedef ... size_type`) named symbol.
    Type* memberAlias(ClassDecl* c, uint32_t symbol)
    {
      for (Decl* m : c->members)
        if (m->kind == NodeKind::UsingAliasDecl && static_cast<UsingAliasDecl*>(m)->name == symbol)
          return static_cast<UsingAliasDecl*>(m)->type;
        else if (m->kind == NodeKind::VariableDecl && (m->flags & DF_Typedef) && static_cast<VariableDecl*>(m)->name.parts.size() == 1 &&
                 static_cast<VariableDecl*>(m)->name.parts[0].symbol == symbol)
          return static_cast<VariableDecl*>(m)->type;
      return nullptr;
    }
    // A std container's size_type, as class c names it: `std::vector<T>::size_type`,
    // `container_type::size_type` of a member alias of one, or a member alias of either (FlatSet's
    // `using size_type = typename container_type::size_type;`).
    bool isStdSizeType(Type* type, ClassDecl* c, int depth)
    {
      if (!type || type->kind != NodeKind::NamedType || depth > 8)
        return false;
      const QualifiedName& name = static_cast<NamedType*>(type)->name;
      for (const NamePart& part : name.parts)
        if (part.symbol == UINT32_MAX)
          return false;
      if (name.parts.size() == 1)
        return isStdSizeType(memberAlias(c, name.parts[0].symbol), c, depth + 1);
      if (symbols.name(name.last().symbol) != "size_type")
        return false;
      if (name.parts.size() == 3 && symbols.name(name.parts[0].symbol) == "std")
        return stdContainerNames().count(symbols.name(name.parts[1].symbol)) != 0;
      return name.parts.size() == 2 && isStdContainerType(memberAlias(c, name.parts[0].symbol));
    }
    // The game's class whose size() (or length(), count()) returns its std container's size_type: a
    // size_t in C++, the library's int in kpp, as a std container's own.
    bool returnsStdSizeType(Type* type, std::string_view function)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner : static_cast<LRefType*>(type)->referenced;
      Decl* d = type && type->kind == NodeKind::NamedType ? static_cast<NamedType*>(type)->resolved : nullptr;
      for (int depth = 0; d && d->kind == NodeKind::UsingAliasDecl && depth < 8; ++depth)
      {
        Type* aliased = static_cast<UsingAliasDecl*>(d)->type;
        d = aliased && aliased->kind == NodeKind::NamedType ? static_cast<NamedType*>(aliased)->resolved : nullptr;
      }
      if (!d || d->kind != NodeKind::ClassDecl)
        return false;
      auto* c = static_cast<ClassDecl*>(d);
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl)
        {
          auto* f = static_cast<FunctionDecl*>(m);
          if (f->name.parts.size() == 1 && f->name.parts[0].symbol != UINT32_MAX && symbols.name(f->name.parts[0].symbol) == function)
            return isStdSizeType(f->returnType, c, 0);
        }
      return false;
    }
    // An expression's type for the std::string rewrites: a cast's own type (`std::string(name) += '_'`),
    // a call's function's return type, else what the declarations say.
    Type* plainTypeOf(Expr* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e)
        return nullptr;
      if (e->kind == NodeKind::CastExpr)
        return static_cast<CastExpr*>(e)->type;
      if (e->kind == NodeKind::NamedCastExpr)
        return static_cast<NamedCastExpr*>(e)->type;
      if (e->kind == NodeKind::FunctionalCastExpr)
        return static_cast<FunctionalCastExpr*>(e)->type;
      if (e->kind == NodeKind::CallExpr)
      {
        Expr* callee = static_cast<CallExpr*>(e)->callee;
        Decl* f = callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(callee)->resolved
                : callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                       : nullptr;
        if (f && f->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(f)->returnType)
          return static_cast<FunctionDecl*>(f)->returnType;
      }
      return typeOfExpr(e, 0).type;
    }
    // Whether a type is C++'s std::string (or wstring, u16string, u32string, basic_string, or an alias of
    // one): kpp's String and WString, which append with `<<` (design.md 14: String has no +=).
    bool isStdStringType(Type* type, int depth = 0)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType || type->kind == NodeKind::RRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner
             : type->kind == NodeKind::LRefType  ? static_cast<LRefType*>(type)->referenced
                                                 : static_cast<RRefType*>(type)->referenced;
      if (!type || depth > 8 || type->kind != NodeKind::NamedType)
        return false;
      auto* named = static_cast<NamedType*>(type);
      const QualifiedName& name = named->name;
      for (const NamePart& part : name.parts)
        if (part.symbol == UINT32_MAX)
          return false;
      if (name.parts.size() == 2 && symbols.name(name.parts[0].symbol) == "std")
      {
        static const std::set<std::string_view> strings = {"string", "wstring", "u16string", "u32string", "basic_string"};
        if (strings.count(symbols.name(name.parts[1].symbol)))
          return true;
      }
      Decl* d = named->resolved;
      if (d && d->kind == NodeKind::UsingAliasDecl)
        return isStdStringType(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return isStdStringType(static_cast<VariableDecl*>(d)->type, depth + 1);
      return false;
    }
    // Whether an expression is a std::string: of such a type, a temporary made by naming the type
    // (`std::string(name) += '_'`, a call of the class or an alias of it), or a `+=` on one, which
    // returns it (`(key += '_') += name`).
    bool isStdStringExpr(Expr* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e)
        return false;
      if (e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->isKeyword &&
          P(static_cast<BinaryExpr*>(e)->op) == P::PlusAssign)
        return isStdStringExpr(static_cast<BinaryExpr*>(e)->lhs);
      // (`std::string("curl_multi_perform failed - ") + curl_multi_strerror(code)`)
      if (e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->isKeyword && P(static_cast<BinaryExpr*>(e)->op) == P::Plus)
        return isStdStringExpr(static_cast<BinaryExpr*>(e)->lhs) || isStdStringExpr(static_cast<BinaryExpr*>(e)->rhs);
      if (e->kind == NodeKind::CallExpr && static_cast<CallExpr*>(e)->callee->kind == NodeKind::NameExpr)
      {
        auto* callee = static_cast<NameExpr*>(static_cast<CallExpr*>(e)->callee);
        if (!callee->resolved || callee->resolved->kind != NodeKind::FunctionDecl)
        {
          NamedType named{};
          named.kind = NodeKind::NamedType;
          named.name = callee->name;
          named.resolved = callee->resolved;
          if (isStdStringType(&named))
            return true;
        }
      }
      return isStdStringType(plainTypeOf(e));
    }
    // Whether a type is an integer other than char, which C++ appends to a std::string as one char and
    // kpp's `<<` as its digits (unsigned char, uint8_t, int...).
    bool isNonCharInteger(Type* type, int depth = 0)
    {
      while (type && type->kind == NodeKind::ConstType)
        type = static_cast<ConstType*>(type)->inner;
      if (!type || depth > 8)
        return false;
      if (type->kind == NodeKind::BuiltinType)
      {
        auto* b = static_cast<BuiltinType*>(type);
        if (b->keyword == Kw::char_)
          return b->isUnsigned || b->isSigned;
        return b->keyword == Kw::int_ || b->keyword == Kw::short_ || b->keyword == Kw::long_ || b->keyword == Kw::unsigned_ ||
               b->keyword == Kw::signed_ || b->keyword == Kw::bool_;
      }
      if (type->kind != NodeKind::NamedType)
        return false;
      auto* named = static_cast<NamedType*>(type);
      if (!named->name.parts.empty() && named->name.last().symbol != UINT32_MAX)
      {
        static const std::set<std::string_view> integers = {"uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t", "uint64_t",
                                                            "int64_t", "size_t", "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64"};
        if (integers.count(symbols.name(named->name.last().symbol)))
          return true;
      }
      Decl* d = named->resolved;
      if (d && d->kind == NodeKind::UsingAliasDecl)
        return isNonCharInteger(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return isNonCharInteger(static_cast<VariableDecl*>(d)->type, depth + 1);
      return false;
    }
    // A std container's iterator type, as the code at owner names it: `std::vector<T>::iterator`, or
    // `WidgetArray::iterator` where WidgetArray is an alias of one in owner or a class or namespace
    // around it (Agui's visible_iterator). Its operator* and operator-> are a pointer's in kpp.
    bool isStdIteratorType(Type* type, Decl* owner)
    {
      while (type && type->kind == NodeKind::ConstType)
        type = static_cast<ConstType*>(type)->inner;
      if (!type || type->kind != NodeKind::NamedType)
        return false;
      const QualifiedName& name = static_cast<NamedType*>(type)->name;
      for (const NamePart& part : name.parts)
        if (part.symbol == UINT32_MAX)
          return false;
      std::string_view last = symbols.name(name.last().symbol);
      if (last != "iterator" && last != "const_iterator" && last != "reverse_iterator" && last != "const_reverse_iterator")
        return false;
      if (name.parts.size() == 3 && symbols.name(name.parts[0].symbol) == "std")
        return stdContainerNames().count(symbols.name(name.parts[1].symbol)) != 0;
      if (name.parts.size() != 2)
        return false;
      for (Decl* d = owner; d; d = d->parent)
        if (d->kind == NodeKind::ClassDecl)
        {
          if (Type* alias = memberAlias(static_cast<ClassDecl*>(d), name.parts[0].symbol))
            return isStdContainerType(alias);
        }
        else if (d->kind == NodeKind::NamespaceDecl)
          for (Decl* member : static_cast<NamespaceDecl*>(d)->decls)
            if (member->kind == NodeKind::UsingAliasDecl && static_cast<UsingAliasDecl*>(member)->name == name.parts[0].symbol)
              return isStdContainerType(static_cast<UsingAliasDecl*>(member)->type);
      return false;
    }
    // Whether e is `x.size()`, `x.length()` or `x.count(k)` of a std container: a size_t in C++
    // (64 bits), whatever the library's ContainerSize is.
    bool isStdSizeCall(Expr* e)
    {
      if (!e || e->kind != NodeKind::CallExpr)
        return false;
      auto* call = static_cast<CallExpr*>(e);
      if (call->callee->kind != NodeKind::MemberExpr)
        return false;
      auto* member = static_cast<MemberExpr*>(call->callee);
      if (member->member.parts.size() != 1 || member->member.parts[0].symbol == UINT32_MAX)
        return false;
      std::string_view name = symbols.name(member->member.parts[0].symbol);
      if (!(call->args.size() == 0 && (name == "size" || name == "length")) && !(call->args.size() == 1 && name == "count"))
        return false;
      Expr* base = member->base;
      while (base && base->kind == NodeKind::ParenExpr)
        base = static_cast<ParenExpr*>(base)->inner;
      SeenType type = typeOfExpr(base, 0);
      return (member->op == P::Arrow) == type.iterator && (isStdContainerType(type.type) || returnsStdSizeType(type.type, name));
    }
    // `output << x.size()`, `x.length()`, `x.count(k)` of a std container: written size_t(...).
    void sizeAsSizeT(Expr* e)
    {
      if (!isStdSizeCall(e))
        return;
      auto* call = static_cast<CallExpr*>(e);
      Expr* base = static_cast<MemberExpr*>(call->callee)->base;
      while (base && base->kind == NodeKind::ParenExpr)
        base = static_cast<ParenExpr*>(base)->inner;
      // The call's first token is its base's leftmost one (a CallExpr's own token is its parenthesis),
      // its last the `)` that closes that parenthesis.
      Expr* leftmost = base;
      while (leftmost)
        if (leftmost->kind == NodeKind::MemberExpr)
          leftmost = static_cast<MemberExpr*>(leftmost)->base;
        else if (leftmost->kind == NodeKind::IndexExpr)
          leftmost = static_cast<IndexExpr*>(leftmost)->base;
        else if (leftmost->kind == NodeKind::CallExpr)
          leftmost = static_cast<CallExpr*>(leftmost)->callee;
        else
          break;
      if (!leftmost)
        return;
      uint32_t first = leftmost->token;
      uint32_t close = call->token;
      if (close >= toks.size() || !toks[close].is(P::LParen))
        return;
      for (int depth = 0; close < toks.size(); ++close)
        if (toks[close].is(P::LParen))
          ++depth;
        else if (toks[close].is(P::RParen) && --depth == 0)
          break;
      if (close >= toks.size())
        return;
      // The closing one replaces the call's own `)`: an insert after it would fall where a macro's
      // rewrite replaces the macro's `)` (`LOG_INFO("%zu", v.size())`), which takes it.
      insert(toks[first].offset, "size_t(");
      replace(toks[close].offset, toks[close].length, "))");
      counts.add("std container sizes written size_t");
    }
    const VariantInfo* variantOfExpr(Expr* e)
    {
      if (!variants || !e)
        return nullptr;
      while (e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (e->kind == NodeKind::UnaryExpr && P(static_cast<UnaryExpr*>(e)->op) == P::Amp)
        return variantOfExpr(static_cast<UnaryExpr*>(e)->operand); // get_if<T>(&x)
      Decl* d = e->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(e)->resolved
              : e->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(e)->resolved
                                                : nullptr;
      if (e->kind == NodeKind::CallExpr)
      {
        Expr* callee = static_cast<CallExpr*>(e)->callee;
        Decl* f = callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(callee)->resolved
                : callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                       : nullptr;
        return f && f->kind == NodeKind::FunctionDecl ? variantOfType(static_cast<FunctionDecl*>(f)->returnType) : nullptr;
      }
      if (!d)
        return nullptr;
      if (auto it = variants->byDecl.find(d); it != variants->byDecl.end())
        return &variants->all[it->second]; // a member declared as std::variant<...>
      if (d->kind == NodeKind::VariableDecl)
        return variantOfType(static_cast<VariableDecl*>(d)->type);
      if (d->kind == NodeKind::ParamDecl)
        return variantOfType(static_cast<ParamDecl*>(d)->type);
      return nullptr;
    }

    // `std::get<T>(x)` and its relatives at token i (`std`): recorded with the call's arguments; run()
    // rewrites it when it knows the variant. std::get<0>(tuple) and the like are no variant uses.
    void recordVariantUse(const std::string& name, uint32_t i)
    {
      VariantUse use;
      use.kind = name;
      use.begin = toks[i].offset;
      use.calleeToken = i;
      uint32_t open = i + 3;
      if (name != "visit")
      {
        if (!toks[open].is(P::Less))
          return;
        uint32_t close = open + 1;
        for (int depth = 0; close < toks.size(); ++close)
        {
          if (toks[close].is(P::Less))
            ++depth;
          else if (toks[close].is(P::Greater) || toks[close].is(P::Shr))
          {
            if (depth == 0)
              break;
            depth -= toks[close].is(P::Shr) ? 2 : 1;
            if (depth < 0)
              break;
          }
          else if (toks[close].is(P::Semi))
            return;
        }
        if (close >= toks.size() || close == open + 1)
          return;
        use.typeSpelling = normalizedSpelling(text.substr(toks[open + 1].offset, endOffset(close - 1) - toks[open + 1].offset));
        use.typeBegin = toks[open + 1].offset;
        use.typeEnd = endOffset(close - 1);
        bool anyAlternative = false;
        for (const VariantInfo& v : variants->all)
          anyAlternative = anyAlternative || alternativeFor(v, use.typeSpelling);
        // `std::get<IDType>(this->extraData)` in a template: no alternative is spelled IDType, and
        // the union's generic accessors serve it once the argument's variant is known (run()).
        use.noAlternative = !anyAlternative;
        open = close + 1;
      }
      if (open >= toks.size() || !toks[open].is(P::LParen))
        return;
      uint32_t argStart = open + 1, k = open + 1;
      for (int depth = 0; k < toks.size(); ++k)
      {
        const Token& tk = toks[k];
        if (tk.is(P::LParen) || tk.is(P::LBracket) || tk.is(P::LBrace))
          ++depth;
        else if (tk.is(P::RParen) || tk.is(P::RBracket) || tk.is(P::RBrace))
        {
          if (depth-- == 0)
            break;
        }
        else if (tk.is(P::Comma) && depth == 0)
        {
          use.args.push_back({toks[argStart].offset, endOffset(k - 1)});
          argStart = k + 1;
        }
      }
      if (k >= toks.size() || !toks[k].is(P::RParen) || k == argStart)
        return;
      use.args.push_back({toks[argStart].offset, endOffset(k - 1)});
      use.end = endOffset(k);
      variantUses.push_back(std::move(use));
    }

    // An alternative's type as the union enum lists it. Inside the union, its alternatives' names
    // come first, so a name in the type spelled like one of them (`Patrolling` next to the
    // alternative Patrolling, `EntityWithQualityID` in `Pair<EntityWithQualityID, ...>`) is written
    // qualified: by the innermost scope at offset that declares it, or as global.
    std::string qualifiedAlternativeType(const std::string& type, const VariantInfo& info, uint32_t offset) const
    {
      std::string out;
      for (size_t i = 0; i < type.size();)
      {
        if (!isalpha((unsigned char)type[i]) && type[i] != '_')
        {
          out += type[i++];
          continue;
        }
        size_t e = i;
        while (e < type.size() && isIdentChar(type[e]))
          ++e;
        std::string id = type.substr(i, e - i);
        size_t p = out.size();
        while (p > 0 && out[p - 1] == ' ')
          --p;
        bool laterPart = p >= 2 && out[p - 1] == ':' && out[p - 2] == ':'; // `A::id`: qualified already
        bool collides = false;
        for (const VariantAlternative& a : info.alternatives)
          collides = collides || (!laterPart && a.name == id);
        if (collides)
        {
          std::string scope;
          for (const ScopeRange& r : scopeRanges)
            if (r.begin <= offset && offset < r.end && scopeNames)
              if (auto it = scopeNames->find(r.key); it != scopeNames->end() && it->second.count(id))
                scope = r.key;
          out += "::" + scope;
        }
        out += id;
        i = e;
      }
      return out;
    }

    // The alternative of a variant that a written type names (exactly, or by its last component).
    static const VariantAlternative* alternativeFor(const VariantInfo& info, const std::string& spelling)
    {
      for (const VariantAlternative& a : info.alternatives)
        if (a.spelling == spelling)
          return &a;
      const VariantAlternative* found = nullptr;
      for (const VariantAlternative& a : info.alternatives)
        if (lastComponent(a.spelling) == lastComponent(spelling))
        {
          if (found)
            return nullptr; // two alternatives end in the same name
          found = &a;
        }
      return found;
    }

    static bool containsWord(std::string_view text, std::string_view word)
    {
      for (size_t p = text.find(word); p != std::string_view::npos; p = text.find(word, p + 1))
        if ((p == 0 || !isIdentChar(text[p - 1])) && (p + word.size() == text.size() || !isIdentChar(text[p + word.size()])))
          return true;
      return false;
    }

    bool convertingConstructor(FunctionDecl* f)
    {
      if (f->params.empty())
        return false;
      // (a pack takes one argument too: OutputInventories(Args&&...) converted the inventory it was given
      // in C++, `return {inventory};`)
      auto* first = static_cast<ParamDecl*>(f->params[0]);
      if (first->isPack)
        return true;
      for (size_t i = 1; i < f->params.size(); ++i)
        if (!static_cast<ParamDecl*>(f->params[i])->defaultValue && !static_cast<ParamDecl*>(f->params[i])->isPack)
          return false;
      // A template is never a copy or move constructor ([class.copy.ctor] 1): FixedPointNumberTemplate's
      // from another precision's value, BasicWireEnd's from another targeter's, convert.
      if (!f->templateParams.empty())
        return true;
      // Copy and move constructors are not conversions: `T(const T&)` takes the class itself. Another
      // class of the same name is a conversion (the game's Color(const agui::Color&)), so the name decides
      // only when the type is not resolved.
      Type* t = first->type;
      while (t && (t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType || t->kind == NodeKind::ConstType))
        t = t->kind == NodeKind::LRefType ? static_cast<LRefType*>(t)->referenced
          : t->kind == NodeKind::RRefType ? static_cast<RRefType*>(t)->referenced
                                          : static_cast<ConstType*>(t)->inner;
      if (t && t->kind == NodeKind::NamedType && !f->name.empty() && f->name.parts.size() >= 1)
      {
        auto* named = static_cast<NamedType*>(t);
        Decl* owner = global.owningClass(f);
        // In a class template, the class with arguments other than its own parameters (BasicWireEnd's
        // `BasicWireEnd(const BasicWireEnd<Targeter<Entity>>&)`) is another instance: a conversion in all
        // but that one.
        if (owner && !owner->templateParams.empty() && !named->name.empty() && named->name.last().hasTemplateArgs &&
            named->name.last().symbol == f->name.last().symbol)
        {
          const Span<Node*>& args = named->name.last().templateArgs;
          bool own = args.size() == owner->templateParams.size();
          for (size_t i = 0; own && i < args.size(); ++i)
          {
            auto* p = static_cast<TemplateParamDecl*>(owner->templateParams[i]);
            auto* a = static_cast<NamedType*>(args[i]);
            own = args[i] && args[i]->kind == NodeKind::NamedType && a->name.parts.size() == 1 && !a->name.last().hasTemplateArgs &&
                  a->name.last().symbol == p->name;
          }
          return !own;
        }
        if (named->resolved && owner && named->resolved->kind == NodeKind::ClassDecl)
          return global.definitionOf(named->resolved) != global.definitionOf(owner);
        const QualifiedName& n = named->name;
        if (!n.empty() && n.last().symbol == f->name.last().symbol)
          return false;
      }
      return true;
    }

    // Removes a whole definition (a function with its body, a class, a global): from the end of
    // whatever declaration precedes it, so that a return type or `template<...>` on an earlier
    // line goes with it.
    void removeDefinition(Decl* d)
    {
      uint32_t start = toks[d->token].offset;
      while (start > 0 && text[start - 1] != ';' && text[start - 1] != '{' && text[start - 1] != '}')
        --start;
      while (start < text.size() && (text[start] == '\n' || text[start] == '\r'))
        ++start;
      uint32_t end = d->endToken > d->token && d->endToken <= toks.size() ? endOffset(d->endToken - 1) : endOffset(d->token);
      // The rest of the line too when nothing else is on it (a `}` closing a namespace stays).
      uint32_t lineEnd = end;
      while (lineEnd < text.size() && (text[lineEnd] == ' ' || text[lineEnd] == '\t' || text[lineEnd] == '\r' || text[lineEnd] == ';'))
        ++lineEnd;
      if (lineEnd < text.size() && text[lineEnd] == '\n')
        end = lineEnd + 1;
      remove(start, end - start);
    }

    // The `)` that closes the `(` at token open, or UINT32_MAX.
    uint32_t closingParenToken(uint32_t open) const
    {
      int depth = 0;
      for (uint32_t k = open; k < toks.size(); ++k)
      {
        if (toks[k].is(P::LParen))
          ++depth;
        else if (toks[k].is(P::RParen) && --depth == 0)
          return k;
        else if (toks[k].getKind() == TokenKind::End)
          break;
      }
      return UINT32_MAX;
    }

    // The `,` or `>` that ends the template parameter starting at token `from`, or UINT32_MAX.
    uint32_t templateParameterEnd(uint32_t from) const
    {
      int depth = 0;
      for (uint32_t k = from; k < toks.size(); ++k)
      {
        const Token& tk = toks[k];
        if (tk.is(P::Less) || tk.is(P::LParen) || tk.is(P::LBracket))
          ++depth;
        else if (tk.is(P::RParen) || tk.is(P::RBracket))
          --depth;
        else if (tk.is(P::Greater) || tk.is(P::Shr))
        {
          if (depth == 0)
            return k;
          depth -= tk.is(P::Shr) ? 2 : 1;
          if (depth < 0)
            return k;
        }
        else if (tk.is(P::Comma) && depth == 0)
          return k;
        else if (tk.is(P::Semi) || tk.is(P::LBrace))
          return UINT32_MAX;
      }
      return UINT32_MAX;
    }

    // Whether d is a template written for any std::variant<T...>: its signature (template header,
    // requires clause, parameters, a partial specialization's arguments) names `std::variant<T...>`
    // with a pack, variant_size_v or variant_alternative_t, or a function's requires clause asks
    // for is_std_variant (not negated).
    bool isGenericVariantCode(Decl* d)
    {
      if (!d->isTemplate || d->templateParams.empty() || (d->kind != NodeKind::FunctionDecl && d->kind != NodeKind::ClassDecl) ||
          d->endToken <= d->token || d->endToken > toks.size())
        return false;
      uint32_t from = d->templateParams[0]->token;
      while (from > 0 && !toks[from].is(Kw::template_) && !toks[from].is(P::Semi) && !toks[from].is(P::RBrace))
        --from;
      uint32_t to = d->endToken;
      for (uint32_t k = d->token; k < d->endToken; ++k)
        if (toks[k].is(P::LBrace) || toks[k].is(P::Semi))
        {
          to = k; // the body (a function's, a class's) or the end of a declaration
          break;
        }
      if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body)
        to = std::min(to, static_cast<FunctionDecl*>(d)->body->token);
      for (uint32_t k = from; k < to; ++k)
      {
        if (toks[k].getKind() != TokenKind::Identifier)
          continue;
        std::string_view s = spelling(k);
        if (s == "variant_size_v" || s == "variant_alternative_t")
          return true;
        if (s == "is_std_variant" && d->kind == NodeKind::FunctionDecl && !(k > 0 && toks[k - 1].is(P::Bang)))
          return true;
        if (s == "variant" && k >= 2 && toks[k - 1].is(P::ColonColon) && spelling(k - 2) == "std" && k + 1 < to && toks[k + 1].is(P::Less))
          for (uint32_t j = k + 2; j < to && !toks[j].is(P::Greater) && !toks[j].is(P::Shr); ++j)
            if (toks[j].is(P::Ellipsis))
              return true;
      }
      return false;
    }

    // A template parameter that the declaration used only in allocator arguments, which convertTokens
    // dropped (`template<typename T, typename Allocate> void eraseIf(std::vector<T, Allocate>& v,
    // Pred pred)`), goes from the header too: nothing could deduce it any more.
    void dropAllocatorParameters(Decl* d)
    {
      size_t n = d->templateParams.size();
      if (droppedAllocators.empty() || n == 0 || d->endToken <= d->token || d->endToken > toks.size())
        return;
      uint32_t headerEnd = templateParameterEnd(d->templateParams[n - 1]->token); // the header's `>`
      if (headerEnd == UINT32_MAX)
        return;
      auto first = droppedAllocators.lower_bound(toks[headerEnd].offset);
      if (first == droppedAllocators.end() || first->first >= endOffset(d->endToken - 1))
        return;
      std::vector<bool> drop(n, false);
      size_t dropped = 0;
      for (size_t p = 0; p < n; ++p)
      {
        auto* tp = static_cast<TemplateParamDecl*>(d->templateParams[p]);
        if (tp->name == UINT32_MAX)
          continue;
        std::string_view name = symbols.name(tp->name);
        bool inAllocator = false, elsewhere = false;
        for (uint32_t k = headerEnd + 1; k < d->endToken && !elsewhere; ++k)
          if (toks[k].getKind() == TokenKind::Identifier && spelling(k) == name)
            (droppedAllocators.count(toks[k].offset) ? inAllocator : elsewhere) = true;
        if (inAllocator && !elsewhere)
        {
          drop[p] = true;
          ++dropped;
        }
      }
      if (dropped == 0)
        return;
      if (dropped == n)
      {
        todo(toks[d->token].offset, "every template parameter was used only in dropped allocator arguments");
        return;
      }
      // A parameter goes with the comma after it; the ones after the last kept go with the comma
      // before them, from the end of the last kept one.
      size_t lastKept = n - 1;
      while (drop[lastKept])
        --lastKept;
      auto paramEnd = [&](size_t p) { return endOffset(templateParameterEnd(d->templateParams[p]->token) - 1); };
      for (size_t p = 0; p < lastKept; ++p)
        if (drop[p])
          remove(toks[d->templateParams[p]->token].offset, toks[d->templateParams[p + 1]->token].offset - toks[d->templateParams[p]->token].offset);
      if (lastKept + 1 < n)
        remove(paramEnd(lastKept), paramEnd(n - 1) - paramEnd(lastKept));
      counts.add("allocator template parameters dropped", (int)dropped);
    }

    void removeDecl(Decl* d)
    {
      uint32_t start = declStart(d);
      uint32_t end = d->endToken > d->token && d->endToken <= toks.size() ? endOffset(d->endToken - 1) : start;
      remove(start, lineEndAfter(end) - start);
    }

    // From `from` to `end` and the rest of its line, when nothing else is on it; then its indentation goes too, which
    // would otherwise stay in front of the next line (`  template class X<int>;`, `    using namespace std::chrono_literals;`).
    void removeWholeLine(uint32_t from, uint32_t end)
    {
      uint32_t to = lineEndAfter(end);
      if (to != end)
      {
        uint32_t s = from;
        while (s > 0 && (text[s - 1] == ' ' || text[s - 1] == '\t'))
          --s;
        if (s == 0 || text[s - 1] == '\n')
          from = s;
      }
      remove(from, to - from);
    }

    // After offset, the rest of its line when nothing else is on it (to the newline included).
    uint32_t lineEndAfter(uint32_t offset) const
    {
      uint32_t lineEnd = offset;
      while (lineEnd < text.size() && (text[lineEnd] == ' ' || text[lineEnd] == '\t' || text[lineEnd] == '\r'))
        ++lineEnd;
      return lineEnd < text.size() && text[lineEnd] == '\n' ? lineEnd + 1 : offset;
    }

    // Where a declaration starts: its first token is its name; the type and specifiers before it on
    // the same line belong to it as well (forward declarations are one line).
    uint32_t declStart(Decl* d) const
    {
      uint32_t start = toks[d->token].offset;
      while (start > 0 && text[start - 1] != '\n' && text[start - 1] != ';' && text[start - 1] != '{' && text[start - 1] != '}')
        --start;
      // A `template<...>` header or `[[attributes]]` alone on the lines above belong to it too
      // (`template<size_t N2> friend class ImmutableString;` written on two lines).
      for (;;)
      {
        if (start == 0 || text[start - 1] != '\n')
          break;
        uint32_t prevEnd = start - 1;
        uint32_t prevStart = prevEnd;
        while (prevStart > 0 && text[prevStart - 1] != '\n')
          --prevStart;
        std::string_view prev = text.substr(prevStart, prevEnd - prevStart);
        while (!prev.empty() && isspace((unsigned char)prev.front()))
          prev.remove_prefix(1);
        while (!prev.empty() && isspace((unsigned char)prev.back()))
          prev.remove_suffix(1);
        bool header = prev.size() > 8 && prev.substr(0, 8) == "template" && prev.back() == '>';
        bool attribute = prev.size() > 4 && prev.substr(0, 2) == "[[" && prev.substr(prev.size() - 2) == "]]";
        if (!header && !attribute)
          break;
        start = prevStart;
      }
      return start;
    }

    // --- bodies: this-> and fallthrough ---

    // Whether variable v, whose parent is a class, is one of the class's data members. A local of
    // a lambda in a constructor's member initializers has the class as its parent too (the
    // initializers are parsed in class scope) and is no member: `: ScopedTypeMigration([&] {
    // PropertyTree result; ...; return result; }(), ...)` keeps `result` as it is.
    std::map<Decl*, std::set<Decl*>> dataMemberCache;
    bool isDataMember(Decl* v)
    {
      auto it = dataMemberCache.find(v->parent);
      if (it == dataMemberCache.end())
      {
        std::set<Decl*> members;
        for (Decl* m : static_cast<ClassDecl*>(v->parent)->members)
          if (m->kind == NodeKind::VariableDecl)
            members.insert(m);
        it = dataMemberCache.emplace(v->parent, std::move(members)).first;
      }
      return it->second.count(v) != 0;
    }

    // Non-static data members and methods of a class and its bases, by name.
    std::map<Decl*, std::set<uint32_t>> memberNameCache;
    const std::set<uint32_t>& memberNames(Decl* cls)
    {
      auto it = memberNameCache.find(cls);
      if (it != memberNameCache.end())
        return it->second;
      std::set<uint32_t>& names = memberNameCache[cls];
      std::function<void(Decl*, int)> collect = [&](Decl* d, int depth)
      {
        Decl* def = global.definitionOf(d);
        if (!def || def->kind != NodeKind::ClassDecl || depth > 8)
          return;
        auto* c = static_cast<ClassDecl*>(def);
        for (Decl* m : c->members)
        {
          if (m->flags & DF_Static)
            continue;
          if (m->kind == NodeKind::VariableDecl && !(m->flags & DF_Typedef) && static_cast<VariableDecl*>(m)->name.parts.size() == 1)
            names.insert(static_cast<VariableDecl*>(m)->name.parts[0].symbol);
          else if (m->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(m)->name.parts.size() == 1 &&
                   static_cast<FunctionDecl*>(m)->name.parts[0].special == 0)
            names.insert(static_cast<FunctionDecl*>(m)->name.parts[0].symbol);
        }
        for (const ClassDecl::Base& b : c->bases)
          if (b.type && b.type->kind == NodeKind::NamedType)
          {
            const Ty* bt = global.typeOfAst(b.type, c);
            if (bt && bt->decl)
              collect(bt->decl, depth + 1);
          }
      };
      collect(cls, 0);
      return names;
    }

    // The `explicit` among f's specifiers, or UINT32_MAX: back from its name, past an `explicit(cond)`'s
    // parentheses, to the end of what precedes the declaration.
    uint32_t explicitToken(FunctionDecl* f)
    {
      // (forward from the declaration's first token to its name or `operator`)
      for (uint32_t k = f->token; k < toks.size() && k < f->token + 16; ++k)
      {
        if (toks[k].is(Kw::explicit_))
          return k;
        if (toks[k].is(Kw::operator_) || toks[k].is(P::LParen) || toks[k].is(P::Semi) || toks[k].is(P::LBrace))
          break;
      }
      for (uint32_t k = f->token; k > 0 && f->token - k < 64;)
      {
        --k;
        const Token& t = toks[k];
        if (t.is(Kw::explicit_))
          return k;
        if (t.is(P::RParen))
        {
          for (int depth = 0;; --k)
          {
            if (toks[k].is(P::RParen))
              ++depth;
            else if (toks[k].is(P::LParen) && --depth == 0)
              break;
            if (k == 0)
              return UINT32_MAX;
          }
          continue;
        }
        if (t.is(P::Semi) || t.is(P::LBrace) || t.is(P::RBrace) || t.is(P::Colon) || t.is(P::Greater) || t.is(P::Shr))
          return UINT32_MAX;
      }
      return UINT32_MAX;
    }

    // Explicit by default (design.md, kovarex 2026-11-07): kpp has no `explicit`. The plain word goes, as an
    // unmarked constructor or conversion operator is what it said; `explicit(cond)` is `implicit(...)` of
    // the condition turned round, as the compiler's fix writes it: a negation's operand, else `!cond`.
    void writeExplicitByDefault(FunctionDecl* f)
    {
      uint32_t at = explicitToken(f);
      if (at == UINT32_MAX || at + 1 >= toks.size())
      {
        todo(toks[f->token].offset, "an explicit constructor or conversion whose `explicit` was not found");
        return;
      }
      if (!toks[at + 1].is(P::LParen))
      {
        remove(toks[at].offset, toks[at + 1].offset - toks[at].offset);
        counts.add("explicit removed");
        return;
      }
      uint32_t close = closingParenToken(at + 1);
      if (close == UINT32_MAX)
      {
        todo(toks[at].offset, "an explicit(condition) whose `)` was not found");
        return;
      }
      std::string condition(text.substr(endOffset(at + 1), toks[close].offset - endOffset(at + 1)));
      while (!condition.empty() && isspace((unsigned char)condition.front()))
        condition.erase(0, 1);
      while (!condition.empty() && isspace((unsigned char)condition.back()))
        condition.pop_back();
      // (an operand: no space, operator or `?` outside brackets, `isSame<A, B>` and `f(x)` included)
      auto operand = [](std::string_view s)
      {
        if (s.empty())
          return false;
        int depth = 0;
        for (char c : s)
        {
          if (c == '(' || c == '[' || c == '<')
            ++depth;
          else if (c == ')' || c == ']' || c == '>')
            --depth;
          else if (depth == 0 && !isIdentChar(c) && c != ':')
            return false;
        }
        return depth == 0;
      };
      std::string turned = condition.size() > 1 && condition[0] == '!' && operand(std::string_view(condition).substr(1)) ? condition.substr(1)
                         : operand(condition)                                                                     ? "!" + condition
                                                                                                                   : "!(" + condition + ")";
      replace(toks[at].offset, endOffset(close) - toks[at].offset, "implicit(" + turned + ")");
      counts.add("explicit(condition) written as implicit(!condition)");
    }

    // Whether a base of cls, or one of theirs, declares f's name virtual with the same parameter types and
    // constness: kpp requires `override` there (design.md 23b) and C++ libraries leave it out
    // (cxxopts). A wrong answer is loud either way: `override` of nothing, or an unmarked override, is
    // the compiler's error.
    bool overridesBaseVirtual(FunctionDecl* f, Decl* cls)
    {
      if (f->name.parts.size() != 1 || f->name.parts[0].special != 0)
        return false;
      uint32_t name = f->name.parts[0].symbol;
      std::function<bool(Decl*, int)> inBases = [&](Decl* d, int depth)
      {
        Decl* def = global.definitionOf(d);
        if (!def || def->kind != NodeKind::ClassDecl || depth > 8)
          return false;
        auto* c = static_cast<ClassDecl*>(def);
        for (const ClassDecl::Base& b : c->bases)
        {
          if (!b.type || b.type->kind != NodeKind::NamedType)
            continue;
          const Ty* bt = global.typeOfAst(b.type, c);
          if (!bt || !bt->decl)
          {
            // (std::exception and its family, which the conversion's resolve does not know: their
            // virtual function is `what() const`, the stdlib's Exception's)
            static const std::set<std::string> exceptions = {"exception", "logic_error", "runtime_error", "out_of_range", "invalid_argument",
                                                             "length_error", "domain_error", "range_error", "overflow_error", "underflow_error", "bad_alloc"};
            const QualifiedName& bn = static_cast<NamedType*>(b.type)->name;
            if (bn.parts.size() == 2 && symbols.name(bn.parts[0].symbol) == "std" && exceptions.count(std::string(symbols.name(bn.parts[1].symbol))) &&
                symbols.name(name) == "what" && f->params.empty() && (f->flags & DF_Const))
              return true;
            continue;
          }
          Decl* baseDef = global.definitionOf(bt->decl);
          if (baseDef && baseDef->kind == NodeKind::ClassDecl)
            for (Decl* m : static_cast<ClassDecl*>(baseDef)->members)
              if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override)))
              {
                auto* v = static_cast<FunctionDecl*>(m);
                if (v->name.parts.size() == 1 && v->name.parts[0].special == 0 && v->name.parts[0].symbol == name &&
                    v->params.size() == f->params.size() && (v->flags & DF_Const) == (f->flags & DF_Const) && sameParameters(v, f))
                  return true;
              }
          if (inBases(bt->decl, depth + 1))
            return true;
        }
        return false;
      };
      return inBases(cls, 0);
    }

    // A parameter's type as its file writes it, one space between tokens, without its name or default
    // value: the tokens between the `(` or `,` before it and the `,`, `=` or `)` after it. Empty when its
    // tokens are not at hand.
    static std::string writtenParameterType(const ParamDecl* p)
    {
      if (!p->file || !p->file->tokens || p->token >= p->file->tokens->size())
        return {};
      const std::vector<Token>& ts = *p->file->tokens;
      uint32_t start = p->token;
      for (int depth = 0; start > 0; --start)
      {
        const Token& t = ts[start - 1];
        if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::Greater))
          ++depth;
        else if ((t.is(P::LParen) || t.is(P::LBracket) || t.is(P::Less)) && depth > 0)
          --depth;
        else if (depth == 0 && (t.is(P::LParen) || t.is(P::Comma)))
          break;
        else if (t.is(P::Semi) || t.is(P::LBrace) || t.is(P::RBrace))
          return {};
      }
      std::string out;
      int depth = 0;
      for (uint32_t k = start; k < ts.size(); ++k)
      {
        const Token& t = ts[k];
        if (depth == 0 && (t.is(P::Assign) || t.is(P::Comma) || t.is(P::RParen)))
          break;
        if (t.is(P::LParen) || t.is(P::LBracket) || t.is(P::Less))
          ++depth;
        else if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::Greater))
          --depth;
        else if (t.is(P::Semi) || t.is(P::LBrace) || t.getKind() == TokenKind::End)
          return {};
        if (t.getKind() == TokenKind::Identifier && t.id == p->name)
          continue;
        out += (out.empty() ? "" : " ") + std::string(p->file->text.substr(t.offset, t.length));
      }
      return out;
    }

    // Whether two functions take the same parameter types (an overload of another type overrides
    // nothing): the same resolved type, or the same type as written (std::string in cxxopts' class
    // template and in its base resolve to two types in the conversion). A wrong yes is the compiler's
    // error, `override` of nothing.
    bool sameParameters(FunctionDecl* a, FunctionDecl* b)
    {
      if (a->params.size() != b->params.size() || ((a->flags ^ b->flags) & DF_Variadic))
        return false;
      const Ty* ta = global.typeOfDecl(a);
      const Ty* tb = global.typeOfDecl(b);
      bool typed = ta && tb && ta->kind == TyKind::Function && tb->kind == TyKind::Function && ta->args.size() == a->params.size() &&
                   tb->args.size() == b->params.size();
      for (size_t i = 0; i < a->params.size(); ++i)
      {
        if (typed && ta->args[i] && tb->args[i] && ta->args[i]->isKnown() && ta->args[i] == tb->args[i])
          continue;
        std::string wa = writtenParameterType(static_cast<ParamDecl*>(a->params[i]));
        if (wa.empty() || wa != writtenParameterType(static_cast<ParamDecl*>(b->params[i])))
          return false;
      }
      return true;
    }

    // Where `override` goes in f's declaration: before the `{`, `;` or `=` that follows its parameters,
    // qualifiers and trailing return type; UINT32_MAX when the parameters' `(` is not found.
    uint32_t overrideToken(FunctionDecl* f)
    {
      uint32_t name = f->name.last().symbol;
      uint32_t end = std::min<uint32_t>(f->endToken, uint32_t(toks.size()));
      for (uint32_t k = f->token; k + 1 < end; ++k)
        if (toks[k].getKind() == TokenKind::Identifier && toks[k].id == name && toks[k + 1].is(P::LParen))
        {
          uint32_t close = closingParenToken(k + 1);
          if (close == UINT32_MAX)
            return UINT32_MAX;
          int depth = 0;
          for (uint32_t at = close + 1; at < end; ++at)
          {
            const Token& tk = toks[at];
            if (tk.is(P::LParen))
              ++depth;
            else if (tk.is(P::RParen))
              --depth;
            else if (depth == 0 && (tk.is(P::LBrace) || tk.is(P::Semi) || tk.is(P::Assign)))
              return at;
          }
          return end < toks.size() && toks[end].is(P::Semi) ? end : UINT32_MAX;
        }
      return UINT32_MAX;
    }

