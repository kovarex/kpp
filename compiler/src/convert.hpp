#pragma once
// The C++ to kpp converter: rewrites parsed and resolved C++ files into kpp sources.
// See docs/path-to-factorio.md. It does the mechanical part (preprocessor lines, forward
// declarations, `this->`, `implicit`, `fallthrough`, `std::` names) and reports what it could not
// do, so that the compiler's errors on the output are the worklist.
#include "Ast/Ast.hpp"
#include "lexer.hpp"
#include "resolver.hpp"
#include <filesystem>
#include <map>
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
  // C, not C++ (kpp --c-sources: a C library): written as kpp by C's rules, where C and C++
  // differ (programs/factorio/tests/convert_c/rules.c), with every conversion C makes implicitly
  // made explicit.
  bool c = false;
};

// C's names that are C++'s keywords (`class`, `new`, `this`, `template`...): in a C file's tokens
// they are identifiers, which the converter renames (`class_`). A use of one of keywordMacros (a
// macro whose body is one keyword) is that keyword, which the converter writes in its place.
void cKeywordsAsNames(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols,
                      const std::map<std::string, std::string>* keywordMacros = nullptr);

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
  // cSources: the inputs are C (kpp --c-sources), preprocessed as factorio-in-kpp builds a C
  // library (with overridesDir's defines.txt), and the parse sees through C's macros that are
  // nothing, one keyword or their argument (cKeywordMacros).
  SourceConfiguration(const std::vector<std::filesystem::path>& paths, const std::vector<std::string_view>& texts,
                      const std::vector<std::filesystem::path>& roots, bool cSources = false,
                      const std::filesystem::path& overridesDir = {});
  ~SourceConfiguration();
  // With cSources: the object-like macros whose body is one keyword (zutil.h's `#define local
  // static`), by name: their uses lex as that keyword (cKeywordsAsNames).
  const std::map<std::string, std::string>& cKeywordMacros() const;
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
// its free functions gets another name (kpp's top-level names are the program's and the library's).
std::string convertToKpp(const std::vector<ConvertInput>& inputs, GlobalScope& global, const SymbolTable& symbols,
                         const std::vector<std::filesystem::path>& roots, const std::filesystem::path& outDir,
                         const std::filesystem::path& overridesDir = {}, const SourceConfiguration* configuration = nullptr,
                         const std::filesystem::path& libraryDir = {}, const std::filesystem::path& baseDir = {});
// kpp --convert-library NAME DIR (design.md 16): the program uses the scoped library NAME, converted
// into DIR. Its names (the top-level declarations of DIR's .kpp files that are not private) are
// written qualified in the converted program, zstd::ZSTD_compress, and an include of its headers
// (zstd/zstd.h) imports nothing.
void setConvertLibraries(std::vector<std::pair<std::string, std::filesystem::path>> libraries);
// overridesDir's source-edits.txt (Converter/SourceEdits.inl): hand edits of the C++ inputs' texts
// (paths[i]'s text is *texts[i]), made before anything reads them. Returns the errors, empty if none.
std::string applyConvertSourceEdits(const std::filesystem::path& overridesDir, const std::vector<std::filesystem::path>& paths,
                                    const std::vector<std::string*>& texts);
// The configuration a conversion preprocesses the game as (kpp --convert-release): Release's
// defines (NDEBUG) instead of FastDebug's; and whether it marks each #if group (--convert-marks), for
// convertMerge. Set before the configuration is built.
void setConvertRelease();
// kpp --convert-final: FinalRelease's (Release's with FINAL_RELEASE, PACKAGED_BUILD and ALL_IN_ONE).
void setConvertFinal();
void setConvertMarks();
// kpp --convert-native-tests: the game's UnitTest++ tests as kpp's suites, tests and checks (design.md 8.7).
void setConvertNativeTests();
// kpp --convert-platform windows|linux: the platform the game is preprocessed for (Windows' defines
// and MSVC's predefines, or Linux's and those --convert-predefines FILE gives, clang's `-dM -E`
// output). False for another name or an unreadable file.
bool setConvertPlatform(const std::string& platform);
bool setConvertPredefines(const std::filesystem::path& file);
// kpp --convert-merge-platform WINDOWS LINUX OUT: one configuration's marked Windows and Linux
// conversions made one, each #if group whose two texts differ chosen by config::platform, its marks
// kept for convertMerge, which makes the configurations one after; a file of one platform alone
// starts `requires config::platform == Platform::X;`. Returns 1 when they differ outside a group.
int convertMergePlatform(const std::filesystem::path& windowsDir, const std::filesystem::path& linuxDir, const std::filesystem::path& outDir,
                         const std::string& firstPlatforms = "Windows", const std::string& secondPlatforms = "Linux");
// kpp --convert-this-reference DIR...: `this` as a reference (design.md 23d) in the .kpp files below each directory.
int convertThisReference(const std::vector<std::filesystem::path>& directories);
// kpp --convert-merge DEBUG RELEASE OUT: the two configurations' marked conversions made one into
// OUT, each #if group whose texts differ written as `if constexpr (config::debug)` (design.md 12).
// Returns 1 when they differ where no group can say so (reported, with the debug text written).
// choicesFile lists the groups no `if constexpr` can choose between, each taken from one
// configuration: `FILE GROUP debug|release  why` (programs/factorio/overrides/configuration-choices.txt).
// finalDir, when given, is the FinalRelease conversion (--convert-final): a group whose final text
// alone differs is written with config::finalRelease, and one whose three texts all differ is a
// problem.
int convertMerge(const std::filesystem::path& debugDir, const std::filesystem::path& releaseDir, const std::filesystem::path& outDir,
                 const std::filesystem::path& choicesFile = {}, const std::filesystem::path& finalDir = {});

// The errors the last convertToKpp found that leave its output wrong, which it printed: C, an #if
// name that nothing defines and the library's c-libraries\NAME\undefined.txt does not list. kpp
// exits 1.
int convertErrorCount();
