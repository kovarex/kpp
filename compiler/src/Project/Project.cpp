// kpp on a project: runProject (kpp's command line), and the language server's cells and checks.
// (Moved out of project.cpp on 2026-11-05 without a change; its parts are this directory's files.)
#include "Project/Internal.hpp"

namespace projects
{
  std::chrono::steady_clock::time_point kppStarted = std::chrono::steady_clock::now();
  double describedAt = 0, usesResolvedAt = 0;
}

using namespace projects;


int runProject(int argc, char** argv, CompileFunction compile)
{
  kppStarted = std::chrono::steady_clock::now();
  std::error_code currentPathError; // (the project's directory as the current one: it is there, found above)
  if (argc >= 2 && strcmp(argv[1], "new") == 0)
    return newProject(argc >= 3 ? fs::absolute(argv[2]) : fs::current_path());
  bool options = argc >= 2 && strcmp(argv[1], "options") == 0; // kpp options [targets] [configurations]
  if (options)
    --argc, ++argv;
  fs::path root = findProject(fs::current_path());
  if (argc >= 2 && strcmp(argv[1], "measure") == 0 && !options)
  {
    if (root.empty())
    {
      fprintf(stderr, "error: kpp measure times a project's builds, and there is no build.kpp here or above\n");
      return 2;
    }
    fs::current_path(root, currentPathError);
    return measureProject(root, argc, argv);
  }
  if (argc == 1 && !options)
  {
    bool asking = interactiveInput();
    if (!asking && root.empty())
      return -1;
    if (!root.empty())
      fs::current_path(root, currentPathError);
    return menu(root, compile, asking);
  }
  if (root.empty() && options)
  {
    fprintf(stderr, "error: kpp options lists a project's library options, and there is no build.kpp here or above\n");
    return 2;
  }
  if (root.empty())
    return -1;
  // A project's command line names targets and configurations, never sources: an argument that is
  // an existing file or directory makes it a plain build of those sources. So does a .kpp file right
  // after --run while no target or configuration is named: `kpp --run hello.kpp a b` runs the file, as
  // outside a project (or says it is not there), and only `kpp app --run hello.kpp` gives it to app.
  Request request;
  request.options = options;
  bool runner = false; // after --test's `--`: the test runner's arguments
  std::string badOption; // the first argument that is no option of kpp's, or lacks its value
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];
    std::error_code ec;
    if (request.run || runner)
      request.runArguments.push_back(arg);
    else if (arg == "--test")
      request.test = true;
    else if (arg == "--" && request.test)
      runner = true;
    else if (arg == "--debug" || arg == "--release")
      request.configurations.push_back(arg.substr(2));
    else if (arg == "--run")
    {
      if (i + 1 < argc && request.names.empty() && !options && std::string_view(argv[i + 1]).ends_with(".kpp"))
        return -1;
      request.run = true;
    }
    else if (arg == "--generate-visual-studio-files")
      request.visualStudio = true;
    else if (arg == "--generate-vs-code-files")
      request.vsCode = true;
    else if (arg == "--no-server")
      request.server = false;
    else if (arg == "--verbose")
      request.verbose = true;
    else if (arg == "--stop-server")
      request.stop = true;
    else if (arg == "--clean")
      request.clean = true;
    else if (arg == "--rebuild")
      request.rebuild = true;
    else if (arg == "--msvc-diagnostics")
      request.msvc = true;
    else if (arg == "--set")
    {
      if (i + 1 >= argc)
      {
        fprintf(stderr, "error: --set needs NAME=VALUE\n");
        return 2;
      }
      std::string item = argv[++i];
      size_t equals = item.find('=');
      if (equals == std::string::npos)
      {
        fprintf(stderr, "error: --set needs NAME=VALUE, not %s\n", item.c_str());
        return 2;
      }
      request.settings.push_back({item.substr(0, equals), item.substr(equals + 1)});
    }
    else if (arg == "--server")
      request.server = true;
    else if (arg[0] == '-')
    {
      // kpp's options go on to each cell's build with their values; any other is an error, never
      // passed on or ignored (`kpp --version` among others built the project). The error waits for
      // the end: a source among the arguments makes them a plain build's (`kpp --server ... build.kpp`).
      int values = compileOptionValues(arg);
      if (values < 0 || i + values >= argc)
      {
        if (badOption.empty())
          badOption = arg;
        continue;
      }
      request.passed.push_back(arg);
      for (int v = 0; v < values; ++v)
        request.passed.push_back(argv[++i]);
    }
    else if (fs::exists(arg, ec))
      return -1;
    else
      request.names.push_back(arg);
  }
  if (!badOption.empty())
    return optionError(badOption);
  fs::current_path(root, currentPathError);
  return act(root, request, compile);
}

bool projectCells(const std::filesystem::path& directory, const ProjectCheck& check, std::vector<ProjectCell>& cells, std::string& error)
{
  fs::path root = check.buildFile.empty() ? findProject(directory) : fs::path(check.buildFile).parent_path();
  if (root.empty())
    return false;
  if (!check.buildFile.empty() && !fs::is_regular_file(check.buildFile))
  {
    error = check.buildFile + " does not exist";
    return true;
  }
  Request request;
  request.server = false;
  request.settings = check.settings;
  Description d;
  if (!describe(root, request, d) || !resolveUses(root, request, d))
  {
    error = (root / "build.kpp").string() + " did not describe the build";
    return true;
  }
  cells.clear();
  // Target by target, each in every configuration (generate's projects and solution configurations);
  // a library project's library in every configuration.
  std::vector<std::string> targets = targetNames(d);
  if (targets.empty())
    for (const KppLibrary& l : d.libraries)
      targets.push_back(l.name);
  for (const std::string& target : targets)
    for (const Configuration& c : d.configurations)
    {
      Program library;
      const Program* p = findCell(d, target, c.name);
      if (!p && d.programs.empty())
      {
        if (!libraryProgram(root, c, d.libraries[0], request, library))
        {
          error = (root / "build.kpp").string() + ": an option of the library " + target + " (above)";
          return true;
        }
        p = &library;
      }
      if (p)
      {
        ProjectCell cell{target, c.name, visualStudioName(c.name), p->options, p->kppLibraries};
        for (const std::string& r : p->roots)
          cell.roots.push_back(rooted(root, r).lexically_normal().string());
        for (const std::string& e : p->excluded)
          cell.excluded.push_back(rooted(root, e).lexically_normal().string());
        for (const ProjectLibrary& l : p->kppLibraries)
        {
          for (const std::string& r : l.roots)
            cell.roots.push_back(rooted(root, r).lexically_normal().string());
          for (const std::string& e : l.excluded)
            cell.excluded.push_back(rooted(root, e).lexically_normal().string());
        }
        cells.push_back(std::move(cell));
      }
    }
  return true;
}

bool projectCheckArguments(const std::filesystem::path& directory, const ProjectCheck& check, std::vector<std::string>& args, std::string& error)
{
  fs::path root = check.buildFile.empty() ? findProject(directory) : fs::path(check.buildFile).parent_path();
  if (root.empty())
    return false;
  if (!check.buildFile.empty() && !fs::is_regular_file(check.buildFile))
  {
    error = check.buildFile + " does not exist";
    return true;
  }
  Request request;
  request.server = false; // no compile server left running in the project for its description
  request.settings = check.settings;
  Description d;
  if (!describe(root, request, d) || !resolveUses(root, request, d))
  {
    error = (root / "build.kpp").string() + " did not describe the build";
    return true;
  }
  // Visual Studio's name of the configuration is accepted too (visualStudioName: Fastdebug).
  const Configuration* configuration = findConfiguration(d, check.configuration);
  for (const Configuration& c : d.configurations)
    if (!configuration && visualStudioName(c.name) == check.configuration)
      configuration = &c;
  if (!configuration)
  {
    std::string known;
    for (const Configuration& c : d.configurations)
      known += (known.empty() ? "" : ", ") + c.name;
    error = "build.kpp has no configuration " + check.configuration + " (" + known + ")";
    return true;
  }
  if (d.programs.empty() && d.libraries.empty())
  {
    error = (root / "build.kpp").string() + " describes no executable";
    return true;
  }
  // One cell, or (no target named) the first target's cell with every target's sources in that
  // configuration; a library's sources and test sources when the project is a library.
  Program program;
  if (d.programs.empty())
  {
    if (!libraryProgram(root, *configuration, d.libraries[0], request, program))
    {
      error = (root / "build.kpp").string() + ": an option of the library " + d.libraries[0].name + " (above)";
      return true;
    }
    program.arguments.push_back("--tests"); // its test blocks are checked too
  }
  else if (!check.target.empty())
  {
    const Program* named = findCell(d, check.target, configuration->name);
    if (const Program* none = named ? nullptr : findUnavailableCell(d, check.target, configuration->name))
    {
      error = check.target + " has no " + configuration->name + " configuration: " + none->unavailable;
      return true;
    }
    if (!named)
    {
      error = "build.kpp describes no executable " + check.target;
      return true;
    }
    program = *named;
  }
  else
  {
    bool first = true;
    for (const Program& p : d.programs)
    {
      if (p.configuration != configuration->name)
        continue;
      if (first)
      {
        program = p, first = false;
        continue;
      }
      auto addAll = [](std::vector<std::string>& to, const std::vector<std::string>& from)
      {
        for (const std::string& x : from)
          if (std::find(to.begin(), to.end(), x) == to.end())
            to.push_back(x);
      };
      addAll(program.roots, p.roots);
      for (const ProjectLibrary& l : p.kppLibraries)
        if (std::none_of(program.kppLibraries.begin(), program.kppLibraries.end(), [&](const ProjectLibrary& k) { return k.name == l.name; }))
          program.kppLibraries.push_back(l);
      addAll(program.excluded, p.excluded);
      addAll(program.cIncludes, p.cIncludes);
    }
  }
  // What only code generation and the link read is left out (the executable, the link's options).
  std::vector<std::string> all = buildArguments(root, d, program, request);
  args.clear();
  for (size_t i = 0; i < all.size(); ++i)
    if (all[i] == "--exe" || all[i] == "--lib")
      ++i;
    else if (all[i] != "--incremental-link" && all[i] != "--debug-info")
      args.push_back(all[i]);
  return true;
}
