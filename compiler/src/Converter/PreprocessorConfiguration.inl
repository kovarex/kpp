// PreprocessorConfiguration.inl: the C libraries' macro tables, text helpers and the build's defines.
// Part of convert.cpp, included where it stood in its anonymous namespace (a file of its own since 2026-11-05, moved without a change).

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

