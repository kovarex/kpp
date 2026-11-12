// StyleEmptyLines.inl: the empty lines of kpp's code style (docs/code-style.md, "Empty lines between
// definitions"; tools/style/kppstyle.py's --fix, which this follows) in a converted file:
// no two consecutive empty lines, none after a block's `{` or before its `}`, and an empty line before
// and after an expanded definition (a function, class, enum or namespace written over several lines)
// among declarations, above the comment that belongs to it. The C++'s .cpp kept its functions apart
// with empty lines, which the bodies the converter moves into their class lose. Only empty lines
// change, never a line inside a comment or a string that spans lines (a raw one, or one a backslash
// continues). Unlike kppstyle.py (reported to its owner), a template's `<class T>` does not make a
// function a class, and a brace initializer or a lambda's body among a constructor's initializers is
// no expanded definition. Run by convertMerge, the last step, on every .kpp file it writes.

namespace styleEmptyLines
{
  inline std::string_view trimmed(std::string_view s)
  {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b]))
      ++b;
    while (e > b && isspace((unsigned char)s[e - 1]))
      --e;
    return s.substr(b, e - b);
  }

  inline bool endsWith(std::string_view s, std::string_view end)
  {
    return s.size() >= end.size() && s.substr(s.size() - end.size()) == end;
  }

  inline bool hasWord(std::string_view s, std::string_view word)
  {
    for (size_t p = s.find(word); p != std::string_view::npos; p = s.find(word, p + 1))
    {
      bool startOk = p == 0 || !isIdentChar(s[p - 1]);
      bool endOk = p + word.size() >= s.size() || !isIdentChar(s[p + word.size()]);
      if (startOk && endOk)
        return true;
    }
    return false;
  }

  // `public:`, `private :`
  inline bool isAccessLabel(std::string_view s)
  {
    for (std::string_view label : {"public", "private", "protected"})
      if (s.substr(0, label.size()) == label && trimmed(s.substr(label.size())) == ":")
        return true;
    return false;
  }

  // Each line with its comments and string and character literals blanked (raw strings too), and
  // whether it starts inside a block comment or a raw string.
  inline void codeOf(const std::vector<std::string>& lines, std::vector<std::string>& code, std::vector<bool>& inside)
  {
    bool inBlock = false;
    std::string rawEnd; // `)delim"` of a raw string that spans lines
    bool inRaw = false;
    char inString = 0; // the quote of a literal that a backslash at a line's end continues
    for (const std::string& line : lines)
    {
      inside.push_back(inBlock || inRaw || inString);
      std::string s = line;
      size_t i = 0, n = line.size();
      auto blank = [&](size_t from, size_t to)
      {
        for (size_t k = from; k < std::min(to, n); ++k)
          s[k] = ' ';
      };
      while (i < n)
      {
        if (inString)
        {
          size_t j = i;
          while (j < n && line[j] != inString)
            j += line[j] == '\\' ? 2 : 1;
          blank(i, j + 1);
          inString = j >= n && !line.empty() && line.back() == '\\' ? inString : 0;
          i = j + 1;
          continue;
        }
        if (inRaw)
        {
          size_t j = line.find(rawEnd, i);
          if (j == std::string::npos)
          {
            blank(i, n);
            break;
          }
          blank(i, j + rawEnd.size());
          i = j + rawEnd.size();
          inRaw = false;
          continue;
        }
        if (inBlock)
        {
          size_t j = line.find("*/", i);
          if (j == std::string::npos)
          {
            blank(i, n);
            break;
          }
          blank(i, j + 2);
          i = j + 2;
          inBlock = false;
          continue;
        }
        char c = line[i];
        if (line.compare(i, 2, "//") == 0)
        {
          blank(i, n);
          break;
        }
        if (line.compare(i, 2, "/*") == 0)
        {
          inBlock = true;
          continue;
        }
        // A raw string: an optional encoding prefix, R"delim(
        if (i == 0 || !isIdentChar(line[i - 1]))
        {
          size_t p = i;
          if (line.compare(p, 2, "u8") == 0)
            p += 2;
          else if (p < n && (line[p] == 'u' || line[p] == 'U' || line[p] == 'L'))
            ++p;
          if (line.compare(p, 2, "R\"") == 0)
          {
            size_t open = line.find('(', p + 2);
            bool delimOk = open != std::string::npos && open - (p + 2) <= 16;
            for (size_t k = p + 2; delimOk && k < open; ++k)
              delimOk = !isspace((unsigned char)line[k]);
            if (delimOk)
            {
              rawEnd = ")" + line.substr(p + 2, open - (p + 2)) + "\"";
              inRaw = true;
              blank(i, open + 1);
              i = open + 1;
              continue;
            }
          }
        }
        if (c == '"' || (c == '\'' && !(i > 0 && isalnum((unsigned char)line[i - 1])))) // (1'000 digit separators)
        {
          size_t j = i + 1;
          while (j < n && line[j] != c)
            j += line[j] == '\\' ? 2 : 1;
          blank(i, j + 1);
          if (j >= n && line.back() == '\\')
            inString = c; // (`"text\` continued on the next line)
          i = j + 1;
          continue;
        }
        ++i;
      }
      code.push_back(s);
    }
  }

  // The first line of what the `{` at line i, column j opens: its own line when code stands before
  // it, else the lines above back to the end of the previous statement or declaration.
  inline size_t headOf(const std::vector<std::string>& lines, const std::vector<std::string>& code, size_t i, size_t j)
  {
    size_t k;
    if (!trimmed(std::string_view(code[i]).substr(0, j)).empty())
      k = i;
    else
    {
      if (i == 0)
        return i;
      k = i - 1;
    }
    while (k > 0)
    {
      std::string_view prev = trimmed(code[k - 1]);
      if (prev.empty() || endsWith(prev, ";") || endsWith(prev, "}") || endsWith(prev, "{") || isAccessLabel(prev) ||
          trimmed(lines[k - 1]).substr(0, 2) == "//")
        break;
      --k;
    }
    return k;
  }
}

inline std::string withStyleEmptyLinesOnce(const std::string& text)
{
  using namespace styleEmptyLines;
  std::vector<std::string> lines;
  for (size_t start = 0; start < text.size();)
  {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  bool finalNewline = !text.empty() && text.back() == '\n';
  std::vector<std::string> code;
  std::vector<bool> inside;
  codeOf(lines, code, inside);
  auto empty = [&](size_t i) { return trimmed(lines[i]).empty(); };
  enum Rule : unsigned
  {
    DoubleEmpty = 1,
    EmptyAfterOpen = 2,
    EmptyBeforeClose = 4,
    ExpandedBefore = 8,
    ExpandedAfter = 16
  };
  std::map<size_t, unsigned> found; // by line number from 1
  for (size_t i = 1; i < lines.size(); ++i)
    if (empty(i) && empty(i - 1))
      found[i + 1] |= DoubleEmpty;
  struct Open
  {
    size_t line;
    bool declarations;
    size_t head;
    bool definition;
  };
  std::vector<Open> opens;
  for (size_t i = 0; i < code.size(); ++i)
    for (size_t j = 0; j < code[i].size(); ++j)
    {
      char ch = code[i][j];
      if (ch == '{')
      {
        size_t head = headOf(lines, code, i, j);
        bool enclosingDeclarations = opens.empty() || opens.back().declarations;
        std::string headCode, headText;
        for (size_t k = head; k <= i; ++k)
          headCode += (k > head ? " " : "") + code[k], headText += (k > head ? " " : "") + lines[k];
        // (a template's parameters are no class: `template<class T> void f()` is a function)
        for (size_t t = headCode.find("template"); t != std::string::npos; t = headCode.find("template", t + 1))
        {
          size_t open = headCode.find_first_not_of(" ", t + 8);
          if (open == std::string::npos || headCode[open] != '<')
            continue;
          int depth = 0;
          size_t close = open;
          for (; close < headCode.size(); ++close)
            if (headCode[close] == '<')
              ++depth;
            else if (headCode[close] == '>' && --depth == 0)
              break;
          for (size_t k = open; k < std::min(close + 1, headCode.size()); ++k)
            headCode[k] = ' ';
        }
        // (what the brace opens is declared after the head line's last `;` or `}`: a test macro's
        // one line holds classes before the function whose body it opens)
        std::string_view beforeBrace = std::string_view(headCode).substr(0, headCode.size() - (code[i].size() - j));
        if (size_t last = beforeBrace.find_last_of(";}"); last != std::string_view::npos)
          beforeBrace = beforeBrace.substr(last + 1);
        bool own = hasWord(beforeBrace, "class") || hasWord(beforeBrace, "struct") || hasWord(beforeBrace, "union") ||
                   hasWord(beforeBrace, "enum") || hasWord(beforeBrace, "namespace") || endsWith(trimmed(beforeBrace), "extern");
        if (!own)
          for (size_t p = headText.find("extern"); p != std::string::npos && !own; p = headText.find("extern", p + 1))
            own = trimmed(std::string_view(headText).substr(p + 6)).substr(0, 3) == "\"C\"";
        // An expanded definition's `{` starts its line (or follows a class's, an enum's... head on it),
        // outside any parentheses: not an initializer's (`allCollections{`, `= {`), nor a lambda's
        // body among a constructor's initializers.
        int parens = 0;
        for (size_t k = head; k <= i; ++k)
          for (size_t c = 0; c < (k == i ? j : code[k].size()); ++c)
            parens += code[k][c] == '(' ? 1 : code[k][c] == ')' ? -1 : 0;
        bool definition = parens <= 0 && (own || trimmed(std::string_view(code[i]).substr(0, j)).empty());
        opens.push_back({i, enclosingDeclarations && own, head, definition});
      }
      else if (ch == '}' && !opens.empty())
      {
        Open open = opens.back();
        opens.pop_back();
        bool enclosingDeclarations = opens.empty() || opens.back().declarations;
        size_t oi = open.line, ci = i;
        if (oi == ci)
          continue;
        if (oi + 1 < lines.size() && empty(oi + 1) && endsWith(trimmed(code[oi]), "{"))
          found[oi + 2] |= EmptyAfterOpen;
        if (ci - 1 > oi && empty(ci - 1) && trimmed(code[ci]).substr(0, 1) == "}")
          found[ci] |= EmptyBeforeClose;
        if (!enclosingDeclarations || !open.definition)
          continue;
        // (the comment right above it is its own, a `/** ... */` as a `//`: a line of comment and nothing else)
        size_t top = open.head;
        while (top > 0 && !empty(top - 1) && trimmed(code[top - 1]).empty())
          --top;
        if (top > 0)
        {
          std::string_view prev = trimmed(code[top - 1]);
          if (!empty(top - 1) && !endsWith(prev, "{") && !isAccessLabel(prev))
            found[top + 1] |= ExpandedBefore;
        }
        if (ci + 1 < lines.size())
        {
          std::string_view next = trimmed(code[ci + 1]);
          if (!empty(ci + 1) && next.substr(0, 1) != "}" && !endsWith(trimmed(code[ci]), ","))
            found[ci + 1] |= ExpandedAfter;
        }
      }
    }
  // Nothing inside a string or a comment that spans lines; one missing empty line between two
  // expanded definitions is one finding.
  for (auto it = found.begin(); it != found.end();)
    if (inside[std::min(it->first, lines.size()) - 1])
      it = found.erase(it);
    else
      ++it;
  for (auto& [n, rules] : found)
    if (rules & ExpandedAfter)
      if (auto next = found.find(n + 1); next != found.end())
        next->second &= ~unsigned(ExpandedBefore);
  if (found.empty())
    return text;
  std::vector<std::string> out = lines;
  for (auto it = found.rbegin(); it != found.rend(); ++it)
  {
    unsigned rules = it->second;
    size_t i = it->first - 1;
    if (rules & ExpandedAfter)
      out.insert(out.begin() + i + 1, "");
    if ((rules & (EmptyAfterOpen | EmptyBeforeClose)) && trimmed(out[i]).empty())
    {
      out.erase(out.begin() + i);
      continue;
    }
    if (rules & ExpandedBefore)
      out.insert(out.begin() + i, "");
    if ((rules & DoubleEmpty) && i < out.size() && trimmed(out[i]).empty())
      out.erase(out.begin() + i);
  }
  std::string result;
  for (size_t k = 0; k < out.size(); ++k)
    result += out[k] + (k + 1 < out.size() || finalNewline ? "\n" : "");
  return result;
}

// One-liners (docs/code-style.md, "Empty lines between definitions"; kovarex 2026-11-10: "short
// oneliners are preferred, when there is just one command"). A function's body that the .cpp wrote on
// lines of its own, empty (`void Entity::upgradeModules(ItemStack&, ItemCounts&, InventoryBuffer&)`
// then `{}`) or one short statement (`{`, `return false;`, `}`), goes on its declarator's line, as the
// headers write one-liners: `... InventoryBuffer&) {}`, `bool canInsert(CanInsertData) const { return
// false; }`, when the line stays within the 130 characters of a line. The declarator is the line
// before, ending with `)` or with `)` then const, override, final or noexcept, whole as code (no
// comment at its end); not a constructor's initializers written over lines (a line starting with `:`
// or `,`), nor a statement's head (`if (x)`). The statement: one line ending with `;`, no comment, no
// block (`{`), no statement of its own kind (if, for, while...). And no empty line between two
// one-liners that follow one another.
// What the C++ wrote that kpp's likely-slip errors (design.md "The likely slips are errors", --slip-checks)
// take for a mistake, written as kpp says it on purpose, with the same meaning:
// - an `if` or `else` whose body is `;` gets `{}` (`if (pattern[x] == ' ')` / `;` / `else if ...`);
// - an assignment as an if's or a while's condition gets its own parentheses (`if ((player =
//   game->getLocalPlayer()))`, `} while ((quality = quality->next.getPrototype()));`). A declaration in
//   a condition (`if (Player* player = f())`) is none: what stands before its `=` is two words.
inline std::string withSlipsSpelledOut(const std::string& text)
{
  using namespace styleEmptyLines;
  std::vector<std::string> lines;
  for (size_t start = 0; start < text.size();)
  {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  bool finalNewline = !text.empty() && text.back() == '\n';
  std::vector<std::string> code;
  std::vector<bool> inside;
  codeOf(lines, code, inside);
  auto word = [](std::string_view t, size_t at, std::string_view w)
  {
    return t.substr(at, w.size()) == w && (at == 0 || !isIdentChar(t[at - 1])) && (at + w.size() == t.size() || !isIdentChar(t[at + w.size()]));
  };
  size_t previousCode = SIZE_MAX; // the last line with code before line i
  for (size_t i = 0; i < lines.size(); ++i)
  {
    if (inside[i])
      continue;
    std::string_view c = trimmed(code[i]);
    if (c.empty())
      continue;
    if (c == ";" && previousCode != SIZE_MAX)
    {
      std::string_view above = trimmed(code[previousCode]);
      if (above.substr(0, 1) == "}")
        above = trimmed(above.substr(1));
      bool header = (word(above, 0, "if") || (word(above, 0, "else") && word(trimmed(above.substr(4)), 0, "if"))) && endsWith(above, ")") &&
                    above.find("constexpr") == std::string_view::npos;
      if (header || above == "else")
      {
        size_t at = code[i].find(';');
        lines[i].replace(at, 1, "{}");
      }
    }
    for (std::string_view keyword : {"if", "while"})
      for (size_t at = code[i].find(keyword); at != std::string::npos; at = code[i].find(keyword, at + 1))
      {
        if (!word(code[i], at, keyword))
          continue;
        size_t open = code[i].find_first_not_of(' ', at + keyword.size());
        if (open == std::string::npos || code[i][open] != '(')
          continue;
        int depth = 0;
        size_t close = std::string::npos, assignment = std::string::npos;
        bool semicolon = false;
        for (size_t k = open; k < code[i].size(); ++k)
        {
          char ch = code[i][k];
          if (ch == '(' || ch == '[' || ch == '{')
            ++depth;
          else if (ch == ')' || ch == ']' || ch == '}')
          {
            if (--depth == 0)
            {
              close = k;
              break;
            }
          }
          else if (depth == 1 && ch == ';')
            semicolon = true;
          else if (depth == 1 && ch == '=' && assignment == std::string::npos)
          {
            char before = k > 0 ? code[i][k - 1] : ' ', after = k + 1 < code[i].size() ? code[i][k + 1] : ' ';
            if (after != '=' && !strchr("=!<>+-*/%&|^", before))
              assignment = k;
          }
        }
        if (close == std::string::npos || semicolon || assignment == std::string::npos)
          continue;
        std::string_view target = trimmed(std::string_view(code[i]).substr(open + 1, assignment - open - 1));
        if (target.empty() || target.find(' ') != std::string_view::npos || target.find('\t') != std::string_view::npos)
          continue; // (a declaration, `Player* player = ...`)
        lines[i].insert(close, ")");
        lines[i].insert(open + 1, "(");
        code[i].insert(close, ")");
        code[i].insert(open + 1, "(");
        at = close + 2;
      }
    previousCode = i;
  }
  std::string result;
  for (size_t i = 0; i < lines.size(); ++i)
  {
    result += lines[i];
    if (i + 1 < lines.size() || finalNewline)
      result += '\n';
  }
  return result;
}

inline std::string withOneLiners(const std::string& input)
{
  using namespace styleEmptyLines;
  const std::string text = withSlipsSpelledOut(input);
  std::vector<std::string> lines;
  for (size_t start = 0; start < text.size();)
  {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  bool finalNewline = !text.empty() && text.back() == '\n';
  std::vector<std::string> code;
  std::vector<bool> inside;
  codeOf(lines, code, inside);
  auto startsWithKeyword = [](std::string_view t, std::initializer_list<std::string_view> keywords)
  {
    for (std::string_view keyword : keywords)
      if (t.substr(0, keyword.size()) == keyword && (t.size() == keyword.size() || !isIdentChar(t[keyword.size()])))
        return true;
    return false;
  };
  auto declarator = [&](size_t i)
  {
    if (inside[i] || trimmed(lines[i]) != trimmed(code[i]))
      return false;
    std::string_view head = trimmed(code[i]);
    if (head.substr(0, 1) == ":" || head.substr(0, 1) == "," || startsWithKeyword(head, {"if", "for", "while", "switch", "catch", "else", "do"}))
      return false;
    for (std::string_view t = head;;)
    {
      bool stripped = false;
      for (std::string_view word : {"const", "override", "final", "noexcept"})
        if (t.size() > word.size() && t.substr(t.size() - word.size()) == word && !isIdentChar(t[t.size() - word.size() - 1]))
        {
          t = trimmed(t.substr(0, t.size() - word.size()));
          stripped = true;
          break;
        }
      if (!stripped)
        return endsWith(t, ")");
    }
  };
  auto shortStatement = [&](size_t i)
  {
    std::string_view statement = trimmed(lines[i]), statementCode = trimmed(code[i]);
    return !inside[i] && endsWith(statementCode, ";") && statementCode.find('{') == std::string_view::npos &&
           statement.find("//") == std::string_view::npos && statement.find("/*") == std::string_view::npos &&
           !startsWithKeyword(statementCode, {"if", "for", "while", "switch", "do", "try", "else", "case", "default"});
  };
  std::vector<std::string> out;
  std::vector<bool> oneLiner; // per line of out: a one-liner this pass made or found
  // Per line: whether it starts inside a function's body (code), where an `if constexpr` takes a
  // statement without braces; at declaration scope (a class's, a namespace's) its braces are required.
  std::vector<bool> inBody(lines.size(), false);
  {
    std::vector<bool> stack; // per open `{`: whether its block is code
    for (size_t i = 0; i < code.size(); ++i)
    {
      inBody[i] = !stack.empty() && stack.back();
      for (size_t j = 0; j < code[i].size(); ++j)
        if (code[i][j] == '{')
        {
          bool enclosingCode = !stack.empty() && stack.back();
          size_t head = headOf(lines, code, i, j);
          std::string headCode;
          for (size_t k = head; k <= i; ++k)
            headCode += (k > head ? " " : "") + (k == i ? code[k].substr(0, j) : code[k]);
          std::string_view h = trimmed(headCode);
          bool declarations = hasWord(h, "class") || hasWord(h, "struct") || hasWord(h, "union") || hasWord(h, "enum") ||
                              hasWord(h, "namespace") || hasWord(h, "extern") || h.empty() || startsWithKeyword(h, {"if"});
          // (an `if constexpr` block at declaration scope holds declarations; one in a body, code)
          stack.push_back(enclosingCode || !declarations);
        }
        else if (code[i][j] == '}' && !stack.empty())
          stack.pop_back();
    }
  }
  auto isOneLiner = [&](std::string_view c)
  {
    std::string_view t = trimmed(c);
    size_t open = t.find('{'), close = t.rfind(')', open);
    return endsWith(t, "}") && open != std::string_view::npos && close != std::string_view::npos && t.substr(0, 1) != "}" &&
           !startsWithKeyword(t, {"if", "for", "while", "switch", "catch", "else", "do", "class", "struct", "enum", "namespace", "union"});
  };
  for (size_t i = 0; i < lines.size(); ++i)
  {
    // A statement a macro left empty (`SAVE_FILE(...);` of a macro that is nothing, `do {} while (0)`,
    // in this configuration and every other): its `;` alone on its line goes, unless it is a statement's
    // body (`if (x)` or `else` above it).
    if (!inside[i] && trimmed(lines[i]) == ";" && trimmed(code[i]) == ";" && !out.empty())
    {
      size_t previous = out.size();
      while (previous > 0 && trimmed(out[previous - 1]).empty())
        --previous;
      std::string_view above = previous > 0 ? trimmed(out[previous - 1]) : std::string_view();
      if (!endsWith(above, ")") && !endsWith(above, "else") && !endsWith(above, "do"))
        continue;
    }
    // The configurations' `if constexpr (!config::finalRelease)` (convertMerge's, for an #if group)
    // around one short statement: without braces, the statement on the next line, indented, as
    // Factorio writes an if's one statement (kovarex 2026-11-11); not before an `else`.
    if (i > 0 && !out.empty() && inBody[i - 1] && !inside[i - 1] && trimmed(lines[i - 1]) == trimmed(code[i - 1]) &&
        trimmed(code[i - 1]).substr(0, 14) == "if constexpr (" && endsWith(trimmed(code[i - 1]), ")") && trimmed(lines[i]) == "{" &&
        i + 2 < lines.size() && trimmed(lines[i + 2]) == "}" && !inside[i + 2] && shortStatement(i + 1) &&
        !(i + 3 < lines.size() && startsWithKeyword(trimmed(code[i + 3]), {"else"})))
    {
      std::string head = out.back();
      while (!head.empty() && isspace((unsigned char)head.back()))
        head.pop_back();
      // (the statement on the next line, indented, as the game writes an if's one statement)
      std::string statement(trimmed(lines[i + 1]));
      size_t indent = head.find_first_not_of(' ');
      out.push_back(std::string(indent == std::string::npos ? 0 : indent, ' ') + "  " + statement);
      oneLiner.push_back(false);
      i += 2;
      continue;
    }
    if (i > 0 && !out.empty() && declarator(i - 1) && !inside[i])
    {
      std::string joined = out.back();
      while (!joined.empty() && isspace((unsigned char)joined.back()))
        joined.pop_back();
      // (`{}`, or `{` / one statement / `}`; a comment after the closing brace, the declaration's own
      // that resolveInlineBodies put after the body, stays after it)
      auto after = [&](size_t k, std::string_view brace) -> std::optional<std::string>
      {
        std::string_view line = trimmed(lines[k]);
        if (inside[k] || trimmed(code[k]) != brace || line.substr(0, brace.size()) != brace)
          return std::nullopt;
        std::string_view rest = trimmed(line.substr(brace.size()));
        return rest.empty() ? std::string() : " " + std::string(rest);
      };
      if (std::optional<std::string> comment = after(i, "{}"))
      {
        if (joined.size() + 3 + comment->size() <= 130)
        {
          out.back() = joined + " {}" + *comment;
          oneLiner.back() = comment->empty();
          continue;
        }
      }
      std::optional<std::string> comment;
      if (trimmed(lines[i]) == "{" && i + 2 < lines.size() && (comment = after(i + 2, "}")) && shortStatement(i + 1))
      {
        joined += " { " + std::string(trimmed(lines[i + 1])) + " }" + *comment;
        if (joined.size() <= 130)
        {
          out.back() = joined;
          oneLiner.back() = comment->empty();
          i += 2;
          continue;
        }
      }
    }
    out.push_back(lines[i]);
    oneLiner.push_back(!inside[i] && isOneLiner(code[i]) && trimmed(lines[i]) == trimmed(code[i]));
  }
  // (no empty line between two one-liners)
  std::vector<std::string> kept;
  for (size_t k = 0; k < out.size(); ++k)
  {
    if (trimmed(out[k]).empty() && !kept.empty())
    {
      size_t next = k;
      while (next < out.size() && trimmed(out[next]).empty())
        ++next;
      if (next < out.size() && oneLiner[k - 1] && oneLiner[next])
      {
        k = next - 1;
        continue;
      }
    }
    kept.push_back(out[k]);
  }
  std::string result;
  for (size_t k = 0; k < kept.size(); ++k)
    result += kept[k] + (k + 1 < kept.size() || finalNewline ? "\n" : "");
  return result;
}

// Until nothing changes: an empty line inserted after a definition can meet one already there.
std::string withStyleEmptyLines(const std::string& text)
{
  std::string result = withOneLiners(text);
  for (int round = 0; round < 4; ++round)
  {
    std::string next = withStyleEmptyLinesOnce(result);
    if (next == result)
      break;
    result = std::move(next);
  }
  return result;
}
