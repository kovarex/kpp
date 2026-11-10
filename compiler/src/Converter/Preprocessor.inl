// Preprocessor.inl: the preprocessed text of a file, and macro expansion's text helpers.
// Part of convert.cpp, included where it stood in its anonymous namespace (a file of its own since 2026-11-05, moved without a change).

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
    // What blankForParse took out of `configured` for the parse: [offset, offset + length) of the
    // text, and what the conversion writes there instead.
    struct Blank { uint32_t offset, length; std::string replacement; };

    std::string configured;                              // the text, the dropped lines and every conditional directive blanked
    std::vector<std::pair<uint32_t, uint32_t>> dropped;  // the [begin, end) offsets of the lines the configuration drops
    std::vector<std::pair<uint32_t, std::string>> notes; // the #if lines that test a symbol nothing defines, with a TODO text
    std::vector<std::set<std::string>> noteNames;        // and those symbols, a set per note
    MacroTable defined;                                  // the object-like macros it leaves defined beyond the build's (its includes' too)
    std::set<std::string> undefined;                     // the build's or its includes' macros it #undefs
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

  // Known object-like and function-like macros of the corpus that have a direct kpp spelling (a
  // platform's own, setConvertPlatform).
  std::map<std::string, std::string> knownMacros = {
    // Lua's per-compiler attributes (llimits.h's l_noret, luaconf.h's LUAI_FUNC: MSVC's __declspec,
    // GCC's and clang's __attribute__), one spelling on every platform: noreturn is kpp's attribute,
    // a symbol's visibility none of kpp's business.
    {"l_noret", "[[noreturn]] void"}, {"LUAI_FUNC", "extern"}, {"LUAI_DDEC", "extern"}, {"LUAI_DDEF", ""},
    // Stack allocation, kpp's builtin on every target (malloc.h's `#define alloca _alloca`, glibc's
    // `#define alloca(size) __builtin_alloca (size)`): the builtin's name, which the C import declares
    // on every target.
    {"alloca", "__builtin_alloca"},
    {"FORMAT_ATTR", ""}, {"TAG_FORMAT_ATTR", ""}, {"NO_INLINE", "[[noinline]]"}, {"ALWAYS_INLINE", "[[forceinline]]"},
    {"NTAPI", ""}, {"WINAPI", ""}, {"APIENTRY", ""}, {"CALLBACK", ""}, {"STDMETHODCALLTYPE", ""}, {"EXTERNC", "extern \"C\""},
    {"FAR", ""}, {"NEAR", ""}, {"VOID", "void"}, {"NULL", "nullptr"},
    // UnitTest++'s per-compiler macros, nothing in kpp: BREAK_UNDER_DEBUGGER (CheckInternal.kpp's
    // breakUnderDebugger is it; `((void)0)` off Windows) and GCC's UNITTEST_EXTENSION, `__extension__`.
    {"BREAK_UNDER_DEBUGGER", ""}, {"UNITTEST_EXTENSION", ""},
    {"DEV_TOOL_VARIABLE", ""}, {"BEGIN_SUPPRESS_CONVERSIONS", ""}, {"END_SUPPRESS_CONVERSIONS", ""}, {"BEGIN_FORCE_OPTIMIZE", ""},
    {"END_FORCE_OPTIMIZE", ""}, {"SUPPRESS_UBSAN", ""}, {"PADDING_WARNING", ""},
    // errno: the stdlib's accessor, one spelling on every platform (_errno() on Windows, glibc's
    // __errno_location(), FreeBSD's __error()).
    {"errno", "(*platform::errnoLocation())"},
    // SDL_stdinc.h's format for an Sint64, which SDLWindow concatenates with a literal (Windows').
    {"SDL_PRIs64", "\"I64d\""},
    // C headers' constants that are casts, which the C import does not take for constants (signal.h,
    // SDL_audio.h, SDL_touch.h).
    // (SIG_DFL is a null handler on every platform: Windows' `((_crt_signal_t)0)`, glibc's
    // `((__sighandler_t) 0)`)
    {"SIG_DFL", "nullptr"},
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
    {"SKIP_VALUE", "if constexpr (std::isReference<decltype($1)>) Values::recordReference(#1, result); else Values::recordValue($1, #1, result)"},
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
    {"InterlockedIncrement", "(std::kpp_atomic_add32((long*)($1), 1) + 1)"}, {"InterlockedDecrement", "(std::kpp_atomic_add32((long*)($1), -1) - 1)"},
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
      "    constexpr " + name + "(" + type + " value) : value(static_cast<Enum>(value)) { this->validate(); }\n"
      "\n"
      "    constexpr operator " + type + "() const { return static_cast<" + type + ">(this->value); }\n"
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
      "        throw RuntimeError(\"Invalid enum value \\\"\" + std::toString(this->asType()) + \"\\\" of " + name + "\");\n"
      "    }\n"
      "\n"
      "    constexpr bool operator==(Enum other) const { return this->value == other; }\n"
      "    constexpr bool operator==(const " + name + "& other) const = default;\n"
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
        "    " + name + "(const std::String& str) : " + name + "(str.c_str()) {}\n"
        "    " + name + "(const char* str) : value(" + name + "::findValue(str)) {}\n"
        "\n"
        "    std::String str() const\n"
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
        "      throw RuntimeError(\"Invalid value \\\"\" + std::String(val) + \"\\\" of " + name + "\");\n"
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

  // std:: names and their kpp library spelling: the standard library is the scoped library std (design.md 16), the
  // compiler's traits with it. Names not here keep their name and are counted. What clib/ declares stays C's, and
  // global: `::name`, as inside a namespace with its own `malloc`, `std::malloc` meant the C one.
  const std::map<std::string, std::string> stdNames = {
    {"string", "std::String"}, {"string_view", "std::StringView"}, {"wstring", "std::WString"}, {"vector", "std::Vector"}, {"pair", "std::Pair"},
    {"optional", "std::Optional"}, {"map", "std::Map"}, {"set", "std::Set"}, {"unordered_map", "std::HashMap"}, {"unordered_set", "std::HashSet"},
    {"deque", "std::Deque"}, {"list", "std::List"}, {"array", "std::Array"}, {"span", "std::Span"}, {"dynamic_extent", "std::dynamicExtent"}, {"unique_ptr", "std::UniquePtr"}, {"default_delete", "std::DefaultDelete"}, {"queue", "std::Queue"},
    {"replace", "std::replace"}, {"advance", "std::advance"}, {"make_optional", "std::makeOptional"}, {"clock", "::clock"}, {"abort", "::abort"},
    {"shared_ptr", "std::SharedPtr"}, {"weak_ptr", "std::WeakPtr"}, {"function", "std::Function"}, {"tuple", "std::Tuple"}, {"variant", "std::Variant"},
    {"initializer_list", "std::InitializerList"}, {"numeric_limits", "std::Limits"}, {"endian", "std::Endian"}, {"runtime_error", "std::RuntimeError"},
    {"strong_ordering", "std::strong_ordering"}, {"weak_ordering", "std::weak_ordering"}, {"partial_ordering", "std::partial_ordering"},
    {"logic_error", "std::LogicError"}, {"exception", "std::Exception"}, {"out_of_range", "std::OutOfRange"}, {"invalid_argument", "std::InvalidArgument"},
    {"move", "std::move"}, {"forward", "std::forward"}, {"swap", "std::swap"}, {"min", "std::min"}, {"max", "std::max"}, {"clamp", "std::clamp"},
    {"sort", "std::sort"}, {"stable_sort", "std::stableSort"}, {"find", "std::find"}, {"find_if", "std::findIf"}, {"remove_if", "std::removeIf"}, {"erase_if", "std::eraseIf"},
    {"remove", "std::remove"}, {"transform", "std::transform"}, {"count_if", "std::countIf"}, {"all_of", "std::allOf"}, {"any_of", "std::anyOf"},
    {"none_of", "std::noneOf"}, {"lower_bound", "std::lowerBound"}, {"upper_bound", "std::upperBound"}, {"reverse", "std::reverse"},
    {"fill", "std::fill"}, {"copy", "std::copy"}, {"accumulate", "std::accumulate"}, {"begin", "std::begin"}, {"end", "std::end"}, {"size", "std::size"},
    {"isnan", "std::isnan"}, {"isinf", "std::isinf"}, {"abs", "std::abs"}, {"floor", "std::floor"}, {"ceil", "std::ceil"}, {"round", "std::round"},
    {"sqrt", "std::sqrt"}, {"pow", "std::pow"}, {"tolower", "::tolower"}, {"toupper", "::toupper"}, {"size_t", "size_t"},
    {"nullptr_t", "nullptr_t"}, {"to_string", "std::toString"}, {"make_unique", "std::makeUnique"}, {"make_shared", "std::makeShared"},
    {"make_pair", "std::makePair"}, {"make_tuple", "std::makeTuple"}, {"get", "std::get"}, {"declval", "std::declval"}, {"memcpy", "std::memcpy"},
    {"memset", "std::memset"}, {"strlen", "std::strlen"}, {"is_same_v", "std::isSame"}, {"is_constructible_v", "std::isConstructible"}, {"is_array_v", "std::isArray"}, {"is_signed_v", "std::isSigned"},
    {"is_unsigned_v", "std::isUnsigned"}, {"is_pointer_v", "std::isPointer"}, {"is_enum_v", "std::isEnum"}, {"is_integral_v", "std::isIntegral"},
    {"is_floating_point_v", "std::isFloatingPoint"}, {"is_base_of_v", "std::isBaseOf"}, {"is_trivially_copyable_v", "std::isTriviallyCopyable"},
    {"is_arithmetic_v", "std::isArithmetic"}, {"is_fundamental_v", "std::isFundamental"},
    {"is_trivially_destructible_v", "std::isTriviallyDestructible"}, {"is_trivially_copy_assignable_v", "std::isTriviallyCopyAssignable"},
    {"is_trivially_copy_constructible_v", "std::isTriviallyCopyConstructible"}, {"is_trivially_default_constructible_v", "std::isTriviallyDefaultConstructible"},
    {"remove_reference_t", "std::RemoveReference"}, {"remove_const_t", "std::RemoveConst"}, {"remove_cv_t", "std::RemoveCv"},
    {"remove_extent_t", "std::RemoveExtent"}, {"remove_pointer_t", "std::RemovePointer"}, {"decay_t", "std::Decay"}, {"underlying_type_t", "std::UnderlyingType"},
    {"conditional_t", "std::Conditional"}, {"enable_if_t", "std::EnableIf"}, {"chrono", "std::Chrono"}, {"atomic", "std::Atomic"}, {"mutex", "std::Mutex"},
    {"lock_guard", "std::LockGuard"}, {"unique_lock", "std::UniqueLock"}, {"thread", "std::Thread"}, {"condition_variable", "std::ConditionVariable"},
    {"exchange", "std::exchange"}, {"hash", "std::Hash"}, {"less", "std::Less"}, {"equal_to", "std::EqualTo"}, {"bind", "std::bind"}, {"ref", "std::ref"},
    {"cref", "std::cref"}, {"reference_wrapper", "std::ReferenceWrapper"}, {"is_const_v", "std::isConst"}, {"is_reference_v", "std::isReference"},
    {"numeric_limits", "std::Limits"}, {"strtoll", "::strtoll"}, {"strtoull", "::strtoull"}, {"strtod", "::strtod"}, {"strtof", "::strtof"},
    {"ostream", "std::OutStream"}, {"istream", "std::InStream"}, {"stringstream", "std::StringStream"}, {"ostringstream", "std::StringStream"},
    {"nullopt", "std::nullopt"}, {"count", "std::count"}, {"distance", "std::distance"}, {"max_element", "std::maxElement"}, {"min_element", "std::minElement"},
    {"nextafter", "std::nextafter"}, {"rbegin", "std::rbegin"}, {"rend", "std::rend"}, {"unique", "std::unique"}, {"is_class_v", "std::isClass"},
    {"is_convertible_v", "std::isConvertible"}, {"is_null_pointer_v", "std::isNullPointer"}, {"back_inserter", "std::backInserter"},
    {"is_void_v", "std::isVoid"}, {"is_function_v", "std::isFunction"}, {"invoke", "std::invoke"}, {"apply", "std::apply"}, {"tie", "std::tie"},
    {"ignore", "std::ignore"}, {"index_sequence", "std::IndexSequence"}, {"make_index_sequence", "std::MakeIndexSequence"},
    {"index_sequence_for", "std::IndexSequenceFor"}, {"byte", "u8"},
    {"int8_t", "int8_t"}, {"int16_t", "int16_t"}, {"int32_t", "int32_t"}, {"int64_t", "int64_t"}, {"uint8_t", "uint8_t"},
    {"uint16_t", "uint16_t"}, {"uint32_t", "uint32_t"}, {"uint64_t", "uint64_t"}, {"intptr_t", "intptr_t"}, {"uintptr_t", "uintptr_t"},
    {"ptrdiff_t", "ptrdiff_t"}, {"fabs", "std::fabs"}, {"fmod", "std::fmod"}, {"log", "std::log"}, {"exp", "std::exp"}, {"sin", "std::sin"}, {"cos", "std::cos"},
    {"atan2", "std::atan2"}, {"isfinite", "std::isfinite"}, {"signbit", "std::signbit"}, {"trunc", "std::trunc"}, {"lround", "std::lround"},
    {"scoped_lock", "std::ScopedLock"}, {"true_type", "std::TrueType"}, {"false_type", "std::FalseType"}, {"bad_alloc", "std::BadAlloc"},
    {"time_t", "::time_t"}, {"time", "::time"}, {"tm", "::tm"}, {"localtime", "::localtime"},
    {"uncaught_exceptions", "std::uncaught_exceptions"}, {"reverse_iterator", "std::ReverseIterator"},
    {"uninitialized_copy", "std::uninitializedCopy"}, {"visit", "std::visit"},
    // --- batch 2: remaining unmapped names from Factorio ---
    // C library functions → global
    {"_Exit", "::_Exit"}, {"exit", "::exit"}, {"printf", "::printf"}, {"signal", "::signal"},
    {"getenv", "std::getenv"}, {"malloc", "std::malloc"}, {"free", "std::free"}, {"memcmp", "std::memcmp"},
    {"strcpy", "::strcpy"}, {"strstr", "::strstr"}, {"qsort", "::qsort"},
    // math → global
    {"acos", "::acos"}, {"atan", "::atan"}, {"hypot", "::hypot"}, {"log2", "::log2"},
    {"log10", "::log10"}, {"modf", "::modf"}, {"isnormal", "::isnormal"},
    {"nextafterf", "std::nextafterf"},
    // time → global
    {"gmtime", "::gmtime"}, {"strftime", "::strftime"},
    // string conversion → global
    {"stoi", "std::stoi"}, {"stod", "std::stod"}, {"stoul", "std::stoul"}, {"stoull", "std::stoull"}, {"stof", "std::stof"}, {"stoll", "std::stoll"},
    {"iterator_traits", "std::IteratorTraits"}, {"make_move_iterator", "std::makeMoveIterator"},
    {"to_wstring", "std::toWString"}, {"getline", "std::getline"},
    // algorithms → global (camelCase)
    {"adjacent_find", "std::adjacentFind"}, {"copy_backward", "std::copyBackward"},
    {"copy_n", "std::copyN"}, {"equal", "std::equal"}, {"fill_n", "std::fillN"},
    {"generate_n", "std::generateN"}, {"is_sorted", "std::isSorted"},
    {"iter_swap", "std::iterSwap"}, {"lexicographical_compare", "std::lexicographicalCompare"},
    {"merge", "std::merge"}, {"move_backward", "std::moveBackward"},
    {"next", "std::next"}, {"prev", "std::prev"}, {"next_permutation", "std::nextPermutation"},
    {"for_each", "std::forEach"}, {"find_if_not", "std::findIfNot"}, {"copy_if", "std::copyIf"}, {"replace_if", "std::replaceIf"},
    {"binary_search", "std::binarySearch"}, {"equal_range", "std::equalRange"}, {"is_sorted_until", "std::isSortedUntil"},
    {"generate", "std::generate"},
    {"nth_element", "std::nthElement"}, {"rotate", "std::rotate"},
    {"set_difference", "std::setDifference"}, {"set_intersection", "std::setIntersection"},
    {"destroy", "std::destroy"}, {"uninitialized_default_construct", "std::uninitializedDefaultConstruct"},
    {"uninitialized_move", "std::uninitializedMove"},
    {"uninitialized_value_construct", "std::uninitializedValueConstruct"},
    {"make_reverse_iterator", "std::makeReverseIterator"},
    // containers and data structures
    {"bitset", "std::Bitset"}, {"stack", "std::Stack"}, {"priority_queue", "std::PriorityQueue"},
    {"forward_list", "std::ForwardList"}, {"multimap", "std::MultiMap"}, {"multiset", "std::MultiSet"},
    // concurrency
    {"atomic_bool", "std::Atomic<bool>"}, {"atomic_size_t", "std::Atomic<size_t>"},
    {"atomic_uint8_t", "std::Atomic<u8>"}, {"atomic_uint16_t", "std::Atomic<u16>"}, {"atomic_uint32_t", "std::Atomic<u32>"},
    {"atomic_uint64_t", "std::Atomic<u64>"}, {"atomic_int8_t", "std::Atomic<i8>"}, {"atomic_int16_t", "std::Atomic<i16>"},
    {"atomic_int32_t", "std::Atomic<i32>"}, {"atomic_int64_t", "std::Atomic<i64>"}, {"atomic_load", "std::atomicLoad"},
    {"atomic_store", "std::atomicStore"}, {"atomic_exchange", "std::atomicExchange"}, {"atomic_is_lock_free", "std::atomicIsLockFree"},
    {"async", "std::async"}, {"future", "std::Future"}, {"shared_future", "std::SharedFuture"},
    {"promise", "std::Promise"}, {"future_status", "std::FutureStatus"}, {"launch", "std::Launch"},
    {"this_thread", "std::thisThread"}, {"recursive_mutex", "std::RecursiveMutex"}, {"shared_mutex", "std::SharedMutex"},
    {"shared_lock", "std::SharedLock"}, {"cv_status", "std::CvStatus"}, {"defer_lock", "std::deferLock"}, {"try_to_lock", "std::tryToLock"},
    {"adopt_lock", "std::adoptLock"}, {"defer_lock_t", "std::DeferLock"}, {"try_to_lock_t", "std::TryToLock"}, {"adopt_lock_t", "std::AdoptLock"},
    // type traits
    {"source_location", "std::CallerLocation"},
    {"type_info", "std::TypeInfo"}, {"nullopt_t", "std::NulloptType"},
    {"is_abstract_v", "std::isAbstract"}, {"is_final_v", "std::isFinal"},
    {"is_polymorphic_v", "std::isPolymorphic"}, {"is_class_v", "std::isClass"},
    {"is_move_assignable_v", "std::isMoveAssignable"},
    {"is_move_constructible_v", "std::isMoveConstructible"},
    {"is_nothrow_move_assignable_v", "std::isNothrowMoveAssignable"},
    {"is_nothrow_move_constructible_v", "std::isNothrowMoveConstructible"},
    {"is_trivially_move_assignable_v", "std::isTriviallyMoveAssignable"},
    {"is_trivially_move_constructible_v", "std::isTriviallyMoveConstructible"},
    {"is_invocable_v", "std::isInvocable"}, {"is_invocable_r_v", "std::isInvocableR"},
    {"is_copy_constructible_v", "std::isCopyConstructible"}, {"is_copy_assignable_v", "std::isCopyAssignable"},
    {"is_member_pointer_v", "std::isMemberPointer"}, {"is_member_function_pointer_v", "std::isMemberFunctionPointer"},
    {"is_member_object_pointer_v", "std::isMemberObjectPointer"},
    {"alignment_of_v", "std::alignmentOf"},
    {"add_const_t", "std::AddConst"}, {"add_lvalue_reference_t", "std::AddLvalueReference"},
    {"remove_cv", "std::RemoveCv"}, {"is_const", "std::IsConst"}, {"is_same", "std::IsSame"},
    {"make_unsigned_t", "std::MakeUnsigned"}, {"conditional", "std::Conditional"},
    {"enable_if", "std::EnableIf"}, {"void_t", "std::VoidT"},
    {"bool_constant", "std::BoolConstant"}, {"integral_constant", "std::IntegralConstant"},
    {"enable_shared_from_this", "std::EnableSharedFromThis"}, {"bad_weak_ptr", "std::BadWeakPtr"},
    {"bad_optional_access", "std::BadOptionalAccess"}, {"bad_function_call", "std::BadFunctionCall"},
    // variant helpers
    {"holds_alternative", "std::holdsAlternative"}, {"get_if", "std::getIf"},
    {"in_place_index", "std::inPlaceIndex"}, {"in_place_type_t", "std::InPlaceType"},
    {"variant_alternative_t", "std::VariantAlternative"}, {"variant_size_v", "std::variantSize"},
    // tuple helpers
    {"forward_as_tuple", "std::forwardAsTuple"}, {"tuple_size_v", "std::tupleSize"}, {"tuple_size", "std::TupleSize"}, {"tuple_element", "std::TupleElement"},
    {"piecewise_construct", "std::piecewiseConstruct"},
    // iterator tags
    {"input_iterator_tag", "std::InputIteratorTag"},
    {"forward_iterator_tag", "std::ForwardIteratorTag"},
    {"bidirectional_iterator_tag", "std::BidirectionalIteratorTag"},
    {"random_access_iterator_tag", "std::RandomAccessIteratorTag"},
    // misc
    {"greater", "std::Greater"}, {"greater_equal", "std::GreaterEqual"},
    {"as_const", "std::asConst"}, {"to_address", "std::toAddress"},
    {"mem_fn", "std::memFn"}, {"move_iterator", "std::MoveIterator"},
    {"inserter", "std::inserter"}, {"back_inserter", "std::backInserter"},
    {"make_exception_ptr", "std::makeExceptionPtr"}, {"exception_ptr", "std::ExceptionPtr"}, {"rethrow_exception", "std::rethrowException"},
    {"set_terminate", "std::setTerminate"},
    {"make_unique_for_overwrite", "std::makeUniqueForOverwrite"},
    {"basic_string_view", "std::StringView"},
    {"istreambuf_iterator", "std::IstreambufIterator"},
    {"streamsize", "std::StreamSize"},
    // I/O
    {"cerr", "std::cerr"}, {"endl", "std::endl"}, {"ifstream", "std::InFileStream"},
    {"ofstream", "std::OutFileStream"},
    // random
    {"default_random_engine", "std::DefaultRandomEngine"},
    {"random_device", "std::RandomDevice"},
    {"uniform_int_distribution", "std::UniformIntDistribution"},
    // concepts
    {"convertible_to", "std::ConvertibleTo"}, {"equality_comparable", "std::EqualityComparable"},
    // misc C++
    {"sig_atomic_t", "::sig_atomic_t"}, {"isupper", "::isupper"},
    {"ratio", "std::Ratio"}, {"nano", "std::Nano"}, {"micro", "std::Micro"}, {"milli", "std::Milli"}, {"centi", "std::Centi"}, {"deci", "std::Deci"}, {"kilo", "std::Kilo"}, {"mega", "std::Mega"},
    {"string_literals", "std::stringLiterals"},
    {"numbers", "std::Numbers"},
    {"filesystem", "std::filesystem"},
    {"error_code", "std::ErrorCode"}, {"errc", "std::Errc"},
    {"system_error", "std::SystemError"}, {"system_category", "std::systemCategory"}, {"generic_category", "std::genericCategory"},
    {"error_category", "std::ErrorCategory"}, {"make_error_code", "std::make_error_code"},
    {"length_error", "std::LengthError"}, {"overflow_error", "std::OverflowError"},
    {"codecvt_utf8", "std::CodecvtUtf8"}, {"wstring_convert", "std::WStringConvert"},
    {"milli", "std::Milli"}, {"throw_with_nested", "std::throw_with_nested"}, {"rotr", "std::rotr"}, {"lcm", "std::lcm"}};

