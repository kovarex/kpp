#pragma once
// What project.cpp's parts share (kpp's projects: build.kpp, its description, the builds of its cells;
// docs/build.md): their types and functions, in namespace projects. Moved out of project.cpp on
// 2026-11-05 without a change (thread J's refactoring list), each part a file of this directory.
#include "project.hpp"
#include "generate.hpp"
#include "run.hpp"
#include "source.hpp"
#include "target.hpp"
#include "config.hpp"
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <io.h>
#include <string>
#include <vector>
#include <windows.h>

namespace projects
{
  // When kpp started (runProject), for KPP_STEP_TIMES' line (buildProgram).
  extern std::chrono::steady_clock::time_point kppStarted;
  extern double describedAt, usesResolvedAt; // (seconds after kppStarted: build.kpp described, its libraries too)

  namespace fs = std::filesystem;

  // A kpp library used (Project.kpp's Dependency): its lowest version ("" for any) and its directory
  // ("" for libraries/NAME), relative to the directory of the build.kpp that uses it.
  struct Use
  {
    std::string name, minimum, path;
    std::vector<std::string> configs; // its options' values given here (use(...).config): NAME=VALUE
  };

  // An option a kpp library declares (Project.kpp's LibraryOption).
  struct Option
  {
    std::string name, type, defaultValue, description;
    bool required = false;
    std::vector<std::string> allowed;
    std::string declaredType, declaredAt; // one declared in the library's source (config TYPE NAME): its type and where
  };

  // One library found for a program: where it is, which build.kpp asked for it first, its options and
  // the values its users' use() gave them.
  struct Found
  {
    std::string name;
    std::filesystem::path directory;
    std::string by;
    std::vector<Option> options;
    std::vector<std::pair<std::string, std::string>> configs; // NAME=VALUE, and which use() in which build.kpp
  };

  // A named configuration (docs/build.md, "Targets and configurations"), its bases' settings in and the
  // project's constants and arguments first: what a library's tests and a check without a target
  // are built with.
  struct Configuration
  {
    std::string name;
    bool optimize = false, debugInfo = false;
    std::vector<std::string> configs, clangFlags, arguments;
    std::vector<std::string> cFlags; // its C libraries' compiles, after kpp's own flags (build.kpp's cFlag)
    std::vector<std::string> configFrom; // per config, who set it: project, configuration, target, cell, option
  };

  // One cell, an executable in a configuration, as build.kpp described it (projectlib\Project.kpp's
  // describe, configure(cell) applied). After resolveUses, kppLibraries holds the libraries it uses,
  // each with its roots, absolute, and its scope.
  struct Program : Configuration
  {
    std::string configuration;
    std::vector<std::string> roots, excluded, libraries, cIncludes, links;
    std::vector<std::string> systemLibraries; // NAME or PLATFORM NAME: an executable's own (Executable::systemLibrary), a library's when its tests are built (libraryProgram)
    std::string beforeBuild, afterBuild, debugCommand, debugArguments, debugDirectory;
    std::string platform; // the platform configure(cell) saw (Cell::platform): windows, linux, freebsd
    std::vector<Use> uses;
    std::vector<ProjectOption> options; // after resolveUses: its libraries' options, with their values
    std::vector<ProjectLibrary> kppLibraries; // after resolveUses: the kpp libraries it uses
  };

  // A kpp library the project is (Project.kpp's Library). After resolveUses, kppLibraries holds the
  // libraries it uses.
  struct KppLibrary
  {
    std::string name, version, license, minimumKpp;
    bool unscoped = false; // its names bare everywhere (design.md 16)
    std::vector<std::string> roots, testRoots;
    std::vector<std::string> systemLibraries; // NAME or PLATFORM NAME (Library::systemLibrary)
    std::vector<ProjectLibrary> kppLibraries;
    std::vector<Use> uses;
    std::vector<Option> options;
    std::vector<Found> used; // after resolveUses: itself and the libraries it uses (applyOptions)
  };

  // A C or C++ library of the project (Project.kpp's CLibrary).
  struct CLibrary
  {
    std::string name, language;
    std::vector<std::string> sources, excluded, includes, defines, prebuilt, resources;
  };

  // What `kpp measure` times (Project.kpp's Measure): the cell, build.kpp's edits, and the commands of
  // another build to compare with, by row.
  struct Measurement
  {
    struct Edit { std::string row, file, from, to; };
    struct Compare { std::string row, command, touch; };

    std::string target, configuration;
    std::vector<Edit> edits;
    std::vector<Compare> compares;
  };

  // build.kpp's matrix: its configurations, then its cells, target by target.
  struct Description
  {
    std::vector<Configuration> configurations;
    std::vector<Program> programs;
    std::vector<CLibrary> cLibraries;
    std::vector<KppLibrary> libraries;
    Measurement measurement;
    std::vector<std::string> errors;
  };

  struct Request
  {
    std::vector<std::string> names;          // the targets and configurations asked for (act sorts them)
    std::vector<std::string> configurations; // --debug, --release
    std::vector<std::pair<std::string, std::string>> settings;
    std::vector<std::string> passed;         // kppc's own options, given to every build
    bool run = false, test = false, visualStudio = false, vsCode = false, server = true, msvc = false, stop = false;
    bool verbose = false; // --verbose: kppc's report of each build (its phases' times), not --quiet
    bool options = false; // kpp options: list the libraries' options of the cells asked for
    std::vector<std::string> runArguments;   // --run's: the program's; --test's: the runner's
  };

  // The config declarations of files (findConfigDeclarations), each kept in cache (a file of
  // describe's directory) with the file's stamp, its time and size as the listing gives them: a file
  // whose stamp is the cache's is not read again. Reading a library's every file on every build took
  // 25 ms of each edit build of the game, which uses six scoped libraries (2026-11-05). A declaration
  // with a tab or a line end in a field is not kept (its file is read each time).
  struct FileDeclarations
  {
    std::vector<ConfigDeclaration> declared;
    std::vector<ConfigError> errors;
  };

  // The platform a program's C libraries are built for, its target's (build.kpp's and the command
  // line's --target): windows (clang-cl, or the bundled toolchain's GNU dialect), or linux and freebsd
  // (clang for the target's triple, with the target's sysroot: --sysroot, else kppc's own beside it).
  struct LibraryTarget
  {
    bool posix() const { return platform != "windows"; }

    std::string platform = "windows"; // windows, linux, freebsd: platformNames' short names
    std::string triple;               // clang's --target, off Windows
    std::string sysroot;              // off Windows; empty when there is none
  };

  // A C library's part of a build (prepareLibrary): its objects' compile commands and its resources',
  // which run in one pool with every other library's of the program (runLibraries), alongside kppc's
  // own work, and then its archive (finishLibrary). Only the objects whose source, headers or flags
  // changed are compiled; prebuilt libraries need nothing. Errors are clang's, at their file and line.
  struct LibraryJob
  {
    const CLibrary* library = nullptr;
    fs::path directory, lib;
    std::vector<std::string> commands;    // the objects' compiles, then the resources' (llvm-rc)
    size_t compiles = 0;                  // how many of commands are the objects'
    std::vector<fs::path> objects, logs, scripts; // scripts: the resources', by command after the compiles
    std::vector<fs::path> resources;      // the .res files those commands write, in the same order
    std::string stamp;                    // what the objects are built with (flags.txt)
    bool gnu = false, archive = false;    // archive: the objects changed, or the library is not there
    std::unique_ptr<std::atomic<bool>[]> failed; // by command
  };

  // What Run and the debugger start (Executable::debug): the program, its arguments and its directory,
  // {exe} and {configuration} expanded as in the build steps (a configuration's own copy of the game,
  // bin\Releasex64\factorio.exe).
  struct Launch
  {
    fs::path program, directory;
    std::string arguments;
  };

  // The functions, by the file they are in.
  // Describe.cpp
  fs::path findProject(fs::path directory);
  std::string kppString(const std::string& text);
  uint64_t hashText(uint64_t h, const std::string& text);
  std::string readText(const fs::path& path);
  std::vector<FileDeclarations> configDeclarationsOf(const std::vector<fs::path>& files, const std::vector<FileStamp>& stamps,
                                                     const fs::path& cache);
  std::string settingsSource(const fs::path& root, const Request& request);
  const Configuration* findConfiguration(const Description& d, const std::string& name);
  const Program* findCell(const Description& d, const std::string& target, const std::string& configuration);
  std::vector<std::string> targetNames(const Description& d);
  int runAndRead(const std::string& command, std::string& out);
  // A description's cache: cache, else root's build\kpp (describe()).
  fs::path descriptionCache(const fs::path& root, const fs::path& cache);
  std::vector<std::string> describeArguments(const fs::path& root, const Request& request, const fs::path& cache = {});
  std::string describeCommand(const fs::path& root, const Request& request, const fs::path& cache = {});
  bool describe(const fs::path& root, const Request& request, Description& out, const fs::path& cache = {});
  bool writeText(const fs::path& path, const std::string& text, const char* what);
  fs::path rooted(const fs::path& root, const std::string& path);
  bool versionNumbers(const std::string& text, std::vector<unsigned long>& numbers);
  bool resolveUses(const fs::path& project, const fs::path& by, const std::vector<Use>& uses, const Request& request,
                   std::vector<Found>& found, std::vector<ProjectLibrary>& libraries);
  std::string setBy(const std::string& from, const Program& p);
  bool applyOptions(Program& p, const std::vector<Found>& used, const std::vector<std::string>& allUsed, const Request& request,
                    std::string& error);
  bool resolveUses(const fs::path& root, const Request& request, Description& d);
  const CLibrary* findLibrary(const Description& d, const std::string& name);
  bool libraryProgram(const fs::path& root, const Configuration& c, const KppLibrary& l, const Request& request, Program& p);
  // DescribeFile.cpp
  Description parse(const std::string& text);
  // CLibraryBuild.cpp
  LibraryTarget libraryTarget(const Program& p, const Request& request);
  std::vector<std::string> forPlatform(const std::vector<std::string>& entries, const std::string& platform);
  fs::path clibDirectory(const fs::path& root, const std::vector<std::string>& cFlags, const std::string& platform = "windows");
  fs::path libraryDirectory(const fs::path& root, const CLibrary& c, const std::vector<std::string>& cFlags, const std::string& platform = "windows");
  std::vector<fs::path> libraryFiles(const fs::path& root, const CLibrary& c, const std::vector<std::string>& cFlags,
                                     const std::string& platform = "windows");
  std::vector<std::string> cppRuntimeLibraries(const fs::path& root, const CLibrary& c, const std::string& platform);
  fs::file_time_type modified(const fs::path& path);
  std::vector<fs::path> resourceInputs(const fs::path& script);
  bool upToDate(const fs::path& object, const fs::path& dependencies, std::unordered_map<std::string, fs::file_time_type>& times);
  bool prepareLibrary(const fs::path& root, const CLibrary& c, const std::vector<std::string>& cFlags, const std::string& prefixMap, LibraryJob& job,
                      std::unordered_map<std::string, fs::file_time_type>& times, const LibraryTarget& target = {});
  bool runLibraries(std::vector<LibraryJob>& jobs);
  // Build.cpp
  int invoke(std::vector<std::string> args, bool server, CompileFunction compile, bool stop = false);
  std::string withExe(std::string command, const fs::path& exe, const std::string& configuration);
  std::string libraryPrefixMap(const fs::path& root, const Program& p, bool gnuDriver);
  std::string toolchainOf(const Program& p, const Request& request);
  std::string targetPlatform(const Program& p, const Request& request);
  fs::path exeOf(const fs::path& root, const Program& p, const Request& request);
  std::vector<std::string> buildArguments(const fs::path& root, const Description& d, const Program& p, const Request& request);
  std::vector<std::string> buildCommand(const fs::path& root, const Description& d, const Program& p, const Request& request);
  int buildProgram(const fs::path& root, const Description& d, const Program& p, const Request& request, CompileFunction compile);
  Launch launchOf(const fs::path& root, const Program& p, const Request& request);
  int runProgram(const fs::path& root, const Program& p, const Request& request, const std::vector<std::string>& extra);
  int runTests(const fs::path& root, const Description& d, const Configuration& configuration, const std::vector<std::string>& targets,
               const Request& request);
  int act(const fs::path& root, const Request& request, CompileFunction compile);
  // IdeProjects.cpp
  std::string visualStudioName(std::string name);
  int generate(const fs::path& root, const Description& d, const Request& request);
  // Measure.cpp
  int measureProject(const fs::path& root, int argc, char** argv);
  // Menu.cpp
  int newProject(const fs::path& directory);
  std::string ask(const char* prompt);
  bool interactiveInput();
  int menu(const fs::path& root, CompileFunction compile, bool asking);
}
