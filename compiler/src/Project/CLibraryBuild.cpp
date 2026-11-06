// The C and C++ libraries a project builds from sources (docs/build.md, "C and C++ libraries"): each
// object checked against its source and headers, the changed ones compiled, the archive made.
// (Moved out of project.cpp on 2026-11-05 without a change.)
#include "Project/Internal.hpp"

namespace projects
{
  // The platform a program's C libraries are built for: the last --target of build.kpp's arguments and
  // the command line's (as targetPlatform), its triple, and the sysroot kppc links with (the last
  // --sysroot, else the one beside kppc, sysroots\linux-x86_64 or freebsd-x86_64).
  LibraryTarget libraryTarget(const Program& p, const Request& request)
  {
    LibraryTarget out;
    Target target;
    std::string sysroot;
    for (const std::vector<std::string>* list : {&p.arguments, &request.passed})
      for (size_t i = 0; i + 1 < list->size(); ++i)
      {
        if ((*list)[i] == "--target")
        {
          std::string why;
          Target named;
          if (targetNamed((*list)[i + 1], named, why))
            target = named;
        }
        else if ((*list)[i] == "--sysroot")
          sysroot = (*list)[i + 1];
      }
    out.platform = platformOf(target.os).name;
    if (!target.windows())
    {
      out.triple = target.triple;
      out.sysroot = !sysroot.empty() ? sysroot : defaultSysroot(compilerPath(), (out.platform + "-x86_64").c_str()).string();
    }
    return out;
  }

  // A C library's entries (sources, includes, defines...) for one platform: an entry build.kpp gave
  // with a platform (`define("_GNU_SOURCE", Platform::Linux)`, kept as "linux _GNU_SOURCE", as
  // systemLibrary's are) only for that platform, without it; every other entry for every platform.
  std::vector<std::string> forPlatform(const std::vector<std::string>& entries, const std::string& platform)
  {
    std::vector<std::string> out;
    for (const std::string& e : entries)
    {
      size_t blank = e.find(' ');
      bool platformEntry = false;
      if (blank != std::string::npos)
        for (const PlatformName& p : platformNames)
          if (e.compare(0, blank, p.name) == 0)
            platformEntry = true;
      if (!platformEntry)
        out.push_back(e);
      else if (e.compare(0, blank, platform) == 0)
        out.push_back(e.substr(blank + 1));
    }
    return out;
  }

  // Where a program's C libraries are built: build\clib, or for a configuration with C flags of its
  // own (build.kpp's cFlag: the final configuration's -flto) build\clib\flags-HASH, those flags' own,
  // which configurations with the same flags share (each library's flags.txt says them). Building
  // one configuration then leaves another's objects as they are. Another platform's are in
  // build\clib-PLATFORM (build\clib-linux), so that building for one leaves the other's as they are.
  fs::path clibDirectory(const fs::path& root, const std::vector<std::string>& cFlags, const std::string& platform)
  {
    fs::path base = root / "build" / (platform == "windows" ? std::string("clib") : "clib-" + platform);
    if (cFlags.empty())
      return base;
    uint32_t hash = 2166136261u;
    for (const std::string& flag : cFlags)
      for (unsigned char ch : flag + '\n')
        hash = (hash ^ ch) * 16777619u;
    char name[24];
    snprintf(name, sizeof name, "flags-%08x", hash);
    return base / name;
  }

  fs::path libraryDirectory(const fs::path& root, const CLibrary& c, const std::vector<std::string>& cFlags, const std::string& platform)
  {
    return clibDirectory(root, cFlags, platform) / c.name;
  }

  // What linking a C library takes: its prebuilt libraries or the one kpp builds (NAME.lib for
  // Windows, the archive NAME.a for Linux and FreeBSD), and its .res files, the platform's.
  std::vector<fs::path> libraryFiles(const fs::path& root, const CLibrary& c, const std::vector<std::string>& cFlags, const std::string& platform)
  {
    std::vector<fs::path> files;
    std::vector<std::string> prebuilt = forPlatform(c.prebuilt, platform);
    for (const std::string& lib : prebuilt)
      files.push_back(rooted(root, lib));
    if (prebuilt.empty() && !forPlatform(c.sources, platform).empty())
      files.push_back(clibDirectory(root, cFlags, platform) / (c.name + (platform == "windows" ? ".lib" : ".a")));
    for (const std::string& rc : forPlatform(c.resources, platform))
      files.push_back(libraryDirectory(root, c, cFlags, platform) / (fs::path(rc).stem().string() + ".res"));
    return files;
  }

  // The C++ library a program links for its C libraries' C++ files, off Windows: an ELF object names
  // no default library, as a COFF one names MSVC's C++ runtime (/DEFAULTLIB), and kppc links with
  // clang's C driver. Linux: libstdc++, statically (as Factorio's Linux build does: the program needs
  // no particular libstdc++ of the system it runs on); FreeBSD: libc++ and libcxxrt, its C++ ABI
  // library (a FreeBSD program links statically). Nothing for Windows or a library of C files only.
  std::vector<std::string> cppRuntimeLibraries(const fs::path& root, const CLibrary& c, const std::string& platform)
  {
    if (platform == "windows" || !forPlatform(c.prebuilt, platform).empty())
      return {};
    for (const std::string& s : forPlatform(c.sources, platform))
      for (const fs::path& file : collectFiles(rooted(root, s), {".cc", ".cpp"}))
      {
        (void)file;
        if (platform == "freebsd")
          return {"-lc++", "-lcxxrt"};
        return {"-l:libstdc++.a"};
      }
    return {};
  }

  fs::file_time_type modified(const fs::path& path)
  {
    std::error_code ec;
    fs::file_time_type t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::max() : t;
  }

  // What a resource script reads: the files next to it (a header it includes, an icon, a manifest)
  // and every file it names in quotes, from its directory, wherever that is (factorio-in-kpp's
  // factorio.rc names its icon in ../../data). A quoted name that is no file (a version string) adds
  // nothing.
  std::vector<fs::path> resourceInputs(const fs::path& script)
  {
    std::vector<fs::path> inputs;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(script.parent_path(), ec))
      if (entry.is_regular_file())
        inputs.push_back(entry.path());
    std::string text = readText(script);
    for (size_t at = text.find('"'); at != std::string::npos; at = text.find('"', at + 1))
    {
      size_t end = text.find('"', at + 1);
      if (end == std::string::npos)
        break;
      std::string name = text.substr(at + 1, end - at - 1);
      at = end;
      for (size_t slash = name.find("\\\\"); slash != std::string::npos; slash = name.find("\\\\", slash + 1))
        name.erase(slash, 1); // (a C string's "..\\icon.ico")
      if (name.empty() || name.find_first_of("\r\n*?<>|") != std::string::npos)
        continue;
      fs::path named = script.parent_path() / fs::path(name);
      if (fs::is_regular_file(named, ec))
        inputs.push_back(named);
    }
    return inputs;
  }

  // Whether object depends only on files older than itself: the source and the headers clang
  // named in its dependency file (-MD). times keeps each file's time for the build: the 641 objects of
  // factorio-in-kpp's libraries name the same headers (the SDK's, SDL's) about 100,000 times in all.
  bool upToDate(const fs::path& object, const fs::path& dependencies, std::unordered_map<std::string, fs::file_time_type>& times)
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
      if (word.empty())
        return true;
      auto [at, added] = times.try_emplace(word);
      if (added)
        at->second = modified(word);
      bool fresh = at->second <= built;
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

  // prefixMap: libraryPrefixMap's flags, after cFlags. target: the platform the program is built for
  // (libraryTarget), whose entries of the library are taken (forPlatform).
  bool prepareLibrary(const fs::path& root, const CLibrary& c, const std::vector<std::string>& cFlags, const std::string& prefixMap, LibraryJob& job,
                      std::unordered_map<std::string, fs::file_time_type>& times, const LibraryTarget& target)
  {
    fs::path tools = fs::path(clangPath()).parent_path();
    const std::string& platform = target.platform;
    job.library = &c;
    job.directory = libraryDirectory(root, c, cFlags, platform);
    std::error_code ec;
    fs::create_directories(job.directory, ec);
    std::vector<std::string> sourceEntries = forPlatform(c.sources, platform), resourceEntries = forPlatform(c.resources, platform);
    // A resource script is Windows' (llvm-rc makes a .res that only link.exe and lld-link take): one
    // in a build for another platform is an error, not left out unsaid.
    if (target.posix() && !resourceEntries.empty())
    {
      fprintf(stderr, "error: the C library %s has the resource script %s, which only a Windows program links: give it with Platform::Windows "
                      "(resource(PATH, Platform::Windows))\n", c.name.c_str(), resourceEntries[0].c_str());
      return false;
    }
    if (forPlatform(c.prebuilt, platform).empty() && !sourceEntries.empty())
    {
      if (target.posix() && target.sysroot.empty())
      {
        fprintf(stderr, "error: the C library %s: building it for %s needs the target's C headers and libraries: --sysroot DIR, or %s\n",
                c.name.c_str(), platform.c_str(), (compilerPath().parent_path() / "sysroots" / (platform + "-x86_64")).string().c_str());
        return false;
      }
      std::vector<fs::path> sources;
      std::vector<fs::path> excluded;
      for (const std::string& e : forPlatform(c.excluded, platform))
        excluded.push_back(rooted(root, e));
      for (const std::string& s : sourceEntries)
        for (const fs::path& file : collectFiles(rooted(root, s), {".c", ".cc", ".cpp"}))
          if (std::find(excluded.begin(), excluded.end(), fs::absolute(file).lexically_normal()) == excluded.end())
            sources.push_back(fs::absolute(file).lexically_normal());
      if (sources.empty())
      {
        fprintf(stderr, "error: the C library %s has no .c, .cc or .cpp file in its sources\n", c.name.c_str());
        return false;
      }
      // The flags of every object: Factorio's prebuilt libraries' (/O2 /MT, no stack cookies as in
      // both of Factorio's C++ builds' /GS-, no assertions), the library's includes and defines; no
      // warnings, since the code is the library's own; then the configuration's (cFlag). The bundled
      // toolchain (llvm-mingw) builds Windows' GNU dialect (design.md 17): clang's own flags, its
      // MinGW-w64 headers and libc++; Visual Studio's clang is clang-cl, with MSVC's. For Linux and
      // FreeBSD, clang for the target's triple with its sysroot's headers (glibc's and libstdc++'s,
      // FreeBSD's and libc++), as kppc compiles and links the program: ELF objects, and an archive
      // llvm-ar makes (NAME.a).
      bool gnu = job.gnu = bundledToolchain() || target.posix();
      std::string flags = !gnu             ? std::string("/nologo /c /O2 /MT /GS- /DNDEBUG /w")
                          : target.posix() ? "--target=" + target.triple + " " + quoteArg("--sysroot=" + target.sysroot) + " -c -O2 -DNDEBUG -w -fno-stack-protector"
                                           : std::string("-c -O2 -DNDEBUG -w -fno-stack-protector");
      // The MSVC headers and libraries of clang's own Visual Studio: on its own, clang-cl takes the
      // newest installed, whose STL may require a newer clang (Build Tools 18's wants clang 20).
      fs::path installation = tools.parent_path().parent_path().parent_path().parent_path().parent_path();
      std::string version = gnu ? std::string() : readText(installation / "VC" / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt");
      while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' '))
        version.pop_back();
      if (!version.empty() && fs::is_directory(installation / "VC" / "Tools" / "MSVC" / version, ec))
        flags += " /vctoolsdir " + quoteArg((installation / "VC" / "Tools" / "MSVC" / version).string());
      // An include directory that starts with `=` is in the target's sysroot, as GCC's -I=DIR (which
      // Visual Studio's clang does not take with a Windows sysroot): SDL's
      // `include("=/usr/include/dbus-1.0", Platform::Linux)`. Elsewhere it is an error.
      for (const std::string& i : forPlatform(c.includes, platform))
      {
        if (!i.empty() && i[0] == '=' && !target.posix())
        {
          fprintf(stderr, "error: the C library %s: the include directory %s is in a sysroot, which only a build for Linux or FreeBSD has\n",
                  c.name.c_str(), i.c_str());
          return false;
        }
        fs::path dir = !i.empty() && i[0] == '=' ? (fs::path(target.sysroot) / fs::path(i.substr(1)).relative_path()).lexically_normal() : rooted(root, i);
        flags += (gnu ? " -I" : " /I") + quoteArg(dir.string());
      }
      for (const std::string& d : forPlatform(c.defines, platform))
        flags += (gnu ? " -D" : " /D") + quoteArg(d);
      for (const std::string& f : cFlags)
        flags += " " + quoteArg(f);
      flags += prefixMap;
      std::string language = c.language.empty() ? std::string("c++17") : c.language;
      std::string cppFlags = gnu ? " -std=" + language : " /EHsc /std:" + language;
      // What the objects are built with: the flags and the compiler (another toolchain's are built again).
      job.stamp = flags + cppFlags + "\n" + clangPath();
      bool flagsChanged = readText(job.directory / "flags.txt") != job.stamp;
      // Gone until all of this build's objects are made (written before the archive): a build that
      // fails part way leaves objects of the new flags beside the old ones, which the old flags'
      // stamp would vouch for in a later build (another toolchain's objects in its archive).
      if (flagsChanged)
        fs::remove(job.directory / "flags.txt", ec);
      for (size_t n = 0; n < sources.size(); ++n)
      {
        const fs::path& source = sources[n];
        std::string stem = source.lexically_relative(root).string();
        for (char& ch : stem)
          if (ch == '\\' || ch == '/' || ch == ':' || ch == '.')
            ch = '_';
        fs::path object = job.directory / (stem + (target.posix() ? ".o" : ".obj")), dependencies = job.directory / (stem + ".d");
        job.objects.push_back(object);
        if (!flagsChanged && upToDate(object, dependencies, times))
          continue;
        bool cpp = source.extension() != ".c";
        job.logs.push_back(job.directory / (stem + ".log"));
        // clang-cl is clang in its cl mode (no clang-cl.exe to ship).
        if (gnu)
          job.commands.push_back(quoteArg(clangPath()) + " " + flags + (cpp ? cppFlags : "") + " -MD -MF" + quoteArg(dependencies.string()) +
                                 " -o " + quoteArg(object.string()) + " " + quoteArg(source.string()) + " > " + quoteArg(job.logs.back().string()) + " 2>&1");
        else
          job.commands.push_back(quoteArg(clangPath()) + " --driver-mode=cl " + flags + (cpp ? cppFlags : "") +
                                 " /clang:-MD /clang:-MF" + quoteArg(dependencies.string()) + " /Fo" + quoteArg(object.string()) +
                                 " " + quoteArg(source.string()) + " > " + quoteArg(job.logs.back().string()) + " 2>&1");
      }
      job.compiles = job.commands.size();
      job.lib = clibDirectory(root, cFlags, platform) / (c.name + (target.posix() ? ".a" : ".lib"));
      job.archive = !job.commands.empty() || !fs::exists(job.lib, ec);
      std::string dialect = target.posix() ? platform + ": clang for " + target.triple + " with the target's sysroot"
                            : gnu          ? std::string("the bundled toolchain, Windows' GNU dialect")
                                           : std::string("clang-cl, MSVC's dialect");
      if (job.archive)
        printf("kpp: %s: compiling %zu of %zu C/C++ files with %s (%s)\n", c.name.c_str(), job.commands.size(), sources.size(), clangPath().c_str(),
               dialect.c_str());
    }
    for (const std::string& rc : resourceEntries)
    {
      fs::path script = rooted(root, rc), res = job.directory / (script.stem().string() + ".res");
      // Rebuilt when any file it reads is newer (resourceInputs).
      bool fresh = fs::exists(res, ec);
      for (const fs::path& input : resourceInputs(script))
        if (modified(input) > modified(res))
          fresh = false;
      if (fresh)
        continue;
      job.commands.push_back("cd /d " + quoteArg(script.parent_path().string()) + " && " + quoteArg((tools / "llvm-rc.exe").string()) +
                             " /nologo /fo " + quoteArg(res.string()) + " " + quoteArg(script.string()));
      job.scripts.push_back(script);
      job.resources.push_back(res);
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
          if (host::shell(jobs[work[i].first].commands[work[i].second]) != 0)
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
          // What a failed llvm-rc wrote is no resource file (it leaves the resources before the one it
          // failed on): never one the next build takes as made, newer than its script, and links.
          fs::remove(job.resources[k - job.compiles], ec);
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
      FILE* file = host::openFile(job.directory / "flags.txt", "wb");
      if (file)
        fwrite(job.stamp.data(), 1, job.stamp.size(), file), fclose(file);
      std::string response;
      for (const fs::path& object : job.objects)
        response += quoteArg(object.string()) + "\n";
      FILE* rsp = host::openFile(job.directory / "objects.rsp", "wb");
      if (rsp)
        fwrite(response.data(), 1, response.size(), rsp), fclose(rsp);
      // (llvm-mingw has llvm-ar alone, which lld's GNU driver links as it links llvm-lib's)
      fs::path tools = fs::path(clangPath()).parent_path();
      std::string archive = job.gnu ? quoteArg((tools / "llvm-ar.exe").string()) + " rcs " + quoteArg(job.lib.string()) + " @" +
                                        quoteArg((job.directory / "objects.rsp").string())
                                    : quoteArg((tools / "llvm-lib.exe").string()) + " /nologo /out:" + quoteArg(job.lib.string()) + " @" +
                                        quoteArg((job.directory / "objects.rsp").string());
      if (job.gnu)
        fs::remove(job.lib, ec); // (llvm-ar adds to an archive that is there)
      if (host::shell(archive) != 0)
      {
        fprintf(stderr, "error: %s could not archive the C library %s\n", job.gnu ? "llvm-ar" : "llvm-lib", c.name.c_str());
        ok = false;
      }
    }
    return ok;
  }
}
