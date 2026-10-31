#include "convert.hpp"
#include "parser.hpp"
#include "source.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace
{
  struct Edit
  {
    uint32_t offset;
    uint32_t length; // 0: an insertion
    std::string text;
  };

  struct Counts
  {
    std::map<std::string, int> values;
    void add(const std::string& what, int n = 1) { values[what] += n; }
  };

  // Default arguments of a function's parameters, by index, as source text: carried from a
  // header declaration to the definition that replaces it in the converted file.
  using CarriedDefaults = std::map<FunctionDecl*, std::vector<std::pair<size_t, std::string>>>;
  // A class template's definition -> (parameter index, default text) from its forward declarations
  // (`template<class K, class V, class Comp = std::less<K>> class FlatMap;` in FlatMapForwards.hpp).
  using CarriedTemplateDefaults = std::map<Decl*, std::vector<std::pair<size_t, std::string>>>;
  // The names each namespace and class of the program declares directly (`NoiseExpressions::` ->
  // {"String", ...}; a class's own name included, as C++ injects it), by the scope's qualified name
  // and `::`: a library name written into such a scope has to be `::String`.
  using ScopeNames = std::map<std::string, std::set<std::string>>;

  // The index of the `,` or `)` that ends the parameter (or default argument) starting at token,
  // skipping nested brackets; UINT32_MAX when a `;` or the end comes first.
  uint32_t parameterEndIn(const std::vector<Token>& toks, uint32_t token)
  {
    int depth = 0;
    for (uint32_t i = token; i < toks.size(); ++i)
    {
      const Token& t = toks[i];
      if (t.is(P::LParen) || t.is(P::LBracket) || t.is(P::LBrace) || t.is(P::Less))
        ++depth;
      else if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::RBrace) || t.is(P::Greater))
      {
        if (depth == 0)
          return t.is(P::RParen) ? i : UINT32_MAX;
        --depth;
      }
      else if (t.is(P::Shr))
        depth -= 2;
      else if (t.is(P::Comma) && depth == 0)
        return i;
      else if (t.is(P::Semi) || t.getKind() == TokenKind::End)
        return UINT32_MAX;
    }
    return UINT32_MAX;
  }

  bool isIdentChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

  // Whether the `'` at i separates a number's digits (`0b1111'0000`, C++14) rather than starting a
  // character literal: the run of letters, digits, dots and quotes before it starts with a digit.
  bool isDigitSeparator(std::string_view text, size_t i)
  {
    size_t start = i;
    while (start > 0 && (isIdentChar(text[start - 1]) || text[start - 1] == '.' || text[start - 1] == '\''))
      --start;
    return start < i && isdigit((unsigned char)text[start]);
  }

  // Where the raw string literal whose `"` is at i ends (after its closing quote), or npos when the
  // `"` starts none: `R"(game.print(... "doesn't" ...))"` (the tests' TestAudioGeneral.cpp, inside a
  // CHECK_EQUAL) holds quotes and parentheses that end nothing.
  size_t rawStringEnd(std::string_view text, size_t i)
  {
    if (i == 0 || text[i] != '"' || text[i - 1] != 'R')
      return std::string_view::npos;
    size_t prefix = i - 1;
    while (prefix > 0 && (text[prefix - 1] == 'L' || text[prefix - 1] == 'u' || text[prefix - 1] == 'U' || text[prefix - 1] == '8'))
      --prefix;
    if (prefix > 0 && isIdentChar(text[prefix - 1]))
      return std::string_view::npos;
    size_t open = text.find('(', i + 1);
    if (open == std::string_view::npos || open - i - 1 > 16)
      return std::string_view::npos;
    std::string closing = ")" + std::string(text.substr(i + 1, open - i - 1)) + "\"";
    size_t end = text.find(closing, open + 1);
    return end == std::string_view::npos ? std::string_view::npos : end + closing.size();
  }

  // A macro's text with its line continuations (a backslash ending a line, or the text: Agui's
  // Transform.cpp continues `#define E(x, y)` onto an empty line) and its line ends as spaces.
  // Every other backslash stays: an escape in a literal (llex.c's `#define currIsNewline(ls)
  // (ls->current == '\n' || ls->current == '\r')`).
  std::string continuationsAsSpaces(std::string text)
  {
    for (size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\\')
      {
        size_t e = i + 1;
        while (e < text.size() && (text[e] == ' ' || text[e] == '\t' || text[e] == '\r'))
          ++e;
        if (e == text.size() || text[e] == '\n')
          text[i] = ' ';
        else
          ++i; // an escape: the character it escapes stays too (`'\\'`)
      }
      else if (text[i] == '\r' || text[i] == '\n')
        text[i] = ' ';
    return text;
  }

  // Whether a block comment is open at the end of text, given whether one was at its start: a `/*`
  // without its `*/`, outside literals and line comments.
  bool blockCommentOpenAfter(std::string_view text, bool open)
  {
    for (size_t i = 0; i < text.size(); ++i)
    {
      if (open)
      {
        size_t close = text.find("*/", i);
        if (close == std::string_view::npos)
          return true;
        open = false;
        i = close + 1;
      }
      else if (text[i] == '"' || (text[i] == '\'' && !isDigitSeparator(text, i)))
      {
        char quote = text[i];
        for (++i; i < text.size() && text[i] != quote && text[i] != '\n'; ++i)
          if (text[i] == '\\')
            ++i;
      }
      else if (text.compare(i, 2, "//") == 0)
        return false;
      else if (text.compare(i, 2, "/*") == 0)
      {
        open = true;
        ++i;
      }
    }
    return open;
  }
  bool opensBlockComment(std::string_view text) { return blockCommentOpenAfter(text, false); }

  // --- the preprocessor configuration ---

  // The build the converter preprocesses the game as: Factorio's FastDebug configuration for
  // Windows x64 with Visual Studio 2022, from fastbuild/fbuild.bff (the Windows `.defines`, the x64
  // `.architectureSpecificDefines`, vs2022's `.solutionSpecificDefines`, FastDebug's
  // `.buildTypeSpecificDefines` and the factorio project's `.librarySpecificDefines` with
  // `.sdlDefines`), then what MSVC itself predefines for x64 and C++, and the Windows SDK names the
  // game tests (sdkddkver.h's version numbers; ENABLE_VIRTUAL_TERMINAL_PROCESSING,
  // DISABLE_NEWLINE_AUTO_RETURN and GUID_DEFINED, which consoleapi.h and guiddef.h define). Name ->
  // body. docs/path-to-factorio.md, "The configuration".
  const std::map<std::string, std::string> buildDefines = {
    // .defines (Windows)
    {"WIN32_LEAN_AND_MEAN", ""}, {"CRT_NONSTDC_NO_DEPRECATE", ""}, {"_CRT_SECURE_NO_WARNINGS", ""}, {"_SCL_SECURE_NO_WARNINGS", ""},
    {"_WINDOWS", ""}, {"_USE_MATH_DEFINES", ""}, {"USE_THREADS", ""}, {"WIN32", ""}, {"_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS", ""},
    {"LUA_USE_LONGLONG", ""}, {"FACTORIO_USE_D3D", ""},
    // .architectureSpecificDefines (x64), .solutionSpecificDefines (vs2022)
    {"WIN64", ""}, {"_WIN32_WINNT", "0x0601"},
    // FastDebug's .buildTypeSpecificDefines
    {"_DEBUG", ""}, {"DEBUG", ""}, {"USE_COMMON_HPP", ""}, {"LUA_USE_APICHECK", ""}, {"DISABLE_EXPRESSION_ID", ""}, {"SPACE_AGE", ""},
    // the factorio project's .librarySpecificDefines and .sdlDefines
    {"NOMINMAX", ""}, {"BUILDING_WARNLESS_C", ""}, {"CURL_STATICLIB", ""}, {"SDL_MAIN_HANDLED", ""}, {"SDL_SENSOR_DISABLED", ""},
    // the lua project's .librarySpecificDefines (.luaConfig); only Lua's own files test it
    {"LUA_COMPAT_ALL", ""},
    // what each of Lua's core and library files defines before its includes (lapi.c's `#define
    // LUA_CORE`, lauxlib.c's `#define LUA_LIB`), together: luaconf.h's sections they open only
    // define (luai_numeq, luai_writestring); only Lua's own files test them
    {"LUA_CORE", ""}, {"LUA_LIB", ""},
    // and the names of the files that luaconf.h and llimits.h define a macro for (lobject.c's and
    // lvm.c's luai_nummod, ltable.c's luai_hashnum), only expanded there
    {"lobject_c", ""}, {"lvm_c", ""}, {"ltable_c", ""},
    // the C runtime's limits.h, which luaconf.h includes and tests (`#if INT_MAX-20 < 32760`: Lua's
    // LUAI_BITSINT, and with it LUAI_MAXSTACK and the types Lua counts its memory in)
    {"INT_MAX", "2147483647"},
    // MSVC for x64, C++ with the standard library (<optional> defines __cpp_lib_optional)
    {"_WIN32", "1"}, {"_WIN64", "1"}, {"_MSC_VER", "1944"}, {"_M_X64", "100"}, {"_M_AMD64", "100"}, {"__cplusplus", "199711L"},
    {"__cpp_lib_optional", "202110L"},
    // C++17's __has_include is a defined name too (`#if !defined (__has_include)`: TestCpp's
    // HasInclude tests); the #if evaluator answers __has_include(<...>) from availableHeaders.
    {"__has_include", ""},
    // the Windows SDK
    {"_WIN32_WINNT_NT4", "0x0400"}, {"_WIN32_WINNT_WIN2K", "0x0500"}, {"_WIN32_WINNT_WINXP", "0x0501"}, {"_WIN32_WINNT_WS03", "0x0502"},
    {"_WIN32_WINNT_VISTA", "0x0600"}, {"_WIN32_WINNT_LONGHORN", "0x0600"}, {"_WIN32_WINNT_WIN7", "0x0601"}, {"_WIN32_WINNT_WIN8", "0x0602"},
    {"_WIN32_WINNT_WINBLUE", "0x0603"}, {"_WIN32_WINNT_WINTHRESHOLD", "0x0A00"}, {"_WIN32_WINNT_WIN10", "0x0A00"},
    {"ENABLE_VIRTUAL_TERMINAL_PROCESSING", "0x0004"}, {"DISABLE_NEWLINE_AUTO_RETURN", "0x0008"}, {"GUID_DEFINED", ""}};
  // What the game tests that this build leaves undefined, known to be: other platforms, compilers
  // and architectures; the flags of other build types (Steam, demo, headless, packaged, the
  // all-in-one and unity builds, sanitizers, Tracy, the packager tool); cxxopts as the game builds
  // it (without ICU, with RTTI); and the debug switches the sources keep commented out
  // (`// #define ENABLE_DEBUG_LOG`). Every other symbol that no build flag and no converted file
  // defines is undefined too, as in C, and its #if gets a TODO.
  const std::set<std::string> undefinedOnPurpose = {
    "LINUX", "MAC_OSX", "MACOSX", "__APPLE__", "__linux__", "NINTENDO_SWITCH", "NINTENDO_SWITCH1", "NINTENDO_SWITCH2",
    "NINTENDO_SWITCH_PROFILER", "FACTORIO_USE_METAL", "__GNUC__", "__clang__", "_GLIBCXX_RELEASE", "__ARM_ARCH", "_LP64", "__SSE2__",
    "_M_IX86_FP", "ARCH_X86_64", "ARCH_ARM64", "_AddressOfReturnAddress", "NDEBUG", "STEAM", "DEMO", "DISABLE_UPDATER", "HEADLESS",
    "FACTORIO_PACKAGE", "WEGAME", "STEM", "ALL_IN_ONE", "UNITY_BUILD", "PACKAGER", "TRACY_ENABLE", "__SANITIZE_ADDRESS__", "ASAN",
    "TSAN", "UBSAN", "GLAD_DEBUG", "CXXOPTS_USE_UNICODE", "CXXOPTS_NO_RTTI", "ENABLE_DEBUG_LOG", "NOISE_DEBUG_ENABLED",
    "DEBUG_SPRITE_POOL", "TILE_RENDER_BLUR_POLLUTION", "SCALAR_VECTOR_ORIENTATION", "LOADING_TIMER_ENABLED", "LOAD_TIME_PROFILER",
    "PRINT_SIZES", "CHECK_DATA_CONSISTENCY", "FEEDBACK_ENABLED", "FORCE_STATIC_INIT_ENABLED", "GPU_IHV_API_ENABLED",
    "DEBUG_INVALID_REPAIR_ORDERS", "DEBUG_MAP_DESERIALIZATION", "DEBUG_DANGLING_TRAIN_STOP_POINTERS", "DEBUG_PLATFORM_BOUNDS",
    "NVIDIA_G7X_HARDWARE_BUG_FIX", "NVIDIA_7X_HARDWARE_BUG_FIX", "DISABLE_LOG_TIME", "SUPPRESS_FLOAT_CAST", "WINDOWS"};
  // The game's sources that its Windows build does not compile (fastbuild/fbuild.bff,
  // .factorioFilesForOtherPlatforms): PngLoad.cpp is Linux's image loader (`#error Only for
  // linux`), StacktraceToStream.cpp the stack trace printer of the other platforms, and UnitTest++'s
  // Posix directory, which the unit-test library's Windows build excludes. Not converted.
  const std::set<std::string> filesForOtherPlatforms = {"src/Graphics/PngLoad.cpp", "src/Util/StacktraceToStream.cpp",
                                                        "UnitTest++/Posix/SignalTranslator.cpp", "UnitTest++/Posix/SignalTranslator.h"};
  // The library files the game's build leaves out (fbuild.bff's .CompilerInputExcludedFiles): Lua's
  // standalone interpreter and compiler, by their directory and name.
  const std::set<std::string> filesNotBuilt = {"Lua/lua.c", "Lua/luac.c"};
  // The headers the game's build force-includes in every file of a project (fbuild.bff's /FI, the
  // precompiled headers), by their path below a root: included even where no file names them.
  const std::set<std::string> forceIncludedHeaders = {"src/Common.hpp", "Agui/AguiCommon.hpp", "tests/TestCommon.hpp"};
  // A project's header whose macros all its files see first, as through its force-included header:
  // the tests' UnitTest.hpp (UnitTest++), which TestCommon.hpp brings in (SelectionToolDrawResult.hpp
  // includes no UnitTest++ header and uses CHECK_FAILED). Not TestCommon.hpp itself: the conversion
  // reads the game's Common.hpp's debug assert and Lua's printf override among its macros.
  const std::map<std::string, std::string> forcedMacroHeaders = {{"tests/", "tests/UnitTest.hpp"}};
  // Headers that use macros of a header they do not include, which each of their includers includes
  // before them: LuaPrototypeFixture.hpp's CHECK_LUA_NUMBER_EQUALS_SOURCELOCATION is
  // LuaCheckMacros.hpp's (since 9c29b449ce4). The conversion checks that every includer does so.
  const std::map<std::string, std::string> includerMacroHeaders = {{"tests/LuaPrototypeFixture.hpp", "tests/LuaCheckMacros.hpp"}};
  // The headers a library's build force-includes (fbuild.bff: Lua's `/FI"override_printf.h"`), by
  // the library's directory name: its source files see them before their own includes (its headers
  // only where those source files include them).
  const std::map<std::string, std::string> forcedIncludes = {{"Lua", "override_printf.h"}};
  // The C libraries a library keeps calling, by the library's directory name (FileConverter's
  // keptCLibraries): Lua formats through trio, as its C++ build does.
  const std::map<std::string, std::set<std::string>> keptCLibraries = {{"Lua", {"trio"}}};
  // The standard headers `__has_include(<...>)` finds: the ones the game asks about, all in MSVC's
  // library (the experimental one is deprecated there, still present).
  const std::set<std::string> availableHeaders = {"filesystem", "experimental/filesystem", "optional", "variant", "string_view"};

  // Object-like macros by name, with their bodies (empty for `#define NAME`).
  using MacroTable = std::map<std::string, std::string>;

  // Evaluates an #if expression as C's preprocessor does, in long long: defined(X), X (its body,
  // evaluated in turn), __has_include, numbers, the unary ! (or `not`), - + ~, then * / %, + -,
  // << >>, the comparisons, & ^ |, && and || (`and`, `or`), with C's precedence and parentheses
  // (luaconf.h's `#if INT_MAX-20 < 32760`, lopcodes.h's `#if SIZE_Bx < LUAI_BITSINT-1`). A name the
  // table does not have is 0; those neither in undefinedOnPurpose nor in `known` (the names some
  // converted file defines, in a branch this build drops) are collected in `undefined`.
  struct IfEval
  {
    std::string s;
    const MacroTable& table;
    std::set<std::string>* undefined = nullptr;
    const std::set<std::string>* known = nullptr;
    int depth = 0;
    size_t p = 0;
    bool unexpected(const std::string& name) const { return !undefinedOnPurpose.count(name) && !(known && known->count(name)); }
    void skipSpace()
    {
      while (p < s.size() && isspace((unsigned char)s[p]))
        ++p;
    }
    long long symbol(const std::string& name)
    {
      auto it = table.find(name);
      if (it == table.end())
      {
        if (undefined && unexpected(name))
          undefined->insert(name);
        return 0;
      }
      if (it->second.empty())
        return 1;
      if (depth > 8)
        return 0;
      IfEval body{it->second, table, undefined, known, depth + 1};
      return body.orExpr();
    }
    bool isDefined(const std::string& name)
    {
      if (table.count(name))
        return true;
      if (undefined && unexpected(name))
        undefined->insert(name);
      return false;
    }
    std::string word()
    {
      size_t start = p;
      while (p < s.size() && isIdentChar(s[p]))
        ++p;
      return s.substr(start, p - start);
    }
    long long primary()
    {
      skipSpace();
      if (p < s.size() && s[p] == '!')
      {
        ++p;
        return !primary();
      }
      if (p < s.size() && (s[p] == '-' || s[p] == '+' || s[p] == '~'))
      {
        char op = s[p++];
        long long v = primary();
        return op == '-' ? -v : op == '~' ? ~v : v;
      }
      if (p < s.size() && s[p] == '(')
      {
        ++p;
        long long v = orExpr();
        skipSpace();
        if (p < s.size() && s[p] == ')')
          ++p;
        return v;
      }
      if (p < s.size() && isdigit((unsigned char)s[p]))
      {
        long long v = strtoll(s.c_str() + p, nullptr, 0);
        while (p < s.size() && isIdentChar(s[p]))
          ++p;
        return v;
      }
      std::string name = word();
      if (name == "not")
        return !primary();
      if (name == "defined")
      {
        skipSpace();
        bool paren = p < s.size() && s[p] == '(';
        if (paren)
          ++p;
        skipSpace();
        std::string sym = word();
        skipSpace();
        if (paren && p < s.size() && s[p] == ')')
          ++p;
        return isDefined(sym);
      }
      if (name == "__has_include")
      {
        size_t open = s.find_first_of("<\"", p);
        size_t close = open == std::string::npos ? open : s.find_first_of(">\"", open + 1);
        size_t paren = close == std::string::npos ? close : s.find(')', close);
        if (paren == std::string::npos)
          return 0;
        std::string header = s.substr(open + 1, close - open - 1);
        p = paren + 1;
        return availableHeaders.count(header) != 0;
      }
      if (name.empty())
      {
        ++p;
        return 0;
      }
      return symbol(name);
    }
    // Whether the operator op comes next (and not a longer one it starts: `<` is not `<<` or `<=`,
    // `&` not `&&`); taken when it does.
    bool next(const char* op)
    {
      skipSpace();
      size_t n = strlen(op);
      if (s.compare(p, n, op) != 0)
        return false;
      char after = p + n < s.size() ? s[p + n] : 0;
      if ((n == 1 && (op[0] == '<' || op[0] == '>') && (after == op[0] || after == '=')) || (n == 1 && (op[0] == '&' || op[0] == '|') && after == op[0]) ||
          (n == 1 && op[0] == '!' && after == '=') || (n == 2 && (op[0] == '<' || op[0] == '>') && op[1] == op[0] && after == '='))
        return false;
      p += n;
      return true;
    }
    long long mulExpr()
    {
      long long l = primary();
      for (;;)
      {
        if (next("*"))
          l *= primary();
        else if (next("/"))
        {
          long long r = primary();
          l = r == 0 ? 0 : l / r;
        }
        else if (next("%"))
        {
          long long r = primary();
          l = r == 0 ? 0 : l % r;
        }
        else
          return l;
      }
    }
    long long addExpr()
    {
      long long l = mulExpr();
      for (;;)
      {
        if (next("+"))
          l += mulExpr();
        else if (next("-"))
          l -= mulExpr();
        else
          return l;
      }
    }
    long long shiftExpr()
    {
      long long l = addExpr();
      for (;;)
      {
        if (next("<<"))
          l <<= addExpr();
        else if (next(">>"))
          l >>= addExpr();
        else
          return l;
      }
    }
    long long relExpr()
    {
      long long l = shiftExpr();
      for (;;)
      {
        if (next("<="))
          l = l <= shiftExpr();
        else if (next(">="))
          l = l >= shiftExpr();
        else if (next("<"))
          l = l < shiftExpr();
        else if (next(">"))
          l = l > shiftExpr();
        else
          return l;
      }
    }
    long long cmpExpr()
    {
      long long l = relExpr();
      for (;;)
      {
        if (next("=="))
          l = l == relExpr();
        else if (next("!="))
          l = l != relExpr();
        else
          return l;
      }
    }
    long long bitAndExpr()
    {
      long long l = cmpExpr();
      while (next("&"))
        l &= cmpExpr();
      return l;
    }
    long long bitXorExpr()
    {
      long long l = bitAndExpr();
      while (next("^"))
        l ^= bitAndExpr();
      return l;
    }
    long long bitExpr()
    {
      long long l = bitXorExpr();
      while (next("|"))
        l |= bitXorExpr();
      return l;
    }
    long long andExpr()
    {
      long long l = bitExpr();
      for (;;)
      {
        skipSpace();
        if (s.compare(p, 2, "&&") == 0)
          p += 2;
        else if (s.compare(p, 3, "and") == 0 && (p + 3 >= s.size() || !isIdentChar(s[p + 3])))
          p += 3;
        else
          return l;
        long long r = bitExpr();
        l = l && r;
      }
    }
    long long orExpr()
    {
      long long l = andExpr();
      for (;;)
      {
        skipSpace();
        if (s.compare(p, 2, "||") == 0)
          p += 2;
        else if (s.compare(p, 2, "or") == 0 && (p + 2 >= s.size() || !isIdentChar(s[p + 2])))
          p += 2;
        else
          return l;
        long long r = andExpr();
        l = l || r;
      }
    }
  };

  // The length of the UTF-8 byte order mark a source file starts with (a few of the game's files
  // have one), or 0. The lexer skips it; the line scans here treat it as space, and the converted
  // text leaves it out (after the header comment it would read as a name).
  size_t byteOrderMarkLength(std::string_view text)
  {
    return text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF ? 3 : 0;
  }

  // A file as the configuration preprocesses it.
  struct Preprocessed
  {
    std::string configured;                              // the text, the dropped lines and every conditional directive blanked
    std::vector<std::pair<uint32_t, uint32_t>> dropped;  // the [begin, end) offsets of the lines the configuration drops
    std::vector<std::pair<uint32_t, std::string>> notes; // the #if lines that test a symbol nothing defines, with a TODO text
    MacroTable defined;                                  // the object-like macros it leaves defined beyond the build's (its includes' too)
    std::set<std::string> undefined;                     // the build's or its includes' macros it #undefs
    // What blankForParse took out of `configured` for the parse: [offset, offset + length) of the
    // text, and what the conversion writes there instead.
    struct Blank { uint32_t offset, length; std::string replacement; };
    std::vector<Blank> blanked;
  };

  // Walks a file's preprocessor lines under the configuration: `#if` and its relatives choose the
  // lines that stay, an active `#define` or `#undef` changes what later lines see, and an active
  // `#include` of a converted file adds what that file leaves defined (`included` gives it, or null
  // for any other header). A `#ifndef X` followed by `#define X` is an include guard, not a test.
  // `known`: the names the converted files define anywhere, which no TODO calls unexpected.
  Preprocessed preprocessText(std::string_view text, const std::function<const Preprocessed*(const std::string&)>& included,
                              const std::set<std::string>* known = nullptr)
  {
    Preprocessed out;
    out.configured = std::string(text);
    MacroTable table = buildDefines;
    auto blank = [&](size_t from, size_t to)
    {
      for (size_t i = from; i < to && i < out.configured.size(); ++i)
        if (out.configured[i] != '\n' && out.configured[i] != '\r')
          out.configured[i] = ' ';
    };
    struct Level
    {
      bool parentActive, taken;
    };
    std::vector<Level> stack;
    bool active = true;
    size_t dropStart = SIZE_MAX;
    std::string guard; // the symbol of a #ifndef that the next directive may define, as a guard
    size_t guardNote = SIZE_MAX;
    auto setActive = [&](bool now, size_t at)
    {
      if (active && !now)
        dropStart = at;
      else if (!active && now && dropStart != SIZE_MAX)
      {
        out.dropped.push_back({uint32_t(dropStart), uint32_t(at)});
        dropStart = SIZE_MAX;
      }
      active = now;
    };
    auto evaluate = [&](const std::string& expr, uint32_t at, const std::string& line)
    {
      std::set<std::string> undefined;
      IfEval e{expr, table, &undefined, known};
      bool value = e.orExpr() != 0;
      if (!undefined.empty())
      {
        std::string names;
        for (const std::string& name : undefined)
          names += (names.empty() ? "" : ", ") + name;
        out.notes.push_back({at, line + ": " + names + " defined by no build flag and no converted file it includes, so undefined"});
      }
      return value;
    };
    size_t pos = 0;
    // Whether a block comment is open where the line starts: its `#` lines are comment, as C removes
    // comments before it reads directives (TestSpellcheck.cpp comments out a test body with its
    // #ifdef WIN32 ... `#endif*/`).
    bool inComment = false;
    while (pos < text.size())
    {
      // A logical line: continuations joined.
      size_t start = pos, end = text.find('\n', pos);
      if (end == std::string_view::npos)
        end = text.size();
      while (end < text.size())
      {
        size_t last = end;
        while (last > start && (text[last - 1] == '\r' || text[last - 1] == ' '))
          --last;
        if (last <= start || text[last - 1] != '\\')
          break;
        end = text.find('\n', end + 1);
        if (end == std::string_view::npos)
          end = text.size();
      }
      pos = end + 1;
      bool commentAtStart = inComment;
      inComment = blockCommentOpenAfter(text.substr(start, end - start), inComment);
      size_t p = start == 0 ? byteOrderMarkLength(text) : start;
      while (p < end && (text[p] == ' ' || text[p] == '\t'))
        ++p;
      if (commentAtStart || p >= end || text[p] != '#')
      {
        if (!active)
          blank(start, end);
        continue;
      }
      ++p;
      while (p < end && (text[p] == ' ' || text[p] == '\t'))
        ++p;
      size_t ws = p;
      while (p < end && isalpha((unsigned char)text[p]))
        ++p;
      std::string word(text.substr(ws, p - ws));
      std::string rest(text.substr(p, end - p));
      if (size_t c = rest.find("//"); c != std::string::npos)
        rest = rest.substr(0, c);
      if (size_t c = rest.find("/*"); c != std::string::npos)
        rest = rest.substr(0, c);
      for (char& ch : rest)
        if (ch == '\\' || ch == '\r')
          ch = ' ';
      auto firstWord = [](const std::string& r)
      {
        std::string sym;
        for (char ch : r)
          if (isIdentChar(ch))
            sym += ch;
          else if (!sym.empty())
            break;
        return sym;
      };
      std::string directive = "#" + word + rest;
      while (!directive.empty() && directive.back() == ' ')
        directive.pop_back();
      std::string pendingGuard = std::move(guard);
      size_t pendingNote = guardNote;
      guard.clear();
      guardNote = SIZE_MAX;
      if (word == "if" || word == "ifdef" || word == "ifndef")
      {
        bool value = false;
        if (active)
        {
          if (word == "if")
            value = evaluate(rest, uint32_t(start), directive);
          else
          {
            std::string sym = firstWord(rest);
            size_t notesBefore = out.notes.size();
            value = evaluate("defined(" + sym + ")", uint32_t(start), directive);
            if (word == "ifndef")
            {
              value = !value;
              guard = sym;
              guardNote = out.notes.size() > notesBefore ? notesBefore : SIZE_MAX;
            }
          }
        }
        stack.push_back({active, value});
        setActive(active && value, start);
        blank(start, end);
      }
      else if (word == "elif" || word == "else")
      {
        if (!stack.empty())
        {
          Level& l = stack.back();
          bool value = word == "else" || (l.parentActive && !l.taken && evaluate(rest, uint32_t(start), directive));
          bool now = l.parentActive && !l.taken && value;
          l.taken = l.taken || now;
          setActive(now, start);
        }
        blank(start, end);
      }
      else if (word == "endif")
      {
        if (!stack.empty())
        {
          setActive(stack.back().parentActive, start);
          stack.pop_back();
        }
        blank(start, end);
      }
      else if (!active)
        blank(start, end);
      else if (word == "define")
      {
        size_t n = rest.find_first_not_of(" \t");
        size_t e = n;
        while (e < rest.size() && isIdentChar(rest[e]))
          ++e;
        if (n != std::string::npos && e > n)
        {
          std::string name = rest.substr(n, e - n);
          std::string body;
          if (e >= rest.size() || rest[e] != '(')
          {
            size_t a = rest.find_first_not_of(" \t", e), b = rest.find_last_not_of(" \t");
            body = a == std::string::npos ? std::string() : rest.substr(a, b - a + 1);
          }
          // A function-like macro is defined for `defined()` too; its body is no #if value.
          table[name] = body;
          out.undefined.erase(name);
          if (name == pendingGuard && pendingNote != SIZE_MAX && pendingNote < out.notes.size())
            out.notes.erase(out.notes.begin() + pendingNote);
        }
      }
      else if (word == "undef")
      {
        std::string sym = firstWord(rest);
        if (table.erase(sym))
          out.undefined.insert(sym);
      }
      else if (word == "include" && included)
      {
        size_t open = rest.find_first_of("<\"");
        size_t close = open == std::string::npos ? open : rest.find_first_of(">\"", open + 1);
        if (close != std::string::npos)
          if (const Preprocessed* header = included(rest.substr(open + 1, close - open - 1)))
          {
            for (const auto& [name, body] : header->defined)
              table[name] = body;
            for (const std::string& name : header->undefined)
              table.erase(name);
          }
      }
    }
    if (!active && dropStart != SIZE_MAX)
      out.dropped.push_back({uint32_t(dropStart), uint32_t(text.size())});
    for (const auto& [name, body] : table)
      if (auto b = buildDefines.find(name); b == buildDefines.end() || b->second != body)
        out.defined[name] = body;
    for (const auto& [name, body] : buildDefines)
      if (!table.count(name))
        out.undefined.insert(name);
    return out;
  }

  // Known object-like and function-like macros of the corpus that have a direct Kpp spelling.
  const std::map<std::string, std::string> knownMacros = {
    {"FORMAT_ATTR", ""}, {"TAG_FORMAT_ATTR", ""}, {"NO_INLINE", "[[noinline]]"}, {"ALWAYS_INLINE", "[[forceinline]]"},
    {"NTAPI", ""}, {"WINAPI", ""}, {"APIENTRY", ""}, {"CALLBACK", ""}, {"STDMETHODCALLTYPE", ""}, {"EXTERNC", "extern \"C\""},
    {"FAR", ""}, {"NEAR", ""}, {"VOID", "void"}, {"NULL", "nullptr"},
    {"DEV_TOOL_VARIABLE", ""}, {"BEGIN_SUPPRESS_CONVERSIONS", ""}, {"END_SUPPRESS_CONVERSIONS", ""}, {"BEGIN_FORCE_OPTIMIZE", ""},
    {"END_FORCE_OPTIMIZE", ""}, {"SUPPRESS_UBSAN", ""}, {"PADDING_WARNING", ""}, {"errno", "(*_errno())"},
    // SDL_stdinc.h's format for an Sint64, which SDLWindow concatenates with a literal (Windows').
    {"SDL_PRIs64", "\"I64d\""},
    // C headers' constants that are casts, which the C import does not take for constants (winsock2.h,
    // signal.h, SDL_audio.h, SDL_touch.h).
    {"INVALID_SOCKET", "((SOCKET)(~0))"}, {"SIG_DFL", "((_crt_signal_t)0)"},
    {"SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK", "((SDL_AudioDeviceID)0xFFFFFFFFu)"}, {"SDL_AUDIO_DEVICE_DEFAULT_RECORDING", "((SDL_AudioDeviceID)0xFFFFFFFEu)"},
    {"SDL_TOUCH_MOUSEID", "((SDL_MouseID)(-1))"}, {"SDL_MOUSE_TOUCHID", "((SDL_TouchID)(-1))"},
    // SDL_hints.h's names, which the game joins to a literal (SoundInterfaceSDL's
    // `SDL_HINT_AUDIO_DRIVER " is set to \"%s\". Ignoring..."`): the literal itself.
    {"SDL_HINT_AUDIO_DRIVER", "\"SDL_AUDIO_DRIVER\""},
    {"EXCEPTION_ACCESS_VIOLATION", "((DWORD)0xC0000005L)"}, {"EXCEPTION_STACK_OVERFLOW", "((DWORD)0xC00000FDL)"},
    {"EXCEPTION_ILLEGAL_INSTRUCTION", "((DWORD)0xC000001DL)"}, {"E_NOT_VALID_STATE", "((HRESULT)0x8007139FL)"},
    {"INIT_ONCE_STATIC_INIT", "{}"}, {"CURLOPT_XFERINFODATA", "CURLOPT_PROGRESSDATA"},
    // luaconf.h's number format, a string the import does not make a constant (StringUtil formats
    // numbers as Lua writes them).
    {"LUA_NUMBER_FMT", "\"%.14g\""},
    // guiddef.h's reference to an interface id, which C (and so the import and the COM classes,
    // native/generate_com.py) has as a pointer (GDIPlus's IStream::QueryInterface).
    {"REFIID", "const IID*"},
    // Windows' functions with an A and a W form, as the game's narrow build has them.
    {"LoadLibrary", "LoadLibraryA"}, {"GetModuleHandle", "GetModuleHandleA"}, {"GetModuleHandleEx", "GetModuleHandleExA"},
    {"MapVirtualKey", "MapVirtualKeyA"}, {"GetModuleFileName", "GetModuleFileNameA"}, {"GetFileAttributes", "GetFileAttributesA"},
    {"GetEnvironmentVariable", "GetEnvironmentVariableA"}, {"WriteConsoleInput", "WriteConsoleInputA"},
    {"GetMonitorInfo", "GetMonitorInfoA"}, {"EnumDisplayDevices", "EnumDisplayDevicesA"}, {"EnumDisplaySettings", "EnumDisplaySettingsA"},
    // stb_dxt's static internals, which DXTCompression calls: native/header_only.c gives them C names
    // (native/include/kpp_stb_dxt.h).
    {"stb__InitDXT", "kpp_stb__InitDXT"}, {"stb__EvalColors", "kpp_stb__EvalColors"}, {"stb__DitherBlock", "kpp_stb__DitherBlock"},
    {"stb__MatchColorsBlock", "kpp_stb__MatchColorsBlock"}, {"stb__OptimizeColorsBlock", "kpp_stb__OptimizeColorsBlock"},
    {"stb__RefineBlock", "kpp_stb__RefineBlock"}, {"stb__CompressAlphaBlock", "kpp_stb__CompressAlphaBlock"},
    {"stb__OMatch5", "kpp_stb__OMatch5"}, {"stb__OMatch6", "kpp_stb__OMatch6"},
    // tchar.h's names, as the narrow build has them (StackWalker).
    {"_tcscat_s", "strcat_s"},
    // psapi.h's functions, which kernel32 has under K32 names (PSAPI_VERSION 2).
    {"GetProcessMemoryInfo", "K32GetProcessMemoryInfo"}, {"EnumProcessModules", "K32EnumProcessModules"},
    {"GetModuleInformation", "K32GetModuleInformation"}, {"GetModuleFileNameEx", "K32GetModuleFileNameExA"},
    {"GetPerformanceInfo", "K32GetPerformanceInfo"},
    {"GetFileVersionInfoSize", "GetFileVersionInfoSizeA"}, {"GetFileVersionInfo", "GetFileVersionInfoA"}, {"VerQueryValue", "VerQueryValueA"},
    // Tracy's other profiling macros, dropped as ZoneScoped is below.
    {"ZoneScopedNC", ""}, {"TracyMessage", ""}, {"TracyPlot", ""}, {"TracySystem", ""}, {"FrameMark", ""},
    // math.h constants
    {"M_PI", "3.14159265358979323846"}, {"M_PI_2", "1.57079632679489661923"}, {"M_PI_4", "0.78539816339744830962"},
    {"M_1_PI", "0.31830988618379067154"}, {"M_2_PI", "0.63661977236758134308"}, {"M_SQRT2", "1.41421356237309504880"},
    {"M_LN2", "0.69314718055994530942"}, {"M_LN10", "2.30258509299404568402"}, {"M_E", "2.71828182845904523536"},
    // Tracy profiling macros (stripped)
    {"ZoneScoped", ""}, {"ZoneScopedN", ""}, {"ZoneNamedN", ""}};
  // `MACRO(arg)` becomes the text with $1 standing for the argument (`toCStr(x)` is `toStr(x).c_str()`).
  // The game's Log macros become calls of Logging's functions with the use's file and line, as
  // Log.hpp defines them for this configuration: ENABLE_DEBUG_LOG is defined by no file, so
  // LOG_DEBUG and its relatives are `((void)0)` (a replacement without $1 keeps the use as a comment).
  const std::map<std::string, std::string> knownFunctionMacros = {{"FORMAT_STRING", "$1"}, {"WHEN_ASSERT", "$1"}, {"FOR_DEVELOPER", "$1"},
                                                                  {"toCStr", "toStr($1).c_str()"},
                                                                  // Scoped guards that needed a unique name per line are `_` locals.
                                                                  {"SAVING_TAG", "Serialiser::TagWriter _($1)"},
                                                                  {"SAVING_STATISTICS", "Serialiser::SavingStatsWriter _($1)"},
                                                                  {"LOG", "Logging::log(__FILE__, __LINE__, $1)"},
                                                                  {"LOG_INFO", "Logging::log(__FILE__, __LINE__, LogLevel::Info, $1)"},
                                                                  {"LOG_NOTICE", "Logging::log(__FILE__, __LINE__, LogLevel::Notice, $1)"},
                                                                  {"LOG_WARNING", "Logging::log(__FILE__, __LINE__, LogLevel::Warning, $1)"},
                                                                  {"LOG_ERROR", "Logging::log(__FILE__, __LINE__, LogLevel::Error, $1)"},
                                                                  {"LOG_DATA", "Logging::log(__FILE__, __LINE__, LogLevel::Data, $1)"},
                                                                  {"LOG_GRAPHICS", "Logging::log(__FILE__, __LINE__, LogLevel::Graphics, $1)"},
                                                                  {"LOG_FUTURE", "Logging::log(__FILE__, __LINE__, LogLevel::Future, $1)"},
                                                                  {"LOG_VERBOSE", "Logging::logVerbose(__FILE__, __LINE__, LogLevel::Verbose, $1)"},
                                                                  {"LOG_DEBUG", "((void)0)"}, {"EXECUTE_FOR_LOG_DEBUG", "((void)0)"}, {"LOG_DEBUG_VALUE", "((void)0)"},
                                                                  {"LOG_REQUEST_MINIDUMP_IN_CRASH_REPORT", "Logging::log(__FILE__, __LINE__, LogLevel::Notice, \"Include minidump in crash report.\")"},
                                                                  {"LOG_STACKTRACE", "Logging::logStackTrace()"}, {"FLUSH_LOG", "Logging::flush()"},
                                                                  // The debug C runtime's report settings (the tests' Main.cpp sends assertion
                                                                  // reports to stderr), as crtdbg.h has them without _DEBUG: nothing to set,
                                                                  // Kpp's asserts report themselves.
                                                                  {"_CrtSetReportMode", "((int)0)"}, {"_CrtSetReportFile", "((void*)0)"},
                                                                  {"LOG_AND_ABORT", "Logging::logAndAbortOrThrow(__FILE__, __LINE__, LogLevel::Error, $1)"},
                                                                  {"LOG_AND_ABORT_NEVER_THROW", "Logging::logAndAbortOrThrow(__FILE__, __LINE__, LogLevel::Error, $1)"},
                                                                  {"LOG_ENUM_AND_ABORT", "Logging::logEnumAndAbortOrThrow(__FILE__, __LINE__, LogLevel::Error, $1)"},
                                                                  {"SUPPRESS_NEXT", "++result.suppressN"},
                                                                  // The Windows headers' function-like macros the game uses, as
                                                                  // winerror.h, minwindef.h, winsock2.h and winnt.h (narrow build)
                                                                  // define them: the C import brings no function-like macros, and
                                                                  // the headers are too large to expand wholesale (minwindef.h's
                                                                  // min and max).
                                                                  {"FAILED", "(((HRESULT)($1)) < 0)"}, {"SUCCEEDED", "(((HRESULT)($1)) >= 0)"},
                                                                  {"LOBYTE", "((BYTE)(((DWORD_PTR)($1)) & 0xff))"},
                                                                  {"HIBYTE", "((BYTE)((((DWORD_PTR)($1)) >> 8) & 0xff))"},
                                                                  {"LOWORD", "((WORD)(((DWORD_PTR)($1)) & 0xffff))"},
                                                                  {"HIWORD", "((WORD)((((DWORD_PTR)($1)) >> 16) & 0xffff))"},
                                                                  {"FD_ZERO", "(((fd_set*)($1))->fd_count = 0)"}, {"TEXT", "$1"}, {"_T", "$1"},
                                                                  // zlib.h's inflateInit, as zlib.h defines it.
                                                                  {"inflateInit", "inflateInit_(($1), ZLIB_VERSION, (int)sizeof(z_stream))"}};
  // Macros that use an argument more than once or stringize it (`#NAME`): $N is argument N as
  // converted (with `this->` and the other rewrites), #N its original spelling as a string literal,
  // which is what the preprocessor's # makes. The prototype explorer's hand-written reflection
  // (macro survey case C) converts use by use, so the explorer shows what it showed before.
  const std::map<std::string, std::string> argumentMacros = {
    {"ADD_VALUE", "Values::addValue($1, PrototypeExplorerWidgets::demangleMemberName(#1), #1, result)"},
    {"SKIP_VALUE", "if constexpr (isReference<decltype($1)>) Values::recordReference(#1, result); else Values::recordValue($1, #1, result)"},
    {"ADD_ENUM", "Values::recordValue($1, #1, result); result.add($1, $2, $3)"},
    // Util.hpp's `verify(x)`: x evaluated in every build, asserted only where asserts are (its #ifndef
    // NDEBUG branch is `assert(x)`, which Kpp's release build compiles out with x:
    // TransitionalRequestBuffers' `verify(set.erase(silo))` erased nothing there).
    {"verify", "[&] { if constexpr (config::debug) { assert($1); } else { (void)($1); } }()"},
    // Windows' wingdi.h and winnt.h, as knownFunctionMacros above (with `(DWORD)((BYTE)(x))` where
    // wingdi.h has `(DWORD)(BYTE)(x)`, which Kpp asks to have written out).
    {"RGB", "((COLORREF)(((BYTE)($1) | ((WORD)((BYTE)($2)) << 8)) | (((DWORD)((BYTE)($3))) << 16)))"},
    {"MAKELANGID", "((((WORD)($2)) << 10) | (WORD)($1))"}, {"FD_ISSET", "__WSAFDIsSet((SOCKET)($1), (fd_set*)($2))"},
    {"FD_SET", "kppFdSet((SOCKET)($1), (fd_set*)($2))"},
    // winnt.h's InterlockedIncrement and InterlockedDecrement, MSVC intrinsics on x64: the new value,
    // by the atomic add the library's Atomic is made of (stdlib/core/Thread.kpp, codegen's intrinsic).
    {"InterlockedIncrement", "(kpp_atomic_add32((long*)($1), 1) + 1)"}, {"InterlockedDecrement", "(kpp_atomic_add32((long*)($1), -1) - 1)"},
    {"ZeroMemory", "memset(($1), 0, ($2))"},
    // stdlib.h's element count of an array, as its C definition has it (GraphicsInterfaceDX11's
    // `_countof(lvl)` of the feature levels).
    {"_countof", "(sizeof($1) / sizeof(($1)[0]))"},
    // d3d11.h's inline function, which it has only for C++ (the import's C has none).
    {"D3D11CalcSubresource", "((UINT)($1) + (UINT)($2) * (UINT)($3))"}};

  // The preprocessor's #: the spelling with each run of whitespace one space, `"` and `\` escaped.
  std::string stringize(std::string_view spelling)
  {
    std::string out = "\"";
    bool space = false;
    for (char c : spelling)
    {
      if (isspace((unsigned char)c))
      {
        space = true;
        continue;
      }
      if (space && out.size() > 1)
        out += ' ';
      space = false;
      if (c == '"' || c == '\\')
        out += '\\';
      out += c;
    }
    return out + "\"";
  }

  // The class body Util/Enums.hpp's ENUM_IMPL_NOSTRING(Name, Type, values...) expands to, and with
  // strings the part ENUM_IMPL adds (str() and construction from text, with the names as
  // StringUtil::addDashes spells them).
  std::string enumClassBody(const std::string& name, const std::string& type, const std::vector<std::string>& values, bool strings)
  {
    std::string list, validate;
    for (const std::string& v : values)
    {
      list += (list.empty() ? "      " : ",\n      ") + v;
      validate += "        case Enum::" + v + ": return true;\n";
    }
    std::string out =
      "  public:\n"
      "    enum class Enum : " + type + "\n"
      "    {\n" + list + "\n"
      "    };\n"
      "\n"
      "    using UnderlyingType = " + type + ";\n"
      "\n"
      "    " + name + "() = delete;\n"
      "    implicit constexpr " + name + "(Enum value) : value(value) {}\n"
      "    constexpr explicit " + name + "(" + type + " value) : value(static_cast<Enum>(value)) { this->validate(); }\n"
      "\n"
      "    explicit constexpr operator " + type + "() const { return static_cast<" + type + ">(this->value); }\n"
      "    implicit constexpr operator Enum() const { return this->value; }\n"
      "    constexpr " + type + " asType() const { return static_cast<" + type + ">(this->value); }\n"
      "    constexpr Enum asEnum() const { return this->value; }\n"
      "    constexpr bool isValid() const\n"
      "    {\n"
      "      switch (this->value)\n"
      "      {\n" + validate +
      "      }\n"
      "      return false;\n"
      "    }\n"
      "\n"
      "    constexpr void validate() const\n"
      "    {\n"
      "      if (!this->isValid())\n"
      "        throw GameRuntimeError(\"Invalid enum value \\\"\" + toString(this->asType()) + \"\\\" of " + name + "\");\n"
      "    }\n"
      "\n"
      "    constexpr bool operator==(Enum other) const { return this->value == other; }\n"
      "    constexpr bool operator!=(Enum other) const { return this->value != other; }\n"
      "    constexpr bool operator==(" + name + " other) const { return this->value == other.value; }\n"
      "    constexpr bool operator!=(" + name + " other) const { return this->value != other.value; }\n"
      "\n"
      "    using enum Enum;\n"
      "    static constexpr size_t value_count = " + std::to_string(values.size()) + ";\n"
      "    static constexpr EnumItems<" + name + ", 0, value_count> values() { return {}; }\n"
      "\n"
      "  protected:\n"
      "    Enum value;\n";
    if (strings)
    {
      std::string toStr, fromStr;
      for (const std::string& v : values)
      {
        toStr += "        case Enum::" + v + ": return StringUtil::addDashes(\"" + v + "\");\n";
        fromStr += "      if (StringUtil::compare(val, StringUtil::addDashes(\"" + v + "\").c_str(), true))\n        return Enum::" + v + ";\n";
      }
      out +=
        "  public:\n"
        "    explicit " + name + "(const String& str) : " + name + "(str.c_str()) {}\n"
        "    explicit " + name + "(const char* str) : value(" + name + "::findValue(str)) {}\n"
        "\n"
        "    String str() const\n"
        "    {\n"
        "      switch (this->value)\n"
        "      {\n" + toStr +
        "      }\n"
        "      return \"\";\n"
        "    }\n"
        "\n"
        "  protected:\n"
        "    static Enum findValue(const char* val)\n"
        "    {\n" + fromStr +
        "      throw GameRuntimeError(\"Invalid value \\\"\" + String(val) + \"\\\" of " + name + "\");\n"
        "    }\n";
    }
    return out + "  private:\n";
  }

  // How many arguments an argument macro takes: the highest N of its $N and #N.
  size_t arityOf(const std::string& replacement)
  {
    size_t arity = 0;
    for (size_t i = 0; i + 1 < replacement.size(); ++i)
      if ((replacement[i] == '$' || replacement[i] == '#') && isdigit((unsigned char)replacement[i + 1]))
        arity = std::max(arity, size_t(replacement[i + 1] - '0'));
    return arity;
  }

  // An argument macro's replacement: $N and #N as argumentMacros describes.
  std::string expandArguments(const std::string& replacement, const std::vector<std::string>& args, const std::vector<std::string>& spellings)
  {
    std::string out;
    for (size_t i = 0; i < replacement.size(); ++i)
    {
      char c = replacement[i];
      if ((c == '$' || c == '#') && i + 1 < replacement.size() && isdigit((unsigned char)replacement[i + 1]))
      {
        size_t n = size_t(replacement[++i] - '1');
        if (n < args.size())
          out += c == '$' ? args[n] : stringize(spellings[n]);
        continue;
      }
      out += c;
    }
    return out;
  }

  // std:: names and their Kpp library spelling. Names not here keep their name and are counted.
  // C library functions become `::name`: inside a namespace with its own `sqrt`, `std::sqrt` meant the C one.
  const std::map<std::string, std::string> stdNames = {
    {"string", "String"}, {"string_view", "StringView"}, {"wstring", "WString"}, {"vector", "Vector"}, {"pair", "Pair"},
    {"optional", "Optional"}, {"map", "Map"}, {"set", "Set"}, {"unordered_map", "HashMap"}, {"unordered_set", "HashSet"},
    {"deque", "Deque"}, {"list", "List"}, {"array", "Array"}, {"span", "Span"}, {"dynamic_extent", "dynamicExtent"}, {"unique_ptr", "UniquePtr"}, {"default_delete", "DefaultDelete"}, {"queue", "Queue"},
    {"replace", "replace"}, {"advance", "advance"}, {"make_optional", "makeOptional"}, {"clock", "::clock"}, {"abort", "::abort"},
    {"shared_ptr", "SharedPtr"}, {"weak_ptr", "WeakPtr"}, {"function", "Function"}, {"tuple", "Tuple"}, {"variant", "Variant"},
    {"initializer_list", "InitializerList"}, {"numeric_limits", "Limits"}, {"endian", "Endian"}, {"runtime_error", "RuntimeError"},
    {"strong_ordering", "strong_ordering"}, {"weak_ordering", "weak_ordering"}, {"partial_ordering", "partial_ordering"},
    {"logic_error", "LogicError"}, {"exception", "Exception"}, {"out_of_range", "OutOfRange"}, {"invalid_argument", "InvalidArgument"},
    {"move", "::move"}, {"forward", "::forward"}, {"swap", "::swap"}, {"min", "::min"}, {"max", "::max"}, {"clamp", "::clamp"},
    {"sort", "::sort"}, {"stable_sort", "::stableSort"}, {"find", "::find"}, {"find_if", "::findIf"}, {"remove_if", "::removeIf"}, {"erase_if", "::eraseIf"},
    {"remove", "::remove"}, {"transform", "::transform"}, {"count_if", "::countIf"}, {"all_of", "::allOf"}, {"any_of", "::anyOf"},
    {"none_of", "::noneOf"}, {"lower_bound", "::lowerBound"}, {"upper_bound", "::upperBound"}, {"reverse", "::reverse"},
    {"fill", "::fill"}, {"copy", "::copy"}, {"accumulate", "::accumulate"}, {"begin", "begin"}, {"end", "end"}, {"size", "size"},
    {"isnan", "::isnan"}, {"isinf", "::isinf"}, {"abs", "::abs"}, {"floor", "::floor"}, {"ceil", "::ceil"}, {"round", "::round"},
    {"sqrt", "::sqrt"}, {"pow", "::pow"}, {"tolower", "::tolower"}, {"toupper", "::toupper"}, {"size_t", "size_t"},
    {"nullptr_t", "nullptr_t"}, {"to_string", "toString"}, {"make_unique", "makeUnique"}, {"make_shared", "makeShared"},
    {"make_pair", "makePair"}, {"make_tuple", "makeTuple"}, {"get", "get"}, {"declval", "declval"}, {"memcpy", "::memcpy"},
    {"memset", "::memset"}, {"strlen", "::strlen"}, {"is_same_v", "isSame"}, {"is_constructible_v", "isConstructible"}, {"is_array_v", "isArray"}, {"is_signed_v", "isSigned"},
    {"is_unsigned_v", "isUnsigned"}, {"is_pointer_v", "isPointer"}, {"is_enum_v", "isEnum"}, {"is_integral_v", "isIntegral"},
    {"is_floating_point_v", "isFloatingPoint"}, {"is_base_of_v", "isBaseOf"}, {"is_trivially_copyable_v", "isTriviallyCopyable"},
    {"is_arithmetic_v", "isArithmetic"}, {"is_fundamental_v", "isFundamental"},
    {"is_trivially_destructible_v", "isTriviallyDestructible"}, {"is_trivially_copy_assignable_v", "isTriviallyCopyAssignable"},
    {"is_trivially_copy_constructible_v", "isTriviallyCopyConstructible"}, {"is_trivially_default_constructible_v", "isTriviallyDefaultConstructible"},
    {"remove_reference_t", "RemoveReference"}, {"remove_const_t", "RemoveConst"}, {"remove_cv_t", "RemoveCv"},
    {"remove_extent_t", "RemoveExtent"}, {"remove_pointer_t", "RemovePointer"}, {"decay_t", "Decay"}, {"underlying_type_t", "UnderlyingType"},
    {"conditional_t", "Conditional"}, {"enable_if_t", "EnableIf"}, {"chrono", "Chrono"}, {"atomic", "Atomic"}, {"mutex", "Mutex"},
    {"lock_guard", "LockGuard"}, {"unique_lock", "UniqueLock"}, {"thread", "Thread"}, {"condition_variable", "ConditionVariable"},
    {"exchange", "exchange"}, {"hash", "Hash"}, {"less", "Less"}, {"equal_to", "EqualTo"}, {"bind", "bind"}, {"ref", "ref"},
    {"cref", "cref"}, {"reference_wrapper", "ReferenceWrapper"}, {"is_const_v", "isConst"}, {"is_reference_v", "isReference"},
    {"numeric_limits", "Limits"}, {"strtoll", "::strtoll"}, {"strtoull", "::strtoull"}, {"strtod", "::strtod"}, {"strtof", "::strtof"},
    {"ostream", "OutStream"}, {"istream", "InStream"}, {"stringstream", "StringStream"}, {"ostringstream", "StringStream"},
    {"nullopt", "nullopt"}, {"count", "count"}, {"distance", "distance"}, {"max_element", "maxElement"}, {"min_element", "minElement"},
    {"nextafter", "::nextafter"}, {"rbegin", "rbegin"}, {"rend", "rend"}, {"unique", "unique"}, {"is_class_v", "isClass"},
    {"is_convertible_v", "isConvertible"}, {"is_null_pointer_v", "isNullPointer"}, {"back_inserter", "backInserter"},
    {"is_void_v", "isVoid"}, {"is_function_v", "isFunction"}, {"invoke", "invoke"}, {"apply", "apply"}, {"tie", "tie"},
    {"ignore", "ignore"}, {"index_sequence", "IndexSequence"}, {"make_index_sequence", "MakeIndexSequence"},
    {"index_sequence_for", "IndexSequenceFor"}, {"byte", "u8"},
    {"int8_t", "int8_t"}, {"int16_t", "int16_t"}, {"int32_t", "int32_t"}, {"int64_t", "int64_t"}, {"uint8_t", "uint8_t"},
    {"uint16_t", "uint16_t"}, {"uint32_t", "uint32_t"}, {"uint64_t", "uint64_t"}, {"intptr_t", "intptr_t"}, {"uintptr_t", "uintptr_t"},
    {"ptrdiff_t", "ptrdiff_t"}, {"fabs", "::fabs"}, {"fmod", "::fmod"}, {"log", "::log"}, {"exp", "::exp"}, {"sin", "::sin"}, {"cos", "::cos"},
    {"atan2", "::atan2"}, {"isfinite", "::isfinite"}, {"signbit", "::signbit"}, {"trunc", "::trunc"}, {"lround", "::lround"},
    {"scoped_lock", "ScopedLock"}, {"true_type", "TrueType"}, {"false_type", "FalseType"}, {"bad_alloc", "BadAlloc"},
    {"time_t", "::time_t"}, {"time", "::time"}, {"tm", "::tm"}, {"localtime", "::localtime"},
    {"uncaught_exceptions", "::uncaught_exceptions"}, {"reverse_iterator", "ReverseIterator"},
    {"uninitialized_copy", "uninitializedCopy"}, {"visit", "visit"},
    // --- batch 2: remaining unmapped names from Factorio ---
    // C library functions → global
    {"_Exit", "::_Exit"}, {"exit", "::exit"}, {"printf", "::printf"}, {"signal", "::signal"},
    {"getenv", "::getenv"}, {"malloc", "::malloc"}, {"free", "::free"}, {"memcmp", "::memcmp"},
    {"strcpy", "::strcpy"}, {"strstr", "::strstr"}, {"qsort", "::qsort"},
    // math → global
    {"acos", "::acos"}, {"atan", "::atan"}, {"hypot", "::hypot"}, {"log2", "::log2"},
    {"log10", "::log10"}, {"modf", "::modf"}, {"isnormal", "::isnormal"},
    {"nextafterf", "::nextafterf"},
    // time → global
    {"gmtime", "::gmtime"}, {"strftime", "::strftime"},
    // string conversion → global
    {"stoi", "stoi"}, {"stod", "stod"}, {"stoul", "stoul"}, {"stoull", "stoull"}, {"stof", "stof"}, {"stoll", "stoll"},
    {"iterator_traits", "IteratorTraits"}, {"make_move_iterator", "makeMoveIterator"},
    {"to_wstring", "toWString"}, {"getline", "getline"},
    // algorithms → global (camelCase)
    {"adjacent_find", "adjacentFind"}, {"copy_backward", "copyBackward"},
    {"copy_n", "copyN"}, {"equal", "equal"}, {"fill_n", "fillN"},
    {"generate_n", "generateN"}, {"is_sorted", "isSorted"},
    {"iter_swap", "iterSwap"}, {"lexicographical_compare", "lexicographicalCompare"},
    {"merge", "merge"}, {"move_backward", "moveBackward"},
    {"next", "next"}, {"prev", "prev"}, {"next_permutation", "nextPermutation"},
    {"for_each", "forEach"}, {"find_if_not", "findIfNot"}, {"copy_if", "copyIf"}, {"replace_if", "replaceIf"},
    {"binary_search", "binarySearch"}, {"equal_range", "equalRange"}, {"is_sorted_until", "isSortedUntil"},
    {"generate", "generate"},
    {"nth_element", "nthElement"}, {"rotate", "rotate"},
    {"set_difference", "setDifference"}, {"set_intersection", "setIntersection"},
    {"destroy", "destroy"}, {"uninitialized_default_construct", "uninitializedDefaultConstruct"},
    {"uninitialized_move", "uninitializedMove"},
    {"uninitialized_value_construct", "uninitializedValueConstruct"},
    {"make_reverse_iterator", "makeReverseIterator"},
    // containers and data structures
    {"bitset", "Bitset"}, {"stack", "Stack"}, {"priority_queue", "PriorityQueue"},
    {"forward_list", "ForwardList"}, {"multimap", "MultiMap"}, {"multiset", "MultiSet"},
    // concurrency
    {"atomic_bool", "Atomic<bool>"}, {"atomic_size_t", "Atomic<size_t>"},
    {"atomic_uint8_t", "Atomic<u8>"}, {"atomic_uint16_t", "Atomic<u16>"}, {"atomic_uint32_t", "Atomic<u32>"},
    {"atomic_uint64_t", "Atomic<u64>"}, {"atomic_int8_t", "Atomic<i8>"}, {"atomic_int16_t", "Atomic<i16>"},
    {"atomic_int32_t", "Atomic<i32>"}, {"atomic_int64_t", "Atomic<i64>"}, {"atomic_load", "atomicLoad"},
    {"atomic_store", "atomicStore"}, {"atomic_exchange", "atomicExchange"}, {"atomic_is_lock_free", "atomicIsLockFree"},
    {"async", "async"}, {"future", "Future"}, {"shared_future", "SharedFuture"},
    {"promise", "Promise"}, {"future_status", "FutureStatus"}, {"launch", "Launch"},
    {"this_thread", "thisThread"}, {"recursive_mutex", "RecursiveMutex"}, {"shared_mutex", "SharedMutex"},
    {"shared_lock", "SharedLock"}, {"cv_status", "CvStatus"}, {"defer_lock", "deferLock"}, {"try_to_lock", "tryToLock"},
    {"adopt_lock", "adoptLock"}, {"defer_lock_t", "DeferLock"}, {"try_to_lock_t", "TryToLock"}, {"adopt_lock_t", "AdoptLock"},
    // type traits
    {"source_location", "CallerLocation"},
    {"type_info", "TypeInfo"}, {"nullopt_t", "NulloptType"},
    {"is_abstract_v", "isAbstract"}, {"is_final_v", "isFinal"},
    {"is_polymorphic_v", "isPolymorphic"}, {"is_class_v", "isClass"},
    {"is_move_assignable_v", "isMoveAssignable"},
    {"is_move_constructible_v", "isMoveConstructible"},
    {"is_nothrow_move_assignable_v", "isNothrowMoveAssignable"},
    {"is_nothrow_move_constructible_v", "isNothrowMoveConstructible"},
    {"is_trivially_move_assignable_v", "isTriviallyMoveAssignable"},
    {"is_trivially_move_constructible_v", "isTriviallyMoveConstructible"},
    {"is_invocable_v", "isInvocable"}, {"is_invocable_r_v", "isInvocableR"},
    {"is_copy_constructible_v", "isCopyConstructible"}, {"is_copy_assignable_v", "isCopyAssignable"},
    {"is_member_pointer_v", "isMemberPointer"}, {"is_member_function_pointer_v", "isMemberFunctionPointer"},
    {"is_member_object_pointer_v", "isMemberObjectPointer"},
    {"alignment_of_v", "alignmentOf"},
    {"add_const_t", "AddConst"}, {"add_lvalue_reference_t", "AddLvalueReference"},
    {"remove_cv", "RemoveCv"}, {"is_const", "IsConst"}, {"is_same", "IsSame"},
    {"make_unsigned_t", "MakeUnsigned"}, {"conditional", "Conditional"},
    {"enable_if", "EnableIf"}, {"void_t", "VoidT"},
    {"bool_constant", "BoolConstant"}, {"integral_constant", "IntegralConstant"},
    {"enable_shared_from_this", "EnableSharedFromThis"}, {"bad_weak_ptr", "BadWeakPtr"},
    {"bad_optional_access", "BadOptionalAccess"}, {"bad_function_call", "BadFunctionCall"},
    // variant helpers
    {"holds_alternative", "holdsAlternative"}, {"get_if", "getIf"},
    {"in_place_index", "inPlaceIndex"}, {"in_place_type_t", "InPlaceType"},
    {"variant_alternative_t", "VariantAlternative"}, {"variant_size_v", "variantSize"},
    // tuple helpers
    {"forward_as_tuple", "forwardAsTuple"}, {"tuple_size_v", "tupleSize"}, {"tuple_size", "TupleSize"}, {"tuple_element", "TupleElement"},
    {"piecewise_construct", "piecewiseConstruct"},
    // iterator tags
    {"input_iterator_tag", "InputIteratorTag"},
    {"forward_iterator_tag", "ForwardIteratorTag"},
    {"bidirectional_iterator_tag", "BidirectionalIteratorTag"},
    {"random_access_iterator_tag", "RandomAccessIteratorTag"},
    // misc
    {"greater", "Greater"}, {"greater_equal", "GreaterEqual"},
    {"as_const", "asConst"}, {"to_address", "toAddress"},
    {"mem_fn", "memFn"}, {"move_iterator", "MoveIterator"},
    {"inserter", "inserter"}, {"back_inserter", "backInserter"},
    {"make_exception_ptr", "makeExceptionPtr"}, {"exception_ptr", "ExceptionPtr"}, {"rethrow_exception", "rethrowException"},
    {"set_terminate", "setTerminate"},
    {"make_unique_for_overwrite", "makeUniqueForOverwrite"},
    {"basic_string_view", "StringView"},
    {"istreambuf_iterator", "IstreambufIterator"},
    {"streamsize", "StreamSize"},
    // I/O
    {"cerr", "cerr"}, {"endl", "endl"}, {"ifstream", "InFileStream"},
    {"ofstream", "OutFileStream"},
    // random
    {"default_random_engine", "DefaultRandomEngine"},
    {"random_device", "RandomDevice"},
    {"uniform_int_distribution", "UniformIntDistribution"},
    // concepts
    {"convertible_to", "ConvertibleTo"}, {"equality_comparable", "EqualityComparable"},
    // misc C++
    {"sig_atomic_t", "::sig_atomic_t"}, {"isupper", "::isupper"},
    {"ratio", "Ratio"}, {"nano", "Nano"}, {"micro", "Micro"}, {"milli", "Milli"}, {"centi", "Centi"}, {"deci", "Deci"}, {"kilo", "Kilo"}, {"mega", "Mega"},
    {"string_literals", "stringLiterals"},
    {"numbers", "Numbers"},
    {"filesystem", "filesystem"},
    {"error_code", "ErrorCode"}, {"errc", "Errc"},
    {"system_error", "SystemError"}, {"system_category", "systemCategory"}, {"generic_category", "genericCategory"},
    {"error_category", "ErrorCategory"}, {"make_error_code", "make_error_code"},
    {"length_error", "LengthError"}, {"overflow_error", "OverflowError"},
    {"codecvt_utf8", "CodecvtUtf8"}, {"wstring_convert", "WStringConvert"},
    {"milli", "Milli"}, {"rotr", "rotr"}, {"lcm", "lcm"}};

  // The game's classes whose names the library uses (Vector, Map, Optional, Pair, RuntimeError),
  // renamed as convertTokens renames them, in text that never was tokens: a macro's expansion, whose body and
  // arguments are the game's C++ (`MACRO(StartWalking,, Vector, vector)` in InputAction's list).
  // `std::` names in it are mapped later, by the pass over the whole output. `counts`, when given,
  // counts the renames.
  std::string renameGameClassNames(const std::string& s, Counts* counts)
  {
    static const std::map<std::string, std::pair<std::string, std::string>> renames = {
      {"Vector", {"Vector2D", "Vector renamed to Vector2D"}},
      {"Map", {"GameMap", "Map renamed to GameMap"}},
      {"Optional", {"GameOptional", "Optional renamed to GameOptional"}},
      {"Pair", {"GamePair", "Pair renamed to GamePair"}},
      {"RuntimeError", {"GameRuntimeError", "RuntimeError renamed to GameRuntimeError"}}};
    std::string out;
    for (size_t i = 0; i < s.size();)
    {
      char c = s[i];
      if (c == '"' || (c == '\'' && !isDigitSeparator(s, i)))
      {
        size_t j = i + 1;
        while (j < s.size() && s[j] != c)
          j += s[j] == '\\' ? 2 : 1;
        j = std::min(j + 1, s.size());
        out.append(s, i, j - i);
        i = j;
        continue;
      }
      if (!(isalpha((unsigned char)c) || c == '_'))
      {
        out += c;
        ++i;
        continue;
      }
      size_t j = i;
      while (j < s.size() && isIdentChar(s[j]))
        ++j;
      std::string word = s.substr(i, j - i);
      i = j;
      auto r = renames.find(word);
      if (r != renames.end())
      {
        // Not after `std::`.
        size_t p = out.size();
        while (p > 0 && out[p - 1] == ' ')
          --p;
        bool qualified = p >= 2 && out[p - 1] == ':' && out[p - 2] == ':';
        if (qualified)
        {
          p -= 2;
          while (p > 0 && out[p - 1] == ' ')
            --p;
        }
        bool afterStd = qualified && p >= 3 && out.compare(p - 3, 3, "std") == 0 && (p == 3 || !isIdentChar(out[p - 4]));
        if (!afterStd)
        {
          out += r->second.first;
          if (counts)
            counts->add(r->second.second);
          continue;
        }
      }
      out += word;
    }
    return out;
  }

  // The aliases the program declares (`using X = ...;` and typedefs), at any scope: their name,
  // the file, and whether at namespace scope (in C++ a .cpp file's namespace-scope alias is its
  // own; in Kpp every namespace-scope name is the program's).
  struct AliasDeclaration
  {
    std::string name;
    const void* file;
    bool namespaceScope;
  };
  void collectAliasesIn(Stmt* s, const void* file, const SymbolTable& symbols, std::vector<AliasDeclaration>& out);
  void collectAliases(Span<Decl*> decls, const void* file, bool namespaceScope, const SymbolTable& symbols, std::vector<AliasDeclaration>& out)
  {
    for (Decl* d : decls)
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl: collectAliases(static_cast<NamespaceDecl*>(d)->decls, file, true, symbols, out); break;
        case NodeKind::ClassDecl: collectAliases(static_cast<ClassDecl*>(d)->members, file, false, symbols, out); break;
        case NodeKind::UsingAliasDecl: out.push_back({std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name)), file, namespaceScope}); break;
        case NodeKind::VariableDecl:
          if ((d->flags & DF_Typedef) && static_cast<VariableDecl*>(d)->name.parts.size() == 1)
            out.push_back({std::string(symbols.name(static_cast<VariableDecl*>(d)->name.parts[0].symbol)), file, namespaceScope});
          break;
        case NodeKind::FunctionDecl: collectAliasesIn(static_cast<FunctionDecl*>(d)->body, file, symbols, out); break;
        default: break;
      }
  }
  void collectAliasesIn(Stmt* s, const void* file, const SymbolTable& symbols, std::vector<AliasDeclaration>& out)
  {
    if (!s)
      return;
    switch (s->kind)
    {
      case NodeKind::BlockStmt:
        for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
          collectAliasesIn(inner, file, symbols, out);
        break;
      case NodeKind::DeclStmt: collectAliases(static_cast<DeclStmt*>(s)->decls, file, false, symbols, out); break;
      case NodeKind::IfStmt:
        collectAliasesIn(static_cast<IfStmt*>(s)->then, file, symbols, out);
        collectAliasesIn(static_cast<IfStmt*>(s)->else_, file, symbols, out);
        break;
      case NodeKind::ForStmt: collectAliasesIn(static_cast<ForStmt*>(s)->body, file, symbols, out); break;
      case NodeKind::RangeForStmt: collectAliasesIn(static_cast<RangeForStmt*>(s)->body, file, symbols, out); break;
      case NodeKind::WhileStmt: collectAliasesIn(static_cast<WhileStmt*>(s)->body, file, symbols, out); break;
      case NodeKind::DoStmt: collectAliasesIn(static_cast<DoStmt*>(s)->body, file, symbols, out); break;
      case NodeKind::SwitchStmt: collectAliasesIn(static_cast<SwitchStmt*>(s)->body, file, symbols, out); break;
      case NodeKind::TryStmt:
        collectAliasesIn(static_cast<TryStmt*>(s)->body, file, symbols, out);
        for (const TryStmt::Handler& h : static_cast<TryStmt*>(s)->handlers)
          collectAliasesIn(h.body, file, symbols, out);
        break;
      default: break;
    }
  }

  // --- hand-written overrides ---

  // What an override file defines, by qualified name: those definitions of the converted file
  // are dropped and the override file's text is appended.
  struct OverrideKeys
  {
    std::set<std::string> functions;           // "ns::name/arity", or "ns::name/*" for every overload
    std::map<std::string, int> functionCounts; // definitions per qualified name
    std::set<std::string> classes;             // "ns::Class" (its out-of-line members go too)
    std::set<std::string> variables;           // "ns::name"
  };

  std::string nameText(const QualifiedName& name, const SymbolTable& symbols)
  {
    std::string s;
    for (size_t i = 0; i < name.parts.size(); ++i)
    {
      const NamePart& p = name.parts[i];
      if (i)
        s += "::";
      if (p.special == 1)
        s += "~";
      if (p.special == 2)
        s += "operator#" + std::to_string(p.opToken);
      else if (p.special == 3)
        s += "operator-conversion";
      else if (p.symbol != UINT32_MAX)
        s += std::string(symbols.name(p.symbol));
    }
    return s;
  }

  // ScopeNames from a scope's declarations; prefix is the scope's qualified name and `::` ("" at
  // global scope). The global namespace's names are under "(global)" ("::" is an unnamed
  // namespace's): its classes, aliases and variables, which a name a using-directive finds is
  // ambiguous with, not its functions (one overload set with the directive's).
  void collectScopeNames(Span<Decl*> decls, const std::string& prefix, const SymbolTable& symbols, ScopeNames& out)
  {
    auto declare = [&](std::string_view name)
    {
      if (!name.empty())
        out[prefix.empty() ? "(global)" : prefix].insert(std::string(name));
    };
    for (Decl* d : decls)
    {
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
        {
          auto* n = static_cast<NamespaceDecl*>(d);
          std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
          collectScopeNames(n->decls, prefix + name + "::", symbols, out);
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (c->name.empty() || c->name.last().symbol == UINT32_MAX)
            break;
          std::string_view own = symbols.name(c->name.last().symbol);
          declare(own);
          if (c->isDefinition)
          {
            std::string inner = prefix + nameText(c->name, symbols) + "::";
            out[inner].insert(std::string(own)); // the injected class name
            collectScopeNames(c->members, inner, symbols, out);
          }
          break;
        }
        case NodeKind::EnumDecl:
        {
          auto* e = static_cast<EnumDecl*>(d);
          if (!e->name.empty() && e->name.last().symbol != UINT32_MAX)
            declare(symbols.name(e->name.last().symbol));
          if (!e->isClass)
            for (Decl* v : e->enumerators)
              declare(symbols.name(static_cast<EnumeratorDecl*>(v)->name));
          break;
        }
        case NodeKind::UsingAliasDecl:
          declare(symbols.name(static_cast<UsingAliasDecl*>(d)->name));
          break;
        case NodeKind::VariableDecl:
        {
          auto* v = static_cast<VariableDecl*>(d);
          if (v->name.parts.size() == 1 && v->name.last().symbol != UINT32_MAX)
            declare(symbols.name(v->name.last().symbol));
          break;
        }
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          if (!prefix.empty() && f->name.parts.size() == 1 && f->name.last().special == 0 && f->name.last().symbol != UINT32_MAX)
            declare(symbols.name(f->name.last().symbol));
          break;
        }
        default: break;
      }
    }
  }

  // The bases of the classes, as written (`ControllerProperties`, `agui::Widget`), by the class's
  // ScopeNames key, with the scope the class is declared in (where a base's name is looked up).
  struct ClassBases
  {
    std::string enclosing;
    std::vector<std::string> bases;
  };
  void collectClassBases(Span<Decl*> decls, const std::string& prefix, const SymbolTable& symbols, std::map<std::string, ClassBases>& out)
  {
    for (Decl* d : decls)
    {
      if (d->kind == NodeKind::NamespaceDecl)
      {
        auto* n = static_cast<NamespaceDecl*>(d);
        std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
        collectClassBases(n->decls, prefix + name + "::", symbols, out);
      }
      else if (d->kind == NodeKind::ClassDecl)
      {
        auto* c = static_cast<ClassDecl*>(d);
        if (c->name.empty() || c->name.last().symbol == UINT32_MAX || !c->isDefinition)
          continue;
        std::string key = prefix + nameText(c->name, symbols) + "::";
        ClassBases& entry = out[key];
        entry.enclosing = prefix;
        for (const ClassDecl::Base& b : c->bases)
          if (b.type && b.type->kind == NodeKind::NamedType && !static_cast<NamedType*>(b.type)->name.empty())
            entry.bases.push_back(nameText(static_cast<NamedType*>(b.type)->name, symbols));
        collectClassBases(c->members, key, symbols, out);
      }
      else if (d->kind == NodeKind::UsingAliasDecl)
      {
        // An alias of a class (`template<class T> using IntrusiveListHook = typename
        // IntrusiveListImplementation<T, ...>::Hook;`) is an entry whose one base is that class, so
        // a class deriving from the alias sees the class's names (UpdatableEntity's `next`).
        auto* a = static_cast<UsingAliasDecl*>(d);
        if (a->type && a->type->kind == NodeKind::NamedType && !static_cast<NamedType*>(a->type)->name.empty())
        {
          ClassBases& entry = out[prefix + std::string(symbols.name(a->name)) + "::"];
          entry.enclosing = prefix;
          entry.bases.push_back(nameText(static_cast<NamedType*>(a->type)->name, symbols));
        }
      }
    }
  }
  // Adds to every class's names the names its bases declare (transitively), which an unqualified
  // name in the class finds before a global one: a class deriving from ControllerProperties sees
  // its `TimePoint`.
  void addInheritedNames(ScopeNames& names, const std::map<std::string, ClassBases>& bases)
  {
    // The base's key: looked up from the class's enclosing scope outwards, as C++ finds it.
    auto keyOf = [&](const std::string& base, std::string scope) -> std::string
    {
      std::string written = base.rfind("::", 0) == 0 ? base.substr(2) : base;
      while (true)
      {
        std::string key = scope + written + "::";
        if (names.count(key) || bases.count(key))
          return key;
        if (scope.empty())
          return "";
        // "A::B::" -> "A::", "A::" (or an anonymous namespace's "::") -> "".
        size_t cut = scope.size() > 2 ? scope.rfind("::", scope.size() - 3) : std::string::npos;
        scope = cut == std::string::npos ? std::string() : scope.substr(0, cut + 2);
      }
    };
    std::map<std::string, std::set<std::string>> inherited;
    std::set<std::string> visited;
    std::function<void(const std::string&, std::set<std::string>&)> gather = [&](const std::string& key, std::set<std::string>& into)
    {
      auto it = bases.find(key);
      if (it == bases.end() || !visited.insert(key).second)
        return;
      for (const std::string& base : it->second.bases)
      {
        std::string baseKey = keyOf(base, it->second.enclosing);
        if (baseKey.empty())
          continue;
        auto own = names.find(baseKey);
        if (own != names.end())
          into.insert(own->second.begin(), own->second.end());
        gather(baseKey, into);
      }
    };
    for (const auto& [key, entry] : bases)
    {
      visited.clear();
      gather(key, inherited[key]);
    }
    for (auto& [key, extra] : inherited)
      names[key].insert(extra.begin(), extra.end());
  }

  // --- std::variant: typed unions (`union enum`, design.md 8.5) ---

  // A std::variant the program declares. An alias (`using AIState = std::variant<A, B>;`) names
  // its union; a member declared as a std::variant gets a union named after it (`data` ->
  // DataVariant), declared just before it.
  struct VariantAlternative
  {
    std::string spelling; // the C++ type as written, without spaces and a leading `::`
    std::string name;     // the alternative's name in the union
  };
  struct VariantInfo
  {
    Decl* decl = nullptr;  // the UsingAliasDecl, or the member's VariableDecl
    std::string unionName; // AIState, DataVariant
    std::string qualified; // ::SegmentedUnitAI::AIState
    std::vector<VariantAlternative> alternatives;
  };
  struct Variants
  {
    std::vector<VariantInfo> all;
    std::map<Decl*, size_t> byDecl;
  };

  // Whether a written type is `std::variant<...>`.
  bool isStdVariant(Type* t, const SymbolTable& symbols)
  {
    if (!t || t->kind != NodeKind::NamedType)
      return false;
    const QualifiedName& n = static_cast<NamedType*>(t)->name;
    return n.parts.size() == 2 && n.parts[0].symbol != UINT32_MAX && n.parts[1].symbol != UINT32_MAX &&
           symbols.name(n.parts[0].symbol) == "std" && symbols.name(n.parts[1].symbol) == "variant" && n.parts[1].hasTemplateArgs;
  }

  // The token ranges [first, last) of the arguments of the first `std::variant<...>` in tokens
  // [from, to), and in variantClose the index of its closing `>`.
  std::vector<std::pair<uint32_t, uint32_t>> variantArgumentTokens(const std::vector<Token>& tk, std::string_view text, uint32_t from, uint32_t to,
                                                                   uint32_t& variantStart, uint32_t& variantClose)
  {
    std::vector<std::pair<uint32_t, uint32_t>> args;
    for (uint32_t k = from; k + 3 < to; ++k)
    {
      if (text.substr(tk[k].offset, tk[k].length) != "std" || !tk[k + 1].is(P::ColonColon) ||
          text.substr(tk[k + 2].offset, tk[k + 2].length) != "variant" || !tk[k + 3].is(P::Less))
        continue;
      int depth = 0;
      uint32_t start = k + 4;
      for (uint32_t j = k + 4; j < to; ++j)
      {
        if (tk[j].is(P::Less) || tk[j].is(P::LParen))
          ++depth;
        else if (tk[j].is(P::RParen))
          --depth;
        else if (tk[j].is(P::Greater) || tk[j].is(P::Shr))
        {
          if (depth == 0)
          {
            args.push_back({start, j});
            variantStart = k;
            variantClose = j;
            return args;
          }
          depth -= tk[j].is(P::Shr) ? 2 : 1;
        }
        else if (tk[j].is(P::Comma) && depth == 0)
        {
          args.push_back({start, j});
          start = j + 1;
        }
      }
      return {};
    }
    return {};
  }

  // A type as written, for comparing: no whitespace, no leading `::`.
  std::string normalizedSpelling(std::string_view text)
  {
    std::string s;
    for (char c : text)
      if (!isspace((unsigned char)c))
        s += c;
    if (s.rfind("::", 0) == 0)
      s.erase(0, 2);
    return s;
  }

  // The last component of a spelled type's name, outside template arguments
  // (`SegmentedUnitAI::Patrolling` -> `Patrolling`, `ID<A::B>` -> `ID<A::B>`).
  std::string lastComponent(const std::string& spelling)
  {
    int depth = 0;
    size_t cut = 0;
    for (size_t i = 0; i + 1 < spelling.size(); ++i)
    {
      if (spelling[i] == '<')
        ++depth;
      else if (spelling[i] == '>')
        --depth;
      else if (depth == 0 && spelling[i] == ':' && spelling[i + 1] == ':')
        cut = i + 2;
    }
    return spelling.substr(cut);
  }

  // An alternative's name from its type: the identifiers of the type's last component and its
  // template arguments, the first letter upper case (`IDWithQuality<FluidID>` -> IDWithQualityFluidID,
  // `int` -> Int, `std::string` -> String).
  std::string alternativeName(const std::string& spelling)
  {
    std::string last = lastComponent(spelling), name;
    for (size_t i = 0; i < last.size();)
    {
      if (isalpha((unsigned char)last[i]) || last[i] == '_')
      {
        size_t j = i;
        while (j < last.size() && isIdentChar(last[j]))
          ++j;
        std::string word = last.substr(i, j - i);
        auto mapped = stdNames.find(word);
        if (word != "std" && word != "const")
          name += (i >= 5 && last.compare(i - 5, 5, "std::") == 0 && mapped != stdNames.end()) ? mapped->second : word;
        i = j;
      }
      else
        ++i;
    }
    if (!name.empty())
      name[0] = (char)toupper((unsigned char)name[0]);
    return name;
  }

  // Records the program's std::variant aliases and members (prefix: the enclosing scopes and `::`).
  void collectVariants(Span<Decl*> decls, const std::string& prefix, const std::vector<Token>& tk, std::string_view text,
                       const SymbolTable& symbols, Variants& out)
  {
    for (Decl* d : decls)
    {
      Type* type = nullptr;
      std::string unionName;
      if (d->kind == NodeKind::NamespaceDecl)
      {
        auto* n = static_cast<NamespaceDecl*>(d);
        std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
        collectVariants(n->decls, prefix + name + "::", tk, text, symbols, out);
        continue;
      }
      if (d->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(d)->isDefinition && !static_cast<ClassDecl*>(d)->name.empty())
      {
        collectVariants(static_cast<ClassDecl*>(d)->members, prefix + nameText(static_cast<ClassDecl*>(d)->name, symbols) + "::", tk, text, symbols, out);
        continue;
      }
      if (d->kind == NodeKind::UsingAliasDecl && isStdVariant(static_cast<UsingAliasDecl*>(d)->type, symbols))
      {
        type = static_cast<UsingAliasDecl*>(d)->type;
        unionName = std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name));
      }
      else if (d->kind == NodeKind::VariableDecl && isStdVariant(static_cast<VariableDecl*>(d)->type, symbols) &&
               static_cast<VariableDecl*>(d)->name.parts.size() == 1 && d->parent && d->parent->kind == NodeKind::ClassDecl)
      {
        type = static_cast<VariableDecl*>(d)->type;
        unionName = std::string(symbols.name(static_cast<VariableDecl*>(d)->name.last().symbol));
        unionName[0] = (char)toupper((unsigned char)unionName[0]);
        unionName += "Variant";
      }
      if (!type || d->endToken <= d->token)
        continue;
      uint32_t variantStart = 0, variantClose = 0;
      auto args = variantArgumentTokens(tk, text, type->token, d->endToken, variantStart, variantClose);
      if (args.empty())
        continue;
      VariantInfo info;
      info.decl = d;
      info.unionName = unionName;
      info.qualified = "::" + prefix + unionName;
      std::set<std::string> used;
      for (auto [a0, a1] : args)
      {
        VariantAlternative alternative;
        alternative.spelling = normalizedSpelling(text.substr(tk[a0].offset, tk[a1 - 1].offset + tk[a1 - 1].length - tk[a0].offset));
        alternative.name = alternativeName(alternative.spelling);
        if (alternative.name.empty())
          alternative.name = "Alternative";
        std::string base = alternative.name;
        for (int n = 2; !used.insert(alternative.name).second; ++n)
          alternative.name = base + std::to_string(n);
        info.alternatives.push_back(alternative);
      }
      out.byDecl[d] = out.all.size();
      out.all.push_back(std::move(info));
    }
  }

  bool isFunctionDefinition(FunctionDecl* f) { return f->body != nullptr || (f->flags & (DF_Default | DF_Delete)); }

  // An override file's `class [[members]] Name { ... }`: members that join the converted class Name,
  // whose other members stay as converted (the game's SerialiserHelper with the typed unions' save).
  struct MemberOverride
  {
    std::string members; // the text between the braces
    uint32_t begin = 0, end = 0; // the class's text in the override file, from its `template` or class keyword
    bool used = false;   // a converted class took them
    std::set<std::string> replaces; // the names of members marked `[[replaces]]`: the converted ones of that name go
  };

  // The `class [[members]]` classes of an override file (lexed into tk), by qualified name.
  // The name a `[[replaces]]` member of an override class replaces: a function's (every overload)
  // or a member alias's (`using StringType = ...;`); empty for any other member.
  std::string replaceableName(Decl* member, const SymbolTable& symbols)
  {
    if (member->kind == NodeKind::FunctionDecl)
      return nameText(static_cast<FunctionDecl*>(member)->name, symbols);
    if (member->kind == NodeKind::UsingAliasDecl)
      return std::string(symbols.name(static_cast<UsingAliasDecl*>(member)->name));
    return std::string();
  }

  void collectMemberOverrides(Span<Decl*> decls, const std::string& prefix, const std::vector<Token>& tk, std::string_view text,
                              const SymbolTable& symbols, std::map<std::string, MemberOverride>& out)
  {
    for (Decl* d : decls)
    {
      if (d->kind == NodeKind::NamespaceDecl)
      {
        auto* n = static_cast<NamespaceDecl*>(d);
        std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
        collectMemberOverrides(n->decls, prefix + name + "::", tk, text, symbols, out);
        continue;
      }
      auto* c = d->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(d) : nullptr;
      if (!c || !c->isDefinition || c->name.empty() || c->endToken <= c->token || c->endToken > tk.size())
        continue;
      // The head: from the `template` keyword (or the class keyword) to the body's `{`. (For a
      // template, the keyword before its parameters: a parameter's own `class` is no head.)
      uint32_t head = c->token;
      if (!c->templateParams.empty())
      {
        head = c->templateParams[0]->token;
        while (head > 0 && !tk[head].is(Kw::template_))
          --head;
      }
      else
        while (head > 0 && !tk[head].is(Kw::class_) && !tk[head].is(Kw::struct_) && !tk[head].is(P::Semi) && !tk[head].is(P::RBrace))
          --head;
      uint32_t open = head;
      while (open < c->endToken && !tk[open].is(P::LBrace))
        ++open;
      bool marked = false;
      for (uint32_t k = head; k + 4 < open && !marked; ++k)
        marked = tk[k].is(P::LBracket) && tk[k + 1].is(P::LBracket) && text.substr(tk[k + 2].offset, tk[k + 2].length) == "members" &&
                 tk[k + 3].is(P::RBracket) && tk[k + 4].is(P::RBracket);
      uint32_t close = c->endToken;
      while (close > open && !tk[close - 1].is(P::RBrace))
        --close;
      if (!marked || open >= c->endToken || close <= open + 1)
        continue;
      MemberOverride m;
      m.members = std::string(text.substr(tk[open].offset + 1, tk[close - 1].offset - tk[open].offset - 1));
      m.begin = tk[head].offset;
      uint32_t last = close - 1;
      if (last + 1 < tk.size() && tk[last + 1].is(P::Semi))
        ++last;
      m.end = tk[last].offset + tk[last].length;
      // A member marked `[[replaces]]` (before its template header, type or `using`) takes the
      // place of the converted members of its name, every overload; the others are added.
      for (Decl* member : c->members)
      {
        if (replaceableName(member, symbols).empty() || member->token <= open || member->token >= close)
          continue;
        uint32_t k = member->token;
        while (k > open + 1 && !tk[k - 1].is(P::Semi) && !tk[k - 1].is(P::RBrace) && !tk[k - 1].is(P::LBrace) && !tk[k - 1].is(P::Colon))
          --k;
        for (; k + 4 < member->token; ++k)
          if (tk[k].is(P::LBracket) && tk[k + 1].is(P::LBracket) && text.substr(tk[k + 2].offset, tk[k + 2].length) == "replaces" &&
              tk[k + 3].is(P::RBracket) && tk[k + 4].is(P::RBracket))
          {
            m.replaces.insert(replaceableName(member, symbols));
            break;
          }
      }
      out[prefix + nameText(c->name, symbols)] = std::move(m);
    }
  }

  void collectOverrideKeys(Span<Decl*> decls, const std::string& prefix, const SymbolTable& symbols, OverrideKeys& out,
                           const std::map<std::string, MemberOverride>* memberClasses = nullptr)
  {
    for (Decl* d : decls)
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
        {
          auto* n = static_cast<NamespaceDecl*>(d);
          std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
          collectOverrideKeys(n->decls, prefix + name + "::", symbols, out, memberClasses);
          break;
        }
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          if (isFunctionDefinition(f))
          {
            out.functions.insert(prefix + nameText(f->name, symbols) + "/" + std::to_string(f->params.size()));
            ++out.functionCounts[prefix + nameText(f->name, symbols)];
          }
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (c->isDefinition && !c->name.empty() && !(memberClasses && memberClasses->count(prefix + nameText(c->name, symbols))))
            out.classes.insert(prefix + nameText(c->name, symbols));
          break;
        }
        case NodeKind::VariableDecl: out.variables.insert(prefix + nameText(static_cast<VariableDecl*>(d)->name, symbols)); break;
        default: break;
      }
  }

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
  // without calling conventions, which Kpp has none of (SSE.hpp's `#define VCALL __vectorcall` is
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

  struct OverrideFile
  {
    std::string text;
    OverrideKeys keys;
    std::map<std::string, MemberOverride> memberClasses;
    std::string appended; // text without the member classes, which go into the converted classes
  };

  struct FileConverter
  {
    const ConvertInput& in;
    GlobalScope& global;
    const SymbolTable& symbols;
    bool isHeader() const
    {
      std::string ext = in.path.extension().string();
      return ext != ".cpp" && ext != ".cc" && ext != ".c";
    }
    const OverrideKeys* overrides = nullptr; // definitions a hand-written file replaces
    std::map<std::string, MemberOverride>* memberOverrides = nullptr; // members a hand-written file adds to classes
    const CarriedDefaults* carriedDefaults = nullptr;
    const CarriedTemplateDefaults* carriedTemplateDefaults = nullptr;
    const ScopeNames* scopeNames = nullptr;
    const std::set<std::string>* libraryTypes = nullptr; // the library's global classes and aliases (libraryTypeNames)
    std::set<std::string>* opaqueKept = nullptr; // classes declared and defined nowhere, kept as declarations (once in the program)
    // The files the conversion leaves out: a class defined only in one of them is defined nowhere
    // (GraphicsInterface.hpp's `class ShaderProgram;` stays, as the game's files see it).
    const std::set<const ParsedFile*>* leftOutFiles = nullptr;
    const Variants* variants = nullptr;
    // A std::variant declared in this file, which becomes a union enum (run() writes it once the
    // edits inside its alternatives' types are known).
    struct VariantDeclaration
    {
      const VariantInfo* info;
      uint32_t begin, end;                             // the alias declaration, or the member's type
      bool member;                                     // a member: the union goes before it, its type becomes the union
      uint32_t memberStart;                            // where the member's declaration starts (its line)
      std::vector<std::pair<uint32_t, uint32_t>> args; // the alternatives' types, as offsets
    };
    std::vector<VariantDeclaration> variantDeclarations;
    // std::get<T>(x), std::holds_alternative<T>(x), std::get_if<T>(p), std::visit(f, x) on a variant.
    struct VariantUse
    {
      std::string kind;
      uint32_t begin, end, calleeToken;
      std::vector<std::pair<uint32_t, uint32_t>> args; // the call's arguments, as offsets
      std::string typeSpelling;                        // T, normalized
      uint32_t typeBegin = 0, typeEnd = 0;             // T, as offsets
      bool noAlternative = false;                      // T is no variant's alternative (a template's parameter)
    };
    std::vector<VariantUse> variantUses;
    std::map<uint32_t, const VariantInfo*> callVariant; // a use's callee token -> the variant its argument is declared as
    // This file's namespaces and classes (and the class scope of out-of-line member definitions),
    // by text range: where a name would find something before the global library name.
    struct ScopeRange
    {
      uint32_t begin, end;
      std::string key; // as ScopeNames has it
      bool classBody = false; // a class's definition (not a namespace, not an out-of-line member's scope)
    };
    std::vector<ScopeRange> scopeRanges;
    void collectScopeRanges(Span<Decl*> decls, const std::string& prefix)
    {
      for (Decl* d : decls)
      {
        if (d->endToken <= d->token || d->endToken > toks.size())
          continue;
        uint32_t begin = toks[d->token].offset, end = endOffset(d->endToken - 1);
        if (d->kind == NodeKind::NamespaceDecl)
        {
          auto* n = static_cast<NamespaceDecl*>(d);
          std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
          scopeRanges.push_back({begin, end, prefix + name + "::"});
          collectScopeRanges(n->decls, prefix + name + "::");
        }
        else if (d->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(d)->isDefinition && !static_cast<ClassDecl*>(d)->name.empty())
        {
          std::string key = prefix + nameText(static_cast<ClassDecl*>(d)->name, symbols) + "::";
          scopeRanges.push_back({begin, end, key, true});
          collectScopeRanges(static_cast<ClassDecl*>(d)->members, key);
        }
        else if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->name.parts.size() > 1)
        {
          // `void NoiseExpressions::String::f(...) {...}`: the parameters and body see the class.
          QualifiedName qualifier = static_cast<FunctionDecl*>(d)->name;
          qualifier.parts.count -= 1;
          std::string scope = prefix;
          for (size_t i = 0; i < qualifier.parts.size(); ++i)
          {
            if (qualifier.parts[i].symbol == UINT32_MAX)
              continue; // the leading `::`
            scope += std::string(symbols.name(qualifier.parts[i].symbol)) + "::";
            scopeRanges.push_back({begin, end, scope});
          }
        }
      }
    }
    // The file's using-directives of the program's namespaces, by text range: from each to the end
    // of the block or namespace it is written in, the names of the namespace it names are found as
    // if declared in the global namespace, beside the library's. So a library name the converter
    // writes there is ambiguous unless qualified (TestMigrations' `std::set<std::string>` under
    // `using namespace TestMigrationHelpers;`, whose own Set class template it would find: `::Set`),
    // and so is the namespace's own name of the library's (that Set, written `Set`:
    // `TestMigrationHelpers::Set`). A directive outside any block of a source file (not a header)
    // goes, its namespace's names written qualified instead: C++ limits it to its translation
    // unit, Kpp has none, and the global namespace it is in is every file's (DeferredTestReporter's
    // `using namespace UnitTest;` put UnitTest's TestReporter beside the tests' own everywhere).
    struct DirectiveRange
    {
      uint32_t begin, end;  // the directive, to the end of its block (or of the file)
      uint32_t lineEnd;     // the end of the directive's line (a removed one goes with it)
      std::string key;      // the namespace's ScopeNames key
      bool removed = false; // a source file's directive outside any block
    };
    std::vector<DirectiveRange> directiveRanges;
    void collectDirectiveRanges()
    {
      if (!scopeNames)
        return;
      std::vector<std::vector<size_t>> pending(1); // per open block: the directives written directly in it
      for (uint32_t i = 0; i < toks.size(); ++i)
      {
        if (toks[i].is(P::LBrace))
          pending.emplace_back();
        else if (toks[i].is(P::RBrace) && pending.size() > 1)
        {
          for (size_t d : pending.back())
            directiveRanges[d].end = toks[i].offset;
          pending.pop_back();
        }
        else if (toks[i].is(Kw::using_) && i + 2 < toks.size() && toks[i + 1].is(Kw::namespace_))
        {
          std::string name;
          uint32_t j = i + 2;
          for (; j < toks.size() && !toks[j].is(P::Semi); ++j)
            name += spelling(j);
          if (name.empty() || name == "std" || name.compare(0, 5, "std::") == 0 || j == toks.size())
            continue;
          if (name.compare(0, 2, "::") == 0)
            name = name.substr(2);
          // A name written in a namespace may be one of that namespace's own (or an enclosing
          // one's): the innermost that the program declares names in.
          std::vector<std::string> keys;
          for (const ScopeRange& sr : scopeRanges)
            if (!sr.classBody && sr.begin <= toks[i].offset && toks[i].offset < sr.end)
              keys.push_back(sr.key + name + "::");
          std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
          keys.push_back(name + "::");
          for (const std::string& key : keys)
            if (scopeNames->count(key))
            {
              pending.back().push_back(directiveRanges.size());
              directiveRanges.push_back({toks[i].offset, UINT32_MAX, lineEndAfter(endOffset(j)), key, pending.size() == 1 && !isHeader()});
              break;
            }
          i = j;
        }
      }
    }
    // Whether the program declares a class, alias or variable of the name in the global namespace.
    bool globalNamed(const std::string& name) const
    {
      auto it = scopeNames->find("(global)");
      return it != scopeNames->end() && it->second.count(name);
    }
    // The namespace (its ScopeNames key) through whose using-directive an unqualified `name`
    // written at offset finds a declaration, and the directive, or none.
    const DirectiveRange* directiveFinding(const std::string& name, uint32_t offset) const
    {
      for (const DirectiveRange& r : directiveRanges)
        if (r.begin <= offset && offset < r.end)
          if (auto it = scopeNames->find(r.key); it != scopeNames->end() && it->second.count(name))
            return &r;
      return nullptr;
    }
    // Whether an unqualified `name` written at offset finds a declaration of the program before a
    // global one: a local or parameter of the function around it declared before, or a member of
    // a namespace or class around it (an out-of-line member's class, `void
    // DeferredTestReporter::f()` with that class UnitTest's through a directive, among them).
    bool declaredCloser(const std::string& name, uint32_t offset) const
    {
      if (auto l = localNames.find(name); l != localNames.end())
        for (auto [declared, end] : l->second)
          if (declared <= offset && offset < end)
            return true;
      if (!scopeNames)
        return false;
      for (const ScopeRange& r : scopeRanges)
        if (r.begin <= offset && offset < r.end)
        {
          auto it = scopeNames->find(r.key);
          if (it != scopeNames->end() && it->second.count(name))
            return true;
          // (Not the class's own name, which its qualified declarator name is written with: the
          // class it names is the directive's namespace's either way.)
          if (it == scopeNames->end() && r.key.size() > name.size() + 2 && r.key.compare(r.key.size() - name.size() - 2, name.size(), name) != 0)
            for (const DirectiveRange& d : directiveRanges)
              if (d.begin <= offset && offset < d.end)
                if (auto via = scopeNames->find(d.key + r.key); via != scopeNames->end() && via->second.count(name))
                  return true;
        }
      return false;
    }
    // Whether an unqualified `name` written at offset finds a declaration of the program before the
    // global (library) one: a namespace's or class's member, a member of a namespace a
    // using-directive names (as ambiguous: the library's needs `::` as well), or a local or
    // parameter of the function around it declared before (`Layer::const_iterator next = std::next(current);`, whose
    // initializer sees the variable being declared).
    bool shadowedAt(const std::string& name, uint32_t offset) const
    {
      return declaredCloser(name, offset) || (scopeNames && directiveFinding(name, offset));
    }
    // The locals and parameters of the file's functions by name: where each declaration starts and
    // where its function ends (the block it is in ends there or sooner: a `::` too many is harmless).
    std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> localNames;
    // The function definitions of the file: where each starts and ends, and its qualified name.
    struct FunctionRange
    {
      uint32_t begin, end;
      std::string name;
    };
    std::vector<FunctionRange> functionRanges;
    void collectLocals(Span<Decl*> decls, const std::string& prefix = "")
    {
      for (Decl* d : decls)
        if (d->kind == NodeKind::NamespaceDecl)
        {
          auto* n = static_cast<NamespaceDecl*>(d);
          std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
          collectLocals(n->decls, name.empty() ? prefix : prefix + name + "::");
        }
        else if (d->kind == NodeKind::ClassDecl)
        {
          auto* c = static_cast<ClassDecl*>(d);
          collectLocals(c->members, c->name.empty() ? prefix : prefix + nameText(c->name, symbols) + "::");
        }
        else if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body && d->endToken > 0 && d->endToken <= toks.size())
        {
          auto* f = static_cast<FunctionDecl*>(d);
          uint32_t end = endOffset(d->endToken - 1);
          functionRanges.push_back({toks[d->token].offset, end, prefix + nameText(f->name, symbols)});
          for (Decl* p : f->params)
            if (auto* pd = static_cast<ParamDecl*>(p); pd->name != UINT32_MAX)
              localNames[std::string(symbols.name(pd->name))].push_back({toks[p->token].offset, end});
          collectLocalsIn(f->body, end);
        }
    }
    // A declaration among a statement's parts (`if (auto* x = ...)`, a range-for's variable).
    void collectLocal(Node* n, uint32_t end)
    {
      if (n && n->kind == NodeKind::VariableDecl)
      {
        auto* v = static_cast<VariableDecl*>(n);
        if (v->name.parts.size() == 1 && v->name.parts[0].symbol != UINT32_MAX)
          localNames[std::string(symbols.name(v->name.parts[0].symbol))].push_back({toks[v->token].offset, end});
        // A local named `_`, which the parser makes unnamed (`_<offset>`), as Kpp does.
        for (uint32_t k = v->token; k < v->endToken && k < toks.size() && !toks[k].is(P::Semi); ++k)
          if (toks[k].getKind() == TokenKind::Identifier && spelling(k) == "_")
          {
            underscoreLocals.push_back({toks[v->token].offset, end, k});
            break;
          }
      }
    }
    // The locals named `_` (where each declaration starts, where its function ends, its name's
    // token): in Kpp `_` is an unnamed variable, so one the code reads after all gets a name.
    struct UnderscoreLocal
    {
      uint32_t declared, end, nameToken;
    };
    std::vector<UnderscoreLocal> underscoreLocals;
    std::set<uint32_t> namedUnderscores;
    // `Filesystem::ErrorCode _; Filesystem::RemoveAll(path, _);` (WriteFileGuard): the `_` the call
    // reads is the latest `_` declared before it in the function; both become `ignored`.
    bool nameUnderscoreUse(uint32_t token)
    {
      uint32_t at = toks[token].offset;
      const UnderscoreLocal* found = nullptr;
      for (const UnderscoreLocal& u : underscoreLocals)
        if (u.declared < at && at < u.end && toks[u.nameToken].offset < at && (!found || u.declared > found->declared))
          found = &u;
      if (!found)
        return false;
      replace(at, toks[token].length, "ignored");
      if (namedUnderscores.insert(found->nameToken).second)
        replace(toks[found->nameToken].offset, toks[found->nameToken].length, "ignored");
      counts.add("read `_` locals named");
      return true;
    }
    void collectLocalsIn(Stmt* s, uint32_t end)
    {
      if (!s)
        return;
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            collectLocalsIn(inner, end);
          break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
            collectLocal(d, end);
          break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          collectLocalsIn(i->init, end);
          collectLocal(i->cond, end);
          collectLocalsIn(i->then, end);
          collectLocalsIn(i->else_, end);
          break;
        }
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          collectLocalsIn(f->init, end);
          collectLocal(f->cond, end);
          collectLocalsIn(f->body, end);
          break;
        }
        case NodeKind::RangeForStmt:
        {
          auto* f = static_cast<RangeForStmt*>(s);
          collectLocal(f->var, end);
          collectLocalsIn(f->body, end);
          break;
        }
        case NodeKind::WhileStmt:
          collectLocal(static_cast<WhileStmt*>(s)->cond, end);
          collectLocalsIn(static_cast<WhileStmt*>(s)->body, end);
          break;
        case NodeKind::DoStmt: collectLocalsIn(static_cast<DoStmt*>(s)->body, end); break;
        case NodeKind::SwitchStmt:
        {
          auto* sw = static_cast<SwitchStmt*>(s);
          collectLocalsIn(sw->init, end);
          collectLocal(sw->cond, end);
          collectLocalsIn(sw->body, end);
          break;
        }
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(s);
          collectLocalsIn(t->body, end);
          for (const TryStmt::Handler& h : t->handlers)
            collectLocalsIn(h.body, end);
          break;
        }
        default: break;
      }
    }
    struct LocalMacro
    {
      std::vector<std::string> params;
      std::string body;
      uint32_t from = 0, to = UINT32_MAX; // the text where this definition holds: from its #define to an #undef or the next #define
    };
    // Function-like macros defined in this file, expanded at their uses; a name can be defined,
    // undefined and defined again (`#define INCREMENT(NAME) 1+` then with four parameters).
    std::map<std::string, std::vector<LocalMacro>> localMacros;
    // Function-like macros of the C headers the program imports (Lua's lua_pop and the rest, from the
    // headers expand-macros.txt in the overrides lists), expanded like this file's own: the imported
    // headers give Kpp their functions and constants, not their macros.
    const std::map<std::string, std::pair<std::vector<std::string>, std::string>>* headerMacros = nullptr;
    // The imported C headers' object-like macros that are another name (glad's glGenTextures is
    // glad_glGenTextures): a use is written as that name.
    const std::map<std::string, std::string>* headerAliases = nullptr;
    // This .cpp file's namespace-scope aliases that another file declares too, with their own
    // names (`using MyBinder = LuaBinder<LuaRocketSiloControlBehavior>;` is
    // MyBinderOfLuaRocketSiloControlBehavior): file-local in C++, program-wide in Kpp.
    std::map<std::string, std::string> fileAliasRenames;
    // The function-like macros of the game's headers this file includes, directly or through other
    // headers, that they leave defined (the X-macro lists a header defines and its .cpp expands), in
    // include order.
    std::vector<const std::map<std::string, std::pair<std::vector<std::string>, std::string>>*> includedMacros;
    // The includes of this file that name a converted file beside it (siblingIncludes): removed.
    const std::set<std::string>* siblingIncludes = nullptr;
    // The std names this file brought in with a using-declaration (XmlTestReporter.cpp's `using
    // std::ostream;`), by their Kpp names: its later unqualified uses are the library's.
    std::map<std::string, std::string> stdUsingNames;
    // Every function-like macro's name (callsFunction): an object-like macro that calls something else
    // is written out at its uses.
    const std::set<std::string>* functionMacroNames = nullptr;
    // The converted files' classes and aliases by their own name (convertToKpp), for the fixture of a
    // fixture test.
    const std::map<std::string, std::vector<Decl*>>* classesByName = nullptr;
    // The tests the conversion leaves out (overrides/left-out-tests.txt: `Suite/Test` -> why), those
    // found and the suites converted: the block of one goes, a comment in its place says why, and its
    // suite runs its other tests (a stub would stop the suite where the test reaches it). The SUITE
    // blocks seen so far tell a test's suite: the suite's name and its block's `{` and `}` tokens.
    const std::map<std::string, std::string>* leftOutTests = nullptr;
    std::set<std::string>* leftOutTestsFound = nullptr;
    std::set<std::string>* suitesConverted = nullptr;
    struct SuiteBlock
    {
      std::string name;
      uint32_t open = 0, close = 0;
    };
    std::vector<SuiteBlock> suiteBlocks;
    // The `}` token closing the `{` token open (0: none).
    uint32_t closingBrace(uint32_t open) const
    {
      int depth = 0;
      for (uint32_t k = open; k < toks.size(); ++k)
        if (toks[k].is(P::LBrace))
          ++depth;
        else if (toks[k].is(P::RBrace) && --depth == 0)
          return k;
      return 0;
    }
    // Whether name is an iterator type's, whose member types (value_type, ...) the library's
    // IteratorTraits gives: an alias of an iterator (`using ListIterator = ...::const_iterator;`; a
    // Vector's iterator is a pointer, which has no members), or, naming no class or alias of the
    // converted files, a template parameter named as std names iterators (RandomIt, InputIt, Iter).
    bool namesIteratorType(const std::string& name) const
    {
      if (classesByName)
        if (auto found = classesByName->find(name); found != classesByName->end() && !found->second.empty())
        {
          for (Decl* d : found->second)
          {
            Type* aliased = d->kind == NodeKind::UsingAliasDecl ? static_cast<UsingAliasDecl*>(d)->type : nullptr;
            if (!aliased || aliased->kind != NodeKind::NamedType || static_cast<NamedType*>(aliased)->name.empty() ||
                static_cast<NamedType*>(aliased)->name.last().symbol == UINT32_MAX)
              return false;
            std::string last(symbols.name(static_cast<NamedType*>(aliased)->name.last().symbol));
            if (last.size() < 8 || last.compare(last.size() - 8, 8, "iterator") != 0)
              return false;
          }
          return true;
        }
      auto endsWith = [&](std::string_view suffix) { return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0; };
      return endsWith("It") || endsWith("Iter") || endsWith("Iterator");
    }
    // A fixture test's body (UnitTest++'s TEST_FIXTURE, whose expansion ends with the
    // `Fixture##Name##Helper::Run()` of a class deriving from the fixture): the parse does not see it
    // (a macro's use with a block), so the names it uses of the fixture's members get their `this->`
    // here, which Kpp requires (TestArithmeticCombinator's `combinator->setParameters(params);`). From
    // its `{` token to its `}`, the fixture's member names, and the locals declared in it (a name, the
    // token declaring it, the end of its block), which hide members of their names.
    struct FixtureBody
    {
      uint32_t open = 0, close = 0;
      std::set<std::string> members;
      std::vector<std::tuple<std::string, uint32_t, uint32_t>> locals;
    };
    std::vector<FixtureBody> fixtureBodies;
    const FixtureBody* fixtureBodyAt(uint32_t token) const
    {
      for (const FixtureBody& b : fixtureBodies)
        if (b.open < token && token < b.close)
          return &b;
      return nullptr;
    }
    static bool isFixtureLocal(const FixtureBody& b, const std::string& name, uint32_t token)
    {
      for (const auto& [local, declared, end] : b.locals)
        if (local == name && declared <= token && token < end)
          return true;
      return false;
    }
    // The body of the fixture test whose expansion is text and whose body begins at token open:
    // its fixture's members (through an alias, and the bases'), and its locals by their declarations.
    void noteFixtureBody(const std::string& expansion, uint32_t open)
    {
      std::string text = expansion;
      while (!text.empty() && isspace((unsigned char)text.back()))
        text.pop_back();
      if (!classesByName || open >= toks.size() || !toks[open].is(P::LBrace) || text.size() < 8 || text.compare(text.size() - 7, 7, "::Run()") != 0)
        return;
      size_t nameEnd = text.size() - 7, nameStart = nameEnd;
      while (nameStart > 0 && isIdentChar(text[nameStart - 1]))
        --nameStart;
      std::string helper = text.substr(nameStart, nameEnd - nameStart);
      size_t at = text.find("class " + helper + " : public ");
      if (helper.empty() || at == std::string::npos)
        return;
      size_t baseStart = at + helper.size() + 16, baseEnd = baseStart;
      while (baseEnd < text.size() && text[baseEnd] != ',' && text[baseEnd] != '{')
        ++baseEnd;
      std::string base = text.substr(baseStart, baseEnd - baseStart);
      while (!base.empty() && isspace((unsigned char)base.back()))
        base.pop_back();
      size_t last = base.rfind(':');
      std::string fixture = last == std::string::npos ? base : base.substr(last + 1);
      while (!fixture.empty() && isspace((unsigned char)fixture.front()))
        fixture.erase(fixture.begin());
      auto found = classesByName->find(fixture);
      if (found == classesByName->end() || found->second.empty())
        return;
      Decl* cls = found->second.front();
      if (cls->kind == NodeKind::UsingAliasDecl)
      {
        const Ty* aliased = global.typeOfAst(static_cast<UsingAliasDecl*>(cls)->type, cls);
        if (!aliased || !aliased->decl)
          return;
        cls = aliased->decl;
      }
      FixtureBody body;
      body.open = open;
      body.close = closingBrace(open);
      if (body.close == 0)
        return;
      // (A class's constructors are among its members by the class's name, which names a type here:
      // TestAchievements' `AchievementFixture fixture("look-at-my-shiny-rare-armor");`.)
      for (uint32_t symbol : memberNames(cls))
        if (std::string name(symbols.name(symbol)); !classesByName->count(name))
          body.members.insert(name);
      // The locals: a name after a type's last token (a name, `>`, `*`, `&`, `&&`, a type word) and
      // before what a declarator ends with, or the names of a structured binding; each until the end
      // of the block it is declared in.
      static const std::set<std::string_view> typeWords = {"auto", "bool", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "void", "const", "volatile"};
      std::vector<uint32_t> blocks; // the `{` of the blocks open at k
      auto blockEnd = [&](uint32_t from)
      {
        int d = 0;
        for (uint32_t k = from; k <= body.close; ++k)
          if (toks[k].is(P::LBrace))
            ++d;
          else if (toks[k].is(P::RBrace) && --d == 0)
            return k;
        return body.close;
      };
      for (uint32_t k = open; k < body.close; ++k)
      {
        if (toks[k].is(P::LBrace))
          blocks.push_back(k);
        else if (toks[k].is(P::RBrace) && !blocks.empty())
          blocks.pop_back();
        uint32_t end = blocks.empty() ? body.close : blockEnd(blocks.back());
        if (toks[k].is(Kw::auto_) || (toks[k].getKind() == TokenKind::Keyword && spelling(k) == "auto"))
        {
          uint32_t b = k + 1;
          while (b < body.close && (toks[b].is(P::Amp) || toks[b].is(P::AndAnd)))
            ++b;
          if (b < body.close && toks[b].is(P::LBracket))
            for (++b; b < body.close && !toks[b].is(P::RBracket); ++b)
              if (toks[b].getKind() == TokenKind::Identifier)
                body.locals.emplace_back(std::string(spelling(b)), b, end);
        }
        if (toks[k].getKind() != TokenKind::Identifier || k == 0)
          continue;
        const Token& before = toks[k - 1];
        const Token& after = toks[k + 1];
        bool typeBefore = before.getKind() == TokenKind::Identifier || before.is(P::Greater) || before.is(P::Star) || before.is(P::Amp) ||
                          before.is(P::AndAnd) || (before.getKind() == TokenKind::Keyword && typeWords.count(spelling(k - 1)));
        bool declaratorAfter = after.is(P::Assign) || after.is(P::Semi) || after.is(P::LParen) || after.is(P::LBrace) || after.is(P::LBracket) ||
                               after.is(P::Comma) || after.is(P::RParen) || after.is(P::Colon);
        bool qualified = k >= 2 && (toks[k - 2].is(P::Dot) || toks[k - 2].is(P::Arrow) || toks[k - 2].is(P::ColonColon));
        if (typeBefore && declaratorAfter && !(before.getKind() == TokenKind::Identifier && qualified))
          body.locals.emplace_back(std::string(spelling(k)), k, end);
      }
      counts.add("fixture test bodies");
      fixtureBodies.push_back(std::move(body));
    }
    // The C libraries this file's library keeps calling, by include path prefix, imported where it
    // includes them (Lua's override_printf.h includes trio.h; the game's own calls of trio are the
    // C runtime's, and its includes of trio.h go).
    const std::set<std::string>* keptCLibraries = nullptr;
    // The object-like macros of this file and the headers it reaches that are written out at their
    // uses: those whose body is string literals (UTF8_BULLET; constants in the conversion) where
    // C++ concatenates them with a literal (`"\n" UTF8_BULLET " "`, since only literals concatenate),
    // and those whose body is a qualified name everywhere (`#define vkCreateInstance
    // VulkanImpl::CreateInstanceWrapper`: Vulkan through the function pointers the game loads).
    std::map<std::string, std::string> objectMacros;
    // Where this file's own #define of a name holds: from the #define to its #undef (a header's,
    // seen through an include, holds everywhere).
    std::map<std::string, std::pair<uint32_t, uint32_t>> ownMacroRanges;
    // Which lines the configuration keeps (null: the file's own lines decide, without its includes).
    const Preprocessed* preprocessed = nullptr;
    // `#pragma pack(push, 1)` to its `#pragma pack(pop)`: the classes defined there are [[packed]]
    // (Tile, ImmutableStringTemplate, PropertyTree). A pack of 8, the largest alignment there is,
    // changes nothing; another value gets a TODO.
    std::vector<std::pair<uint32_t, uint32_t>> packedRanges;
    std::vector<int> packStack;
    int pack = 0; // 0: the default
    uint32_t packedSince = 0;
    void notePack(const std::string& rest, uint32_t offset)
    {
      size_t p = rest.find("pack");
      if (p == std::string::npos || rest.find('(', p) == std::string::npos)
        return;
      std::string args = rest.substr(rest.find('(', p) + 1);
      args = args.substr(0, args.find(')'));
      std::vector<std::string> parts;
      for (size_t start = 0; start <= args.size();)
      {
        size_t comma = args.find(',', start);
        std::string part = args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t a = part.find_first_not_of(" \t"), b = part.find_last_not_of(" \t");
        if (a != std::string::npos)
          parts.push_back(part.substr(a, b - a + 1));
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
      int before = pack;
      if (!parts.empty() && parts[0] == "push")
      {
        packStack.push_back(pack);
        if (parts.size() > 1)
          pack = atoi(parts[1].c_str());
      }
      else if (!parts.empty() && parts[0] == "pop")
      {
        pack = packStack.empty() ? 0 : packStack.back();
        if (!packStack.empty())
          packStack.pop_back();
      }
      else
        pack = parts.empty() ? 0 : atoi(parts[0].c_str());
      if (pack != 0 && pack != 1 && pack != 8 && pack != 16)
        todo(offset, "#pragma pack(" + args + "): only a pack of 1 ([[packed]]) is written out");
      if (before != 1 && pack == 1)
        packedSince = offset;
      else if (before == 1 && pack != 1)
        packedRanges.push_back({packedSince, offset});
    }
    bool packedAt(uint32_t offset) const
    {
      for (auto [from, to] : packedRanges)
        if (from <= offset && offset < to)
          return true;
      return pack == 1 && packedSince <= offset; // an unclosed push lasts to the end of the file
    }
    void seedHeaderMacros()
    {
      if (headerMacros)
        for (const auto& [name, macro] : *headerMacros)
        {
          LocalMacro m;
          m.params = macro.first;
          m.body = macro.second;
          localMacros[name].push_back(m);
        }
      for (const auto* macros : includedMacros)
        for (const auto& [name, macro] : *macros)
        {
          // The macros the converter rewrites by name keep their rewrite (convertTokens writes
          // Util/Enums.hpp's ENUM as a readable class, ...); the first header to define a name gives
          // its definition.
          static const std::set<std::string> rewrittenByName = {"ENUM", "ENUM_IMPL", "ENUM_IMPL_NOSTRING", "BITFIELD", "MAKE_OPTIONAL"};
          if (knownMacros.count(name) || knownFunctionMacros.count(name) || argumentMacros.count(name) || rewrittenByName.count(name) ||
              localMacros.count(name))
            continue;
          LocalMacro m;
          m.params = macro.first;
          m.body = macro.second;
          localMacros[name].push_back(m);
        }
    }
    const LocalMacro* localMacroAt(const std::string& name, uint32_t offset) const
    {
      auto it = localMacros.find(name);
      if (it != localMacros.end())
        for (const LocalMacro& m : it->second)
          if (m.from <= offset && offset < m.to)
            return &m;
      return nullptr;
    }
    void endLocalMacro(const std::string& name, uint32_t offset)
    {
      auto it = localMacros.find(name);
      if (it != localMacros.end() && !it->second.empty() && it->second.back().to == UINT32_MAX)
        it->second.back().to = offset;
    }
    std::set<std::string> replacedFunctions;  // qualified names whose definitions were found and removed
    std::set<std::string> replacedVariables;  // the same, of variables
    std::vector<Edit> edits;
    // A use of an argument macro, expanded in run() once the edits inside its arguments are known.
    struct ArgumentMacroUse
    {
      uint32_t begin, end; // the use, from the name to the closing parenthesis
      std::vector<std::pair<uint32_t, uint32_t>> args; // offsets
      const std::string* replacement;
    };
    std::vector<ArgumentMacroUse> argumentMacroUses;
    // A use of a local macro (this file's, or a header's it includes), expanded in run() once the
    // edits inside its arguments are known, as argument macros are: the arguments' tokens stay in
    // the stream, so the rewrites of code apply in them too (a renamed alias, a chrono literal,
    // this->: TestLatencyCarWhiteBox's `CHECK_EQUAL(State(...), ...)`, TestUDPSocket's
    // `CHECK(wait_for(50ms))`).
    struct LocalMacroUse
    {
      uint32_t begin, end; // the use, from the name to the closing parenthesis
      std::vector<std::pair<uint32_t, uint32_t>> args; // offsets; an empty argument is an empty range
      const LocalMacro* macro;
    };
    std::vector<LocalMacroUse> localMacroUses;
    // A hidden friend defined in a class that moves out to namespace scope (run() renders it once
    // the edits inside it are known): text[begin, end) goes to `at`, after `header`, with the names
    // it found in the class's scope (nested types) qualified; [begin, removeEnd) goes.
    struct MovedFriend
    {
      uint32_t begin, end, removeEnd, at;
      std::string header;
      std::map<std::string, std::string> qualify; // bare name -> qualified name
    };
    std::vector<MovedFriend> movedFriends;
    std::vector<ClassDecl*> classStack; // the classes whose members convertDecls is in, outermost first
    // The identifiers of allocator arguments dropped from std container types (convertTokens), by
    // offset: a template parameter used only there goes from its header too (dropAllocatorParameters).
    std::map<uint32_t, std::string> droppedAllocators;
    Counts counts;
    bool importedCommon = false;
    bool importedDirectX = false; // kpp_directx.h for the first DirectX header of the file
    std::vector<std::string> todos; // "line: text"
    std::string_view text;
    const std::vector<Token>& toks;

    FileConverter(const ConvertInput& in, GlobalScope& global, const SymbolTable& symbols)
      : in(in), global(global), symbols(symbols), text(in.text), toks(*in.tokens)
    {
    }

    uint32_t lineOf(uint32_t offset) const
    {
      uint32_t line = 1;
      for (uint32_t i = 0; i < offset && i < text.size(); ++i)
        if (text[i] == '\n')
          ++line;
      return line;
    }

    void todo(uint32_t offset, const std::string& what)
    {
      todos.push_back(std::to_string(lineOf(offset)) + ": " + what);
      counts.add("todo");
    }

    void remove(uint32_t offset, uint32_t length) { edits.push_back({offset, length, ""}); }
    void insert(uint32_t offset, const std::string& s) { edits.push_back({offset, 0, s}); }
    void replace(uint32_t offset, uint32_t length, const std::string& s) { edits.push_back({offset, length, s}); }

    // s with __FILE__ and __LINE__ replaced by this file's name and the line of offset.
    std::string withFileAndLine(std::string s, uint32_t offset) const
    {
      for (size_t p = s.find("__FILE__"); p != std::string::npos; p = s.find("__FILE__", p))
      {
        std::string literal = "\"" + in.path.filename().string() + "\"";
        s.replace(p, 8, literal);
        p += literal.size();
      }
      for (size_t p = s.find("__LINE__"); p != std::string::npos; p = s.find("__LINE__", p))
      {
        std::string line = std::to_string(1 + std::count(text.begin(), text.begin() + std::min<size_t>(offset, text.size()), '\n'));
        s.replace(p, 8, line);
        p += line.size();
      }
      // __FUNCTION__ and __func__: the name of the function around the use, qualified as MSVC's
      // __FUNCTION__ is (`FramebufferOpenGL::generateFramebufferObject`; a template's without its
      // arguments). Outside a function they stay, and the compiler reports them.
      for (const char* macro : {"__FUNCTION__", "__func__"})
        for (size_t p = s.find(macro); p != std::string::npos; p = s.find(macro, p))
        {
          size_t length = strlen(macro);
          bool word = (p == 0 || !isIdentChar(s[p - 1])) && (p + length >= s.size() || !isIdentChar(s[p + length]));
          const std::string* name = functionNameAt(offset);
          if (!word || !name)
          {
            p += length;
            continue;
          }
          std::string literal = "\"" + *name + "\"";
          s.replace(p, length, literal);
          p += literal.size();
        }
      return s;
    }
    // The qualified name of the innermost function whose definition holds offset, or null.
    const std::string* functionNameAt(uint32_t offset) const
    {
      const FunctionRange* best = nullptr;
      for (const FunctionRange& f : functionRanges)
        if (f.begin <= offset && offset < f.end && (!best || f.begin >= best->begin))
          best = &f;
      return best ? &best->name : nullptr;
    }

    // Whether token is a keyword that a parenthesis after it belongs to, which makes `(NAME)` no
    // cast: a functional cast to a builtin type (`float(M_PI) * x`), sizeof, alignof, decltype, noexcept.
    bool callsWithParenthesis(uint32_t token) const
    {
      static const std::set<std::string_view> keywords = {"bool", "char", "char8_t", "char16_t", "char32_t", "wchar_t", "short", "int", "long", "signed",
                                                          "unsigned", "float", "double", "sizeof", "alignof", "decltype", "noexcept"};
      return toks[token].getKind() == TokenKind::Keyword && keywords.count(this->spelling(token));
    }
    std::string_view spelling(uint32_t token) const
    {
      const Token& t = toks[token];
      return text.substr(t.offset, t.length);
    }
    uint32_t endOffset(uint32_t token) const { return toks[token].offset + toks[token].length; }

    // --- preprocessor lines ---

    struct Lines
    {
      std::vector<std::pair<uint32_t, uint32_t>> lines; // offset, length (without the newline)
    };

    // Turns `#define NAME body` into a constant where the body is one, else a TODO comment.
    std::string macroToKpp(const std::string& line, uint32_t offset)
    {
      size_t p = line.find("define") + 6;
      while (p < line.size() && isspace((unsigned char)line[p]))
        ++p;
      size_t nameStart = p;
      while (p < line.size() && isIdentChar(line[p]))
        ++p;
      std::string name = line.substr(nameStart, p - nameStart);
      bool functionLike = p < line.size() && line[p] == '(';
      std::string body = line.substr(p);
      size_t comment = body.find("//");
      std::string trailing;
      if (comment != std::string::npos)
      {
        trailing = " " + body.substr(comment);
        body = body.substr(0, comment);
      }
      while (!body.empty() && isspace((unsigned char)body.back()))
        body.pop_back();
      while (!body.empty() && isspace((unsigned char)body.front()))
        body.erase(body.begin());
      if (knownMacros.count(name) || knownFunctionMacros.count(name) || argumentMacros.count(name))
      {
        counts.add("macro known");
        return ""; // its uses are rewritten
      }
      if (functionLike)
      {
        // A function-like macro of this file (HANDLE_CASE in Listener.cpp): its uses in this file
        // are expanded in place by convertTokens.
        size_t close = body.find(')');
        if (close != std::string::npos)
        {
          LocalMacro m;
          std::string paramText = body.substr(1, close - 1);
          // A parameter list continued over lines (CALCULATE_VERTICES): the line ends are spaces.
          for (char& ch : paramText)
            if (ch == '\\' || ch == '\r' || ch == '\n')
              ch = ' ';
          size_t start = 0;
          while (start <= paramText.size())
          {
            size_t comma = paramText.find(',', start);
            std::string param = paramText.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            while (!param.empty() && isspace((unsigned char)param.front()))
              param.erase(param.begin());
            while (!param.empty() && isspace((unsigned char)param.back()))
              param.pop_back();
            if (!param.empty())
              m.params.push_back(param);
            if (comma == std::string::npos)
              break;
            start = comma + 1;
          }
          m.body = continuationsAsSpaces(body.substr(close + 1));
          while (!m.body.empty() && isspace((unsigned char)m.body.front()))
            m.body.erase(m.body.begin());
          while (!m.body.empty() && isspace((unsigned char)m.body.back()))
            m.body.pop_back();
          if (!m.body.empty())
          {
            endLocalMacro(name, offset); // a redefinition without #undef replaces the earlier one
            m.from = offset;
            localMacros[name].push_back(m);
            counts.add("local function-like macro");
            return "";
          }
        }
      }
      if (!functionLike && body == name)
      {
        // Crypto.cpp's `#define fileno fileno` (so that curl's `#ifndef fileno` declares nothing):
        // a use is the name itself, as the preprocessor leaves it.
        counts.add("macros naming themselves");
        return "// macro naming itself: " + line;
      }
      if (const LocalMacro* aliased = functionLike || body == name ? nullptr : localMacroAt(body, offset))
      {
        // Another name for a function-like macro (lobject.h's `#define setobj2s setobj`): that macro
        // under this name, expanded at its uses as the preprocessor expands it (a constant of it
        // would name no value).
        LocalMacro m = *aliased;
        endLocalMacro(name, offset);
        m.from = offset;
        m.to = UINT32_MAX;
        localMacros[name].push_back(m);
        counts.add("local function-like macro");
        return "";
      }
      // Made of string macros (lua.h's LUA_RELEASE): their literals; another name for a macro
      // written out at its uses (luaconf.h's `#define LUALIB_API LUA_API`, LUA_API being `extern
      // "C"`): its text.
      if (!functionLike && !isStringLiterals(body) && objectMacros.count(name) &&
          (isStringLiterals(objectMacros.at(name)) || isSplicedMacroBody(objectMacros.at(name))))
        body = objectMacros.at(name);
      if (!functionLike && (isSplicedMacroBody(body) || (isAliasMacro(name, body) && !localMacroAt(body, offset)) ||
                            (functionMacroNames && callsFunction(body, *functionMacroNames))))
      {
        // Written out at its uses (convertTokens, objectMacros): declarations, a type, storage words,
        // another name.
        counts.add("spliced macros");
        std::string commented = "// macro written out at its uses: ";
        for (char c : line)
        {
          commented += c;
          if (c == '\n')
            commented += "// ";
        }
        return commented;
      }
      if (!functionLike && !body.empty())
      {
        // Continued over lines (lgc.c's `#define sweepphases \` with its value on the next line):
        // the lines without their backslashes.
        for (size_t k = body.find('\\'); k != std::string::npos; k = body.find('\\', k))
        {
          size_t e = k + 1;
          while (e < body.size() && (body[e] == ' ' || body[e] == '\t' || body[e] == '\r'))
            ++e;
          if (e < body.size() && body[e] == '\n')
            body.erase(k, 1);
          else
            k += 2; // an escape in a literal (`'\\'`)
        }
        bool constant = true;
        int quotes = 0, depth = 0;
        // Checked without its comments, with tabs and line ends as spaces (lstate.h's `#define
        // CIST_LUA\t(1<<0)\t/* ... */`) and with what a character literal holds as `x`
        // (lstrlib.c's `#define L_ESC '%'`).
        std::string checked = body;
        for (size_t c = checked.find("/*"); c != std::string::npos; c = checked.find("/*", c))
        {
          size_t e = checked.find("*/", c + 2);
          checked.replace(c, (e == std::string::npos ? checked.size() : e + 2) - c, " ");
        }
        for (char& c : checked)
          if (c == '\t' || c == '\r' || c == '\n')
            c = ' ';
        for (size_t c = 0; c < checked.size(); ++c)
          if (checked[c] == '"')
          {
            for (++c; c < checked.size() && checked[c] != '"'; ++c)
              if (checked[c] == '\\')
                ++c;
          }
          else if (checked[c] == '\'' && !(c > 0 && isalnum((unsigned char)checked[c - 1]))) // not a digit separator
          {
            for (++c; c < checked.size() && checked[c] != '\''; ++c)
            {
              if (checked[c] == '\\' && c + 1 < checked.size())
                checked[c++] = 'x';
              checked[c] = 'x';
            }
          }
        for (char c : checked)
        {
          if (c == '"')
            ++quotes;
          else if (!(isIdentChar(c) || strchr(" .+-*/()<>|&~!^,'\\xX", c)))
            constant = false;
          else if (quotes % 2 == 0)
          {
            depth += c == '(' ? 1 : c == ')' ? -1 : 0;
            if (c == ',' && depth == 0)
              constant = false; // a list (`2,1,0` for a resource script), not a value
          }
        }
        // A body of declaration words (`#define STBDDEF static`) is no value either.
        static const std::set<std::string> declarationWords = {"static", "inline", "extern", "const", "volatile", "register", "thread_local",
                                                              "constexpr", "virtual", "explicit", "friend", "typedef", "struct", "class",
                                                              "union", "enum", "template", "typename", "using", "namespace", "__forceinline",
                                                              "__inline", "__declspec", "__cdecl", "__stdcall", "__fastcall"};
        for (size_t k = 0; k < body.size() && constant;)
        {
          if (body[k] == '"')
          {
            size_t e = body.find('"', k + 1);
            k = e == std::string::npos ? body.size() : e + 1;
          }
          else if (isalpha((unsigned char)body[k]) || body[k] == '_')
          {
            size_t e = k;
            while (e < body.size() && isIdentChar(body[e]))
              ++e;
            if (declarationWords.count(body.substr(k, e - k)))
              constant = false;
            k = e;
          }
          else if (isdigit((unsigned char)body[k]))
          {
            while (k < body.size() && isIdentChar(body[k]))
              ++k;
          }
          else
            ++k;
        }
        if (constant && (quotes == 0 || quotes == 2))
        {
          // Its function-like macros expanded, as at a use (lgc.h's `#define WHITEBITS
          // bit2mask(WHITE0BIT, WHITE1BIT)`).
          if (!localMacros.empty())
            body = rescanMacros(body, offset);
          counts.add("macro to constant");
          std::string indent = line.substr(0, line.find('#'));
          // In a class's definition (StackWalker's `#define MAX_MODULE_NAME32 255` among its
          // members) the constant is the class's, not a data member of each object.
          const ScopeRange* innermost = nullptr;
          for (const ScopeRange& r : scopeRanges)
            if (r.begin <= offset && offset < r.end && (!innermost || r.begin >= innermost->begin))
              innermost = &r;
          return indent + (innermost && innermost->classBody ? "static constexpr auto " : "const auto ") + name + " = " + body + ";" + trailing;
        }
      }
      todo(offset, "macro " + name + (functionLike ? "(...)" : "") + " kept as a comment");
      counts.add("macro todo");
      std::string commented = "// TODO(kpp) macro: ";
      for (char c : line)
      {
        commented += c;
        if (c == '\n')
          commented += "// ";
      }
      return commented;
    }

    void convertPreprocessor()
    {
      // Logical lines with continuations joined.
      std::vector<std::pair<uint32_t, uint32_t>> lines;
      size_t pos = 0;
      while (pos < text.size())
      {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
          end = text.size();
        size_t start = pos;
        while (end < text.size())
        {
          size_t last = end;
          while (last > start && (text[last - 1] == '\r' || text[last - 1] == ' '))
            --last;
          if (last > start && text[last - 1] == '\\')
          {
            end = text.find('\n', end + 1);
            if (end == std::string::npos)
              end = text.size();
            continue;
          }
          break;
        }
        // A directive whose block comment goes on past its line (lstate.h's `#define CIST_REENTRY
        // (1<<2) /* call is running on same invocation of`, closed on the next line) ends where the
        // comment does, as the preprocessor reads it.
        size_t first = text.find_first_not_of(" \t", start);
        if (first < end && text[first] == '#')
          while (end < text.size() && opensBlockComment(text.substr(start, end - start)))
          {
            size_t close = text.find("*/", end);
            end = close == std::string_view::npos ? text.size() : text.find('\n', close + 2);
            if (end == std::string_view::npos)
              end = text.size();
          }
        lines.push_back({uint32_t(start), uint32_t(end - start)});
        pos = end + 1;
      }
      // The configuration decided which lines stay (preprocessText, shared with the front end).
      Preprocessed own;
      if (!preprocessed)
        own = preprocessText(text, nullptr);
      const Preprocessed& decided = preprocessed ? *preprocessed : own;
      size_t nextDrop = 0, nextNote = 0;
      bool inComment = false; // as in preprocessText: a line starting inside a block comment is no directive
      for (auto [offset, length] : lines)
      {
        while (nextDrop < decided.dropped.size() && decided.dropped[nextDrop].second <= offset)
          ++nextDrop;
        bool active = !(nextDrop < decided.dropped.size() && decided.dropped[nextDrop].first <= offset);
        while (nextNote < decided.notes.size() && decided.notes[nextNote].first < offset)
          ++nextNote;
        for (; nextNote < decided.notes.size() && decided.notes[nextNote].first == offset; ++nextNote)
          todo(offset, decided.notes[nextNote].second);
        std::string line(text.substr(offset, length));
        bool commentAtStart = inComment;
        inComment = blockCommentOpenAfter(line, inComment);
        size_t p = offset == 0 ? byteOrderMarkLength(text) : 0;
        while (p < line.size() && isspace((unsigned char)line[p]))
          ++p;
        bool directive = !commentAtStart && p < line.size() && line[p] == '#';
        uint32_t removeLength = length + (offset + length < text.size() ? 1 : 0); // with its newline
        if (!directive)
        {
          if (!active)
          {
            remove(offset, removeLength);
            counts.add("lines dropped by #if");
          }
          continue;
        }
        ++p;
        while (p < line.size() && isspace((unsigned char)line[p]))
          ++p;
        size_t ws = p;
        while (p < line.size() && isalpha((unsigned char)line[p]))
          ++p;
        std::string word = line.substr(ws, p - ws);
        std::string rest = line.substr(p);
        size_t c = rest.find("//");
        if (c != std::string::npos)
          rest = rest.substr(0, c);
        for (char& ch : rest)
          if (ch == '\\' || ch == '\r')
            ch = ' ';
        if (word == "if" || word == "ifdef" || word == "ifndef")
          counts.add("#if");
        else if (word == "elif" || word == "else" || word == "endif")
        {
        }
        else if (!active)
        {
          // a directive inside a dropped branch
        }
        else if (word == "include")
        {
          // C headers become imports; C++ and project headers go (every project symbol is visible).
          static const std::set<std::string> cHeaders = {"windows.h", "stdio.h", "stdlib.h", "string.h", "math.h", "ctype.h", "errno.h",
                                                         "stdarg.h", "stdint.h", "stddef.h", "time.h", "io.h", "fcntl.h", "direct.h",
                                                         "process.h", "signal.h", "locale.h", "wchar.h", "float.h", "limits.h", "assert.h",
                                                         "conio.h", "intrin.h", "malloc.h", "memory.h", "wctype.h", "inttypes.h",
                                                         "shellapi.h", "shlobj.h", "winsock2.h", "ws2tcpip.h", "psapi.h", "dbghelp.h",
                                                         "tlhelp32.h", "shlwapi.h", "timeapi.h", "versionhelpers.h", "iphlpapi.h", "windns.h"};
          size_t open = rest.find_first_of("<\"");
          size_t close = rest.find_first_of(">\"", open + 1);
          std::string header = open != std::string::npos && close != std::string::npos ? rest.substr(open + 1, close - open - 1) : "";
          std::string base = header.substr(header.find_last_of('/') + 1);
          static const std::map<std::string, std::string> cxxToC = {{"cmath", "math.h"}, {"cstdio", "stdio.h"}, {"cstring", "string.h"},
                                                                    {"cstdlib", "stdlib.h"}, {"cctype", "ctype.h"}, {"cerrno", "errno.h"},
                                                                    {"cstdarg", "stdarg.h"}, {"ctime", "time.h"}, {"cwchar", "wchar.h"},
                                                                    {"cfloat", "float.h"}, {"climits", "limits.h"}, {"cstdint", "stdint.h"},
                                                                    {"cstddef", "stddef.h"}, {"cassert", "assert.h"}, {"csignal", "signal.h"},
                                                                    {"clocale", "locale.h"}, {"cwctype", "wctype.h"}, {"cinttypes", "inttypes.h"}};
          if (siblingIncludes && siblingIncludes->count(header))
          {
            // A converted file of this file's directory (Lua's own headers, by names a C library's
            // prefix would otherwise take for an import): its symbols are the program's.
            remove(offset, removeLength);
            counts.add("includes removed");
            continue;
          }
          auto cxx = cxxToC.find(base);
          if (cxx != cxxToC.end())
            base = cxx->second;
          // C libraries the game uses, by their include path prefix.
          // (stb_rect_pack, jc_voronoi and sdf are header-only C libraries whose implementations the
          // game compiles in its own .cpp files.)
          static const char* cLibraries[] = {"SDL3/", "SDL2/", "SDL", "zlib", "zconf", "curl/", "png", "jpeg", "zstd", "opus", "ogg",
                                             "vorbis", "minizip", "stb_image", "fontstash", "imgui/stb_rect_pack", "jc_voronoi/", "sdf/", "glad/",
                                             "CL/"};
          bool cLibrary = false;
          for (const char* prefix : cLibraries)
            if (header.rfind(prefix, 0) == 0)
              cLibrary = true;
          if (keptCLibraries)
            for (const std::string& prefix : *keptCLibraries)
              if (header.rfind(prefix, 0) == 0)
                cLibrary = true;
          static const std::set<std::string> directXHeaders = {"d3d11.h", "d3d11_1.h", "d3d11sdklayers.h", "dxgi.h", "dxgi1_2.h", "dxgi1_3.h",
                                                               "dxgi1_4.h", "dxgi1_5.h", "dxgi1_6.h", "d3dcommon.h", "d3d9.h", "d3dcompiler.h",
                                                               "wincodec.h", "objidl.h", "wbemidl.h", "wbemcli.h"};
          std::string lowered = base;
          for (char& ch : lowered)
            ch = (char)tolower((unsigned char)ch);
          if (directXHeaders.count(lowered))
          {
            // DirectX, DXGI, WIC, WMI and the COM headers: programs/factorio/native/include/kpp_directx.h,
            // which leaves out the C structs of the interfaces Kpp declares as classes
            // (overrides/added/src/Graphics/DX11/ComInterfaces.kpp, generate_com.py). Once per file.
            if (importedDirectX)
            {
              remove(offset, removeLength);
              counts.add("includes removed");
              continue;
            }
            importedDirectX = true;
            replace(offset, length, "extern \"C\" import \"kpp_directx.h\";");
            counts.add("includes to imports");
            continue;
          }
          if (cHeaders.count(lowered) || cLibrary) // Windows' headers by any case (<Psapi.h>, <TlHelp32.h>)
          {
            // A C library that the game configures with #defines before its include imports through a
            // header of programs/factorio/native/include that gives the import the same configuration
            // (jc_voronoi in double precision, as Triangulation.cpp has it).
            static const std::map<std::string, std::string> configuredImports = {{"jc_voronoi/jc_voronoi.h", "kpp_jc_voronoi.h"},
                                                                                 {"stb_image/stb_dxt.h", "kpp_stb_dxt.h"}};
            auto configured = configuredImports.find(header);
            std::string imported = configured != configuredImports.end() ? configured->second : cLibrary ? header : lowered;
            std::string imports = "extern \"C\" import \"" + imported + "\";";
            if (!importedCommon)
            {
              // The C++ standard headers brought these in transitively; converted code expects them.
              importedCommon = true;
              for (const char* common : {"stdlib.h", "string.h", "stdio.h"})
                if (base != common)
                  imports += std::string("\nextern \"C\" import \"") + common + "\";";
            }
            replace(offset, length, imports);
            counts.add("includes to imports");
            continue;
          }
          counts.add("includes removed");
        }
        else if (word == "pragma" || word == "undef" || word == "error" || word == "warning")
        {
          if (word == "pragma")
            notePack(rest, offset);
          if (word == "error")
            todo(offset, "#error" + rest + ": an #error this configuration reaches (a file for another platform or build?)");
          if (word == "undef")
          {
            size_t n = rest.find_first_not_of(" \t");
            size_t e = n == std::string::npos ? n : rest.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", n);
            if (n != std::string::npos)
            {
              std::string name = rest.substr(n, e == std::string::npos ? std::string::npos : e - n);
              endLocalMacro(name, offset);
              if (auto range = ownMacroRanges.find(name); range != ownMacroRanges.end() && range->second.second == UINT32_MAX)
                range->second.second = offset;
            }
          }
          counts.add("#" + word + " removed");
        }
        else if (word == "define")
        {
          {
            size_t n = rest.find_first_not_of(" \t");
            size_t e = n == std::string::npos ? n : rest.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", n);
            if (n != std::string::npos)
              ownMacroRanges[rest.substr(n, e == std::string::npos ? std::string::npos : e - n)] = {offset, UINT32_MAX};
          }
          std::string replacement = macroToKpp(line, offset);
          if (!replacement.empty())
          {
            replace(offset, length, replacement);
            continue;
          }
        }
        remove(offset, removeLength);
      }
    }

    // --- token-level rewrites ---

    // A macro argument as `#x` makes it a string literal, by C's rule: the white space between its
    // tokens, line breaks too (an argument may span lines: a release_assert of a long condition),
    // becomes one space, none at its ends, and the `"` and `\` of its string and character
    // literals are escaped.
    static std::string stringized(const std::string& argument)
    {
      std::string out = "\"";
      bool space = false;
      for (size_t i = 0; i < argument.size();)
      {
        char c = argument[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
          space = out.size() > 1;
          ++i;
          continue;
        }
        if (space)
          out += ' ';
        space = false;
        if (c == '"' || (c == '\'' && !isDigitSeparator(argument, i)))
        {
          size_t j = i + 1;
          if (size_t raw = rawStringEnd(argument, i); raw != std::string_view::npos)
            j = raw - 1;
          while (j < argument.size() && argument[j] != c && argument[j] != '\n')
            j += argument[j] == '\\' && j + 1 < argument.size() ? 2 : 1;
          size_t end = std::min(j + 1, argument.size());
          for (size_t k = i; k < end; ++k)
          {
            if (argument[k] == '"' || argument[k] == '\\')
              out += '\\';
            out += argument[k];
          }
          i = end;
          continue;
        }
        out += c;
        ++i;
      }
      return out + "\"";
    }

    // Whether a macro is variadic: its last parameter is `...`, which __VA_ARGS__ stands for.
    static bool variadic(const LocalMacro& m) { return !m.params.empty() && m.params.back() == "..."; }
    // Whether a use with count arguments fits the macro: one each, or for a variadic macro any number
    // for the `...` (none included). Fewer are empty, as the game's compiler (MSVC) has it:
    // UpgradableEntity's FOR_EACH_SUPPORTED_TYPE(DEFINE_ENUM_ENTRY,) passes DEFINE_ENUM_ENTRY(type, _)
    // one argument once its empty __VA_ARGS__ takes the comma along.
    static bool argumentsFit(const LocalMacro& m, size_t count)
    {
      return variadic(m) || count <= m.params.size();
    }

    // The body of a local macro with its parameters replaced by the argument texts; `#x` quotes,
    // `##` pastes. __VA_ARGS__ is the arguments of a variadic macro's `...`, joined by commas; when
    // there are none, a comma right before it goes too, as the game's compiler (MSVC) has it.
    // An argument passed on from one macro to another: its rewritten text and its spelling, between
    // \x01, \x02 and \x03, until the inner macro takes one of them (untagged).
    static std::string taggedArgument(const std::string& rewritten, const std::string& spelling)
    {
      return "\x01" + untagged(rewritten, true) + "\x02" + untagged(spelling, false) + "\x03";
    }
    static std::string withoutProtection(std::string text)
    {
      text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == '\x04' || c == '\x05'; }), text.end());
      return text;
    }
    static std::string untagged(const std::string& text, bool rewritten)
    {
      if (text.find('\x01') == std::string::npos)
        return text;
      std::string out;
      for (size_t i = 0; i < text.size();)
      {
        size_t open = text.find('\x01', i);
        if (open == std::string::npos)
        {
          out.append(text, i, std::string::npos);
          break;
        }
        size_t middle = text.find('\x02', open), close = text.find('\x03', middle);
        out.append(text, i, open - i);
        out += rewritten ? text.substr(open + 1, middle - open - 1) : text.substr(middle + 1, close - middle - 1);
        i = close + 1;
      }
      return out;
    }
    // The macro's body with the arguments put in. spellings, when given, are the arguments as written,
    // which # and ## take (C++ stringizes and pastes the tokens as they are), a place right after
    // `.`, `->` or `::` (a member's name: Agui's Listener puts its argument in `this->NAME` and
    // `other.NAME`); elsewhere args, the arguments with the conversion's rewrites. An argument passed on
    // to another macro takes both, tagged (taggedArgument), for that macro to choose by its own use
    // of it: UpgradableEntity's dispatch puts its call after `->`, UnitTest++'s TEST passes its name
    // on to be pasted and stringized, CHECK passes its value on to be evaluated.
    std::string expandMacroOnce(const LocalMacro& m, const std::vector<std::string>& args, const std::vector<std::string>* spellings = nullptr,
                                uint32_t useOffset = 0)
    {
      // For each parenthesis open in the body, whether it is a macro's argument list.
      std::vector<bool> macroCalls;
      std::string out;
      const std::string& b = m.body;
      auto variadicOf = [&](const std::vector<std::string>& list)
      {
        std::string joined;
        for (size_t k = m.params.size() - 1; k < list.size(); ++k)
          joined += (k + 1 == m.params.size() ? "" : ", ") + list[k];
        return joined;
      };
      std::string variadicArgs = variadic(m) ? variadicOf(args) : std::string();
      bool afterPaste = false;
      // The text of argument k put in at body position at (just after the parameter's name): its
      // spelling after # or next to ##, or after `.`, `->` or `::`; both, tagged, inside another
      // macro's arguments; else the rewritten argument.
      auto argumentAt = [&](size_t k, size_t at, bool stringize, bool pasteBefore) -> std::string
      {
        if (!spellings)
          return k < args.size() ? args[k] : std::string();
        size_t next = at;
        while (next < b.size() && b[next] == ' ')
          ++next;
        bool pasted = pasteBefore || b.compare(next, 2, "##") == 0;
        size_t last = out.find_last_not_of(" \t");
        bool member = last != std::string::npos &&
                      (out[last] == '.' || (last > 0 && out[last] == '>' && out[last - 1] == '-') || (last > 0 && out[last] == ':' && out[last - 1] == ':'));
        bool forwarded = std::find(macroCalls.begin(), macroCalls.end(), true) != macroCalls.end();
        std::string argument = k < args.size() ? args[k] : std::string();
        std::string spelling = k < spellings->size() ? (*spellings)[k] : std::string();
        if (stringize || pasted || member)
          return spelling;
        if (forwarded && argument != spelling)
          return taggedArgument(argument, spelling);
        // (Renamed already where a rewrite changed it: expandMacro's renames pass it by. One the
        // rewrites left alone stays plain text, a macro's name passed to an X-macro list among them.)
        return argument == spelling ? argument : "\x04" + argument + "\x05";
      };
      for (size_t i = 0; i < b.size();)
      {
        char c = b[i];
        if (c == '#' && i + 1 < b.size() && b[i + 1] == '#')
        {
          while (!out.empty() && out.back() == ' ')
            out.pop_back();
          i += 2;
          while (i < b.size() && b[i] == ' ')
            ++i;
          afterPaste = true;
          continue;
        }
        bool pasteBefore = afterPaste;
        afterPaste = false;
        if (spellings && c == '(')
        {
          size_t last = out.find_last_not_of(" \t");
          size_t start = last;
          while (start != std::string::npos && start > 0 && isIdentChar(out[start - 1]))
            --start;
          bool call = last != std::string::npos && isIdentChar(out[last]) && localMacroAt(out.substr(start, last + 1 - start), useOffset);
          macroCalls.push_back(call);
        }
        else if (spellings && c == ')' && !macroCalls.empty())
          macroCalls.pop_back();
        if (variadic(m) && b.compare(i, 11, "__VA_ARGS__") == 0 && (i == 0 || !isIdentChar(b[i - 1])) &&
            (i + 11 == b.size() || !isIdentChar(b[i + 11])))
        {
          std::string joined;
          for (size_t k = m.params.size() - 1; k < args.size(); ++k)
            joined += (k + 1 == m.params.size() ? "" : ", ") + argumentAt(k, i + 11, false, pasteBefore);
          if (joined.empty())
          {
            size_t end = out.find_last_not_of(" \t");
            if (end != std::string::npos && out[end] == ',')
              out.erase(end);
          }
          out += joined;
          i += 11;
          continue;
        }
        bool stringize = c == '#';
        size_t idStart = stringize ? i + 1 : i;
        if (idStart < b.size() && (isalpha((unsigned char)b[idStart]) || b[idStart] == '_') && (idStart == 0 || stringize || !isIdentChar(b[idStart - 1])))
        {
          size_t j = idStart;
          while (j < b.size() && isIdentChar(b[j]))
            ++j;
          std::string word = b.substr(idStart, j - idStart);
          size_t which = m.params.size();
          for (size_t k = 0; k < m.params.size(); ++k)
            if (m.params[k] == word)
              which = k;
          if (which < m.params.size())
          {
            std::string argument = argumentAt(which, j, stringize, pasteBefore);
            out += stringize ? stringized(withoutProtection(untagged(argument, false))) : argument;
          }
          else if (stringize && word == "__VA_ARGS__" && variadic(m))
            out += stringized(spellings ? variadicOf(*spellings) : variadicArgs);
          else
            out.append(b, i, j - i);
          i = j;
          continue;
        }
        if (size_t end = commentEnd(b, i); end != std::string::npos)
        {
          // A comment the expansion keeps, as written: an apostrophe in it (DrawQueue's "I'm not
          // sure") is no character literal that would hide the parameters after it.
          out.append(b, i, end - i);
          i = end;
          continue;
        }
        if (c == '"' || (c == '\'' && !isDigitSeparator(b, i)))
        {
          size_t j = i + 1;
          if (size_t raw = rawStringEnd(b, i); raw != std::string_view::npos)
            j = raw - 1;
          while (j < b.size() && b[j] != c)
          {
            if (b[j] == '\\')
              ++j;
            ++j;
          }
          size_t end = std::min(j + 1, b.size());
          out.append(b, i, end - i);
          i = end;
          continue;
        }
        out += c;
        ++i;
      }
      return out;
    }

    // The end of the comment (`/* */` or `//` to the line's end) that starts at i in macro text;
    // npos when none starts there.
    static size_t commentEnd(const std::string& s, size_t i)
    {
      if (s[i] != '/' || i + 1 >= s.size())
        return std::string::npos;
      if (s[i + 1] == '*')
      {
        size_t close = s.find("*/", i + 2);
        return close == std::string::npos ? s.size() : close + 2;
      }
      if (s[i + 1] == '/')
        return std::min(s.find('\n', i), s.size());
      return std::string::npos;
    }

    // The `)` that closes the `(` at open in macro text, skipping string and character literals and
    // comments; npos when there is none.
    static size_t closingParen(const std::string& s, size_t open)
    {
      int depth = 0;
      for (size_t k = open; k < s.size(); ++k)
      {
        if (size_t raw = rawStringEnd(s, k); raw != std::string_view::npos)
          k = raw - 1;
        else if (size_t end = commentEnd(s, k); end != std::string::npos)
          k = end - 1;
        else if (s[k] == '"' || (s[k] == '\'' && !isDigitSeparator(s, k)))
        {
          char quote = s[k];
          for (++k; k < s.size() && s[k] != quote; ++k)
            if (s[k] == '\\')
              ++k;
        }
        else if (s[k] == '(')
          ++depth;
        else if (s[k] == ')' && --depth == 0)
          return k;
      }
      return std::string::npos;
    }

    // useOffset is where the macro is used, for __FILE__ and __LINE__ in known macros' replacements.
    std::string expandMacro(const LocalMacro& m, const std::vector<std::string>& args, uint32_t useOffset, const std::vector<std::string>* spellings = nullptr)
    {
      // The preprocessor's file and line in the body are the use's (`__FILE__` in a header's
      // release_assert-like macro).
      return this->renameOutsideArguments(withFileAndLine(untagged(rescanMacros(expandMacroOnce(m, args, spellings, useOffset), useOffset), true), useOffset));
    }
    // C's `assert(c)` where an expression stands (llimits.h's `#define check_exp(c,e)
    // (lua_assert(c), (e))`): Kpp's assert is a statement, so the assert goes in a lambda called on
    // the spot, which asserts as the statement does.
    static bool expressionPosition(const std::string& before)
    {
      size_t last = before.find_last_not_of(" \t\n");
      return last != std::string::npos && strchr("(,?=&|!+-*/%<>^~[", before[last]);
    }
    std::string rescanMacros(std::string result, uint32_t useOffset)
    {
      // Re-scan for nested local macro calls (X-macro pattern: outer expands to inner(args) calls)
      // and for the known function macros (a local macro wrapping SAVING_TAG, for example), which
      // the token-level rewrite does not see inside the expansion's text. Each pass goes one level
      // deeper, until nothing is left to expand: Lua's nest about eight deep (setsvalue2s through
      // checkliveness, righttt and ttypenv to rttype).
      for (int pass = 0; pass < 16; ++pass)
      {
        bool changed = false;
        std::string next;
        for (size_t i = 0; i < result.size();)
        {
          if (isalpha((unsigned char)result[i]) || result[i] == '_')
          {
            size_t j = i;
            while (j < result.size() && isIdentChar(result[j]))
              ++j;
            std::string name = result.substr(i, j - i);
            if (name == "assert" && j < result.size() && result[j] == '(' && expressionPosition(next))
            {
              size_t close = closingParen(result, j);
              if (close != std::string::npos)
              {
                next += "([&] { assert(" + result.substr(j + 1, close - j - 1) + "); }())";
                changed = true;
                i = close + 1;
                counts.add("asserts in expressions put in lambdas");
                continue;
              }
            }
            const LocalMacro* inner = localMacroAt(name, useOffset); // the definition in force where the outer macro is used
            if (inner && j < result.size() && result[j] == '(')
            {
              // Parse arguments from the text.
              size_t k = j + 1;
              int depth = 0;
              std::vector<std::string> innerArgs;
              size_t argStart = k;
              bool closed = false;
              for (; k < result.size(); ++k)
              {
                if (result[k] == '(' || result[k] == '[' || result[k] == '{')
                  ++depth;
                else if (result[k] == ')' || result[k] == ']' || result[k] == '}')
                {
                  if (depth == 0) { closed = result[k] == ')'; break; }
                  --depth;
                }
                else if (result[k] == ',' && depth == 0)
                {
                  innerArgs.push_back(result.substr(argStart, k - argStart));
                  argStart = k + 1;
                }
                else if (size_t raw = rawStringEnd(result, k); raw != std::string_view::npos)
                  k = raw - 1;
                else if (result[k] == '"')
                {
                  ++k;
                  while (k < result.size() && result[k] != '"') { if (result[k] == '\\') ++k; ++k; }
                }
              }
              if (closed && (k > argStart || !innerArgs.empty()))
                innerArgs.push_back(result.substr(argStart, k - argStart));
              // Trim whitespace from args.
              for (auto& a : innerArgs)
              {
                while (!a.empty() && isspace((unsigned char)a.front())) a.erase(a.begin());
                while (!a.empty() && isspace((unsigned char)a.back())) a.pop_back();
              }
              if (closed && argumentsFit(*inner, innerArgs.size()))
              {
                bool tagged = false;
                for (const std::string& a : innerArgs)
                  tagged = tagged || a.find('\x01') != std::string::npos;
                if (tagged)
                {
                  std::vector<std::string> rewritten, spelled;
                  for (const std::string& a : innerArgs)
                  {
                    rewritten.push_back(untagged(a, true));
                    spelled.push_back(untagged(a, false));
                  }
                  next += expandMacroOnce(*inner, rewritten, &spelled, useOffset);
                }
                else
                  next += expandMacroOnce(*inner, innerArgs);
                changed = true;
                i = k + 1;
                continue;
              }
            }
            auto kf = knownFunctionMacros.find(name);
            size_t close = kf != knownFunctionMacros.end() && j < result.size() && result[j] == '(' ? closingParen(result, j) : std::string::npos;
            if (close != std::string::npos)
            {
              // As in convertTokens: $1 stands for the argument text.
              size_t dollar = kf->second.find("$1");
              if (dollar == std::string::npos)
                next += withFileAndLine(kf->second, useOffset);
              else
                next += withFileAndLine(kf->second.substr(0, dollar), useOffset) + result.substr(j + 1, close - j - 1) +
                        withFileAndLine(kf->second.substr(dollar + 2), useOffset);
              changed = true;
              i = close + 1;
              continue;
            }
            auto am = argumentMacros.find(name);
            close = am != argumentMacros.end() && j < result.size() && result[j] == '(' ? closingParen(result, j) : std::string::npos;
            if (close != std::string::npos)
            {
              // The arguments are text here already: $N and #N both come from it.
              std::vector<std::string> args;
              int depth = 0;
              size_t argStart = j + 1;
              for (size_t k = j + 1; k <= close; ++k)
              {
                char c = result[k];
                if (size_t raw = rawStringEnd(result, k); raw != std::string_view::npos)
                  k = raw - 1;
                else if (c == '"' || (c == '\'' && !isDigitSeparator(result, k)))
                  for (++k; k < close && result[k] != c; ++k)
                    k += result[k] == '\\';
                else if (c == '(' || c == '[' || c == '{')
                  ++depth;
                else if ((c == ')' || c == ']' || c == '}') && depth > 0)
                  --depth;
                else if ((c == ',' && depth == 0) || k == close)
                {
                  std::string a = result.substr(argStart, k - argStart);
                  while (!a.empty() && isspace((unsigned char)a.front()))
                    a.erase(a.begin());
                  while (!a.empty() && isspace((unsigned char)a.back()))
                    a.pop_back();
                  args.push_back(a);
                  argStart = k + 1;
                }
              }
              if (args.size() == arityOf(am->second))
              {
                next += expandArguments(am->second, args, args);
                changed = true;
                i = close + 1;
                continue;
              }
            }
            // A string, name or spliced macro (lstring.h's `luaS_newliteral(L, MEMERRMSG)` gives
            // `"" MEMERRMSG`), which convertTokens writes out at the uses it sees, but not in here.
            // Another name for a function is one before a call too: luaconf.h's lua_number2str is
            // `sprintf(...)`, which override_printf.h's `#define sprintf trio_sprintf` makes trio's
            // in Lua's C++ build (the C runtime's printed 0/0 as `-nan(ind)`, trio `nan`).
            if (auto om = objectMacros.find(name); om != objectMacros.end() &&
                (!(j < result.size() && result[j] == '(') || isAliasMacro(om->first, om->second)) &&
                !(i > 0 && (result[i - 1] == '.' || (result[i - 1] == '>' && i > 1 && result[i - 2] == '-') ||
                            (result[i - 1] == ':' && i > 1 && result[i - 2] == ':'))) &&
                macroHoldsAt(om->first, useOffset))
            {
              next += om->second;
              changed = true;
              i = j;
              continue;
            }
            // The object-like names the converter rewrites (SDL_PRIs64 in a LOG_GRAPHICS_DEBUG
            // argument, FAR, ...), which the token-level rewrite does not see inside the expansion.
            if (auto km = knownMacros.find(name); km != knownMacros.end() && !(j < result.size() && result[j] == '('))
            {
              next += km->second;
              changed = true;
              i = j;
              continue;
            }
            next.append(result, i, j - i);
            i = j;
            continue;
          }
          if (result[i] == '/' && i + 1 < result.size() && (result[i + 1] == '*' || result[i + 1] == '/'))
          {
            // A comment goes as it is: no macro is used inside it, and its words are no literals
            // (DrawQueue's `/* but I'm not sure ... */` took the rest of its expansion for a character
            // literal, and the PUSH_TO_SPRITE_LIST after it stayed unexpanded).
            size_t k = result[i + 1] == '*' ? result.find("*/", i + 2) : result.find('\n', i + 2);
            k = k == std::string::npos ? result.size() : k + (result[i + 1] == '*' ? 2 : 0);
            next.append(result, i, k - i);
            i = k;
            continue;
          }
          if (result[i] == '"' || (result[i] == '\'' && !isDigitSeparator(result, i)))
          {
            // A string or character literal goes as it is: no macro is used inside it.
            size_t k = i + 1;
            if (size_t raw = rawStringEnd(result, i); raw != std::string_view::npos)
              k = raw - 1;
            while (k < result.size() && result[k] != result[i])
              k += result[k] == '\\' ? 2 : 1;
            k = std::min(k + 1, result.size());
            next.append(result, i, k - i);
            i = k;
            continue;
          }
          next += result[i++];
        }
        if (!changed)
          break;
        result = std::move(next);
      }
      return result;
    }

    // The game's classes whose names the library uses, renamed in text that never was tokens.
    std::string renameGameClasses(const std::string& s) { return renameGameClassNames(s, &counts); }
    // A macro's expansion with the game's classes renamed in the text that came from its body, not in
    // the arguments put in with their rewrites (between \x04 and \x05, which go): their names had the
    // renames of code, in their context (a std::vector there is the library's Vector already, no
    // game class Vector).
    std::string renameOutsideArguments(const std::string& s)
    {
      if (s.find('\x04') == std::string::npos)
        return this->renameGameClasses(s);
      std::string out, pending;
      int depth = 0;
      for (char c : s)
        if (c == '\x04' || c == '\x05')
        {
          if (depth == 0 && c == '\x04')
          {
            out += this->renameGameClasses(pending);
            pending.clear();
          }
          depth += c == '\x04' ? 1 : -1;
        }
        else if (depth > 0)
          out += c;
        else
          pending += c;
      return out + this->renameGameClasses(pending);
    }

    bool macroHoldsAt(const std::string& name, uint32_t offset) const
    {
      auto range = ownMacroRanges.find(name);
      return range == ownMacroRanges.end() || (range->second.first <= offset && offset < range->second.second);
    }

    // The name at token i is a member reached through `.`, or a method called through `->`
    // (`immediateContext->Map(...)`, a COM method): not the game's class of that name, so none of the
    // renames for those apply. (After `->` a name not called is a trailing return type, `-> Vector`.)
    // Whether toks[i] is the name a declaration declares rather than one it uses: after a class
    // key, `using`, `namespace` or `goto`, or after a type (a name, `>`, a pointer or reference)
    // and before what follows a declarator (`Set set(...)`: set, not Set).
    bool declaredNameAt(uint32_t i) const
    {
      if (i == 0)
        return false;
      const Token& before = toks[i - 1];
      if (before.is(Kw::class_) || before.is(Kw::struct_) || before.is(Kw::union_) || before.is(Kw::enum_) || before.is(Kw::using_) ||
          before.is(Kw::namespace_) || before.is(Kw::goto_))
        return true;
      bool afterType = before.getKind() == TokenKind::Identifier || before.is(P::Greater) || before.is(P::Shr) || before.is(P::Star) ||
                       before.is(P::Amp) || before.is(P::AndAnd) ||
                       (before.getKind() == TokenKind::Keyword &&
                        (before.is(Kw::void_) || before.is(Kw::bool_) || before.is(Kw::char_) || before.is(Kw::char8_t_) || before.is(Kw::char16_t_) ||
                         before.is(Kw::char32_t_) || before.is(Kw::wchar_t_) || before.is(Kw::short_) || before.is(Kw::int_) || before.is(Kw::long_) ||
                         before.is(Kw::float_) || before.is(Kw::double_) || before.is(Kw::signed_) || before.is(Kw::unsigned_) || before.is(Kw::auto_)));
      return afterType && i + 1 < toks.size() &&
             (toks[i + 1].is(P::LParen) || toks[i + 1].is(P::Semi) || toks[i + 1].is(P::Assign) || toks[i + 1].is(P::LBrace) ||
              toks[i + 1].is(P::LBracket) || toks[i + 1].is(P::Comma) || toks[i + 1].is(P::Colon) || toks[i + 1].is(P::RParen));
    }
    bool memberAccessAt(uint32_t i) const
    {
      return i > 0 && (toks[i - 1].is(P::Dot) || (toks[i - 1].is(P::Arrow) && i + 1 < toks.size() && toks[i + 1].is(P::LParen)));
    }

    void convertTokens()
    {
      for (uint32_t i = 0; i + 2 < toks.size(); ++i)
      {
        const Token& t = toks[i];
        if (t.getKind() == TokenKind::Identifier && !fixtureBodies.empty())
          if (const FixtureBody* body = fixtureBodyAt(i))
          {
            std::string name(spelling(i));
            bool qualified = i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon));
            // (A name before a name is a declaration's type.)
            if (!qualified && !toks[i + 1].is(P::ColonColon) && toks[i + 1].getKind() != TokenKind::Identifier && body->members.count(name) &&
                !isFixtureLocal(*body, name, i))
            {
              insert(t.offset, "this->");
              counts.add("this-> added in fixture test bodies");
            }
          }
        // C's parenthesized declarator name, lua.h's `LUA_API int (lua_gettop) (lua_State *L);`
        // (written so that a function-like macro of the same name does not expand): the name alone,
        // in a declaration outside any function body. `(f)(x)` in an expression is a call and stays.
        if (t.is(P::LParen) && i > 0 && i + 3 < toks.size() && toks[i + 1].getKind() == TokenKind::Identifier && toks[i + 2].is(P::RParen) &&
            toks[i + 3].is(P::LParen) && !functionNameAt(t.offset))
        {
          static const std::set<std::string_view> typeWords = {"void", "char", "short", "int", "long", "float", "double", "bool", "signed",
                                                               "unsigned", "const", "volatile", "auto"};
          const Token& before = toks[i - 1];
          if (before.getKind() == TokenKind::Identifier || (before.getKind() == TokenKind::Keyword && typeWords.count(spelling(i - 1))) ||
              before.is(P::Star) || before.is(P::Amp) || before.is(P::AndAnd) || before.is(P::Greater))
          {
            remove(t.offset, t.length);
            remove(toks[i + 2].offset, toks[i + 2].length);
            counts.add("parenthesized declarator names unwrapped");
            i += 2;
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "__uuidof" && i + 3 < toks.size() && toks[i + 1].is(P::LParen) &&
            toks[i + 2].getKind() == TokenKind::Identifier && toks[i + 3].is(P::RParen) && i > 2 && (toks[i - 1].is(P::EqEq) || toks[i - 1].is(P::NotEq)) &&
            toks[i - 2].getKind() == TokenKind::Identifier &&
            !(toks[i - 3].is(P::Dot) || toks[i - 3].is(P::Arrow) || toks[i - 3].is(P::ColonColon)))
        {
          // `iid == __uuidof(IUnknown)` with iid a REFIID (a pointer, above): C++ compares the ids'
          // values (guiddef.h's operator==, a memcmp), as C's IsEqualIID does.
          insert(toks[i - 2].offset, "(memcmp(");
          replace(toks[i - 1].offset, endOffset(i + 3) - toks[i - 1].offset,
                  ", &IID_" + std::string(spelling(i + 2)) + ", sizeof(IID)) " + std::string(spelling(i - 1)) + " 0)");
          counts.add("interface ids compared by value");
          i += 3;
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "__uuidof" && i + 3 < toks.size() && toks[i + 1].is(P::LParen) &&
            toks[i + 2].getKind() == TokenKind::Identifier && toks[i + 3].is(P::RParen))
        {
          // MSVC's `__uuidof(IFoo)`: the interface's IID, which the SDK declares as IID_IFoo (in C,
          // a REFIID is a pointer to it; kpp_directx.h declares the generated interfaces' IIDs).
          replace(t.offset, endOffset(i + 3) - t.offset, "&IID_" + std::string(spelling(i + 2)));
          counts.add("__uuidof to IID");
          i += 3;
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && i > 1 && (toks[i - 1].is(P::Comma) || (toks[i - 1].is(P::LParen) && (toks[i - 2].getKind() == TokenKind::Identifier || toks[i - 2].is(P::Greater)))) &&
            (toks[i + 1].is(P::Comma) || toks[i + 1].is(P::RParen)) && (spelling(i).rfind("CLSID_", 0) == 0 || spelling(i).rfind("IID_", 0) == 0))
        {
          // A class or interface id passed by name (`CoCreateInstance(CLSID_WbemLocator, ...,
          // IID_IWbemLocator, ...)`): C++ binds it to a REFCLSID or REFIID reference, which C's (and
          // so the import's, and the converted functions') is a pointer to it.
          insert(t.offset, "&");
          counts.add("class and interface ids passed by address");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && !fileAliasRenames.empty() &&
            !(i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon))))
          if (auto rename = fileAliasRenames.find(std::string(spelling(i))); rename != fileAliasRenames.end())
          {
            replace(t.offset, t.length, rename->second);
            counts.add("file-local aliases renamed");
            continue;
          }
        if (t.is(P::LParen) && i + 4 < toks.size() && toks[i + 1].getKind() == TokenKind::Identifier && isAllCapsName(spelling(i + 1)) &&
            toks[i + 2].is(P::RParen) && (toks[i + 3].is(P::Amp) || toks[i + 3].is(P::Star)) && toks[i + 4].getKind() == TokenKind::Identifier &&
            !(i > 0 && (toks[i - 1].getKind() == TokenKind::Identifier || toks[i - 1].is(P::RParen) || toks[i - 1].is(P::RBracket) ||
                        toks[i - 1].is(P::Greater) || callsWithParenthesis(i - 1))))
        {
          // `(LPWSTR)&msg`, a C type's cast of an address or a dereference: Kpp reads `(NAME) & x`
          // without knowing that NAME is a type, as a binary `&`, so the operand is parenthesized,
          // `(LPWSTR)(&msg)`, which is a cast. A name in capitals is taken for a C type (the
          // Windows typedefs), as C++ code does not write `(CONSTANT) & x`.
          uint32_t end = i + 4;
          for (;;)
          {
            if (end + 2 < toks.size() && (toks[end + 1].is(P::Dot) || toks[end + 1].is(P::Arrow) || toks[end + 1].is(P::ColonColon)) &&
                toks[end + 2].getKind() == TokenKind::Identifier)
              end += 2;
            else if (end + 1 < toks.size() && (toks[end + 1].is(P::LBracket) || toks[end + 1].is(P::LParen)))
            {
              int depth = 0;
              uint32_t k = end + 1;
              for (; k < toks.size(); ++k)
              {
                if (toks[k].is(P::LBracket) || toks[k].is(P::LParen))
                  ++depth;
                else if ((toks[k].is(P::RBracket) || toks[k].is(P::RParen)) && --depth == 0)
                  break;
              }
              if (k == toks.size())
                break;
              end = k;
            }
            else
              break;
          }
          insert(toks[i + 3].offset, "(");
          insert(endOffset(end), ")");
          counts.add("casts of an address parenthesized");
        }
        if (t.is(P::LParen) && i + 4 < toks.size() && toks[i + 1].getKind() == TokenKind::Identifier && isAllCapsName(spelling(i + 1)) &&
            toks[i + 2].is(P::RParen) && toks[i + 3].is(P::Minus) && toks[i + 4].getKind() == TokenKind::Number &&
            !(i > 0 && (toks[i - 1].getKind() == TokenKind::Identifier || toks[i - 1].is(P::RParen) || toks[i - 1].is(P::RBracket) ||
                        toks[i - 1].is(P::Greater) || callsWithParenthesis(i - 1))))
        {
          // `(HANDLE) -1`, a C type's cast of a negative number (StackWalker's invalid snapshot
          // handle), which Kpp reads as a subtraction from NAME, as above: `(HANDLE)(-1)`.
          insert(toks[i + 3].offset, "(");
          insert(endOffset(i + 4), ")");
          counts.add("casts of a negative number parenthesized");
        }
        // `X::value_type` (reference, pointer, difference_type, iterator_category) of an iterator type
        // X is IteratorTraits<X>'s, as std::iterator_traits gives them: CreatableEntityTypeIterator's
        // `using value_type = ListIterator::value_type;` with ListIterator a std::vector's const_iterator,
        // a pointer in Kpp.
        static const std::set<std::string_view> iteratorMemberTypes = {"value_type", "reference", "pointer", "difference_type", "iterator_category"};
        if (t.getKind() == TokenKind::Identifier && i + 2 < toks.size() && toks[i + 1].is(P::ColonColon) && iteratorMemberTypes.count(spelling(i + 2)) &&
            !toks[i + 3].is(P::ColonColon) && !(i > 0 && (toks[i - 1].is(P::ColonColon) || toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow))) &&
            namesIteratorType(std::string(spelling(i))))
        {
          insert(t.offset, "IteratorTraits<");
          insert(endOffset(i), ">");
          counts.add("iterator member types through IteratorTraits");
          i += 2;
          continue;
        }
        // The same of a container's iterator named through the container, `WidgetArray::iterator::reference`
        // (agui::Widget::visible_iterator, WidgetArray a Vector): IteratorTraits<WidgetArray::iterator>.
        if (t.getKind() == TokenKind::Identifier && i >= 2 && i + 2 < toks.size() && toks[i - 1].is(P::ColonColon) &&
            toks[i - 2].getKind() == TokenKind::Identifier && toks[i + 1].is(P::ColonColon) && iteratorMemberTypes.count(spelling(i + 2)) &&
            !toks[i + 3].is(P::ColonColon) && spelling(i).size() >= 8 && spelling(i).substr(spelling(i).size() - 8) == "iterator")
        {
          uint32_t start = i - 2;
          while (start >= 2 && toks[start - 1].is(P::ColonColon) && toks[start - 2].getKind() == TokenKind::Identifier)
            start -= 2;
          if (!(start > 0 && (toks[start - 1].is(P::ColonColon) || toks[start - 1].is(P::Greater) || toks[start - 1].is(P::Dot) || toks[start - 1].is(P::Arrow))))
          {
            insert(toks[start].offset, "IteratorTraits<");
            insert(endOffset(i), ">");
            counts.add("iterator member types through IteratorTraits");
            i += 2;
            continue;
          }
        }
        // A string macro or a macro that is a qualified name is its text wherever it is used: macros
        // know no scopes (the constant the conversion makes of UTF8_BULLET is inside StringUtil's
        // namespace), and only literals concatenate (`"\n" UTF8_BULLET " "`).
        if (t.getKind() == TokenKind::Identifier && !objectMacros.empty() &&
            !(i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon))))
          if (auto macro = objectMacros.find(std::string(spelling(i))); macro != objectMacros.end() && macroHoldsAt(macro->first, t.offset))
          {
            replace(t.offset, t.length, macro->second);
            counts.add(isStringLiterals(macro->second)                                       ? "string macros written out"
                       : isQualifiedName(macro->second) || isAliasMacro(macro->first, macro->second) ? "name macros written out"
                                                                                                      : "spliced macros written out");
            continue;
          }
        if (t.getKind() == TokenKind::Identifier && headerAliases && !headerAliases->empty() &&
            !(i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon))))
          if (auto alias = headerAliases->find(std::string(spelling(i))); alias != headerAliases->end())
          {
            replace(t.offset, t.length, alias->second);
            counts.add("C header alias macros expanded");
            continue;
          }
        if (t.getKind() == TokenKind::Identifier && toks[i + 1].is(P::LParen) && !localMacros.empty())
        {
          const LocalMacro* lm = localMacroAt(std::string(spelling(i)), t.offset);
          if (lm)
          {
            // The arguments: token ranges between top-level commas, up to the matching ')'.
            std::vector<std::pair<uint32_t, uint32_t>> args;
            uint32_t j = i + 2, argStart = j;
            int depth = 0;
            bool closed = false;
            for (; j < toks.size(); ++j)
            {
              const Token& tj = toks[j];
              if (tj.is(P::LParen) || tj.is(P::LBracket) || tj.is(P::LBrace))
                ++depth;
              else if (tj.is(P::RParen) || tj.is(P::RBracket) || tj.is(P::RBrace))
              {
                if (depth == 0)
                {
                  closed = tj.is(P::RParen);
                  break;
                }
                --depth;
              }
              else if (tj.is(P::Comma) && depth == 0)
              {
                args.push_back({argStart, j});
                argStart = j + 1;
              }
              else if (tj.getKind() == TokenKind::End)
                break; // (an argument may hold statements: lvm.c's `vmcase(OP_MOVE, setobjs2s(L, ra, RB(i));)`)
            }
            if (closed)
            {
              if (j > argStart || !args.empty())
                args.push_back({argStart, j});
              if (argumentsFit(*lm, args.size()))
              {
                std::vector<std::string> argText;
                for (auto [a0, a1] : args)
                  argText.push_back(a1 > a0 ? std::string(text.substr(toks[a0].offset, endOffset(a1 - 1) - toks[a0].offset)) : std::string());
                if (uint32_t close = leftOutTests && !leftOutTests->empty() && toks[j + 1].is(P::LBrace) ? closingBrace(j + 1) : 0)
                {
                  // A test's name is its macro's last argument (TEST(Name), TEST_FIXTURE(Fixture, Name),
                  // TEST_PARAMETRISED_FIXTURE(Fixture, Parameter, Name)), its suite the innermost SUITE
                  // block around it.
                  std::string name(spelling(i));
                  if (name == "SUITE" && argText.size() == 1)
                  {
                    suiteBlocks.push_back({argText[0], j + 1, close});
                    suitesConverted->insert(argText[0]);
                  }
                  else if ((name == "TEST" || name.rfind("TEST_", 0) == 0) && !argText.empty())
                  {
                    std::string suite;
                    for (const SuiteBlock& s : suiteBlocks)
                      if (s.open < i && i < s.close)
                        suite = s.name;
                    if (auto left = leftOutTests->find(suite + "/" + argText.back()); left != leftOutTests->end())
                    {
                      replace(t.offset, endOffset(close) - t.offset, "// " + left->first + " is left out (overrides/left-out-tests.txt): " + left->second);
                      leftOutTestsFound->insert(left->first);
                      counts.add("tests left out");
                      i = close;
                      continue;
                    }
                  }
                }
                // (The fixture class a TEST_FIXTURE's expansion names, for its body's this->.)
                if (j + 1 < toks.size() && toks[j + 1].is(P::LBrace))
                  noteFixtureBody(expandMacro(*lm, argText, t.offset), j + 1);
                LocalMacroUse use{t.offset, endOffset(j), {}, lm};
                for (auto [a0, a1] : args)
                  use.args.push_back(a1 > a0 ? std::pair(toks[a0].offset, endOffset(a1 - 1)) : std::pair(toks[a0].offset, toks[a0].offset));
                localMacroUses.push_back(std::move(use));
                counts.add(lm->from == 0 && lm->to == UINT32_MAX && headerMacros && headerMacros->count(std::string(spelling(i))) ? "C header macro expanded" : "local macro expanded");
                ++i; // on to the arguments: run() expands the use
                continue;
              }
            }
          }
        }
        if (t.getKind() == TokenKind::Number)
        {
          // A std::chrono literal (`10ms`, `1.5s`): the library's duration of that count, in double
          // for a fractional one.
          static const std::tuple<const char*, const char*, const char*> suffixes[] = {
            {"ns", "Nanoseconds", "Duration<double, Nano>"}, {"us", "Microseconds", "Duration<double, Micro>"},
            {"ms", "Milliseconds", "Duration<double, Milli>"}, {"min", "Minutes", "Duration<double, Ratio<60>>"},
            {"s", "Seconds", "Duration<double>"}, {"h", "Hours", "Duration<double, Ratio<3600>>"}};
          std::string_view number = spelling(i);
          size_t digits = 0;
          while (digits < number.size() && (isdigit((unsigned char)number[digits]) || number[digits] == '.' || number[digits] == '\''))
            ++digits;
          std::string_view suffix = number.substr(digits);
          bool replaced = false;
          for (const auto& [name, integral, floating] : suffixes)
            if (digits > 0 && suffix == name)
            {
              std::string count;
              for (char c : number.substr(0, digits))
                if (c != '\'')
                  count += c;
              bool fractional = count.find('.') != std::string::npos;
              replace(t.offset, t.length, std::string(fractional ? floating : integral) + "(" + count + ")");
              counts.add("chrono literal");
              replaced = true;
              break;
            }
          if (replaced)
            continue;
        }
        if (t.is(Kw::using_) && toks[i + 1].is(Kw::namespace_) && spelling(i + 2) == "std")
        {
          // `using namespace std::chrono_literals;` (or std::chrono, std::literals): the literals
          // are rewritten where they are written and the library's names are global.
          uint32_t j = i + 2;
          std::string path;
          for (; j < toks.size() && !toks[j].is(P::Semi); ++j)
            path += std::string(spelling(j));
          if (j < toks.size() && (path == "std::chrono_literals" || path == "std::chrono" || path == "std::literals" ||
                                  path == "std::literals::chrono_literals" || path == "std::chrono::literals"))
          {
            remove(t.offset, lineEndAfter(endOffset(j)) - t.offset);
            counts.add("using namespace for chrono removed");
            i = j;
            continue;
          }
        }
        if (t.is(Kw::using_) && i + 4 < toks.size() && spelling(i + 1) == "std" && toks[i + 2].is(P::ColonColon) &&
            toks[i + 3].getKind() == TokenKind::Identifier && toks[i + 4].is(P::Semi))
        {
          // `using std::ostream;`: the library's names are global, so the declaration goes and the
          // file's later uses of the name are the library's (OutStream).
          std::string name(spelling(i + 3));
          if (auto known = stdNames.find(name); known != stdNames.end())
          {
            stdUsingNames[name] = known->second;
            remove(t.offset, lineEndAfter(endOffset(i + 4)) - t.offset);
            counts.add("using-declarations of std names removed");
            i += 4;
            continue;
          }
          todo(t.offset, "using std::" + name + ": a std name the converter has no Kpp name for");
        }
        if (t.getKind() == TokenKind::Identifier && !stdUsingNames.empty() &&
            !(i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon))))
          if (auto used = stdUsingNames.find(std::string(spelling(i))); used != stdUsingNames.end())
          {
            replace(t.offset, t.length, used->second);
            counts.add("std names of using-declarations renamed");
            continue;
          }
        if (t.is(Kw::using_) && toks[i + 1].is(Kw::namespace_) && !directiveRanges.empty())
        {
          auto removed = std::find_if(directiveRanges.begin(), directiveRanges.end(), [&](const DirectiveRange& r) { return r.begin == t.offset && r.removed; });
          if (removed != directiveRanges.end())
          {
            remove(t.offset, removed->lineEnd - t.offset);
            counts.add("file-scope using-directives replaced by qualified names");
            while (i + 1 < toks.size() && !toks[i].is(P::Semi))
              ++i;
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier && !directiveRanges.empty() && !(i > 0 && (toks[i - 1].is(P::ColonColon) || toks[i - 1].is(P::Arrow))) &&
            !memberAccessAt(i))
        {
          // A name a using-directive finds: qualified where the directive goes, and where a global
          // class, alias or variable of the name would be found beside it, the library's or the
          // program's (Lua's `Token` beside MathExpressions' in its tests: C++ never had both in one
          // translation unit).
          std::string name(spelling(i));
          if (const DirectiveRange* via = directiveFinding(name, t.offset);
              via && (via->removed || (libraryTypes && libraryTypes->count(name)) || globalNamed(name)) && !declaredCloser(name, t.offset) &&
              !declaredNameAt(i))
          {
            insert(t.offset, via->key);
            counts.add(via->removed ? "names of a removed using-directive qualified" : "names clashing with a global one qualified by their namespace");
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier &&
            (spelling(i) == "__FILE__" || spelling(i) == "__LINE__" || ((spelling(i) == "__FUNCTION__" || spelling(i) == "__func__") && functionNameAt(t.offset))))
        {
          // The preprocessor's file name and line: the literal values, since Kpp has no preprocessor.
          replace(t.offset, endOffset(i) - t.offset, withFileAndLine(std::string(spelling(i)), t.offset));
          counts.add("__FILE__, __LINE__, __FUNCTION__ replaced");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Vector" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's own 2D `Vector` class is Vector2D in Kpp, where Vector is the library's std::vector.
          replace(t.offset, endOffset(i) - t.offset, "Vector2D");
          counts.add("Vector renamed to Vector2D");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Map" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's `class Map` is GameMap in Kpp, where Map<K,V> is the library's std::map.
          replace(t.offset, endOffset(i) - t.offset, "GameMap");
          counts.add("Map renamed to GameMap");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Optional" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's sentinel-based `Optional<T, empty>` is GameOptional in Kpp, where Optional<T> is the library's std::optional.
          replace(t.offset, endOffset(i) - t.offset, "GameOptional");
          counts.add("Optional renamed to GameOptional");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Pair" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's `Pair<K,V>` is GamePair in Kpp, where Pair<A,B> is the library's std::pair.
          replace(t.offset, endOffset(i) - t.offset, "GamePair");
          counts.add("Pair renamed to GamePair");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "RuntimeError" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's `class RuntimeError : public std::runtime_error` is GameRuntimeError in Kpp,
          // where RuntimeError is the library's std::runtime_error.
          replace(t.offset, endOffset(i) - t.offset, "GameRuntimeError");
          counts.add("RuntimeError renamed to GameRuntimeError");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && (spelling(i) == "Clock" || spelling(i) == "TimePoint"))
        {
          // `using Clock = std::chrono::steady_clock;` and `using TimePoint = Clock::time_point;` at
          // global scope (three Net headers), where the library declares Clock and TimePoint: those
          // are GameClock and GameTimePoint, at their declarations and wherever the unqualified (or
          // `::`-qualified) name finds them. A class's own Clock (MainLoop::Clock) keeps its name.
          bool qualified = i >= 2 && toks[i - 1].is(P::ColonColon) &&
                           (toks[i - 2].getKind() == TokenKind::Identifier || toks[i - 2].is(P::Greater) || toks[i - 2].is(P::Shr) ||
                            toks[i - 2].is(P::RParen));
          bool global = i >= 1 && toks[i - 1].is(P::ColonColon) && !qualified;
          std::string name(spelling(i));
          if (!qualified && (global || !shadowedAt(name, t.offset)))
          {
            replace(t.offset, endOffset(i) - t.offset, "Game" + name);
            counts.add(name + " renamed to Game" + name);
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier && i >= 2 && toks[i - 1].is(P::Tilde) && (toks[i - 2].is(P::Dot) || toks[i - 2].is(P::Arrow)) &&
            i + 1 < toks.size() && (toks[i + 1].is(P::LParen) || toks[i + 1].is(P::Less)))
        {
          // `value.~vector()`: the destructor of a std class, by the class's Kpp name.
          auto it = stdNames.find(std::string(spelling(i)));
          if (it != stdNames.end() && it->second.find("::") == std::string::npos && isupper((unsigned char)it->second[0]))
          {
            replace(t.offset, endOffset(i) - t.offset, it->second);
            counts.add("std renamed");
          }
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "std" && i + 5 < toks.size() && toks[i + 1].is(P::ColonColon) &&
            spelling(i + 2) == "pair" && toks[i + 3].is(P::Less) && toks[i + 4].is(Kw::const_))
        {
          // `std::pair<const K, V>`, a map's entry as C++ spells it (AsteroidCollectorNavmesh's
          // `for (const std::pair<const ChunkPosition, std::unique_ptr<...>>& entry : records)`): a
          // Kpp Map's entries are Pair<K, V> (docs/stdlib/map.md), so the key's const goes. A const
          // that belongs to a pointer's or a reference's target (`std::pair<const T*, float>`) stays.
          uint32_t k = i + 5;
          for (int depth = 0; k < toks.size(); ++k)
          {
            if (toks[k].is(P::Less) || toks[k].is(P::LParen))
              ++depth;
            else if (toks[k].is(P::Greater) || toks[k].is(P::RParen))
              --depth;
            else if (toks[k].is(P::Shr))
              depth -= 2;
            if (depth < 0 || (depth == 0 && (toks[k].is(P::Comma) || toks[k].is(P::Semi))))
              break;
          }
          if (k < toks.size() && toks[k].is(P::Comma) && !(toks[k - 1].is(P::Star) || toks[k - 1].is(P::Amp) || toks[k - 1].is(P::AndAnd)))
          {
            remove(toks[i + 4].offset, toks[i + 5].offset - toks[i + 4].offset);
            counts.add("const keys of map entries dropped");
          }
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "std" && toks[i + 1].is(P::ColonColon) &&
            toks[i + 2].getKind() == TokenKind::Identifier)
        {
          std::string name(spelling(i + 2));
          if (variants && !variants->all.empty() && (name == "get" || name == "holds_alternative" || name == "get_if" || name == "visit"))
            recordVariantUse(name, i);
          if (name == "source_location" && i + 6 < toks.size() && toks[i + 3].is(P::ColonColon) && spelling(i + 4) == "current" &&
              toks[i + 5].is(P::LParen) && toks[i + 6].is(P::RParen))
          {
            // `std::source_location loc = std::source_location::current()` as a default argument:
            // Kpp's caller location, which the compiler fills at each call (design.md section 7).
            replace(t.offset, endOffset(i + 6) - t.offset, "caller");
            counts.add("source_location::current() to caller");
            i += 6;
            continue;
          }
          if ((name == "views" || name == "ranges") && i + 4 < toks.size() && toks[i + 3].is(P::ColonColon) && toks[i + 4].getKind() == TokenKind::Identifier)
          {
            // std::views and std::ranges::subrange are the library's views (Views.kpp, docs/stdlib/views.md);
            // std::ranges::find and find_if over a whole range its algorithms' range forms (algorithms.md).
            static const std::map<std::string, std::string> viewNames = {
              {"views::iota", "iotaView"}, {"views::transform", "transformView"}, {"views::reverse", "reverseView"},
              {"ranges::subrange", "Subrange"}, {"ranges::find", "::find"}, {"ranges::find_if", "::findIf"}};
            auto v = viewNames.find(name + "::" + std::string(spelling(i + 4)));
            if (v != viewNames.end())
            {
              bool qualify = v->second[0] != ':' && shadowedAt(v->second, t.offset);
              replace(t.offset, endOffset(i + 4) - t.offset, qualify ? "::" + v->second : v->second);
              counts.add(name == "views" || v->second == "Subrange" ? "std::views renamed" : "std::ranges algorithms renamed");
              i += 4;
              continue;
            }
            counts.add("std::" + name + "::" + std::string(spelling(i + 4)) + " (no mapping)");
          }
          if (name == "locale" && i + 13 < toks.size() && toks[i + 3].is(P::ColonColon) && spelling(i + 4) == "global" && toks[i + 5].is(P::LParen) &&
              spelling(i + 6) == "std" && spelling(i + 8) == "locale" && spelling(i + 10) == "classic" && toks[i + 11].is(P::LParen) &&
              toks[i + 12].is(P::RParen) && toks[i + 13].is(P::RParen) && i + 14 < toks.size() && toks[i + 14].is(P::Semi))
          {
            // `std::locale::global(std::locale::classic());` (Main.cpp): the library's text conversion is
            // the classic ("C") locale's always, so making it the global one changes nothing.
            replace(t.offset, endOffset(i + 14) - t.offset, "/* std::locale::global(std::locale::classic()): Kpp converts text as the classic locale */");
            counts.add("std::locale::global(classic) removed");
            i += 14;
            continue;
          }
          if ((name == "in_place_type_t" || name == "in_place_type") && i > 0 && toks[i - 1].is(P::LParen) && i + 3 < toks.size() && toks[i + 3].is(P::Less))
          {
            // `f(std::in_place_type_t<T>(), a, b)`, a std::variant built in place as its alternative
            // T (DrawCommandBatch's batches.emplace_back): the alternative's own constructor,
            // `f(T(a, b))`, which sets the typed union's alternative from the value.
            uint32_t j = i + 4;
            int depth = 0;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::Less))
                ++depth;
              else if (toks[j].is(P::Greater) && depth-- == 0)
                break;
              else if (toks[j].is(P::Semi) || toks[j].getKind() == TokenKind::End)
                break;
            }
            uint32_t after = j + 1;
            if (j < toks.size() && toks[j].is(P::Greater) && name == "in_place_type_t" && after + 1 < toks.size() && toks[after].is(P::LParen) && toks[after + 1].is(P::RParen))
              after += 2;
            uint32_t close = closingParenToken(i - 1);
            if (j < toks.size() && toks[j].is(P::Greater) && close != UINT32_MAX && after <= close && (toks[after].is(P::Comma) || after == close))
            {
              remove(t.offset, endOffset(i + 3) - t.offset); // `std::in_place_type_t<`
              if (after == close)
                replace(toks[j].offset, toks[close].offset - toks[j].offset, "()");
              else
              {
                uint32_t next = after + 1;
                replace(toks[j].offset, toks[next].offset - toks[j].offset, "(");
                insert(toks[close].offset, ")");
              }
              counts.add("std::in_place_type to the alternative's constructor");
              i = j;
              continue;
            }
          }
          if ((name == "memory_order" && i + 4 < toks.size() && toks[i + 3].is(P::ColonColon) && toks[i + 4].getKind() == TokenKind::Identifier) ||
              name.rfind("memory_order_", 0) == 0)
          {
            // A memory order passed to an atomic operation (`value.load(std::memory_order::relaxed)`,
            // TimeStatistics' AtomicNanoseconds): the library's Atomic takes none, every operation is
            // sequentially consistent (docs/stdlib/atomic.md), which is at least as strong. The
            // argument goes with its comma; elsewhere a memory order stays and fails to compile.
            uint32_t last = name == "memory_order" ? i + 4 : i + 2;
            bool before = i > 0 && (toks[i - 1].is(P::Comma) || toks[i - 1].is(P::LParen));
            bool after = last + 1 < toks.size() && (toks[last + 1].is(P::Comma) || toks[last + 1].is(P::RParen));
            if (before && after)
            {
              if (toks[i - 1].is(P::Comma))
                remove(toks[i - 1].offset, endOffset(last) - toks[i - 1].offset);
              else if (toks[last + 1].is(P::Comma))
                remove(t.offset, toks[last + 2].offset - t.offset);
              else
                remove(t.offset, endOffset(last) - t.offset);
              counts.add("memory order arguments dropped");
              i = last;
              continue;
            }
          }
          if (name == "thread" && i + 4 < toks.size() && toks[i + 3].is(P::ColonColon) && spelling(i + 4) == "hardware_concurrency")
          {
            // A static member whose library name differs: std::thread::hardware_concurrency().
            replace(t.offset, endOffset(i + 4) - t.offset, shadowedAt("Thread", t.offset) ? "::Thread::hardwareConcurrency" : "Thread::hardwareConcurrency");
            counts.add("std static member renamed");
            i += 4;
            continue;
          }
          if (name == "chrono" && i + 4 < toks.size() && toks[i + 3].is(P::ColonColon) && toks[i + 4].getKind() == TokenKind::Identifier)
          {
            // std::chrono's names are the library's time types (Time.kpp, docs/stdlib/time.md).
            static const std::map<std::string_view, std::string> chronoNames = {
              {"nanoseconds", "Nanoseconds"}, {"microseconds", "Microseconds"}, {"milliseconds", "Milliseconds"},
              {"seconds", "Seconds"}, {"minutes", "Minutes"}, {"hours", "Hours"}, {"days", "Days"},
              {"duration", "Duration"}, {"time_point", "TimePoint"}, {"steady_clock", "SteadyClock"},
              {"system_clock", "SystemClock"}, {"high_resolution_clock", "HighResolutionClock"},
              {"duration_cast", "durationCast"}, {"time_point_cast", "timePointCast"}, {"floor", "floor"}, {"ceil", "ceil"}};
            auto c = chronoNames.find(spelling(i + 4));
            if (c != chronoNames.end())
            {
              std::string chronoTo = c->second;
              if (shadowedAt(chronoTo, t.offset))
                chronoTo = "::" + chronoTo;
              replace(t.offset, endOffset(i + 4) - t.offset, chronoTo);
              counts.add("std::chrono renamed");
              i += 4;
              continue;
            }
            counts.add("std::chrono::" + std::string(spelling(i + 4)) + " (no mapping)");
          }
          auto it = stdNames.find(name);
          std::string to = it == stdNames.end() ? name : it->second;
          static const std::map<std::string, size_t> containerArguments = {
            {"vector", 1}, {"deque", 1}, {"list", 1}, {"forward_list", 1}, {"set", 2}, {"multiset", 2}, {"map", 3}, {"multimap", 3},
            {"unordered_set", 3}, {"unordered_multiset", 3}, {"unordered_map", 4}, {"unordered_multimap", 4}};
          auto ca = containerArguments.find(name);
          if (ca != containerArguments.end() && i + 3 < toks.size() && toks[i + 3].is(P::Less))
          {
            // `std::vector<T, Allocate>`: the library's containers have no allocator parameter, so the
            // argument goes (CommonUtil's eraseIf overloads); a template parameter used only there
            // goes from its template header too (dropAllocatorParameters).
            std::vector<std::pair<uint32_t, uint32_t>> args; // token ranges of the arguments
            uint32_t j = i + 4, argStart = j;
            uint32_t close = UINT32_MAX; // offset of the `>` that closes the container's arguments
            int angle = 0, paren = 0;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++paren;
              else if (toks[j].is(P::RParen))
              {
                if (--paren < 0)
                  break;
              }
              else if (paren > 0)
                continue;
              else if (toks[j].is(P::Less))
                ++angle;
              else if (toks[j].is(P::Greater) || toks[j].is(P::Shr))
              {
                // `>>` one level in closes the last argument's own arguments and then the container's.
                if (angle == 0 || (angle == 1 && toks[j].is(P::Shr)))
                {
                  close = toks[j].offset + angle;
                  break;
                }
                angle -= toks[j].is(P::Shr) ? 2 : 1;
              }
              else if (toks[j].is(P::Comma) && angle == 0)
              {
                args.push_back({argStart, j});
                argStart = j + 1;
              }
              else if (toks[j].is(P::Semi) || toks[j].is(P::LBrace) || toks[j].getKind() == TokenKind::End)
                break;
            }
            if (close != UINT32_MAX)
            {
              args.push_back({argStart, j});
              if (args.size() > ca->second)
              {
                // From the end of the last argument kept to the closing `>` (the first of a `>>`).
                uint32_t from = endOffset(args[ca->second - 1].second - 1);
                remove(from, close - from);
                for (size_t a = ca->second; a < args.size(); ++a)
                  for (uint32_t k = args[a].first; k < args[a].second; ++k)
                    if (toks[k].getKind() == TokenKind::Identifier)
                      droppedAllocators[toks[k].offset] = std::string(spelling(k));
                counts.add("allocator arguments dropped");
              }
            }
          }
          if ((name == "unique_ptr" || name == "make_unique" || name == "make_unique_for_overwrite") && i + 3 < toks.size() && toks[i + 3].is(P::Less))
          {
            // `unique_ptr<T[]>`, `make_unique<T[]>(n)` and `make_unique_for_overwrite<T[]>(n)`: the
            // array form is its own class.
            uint32_t j = i + 4;
            int depth = 0;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::Less))
                ++depth;
              else if (toks[j].is(P::Greater) || toks[j].is(P::Shr))
              {
                if (depth == 0)
                  break;
                depth -= toks[j].is(P::Shr) ? 2 : 1;
                if (depth < 0)
                  break;
              }
              else if (toks[j].is(P::Semi) || toks[j].getKind() == TokenKind::End)
                break;
            }
            if (j < toks.size() && j >= i + 6 && toks[j - 1].is(P::RBracket) && toks[j - 2].is(P::LBracket))
            {
              to = name == "unique_ptr" ? "UniqueArray" : name == "make_unique" ? "makeUniqueArray" : "makeUniqueArrayForOverwrite";
              remove(toks[j - 2].offset, endOffset(j - 1) - toks[j - 2].offset);
              counts.add("unique_ptr<T[]> to UniqueArray");
            }
          }
          auto variable = stdNames.find(name + "_v");
          auto alias = stdNames.find(name + "_t");
          if (alias != stdNames.end() && i + 3 < toks.size() && toks[i + 3].is(P::Less))
          {
            // `std::make_unsigned<T>::type`: the trait's type, which is the library's MakeUnsigned<T>
            // (what std::make_unsigned_t<T> converts to).
            uint32_t j = i + 4;
            for (int depth = 0; j < toks.size(); ++j)
            {
              if (toks[j].is(P::Less))
                ++depth;
              else if (toks[j].is(P::Greater) || toks[j].is(P::Shr))
              {
                if (depth == 0)
                  break;
                depth -= toks[j].is(P::Shr) ? 2 : 1;
                if (depth < 0)
                  break;
              }
              else if (toks[j].is(P::Semi) || toks[j].getKind() == TokenKind::End)
                break;
            }
            if (j + 2 < toks.size() && toks[j + 1].is(P::ColonColon) && spelling(j + 2) == "type")
            {
              to = alias->second;
              it = alias;
              remove(toks[j + 1].offset, endOffset(j + 2) - toks[j + 1].offset);
              counts.add("trait class to its type");
            }
          }
          if (name.compare(0, 3, "is_") == 0 && variable != stdNames.end() && i + 3 < toks.size() && toks[i + 3].is(P::Less))
          {
            // `std::is_member_function_pointer<T>()` and `std::is_const<T>::value`: the trait's
            // value, which is the compiler's isMemberFunctionPointer<T>; Kpp has no class form.
            uint32_t j = i + 4;
            int depth = 0;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::Less))
                ++depth;
              else if (toks[j].is(P::Greater) || toks[j].is(P::Shr))
              {
                if (depth == 0)
                  break;
                depth -= toks[j].is(P::Shr) ? 2 : 1;
                if (depth < 0)
                  break;
              }
              else if (toks[j].is(P::Semi) || toks[j].getKind() == TokenKind::End)
                break;
            }
            bool call = j + 2 < toks.size() && ((toks[j + 1].is(P::LParen) && toks[j + 2].is(P::RParen)) ||
                                                (toks[j + 1].is(P::LBrace) && toks[j + 2].is(P::RBrace)));
            bool value = j + 2 < toks.size() && toks[j + 1].is(P::ColonColon) && spelling(j + 2) == "value";
            if (call || value)
            {
              to = variable->second;
              it = variable;
              remove(toks[j + 1].offset, endOffset(j + 2) - toks[j + 1].offset);
              counts.add("trait class to its value");
            }
          }
          counts.add(it == stdNames.end() ? "std::" + name + " (no mapping)" : "std renamed");
          if (to.find("::") == std::string::npos && shadowedAt(to, t.offset))
          {
            // `std::string` inside a class or namespace with its own String, `std::next(source)` in
            // a class whose base has a `next` member (Loader through UpdatableEntity's list hook).
            to = "::" + to;
            counts.add("std name qualified as global");
          }
          replace(t.offset, endOffset(i + 2) - t.offset, to);
          if (it != stdNames.end() && i + 4 < toks.size() && toks[i + 3].is(P::ColonColon) && spelling(i + 4) == name &&
              isIdentChar(to.back()))
          {
            // The class's constructors by its name: `using std::string::string;` inherits them
            // (TestFlatMap's MoveOnlyString), `using String::String;` in Kpp.
            replace(toks[i + 4].offset, toks[i + 4].length, to.substr(to.rfind(':') == std::string::npos ? 0 : to.rfind(':') + 1));
            counts.add("std constructor name renamed");
            i += 4;
            continue;
          }
          i += 2;
          continue;
        }
        if (t.is(Kw::typename_) && toks[i + 1].getKind() == TokenKind::Identifier && toks[i + 2].is(P::ColonColon))
        {
          remove(t.offset, toks[i + 1].offset - t.offset); // `typename T::x` -> `T::x`
          counts.add("typename dropped");
          continue;
        }
        if (t.is(Kw::template_) && i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon)))
        {
          remove(t.offset, toks[i + 1].offset - t.offset); // `x.template f<T>()` -> `x.f<T>()`
          counts.add("template keyword dropped");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && toks[i + 1].is(P::LParen))
        {
          // Library functions Factorio wraps that the C runtime provides as well.
          static const std::map<std::string_view, std::string> renames = {
            {"trio_vsnprintf", "vsnprintf"}, {"trio_snprintf", "snprintf"}, {"trio_sprintf", "sprintf"}, {"trio_vsprintf", "vsprintf"}};
          auto rn = renames.find(spelling(i));
          if (rn != renames.end())
          {
            replace(t.offset, t.length, rn->second);
            counts.add("function renamed");
            continue;
          }
          // Members of std types whose library spelling differs and that no game class declares:
          // std::type_info's hash_code is TypeInfo's hashCode. (Optional has std's value_or: the
          // game's OptionalFundamental declares one of its own.)
          static const std::map<std::string_view, std::string> memberRenames = {{"hash_code", "hashCode"}};
          auto mr = memberRenames.find(spelling(i));
          if (mr != memberRenames.end() && i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow)))
          {
            replace(t.offset, t.length, mr->second);
            counts.add("std member renamed");
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier && i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow)))
        {
          // The C headers' macros that name a member path (in_addr's `#define s_addr S_un.S_addr`,
          // in6_addr's s6_addr, whose `_S6_un._S6_u8` are macros too): the import has the structs' own
          // members, so a use gets the path.
          static const std::map<std::string_view, std::string> memberMacros = {{"s_addr", "S_un.S_addr"}, {"s6_addr", "u.Byte"}};
          if (auto mm = memberMacros.find(spelling(i)); mm != memberMacros.end())
          {
            replace(t.offset, t.length, mm->second);
            counts.add("C member macros expanded");
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier)
        {
          std::string name(spelling(i));
          auto km = knownMacros.find(name);
          if (km != knownMacros.end())
          {
            if (km->second.empty() && toks[i + 1].is(P::LParen) && (name == "FORMAT_ATTR" || name == "TAG_FORMAT_ATTR"))
            {
              // `FORMAT_ATTR(4, 5)` after a parameter list: the attribute macro and its arguments go.
              int depth = 0;
              uint32_t j = i + 1;
              for (; j < toks.size(); ++j)
              {
                if (toks[j].is(P::LParen))
                  ++depth;
                else if (toks[j].is(P::RParen) && --depth == 0)
                  break;
              }
              if (j < toks.size())
              {
                remove(t.offset, endOffset(j) - t.offset);
                counts.add("macro use rewritten");
                i = j;
                continue;
              }
            }
            replace(t.offset, t.length, km->second);
            counts.add("macro use rewritten");
            continue;
          }
          if (name == "__try")
          {
            // Structured exception handling: Kpp's exceptions are SEH underneath, so `try` catches
            // what `__try` did; the filter expression is lost (a todo).
            replace(t.offset, t.length, "try");
            counts.add("SEH __try to try");
            continue;
          }
          if (name == "__except" && toks[i + 1].is(P::LParen))
          {
            int depth = 0;
            uint32_t j = i + 1;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && --depth == 0)
                break;
            }
            if (j < toks.size())
            {
              todo(t.offset, "SEH __except filter dropped: " + std::string(text.substr(toks[i + 1].offset, endOffset(j) - toks[i + 1].offset)));
              replace(t.offset, endOffset(j) - t.offset, "catch (...)");
              counts.add("SEH __except to catch (...)");
              i = j;
              continue;
            }
          }
          // Util/Enums.hpp: ENUM(Name, Type, A, B, ...) is a class holding an `enum class Enum` with
          // conversions, validation, str() and parsing from text; ENUM_IMPL is that class's body and
          // ENUM_IMPL_NOSTRING the body without the text part. Expanded as the macros expand.
          if ((name == "ENUM" || name == "ENUM_IMPL" || name == "ENUM_IMPL_NOSTRING") && toks[i + 1].is(P::LParen))
          {
            std::vector<std::string> args;
            uint32_t j = i + 2;
            int depth = 0;
            bool simple = true;
            uint32_t argStart = j;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && depth-- == 0)
                break;
              else if (toks[j].is(P::Comma) && depth == 0)
              {
                simple = simple && j == argStart + 1;
                args.push_back(std::string(spelling(argStart)));
                argStart = j + 1;
              }
            }
            if (j < toks.size() && j > argStart)
            {
              simple = simple && j == argStart + 1;
              args.push_back(std::string(spelling(argStart)));
            }
            // Name, Type and the enumerators, each a single name (types such as `uint8_t` are one).
            if (j < toks.size() && simple && args.size() >= 3)
            {
              std::vector<std::string> values(args.begin() + 2, args.end());
              std::string body = enumClassBody(args[0], args[1], values, name != "ENUM_IMPL_NOSTRING");
              replace(t.offset, endOffset(j) - t.offset, name == "ENUM" ? "class " + args[0] + "\n{\n" + body + "}" : body);
              counts.add(name + " expanded");
              i = j;
              continue;
            }
            todo(t.offset, name + " with arguments that are not single names: left as written");
          }
          // BITFIELD(Name, member1, member2, ...) -> class Name : public BitFieldBase { enum class Enum { members }; using enum Enum; ... }
          if (name == "BITFIELD" && toks[i + 1].is(P::LParen))
          {
            int depth = 0;
            uint32_t j = i + 1;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && --depth == 0)
                break;
            }
            if (j < toks.size())
            {
              // Collect all arguments: first is the Name, rest are enum members.
              std::vector<std::string> args;
              uint32_t argStart = i + 2;
              int d = 0;
              for (uint32_t k = argStart; k <= j; ++k)
              {
                if (toks[k].is(P::LParen) || toks[k].is(P::LBracket) || toks[k].is(P::LBrace))
                  ++d;
                else if (toks[k].is(P::RParen) || toks[k].is(P::RBracket) || toks[k].is(P::RBrace))
                {
                  if (d > 0) { --d; continue; }
                  // closing RParen; after a trailing comma (`Unhandled, )`) the last argument is
                  // empty: k is argStart, and endOffset(k - 1) would come before it.
                  if (k == argStart)
                    break;
                  std::string a(text.substr(toks[argStart].offset, endOffset(k - 1) - toks[argStart].offset));
                  while (!a.empty() && isspace((unsigned char)a.front())) a.erase(a.begin());
                  while (!a.empty() && isspace((unsigned char)a.back())) a.pop_back();
                  if (!a.empty())
                    args.push_back(a);
                  break;
                }
                else if (toks[k].is(P::Comma) && d == 0)
                {
                  std::string a(text.substr(toks[argStart].offset, toks[k].offset - toks[argStart].offset));
                  while (!a.empty() && isspace((unsigned char)a.front())) a.erase(a.begin());
                  while (!a.empty() && isspace((unsigned char)a.back())) a.pop_back();
                  args.push_back(a);
                  argStart = k + 1;
                }
              }
              if (args.size() >= 2)
              {
                std::string bfName = args[0];
                std::string members;
                for (size_t m = 1; m < args.size(); ++m)
                {
                  if (m > 1) members += ", ";
                  members += args[m];
                }
                std::string expanded =
                  "class " + bfName + " : public BitFieldBase\n"
                  "{\n"
                  "public:\n"
                  "  enum class Enum\n"
                  "  {\n"
                  "    " + members + "\n"
                  "  };\n"
                  "  constexpr static size_t value_count = " + std::to_string(args.size() - 1) + ";\n"
                  "  using UnderlyingType = SmallBitset<value_count>::UnderlyingType;\n"
                  "\n"
                  "  constexpr " + bfName + "() = default;\n"
                  "  constexpr " + bfName + "(InitializerList<Enum> values) : bits(" + bfName + "::toBits(values)) {}\n"
                  "  constexpr " + bfName + "(Enum value) : bits(static_cast<uint64_t>(1ULL << static_cast<size_t>(value))) {}\n"
                  "\n"
                  "private:\n"
                  "  constexpr " + bfName + "(SmallBitset<value_count> bits) : bits(bits) {}\n"
                  "\n"
                  "public:\n"
                  "  " + bfName + "& add(Enum value) { this->bits.set(static_cast<size_t>(value)); return *this; }\n"
                  "  " + bfName + "& remove(Enum value) { this->bits.reset(static_cast<size_t>(value)); return *this; }\n"
                  "  " + bfName + "& flip(Enum value) { this->bits.flip(static_cast<size_t>(value)); return *this; }\n"
                  "  " + bfName + "& set(Enum value, bool boolValue) { boolValue ? this->add(value) : this->remove(value); return *this; }\n"
                  "\n"
                  "  " + bfName + "& set() { this->bits.set(); return *this; }\n"
                  "  " + bfName + "& reset() { this->bits.reset(); return *this; }\n"
                  "  " + bfName + "& flip() { this->bits.flip(); return *this; }\n"
                  "\n"
                  "  constexpr bool none() const { return this->bits.none(); }\n"
                  "  constexpr bool any() const { return this->bits.any(); }\n"
                  "  constexpr bool all() const { return this->bits.all(); }\n"
                  "  template<Enum value>\n"
                  "  constexpr bool contains() const { return this->bits.test<static_cast<size_t>(value)>(); }\n"
                  "  constexpr bool checkedContains(Enum value) const { return this->bits.test(static_cast<size_t>(value)); }\n"
                  "\n"
                  "  constexpr bool operator==(const " + bfName + " other) const { return this->bits == other.bits; }\n"
                  "  constexpr bool operator!=(const " + bfName + " other) const { return this->bits != other.bits; }\n"
                  "  constexpr " + bfName + " operator|(Enum value) const { return (*this) | " + bfName + "(value); }\n"
                  "  constexpr " + bfName + " operator|(const " + bfName + " other) const { return " + bfName + "(this->bits | other.bits); }\n"
                  "  constexpr " + bfName + " operator&(Enum value) const { return (*this) & " + bfName + "(value); }\n"
                  "  constexpr " + bfName + " operator&(const " + bfName + " other) const { return " + bfName + "(this->bits & other.bits); }\n"
                  "  " + bfName + "& operator&=(const " + bfName + "& other) { this->bits &= other.bits; return *this; }\n"
                  "  " + bfName + "& operator|=(const " + bfName + "& other) { this->bits |= other.bits; return *this; }\n"
                  "\n"
                  "  constexpr SmallBitset<value_count>::UnderlyingType getBits() const { return this->bits.getBits(); }\n"
                  "  static " + bfName + " fromBits(uint64_t bits) { " + bfName + " value; value.bits = SmallBitset<value_count>(bits); return value; }\n"
                  "\n"
                  "  using enum Enum;\n"
                  "private:\n"
                  "  constexpr static uint64_t toBits(InitializerList<Enum> values)\n"
                  "  {\n"
                  "    uint64_t acc = 0;\n"
                  "    for (auto value : values)\n"
                  "      acc |= static_cast<uint64_t>(1ULL << static_cast<size_t>(value));\n"
                  "    return acc;\n"
                  "  }\n"
                  "\n"
                  "  SmallBitset<value_count> bits;\n"
                  "};";
                // The expansion ends the class with its own `;`, so a `;` after the use goes with it.
                uint32_t end = j;
                if (end + 1 < toks.size() && toks[end + 1].is(P::Semi))
                  end = j + 1;
                replace(t.offset, endOffset(end) - t.offset, expanded);
                counts.add("BITFIELD expanded");
                i = end;
                continue;
              }
            }
          }
          // MAKE_OPTIONAL(BaseClass, EmptyExpr) -> what the macro defines: the OptionalEmptyValue<BaseClass>
          // specialisation returning EmptyExpr, and `using OptionalBaseClass = GameOptional<BaseClass>;`.
          if (name == "MAKE_OPTIONAL" && toks[i + 1].is(P::LParen))
          {
            int depth = 0;
            uint32_t j = i + 1;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && --depth == 0)
                break;
            }
            if (j < toks.size())
            {
              // First argument is BaseClass name.
              uint32_t firstArgStart = i + 2;
              uint32_t firstArgEnd = firstArgStart;
              for (; firstArgEnd < j; ++firstArgEnd)
                if (toks[firstArgEnd].is(P::Comma))
                  break;
              if (firstArgEnd < j)
              {
                std::string baseClass(text.substr(toks[firstArgStart].offset, toks[firstArgEnd].offset - toks[firstArgStart].offset));
                while (!baseClass.empty() && isspace((unsigned char)baseClass.back())) baseClass.pop_back();
                while (!baseClass.empty() && isspace((unsigned char)baseClass.front())) baseClass.erase(baseClass.begin());
                std::string empty(text.substr(toks[firstArgEnd + 1].offset, toks[j].offset - toks[firstArgEnd + 1].offset));
                while (!empty.empty() && isspace((unsigned char)empty.back())) empty.pop_back();
                while (!empty.empty() && isspace((unsigned char)empty.front())) empty.erase(empty.begin());
                std::string expanded = "template<>\n"
                                       "struct OptionalEmptyValue<" + baseClass + ">\n"
                                       "{\n"
                                       "  constexpr static " + baseClass + " value() { return " + empty + "; }\n"
                                       "};\n"
                                       "using Optional" + baseClass + " = GameOptional<" + baseClass + ">;";
                uint32_t end = j;
                if (end + 1 < toks.size() && toks[end + 1].is(P::Semi))
                  end = j + 1;
                replace(t.offset, endOffset(end) - t.offset, expanded);
                counts.add("MAKE_OPTIONAL expanded");
                i = end;
                continue;
              }
            }
          }
          auto am = argumentMacros.find(name);
          if (am != argumentMacros.end() && toks[i + 1].is(P::LParen))
          {
            // Recorded with its arguments' token ranges; the tokens inside stay in the stream for
            // the other rewrites, and run() replaces the whole use.
            ArgumentMacroUse use{t.offset, 0, {}, &am->second};
            uint32_t j = i + 2, argStart = j;
            int depth = 0;
            for (; j < toks.size(); ++j)
            {
              const Token& tj = toks[j];
              if (tj.is(P::LParen) || tj.is(P::LBracket) || tj.is(P::LBrace))
                ++depth;
              else if ((tj.is(P::RParen) || tj.is(P::RBracket) || tj.is(P::RBrace)) && depth-- == 0)
                break;
              else if (tj.is(P::Comma) && depth == 0)
              {
                use.args.push_back({toks[argStart].offset, endOffset(j - 1)});
                argStart = j + 1;
              }
            }
            if (j < toks.size() && toks[j].is(P::RParen) && j > argStart)
            {
              use.args.push_back({toks[argStart].offset, endOffset(j - 1)});
              use.end = endOffset(j);
            }
            if (use.end && use.args.size() == arityOf(am->second))
            {
              argumentMacroUses.push_back(std::move(use));
              counts.add("argument macro expanded");
            }
            else
              todo(t.offset, name + " with arguments it does not take: left as written");
            continue;
          }
          auto kf = knownFunctionMacros.find(name);
          if (kf != knownFunctionMacros.end() && toks[i + 1].is(P::LParen))
          {
            int depth = 0;
            uint32_t j = i + 1;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && --depth == 0)
                break;
            }
            if (j < toks.size())
            {
              // The text before $1 replaces `MACRO(`, the text after it the closing `)`; the
              // argument's own tokens stay in the stream for the other rewrites.
              size_t dollar = kf->second.find("$1");
              std::string before = withFileAndLine(dollar == std::string::npos ? kf->second : kf->second.substr(0, dollar), t.offset);
              std::string after = withFileAndLine(dollar == std::string::npos ? std::string() : kf->second.substr(dollar + 2), t.offset);
              if (dollar == std::string::npos && j > i + 2)
              {
                // The arguments go with the macro (LOG_DEBUG is `((void)0)` here); the use stays
                // readable as a comment.
                std::string use(text.substr(t.offset, endOffset(j) - t.offset));
                for (size_t p = use.find("*/"); p != std::string::npos; p = use.find("*/", p))
                  use.replace(p, 2, "* /");
                replace(t.offset, endOffset(j) - t.offset, before + " /* " + use + " */");
              }
              else if (dollar == std::string::npos)
                replace(t.offset, endOffset(j) - t.offset, before);
              else
              {
                replace(t.offset, toks[i + 2].offset - t.offset, before);
                replace(toks[j].offset, toks[j].length, after);
              }
              counts.add("macro use rewritten");
            }
          }
        }
      }
    }

    // --- declaration-level rewrites ---

    static bool isDefinition(FunctionDecl* f) { return f->body != nullptr || (f->flags & (DF_Default | DF_Delete)); }

    // Whether a hand-written override replaces this namespace-scope definition.
    bool overridden(Decl* d, const std::string& prefix)
    {
      if (!overrides)
        return false;
      if (d->kind == NodeKind::FunctionDecl)
      {
        auto* f = static_cast<FunctionDecl*>(d);
        if (!isFunctionDefinition(f))
          return false;
        std::string q = prefix + nameText(f->name, symbols);
        std::string resolved = global.qualifiedName(f); // the namespace the resolver put it in
        size_t cut = resolved.rfind("::");
        std::string alt = (cut == std::string::npos ? std::string() : resolved.substr(0, cut + 2)) + nameText(f->name, symbols);
        for (const std::string& key : {q, alt})
          if (overrides->functions.count(key + "/" + std::to_string(f->params.size())) || overrides->functions.count(key + "/*"))
          {
            replacedFunctions.insert(key);
            return true;
          }
        for (const std::string& c : overrides->classes)
          if (q.size() > c.size() + 2 && q.compare(0, c.size(), c) == 0 && q.compare(c.size(), 2, "::") == 0)
            return true; // a member of a replaced class, defined out of line
        return false;
      }
      if (d->kind == NodeKind::ClassDecl)
      {
        auto* c = static_cast<ClassDecl*>(d);
        return c->isDefinition && !c->name.empty() && overrides->classes.count(prefix + nameText(c->name, symbols));
      }
      if (d->kind == NodeKind::VariableDecl)
      {
        std::string q = prefix + nameText(static_cast<VariableDecl*>(d)->name, symbols);
        if (!overrides->variables.count(q))
          return false;
        replacedVariables.insert(q);
        return true;
      }
      return false;
    }

    void convertDecls(Span<Decl*> decls, bool namespaceScope, const std::string& prefix = "")
    {
      for (Decl* d : decls)
      {
        if (namespaceScope && overridden(d, prefix))
        {
          removeDefinition(d);
          counts.add("replaced by the override file");
          continue;
        }
        if ((d->flags & DF_Friend) && d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body && !classStack.empty())
        {
          // A hidden friend, defined inside the class: Kpp has no friends, so it becomes a member
          // operator or moves out to namespace scope, body and all.
          if (!convertHiddenFriend(static_cast<FunctionDecl*>(d)))
            continue;
        }
        else if ((d->flags & DF_Friend) && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::FunctionDecl))
        {
          removeDecl(d); // no friends in Kpp; `friend class X;` would read as a nested class
          counts.add("friend declarations removed");
          continue;
        }
        if (d->kind == NodeKind::DeductionGuideDecl)
        {
          removeDecl(d); // Kpp deduces class arguments from constructors
          counts.add("deduction guides removed");
          continue;
        }
        if (namespaceScope && (d->flags & DF_Static) && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) && isHeader())
        {
          // `static` at namespace scope in a header gives every C++ file that includes it a copy
          // of its own; in Kpp a header and its source are one file and a `static` name stays in
          // it, so the other files would not find it (EMPTY_CARGO_STATION_ID): the `static` goes.
          uint32_t start = declStart(d);
          uint32_t first = d->token;
          while (first > 0 && toks[first - 1].offset >= start)
            --first;
          for (uint32_t k = first; k < toks.size(); ++k)
          {
            if (toks[k].is(P::LParen) || toks[k].is(P::Assign) || toks[k].is(P::LBrace) || toks[k].is(P::Semi))
              break;
            if (toks[k].is(Kw::static_))
            {
              uint32_t end = endOffset(k);
              while (end < text.size() && (text[end] == ' ' || text[end] == '\t'))
                ++end;
              remove(toks[k].offset, end - toks[k].offset);
              counts.add("static dropped from a header declaration");
              break;
            }
          }
        }
        if (isGenericVariantCode(d))
        {
          // Code for any std::variant<T...> (index recursion over variant_size_v): a std::variant
          // becomes a typed union, which takes no such code; the game's generic save and load of
          // them are hand-written with alternatives(U) (overrides, `[[members]]` classes).
          removeDecl(d);
          counts.add("generic std::variant code removed");
          continue;
        }
        dropAllocatorParameters(d);
        switch (d->kind)
        {
          case NodeKind::NamespaceDecl:
          {
            auto* n = static_cast<NamespaceDecl*>(d);
            std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
            convertDecls(n->decls, true, prefix + name + "::");
            break;
          }
          case NodeKind::ClassDecl:
          {
            auto* c = static_cast<ClassDecl*>(d);
            if (c->isDefinition && packedAt(toks[c->token].offset))
            {
              // The class keyword, after any `template<...>`.
              uint32_t k = c->token;
              if (toks[k].is(Kw::template_) && k + 1 < toks.size() && toks[k + 1].is(P::Less))
              {
                for (int depth = 0; ++k < toks.size();)
                {
                  if (toks[k].is(P::Less))
                    ++depth;
                  else if (toks[k].is(P::Greater) && --depth == 0)
                    break;
                  else if (toks[k].is(P::Shr) && (depth -= 2) <= 0)
                    break;
                }
                ++k;
              }
              if (k < toks.size() && (toks[k].is(Kw::class_) || toks[k].is(Kw::struct_) || toks[k].is(Kw::union_)))
              {
                insert(endOffset(k), " [[packed]]");
                counts.add("#pragma pack(push, 1) classes [[packed]]");
              }
            }
            if (!c->isDefinition)
            {
              Decl* def = global.definitionOf(c);
              // A class template whose primary is only declared, defined through its partial
              // specializations (`template<typename...> struct IndexInTemplateList;`): the
              // declaration is the primary template, which the specializations need.
              bool specializedOnly = def && def != c && def->kind == NodeKind::ClassDecl && !c->templateParams.empty() &&
                                     !static_cast<ClassDecl*>(def)->name.empty() && static_cast<ClassDecl*>(def)->name.last().hasTemplateArgs;
              bool definedInLeftOutFile = def && leftOutFiles && leftOutFiles->count(def->file);
              if (namespaceScope && opaqueKept && (!def || def == c || specializedOnly || definedInLeftOutFile) &&
                  opaqueKept->insert(global.qualifiedName(c)).second)
              {
                // No definition anywhere (`namespace TaskBar { class Interface; }`, used only through
                // pointers; the tags only template arguments name, `class ElectricityTag;`): the
                // declaration is the class, an opaque type; kept once in the program. At global scope
                // the name may also be a C library's type (SDL_Cursor, which the C import defines too)
                // or a class a macro defines (BITFIELD): a declaration next to a definition is fine.
                counts.add(specializedOnly ? "primary templates only specialized kept" : "declarations of classes defined nowhere kept");
              }
              else if (namespaceScope)
              {
                removeDecl(d);
                counts.add("forward declarations removed");
              }
              break;
            }
            if (carriedTemplateDefaults)
              if (auto cd = carriedTemplateDefaults->find(c); cd != carriedTemplateDefaults->end())
                for (auto& [index, value] : cd->second)
                  if (index < c->templateParams.size())
                    if (uint32_t end = templateParameterEnd(c->templateParams[index]->token); end != UINT32_MAX)
                    {
                      insert(endOffset(end - 1), " = " + value);
                      counts.add("default template arguments carried from forward declarations");
                    }
            if (memberOverrides)
              if (auto mo = memberOverrides->find(prefix + nameText(c->name, symbols)); mo != memberOverrides->end() && !mo->second.replaces.empty())
                for (Decl* member : c->members)
                  if (std::string name = replaceableName(member, symbols); !name.empty() && mo->second.replaces.count(name))
                  {
                    removeDecl(member);
                    counts.add("members replaced by hand-written ones");
                  }
            classStack.push_back(c);
            convertDecls(c->members, false, prefix + nameText(c->name, symbols) + "::");
            classStack.pop_back();
            if (memberOverrides)
              if (auto mo = memberOverrides->find(prefix + nameText(c->name, symbols)); mo != memberOverrides->end())
              {
                // An override file's `class [[members]] Name { ... }`: its members join this class,
                // before the closing brace.
                uint32_t close = d->endToken;
                while (close > d->token && !toks[close - 1].is(P::RBrace))
                  --close;
                if (close > d->token)
                {
                  insert(toks[close - 1].offset, "\n  // --- Hand-written members, from the override file ---\n" + mo->second.members + "\n");
                  mo->second.used = true;
                  counts.add("hand-written members added");
                }
              }
            break;
          }
          case NodeKind::EnumDecl:
          {
            auto* e = static_cast<EnumDecl*>(d);
            if (!e->isDefinition && namespaceScope)
            {
              removeDecl(d);
              counts.add("forward declarations removed");
            }
            break;
          }
          case NodeKind::FunctionDecl:
          {
            auto* f = static_cast<FunctionDecl*>(d);
            if (f->templateParams.empty() && !f->body && !f->name.empty() && f->name.last().hasTemplateArgs && !(f->flags & DF_Specialization))
            {
              removeDecl(d); // an explicit instantiation: templates compile from source in Kpp
              counts.add("explicit instantiations removed");
              break;
            }
            if (!isDefinition(f) && namespaceScope && !(f->flags & DF_ExternC))
            {
              Decl* def = global.definitionOf(f);
              if (def && def != f && def->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(def)->body)
              {
                removeDecl(d);
                counts.add("forward declarations removed");
                break;
              }
            }
            if (f->body && carriedTemplateDefaults)
              if (auto cd = carriedTemplateDefaults->find(f); cd != carriedTemplateDefaults->end())
                for (auto& [index, value] : cd->second)
                  if (index < f->templateParams.size())
                    if (uint32_t end = templateParameterEnd(f->templateParams[index]->token); end != UINT32_MAX)
                    {
                      insert(endOffset(end - 1), " = " + value);
                      counts.add("default template arguments carried from forward declarations");
                    }
            if (f->body && carriedDefaults)
              if (auto cd = carriedDefaults->find(f); cd != carriedDefaults->end())
                for (auto& [index, value] : cd->second)
                  if (index < f->params.size())
                    if (uint32_t end = parameterEndIn(toks, f->params[index]->token); end != UINT32_MAX)
                    {
                      insert(toks[end].offset, " = " + value);
                      counts.add("default arguments carried from declarations");
                    }
            if ((f->flags & DF_Constructor) && !(f->flags & (DF_Explicit | DF_Implicit)) && convertingConstructor(f))
            {
              insert(toks[f->token].offset, "implicit ");
              counts.add("implicit added");
            }
            if ((f->flags & DF_Conversion) && !(f->flags & (DF_Explicit | DF_Implicit)))
            {
              insert(toks[f->token].offset, "implicit ");
              counts.add("implicit added");
            }
            if (f->body)
            {
              Decl* owner = global.owningClass(f);
              walkStmt(f->body, owner);
            }
            for (const FunctionDecl::CtorInit& init : f->ctorInits)
              for (Expr* a : init.args)
                walkExpr(a, global.owningClass(f));
            break;
          }
          case NodeKind::VariableDecl:
          {
            auto* v = static_cast<VariableDecl*>(d);
            recordVariantDeclaration(d, true);
            walkExpr(v->init, nullptr);
            break;
          }
          case NodeKind::UsingAliasDecl:
          {
            recordVariantDeclaration(d, false);
            // `using path = path;` in a namespace that a using-directive opens to the other path
            // (Filesystem.hpp: `using namespace __fs;`): in C++ the alias's own name is not in
            // scope in its type, in Kpp it would name the alias itself; the type is qualified
            // with the namespace the directive names (with `::` when there is none).
            auto* a = static_cast<UsingAliasDecl*>(d);
            Type* t = a->type;
            if (t && t->token + 1 < toks.size() && toks[t->token].getKind() == TokenKind::Identifier &&
                spelling(t->token) == symbols.name(a->name) && toks[t->token + 1].is(P::Semi) && !(t->token > 0 && toks[t->token - 1].is(P::ColonColon)))
            {
              std::string qualifier;
              for (Decl* other : decls)
                if (other->kind == NodeKind::UsingDirectiveDecl)
                  qualifier = nameText(static_cast<UsingDirectiveDecl*>(other)->name, symbols);
              insert(toks[t->token].offset, qualifier + "::");
              counts.add("alias of its own name qualified");
            }
            break;
          }
          default: break;
        }
      }
    }

    // A hidden friend of the innermost class on classStack (`friend bool operator==(const A& a,
    // const A& b) { ... }`). An operator whose first parameter is that class becomes a member
    // operator: the first parameter goes, its uses read (*this), and the member is const unless the
    // parameter was a non-const reference; returns true, and the caller converts it as a member.
    // Anything else moves after the outermost class as a function at namespace scope (a function
    // template with the parameters of a class template that its signature names); returns false.
    bool convertHiddenFriend(FunctionDecl* f)
    {
      ClassDecl* cls = classStack.back();
      // `friend` is among the specifiers, before or after the declaration's first token.
      uint32_t from = f->token;
      while (from > 0 && !(toks[from - 1].is(P::Semi) || toks[from - 1].is(P::LBrace) || toks[from - 1].is(P::RBrace) || toks[from - 1].is(P::Colon)))
        --from;
      uint32_t friendToken = UINT32_MAX;
      for (uint32_t k = from; k < toks.size() && !toks[k].is(P::LParen) && !toks[k].is(P::LBrace); ++k)
        if (spelling(k) == "friend")
        {
          friendToken = k;
          break;
        }
      bool nested = classStack.size() > 1;
      bool inTemplate = false;
      for (ClassDecl* c : classStack)
        inTemplate = inTemplate || c->isTemplate;
      if (friendToken == UINT32_MAX || f->params.empty() || f->endToken <= f->token || cls->name.empty())
      {
        todo(toks[f->token].offset, "a hidden friend the converter cannot place: left out");
        removeDecl(f);
        counts.add("friend declarations removed");
        return false;
      }

      auto* first = static_cast<ParamDecl*>(f->params[0]);
      Type* t = first->type;
      bool reference = t && t->kind == NodeKind::LRefType;
      if (reference)
        t = static_cast<LRefType*>(t)->referenced;
      bool isConst = t && t->kind == NodeKind::ConstType;
      if (isConst)
        t = static_cast<ConstType*>(t)->inner;
      bool ofClass = t && t->kind == NodeKind::NamedType && !static_cast<NamedType*>(t)->name.empty() &&
                     static_cast<NamedType*>(t)->name.last().symbol == cls->name.last().symbol;
      bool isOperator = !f->name.empty() && f->name.last().special == 2;
      if (isOperator && f->params.size() == 2 && ofClass && first->name != UINT32_MAX)
      {
        uint32_t comma = parameterEndIn(toks, first->token);
        uint32_t close = parameterEndIn(toks, f->params[f->params.size() - 1]->token);
        if (comma != UINT32_MAX && toks[comma].is(P::Comma) && close != UINT32_MAX)
        {
          remove(toks[friendToken].offset, toks[friendToken + 1].offset - toks[friendToken].offset);
          remove(toks[first->token].offset, toks[comma + 1].offset - toks[first->token].offset);
          if (isConst || !reference)
            insert(endOffset(close), " const");
          std::string_view self = symbols.name(first->name);
          for (uint32_t k = f->body->token; k < f->endToken; ++k)
            if (toks[k].getKind() == TokenKind::Identifier && spelling(k) == self &&
                !(toks[k - 1].is(P::Dot) || toks[k - 1].is(P::Arrow) || toks[k - 1].is(P::ColonColon)))
              replace(toks[k].offset, toks[k].length, "(*this)");
          counts.add("hidden friend operators made members");
          return true;
        }
      }
      // Moved out, a friend of a class nested in a class template would be a function template
      // whose parameters its signature cannot deduce (`RectangleUnionLogic<P>::IntervalPoint`).
      if (nested && inTemplate)
      {
        todo(toks[f->token].offset, "a hidden friend the converter cannot place: left out");
        removeDecl(f);
        counts.add("friend declarations removed");
        return false;
      }
      remove(toks[friendToken].offset, toks[friendToken + 1].offset - toks[friendToken].offset);

      MovedFriend moved;
      moved.begin = declStart(f);
      moved.end = endOffset(f->endToken - 1);
      moved.removeEnd = lineEndAfter(moved.end);
      ClassDecl* outer = classStack.front();
      uint32_t last = outer->endToken - 1;
      if (!toks[last].is(P::Semi) && last + 1 < toks.size() && toks[last + 1].is(P::Semi))
        ++last;
      moved.at = endOffset(last);
      if (cls->isTemplate)
      {
        // The class template's parameters that the signature names (others cannot be deduced).
        std::string_view signature = text.substr(moved.begin, toks[f->body->token].offset - moved.begin);
        std::string params;
        for (Decl* p : cls->templateParams)
        {
          auto* tp = static_cast<TemplateParamDecl*>(p);
          if (tp->name == UINT32_MAX || !containsWord(signature, symbols.name(tp->name)))
            continue;
          uint32_t end = templateParameterEnd(tp->token);
          uint32_t stop = tp->token;
          while (stop < end && !toks[stop].is(P::Assign))
            ++stop;
          std::string param(text.substr(toks[tp->token].offset, endOffset(stop - 1) - toks[tp->token].offset));
          params += (params.empty() ? "" : ", ") + param;
        }
        moved.header = "template<" + params + ">\n";
      }
      // In the class, the friend saw the classes' nested types (and a nested class itself) by their
      // bare names; at namespace scope they need the classes' names in front.
      std::string chain;
      for (ClassDecl* c : classStack)
      {
        std::string outerChain = chain;
        chain += (chain.empty() ? "" : "::") + nameText(c->name, symbols);
        if (!outerChain.empty())
          moved.qualify[std::string(symbols.name(c->name.last().symbol))] = chain;
        for (Decl* m : c->members)
        {
          uint32_t name = UINT32_MAX;
          if (m->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(m)->name.empty())
            name = static_cast<ClassDecl*>(m)->name.last().symbol;
          else if (m->kind == NodeKind::EnumDecl && !static_cast<EnumDecl*>(m)->name.empty())
            name = static_cast<EnumDecl*>(m)->name.last().symbol;
          else if (m->kind == NodeKind::UsingAliasDecl)
            name = static_cast<UsingAliasDecl*>(m)->name;
          if (name != UINT32_MAX)
            moved.qualify[std::string(symbols.name(name))] = chain + "::" + std::string(symbols.name(name));
        }
      }
      movedFriends.push_back(moved);
      walkStmt(f->body, nullptr); // not a member: no this->
      counts.add("hidden friends moved to namespace scope");
      return false;
    }

    // A std::variant alias or member of this file (one of `variants`): what run() turns into a union enum.
    void recordVariantDeclaration(Decl* d, bool member)
    {
      if (!variants || d->endToken <= d->token)
        return;
      auto it = variants->byDecl.find(d);
      if (it == variants->byDecl.end())
        return;
      Type* type = member ? static_cast<VariableDecl*>(d)->type : static_cast<UsingAliasDecl*>(d)->type;
      uint32_t variantStart = 0, variantClose = 0;
      auto args = variantArgumentTokens(toks, text, type->token, d->endToken, variantStart, variantClose);
      if (args.empty())
        return;
      VariantDeclaration v;
      v.info = &variants->all[it->second];
      v.member = member;
      v.memberStart = declStart(d);
      if (member)
      {
        v.begin = toks[variantStart].offset;
        v.end = endOffset(variantClose);
      }
      else
      {
        v.begin = declStart(d);
        uint32_t last = d->endToken - 1;
        if (!toks[last].is(P::Semi) && last + 1 < toks.size() && toks[last + 1].is(P::Semi))
          ++last;
        v.end = endOffset(last);
      }
      for (auto [a0, a1] : args)
        v.args.push_back({toks[a0].offset, endOffset(a1 - 1)});
      variantDeclarations.push_back(std::move(v));
      counts.add("std::variant to union enum");
    }

    // The variant an expression is declared as: a variable, parameter or member whose type is one of
    // the variant aliases (or a variant member itself), or a call of a function returning one.
    const VariantInfo* variantOfType(Type* t)
    {
      while (t && (t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType || t->kind == NodeKind::ConstType))
        t = t->kind == NodeKind::LRefType ? static_cast<LRefType*>(t)->referenced
          : t->kind == NodeKind::RRefType ? static_cast<RRefType*>(t)->referenced
                                          : static_cast<ConstType*>(t)->inner;
      if (!t || t->kind != NodeKind::NamedType || !static_cast<NamedType*>(t)->resolved)
        return nullptr;
      auto it = variants->byDecl.find(static_cast<NamedType*>(t)->resolved);
      return it == variants->byDecl.end() ? nullptr : &variants->all[it->second];
    }
    // An argument that is a variable of a GUID type (GUID, IID, CLSID, WIC's pixel format GUID), by
    // name: C++ passes it to a REFGUID-like parameter, a reference, which C's (and so the import's
    // and the COM classes') is a pointer to it.
    bool isGuidVariable(Expr* e) const
    {
      Decl* d = e && e->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(e)->resolved : nullptr;
      Type* t = !d ? nullptr : d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d)->type
              : d->kind == NodeKind::ParamDecl                   ? static_cast<ParamDecl*>(d)->type
                                                                 : nullptr;
      while (t && t->kind == NodeKind::ConstType)
        t = static_cast<ConstType*>(t)->inner;
      if (!t || t->kind != NodeKind::NamedType || static_cast<NamedType*>(t)->name.parts.size() != 1)
        return false;
      uint32_t symbol = static_cast<NamedType*>(t)->name.parts[0].symbol;
      if (symbol == UINT32_MAX)
        return false;
      std::string_view name = symbols.name(symbol);
      return name == "GUID" || name == "IID" || name == "CLSID" || name == "WICPixelFormatGUID";
    }
    // `output << this->tagsByTagNumber.size()`: a std container's size() is a size_t in C++ and an
    // int in Kpp (docs/stdlib/common.md), which a serialiser's operator<< writes in 4 bytes instead
    // of 8: the game's complete-mode saves (its CRCs, desync reports) came out other than the C++
    // build's (TagManager::save, 4 bytes short per force). Written `size_t(...)` where it is the
    // right operand of a <<, the one place where C++ code sends a size on by its type (the game's
    // saveContainerSize and saveSpaceOptimized take it as a FullType). length() and count(k) alike,
    // and wherever the declarations say the container's type (typeOfExpr): CustomGui::save's
    // `item.second.size()` of a map's std::set put the heuristic CRC 4 bytes short per mod.
    // A std container, written so or through an alias of one, or a class derived from one
    // (ScheduleRecords : std::vector<ScheduleRecord>): whose size() is the library's int.
    static const std::set<std::string_view>& stdContainerNames()
    {
      static const std::set<std::string_view> containers = {
        "vector", "deque", "list", "map", "multimap", "set", "multiset", "unordered_map", "unordered_multimap", "unordered_set",
        "unordered_multiset", "string", "wstring", "u16string", "u32string", "string_view", "wstring_view", "array", "bitset", "span",
        "queue", "stack", "priority_queue"};
      return containers;
    }
    bool isStdContainerType(Type* type)
    {
      NamedType* named = stdTemplateOf(type, 0);
      return named && stdContainerNames().count(stdTemplateName(named)) != 0;
    }
    // The std container (or std::pair) a type names, written so, through aliases or as a base class:
    // the NamedType whose last part carries its template arguments. Null for any other type.
    NamedType* stdTemplateOf(Type* type, int depth)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType || type->kind == NodeKind::RRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner
             : type->kind == NodeKind::LRefType  ? static_cast<LRefType*>(type)->referenced
                                                 : static_cast<RRefType*>(type)->referenced;
      if (!type || type->kind != NodeKind::NamedType || depth > 8)
        return nullptr;
      auto* named = static_cast<NamedType*>(type);
      const QualifiedName& name = named->name;
      if (name.parts.size() == 2 && name.parts[0].symbol != UINT32_MAX && name.parts[1].symbol != UINT32_MAX &&
          symbols.name(name.parts[0].symbol) == "std")
        return named;
      Decl* d = named->resolved;
      if (!d)
        return nullptr;
      if (d->kind == NodeKind::UsingAliasDecl)
        return stdTemplateOf(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return stdTemplateOf(static_cast<VariableDecl*>(d)->type, depth + 1);
      if (d->kind == NodeKind::ClassDecl)
        for (const ClassDecl::Base& b : static_cast<ClassDecl*>(d)->bases)
          if (NamedType* base = stdTemplateOf(b.type, depth + 1))
            return base;
      return nullptr;
    }
    std::string_view stdTemplateName(NamedType* named) { return symbols.name(named->name.parts[1].symbol); }
    Type* stdTemplateArg(NamedType* named, uint32_t i)
    {
      const NamePart& part = named->name.parts[1];
      Node* arg = i < part.templateArgs.size() ? part.templateArgs[i] : nullptr;
      return arg && arg->kind <= NodeKind::PackType ? static_cast<Type*>(arg) : nullptr;
    }
    // What an expression is, as far as the declarations say: a type, or a std::pair's (a map element's)
    // two types; an iterator's element (begin(), find()) is the same with `iterator` set. The type of
    // an auto variable is its initialiser's or, for a range-for's, its range's element.
    struct SeenType
    {
      Type* type = nullptr;
      Type* first = nullptr;
      Type* second = nullptr;
      bool iterator = false;
      bool known() const { return type || first; }
    };
    std::map<Decl*, Expr*> rangeForRanges; // a range-for's variable: its range
    // A std container's element: a map's key and value pair, an array's or a sequence's T.
    SeenType elementOf(const SeenType& of)
    {
      SeenType element;
      if (!of.type)
        return element;
      Type* t = of.type;
      while (t && (t->kind == NodeKind::ConstType || t->kind == NodeKind::LRefType))
        t = t->kind == NodeKind::ConstType ? static_cast<ConstType*>(t)->inner : static_cast<LRefType*>(t)->referenced;
      if (t && t->kind == NodeKind::ArrayType)
      {
        element.type = static_cast<ArrayType*>(t)->element;
        return element;
      }
      NamedType* named = stdTemplateOf(t, 0);
      if (!named)
        return element;
      std::string_view name = stdTemplateName(named);
      if (name == "map" || name == "multimap" || name == "unordered_map" || name == "unordered_multimap")
      {
        element.first = stdTemplateArg(named, 0);
        element.second = stdTemplateArg(named, 1);
      }
      else if (name == "vector" || name == "deque" || name == "list" || name == "set" || name == "multiset" || name == "unordered_set" ||
               name == "unordered_multiset" || name == "array" || name == "span")
        element.type = stdTemplateArg(named, 0);
      return element;
    }
    SeenType typeOfDecl(Decl* d, int depth)
    {
      SeenType seen;
      if (!d || depth > 16)
        return seen;
      Type* type = d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d)->type
                 : d->kind == NodeKind::ParamDecl    ? static_cast<ParamDecl*>(d)->type
                                                     : nullptr;
      Type* t = type;
      while (t && (t->kind == NodeKind::ConstType || t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType))
        t = t->kind == NodeKind::ConstType ? static_cast<ConstType*>(t)->inner
          : t->kind == NodeKind::LRefType  ? static_cast<LRefType*>(t)->referenced
                                           : static_cast<RRefType*>(t)->referenced;
      if (!t || t->kind != NodeKind::AutoType)
      {
        seen.type = type;
        return seen;
      }
      if (auto it = rangeForRanges.find(d); it != rangeForRanges.end())
        return elementOf(typeOfExpr(it->second, depth + 1));
      if (d->kind == NodeKind::VariableDecl)
        if (Expr* init = static_cast<VariableDecl*>(d)->init)
          return typeOfExpr(init, depth + 1);
      return seen;
    }
    SeenType typeOfExpr(Expr* e, int depth)
    {
      SeenType seen;
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e || depth > 16)
        return seen;
      switch (e->kind)
      {
        case NodeKind::NameExpr: return typeOfDecl(static_cast<NameExpr*>(e)->resolved, depth + 1);
        case NodeKind::MemberExpr:
        {
          auto* member = static_cast<MemberExpr*>(e);
          if (member->resolved)
            return typeOfDecl(member->resolved, depth + 1);
          if (member->member.parts.size() != 1 || member->member.parts[0].symbol == UINT32_MAX)
            return seen;
          // `item.second` of a map element, `it->first` of a map iterator, `pair.first` of a std::pair.
          SeenType base = typeOfExpr(member->base, depth + 1);
          if ((member->op == P::Arrow) != base.iterator)
            return seen;
          if (!base.first && base.type)
            if (NamedType* named = stdTemplateOf(base.type, 0); named && stdTemplateName(named) == "pair")
            {
              base.first = stdTemplateArg(named, 0);
              base.second = stdTemplateArg(named, 1);
            }
          std::string_view name = symbols.name(member->member.parts[0].symbol);
          seen.type = name == "first" ? base.first : name == "second" ? base.second : nullptr;
          return seen;
        }
        case NodeKind::IndexExpr:
        {
          auto* index = static_cast<IndexExpr*>(e);
          SeenType base = typeOfExpr(index->base, depth + 1);
          SeenType element = elementOf(base);
          if (element.first) // a map's [] is its value
            seen.type = element.second;
          else
            seen = element;
          return seen;
        }
        case NodeKind::UnaryExpr:
        {
          auto* unary = static_cast<UnaryExpr*>(e);
          if (P(unary->op) != P::Star)
            return seen;
          SeenType operand = typeOfExpr(unary->operand, depth + 1);
          if (!operand.iterator)
            return seen;
          operand.iterator = false;
          return operand;
        }
        case NodeKind::CallExpr:
        {
          // A std container's front() / back() / at(i): its element; begin() / find(k): an iterator to one.
          auto* call = static_cast<CallExpr*>(e);
          if (call->callee->kind != NodeKind::MemberExpr)
            return seen;
          auto* member = static_cast<MemberExpr*>(call->callee);
          if (member->op == P::Arrow || member->member.parts.size() != 1 || member->member.parts[0].symbol == UINT32_MAX)
            return seen;
          std::string_view name = symbols.name(member->member.parts[0].symbol);
          bool access = name == "front" || name == "back" || name == "at";
          bool iterator = name == "begin" || name == "cbegin" || name == "find" || name == "lower_bound" || name == "upper_bound";
          if (!access && !iterator)
            return seen;
          SeenType base = typeOfExpr(member->base, depth + 1);
          if (!base.type || base.iterator || !stdTemplateOf(base.type, 0))
            return seen;
          seen = elementOf(base);
          if (name == "at" && seen.first)
            seen = SeenType{seen.second};
          seen.iterator = iterator;
          return seen;
        }
        default: return seen;
      }
    }
    // A class's member type alias (`using size_type = ...`, `typedef ... size_type`) named symbol.
    Type* memberAlias(ClassDecl* c, uint32_t symbol)
    {
      for (Decl* m : c->members)
        if (m->kind == NodeKind::UsingAliasDecl && static_cast<UsingAliasDecl*>(m)->name == symbol)
          return static_cast<UsingAliasDecl*>(m)->type;
        else if (m->kind == NodeKind::VariableDecl && (m->flags & DF_Typedef) && static_cast<VariableDecl*>(m)->name.parts.size() == 1 &&
                 static_cast<VariableDecl*>(m)->name.parts[0].symbol == symbol)
          return static_cast<VariableDecl*>(m)->type;
      return nullptr;
    }
    // A std container's size_type, as class c names it: `std::vector<T>::size_type`,
    // `container_type::size_type` of a member alias of one, or a member alias of either (FlatSet's
    // `using size_type = typename container_type::size_type;`).
    bool isStdSizeType(Type* type, ClassDecl* c, int depth)
    {
      if (!type || type->kind != NodeKind::NamedType || depth > 8)
        return false;
      const QualifiedName& name = static_cast<NamedType*>(type)->name;
      for (const NamePart& part : name.parts)
        if (part.symbol == UINT32_MAX)
          return false;
      if (name.parts.size() == 1)
        return isStdSizeType(memberAlias(c, name.parts[0].symbol), c, depth + 1);
      if (symbols.name(name.last().symbol) != "size_type")
        return false;
      if (name.parts.size() == 3 && symbols.name(name.parts[0].symbol) == "std")
        return stdContainerNames().count(symbols.name(name.parts[1].symbol)) != 0;
      return name.parts.size() == 2 && isStdContainerType(memberAlias(c, name.parts[0].symbol));
    }
    // The game's class whose size() (or length(), count()) returns its std container's size_type: a
    // size_t in C++, the library's int in Kpp, as a std container's own.
    bool returnsStdSizeType(Type* type, std::string_view function)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner : static_cast<LRefType*>(type)->referenced;
      Decl* d = type && type->kind == NodeKind::NamedType ? static_cast<NamedType*>(type)->resolved : nullptr;
      for (int depth = 0; d && d->kind == NodeKind::UsingAliasDecl && depth < 8; ++depth)
      {
        Type* aliased = static_cast<UsingAliasDecl*>(d)->type;
        d = aliased && aliased->kind == NodeKind::NamedType ? static_cast<NamedType*>(aliased)->resolved : nullptr;
      }
      if (!d || d->kind != NodeKind::ClassDecl)
        return false;
      auto* c = static_cast<ClassDecl*>(d);
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl)
        {
          auto* f = static_cast<FunctionDecl*>(m);
          if (f->name.parts.size() == 1 && f->name.parts[0].symbol != UINT32_MAX && symbols.name(f->name.parts[0].symbol) == function)
            return isStdSizeType(f->returnType, c, 0);
        }
      return false;
    }
    // A std container's iterator type, as the code at owner names it: `std::vector<T>::iterator`, or
    // `WidgetArray::iterator` where WidgetArray is an alias of one in owner or a class or namespace
    // around it (Agui's visible_iterator). Its operator* and operator-> are a pointer's in Kpp.
    bool isStdIteratorType(Type* type, Decl* owner)
    {
      while (type && type->kind == NodeKind::ConstType)
        type = static_cast<ConstType*>(type)->inner;
      if (!type || type->kind != NodeKind::NamedType)
        return false;
      const QualifiedName& name = static_cast<NamedType*>(type)->name;
      for (const NamePart& part : name.parts)
        if (part.symbol == UINT32_MAX)
          return false;
      std::string_view last = symbols.name(name.last().symbol);
      if (last != "iterator" && last != "const_iterator" && last != "reverse_iterator" && last != "const_reverse_iterator")
        return false;
      if (name.parts.size() == 3 && symbols.name(name.parts[0].symbol) == "std")
        return stdContainerNames().count(symbols.name(name.parts[1].symbol)) != 0;
      if (name.parts.size() != 2)
        return false;
      for (Decl* d = owner; d; d = d->parent)
        if (d->kind == NodeKind::ClassDecl)
        {
          if (Type* alias = memberAlias(static_cast<ClassDecl*>(d), name.parts[0].symbol))
            return isStdContainerType(alias);
        }
        else if (d->kind == NodeKind::NamespaceDecl)
          for (Decl* member : static_cast<NamespaceDecl*>(d)->decls)
            if (member->kind == NodeKind::UsingAliasDecl && static_cast<UsingAliasDecl*>(member)->name == name.parts[0].symbol)
              return isStdContainerType(static_cast<UsingAliasDecl*>(member)->type);
      return false;
    }
    // `output << x.size()`, `x.length()`, `x.count(k)` of a std container: written size_t(...).
    void sizeAsSizeT(Expr* e)
    {
      if (!e || e->kind != NodeKind::CallExpr)
        return;
      auto* call = static_cast<CallExpr*>(e);
      if (call->callee->kind != NodeKind::MemberExpr)
        return;
      auto* member = static_cast<MemberExpr*>(call->callee);
      if (member->member.parts.size() != 1 || member->member.parts[0].symbol == UINT32_MAX)
        return;
      std::string_view name = symbols.name(member->member.parts[0].symbol);
      if (!(call->args.size() == 0 && (name == "size" || name == "length")) && !(call->args.size() == 1 && name == "count"))
        return;
      Expr* base = member->base;
      while (base && base->kind == NodeKind::ParenExpr)
        base = static_cast<ParenExpr*>(base)->inner;
      SeenType type = typeOfExpr(base, 0);
      if ((member->op == P::Arrow) != type.iterator || !(isStdContainerType(type.type) || returnsStdSizeType(type.type, name)))
        return;
      // The call's first token is its base's leftmost one (a CallExpr's own token is its parenthesis),
      // its last the `)` that closes that parenthesis.
      Expr* leftmost = base;
      while (leftmost)
        if (leftmost->kind == NodeKind::MemberExpr)
          leftmost = static_cast<MemberExpr*>(leftmost)->base;
        else if (leftmost->kind == NodeKind::IndexExpr)
          leftmost = static_cast<IndexExpr*>(leftmost)->base;
        else if (leftmost->kind == NodeKind::CallExpr)
          leftmost = static_cast<CallExpr*>(leftmost)->callee;
        else
          break;
      if (!leftmost)
        return;
      uint32_t first = leftmost->token;
      uint32_t close = call->token;
      if (close >= toks.size() || !toks[close].is(P::LParen))
        return;
      for (int depth = 0; close < toks.size(); ++close)
        if (toks[close].is(P::LParen))
          ++depth;
        else if (toks[close].is(P::RParen) && --depth == 0)
          break;
      if (close >= toks.size())
        return;
      insert(toks[first].offset, "size_t(");
      insert(endOffset(close), ")");
      counts.add("std container sizes written size_t");
    }
    const VariantInfo* variantOfExpr(Expr* e)
    {
      if (!variants || !e)
        return nullptr;
      while (e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (e->kind == NodeKind::UnaryExpr && P(static_cast<UnaryExpr*>(e)->op) == P::Amp)
        return variantOfExpr(static_cast<UnaryExpr*>(e)->operand); // get_if<T>(&x)
      Decl* d = e->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(e)->resolved
              : e->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(e)->resolved
                                                : nullptr;
      if (e->kind == NodeKind::CallExpr)
      {
        Expr* callee = static_cast<CallExpr*>(e)->callee;
        Decl* f = callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(callee)->resolved
                : callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                       : nullptr;
        return f && f->kind == NodeKind::FunctionDecl ? variantOfType(static_cast<FunctionDecl*>(f)->returnType) : nullptr;
      }
      if (!d)
        return nullptr;
      if (auto it = variants->byDecl.find(d); it != variants->byDecl.end())
        return &variants->all[it->second]; // a member declared as std::variant<...>
      if (d->kind == NodeKind::VariableDecl)
        return variantOfType(static_cast<VariableDecl*>(d)->type);
      if (d->kind == NodeKind::ParamDecl)
        return variantOfType(static_cast<ParamDecl*>(d)->type);
      return nullptr;
    }

    // `std::get<T>(x)` and its relatives at token i (`std`): recorded with the call's arguments; run()
    // rewrites it when it knows the variant. std::get<0>(tuple) and the like are no variant uses.
    void recordVariantUse(const std::string& name, uint32_t i)
    {
      VariantUse use;
      use.kind = name;
      use.begin = toks[i].offset;
      use.calleeToken = i;
      uint32_t open = i + 3;
      if (name != "visit")
      {
        if (!toks[open].is(P::Less))
          return;
        uint32_t close = open + 1;
        for (int depth = 0; close < toks.size(); ++close)
        {
          if (toks[close].is(P::Less))
            ++depth;
          else if (toks[close].is(P::Greater) || toks[close].is(P::Shr))
          {
            if (depth == 0)
              break;
            depth -= toks[close].is(P::Shr) ? 2 : 1;
            if (depth < 0)
              break;
          }
          else if (toks[close].is(P::Semi))
            return;
        }
        if (close >= toks.size() || close == open + 1)
          return;
        use.typeSpelling = normalizedSpelling(text.substr(toks[open + 1].offset, endOffset(close - 1) - toks[open + 1].offset));
        use.typeBegin = toks[open + 1].offset;
        use.typeEnd = endOffset(close - 1);
        bool anyAlternative = false;
        for (const VariantInfo& v : variants->all)
          anyAlternative = anyAlternative || alternativeFor(v, use.typeSpelling);
        // `std::get<IDType>(this->extraData)` in a template: no alternative is spelled IDType, and
        // the union's generic accessors serve it once the argument's variant is known (run()).
        use.noAlternative = !anyAlternative;
        open = close + 1;
      }
      if (open >= toks.size() || !toks[open].is(P::LParen))
        return;
      uint32_t argStart = open + 1, k = open + 1;
      for (int depth = 0; k < toks.size(); ++k)
      {
        const Token& tk = toks[k];
        if (tk.is(P::LParen) || tk.is(P::LBracket) || tk.is(P::LBrace))
          ++depth;
        else if (tk.is(P::RParen) || tk.is(P::RBracket) || tk.is(P::RBrace))
        {
          if (depth-- == 0)
            break;
        }
        else if (tk.is(P::Comma) && depth == 0)
        {
          use.args.push_back({toks[argStart].offset, endOffset(k - 1)});
          argStart = k + 1;
        }
      }
      if (k >= toks.size() || !toks[k].is(P::RParen) || k == argStart)
        return;
      use.args.push_back({toks[argStart].offset, endOffset(k - 1)});
      use.end = endOffset(k);
      variantUses.push_back(std::move(use));
    }

    // An alternative's type as the union enum lists it. Inside the union, its alternatives' names
    // come first, so a name in the type spelled like one of them (`Patrolling` next to the
    // alternative Patrolling, `EntityWithQualityID` in `Pair<EntityWithQualityID, ...>`) is written
    // qualified: by the innermost scope at offset that declares it, or as global.
    std::string qualifiedAlternativeType(const std::string& type, const VariantInfo& info, uint32_t offset) const
    {
      std::string out;
      for (size_t i = 0; i < type.size();)
      {
        if (!isalpha((unsigned char)type[i]) && type[i] != '_')
        {
          out += type[i++];
          continue;
        }
        size_t e = i;
        while (e < type.size() && isIdentChar(type[e]))
          ++e;
        std::string id = type.substr(i, e - i);
        size_t p = out.size();
        while (p > 0 && out[p - 1] == ' ')
          --p;
        bool laterPart = p >= 2 && out[p - 1] == ':' && out[p - 2] == ':'; // `A::id`: qualified already
        bool collides = false;
        for (const VariantAlternative& a : info.alternatives)
          collides = collides || (!laterPart && a.name == id);
        if (collides)
        {
          std::string scope;
          for (const ScopeRange& r : scopeRanges)
            if (r.begin <= offset && offset < r.end && scopeNames)
              if (auto it = scopeNames->find(r.key); it != scopeNames->end() && it->second.count(id))
                scope = r.key;
          out += "::" + scope;
        }
        out += id;
        i = e;
      }
      return out;
    }

    // The alternative of a variant that a written type names (exactly, or by its last component).
    static const VariantAlternative* alternativeFor(const VariantInfo& info, const std::string& spelling)
    {
      for (const VariantAlternative& a : info.alternatives)
        if (a.spelling == spelling)
          return &a;
      const VariantAlternative* found = nullptr;
      for (const VariantAlternative& a : info.alternatives)
        if (lastComponent(a.spelling) == lastComponent(spelling))
        {
          if (found)
            return nullptr; // two alternatives end in the same name
          found = &a;
        }
      return found;
    }

    static bool containsWord(std::string_view text, std::string_view word)
    {
      for (size_t p = text.find(word); p != std::string_view::npos; p = text.find(word, p + 1))
        if ((p == 0 || !isIdentChar(text[p - 1])) && (p + word.size() == text.size() || !isIdentChar(text[p + word.size()])))
          return true;
      return false;
    }

    static bool convertingConstructor(FunctionDecl* f)
    {
      if (f->params.empty())
        return false;
      auto* first = static_cast<ParamDecl*>(f->params[0]);
      if (first->isPack)
        return false;
      for (size_t i = 1; i < f->params.size(); ++i)
        if (!static_cast<ParamDecl*>(f->params[i])->defaultValue)
          return false;
      // Copy and move constructors are not conversions: `T(const T&)` has the class's own name.
      Type* t = first->type;
      while (t && (t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType || t->kind == NodeKind::ConstType))
        t = t->kind == NodeKind::LRefType ? static_cast<LRefType*>(t)->referenced
          : t->kind == NodeKind::RRefType ? static_cast<RRefType*>(t)->referenced
                                          : static_cast<ConstType*>(t)->inner;
      if (t && t->kind == NodeKind::NamedType && !f->name.empty() && f->name.parts.size() >= 1)
      {
        const QualifiedName& n = static_cast<NamedType*>(t)->name;
        if (!n.empty() && n.last().symbol == f->name.last().symbol)
          return false;
      }
      return true;
    }

    // Removes a whole definition (a function with its body, a class, a global): from the end of
    // whatever declaration precedes it, so that a return type or `template<...>` on an earlier
    // line goes with it.
    void removeDefinition(Decl* d)
    {
      uint32_t start = toks[d->token].offset;
      while (start > 0 && text[start - 1] != ';' && text[start - 1] != '{' && text[start - 1] != '}')
        --start;
      while (start < text.size() && (text[start] == '\n' || text[start] == '\r'))
        ++start;
      uint32_t end = d->endToken > d->token && d->endToken <= toks.size() ? endOffset(d->endToken - 1) : endOffset(d->token);
      // The rest of the line too when nothing else is on it (a `}` closing a namespace stays).
      uint32_t lineEnd = end;
      while (lineEnd < text.size() && (text[lineEnd] == ' ' || text[lineEnd] == '\t' || text[lineEnd] == '\r' || text[lineEnd] == ';'))
        ++lineEnd;
      if (lineEnd < text.size() && text[lineEnd] == '\n')
        end = lineEnd + 1;
      remove(start, end - start);
    }

    // The `)` that closes the `(` at token open, or UINT32_MAX.
    uint32_t closingParenToken(uint32_t open) const
    {
      int depth = 0;
      for (uint32_t k = open; k < toks.size(); ++k)
      {
        if (toks[k].is(P::LParen))
          ++depth;
        else if (toks[k].is(P::RParen) && --depth == 0)
          return k;
        else if (toks[k].getKind() == TokenKind::End)
          break;
      }
      return UINT32_MAX;
    }

    // The `,` or `>` that ends the template parameter starting at token `from`, or UINT32_MAX.
    uint32_t templateParameterEnd(uint32_t from) const
    {
      int depth = 0;
      for (uint32_t k = from; k < toks.size(); ++k)
      {
        const Token& tk = toks[k];
        if (tk.is(P::Less) || tk.is(P::LParen) || tk.is(P::LBracket))
          ++depth;
        else if (tk.is(P::RParen) || tk.is(P::RBracket))
          --depth;
        else if (tk.is(P::Greater) || tk.is(P::Shr))
        {
          if (depth == 0)
            return k;
          depth -= tk.is(P::Shr) ? 2 : 1;
          if (depth < 0)
            return k;
        }
        else if (tk.is(P::Comma) && depth == 0)
          return k;
        else if (tk.is(P::Semi) || tk.is(P::LBrace))
          return UINT32_MAX;
      }
      return UINT32_MAX;
    }

    // Whether d is a template written for any std::variant<T...>: its signature (template header,
    // requires clause, parameters, a partial specialization's arguments) names `std::variant<T...>`
    // with a pack, variant_size_v or variant_alternative_t, or a function's requires clause asks
    // for is_std_variant (not negated).
    bool isGenericVariantCode(Decl* d)
    {
      if (!d->isTemplate || d->templateParams.empty() || (d->kind != NodeKind::FunctionDecl && d->kind != NodeKind::ClassDecl) ||
          d->endToken <= d->token || d->endToken > toks.size())
        return false;
      uint32_t from = d->templateParams[0]->token;
      while (from > 0 && !toks[from].is(Kw::template_) && !toks[from].is(P::Semi) && !toks[from].is(P::RBrace))
        --from;
      uint32_t to = d->endToken;
      for (uint32_t k = d->token; k < d->endToken; ++k)
        if (toks[k].is(P::LBrace) || toks[k].is(P::Semi))
        {
          to = k; // the body (a function's, a class's) or the end of a declaration
          break;
        }
      if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body)
        to = std::min(to, static_cast<FunctionDecl*>(d)->body->token);
      for (uint32_t k = from; k < to; ++k)
      {
        if (toks[k].getKind() != TokenKind::Identifier)
          continue;
        std::string_view s = spelling(k);
        if (s == "variant_size_v" || s == "variant_alternative_t")
          return true;
        if (s == "is_std_variant" && d->kind == NodeKind::FunctionDecl && !(k > 0 && toks[k - 1].is(P::Bang)))
          return true;
        if (s == "variant" && k >= 2 && toks[k - 1].is(P::ColonColon) && spelling(k - 2) == "std" && k + 1 < to && toks[k + 1].is(P::Less))
          for (uint32_t j = k + 2; j < to && !toks[j].is(P::Greater) && !toks[j].is(P::Shr); ++j)
            if (toks[j].is(P::Ellipsis))
              return true;
      }
      return false;
    }

    // A template parameter that the declaration used only in allocator arguments, which convertTokens
    // dropped (`template<typename T, typename Allocate> void eraseIf(std::vector<T, Allocate>& v,
    // Pred pred)`), goes from the header too: nothing could deduce it any more.
    void dropAllocatorParameters(Decl* d)
    {
      size_t n = d->templateParams.size();
      if (droppedAllocators.empty() || n == 0 || d->endToken <= d->token || d->endToken > toks.size())
        return;
      uint32_t headerEnd = templateParameterEnd(d->templateParams[n - 1]->token); // the header's `>`
      if (headerEnd == UINT32_MAX)
        return;
      auto first = droppedAllocators.lower_bound(toks[headerEnd].offset);
      if (first == droppedAllocators.end() || first->first >= endOffset(d->endToken - 1))
        return;
      std::vector<bool> drop(n, false);
      size_t dropped = 0;
      for (size_t p = 0; p < n; ++p)
      {
        auto* tp = static_cast<TemplateParamDecl*>(d->templateParams[p]);
        if (tp->name == UINT32_MAX)
          continue;
        std::string_view name = symbols.name(tp->name);
        bool inAllocator = false, elsewhere = false;
        for (uint32_t k = headerEnd + 1; k < d->endToken && !elsewhere; ++k)
          if (toks[k].getKind() == TokenKind::Identifier && spelling(k) == name)
            (droppedAllocators.count(toks[k].offset) ? inAllocator : elsewhere) = true;
        if (inAllocator && !elsewhere)
        {
          drop[p] = true;
          ++dropped;
        }
      }
      if (dropped == 0)
        return;
      if (dropped == n)
      {
        todo(toks[d->token].offset, "every template parameter was used only in dropped allocator arguments");
        return;
      }
      // A parameter goes with the comma after it; the ones after the last kept go with the comma
      // before them, from the end of the last kept one.
      size_t lastKept = n - 1;
      while (drop[lastKept])
        --lastKept;
      auto paramEnd = [&](size_t p) { return endOffset(templateParameterEnd(d->templateParams[p]->token) - 1); };
      for (size_t p = 0; p < lastKept; ++p)
        if (drop[p])
          remove(toks[d->templateParams[p]->token].offset, toks[d->templateParams[p + 1]->token].offset - toks[d->templateParams[p]->token].offset);
      if (lastKept + 1 < n)
        remove(paramEnd(lastKept), paramEnd(n - 1) - paramEnd(lastKept));
      counts.add("allocator template parameters dropped", (int)dropped);
    }

    void removeDecl(Decl* d)
    {
      uint32_t start = declStart(d);
      uint32_t end = d->endToken > d->token && d->endToken <= toks.size() ? endOffset(d->endToken - 1) : start;
      remove(start, lineEndAfter(end) - start);
    }

    // After offset, the rest of its line when nothing else is on it (to the newline included).
    uint32_t lineEndAfter(uint32_t offset) const
    {
      uint32_t lineEnd = offset;
      while (lineEnd < text.size() && (text[lineEnd] == ' ' || text[lineEnd] == '\t' || text[lineEnd] == '\r'))
        ++lineEnd;
      return lineEnd < text.size() && text[lineEnd] == '\n' ? lineEnd + 1 : offset;
    }

    // Where a declaration starts: its first token is its name; the type and specifiers before it on
    // the same line belong to it as well (forward declarations are one line).
    uint32_t declStart(Decl* d) const
    {
      uint32_t start = toks[d->token].offset;
      while (start > 0 && text[start - 1] != '\n' && text[start - 1] != ';' && text[start - 1] != '{' && text[start - 1] != '}')
        --start;
      // A `template<...>` header or `[[attributes]]` alone on the lines above belong to it too
      // (`template<size_t N2> friend class ImmutableString;` written on two lines).
      for (;;)
      {
        if (start == 0 || text[start - 1] != '\n')
          break;
        uint32_t prevEnd = start - 1;
        uint32_t prevStart = prevEnd;
        while (prevStart > 0 && text[prevStart - 1] != '\n')
          --prevStart;
        std::string_view prev = text.substr(prevStart, prevEnd - prevStart);
        while (!prev.empty() && isspace((unsigned char)prev.front()))
          prev.remove_prefix(1);
        while (!prev.empty() && isspace((unsigned char)prev.back()))
          prev.remove_suffix(1);
        bool header = prev.size() > 8 && prev.substr(0, 8) == "template" && prev.back() == '>';
        bool attribute = prev.size() > 4 && prev.substr(0, 2) == "[[" && prev.substr(prev.size() - 2) == "]]";
        if (!header && !attribute)
          break;
        start = prevStart;
      }
      return start;
    }

    // --- bodies: this-> and fallthrough ---

    // Whether variable v, whose parent is a class, is one of the class's data members. A local of
    // a lambda in a constructor's member initializers has the class as its parent too (the
    // initializers are parsed in class scope) and is no member: `: ScopedTypeMigration([&] {
    // PropertyTree result; ...; return result; }(), ...)` keeps `result` as it is.
    std::map<Decl*, std::set<Decl*>> dataMemberCache;
    bool isDataMember(Decl* v)
    {
      auto it = dataMemberCache.find(v->parent);
      if (it == dataMemberCache.end())
      {
        std::set<Decl*> members;
        for (Decl* m : static_cast<ClassDecl*>(v->parent)->members)
          if (m->kind == NodeKind::VariableDecl)
            members.insert(m);
        it = dataMemberCache.emplace(v->parent, std::move(members)).first;
      }
      return it->second.count(v) != 0;
    }

    // Non-static data members and methods of a class and its bases, by name.
    std::map<Decl*, std::set<uint32_t>> memberNameCache;
    const std::set<uint32_t>& memberNames(Decl* cls)
    {
      auto it = memberNameCache.find(cls);
      if (it != memberNameCache.end())
        return it->second;
      std::set<uint32_t>& names = memberNameCache[cls];
      std::function<void(Decl*, int)> collect = [&](Decl* d, int depth)
      {
        Decl* def = global.definitionOf(d);
        if (!def || def->kind != NodeKind::ClassDecl || depth > 8)
          return;
        auto* c = static_cast<ClassDecl*>(def);
        for (Decl* m : c->members)
        {
          if (m->flags & DF_Static)
            continue;
          if (m->kind == NodeKind::VariableDecl && !(m->flags & DF_Typedef) && static_cast<VariableDecl*>(m)->name.parts.size() == 1)
            names.insert(static_cast<VariableDecl*>(m)->name.parts[0].symbol);
          else if (m->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(m)->name.parts.size() == 1 &&
                   static_cast<FunctionDecl*>(m)->name.parts[0].special == 0)
            names.insert(static_cast<FunctionDecl*>(m)->name.parts[0].symbol);
        }
        for (const ClassDecl::Base& b : c->bases)
          if (b.type && b.type->kind == NodeKind::NamedType)
          {
            const Ty* bt = global.typeOfAst(b.type, c);
            if (bt && bt->decl)
              collect(bt->decl, depth + 1);
          }
      };
      collect(cls, 0);
      return names;
    }

    static bool terminates(Stmt* s)
    {
      if (!s)
        return false;
      switch (s->kind)
      {
        case NodeKind::ReturnStmt: case NodeKind::BreakStmt: case NodeKind::ContinueStmt: case NodeKind::GotoStmt:
        case NodeKind::FallthroughStmt:
          return true;
        case NodeKind::ExprStmt:
        {
          Expr* e = static_cast<ExprStmt*>(s)->expr;
          return e && e->kind == NodeKind::ThrowExpr;
        }
        case NodeKind::BlockStmt:
        {
          auto* b = static_cast<BlockStmt*>(s);
          return !b->stmts.empty() && terminates(b->stmts[b->stmts.size() - 1]);
        }
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          return i->else_ && terminates(i->then) && terminates(i->else_);
        }
        default: return false;
      }
    }

    void walkSwitchBody(Stmt* body, Decl* owner)
    {
      if (!body || body->kind != NodeKind::BlockStmt)
      {
        walkStmt(body, owner);
        return;
      }
      auto* b = static_cast<BlockStmt*>(body);
      Stmt* last = nullptr; // last statement since the previous label
      for (Stmt* s : b->stmts)
      {
        if (s->kind == NodeKind::CaseStmt || s->kind == NodeKind::DefaultStmt)
        {
          if (last && !terminates(last))
          {
            // Indent like the label.
            uint32_t at = toks[s->token].offset;
            uint32_t lineStart = at;
            while (lineStart > 0 && text[lineStart - 1] != '\n')
              --lineStart;
            insert(at, "fallthrough;\n" + std::string(text.substr(lineStart, at - lineStart)));
            counts.add("fallthrough added");
          }
          last = nullptr;
          if (s->kind == NodeKind::CaseStmt)
            walkExpr(static_cast<CaseStmt*>(s)->value, owner);
          continue;
        }
        walkStmt(s, owner);
        last = s;
      }
    }

    void walkStmt(Stmt* s, Decl* owner)
    {
      if (!s)
        return;
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            walkStmt(inner, owner);
          break;
        case NodeKind::ExprStmt: walkExpr(static_cast<ExprStmt*>(s)->expr, owner); break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
            if (d->kind == NodeKind::VariableDecl)
              walkExpr(static_cast<VariableDecl*>(d)->init, owner);
          break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          walkStmt(i->init, owner);
          walkNode(i->cond, owner);
          walkStmt(i->then, owner);
          walkStmt(i->else_, owner);
          break;
        }
        case NodeKind::AssertStmt: walkExpr(static_cast<AssertStmt*>(s)->cond, owner); break;
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          walkStmt(f->init, owner);
          walkNode(f->cond, owner);
          walkExpr(f->step, owner);
          walkStmt(f->body, owner);
          break;
        }
        case NodeKind::RangeForStmt:
        {
          auto* f = static_cast<RangeForStmt*>(s);
          rangeForRanges[f->var] = f->range;
          walkExpr(f->range, owner);
          walkStmt(f->body, owner);
          break;
        }
        case NodeKind::WhileStmt:
          walkNode(static_cast<WhileStmt*>(s)->cond, owner);
          walkStmt(static_cast<WhileStmt*>(s)->body, owner);
          break;
        case NodeKind::DoStmt:
          walkStmt(static_cast<DoStmt*>(s)->body, owner);
          walkExpr(static_cast<DoStmt*>(s)->cond, owner);
          break;
        case NodeKind::SwitchStmt:
        {
          auto* sw = static_cast<SwitchStmt*>(s);
          walkStmt(sw->init, owner);
          walkNode(sw->cond, owner);
          walkSwitchBody(sw->body, owner);
          break;
        }
        case NodeKind::CaseStmt: walkExpr(static_cast<CaseStmt*>(s)->value, owner); break;
        case NodeKind::ReturnStmt: walkExpr(static_cast<ReturnStmt*>(s)->value, owner); break;
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(s);
          walkStmt(t->body, owner);
          for (const TryStmt::Handler& h : t->handlers)
            walkStmt(h.body, owner);
          break;
        }
        default: break;
      }
    }

    void walkNode(Node* n, Decl* owner)
    {
      if (!n)
        return;
      if (n->kind == NodeKind::DeclStmt || n->kind == NodeKind::VariableDecl)
      {
        if (n->kind == NodeKind::VariableDecl)
          walkExpr(static_cast<VariableDecl*>(n)->init, owner);
        else
          walkStmt(static_cast<Stmt*>(n), owner);
        return;
      }
      walkExpr(static_cast<Expr*>(n), owner);
    }

    // The first token of an expression: a postfix node's own token is its operator (`.`, `(`, `[`).
    static uint32_t firstTokenOf(Expr* e)
    {
      for (;;)
      {
        if (e->kind == NodeKind::MemberExpr)
          e = static_cast<MemberExpr*>(e)->base;
        else if (e->kind == NodeKind::CallExpr)
          e = static_cast<CallExpr*>(e)->callee;
        else if (e->kind == NodeKind::IndexExpr)
          e = static_cast<IndexExpr*>(e)->base;
        else if (e->kind == NodeKind::UnaryExpr && static_cast<UnaryExpr*>(e)->postfix)
          e = static_cast<UnaryExpr*>(e)->operand;
        else
          return e->token;
      }
    }
    void walkExprs(Span<Expr*> es, Decl* owner)
    {
      for (Expr* e : es)
        walkExpr(e, owner);
    }

    void walkExpr(Expr* e, Decl* owner)
    {
      if (!e)
        return;
      switch (e->kind)
      {
        case NodeKind::NameExpr:
        {
          auto* n = static_cast<NameExpr*>(e);
          Decl* r = n->resolved;
          if (!r && n->name.parts.size() == 1 && !n->name.global && n->name.parts[0].symbol != UINT32_MAX &&
              symbols.name(n->name.parts[0].symbol) == "_" && nameUnderscoreUse(n->token))
            break;
          if (owner && n->name.parts.size() == 1 && !n->name.global && n->name.parts[0].special == 0)
          {
            bool member = false;
            if (r)
              member = r->parent && r->parent->kind == NodeKind::ClassDecl &&
                       (r->kind == NodeKind::FunctionDecl || (r->kind == NodeKind::VariableDecl && !(r->flags & DF_Typedef) && isDataMember(r))) &&
                       !(r->flags & DF_Static);
            else
              member = memberNames(owner).count(n->name.parts[0].symbol) != 0; // unresolved, but spelled like a member
            if (member)
            {
              insert(toks[n->token].offset, "this->");
              counts.add("this-> added");
            }
          }
          break;
        }
        case NodeKind::CallExpr:
        {
          auto* call = static_cast<CallExpr*>(e);
          if (variants && call->callee->kind == NodeKind::NameExpr && !call->args.empty())
          {
            // std::get<T>(x), std::visit(f, x)...: which variant x is declared as, for the rewrite in run().
            const QualifiedName& n = static_cast<NameExpr*>(call->callee)->name;
            if (n.parts.size() == 2 && n.parts[0].symbol != UINT32_MAX && n.parts[1].symbol != UINT32_MAX && symbols.name(n.parts[0].symbol) == "std")
            {
              std::string_view f = symbols.name(n.parts[1].symbol);
              Expr* variantArg = f == "visit" && call->args.size() >= 2 ? call->args[1]
                               : (f == "get" || f == "holds_alternative" || f == "get_if") ? call->args[0]
                                                                                            : nullptr;
              if (const VariantInfo* info = variantOfExpr(variantArg))
                callVariant[call->callee->token] = info;
            }
          }
          for (Expr* arg : call->args)
            if (isGuidVariable(arg))
            {
              // `object->SetPrivateData(WKPDID_D3DDebugObjectName, ...)`, `FC->CanConvert(pixelFormat,
              // convertGUID, ...)`: by address, as the class and interface ids are below.
              insert(toks[arg->token].offset, "&");
              counts.add("GUIDs passed by address");
            }
          // `it.operator*()` and `it.operator->()` on an iterator: a std::vector's is a class in the
          // game's library and a pointer in Kpp's, which has no member call for them (Agui's
          // visible_iterator), so they are written `(*it)` and `toAddress(it)`, which C++ gives the
          // same meaning for an iterator. Only where the declarations say a std iterator: any other
          // keeps the member call (PrototypeIDWithQualityIterator's `this->idIterator.operator->()`
          // calls PrototypeIDIterator's non-const one, which toAddress's const parameter would
          // not). The opening goes in at the base's first token before the
          // base's own edits there (`this->`); a postfix node's token is its operator, the `.`.
          if (call->args.empty() && call->callee->kind == NodeKind::MemberExpr && static_cast<MemberExpr*>(call->callee)->member.parts.size() == 1)
          {
            auto* m = static_cast<MemberExpr*>(call->callee);
            const NamePart& name = m->member.parts[0];
            bool star = name.opToken == uint32_t(P::Star), arrow = name.opToken == uint32_t(P::Arrow);
            uint32_t dot = m->token;
            if (m->op == P::Dot && name.special == 2 && (star || arrow) && dot + 4 < toks.size() &&
                toks[dot + 3].is(P::LParen) && toks[dot + 4].is(P::RParen) && isStdIteratorType(typeOfExpr(m->base, 0).type, owner))
            {
              insert(toks[firstTokenOf(m->base)].offset, star ? "(*" : "toAddress(");
              replace(toks[dot].offset, toks[dot + 4].offset + 1 - toks[dot].offset, ")");
              counts.add(star ? "explicit operator*() calls written *x" : "explicit operator->() calls written toAddress(x)");
            }
          }
          walkExpr(call->callee, owner);
          walkExprs(call->args, owner);
          break;
        }
        case NodeKind::IndexExpr:
          walkExpr(static_cast<IndexExpr*>(e)->base, owner);
          walkExprs(static_cast<IndexExpr*>(e)->args, owner);
          break;
        case NodeKind::MemberExpr: walkExpr(static_cast<MemberExpr*>(e)->base, owner); break;
        case NodeKind::UnaryExpr: walkExpr(static_cast<UnaryExpr*>(e)->operand, owner); break;
        case NodeKind::BinaryExpr:
        {
          auto* binary = static_cast<BinaryExpr*>(e);
          if (P(binary->op) == P::Shl)
            sizeAsSizeT(binary->rhs);
          walkExpr(binary->lhs, owner);
          walkExpr(binary->rhs, owner);
          break;
        }
        case NodeKind::ConditionalExpr:
          walkExpr(static_cast<ConditionalExpr*>(e)->cond, owner);
          walkExpr(static_cast<ConditionalExpr*>(e)->then, owner);
          walkExpr(static_cast<ConditionalExpr*>(e)->else_, owner);
          break;
        case NodeKind::CastExpr: walkExpr(static_cast<CastExpr*>(e)->operand, owner); break;
        case NodeKind::NamedCastExpr: walkExpr(static_cast<NamedCastExpr*>(e)->operand, owner); break;
        case NodeKind::SizeofExpr: walkExpr(static_cast<SizeofExpr*>(e)->operand, owner); break;
        case NodeKind::NameofExpr: walkExpr(static_cast<NameofExpr*>(e)->operand, owner); break;
        case NodeKind::NewExpr:
          walkExprs(static_cast<NewExpr*>(e)->placement, owner);
          walkExprs(static_cast<NewExpr*>(e)->args, owner);
          break;
        case NodeKind::DeleteExpr: walkExpr(static_cast<DeleteExpr*>(e)->operand, owner); break;
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(e);
          for (const LambdaCapture& c : l->captures)
            walkExpr(c.init, owner);
          walkStmt(l->body, owner);
          break;
        }
        case NodeKind::BraceInitExpr: walkExprs(static_cast<BraceInitExpr*>(e)->args, owner); break;
        case NodeKind::ThrowExpr: walkExpr(static_cast<ThrowExpr*>(e)->operand, owner); break;
        case NodeKind::FunctionalCastExpr: walkExprs(static_cast<FunctionalCastExpr*>(e)->args, owner); break;
        case NodeKind::FoldExpr:
          walkExpr(static_cast<FoldExpr*>(e)->lhs, owner);
          walkExpr(static_cast<FoldExpr*>(e)->rhs, owner);
          break;
        case NodeKind::ParenExpr: walkExpr(static_cast<ParenExpr*>(e)->inner, owner); break;
        default: break;
      }
    }

    // --- putting it together ---

    // Definitions of overridden functions the parser did not see as functions, found by text:
    // `... name(` at the start of a line with a `{` before the next `;`, through the body's end.
    void removeUnparsedDefinitions()
    {
      if (!overrides)
        return;
      for (const auto& [qualified, count] : overrides->functionCounts)
      {
        if (count != 1 || replacedFunctions.count(qualified))
          continue;
        std::string name = qualified.substr(qualified.rfind(':') == std::string::npos ? 0 : qualified.rfind(':') + 1);
        for (size_t p = text.find(name); p != std::string::npos; p = text.find(name, p + 1))
        {
          if (p > 0 && (isalnum((unsigned char)text[p - 1]) || text[p - 1] == '_' || text[p - 1] == ':' || text[p - 1] == '.'))
            continue;
          size_t q = p + name.size();
          while (q < text.size() && text[q] == ' ')
            ++q;
          if (q >= text.size() || text[q] != '(')
            continue;
          size_t lineStart = text.rfind('\n', p);
          lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
          std::string_view head = text.substr(lineStart, p - lineStart);
          if (head.find_first_not_of(" ") == std::string_view::npos || head.find(';') != std::string_view::npos || head.find("//") != std::string_view::npos)
            continue; // not a declaration line
          size_t brace = text.find('{', q), semi = text.find(';', q);
          if (brace == std::string::npos || (semi != std::string::npos && semi < brace))
            continue; // a prototype
          int depth = 0;
          size_t end = brace;
          for (; end < text.size(); ++end)
          {
            if (text[end] == '{')
              ++depth;
            else if (text[end] == '}' && --depth == 0)
              break;
          }
          if (end >= text.size())
            break;
          ++end;
          while (end < text.size() && (text[end] == ' ' || text[end] == '\r'))
            ++end;
          if (end < text.size() && text[end] == '\n')
            ++end;
          remove(uint32_t(lineStart), uint32_t(end - lineStart));
          counts.add("replaced by the override file (by text)");
          replacedFunctions.insert(qualified);
          break;
        }
      }
      // Variables likewise (a SUITE's `const char* address = "...";`): `... name =` (or `{`, `;`)
      // after a type on the line, through the `;` that ends it.
      for (const std::string& qualified : overrides->variables)
      {
        if (replacedVariables.count(qualified))
          continue;
        std::string name = qualified.substr(qualified.rfind(':') == std::string::npos ? 0 : qualified.rfind(':') + 1);
        for (size_t p = text.find(name); p != std::string::npos; p = text.find(name, p + 1))
        {
          if (p > 0 && (isalnum((unsigned char)text[p - 1]) || text[p - 1] == '_' || text[p - 1] == ':' || text[p - 1] == '.' || text[p - 1] == '>'))
            continue;
          size_t q = p + name.size();
          while (q < text.size() && text[q] == ' ')
            ++q;
          if (q >= text.size() || !(text[q] == '{' || text[q] == ';' || (text[q] == '=' && (q + 1 >= text.size() || text[q + 1] != '='))))
            continue;
          size_t lineStart = text.rfind('\n', p);
          lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
          std::string_view head = text.substr(lineStart, p - lineStart);
          if (head.find_first_not_of(" ") == std::string_view::npos || head.find_first_of(";(=,") != std::string_view::npos || head.find("//") != std::string_view::npos ||
              head.find("return") != std::string_view::npos)
            continue; // not a declaration line
          int depth = 0;
          size_t end = q;
          for (; end < text.size(); ++end)
          {
            if (text[end] == '{' || text[end] == '(')
              ++depth;
            else if (text[end] == '}' || text[end] == ')')
              --depth;
            else if (text[end] == ';' && depth == 0)
              break;
          }
          if (end >= text.size())
            break;
          ++end;
          while (end < text.size() && (text[end] == ' ' || text[end] == '\r'))
            ++end;
          if (end < text.size() && text[end] == '\n')
            ++end;
          remove(uint32_t(lineStart), uint32_t(end - lineStart));
          counts.add("replaced by the override file (by text)");
          replacedVariables.insert(qualified);
          break;
        }
      }
    }

    // text[begin, end) with the edits inside it applied; edits must be sorted as run() sorts them.
    // An insertion at end belongs to what follows, unless end is the end of the file.
    // The text [begin, end) with the edits inside it; an insertion at end goes with the next text,
    // or with this one when it is a macro's argument (keepEnd: what is inserted after its last token).
    std::string applyEdits(const std::vector<Edit>& sorted, uint32_t begin, uint32_t end, bool keepEnd = false) const
    {
      std::string out;
      uint32_t pos = begin;
      for (const Edit& e : sorted)
      {
        if (e.offset < begin || e.offset + e.length > end || (!keepEnd && e.length == 0 && e.offset == end && end < text.size()))
          continue;
        if (e.offset < pos)
        {
          // Inside an earlier removal or replacement: an insertion is dropped, a longer removal extends.
          if (e.length && e.offset + e.length > pos)
          {
            pos = e.offset + e.length;
            out += e.text;
          }
          continue;
        }
        out.append(text.substr(pos, e.offset - pos));
        out += e.text;
        pos = e.offset + e.length;
      }
      out.append(text.substr(std::min<size_t>(pos, end), end - std::min<size_t>(pos, end)));
      return out;
    }

    // What the configuration took out of the parsed text (blankForParse) gets its replacement:
    // `__try` is try, `__except (filter)` catch (...) (the filter is lost: a todo), the converter's
    // empty macros go.
    void convertBlanked()
    {
      if (!preprocessed)
        return;
      for (const Preprocessed::Blank& b : preprocessed->blanked)
      {
        replace(b.offset, b.length, b.replacement);
        if (b.replacement == "try")
          counts.add("SEH __try to try");
        else if (b.replacement == "catch (...)")
        {
          todo(b.offset, "SEH filter dropped: " + std::string(text.substr(b.offset, b.length)));
          counts.add("SEH __except to catch (...)");
        }
        else
          counts.add("macro use rewritten");
      }
    }

    std::string run()
    {
      collectScopeRanges(in.parsed->decls, "");
      collectLocals(in.parsed->decls);
      collectDirectiveRanges();
      seedHeaderMacros();
      convertPreprocessor();
      convertBlanked();
      convertTokens();
      convertDecls(in.parsed->decls, true);
      removeUnparsedDefinitions();
      // By offset; at the same offset the longest first, so that a removal cancels the edits inside it.
      auto byOffset = [](const Edit& a, const Edit& b) { return a.offset != b.offset ? a.offset < b.offset : a.length > b.length; };
      std::stable_sort(edits.begin(), edits.end(), byOffset);
      // Local macros, now that their arguments' rewrites are known: each use becomes one replacement,
      // which cancels the edits inside it; the innermost first, as a use inside another's argument
      // is part of that argument. (The uses are in source order: one inside another comes after it.)
      if (!localMacroUses.empty())
      {
        std::vector<int> depth(localMacroUses.size(), 0);
        std::vector<size_t> open;
        int deepest = 0;
        for (size_t u = 0; u < localMacroUses.size(); ++u)
        {
          while (!open.empty() && localMacroUses[open.back()].end <= localMacroUses[u].begin)
            open.pop_back();
          depth[u] = int(open.size());
          deepest = std::max(deepest, depth[u]);
          open.push_back(u);
        }
        for (int level = deepest; level >= 0; --level)
        {
          for (size_t u = 0; u < localMacroUses.size(); ++u)
            if (depth[u] == level)
            {
              const LocalMacroUse& use = localMacroUses[u];
              std::vector<std::string> args, spellings;
              for (auto [b, e] : use.args)
              {
                args.push_back(applyEdits(edits, b, e, true));
                spellings.push_back(std::string(text.substr(b, e - b)));
              }
              edits.push_back({use.begin, use.end - use.begin, expandMacro(*use.macro, args, use.begin, &spellings)});
            }
          std::stable_sort(edits.begin(), edits.end(), byOffset);
        }
      }
      // Argument macros, now that their arguments' rewrites (this->, renames) are known: each use
      // becomes one replacement, which cancels the edits inside it.
      if (!argumentMacroUses.empty())
      {
        for (const ArgumentMacroUse& use : argumentMacroUses)
        {
          std::vector<std::string> args, spellings;
          for (auto [b, e] : use.args)
          {
            args.push_back(applyEdits(edits, b, e));
            spellings.push_back(std::string(text.substr(b, e - b)));
          }
          edits.push_back({use.begin, use.end - use.begin, expandArguments(*use.replacement, args, spellings)});
        }
        std::stable_sort(edits.begin(), edits.end(), byOffset);
      }
      // std::variant declarations and uses, now that the edits inside their types and arguments are known.
      if (!variantDeclarations.empty() || !variantUses.empty())
      {
        for (const VariantDeclaration& v : variantDeclarations)
        {
          size_t lineStart = v.memberStart;
          std::string indent;
          for (size_t p = lineStart; p < text.size() && (text[p] == ' ' || text[p] == '\t'); ++p)
            indent += text[p];
          std::string list;
          for (size_t a = 0; a < v.args.size() && a < v.info->alternatives.size(); ++a)
          {
            std::string type = applyEdits(edits, v.args[a].first, v.args[a].second);
            type = qualifiedAlternativeType(type, *v.info, v.memberStart);
            list += std::string(list.empty() ? "" : ", ") + "{" + type + ", " + v.info->alternatives[a].name + "}";
          }
          // std::variant's ==, the same alternative with equal values; a template, so that as for
          // std::variant an alternative without == matters only where two unions are compared.
          const std::string& u = v.info->unionName;
          std::string equality = indent + "  template<typename Other> requires (isSame<Other, " + u + ">)\n" + indent +
                                 "  bool operator==(const Other& other) const\n" + indent + "  {\n" + indent + "    for constexpr (auto a : alternatives(" + u +
                                 "))\n" + indent + "      if (*this == a.tag)\n" + indent + "        return other == a.tag && (*this)[a] == other[a];\n" + indent +
                                 "    return false;\n" + indent + "  }\n";
          // std::get<T>, std::holds_alternative<T> and std::get_if<T> for a T only an instantiation
          // knows (a template's parameter): the alternative of type T, by the union's alternatives.
          std::string loop = "for constexpr (auto a : alternatives(" + u + ")) if constexpr (isSame<a.type, T>) ";
          std::string generic = indent + "  template<typename T>\n" + indent + "  T& get() { " + loop + "return (*this)[a]; }\n" + indent +
                                "  template<typename T>\n" + indent + "  const T& get() const { " + loop + "return (*this)[a]; }\n" + indent +
                                "  template<typename T>\n" + indent + "  bool holds() const { " + loop + "return *this == a.tag; return false; }\n" + indent +
                                "  template<typename T>\n" + indent + "  T* getIf() { " + loop + "return *this == a.tag ? &(*this)[a] : nullptr; return nullptr; }\n" +
                                indent + "  template<typename T>\n" + indent + "  const T* getIf() const { " + loop +
                                "return *this == a.tag ? &(*this)[a] : nullptr; return nullptr; }\n" +
                                // std::variant's emplace<T>(args...) and index().
                                indent + "  template<typename T, typename... Args>\n" + indent + "  T& emplace(Args&&... args) { " + loop +
                                "{ (*this)[a] = T(forward<Args>(args)...); return (*this)[a]; } }\n" + indent +
                                "  int index() const { return int(this->tag()); }\n";
          std::string declaration = indent + "union enum " + u + "\n" + indent + "{\n" + indent + "  { " + list + " }\n" + equality + generic + indent + "};";
          if (v.member)
          {
            edits.push_back({v.memberStart, 0, declaration + "\n"});
            edits.push_back({v.begin, v.end - v.begin, v.info->unionName});
          }
          else
            edits.push_back({v.begin, v.end - v.begin, declaration});
        }
        for (const VariantUse& use : variantUses)
        {
          auto known = callVariant.find(use.calleeToken);
          const VariantInfo* info = known != callVariant.end() ? known->second : nullptr;
          if (use.noAlternative)
          {
            // T is no alternative's spelling: through the union's generic accessors, when the
            // argument is known to be one of the converted variants; left as written otherwise
            // (std::get<T> of something else).
            if (!info || use.args.empty() || use.typeSpelling.empty() || isdigit((unsigned char)use.typeSpelling[0]))
              continue;
            std::string arg = applyEdits(edits, use.args[0].first, use.args[0].second);
            std::string type = applyEdits(edits, use.typeBegin, use.typeEnd);
            std::string replacement = use.kind == "get"                 ? "(" + arg + ").template get<" + type + ">()"
                                    : use.kind == "holds_alternative" ? "(" + arg + ").template holds<" + type + ">()"
                                                                      : "((" + arg + ") ? (" + arg + ")->template getIf<" + type + ">() : nullptr)";
            edits.push_back({use.begin, use.end - use.begin, replacement});
            counts.add("std::" + use.kind + " of a template's type on a union enum");
            continue;
          }
          if (!info && use.kind != "visit")
            for (const VariantInfo& v : variants->all)
              if (alternativeFor(v, use.typeSpelling))
              {
                if (info)
                {
                  info = nullptr; // several variants have this alternative: the argument's declaration has to tell
                  break;
                }
                info = &v;
              }
          const VariantAlternative* alternative = info && use.kind != "visit" ? alternativeFor(*info, use.typeSpelling) : nullptr;
          if (!info || (use.kind != "visit" && !alternative) || (use.kind == "visit" && use.args.size() != 2))
          {
            todo(use.begin, "std::" + use.kind + " on a variant the converter could not tell: left as written");
            continue;
          }
          std::vector<std::string> args;
          for (auto [b, e] : use.args)
            args.push_back(applyEdits(edits, b, e));
          std::string u = info->qualified + "::";
          std::string replacement;
          if (use.kind == "holds_alternative")
            replacement = "((" + args[0] + ") == " + u + alternative->name + ")";
          else if (use.kind == "get")
            replacement = "(" + args[0] + ")[" + u + alternative->name + "]";
          else if (use.kind == "get_if")
            replacement = "(*(" + args[0] + ") == " + u + alternative->name + " ? &(*(" + args[0] + "))[" + u + alternative->name + "] : nullptr)";
          else
          {
            // std::visit(f, x): f called with the alternative x holds, by a switch over the alternatives.
            std::string cases;
            for (const VariantAlternative& a : info->alternatives)
              cases += "case " + u + a.name + ": return visitor(value[" + u + a.name + "]); ";
            // (The visitor is held by value: `auto&&` bound to a lambda does not compile yet, spec
            // language/forwarding_reference_to_lambda.)
            replacement = "[&]() { auto visitor = " + args[0] + "; auto&& value = " + args[1] + "; switch (value) { " + cases + "} }()";
          }
          edits.push_back({use.begin, use.end - use.begin, replacement});
          counts.add("std::" + use.kind + " on a union enum");
        }
        std::stable_sort(edits.begin(), edits.end(), byOffset);
      }
      // Hidden friends that move out of their class, likewise with the edits inside them.
      if (!movedFriends.empty())
      {
        for (const MovedFriend& m : movedFriends)
        {
          std::string rendered = applyEdits(edits, m.begin, m.end);
          rendered.erase(0, rendered.find_first_not_of(" \t"));
          // The class-scope names, where nothing qualifies them already (`::`, `.`, `->` before).
          std::string moved;
          for (size_t i = 0; i < rendered.size();)
          {
            char c = rendered[i];
            if (c == '"' || (c == '\'' && !isDigitSeparator(rendered, i)))
            {
              size_t j = i + 1;
              while (j < rendered.size() && rendered[j] != c)
                j += rendered[j] == '\\' ? 2 : 1;
              moved.append(rendered, i, j + 1 - i);
              i = j + 1;
              continue;
            }
            if (isIdentChar(c) && (i == 0 || !isIdentChar(rendered[i - 1])))
            {
              size_t j = i;
              while (j < rendered.size() && isIdentChar(rendered[j]))
                ++j;
              auto q = m.qualify.find(rendered.substr(i, j - i));
              bool qualifiedAlready = i >= 1 && (rendered[i - 1] == ':' || rendered[i - 1] == '.' || rendered[i - 1] == '>');
              moved += q != m.qualify.end() && !qualifiedAlready ? q->second : rendered.substr(i, j - i);
              i = j;
              continue;
            }
            moved += c;
            ++i;
          }
          edits.push_back({m.begin, m.removeEnd - m.begin, ""});
          edits.push_back({m.at, 0, "\n" + m.header + moved});
        }
        std::stable_sort(edits.begin(), edits.end(), byOffset);
      }
      std::string out = applyEdits(edits, 0, uint32_t(text.size()));
      // At most two consecutive blank lines where includes and directives used to be.
      std::string squeezed;
      squeezed.reserve(out.size());
      int blank = 0;
      size_t lineStart = 0;
      for (size_t i = 0; i <= out.size(); ++i)
      {
        if (i == out.size() || out[i] == '\n')
        {
          std::string_view line(out.data() + lineStart, i - lineStart);
          bool isBlank = line.find_first_not_of(" \t\r") == std::string_view::npos;
          blank = isBlank ? blank + 1 : 0;
          if (blank <= 2)
          {
            squeezed.append(line);
            if (i < out.size())
              squeezed += '\n';
          }
          lineStart = i + 1;
        }
      }
      // `std::name` in text the parser never tokenized (the other branch of an #if, a macro's
      // expansion, where an X-macro list's `PREFIX TYPE` makes `std:: string`): the same renames
      // by text.
      for (size_t p = squeezed.find("std::"); p != std::string::npos; p = squeezed.find("std::", p + 1))
      {
        if (p > 0 && (isalnum((unsigned char)squeezed[p - 1]) || squeezed[p - 1] == '_'))
          continue;
        size_t s = p + 5;
        while (s < squeezed.size() && (squeezed[s] == ' ' || squeezed[s] == '\t'))
          ++s;
        size_t e = s;
        while (e < squeezed.size() && (isalnum((unsigned char)squeezed[e]) || squeezed[e] == '_'))
          ++e;
        auto it = stdNames.find(squeezed.substr(s, e - s));
        if (it != stdNames.end())
        {
          squeezed.replace(p, e - p, it->second);
          counts.add("std renamed");
        }
      }
      return squeezed;
    }
  };

  std::string relativeTo(const std::filesystem::path& p, const std::vector<std::filesystem::path>& roots)
  {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec).lexically_normal();
    for (const auto& root : roots)
    {
      std::filesystem::path r = std::filesystem::absolute(root, ec).lexically_normal();
      if (abs == r && r.has_parent_path())
        return (r.parent_path().filename() / r.filename()).generic_string(); // a file given alone keeps its directory (Lua/LuaCPPUtilities.hpp)
      std::filesystem::path rel = abs.lexically_relative(r);
      if (!rel.empty() && rel.native()[0] != '.')
        return (r.filename() / rel).generic_string();
    }
    return abs.filename().generic_string();
  }
}

// A path as a key: absolute, normalised, lower case (Windows paths ignore case).
static std::string pathKeyOf(const std::filesystem::path& p)
{
  std::error_code ec;
  std::string s = std::filesystem::absolute(p, ec).lexically_normal().generic_string();
  for (char& c : s)
    c = (char)tolower((unsigned char)c);
  return s;
}

// Takes out of a file's configured text, for the parse, what is no C++ to the parser and what the
// conversion writes differently, recording each in `blanked` with its replacement (offsets stay):
// the empty macros the converter drops (BEGIN_SUPPRESS_CONVERSIONS before a definition, FAR in a
// cast; one used as a call with its arguments, FORMAT_ATTR(4, 5)), FORMAT_STRING( and its `)`
// around a parameter, and
// MSVC's structured exception handling, `__try` (try) and `__except (filter)` (catch (...)), which
// the parse reads as two blocks. Comments, literals and preprocessor lines are left alone.
static void blankForParse(Preprocessed& p)
{
  std::string& t = p.configured;
  auto blank = [&](size_t from, size_t to, std::string replacement)
  {
    for (size_t k = from; k < to; ++k)
      if (t[k] != '\n' && t[k] != '\r')
        t[k] = ' ';
    p.blanked.push_back({uint32_t(from), uint32_t(to - from), std::move(replacement)});
  };
  // The `)` that closes the `(` at open, or npos.
  auto closing = [&](size_t open)
  {
    int depth = 0;
    for (size_t k = open; k < t.size(); ++k)
    {
      if (t[k] == '"' || (t[k] == '\'' && !isDigitSeparator(t, k)))
      {
        char quote = t[k];
        for (++k; k < t.size() && t[k] != quote; ++k)
          k += t[k] == '\\';
      }
      else if (t[k] == '(')
        ++depth;
      else if (t[k] == ')' && --depth == 0)
        return k;
    }
    return std::string::npos;
  };
  bool lineStart = true;
  for (size_t i = 0; i < t.size();)
  {
    char c = t[i];
    if (c == '\n')
    {
      lineStart = true;
      ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r')
    {
      ++i;
      continue;
    }
    if (lineStart && c == '#')
    {
      // A preprocessor line, continuations included.
      while (i < t.size() && t[i] != '\n')
        i += t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '\n' ? 2 : 1;
      continue;
    }
    lineStart = false;
    if (c == '/' && i + 1 < t.size() && t[i + 1] == '/')
    {
      while (i < t.size() && t[i] != '\n')
        ++i;
      continue;
    }
    if (c == '/' && i + 1 < t.size() && t[i + 1] == '*')
    {
      size_t end = t.find("*/", i + 2);
      i = end == std::string::npos ? t.size() : end + 2;
      continue;
    }
    if (c == 'R' && i + 1 < t.size() && t[i + 1] == '"' && (i == 0 || !isIdentChar(t[i - 1])))
    {
      // A raw string: R"delimiter( ... )delimiter".
      size_t open = t.find('(', i + 2);
      if (open != std::string::npos)
      {
        std::string close = ")" + t.substr(i + 2, open - i - 2) + "\"";
        size_t end = t.find(close, open);
        i = end == std::string::npos ? t.size() : end + close.size();
        continue;
      }
    }
    if (c == '"' || (c == '\'' && !isDigitSeparator(t, i)))
    {
      for (++i; i < t.size() && t[i] != c && t[i] != '\n'; ++i)
        i += t[i] == '\\';
      ++i;
      continue;
    }
    if (!(isalpha((unsigned char)c) || c == '_') || (i > 0 && isIdentChar(t[i - 1])))
    {
      ++i;
      continue;
    }
    size_t end = i;
    while (end < t.size() && isIdentChar(t[end]))
      ++end;
    std::string name = t.substr(i, end - i);
    size_t open = end;
    while (open < t.size() && (t[open] == ' ' || t[open] == '\t' || t[open] == '\r' || t[open] == '\n'))
      ++open;
    bool call = open < t.size() && t[open] == '(';
    if (name == "__try")
      blank(i, end, "try");
    else if (name == "__except" && call)
    {
      size_t close = closing(open);
      if (close != std::string::npos)
      {
        blank(i, close + 1, "catch (...)");
        end = close + 1;
      }
    }
    else if (name == "FORMAT_STRING" && call)
    {
      size_t close = closing(open);
      if (close != std::string::npos)
      {
        blank(i, open + 1, "");
        blank(close, close + 1, "");
      }
      end = open + 1;
    }
    else if (auto known = knownMacros.find(name); known != knownMacros.end() && known->second.empty())
    {
      // Called, it goes with its arguments (FORMAT_ATTR(4, 5), ZoneScopedNC("name", color)).
      size_t close = call ? closing(open) : std::string::npos;
      if (close != std::string::npos)
        end = close + 1;
      blank(i, end, "");
    }
    i = end;
  }
}

struct SourceConfiguration::Impl
{
  std::vector<std::filesystem::path> paths;
  std::vector<std::string_view> texts;
  std::vector<std::filesystem::path> roots;
  std::map<std::string, size_t> byPath; // pathKeyOf -> index
  std::vector<std::unique_ptr<Preprocessed>> results;
  std::vector<char> state; // 0: not yet, 1: being preprocessed (an include cycle stops there), 2: done
  std::set<std::string> defines; // every name a `#define` of some file names, in any branch

  // The file a `#include` in file i names, among the configured ones: next to file i, or under a
  // root or the directory above it (`<Util/X.hpp>` under src, `<CommonUtil/X.hpp>` under libraries).
  size_t resolve(size_t i, const std::string& name) const
  {
    std::filesystem::path spelled(name);
    std::vector<std::filesystem::path> candidates = {paths[i].parent_path() / spelled};
    for (const auto& root : roots)
    {
      candidates.push_back(root / spelled);
      candidates.push_back(root.parent_path() / spelled);
    }
    for (const auto& c : candidates)
      if (auto it = byPath.find(pathKeyOf(c)); it != byPath.end() && it->second != i)
        return it->second;
    return SIZE_MAX;
  }

  const Preprocessed* of(size_t i)
  {
    if (state[i] == 2)
      return results[i].get();
    if (state[i] == 1)
      return nullptr;
    state[i] = 1;
    auto result = std::make_unique<Preprocessed>(preprocessText(texts[i], [&](const std::string& name) -> const Preprocessed*
    {
      size_t j = resolve(i, name);
      return j == SIZE_MAX ? nullptr : of(j);
    }, &defines));
    results[i] = std::move(result);
    state[i] = 2;
    return results[i].get();
  }

  const Preprocessed* find(const std::filesystem::path& p) const
  {
    auto it = byPath.find(pathKeyOf(p));
    return it == byPath.end() ? nullptr : results[it->second].get();
  }
};

// The function-like macros the C headers at paths define (in the branches their #if lines select, as
// the converter selects them), by name: parameters and body, the body on one line without comments,
// and C's NULL as nullptr. aliases, when given, gets the object-like macros that are another name,
// objects those whose body is string literals or a qualified name and that the headers leave
// defined, definedObjects every such macro they define (one a file #undefs at its end still holds
// before that).
std::map<std::string, std::pair<std::vector<std::string>, std::string>> readHeaderMacros(const std::vector<std::filesystem::path>& paths,
                                                                                       const SourceConfiguration* configuration,
                                                                                       std::map<std::string, std::string>* aliases,
                                                                                       std::map<std::string, std::string>* objects = nullptr,
                                                                                       std::map<std::string, std::string>* definedObjects = nullptr,
                                                                                       const std::set<std::string>* functionMacroNames = nullptr)
{
  std::map<std::string, std::pair<std::vector<std::string>, std::string>> macros;
  for (const std::filesystem::path& path : paths)
  {
    std::string raw;
    if (const Preprocessed* decided = configuration ? configuration->impl->find(path) : nullptr)
      raw = decided->configured; // a converted header: its branches as the configuration selects them with its includes
    else if (readFile(path, raw))
      raw = preprocessText(raw, nullptr).configured;
    else
    {
      fprintf(stderr, "convert: cannot read the macro header %s\n", path.string().c_str());
      continue;
    }
    raw.erase(0, byteOrderMarkLength(raw));
    // The dropped #if branches are blank; comments out (they may hold a #define or span lines).
    std::string text;
    for (size_t i = 0; i < raw.size();)
    {
      if (raw.compare(i, 2, "/*") == 0)
      {
        size_t end = raw.find("*/", i + 2);
        end = end == std::string::npos ? raw.size() : end + 2;
        for (size_t k = i; k < end; ++k)
          text += raw[k] == '\n' ? '\n' : ' ';
        i = end;
      }
      else if (raw.compare(i, 2, "//") == 0)
      {
        while (i < raw.size() && raw[i] != '\n')
          ++i;
      }
      else if (raw[i] == '"' || (raw[i] == '\'' && !isDigitSeparator(raw, i)))
      {
        char quote = raw[i];
        text += raw[i++];
        while (i < raw.size() && raw[i] != quote && raw[i] != '\n')
        {
          if (raw[i] == '\\' && i + 1 < raw.size())
            text += raw[i++];
          text += raw[i++];
        }
        if (i < raw.size() && raw[i] == quote)
          text += raw[i++];
      }
      else
        text += raw[i++];
    }
    for (size_t pos = 0; pos < text.size();)
    {
      // A logical line, continuations joined.
      std::string line;
      while (pos < text.size())
      {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
          end = text.size();
        std::string part = text.substr(pos, end - pos);
        pos = end + 1;
        while (!part.empty() && (part.back() == '\r' || part.back() == ' ' || part.back() == '\t'))
          part.pop_back();
        bool continued = !part.empty() && part.back() == '\\';
        if (continued)
          part.pop_back();
        line += part + " ";
        if (!continued)
          break;
      }
      size_t p = line.find_first_not_of(" \t");
      if (p == std::string::npos || line[p] != '#')
        continue;
      p = line.find_first_not_of(" \t", p + 1);
      if (p != std::string::npos && line.compare(p, 5, "undef") == 0)
      {
        // What the header leaves defined is what its includers see.
        size_t n = line.find_first_not_of(" \t", p + 5), e = n;
        while (e < line.size() && (isalnum((unsigned char)line[e]) || line[e] == '_'))
          ++e;
        if (n != std::string::npos)
        {
          macros.erase(line.substr(n, e - n));
          if (objects)
            objects->erase(line.substr(n, e - n));
        }
        continue;
      }
      if (p == std::string::npos || line.compare(p, 6, "define") != 0)
        continue;
      p = line.find_first_not_of(" \t", p + 6);
      if (p == std::string::npos)
        continue;
      size_t nameEnd = p;
      while (nameEnd < line.size() && (isalnum((unsigned char)line[nameEnd]) || line[nameEnd] == '_'))
        ++nameEnd;
      if (nameEnd == p || nameEnd >= line.size() || line[nameEnd] != '(')
      {
        // Object-like: the import makes the constants. One whose body is another name (glad's
        // `#define glGenTextures glad_glGenTextures`, the function pointer the loader fills) is an
        // alias the converted code is written through; when that name is a function-like macro
        // (lobject.h's `#define setobj2s setobj`), the alias is that macro under its own name.
        if (nameEnd > p)
        {
          std::string body = line.substr(nameEnd);
          size_t a = body.find_first_not_of(" \t"), b = body.find_last_not_of(" \t");
          body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
          static const std::set<std::string> keywords = {"extern", "static", "const", "volatile", "inline", "void", "char", "short", "int", "long",
                                                         "float", "double", "signed", "unsigned", "struct", "union", "enum", "typedef", "register"};
          bool identifier = !body.empty() && (isalpha((unsigned char)body[0]) || body[0] == '_');
          for (char c : body)
            identifier = identifier && (isalnum((unsigned char)c) || c == '_');
          if (identifier && !keywords.count(body))
          {
            if (auto macro = macros.find(body); macro != macros.end())
              macros[line.substr(p, nameEnd - p)] = macro->second;
            else if (aliases)
              (*aliases)[line.substr(p, nameEnd - p)] = body;
          }
        }
        // One whose body is string literals (`#define UTF8_BULLET "\xE2\x80\xA2"`: the conversion
        // makes it a constant, and a use inside a literal concatenation gets its text) or a
        // qualified name (`#define vkCreateInstance VulkanImpl::CreateInstanceWrapper`: every use
        // gets it).
        if ((objects || definedObjects) && nameEnd > p)
        {
          std::string body = line.substr(nameEnd);
          size_t a = body.find_first_not_of(" \t"), b = body.find_last_not_of(" \t");
          body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
          // Another name for one of them (luaconf.h's `#define LUALIB_API LUA_API`): its text.
          if (objects && objects->count(body))
            body = objects->at(body);
          // Made of string macros and literals (lua.h's `#define LUA_RELEASE LUA_VERSION "."
          // LUA_VERSION_RELEASE`, luaconf.h's `#define LUA_QS LUA_QL("%s")`): their literals.
          if (!isStringLiterals(body) && objects)
            if (std::string literals = stringMacroText(body, *objects, macros); !literals.empty())
              body = literals;
          if (!isStringLiterals(body) && !isQualifiedName(body) && isSplicedMacroBody(body) && objects)
            body = splicedMacroText(body, *objects);
          // One that calls a function (callsFunction) is written out too.
          if (functionMacroNames && objects && !isStringLiterals(body) && callsFunction(body, *functionMacroNames))
            body = splicedMacroText(body, *objects);
          // (An alias of a function-like macro, lobject.h's `#define setobj2s setobj`, is that macro, above.)
          if (isStringLiterals(body) || isQualifiedName(body) || isSplicedMacroBody(body) ||
              (isAliasMacro(line.substr(p, nameEnd - p), body) && !macros.count(body)) ||
              (functionMacroNames && callsFunction(body, *functionMacroNames)))
          {
            if (objects)
              (*objects)[line.substr(p, nameEnd - p)] = body;
            if (definedObjects)
              (*definedObjects)[line.substr(p, nameEnd - p)] = body;
          }
        }
        continue;
      }
      size_t close = line.find(')', nameEnd);
      if (close == std::string::npos)
        continue;
      std::vector<std::string> params;
      std::string paramText = line.substr(nameEnd + 1, close - nameEnd - 1);
      for (size_t start = 0; start <= paramText.size();)
      {
        size_t comma = paramText.find(',', start);
        std::string param = paramText.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t a = param.find_first_not_of(" \t"), b = param.find_last_not_of(" \t");
        if (a != std::string::npos)
          params.push_back(param.substr(a, b - a + 1));
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
      std::string body = line.substr(close + 1);
      size_t a = body.find_first_not_of(" \t"), b = body.find_last_not_of(" \t");
      body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
      // C's NULL is a cast of 0 to void*, not a constant the import makes.
      for (size_t at = body.find("NULL"); at != std::string::npos; at = body.find("NULL", at + 1))
        if ((at == 0 || !(isalnum((unsigned char)body[at - 1]) || body[at - 1] == '_')) &&
            (at + 4 >= body.size() || !(isalnum((unsigned char)body[at + 4]) || body[at + 4] == '_')))
          body.replace(at, 4, "nullptr");
      if (!body.empty())
        macros[line.substr(p, nameEnd - p)] = {params, body};
    }
  }
  return macros;
}

SourceConfiguration::SourceConfiguration(const std::vector<std::filesystem::path>& paths, const std::vector<std::string_view>& texts,
                                         const std::vector<std::filesystem::path>& roots)
  : impl(std::make_unique<Impl>())
{
  impl->paths = paths;
  impl->texts = texts;
  impl->roots = roots;
  impl->results.resize(paths.size());
  impl->state.assign(paths.size(), 0);
  for (size_t i = 0; i < paths.size(); ++i)
  {
    impl->byPath[pathKeyOf(paths[i])] = i;
    std::string_view t = texts[i];
    for (size_t at = t.find("define"); at != std::string_view::npos; at = t.find("define", at + 6))
    {
      size_t hash = at;
      while (hash > 0 && (t[hash - 1] == ' ' || t[hash - 1] == '\t'))
        --hash;
      if (hash == 0 || t[hash - 1] != '#')
        continue;
      size_t n = at + 6, e;
      while (n < t.size() && (t[n] == ' ' || t[n] == '\t'))
        ++n;
      for (e = n; e < t.size() && isIdentChar(t[e]); ++e)
      {
      }
      if (e > n && n > at + 6)
        impl->defines.insert(std::string(t.substr(n, e - n)));
    }
  }
  for (size_t i = 0; i < paths.size(); ++i)
    impl->of(i);
  for (auto& result : impl->results)
    if (result)
      blankForParse(*result);
}

SourceConfiguration::~SourceConfiguration() = default;

const std::string& SourceConfiguration::configuredText(size_t i) const { return impl->results[i]->configured; }

// The names of the standard library's free functions: a top-level declaration line (no
// indentation) with a name before its parameter list, `void sort(It first, It last, F less)`.
std::set<std::string> libraryFunctionNames(const std::filesystem::path& libraryDir)
{
  std::set<std::string> names;
  std::error_code ec;
  if (libraryDir.empty() || !std::filesystem::is_directory(libraryDir, ec))
    return names;
  static const std::set<std::string> notNames = {"if", "for", "while", "switch", "return", "sizeof", "decltype", "requires", "operator",
                                                "alignof", "static_assert", "template"};
  for (const auto& entry : std::filesystem::recursive_directory_iterator(libraryDir, ec))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".kpp")
      continue;
    std::string text;
    if (!readFile(entry.path(), text))
      continue;
    for (size_t pos = 0; pos < text.size();)
    {
      size_t end = text.find('\n', pos);
      if (end == std::string::npos)
        end = text.size();
      std::string_view line(text.data() + pos, end - pos);
      pos = end + 1;
      if (line.empty() || !(isalpha((unsigned char)line[0]) || line[0] == '_') || line.rfind("//", 0) == 0)
        continue;
      size_t paren = line.find('(');
      if (paren == std::string_view::npos || paren == 0)
        continue;
      size_t e = paren;
      while (e > 0 && line[e - 1] == ' ')
        --e;
      size_t b = e;
      while (b > 0 && (isalnum((unsigned char)line[b - 1]) || line[b - 1] == '_'))
        --b;
      if (b == e || b == 0 || line[b - 1] == ':' || line[b - 1] == '.')
        continue; // no name, or a qualified one (an out-of-line member)
      std::string name(line.substr(b, e - b));
      if (!notNames.count(name))
        names.insert(name);
    }
  }
  return names;
}

// The names of the standard library's global classes, unions, enums and aliases: a top-level
// declaration line (no indentation), after a template header on it if any, `struct Set`,
// `template<> struct AtomicWord<1> ...`, `using Byte = uint8_t;`.
std::set<std::string> libraryTypeNames(const std::filesystem::path& libraryDir)
{
  std::set<std::string> names;
  std::error_code ec;
  if (libraryDir.empty() || !std::filesystem::is_directory(libraryDir, ec))
    return names;
  auto word = [](std::string_view line, size_t& at)
  {
    while (at < line.size() && line[at] == ' ')
      ++at;
    size_t b = at;
    while (at < line.size() && (isalnum((unsigned char)line[at]) || line[at] == '_'))
      ++at;
    return line.substr(b, at - b);
  };
  for (const auto& entry : std::filesystem::recursive_directory_iterator(libraryDir, ec))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".kpp")
      continue;
    std::string text;
    if (!readFile(entry.path(), text))
      continue;
    for (size_t pos = 0; pos < text.size();)
    {
      size_t end = text.find('\n', pos);
      if (end == std::string::npos)
        end = text.size();
      std::string_view line(text.data() + pos, end - pos);
      pos = end + 1;
      if (line.empty() || !isalpha((unsigned char)line[0]))
        continue; // not at the top level
      size_t at = 0;
      if (line.rfind("template<", 0) == 0)
      {
        int depth = 0;
        for (; at < line.size(); ++at)
          if (line[at] == '<')
            ++depth;
          else if (line[at] == '>' && --depth == 0)
            break;
        ++at;
      }
      std::string_view first = word(line, at);
      if (first == "using")
      {
        std::string_view name = word(line, at);
        while (at < line.size() && line[at] == ' ')
          ++at;
        if (!name.empty() && name != "namespace" && at < line.size() && line[at] == '=')
          names.insert(std::string(name));
        continue;
      }
      if (first == "union" || first == "enum")
      {
        size_t next = at;
        std::string_view second = word(line, next);
        if (second == "class" || second == "struct" || second == "enum")
          at = next;
      }
      else if (first != "class" && first != "struct")
        continue;
      std::string_view name = word(line, at);
      if (!name.empty() && !isdigit((unsigned char)name[0]))
        names.insert(std::string(name));
    }
  }
  return names;
}

std::string convertToKpp(const std::vector<ConvertInput>& inputs, GlobalScope& global, const SymbolTable& symbols,
                         const std::vector<std::filesystem::path>& roots, const std::filesystem::path& outDir,
                         const std::filesystem::path& overridesDir, const SourceConfiguration* configuration,
                         const std::filesystem::path& libraryDir)
{
  struct Piece
  {
    std::string text;
    Counts counts;
    std::vector<std::string> todos;
    std::string source;
    bool header;
  };
  std::map<std::string, std::vector<Piece>> outputs; // output path -> header piece(s) then source piece(s)
  Counts total;
  // The C headers whose function-like macros the converted code uses (overrides/expand-macros.txt:
  // one path per line, # starts a comment). Those of a line `included PATH` are expanded only in the
  // files that include one of the headers of PATH's directory listed so, directly or through a
  // converted header, as only those see them in C++ (Lua's internals: lobject.h's hvalue, ...).
  std::map<std::string, std::pair<std::vector<std::string>, std::string>> headerMacros;
  std::map<std::string, std::string> headerAliases; // their object-like macros that name another identifier
  std::map<std::string, std::string> headerObjects; // and those that are string literals or a qualified name (LUA_NUMBER_FMT)
  std::map<std::string, std::vector<std::filesystem::path>> includedHeaders; // by directory
  if (!overridesDir.empty())
  {
    std::string list;
    if (readFile(overridesDir / "expand-macros.txt", list))
    {
      std::vector<std::filesystem::path> headers;
      for (size_t pos = 0; pos < list.size();)
      {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos)
          end = list.size();
        std::string line = list.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#')
          continue;
        std::string entry = line.substr(a, b - a + 1);
        if (entry.rfind("included ", 0) == 0)
        {
          std::filesystem::path path = entry.substr(entry.find_first_not_of(" \t", 9));
          includedHeaders[path.parent_path().string()].push_back(path);
        }
        else
          headers.push_back(entry);
      }
      headerMacros = readHeaderMacros(headers, nullptr, &headerAliases, &headerObjects);
    }
  }
  // The tests the conversion leaves out (overrides/left-out-tests.txt: `Suite/Test: why` per line,
  // # starts a comment; FileConverter::leftOutTests).
  std::map<std::string, std::string> leftOutTests;
  std::set<std::string> leftOutTestsFound, suitesConverted;
  if (!overridesDir.empty())
  {
    std::string list;
    if (readFile(overridesDir / "left-out-tests.txt", list))
      for (size_t pos = 0; pos < list.size();)
      {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos)
          end = list.size();
        std::string line = list.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#')
          continue;
        line = line.substr(a, b - a + 1);
        size_t colon = line.find(':');
        size_t why = colon == std::string::npos ? std::string::npos : line.find_first_not_of(" \t", colon + 1);
        if (colon == std::string::npos || line.find('/') > colon || why == std::string::npos)
        {
          fprintf(stderr, "convert: %s: `%s` is not `Suite/Test: why`\n", (overridesDir / "left-out-tests.txt").generic_string().c_str(), line.c_str());
          continue;
        }
        std::string test = line.substr(0, colon);
        while (!test.empty() && isspace((unsigned char)test.back()))
          test.pop_back();
        leftOutTests[test] = line.substr(why);
      }
  }
  // The game's own headers each input includes (the includes resolved against the including file's
  // directory, each root and each root's parent, as the game's include paths do), and the
  // function-like macros each header leaves defined: an input sees those of every header it
  // reaches, as C++ does, so that a .cpp expands the X-macro lists its header defines.
  using MacroMap = std::map<std::string, std::pair<std::vector<std::string>, std::string>>;
  auto pathKey = pathKeyOf;
  std::map<std::string, size_t> inputByPath;
  for (size_t k = 0; k < inputs.size(); ++k)
    if (inputs[k].parsed && !inputs[k].parsed->isImported && inputs[k].path.string().rfind("<", 0) != 0)
      inputByPath[pathKey(inputs[k].path)] = k;
  std::vector<std::vector<size_t>> includedInputs(inputs.size());
  std::vector<std::set<std::string>> externalIncludes(inputs.size()); // the file names of the other includes
  // The includes written as a file of the including file's own directory that is converted too (Lua's
  // `#include "lua.h"` in lapi.c): the converted file, never an import, whatever its name.
  std::vector<std::set<std::string>> siblingIncludes(inputs.size());
  // Only the library's source files: its headers are included by the game's files too, which the
  // build compiles without it (StringUtil.cpp's lua_number2str and Benchmark.cpp's printf are the
  // C runtime's, not trio's).
  for (const auto& [key, k] : inputByPath)
    if (auto forced = forcedIncludes.find(inputs[k].path.parent_path().filename().string());
        forced != forcedIncludes.end() && (inputs[k].path.extension() == ".c" || inputs[k].path.extension() == ".cpp"))
      if (auto header = inputByPath.find(pathKey(inputs[k].path.parent_path() / forced->second)); header != inputByPath.end() && header->second != k)
        includedInputs[k].push_back(header->second);
  // A forced macro header (forcedMacroHeaders) comes first in every file of its project,
  // subdirectories too, as the build's /FI puts it.
  for (const auto& [project, forced] : forcedMacroHeaders)
  {
    size_t header = SIZE_MAX;
    for (const auto& [key, k] : inputByPath)
      if (relativeTo(inputs[k].path, roots) == forced)
        header = k;
    if (header != SIZE_MAX)
      for (const auto& [key, k] : inputByPath)
        if (k != header && relativeTo(inputs[k].path, roots).rfind(project, 0) == 0)
          includedInputs[k].insert(includedInputs[k].begin(), header);
  }
  for (const auto& [key, k] : inputByPath)
  {
    std::string_view t = inputs[k].text;
    for (size_t at = t.find("#include"); at != std::string_view::npos; at = t.find("#include", at + 8))
    {
      size_t open = t.find_first_of("<\"\n", at + 8);
      if (open == std::string_view::npos || t[open] == '\n')
        continue;
      size_t close = t.find(t[open] == '<' ? '>' : '"', open + 1);
      if (close == std::string_view::npos || t.substr(open, close - open).find('\n') != std::string_view::npos)
        continue;
      std::filesystem::path name(std::string(t.substr(open + 1, close - open - 1)));
      std::vector<std::filesystem::path> candidates = {inputs[k].path.parent_path() / name};
      for (const auto& root : roots)
      {
        candidates.push_back(root / name);
        candidates.push_back(root.parent_path() / name);
      }
      bool converted = false;
      for (const auto& c : candidates)
        if (auto it = inputByPath.find(pathKey(c)); it != inputByPath.end() && it->second != k)
        {
          includedInputs[k].push_back(it->second);
          converted = true;
          if (&c == &candidates[0] && !name.has_parent_path())
            siblingIncludes[k].insert(name.string());
          break;
        }
      if (!converted)
        externalIncludes[k].insert(name.filename().string());
    }
  }
  // An includer macro header (includerMacroHeaders) comes first in the header that relies on it, once
  // every file that includes that header has been seen to include the macro header before it.
  for (const auto& [user, provider] : includerMacroHeaders)
  {
    size_t header = SIZE_MAX, macros = SIZE_MAX;
    for (const auto& [key, k] : inputByPath)
    {
      std::string path = relativeTo(inputs[k].path, roots);
      if (path == user)
        header = k;
      else if (path == provider)
        macros = k;
    }
    if (header == SIZE_MAX || macros == SIZE_MAX)
      continue;
    bool everyIncluder = true;
    for (const auto& [key, k] : inputByPath)
    {
      const std::vector<size_t>& included = includedInputs[k];
      auto at = std::find(included.begin(), included.end(), header);
      if (at != included.end() && std::find(included.begin(), at, macros) == at)
      {
        fprintf(stderr, "convert: %s includes %s without including %s before it, whose macros that header uses (includerMacroHeaders)\n",
                relativeTo(inputs[k].path, roots).c_str(), user.c_str(), provider.c_str());
        everyIncluder = false;
      }
    }
    if (everyIncluder)
      includedInputs[header].insert(includedInputs[header].begin(), macros);
  }
  // The headers that no file includes, directly or through other headers, from the sources the
  // build compiles, the files given alone and the force-included headers: the C++ build never
  // compiles them, so they are left out. Dead code that Kpp, compiling every file, would compile:
  // ShaderProgram.hpp (the OpenGL renderer's) names the namespace VertexLayout as a type,
  // ItemStackChangeWatcher.hpp assigns an ItemWithQualityID to an ItemID. The includes are read
  // from the text, those in dropped #if branches too, so no header that may be included goes.
  std::vector<bool> reached(inputs.size(), false);
  {
    std::vector<size_t> work;
    auto reach = [&](size_t k)
    {
      if (!reached[k])
      {
        reached[k] = true;
        work.push_back(k);
      }
    };
    for (const auto& [key, k] : inputByPath)
    {
      const std::filesystem::path& path = inputs[k].path;
      std::string ext = path.extension().string();
      std::string rel = relativeTo(path, roots);
      bool built = !filesForOtherPlatforms.count(rel) && !filesNotBuilt.count((path.parent_path().filename() / path.filename()).generic_string());
      if (((ext == ".cpp" || ext == ".cc" || ext == ".c") && built) || forceIncludedHeaders.count(rel))
        reach(k);
    }
    for (const auto& root : roots)
      if (auto given = inputByPath.find(pathKey(root)); given != inputByPath.end()) // a file given alone (Lua/LuaCPPUtilities.hpp)
        reach(given->second);
    while (!work.empty())
    {
      size_t k = work.back();
      work.pop_back();
      for (size_t included : includedInputs[k])
        reach(included);
    }
  }
  // The macros of the `included` headers, each directory's together, and which directory each
  // header's file name belongs to.
  std::map<std::string, MacroMap> includedHeaderMacros;
  std::map<std::string, std::string> includedHeaderDirectory;
  for (const auto& [directory, paths] : includedHeaders)
  {
    includedHeaderMacros[directory] = readHeaderMacros(paths, nullptr, nullptr);
    for (const auto& path : paths)
      includedHeaderDirectory[path.filename().string()] = directory;
  }
  // Every function-like macro the converted files and the expanded C headers define, by name: what
  // callsFunction takes for no function.
  std::set<std::string> functionMacroNames;
  for (const auto& [name, macro] : headerMacros)
    functionMacroNames.insert(name);
  for (const auto& [directory, macros] : includedHeaderMacros)
    for (const auto& [name, macro] : macros)
      functionMacroNames.insert(name);
  for (const auto& [key, k] : inputByPath)
  {
    std::string_view t = inputs[k].text;
    for (size_t at = t.find("define"); at != std::string_view::npos; at = t.find("define", at + 6))
    {
      size_t hash = at;
      while (hash > 0 && (t[hash - 1] == ' ' || t[hash - 1] == '\t'))
        --hash;
      if (hash == 0 || t[hash - 1] != '#')
        continue;
      size_t n = at + 6;
      while (n < t.size() && (t[n] == ' ' || t[n] == '\t'))
        ++n;
      size_t e = n;
      while (e < t.size() && isIdentChar(t[e]))
        ++e;
      if (e > n && e < t.size() && t[e] == '(')
        functionMacroNames.insert(std::string(t.substr(n, e - n)));
    }
  }
  std::map<size_t, MacroMap> ownMacros; // a header's macros it leaves defined, read once
  std::map<size_t, std::map<std::string, std::string>> ownObjects; // and its string and name macros
  std::map<size_t, std::map<std::string, std::string>> definedObjects; // every string or name macro it defines
  auto macrosOf = [&](size_t k) -> const MacroMap&
  {
    auto it = ownMacros.find(k);
    if (it == ownMacros.end())
      it = ownMacros.emplace(k, readHeaderMacros({inputs[k].path}, configuration, nullptr, &ownObjects[k], &definedObjects[k], &functionMacroNames))
             .first;
    return it->second;
  };
  // The string and name macros an input sees: all of its own (TestLuaTerritory.cpp #undefs its
  // NINE_CHUNKS at the end) and those the headers it reaches leave defined (the first to define a
  // name gives its text).
  auto visibleObjectMacros = [&](size_t k)
  {
    std::map<std::string, std::string> out;
    std::set<size_t> seen;
    std::function<void(size_t)> visit = [&](size_t h)
    {
      if (!seen.insert(h).second)
        return;
      macrosOf(h);
      for (const auto& [name, body] : h == k ? definedObjects[h] : ownObjects[h])
        out.emplace(name, body);
      for (size_t inc : includedInputs[h])
        visit(inc);
    };
    visit(k);
    return out;
  };
  auto visibleMacros = [&](size_t k)
  {
    // Depth first in include order, each header once; then the `included` headers' macros of the
    // directories whose headers the file or one of those includes.
    std::vector<const MacroMap*> out;
    std::set<size_t> seen = {k};
    std::function<void(size_t)> visit = [&](size_t h)
    {
      for (size_t inc : includedInputs[h])
        if (seen.insert(inc).second)
        {
          visit(inc);
          if (const MacroMap& m = macrosOf(inc); !m.empty())
            out.push_back(&m);
        }
    };
    visit(k);
    std::set<std::string> directories;
    for (size_t h : seen)
      for (const std::string& name : externalIncludes[h])
        if (auto directory = includedHeaderDirectory.find(name); directory != includedHeaderDirectory.end() && directories.insert(directory->second).second)
          out.push_back(&includedHeaderMacros[directory->second]);
    return out;
  };
  // Hand-written override files, parsed once: what they define replaces the converted definitions.
  std::map<std::string, std::unique_ptr<OverrideFile>> overrideFiles;
  auto overrideFor = [&](const std::string& key) -> OverrideFile*
  {
    if (overridesDir.empty())
      return nullptr;
    auto it = overrideFiles.find(key);
    if (it != overrideFiles.end())
      return it->second.get();
    std::unique_ptr<OverrideFile>& slot = overrideFiles[key];
    std::error_code ec;
    std::filesystem::path p = overridesDir / key;
    if (!std::filesystem::exists(p, ec))
      return nullptr;
    slot = std::make_unique<OverrideFile>();
    if (FILE* f = fopen(p.string().c_str(), "rb"))
    {
      char buffer[65536];
      size_t n;
      while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
        slot->text.append(buffer, n);
      fclose(f);
    }
    SymbolTable localSymbols;
    LexResult lexed = lex(slot->text, localSymbols);
    ParsedFile parsed;
    parseFile(lexed.tokens, slot->text, localSymbols, parsed, false, nullptr);
    collectMemberOverrides(parsed.decls, "", lexed.tokens, slot->text, localSymbols, slot->memberClasses);
    collectOverrideKeys(parsed.decls, "", localSymbols, slot->keys, &slot->memberClasses);
    // What is appended to the converted file: the text without the `[[members]]` classes, which
    // go into the converted classes instead.
    uint32_t at = 0;
    std::vector<std::pair<uint32_t, uint32_t>> cuts;
    for (const auto& [name, m] : slot->memberClasses)
      cuts.push_back({m.begin, m.end});
    std::sort(cuts.begin(), cuts.end());
    for (auto [begin, end] : cuts)
    {
      slot->appended += slot->text.substr(at, begin - at);
      at = end;
    }
    slot->appended += slot->text.substr(at);
    // One definition of a name replaces every overload of it (the original's parameter list
    // may hide behind a macro); several definitions match by arity.
    for (const auto& [name, count] : slot->keys.functionCounts)
      if (count == 1)
        slot->keys.functions.insert(name + "/*");
    return slot.get();
  };
  // Default arguments live on the header declaration, which the conversion drops when the
  // definition is elsewhere: carry them to the definition's parameters.
  CarriedDefaults carriedDefaults;
  CarriedTemplateDefaults carriedTemplateDefaults;
  {
    std::map<ParsedFile*, const ConvertInput*> inputOf;
    for (const ConvertInput& in : inputs)
      if (in.parsed)
        inputOf[in.parsed] = &in;
    // std:: names in carried text, mapped as the token pass maps them, after the game's classes
    // whose names the library uses are renamed (`template<class V = Vector>` is Vector2D).
    auto mapStd = [&](std::string value)
    {
      value = renameGameClassNames(value, nullptr);
      for (size_t at = value.find("std::"); at != std::string::npos; at = value.find("std::", at))
      {
        size_t nameEnd = at + 5;
        while (nameEnd < value.size() && (isalnum((unsigned char)value[nameEnd]) || value[nameEnd] == '_'))
          ++nameEnd;
        std::string name = value.substr(at + 5, nameEnd - at - 5);
        auto it = stdNames.find(name);
        std::string to = it == stdNames.end() ? name : it->second;
        value.replace(at, nameEnd - at, to);
        at += to.size();
      }
      return value;
    };
    // The defaults of a template's parameters on a declaration that goes, for the definition's
    // parameters that have none.
    auto carryTemplateDefaults = [&](Decl* declaration, Decl* def)
    {
      if (def->templateParams.size() != declaration->templateParams.size())
        return;
      auto it = inputOf.find(declaration->file);
      if (it == inputOf.end() || !it->second->tokens)
        return;
      const ConvertInput& in = *it->second;
      const std::vector<Token>& tk = *in.tokens;
      for (size_t i = 0; i < declaration->templateParams.size(); ++i)
      {
        auto* p = static_cast<TemplateParamDecl*>(declaration->templateParams[i]);
        if (!p->defaultArg || static_cast<TemplateParamDecl*>(def->templateParams[i])->defaultArg)
          continue;
        uint32_t from = p->defaultArg->token, k = from;
        int depth = 0;
        bool splitShr = false;
        for (; k < tk.size(); ++k)
        {
          if (tk[k].is(P::Less) || tk[k].is(P::LParen) || tk[k].is(P::LBracket))
            ++depth;
          else if (tk[k].is(P::RParen) || tk[k].is(P::RBracket))
            --depth;
          else if (tk[k].is(P::Greater))
          {
            if (depth == 0)
              break;
            --depth;
          }
          else if (tk[k].is(P::Shr))
          {
            if (depth <= 1)
            {
              splitShr = depth == 1; // `std::less<K>>`: the first > closes the default
              break;
            }
            depth -= 2;
          }
          else if (tk[k].is(P::Comma) && depth == 0)
            break;
          else if (tk[k].is(P::Semi))
            break;
        }
        if (k >= tk.size() || k <= from)
          continue;
        uint32_t a = tk[from].offset;
        uint32_t b = splitShr ? tk[k].offset + 1 : tk[k - 1].offset + tk[k - 1].length;
        carriedTemplateDefaults[def].push_back({i, mapStd(std::string(in.text.substr(a, b - a)))});
      }
    };
    std::function<void(Span<Decl*>)> scan = [&](Span<Decl*> decls)
    {
      for (Decl* d : decls)
      {
        if (d->kind == NodeKind::NamespaceDecl)
        {
          scan(static_cast<NamespaceDecl*>(d)->decls);
          continue;
        }
        if (d->kind == NodeKind::ClassDecl)
        {
          // `template<class K, class V, class Comp = std::less<K>> class FlatMap;`: the defaults
          // live on the forward declaration, which goes; the definition's parameters take them.
          auto* c = static_cast<ClassDecl*>(d);
          if (c->isDefinition || c->templateParams.empty())
            continue;
          Decl* defDecl = global.definitionOf(c);
          if (defDecl && defDecl != c && defDecl->kind == NodeKind::ClassDecl)
            carryTemplateDefaults(c, defDecl);
          continue;
        }
        if (d->kind != NodeKind::FunctionDecl)
          continue;
        auto* f = static_cast<FunctionDecl*>(d);
        if (f->body || (f->flags & (DF_Default | DF_Delete)))
          continue;
        if (!f->templateParams.empty())
        {
          // `template<class V = Vector> ... intersection(std::pair<V, V>, std::pair<V, V>);` in
          // the header, defined in the .cpp without the default: the definition takes it.
          Decl* defDecl = global.definitionOf(f);
          if (defDecl && defDecl != f && defDecl->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(defDecl)->body)
            carryTemplateDefaults(f, defDecl);
        }
        bool any = false;
        for (Decl* p : f->params)
          any = any || static_cast<ParamDecl*>(p)->defaultValue;
        if (!any)
          continue;
        Decl* defDecl = global.definitionOf(f);
        if (!defDecl || defDecl == f || defDecl->kind != NodeKind::FunctionDecl)
          continue;
        auto* def = static_cast<FunctionDecl*>(defDecl);
        if (!def->body || def->params.size() != f->params.size())
          continue;
        auto it = inputOf.find(f->file);
        if (it == inputOf.end() || !it->second->tokens)
          continue;
        const ConvertInput& in = *it->second;
        for (size_t i = 0; i < f->params.size(); ++i)
        {
          Expr* dv = static_cast<ParamDecl*>(f->params[i])->defaultValue;
          if (!dv || static_cast<ParamDecl*>(def->params[i])->defaultValue)
            continue;
          uint32_t end = parameterEndIn(*in.tokens, dv->token);
          if (end == UINT32_MAX || end <= dv->token)
            continue;
          // The text after the parameter's `=` (the expression node's token can sit inside it:
          // `std::string()` starts at its parenthesis), with std:: names mapped as elsewhere.
          uint32_t fromTok = dv->token;
          for (uint32_t k = f->params[i]->token, depth = 0; k < end; ++k)
          {
            const Token& tk = (*in.tokens)[k];
            if (tk.is(P::LParen) || tk.is(P::LBracket) || tk.is(P::LBrace))
              ++depth;
            else if (tk.is(P::RParen) || tk.is(P::RBracket) || tk.is(P::RBrace))
              --depth;
            else if (tk.is(P::Assign) && depth == 0)
            {
              fromTok = k + 1;
              break;
            }
          }
          uint32_t from = (*in.tokens)[fromTok].offset;
          uint32_t to = (*in.tokens)[end - 1].offset + (*in.tokens)[end - 1].length;
          carriedDefaults[def].push_back({i, mapStd(std::string(in.text.substr(from, to - from)))});
        }
      }
    };
    for (const ConvertInput& in : inputs)
      if (in.parsed && !in.parsed->isImported)
        scan(in.parsed->decls);
  }
  ScopeNames scopeNames;
  std::set<std::string> opaqueKept;
  Variants variants;
  std::map<std::string, ClassBases> classBases;
  for (const ConvertInput& in : inputs)
    if (in.parsed && !in.parsed->isImported)
    {
      collectScopeNames(in.parsed->decls, "", symbols, scopeNames);
      collectClassBases(in.parsed->decls, "", symbols, classBases);
      if (in.tokens)
        collectVariants(in.parsed->decls, "", *in.tokens, in.text, symbols, variants);
    }
  addInheritedNames(scopeNames, classBases);
  // The .cpp files' namespace-scope aliases whose names another file declares as an alias too
  // (Cliff.cpp's and CliffPrototype.cpp's `using CO = CliffOrientation;`, EntityPrototypeFlags'
  // and ItemPrototypeFlags' different FlagPairTypes, the rocket silo's MyBinder beside 140
  // function-local ones): each gets its file's name after it.
  std::vector<AliasDeclaration> aliases;
  for (const ConvertInput& in : inputs)
    if (in.parsed && !in.parsed->isImported)
      collectAliases(in.parsed->decls, &in, true, symbols, aliases);
  std::map<std::string, std::set<const void*>> aliasFiles;
  for (const AliasDeclaration& a : aliases)
    aliasFiles[a.name].insert(a.file);
  std::map<const void*, std::map<std::string, std::string>> fileAliasRenames;
  for (const AliasDeclaration& a : aliases)
  {
    const ConvertInput& in = *static_cast<const ConvertInput*>(a.file);
    std::string ext = in.path.extension().string();
    if (!a.namespaceScope || (ext != ".cpp" && ext != ".cc" && ext != ".c") || aliasFiles[a.name].size() < 2)
      continue;
    std::string stem;
    for (char c : in.path.stem().string())
      if (isIdentChar(c))
        stem += c;
    fileAliasRenames[a.file][a.name] = a.name + "Of" + stem;
  }
  // A source file's static function named as one of the library's free functions (ltablib.c's
  // `static int sort (lua_State *L)`, beside the library's sort): file-local in C and C++, a
  // program-wide name in Kpp, which the library's has already, so it gets its file's name after it.
  std::set<std::string> libraryNames = libraryFunctionNames(libraryDir);
  std::set<std::string> libraryTypes = libraryTypeNames(libraryDir);
  for (const ConvertInput& in : inputs)
  {
    std::string ext = in.path.extension().string();
    if (!in.parsed || in.parsed->isImported || (ext != ".cpp" && ext != ".cc" && ext != ".c"))
      continue;
    for (Decl* d : in.parsed->decls)
      if (d->kind == NodeKind::FunctionDecl && (d->flags & DF_Static))
      {
        auto* f = static_cast<FunctionDecl*>(d);
        if (f->name.parts.size() != 1 || f->name.last().symbol == UINT32_MAX)
          continue;
        std::string name(symbols.name(f->name.last().symbol));
        if (!libraryNames.count(name))
          continue;
        std::string stem;
        for (char c : in.path.stem().string())
          if (isIdentChar(c))
            stem += c;
        fileAliasRenames[&in][name] = name + "Of" + stem;
      }
  }
  // The converted files' class definitions and aliases by their own name (FileConverter::classesByName).
  std::map<std::string, std::vector<Decl*>> classesByName;
  {
    std::function<void(Span<Decl*>)> collect = [&](Span<Decl*> decls)
    {
      for (Decl* d : decls)
        if (d->kind == NodeKind::NamespaceDecl)
          collect(static_cast<NamespaceDecl*>(d)->decls);
        else if (d->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(d)->isDefinition && !static_cast<ClassDecl*>(d)->name.empty() &&
                 static_cast<ClassDecl*>(d)->name.last().symbol != UINT32_MAX)
        {
          classesByName[std::string(symbols.name(static_cast<ClassDecl*>(d)->name.last().symbol))].push_back(d);
          collect(static_cast<ClassDecl*>(d)->members);
        }
        else if (d->kind == NodeKind::UsingAliasDecl)
          classesByName[std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name))].push_back(d);
    };
    for (const ConvertInput& in : inputs)
      if (in.parsed && !in.parsed->isImported)
        collect(in.parsed->decls);
  }
  // The files left out below, for the classes defined only there (FileConverter::leftOutFiles).
  std::set<const ParsedFile*> leftOutFiles;
  for (const ConvertInput& in : inputs)
  {
    std::string rel = relativeTo(in.path, roots);
    if (in.parsed && (filesForOtherPlatforms.count(rel) || (inputByPath.count(pathKey(in.path)) && !reached[&in - inputs.data()]) ||
                      filesNotBuilt.count((in.path.parent_path().filename() / in.path.filename()).generic_string())))
      leftOutFiles.insert(in.parsed);
  }
  for (const ConvertInput& in : inputs)
  {
    if (!in.parsed || in.parsed->isImported || in.path.string().rfind("<", 0) == 0)
      continue;
    std::string ext = in.path.extension().string();
    // A .c file is converted as the C++ the game compiles it as (Lua's, fbuild.bff's `/TP`).
    if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc" && ext != ".inl" && ext != ".c")
      continue;
    std::string rel = relativeTo(in.path, roots);
    if (filesForOtherPlatforms.count(rel))
    {
      total.add("files of other platforms left out");
      continue;
    }
    if (filesNotBuilt.count((in.path.parent_path().filename() / in.path.filename()).generic_string()))
    {
      total.add("files the build leaves out");
      continue;
    }
    if (inputByPath.count(pathKey(in.path)) && !reached[&in - inputs.data()])
    {
      total.add("headers no file includes left out");
      continue;
    }
    std::string key = rel.substr(0, rel.size() - ext.size()) + ".kpp";
    FileConverter fc(in, global, symbols);
    fc.leftOutFiles = &leftOutFiles;
    fc.functionMacroNames = &functionMacroNames;
    fc.classesByName = &classesByName;
    fc.leftOutTests = &leftOutTests;
    fc.leftOutTestsFound = &leftOutTestsFound;
    fc.suitesConverted = &suitesConverted;
    if (auto kept = keptCLibraries.find(in.path.parent_path().filename().string()); kept != keptCLibraries.end())
      fc.keptCLibraries = &kept->second;
    fc.scopeNames = &scopeNames;
    fc.libraryTypes = &libraryTypes;
    fc.opaqueKept = &opaqueKept;
    fc.headerMacros = &headerMacros;
    fc.headerAliases = &headerAliases;
    if (auto renames = fileAliasRenames.find(&in); renames != fileAliasRenames.end())
      fc.fileAliasRenames = renames->second;
    if (configuration)
      fc.preprocessed = configuration->impl->find(in.path);
    if (auto ip = inputByPath.find(pathKey(in.path)); ip != inputByPath.end())
    {
      fc.includedMacros = visibleMacros(ip->second);
      fc.objectMacros = visibleObjectMacros(ip->second);
      fc.siblingIncludes = &siblingIncludes[ip->second];
      for (const auto& [name, body] : headerObjects)
        if (!knownMacros.count(name)) // the converter's own rewrite (SDL_PRIs64) stands
          fc.objectMacros.emplace(name, body);
    }
    fc.variants = &variants;
    fc.carriedDefaults = &carriedDefaults;
    fc.carriedTemplateDefaults = &carriedTemplateDefaults;
    if (OverrideFile* ov = overrideFor(key))
    {
      fc.overrides = &ov->keys;
      fc.memberOverrides = &ov->memberClasses;
    }
    Piece piece;
    piece.text = fc.run();
    piece.counts = fc.counts;
    piece.todos = fc.todos;
    piece.source = in.path.string();
    piece.header = ext != ".cpp" && ext != ".cc" && ext != ".c";
    outputs[key].push_back(std::move(piece));
    for (auto& [k, v] : fc.counts.values)
      total.add(k, v);
  }
  // (A conversion without the tests, the headline's, has none to find.)
  for (const auto& [test, why] : leftOutTests)
    if (!suitesConverted.empty() && !leftOutTestsFound.count(test))
      fprintf(stderr, "convert: %s: no converted test is %s\n", (overridesDir / "left-out-tests.txt").generic_string().c_str(), test.c_str());
  std::string report = "# Conversion report\n\n";
  size_t files = 0;
  for (auto& [key, pieces] : outputs)
  {
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.header && !b.header; });
    std::filesystem::path outPath = outDir / key;
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    OverrideFile* ov = overrideFor(key);
    std::string text = "// Converted from";
    for (const Piece& p : pieces)
      text += " " + std::filesystem::path(p.source).filename().string();
    text += " by kppc --convert; see CONVERSION.md.\n\n";
    for (const Piece& p : pieces)
    {
      text += std::string_view(p.text).substr(byteOrderMarkLength(p.text));
      if (text.empty() || text.back() != '\n')
        text += '\n';
      text += '\n';
    }
    if (ov)
    {
      // The hand-written part: appended, its definitions replaced the converted ones above.
      text += "// --- Hand-written, from " + (overridesDir / key).generic_string() + " ---\n\n" + ov->appended;
      if (text.back() != '\n')
        text += '\n';
      total.add("files with overrides");
      for (const auto& [name, m] : ov->memberClasses)
        if (!m.used)
        {
          // Members for a class this file does not define would be lost: say so.
          fprintf(stderr, "convert: %s: the members of %s have no class to join\n", (overridesDir / key).generic_string().c_str(), name.c_str());
          total.add("hand-written members with no class");
        }
    }
    FILE* f = fopen(outPath.string().c_str(), "wb");
    if (f)
    {
      fwrite(text.data(), 1, text.size(), f);
      fclose(f);
      ++files;
    }
    report += "## " + key + "\n";
    for (const Piece& p : pieces)
    {
      report += "- " + std::filesystem::path(p.source).filename().string() + ":";
      for (auto& [k, v] : p.counts.values)
        report += " " + k + " " + std::to_string(v) + ";";
      report += "\n";
      for (const std::string& t : p.todos)
        report += "  - TODO " + t + "\n";
    }
  }
  // Hand-written files with no C++ counterpart (overrides/added/...), written into the output at
  // their path below added/: the DirectX backend's COM interfaces and CComPtr.
  if (!overridesDir.empty())
  {
    std::error_code ec;
    std::filesystem::path added = overridesDir / "added";
    if (std::filesystem::is_directory(added, ec))
      for (const auto& entry : std::filesystem::recursive_directory_iterator(added, ec))
        if (entry.is_regular_file(ec))
        {
          std::filesystem::path rel = entry.path().lexically_relative(added);
          std::filesystem::path target = outDir / rel;
          std::filesystem::create_directories(target.parent_path(), ec);
          if (std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing, ec))
          {
            ++files;
            total.add("hand-written files added");
            report += "## " + rel.generic_string() + "\n- added from " + entry.path().generic_string() + "\n";
          }
        }
  }
  std::string summary = "Files written: " + std::to_string(files) + "\n\n";
  for (auto& [k, v] : total.values)
    summary += "- " + k + ": " + std::to_string(v) + "\n";
  report.insert(strlen("# Conversion report\n\n"), summary + "\n");
  std::filesystem::path reportPath = outDir / "CONVERSION.md";
  FILE* f = fopen(reportPath.string().c_str(), "wb");
  if (f)
  {
    fwrite(report.data(), 1, report.size(), f);
    fclose(f);
  }
  return summary;
}
