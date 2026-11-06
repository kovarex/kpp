#include "lsp.hpp"
#include "Ast/Ast.hpp"
#include "Ast/AstWalk.hpp"
#include "resolver.hpp"
#include "TypeTable.hpp"
#include "project.hpp"
#include "source.hpp"
#include "LineStarts.hpp"
#include <chrono>
#include <algorithm>
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
#include <tuple>
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

#include "Lsp/Json.hpp"

#include "Lsp/JsonParser.hpp"

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
  HANDLE logFile = INVALID_HANDLE_VALUE; // --lsp-log FILE
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
    if (logFile != INVALID_HANDLE_VALUE)
      writeHandle(logFile, std::string(text) + "\n");
  }

  void send(const Json& message)
  {
    std::string body;
    write(message, body);
    std::lock_guard lock(outputMutex);
    writeHandle(output, "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  }

#include "Lsp/Reader.hpp"

#include "Lsp/Inbox.hpp"
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

  Json position(std::string_view text, const LineStarts& starts, size_t offset)
  {
    size_t line = starts.index(offset);
    return Json::object().set("line", line).set("character", utf16Units(text, starts[line], offset));
  }

  Json range(std::string_view text, const LineStarts& starts, size_t from, size_t to)
  {
    return Json::object().set("start", position(text, starts, from)).set("end", position(text, starts, to));
  }

  // The byte offset of an LSP position (clamped to its line).
  size_t offsetOf(std::string_view text, const LineStarts& starts, const Json& position)
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

  // The token of each part of a qualified name, found in turn from `from` (UINT32_MAX for a part
  // without one: `::`, an operator).
  std::vector<uint32_t> partTokens(const std::vector<Token>& tokens, const QualifiedName& name, uint32_t from, uint32_t end)
  {
    std::vector<uint32_t> out;
    end = std::min<uint32_t>(end, uint32_t(tokens.size()));
    for (const NamePart& part : name.parts)
    {
      uint32_t k = from;
      if (part.symbol != UINT32_MAX && part.special < 2)
        while (k < end && !sameIdentifier(tokens[k], part.symbol))
          ++k;
      else
        k = end;
      out.push_back(k < end ? k : UINT32_MAX);
      if (k < end)
        from = k + 1;
    }
    return out;
  }

  // The namespace, class or enum a qualifier names: the nearest enclosing one of start so named.
  Decl* scopeNamed(Decl* start, uint32_t symbol)
  {
    for (Decl* d = start; d; d = d->parent)
      if ((d->kind == NodeKind::NamespaceDecl || d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl) && symbolOf(d) == symbol)
        return d;
    return nullptr;
  }

  // A qualified name's qualifiers as occurrences of the scopes they name (`Shape` in `Shape::area`),
  // the scopes found from the declaration the whole name is (and its class, for an out-of-line member).
  void addQualifiers(std::vector<Occurrence>& out, const std::vector<Token>& tokens, const QualifiedName& name, Node* node, Decl* named,
                     GlobalScope* global, uint32_t from, uint32_t end)
  {
    if (name.parts.size() < 2 || !named)
      return;
    std::vector<uint32_t> at = partTokens(tokens, name, from, end);
    Decl* start = named->parent;
    if (global && (named->kind == NodeKind::FunctionDecl || named->kind == NodeKind::VariableDecl))
      if (Decl* cls = global->owningClass(named))
        start = cls;
    for (size_t i = name.parts.size() - 1; i-- > 0;)
    {
      if (at[i] == UINT32_MAX)
        continue;
      Decl* scope = scopeNamed(start, name.parts[i].symbol);
      if (!scope)
        break; // (an alias, a using: left to the rename's check, which refuses what it cannot account for)
      out.push_back({at[i], scope, node, false});
      start = scope->parent;
    }
  }

  // A constructor's member initializers: each names a data member of its class, or a base class.
  void memberInitializers(std::vector<Occurrence>& out, const std::vector<Token>& tokens, FunctionDecl* f, GlobalScope& global)
  {
    if (f->ctorInits.empty())
      return;
    Decl* cls = global.owningClass(f);
    if (cls)
      if (Decl* defined = global.definitionOf(cls))
        cls = defined;
    if (!cls || cls->kind != NodeKind::ClassDecl)
      return;
    auto* c = static_cast<ClassDecl*>(cls);
    uint32_t from = f->token, end = f->body ? f->body->token : f->endToken;
    for (const FunctionDecl::CtorInit& init : f->ctorInits)
    {
      if (init.member.empty())
        continue;
      uint32_t symbol = init.member.last().symbol;
      Decl* named = nullptr;
      for (Decl* m : c->members)
        if (!named && m->kind == NodeKind::VariableDecl && symbolOf(m) == symbol)
          named = m;
      for (const ClassDecl::Base& b : c->bases)
        if (!named && b.type && b.type->kind == NodeKind::NamedType)
          if (Decl* base = static_cast<NamedType*>(b.type)->resolved; base && symbolOf(base) == symbol)
            named = base;
      // Searched from the `:` on: the parameters come first and may be named as the members.
      uint32_t colon = from;
      while (colon < end && !(tokens[colon].is(P::Colon) && colon + 1 < tokens.size() && tokens[colon + 1].getKind() == TokenKind::Identifier))
        ++colon;
      for (uint32_t k = colon; named && k < end; ++k)
        if (sameIdentifier(tokens[k], symbol) && k + 1 < end && (tokens[k + 1].is(P::LParen) || tokens[k + 1].is(P::LBrace) || tokens[k + 1].is(P::Less)))
        {
          out.push_back({k, named, f, false});
          break;
        }
    }
  }

  // Every name in a file the compiler resolved, and every declaration's own name, by token.
  std::vector<Occurrence> occurrencesIn(const ParsedFile& file, GlobalScope* global)
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
          {
            out.push_back({t, d, n, true});
            if (const QualifiedName* q = qualifiedNameOf(d))
              addQualifiers(out, tokens, *q, n, d, global, d->token, t + 1);
          }
        if (d->kind == NodeKind::FunctionDecl && global && d->file == &file)
          memberInitializers(out, tokens, static_cast<FunctionDecl*>(d), *global);
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
        if (t != UINT32_MAX)
          addQualifiers(out, tokens, q, n, d, global, n->token, t + 1);
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
    // The cells checked before (a target and a configuration of build.kpp's), each with the build it
    // kept, so that switching back to one is incremental; the active cell's build is the process's
    // (main.cpp). At most keptCells in all, the least recently used ones dropped.
    struct Cell
    {
      std::shared_ptr<KeptState> kept;
      std::vector<std::string> arguments; // what it was built with: kept for those only
      size_t used = 0;
    };

    // What the queries keep about the kept program, made when first asked and forgotten at every
    // check (the program changes in place).
    struct Cache
    {
      struct Symbol
      {
        std::string name, lower, container;
        int kind;
        Decl* decl;
      };

      bool filesKnown = false;
      std::map<std::string, ParsedFile*> files;                          // by overlayKey
      std::unordered_map<const ParsedFile*, std::vector<Occurrence>> occurrences;
      std::unordered_map<const ParsedFile*, LineStarts> starts;
      bool referencesKnown = false;
      std::unordered_map<const Decl*, std::vector<std::pair<const ParsedFile*, uint32_t>>> references; // by canonical declaration
      std::vector<Symbol> symbols;
      bool symbolsKnown = false;
    };


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
    // The keys of initializationOptions, or of didChangeConfiguration's `kpp` (docs/editors.md), into
    // the cell wanted. `executable` is the old name of `target` (a target named in both: target's);
    // null in both is every target.
    void readOptions(const Json& options, ProjectCheck& wanted)
    {
      if (options.kind != Json::Object)
        return;
      if (options["configuration"].kind == Json::String)
        wanted.configuration = options["configuration"].text;
      bool targetSent = false;
      std::string target;
      for (const auto& [name, value] : options.members)
        if (name == "executable" || name == "target")
        {
          targetSent = true;
          if (value.kind == Json::String && (target.empty() || name == "target"))
            target = value.text;
        }
      if (targetSent)
        wanted.target = target;
      if (options["keptCells"].kind == Json::Number && options["keptCells"].number >= 1)
        keptCells = size_t(options["keptCells"].number);
      if (options["settings"].kind == Json::Object)
      {
        wanted.settings.clear();
        for (const auto& [name, value] : options["settings"].members)
        {
          std::string text;
          if (value.kind == Json::String)
            text = value.text;
          else
            write(value, text);
          wanted.settings.push_back({name, text});
        }
      }
      if (options["buildFile"].kind == Json::String)
      {
        fs::path fromUri = uriToPath(options["buildFile"].text);
        wanted.buildFile = fromUri.empty() ? options["buildFile"].text : fromUri.string();
      }
    }

    static std::string cellKey(const ProjectCheck& c) { return c.target + "\n" + c.configuration; }

    std::string cellName(const ProjectCheck& c) const
    {
      return (c.target.empty() ? std::string("every target") : c.target) + ", " + c.configuration;
    }

    // The cell wanted as build.kpp names it: its configuration by name, or by its Visual Studio name
    // (Visual Studio sends its solution configuration, `Debug|x64`), any case; a target and a
    // configuration build.kpp does not have are an error.
    bool resolveCell(ProjectCheck& wanted, const std::vector<ProjectCell>& cells, std::string& error)
    {
      std::string configuration = wanted.configuration.substr(0, wanted.configuration.find('|'));
      std::string found, configurations, targets;
      for (const ProjectCell& c : cells)
        if (c.configuration == configuration)
          found = c.configuration;
      for (const ProjectCell& c : cells)
        if (found.empty() && (_stricmp(c.visualStudio.c_str(), configuration.c_str()) == 0 || _stricmp(c.configuration.c_str(), configuration.c_str()) == 0))
          found = c.configuration;
      bool targetFound = wanted.target.empty(), pairFound = wanted.target.empty() && !found.empty();
      for (const ProjectCell& c : cells)
      {
        if (configurations.find(" " + c.configuration + ",") == std::string::npos)
          configurations += " " + c.configuration + ",";
        if (targets.find(" " + c.target + ",") == std::string::npos)
          targets += " " + c.target + ",";
        targetFound = targetFound || c.target == wanted.target;
        pairFound = pairFound || (c.target == wanted.target && c.configuration == found);
      }
      if (!configurations.empty())
        configurations.pop_back();
      if (!targets.empty())
        targets.pop_back();
      if (found.empty())
        error = "build.kpp has no configuration " + wanted.configuration + " (its configurations:" + configurations + ")";
      else if (!targetFound)
        error = "build.kpp has no target " + wanted.target + " (its targets:" + targets + ")";
      else if (!pairFound)
        error = "build.kpp has no configuration " + found + " of " + wanted.target;
      if (!error.empty())
        return false;
      wanted.configuration = found;
      return true;
    }

    // The active cell becomes key, checked with checkArguments: the active one's kept build is parked,
    // the new one's put back when it was made with these arguments (a change of settings or of
    // build.kpp's description builds it from scratch).
    void enterCell(const std::string& key)
    {
      if (key == activeCell && checkArguments == activeArguments)
        return;
      std::shared_ptr<KeptState> current = takeKeptState();
      if (key != activeCell && current && !activeArguments.empty())
        parkedCells[activeCell] = Cell{std::move(current), activeArguments, ++cellClock};
      else if (key == activeCell)
        log("the arguments of the cell changed: it is checked from scratch");
      current.reset(); // (freed here, if not parked)
      if (auto c = parkedCells.find(key); c != parkedCells.end())
      {
        if (c->second.arguments == checkArguments)
          putKeptState(std::move(c->second.kept));
        else
          log("the arguments of the cell changed since it was checked: it is checked from scratch");
        parkedCells.erase(c);
      }
      while (!parkedCells.empty() && parkedCells.size() + 1 > keptCells)
      {
        auto oldest = parkedCells.begin();
        for (auto c = parkedCells.begin(); c != parkedCells.end(); ++c)
          if (c->second.used < oldest->second.used)
            oldest = c;
        log("the kept build of %s is dropped (keptCells %zu)", oldest->first.substr(0, oldest->first.find('\n')).c_str(), keptCells);
        parkedCells.erase(oldest);
      }
      if (key != activeCell)
        cache = Cache();
      activeCell = key;
      activeArguments = checkArguments;
      dirty = true;
    }

    // What a check builds becomes wanted's cell (or the given sources, or the folder). False, with
    // error set, when build.kpp does not have that cell or does not describe the build: nothing
    // changes then.
    bool chooseArguments(const char* self, ProjectCheck wanted, std::string& error)
    {
      std::vector<std::string> args;
      std::string key;
      if (!explicitArguments.empty())
      {
        args.push_back(self);
        args.insert(args.end(), explicitArguments.begin(), explicitArguments.end());
      }
      else if (!workspace.empty() || !wanted.buildFile.empty())
      {
        std::vector<ProjectCell> cells;
        if (projectCells(workspace, wanted, cells, error))
        {
          if (!error.empty() || !resolveCell(wanted, cells, error) || (projectCheckArguments(workspace, wanted, args, error), !error.empty()))
          {
            log("%s", error.c_str());
            return false;
          }
          project = wanted;
          key = cellKey(project);
          // build.kpp's folder, found as kpp finds it (Visual Studio's root is a generated solution's
          // folder below it): hover gives the paths in it from there.
          projectFolder.clear();
          if (!wanted.buildFile.empty())
            projectFolder = fs::path(wanted.buildFile).parent_path();
          else
            for (fs::path at = workspace; !at.empty(); at = at.parent_path())
            {
              std::error_code ec;
              if (fs::exists(at / "build.kpp", ec))
              {
                projectFolder = at;
                break;
              }
              if (at == at.parent_path())
                break;
            }
          activeOptions.clear();
          for (const ProjectCell& c : cells)
            if (c.configuration == project.configuration && (project.target.empty() || c.target == project.target))
              for (const ProjectOption& o : c.options)
              {
                bool known = false;
                for (const ProjectOption& k : activeOptions)
                  known = known || (k.library == o.library && k.name == o.name);
                if (!known)
                  activeOptions.push_back(o);
              }
          log("the project of %s (build.kpp), %s", (projectFolder.empty() ? workspace : projectFolder).string().c_str(), cellName(project).c_str());
        }
        else
        {
          project = wanted;
          activeOptions.clear();
          args = {self, workspace.string()};
          log("no build.kpp at or above %s: its files are the program", workspace.string().c_str());
        }
      }
      else
      {
        log("no workspace folder and no sources given: nothing is checked");
        checkArguments.clear();
        enterCell("");
        return true;
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
      enterCell(key);
      return true;
    }

    Json cellJson(const ProjectCheck& c)
    {
      return Json::object().set("target", c.target.empty() ? Json(nullptr) : Json(c.target)).set("configuration", c.configuration);
    }

    // kpp/cells: build.kpp's cells and the active one (none without a build.kpp).
    Json cellList()
    {
      Json list = Json::array();
      std::vector<ProjectCell> cells;
      std::string error;
      bool isProject = explicitArguments.empty() && projectCells(workspace, project, cells, error) && error.empty();
      if (!error.empty())
        log("kpp/cells: %s", error.c_str());
      for (const ProjectCell& c : cells)
      {
        Json libraries = Json::array(); // its kpp libraries, each scoped (NAME::x) or unscoped (design.md 16)
        for (const ProjectLibrary& l : c.libraries)
          libraries.push(Json::object().set("name", l.name).set("unscoped", l.unscoped));
        list.push(Json::object().set("target", c.target).set("configuration", c.configuration).set("visualStudio", c.visualStudio)
                    .set("libraries", std::move(libraries)));
      }
      return Json::object().set("cells", std::move(list)).set("active", isProject && !activeCell.empty() ? cellJson(project) : Json(nullptr));
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

    Json diagnosticJson(const std::string& text, const LineStarts& starts, const Diagnostic& d)
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
        .set("source", "kpp")
        .set("message", d.message);
    }

    // Builds the program as the documents are now and publishes what changed in its diagnostics.
    void check()
    {
      dirty = false;
      cache = Cache(); // what it pointed into changes
      if (!activeCell.empty() && announcedCell != activeCell)
      {
        notify("kpp/activeCell", cellJson(project));
        announcedCell = activeCell;
      }
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
        LineStarts starts(text);
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
      // A check that fails without a diagnostic (an argument it does not take, a library it cannot
      // find) would leave the editor showing a clean program: it is told, once per failure.
      std::string failure;
      if (code != 0 && count == 0)
      {
        std::string text = err + out;
        size_t from = text.find_first_not_of(" \r\n");
        failure = from == std::string::npos ? "exit " + std::to_string(code)
                                            : text.substr(from, text.find_first_of("\r\n", from) - from);
      }
      if (failure != shownFailure)
      {
        if (!failure.empty())
          showMessage(1, "kpp: the check failed with no diagnostic: " + failure);
        shownFailure = failure;
      }
      log("check %zu: %.3f s, exit %d, %zu diagnostics in %zu files", checks, seconds, code, count, published.size());
      for (const std::string& note : notes)
        log("  %s", note.c_str());
      sendInactiveRegions();
      // Semantic tokens are of the checked program: a client asks for them again after each check
      // (it may have asked before the first one, when there was nothing to answer).
      if (semanticTokensRefresh)
        send(Json::object().set("jsonrpc", "2.0").set("id", "kpp-refresh-" + std::to_string(++refreshes)).set("method", "workspace/semanticTokens/refresh"));
      if (semanticColors)
        notify("kpp/semanticTokensChanged", Json::object());
    }

    // ---- inactive code (docs/editors.md, "Inactive code") ----

    // One past a statement's last token: a block's closing brace, else the `;` that ends it.
    uint32_t statementEnd(const ParsedFile& f, Stmt* s)
    {
      const std::vector<Token>& tokens = *f.tokens;
      uint32_t n = uint32_t(tokens.size());
      if (s->token < n && tokens[s->token].is(P::LBrace))
      {
        int depth = 0;
        for (uint32_t t = s->token; t < n; ++t)
          if (tokens[t].is(P::LBrace))
            ++depth;
          else if (tokens[t].is(P::RBrace) && --depth == 0)
            return t + 1;
        return n;
      }
      uint32_t last = s->token;
      forEachNode(s, [&](Node* node) { last = std::max(last, node->token); });
      int depth = 0;
      for (uint32_t t = last; t < n; ++t)
      {
        if (tokens[t].is(P::LParen) || tokens[t].is(P::LBrace) || tokens[t].is(P::LBracket))
          ++depth;
        else if (tokens[t].is(P::RParen) || tokens[t].is(P::RBrace) || tokens[t].is(P::RBracket))
          --depth;
        else if (tokens[t].is(P::Semi) && depth <= 0)
          return t + 1;
      }
      return last + 1;
    }

    // The tokens [from, to) of a file its cell does not build: what the parser dropped (a false
    // `when` at namespace or class scope, a declaration-scope `if constexpr` branch not taken), and
    // what the walk decided in function bodies: an `if constexpr`'s discarded branch and a local's
    // false `when`. A template's own body is decided per instance and none of it is shown.
    std::vector<std::pair<uint32_t, uint32_t>> inactiveTokens(const ParsedFile& f)
    {
      std::vector<std::pair<uint32_t, uint32_t>> out = f.droppedTokens;
      const std::vector<Token>& tokens = *f.tokens;
      forEachNode(f.decls, [&](Node* n)
      {
        if (n->kind == NodeKind::IfStmt)
        {
          auto* s = static_cast<IfStmt*>(n);
          Stmt* discarded = !s->isConstexpr || s->constexprKnown < 0 ? nullptr : s->constexprKnown ? s->else_ : s->then;
          if (discarded)
            out.push_back({discarded->token, statementEnd(f, discarded)});
        }
        else if (n->kind == NodeKind::VariableDecl || n->kind == NodeKind::UsingAliasDecl || n->kind == NodeKind::ClassDecl || n->kind == NodeKind::EnumDecl)
        {
          // A local's false `when` (what a block may declare with one), from `when`: the declaration's
          // tokens begin after its parenthesized condition.
          auto* d = static_cast<Decl*>(n);
          if (!d->when || d->whenKnown != 0)
            return;
          uint32_t from = d->token;
          if (from > 0 && tokens[from - 1].is(P::RParen))
          {
            int depth = 0;
            for (uint32_t t = from; t-- > 0;)
              if (tokens[t].is(P::RParen))
                ++depth;
              else if (tokens[t].is(P::LParen) && --depth == 0)
              {
                if (t > 0 && tokens[t - 1].is(Kw::when_))
                  from = t - 1;
                break;
              }
          }
          out.push_back({from, std::max(d->endToken, d->token + 1)});
        }
      });
      std::sort(out.begin(), out.end());
      std::vector<std::pair<uint32_t, uint32_t>> merged;
      for (const auto& r : out)
        if (!merged.empty() && r.first <= merged.back().second)
          merged.back().second = std::max(merged.back().second, r.second);
        else
          merged.push_back(r);
      return merged;
    }

    // textDocument/inactiveRegions for each open document whose regions changed (an empty list when
    // they went), to a client that takes them.
    void sendInactiveRegions()
    {
      if (!inactiveRegions)
        return;
      auto start = Clock::now();
      std::vector<std::pair<std::string, Json>> changed;
      onProgram([&](const KeptProgram& program)
      {
        for (const auto& [key, document] : documents)
        {
          Json regions = Json::array();
          if (ParsedFile* f = fileOf(program, document.uri); f && f->tokens && !f->tokens->empty())
          {
            const std::vector<Token>& tokens = *f->tokens;
            for (const auto& [from, to] : inactiveTokens(*f))
            {
              if (from >= to || to > tokens.size())
                continue;
              const Token& last = tokens[to - 1];
              regions.push(range(f->text, startsOf(f), tokens[from].offset, size_t(last.offset) + last.length));
            }
          }
          std::string body;
          write(regions, body);
          if (inactiveSent[key] != body)
          {
            inactiveSent[key] = body;
            changed.push_back({document.uri, std::move(regions)});
          }
        }
      });
      log("inactive regions: %zu open document(s), %zu changed, %.3f ms", documents.size(), changed.size(),
          std::chrono::duration<double, std::milli>(Clock::now() - start).count());
      for (auto& [uri, regions] : changed)
        notify("textDocument/inactiveRegions",
               Json::object().set("textDocument", Json::object().set("uri", uri)).set("regions", std::move(regions)));
    }

    // ---- queries (under withKeptProgram) ----

    template<typename F>
    void onProgram(F&& f)
    {
      withKeptProgram([&](const KeptProgram& program)
      {
        currentGlobal = program.global;
        f(program);
        currentGlobal = nullptr;
      });
    }

    const LineStarts& startsOf(const ParsedFile* f)
    {
      auto found = cache.starts.find(f);
      if (found == cache.starts.end())
        found = cache.starts.emplace(f, LineStarts(f->text)).first;
      return found->second;
    }

    const std::vector<Occurrence>& occurrencesOf(const ParsedFile* f)
    {
      auto found = cache.occurrences.find(f);
      if (found == cache.occurrences.end())
        found = cache.occurrences.emplace(f, occurrencesIn(*f, currentGlobal)).first;
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

    // Hover's code and emphasis: markdown's, or the text alone for a client of plain text.
    std::string code(const std::string& text) { return markdownHover ? "`" + text + "`" : text; }
    std::string emphasis(const std::string& text) { return markdownHover ? "*" + text + "*" : text; }

    // A config:: constant in the checked cell: a library option's value, who set it, its default
    // and description (build.kpp's description, ProjectCell::options); another constant's cell.
    std::string configNote(const std::string& name)
    {
      std::string cell = "the cell " + emphasis(cellName(project));
      for (const ProjectOption& o : activeOptions)
      {
        if (o.library + "::" + o.name != name)
          continue;
        std::string setBy = o.setBy; // (build.kpp's path in it from the project's folder)
        std::string folder = (projectFolder.empty() ? workspace : projectFolder).string() + "\\";
        if (auto at = std::search(setBy.begin(), setBy.end(), folder.begin(), folder.end(), [](char x, char y) { return tolower((unsigned char)x) == tolower((unsigned char)y); });
            !folder.empty() && at != setBy.end())
          setBy.erase(at, at + folder.size());
        std::string note = code(o.value) + " in " + cell + ", set by " + setBy + "; ";
        note += o.required ? "required" : "default " + code(o.defaultValue);
        note += ", " + o.type;
        if (!o.allowed.empty())
        {
          note += ", one of";
          for (size_t i = 0; i < o.allowed.size(); ++i)
            note += (i ? ", " : " ") + code(o.allowed[i]);
        }
        note += ". An option of the library " + o.library + ".";
        if (!o.description.empty())
          note += "\n\n" + o.description;
        return note;
      }
      return "As in " + cell + ".";
    }

    Json hover(const Json& params)
    {
      Json result;
      onProgram([&](const KeptProgram& program)
      {
        const ParsedFile* file = nullptr;
        const Occurrence* o = occurrenceAt(program, params, &file);
        if (!o)
          return;
        Decl* d = o->decl;
        std::string value = markdownHover ? "```kpp\n" + declarationText(program, d) + "\n```\n" : declarationText(program, d) + "\n";
        std::string where = kindWord(d);
        std::string qualified = program.global->qualifiedName(d);
        if (!qualified.empty() && d->kind != NodeKind::ParamDecl && !(d->kind == NodeKind::VariableDecl && d->parent && d->parent->kind == NodeKind::FunctionDecl))
          where += " " + code(qualified);
        // An expression's type where the name is one (a function's name typed as its call's result is not
        // shown, nor a qualifier's: `Entity` in `Entity::count` is the class, the type is count's).
        if (!o->declaration && (o->node->kind == NodeKind::NameExpr || o->node->kind == NodeKind::MemberExpr) && d->kind != NodeKind::FunctionDecl &&
            d->kind != NodeKind::NamespaceDecl && d->kind != NodeKind::ClassDecl && d->kind != NodeKind::EnumDecl)
          if (const Ty* ty = static_cast<Expr*>(o->node)->ty)
            where += ", here of type " + code(program.global->types().toString(ty, program.symbols));
        value += where;
        if (qualified.rfind("config::", 0) == 0 && !activeCell.empty())
          value += "\n\n" + configNote(qualified.substr(8));
        if (std::string comment = commentAbove(d); !comment.empty())
          value += "\n\n" + comment;
        result = Json::object()
                   .set("contents", Json::object().set("kind", markdownHover ? "markdown" : "plaintext").set("value", value))
                   .set("range", tokenRange(file, o->token));
      });
      return result;
    }

    // definition: a class's definition, a member function's out-of-line definition, else the
    // declaration itself; declaration: a definition's in-class declaration.
    Json definition(const Json& params, bool declaration)
    {
      Json result;
      onProgram([&](const KeptProgram& program)
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
        threads.emplace_back([&] { for (size_t i; (i = next++) < files.size();) found[i] = occurrencesIn(*files[i], program.global); });
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
      onProgram([&](const KeptProgram& program)
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
      onProgram([&](const KeptProgram& program)
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
      // A scoped library's top-level names are its name's (design.md 16: `json::parseJson`).
      for (ParsedFile* f : program.files)
        add(f->decls, f->libraryScope != UINT32_MAX ? f->library : std::string());
      cache.symbolsKnown = true;
    }

    // ---- completion ----

    // The function whose tokens hold `token`, searched through namespaces and classes.
    FunctionDecl* enclosingFunction(Span<Decl*> decls, uint32_t token)
    {
      for (Decl* d : decls)
      {
        if (token < d->token || (d->endToken && token >= d->endToken))
          continue;
        if (d->kind == NodeKind::FunctionDecl)
          return static_cast<FunctionDecl*>(d);
        if (d->kind == NodeKind::NamespaceDecl || d->kind == NodeKind::ClassDecl)
          if (FunctionDecl* f = enclosingFunction(childrenOf(d), token))
            return f;
      }
      return nullptr;
    }

    // A class's definition with its bases', most derived first.
    void classesOf(GlobalScope& global, Decl* c, std::vector<ClassDecl*>& out, int depth = 0)
    {
      if (!c || depth > 32)
        return;
      if (Decl* defined = global.definitionOf(c))
        c = defined;
      if (c->kind != NodeKind::ClassDecl || std::find(out.begin(), out.end(), c) != out.end())
        return;
      auto* cls = static_cast<ClassDecl*>(c);
      out.push_back(cls);
      for (const ClassDecl::Base& b : cls->bases)
        if (b.type && b.type->kind == NodeKind::NamedType)
          classesOf(global, static_cast<NamedType*>(b.type)->resolved, out, depth + 1);
    }

    // The type a declaration gives a name: its declared type, else the type of a use of it.
    const Ty* typeOf(const Decl* d, const ParsedFile* f)
    {
      if (const Ty* t = d->cachedType.load())
        return t;
      for (const Occurrence& o : occurrencesOf(f))
        if (o.decl == d && !o.declaration && (o.node->kind == NodeKind::NameExpr || o.node->kind == NodeKind::MemberExpr))
          if (const Ty* t = static_cast<Expr*>(o.node)->ty)
            return t;
      return nullptr;
    }

    // The class a type is, through references, const, and (after `->`) one pointer.
    Decl* classOfType(const Ty* t, bool arrow)
    {
      while (t && (t->isReference() || (arrow && t->kind == TyKind::Pointer)))
      {
        if (t->kind == TyKind::Pointer)
          arrow = false;
        t = t->inner;
      }
      if (t && t->kind == TyKind::Function)
        t = t->inner;
      while (t && t->isReference())
        t = t->inner;
      if (arrow && t && t->kind == TyKind::Pointer)
        t = t->inner;
      return t && (t->kind == TyKind::Class || t->kind == TyKind::Enum) ? t->decl : nullptr;
    }

    // The declaration a name before the cursor is: the nearest name spelled so before it in its
    // function that the compiler resolved, else a member of the function's class, else a top-level name.
    Decl* lookupBefore(const KeptProgram& program, const ParsedFile* f, uint32_t symbol, uint32_t before, FunctionDecl* function)
    {
      const std::vector<Token>& tokens = *f->tokens;
      const std::vector<Occurrence>& list = occurrencesOf(f);
      uint32_t from = function ? function->token : 0;
      for (auto it = std::lower_bound(list.begin(), list.end(), before, [](const Occurrence& o, uint32_t t) { return o.token < t; }); it != list.begin();)
      {
        --it;
        if (it->token < from)
          break;
        if (sameIdentifier(tokens[it->token], symbol))
          return it->decl;
      }
      if (function)
      {
        std::vector<ClassDecl*> classes;
        classesOf(*program.global, program.global->owningClass(function), classes);
        for (ClassDecl* c : classes)
          for (Decl* m : c->members)
            if (symbolOf(m) == symbol)
              return m;
      }
      knowSymbols(program);
      std::string_view name = program.symbols->name(symbol);
      for (const Cache::Symbol& s : cache.symbols)
        if (s.name == name && s.container.empty())
          return s.decl;
      return nullptr;
    }

    // The class the expression before `.` or `->` (at token op) is: names, calls and indexing,
    // chained with `.`, `->` and `this`.
    Decl* classBefore(const KeptProgram& program, const ParsedFile* f, uint32_t op, FunctionDecl* function)
    {
      const std::vector<Token>& tokens = *f->tokens;
      // The segments, last first: a name (or `this`) and whether a call or an index follows it.
      struct Segment { uint32_t token; bool call; bool arrowAfter; };
      std::vector<Segment> segments;
      int32_t k = int32_t(op) - 1;
      bool arrowAfter = tokens[op].is(P::Arrow);
      for (;;)
      {
        bool call = false;
        if (k >= 0 && (tokens[k].is(P::RParen) || tokens[k].is(P::RBracket)))
        {
          call = tokens[k].is(P::RParen);
          int depth = 0;
          for (; k >= 0; --k)
          {
            if (tokens[k].is(P::RParen) || tokens[k].is(P::RBracket))
              ++depth;
            else if ((tokens[k].is(P::LParen) || tokens[k].is(P::LBracket)) && --depth == 0)
              break;
          }
          --k;
        }
        if (k < 0 || !(tokens[k].getKind() == TokenKind::Identifier || tokens[k].is(Kw::this_)))
          return nullptr;
        segments.push_back({uint32_t(k), call, arrowAfter});
        if (k >= 1 && (tokens[k - 1].is(P::Dot) || tokens[k - 1].is(P::Arrow)))
        {
          arrowAfter = tokens[k - 1].is(P::Arrow);
          k -= 2;
          continue;
        }
        break;
      }
      Decl* cls = nullptr;
      for (auto it = segments.rbegin(); it != segments.rend(); ++it)
      {
        const Token& t = tokens[it->token];
        if (t.is(Kw::this_))
        {
          cls = function ? program.global->owningClass(function) : nullptr;
          continue;
        }
        Decl* d = nullptr;
        if (it == segments.rbegin())
          d = lookupBefore(program, f, t.id, it->token, function);
        else
        {
          std::vector<ClassDecl*> classes;
          classesOf(*program.global, cls, classes);
          for (ClassDecl* c : classes)
            for (Decl* m : c->members)
              if (!d && symbolOf(m) == t.id)
                d = m;
        }
        if (!d)
          return nullptr;
        const Ty* ty = d->kind == NodeKind::ClassDecl ? nullptr : typeOf(d, f);
        cls = d->kind == NodeKind::ClassDecl ? d : classOfType(ty, it->arrowAfter);
        if (!cls)
          return nullptr;
      }
      return cls;
    }

    int completionKind(const Decl* d)
    {
      switch (symbolKind(d))
      {
      case 3: return 9;   // namespace: module
      case 5: return 7;   // class
      case 23: return 22; // struct
      case 10: return 13; // enum
      case 22: return 20; // enumerator
      case 9: return 4;   // constructor
      case 6: return 2;   // method
      case 12: return 3;  // function
      case 8: return 5;   // field
      default: return 6;  // variable
      }
    }

    Json completionItem(const KeptProgram& program, Decl* d, const std::string& sortPrefix)
    {
      std::string name = declarationName(d, *program.symbols);
      size_t colons = name.rfind("::");
      if (colons != std::string::npos)
        name = name.substr(colons + 2);
      return Json::object()
        .set("label", name)
        .set("kind", completionKind(d))
        .set("detail", declarationText(program, d))
        .set("sortText", sortPrefix + name);
    }

    Json completion(const Json& params)
    {
      Json items = Json::array();
      bool incomplete = false;
      onProgram([&](const KeptProgram& program)
      {
        ParsedFile* f = fileOf(program, params["textDocument"]["uri"].text);
        if (!f || !f->tokens || f->tokens->empty())
          return;
        const std::vector<Token>& tokens = *f->tokens;
        size_t offset = offsetOf(f->text, startsOf(f), params["position"]);
        // The token the cursor is in or after, and the prefix of a name typed so far.
        auto it = std::upper_bound(tokens.begin(), tokens.end(), offset, [](size_t o, const Token& t) { return o < t.offset; });
        if (it == tokens.begin())
          return;
        uint32_t k = uint32_t(it - tokens.begin()) - 1;
        std::string prefix;
        if (tokens[k].getKind() == TokenKind::Identifier && offset <= size_t(tokens[k].offset) + tokens[k].length)
        {
          prefix = std::string(f->text.substr(tokens[k].offset, offset - tokens[k].offset));
          if (k == 0)
            return;
          --k;
        }
        else if (offset < size_t(tokens[k].offset) + tokens[k].length)
          return; // inside a literal or an operator
        for (char& c : prefix)
          c = char(tolower((unsigned char)c));
        auto matches = [&](std::string_view name)
        {
          if (name.size() < prefix.size())
            return false;
          for (size_t i = 0; i < prefix.size(); ++i)
            if (tolower((unsigned char)name[i]) != prefix[i])
              return false;
          return true;
        };
        FunctionDecl* function = enclosingFunction(f->decls, k);
        std::unordered_map<std::string, bool> seen;
        auto add = [&](Decl* d, const char* sortPrefix)
        {
          std::string name = declarationName(d, *program.symbols);
          size_t colons = name.rfind("::");
          std::string own = colons == std::string::npos ? name : name.substr(colons + 2);
          if (own.empty() || own == "(anonymous)" || !matches(own) || own.compare(0, 8, "operator") == 0 || own[0] == '~')
            return;
          if (d->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(d)->returnType && !static_cast<FunctionDecl*>(d)->name.empty() &&
              static_cast<FunctionDecl*>(d)->name.last().special == 0)
            return; // a constructor is not called by name
          std::string key = own + "\n" + declarationText(program, d);
          if (seen.emplace(key, true).second)
            items.push(completionItem(program, d, sortPrefix));
        };
        // `>` and `:` are trigger characters for `->` and `::` only.
        const std::string& trigger = params["context"]["triggerCharacter"].text;
        if ((trigger == ">" && !tokens[k].is(P::Arrow)) || (trigger == ":" && !tokens[k].is(P::ColonColon)))
          return;
        // Members after `.` and `->`, a scope's names after `::`.
        if (tokens[k].is(P::Dot) || tokens[k].is(P::Arrow))
        {
          Decl* cls = classBefore(program, f, k, function);
          std::vector<ClassDecl*> classes;
          classesOf(*program.global, cls, classes);
          for (size_t c = 0; c < classes.size(); ++c)
            for (Decl* m : classes[c]->members)
              if (symbolKind(m))
                add(m, c == 0 ? "0" : "1");
          if (cls && cls->kind == NodeKind::EnumDecl)
            for (Decl* m : static_cast<EnumDecl*>(cls)->members)
              add(m, "0");
          return;
        }
        if (tokens[k].is(P::ColonColon))
        {
          if (k == 0 || tokens[k - 1].getKind() != TokenKind::Identifier)
            return;
          std::string scope(program.symbols->name(tokens[k - 1].id));
          knowSymbols(program);
          for (const Cache::Symbol& s : cache.symbols)
          {
            size_t at = s.container.size() >= scope.size() ? s.container.size() - scope.size() : std::string::npos;
            if (at != std::string::npos && s.container.compare(at, scope.size(), scope) == 0 && (at == 0 || s.container.compare(at - 2, 2, "::") == 0))
              add(s.decl, "0");
          }
          // A scoped enum's enumerators (an unscoped one's are named without it).
          for (const Cache::Symbol& s : cache.symbols)
            if (s.name == scope && s.decl->kind == NodeKind::EnumDecl)
              for (Decl* e : static_cast<EnumDecl*>(s.decl)->enumerators)
                add(e, "0");
          return;
        }
        // A name: the function's parameters and locals declared before, its class's members, then
        // every top-level name.
        if (function)
        {
          for (Decl* p : function->params)
            add(p, "0");
          for (const Occurrence& o : occurrencesOf(f))
            if (o.declaration && o.token > function->token && o.token < k + 1 && o.decl->kind == NodeKind::VariableDecl)
              add(o.decl, "0");
          std::vector<ClassDecl*> classes;
          classesOf(*program.global, program.global->owningClass(function), classes);
          for (ClassDecl* c : classes)
            for (Decl* m : c->members)
              if (symbolKind(m))
                add(m, "1");
        }
        if (prefix.empty())
        {
          incomplete = true; // every top-level name would be too many: they come with the first letter
          return;
        }
        knowSymbols(program);
        size_t added = 0;
        // (In a scoped library's file, its own top-level names are written bare too.)
        const std::string own = f->libraryScope != UINT32_MAX ? f->library : std::string();
        for (const Cache::Symbol& s : cache.symbols)
          if ((s.container.empty() || (!own.empty() && s.container == own)) && matches(s.name))
          {
            if (++added > 300)
            {
              incomplete = true;
              break;
            }
            add(s.decl, "2");
          }
      });
      return Json::object().set("isIncomplete", incomplete).set("items", std::move(items));
    }

    // ---- rename ----

    // Places in files, the files in the build's order (not by address: the answer is the same every run).
    struct ByFileOrder
    {
      bool operator()(const ParsedFile* a, const ParsedFile* b) const { return a->index < b->index; }
    };
    using FilePlaces = std::map<const ParsedFile*, std::vector<uint32_t>, ByFileOrder>;

    // Why a declaration cannot be renamed; empty when it can.
    std::string notRenameable(const Decl* d)
    {
      if (!d->file || d->file->isImported)
        return "it is declared in an imported C header";
      if (d->file->isLibrary)
        return "it is the standard library's";
      if (d->file->path.empty() || d->file->path[0] == '<')
        return "it is the compiler's";
      switch (d->kind)
      {
      case NodeKind::ClassDecl: case NodeKind::EnumDecl: case NodeKind::EnumeratorDecl: case NodeKind::VariableDecl: case NodeKind::ParamDecl:
      case NodeKind::UsingAliasDecl: case NodeKind::TemplateParamDecl:
        break;
      case NodeKind::FunctionDecl:
        if (d->flags & (DF_Constructor | DF_Destructor))
          return "it is a constructor or destructor: rename its class";
        if (d->flags & (DF_Operator | DF_Conversion))
          return "it is an operator";
        if (d->flags & (DF_Virtual | DF_Override))
          return "it is virtual: its overrides would keep the old name (not supported yet)";
        if (d->flags & DF_ExternC)
          return "it has C linkage: its name is its symbol";
        break;
      default:
        return "renaming a declaration of this kind is not supported";
      }
      if (symbolOf(d) == UINT32_MAX)
        return "it has no name of its own";
      return {};
    }

    // Every place a declaration's name is written, the declaration's own included (its other
    // declarations too), or why that cannot be told: every identifier spelled like it in the
    // program's own files must be a use the compiler resolved, to it or to another declaration, or
    // a qualifier naming it (in a template's instances too); a name it did not resolve (a
    // template's dependent name never instantiated, a macro-like use) makes the answer unknown.
    std::string placesOf(const KeptProgram& program, Decl* target, FilePlaces& places)
    {
      GlobalScope& global = *program.global;
      knowReferences(program);
      uint32_t symbol = symbolOf(target);
      // A class's constructors and destructor are named by it.
      auto meaning = [&](Decl* d) -> Decl*
      {
        d = canonical(global, d);
        if (d->kind == NodeKind::FunctionDecl && (d->flags & (DF_Constructor | DF_Destructor)))
          if (Decl* cls = global.owningClass(d); cls && canonical(global, cls) == target)
            return target;
        return d;
      };
      // What each token spelled so is: the declarations the original trees and the instances' copies resolve it to.
      std::map<const ParsedFile*, std::map<uint32_t, std::vector<Decl*>>> meanings;
      for (const ParsedFile* f : program.files)
        for (const Occurrence& o : cache.occurrences[f])
          if (o.token < f->tokens->size() && sameIdentifier((*f->tokens)[o.token], symbol))
            meanings[f][o.token].push_back(meaning(o.decl));
      global.forEachInstanceCopy([&](Node* copy, const Decl*, const ParsedFile& f)
      {
        if (!f.tokens)
          return;
        forEachNode(copy, [&](Node* n)
        {
          Decl* d = isDeclarationNode(n) ? nullptr : resolvedOf(n);
          if (!d)
            return;
          const QualifiedName* q = n->kind == NodeKind::NameExpr ? &static_cast<NameExpr*>(n)->name
                                   : n->kind == NodeKind::NamedType ? &static_cast<NamedType*>(n)->name
                                   : n->kind == NodeKind::MemberExpr ? &static_cast<MemberExpr*>(n)->member : nullptr;
          if (!q || q->empty() || q->last().symbol != symbol)
            return;
          uint32_t from = n->token;
          if (n->kind == NodeKind::MemberExpr && static_cast<MemberExpr*>(n)->base)
            forEachNode(static_cast<MemberExpr*>(n)->base, [&](Node* b) { from = std::max(from, b->token + 1); });
          uint32_t t = nameToken(*f.tokens, *q, from, from + 64);
          if (t == UINT32_MAX)
            return;
          // A copy's own declaration (a member of a class template's instance) is no declaration of the program's.
          if (d->attributes & ATTR_InstanceCopy)
            d = nullptr;
          meanings[&f][t].push_back(d ? meaning(d) : nullptr);
        });
      });
      auto where = [&](const ParsedFile* f, uint32_t token)
      {
        const LineStarts& starts = startsOf(f);
        size_t line = starts.line((*f->tokens)[token].offset);
        return f->path + ":" + std::to_string(line);
      };
      for (const ParsedFile* f : program.files)
      {
        if (!f->tokens || f->path.empty() || f->path[0] == '<')
          continue;
        const std::map<uint32_t, std::vector<Decl*>>& known = meanings[f];
        for (uint32_t k = 0; k < f->tokens->size(); ++k)
        {
          if (!sameIdentifier((*f->tokens)[k], symbol))
            continue;
          auto found = known.find(k);
          bool isTarget = false, isOther = false;
          if (found != known.end())
            for (Decl* d : found->second)
              (d == target ? isTarget : isOther) = true;
          if (isTarget && isOther)
            return "the name at " + where(f, k) + " means it in one instance of a template and something else in another";
          if (isTarget && (f->isLibrary || f->isImported))
            return "the standard library uses it (" + where(f, k) + ")";
          if (isTarget)
            places[f].push_back(k);
          else if (!isOther && !f->isLibrary && !f->isImported)
            return "the name at " + where(f, k) + " is one the compiler did not resolve (a template's name that depends on its parameters, never instantiated), so whether it means this cannot be told";
        }
      }
      return {};
    }

    // Why newName would change what an existing name means, or would not be a name; empty when not.
    std::string clashOf(const KeptProgram& program, Decl* target, const std::string& newName)
    {
      bool valid = !newName.empty() && (isalpha((unsigned char)newName[0]) || newName[0] == '_');
      for (char c : newName)
        valid = valid && (isalnum((unsigned char)c) || c == '_');
      if (!valid)
        return "'" + newName + "' is not a name";
      if (lookupKeyword(newName) != UINT32_MAX)
        return "'" + newName + "' is a keyword";
      GlobalScope& global = *program.global;
      auto spelled = [&](const Decl* d) { return declarationName(d, *program.symbols) == newName || (symbolOf(d) != UINT32_MAX && program.symbols->name(symbolOf(d)) == newName); };
      // Locals and parameters: any name so spelled in the function, whatever it means there.
      if (target->kind == NodeKind::ParamDecl || (target->kind == NodeKind::VariableDecl && target->parent && target->parent->kind == NodeKind::FunctionDecl) ||
          (target->kind == NodeKind::VariableDecl && enclosingFunction(target->file->decls, target->token)))
      {
        FunctionDecl* f = target->kind == NodeKind::ParamDecl && target->parent && target->parent->kind == NodeKind::FunctionDecl
                            ? static_cast<FunctionDecl*>(target->parent) : enclosingFunction(target->file->decls, target->token);
        if (f)
          for (uint32_t k = f->token; k < f->endToken && k < target->file->tokens->size(); ++k)
            if ((*target->file->tokens)[k].getKind() == TokenKind::Identifier && program.symbols->name((*target->file->tokens)[k].id) == newName)
              return "'" + newName + "' is already a name in " + declarationName(f, *program.symbols);
        return {};
      }
      // Members: the class's and its bases' members.
      if (Decl* cls = global.owningClass(target))
      {
        std::vector<ClassDecl*> classes;
        classesOf(global, cls, classes);
        for (ClassDecl* c : classes)
          for (Decl* m : c->members)
            if (spelled(m))
              return "'" + newName + "' is already a member of " + declarationName(c, *program.symbols);
        return {};
      }
      // Enumerators: the enum's (and an unscoped enum's names are its scope's too).
      if (target->kind == NodeKind::EnumeratorDecl && target->parent && target->parent->kind == NodeKind::EnumDecl)
        for (Decl* e : static_cast<EnumDecl*>(target->parent)->enumerators)
          if (spelled(e))
            return "'" + newName + "' is already an enumerator of it";
      // Everything else: one global scope (design.md), so any declaration outside a function body so named.
      knowSymbols(program);
      for (const Cache::Symbol& sym : cache.symbols)
        if (sym.name == newName)
          return "'" + newName + "' is already declared (" + (sym.container.empty() ? "" : sym.container + "::") + newName + ")";
      return {};
    }

    // prepareRename: the name's range, or an error saying why it cannot be renamed.
    bool prepareRename(const Json& params, Json& result, std::string& error)
    {
      onProgram([&](const KeptProgram& program)
      {
        const ParsedFile* file = nullptr;
        const Occurrence* o = occurrenceAt(program, params, &file);
        if (!o)
        {
          error = "no name the compiler resolved is here";
          return;
        }
        Decl* target = canonical(*program.global, o->decl);
        if (std::string why = notRenameable(target); !why.empty())
        {
          error = "cannot rename " + declarationName(target, *program.symbols) + ": " + why;
          return;
        }
        const Token& t = (*file->tokens)[o->token];
        result = Json::object().set("range", tokenRange(file, o->token)).set("placeholder", std::string(file->text.substr(t.offset, t.length)));
      });
      return error.empty() && result.kind != Json::Null;
    }

    bool rename(const Json& params, Json& result, std::string& error)
    {
      std::string newName = params["newName"].text;
      onProgram([&](const KeptProgram& program)
      {
        const Occurrence* o = occurrenceAt(program, params);
        if (!o)
        {
          error = "no name the compiler resolved is here";
          return;
        }
        Decl* target = canonical(*program.global, o->decl);
        std::string oldName = std::string(program.symbols->name(symbolOf(target)));
        if (std::string why = notRenameable(target); !why.empty())
        {
          error = "cannot rename " + oldName + ": " + why;
          return;
        }
        if (std::string why = clashOf(program, target, newName); !why.empty())
        {
          error = "cannot rename " + oldName + " to " + newName + ": " + why;
          return;
        }
        FilePlaces places;
        if (std::string why = placesOf(program, target, places); !why.empty())
        {
          error = "cannot rename " + oldName + ": " + why;
          return;
        }
        Json changes = Json::object();
        for (const auto& [f, tokens] : places)
        {
          Json edits = Json::array();
          for (uint32_t k : tokens)
            edits.push(Json::object().set("range", tokenRange(f, k)).set("newText", newName));
          changes.set(uriOf(overlayKey(f->path), f->path), std::move(edits));
        }
        result = Json::object().set("changes", std::move(changes));
      });
      return error.empty();
    }

    // ---- signature help ----

    Json signatureHelp(const Json& params)
    {
      Json result;
      onProgram([&](const KeptProgram& program)
      {
        ParsedFile* f = fileOf(program, params["textDocument"]["uri"].text);
        if (!f || !f->tokens || f->tokens->empty())
          return;
        const std::vector<Token>& tokens = *f->tokens;
        size_t offset = offsetOf(f->text, startsOf(f), params["position"]);
        auto it = std::lower_bound(tokens.begin(), tokens.end(), offset, [](const Token& t, size_t o) { return size_t(t.offset) < o; });
        // The call's `(` that the cursor is in, and the argument it is at.
        int32_t k = int32_t(it - tokens.begin()) - 1;
        int depth = 0;
        uint32_t argument = 0;
        for (; k >= 0; --k)
        {
          const Token& t = tokens[k];
          if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::RBrace))
            ++depth;
          else if (t.is(P::LParen) || t.is(P::LBracket) || t.is(P::LBrace))
          {
            if (depth == 0)
              break;
            --depth;
          }
          else if (t.is(P::Semi) && depth == 0)
            return;
          else if (t.is(P::Comma) && depth == 0)
            ++argument;
        }
        if (k < 1 || !tokens[k].is(P::LParen) || tokens[k - 1].getKind() != TokenKind::Identifier)
          return;
        uint32_t name = uint32_t(k - 1);
        FunctionDecl* function = enclosingFunction(f->decls, name);
        // The functions so named: a class's members after `.` and `->`, else the name's own and its overloads.
        std::vector<Decl*> candidates;
        uint32_t symbol = tokens[name].id;
        auto addFunction = [&](Decl* d)
        {
          if (d && d->kind == NodeKind::FunctionDecl && symbolOf(d) == symbol && std::find(candidates.begin(), candidates.end(), d) == candidates.end())
            candidates.push_back(d);
        };
        if (name >= 1 && (tokens[name - 1].is(P::Dot) || tokens[name - 1].is(P::Arrow)))
        {
          std::vector<ClassDecl*> classes;
          classesOf(*program.global, classBefore(program, f, name - 1, function), classes);
          for (ClassDecl* c : classes)
            for (Decl* m : c->members)
              addFunction(m);
        }
        else
        {
          addFunction(lookupBefore(program, f, symbol, name, function));
          if (function)
          {
            std::vector<ClassDecl*> classes;
            classesOf(*program.global, program.global->owningClass(function), classes);
            for (ClassDecl* c : classes)
              for (Decl* m : c->members)
                addFunction(m);
          }
          knowSymbols(program);
          std::string_view spelled = program.symbols->name(symbol);
          std::string scope = name >= 2 && tokens[name - 1].is(P::ColonColon) && tokens[name - 2].getKind() == TokenKind::Identifier
                                ? std::string(program.symbols->name(tokens[name - 2].id)) : std::string();
          for (const Cache::Symbol& sym : cache.symbols)
            if (sym.name == spelled &&
                (scope.empty() ? sym.container.empty()
                               : sym.container == scope || (sym.container.size() > scope.size() + 2 && sym.container.compare(sym.container.size() - scope.size() - 2, std::string::npos, "::" + scope) == 0)))
              addFunction(sym.decl);
        }
        if (candidates.empty())
          return;
        Json signatures = Json::array();
        uint32_t active = 0;
        for (size_t c = 0; c < candidates.size(); ++c)
        {
          auto* fn = static_cast<FunctionDecl*>(candidates[c]);
          std::string label = declarationText(program, fn);
          // Each parameter's text in the label: between the outer parentheses, split at commas.
          Json parameters = Json::array();
          size_t open = label.find('(');
          if (open != std::string::npos)
          {
            int level = 0;
            size_t start = open + 1;
            for (size_t i = open + 1; i < label.size(); ++i)
            {
              char ch = label[i];
              if (ch == '(' || ch == '<' || ch == '[' || ch == '{')
                ++level;
              else if ((ch == ')' || ch == '>' || ch == ']' || ch == '}') && level > 0)
                --level;
              else if ((ch == ',' && level == 0) || (ch == ')' && level == 0))
              {
                size_t a = start, b = i;
                while (a < b && label[a] == ' ')
                  ++a;
                if (b > a)
                  parameters.push(Json::object().set("label", Json::array().push(utf16Units(label, 0, a)).push(utf16Units(label, 0, b))));
                start = i + 1;
                if (ch == ')')
                  break;
              }
            }
          }
          if (active == 0 && fn->params.size() > argument)
            active = uint32_t(c);
          signatures.push(Json::object().set("label", label).set("parameters", std::move(parameters)));
        }
        result = Json::object().set("signatures", std::move(signatures)).set("activeSignature", active).set("activeParameter", argument);
      });
      return result;
    }

    // ---- semantic tokens ----

    // The legend's token types, in order (semanticTokensProvider.legend).
    static constexpr const char* tokenTypes[] = {"namespace", "type", "class", "enum", "struct", "typeParameter", "parameter", "variable",
                                                 "property", "enumMember", "function", "method"};

    int tokenType(const Decl* d)
    {
      switch (d->kind)
      {
      case NodeKind::NamespaceDecl: return 0;
      case NodeKind::UsingAliasDecl: return 1;
      case NodeKind::ClassDecl: return static_cast<const ClassDecl*>(d)->keyword == Kw::class_ ? 2 : 4;
      case NodeKind::EnumDecl: return 3;
      case NodeKind::TemplateParamDecl: return 5;
      case NodeKind::ParamDecl: return 6;
      case NodeKind::VariableDecl: return d->parent && d->parent->kind == NodeKind::ClassDecl ? 8 : 7;
      case NodeKind::EnumeratorDecl: return 9;
      case NodeKind::FunctionDecl: return symbolKind(d) == 12 ? 10 : 11;
      default: return -1;
      }
    }

    // The legend's modifiers past declaration (1) and defaultLibrary (2): static (4), a class's
    // static member; globalScope (8, clangd's), a variable of a namespace or the file. A client
    // tells a global from a local, and a static member from an instance's, by them (Visual Studio's
    // C++ colors: cppGlobalVariable, cppStaticMemberField, cppStaticMemberFunction).
    static constexpr const char* tokenModifiers[] = {"declaration", "defaultLibrary", "static", "globalScope"};
    enum : int { ModifierDeclaration = 1, ModifierDefaultLibrary = 2, ModifierStatic = 4, ModifierGlobalScope = 8 }; // (bits in tokenModifiers' order)

    int scopeModifiers(const KeptProgram& program, Decl* d)
    {
      if (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::FunctionDecl)
        return 0;
      Decl* declared = d;
      if (d->kind == NodeKind::FunctionDecl)
        if (FunctionDecl* f = program.global->declarationOf(static_cast<FunctionDecl*>(d)))
          declared = f;
      if (program.global->owningClass(d)) // (a data member defined outside its class, `int Shape::count = 0;`, is a static one)
        return declared->flags & DF_Static || (d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->name.parts.size() > 1) ? ModifierStatic : 0;
      if (d->kind == NodeKind::VariableDecl && (!d->parent || d->parent->kind == NodeKind::NamespaceDecl))
        return ModifierGlobalScope;
      return 0;
    }

    Json semanticTokens(const Json& params)
    {
      Json data = Json::array();
      onProgram([&](const KeptProgram& program)
      {
        ParsedFile* f = fileOf(program, params["textDocument"]["uri"].text);
        if (!f || !f->tokens)
          return;
        const LineStarts& starts = startsOf(f);
        size_t previousLine = 0, previousStart = 0;
        for (const Occurrence& o : occurrencesOf(f))
        {
          int type = tokenType(o.decl);
          if (type < 0)
            continue;
          const Token& t = (*f->tokens)[o.token];
          size_t line = starts.index(t.offset);
          size_t start = utf16Units(f->text, starts[line], t.offset);
          int modifiers = (o.declaration ? ModifierDeclaration : 0) | (o.decl->file && o.decl->file->isLibrary ? ModifierDefaultLibrary : 0) | scopeModifiers(program, o.decl);
          data.push(line - previousLine).push(line == previousLine ? start - previousStart : start)
            .push(utf16Units(f->text, t.offset, size_t(t.offset) + t.length)).push(type).push(modifiers);
          previousLine = line;
          previousStart = start;
        }
      });
      return Json::object().set("data", std::move(data));
    }

    Json workspaceSymbols(const Json& params)
    {
      Json result = Json::array();
      std::string query = params["query"].text;
      for (char& c : query)
        c = char(tolower((unsigned char)c));
      onProgram([&](const KeptProgram& program)
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
          const Cache::Symbol& x = *a.second;
          const Cache::Symbol& y = *b.second;
          uint32_t xf = x.decl->file ? x.decl->file->index : 0, yf = y.decl->file ? y.decl->file->index : 0;
          return std::tie(a.first, x.name, x.container, xf, x.decl->token) < std::tie(b.first, y.name, y.container, yf, y.decl->token);
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
        LineStarts starts(d.text);
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
      inactiveSent.erase(key);
      if (documents.erase(key))
      {
        setFileOverlay(path, nullptr); // the file on disk again
        dirty = true;
      }
    }

    Json semanticTokensOptions()
    {
      Json types = Json::array();
      for (const char* t : tokenTypes)
        types.push(t);
      return Json::object().set("legend", semanticTokensLegend()).set("full", true);
    }

    // The legend: the token types (tokenTypes) and modifiers (tokenModifiers) by name, the one
    // source of both. Visual Studio's extension asks for it (kpp/semanticTokensLegend), as its
    // client is not offered the capability (semanticColors).
    Json semanticTokensLegend()
    {
      Json types = Json::array(), modifiers = Json::array();
      for (const char* t : tokenTypes)
        types.push(t);
      for (const char* m : tokenModifiers)
        modifiers.push(m);
      return Json::object().set("tokenTypes", std::move(types)).set("tokenModifiers", std::move(modifiers));
    }

    Json capabilities()
    {
      Json c = Json::object();
      c.set("positionEncoding", "utf-16")
        .set("textDocumentSync", Json::object()
                                   .set("openClose", true)
                                   .set("change", 2) // incremental
                                   .set("save", Json::object().set("includeText", false)))
        .set("hoverProvider", true)
        .set("completionProvider", Json::object().set("triggerCharacters", Json::array().push(".").push(">").push(":")))
        .set("signatureHelpProvider", Json::object().set("triggerCharacters", Json::array().push("(").push(",")))
        .set("renameProvider", Json::object().set("prepareProvider", true));
      if (!semanticColors)
        c.set("semanticTokensProvider", semanticTokensOptions());
      c.set("definitionProvider", true)
        .set("declarationProvider", true)
        .set("referencesProvider", true)
        .set("documentSymbolProvider", true)
        .set("workspaceSymbolProvider", true);
      return c;
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
        if (params["workspaceFolders"].kind == Json::Array && !params["workspaceFolders"].items.empty())
          workspace = uriToPath(params["workspaceFolders"].items[0]["uri"].text);
        else if (params["rootUri"].kind == Json::String)
          workspace = uriToPath(params["rootUri"].text);
        else if (params["rootPath"].kind == Json::String)
          workspace = params["rootPath"].text;
        // What the client renders (hover's formats, completion items'): in the log, for its editor's checks.
        {
          std::string hoverCaps, completionCaps;
          write(params["capabilities"]["textDocument"]["hover"], hoverCaps);
          write(params["capabilities"]["textDocument"]["completion"], completionCaps);
          log("client %s %s, hover %s, completion %.1200s", params["clientInfo"]["name"].text.c_str(), params["clientInfo"]["version"].text.c_str(),
              hoverCaps.c_str(), completionCaps.c_str());
        }
        // Markdown only for a client that names it: without contentFormat a client takes plain text
        // (the protocol's default), and Visual Studio's names plain text alone (docs/editors.md).
        markdownHover = false;
        for (const Json& format : params["capabilities"]["textDocument"]["hover"]["contentFormat"].items)
          markdownHover = markdownHover || format.text == "markdown";
        // clangd's capability, or initializationOptions' inactiveRegions from a client that cannot add
        // one (Visual Studio's in-process client).
        inactiveRegions = params["capabilities"]["textDocument"]["inactiveRegionsCapabilities"]["inactiveRegions"].boolean ||
                          params["initializationOptions"]["inactiveRegions"].boolean;
        // The protocol's capability (VS Code's). Visual Studio's client takes the tokens too, but
        // draws types and functions alike in kpp's colors; its extension asks for them itself and
        // draws them in Visual Studio's C++ colors (initializationOptions' semanticColors): the
        // client is not offered them, and the extension is told after each check.
        semanticTokensRefresh = params["capabilities"]["workspace"]["semanticTokens"]["refreshSupport"].boolean;
        semanticColors = params["initializationOptions"]["semanticColors"].boolean;
        ProjectCheck wanted = project;
        readOptions(params["initializationOptions"], wanted);
        std::string error;
        if (!chooseArguments(self, wanted, error))
        {
          showMessage(1, "kpp: " + error + "; nothing is checked.");
          checkArguments.clear();
        }
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
      {
        dirty = true; // files changed on disk by others are read again too
        std::string error;
        if (_wcsicmp(uriToPath(params["textDocument"]["uri"].text).filename().c_str(), L"build.kpp") == 0 &&
            !chooseArguments(self, project, error)) // the project described again
        {
          showMessage(1, "kpp: " + error + "; nothing is checked.");
          checkArguments.clear();
          enterCell("");
        }
      }
      else if (method == "workspace/didChangeConfiguration")
      {
        ProjectCheck wanted = project;
        readOptions(params["settings"]["kpp"], wanted);
        std::string error;
        if (!chooseArguments(self, wanted, error))
          showMessage(1, "kpp: " + error + "; still checking " + cellName(project) + ".");
        dirty = true;
      }
      else if (method == "kpp/semanticTokensLegend")
        respond(id, semanticTokensLegend());
      else if (method == "kpp/cells")
        respond(id, cellList());
      else if (method == "kpp/selectCell")
      {
        ProjectCheck wanted = project;
        wanted.target = params["target"].kind == Json::String ? params["target"].text : std::string();
        if (params["configuration"].kind == Json::String)
          wanted.configuration = params["configuration"].text;
        std::string error;
        if (activeCell.empty())
          respondError(id, -32803, "no build.kpp: the program has no cells");
        else if (!chooseArguments(self, wanted, error))
          respondError(id, -32803, error);
        else
          respond(id, cellJson(project));
      }
      else if (method == "textDocument/completion")
        respond(id, completion(params));
      else if (method == "textDocument/prepareRename" || method == "textDocument/rename")
      {
        Json result;
        std::string error;
        if (method == "textDocument/prepareRename" ? prepareRename(params, result, error) : rename(params, result, error))
          respond(id, std::move(result));
        else
          respondError(id, -32803, error.empty() ? "nothing to rename here" : error); // RequestFailed
      }
      else if (method == "textDocument/signatureHelp")
        respond(id, signatureHelp(params));
      else if (method == "textDocument/semanticTokens/full")
      {
        Json tokens = semanticTokens(params);
        log("semantic tokens: %s, %zu", params["textDocument"]["uri"].text.c_str(), tokens["data"].items.size() / 5);
        respond(id, std::move(tokens));
      }
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

    CompileFunction compile = nullptr;
    std::vector<std::string> explicitArguments; // `kppc --lsp OPTIONS SOURCES`
    fs::path workspace;                         // the editor's root folder
    fs::path projectFolder;                     // build.kpp's folder: the workspace or one above it
    ProjectCheck project;                       // the cell checked: initializationOptions, workspace/didChangeConfiguration, kpp/selectCell (docs/editors.md)
    std::vector<std::string> checkArguments;    // what a check builds (empty: nothing to check)
    std::map<std::string, Cell> parkedCells;    // by cellKey
    std::string activeCell;                     // cellKey of the cell checked now ("": no build.kpp's)
    std::vector<ProjectOption> activeOptions;   // its libraries' options (every target's, for all of them)
    std::vector<std::string> activeArguments;   // what the process's kept build was made with
    std::string announcedCell = "\n";          // the cell kpp/activeCell last named
    std::string shownFailure;                   // the diagnostic-less failure last shown (check)
    bool markdownHover = false;                 // the client renders hover's markdown (else plain text)
    bool inactiveRegions = false;               // the client takes textDocument/inactiveRegions (clangd's)
    bool semanticTokensRefresh = false;         // the client takes workspace/semanticTokens/refresh
    bool semanticColors = false;                // the client's extension asks for the tokens itself (kpp/semanticTokensChanged)
    size_t refreshes = 0;                       // the refresh requests sent (their ids)
    std::map<std::string, std::string> inactiveSent; // the regions last sent per open document (overlayKey -> JSON)
    size_t keptCells = 4, cellClock = 0;
    bool checkEachChange = false;
    bool initialized = false, shutdownRequested = false;
    bool dirty = false;                         // a document changed since the last check
    size_t checks = 0;
    std::map<std::string, Document> documents;  // the open documents, by overlayKey
    std::map<std::string, std::string> published; // what was last published per file (overlayKey -> the message's JSON)
    std::map<std::string, std::string> uris;      // the URI each published file goes by
    Cache cache;
    GlobalScope* currentGlobal = nullptr; // the kept program's, while a query runs on it
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
    // The transport options of LSP's "Implementation Considerations": stdin and stdout is the one
    // this server has (vscode-languageclient adds --stdio for it), the others are refused.
    if (arg == "--stdio")
      continue;
    if (arg == "--pipe" || arg.rfind("--pipe=", 0) == 0 || arg.rfind("--socket", 0) == 0 || arg == "--node-ipc")
    {
      fprintf(stderr, "kppc --lsp: %s is not supported: the language server talks on stdin and stdout (--stdio)\n", arg.c_str());
      return 2;
    }
    if (arg == "--check-each-change")
      server.checkEachChange = true;
    else if (arg == "--lsp-log" && i + 1 < argc)
    {
      logFile = CreateFileA(argv[++i], GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (logFile == INVALID_HANDLE_VALUE)
        fprintf(stderr, "kppc --lsp: cannot write %s\n", argv[i]);
    }
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
