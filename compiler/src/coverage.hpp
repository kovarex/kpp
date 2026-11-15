#pragma once

#include <string>

// Test coverage (docs/design.md 28). A build with `--coverage` has the config constant `coverage`;
// codegen gives the program's own functions probes and module 0 a table of them
// (Codegen/CoverageProbes.hpp), and the build gets one more source file, this runtime: it reads the
// table and writes what ran, at the program's exit or when the test runner says.
constexpr const char* coverageRuntimePath = "<coverage>.kpp";
std::string coverageRuntimeSource();
