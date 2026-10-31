#include "lsp.hpp"
#include "ast.hpp"
#include "ast_walk.hpp"
#include "resolver.hpp"
#include "types.hpp"
#include "project.hpp"
#include "source.hpp"
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <io.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <type_traits>
#include <vector>
#include <windows.h>

// The Language Server Protocol 3.17 (https://microsoft.github.io/language-server-protocol/): a
// message is a `Content-Length: N` header, an empty line and N bytes of JSON-RPC 2.0. Positions are
// lines and UTF-16 code units, from 0.

namespace
{
  namespace fs = std::filesystem;
  using Clock = std::chrono::steady_clock;

  // ---------------- JSON ----------------

  struct Json
  {
    enum Kind : uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : kind(Bool), boolean(b) {}
    template<typename T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Json(T n) : kind(Number), number(double(n)) {}
    Json(const char* s) : kind(String), text(s) {}
    Json(std::string s) : kind(String), text(std::move(s)) {}
    static Json array()
    {
      Json j;
      j.kind = Array;
      return j;
    }
    static Json object()
    {
      Json j;
      j.kind = Object;
      return j;
    }
    const Json& operator[](std::string_view key) const
    {
      static const Json none;
      for (const auto& [name, value] : members)
        if (name == key)
          return value;
      return none;
    }
    bool has(std::string_view key) const { return (*this)[key].kind != Null; }
    Json& set(std::string key, Json value)
    {
      kind = Object;
      members.emplace_back(std::move(key), std::move(value));
      return *this;
    }
    Json& push(Json value)
    {
      kind = Array;
      items.push_back(std::move(value));
      return *this;
    }
    int64_t integer() const { return int64_t(number); }
  };

  struct JsonParser
  {
    std::string_view s;
    size_t i = 0;
    bool ok = true;

    void space()
    {
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        ++i;
    }
    bool literal(std::string_view word)
    {
      if (s.substr(i, word.size()) != word)
        return ok = false;
      i += word.size();
      return true;
    }
    static void appendUtf8(std::string& out, uint32_t c)
    {
      if (c < 0x80)
        out += char(c);
      else if (c < 0x800)
        out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
      else if (c < 0x10000)
        out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
      else
        out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
    uint32_t hex4()
    {
      if (i + 4 > s.size())
        return ok = false, 0;
      uint32_t v = 0;
      for (int k = 0; k < 4; ++k)
      {
        char c = s[i++];
        v = v * 16 + uint32_t(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : (ok = false, 0));
      }
      return v;
    }
    std::string string()
    {
      std::string out;
      ++i; // "
      while (ok && i < s.size() && s[i] != '"')
      {
        char c = s[i++];
        if (c != '\\')
        {
          out += c;
          continue;
        }
        if (i >= s.size())
          break;
        char e = s[i++];
        switch (e)
        {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u':
        {
          uint32_t c1 = hex4();
          if (c1 >= 0xD800 && c1 < 0xDC00 && s.substr(i, 2) == "\\u")
          {
            i += 2;
            uint32_t c2 = hex4();
            c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
          }
          appendUtf8(out, c1);
          break;
        }
        default: out += e;
        }
      }
      if (i >= s.size())
        ok = false;
      ++i; // "
      return out;
    }
    Json value(int depth = 0)
    {
      space();
      if (i >= s.size() || depth > 200)
        return ok = false, Json();
      char c = s[i];
      if (c == '{')
      {
        ++i;
        Json o = Json::object();
        space();
        if (i < s.size() && s[i] == '}')
          return ++i, o;
        while (ok)
        {
          space();
          if (i >= s.size() || s[i] != '"')
            return ok = false, o;
          std::string key = string();
          space();
          if (i >= s.size() || s[i] != ':')
            return ok = false, o;
          ++i;
          o.members.emplace_back(std::move(key), value(depth + 1));
          space();
          if (i < s.size() && s[i] == ',')
            ++i;
          else if (i < s.size() && s[i] == '}')
            return ++i, o;
          else
            ok = false;
        }
        return o;
      }
      if (c == '[')
      {
        ++i;
        Json a = Json::array();
        space();
        if (i < s.size() && s[i] == ']')
          return ++i, a;
        while (ok)
        {
          a.items.push_back(value(depth + 1));
          space();
          if (i < s.size() && s[i] == ',')
            ++i;
          else if (i < s.size() && s[i] == ']')
            return ++i, a;
          else
            ok = false;
        }
        return a;
      }
      if (c == '"')
        return Json(string());
      if (c == 't')
        return literal("true"), Json(true);
      if (c == 'f')
        return literal("false"), Json(false);
      if (c == 'n')
        return literal("null"), Json();
      size_t start = i;
      while (i < s.size() && (isdigit((unsigned char)s[i]) || strchr("+-.eE", s[i])))
        ++i;
      if (start == i)
        return ok = false, Json();
      return Json(strtod(std::string(s.substr(start, i - start)).c_str(), nullptr));
    }
  };

  // Text as JSON wants it: valid UTF-8 (a byte that is not is written as U+FFFD: a diagnostic may
  // quote a file's Latin-1 comment).
  void writeString(const std::string& text, std::string& out)
  {
    out += '"';
    for (size_t i = 0; i < text.size();)
    {
      unsigned char c = text[i];
      if (c < 0x80)
      {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20)
          {
            char buffer[8];
            snprintf(buffer, sizeof buffer, "\\u%04x", c);
            out += buffer;
          }
          else
            out += char(c);
        }
        ++i;
        continue;
      }
      size_t length = c >= 0xF0 && c < 0xF8 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
      bool valid = length && i + length <= text.size();
      for (size_t k = 1; valid && k < length; ++k)
        valid = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
      if (valid)
        out.append(text, i, length), i += length;
      else
        out += "\xEF\xBF\xBD", ++i;
    }
    out += '"';
  }

  void write(const Json& j, std::string& out)
  {
    switch (j.kind)
    {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.boolean ? "true" : "false"; break;
    case Json::Number:
    {
      char buffer[32];
      if (j.number == double(int64_t(j.number)))
        snprintf(buffer, sizeof buffer, "%lld", (long long)j.number);
      else
        snprintf(buffer, sizeof buffer, "%.17g", j.number);
      out += buffer;
      break;
    }
    case Json::String: writeString(j.text, out); break;
    case Json::Array:
      out += '[';
      for (size_t k = 0; k < j.items.size(); ++k)
      {
        if (k)
          out += ',';
        write(j.items[k], out);
      }
      out += ']';
      break;
    case Json::Object:
      out += '{';
      for (size_t k = 0; k < j.members.size(); ++k)
      {
        if (k)
          out += ',';
        writeString(j.members[k].first, out);
        out += ':';
        write(j.members[k].second, out);
      }
      out += '}';
      break;
    }
  }

  // ---------------- transport ----------------

  HANDLE input = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE, logHandle = INVALID_HANDLE_VALUE;
  std::mutex outputMutex;

  void writeHandle(HANDLE h, const std::string& bytes)
  {
    for (size_t done = 0; done < bytes.size();)
    {
      DWORD n = 0;
      if (!WriteFile(h, bytes.data() + done, DWORD(bytes.size() - done), &n, nullptr) || n == 0)
        return;
      done += n;
    }
  }

  // A line for the editor's log of this server (stderr, which editors show).
  void log(const char* format, ...)
  {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    std::lock_guard lock(outputMutex);
    writeHandle(logHandle, std::string("kppc --lsp: ") + text + "\n");
  }

  void send(const Json& message)
  {
    std::string body;
    write(message, body);
    std::lock_guard lock(outputMutex);
    writeHandle(output, "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  }

  struct Reader
  {
    char buffer[1 << 16];
    DWORD size = 0, at = 0;
    bool get(char& c)
    {
      if (at == size)
      {
        at = size = 0;
        if (!ReadFile(input, buffer, DWORD(sizeof buffer), &size, nullptr) || size == 0)
          return false;
      }
      c = buffer[at++];
      return true;
    }
    // One message's body; false at the end of the input.
    bool message(std::string& body)
    {
      size_t length = SIZE_MAX;
      for (;;)
      {
        std::string line;
        char c;
        while (get(c) && c != '\n')
          line += c;
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (line.empty())
        {
          if (length != SIZE_MAX)
            break;
          if (at == size && size == 0)
            return false;
          continue;
        }
        if (_strnicmp(line.c_str(), "Content-Length:", 15) == 0)
          length = size_t(strtoull(line.c_str() + 15, nullptr, 10));
      }
      body.resize(length);
      for (size_t k = 0; k < length; ++k)
        if (!get(body[k]))
          return false;
      return true;
    }
  };

  // The messages read, in order; the end of the input is a message with ended set.
  struct Inbox
  {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Json> messages;
    bool ended = false;

    void put(Json m)
    {
      std::lock_guard lock(mutex);
      messages.push_back(std::move(m));
      ready.notify_one();
    }
    void end()
    {
      std::lock_guard lock(mutex);
      ended = true;
      ready.notify_one();
    }
    bool take(Json& m)
    {
      std::unique_lock lock(mutex);
      ready.wait(lock, [&] { return !messages.empty() || ended; });
      if (messages.empty())
        return false;
      m = std::move(messages.front());
      messages.pop_front();
      return true;
    }
    bool empty()
    {
      std::lock_guard lock(mutex);
      return messages.empty();
    }
  };
  Inbox inbox;

  // ---------------- documents and positions ----------------

  std::string percentDecode(std::string_view s)
  {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
      if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]))
      {
        out += char(strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16));
        i += 2;
      }
      else
        out += s[i];
    return out;
  }

  fs::path uriToPath(const std::string& uri)
  {
    std::string_view s = uri;
    if (s.substr(0, 7) != "file://")
      return {};
    s.remove_prefix(7);
    std::string path = percentDecode(s);
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':')
      path.erase(0, 1); // /c:/x -> c:/x
    else if (!path.empty() && path[0] != '/')
      path = "//" + path; // file://server/share
    std::wstring wide(path.size(), L'\0');
    wide.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, path.data(), int(path.size()), wide.data(), int(wide.size()))));
    return fs::path(wide).make_preferred();
  }

  std::string pathToUri(const fs::path& path)
  {
    std::error_code ec;
    std::string text;
    std::u8string u8 = fs::absolute(path, ec).lexically_normal().generic_u8string();
    text.assign(u8.begin(), u8.end());
    if (text.size() >= 2 && text[1] == ':')
      text[0] = char(tolower((unsigned char)text[0]));
    std::string out = "file:///";
    for (unsigned char c : text)
      if (isalnum(c) || strchr("/-._~", c))
        out += char(c);
      else
      {
        char buffer[4];
        snprintf(buffer, sizeof buffer, "%%%02X", c);
        out += buffer;
      }
    return out;
  }

  // Where each line starts in a text.
  std::vector<size_t> lineStarts(std::string_view text)
  {
    std::vector<size_t> starts{0};
    for (size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n')
        starts.push_back(i + 1);
    return starts;
  }

  // UTF-16 code units from `from` to `to` in UTF-8 text.
  size_t utf16Units(std::string_view text, size_t from, size_t to)
  {
    size_t units = 0;
    for (size_t i = from; i < to && i < text.size();)
    {
      unsigned char c = text[i];
      size_t length = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      units += length == 4 ? 2 : 1;
      i += length;
    }
    return units;
  }

  Json position(std::string_view text, const std::vector<size_t>& starts, size_t offset)
  {
    size_t line = size_t(std::upper_bound(starts.begin(), starts.end(), offset) - starts.begin()) - 1;
    return Json::object().set("line", line).set("character", utf16Units(text, starts[line], offset));
  }

  Json range(std::string_view text, const std::vector<size_t>& starts, size_t from, size_t to)
  {
    return Json::object().set("start", position(text, starts, from)).set("end", position(text, starts, to));
  }

  // The byte offset of an LSP position (clamped to its line).
  size_t offsetOf(std::string_view text, const std::vector<size_t>& starts, const Json& position)
  {
    size_t line = size_t(std::max<int64_t>(0, position["line"].integer()));
    if (line >= starts.size())
      return text.size();
    size_t units = size_t(std::max<int64_t>(0, position["character"].integer()));
    size_t i = starts[line];
    size_t end = line + 1 < starts.size() ? starts[line + 1] - 1 : text.size();
    while (i < end && units > 0)
    {
      unsigned char c = text[i];
      size_t length = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      size_t u = length == 4 ? 2 : 1;
      if (u > units)
        break;
      units -= u;
      i += length;
    }
    return std::min(i, end);
  }

  struct Document
  {
    std::string uri;
    fs::path path;
    std::string text;
    int64_t version = 0;
  };

  // ---------------- what the kept program answers ----------------

  // A name in a file's text and the declaration it is: a use (resolved by the compiler) or the
  // declaration's own name.
  struct Occurrence
  {
    uint32_t token;
    Decl* decl;
    Node* node;          // the name's node (NameExpr, MemberExpr, NamedType) or the declaration
    bool declaration;    // the declaration's own name
  };

  bool sameIdentifier(const Token& t, uint32_t symbol) { return t.getKind() == TokenKind::Identifier && t.id == symbol; }

  // The token of a qualified name's last part, found from `from` on: each part's identifier in turn
  // (`Shape::Shape` is the class, then the constructor). UINT32_MAX when it is not there (an
  // operator, a name the parser made up).
  uint32_t nameToken(const std::vector<Token>& tokens, const QualifiedName& name, uint32_t from, uint32_t end)
  {
    uint32_t at = UINT32_MAX;
    end = std::min<uint32_t>(end, uint32_t(tokens.size()));
    for (const NamePart& part : name.parts)
    {
      if (part.symbol == UINT32_MAX || part.special >= 2)
      {
        at = UINT32_MAX;
        continue;
      }
      uint32_t k = from;
      while (k < end && !sameIdentifier(tokens[k], part.symbol))
        ++k;
      if (k == end)
        return UINT32_MAX;
      at = k;
      from = k + 1;
    }
    return at;
  }

  uint32_t symbolToken(const std::vector<Token>& tokens, uint32_t symbol, uint32_t from, uint32_t end)
  {
    end = std::min<uint32_t>(end, uint32_t(tokens.size()));
    for (uint32_t k = from; symbol != UINT32_MAX && k < end; ++k)
      if (sameIdentifier(tokens[k], symbol))
        return k;
    return UINT32_MAX;
  }

  const QualifiedName* qualifiedNameOf(const Decl* d)
  {
    switch (d->kind)
    {
    case NodeKind::ClassDecl: return &static_cast<const ClassDecl*>(d)->name;
    case NodeKind::EnumDecl: return &static_cast<const EnumDecl*>(d)->name;
    case NodeKind::FunctionDecl: return &static_cast<const FunctionDecl*>(d)->name;
    case NodeKind::VariableDecl: return &static_cast<const VariableDecl*>(d)->name;
    default: return nullptr;
    }
  }

  uint32_t symbolOf(const Decl* d)
  {
    switch (d->kind)
    {
    case NodeKind::NamespaceDecl: return static_cast<const NamespaceDecl*>(d)->name;
    case NodeKind::ParamDecl: return static_cast<const ParamDecl*>(d)->name;
    case NodeKind::EnumeratorDecl: return static_cast<const EnumeratorDecl*>(d)->name;
    case NodeKind::UsingAliasDecl: return static_cast<const UsingAliasDecl*>(d)->name;
    case NodeKind::TemplateParamDecl: return static_cast<const TemplateParamDecl*>(d)->name;
    default:
      if (const QualifiedName* q = qualifiedNameOf(d); q && !q->empty())
        return q->last().special == 0 || q->last().special == 1 ? q->last().symbol : UINT32_MAX;
      return UINT32_MAX;
    }
  }

  // The token of a declaration's own name in its file; UINT32_MAX when it has none there.
  uint32_t declarationNameToken(const Decl* d)
  {
    if (!d->file || !d->file->tokens)
      return UINT32_MAX;
    const std::vector<Token>& tokens = *d->file->tokens;
    uint32_t end = d->endToken > d->token ? d->endToken : uint32_t(tokens.size());
    if (d->kind == NodeKind::FunctionDecl && static_cast<const FunctionDecl*>(d)->body)
      end = std::min(end, static_cast<const FunctionDecl*>(d)->body->token);
    if (const QualifiedName* q = qualifiedNameOf(d))
      return q->empty() ? UINT32_MAX : nameToken(tokens, *q, d->token, end);
    return symbolToken(tokens, symbolOf(d), d->token, end);
  }

  std::string declarationName(const Decl* d, const SymbolTable& symbols)
  {
    if (const QualifiedName* q = qualifiedNameOf(d))
    {
      std::string out;
      for (const NamePart& part : q->parts)
      {
        if (!out.empty() || q->global)
          out += "::";
        if (part.special == 1)
          out += "~" + std::string(symbols.name(part.symbol));
        else if (part.special == 2 && part.opToken < uint32_t(P::Count))
          out += "operator" + std::string(punctuatorSpelling(P(part.opToken)));
        else if (part.special == 2 || part.special == 3)
          out += "operator";
        else if (part.symbol != UINT32_MAX)
          out += symbols.name(part.symbol);
      }
      return out.empty() ? "(anonymous)" : out;
    }
    uint32_t s = symbolOf(d);
    return s == UINT32_MAX ? std::string() : std::string(symbols.name(s));
  }

  Decl* resolvedOf(Node* n)
  {
    switch (n->kind)
    {
    case NodeKind::NameExpr: return static_cast<NameExpr*>(n)->resolved;
    case NodeKind::MemberExpr: return static_cast<MemberExpr*>(n)->resolved;
    case NodeKind::NamedType: return static_cast<NamedType*>(n)->resolved;
    default: return nullptr;
    }
  }

  bool isDeclarationNode(const Node* n) { return n->kind >= NodeKind::NamespaceDecl && n->kind <= NodeKind::ImportDecl; }

  // Every name in a file the compiler resolved, and every declaration's own name, by token.
  std::vector<Occurrence> occurrencesIn(const ParsedFile& file)
  {
    std::vector<Occurrence> out;
    if (!file.tokens)
      return out;
    const std::vector<Token>& tokens = *file.tokens;
    forEachNode(file.decls, [&](Node* n)
    {
      if (isDeclarationNode(n))
      {
        Decl* d = static_cast<Decl*>(n);
        if (d->file == &file)
          if (uint32_t t = declarationNameToken(d); t != UINT32_MAX)
            out.push_back({t, d, n, true});
        return;
      }
      Decl* d = resolvedOf(n);
      if (!d)
        return;
      uint32_t t = UINT32_MAX;
      if (n->kind == NodeKind::MemberExpr)
      {
        auto* m = static_cast<MemberExpr*>(n);
        uint32_t after = m->token;
        if (m->base)
          forEachNode(m->base, [&](Node* b) { after = std::max(after, b->token); });
        t = nameToken(tokens, m->member, after + 1, after + 64);
      }
      else
      {
        const QualifiedName& q = n->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(n)->name : static_cast<NamedType*>(n)->name;
        t = nameToken(tokens, q, n->token, n->token + 64);
      }
      if (t != UINT32_MAX)
        out.push_back({t, d, n, false});
    });
    std::sort(out.begin(), out.end(), [](const Occurrence& a, const Occurrence& b) { return a.token != b.token ? a.token < b.token : a.declaration > b.declaration; });
    out.erase(std::unique(out.begin(), out.end(), [](const Occurrence& a, const Occurrence& b) { return a.token == b.token; }), out.end());
    return out;
  }

  // The token at a byte offset: the one it is in, or the one it ends (a cursor after a name).
  uint32_t tokenAt(const std::vector<Token>& tokens, size_t offset)
  {
    auto it = std::upper_bound(tokens.begin(), tokens.end(), offset, [](size_t o, const Token& t) { return o < t.offset; });
    if (it == tokens.begin())
      return UINT32_MAX;
    uint32_t k = uint32_t(it - tokens.begin()) - 1;
    // Right after a name and before what follows it (`total|(`): the name.
    if (offset == tokens[k].offset && k > 0 && tokens[k].getKind() != TokenKind::Identifier && tokens[k - 1].getKind() == TokenKind::Identifier &&
        size_t(tokens[k - 1].offset) + tokens[k - 1].length == offset)
      return k - 1;
    if (offset < size_t(tokens[k].offset) + tokens[k].length || (offset == size_t(tokens[k].offset) + tokens[k].length && tokens[k].getKind() == TokenKind::Identifier))
      return k;
    return UINT32_MAX;
  }

  // ---------------- the server ----------------

  struct Diagnostic
  {
    size_t line = 0, column = 0, length = 0; // line and column from 1 (column 0: unknown); length 0: the token there
    int severity = 1;                       // LSP: 1 error, 2 warning, 3 information
    std::string message;
  };

  struct Server
  {
    CompileFunction compile = nullptr;
    std::vector<std::string> explicitArguments; // `kppc --lsp OPTIONS SOURCES`
    std::vector<std::string> checkArguments;    // what a check builds (empty: nothing to check)
    bool checkEachChange = false;
    bool initialized = false, shutdownRequested = false;
    bool dirty = false;                         // a document changed since the last check
    size_t checks = 0;
    std::map<std::string, Document> documents;  // the open documents, by overlayKey
    std::map<std::string, std::string> published; // what was last published per file (overlayKey -> the message's JSON)
    std::map<std::string, std::string> uris;      // the URI each published file goes by

    // What the queries keep about the kept program, made when first asked and forgotten at every
    // check (the program changes in place).
    struct Cache
    {
      bool filesKnown = false;
      std::map<std::string, ParsedFile*> files;                          // by overlayKey
      std::unordered_map<const ParsedFile*, std::vector<Occurrence>> occurrences;
      std::unordered_map<const ParsedFile*, std::vector<size_t>> starts;
      bool referencesKnown = false;
      std::unordered_map<const Decl*, std::vector<std::pair<const ParsedFile*, uint32_t>>> references; // by canonical declaration
      struct Symbol
      {
        std::string name, lower, container;
        int kind;
        Decl* decl;
      };
      std::vector<Symbol> symbols;
      bool symbolsKnown = false;
    };
    Cache cache;

    std::string uriOf(const std::string& key, const fs::path& path)
    {
      if (auto d = documents.find(key); d != documents.end())
        return d->second.uri;
      if (auto u = uris.find(key); u != uris.end())
        return u->second;
      return pathToUri(path);
    }

    void respond(const Json& id, Json result)
    {
      send(Json::object().set("jsonrpc", "2.0").set("id", id).set("result", std::move(result)));
    }

    void respondError(const Json& id, int code, const std::string& message)
    {
      send(Json::object().set("jsonrpc", "2.0").set("id", id).set("error", Json::object().set("code", code).set("message", message)));
    }

    void notify(const char* method, Json params)
    {
      send(Json::object().set("jsonrpc", "2.0").set("method", method).set("params", std::move(params)));
    }

    void showMessage(int type, const std::string& message)
    {
      notify("window/showMessage", Json::object().set("type", type).set("message", message));
    }

    // What a check builds: the sources and options given after --lsp, else the workspace's build.kpp
    // project, else the workspace folder; always the front end alone, with columns in diagnostics.
    void chooseArguments(const fs::path& workspace, const char* self)
    {
      std::vector<std::string> args;
      if (!explicitArguments.empty())
      {
        args.push_back(self);
        args.insert(args.end(), explicitArguments.begin(), explicitArguments.end());
      }
      else if (!workspace.empty())
      {
        std::string error;
        if (projectCheckArguments(workspace, args, error))
        {
          if (!error.empty())
          {
            log("%s", error.c_str());
            showMessage(1, "Kpp: " + error + "; nothing is checked.");
            checkArguments.clear();
            return;
          }
          log("the project of %s (build.kpp)", workspace.string().c_str());
        }
        else
        {
          args = {self, workspace.string()};
          log("no build.kpp at or above %s: its files are the program", workspace.string().c_str());
        }
      }
      else
      {
        log("no workspace folder and no sources given: nothing is checked");
        checkArguments.clear();
        return;
      }
      // A check is the front end: what only code generation reads is left out.
      std::vector<std::string> kept;
      bool hasStd = false;
      for (size_t i = 0; i < args.size(); ++i)
      {
        if ((args[i] == "--exe" || args[i] == "--emit-llvm") && i + 1 < args.size())
        {
          log("%s %s left out: a check is the front end", args[i].c_str(), args[i + 1].c_str());
          ++i;
          continue;
        }
        if (args[i] == "--server" || args[i] == "--verify-incremental")
          continue;
        hasStd = hasStd || args[i] == "--std" || args[i] == "--no-std";
        kept.push_back(args[i]);
      }
      // Without --exe the standard library joins only when --std names it (main.cpp).
      if (!hasStd)
        kept.insert(kept.begin() + 1, {"--std", defaultStdlib(compilerPath()).string()});
      // --check: codegen's errors too (the checker's type errors are reported by codegen), nothing written.
      kept.insert(kept.begin() + 1, {"--quiet", "--diagnostic-columns", "--check"});
      checkArguments = kept;
      std::string line;
      for (const std::string& a : checkArguments)
        line += (line.empty() ? "" : " ") + a;
      log("checks build: %s", line.c_str());
    }

    // The diagnostics in what a build printed, by file (overlayKey); lines without a place are logged.
    std::map<std::string, std::pair<fs::path, std::vector<Diagnostic>>> diagnosticsOf(const std::string& printed)
    {
      static const std::regex located("^(.*?):([0-9]+)(?::([0-9]+))?: (error|warning|note): (.*)$");
      static const std::regex quoted("\\(at '(.*)'\\)$");
      std::map<std::string, std::pair<fs::path, std::vector<Diagnostic>>> out;
      size_t start = 0;
      while (start < printed.size())
      {
        size_t end = printed.find('\n', start);
        std::string line = printed.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? printed.size() : end + 1;
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        std::smatch m;
        if (!std::regex_match(line, m, located))
        {
          if (line.find("error") != std::string::npos)
            log("%s", line.c_str());
          continue;
        }
        Diagnostic d;
        d.line = size_t(std::stoull(m[2].str()));
        d.column = m[3].matched ? size_t(std::stoull(m[3].str())) : 0;
        d.severity = m[4] == "error" ? 1 : m[4] == "warning" ? 2 : 3;
        d.message = m[5].str();
        std::smatch q;
        if (std::regex_search(d.message, q, quoted))
          d.length = q[1].length();
        fs::path path = m[1].str();
        auto& entry = out[overlayKey(path)];
        entry.first = path;
        entry.second.push_back(std::move(d));
      }
      return out;
    }

    Json diagnosticJson(const std::string& text, const std::vector<size_t>& starts, const Diagnostic& d)
    {
      size_t line = std::min(d.line == 0 ? 0 : d.line - 1, starts.size() - 1);
      size_t lineStart = starts[line];
      size_t lineEnd = line + 1 < starts.size() ? starts[line + 1] - 1 : text.size();
      if (lineEnd > lineStart && text[lineEnd - 1] == '\r')
        --lineEnd;
      size_t from = lineStart, to = lineEnd;
      if (d.column == 0)
        while (from < to && (text[from] == ' ' || text[from] == '\t'))
          ++from;
      else
      {
        from = std::min(lineStart + d.column - 1, lineEnd);
        to = from;
        if (d.length)
          to = std::min(from + d.length, lineEnd);
        else
          while (to < lineEnd && (isalnum((unsigned char)text[to]) || text[to] == '_'))
            ++to;
        if (to == from)
          to = std::min(from + 1, lineEnd);
      }
      return Json::object()
        .set("range", range(text, starts, from, to))
        .set("severity", d.severity)
        .set("source", "kppc")
        .set("message", d.message);
    }

    // Builds the program as the documents are now and publishes what changed in its diagnostics.
    void check()
    {
      dirty = false;
      cache = Cache(); // what it pointed into changes
      if (checkArguments.empty())
        return;
      auto start = Clock::now();
      std::string out, err;
      std::vector<std::string> notes;
      int code = buildInThisProcess(compile, checkArguments, out, err, &notes);
      double seconds = std::chrono::duration<double>(Clock::now() - start).count();
      ++checks;
      auto found = diagnosticsOf(err + out);
      size_t count = 0;
      std::map<std::string, std::string> now;
      for (auto& [key, entry] : found)
      {
        std::string text;
        if (auto d = documents.find(key); d != documents.end())
          text = d->second.text;
        else
          readFile(entry.first, text);
        std::vector<size_t> starts = lineStarts(text);
        Json list = Json::array();
        for (const Diagnostic& d : entry.second)
          list.push(diagnosticJson(text, starts, d));
        count += entry.second.size();
        std::string uri = uriOf(key, entry.first);
        uris[key] = uri;
        Json params = Json::object().set("uri", uri).set("diagnostics", std::move(list));
        std::string body;
        write(params, body);
        now[key] = body;
      }
      for (const auto& [key, body] : published)
        if (!now.count(key))
        {
          Json params = Json::object().set("uri", uriOf(key, key)).set("diagnostics", Json::array());
          notify("textDocument/publishDiagnostics", std::move(params));
        }
      for (const auto& [key, body] : now)
        if (published[key] != body)
        {
          JsonParser p{body};
          notify("textDocument/publishDiagnostics", p.value());
        }
      published = std::move(now);
      log("check %zu: %.3f s, exit %d, %zu diagnostics in %zu files", checks, seconds, code, count, published.size());
      for (const std::string& note : notes)
        log("  %s", note.c_str());
    }

    // ---- queries (under withKeptProgram) ----

    const std::vector<size_t>& startsOf(const ParsedFile* f)
    {
      auto found = cache.starts.find(f);
      if (found == cache.starts.end())
        found = cache.starts.emplace(f, lineStarts(f->text)).first;
      return found->second;
    }

    const std::vector<Occurrence>& occurrencesOf(const ParsedFile* f)
    {
      auto found = cache.occurrences.find(f);
      if (found == cache.occurrences.end())
        found = cache.occurrences.emplace(f, occurrencesIn(*f)).first;
      return found->second;
    }

    ParsedFile* fileOf(const KeptProgram& program, const std::string& uri)
    {
      if (!cache.filesKnown)
      {
        for (ParsedFile* f : program.files)
          if (!f->path.empty() && f->path[0] != '<')
            cache.files[overlayKey(f->path)] = f;
        cache.filesKnown = true;
      }
      auto found = cache.files.find(overlayKey(uriToPath(uri)));
      return found == cache.files.end() ? nullptr : found->second;
    }

    // The name at a request's position: its occurrence, or null.
    const Occurrence* occurrenceAt(const KeptProgram& program, const Json& params, const ParsedFile** fileOut = nullptr)
    {
      ParsedFile* f = fileOf(program, params["textDocument"]["uri"].text);
      if (!f || !f->tokens)
        return nullptr;
      if (fileOut)
        *fileOut = f;
      size_t offset = offsetOf(f->text, startsOf(f), params["position"]);
      uint32_t t = tokenAt(*f->tokens, offset);
      if (t == UINT32_MAX)
        return nullptr;
      const std::vector<Occurrence>& list = occurrencesOf(f);
      auto it = std::lower_bound(list.begin(), list.end(), t, [](const Occurrence& o, uint32_t token) { return o.token < token; });
      return it != list.end() && it->token == t ? &*it : nullptr;
    }

    Json tokenRange(const ParsedFile* f, uint32_t token)
    {
      const Token& t = (*f->tokens)[token];
      return range(f->text, startsOf(f), t.offset, size_t(t.offset) + t.length);
    }

    Json locationOf(const Decl* d)
    {
      if (!d->file || !d->file->tokens || d->file->path.empty() || d->file->path[0] == '<')
        return nullptr;
      uint32_t t = declarationNameToken(d);
      if (t == UINT32_MAX)
        t = std::min<uint32_t>(d->token, uint32_t(d->file->tokens->size() - 1));
      return Json::object().set("uri", uriOf(overlayKey(d->file->path), d->file->path)).set("range", tokenRange(d->file, t));
    }

    // One declaration for all of its declarations: a member function's in-class declaration for its
    // out-of-line definition, a class's definition for its forward declarations.
    Decl* canonical(GlobalScope& global, Decl* d)
    {
      if (d->kind == NodeKind::FunctionDecl)
        if (FunctionDecl* declared = global.declarationOf(static_cast<FunctionDecl*>(d)))
          return declared;
      if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl)
        if (Decl* defined = global.definitionOf(d))
          return defined;
      return d;
    }

    const char* kindWord(const Decl* d)
    {
      bool member = d->parent && (d->parent->kind == NodeKind::ClassDecl);
      switch (d->kind)
      {
      case NodeKind::NamespaceDecl: return "namespace";
      case NodeKind::ClassDecl: return static_cast<const ClassDecl*>(d)->keyword == Kw::class_ ? "class" : static_cast<const ClassDecl*>(d)->keyword == Kw::union_ ? "union" : "struct";
      case NodeKind::EnumDecl: return "enum";
      case NodeKind::EnumeratorDecl: return "enumerator";
      case NodeKind::FunctionDecl:
      {
        auto* f = static_cast<const FunctionDecl*>(d);
        if (!f->returnType && !f->name.empty() && f->name.last().special == 0)
          return "constructor";
        return member || f->name.parts.size() > 1 ? "member function" : "function";
      }
      case NodeKind::ParamDecl: return "parameter";
      case NodeKind::VariableDecl: return member ? "data member" : "variable";
      case NodeKind::UsingAliasDecl: return "type alias";
      case NodeKind::TemplateParamDecl: return "template parameter";
      default: return "declaration";
      }
    }

    // A declaration as written, from its first token to its body (or initializer), on one line.
    std::string declarationText(const KeptProgram& program, const Decl* d)
    {
      if (!d->file || !d->file->tokens || d->file->tokens->empty())
        return declarationName(d, *program.symbols);
      const std::vector<Token>& tokens = *d->file->tokens;
      uint32_t from = std::min<uint32_t>(d->token, uint32_t(tokens.size() - 1));
      uint32_t end = d->endToken > from ? std::min<uint32_t>(d->endToken, uint32_t(tokens.size())) : from + 1;
      if (d->kind == NodeKind::FunctionDecl && static_cast<const FunctionDecl*>(d)->body)
        end = std::min(end, static_cast<const FunctionDecl*>(d)->body->token);
      int depth = 0;
      for (uint32_t k = from; k < end; ++k)
      {
        const Token& t = tokens[k];
        if (t.getKind() != TokenKind::Punctuator)
          continue;
        P p = P(t.id);
        if (p == P::LParen || p == P::LBracket || p == P::Less)
          ++depth;
        else if (p == P::RParen || p == P::RBracket || p == P::Greater)
          --depth;
        // A class's or enum's body, an initializer, the next declarator, the end.
        if (depth <= 0 && (p == P::LBrace || p == P::Semi || ((p == P::Assign || p == P::Comma) && d->kind != NodeKind::FunctionDecl && d->kind != NodeKind::ClassDecl)))
        {
          end = k;
          break;
        }
      }
      if (end <= from)
        end = from + 1;
      std::string out;
      std::string_view text = d->file->text;
      size_t a = tokens[from].offset, b = size_t(tokens[end - 1].offset) + tokens[end - 1].length;
      bool space = false;
      for (size_t i = a; i < b && i < text.size() && out.size() < 400; ++i)
      {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
          space = !out.empty();
        else
        {
          if (space)
            out += ' ';
          space = false;
          out += c;
        }
      }
      // A variable's or parameter's declared type when it was deduced.
      if ((d->kind == NodeKind::VariableDecl || d->kind == NodeKind::ParamDecl) && out.find("auto") != std::string::npos)
        if (const Ty* ty = d->cachedType.load())
          out += "  // " + program.global->types().toString(ty, program.symbols);
      return out;
    }

    // The `//` lines right above a declaration.
    std::string commentAbove(const Decl* d)
    {
      if (!d->file || !d->file->tokens || d->token >= d->file->tokens->size())
        return {};
      std::string_view text = d->file->text;
      size_t at = (*d->file->tokens)[d->token].offset;
      size_t lineStart = text.rfind('\n', at == 0 ? 0 : at - 1);
      std::vector<std::string> lines;
      while (lineStart != std::string_view::npos && lineStart > 0)
      {
        size_t previous = text.rfind('\n', lineStart - 1);
        size_t from = previous == std::string_view::npos ? 0 : previous + 1;
        std::string_view line = text.substr(from, lineStart - from);
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos || line.substr(first, 2) != "//")
          break;
        std::string content(line.substr(first + 2));
        while (!content.empty() && (content.back() == '\r' || content.back() == ' '))
          content.pop_back();
        if (!content.empty() && content[0] == ' ')
          content.erase(0, 1);
        lines.push_back(content);
        if (previous == std::string_view::npos)
          break;
        lineStart = previous;
      }
      std::string out;
      for (auto it = lines.rbegin(); it != lines.rend(); ++it)
        out += (out.empty() ? "" : "\n") + *it;
      return out;
    }

    Json hover(const Json& params)
    {
      Json result;
      withKeptProgram([&](const KeptProgram& program)
      {
        const ParsedFile* file = nullptr;
        const Occurrence* o = occurrenceAt(program, params, &file);
        if (!o)
          return;
        Decl* d = o->decl;
        std::string value = "```kpp\n" + declarationText(program, d) + "\n```\n";
        std::string where = kindWord(d);
        std::string qualified = program.global->qualifiedName(d);
        if (!qualified.empty() && d->kind != NodeKind::ParamDecl && !(d->kind == NodeKind::VariableDecl && d->parent && d->parent->kind == NodeKind::FunctionDecl))
          where += " `" + qualified + "`";
        // An expression's type where the name is one (a function's name typed as its call's result is not shown).
        if (!o->declaration && (o->node->kind == NodeKind::NameExpr || o->node->kind == NodeKind::MemberExpr) && d->kind != NodeKind::FunctionDecl)
          if (const Ty* ty = static_cast<Expr*>(o->node)->ty)
            where += ", here of type `" + program.global->types().toString(ty, program.symbols) + "`";
        value += where;
        if (std::string comment = commentAbove(d); !comment.empty())
          value += "\n\n" + comment;
        result = Json::object()
                   .set("contents", Json::object().set("kind", "markdown").set("value", value))
                   .set("range", tokenRange(file, o->token));
      });
      return result;
    }

    // definition: a class's definition, a member function's out-of-line definition, else the
    // declaration itself; declaration: a definition's in-class declaration.
    Json definition(const Json& params, bool declaration)
    {
      Json result;
      withKeptProgram([&](const KeptProgram& program)
      {
        const Occurrence* o = occurrenceAt(program, params);
        if (!o)
          return;
        Decl* d = o->decl;
        Decl* target = d;
        if (declaration)
          target = canonical(*program.global, d);
        else if (Decl* defined = program.global->definitionOf(d))
          target = defined;
        result = locationOf(target);
      });
      return result;
    }

    // Every file's names, grouped by the canonical declaration they are (on all threads).
    void knowReferences(const KeptProgram& program)
    {
      if (cache.referencesKnown)
        return;
      std::vector<const ParsedFile*> files;
      for (const ParsedFile* f : program.files)
        if (!cache.occurrences.count(f))
          files.push_back(f);
      std::vector<std::vector<Occurrence>> found(files.size());
      std::atomic<size_t> next{0};
      std::vector<std::thread> threads;
      for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t)
        threads.emplace_back([&] { for (size_t i; (i = next++) < files.size();) found[i] = occurrencesIn(*files[i]); });
      for (std::thread& t : threads)
        t.join();
      for (size_t i = 0; i < files.size(); ++i)
        cache.occurrences.emplace(files[i], std::move(found[i]));
      std::unordered_map<Decl*, Decl*> canonicals;
      for (const ParsedFile* f : program.files)
        for (const Occurrence& o : cache.occurrences[f])
        {
          auto [it, fresh] = canonicals.emplace(o.decl, nullptr);
          if (fresh)
            it->second = canonical(*program.global, o.decl);
          cache.references[it->second].push_back({f, o.token});
        }
      cache.referencesKnown = true;
    }

    Json references(const Json& params)
    {
      Json result = Json::array();
      withKeptProgram([&](const KeptProgram& program)
      {
        const Occurrence* o = occurrenceAt(program, params);
        if (!o)
          return;
        Decl* d = canonical(*program.global, o->decl);
        knowReferences(program);
        bool withDeclarations = params["context"]["includeDeclaration"].boolean;
        auto found = cache.references.find(d);
        if (found == cache.references.end())
          return;
        for (const auto& [f, token] : found->second)
        {
          const std::vector<Occurrence>& list = cache.occurrences[f];
          auto it = std::lower_bound(list.begin(), list.end(), token, [](const Occurrence& x, uint32_t t) { return x.token < t; });
          if (!withDeclarations && it != list.end() && it->token == token && it->declaration)
            continue;
          result.push(Json::object().set("uri", uriOf(overlayKey(f->path), f->path)).set("range", tokenRange(f, token)));
        }
      });
      return result;
    }

    int symbolKind(const Decl* d)
    {
      switch (d->kind)
      {
      case NodeKind::NamespaceDecl: return 3;
      case NodeKind::ClassDecl: return static_cast<const ClassDecl*>(d)->keyword == Kw::class_ ? 5 : 23;
      case NodeKind::EnumDecl: return 10;
      case NodeKind::EnumeratorDecl: return 22;
      case NodeKind::FunctionDecl:
      {
        const char* word = kindWord(d);
        return strcmp(word, "constructor") == 0 ? 9 : strcmp(word, "member function") == 0 ? 6 : 12;
      }
      case NodeKind::VariableDecl: return d->parent && d->parent->kind == NodeKind::ClassDecl ? 8 : 13;
      case NodeKind::UsingAliasDecl: return 5;
      default: return 0;
      }
    }

    // The declarations a file's outline shows: namespaces, types, functions, variables, members.
    Span<Decl*> childrenOf(Decl* d)
    {
      switch (d->kind)
      {
      case NodeKind::NamespaceDecl: return static_cast<NamespaceDecl*>(d)->decls;
      case NodeKind::ClassDecl: return static_cast<ClassDecl*>(d)->members;
      case NodeKind::EnumDecl: return static_cast<EnumDecl*>(d)->enumerators;
      default: return {};
      }
    }

    Json outline(const KeptProgram& program, const ParsedFile* f, Span<Decl*> decls)
    {
      Json list = Json::array();
      const std::vector<Token>& tokens = *f->tokens;
      for (Decl* d : decls)
      {
        int kind = symbolKind(d);
        if (!kind || d->file != f || d->token >= tokens.size())
          continue;
        std::string name = declarationName(d, *program.symbols);
        if (name.empty())
          continue;
        uint32_t last = std::min<uint32_t>(std::max(d->endToken, d->token + 1), uint32_t(tokens.size())) - 1;
        uint32_t nameAt = declarationNameToken(d);
        Json item = Json::object()
                      .set("name", name)
                      .set("detail", declarationText(program, d))
                      .set("kind", kind)
                      .set("range", range(f->text, startsOf(f), tokens[d->token].offset, size_t(tokens[last].offset) + tokens[last].length))
                      .set("selectionRange", tokenRange(f, nameAt == UINT32_MAX ? d->token : nameAt));
        Span<Decl*> children = childrenOf(d);
        Span<Decl*> enumMembers = d->kind == NodeKind::EnumDecl ? static_cast<EnumDecl*>(d)->members : Span<Decl*>{};
        if (!children.empty() || !enumMembers.empty())
        {
          Json inner = outline(program, f, children);
          for (Json& j : outline(program, f, enumMembers).items)
            inner.push(std::move(j));
          item.set("children", std::move(inner));
        }
        list.push(std::move(item));
      }
      return list;
    }

    Json documentSymbols(const Json& params)
    {
      Json result = Json::array();
      withKeptProgram([&](const KeptProgram& program)
      {
        if (ParsedFile* f = fileOf(program, params["textDocument"]["uri"].text); f && f->tokens)
          result = outline(program, f, f->decls);
      });
      return result;
    }

    void knowSymbols(const KeptProgram& program)
    {
      if (cache.symbolsKnown)
        return;
      std::function<void(Span<Decl*>, const std::string&)> add = [&](Span<Decl*> decls, const std::string& container)
      {
        for (Decl* d : decls)
        {
          int kind = symbolKind(d);
          if (!kind)
            continue;
          std::string name = declarationName(d, *program.symbols);
          if (name.empty() || name == "(anonymous)")
            continue;
          // An out-of-line definition goes by its declaration.
          if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->name.parts.size() > 1 && program.global->declarationOf(static_cast<FunctionDecl*>(d)))
            continue;
          size_t colons = name.rfind("::");
          std::string own = colons == std::string::npos ? name : name.substr(colons + 2);
          std::string lower = own;
          for (char& c : lower)
            c = char(tolower((unsigned char)c));
          cache.symbols.push_back({own, lower, container + (colons == std::string::npos ? "" : (container.empty() ? "" : "::") + name.substr(0, colons)), kind, d});
          if (d->kind != NodeKind::EnumDecl || static_cast<EnumDecl*>(d)->isClass)
            add(childrenOf(d), (container.empty() ? "" : container + "::") + name);
          else
            add(childrenOf(d), container); // an unscoped enumerator is named without its enum
        }
      };
      for (ParsedFile* f : program.files)
        add(f->decls, "");
      cache.symbolsKnown = true;
    }

    Json workspaceSymbols(const Json& params)
    {
      Json result = Json::array();
      std::string query = params["query"].text;
      for (char& c : query)
        c = char(tolower((unsigned char)c));
      withKeptProgram([&](const KeptProgram& program)
      {
        knowSymbols(program);
        // Exact names first, then prefixes, substrings and the query's letters in order; shorter first.
        std::vector<std::pair<std::pair<int, size_t>, const Cache::Symbol*>> ranked;
        for (const Cache::Symbol& s : cache.symbols)
        {
          int rank;
          if (s.lower == query)
            rank = 0;
          else if (s.lower.compare(0, query.size(), query) == 0)
            rank = 1;
          else if (s.lower.find(query) != std::string::npos)
            rank = 2;
          else
          {
            size_t q = 0;
            for (char c : s.lower)
              if (q < query.size() && c == query[q])
                ++q;
            if (q < query.size())
              continue;
            rank = 3;
          }
          ranked.push_back({{rank, s.name.size()}, &s});
        }
        size_t shown = std::min<size_t>(ranked.size(), 200);
        std::partial_sort(ranked.begin(), ranked.begin() + shown, ranked.end(), [](const auto& a, const auto& b)
        {
          return a.first != b.first ? a.first < b.first : a.second->name != b.second->name ? a.second->name < b.second->name : a.second->container < b.second->container;
        });
        for (size_t k = 0; k < shown; ++k)
        {
          const Cache::Symbol& s = *ranked[k].second;
          Json location = locationOf(s.decl);
          if (location.kind == Json::Null)
            continue;
          Json item = Json::object().set("name", s.name).set("kind", s.kind).set("location", std::move(location));
          if (!s.container.empty())
            item.set("containerName", s.container);
          result.push(std::move(item));
        }
      });
      return result;
    }

    void ensureChecked()
    {
      if (dirty)
        check();
    }

    void open(const Json& params)
    {
      const Json& item = params["textDocument"];
      Document d;
      d.uri = item["uri"].text;
      d.path = uriToPath(d.uri);
      d.text = item["text"].text;
      d.version = item["version"].integer();
      std::string key = overlayKey(d.path);
      setFileOverlay(d.path, &d.text);
      documents[key] = std::move(d);
      dirty = true;
    }

    void change(const Json& params)
    {
      fs::path path = uriToPath(params["textDocument"]["uri"].text);
      auto found = documents.find(overlayKey(path));
      if (found == documents.end())
      {
        log("a change of %s, which is not open, ignored", params["textDocument"]["uri"].text.c_str());
        return;
      }
      Document& d = found->second;
      for (const Json& c : params["contentChanges"].items)
      {
        if (c["range"].kind != Json::Object)
        {
          d.text = c["text"].text;
          continue;
        }
        std::vector<size_t> starts = lineStarts(d.text);
        size_t from = offsetOf(d.text, starts, c["range"]["start"]);
        size_t to = std::max(from, offsetOf(d.text, starts, c["range"]["end"]));
        d.text.replace(from, to - from, c["text"].text);
      }
      d.version = params["textDocument"]["version"].integer();
      setFileOverlay(d.path, &d.text);
      dirty = true;
    }

    void close(const Json& params)
    {
      fs::path path = uriToPath(params["textDocument"]["uri"].text);
      std::string key = overlayKey(path);
      if (documents.erase(key))
      {
        setFileOverlay(path, nullptr); // the file on disk again
        dirty = true;
      }
    }

    Json capabilities()
    {
      return Json::object()
        .set("positionEncoding", "utf-16")
        .set("textDocumentSync", Json::object()
                                   .set("openClose", true)
                                   .set("change", 2) // incremental
                                   .set("save", Json::object().set("includeText", false)))
        .set("hoverProvider", true)
        .set("definitionProvider", true)
        .set("declarationProvider", true)
        .set("referencesProvider", true)
        .set("documentSymbolProvider", true)
        .set("workspaceSymbolProvider", true);
    }

    // One message; false when the server is to end (exit).
    bool handle(const Json& m, const char* self)
    {
      const std::string& method = m["method"].text;
      const Json& id = m["id"];
      bool request = id.kind != Json::Null;
      const Json& params = m["params"];
      if (method.empty())
        return true; // a response to a request of ours (none are sent that want one)
      if (!initialized && method != "initialize" && method != "exit")
      {
        if (request)
          respondError(id, -32002, "the server is not initialized");
        return true;
      }
      if (method == "initialize")
      {
        fs::path workspace;
        if (params["workspaceFolders"].kind == Json::Array && !params["workspaceFolders"].items.empty())
          workspace = uriToPath(params["workspaceFolders"].items[0]["uri"].text);
        else if (params["rootUri"].kind == Json::String)
          workspace = uriToPath(params["rootUri"].text);
        else if (params["rootPath"].kind == Json::String)
          workspace = params["rootPath"].text;
        chooseArguments(workspace, self);
        initialized = true;
        respond(id, Json::object()
                      .set("capabilities", capabilities())
                      .set("serverInfo", Json::object().set("name", "kppc").set("version", "0.1")));
        return true;
      }
      if (method == "initialized")
      {
        dirty = true; // the first check: the whole program, before any document opens
        return true;
      }
      if (method == "shutdown")
      {
        shutdownRequested = true;
        respond(id, nullptr);
        return true;
      }
      if (method == "exit")
        return false;
      if (method == "textDocument/didOpen")
        open(params);
      else if (method == "textDocument/didChange")
        change(params);
      else if (method == "textDocument/didClose")
        close(params);
      else if (method == "textDocument/didSave")
        dirty = true; // files changed on disk by others are read again too
      else if (method == "textDocument/hover")
        respond(id, hover(params));
      else if (method == "textDocument/definition")
        respond(id, definition(params, false));
      else if (method == "textDocument/declaration")
        respond(id, definition(params, true));
      else if (method == "textDocument/references")
        respond(id, references(params));
      else if (method == "textDocument/documentSymbol")
        respond(id, documentSymbols(params));
      else if (method == "workspace/symbol")
        respond(id, workspaceSymbols(params));
      else if (request)
        respondError(id, -32601, "kppc --lsp does not support " + method);
      return true;
    }
  };
}

int runLanguageServer(int argc, char** argv, CompileFunction compile)
{
  Server server;
  server.compile = compile;
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];
    if (arg == "--lsp")
      continue;
    if (arg == "--check-each-change")
      server.checkEachChange = true;
    else
      server.explicitArguments.push_back(arg);
  }
  // The protocol has stdin and stdout to itself: the build's prints go to NUL (and are captured
  // per build: buildInThisProcess), this server's log to the stderr it was started with.
  HANDLE process = GetCurrentProcess();
  DuplicateHandle(process, GetStdHandle(STD_INPUT_HANDLE), process, &input, 0, FALSE, DUPLICATE_SAME_ACCESS);
  DuplicateHandle(process, GetStdHandle(STD_OUTPUT_HANDLE), process, &output, 0, FALSE, DUPLICATE_SAME_ACCESS);
  DuplicateHandle(process, GetStdHandle(STD_ERROR_HANDLE), process, &logHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
  fflush(stdout);
  fflush(stderr);
  int nul = _open("NUL", _O_RDWR | _O_BINARY);
  _dup2(nul, 0);
  _dup2(nul, 1);
  _dup2(nul, 2);
  _close(nul);
  SetStdHandle(STD_INPUT_HANDLE, HANDLE(_get_osfhandle(0)));
  SetStdHandle(STD_OUTPUT_HANDLE, HANDLE(_get_osfhandle(1)));
  SetStdHandle(STD_ERROR_HANDLE, HANDLE(_get_osfhandle(2)));

  std::thread([]
  {
    Reader reader;
    std::string body;
    while (reader.message(body))
    {
      JsonParser parser{body};
      Json m = parser.value();
      if (!parser.ok)
      {
        log("a message that is no JSON, ignored: %.200s", body.c_str());
        continue;
      }
      inbox.put(std::move(m));
    }
    inbox.end();
  }).detach();

  Json m;
  while (inbox.take(m))
  {
    bool request = m["id"].kind != Json::Null && !m["method"].text.empty();
    const std::string& method = m["method"].text;
    // A request is answered from the program as the documents are now.
    if (request && method != "initialize" && method != "shutdown")
      server.ensureChecked();
    if (!server.handle(m, argv[0]))
      break;
    // Changes that came together are checked together; a check runs once no message waits.
    if (server.dirty && (server.checkEachChange || inbox.empty()))
      server.check();
  }
  int code = server.shutdownRequested ? 0 : 1;
  log("exit %d after %zu checks", code, server.checks);
  _exit(code); // the kept program is not freed (seconds on a large program)
}
