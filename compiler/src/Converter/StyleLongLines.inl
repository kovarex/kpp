// StyleLongLines.inl: a line over the code style's 130 characters (docs/code-style.md, "Layout") that holds several
// statements or definitions, or a list, laid out as the code style lays them out. A macro's expansion is written where
// its use stood, on one line, and an X-macro's writes its whole list there: InputAction's
// CALL_ON_EVERY_INPUT_ACTION_TYPE_WITH_DATA(...) uses were lines of up to 172,566 characters (wube/Factorio's kpp branch,
// 2026-11-16: "bad indentation and very long lines (saveUnion etc.)").
//
// Only whitespace changes, and only on such a line: its statements and definitions go one to a line at its indentation
// (`;` ends one, as does the `}` of a body when a name follows it); a definition that still does not fit is expanded,
// its head, `{` and `}` on lines of their own and its body's statements a step in; an if's, else's, for's, while's
// and switch's statement goes on the next line a step in, a block's between braces of its own (a body this pass
// expands follows the code style wholly); a case label that does not fit with its statement goes on a line of its own,
// its statement a step in, its block's braces at its column. A list (an enum's constants, a case label's values) or
// an operand chain (`1+ 1+ ... 0`) that is still too long is filled to the line's length, broken after its commas or
// operators, a case label's values lined up after `case `. A line that starts or ends inside a comment or a literal,
// opens or closes more brackets than it closes or opens, or has nothing to break at stays as it is. Run by
// withStyleEmptyLines (convertMerge's last step) before the one-liners and the empty lines, and by
// `kpp --convert-style PATH...` on a checkout's files.

namespace styleLongLines
{
  constexpr size_t maxLine = 130;

  // A part of a line: its text and its code (styleEmptyLines::codeOf's: comments and literals blanked), as long.
  struct Piece
  {
    std::string text, code;
    bool empty() const { return this->text.empty(); }
  };

  inline Piece sub(const Piece& p, size_t from, size_t to = std::string::npos)
  {
    to = std::min(to, p.text.size());
    from = std::min(from, to);
    return Piece{p.text.substr(from, to - from), p.code.substr(from, to - from)};
  }

  inline Piece trimmed(const Piece& p)
  {
    size_t b = 0, e = p.text.size();
    while (b < e && isspace((unsigned char)p.text[b]))
      ++b;
    while (e > b && isspace((unsigned char)p.text[e - 1]))
      --e;
    return sub(p, b, e);
  }

  inline bool codeless(const Piece& p) { return p.code.find_first_not_of(" \t") == std::string::npos; }

  // p without the spaces a macro's empty argument leaves in its code (`ActionData:: CloseGuiData`, `sizeof( PlaySound)`,
  // `HeapData< PlaySound>`, `const  PlaySound&`): none after `::`, `(` or a template's `<` (one with no space before
  // it), and one at most between words. Comments and literals keep theirs.
  inline Piece tidied(const Piece& p)
  {
    // Which characters are a literal's or a comment's (the code blanks them, their spaces among them).
    const std::string& t = p.text;
    std::vector<bool> literal(t.size(), false);
    for (size_t k = 0; k < t.size();)
    {
      size_t end = k;
      if (t.compare(k, 2, "//") == 0)
        end = t.size();
      else if (t.compare(k, 2, "/*") == 0)
      {
        end = t.find("*/", k + 2);
        end = end == std::string::npos ? t.size() : end + 2;
      }
      else if (t.compare(k, 2, "R\"") == 0 && (k == 0 || !isIdentChar(t[k - 1]) || t[k - 1] == '8' || t[k - 1] == 'u' || t[k - 1] == 'U' || t[k - 1] == 'L'))
      {
        size_t open = t.find('(', k + 2);
        std::string close = open == std::string::npos ? std::string() : ")" + t.substr(k + 2, open - k - 2) + "\"";
        end = open == std::string::npos ? t.size() : t.find(close, open);
        end = end == std::string::npos ? t.size() : end + close.size();
      }
      else if (t[k] == '"' || (t[k] == '\'' && !(k > 0 && isalnum((unsigned char)t[k - 1]))))
      {
        end = k + 1;
        while (end < t.size() && t[end] != t[k])
          end += t[end] == '\\' ? 2 : 1;
        end = std::min(end + 1, t.size());
      }
      if (end == k)
      {
        ++k;
        continue;
      }
      for (; k < end; ++k)
        literal[k] = true;
    }
    Piece out;
    bool lastLiteral = false; // whether the last character kept is a literal's or a comment's
    for (size_t k = 0; k < p.code.size(); ++k)
    {
      if (p.code[k] == ' ' && !literal[k] && !out.code.empty() && !lastLiteral)
      {
        const std::string& c = out.code;
        char last = c.back();
        bool afterScope = c.size() >= 2 && last == ':' && c[c.size() - 2] == ':';
        bool afterTemplate = last == '<' && c.size() >= 2 && isIdentChar(c[c.size() - 2]);
        if (last == ' ' || last == '(' || afterScope || afterTemplate)
          continue;
      }
      out.text += p.text[k];
      out.code += p.code[k];
      lastLiteral = literal[k];
    }
    return out;
  }

  inline bool wordAt(const std::string& s, size_t at, std::string_view w)
  {
    return s.compare(at, w.size(), w) == 0 && (at == 0 || !isIdentChar(s[at - 1])) && (at + w.size() >= s.size() || !isIdentChar(s[at + w.size()]));
  }

  inline size_t skipSpaces(const std::string& s, size_t at)
  {
    while (at < s.size() && (s[at] == ' ' || s[at] == '\t'))
      ++at;
    return at;
  }

  // The bracket closing the one at open, or npos.
  inline size_t matching(const std::string& code, size_t open)
  {
    int depth = 0;
    for (size_t k = open; k < code.size(); ++k)
      if (code[k] == '(' || code[k] == '[' || code[k] == '{')
        ++depth;
      else if ((code[k] == ')' || code[k] == ']' || code[k] == '}') && --depth == 0)
        return k;
    return std::string::npos;
  }

  // The statements and definitions of p, one after another at its bracket depth 0 (none when its brackets do not
  // balance): each ends with a `;`, or with a `}` that a name follows (the next definition, not `else`, `while` or
  // `catch`, nor a declarator: `} name;`). A comment after one goes with it, as does an empty statement.
  inline std::optional<std::vector<Piece>> statements(const Piece& p)
  {
    std::vector<Piece> out;
    int depth = 0;
    size_t start = 0;
    auto cut = [&](size_t end)
    {
      Piece u = trimmed(sub(p, start, end));
      start = end;
      if (u.empty())
        return;
      // (a comment after the last, or an empty statement after one, `f();;`, stays on its line)
      if ((codeless(u) || u.code == ";") && !out.empty())
      {
        std::string space = u.code == ";" ? "" : " ";
        out.back().text += space + u.text;
        out.back().code += space + u.code;
        return;
      }
      out.push_back(u);
    };
    for (size_t k = 0; k < p.code.size(); ++k)
    {
      char c = p.code[k];
      if (c == '(' || c == '[' || c == '{')
        ++depth;
      else if (c == ')' || c == ']' || c == '}')
      {
        if (--depth < 0)
          return std::nullopt;
        if (c != '}' || depth != 0)
          continue;
        size_t next = skipSpaces(p.code, k + 1);
        if (next >= p.code.size() || !(isIdentChar(p.code[next]) || p.code[next] == '~') || isdigit((unsigned char)p.code[next]))
          continue;
        if (wordAt(p.code, next, "else") || wordAt(p.code, next, "while") || wordAt(p.code, next, "catch"))
          continue;
        size_t after = next;
        while (after < p.code.size() && isIdentChar(p.code[after]))
          ++after;
        after = skipSpaces(p.code, after);
        if (after < p.code.size() && strchr(";,=[", p.code[after]))
          continue;
        cut(k + 1);
      }
      else if (c == ';' && depth == 0)
        cut(k + 1);
    }
    if (depth != 0)
      return std::nullopt;
    cut(p.code.size());
    return out;
  }

  // The `:` ending a case or default label at the start of p (not a `::`), or npos.
  inline size_t labelEnd(const Piece& p)
  {
    if (!wordAt(p.code, 0, "case") && !wordAt(p.code, 0, "default"))
      return std::string::npos;
    int depth = 0;
    for (size_t k = 0; k < p.code.size(); ++k)
    {
      char c = p.code[k];
      if (c == '(' || c == '[' || c == '{')
        ++depth;
      else if (c == ')' || c == ']' || c == '}')
        --depth;
      else if (c == ':' && depth == 0)
      {
        if (k + 1 < p.code.size() && p.code[k + 1] == ':')
        {
          ++k;
          continue;
        }
        return k;
      }
    }
    return std::string::npos;
  }

  // The first `{` at depth 0 of p that opens a body (a function's, a class's, a compound statement's), not a braced
  // list (`= {`, `T{`, `return {`), or npos.
  inline size_t bodyBrace(const Piece& p)
  {
    int depth = 0;
    for (size_t k = 0; k < p.code.size(); ++k)
    {
      char c = p.code[k];
      if (c == '(' || c == '[')
        ++depth;
      else if (c == ')' || c == ']')
        --depth;
      else if (c == '{' && depth == 0)
      {
        size_t before = p.code.find_last_not_of(" \t", k == 0 ? std::string::npos : k - 1);
        if (k == 0 || before == std::string::npos)
          return k;
        char b = p.code[before];
        if (b == ')')
          return k;
        if (!isIdentChar(b))
          return std::string::npos;
        size_t wordStart = before + 1;
        while (wordStart > 0 && isIdentChar(p.code[wordStart - 1]))
          --wordStart;
        std::string word = p.code.substr(wordStart, before + 1 - wordStart);
        for (std::string_view w : {"const", "noexcept", "override", "final", "mutable", "else", "do", "try"})
          if (word == w)
            return k;
        for (std::string_view w : {"class", "struct", "union", "enum", "namespace"})
          for (size_t at = p.code.find(w); at != std::string::npos && at < k; at = p.code.find(w, at + 1))
            if (wordAt(p.code, at, w))
              return k;
        return std::string::npos;
      }
      else if (c == '{' || c == '}')
        depth += c == '{' ? 1 : -1;
    }
    return std::string::npos;
  }

  // p filled into lines of at most maxLine, broken after the places breakAfter allows (a space follows each), the
  // first line at indent, the rest at continuation; false when nothing can be broken.
  template<typename BreakAfter>
  bool filled(const Piece& p, size_t indent, size_t continuation, BreakAfter breakAfter, std::vector<std::string>& lines)
  {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t k = 0; k + 1 < p.code.size(); ++k)
      if (p.code[k + 1] == ' ' && breakAfter(k))
      {
        parts.push_back(p.text.substr(start, k + 1 - start));
        start = skipSpaces(p.text, k + 1);
        k = start - 1;
      }
    parts.push_back(p.text.substr(start));
    if (parts.size() < 2)
      return false;
    std::string line = std::string(indent, ' ') + parts[0];
    for (size_t k = 1; k < parts.size(); ++k)
      if (line.size() + 1 + parts[k].size() > maxLine)
      {
        lines.push_back(line);
        line = std::string(continuation, ' ') + parts[k];
      }
      else
        line += " " + parts[k];
    lines.push_back(line);
    return true;
  }

  // The bracket depth at each position of p.
  inline std::vector<int> depths(const Piece& p)
  {
    std::vector<int> d(p.code.size());
    int depth = 0;
    for (size_t k = 0; k < p.code.size(); ++k)
    {
      char c = p.code[k];
      if (c == ')' || c == ']' || c == '}')
        --depth;
      d[k] = depth;
      if (c == '(' || c == '[' || c == '{')
        ++depth;
    }
    return d;
  }

  bool sequence(const Piece& p, size_t indent, std::vector<std::string>& lines);
  bool statement(const Piece& u, size_t indent, std::vector<std::string>& lines);

  // A case label, its values filled and lined up after `case ` when they do not fit.
  inline void label(const Piece& l, size_t indent, std::vector<std::string>& lines)
  {
    if (indent + l.text.size() <= maxLine)
    {
      lines.push_back(std::string(indent, ' ') + l.text);
      return;
    }
    std::vector<int> d = depths(l);
    if (!filled(l, indent, indent + 5, [&](size_t k) { return l.code[k] == ',' && d[k] == 0; }, lines))
      lines.push_back(std::string(indent, ' ') + l.text);
  }

  // A body, `{ ... }`, at the start of b, its braces at indent and its statements a step in, then what follows it.
  inline bool block(const Piece& b, size_t indent, std::vector<std::string>& lines)
  {
    size_t close = matching(b.code, 0);
    if (close == std::string::npos)
      return false;
    Piece inner = trimmed(sub(b, 1, close)), rest = trimmed(sub(b, close + 1));
    std::string pad(indent, ' ');
    if (inner.empty())
      lines.push_back(pad + "{}");
    else
    {
      lines.push_back(pad + "{");
      if (!sequence(inner, indent + 2, lines))
        return false;
      lines.push_back(pad + "}");
    }
    if (rest.empty())
      return true;
    if (rest.code[0] == ';' || rest.code[0] == ',')
    {
      Piece more = trimmed(sub(rest, 1));
      lines.back() += rest.text[0];
      return more.empty() || sequence(more, indent, lines);
    }
    return sequence(rest, indent, lines);
  }

  // One statement or definition at indent.
  inline bool statement(const Piece& u, size_t indent, std::vector<std::string>& lines)
  {
    std::string pad(indent, ' ');
    // A block comment before it (a macro's `else /* why */ return ...`): on a line of its own above it.
    if (u.text.compare(0, 2, "/*") == 0)
    {
      size_t codeStart = u.code.find_first_not_of(" \t");
      if (codeStart != std::string::npos)
      {
        lines.push_back(pad + trimmed(sub(u, 0, codeStart)).text);
        return statement(trimmed(sub(u, codeStart)), indent, lines);
      }
    }
    bool fits = indent + u.text.size() <= maxLine;
    auto startsControl = [](const Piece& p)
    {
      for (std::string_view w : {"if", "else", "for", "while", "switch", "do", "try"})
        if (wordAt(p.code, 0, w))
          return true;
      return false;
    };
    // A case label: with its statement when that fits and is no block or control statement.
    if (size_t colon = labelEnd(u); colon != std::string::npos)
    {
      Piece l = trimmed(sub(u, 0, colon + 1)), rest = trimmed(sub(u, colon + 1));
      if (rest.empty() || (fits && rest.code[0] != '{' && !startsControl(rest)))
      {
        if (rest.empty())
          label(l, indent, lines);
        else
          lines.push_back(pad + u.text);
        return true;
      }
      label(l, indent, lines);
      if (rest.code[0] == '{')
        return block(rest, indent, lines);
      return sequence(rest, indent + 2, lines);
    }
    // if, for, while, switch: the head, then the statement a step in or the block.
    for (std::string_view w : {"if", "for", "while", "switch"})
      if (wordAt(u.code, 0, w))
      {
        size_t open = skipSpaces(u.code, w.size());
        if (wordAt(u.code, open, "constexpr"))
          open = skipSpaces(u.code, open + 9);
        if (open >= u.code.size() || u.code[open] != '(')
          break;
        size_t close = matching(u.code, open);
        if (close == std::string::npos)
          return false;
        Piece body = trimmed(sub(u, close + 1));
        if (body.empty() || body.code[0] == ';')
        {
          lines.push_back(pad + u.text);
          return true;
        }
        lines.push_back(pad + trimmed(sub(u, 0, close + 1)).text);
        if (body.code[0] == '{')
          return block(body, indent, lines);
        return statement(body, indent + 2, lines);
      }
    if (wordAt(u.code, 0, "else"))
    {
      Piece rest = trimmed(sub(u, 4));
      if (wordAt(rest.code, 0, "if"))
      {
        size_t first = lines.size();
        if (!statement(rest, indent, lines))
          return false;
        lines[first].insert(indent, "else ");
        return true;
      }
      lines.push_back(pad + "else");
      if (!rest.empty() && rest.code[0] == '{')
        return block(rest, indent, lines);
      return rest.empty() || statement(rest, indent + 2, lines);
    }
    // A definition with a body: whole when it fits (a one-liner), else expanded.
    if (size_t brace = bodyBrace(u); brace != std::string::npos && !wordAt(u.code, 0, "do") && !wordAt(u.code, 0, "try"))
    {
      if (fits)
      {
        lines.push_back(pad + u.text);
        return true;
      }
      Piece head = trimmed(sub(u, 0, brace));
      if (!head.empty())
        lines.push_back(pad + head.text);
      return block(sub(u, brace), indent, lines);
    }
    if (fits)
    {
      lines.push_back(pad + u.text);
      return true;
    }
    // A list (an enum's constants) or an operand chain, filled; anything else stays as it is.
    std::vector<int> d = depths(u);
    bool list = u.code.find_first_of(";(<=") == std::string::npos;
    if (list && filled(u, indent, indent, [&](size_t k) { return d[k] == 0 && (u.code[k] == ',' || strchr("+-*/|&", u.code[k])); }, lines))
      return true;
    lines.push_back(pad + u.text);
    return true;
  }

  inline bool sequence(const Piece& p, size_t indent, std::vector<std::string>& lines)
  {
    std::optional<std::vector<Piece>> units = statements(p);
    if (!units)
      return false;
    for (const Piece& u : *units)
      if (!statement(u, indent, lines))
        return false;
    return true;
  }
}

namespace styleLongLines
{
  // Whether a long line is one this pass lays out: several statements or definitions, a case label alone (its
  // values), or a list. A single statement or definition the C++ wrote long stays as it was written.
  inline bool several(const Piece& p)
  {
    std::optional<std::vector<Piece>> units = statements(p);
    if (!units || units->empty())
      return false;
    if (units->size() > 1)
      return true;
    const Piece& u = units->front();
    size_t colon = labelEnd(u);
    if (colon != std::string::npos)
      return trimmed(sub(u, colon + 1)).empty();
    return u.code.find_first_of(";(<={}") == std::string::npos;
  }
}

inline std::string withLongLinesSplit(const std::string& text)
{
  using namespace styleLongLines;
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
  styleEmptyLines::codeOf(lines, code, inside);
  std::vector<std::string> out;
  std::string previous;       // the last line with code before line i, trimmed
  size_t previousIndent = 0;
  for (size_t i = 0; i < lines.size(); ++i)
  {
    const std::string& line = lines[i];
    bool cr = !line.empty() && line.back() == '\r';
    size_t length = line.size() - (cr ? 1 : 0);
    size_t indent = line.find_first_not_of(' ');
    bool candidate = length > maxLine && !inside[i] && !(i + 1 < lines.size() && inside[i + 1]) && indent != std::string::npos &&
                     line[indent] != '#' && line.find('\t') == std::string::npos;
    // A line that starts a statement takes the previous statement's indentation (a step in after a `{` or a case
    // label) where that is less than its own: the C++ indented a macro's use as it liked (InputAction's named
    // constructors stood a step too far in, its out-of-class constructors a step in at namespace scope). A continued
    // line keeps its own, as does one after a braceless body, or one third-party code indents its way (zstd's
    // `{   U32 n;` blocks).
    size_t writtenIndent = indent;
    if (candidate && !previous.empty())
    {
      char last = previous.back();
      bool label = last == ':' && (previous.compare(0, 5, "case ") == 0 || previous.compare(0, 7, "default") == 0);
      size_t wanted = indent;
      if (label || last == '{')
        wanted = previousIndent + 2;
      else if (last == ';' || last == '}')
        wanted = previousIndent;
      writtenIndent = std::min(indent, wanted);
    }
    else if (candidate)
      writtenIndent = 0;
    if (!inside[i])
    {
      std::string_view c = styleEmptyLines::trimmed(code[i]);
      if (!c.empty())
      {
        previous = std::string(c);
        previousIndent = indent == std::string::npos ? 0 : indent;
      }
    }
    std::vector<std::string> written;
    Piece whole = candidate ? tidied(trimmed(Piece{line.substr(0, length), code[i].substr(0, length)})) : Piece();
    if (candidate && several(whole) && sequence(whole, writtenIndent, written) && written.size() > 1)
    {
      for (std::string& w : written)
      {
        while (!w.empty() && w.back() == ' ')
          w.pop_back();
        out.push_back(w + (cr ? "\r" : ""));
      }
      continue;
    }
    out.push_back(line);
  }
  std::string result;
  for (size_t k = 0; k < out.size(); ++k)
    result += out[k] + (k + 1 < out.size() || finalNewline ? "\n" : "");
  return result;
}
