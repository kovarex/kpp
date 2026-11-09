// The driver: the link's command and flags; its declarations in Link.hpp (Driver.hpp). A
// translation unit of its own since 2026-11-05 (main.cpp split, stage 2), moved without a change.
#include "Driver.hpp"

namespace driver
{
void deferLinkCommand(const Options& options, const std::string& command)
{
  for (auto [suffix, text] : {std::pair<const char*, const std::string*>{".link.command", &command}, {".link.pending", nullptr}})
    if (FILE* f = fopen((options.exe + suffix).c_str(), "wb"))
    {
      if (text)
        fwrite(text->data(), 1, text->size(), f);
      fclose(f);
    }
    else
    {
      fprintf(stderr, "cannot write %s%s\n", options.exe.c_str(), suffix);
      codegenExit = 1;
    }
}

std::string natvisPath(const Options& options)
{
  return std::filesystem::path(options.exe).replace_extension(".natvis").string();
}

// The link of every target but MSVC's takes lld (-fuse-ld=lld: ld.lld, lld's GNU driver), which clang
// finds beside itself or on PATH. Where it is not, the build fails naming it (B, 2026-11-06: a Linux
// host's clang may come without lld; never GNU ld in its place, whose links differ).
void requireLld()
{
  std::string ld = std::string("ld.lld") + host::executableSuffix();
  std::filesystem::path clang = clangPath();
  std::error_code ec;
  if (std::filesystem::exists(clang.parent_path() / ld, ec) || std::filesystem::exists(std::filesystem::canonical(clang, ec).parent_path() / ld, ec) ||
      !host::findOnPath("ld.lld").empty())
    return;
  fprintf(stderr, "error: the link needs ld.lld (lld), which is neither beside %s nor on PATH: install lld (apt install lld, dnf install lld, "
                  "pkg install llvm)\n",
          clang.string().c_str());
  codegenExit = 1;
}

std::string linkFlags(const Options& options, bool wideEntry)
{
  if (options.target.gnu || !options.target.windows())
    requireLld();
  // Windows' GNU dialect (windows-gnu): lld's GNU driver, unused sections dropped as /OPT:REF
  // drops them, the Windows libraries programs commonly reach, ucrtbase's functions MinGW-w64's
  // import libraries leave out, -municode for a wmain, --lib files.
  if (options.target.gnu)
  {
    std::string flags = targetFlags(options).substr(1) + " -fuse-ld=lld -Wl,--gc-sections -luser32 -ladvapi32 -lshell32 -lole32 -lws2_32";
    flags += gnuRuntime(options);
    flags += libraryCaseDirectory(clangPath(), options.target.triple);
    if (!options.sysroot.empty())
      flags += " \"--sysroot=" + options.sysroot + "\"";
    if (wideEntry)
      flags += " -municode";
    // The PDB, with the .natvis in it as the MSVC link's /NATVIS puts it (lld's COFF option through
    // --Xlink): Visual Studio shows the library's values in a windows-gnu build too, the bundled
    // toolchain's default.
    if (options.debugInfo)
    {
      flags += " -g -Wl,--pdb= \"-Wl,--Xlink=/natvis:" + natvisPath(options) + "\"";
      for (const std::string& file : options.natvisFiles)
        flags += " \"-Wl,--Xlink=/natvis:" + file + "\"";
      if (options.optimize) // (the PDB named without its directory, as below)
        flags += " -Wl,--Xlink=/pdbaltpath:%_PDB%";
    }
    for (const std::string& symbol : options.keepSymbols)
      flags += " -Wl,-u," + symbol;
    for (const std::string& lib : options.libs)
      flags += " \"" + lib + "\"";
    return flags;
  }
  // macOS (design.md, "Targets and hosts", stage 8, M3): ld64.lld through clang's Darwin driver,
  // which passes -arch, -platform_version (the triple's oldest system and the SDK's version, read from
  // its SDKSettings.json), -syslibroot and -lSystem: the C library, its math, threads and the unwinder
  // are all libSystem's. The SDK goes as -isysroot (given --sysroot instead the driver reads no
  // version from it), unused code is dropped (-dead_strip, as --gc-sections), and ld64.lld signs arm64
  // output ad hoc itself (macOS runs no unsigned arm64 program). A symbol to keep has Mach-O's
  // leading underscore.
  if (options.target.mac())
  {
    std::string flags = targetFlags(options).substr(1) + " -fuse-ld=lld -Wl,-dead_strip";
    // The personality's weak references to kpp_eh_offset and kpp_eh_terminate (Runtime.cpp), which a
    // program without a throw of its own does not define: ELF makes a missing one null, ld64.lld wants
    // every undefined symbol found unless told (-U), and dyld then binds the missing weak import to
    // null too. A program that defines them links them as any symbol.
    flags += itaniumRuntime(options); // (an empty one, its error printed, fails the link by name)
    flags += " -Wl,-U,_kpp_eh_offset -Wl,-U,_kpp_eh_terminate";
    if (!options.sysroot.empty())
      flags += " -isysroot \"" + options.sysroot + "\"";
    for (const std::string& symbol : options.keepSymbols)
      flags += " -Wl,-u,_" + symbol;
    for (const std::string& lib : options.libs)
      flags += " \"" + lib + "\"";
    return flags;
  }
  // Another target (ELF): ld.lld with the target's sysroot, unused sections dropped as /OPT:REF
  // drops them, the C library's math and threads (libm and pthreads are libraries of their own
  // there), --lib files.
  if (!options.target.windows())
  {
    std::string flags = targetFlags(options).substr(1) + " -fuse-ld=lld -Wl,--gc-sections -lm -pthread";
    // FreeBSD links statically: kpp.net's programs then need none of the host's shared libraries,
    // and keep running when NearlyFreeSpeech.NET upgrades its realm (design.md, "Targets and
    // hosts", stage 6).
    if (options.target.os == Target::Os::FreeBSD)
      flags += " -static";
    flags += itaniumRuntime(options); // (an empty one, its error printed, fails the link by name)
    if (!options.sysroot.empty())
      flags += " \"--sysroot=" + options.sysroot + "\"";
    for (const std::string& symbol : options.keepSymbols)
      flags += " -Wl,-u," + symbol;
    for (const std::string& lib : options.libs)
      flags += " \"" + lib + "\"";
    return flags;
  }
  // (an optimized build's /OPT:ICF said, not left to the default, which /DEBUG turns off: a release
  // build's code is the same with its PDB and without)
  std::string flags = options.optimize           ? "-Wl,/OPT:REF -Wl,/OPT:ICF"
                      : options.incrementalLink ? "-Wl,/OPT:NOREF -Wl,/INCREMENTAL"
                                                : "-Wl,/OPT:REF -Wl,/OPT:NOICF";
  flags += " -llegacy_stdio_definitions -luser32 -ladvapi32 -lshell32 -lole32 -lws2_32";
  if (bundledToolchain()) // its lld-link (Visual Studio's link.exe is clang's default)
    flags += " -fuse-ld=lld";
  if (wideEntry)
    flags += " -Wl,/ENTRY:wmainCRTStartup";
  if (options.debugInfo)
  {
    flags += " -Wl,/DEBUG \"-Wl,/NATVIS:" + natvisPath(options) + "\""; // (the .natvis in the PDB: Visual Studio loads it with it)
    for (const std::string& file : options.natvisFiles) // (the program's own: --natvis, build.kpp's natvis)
      flags += " \"-Wl,/NATVIS:" + file + "\"";
    // An optimized build (release, final: debug information by default, kovarex 2026-11-07) names its
    // PDB without the directory, so that the executable holds no path of this machine; the debugger
    // and dbghelp (a crash report's names) find it beside the executable.
    if (options.optimize)
      flags += " -Wl,/PDBALTPATH:%_PDB%";
  }
  for (const std::string& symbol : options.keepSymbols)
    flags += " -Wl,/INCLUDE:" + symbol;
  for (const std::string& lib : options.libs)
    flags += " \"" + lib + "\"";
  return flags;
}

// An optimized ELF build's debug information goes into a file of its own, NAME.debug beside the
// executable, as a Windows build's goes into its PDB (kovarex 2026-11-07: release builds have debug
// information for crash reports, the code unchanged): llvm-objcopy copies the DWARF out, then strips
// it from the executable and leaves a .gnu_debuglink naming the file, where gdb, lldb and a crash
// report's symbolizer look for it. A debug build keeps its DWARF in the executable.
std::string splitDebugInfoCommand(const Options& options)
{
  // (macOS keeps its DWARF in the module objects, which the executable's debug map names, as a Mac
  // build does until dsymutil gathers it into a .dSYM: the macOS plan's M5)
  if (!options.debugInfo || !options.optimize || options.target.windows() || options.target.mac() || options.exe.empty())
    return "";
  std::string objcopy = std::string("llvm-objcopy") + host::executableSuffix();
  std::filesystem::path clang = clangPath();
  std::error_code ec;
  std::filesystem::path tool;
  for (const std::filesystem::path& dir : {clang.parent_path(), std::filesystem::canonical(clang, ec).parent_path()})
    if (!dir.empty() && std::filesystem::is_regular_file(dir / objcopy, ec))
    {
      tool = dir / objcopy;
      break;
    }
  if (tool.empty())
    tool = host::findOnPath("llvm-objcopy");
  if (tool.empty())
  {
    fprintf(stderr, "error: an optimized build's debug information goes into %s.debug with llvm-objcopy, which is neither beside %s nor on "
                    "PATH: install LLVM's tools (apt install llvm, dnf install llvm, pkg install llvm), or build without --debug-info\n",
            options.exe.c_str(), clang.string().c_str());
    codegenExit = 1;
    return "";
  }
  std::string debugFile = options.exe + ".debug";
  return " && \"" + tool.string() + "\" --only-keep-debug \"" + options.exe + "\" \"" + debugFile + "\" && \"" + tool.string() +
         "\" --strip-debug \"--add-gnu-debuglink=" + debugFile + "\" \"" + options.exe + "\"";
}

std::string ltoLinkFlags(const Options& options)
{
  std::string flags;
  if (!options.optimize)
    return flags;
  bool msvc = options.target.msvc();
  std::string partitions = std::to_string(options.ltoPartitions ? options.ltoPartitions : std::max(1u, options.threads));
  std::string level = std::to_string(options.optLevel);
  // (ld64.lld has no LTO partitions: a full LTO link for macOS is one)
  if (options.fullLTO && !options.target.gnu && !options.target.mac())
    flags += msvc ? " -Wl,/opt:lldltopartitions=" + partitions : " -Wl,--lto-partitions=" + partitions;
  if (options.optLevel != 2)
    flags += msvc ? " -Wl,/opt:lldlto=" + level : " -Wl,--lto-O" + level;
  // MinGW-w64's startup walks .ctors from its end, and LLVM writes the list reversed for that only
  // when told it has .ctors: lld's LTO back end assumes .init_array, so a release build's globals
  // were built last to first.
  if (options.target.gnu)
    flags += " -Wl,-mllvm,-use-ctors";
  return flags.empty() ? flags : flags.substr(1);
}

// The program's global definition of main or wmain (`name`), or null.
static const FunctionDecl* entryDefinition(const std::vector<std::unique_ptr<ParsedFile>>& parsed, const SymbolTable& symbols, std::string_view name)
{
  for (const auto& file : parsed)
    for (Decl* d : file->decls)
      if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body)
      {
        const QualifiedName& q = static_cast<FunctionDecl*>(d)->name;
        if (q.parts.size() == 1 && q.parts[0].special == 0 && q.parts[0].symbol != UINT32_MAX && symbols.name(q.parts[0].symbol) == name)
          return static_cast<FunctionDecl*>(d);
      }
  return nullptr;
}

bool definesWideEntryOnly(const std::vector<std::unique_ptr<ParsedFile>>& parsed, const SymbolTable& symbols)
{
  return !entryDefinition(parsed, symbols, "main") && entryDefinition(parsed, symbols, "wmain");
}

bool wideEntry(const std::vector<std::unique_ptr<ParsedFile>>& parsed, const SymbolTable& symbols, const Target& target)
{
  if (!target.windows())
    return false;
  if (const FunctionDecl* main = entryDefinition(parsed, symbols, "main"))
    return !main->params.empty();
  return entryDefinition(parsed, symbols, "wmain") != nullptr;
}

std::vector<std::string> splitFlags(const std::string& text)
{
  std::vector<std::string> out;
  std::string current;
  bool quoted = false;
  for (char c : text)
  {
    if (c == '"')
      quoted = !quoted;
    else if (c == ' ' && !quoted)
    {
      if (!current.empty())
        out.push_back(current);
      current.clear();
    }
    else
      current += c;
  }
  if (!current.empty())
    out.push_back(current);
  return out;
}
}
