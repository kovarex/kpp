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

std::string linkFlags(const Options& options, bool wideEntry)
{
  // Windows' GNU dialect (windows-gnu): lld's GNU driver, unused sections dropped as /OPT:REF
  // drops them, the Windows libraries programs commonly reach, ucrtbase's functions MinGW-w64's
  // import libraries leave out, -municode for a wmain, --lib files.
  if (options.target.gnu)
  {
    std::string flags = targetFlags(options).substr(1) + " -fuse-ld=lld -Wl,--gc-sections -luser32 -ladvapi32 -lshell32 -lole32 -lws2_32";
    flags += gnuRuntime(options);
    if (!options.sysroot.empty())
      flags += " \"--sysroot=" + options.sysroot + "\"";
    if (wideEntry)
      flags += " -municode";
    if (options.debugInfo)
      flags += " -g -Wl,--pdb=";
    for (const std::string& symbol : options.keepSymbols)
      flags += " -Wl,-u," + symbol;
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
  std::string flags = options.optimize           ? "-Wl,/OPT:REF"
                      : options.incrementalLink ? "-Wl,/OPT:NOREF -Wl,/INCREMENTAL"
                                                : "-Wl,/OPT:REF -Wl,/OPT:NOICF";
  flags += " -llegacy_stdio_definitions -luser32 -ladvapi32 -lshell32 -lole32 -lws2_32";
  if (bundledToolchain()) // its lld-link (Visual Studio's link.exe is clang's default)
    flags += " -fuse-ld=lld";
  if (wideEntry)
    flags += " -Wl,/ENTRY:wmainCRTStartup";
  if (options.debugInfo)
    flags += " -Wl,/DEBUG \"-Wl,/NATVIS:" + natvisPath(options) + "\""; // (the .natvis in the PDB: Visual Studio loads it with it)
  for (const std::string& symbol : options.keepSymbols)
    flags += " -Wl,/INCLUDE:" + symbol;
  for (const std::string& lib : options.libs)
    flags += " \"" + lib + "\"";
  return flags;
}

std::string ltoLinkFlags(const Options& options)
{
  std::string flags;
  if (!options.optimize)
    return flags;
  bool msvc = options.target.msvc();
  std::string partitions = std::to_string(options.ltoPartitions ? options.ltoPartitions : std::max(1u, options.threads));
  std::string level = std::to_string(options.optLevel);
  if (options.fullLTO && !options.target.gnu)
    flags += msvc ? " -Wl,/opt:lldltopartitions=" + partitions : " -Wl,--lto-partitions=" + partitions;
  if (options.optLevel != 2)
    flags += msvc ? " -Wl,/opt:lldlto=" + level : " -Wl,--lto-O" + level;
  return flags.empty() ? flags : flags.substr(1);
}

bool definesWideEntryOnly(const std::vector<std::unique_ptr<ParsedFile>>& parsed, const SymbolTable& symbols)
{
  bool wide = false;
  for (const auto& file : parsed)
    for (Decl* d : file->decls)
      if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body)
      {
        const QualifiedName& name = static_cast<FunctionDecl*>(d)->name;
        if (name.parts.size() != 1 || name.parts[0].special != 0 || name.parts[0].symbol == UINT32_MAX)
          continue;
        std::string_view spelled = symbols.name(name.parts[0].symbol);
        if (spelled == "main")
          return false;
        wide = wide || spelled == "wmain";
      }
  return wide;
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
