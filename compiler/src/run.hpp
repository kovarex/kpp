#pragma once

// `kppc [options] --run SOURCE [program arguments]` (docs/design.md, "Running and testing kpp
// code"): builds SOURCE with the options and runs it with the arguments; the exit code is the
// program's, or the compiler's when the build fails. argv[runIndex] is "--run", or "--test":
// `kppc [options] --test SOURCE... [-- runner arguments]` builds the sources with --tests and runs
// the test runner (tests.hpp) with the arguments after `--`.
int runProgram(int argc, char** argv, int runIndex);
