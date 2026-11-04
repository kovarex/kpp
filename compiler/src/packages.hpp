#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Code packages (docs/design.md, "Code packages", decided 2026-11-04): every directory under a source
// root is a package, named as the directory; a root is named as its directory (the standard library's,
// the program's) or as its library. A package's README.md documents it; its first paragraph is the
// description tools show. Package names never appear in code: they are for reading and browsing.

struct ParsedFile;
class SymbolTable;

// A source file's package: its root's directory and name, and the directory under the root ("" for
// the root itself, else `a/b`).
struct PackageOf
{
  std::filesystem::path rootDir;
  std::string rootName, under;
};

// The first paragraph of the README.md in dir, the headings, pictures, HTML and code blocks before it
// skipped and its lines joined; empty without one.
std::string packageDescription(const std::filesystem::path& dir);

// `kppc --packages`: each package by its path from its root, sorted (`a`, `a/b`, `a/c`), with `: ` and
// its description when it has one, then each of its files by name with the names it declares at its
// top level: types, functions, variables and constants, namespaces. packages[i] is files[i]'s package;
// the files after packages.size() (the driver's own) are not listed.
void printPackages(const std::vector<PackageOf>& packages, const std::vector<std::filesystem::path>& files,
                   const std::vector<std::unique_ptr<ParsedFile>>& parsed, const SymbolTable& symbols);
