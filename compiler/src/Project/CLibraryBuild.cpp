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
    Target target = hostTarget();
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
    // (the bundled toolchain builds Windows' GNU dialect, as kppc takes it: Compile.cpp)
    if (bundledToolchain() && target.windows() && !target.gnu)
    {
      std::string why;
      targetNamed("windows-gnu", target, why);
    }
    out.platform = platformOf(target.os).name;
    // (windows-gnu's triple said: a Linux host's llvm-mingw clang targets Linux by default)
    out.gnu = target.gnu;
    if (target.gnu)
      out.triple = target.triple;
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
  // library (a FreeBSD program links statically); windows-gnu: llvm-mingw's libc++ (its C++ ABI part
  // in it) and libunwind, their archives by name, as the import libraries would make the program need
  // libc++.dll and libunwind.dll. Nothing for MSVC's Windows or a library of C files only.
  std::vector<std::string> cppRuntimeLibraries(const fs::path& root, const CLibrary& c, const std::string& platform, bool gnu)
  {
    if ((platform == "windows" && !gnu) || !forPlatform(c.prebuilt, platform).empty())
      return {};
    for (const std::string& s : forPlatform(c.sources, platform))
      for (const fs::path& file : collectFiles(rooted(root, s), {".cc", ".cpp"}))
      {
        (void)file;
        if (platform == "freebsd")
          return {"-lc++", "-lcxxrt"};
        if (platform == "windows")
          return {"-l:libc++.a", "-l:libunwind.a"};
        return {"-l:libstdc++.a"};
      }
    return {};
  }

  // One of LLVM's tools (llvm-ar, llvm-lib, llvm-rc) of the clang that builds the C libraries: beside
  // it (Visual Studio's, the bundled toolchain's), beside its real path (a Linux distribution's
  // /usr/bin/clang-19 is /usr/lib/llvm-19/bin/clang, where its tools are), else on PATH. Empty, the
  // error said, where none is.
  std::string llvmTool(const std::string& name, const std::string& library)
  {
    std::string file = name + host::executableSuffix();
    fs::path clang = clangPath();
    std::error_code ec;
    for (const fs::path& dir : {clang.parent_path(), fs::canonical(clang, ec).parent_path()})
      if (!dir.empty() && fs::is_regular_file(dir / file, ec))
        return (dir / file).string();
    if (fs::path onPath = host::findOnPath(name); !onPath.empty())
      return onPath.string();
    fprintf(stderr, "error: the C library %s needs %s, which is neither beside %s nor on PATH: install LLVM's tools (apt install llvm, "
                    "dnf install llvm)\n", library.c_str(), name.c_str(), clang.string().c_str());
    return "";
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
  // The header a source's first directive includes, as written between its quotes or angle brackets
  // ("" when its first directive is another, or something before it is not a comment or blank): which
  // sources a library's precompiled header stands in for.
  std::string firstInclude(const fs::path& source)
  {
    std::string text = readText(source);
    size_t i = 0;
    auto blank = [&](bool newlines)
    {
      for (;;)
      {
        if (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || (newlines && text[i] == '\n') || text[i] == '\f' || text[i] == '\v'))
          ++i;
        else if (text.compare(i, 2, "\\\n") == 0)
          i += 2;
        else if (text.compare(i, 3, "\\\r\n") == 0)
          i += 3;
        else if (text.compare(i, 2, "/*") == 0)
        {
          size_t end = text.find("*/", i + 2);
          i = end == std::string::npos ? text.size() : end + 2;
        }
        else if (newlines && text.compare(i, 2, "//") == 0)
        {
          size_t end = text.find('\n', i);
          i = end == std::string::npos ? text.size() : end;
        }
        else
          return;
      }
    };
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
      i = 3;
    blank(true);
    if (i >= text.size() || text[i] != '#')
      return "";
    ++i;
    blank(false);
    if (text.compare(i, 7, "include") != 0)
      return "";
    i += 7;
    blank(false);
    if (i >= text.size() || (text[i] != '"' && text[i] != '<'))
      return "";
    char close = text[i] == '"' ? '"' : '>';
    size_t end = text.find(close, i + 1);
    if (end == std::string::npos || text.find('\n', i) < end)
      return "";
    return text.substr(i + 1, end - i - 1);
  }

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
      // (windows-gnu decides it, not the toolchain alone: KPP_CLANG naming llvm-mingw's clang, a Linux
      // host's, built the libraries in MSVC's dialect, clang-cl's flags)
      bool gnu = job.gnu = bundledToolchain() || target.gnu || target.posix();
      std::string flags = !gnu             ? std::string("/nologo /c /O2 /MT /GS- /DNDEBUG /w")
                          : target.posix() ? "--target=" + target.triple + " " + quoteArg("--sysroot=" + target.sysroot) + " -c -O2 -DNDEBUG -w -fno-stack-protector"
                          : target.gnu     ? "--target=" + target.triple + headerCaseOverlay(clangPath(), target.triple) + " -c -O2 -DNDEBUG -w -fno-stack-protector"
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
      job.stamp = flags + cppFlags + "\n" + clangPath() + (c.precompiled.empty() ? std::string() : "\nprecompiled " + c.precompiled);
      bool flagsChanged = readText(job.directory / "flags.txt") != job.stamp;
      // Gone until all of this build's objects are made (written before the archive): a build that
      // fails part way leaves objects of the new flags beside the old ones, which the old flags'
      // stamp would vouch for in a later build (another toolchain's objects in its archive).
      if (flagsChanged)
        fs::remove(job.directory / "flags.txt", ec);
      // Its precompiled header (CLibrary::precompiledHeader, B's whole-build profile: SDL's 234 files
      // spent most of their 118 s of CPU in Windows' and SDL's headers): the file the include
      // directories give, built once per language, and used by each source whose first directive
      // includes it as written and that has no file of that name of its own beside it (which its
      // `#include "..."` would find first). The objects are those of the compiles without it.
      fs::path pchHeader;
      if (!c.precompiled.empty())
      {
        for (const std::string& i : forPlatform(c.includes, platform))
          if (!i.empty() && i[0] != '=' && fs::is_regular_file(rooted(root, i) / c.precompiled, ec))
          {
            pchHeader = fs::absolute(rooted(root, i) / c.precompiled, ec).lexically_normal();
            break;
          }
        if (pchHeader.empty())
        {
          fprintf(stderr, "error: the C library %s: its precompiled header %s is in none of its include directories\n", c.name.c_str(),
                  c.precompiled.c_str());
          return false;
        }
      }
      auto usesHeader = [&](const fs::path& source)
      {
        if (pchHeader.empty() || firstInclude(source) != c.precompiled)
          return false;
        fs::path beside = source.parent_path() / c.precompiled;
        return !fs::exists(beside, ec) || fs::equivalent(beside, pchHeader, ec);
      };
      // per language (0 C, 1 C++): the PCH, whether it is built in this build
      fs::path pchFile[2];
      bool pchBuilt[2] = {false, false};
      int pchIndex[2] = {-1, -1};
      std::vector<char> eligible(sources.size(), 0);
      for (size_t n = 0; n < sources.size(); ++n)
        eligible[n] = usesHeader(sources[n]);
      for (int l = 0; l < 2; ++l)
      {
        bool any = false;
        for (size_t n = 0; n < sources.size(); ++n)
          any = any || (eligible[n] && (sources[n].extension() != ".c") == (l == 1));
        if (!any)
          continue;
        std::string name = l ? "pch-cpp" : "pch-c";
        pchFile[l] = job.directory / (name + ".pch");
        fs::path stub = job.directory / (name + (gnu ? (l ? ".hpp" : ".h") : (l ? ".cpp" : ".c")));
        fs::path deps = job.directory / (name + ".d"), log = job.directory / (name + ".log");
        std::string include = "#include \"" + c.precompiled + "\"\n";
        if (readText(stub) != include)
          if (FILE* f = host::openFile(stub, "wb"))
            fwrite(include.data(), 1, include.size(), f), fclose(f);
        pchIndex[l] = int(job.pchs.size());
        LibraryJob::Precompiled& p = job.pchs.emplace_back();
        pchBuilt[l] = flagsChanged || !upToDate(pchFile[l], deps, times);
        if (!pchBuilt[l])
          continue;
        p.state->store(0);
        p.log = log;
        if (gnu)
          p.command = quoteArg(clangPath()) + " " + flags + (l ? cppFlags : "") + (l ? " -x c++-header" : " -x c-header") + " -MD -MF" + quoteArg(deps.string()) +
                      " -o " + quoteArg(pchFile[l].string()) + " " + quoteArg(stub.string()) + " > " + quoteArg(log.string()) + " 2>&1";
        else
          p.command = quoteArg(clangPath()) + " --driver-mode=cl " + flags + (l ? cppFlags : "") + " /Yc" + quoteArg(c.precompiled) + " /Fp" +
                      quoteArg(pchFile[l].string()) + " /clang:-MD /clang:-MF" + quoteArg(deps.string()) + " /Fo" +
                      quoteArg((job.directory / (name + ".obj")).string()) + " " + quoteArg(stub.string()) + " > " + quoteArg(log.string()) + " 2>&1";
      }
      for (size_t n = 0; n < sources.size(); ++n)
      {
        const fs::path& source = sources[n];
        std::string stem = source.lexically_relative(root).string();
        for (char& ch : stem)
          if (ch == '\\' || ch == '/' || ch == ':' || ch == '.')
            ch = '_';
        fs::path object = job.directory / (stem + (target.posix() ? ".o" : ".obj")), dependencies = job.directory / (stem + ".d");
        job.objects.push_back(object);
        bool cpp = source.extension() != ".c";
        int l = cpp ? 1 : 0;
        bool withPch = eligible[n] && pchIndex[l] >= 0;
        if (!flagsChanged && !(withPch && pchBuilt[l]) && upToDate(object, dependencies, times))
          continue;
        job.logs.push_back(job.directory / (stem + ".log"));
        job.usesPch.push_back(withPch ? pchIndex[l] : -1);
        std::string pch = !withPch ? std::string() : gnu ? " -include-pch " + quoteArg(pchFile[l].string())
                                                         : " /Yu" + quoteArg(c.precompiled) + " /Fp" + quoteArg(pchFile[l].string());
        // clang-cl is clang in its cl mode (no clang-cl.exe to ship).
        if (gnu)
          job.commands.push_back(quoteArg(clangPath()) + " " + flags + (cpp ? cppFlags : "") + pch + " -MD -MF" + quoteArg(dependencies.string()) +
                                 " -o " + quoteArg(object.string()) + " " + quoteArg(source.string()) + " > " + quoteArg(job.logs.back().string()) + " 2>&1");
        else
          job.commands.push_back(quoteArg(clangPath()) + " --driver-mode=cl " + flags + (cpp ? cppFlags : "") + pch +
                                 " /clang:-MD /clang:-MF" + quoteArg(dependencies.string()) + " /Fo" + quoteArg(object.string()) +
                                 " " + quoteArg(source.string()) + " > " + quoteArg(job.logs.back().string()) + " 2>&1");
      }
      job.compiles = job.commands.size();
      job.lib = clibDirectory(root, cFlags, platform) / (c.name + (target.posix() ? ".a" : ".lib"));
      job.archive = !job.commands.empty() || !fs::exists(job.lib, ec);
      std::string dialect = target.posix() ? platform + ": clang for " + target.triple + " with the target's sysroot"
                            : gnu          ? "Windows' GNU dialect, llvm-mingw's clang for " + target.triple
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
      std::string llvmRc = llvmTool("llvm-rc", c.name);
      if (llvmRc.empty())
        return false;
      job.commands.push_back("cd /d " + quoteArg(script.parent_path().string()) + " && " + quoteArg(llvmRc) +
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
    // (job, command), a precompiled header's build as command SIZE_MAX - its index: those first, then the
    // compiles that use none, then the ones that do, each waiting for its header if it is not built yet
    // (by then it nearly always is)
    std::vector<std::pair<size_t, size_t>> work;
    auto usesPch = [&](size_t j, size_t k) { return k < jobs[j].usesPch.size() ? jobs[j].usesPch[k] : -1; };
    for (size_t j = 0; j < jobs.size(); ++j)
      for (size_t p = 0; p < jobs[j].pchs.size(); ++p)
        if (!jobs[j].pchs[p].command.empty())
          work.push_back({j, SIZE_MAX - p});
    for (int pass = 0; pass < 2; ++pass)
      for (size_t j = 0; j < jobs.size(); ++j)
        for (size_t k = 0; k < jobs[j].commands.size(); ++k)
          if ((usesPch(j, k) >= 0) == (pass == 1))
            work.push_back({j, k});
    std::atomic<size_t> next = 0;
    std::vector<std::thread> threads;
    unsigned count = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned t = 0; t < count && t < work.size(); ++t)
      threads.emplace_back([&]
      {
        for (size_t i; (i = next++) < work.size();)
        {
          LibraryJob& job = jobs[work[i].first];
          size_t k = work[i].second;
          if (k >= job.commands.size())
          {
            LibraryJob::Precompiled& p = job.pchs[SIZE_MAX - k];
            p.state->store(host::shell(p.command) == 0 ? 1 : 2);
            continue;
          }
          if (int p = usesPch(work[i].first, k); p >= 0)
          {
            while (job.pchs[size_t(p)].state->load() == 0)
              std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (job.pchs[size_t(p)].state->load() == 2)
            {
              job.failed[k] = true; // (its header's errors are the ones reported)
              continue;
            }
          }
          if (host::shell(job.commands[k]) != 0)
            job.failed[k] = true;
        }
      });
    for (std::thread& t : threads)
      t.join();
    bool ok = true;
    std::error_code ec;
    for (LibraryJob& job : jobs)
    {
      const CLibrary& c = *job.library;
      bool pchFailed = false;
      for (const LibraryJob::Precompiled& p : job.pchs)
        if (!p.command.empty())
        {
          std::string text = readText(p.log);
          if (!text.empty())
            fprintf(stderr, "%s", text.c_str());
          if (p.state->load() == 2)
          {
            fprintf(stderr, "error: the C library %s: its precompiled header %s did not compile (above)\n", c.name.c_str(), c.precompiled.c_str());
            pchFailed = true;
          }
        }
      if (pchFailed)
      {
        ok = false;
        continue;
      }
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
      std::string tool = llvmTool(job.gnu ? "llvm-ar" : "llvm-lib", c.name);
      if (tool.empty())
      {
        ok = false;
        continue;
      }
      std::string archive = job.gnu ? quoteArg(tool) + " rcs " + quoteArg(job.lib.string()) + " @" + quoteArg((job.directory / "objects.rsp").string())
                                    : quoteArg(tool) + " /nologo /out:" + quoteArg(job.lib.string()) + " @" + quoteArg((job.directory / "objects.rsp").string());
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
