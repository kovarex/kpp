// What kpp does itself around a cell's build, so that build.kpp starts no process (docs/build.md, "Build
// information" and "Steps after the build"; design.md, "What build.kpp can run"): the buildInfo:: constants,
// read from git and the project's files, and the after-build links and files, inside the project's directory.
// And, below them, the commands a project runs itself: its command steps (2026-11-24).
#include "Project/Internal.hpp"
#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace projects
{
  namespace
  {
#ifdef _WIN32
    constexpr bool kppHostWindows = true;
#else
    constexpr bool kppHostWindows = false;
#endif

    std::string trimmed(std::string text)
    {
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
      size_t start = 0;
      while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
        ++start;
      return text.substr(start);
    }

    bool isHash(const std::string& text)
    {
      if (text.size() != 40 && text.size() != 64)
        return false;
      for (char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
          return false;
      return true;
    }

    bool isDigits(const std::string& text)
    {
      if (text.empty() || text.size() > 18)
        return false;
      for (char c : text)
        if (c < '0' || c > '9')
          return false;
      return true;
    }

    bool isIdentifier(const std::string& text)
    {
      if (text.empty() || (text[0] >= '0' && text[0] <= '9'))
        return false;
      for (char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
          return false;
      return true;
    }

    // path absolute and normal, the links and junctions of its existing part followed.
    fs::path resolved(const fs::path& path)
    {
      std::error_code ec;
      fs::path absolute = fs::absolute(path, ec).lexically_normal();
      fs::path canonical = fs::weakly_canonical(absolute, ec);
      return ec ? absolute : canonical;
    }

    // A project's git directories: its own (a worktree's .git file names it) and the common one (its
    // commondir), found at the project's directory or above it.
    struct GitDirectories
    {
      fs::path git, common;
    };

    bool findGitDirectories(const fs::path& root, GitDirectories& dirs)
    {
      std::error_code ec;
      for (fs::path at = resolved(root);; at = at.parent_path())
      {
        fs::path dotGit = at / ".git";
        if (fs::is_directory(dotGit, ec))
        {
          dirs.git = dotGit;
          break;
        }
        if (fs::is_regular_file(dotGit, ec))
        {
          std::string text = trimmed(readText(dotGit));
          if (text.rfind("gitdir: ", 0) != 0)
            return false;
          fs::path dir = trimmed(text.substr(8));
          dirs.git = dir.is_absolute() ? dir : at / dir;
          break;
        }
        if (at == at.parent_path())
          return false;
      }
      dirs.common = dirs.git;
      if (fs::is_regular_file(dirs.git / "commondir", ec))
      {
        fs::path dir = trimmed(readText(dirs.git / "commondir"));
        dirs.common = dir.is_absolute() ? dir : (dirs.git / dir).lexically_normal();
      }
      return true;
    }

    // HEAD's commit read from git's files (HEAD, the ref it names, packed-refs): "" when kpp cannot tell
    // that way, and git is asked.
    std::string headCommit(const GitDirectories& dirs)
    {
      std::string head = trimmed(readText(dirs.git / "HEAD"));
      if (head.rfind("ref: ", 0) != 0)
        return isHash(head) ? head : "";
      std::string ref = head.substr(5);
      if (ref.rfind("refs/", 0) != 0 || ref.find("..") != std::string::npos)
        return "";
      for (const fs::path& dir : {dirs.git, dirs.common})
        if (std::string commit = trimmed(readText(dir / ref)); isHash(commit))
          return commit;
      std::string packed = readText(dirs.common / "packed-refs");
      for (size_t pos = 0; pos < packed.size();)
      {
        size_t end = std::min(packed.find('\n', pos), packed.size());
        std::string line = trimmed(packed.substr(pos, end - pos));
        pos = end + 1;
        size_t blank = line.find(' ');
        if (blank != std::string::npos && line.substr(blank + 1) == ref && isHash(line.substr(0, blank)))
          return line.substr(0, blank);
      }
      return "";
    }

    // git, found on PATH (its absolute directories only, so that a git.exe in the project or the current
    // directory is never the one run), or where Git for Windows installs it. Empty when there is none.
    fs::path gitProgram()
    {
      const char* name = kppHostWindows ? "git.exe" : "git";
      const char separator = kppHostWindows ? ';' : ':';
      std::error_code ec;
      if (const char* path = getenv("PATH"))
        for (std::string rest = path; !rest.empty();)
        {
          size_t end = std::min(rest.find(separator), rest.size());
          std::string dir = rest.substr(0, end);
          rest = end < rest.size() ? rest.substr(end + 1) : std::string();
          if (!dir.empty() && dir.front() == '"' && dir.back() == '"' && dir.size() >= 2)
            dir = dir.substr(1, dir.size() - 2);
          if (dir.empty() || !fs::path(dir).is_absolute())
            continue;
          fs::path candidate = fs::path(dir) / name;
          if (fs::is_regular_file(candidate, ec))
            return candidate;
        }
      if (kppHostWindows)
        for (const char* candidate : {"C:\\Program Files\\Git\\cmd\\git.exe", "C:\\Program Files\\Git\\bin\\git.exe"})
          if (fs::is_regular_file(candidate, ec))
            return candidate;
      return {};
    }

    // git with arguments in root, its output trimmed into out: false, with error set, when it fails. The
    // repository's file-system monitor hook is switched off: no program the repository names runs.
    // Its output goes to a file beside the BuildInfo.kpp written (host::run passes a process's output
    // through to this one's otherwise).
    bool runGit(const fs::path& git, const fs::path& root, const fs::path& scratch, const std::vector<std::string>& arguments, std::string& out,
                std::string& error)
    {
      std::vector<std::string> args = {"-C", root.string(), "-c", "core.fsmonitor=false"};
      args.insert(args.end(), arguments.begin(), arguments.end());
      std::error_code ec;
      fs::create_directories(scratch.parent_path(), ec);
      fs::remove(scratch, ec);
      std::string why;
      host::Run how;
      how.appendTo = scratch;
      int code = host::run(git.string(), args, how, why);
      std::string output = trimmed(readText(scratch));
      fs::remove(scratch, ec);
      if (code != 0)
      {
        std::string command = "git";
        for (const std::string& a : arguments)
          command += " " + a;
        error = command + (code == -1 ? " could not be started: " + why : " failed: " + output);
        return false;
      }
      out = output;
      return true;
    }

    // A C string literal's text of a project file's: its trailing blanks and line ends left out; false
    // for a control character other than a line end or a tab.
    bool literalOf(const std::string& text, std::string& out)
    {
      out.clear();
      for (char c : trimmed(text))
      {
        if (c == '\\' || c == '"')
          out += '\\', out += c;
        else if (c == '\n')
          out += "\\n";
        else if (c == '\r')
          continue;
        else if (c == '\t')
          out += "\\t";
        else if ((unsigned char)c < 0x20)
          return false;
        else
          out += c;
      }
      return true;
    }

    // The key line of a written file: `// kpp key: HEAD SINCE COUNT` (SINCE "-" for none).
    const char* keyPrefix = "// kpp key: ";
  }

  bool insideProject(const fs::path& root, const fs::path& path)
  {
    fs::path base = resolved(root), at = resolved(path);
    auto b = base.begin(), a = at.begin();
    for (; b != base.end(); ++b, ++a)
      if (a == at.end() || *a != *b)
        return false;
    return true;
  }

  fs::path buildInfoFile(const fs::path& root, const Program& p) { return root / "build" / "kpp" / "buildinfo" / p.name / "BuildInfo.kpp"; }

  bool writeBuildInfo(const fs::path& root, const Program& p, std::string& error)
  {
    const BuildInfoWanted& info = p.buildInfo;
    fs::path file = buildInfoFile(root, p);
    std::string what = p.name + "'s buildInfo()";
    std::string number, revision, key;
    if (!info.number.empty())
    {
      if (!isDigits(info.number))
      {
        error = what + ": the given number " + info.number + " is not a number of digits";
        return false;
      }
      number = info.number;
    }
    else
    {
      if (!isDigits(info.base))
      {
        error = what + ": countSince's base " + info.base + " is not a number of digits";
        return false;
      }
      if (!info.since.empty() && !isHash(info.since))
      {
        error = what + ": countSince's commit " + info.since + " is not a commit's whole hash (40 hexadecimal digits)";
        return false;
      }
      GitDirectories dirs;
      if (!findGitDirectories(root, dirs))
      {
        error = what + ": " + root.string() + " is in no git repository; without one, give the number: buildInfo().given(...)";
        return false;
      }
      std::error_code ec;
      if (fs::exists(dirs.common / "shallow", ec))
      {
        error = what + ": " + root.string() + " is a shallow clone, whose count of commits is not the history's: git fetch --unshallow --filter=tree:0 origin";
        return false;
      }
      fs::path git = gitProgram();
      std::string since = info.since.empty() ? "-" : info.since;
      revision = headCommit(dirs);
      // An unchanged HEAD keeps the count the file has: no git runs (git rev-list --count of Factorio's
      // 88,000 commits: 130 ms, 5 s with a cold disk cache).
      std::string old = readText(file);
      if (!revision.empty() && old.rfind(keyPrefix, 0) == 0)
      {
        std::string line = old.substr(strlen(keyPrefix), old.find('\n') - strlen(keyPrefix));
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        std::string prefix = revision + " " + since + " ";
        if (line.rfind(prefix, 0) == 0 && isDigits(line.substr(prefix.size())))
          number = line.substr(prefix.size());
      }
      if (number.empty())
      {
        if (git.empty())
        {
          error = what + ": no git found on PATH (nor in C:\\Program Files\\Git); without git, give the number: buildInfo().given(...)";
          return false;
        }
        fs::path gitOutput = file.parent_path() / ("git." + std::to_string(host::processId()) + ".txt");
        if (revision.empty() && !runGit(git, root, gitOutput, {"rev-parse", "HEAD"}, revision, error))
        {
          error = what + ": " + error;
          return false;
        }
        std::string count;
        if (!runGit(git, root, gitOutput, {"rev-list", "--count", info.since.empty() ? std::string("HEAD") : info.since + "..HEAD"}, count, error) || !isDigits(count))
        {
          error = what + ": " + (error.empty() ? "git rev-list gave " + count : error) +
                  (info.since.empty() ? "" : " (is " + info.since + " in this repository's history?)");
          return false;
        }
        number = count;
      }
      key = std::string(keyPrefix) + revision + " " + since + " " + number + "\n";
      number = std::to_string(std::stoll(number) + std::stoll(info.base));
    }
    std::string text = "// Written by kpp from build.kpp's buildInfo() (docs/build.md, \"Build information\"), again when HEAD moves.\n";
    text += key;
    text += "namespace buildInfo\n{\n";
    text += "  const i64 number = " + number + ";\n";
    text += "  const auto revision = \"" + revision + "\";\n";
    for (const std::string& t : info.texts)
    {
      size_t equals = t.find('=');
      std::string name = t.substr(0, equals), path = equals == std::string::npos ? "" : t.substr(equals + 1);
      if (!isIdentifier(name) || name == "number" || name == "revision")
      {
        error = what + ": text(" + name + ", ...) needs a name of letters, digits and _, other than number and revision";
        return false;
      }
      fs::path source = rooted(root, path);
      std::error_code ec;
      if (fs::path(path).is_absolute() || !insideProject(root, source) || !fs::is_regular_file(source, ec))
      {
        error = what + ": text(" + name + ", " + path + "): " +
                (!fs::path(path).is_absolute() && insideProject(root, source) ? "no such file in the project" : "the file must be in the project's directory");
        return false;
      }
      std::string literal;
      if (!literalOf(readText(source), literal))
      {
        error = what + ": text(" + name + ", " + path + "): the file holds a control character";
        return false;
      }
      text += "  const auto " + name + " = \"" + literal + "\";\n";
    }
    text += "}\n";
    if (readText(file) == text)
      return true;
    // Written to a name of this process's and moved into place: Visual Studio builds two targets at once,
    // and a reader never sees half a file.
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path temporary = file;
    temporary += "." + std::to_string(host::processId()) + ".tmp";
    if (!writeText(temporary, text, "buildInfo()"))
    {
      error = what + ": " + temporary.string() + " could not be written";
      return false;
    }
    for (int attempt = 0; attempt < 20; ++attempt)
    {
      fs::rename(temporary, file, ec);
      if (!ec || readText(file) == text)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    fs::remove(temporary, ec);
    if (readText(file) != text)
    {
      error = what + ": " + file.string() + " could not be written";
      return false;
    }
    return true;
  }

  bool runSteps(const fs::path& root, const Program& p, const fs::path& exe)
  {
    for (const BuildStep& step : p.steps)
    {
      fs::path from = rooted(root, withExe(step.from, exe, p.configuration)), to = rooted(root, withExe(step.to, exe, p.configuration));
      std::string name = p.name + "'s after-build step (" + step.kind + " " + (step.from.empty() ? "" : step.from + " ") + step.to + ")";
      if (step.to.empty() || !insideProject(root, to) || (step.kind != "write-if-missing" && !insideProject(root, from)))
      {
        fprintf(stderr, "error: %s: its files must be in the project's directory, %s\n", name.c_str(), root.string().c_str());
        return false;
      }
      std::error_code ec;
      if (step.kind == "link" || step.kind == "link-if-there")
      {
        if (!fs::is_regular_file(from, ec))
        {
          if (step.kind == "link-if-there")
            continue;
          fprintf(stderr, "error: %s: %s does not exist\n", name.c_str(), from.string().c_str());
          return false;
        }
        fs::create_directories(to.parent_path(), ec);
        fs::remove(to, ec);
        fs::create_hard_link(from, to, ec);
        if (ec)
        {
          std::string why = ec.message();
          fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
          if (ec)
          {
            fprintf(stderr, "error: %s: neither a hard link nor a copy could be made: %s\n", name.c_str(), ec.message().c_str());
            return false;
          }
          printf("kpp: no hard link possible (%s), copied %s to %s\n", why.c_str(), from.string().c_str(), to.string().c_str());
        }
      }
      else if (step.kind == "write-if-missing")
      {
        if (fs::exists(to, ec))
          continue;
        std::string text;
        for (const std::string& line : step.lines)
          text += line + "\n";
        fs::path temporary = to;
        temporary += "." + std::to_string(host::processId()) + ".tmp";
        if (!writeText(temporary, text, name.c_str()))
          return false;
        // (two builds at once: the other's file, with the same text, may be there first)
        fs::rename(temporary, to, ec);
        fs::remove(temporary, ec);
        if (!fs::exists(to, ec))
        {
          fprintf(stderr, "error: %s: %s could not be written\n", name.c_str(), to.string().c_str());
          return false;
        }
      }
      else
      {
        fprintf(stderr, "error: %s: no step kpp knows (projectlib and kpp of different releases?)\n", name.c_str());
        return false;
      }
    }
    return true;
  }

  // Command steps (Executable::step; docs/build.md, "Command steps"; design.md, "What build.kpp can run",
  // kovarex 2026-11-24): commands of the project being built, each run when it is out of date.
  namespace
  {
    bool stepNameFits(const std::string& name)
    {
      if (name.empty() || name[0] == '.')
        return false;
      for (char c : name)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == '.'))
          return false;
      return true;
    }

    // A path of the file system as two steps name it alike: absolute, normal, in Windows' lower case.
    std::string pathKey(const fs::path& path)
    {
      std::error_code ec;
      std::string key = fs::absolute(path, ec).lexically_normal().generic_string();
      if (kppHostWindows)
        for (char& c : key)
          c = char(tolower((unsigned char)c));
      while (key.size() > 1 && key.back() == '/')
        key.pop_back();
      return key;
    }

    bool namesExe(const std::string& text) { return text.find("{exe}") != std::string::npos; }

    // The program a step runs: a path with a separator is a file of the project's directory (with the
    // host's executable suffix when the name as given is not there), a bare name is found on PATH.
    fs::path stepProgram(const fs::path& root, const std::string& program, std::string& error)
    {
      std::error_code ec;
      if (program.find('/') != std::string::npos || program.find('\\') != std::string::npos)
      {
        fs::path file = rooted(root, program).lexically_normal();
        if (fs::is_regular_file(file, ec))
          return file;
        fs::path suffixed = file;
        suffixed += host::executableSuffix();
        if (fs::is_regular_file(suffixed, ec))
          return suffixed;
        error = "its program " + file.string() + " does not exist";
        return {};
      }
      fs::path found = host::findOnPath(program);
      if (found.empty() && *host::executableSuffix())
      {
        // (a name with its suffix given: cmd.exe)
        std::string suffix = host::executableSuffix();
        if (program.size() > suffix.size() && program.compare(program.size() - suffix.size(), suffix.size(), suffix) == 0)
          found = host::findOnPath(program.substr(0, program.size() - suffix.size()));
      }
      if (found.empty())
        error = "no program " + program + " on PATH (a file of the project is named with its directory: tools/" + program + ")";
      return found;
    }

    fs::path stepStamp(const fs::path& root, const Program& p, const CommandStep& step)
    {
      return root / "build" / "kpp" / "steps" / p.name / p.configuration / (step.name + ".txt");
    }

    // What a step's last run is remembered by: its program, directory and arguments, and each file it reads
    // with its size and time (a directory: every file under it). false, with error set, for an input that
    // is not there.
    bool stepState(const fs::path& program, const fs::path& directory, const std::vector<std::string>& arguments, const std::vector<fs::path>& inputs,
                   std::string& state, std::string& error)
    {
      state = "program " + program.string() + "\ndirectory " + directory.string() + "\n";
      for (const std::string& a : arguments)
        state += "argument " + a + "\n";
      auto add = [&](const fs::path& file)
      {
        std::error_code ec;
        auto size = fs::file_size(file, ec);
        int64_t time = int64_t(fs::last_write_time(file, ec).time_since_epoch().count());
        state += "input " + file.string() + "\t" + std::to_string(size) + "\t" + std::to_string(time) + "\n";
      };
      for (const fs::path& input : inputs)
      {
        std::error_code ec;
        if (fs::is_directory(input, ec))
        {
          std::vector<fs::path> files;
          for (fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec))
              files.push_back(it->path());
          std::sort(files.begin(), files.end());
          state += "input " + input.string() + "\tdirectory\t" + std::to_string(files.size()) + "\n";
          for (const fs::path& file : files)
            add(file);
        }
        else if (fs::is_regular_file(input, ec))
          add(input);
        else
        {
          error = "its input " + input.string() + " does not exist";
          return false;
        }
      }
      return true;
    }

    // The first line of now that then has not: what changed since a step's last run, for kpp's line.
    std::string stepChange(const std::string& then, const std::string& now)
    {
      size_t a = 0, b = 0;
      while (a < then.size() && b < now.size())
      {
        size_t aEnd = std::min(then.find('\n', a), then.size()), bEnd = std::min(now.find('\n', b), now.size());
        std::string was = then.substr(a, aEnd - a), is = now.substr(b, bEnd - b);
        if (was != is)
        {
          if (is.rfind("input ", 0) == 0)
            return is.substr(6, is.find('\t') == std::string::npos ? std::string::npos : is.find('\t') - 6) + " changed";
          return "its command changed";
        }
        a = aEnd + 1, b = bEnd + 1;
      }
      return "its command or its inputs changed";
    }
  }

  bool planCommands(const fs::path& root, const Program& p, const fs::path& exe, CommandPlan& plan, std::string& error)
  {
    const std::vector<CommandStep>& steps = p.commands;
    size_t count = steps.size();
    plan.order.clear();
    plan.afterLink.assign(count, 0);
    plan.ran.assign(count, 0);
    auto wrong = [&](const CommandStep& s, const std::string& what)
    {
      error = p.name + "'s step " + s.name + ": " + what;
      return false;
    };
    std::map<std::string, size_t> byName, writer; // (writer: an output's pathKey -> its step)
    for (size_t i = 0; i < count; ++i)
    {
      const CommandStep& s = steps[i];
      if (!stepNameFits(s.name))
        return wrong(s, "a step's name is letters, digits, _, - and . (it names the file kpp remembers its last run in)");
      if (!byName.emplace(s.name, i).second)
        return wrong(s, "two steps of that name");
      if (s.program.empty())
        return wrong(s, "no program to run");
      if (!s.directory.empty() && !insideProject(root, rooted(root, withExe(s.directory, exe, p.configuration))))
        return wrong(s, "its directory " + s.directory + " must be in the project's directory, " + root.string());
      for (const std::string& o : s.outputs)
      {
        fs::path file = rooted(root, withExe(o, exe, p.configuration));
        if (!insideProject(root, file))
          return wrong(s, "its output " + o + " must be in the project's directory, " + root.string());
        auto [at, added] = writer.emplace(pathKey(file), i);
        if (!added && at->second != i)
          return wrong(s, "its output " + o + " is also the output of the step " + steps[at->second].name);
      }
    }
    // The steps a step waits for: those it names (after), and the writers of what it reads.
    std::vector<std::vector<size_t>> before(count);
    for (size_t i = 0; i < count; ++i)
    {
      const CommandStep& s = steps[i];
      for (const std::string& name : s.after)
      {
        auto at = byName.find(name);
        if (at == byName.end())
          return wrong(s, "it comes after " + name + ", which is no step of " + p.name);
        if (at->second == i)
          return wrong(s, "it comes after itself");
        before[i].push_back(at->second);
      }
      for (const std::string& input : s.inputs)
        if (auto at = writer.find(pathKey(rooted(root, withExe(input, exe, p.configuration)))); at != writer.end() && at->second != i)
          before[i].push_back(at->second);
    }
    // In the order build.kpp gives them, each after the steps it waits for; a step left over waits in a circle.
    std::vector<char> placed(count, 0);
    while (plan.order.size() < count)
    {
      size_t next = count;
      for (size_t i = 0; i < count && next == count; ++i)
        if (!placed[i] && std::all_of(before[i].begin(), before[i].end(), [&](size_t b) { return placed[b] != 0; }))
          next = i;
      if (next == count)
      {
        std::string names;
        for (size_t i = 0; i < count; ++i)
          if (!placed[i])
            names += (names.empty() ? "" : ", ") + steps[i].name;
        error = p.name + "'s steps " + names + " wait for each other in a circle (after, or one's input is the other's output)";
        return false;
      }
      placed[next] = 1;
      plan.order.push_back(next);
      const CommandStep& s = steps[next];
      bool names = namesExe(s.program) || namesExe(s.directory);
      for (const std::vector<std::string>* list : {&s.arguments, &s.inputs, &s.outputs})
        for (const std::string& text : *list)
          names = names || namesExe(text);
      for (size_t b : before[next])
        names = names || plan.afterLink[b];
      plan.afterLink[next] = names;
    }
    return true;
  }

  bool runCommands(const fs::path& root, const Program& p, const fs::path& exe, CommandPlan& plan, bool afterLink)
  {
    std::map<std::string, size_t> byName;
    for (size_t i = 0; i < p.commands.size(); ++i)
      byName[p.commands[i].name] = i;
    for (size_t index : plan.order)
    {
      if (bool(plan.afterLink[index]) != afterLink)
        continue;
      const CommandStep& step = p.commands[index];
      std::string name = p.name + "'s step " + step.name;
      auto expanded = [&](const std::string& text) { return withExe(text, exe, p.configuration); };
      std::string error;
      fs::path program = stepProgram(root, expanded(step.program), error);
      if (program.empty())
      {
        fprintf(stderr, "error: %s: %s\n", name.c_str(), error.c_str());
        return false;
      }
      fs::path directory = step.directory.empty() ? root : rooted(root, expanded(step.directory)).lexically_normal();
      std::vector<std::string> arguments;
      for (const std::string& a : step.arguments)
        arguments.push_back(expanded(a));
      std::vector<fs::path> inputs, outputs;
      for (const std::string& i : step.inputs)
        inputs.push_back(rooted(root, expanded(i)).lexically_normal());
      for (const std::string& o : step.outputs)
        outputs.push_back(rooted(root, expanded(o)).lexically_normal());
      std::string state;
      if (!stepState(program, directory, arguments, inputs, state, error))
      {
        fprintf(stderr, "error: %s: %s\n", name.c_str(), error.c_str());
        return false;
      }
      // Out of date: a step it comes after ran, it never ran (or what it ran with is not kept), a file it
      // writes is missing, or its command or a file it reads changed; one that names nothing to tell by
      // (no input, no output, no step before it) runs every build.
      fs::path stamp = stepStamp(root, p, step);
      std::error_code ec;
      std::string why;
      for (const std::string& a : step.after)
        if (auto at = byName.find(a); why.empty() && at != byName.end() && plan.ran[at->second])
          why = "the step " + a + " ran";
      std::string then = readText(stamp);
      if (why.empty() && !fs::exists(stamp, ec))
        why = "its first run";
      for (const fs::path& o : outputs)
        if (why.empty() && !fs::exists(o, ec))
          why = o.string() + " is missing";
      if (why.empty() && then != state)
        why = stepChange(then, state);
      if (why.empty() && step.inputs.empty() && step.outputs.empty() && step.after.empty())
        why = "it names no input, output or step before it, so it runs every build";
      if (why.empty())
        continue;
      std::string line = program.filename().string();
      for (const std::string& a : arguments)
        line += " " + quoteArg(a);
      printf("kpp: step %s (%s): %s\n", step.name.c_str(), why.c_str(), line.c_str());
      fflush(stdout);
      // (its record goes first: a run that fails, or is stopped, is not taken for done)
      fs::remove(stamp, ec);
      for (const fs::path& o : outputs)
        fs::create_directories(o.parent_path(), ec);
      if (!fs::is_directory(directory, ec))
      {
        fprintf(stderr, "error: %s: its directory %s does not exist\n", name.c_str(), directory.string().c_str());
        return false;
      }
      host::Run how;
      how.directory = directory;
      std::string started;
      int code = host::run(program.string(), arguments, how, started);
      if (code != 0)
      {
        if (code == -1 && !started.empty())
          fprintf(stderr, "error: %s: %s could not be started: %s\n", name.c_str(), program.string().c_str(), started.c_str());
        else
          fprintf(stderr, "error: %s failed (exit %d): %s\n", name.c_str(), code, line.c_str());
        return false;
      }
      for (const fs::path& o : outputs)
        if (!fs::exists(o, ec))
        {
          fprintf(stderr, "error: %s ran and did not write its output %s\n", name.c_str(), o.string().c_str());
          return false;
        }
      fs::create_directories(stamp.parent_path(), ec);
      if (!writeText(stamp, state, name.c_str()))
        return false;
      plan.ran[index] = 1;
    }
    return true;
  }
}
