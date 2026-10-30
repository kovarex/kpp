#pragma once

// `kppc [options] --run SOURCE [program arguments]` (docs/design.md, "Running and testing Kpp
// code"): builds SOURCE with the options and runs it with the arguments; the exit code is the
// program's, or the compiler's when the build fails. argv[runIndex] is "--run".
int runProgram(int argc, char** argv, int runIndex);
