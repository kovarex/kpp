// ConvertToKpp.inl: convertToKpp: every file converted and written.
// Part of convert.cpp, included where it stood (a file of its own since 2026-11-05, moved without a change).

std::string convertToKpp(const std::vector<ConvertInput>& inputs, GlobalScope& global, const SymbolTable& symbols,
                         const std::vector<std::filesystem::path>& roots, const std::filesystem::path& outDir,
                         const std::filesystem::path& overridesDir, const SourceConfiguration* configuration,
                         const std::filesystem::path& libraryDir, const std::filesystem::path& baseDir)
{
  struct Piece
  {
    std::string text;
    Counts counts;
    std::vector<std::string> cImports;
    std::vector<std::string> todos;
    std::string source;
    bool header;
    bool scopeInterface = false; // converted from an interface header (overrides/interface.txt)
    std::vector<std::string> scopeFunctions; // and its functions
  };
  std::map<std::string, std::vector<Piece>> outputs; // output path -> header piece(s) then source piece(s)
  Counts total;
  // The C headers whose function-like macros the converted code uses (overrides/expand-macros.txt:
  // one path per line, # starts a comment). Those of a line `included PATH` are expanded only in the
  // files that include one of the headers of PATH's directory listed so, directly or through a
  // converted header, as only those see them in C++ (Lua's internals: lobject.h's hvalue, ...).
  std::map<std::string, std::pair<std::vector<std::string>, std::string>> headerMacros;
  std::map<std::string, std::string> headerAliases; // their object-like macros that name another identifier
  std::map<std::string, std::string> headerObjects; // and those that are string literals or a qualified name (LUA_NUMBER_FMT)
  std::map<std::string, std::vector<std::filesystem::path>> includedHeaders; // by directory
  // A C library's interface (overrides/c-headers.txt, file names: zlib's zlib.h and zconf.h): headers
  // that stay C. They are parsed with the library, not written, and an include of one imports it.
  std::set<std::string> cInterfaceHeaders;
  // `zstd.h kpp_zstd.h`: an include of zstd.h imports kpp_zstd.h, a header of the overrides that
  // configures it as the library's files do before their includes (`#define
  // ZSTD_STATIC_LINKING_ONLY`), written beside the converted files: kpp's import has no defines.
  std::map<std::string, std::string> cConfiguredImports;
  if (!overridesDir.empty())
  {
    std::string headersList;
    if (readFile(overridesDir / "c-headers.txt", headersList))
      for (size_t pos = 0; pos < headersList.size();)
      {
        size_t end = headersList.find('\n', pos);
        if (end == std::string::npos)
          end = headersList.size();
        std::string line = headersList.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a != std::string::npos && line[a] != '#')
        {
          std::string entry = line.substr(a, b - a + 1);
          size_t space = entry.find_first_of(" \t");
          std::string header = entry.substr(0, space);
          cInterfaceHeaders.insert(header);
          if (space != std::string::npos)
            cConfiguredImports[header] = entry.substr(entry.find_first_not_of(" \t", space));
        }
      }
  }
  // A scoped C library's interface (overrides/interface.txt, file names: zstd's zstd.h and
  // zstd_errors.h, design.md 16): headers converted into the library's scope, their number macros
  // its constants, which every file's uses write by name (cScopeConstants, filled as the headers are
  // converted, before the other files).
  std::set<std::string> cScopeHeaders;
  std::map<std::string, std::string> cScopeConstants;
  // The scoped libraries the program uses (--convert-library): their names, by library.
  std::map<std::string, std::string> scopedLibraryNames;
  std::set<std::string> scopedLibraries;
  std::set<std::string> scopedHeaders; // their interface headers' names
  // What an include of a scoped library's header still imports, by the header's name and by the
  // library's: its library's C imports (its c-imports.kpp's). A user took C's headers through the
  // library's header (Lua's override_printf.h included trio.h, and with it limits.h's UCHAR_MAX), and
  // keeps them when the header itself is the library's.
  std::map<std::string, std::vector<std::string>> scopedHeaderImports;
  for (const auto& [library, dir] : convertLibraries)
  {
    scopedLibraries.insert(library);
    std::set<std::string> headers;
    for (const std::string& name : scopedNamesOf(dir, headers))
      scopedLibraryNames.emplace(name, library);
    scopedHeaders.insert(headers.begin(), headers.end());
    std::vector<std::string> imports;
    if (std::string text; readFile(dir / "c-imports.kpp", text))
      for (size_t at = text.find("import \""); at != std::string::npos; at = text.find("import \"", at + 1))
      {
        size_t start = at + 8, end = text.find('"', start);
        if (end != std::string::npos)
          imports.push_back(text.substr(start, end - start));
      }
    scopedHeaderImports[library] = imports;
    for (const std::string& header : headers)
      scopedHeaderImports[header] = imports;
  }
  // (a name the program or its C imports declare is theirs, as a bare name resolves: Windows' BYTE,
  // which zstd's mem.h declares too; the library's is reached qualified only)
  // (but the library's own interface headers given to parse with a C library that uses it: minizip's
  // zlib.h, whose names are zlib's)
  if (!scopedLibraryNames.empty())
    for (const ConvertInput& in : inputs)
      if (in.parsed && !scopedHeaders.count(in.path.filename().string()))
        for (Decl* d : in.parsed->decls)
        {
          if (redeclaresScoped(d, symbols, scopedLibraryNames))
            continue; // (the library's, removed where the file is converted)
          const QualifiedName* name = d->kind == NodeKind::FunctionDecl ? &static_cast<FunctionDecl*>(d)->name
                                      : d->kind == NodeKind::ClassDecl  ? &static_cast<ClassDecl*>(d)->name
                                      : d->kind == NodeKind::VariableDecl ? &static_cast<VariableDecl*>(d)->name
                                      : d->kind == NodeKind::EnumDecl     ? &static_cast<EnumDecl*>(d)->name
                                                                          : nullptr;
          if (name && name->parts.size() == 1 && name->last().symbol != UINT32_MAX)
            scopedLibraryNames.erase(std::string(symbols.name(name->last().symbol)));
          else if (d->kind == NodeKind::UsingAliasDecl)
            scopedLibraryNames.erase(std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name)));
          if (d->kind == NodeKind::EnumDecl)
            for (Decl* e : static_cast<EnumDecl*>(d)->enumerators)
              scopedLibraryNames.erase(std::string(symbols.name(static_cast<EnumeratorDecl*>(e)->name)));
        }
  if (!overridesDir.empty())
  {
    std::string list;
    if (readFile(overridesDir / "interface.txt", list))
      for (size_t pos = 0; pos < list.size();)
      {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos)
          end = list.size();
        std::string line = list.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a != std::string::npos && line[a] != '#')
          cScopeHeaders.insert(line.substr(a, b - a + 1));
      }
  }
  if (!overridesDir.empty())
  {
    std::string list;
    if (readFile(overridesDir / "expand-macros.txt", list))
    {
      std::vector<std::filesystem::path> headers;
      for (size_t pos = 0; pos < list.size();)
      {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos)
          end = list.size();
        std::string line = list.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#')
          continue;
        std::string entry = line.substr(a, b - a + 1);
        if (entry.rfind("included ", 0) == 0)
        {
          std::filesystem::path path = entry.substr(entry.find_first_not_of(" \t", 9));
          includedHeaders[path.parent_path().string()].push_back(path);
        }
        else
          headers.push_back(entry);
      }
      headerMacros = readHeaderMacros(headers, nullptr, &headerAliases, &headerObjects);
    }
  }
  // The tests the conversion leaves out (overrides/left-out-tests.txt: `Suite/Test: why` per line,
  // # starts a comment; FileConverter::leftOutTests).
  std::map<std::string, std::string> leftOutTests;
  std::set<std::string> leftOutTestsFound, suitesConverted;
  if (!overridesDir.empty())
  {
    std::string list;
    if (readFile(overridesDir / "left-out-tests.txt", list))
      for (size_t pos = 0; pos < list.size();)
      {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos)
          end = list.size();
        std::string line = list.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#')
          continue;
        line = line.substr(a, b - a + 1);
        size_t colon = line.find(':');
        size_t why = colon == std::string::npos ? std::string::npos : line.find_first_not_of(" \t", colon + 1);
        if (colon == std::string::npos || line.find('/') > colon || why == std::string::npos)
        {
          fprintf(stderr, "convert: %s: `%s` is not `Suite/Test: why`\n", (overridesDir / "left-out-tests.txt").generic_string().c_str(), line.c_str());
          continue;
        }
        std::string test = line.substr(0, colon);
        while (!test.empty() && isspace((unsigned char)test.back()))
          test.pop_back();
        leftOutTests[test] = line.substr(why);
      }
  }
  // The game's own headers each input includes (the includes resolved against the including file's
  // directory, each root and each root's parent, as the game's include paths do), and the
  // function-like macros each header leaves defined: an input sees those of every header it
  // reaches, as C++ does, so that a .cpp expands the X-macro lists its header defines.
  using MacroMap = std::map<std::string, std::pair<std::vector<std::string>, std::string>>;
  auto pathKey = pathKeyOf;
  std::map<std::string, size_t> inputByPath;
  for (size_t k = 0; k < inputs.size(); ++k)
    if (inputs[k].parsed && !inputs[k].parsed->isImported && inputs[k].path.string().rfind("<", 0) != 0)
      inputByPath[pathKey(inputs[k].path)] = k;
  const std::vector<std::filesystem::path> cIncludes = cIncludeDirectories(overridesDir, roots); // (C libraries)
  std::vector<std::vector<size_t>> includedInputs(inputs.size());
  std::vector<std::set<std::string>> externalIncludes(inputs.size()); // the file names of the other includes
  // The includes written as a file of the including file's own directory that is converted too (Lua's
  // `#include "lua.h"` in lapi.c): the converted file, never an import, whatever its name.
  std::vector<std::set<std::string>> siblingIncludes(inputs.size());
  // Only the library's source files: its headers are included by the game's files too, which the
  // build compiles without it (StringUtil.cpp's lua_number2str and Benchmark.cpp's printf are the
  // C runtime's, not trio's).
  for (const auto& [key, k] : inputByPath)
    if (auto forced = forcedIncludes.find(inputs[k].path.parent_path().filename().string());
        forced != forcedIncludes.end() && (inputs[k].path.extension() == ".c" || inputs[k].path.extension() == ".cpp"))
      if (auto header = inputByPath.find(pathKey(inputs[k].path.parent_path() / forced->second)); header != inputByPath.end() && header->second != k)
        includedInputs[k].push_back(header->second);
  // A forced macro header (forcedMacroHeaders) comes first in every file of its project,
  // subdirectories too, as the build's /FI puts it.
  for (const auto& [project, forced] : forcedMacroHeaders)
  {
    size_t header = SIZE_MAX;
    for (const auto& [key, k] : inputByPath)
      if (relativeTo(inputs[k].path, roots) == forced)
        header = k;
    if (header != SIZE_MAX)
      for (const auto& [key, k] : inputByPath)
        if (k != header && relativeTo(inputs[k].path, roots).rfind(project, 0) == 0)
          includedInputs[k].insert(includedInputs[k].begin(), header);
  }
  for (const auto& [key, k] : inputByPath)
  {
    // (the includes of the branches the configuration keeps: the Windows controllers' headers
    // RazerChromaWindowsController.hpp includes under WIN32 are no Linux file's)
    const Preprocessed* decided = configuration ? configuration->impl->find(inputs[k].path) : nullptr;
    std::string_view t = decided ? std::string_view(decided->configured) : inputs[k].text;
    // `#include`, or `#  include` (zlib's trees.c, indented under its #if)
    for (size_t at = t.find("include"); at != std::string_view::npos; at = t.find("include", at + 7))
    {
      size_t hash = at;
      while (hash > 0 && (t[hash - 1] == ' ' || t[hash - 1] == '\t'))
        --hash;
      if (hash == 0 || t[hash - 1] != '#')
        continue;
      size_t lineStart = hash - 1;
      while (lineStart > 0 && (t[lineStart - 1] == ' ' || t[lineStart - 1] == '\t'))
        --lineStart;
      if (lineStart > 0 && t[lineStart - 1] != '\n')
        continue;
      size_t open = t.find_first_of("<\"\n", at + 7);
      if (open == std::string_view::npos || t[open] == '\n')
        continue;
      size_t close = t.find(t[open] == '<' ? '>' : '"', open + 1);
      if (close == std::string_view::npos || t.substr(open, close - open).find('\n') != std::string_view::npos)
        continue;
      std::filesystem::path name(std::string(t.substr(open + 1, close - open - 1)));
      std::vector<std::filesystem::path> candidates = {inputs[k].path.parent_path() / name};
      for (const auto& root : roots)
      {
        candidates.push_back(root / name);
        candidates.push_back(root.parent_path() / name);
      }
      if (inputs[k].c)
        for (const auto& directory : cIncludes)
          candidates.push_back(directory / name);
      bool converted = false;
      for (const auto& c : candidates)
        if (auto it = inputByPath.find(pathKey(c)); it != inputByPath.end() && it->second != k)
        {
          includedInputs[k].push_back(it->second);
          converted = true;
          if (&c == &candidates[0] && !name.has_parent_path())
            siblingIncludes[k].insert(name.string());
          // (C: any converted file of the library, by whatever name: zstd's "zstd_internal.h", which
          // the game's rule would take for zstd's interface by its prefix)
          else if (inputs[k].c && !cInterfaceHeaders.count(name.filename().string()))
            siblingIncludes[k].insert(name.string());
          break;
        }
      if (!converted)
        externalIncludes[k].insert(name.filename().string());
    }
  }
  // An includer macro header (includerMacroHeaders) comes first in the header that relies on it, once
  // every file that includes that header has been seen to include the macro header before it.
  for (const auto& [user, provider] : includerMacroHeaders)
  {
    size_t header = SIZE_MAX, macros = SIZE_MAX;
    for (const auto& [key, k] : inputByPath)
    {
      std::string path = relativeTo(inputs[k].path, roots);
      if (path == user)
        header = k;
      else if (path == provider)
        macros = k;
    }
    if (header == SIZE_MAX || macros == SIZE_MAX)
      continue;
    bool everyIncluder = true;
    for (const auto& [key, k] : inputByPath)
    {
      const std::vector<size_t>& included = includedInputs[k];
      auto at = std::find(included.begin(), included.end(), header);
      if (at != included.end() && std::find(included.begin(), at, macros) == at)
      {
        fprintf(stderr, "convert: %s includes %s without including %s before it, whose macros that header uses (includerMacroHeaders)\n",
                relativeTo(inputs[k].path, roots).c_str(), user.c_str(), provider.c_str());
        everyIncluder = false;
      }
    }
    if (everyIncluder)
      includedInputs[header].insert(includedInputs[header].begin(), macros);
  }
  // The headers that no file includes, directly or through other headers, from the sources the
  // build compiles, the files given alone and the force-included headers: the C++ build never
  // compiles them, so they are left out. Dead code that kpp, compiling every file, would compile:
  // ShaderProgram.hpp (the OpenGL renderer's) names the namespace VertexLayout as a type,
  // ItemStackChangeWatcher.hpp assigns an ItemWithQualityID to an ItemID. The includes are read
  // from the text, those in dropped #if branches too, so no header that may be included goes.
  // C: an #if name that nothing defines is undefined only where the library's undefined.txt says
  // the C build leaves it so; any other is an error, an #if the conversion may answer otherwise than C.
  convertErrors = 0;
  if (configuration && configuration->impl->cBuild)
  {
    std::map<std::string, std::string> unknown; // name -> where first
    for (size_t k = 0; k < inputs.size(); ++k)
      if (inputs[k].c && configuration->impl->results[k])
      {
        const Preprocessed& p = *configuration->impl->results[k];
        for (size_t n = 0; n < p.noteNames.size() && n < p.notes.size(); ++n)
          for (const std::string& name : p.noteNames[n])
            if (!configuration->impl->cKnownUndefined.count(name) && !unknown.count(name))
            {
              size_t at = std::min<size_t>(p.notes[n].first, inputs[k].text.size());
              unknown[name] = inputs[k].path.string() + ":" + std::to_string(1 + std::count(inputs[k].text.begin(), inputs[k].text.begin() + at, '\n'));
            }
      }
    for (const auto& [name, where] : unknown)
    {
      fprintf(stderr, "%s: error: an #if tests %s, which nothing the conversion reads defines and the library's undefined.txt does not list\n",
              where.c_str(), name.c_str());
      ++convertErrors;
    }
  }
  std::vector<bool> reached(inputs.size(), false);
  {
    std::vector<size_t> work;
    auto reach = [&](size_t k)
    {
      if (!reached[k])
      {
        reached[k] = true;
        work.push_back(k);
      }
    };
    for (const auto& [key, k] : inputByPath)
    {
      const std::filesystem::path& path = inputs[k].path;
      std::string ext = path.extension().string();
      std::string rel = relativeTo(path, roots);
      bool built = !fileForOtherPlatform(rel) && !filesNotBuilt.count((path.parent_path().filename() / path.filename()).generic_string());
      if (((ext == ".cpp" || ext == ".cc" || ext == ".c") && built) || forceIncludedHeaders.count(rel))
        reach(k);
    }
    for (const auto& root : roots)
      if (auto given = inputByPath.find(pathKey(root)); given != inputByPath.end()) // a file given alone (Lua/LuaCPPUtilities.hpp)
        reach(given->second);
    while (!work.empty())
    {
      size_t k = work.back();
      work.pop_back();
      for (size_t included : includedInputs[k])
        reach(included);
    }
  }
  // The macros of the `included` headers, each directory's together, and which directory each
  // header's file name belongs to.
  std::map<std::string, MacroMap> includedHeaderMacros;
  std::map<std::string, std::string> includedHeaderDirectory;
  for (const auto& [directory, paths] : includedHeaders)
  {
    includedHeaderMacros[directory] = readHeaderMacros(paths, nullptr, nullptr);
    for (const auto& path : paths)
      includedHeaderDirectory[path.filename().string()] = directory;
  }
  // Every function-like macro the converted files and the expanded C headers define, by name: what
  // callsFunction takes for no function.
  std::set<std::string> functionMacroNames;
  for (const auto& [name, macro] : headerMacros)
    functionMacroNames.insert(name);
  for (const auto& [directory, macros] : includedHeaderMacros)
    for (const auto& [name, macro] : macros)
      functionMacroNames.insert(name);
  for (const auto& [key, k] : inputByPath)
  {
    std::string_view t = inputs[k].text;
    for (size_t at = t.find("define"); at != std::string_view::npos; at = t.find("define", at + 6))
    {
      size_t hash = at;
      while (hash > 0 && (t[hash - 1] == ' ' || t[hash - 1] == '\t'))
        --hash;
      if (hash == 0 || t[hash - 1] != '#')
        continue;
      size_t n = at + 6;
      while (n < t.size() && (t[n] == ' ' || t[n] == '\t'))
        ++n;
      size_t e = n;
      while (e < t.size() && isIdentChar(t[e]))
        ++e;
      if (e > n && e < t.size() && t[e] == '(')
        functionMacroNames.insert(std::string(t.substr(n, e - n)));
    }
  }
  // C: every macro constant of a C library is written out at its uses, in the files that see it,
  // as the preprocessor does. A C macro holds in one translation unit, a kpp constant is a name of
  // the whole program: gzguts.h's `#define COPY 1` met inflate.h's enumerator COPY, inftrees.h's
  // MAXBITS Lua's, crc32.c's W the game's.
  std::set<std::string> cWrittenOutMacros;
  for (const ConvertInput& in : inputs)
    if (in.c)
    {
      cWrittenOutMacros.insert("*");
      break;
    }
  // C: the library's private names (private-names.txt), renamed with their file's name in each of
  // its files that uses one (inflate_OS): names a header of the library declares, which `private`
  // (a file's own) cannot keep to the files that include it. (A .c file's statics and types are
  // `private`, cPrivate below.)
  std::map<const ConvertInput*, std::map<std::string, std::string>> cStaticRenames;
  {
    if (configuration)
      for (const ConvertInput& in : inputs)
      {
        if (!in.c || !in.parsed || !in.tokens)
          continue;
        std::string stem = in.path.stem().string();
        for (char& ch : stem)
          if (!isIdentChar(ch))
            ch = '_';
        for (const Token& t : *in.tokens)
          if (t.getKind() == TokenKind::Identifier)
            if (std::string name(in.text.substr(t.offset, t.length)); configuration->impl->cPrivateNames.count(name))
              cStaticRenames[&in][name] = stem + "_" + name;
      }
  }
  std::map<std::string, std::set<size_t>> cInterfaceNames; // the functions the C interface headers declare: their parameter counts
  // (and a header-only library's: the declarations of a C header given alone, which holds its
  // implementation, stb_image.hpp's and fontstash.h's, which the game imports as C)
  std::set<size_t> givenCHeaders;
  for (const auto& root : roots)
    if (auto given = inputByPath.find(pathKey(root)); given != inputByPath.end() && inputs[given->second].c &&
                                                     inputs[given->second].path.extension() != ".c")
      givenCHeaders.insert(given->second);
  // C: what each .c file keeps to itself, written `private` (design.md 15): its statics (C's
  // file-level static: cover.c's and fastcover.c's g_displayLevel), the types it defines that no
  // header of the library declares (trio's math.c ieee_double_shape_type, beside Lua's
  // lmathlib.c's), its anonymous enums ("" in the set), and the functions whose declarations name
  // such a type. A C header given alone (a header-only library's implementation, stb_truetype.h):
  // its statics (its fallback packer's stbrp_init_target); its types may be its C interface's.
  std::map<const ConvertInput*, std::set<std::string>> cPrivate;
  {
    auto declName = [&](Decl* d) -> std::string
    {
      const QualifiedName* name = d->kind == NodeKind::FunctionDecl ? &static_cast<FunctionDecl*>(d)->name
                                : d->kind == NodeKind::VariableDecl ? &static_cast<VariableDecl*>(d)->name
                                : d->kind == NodeKind::ClassDecl    ? &static_cast<ClassDecl*>(d)->name
                                : d->kind == NodeKind::EnumDecl     ? &static_cast<EnumDecl*>(d)->name
                                                                    : nullptr;
      if (!name || name->empty() || name->last().symbol == UINT32_MAX)
        return {};
      return std::string(symbols.name(name->last().symbol));
    };
    // every name the library's headers spell: what they declare, and a struct a .c file defines that
    // they name (zstd.h's `typedef struct ZSTD_CDict_s ZSTD_CDict;`, deflate.h's
    // `struct static_tree_desc_s`), which is theirs then
    std::set<std::string> headerNames;
    for (size_t i = 0; i < inputs.size(); ++i)
      if (inputs[i].c && inputs[i].tokens && inputs[i].path.extension() != ".c" && !givenCHeaders.count(i))
        for (const Token& t : *inputs[i].tokens)
          if (t.getKind() == TokenKind::Identifier)
            headerNames.insert(std::string(inputs[i].text.substr(t.offset, t.length)));
    std::function<bool(Type*, const std::set<std::string>&, int)> names = [&](Type* t, const std::set<std::string>& types, int depth)
    {
      while (t && depth < 16)
      {
        ++depth;
        if (t->kind == NodeKind::PointerType)
          t = static_cast<PointerType*>(t)->pointee;
        else if (t->kind == NodeKind::ConstType)
          t = static_cast<ConstType*>(t)->inner;
        else if (t->kind == NodeKind::LRefType)
          t = static_cast<LRefType*>(t)->referenced;
        else if (t->kind == NodeKind::ArrayType)
          t = static_cast<ArrayType*>(t)->element;
        else
          break;
      }
      return t && t->kind == NodeKind::NamedType && !static_cast<NamedType*>(t)->name.empty() &&
             static_cast<NamedType*>(t)->name.last().symbol != UINT32_MAX &&
             types.count(std::string(symbols.name(static_cast<NamedType*>(t)->name.last().symbol)));
    };
    for (size_t i = 0; i < inputs.size(); ++i)
    {
      const ConvertInput& in = inputs[i];
      bool given = givenCHeaders.count(i) > 0;
      if (!in.c || !in.parsed || (in.path.extension() != ".c" && !given))
        continue;
      std::set<std::string>& own = cPrivate[&in];
      std::set<std::string> types;
      for (Decl* d : in.parsed->decls)
      {
        std::string name = declName(d);
        if ((d->flags & DF_Static) && (d->kind == NodeKind::FunctionDecl || d->kind == NodeKind::VariableDecl) && !name.empty())
          own.insert(name);
        else if (!given && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl ||
                            (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))))
        {
          if (!name.empty() && !headerNames.count(name))
          {
            own.insert(name);
            types.insert(name);
          }
          else if (name.empty() && d->kind == NodeKind::EnumDecl)
            own.insert("");
        }
      }
      if (!types.empty())
        for (Decl* d : in.parsed->decls)
          if (d->kind == NodeKind::FunctionDecl && !(d->flags & DF_Static))
          {
            auto* f = static_cast<FunctionDecl*>(d);
            bool named = names(f->returnType, types, 0);
            for (Decl* param : f->params)
              named = named || (param->kind == NodeKind::ParamDecl && names(static_cast<ParamDecl*>(param)->type, types, 0));
            if (named && !declName(d).empty())
              own.insert(declName(d));
          }
    }
  }
  for (const ConvertInput& in : inputs)
    if (in.parsed && (cInterfaceHeaders.count(in.path.filename().string()) || givenCHeaders.count(size_t(&in - inputs.data()))))
      for (Decl* d : in.parsed->decls)
        if (d->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(d)->name.empty() &&
            !(givenCHeaders.count(size_t(&in - inputs.data())) && static_cast<FunctionDecl*>(d)->body) &&
            static_cast<FunctionDecl*>(d)->name.last().symbol != UINT32_MAX)
          cInterfaceNames[std::string(symbols.name(static_cast<FunctionDecl*>(d)->name.last().symbol))].insert(static_cast<FunctionDecl*>(d)->params.size());
  std::map<size_t, MacroMap> ownMacros; // a header's macros it leaves defined, read once
  std::map<size_t, std::map<std::string, std::string>> ownObjects; // and its string and name macros
  std::map<size_t, std::map<std::string, std::string>> definedObjects; // every string or name macro it defines
  auto macrosOf = [&](size_t k) -> const MacroMap&
  {
    auto it = ownMacros.find(k);
    if (it == ownMacros.end())
      it = ownMacros.emplace(k, readHeaderMacros({inputs[k].path}, configuration, nullptr, &ownObjects[k], &definedObjects[k], &functionMacroNames,
                                                 cWrittenOutMacros.empty() ? nullptr : &cWrittenOutMacros))
             .first;
    return it->second;
  };
  // The string and name macros an input sees: all of its own (TestLuaTerritory.cpp #undefs its
  // NINE_CHUNKS at the end) and those the headers it reaches leave defined (the first to define a
  // name gives its text).
  auto visibleObjectMacros = [&](size_t k)
  {
    std::map<std::string, std::string> out;
    std::set<size_t> seen;
    std::function<void(size_t)> visit = [&](size_t h)
    {
      if (!seen.insert(h).second)
        return;
      macrosOf(h);
      for (const auto& [name, body] : h == k ? definedObjects[h] : ownObjects[h])
        out.emplace(name, body);
      for (size_t inc : includedInputs[h])
        visit(inc);
    };
    visit(k);
    return out;
  };
  auto visibleMacros = [&](size_t k)
  {
    // Depth first in include order, each header once; then the `included` headers' macros of the
    // directories whose headers the file or one of those includes.
    std::vector<const MacroMap*> out;
    std::set<size_t> seen = {k};
    std::function<void(size_t)> visit = [&](size_t h)
    {
      for (size_t inc : includedInputs[h])
        if (seen.insert(inc).second)
        {
          visit(inc);
          if (const MacroMap& m = macrosOf(inc); !m.empty())
            out.push_back(&m);
        }
    };
    visit(k);
    std::set<std::string> directories;
    for (size_t h : seen)
      for (const std::string& name : externalIncludes[h])
        if (auto directory = includedHeaderDirectory.find(name); directory != includedHeaderDirectory.end() && directories.insert(directory->second).second)
          out.push_back(&includedHeaderMacros[directory->second]);
    return out;
  };
  // Hand-written override files, parsed once: what they define replaces the converted definitions.
  std::map<std::string, std::unique_ptr<OverrideFile>> overrideFiles;
  auto overrideFor = [&](const std::string& key) -> OverrideFile*
  {
    if (overridesDir.empty())
      return nullptr;
    auto it = overrideFiles.find(key);
    if (it != overrideFiles.end())
      return it->second.get();
    std::unique_ptr<OverrideFile>& slot = overrideFiles[key];
    std::error_code ec;
    std::filesystem::path p = overridesDir / key;
    if (!std::filesystem::exists(p, ec))
      return nullptr;
    slot = std::make_unique<OverrideFile>();
    if (FILE* f = fopen(p.string().c_str(), "rb"))
    {
      char buffer[65536];
      size_t n;
      while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
        slot->text.append(buffer, n);
      fclose(f);
    }
    SymbolTable localSymbols;
    LexResult lexed = lex(slot->text, localSymbols);
    ParsedFile parsed;
    parseFile(lexed.tokens, slot->text, localSymbols, parsed, Dialect::Cpp, nullptr);
    collectMemberOverrides(parsed.decls, "", lexed.tokens, slot->text, localSymbols, slot->memberClasses);
    collectOverrideKeys(parsed.decls, "", localSymbols, slot->keys, &slot->memberClasses);
    // What is appended to the converted file: the text without the `[[members]]` classes, which
    // go into the converted classes instead.
    uint32_t at = 0;
    std::vector<std::pair<uint32_t, uint32_t>> cuts;
    for (const auto& [name, m] : slot->memberClasses)
      cuts.push_back({m.begin, m.end});
    std::sort(cuts.begin(), cuts.end());
    for (auto [begin, end] : cuts)
    {
      slot->appended += slot->text.substr(at, begin - at);
      at = end;
    }
    slot->appended += slot->text.substr(at);
    // One definition of a name replaces every overload of it (the original's parameter list
    // may hide behind a macro); several definitions match by arity.
    for (const auto& [name, count] : slot->keys.functionCounts)
      if (count == 1)
        slot->keys.functions.insert(name + "/*");
    return slot.get();
  };
  // Default arguments live on the header declaration, which the conversion drops when the
  // definition is elsewhere: carry them to the definition's parameters.
  CarriedDefaults carriedDefaults;
  CarriedTemplateDefaults carriedTemplateDefaults;
  {
    std::map<ParsedFile*, const ConvertInput*> inputOf;
    for (const ConvertInput& in : inputs)
      if (in.parsed)
        inputOf[in.parsed] = &in;
    // std:: names in carried text, mapped as the token pass maps them, after the game's classes
    // whose names the library uses are renamed (`template<class V = Vector>` is Vector2D).
    auto mapStd = [&](std::string value) { return mapStdText(std::move(value)); };
    // The defaults of a template's parameters on a declaration that goes, for the definition's
    // parameters that have none.
    auto carryTemplateDefaults = [&](Decl* declaration, Decl* def)
    {
      if (def->templateParams.size() != declaration->templateParams.size())
        return;
      auto it = inputOf.find(declaration->file);
      if (it == inputOf.end() || !it->second->tokens)
        return;
      const ConvertInput& in = *it->second;
      const std::vector<Token>& tk = *in.tokens;
      for (size_t i = 0; i < declaration->templateParams.size(); ++i)
      {
        auto* p = static_cast<TemplateParamDecl*>(declaration->templateParams[i]);
        if (!p->defaultArg || static_cast<TemplateParamDecl*>(def->templateParams[i])->defaultArg)
          continue;
        uint32_t from = p->defaultArg->token, k = from;
        int depth = 0;
        bool splitShr = false;
        for (; k < tk.size(); ++k)
        {
          if (tk[k].is(P::Less) || tk[k].is(P::LParen) || tk[k].is(P::LBracket))
            ++depth;
          else if (tk[k].is(P::RParen) || tk[k].is(P::RBracket))
            --depth;
          else if (tk[k].is(P::Greater))
          {
            if (depth == 0)
              break;
            --depth;
          }
          else if (tk[k].is(P::Shr))
          {
            if (depth <= 1)
            {
              splitShr = depth == 1; // `std::less<K>>`: the first > closes the default
              break;
            }
            depth -= 2;
          }
          else if (tk[k].is(P::Comma) && depth == 0)
            break;
          else if (tk[k].is(P::Semi))
            break;
        }
        if (k >= tk.size() || k <= from)
          continue;
        uint32_t a = tk[from].offset;
        uint32_t b = splitShr ? tk[k].offset + 1 : tk[k - 1].offset + tk[k - 1].length;
        carriedTemplateDefaults[def].push_back({i, mapStd(std::string(in.text.substr(a, b - a)))});
      }
    };
    std::function<void(Span<Decl*>)> scan = [&](Span<Decl*> decls)
    {
      for (Decl* d : decls)
      {
        if (d->kind == NodeKind::NamespaceDecl)
        {
          scan(static_cast<NamespaceDecl*>(d)->decls);
          continue;
        }
        if (d->kind == NodeKind::ClassDecl)
        {
          // `template<class K, class V, class Comp = std::less<K>> class FlatMap;`: the defaults
          // live on the forward declaration, which goes; the definition's parameters take them.
          auto* c = static_cast<ClassDecl*>(d);
          if (c->isDefinition || c->templateParams.empty())
            continue;
          Decl* defDecl = global.definitionOf(c);
          if (defDecl && defDecl != c && defDecl->kind == NodeKind::ClassDecl)
            carryTemplateDefaults(c, defDecl);
          continue;
        }
        if (d->kind != NodeKind::FunctionDecl)
          continue;
        auto* f = static_cast<FunctionDecl*>(d);
        if (f->body || (f->flags & (DF_Default | DF_Delete)))
          continue;
        if (!f->templateParams.empty())
        {
          // `template<class V = Vector> ... intersection(std::pair<V, V>, std::pair<V, V>);` in
          // the header, defined in the .cpp without the default: the definition takes it.
          Decl* defDecl = global.definitionOf(f);
          if (defDecl && defDecl != f && defDecl->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(defDecl)->body)
            carryTemplateDefaults(f, defDecl);
        }
        bool any = false;
        for (Decl* p : f->params)
          any = any || static_cast<ParamDecl*>(p)->defaultValue;
        if (!any)
          continue;
        Decl* defDecl = global.definitionOf(f);
        if (!defDecl || defDecl == f || defDecl->kind != NodeKind::FunctionDecl)
          continue;
        auto* def = static_cast<FunctionDecl*>(defDecl);
        if (!def->body || def->params.size() != f->params.size())
          continue;
        auto it = inputOf.find(f->file);
        if (it == inputOf.end() || !it->second->tokens)
          continue;
        const ConvertInput& in = *it->second;
        for (size_t i = 0; i < f->params.size(); ++i)
        {
          Expr* dv = static_cast<ParamDecl*>(f->params[i])->defaultValue;
          if (!dv || static_cast<ParamDecl*>(def->params[i])->defaultValue)
            continue;
          uint32_t end = parameterEndIn(*in.tokens, dv->token);
          if (end == UINT32_MAX || end <= dv->token)
            continue;
          // The text after the parameter's `=` (the expression node's token can sit inside it:
          // `std::string()` starts at its parenthesis), with std:: names mapped as elsewhere.
          uint32_t fromTok = dv->token;
          for (uint32_t k = f->params[i]->token, depth = 0; k < end; ++k)
          {
            const Token& tk = (*in.tokens)[k];
            if (tk.is(P::LParen) || tk.is(P::LBracket) || tk.is(P::LBrace))
              ++depth;
            else if (tk.is(P::RParen) || tk.is(P::RBracket) || tk.is(P::RBrace))
              --depth;
            else if (tk.is(P::Assign) && depth == 0)
            {
              fromTok = k + 1;
              break;
            }
          }
          uint32_t from = (*in.tokens)[fromTok].offset;
          uint32_t to = (*in.tokens)[end - 1].offset + (*in.tokens)[end - 1].length;
          carriedDefaults[def].push_back({i, mapStd(std::string(in.text.substr(from, to - from)))});
        }
      }
    };
    for (const ConvertInput& in : inputs)
      if (in.parsed && !in.parsed->isImported)
        scan(in.parsed->decls);
  }
  ScopeNames scopeNames;
  std::set<std::string> opaqueKept;
  std::set<std::string> macroClasses = collectMacroClasses(inputs);
  InlineBodies inlineBodies;
  for (const ConvertInput& in : inputs)
    if (in.parsed && !in.parsed->isImported && !in.c)
      collectInlineBodies(in.parsed->decls, false, global, symbols, inlineBodies);
  total.add("member function bodies written in their classes", inlineBodies.definitions.size());
  for (const auto& [why, n] : inlineBodies.kept)
    total.add("member function bodies kept outside their classes: " + why, n);
  std::set<std::string> braceableClasses;
  for (const ConvertInput& in : inputs)
    if (in.parsed && !in.parsed->isImported && !in.c)
      collectBraceableClasses(in.parsed->decls, global, symbols, braceableClasses);
  Variants variants;
  std::map<std::string, ClassBases> classBases;
  for (const ConvertInput& in : inputs)
    if (in.parsed && !in.parsed->isImported)
    {
      collectScopeNames(in.parsed->decls, "", symbols, scopeNames);
      collectClassBases(in.parsed->decls, "", symbols, classBases);
      if (in.tokens)
        collectVariants(in.parsed->decls, "", *in.tokens, in.text, symbols, variants);
    }
  addInheritedNames(scopeNames, classBases);
  // The .cpp files' namespace-scope aliases whose names another file declares as an alias too
  // (Cliff.cpp's and CliffPrototype.cpp's `using CO = CliffOrientation;`, EntityPrototypeFlags'
  // and ItemPrototypeFlags' different FlagPairTypes, the rocket silo's MyBinder beside 140
  // function-local ones): each gets its file's name after it.
  std::vector<AliasDeclaration> aliases;
  for (const ConvertInput& in : inputs)
    if (in.parsed && !in.parsed->isImported)
      collectAliases(in.parsed->decls, &in, true, symbols, aliases);
  std::map<std::string, std::set<const void*>> aliasFiles;
  for (const AliasDeclaration& a : aliases)
    aliasFiles[a.name].insert(a.file);
  std::map<const void*, std::map<std::string, std::string>> fileAliasRenames;
  for (const AliasDeclaration& a : aliases)
  {
    const ConvertInput& in = *static_cast<const ConvertInput*>(a.file);
    std::string ext = in.path.extension().string();
    if (!a.namespaceScope || (ext != ".cpp" && ext != ".cc" && ext != ".c") || aliasFiles[a.name].size() < 2)
      continue;
    std::string stem;
    for (char c : in.path.stem().string())
      if (isIdentChar(c))
        stem += c;
    fileAliasRenames[a.file][a.name] = a.name + "Of" + stem;
  }
  // A source file's static function named as one of the library's free functions (ltablib.c's
  // `static int sort (lua_State *L)`, beside the library's sort): file-local in C and C++, a
  // program-wide name in kpp, which the library's has already, so it gets its file's name after it.
  std::set<std::string> libraryNames = libraryFunctionNames(libraryDir);
  std::set<std::string> libraryTypes = libraryTypeNames(libraryDir);
  for (const ConvertInput& in : inputs)
  {
    std::string ext = in.path.extension().string();
    if (!in.parsed || in.parsed->isImported || (ext != ".cpp" && ext != ".cc" && ext != ".c"))
      continue;
    for (Decl* d : in.parsed->decls)
      if (d->kind == NodeKind::FunctionDecl && (d->flags & DF_Static))
      {
        auto* f = static_cast<FunctionDecl*>(d);
        if (f->name.parts.size() != 1 || f->name.last().symbol == UINT32_MAX)
          continue;
        std::string name(symbols.name(f->name.last().symbol));
        if (!libraryNames.count(name))
          continue;
        std::string stem;
        for (char c : in.path.stem().string())
          if (isIdentChar(c))
            stem += c;
        fileAliasRenames[&in][name] = name + "Of" + stem;
      }
  }
  // The converted files' class definitions and aliases by their own name (FileConverter::classesByName).
  std::map<std::string, std::vector<Decl*>> classesByName;
  {
    std::function<void(Span<Decl*>)> collect = [&](Span<Decl*> decls)
    {
      for (Decl* d : decls)
        if (d->kind == NodeKind::NamespaceDecl)
          collect(static_cast<NamespaceDecl*>(d)->decls);
        else if (d->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(d)->isDefinition && !static_cast<ClassDecl*>(d)->name.empty() &&
                 static_cast<ClassDecl*>(d)->name.last().symbol != UINT32_MAX)
        {
          classesByName[std::string(symbols.name(static_cast<ClassDecl*>(d)->name.last().symbol))].push_back(d);
          collect(static_cast<ClassDecl*>(d)->members);
        }
        else if (d->kind == NodeKind::UsingAliasDecl)
          classesByName[std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name))].push_back(d);
    };
    for (const ConvertInput& in : inputs)
      if (in.parsed && !in.parsed->isImported)
        collect(in.parsed->decls);
  }
  // The files left out below, for the classes defined only there (FileConverter::leftOutFiles).
  std::set<const ParsedFile*> leftOutFiles;
  for (const ConvertInput& in : inputs)
  {
    std::string rel = relativeTo(in.path, roots);
    if (in.parsed && (fileForOtherPlatform(rel) || (inputByPath.count(pathKey(in.path)) && !reached[&in - inputs.data()]) ||
                      filesNotBuilt.count((in.path.parent_path().filename() / in.path.filename()).generic_string())))
      leftOutFiles.insert(in.parsed);
  }
  // (the interface headers first: the constants they make are written by name in the other files)
  std::vector<const ConvertInput*> converted;
  for (const ConvertInput& in : inputs)
    if (in.c && cScopeHeaders.count(in.path.filename().string()))
      converted.push_back(&in);
  for (const ConvertInput& in : inputs)
    if (!(in.c && cScopeHeaders.count(in.path.filename().string())))
      converted.push_back(&in);
  for (const ConvertInput* convertedInput : converted)
  {
    const ConvertInput& in = *convertedInput;
    if (!in.parsed || in.parsed->isImported || in.path.string().rfind("<", 0) == 0)
      continue;
    std::string ext = in.path.extension().string();
    // A .c file is converted as the C++ the game compiles it as (Lua's, fbuild.bff's `/TP`).
    if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc" && ext != ".inl" && ext != ".c")
      continue;
    std::string rel = relativeTo(in.path, roots);
    if (fileForOtherPlatform(rel))
    {
      total.add("files of other platforms left out");
      continue;
    }
    if (filesNotBuilt.count((in.path.parent_path().filename() / in.path.filename()).generic_string()))
    {
      total.add("files the build leaves out");
      continue;
    }
    if (inputByPath.count(pathKey(in.path)) && !reached[&in - inputs.data()])
    {
      total.add("headers no file includes left out");
      continue;
    }
    // (A header given alone that the library's defines configure otherwise than its import holds a
    // header-only library's implementation, fontstash.h's under FONTSTASH_IMPLEMENTATION: converted,
    // without what its import declares. zlib.h, given to minizip's conversion, stays C.)
    std::string interfaceText;
    if (cInterfaceHeaders.count(in.path.filename().string()))
    {
      size_t index = size_t(&in - inputs.data());
      const Preprocessed* full = configuration && givenCHeaders.count(index) ? configuration->impl->of(index) : nullptr;
      if (full)
        interfaceText = configuration->impl->interfaceConfigured(index);
      // (By the lines each keeps: the parse's blanking, `FAR` in zconf.h's typedefs, is no branch.)
      auto keptLines = [](const std::string& t)
      {
        std::vector<bool> kept;
        bool any = false;
        for (char c : t)
          if (c == '\n')
          {
            kept.push_back(any);
            any = false;
          }
          else if (!isspace((unsigned char)c))
            any = true;
        kept.push_back(any);
        return kept;
      };
      if (!full || keptLines(interfaceText) == keptLines(full->configured))
      {
        total.add("the C library's interface headers kept as C (c-headers.txt)");
        continue;
      }
    }
    std::string key = rel.substr(0, rel.size() - ext.size()) + ".kpp";
    FileConverter fc(in, global, symbols);
    fc.cInterfaceHeaders = &cInterfaceHeaders;
    fc.cInterfaceNames = &cInterfaceNames;
    fc.cGivenHeader = givenCHeaders.count(size_t(&in - inputs.data())) > 0;
    if (!interfaceText.empty())
      fc.cInterfaceText = &interfaceText;
    if (configuration)
      fc.cPrivateNames = &configuration->impl->cPrivateNames;
    if (auto renames = cStaticRenames.find(&in); renames != cStaticRenames.end())
      fc.cStaticRenames = &renames->second;
    if (auto own = cPrivate.find(&in); own != cPrivate.end())
      fc.cPrivate = &own->second;
    fc.cConfiguredImports = &cConfiguredImports;
    fc.cWrittenOutMacros = &cWrittenOutMacros;
    fc.cScopeHeaders = &cScopeHeaders;
    fc.cScopeConstants = &cScopeConstants;
    fc.scopedLibraryNames = &scopedLibraryNames;
    fc.scopedLibraries = &scopedLibraries;
    fc.scopedHeaders = &scopedHeaders;
    fc.scopedHeaderImports = &scopedHeaderImports;
    fc.leftOutFiles = &leftOutFiles;
    fc.functionMacroNames = &functionMacroNames;
    fc.classesByName = &classesByName;
    fc.leftOutTests = &leftOutTests;
    fc.leftOutTestsFound = &leftOutTestsFound;
    fc.suitesConverted = &suitesConverted;
    if (auto kept = keptCLibraries.find(in.path.parent_path().filename().string()); kept != keptCLibraries.end())
      fc.keptCLibraries = &kept->second;
    fc.scopeNames = &scopeNames;
    fc.libraryTypes = &libraryTypes;
    fc.opaqueKept = &opaqueKept;
    fc.macroClasses = &macroClasses;
    fc.braceableClasses = &braceableClasses;
    fc.inlineBodies = &inlineBodies;
    fc.headerMacros = &headerMacros;
    fc.headerAliases = &headerAliases;
    if (auto renames = fileAliasRenames.find(&in); renames != fileAliasRenames.end())
      fc.fileAliasRenames = renames->second;
    if (configuration)
      fc.preprocessed = configuration->impl->find(in.path);
    if (configuration && in.c)
      fc.cMacros = &configuration->impl->cMacros;
    if (auto ip = inputByPath.find(pathKey(in.path)); ip != inputByPath.end())
    {
      fc.includedMacros = visibleMacros(ip->second);
      fc.objectMacros = visibleObjectMacros(ip->second);
      // The converter's own rewrite of a name stands over a header's definition of it, which may
      // differ by configuration: AlwaysInline.hpp's ALWAYS_INLINE is nothing in DEBUG and
      // __forceinline in Release, [[forceinline]] in both (design.md 12).
      if (!in.c)
        for (const auto& [name, value] : knownMacros)
          fc.objectMacros.erase(name);
      // C: a header holds in the files that include it, with what they define before it (trees.h's
      // tables are D_CODES long, deflate.h's macro, which trees.c includes first).
      if (in.c && fc.isHeader())
        for (size_t k = 0; k < inputs.size(); ++k)
          if (k != ip->second && std::find(includedInputs[k].begin(), includedInputs[k].end(), ip->second) != includedInputs[k].end())
            for (const auto& [name, body] : visibleObjectMacros(k))
              fc.objectMacros.emplace(name, body);
      // C: a define of the library's that is a name holds in every file, written out at its uses
      // (stb_image_write's `STBIW_ZLIB_COMPRESS(data, data_len, out_len, quality)`)
      if (in.c && configuration)
        for (const auto& [name, body] : configuration->impl->cNameDefines)
          fc.objectMacros.emplace(name, body);
      fc.siblingIncludes = &siblingIncludes[ip->second];
      for (const auto& [name, body] : headerObjects)
        if (!knownMacros.count(name)) // the converter's own rewrite (SDL_PRIs64) stands
          fc.objectMacros.emplace(name, body);
    }
    fc.variants = &variants;
    fc.carriedDefaults = &carriedDefaults;
    fc.carriedTemplateDefaults = &carriedTemplateDefaults;
    if (OverrideFile* ov = overrideFor(key))
    {
      fc.overrides = &ov->keys;
      fc.memberOverrides = &ov->memberClasses;
    }
    Piece piece;
    piece.text = fc.run();
    piece.counts = fc.counts;
    piece.todos = fc.todos;
    piece.cImports = fc.cImports;
    piece.source = in.path.string();
    piece.header = ext != ".cpp" && ext != ".cc" && ext != ".c";
    piece.scopeInterface = in.c && cScopeHeaders.count(in.path.filename().string());
    piece.scopeFunctions = fc.scopeFunctions;
    outputs[key].push_back(std::move(piece));
    for (auto& [k, v] : fc.counts.values)
      total.add(k, v);
  }
  // (A conversion without the tests, the headline's, has none to find.)
  for (const auto& [test, why] : leftOutTests)
    if (!suitesConverted.empty() && !leftOutTestsFound.count(test))
      fprintf(stderr, "convert: %s: no converted test is %s\n", (overridesDir / "left-out-tests.txt").generic_string().c_str(), test.c_str());
  // Where a file is written: below its root's name (its key, by which its overrides are found), or
  // with --convert-base, at its root's path below that directory, so that the output mirrors the
  // source tree (C:/Factorio/libraries/Lua/lapi.c as libraries/Lua/lapi.kpp).
  std::map<std::string, std::string> rootPaths;
  if (!baseDir.empty())
  {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::absolute(baseDir, ec).lexically_normal();
    for (const auto& root : roots)
    {
      std::filesystem::path r = std::filesystem::absolute(root, ec).lexically_normal();
      if (!std::filesystem::is_directory(r, ec))
        r = r.parent_path();
      std::filesystem::path rel = r.lexically_relative(base);
      if (!rel.empty() && rel.native()[0] != '.')
        rootPaths[r.filename().generic_string()] = rel.generic_string();
    }
  }
  auto outputKey = [&](const std::string& key) -> std::string
  {
    size_t slash = key.find('/');
    if (slash != std::string::npos)
      if (auto it = rootPaths.find(key.substr(0, slash)); it != rootPaths.end())
        return it->second + key.substr(slash);
    return key;
  };
  // The configured imports' headers (c-headers.txt), beside the library's converted files.
  for (const auto& [header, configured] : cConfiguredImports)
  {
    std::string text;
    if (!readFile(overridesDir / configured, text))
    {
      fprintf(stderr, "convert: %s: no %s for c-headers.txt's `%s %s`\n", overridesDir.generic_string().c_str(), configured.c_str(), header.c_str(),
              configured.c_str());
      continue;
    }
    std::error_code ec;
    // (beside the header it configures: zstd.h's directory)
    std::vector<std::filesystem::path> beside;
    for (const ConvertInput& in : inputs)
      if (in.path.filename().string() == header)
        beside.push_back(in.path.parent_path());
    for (const auto& root : roots)
      beside.push_back(root);
    for (const auto& directory : beside)
      if (std::filesystem::is_directory(directory, ec))
      {
        std::filesystem::path outPath = outDir / outputKey(relativeTo(directory / configured, roots));
        std::filesystem::create_directories(outPath.parent_path(), ec);
        writeConvertedFile(outPath, text);
        total.add("configured imports written (c-headers.txt)");
        break;
      }
  }
  std::string report = "# Conversion report\n\n";
  size_t files = 0;
  // Each library's (each root's) C imports, in its files' include order, and the names its files use
  // of the headers C++'s standard headers bring in (writeLibraryImports).
  struct LibraryImports
  {
    std::vector<std::string> imports;
    std::map<std::string, std::set<std::string>> used; // padding header -> names
  };
  std::map<std::string, LibraryImports> libraryImports;
  // The namespaces some file holds something in (withoutEmptyNamespaces keeps a file's empty one
  // that is the only declaration of its namespace): the converted files, their hand-written parts and
  // the added files.
  std::set<std::string> holdingNamespaces;
  for (const auto& [key, pieces] : outputs)
    for (const Piece& p : pieces)
      noteNamespacesHolding(p.text, holdingNamespaces);
  for (const auto& [key, ov] : overrideFiles)
    if (ov) // (a key without an override file has none)
      noteNamespacesHolding(ov->appended, holdingNamespaces);
  if (!overridesDir.empty())
  {
    std::error_code ec;
    if (std::filesystem::is_directory(overridesDir / "added", ec))
      for (const auto& entry : std::filesystem::recursive_directory_iterator(overridesDir / "added", ec))
        if (std::string addedText; entry.is_regular_file(ec) && readFile(entry.path(), addedText))
          noteNamespacesHolding(addedText, holdingNamespaces);
  }
  auto rootOf = [](const std::string& key) { return key.find('/') == std::string::npos ? std::string() : key.substr(0, key.find('/')); };
  auto noteLibraryText = [&](const std::string& root, const std::string& text)
  {
    LibraryImports& library = libraryImports[root];
    std::set<std::string> identifiers = identifiersOf(text);
    for (const auto& [header, names] : paddingHeaderNames())
      for (const std::string& name : identifiers)
        if (names.count(name))
          library.used[header].insert(name);
  };
  for (auto& [key, pieces] : outputs)
  {
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.header && !b.header; });
    std::filesystem::path outPath = outDir / outputKey(key);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    OverrideFile* ov = overrideFor(key);
    // (no line naming the sources: the file reads as the program's own, kovarex 2026-11-04;
    // CONVERSION.md lists what each file was converted from)
    std::string text;
    // (the line kppc --convert-library finds a library's interface files by: scopedNamesOf)
    if (std::any_of(pieces.begin(), pieces.end(), [](const Piece& p) { return p.scopeInterface; }))
    {
      text += std::string(scopeInterfaceLine) + "\n";
      // (its functions, whose declarations go: their definitions are in the library's files)
      std::vector<std::string> functions;
      for (const Piece& p : pieces)
        functions.insert(functions.end(), p.scopeFunctions.begin(), p.scopeFunctions.end());
      std::string line = scopeFunctionsLine;
      bool first = true;
      for (const std::string& name : functions)
      {
        if (line.size() + name.size() + 2 > 110)
        {
          text += line + ",\n";
          line = "//  ";
          first = true;
        }
        line += (first ? " " : ", ") + name;
        first = false;
      }
      if (!functions.empty())
        text += line + "\n";
      text += "\n";
    }
    for (const Piece& p : pieces)
    {
      text += std::string_view(p.text).substr(byteOrderMarkLength(p.text));
      if (text.empty() || text.back() != '\n')
        text += '\n';
      text += '\n';
    }
    text = resolveInlineBodies(std::move(text), total);
    if (ov)
    {
      // The hand-written part: appended, its definitions replaced the converted ones above.
      text += "// --- Hand-written, from " + (overridesDir / key).generic_string() + " ---\n\n" + ov->appended;
      if (text.back() != '\n')
        text += '\n';
      total.add("files with overrides");
      for (const auto& [name, m] : ov->memberClasses)
        if (!m.used)
        {
          // Members for a class this file does not define would be lost: say so.
          fprintf(stderr, "convert: %s: the members of %s have no class to join\n", (overridesDir / key).generic_string().c_str(), name.c_str());
          total.add("hand-written members with no class");
        }
    }
    size_t namespacesRemoved = 0, namespacesKept = 0;
    text = withoutEmptyNamespaces(text, holdingNamespaces, namespacesRemoved, namespacesKept);
    total.add("empty namespaces removed", namespacesRemoved);
    total.add("empty namespaces kept: the program's only declaration of their namespace", namespacesKept);
    size_t blankLinesRemoved = 0;
    text = withTidyBlankLines(text, blankLinesRemoved);
    total.add("blank lines removed (runs, at the start and end, inside braces)", blankLinesRemoved);
    for (const Piece& p : pieces)
      for (const std::string& header : p.cImports)
        if (std::vector<std::string>& imports = libraryImports[rootOf(key)].imports; std::find(imports.begin(), imports.end(), header) == imports.end())
          imports.push_back(header);
    noteLibraryText(rootOf(key), text);
    if (writeConvertedFile(outPath, text))
      ++files;
    report += "## " + outputKey(key) + "\n";
    for (const Piece& p : pieces)
    {
      report += "- " + std::filesystem::path(p.source).filename().string() + ":";
      for (auto& [k, v] : p.counts.values)
        report += " " + k + " " + std::to_string(v) + ";";
      report += "\n";
      for (const std::string& t : p.todos)
        report += "  - TODO " + t + "\n";
    }
  }
  // Hand-written files with no C++ counterpart (overrides/added/...), written into the output at
  // their path below added/: the DirectX backend's COM interfaces and CComPtr.
  if (!overridesDir.empty())
  {
    std::error_code ec;
    std::filesystem::path added = overridesDir / "added";
    if (std::filesystem::is_directory(added, ec))
      for (const auto& entry : std::filesystem::recursive_directory_iterator(added, ec))
        if (entry.is_regular_file(ec))
        {
          std::string addedKey = entry.path().lexically_relative(added).generic_string();
          // (only for a library this conversion converts: Linux's leaves StackWalker out; a C
          // library's added files are named by their path below the base, libraries/NAME/...)
          std::string addedRoot = addedKey.substr(0, addedKey.find('/'));
          if (addedRoot != "libraries" && std::none_of(roots.begin(), roots.end(), [&](const std::filesystem::path& r) { return r.filename().generic_string() == addedRoot; }))
            continue;
          std::filesystem::path rel = outputKey(addedKey);
          std::filesystem::path target = outDir / rel;
          std::string addedText;
          if (!readFile(entry.path(), addedText))
            continue;
          noteLibraryText(rootOf(addedKey), addedText);
          std::filesystem::create_directories(target.parent_path(), ec);
          if (writeConvertedFile(target, addedText))
          {
            ++files;
            total.add("hand-written files added");
            report += "## " + rel.generic_string() + "\n- added from " + entry.path().generic_string() + "\n";
          }
        }
  }
  // Each library's C imports, once, in its c-imports.kpp: an import is the program's wherever it is
  // written (design.md 5), so its files start with their code. In the include order of its files,
  // not sorted: a Windows header may need another before it (dbghelp.h and psapi.h windows.h). The
  // C headers that C++'s standard headers bring in (stdlib.h, string.h, stdio.h: <string> and
  // <memory> include them) are imported only where a file uses a name of theirs it includes no
  // header for, with those names in a comment.
  for (const auto& [root, library] : libraryImports)
  {
    std::string lines;
    size_t count = 0;
    for (const auto& [header, names] : library.used)
      if (std::find(library.imports.begin(), library.imports.end(), header) == library.imports.end())
      {
        std::string shown;
        size_t n = 0;
        for (const std::string& name : names)
          if (n++ < 4)
            shown += (shown.empty() ? "" : ", ") + name;
        lines += "extern \"C\" import \"" + header + "\"; // " + shown + (names.size() > 4 ? ", ..." : "") + " (C++'s headers brought it in)\n";
        ++count;
        total.add("C imports for names C++'s headers brought in (c-imports.kpp)");
      }
    for (const std::string& header : library.imports)
    {
      lines += "extern \"C\" import \"" + header + "\";\n";
      ++count;
    }
    if (!count)
      continue;
    std::string text = "// The C headers this library imports, written once by kppc --convert from its files' #includes:\n"
                       "// an import is the whole program's wherever it is written (design.md 5), so the files start with\n"
                       "// their code. In the include order of the files (a Windows header may need another before it).\n\n" +
                       lines;
    std::filesystem::path outPath = outDir / outputKey(root.empty() ? std::string("c-imports.kpp") : root + "/c-imports.kpp");
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (writeConvertedFile(outPath, text))
      ++files;
    total.add("library C import files written (c-imports.kpp)");
    total.add("C imports written (c-imports.kpp)", count);
    report += "## " + outputKey(root.empty() ? std::string("c-imports.kpp") : root + "/c-imports.kpp") + "\n- the library's C imports: " + std::to_string(count) + "\n";
  }
  std::string summary = "Files written: " + std::to_string(files) + "\n\n";
  for (auto& [k, v] : total.values)
    summary += "- " + k + ": " + std::to_string(v) + "\n";
  report.insert(strlen("# Conversion report\n\n"), summary + "\n");
  writeConvertedFile(outDir / "CONVERSION.md", report);
  return summary;
}

// --- the game's two configurations made one (design.md 12, "The game's DEBUG as config::debug") ---

