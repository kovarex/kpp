// HeaderMacros.inl: the headers' macros, and SourceConfiguration.
// Part of convert.cpp, included where it stood (a file of its own since 2026-11-05, moved without a change).

std::map<std::string, std::pair<std::vector<std::string>, std::string>> readHeaderMacros(const std::vector<std::filesystem::path>& paths,
                                                                                       const SourceConfiguration* configuration,
                                                                                       std::map<std::string, std::string>* aliases,
                                                                                       std::map<std::string, std::string>* objects = nullptr,
                                                                                       std::map<std::string, std::string>* definedObjects = nullptr,
                                                                                       const std::set<std::string>* functionMacroNames = nullptr,
                                                                                       const std::set<std::string>* writtenOut = nullptr)
{
  std::map<std::string, std::pair<std::vector<std::string>, std::string>> macros;
  for (const std::filesystem::path& path : paths)
  {
    std::string raw;
    if (const Preprocessed* decided = configuration ? configuration->impl->find(path) : nullptr)
      raw = decided->configured; // a converted header: its branches as the configuration selects them with its includes
    else if (readFile(path, raw))
      raw = preprocessText(raw, nullptr).configured;
    else
    {
      fprintf(stderr, "convert: cannot read the macro header %s\n", path.string().c_str());
      continue;
    }
    raw.erase(0, byteOrderMarkLength(raw));
    // The dropped #if branches are blank; comments out (they may hold a #define or span lines).
    std::string text;
    for (size_t i = 0; i < raw.size();)
    {
      if (raw.compare(i, 2, "/*") == 0)
      {
        size_t end = raw.find("*/", i + 2);
        end = end == std::string::npos ? raw.size() : end + 2;
        for (size_t k = i; k < end; ++k)
          text += raw[k] == '\n' ? '\n' : ' ';
        i = end;
      }
      else if (raw.compare(i, 2, "//") == 0)
      {
        while (i < raw.size() && raw[i] != '\n')
          ++i;
      }
      else if (raw[i] == '"' || (raw[i] == '\'' && !isDigitSeparator(raw, i)))
      {
        char quote = raw[i];
        text += raw[i++];
        while (i < raw.size() && raw[i] != quote && raw[i] != '\n')
        {
          if (raw[i] == '\\' && i + 1 < raw.size())
            text += raw[i++];
          text += raw[i++];
        }
        if (i < raw.size() && raw[i] == quote)
          text += raw[i++];
      }
      else
        text += raw[i++];
    }
    for (size_t pos = 0; pos < text.size();)
    {
      // A logical line, continuations joined.
      std::string line;
      while (pos < text.size())
      {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
          end = text.size();
        std::string part = text.substr(pos, end - pos);
        pos = end + 1;
        while (!part.empty() && (part.back() == '\r' || part.back() == ' ' || part.back() == '\t'))
          part.pop_back();
        bool continued = !part.empty() && part.back() == '\\';
        if (continued)
          part.pop_back();
        line += part + " ";
        if (!continued)
          break;
      }
      size_t p = line.find_first_not_of(" \t");
      if (p == std::string::npos || line[p] != '#')
        continue;
      p = line.find_first_not_of(" \t", p + 1);
      if (p != std::string::npos && line.compare(p, 5, "undef") == 0)
      {
        // What the header leaves defined is what its includers see.
        size_t n = line.find_first_not_of(" \t", p + 5), e = n;
        while (e < line.size() && (isalnum((unsigned char)line[e]) || line[e] == '_'))
          ++e;
        if (n != std::string::npos)
        {
          macros.erase(line.substr(n, e - n));
          if (objects)
            objects->erase(line.substr(n, e - n));
        }
        continue;
      }
      if (p == std::string::npos || line.compare(p, 6, "define") != 0)
        continue;
      p = line.find_first_not_of(" \t", p + 6);
      if (p == std::string::npos)
        continue;
      size_t nameEnd = p;
      while (nameEnd < line.size() && (isalnum((unsigned char)line[nameEnd]) || line[nameEnd] == '_'))
        ++nameEnd;
      if (nameEnd == p || nameEnd >= line.size() || line[nameEnd] != '(')
      {
        // Object-like: the import makes the constants. One whose body is another name (glad's
        // `#define glGenTextures glad_glGenTextures`, the function pointer the loader fills) is an
        // alias the converted code is written through; when that name is a function-like macro
        // (lobject.h's `#define setobj2s setobj`), the alias is that macro under its own name.
        if (nameEnd > p)
        {
          std::string body = line.substr(nameEnd);
          size_t a = body.find_first_not_of(" \t"), b = body.find_last_not_of(" \t");
          body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
          static const std::set<std::string> keywords = {"extern", "static", "const", "volatile", "inline", "void", "char", "short", "int", "long",
                                                         "float", "double", "signed", "unsigned", "struct", "union", "enum", "typedef", "register"};
          bool identifier = !body.empty() && (isalpha((unsigned char)body[0]) || body[0] == '_');
          for (char c : body)
            identifier = identifier && (isalnum((unsigned char)c) || c == '_');
          if (identifier && !keywords.count(body))
          {
            if (auto macro = macros.find(body); macro != macros.end())
              macros[line.substr(p, nameEnd - p)] = macro->second;
            else if (aliases)
              (*aliases)[line.substr(p, nameEnd - p)] = body;
          }
        }
        // One whose body is string literals (`#define UTF8_BULLET "\xE2\x80\xA2"`: the conversion
        // makes it a constant, and a use inside a literal concatenation gets its text) or a
        // qualified name (`#define vkCreateInstance VulkanImpl::CreateInstanceWrapper`: every use
        // gets it).
        if ((objects || definedObjects) && nameEnd > p)
        {
          std::string body = line.substr(nameEnd);
          size_t a = body.find_first_not_of(" \t"), b = body.find_last_not_of(" \t");
          body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
          // Another name for one of them (luaconf.h's `#define LUALIB_API LUA_API`): its text.
          if (objects && objects->count(body))
            body = objects->at(body);
          // Made of string macros and literals (lua.h's `#define LUA_RELEASE LUA_VERSION "."
          // LUA_VERSION_RELEASE`, luaconf.h's `#define LUA_QS LUA_QL("%s")`): their literals.
          if (!isStringLiterals(body) && objects)
            if (std::string literals = stringMacroText(body, *objects, macros); !literals.empty())
              body = literals;
          if (!isStringLiterals(body) && !isQualifiedName(body) && isSplicedMacroBody(body) && objects)
            body = splicedMacroText(body, *objects);
          // One that calls a function (callsFunction) is written out too.
          if (functionMacroNames && objects && !isStringLiterals(body) && callsFunction(body, *functionMacroNames))
            body = splicedMacroText(body, *objects);
          // (An alias of a function-like macro, lobject.h's `#define setobj2s setobj`, is that macro, above.)
          if (isStringLiterals(body) || isQualifiedName(body) || isSplicedMacroBody(body) ||
              (writtenOut && (writtenOut->count("*") || writtenOut->count(line.substr(p, nameEnd - p)))) ||
              (isAliasMacro(line.substr(p, nameEnd - p), body) && !macros.count(body)) ||
              (functionMacroNames && callsFunction(body, *functionMacroNames)))
          {
            if (objects)
              (*objects)[line.substr(p, nameEnd - p)] = body;
            if (definedObjects)
              (*definedObjects)[line.substr(p, nameEnd - p)] = body;
          }
        }
        continue;
      }
      size_t close = line.find(')', nameEnd);
      if (close == std::string::npos)
        continue;
      std::vector<std::string> params;
      std::string paramText = line.substr(nameEnd + 1, close - nameEnd - 1);
      for (size_t start = 0; start <= paramText.size();)
      {
        size_t comma = paramText.find(',', start);
        std::string param = paramText.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t a = param.find_first_not_of(" \t"), b = param.find_last_not_of(" \t");
        if (a != std::string::npos)
          params.push_back(param.substr(a, b - a + 1));
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
      std::string body = line.substr(close + 1);
      size_t a = body.find_first_not_of(" \t"), b = body.find_last_not_of(" \t");
      body = a == std::string::npos ? std::string() : collapsedSpaces(body.substr(a, b - a + 1));
      // C's NULL is a cast of 0 to void*, not a constant the import makes.
      for (size_t at = body.find("NULL"); at != std::string::npos; at = body.find("NULL", at + 1))
        if ((at == 0 || !(isalnum((unsigned char)body[at - 1]) || body[at - 1] == '_')) &&
            (at + 4 >= body.size() || !(isalnum((unsigned char)body[at + 4]) || body[at + 4] == '_')))
          body.replace(at, 4, "nullptr");
      if (!body.empty())
        macros[line.substr(p, nameEnd - p)] = {params, body};
    }
  }
  return macros;
}

SourceConfiguration::SourceConfiguration(const std::vector<std::filesystem::path>& paths, const std::vector<std::string_view>& texts,
                                         const std::vector<std::filesystem::path>& roots, bool cSources,
                                         const std::filesystem::path& overridesDir)
  : impl(std::make_unique<Impl>())
{
  if (cSources)
  {
    impl->includeDirectories = cIncludeDirectories(overridesDir, roots);
    impl->cBuild = true;
    impl->cDefines = cLibraryDefines();
    impl->cLibraryBase = impl->cDefines;
    // The library's own: `NAME` or `NAME=VALUE` a line, as build.kpp's define() gives them.
    std::string list;
    if (!overridesDir.empty())
      if (FILE* f = fopen((overridesDir / "defines.txt").string().c_str(), "rb"))
      {
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
          list.append(buffer, n);
        fclose(f);
      }
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
      size_t equals = line.find('=');
      impl->cDefines[line.substr(0, equals)] = equals == std::string::npos ? std::string() : line.substr(equals + 1);
      if (equals != std::string::npos && equals + 1 < line.size() && (isalpha((unsigned char)line[equals + 1]) || line[equals + 1] == '_') &&
          std::all_of(line.begin() + equals + 1, line.end(), [](char c) { return isIdentChar(c); }))
        impl->cNameDefines[line.substr(0, equals)] = line.substr(equals + 1);
      if (equals != std::string::npos && equals + 1 == line.size())
        impl->cEmptyDefines.insert(line.substr(0, equals));
    }
    // Names the library keeps to its own files that another part of the program declares too, one a
    // line: stb_truetype.h's fallback rect packer (stbrp_context, stbrp_init_target, ... without
    // STB_RECT_PACK_VERSION), private to fontstash.cpp's translation unit, beside the game's
    // stb_rect_pack, which kpp's C import makes one program's names.
    if (!overridesDir.empty())
      if (FILE* f = fopen((overridesDir / "private-names.txt").string().c_str(), "rb"))
      {
        std::string names;
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
          names.append(buffer, n);
        fclose(f);
        for (size_t pos = 0; pos < names.size();)
        {
          size_t end = names.find('\n', pos);
          if (end == std::string::npos)
            end = names.size();
          std::string line = names.substr(pos, end - pos);
          pos = end + 1;
          size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
          if (a != std::string::npos && line[a] != '#')
            impl->cPrivateNames.insert(line.substr(a, b - a + 1));
        }
      }
    // Headers whose #if lines test a macro of a header they do not include, which each of their
    // includers includes before them, `HEADER PROVIDER` a line by file name: minizip's crypt.h tests
    // zlib.h's ZLIB_VERNUM (`#if ZLIB_VERNUM < 0x1270`, its own z_crc_t), and crypt.c, unzip.c and
    // zip.c include zlib.h first. The header is configured with what the provider leaves defined.
    if (!overridesDir.empty())
      if (FILE* f = fopen((overridesDir / "includer-headers.txt").string().c_str(), "rb"))
      {
        std::string lines;
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
          lines.append(buffer, n);
        fclose(f);
        for (size_t pos = 0; pos < lines.size();)
        {
          size_t end = lines.find('\n', pos);
          if (end == std::string::npos)
            end = lines.size();
          std::string line = lines.substr(pos, end - pos);
          pos = end + 1;
          size_t a = line.find_first_not_of(" \t\r");
          if (a == std::string::npos || line[a] == '#')
            continue;
          size_t space = line.find_first_of(" \t", a), b = space == std::string::npos ? space : line.find_first_not_of(" \t", space);
          size_t c = b == std::string::npos ? b : line.find_last_not_of(" \t\r");
          if (b != std::string::npos)
            impl->cIncluderHeaders[line.substr(a, space - a)] = line.substr(b, c - b + 1);
        }
      }
  }
  // The #if names nothing defines that the library's C build leaves undefined too, one a line,
  // checked against clang-cl's -dM of each of its source files with the library's flags: any other
  // is an error (convertToKpp), as the converter does not read the C runtime's and Windows' headers
  // (trionan.c's FLT_RADIX, DBL_MAX_EXP and DBL_MANT_DIG were taken as undefined: a negative zero
  // printed 0).
  if (cSources && !overridesDir.empty())
    if (FILE* f = fopen((overridesDir / "undefined.txt").string().c_str(), "rb"))
    {
      std::string names;
      char buffer[4096];
      size_t n;
      while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
        names.append(buffer, n);
      fclose(f);
      for (size_t pos = 0; pos < names.size();)
      {
        size_t end = names.find('\n', pos);
        if (end == std::string::npos)
          end = names.size();
        std::string line = names.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a != std::string::npos && line[a] != '#')
          impl->cKnownUndefined.insert(line.substr(a, b - a + 1));
      }
    }
  impl->paths = paths;
  impl->texts = texts;
  impl->roots = roots;
  impl->results.resize(paths.size());
  impl->state.assign(paths.size(), 0);
  for (size_t i = 0; i < paths.size(); ++i)
  {
    impl->byPath[pathKeyOf(paths[i])] = i;
    std::string_view t = texts[i];
    for (size_t at = t.find("define"); at != std::string_view::npos; at = t.find("define", at + 6))
    {
      size_t hash = at;
      while (hash > 0 && (t[hash - 1] == ' ' || t[hash - 1] == '\t'))
        --hash;
      if (hash == 0 || t[hash - 1] != '#')
        continue;
      size_t n = at + 6, e;
      while (n < t.size() && (t[n] == ' ' || t[n] == '\t'))
        ++n;
      for (e = n; e < t.size() && isIdentChar(t[e]); ++e)
      {
      }
      if (e > n && n > at + 6)
        impl->defines.insert(std::string(t.substr(n, e - n)));
    }
  }
  for (size_t i = 0; i < paths.size(); ++i)
    impl->of(i);
  if (cSources)
  {
    std::vector<const std::string*> configured;
    for (auto& result : impl->results)
      if (result)
        configured.push_back(&result->configured);
    // (A library define that is empty goes at its uses, as one its sources define empty: trio's
    // `TRIO_PUBLIC int trio_printf`, whose own definitions the define leaves out.)
    impl->cMacros = collectCMacros(configured, impl->cEmptyDefines);
    // C's restrict qualifies a pointer for the optimizer only (xxhash.c's `XXH32_state_t* restrict
    // dstState`): nothing, as kpp has no restrict
    for (const char* restrictWord : {"restrict", "__restrict", "__restrict__"})
      impl->cMacros.empty.insert(restrictWord);
  }
  for (auto& result : impl->results)
    if (result)
      blankForParse(*result, cSources ? &impl->cMacros : nullptr);
}

const std::map<std::string, std::string>& SourceConfiguration::cKeywordMacros() const { return impl->cMacros.keyword; }

SourceConfiguration::~SourceConfiguration() = default;

const std::string& SourceConfiguration::configuredText(size_t i) const { return impl->results[i]->configured; }

