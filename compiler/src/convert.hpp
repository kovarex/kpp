#pragma once
// The C++ to Kpp converter: rewrites parsed and resolved C++ files into Kpp sources.
// See docs/path-to-factorio.md. It does the mechanical part (preprocessor lines, forward
// declarations, `this->`, `implicit`, `fallthrough`, `std::` names) and reports what it could not
// do, so that the compiler's errors on the output are the worklist.
#include "ast.hpp"
#include "lexer.hpp"
#include "resolver.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ConvertInput
{
  std::filesystem::path path;
  std::string_view text;
  const std::vector<Token>* tokens;
  ParsedFile* parsed;
  // C, not C++ (kppc --c-sources: a C library): written as Kpp by C's rules, where C and C++
  // differ (programs/factorio/tests/convert_c/rules.c), with every conversion C makes implicitly
  // made explicit.
  bool c = false;
};

// C's names that are C++'s keywords (`class`, `new`, `this`, `template`...): in a C file's tokens
// they are identifiers, which the converter renames (`class_`).
void cKeywordsAsNames(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols);

// Writes one .kpp file per input (a header and its source of the same stem merged into one)
// under outDir, mirroring the paths below `roots`. A file of the same relative path under
// The preprocessor configuration of a conversion: which #if branches of each C++ file stay, as
// the game's FastDebug build selects them (convert.cpp, "the preprocessor configuration"), with the
// object-like macros a file defines and those the configured headers it includes leave defined.
// Built once from all the C++ inputs; the front end lexes the configured texts and the converter
// writes out the same branches.
class SourceConfiguration
{
public:
  SourceConfiguration(const std::vector<std::filesystem::path>& paths, const std::vector<std::string_view>& texts,
                      const std::vector<std::filesystem::path>& roots);
  ~SourceConfiguration();
  // File i's text with the #if branches the configuration drops, and every conditional directive
  // line, blanked (newlines and offsets kept), so that the front end parses the branches that are
  // written out.
  const std::string& configuredText(size_t i) const;

  struct Impl;
  std::unique_ptr<Impl> impl;
};

// overridesDir replaces the generated one: the hand-written conversion of what the tool cannot
// do. `configuration` is the one the front end lexed the inputs with. Returns the conversion report.
// libraryDir is the standard library's directory: a source file's static function named as one of
// its free functions gets another name (Kpp's top-level names are the program's and the library's).
std::string convertToKpp(const std::vector<ConvertInput>& inputs, GlobalScope& global, const SymbolTable& symbols,
                         const std::vector<std::filesystem::path>& roots, const std::filesystem::path& outDir,
                         const std::filesystem::path& overridesDir = {}, const SourceConfiguration* configuration = nullptr,
                         const std::filesystem::path& libraryDir = {}, const std::filesystem::path& baseDir = {});
