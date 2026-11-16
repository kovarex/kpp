#pragma once

#include <array>
#include <map>
#include <string>
#include <utility>

// Test coverage (docs/design.md 28). A build with `--coverage` has the config constant `coverage`;
// codegen gives the program's own functions probes and module 0 a table of them
// (Codegen/CoverageProbes.hpp), and the build gets one more source file, this runtime: it reads the
// table and writes what ran, at the program's exit or when the test runner says.
constexpr const char* coverageRuntimePath = "<coverage>.kpp";
// defaultFile: where the run writes the coverage when neither the runner's --coverage nor
// KPP_COVERAGE_FILE says (--coverage-file; empty: kpp-coverage.json in the working directory).
std::string coverageRuntimeSource(const std::string& defaultFile);

// A coverage file's line for one source file (the runtime's format, coverage.cpp), read back: each
// place by its numbers (line, column, last line, the column after).
struct CoverageFile
{
  std::string hash;
  std::map<std::array<int, 4>, int> statements; // 1 ran
  std::map<std::array<int, 4>, int> conditions; // 1 it was true, 2 it was false
  std::map<std::array<int, 2>, int> defaults;   // a switch's missing default, at its `switch`: 1 taken
  std::map<std::pair<int, std::string>, int> functions; // by line and name: 1 called
};
using Coverage = std::map<std::string, CoverageFile>; // by the file's path as the program names it

// Reads a coverage file into `into`, each place's outcomes added to what is there. False with
// `error` when it cannot be read or is not one, or names a file with another hash than `into` has
// for it (two versions of the source cannot be merged).
bool readCoverage(const std::string& path, Coverage& into, std::string& error);
// Each line of a file's statements: 1 every one on it ran, 0 none, 2 some.
std::map<int, int> coverageLines(const CoverageFile& file);
// The coverage file's text (the runtime's format) and its summary line.
std::string coverageText(const Coverage& coverage, std::string& summary);
// lcov's tracefile: SF, FN/FNDA, DA, BRDA (a condition's true and false, a missing default), end_of_record.
std::string coverageLcov(const Coverage& coverage);

// `kpp --coverage-merge FILE... -o OUT` and `kpp --coverage-export lcov FILE... -o OUT` (- for stdout).
bool isCoverageCommand(const char* argument);
int coverageCommand(int argc, char** argv);
