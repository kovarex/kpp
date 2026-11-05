// SourceConfiguration.inl: the configured sources: include directories, blanking, SourceConfiguration::Impl.
// Part of convert.cpp, included where it stood (a file of its own since 2026-11-05, moved without a change).

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
    // (glibc's pthread.h, which libstdc++'s <thread> brings in: SystemUtil's Linux thread names)
    {"pthread.h", "pthread_self pthread_setname_np pthread_getname_np pthread_t "},
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

