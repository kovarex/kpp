// The driver: parsing and resolving a build, and their reports. Part of main.cpp, included in its
// namespace there (a file of its own since 2026-11-05, moved without a change).
  // A config:: constant of an enumeration type (config.cpp takes any other name for one: `config Mode
  // m = Mode::Fast;`): when the parsed program declares its type as a class, or as an enumeration
  // without that enumerator, an error at its declaration, not at <config>'s constant. A type it does
  // not find (an alias, a template's) is the resolver's. The number of errors.
  size_t checkConfigEnumerations(Build& build)
  {
    if (build.configDeclared.empty())
      return 0;
    SymbolTable& symbols = *build.symbols;
    std::map<std::string, std::vector<std::string>> enumerations; // qualified name: its enumerators
    std::set<std::string> classes;
    std::function<void(Span<Decl*>, const std::string&)> walk = [&](Span<Decl*> decls, const std::string& scope)
    {
      for (Decl* d : decls)
      {
        if (!d)
          continue;
        if (d->kind == NodeKind::NamespaceDecl)
        {
          auto* n = static_cast<NamespaceDecl*>(d);
          std::string inner = scope;
          if (!n->nested.empty())
            for (const NamePart& p : n->nested.parts)
              inner += (p.symbol == UINT32_MAX ? "" : std::string(symbols.name(p.symbol))) + "::";
          else if (n->name != UINT32_MAX)
            inner += std::string(symbols.name(n->name)) + "::";
          walk(n->decls, inner);
        }
        else if (d->kind == NodeKind::ClassDecl)
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (c->name.empty() || c->name.last().symbol == UINT32_MAX)
            continue;
          std::string name = scope + std::string(symbols.name(c->name.last().symbol));
          classes.insert(name);
          walk(c->members, name + "::");
        }
        else if (d->kind == NodeKind::EnumDecl)
        {
          auto* e = static_cast<EnumDecl*>(d);
          if (e->name.empty() || e->name.last().symbol == UINT32_MAX || !e->isDefinition)
            continue;
          std::vector<std::string>& names = enumerations[scope + std::string(symbols.name(e->name.last().symbol))];
          for (Decl* x : e->enumerators)
            if (x && x->kind == NodeKind::EnumeratorDecl)
              names.push_back(std::string(symbols.name(static_cast<EnumeratorDecl*>(x)->name)));
        }
      }
    };
    for (const std::unique_ptr<ParsedFile>& p : build.parsed)
      if (p && !p->isImported)
        walk(p->decls, p->libraryScope == UINT32_MAX ? "" : std::string(symbols.name(p->libraryScope)) + "::");
    size_t errors = 0;
    for (const auto& [name, source] : build.configDeclared)
    {
      const ConfigDeclaration& d = source.declaration;
      auto value = build.options.configValues.find(name);
      if (d.kind != "enum" || value == build.options.configValues.end())
        continue;
      std::string problem;
      auto e = enumerations.find(d.type);
      if (classes.count(d.type) && e == enumerations.end())
        problem = "its type " + d.type + " is a class, not an enumeration";
      else if (e != enumerations.end() && std::find(e->second.begin(), e->second.end(), value->second.text) == e->second.end())
        problem = d.type + " has no enumerator " + value->second.text;
      if (problem.empty())
        continue;
      ++errors;
      fprintf(stderr, "%s: error: config::%s: %s\n", sourceLocation(source.path, d.line, d.column).c_str(), name.c_str(), problem.c_str());
    }
    return errors;
  }

  void runResolve(Build& build);
  void reportParse(Build& build, double parseTime);
  void reportResolve(Build& build);

  uint32_t lineOf(const std::string& text, uint32_t offset)
  {
    uint32_t line = 1;
    for (uint32_t i = 0; i < offset && i < text.size(); ++i)
      if (text[i] == '\n')
        ++line;
    return line;
  }

  // A byte offset's column, from 1 (--diagnostic-columns).
  uint32_t columnOf(const std::string& text, uint32_t offset)
  {
    uint32_t start = std::min<uint32_t>(offset, uint32_t(text.size()));
    while (start > 0 && text[start - 1] != '\n')
      --start;
    return 1 + offset - start;
  }

  // --c-type-warnings: C's names of the integer types (int, unsigned, short, long, long long, signed
  // and unsigned char, int32_t, size_t ...) are for C interop only (design.md, "Integer types and
  // sizes"): each one written outside an extern "C" declaration, the C headers and the standard
  // library is a warning naming kpp's type (u32, i64 ...). No error: the exit code stays.
  void warnCTypeNames(const std::vector<SourceFile>& files, const std::vector<LexResult>& lexed, const std::vector<std::unique_ptr<ParsedFile>>& parsed)
  {
    static const std::map<std::string_view, const char*> named = {
      {"int8_t", "i8"},    {"int16_t", "i16"},  {"int32_t", "i32"},  {"int64_t", "i64"},   {"uint8_t", "u8"},   {"uint16_t", "u16"},
      {"uint32_t", "u32"}, {"uint64_t", "u64"}, {"size_t", "usize"}, {"ptrdiff_t", "i64"}, {"intptr_t", "i64"}, {"uintptr_t", "u64"},
      {"ssize_t", "i64"},  {"__int8", "i8"},    {"__int16", "i16"},  {"__int32", "i32"},   {"__int64", "i64"}};
    auto typeKeyword = [](const Token& t) {
      return t.is(Kw::signed_) || t.is(Kw::unsigned_) || t.is(Kw::short_) || t.is(Kw::int_) || t.is(Kw::long_) || t.is(Kw::char_) || t.is(Kw::double_);
    };
    for (size_t i = 0; i < files.size(); ++i)
    {
      if (parsed[i]->isImported || parsed[i]->isLibrary)
        continue;
      // The token ranges of the extern "C" declarations (each flagged, in a block or not), in order.
      std::vector<std::pair<uint32_t, uint32_t>> externC;
      auto collect = [&](auto& self, Span<Decl*> decls) -> void {
        for (Decl* d : decls)
          if (d->flags & DF_ExternC)
            externC.push_back({d->token, std::max(d->endToken, d->token + 1)});
          else if (d->kind == NodeKind::NamespaceDecl)
            self(self, static_cast<NamespaceDecl*>(d)->decls);
      };
      collect(collect, parsed[i]->decls);
      std::sort(externC.begin(), externC.end());
      const std::vector<Token>& tokens = lexed[i].tokens;
      const std::string& text = files[i].text;
      auto spell = [&](const Token& t) { return std::string_view(text).substr(t.offset, t.length); };
      size_t range = 0;
      for (size_t k = 0; k < tokens.size(); ++k)
      {
        while (range < externC.size() && externC[range].second <= k)
          ++range;
        if (range < externC.size() && externC[range].first <= k)
        {
          k = externC[range].second - 1;
          continue;
        }
        const Token& t = tokens[k];
        std::string phrase;
        const char* kpp = nullptr;
        if (typeKeyword(t))
        {
          // A run of type keywords is one type: `unsigned long long`, `long double`, `signed char`.
          int longs = 0;
          bool isUnsigned = false, isSigned = false, isShort = false, isChar = false, isDouble = false;
          size_t e = k;
          for (; e < tokens.size() && typeKeyword(tokens[e]); ++e)
          {
            longs += tokens[e].is(Kw::long_);
            isUnsigned |= tokens[e].is(Kw::unsigned_);
            isSigned |= tokens[e].is(Kw::signed_);
            isShort |= tokens[e].is(Kw::short_);
            isChar |= tokens[e].is(Kw::char_);
            isDouble |= tokens[e].is(Kw::double_);
            phrase += (phrase.empty() ? "" : " ") + std::string(spell(tokens[e]));
          }
          if (e < tokens.size() && tokens[e].getKind() == TokenKind::Identifier && named.count(spell(tokens[e])))
          {
            phrase += " " + std::string(spell(tokens[e])); // `unsigned __int64`
            longs = spell(tokens[e]) == "__int64" ? 2 : longs;
            ++e;
          }
          if (isDouble)
            kpp = longs ? "f64" : nullptr; // (double itself is no integer name)
          else if (isChar)
            kpp = isUnsigned ? "u8" : isSigned ? "i8" : nullptr; // (char is the text character)
          else if (isShort)
            kpp = isUnsigned ? "u16" : "i16";
          else if (longs >= 2)
            kpp = isUnsigned ? "u64" : "i64";
          else if (longs == 1)
            kpp = isUnsigned ? "u32 (C's unsigned long is 32 bits on Windows)" : "i32 (C's long is 32 bits on Windows)";
          else
            kpp = isUnsigned ? "u32" : "i32";
          k = e - 1;
        }
        else if (t.getKind() == TokenKind::Identifier)
          if (auto it = named.find(spell(t)); it != named.end())
          {
            phrase = it->first;
            kpp = it->second;
          }
        if (kpp)
          fprintf(stderr, "%s: warning: %s is C's name, for C interop (extern \"C\"): write %s\n",
                  sourceLocation(files[i].path.string(), lineOf(text, t.offset), columnOf(text, t.offset)).c_str(), phrase.c_str(), kpp);
      }
    }
  }

  void runParse(Build& build)
  {
    using Clock = std::chrono::steady_clock;
    std::vector<SourceFile>& files = build.files;
    std::vector<LexResult>& lexed = build.lexed;
    SymbolTable& symbols = *build.symbols;
    const Options& options = build.options;
    std::vector<std::unique_ptr<ParsedFile>>& parsed = build.parsed;
    parsed.resize(files.size());
    double parseTime = 1e9;
    for (int round = 0; round < std::max(1, options.repeat); ++round)
    {
      // Repeated rounds re-parse from the original tokens ('>>' splits are undone by re-lexing).
      std::vector<std::vector<Token>> tokens(files.size());
      if (round > 0)
        parallelFor(files.size(), options.threads, [&](size_t i) { tokens[i] = lexed[i].tokens; });
      auto start = Clock::now();
      parallelFor(files.size(), options.threads, [&](size_t i)
      {
        parsed[i] = std::make_unique<ParsedFile>();
        parsed[i]->path = files[i].path.string();
        parsed[i]->index = uint32_t(i);
        parsed[i]->lambdaTag = i < build.lambdaTags.size() ? build.lambdaTags[i] : 0;
        std::string library = options.libraryRoot.string();
        parsed[i]->isLibrary = !library.empty() && parsed[i]->path.compare(0, library.size(), library) == 0;
        if (i < build.fileLibrary.size() && build.fileLibrary[i] >= 0)
        {
          const Options::LibraryRoot& root = options.libraryRoots[size_t(build.fileLibrary[i])];
          parsed[i]->library = root.name;
          if (root.scoped)
            parsed[i]->libraryScope = symbols.intern(root.name);
        }
        parseFile(round == 0 ? lexed[i].tokens : tokens[i], files[i].text, symbols, *parsed[i], false, &options.configValues);
      });
      double t = seconds(Clock::now() - start);
      if (round > 0)
        report("parse round %d: %.3f s\n", round + 1, t);
      parseTime = std::min(parseTime, t);
      if (round + 1 < std::max(1, options.repeat))
        continue;
      if (round > 0)
        for (size_t i = 0; i < files.size(); ++i)
        {
          lexed[i].tokens = std::move(tokens[i]);
          parsed[i]->tokens = &lexed[i].tokens;
        }
    }
    lastImportsLine.clear();
    importCHeaders(files, lexed, parsed, symbols, options);
    build.importsLine = lastImportsLine;
    reportParse(build, parseTime);
    build.parseErrors += checkConfigEnumerations(build);
    if (build.parseErrors && options.convertDir.empty())
      codegenExit = 1;
    runResolve(build);
  }

  // The parse statistics and the parse errors.
  void reportParse(Build& build, double parseTime)
  {
    std::vector<SourceFile>& files = build.files;
    std::vector<LexResult>& lexed = build.lexed;
    const Options& options = build.options;
    std::vector<std::unique_ptr<ParsedFile>>& parsed = build.parsed;
    uint64_t decls = 0, bodies = 0, nodes = 0, errors = 0, filesWithErrors = 0, arenaBytes = 0, lines = 0;
    for (size_t i = 0; i < files.size(); ++i)
    {
      if (parsed[i]->isImported)
        continue; // C headers are not part of the parse statistics
      decls += parsed[i]->declCount;
      bodies += parsed[i]->functionBodies;
      nodes += parsed[i]->nodeCount;
      errors += parsed[i]->diagnostics.size();
      filesWithErrors += !parsed[i]->diagnostics.empty();
      arenaBytes += parsed[i]->arenaBytesReported ? parsed[i]->arenaBytesReported : parsed[i]->arena.bytesAllocated();
      lines += lexed[i].lineCount;
    }
    report("parse:   %.3f s  (%.1f M lines/s)\n", parseTime, lines / 1e6 / parseTime);
    report("  decls: %llu, function bodies: %llu, nodes: %llu, AST memory: %.1f MB\n", (unsigned long long)decls,
           (unsigned long long)bodies, (unsigned long long)nodes, arenaBytes / 1e6);
    report("  parse errors: %llu in %llu files (%.2f%% of files clean)\n", (unsigned long long)errors,
           (unsigned long long)filesWithErrors, 100.0 * (files.size() - filesWithErrors) / std::max<size_t>(1, files.size()));
    // A parse error in the program's files is an error of the build, always shown (whatever --errors
    // says) and on stderr: the parser recovers to see more of the file, but what it recovered to is no
    // program, so nothing is generated from it (runResolve), --keep-going or not.
    build.parseErrors = 0;
    for (size_t i = 0; i < files.size(); ++i)
      if (!parsed[i]->isImported)
        for (const Diagnostic& d : parsed[i]->diagnostics)
        {
          ++build.parseErrors;
          const Token& t = lexed[i].tokens[std::min<size_t>(d.token, lexed[i].tokens.size() - 1)];
          fprintf(stderr, "%s: error: %s (at '%.*s')\n", sourceLocation(files[i].path.string(), lineOf(files[i].text, t.offset), columnOf(files[i].text, t.offset)).c_str(), d.message,
                  int(std::min<uint32_t>(t.length, 30)), files[i].text.data() + t.offset);
        }
    if (options.cTypeWarnings)
      warnCTypeNames(files, lexed, parsed);
    // --convert reads C++, whose macro uses the parser does not expand (the converter expands them
    // in the text it writes): its parse errors are shown, and the converted kpp is what must parse.
    if (build.parseErrors && options.convertDir.empty())
      codegenExit = 1;
    int shown = 0;
    for (size_t i = 0; i < files.size() && shown < options.showErrors; ++i)
      for (const Diagnostic& d : parsed[i]->diagnostics)
      {
        if (!parsed[i]->isImported)
          break; // on stderr, above
        if (!options.showImportErrors)
          break; // reported in one line by importCHeaders
        if (shown++ >= options.showErrors)
          break;
        const Token& t = lexed[i].tokens[std::min<size_t>(d.token, lexed[i].tokens.size() - 1)];
        if (quietOutput) // --quiet hides the statistics, not the program's errors
          fprintf(stderr, "%s: error: %s (at '%.*s')\n", sourceLocation(files[i].path.string(), lineOf(files[i].text, t.offset), columnOf(files[i].text, t.offset)).c_str(), d.message,
                  int(std::min<uint32_t>(t.length, 30)), files[i].text.data() + t.offset);
        else
          report("  %s: error: %s (at '%.*s')\n", sourceLocation(files[i].path.string(), lineOf(files[i].text, t.offset), columnOf(files[i].text, t.offset)).c_str(), d.message,
                 int(std::min<uint32_t>(t.length, 30)), files[i].text.data() + t.offset);
      }
  }

  void runResolve(Build& build)
  {
    using Clock = std::chrono::steady_clock;
    std::vector<SourceFile>& files = build.files;
    std::vector<LexResult>& lexed = build.lexed;
    SymbolTable& symbols = *build.symbols;
    const Options& options = build.options;
    std::vector<std::unique_ptr<ParsedFile>>& parsed = build.parsed;
    build.global = std::make_unique<GlobalScope>(symbols);
    GlobalScope& global = *build.global;
    global.setLP64(!options.target.windows()); // (long, wchar_t, printf's %ld; docs/design.md, "Targets and hosts")
    global.setConverting(!options.convertDir.empty());
    global.setDebug(options.debug);
    for (size_t begin = 0; begin < options.whyAt.size();)
    {
      size_t end = std::min(options.whyAt.find(',', begin), options.whyAt.size());
      std::string at = options.whyAt.substr(begin, end - begin);
      if (size_t colon = at.rfind(':'); colon != std::string::npos)
      {
        std::string file = at.substr(0, colon);
        std::replace(file.begin(), file.end(), '/', '\\');
        global.traceLine(file, uint32_t(atoi(at.c_str() + colon + 1)));
      }
      begin = end + 1;
    }
    // What each unit of the resolve read, for a compile server's next, incremental, build
    // (docs/compile-speed.md, step 2); KPP_DEPENDENCY_STATS records them anywhere and reports them.
    // From the index on: an instance checked there (finishIndex) counts in the instances'
    // statistics, and a total made again from what each kept (step 4) must have it.
    GlobalScope::recordDependencies(servingBuild() || getenv("KPP_DEPENDENCY_STATS"));
    auto start = Clock::now();
    parallelFor(parsed.size(), options.threads, [&](size_t i) { global.indexFile(*parsed[i]); });
    double indexTime = seconds(Clock::now() - start);
    start = Clock::now();
    global.finishIndex();
    double finishTime = seconds(Clock::now() - start);
    char indexLine[256];
    snprintf(indexLine, sizeof indexLine, "index:   %.3f s  (+%.3f s for out-of-line class definitions): %zu scopes, %zu entries\n", indexTime,
             finishTime, global.scopeCount(), global.entryCount());
    build.indexLine = indexLine;
    build.indexTime = indexTime;
    build.indexScopes = global.scopeCount();
    build.indexEntries = global.entryCount();
    build.finishTime = finishTime;
    report("%s", indexLine);
    // (kpp's one global scope: not the rule of the C++ a conversion reads, whose names local to their file are)
    if (options.convertDir.empty())
      build.clashes = global.topLevelClashes();
    for (const std::string& clash : build.clashes)
    {
      fprintf(stderr, "%s\n", clash.c_str());
      codegenExit = 1;
    }

    std::vector<ResolveStats>& stats = build.stats;
    stats.assign(parsed.size(), ResolveStats{});
    start = Clock::now();
    for (size_t i = 0; i < stats.size(); ++i)
    {
      stats[i].fileIndex = uint32_t(i);
      stats[i].collectWhy = options.showWhy > 0;
    }
    // Class instances' member bodies as tasks, taken by the workers that run out of files
    // (docs/compile-speed.md, step 3), so that no file drags its instances onto one thread.
    if (options.accessWarnings)
      global.enableAccessWarnings();
    setZeroNullWarnings(options.zeroNullWarnings); // (the walks' notes and codegen's)
    if (options.convertDir.empty())
      global.enableAccessErrors(); // (not the C++ a conversion reads: its friends, its quirks)
    if (options.structRule && options.convertDir.empty()) // (not the C++ a conversion reads, whose structs may break it)
      global.enableStructRule();
    global.resolveAll(parsed.size(), options.threads, [&](size_t i) { global.resolveFile(*parsed[i], lexed[i].tokens, files[i].text, stats[i]); });
    build.resolveTime = seconds(Clock::now() - start);
    // What the reports count is the resolve's: codegen instantiates more afterwards.
    global.resolveDone();
    build.instantiation = global.instantiationStats();
    build.instanceStats = global.instanceStats();
    reportResolve(build);
  }

  // The resolve's statistics, then codegen (or the conversion), and the build is done.
  void reportResolve(Build& build)
  {
    std::vector<SourceFile>& files = build.files;
    std::vector<LexResult>& lexed = build.lexed;
    SymbolTable& symbols = *build.symbols;
    const Options& options = build.options;
    std::vector<std::unique_ptr<ParsedFile>>& parsed = build.parsed;
    GlobalScope& global = *build.global;
    std::vector<ResolveStats>& stats = build.stats;
    double resolveTime = build.resolveTime;
    ResolveStats total;
    for (const ResolveStats& s : stats)
      total.add(s);
    total.add(build.instanceStats); // the instances', each once whoever asked first (at the end of the resolve)
    // KPP_DUMP_FILESTATS=path: each file's statistics and the instances', one line each, for finding
    // where two runs differ (tests/determinism.ps1 compares only the totals).
    if (const char* dumpPath = getenv("KPP_DUMP_FILESTATS"))
      if (FILE* f = fopen(dumpPath, "w"))
      {
        auto line = [&](const std::string& what, const ResolveStats& s)
        {
          fprintf(f, "%s types %llu/%llu/%llu names %llu/%llu/%llu/%llu/%llu exprs %llu/%llu/%llu calls %llu/%llu auto %llu\n", what.c_str(),
                  (unsigned long long)s.typesResolved, (unsigned long long)s.typesDependent, (unsigned long long)s.typesUnresolved,
                  (unsigned long long)s.namesLocal, (unsigned long long)s.namesMember, (unsigned long long)s.namesGlobal,
                  (unsigned long long)s.namesDependent, (unsigned long long)s.namesUnresolved, (unsigned long long)s.exprsTyped,
                  (unsigned long long)s.exprsDependent, (unsigned long long)s.exprsUnknown, (unsigned long long)s.callsResolved,
                  (unsigned long long)s.callsAmbiguousArity, (unsigned long long)s.unknownAuto);
        };
        for (size_t i = 0; i < stats.size(); ++i)
          line(files[i].path.string(), stats[i]);
        line("<instances>", build.instanceStats);
        fclose(f);
      }
    uint64_t types = total.typesResolved + total.typesDependent + total.typesUnresolved;
    uint64_t names = total.namesLocal + total.namesMember + total.namesGlobal + total.namesDependent + total.namesUnresolved;
    uint64_t project = total.typesUnresolved + total.namesUnresolved - total.qualifiedUnresolvedStd - total.unresolvedMacroLike;
    report("resolve: %.3f s\n", resolveTime);
    report("  type names:  %llu  (%.1f%% resolved, %.1f%% dependent on template parameters, %.1f%% unresolved)\n",
           (unsigned long long)types, 100.0 * total.typesResolved / std::max<uint64_t>(1, types),
           100.0 * total.typesDependent / std::max<uint64_t>(1, types), 100.0 * total.typesUnresolved / std::max<uint64_t>(1, types));
    report("  identifiers: %llu  (%.1f%% local, %.1f%% member, %.1f%% namespace/global, %.1f%% dependent, %.1f%% unresolved)\n",
           (unsigned long long)names, 100.0 * total.namesLocal / std::max<uint64_t>(1, names),
           100.0 * total.namesMember / std::max<uint64_t>(1, names), 100.0 * total.namesGlobal / std::max<uint64_t>(1, names),
           100.0 * total.namesDependent / std::max<uint64_t>(1, names), 100.0 * total.namesUnresolved / std::max<uint64_t>(1, names));
    report("  unresolved: %llu std::, %llu macro-like, %llu other; %llu fixed-width integer names typed as builtins\n",
           (unsigned long long)total.qualifiedUnresolvedStd, (unsigned long long)total.unresolvedMacroLike,
           (unsigned long long)project, (unsigned long long)total.typesBuiltinByName);
    uint64_t members = total.membersBound + total.membersDependent + total.membersUnknownBase + total.membersNotFound;
    uint64_t exprs = total.exprsTyped + total.exprsDependent + total.exprsUnknown;
    report("check:   %zu distinct types\n", global.typeCount());
    report("  member accesses: %llu  (%.1f%% bound to a declaration, %.1f%% dependent, %.1f%% object type unknown, %.1f%% no such member)\n",
           (unsigned long long)members, 100.0 * total.membersBound / std::max<uint64_t>(1, members),
           100.0 * total.membersDependent / std::max<uint64_t>(1, members), 100.0 * total.membersUnknownBase / std::max<uint64_t>(1, members),
           100.0 * total.membersNotFound / std::max<uint64_t>(1, members));
    report("  expressions: %llu  (%.1f%% typed, %.1f%% dependent on template parameters, %.1f%% unknown)\n",
           (unsigned long long)exprs, 100.0 * total.exprsTyped / std::max<uint64_t>(1, exprs),
           100.0 * total.exprsDependent / std::max<uint64_t>(1, exprs), 100.0 * total.exprsUnknown / std::max<uint64_t>(1, exprs));
    report("  unknown because: name %llu, object type %llu, member %llu, overload %llu, deduction %llu, auto return %llu, "
           "auto %llu, operator %llu, unsupported construct %llu\n",
           (unsigned long long)total.unknownName, (unsigned long long)total.unknownBase, (unsigned long long)total.unknownMember,
           (unsigned long long)total.unknownOverload, (unsigned long long)total.unknownDeduction, (unsigned long long)total.unknownAutoReturn,
           (unsigned long long)total.unknownAuto, (unsigned long long)total.unknownOperator, (unsigned long long)total.unknownUnsupported);
    report("  calls resolved to a function: %llu (%llu still ambiguous after conversion ranking)\n", (unsigned long long)total.callsResolved,
           (unsigned long long)total.callsAmbiguousArity);
    GlobalScope::InstantiationStats inst = build.instantiation;
    uint64_t instExprs = total.instExprsTyped + total.instExprsDependent + total.instExprsUnknown;
    report("instantiate: %zu class and %zu function template instantiations checked once each (%zu requests, %zu waits), %.3f s of CPU\n",
           inst.classes, inst.functions, inst.requests, inst.waits, inst.seconds);
    report("  %zu member bodies checked inside instantiations; %llu expressions there (%.1f%% typed, %.1f%% dependent, %.1f%% unknown)\n",
           inst.bodies, (unsigned long long)instExprs, 100.0 * total.instExprsTyped / std::max<uint64_t>(1, instExprs),
           100.0 * total.instExprsDependent / std::max<uint64_t>(1, instExprs), 100.0 * total.instExprsUnknown / std::max<uint64_t>(1, instExprs));
    if (getenv("KPP_DEPENDENCY_STATS")) // not in the normal report: a server's build prints what a new process's does
    {
      GlobalScope::DependencyStats deps = global.dependencyStats();
      report("dependencies: %zu walks, %zu declared types, %zu instances, %zu class instances, %zu scopes' relations; %zu names and %zu uses recorded\n",
             deps.walks, deps.declaredTypes, deps.instances, deps.classInstances, deps.scopes, deps.names, deps.uses);
    }
    std::vector<std::pair<uint32_t, uint32_t>> missing(total.missingMembers.begin(), total.missingMembers.end());
    std::sort(missing.begin(), missing.end(), [&](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : symbols.name(a.first) < symbols.name(b.first); }); // ties by name: symbol ids depend on the threads
    if (!missing.empty() && options.showUnresolved > 0)
      report("  most common members not found in a known class:\n");
    for (size_t i = 0; i < missing.size() && int(i) < options.showUnresolved / 2; ++i)
    {
      ResolveStats::Where w = total.missingMembersFirst[missing[i].first];
      const Token& t = lexed[w.file].tokens[std::min<size_t>(w.token, lexed[w.file].tokens.size() - 1)];
      report("    %6u  %-28.*s first at %s:%u\n", missing[i].second, int(symbols.name(missing[i].first).size()),
             symbols.name(missing[i].first).data(), files[w.file].path.string().c_str(), lineOf(files[w.file].text, t.offset));
    }
    std::vector<std::pair<uint32_t, uint32_t>> unknownBase(total.unknownBaseMembers.begin(), total.unknownBaseMembers.end());
    std::sort(unknownBase.begin(), unknownBase.end(), [&](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : symbols.name(a.first) < symbols.name(b.first); }); // ties by name: symbol ids depend on the threads
    if (!unknownBase.empty() && options.showUnresolved > 0)
      report("  most common members accessed on an object of unknown type:\n");
    for (size_t i = 0; i < unknownBase.size() && int(i) < options.showUnresolved / 2; ++i)
      report("    %6u  %.*s\n", unknownBase[i].second, int(symbols.name(unknownBase[i].first).size()), symbols.name(unknownBase[i].first).data());
    if (options.showWhy > 0)
      report("%s", whyReport(total, symbols, options.showWhy).c_str());
    if (options.showUnresolved > 0)
      report("  most common unresolved names:\n");
    std::vector<std::pair<uint32_t, uint32_t>> top(total.unresolvedSymbols.begin(), total.unresolvedSymbols.end());
    std::sort(top.begin(), top.end(), [&](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : symbols.name(a.first) < symbols.name(b.first); }); // ties by name: symbol ids depend on the threads
    // (no code generated: no function counts as emitted, as in a build from scratch, whatever a kept
    // program's last code generator emitted)
    if (build.parseErrors && generatesCode(options))
    {
      fprintf(stderr, "error: %zu parse error(s): no code generated\n", build.parseErrors);
      global.setCodeGenerated(false);
      reportCheckerErrors(global);
      build.codegenBehind = true;
    }
    else if (!build.clashes.empty() && generatesCode(options))
    {
      // (a name two entities claim is looked up as one of them: what codegen made of it would only
      // add errors that follow from the clash)
      fprintf(stderr, "error: %zu top-level name(s) declared twice: no code generated\n", build.clashes.size());
      global.setCodeGenerated(false);
      reportCheckerErrors(global);
      build.codegenBehind = true;
    }
    else if (generatesCode(options) && options.check && !build.rebuildCodegen && global.hasCheckerErrors())
    {
      // kppc --check (`kppc --lsp`'s checks) of a program the front end found errors in, when codegen
      // has no kept session to follow the edit: the front end's errors are the answer, codegen's own
      // come once they are fixed. Codegen keeps no session across some errors (a failure remembered for
      // every use, one it cannot give to a function), so while they persisted every check generated the
      // whole program again: the game with 264 errors (`when` conditions naming an undeclared config
      // constant) took 17-22 s a check, with 1,602 errors 6-9 s, for a front end of 0.4 s. With a kept
      // session that can follow, codegen runs as before, so a typo's error shows beside codegen's own
      // (a member that does not exist). The next check that generates code emits what changed
      // meanwhile (codegenBehind).
      global.setCodeGenerated(false);
      size_t errors = reportCheckerErrors(global);
      fprintf(stderr, "error: %zu error(s) before code generation: no code generated (--check without a kept session)\n", errors);
      build.codegenBehind = true;
    }
    else if (generatesCode(options))
      runCodegen(build);
    else if (options.convertDir.empty())
    {
      reportCheckerErrors(global);
      reportAccessWarnings(build);
      reportReturnCopyWarnings(build);
      if (!options.runBuild.empty() && !codegenExit)
      {
        // `--run-build debug,release`: build.kpp's description of each configuration, computed by
        // the interpreter, on stdout one after the other.
        for (size_t start = 0; start <= options.runBuild.size() && !codegenExit;)
        {
          size_t comma = std::min(options.runBuild.find(',', start), options.runBuild.size());
          std::string error;
          if (std::optional<std::string> text = global.runBuild(std::string_view(options.runBuild).substr(start, comma - start), error))
            fwrite(text->data(), 1, text->size(), stdout);
          else
          {
            fprintf(stderr, "error: %s\n", error.c_str());
            codegenExit = 1;
          }
          start = comma + 1;
        }
      }
    }
    if (!options.convertDir.empty())
    {
      std::vector<ConvertInput> inputs;
      for (size_t i = 0; i < parsed.size(); ++i)
        inputs.push_back({files[i].path, files[i].text, &lexed[i].tokens, parsed[i].get(), options.cSources && isCSource(files[i].path)});
      std::vector<std::filesystem::path> roots(options.roots.begin(), options.roots.end());
      setConvertLibraries(options.convertLibraries);
      std::string summary =
        convertToKpp(inputs, global, symbols, roots, options.convertDir, options.overridesDir, options.configuration, options.conversionLibrary,
                     options.convertBase);
      report("convert: %s\n%s", options.convertDir.c_str(), summary.c_str());
      if (convertErrorCount())
        codegenExit = 1;
      reportAccessWarnings(build); // over the C++ the conversion read, its friends still declared
    }
    for (size_t i = 0; i < top.size() && int(i) < options.showUnresolved; ++i)
    {
      ResolveStats::Where w = total.unresolvedFirst[top[i].first];
      const Token& t = lexed[w.file].tokens[std::min<size_t>(w.token, lexed[w.file].tokens.size() - 1)];
      report("    %6u  %-28.*s first at %s:%u\n", top[i].second, int(symbols.name(top[i].first).size()),
             symbols.name(top[i].first).data(), files[w.file].path.string().c_str(), lineOf(files[w.file].text, t.offset));
    }
    // Done: end the process without freeing what it built. The destructors of the syntax trees,
    // the types, the instances and the IR text cost about 5 s on the full game (docs/compile-speed.md,
    // "End to end"), and the operating system takes the memory back at once. Everything the run
    // writes is closed by now. A compile server answers its client here and frees afterwards.
    if (options.packages)
    {
      std::vector<std::filesystem::path> paths;
      for (const SourceFile& f : files)
        paths.push_back(f.path);
      printPackages(build.packages, paths, parsed, symbols);
    }
    if (!options.dumpTokens)
      buildDone(codegenExit);
  }

  double seconds(std::chrono::steady_clock::duration d)
  {
    return std::chrono::duration<double>(d).count();
  }

  void reportLex(size_t fileCount, const FileStats& total, size_t symbolCount, unsigned threads, double collectTime, double lexTime)
  {
    report("files:   %zu\n", fileCount);
    report("bytes:   %llu (%.1f MB)\n", (unsigned long long)total.bytes, total.bytes / 1e6);
    report("lines:   %llu\n", (unsigned long long)total.lines);
    report("tokens:  %llu\n", (unsigned long long)total.tokens);
    report("symbols: %zu distinct identifiers\n", symbolCount);
    report("errors:  %llu\n", (unsigned long long)total.errors);
    report("threads: %u\n", threads);
    report("read:    %.3f s\n", collectTime);
    report("lex:     %.3f s  (%.0f MB/s, %.1f M lines/s, %.1f M tokens/s)\n", lexTime, total.bytes / 1e6 / lexTime,
           total.lines / 1e6 / lexTime, total.tokens / 1e6 / lexTime);
  }

