#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Project files for an IDE (docs/build.md, "Project files for an IDE"):
// `kppc --generate-visual-studio-files <build options> <sources> --exe OUT` writes a solution and a
// Makefile project into build\visual-studio\, `--generate-vs-code-files ...` writes .vscode\
// (tasks, launch, settings). Both list every source file and build with the same arguments,
// debug and release, through the compile server. A project with a build.kpp (project.hpp) writes
// them for all its executables at once, through writeIdeFiles.

bool isGenerateOption(const char* arg);
int generateProjectFiles(int argc, char** argv);

// kppc's options that take a value (main.cpp's compile), so that the value is not read as a source.
bool takesValue(const std::string& arg);
// An argument as cmd.exe needs it: quoted when it has a blank or a character cmd reads.
std::string quoteArg(const std::string& arg);

// One configuration of a program in the project files: how it is built (a cmd.exe command line,
// run in no particular directory) and what the debugger starts.
struct IdeConfiguration
{
  std::string visualStudioName; // "Debug"
  std::string name;             // "debug"
  std::filesystem::path exe;
  std::string buildCommand;
  std::filesystem::path program;
  std::string arguments;
  std::filesystem::path directory;
};

// One program: the sources listed (roots, absolute, without the excluded files) and its
// configurations.
struct IdeTarget
{
  std::string name;
  std::vector<std::filesystem::path> roots;
  std::vector<std::filesystem::path> excluded;
  std::vector<IdeConfiguration> configurations;
};

// Writes base\build\visual-studio\<solution>.sln with one project per target, and/or base\.vscode.
// stdlib (empty: none) is listed as the folder Standard Library.
bool writeIdeFiles(bool visualStudio, bool vsCode, const std::string& solution, const std::filesystem::path& base,
                   const std::vector<IdeTarget>& targets, const std::filesystem::path& stdlib,
                   const std::vector<std::string>& extensions);
