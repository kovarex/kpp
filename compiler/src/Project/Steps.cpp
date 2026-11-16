// What kpp does itself around a cell's build, so that build.kpp starts no process (docs/build.md, "Build
// information" and "Steps after the build"; design.md, "What build.kpp can run"): the buildInfo:: constants,
// read from git and the project's files, and the after-build links and files, inside the project's directory.
#include "Project/Internal.hpp"
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
}
