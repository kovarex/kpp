// build.kpp's description and the libraries it uses: describe() (build.kpp run once, its description
// cached), the uses and their options, the config declarations of a library's sources.
// (Moved out of project.cpp on 2026-11-05 without a change.)
#include "Project/Internal.hpp"

namespace projects
{
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

  // text into path, its directory made first; false, having said which file and why, when it cannot be
  // written (an installed kpp's libraries are under C:\Program Files\kpp, read-only for most users:
  // describe() wrote their cache there and failed with no word, 2026-11-05).
  bool writeText(const fs::path& path, const std::string& text, const char* what)
  {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
    {
      fprintf(stderr, "error: kpp cannot make %s for %s: %s\n", path.parent_path().string().c_str(), what, ec.message().c_str());
      return false;
    }
    FILE* file = host::openFile(path, "wb");
    if (!file)
    {
      fprintf(stderr, "error: kpp cannot write %s (%s): %s\n", path.string().c_str(), what, strerror(errno));
      return false;
    }
    bool written = fwrite(text.data(), 1, text.size(), file) == text.size();
    if (fclose(file) != 0 || !written)
    {
      fprintf(stderr, "error: kpp could not write all of %s (%s)\n", path.string().c_str(), what);
      return false;
    }
    return true;
  }

  std::vector<FileDeclarations> configDeclarationsOf(const std::vector<fs::path>& files, const std::vector<FileStamp>& stamps,
                                                     const fs::path& cache)
  {
    auto split = [](const std::string& line)
    {
      std::vector<std::string> fields;
      for (size_t from = 0;;)
      {
        size_t tab = line.find('\t', from);
        fields.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
        if (tab == std::string::npos)
          return fields;
        from = tab + 1;
      }
    };
    // The cache: `file PATH TIME SIZE`, then its `declaration NAME TYPE KIND VALUE REQUIRED LINE COLUMN
    // DESCRIPTION` and `error LINE COLUMN MESSAGE` lines, tab-separated.
    std::map<std::string, std::pair<FileStamp, FileDeclarations>> kept;
    {
      std::string text = readText(cache), line;
      std::pair<FileStamp, FileDeclarations>* current = nullptr;
      for (size_t from = 0; from < text.size();)
      {
        size_t end = text.find('\n', from);
        line = text.substr(from, end == std::string::npos ? std::string::npos : end - from);
        from = end == std::string::npos ? text.size() : end + 1;
        std::vector<std::string> f = split(line);
        if (f[0] == "file" && f.size() == 4)
          current = &kept[f[1]], current->first = {strtoll(f[2].c_str(), nullptr, 10), strtoull(f[3].c_str(), nullptr, 10)};
        else if (current && f[0] == "declaration" && f.size() == 9)
          current->second.declared.push_back({f[1], f[2], f[3], f[4], f[5] == "1", f[8], uint32_t(atoi(f[6].c_str())), uint32_t(atoi(f[7].c_str()))});
        else if (current && f[0] == "error" && f.size() == 4)
          current->second.errors.push_back({uint32_t(atoi(f[1].c_str())), uint32_t(atoi(f[2].c_str())), f[3]});
      }
    }
    std::vector<FileDeclarations> out(files.size());
    bool changed = kept.size() != files.size();
    std::string written;
    for (size_t i = 0; i < files.size(); ++i)
    {
      auto k = kept.find(files[i].string());
      if (k != kept.end() && i < stamps.size() && k->second.first == stamps[i])
        out[i] = k->second.second;
      else
      {
        findConfigDeclarations(readText(files[i]), out[i].declared, out[i].errors);
        changed = true;
      }
      bool keepable = i < stamps.size();
      std::string entry = "file\t" + files[i].string() + "\t" + (keepable ? std::to_string(stamps[i].time) + "\t" + std::to_string(stamps[i].size) : "") + "\n";
      for (const ConfigDeclaration& d : out[i].declared)
      {
        for (const std::string* field : {&d.name, &d.type, &d.kind, &d.value, &d.description})
          keepable = keepable && field->find_first_of("\t\r\n") == std::string::npos;
        entry += "declaration\t" + d.name + "\t" + d.type + "\t" + d.kind + "\t" + d.value + "\t" + (d.required ? "1" : "0") + "\t" +
                 std::to_string(d.line) + "\t" + std::to_string(d.column) + "\t" + d.description + "\n";
      }
      for (const ConfigError& e : out[i].errors)
      {
        keepable = keepable && e.message.find_first_of("\t\r\n") == std::string::npos;
        entry += "error\t" + std::to_string(e.line) + "\t" + std::to_string(e.column) + "\t" + e.message + "\n";
      }
      if (keepable)
        written += entry;
      else
        changed = true;
    }
    // A cache: one not written is read again next time, and the declarations are right either way, so
    // a failure is a warning (writeText's line says which file), not a failed build.
    if (changed && !writeText(cache, written, "a cache of the config:: declarations"))
      fprintf(stderr, "warning: the config:: declarations are read from the sources again next time\n");
    return out;
  }

  // The file compiled with build.kpp and projectlib: the command line's settings and the project's
  // name, which Project::setting and describe() read.
  std::string settingsSource(const fs::path& root, const Request& request)
  {
    std::string s = "// Generated by kpp from the command line's settings: compiled with build.kpp and projectlib.\n"
                    "std::StringView kppSetting(std::StringView name, std::StringView otherwise)\n{\n";
    for (const auto& [name, value] : request.settings)
      s += "  if (name == " + kppString(name) + ")\n    return " + kppString(value) + ";\n";
    // The command line's --target, its last one (Cell::platform, where it overrides build.kpp's own
    // --target arguments): "" without one.
    std::string target;
    for (size_t i = 0; i + 1 < request.passed.size(); ++i)
      if (request.passed[i] == "--target")
        target = request.passed[i + 1];
    s += "  return otherwise;\n}\n"
         "std::StringView kppProjectName() { return " + kppString(root.filename().string()) + "; }\n"
         "std::StringView kppCommandLineTarget() { return " + kppString(target) + "; }\n"
         // (the default target, kpp's hostTarget: Cell::platform without any --target)
         "std::StringView kppHostTarget() { return " + kppString(host::platformName()) + "; }\n";
    return s;
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

  const Program* findUnavailableCell(const Description& d, const std::string& target, const std::string& configuration)
  {
    for (const Program& p : d.unavailable)
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
    FILE* pipe = host::openPipe(command, "rb");
    if (!pipe)
      return -1;
    char buffer[4096];
    for (size_t n; (n = fread(buffer, 1, sizeof(buffer), pipe)) > 0;)
      out.append(buffer, n);
    return host::closePipe(pipe);
  }

  // kpp on build.kpp, projectlib and the settings: the interpreter runs build() once and
  // describe(), which runs configure() for every cell (--run-build; projectlib's Project has no
  // configuration, so the name `matrix` is not used), and prints the description. Through the
  // compile server unless --no-server, so that the standard library is not checked again for every
  // build.
  std::vector<std::string> describeArguments(const fs::path& root, const Request& request, const fs::path& cache)
  {
    fs::path self = compilerPath();
    std::vector<std::string> args = {self.string(), "--quiet", "--run-build", "matrix"};
    if (request.msvc)
      args.push_back("--msvc-diagnostics");
    for (const fs::path& a : {defaultStdlib(self).parent_path() / "projectlib", root / "build.kpp", descriptionCache(root, cache) / "settings.kpp"})
      args.push_back(a.string());
    return args;
  }

  fs::path descriptionCache(const fs::path& root, const fs::path& cache) { return cache.empty() ? root / "build" / "kpp" : cache; }

  // Whether path is directory or under it, after the links and junctions of both are followed (a
  // root that is a junction to elsewhere is elsewhere) and with Windows' letter case.
  bool isInside(const fs::path& directory, const fs::path& path)
  {
    std::error_code ec, ec2;
    fs::path d = fs::weakly_canonical(fs::absolute(directory, ec), ec);
    fs::path p = fs::weakly_canonical(fs::absolute(path, ec2), ec2);
    if (ec || ec2)
      return false;
    fs::path relative = p.lexically_relative(d);
    if (relative.empty())
      return false;
    for (const fs::path& part : relative)
      if (part == "..")
        return false;
    return true;
  }

  // A system library's name as Library::systemLibrary and Executable::systemLibrary give it: kpp
  // passes it as -lNAME, so it is a name the linker searches for (ssl, gdi32, libssl.a after a ':'),
  // never a path (/, \, ..) nor something an option could be read from (a leading -, a blank, a quote).
  bool isSystemLibraryName(const std::string& name)
  {
    size_t start = !name.empty() && name[0] == ':' ? 1 : 0;
    if (name.size() == start || name[start] == '-' || name.find("..") != std::string::npos)
      return false;
    return std::all_of(name.begin() + start, name.end(),
                       [](char c) { return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '+' || c == '-'; });
  }

  std::string describeCommand(const fs::path& root, const Request& request, const fs::path& cache)
  {
    std::vector<std::string> args = describeArguments(root, request, cache);
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
  // The cache (describe.txt, its key, settings.kpp, the config:: declarations) is in cache, else in
  // root's build\kpp: a used library's is in the using project's build\kpp\uses\NAME (resolveUses),
  // never in the library's directory, which may be read-only (an installed kpp's libraries).
  bool describe(const fs::path& root, const Request& request, Description& out, const fs::path& cache)
  {
    fs::path self = compilerPath();
    fs::path library = defaultStdlib(self).parent_path() / "projectlib";
    fs::path directory = descriptionCache(root, cache);
    std::error_code ec;
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
      std::string what = "the description of " + (root / "build.kpp").string();
      if (!writeText(directory / "settings.kpp", source, what.c_str()))
        return false;
      std::string text;
      if (runAndRead(describeCommand(root, request, cache), text) != 0 || text.find("configuration ") == std::string::npos)
      {
        fprintf(stderr, "%s: error: build.kpp did not describe the build (above)\n", (root / "build.kpp").string().c_str());
        return false;
      }
      // describe.txt first, its key last: a key is never kept beside a description it is not the key of.
      if (!writeText(described, text, what.c_str()) || !writeText(directory / "describe.key", keyText, what.c_str()))
        return false;
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
    // What a build.kpp may point kpp at (docs/design.md, "What a library's build.kpp can do"): a
    // library's roots and test roots are inside its directory, and a system library is a name the
    // linker searches for, never a path or an option. Checked before any file of a root is read.
    for (const KppLibrary& l : out.libraries)
      for (const std::vector<std::string>* list : {&l.roots, &l.testRoots})
        for (const std::string& r : *list)
          if (!isInside(root, rooted(root, r)))
          {
            fprintf(stderr, "%s: error: the library %s's root %s is outside its directory %s: a library's roots and test roots are inside it\n",
                    file.c_str(), l.name.c_str(), r.c_str(), fs::absolute(root, ec).lexically_normal().string().c_str());
            out.errors.push_back(r);
          }
    std::vector<const std::vector<std::string>*> systemLists;
    for (const Program& p : out.programs)
      systemLists.push_back(&p.systemLibraries);
    for (const KppLibrary& l : out.libraries)
      systemLists.push_back(&l.systemLibraries);
    for (const std::vector<std::string>* list : systemLists)
      for (const std::string& s : *list)
      {
        std::string name = s.find(' ') == std::string::npos ? s : s.substr(s.find(' ') + 1);
        if (!isSystemLibraryName(name))
        {
          fprintf(stderr, "%s: error: the system library \"%s\" is not a library's name (letters, digits, _ . + -, not starting with -, "
                          "optionally after one :), which the linker would take as a path or an option\n",
                  file.c_str(), name.c_str());
          out.errors.push_back(name);
        }
      }
    if (!out.errors.empty())
      return false;
    // A library's config:: constants declared in its source (`config i32 level = 6;`, config.cpp;
    // design.md, "config:: defaults in source") are its options as option()'s are: set by its users,
    // checked, listed by `kpp options` with the default. One name declared both ways is an error.
    for (KppLibrary& l : out.libraries)
    {
      std::vector<fs::path> files;
      std::vector<FileStamp> stamps;
      for (const std::string& r : l.roots)
      {
        std::vector<FileStamp> rootStamps;
        std::vector<fs::path> rootFiles = collectFiles((fs::path(r).is_absolute() ? fs::path(r) : root / r).lexically_normal(), {".kpp"}, &rootStamps);
        for (size_t i = 0; i < rootFiles.size(); ++i)
          if (rootFiles[i].lexically_normal() != (root / "build.kpp").lexically_normal())
            files.push_back(rootFiles[i]), stamps.push_back(i < rootStamps.size() ? rootStamps[i] : FileStamp{});
      }
      std::vector<FileDeclarations> found = configDeclarationsOf(files, stamps, directory / ("config-" + l.name + ".txt"));
      for (size_t f = 0; f < files.size(); ++f)
        {
          const fs::path& path = files[f];
          const std::vector<ConfigDeclaration>& declared = found[f].declared;
          const std::vector<ConfigError>& errors = found[f].errors;
          for (const ConfigError& e : errors)
          {
            fprintf(stderr, "%s: error: %s\n", sourceLocation(path.string(), e.line, e.column).c_str(), e.message.c_str());
            out.errors.push_back(e.message);
          }
          for (const ConfigDeclaration& d : declared)
          {
            std::string at = sourceLocation(path.string(), d.line);
            auto same = std::find_if(l.options.begin(), l.options.end(), [&](const Option& o) { return o.name == d.name; });
            if (same != l.options.end() && !same->declaredAt.empty())
            {
              if (same->defaultValue != d.value || same->required != d.required || same->declaredType != d.type)
              {
                fprintf(stderr, "%s: error: config::%s::%s is declared again, differently, at %s\n", at.c_str(), l.name.c_str(),
                        d.name.c_str(), same->declaredAt.c_str());
                out.errors.push_back(d.name);
              }
              continue;
            }
            if (same != l.options.end())
            {
              fprintf(stderr, "%s: error: config::%s::%s is declared here and by build.kpp's option(\"%s\"): one declaration per name\n",
                      at.c_str(), l.name.c_str(), d.name.c_str(), d.name.c_str());
              out.errors.push_back(d.name);
              continue;
            }
            Option o;
            o.name = d.name;
            o.type = d.kind == "enum" ? "string" : d.kind; // kpp checks the value against its type (an enumerator's name, an integer's range)
            o.declaredType = d.type;
            o.declaredAt = at;
            o.defaultValue = d.value;
            o.required = d.required;
            o.description = (d.description.empty() ? "config " + d.type : d.description) + " (" + at + ")";
            l.options.push_back(o);
          }
        }
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
  // libraries of one libraries/ directory use each other), or else among kpp's own (defaultLibraries:
  // the release's libraries/, a checkout's for a build of kpp), then described (the same settings),
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
      fs::path looked = directory;
      if (use.path.empty() && !fs::is_regular_file(directory / "build.kpp", ec) && by.parent_path().filename() == "libraries")
        directory = by.parent_path() / use.name;
      fs::path official = defaultLibraries(compilerPath()) / use.name;
      if (use.path.empty() && !fs::is_regular_file(directory / "build.kpp", ec) && fs::is_regular_file(official / "build.kpp", ec))
        directory = official;
      directory = fs::absolute(directory, ec).lexically_normal();
      std::string user = (by / "build.kpp").string();
      // A used library's use(NAME, local(PATH)) names a library inside the directory that holds it,
      // its own or a sibling's (minizip's "../zlib"); the project's own build.kpp may name any
      // (docs/design.md, "What a library's build.kpp can do").
      if (!use.path.empty() && !fs::equivalent(by, project, ec) && !isInside(by.parent_path(), directory))
      {
        fprintf(stderr, "%s: error: the library %s is used from %s, outside %s: a used library's use() paths stay in the directory that holds it\n",
                user.c_str(), use.name.c_str(), directory.string().c_str(), fs::absolute(by.parent_path(), ec).lexically_normal().string().c_str());
        return false;
      }
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
          fprintf(stderr, "%s: error: the library %s is neither in %s nor among kpp's own libraries (%s); there is no registry yet\n",
                  user.c_str(), use.name.c_str(), fs::absolute(looked, ec).lexically_normal().string().c_str(),
                  defaultLibraries(compilerPath()).string().c_str());
        else
          fprintf(stderr, "%s: error: the library %s: %s has no build.kpp\n", user.c_str(), use.name.c_str(), directory.string().c_str());
        return false;
      }
      Request serverless = request; // no compile server left running in a library for its description
      serverless.server = false;
      Description d;
      // Its description kept in the using project's build (an installed kpp's libraries, under
      // C:\Program Files\kpp, are read-only; one name is one directory, as checked above).
      if (!describe(directory, serverless, d, project / "build" / "kpp" / "uses" / use.name))
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
      // minimum-kpp against kpp's own version (docs/build.md, "Versions"; 0.2.N+dirty is 0.2.N with
      // changes). It was a constant, 0.1.0, which refused every library needing 0.2 (2026-11-03). A
      // kpp built outside a git checkout has no version, and the check is an error, not a guess.
      std::string own = ::kppVersion();
      own = own.substr(0, own.find('+'));
      if (!library->minimumKpp.empty() && !versionNumbers(own, kpp))
      {
        fprintf(stderr, "%s: error: the library %s needs kpp %s or later, and this kpp's version is unknown (built outside a git checkout)\n",
                (directory / "build.kpp").string().c_str(), use.name.c_str(), library->minimumKpp.c_str());
        return false;
      }
      if (!library->minimumKpp.empty() && (!versionNumbers(library->minimumKpp, need) || kpp < need))
      {
        fprintf(stderr, "%s: error: the library %s needs kpp %s or later, and this is kpp %s\n",
                (directory / "build.kpp").string().c_str(), use.name.c_str(), library->minimumKpp.c_str(), ::kppVersion());
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
      used.systemLibraries = library->systemLibraries;
      used.thisReference = library->thisReference;
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
        ProjectOption result{f.name, o.name, o.declaredType.empty() ? o.type : o.declaredType, o.defaultValue, o.defaultValue, o.required ? "" : "the default", o.description, o.required, o.allowed};
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

  // kpp's arguments for one executable of one configuration.
  const CLibrary* findLibrary(const Description& d, const std::string& name)
  {
    for (const CLibrary& c : d.cLibraries)
      if (c.name == name)
        return &c;
    return nullptr;
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
    p.systemLibraries = l.systemLibraries; // its own, which its tests link as its users do
    p.thisReference = l.thisReference;
    std::vector<std::string> allUsed;
    for (const Found& f : l.used)
      allUsed.push_back(f.name);
    std::string error;
    if (applyOptions(p, l.used, allUsed, request, error))
      return true;
    fprintf(stderr, "%s: error: %s\n", (root / "build.kpp").string().c_str(), error.c_str());
    return false;
  }
}
