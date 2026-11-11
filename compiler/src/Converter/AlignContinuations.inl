// AlignContinuations.inl: continuation lines keep their alignment when a rewrite changes the length of the text before
// what they align to (kovarex 2026-11-10: `std::unique_ptr<Scenario> scenario(Scenario::loadFactory(MapInterface(),`
// is `std::UniquePtr<...>`, one shorter, and the argument lines under `MapInterface()` stayed a column off).
// Part of convert.cpp, included where it stands in its anonymous namespace.
//
// The rule (kovarex): a line moves only when, before the rewrite, its first character stood exactly at the column of a
// token on an earlier line of the same open parenthesis or bracket, or of a brace that is a braced list; it moves by
// what the rewrite changed on that line before that token (and by what that line itself moved). A block's lines follow
// the block, never an alignment: the body of an `if` whose condition got longer, a loop's, a function's, a lambda's.

namespace alignContinuations
{
  struct Token
  {
    uint32_t offset;
    uint32_t column;
  };
  struct Group
  {
    char open;          // '(', '[', or '{' of a braced list
    bool block;         // a `{` that opens a block or a class body: its lines follow its indentation
    size_t firstLine;   // the line of the bracket
  };

  // Whether the `{` at offset i opens a braced list (`= {`, `f({`, `{a, {`, `return {`, `Type{`): after `=`, `(`,
  // `,`, `{`, `[`, `return`, or a name or `>` on its own line (`x{`); anything else (after `)`, `const`, a class head
  // on the lines above, a line of its own) opens a block.
  inline bool bracedList(const std::string& text, size_t i, size_t lineStart)
  {
    size_t p = i;
    while (p > lineStart && (text[p - 1] == ' ' || text[p - 1] == '\t'))
      --p;
    if (p == lineStart)
      return false;
    char c = text[p - 1];
    if (c == '=' || c == '(' || c == ',' || c == '{' || c == '[')
      return true;
    if (p >= lineStart + 6 && text.compare(p - 6, 6, "return") == 0 && (p - 6 == lineStart || !isIdentChar(text[p - 7])))
      return true;
    if (isIdentChar(c) || c == '>')
    {
      // (a name before it: a list, `Type{a, b}`, unless the name is a class head's or a function's qualifier)
      size_t w = p;
      while (w > lineStart && isIdentChar(text[w - 1]))
        --w;
      std::string word = text.substr(w, p - w);
      static const std::set<std::string> heads = {"const", "noexcept", "override", "final", "mutable", "try", "else", "do", "class", "struct", "union",
                                                  "enum", "namespace", "public", "private", "protected", "volatile", "constexpr", "consteval"};
      if (heads.count(word))
        return false;
      // (the class head before it on this line, `class X : public Y {`, `struct Z {`)
      std::string line = text.substr(lineStart, p - lineStart);
      for (const char* head : {"class ", "struct ", "union ", "enum ", "namespace "})
        if (line.find(head) != std::string::npos)
          return false;
      return true;
    }
    return false;
  }
}

// The edits that move the continuation lines of text, as sorted (by offset, the longest first at one offset) rewrites
// it, by alignContinuations' rule: insertions or removals of spaces at the lines' starts, to be added to the edits.
inline std::vector<Edit> alignmentEdits(const std::string& text, const std::vector<Edit>& sorted)
{
  using namespace alignContinuations;
  std::vector<Edit> out;
  if (sorted.empty())
    return out;
  std::vector<uint32_t> lineStarts{0};
  for (uint32_t i = 0; i < text.size(); ++i)
    if (text[i] == '\n')
      lineStarts.push_back(i + 1);
  // The tokens of each line (their starts), and per line the innermost open group when the line starts.
  std::vector<std::vector<Token>> tokens(lineStarts.size());
  std::vector<int> groupAtStart(lineStarts.size(), -1);
  std::vector<Group> groups;
  std::vector<int> stack;
  size_t line = 0;
  auto lineOf = [&](size_t offset)
  {
    while (line + 1 < lineStarts.size() && lineStarts[line + 1] <= offset)
    {
      ++line;
      groupAtStart[line] = stack.empty() ? -1 : stack.back();
    }
    return line;
  };
  bool lineStartOfText = true;
  for (size_t i = 0; i < text.size();)
  {
    char c = text[i];
    size_t l = lineOf(i);
    if (c == '\n')
    {
      lineStartOfText = true;
      ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r')
    {
      ++i;
      continue;
    }
    if (lineStartOfText && c == '#')
    {
      // (a preprocessor line, a backslash continuing it: no tokens)
      while (i < text.size() && !(text[i] == '\n' && text[i - 1] != '\\'))
        ++i;
      continue;
    }
    lineStartOfText = false;
    // (a comment is a token here: a line of comment among the arguments moves with them)
    if (text.compare(i, 2, "//") == 0)
    {
      tokens[l].push_back({uint32_t(i), uint32_t(i - lineStarts[l])});
      size_t e = text.find('\n', i);
      i = e == std::string::npos ? text.size() : e;
      continue;
    }
    if (text.compare(i, 2, "/*") == 0)
    {
      tokens[l].push_back({uint32_t(i), uint32_t(i - lineStarts[l])});
      size_t e = text.find("*/", i + 2);
      i = e == std::string::npos ? text.size() : e + 2;
      continue;
    }
    tokens[l].push_back({uint32_t(i), uint32_t(i - lineStarts[l])});
    if (size_t raw = rawStringEnd(text, i); raw != std::string_view::npos)
    {
      i = raw;
      continue;
    }
    if (c == '"' || (c == '\'' && !isDigitSeparator(text, i)))
    {
      size_t e = i + 1;
      for (; e < text.size() && text[e] != c && text[e] != '\n'; ++e)
        e += text[e] == '\\';
      i = std::min(e + 1, text.size());
      continue;
    }
    if (isIdentChar(c))
    {
      while (i < text.size() && (isIdentChar(text[i]) || (text[i] == '\'' && isDigitSeparator(text, i))))
        ++i;
      continue;
    }
    if (c == '(' || c == '[' || c == '{')
    {
      bool block = c == '{' && !bracedList(text, i, lineStarts[l]);
      groups.push_back({c, block, l});
      stack.push_back(int(groups.size() - 1));
    }
    else if ((c == ')' || c == ']' || c == '}') && !stack.empty())
      stack.pop_back();
    ++i;
  }
  lineOf(text.size());
  // What the edits change on a line before an offset in it, as applyEdits applies them: nullopt when an edit spans the
  // offset, writes a line break before it, or starts before the line and ends in it.
  auto delta = [&](uint32_t lineStart, uint32_t anchor) -> std::optional<int64_t>
  {
    auto it = std::lower_bound(sorted.begin(), sorted.end(), lineStart, [](const Edit& e, uint32_t o) { return e.offset < o; });
    // (an edit from before the line that reaches into it: its text and the line's start are another's)
    for (auto back = it; back != sorted.begin();)
    {
      --back;
      if (back->offset + back->length > lineStart)
        return std::nullopt;
      if (back->offset + 4096 < lineStart)
        break;
    }
    int64_t d = 0;
    uint32_t pos = lineStart;
    for (; it != sorted.end() && it->offset <= anchor; ++it)
    {
      const Edit& e = *it;
      if (e.offset == anchor && e.length > 0)
        break; // (the token itself replaced: its start stays)
      if (e.offset < pos)
      {
        if (e.length && e.offset + e.length > pos)
          return std::nullopt;
        continue;
      }
      if (e.offset + e.length > anchor || e.text.find('\n') != std::string::npos)
        return std::nullopt;
      d += int64_t(e.text.size()) - int64_t(e.length);
      pos = e.offset + e.length;
    }
    return d;
  };
  // Whether an edit touches the indentation of the line (its start to its first character).
  auto indentationEdited = [&](uint32_t lineStart, uint32_t first)
  {
    auto it = std::lower_bound(sorted.begin(), sorted.end(), lineStart, [](const Edit& e, uint32_t o) { return e.offset < o; });
    if (it != sorted.end() && it->offset <= first)
      return true;
    for (auto back = it; back != sorted.begin();)
    {
      --back;
      if (back->offset + back->length > lineStart)
        return true;
      if (back->offset + 4096 < lineStart)
        break;
    }
    return false;
  };
  std::vector<int64_t> moved(lineStarts.size(), 0);
  for (size_t j = 1; j < lineStarts.size(); ++j)
  {
    int g = groupAtStart[j];
    if (g < 0 || groups[size_t(g)].block || tokens[j].empty())
      continue;
    uint32_t first = tokens[j].front().offset;
    uint32_t column = first - lineStarts[j];
    // (the nearest earlier line of the group with a token at that column)
    std::optional<std::pair<size_t, uint32_t>> anchor;
    for (size_t k = j; k-- > groups[size_t(g)].firstLine && !anchor;)
      for (const Token& t : tokens[k])
        if (t.column == column)
        {
          anchor = std::pair(k, t.offset);
          break;
        }
    if (!anchor)
      continue;
    // (a token at the line's own start on the line above, the same indentation: no alignment to keep)
    if (tokens[anchor->first].front().offset == anchor->second && anchor->first != groups[size_t(g)].firstLine)
    {
      moved[j] = moved[anchor->first];
      if (moved[j] == 0)
        continue;
    }
    else
    {
      std::optional<int64_t> d = delta(lineStarts[anchor->first], anchor->second);
      if (!d)
        continue;
      moved[j] = moved[anchor->first] + *d;
    }
    if (moved[j] == 0 || indentationEdited(lineStarts[j], first) || int64_t(column) + moved[j] < 0)
    {
      moved[j] = 0;
      continue;
    }
    if (moved[j] > 0)
      out.push_back({lineStarts[j], 0, std::string(size_t(moved[j]), ' ')});
    else
    {
      uint32_t spaces = 0;
      while (spaces < uint32_t(-moved[j]) && text[lineStarts[j] + spaces] == ' ')
        ++spaces;
      if (spaces < uint32_t(-moved[j]))
      {
        moved[j] = 0;
        continue;
      }
      out.push_back({lineStarts[j], spaces, ""});
    }
  }
  return out;
}

// Text rewritten by a pass that keeps its lines (one line for one: kppNumberNames, `this` as a reference), its
// continuation lines aligned as before: the rewrite's edits, line by line, are what differs between before and after
// (tokens matched by their longest common run), then alignmentEdits moves the lines.
inline std::string alignedAsBefore(const std::string& before, const std::string& after)
{
  if (before == after)
    return after;
  auto split = [](const std::string& s)
  {
    std::vector<std::pair<uint32_t, uint32_t>> lines; // (start, end)
    uint32_t start = 0;
    for (uint32_t i = 0; i <= s.size(); ++i)
      if (i == s.size() || s[i] == '\n')
      {
        lines.push_back({start, i});
        start = i + 1;
      }
    return lines;
  };
  auto b = split(before), a = split(after);
  if (b.size() != a.size())
    return after;
  // A line's pieces: names, numbers, single punctuation characters and runs of spaces.
  auto pieces = [](std::string_view s)
  {
    std::vector<std::string_view> out;
    for (size_t i = 0; i < s.size();)
    {
      size_t j = i + 1;
      if (isIdentChar(s[i]))
        while (j < s.size() && isIdentChar(s[j]))
          ++j;
      else if (s[i] == ' ')
        while (j < s.size() && s[j] == ' ')
          ++j;
      out.push_back(s.substr(i, j - i));
      i = j;
    }
    return out;
  };
  std::vector<Edit> edits;
  for (size_t l = 0; l < b.size(); ++l)
  {
    std::string_view x(before.data() + b[l].first, b[l].second - b[l].first), y(after.data() + a[l].first, a[l].second - a[l].first);
    if (x == y)
      continue;
    std::vector<std::string_view> p = pieces(x), q = pieces(y);
    if (p.size() > 400 || q.size() > 400)
    {
      // (a line too long to match piece by piece, Entity::str()'s: what changed is what lies between the
      // text the two have in common at the start and at the end; it was every line's alignment of the
      // file left as it was, kovarex's TriggerContext arguments a column off, 2026-11-11)
      size_t prefix = 0;
      while (prefix < x.size() && prefix < y.size() && x[prefix] == y[prefix])
        ++prefix;
      size_t suffix = 0;
      while (suffix < x.size() - prefix && suffix < y.size() - prefix && x[x.size() - 1 - suffix] == y[y.size() - 1 - suffix])
        ++suffix;
      edits.push_back({uint32_t(b[l].first + prefix), uint32_t(x.size() - prefix - suffix), std::string(y.substr(prefix, y.size() - prefix - suffix))});
      continue;
    }
    // The longest common subsequence of the pieces; the rest is what changed.
    std::vector<std::vector<uint16_t>> lcs(p.size() + 1, std::vector<uint16_t>(q.size() + 1, 0));
    for (size_t i = p.size(); i-- > 0;)
      for (size_t k = q.size(); k-- > 0;)
        lcs[i][k] = p[i] == q[k] ? uint16_t(lcs[i + 1][k + 1] + 1) : std::max(lcs[i + 1][k], lcs[i][k + 1]);
    size_t i = 0, k = 0;
    uint32_t xo = b[l].first;
    std::string inserted;
    uint32_t removedFrom = UINT32_MAX, removedLength = 0;
    auto flush = [&](uint32_t at)
    {
      if (removedFrom == UINT32_MAX && inserted.empty())
        return;
      edits.push_back({removedFrom == UINT32_MAX ? at : removedFrom, removedLength, inserted});
      removedFrom = UINT32_MAX, removedLength = 0, inserted.clear();
    };
    while (i < p.size() || k < q.size())
    {
      if (i < p.size() && k < q.size() && p[i] == q[k])
      {
        flush(xo);
        xo += uint32_t(p[i].size());
        ++i, ++k;
      }
      else if (k < q.size() && (i == p.size() || lcs[i][k + 1] >= lcs[i + 1][k]))
        inserted += q[k++];
      else
      {
        if (removedFrom == UINT32_MAX)
          removedFrom = xo;
        removedLength += uint32_t(p[i].size());
        xo += uint32_t(p[i].size());
        ++i;
      }
    }
    flush(xo);
  }
  std::vector<Edit> moves = alignmentEdits(before, edits);
  if (moves.empty())
    return after;
  // The moves are at line starts, where the rewrite changed nothing (alignmentEdits checked): applied to after by line.
  std::sort(moves.begin(), moves.end(), [](const Edit& x, const Edit& y) { return x.offset > y.offset; });
  std::string out = after;
  for (const Edit& m : moves)
  {
    size_t l = size_t(std::upper_bound(b.begin(), b.end(), m.offset, [](uint32_t o, const std::pair<uint32_t, uint32_t>& line) { return o < line.first; }) - b.begin()) - 1;
    out.replace(a[l].first, m.length, m.text);
    // (later lines of after shift: the moves go from the last line up, so each line's start is still right)
  }
  return out;
}
