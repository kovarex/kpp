#pragma once
#include "server.hpp"
#include <filesystem>
#include <string>
#include <vector>

// A project described by build.kpp (docs/build.md, "build.kpp"): `kpp` with no sources, in the
// project's directory or below it, builds its executables (`kpp [names] [--debug|--release]
// [--run ARGS] [--set NAME=VALUE]`), runs its executables' and libraries' tests (`kpp --test [names]
// [-- runner arguments]`), writes its IDE files (`--generate-visual-studio-files`,
// `--generate-vs-code-files`), and `kpp` alone in a console asks what to do. `kpp new` makes a
// project. Returns -1 when the arguments are no project's (a build of the sources they name).
int runProject(int argc, char** argv, CompileFunction compile);

// The arguments a check of the project whose build.kpp is in directory or above it (or is
// buildFile) compiles with (`kppc --lsp`, docs/ide.md and docs/editors.md): one configuration of one
// executable, or of all of them together (their sources and exclusions with the first one's options),
// without what only code generation and the link read; args[0] is the compiler. False when there is
// no build.kpp; true with error set when it does not describe what is asked.
struct ProjectCheck
{
  std::string configuration = "debug", executable, buildFile;
  std::vector<std::pair<std::string, std::string>> settings; // build.kpp's settings (`kpp --set`)
};
bool projectCheckArguments(const std::filesystem::path& directory, const ProjectCheck& check, std::vector<std::string>& args, std::string& error);
