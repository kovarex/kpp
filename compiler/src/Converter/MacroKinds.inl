// MacroKinds.inl: what a macro is: a string, a name, spliced text.
// Part of convert.cpp, included where it stood in its anonymous namespace (a file of its own since 2026-11-05, moved without a change).

  // Whether text is one or more string literals and nothing else (`"a" "b"`, `u8"x"`).
  bool isStringLiterals(const std::string& text)
  {
    size_t i = 0;
    bool any = false;
    for (;;)
    {
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
        ++i;
      if (i == text.size())
        return any;
      if (text.compare(i, 2, "u8") == 0)
        i += 2;
      else if (text[i] == 'u' || text[i] == 'U' || text[i] == 'L')
        ++i;
      if (i == text.size() || text[i] != '"')
        return false;
      for (++i; i < text.size() && text[i] != '"'; ++i)
        if (text[i] == '\\')
          ++i;
      if (i >= text.size())
        return false;
      ++i;
      any = true;
    }
  }

  // Whether name is in capitals (letters, digits and underscores, a letter among them), as C's
  // macros and Windows' typedefs are.
  bool isAllCapsName(std::string_view name)
  {
    bool letter = false;
    for (char c : name)
    {
      if (c >= 'a' && c <= 'z')
        return false;
      letter = letter || (c >= 'A' && c <= 'Z');
    }
    return letter && name.size() > 1;
  }

  // Whether text is a qualified name, `A::b` (`#define vkCreateInstance VulkanImpl::CreateInstanceWrapper`).
  bool isQualifiedName(const std::string& text)
  {
    size_t i = 0, parts = 0;
    while (i < text.size())
    {
      if (!(isalpha((unsigned char)text[i]) || text[i] == '_'))
        return false;
      while (i < text.size() && (isalnum((unsigned char)text[i]) || text[i] == '_'))
        ++i;
      ++parts;
      if (i == text.size())
        break;
      if (text.compare(i, 2, "::") != 0)
        return false;
      i += 2;
    }
    return parts >= 2;
  }

  // The text of a macro body made of string literals, string macros (in objects) and calls of
  // function-like macros (in macros) that give string literals, expanded: luaconf.h's
  // `LUA_QL("%s")` with `#define LUA_QL(x) "'" x "'"` is `"'" "%s" "'"`. Empty when the body is
  // anything else.
  std::string stringMacroText(const std::string& body, const std::map<std::string, std::string>& objects,
                              const std::map<std::string, std::pair<std::vector<std::string>, std::string>>& macros, int depth = 0)
  {
    if (depth > 8)
      return "";
    std::string out;
    size_t i = 0;
    auto skipSpace = [&]()
    {
      while (i < body.size() && (body[i] == ' ' || body[i] == '\t'))
        ++i;
    };
    for (skipSpace(); i < body.size(); skipSpace())
    {
      if (body[i] == '"')
      {
        size_t e = i + 1;
        while (e < body.size() && body[e] != '"')
          e += body[e] == '\\' ? 2 : 1;
        if (e >= body.size())
          return "";
        out += (out.empty() ? "" : " ") + body.substr(i, e + 1 - i);
        i = e + 1;
      }
      else if (isalpha((unsigned char)body[i]) || body[i] == '_')
      {
        size_t e = i;
        while (e < body.size() && (isalnum((unsigned char)body[e]) || body[e] == '_'))
          ++e;
        std::string name = body.substr(i, e - i);
        i = e;
        skipSpace();
        std::string text;
        if (auto o = objects.find(name); o != objects.end() && isStringLiterals(o->second))
          text = o->second;
        else if (auto m = macros.find(name); m != macros.end() && i < body.size() && body[i] == '(')
        {
          // The arguments, split at top-level commas, put in for the parameters by name.
          std::vector<std::string> args;
          int level = 0;
          size_t start = i + 1, k = i;
          for (; k < body.size(); ++k)
          {
            if (body[k] == '"')
            {
              for (++k; k < body.size() && body[k] != '"'; ++k)
                if (body[k] == '\\')
                  ++k;
              continue;
            }
            if (body[k] == '(')
              ++level;
            else if (body[k] == ')' && --level == 0)
              break;
            else if (body[k] == ',' && level == 1)
            {
              args.push_back(body.substr(start, k - start));
              start = k + 1;
            }
          }
          if (k >= body.size())
            return "";
          args.push_back(body.substr(start, k - start));
          i = k + 1;
          const auto& [params, macroBody] = m->second;
          if (args.size() != params.size())
            return "";
          std::string expanded;
          for (size_t q = 0; q < macroBody.size();)
          {
            if (isalpha((unsigned char)macroBody[q]) || macroBody[q] == '_')
            {
              size_t r = q;
              while (r < macroBody.size() && (isalnum((unsigned char)macroBody[r]) || macroBody[r] == '_'))
                ++r;
              std::string word = macroBody.substr(q, r - q);
              auto param = std::find(params.begin(), params.end(), word);
              expanded += param == params.end() ? word : args[param - params.begin()];
              q = r;
            }
            else if (macroBody[q] == '"')
            {
              size_t r = q + 1;
              while (r < macroBody.size() && macroBody[r] != '"')
                r += macroBody[r] == '\\' ? 2 : 1;
              expanded += macroBody.substr(q, r + 1 - q);
              q = r + 1;
            }
            else
              expanded += macroBody[q++];
          }
          text = stringMacroText(expanded, objects, macros, depth + 1);
        }
        if (text.empty())
          return "";
        out += (out.empty() ? "" : " ") + text;
      }
      else
        return "";
    }
    return isStringLiterals(out) ? out : "";
  }

  // Whether an object-like macro's body is no value but text C++ splices in where the name stands:
  // nothing (`#define LUAI_DDEF /* empty */`), declarations (lobject.h's `#define CommonHeader
  // GCObject *next; lu_byte tt; lu_byte marked`, used as `CommonHeader;` among a struct's members),
  // a type (`#define LUA_NUMBER double`, `#define LUAI_UMEM size_t`, `unsigned LUA_INT32`), or
  // storage and attribute words (`#define LUA_API extern "C"`, `#define l_noret void
  // __declspec(noreturn)`). The conversion writes such a macro out at its uses, as the
  // preprocessor did; a constant of it would be wrong.
  bool isSplicedMacroBody(std::string body)
  {
    for (size_t c = body.find("/*"); c != std::string::npos; c = body.find("/*", c))
    {
      size_t e = body.find("*/", c + 2);
      body.replace(c, (e == std::string::npos ? body.size() : e + 2) - c, " ");
    }
    if (body.find_first_not_of(" \t") == std::string::npos)
      return true;
    {
      // A member's path (zlib's deflate.h `#define Freq fc.freq`, used as `tree[n].Freq`).
      size_t a = body.find_first_not_of(" \t"), b = body.find_last_not_of(" \t");
      std::string path = body.substr(a, b - a + 1);
      bool member = path.find('.') != std::string::npos && !isdigit((unsigned char)path[0]) && path.back() != '.';
      for (size_t k = 0; k < path.size() && member; ++k)
        member = isIdentChar(path[k]) || (path[k] == '.' && k > 0 && path[k - 1] != '.');
      if (member)
        return true;
    }
    // Declarations, a braced initializer, a list (lobject.h's `#define NILCONSTANT {NULL}, LUA_TNIL`).
    int depth = 0;
    for (size_t i = 0; i < body.size(); ++i)
      if (body[i] == '"' || (body[i] == '\'' && !isDigitSeparator(body, i)))
        for (char quote = body[i++]; i < body.size() && body[i] != quote; ++i)
          i += body[i] == '\\';
      else if (body[i] == ';' || body[i] == '{' || (body[i] == ',' && depth == 0))
        return true;
      else if (body[i] == '(')
        ++depth;
      else if (body[i] == ')')
        --depth;
    static const std::set<std::string> words = {"void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "bool",
                                                "const", "volatile", "extern", "static", "inline", "__forceinline", "__inline", "register",
                                                "__vectorcall", "__cdecl", "__stdcall", "__fastcall", "__thiscall"};
    bool keyword = false;
    size_t identifiers = 0;
    std::string last;
    for (size_t i = 0; i < body.size();)
    {
      char c = body[i];
      if (c == ' ' || c == '\t' || c == '*' || c == '&')
        ++i;
      else if (c == '"')
      {
        // extern "C"
        if (body.compare(i, 3, "\"C\"") != 0)
          return false;
        i += 3;
      }
      else if (isalpha((unsigned char)c) || c == '_')
      {
        size_t e = i;
        while (e < body.size() && (isalnum((unsigned char)body[e]) || body[e] == '_'))
          ++e;
        std::string w = body.substr(i, e - i);
        i = e;
        if (w == "__declspec" || w == "__attribute__")
        {
          size_t open = body.find_first_not_of(" \t", i);
          if (open == std::string::npos || body[open] != '(')
            return false;
          int depth = 0;
          size_t k = open;
          for (; k < body.size(); ++k)
            if (body[k] == '(')
              ++depth;
            else if (body[k] == ')' && --depth == 0)
              break;
          if (k >= body.size())
            return false;
          i = k + 1;
          keyword = true;
        }
        else if (words.count(w))
          keyword = true;
        else
        {
          ++identifiers;
          last = w;
        }
      }
      else
        return false;
    }
    // Words with at most one other name (`unsigned LUA_INT32`, `struct lua_State`), or a C typedef's
    // name alone (`size_t`, `ptrdiff_t`).
    return (keyword && identifiers <= 1) || (identifiers == 1 && last.size() > 2 && last.compare(last.size() - 2, 2, "_t") == 0);
  }

  // Whether an object-like macro's body calls a function, a name followed by `(` that is no
  // function-like macro (functionMacroNames: every one the converted files define) and no operator
  // of the language: UnitTest++'s `#define BREAK_UNDER_DEBUGGER (IsDebuggerPresent() &&
  // (DebugBreak(), true))`. Such a macro is written out at its uses, as the preprocessor writes it:
  // a constant would call the functions once, when the program starts.
  bool callsFunction(const std::string& body, const std::set<std::string>& functionMacroNames)
  {
    static const std::set<std::string> notCalls = {"sizeof", "alignof", "decltype", "static_cast", "reinterpret_cast", "const_cast",
                                                   "dynamic_cast", "noexcept", "alignas", "__declspec", "__attribute__", "__pragma",
                                                   "defined", "typeid", "L", "u8"};
    for (size_t i = 0; i < body.size();)
    {
      if (body[i] == '"' || (body[i] == '\'' && !isDigitSeparator(body, i)))
      {
        char quote = body[i];
        for (++i; i < body.size() && body[i] != quote; ++i)
          if (body[i] == '\\')
            ++i;
        ++i;
      }
      else if (body.compare(i, 2, "/*") == 0)
      {
        size_t e = body.find("*/", i + 2);
        i = e == std::string::npos ? body.size() : e + 2;
      }
      else if (body.compare(i, 2, "//") == 0)
        return false;
      else if (isalpha((unsigned char)body[i]) || body[i] == '_')
      {
        size_t e = i;
        while (e < body.size() && (isIdentChar(body[e]) || (body[e] == ':' && e + 1 < body.size() && body[e + 1] == ':')))
          e += body[e] == ':' ? 2 : 1;
        std::string name = body.substr(i, e - i);
        size_t after = e;
        while (after < body.size() && (body[after] == ' ' || body[after] == '\t'))
          ++after;
        std::string last = name.substr(name.rfind(':') == std::string::npos ? 0 : name.rfind(':') + 1);
        if (after < body.size() && body[after] == '(' && !notCalls.count(last) && !functionMacroNames.count(name))
          return true;
        i = e;
      }
      else
        ++i;
    }
    return false;
  }

  // Whether `#define name body` makes name another name for a function or variable, written out at
  // its uses as the preprocessor does: override_printf.h's `#define sprintf trio_sprintf`, which
  // Lua's C++ build is force-included with so that it formats as trio does (a constant of it would
  // be another sprintf beside the library's). A name in capitals keeps its constant.
  bool isAliasMacro(const std::string& name, const std::string& body)
  {
    if (body.empty() || body == name || isAllCapsName(name) || !(isalpha((unsigned char)body[0]) || body[0] == '_'))
      return false;
    for (char c : body)
      if (!(isalnum((unsigned char)c) || c == '_'))
        return false;
    return true;
  }

  // What a spliced macro (isSplicedMacroBody) is written out as: its body without comments and
  // without calling conventions, which kpp has none of (SSE.hpp's `#define VCALL __vectorcall` is
  // nothing), the spliced and string macros it names (in objects) written out in turn
  // (lobject.h's ClosureHeader begins with CommonHeader).
  std::string splicedMacroText(std::string body, const std::map<std::string, std::string>& objects, int depth = 0)
  {
    for (size_t c = body.find("/*"); c != std::string::npos; c = body.find("/*", c))
    {
      size_t e = body.find("*/", c + 2);
      body.replace(c, (e == std::string::npos ? body.size() : e + 2) - c, " ");
    }
    static const std::set<std::string> conventions = {"__vectorcall", "__cdecl", "__stdcall", "__fastcall", "__thiscall"};
    std::string out;
    for (size_t i = 0; i < body.size();)
    {
      if (body[i] == '"' || (body[i] == '\'' && !isDigitSeparator(body, i)))
      {
        size_t e = i + 1;
        while (e < body.size() && body[e] != body[i])
          e += body[e] == '\\' ? 2 : 1;
        e = std::min(e + 1, body.size());
        out.append(body, i, e - i);
        i = e;
      }
      else if (isalpha((unsigned char)body[i]) || body[i] == '_')
      {
        size_t e = i;
        while (e < body.size() && (isalnum((unsigned char)body[e]) || body[e] == '_'))
          ++e;
        std::string word = body.substr(i, e - i);
        i = e;
        if (conventions.count(word))
          continue;
        if (word == "NULL")
        {
          out += "nullptr"; // lobject.h's NILCONSTANT, `{NULL}, LUA_TNIL`
          continue;
        }
        if (auto known = knownMacros.find(word); known != knownMacros.end())
        {
          out += known->second; // as at its uses in the source (NILCONSTANT's LUA_TNIL: 0, whose #define goes)
          continue;
        }
        auto o = objects.find(word);
        if (o != objects.end() && depth < 8 && (isSplicedMacroBody(o->second) || isStringLiterals(o->second)))
          out += splicedMacroText(o->second, objects, depth + 1);
        else
          out += word;
      }
      else
        out += body[i] == '\t' ? ' ' : body[i], ++i;
    }
    size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
  }

