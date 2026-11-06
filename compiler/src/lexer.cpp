#include "lexer.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

// --- Keywords and punctuators -----------------------------------------------------------

static constexpr std::string_view keywords[] = {
#define X(name) #name,
  KPP_KEYWORDS(X)
#undef X
};
static_assert(std::size(keywords) == size_t(Kw::Count));

static constexpr std::string_view punctuators[] = {
#define X(name, spelling) spelling,
  KPP_PUNCTUATORS(X)
#undef X
};
static_assert(std::size(punctuators) == size_t(P::Count));

std::string_view keywordSpelling(Kw id) { return keywords[uint32_t(id)]; }
std::string_view punctuatorSpelling(P id) { return punctuators[uint32_t(id)]; }

namespace
{
  struct KeywordMap
  {
    KeywordMap()
    {
      for (uint32_t i = 0; i < std::size(keywords); ++i)
        map.emplace(keywords[i], i);
    }

    std::unordered_map<std::string_view, uint32_t> map;
  };
  const KeywordMap& keywordMap()
  {
    static KeywordMap instance;
    return instance;
  }

  // Punctuator candidates per first byte, longest first.
  struct PunctuatorIndex
  {
    PunctuatorIndex()
    {
      for (uint32_t i = 0; i < std::size(punctuators); ++i)
        byFirst[uint8_t(punctuators[i][0])].push_back(i);
      for (auto& list : byFirst)
        std::sort(list.begin(), list.end(),
                  [](uint32_t a, uint32_t b) { return punctuators[a].size() > punctuators[b].size(); });
    }

    std::array<std::vector<uint32_t>, 256> byFirst;
  };
  const PunctuatorIndex& punctuatorIndex()
  {
    static PunctuatorIndex instance;
    return instance;
  }
}

uint32_t lookupKeyword(std::string_view spelling)
{
  const auto& map = keywordMap().map;
  auto it = map.find(spelling);
  return it == map.end() ? UINT32_MAX : it->second;
}

// --- Lexer ------------------------------------------------------------------------------

namespace
{
  inline bool isIdentStart(unsigned char c)
  {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
  }
  inline bool isIdentChar(unsigned char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
  inline bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }

  struct Lexer
  {
    Lexer(std::string_view text, SymbolTable& symbols)
      : begin(text.data()), p(text.data()), end(text.data() + text.size()), symbols(symbols)
    {
    }

    void push(TokenKind kind, const char* start, uint32_t id = 0)
    {
      Token t;
      t.offset = uint32_t(start - begin);
      t.length = uint32_t(p - start) & 0xFFFFFF;
      t.kind = uint32_t(kind);
      t.spliced = 0;
      t.id = id;
      result.tokens.push_back(t);
    }

    void error(const char* start)
    {
      ++result.errorCount;
      push(TokenKind::Error, start);
    }

    // Skips whitespace and comments. Returns true if a newline was crossed.
    bool skipTrivia()
    {
      bool newline = false;
      for (;;)
      {
        char c = *p;
        if (c == '\n')
        {
          ++p;
          ++result.lineCount;
          newline = true;
        }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')
          ++p;
        else if (c == '/' && p[1] == '/')
        {
          // (a splice continues the comment on the next line, as in C++)
          p += 2;
          while (*p && *p != '\n')
            if (!skipSplice())
              ++p;
        }
        else if (c == '/' && p[1] == '*')
        {
          p += 2;
          while (*p && !(*p == '*' && p[1] == '/'))
          {
            if (*p == '\n')
              ++result.lineCount;
            ++p;
          }
          if (*p)
            p += 2;
        }
        else if (c == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n')))
        {
          // A line splice between tokens: whitespace, unless it joins the token before it with what
          // follows into one (`count\` + `er`, `+\` + `=`), which is an error, never two tokens.
          const char* at = p;
          skipSplice();
          if (joinsTokens(at))
          {
            const char* save = p;
            p = at + 1;
            error(at);
            p = save;
          }
        }
        else
          return newline;
      }
    }

    // A line splice at p (a backslash ending a line): skipped, true.
    bool skipSplice()
    {
      if (*p != '\\' || !(p[1] == '\n' || (p[1] == '\r' && p[2] == '\n')))
        return false;
      p += p[1] == '\r' ? 3 : 2;
      ++result.lineCount;
      return true;
    }

    // Whether the splice at `at` (p just past it) stands inside a token: the token before it ends
    // right there and the character after it continues that token.
    bool joinsTokens(const char* at)
    {
      if (result.tokens.empty())
        return false;
      const Token& last = result.tokens.back();
      if (begin + last.offset + last.length != at)
        return false;
      unsigned char next = uint8_t(*p);
      switch (last.getKind())
      {
        case TokenKind::Identifier:
        case TokenKind::Keyword:
        case TokenKind::Number:
        case TokenKind::String: // (a literal's suffix)
        case TokenKind::Char:
          return isIdentChar(next) || (last.getKind() == TokenKind::Number && next == '.');
        case TokenKind::Punctuator:
        {
          std::string joined(begin + last.offset, last.length);
          joined += char(next);
          for (std::string_view s : punctuators)
            if (s.size() >= joined.size() && s.substr(0, joined.size()) == joined)
              return true;
          return false;
        }
        default:
          return false;
      }
    }

    void lexDirective(const char* start)
    {
      // Consume to end of line, honouring backslash continuations and comments.
      for (;;)
      {
        char c = *p;
        if (c == 0)
          break;
        if (c == '\n')
          break;
        if (c == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n')))
        {
          p += p[1] == '\r' ? 3 : 2;
          ++result.lineCount;
          continue;
        }
        if (c == '/' && p[1] == '*')
        {
          p += 2;
          while (*p && !(*p == '*' && p[1] == '/'))
          {
            if (*p == '\n')
              ++result.lineCount;
            ++p;
          }
          if (*p)
            p += 2;
          continue;
        }
        if (c == '/' && p[1] == '/')
        {
          while (*p && *p != '\n')
            ++p;
          break;
        }
        ++p;
      }
      push(TokenKind::Directive, start);
    }

    bool lexRawString(const char* start)
    {
      // p points at '"' after R. Delimiter up to ')'.
      ++p;
      const char* delimStart = p;
      while (*p && *p != '(' && *p != '"' && *p != '\n' && p - delimStart < 16)
        ++p;
      if (*p != '(')
      {
        error(start);
        return false;
      }
      std::string_view delim(delimStart, size_t(p - delimStart));
      ++p;
      for (;;)
      {
        if (*p == 0)
        {
          error(start);
          return false;
        }
        if (*p == '\n')
          ++result.lineCount;
        if (*p == ')' && strncmp(p + 1, delim.data(), delim.size()) == 0 && p[1 + delim.size()] == '"')
        {
          p += 2 + delim.size();
          break;
        }
        ++p;
      }
      push(TokenKind::String, start);
      return true;
    }

    void lexQuoted(const char* start, char quote)
    {
      // Line splices come out before escapes, as C++'s phase 2 before phase 5: `\\` + newline is a
      // backslash escaping the next line's first character. tokenSpelling takes them out.
      bool spliced = false;
      ++p;
      for (;;)
      {
        while (skipSplice())
          spliced = true;
        char c = *p;
        if (c == 0 || c == '\n')
        {
          error(start);
          return;
        }
        ++p;
        if (c == '\\')
        {
          while (skipSplice())
            spliced = true;
          if (*p && *p != '\n')
            ++p;
          continue;
        }
        if (c == quote)
          break;
      }
      // A literal's suffix; a program's own (`"text"_s`, `'x'_c`: '_' first) is a token of its own, as
      // after a number.
      if (*p != '_')
        while (isIdentChar(uint8_t(*p)))
          ++p;
      push(quote == '"' ? TokenKind::String : TokenKind::Char, start);
      result.tokens.back().spliced = spliced;
    }

    void lexNumber(const char* start)
    {
      // pp-number: digit or .digit, then [0-9a-zA-Z_.'] with e+/e-/p+/p-.
      for (;;)
      {
        char c = *p;
        if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (p[1] == '+' || p[1] == '-'))
          p += 2;
        else if (isIdentChar(uint8_t(c)) || c == '.' || (c == '\'' && isIdentChar(uint8_t(p[1]))))
          ++p;
        else
          break;
      }
      // A program's literal suffix (C++'s ud-suffix, `1.5_km`, `21_x2`, which starts with '_'; no
      // number has one): a token of its own, the identifier the parser makes a call of operator""_km.
      for (const char* q = start + 1; q < p; ++q)
        if (*q == '_')
        {
          p = q;
          break;
        }
      push(TokenKind::Number, start);
    }

    void lexIdentifier(const char* start)
    {
      while (isIdentChar(uint8_t(*p)))
        ++p;
      // String/char literal prefix: u8"..", L'..', R"(..)", u8R"(..)" etc.
      if (*p == '"' || *p == '\'')
      {
        std::string_view prefix(start, size_t(p - start));
        bool raw = !prefix.empty() && prefix.back() == 'R';
        std::string_view enc = raw ? prefix.substr(0, prefix.size() - 1) : prefix;
        if (enc.empty() || enc == "u8" || enc == "u" || enc == "U" || enc == "L")
        {
          if (raw && *p == '"')
          {
            lexRawString(start);
            return;
          }
          if (!raw)
          {
            lexQuoted(start, *p);
            return;
          }
        }
      }
      std::string_view spelling(start, size_t(p - start));
      uint32_t keyword = lookupKeyword(spelling);
      if (keyword != UINT32_MAX)
        push(TokenKind::Keyword, start, keyword);
      else
        push(TokenKind::Identifier, start, symbols.intern(spelling));
    }

    void lexPunctuator(const char* start)
    {
      for (uint32_t id : punctuatorIndex().byFirst[uint8_t(*p)])
      {
        std::string_view s = punctuators[id];
        if (strncmp(p, s.data(), s.size()) == 0)
        {
          p += s.size();
          push(TokenKind::Punctuator, start, id);
          return;
        }
      }
      ++p;
      error(start);
    }

    void run()
    {
      result.tokens.reserve(size_t(end - begin) / 5);
      if (end - begin >= 3 && uint8_t(p[0]) == 0xEF && uint8_t(p[1]) == 0xBB && uint8_t(p[2]) == 0xBF)
        p += 3; // UTF-8 byte order mark
      bool lineStart = true;
      for (;;)
      {
        if (skipTrivia())
          lineStart = true;
        const char* start = p;
        unsigned char c = uint8_t(*p);
        if (c == 0 && p >= end)
          break;
        if (c == '#' && lineStart)
        {
          lexDirective(start);
          continue;
        }
        lineStart = false;
        if (isIdentStart(c))
          lexIdentifier(start);
        else if (isDigit(c) || (c == '.' && isDigit(uint8_t(p[1]))))
          lexNumber(start);
        else if (c == '"' || c == '\'')
          lexQuoted(start, char(c));
        else
          lexPunctuator(start);
      }
      push(TokenKind::End, p);
    }

    const char* begin;
    const char* p;
    const char* end;
    SymbolTable& symbols;
    LexResult result;
  };
}

LexResult lex(std::string_view text, SymbolTable& symbols)
{
  Lexer lexer(text, symbols);
  lexer.run();
  return std::move(lexer.result);
}

std::string_view tokenSpelling(std::string_view text, const Token& t, std::string& buffer)
{
  std::string_view s = text.substr(t.offset, t.length);
  if (!t.spliced)
    return s;
  buffer.clear();
  for (size_t i = 0; i < s.size(); ++i)
  {
    if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '\n' || (s[i + 1] == '\r' && i + 2 < s.size() && s[i + 2] == '\n')))
    {
      i += s[i + 1] == '\r' ? 2 : 1;
      continue;
    }
    buffer += s[i];
  }
  return buffer;
}

void appendRawStringBody(std::string& out, std::string_view body)
{
  for (size_t i = 0; i < body.size(); ++i)
    if (!(body[i] == '\r' && i + 1 < body.size() && body[i + 1] == '\n'))
      out += body[i];
}

const char* tokenKindName(TokenKind kind)
{
  switch (kind)
  {
    case TokenKind::End: return "end";
    case TokenKind::Identifier: return "identifier";
    case TokenKind::Keyword: return "keyword";
    case TokenKind::Number: return "number";
    case TokenKind::String: return "string";
    case TokenKind::Char: return "char";
    case TokenKind::Punctuator: return "punctuator";
    case TokenKind::Directive: return "directive";
    case TokenKind::Error: return "error";
  }
  return "?";
}
