#pragma once

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Test coverage (docs/design.md 28). A build with `--coverage` has the config constant `coverage`;
// codegen gives the program's own functions probes and module 0 a table of them
// (Codegen/CoverageProbes.hpp), and the build gets one more source file, this runtime: it reads the
// table and writes what ran, at the program's exit or when the test runner says.
constexpr const char* coverageRuntimePath = "<coverage>.kpp";
// defaultFile: where the run writes the coverage when neither the runner's --coverage nor
// KPP_COVERAGE_FILE says (--coverage-file; empty: kpp-coverage.json in the working directory).
std::string coverageRuntimeSource(const std::string& defaultFile);

// Per test (the runner's --coverage-tests): how many tests ran a place and the lowest-numbered of them,
// as indexes into Coverage::tests.
struct CoverageTests
{
  long long count = 0;
  std::vector<int> tests;
};

struct CoveragePlace
{
  int hit = 0;
  CoverageTests ran;
};

struct CoverageCondition
{
  int whenTrue = 0, whenFalse = 0;
  CoverageTests ranTrue, ranFalse;
};

// A coverage file's line for one source file (the runtime's format, coverage.cpp), read back: each
// place by its numbers (line, column, last line, the column after).
struct CoverageFile
{
  std::string hash;
  std::map<std::array<int, 4>, CoveragePlace> statements;
  std::map<std::array<int, 4>, CoverageCondition> conditions;
  std::map<std::array<int, 2>, CoveragePlace> defaults; // a switch's missing default, at its `switch`
  std::map<std::pair<int, std::string>, CoveragePlace> functions; // by line and name
};

struct Coverage
{
  std::map<std::string, CoverageFile> files; // by the file's path as the program names it
  // Per test: the tests' names (the first file's order, then each next file's new ones), and how many
  // a place names at most (the largest limit read).
  bool perTest = false;
  std::vector<std::string> tests;
  std::map<std::string, int> testIndex;
  size_t limit = 0;
};

// Reads a coverage file into `into`, each place's outcomes added to what is there (per test: the counts
// added up, the tests named by name, the lowest-numbered kept). False with
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
