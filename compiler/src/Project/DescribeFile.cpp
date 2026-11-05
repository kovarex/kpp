// describe.txt, the description build.kpp's describe() writes (projectlib/Project.kpp), read into a Description.
// (Moved out of project.cpp on 2026-11-05 without a change.)
#include "Project/Internal.hpp"

namespace projects
{
  Description parse(const std::string& text)
  {
    Description d;
    enum { none, configuration, program, cLibrary, library, measure } section = none;
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
      else if (key == "measure")
        section = measure;
      else if (key == "end")
        section = none;
      else if (section == measure)
      {
        Measurement& m = d.measurement;
        if (key == "measure-target") m.target = value;
        else if (key == "measure-configuration") m.configuration = value;
        else if (key == "edit") m.edits.push_back({value});
        else if (key == "compare") m.compares.push_back({value});
        else if (key == "edit-file" && !m.edits.empty()) m.edits.back().file = value;
        else if (key == "edit-from" && !m.edits.empty()) m.edits.back().from = value;
        else if (key == "edit-to" && !m.edits.empty()) m.edits.back().to = value;
        else if (key == "compare-command" && !m.compares.empty()) m.compares.back().command = value;
        else if (key == "compare-touch" && !m.compares.empty()) m.compares.back().touch = value;
      }
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
               (key == "optimize" || key == "debug-info" || key == "config" || key == "clang-flag" || key == "c-flag" || key == "argument"))
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
        else if (key == "c-flag") c.cFlags.push_back(value);
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
        else if (key == "system-library") l.systemLibraries.push_back(value);
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
        else if (key == "system-library") p.systemLibraries.push_back(value);
      }
    }
    return d;
  }
}
