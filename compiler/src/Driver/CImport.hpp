// The driver: the C import: `extern "C" import` headers preprocessed by clang, their macros'
// constants, the library's functions left out; the declarations, in namespace driver (Driver.hpp
// includes this), the definitions in CImport.cpp (main.cpp split, stage 2, 2026-11-05, moved
// without a change).
  std::string readWholeFile(const std::filesystem::path& path);


  // The files a make-style dependency list (clang's -MD) names after its target: words split at
  // unescaped blanks, `\` before a newline continuing the line, `\ ` a blank in a path.
  std::vector<std::string> makeDependencies(const std::string& text);


  // "size time path" of a file, what a cached import remembers of each header it read.
  std::string fileStamp(const std::string& path);


  // Writes the stamps of the headers a dependency list names, one per line; false when there is no
  // list or a header cannot be stamped (no stamps: the import is never taken for a hit).
  bool writeHeaderStamps(const std::string& makeDeps, const std::filesystem::path& source, const std::filesystem::path& out);


  // Whether every header a cached import's stamps name still has its size and time.
  bool headersUnchanged(const std::filesystem::path& stampsPath);


  // The bodies of the object-like macros that call function-like ones (SDL3's
  // `SDL_WINDOW_FULLSCREEN SDL_UINT64_C(0x0000000000000001)`, windows.h's `E_FAIL
  // _HRESULT_TYPEDEF_(0x80004005L)`), expanded by clang: each such name on a line of its own after
  // the headers, behind a numbered marker. Name -> expansion.
  std::map<std::string, std::string> expandMacroCalls(const std::string& defines, const std::string& wrapper, const std::filesystem::path& source,
                                                      const std::string& clang, const std::string& includeArgs);


  // The names the preprocessed headers declare with typedef: in each `typedef ...;`, a name before a
  // ',', ';' or '[' outside parentheses and braces (`typedef unsigned long u_long;`, `} NAME, *PNAME;`),
  // or after a '*' and before a ')' in one level of parentheses (`typedef void (*NAME)(int);`).
  std::set<std::string> typedefNames(const std::string& text);


  // Turns `#define NAME body` lines (clang -dM output) into kpp constant declarations where the
  // body looks like a constant: numbers, string literals, operators, parentheses and names (other
  // macros', type words, and the typedefs of the headers: `(u_long)0x00000000`). A body that calls
  // function-like macros is taken as clang expanded it (expansions).
  // The names the preprocessed headers declare as functions or enumerators, which an object-like
  // macro may alias (`#define res_ninit __res_ninit`).
  std::set<std::string> valueNames(const std::string& text);
  std::string macroConstants(const std::string& defines, const std::map<std::string, std::string>& expansions, const std::set<std::string>& typedefs,
                             const std::set<std::string>& values);


  // Lexes one large text on several threads: the C import, 10 MB of preprocessed headers (the
  // Factorio build's), took 0.3 to 0.6 s on one thread, most of the import. The pieces end at
  // line ends with no continuation (no token of the preprocessed text spans one) and each is
  // lexed from a copy with its own terminator; the tokens are those of one lex of the whole.
  LexResult lexInChunks(std::string_view text, SymbolTable& symbols, unsigned threads);


  // Collects `extern "C" import "header";` declarations, preprocesses the headers with clang
  // once (cached by content hash in the temp directory) and parses the result as one more file.
  // An imported C text for a target other than Windows: its va_list (clang's __builtin_va_list) is
  // the target's. SysV x86-64's is an array of one register save area (`__va_list_tag[1]`, 24 bytes),
  // which C passes on as a pointer to it; Linux's AAPCS64's a struct of 32 bytes (__stack, __gr_top,
  // __vr_top, __gr_offs, __vr_offs); Apple's arm64's a char*, as Windows' (the resolver's
  // __builtin_va_list), which is left as it is.
  std::string vaListForSysV(const std::string& text, bool aapcs64 = false);


  // The toolchain an import is made with, for its cache key: clang's version, the target triple and
  // _MSC_VER it gives, and the directories its `#include <...>` searches. clang picks the MSVC and
  // the Windows SDK from the environment (INCLUDE and the rest under VsDevCmd, else the newest Visual
  // Studio with MSVC: installing another one changes them), and _MSC_VER from that MSVC: under 14.44's
  // (1944) the UCRT's time.h defines time() and localtime() `static __inline`, under 14.51's not
  // (corecrt.h's _STATIC_INLINE_UCRT_FUNCTIONS), from the same header files.
  std::string clangToolchain(const std::string& clang, const std::string& includeArgs);


  // Whether two types are written alike: the same builtin type, the same name (no template
  // arguments), or const, volatile and pointers to such (sameWrittenType's whole rule: any other form
  // is another type here).
  bool sameWrittenType(const Type* a, const Type* b);


  // The imported C functions that the standard library defines at global scope as functions of its
  // own, with the same name and parameter types, are left out of the import (C's decision,
  // 2026-11-05): the library's is the function a call finds. glibc's math.h declares the BSD functions
  // `int isinf(double)` and `int isnan(double)` that C99's macros replaced, beside the library's
  // `bool isinf(double)`, and every call of one was ambiguous, the library's own isfinite's included.
  // No rename and no fallback: a C function whose parameters differ, or are written another way (a
  // typedef the library does not write), stays, and an ambiguity left is the program's error. The
  // library's own `extern "C"` declarations of C functions (`extern "C" double sqrt(double)`) are the
  // same functions, no clash, and leave nothing out; nor does a library function that may not exist (a
  // `when`, or private to its file). The names left out, sorted, in names.
  size_t leaveOutLibraryFunctions(std::vector<std::unique_ptr<ParsedFile>>& parsed, ParsedFile& imported, const SymbolTable& symbols,
                                  std::vector<std::string>& names);


  void importCHeaders(std::vector<SourceFile>& files, std::vector<LexResult>& lexed, std::vector<std::unique_ptr<ParsedFile>>& parsed,
                      SymbolTable& symbols, const Options& options);
