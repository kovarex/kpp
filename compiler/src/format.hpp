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

// columns 0: no wrapping (each line break inside a statement stays where it is).
FormatOutcome formatText(std::string_view path, const std::string& text, int columns);

// `kpp format [--check] [--stats] [--examples N] [--stdout] PATH...` (main.cpp).
int runFormat(int argc, char** argv);
