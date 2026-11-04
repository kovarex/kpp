#include "config.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace
{
  bool identifierChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80; }

  std::string_view spelled(std::string_view text, const Token& t) { return text.substr(t.offset, t.length); }

  bool isIdentifier(std::string_view text, const Token& t, std::string_view s)
  {
    return t.getKind() == TokenKind::Identifier && spelled(text, t) == s;
  }

  void position(std::string_view text, uint32_t offset, uint32_t& line, uint32_t& column)
  {
    line = 1;
    size_t start = 0;
    for (size_t i = 0; i < offset && i < text.size(); ++i)
      if (text[i] == '\n')
        ++line, start = i + 1;
    column = uint32_t(offset - start + 1);
  }

  // The integer types a config constant may have, with their ranges (long is left out: its width is
  // the platform's).
  struct IntegerType
  {
    const char* name;
    bool isSigned;
    int bits;
  };
  const IntegerType integerTypes[] = {
    {"i8", true, 8},       {"i16", true, 16},       {"i32", true, 32},       {"i64", true, 64},      {"u8", false, 8},
    {"u16", false, 16},    {"u32", false, 32},      {"u64", false, 64},      {"usize", false, 64},   {"int8_t", true, 8},
    {"int16_t", true, 16}, {"int32_t", true, 32},   {"int64_t", true, 64},   {"uint8_t", false, 8},  {"uint16_t", false, 16},
    {"uint32_t", false, 32}, {"uint64_t", false, 64}, {"size_t", false, 64}, {"int", true, 32},      {"short", true, 16},
    {"unsigned", false, 32}, {"unsigned int", false, 32}, {"unsigned short", false, 16}, {"signed", true, 32},
    {"signed int", true, 32}, {"short int", true, 16}, {"long long", true, 64}, {"unsigned long long", false, 64},
  };
  const IntegerType* integerType(const std::string& type)
  {
    for (const IntegerType& t : integerTypes)
      if (type == t.name)
        return &t;
    return nullptr;
  }

  enum class Found
  {
    None,     // not a config declaration
    Declared, // d is one; tokens [start, end] are its
    Malformed // error says why
  };

  // Whether tokens[i], at the top level and starting a declaration, is a config declaration.
  Found declarationAt(const std::vector<Token>& tokens, std::string_view text, size_t i, ConfigDeclaration& d, size_t& end, ConfigError& error)
  {
    if (!isIdentifier(text, tokens[i], "config") || i + 1 >= tokens.size())
      return Found::None;
    TokenKind next = tokens[i + 1].getKind();
    if (next != TokenKind::Identifier && next != TokenKind::Keyword)
      return Found::None; // config::NAME, an expression
    position(text, tokens[i].offset, d.line, d.column);
    auto fail = [&](size_t at, std::string message)
    {
      position(text, tokens[at < tokens.size() ? at : tokens.size() - 1].offset, error.line, error.column);
      error.message = std::move(message);
      return Found::Malformed;
    };
    // First on its line (kpp's options and readers find it there).
    for (size_t k = tokens[i].offset; k > 0 && text[k - 1] != '\n'; --k)
      if (text[k - 1] != ' ' && text[k - 1] != '\t' && text[k - 1] != '\r')
        return fail(i, "a config declaration starts its line");
    size_t j = i + 1;
    auto at = [&](size_t k) -> const Token* { return k < tokens.size() && tokens[k].getKind() != TokenKind::End ? &tokens[k] : nullptr; };
    auto punctuator = [&](size_t k, P p) { return at(k) && at(k)->is(p); };
    const char* types = "a config constant's type is bool, an integer type (i32, u64, int...), StringView or an enumeration";
    // Its type: keywords (bool, unsigned int...) or a name, qualified or not (i32, Platform, zlib::Mode).
    if (tokens[j].getKind() == TokenKind::Keyword)
    {
      while (at(j) && at(j)->getKind() == TokenKind::Keyword && at(j + 1) && at(j + 1)->getKind() == TokenKind::Keyword)
        d.type += std::string(spelled(text, tokens[j++])) + " ";
      d.type += spelled(text, tokens[j++]);
      if (d.type == "bool")
        d.kind = "bool";
      else if (integerType(d.type))
        d.kind = "int";
      else
        return fail(i + 1, std::string(types) + ", not " + d.type);
    }
    else
    {
      d.type = spelled(text, tokens[j++]);
      while (punctuator(j, P::ColonColon) && at(j + 1) && at(j + 1)->getKind() == TokenKind::Identifier)
        d.type += "::" + std::string(spelled(text, tokens[j + 1])), j += 2;
      d.kind = integerType(d.type) ? "int" : d.type == "StringView" ? "string" : "enum";
    }
    if (!at(j) || at(j)->getKind() != TokenKind::Identifier)
      return fail(j, "expected the config constant's name after its type " + d.type + " (`config TYPE NAME = DEFAULT;`)");
    d.name = spelled(text, tokens[j++]);
    if (punctuator(j, P::Semi))
    {
      d.required = true;
      end = j;
      return Found::Declared;
    }
    if (!punctuator(j, P::Assign))
      return fail(j, "expected '=' and its default, or ';' for a required one, after config " + d.type + " " + d.name);
    ++j;
    const Token* v = at(j);
    if (d.kind == "bool")
    {
      if (!v || !(v->is(Kw::true_) || v->is(Kw::false_)))
        return fail(j, "config bool " + d.name + "'s default is true or false");
      d.value = spelled(text, *v);
      ++j;
    }
    else if (d.kind == "int")
    {
      bool negative = punctuator(j, P::Minus);
      if (negative)
        v = at(++j);
      if (!v || v->getKind() != TokenKind::Number)
        return fail(j, "config " + d.type + " " + d.name + "'s default is an integer");
      std::string digits;
      for (char c : spelled(text, *v))
        if (c != '\'')
          digits += c;
      while (!digits.empty() && strchr("uUlLzZ", digits.back()))
        digits.pop_back();
      const IntegerType* t = integerType(d.type);
      errno = 0;
      char* stop = nullptr;
      unsigned long long magnitude = strtoull(digits.c_str(), &stop, 0);
      bool fits = errno == 0 && stop && *stop == 0 && !digits.empty();
      if (fits && t->isSigned)
        fits = negative ? magnitude <= (1ull << (t->bits - 1)) : magnitude < (1ull << (t->bits - 1));
      else if (fits)
        fits = !negative && (t->bits == 64 || magnitude < (1ull << t->bits));
      if (!fits)
        return fail(j, "config " + d.type + " " + d.name + "'s default " + (negative ? "-" : "") + std::string(spelled(text, *v)) + " does not fit its type " + d.type);
      d.value = negative ? (magnitude == 0 ? "0" : "-" + std::to_string(magnitude)) : std::to_string(magnitude);
      ++j;
    }
    else if (d.kind == "string")
    {
      std::string_view s = v ? spelled(text, *v) : std::string_view();
      if (!v || v->getKind() != TokenKind::String || v->spliced || s.size() < 2 || s.front() != '"' || s.back() != '"' ||
          s.find('\\') != std::string_view::npos)
        return fail(j, "config StringView " + d.name + "'s default is a plain \"...\" string, without escapes or a prefix");
      d.value = s.substr(1, s.size() - 2);
      ++j;
    }
    else
    {
      // An enumerator of its type: TYPE::NAME.
      std::string written;
      size_t first = j;
      while (at(j) && at(j)->getKind() == TokenKind::Identifier)
      {
        written += (written.empty() ? "" : "::") + std::string(spelled(text, tokens[j++]));
        if (!punctuator(j, P::ColonColon))
          break;
        ++j;
      }
      size_t last = written.rfind("::");
      if (last == std::string::npos || written.substr(0, last) != d.type)
        return fail(first, "config " + d.type + " " + d.name + "'s default is one of its enumerators, " + d.type + "::NAME");
      d.value = written.substr(last + 2);
    }
    if (!punctuator(j, P::Semi))
      return fail(j, "expected ';' after config " + d.type + " " + d.name + "'s default");
    end = j;
    return Found::Declared;
  }

  // The // comment lines right above a declaration's line, joined by blanks.
  std::string descriptionAbove(std::string_view text, uint32_t offset)
  {
    size_t lineStart = offset;
    while (lineStart > 0 && text[lineStart - 1] != '\n')
      --lineStart;
    std::vector<std::string_view> lines;
    while (lineStart > 0)
    {
      size_t end = lineStart - 1; // its '\n'
      size_t start = end;
      while (start > 0 && text[start - 1] != '\n')
        --start;
      std::string_view line = text.substr(start, end - start);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.remove_suffix(1);
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
      if (!line.starts_with("//"))
        break;
      line.remove_prefix(2);
      while (!line.empty() && line.front() == ' ')
        line.remove_prefix(1);
      lines.push_back(line);
      lineStart = start;
    }
    std::string out;
    for (size_t k = lines.size(); k-- > 0;)
      out += (out.empty() ? "" : " ") + std::string(lines[k]);
    return out;
  }

  // Calls f(i) for each token that starts a declaration at the file's top level.
  template <typename F> void topLevelStarts(const std::vector<Token>& tokens, F f)
  {
    int depth = 0;
    bool start = true;
    for (size_t i = 0; i < tokens.size() && tokens[i].getKind() != TokenKind::End; ++i)
    {
      const Token& t = tokens[i];
      if (t.getKind() == TokenKind::Directive)
        continue;
      if (depth == 0 && start)
      {
        size_t skip = f(i);
        if (skip != i)
        {
          i = skip; // its ';'
          continue;
        }
      }
      if (t.is(P::LBrace))
        ++depth;
      else if (t.is(P::RBrace) && depth > 0)
        --depth;
      start = t.is(P::Semi) || t.is(P::RBrace);
    }
  }
}

bool mayDeclareConfig(std::string_view text)
{
  for (size_t at = text.find("config"); at != std::string_view::npos; at = text.find("config", at + 6))
  {
    if (at > 0 && identifierChar(text[at - 1]))
      continue;
    size_t k = at + 6;
    if (k < text.size() && identifierChar(text[k]))
      continue;
    while (k < text.size() && (text[k] == ' ' || text[k] == '\t' || text[k] == '\r' || text[k] == '\n'))
      ++k;
    if (k + 1 < text.size() && text[k] == ':' && text[k + 1] == ':')
      continue;
    return true;
  }
  return false;
}

void collectConfigDeclarations(const std::vector<Token>& tokens, std::string_view text, std::vector<ConfigDeclaration>& out,
                               std::vector<ConfigError>& errors)
{
  topLevelStarts(tokens, [&](size_t i)
  {
    ConfigDeclaration d;
    ConfigError error;
    size_t end = i;
    Found found = declarationAt(tokens, text, i, d, end, error);
    if (found == Found::Malformed)
      errors.push_back(error);
    if (found != Found::Declared)
      return i;
    d.description = descriptionAbove(text, tokens[i].offset);
    out.push_back(std::move(d));
    return end;
  });
}

void findConfigDeclarations(std::string_view text, std::vector<ConfigDeclaration>& out, std::vector<ConfigError>& errors)
{
  if (!mayDeclareConfig(text))
    return;
  SymbolTable symbols;
  LexResult lexed = lex(text, symbols);
  collectConfigDeclarations(lexed.tokens, text, out, errors);
}

void dropConfigDeclarations(std::vector<Token>& tokens, std::string_view text)
{
  // Every .kpp file comes here before it is parsed: most have no `config` before a name at all.
  bool any = false;
  for (size_t i = 0; i + 1 < tokens.size() && !any; ++i)
    any = tokens[i].length == 6 && isIdentifier(text, tokens[i], "config") &&
          (tokens[i + 1].getKind() == TokenKind::Identifier || tokens[i + 1].getKind() == TokenKind::Keyword);
  if (!any)
    return;
  std::vector<std::pair<size_t, size_t>> dropped;
  topLevelStarts(tokens, [&](size_t i)
  {
    ConfigDeclaration d;
    ConfigError error;
    size_t end = i;
    if (declarationAt(tokens, text, i, d, end, error) != Found::Declared)
      return i;
    dropped.push_back({i, end});
    return end;
  });
  if (dropped.empty())
    return;
  std::vector<Token> kept;
  kept.reserve(tokens.size());
  size_t next = 0;
  for (size_t i = 0; i < tokens.size(); ++i)
  {
    if (next < dropped.size() && i == dropped[next].first)
    {
      i = dropped[next++].second;
      continue;
    }
    kept.push_back(tokens[i]);
  }
  tokens = std::move(kept);
}
