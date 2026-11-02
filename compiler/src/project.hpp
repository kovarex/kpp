#pragma once
#include "server.hpp"
#include <filesystem>
#include <string>
#include <vector>

// A project described by build.kpp (docs/build.md, "build.kpp"): `kpp` with no sources, in the
// project's directory or below it, builds cells of its targets x configurations matrix (`kpp
// [targets] [configurations] [--run ARGS] [--set NAME=VALUE] [--config NAME=VALUE]`), runs its
// executables' and libraries' tests (`kpp --test [names] [-- runner arguments]`), writes its IDE
// files (`--generate-visual-studio-files`,
// `--generate-vs-code-files`), and `kpp` alone in a console asks what to do. `kpp new` makes a
// project. Returns -1 when the arguments are no project's (a build of the sources they name).
int runProject(int argc, char** argv, CompileFunction compile);

// The arguments a check of the project whose build.kpp is in directory or above it (or is
// buildFile) compiles with (`kppc --lsp`, docs/ide.md and docs/editors.md): one cell, an executable
// in a configuration (by its name or its Visual Studio name: fastdebug or Fastdebug), or every
// executable in it together (their sources and exclusions with the first one's options), without
// what only code generation and the link read; args[0] is the compiler. False when there is no
// build.kpp; true with error set when it does not describe what is asked.
struct ProjectCheck
{
  std::string configuration = "debug", target, buildFile; // target empty: every target
  std::vector<std::pair<std::string, std::string>> settings; // build.kpp's settings (`kpp --set`)
};
bool projectCheckArguments(const std::filesystem::path& directory, const ProjectCheck& check, std::vector<std::string>& args, std::string& error);

// The cells a project can be checked in (the language server's picker, docs/editors.md): every
// target in every configuration, in build.kpp's order; visualStudio is the configuration's name in
// the generated Visual Studio files (`Debug`), which Visual Studio's client sends. False and true
// with error as projectCheckArguments.
// An option of a library a cell uses (docs/build.md, "Kpp libraries"): config::LIBRARY::NAME, its
// type (bool, int, string), default ("" when required), the cell's value and who set it ("the
// default", "use(\"zlib\") in ...build.kpp", "the configuration release", "the command line
// (--config)"), its description and a string option's allowed values (none: any).
struct ProjectOption
{
  std::string library, name, type, defaultValue, value, setBy, description;
  bool required = false;
  std::vector<std::string> allowed;
};
// A Kpp library a cell uses (docs/build.md, "Kpp libraries"; design.md 16): its name, its roots,
// absolute, and whether it is unscoped (its names bare everywhere) or scoped (others write NAME::x).
struct ProjectLibrary
{
  std::string name;
  std::vector<std::string> roots;
  bool unscoped = false;
  std::vector<std::string> excluded; // files under its roots that are not its sources: its build.kpp
};
struct ProjectCell
{
  std::string target, configuration, visualStudio;
  std::vector<ProjectOption> options;    // the libraries' options in this cell, library by library
  std::vector<ProjectLibrary> libraries; // the Kpp libraries it uses, in kppc's order
};
bool projectCells(const std::filesystem::path& directory, const ProjectCheck& check, std::vector<ProjectCell>& cells, std::string& error);
