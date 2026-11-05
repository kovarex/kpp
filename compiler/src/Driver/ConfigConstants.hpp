// The driver: config:: constants: their values and declarations; the declarations, in namespace
// driver (Driver.hpp includes this), the definitions in ConfigConstants.cpp (main.cpp split, stage 2,
// 2026-11-05, moved without a change).
  // A --config value: true/false, an integer, or a word (an enumerator such as Windows, or a string).
  ConfigValue parseConfigValue(const std::string& text);


  // A string as a kpp literal's text: its quotes and backslashes escaped.
  std::string escapedString(const std::string& text);


  // The `config` namespace as kpp source, one constant per --config value; one declared in source
  // (declared) has its declared type.
  std::string configSource(const ConfigValues& values, const std::map<std::string, ConfigDeclared>& declared);


  // The config:: constants the program's .kpp files declare (config.cpp; design.md, "config:: defaults
  // in source"): config::NAME, a library's config::LIB::NAME. Each takes its --config value, which must
  // fit its type, else its default; one with no default that nothing sets is an error, and so are two
  // declarations of one name that differ and one of a name kppc sets itself. False, the errors
  // printed, when there is one.
  bool configDeclarations(const std::vector<SourceFile>& files, size_t count, const std::vector<int>& fileLibrary, Options& options,
                          std::map<std::string, ConfigDeclared>& declared);
