// FileConverterPreprocessorLines.inl: FileConverter's members: preprocessor lines: #define to kpp, #if marks.
// Part of convert.cpp, included in struct FileConverter's body in Converter/FileConverter.hpp (a file of its own since 2026-11-05, moved without a change).

    std::string macroToKpp(const std::string& line, uint32_t offset)
    {
      std::string out = macroToKppText(line, offset);
      if (!isScopeHeader() || !atFileScope(offset))
        return out;
      size_t p = line.find("define") + 6;
      while (p < line.size() && isspace((unsigned char)line[p]))
        ++p;
      size_t nameStart = p;
      while (p < line.size() && isIdentChar(line[p]))
        ++p;
      std::string name = line.substr(nameStart, p - nameStart);
      bool functionLike = p < line.size() && line[p] == '(';
      std::string body = collapsedSpaces(continuationsAsSpaces(line.substr(p)));
      for (size_t c = body.find("/*"); c != std::string::npos; c = body.find("/*", c))
      {
        size_t e = body.find("*/", c + 2);
        body.replace(c, (e == std::string::npos ? body.size() : e + 2) - c, " ");
      }
      if (size_t c = body.find("//"); c != std::string::npos)
        body = body.substr(0, c);
      body = collapsedSpaces(body);
      std::vector<std::string> params;
      if (functionLike)
      {
        size_t close = body.find(')');
        std::string list = close == std::string::npos ? std::string() : body.substr(1, close - 1);
        for (size_t start = 0; start <= list.size();)
        {
          size_t comma = list.find(',', start);
          std::string param = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
          size_t a = param.find_first_not_of(" \t"), b = param.find_last_not_of(" \t");
          if (a != std::string::npos)
            params.push_back(param.substr(a, b - a + 1));
          if (comma == std::string::npos)
            break;
          start = comma + 1;
        }
        body = close == std::string::npos ? std::string() : body.substr(close + 1);
      }
      size_t a = body.find_first_not_of(" \t\r\n"), b = body.find_last_not_of(" \t\r\n");
      body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
      if (!functionLike && cOwnConstant(name))
        return out;
      if (!functionLike && cStandardName(name))
      {
        counts.add("C standard names an interface header defines where C's headers did not (left to them)");
        return out;
      }
      std::string indent = line.substr(0, line.find('#'));
      // A string (zlib.h's `#define ZLIB_VERSION "1.3.2"`): a constant array of its characters, as the
      // literal is (sizeof is the literal's). The library's own files keep it written out (a literal
      // may be concatenated with another there).
      if (!functionLike && !body.empty() && body[0] == '"' && isStringLiterals(body))
      {
        counts.add("C string macros of the interface written constants of the library's scope");
        cScopeStrings.insert(name);
        return indent + "const char " + name + "[] = " + body + ";";
      }
      // A type (zconf.h's `#define z_off_t long long`): an alias of it.
      if (!functionLike && isTypeWords(body))
      {
        counts.add("C type macros of the interface written aliases of the library's scope");
        std::string type = body;
        for (size_t k = type.find("__int64"); k != std::string::npos; k = type.find("__int64", k))
          type.replace(k, 7, "long long");
        return indent + "using " + name + " = " + type + ";";
      }
      // A parameter list's wrapper (zconf.h's `#define OF(args) args`, for K&R C's prototypes): its body
      // is its parameter, no value.
      if (functionLike && params.size() == 1 && body == params[0])
      {
        counts.add("C macros of the interface that are no value (guards, linkage words)");
        return out;
      }
      // One beside the function of its name (zlib.h's gzgetc, an inline fast path of the function
      // gzgetc): the function stands, as `(gzgetc)(g)` reaches it in C.
      if (functionLike && cScopeDeclared().count(name))
      {
        counts.add("C function-like macros of the interface beside its function of one name (the function stands)");
        return out;
      }
      if (functionLike)
        if (std::string function = cScopeFunction(name, params, body); !function.empty())
        {
          // (its uses in the library stay expanded; this is its users')
          counts.add("C function-like macros of the interface written functions of the library's scope");
          return out.empty() ? indent + function : out + "\n" + indent + function;
        }
      static const std::set<std::string> linkageWords = {"__declspec", "dllexport", "dllimport", "__attribute__", "visibility", "default", "extern",
                                                         "static", "inline", "__inline", "__forceinline"};
      bool noValue = body.empty();
      if (!noValue && !functionLike)
      {
        // (a linkage word, or another name of one: ZSTDLIB_API is ZSTDLIB_VISIBILITY, empty here)
        noValue = true;
        for (size_t k = 0; k < body.size() && noValue;)
          if (isalpha((unsigned char)body[k]) || body[k] == '_')
          {
            size_t e = k;
            while (e < body.size() && isIdentChar(body[e]))
              ++e;
            std::string word = body.substr(k, e - k);
            auto object = objectMacros.find(word);
            if (!linkageWords.count(word) && !(object != objectMacros.end() && collapsedSpaces(object->second).find_first_not_of(" \t") == std::string::npos) &&
                word.find("VISIBILITY") == std::string::npos)
              noValue = false;
            k = e;
          }
          else if (strchr(" \t()\"", body[k]))
            ++k;
          else
            noValue = false;
      }
      if (noValue)
      {
        counts.add("C macros of the interface that are no value (guards, linkage words)");
        return out;
      }
      counts.add("C macros of the interface not expressed in kpp");
      todo(offset, "macro of the library's interface not expressed in kpp, its users have no " + name + ": #define " + name +
                       (functionLike ? "(...) " : " ") + body);
      return out;
    }

    // A name of C's standard headers that an interface header defines where they did not (zconf.h's
    // `#ifndef SEEK_SET`): the C headers' (stdio's), never the library's, so not its constant.
    static bool cStandardName(const std::string& name)
    {
      static const std::set<std::string> names = {"SEEK_SET", "SEEK_CUR", "SEEK_END", "NULL", "EOF", "TRUE", "FALSE", "BUFSIZ", "FILENAME_MAX"};
      return names.count(name) != 0;
    }
    // Whether a macro's body is a type of C's words alone (`long long`, `unsigned`, `__int64`).
    static bool isTypeWords(const std::string& body)
    {
      static const std::set<std::string> words = {"unsigned", "signed", "long", "short", "int", "char", "float", "double", "void", "__int64"};
      bool any = false;
      for (size_t i = 0; i < body.size();)
        if (isalpha((unsigned char)body[i]) || body[i] == '_')
        {
          size_t e = i;
          while (e < body.size() && isIdentChar(body[e]))
            ++e;
          if (!words.count(body.substr(i, e - i)))
            return false;
          any = true;
          i = e;
        }
        else if (body[i] == ' ' || body[i] == '\t')
          ++i;
        else
          return false;
      return any;
    }
    // An interface header's own names (its functions, types and enumerators: zlib.h's deflateInit_,
    // z_stream), which its function-like macros may name; and its string constants (ZLIB_VERSION).
    std::set<std::string> cScopeDeclaredNames;
    bool cScopeDeclaredRead = false;
    std::set<std::string> cScopeStrings;
    const std::set<std::string>& cScopeDeclared()
    {
      if (!cScopeDeclaredRead && in.parsed)
      {
        cScopeDeclaredRead = true;
        auto add = [&](const QualifiedName& n)
        {
          if (n.parts.size() == 1 && n.last().symbol != UINT32_MAX)
            cScopeDeclaredNames.insert(std::string(symbols.name(n.last().symbol)));
        };
        for (Decl* d : in.parsed->decls)
          if (d->kind == NodeKind::FunctionDecl)
            add(static_cast<FunctionDecl*>(d)->name);
          else if (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
            add(static_cast<VariableDecl*>(d)->name);
          else if (d->kind == NodeKind::ClassDecl)
            add(static_cast<ClassDecl*>(d)->name);
          else if (d->kind == NodeKind::UsingAliasDecl)
            cScopeDeclaredNames.insert(std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name)));
          else if (d->kind == NodeKind::EnumDecl)
          {
            add(static_cast<EnumDecl*>(d)->name);
            for (Decl* e : static_cast<EnumDecl*>(d)->enumerators)
              cScopeDeclaredNames.insert(std::string(symbols.name(static_cast<EnumeratorDecl*>(e)->name)));
          }
      }
      return cScopeDeclaredNames;
    }

    // An interface header's function-like macro as a function of its scope: each parameter a value of
    // its own type (a template parameter), the body an expression of the parameters, numbers, the
    // interface's constants, functions and types (zlib.h's `deflateInit_((strm), (level),
    // ZLIB_VERSION, (int)sizeof(z_stream))`) and operators, which the function returns: the macro's
    // arithmetic on the arguments' types (C's conversions are C++'s); constexpr when it calls none of
    // the interface's functions. None (""): a body of anything else (a stringizing `#str`, a
    // statement, a name it would capture at its use).
    std::string cScopeFunction(const std::string& name, const std::vector<std::string>& params, const std::string& body)
    {
      if (params.empty() || body.empty())
        return std::string();
      static const std::set<std::string> typeWords = {"sizeof", "unsigned", "signed", "int", "long", "short", "char", "float", "double", "size_t"};
      bool calls = false;
      for (size_t i = 0; i < body.size();)
      {
        char c = body[i];
        if (isdigit((unsigned char)c))
        {
          while (i < body.size() && (isIdentChar(body[i]) || body[i] == '.'))
            ++i;
          continue;
        }
        if (isalpha((unsigned char)c) || c == '_')
        {
          size_t j = i;
          while (j < body.size() && isIdentChar(body[j]))
            ++j;
          std::string word = body.substr(i, j - i);
          i = j;
          if (typeWords.count(word) || std::find(params.begin(), params.end(), word) != params.end() || cOwnConstant(word) || cScopeStrings.count(word))
            continue;
          // (a member, after `.` or `->`: ioapi.h's `(filefunc).zfile_func64.zread_file`)
          size_t start = i - word.size();
          size_t before = start == 0 ? std::string::npos : body.find_last_not_of(" \t", start - 1);
          if (before != std::string::npos && (body[before] == '.' || (body[before] == '>' && before > 0 && body[before - 1] == '-')))
            continue;
          if (!cScopeDeclared().count(word))
            return std::string();
          size_t next = body.find_first_not_of(" \t", i);
          calls = calls || (next != std::string::npos && body[next] == '(');
          continue;
        }
        if (!strchr(" \t()+-*/%<>|&~!^?:=,.", c))
          return std::string();
        ++i;
      }
      std::string function = "template<";
      for (size_t k = 0; k < params.size(); ++k)
        function += std::string(k ? ", " : "") + "typename T" + std::to_string(k + 1);
      function += std::string(calls ? "> auto " : "> constexpr auto ") + name + "(";
      // (a parameter whose address the body takes, ioapi.h's `call_zopen64((&(filefunc)),...)`, refers to
      // the argument itself, as the macro's does: a forwarding reference, not a copy)
      auto addressTaken = [&](const std::string& param)
      {
        for (size_t at = body.find('&'); at != std::string::npos; at = body.find('&', at + 1))
        {
          size_t before = at == 0 ? std::string::npos : body.find_last_not_of(" \t", at - 1);
          if (before != std::string::npos && !strchr("(,=?:", body[before]))
            continue; // (a binary `&`)
          size_t k = body.find_first_not_of(" \t(", at + 1);
          if (k != std::string::npos && body.compare(k, param.size(), param) == 0 &&
              (k + param.size() == body.size() || !isIdentChar(body[k + param.size()])))
            return true;
        }
        return false;
      };
      for (size_t k = 0; k < params.size(); ++k)
        function += std::string(k ? ", " : "") + "T" + std::to_string(k + 1) + (addressTaken(params[k]) ? "&& " : " ") + params[k];
      return function + ") { return " + body + "; }";
    }

    // Turns `#define NAME body` into a constant where the body is one, else a TODO comment.
    std::string macroToKppText(const std::string& line, uint32_t offset)
    {
      size_t p = line.find("define") + 6;
      while (p < line.size() && isspace((unsigned char)line[p]))
        ++p;
      size_t nameStart = p;
      while (p < line.size() && isIdentChar(line[p]))
        ++p;
      std::string name = line.substr(nameStart, p - nameStart);
      bool functionLike = p < line.size() && line[p] == '(';
      std::string body = line.substr(p);
      size_t comment = body.find("//");
      std::string trailing;
      if (comment != std::string::npos)
      {
        trailing = " " + body.substr(comment);
        body = body.substr(0, comment);
      }
      while (!body.empty() && isspace((unsigned char)body.back()))
        body.pop_back();
      while (!body.empty() && isspace((unsigned char)body.front()))
        body.erase(body.begin());
      if (knownMacros.count(name) || knownFunctionMacros.count(name) || argumentMacros.count(name))
      {
        counts.add("macro known");
        return ""; // its uses are rewritten
      }
      if (functionLike)
      {
        // A function-like macro of this file (HANDLE_CASE in Listener.cpp): its uses in this file
        // are expanded in place by convertTokens.
        size_t close = body.find(')');
        if (close != std::string::npos)
        {
          LocalMacro m;
          std::string paramText = body.substr(1, close - 1);
          // A parameter list continued over lines (CALCULATE_VERTICES): the line ends are spaces.
          for (char& ch : paramText)
            if (ch == '\\' || ch == '\r' || ch == '\n')
              ch = ' ';
          size_t start = 0;
          while (start <= paramText.size())
          {
            size_t comma = paramText.find(',', start);
            std::string param = paramText.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            while (!param.empty() && isspace((unsigned char)param.front()))
              param.erase(param.begin());
            while (!param.empty() && isspace((unsigned char)param.back()))
              param.pop_back();
            if (!param.empty())
              m.params.push_back(param);
            if (comma == std::string::npos)
              break;
            start = comma + 1;
          }
          m.body = collapsedSpaces(continuationsAsSpaces(body.substr(close + 1)));
          while (!m.body.empty() && isspace((unsigned char)m.body.front()))
            m.body.erase(m.body.begin());
          while (!m.body.empty() && isspace((unsigned char)m.body.back()))
            m.body.pop_back();
          if (std::optional<std::string> failure = kppReleaseAssert(name, m.params, m.body))
          {
            counts.add("release_assert written as kpp's statement");
            return line.substr(0, line.find('#')) +
                   "// release_assert is kpp's own statement, which takes its file, line and expression text where\n"
                   "// it is written. A failing one, and a failing assert, comes here, to " + *failure + "\n"
                   "// as the macro called it.\n"
                   "extern \"C\" void kpp_assert_failed(const char* file, i32 line, i32 column, const char* expression) { " + *failure +
                   "(file, line, expression); }" + trailing;
          }
          // (one that is nothing, too: GameRenderer.cpp's CHECK_AND_ADVANCE_STATE in Release; its uses
          // go)
          endLocalMacro(name, offset); // a redefinition without #undef replaces the earlier one
          m.from = offset;
          localMacros[name].push_back(m);
          counts.add(m.body.empty() ? "local function-like macros that are nothing" : "local function-like macro");
          return "";
        }
      }
      if (!functionLike)
        if (const std::string* value = cOwnConstant(name))
        {
          if (isScopeHeader())
          {
            // (the scope's: its users write zstd::ZSTD_CONTENTSIZE_UNKNOWN, its files the name)
            counts.add("C macro constants of the interface written constants of the library's scope");
            (*cScopeConstants)[name] = *value;
            // (C's null pointer by another name, zlib.h's `#define Z_NULL 0`: nullptr, as its uses are
            // written, so a user's `strm.zalloc = zlib::Z_NULL` is a pointer's)
            bool nullPointer = name.size() >= 4 && name.compare(name.size() - 4, 4, "NULL") == 0 && *value == "0";
            return line.substr(0, line.find('#')) + "const auto " + name + " = " + (nullPointer ? std::string("nullptr") : *value) + ";" + trailing;
          }
          counts.add("C macro constants of a .c file written private constants");
          return line.substr(0, line.find('#')) + "private const auto " + name + " = " + *value + ";" + trailing;
        }
      if (!functionLike && body == name)
      {
        // Crypto.cpp's `#define fileno fileno` (so that curl's `#ifndef fileno` declares nothing):
        // a use is the name itself, as the preprocessor leaves it.
        counts.add("macros naming themselves");
        return "// macro naming itself: " + line;
      }
      if (const LocalMacro* aliased = functionLike || body == name ? nullptr : localMacroAt(body, offset))
      {
        // Another name for a function-like macro (lobject.h's `#define setobj2s setobj`): that macro
        // under this name, expanded at its uses as the preprocessor expands it (a constant of it
        // would name no value).
        LocalMacro m = *aliased;
        endLocalMacro(name, offset);
        m.from = offset;
        m.to = UINT32_MAX;
        localMacros[name].push_back(m);
        counts.add("local function-like macro");
        return "";
      }
      // Made of string macros (lua.h's LUA_RELEASE): their literals; another name for a macro
      // written out at its uses (luaconf.h's `#define LUALIB_API LUA_API`, LUA_API being `extern
      // "C"`): its text.
      if (!functionLike && !isStringLiterals(body) && objectMacros.count(name) &&
          (isStringLiterals(objectMacros.at(name)) || isSplicedMacroBody(objectMacros.at(name))))
        body = objectMacros.at(name);
      if (!functionLike && (isSplicedMacroBody(body) || (isAliasMacro(name, body) && !localMacroAt(body, offset)) ||
                            (functionMacroNames && callsFunction(body, *functionMacroNames))))
      {
        // Written out at its uses (convertTokens, objectMacros): declarations, a type, storage words,
        // another name.
        counts.add("spliced macros");
        // (its name alone: the text, the body, may be another on another platform, FileOperations'
        // `#define ftello64 _ftelli64` or `ftello`, where the converted files are to be the same;
        // the lines a continued definition took stay, blank)
        std::string commented = "// macro written out at its uses: " + name;
        for (char c : line)
          if (c == '\n')
            commented += '\n';
        return commented;
      }
      if (!functionLike && !body.empty())
      {
        // Continued over lines (lgc.c's `#define sweepphases \` with its value on the next line):
        // the lines without their backslashes.
        for (size_t k = body.find('\\'); k != std::string::npos; k = body.find('\\', k))
        {
          size_t e = k + 1;
          while (e < body.size() && (body[e] == ' ' || body[e] == '\t' || body[e] == '\r'))
            ++e;
          if (e < body.size() && body[e] == '\n')
            body.erase(k, 1);
          else
            k += 2; // an escape in a literal (`'\\'`)
        }
        bool constant = true;
        int quotes = 0, depth = 0;
        // Checked without its comments, with tabs and line ends as spaces (lstate.h's `#define
        // CIST_LUA\t(1<<0)\t/* ... */`) and with what a character literal holds as `x`
        // (lstrlib.c's `#define L_ESC '%'`).
        std::string checked = body;
        for (size_t c = checked.find("/*"); c != std::string::npos; c = checked.find("/*", c))
        {
          size_t e = checked.find("*/", c + 2);
          checked.replace(c, (e == std::string::npos ? checked.size() : e + 2) - c, " ");
        }
        for (char& c : checked)
          if (c == '\t' || c == '\r' || c == '\n')
            c = ' ';
        for (size_t c = 0; c < checked.size(); ++c)
          if (checked[c] == '"')
          {
            for (++c; c < checked.size() && checked[c] != '"'; ++c)
              if (checked[c] == '\\')
                ++c;
          }
          else if (checked[c] == '\'' && !(c > 0 && isalnum((unsigned char)checked[c - 1]))) // not a digit separator
          {
            for (++c; c < checked.size() && checked[c] != '\''; ++c)
            {
              if (checked[c] == '\\' && c + 1 < checked.size())
                checked[c++] = 'x';
              checked[c] = 'x';
            }
          }
        for (char c : checked)
        {
          if (c == '"')
            ++quotes;
          else if (!(isIdentChar(c) || strchr(" .+-*/()<>|&~!^,'\\xX", c)))
            constant = false;
          else if (quotes % 2 == 0)
          {
            depth += c == '(' ? 1 : c == ')' ? -1 : 0;
            if (c == ',' && depth == 0)
              constant = false; // a list (`2,1,0` for a resource script), not a value
          }
        }
        // A body of declaration words (`#define STBDDEF static`) is no value either.
        static const std::set<std::string> declarationWords = {"static", "inline", "extern", "const", "volatile", "register", "thread_local",
                                                              "constexpr", "virtual", "explicit", "friend", "typedef", "struct", "class",
                                                              "union", "enum", "template", "typename", "using", "namespace", "__forceinline",
                                                              "__inline", "__declspec", "__cdecl", "__stdcall", "__fastcall"};
        for (size_t k = 0; k < body.size() && constant;)
        {
          if (body[k] == '"')
          {
            size_t e = body.find('"', k + 1);
            k = e == std::string::npos ? body.size() : e + 1;
          }
          else if (isalpha((unsigned char)body[k]) || body[k] == '_')
          {
            size_t e = k;
            while (e < body.size() && isIdentChar(body[e]))
              ++e;
            if (declarationWords.count(body.substr(k, e - k)))
              constant = false;
            k = e;
          }
          else if (isdigit((unsigned char)body[k]))
          {
            while (k < body.size() && isIdentChar(body[k]))
              ++k;
          }
          else
            ++k;
        }
        if (constant && (quotes == 0 || quotes == 2) && cWrittenOutMacros && (cWrittenOutMacros->count("*") || cWrittenOutMacros->count(name)))
        {
          counts.add("C macro constants written out at their uses");
          return std::string();
        }
        if (constant && (quotes == 0 || quotes == 2))
        {
          // Its function-like macros expanded, as at a use (lgc.h's `#define WHITEBITS
          // bit2mask(WHITE0BIT, WHITE1BIT)`).
          if (!localMacros.empty())
            body = rescanMacros(body, offset);
          counts.add("macro to constant");
          std::string indent = line.substr(0, line.find('#'));
          // In a class's definition (StackWalker's `#define MAX_MODULE_NAME32 255` among its
          // members) the constant is the class's, not a data member of each object.
          const ScopeRange* innermost = nullptr;
          for (const ScopeRange& r : scopeRanges)
            if (r.begin <= offset && offset < r.end && (!innermost || r.begin >= innermost->begin))
              innermost = &r;
          if (in.c && innermost && innermost->classBody)
          {
            // C's macros are global (deflate.h's `#define LIT_BUFS 4` among internal_state's
            // members, used in deflate.c): the constant goes before the struct.
            const ScopeRange* outermost = innermost;
            for (const ScopeRange& r : scopeRanges)
              if (r.begin <= offset && offset < r.end && r.begin < outermost->begin)
                outermost = &r;
            size_t lineStart = text.rfind('\n', outermost->begin);
            lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
            cInsertBefore(uint32_t(lineStart), "const auto " + name + " = " + body + ";\n");
            counts.add("C macros defined in a struct written before it");
            return std::string();
          }
          return indent + (innermost && innermost->classBody ? "static constexpr auto " : "const auto ") + name + " = " + body + ";" + trailing;
        }
      }
      todo(offset, "macro " + name + (functionLike ? "(...)" : "") + " kept as a comment");
      counts.add("macro todo");
      std::string commented = "// TODO(kpp) macro: ";
      for (char c : line)
      {
        commented += c;
        if (c == '\n')
          commented += "// ";
      }
      return commented;
    }

    void convertPreprocessor()
    {
      // Logical lines with continuations joined.
      std::vector<std::pair<uint32_t, uint32_t>> lines;
      size_t pos = 0;
      while (pos < text.size())
      {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
          end = text.size();
        size_t start = pos;
        while (end < text.size())
        {
          size_t last = end;
          while (last > start && (text[last - 1] == '\r' || text[last - 1] == ' '))
            --last;
          if (last > start && text[last - 1] == '\\')
          {
            end = text.find('\n', end + 1);
            if (end == std::string::npos)
              end = text.size();
            continue;
          }
          break;
        }
        // A directive whose block comment goes on past its line (lstate.h's `#define CIST_REENTRY
        // (1<<2) /* call is running on same invocation of`, closed on the next line) ends where the
        // comment does, as the preprocessor reads it.
        size_t first = text.find_first_not_of(" \t", start);
        if (first < end && text[first] == '#')
          while (end < text.size() && opensBlockComment(text.substr(start, end - start)))
          {
            size_t close = text.find("*/", end);
            end = close == std::string_view::npos ? text.size() : text.find('\n', close + 2);
            if (end == std::string_view::npos)
              end = text.size();
          }
        lines.push_back({uint32_t(start), uint32_t(end - start)});
        pos = end + 1;
      }
      // The configuration decided which lines stay (preprocessText, shared with the front end).
      Preprocessed own;
      if (!preprocessed)
        own = preprocessText(text, nullptr);
      const Preprocessed& decided = preprocessed ? *preprocessed : own;
      size_t nextDrop = 0, nextNote = 0;
      bool inComment = false; // as in preprocessText: a line starting inside a block comment is no directive
      for (auto [offset, length] : lines)
      {
        while (nextDrop < decided.dropped.size() && decided.dropped[nextDrop].second <= offset)
          ++nextDrop;
        bool active = !(nextDrop < decided.dropped.size() && decided.dropped[nextDrop].first <= offset);
        while (nextNote < decided.notes.size() && decided.notes[nextNote].first < offset)
          ++nextNote;
        for (; nextNote < decided.notes.size() && decided.notes[nextNote].first == offset; ++nextNote)
          todo(offset, decided.notes[nextNote].second);
        std::string line(text.substr(offset, length));
        bool commentAtStart = inComment;
        inComment = blockCommentOpenAfter(line, inComment);
        size_t p = offset == 0 ? byteOrderMarkLength(text) : 0;
        while (p < line.size() && isspace((unsigned char)line[p]))
          ++p;
        bool directive = !commentAtStart && p < line.size() && line[p] == '#';
        uint32_t removeLength = length + (offset + length < text.size() ? 1 : 0); // with its newline
        if (!directive)
        {
          if (!active)
          {
            remove(offset, removeLength);
            counts.add("lines dropped by #if");
          }
          continue;
        }
        ++p;
        while (p < line.size() && isspace((unsigned char)line[p]))
          ++p;
        size_t ws = p;
        while (p < line.size() && isalpha((unsigned char)line[p]))
          ++p;
        std::string word = line.substr(ws, p - ws);
        std::string rest = line.substr(p);
        size_t c = rest.find("//");
        if (c != std::string::npos)
          rest = rest.substr(0, c);
        for (char& ch : rest)
          if (ch == '\\' || ch == '\r')
            ch = ' ';
        if (word == "if" || word == "ifdef" || word == "ifndef")
        {
          counts.add("#if");
          // (written where the text around it is: a drop its own condition starts begins at it)
          bool around = active || (nextDrop < decided.dropped.size() && decided.dropped[nextDrop].first == offset);
          openIfGroups.push_back({nextIfGroup++, around});
          if (convertMarks && around)
          {
            replace(offset, length, "/*kpp:if " + in.path.filename().string() + ":" + std::to_string(openIfGroups.back().first) + "*/");
            continue;
          }
        }
        else if (word == "endif" && !openIfGroups.empty())
        {
          auto [group, written] = openIfGroups.back();
          openIfGroups.pop_back();
          if (convertMarks && written)
          {
            replace(offset, length, "/*kpp:endif " + in.path.filename().string() + ":" + std::to_string(group) + "*/");
            continue;
          }
        }
        else if (word == "elif" || word == "else" || word == "endif")
        {
        }
        else if (!active)
        {
          // a directive inside a dropped branch
        }
        else if (word == "include")
        {
          // C headers become imports; C++ and project headers go (every project symbol is visible).
          static const std::set<std::string> cHeaders = {"windows.h", "stdio.h", "stdlib.h", "string.h", "math.h", "ctype.h", "errno.h",
                                                         "stdarg.h", "stdint.h", "stddef.h", "time.h", "io.h", "fcntl.h", "direct.h",
                                                         "process.h", "signal.h", "locale.h", "wchar.h", "float.h", "limits.h", "assert.h",
                                                         "conio.h", "intrin.h", "malloc.h", "memory.h", "wctype.h", "inttypes.h",
                                                         "shellapi.h", "shlobj.h", "winsock2.h", "ws2tcpip.h", "psapi.h", "dbghelp.h",
                                                         "tlhelp32.h", "shlwapi.h", "timeapi.h", "versionhelpers.h", "iphlpapi.h", "windns.h"};
          size_t open = rest.find_first_of("<\"");
          size_t close = rest.find_first_of(">\"", open + 1);
          std::string header = open != std::string::npos && close != std::string::npos ? rest.substr(open + 1, close - open - 1) : "";
          std::string base = header.substr(header.find_last_of('/') + 1);
          static const std::map<std::string, std::string> cxxToC = {{"cmath", "math.h"}, {"cstdio", "stdio.h"}, {"cstring", "string.h"},
                                                                    {"cstdlib", "stdlib.h"}, {"cctype", "ctype.h"}, {"cerrno", "errno.h"},
                                                                    {"cstdarg", "stdarg.h"}, {"ctime", "time.h"}, {"cwchar", "wchar.h"},
                                                                    {"cfloat", "float.h"}, {"climits", "limits.h"}, {"cstdint", "stdint.h"},
                                                                    {"cstddef", "stddef.h"}, {"cassert", "assert.h"}, {"csignal", "signal.h"},
                                                                    {"clocale", "locale.h"}, {"cwctype", "wctype.h"}, {"cinttypes", "inttypes.h"}};
          bool cInterface = cInterfaceHeaders && cInterfaceHeaders->count(base);
          if (siblingIncludes && siblingIncludes->count(header) && !cInterface)
          {
            // A converted file of this file's directory (Lua's own headers, by names a C library's
            // prefix would otherwise take for an import): its symbols are the program's.
            remove(offset, removeLength);
            counts.add("includes removed");
            continue;
          }
          // A scoped library's interface header (`zstd/zstd.h`, --convert-library): nothing to import, its
          // names are the library's, written qualified (convertTokens). Another header of its directory
          // (stb_image's stb_dxt.h, which is no interface of it) stays a C import.
          if (scopedLibraries && scopedHeaders && scopedHeaders->count(base))
          {
            // (the C headers the library's header gave its includer stay imported: its library's C
            // imports, scopedHeaderImports)
            if (scopedHeaderImports)
            {
              auto imports = scopedHeaderImports->find(base);
              if (imports != scopedHeaderImports->end())
                for (const std::string& imported : imports->second)
                  importC(imported);
            }
            remove(offset, removeLength);
            counts.add("includes of a scoped library's headers removed");
            continue;
          }
          auto cxx = cxxToC.find(base);
          if (cxx != cxxToC.end())
            base = cxx->second;
          // C libraries the game uses, by their include path prefix.
          // (stb_rect_pack, jc_voronoi and sdf are header-only C libraries whose implementations the
          // game compiles in its own .cpp files.)
          static const char* cLibraries[] = {"SDL3/", "SDL2/", "SDL", "zlib", "zconf", "curl/", "png", "jpeg", "zstd", "opus", "ogg",
                                             "vorbis", "minizip", "stb_image", "fontstash", "imgui/stb_rect_pack", "jc_voronoi/", "sdf/", "glad/",
                                             "CL/"};
          bool cLibrary = false;
          for (const char* prefix : cLibraries)
            if (header.rfind(prefix, 0) == 0)
              cLibrary = true;
          if (keptCLibraries)
            for (const std::string& prefix : *keptCLibraries)
              if (header.rfind(prefix, 0) == 0)
                cLibrary = true;
          static const std::set<std::string> directXHeaders = {"d3d11.h", "d3d11_1.h", "d3d11sdklayers.h", "dxgi.h", "dxgi1_2.h", "dxgi1_3.h",
                                                               "dxgi1_4.h", "dxgi1_5.h", "dxgi1_6.h", "d3dcommon.h", "d3d9.h", "d3dcompiler.h",
                                                               "wincodec.h", "objidl.h", "wbemidl.h", "wbemcli.h"};
          std::string lowered = base;
          for (char& ch : lowered)
            ch = (char)tolower((unsigned char)ch);
          if (directXHeaders.count(lowered))
          {
            // DirectX, DXGI, WIC, WMI and the COM headers: programs/factorio/native/include/kpp_directx.h,
            // which leaves out the C structs of the interfaces kpp declares as classes
            // (overrides/added/src/Graphics/DX11/ComInterfaces.kpp, generate_com.py).
            importC("kpp_directx.h");
            remove(offset, removeLength);
            counts.add("includes to imports");
            continue;
          }
          // (and the POSIX headers the game's Linux code includes, imported by their paths)
          static const std::set<std::string> sysHeaders = {"sys/stat.h", "sys/types.h", "sys/timeb.h", "sys/utime.h", "unistd.h", "poll.h",
                                                           "sys/poll.h", "termios.h", "arpa/inet.h", "arpa/nameser.h", "resolv.h",
                                                           "netinet/in.h", "netdb.h", "ifaddrs.h", "sys/socket.h", "sys/wait.h", "sys/file.h",
                                                           "sys/inotify.h", "sys/param.h", "sys/resource.h", "dlfcn.h", "execinfo.h", "pwd.h",
                                                           "setjmp.h", "cpuid.h", "pthread.h", "utime.h", "sys/mman.h", "sys/time.h",
                                                           "sys/utsname.h", "sys/sysinfo.h", "dirent.h", "glob.h", "langinfo.h"};
          bool sysHeader = sysHeaders.count(header) != 0; // (zlib's gzguts.h: _S_IREAD for _wopen)
          if (cHeaders.count(lowered) || cLibrary || cInterface || sysHeader) // Windows' headers by any case (<Psapi.h>, <TlHelp32.h>)
          {
            // A C library that the game configures with #defines before its include imports through a
            // header of programs/factorio/native/include that gives the import the same configuration
            // (jc_voronoi in double precision, as Triangulation.cpp has it).
            static const std::map<std::string, std::string> configuredImports = {{"jc_voronoi/jc_voronoi.h", "kpp_jc_voronoi.h"},
                                                                                 {"stb_image/stb_dxt.h", "kpp_stb_dxt.h"}};
            auto configured = configuredImports.find(header);
            std::string imported = configured != configuredImports.end() ? configured->second : cLibrary || cInterface || sysHeader ? header : lowered;
            if (cInterface && cConfiguredImports)
              if (auto c = cConfiguredImports->find(base); c != cConfiguredImports->end())
                imported = c->second;
            importC(imported);
            remove(offset, removeLength);
            counts.add("includes to imports");
            continue;
          }
          counts.add("includes removed");
        }
        else if (word == "pragma" || word == "undef" || word == "error" || word == "warning")
        {
          if (word == "pragma")
            notePack(rest, offset);
          if (word == "error")
            todo(offset, "#error" + rest + ": an #error this configuration reaches (a file for another platform or build?)");
          if (word == "undef")
          {
            size_t n = rest.find_first_not_of(" \t");
            size_t e = n == std::string::npos ? n : rest.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", n);
            if (n != std::string::npos)
            {
              std::string name = rest.substr(n, e == std::string::npos ? std::string::npos : e - n);
              endLocalMacro(name, offset);
              if (auto range = ownMacroRanges.find(name); range != ownMacroRanges.end() && range->second.second == UINT32_MAX)
                range->second.second = offset;
              else if (range == ownMacroRanges.end())
                ownMacroRanges[name] = {0, offset}; // an included header's, until here (zstd_common.c's `#undef ZSTD_isError`)
            }
          }
          counts.add("#" + word + " removed");
        }
        else if (word == "define")
        {
          {
            size_t n = rest.find_first_not_of(" \t");
            size_t e = n == std::string::npos ? n : rest.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", n);
            if (n != std::string::npos)
              ownMacroRanges[rest.substr(n, e == std::string::npos ? std::string::npos : e - n)] = {offset, UINT32_MAX};
          }
          std::string replacement = macroToKpp(line, offset);
          if (!replacement.empty())
          {
            replace(offset, length, replacement);
            continue;
          }
        }
        remove(offset, removeLength);
      }
    }

    // --- token-level rewrites ---

