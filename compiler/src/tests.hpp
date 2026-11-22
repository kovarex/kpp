#pragma once

#include <string>
#include <string_view>

// Native tests (docs/design.md 8.6 and "Running and testing kpp code"). A build with `--tests` has
// the config constant `tests`; the parser then turns every `test name { }` into a function and a
// global whose initializer registers it, and the build gets one more source file, this runner: it
// is the program's main (the program's own main is renamed), runs the registered tests and
// reports their checks.
constexpr const char* testRunnerPath = "<tests>.kpp";
std::string testRunnerSource();
// What --jobs needs of the system, one file for each kind (a file's `requires` leaves out the other).
constexpr const char* testRunnerWindowsPath = "<tests-windows>.kpp";
constexpr const char* testRunnerPosixPath = "<tests-posix>.kpp";
std::string testRunnerWindowsSource();
std::string testRunnerPosixSource();

// A name as the debugger and a stack trace show it (debug information's names, Codegen/DebugInfo.cpp): the parser's
// names for a test, a fixture or a generator (`__kpp_test_Name__<hash>_<n>`, parseTest) and for a suite's
// namespace (`__kpp_suite_Suite`) are the names written, `Suite::Name()` for
// `__kpp_suite_Suite::__kpp_test_Name__2a62b8e4c1d0f9e7_3()`, a fixture's body `Suite::Name::body()`, a generator's
// `Suite::Name::generate()` (kovarex,
// 2026-11-22). Any other text is returned as it is.
std::string readableGeneratedName(std::string_view name);
// A source the driver generates with functions in it, which the debug information names as a file it writes (the test
// runner's `<tests>.kpp`, `<tests-windows>.kpp`, `--coverage`'s `<coverage>.kpp`; not `<config>`, constants alone).
inline bool isGeneratedSource(const std::string& path) { return path.size() > 5 && path[0] == '<' && path.ends_with(">.kpp"); }
// The file such a source is written to for the debugger, in directory: its name without the angle brackets,
// `tests.kpp` (Driver/ModuleCompile.cpp, Codegen/DebugInfo.cpp).
std::string generatedSourceFile(const std::string& directory, const std::string& path);
