#include "project.hpp"
#include "generate.hpp"
#include "run.hpp"
#include "source.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <io.h>
#include <string>
#include <vector>
#include <windows.h>

namespace
{
  namespace fs = std::filesystem;

  // The version a library's minimum-kpp is compared with (docs/build.md, "kpp libraries"); raised
  // with each release.
  const char* const kppVersion = "0.1.0";

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
    std::vector<std::string> configFrom; // per config, who set it: project, configuration, target, cell, option
  };

  // One cell, an executable in a configuration, as build.kpp described it (projectlib\Project.kpp's
  // describe, configure(cell) applied). After resolveUses, kppLibraries holds the libraries it uses,
  // each with its roots, absolute, and its scope.
  struct Program : Configuration
  {
    std::string configuration;
    std::vector<std::string> roots, excluded, libraries, cIncludes, links;
    std::string beforeBuild, afterBuild, debugCommand, debugArguments, debugDirectory;
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

  // build.kpp's matrix: its configurations, then its cells, target by target.
  struct Description
  {
    std::vector<Configuration> configurations;
    std::vector<Program> programs;
    std::vector<CLibrary> cLibraries;
    std::vector<KppLibrary> libraries;
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

  // One kppc compile: through the compile server (runClient), or a kppc process of its own. Never
  // compile() in this process: a build that is no server's ends its process when it is done.
  int invoke(std::vector<std::string> args, bool server, CompileFunction compile, bool stop = false)
  {
    if (stop)
    {
      std::vector<char*> argv;
      for (std::string& a : args)
        argv.push_back(a.data());
      argv.push_back(nullptr);
      return stopServer(int(args.size()), argv.data());
    }
    if (!server)
    {
      std::string command;
      for (const std::string& a : args)
        command += (command.empty() ? "" : " ") + quoteArg(a);
      return system(("\"" + command + "\"").c_str());
    }
    std::vector<char*> argv;
    for (std::string& a : args)
      argv.push_back(a.data());
    argv.push_back(nullptr);
    return runClient(int(args.size()), argv.data(), compile);
  }

  fs::path findProject(fs::path directory)
  {
    std::error_code ec;
    for (directory = fs::absolute(directory, ec); !directory.empty(); directory = directory.parent_path())
    {
      if (fs::is_regular_file(directory / "build.kpp", ec))
        return directory;
      if (directory == directory.parent_path())
        break;
    }
    return {};
  }

  std::string kppString(const std::string& text)
  {
    std::string out = "\"";
    for (char c : text)
      if (c == '\\' || c == '"') out += '\\', out += c;
      else out += c;
    return out + "\"";
  }

  uint64_t hashText(uint64_t h, const std::string& text)
  {
    for (unsigned char c : text)
      h = (h ^ c) * 1099511628211ull;
    return (h ^ 0xff) * 1099511628211ull;
  }

  std::string readText(const fs::path& path)
  {
    std::string text;
    readFile(path, text);
    return text;
  }

  // The file compiled with build.kpp and projectlib: the command line's settings and the project's
  // name, which Project::setting and describe() read.
  std::string settingsSource(const fs::path& root, const Request& request)
  {
    std::string s = "// Generated by kpp from the command line's settings: compiled with build.kpp and projectlib.\n"
                    "StringView kppSetting(StringView name, StringView otherwise)\n{\n";
    for (const auto& [name, value] : request.settings)
      s += "  if (name == " + kppString(name) + ")\n    return " + kppString(value) + ";\n";
    s += "  return otherwise;\n}\n"
         "StringView kppProjectName() { return " + kppString(root.filename().string()) + "; }\n";
    return s;
  }

  Description parse(const std::string& text)
  {
    Description d;
    enum { none, configuration, program, cLibrary, library } section = none;
    size_t pos = 0;
    while (pos < text.size())
    {
      size_t end = text.find('\n', pos);
      if (end == std::string::npos)
        end = text.size();
      std::string line = text.substr(pos, end - pos);
      pos = end + 1;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      size_t space = line.find(' ');
      std::string key = line.substr(0, space), value = space == std::string::npos ? "" : line.substr(space + 1);
      if (key == "error")
        d.errors.push_back(value);
      else if (key == "configuration")
        d.configurations.push_back({value}), section = configuration;
      else if (key == "cell")
      {
        size_t blank = value.find(' ');
        Program p;
        p.name = value.substr(0, blank);
        p.configuration = blank == std::string::npos ? "" : value.substr(blank + 1);
        d.programs.push_back(p), section = program;
      }
      else if (key == "clibrary")
        d.cLibraries.push_back({value}), section = cLibrary;
      else if (key == "kpp-library") // not "library": an executable's link file is that key
        d.libraries.push_back({value}), section = library;
      else if (key == "end")
        section = none;
      else if ((section == program || section == library) && (key == "use" || key == "use-minimum" || key == "use-path" || key == "use-config"))
      {
        std::vector<Use>& uses = section == program ? d.programs.back().uses : d.libraries.back().uses;
        if (key == "use") uses.push_back({value});
        else if (uses.empty()) continue;
        else if (key == "use-minimum") uses.back().minimum = value;
        else if (key == "use-config") uses.back().configs.push_back(value);
        else uses.back().path = value;
      }
      else if ((section == configuration || section == program) &&
               (key == "optimize" || key == "debug-info" || key == "config" || key == "clang-flag" || key == "argument"))
      {
        Configuration& c = section == program ? d.programs.back() : d.configurations.back();
        if (key == "optimize") c.optimize = value == "1";
        else if (key == "debug-info") c.debugInfo = value == "1";
        else if (key == "config") // `config SOURCE NAME=VALUE`
        {
          size_t blank = value.find(' ');
          c.configFrom.push_back(value.substr(0, blank));
          c.configs.push_back(blank == std::string::npos ? "" : value.substr(blank + 1));
        }
        else if (key == "clang-flag") c.clangFlags.push_back(value);
        else c.arguments.push_back(value);
      }
      else if (section == library)
      {
        KppLibrary& l = d.libraries.back();
        if (key == "version") l.version = value;
        else if (key == "license") l.license = value;
        else if (key == "minimum-kpp") l.minimumKpp = value;
        else if (key == "unscoped") l.unscoped = value == "1";
        else if (key == "root") l.roots.push_back(value);
        else if (key == "test-root") l.testRoots.push_back(value);
        else if (key == "option") l.options.push_back({value});
        else if (l.options.empty()) continue;
        else if (key == "option-type") l.options.back().type = value;
        else if (key == "option-default") l.options.back().defaultValue = value;
        else if (key == "option-required") l.options.back().required = true;
        else if (key == "option-value") l.options.back().allowed.push_back(value);
        else if (key == "option-description") l.options.back().description = value;
      }
      else if (section == cLibrary)
      {
        CLibrary& c = d.cLibraries.back();
        if (key == "source") c.sources.push_back(value);
        else if (key == "exclude") c.excluded.push_back(value);
        else if (key == "include") c.includes.push_back(value);
        else if (key == "define") c.defines.push_back(value);
        else if (key == "prebuilt") c.prebuilt.push_back(value);
        else if (key == "resource") c.resources.push_back(value);
        else if (key == "language") c.language = value;
      }
      else if (section != program)
        continue;
      else
      {
        Program& p = d.programs.back();
        if (key == "root") p.roots.push_back(value);
        else if (key == "exclude") p.excluded.push_back(value);
        else if (key == "library") p.libraries.push_back(value);
        else if (key == "cinclude") p.cIncludes.push_back(value);
        else if (key == "before-build") p.beforeBuild = value;
        else if (key == "after-build") p.afterBuild = value;
        else if (key == "debug-command") p.debugCommand = value;
        else if (key == "debug-arguments") p.debugArguments = value;
        else if (key == "debug-directory") p.debugDirectory = value;
        else if (key == "link") p.links.push_back(value);
      }
    }
    return d;
  }

  const Configuration* findConfiguration(const Description& d, const std::string& name)
  {
    for (const Configuration& c : d.configurations)
      if (c.name == name)
        return &c;
    return nullptr;
  }

  const Program* findCell(const Description& d, const std::string& target, const std::string& configuration)
  {
    for (const Program& p : d.programs)
      if (p.name == target && p.configuration == configuration)
        return &p;
    return nullptr;
  }

  // The targets, in build.kpp's order.
  std::vector<std::string> targetNames(const Description& d)
  {
    std::vector<std::string> names;
    for (const Program& p : d.programs)
      if (std::find(names.begin(), names.end(), p.name) == names.end())
        names.push_back(p.name);
    return names;
  }

  // What command prints on stdout, and its exit code; its stderr goes where this process's does.
  int runAndRead(const std::string& command, std::string& out)
  {
    FILE* pipe = _popen(("\"" + command + "\"").c_str(), "rb");
    if (!pipe)
      return -1;
    char buffer[4096];
    for (size_t n; (n = fread(buffer, 1, sizeof(buffer), pipe)) > 0;)
      out.append(buffer, n);
    return _pclose(pipe);
  }

  // kppc on build.kpp, projectlib and the settings: the interpreter runs build() once and
  // describe(), which runs configure() for every cell (--run-build; projectlib's Project has no
  // configuration, so the name `matrix` is not used), and prints the description. Through the
  // compile server unless --no-server, so that the standard library is not checked again for every
  // build.
  std::vector<std::string> describeArguments(const fs::path& root, const Request& request)
  {
    fs::path self = compilerPath();
    std::vector<std::string> args = {self.string(), "--quiet", "--run-build", "matrix"};
    if (request.msvc)
      args.push_back("--msvc-diagnostics");
    for (const fs::path& a : {defaultStdlib(self).parent_path() / "projectlib", root / "build.kpp", root / "build" / "kpp" / "settings.kpp"})
      args.push_back(a.string());
    return args;
  }

  std::string describeCommand(const fs::path& root, const Request& request)
  {
    std::vector<std::string> args = describeArguments(root, request);
    if (request.server)
      args.insert(args.begin() + 1, "--server");
    std::string command;
    for (const std::string& a : args)
      command += (command.empty() ? "" : " ") + quoteArg(a);
    return command;
  }

  // build.kpp's description, every cell (describeCommand). Kept in build\kpp\describe.txt with a key
  // of everything it depends on, so that a build after no change of them compiles nothing. False,
  // with the errors printed, when it has an error line or names a target and a configuration alike.
  bool describe(const fs::path& root, const Request& request, Description& out)
  {
    fs::path self = compilerPath();
    fs::path library = defaultStdlib(self).parent_path() / "projectlib";
    fs::path directory = root / "build" / "kpp";
    std::error_code ec;
    fs::create_directories(directory, ec);
    std::string source = settingsSource(root, request);
    uint64_t key = hashText(1469598103934665603ull, source);
    key = hashText(key, readText(root / "build.kpp"));
    for (const fs::path& file : collectFiles(library, {".kpp"}))
      key = hashText(key, readText(file));
    key = hashText(key, self.string() + std::to_string(fs::file_size(self, ec)) +
                            std::to_string(fs::last_write_time(self, ec).time_since_epoch().count()));
    std::string keyText = std::to_string(key);
    fs::path described = directory / "describe.txt";
    if (readText(directory / "describe.key") != keyText || !fs::exists(described, ec))
    {
      FILE* file = _wfopen((directory / "settings.kpp").c_str(), L"wb");
      if (!file)
        return false;
      fwrite(source.data(), 1, source.size(), file);
      fclose(file);
      std::string text;
      if (runAndRead(describeCommand(root, request), text) != 0 || text.find("configuration ") == std::string::npos)
      {
        fprintf(stderr, "%s: error: build.kpp did not describe the build (above)\n", (root / "build.kpp").string().c_str());
        return false;
      }
      FILE* keep = _wfopen(described.c_str(), L"wb");
      if (keep)
        fwrite(text.data(), 1, text.size(), keep), fclose(keep);
      FILE* keyFile = _wfopen((directory / "describe.key").c_str(), L"wb");
      if (keyFile)
        fwrite(keyText.data(), 1, keyText.size(), keyFile), fclose(keyFile);
    }
    out = parse(readText(described));
    std::string file = (root / "build.kpp").string();
    for (const std::string& error : out.errors)
      fprintf(stderr, "%s: error: %s\n", file.c_str(), error.c_str());
    for (const std::string& target : targetNames(out))
      if (findConfiguration(out, target))
      {
        fprintf(stderr, "%s: error: %s is a target and a configuration; `kpp %s` would not say which\n", file.c_str(),
                target.c_str(), target.c_str());
        out.errors.push_back(target);
      }
    // A library's name is the qualifier of its names (zlib::compress, design.md 16): an identifier.
    for (const KppLibrary& l : out.libraries)
      if (l.name.empty() || isdigit((unsigned char)l.name[0]) ||
          !std::all_of(l.name.begin(), l.name.end(), [](char c) { return isalnum((unsigned char)c) || c == '_'; }))
      {
        fprintf(stderr, "%s: error: the library \"%s\" needs a name that is an identifier: its users write %s::NAME\n",
                file.c_str(), l.name.c_str(), l.name.c_str());
        out.errors.push_back(l.name);
      }
    return out.errors.empty() && !out.configurations.empty();
  }

  fs::path rooted(const fs::path& root, const std::string& path)
  {
    fs::path p(path);
    return (p.is_absolute() ? p : root / p).lexically_normal();
  }

  // A version's numbers, major.minor.patch; a missing one is 0. False when it is not of that form.
  bool versionNumbers(const std::string& text, std::vector<unsigned long>& numbers)
  {
    numbers.clear();
    for (size_t start = 0;; )
    {
      size_t dot = std::min(text.find('.', start), text.size());
      if (dot == start || dot - start > 9 || numbers.size() == 3 || text.find_first_not_of("0123456789", start) < dot)
        return false;
      numbers.push_back(std::stoul(text.substr(start, dot - start)));
      if (dot == text.size())
        break;
      start = dot + 1;
    }
    numbers.resize(3, 0);
    return true;
  }

  // The libraries uses names (by's build.kpp says use()), each found at its path, or else in the
  // project's libraries/NAME, or else next to by when by is in a directory named libraries (the
  // libraries of one libraries/ directory use each other), then described (the same settings),
  // checked (its version, its minimum kpp), and then the libraries it uses, depth
  // first. Each library is added to libraries once, with its roots, absolute, and its scope. A name
  // found in two directories is an error: a program has one copy of each library.
  bool resolveUses(const fs::path& project, const fs::path& by, const std::vector<Use>& uses, const Request& request,
                   std::vector<Found>& found, std::vector<ProjectLibrary>& libraries)
  {
    std::error_code ec;
    for (const Use& use : uses)
    {
      fs::path directory = use.path.empty() ? project / "libraries" / use.name : rooted(by, use.path);
      if (use.path.empty() && !fs::is_regular_file(directory / "build.kpp", ec) && by.parent_path().filename() == "libraries")
        directory = by.parent_path() / use.name;
      directory = fs::absolute(directory, ec).lexically_normal();
      std::string user = (by / "build.kpp").string();
      auto same = std::find_if(found.begin(), found.end(), [&](const Found& f) { return f.name == use.name; });
      if (same != found.end() && same->directory != directory)
      {
        fprintf(stderr, "%s: error: the library %s is used from %s, and from %s by %s; a program has one copy of each library\n",
                user.c_str(), use.name.c_str(), directory.string().c_str(), same->directory.string().c_str(), same->by.c_str());
        return false;
      }
      if (!fs::is_regular_file(directory / "build.kpp", ec))
      {
        if (use.path.empty())
          fprintf(stderr, "%s: error: the library %s is not in %s (no build.kpp there), and there is no registry yet\n",
                  user.c_str(), use.name.c_str(), directory.string().c_str());
        else
          fprintf(stderr, "%s: error: the library %s: %s has no build.kpp\n", user.c_str(), use.name.c_str(), directory.string().c_str());
        return false;
      }
      Request serverless = request; // no compile server left running in a library for its description
      serverless.server = false;
      Description d;
      if (!describe(directory, serverless, d))
        return false;
      const KppLibrary* library = nullptr;
      for (const KppLibrary& l : d.libraries)
        if (l.name == use.name)
          library = &l;
      if (!library)
      {
        fprintf(stderr, "%s: error: it declares no library %s (project.library(\"%s\")), which %s uses\n",
                (directory / "build.kpp").string().c_str(), use.name.c_str(), use.name.c_str(), user.c_str());
        return false;
      }
      if (!d.cLibraries.empty())
      {
        fprintf(stderr, "%s: error: the library %s declares C libraries, which a used library cannot have yet\n",
                (directory / "build.kpp").string().c_str(), use.name.c_str());
        return false;
      }
      std::vector<unsigned long> have, need, kpp;
      if (!versionNumbers(library->version, have))
      {
        fprintf(stderr, "%s: error: the library %s has version \"%s\", not major.minor.patch\n",
                (directory / "build.kpp").string().c_str(), use.name.c_str(), library->version.c_str());
        return false;
      }
      if (!use.minimum.empty() && (!versionNumbers(use.minimum, need) || have < need))
      {
        fprintf(stderr, "%s: error: it uses the library %s %s or later, and %s has %s\n", user.c_str(), use.name.c_str(),
                use.minimum.c_str(), directory.string().c_str(), library->version.c_str());
        return false;
      }
      versionNumbers(kppVersion, kpp);
      if (!library->minimumKpp.empty() && (!versionNumbers(library->minimumKpp, need) || kpp < need))
      {
        fprintf(stderr, "%s: error: the library %s needs kpp %s or later, and this is kpp %s\n",
                (directory / "build.kpp").string().c_str(), use.name.c_str(), library->minimumKpp.c_str(), kppVersion);
        return false;
      }
      std::vector<std::pair<std::string, std::string>> configs;
      for (const std::string& c : use.configs)
        configs.push_back({c, "use(\"" + use.name + "\") in " + user});
      if (same != found.end())
      {
        same->configs.insert(same->configs.end(), configs.begin(), configs.end());
        continue;
      }
      found.push_back({use.name, directory, user, library->options, configs});
      ProjectLibrary& used = libraries.emplace_back(ProjectLibrary{use.name, {}, library->unscoped});
      fs::path buildFile = directory / "build.kpp";
      for (const std::string& r : library->roots)
      {
        fs::path at = rooted(directory, r).lexically_normal();
        used.roots.push_back(at.string());
        // A root that holds the library's directory (`root(".")`, a converted library's) would take
        // its build.kpp for a source.
        fs::path inside = buildFile.lexically_relative(at);
        if (!inside.empty() && *inside.begin() != ".." &&
            std::find(used.excluded.begin(), used.excluded.end(), buildFile.string()) == used.excluded.end())
          used.excluded.push_back(buildFile.string());
      }
      if (!resolveUses(project, directory, library->uses, request, found, libraries))
        return false;
    }
    return true;
  }

  // Every executable's and library's uses (resolveUses), their roots added to its own.
  // Who set a config:: constant, for messages and `kpp options`.
  std::string setBy(const std::string& from, const Program& p)
  {
    if (from == "project") return "the project (project.config)";
    if (from == "configuration") return "the configuration " + p.configuration;
    if (from == "target") return "the target " + p.name;
    if (from == "cell") return "configure() for " + p.name + " " + p.configuration;
    return from;
  }

  // The options of the libraries a cell uses (used, from resolveUses), each with its value: its
  // default, then what use() gave, the project, the configuration, the target, configure(), and the
  // command line (`--config LIB::NAME=VALUE`), the later one winning. Each value is added to the
  // cell's constants as LIB::NAME, which the library reads as config::LIB::NAME. False with error
  // for an option nothing sets that has no default, a value of the wrong type or not among the
  // allowed ones, and a LIB::NAME no used library declares (allUsed: every library some cell
  // uses, whose options the project's constants may set for the other cells too).
  bool applyOptions(Program& p, const std::vector<Found>& used, const std::vector<std::string>& allUsed, const Request& request,
                    std::string& error)
  {
    std::vector<std::pair<std::string, std::string>> set; // LIB::NAME=VALUE, who set it; in order
    for (size_t i = 0; i < p.configs.size(); ++i)
      if (p.configs[i].find("::") != std::string::npos)
        set.push_back({p.configs[i], setBy(i < p.configFrom.size() ? p.configFrom[i] : "", p)});
    for (size_t i = 0; i + 1 < request.passed.size(); ++i)
      if (request.passed[i] == "--config" && request.passed[i + 1].find("::") != std::string::npos)
        set.push_back({request.passed[i + 1], "the command line (--config)"});
    std::string cell = p.name + " " + p.configuration;
    auto split = [](const std::string& item, std::string& name, std::string& value)
    {
      size_t equals = item.find('=');
      name = item.substr(0, equals);
      value = equals == std::string::npos ? "" : item.substr(equals + 1);
    };
    for (const auto& [item, by] : set)
    {
      std::string name, value;
      split(item, name, value);
      std::string library = name.substr(0, name.find("::")), option = name.substr(name.find("::") + 2);
      const Found* f = nullptr;
      for (const Found& candidate : used)
        if (candidate.name == library)
          f = &candidate;
      if (!f && std::find(allUsed.begin(), allUsed.end(), library) == allUsed.end())
      {
        error = name + " is set by " + by + ", and no library " + library + " is used";
        return false;
      }
      if (f && std::none_of(f->options.begin(), f->options.end(), [&](const Option& o) { return o.name == option; }))
      {
        std::string known;
        for (const Option& o : f->options)
          known += (known.empty() ? "" : ", ") + o.name;
        error = name + " is set by " + by + ", and the library " + library + " has no option " + option + " (its options: " +
                (known.empty() ? "none" : known) + ")";
        return false;
      }
    }
    for (const Found& f : used)
    {
      for (const auto& [item, by] : f.configs)
      {
        std::string name, value;
        split(item, name, value);
        if (std::none_of(f.options.begin(), f.options.end(), [&](const Option& o) { return o.name == name; }))
        {
          error = by + " sets " + name + ", and the library " + f.name + " has no option " + name;
          return false;
        }
      }
      for (const Option& o : f.options)
      {
        ProjectOption result{f.name, o.name, o.type, o.defaultValue, o.defaultValue, o.required ? "" : "the default", o.description, o.required, o.allowed};
        for (const auto& [item, by] : f.configs)
        {
          std::string name, value;
          split(item, name, value);
          if (name == o.name)
            result.value = value, result.setBy = by;
        }
        for (const auto& [item, by] : set)
        {
          std::string name, value;
          split(item, name, value);
          if (name == f.name + "::" + o.name)
            result.value = value, result.setBy = by;
        }
        std::string qualified = f.name + "::" + o.name;
        if (result.setBy.empty())
        {
          error = "the library " + f.name + " needs its option " + o.name + " (" + o.type + ": " + o.description + "), which nothing sets for " +
                  cell + "; set it with use(\"" + f.name + "\").config(\"" + o.name + "\", VALUE), config(\"" + qualified +
                  "\", VALUE) of the project, a configuration or a target, or kpp --config " + qualified + "=VALUE";
          return false;
        }
        char* end = nullptr;
        bool ok = o.type == "bool" ? result.value == "true" || result.value == "false"
                  : o.type == "int" ? (strtoll(result.value.c_str(), &end, 0), !result.value.empty() && end && *end == 0)
                                    : o.allowed.empty() || std::find(o.allowed.begin(), o.allowed.end(), result.value) != o.allowed.end();
        if (!ok)
        {
          std::string allowed;
          for (const std::string& a : o.allowed)
            allowed += (allowed.empty() ? "" : ", ") + a;
          error = qualified + " is " + result.value + " (set by " + result.setBy + "), which is not " +
                  (o.type == "bool" ? "true or false" : o.type == "int" ? "an int" : "one of its values: " + allowed);
          return false;
        }
        p.configs.push_back(qualified + "=" + result.value);
        p.configFrom.push_back("option");
        p.options.push_back(result);
      }
    }
    return true;
  }

  // Every executable's and library's uses (resolveUses), the libraries kept with it, and every
  // cell's library options (applyOptions); a library's are applied when its tests are built
  // (libraryProgram).
  bool resolveUses(const fs::path& root, const Request& request, Description& d)
  {
    std::vector<std::vector<Found>> founds;
    std::vector<std::string> allUsed;
    for (Program& p : d.programs)
    {
      std::vector<Found> found;
      if (!resolveUses(root, root, p.uses, request, found, p.kppLibraries))
        return false;
      for (const Found& f : found)
        if (std::find(allUsed.begin(), allUsed.end(), f.name) == allUsed.end())
          allUsed.push_back(f.name);
      founds.push_back(found);
    }
    for (KppLibrary& l : d.libraries)
    {
      std::vector<Found> found = {{l.name, fs::absolute(root).lexically_normal(), (root / "build.kpp").string(), l.options}};
      if (!resolveUses(root, root, l.uses, request, found, l.kppLibraries))
        return false;
      l.used = found;
    }
    for (size_t i = 0; i < d.programs.size(); ++i)
    {
      std::string error;
      if (!applyOptions(d.programs[i], founds[i], allUsed, request, error))
      {
        fprintf(stderr, "%s: error: %s\n", (root / "build.kpp").string().c_str(), error.c_str());
        return false;
      }
    }
    return true;
  }

  fs::path exeOf(const fs::path& root, const Program& p)
  {
    return root / "build" / p.configuration / (p.name + ".exe");
  }

  std::string withExe(std::string command, const fs::path& exe, const std::string& configuration)
  {
    for (auto [key, value] : {std::pair<std::string, std::string>{"{exe}", exe.string()}, {"{configuration}", configuration}})
      for (size_t at; (at = command.find(key)) != std::string::npos;)
        command.replace(at, key.size(), value);
    return command;
  }

  // kppc's arguments for one executable of one configuration.
  const CLibrary* findLibrary(const Description& d, const std::string& name)
  {
    for (const CLibrary& c : d.cLibraries)
      if (c.name == name)
        return &c;
    return nullptr;
  }

  fs::path libraryDirectory(const fs::path& root, const CLibrary& c) { return root / "build" / "clib" / c.name; }

  // What linking a C library takes: its prebuilt .lib files or the one kpp builds, and its .res files.
  std::vector<fs::path> libraryFiles(const fs::path& root, const CLibrary& c)
  {
    std::vector<fs::path> files;
    for (const std::string& lib : c.prebuilt)
      files.push_back(rooted(root, lib));
    if (c.prebuilt.empty() && !c.sources.empty())
      files.push_back(root / "build" / "clib" / (c.name + ".lib"));
    for (const std::string& rc : c.resources)
      files.push_back(libraryDirectory(root, c) / (fs::path(rc).stem().string() + ".res"));
    return files;
  }

  fs::file_time_type modified(const fs::path& path)
  {
    std::error_code ec;
    fs::file_time_type t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::max() : t;
  }

  // Whether object depends only on files older than itself: the source and the headers clang
  // named in its dependency file (-MD).
  bool upToDate(const fs::path& object, const fs::path& dependencies)
  {
    std::error_code ec;
    if (!fs::exists(object, ec) || !fs::exists(dependencies, ec))
      return false;
    fs::file_time_type built = fs::last_write_time(object, ec);
    std::string text = readText(dependencies);
    // A make rule: "target: dep dep \<newline> dep"; a blank inside a path is "\ ".
    size_t colon = text.find(": ");
    std::string word;
    auto check = [&]
    {
      bool fresh = word.empty() || modified(word) <= built;
      word.clear();
      return fresh;
    };
    for (size_t i = colon == std::string::npos ? text.size() : colon + 2; i < text.size(); ++i)
    {
      char c = text[i];
      if (c == '\\' && i + 1 < text.size() && text[i + 1] == ' ')
        word += ' ', ++i;
      else if (c == '\\' && i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r'))
        ;
      else if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
      {
        if (!check())
          return false;
      }
      else
        word += c;
    }
    return check();
  }

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
    std::string stamp;                    // what the objects are built with (flags.txt)
    bool gnu = false, archive = false;    // archive: the objects changed, or the library is not there
    std::unique_ptr<std::atomic<bool>[]> failed; // by command
  };

  bool prepareLibrary(const fs::path& root, const CLibrary& c, LibraryJob& job)
  {
    fs::path tools = fs::path(clangPath()).parent_path();
    job.library = &c;
    job.directory = libraryDirectory(root, c);
    std::error_code ec;
    fs::create_directories(job.directory, ec);
    if (c.prebuilt.empty() && !c.sources.empty())
    {
      std::vector<fs::path> sources;
      std::vector<fs::path> excluded;
      for (const std::string& e : c.excluded)
        excluded.push_back(rooted(root, e));
      for (const std::string& s : c.sources)
        for (const fs::path& file : collectFiles(rooted(root, s), {".c", ".cc", ".cpp"}))
          if (std::find(excluded.begin(), excluded.end(), fs::absolute(file).lexically_normal()) == excluded.end())
            sources.push_back(fs::absolute(file).lexically_normal());
      if (sources.empty())
      {
        fprintf(stderr, "error: the C library %s has no .c, .cc or .cpp file in its sources\n", c.name.c_str());
        return false;
      }
      // The flags of every object: Factorio's prebuilt libraries' (/O2 /MT, no assertions), the
      // library's includes and defines; no warnings, since the code is the library's own. The bundled
      // toolchain (llvm-mingw) builds Windows' GNU dialect (design.md 17): clang's own flags, its
      // MinGW-w64 headers and libc++; Visual Studio's clang is clang-cl, with MSVC's.
      bool gnu = job.gnu = bundledToolchain();
      std::string flags = gnu ? "-c -O2 -DNDEBUG -w" : "/nologo /c /O2 /MT /DNDEBUG /w";
      // The MSVC headers and libraries of clang's own Visual Studio: on its own, clang-cl takes the
      // newest installed, whose STL may require a newer clang (Build Tools 18's wants clang 20).
      fs::path installation = tools.parent_path().parent_path().parent_path().parent_path().parent_path();
      std::string version = gnu ? std::string() : readText(installation / "VC" / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt");
      while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' '))
        version.pop_back();
      if (!version.empty() && fs::is_directory(installation / "VC" / "Tools" / "MSVC" / version, ec))
        flags += " /vctoolsdir " + quoteArg((installation / "VC" / "Tools" / "MSVC" / version).string());
      for (const std::string& i : c.includes)
        flags += (gnu ? " -I" : " /I") + quoteArg(rooted(root, i).string());
      for (const std::string& d : c.defines)
        flags += (gnu ? " -D" : " /D") + quoteArg(d);
      std::string language = c.language.empty() ? std::string("c++17") : c.language;
      std::string cppFlags = gnu ? " -std=" + language : " /EHsc /std:" + language;
      // What the objects are built with: the flags and the compiler (another toolchain's are built again).
      job.stamp = flags + cppFlags + "\n" + clangPath();
      bool flagsChanged = readText(job.directory / "flags.txt") != job.stamp;
      for (size_t n = 0; n < sources.size(); ++n)
      {
        const fs::path& source = sources[n];
        std::string stem = source.lexically_relative(root).string();
        for (char& ch : stem)
          if (ch == '\\' || ch == '/' || ch == ':' || ch == '.')
            ch = '_';
        fs::path object = job.directory / (stem + ".obj"), dependencies = job.directory / (stem + ".d");
        job.objects.push_back(object);
        if (!flagsChanged && upToDate(object, dependencies))
          continue;
        bool cpp = source.extension() != ".c";
        job.logs.push_back(job.directory / (stem + ".log"));
        // clang-cl is clang in its cl mode (no clang-cl.exe to ship).
        if (gnu)
          job.commands.push_back("\"" + quoteArg(clangPath()) + " " + flags + (cpp ? cppFlags : "") + " -MD -MF" + quoteArg(dependencies.string()) +
                                 " -o " + quoteArg(object.string()) + " " + quoteArg(source.string()) + " > " + quoteArg(job.logs.back().string()) + " 2>&1\"");
        else
          job.commands.push_back("\"" + quoteArg(clangPath()) + " --driver-mode=cl " + flags + (cpp ? cppFlags : "") +
                                 " /clang:-MD /clang:-MF" + quoteArg(dependencies.string()) + " /Fo" + quoteArg(object.string()) +
                                 " " + quoteArg(source.string()) + " > " + quoteArg(job.logs.back().string()) + " 2>&1\"");
      }
      job.compiles = job.commands.size();
      job.lib = root / "build" / "clib" / (c.name + ".lib");
      job.archive = !job.commands.empty() || !fs::exists(job.lib, ec);
      if (job.archive)
        printf("kpp: %s: compiling %zu of %zu C/C++ files with %s (%s)\n", c.name.c_str(), job.commands.size(), sources.size(), clangPath().c_str(),
               gnu ? "the bundled toolchain, Windows' GNU dialect" : "clang-cl, MSVC's dialect");
    }
    for (const std::string& rc : c.resources)
    {
      fs::path script = rooted(root, rc), res = job.directory / (script.stem().string() + ".res");
      // A resource script names files next to it (icons, a manifest): rebuilt when any is newer.
      bool fresh = fs::exists(res, ec);
      for (const fs::directory_entry& entry : fs::directory_iterator(script.parent_path(), ec))
        if (entry.is_regular_file() && modified(entry.path()) > modified(res))
          fresh = false;
      if (fresh)
        continue;
      job.commands.push_back("\"cd /d " + quoteArg(script.parent_path().string()) + " && " + quoteArg((tools / "llvm-rc.exe").string()) +
                             " /nologo /fo " + quoteArg(res.string()) + " " + quoteArg(script.string()) + "\"");
      job.scripts.push_back(script);
    }
    job.failed.reset(new std::atomic<bool>[job.commands.size()]());
    return true;
  }

  // Every library's commands in one pool on all cores (none waits for another library), then each
  // one's errors and archive; false when one did not build.
  bool runLibraries(std::vector<LibraryJob>& jobs)
  {
    std::vector<std::pair<size_t, size_t>> work; // (job, command)
    for (size_t j = 0; j < jobs.size(); ++j)
      for (size_t k = 0; k < jobs[j].commands.size(); ++k)
        work.push_back({j, k});
    std::atomic<size_t> next = 0;
    std::vector<std::thread> threads;
    unsigned count = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned t = 0; t < count && t < work.size(); ++t)
      threads.emplace_back([&]
      {
        for (size_t i; (i = next++) < work.size();)
          if (system(jobs[work[i].first].commands[work[i].second].c_str()) != 0)
            jobs[work[i].first].failed[work[i].second] = true;
      });
    for (std::thread& t : threads)
      t.join();
    bool ok = true;
    std::error_code ec;
    for (LibraryJob& job : jobs)
    {
      const CLibrary& c = *job.library;
      for (const fs::path& log : job.logs)
      {
        std::string text = readText(log);
        if (!text.empty())
          fprintf(stderr, "%s", text.c_str());
      }
      bool failed = false;
      for (size_t k = 0; k < job.compiles; ++k)
        failed = failed || job.failed[k];
      for (size_t k = job.compiles; k < job.commands.size(); ++k)
        if (job.failed[k])
        {
          fprintf(stderr, "%s: error: llvm-rc could not compile it\n", job.scripts[k - job.compiles].string().c_str());
          ok = false;
        }
      if (failed)
      {
        fprintf(stderr, "error: the C library %s did not compile (above)\n", c.name.c_str());
        ok = false;
        continue;
      }
      if (!job.archive)
        continue;
      FILE* file = _wfopen((job.directory / "flags.txt").c_str(), L"wb");
      if (file)
        fwrite(job.stamp.data(), 1, job.stamp.size(), file), fclose(file);
      std::string response;
      for (const fs::path& object : job.objects)
        response += quoteArg(object.string()) + "\n";
      FILE* rsp = _wfopen((job.directory / "objects.rsp").c_str(), L"wb");
      if (rsp)
        fwrite(response.data(), 1, response.size(), rsp), fclose(rsp);
      // (llvm-mingw has llvm-ar alone, which lld's GNU driver links as it links llvm-lib's)
      fs::path tools = fs::path(clangPath()).parent_path();
      std::string archive = job.gnu ? "\"" + quoteArg((tools / "llvm-ar.exe").string()) + " rcs " + quoteArg(job.lib.string()) + " @" +
                                        quoteArg((job.directory / "objects.rsp").string()) + "\""
                                    : "\"" + quoteArg((tools / "llvm-lib.exe").string()) + " /nologo /out:" + quoteArg(job.lib.string()) + " @" +
                                        quoteArg((job.directory / "objects.rsp").string()) + "\"";
      if (job.gnu)
        fs::remove(job.lib, ec); // (llvm-ar adds to an archive that is there)
      if (system(archive.c_str()) != 0)
      {
        fprintf(stderr, "error: %s could not archive the C library %s\n", job.gnu ? "llvm-ar" : "llvm-lib", c.name.c_str());
        ok = false;
      }
    }
    return ok;
  }

  // A program's --toolchain (source.hpp's selectToolchain): the last of build.kpp's arguments and the
  // command line's, which come after them; empty for the default.
  std::string toolchainOf(const Program& p, const Request& request)
  {
    std::string toolchain;
    for (const std::vector<std::string>* list : {&p.arguments, &request.passed})
      for (size_t i = 0; i + 1 < list->size(); ++i)
        if ((*list)[i] == "--toolchain")
          toolchain = (*list)[i + 1];
    return toolchain;
  }

  // kppc's arguments for one cell: its configuration's options and constants (each config a --config,
  // in the order the project, the configuration, the target and configure() set them, so that the
  // later one wins), its own, and then the command line's (`kpp --config N=V` overrides them all).
  std::vector<std::string> buildArguments(const fs::path& root, const Description& d, const Program& p, const Request& request)
  {
    std::vector<std::string> args = {compilerPath().string()};
    if (request.msvc)
      args.push_back("--msvc-diagnostics");
    if (!request.verbose)
      args.push_back("--quiet");
    if (p.debugInfo)
      args.push_back("--debug-info");
    args.push_back(p.optimize ? "--optimize" : "--no-optimize");
    // An unoptimized build links incrementally with Visual Studio's link.exe; the bundled toolchain's
    // lld links whole.
    std::string toolchain = toolchainOf(p, request);
    if (!p.optimize && (toolchain.empty() ? !bundledToolchain() : toolchain != "bundled"))
      args.push_back("--incremental-link");
    for (const std::string& c : p.configs)
      args.push_back("--config"), args.push_back(c);
    std::string clangFlags;
    for (const std::string& f : p.clangFlags)
      clangFlags += (clangFlags.empty() ? "" : " ") + f;
    if (!clangFlags.empty())
      args.push_back("--clang-flags"), args.push_back(clangFlags);
    for (const std::string& a : p.arguments)
      args.push_back(a);
    for (const std::string& c : p.cIncludes)
      args.push_back("--cinclude"), args.push_back(rooted(root, c).string());
    for (const std::string& l : p.libraries)
      args.push_back("--lib"), args.push_back(l[0] == '-' ? l : rooted(root, l).string());
    for (const std::string& name : p.links)
      if (const CLibrary* c = findLibrary(d, name))
        for (const fs::path& file : libraryFiles(root, *c))
          args.push_back("--lib"), args.push_back(file.string());
    for (const std::string& e : p.excluded)
      args.push_back("--exclude"), args.push_back(rooted(root, e).string());
    // The kpp libraries it uses, root by root with their names and scopes (design.md 16), before --exe:
    // kpp --test takes what follows it as the sources.
    for (const ProjectLibrary& l : p.kppLibraries)
    {
      for (const std::string& r : l.roots)
        args.push_back(l.unscoped ? "--unscoped-library-root" : "--library-root"), args.push_back(l.name), args.push_back(r);
      for (const std::string& e : l.excluded)
        args.push_back("--exclude"), args.push_back(e);
    }
    args.insert(args.end(), request.passed.begin(), request.passed.end());
    args.push_back("--exe");
    args.push_back(exeOf(root, p).string());
    for (const std::string& r : p.roots)
      args.push_back(rooted(root, r).string());
    return args;
  }

  // kppc's arguments for a cell's build: buildArguments' with the link left to kpp (buildProgram). The
  // compile server of a build is the one of these arguments: --stop-server stops it by them too.
  std::vector<std::string> buildCommand(const fs::path& root, const Description& d, const Program& p, const Request& request)
  {
    std::vector<std::string> args = buildArguments(root, d, p, request);
    args.insert(args.begin() + 1, "--defer-link");
    return args;
  }

  int buildProgram(const fs::path& root, const Description& d, const Program& p, const Request& request, CompileFunction compile)
  {
    fs::path exe = exeOf(root, p);
    std::error_code ec;
    // The program's toolchain, its C libraries' too (a game that links Visual Studio's C++ libraries
    // asks for --toolchain msvc): one that is not there is said here, before anything compiles.
    if (std::string error = selectToolchain(compilerPath(), toolchainOf(p, request)); !error.empty())
    {
      fprintf(stderr, "error: %s (%s): %s\n", p.name.c_str(), p.configuration.c_str(), error.c_str());
      return 1;
    }
    fs::create_directories(exe.parent_path(), ec);
    if (!p.beforeBuild.empty() && system(withExe(p.beforeBuild, exe, p.configuration).c_str()) != 0)
    {
      fprintf(stderr, "error: %s's before-build command failed: %s\n", p.name.c_str(), p.beforeBuild.c_str());
      return 1;
    }
    using Clock = std::chrono::steady_clock;
    auto since = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
    auto start = Clock::now();
    std::vector<LibraryJob> jobs(p.links.size());
    for (size_t i = 0; i < p.links.size(); ++i)
    {
      const CLibrary* c = findLibrary(d, p.links[i]);
      if (!c)
      {
        fprintf(stderr, "error: %s links %s, which build.kpp declares no C library of\n", p.name.c_str(), p.links[i].c_str());
        return 1;
      }
      if (!prepareLibrary(root, *c, jobs[i]))
        return 1;
    }
    // The C libraries' files compile in one pool while kppc builds the program (its front end, code
    // generation and modules need no library); kppc leaves the link (--defer-link), which runs here
    // once both are done (docs/build.md, "The whole build").
    bool libraryWork = std::any_of(jobs.begin(), jobs.end(), [](const LibraryJob& j) { return j.archive || !j.commands.empty(); });
    bool librariesBuilt = true;
    double librarySeconds = 0;
    std::thread libraries;
    if (libraryWork)
      libraries = std::thread([&] { librariesBuilt = runLibraries(jobs); librarySeconds = since(start); });
    std::vector<std::string> args = buildCommand(root, d, p, request);
    auto kppStart = Clock::now();
    int code = invoke(args, request.server, compile);
    double kppSeconds = since(kppStart);
    if (libraries.joinable())
      libraries.join();
    if (!librariesBuilt)
      code = 1;
    // The link: kppc's command (it marks it pending when it built new objects), also when only a
    // C library changed (kppc replays an unchanged build, which does not link).
    double linkSeconds = 0;
    fs::path pending = exe.string() + ".link.pending", command = exe.string() + ".link.command";
    // Also when what the link writes is gone (the executable, a debug build's .pdb beside it, deleted
    // while the compile server kept the build): kppc's objects are all there (it replays a build only
    // then), so the link makes them again.
    fs::path pdb = fs::path(exe).replace_extension(".pdb");
    bool linked = fs::exists(exe, ec) && !(p.debugInfo && exe.extension() == ".exe" && !fs::exists(pdb, ec));
    bool linkPending = fs::exists(pending, ec) || (code == 0 && !linked);
    if (librariesBuilt && (linkPending || (code == 0 && libraryWork)) && fs::exists(command, ec))
    {
      auto linkStart = Clock::now();
      int rc = system(readText(command).c_str());
      linkSeconds = since(linkStart);
      if (rc != 0)
      {
        fprintf(stderr, "error: %s (%s): the link failed (exit %d)\n", p.name.c_str(), p.configuration.c_str(), rc);
        code = 1;
      }
      else
        fs::remove(pending, ec);
    }
    // Built means written: never "built" for an executable that is not there.
    if (code == 0 && !fs::exists(exe, ec))
    {
      fprintf(stderr, "error: %s (%s): %s was not written (kppc reported success; no link command to make it)\n", p.name.c_str(),
              p.configuration.c_str(), exe.string().c_str());
      code = 1;
    }
    double seconds = since(start);
    bool keepGoing = std::find(p.arguments.begin(), p.arguments.end(), "--keep-going") != p.arguments.end();
    char split[256];
    if (libraryWork)
      snprintf(split, sizeof split, "C libraries %.1f s alongside kpp %.1f s, link %.1f s", librarySeconds, kppSeconds, linkSeconds);
    else
      snprintf(split, sizeof split, "kpp %.1f s, link %.1f s", kppSeconds, linkSeconds);
    if (code == 0)
      printf("kpp: built %s (%s, total %.1f s: %s)\n", exe.string().c_str(), p.configuration.c_str(), seconds, split);
    else
      fprintf(stderr, "kpp: %s (%s) failed after %.1f s (%s)\n", p.name.c_str(), p.configuration.c_str(), seconds, split);
    if (!p.afterBuild.empty() && (code == 0 || (keepGoing && fs::exists(exe, ec))) &&
        system(withExe(p.afterBuild, exe, p.configuration).c_str()) != 0)
    {
      fprintf(stderr, "error: %s's after-build command failed: %s\n", p.name.c_str(), p.afterBuild.c_str());
      return 1;
    }
    return code;
  }

  // What Run and the debugger start (Executable::debug): the program, its arguments and its directory,
  // {exe} and {configuration} expanded as in the build steps (a configuration's own copy of the game,
  // bin\Releasex64\factorio.exe).
  struct Launch
  {
    fs::path program, directory;
    std::string arguments;
  };
  Launch launchOf(const fs::path& root, const Program& p)
  {
    fs::path exe = exeOf(root, p);
    Launch l;
    l.program = p.debugCommand.empty() ? exe : rooted(root, withExe(p.debugCommand, exe, p.configuration));
    l.directory = p.debugDirectory.empty() ? root : rooted(root, withExe(p.debugDirectory, exe, p.configuration));
    l.arguments = withExe(p.debugArguments, exe, p.configuration);
    return l;
  }

  int runProgram(const fs::path& root, const Program& p, const std::vector<std::string>& extra)
  {
    Launch l = launchOf(root, p);
    std::string command = "cd /d " + quoteArg(l.directory.string()) + " && " + quoteArg(l.program.string());
    if (!l.arguments.empty())
      command += " " + l.arguments;
    for (const std::string& a : extra)
      command += " " + quoteArg(a);
    return system(("\"" + command + "\"").c_str());
  }

  // A library's program in a configuration: the configuration's options, the library's roots and
  // test roots as the program's own (inside the library its names are bare, its tests' too), the
  // libraries it uses, and its own options and its libraries' (applyOptions). False, the error
  // printed, when an option is wrong.
  bool libraryProgram(const fs::path& root, const Configuration& c, const KppLibrary& l, const Request& request, Program& p)
  {
    static_cast<Configuration&>(p) = c;
    p.name = l.name;
    p.configuration = c.name;
    p.roots = l.roots;
    p.roots.insert(p.roots.end(), l.testRoots.begin(), l.testRoots.end());
    p.kppLibraries = l.kppLibraries;
    std::vector<std::string> allUsed;
    for (const Found& f : l.used)
      allUsed.push_back(f.name);
    std::string error;
    if (applyOptions(p, l.used, allUsed, request, error))
      return true;
    fprintf(stderr, "%s: error: %s\n", (root / "build.kpp").string().c_str(), error.c_str());
    return false;
  }

  // kpp --test: the tests of the executables and libraries asked for (all when none is named) in
  // one configuration, each built and run by kppc --test (run.hpp) with its cell's options and roots,
  // a library's test roots too, and the runner's arguments (after `--`). Every one runs; 1 when one
  // failed.
  int runTests(const fs::path& root, const Description& d, const Configuration& configuration, const std::vector<std::string>& targets,
               const Request& request)
  {
    std::vector<Program> chosen;
    auto asked = [&](const std::string& name)
    { return targets.empty() || std::find(targets.begin(), targets.end(), name) != targets.end(); };
    for (const Program& p : d.programs)
      if (p.configuration == configuration.name && asked(p.name))
        chosen.push_back(p);
    for (const KppLibrary& l : d.libraries)
      if (asked(l.name) && !libraryProgram(root, configuration, l, request, chosen.emplace_back()))
        return 1;
    int result = 0;
    for (const Program& p : chosen)
    {
      std::vector<std::string> args = buildArguments(root, d, p, request);
      size_t exe = std::find(args.begin(), args.end(), "--exe") - args.begin();
      std::vector<std::string> roots(args.begin() + std::min(exe + 2, args.size()), args.end());
      args.resize(exe);
      args.erase(std::remove(args.begin(), args.end(), "--incremental-link"), args.end());
      if (!request.server)
        args.push_back("--no-server");
      size_t runIndex = args.size();
      args.push_back("--test");
      args.insert(args.end(), roots.begin(), roots.end());
      args.push_back("--");
      args.insert(args.end(), request.runArguments.begin(), request.runArguments.end());
      std::vector<char*> argv;
      for (std::string& a : args)
        argv.push_back(a.data());
      argv.push_back(nullptr);
      printf("kpp: testing %s (%s)\n", p.name.c_str(), p.configuration.c_str());
      fflush(stdout);
      if (::runProgram(int(args.size()), argv.data(), int(runIndex)) != 0)
        result = 1;
    }
    return result;
  }

  // Visual Studio's name of a configuration: its own with the first letter capital (Debug, Fastdebug).
  std::string visualStudioName(std::string name)
  {
    if (!name.empty() && name[0] >= 'a' && name[0] <= 'z')
      name[0] = char(name[0] - 'a' + 'A');
    return name;
  }

  // One project per target, one solution configuration per named configuration: building builds
  // that cell.
  int generate(const fs::path& root, const Description& d, const Request& request)
  {
    fs::path self = compilerPath();
    std::vector<IdeTarget> targets;
    for (const std::string& name : targetNames(d))
    {
      IdeTarget target;
      target.name = name;
      bool first = true;
      for (const Configuration& configuration : d.configurations)
      {
        const Program* q = findCell(d, name, configuration.name);
        if (!q)
          continue;
        if (first)
        {
          for (const std::string& r : q->roots)
            target.roots.push_back(rooted(root, r));
          for (const ProjectLibrary& l : q->kppLibraries)
          {
            for (const std::string& r : l.roots)
              target.roots.push_back(rooted(root, r));
            for (const std::string& e : l.excluded)
              target.excluded.push_back(rooted(root, e));
          }
          for (const std::string& e : q->excluded)
            target.excluded.push_back(rooted(root, e));
          first = false;
        }
        IdeConfiguration c;
        c.visualStudioName = visualStudioName(configuration.name);
        c.name = configuration.name;
        c.exe = exeOf(root, *q);
        // The IDE builds through kpp itself, so that an edit of build.kpp needs no new files.
        c.buildCommand = "cd /d " + quoteArg(root.string()) + " && " + quoteArg(self.string()) + " " + quoteArg(name) + " " +
                         quoteArg(configuration.name) + " --msvc-diagnostics";
        for (const auto& [setting, value] : request.settings)
          c.buildCommand += " --set " + quoteArg(setting + "=" + value);
        Launch l = launchOf(root, *q);
        c.program = l.program;
        c.arguments = l.arguments;
        c.directory = l.directory;
        target.configurations.push_back(c);
      }
      targets.push_back(target);
    }
    fs::path stdlib = defaultStdlib(self);
    return writeIdeFiles(request.visualStudio, request.vsCode, root.filename().string(), root, targets, stdlib,
                         {".cpp", ".hpp", ".h", ".kpp"}) ? 0 : 1;
  }

  int newProject(const fs::path& directory)
  {
    std::error_code ec;
    if (fs::exists(directory / "build.kpp", ec))
    {
      fprintf(stderr, "error: %s already has a build.kpp\n", directory.string().c_str());
      return 1;
    }
    fs::create_directories(directory / "src", ec);
    auto write = [&](const fs::path& path, const std::string& text)
    {
      if (fs::exists(path, ec))
        return;
      FILE* file = _wfopen(path.c_str(), L"wb");
      if (file)
        fwrite(text.data(), 1, text.size(), file), fclose(file);
      printf("wrote %s\n", path.string().c_str());
    };
    write(directory / "build.kpp",
          "// How this project is built (https://github.com/kovarex/kpp/blob/master/docs/build.md, \"build.kpp\").\n"
          "// As it is, src\\ is one executable named after this directory, in debug and release.\n"
          "void build(Project& project)\n"
          "{\n"
          "}\n");
    write(directory / "src" / "main.kpp",
          "int main()\n"
          "{\n"
          "  println(\"Hello from kpp\");\n"
          "  return 0;\n"
          "}\n");
    write(directory / ".gitignore", "build/\n.vscode/\n");
    return 0;
  }

  std::string ask(const char* prompt)
  {
    printf("%s", prompt);
    fflush(stdout);
    char line[256] = {};
    if (!fgets(line, sizeof(line), stdin))
      return {};
    std::string answer = line;
    while (!answer.empty() && (answer.back() == '\n' || answer.back() == '\r' || answer.back() == ' '))
      answer.pop_back();
    return answer;
  }

  int act(const fs::path& root, const Request& request, CompileFunction compile)
  {
    auto commandStart = std::chrono::steady_clock::now(); // (the whole command's time: build.kpp's description too)
    // --stop-server: the compile servers of build.kpp's description and of every cell's builds. The
    // description's first, and the description then computed without a server, so that a build.kpp
    // that no longer describes stops its server too.
    if (request.stop)
    {
      invoke(describeArguments(root, request), true, compile, true);
      Request serverless = request;
      serverless.server = false;
      Description d;
      if (!describe(root, serverless, d))
        return 1;
      for (const Program& p : d.programs)
        invoke(buildCommand(root, d, p, request), true, compile, true);
      return 0;
    }
    Description d;
    if (!describe(root, request, d) || !resolveUses(root, request, d))
      return 1;
    if (request.visualStudio || request.vsCode)
      return generate(root, d, request);
    // The names: each a configuration's or a target's (a library's, for --test).
    std::vector<std::string> configurations = request.configurations, targets;
    for (const std::string& name : request.names)
      if (findConfiguration(d, name))
        configurations.push_back(name);
      else if (std::any_of(d.programs.begin(), d.programs.end(), [&](const Program& p) { return p.name == name; }) ||
               (request.test && std::any_of(d.libraries.begin(), d.libraries.end(), [&](const KppLibrary& l) { return l.name == name; })))
        targets.push_back(name);
      else
      {
        std::string known;
        for (const Configuration& c : d.configurations)
          known += " " + c.name;
        fprintf(stderr, "error: build.kpp describes no target or configuration %s (its configurations:%s)\n", name.c_str(), known.c_str());
        return 2;
      }
    if (configurations.empty())
      configurations.push_back("debug");
    if (request.test)
    {
      int result = 0;
      for (const std::string& name : configurations)
        result = std::max(result, runTests(root, d, *findConfiguration(d, name), targets, request));
      return result;
    }
    std::vector<const Program*> chosen;
    for (const std::string& configuration : configurations)
      for (const Program& p : d.programs)
        if (p.configuration == configuration && (targets.empty() || std::find(targets.begin(), targets.end(), p.name) != targets.end()))
          chosen.push_back(&p);
    // kpp options: each chosen cell's libraries, scoped or unscoped, and their options, with their
    // values and who set them (a library project: its own and its libraries', in each configuration).
    if (request.options)
    {
      std::vector<Program> listed;
      for (const Program* p : chosen)
        listed.push_back(*p);
      if (d.programs.empty())
        for (const std::string& configuration : configurations)
          for (const KppLibrary& l : d.libraries)
            if (!libraryProgram(root, *findConfiguration(d, configuration), l, request, listed.emplace_back()))
              return 1;
      for (const Program& p : listed)
      {
        // The library this project is (its own tests' program), then the libraries the cell uses.
        std::vector<ProjectLibrary> libraries = p.kppLibraries;
        for (const KppLibrary& l : d.libraries)
          if (d.programs.empty() && l.name == p.name)
            libraries.insert(libraries.begin(), ProjectLibrary{l.name, {}, l.unscoped});
        printf("%s %s:%s\n", p.name.c_str(), p.configuration.c_str(), libraries.empty() ? " no libraries" : "");
        for (const ProjectLibrary& l : libraries)
          printf(l.unscoped ? "  library %s, unscoped: its names are bare\n" : "  library %s, scoped: %s::NAME\n", l.name.c_str(),
                 l.name.c_str());
        for (const ProjectOption& o : p.options)
        {
          std::string values;
          for (const std::string& a : o.allowed)
            values += (values.empty() ? " (" : ", ") + a;
          printf("  %s::%s = %s  %s%s, %s; set by %s\n    %s\n", o.library.c_str(), o.name.c_str(), o.value.c_str(), o.type.c_str(),
                 values.empty() ? "" : (values + ")").c_str(), o.required ? "required" : ("default " + o.defaultValue).c_str(),
                 o.setBy.c_str(), o.description.c_str());
        }
      }
      return 0;
    }
    if (request.run && chosen.size() != 1)
    {
      fprintf(stderr, "error: --run needs one target in one configuration; this names %zu cells\n", chosen.size());
      return 2;
    }
    if (chosen.empty())
      printf("kpp: build.kpp describes no executable; kpp --test builds and runs its library's tests\n");
    for (const Program* p : chosen)
      if (int code = buildProgram(root, d, *p, request, compile))
        return code;
    // The whole command (build.kpp's description, every cell, its C libraries and links), to compare
    // with another build system's.
    if (chosen.size() > 1)
      printf("kpp: built %zu programs, total %.1f s\n", chosen.size(),
             std::chrono::duration<double>(std::chrono::steady_clock::now() - commandStart).count());
    return request.run ? runProgram(root, *chosen[0], request.runArguments) : 0;
  }

  // `kpp` alone in a console: the actions, each with the command that does it.
  int menu(const fs::path& root, CompileFunction compile)
  {
    if (root.empty())
    {
      fs::path here = fs::current_path();
      printf("kpp: no build.kpp in %s or above it.\n"
             "  1  make a new project here (build.kpp, src\\main.kpp)   kpp new\n"
             "  0  quit\n", here.string().c_str());
      return ask("> ") == "1" ? newProject(here) : 0;
    }
    Request request;
    Description d;
    if (!describe(root, request, d))
      return 1;
    std::vector<std::string> targets = targetNames(d);
    std::string first = targets.empty() ? "" : targets[0];
    printf("kpp: project %s (%s)\n  targets:", root.filename().string().c_str(), root.string().c_str());
    for (const std::string& t : targets)
      printf(" %s", t.c_str());
    printf("\n  configurations:");
    for (const Configuration& c : d.configurations)
      printf(" %s", c.name.c_str());
    printf("\n"
           "  1  build, debug                     kpp debug\n"
           "  2  build, release                   kpp release\n"
           "  3  build and run %-18s kpp %s --run\n"
           "  4  write Visual Studio files        kpp --generate-visual-studio-files\n"
           "  5  write VS Code files              kpp --generate-vs-code-files\n"
           "  0  quit\n", first.c_str(), first.c_str());
    std::string answer = ask("> ");
    if (answer == "1" || answer == "2")
      request.configurations = {answer == "1" ? "debug" : "release"};
    else if (answer == "3")
      request.run = true, request.names = {first};
    else if (answer == "4")
      request.visualStudio = true;
    else if (answer == "5")
      request.vsCode = true;
    else
      return 0;
    return act(root, request, compile);
  }
}

int runProject(int argc, char** argv, CompileFunction compile)
{
  if (argc >= 2 && strcmp(argv[1], "new") == 0)
    return newProject(argc >= 3 ? fs::absolute(argv[2]) : fs::current_path());
  bool options = argc >= 2 && strcmp(argv[1], "options") == 0; // kpp options [targets] [configurations]
  if (options)
    --argc, ++argv;
  fs::path root = findProject(fs::current_path());
  if (argc == 1 && !options)
  {
    if (!_isatty(_fileno(stdin)))
      return -1;
    if (!root.empty())
      SetCurrentDirectoryW(root.c_str());
    return menu(root, compile);
  }
  if (root.empty() && options)
  {
    fprintf(stderr, "error: kpp options lists a project's library options, and there is no build.kpp here or above\n");
    return 2;
  }
  if (root.empty())
    return -1;
  // A project's command line names targets and configurations, never sources: an argument that is
  // an existing file or directory makes it a plain build of those sources.
  Request request;
  request.options = options;
  bool runner = false; // after --test's `--`: the test runner's arguments
  std::string badOption; // the first argument that is no option of kpp's or kppc's, or lacks its value
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
      request.run = true;
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
      // kppc's options go on to each cell's build with their values; any other is an error, never
      // passed on or ignored (`kpp --version` among others built the project). The error waits for
      // the end: a source among the arguments makes them a plain build's (`kppc --server ... build.kpp`).
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
  SetCurrentDirectoryW(root.c_str());
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
        cells.push_back({target, c.name, visualStudioName(c.name), p->options, p->kppLibraries});
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
