// describe.txt, the description build.kpp's describe() writes (projectlib/Project.kpp), read into a
// Description: one table of its keys, each with the sections it may stand in and what it sets
// (2026-11-05, thread J's refactoring list: a new build.kpp setting is one entry here). A key the
// table does not have, or has for other sections, is an error naming it: projectlib and kpp
// disagree, which until then dropped the setting silently.
#include "Project/Internal.hpp"

namespace projects
{
  namespace
  {
    // Where a line stands: after which section's first line (`configuration NAME`, `cell TARGET
    // CONFIGURATION`, `clibrary NAME`, `kpp-library NAME`, `measure`), until `end` or the next one.
    enum Section : unsigned
    {
      None = 1,
      InConfiguration = 2,
      InProgram = 4,
      InCLibrary = 8,
      InLibrary = 16,
      InMeasure = 32,
      Anywhere = 63,
    };

    struct Reading
    {
      Description& d;
      Section section = None;
      std::string error; // set by an entry: the line is wrong (a part before its whole)
    };

    // The configuration or the cell a line of either sets (a cell is a configuration of its own).
    Configuration& configurationOf(Reading& r) { return r.section == InProgram ? r.d.programs.back() : r.d.configurations.back(); }
    std::vector<Use>& usesOf(Reading& r) { return r.section == InProgram ? r.d.programs.back().uses : r.d.libraries.back().uses; }
    template <typename T> T* lastOf(std::vector<T>& list, Reading& r, const char* whole)
    {
      if (list.empty())
        r.error = std::string("before any `") + whole + "` line";
      return list.empty() ? nullptr : &list.back();
    }

    struct Key
    {
      const char* key;
      unsigned sections;
      void (*apply)(Reading& r, const std::string& value);
    };

    const Key keys[] = {
      // The sections and what is everywhere.
      {"error", Anywhere, [](Reading& r, const std::string& v) { r.d.errors.push_back(v); }},
      {"configuration", Anywhere, [](Reading& r, const std::string& v) { r.d.configurations.push_back({v}), r.section = InConfiguration; }},
      {"cell", Anywhere,
       [](Reading& r, const std::string& v)
       {
         size_t blank = v.find(' ');
         Program p;
         p.name = v.substr(0, blank);
         p.configuration = blank == std::string::npos ? "" : v.substr(blank + 1);
         r.d.programs.push_back(p), r.section = InProgram;
       }},
      {"clibrary", Anywhere, [](Reading& r, const std::string& v) { r.d.cLibraries.push_back({v}), r.section = InCLibrary; }},
      // (not "library": an executable's link file is that key)
      {"kpp-library", Anywhere, [](Reading& r, const std::string& v) { r.d.libraries.push_back({v}), r.section = InLibrary; }},
      {"measure", Anywhere, [](Reading& r, const std::string&) { r.section = InMeasure; }},
      {"end", Anywhere, [](Reading& r, const std::string&) { r.section = None; }},
      // A configuration's and a cell's.
      {"optimize", InConfiguration | InProgram, [](Reading& r, const std::string& v) { configurationOf(r).optimize = v == "1"; }},
      {"debug-info", InConfiguration | InProgram, [](Reading& r, const std::string& v) { configurationOf(r).debugInfo = v == "1"; }},
      {"config", InConfiguration | InProgram, // `config SOURCE NAME=VALUE`
       [](Reading& r, const std::string& v)
       {
         size_t blank = v.find(' ');
         configurationOf(r).configFrom.push_back(v.substr(0, blank));
         configurationOf(r).configs.push_back(blank == std::string::npos ? "" : v.substr(blank + 1));
       }},
      {"clang-flag", InConfiguration | InProgram, [](Reading& r, const std::string& v) { configurationOf(r).clangFlags.push_back(v); }},
      {"c-flag", InConfiguration | InProgram, [](Reading& r, const std::string& v) { configurationOf(r).cFlags.push_back(v); }},
      {"argument", InConfiguration | InProgram, [](Reading& r, const std::string& v) { configurationOf(r).arguments.push_back(v); }},
      // A cell's and a kpp library's uses.
      {"use", InProgram | InLibrary, [](Reading& r, const std::string& v) { usesOf(r).push_back({v}); }},
      {"use-minimum", InProgram | InLibrary, [](Reading& r, const std::string& v) { if (Use* u = lastOf(usesOf(r), r, "use")) u->minimum = v; }},
      {"use-path", InProgram | InLibrary, [](Reading& r, const std::string& v) { if (Use* u = lastOf(usesOf(r), r, "use")) u->path = v; }},
      {"use-config", InProgram | InLibrary, [](Reading& r, const std::string& v) { if (Use* u = lastOf(usesOf(r), r, "use")) u->configs.push_back(v); }},
      // A cell's.
      {"root", InProgram | InLibrary,
       [](Reading& r, const std::string& v) { (r.section == InProgram ? r.d.programs.back().roots : r.d.libraries.back().roots).push_back(v); }},
      {"exclude", InProgram | InCLibrary,
       [](Reading& r, const std::string& v) { (r.section == InProgram ? r.d.programs.back().excluded : r.d.cLibraries.back().excluded).push_back(v); }},
      {"library", InProgram, [](Reading& r, const std::string& v) { r.d.programs.back().libraries.push_back(v); }},
      {"cinclude", InProgram, [](Reading& r, const std::string& v) { r.d.programs.back().cIncludes.push_back(v); }},
      {"before-build", InProgram, [](Reading& r, const std::string& v) { r.d.programs.back().beforeBuild = v; }},
      {"platform", InProgram, [](Reading& r, const std::string& v) { r.d.programs.back().platform = v; }},
      {"after-build", InProgram, [](Reading& r, const std::string& v) { r.d.programs.back().afterBuild = v; }},
      {"debug-command", InProgram, [](Reading& r, const std::string& v) { r.d.programs.back().debugCommand = v; }},
      {"debug-arguments", InProgram, [](Reading& r, const std::string& v) { r.d.programs.back().debugArguments = v; }},
      {"debug-directory", InProgram, [](Reading& r, const std::string& v) { r.d.programs.back().debugDirectory = v; }},
      {"link", InProgram, [](Reading& r, const std::string& v) { r.d.programs.back().links.push_back(v); }},
      {"natvis", InProgram, [](Reading& r, const std::string& v) { r.d.programs.back().natvis.push_back(v); }},
      {"system-library", InProgram | InLibrary,
       [](Reading& r, const std::string& v)
       { (r.section == InProgram ? r.d.programs.back().systemLibraries : r.d.libraries.back().systemLibraries).push_back(v); }},
      // A kpp library's.
      {"version", InLibrary, [](Reading& r, const std::string& v) { r.d.libraries.back().version = v; }},
      {"license", InLibrary, [](Reading& r, const std::string& v) { r.d.libraries.back().license = v; }},
      {"minimum-kpp", InLibrary, [](Reading& r, const std::string& v) { r.d.libraries.back().minimumKpp = v; }},
      {"unscoped", InLibrary, [](Reading& r, const std::string& v) { r.d.libraries.back().unscoped = v == "1"; }},
      {"this-reference", InLibrary, [](Reading& r, const std::string& v) { r.d.libraries.back().thisReference = v == "1"; }},
      {"test-root", InLibrary, [](Reading& r, const std::string& v) { r.d.libraries.back().testRoots.push_back(v); }},
      {"option", InLibrary, [](Reading& r, const std::string& v) { r.d.libraries.back().options.push_back({v}); }},
      {"option-type", InLibrary, [](Reading& r, const std::string& v) { if (Option* o = lastOf(r.d.libraries.back().options, r, "option")) o->type = v; }},
      {"option-default", InLibrary,
       [](Reading& r, const std::string& v) { if (Option* o = lastOf(r.d.libraries.back().options, r, "option")) o->defaultValue = v; }},
      {"option-required", InLibrary, [](Reading& r, const std::string&) { if (Option* o = lastOf(r.d.libraries.back().options, r, "option")) o->required = true; }},
      {"option-value", InLibrary,
       [](Reading& r, const std::string& v) { if (Option* o = lastOf(r.d.libraries.back().options, r, "option")) o->allowed.push_back(v); }},
      {"option-description", InLibrary,
       [](Reading& r, const std::string& v) { if (Option* o = lastOf(r.d.libraries.back().options, r, "option")) o->description = v; }},
      // A C library's.
      {"source", InCLibrary, [](Reading& r, const std::string& v) { r.d.cLibraries.back().sources.push_back(v); }},
      {"include", InCLibrary, [](Reading& r, const std::string& v) { r.d.cLibraries.back().includes.push_back(v); }},
      {"define", InCLibrary, [](Reading& r, const std::string& v) { r.d.cLibraries.back().defines.push_back(v); }},
      {"prebuilt", InCLibrary, [](Reading& r, const std::string& v) { r.d.cLibraries.back().prebuilt.push_back(v); }},
      {"resource", InCLibrary, [](Reading& r, const std::string& v) { r.d.cLibraries.back().resources.push_back(v); }},
      {"language", InCLibrary, [](Reading& r, const std::string& v) { r.d.cLibraries.back().language = v; }},
      {"precompiled-header", InCLibrary, [](Reading& r, const std::string& v) { r.d.cLibraries.back().precompiled = v; }},
      // kpp measure's (build.kpp's measure()).
      {"measure-target", InMeasure, [](Reading& r, const std::string& v) { r.d.measurement.target = v; }},
      {"measure-configuration", InMeasure, [](Reading& r, const std::string& v) { r.d.measurement.configuration = v; }},
      {"edit", InMeasure, [](Reading& r, const std::string& v) { r.d.measurement.edits.push_back({v}); }},
      {"edit-file", InMeasure, [](Reading& r, const std::string& v) { if (auto* e = lastOf(r.d.measurement.edits, r, "edit")) e->file = v; }},
      {"edit-from", InMeasure, [](Reading& r, const std::string& v) { if (auto* e = lastOf(r.d.measurement.edits, r, "edit")) e->from = v; }},
      {"edit-to", InMeasure, [](Reading& r, const std::string& v) { if (auto* e = lastOf(r.d.measurement.edits, r, "edit")) e->to = v; }},
      {"compare", InMeasure, [](Reading& r, const std::string& v) { r.d.measurement.compares.push_back({v}); }},
      {"compare-command", InMeasure,
       [](Reading& r, const std::string& v) { if (auto* c = lastOf(r.d.measurement.compares, r, "compare")) c->command = v; }},
      {"compare-touch", InMeasure, [](Reading& r, const std::string& v) { if (auto* c = lastOf(r.d.measurement.compares, r, "compare")) c->touch = v; }},
    };

    const char* sectionName(Section s)
    {
      return s == InConfiguration ? "a configuration" : s == InProgram ? "a cell" : s == InCLibrary ? "a C library" : s == InLibrary ? "a kpp library"
           : s == InMeasure ? "kpp measure's" : "no section";
    }
  }

  Description parse(const std::string& text)
  {
    Description d;
    Reading r{d};
    size_t pos = 0;
    for (size_t number = 1; pos < text.size(); ++number)
    {
      size_t end = text.find('\n', pos);
      if (end == std::string::npos)
        end = text.size();
      std::string line = text.substr(pos, end - pos);
      pos = end + 1;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty())
        continue;
      size_t space = line.find(' ');
      std::string key = line.substr(0, space), value = space == std::string::npos ? "" : line.substr(space + 1);
      const Key* k = nullptr;
      for (const Key& candidate : keys)
        if (key == candidate.key)
          k = &candidate;
      if (!k || !(k->sections & r.section))
      {
        d.errors.push_back("build\\kpp\\describe.txt line " + std::to_string(number) + ": `" + key + "` " +
                           (k ? std::string("in ") + sectionName(r.section) + ", where it has no meaning" : std::string("is no key kpp knows")) +
                           " (projectlib/Project.kpp writes what this kpp does not read: are they one release's?)");
        continue;
      }
      k->apply(r, value);
      if (!r.error.empty())
      {
        d.errors.push_back("build\\kpp\\describe.txt line " + std::to_string(number) + ": `" + key + "` " + r.error);
        r.error.clear();
      }
    }
    return d;
  }
}
