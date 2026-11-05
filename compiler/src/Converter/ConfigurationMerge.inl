// ConfigurationMerge.inl: the configurations made one (--convert-merge).
// Part of convert.cpp, included where it stood (a file of its own since 2026-11-05, moved without a change).

void setConvertRelease()
{
  convertRelease = true;
}

void setConvertFinal()
{
  convertRelease = true;
  convertFinal = true;
}

void setConvertMarks()
{
  convertMarks = true;
}

bool setConvertPlatform(const std::string& platform)
{
  if (platform != "windows" && platform != "linux")
    return false;
  convertPlatform = platform;
  if (platform == "linux")
  {
    knownMacros["errno"] = "(*__errno_location())"; // (glibc's errno.h)
    knownMacros.erase("INVALID_SOCKET");             // (the game's own on Linux: SocketHppCompatibility.hpp's -1)
  }
  return true;
}

bool setConvertPredefines(const std::filesystem::path& file)
{
  std::string text;
  if (!readFile(file, text))
    return false;
  // (clang's `-dM -E`: `#define NAME BODY` lines)
  for (size_t start = 0; start < text.size();)
  {
    size_t e = text.find('\n', start);
    if (e == std::string::npos)
      e = text.size();
    std::string line = text.substr(start, e - start);
    start = e + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line.rfind("#define ", 0) != 0)
      continue;
    size_t nameEnd = line.find_first_of(" (", 8);
    std::string name = line.substr(8, nameEnd == std::string::npos ? std::string::npos : nameEnd - 8);
    if (nameEnd != std::string::npos && line[nameEnd] == '(')
      continue; // (a function-like one: none the #if lines test)
    convertPredefines[name] = nameEnd == std::string::npos ? "" : line.substr(nameEnd + 1);
  }
  return !convertPredefines.empty();
}

namespace
{
  // A converted text cut at its #if groups' marks (convertMarks): text parts, and groups of parts.
  struct MarkedPart
  {
    std::string text;              // a text part's text, without its marks' lines
    std::string group;             // a group's id (FILE:N); empty for a text part
    std::vector<MarkedPart> parts; // a group's contents
  };

  void addText(std::vector<MarkedPart>& parts, std::string_view text)
  {
    if (text.empty())
      return;
    if (!parts.empty() && parts.back().group.empty())
      parts.back().text += text;
    else
      parts.push_back({std::string(text), {}, {}});
  }

  // The parts from `at` to the end mark of `group`, or to the text's end for the whole text (group
  // ""). A mark alone on its line takes the line with it. A mark the conversion lost (an #if line
  // inside a macro's use, which the expansion replaced) is lost in both configurations alike: an end
  // mark without its start is left out, and one that closes an outer group ends the inner ones.
  bool parseMarked(const std::string& text, size_t& at, const std::string& group, std::vector<MarkedPart>& parts,
                   std::vector<std::string>& openGroups)
  {
    static const std::string open = "/*kpp:if ", close = "/*kpp:endif ";
    for (;;)
    {
      size_t o = text.find(open, at), c = text.find(close, at);
      size_t mark = std::min(o, c);
      if (mark == std::string::npos)
      {
        addText(parts, std::string_view(text).substr(at));
        at = text.size();
        return true;
      }
      bool opening = mark == o;
      size_t idStart = mark + (opening ? open.size() : close.size());
      size_t end = text.find("*/", idStart);
      if (end == std::string::npos)
        return false;
      std::string id = text.substr(idStart, end - idStart);
      end += 2;
      size_t from = mark;
      while (from > at && (text[from - 1] == ' ' || text[from - 1] == '\t'))
        --from;
      size_t to = end;
      while (to < text.size() && (text[to] == ' ' || text[to] == '\t' || text[to] == '\r'))
        ++to;
      if ((from == 0 || text[from - 1] == '\n') && (to == text.size() || text[to] == '\n'))
        to = std::min(to + 1, text.size());
      else
        from = mark, to = end;
      addText(parts, std::string_view(text).substr(at, from - at));
      if (!opening && id != group && std::find(openGroups.begin(), openGroups.end(), id) != openGroups.end())
      {
        at = from; // (the outer group's end: this one's was lost)
        return true;
      }
      at = to;
      if (!opening)
      {
        if (id == group)
          return true;
        continue; // (an end without its start)
      }
      MarkedPart g;
      g.group = id;
      openGroups.push_back(id);
      bool nested = parseMarked(text, at, id, g.parts, openGroups);
      openGroups.pop_back();
      if (!nested)
        return false;
      parts.push_back(std::move(g));
    }
  }

  std::string flattened(const std::vector<MarkedPart>& parts)
  {
    std::string out;
    for (const MarkedPart& p : parts)
      out += p.group.empty() ? p.text : flattened(p.parts);
    return out;
  }

  struct MergeReport
  {
    size_t conditional = 0;
    size_t byAsserts = 0; // groups whose debug text is the release one with asserts (differsByAsserts)
    size_t when = 0; // declarations of one configuration written `when (config::debug)` (design.md 19)
    size_t perConfiguration = 0; // declarations whose initializer alone differs, `= config::debug ? a : b`
    size_t final = 0; // groups whose final text alone differs, written with config::finalRelease
    size_t allThree = 0; // groups whose three texts all differ (problems)
    std::map<std::string, std::string> choices; // "FILE GROUP" -> the configuration whose text both get
    std::set<std::string> chosen;
    std::vector<std::string> problems;
  };

  // The configuration constant a group's two texts are chosen by: config::debug between FastDebug's
  // (on) and Release's text, or config::finalRelease between FinalRelease's (on) and Release's (a
  // final build is never a debug one: BuildConstants.hpp's #error).
  struct MergeDimension
  {
    const char* on;      // the condition of the first text
    const char* off;     // of the second
    const char* onName;  // the configurations' names in problems
    const char* offName;
  };
  const MergeDimension debugDimension = {"config::debug", "!config::debug", "debug", "release"};
  const MergeDimension finalDimension = {"config::finalRelease", "!config::finalRelease", "final", "release"};
  // The platform a group's two texts are chosen by (--convert-merge-platform): each platform named, so
  // that a third one (FreeBSD, macOS) has neither text and its build says so, never one of theirs.
  const MergeDimension platformDimension = {"config::platform == Platform::Windows", "config::platform == Platform::Linux", "windows", "linux"};

  // A group's merged text with its marks again (--convert-merge-platform keeps them for convertMerge,
  // which makes the configurations one after): inline, so that they take no line of their own.
  std::string marked(const std::string& group, const std::string& text) { return "/*kpp:if " + group + "*/" + text + "/*kpp:endif " + group + "*/"; }

  // The last character of code in a text, comments and white space left out (0 for none).
  char lastCodeChar(std::string_view text)
  {
    char last = 0;
    for (size_t i = 0; i < text.size();)
    {
      if (text.compare(i, 2, "//") == 0)
        i = std::min(text.find('\n', i), text.size());
      else if (text.compare(i, 2, "/*") == 0)
      {
        size_t e = text.find("*/", i + 2);
        i = e == std::string_view::npos ? text.size() : e + 2;
      }
      else if (text[i] == '"' || (text[i] == '\'' && !isDigitSeparator(text, i)))
      {
        char quote = text[i];
        for (++i; i < text.size() && text[i] != quote && text[i] != '\n'; ++i)
          i += text[i] == '\\';
        last = quote;
        ++i;
      }
      else
      {
        if (!isspace((unsigned char)text[i]))
          last = text[i];
        ++i;
      }
    }
    return last;
  }

  std::string firstLine(const std::string& text)
  {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      return "";
    size_t end = text.find_first_of("\r\n", first);
    std::string line = text.substr(first, end == std::string::npos ? std::string::npos : end - first);
    return line.size() > 90 ? line.substr(0, 90) + "..." : line;
  }

  bool blankText(const std::string& text) { return text.find_first_not_of(" \t\r\n") == std::string::npos; }

  // Whether a statement or declaration may start after a text: after `;`, `{`, `}`, the `:` of an
  // access specifier or a label, the `)` of a condition, `else` or `do`, or at the start.
  bool statementAfter(std::string_view before)
  {
    std::string_view tail = before.substr(before.size() > 4096 ? before.size() - 4096 : 0);
    char previous = lastCodeChar(tail);
    if (previous == 0 || previous == ';' || previous == '{' || previous == '}' || previous == ':')
      return true;
    // (a `)` closes a statement's condition only after if, while, for or switch: a call's, a chain
    // of cxxopts' `("w,wait", "...")`, is an expression's part)
    if (previous == ')')
    {
      size_t close = tail.find_last_of(')');
      int depth = 0;
      size_t open = close;
      for (; open != std::string_view::npos; --open)
      {
        depth += tail[open] == ')';
        depth -= tail[open] == '(';
        if (depth == 0 || open == 0)
          break;
      }
      if (depth != 0)
        return false;
      size_t end = tail.find_last_not_of(" \t\r\n", open == 0 ? std::string_view::npos : open - 1);
      if (end == std::string_view::npos || open == 0)
        return false;
      size_t start = end;
      while (start > 0 && isIdentChar(tail[start - 1]))
        --start;
      std::string_view word = tail.substr(start, end + 1 - start);
      return word == "if" || word == "while" || word == "for" || word == "switch" || word == "constexpr";
    }
    size_t end = tail.find_last_not_of(" \t\r\n");
    for (std::string_view word : {"else", "do"})
      if (end != std::string_view::npos && end + 1 >= word.size() && tail.substr(end + 1 - word.size(), word.size()) == word &&
          (end + 1 == word.size() || !isIdentChar(tail[end - word.size()])))
        return true;
    return false;
  }

  // A group's text that continues an if-else chain, links alone (`#ifndef FINAL_RELEASE else if (b)
  // {...} #endif`: `else if (c) A`, `else B`), with each link's condition joined with the
  // configuration's: `else if (!config::finalRelease && (c)) A`, `else if (!config::finalRelease) B`.
  // In the configuration without the text no link is taken, as no link was there. "" when the text
  // is no such links.
  std::string elseLinksWhen(const std::string& text, const char* condition)
  {
    auto skip = [&](size_t k)
    {
      while (k < text.size())
      {
        if (isspace((unsigned char)text[k]))
          ++k;
        else if (text.compare(k, 2, "//") == 0)
          k = std::min(text.find('\n', k), text.size());
        else if (text.compare(k, 2, "/*") == 0)
        {
          size_t e = text.find("*/", k + 2);
          k = e == std::string::npos ? text.size() : e + 2;
        }
        else
          break;
      }
      return k;
    };
    // (the position after the bracket that closes the one at k, string and character literals skipped)
    auto matching = [&](size_t k) -> size_t
    {
      int depth = 0;
      for (; k < text.size(); ++k)
      {
        char c = text[k];
        if (c == '"' || (c == '\'' && !isDigitSeparator(text, k)))
        {
          for (++k; k < text.size() && text[k] != c; ++k)
            k += text[k] == '\\';
          continue;
        }
        depth += c == '(' || c == '{';
        depth -= c == ')' || c == '}';
        if (depth == 0)
          return k + 1;
      }
      return std::string::npos;
    };
    // (the end of the statement at k: a block, or up to its `;` outside brackets)
    auto statementEnd = [&](size_t k) -> size_t
    {
      k = skip(k);
      if (k < text.size() && text[k] == '{')
        return matching(k);
      for (; k < text.size(); ++k)
      {
        if (text[k] == '(' || text[k] == '{')
        {
          k = matching(k);
          if (k == std::string::npos)
            return k;
          --k;
        }
        else if (text[k] == ';')
          return k + 1;
      }
      return std::string::npos;
    };
    std::string out;
    bool any = false;
    for (size_t i = 0;;)
    {
      size_t k = skip(i);
      out += text.substr(i, k - i);
      i = k;
      if (i >= text.size())
        break;
      if (text.compare(i, 4, "else") != 0 || (i + 4 < text.size() && isIdentChar(text[i + 4])))
        return "";
      size_t j = skip(i + 4);
      size_t end;
      if (text.compare(j, 2, "if") == 0 && (j + 2 >= text.size() || !isIdentChar(text[j + 2])))
      {
        size_t open = skip(j + 2);
        if (open >= text.size() || text[open] != '(')
          return "";
        size_t close = matching(open);
        if (close == std::string::npos)
          return "";
        out += text.substr(i, open + 1 - i) + condition + " && (" + text.substr(open + 1, close - 1 - (open + 1)) + "))";
        end = statementEnd(close);
        if (end == std::string::npos)
          return "";
        out += text.substr(close, end - close);
      }
      else
      {
        end = statementEnd(i + 4);
        if (end == std::string::npos)
          return "";
        out += "else if (" + std::string(condition) + ")" + text.substr(i + 4, end - (i + 4));
      }
      i = end;
      any = true;
    }
    return any ? out : "";
  }

  std::string_view codeStart(std::string_view item);
  bool definesType(std::string_view item);

  // The items of a group's text where declarations and statements stand: each a statement or a
  // declaration, ending with its `;` at depth 0 or with the `}` that closes a body at depth 0 (one
  // followed by `;`, `else`, `while`, `catch` or a name, `struct S {...} s;`, runs on). Comments and
  // blank lines before an item are its own.
  std::vector<std::string> groupItems(const std::string& text)
  {
    std::vector<std::string> items;
    size_t start = 0;
    int depth = 0;
    auto nextCode = [&](size_t i)
    {
      for (;;)
      {
        while (i < text.size() && isspace((unsigned char)text[i]))
          ++i;
        if (text.compare(i, 2, "//") == 0)
          i = std::min(text.find('\n', i), text.size());
        else if (text.compare(i, 2, "/*") == 0)
        {
          size_t e = text.find("*/", i + 2);
          i = e == std::string::npos ? text.size() : e + 2;
        }
        else
          return i;
      }
    };
    for (size_t i = 0; i < text.size();)
    {
      if (text.compare(i, 2, "//") == 0)
      {
        i = std::min(text.find('\n', i), text.size());
        continue;
      }
      if (text.compare(i, 2, "/*") == 0)
      {
        size_t e = text.find("*/", i + 2);
        i = e == std::string::npos ? text.size() : e + 2;
        continue;
      }
      char c = text[i];
      if (c == '"' || (c == '\'' && !isDigitSeparator(text, i)))
      {
        for (++i; i < text.size() && text[i] != c && text[i] != '\n'; ++i)
          i += text[i] == '\\';
        ++i;
        continue;
      }
      if (c == '(' || c == '[' || c == '{')
        ++depth;
      else if (c == ')' || c == ']' || c == '}')
        --depth;
      ++i;
      bool end = depth == 0 && c == ';';
      if (depth == 0 && c == '}')
      {
        size_t n = nextCode(i);
        auto word = [&](std::string_view w)
        { return text.compare(n, w.size(), w) == 0 && (n + w.size() >= text.size() || !isIdentChar(text[n + w.size()])); };
        end = !(n < text.size() && (text[n] == ';' || text[n] == ',' || word("else") || word("while") || word("catch") ||
                                    (isIdentChar(text[n]) && !isdigit((unsigned char)text[n]) && lastCodeChar(text.substr(start, i - start)) == '}' &&
                                     definesType(text.substr(start, i - start)))));
      }
      if (end)
      {
        // (the rest of its line, a trailing comment, is its own)
        size_t e = i;
        while (e < text.size() && (text[e] == ' ' || text[e] == '\t'))
          ++e;
        if (text.compare(e, 2, "//") == 0)
          e = std::min(text.find('\n', e), text.size());
        if (e < text.size() && text[e] == '\r')
          ++e;
        if (e < text.size() && text[e] == '\n')
          ++e;
        items.push_back(text.substr(start, e - start));
        start = i = e;
      }
    }
    if (start < text.size())
      items.push_back(text.substr(start));
    return items;
  }

  // The first words of an item's code, comments and attributes left out.
  std::string_view codeStart(std::string_view item)
  {
    size_t i = 0;
    for (;;)
    {
      while (i < item.size() && isspace((unsigned char)item[i]))
        ++i;
      if (item.compare(i, 2, "//") == 0)
        i = std::min(item.find('\n', i), item.size());
      else if (item.compare(i, 2, "/*") == 0)
      {
        size_t e = item.find("*/", i + 2);
        i = e == std::string_view::npos ? item.size() : e + 2;
      }
      else if (item.compare(i, 2, "[[") == 0)
      {
        size_t e = item.find("]]", i + 2);
        i = e == std::string_view::npos ? item.size() : e + 2;
      }
      else
        return item.substr(i);
    }
  }

  // Whether an item defines a class, union or enumeration (`struct S {...} s;` runs on after its `}`).
  bool definesType(std::string_view item)
  {
    std::string_view code = codeStart(item);
    for (std::string_view w : {"typedef", "struct", "class", "union", "enum"})
      if (code.substr(0, w.size()) == w && (code.size() == w.size() || !isIdentChar(code[w.size()])))
        return true;
    return false;
  }

  // Whether an item declares a name (a local, a member, a namespace's variable, function, class or
  // alias), as `when` takes it (design.md 19), or is a statement or something else (an access
  // specifier, static_assert): a declaration starts with a type, its words (const, static,
  // constexpr...) or a class's keyword, and names something after the type: `int x = 3;`,
  // `std::vector<Cell*> neighbors;`, `Timer timer("x");`, `void dump() const {...}`; not `x = 3;`,
  // `f(x);`, `obj.f();`, `return x;`, `++x;`, `f<int>(x);`.
  bool declaresName(std::string_view item)
  {
    std::string_view code = codeStart(item);
    if (code.empty() || !(isalpha((unsigned char)code[0]) || code[0] == '_' || code[0] == ':'))
      return false;
    size_t i = 0;
    auto skipSpace = [&] { while (i < code.size() && isspace((unsigned char)code[i])) ++i; };
    auto word = [&]() -> std::string_view
    {
      skipSpace();
      size_t b = i;
      while (i < code.size() && isIdentChar(code[i]))
        ++i;
      return code.substr(b, i - b);
    };
    static const std::set<std::string_view> statements = {"return", "if", "for", "while", "switch", "do", "delete", "throw", "break",
                                                         "continue", "goto", "case", "default", "else", "try", "static_assert",
                                                         "public", "private", "protected", "using", "namespace", "friend", "template",
                                                         "assert", "release_assert", "co_return", "co_yield", "co_await"};
    static const std::set<std::string_view> leading = {"const", "static", "constexpr", "inline", "thread_local", "mutable", "volatile",
                                                      "extern", "virtual", "explicit", "unsigned", "signed", "long", "short",
                                                      "struct", "class", "union", "enum", "typename", "consteval", "constinit"};
    std::string_view first = word();
    if (first == "using")
    {
      // an alias, `using MapType = HashMap<...>;` (a debug-only one used by a later `when`), not
      // `using namespace` or a using-declaration
      std::string_view alias = word();
      skipSpace();
      return !alias.empty() && alias != "namespace" && i < code.size() && code[i] == '=';
    }
    if (first.empty() || statements.count(first))
      return false;
    if (first == "typedef")
      return true;
    while (leading.count(first))
    {
      size_t save = i;
      std::string_view next = word();
      if (next.empty())
      {
        i = save;
        break;
      }
      first = next;
    }
    // the type's name: NAME(::NAME)*(<...>)?, with `*`, `&` and const after it
    for (;;)
    {
      skipSpace();
      if (i < code.size() && code[i] == '<')
      {
        int depth = 0;
        for (; i < code.size(); ++i)
        {
          depth += code[i] == '<' ? 1 : code[i] == '>' ? -1 : 0;
          if (depth == 0)
            break;
          if (code[i] == ';' || code[i] == '{')
            return false;
        }
        ++i;
        skipSpace();
      }
      if (code.compare(i, 2, "::") == 0)
      {
        i += 2;
        if (word().empty())
          return false;
        continue;
      }
      break;
    }
    for (;;)
    {
      skipSpace();
      if (i < code.size() && (code[i] == '*' || code[i] == '&'))
        ++i;
      else if (code.compare(i, 5, "const") == 0 && (i + 5 >= code.size() || !isIdentChar(code[i + 5])))
        i += 5;
      else
        break;
    }
    // then the declared name (an operator is a member function too)
    skipSpace();
    if (code.compare(i, 8, "operator") == 0)
      return true;
    std::string_view name = word();
    if (name.empty())
      return false; // (`f(x);`, `x = 3;`; a constructor declared in one configuration stays `if constexpr`)
    skipSpace();
    return i < code.size() && strchr("=;({[,:", code[i]);
  }

  // A declaration that differs per configuration only in its initializer (`constexpr i32 ROW_STEP = 4;`
  // in debug, `= 2;` in release): one declaration choosing by the configuration, design.md 19's
  // `constexpr i32 ROW_STEP = config::debug ? 4 : 2;`. "" when the two are not so.
  std::string perConfigurationInitializer(const std::string& debug, const std::string& release, const MergeDimension& dimension)
  {
    auto split = [](const std::string& item, std::string& head, std::string& value, std::string& tail)
    {
      size_t semicolon = item.rfind(';');
      size_t equals = item.find(" = ");
      if (semicolon == std::string::npos || equals == std::string::npos || equals > semicolon)
        return false;
      head = item.substr(0, equals);
      value = item.substr(equals + 3, semicolon - equals - 3);
      tail = item.substr(semicolon);
      return head.find_first_of("({") == std::string::npos && value.find_first_of(";{") == std::string::npos;
    };
    // (each value a literal: a choice's both arms are compiled in both configurations, so a name of one
    // configuration's, SpiderLeg's `this->debugDraw`, is two `when`s instead)
    auto literal = [](const std::string& value)
    {
      for (size_t i = 0; i < value.size();)
        if (isalpha((unsigned char)value[i]) || value[i] == '_')
        {
          size_t e = i;
          while (e < value.size() && isIdentChar(value[e]))
            ++e;
          std::string word = value.substr(i, e - i);
          if (word != "true" && word != "false" && word != "nullptr")
            return false;
          i = e;
        }
        else if (isdigit((unsigned char)value[i]))
        {
          while (i < value.size() && (isIdentChar(value[i]) || value[i] == '.'))
            ++i;
        }
        else if (strchr(" \t+-()", value[i]))
          ++i;
        else
          return false;
      return true;
    };
    std::string dh, dv, dt, rh, rv, rt;
    if (!split(debug, dh, dv, dt) || !split(release, rh, rv, rt) || dh != rh || dv == rv || !literal(dv) || !literal(rv))
      return "";
    return dh + " = " + dimension.on + " ? " + dv + " : " + rv + dt;
  }

  // A group whose configurations' texts differ, where declarations and statements stand (after `;`,
  // `{`, `}` or the `:` of an access specifier or label), each text whole ones (ending with `;` or
  // `}`), in a block, a class or a namespace (design.md 19):
  // - a declaration of one configuration is a `when (config::debug)` (`when (!config::debug)`) one:
  //   it is the enclosing scope's, so a later group's use sees it, an object lives to the end of
  //   the block, a member is in the class's layout where the configuration has it;
  // - one declaration in each that differs only in its initializer is one, choosing:
  //   `constexpr i32 ROW_STEP = config::debug ? 4 : 2;`;
  // - the other items, statements, are `if constexpr (config::debug) { ... }` blocks, in order.
  // Anywhere else (an initializer list, a parameter list, an expression) is a problem, reported.
  std::string conditionalGroup(const std::string& debug, const std::string& release, std::string_view before, const std::string& where,
                               MergeReport& report, const MergeDimension& dimension)
  {
    auto whole = [](const std::string& text)
    {
      char last = lastCodeChar(text);
      return last == 0 || last == ';' || last == '}';
    };
    if (!statementAfter(before) || !whole(debug) || !whole(release))
    {
      report.problems.push_back(where + ": the configurations differ where no statement or declaration stands (" + dimension.onName + " `" +
                                firstLine(debug) + "`, " + dimension.offName + " `" + firstLine(release) + "`)");
      return debug;
    }
    ++report.conditional;
    const std::string& sample = blankText(debug) ? release : debug;
    size_t first = sample.find_first_not_of(" \t\r\n");
    size_t lineStart = sample.rfind('\n', first);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    std::string indent = sample.substr(lineStart, first - lineStart);
    auto block = [&](const std::string& text)
    {
      std::string out = indent + "{\n";
      for (size_t start = 0; start < text.size();)
      {
        size_t e = text.find('\n', start);
        if (e == std::string::npos)
          e = text.size();
        std::string line = text.substr(start, e - start);
        out += (line.find_first_not_of(" \t\r") == std::string::npos ? std::string() : "  " + line) + "\n";
        start = e + 1;
      }
      return out + indent + "}\n";
    };
    std::vector<std::string> debugItems = groupItems(debug), releaseItems = groupItems(release);
    auto onlyCode = [&](const std::vector<std::string>& items)
    {
      std::vector<std::string> code;
      for (const std::string& item : items)
        if (!blankText(item) && lastCodeChar(item) != 0)
          code.push_back(item);
      return code;
    };
    if (std::vector<std::string> d = onlyCode(debugItems), r = onlyCode(releaseItems); d.size() == 1 && r.size() == 1 && declaresName(d[0]))
      if (std::string chosen = perConfigurationInitializer(d[0], r[0], dimension); !chosen.empty())
      {
        ++report.perConfiguration;
        return chosen;
      }
    // One configuration's items: each declaration a `when` (an alias too: Gui.cpp's debug-only
    // `using MapType`, design.md 19), each run of statements one block.
    auto items = [&](const std::vector<std::string>& list, const char* condition)
    {
      std::string out, statements;
      auto flush = [&]
      {
        if (!blankText(statements))
          out += indent + "if constexpr (" + condition + ")\n" + block(statements);
        else
          out += statements;
        statements.clear();
      };
      for (const std::string& item : list)
        if (std::string_view code = codeStart(item); code.substr(0, 14) == "static_assert(")
        {
          // A static_assert of one configuration (Map.cpp's FINAL_RELEASE one, at namespace scope,
          // where no `if constexpr` stands): one checked where its configuration's is,
          // `static_assert(!config::finalRelease || (condition), "...")`.
          flush();
          size_t comma = 14;
          for (int depth = 0; comma < code.size(); ++comma)
          {
            if (code[comma] == '"')
              break;
            depth += code[comma] == '(';
            depth -= code[comma] == ')';
            if (depth < 0 || (depth == 0 && code[comma] == ','))
              break;
          }
          size_t at = item.size() - code.size();
          std::string negated = condition[0] == '!' ? std::string(condition + 1) : "!" + std::string(condition);
          out += item.substr(0, at) + "static_assert(" + negated + " || (" + std::string(code.substr(14, comma - 14)) + ")" + std::string(code.substr(comma));
        }
        else if (std::string_view code = codeStart(item); code.substr(0, 6) == "when (")
        {
          // A `when` of the other dimension's merge (the debug and release texts made one, in a group
          // whose three texts differ): one `when` of both conditions.
          flush();
          size_t close = 5;
          for (int depth = 0; close < code.size(); ++close)
          {
            depth += code[close] == '(';
            depth -= code[close] == ')';
            if (depth == 0)
              break;
          }
          size_t at = item.size() - code.size();
          out += item.substr(0, at) + "when (" + condition + " && " + std::string(code.substr(6, close - 6)) + ")" + std::string(code.substr(close + 1));
          ++report.when;
        }
        else if (declaresName(item))
        {
          flush();
          // (its comments and blank lines before it stay before it)
          std::string_view code = codeStart(item);
          size_t at = item.size() - code.size();
          out += item.substr(0, at) + "when (" + condition + ") " + std::string(code);
          ++report.when;
        }
        else
          statements += item;
      flush();
      return out;
    };
    return items(debugItems, dimension.on) + items(releaseItems, dimension.off);
  }

  // Whether a group's debug text is its release text with asserts (Lua's check_exp, `(lua_assert(c),
  // (e))` in debug and `(e)` in release; its lua_assert, `assert(c)` and `((void)0)`): kpp's assert
  // is the configuration's own (nothing where config::debug is false), so the debug text is right in
  // both. Compared with the asserts taken out of the debug text, `(void)0` out of the release one,
  // and parentheses, commas and white space out of both.
  bool differsByAsserts(const std::string& debug, const std::string& release)
  {
    if (debug.find("assert(") == std::string::npos)
      return false;
    std::string stripped;
    for (size_t i = 0; i < debug.size();)
    {
      // `([&] { assert(...); }())`, the converter's assert in an expression, or `assert(...)`
      size_t lambda = debug.compare(i, 14, "([&] { assert(") == 0 ? i : std::string::npos;
      size_t call = lambda == std::string::npos && debug.compare(i, 7, "assert(") == 0 && (i == 0 || !isIdentChar(debug[i - 1])) ? i : std::string::npos;
      if (lambda == std::string::npos && call == std::string::npos)
      {
        stripped += debug[i++];
        continue;
      }
      size_t open = lambda != std::string::npos ? lambda : i + 6;
      int depth = 0;
      size_t k = open;
      for (; k < debug.size(); ++k)
      {
        if (debug[k] == '"' || (debug[k] == '\'' && !isDigitSeparator(debug, k)))
        {
          char quote = debug[k];
          for (++k; k < debug.size() && debug[k] != quote; ++k)
            k += debug[k] == '\\';
          continue;
        }
        depth += debug[k] == '(';
        depth -= debug[k] == ')';
        if (depth == 0)
          break;
      }
      i = k + 1;
    }
    auto normal = [](const std::string& text)
    {
      std::string out;
      for (size_t i = 0; i < text.size(); ++i)
      {
        if (text.compare(i, 2, "//") == 0)
          i = std::min(text.find('\n', i), text.size());
        else if (text.compare(i, 2, "/*") == 0)
          i = std::min(text.find("*/", i + 2), text.size() - 2) + 1;
        else if (!isspace((unsigned char)text[i]) && text[i] != '(' && text[i] != ')' && text[i] != ',' && text[i] != ';')
          out += text[i];
      }
      for (size_t at = out.find("void0"); at != std::string::npos; at = out.find("void0"))
        out.erase(at, 5);
      return out;
    };
    // (an `else` whose statement is an assert, PlayerInputSource's `#ifdef DEBUG else assert(false);
    // #endif`: an empty else in release)
    std::string releaseText = normal(release), debugText = normal(stripped);
    return debugText == releaseText || (releaseText.empty() && debugText == "else");
  }

  // The two configurations' parts as one text: their text parts the same, each group the same (its
  // text once) or written by conditionalGroup. False where a text part differs or the parts do not
  // match (a mark the conversion lost in one of them).
  // With a third configuration (final, FinalRelease's parts; null for none): a group whose release
  // and final texts are the same is chosen by config::debug as above; one whose debug and release
  // texts are the same by config::finalRelease (final the first text); one whose three texts all
  // differ is a problem, reported.
  // dimension: what the first two texts are chosen by (config::debug, or config::platform for
  // --convert-merge-platform, which keeps the groups' marks: keepMarks).
  bool mergeParts(const std::vector<MarkedPart>& debug, const std::vector<MarkedPart>& release, const std::vector<MarkedPart>* final, std::string& out,
                  const std::string& file, MergeReport& report, const MergeDimension& firstDimension = debugDimension, bool keepMarks = false)
  {
    if (debug.size() != release.size() || (final && final->size() != debug.size()))
      return false;
    MergeReport before = report;
    std::string merged;
    size_t skipSemicolon = std::string::npos; // the next text part's `;`, which a statement's group took
    for (size_t i = 0; i < debug.size(); ++i)
    {
      const MarkedPart& d = debug[i];
      const MarkedPart& r = release[i];
      const MarkedPart* f = final ? &(*final)[i] : nullptr;
      bool same = d.group == r.group && (!f || f->group == d.group) && (!d.group.empty() || (d.text == r.text && (!f || f->text == d.text)));
      if (!same)
      {
        report = before;
        return false;
      }
      if (d.group.empty())
      {
        merged += skipSemicolon == std::string::npos ? d.text : d.text.substr(0, skipSemicolon) + d.text.substr(skipSemicolon + 1);
        skipSemicolon = std::string::npos;
        continue;
      }
      std::string inner;
      if (mergeParts(d.parts, r.parts, f ? &f->parts : nullptr, inner, file, report, firstDimension, keepMarks))
        merged += keepMarks ? marked(d.group, inner) : inner;
      else if (auto choice = report.choices.find(file + " " + d.group); !keepMarks && choice != report.choices.end())
      {
        merged += flattened(choice->second == "release" ? r.parts : choice->second == "final" && f ? f->parts : d.parts);
        report.chosen.insert(choice->first);
      }
      else
      {
        std::string dt = flattened(d.parts), rt = flattened(r.parts);
        std::string ft = f ? flattened(f->parts) : rt;
        const MergeDimension* dimension = &firstDimension;
        std::string on = dt, off = rt;
        if (ft != rt)
        {
          // All three differ (an IMGUI_ENABLED block that FinalRelease leaves out, its DEBUG groups
          // inside): the debug and release texts made one first, chosen by config::debug, and that
          // text against the final one by config::finalRelease (conditionalGroup joins a `when` in
          // it: `when (!config::finalRelease && config::debug)`).
          if (dt != rt)
          {
            std::string debugAndRelease;
            if (!mergeParts(d.parts, r.parts, nullptr, debugAndRelease, file, report))
              debugAndRelease = conditionalGroup(dt, rt, out + merged, file + " (#if group " + d.group + ")", report, debugDimension);
            off = debugAndRelease;
            ++report.allThree;
          }
          dimension = &finalDimension;
          on = ft;
          ++report.final;
        }
        // (an assert is the configuration's own, config::debug's; a release_assert is in every build)
        else if (&firstDimension == &debugDimension && differsByAsserts(dt, rt))
        {
          merged += dt;
          ++report.byAsserts;
          continue;
        }
        // Links of an if-else chain that one configuration alone has: each link's condition joined
        // with the configuration's (elseLinksWhen).
        if (blankText(on) != blankText(off))
          if (std::string joined = elseLinksWhen(blankText(on) ? off : on, blankText(on) ? dimension->off : dimension->on); !joined.empty())
          {
            merged += keepMarks ? marked(d.group, joined) : joined;
            ++report.conditional;
            continue;
          }
        // A macro's use within a line (its id `FILE:@OFFSET`): as a statement, the `;` after it
        // closes it, `if constexpr (config::debug) { debug; } else { release; }`.
        bool inLine = on.find('\n') == std::string::npos && off.find('\n') == std::string::npos;
        const MarkedPart* next = i + 1 < debug.size() && debug[i + 1].group.empty() ? &debug[i + 1] : nullptr;
        size_t semicolon = next ? next->text.find_first_not_of(" \t") : std::string::npos;
        if (inLine && semicolon != std::string::npos && next->text[semicolon] == ';' && statementAfter(out + merged))
        {
          auto statement = [](const std::string& text) { return blankText(text) ? std::string() : " " + text + ";"; };
          std::string chosen = blankText(on) ? "if constexpr (" + std::string(dimension->off) + ") {" + statement(off) + " }"
                                             : "if constexpr (" + std::string(dimension->on) + ") {" + statement(on) + " }" +
                                                 (blankText(off) ? "" : " else {" + statement(off) + " }");
          merged += keepMarks ? marked(d.group, chosen) : chosen;
          ++report.conditional;
          skipSemicolon = semicolon;
        }
        else
        {
          std::string chosen = conditionalGroup(on, off, out + merged, file + " (#if group " + d.group + ")", report, *dimension);
          merged += keepMarks ? marked(d.group, chosen) : chosen;
        }
      }
    }
    out += merged;
    return true;
  }

  // (final: FinalRelease's text, or null without a third configuration)
  std::string mergedConfigurations(const std::string& file, const std::string& debug, const std::string& release, const std::string* final,
                                   MergeReport& report)
  {
    std::vector<MarkedPart> d, r, f;
    size_t a = 0, b = 0, c = 0;
    std::vector<std::string> openD, openR, openF;
    if (!parseMarked(debug, a, "", d, openD) || !parseMarked(release, b, "", r, openR) || (final && !parseMarked(*final, c, "", f, openF)))
    {
      report.problems.push_back(file + ": its #if marks do not nest");
      return debug;
    }
    std::string out;
    if (mergeParts(d, r, final ? &f : nullptr, out, file, report))
      return out;
    // (the first line where they differ: debug's and release's, else release's and final's)
    std::string fd = flattened(d), fr = flattened(r);
    const char* firstName = "debug";
    const char* secondName = "release";
    if (final && fd == fr)
    {
      fr = flattened(f);
      firstName = "release";
      secondName = "final";
    }
    size_t at = 0, line = 1;
    while (at < fd.size() && at < fr.size() && fd[at] == fr[at])
      line += fd[at++] == '\n';
    size_t lineStart = fd.rfind('\n', at == 0 ? 0 : at - 1);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    report.problems.push_back(file + ":" + std::to_string(line) + ": the configurations differ outside an #if group (" + firstName + " `" +
                              firstLine(fd.substr(lineStart)) + "`, " + secondName + " `" + firstLine(fr.substr(lineStart)) + "`)");
    return fd;
  }

  // A library's c-imports.kpp: the debug one's imports, then the release one's it lacks (an import is
  // harmless where nothing uses it).
  std::string mergedImports(const std::string& debug, const std::string& release)
  {
    std::string out = debug;
    if (!out.empty() && out.back() != '\n')
      out += '\n';
    for (size_t start = 0; start < release.size();)
    {
      size_t e = release.find('\n', start);
      if (e == std::string::npos)
        e = release.size();
      std::string line = release.substr(start, e - start);
      if (line.rfind("extern \"C\" import", 0) == 0 && debug.find(line) == std::string::npos)
        out += line + "\n";
      start = e + 1;
    }
    return out;
  }
}

int convertMerge(const std::filesystem::path& debugDir, const std::filesystem::path& releaseDir, const std::filesystem::path& outDir,
                 const std::filesystem::path& choicesFile, const std::filesystem::path& finalDir)
{
  MergeReport report;
  // The groups whose texts no `if constexpr` can choose between (an expression's part), each taken
  // from one configuration for all: `FILE GROUP debug|release|final  why`, reviewed by hand.
  if (!choicesFile.empty())
  {
    std::string text;
    if (!readFile(choicesFile, text))
      report.problems.push_back(choicesFile.generic_string() + ": cannot be read");
    for (size_t start = 0; start < text.size();)
    {
      size_t e = text.find('\n', start);
      if (e == std::string::npos)
        e = text.size();
      std::string line = text.substr(start, e - start);
      start = e + 1;
      if (line.empty() || line[0] == '#')
        continue;
      char file[512], group[256], configuration[16];
      if (sscanf(line.c_str(), "%511s %255s %15s", file, group, configuration) == 3 &&
          (std::string(configuration) == "debug" || std::string(configuration) == "release" || std::string(configuration) == "final"))
        report.choices[std::string(file) + " " + group] = configuration;
      else
        report.problems.push_back(choicesFile.generic_string() + ": not `FILE GROUP debug|release|final why`: " + line);
    }
  }
  std::map<std::string, std::string> merged; // the converted files, by path below outDir
  size_t files = 0;
  std::error_code ec;
  auto write = [&](const std::string& rel, const std::string& text)
  {
    std::filesystem::path target = outDir / rel;
    std::filesystem::create_directories(target.parent_path(), ec);
    writeConvertedFile(target, text);
  };
  for (const auto& entry : std::filesystem::recursive_directory_iterator(debugDir, ec))
  {
    if (!entry.is_regular_file(ec))
      continue;
    std::string rel = entry.path().lexically_relative(debugDir).generic_string();
    std::string debug, release, final;
    readFile(entry.path(), debug);
    if (entry.path().extension() != ".kpp")
    {
      write(rel, debug); // CONVERSION.md, the configured headers
      continue;
    }
    ++files;
    if (!readFile(releaseDir / rel, release))
    {
      report.problems.push_back(rel + ": only the debug configuration writes it");
      merged[rel] = debug;
    }
    else if (!finalDir.empty() && !readFile(finalDir / rel, final))
    {
      report.problems.push_back(rel + ": the final configuration does not write it");
      merged[rel] = mergedConfigurations(rel, debug, release, nullptr, report);
    }
    else if (debug == release && (finalDir.empty() || final == release) && debug.find("/*kpp:") == std::string::npos)
      write(rel, debug); // (a hand-written file, or one without an #if)
    else if (entry.path().filename() == "c-imports.kpp")
      merged[rel] = finalDir.empty() ? mergedImports(debug, release) : mergedImports(mergedImports(debug, release), final);
    else
      merged[rel] = mergedConfigurations(rel, debug, release, finalDir.empty() ? nullptr : &final, report);
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(releaseDir, ec))
    if (entry.is_regular_file(ec) && entry.path().extension() == ".kpp" &&
        !std::filesystem::exists(debugDir / entry.path().lexically_relative(releaseDir), ec))
      report.problems.push_back(entry.path().lexically_relative(releaseDir).generic_string() + ": only the release configuration writes it");
  if (!finalDir.empty())
    for (const auto& entry : std::filesystem::recursive_directory_iterator(finalDir, ec))
      if (entry.is_regular_file(ec) && entry.path().extension() == ".kpp" &&
          !std::filesystem::exists(debugDir / entry.path().lexically_relative(finalDir), ec))
        report.problems.push_back(entry.path().lexically_relative(finalDir).generic_string() + ": only the final configuration writes it");
  // The cleanups again, now that the marks are gone: an #if group's marks held a namespace open, and
  // their lines stood between blank ones.
  std::set<std::string> holding;
  for (const auto& [rel, text] : merged)
    noteNamespacesHolding(text, holding);
  size_t namespacesRemoved = 0, namespacesKept = 0, blankLines = 0;
  for (auto& [rel, text] : merged)
    write(rel, withTidyBlankLines(withoutEmptyNamespaces(text, holding, namespacesRemoved, namespacesKept), blankLines));
  // (a choice of a file this merge has not, the tests' in the conversion without them, is another
  // merge's)
  for (const auto& [key, configuration] : report.choices)
    if (!report.chosen.count(key) && std::filesystem::exists(debugDir / key.substr(0, key.find(' ')), ec))
      report.problems.push_back(choicesFile.generic_string() + ": `" + key + "` chooses for no group whose configurations differ");
  std::string summary = "\n## The two configurations made one (kppc --convert-merge)\n\n- files: " + std::to_string(files) +
                        "\n- #if groups whose configurations differ, written with `when (config::debug)` and `if constexpr (config::debug)` (design.md 19): " + std::to_string(report.conditional) +
                        "\n- their declarations, each `when (config::debug)` or `when (!config::debug)`: " + std::to_string(report.when) +
                        "\n- their declarations whose initializer alone differs, `= config::debug ? a : b`: " + std::to_string(report.perConfiguration) +
                        "\n- groups whose final text alone differs (FinalRelease's), written with `when (config::finalRelease)` and `if constexpr (config::finalRelease)`: " +
                        std::to_string(report.final) + "\n- groups whose debug, release and final texts all differ (problems): " + std::to_string(report.allThree) +
                        "\n- groups whose configurations differ by asserts alone, written as the debug one (kpp's assert is the configuration's): " +
                        std::to_string(report.byAsserts) + "\n- groups taken from one configuration (" + choicesFile.filename().string() + "): " +
                        std::to_string(report.chosen.size()) + "\n- empty namespaces removed once the marks were gone: " + std::to_string(namespacesRemoved) +
                        "\n- problems: " + std::to_string(report.problems.size()) + "\n";
  for (const std::string& p : report.problems)
    summary += "  - " + p + "\n";
  writeConvertedFile(outDir / "CONVERSION.md", summary, "ab");
  printf("convert-merge: %zu files, %zu groups whose configurations differ (%zu declarations when, %zu initializers chosen by the configuration), %zu of them final's, %zu differing by asserts alone, %zu problems\n",
         files, report.conditional, report.when, report.perConfiguration, report.final, report.byAsserts, report.problems.size());
  for (const std::string& p : report.problems)
    fprintf(stderr, "convert-merge: %s\n", p.c_str());
  return report.problems.empty() ? 0 : 1;
}

// --- one configuration's two platforms made one (CONVERSION.md TODO 17) ---

int convertMergePlatform(const std::filesystem::path& windowsDir, const std::filesystem::path& linuxDir, const std::filesystem::path& outDir)
{
  MergeReport report;
  std::error_code ec;
  size_t files = 0, onePlatform = 0;
  auto write = [&](const std::string& rel, const std::string& text)
  {
    std::filesystem::path target = outDir / rel;
    std::filesystem::create_directories(target.parent_path(), ec);
    writeConvertedFile(target, text);
  };
  // (a file one platform alone has: its declarations are that platform's)
  auto alone = [&](const std::string& text, const char* platform)
  {
    ++onePlatform;
    return std::string("requires config::platform == Platform::") + platform + ";\n" + text;
  };
  // An imports file: the imports both have, then each platform's own in a file of its own, which
  // requires the platform (an import cannot be `when`).
  std::map<std::string, std::string> platformImports;
  auto splitImports = [&](const std::string& rel, const std::string& windows, const std::string& linux)
  {
    auto lines = [](const std::string& text)
    {
      std::vector<std::string> out;
      for (size_t start = 0; start < text.size();)
      {
        size_t e = text.find('\n', start);
        if (e == std::string::npos)
          e = text.size();
        out.push_back(text.substr(start, e - start));
        start = e + 1;
      }
      return out;
    };
    std::vector<std::string> w = lines(windows), l = lines(linux);
    std::string common, windowsOnly, linuxOnly;
    for (const std::string& line : w)
      if (line.rfind("extern \"C\" import", 0) != 0 || std::find(l.begin(), l.end(), line) != l.end())
        common += line + "\n";
      else
        windowsOnly += line + "\n";
    for (const std::string& line : l)
      if (line.rfind("extern \"C\" import", 0) == 0 && std::find(w.begin(), w.end(), line) == w.end())
        linuxOnly += line + "\n";
    write(rel, common);
    std::string stem = rel.substr(0, rel.size() - 4);
    if (!windowsOnly.empty())
      write(stem + "-windows.kpp", "requires config::platform == Platform::Windows;\n" + windowsOnly);
    if (!linuxOnly.empty())
      write(stem + "-linux.kpp", "requires config::platform == Platform::Linux;\n" + linuxOnly);
  };
  for (const auto& entry : std::filesystem::recursive_directory_iterator(windowsDir, ec))
  {
    if (!entry.is_regular_file(ec))
      continue;
    std::string rel = entry.path().lexically_relative(windowsDir).generic_string();
    std::string windows, linux;
    readFile(entry.path(), windows);
    bool both = readFile(linuxDir / rel, linux);
    if (entry.path().extension() != ".kpp")
    {
      write(rel, windows); // CONVERSION.md (Windows'), the configured headers
      continue;
    }
    ++files;
    if (!both)
    {
      write(rel, alone(windows, "Windows"));
      continue;
    }
    if (entry.path().filename().string().rfind("c-imports", 0) == 0)
    {
      splitImports(rel, windows, linux);
      continue;
    }
    std::vector<MarkedPart> w, l;
    size_t a = 0, b = 0;
    std::vector<std::string> openW, openL;
    std::string out;
    if (!parseMarked(windows, a, "", w, openW) || !parseMarked(linux, b, "", l, openL))
      report.problems.push_back(rel + ": its #if marks do not nest");
    else if (mergeParts(w, l, nullptr, out, rel, report, platformDimension, true))
    {
      write(rel, out);
      continue;
    }
    else
    {
      std::string fw = flattened(w), fl = flattened(l);
      size_t at = 0, line = 1;
      while (at < fw.size() && at < fl.size() && fw[at] == fl[at])
        line += fw[at++] == '\n';
      size_t lineStart = fw.rfind('\n', at == 0 ? 0 : at - 1);
      lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
      report.problems.push_back(rel + ":" + std::to_string(line) + ": the platforms differ outside an #if group (windows `" + firstLine(fw.substr(lineStart)) +
                                "`, linux `" + firstLine(fl.substr(lineStart)) + "`)");
    }
    write(rel, windows);
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(linuxDir, ec))
    if (entry.is_regular_file(ec) && entry.path().extension() == ".kpp")
    {
      std::string rel = entry.path().lexically_relative(linuxDir).generic_string();
      if (std::filesystem::exists(windowsDir / rel, ec))
        continue;
      std::string linux;
      readFile(entry.path(), linux);
      ++files;
      write(rel, alone(linux, "Linux"));
    }
  printf("convert-merge-platform: %zu files, %zu groups whose platforms differ, %zu files of one platform, %zu problems\n", files, report.conditional,
         onePlatform, report.problems.size());
  for (const std::string& problem : report.problems)
    fprintf(stderr, "convert-merge-platform: %s\n", problem.c_str());
  return report.problems.empty() ? 0 : 1;
}
