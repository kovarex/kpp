#pragma once

#include <string>

// Native tests (docs/design.md 8.6 and "Running and testing kpp code"). A build with `--tests` has
// the config constant `tests`; the parser then turns every `test "name" { }` into a function and a
// global whose initializer registers it, and the build gets one more source file, this runner: it
// is the program's main (the program's own main is renamed), runs the registered tests and
// reports their checks.
constexpr const char* testRunnerPath = "<tests>.kpp";
std::string testRunnerSource();
