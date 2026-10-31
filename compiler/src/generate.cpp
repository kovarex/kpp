#include "generate.hpp"
#include "source.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace
{
  namespace fs = std::filesystem;

  // Everything the project files need from the command line: the build's own arguments (passed on
  // to kppc unchanged), what lists the source files, and how the program is started.
  struct Generation
  {
    bool visualStudio = false, vsCode = false;
    std::vector<std::string> arguments;    // kppc's, without the configuration's and --exe
    std::vector<std::string> roots;
    std::vector<std::string> extensions = {".cpp", ".hpp", ".h", ".kpp"};
    std::string exe, stdDir;
    bool noStd = false;
    std::string debugCommand, debugArguments, debugDirectory, beforeBuild, afterBuild;
  };

  struct Configuration
  {
    const char* name;       // Visual Studio's
    const char* directory;  // build\<directory>\, docs/build.md's configuration name
    std::vector<std::string> arguments;
  };

  // debug and release as docs/build.md, "Named configurations": debug has line tables and links
  // incrementally, release is kppc's --release.
  const std::vector<Configuration> configurations = {
    {"Debug", "debug", {"--debug-info", "--incremental-link"}},
    {"Release", "release", {"--release"}},
  };

  const char* const valueOptions[] = {"-j", "--ext", "--repeat", "--errors", "--unresolved", "--why", "--why-at",
                                      "--emit-llvm", "--std", "--cinclude", "--clang-flags", "--codegen-threads",
                                      "--lib", "--config", "--modules", "--convert", "--overrides", "--exclude"};

  std::string xml(const std::string& text)
  {
    std::string out;
    for (char c : text)
      if (c == '&') out += "&amp;";
      else if (c == '<') out += "&lt;";
      else if (c == '>') out += "&gt;";
      else if (c == '"') out += "&quot;";
      else out += c;
    return out;
  }

  std::string json(const std::string& text)
  {
    std::string out = "\"";
    for (char c : text)
      if (c == '\\' || c == '"') out += '\\', out += c;
      else out += c;
    return out + "\"";
  }

  // A GUID that depends only on the text, so that generating again gives the same files.
  std::string guidOf(const std::string& text)
  {
    unsigned long long a = 1469598103934665603ull, b = 1099511628211ull * 31;
    for (unsigned char c : text)
      a = (a ^ c) * 1099511628211ull, b = (b ^ c) * 1469598103934665603ull + 7;
    char buffer[40];
    snprintf(buffer, sizeof(buffer), "%08X-%04X-%04X-%04X-%012llX", unsigned(a >> 32), unsigned(a >> 16) & 0xFFFF,
             unsigned(a) & 0xFFFF, unsigned(b >> 48), b & 0xFFFFFFFFFFFFull);
    return buffer;
  }

  std::string backslashes(std::string path)
  {
    for (char& c : path)
      if (c == '/')
        c = '\\';
    return path;
  }

  fs::path exeFor(const Generation& g, const Configuration& c)
  {
    fs::path exe = fs::absolute(g.exe);
    return (exe.parent_path() / c.directory / exe.filename()).lexically_normal();
  }

  // A step's command for one configuration: {exe} is the configuration's executable,
  // {configuration} its name.
  std::string step(std::string command, const fs::path& exe, const std::string& configuration)
  {
    for (auto [key, value] : {std::pair<std::string, std::string>{"{exe}", exe.string()}, {"{configuration}", configuration}})
      for (size_t at; (at = command.find(key)) != std::string::npos;)
        command.replace(at, key.size(), value);
    return command;
  }

  // Each source file with the folder it is shown in: its root's name and the directories below it;
  // the standard library's root is "Standard Library".
  struct Source
  {
    fs::path path;
    std::string folder;
  };

  std::vector<Source> sources(const IdeTarget& target, const fs::path& stdlib, const std::vector<std::string>& extensions)
  {
    std::vector<Source> result;
    std::vector<std::pair<fs::path, std::string>> roots;
    if (!stdlib.empty())
      roots.push_back({stdlib, "Standard Library"});
    for (const fs::path& root : target.roots)
    {
      fs::path path = root.lexically_normal();
      std::string name = path.has_filename() ? path.filename().string() : path.parent_path().filename().string();
      roots.push_back({path, fs::is_regular_file(path) ? "" : name});
    }
    std::set<fs::path> excluded;
    for (const fs::path& file : target.excluded)
      excluded.insert(file.lexically_normal());
    for (const auto& [root, name] : roots)
      for (const fs::path& file : collectFiles(root, extensions))
      {
        fs::path path = fs::absolute(file).lexically_normal();
        if (excluded.count(path))
          continue;
        std::string folder = name;
        if (!name.empty())
        {
          std::string below = file.parent_path().lexically_relative(root).string();
          if (!below.empty() && below != ".")
            folder += "\\" + backslashes(below);
        }
        result.push_back({path, folder});
      }
    return result;
  }

  bool writeFile(const fs::path& path, const std::string& text, bool crlf)
  {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::string out;
    for (char c : text)
    {
      if (c == '\n' && crlf)
        out += '\r';
      out += c;
    }
    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file)
    {
      fprintf(stderr, "error: cannot write %s\n", path.string().c_str());
      return false;
    }
    fwrite(out.data(), 1, out.size(), file);
    fclose(file);
    return true;
  }

  const char* const generatedMark = "// Generated by kppc";

  // A file in .vscode that someone else wrote is never overwritten.
  bool isOurs(const fs::path& path)
  {
    std::string text;
    return !fs::exists(path) || (readFile(path, text) && text.rfind(generatedMark, 0) == 0);
  }

  std::string vcxproj(const IdeTarget& target, const std::vector<Source>& files, const std::string& guid)
  {
    std::string project = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                          "<!-- Generated by kppc (its option generate-visual-studio-files): generate again instead of editing. -->\n"
                          "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
                          "  <ItemGroup Label=\"ProjectConfigurations\">\n";
    for (const IdeConfiguration& c : target.configurations)
      project += "    <ProjectConfiguration Include=\"" + c.visualStudioName + "|x64\"><Configuration>" + c.visualStudioName +
                 "</Configuration><Platform>x64</Platform></ProjectConfiguration>\n";
    project += "  </ItemGroup>\n"
               "  <PropertyGroup Label=\"Globals\">\n"
               "    <ProjectGuid>{" + guid + "}</ProjectGuid>\n"
               "    <Keyword>MakeFileProj</Keyword>\n"
               "  </PropertyGroup>\n"
               "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n"
               "  <PropertyGroup>\n"
               "    <ConfigurationType>Makefile</ConfigurationType>\n"
               "    <PlatformToolset>v143</PlatformToolset>\n"
               "    <OutDir>$(MSBuildThisFileDirectory)" + target.name + "\\$(Configuration)\\</OutDir>\n"
               "    <IntDir>$(MSBuildThisFileDirectory)" + target.name + "\\$(Configuration)\\</IntDir>\n"
               "    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>\n"
               "  </PropertyGroup>\n"
               "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n";
    for (const IdeConfiguration& c : target.configurations)
    {
      std::string exe = c.exe.string();
      project += "  <PropertyGroup Condition=\"'$(Configuration)' == '" + c.visualStudioName + "'\">\n"
                 "    <NMakeBuildCommandLine>" + xml(c.buildCommand) + "</NMakeBuildCommandLine>\n"
                 "    <NMakeReBuildCommandLine>" + xml(c.buildCommand) + "</NMakeReBuildCommandLine>\n"
                 "    <NMakeCleanCommandLine>" + xml("if exist " + quoteArg(exe) + " del " + quoteArg(exe)) + "</NMakeCleanCommandLine>\n"
                 "    <NMakeOutput>" + xml(exe) + "</NMakeOutput>\n"
                 "    <LocalDebuggerCommand>" + xml(c.program.string()) + "</LocalDebuggerCommand>\n"
                 "    <LocalDebuggerCommandArguments>" + xml(c.arguments) + "</LocalDebuggerCommandArguments>\n"
                 "    <LocalDebuggerWorkingDirectory>" + xml(c.directory.string()) + "</LocalDebuggerWorkingDirectory>\n"
                 "  </PropertyGroup>\n";
    }
    project += "  <ItemGroup>\n";
    for (const Source& file : files)
      project += "    <None Include=\"" + xml(file.path.string()) + "\" />\n";
    project += "  </ItemGroup>\n"
               "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n"
               "</Project>\n";
    return project;
  }

  // The folders of Solution Explorer: every source's folder and the folders above it.
  std::string filtersOf(const std::vector<Source>& files)
  {
    std::set<std::string> folders;
    for (const Source& file : files)
      for (std::string folder = file.folder; !folder.empty();)
      {
        folders.insert(folder);
        size_t slash = folder.rfind('\\');
        folder = slash == std::string::npos ? "" : folder.substr(0, slash);
      }
    std::string filters = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                          "<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
                          "  <ItemGroup>\n";
    for (const std::string& folder : folders)
      filters += "    <Filter Include=\"" + xml(folder) + "\"><UniqueIdentifier>{" + guidOf("folder " + folder) +
                 "}</UniqueIdentifier></Filter>\n";
    filters += "  </ItemGroup>\n"
               "  <ItemGroup>\n";
    for (const Source& file : files)
      filters += file.folder.empty() ? "    <None Include=\"" + xml(file.path.string()) + "\" />\n"
                                     : "    <None Include=\"" + xml(file.path.string()) + "\"><Filter>" + xml(file.folder) +
                                         "</Filter></None>\n";
    filters += "  </ItemGroup>\n"
               "</Project>\n";
    return filters;
  }

  bool writeVisualStudio(const std::string& solutionName, const fs::path& base, const std::vector<IdeTarget>& targets,
                         const std::vector<std::vector<Source>>& files)
  {
    fs::path directory = (base / "build" / "visual-studio").lexically_normal();
    std::string solution = "\xEF\xBB\xBF\n"
                           "Microsoft Visual Studio Solution File, Format Version 12.00\n"
                           "# Visual Studio Version 17\n"
                           "VisualStudioVersion = 17.0.31903.59\n"
                           "MinimumVisualStudioVersion = 10.0.40219.1\n";
    std::string platforms;
    size_t count = 0;
    for (size_t i = 0; i < targets.size(); ++i)
    {
      const IdeTarget& target = targets[i];
      std::string guid = guidOf("project " + target.name);
      if (!writeFile(directory / (target.name + ".vcxproj"), vcxproj(target, files[i], guid), true) ||
          !writeFile(directory / (target.name + ".vcxproj.filters"), filtersOf(files[i]), true))
        return false;
      solution += "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"" + target.name + "\", \"" + target.name +
                  ".vcxproj\", \"{" + guid + "}\"\nEndProject\n";
      for (const IdeConfiguration& c : target.configurations)
        platforms += "\t\t{" + guid + "}." + c.visualStudioName + "|x64.ActiveCfg = " + c.visualStudioName + "|x64\n"
                     "\t\t{" + guid + "}." + c.visualStudioName + "|x64.Build.0 = " + c.visualStudioName + "|x64\n";
      count += files[i].size();
    }
    solution += "Global\n"
                "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n";
    for (const IdeConfiguration& c : targets.front().configurations)
      solution += "\t\t" + c.visualStudioName + "|x64 = " + c.visualStudioName + "|x64\n";
    solution += "\tEndGlobalSection\n"
                "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n" + platforms +
                "\tEndGlobalSection\n"
                "EndGlobal\n";
    if (!writeFile(directory / (solutionName + ".sln"), solution, true))
      return false;
    printf("wrote %s (%zu project%s, %zu source files)\n", (directory / (solutionName + ".sln")).string().c_str(),
           targets.size(), targets.size() == 1 ? "" : "s", count);
    return true;
  }

  bool writeVsCode(const fs::path& base, const std::vector<IdeTarget>& targets)
  {
    fs::path directory = (base / ".vscode").lexically_normal();
    fs::path tasksPath = directory / "tasks.json", launchPath = directory / "launch.json",
             settingsPath = directory / "settings.json";
    for (const fs::path& path : {tasksPath, launchPath, settingsPath})
      if (!isOurs(path))
      {
        fprintf(stderr, "error: %s was not written by kppc; move it away to generate .vscode\n", path.string().c_str());
        return false;
      }
    std::string header = std::string(generatedMark) + " --generate-vs-code-files; generate again instead of editing.\n";
    // kppc's errors, path(line): error: text (absolute paths), in the Problems view.
    std::string matcher = "{ \"owner\": \"kpp\", \"fileLocation\": \"absolute\", \"pattern\": { \"regexp\": "
                          "\"^(.*)\\\\((\\\\d+)\\\\): (error|warning): (.*)$\", \"file\": 1, \"line\": 2, \"severity\": 3, \"message\": 4 } }";
    std::string tasks = header + "{\n  \"version\": \"2.0.0\",\n  \"tasks\": [\n";
    // Visual Studio's debugger (the C/C++ extension's cppvsdbg) reads the PDB --debug-info writes.
    std::string launch = header + "{\n  \"version\": \"0.2.0\",\n  \"configurations\": [\n";
    bool first = true;
    for (const IdeTarget& target : targets)
      for (const IdeConfiguration& c : target.configurations)
      {
        std::string label = "kpp: " + target.name + " " + c.name;
        tasks += std::string(first ? "" : ",\n") + "    { \"label\": " + json(label) +
                 ", \"type\": \"process\", \"command\": \"cmd.exe\", \"args\": [\"/d\", \"/s\", \"/c\", " + json(c.buildCommand) +
                 "], \"problemMatcher\": " + matcher + ", \"group\": " +
                 (first ? "{ \"kind\": \"build\", \"isDefault\": true }" : "\"build\"") + " }";
        launch += std::string(first ? "" : ",\n") + "    { \"name\": " + json(label) +
                  ", \"type\": \"cppvsdbg\", \"request\": \"launch\", \"program\": " + json(c.program.string()) + ", \"args\": [";
        std::string word;
        bool firstWord = true, inQuotes = false;
        auto flush = [&]
        {
          if (!word.empty())
            launch += (firstWord ? "" : ", ") + json(word), firstWord = false, word.clear();
        };
        for (char ch : c.arguments)
          if (ch == '"') inQuotes = !inQuotes;
          else if (ch == ' ' && !inQuotes) flush();
          else word += ch;
        flush();
        launch += "], \"cwd\": " + json(c.directory.string()) + ", \"preLaunchTask\": " + json(label) + " }";
        first = false;
      }
    tasks += "\n  ]\n}\n";
    launch += "\n  ]\n}\n";
    std::string settings = header + "{\n  \"debug.allowBreakpointsEverywhere\": true,\n"
                                    "  \"files.associations\": { \"*.kpp\": \"kpp\" }\n}\n";
    if (!writeFile(tasksPath, tasks, false) || !writeFile(launchPath, launch, false) || !writeFile(settingsPath, settings, false))
      return false;
    printf("wrote %s (tasks, launch, settings)\n", directory.string().c_str());
    return true;
  }
}

bool takesValue(const std::string& arg)
{
  for (const char* option : valueOptions)
    if (arg == option)
      return true;
  return false;
}

std::string quoteArg(const std::string& arg)
{
  return arg.empty() || arg.find_first_of(" \t&|<>^") != std::string::npos ? "\"" + arg + "\"" : arg;
}

bool writeIdeFiles(bool visualStudio, bool vsCode, const std::string& solution, const fs::path& base,
                   const std::vector<IdeTarget>& targets, const fs::path& stdlib, const std::vector<std::string>& extensions)
{
  std::vector<std::vector<Source>> files;
  for (const IdeTarget& target : targets)
    files.push_back(sources(target, stdlib, extensions));
  // kppc writes the executable and its object cache into an existing directory only.
  for (const IdeTarget& target : targets)
    for (const IdeConfiguration& c : target.configurations)
    {
      std::error_code ec;
      fs::create_directories(c.exe.parent_path(), ec);
    }
  return (!visualStudio || writeVisualStudio(solution, base, targets, files)) && (!vsCode || writeVsCode(base, targets));
}

bool isGenerateOption(const char* arg)
{
  return strcmp(arg, "--generate-visual-studio-files") == 0 || strcmp(arg, "--generate-vs-code-files") == 0;
}

int generateProjectFiles(int argc, char** argv)
{
  Generation g;
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];
    std::string* own = arg == "--debug-command" ? &g.debugCommand
                     : arg == "--debug-arguments" ? &g.debugArguments
                     : arg == "--debug-directory" ? &g.debugDirectory
                     : arg == "--before-build" ? &g.beforeBuild
                     : arg == "--after-build" ? &g.afterBuild
                     : nullptr;
    if (arg == "--generate-visual-studio-files")
      g.visualStudio = true;
    else if (arg == "--generate-vs-code-files")
      g.vsCode = true;
    else if (own)
    {
      if (i + 1 == argc)
      {
        fprintf(stderr, "error: %s needs a value\n", arg.c_str());
        return 2;
      }
      *own = argv[++i];
    }
    else if (arg == "--exe" && i + 1 < argc)
      g.exe = argv[++i];
    // The configurations' own options, and the ones the project files add themselves.
    else if (arg == "--release" || arg == "--debug-info" || arg == "--incremental-link" || arg == "--server" ||
             arg == "--msvc-diagnostics" || arg == "--quiet")
      ;
    else if (arg == "--run" || arg == "--test" || arg == "--stop-server")
    {
      fprintf(stderr, "error: %s cannot be part of generated project files\n", arg.c_str());
      return 2;
    }
    else if (takesValue(arg) && i + 1 < argc)
    {
      std::string value = argv[++i];
      if (arg == "--std")
        g.stdDir = value;
      else if (arg == "--ext")
      {
        g.extensions.clear();
        for (size_t pos = 0; pos <= value.size();)
        {
          size_t comma = value.find(',', pos);
          if (comma == std::string::npos)
            comma = value.size();
          g.extensions.push_back(value.substr(pos, comma - pos));
          pos = comma + 1;
        }
      }
      g.arguments.push_back(arg);
      g.arguments.push_back(value);
    }
    else
    {
      if (arg == "--no-std")
        g.noStd = true;
      // A source as an absolute path: kppc names files in its errors as they were found, and the
      // IDE opens an error's file relative to the project file, which is elsewhere.
      else if (arg[0] != '-')
      {
        g.roots.push_back(arg);
        arg = fs::absolute(arg).lexically_normal().string();
      }
      g.arguments.push_back(arg);
    }
  }
  if (g.exe.empty() || g.roots.empty())
  {
    fprintf(stderr, "error: project files need the sources and --exe, as a build does:\n"
                    "  kppc --generate-visual-studio-files [build options] --exe OUT.exe SOURCES...\n");
    return 2;
  }
  fs::path kppc = compilerPath();
  fs::path stdlib;
  if (!g.noStd)
    stdlib = g.stdDir.empty() ? defaultStdlib(kppc) : fs::absolute(g.stdDir).lexically_normal();
  if (!stdlib.empty() && !fs::is_directory(stdlib))
    stdlib.clear();
  fs::path cwd = fs::current_path();
  // With --keep-going the build links despite errors and fails, and the step after it still runs
  // on what it linked.
  bool keepGoing = std::find(g.arguments.begin(), g.arguments.end(), "--keep-going") != g.arguments.end();
  IdeTarget target;
  target.name = fs::path(g.exe).stem().string();
  for (const std::string& root : g.roots)
    target.roots.push_back(fs::absolute(root).lexically_normal());
  for (const Configuration& c : configurations)
  {
    IdeConfiguration ide;
    ide.visualStudioName = c.name;
    ide.name = c.directory;
    ide.exe = exeFor(g, c);
    std::string command = "cd /d " + quoteArg(cwd.string());
    if (!g.beforeBuild.empty())
      command += " && " + step(g.beforeBuild, ide.exe, ide.name) + " && cd /d " + quoteArg(cwd.string());
    command += " && " + quoteArg(kppc.string()) + " --server --msvc-diagnostics --quiet";
    for (const std::string& arg : c.arguments)
      command += " " + arg;
    for (const std::string& arg : g.arguments)
      command += " " + quoteArg(arg);
    command += " --exe " + quoteArg(ide.exe.string());
    if (!g.afterBuild.empty())
      command += (keepGoing ? " & " : " && ") + step(g.afterBuild, ide.exe, ide.name);
    ide.buildCommand = command;
    ide.program = g.debugCommand.empty() ? ide.exe : fs::absolute(g.debugCommand);
    ide.arguments = g.debugArguments;
    ide.directory = g.debugDirectory.empty() ? cwd : fs::absolute(g.debugDirectory);
    target.configurations.push_back(ide);
  }
  return writeIdeFiles(g.visualStudio, g.vsCode, target.name, cwd, {target}, stdlib, g.extensions) ? 0 : 1;
}
