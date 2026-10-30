#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct SourceFile
{
  std::filesystem::path path;
  std::string text; // Always followed by a '\0' sentinel so the lexer needs no bounds checks.
};

// Recursively collects files with the given extensions under root.
std::vector<std::filesystem::path> collectFiles(const std::filesystem::path& root,
                                                const std::vector<std::string>& extensions);

bool readFile(const std::filesystem::path& path, std::string& out);

// Where a diagnostic is: `path:line`, or with --msvc-diagnostics `path(line)`, the form Visual
// Studio's Error List reads (docs/build.md, "Visual Studio"). Every diagnostic's place is made here.
extern bool msvcDiagnostics;
std::string sourceLocation(const std::string& path, size_t line);
