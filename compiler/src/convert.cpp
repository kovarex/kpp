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

  // C's macros the parse of C sources sees through (SourceConfiguration with cSources): those whose
  // body is nothing (zconf.h's FAR and ZEXPORT, z_const without ZLIB_CONST), one keyword (zutil.h's
  // `#define local static`, ZEXTERN's extern) or their one argument (`#define OF(args) args`), as
  // the active #defines of every input have them (a name defined two ways is none of them).
  struct CMacros
  {
    std::set<std::string> empty;
    std::map<std::string, std::string> keyword;
    std::set<std::string> identity;
    std::set<std::string> emptyCall; // function-like with no body (zutil.h's Assert(cond,msg), Tracev(x))
    // function-like whose body is its other arguments in parentheses: trio's K&R-or-ANSI parameter
    // lists, `#define TRIO_ARGS2(list,a1,a2) (a1,a2)` used as `f TRIO_ARGS2((format, args), char
    // *format, va_list args)`, and the names that are another name for one (TRIO_VARGS2)
    std::set<std::string> dropFirst;
    // function-like whose body is one of its arguments, by index, also through an identity macro:
    // trio.h's `#define TRIO_PROTO_SCANF(x,a) TRIO_PROTO(x)` keeps argument 0
    std::map<std::string, size_t> keepArgument;
    // object-like whose body is a `*` among empty macros and calling conventions: glad.h's
    // `#define APIENTRYP APIENTRY *` in `typedef void (APIENTRYP PFNGLCLEARPROC)(GLbitfield mask);`
    std::set<std::string> pointer;
    // object-like whose body is storage words only, other macros of them and attributes resolved:
    // zstd's `#define MEM_STATIC static __inline`, `FORCE_INLINE_TEMPLATE static INLINE_KEYWORD
    // FORCE_INLINE_ATTR`; the words, normalized (`static inline`)
    std::map<std::string, std::string> storage;
    // object-like whose body is a multiplication by a parenthesized constant, used after a number:
    // zstd's `#define KB *(1 <<10)` in `16 KB`; the body
    std::map<std::string, std::string> postfix;
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

  // A macro's body with each run of spaces and tabs one space, outside literals: the padding before
  // the continuations' backslashes (UnitTest++'s CHECK_INTERNAL aligns them in column 80) is no
  // text of the expansion.
  std::string collapsedSpaces(const std::string& body)
  {
    std::string out;
    for (size_t i = 0; i < body.size();)
    {
      if (size_t raw = rawStringEnd(body, i); raw != std::string_view::npos)
      {
        out += body.substr(i, raw - i);
        i = raw;
      }
      else if (body[i] == '"' || (body[i] == '\'' && !isDigitSeparator(body, i)))
      {
        char quote = body[i];
        size_t e = i + 1;
        for (; e < body.size() && body[e] != quote; ++e)
          e += body[e] == '\\';
        e = std::min(e + 1, body.size());
        out += body.substr(i, e - i);
        i = e;
      }
      else if (body[i] == ' ' || body[i] == '\t')
      {
        out += ' ';
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t'))
          ++i;
      }
      else
        out += body[i++];
    }
    return out;
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
  const std::map<std::string, std::string> fastDebugDefines = {
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
    {"ENABLE_VIRTUAL_TERMINAL_PROCESSING", "0x0004"}, {"DISABLE_NEWLINE_AUTO_RETURN", "0x0008"}, {"GUID_DEFINED", ""},
    // (fileapi.h: StackWalker.cpp defines it `#ifndef INVALID_FILE_ATTRIBUTES`, for VC5/6)
    {"INVALID_FILE_ATTRIBUTES", "((DWORD)-1)"}};
  // The game is converted in two configurations and the two made one (design.md 12, "The game's
  // DEBUG as config::debug"): FastDebug's defines above, and with kppc --convert-release Release's,
  // which have NDEBUG for _DEBUG and DEBUG and no DISABLE_EXPRESSION_ID (fbuild.bff's
  // .BuildVanillaRelease). With --convert-marks each #if group is marked where its first and last
  // directive stood (`/*kpp:if FILE:N*/`, `/*kpp:endif FILE:N*/`), and convertMerge writes a group
  // whose two texts differ as `if constexpr (config::debug)`.
  // With kppc --convert-final, the official builds' FinalRelease: Release's defines with FINAL_RELEASE
  // and PACKAGED_BUILD (fbuild.bff's .BuildVanillaFinalRelease) and ALL_IN_ONE=1, which the
  // factorio-all-in-one build that packages are made from adds (docs/final-release.md).
  bool convertRelease = false;
  bool convertFinal = false;
  bool convertMarks = false;
  const std::map<std::string, std::string>& buildDefines()
  {
    static const std::map<std::string, std::string> release = []
    {
      std::map<std::string, std::string> d = fastDebugDefines;
      for (const char* name : {"_DEBUG", "DEBUG", "DISABLE_EXPRESSION_ID"})
        d.erase(name);
      d["NDEBUG"] = "";
      return d;
    }();
    static const std::map<std::string, std::string> final = []
    {
      std::map<std::string, std::string> d = release;
      d["FINAL_RELEASE"] = "";
      d["PACKAGED_BUILD"] = "";
      d["ALL_IN_ONE"] = "1";
      return d;
    }();
    return convertFinal ? final : convertRelease ? release : fastDebugDefines;
  }
  // The build a C library converted as C (kppc --c-sources) is preprocessed as: factorio-in-kpp's
  // build.kpp compiles the C libraries with clang-cl as C, release (/O2 /DNDEBUG), with Factorio's
  // global defines (its fromSource); no C++ and no debug build's names. MSVC's x64 predefines, and
  // the C runtime's limits and the open flags the libraries test in #if (limits.h, fcntl.h:
  // gzlib.c's `#ifdef O_BINARY`), which the configuration does not read from the headers. A
  // library's own defines come from its c-libraries\NAME\defines.txt (zlib's verbose=-1).
  std::map<std::string, std::string> cLibraryDefines()
  {
    std::map<std::string, std::string> d = fastDebugDefines;
    for (const char* name : {"_DEBUG", "DEBUG", "DISABLE_EXPRESSION_ID", "__cplusplus", "__cpp_lib_optional", "NOMINMAX", "BUILDING_WARNLESS_C",
                             "CURL_STATICLIB", "SDL_MAIN_HANDLED", "SDL_SENSOR_DISABLED", "LUA_COMPAT_ALL", "LUA_CORE", "LUA_LIB", "lobject_c",
                             "lvm_c", "ltable_c", "__has_include"})
      d.erase(name);
    for (const auto& [name, body] : std::map<std::string, std::string>{
           {"NDEBUG", ""}, {"CHAR_BIT", "8"}, {"SCHAR_MAX", "127"}, {"UCHAR_MAX", "0xff"}, {"SHRT_MAX", "32767"}, {"USHRT_MAX", "0xffff"},
           {"INT_MAX", "2147483647"}, {"UINT_MAX", "0xffffffff"}, {"LONG_MAX", "2147483647L"}, {"ULONG_MAX", "0xffffffffUL"},
           {"LLONG_MAX", "9223372036854775807i64"}, {"ULLONG_MAX", "0xffffffffffffffffui64"}, {"SIZE_MAX", "0xffffffffffffffffui64"},
           {"O_BINARY", "_O_BINARY"}, {"O_EXCL", "_O_EXCL"}, {"O_CREAT", "_O_CREAT"}, {"O_TRUNC", "_O_TRUNC"}, {"O_APPEND", "_O_APPEND"},
           {"O_RDONLY", "_O_RDONLY"}, {"O_WRONLY", "_O_WRONLY"}, {"O_RDWR", "_O_RDWR"},
           // clang-cl 19's own for C on x64 (`clang-cl /E -Xclang -dM`), those the libraries test:
           // zlib's crc32.c braids 64-bit words with __x86_64__
           {"__clang__", "1"}, {"__clang_major__", "19"}, {"__x86_64__", "1"}, {"__x86_64", "1"}, {"__amd64__", "1"}, {"__amd64", "1"},
           {"__MMX__", "1"}, {"__SSE__", "1"}, {"__SSE2__", "1"}, {"__STDC_VERSION__", "201710L"}, {"__STDC_HOSTED__", "1"},
           {"__LITTLE_ENDIAN__", "1"}, {"__ORDER_LITTLE_ENDIAN__", "1234"}, {"__ORDER_BIG_ENDIAN__", "4321"},
           {"__BYTE_ORDER__", "__ORDER_LITTLE_ENDIAN__"}, {"__CHAR_BIT__", "8"}, {"__SIZEOF_POINTER__", "8"}, {"__SIZEOF_LONG__", "4"},
           {"__SIZEOF_LONG_LONG__", "8"}, {"__SIZEOF_INT__", "4"}, {"__SIZEOF_SIZE_T__", "8"}, {"_MSC_VER", "1951"},
           // (trio's triodef.h knows C89 by _MSC_EXTENSIONS: clang-cl does not define __STDC__)
           {"INVALID_HANDLE_VALUE", "((HANDLE)(LONG_PTR)-1)"}, // (handleapi.h: minizip's iowin32.c defines it if it is not)
           // float.h's, which trionan.c tests (`(FLT_RADIX == 2) && (DBL_MAX_EXP == 1024) && (DBL_MANT_DIG ==
           // 53)`: TRIO_IEEE_754, without which a negative zero printed as 0), MSVC's long double a double
           {"FLT_RADIX", "2"}, {"FLT_MANT_DIG", "24"}, {"DBL_MANT_DIG", "53"}, {"LDBL_MANT_DIG", "53"}, {"FLT_DIG", "6"},
           {"DBL_DIG", "15"}, {"LDBL_DIG", "15"}, {"FLT_MAX_EXP", "128"}, {"DBL_MAX_EXP", "1024"}, {"LDBL_MAX_EXP", "1024"},
           {"FLT_MIN_EXP", "(-125)"}, {"DBL_MIN_EXP", "(-1021)"}, {"LDBL_MIN_EXP", "(-1021)"}, {"FLT_MAX_10_EXP", "38"},
           {"DBL_MAX_10_EXP", "308"}, {"LDBL_MAX_10_EXP", "308"},
           // and math.h's classification macros, for `defined(...)` (trionan.c's `defined(PREDEF_STANDARD_C99) &&
           // defined(fpclassify)` takes C99's fpclassify and signbit); their bodies are no #if values
           {"fpclassify", ""}, {"signbit", ""}, {"isnan", ""}, {"isinf", ""}, {"isfinite", ""}, {"isnormal", ""},
           {"INFINITY", ""}, {"NAN", ""}, {"HUGE_VAL", ""}, {"HUGE_VALF", ""}, {"HUGE_VALL", ""},
           {"_MSC_EXTENSIONS", "1"}, {"_MSC_FULL_VER", "195136260"}, {"_MT", "1"}, {"_INTEGRAL_MAX_BITS", "64"}, {"__STDC_NO_THREADS__", "1"}})
      d[name] = body;
    return d;
  }

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
  // keptCLibraries): Lua formats through trio, as its C++ build does, and so do CommonUtil's ssprintf
  // and its double-to-text (trio's %g and %.17g are not the C runtime's: `%.17g` of 0.1 is `0.1`).
  const std::map<std::string, std::set<std::string>> keptCLibraries = {{"Lua", {"trio"}}, {"CommonUtil", {"trio"}}};
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
    std::vector<std::set<std::string>> noteNames;        // and those symbols, a set per note
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
                              const std::set<std::string>* known = nullptr, const MacroTable* base = nullptr)
  {
    Preprocessed out;
    out.configured = std::string(text);
    const MacroTable& baseDefines = base ? *base : buildDefines(); // (C sources: cLibraryDefines)
    MacroTable table = baseDefines;
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
        out.noteNames.push_back(undefined);
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
          {
            out.notes.erase(out.notes.begin() + pendingNote);
            out.noteNames.erase(out.noteNames.begin() + pendingNote);
          }
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
      if (auto b = baseDefines.find(name); b == baseDefines.end() || b->second != body)
        out.defined[name] = body;
    for (const auto& [name, body] : baseDefines)
      if (!table.count(name))
        out.undefined.insert(name);
    return out;
  }

  // Known object-like and function-like macros of the corpus that have a direct kpp spelling.
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
                                                                  // kpp's asserts report themselves.
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
    // NDEBUG branch is `assert(x)`, which kpp's release build compiles out with x:
    // TransitionalRequestBuffers' `verify(set.erase(silo))` erased nothing there).
    {"verify", "[&] { if constexpr (config::debug) { assert($1); } else { (void)($1); } }()"},
    // Windows' wingdi.h and winnt.h, as knownFunctionMacros above (with `(DWORD)((BYTE)(x))` where
    // wingdi.h has `(DWORD)(BYTE)(x)`, which kpp asks to have written out).
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

  // Header macros the conversion defines its own way, wherever a header defines them. UnitTest++'s
  // CHECK_INTERNAL, which every check macro (CHECK, CHECK_EQUAL, ...) expands to, wrote a try and three
  // catches out at each check; its replacement is one call of UnitTest::checkInternal
  // (overrides/added/UnitTest++/CheckInternal.kpp), which has them once, with the condition and the
  // id in lambdas, evaluated where the macro evaluates them (the condition inside the try, the id
  // only when the check fails). An id of nullptr, the checks' other than *_SIGNED, calls the
  // overload without one (checkInternalWithoutId).
  const std::map<std::string, std::string> replacedMacroBodies = {
    {"CHECK_INTERNAL", "UnitTest::checkInternal([&](UnitTest::MemoryOutStream& CHECK_INTERNAL_stream) -> bool { return (condition); }, (filename), "
                       "(line), [&](UnitTest::MemoryOutStream& UNIT_TEST_ID_OUT_stream) { UNIT_TEST_ID_OUT_stream << (id); })"}};
  const std::string checkInternalWithoutId =
    "UnitTest::checkInternal([&](UnitTest::MemoryOutStream& CHECK_INTERNAL_stream) -> bool { return (condition); }, (filename), (line))";

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

  // std:: names and their kpp library spelling. Names not here keep their name and are counted.
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

  // Text of the game's C++ carried into the output by hand (a default argument a declaration that
  // goes had, a type written again): the game's class names and std:: names mapped as the token
  // pass maps them (`std::atomic<bool>` is Atomic<bool>, `template<class V = Vector>` is Vector2D).
  std::string mapStdText(std::string value)
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
  }

  // The aliases the program declares (`using X = ...;` and typedefs), at any scope: their name,
  // the file, and whether at namespace scope (in C++ a .cpp file's namespace-scope alias is its
  // own; in kpp every namespace-scope name is the program's).
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
    std::set<std::string> localTypes; // a block's union: the classes declared in its blocks, which it names as written
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

  void collectVariants(Span<Decl*> decls, const std::string& prefix, const std::vector<Token>& tk, std::string_view text,
                       const SymbolTable& symbols, Variants& out);
  // A function body's block-local aliases (`using VariantType = std::variant<...>;` in a test):
  // the union is declared in the block, named as written, unqualified.
  void collectLocalVariants(Stmt* s, const std::vector<Token>& tk, std::string_view text, const SymbolTable& symbols, Variants& out,
                            std::set<std::string> localClasses)
  {
    if (!s)
      return;
    switch (s->kind)
    {
      case NodeKind::BlockStmt:
        for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
        {
          // A class declared in the block is in scope for what follows it there.
          if (inner->kind == NodeKind::DeclStmt)
            for (Decl* d : static_cast<DeclStmt*>(inner)->decls)
              if (d->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(d)->name.empty())
                localClasses.insert(nameText(static_cast<ClassDecl*>(d)->name, symbols));
          collectLocalVariants(inner, tk, text, symbols, out, localClasses);
        }
        break;
      case NodeKind::DeclStmt:
      {
        size_t before = out.all.size();
        collectVariants(static_cast<DeclStmt*>(s)->decls, "", tk, text, symbols, out);
        for (size_t i = before; i < out.all.size(); ++i)
        {
          out.all[i].qualified = out.all[i].unionName;
          out.all[i].localTypes = localClasses;
        }
        break;
      }
      case NodeKind::IfStmt:
        collectLocalVariants(static_cast<IfStmt*>(s)->then, tk, text, symbols, out, localClasses);
        collectLocalVariants(static_cast<IfStmt*>(s)->else_, tk, text, symbols, out, localClasses);
        break;
      case NodeKind::ForStmt: collectLocalVariants(static_cast<ForStmt*>(s)->body, tk, text, symbols, out, localClasses); break;
      case NodeKind::RangeForStmt: collectLocalVariants(static_cast<RangeForStmt*>(s)->body, tk, text, symbols, out, localClasses); break;
      case NodeKind::WhileStmt: collectLocalVariants(static_cast<WhileStmt*>(s)->body, tk, text, symbols, out, localClasses); break;
      case NodeKind::DoStmt: collectLocalVariants(static_cast<DoStmt*>(s)->body, tk, text, symbols, out, localClasses); break;
      case NodeKind::SwitchStmt: collectLocalVariants(static_cast<SwitchStmt*>(s)->body, tk, text, symbols, out, localClasses); break;
      default: break;
    }
  }

  // Records the program's std::variant aliases and members (prefix: the enclosing scopes and `::`),
  // and the block-local aliases of its functions' bodies.
  void collectVariants(Span<Decl*> decls, const std::string& prefix, const std::vector<Token>& tk, std::string_view text,
                       const SymbolTable& symbols, Variants& out)
  {
    for (Decl* d : decls)
    {
      Type* type = nullptr;
      std::string unionName;
      if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body)
      {
        collectLocalVariants(static_cast<FunctionDecl*>(d)->body, tk, text, symbols, out, {});
        continue;
      }
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

  struct OverrideFile
  {
    std::string text;
    OverrideKeys keys;
    std::map<std::string, MemberOverride> memberClasses;
    std::string appended; // text without the member classes, which go into the converted classes
  };

  // Whether a program's global declaration only redeclares a scoped library's name (design.md 16): a
  // class declared, not defined (`struct unz_file_info64_s;`), or an alias (`using unzFile = void*;`,
  // `using unz_file_info64 = unz_file_info64_s;`) of a name the library has, as C++ code writes them so
  // as not to include the library's header. They are the library's own (C++ allows a typedef again
  // only of the same type), so they go and the uses are qualified (FileConverter::convertDecls).
  bool redeclaresScoped(Decl* d, const SymbolTable& symbols, const std::map<std::string, std::string>& names)
  {
    std::string name;
    if (d->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(d)->isDefinition && static_cast<ClassDecl*>(d)->name.parts.size() == 1 &&
        static_cast<ClassDecl*>(d)->name.last().symbol != UINT32_MAX && d->templateParams.empty())
      name = symbols.name(static_cast<ClassDecl*>(d)->name.last().symbol);
    else if (d->kind == NodeKind::UsingAliasDecl && d->templateParams.empty())
      name = symbols.name(static_cast<UsingAliasDecl*>(d)->name);
    else if (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef) && static_cast<VariableDecl*>(d)->name.parts.size() == 1 &&
             static_cast<VariableDecl*>(d)->name.last().symbol != UINT32_MAX)
      name = symbols.name(static_cast<VariableDecl*>(d)->name.last().symbol);
    return !name.empty() && names.count(name);
  }

  // Whether a class has an initializer-list constructor (C++'s: its first parameter a
  // std::initializer_list, by value or reference, the others defaulted): kpp's braces choose it
  // first (design.md: list-initialization), so `T x(3, 4)` is not `T x{3, 4}` for it. A list later
  // among the parameters (UnitTest++'s SuiteModpackAdder(Suite*, initializer_list)) is no such one.
  bool hasListConstructor(const ClassDecl* cls, const SymbolTable& symbols)
  {
    for (Decl* m : cls->members)
    {
      if (m->kind != NodeKind::FunctionDecl || static_cast<FunctionDecl*>(m)->params.empty())
        continue;
      Span<Decl*> params = static_cast<FunctionDecl*>(m)->params;
      bool othersDefaulted = true;
      for (size_t k = 1; k < params.size(); ++k)
        othersDefaulted = othersDefaulted && params[k]->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(params[k])->defaultValue;
      Type* t = params[0]->kind == NodeKind::ParamDecl ? static_cast<ParamDecl*>(params[0])->type : nullptr;
      while (t && (t->kind == NodeKind::ConstType || t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType))
        t = t->kind == NodeKind::ConstType  ? static_cast<ConstType*>(t)->inner
            : t->kind == NodeKind::LRefType ? static_cast<LRefType*>(t)->referenced
                                            : static_cast<RRefType*>(t)->referenced;
      if (othersDefaulted && t && t->kind == NodeKind::NamedType)
        for (const NamePart& part : static_cast<NamedType*>(t)->name.parts)
          if (part.symbol != UINT32_MAX && symbols.name(part.symbol) == "initializer_list")
            return true;
    }
    return false;
  }

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
    const std::set<std::string>* macroClasses = nullptr; // the classes ENUM and BITFIELD define (collectMacroClasses)
    const std::set<std::string>* braceableClasses = nullptr; // collectBraceableClasses
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
    // unit, kpp has none, and the global namespace it is in is every file's (DeferredTestReporter's
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
        // A local named `_`, which the parser makes unnamed (`_<offset>`), as kpp does.
        for (uint32_t k = v->token; k < v->endToken && k < toks.size() && !toks[k].is(P::Semi); ++k)
          if (toks[k].getKind() == TokenKind::Identifier && spelling(k) == "_")
          {
            underscoreLocals.push_back({toks[v->token].offset, end, k});
            break;
          }
      }
    }
    // The locals named `_` (where each declaration starts, where its function ends, its name's
    // token): in kpp `_` is an unnamed variable, so one the code reads after all gets a name.
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
    // headers give kpp their functions and constants, not their macros.
    const std::map<std::string, std::pair<std::vector<std::string>, std::string>>* headerMacros = nullptr;
    // The imported C headers' object-like macros that are another name (glad's glGenTextures is
    // glad_glGenTextures): a use is written as that name.
    const std::map<std::string, std::string>* headerAliases = nullptr;
    // This .cpp file's namespace-scope aliases that another file declares too, with their own
    // names (`using MyBinder = LuaBinder<LuaRocketSiloControlBehavior>;` is
    // MyBinderOfLuaRocketSiloControlBehavior): file-local in C++, program-wide in kpp.
    std::map<std::string, std::string> fileAliasRenames;
    // The function-like macros of the game's headers this file includes, directly or through other
    // headers, that they leave defined (the X-macro lists a header defines and its .cpp expands), in
    // include order.
    std::vector<const std::map<std::string, std::pair<std::vector<std::string>, std::string>>*> includedMacros;
    // The includes of this file that name a converted file beside it (siblingIncludes): removed.
    const std::set<std::string>* siblingIncludes = nullptr;
    const std::set<std::string>* cInterfaceHeaders = nullptr; // overrides/c-headers.txt: included, imported
    const std::map<std::string, std::set<size_t>>* cInterfaceNames = nullptr; // the functions those headers declare: their parameter counts
    const std::map<std::string, std::string>* cStaticRenames = nullptr; // C: this file's statics another file declares too
    bool cGivenHeader = false; // C: a header given alone, which holds a header-only library's implementation (stb_truetype.h)
    // C: a header given alone that c-headers.txt keeps as C, which the program imports (fontstash.h):
    // its text as the import configures it. What that leaves in is the import's, and goes.
    const std::string* cInterfaceText = nullptr;
    const std::set<std::string>* cPrivateNames = nullptr; // C: the library's private-names.txt (zlib's inflate.h OS)
    // C: the names this .c file (or C header given alone) keeps to itself, written `private`
    // (design.md 15), and the declaration starts that have it already (`typedef struct {...} X;` is
    // two declarations)
    const std::set<std::string>* cPrivate = nullptr;
    std::set<uint32_t> cPrivateAt;
    const std::map<std::string, std::string>* cConfiguredImports = nullptr; // and what an include of one imports instead
    const CMacros* cMacros = nullptr; // C: the macros the parse saw through, also inside macro expansions
    const std::set<std::string>* cWrittenOutMacros = nullptr; // C: macro constants written out at their uses, not made constants ("*": all)
    // C: the library's interface headers converted into its scope (overrides/interface.txt, design.md
    // 16: zstd's zstd.h), whose number macros are constants of the scope and whose function-like
    // macros of values are functions; and the constants they made, by name, which every file's uses
    // write by name.
    const std::set<std::string>* cScopeHeaders = nullptr;
    std::map<std::string, std::string>* cScopeConstants = nullptr;
    bool isScopeHeader() const { return in.c && cScopeHeaders && cScopeHeaders->count(in.path.filename().string()); }
    std::vector<std::string> scopeFunctions; // an interface header's functions, in its order
    bool redeclaresScopedName(Decl* d) const
    {
      return scopedLibraryNames && !scopedLibraryNames->empty() && redeclaresScoped(d, symbols, *scopedLibraryNames);
    }
    // A macro whose uses in the code write its name: this file's constant, or one an interface header
    // made. (Inside another macro's expansion an interface constant is its value, as the preprocessor
    // rescans: ZSTD_VERSION_STRING stringizes ZSTD_LIB_VERSION's numbers, "1.4.0".)
    bool cNamedConstant(const std::string& name)
    {
      return cOwnConstant(name) || (cScopeConstants && cScopeConstants->count(name) && !isScopeHeader());
    }
    // --convert-library: the scoped libraries this program uses, their names by library (design.md
    // 16): a use of one is written qualified (zstd::ZSTD_compress), and an include of its headers
    // (zstd/zstd.h) imports nothing.
    const std::map<std::string, std::string>* scopedLibraryNames = nullptr; // name -> its library
    const std::set<std::string>* scopedLibraries = nullptr;
    const std::set<std::string>* scopedHeaders = nullptr; // their interface headers' names (zlib.h), included as "zlib.h" too
    const std::map<std::string, std::vector<std::string>>* scopedHeaderImports = nullptr; // what an include of one still imports
    // The std names this file brought in with a using-declaration (XmlTestReporter.cpp's `using
    // std::ostream;`), by their kpp names: its later unqualified uses are the library's.
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
    // here, which kpp requires (TestArithmeticCombinator's `combinator->setParameters(params);`). From
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
    // includes them (Lua's override_printf.h includes trio.h, CommonUtil's StringUtil.cpp too; the
    // includes of trio.h elsewhere, which call none of it, go).
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
      auto replaced = [&](const std::string& name, const std::string& body) -> const std::string&
      {
        auto r = replacedMacroBodies.find(name);
        return r == replacedMacroBodies.end() ? body : r->second;
      };
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
          m.body = replaced(name, macro.second);
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
    std::vector<ClassDecl*> classStack; // the classes whose members convertDecls is in, outermost first
    // The identifiers of allocator arguments dropped from std container types (convertTokens), by
    // offset: a template parameter used only there goes from its header too (dropAllocatorParameters).
    std::map<uint32_t, std::string> droppedAllocators;
    Counts counts;
    // This file's #if groups (convertMarks): the next one's number, and those open, each with whether
    // its #if line is written out.
    uint32_t nextIfGroup = 0;
    std::vector<std::pair<uint32_t, bool>> openIfGroups;
    // The C headers this file imports, in its include order: written once per library, in its
    // c-imports.kpp (writeLibraryImports), not in the file.
    std::vector<std::string> cImports;
    void importC(const std::string& header)
    {
      if (std::find(cImports.begin(), cImports.end(), header) == cImports.end())
        cImports.push_back(header);
    }
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

    // An interface header's macro (isScopeHeader, design.md 16) is the library's interface: a number
    // is a constant of its scope (macroToKppText), a function-like macro computing a value from its
    // arguments a function (`template<typename T> constexpr auto ZSTD_COMPRESSBOUND(T srcSize)`,
    // whose arithmetic is the macro's on the argument's type), and one that is neither (a stringized
    // version, `ZSTD_LIB_VERSION`'s `1.4.0`) a TODO of the report, never left out silently. A guard,
    // an empty one and a linkage word (ZSTDLIB_API) are no value.
    // A macro's text that is one name of a scoped library (Lua's override_printf.h, `#define sprintf
    // trio_sprintf`), written qualified as the name is where the code spells it: `trio::trio_sprintf`.
    std::string scopedQualified(const std::string& text) const
    {
      if (!scopedLibraryNames || text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return isIdentChar(c); }))
        return text;
      auto library = scopedLibraryNames->find(text);
      return library == scopedLibraryNames->end() ? text : library->second + "::" + text;
    }

    // Whether a position of the file is at its scope: outside every function's, class's and
    // initializer's braces (a header's `extern "C" {` block is the file's scope), comments, literals
    // and preprocessor lines skipped. A header-only library's implementation defines macros in its
    // functions (stb_image's STBI__COMBO, stb_truetype's STBTT__CSERR): those are no interface names.
    bool atFileScope(uint32_t offset) const
    {
      std::vector<bool> counted; // each open brace: whether it is a scope's (not extern "C"'s)
      int depth = 0;
      bool lineStart = true;
      for (size_t i = 0; i < offset && i < text.size(); ++i)
      {
        char c = text[i];
        if (c == '\n')
        {
          lineStart = true;
          continue;
        }
        if (isspace((unsigned char)c))
          continue;
        if (lineStart && c == '#')
        {
          for (; i < text.size() && text[i] != '\n'; ++i)
            if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r'))
              i += text[i + 1] == '\r' && i + 2 < text.size() && text[i + 2] == '\n' ? 2 : 1;
          lineStart = true;
          continue;
        }
        lineStart = false;
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
          while (i < text.size() && text[i] != '\n')
            ++i;
          lineStart = true;
        }
        else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
        {
          size_t e = text.find("*/", i + 2);
          i = e == std::string_view::npos ? text.size() : e + 1;
        }
        else if (c == '"' || c == '\'')
        {
          for (++i; i < text.size() && text[i] != c && text[i] != '\n'; ++i)
            i += text[i] == '\\';
        }
        else if (c == '{')
        {
          size_t k = i;
          while (k > 0 && isspace((unsigned char)text[k - 1]))
            --k;
          bool externC = k >= 3 && text.substr(k - 3, 3) == "\"C\"";
          counted.push_back(!externC);
          depth += !externC;
        }
        else if (c == '}' && !counted.empty())
        {
          depth -= counted.back();
          counted.pop_back();
        }
      }
      return depth == 0;
    }

    std::string macroToKpp(const std::string& line, uint32_t offset)
    {
      std::string out = macroToKppText(line, offset);
      if (!isScopeHeader() || !atFileScope(offset))
        return out;
      size_t p = line.find("define") + 6;
      while (p < line.size() && isspace((unsigned char)line[p]))
        ++p;
      size_t nameStart = p;
      while (p < line.size() && isIdentChar(line[p]))
        ++p;
      std::string name = line.substr(nameStart, p - nameStart);
      bool functionLike = p < line.size() && line[p] == '(';
      std::string body = collapsedSpaces(continuationsAsSpaces(line.substr(p)));
      for (size_t c = body.find("/*"); c != std::string::npos; c = body.find("/*", c))
      {
        size_t e = body.find("*/", c + 2);
        body.replace(c, (e == std::string::npos ? body.size() : e + 2) - c, " ");
      }
      if (size_t c = body.find("//"); c != std::string::npos)
        body = body.substr(0, c);
      body = collapsedSpaces(body);
      std::vector<std::string> params;
      if (functionLike)
      {
        size_t close = body.find(')');
        std::string list = close == std::string::npos ? std::string() : body.substr(1, close - 1);
        for (size_t start = 0; start <= list.size();)
        {
          size_t comma = list.find(',', start);
          std::string param = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
          size_t a = param.find_first_not_of(" \t"), b = param.find_last_not_of(" \t");
          if (a != std::string::npos)
            params.push_back(param.substr(a, b - a + 1));
          if (comma == std::string::npos)
            break;
          start = comma + 1;
        }
        body = close == std::string::npos ? std::string() : body.substr(close + 1);
      }
      size_t a = body.find_first_not_of(" \t\r\n"), b = body.find_last_not_of(" \t\r\n");
      body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
      if (!functionLike && cOwnConstant(name))
        return out;
      if (!functionLike && cStandardName(name))
      {
        counts.add("C standard names an interface header defines where C's headers did not (left to them)");
        return out;
      }
      std::string indent = line.substr(0, line.find('#'));
      // A string (zlib.h's `#define ZLIB_VERSION "1.3.2"`): a constant array of its characters, as the
      // literal is (sizeof is the literal's). The library's own files keep it written out (a literal
      // may be concatenated with another there).
      if (!functionLike && !body.empty() && body[0] == '"' && isStringLiterals(body))
      {
        counts.add("C string macros of the interface written constants of the library's scope");
        cScopeStrings.insert(name);
        return indent + "const char " + name + "[] = " + body + ";";
      }
      // A type (zconf.h's `#define z_off_t long long`): an alias of it.
      if (!functionLike && isTypeWords(body))
      {
        counts.add("C type macros of the interface written aliases of the library's scope");
        std::string type = body;
        for (size_t k = type.find("__int64"); k != std::string::npos; k = type.find("__int64", k))
          type.replace(k, 7, "long long");
        return indent + "using " + name + " = " + type + ";";
      }
      // A parameter list's wrapper (zconf.h's `#define OF(args) args`, for K&R C's prototypes): its body
      // is its parameter, no value.
      if (functionLike && params.size() == 1 && body == params[0])
      {
        counts.add("C macros of the interface that are no value (guards, linkage words)");
        return out;
      }
      // One beside the function of its name (zlib.h's gzgetc, an inline fast path of the function
      // gzgetc): the function stands, as `(gzgetc)(g)` reaches it in C.
      if (functionLike && cScopeDeclared().count(name))
      {
        counts.add("C function-like macros of the interface beside its function of one name (the function stands)");
        return out;
      }
      if (functionLike)
        if (std::string function = cScopeFunction(name, params, body); !function.empty())
        {
          // (its uses in the library stay expanded; this is its users')
          counts.add("C function-like macros of the interface written functions of the library's scope");
          return out.empty() ? indent + function : out + "\n" + indent + function;
        }
      static const std::set<std::string> linkageWords = {"__declspec", "dllexport", "dllimport", "__attribute__", "visibility", "default", "extern",
                                                         "static", "inline", "__inline", "__forceinline"};
      bool noValue = body.empty();
      if (!noValue && !functionLike)
      {
        // (a linkage word, or another name of one: ZSTDLIB_API is ZSTDLIB_VISIBILITY, empty here)
        noValue = true;
        for (size_t k = 0; k < body.size() && noValue;)
          if (isalpha((unsigned char)body[k]) || body[k] == '_')
          {
            size_t e = k;
            while (e < body.size() && isIdentChar(body[e]))
              ++e;
            std::string word = body.substr(k, e - k);
            auto object = objectMacros.find(word);
            if (!linkageWords.count(word) && !(object != objectMacros.end() && collapsedSpaces(object->second).find_first_not_of(" \t") == std::string::npos) &&
                word.find("VISIBILITY") == std::string::npos)
              noValue = false;
            k = e;
          }
          else if (strchr(" \t()\"", body[k]))
            ++k;
          else
            noValue = false;
      }
      if (noValue)
      {
        counts.add("C macros of the interface that are no value (guards, linkage words)");
        return out;
      }
      counts.add("C macros of the interface not expressed in kpp");
      todo(offset, "macro of the library's interface not expressed in kpp, its users have no " + name + ": #define " + name +
                       (functionLike ? "(...) " : " ") + body);
      return out;
    }

    // A name of C's standard headers that an interface header defines where they did not (zconf.h's
    // `#ifndef SEEK_SET`): the C headers' (stdio's), never the library's, so not its constant.
    static bool cStandardName(const std::string& name)
    {
      static const std::set<std::string> names = {"SEEK_SET", "SEEK_CUR", "SEEK_END", "NULL", "EOF", "TRUE", "FALSE", "BUFSIZ", "FILENAME_MAX"};
      return names.count(name) != 0;
    }
    // Whether a macro's body is a type of C's words alone (`long long`, `unsigned`, `__int64`).
    static bool isTypeWords(const std::string& body)
    {
      static const std::set<std::string> words = {"unsigned", "signed", "long", "short", "int", "char", "float", "double", "void", "__int64"};
      bool any = false;
      for (size_t i = 0; i < body.size();)
        if (isalpha((unsigned char)body[i]) || body[i] == '_')
        {
          size_t e = i;
          while (e < body.size() && isIdentChar(body[e]))
            ++e;
          if (!words.count(body.substr(i, e - i)))
            return false;
          any = true;
          i = e;
        }
        else if (body[i] == ' ' || body[i] == '\t')
          ++i;
        else
          return false;
      return any;
    }
    // An interface header's own names (its functions, types and enumerators: zlib.h's deflateInit_,
    // z_stream), which its function-like macros may name; and its string constants (ZLIB_VERSION).
    std::set<std::string> cScopeDeclaredNames;
    bool cScopeDeclaredRead = false;
    std::set<std::string> cScopeStrings;
    const std::set<std::string>& cScopeDeclared()
    {
      if (!cScopeDeclaredRead && in.parsed)
      {
        cScopeDeclaredRead = true;
        auto add = [&](const QualifiedName& n)
        {
          if (n.parts.size() == 1 && n.last().symbol != UINT32_MAX)
            cScopeDeclaredNames.insert(std::string(symbols.name(n.last().symbol)));
        };
        for (Decl* d : in.parsed->decls)
          if (d->kind == NodeKind::FunctionDecl)
            add(static_cast<FunctionDecl*>(d)->name);
          else if (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
            add(static_cast<VariableDecl*>(d)->name);
          else if (d->kind == NodeKind::ClassDecl)
            add(static_cast<ClassDecl*>(d)->name);
          else if (d->kind == NodeKind::UsingAliasDecl)
            cScopeDeclaredNames.insert(std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name)));
          else if (d->kind == NodeKind::EnumDecl)
          {
            add(static_cast<EnumDecl*>(d)->name);
            for (Decl* e : static_cast<EnumDecl*>(d)->enumerators)
              cScopeDeclaredNames.insert(std::string(symbols.name(static_cast<EnumeratorDecl*>(e)->name)));
          }
      }
      return cScopeDeclaredNames;
    }

    // An interface header's function-like macro as a function of its scope: each parameter a value of
    // its own type (a template parameter), the body an expression of the parameters, numbers, the
    // interface's constants, functions and types (zlib.h's `deflateInit_((strm), (level),
    // ZLIB_VERSION, (int)sizeof(z_stream))`) and operators, which the function returns: the macro's
    // arithmetic on the arguments' types (C's conversions are C++'s); constexpr when it calls none of
    // the interface's functions. None (""): a body of anything else (a stringizing `#str`, a
    // statement, a name it would capture at its use).
    std::string cScopeFunction(const std::string& name, const std::vector<std::string>& params, const std::string& body)
    {
      if (params.empty() || body.empty())
        return std::string();
      static const std::set<std::string> typeWords = {"sizeof", "unsigned", "signed", "int", "long", "short", "char", "float", "double", "size_t"};
      bool calls = false;
      for (size_t i = 0; i < body.size();)
      {
        char c = body[i];
        if (isdigit((unsigned char)c))
        {
          while (i < body.size() && (isIdentChar(body[i]) || body[i] == '.'))
            ++i;
          continue;
        }
        if (isalpha((unsigned char)c) || c == '_')
        {
          size_t j = i;
          while (j < body.size() && isIdentChar(body[j]))
            ++j;
          std::string word = body.substr(i, j - i);
          i = j;
          if (typeWords.count(word) || std::find(params.begin(), params.end(), word) != params.end() || cOwnConstant(word) || cScopeStrings.count(word))
            continue;
          // (a member, after `.` or `->`: ioapi.h's `(filefunc).zfile_func64.zread_file`)
          size_t start = i - word.size();
          size_t before = start == 0 ? std::string::npos : body.find_last_not_of(" \t", start - 1);
          if (before != std::string::npos && (body[before] == '.' || (body[before] == '>' && before > 0 && body[before - 1] == '-')))
            continue;
          if (!cScopeDeclared().count(word))
            return std::string();
          size_t next = body.find_first_not_of(" \t", i);
          calls = calls || (next != std::string::npos && body[next] == '(');
          continue;
        }
        if (!strchr(" \t()+-*/%<>|&~!^?:=,.", c))
          return std::string();
        ++i;
      }
      std::string function = "template<";
      for (size_t k = 0; k < params.size(); ++k)
        function += std::string(k ? ", " : "") + "typename T" + std::to_string(k + 1);
      function += std::string(calls ? "> auto " : "> constexpr auto ") + name + "(";
      // (a parameter whose address the body takes, ioapi.h's `call_zopen64((&(filefunc)),...)`, refers to
      // the argument itself, as the macro's does: a forwarding reference, not a copy)
      auto addressTaken = [&](const std::string& param)
      {
        for (size_t at = body.find('&'); at != std::string::npos; at = body.find('&', at + 1))
        {
          size_t before = at == 0 ? std::string::npos : body.find_last_not_of(" \t", at - 1);
          if (before != std::string::npos && !strchr("(,=?:", body[before]))
            continue; // (a binary `&`)
          size_t k = body.find_first_not_of(" \t(", at + 1);
          if (k != std::string::npos && body.compare(k, param.size(), param) == 0 &&
              (k + param.size() == body.size() || !isIdentChar(body[k + param.size()])))
            return true;
        }
        return false;
      };
      for (size_t k = 0; k < params.size(); ++k)
        function += std::string(k ? ", " : "") + "T" + std::to_string(k + 1) + (addressTaken(params[k]) ? "&& " : " ") + params[k];
      return function + ") { return " + body + "; }";
    }

    // Turns `#define NAME body` into a constant where the body is one, else a TODO comment.
    std::string macroToKppText(const std::string& line, uint32_t offset)
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
          m.body = collapsedSpaces(continuationsAsSpaces(body.substr(close + 1)));
          while (!m.body.empty() && isspace((unsigned char)m.body.front()))
            m.body.erase(m.body.begin());
          while (!m.body.empty() && isspace((unsigned char)m.body.back()))
            m.body.pop_back();
          // (one that is nothing, too: GameRenderer.cpp's CHECK_AND_ADVANCE_STATE in Release; its uses
          // go)
          endLocalMacro(name, offset); // a redefinition without #undef replaces the earlier one
          m.from = offset;
          localMacros[name].push_back(m);
          counts.add(m.body.empty() ? "local function-like macros that are nothing" : "local function-like macro");
          return "";
        }
      }
      if (!functionLike)
        if (const std::string* value = cOwnConstant(name))
        {
          if (isScopeHeader())
          {
            // (the scope's: its users write zstd::ZSTD_CONTENTSIZE_UNKNOWN, its files the name)
            counts.add("C macro constants of the interface written constants of the library's scope");
            (*cScopeConstants)[name] = *value;
            // (C's null pointer by another name, zlib.h's `#define Z_NULL 0`: nullptr, as its uses are
            // written, so a user's `strm.zalloc = zlib::Z_NULL` is a pointer's)
            bool nullPointer = name.size() >= 4 && name.compare(name.size() - 4, 4, "NULL") == 0 && *value == "0";
            return line.substr(0, line.find('#')) + "const auto " + name + " = " + (nullPointer ? std::string("nullptr") : *value) + ";" + trailing;
          }
          counts.add("C macro constants of a .c file written private constants");
          return line.substr(0, line.find('#')) + "private const auto " + name + " = " + *value + ";" + trailing;
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
        if (constant && (quotes == 0 || quotes == 2) && cWrittenOutMacros && (cWrittenOutMacros->count("*") || cWrittenOutMacros->count(name)))
        {
          counts.add("C macro constants written out at their uses");
          return std::string();
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
          if (in.c && innermost && innermost->classBody)
          {
            // C's macros are global (deflate.h's `#define LIT_BUFS 4` among internal_state's
            // members, used in deflate.c): the constant goes before the struct.
            const ScopeRange* outermost = innermost;
            for (const ScopeRange& r : scopeRanges)
              if (r.begin <= offset && offset < r.end && r.begin < outermost->begin)
                outermost = &r;
            size_t lineStart = text.rfind('\n', outermost->begin);
            lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
            cInsertBefore(uint32_t(lineStart), "const auto " + name + " = " + body + ";\n");
            counts.add("C macros defined in a struct written before it");
            return std::string();
          }
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
        {
          counts.add("#if");
          // (written where the text around it is: a drop its own condition starts begins at it)
          bool around = active || (nextDrop < decided.dropped.size() && decided.dropped[nextDrop].first == offset);
          openIfGroups.push_back({nextIfGroup++, around});
          if (convertMarks && around)
          {
            replace(offset, length, "/*kpp:if " + in.path.filename().string() + ":" + std::to_string(openIfGroups.back().first) + "*/");
            continue;
          }
        }
        else if (word == "endif" && !openIfGroups.empty())
        {
          auto [group, written] = openIfGroups.back();
          openIfGroups.pop_back();
          if (convertMarks && written)
          {
            replace(offset, length, "/*kpp:endif " + in.path.filename().string() + ":" + std::to_string(group) + "*/");
            continue;
          }
        }
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
          bool cInterface = cInterfaceHeaders && cInterfaceHeaders->count(base);
          if (siblingIncludes && siblingIncludes->count(header) && !cInterface)
          {
            // A converted file of this file's directory (Lua's own headers, by names a C library's
            // prefix would otherwise take for an import): its symbols are the program's.
            remove(offset, removeLength);
            counts.add("includes removed");
            continue;
          }
          // A scoped library's interface header (`zstd/zstd.h`, --convert-library): nothing to import, its
          // names are the library's, written qualified (convertTokens). Another header of its directory
          // (stb_image's stb_dxt.h, which is no interface of it) stays a C import.
          if (scopedLibraries && scopedHeaders && scopedHeaders->count(base))
          {
            // (the C headers the library's header gave its includer stay imported: its library's C
            // imports, scopedHeaderImports)
            if (scopedHeaderImports)
            {
              auto imports = scopedHeaderImports->find(base);
              if (imports != scopedHeaderImports->end())
                for (const std::string& imported : imports->second)
                  importC(imported);
            }
            remove(offset, removeLength);
            counts.add("includes of a scoped library's headers removed");
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
            // which leaves out the C structs of the interfaces kpp declares as classes
            // (overrides/added/src/Graphics/DX11/ComInterfaces.kpp, generate_com.py).
            importC("kpp_directx.h");
            remove(offset, removeLength);
            counts.add("includes to imports");
            continue;
          }
          static const std::set<std::string> sysHeaders = {"sys/stat.h", "sys/types.h", "sys/timeb.h", "sys/utime.h"};
          bool sysHeader = sysHeaders.count(header) != 0; // (zlib's gzguts.h: _S_IREAD for _wopen)
          if (cHeaders.count(lowered) || cLibrary || cInterface || sysHeader) // Windows' headers by any case (<Psapi.h>, <TlHelp32.h>)
          {
            // A C library that the game configures with #defines before its include imports through a
            // header of programs/factorio/native/include that gives the import the same configuration
            // (jc_voronoi in double precision, as Triangulation.cpp has it).
            static const std::map<std::string, std::string> configuredImports = {{"jc_voronoi/jc_voronoi.h", "kpp_jc_voronoi.h"},
                                                                                 {"stb_image/stb_dxt.h", "kpp_stb_dxt.h"}};
            auto configured = configuredImports.find(header);
            std::string imported = configured != configuredImports.end() ? configured->second : cLibrary || cInterface || sysHeader ? header : lowered;
            if (cInterface && cConfiguredImports)
              if (auto c = cConfiguredImports->find(base); c != cConfiguredImports->end())
                imported = c->second;
            importC(imported);
            remove(offset, removeLength);
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
              else if (range == ownMacroRanges.end())
                ownMacroRanges[name] = {0, offset}; // an included header's, until here (zstd_common.c's `#undef ZSTD_isError`)
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
      bool withoutId = args.size() == 4 && m.body == replacedMacroBodies.at("CHECK_INTERNAL") &&
                       args[3].find_first_not_of(" \t") != std::string::npos &&
                       args[3].substr(args[3].find_first_not_of(" \t"), args[3].find_last_not_of(" \t") + 1 - args[3].find_first_not_of(" \t")) == "nullptr";
      const std::string& b = withoutId ? checkInternalWithoutId : m.body;
      if (m.body == replacedMacroBodies.at("CHECK_INTERNAL"))
        counts.add(withoutId ? "checks written as a call of UnitTest::checkInternal" : "checks with an id written as a call of UnitTest::checkInternal");
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
      std::string text = this->renameOutsideArguments(withFileAndLine(untagged(rescanMacros(expandMacroOnce(m, args, spellings, useOffset), useOffset), true), useOffset));
      return atDeclarationScope(useOffset) ? bracedObjects(std::move(text)) : text;
    }
    // Whether the text at offset stands at namespace or class scope, outside every function body:
    // where kpp reads `T f(...)` as a function (in a body it is an object, and a macro's
    // `Serialiser::TagWriter _(output, "entity", ...)` stays as the printf rewrite reads it). Each
    // `{` is a scope's when its statement names namespace, class, struct, union or enum (or is
    // `extern "C"`), an initializer's after `=`, `(`, `,` or `{` (its enclosing one's), a body's
    // otherwise; inside a body everything is.
    std::vector<uint8_t> tokenInBody;
    bool atDeclarationScope(uint32_t offset)
    {
      if (tokenInBody.empty() && !toks.empty())
      {
        tokenInBody.assign(toks.size(), 0);
        std::vector<uint8_t> stack;
        uint32_t statementStart = 0;
        for (uint32_t k = 0; k < toks.size(); ++k)
        {
          tokenInBody[k] = !stack.empty() && stack.back();
          if (toks[k].is(P::LBrace))
          {
            bool parentBody = !stack.empty() && stack.back();
            bool scope = false;
            for (uint32_t j = statementStart; j < k && !scope; ++j)
              if (toks[j].getKind() == TokenKind::Keyword && !(j > 0 && (toks[j - 1].is(P::Less) || toks[j - 1].is(P::Comma)))) // (not a template parameter's `class T`)
              {
                std::string_view w = spelling(j);
                scope = w == "namespace" || w == "class" || w == "struct" || w == "union" || w == "enum" || w == "extern";
              }
              else if (toks[j].getKind() == TokenKind::Identifier)
              {
                // (a macro that opens a namespace: UnitTest++'s `SUITE(Name) { ... }`, whose TESTs
                // stand at namespace scope)
                auto macro = localMacros.find(std::string(spelling(j)));
                if (macro != localMacros.end())
                  for (const LocalMacro& m : macro->second)
                    scope = scope || m.body.find("namespace") != std::string::npos;
              }
            bool initializer = k > 0 && (toks[k - 1].is(P::Assign) || toks[k - 1].is(P::LParen) || toks[k - 1].is(P::Comma) || toks[k - 1].is(P::LBrace));
            stack.push_back(parentBody || (!scope && !initializer));
            statementStart = k + 1;
          }
          else if (toks[k].is(P::RBrace))
          {
            if (!stack.empty())
              stack.pop_back();
            statementStart = k + 1;
          }
          else if (toks[k].is(P::Semi) || toks[k].getKind() == TokenKind::Directive)
            statementStart = k + 1;
        }
      }
      auto at = std::lower_bound(toks.begin(), toks.end(), offset, [](const Token& t, uint32_t o) { return t.offset < o; });
      return at == toks.end() || !tokenInBody[size_t(at - toks.begin())];
    }
    // An object a macro's expansion declares by parentheses, `UnitTest::ListAdder adderX (List,
    // &testX);` (TEST's, at namespace scope, where kpp reads `T f(...)` as a function): braces, when
    // its class is the program's and braces construct it as parentheses do (braceableClasses). A
    // statement's start only (after `;`, `{`, `}` or the text's start), its `)` ending the statement.
    std::string bracedObjects(std::string text) const
    {
      if (in.c || !braceableClasses || braceableClasses->empty())
        return text;
      for (size_t i = 0; i < text.size(); ++i)
      {
        if (!(isalpha((unsigned char)text[i]) || text[i] == '_') || (i > 0 && (isIdentChar(text[i - 1]) || text[i - 1] == ':')))
          continue;
        size_t before = text.find_last_not_of(" \t\r\n", i == 0 ? std::string::npos : i - 1);
        if (i > 0 && before != std::string::npos && !strchr(";{}", text[before]))
          continue;
        size_t typeEnd = i;
        while (typeEnd < text.size() && (isIdentChar(text[typeEnd]) || (text[typeEnd] == ':' && typeEnd + 1 < text.size() && text[typeEnd + 1] == ':')))
          typeEnd += text[typeEnd] == ':' ? 2 : 1;
        std::string type = text.substr(i, typeEnd - i);
        if (!braceableClasses->count(type))
          continue;
        size_t name = text.find_first_not_of(" \t", typeEnd);
        if (name == std::string::npos || name == typeEnd || !(isalpha((unsigned char)text[name]) || text[name] == '_'))
          continue;
        size_t nameEnd = name;
        while (nameEnd < text.size() && isIdentChar(text[nameEnd]))
          ++nameEnd;
        size_t open = text.find_first_not_of(" \t", nameEnd);
        if (open == std::string::npos || text[open] != '(')
          continue;
        size_t close = open;
        for (int depth = 0; close < text.size(); ++close)
          if (text[close] == '(')
            ++depth;
          else if (text[close] == ')' && --depth == 0)
            break;
        if (close >= text.size())
          continue;
        size_t after = text.find_first_not_of(" \t", close + 1);
        if (after != std::string::npos && text[after] != ';')
          continue;
        text[open] = '{';
        text[close] = '}';
        text.erase(nameEnd, open - nameEnd); // (`adderX {...}`: no space before the brace)
        i = close - (open - nameEnd);
      }
      return text;
    }
    // C's `assert(c)` where an expression stands (llimits.h's `#define check_exp(c,e)
    // (lua_assert(c), (e))`): kpp's assert is a statement, so the assert goes in a lambda called on
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
            if (in.c && cWrittenOutMacros && cWrittenOutMacros->count("*") && !cOwnConstant(name))
              if (const std::string* own = cOwnObjectAt(name, useOffset))
              {
                next += *own;
                changed = true;
                i = j;
                continue;
              }
            if (in.c && cStaticRenames && cStaticRenames->count(name) && !(i > 0 && (result[i - 1] == '.' || result[i - 1] == '>')))
            {
              next += cStaticRenames->at(name);
              i = j;
              continue;
            }
            if (in.c && !objectMacros.empty())
              if (auto object = objectMacros.find(name); object != objectMacros.end() && macroHoldsAt(object->first, useOffset) && !cOwnConstant(name))
              {
                // C: an object macro inside an expansion too, after `.` as well (`tree[n].Freq` in
                // trees.c's smaller(tree, n, m, depth))
                next += object->second;
                changed = true;
                i = j;
                continue;
              }
            if (cMacros && !(i > 0 && (result[i - 1] == '.' || result[i - 1] == '>')))
            {
              // C's macros that are nothing, one keyword or their argument, inside an expansion as
              // in the text (FLUSH_BLOCK's `Tracev((stderr,"[FLUSH]"));`)
              size_t open = j;
              while (open < result.size() && (result[open] == ' ' || result[open] == '\t'))
                ++open;
              bool called = open < result.size() && result[open] == '(';
              size_t close = called ? closingParen(result, open) : std::string::npos;
              if (cMacros->empty.count(name) && !called)
              {
                changed = true;
                i = j;
                continue;
              }
              if (auto keyword = cMacros->keyword.find(name); keyword != cMacros->keyword.end())
              {
                next += keyword->second;
                changed = true;
                i = j;
                continue;
              }
              if (cMacros->emptyCall.count(name) && close != std::string::npos)
              {
                changed = true;
                i = close + 1;
                continue;
              }
              if (cMacros->identity.count(name) && close != std::string::npos)
              {
                next += result.substr(open + 1, close - open - 1);
                changed = true;
                i = close + 1;
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
                macroHoldsAt(om->first, useOffset) && !cOwnConstant(om->first))
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

    // The names of a scoped library in text that never was tokens, qualified as convertTokens
    // qualifies them in code (design.md 16): a function-like macro's body through an alias macro,
    // Lua's `lua_number2str(s,n)` writing `sprintf` that override_printf.h made `trio_sprintf`, is
    // `trio::trio_sprintf`. Literals, members (after `.` or `->`) and qualified names stay.
    std::string qualifyScopedNames(const std::string& s)
    {
      if (!scopedLibraryNames || scopedLibraryNames->empty())
        return s;
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
        if (!(isalpha((unsigned char)c) || c == '_') || (i > 0 && isIdentChar(s[i - 1])))
        {
          out += c;
          ++i;
          continue;
        }
        size_t j = i;
        while (j < s.size() && isIdentChar(s[j]))
          ++j;
        std::string word = s.substr(i, j - i);
        size_t p = out.size();
        while (p > 0 && out[p - 1] == ' ')
          --p;
        bool reached = p > 0 && (out[p - 1] == '.' || (out[p - 1] == '>' && p > 1 && out[p - 2] == '-') || (out[p - 1] == ':' && p > 1 && out[p - 2] == ':'));
        auto library = reached ? scopedLibraryNames->end() : scopedLibraryNames->find(word);
        if (library != scopedLibraryNames->end())
        {
          out += library->second + "::";
          counts.add("names of a scoped library qualified");
        }
        out += word;
        i = j;
      }
      return out;
    }
    // The game's classes whose names the library uses, renamed in text that never was tokens; in C,
    // and this file's renamed statics and private names (cStaticRenames) too: trio's math.c names
    // its ieee_double_shape_type in its EXTRACT_WORDS macros, whose expansions are such text.
    std::string renameGameClasses(const std::string& s)
    {
      std::string renamed = renameGameClassNames(qualifyScopedNames(s), &counts);
      if (!in.c || !cStaticRenames || cStaticRenames->empty())
        return renamed;
      std::string out;
      for (size_t i = 0; i < renamed.size();)
      {
        char c = renamed[i];
        if (c == '"' || (c == '\'' && !isDigitSeparator(renamed, i)))
        {
          size_t j = i + 1;
          while (j < renamed.size() && renamed[j] != c)
            j += renamed[j] == '\\' ? 2 : 1;
          j = std::min(j + 1, renamed.size());
          out.append(renamed, i, j - i);
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
        while (j < renamed.size() && isIdentChar(renamed[j]))
          ++j;
        std::string word = renamed.substr(i, j - i);
        size_t p = out.size();
        while (p > 0 && out[p - 1] == ' ')
          --p;
        bool member = p > 0 && (out[p - 1] == '.' || (out[p - 1] == '>' && p > 1 && out[p - 2] == '-'));
        auto r = member ? cStaticRenames->end() : cStaticRenames->find(word);
        out += r != cStaticRenames->end() ? r->second : word;
        i = j;
      }
      return out;
    }
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

    // A printf-style format's conversions, in the order of the arguments they read: the conversion
    // character ('*' for a width or precision given as an argument) and whether it reads a 64-bit
    // integer (the z, j, t, ll and I64 sizes of d i u x X o).
    struct FormatConversion
    {
      char conversion;
      bool wide;
    };
    static std::vector<FormatConversion> formatConversions(const std::string& format)
    {
      std::vector<FormatConversion> out;
      for (size_t k = 0; k < format.size(); ++k)
      {
        if (format[k] != '%')
          continue;
        if (++k < format.size() && format[k] == '%')
          continue;
        while (k < format.size() && strchr("-+ #0'", format[k]))
          ++k;
        for (int part = 0; part < 2; ++part) // the width, then the precision after a `.`
        {
          if (part == 1 && !(k < format.size() && format[k] == '.'))
            break;
          if (part == 1)
            ++k;
          if (k < format.size() && format[k] == '*')
          {
            out.push_back({'*', false});
            ++k;
          }
          while (k < format.size() && isdigit((unsigned char)format[k]))
            ++k;
        }
        bool isWide = false;
        if (format.compare(k, 3, "I64") == 0)
          isWide = true, k += 3;
        else if (format.compare(k, 3, "I32") == 0)
          k += 3;
        else if (format.compare(k, 2, "ll") == 0)
          isWide = true, k += 2;
        else if (k < format.size() && strchr("zjt", format[k]))
          isWide = true, ++k;
        else
          while (k < format.size() && strchr("hlLqI", format[k]))
            ++k;
        if (k >= format.size())
          break;
        out.push_back({format[k], isWide && strchr("diuxXo", format[k]) != nullptr});
      }
      return out;
    }
    // The format at token i: adjacent literals and the string macros between them (`"%s "
    // UTF8_MULTIPLY " %zu"`), into format; returns its last token.
    uint32_t formatAt(uint32_t i, std::string& format)
    {
      uint32_t last = i;
      for (; last < toks.size(); ++last)
        if (toks[last].getKind() == TokenKind::String)
          format += spelling(last);
        else if (auto macro = toks[last].getKind() == TokenKind::Identifier ? objectMacros.find(std::string(spelling(last))) : objectMacros.end();
                 macro != objectMacros.end() && isStringLiterals(macro->second))
          format += macro->second;
        else
          break;
      return last - 1;
    }
    // The token ranges [first, end) of the arguments from token at to the call's closing
    // parenthesis, by depth; the last range's end is that parenthesis.
    std::vector<std::pair<uint32_t, uint32_t>> argumentRanges(uint32_t at)
    {
      std::vector<std::pair<uint32_t, uint32_t>> out;
      while (at < toks.size())
      {
        uint32_t end = at;
        for (int depth = 0; end < toks.size(); ++end)
          if (toks[end].is(P::LParen) || toks[end].is(P::LBracket) || toks[end].is(P::LBrace))
            ++depth;
          else if (toks[end].is(P::RParen) || toks[end].is(P::RBracket) || toks[end].is(P::RBrace))
          {
            if (depth-- == 0)
              break;
          }
          else if (toks[end].is(P::Comma) && depth == 0)
            break;
        if (end == at || end >= toks.size())
          break;
        out.push_back({at, end});
        if (!toks[end].is(P::Comma))
          break;
        at = end + 1;
      }
      return out;
    }
    // Writes the argument [first, end) as `type(argument)`. The argument's last token ends with the
    // parenthesis in place of itself: an insert after it would fall where a macro's rewrite of its
    // own closing parenthesis takes it (`LOG_AND_ABORT("%zu", directionCount)`); a one-token
    // argument is replaced whole, as an insert at its start would be taken by the replacement. A
    // second rewrite of that token would leave the parentheses unbalanced, which the build reports.
    void castArgument(uint32_t first, uint32_t end, const std::string& type)
    {
      uint32_t last = end - 1;
      if (last == first)
        replace(toks[first].offset, toks[first].length, type + "(" + std::string(spelling(first)) + ")");
      else
      {
        insert(toks[first].offset, type + "(");
        replace(toks[last].offset, toks[last].length, std::string(spelling(last)) + ")");
      }
    }
    // A printf-style format at token i (a string literal, or adjacent ones, that is an argument and
    // has a `%`): each argument a 64-bit conversion reads (%zu, %llu, %I64u and their d/x kin) is
    // written size_t(...), the size_t C++ passes there, which a container size's int (kpp's
    // library) is not: the vararg would carry 32 bits where printf reads 64. By the format, not by
    // types, so that it holds where the converter has none (templates' instances, fixtures'
    // members). Returns the last token of the literal.
    uint32_t widenFormatArguments(uint32_t i)
    {
      std::string format;
      uint32_t last = formatAt(i, format);
      if (format.find('%') == std::string::npos || last + 1 >= toks.size() || !toks[last + 1].is(P::Comma))
        return last;
      std::vector<FormatConversion> conversions = formatConversions(format);
      std::vector<std::pair<uint32_t, uint32_t>> arguments = argumentRanges(last + 2);
      for (size_t n = 0; n < conversions.size() && n < arguments.size(); ++n)
      {
        auto [at, end] = arguments[n];
        bool cast = toks[at].getKind() == TokenKind::Identifier && spelling(at) == "size_t" && toks[at + 1].is(P::LParen);
        if (conversions[n].wide && !cast)
        {
          { size_t before = edits.size(); castArgument(at, end, "size_t"); for (size_t e = before; e < edits.size(); ++e) widenedFormats[i].push_back(e); }
          counts.add("64-bit printf arguments written size_t");
        }
      }
      return last;
    }
    // Whether the declarations say an expression is a 64-bit integer: a variable, member, parameter
    // or function result of such a type (size_t, int64_t, uint64_t, long long, and aliases of them:
    // UnitNumber), arithmetic with one, a cast to one, a literal suffixed so.
    bool isWideIntegerType(Type* type, int depth = 0)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType || type->kind == NodeKind::RRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner
             : type->kind == NodeKind::LRefType  ? static_cast<LRefType*>(type)->referenced
                                                 : static_cast<RRefType*>(type)->referenced;
      if (!type || depth > 8)
        return false;
      if (type->kind == NodeKind::BuiltinType)
        return static_cast<BuiltinType*>(type)->longCount >= 2;
      if (type->kind != NodeKind::NamedType)
        return false;
      auto* named = static_cast<NamedType*>(type);
      const QualifiedName& name = named->name;
      if (!name.parts.empty() && name.last().symbol != UINT32_MAX)
      {
        static const std::set<std::string_view> wide = {"size_t", "int64_t", "uint64_t", "ptrdiff_t", "intptr_t", "uintptr_t", "ssize_t", "time_t"};
        if (wide.count(symbols.name(name.last().symbol)))
          return true;
      }
      Decl* d = named->resolved;
      // `typename container_type::size_type` (FlatSet's, over a std::vector) and the like, which the
      // C++ parse does not resolve: a std container's size_type is size_t, its difference_type
      // ptrdiff_t, whatever width the library's sizes have.
      if (!d && name.parts.size() >= 2 && name.last().symbol != UINT32_MAX)
      {
        std::string_view last = symbols.name(name.last().symbol);
        if (last == "size_type" || last == "difference_type")
          return true;
      }
      if (d && d->kind == NodeKind::UsingAliasDecl)
        return isWideIntegerType(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return isWideIntegerType(static_cast<VariableDecl*>(d)->type, depth + 1);
      return false;
    }
    bool isWideInteger(Expr* e, int depth = 0)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e || depth > 16)
        return false;
      switch (e->kind)
      {
        case NodeKind::LiteralExpr:
        {
          std::string_view text = spelling(e->token);
          return static_cast<LiteralExpr*>(e)->literalKind == TokenKind::Number &&
                 (text.find("ll") != std::string_view::npos || text.find("LL") != std::string_view::npos || text.find("i64") != std::string_view::npos);
        }
        case NodeKind::NameExpr:
        case NodeKind::MemberExpr:
          return isWideIntegerType(typeOfExpr(e, 0).type);
        case NodeKind::CallExpr:
        {
          // A std container's size, a size_t in C++ (TestLuaEvents' `%u` of getChunks().size()):
          // written uint32_t(...), as C++ reads it, whichever width the library's sizes have.
          if (isStdSizeCall(e))
            return true;
          Expr* callee = static_cast<CallExpr*>(e)->callee;
          Decl* f = callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(callee)->resolved
                  : callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                         : nullptr;
          return f && f->kind == NodeKind::FunctionDecl && isWideIntegerType(static_cast<FunctionDecl*>(f)->returnType);
        }
        case NodeKind::BinaryExpr:
        {
          auto* binary = static_cast<BinaryExpr*>(e);
          if (binary->isKeyword)
            return false;
          switch (P(binary->op))
          {
            case P::Plus: case P::Minus: case P::Star: case P::Slash: case P::Percent: case P::Amp: case P::Pipe: case P::Caret:
              return isWideInteger(binary->lhs, depth + 1) || isWideInteger(binary->rhs, depth + 1);
            case P::Shl: case P::Shr: case P::Assign: case P::PlusAssign: case P::MinusAssign:
              return isWideInteger(binary->lhs, depth + 1);
            default:
              return false;
          }
        }
        case NodeKind::UnaryExpr:
        {
          auto* unary = static_cast<UnaryExpr*>(e);
          return !unary->isKeyword && (P(unary->op) == P::Minus || P(unary->op) == P::Tilde || P(unary->op) == P::Plus ||
                                       P(unary->op) == P::PlusPlus || P(unary->op) == P::MinusMinus) && isWideInteger(unary->operand, depth + 1);
        }
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          return (c->then && isWideInteger(c->then, depth + 1)) || isWideInteger(c->else_, depth + 1);
        }
        case NodeKind::CastExpr: return isWideIntegerType(static_cast<CastExpr*>(e)->type);
        case NodeKind::NamedCastExpr: return isWideIntegerType(static_cast<NamedCastExpr*>(e)->type);
        case NodeKind::FunctionalCastExpr: return isWideIntegerType(static_cast<FunctionalCastExpr*>(e)->type);
        default: return false;
      }
    }
    // design.md 14's printf rewrite: a call of ssprintf, or a printf statement, with a literal format
    // becomes toString(...) or print(...) of pieces, the text C printed: the format's text between
    // its conversions as literals, each conversion its argument as the piece that writes it so
    // (`%u` uint32_t(x), the 32 bits printf reads; `%s` StringView(p), or the String a c_str() came
    // from; `%5.2f` alignRight(fixed(x, 2), 5); `%08x` hex(x, 8); `%p` address(p)). A conversion
    // outside that leaves the call as it is (counted): `*` widths, precision on integers and
    // strings, the ' ' and '#' flags but for `%#0Nx`, %E %G %a %n, wide strings. A `*` width is
    // the argument before the value, which the piece takes as its width (LuaHelper's `%*c`).
    struct PrintfConversion
    {
      std::string flags, length;
      int width = -1, precision = -1;
      bool starWidth = false; // the width is an argument (`%*c`), read before the value
      char conversion = 0;
    };
    // The format's raw text (its literals' contents, escapes as written) cut at its conversions:
    // segments[k] before conversions[k], the last after the last. False when it holds a conversion
    // the rewrite does not write (unsupported names it).
    static bool splitPrintfFormat(const std::string& raw, bool rawLiteral, std::vector<std::string>& segments, std::vector<PrintfConversion>& conversions,
                                  std::string& unsupported)
    {
      std::string segment;
      for (size_t k = 0; k < raw.size(); ++k)
      {
        if (raw[k] == '\\' && k + 1 < raw.size() && !rawLiteral)
        {
          segment += raw.substr(k, 2);
          ++k;
          continue;
        }
        if (raw[k] != '%')
        {
          segment += raw[k];
          continue;
        }
        if (k + 1 < raw.size() && raw[k + 1] == '%')
        {
          segment += '%';
          ++k;
          continue;
        }
        PrintfConversion c;
        size_t p = k + 1;
        while (p < raw.size() && strchr("-+ #0'", raw[p]))
          c.flags += raw[p++];
        if (p < raw.size() && raw[p] == '*')
        {
          c.starWidth = true;
          ++p;
        }
        if (p < raw.size() && isdigit((unsigned char)raw[p]))
        {
          c.width = 0;
          while (p < raw.size() && isdigit((unsigned char)raw[p]))
            c.width = c.width * 10 + (raw[p++] - '0');
        }
        if (p < raw.size() && raw[p] == '.')
        {
          ++p;
          if (p < raw.size() && raw[p] == '*')
          {
            unsupported = "`*` precision";
            return false;
          }
          c.precision = 0;
          while (p < raw.size() && isdigit((unsigned char)raw[p]))
            c.precision = c.precision * 10 + (raw[p++] - '0');
        }
        for (const char* length : {"I64", "I32", "hh", "ll", "h", "l", "z", "j", "t", "L", "q"})
          if (raw.compare(p, strlen(length), length) == 0)
          {
            c.length = length;
            p += strlen(length);
            break;
          }
        if (p >= raw.size())
        {
          unsupported = "a format ending in a conversion's middle";
          return false;
        }
        c.conversion = raw[p];
        k = p;
        if (!strchr("diuxXocsfegp", c.conversion) || c.flags.find(' ') != std::string::npos || c.flags.find('\'') != std::string::npos ||
            (c.length == "L") || (c.length == "l" && (c.conversion == 's' || c.conversion == 'c')))
        {
          unsupported = std::string("%") + c.flags + (c.length) + c.conversion;
          return false;
        }
        if (c.precision >= 0 && strchr("diuxXocsp", c.conversion))
        {
          unsupported = "precision on %" + std::string(1, c.conversion);
          return false;
        }
        if (c.flags.find('#') != std::string::npos && !((c.conversion == 'x' || c.conversion == 'X') && c.flags.find('0') != std::string::npos && c.width > 2))
        {
          unsupported = "the # flag";
          return false;
        }
        segments.push_back(segment);
        segment.clear();
        conversions.push_back(c);
      }
      segments.push_back(segment);
      return true;
    }
    // An integer type's exact name, when the declarations give it: int32_t for int, uint32_t for
    // unsigned or uint32_t, int64_t for long long, char for char ("" otherwise or when unknown).
    std::string exactIntegerName(Type* type, int depth = 0)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType || type->kind == NodeKind::RRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner
             : type->kind == NodeKind::LRefType  ? static_cast<LRefType*>(type)->referenced
                                                 : static_cast<RRefType*>(type)->referenced;
      if (!type || depth > 8)
        return {};
      if (type->kind == NodeKind::BuiltinType)
      {
        auto* b = static_cast<BuiltinType*>(type);
        bool isUnsigned = b->isUnsigned || b->keyword == Kw::unsigned_;
        if (b->keyword == Kw::char_)
          return b->isUnsigned || b->isSigned ? std::string() : "char";
        if (b->keyword == Kw::short_)
          return isUnsigned ? "uint16_t" : "int16_t";
        if (b->keyword == Kw::int_ || b->keyword == Kw::unsigned_ || b->keyword == Kw::signed_ || b->keyword == Kw::long_)
          return std::string(isUnsigned ? "uint" : "int") + (b->longCount >= 2 ? "64_t" : "32_t");
        return {};
      }
      if (type->kind != NodeKind::NamedType)
        return {};
      auto* named = static_cast<NamedType*>(type);
      if (!named->name.parts.empty() && named->name.last().symbol != UINT32_MAX)
      {
        static const std::map<std::string_view, std::string> names = {
          {"int8_t", "int8_t"},   {"uint8_t", "uint8_t"},   {"int16_t", "int16_t"}, {"uint16_t", "uint16_t"}, {"int32_t", "int32_t"},
          {"uint32_t", "uint32_t"}, {"int64_t", "int64_t"}, {"uint64_t", "uint64_t"}, {"size_t", "size_t"}};
        if (auto n = names.find(symbols.name(named->name.last().symbol)); n != names.end())
          return n->second;
      }
      Decl* d = named->resolved;
      if (d && d->kind == NodeKind::UsingAliasDecl)
        return exactIntegerName(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return exactIntegerName(static_cast<VariableDecl*>(d)->type, depth + 1);
      return {};
    }
    // The opening and closing text around an argument that make it the piece a conversion writes.
    bool printfPiece(const PrintfConversion& c, Expr* arg, uint32_t argEnd, bool trio, std::string& open, std::string& close, uint32_t& cStrDot,
                     const std::string& widthText = std::string())
    {
      // (a `*` width: its argument's text; with the `0` flag, which pads after a sign, none)
      if (c.starWidth && (widthText.empty() || c.flags.find('0') != std::string::npos))
        return false;
      Expr* plain = arg;
      while (plain && plain->kind == NodeKind::ParenExpr)
        plain = static_cast<ParenExpr*>(plain)->inner;
      std::string exact = arg ? exactIntegerName(plainTypeOf(arg)) : std::string();
      // (`int32_t(value)`, which the parse reads as a call of the type's name)
      if (exact.empty() && plain && plain->kind == NodeKind::CallExpr && static_cast<CallExpr*>(plain)->args.size() == 1 &&
          static_cast<CallExpr*>(plain)->callee->kind == NodeKind::NameExpr)
      {
        auto* typeName = static_cast<NameExpr*>(static_cast<CallExpr*>(plain)->callee);
        if (!typeName->resolved || typeName->resolved->kind != NodeKind::FunctionDecl)
        {
          NamedType named{};
          named.kind = NodeKind::NamedType;
          named.name = typeName->name;
          named.resolved = typeName->resolved;
          exact = exactIntegerName(&named);
        }
      }
      bool literalInteger = plain && plain->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(plain)->literalKind == TokenKind::Number &&
                            isdigit((unsigned char)spelling(plain->token)[0]) && spelling(plain->token).find('.') == std::string_view::npos;
      auto integerType = [&](bool isSigned)
      {
        std::string bits = c.length == "ll" || c.length == "I64" || c.length == "j" || c.length == "q" || c.length == "z" || c.length == "t" ? "64"
                         : c.length == "h"                                                                                                 ? "16"
                         : c.length == "hh"                                                                                                ? "8"
                                                                                                                                           : "32";
        if (c.length == "z" && !isSigned)
          return std::string("size_t");
        return std::string(isSigned ? "int" : "uint") + bits + "_t";
      };
      std::string valueOpen, valueClose;
      switch (c.conversion)
      {
        case 'd':
        case 'i':
        case 'u':
        {
          std::string type = integerType(c.conversion != 'u');
          if (exact != type && !(literalInteger && (type == "int32_t" || type == "uint32_t")))
            valueOpen = type + "(", valueClose = ")";
          break;
        }
        case 'x':
        case 'X':
        case 'o':
        {
          std::string type = integerType(false);
          std::string name = c.conversion == 'x' ? "hex(" : c.conversion == 'X' ? "hexUpper(" : "octal(";
          valueOpen = name + (exact == type || literalInteger ? "" : type + "(");
          valueClose = std::string(exact == type || literalInteger ? "" : ")");
          if (c.flags.find('0') != std::string::npos && c.width > 0 && c.flags.find('-') == std::string::npos)
          {
            bool prefix = c.flags.find('#') != std::string::npos;
            valueOpen = (prefix ? std::string(c.conversion == 'X' ? "\"0X\", " : "\"0x\", ") : std::string()) + valueOpen;
            valueClose += ", " + std::to_string(prefix ? c.width - 2 : c.width) + ")";
            open = valueOpen, close = valueClose;
            return true;
          }
          valueClose += ")";
          break;
        }
        case 'c':
          if (exact != "char")
            valueOpen = "char(", valueClose = ")";
          break;
        case 's':
          if (plain && plain->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(plain)->literalKind == TokenKind::String)
            break;
          if (plain && plain->kind == NodeKind::CallExpr && static_cast<CallExpr*>(plain)->args.empty() &&
              static_cast<CallExpr*>(plain)->callee->kind == NodeKind::MemberExpr)
          {
            auto* m = static_cast<MemberExpr*>(static_cast<CallExpr*>(plain)->callee);
            if (m->op == P::Dot && !m->member.parts.empty() && m->member.parts[0].symbol != UINT32_MAX &&
                symbols.name(m->member.parts[0].symbol) == "c_str" && isStdStringType(plainTypeOf(m->base)))
            {
              // (the String itself: the argument's own `.c_str()`, its last tokens, goes)
              uint32_t dot = argEnd >= 4 ? argEnd - 4 : 0;
              if (argEnd >= 4 && toks[dot].is(P::Dot) && spelling(dot + 1) == "c_str" && toks[dot + 2].is(P::LParen) && toks[dot + 3].is(P::RParen))
              {
                cStrDot = dot;
                break;
              }
            }
          }
          {
            // (a char pointer, or the converter knows no type: `(const char*)` as printf read it,
            // glGetString's `const GLubyte*`)
            Type* type = arg ? plainTypeOf(arg) : nullptr;
            while (type && type->kind == NodeKind::ConstType)
              type = static_cast<ConstType*>(type)->inner;
            bool charPointer = false;
            if (type && type->kind == NodeKind::PointerType)
            {
              Type* pointee = static_cast<PointerType*>(type)->pointee;
              while (pointee && pointee->kind == NodeKind::ConstType)
                pointee = static_cast<ConstType*>(pointee)->inner;
              charPointer = pointee && pointee->kind == NodeKind::BuiltinType && static_cast<BuiltinType*>(pointee)->keyword == Kw::char_ &&
                            !static_cast<BuiltinType*>(pointee)->isUnsigned && !static_cast<BuiltinType*>(pointee)->isSigned;
            }
            // (`x.c_str()` and `e.what()` are C text whatever x's class: a wide one fails to compile)
            bool cText = plain && plain->kind == NodeKind::CallExpr && static_cast<CallExpr*>(plain)->callee->kind == NodeKind::MemberExpr &&
                         !static_cast<MemberExpr*>(static_cast<CallExpr*>(plain)->callee)->member.parts.empty() &&
                         static_cast<MemberExpr*>(static_cast<CallExpr*>(plain)->callee)->member.parts[0].symbol != UINT32_MAX &&
                         (symbols.name(static_cast<MemberExpr*>(static_cast<CallExpr*>(plain)->callee)->member.parts[0].symbol) == "c_str" ||
                          symbols.name(static_cast<MemberExpr*>(static_cast<CallExpr*>(plain)->callee)->member.parts[0].symbol) == "what");
            bool text = charPointer || cText || (type && isStdStringType(type)) || (plain && plain->kind == NodeKind::ConditionalExpr);
            if (arg && !text)
            {
              valueOpen = "StringView((const char*)(", valueClose = "))";
              counts.add("printf %s of a pointer of another or unknown type written StringView((const char*)(x))");
            }
            else
              valueOpen = "StringView(", valueClose = ")";
          }
          break;
        case 'f':
        case 'e':
        case 'g':
        {
          int precision = c.precision < 0 ? 6 : c.precision;
          if (c.conversion == 'g' && precision == 0)
            precision = 1;
          // (ssprintf's text is trio's, CommonUtil's StringUtil.cpp: the game's TrioPieces.kpp)
          if (trio)
            valueOpen = std::string(c.conversion == 'f' ? "trioFixed(" : c.conversion == 'e' ? "trioScientific(" : "trioSignificant(");
          else
            valueOpen = std::string(c.conversion == 'f' ? "fixed(" : c.conversion == 'e' ? "scientific(" : "significant(");
          valueClose = ", " + std::to_string(precision) + ")";
          break;
        }
        case 'p':
          valueOpen = "address(", valueClose = ")";
          break;
        default: return false;
      }
      if (c.flags.find('+') != std::string::npos)
      {
        if (!strchr("difeg", c.conversion))
          return false;
        valueOpen = "withSign(" + valueOpen, valueClose += ")";
      }
      // (a `*` width's argument as the width: negative, it aligns left, as printf's does)
      std::string width = c.starWidth ? widthText : c.width > 0 ? std::to_string(c.width) : std::string();
      if (!width.empty())
      {
        if (c.flags.find('-') != std::string::npos)
          valueOpen = "alignLeft(" + valueOpen, valueClose += ", " + width + ")";
        else if (c.flags.find('0') != std::string::npos && strchr("diufeg", c.conversion))
          valueOpen = "zeroPadded(" + valueOpen, valueClose += ", " + width + ")";
        else
          valueOpen = "alignRight(" + valueOpen, valueClose += ", " + width + ")";
      }
      open = valueOpen, close = valueClose;
      return true;
    }
    // The printf rewrite in a macro's expansion (a local macro's text, written at its use), where
    // there are no expressions: the calls of ssprintf and of the game's format functions found by
    // name, each value written with its type (`int32_t(x)` for %d, `StringView(p)` for %s).
    std::string rewritePrintfText(const std::string& t, int depth = 0)
    {
      if (depth > 8 || (t.find("ssprintf") == std::string::npos && t.find("Logging::") == std::string::npos && t.find("printf") == std::string::npos &&
                                t.find("addDebugProperty") == std::string::npos && t.find("makeProperty") == std::string::npos &&
                                t.find("AbortException::make") == std::string::npos && t.find("TagWriter") == std::string::npos))
        return t;
      // a literal's end from its opening quote (raw ones too)
      auto literalEnd = [&](size_t q) -> size_t
      {
        if (t[q] == 'R' && q + 1 < t.size() && t[q + 1] == '"')
        {
          size_t open = t.find('(', q + 2);
          if (open == std::string::npos)
            return std::string::npos;
          std::string delimiter = t.substr(q + 2, open - q - 2);
          size_t close = t.find(")" + delimiter + "\"", open);
          return close == std::string::npos ? close : close + delimiter.size() + 2;
        }
        size_t e = q + 1;
        while (e < t.size() && t[e] != t[q])
          e += t[e] == '\\' ? 2 : 1;
        return e < t.size() ? e + 1 : std::string::npos;
      };
      std::string out;
      for (size_t i = 0; i < t.size();)
      {
        char c = t[i];
        if (c == '"' || c == '\'')
        {
          size_t e = literalEnd(i);
          if (e == std::string::npos)
            return t;
          out += t.substr(i, e - i);
          i = e;
          continue;
        }
        if (!(isalpha((unsigned char)c) || c == '_') || (i > 0 && isIdentChar(t[i - 1])))
        {
          out += c;
          ++i;
          continue;
        }
        size_t j = i;
        while (j < t.size() && isIdentChar(t[j]))
          ++j;
        std::string word = t.substr(i, j - i);
        size_t open = j;
        while (open < t.size() && t[open] == ' ')
          ++open;
        auto preceded = [&](const std::string& qualifier)
        { return out.size() >= qualifier.size() && out.compare(out.size() - qualifier.size(), qualifier.size(), qualifier) == 0; };
        size_t formatIndex = SIZE_MAX;
        bool member = out.size() >= 1 && (out.back() == '.' || (out.size() >= 2 && out.compare(out.size() - 2, 2, "->") == 0));
        // (printf and fprintf print through the C runtime, not trio; zstd's DISPLAY macro's
        // `fprintf(stderr, ...)`)
        bool printLike = (word == "printf" || word == "fprintf") && !member && !preceded("::");
        if (in.c && !printLike)
          formatIndex = SIZE_MAX;
        else if (printLike)
          formatIndex = word == "fprintf" ? 1 : 0;
        else if (word == "ssprintf" && !member)
          formatIndex = 0;
        else if ((word == "log" || word == "logVerbose" || word == "logAndAbortOrThrow") && preceded("Logging::"))
          formatIndex = 3;
        else if (word == "addDebugProperty" && member)
          formatIndex = 1;
        else if (word == "makeProperty" && preceded("PropertyItem::"))
          formatIndex = 1;
        else if (word == "make" && preceded("AbortException::"))
          formatIndex = 0;
        else if (word == "_" && preceded("TagWriter "))
          formatIndex = 2; // (SAVING_TAG's `Serialiser::TagWriter _(output, "tag", format, ...)`)
        if (formatIndex == SIZE_MAX || open >= t.size() || t[open] != '(')
        {
          out += word;
          i = j;
          continue;
        }
        // the arguments, at depth 0 of the call
        std::vector<std::pair<size_t, size_t>> args;
        size_t k = open + 1, start = k;
        int nest = 0;
        for (; k < t.size(); ++k)
        {
          char d = t[k];
          if (d == '"' || d == '\'' || (d == 'R' && k + 1 < t.size() && t[k + 1] == '"' && !(k > 0 && isIdentChar(t[k - 1]))))
          {
            size_t e = literalEnd(k);
            if (e == std::string::npos)
              return t;
            k = e - 1;
          }
          else if (d == '(' || d == '[' || d == '{')
            ++nest;
          else if (d == ')' || d == ']' || d == '}')
          {
            if (nest-- == 0)
              break;
          }
          else if (d == ',' && nest == 0)
          {
            args.push_back({start, k});
            start = k + 1;
          }
        }
        if (k >= t.size())
          return t;
        if (k > start || !args.empty())
          args.push_back({start, k});
        auto trimmed = [&](std::pair<size_t, size_t> a)
        {
          size_t b = a.first, e = a.second;
          while (b < e && isspace((unsigned char)t[b]))
            ++b;
          while (e > b && isspace((unsigned char)t[e - 1]))
            --e;
          return std::pair<size_t, size_t>{b, e};
        };
        // the call's arguments, each rewritten inside first
        std::vector<std::string> texts;
        for (auto a : args)
        {
          auto [b, e] = trimmed(a);
          texts.push_back(rewritePrintfText(t.substr(b, e - b), depth + 1));
        }
        std::string callee = word == "ssprintf" ? "toString" : printLike ? "print" : word;
        std::string rewritten;
        bool done = false;
        // (print returns no count: only a statement's printf, after `;`, `{`, `}` or `)` and before `;`)
        if (printLike)
        {
          size_t before = out.find_last_not_of(" \t\r\n"), after = t.find_first_not_of(" \t", k + 1);
          bool statement = (before == std::string::npos || strchr(";{}):", out[before])) && after != std::string::npos && t[after] == ';';
          if (!statement)
          {
            counts.add("printf calls in macro expansions left as they are (no statement)");
            formatIndex = SIZE_MAX;
          }
        }
        if (formatIndex < texts.size())
        {
          // the format: literals only (raw ones of one delimiter)
          const std::string& f = texts[formatIndex];
          std::string raw, rawDelimiter;
          int rawLiterals = 0;
          bool literals = !f.empty();
          for (size_t q = 0; q < f.size() && literals;)
          {
            if (isspace((unsigned char)f[q]))
            {
              ++q;
              continue;
            }
            if (f[q] == 'R' && q + 1 < f.size() && f[q + 1] == '"')
            {
              size_t o = f.find('(', q + 2);
              std::string delimiter = o == std::string::npos ? std::string() : f.substr(q + 2, o - q - 2);
              size_t close = o == std::string::npos ? o : f.find(")" + delimiter + "\"", o);
              if (close == std::string::npos || (rawLiterals == 0 && !raw.empty()) || (rawLiterals > 0 && delimiter != rawDelimiter))
              {
                literals = false;
                break;
              }
              rawDelimiter = delimiter;
              raw += f.substr(o + 1, close - o - 1);
              q = close + delimiter.size() + 2;
              ++rawLiterals;
              continue;
            }
            if (f[q] != '"' || rawLiterals > 0)
            {
              literals = false;
              break;
            }
            size_t e = q + 1;
            while (e < f.size() && f[e] != '"')
              e += f[e] == '\\' ? 2 : 1;
            raw += f.substr(q + 1, e - q - 1);
            q = e + 1;
          }
          std::vector<std::string> segments;
          std::vector<PrintfConversion> conversions;
          std::string unsupported;
          size_t following = texts.size() - formatIndex - 1;
          size_t starsHere = 0;
          auto literal = [&](const std::string& x)
          { return rawLiterals > 0 ? "R\"" + rawDelimiter + "(" + x + ")" + rawDelimiter + "\"" : "\"" + x + "\""; };
          if (literals && splitPrintfFormat(raw, rawLiterals > 0, segments, conversions, unsupported) &&
              conversions.size() + (starsHere = size_t(std::count_if(conversions.begin(), conversions.end(), [](const PrintfConversion& c) { return c.starWidth; }))) == following &&
              !(word != "ssprintf" && !printLike && conversions.empty() && following == 0 && raw.find('%') == std::string::npos))
          {
            std::vector<std::string> pieces;
            bool ok = true;
            for (size_t n = 0, a = formatIndex + 1; n < conversions.size() && ok; ++n)
            {
              std::string o, cl;
              uint32_t noDot = UINT32_MAX;
              std::string widthText = conversions[n].starWidth ? texts[a++] : std::string();
              const std::string& value = texts[a++];
              PrintfConversion conversion = conversions[n];
              if (conversion.conversion == 's' && !value.empty() && (value[0] == '"' || (value[0] == 'R' && value.size() > 1 && value[1] == '"')))
              {
                // (a literal: itself, aligned as asked)
                ok = printfPiece(conversion, nullptr, 0, !printLike, o, cl, noDot, widthText);
                if (ok && o.compare(0, 11, "StringView(") == 0)
                  o.erase(0, 11), cl.erase(0, 1);
                else if (ok && o.find("StringView(") != std::string::npos)
                {
                  size_t at = o.find("StringView(");
                  o.erase(at, 11);
                  cl.erase(0, 1);
                }
              }
              else
                ok = printfPiece(conversion, nullptr, 0, !printLike, o, cl, noDot, widthText);
              if (ok)
              {
                if (!segments[n].empty())
                  pieces.push_back(literal(segments[n]));
                pieces.push_back(o + value + cl);
              }
            }
            if (ok)
            {
              if (!segments.back().empty() || pieces.empty())
                pieces.push_back(literal(segments.back()));
              for (size_t a = 0; a < formatIndex; ++a)
                rewritten += texts[a] + ", ";
              for (size_t n = 0; n < pieces.size(); ++n)
                rewritten += (n ? ", " : "") + pieces[n];
              done = true;
              counts.add("printf calls in macro expansions written with pieces");
            }
          }
          else if (!literals && word != "ssprintf" && !printLike)
          {
            // (a run-time format, as rewritePrintfCall writes it)
            for (size_t a = 0; a < formatIndex; ++a)
              rewritten += texts[a] + ", ";
            if (following == 0)
              rewritten += "StringView(" + f + ")";
            else
            {
              rewritten += "ssprintf(" + f;
              for (size_t a = formatIndex + 1; a < texts.size(); ++a)
                rewritten += ", " + texts[a];
              rewritten += ")";
            }
            done = true;
            counts.add("format function calls of a run-time format in macro expansions written ssprintf(format, ...)");
          }
        }
        if (!done)
        {
          callee = word;
          rewritten.clear();
          for (size_t a = 0; a < texts.size(); ++a)
            rewritten += (a ? ", " : "") + texts[a];
          if (printLike)
            counts.add("printf calls in macro expansions left as they are");
          else if (word != "ssprintf")
            counts.add("format function calls in macro expansions left as they are");
        }
        out += callee + t.substr(j, open - j) + "(" + rewritten + ")";
        i = k + 1;
      }
      return out;
    }
    std::vector<Edit> backEdits; // inserts that go after every other edit at their offset
    std::map<uint32_t, std::vector<size_t>> widenedFormats; // a format's token: the edits widenFormatArguments made for it
    std::set<CallExpr*> printfRewritten;
    // The game's own format functions (`..., const char* format, ...)` formatting through
    // CommonUtil's vssprintf): each takes a Text instead (convertFormatFunction), its calls the
    // pieces. Recognized alike in every file: C-variadic, the last named parameter `format`, a name of
    // these.
    static bool isFormatFunctionName(std::string_view name)
    {
      static const std::set<std::string_view> names = {"log", "logVerbose", "logAndAbortOrThrow", "addDebugProperty", "makeProperty",
                                                       "make", "printf", "TagWriter"};
      return names.count(name) > 0;
    }
    bool isFormatFunction(Decl* d) const
    {
      if (!d || d->kind != NodeKind::FunctionDecl || !(d->flags & DF_Variadic))
        return false;
      auto* f = static_cast<FunctionDecl*>(d);
      if (f->name.empty() || f->name.last().symbol == UINT32_MAX || f->params.empty() || !isFormatFunctionName(symbols.name(f->name.last().symbol)))
        return false;
      Decl* last = f->params[f->params.size() - 1];
      return last->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(last)->name != UINT32_MAX &&
             symbols.name(static_cast<ParamDecl*>(last)->name) == "format";
    }
    // A format function's declaration or definition (isFormatFunction): `const char* format, ...` is
    // `Text text`, and its body's `va_list args; va_start(args, format); X = vssprintf(format, args);
    // va_end(args);` is `X = toString(text);`, where it was (TagWriter's after its early return: the
    // text is written only where the C++ wrote it). Another use of the format or its va_list is an
    // error, not a silent fallback.
    void convertFormatFunction(FunctionDecl* f)
    {
      uint32_t open = f->token;
      while (open < toks.size() && !toks[open].is(P::LParen))
        ++open;
      uint32_t ellipsis = UINT32_MAX, lastComma = UINT32_MAX, beforeFormat = open;
      for (uint32_t k = open, depth = 0; k < toks.size(); ++k)
      {
        if (toks[k].is(P::LParen))
          ++depth;
        else if (toks[k].is(P::RParen) && --depth == 0)
          break;
        else if (depth == 1 && toks[k].is(P::Comma))
        {
          if (lastComma != UINT32_MAX)
            beforeFormat = lastComma;
          lastComma = k;
        }
        else if (depth == 1 && toks[k].is(P::Ellipsis))
        {
          ellipsis = k;
          break;
        }
      }
      if (ellipsis == UINT32_MAX || lastComma == UINT32_MAX)
      {
        todo(toks[f->token].offset, "a format function whose `format, ...` the converter cannot find");
        return;
      }
      replace(toks[beforeFormat + 1].offset, endOffset(ellipsis) - toks[beforeFormat + 1].offset, "Text text");
      counts.add("format functions taking a Text");
      if (!f->body || f->endToken <= f->body->token || f->endToken > toks.size())
        return;
      uint32_t first = f->body->token, end = f->endToken;
      // the statements: va_list NAME; va_start(NAME, format); va_end(NAME);
      std::string list;
      auto removeStatement = [&](uint32_t from, uint32_t semicolon)
      {
        uint32_t start = toks[from].offset, stop = endOffset(semicolon);
        size_t lineStart = text.rfind('\n', start == 0 ? 0 : start - 1);
        lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
        if (text.substr(lineStart, start - lineStart).find_first_not_of(" \t") == std::string_view::npos)
        {
          size_t lineEnd = text.find('\n', stop);
          if (lineEnd != std::string_view::npos && text.substr(stop, lineEnd - stop).find_first_not_of(" \t\r") == std::string_view::npos)
            start = uint32_t(lineStart), stop = uint32_t(lineEnd + 1);
        }
        remove(start, stop - start);
      };
      bool replaced = false;
      std::set<uint32_t> handled;
      for (uint32_t k = first; k + 1 < end; ++k)
      {
        std::string_view word = spelling(k);
        if (word == "va_list" && k + 2 < end && toks[k + 2].is(P::Semi))
        {
          list = std::string(spelling(k + 1));
          removeStatement(k, k + 2);
          for (uint32_t h = k; h <= k + 2; ++h)
            handled.insert(h);
        }
        else if ((word == "va_start" || word == "va_end") && toks[k + 1].is(P::LParen))
        {
          uint32_t semicolon = k + 1;
          while (semicolon < end && !toks[semicolon].is(P::Semi))
            ++semicolon;
          removeStatement(k, semicolon);
          for (uint32_t h = k; h <= semicolon; ++h)
            handled.insert(h);
        }
        else if (word == "vssprintf" && k + 5 < end && toks[k + 1].is(P::LParen) && spelling(k + 2) == "format" && toks[k + 3].is(P::Comma) &&
                 spelling(k + 4) == list && toks[k + 5].is(P::RParen))
        {
          replace(toks[k].offset, endOffset(k + 5) - toks[k].offset,
                  std::string(shadowedAt("toString", toks[k].offset) ? "::toString" : "toString") + "(text)");
          replaced = true;
          for (uint32_t h = k; h <= k + 5; ++h)
            handled.insert(h);
        }
      }
      for (uint32_t k = first; k < end; ++k)
        if (!handled.count(k) && toks[k].getKind() == TokenKind::Identifier && (spelling(k) == "format" || (!list.empty() && spelling(k) == list)))
        {
          todo(toks[k].offset, "a format function's body that uses its format otherwise than through vssprintf");
          return;
        }
      if (!replaced)
        todo(toks[f->token].offset, "a format function's body without its vssprintf");
    }
    // design.md "struct and class" (option 3, kovarex 2026-11-01): a struct is a public value type. A
    // struct of the C++ with a private or protected member, a base that is not public, a virtual
    // function or a polymorphic base is a class: `class`, then `public:` first so that its members
    // keep the C++'s access, and `public` before each base written without an access (a class's bases
    // start private). Its friends stay.
    static bool declaresVirtualFunction(const ClassDecl* c)
    {
      for (Decl* m : c->members)
        if (m && m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override | DF_Final | DF_Pure)))
          return true;
      return false;
    }
    bool polymorphicClass(const ClassDecl* c, int depth)
    {
      if (!c || depth > 16)
        return false;
      if (declaresVirtualFunction(c))
        return true;
      for (const ClassDecl::Base& b : c->bases)
        if (b.type && b.type->kind == NodeKind::NamedType)
          if (Decl* d = static_cast<NamedType*>(b.type)->resolved)
          {
            Decl* def = global.definitionOf(d);
            if (def && def->kind == NodeKind::ClassDecl && polymorphicClass(static_cast<ClassDecl*>(def), depth + 1))
              return true;
          }
      return false;
    }
    void structAsClass(ClassDecl* c)
    {
      if (in.c || c->keyword != Kw::struct_ || !c->isDefinition || c->isClosure || (c->flags & DF_Friend))
        return;
      bool privateMember = false, privateBase = false, polymorphicBase = false;
      for (Decl* m : c->members)
        privateMember = privateMember || (m && !(m->flags & DF_Friend) && (m->access == Access::Private || m->access == Access::Protected));
      for (const ClassDecl::Base& b : c->bases)
      {
        privateBase = privateBase || b.access == Access::Private || b.access == Access::Protected;
        if (b.type && b.type->kind == NodeKind::NamedType)
          if (Decl* d = static_cast<NamedType*>(b.type)->resolved)
            if (Decl* def = global.definitionOf(d); def && def->kind == NodeKind::ClassDecl)
              polymorphicBase = polymorphicBase || polymorphicClass(static_cast<ClassDecl*>(def), 1);
      }
      bool isVirtual = declaresVirtualFunction(c);
      if (!privateMember && !privateBase && !isVirtual && !polymorphicBase)
        return;
      // The keyword, after any `template<...>`.
      uint32_t k = c->token;
      while (k < toks.size() && !toks[k].is(Kw::struct_) && !toks[k].is(P::LBrace))
        ++k;
      if (k >= toks.size() || !toks[k].is(Kw::struct_))
        return;
      // The bases written without an access, and the body's `{`.
      uint32_t brace = k;
      bool inBases = false;
      for (int depth = 0; ++brace < toks.size();)
      {
        const Token& t = toks[brace];
        if (t.is(P::LParen) || t.is(P::LBracket) || t.is(P::Less))
          ++depth;
        else if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::Greater))
          --depth;
        else if (t.is(P::Shr))
          depth -= 2;
        else if (depth <= 0 && t.is(P::LBrace))
          break;
        else if (depth <= 0 && (t.is(P::Colon) || (inBases && t.is(P::Comma))))
        {
          inBases = true;
          uint32_t first = brace + 1;
          while (first < toks.size() && toks[first].is(Kw::virtual_))
            ++first;
          if (first < toks.size() && !toks[first].is(Kw::public_) && !toks[first].is(Kw::private_) && !toks[first].is(Kw::protected_))
          {
            insert(toks[first].offset, "public ");
            counts.add("struct bases written public (structs written as classes)");
          }
        }
      }
      if (brace >= toks.size())
        return;
      replace(toks[k].offset, toks[k].length, "class");
      if (!(brace + 2 < toks.size() && toks[brace + 1].is(Kw::public_) && toks[brace + 2].is(P::Colon)))
      {
        size_t lineStart = text.rfind('\n', toks[k].offset);
        lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
        size_t indentEnd = text.find_first_not_of(" \t", lineStart);
        std::string indent(text.substr(lineStart, indentEnd - lineStart));
        insert(endOffset(brace), "\n" + indent + "public:");
      }
      counts.add(std::string("structs written as classes (design.md \"struct and class\"): ") +
                 (isVirtual ? "a virtual function" : privateMember ? "a private or protected member" : privateBase ? "a base that is not public" : "a polymorphic base"));
    }

    // How std::to_string writes a value of a type, by the overload C++ calls for it: 'i' an integer's
    // digits, as kpp's toString writes them; 'f' a floating value; '?' a bool, a plain char, an
    // enumeration (each promoted to int, where kpp's toString writes true, the character, the
    // enumerator's name) or a type the declarations do not resolve.
    char toStringKind(Type* type, int depth = 0)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType || type->kind == NodeKind::RRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner
             : type->kind == NodeKind::LRefType  ? static_cast<LRefType*>(type)->referenced
                                                 : static_cast<RRefType*>(type)->referenced;
      if (!type || depth > 8)
        return '?';
      if (type->kind == NodeKind::BuiltinType)
      {
        auto* builtin = static_cast<BuiltinType*>(type);
        if (builtin->keyword == Kw::float_ || builtin->keyword == Kw::double_)
          return 'f';
        if (builtin->keyword == Kw::char_ && !builtin->isSigned && !builtin->isUnsigned)
          return '?';
        return builtin->keyword == Kw::int_ || builtin->keyword == Kw::char_ || builtin->keyword == Kw::short_ || builtin->keyword == Kw::long_ ||
                   builtin->keyword == Kw::signed_ || builtin->keyword == Kw::unsigned_
                 ? 'i'
                 : '?';
      }
      if (type->kind != NodeKind::NamedType)
        return '?';
      Decl* d = static_cast<NamedType*>(type)->resolved;
      if (d && d->kind == NodeKind::UsingAliasDecl)
        return toStringKind(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return toStringKind(static_cast<VariableDecl*>(d)->type, depth + 1);
      return '?';
    }
    // std::to_string(x) by x's type (toStringKind): an integer's is toString(x) (the std:: rename); a
    // floating value's is C's %f, six decimals (`0.100000`), where kpp's toString is the shortest
    // text (docs/stdlib/text.md), so toString(fixed(x, 6)), the fixed inside the call's own
    // parentheses; the rest's is toStringAsStd(x) (overrides/added/CommonUtil/ToStringAsStd.kpp),
    // which picks among those by the type the compiler knows.
    void stdToString(CallExpr* call)
    {
      if (in.c || call->args.size() != 1 || call->callee->kind != NodeKind::NameExpr)
        return;
      const QualifiedName& name = static_cast<NameExpr*>(call->callee)->name;
      if (name.parts.size() != 2 || name.parts[0].symbol == UINT32_MAX || name.parts[1].symbol == UINT32_MAX ||
          symbols.name(name.parts[0].symbol) != "std" || symbols.name(name.parts[1].symbol) != "to_string")
        return;
      char kind = toStringKind(plainTypeOf(call->args[0]));
      if (kind == 'i')
      {
        counts.add("std::to_string of an integer written toString(x)");
        return;
      }
      uint32_t first = firstTokenOf(call->callee);
      uint32_t open = first;
      while (open < toks.size() && !toks[open].is(P::LParen))
        ++open;
      std::vector<std::pair<uint32_t, uint32_t>> ranges = open < toks.size() ? argumentRanges(open + 1) : std::vector<std::pair<uint32_t, uint32_t>>();
      if (ranges.size() != 1 || !toks[ranges[0].second].is(P::RParen))
      {
        todo(toks[first].offset, "std::to_string whose argument the converter does not find");
        return;
      }
      if (kind == 'f')
      {
        std::string fixed = shadowedAt("fixed", toks[open].offset) ? "::fixed(" : "fixed(";
        replace(toks[open].offset, toks[open].length, "(" + fixed);
        replace(toks[ranges[0].second].offset, toks[ranges[0].second].length, ", 6))");
        counts.add("std::to_string of a floating value written toString(fixed(x, 6))");
        return;
      }
      // (the std:: rename's edit, `toString`, gets the name)
      for (Edit& e : edits)
        if (e.offset == toks[first].offset && e.length && e.text.size() >= 8 && e.text.compare(e.text.size() - 8, 8, "toString") == 0)
        {
          e.text += "AsStd";
          counts.add("std::to_string of a bool, char, enumeration or unknown type written toStringAsStd(x)");
          return;
        }
      todo(toks[first].offset, "std::to_string without the std:: rename's edit");
    }
    bool rewritePrintfCall(CallExpr* call)
    {
      if (call->args.empty())
        return false;
      // the callee: ssprintf, a printf statement, a Log macro or SAVING_TAG, or a format function
      Decl* resolved = call->callee->kind == NodeKind::NameExpr     ? static_cast<NameExpr*>(call->callee)->resolved
                     : call->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(call->callee)->resolved
                                                                  : nullptr;
      std::string_view name;
      if (call->callee->kind == NodeKind::NameExpr && static_cast<NameExpr*>(call->callee)->name.parts.size() == 1 &&
          static_cast<NameExpr*>(call->callee)->name.parts[0].symbol != UINT32_MAX)
        name = symbols.name(static_cast<NameExpr*>(call->callee)->name.parts[0].symbol);
      static const std::map<std::string_view, size_t> formatMacros = {
        {"LOG_INFO", 0},    {"LOG_NOTICE", 0}, {"LOG_WARNING", 0}, {"LOG_ERROR", 0}, {"LOG_DATA", 0},     {"LOG_GRAPHICS", 0}, {"LOG_FUTURE", 0},
        {"LOG_VERBOSE", 0}, {"LOG_AND_ABORT", 0}, {"LOG_AND_ABORT_NEVER_THROW", 0}, {"LOG", 1}, {"SAVING_TAG", 2}};
      std::string to;
      size_t formatIndex = 0;
      bool trio = true; // (formatted through CommonUtil's vssprintf: trio's floating text)
      // (C: printf and fprintf, the converted C libraries' too; their own variadic interfaces,
      // trio's, gzprintf, stb's writef, keep C's `...`, and a write into a buffer, snprintf and its
      // relatives, has no piece)
      if (in.c && name != "printf" && name != "fprintf")
        return false;
      if (name == "ssprintf")
        to = "toString";
      else if (name == "printf" && !resolved)
        to = "print", trio = false;
      else if (name == "fprintf" && !resolved)
        to = "print", trio = false, formatIndex = 1; // print(stream, pieces...), as C++23's std::print(FILE*, ...)
      else if (auto macro = !resolved && !name.empty() ? formatMacros.find(name) : formatMacros.end(); macro != formatMacros.end())
        formatIndex = macro->second;
      else if (isFormatFunction(resolved))
        formatIndex = static_cast<FunctionDecl*>(resolved)->params.size() - 1;
      else
        return false;
      if (formatIndex >= call->args.size())
        return false;
      // (a format function's call with a format of no conversion and nothing after it: as it is,
      // the overload of a string takes it)
      Expr* format = call->args[formatIndex];
      bool stringMacroFirst = format->kind == NodeKind::NameExpr && format->token < toks.size() &&
                              toks[format->token].getKind() == TokenKind::Identifier && objectMacros.count(std::string(spelling(format->token))) &&
                              isStringLiterals(objectMacros.at(std::string(spelling(format->token))));
      // A format function's (or macro's) format the pieces do not write: one chosen at run time
      // (TagWriter's `this->statistics ? "true" : "false"`), or literals the rewrite cannot read (a C
      // header's string macro among them, SDL_HINT_AUDIO_DRIVER's): its text as C++ wrote it,
      // ssprintf's (trio's) at run time, one piece; with nothing after it, the text itself.
      auto runtimeFormat = [&]() -> bool
      {
        if (!to.empty())
          return false;
        // (a std::string there is no format: `LOG_AND_ABORT(ssprintf(...))` took the overload of a
        // string in C++, as it does here)
        if (isStdStringExpr(format))
          return false;
        uint32_t formatFirst = cFirstToken(format);
        std::vector<std::pair<uint32_t, uint32_t>> all = argumentRanges(formatFirst);
        if (all.size() != call->args.size() - formatIndex)
          return false;
        uint32_t paren = all.back().second;
        if (paren >= toks.size() || !toks[paren].is(P::RParen))
          return false;
        bool alone = all.size() == 1;
        if (alone && format->kind == NodeKind::LiteralExpr)
          return false;
        // (before the format, not at it: a rewrite of the format itself, ssprintf's of a run-time
        // format inside, replaces it and would cancel an insertion there)
        if (formatFirst == 0)
          return false;
        uint32_t after = endOffset(formatFirst - 1);
        Edit* ending = nullptr; // (a rewrite of the token before, a Log macro's name and `(`: the opening goes into its text)
        for (Edit& e : edits)
          if (e.length > 0 && e.offset < after && e.offset + e.length == after)
            ending = &e;
        if (ending)
          ending->text += alone ? "StringView(" : "ssprintf(";
        else if (after < toks[formatFirst].offset)
          replace(after, toks[formatFirst].offset - after, std::string(" ") + (alone ? "StringView(" : "ssprintf("));
        else
          insert(after, alone ? "StringView(" : "ssprintf(");
        uint32_t at = endOffset(paren - 1);
        Edit* parenRewrite = nullptr;
        for (Edit& e : edits)
          if (e.offset == toks[paren].offset && e.length > 0)
            parenRewrite = &e;
        if (parenRewrite && at == toks[paren].offset)
          parenRewrite->text = ")" + parenRewrite->text;
        else
          backEdits.push_back({at, 0, ")"});
        printfRewritten.insert(call);
        counts.add(alone ? "format function calls of a run-time format alone written StringView(format)"
                         : "format function calls of a run-time format written ssprintf(format, ...)");
        return true;
      };
      if (!stringMacroFirst && (format->kind != NodeKind::LiteralExpr || static_cast<LiteralExpr*>(format)->literalKind != TokenKind::String))
        return runtimeFormat();
      // the raw text of the format's literals (and of string macros among them)
      std::string raw, rawDelimiter;
      int rawLiterals = 0;
      uint32_t last = format->token;
      for (; last < toks.size(); ++last)
      {
        std::string piece;
        const std::string* blanked = nullptr; // (a C header's string macro the parse read as its text: SDL_HINT_AUDIO_DRIVER)
        std::string blankedText;
        if (preprocessed && toks[last].getKind() == TokenKind::Identifier)
          for (const Preprocessed::Blank& b : preprocessed->blanked)
            if (b.offset == toks[last].offset && b.length >= toks[last].length)
            {
              size_t x = b.replacement.find_first_not_of(" \t"), y = b.replacement.find_last_not_of(" \t");
              if (x != std::string::npos && isStringLiterals(b.replacement.substr(x, y - x + 1)))
                blankedText = b.replacement.substr(x, y - x + 1), blanked = &blankedText;
            }
        if (toks[last].getKind() == TokenKind::String)
          piece = std::string(spelling(last));
        else if (blanked)
          piece = *blanked;
        else if (auto macro = toks[last].getKind() == TokenKind::Identifier ? objectMacros.find(std::string(spelling(last))) : objectMacros.end();
                 macro != objectMacros.end() && isStringLiterals(macro->second))
          piece = macro->second;
        else
          break;
        for (size_t q = 0; q < piece.size();)
        {
          if (piece[q] == ' ' || piece[q] == '\t' || piece[q] == '\n' || piece[q] == '\r')
          {
            ++q;
            continue;
          }
          if (piece[q] == 'R' && q + 1 < piece.size() && piece[q + 1] == '"')
          {
            // a raw literal (`R"(...)"`, the tests' Lua): its pieces are raw literals of its delimiter
            size_t open = piece.find('(', q + 2);
            if (open == std::string::npos || (rawLiterals == 0 && !raw.empty()) || (rawLiterals > 0 && piece.substr(q + 2, open - q - 2) != rawDelimiter))
            {
              if (runtimeFormat())
                return true;
              counts.add("printf calls left as they are (raw and plain literals together)");
              return false;
            }
            rawDelimiter = piece.substr(q + 2, open - q - 2);
            size_t closeAt = piece.find(")" + rawDelimiter + "\"", open + 1);
            if (closeAt == std::string::npos)
              return false;
            raw += piece.substr(open + 1, closeAt - open - 1);
            q = closeAt + rawDelimiter.size() + 2;
            ++rawLiterals;
            continue;
          }
          if (piece[q] != '"' || rawLiterals > 0)
          {
            if (runtimeFormat())
              return true;
            counts.add(rawLiterals > 0 ? "printf calls left as they are (raw and plain literals together)" : "printf calls left as they are (a prefixed literal)");
            return false;
          }
          size_t e = q + 1;
          while (e < piece.size() && piece[e] != '"')
            e += piece[e] == '\\' ? 2 : 1;
          raw += piece.substr(q + 1, e - q - 1);
          q = e + 1;
        }
      }
      --last;
      std::vector<std::string> segments;
      std::vector<PrintfConversion> conversions;
      std::string unsupported;
      if (!splitPrintfFormat(raw, rawLiterals > 0, segments, conversions, unsupported))
      {
        if (runtimeFormat())
          return true;
        counts.add("printf calls left as they are (" + unsupported + ")");
        return false;
      }
      std::vector<std::pair<uint32_t, uint32_t>> ranges =
        last + 1 < toks.size() && toks[last + 1].is(P::Comma) ? argumentRanges(last + 2) : std::vector<std::pair<uint32_t, uint32_t>>();
      if (to.empty() && conversions.empty() && ranges.empty() && raw.find('%') == std::string::npos && call->args.size() == formatIndex + 1)
        return false;
      // (a `*` width reads an argument before its value: the value's range, and the width's text,
      // one name or number, which the piece takes)
      size_t stars = 0;
      for (const PrintfConversion& c : conversions)
        stars += c.starWidth;
      std::vector<std::pair<uint32_t, uint32_t>> all = ranges;
      std::vector<size_t> valueArgument(conversions.size());
      std::vector<std::string> widthTexts(conversions.size());
      bool widthsKnown = all.size() == conversions.size() + stars;
      if (stars && widthsKnown)
      {
        ranges.clear();
        for (size_t k = 0, r = 0; k < conversions.size(); ++k)
        {
          if (conversions[k].starWidth)
          {
            auto [b, e] = all[r++];
            if (e == b + 1 && (toks[b].getKind() == TokenKind::Identifier || toks[b].getKind() == TokenKind::Number))
              widthTexts[k] = std::string(spelling(b));
            else if (e == b + 2 && toks[b].is(P::Minus) && toks[b + 1].getKind() == TokenKind::Number)
              widthTexts[k] = "-" + std::string(spelling(b + 1));
            else
              widthsKnown = false;
          }
          valueArgument[k] = r;
          ranges.push_back(all[r++]);
        }
      }
      else
        for (size_t k = 0; k < conversions.size(); ++k)
          valueArgument[k] = k;
      if (all.size() != call->args.size() - 1 - formatIndex || !widthsKnown || conversions.size() != ranges.size())
      {
        if (runtimeFormat())
          return true;
        counts.add("printf calls left as they are (arguments not one per conversion, or a macro's)");
        return false;
      }
      if (to == "print")
      {
        // (a statement: print returns no count)
        uint32_t first = firstTokenOf(call->callee), close = ranges.empty() ? last + 1 : ranges.back().second;
        if (first == 0 || !(toks[first - 1].is(P::Semi) || toks[first - 1].is(P::LBrace) || toks[first - 1].is(P::RBrace) || toks[first - 1].is(P::RParen) ||
                            toks[first - 1].is(Kw::else_) || toks[first - 1].is(P::Colon)) ||
            !(close < toks.size() && toks[close].is(P::RParen) && close + 1 < toks.size() && toks[close + 1].is(P::Semi)))
          return false;
      }
      std::vector<std::string> opens(conversions.size()), closes(conversions.size());
      std::vector<uint32_t> cStrDots(conversions.size(), UINT32_MAX);
      for (size_t k = 0; k < conversions.size(); ++k)
        if (!printfPiece(conversions[k], call->args[valueArgument[k] + 1 + formatIndex], ranges[k].second, trio, opens[k], closes[k], cStrDots[k],
                         widthTexts[k]))
        {
          if (runtimeFormat())
            return true;
          counts.add("printf calls left as they are (a + flag on %" + std::string(1, conversions[k].conversion) + ")");
          return false;
        }
      // widenFormatArguments' casts for this format go: the pieces say the width
      if (auto widened = widenedFormats.find(format->token); widened != widenedFormats.end())
        for (size_t index : widened->second)
        {
          Edit& e = edits[index];
          e.text = e.length == 0 ? std::string() : std::string(text.substr(e.offset, e.length));
        }
      auto literal = [&](const std::string& s) { return rawLiterals > 0 ? "R\"" + rawDelimiter + "(" + s + ")" + rawDelimiter + "\"" : "\"" + s + "\""; };
      // (the library's names, `::toString` where the program's own hides it: NoiseProgram's
      // toString(), Color's hex(), DebugTools' parameter toString)
      uint32_t callAt = toks[firstTokenOf(call->callee)].offset;
      auto qualified = [&](std::string text)
      {
        static const char* names[] = {"toString", "print", "hex", "hexUpper", "octal", "fixed", "significant", "scientific", "trioFixed",
                                      "trioSignificant", "trioScientific", "withSign",
                                      "alignLeft", "alignRight", "zeroPadded", "address", "StringView"};
        for (const char* name : names)
          for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1))
          {
            size_t end = at + strlen(name);
            if ((at > 0 && (isIdentChar(text[at - 1]) || text[at - 1] == ':')) || end >= text.size() || text[end] != '(')
              continue;
            if (shadowedAt(name, callAt))
            {
              text.insert(at, "::");
              at += 2;
            }
          }
        return text;
      };
      if (!to.empty())
      {
        to = qualified(to + "(");
        to.pop_back();
      }
      for (std::string& open : opens)
        open = qualified(open);
      // the callee (also written `::printf`: InteractiveTestSelect has a printf of its own); a
      // format function's and a macro's stay
      if (!to.empty())
      {
        uint32_t calleeFirst = firstTokenOf(call->callee), calleeLast = calleeFirst;
        while (calleeLast + 1 < toks.size() && !toks[calleeLast + 1].is(P::LParen))
          ++calleeLast;
        replace(toks[calleeFirst].offset, endOffset(calleeLast) - toks[calleeFirst].offset, to);
      }
      // The format and the comma and spaces after it become its first segment and the first piece's
      // opening; each later comma the segment before its piece and that piece's opening. (Before
      // the argument, not at it: a rewrite of the argument itself, a macro written out or a
      // variant's access, replaces it and would cancel an insertion there.)
      uint32_t formatStart = toks[format->token].offset, formatEnd = endOffset(last);
      if (conversions.empty())
        replace(formatStart, formatEnd - formatStart, literal(segments[0]));
      else
        replace(formatStart, toks[ranges[0].first].offset - formatStart, (segments[0].empty() ? std::string() : literal(segments[0]) + ", ") + opens[0]);
      for (size_t k = 0; k < conversions.size(); ++k)
      {
        if (k > 0)
        {
          uint32_t comma = toks[ranges[k - 1].second].offset;
          replace(comma, toks[ranges[k].first].offset - comma,
                  closes[k - 1] + ", " + (segments[k].empty() ? std::string() : literal(segments[k]) + ", ") + opens[k]);
        }
        // (the last piece's closing after its argument; the others' are in the next comma's text)
        std::string after = closes[k] + (!segments[k + 1].empty() ? ", " + literal(segments[k + 1]) : std::string());
        if (k + 1 == conversions.size() && !after.empty())
        {
          // (before the call's `)`, which a macro's rewrite may replace: `LOG_ERROR(...)`'s is the
          // expansion's, which would cancel an insertion there; then into its text)
          uint32_t at = endOffset(ranges[k].second - 1), paren = toks[ranges[k].second].offset;
          Edit* parenRewrite = nullptr;
          for (Edit& e : edits)
            if (e.offset == paren && e.length > 0)
              parenRewrite = &e;
          if (parenRewrite && at == paren)
            parenRewrite->text = after + parenRewrite->text;
          else
            backEdits.push_back({at, 0, after});
        }
        if (cStrDots[k] != UINT32_MAX)
          remove(toks[cStrDots[k]].offset, endOffset(cStrDots[k] + 3) - toks[cStrDots[k]].offset);
      }
      printfRewritten.insert(call);
      counts.add(name == "ssprintf" ? "ssprintf calls written toString(pieces)"
                 : to == "print"     ? "printf statements written print(pieces)"
                 : !resolved         ? "Log and SAVING_TAG macro calls written with pieces"
                                     : "calls of the game's format functions written with pieces");
      return true;
    }
    // Factorio's own format mismatches, made explicit as C++ prints them on x64: an argument the
    // declarations say is 64-bit, read by a 32-bit conversion (`%u` of a UnitNumber or a size_t
    // index, `#%ull`), is written uint32_t(...) (int32_t for %d and %i), the 32 bits printf reads
    // of it; arguments after the last the format reads are dropped (C++ evaluates and ignores
    // them; the one the game has, a toUint32(), has no effect).
    void narrowFormatArguments(CallExpr* call)
    {
      // A C-variadic function's call whose last named parameter is the format (ssprintf, printf,
      // snprintf), as kppc checks them: no other call's literal is a format.
      Expr* callee = call->callee;
      Decl* d = callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(callee)->resolved
              : callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                     : nullptr;
      // The C library's, which the C++ parse does not declare: by name, with the format's place.
      static const std::map<std::string_view, size_t> cFunctions = {{"printf", 0}, {"sprintf", 1}, {"snprintf", 2}, {"fprintf", 1}};
      size_t formatIndex;
      std::string_view calleeName;
      if (d && d->kind == NodeKind::FunctionDecl && (d->flags & DF_Variadic) && !static_cast<FunctionDecl*>(d)->params.empty())
      {
        formatIndex = static_cast<FunctionDecl*>(d)->params.size() - 1;
        calleeName = symbols.name(static_cast<FunctionDecl*>(d)->name.last().symbol);
      }
      else if (auto c = !d && callee->kind == NodeKind::NameExpr && static_cast<NameExpr*>(callee)->name.parts.size() == 1
                          ? cFunctions.find(symbols.name(static_cast<NameExpr*>(callee)->name.parts[0].symbol))
                          : cFunctions.end();
               c != cFunctions.end())
      {
        formatIndex = c->second;
        calleeName = c->first;
      }
      else
        return;
      for (size_t a = formatIndex; a < call->args.size() && a == formatIndex; ++a)
      {
        Expr* arg = call->args[a];
        if (!arg || arg->kind != NodeKind::LiteralExpr || static_cast<LiteralExpr*>(arg)->literalKind != TokenKind::String)
          continue;
        std::string format;
        uint32_t last = formatAt(arg->token, format);
        if (format.find('%') == std::string::npos && !(last + 1 < toks.size() && toks[last + 1].is(P::Comma)))
          return;
        std::vector<FormatConversion> conversions = formatConversions(format);
        std::vector<std::pair<uint32_t, uint32_t>> ranges =
          last + 1 < toks.size() && toks[last + 1].is(P::Comma) ? argumentRanges(last + 2) : std::vector<std::pair<uint32_t, uint32_t>>();
        size_t following = call->args.size() - a - 1;
        if (ranges.size() != following)
          return; // the tokens are not the call's arguments (a macro's): leave them
        for (size_t n = 0; n < conversions.size() && n < following; ++n)
          if (!conversions[n].wide && strchr("diuxXoc", conversions[n].conversion) && isWideInteger(call->args[a + 1 + n]))
          {
            castArgument(ranges[n].first, ranges[n].second, strchr("di", conversions[n].conversion) ? "int32_t" : "uint32_t");
            counts.add("64-bit printf arguments read as 32 bits written so");
          }
        if (following > 0 && format.find('%') == std::string::npos && callee->kind == NodeKind::NameExpr && calleeName == "ssprintf")
        {
          // A format that reads nothing: `ssprintf("text", x)` is the text (ssprintf without
          // arguments is deleted: CommonUtil's StringUtil.hpp).
          replace(toks[firstTokenOf(callee)].offset, toks[arg->token].offset - toks[firstTokenOf(callee)].offset, "String(");
          remove(toks[last + 1].offset, toks[ranges.back().second].offset - toks[last + 1].offset);
          counts.add("printf arguments the format does not read dropped");
        }
        // (Not in C: a C library's variadic function may read its own format, stb_image_write's
        // `stbiw__writef(s, "111 221 2222 11", 0, 0, format, ...)`, whose arguments are its data;
        // kppc's format check reports it instead: language/variadic_with_own_format.)
        else if (following > conversions.size() && !in.c)
        {
          uint32_t from = conversions.empty() ? last + 1 : ranges[conversions.size() - 1].second;
          remove(toks[from].offset, toks[ranges.back().second].offset - toks[from].offset);
          counts.add("printf arguments the format does not read dropped");
        }
        return;
      }
    }

    void convertTokens()
    {
      for (uint32_t i = 0; i + 2 < toks.size(); ++i)
      {
        const Token& t = toks[i];
        if (in.c && t.getKind() == TokenKind::Keyword && spelling(i) != keywordSpelling(Kw(t.id)))
        {
          // A C macro of one keyword (zutil.h's `local`, static): the keyword.
          replace(t.offset, t.length, std::string(keywordSpelling(Kw(t.id))));
          counts.add("C macros of one keyword written as it");
          continue;
        }
        if (in.c && preprocessed && t.getKind() == TokenKind::Identifier && spelling(i).size() >= 4 &&
            spelling(i).substr(spelling(i).size() - 4) == "NULL" && !memberAccessAt(i))
          if (auto macro = preprocessed->defined.find(std::string(spelling(i))); macro != preprocessed->defined.end() && macro->second == "0")
          {
            // C's null pointer by another name (zlib.h's `#define Z_NULL 0`): kpp imports the macro
            // as an int constant, which is no null pointer.
            replace(t.offset, t.length, "nullptr");
            counts.add("C null pointer macros written as nullptr");
            continue;
          }
        if (in.c && t.getKind() == TokenKind::Identifier && !memberAccessAt(i))
        {
          // Windows' TCHAR names, macros of its headers for the narrow or the wide function: the
          // narrow one, as the C libraries are built without UNICODE (minizip's iowin32.c).
          static const std::map<std::string_view, std::string_view> tcharNames = {
            {"CreateFile", "CreateFileA"}, {"DeleteFile", "DeleteFileA"}, {"MoveFile", "MoveFileA"}, {"GetFileAttributes", "GetFileAttributesA"},
            {"GetFileAttributesEx", "GetFileAttributesExA"}, {"FindFirstFile", "FindFirstFileA"}, {"FindNextFile", "FindNextFileA"},
            {"GetModuleFileName", "GetModuleFileNameA"}, {"GetModuleHandle", "GetModuleHandleA"}, {"LoadLibrary", "LoadLibraryA"},
            {"GetFullPathName", "GetFullPathNameA"}, {"CreateDirectory", "CreateDirectoryA"}, {"RemoveDirectory", "RemoveDirectoryA"},
            {"GetTempPath", "GetTempPathA"}, {"FormatMessage", "FormatMessageA"}, {"OutputDebugString", "OutputDebugStringA"},
            {"GetEnvironmentVariable", "GetEnvironmentVariableA"}, {"_tcslen", "strlen"}, {"_tcsncpy", "strncpy"}, {"_tcscpy", "strcpy"},
            {"_tcscat", "strcat"}, {"_tcschr", "strchr"}, {"_tcsrchr", "strrchr"}, {"_tcscmp", "strcmp"}, {"_tcsicmp", "_stricmp"},
            {"_sntprintf", "_snprintf"}, {"_stprintf", "sprintf"}, {"_tfopen", "fopen"}, {"_topen", "_open"}, {"_tstat", "_stat"}};
          if (auto narrow = tcharNames.find(spelling(i)); narrow != tcharNames.end())
          {
            replace(t.offset, t.length, std::string(narrow->second));
            counts.add("Windows' TCHAR names written as the narrow ones (C)");
            continue;
          }
        }
        // (A private name, private-names.txt's, is a type too: also in a class body, stb_truetype's
        // `struct stbrp_rect { stbrp_coord x,y; ... }`.)
        if (in.c && cStaticRenames && t.getKind() == TokenKind::Identifier && !memberAccessAt(i) &&
            (!inClassBody(t.offset) || (cPrivateNames && cPrivateNames->count(std::string(spelling(i))))))
          if (auto renamed = cStaticRenames->find(std::string(spelling(i))); renamed != cStaticRenames->end())
          {
            replace(t.offset, t.length, renamed->second);
            counts.add("C statics another file declares too renamed with their file's name");
            continue;
          }
        // A name of a scoped library the program uses (--convert-library), qualified by the library:
        // `zstd::ZSTD_compress(...)`, `zstd::ZSTD_inBuffer in = {}` (design.md 16).
        // (a global one, C++'s `::stbi_write_png_compression_level` in MemoryBitmap.cpp, is the
        // library's too: `stb_image::stbi_write_png_compression_level`)
        bool globalQualifier = i > 0 && toks[i - 1].is(P::ColonColon) &&
                               (i < 2 || !(toks[i - 2].getKind() == TokenKind::Identifier || toks[i - 2].is(P::Greater)));
        if (scopedLibraryNames && !scopedLibraryNames->empty() && t.getKind() == TokenKind::Identifier &&
            !(i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || (toks[i - 1].is(P::ColonColon) && !globalQualifier))))
          if (auto library = scopedLibraryNames->find(std::string(spelling(i)));
              library != scopedLibraryNames->end() && !shadowedAt(library->first, t.offset)) // (a local `compress`, BitmapUtil's)
          {
            if (globalQualifier)
              replace(toks[i - 1].offset, t.offset + t.length - toks[i - 1].offset, library->second + "::" + library->first);
            else
              replace(t.offset, t.length, library->second + "::" + library->first);
            counts.add("names of a scoped library qualified");
            continue;
          }
        if (in.c && t.getKind() == TokenKind::Identifier && spelling(i) == "config" && !memberAccessAt(i) && !inClassBody(t.offset))
        {
          // kpp's build configuration is the namespace `config` (config::debug): zlib's
          // `typedef struct config_s {...} config;` and opus's locals named so are renamed. A member
          // keeps its name (it is reached through `.` or `->`).
          replace(t.offset, t.length, "config_");
          counts.add("C names `config` renamed (kpp's configuration namespace)");
          continue;
        }
        if (in.c && t.getKind() == TokenKind::Identifier && lookupKeyword(spelling(i)) != UINT32_MAX)
        {
          // A C name that is a C++ keyword (trio's `class`, curl's `new`): kpp has the keyword.
          replace(t.offset, t.length, std::string(spelling(i)) + "_");
          counts.add("C names that are C++ keywords renamed");
          continue;
        }
        // (Not in C: its 64-bit arguments have their C types, and a scanf's are pointers, trio's
        // `trio_sscanf(buffer, "%lld", &value)`.)
        if (!in.c && t.getKind() == TokenKind::String && i > 0 && (toks[i - 1].is(P::LParen) || toks[i - 1].is(P::Comma)) &&
            !(i > 0 && toks[i - 1].getKind() == TokenKind::String))
          widenFormatArguments(i);
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
          // `(LPWSTR)&msg`, a C type's cast of an address or a dereference: kpp reads `(NAME) & x`
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
          // handle), which kpp reads as a subtraction from NAME, as above: `(HANDLE)(-1)`.
          insert(toks[i + 3].offset, "(");
          insert(endOffset(i + 4), ")");
          counts.add("casts of a negative number parenthesized");
        }
        // `X::value_type` (reference, pointer, difference_type, iterator_category) of an iterator type
        // X is IteratorTraits<X>'s, as std::iterator_traits gives them: CreatableEntityTypeIterator's
        // `using value_type = ListIterator::value_type;` with ListIterator a std::vector's const_iterator,
        // a pointer in kpp.
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
        if (in.c && cWrittenOutMacros && cWrittenOutMacros->count("*") && t.getKind() == TokenKind::Identifier && !(i > 0 && toks[i - 1].is(P::ColonColon)) &&
            !cNamedConstant(std::string(spelling(i))))
          if (const std::string* own = cOwnObjectAt(std::string(spelling(i)), t.offset))
          {
            // C: this file's own macro in force here (one of several definitions of the name)
            replace(t.offset, t.length, rescanMacros(cExpandedObject(*own, t.offset), t.offset));
            counts.add("C macro constants written out at their uses");
            continue;
          }
        // (In C a macro is one after `.` too: zlib's `tree[n].Freq` is `tree[n].fc.freq`.)
        if (t.getKind() == TokenKind::Identifier && !objectMacros.empty() &&
            !(i > 0 && (((toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow)) && !in.c) || toks[i - 1].is(P::ColonColon))))
          if (auto macro = objectMacros.find(std::string(spelling(i))); macro != objectMacros.end() && macroHoldsAt(macro->first, t.offset) &&
              !cNamedConstant(macro->first))
          {
            replace(t.offset, t.length, in.c ? rescanMacros(cExpandedObject(macro->second, t.offset), t.offset) : scopedQualified(macro->second));
            counts.add(isStringLiterals(macro->second)                                       ? "string macros written out"
                       : isQualifiedName(macro->second) || isAliasMacro(macro->first, macro->second) ? "name macros written out"
                                                                                                      : "spliced macros written out");
            continue;
          }
        if (t.getKind() == TokenKind::Identifier && headerAliases && !headerAliases->empty() &&
            !(i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon))))
          if (auto alias = headerAliases->find(std::string(spelling(i))); alias != headerAliases->end())
          {
            replace(t.offset, t.length, scopedQualified(alias->second));
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
          todo(t.offset, "using std::" + name + ": a std name the converter has no kpp name for");
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
          // The preprocessor's file name and line: the literal values, since kpp has no preprocessor.
          replace(t.offset, endOffset(i) - t.offset, withFileAndLine(std::string(spelling(i)), t.offset));
          counts.add("__FILE__, __LINE__, __FUNCTION__ replaced");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Vector" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's own 2D `Vector` class is Vector2D in kpp, where Vector is the library's std::vector.
          replace(t.offset, endOffset(i) - t.offset, "Vector2D");
          counts.add("Vector renamed to Vector2D");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Map" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's `class Map` is GameMap in kpp, where Map<K,V> is the library's std::map.
          replace(t.offset, endOffset(i) - t.offset, "GameMap");
          counts.add("Map renamed to GameMap");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Optional" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's sentinel-based `Optional<T, empty>` is GameOptional in kpp, where Optional<T> is the library's std::optional.
          replace(t.offset, endOffset(i) - t.offset, "GameOptional");
          counts.add("Optional renamed to GameOptional");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Pair" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's `Pair<K,V>` is GamePair in kpp, where Pair<A,B> is the library's std::pair.
          replace(t.offset, endOffset(i) - t.offset, "GamePair");
          counts.add("Pair renamed to GamePair");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "RuntimeError" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's `class RuntimeError : public std::runtime_error` is GameRuntimeError in kpp,
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
          // `value.~vector()`: the destructor of a std class, by the class's kpp name.
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
          // kpp Map's entries are Pair<K, V> (docs/stdlib/map.md), so the key's const goes. A const
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
            // kpp's caller location, which the compiler fills at each call (design.md section 7).
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
            replace(t.offset, endOffset(i + 14) - t.offset, "/* std::locale::global(std::locale::classic()): kpp converts text as the classic locale */");
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
            // value, which is the compiler's isMemberFunctionPointer<T>; kpp has no class form.
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
            // (TestFlatMap's MoveOnlyString), `using String::String;` in kpp.
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
            // Structured exception handling: kpp's exceptions are SEH underneath, so `try` catches
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
      if (!in.c)
        for (Decl* d : decls)
          if (isFormatFunction(d))
            convertFormatFunction(static_cast<FunctionDecl*>(d));
      for (Decl* d : decls)
      {
        if (namespaceScope && prefix.empty() && redeclaresScopedName(d))
        {
          // (ZipPackage.hpp's `struct unz_file_info64_s;` and `using unz_file_info64 = ...;`, written
          // so as not to include unzip.h: they declare minizip's own, now minizip::unz_file_info64)
          removeDecl(d);
          counts.add("forward declarations and aliases of a scoped library's names removed (the library's)");
          continue;
        }
        if (namespaceScope && overridden(d, prefix))
        {
          removeDefinition(d);
          counts.add("replaced by the override file");
          continue;
        }
        if ((d->flags & DF_Friend) && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::FunctionDecl))
        {
          // kpp keeps friend (design.md "struct and class", kovarex 2026-11-01): a friend class or
          // function as the C++ declares it, and a hidden friend, defined in its class, as C++ has it
          // (found by argument-dependent lookup, through a nested enum or class too).
          bool hidden = d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body;
          counts.add(hidden ? "hidden friends kept" : "friend declarations kept");
          if (!hidden)
            continue;
        }
        if (d->kind == NodeKind::DeductionGuideDecl)
        {
          removeDecl(d); // kpp deduces class arguments from constructors
          counts.add("deduction guides removed");
          continue;
        }
        // C++: a source file's names of its own, a `static` at the global namespace's scope and what
        // an anonymous namespace there declares, are `private` (design.md 15: a file's names; not in
        // a named namespace, where private is no kpp, nor in a header, which other files include).
        if (!in.c && namespaceScope && !isHeader() && (d->flags & DF_Static) && prefix.empty() &&
            (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) && cDeclName(d).size() &&
            !(d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->name.parts.size() > 1))
          writePrivate(d);
        if (!in.c && namespaceScope && !isHeader() && prefix == "::" &&
            (d->kind == NodeKind::FunctionDecl || d->kind == NodeKind::VariableDecl || d->kind == NodeKind::ClassDecl ||
             d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl) &&
            !(d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->name.parts.size() > 1))
          writePrivate(d);
        if (namespaceScope && (d->flags & DF_Static) && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) && isHeader() &&
            !cGivenHeader)
        {
          // `static` at namespace scope in a header gives every C++ file that includes it a copy
          // of its own; in kpp a header and its source are one file and a `static` name stays in
          // it, so the other files would not find it (EMPTY_CARGO_STATION_ID): the `static` goes.
          // (Not in a C header given alone: it is the implementation's one file, and its statics
          // are its own, stb_truetype.h's fallback stbrp_init_target and stbrp_pack_rects beside
          // the game's stb_rect_pack.)
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
            if (!in.c && name.empty() && prefix.empty() && !isHeader() && !n->isInline && cUnwrapAnonymousNamespace(n))
            {
              // its declarations private, each (convertDecls with the prefix "::")
              convertDecls(n->decls, true, "::");
              break;
            }
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
              // (a class ENUM or BITFIELD defines has a definition the resolver does not see: its
              // declaration goes as any other's)
              bool macroDefined = !def && macroClasses && macroClasses->count(global.qualifiedName(c));
              if (namespaceScope && opaqueKept && !macroDefined && (!def || def == c || specializedOnly || definedInLeftOutFile) &&
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
            structAsClass(c);
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
            if (objectOfMacroArguments(f))
              break;
            // (an interface header's function, defined in the library's files: listed in its file,
            // the interface's own record of it once the declaration goes)
            if (isScopeHeader() && namespaceScope && !f->body && f->name.parts.size() == 1 && f->name.last().symbol != UINT32_MAX)
              scopeFunctions.push_back(std::string(symbols.name(f->name.last().symbol)));
            if (f->templateParams.empty() && !f->body && !f->name.empty() && f->name.last().hasTemplateArgs && !(f->flags & DF_Specialization))
            {
              removeDecl(d); // an explicit instantiation: templates compile from source in kpp
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
            directInitialization(v);
            break;
          }
          case NodeKind::UsingAliasDecl:
          {
            recordVariantDeclaration(d, false);
            // `using path = path;` in a namespace that a using-directive opens to the other path
            // (Filesystem.hpp: `using namespace __fs;`): in C++ the alias's own name is not in
            // scope in its type, in kpp it would name the alias itself; the type is qualified
            // with the namespace the directive names (with `::` when there is none).
            auto* a = static_cast<UsingAliasDecl*>(d);
            Type* t = a->type;
            bool ownName = t && t->token + 1 < toks.size() && toks[t->token].getKind() == TokenKind::Identifier &&
                           spelling(t->token) == symbols.name(a->name) && toks[t->token + 1].is(P::Semi) &&
                           !(t->token > 0 && toks[t->token - 1].is(P::ColonColon));
            // Unless the scope declared a class or enum of that name before it (InfinityPipeSettings's
            // `class Filter { ... }; using Filter = Filter;`): the alias then names that member,
            // which MSVC takes as a redeclaration, so it goes (there is no outer one to qualify).
            bool memberOfThatName = false;
            for (Decl* other : decls)
            {
              if (other == d)
                break;
              const QualifiedName* n = other->kind == NodeKind::ClassDecl  ? &static_cast<ClassDecl*>(other)->name
                                       : other->kind == NodeKind::EnumDecl ? &static_cast<EnumDecl*>(other)->name
                                                                           : nullptr;
              if (n && !n->empty() && n->last().symbol == a->name)
                memberOfThatName = true;
            }
            if (ownName && memberOfThatName && t->token >= 3 && spelling(t->token - 3) == "using")
            {
              uint32_t from = toks[t->token - 3].offset, to = endOffset(t->token + 1);
              while (from > 0 && (text[from - 1] == ' ' || text[from - 1] == '\t'))
                --from;
              if (to < text.size() && text[to] == '\r')
                ++to;
              if (to < text.size() && text[to] == '\n')
                ++to;
              remove(from, to - from);
              counts.add("alias of a member of its own name removed");
            }
            else if (ownName)
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

    // A namespace- or class-scope object initialized by parentheses, `Widget w(Size(x));`: at those
    // scopes kpp reads `T f(...)` as a function always (design.md 4, most vexing parse), so the
    // object is written with braces, `Widget w{Size(x)};`, or, where braces would choose another
    // constructor or say something else, with `=`:
    // - a class of the program without an InitializerList constructor (an enumeration too): braces;
    // - a pointer, a reference or a built-in type, one argument: `T* p = x;`, `i32 n = x;`;
    // - anything else (a class with an InitializerList constructor, whose list kpp's braces would
    //   choose, as Vector's, a library class, an alias, a template parameter): `T x = T(args);`.
    void directInitialization(VariableDecl* v)
    {
      // (C has no `T x(args)` object: its parentheses after a name are a function's, `double
      // trio_nzero(void)`)
      if (in.c || !v->init || v->init->kind != NodeKind::FunctionalCastExpr || !v->type)
        return;
      auto* c = static_cast<FunctionalCastExpr*>(v->init);
      if (c->braced || c->type != v->type || c->token >= toks.size() || v->name.empty())
        return;
      // (the resolver's object from a declaration the parser read as a function, `Color color(Color::Green);`
      // with only names in the parentheses: its initializer starts at the declaration; the
      // parenthesis is the one after the name)
      directInitialization(v->type, v->name, v->token, v->endToken, c->token, c->args.size());
    }

    // A declaration the parser read as a function with unnamed parameters, `const ApplicationVersion
    // ApplicationVersion::currentVersion(VERSION_STRING, BUILD_VERSION, PLATFORM);`, whose names are
    // macros the conversion writes out (string or number macros: values, no types): C++ preprocessed
    // it into an object, and so is it written.
    bool objectOfMacroArguments(FunctionDecl* f)
    {
      if (in.c || f->objectArgs.empty() || f->body || !f->returnType || f->name.empty() || objectMacros.empty())
        return false;
      // (one value is enough: a literal is no parameter's type; the other names, BUILD_VERSION, are
      // then values too)
      bool value = false;
      for (Expr* a : f->objectArgs)
        if (a->kind == NodeKind::NameExpr && a->token < toks.size())
          if (auto macro = objectMacros.find(std::string(spelling(a->token)));
              macro != objectMacros.end() && macroHoldsAt(macro->first, toks[a->token].offset) && !isQualifiedName(macro->second) &&
              !isAliasMacro(macro->first, macro->second))
            value = true;
      if (!value)
        return false;
      directInitialization(f->returnType, f->name, f->token, f->endToken, f->token, f->objectArgs.size());
      return true;
    }

    void directInitialization(Type* type, const QualifiedName& declName, uint32_t declToken, uint32_t declEnd, uint32_t open, size_t argCount)
    {
      if (!toks[open].is(P::LParen))
      {
        uint32_t name = declName.last().symbol;
        uint32_t at = open;
        open = UINT32_MAX;
        for (uint32_t k = declToken; k + 1 < declEnd && k + 1 < toks.size(); ++k)
          if (toks[k].getKind() == TokenKind::Identifier && toks[k].id == name && toks[k + 1].is(P::LParen))
          {
            open = k + 1;
            break;
          }
        if (open == UINT32_MAX)
        {
          todo(toks[at].offset, "a namespace- or class-scope object initialized by parentheses whose parenthesis was not found: written as it was");
          return;
        }
      }
      uint32_t close = open;
      for (int depth = 0; close < toks.size(); ++close)
        if (toks[close].is(P::LParen))
          ++depth;
        else if (toks[close].is(P::RParen) && --depth == 0)
          break;
      if (close >= toks.size())
        return;
      Type* t = type;
      while (t->kind == NodeKind::ConstType)
        t = static_cast<ConstType*>(t)->inner;
      auto braces = [&](const char* what)
      {
        replace(toks[open].offset, 1, "{");
        replace(toks[close].offset, 1, "}");
        counts.add(std::string("namespace- and class-scope objects initialized by parentheses written with braces: ") + what);
      };
      if (t->kind == NodeKind::PointerType || t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType ||
          t->kind == NodeKind::BuiltinType || t->kind == NodeKind::MemberPointerType)
      {
        if (argCount == 1)
        {
          replace(toks[open].offset, 1, " = ");
          remove(toks[close].offset, 1);
          counts.add("namespace- and class-scope objects initialized by parentheses written with =: a pointer, reference or built-in type");
        }
        else
          todo(toks[open].offset, "an object of a pointer, reference or built-in type initialized by parentheses with other than one argument");
        return;
      }
      if (t->kind == NodeKind::NamedType)
      {
        Decl* resolved = static_cast<NamedType*>(t)->resolved;
        if (resolved && resolved->kind == NodeKind::EnumDecl)
          return braces("an enumeration");
        if (resolved && resolved->kind == NodeKind::ClassDecl)
        {
          Decl* def = global.definitionOf(resolved);
          auto* cls = static_cast<ClassDecl*>(def && def->kind == NodeKind::ClassDecl ? def : resolved);
          bool listConstructor = hasListConstructor(cls, symbols);
          if (!listConstructor && cls->isDefinition)
            return braces("a class of the program");
        }
      }
      // `T x = T(args)`: the type written again (its C++ text, mapped as the token pass maps it),
      // without the declaration's own const.
      uint32_t nameStart = open;
      if (nameStart > 0 && toks[nameStart - 1].getKind() == TokenKind::Identifier)
        --nameStart;
      while (nameStart >= 2 && toks[nameStart - 1].is(P::ColonColon) && toks[nameStart - 2].getKind() == TokenKind::Identifier)
        nameStart -= 2;
      uint32_t typeStart = t->token;
      std::string typeText = typeStart < nameStart ? std::string(text.substr(toks[typeStart].offset, endOffset(nameStart - 1) - toks[typeStart].offset)) : "";
      while (!typeText.empty() && isspace((unsigned char)typeText.back()))
        typeText.pop_back();
      if (typeText.size() > 6 && typeText.compare(typeText.size() - 6, 6, " const") == 0)
        typeText.resize(typeText.size() - 6);
      if (typeText.empty() || typeText.find_first_of("&*(") != std::string::npos)
      {
        todo(toks[open].offset, "a namespace- or class-scope object initialized by parentheses whose type could not be written again: written as it was");
        return;
      }
      insert(toks[open].offset, " = " + mapStdText(typeText));
      counts.add("namespace- and class-scope objects initialized by parentheses written `= T(args)` (a library class, an alias, a class with an InitializerList constructor)");
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

    // A std::variant's construction `V(...)` as the typed union's from a tag (design.md 8.5,
    // `U(U::Tag, args...)`): `V(std::in_place_index<I>, args...)` names the I-th alternative,
    // `V(std::in_place_type<T>, args...)` T's; and the C++ converting constructions the union's
    // from-a-value does not make, where std::variant selects an alternative by conversion: a
    // string literal (the std::string alternative) and std::nullopt (the std::optional one), when
    // one alternative is of that kind. Any other construction stays as written (from a value of
    // one alternative's type; kppc rejects one it cannot decide).
    // (A cast `V(...)` or, in a function body, a call of the alias's name: the parse takes a name
    // it does not know as a type for a function.)
    void variantConstruction(const VariantInfo& info, uint32_t typeToken, Span<Expr*> args)
    {
      if (args.empty())
        return;
      uint32_t open = typeToken;
      while (open < toks.size() && !toks[open].is(P::LParen))
        ++open;
      if (open >= toks.size())
        return;
      std::vector<std::pair<uint32_t, uint32_t>> ranges = argumentRanges(open + 1);
      if (ranges.size() != args.size())
        return;
      auto [first, end] = ranges[0];
      // `std::in_place_index<I>` / `std::in_place_type<T>` (the `std::` optional).
      uint32_t k = first;
      if (k + 2 < end && spelling(k) == "std" && toks[k + 1].is(P::ColonColon))
        k += 2;
      std::string_view marker = k < end ? spelling(k) : std::string_view();
      if ((marker == "in_place_index" || marker == "in_place_type") && k + 2 < end && toks[k + 1].is(P::Less) && toks[end - 1].is(P::Greater))
      {
        std::string argument = normalizedSpelling(text.substr(toks[k + 2].offset, toks[end - 1].offset - toks[k + 2].offset));
        const VariantAlternative* chosen = nullptr;
        if (marker == "in_place_index")
        {
          size_t index = size_t(atoi(argument.c_str()));
          chosen = index < info.alternatives.size() ? &info.alternatives[index] : nullptr;
        }
        else
        {
          int matches = 0;
          for (const VariantAlternative& a : info.alternatives)
            if (a.spelling == argument)
            {
              chosen = &a;
              ++matches;
            }
          if (matches != 1)
            chosen = nullptr;
        }
        if (chosen)
        {
          replace(toks[first].offset, endOffset(end - 1) - toks[first].offset, info.qualified + "::" + chosen->name);
          counts.add("std::variant constructions in an alternative written U(U::Tag, ...)");
        }
        return;
      }
      if (args.size() != 1)
        return;
      Expr* argument = args[0];
      bool literal = argument->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(argument)->literalKind == TokenKind::String;
      bool nullopt = end - first <= 3 && spelling(end - 1) == "nullopt";
      if (!literal && !nullopt)
        return;
      const VariantAlternative* chosen = nullptr;
      int matches = 0;
      for (const VariantAlternative& a : info.alternatives)
        if ((literal && (a.spelling == "std::string" || a.spelling == "String")) || (nullopt && a.spelling.rfind("std::optional<", 0) == 0))
        {
          chosen = &a;
          ++matches;
        }
      if (matches != 1)
        return;
      // In place of the call's `(`: an insert at the argument would be taken by the argument's own
      // rewrite starting there (`std::nullopt`).
      replace(toks[open].offset, toks[open].length, "(" + info.qualified + "::" + chosen->name + ", ");
      counts.add("std::variant converting constructions written U(U::Tag, value)");
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
    // int in kpp (docs/stdlib/common.md), which a serialiser's operator<< writes in 4 bytes instead
    // of 8: the game's complete-mode saves (its CRCs, desync reports) came out other than the C++
    // build's (TagManager::save, 4 bytes short per force). Written `size_t(...)` where it is the
    // right operand of a <<, the place where C++ code sends a size on by its type (a printf-style
    // argument is widenFormatArguments's, by its format; the game's
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
    // size_t in C++, the library's int in kpp, as a std container's own.
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
    // An expression's type for the std::string rewrites: a cast's own type (`std::string(name) += '_'`),
    // a call's function's return type, else what the declarations say.
    Type* plainTypeOf(Expr* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e)
        return nullptr;
      if (e->kind == NodeKind::CastExpr)
        return static_cast<CastExpr*>(e)->type;
      if (e->kind == NodeKind::NamedCastExpr)
        return static_cast<NamedCastExpr*>(e)->type;
      if (e->kind == NodeKind::FunctionalCastExpr)
        return static_cast<FunctionalCastExpr*>(e)->type;
      if (e->kind == NodeKind::CallExpr)
      {
        Expr* callee = static_cast<CallExpr*>(e)->callee;
        Decl* f = callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(callee)->resolved
                : callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                       : nullptr;
        if (f && f->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(f)->returnType)
          return static_cast<FunctionDecl*>(f)->returnType;
      }
      return typeOfExpr(e, 0).type;
    }
    // Whether a type is C++'s std::string (or wstring, u16string, u32string, basic_string, or an alias of
    // one): kpp's String and WString, which append with `<<` (design.md 14: String has no +=).
    bool isStdStringType(Type* type, int depth = 0)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType || type->kind == NodeKind::RRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner
             : type->kind == NodeKind::LRefType  ? static_cast<LRefType*>(type)->referenced
                                                 : static_cast<RRefType*>(type)->referenced;
      if (!type || depth > 8 || type->kind != NodeKind::NamedType)
        return false;
      auto* named = static_cast<NamedType*>(type);
      const QualifiedName& name = named->name;
      for (const NamePart& part : name.parts)
        if (part.symbol == UINT32_MAX)
          return false;
      if (name.parts.size() == 2 && symbols.name(name.parts[0].symbol) == "std")
      {
        static const std::set<std::string_view> strings = {"string", "wstring", "u16string", "u32string", "basic_string"};
        if (strings.count(symbols.name(name.parts[1].symbol)))
          return true;
      }
      Decl* d = named->resolved;
      if (d && d->kind == NodeKind::UsingAliasDecl)
        return isStdStringType(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return isStdStringType(static_cast<VariableDecl*>(d)->type, depth + 1);
      return false;
    }
    // Whether an expression is a std::string: of such a type, a temporary made by naming the type
    // (`std::string(name) += '_'`, a call of the class or an alias of it), or a `+=` on one, which
    // returns it (`(key += '_') += name`).
    bool isStdStringExpr(Expr* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e)
        return false;
      if (e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->isKeyword &&
          P(static_cast<BinaryExpr*>(e)->op) == P::PlusAssign)
        return isStdStringExpr(static_cast<BinaryExpr*>(e)->lhs);
      // (`std::string("curl_multi_perform failed - ") + curl_multi_strerror(code)`)
      if (e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->isKeyword && P(static_cast<BinaryExpr*>(e)->op) == P::Plus)
        return isStdStringExpr(static_cast<BinaryExpr*>(e)->lhs) || isStdStringExpr(static_cast<BinaryExpr*>(e)->rhs);
      if (e->kind == NodeKind::CallExpr && static_cast<CallExpr*>(e)->callee->kind == NodeKind::NameExpr)
      {
        auto* callee = static_cast<NameExpr*>(static_cast<CallExpr*>(e)->callee);
        if (!callee->resolved || callee->resolved->kind != NodeKind::FunctionDecl)
        {
          NamedType named{};
          named.kind = NodeKind::NamedType;
          named.name = callee->name;
          named.resolved = callee->resolved;
          if (isStdStringType(&named))
            return true;
        }
      }
      return isStdStringType(plainTypeOf(e));
    }
    // Whether a type is an integer other than char, which C++ appends to a std::string as one char and
    // kpp's `<<` as its digits (unsigned char, uint8_t, int...).
    bool isNonCharInteger(Type* type, int depth = 0)
    {
      while (type && type->kind == NodeKind::ConstType)
        type = static_cast<ConstType*>(type)->inner;
      if (!type || depth > 8)
        return false;
      if (type->kind == NodeKind::BuiltinType)
      {
        auto* b = static_cast<BuiltinType*>(type);
        if (b->keyword == Kw::char_)
          return b->isUnsigned || b->isSigned;
        return b->keyword == Kw::int_ || b->keyword == Kw::short_ || b->keyword == Kw::long_ || b->keyword == Kw::unsigned_ ||
               b->keyword == Kw::signed_ || b->keyword == Kw::bool_;
      }
      if (type->kind != NodeKind::NamedType)
        return false;
      auto* named = static_cast<NamedType*>(type);
      if (!named->name.parts.empty() && named->name.last().symbol != UINT32_MAX)
      {
        static const std::set<std::string_view> integers = {"uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t", "uint64_t",
                                                            "int64_t", "size_t", "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64"};
        if (integers.count(symbols.name(named->name.last().symbol)))
          return true;
      }
      Decl* d = named->resolved;
      if (d && d->kind == NodeKind::UsingAliasDecl)
        return isNonCharInteger(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return isNonCharInteger(static_cast<VariableDecl*>(d)->type, depth + 1);
      return false;
    }
    // A std container's iterator type, as the code at owner names it: `std::vector<T>::iterator`, or
    // `WidgetArray::iterator` where WidgetArray is an alias of one in owner or a class or namespace
    // around it (Agui's visible_iterator). Its operator* and operator-> are a pointer's in kpp.
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
    // Whether e is `x.size()`, `x.length()` or `x.count(k)` of a std container: a size_t in C++
    // (64 bits), whatever the library's ContainerSize is.
    bool isStdSizeCall(Expr* e)
    {
      if (!e || e->kind != NodeKind::CallExpr)
        return false;
      auto* call = static_cast<CallExpr*>(e);
      if (call->callee->kind != NodeKind::MemberExpr)
        return false;
      auto* member = static_cast<MemberExpr*>(call->callee);
      if (member->member.parts.size() != 1 || member->member.parts[0].symbol == UINT32_MAX)
        return false;
      std::string_view name = symbols.name(member->member.parts[0].symbol);
      if (!(call->args.size() == 0 && (name == "size" || name == "length")) && !(call->args.size() == 1 && name == "count"))
        return false;
      Expr* base = member->base;
      while (base && base->kind == NodeKind::ParenExpr)
        base = static_cast<ParenExpr*>(base)->inner;
      SeenType type = typeOfExpr(base, 0);
      return (member->op == P::Arrow) == type.iterator && (isStdContainerType(type.type) || returnsStdSizeType(type.type, name));
    }
    // `output << x.size()`, `x.length()`, `x.count(k)` of a std container: written size_t(...).
    void sizeAsSizeT(Expr* e)
    {
      if (!isStdSizeCall(e))
        return;
      auto* call = static_cast<CallExpr*>(e);
      Expr* base = static_cast<MemberExpr*>(call->callee)->base;
      while (base && base->kind == NodeKind::ParenExpr)
        base = static_cast<ParenExpr*>(base)->inner;
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
      // The closing one replaces the call's own `)`: an insert after it would fall where a macro's
      // rewrite replaces the macro's `)` (`LOG_INFO("%zu", v.size())`), which takes it.
      insert(toks[first].offset, "size_t(");
      replace(toks[close].offset, toks[close].length, "))");
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
            else if (d->kind == NodeKind::UsingAliasDecl)
              recordVariantDeclaration(d, false); // a block's `using V = std::variant<...>;`
            else if (d->kind == NodeKind::ClassDecl)
              structAsClass(static_cast<ClassDecl*>(d)); // a local struct (MainLoop's `struct : ProgressObserver { ... }`)
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
            // (A fixture's test body, now parsed as its helper's Run(): the fixture handling writes
            // `this->` there, by text.)
            if (member && !fixtureBodyAt(n->token))
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
          // game's library and a pointer in kpp's, which has no member call for them (Agui's
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
          // (A printf-style call's 64-bit arguments: widenFormatArguments, by the format; its
          // 64-bit arguments read as 32 bits, by the declarations:)
          stdToString(call);
          if (!rewritePrintfCall(call))
            narrowFormatArguments(call);
          if (variants && call->callee->kind == NodeKind::NameExpr)
            if (auto v = variants->byDecl.find(static_cast<NameExpr*>(call->callee)->resolved); v != variants->byDecl.end())
              variantConstruction(variants->all[v->second], call->callee->token, call->args);
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
          // C++'s `text += x` on a std::string: kpp's String appends with `<<` (design.md 14), the right
          // side in parentheses where `<<` would bind otherwise (`text += ok ? "a" : "b"`), and an
          // integer that C++ appends as one char written char(x) (PropertyTreeJson's UTF-8 encoder:
          // `buffer += (unsigned char)(0xc0 | (c >> 6))`).
          if (!in.c && !binary->isKeyword && P(binary->op) == P::PlusAssign && binary->rhs &&
              isStdStringExpr(binary->lhs))
          {
            // (the operator's token: the first `+=` outside brackets after the left side's first token,
            // past an inner one in `(key += '_') += name`)
            uint32_t op = firstTokenOf(binary->lhs);
            for (int depth = 0; op < toks.size() && !(depth == 0 && toks[op].is(P::PlusAssign)); ++op)
              if (toks[op].is(P::LParen) || toks[op].is(P::LBracket) || toks[op].is(P::LBrace))
                ++depth;
              else if (toks[op].is(P::RParen) || toks[op].is(P::RBracket) || toks[op].is(P::RBrace))
                --depth;
            uint32_t rhsFirst = op + 1;
            if (op < toks.size() && toks[op].is(P::PlusAssign))
            {
              // the end of the right side: the `;`, `)` or `,` that ends the expression
              uint32_t end = rhsFirst;
              for (int depth = 0; end < toks.size(); ++end)
              {
                if (toks[end].is(P::LParen) || toks[end].is(P::LBracket) || toks[end].is(P::LBrace))
                  ++depth;
                else if (toks[end].is(P::RParen) || toks[end].is(P::RBracket) || toks[end].is(P::RBrace))
                {
                  if (depth == 0)
                    break;
                  --depth;
                }
                else if (depth == 0 && (toks[end].is(P::Semi) || toks[end].is(P::Comma)))
                  break;
              }
              Expr* rhs = binary->rhs;
              while (rhs->kind == NodeKind::ParenExpr)
                rhs = static_cast<ParenExpr*>(rhs)->inner;
              Type* rhsType = plainTypeOf(rhs);
              bool asChar = isNonCharInteger(rhsType);
              // C text that is no literal or array: a char pointer, or a conditional of literals (`ok ?
              // "yes" : "no"`, whose type the parse may not know), is no piece of a text (design.md 14):
              // StringView(...)
              auto isCharPointer = [&](Type* t)
              {
                while (t && t->kind == NodeKind::ConstType)
                  t = static_cast<ConstType*>(t)->inner;
                if (!t || t->kind != NodeKind::PointerType)
                  return false;
                Type* pointee = static_cast<PointerType*>(t)->pointee;
                while (pointee && pointee->kind == NodeKind::ConstType)
                  pointee = static_cast<ConstType*>(pointee)->inner;
                return pointee && pointee->kind == NodeKind::BuiltinType && static_cast<BuiltinType*>(pointee)->keyword == Kw::char_ &&
                       !static_cast<BuiltinType*>(pointee)->isUnsigned && !static_cast<BuiltinType*>(pointee)->isSigned;
              };
              auto isStringLiteral = [&](Expr* x)
              {
                while (x && x->kind == NodeKind::ParenExpr)
                  x = static_cast<ParenExpr*>(x)->inner;
                return x && x->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(x)->literalKind == TokenKind::String;
              };
              bool cText = !asChar && !isStringLiteral(rhs) &&
                           (isCharPointer(rhsType) ||
                            // (a std::string's c_str(), whose type the parse does not know without the library)
                            (rhs->kind == NodeKind::CallExpr && static_cast<CallExpr*>(rhs)->args.empty() &&
                             static_cast<CallExpr*>(rhs)->callee->kind == NodeKind::MemberExpr &&
                             static_cast<MemberExpr*>(static_cast<CallExpr*>(rhs)->callee)->member.parts.size() == 1 &&
                             static_cast<MemberExpr*>(static_cast<CallExpr*>(rhs)->callee)->member.parts[0].symbol != UINT32_MAX &&
                             (symbols.name(static_cast<MemberExpr*>(static_cast<CallExpr*>(rhs)->callee)->member.parts[0].symbol) == "c_str" ||
                              symbols.name(static_cast<MemberExpr*>(static_cast<CallExpr*>(rhs)->callee)->member.parts[0].symbol) == "what")) ||
                            (rhs->kind == NodeKind::ConditionalExpr && static_cast<ConditionalExpr*>(rhs)->then &&
                             (isStringLiteral(static_cast<ConditionalExpr*>(rhs)->then) || isStringLiteral(static_cast<ConditionalExpr*>(rhs)->else_))));
              bool loose = binary->rhs->kind == NodeKind::ConditionalExpr ||
                           (binary->rhs->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(binary->rhs)->isKeyword &&
                            P(static_cast<BinaryExpr*>(binary->rhs)->op) != P::Plus && P(static_cast<BinaryExpr*>(binary->rhs)->op) != P::Minus &&
                            P(static_cast<BinaryExpr*>(binary->rhs)->op) != P::Star && P(static_cast<BinaryExpr*>(binary->rhs)->op) != P::Slash &&
                            P(static_cast<BinaryExpr*>(binary->rhs)->op) != P::Percent);
              replace(toks[op].offset, toks[op].length, "<<");
              if (asChar || cText || loose)
              {
                insert(toks[rhsFirst].offset, asChar ? "char(" : cText ? "StringView(" : "(");
                insert(toks[end].offset, ")");
              }
              counts.add(asChar  ? "std::string += of an integer written << char(x)"
                         : cText ? "std::string += of C text written << StringView(x)"
                                 : "std::string += written <<");
            }
          }
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
        case NodeKind::NoexceptExpr: walkExpr(static_cast<NoexceptExpr*>(e)->operand, owner); break;
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
        case NodeKind::FunctionalCastExpr:
        {
          auto* cast = static_cast<FunctionalCastExpr*>(e);
          if (const VariantInfo* info = variants ? variantOfType(cast->type) : nullptr)
            variantConstruction(*info, cast->type->token, cast->args);
          walkExprs(cast->args, owner);
          break;
        }
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

    // --- C's rules (ConvertInput::c, kppc --c-sources) ---
    // C converts a void* to any object pointer implicitly (`unsigned char* p = malloc(n);`,
    // `b->buffer = ret;`), kpp only with a cast: the converter writes the cast. The types come from
    // the declarations the converted files and their headers make; a C library function that
    // returns void* is known by name, as the converter does not read the C library's headers.

    // The type a typedef names, followed through typedefs; const and volatile kept.
    Type* cUnaliased(Type* t, int depth = 0) const
    {
      while (t && depth++ < 16)
      {
        if (t->kind != NodeKind::NamedType)
          return t;
        Decl* d = static_cast<NamedType*>(t)->resolved;
        if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
          t = static_cast<VariableDecl*>(d)->type;
        else if (d && d->kind == NodeKind::UsingAliasDecl)
          t = static_cast<UsingAliasDecl*>(d)->type;
        else
          return t;
      }
      return t;
    }
    static Type* cWithoutQualifiers(Type* t)
    {
      while (t && t->kind == NodeKind::ConstType)
        t = static_cast<ConstType*>(t)->inner;
      return t;
    }
    // Whether t is a pointer to void (const or not), or to an object type other than void.
    bool cIsVoidPointer(Type* t) const
    {
      t = cWithoutQualifiers(cUnaliased(t));
      if (!t || t->kind != NodeKind::PointerType)
        return false;
      Type* pointee = cWithoutQualifiers(cUnaliased(static_cast<PointerType*>(t)->pointee));
      return pointee && pointee->kind == NodeKind::BuiltinType && static_cast<BuiltinType*>(pointee)->keyword == Kw::void_;
    }
    bool cIsObjectPointer(Type* t) const
    {
      Type* bare = cWithoutQualifiers(cUnaliased(t));
      if (!bare || bare->kind != NodeKind::PointerType || cIsVoidPointer(t))
        return false;
      Type* pointee = cWithoutQualifiers(cUnaliased(static_cast<PointerType*>(bare)->pointee));
      return pointee && pointee->kind != NodeKind::FunctionType;
    }
    // A type as kpp source: what a cast to it writes. Empty for a type this does not spell (a
    // function pointer).
    std::string cSpelling(Type* t) const
    {
      if (!t)
        return "";
      switch (t->kind)
      {
        case NodeKind::BuiltinType:
        {
          auto* b = static_cast<BuiltinType*>(t);
          std::string s = b->isUnsigned ? "unsigned " : b->isSigned ? "signed " : "";
          for (int i = 0; i < b->longCount; ++i)
            s += "long ";
          if (b->keyword != Kw::int_ || s.empty())
            s += std::string(keywordSpelling(b->keyword));
          else
            s.pop_back();
          return s;
        }
        case NodeKind::NamedType:
        {
          const QualifiedName& name = static_cast<NamedType*>(t)->name;
          std::string s = name.global ? "::" : "";
          for (size_t i = 0; i < name.parts.size(); ++i)
          {
            if (name.parts[i].hasTemplateArgs || name.parts[i].symbol == UINT32_MAX)
              return "";
            std::string part(symbols.name(name.parts[i].symbol));
            s += (i ? "::" : "") + part + (lookupKeyword(part) != UINT32_MAX ? "_" : ""); // (renamed as the C names are)
          }
          // (a scoped library's type, qualified as the code's uses are: minizip's `(const zlib::Bytef*)buf`)
          if (name.parts.size() == 1 && !name.global && scopedLibraryNames)
            if (auto library = scopedLibraryNames->find(s); library != scopedLibraryNames->end())
              s = library->second + "::" + s;
          return s;
        }
        case NodeKind::PointerType:
        {
          std::string pointee = cSpelling(static_cast<PointerType*>(t)->pointee);
          return pointee.empty() ? "" : pointee + "*";
        }
        case NodeKind::ConstType:
        {
          auto* c = static_cast<ConstType*>(t);
          std::string inner = cSpelling(c->inner);
          if (inner.empty())
            return "";
          return std::string(c->isConst ? "const " : "") + (c->isVolatile ? "volatile " : "") + inner;
        }
        default: return "";
      }
    }
    // The C library's functions that return void*.
    static bool cReturnsVoidPointer(std::string_view name)
    {
      static const std::set<std::string_view> names = {"malloc", "calloc", "realloc", "aligned_alloc", "_aligned_malloc",
                                                       "_aligned_realloc", "_recalloc", "memchr", "memcpy", "memmove",
                                                       "memset", "bsearch", "alloca", "_alloca", "_malloca"};
      return names.count(name) != 0;
    }
    // The declared type of what d names: a variable's, a parameter's, a data member's.
    static Type* cDeclType(Decl* d)
    {
      if (!d)
        return nullptr;
      if (d->kind == NodeKind::VariableDecl && !(d->flags & DF_Typedef))
        return static_cast<VariableDecl*>(d)->type;
      if (d->kind == NodeKind::ParamDecl)
        return static_cast<ParamDecl*>(d)->type;
      return nullptr;
    }
    // The member named `member` of the struct or union t (a pointer to it with ->), or null.
    Decl* cMemberOf(Type* t, uint32_t member, bool arrow) const
    {
      t = cWithoutQualifiers(cUnaliased(t));
      if (arrow)
      {
        if (!t || (t->kind != NodeKind::PointerType && t->kind != NodeKind::ArrayType))
          return nullptr;
        t = cWithoutQualifiers(cUnaliased(t->kind == NodeKind::PointerType ? static_cast<PointerType*>(t)->pointee : static_cast<ArrayType*>(t)->element));
      }
      if (!t || t->kind != NodeKind::NamedType)
        return nullptr;
      Decl* d = static_cast<NamedType*>(t)->resolved;
      if (!d || d->kind != NodeKind::ClassDecl)
        return nullptr;
      std::function<Decl*(ClassDecl*)> find = [&](ClassDecl* c) -> Decl*
      {
        for (Decl* m : c->members)
        {
          if (m->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(m)->name.empty() && static_cast<VariableDecl*>(m)->name.last().symbol == member)
            return m;
          if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty()) // an anonymous struct or union's members
            if (Decl* inner = find(static_cast<ClassDecl*>(m)))
              return inner;
        }
        return nullptr;
      };
      return find(static_cast<ClassDecl*>(d));
    }
    // The type of a C expression, as far as the conversions below need it: null when unknown.
    Type* cTypeOf(Expr* e)
    {
      static BuiltinType voidType = []
      {
        BuiltinType v;
        v.kind = NodeKind::BuiltinType;
        v.token = 0;
        v.keyword = Kw::void_;
        return v;
      }();
      static PointerType voidPointer = []
      {
        PointerType p;
        p.kind = NodeKind::PointerType;
        p.token = 0;
        p.pointee = &voidType;
        return p;
      }();
      if (!e)
        return nullptr;
      switch (e->kind)
      {
        case NodeKind::ParenExpr: return cTypeOf(static_cast<ParenExpr*>(e)->inner);
        case NodeKind::NameExpr: return cDeclType(static_cast<NameExpr*>(e)->resolved);
        case NodeKind::MemberExpr:
        {
          auto* m = static_cast<MemberExpr*>(e);
          if (Type* t = cDeclType(m->resolved))
            return t;
          if (m->member.parts.size() != 1)
            return nullptr;
          return cDeclType(cMemberOf(cTypeOf(m->base), m->member.parts[0].symbol, m->op == P::Arrow));
        }
        case NodeKind::CallExpr:
        {
          Expr* callee = static_cast<CallExpr*>(e)->callee;
          while (callee->kind == NodeKind::ParenExpr)
            callee = static_cast<ParenExpr*>(callee)->inner;
          if (callee->kind != NodeKind::NameExpr || !static_cast<NameExpr*>(callee)->resolved ||
              static_cast<NameExpr*>(callee)->resolved->kind != NodeKind::FunctionDecl)
          {
            // through a function pointer (minizip's `bufio->filefunc.zopendisk_file(...)`)
            Type* f = cWithoutQualifiers(cUnaliased(cTypeOf(callee)));
            if (f && f->kind == NodeKind::PointerType)
              f = cWithoutQualifiers(cUnaliased(static_cast<PointerType*>(f)->pointee));
            if (f && f->kind == NodeKind::FunctionType)
              return static_cast<FunctionType*>(f)->result;
          }
          if (callee->kind != NodeKind::NameExpr)
            return nullptr;
          auto* name = static_cast<NameExpr*>(callee);
          if (name->resolved && name->resolved->kind == NodeKind::FunctionDecl)
            return static_cast<FunctionDecl*>(name->resolved)->returnType;
          if (!name->resolved && name->name.parts.size() == 1 && name->name.parts[0].symbol != UINT32_MAX)
          {
            // (a header's macro that names the function: ogg's `#define _ogg_malloc malloc`)
            std::string called(symbols.name(name->name.parts[0].symbol));
            if (headerAliases)
              if (auto alias = headerAliases->find(called); alias != headerAliases->end())
                called = alias->second;
            if (cReturnsVoidPointer(called))
              return &voidPointer;
          }
          return nullptr;
        }
        case NodeKind::CastExpr: return static_cast<CastExpr*>(e)->type;
        case NodeKind::IndexExpr:
        {
          Type* t = cWithoutQualifiers(cUnaliased(cTypeOf(static_cast<IndexExpr*>(e)->base)));
          return !t ? nullptr : t->kind == NodeKind::PointerType ? static_cast<PointerType*>(t)->pointee : t->kind == NodeKind::ArrayType ? static_cast<ArrayType*>(t)->element : nullptr;
        }
        case NodeKind::UnaryExpr:
        {
          auto* u = static_cast<UnaryExpr*>(e);
          if (!u->isKeyword && u->op == uint32_t(P::Star) && !u->postfix)
          {
            Type* t = cWithoutQualifiers(cUnaliased(cTypeOf(u->operand)));
            return t && t->kind == NodeKind::PointerType ? static_cast<PointerType*>(t)->pointee : nullptr;
          }
          return nullptr;
        }
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          if (!b->isKeyword && (b->op == uint32_t(P::Assign) || b->op == uint32_t(P::PlusAssign) || b->op == uint32_t(P::MinusAssign)))
            return cTypeOf(b->lhs);
          if (!b->isKeyword && (b->op == uint32_t(P::Plus) || b->op == uint32_t(P::Minus)))
          {
            // pointer arithmetic keeps the pointer's type (a pointer minus a pointer is no pointer)
            Type* l = cTypeOf(b->lhs);
            Type* bare = cWithoutQualifiers(cUnaliased(l));
            if (bare && bare->kind == NodeKind::PointerType)
            {
              Type* r = cWithoutQualifiers(cUnaliased(cTypeOf(b->rhs)));
              return r && r->kind == NodeKind::PointerType ? nullptr : l;
            }
          }
          return nullptr;
        }
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          return cTypeOf(c->then ? c->then : c->cond);
        }
        default: return nullptr;
      }
    }
    // Where an expression ends, from its first token: at the `;`, `,` or closing bracket that ends
    // the initializer, assignment, argument or return value it is.
    uint32_t cExpressionEnd(uint32_t first) const
    {
      int depth = 0;
      for (uint32_t i = first; i < toks.size(); ++i)
      {
        const Token& t = toks[i];
        if (t.is(P::LParen) || t.is(P::LBracket) || t.is(P::LBrace))
          ++depth;
        else if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::RBrace))
        {
          if (depth == 0)
            return toks[i].offset;
          --depth;
        }
        else if (depth == 0 && (t.is(P::Semi) || t.is(P::Comma)))
          return toks[i].offset;
      }
      return uint32_t(text.size());
    }
    static uint32_t cFirstToken(Expr* e)
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
        else if (e->kind == NodeKind::BinaryExpr)
          e = static_cast<BinaryExpr*>(e)->lhs;
        else if (e->kind == NodeKind::ConditionalExpr)
          e = static_cast<ConditionalExpr*>(e)->cond;
        else
          return e->token;
      }
    }
    // An insertion before the token at offset, which a rewrite of that token (a header's name macro
    // written out: `_ogg_malloc` as `malloc`) would cancel: then it goes into the rewrite's text.
    void cInsertBefore(uint32_t offset, const std::string& s)
    {
      for (Edit& e : edits)
        if (e.offset == offset && e.length > 0)
        {
          e.text = s + e.text;
          return;
        }
      insert(offset, s);
    }
    // C: a top-level declaration of a name its file keeps to itself (cPrivate): `private` before the
    // whole declaration and its `static` gone, also a `static` a keyword macro writes (zstd's
    // FORCE_INLINE_TEMPLATE, `static inline`), which drops the word from that rewrite.
    std::string cDeclName(Decl* d) const
    {
      const QualifiedName* name = d->kind == NodeKind::FunctionDecl ? &static_cast<FunctionDecl*>(d)->name
                                : d->kind == NodeKind::VariableDecl ? &static_cast<VariableDecl*>(d)->name
                                : d->kind == NodeKind::ClassDecl    ? &static_cast<ClassDecl*>(d)->name
                                : d->kind == NodeKind::EnumDecl     ? &static_cast<EnumDecl*>(d)->name
                                                                    : nullptr;
      if (!name || name->empty() || name->last().symbol == UINT32_MAX)
        return {};
      return std::string(symbols.name(name->last().symbol));
    }
    void cMakePrivate(Decl* d)
    {
      std::string name = cDeclName(d);
      bool anonymousEnum = d->kind == NodeKind::EnumDecl && name.empty();
      if (!cPrivate || (name.empty() && !anonymousEnum) || !(anonymousEnum ? cPrivate->count("") : cPrivate->count(name)))
        return;
      writePrivate(d);
    }
    // C++: an anonymous namespace at a source file's global scope (Atlas.cpp's helpers, AppManager.cpp's
    // ConfirmationBoxConnectToServer): its braces go and its declarations are the file's private ones
    // (design.md 15), when it holds no namespace (private is for a file's top-level declarations).
    bool cUnwrapAnonymousNamespace(NamespaceDecl* n)
    {
      for (Decl* inner : n->decls)
        if (inner->kind == NodeKind::NamespaceDecl || inner->kind == NodeKind::UsingDirectiveDecl)
          return false;
      uint32_t open = n->token;
      while (open < toks.size() && !toks[open].is(P::LBrace))
        ++open;
      uint32_t close = n->endToken;
      while (close > open && !toks[close - 1].is(P::RBrace))
        --close;
      if (open >= toks.size() || close <= open + 1 || !toks[n->token].is(Kw::namespace_))
        return false;
      uint32_t end = endOffset(open);
      remove(toks[n->token].offset, end - toks[n->token].offset);
      remove(toks[close - 1].offset, toks[close - 1].length);
      counts.add("C++ anonymous namespaces unwrapped, their declarations private");
      return true;
    }
    // `private` before a declaration (design.md 15) and its `static` gone, also a `static` a keyword
    // macro writes (zstd's FORCE_INLINE_TEMPLATE, `static inline`), which drops the word from that
    // rewrite. Not a function's prototype or a type's forward declaration: the conversion removes
    // those, kpp needs none.
    void writePrivate(Decl* d)
    {
      if ((d->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(d)->body) ||
          (d->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(d)->isDefinition) ||
          (d->kind == NodeKind::EnumDecl && !static_cast<EnumDecl*>(d)->isDefinition))
        return;
      uint32_t start = declStart(d);
      while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
        ++start;
      if (!cPrivateAt.insert(start).second)
        return;
      uint32_t k = d->token;
      while (k > 0 && toks[k - 1].offset >= start)
        --k;
      for (; k < toks.size() && !toks[k].is(P::LParen) && !toks[k].is(P::Assign) && !toks[k].is(P::LBrace) && !toks[k].is(P::Semi); ++k)
        if (toks[k].is(Kw::static_))
        {
          bool rewritten = false;
          for (Edit& e : edits)
            if (e.offset == toks[k].offset && e.length > 0)
            {
              // (a keyword macro's words: `static inline` -> `inline`)
              std::string words;
              for (size_t a = 0; a < e.text.size();)
              {
                size_t b = e.text.find(' ', a);
                if (b == std::string::npos)
                  b = e.text.size();
                if (e.text.compare(a, b - a, "static") != 0)
                  words += (words.empty() ? "" : " ") + e.text.substr(a, b - a);
                a = b + 1;
              }
              e.text = words;
              rewritten = true;
              break;
            }
          if (!rewritten)
          {
            uint32_t end = endOffset(k);
            while (end < text.size() && (text[end] == ' ' || text[end] == '\t'))
              ++end;
            remove(toks[k].offset, end - toks[k].offset);
          }
          break;
        }
      cInsertBefore(start, "private ");
      counts.add(in.c ? "C names kept to their file written private" : "C++ names kept to their file written private");
    }
    // A rewrite of the token at offset, which another rewrite of it (a keyword macro written out:
    // stb_image_write's `STBIWDEF` as `extern`) would cancel: then it takes that one's place.
    void cReplace(uint32_t offset, uint32_t length, const std::string& s)
    {
      for (Edit& e : edits)
        if (e.offset == offset && e.length == length)
        {
          e.text = s;
          return;
        }
      replace(offset, length, s);
    }
    // C: this file's own object-like macros, each with the text where it holds (from its #define to
    // its #undef or next #define: divsufsort.c defines STACK_SIZE twice), from the configured text
    // (the definitions of the branches the configuration keeps).
    struct COwnObject
    {
      std::string name, body;
      uint32_t from, to;
    };
    std::vector<COwnObject> cOwnObjects;
    bool cOwnObjectsRead = false;
    const std::string* cOwnObjectAt(const std::string& name, uint32_t offset)
    {
      if (!cOwnObjectsRead)
      {
        cOwnObjectsRead = true;
        std::string_view t = preprocessed ? std::string_view(preprocessed->configured) : text;
        std::map<std::string, size_t> open;
        for (size_t at = 0; at < t.size();)
        {
          size_t end = t.find('\n', at);
          if (end == std::string_view::npos)
            end = t.size();
          size_t lineStart = at;
          std::string line(t.substr(at, end - at));
          while (!line.empty() && (line.back() == '\\' || (line.back() == '\r' && line.size() > 1 && line[line.size() - 2] == '\\')) && end < t.size())
          {
            line.erase(line.find_last_of('\\'));
            size_t next = t.find('\n', end + 1);
            if (next == std::string_view::npos)
              next = t.size();
            line += " " + std::string(t.substr(end + 1, next - end - 1));
            end = next;
          }
          at = end + 1;
          size_t p = line.find_first_not_of(" \t");
          if (p == std::string::npos || line[p] != '#')
            continue;
          p = line.find_first_not_of(" \t", p + 1);
          if (p == std::string::npos)
            continue;
          bool define = line.compare(p, 6, "define") == 0, undef = line.compare(p, 5, "undef") == 0;
          if (!define && !undef)
            continue;
          p = line.find_first_not_of(" \t", p + (define ? 6 : 5));
          if (p == std::string::npos)
            continue;
          size_t n = p;
          while (n < line.size() && isIdentChar(line[n]))
            ++n;
          std::string name = line.substr(p, n - p);
          if (auto o = open.find(name); o != open.end())
          {
            cOwnObjects[o->second].to = uint32_t(lineStart);
            open.erase(o);
          }
          if (!define || (n < line.size() && line[n] == '('))
            continue;
          std::string body = line.substr(n);
          for (size_t c = body.find("/*"); c != std::string::npos; c = body.find("/*", c))
          {
            size_t e = body.find("*/", c + 2);
            body.replace(c, (e == std::string::npos ? body.size() : e + 2) - c, " ");
          }
          if (size_t c = body.find("//"); c != std::string::npos)
            body.erase(c);
          size_t a = body.find_first_not_of(" \t\r"), b = body.find_last_not_of(" \t\r");
          body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
          if (body.empty())
            continue;
          open[name] = cOwnObjects.size();
          cOwnObjects.push_back({name, body, uint32_t(end), UINT32_MAX});
        }
      }
      const std::string* found = nullptr;
      for (const COwnObject& o : cOwnObjects)
        if (o.name == name && o.from <= offset && offset < o.to)
          found = &o.body;
      return found;
    }
    // C: a written-out macro's body with the object macros it names written out too, as the
    // preprocessor rescans (inftrees.h's `#define ENOUGH (ENOUGH_LENS+ENOUGH_DISTS)`).
    std::string cExpandedObject(const std::string& body, uint32_t useOffset, int depth = 0)
    {
      if (depth > 16)
        return body;
      std::string out;
      for (size_t i = 0; i < body.size();)
      {
        if (body[i] == '"' || body[i] == '\'')
        {
          size_t k = i + 1;
          while (k < body.size() && body[k] != body[i])
            k += body[k] == '\\' ? 2 : 1;
          out += body.substr(i, k + 1 - i);
          i = k + 1;
          continue;
        }
        if (isalpha((unsigned char)body[i]) || body[i] == '_')
        {
          size_t j = i;
          while (j < body.size() && isIdentChar(body[j]))
            ++j;
          std::string name = body.substr(i, j - i);
          auto macro = objectMacros.find(name);
          if (cOwnConstant(name))
            out += name;
          else if (const std::string* own = cOwnObjectAt(name, useOffset); own && *own != body && cWrittenOutMacros && cWrittenOutMacros->count("*"))
            out += cExpandedObject(*own, useOffset, depth + 1);
          else if (macro != objectMacros.end() && macro->second != body && macroHoldsAt(macro->first, useOffset))
            out += cExpandedObject(macro->second, useOffset, depth + 1);
          else if (cStaticRenames && cStaticRenames->count(name))
            out += cStaticRenames->at(name);
          else
            out += name;
          i = j;
          continue;
        }
        out += body[i++];
      }
      return out;
    }
    // C: an object-like macro a .c file defines once, at file scope, whose value is a number the
    // same at every use: a `private const auto` of its file (design.md 15) instead of its text
    // written out at its uses. Its body, with the header macros it names written out (as the
    // preprocessor rescans), is a literal, a negative one or parenthesized (so `x / SIZE` means
    // what it did), with no string (sizeof would differ) and no name but this file's other such
    // constants and type words (a variable it named would be the use's local in C, a global here).
    std::map<std::string, std::string> cOwnConstants; // name -> its value's text ("" once found not to be one)
    std::set<std::string> cScopeEnumerators; // an interface header's: its unscoped enumerations' enumerators
    std::vector<std::pair<uint32_t, uint32_t>> cTopLevelRanges;
    bool cTopLevelRangesRead = false;
    const std::string* cOwnConstant(const std::string& name, int depth = 0)
    {
      bool scope = isScopeHeader();
      if (!in.c || (in.path.extension() != ".c" && !scope) || !cWrittenOutMacros || !cWrittenOutMacros->count("*") || depth > 16)
        return nullptr;
      if (scope && cStandardName(name))
        return nullptr;
      if (knownFunctionMacros.count(name))
        return nullptr; // (trio's FORMAT_STRING, a SAL annotation's name too, whose #define is not read as one)
      if (auto known = cOwnConstants.find(name); known != cOwnConstants.end())
        return known->second.empty() ? nullptr : &known->second;
      cOwnConstants[name] = std::string(); // (also while its body is checked: a cycle is no constant)
      cOwnObjectAt(std::string(), 0);
      const COwnObject* own = nullptr;
      for (const COwnObject& o : cOwnObjects)
        if (o.name == name)
        {
          if (own)
            return nullptr; // defined twice
          own = &o;
        }
      if (!own || own->to != UINT32_MAX)
        return nullptr;
      if (!cTopLevelRangesRead && in.parsed)
      {
        cTopLevelRangesRead = true;
        for (Decl* d : in.parsed->decls)
          if (d->endToken > d->token && d->endToken <= toks.size())
            cTopLevelRanges.push_back({toks[d->token].offset, endOffset(d->endToken - 1)});
      }
      for (const auto& [from, to] : cTopLevelRanges)
        if (from <= own->from && own->from < to)
          return nullptr; // in a function or a struct
      // the body, header macros written out
      std::string value;
      std::string body = own->body;
      int expansions = 0;
      static const std::set<std::string> typeWords = {"sizeof", "unsigned", "signed", "int", "long", "short", "char", "float", "double", "void"};
      // An interface header's (scope): also a choice (zstd.h's `((int)(sizeof(size_t) == 4 ?
      // ZSTD_WINDOWLOG_MAX_32 : ZSTD_WINDOWLOG_MAX_64))`) and another name of one of its enumerators
      // (`#define ZSTD_STRATEGY_MIN ZSTD_fast`), a constant of the enumeration's type.
      bool enumerator = false;
      if (scope && cScopeEnumerators.empty() && in.parsed)
        for (Decl* d : in.parsed->decls)
          if (d->kind == NodeKind::EnumDecl && !static_cast<EnumDecl*>(d)->isClass)
            for (Decl* e : static_cast<EnumDecl*>(d)->enumerators)
              cScopeEnumerators.insert(std::string(symbols.name(static_cast<EnumeratorDecl*>(e)->name)));
      for (size_t i = 0; i < body.size();)
      {
        char c = body[i];
        if (c == '"')
          return nullptr;
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < body.size() && isdigit((unsigned char)body[i + 1])))
        {
          size_t j = i;
          while (j < body.size() && (isIdentChar(body[j]) || body[j] == '.' ||
                                     ((body[j] == '+' || body[j] == '-') && (body[j - 1] == 'e' || body[j - 1] == 'E') && !(body[i] == '0' && j > i + 1 && (body[i + 1] == 'x' || body[i + 1] == 'X')))))
            ++j;
          value += body.substr(i, j - i);
          i = j;
          continue;
        }
        if (isalpha((unsigned char)c) || c == '_')
        {
          size_t j = i;
          while (j < body.size() && isIdentChar(body[j]))
            ++j;
          std::string word = body.substr(i, j - i);
          i = j;
          if (typeWords.count(word) || (scope && word == "size_t"))
            value += word;
          else if (cOwnConstant(word, depth + 1) || (cScopeConstants && cScopeConstants->count(word) && !scope))
          {
            value += word;
            enumerator = enumerator || scope; // (made of the interface's constants: `ZSTD_CHAINLOG_MIN ZSTD_HASHLOG_MIN`)
          }
          else if (scope && cScopeEnumerators.count(word))
          {
            value += word;
            enumerator = true;
          }
          else if (auto macro = objectMacros.find(word); macro != objectMacros.end() && macroHoldsAt(macro->first, own->from) && ++expansions < 64)
          {
            // (a header's macro: its text in its place, checked as this one's; as the
            // preprocessor substitutes it, without parentheses of its own)
            body = body.substr(0, i - word.size()) + macro->second + body.substr(i);
            i -= word.size();
          }
          else
            return nullptr;
          continue;
        }
        if (!strchr(" 	()+-*/%<>|&~!^", c) && !(scope && strchr("?:=", c)))
          return nullptr;
        value += c;
        ++i;
      }
      // a literal, a negative one, or parenthesized as a whole
      size_t a = value.find_first_not_of(" 	"), b = value.find_last_not_of(" 	");
      if (a == std::string::npos)
        return nullptr;
      value = value.substr(a, b - a + 1);
      bool atom = value.find_first_of(" 	()+*/%<>|&~!^") == std::string::npos &&
                  value.find('-', 1) == std::string::npos;
      bool wrapped = value.front() == '(' && value.back() == ')';
      if (wrapped)
        for (size_t k = 0, open = 0; k < value.size(); ++k)
        {
          open += value[k] == '(';
          open -= value[k] == ')';
          if (open == 0 && k + 1 < value.size())
            wrapped = false;
        }
      if ((!atom && !wrapped) || (value.find_first_of("0123456789") == std::string::npos && !enumerator))
        return nullptr;
      cOwnConstants[name] = value;
      return &cOwnConstants[name];
    }
    // C's `U32 const x`, `ZSTD_window_t const window`, `struct S const* s` (zstd writes its consts
    // after the type): `const U32 x`, the const first, as kpp's parse reads it after a type's name
    // only so (language/east_const_after_type_name, pending).
    void cWestConst()
    {
      for (uint32_t i = 0; i + 2 < toks.size(); ++i)
      {
        if (toks[i].getKind() != TokenKind::Identifier || !toks[i + 1].is(Kw::const_) || memberAccessAt(i) ||
            (i > 0 && toks[i - 1].is(P::ColonColon)))
          continue;
        // (`x const` is no expression in C: a name before const is a type's, or an empty macro's,
        // triostr.c's `TRIO_PUBLIC_STRING TRIO_CONST char *`, which is no east const)
        if (cMacros && cMacros->empty.count(std::string(spelling(i))))
          continue;
        uint32_t first = i;
        if (i > 0 && (toks[i - 1].is(Kw::struct_) || toks[i - 1].is(Kw::union_) || toks[i - 1].is(Kw::enum_)))
          first = i - 1;
        cInsertBefore(toks[first].offset, "const ");
        remove(endOffset(i), endOffset(i + 1) - endOffset(i));
        counts.add("C's consts after a type's name written before it");
      }
    }
    // C's array parameters are pointers (`U32 rep[ZSTD_REP_NUM]` is `U32* rep`): written so, in
    // function declarations and in function types (zstd's ZSTD_blockCompressor), where kpp keeps the
    // array (language/array_parameter_is_pointer, pending).
    void cArrayParameter(Decl* param)
    {
      if (param->kind != NodeKind::ParamDecl)
        return;
      auto* p = static_cast<ParamDecl*>(param);
      cArrayParametersIn(p->type);
      if (!p->type || p->type->kind != NodeKind::ArrayType || p->name == UINT32_MAX)
        return;
      for (uint32_t k = p->token; k + 1 < toks.size() && !toks[k].is(P::Comma) && !toks[k].is(P::RParen); ++k)
        if (toks[k].getKind() == TokenKind::Identifier && toks[k].id == p->name && toks[k + 1].is(P::LBracket))
        {
          uint32_t close = k + 1;
          int depth = 0;
          for (; close < toks.size(); ++close)
            if (toks[close].is(P::LBracket))
              ++depth;
            else if (toks[close].is(P::RBracket) && --depth == 0)
              break;
          if (close >= toks.size() || toks[close + 1].is(P::LBracket)) // (an array of arrays: left alone)
            return;
          cInsertBefore(toks[k].offset, "*");
          remove(endOffset(k), endOffset(close) - endOffset(k));
          counts.add("C array parameters written as pointers");
          return;
        }
    }
    void cArrayParametersIn(Type* t)
    {
      for (int depth = 0; t && depth < 16; ++depth)
      {
        if (t->kind == NodeKind::PointerType)
          t = static_cast<PointerType*>(t)->pointee;
        else if (t->kind == NodeKind::ConstType)
          t = static_cast<ConstType*>(t)->inner;
        else if (t->kind == NodeKind::FunctionType)
        {
          for (Decl* param : static_cast<FunctionType*>(t)->params)
            cArrayParameter(param);
          return;
        }
        else
          return;
      }
    }
    void cArrayParameters(Span<Decl*> decls)
    {
      for (Decl* d : decls)
      {
        if (d->kind == NodeKind::FunctionDecl)
          for (Decl* param : static_cast<FunctionDecl*>(d)->params)
            cArrayParameter(param);
        else if (d->kind == NodeKind::VariableDecl)
          cArrayParametersIn(static_cast<VariableDecl*>(d)->type);
        else if (d->kind == NodeKind::ClassDecl)
          cArrayParameters(static_cast<ClassDecl*>(d)->members);
        else if (d->kind == NodeKind::NamespaceDecl)
          cArrayParameters(static_cast<NamespaceDecl*>(d)->decls);
      }
    }
    // C's typedef of a function type, `typedef void stbi_write_func(void *context, void *data, int size);`
    // (stb_image_write's): `using stbi_write_func = void(void *context, void *data, int size);`, the
    // form kpp resolves (language/typedef_of_function_type, pending).
    void cFunctionTypedefs()
    {
      for (uint32_t i = 0; i + 4 < toks.size(); ++i)
      {
        if (!toks[i].is(Kw::typedef_))
          continue;
        uint32_t name = i + 1;
        while (name + 1 < toks.size() && !toks[name].is(P::Semi) && !toks[name].is(P::LBrace) && !toks[name].is(P::LParen) &&
               !(toks[name].getKind() == TokenKind::Identifier && toks[name + 1].is(P::LParen)))
          ++name;
        if (name == i + 1 || name + 1 >= toks.size() || toks[name].getKind() != TokenKind::Identifier || !toks[name + 1].is(P::LParen))
          continue;
        uint32_t close = name + 1;
        for (int depth = 0; close < toks.size(); ++close)
          if (toks[close].is(P::LParen))
            ++depth;
          else if (toks[close].is(P::RParen) && --depth == 0)
            break;
        if (close + 1 >= toks.size() || !toks[close + 1].is(P::Semi))
          continue;
        // (the name as written out: a rename of it moves to the using)
        std::string spelled(text.substr(toks[name].offset, toks[name].length));
        for (const Edit& e : edits)
          if (e.offset == toks[name].offset && e.length == toks[name].length)
            spelled = e.text;
        cReplace(toks[i].offset, toks[i].length, "using " + spelled + " =");
        cReplace(toks[name].offset, toks[name].length, "");
        counts.add("C typedefs of function types written as using");
      }
    }
    // C's `(U64)(size_t)(oend-op)`, a cast of a cast of a parenthesized expression that the parse
    // may not have read as casts (size_t is no declared name for it): kpp reads `(T)(x)` before `(`
    // as a cast of the whole postfix expression, so the inner cast gets parentheses of its own:
    // `(U64)((size_t)(oend-op))`.
    void cCastChains()
    {
      for (uint32_t i = 0; i + 8 < toks.size(); ++i)
      {
        if (!toks[i].is(P::LParen) || toks[i + 1].getKind() != TokenKind::Identifier || !toks[i + 2].is(P::RParen) || !toks[i + 3].is(P::LParen) ||
            toks[i + 4].getKind() != TokenKind::Identifier || !toks[i + 5].is(P::RParen) || !toks[i + 6].is(P::LParen))
          continue;
        // (`f(a)(b)(c)` is calls: the first group must follow no name or `)`)
        if (i > 0 && (toks[i - 1].getKind() == TokenKind::Identifier || toks[i - 1].is(P::RParen) || toks[i - 1].is(P::RBracket)))
          continue;
        int depth = 0;
        for (uint32_t k = i + 6; k < toks.size(); ++k)
          if (toks[k].is(P::LParen))
            ++depth;
          else if (toks[k].is(P::RParen) && --depth == 0)
          {
            if (k + 1 < toks.size() && (toks[k + 1].is(P::Dot) || toks[k + 1].is(P::Arrow) || toks[k + 1].is(P::LBracket) || toks[k + 1].is(P::LParen)))
              break; // (a postfix after it: the chain means something else)
            cInsertBefore(toks[i + 3].offset, "(");
            insert(endOffset(k), ")");
            counts.add("C casts of casts parenthesized");
            break;
          }
      }
    }
    // Whether offset is directly inside a struct's or union's body (a member's declaration).
    bool inClassBody(uint32_t offset) const
    {
      const ScopeRange* innermost = nullptr;
      for (const ScopeRange& r : scopeRanges)
        if (r.begin <= offset && offset < r.end && (!innermost || r.begin >= innermost->begin))
          innermost = &r;
      return innermost && innermost->classBody;
    }
    // value converted to target as C converts it on assignment: a void* to an object pointer
    // gets the cast kpp needs.
    void cConvert(Expr* value, Type* target)
    {
      if (!value || !target || !cIsObjectPointer(target) || !cIsVoidPointer(cTypeOf(value)))
        return;
      cCast(value, cSpelling(target));
    }
    // The typed pointer parameters of the C library's functions that C code passes a void* (the
    // converter does not read the C library's headers): by function, by argument index.
    static const std::map<int, std::string>* cLibraryPointerParameters(std::string_view function)
    {
      static const std::map<std::string_view, std::map<int, std::string>> table = {
        {"_wopen", {{0, "const wchar_t*"}}},   // zlib's gz_open(const void* path, ...)
        {"wcstombs", {{1, "const wchar_t*"}}}, // and its length
        {"wcslen", {{0, "const wchar_t*"}}},   // minizip's iowin32.c (its filename is a void*)
        {"wcsncpy", {{0, "wchar_t*"}, {1, "const wchar_t*"}}},
        {"wcscpy", {{0, "wchar_t*"}, {1, "const wchar_t*"}}},
        {"wcscat", {{0, "wchar_t*"}, {1, "const wchar_t*"}}},
        {"strlen", {{0, "const char*"}}},
        {"strncpy", {{0, "char*"}, {1, "const char*"}}},
        {"strcpy", {{0, "char*"}, {1, "const char*"}}},
        {"strcat", {{0, "char*"}, {1, "const char*"}}},
        {"_tcslen", {{0, "const char*"}}}, // (the TCHAR names, narrow: tcharNames)
        {"_tcsncpy", {{0, "char*"}, {1, "const char*"}}},
        {"_tcscpy", {{0, "char*"}, {1, "const char*"}}},
        {"_tcscat", {{0, "char*"}, {1, "const char*"}}},
      };
      auto it = table.find(function);
      return it == table.end() ? nullptr : &it->second;
    }
    // value, a void*, cast to the object pointer type spelled.
    void cCast(Expr* value, const std::string& spelled)
    {
      if (spelled.empty())
        return;
      Expr* bare = value;
      while (bare->kind == NodeKind::ParenExpr)
        bare = static_cast<ParenExpr*>(bare)->inner;
      uint32_t first = cFirstToken(value);
      bool postfix = bare->kind == NodeKind::NameExpr || bare->kind == NodeKind::MemberExpr || bare->kind == NodeKind::CallExpr ||
                     bare->kind == NodeKind::IndexExpr || value->kind == NodeKind::ParenExpr;
      cInsertBefore(toks[first].offset, "(" + spelled + ")" + (postfix ? "" : "("));
      if (!postfix)
        insert(cExpressionEnd(first), ")");
      counts.add("void* converted to object pointers with a cast (C)");
    }
    void cWalkExprs(Span<Expr*> es)
    {
      for (Expr* e : es)
        cWalkExpr(e);
    }
    void cWalkExpr(Expr* e)
    {
      if (!e)
        return;
      switch (e->kind)
      {
        case NodeKind::ParenExpr: cWalkExpr(static_cast<ParenExpr*>(e)->inner); break;
        case NodeKind::CallExpr:
        {
          auto* call = static_cast<CallExpr*>(e);
          cWalkExpr(call->callee);
          cWalkExprs(call->args);
          Expr* callee = call->callee;
          while (callee->kind == NodeKind::ParenExpr)
            callee = static_cast<ParenExpr*>(callee)->inner;
          if (callee->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(callee)->resolved &&
              static_cast<NameExpr*>(callee)->name.parts.size() == 1 && static_cast<NameExpr*>(callee)->name.parts[0].symbol != UINT32_MAX)
            if (const std::map<int, std::string>* parameters = cLibraryPointerParameters(symbols.name(static_cast<NameExpr*>(callee)->name.parts[0].symbol)))
              for (const auto& [index, spelled] : *parameters)
                if (size_t(index) < call->args.size() && cIsVoidPointer(cTypeOf(call->args[index])))
                  cCast(call->args[index], spelled);
          if (callee->kind == NodeKind::NameExpr && static_cast<NameExpr*>(callee)->resolved &&
              static_cast<NameExpr*>(callee)->resolved->kind == NodeKind::FunctionDecl)
          {
            auto* f = static_cast<FunctionDecl*>(static_cast<NameExpr*>(callee)->resolved);
            for (size_t a = 0; a < call->args.size() && a < f->params.size(); ++a)
              if (f->params[a]->kind == NodeKind::ParamDecl)
                cConvert(call->args[a], static_cast<ParamDecl*>(f->params[a])->type);
          }
          break;
        }
        case NodeKind::IndexExpr:
          cWalkExpr(static_cast<IndexExpr*>(e)->base);
          cWalkExprs(static_cast<IndexExpr*>(e)->args);
          break;
        case NodeKind::MemberExpr: cWalkExpr(static_cast<MemberExpr*>(e)->base); break;
        case NodeKind::UnaryExpr: cWalkExpr(static_cast<UnaryExpr*>(e)->operand); break;
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          cWalkExpr(b->lhs);
          cWalkExpr(b->rhs);
          if (!b->isKeyword && b->op == uint32_t(P::Assign))
            cConvert(b->rhs, cTypeOf(b->lhs));
          break;
        }
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          cWalkExpr(c->cond);
          cWalkExpr(c->then);
          cWalkExpr(c->else_);
          break;
        }
        case NodeKind::CastExpr:
        {
          auto* cast = static_cast<CastExpr*>(e);
          cWalkExpr(cast->operand);
          break;
        }
        case NodeKind::SizeofExpr: cWalkExpr(static_cast<SizeofExpr*>(e)->operand); break;
        case NodeKind::BraceInitExpr: cWalkExprs(static_cast<BraceInitExpr*>(e)->args); break;
        default: break;
      }
    }
    void cWalkVariable(Decl* d)
    {
      if (d->kind != NodeKind::VariableDecl)
        return;
      auto* v = static_cast<VariableDecl*>(d);
      cWalkExpr(v->init);
      if (!v->bracedInit)
        cConvert(v->init, v->type);
    }
    void cWalkStmt(Stmt* s, FunctionDecl* function)
    {
      if (!s)
        return;
      auto walkNode = [&](Node* n)
      {
        if (!n)
          return;
        if (n->kind == NodeKind::DeclStmt)
          cWalkStmt(static_cast<Stmt*>(n), function);
        else if (n->kind == NodeKind::VariableDecl)
          cWalkVariable(static_cast<Decl*>(n));
        else
          cWalkExpr(static_cast<Expr*>(n));
      };
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            cWalkStmt(inner, function);
          break;
        case NodeKind::ExprStmt: cWalkExpr(static_cast<ExprStmt*>(s)->expr); break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
            cWalkVariable(d);
          break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          cWalkStmt(i->init, function);
          walkNode(i->cond);
          cWalkStmt(i->then, function);
          cWalkStmt(i->else_, function);
          break;
        }
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          cWalkStmt(f->init, function);
          walkNode(f->cond);
          cWalkExpr(f->step);
          cWalkStmt(f->body, function);
          break;
        }
        case NodeKind::WhileStmt:
          walkNode(static_cast<WhileStmt*>(s)->cond);
          cWalkStmt(static_cast<WhileStmt*>(s)->body, function);
          break;
        case NodeKind::DoStmt:
          cWalkStmt(static_cast<DoStmt*>(s)->body, function);
          cWalkExpr(static_cast<DoStmt*>(s)->cond);
          break;
        case NodeKind::SwitchStmt:
        {
          auto* sw = static_cast<SwitchStmt*>(s);
          cWalkStmt(sw->init, function);
          walkNode(sw->cond);
          cWalkStmt(sw->body, function);
          break;
        }
        case NodeKind::ReturnStmt:
        {
          Expr* value = static_cast<ReturnStmt*>(s)->value;
          cWalkExpr(value);
          if (function)
            cConvert(value, function->returnType);
          break;
        }
        default: break;
      }
    }
    void convertC(Span<Decl*> decls)
    {
      if (cInterfaceText)
      {
        // The header imports itself: its interface is the C import's, one entity for the program
        // (the game imports fontstash.h; its FONSparams, FONSflags and prototypes were declared twice).
        importC(in.path.filename().string());
        counts.add("C headers given alone that stay C imports: importing themselves");
      }
      for (Decl* d : decls)
      {
        if (cInterfaceText && !(d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body) && d->token < toks.size())
        {
          const Token& name = toks[d->token];
          if (name.offset + name.length <= cInterfaceText->size() &&
              cInterfaceText->compare(name.offset, name.length, text.substr(name.offset, name.length)) == 0)
          {
            // The whole declaration, to its `;`: `typedef struct { ... } stbtt__buf;`'s name and the
            // `;` after an enum's braces too.
            uint32_t start = declStart(d), k = d->token;
            while (k > 0 && toks[k - 1].offset >= start)
              --k;
            int depth = 0;
            for (; k < toks.size(); ++k)
            {
              if (toks[k].is(P::LBrace))
                ++depth;
              else if (toks[k].is(P::RBrace))
                --depth;
              else if (toks[k].is(P::Semi) && depth == 0)
                break;
            }
            uint32_t end = k < toks.size() ? endOffset(k) : endOffset(toks.size() - 1);
            remove(start, lineEndAfter(end) - start);
            counts.add("C declarations the header's import gives removed");
            continue;
          }
        }
        cMakePrivate(d);
        if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body)
        {
          cWalkStmt(static_cast<FunctionDecl*>(d)->body, static_cast<FunctionDecl*>(d));
          // A function C code calls by its name (vorbis calls ogg's): C linkage, as every function
          // a C file defines without static has. (Its own flags may say C already: the resolver
          // gives it the linkage of the header's extern "C" declaration, which kpp imports.)
          bool written = d->token >= 2 && toks[d->token - 1].getKind() == TokenKind::String && toks[d->token - 2].is(Kw::extern_);
          // A function that takes or returns a struct by value, which no interface header of the
          // library declares (cover.c's COVER_computeEpochs): the converted files are its only
          // callers, and it keeps kpp's linkage (kpp's C-callable functions pass no struct values).
          auto byValue = [&](Type* t)
          {
            t = cWithoutQualifiers(cUnaliased(t));
            return t && t->kind == NodeKind::NamedType && static_cast<NamedType*>(t)->resolved &&
                   static_cast<NamedType*>(t)->resolved->kind == NodeKind::ClassDecl;
          };
          auto* f = static_cast<FunctionDecl*>(d);
          bool structValue = byValue(f->returnType);
          for (Decl* param : f->params)
            structValue = structValue || (param->kind == NodeKind::ParamDecl && byValue(static_cast<ParamDecl*>(param)->type));
          if (structValue && !f->name.empty() && f->name.last().symbol != UINT32_MAX &&
              !(cInterfaceNames && cInterfaceNames->count(std::string(symbols.name(f->name.last().symbol)))))
          {
            counts.add("C functions passing structs by value the interface does not declare kept kpp's linkage");
            written = true;
          }
          // A function whose declaration names one of the library's private names (private-names.txt):
          // its C view, which the game imports, has the program's type of that name, another one
          // (stb_truetype's stbtt_PackFontRangesGatherRects takes its fallback's stbrp_rect, the
          // imported fontstash.h stb_rect_pack's), which C allows only across translation units: it
          // keeps kpp's linkage.
          if (cPrivate && cPrivate->count(cDeclName(d)) && !written)
          {
            counts.add("C functions kept to their file kept kpp's linkage");
            written = true;
          }
          if (cPrivateNames && !cPrivateNames->empty() && !written)
          {
            uint32_t k = d->token;
            for (uint32_t start = declStart(d); k > 0 && toks[k - 1].offset >= start;)
              --k;
            for (; k < toks.size() && !toks[k].is(P::LBrace); ++k)
              if (toks[k].getKind() == TokenKind::Identifier && cPrivateNames->count(std::string(spelling(k))))
              {
                counts.add("C functions taking the library's private types kept kpp's linkage");
                written = true;
                break;
              }
          }
          // A C++ overload of an interface function (fontstash.h, which the game compiles as C++, defines
          // a fonsAddFontSdf of three parameters besides the interface's of four): C++'s linkage there,
          // kpp's here, as C has one function of a name.
          if (!f->name.empty() && f->name.last().symbol != UINT32_MAX && cInterfaceNames)
            if (auto declared = cInterfaceNames->find(std::string(symbols.name(f->name.last().symbol)));
                declared != cInterfaceNames->end() && !declared->second.count(f->params.size()) && !written)
            {
              counts.add("C++ overloads of the interface's functions kept kpp's linkage");
              written = true;
            }
          if (!(d->flags & DF_Static) && !written && toks[d->token].is(Kw::extern_))
          {
            cReplace(toks[d->token].offset, toks[d->token].length, "extern \"C\""); // (`extern int f(...) {...}`, minizip's)
            counts.add("functions given C linkage (C)");
          }
          else if (!(d->flags & DF_Static) && !written)
          {
            cInsertBefore(toks[d->token].offset, "extern \"C\" ");
            counts.add("functions given C linkage (C)");
          }
        }
        else if (d->kind == NodeKind::VariableDecl)
        {
          cWalkVariable(d);
          cVariableLinkage(static_cast<VariableDecl*>(d));
        }
        else if (d->kind == NodeKind::NamespaceDecl)
          convertC(static_cast<NamespaceDecl*>(d)->decls);
      }
    }
    // A variable a C file defines without static (stb_image_write's stbi_write_png_compression_level,
    // which the game sets through the C header): C linkage, as functions get. A definition without
    // an initializer (C's tentative one, zero) gets `= {}`, since `extern "C" int x;` declares only.
    // (Declarations, `extern int x;`, keep theirs: they name the definition either way.)
    void cVariableLinkage(VariableDecl* v)
    {
      if (v->flags & (DF_Static | DF_Extern | DF_ExternC | DF_Typedef) || v->name.empty() || v->structuredBindings.size() ||
          (v->token >= 2 && toks[v->token - 1].getKind() == TokenKind::String && toks[v->token - 2].is(Kw::extern_)))
        return;
      uint32_t end = v->token;
      int depth = 0;
      bool several = false;
      for (; end < toks.size(); ++end)
      {
        if (toks[end].is(P::LParen) || toks[end].is(P::LBrace) || toks[end].is(P::LBracket))
          ++depth;
        else if (toks[end].is(P::RParen) || toks[end].is(P::RBrace) || toks[end].is(P::RBracket))
          --depth;
        else if (depth == 0 && toks[end].is(P::Comma))
          several = true;
        else if (depth == 0 && toks[end].is(P::Semi))
          break;
      }
      if (end >= toks.size())
        return;
      if (several)
      {
        // (C code naming one of them fails to link: kpp's names differ)
        counts.add("C variables of several declarators kept kpp's linkage");
        return;
      }
      cInsertBefore(toks[v->token].offset, "extern \"C\" ");
      if (!v->init && !v->bracedInit)
        insert(toks[end].offset, " = {}");
      counts.add("variables given C linkage (C)");
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
      if (in.c)
      {
        convertC(in.parsed->decls);
        cWestConst();
        cArrayParameters(in.parsed->decls);
        cFunctionTypedefs();
        cCastChains();
      }
      removeUnparsedDefinitions();
      // (the printf rewrite's closings after its arguments: after every other edit there)
      edits.insert(edits.end(), backEdits.begin(), backEdits.end());
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
              std::string expansion = rewritePrintfText(expandMacro(*use.macro, args, use.begin, &spellings));
              // (marked: the macro may be defined otherwise in the other configuration, Lua's api_check
              // an assert in one and a check that aborts in the other; the use's offset is its id)
              if (convertMarks)
              {
                std::string id = in.path.filename().string() + ":@" + std::to_string(use.begin);
                expansion = "/*kpp:if " + id + "*/" + expansion + "/*kpp:endif " + id + "*/";
              }
              edits.push_back({use.begin, use.end - use.begin, expansion});
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
          edits.push_back({use.begin, use.end - use.begin, rewritePrintfText(expandArguments(*use.replacement, args, spellings))});
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
            // A block's union names its blocks' classes as written (they have no qualified name),
            // the others qualified as any union's (an alternative named String hides the type).
            if (!v.info->localTypes.count(normalizedSpelling(type)))
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
                                // std::variant's emplace<T>(args...) and index(), a size_t as std's (a
                                // `%zu` reads 8 bytes, a << writes them).
                                indent + "  template<typename T, typename... Args>\n" + indent + "  T& emplace(Args&&... args) { " + loop +
                                "{ (*this)[a] = T(forward<Args>(args)...); return (*this)[a]; } }\n" + indent +
                                "  size_t index() const { return size_t(this->tag()); }\n";
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
      return in.c ? squeezed : kppNumberNames(squeezed);
    }

    // kpp's names of the integer and floating types (design.md 13, "What the converter does"):
    // `uint32_t` is `u32`, `int` `i32`, `unsigned char` `u8`, `size_t` `usize`, `double` `f64`. Each is
    // the same type under kpp's name, so nothing else changes. Done on the converted C++ file's text,
    // so that the names the converter writes itself (a printf piece's `uint32_t(x)`) and the macros'
    // expansions get them too; literals and comments keep their text (a check's `#value`). C's names
    // stay inside `extern "C"` declarations and blocks, which meet C, and `long` stays: C's long is
    // a type of its own (32 bits on Windows) that kpp has no other name for. Plain char is the text
    // character, and stays.
    std::string kppNumberNames(const std::string& text)
    {
      static const std::map<std::string_view, std::string_view> names = {
        {"uint8_t", "u8"},  {"uint16_t", "u16"}, {"uint32_t", "u32"}, {"uint64_t", "u64"}, {"int8_t", "i8"},     {"int16_t", "i16"},
        {"int32_t", "i32"}, {"int64_t", "i64"},  {"size_t", "usize"}, {"ptrdiff_t", "i64"}, {"intptr_t", "i64"}, {"uintptr_t", "u64"}};
      static const std::set<std::string_view> words = {"signed", "unsigned", "short", "long", "int", "char", "float", "double"};
      auto identStart = [](char c) { return isalpha((unsigned char)c) || c == '_'; };
      std::string out;
      out.reserve(text.size());
      // The last two characters of code written (not of comments): `.` or `->` before a name makes it
      // a member's (a comment ending with a period before a declaration does not).
      char last = 0, beforeLast = 0;
      auto code = [&](std::string_view written)
      {
        out.append(written);
        for (char ch : written)
          if (!isspace((unsigned char)ch))
            beforeLast = last, last = ch;
      };
      for (size_t i = 0; i < text.size();)
      {
        char c = text[i];
        if (size_t raw = rawStringEnd(text, i); raw != std::string_view::npos)
        {
          code(std::string_view(text).substr(i, raw - i));
          i = raw;
          continue;
        }
        if (c == '"' || (c == '\'' && !isDigitSeparator(text, i)))
        {
          size_t e = i + 1;
          for (; e < text.size() && text[e] != c && text[e] != '\n'; ++e)
            e += text[e] == '\\';
          e = std::min(e + 1, text.size());
          code(std::string_view(text).substr(i, e - i));
          i = e;
          continue;
        }
        if (text.compare(i, 2, "//") == 0)
        {
          size_t e = text.find('\n', i);
          e = e == std::string::npos ? text.size() : e;
          out.append(text, i, e - i);
          i = e;
          continue;
        }
        if (text.compare(i, 2, "/*") == 0)
        {
          size_t e = text.find("*/", i + 2);
          e = e == std::string::npos ? text.size() : e + 2;
          out.append(text, i, e - i);
          i = e;
          continue;
        }
        if (isdigit((unsigned char)c))
        {
          // (a number's suffix is no name: `1ull`, `0x1fu`)
          size_t e = i;
          while (e < text.size() && (isIdentChar(text[e]) || (text[e] == '\'' && isDigitSeparator(text, e)) || text[e] == '.'))
            ++e;
          code(std::string_view(text).substr(i, e - i));
          i = e;
          continue;
        }
        if (!identStart(c))
        {
          code(std::string_view(&text[i], 1));
          ++i;
          continue;
        }
        size_t e = i;
        while (e < text.size() && isIdentChar(text[e]))
          ++e;
        std::string_view word(text.data() + i, e - i);
        if (word == "extern")
        {
          // `extern "C"`: the declaration, or the block, as it is.
          size_t q = e;
          while (q < text.size() && isspace((unsigned char)text[q]))
            ++q;
          if (text.compare(q, 3, "\"C\"") == 0)
          {
            size_t end = q + 3;
            int depth = 0;
            for (; end < text.size(); ++end)
            {
              if (size_t raw = rawStringEnd(text, end); raw != std::string_view::npos)
                end = raw - 1;
              else if (text[end] == '"' || (text[end] == '\'' && !isDigitSeparator(text, end)))
              {
                char quote = text[end];
                for (++end; end < text.size() && text[end] != quote && text[end] != '\n'; ++end)
                  end += text[end] == '\\';
              }
              else if (text.compare(end, 2, "//") == 0)
                end = std::min(text.find('\n', end), text.size()) - 1;
              else if (text.compare(end, 2, "/*") == 0)
                end = std::min(text.find("*/", end + 2), text.size() - 2) + 1;
              else if (text[end] == '{' || text[end] == '(' || text[end] == '[')
                ++depth;
              else if (text[end] == '}' || text[end] == ')' || text[end] == ']')
              {
                if (--depth == 0 && text[end] == '}')
                  break;
              }
              else if (text[end] == ';' && depth == 0)
                break;
            }
            end = std::min(end + 1, text.size());
            code(std::string_view(text).substr(i, end - i));
            i = end;
            continue;
          }
        }
        if (words.count(word))
        {
          // A run of C's type words: `unsigned long long`, `short int`, `signed char`.
          int isSigned = 0, isUnsigned = 0, shorts = 0, longs = 0, ints = 0, chars = 0, floats = 0, doubles = 0;
          size_t runEnd = i, q = i;
          std::string spelled;
          while (q < text.size() && identStart(text[q]))
          {
            size_t we = q;
            while (we < text.size() && isIdentChar(text[we]))
              ++we;
            std::string_view w(text.data() + q, we - q);
            if (!words.count(w))
              break;
            isSigned += w == "signed", isUnsigned += w == "unsigned", shorts += w == "short", longs += w == "long", ints += w == "int";
            chars += w == "char", floats += w == "float", doubles += w == "double";
            spelled += (spelled.empty() ? "" : " ") + std::string(w);
            runEnd = we;
            q = we;
            while (q < text.size() && (text[q] == ' ' || text[q] == '\t'))
              ++q;
          }
          std::string kpp;
          if (floats == 1 && runEnd == i + 5)
            kpp = "f32";
          else if (doubles == 1 && runEnd == i + 6)
            kpp = "f64";
          else if (floats || doubles || longs == 1)
            kpp = ""; // long, long double: as they are
          else if (chars)
            kpp = isUnsigned ? "u8" : isSigned ? "i8" : "";
          else if (shorts)
            kpp = isUnsigned ? "u16" : "i16";
          else if (longs == 2)
            kpp = isUnsigned ? "u64" : "i64";
          else
            kpp = isUnsigned ? "u32" : "i32";
          if (!kpp.empty())
          {
            counts.add("C type names written as kpp's: " + spelled + " as " + kpp);
            code(kpp);
          }
          else
          {
            if (longs == 1 && !doubles)
              counts.add("C type names kept: " + spelled);
            code(std::string_view(text).substr(i, runEnd - i));
          }
          i = runEnd;
          continue;
        }
        auto name = names.find(word);
        char before = last;
        bool member = before == '.' || (before == '>' && beforeLast == '-');
        if (name != names.end() && !member)
        {
          if (before == ':')
          {
            // `::size_t`: kpp's name is a built-in, which no `::` qualifies; `glm::uint32_t` (a
            // qualified one, another namespace's) stays.
            size_t colons = out.find_last_not_of(" \t\r\n");
            size_t prior = colons >= 2 ? out.find_last_not_of(" \t\r\n", colons - 2) : std::string::npos;
            bool global = colons >= 1 && out[colons - 1] == ':' &&
                          (prior == std::string::npos || !(isIdentChar(out[prior]) || out[prior] == '>'));
            if (!global)
            {
              code(word);
              i = e;
              continue;
            }
            out.erase(colons - 1);
          }
          counts.add("C type names written as kpp's: " + std::string(word) + " as " + std::string(name->second));
          code(name->second);
          i = e;
          continue;
        }
        code(word);
        i = e;
      }
      return out;
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

void cKeywordsAsNames(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols,
                      const std::map<std::string, std::string>* keywordMacros)
{
  // C++'s keywords that C does not have. C's own (and those it has as macros of its headers:
  // bool, true, false, alignas, static_assert, thread_local, wchar_t) stay keywords.
  static const Kw cppOnly[] = {Kw::and_, Kw::and_eq_, Kw::bitand_, Kw::bitor_, Kw::catch_, Kw::char8_t_, Kw::class_, Kw::compl_,
                               Kw::concept_, Kw::consteval_, Kw::constexpr_, Kw::constinit_, Kw::const_cast_, Kw::co_await_,
                               Kw::co_return_, Kw::co_yield_, Kw::decltype_, Kw::delete_, Kw::dynamic_cast_, Kw::explicit_,
                               Kw::export_, Kw::friend_, Kw::mutable_, Kw::namespace_, Kw::new_, Kw::noexcept_, Kw::not_,
                               Kw::not_eq_, Kw::operator_, Kw::or_, Kw::or_eq_, Kw::private_, Kw::protected_, Kw::public_,
                               Kw::reinterpret_cast_, Kw::requires_, Kw::static_cast_, Kw::template_, Kw::this_, Kw::throw_,
                               Kw::try_, Kw::typeid_, Kw::typename_, Kw::using_, Kw::virtual_, Kw::xor_, Kw::xor_eq_,
                               Kw::when_}; // (kpp's: design.md 19)
  for (Token& t : tokens)
    if (t.getKind() == TokenKind::Keyword && std::find(std::begin(cppOnly), std::end(cppOnly), Kw(t.id)) != std::end(cppOnly))
    {
      t.kind = uint32_t(TokenKind::Identifier);
      t.id = symbols.intern(text.substr(t.offset, t.length));
    }
    else if (t.getKind() == TokenKind::Identifier && keywordMacros && !keywordMacros->empty())
      if (auto macro = keywordMacros->find(std::string(text.substr(t.offset, t.length))); macro != keywordMacros->end())
      {
        t.kind = uint32_t(TokenKind::Keyword);
        t.id = lookupKeyword(macro->second);
      }
}

namespace
{
  // emptyDefines: the library's defines that are empty (defines.txt's `TRIO_PUBLIC=`), which hold
  // over its own definitions of them, so that another name for one is empty too (triostr.h's
  // `#define TRIO_PUBLIC_STRING TRIO_PUBLIC`).
  CMacros collectCMacros(const std::vector<const std::string*>& configuredTexts, const std::set<std::string>& emptyDefines)
  {
    std::map<std::string, std::set<std::string>> bodies; // name -> "(param) body" or " body", every definition
    for (const std::string* text : configuredTexts)
    {
      const std::string& t = *text;
      for (size_t at = 0; at < t.size();)
      {
        size_t end = t.find('\n', at);
        if (end == std::string::npos)
          end = t.size();
        std::string line = t.substr(at, end - at);
        // continuation lines
        while (!line.empty() && (line.back() == '\\' || (line.size() > 1 && line.back() == '\r' && line[line.size() - 2] == '\\')) && end < t.size())
        {
          line.erase(line.find_last_of('\\'));
          size_t next = t.find('\n', end + 1);
          if (next == std::string::npos)
            next = t.size();
          line += " " + t.substr(end + 1, next - end - 1);
          end = next;
        }
        at = end + 1;
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos || line[p] != '#')
          continue;
        p = line.find_first_not_of(" \t", p + 1);
        if (p == std::string::npos || line.compare(p, 6, "define") != 0)
          continue;
        p = line.find_first_not_of(" \t", p + 6);
        if (p == std::string::npos)
          continue;
        size_t n = p;
        while (n < line.size() && isIdentChar(line[n]))
          ++n;
        if (n == p)
          continue;
        std::string name = line.substr(p, n - p), params;
        if (n < line.size() && line[n] == '(')
        {
          size_t close = line.find(')', n);
          if (close == std::string::npos)
            continue;
          params = line.substr(n + 1, close - n - 1);
          n = close + 1;
        }
        std::string body = line.substr(n);
        for (size_t c = body.find("/*"); c != std::string::npos; c = body.find("/*", c))
        {
          size_t e = body.find("*/", c + 2);
          body.replace(c, (e == std::string::npos ? body.size() : e + 2) - c, " ");
        }
        if (size_t c = body.find("//"); c != std::string::npos)
          body.erase(c);
        size_t a = body.find_first_not_of(" \t\r"), b = body.find_last_not_of(" \t\r");
        body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
        size_t pa = params.find_first_not_of(" \t"), pb = params.find_last_not_of(" \t");
        params = pa == std::string::npos ? std::string() : params.substr(pa, pb - pa + 1);
        bodies[name].insert(n > p && line[p + name.size()] == '(' ? "(" + params + ") " + body : " " + body);
      }
    }
    for (const std::string& name : emptyDefines)
      bodies[name] = {" "};
    static const std::set<std::string> keywords = {"static", "const", "extern", "inline", "volatile", "register"};
    CMacros out;
    for (const auto& [name, defined] : bodies)
    {
      if (defined.size() != 1)
        continue;
      const std::string& d = *defined.begin();
      if (d[0] == ' ')
      {
        std::string body = d.substr(1);
        if (body.empty())
          out.empty.insert(name);
        else if (keywords.count(body))
          out.keyword[name] = body;
      }
      else
      {
        size_t close = d.find(") ");
        std::string param = d.substr(1, close - 1), body = d.substr(close + 2);
        bool identifier = !param.empty();
        for (char c : param)
          identifier = identifier && isIdentChar(c);
        if (identifier && body == param)
          out.identity.insert(name);
        else if (body.empty())
          out.emptyCall.insert(name);
        else
        {
          // (list, a1, ..., an) -> (a1,...,an)
          std::vector<std::string> params;
          std::string compact;
          for (size_t a = 0; a <= param.size();)
          {
            size_t comma = param.find(',', a);
            if (comma == std::string::npos)
              comma = param.size();
            std::string one = param.substr(a, comma - a);
            size_t x = one.find_first_not_of(" \t"), y = one.find_last_not_of(" \t");
            params.push_back(x == std::string::npos ? std::string() : one.substr(x, y - x + 1));
            a = comma + 1;
          }
          for (char ch : body)
            if (ch != ' ' && ch != '\t')
              compact += ch;
          std::string expected = "(";
          for (size_t k = 1; k < params.size(); ++k)
            expected += (k > 1 ? "," : "") + params[k];
          expected += ")";
          if (params.size() >= 2 && compact == expected)
            out.dropFirst.insert(name);
        }
      }
    }
    // body: one parameter, or an identity macro of one
    for (const auto& [name, defined] : bodies)
    {
      if (defined.size() != 1 || (*defined.begin())[0] != '(' || out.identity.count(name))
        continue;
      const std::string& d = *defined.begin();
      size_t close = d.find(") ");
      std::string params = d.substr(1, close - 1), body = d.substr(close + 2);
      std::string compact;
      for (char ch : body)
        if (ch != ' ' && ch != '\t')
          compact += ch;
      size_t open = compact.find('(');
      if (open != std::string::npos && compact.back() == ')' && out.identity.count(compact.substr(0, open)))
        compact = compact.substr(open + 1, compact.size() - open - 2);
      size_t index = 0;
      for (size_t a = 0; a <= params.size(); ++index)
      {
        size_t comma = params.find(',', a);
        if (comma == std::string::npos)
          comma = params.size();
        std::string one = params.substr(a, comma - a);
        size_t x = one.find_first_not_of(" \t"), y = one.find_last_not_of(" \t");
        if (x != std::string::npos && one.substr(x, y - x + 1) == compact)
        {
          out.keepArgument[name] = index;
          break;
        }
        a = comma + 1;
      }
    }
    // storage words, through other macros and past attributes (one word: a keyword macro; none:
    // an empty one)
    {
      std::function<bool(const std::string&, std::vector<std::string>&, int)> words = [&](const std::string& body, std::vector<std::string>& out,
                                                                                            int depth) -> bool
      {
        static const std::map<std::string, std::string> storageWords = {
          {"static", "static"}, {"extern", "extern"}, {"inline", "inline"}, {"__inline", "inline"}, {"__inline__", "inline"},
          {"__forceinline", "inline"}, {"const", "const"}, {"volatile", "volatile"}, {"register", "register"}};
        for (size_t k = 0; k < body.size();)
        {
          if (body[k] == ' ' || body[k] == '\t')
          {
            ++k;
            continue;
          }
          if (!isIdentChar(body[k]))
            return false;
          size_t e = k;
          while (e < body.size() && isIdentChar(body[e]))
            ++e;
          std::string word = body.substr(k, e - k);
          k = e;
          if (word == "__attribute__" || word == "__declspec")
          {
            // its parenthesized argument goes with it
            while (k < body.size() && body[k] == ' ')
              ++k;
            if (k >= body.size() || body[k] != '(')
              return false;
            int depth2 = 0;
            for (; k < body.size(); ++k)
            {
              if (body[k] == '(')
                ++depth2;
              else if (body[k] == ')' && --depth2 == 0)
              {
                ++k;
                break;
              }
            }
            continue;
          }
          if (auto w = storageWords.find(word); w != storageWords.end())
          {
            if (std::find(out.begin(), out.end(), w->second) == out.end())
              out.push_back(w->second);
            continue;
          }
          auto other = bodies.find(word);
          if (depth > 8 || other == bodies.end() || other->second.size() != 1 || (*other->second.begin())[0] != ' ' ||
              !words(other->second.begin()->substr(1), out, depth + 1))
            return false;
        }
        return true;
      };
      for (const auto& [name, defined] : bodies)
      {
        if (defined.size() != 1 || (*defined.begin())[0] != ' ')
          continue;
        std::vector<std::string> found;
        if (!words(defined.begin()->substr(1), found, 0))
          continue;
        if (found.empty())
          out.empty.insert(name);
        else if (found.size() == 1)
          out.keyword[name] = found[0];
        else
        {
          // static first, as C writes it
          std::stable_sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) { return a == "static" && b != "static"; });
          std::string joined;
          for (const std::string& w : found)
            joined += (joined.empty() ? "" : " ") + w;
          out.storage[name] = joined;
        }
      }
    }
    // `*(...)`
    for (const auto& [name, defined] : bodies)
      if (defined.size() == 1 && (*defined.begin())[0] == ' ')
      {
        std::string body = defined.begin()->substr(1);
        if (body.size() > 3 && body[0] == '*' && body.find_first_not_of(" ", 1) != std::string::npos && body[body.find_first_not_of(" ", 1)] == '(' &&
            body.back() == ')')
          out.postfix[name] = body;
      }
    // a `*` among empty macros and calling conventions
    for (const auto& [name, defined] : bodies)
    {
      if (defined.size() != 1 || (*defined.begin())[0] != ' ')
        continue;
      std::string body = defined.begin()->substr(1), left;
      static const std::set<std::string> conventions = {"__stdcall", "__cdecl", "__fastcall", "__vectorcall", "WINAPI", "APIENTRY", "CALLBACK"};
      for (size_t k = 0; k < body.size();)
      {
        if (isIdentChar(body[k]))
        {
          size_t e = k;
          while (e < body.size() && isIdentChar(body[e]))
            ++e;
          std::string word = body.substr(k, e - k);
          if (!out.empty.count(word) && !conventions.count(word))
            left += word;
          k = e;
        }
        else if (body[k] == ' ' || body[k] == '\t')
          ++k;
        else
          left += body[k++];
      }
      if (left == "*")
        out.pointer.insert(name);
    }
    // an object-like name for one of them (`#define TRIO_VARGS2 TRIO_ARGS2`)
    for (const auto& [name, defined] : bodies)
      if (defined.size() == 1 && (*defined.begin())[0] == ' ' && out.dropFirst.count(defined.begin()->substr(1)))
        out.dropFirst.insert(name);
    return out;
  }
}

// A C library's include directories (its overrides' includes.txt, directories below its root, as
// build.kpp's include() gives them: zstd's common\), where its files' includes are found too
// (`#include "mem.h"` in compress\zstd_compress.c).
static std::vector<std::filesystem::path> cIncludeDirectories(const std::filesystem::path& overridesDir,
                                                              const std::vector<std::filesystem::path>& roots)
{
  std::vector<std::filesystem::path> out;
  if (overridesDir.empty())
    return out;
  std::string list;
  if (FILE* f = fopen((overridesDir / "includes.txt").string().c_str(), "rb"))
  {
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
      list.append(buffer, n);
    fclose(f);
  }
  std::error_code ec;
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
    for (const auto& root : roots)
      if (std::filesystem::is_directory(root, ec))
        out.push_back((root / line.substr(a, b - a + 1)).lexically_normal());
  }
  return out;
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
static void blankForParse(Preprocessed& p, const CMacros* cMacros = nullptr)
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
    if (cMacros && (name == "unsigned" || name == "signed"))
    {
      // C: `unsigned const char` (gzread.c) is `const unsigned char`, the qualifiers first, for the
      // parse and the conversion (the same length: the words reordered).
      std::vector<std::string> words = {name}, qualifiers;
      size_t k = end, last = end;
      for (;;)
      {
        size_t w = k;
        while (w < t.size() && (t[w] == ' ' || t[w] == '\t'))
          ++w;
        size_t we = w;
        while (we < t.size() && isIdentChar(t[we]))
          ++we;
        std::string word = t.substr(w, we - w);
        if (word == "const" || word == "volatile")
          qualifiers.push_back(word);
        else if (word == "char" || word == "short" || word == "int" || word == "long")
          words.push_back(word);
        else
          break;
        k = last = we;
      }
      if (!qualifiers.empty() && words.size() > 1)
      {
        std::string reordered;
        for (const std::string& q : qualifiers)
          reordered += q + " ";
        for (size_t w = 0; w < words.size(); ++w)
          reordered += (w ? " " : "") + words[w];
        reordered.resize(last - i, ' ');
        blank(i, last, reordered);
        t.replace(i, last - i, reordered);
        end = last;
      }
    }
    else if (name == "__try")
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
    else if (cMacros && cMacros->empty.count(name) && !call && !(i > 0 && (t[i - 1] == '.' || t[i - 1] == '>')))
      blank(i, end, ""); // (C: FAR, ZEXPORT)
    else if (auto storage = cMacros ? cMacros->storage.find(name) : decltype(cMacros->storage.end()){};
             cMacros && storage != cMacros->storage.end() && !call)
    {
      // C: `MEM_STATIC` is `static inline`: the parse reads the words that fit in the name's place
      // (`static`), the conversion writes them all
      blank(i, end, storage->second);
      std::string fits;
      for (size_t a = 0; a <= storage->second.size();)
      {
        size_t space = storage->second.find(' ', a);
        if (space == std::string::npos)
          space = storage->second.size();
        if (space > end - i)
          break;
        fits = storage->second.substr(0, space);
        a = space + 1;
      }
      t.replace(i, fits.size(), fits);
    }
    else if (auto postfix = cMacros ? cMacros->postfix.find(name) : decltype(cMacros->postfix.end()){};
             cMacros && postfix != cMacros->postfix.end() && !call)
      blank(i, end, " " + postfix->second); // (C: `16 KB` is 16 for the parse, `16 *(1 <<10)` written)
    else if (cMacros && cMacros->pointer.count(name) && !call)
    {
      blank(i, end, "*"); // (C: glad's APIENTRYP)
      t[i] = '*';
    }
    else if (auto keep = cMacros ? cMacros->keepArgument.find(name) : decltype(cMacros->keepArgument.end()){};
             cMacros && keep != cMacros->keepArgument.end() && call)
    {
      // C: `TRIO_PROTO_SCANF((const char *format, ...), 1)` is its argument 0
      size_t close = closing(open);
      if (close != std::string::npos)
      {
        // the arguments' bounds, at depth 0
        std::vector<std::pair<size_t, size_t>> args;
        size_t from = open + 1;
        int depth = 0;
        for (size_t k = open + 1; k <= close; ++k)
        {
          char ch = t[k];
          if (ch == '"' || ch == '\'')
          {
            for (++k; k < close && t[k] != ch; ++k)
              k += t[k] == '\\';
            continue;
          }
          if (ch == '(')
            ++depth;
          else if (ch == ')' && depth > 0)
            --depth;
          else if ((ch == ',' && depth == 0) || k == close)
          {
            args.push_back({from, k});
            from = k + 1;
          }
        }
        if (keep->second < args.size())
        {
          blank(i, args[keep->second].first, "");
          blank(args[keep->second].second, close + 1, "");
          end = close + 1;
        }
      }
    }
    else if (cMacros && cMacros->dropFirst.count(name) && call)
    {
      // C: `TRIO_ARGS2((format, args), char *format, va_list args)` is `(char *format, va_list
      // args)`: the name and its first argument go, the call's parentheses stay
      size_t close = closing(open);
      size_t first = open + 1;
      while (first < t.size() && (t[first] == ' ' || t[first] == '\t'))
        ++first;
      size_t firstClose = first < t.size() && t[first] == '(' ? closing(first) : std::string::npos;
      size_t comma = firstClose == std::string::npos ? std::string::npos : t.find_first_not_of(" \t\r\n", firstClose + 1);
      if (close != std::string::npos && comma != std::string::npos && t[comma] == ',')
      {
        blank(i, end, "");
        blank(open + 1, comma + 1, "");
      }
    }
    else if (cMacros && cMacros->emptyCall.count(name) && call)
    {
      // C: a call of a macro that is nothing (`Tracev((stderr, "..."));`) goes with its arguments
      size_t close = closing(open);
      if (close != std::string::npos)
      {
        blank(i, close + 1, "");
        end = close + 1;
      }
    }
    else if (cMacros && cMacros->identity.count(name) && call)
    {
      // C: `OF((voidpf opaque))` is its argument
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
  CMacros cMacros; // with cSources
  std::vector<std::filesystem::path> includeDirectories; // with cSources: cIncludeDirectories
  bool cBuild = false;        // with cSources: preprocessed as factorio-in-kpp builds a C library
  MacroTable cDefines;        // then: cLibraryDefines and the library's defines.txt
  std::map<std::string, std::string> cNameDefines; // the defines.txt ones that are a name (STBIW_ZLIB_COMPRESS=zlib_compress_stb_adapter)
  std::set<std::string> cEmptyDefines;             // the defines.txt ones that are empty (trio's TRIO_PUBLIC=)
  std::set<std::string> cPrivateNames;             // private-names.txt: names the library keeps to its files (stb_truetype's stbrp_*)
  std::map<std::string, std::string> cIncluderHeaders; // includer-headers.txt: a header, by file name -> the header its includers include first
  MacroTable cLibraryBase;                         // cDefines without the library's defines.txt: how the program's imports see its headers
  std::set<std::string> cKnownUndefined;           // undefined.txt: the #if names the library's C build leaves undefined

  // A header as the program imports it (C: without the library's own defines, its *_IMPLEMENTATION
  // switches): the configured text, the branches it leaves out blank.
  std::string interfaceConfigured(size_t i)
  {
    return preprocessText(texts[i], [&](const std::string& name) -> const Preprocessed*
    {
      size_t j = resolve(i, name);
      return j == SIZE_MAX ? nullptr : of(j);
    }, &defines, &cLibraryBase).configured;
  }
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
    for (const auto& directory : includeDirectories)
      candidates.push_back(directory / spelled);
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
    // (C: a header its includers give a provider's macros first, includer-headers.txt: configured
    // with them.)
    MacroTable withProvider;
    const MacroTable* base = cBuild ? &cDefines : nullptr;
    if (cBuild)
      if (auto ih = cIncluderHeaders.find(paths[i].filename().string()); ih != cIncluderHeaders.end())
      {
        size_t provider = SIZE_MAX;
        for (size_t k = 0; k < paths.size(); ++k)
          if (paths[k].filename().string() == ih->second)
            provider = k;
        const Preprocessed* p = provider == SIZE_MAX ? nullptr : of(provider);
        if (!p)
          fprintf(stderr, "convert: %s's includer header %s is no input (includer-headers.txt)\n", paths[i].string().c_str(), ih->second.c_str());
        else
        {
          withProvider = cDefines;
          for (const auto& [name, body] : p->defined)
            withProvider[name] = body;
          base = &withProvider;
        }
      }
    auto result = std::make_unique<Preprocessed>(preprocessText(texts[i], [&](const std::string& name) -> const Preprocessed*
    {
      size_t j = resolve(i, name);
      return j == SIZE_MAX ? nullptr : of(j);
    }, &defines, base));
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
// The names stdlib.h, string.h and stdio.h declare (their functions, macros and types, string.h's
// with errno.h's, which it includes), from the UCRT 10.0.26100.0 and MSVC 14.44 headers and the
// headers they include (stdlib.h's min and max left out: C's only). C++'s standard headers bring
// these three in, so converted code uses their names without including them; a library imports one
// where its files use a name of it (writeLibraryImports in convertSources).
const std::map<std::string, std::set<std::string>>& paddingHeaderNames()
{
  static const std::map<std::string, std::set<std::string>> names = []
  {
    static const std::pair<const char*, const char*> lists[] = {
    {"stdlib.h",
     "EXIT_FAILURE EXIT_SUCCESS MB_CUR_MAX RAND_MAX _CALL_REPORTFAULT _CVTBUFSIZE _LDBL12 _LDOUBLE _LONGDOUBLE _MAX_DIR "
     "_MAX_DRIVE _MAX_ENV _MAX_EXT _MAX_FNAME _MAX_I64TOSTR_BASE10_COUNT _MAX_I64TOSTR_BASE16_COUNT _MAX_I64TOSTR_BASE2_COUNT "
     "_MAX_I64TOSTR_BASE8_COUNT _MAX_ITOSTR_BASE10_COUNT _MAX_ITOSTR_BASE16_COUNT _MAX_ITOSTR_BASE2_COUNT "
     "_MAX_ITOSTR_BASE8_COUNT _MAX_LTOSTR_BASE10_COUNT _MAX_LTOSTR_BASE16_COUNT _MAX_LTOSTR_BASE2_COUNT "
     "_MAX_LTOSTR_BASE8_COUNT _MAX_PATH _MAX_U64TOSTR_BASE10_COUNT _MAX_U64TOSTR_BASE16_COUNT _MAX_U64TOSTR_BASE2_COUNT "
     "_MAX_U64TOSTR_BASE8_COUNT _MAX_ULTOSTR_BASE10_COUNT _MAX_ULTOSTR_BASE16_COUNT _MAX_ULTOSTR_BASE2_COUNT "
     "_MAX_ULTOSTR_BASE8_COUNT _OUT_TO_DEFAULT _OUT_TO_MSGBOX _OUT_TO_STDERR _PTR_LD _REPORT_ERRMODE _WRITE_ABORT_MSG _abs64 "
     "_aligned_free _aligned_malloc _aligned_msize _aligned_offset_malloc _aligned_offset_realloc _aligned_offset_recalloc "
     "_aligned_realloc _aligned_recalloc _atodbl _atodbl_l _atof_l _atoflt _atoflt_l _atoi64 _atoi64_l _atoi_l _atol_l "
     "_atoldbl _atoldbl_l _atoll_l _beep _byteswap_uint64 _byteswap_ulong _byteswap_ushort _callnewh _calloc_base _countof "
     "_doserrno _dupenv_s _ecvt _ecvt_s _environ _errno _exit _expand _fcvt _fcvt_s _fmode _free_base _fullpath _gcvt _gcvt_s "
     "_get_doserrno _get_errno _get_fmode _get_invalid_parameter_handler _get_pgmptr _get_purecall_handler "
     "_get_thread_local_invalid_parameter_handler _get_wpgmptr _i64toa _i64toa_s _i64tow _i64tow_s _itoa _itoa_s _itow "
     "_itow_s _lfind _lfind_s _lrotl _lrotr _lsearch _lsearch_s _ltoa _ltoa_s _ltow _ltow_s _makepath _makepath_s "
     "_malloc_base _mblen_l _mbstowcs_l _mbstowcs_s_l _mbstrlen _mbstrlen_l _mbstrnlen _mbstrnlen_l _mbtowc_l _msize "
     "_msize_base _onexit _pgmptr _purecall_handler_m _putenv _putenv_s _realloc_base _recalloc _recalloc_base _rotl _rotl64 "
     "_rotr _rotr64 _searchenv _searchenv_s _set_abort_behavior _set_doserrno _set_errno _set_error_mode _set_fmode "
     "_set_invalid_parameter_handler _set_purecall_handler _set_thread_local_invalid_parameter_handler _seterrormode _sleep "
     "_splitpath _splitpath_s _strtod_l _strtof_l _strtoi64 _strtoi64_l _strtol_l _strtold_l _strtoll_l _strtoui64 "
     "_strtoui64_l _strtoul_l _strtoull_l _swab _sys_errlist _sys_nerr _ui64toa _ui64toa_s _ui64tow _ui64tow_s _ultoa "
     "_ultoa_s _ultow _ultow_s _wcstod_l _wcstof_l _wcstoi64 _wcstoi64_l _wcstol_l _wcstold_l _wcstoll_l _wcstombs_l "
     "_wcstombs_s_l _wcstoui64 _wcstoui64_l _wcstoul_l _wcstoull_l _wctomb_l _wctomb_s_l _wdupenv_s _wenviron _wfullpath "
     "_wgetenv _wgetenv_s _wmakepath _wmakepath_s _wperror _wpgmptr _wputenv _wputenv_s _wsearchenv _wsearchenv_s _wsplitpath "
     "_wsplitpath_s _wsystem _wtof _wtof_l _wtoi _wtoi64 _wtoi64_l _wtoi_l _wtol _wtol_l _wtoll _wtoll_l abort abs "
     "at_quick_exit atexit atof atoi atol atoll bsearch bsearch_s calloc div div_t ecvt environ errno exit fcvt free gcvt "
     "getenv getenv_s itoa labs ldiv ldiv_t lfind llabs lldiv lldiv_t lsearch ltoa malloc mblen mbstowcs mbstowcs_s mbtowc "
     "onexit onexit_t perror putenv qsort qsort_s quick_exit rand rand_s realloc srand strtod strtof strtol strtold strtoll "
     "strtoul strtoull swab sys_errlist sys_nerr system ultoa wcstod wcstof wcstol wcstold wcstoll wcstombs wcstombs_s "
     "wcstoul wcstoull wctomb wctomb_s "},
    {"string.h",
     "E2BIG EACCES EADDRINUSE EADDRNOTAVAIL EAFNOSUPPORT EAGAIN EALREADY EBADF EBADMSG EBUSY ECANCELED ECHILD ECONNABORTED "
     "ECONNREFUSED ECONNRESET EDEADLK EDEADLOCK EDESTADDRREQ EDOM EEXIST EFAULT EFBIG EHOSTUNREACH EIDRM EILSEQ EINPROGRESS "
     "EINTR EINVAL EIO EISCONN EISDIR ELOOP EMFILE EMLINK EMSGSIZE ENAMETOOLONG ENETDOWN ENETRESET ENETUNREACH ENFILE ENOBUFS "
     "ENODATA ENODEV ENOENT ENOEXEC ENOLCK ENOLINK ENOMEM ENOMSG ENOPROTOOPT ENOSPC ENOSR ENOSTR ENOSYS ENOTCONN ENOTDIR "
     "ENOTEMPTY ENOTRECOVERABLE ENOTSOCK ENOTSUP ENOTTY ENXIO EOPNOTSUPP EOTHER EOVERFLOW EOWNERDEAD EPERM EPIPE EPROTO "
     "EPROTONOSUPPORT EPROTOTYPE ERANGE EROFS ESPIPE ESRCH ETIME ETIMEDOUT ETXTBSY EWOULDBLOCK EXDEV STRUNCATE _NLSCMPERROR "
     "_SECURECRT_ERRCODE_VALUES_DEFINED _WCSTOK_DEPRECATED _doserrno _errno _get_doserrno _get_errno _memccpy _memicmp "
     "_memicmp_l _set_doserrno _set_errno _strcmpi _strcoll_l _strdup _strerror _strerror_s _stricmp _stricmp_l _stricoll "
     "_stricoll_l _strlwr _strlwr_l _strlwr_s _strlwr_s_l _strncoll _strncoll_l _strnicmp _strnicmp_l _strnicoll _strnicoll_l "
     "_strnset _strnset_s _strrev _strset _strset_s _strupr _strupr_l _strupr_s _strupr_s_l _strxfrm_l _wcscoll_l _wcsdup "
     "_wcserror _wcserror_s _wcsicmp _wcsicmp_l _wcsicoll _wcsicoll_l _wcslwr _wcslwr_l _wcslwr_s _wcslwr_s_l _wcsncoll "
     "_wcsncoll_l _wcsnicmp _wcsnicmp_l _wcsnicoll _wcsnicoll_l _wcsnset _wcsnset_s _wcsrev _wcsset _wcsset_s _wcstok _wcsupr "
     "_wcsupr_l _wcsupr_s _wcsupr_s_l _wcsxfrm_l errno memccpy memchr memcmp memcpy memcpy_s memicmp memmove memmove_s memset "
     "strcat strcat_s strchr strcmp strcmpi strcoll strcpy strcpy_s strcspn strdup strerror strerror_s stricmp strlen strlwr "
     "strncat strncat_s strncmp strncpy strncpy_s strnicmp strnlen strnlen_s strnset strpbrk strrchr strrev strset strspn "
     "strstr strtok strtok_s strupr strxfrm wcscat wcscat_s wcschr wcscmp wcscoll wcscpy wcscpy_s wcscspn wcsdup wcsicmp "
     "wcsicoll wcslen wcslwr wcsncat wcsncat_s wcsncmp wcsncpy wcsncpy_s wcsnicmp wcsnlen wcsnlen_s wcsnset wcspbrk wcsrchr "
     "wcsrev wcsset wcsspn wcsstr wcstok wcstok_s wcsupr wcswcs wcsxfrm "},
    {"stdio.h",
     "BUFSIZ EOF FILE FILENAME_MAX FOPEN_MAX L_tmpnam L_tmpnam_s SEEK_CUR SEEK_END SEEK_SET SYS_OPEN TMP_MAX TMP_MAX_S WEOF "
     "_FILE_DEFINED _IOB_ENTRIES _IOFBF _IOLBF _IONBF _NFILE _NO_CRT_STDIO_INLINE _NSTREAM_ _SWPRINTFS_DEPRECATED _SYS_OPEN "
     "_TMP_MAX_S _commode _fclose_nolock _fcloseall _fdopen _fflush_nolock _fgetc_nolock _fgetchar _fgetwc_nolock _fgetwchar "
     "_fileno _flushall _fprintf_l _fprintf_p _fprintf_p_l _fprintf_s_l _fputc_nolock _fputchar _fputwc_nolock _fputwchar "
     "_fread_nolock _fread_nolock_s _fscanf_l _fscanf_s_l _fseek_nolock _fseeki64 _fseeki64_nolock _fsopen _ftell_nolock "
     "_ftelli64 _ftelli64_nolock _fwprintf_l _fwprintf_p _fwprintf_p_l _fwprintf_s_l _fwrite_nolock _fwscanf_l _fwscanf_s_l "
     "_get_printf_count_output _get_stream_buffer_pointers _getc_nolock _getchar_nolock _getmaxstdio _getw _getwc_nolock "
     "_getwchar_nolock _getws_s _lock_file _pclose _popen _printf_l _printf_p _printf_p_l _printf_s_l _putc_nolock "
     "_putchar_nolock _putw _putwc_nolock _putwchar_nolock _putws _rmtmp _scanf_l _scanf_s_l _scprintf _scprintf_l "
     "_scprintf_p _scprintf_p_l _scwprintf _scwprintf_l _scwprintf_p _scwprintf_p_l _set_printf_count_output _setmaxstdio "
     "_snprintf _snprintf_c _snprintf_c_l _snprintf_l _snprintf_s _snprintf_s_l _snscanf _snscanf_l _snscanf_s _snscanf_s_l "
     "_snwprintf _snwprintf_l _snwprintf_s _snwprintf_s_l _snwscanf _snwscanf_l _snwscanf_s _snwscanf_s_l _sprintf_l "
     "_sprintf_p _sprintf_p_l _sprintf_s_l _sscanf_l _sscanf_s_l _swprintf _swprintf_c _swprintf_c_l _swprintf_l _swprintf_p "
     "_swprintf_p_l _swprintf_s_l _swscanf_l _swscanf_s_l _tempnam _ungetc_nolock _ungetwc_nolock _unlink _unlock_file "
     "_vfprintf_l _vfprintf_p _vfprintf_p_l _vfprintf_s_l _vfscanf_l _vfscanf_s_l _vfwprintf_l _vfwprintf_p _vfwprintf_p_l "
     "_vfwprintf_s_l _vfwscanf_l _vfwscanf_s_l _vprintf_l _vprintf_p _vprintf_p_l _vprintf_s_l _vscanf_l _vscanf_s_l "
     "_vscprintf _vscprintf_l _vscprintf_p _vscprintf_p_l _vscwprintf _vscwprintf_l _vscwprintf_p _vscwprintf_p_l _vsnprintf "
     "_vsnprintf_c _vsnprintf_c_l _vsnprintf_l _vsnprintf_s _vsnprintf_s_l _vsnwprintf _vsnwprintf_l _vsnwprintf_s "
     "_vsnwprintf_s_l _vsnwscanf_l _vsnwscanf_s_l _vsprintf_l _vsprintf_p _vsprintf_p_l _vsprintf_s_l _vsscanf_l _vsscanf_s_l "
     "_vswprintf _vswprintf_c _vswprintf_c_l _vswprintf_l _vswprintf_p _vswprintf_p_l _vswprintf_s_l _vswscanf_l "
     "_vswscanf_s_l _vwprintf_l _vwprintf_p _vwprintf_p_l _vwprintf_s_l _vwscanf_l _vwscanf_s_l _wfdopen _wfopen _wfopen_s "
     "_wfreopen _wfreopen_s _wfsopen _wperror _wpopen _wprintf_l _wprintf_p _wprintf_p_l _wprintf_s_l _wremove _wscanf_l "
     "_wscanf_s_l _wtempnam _wtmpnam _wtmpnam_s clearerr clearerr_s fclose fcloseall fdopen feof ferror fflush fgetc fgetchar "
     "fgetpos fgets fgetwc fgetws fileno flushall fopen fopen_s fpos_t fprintf fprintf_s fputc fputchar fputs fputwc fputws "
     "fread fread_s freopen freopen_s fscanf fscanf_s fseek fsetpos ftell fwprintf fwprintf_s fwrite fwscanf fwscanf_s getc "
     "getchar gets_s getw getwc getwchar perror printf printf_s putc putchar puts putw putwc putwchar remove rename rewind "
     "rmtmp scanf scanf_s setbuf setvbuf snprintf sprintf sprintf_s sscanf sscanf_s stderr stdin stdout swprintf swprintf_s "
     "swscanf swscanf_s tempnam tmpfile tmpfile_s tmpnam tmpnam_s ungetc ungetwc unlink va_list vfprintf vfprintf_s vfscanf "
     "vfscanf_s vfwprintf vfwprintf_s vfwscanf vfwscanf_s vprintf vprintf_s vscanf vscanf_s vsnprintf vsnprintf_s vsprintf "
     "vsprintf_s vsscanf vsscanf_s vswprintf vswprintf_s vswscanf vswscanf_s vwprintf vwprintf_s vwscanf vwscanf_s wprintf "
     "wprintf_s wscanf wscanf_s "},
    };
    std::map<std::string, std::set<std::string>> out;
    for (const auto& [header, list] : lists)
    {
      std::string_view rest(list);
      while (!rest.empty())
      {
        size_t space = rest.find(' ');
        if (space != 0)
          out[header].insert(std::string(rest.substr(0, space)));
        rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
      }
    }
    return out;
  }();
  return names;
}

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

std::map<std::string, std::pair<std::vector<std::string>, std::string>> readHeaderMacros(const std::vector<std::filesystem::path>& paths,
                                                                                       const SourceConfiguration* configuration,
                                                                                       std::map<std::string, std::string>* aliases,
                                                                                       std::map<std::string, std::string>* objects = nullptr,
                                                                                       std::map<std::string, std::string>* definedObjects = nullptr,
                                                                                       const std::set<std::string>* functionMacroNames = nullptr,
                                                                                       const std::set<std::string>* writtenOut = nullptr)
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
              (writtenOut && (writtenOut->count("*") || writtenOut->count(line.substr(p, nameEnd - p)))) ||
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
      body = a == std::string::npos ? std::string() : collapsedSpaces(body.substr(a, b - a + 1));
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
                                         const std::vector<std::filesystem::path>& roots, bool cSources,
                                         const std::filesystem::path& overridesDir)
  : impl(std::make_unique<Impl>())
{
  if (cSources)
  {
    impl->includeDirectories = cIncludeDirectories(overridesDir, roots);
    impl->cBuild = true;
    impl->cDefines = cLibraryDefines();
    impl->cLibraryBase = impl->cDefines;
    // The library's own: `NAME` or `NAME=VALUE` a line, as build.kpp's define() gives them.
    std::string list;
    if (!overridesDir.empty())
      if (FILE* f = fopen((overridesDir / "defines.txt").string().c_str(), "rb"))
      {
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
          list.append(buffer, n);
        fclose(f);
      }
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
      size_t equals = line.find('=');
      impl->cDefines[line.substr(0, equals)] = equals == std::string::npos ? std::string() : line.substr(equals + 1);
      if (equals != std::string::npos && equals + 1 < line.size() && (isalpha((unsigned char)line[equals + 1]) || line[equals + 1] == '_') &&
          std::all_of(line.begin() + equals + 1, line.end(), [](char c) { return isIdentChar(c); }))
        impl->cNameDefines[line.substr(0, equals)] = line.substr(equals + 1);
      if (equals != std::string::npos && equals + 1 == line.size())
        impl->cEmptyDefines.insert(line.substr(0, equals));
    }
    // Names the library keeps to its own files that another part of the program declares too, one a
    // line: stb_truetype.h's fallback rect packer (stbrp_context, stbrp_init_target, ... without
    // STB_RECT_PACK_VERSION), private to fontstash.cpp's translation unit, beside the game's
    // stb_rect_pack, which kpp's C import makes one program's names.
    if (!overridesDir.empty())
      if (FILE* f = fopen((overridesDir / "private-names.txt").string().c_str(), "rb"))
      {
        std::string names;
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
          names.append(buffer, n);
        fclose(f);
        for (size_t pos = 0; pos < names.size();)
        {
          size_t end = names.find('\n', pos);
          if (end == std::string::npos)
            end = names.size();
          std::string line = names.substr(pos, end - pos);
          pos = end + 1;
          size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
          if (a != std::string::npos && line[a] != '#')
            impl->cPrivateNames.insert(line.substr(a, b - a + 1));
        }
      }
    // Headers whose #if lines test a macro of a header they do not include, which each of their
    // includers includes before them, `HEADER PROVIDER` a line by file name: minizip's crypt.h tests
    // zlib.h's ZLIB_VERNUM (`#if ZLIB_VERNUM < 0x1270`, its own z_crc_t), and crypt.c, unzip.c and
    // zip.c include zlib.h first. The header is configured with what the provider leaves defined.
    if (!overridesDir.empty())
      if (FILE* f = fopen((overridesDir / "includer-headers.txt").string().c_str(), "rb"))
      {
        std::string lines;
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
          lines.append(buffer, n);
        fclose(f);
        for (size_t pos = 0; pos < lines.size();)
        {
          size_t end = lines.find('\n', pos);
          if (end == std::string::npos)
            end = lines.size();
          std::string line = lines.substr(pos, end - pos);
          pos = end + 1;
          size_t a = line.find_first_not_of(" \t\r");
          if (a == std::string::npos || line[a] == '#')
            continue;
          size_t space = line.find_first_of(" \t", a), b = space == std::string::npos ? space : line.find_first_not_of(" \t", space);
          size_t c = b == std::string::npos ? b : line.find_last_not_of(" \t\r");
          if (b != std::string::npos)
            impl->cIncluderHeaders[line.substr(a, space - a)] = line.substr(b, c - b + 1);
        }
      }
  }
  // The #if names nothing defines that the library's C build leaves undefined too, one a line,
  // checked against clang-cl's -dM of each of its source files with the library's flags: any other
  // is an error (convertToKpp), as the converter does not read the C runtime's and Windows' headers
  // (trionan.c's FLT_RADIX, DBL_MAX_EXP and DBL_MANT_DIG were taken as undefined: a negative zero
  // printed 0).
  if (cSources && !overridesDir.empty())
    if (FILE* f = fopen((overridesDir / "undefined.txt").string().c_str(), "rb"))
    {
      std::string names;
      char buffer[4096];
      size_t n;
      while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
        names.append(buffer, n);
      fclose(f);
      for (size_t pos = 0; pos < names.size();)
      {
        size_t end = names.find('\n', pos);
        if (end == std::string::npos)
          end = names.size();
        std::string line = names.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a != std::string::npos && line[a] != '#')
          impl->cKnownUndefined.insert(line.substr(a, b - a + 1));
      }
    }
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
  if (cSources)
  {
    std::vector<const std::string*> configured;
    for (auto& result : impl->results)
      if (result)
        configured.push_back(&result->configured);
    // (A library define that is empty goes at its uses, as one its sources define empty: trio's
    // `TRIO_PUBLIC int trio_printf`, whose own definitions the define leaves out.)
    impl->cMacros = collectCMacros(configured, impl->cEmptyDefines);
    // C's restrict qualifies a pointer for the optimizer only (xxhash.c's `XXH32_state_t* restrict
    // dstState`): nothing, as kpp has no restrict
    for (const char* restrictWord : {"restrict", "__restrict", "__restrict__"})
      impl->cMacros.empty.insert(restrictWord);
  }
  for (auto& result : impl->results)
    if (result)
      blankForParse(*result, cSources ? &impl->cMacros : nullptr);
}

const std::map<std::string, std::string>& SourceConfiguration::cKeywordMacros() const { return impl->cMacros.keyword; }

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

// --convert-library (setConvertLibraries): the scoped libraries the converted program uses.
// The program's classes that braces construct as parentheses do (defined, no InitializerList
// constructor), by qualified name: a macro's expansion text names them (UnitTest++'s TEST, its
// `UnitTest::ListAdder adder##Fixture##Name (List, &test##Fixture##Name##Instance);`).
static void collectBraceableClasses(Span<Decl*> decls, GlobalScope& global, const SymbolTable& symbols, std::set<std::string>& out)
{
  for (Decl* d : decls)
    if (d->kind == NodeKind::NamespaceDecl)
      collectBraceableClasses(static_cast<NamespaceDecl*>(d)->decls, global, symbols, out);
    else if (d->kind == NodeKind::ClassDecl)
    {
      auto* c = static_cast<ClassDecl*>(d);
      if (c->isDefinition && !c->name.empty() && c->templateParams.empty() && !hasListConstructor(c, symbols))
        out.insert(global.qualifiedName(c));
      collectBraceableClasses(c->members, global, symbols, out);
    }
}

// The classes Factorio's ENUM(Name, ...) and BITFIELD(Name, ...) define at namespace scope, by
// qualified name (`ns::Name`). The resolver sees the macro's use, not the class the converter writes
// for it, so a forward declaration of one (Entity.hpp's `class DrawingFlags;`) finds no definition;
// with this set it goes as any class's forward declaration does.
static std::set<std::string> collectMacroClasses(const std::vector<ConvertInput>& inputs)
{
  std::set<std::string> classes;
  for (const ConvertInput& in : inputs)
  {
    if (!in.parsed || in.parsed->isImported || !in.tokens)
      continue;
    const std::vector<Token>& toks = *in.tokens;
    auto spelling = [&](size_t k) { return in.text.substr(toks[k].offset, toks[k].length); };
    std::vector<std::pair<std::string, int>> namespaces; // a namespace's name ("" anonymous) and the brace depth inside it
    int depth = 0;
    for (size_t k = 0; k < toks.size(); ++k)
    {
      if (toks[k].is(P::LBrace))
        ++depth;
      else if (toks[k].is(P::RBrace))
      {
        if (!namespaces.empty() && namespaces.back().second == depth)
          namespaces.pop_back();
        --depth;
      }
      else if (toks[k].getKind() == TokenKind::Keyword && spelling(k) == "namespace")
      {
        // `namespace a::b {`, `namespace {`; an alias (`namespace fs = ...;`) opens nothing
        std::string name;
        size_t j = k + 1;
        while (j < toks.size() && (toks[j].getKind() == TokenKind::Identifier || toks[j].is(P::ColonColon)))
          name += spelling(j++);
        if (j < toks.size() && toks[j].is(P::LBrace))
        {
          namespaces.push_back({name, depth + 1});
          ++depth;
          k = j;
        }
      }
      else if (toks[k].getKind() == TokenKind::Identifier && (spelling(k) == "ENUM" || spelling(k) == "BITFIELD") && k + 2 < toks.size() &&
               toks[k + 1].is(P::LParen) && toks[k + 2].getKind() == TokenKind::Identifier &&
               depth == (namespaces.empty() ? 0 : namespaces.back().second))
      {
        std::string qualified;
        for (const auto& [name, inside] : namespaces)
          if (!name.empty())
            qualified += name + "::";
        classes.insert(qualified + std::string(spelling(k + 2)));
      }
    }
  }
  return classes;
}

static std::vector<std::pair<std::string, std::filesystem::path>> convertLibraries;
void setConvertLibraries(std::vector<std::pair<std::string, std::filesystem::path>> libraries) { convertLibraries = std::move(libraries); }

// The line a converted interface header's file starts with.
static const char* const scopeInterfaceLine =
  "// The library's interface (interface.txt): its users write its names qualified (design.md 16).";
// The line after it that lists the interface's functions, whose declarations the conversion removes
// (they are defined in the library's files), continued on `//  ` lines, the names comma-separated.
static const char* const scopeFunctionsLine = "// Its functions, defined in the library's files:";

// A scoped library's names, as its users write them qualified (design.md 16): the top-level
// declarations of its interface files under dir (scopeInterfaceLine: what C's users include, zstd.h,
// not its internal mem.h's BYTE, which Windows' headers declare too) that are not private
// (functions, classes, enumerations and their enumerators, aliases, variables and constants),
// parsed as kppc parses them.
static std::set<std::string> scopedNamesOf(const std::filesystem::path& dir, std::set<std::string>& headers)
{
  std::set<std::string> names;
  std::error_code ec;
  SymbolTable symbols;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".kpp")
      continue;
    std::string text;
    if (!readFile(entry.path(), text) || text.find(scopeInterfaceLine) == std::string::npos)
      continue;
    // (its C header's name: zlib.kpp is zlib.h's, stb_image.kpp stb_image.hpp's)
    headers.insert(entry.path().stem().string() + ".h");
    headers.insert(entry.path().stem().string() + ".hpp");
    if (size_t list = text.find(scopeFunctionsLine); list != std::string::npos)
    {
      size_t at = list + strlen(scopeFunctionsLine);
      for (;;)
      {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        for (size_t k = 0; k < line.size();)
          if (isIdentChar(line[k]))
          {
            size_t e = k;
            while (e < line.size() && isIdentChar(line[e]))
              ++e;
            names.insert(line.substr(k, e - k));
            k = e;
          }
          else
            ++k;
        if (end == std::string::npos || text.compare(end + 1, 4, "//  ") != 0)
          break;
        at = end + 5;
      }
    }
    LexResult lexed = lex(text, symbols);
    ParsedFile parsed;
    parseFile(lexed.tokens, text, symbols, parsed);
    auto add = [&](const QualifiedName& name)
    {
      if (name.parts.size() == 1 && name.last().symbol != UINT32_MAX)
        names.insert(std::string(symbols.name(name.last().symbol)));
    };
    for (Decl* d : parsed.decls)
    {
      if (d->filePrivate)
        continue;
      if (d->kind == NodeKind::FunctionDecl)
        add(static_cast<FunctionDecl*>(d)->name);
      else if (d->kind == NodeKind::ClassDecl)
        add(static_cast<ClassDecl*>(d)->name);
      else if (d->kind == NodeKind::VariableDecl)
        add(static_cast<VariableDecl*>(d)->name);
      else if (d->kind == NodeKind::UsingAliasDecl)
        names.insert(std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name)));
      else if (d->kind == NodeKind::EnumDecl)
      {
        add(static_cast<EnumDecl*>(d)->name);
        if (!static_cast<EnumDecl*>(d)->isClass)
          for (Decl* e : static_cast<EnumDecl*>(d)->enumerators)
            names.insert(std::string(symbols.name(static_cast<EnumeratorDecl*>(e)->name)));
      }
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

static int convertErrors = 0;
int convertErrorCount()
{
  return convertErrors;
}

std::string convertToKpp(const std::vector<ConvertInput>& inputs, GlobalScope& global, const SymbolTable& symbols,
                         const std::vector<std::filesystem::path>& roots, const std::filesystem::path& outDir,
                         const std::filesystem::path& overridesDir, const SourceConfiguration* configuration,
                         const std::filesystem::path& libraryDir, const std::filesystem::path& baseDir)
{
  struct Piece
  {
    std::string text;
    Counts counts;
    std::vector<std::string> cImports;
    std::vector<std::string> todos;
    std::string source;
    bool header;
    bool scopeInterface = false; // converted from an interface header (overrides/interface.txt)
    std::vector<std::string> scopeFunctions; // and its functions
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
  // A C library's interface (overrides/c-headers.txt, file names: zlib's zlib.h and zconf.h): headers
  // that stay C. They are parsed with the library, not written, and an include of one imports it.
  std::set<std::string> cInterfaceHeaders;
  // `zstd.h kpp_zstd.h`: an include of zstd.h imports kpp_zstd.h, a header of the overrides that
  // configures it as the library's files do before their includes (`#define
  // ZSTD_STATIC_LINKING_ONLY`), written beside the converted files: kpp's import has no defines.
  std::map<std::string, std::string> cConfiguredImports;
  if (!overridesDir.empty())
  {
    std::string headersList;
    if (readFile(overridesDir / "c-headers.txt", headersList))
      for (size_t pos = 0; pos < headersList.size();)
      {
        size_t end = headersList.find('\n', pos);
        if (end == std::string::npos)
          end = headersList.size();
        std::string line = headersList.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a != std::string::npos && line[a] != '#')
        {
          std::string entry = line.substr(a, b - a + 1);
          size_t space = entry.find_first_of(" \t");
          std::string header = entry.substr(0, space);
          cInterfaceHeaders.insert(header);
          if (space != std::string::npos)
            cConfiguredImports[header] = entry.substr(entry.find_first_not_of(" \t", space));
        }
      }
  }
  // A scoped C library's interface (overrides/interface.txt, file names: zstd's zstd.h and
  // zstd_errors.h, design.md 16): headers converted into the library's scope, their number macros
  // its constants, which every file's uses write by name (cScopeConstants, filled as the headers are
  // converted, before the other files).
  std::set<std::string> cScopeHeaders;
  std::map<std::string, std::string> cScopeConstants;
  // The scoped libraries the program uses (--convert-library): their names, by library.
  std::map<std::string, std::string> scopedLibraryNames;
  std::set<std::string> scopedLibraries;
  std::set<std::string> scopedHeaders; // their interface headers' names
  // What an include of a scoped library's header still imports, by the header's name and by the
  // library's: its library's C imports (its c-imports.kpp's). A user took C's headers through the
  // library's header (Lua's override_printf.h included trio.h, and with it limits.h's UCHAR_MAX), and
  // keeps them when the header itself is the library's.
  std::map<std::string, std::vector<std::string>> scopedHeaderImports;
  for (const auto& [library, dir] : convertLibraries)
  {
    scopedLibraries.insert(library);
    std::set<std::string> headers;
    for (const std::string& name : scopedNamesOf(dir, headers))
      scopedLibraryNames.emplace(name, library);
    scopedHeaders.insert(headers.begin(), headers.end());
    std::vector<std::string> imports;
    if (std::string text; readFile(dir / "c-imports.kpp", text))
      for (size_t at = text.find("import \""); at != std::string::npos; at = text.find("import \"", at + 1))
      {
        size_t start = at + 8, end = text.find('"', start);
        if (end != std::string::npos)
          imports.push_back(text.substr(start, end - start));
      }
    scopedHeaderImports[library] = imports;
    for (const std::string& header : headers)
      scopedHeaderImports[header] = imports;
  }
  // (a name the program or its C imports declare is theirs, as a bare name resolves: Windows' BYTE,
  // which zstd's mem.h declares too; the library's is reached qualified only)
  // (but the library's own interface headers given to parse with a C library that uses it: minizip's
  // zlib.h, whose names are zlib's)
  if (!scopedLibraryNames.empty())
    for (const ConvertInput& in : inputs)
      if (in.parsed && !scopedHeaders.count(in.path.filename().string()))
        for (Decl* d : in.parsed->decls)
        {
          if (redeclaresScoped(d, symbols, scopedLibraryNames))
            continue; // (the library's, removed where the file is converted)
          const QualifiedName* name = d->kind == NodeKind::FunctionDecl ? &static_cast<FunctionDecl*>(d)->name
                                      : d->kind == NodeKind::ClassDecl  ? &static_cast<ClassDecl*>(d)->name
                                      : d->kind == NodeKind::VariableDecl ? &static_cast<VariableDecl*>(d)->name
                                      : d->kind == NodeKind::EnumDecl     ? &static_cast<EnumDecl*>(d)->name
                                                                          : nullptr;
          if (name && name->parts.size() == 1 && name->last().symbol != UINT32_MAX)
            scopedLibraryNames.erase(std::string(symbols.name(name->last().symbol)));
          else if (d->kind == NodeKind::UsingAliasDecl)
            scopedLibraryNames.erase(std::string(symbols.name(static_cast<UsingAliasDecl*>(d)->name)));
          if (d->kind == NodeKind::EnumDecl)
            for (Decl* e : static_cast<EnumDecl*>(d)->enumerators)
              scopedLibraryNames.erase(std::string(symbols.name(static_cast<EnumeratorDecl*>(e)->name)));
        }
  if (!overridesDir.empty())
  {
    std::string list;
    if (readFile(overridesDir / "interface.txt", list))
      for (size_t pos = 0; pos < list.size();)
      {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos)
          end = list.size();
        std::string line = list.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a != std::string::npos && line[a] != '#')
          cScopeHeaders.insert(line.substr(a, b - a + 1));
      }
  }
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
  const std::vector<std::filesystem::path> cIncludes = cIncludeDirectories(overridesDir, roots); // (C libraries)
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
    // `#include`, or `#  include` (zlib's trees.c, indented under its #if)
    for (size_t at = t.find("include"); at != std::string_view::npos; at = t.find("include", at + 7))
    {
      size_t hash = at;
      while (hash > 0 && (t[hash - 1] == ' ' || t[hash - 1] == '\t'))
        --hash;
      if (hash == 0 || t[hash - 1] != '#')
        continue;
      size_t lineStart = hash - 1;
      while (lineStart > 0 && (t[lineStart - 1] == ' ' || t[lineStart - 1] == '\t'))
        --lineStart;
      if (lineStart > 0 && t[lineStart - 1] != '\n')
        continue;
      size_t open = t.find_first_of("<\"\n", at + 7);
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
      if (inputs[k].c)
        for (const auto& directory : cIncludes)
          candidates.push_back(directory / name);
      bool converted = false;
      for (const auto& c : candidates)
        if (auto it = inputByPath.find(pathKey(c)); it != inputByPath.end() && it->second != k)
        {
          includedInputs[k].push_back(it->second);
          converted = true;
          if (&c == &candidates[0] && !name.has_parent_path())
            siblingIncludes[k].insert(name.string());
          // (C: any converted file of the library, by whatever name: zstd's "zstd_internal.h", which
          // the game's rule would take for zstd's interface by its prefix)
          else if (inputs[k].c && !cInterfaceHeaders.count(name.filename().string()))
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
  // compiles them, so they are left out. Dead code that kpp, compiling every file, would compile:
  // ShaderProgram.hpp (the OpenGL renderer's) names the namespace VertexLayout as a type,
  // ItemStackChangeWatcher.hpp assigns an ItemWithQualityID to an ItemID. The includes are read
  // from the text, those in dropped #if branches too, so no header that may be included goes.
  // C: an #if name that nothing defines is undefined only where the library's undefined.txt says
  // the C build leaves it so; any other is an error, an #if the conversion may answer otherwise than C.
  convertErrors = 0;
  if (configuration && configuration->impl->cBuild)
  {
    std::map<std::string, std::string> unknown; // name -> where first
    for (size_t k = 0; k < inputs.size(); ++k)
      if (inputs[k].c && configuration->impl->results[k])
      {
        const Preprocessed& p = *configuration->impl->results[k];
        for (size_t n = 0; n < p.noteNames.size() && n < p.notes.size(); ++n)
          for (const std::string& name : p.noteNames[n])
            if (!configuration->impl->cKnownUndefined.count(name) && !unknown.count(name))
            {
              size_t at = std::min<size_t>(p.notes[n].first, inputs[k].text.size());
              unknown[name] = inputs[k].path.string() + ":" + std::to_string(1 + std::count(inputs[k].text.begin(), inputs[k].text.begin() + at, '\n'));
            }
      }
    for (const auto& [name, where] : unknown)
    {
      fprintf(stderr, "%s: error: an #if tests %s, which nothing the conversion reads defines and the library's undefined.txt does not list\n",
              where.c_str(), name.c_str());
      ++convertErrors;
    }
  }
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
  // C: every macro constant of a C library is written out at its uses, in the files that see it,
  // as the preprocessor does. A C macro holds in one translation unit, a kpp constant is a name of
  // the whole program: gzguts.h's `#define COPY 1` met inflate.h's enumerator COPY, inftrees.h's
  // MAXBITS Lua's, crc32.c's W the game's.
  std::set<std::string> cWrittenOutMacros;
  for (const ConvertInput& in : inputs)
    if (in.c)
    {
      cWrittenOutMacros.insert("*");
      break;
    }
  // C: the library's private names (private-names.txt), renamed with their file's name in each of
  // its files that uses one (inflate_OS): names a header of the library declares, which `private`
  // (a file's own) cannot keep to the files that include it. (A .c file's statics and types are
  // `private`, cPrivate below.)
  std::map<const ConvertInput*, std::map<std::string, std::string>> cStaticRenames;
  {
    if (configuration)
      for (const ConvertInput& in : inputs)
      {
        if (!in.c || !in.parsed || !in.tokens)
          continue;
        std::string stem = in.path.stem().string();
        for (char& ch : stem)
          if (!isIdentChar(ch))
            ch = '_';
        for (const Token& t : *in.tokens)
          if (t.getKind() == TokenKind::Identifier)
            if (std::string name(in.text.substr(t.offset, t.length)); configuration->impl->cPrivateNames.count(name))
              cStaticRenames[&in][name] = stem + "_" + name;
      }
  }
  std::map<std::string, std::set<size_t>> cInterfaceNames; // the functions the C interface headers declare: their parameter counts
  // (and a header-only library's: the declarations of a C header given alone, which holds its
  // implementation, stb_image.hpp's and fontstash.h's, which the game imports as C)
  std::set<size_t> givenCHeaders;
  for (const auto& root : roots)
    if (auto given = inputByPath.find(pathKey(root)); given != inputByPath.end() && inputs[given->second].c &&
                                                     inputs[given->second].path.extension() != ".c")
      givenCHeaders.insert(given->second);
  // C: what each .c file keeps to itself, written `private` (design.md 15): its statics (C's
  // file-level static: cover.c's and fastcover.c's g_displayLevel), the types it defines that no
  // header of the library declares (trio's math.c ieee_double_shape_type, beside Lua's
  // lmathlib.c's), its anonymous enums ("" in the set), and the functions whose declarations name
  // such a type. A C header given alone (a header-only library's implementation, stb_truetype.h):
  // its statics (its fallback packer's stbrp_init_target); its types may be its C interface's.
  std::map<const ConvertInput*, std::set<std::string>> cPrivate;
  {
    auto declName = [&](Decl* d) -> std::string
    {
      const QualifiedName* name = d->kind == NodeKind::FunctionDecl ? &static_cast<FunctionDecl*>(d)->name
                                : d->kind == NodeKind::VariableDecl ? &static_cast<VariableDecl*>(d)->name
                                : d->kind == NodeKind::ClassDecl    ? &static_cast<ClassDecl*>(d)->name
                                : d->kind == NodeKind::EnumDecl     ? &static_cast<EnumDecl*>(d)->name
                                                                    : nullptr;
      if (!name || name->empty() || name->last().symbol == UINT32_MAX)
        return {};
      return std::string(symbols.name(name->last().symbol));
    };
    // every name the library's headers spell: what they declare, and a struct a .c file defines that
    // they name (zstd.h's `typedef struct ZSTD_CDict_s ZSTD_CDict;`, deflate.h's
    // `struct static_tree_desc_s`), which is theirs then
    std::set<std::string> headerNames;
    for (size_t i = 0; i < inputs.size(); ++i)
      if (inputs[i].c && inputs[i].tokens && inputs[i].path.extension() != ".c" && !givenCHeaders.count(i))
        for (const Token& t : *inputs[i].tokens)
          if (t.getKind() == TokenKind::Identifier)
            headerNames.insert(std::string(inputs[i].text.substr(t.offset, t.length)));
    std::function<bool(Type*, const std::set<std::string>&, int)> names = [&](Type* t, const std::set<std::string>& types, int depth)
    {
      while (t && depth < 16)
      {
        ++depth;
        if (t->kind == NodeKind::PointerType)
          t = static_cast<PointerType*>(t)->pointee;
        else if (t->kind == NodeKind::ConstType)
          t = static_cast<ConstType*>(t)->inner;
        else if (t->kind == NodeKind::LRefType)
          t = static_cast<LRefType*>(t)->referenced;
        else if (t->kind == NodeKind::ArrayType)
          t = static_cast<ArrayType*>(t)->element;
        else
          break;
      }
      return t && t->kind == NodeKind::NamedType && !static_cast<NamedType*>(t)->name.empty() &&
             static_cast<NamedType*>(t)->name.last().symbol != UINT32_MAX &&
             types.count(std::string(symbols.name(static_cast<NamedType*>(t)->name.last().symbol)));
    };
    for (size_t i = 0; i < inputs.size(); ++i)
    {
      const ConvertInput& in = inputs[i];
      bool given = givenCHeaders.count(i) > 0;
      if (!in.c || !in.parsed || (in.path.extension() != ".c" && !given))
        continue;
      std::set<std::string>& own = cPrivate[&in];
      std::set<std::string> types;
      for (Decl* d : in.parsed->decls)
      {
        std::string name = declName(d);
        if ((d->flags & DF_Static) && (d->kind == NodeKind::FunctionDecl || d->kind == NodeKind::VariableDecl) && !name.empty())
          own.insert(name);
        else if (!given && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl ||
                            (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))))
        {
          if (!name.empty() && !headerNames.count(name))
          {
            own.insert(name);
            types.insert(name);
          }
          else if (name.empty() && d->kind == NodeKind::EnumDecl)
            own.insert("");
        }
      }
      if (!types.empty())
        for (Decl* d : in.parsed->decls)
          if (d->kind == NodeKind::FunctionDecl && !(d->flags & DF_Static))
          {
            auto* f = static_cast<FunctionDecl*>(d);
            bool named = names(f->returnType, types, 0);
            for (Decl* param : f->params)
              named = named || (param->kind == NodeKind::ParamDecl && names(static_cast<ParamDecl*>(param)->type, types, 0));
            if (named && !declName(d).empty())
              own.insert(declName(d));
          }
    }
  }
  for (const ConvertInput& in : inputs)
    if (in.parsed && (cInterfaceHeaders.count(in.path.filename().string()) || givenCHeaders.count(size_t(&in - inputs.data()))))
      for (Decl* d : in.parsed->decls)
        if (d->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(d)->name.empty() &&
            !(givenCHeaders.count(size_t(&in - inputs.data())) && static_cast<FunctionDecl*>(d)->body) &&
            static_cast<FunctionDecl*>(d)->name.last().symbol != UINT32_MAX)
          cInterfaceNames[std::string(symbols.name(static_cast<FunctionDecl*>(d)->name.last().symbol))].insert(static_cast<FunctionDecl*>(d)->params.size());
  std::map<size_t, MacroMap> ownMacros; // a header's macros it leaves defined, read once
  std::map<size_t, std::map<std::string, std::string>> ownObjects; // and its string and name macros
  std::map<size_t, std::map<std::string, std::string>> definedObjects; // every string or name macro it defines
  auto macrosOf = [&](size_t k) -> const MacroMap&
  {
    auto it = ownMacros.find(k);
    if (it == ownMacros.end())
      it = ownMacros.emplace(k, readHeaderMacros({inputs[k].path}, configuration, nullptr, &ownObjects[k], &definedObjects[k], &functionMacroNames,
                                                 cWrittenOutMacros.empty() ? nullptr : &cWrittenOutMacros))
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
    auto mapStd = [&](std::string value) { return mapStdText(std::move(value)); };
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
  std::set<std::string> macroClasses = collectMacroClasses(inputs);
  std::set<std::string> braceableClasses;
  for (const ConvertInput& in : inputs)
    if (in.parsed && !in.parsed->isImported && !in.c)
      collectBraceableClasses(in.parsed->decls, global, symbols, braceableClasses);
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
  // program-wide name in kpp, which the library's has already, so it gets its file's name after it.
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
  // (the interface headers first: the constants they make are written by name in the other files)
  std::vector<const ConvertInput*> converted;
  for (const ConvertInput& in : inputs)
    if (in.c && cScopeHeaders.count(in.path.filename().string()))
      converted.push_back(&in);
  for (const ConvertInput& in : inputs)
    if (!(in.c && cScopeHeaders.count(in.path.filename().string())))
      converted.push_back(&in);
  for (const ConvertInput* convertedInput : converted)
  {
    const ConvertInput& in = *convertedInput;
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
    // (A header given alone that the library's defines configure otherwise than its import holds a
    // header-only library's implementation, fontstash.h's under FONTSTASH_IMPLEMENTATION: converted,
    // without what its import declares. zlib.h, given to minizip's conversion, stays C.)
    std::string interfaceText;
    if (cInterfaceHeaders.count(in.path.filename().string()))
    {
      size_t index = size_t(&in - inputs.data());
      const Preprocessed* full = configuration && givenCHeaders.count(index) ? configuration->impl->of(index) : nullptr;
      if (full)
        interfaceText = configuration->impl->interfaceConfigured(index);
      // (By the lines each keeps: the parse's blanking, `FAR` in zconf.h's typedefs, is no branch.)
      auto keptLines = [](const std::string& t)
      {
        std::vector<bool> kept;
        bool any = false;
        for (char c : t)
          if (c == '\n')
          {
            kept.push_back(any);
            any = false;
          }
          else if (!isspace((unsigned char)c))
            any = true;
        kept.push_back(any);
        return kept;
      };
      if (!full || keptLines(interfaceText) == keptLines(full->configured))
      {
        total.add("the C library's interface headers kept as C (c-headers.txt)");
        continue;
      }
    }
    std::string key = rel.substr(0, rel.size() - ext.size()) + ".kpp";
    FileConverter fc(in, global, symbols);
    fc.cInterfaceHeaders = &cInterfaceHeaders;
    fc.cInterfaceNames = &cInterfaceNames;
    fc.cGivenHeader = givenCHeaders.count(size_t(&in - inputs.data())) > 0;
    if (!interfaceText.empty())
      fc.cInterfaceText = &interfaceText;
    if (configuration)
      fc.cPrivateNames = &configuration->impl->cPrivateNames;
    if (auto renames = cStaticRenames.find(&in); renames != cStaticRenames.end())
      fc.cStaticRenames = &renames->second;
    if (auto own = cPrivate.find(&in); own != cPrivate.end())
      fc.cPrivate = &own->second;
    fc.cConfiguredImports = &cConfiguredImports;
    fc.cWrittenOutMacros = &cWrittenOutMacros;
    fc.cScopeHeaders = &cScopeHeaders;
    fc.cScopeConstants = &cScopeConstants;
    fc.scopedLibraryNames = &scopedLibraryNames;
    fc.scopedLibraries = &scopedLibraries;
    fc.scopedHeaders = &scopedHeaders;
    fc.scopedHeaderImports = &scopedHeaderImports;
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
    fc.macroClasses = &macroClasses;
    fc.braceableClasses = &braceableClasses;
    fc.headerMacros = &headerMacros;
    fc.headerAliases = &headerAliases;
    if (auto renames = fileAliasRenames.find(&in); renames != fileAliasRenames.end())
      fc.fileAliasRenames = renames->second;
    if (configuration)
      fc.preprocessed = configuration->impl->find(in.path);
    if (configuration && in.c)
      fc.cMacros = &configuration->impl->cMacros;
    if (auto ip = inputByPath.find(pathKey(in.path)); ip != inputByPath.end())
    {
      fc.includedMacros = visibleMacros(ip->second);
      fc.objectMacros = visibleObjectMacros(ip->second);
      // The converter's own rewrite of a name stands over a header's definition of it, which may
      // differ by configuration: AlwaysInline.hpp's ALWAYS_INLINE is nothing in DEBUG and
      // __forceinline in Release, [[forceinline]] in both (design.md 12).
      if (!in.c)
        for (const auto& [name, value] : knownMacros)
          fc.objectMacros.erase(name);
      // C: a header holds in the files that include it, with what they define before it (trees.h's
      // tables are D_CODES long, deflate.h's macro, which trees.c includes first).
      if (in.c && fc.isHeader())
        for (size_t k = 0; k < inputs.size(); ++k)
          if (k != ip->second && std::find(includedInputs[k].begin(), includedInputs[k].end(), ip->second) != includedInputs[k].end())
            for (const auto& [name, body] : visibleObjectMacros(k))
              fc.objectMacros.emplace(name, body);
      // C: a define of the library's that is a name holds in every file, written out at its uses
      // (stb_image_write's `STBIW_ZLIB_COMPRESS(data, data_len, out_len, quality)`)
      if (in.c && configuration)
        for (const auto& [name, body] : configuration->impl->cNameDefines)
          fc.objectMacros.emplace(name, body);
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
    piece.cImports = fc.cImports;
    piece.source = in.path.string();
    piece.header = ext != ".cpp" && ext != ".cc" && ext != ".c";
    piece.scopeInterface = in.c && cScopeHeaders.count(in.path.filename().string());
    piece.scopeFunctions = fc.scopeFunctions;
    outputs[key].push_back(std::move(piece));
    for (auto& [k, v] : fc.counts.values)
      total.add(k, v);
  }
  // (A conversion without the tests, the headline's, has none to find.)
  for (const auto& [test, why] : leftOutTests)
    if (!suitesConverted.empty() && !leftOutTestsFound.count(test))
      fprintf(stderr, "convert: %s: no converted test is %s\n", (overridesDir / "left-out-tests.txt").generic_string().c_str(), test.c_str());
  // Where a file is written: below its root's name (its key, by which its overrides are found), or
  // with --convert-base, at its root's path below that directory, so that the output mirrors the
  // source tree (C:/Factorio/libraries/Lua/lapi.c as libraries/Lua/lapi.kpp).
  std::map<std::string, std::string> rootPaths;
  if (!baseDir.empty())
  {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::absolute(baseDir, ec).lexically_normal();
    for (const auto& root : roots)
    {
      std::filesystem::path r = std::filesystem::absolute(root, ec).lexically_normal();
      if (!std::filesystem::is_directory(r, ec))
        r = r.parent_path();
      std::filesystem::path rel = r.lexically_relative(base);
      if (!rel.empty() && rel.native()[0] != '.')
        rootPaths[r.filename().generic_string()] = rel.generic_string();
    }
  }
  auto outputKey = [&](const std::string& key) -> std::string
  {
    size_t slash = key.find('/');
    if (slash != std::string::npos)
      if (auto it = rootPaths.find(key.substr(0, slash)); it != rootPaths.end())
        return it->second + key.substr(slash);
    return key;
  };
  // The configured imports' headers (c-headers.txt), beside the library's converted files.
  for (const auto& [header, configured] : cConfiguredImports)
  {
    std::string text;
    if (!readFile(overridesDir / configured, text))
    {
      fprintf(stderr, "convert: %s: no %s for c-headers.txt's `%s %s`\n", overridesDir.generic_string().c_str(), configured.c_str(), header.c_str(),
              configured.c_str());
      continue;
    }
    std::error_code ec;
    // (beside the header it configures: zstd.h's directory)
    std::vector<std::filesystem::path> beside;
    for (const ConvertInput& in : inputs)
      if (in.path.filename().string() == header)
        beside.push_back(in.path.parent_path());
    for (const auto& root : roots)
      beside.push_back(root);
    for (const auto& directory : beside)
      if (std::filesystem::is_directory(directory, ec))
      {
        std::filesystem::path outPath = outDir / outputKey(relativeTo(directory / configured, roots));
        std::filesystem::create_directories(outPath.parent_path(), ec);
        writeConvertedFile(outPath, text);
        total.add("configured imports written (c-headers.txt)");
        break;
      }
  }
  std::string report = "# Conversion report\n\n";
  size_t files = 0;
  // Each library's (each root's) C imports, in its files' include order, and the names its files use
  // of the headers C++'s standard headers bring in (writeLibraryImports).
  struct LibraryImports
  {
    std::vector<std::string> imports;
    std::map<std::string, std::set<std::string>> used; // padding header -> names
  };
  std::map<std::string, LibraryImports> libraryImports;
  // The namespaces some file holds something in (withoutEmptyNamespaces keeps a file's empty one
  // that is the only declaration of its namespace): the converted files, their hand-written parts and
  // the added files.
  std::set<std::string> holdingNamespaces;
  for (const auto& [key, pieces] : outputs)
    for (const Piece& p : pieces)
      noteNamespacesHolding(p.text, holdingNamespaces);
  for (const auto& [key, ov] : overrideFiles)
    if (ov) // (a key without an override file has none)
      noteNamespacesHolding(ov->appended, holdingNamespaces);
  if (!overridesDir.empty())
  {
    std::error_code ec;
    if (std::filesystem::is_directory(overridesDir / "added", ec))
      for (const auto& entry : std::filesystem::recursive_directory_iterator(overridesDir / "added", ec))
        if (std::string addedText; entry.is_regular_file(ec) && readFile(entry.path(), addedText))
          noteNamespacesHolding(addedText, holdingNamespaces);
  }
  auto rootOf = [](const std::string& key) { return key.find('/') == std::string::npos ? std::string() : key.substr(0, key.find('/')); };
  auto noteLibraryText = [&](const std::string& root, const std::string& text)
  {
    LibraryImports& library = libraryImports[root];
    std::set<std::string> identifiers = identifiersOf(text);
    for (const auto& [header, names] : paddingHeaderNames())
      for (const std::string& name : identifiers)
        if (names.count(name))
          library.used[header].insert(name);
  };
  for (auto& [key, pieces] : outputs)
  {
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.header && !b.header; });
    std::filesystem::path outPath = outDir / outputKey(key);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    OverrideFile* ov = overrideFor(key);
    // (no line naming the sources: the file reads as the program's own, kovarex 2026-11-04;
    // CONVERSION.md lists what each file was converted from)
    std::string text;
    // (the line kppc --convert-library finds a library's interface files by: scopedNamesOf)
    if (std::any_of(pieces.begin(), pieces.end(), [](const Piece& p) { return p.scopeInterface; }))
    {
      text += std::string(scopeInterfaceLine) + "\n";
      // (its functions, whose declarations go: their definitions are in the library's files)
      std::vector<std::string> functions;
      for (const Piece& p : pieces)
        functions.insert(functions.end(), p.scopeFunctions.begin(), p.scopeFunctions.end());
      std::string line = scopeFunctionsLine;
      bool first = true;
      for (const std::string& name : functions)
      {
        if (line.size() + name.size() + 2 > 110)
        {
          text += line + ",\n";
          line = "//  ";
          first = true;
        }
        line += (first ? " " : ", ") + name;
        first = false;
      }
      if (!functions.empty())
        text += line + "\n";
      text += "\n";
    }
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
    size_t namespacesRemoved = 0, namespacesKept = 0;
    text = withoutEmptyNamespaces(text, holdingNamespaces, namespacesRemoved, namespacesKept);
    total.add("empty namespaces removed", namespacesRemoved);
    total.add("empty namespaces kept: the program's only declaration of their namespace", namespacesKept);
    size_t blankLinesRemoved = 0;
    text = withTidyBlankLines(text, blankLinesRemoved);
    total.add("blank lines removed (runs, at the start and end, inside braces)", blankLinesRemoved);
    for (const Piece& p : pieces)
      for (const std::string& header : p.cImports)
        if (std::vector<std::string>& imports = libraryImports[rootOf(key)].imports; std::find(imports.begin(), imports.end(), header) == imports.end())
          imports.push_back(header);
    noteLibraryText(rootOf(key), text);
    if (writeConvertedFile(outPath, text))
      ++files;
    report += "## " + outputKey(key) + "\n";
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
          std::string addedKey = entry.path().lexically_relative(added).generic_string();
          std::filesystem::path rel = outputKey(addedKey);
          std::filesystem::path target = outDir / rel;
          std::string addedText;
          if (!readFile(entry.path(), addedText))
            continue;
          noteLibraryText(rootOf(addedKey), addedText);
          std::filesystem::create_directories(target.parent_path(), ec);
          if (writeConvertedFile(target, addedText))
          {
            ++files;
            total.add("hand-written files added");
            report += "## " + rel.generic_string() + "\n- added from " + entry.path().generic_string() + "\n";
          }
        }
  }
  // Each library's C imports, once, in its c-imports.kpp: an import is the program's wherever it is
  // written (design.md 5), so its files start with their code. In the include order of its files,
  // not sorted: a Windows header may need another before it (dbghelp.h and psapi.h windows.h). The
  // C headers that C++'s standard headers bring in (stdlib.h, string.h, stdio.h: <string> and
  // <memory> include them) are imported only where a file uses a name of theirs it includes no
  // header for, with those names in a comment.
  for (const auto& [root, library] : libraryImports)
  {
    std::string lines;
    size_t count = 0;
    for (const auto& [header, names] : library.used)
      if (std::find(library.imports.begin(), library.imports.end(), header) == library.imports.end())
      {
        std::string shown;
        size_t n = 0;
        for (const std::string& name : names)
          if (n++ < 4)
            shown += (shown.empty() ? "" : ", ") + name;
        lines += "extern \"C\" import \"" + header + "\"; // " + shown + (names.size() > 4 ? ", ..." : "") + " (C++'s headers brought it in)\n";
        ++count;
        total.add("C imports for names C++'s headers brought in (c-imports.kpp)");
      }
    for (const std::string& header : library.imports)
    {
      lines += "extern \"C\" import \"" + header + "\";\n";
      ++count;
    }
    if (!count)
      continue;
    std::string text = "// The C headers this library imports, written once by kppc --convert from its files' #includes:\n"
                       "// an import is the whole program's wherever it is written (design.md 5), so the files start with\n"
                       "// their code. In the include order of the files (a Windows header may need another before it).\n\n" +
                       lines;
    std::filesystem::path outPath = outDir / outputKey(root.empty() ? std::string("c-imports.kpp") : root + "/c-imports.kpp");
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (writeConvertedFile(outPath, text))
      ++files;
    total.add("library C import files written (c-imports.kpp)");
    total.add("C imports written (c-imports.kpp)", count);
    report += "## " + outputKey(root.empty() ? std::string("c-imports.kpp") : root + "/c-imports.kpp") + "\n- the library's C imports: " + std::to_string(count) + "\n";
  }
  std::string summary = "Files written: " + std::to_string(files) + "\n\n";
  for (auto& [k, v] : total.values)
    summary += "- " + k + ": " + std::to_string(v) + "\n";
  report.insert(strlen("# Conversion report\n\n"), summary + "\n");
  writeConvertedFile(outDir / "CONVERSION.md", report);
  return summary;
}

// --- the game's two configurations made one (design.md 12, "The game's DEBUG as config::debug") ---

void setConvertRelease()
{
  convertRelease = true;
}

void setConvertFinal()
{
  convertRelease = true;
  convertFinal = true;
}

void setConvertMarks()
{
  convertMarks = true;
}

namespace
{
  // A converted text cut at its #if groups' marks (convertMarks): text parts, and groups of parts.
  struct MarkedPart
  {
    std::string text;              // a text part's text, without its marks' lines
    std::string group;             // a group's id (FILE:N); empty for a text part
    std::vector<MarkedPart> parts; // a group's contents
  };

  void addText(std::vector<MarkedPart>& parts, std::string_view text)
  {
    if (text.empty())
      return;
    if (!parts.empty() && parts.back().group.empty())
      parts.back().text += text;
    else
      parts.push_back({std::string(text), {}, {}});
  }

  // The parts from `at` to the end mark of `group`, or to the text's end for the whole text (group
  // ""). A mark alone on its line takes the line with it. A mark the conversion lost (an #if line
  // inside a macro's use, which the expansion replaced) is lost in both configurations alike: an end
  // mark without its start is left out, and one that closes an outer group ends the inner ones.
  bool parseMarked(const std::string& text, size_t& at, const std::string& group, std::vector<MarkedPart>& parts,
                   std::vector<std::string>& openGroups)
  {
    static const std::string open = "/*kpp:if ", close = "/*kpp:endif ";
    for (;;)
    {
      size_t o = text.find(open, at), c = text.find(close, at);
      size_t mark = std::min(o, c);
      if (mark == std::string::npos)
      {
        addText(parts, std::string_view(text).substr(at));
        at = text.size();
        return true;
      }
      bool opening = mark == o;
      size_t idStart = mark + (opening ? open.size() : close.size());
      size_t end = text.find("*/", idStart);
      if (end == std::string::npos)
        return false;
      std::string id = text.substr(idStart, end - idStart);
      end += 2;
      size_t from = mark;
      while (from > at && (text[from - 1] == ' ' || text[from - 1] == '\t'))
        --from;
      size_t to = end;
      while (to < text.size() && (text[to] == ' ' || text[to] == '\t' || text[to] == '\r'))
        ++to;
      if ((from == 0 || text[from - 1] == '\n') && (to == text.size() || text[to] == '\n'))
        to = std::min(to + 1, text.size());
      else
        from = mark, to = end;
      addText(parts, std::string_view(text).substr(at, from - at));
      if (!opening && id != group && std::find(openGroups.begin(), openGroups.end(), id) != openGroups.end())
      {
        at = from; // (the outer group's end: this one's was lost)
        return true;
      }
      at = to;
      if (!opening)
      {
        if (id == group)
          return true;
        continue; // (an end without its start)
      }
      MarkedPart g;
      g.group = id;
      openGroups.push_back(id);
      bool nested = parseMarked(text, at, id, g.parts, openGroups);
      openGroups.pop_back();
      if (!nested)
        return false;
      parts.push_back(std::move(g));
    }
  }

  std::string flattened(const std::vector<MarkedPart>& parts)
  {
    std::string out;
    for (const MarkedPart& p : parts)
      out += p.group.empty() ? p.text : flattened(p.parts);
    return out;
  }

  struct MergeReport
  {
    size_t conditional = 0;
    size_t byAsserts = 0; // groups whose debug text is the release one with asserts (differsByAsserts)
    size_t when = 0; // declarations of one configuration written `when (config::debug)` (design.md 19)
    size_t perConfiguration = 0; // declarations whose initializer alone differs, `= config::debug ? a : b`
    size_t final = 0; // groups whose final text alone differs, written with config::finalRelease
    size_t allThree = 0; // groups whose three texts all differ (problems)
    std::map<std::string, std::string> choices; // "FILE GROUP" -> the configuration whose text both get
    std::set<std::string> chosen;
    std::vector<std::string> problems;
  };

  // The configuration constant a group's two texts are chosen by: config::debug between FastDebug's
  // (on) and Release's text, or config::finalRelease between FinalRelease's (on) and Release's (a
  // final build is never a debug one: BuildConstants.hpp's #error).
  struct MergeDimension
  {
    const char* on;      // the condition of the first text
    const char* off;     // of the second
    const char* onName;  // the configurations' names in problems
    const char* offName;
  };
  const MergeDimension debugDimension = {"config::debug", "!config::debug", "debug", "release"};
  const MergeDimension finalDimension = {"config::finalRelease", "!config::finalRelease", "final", "release"};

  // The last character of code in a text, comments and white space left out (0 for none).
  char lastCodeChar(std::string_view text)
  {
    char last = 0;
    for (size_t i = 0; i < text.size();)
    {
      if (text.compare(i, 2, "//") == 0)
        i = std::min(text.find('\n', i), text.size());
      else if (text.compare(i, 2, "/*") == 0)
      {
        size_t e = text.find("*/", i + 2);
        i = e == std::string_view::npos ? text.size() : e + 2;
      }
      else if (text[i] == '"' || (text[i] == '\'' && !isDigitSeparator(text, i)))
      {
        char quote = text[i];
        for (++i; i < text.size() && text[i] != quote && text[i] != '\n'; ++i)
          i += text[i] == '\\';
        last = quote;
        ++i;
      }
      else
      {
        if (!isspace((unsigned char)text[i]))
          last = text[i];
        ++i;
      }
    }
    return last;
  }

  std::string firstLine(const std::string& text)
  {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      return "";
    size_t end = text.find_first_of("\r\n", first);
    std::string line = text.substr(first, end == std::string::npos ? std::string::npos : end - first);
    return line.size() > 90 ? line.substr(0, 90) + "..." : line;
  }

  bool blankText(const std::string& text) { return text.find_first_not_of(" \t\r\n") == std::string::npos; }

  // Whether a statement or declaration may start after a text: after `;`, `{`, `}`, the `:` of an
  // access specifier or a label, the `)` of a condition, `else` or `do`, or at the start.
  bool statementAfter(std::string_view before)
  {
    std::string_view tail = before.substr(before.size() > 4096 ? before.size() - 4096 : 0);
    char previous = lastCodeChar(tail);
    if (previous == 0 || previous == ';' || previous == '{' || previous == '}' || previous == ':')
      return true;
    // (a `)` closes a statement's condition only after if, while, for or switch: a call's, a chain
    // of cxxopts' `("w,wait", "...")`, is an expression's part)
    if (previous == ')')
    {
      size_t close = tail.find_last_of(')');
      int depth = 0;
      size_t open = close;
      for (; open != std::string_view::npos; --open)
      {
        depth += tail[open] == ')';
        depth -= tail[open] == '(';
        if (depth == 0 || open == 0)
          break;
      }
      if (depth != 0)
        return false;
      size_t end = tail.find_last_not_of(" \t\r\n", open == 0 ? std::string_view::npos : open - 1);
      if (end == std::string_view::npos || open == 0)
        return false;
      size_t start = end;
      while (start > 0 && isIdentChar(tail[start - 1]))
        --start;
      std::string_view word = tail.substr(start, end + 1 - start);
      return word == "if" || word == "while" || word == "for" || word == "switch" || word == "constexpr";
    }
    size_t end = tail.find_last_not_of(" \t\r\n");
    for (std::string_view word : {"else", "do"})
      if (end != std::string_view::npos && end + 1 >= word.size() && tail.substr(end + 1 - word.size(), word.size()) == word &&
          (end + 1 == word.size() || !isIdentChar(tail[end - word.size()])))
        return true;
    return false;
  }

  // A group's text that continues an if-else chain, links alone (`#ifndef FINAL_RELEASE else if (b)
  // {...} #endif`: `else if (c) A`, `else B`), with each link's condition joined with the
  // configuration's: `else if (!config::finalRelease && (c)) A`, `else if (!config::finalRelease) B`.
  // In the configuration without the text no link is taken, as no link was there. "" when the text
  // is no such links.
  std::string elseLinksWhen(const std::string& text, const char* condition)
  {
    auto skip = [&](size_t k)
    {
      while (k < text.size())
      {
        if (isspace((unsigned char)text[k]))
          ++k;
        else if (text.compare(k, 2, "//") == 0)
          k = std::min(text.find('\n', k), text.size());
        else if (text.compare(k, 2, "/*") == 0)
        {
          size_t e = text.find("*/", k + 2);
          k = e == std::string::npos ? text.size() : e + 2;
        }
        else
          break;
      }
      return k;
    };
    // (the position after the bracket that closes the one at k, string and character literals skipped)
    auto matching = [&](size_t k) -> size_t
    {
      int depth = 0;
      for (; k < text.size(); ++k)
      {
        char c = text[k];
        if (c == '"' || (c == '\'' && !isDigitSeparator(text, k)))
        {
          for (++k; k < text.size() && text[k] != c; ++k)
            k += text[k] == '\\';
          continue;
        }
        depth += c == '(' || c == '{';
        depth -= c == ')' || c == '}';
        if (depth == 0)
          return k + 1;
      }
      return std::string::npos;
    };
    // (the end of the statement at k: a block, or up to its `;` outside brackets)
    auto statementEnd = [&](size_t k) -> size_t
    {
      k = skip(k);
      if (k < text.size() && text[k] == '{')
        return matching(k);
      for (; k < text.size(); ++k)
      {
        if (text[k] == '(' || text[k] == '{')
        {
          k = matching(k);
          if (k == std::string::npos)
            return k;
          --k;
        }
        else if (text[k] == ';')
          return k + 1;
      }
      return std::string::npos;
    };
    std::string out;
    bool any = false;
    for (size_t i = 0;;)
    {
      size_t k = skip(i);
      out += text.substr(i, k - i);
      i = k;
      if (i >= text.size())
        break;
      if (text.compare(i, 4, "else") != 0 || (i + 4 < text.size() && isIdentChar(text[i + 4])))
        return "";
      size_t j = skip(i + 4);
      size_t end;
      if (text.compare(j, 2, "if") == 0 && (j + 2 >= text.size() || !isIdentChar(text[j + 2])))
      {
        size_t open = skip(j + 2);
        if (open >= text.size() || text[open] != '(')
          return "";
        size_t close = matching(open);
        if (close == std::string::npos)
          return "";
        out += text.substr(i, open + 1 - i) + condition + " && (" + text.substr(open + 1, close - 1 - (open + 1)) + "))";
        end = statementEnd(close);
        if (end == std::string::npos)
          return "";
        out += text.substr(close, end - close);
      }
      else
      {
        end = statementEnd(i + 4);
        if (end == std::string::npos)
          return "";
        out += "else if (" + std::string(condition) + ")" + text.substr(i + 4, end - (i + 4));
      }
      i = end;
      any = true;
    }
    return any ? out : "";
  }

  std::string_view codeStart(std::string_view item);
  bool definesType(std::string_view item);

  // The items of a group's text where declarations and statements stand: each a statement or a
  // declaration, ending with its `;` at depth 0 or with the `}` that closes a body at depth 0 (one
  // followed by `;`, `else`, `while`, `catch` or a name, `struct S {...} s;`, runs on). Comments and
  // blank lines before an item are its own.
  std::vector<std::string> groupItems(const std::string& text)
  {
    std::vector<std::string> items;
    size_t start = 0;
    int depth = 0;
    auto nextCode = [&](size_t i)
    {
      for (;;)
      {
        while (i < text.size() && isspace((unsigned char)text[i]))
          ++i;
        if (text.compare(i, 2, "//") == 0)
          i = std::min(text.find('\n', i), text.size());
        else if (text.compare(i, 2, "/*") == 0)
        {
          size_t e = text.find("*/", i + 2);
          i = e == std::string::npos ? text.size() : e + 2;
        }
        else
          return i;
      }
    };
    for (size_t i = 0; i < text.size();)
    {
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
      char c = text[i];
      if (c == '"' || (c == '\'' && !isDigitSeparator(text, i)))
      {
        for (++i; i < text.size() && text[i] != c && text[i] != '\n'; ++i)
          i += text[i] == '\\';
        ++i;
        continue;
      }
      if (c == '(' || c == '[' || c == '{')
        ++depth;
      else if (c == ')' || c == ']' || c == '}')
        --depth;
      ++i;
      bool end = depth == 0 && c == ';';
      if (depth == 0 && c == '}')
      {
        size_t n = nextCode(i);
        auto word = [&](std::string_view w)
        { return text.compare(n, w.size(), w) == 0 && (n + w.size() >= text.size() || !isIdentChar(text[n + w.size()])); };
        end = !(n < text.size() && (text[n] == ';' || text[n] == ',' || word("else") || word("while") || word("catch") ||
                                    (isIdentChar(text[n]) && !isdigit((unsigned char)text[n]) && lastCodeChar(text.substr(start, i - start)) == '}' &&
                                     definesType(text.substr(start, i - start)))));
      }
      if (end)
      {
        // (the rest of its line, a trailing comment, is its own)
        size_t e = i;
        while (e < text.size() && (text[e] == ' ' || text[e] == '\t'))
          ++e;
        if (text.compare(e, 2, "//") == 0)
          e = std::min(text.find('\n', e), text.size());
        if (e < text.size() && text[e] == '\r')
          ++e;
        if (e < text.size() && text[e] == '\n')
          ++e;
        items.push_back(text.substr(start, e - start));
        start = i = e;
      }
    }
    if (start < text.size())
      items.push_back(text.substr(start));
    return items;
  }

  // The first words of an item's code, comments and attributes left out.
  std::string_view codeStart(std::string_view item)
  {
    size_t i = 0;
    for (;;)
    {
      while (i < item.size() && isspace((unsigned char)item[i]))
        ++i;
      if (item.compare(i, 2, "//") == 0)
        i = std::min(item.find('\n', i), item.size());
      else if (item.compare(i, 2, "/*") == 0)
      {
        size_t e = item.find("*/", i + 2);
        i = e == std::string_view::npos ? item.size() : e + 2;
      }
      else if (item.compare(i, 2, "[[") == 0)
      {
        size_t e = item.find("]]", i + 2);
        i = e == std::string_view::npos ? item.size() : e + 2;
      }
      else
        return item.substr(i);
    }
  }

  // Whether an item defines a class, union or enumeration (`struct S {...} s;` runs on after its `}`).
  bool definesType(std::string_view item)
  {
    std::string_view code = codeStart(item);
    for (std::string_view w : {"typedef", "struct", "class", "union", "enum"})
      if (code.substr(0, w.size()) == w && (code.size() == w.size() || !isIdentChar(code[w.size()])))
        return true;
    return false;
  }

  // Whether an item declares a name (a local, a member, a namespace's variable, function, class or
  // alias), as `when` takes it (design.md 19), or is a statement or something else (an access
  // specifier, static_assert): a declaration starts with a type, its words (const, static,
  // constexpr...) or a class's keyword, and names something after the type: `int x = 3;`,
  // `std::vector<Cell*> neighbors;`, `Timer timer("x");`, `void dump() const {...}`; not `x = 3;`,
  // `f(x);`, `obj.f();`, `return x;`, `++x;`, `f<int>(x);`.
  bool declaresName(std::string_view item)
  {
    std::string_view code = codeStart(item);
    if (code.empty() || !(isalpha((unsigned char)code[0]) || code[0] == '_' || code[0] == ':'))
      return false;
    size_t i = 0;
    auto skipSpace = [&] { while (i < code.size() && isspace((unsigned char)code[i])) ++i; };
    auto word = [&]() -> std::string_view
    {
      skipSpace();
      size_t b = i;
      while (i < code.size() && isIdentChar(code[i]))
        ++i;
      return code.substr(b, i - b);
    };
    static const std::set<std::string_view> statements = {"return", "if", "for", "while", "switch", "do", "delete", "throw", "break",
                                                         "continue", "goto", "case", "default", "else", "try", "static_assert",
                                                         "public", "private", "protected", "using", "namespace", "friend", "template",
                                                         "assert", "release_assert", "co_return", "co_yield", "co_await"};
    static const std::set<std::string_view> leading = {"const", "static", "constexpr", "inline", "thread_local", "mutable", "volatile",
                                                      "extern", "virtual", "explicit", "unsigned", "signed", "long", "short",
                                                      "struct", "class", "union", "enum", "typename", "consteval", "constinit"};
    std::string_view first = word();
    if (first == "using")
    {
      // an alias, `using MapType = HashMap<...>;` (a debug-only one used by a later `when`), not
      // `using namespace` or a using-declaration
      std::string_view alias = word();
      skipSpace();
      return !alias.empty() && alias != "namespace" && i < code.size() && code[i] == '=';
    }
    if (first.empty() || statements.count(first))
      return false;
    if (first == "typedef")
      return true;
    while (leading.count(first))
    {
      size_t save = i;
      std::string_view next = word();
      if (next.empty())
      {
        i = save;
        break;
      }
      first = next;
    }
    // the type's name: NAME(::NAME)*(<...>)?, with `*`, `&` and const after it
    for (;;)
    {
      skipSpace();
      if (i < code.size() && code[i] == '<')
      {
        int depth = 0;
        for (; i < code.size(); ++i)
        {
          depth += code[i] == '<' ? 1 : code[i] == '>' ? -1 : 0;
          if (depth == 0)
            break;
          if (code[i] == ';' || code[i] == '{')
            return false;
        }
        ++i;
        skipSpace();
      }
      if (code.compare(i, 2, "::") == 0)
      {
        i += 2;
        if (word().empty())
          return false;
        continue;
      }
      break;
    }
    for (;;)
    {
      skipSpace();
      if (i < code.size() && (code[i] == '*' || code[i] == '&'))
        ++i;
      else if (code.compare(i, 5, "const") == 0 && (i + 5 >= code.size() || !isIdentChar(code[i + 5])))
        i += 5;
      else
        break;
    }
    // then the declared name (an operator is a member function too)
    skipSpace();
    if (code.compare(i, 8, "operator") == 0)
      return true;
    std::string_view name = word();
    if (name.empty())
      return false; // (`f(x);`, `x = 3;`; a constructor declared in one configuration stays `if constexpr`)
    skipSpace();
    return i < code.size() && strchr("=;({[,:", code[i]);
  }

  // A declaration that differs per configuration only in its initializer (`constexpr i32 ROW_STEP = 4;`
  // in debug, `= 2;` in release): one declaration choosing by the configuration, design.md 19's
  // `constexpr i32 ROW_STEP = config::debug ? 4 : 2;`. "" when the two are not so.
  std::string perConfigurationInitializer(const std::string& debug, const std::string& release, const MergeDimension& dimension)
  {
    auto split = [](const std::string& item, std::string& head, std::string& value, std::string& tail)
    {
      size_t semicolon = item.rfind(';');
      size_t equals = item.find(" = ");
      if (semicolon == std::string::npos || equals == std::string::npos || equals > semicolon)
        return false;
      head = item.substr(0, equals);
      value = item.substr(equals + 3, semicolon - equals - 3);
      tail = item.substr(semicolon);
      return head.find_first_of("({") == std::string::npos && value.find_first_of(";{") == std::string::npos;
    };
    // (each value a literal: a choice's both arms are compiled in both configurations, so a name of one
    // configuration's, SpiderLeg's `this->debugDraw`, is two `when`s instead)
    auto literal = [](const std::string& value)
    {
      for (size_t i = 0; i < value.size();)
        if (isalpha((unsigned char)value[i]) || value[i] == '_')
        {
          size_t e = i;
          while (e < value.size() && isIdentChar(value[e]))
            ++e;
          std::string word = value.substr(i, e - i);
          if (word != "true" && word != "false" && word != "nullptr")
            return false;
          i = e;
        }
        else if (isdigit((unsigned char)value[i]))
        {
          while (i < value.size() && (isIdentChar(value[i]) || value[i] == '.'))
            ++i;
        }
        else if (strchr(" \t+-()", value[i]))
          ++i;
        else
          return false;
      return true;
    };
    std::string dh, dv, dt, rh, rv, rt;
    if (!split(debug, dh, dv, dt) || !split(release, rh, rv, rt) || dh != rh || dv == rv || !literal(dv) || !literal(rv))
      return "";
    return dh + " = " + dimension.on + " ? " + dv + " : " + rv + dt;
  }

  // A group whose configurations' texts differ, where declarations and statements stand (after `;`,
  // `{`, `}` or the `:` of an access specifier or label), each text whole ones (ending with `;` or
  // `}`), in a block, a class or a namespace (design.md 19):
  // - a declaration of one configuration is a `when (config::debug)` (`when (!config::debug)`) one:
  //   it is the enclosing scope's, so a later group's use sees it, an object lives to the end of
  //   the block, a member is in the class's layout where the configuration has it;
  // - one declaration in each that differs only in its initializer is one, choosing:
  //   `constexpr i32 ROW_STEP = config::debug ? 4 : 2;`;
  // - the other items, statements, are `if constexpr (config::debug) { ... }` blocks, in order.
  // Anywhere else (an initializer list, a parameter list, an expression) is a problem, reported.
  std::string conditionalGroup(const std::string& debug, const std::string& release, std::string_view before, const std::string& where,
                               MergeReport& report, const MergeDimension& dimension)
  {
    auto whole = [](const std::string& text)
    {
      char last = lastCodeChar(text);
      return last == 0 || last == ';' || last == '}';
    };
    if (!statementAfter(before) || !whole(debug) || !whole(release))
    {
      report.problems.push_back(where + ": the configurations differ where no statement or declaration stands (" + dimension.onName + " `" +
                                firstLine(debug) + "`, " + dimension.offName + " `" + firstLine(release) + "`)");
      return debug;
    }
    ++report.conditional;
    const std::string& sample = blankText(debug) ? release : debug;
    size_t first = sample.find_first_not_of(" \t\r\n");
    size_t lineStart = sample.rfind('\n', first);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    std::string indent = sample.substr(lineStart, first - lineStart);
    auto block = [&](const std::string& text)
    {
      std::string out = indent + "{\n";
      for (size_t start = 0; start < text.size();)
      {
        size_t e = text.find('\n', start);
        if (e == std::string::npos)
          e = text.size();
        std::string line = text.substr(start, e - start);
        out += (line.find_first_not_of(" \t\r") == std::string::npos ? std::string() : "  " + line) + "\n";
        start = e + 1;
      }
      return out + indent + "}\n";
    };
    std::vector<std::string> debugItems = groupItems(debug), releaseItems = groupItems(release);
    auto onlyCode = [&](const std::vector<std::string>& items)
    {
      std::vector<std::string> code;
      for (const std::string& item : items)
        if (!blankText(item) && lastCodeChar(item) != 0)
          code.push_back(item);
      return code;
    };
    if (std::vector<std::string> d = onlyCode(debugItems), r = onlyCode(releaseItems); d.size() == 1 && r.size() == 1 && declaresName(d[0]))
      if (std::string chosen = perConfigurationInitializer(d[0], r[0], dimension); !chosen.empty())
      {
        ++report.perConfiguration;
        return chosen;
      }
    // One configuration's items: each declaration a `when` (an alias too: Gui.cpp's debug-only
    // `using MapType`, design.md 19), each run of statements one block.
    auto items = [&](const std::vector<std::string>& list, const char* condition)
    {
      std::string out, statements;
      auto flush = [&]
      {
        if (!blankText(statements))
          out += indent + "if constexpr (" + condition + ")\n" + block(statements);
        else
          out += statements;
        statements.clear();
      };
      for (const std::string& item : list)
        if (std::string_view code = codeStart(item); code.substr(0, 14) == "static_assert(")
        {
          // A static_assert of one configuration (Map.cpp's FINAL_RELEASE one, at namespace scope,
          // where no `if constexpr` stands): one checked where its configuration's is,
          // `static_assert(!config::finalRelease || (condition), "...")`.
          flush();
          size_t comma = 14;
          for (int depth = 0; comma < code.size(); ++comma)
          {
            if (code[comma] == '"')
              break;
            depth += code[comma] == '(';
            depth -= code[comma] == ')';
            if (depth < 0 || (depth == 0 && code[comma] == ','))
              break;
          }
          size_t at = item.size() - code.size();
          std::string negated = condition[0] == '!' ? std::string(condition + 1) : "!" + std::string(condition);
          out += item.substr(0, at) + "static_assert(" + negated + " || (" + std::string(code.substr(14, comma - 14)) + ")" + std::string(code.substr(comma));
        }
        else if (std::string_view code = codeStart(item); code.substr(0, 6) == "when (")
        {
          // A `when` of the other dimension's merge (the debug and release texts made one, in a group
          // whose three texts differ): one `when` of both conditions.
          flush();
          size_t close = 5;
          for (int depth = 0; close < code.size(); ++close)
          {
            depth += code[close] == '(';
            depth -= code[close] == ')';
            if (depth == 0)
              break;
          }
          size_t at = item.size() - code.size();
          out += item.substr(0, at) + "when (" + condition + " && " + std::string(code.substr(6, close - 6)) + ")" + std::string(code.substr(close + 1));
          ++report.when;
        }
        else if (declaresName(item))
        {
          flush();
          // (its comments and blank lines before it stay before it)
          std::string_view code = codeStart(item);
          size_t at = item.size() - code.size();
          out += item.substr(0, at) + "when (" + condition + ") " + std::string(code);
          ++report.when;
        }
        else
          statements += item;
      flush();
      return out;
    };
    return items(debugItems, dimension.on) + items(releaseItems, dimension.off);
  }

  // Whether a group's debug text is its release text with asserts (Lua's check_exp, `(lua_assert(c),
  // (e))` in debug and `(e)` in release; its lua_assert, `assert(c)` and `((void)0)`): kpp's assert
  // is the configuration's own (nothing where config::debug is false), so the debug text is right in
  // both. Compared with the asserts taken out of the debug text, `(void)0` out of the release one,
  // and parentheses, commas and white space out of both.
  bool differsByAsserts(const std::string& debug, const std::string& release)
  {
    if (debug.find("assert(") == std::string::npos)
      return false;
    std::string stripped;
    for (size_t i = 0; i < debug.size();)
    {
      // `([&] { assert(...); }())`, the converter's assert in an expression, or `assert(...)`
      size_t lambda = debug.compare(i, 14, "([&] { assert(") == 0 ? i : std::string::npos;
      size_t call = lambda == std::string::npos && debug.compare(i, 7, "assert(") == 0 && (i == 0 || !isIdentChar(debug[i - 1])) ? i : std::string::npos;
      if (lambda == std::string::npos && call == std::string::npos)
      {
        stripped += debug[i++];
        continue;
      }
      size_t open = lambda != std::string::npos ? lambda : i + 6;
      int depth = 0;
      size_t k = open;
      for (; k < debug.size(); ++k)
      {
        if (debug[k] == '"' || (debug[k] == '\'' && !isDigitSeparator(debug, k)))
        {
          char quote = debug[k];
          for (++k; k < debug.size() && debug[k] != quote; ++k)
            k += debug[k] == '\\';
          continue;
        }
        depth += debug[k] == '(';
        depth -= debug[k] == ')';
        if (depth == 0)
          break;
      }
      i = k + 1;
    }
    auto normal = [](const std::string& text)
    {
      std::string out;
      for (size_t i = 0; i < text.size(); ++i)
      {
        if (text.compare(i, 2, "//") == 0)
          i = std::min(text.find('\n', i), text.size());
        else if (text.compare(i, 2, "/*") == 0)
          i = std::min(text.find("*/", i + 2), text.size() - 2) + 1;
        else if (!isspace((unsigned char)text[i]) && text[i] != '(' && text[i] != ')' && text[i] != ',' && text[i] != ';')
          out += text[i];
      }
      for (size_t at = out.find("void0"); at != std::string::npos; at = out.find("void0"))
        out.erase(at, 5);
      return out;
    };
    // (an `else` whose statement is an assert, PlayerInputSource's `#ifdef DEBUG else assert(false);
    // #endif`: an empty else in release)
    std::string releaseText = normal(release), debugText = normal(stripped);
    return debugText == releaseText || (releaseText.empty() && debugText == "else");
  }

  // The two configurations' parts as one text: their text parts the same, each group the same (its
  // text once) or written by conditionalGroup. False where a text part differs or the parts do not
  // match (a mark the conversion lost in one of them).
  // With a third configuration (final, FinalRelease's parts; null for none): a group whose release
  // and final texts are the same is chosen by config::debug as above; one whose debug and release
  // texts are the same by config::finalRelease (final the first text); one whose three texts all
  // differ is a problem, reported.
  bool mergeParts(const std::vector<MarkedPart>& debug, const std::vector<MarkedPart>& release, const std::vector<MarkedPart>* final, std::string& out,
                  const std::string& file, MergeReport& report)
  {
    if (debug.size() != release.size() || (final && final->size() != debug.size()))
      return false;
    MergeReport before = report;
    std::string merged;
    size_t skipSemicolon = std::string::npos; // the next text part's `;`, which a statement's group took
    for (size_t i = 0; i < debug.size(); ++i)
    {
      const MarkedPart& d = debug[i];
      const MarkedPart& r = release[i];
      const MarkedPart* f = final ? &(*final)[i] : nullptr;
      bool same = d.group == r.group && (!f || f->group == d.group) && (!d.group.empty() || (d.text == r.text && (!f || f->text == d.text)));
      if (!same)
      {
        report = before;
        return false;
      }
      if (d.group.empty())
      {
        merged += skipSemicolon == std::string::npos ? d.text : d.text.substr(0, skipSemicolon) + d.text.substr(skipSemicolon + 1);
        skipSemicolon = std::string::npos;
        continue;
      }
      std::string inner;
      if (mergeParts(d.parts, r.parts, f ? &f->parts : nullptr, inner, file, report))
        merged += inner;
      else if (auto choice = report.choices.find(file + " " + d.group); choice != report.choices.end())
      {
        merged += flattened(choice->second == "release" ? r.parts : choice->second == "final" && f ? f->parts : d.parts);
        report.chosen.insert(choice->first);
      }
      else
      {
        std::string dt = flattened(d.parts), rt = flattened(r.parts);
        std::string ft = f ? flattened(f->parts) : rt;
        const MergeDimension* dimension = &debugDimension;
        std::string on = dt, off = rt;
        if (ft != rt)
        {
          // All three differ (an IMGUI_ENABLED block that FinalRelease leaves out, its DEBUG groups
          // inside): the debug and release texts made one first, chosen by config::debug, and that
          // text against the final one by config::finalRelease (conditionalGroup joins a `when` in
          // it: `when (!config::finalRelease && config::debug)`).
          if (dt != rt)
          {
            std::string debugAndRelease;
            if (!mergeParts(d.parts, r.parts, nullptr, debugAndRelease, file, report))
              debugAndRelease = conditionalGroup(dt, rt, out + merged, file + " (#if group " + d.group + ")", report, debugDimension);
            off = debugAndRelease;
            ++report.allThree;
          }
          dimension = &finalDimension;
          on = ft;
          ++report.final;
        }
        // (an assert is the configuration's own, config::debug's; a release_assert is in every build)
        else if (differsByAsserts(dt, rt))
        {
          merged += dt;
          ++report.byAsserts;
          continue;
        }
        // Links of an if-else chain that one configuration alone has: each link's condition joined
        // with the configuration's (elseLinksWhen).
        if (blankText(on) != blankText(off))
          if (std::string joined = elseLinksWhen(blankText(on) ? off : on, blankText(on) ? dimension->off : dimension->on); !joined.empty())
          {
            merged += joined;
            ++report.conditional;
            continue;
          }
        // A macro's use within a line (its id `FILE:@OFFSET`): as a statement, the `;` after it
        // closes it, `if constexpr (config::debug) { debug; } else { release; }`.
        bool inLine = on.find('\n') == std::string::npos && off.find('\n') == std::string::npos;
        const MarkedPart* next = i + 1 < debug.size() && debug[i + 1].group.empty() ? &debug[i + 1] : nullptr;
        size_t semicolon = next ? next->text.find_first_not_of(" \t") : std::string::npos;
        if (inLine && semicolon != std::string::npos && next->text[semicolon] == ';' && statementAfter(out + merged))
        {
          auto statement = [](const std::string& text) { return blankText(text) ? std::string() : " " + text + ";"; };
          merged += blankText(on) ? "if constexpr (" + std::string(dimension->off) + ") {" + statement(off) + " }"
                                  : "if constexpr (" + std::string(dimension->on) + ") {" + statement(on) + " }" +
                                      (blankText(off) ? "" : " else {" + statement(off) + " }");
          ++report.conditional;
          skipSemicolon = semicolon;
        }
        else
          merged += conditionalGroup(on, off, out + merged, file + " (#if group " + d.group + ")", report, *dimension);
      }
    }
    out += merged;
    return true;
  }

  // (final: FinalRelease's text, or null without a third configuration)
  std::string mergedConfigurations(const std::string& file, const std::string& debug, const std::string& release, const std::string* final,
                                   MergeReport& report)
  {
    std::vector<MarkedPart> d, r, f;
    size_t a = 0, b = 0, c = 0;
    std::vector<std::string> openD, openR, openF;
    if (!parseMarked(debug, a, "", d, openD) || !parseMarked(release, b, "", r, openR) || (final && !parseMarked(*final, c, "", f, openF)))
    {
      report.problems.push_back(file + ": its #if marks do not nest");
      return debug;
    }
    std::string out;
    if (mergeParts(d, r, final ? &f : nullptr, out, file, report))
      return out;
    // (the first line where they differ: debug's and release's, else release's and final's)
    std::string fd = flattened(d), fr = flattened(r);
    const char* firstName = "debug";
    const char* secondName = "release";
    if (final && fd == fr)
    {
      fr = flattened(f);
      firstName = "release";
      secondName = "final";
    }
    size_t at = 0, line = 1;
    while (at < fd.size() && at < fr.size() && fd[at] == fr[at])
      line += fd[at++] == '\n';
    size_t lineStart = fd.rfind('\n', at == 0 ? 0 : at - 1);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    report.problems.push_back(file + ":" + std::to_string(line) + ": the configurations differ outside an #if group (" + firstName + " `" +
                              firstLine(fd.substr(lineStart)) + "`, " + secondName + " `" + firstLine(fr.substr(lineStart)) + "`)");
    return fd;
  }

  // A library's c-imports.kpp: the debug one's imports, then the release one's it lacks (an import is
  // harmless where nothing uses it).
  std::string mergedImports(const std::string& debug, const std::string& release)
  {
    std::string out = debug;
    if (!out.empty() && out.back() != '\n')
      out += '\n';
    for (size_t start = 0; start < release.size();)
    {
      size_t e = release.find('\n', start);
      if (e == std::string::npos)
        e = release.size();
      std::string line = release.substr(start, e - start);
      if (line.rfind("extern \"C\" import", 0) == 0 && debug.find(line) == std::string::npos)
        out += line + "\n";
      start = e + 1;
    }
    return out;
  }
}

int convertMerge(const std::filesystem::path& debugDir, const std::filesystem::path& releaseDir, const std::filesystem::path& outDir,
                 const std::filesystem::path& choicesFile, const std::filesystem::path& finalDir)
{
  MergeReport report;
  // The groups whose texts no `if constexpr` can choose between (an expression's part), each taken
  // from one configuration for all: `FILE GROUP debug|release|final  why`, reviewed by hand.
  if (!choicesFile.empty())
  {
    std::string text;
    if (!readFile(choicesFile, text))
      report.problems.push_back(choicesFile.generic_string() + ": cannot be read");
    for (size_t start = 0; start < text.size();)
    {
      size_t e = text.find('\n', start);
      if (e == std::string::npos)
        e = text.size();
      std::string line = text.substr(start, e - start);
      start = e + 1;
      if (line.empty() || line[0] == '#')
        continue;
      char file[512], group[256], configuration[16];
      if (sscanf(line.c_str(), "%511s %255s %15s", file, group, configuration) == 3 &&
          (std::string(configuration) == "debug" || std::string(configuration) == "release" || std::string(configuration) == "final"))
        report.choices[std::string(file) + " " + group] = configuration;
      else
        report.problems.push_back(choicesFile.generic_string() + ": not `FILE GROUP debug|release|final why`: " + line);
    }
  }
  std::map<std::string, std::string> merged; // the converted files, by path below outDir
  size_t files = 0;
  std::error_code ec;
  auto write = [&](const std::string& rel, const std::string& text)
  {
    std::filesystem::path target = outDir / rel;
    std::filesystem::create_directories(target.parent_path(), ec);
    writeConvertedFile(target, text);
  };
  for (const auto& entry : std::filesystem::recursive_directory_iterator(debugDir, ec))
  {
    if (!entry.is_regular_file(ec))
      continue;
    std::string rel = entry.path().lexically_relative(debugDir).generic_string();
    std::string debug, release, final;
    readFile(entry.path(), debug);
    if (entry.path().extension() != ".kpp")
    {
      write(rel, debug); // CONVERSION.md, the configured headers
      continue;
    }
    ++files;
    if (!readFile(releaseDir / rel, release))
    {
      report.problems.push_back(rel + ": only the debug configuration writes it");
      merged[rel] = debug;
    }
    else if (!finalDir.empty() && !readFile(finalDir / rel, final))
    {
      report.problems.push_back(rel + ": the final configuration does not write it");
      merged[rel] = mergedConfigurations(rel, debug, release, nullptr, report);
    }
    else if (debug == release && (finalDir.empty() || final == release) && debug.find("/*kpp:") == std::string::npos)
      write(rel, debug); // (a hand-written file, or one without an #if)
    else if (entry.path().filename() == "c-imports.kpp")
      merged[rel] = finalDir.empty() ? mergedImports(debug, release) : mergedImports(mergedImports(debug, release), final);
    else
      merged[rel] = mergedConfigurations(rel, debug, release, finalDir.empty() ? nullptr : &final, report);
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(releaseDir, ec))
    if (entry.is_regular_file(ec) && entry.path().extension() == ".kpp" &&
        !std::filesystem::exists(debugDir / entry.path().lexically_relative(releaseDir), ec))
      report.problems.push_back(entry.path().lexically_relative(releaseDir).generic_string() + ": only the release configuration writes it");
  if (!finalDir.empty())
    for (const auto& entry : std::filesystem::recursive_directory_iterator(finalDir, ec))
      if (entry.is_regular_file(ec) && entry.path().extension() == ".kpp" &&
          !std::filesystem::exists(debugDir / entry.path().lexically_relative(finalDir), ec))
        report.problems.push_back(entry.path().lexically_relative(finalDir).generic_string() + ": only the final configuration writes it");
  // The cleanups again, now that the marks are gone: an #if group's marks held a namespace open, and
  // their lines stood between blank ones.
  std::set<std::string> holding;
  for (const auto& [rel, text] : merged)
    noteNamespacesHolding(text, holding);
  size_t namespacesRemoved = 0, namespacesKept = 0, blankLines = 0;
  for (auto& [rel, text] : merged)
    write(rel, withTidyBlankLines(withoutEmptyNamespaces(text, holding, namespacesRemoved, namespacesKept), blankLines));
  // (a choice of a file this merge has not, the tests' in the conversion without them, is another
  // merge's)
  for (const auto& [key, configuration] : report.choices)
    if (!report.chosen.count(key) && std::filesystem::exists(debugDir / key.substr(0, key.find(' ')), ec))
      report.problems.push_back(choicesFile.generic_string() + ": `" + key + "` chooses for no group whose configurations differ");
  std::string summary = "\n## The two configurations made one (kppc --convert-merge)\n\n- files: " + std::to_string(files) +
                        "\n- #if groups whose configurations differ, written with `when (config::debug)` and `if constexpr (config::debug)` (design.md 19): " + std::to_string(report.conditional) +
                        "\n- their declarations, each `when (config::debug)` or `when (!config::debug)`: " + std::to_string(report.when) +
                        "\n- their declarations whose initializer alone differs, `= config::debug ? a : b`: " + std::to_string(report.perConfiguration) +
                        "\n- groups whose final text alone differs (FinalRelease's), written with `when (config::finalRelease)` and `if constexpr (config::finalRelease)`: " +
                        std::to_string(report.final) + "\n- groups whose debug, release and final texts all differ (problems): " + std::to_string(report.allThree) +
                        "\n- groups whose configurations differ by asserts alone, written as the debug one (kpp's assert is the configuration's): " +
                        std::to_string(report.byAsserts) + "\n- groups taken from one configuration (" + choicesFile.filename().string() + "): " +
                        std::to_string(report.chosen.size()) + "\n- empty namespaces removed once the marks were gone: " + std::to_string(namespacesRemoved) +
                        "\n- problems: " + std::to_string(report.problems.size()) + "\n";
  for (const std::string& p : report.problems)
    summary += "  - " + p + "\n";
  writeConvertedFile(outDir / "CONVERSION.md", summary, "ab");
  printf("convert-merge: %zu files, %zu groups whose configurations differ (%zu declarations when, %zu initializers chosen by the configuration), %zu of them final's, %zu differing by asserts alone, %zu problems\n",
         files, report.conditional, report.when, report.perConfiguration, report.final, report.byAsserts, report.problems.size());
  for (const std::string& p : report.problems)
    fprintf(stderr, "convert-merge: %s\n", p.c_str());
  return report.problems.empty() ? 0 : 1;
}
