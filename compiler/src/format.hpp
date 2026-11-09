#pragma once
// kpp format (docs/format.md): lays out kpp source in kpp's style, changing whitespace only. The
// compiler's lexer and the parser's format mode (ParsedFile::formatRoles) give every token its role;
// the output is lexed again and must have the input's tokens and comments, else nothing is written.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct FormatChange
{
  uint8_t rule;        // the rule that changed it (format.cpp's KPP_FORMAT_RULES, docs/format.md's numbers)
  uint32_t line;       // the input's line, 1-based
  uint32_t outputLine; // the output's line, 1-based
};

struct FormatOutcome
{
  bool formatted = false; // false: the file is left as it is, error says why
  std::string error;
  std::string text;                  // the formatted text
  std::vector<FormatChange> changes; // one per changed whitespace gap (or more where several rules change it)
  uint32_t unclassified = 0;         // gaps kept as written because no rule covers them
  std::vector<uint32_t> unclassifiedLines;
  std::vector<std::string> unclassifiedPairs; // the two tokens around each of unclassifiedLines
};

// A style: one value for each of the 13 settings (docs/format.md, "Settings"), kpp's default as constructed.
struct FormatStyle
{
  enum class Pointer : uint8_t { Type, Name, Both };
  int indent = 2;          // indent: 2, 4, tab (indentTabs, 4 columns each)
  bool indentTabs = false;
  int columns = 130;       // columns: 130, 100, 80, 160 (0, --columns 0 only: no wrapping)
  bool bracesEndOfLine = false;
  bool caseIndent = true;
  bool namespaceIndent = true;
  bool accessAtMembers = false; // access-label member
  bool wrapBeforeOperator = false; // wrap-operator start
  int continuation = 0;    // continuation: align (0), 4, 8
  Pointer pointer = Pointer::Type;
  bool bracedListSpaces = false;
  bool templateSpace = false;
  bool trailingCommas = false; // initializer-commas trailing
  int blankLinesAroundDefinitions = 1; // blank-lines-between-functions: 1, 2
};

// Reads a .kppstyle's `key = value` lines into style; false with error (naming the line and the
// allowed values) for an unknown setting, an unlisted value or a setting given twice.
bool readFormatStyle(std::string_view text, std::string_view path, FormatStyle& style, std::string& error);

FormatOutcome formatText(std::string_view path, const std::string& text, const FormatStyle& style);

// The text with the whitespace between its tokens moved where the formatter decides it (spaces, tabs
// and line breaks), keeping its tokens and the facts it keeps (run_format.py's perturbed copies).
// Empty error: done.
std::string perturbText(std::string_view path, const std::string& text, uint32_t seed, std::string& error);

// `kpp format ...` (main.cpp; docs/format.md, Commands).
int runFormat(int argc, char** argv);
