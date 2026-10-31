#pragma once
#include "server.hpp"

// A project described by build.kpp (docs/build.md, "build.kpp"): `kpp` with no sources, in the
// project's directory or below it, builds its executables (`kpp [names] [--debug|--release]
// [--run ARGS] [--set NAME=VALUE]`), writes its IDE files (`--generate-visual-studio-files`,
// `--generate-vs-code-files`), and `kpp` alone in a console asks what to do. `kpp new` makes a
// project. Returns -1 when the arguments are no project's (a build of the sources they name).
int runProject(int argc, char** argv, CompileFunction compile);
