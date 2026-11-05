// NamespaceBlocks.inl: namespace blocks and the output text's tidying.
// Part of convert.cpp, included where it stood (a file of its own since 2026-11-05, moved without a change).

// The namespaces of a kpp text, each by its path (`agui`, `agui::detail`), with the place each
// opens at and closes at; `holds` says whether one holds anything besides whitespace, comments and
// other namespaces. A qualified `namespace a::b {` is a and a::b. An anonymous one's path is "".
struct NamespaceBlock
{
  std::string path;
  size_t begin, open, close; // `namespace`, its `{` and its `}`
  bool holds = false;
};
std::vector<NamespaceBlock> namespaceBlocks(const std::string& text)
{
  std::vector<NamespaceBlock> blocks;
  struct Open
  {
    std::vector<size_t> blocks; // the namespace blocks this brace opened (two for `a::b`), or none
  };
  std::vector<Open> braces;
  std::vector<std::string> paths; // the namespaces open here, innermost last
  auto holdHere = [&]()
  {
    for (Open& o : braces)
      for (size_t b : o.blocks)
        blocks[b].holds = true;
  };
  for (size_t i = 0; i < text.size();)
  {
    char c = text[i];
    if (size_t raw = rawStringEnd(text, i); raw != std::string_view::npos)
    {
      holdHere();
      i = raw;
      continue;
    }
    if (c == '"' || (c == '\'' && !isDigitSeparator(text, i)))
    {
      holdHere();
      for (++i; i < text.size() && text[i] != c && text[i] != '\n'; ++i)
        i += text[i] == '\\';
      ++i;
      continue;
    }
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
    if (isspace((unsigned char)c))
    {
      ++i;
      continue;
    }
    if (c == '}')
    {
      if (!braces.empty())
      {
        for (size_t b : braces.back().blocks)
        {
          blocks[b].close = i;
          paths.pop_back();
        }
        braces.pop_back();
      }
      ++i;
      continue;
    }
    if (text.compare(i, 9, "namespace") == 0 && (i == 0 || !isIdentChar(text[i - 1])) && i + 9 < text.size() && !isIdentChar(text[i + 9]))
    {
      // `namespace NAME {`, `namespace a::b {`, `namespace {` (not `using namespace` or an alias)
      size_t q = i + 9;
      while (q < text.size() && isspace((unsigned char)text[q]))
        ++q;
      size_t nameStart = q;
      while (q < text.size() && (isIdentChar(text[q]) || text[q] == ':'))
        ++q;
      std::string name = text.substr(nameStart, q - nameStart);
      while (q < text.size() && isspace((unsigned char)text[q]))
        ++q;
      bool usingBefore = false;
      {
        size_t b = text.find_last_not_of(" \t\r\n", i == 0 ? 0 : i - 1);
        usingBefore = i > 0 && b != std::string::npos && b >= 4 && text.compare(b - 4, 5, "using") == 0;
      }
      if (q < text.size() && text[q] == '{' && !usingBefore)
      {
        // (a namespace inside another is no holding of the outer one: its contents are)
        Open o;
        std::string path = paths.empty() ? std::string() : paths.back();
        for (size_t start = 0; start <= name.size();)
        {
          size_t sep = name.find("::", start);
          std::string part = name.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
          path = path.empty() ? part : path + "::" + part;
          if (name.empty())
            path = paths.empty() ? std::string("") : paths.back() + "::";
          o.blocks.push_back(blocks.size());
          blocks.push_back({path, i, q, std::string::npos});
          paths.push_back(path);
          if (sep == std::string::npos)
            break;
          start = sep + 2;
        }
        braces.push_back(o);
        i = q + 1;
        continue;
      }
    }
    if (c == '{')
    {
      holdHere();
      braces.push_back({});
      ++i;
      continue;
    }
    holdHere();
    if (isIdentChar(c))
      while (i < text.size() && isIdentChar(text[i]))
        ++i;
    else
      ++i;
  }
  return blocks;
}

// The namespaces a text holds something in (namespaceBlocks), the program's by all its files.
void noteNamespacesHolding(const std::string& text, std::set<std::string>& holding)
{
  for (const NamespaceBlock& b : namespaceBlocks(text))
    if (b.holds)
      holding.insert(b.path);
}

// The text without the namespaces that hold nothing (`namespace agui { }`, on one line or on
// several), which the removals leave (forward declarations, mostly); an outer one an inner one's
// going empties goes too. One that is the program's only declaration of its namespace stays:
// `void GuiUtil::f() { g(); }` finds g only through a declared GuiUtil (`holding`: the namespaces
// some file of the program holds something in). One holding a comment stays too: the comment says
// something (LuaHelper's `//?` API documentation).
std::string withoutEmptyNamespaces(std::string text, const std::set<std::string>& holding, size_t& removed, size_t& kept)
{
  for (bool again = true; again;)
  {
    again = false;
    std::vector<NamespaceBlock> blocks = namespaceBlocks(text);
    // innermost (last opened among nested) first: from the end
    for (size_t k = blocks.size(); k-- > 0;)
    {
      const NamespaceBlock& b = blocks[k];
      if (b.holds || b.close == std::string::npos || text.find_first_not_of(" \t\r\n", b.open + 1) != b.close)
        continue;
      if (!b.path.empty() && b.path.back() != ':' && !holding.count(b.path))
      {
        ++kept;
        continue;
      }
      // From its line's start, when only indentation is before it, to its closing line's end, when
      // nothing follows the brace there.
      size_t from = b.begin;
      while (from > 0 && (text[from - 1] == ' ' || text[from - 1] == '\t'))
        --from;
      bool ownLine = from == 0 || text[from - 1] == '\n';
      if (!ownLine)
        from = b.begin;
      size_t to = b.close + 1, after = to;
      while (after < text.size() && (text[after] == ' ' || text[after] == '\t' || text[after] == '\r'))
        ++after;
      if (ownLine && (after >= text.size() || text[after] == '\n'))
        to = std::min(after + 1, text.size());
      // (a qualified `namespace a::b { }`, two blocks at one place, stays: none in the game)
      if ((k > 0 && blocks[k - 1].begin == b.begin) || (k + 1 < blocks.size() && blocks[k + 1].begin == b.begin))
        continue;
      text.erase(from, to - from);
      ++removed;
      again = true;
      break;
    }
  }
  return text;
}

// A converted file's text with its blank lines tidied, as the removals (includes, forward
// declarations, imports, empty namespaces) leave them: a run of blank lines (whitespace only) is
// one, none at the start of the file, after a line ending with `{` or before one starting with `}`,
// and the file ends with one line end. A blank line inside a raw string literal is the string's
// text, and stays. `removed` counts the blank lines taken out.
// A converted file's text as it is written: every line ends with LF alone. A source whose lines end
// with CRLF (C:\Factorio's working tree, checked out with core.autocrlf; a library's file kept so in
// its repository) gives the pieces copied from it their CRs, and a file of them and the converter's
// own lines had both. C++ reads a CRLF as one new-line (translation phase 1), so a raw string literal
// over such lines means the same with LF.
std::string withLfLineEnds(const std::string& text)
{
  if (text.find('\r') == std::string::npos)
    return text;
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i)
    if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n'))
      out += text[i];
  return out;
}

// Writes a converted file (withLfLineEnds); false when it cannot be opened.
bool writeConvertedFile(const std::filesystem::path& path, const std::string& text, const char* mode = "wb")
{
  FILE* f = fopen(path.string().c_str(), mode);
  if (!f)
    return false;
  std::string lf = withLfLineEnds(text);
  fwrite(lf.data(), 1, lf.size(), f);
  fclose(f);
  return true;
}

std::string withTidyBlankLines(const std::string& text, size_t& removed)
{
  // Which line ends are inside a raw string literal.
  std::vector<bool> inRaw(text.size() + 1, false);
  for (size_t i = 0; i < text.size();)
  {
    if (size_t raw = rawStringEnd(text, i); raw != std::string_view::npos)
    {
      for (size_t k = i; k < raw; ++k)
        inRaw[k] = true;
      i = raw;
    }
    else if (text[i] == '"' || (text[i] == '\'' && !isDigitSeparator(text, i)))
    {
      char quote = text[i];
      for (++i; i < text.size() && text[i] != quote && text[i] != '\n'; ++i)
        i += text[i] == '\\';
      ++i;
    }
    else if (text.compare(i, 2, "//") == 0)
      i = std::min(text.find('\n', i), text.size());
    else if (text.compare(i, 2, "/*") == 0)
    {
      size_t e = text.find("*/", i + 2);
      i = e == std::string::npos ? text.size() : e + 2;
    }
    else
      ++i;
  }
  struct Line
  {
    std::string_view text;
    bool blank, raw; // raw: it starts inside a raw string literal
  };
  std::vector<Line> lines;
  for (size_t start = 0; start < text.size();)
  {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    std::string_view line(text.data() + start, end - start);
    lines.push_back({line, line.find_first_not_of(" \t\r") == std::string_view::npos, start > 0 && inRaw[start - 1]});
    start = end + 1;
  }
  auto lastChar = [](std::string_view line)
  {
    size_t last = line.find_last_not_of(" \t\r");
    return last == std::string_view::npos ? '\0' : line[last];
  };
  auto firstChar = [](std::string_view line)
  {
    size_t first = line.find_first_not_of(" \t\r");
    return first == std::string_view::npos ? '\0' : line[first];
  };
  std::string out;
  out.reserve(text.size());
  const Line* previous = nullptr; // the last line kept that is not blank
  bool pendingBlank = false;
  for (size_t k = 0; k < lines.size(); ++k)
  {
    const Line& line = lines[k];
    if (line.raw)
    {
      out.append(line.text);
      out += '\n';
      continue;
    }
    if (line.blank)
    {
      if (pendingBlank || !previous || lastChar(previous->text) == '{')
        ++removed;
      pendingBlank = previous && lastChar(previous->text) != '{';
      continue;
    }
    if (pendingBlank)
    {
      if (firstChar(line.text) == '}')
        ++removed;
      else
        out += '\n';
      pendingBlank = false;
    }
    out.append(line.text);
    out += '\n';
    previous = &line;
  }
  // (a blank line at the end, pending, is dropped: the file ends with its last line's end)
  removed += pendingBlank;
  return out;
}

// The identifiers of a kpp text, outside comments and literals.
std::set<std::string> identifiersOf(const std::string& text)
{
  std::set<std::string> out;
  for (size_t i = 0; i < text.size();)
  {
    if (size_t raw = rawStringEnd(text, i); raw != std::string_view::npos)
      i = raw;
    else if (text[i] == '"' || (text[i] == '\'' && !isDigitSeparator(text, i)))
    {
      char quote = text[i];
      for (++i; i < text.size() && text[i] != quote && text[i] != '\n'; ++i)
        i += text[i] == '\\';
      ++i;
    }
    else if (text.compare(i, 2, "//") == 0)
      i = std::min(text.find('\n', i), text.size());
    else if (text.compare(i, 2, "/*") == 0)
      i = std::min(text.find("*/", i + 2), text.size() - 2) + 2;
    else if (isalpha((unsigned char)text[i]) || text[i] == '_')
    {
      size_t e = i;
      while (e < text.size() && isIdentChar(text[e]))
        ++e;
      // (a member's name is no C function's: `widgets.remove(w)` is no stdio.h)
      size_t before = text.find_last_not_of(" \t\r\n", i == 0 ? 0 : i - 1);
      bool member = i > 0 && before != std::string::npos && (text[before] == '.' || (text[before] == '>' && before > 0 && text[before - 1] == '-'));
      if (!member)
        out.insert(text.substr(i, e - i));
      i = e;
    }
    else if (isdigit((unsigned char)text[i]))
      while (i < text.size() && (isIdentChar(text[i]) || text[i] == '.'))
        ++i;
    else
      ++i;
  }
  return out;
}

