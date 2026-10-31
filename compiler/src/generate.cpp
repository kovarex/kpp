#include "generate.hpp"
#include "source.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

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

  // kppc's options that take a value (main.cpp's compile), so that the value is not read as a source.
  const char* const valueOptions[] = {"-j", "--ext", "--repeat", "--errors", "--unresolved", "--why", "--why-at",
                                      "--emit-llvm", "--std", "--cinclude", "--clang-flags", "--codegen-threads",
                                      "--lib", "--config", "--modules", "--convert", "--overrides"};

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
  std::string step(std::string command, const Generation& g, const Configuration& c)
  {
    for (auto [key, value] : {std::pair<std::string, std::string>{"{exe}", exeFor(g, c).string()}, {"{configuration}", c.directory}})
      for (size_t at; (at = command.find(key)) != std::string::npos;)
        command.replace(at, key.size(), value);
    return command;
  }

  // The kppc command of one configuration, as its arguments.
  std::vector<std::string> buildArguments(const Generation& g, const Configuration& c)
  {
    std::vector<std::string> args = {"--server", "--msvc-diagnostics", "--quiet"};
    args.insert(args.end(), c.arguments.begin(), c.arguments.end());
    args.insert(args.end(), g.arguments.begin(), g.arguments.end());
    args.push_back("--exe");
    args.push_back(exeFor(g, c).string());
    return args;
  }

  std::vector<std::string> split(const std::string& text)
  {
    std::vector<std::string> words;
    std::string word;
    bool inQuotes = false;
    for (char c : text)
      if (c == '"') inQuotes = !inQuotes;
      else if (c == ' ' && !inQuotes)
      {
        if (!word.empty())
          words.push_back(word), word.clear();
      }
      else word += c;
    if (!word.empty())
      words.push_back(word);
    return words;
  }

  // Each source file with the folder it is shown in: its root's name and the directories below it;
  // the standard library's root is "Standard Library".
  struct Source
  {
    fs::path path;
    std::string folder;
  };

  std::vector<Source> sources(const Generation& g, const fs::path& stdlib)
  {
    std::vector<Source> result;
    std::vector<std::pair<fs::path, std::string>> roots;
    if (!stdlib.empty())
      roots.push_back({stdlib, "Standard Library"});
    for (const std::string& root : g.roots)
    {
      fs::path path = fs::absolute(root).lexically_normal();
      std::string name = path.has_filename() ? path.filename().string() : path.parent_path().filename().string();
      roots.push_back({path, fs::is_regular_file(path) ? "" : name});
    }
    for (const auto& [root, name] : roots)
      for (const fs::path& file : collectFiles(root, g.extensions))
      {
        std::string folder = name;
        if (!name.empty())
        {
          std::string below = file.parent_path().lexically_relative(root).string();
          if (!below.empty() && below != ".")
            folder += "\\" + backslashes(below);
        }
        result.push_back({fs::absolute(file).lexically_normal(), folder});
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

  bool writeVisualStudio(const Generation& g, const std::vector<Source>& files, const fs::path& kppc)
  {
    std::string name = fs::path(g.exe).stem().string();
    fs::path directory = fs::absolute("build/visual-studio").lexically_normal();
    fs::path cwd = fs::current_path();
    std::string guid = guidOf("project " + name);

    std::string project = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                          "<!-- Generated by kppc (its option generate-visual-studio-files): generate again instead of editing. -->\n"
                          "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
                          "  <ItemGroup Label=\"ProjectConfigurations\">\n";
    for (const Configuration& c : configurations)
      project += std::string("    <ProjectConfiguration Include=\"") + c.name + "|x64\"><Configuration>" + c.name +
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
               "    <OutDir>$(MSBuildThisFileDirectory)$(Configuration)\\</OutDir>\n"
               "    <IntDir>$(MSBuildThisFileDirectory)$(Configuration)\\</IntDir>\n"
               "    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>\n"
               "  </PropertyGroup>\n"
               "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n";
    for (const Configuration& c : configurations)
    {
      std::string command = "cd /d " + quoteArg(cwd.string());
      if (!g.beforeBuild.empty())
        command += " && " + step(g.beforeBuild, g, c) + " && cd /d " + quoteArg(cwd.string());
      command += " && " + quoteArg(kppc.string());
      for (const std::string& arg : buildArguments(g, c))
        command += " " + quoteArg(arg);
      // With --keep-going the build links despite errors and fails, and the step still runs on what it linked.
      bool keepGoing = std::find(g.arguments.begin(), g.arguments.end(), "--keep-going") != g.arguments.end();
      if (!g.afterBuild.empty())
        command += (keepGoing ? " & " : " && ") + step(g.afterBuild, g, c);
      std::string exe = exeFor(g, c).string();
      project += std::string("  <PropertyGroup Condition=\"'$(Configuration)' == '") + c.name + "'\">\n"
                 "    <NMakeBuildCommandLine>" + xml(command) + "</NMakeBuildCommandLine>\n"
                 "    <NMakeReBuildCommandLine>" + xml(command) + "</NMakeReBuildCommandLine>\n"
                 "    <NMakeCleanCommandLine>" + xml("if exist " + quoteArg(exe) + " del " + quoteArg(exe)) + "</NMakeCleanCommandLine>\n"
                 "    <NMakeOutput>" + xml(exe) + "</NMakeOutput>\n"
                 "    <LocalDebuggerCommand>" + xml(g.debugCommand.empty() ? exe : fs::absolute(g.debugCommand).string()) + "</LocalDebuggerCommand>\n"
                 "    <LocalDebuggerCommandArguments>" + xml(g.debugArguments) + "</LocalDebuggerCommandArguments>\n"
                 "    <LocalDebuggerWorkingDirectory>" + xml(g.debugDirectory.empty() ? cwd.string() : fs::absolute(g.debugDirectory).string()) + "</LocalDebuggerWorkingDirectory>\n"
                 "  </PropertyGroup>\n";
    }
    project += "  <ItemGroup>\n";
    for (const Source& file : files)
      project += "    <None Include=\"" + xml(file.path.string()) + "\" />\n";
    project += "  </ItemGroup>\n"
               "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n"
               "</Project>\n";

    // The folders of Solution Explorer: every source's folder and the folders above it.
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

    std::string solution = "\xEF\xBB\xBF\n"
                           "Microsoft Visual Studio Solution File, Format Version 12.00\n"
                           "# Visual Studio Version 17\n"
                           "VisualStudioVersion = 17.0.31903.59\n"
                           "MinimumVisualStudioVersion = 10.0.40219.1\n"
                           "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"" + name + "\", \"" + name +
                           ".vcxproj\", \"{" + guid + "}\"\n"
                           "EndProject\n"
                           "Global\n"
                           "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n";
    for (const Configuration& c : configurations)
      solution += std::string("\t\t") + c.name + "|x64 = " + c.name + "|x64\n";
    solution += "\tEndGlobalSection\n"
                "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n";
    for (const Configuration& c : configurations)
      solution += "\t\t{" + guid + "}." + c.name + "|x64.ActiveCfg = " + c.name + "|x64\n"
                  "\t\t{" + guid + "}." + c.name + "|x64.Build.0 = " + c.name + "|x64\n";
    solution += "\tEndGlobalSection\n"
                "EndGlobal\n";

    if (!writeFile(directory / (name + ".vcxproj"), project, true) ||
        !writeFile(directory / (name + ".vcxproj.filters"), filters, true) ||
        !writeFile(directory / (name + ".sln"), solution, true))
      return false;
    printf("wrote %s (%zu source files)\n", (directory / (name + ".sln")).string().c_str(), files.size());
    return true;
  }

  bool writeVsCode(const Generation& g, const std::vector<Source>& files, const fs::path& kppc)
  {
    fs::path directory = fs::absolute(".vscode").lexically_normal();
    fs::path tasksPath = directory / "tasks.json", launchPath = directory / "launch.json",
             settingsPath = directory / "settings.json";
    for (const fs::path& path : {tasksPath, launchPath, settingsPath})
      if (!isOurs(path))
      {
        fprintf(stderr, "error: %s was not written by kppc; move it away to generate .vscode\n", path.string().c_str());
        return false;
      }
    std::string cwd = fs::current_path().string();
    std::string header = std::string(generatedMark) + " --generate-vs-code-files; generate again instead of editing.\n";

    // kppc's errors, path(line): error: text, in the Problems view.
    std::string matcher = "{ \"owner\": \"kpp\", \"fileLocation\": [\"autoDetect\", " + json(cwd) +
                          "], \"pattern\": { \"regexp\": \"^(.*)\\\\((\\\\d+)\\\\): (error|warning): (.*)$\", "
                          "\"file\": 1, \"line\": 2, \"severity\": 3, \"message\": 4 } }";
    std::string tasks = header + "{\n  \"version\": \"2.0.0\",\n  \"tasks\": [\n";
    auto shellTask = [&](const std::string& label, const std::string& command)
    {
      tasks += "    { \"label\": " + json(label) + ", \"type\": \"process\", \"command\": \"cmd.exe\", \"args\": [\"/d\", \"/c\", " +
               json(command) + "], \"options\": { \"cwd\": " + json(cwd) + " } },\n";
    };
    for (const Configuration& c : configurations)
    {
      std::string name = c.directory;
      if (!g.beforeBuild.empty())
        shellTask("kpp: before build " + name, step(g.beforeBuild, g, c));
      if (!g.afterBuild.empty())
        shellTask("kpp: after build " + name, step(g.afterBuild, g, c));
      tasks += std::string("    { \"label\": \"kpp: compile ") + c.directory + "\", \"type\": \"process\", \"command\": " +
               json(kppc.string()) + ", \"args\": [";
      bool first = true;
      for (const std::string& arg : buildArguments(g, c))
        tasks += (first ? "" : ", ") + json(arg), first = false;
      tasks += "], \"options\": { \"cwd\": " + json(cwd) + " }, \"problemMatcher\": " + matcher + " },\n";
      std::string steps = (g.beforeBuild.empty() ? "" : "\"kpp: before build " + name + "\", ") + "\"kpp: compile " + name +
                          "\"" + (g.afterBuild.empty() ? "" : ", \"kpp: after build " + name + "\"");
      tasks += std::string("    { \"label\": \"kpp: ") + c.directory + "\", \"dependsOn\": [" + steps +
               "], \"dependsOrder\": \"sequence\", \"problemMatcher\": [], \"group\": " +
               (c.directory == std::string("debug") ? "{ \"kind\": \"build\", \"isDefault\": true }" : "\"build\"") + " }" +
               (&c == &configurations.back() ? "\n" : ",\n");
    }
    tasks += "  ]\n}\n";

    // Visual Studio's debugger (the C/C++ extension's cppvsdbg) reads the PDB --debug-info writes.
    std::string launch = header + "{\n  \"version\": \"0.2.0\",\n  \"configurations\": [\n";
    for (const Configuration& c : configurations)
    {
      std::string program = g.debugCommand.empty() ? exeFor(g, c).string() : fs::absolute(g.debugCommand).string();
      launch += std::string("    { \"name\": \"kpp: ") + c.directory + "\", \"type\": \"cppvsdbg\", \"request\": \"launch\", \"program\": " +
                json(program) + ", \"args\": [";
      bool first = true;
      for (const std::string& arg : split(g.debugArguments))
        launch += (first ? "" : ", ") + json(arg), first = false;
      launch += "], \"cwd\": " + json(g.debugDirectory.empty() ? cwd : fs::absolute(g.debugDirectory).string()) +
                ", \"preLaunchTask\": \"kpp: " + c.directory + "\" }" + (&c == &configurations.back() ? "\n" : ",\n");
    }
    launch += "  ]\n}\n";

    std::string settings = header + "{\n  \"debug.allowBreakpointsEverywhere\": true,\n"
                                    "  \"files.associations\": { \"*.kpp\": \"kpp\" }\n}\n";
    if (!writeFile(tasksPath, tasks, false) || !writeFile(launchPath, launch, false) || !writeFile(settingsPath, settings, false))
      return false;
    printf("wrote %s (tasks, launch, settings; %zu source files)\n", directory.string().c_str(), files.size());
    return true;
  }
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
  std::vector<Source> files = sources(g, stdlib);
  // kppc writes the executable and its object cache into an existing directory only.
  for (const Configuration& c : configurations)
  {
    std::error_code ec;
    fs::create_directories(exeFor(g, c).parent_path(), ec);
  }
  if ((g.visualStudio && !writeVisualStudio(g, files, kppc)) || (g.vsCode && !writeVsCode(g, files, kppc)))
    return 1;
  return 0;
}
