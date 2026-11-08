// The project files of an IDE: Visual Studio's solution and VS Code's (generate.cpp writes them).
// (Moved out of project.cpp on 2026-11-05 without a change.)
#include "Project/Internal.hpp"

namespace projects
{
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
          // A used library's files under Libraries\NAME (its roots' own names below that when it has
          // several), as the stdlib's are under Standard Library: each root's last directory alone
          // put gorating's src and every library's src into one folder (G, 2026-11-05).
          target.folders.resize(target.roots.size());
          for (const ProjectLibrary& l : q->kppLibraries)
          {
            for (const std::string& r : l.roots)
            {
              fs::path at = rooted(root, r).lexically_normal();
              std::string own = at.has_filename() ? at.filename().string() : at.parent_path().filename().string();
              target.roots.push_back(at);
              target.folders.push_back("Libraries\\" + l.name + (l.roots.size() > 1 ? "\\" + own : std::string()));
            }
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
        c.exe = exeOf(root, *q, request);
        // The IDE builds through kpp itself, so that an edit of build.kpp needs no new files.
        c.buildCommand = "cd /d " + quoteArg(root.string()) + " && " + quoteArg(self.string()) + " " + quoteArg(name) + " " +
                         quoteArg(configuration.name) + " --msvc-diagnostics";
        c.buildProgram = {self.string(), name, configuration.name, "--msvc-diagnostics"};
        c.buildDirectory = root;
        for (const auto& [setting, value] : request.settings)
        {
          c.buildCommand += " --set " + quoteArg(setting + "=" + value);
          c.buildProgram.push_back("--set");
          c.buildProgram.push_back(setting + "=" + value);
        }
        // (Rebuild: kpp's own --rebuild, the cell's kpp objects, incremental state and compile server gone
        // first, its C libraries' objects kept; Clean: that state gone alone, kpp's --clean)
        c.rebuildCommand = c.buildCommand + " --rebuild";
        c.cleanCommand = c.buildCommand + " --clean";
        Launch l = launchOf(root, *q, request);
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
}
