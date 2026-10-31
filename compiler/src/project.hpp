#pragma once
#include "server.hpp"
#include <filesystem>
#include <string>
#include <vector>

// A project described by build.kpp (docs/build.md, "build.kpp"): `kpp` with no sources, in the
// project's directory or below it, builds its executables (`kpp [names] [--debug|--release]
// [--run ARGS] [--set NAME=VALUE]`), writes its IDE files (`--generate-visual-studio-files`,
// `--generate-vs-code-files`), and `kpp` alone in a console asks what to do. `kpp new` makes a
// project. Returns -1 when the arguments are no project's (a build of the sources they name).
int runProject(int argc, char** argv, CompileFunction compile);

// The arguments a check of the project whose build.kpp is in directory or above it compiles with
// (`kppc --lsp`, docs/ide.md): the debug configuration of build.kpp's first executable, without
// what only code generation and the link read; args[0] is the compiler. False when there is no
// build.kpp; true with error set when it describes no executable.
bool projectCheckArguments(const std::filesystem::path& directory, std::vector<std::string>& args, std::string& error);
