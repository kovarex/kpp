// kpp: the kpp compiler. Currently the speed-test prototype: parallel lexing of a file tree.
#include "Driver/Driver.hpp"

// The driver is namespace driver's (Driver/, translation units of their own since the main.cpp split's
// stage 2): this file keeps the kept-state functions, compile's options table and main().
using namespace driver;

int compile(int argc, char** argv);

bool withKeptProgram(const std::function<void(const KeptProgram&)>& f)
{
  std::lock_guard lock(keptMutex);
  if (!keptBuild || !keptBuild->global)
    return false;
  KeptProgram program;
  for (const std::unique_ptr<ParsedFile>& p : keptBuild->parsed)
    program.files.push_back(p.get());
  program.symbols = keptBuild->symbols.get();
  program.global = keptBuild->global.get();
  f(program);
  return true;
}

struct KeptState
{
  std::unique_ptr<Build> build;
  std::string importStamps; // lastImportStamps, which tell its next build whether the C headers changed
};

std::shared_ptr<KeptState> takeKeptState()
{
  std::lock_guard lock(keptMutex);
  auto state = std::make_shared<KeptState>();
  state->build = std::move(keptBuild);
  state->importStamps = std::move(lastImportStamps);
  lastImportStamps.clear();
  return state;
}

void putKeptState(std::shared_ptr<KeptState> state)
{
  std::unique_ptr<Build> previous;
  {
    std::lock_guard lock(keptMutex);
    previous = std::move(keptBuild);
    keptBuild = state ? std::move(state->build) : nullptr;
    lastImportStamps = state ? std::move(state->importStamps) : std::string();
  }
}

// compile's options and how many values each takes, which a project's command line passes on to it
// (Project/Project.cpp); compile's own loop below reads them. An option compile reads and this table lacks is
// an error in a project's build.
namespace
{
  const std::pair<const char*, int> compileOptions[] = {
    {"-j", 1}, {"--ext", 1}, {"--repeat", 1}, {"--dump", 0}, {"--lex-only", 0}, {"--errors", 1}, {"--unresolved", 1},
    {"--why", 1}, {"--why-at", 1}, {"--access-warnings", 0}, {"--return-copy-warnings", 0}, {"--zero-null-warnings", 0}, {"--override-warnings", 0}, {"--this-warnings", 0}, {"--this-reference-warnings", 0}, {"--plain-enum-warnings", 0}, {"--explicit-by-default", 0}, {"--no-implicit-fallthrough", 0}, {"--struct-rule", 0}, {"--c-type-warnings", 0},
    {"--quiet", 0}, {"--msvc-diagnostics", 0}, {"--diagnostic-columns", 0}, {"--check", 0}, {"--packages", 0}, {"--emit-llvm", 1},
    {"--exe", 1}, {"--run-build", 1}, {"--std", 1}, {"--no-std", 0}, {"--cinclude", 1}, {"--incremental-link", 0}, {"--defer-link", 0},
    {"--toolchain", 1}, {"--debug-info", 0}, {"--optimize", 0}, {"--no-optimize", 0}, {"--target", 1},
    {"--sysroot", 1}, {"--keep-going", 0}, {"--stub-missing", 0}, {"--no-inline", 0}, {"--clang-flags", 1}, {"--lto", 1}, {"--lto-partitions", 1},
    {"--opt-level", 1},
    {"--codegen-threads", 1}, {"--lib", 1}, {"--keep-symbol", 1}, {"--exclude", 1}, {"--file-prefix-map", 1},{"--library-root", 2}, {"--unscoped-library-root", 2},
    {"--config", 1}, {"--release", 0}, {"--tests", 0}, {"--modules", 1}, {"--convert", 1},
    {"--verify-incremental", 0}, {"--overrides", 1}, {"--convert-base", 1}, {"--c-sources", 0}, {"--convert-library", 2},
    {"--convert-release", 0}, {"--convert-final", 0}, {"--convert-marks", 0}, {"--convert-native-tests", 0}, {"--convert-platform", 1}, {"--convert-predefines", 1},
    {"--no-run", 0}};
}

int compileOptionValues(const std::string& arg)
{
  for (const auto& [name, values] : compileOptions)
    if (arg == name)
      return values;
  return -1;
}

int optionError(const std::string& arg)
{
  int values = compileOptionValues(arg);
  if (arg == "--version" || arg == "--help")
    fprintf(stderr, "error: %s takes no other arguments\n", arg.c_str());
  else if (values > 0)
    fprintf(stderr, "error: %s needs %s\n", arg.c_str(), values == 1 ? "a value" : "two values");
  else
    fprintf(stderr, "error: unknown option %s\n", arg.c_str());
  fprintf(stderr, "kpp --help lists the options\n");
  return 2;
}

static int compilerMain(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0); // progress lines survive a crash
  std::set_terminate([]
  {
    try
    {
      if (auto e = std::current_exception())
        std::rethrow_exception(e);
    }
    catch (const std::exception& e) { fprintf(stderr, "fatal: %s\n", e.what()); }
    catch (...) { fprintf(stderr, "fatal: unknown exception\n"); }
    abort();
  });
  // argv[0] as the executable's full path, also when found on PATH: a compile server, which runs
  // from a copy of kpp, finds the standard library next to the client's (source.hpp).
  static std::string self = compilerPath().string();
  if (argc > 0 && !(argc == 5 && std::string(argv[1]) == "--serve"))
    argv[0] = self.data();
  // The compile server (server.hpp): `--server` anywhere makes this a client, `--stop-server`
  // stops the server of the same arguments, `--serve KEY ORIGIN STAMP` is the server a client
  // started, `--stop-all-servers` alone stops every server (in a project's directory too).
  if (argc == 5 && std::string(argv[1]) == "--serve")
    return runServer(argv[2], argv[3], argv[4], compile);
  if (argc == 2 && std::string(argv[1]) == "--stop-all-servers")
    return stopAllServers();
  // `--version` and `--help` alone, in a project's directory too; with other arguments, an error
  // (optionError).
  if (argc == 2 && std::string(argv[1]) == "--version")
    return printVersion();
  if (argc == 2 && std::string(argv[1]) == "--help")
    return usage(stdout), 0;
  // The game's two configurations' conversions made one (convert.hpp): `kpp --convert-merge DEBUG
  // RELEASE OUT`.
  if ((argc >= 5 && argc <= 7) && std::string(argv[1]) == "--convert-merge")
    return convertMerge(argv[2], argv[3], argv[4], argc >= 6 ? argv[5] : "", argc == 7 ? argv[6] : "");
  // One configuration's Windows and Linux conversions made one (convert.hpp): `kpp
  // --convert-merge-platform WINDOWS LINUX OUT`.
  // `... FIRST SECOND OUT LINUX FREEBSD`: other platforms than Windows and Linux, each a comma list
  // (`Windows` and `Linux,FreeBSD`: Linux's and FreeBSD's merge against Windows').
  if (argc == 5 && std::string(argv[1]) == "--convert-merge-platform")
    return convertMergePlatform(argv[2], argv[3], argv[4]);
  if (argc == 7 && std::string(argv[1]) == "--convert-merge-platform")
    return convertMergePlatform(argv[2], argv[3], argv[4], argv[5], argv[6]);
  // The language server (lsp.hpp): `kpp --lsp`, started by an editor, JSON-RPC on stdin and stdout.
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--lsp")
      return runLanguageServer(argc, argv, compile);
  // A project's build.kpp (project.hpp): `kpp` with no sources in its directory, `kpp new`.
  if (int code = runProject(argc, argv, compile); code >= 0)
    return code;
  // Project files for an IDE (generate.hpp) take the build's own arguments.
  for (int i = 1; i < argc; ++i)
    if (isGenerateOption(argv[i]))
      return generateProjectFiles(argc, argv);
  // `--run` and `--test` (run.hpp): the arguments after the sources are the program's.
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--run" || std::string(argv[i]) == "--test")
      return runProgram(argc, argv, i);
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--server" || std::string(argv[i]) == "--stop-server")
    {
      bool stop = std::string(argv[i]) == "--stop-server";
      std::vector<char*> rest(argv, argv + argc);
      rest.erase(rest.begin() + i);
      rest.push_back(nullptr);
      return stop ? stopServer(argc - 1, rest.data()) : runClient(argc - 1, rest.data(), compile);
    }
  return compile(argc, argv);
}

// On the compiler's stack, as every thread kpp starts is (host::compilerStack: the evaluator's bounds
// come before any thread's stack ends).
int main(int argc, char** argv)
{
  host::useCompilerStackForThreads();
  return host::runOnCompilerStack(compilerMain, argc, argv);
}
