// The driver: parsing and resolving a build, and their reports; the declarations, in namespace
// driver (Driver.hpp includes this), the definitions in FrontEnd.cpp (main.cpp split, stage 2,
// 2026-11-05, moved without a change).
  // A config:: constant of an enumeration type (config.cpp takes any other name for one: `config Mode
  // m = Mode::Fast;`): when the parsed program declares its type as a class, or as an enumeration
  // without that enumerator, an error at its declaration, not at <config>'s constant. A type it does
  // not find (an alias, a template's) is the resolver's. The number of errors.
  size_t checkConfigEnumerations(Build& build);


  void runResolve(Build& build);
  void reportParse(Build& build, double parseTime);
  void reportResolve(Build& build);


  uint32_t lineOf(const std::string& text, uint32_t offset);


  // A byte offset's column, from 1 (--diagnostic-columns).
  uint32_t columnOf(const std::string& text, uint32_t offset);


  // --c-type-warnings: C's names of the integer types (int, unsigned, short, long, long long, signed
  // and unsigned char, int32_t, size_t ...) are for C interop only (design.md, "Integer types and
  // sizes"): each one written outside an extern "C" declaration, the C headers and the standard
  // library is a warning naming kpp's type (u32, i64 ...). No error: the exit code stays.
  void warnCTypeNames(const std::vector<SourceFile>& files, const std::vector<LexResult>& lexed, const std::vector<std::unique_ptr<ParsedFile>>& parsed);


  void runParse(Build& build);


  // The parse statistics and the parse errors.
  void reportParse(Build& build, double parseTime);


  void runResolve(Build& build);


  // The resolve's statistics, then codegen (or the conversion), and the build is done.
  void reportResolve(Build& build);


  double seconds(std::chrono::steady_clock::duration d);


  void reportLex(size_t fileCount, const FileStats& total, size_t symbolCount, unsigned threads, double collectTime, double lexTime);
