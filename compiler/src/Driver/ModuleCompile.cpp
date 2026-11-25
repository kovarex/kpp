// The driver: codegen's modules compiled (clang or the back end in kpp, the shared object cache)
// and linked: runCodegen; its declarations in ModuleCompile.hpp (Driver.hpp). A translation unit of
// its own since 2026-11-05 (main.cpp split, stage 2), moved without a change.
#include "Driver.hpp"
#include <filesystem>

namespace driver
{
// text with every function definition given unwind information (LLVM's `uwtable` after its
// parameters, where a definition's function attributes go), as clang gives every function on x86-64.
// Without it an optimized build's back end leaves a function the optimizer proves cannot throw
// without its unwind information (Windows x64's .pdata, an ELF's .eh_frame), so a crash in it is
// walked as a leaf's: on Windows the walk ended there, and the process died with no crash report, its
// unhandled-exception filter never reached (E's finding, 2026-11-07). For a module clang compiles (an
// unoptimized build in kpp keeps every function's tables, none proven unable to throw).
// text without its COMDATs, for Mach-O (macOS), which has none: LLVM refuses a module that names one
// there. A shared definition stays linkonce_odr or weak_odr, which the Mach-O linker coalesces by name
// (design.md, "Targets and hosts", the COMDAT row). Each `$NAME = comdat any` line goes, and in a
// definition's or a global's line `comdat` and `comdat($NAME)` outside quotes, with the space or comma
// before it.
std::string withoutComdats(const std::string& text)
{
  std::string out;
  out.reserve(text.size());
  size_t from = 0;
  while (from < text.size())
  {
    size_t end = text.find('\n', from);
    end = end == std::string::npos ? text.size() : end + 1;
    std::string_view line(text.data() + from, end - from);
    from = end;
    std::string_view bare = line.substr(0, line.find_last_not_of("\r\n") + 1);
    if (bare.starts_with("$") && bare.ends_with(" = comdat any"))
      continue;
    if (!bare.starts_with("define ") && !bare.starts_with("@"))
    {
      out += line;
      continue;
    }
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i)
    {
      char c = line[i];
      if (c == '"')
        quoted = !quoted;
      if (!quoted && line.compare(i, 7, " comdat") == 0 && (i + 7 == line.size() || !(isalnum((unsigned char)line[i + 7]) || line[i + 7] == '_' || line[i + 7] == '.')))
      {
        size_t next = i + 7;
        if (next < line.size() && line[next] == '(') // comdat($NAME): to its parenthesis, a quoted name's included
        {
          bool inner = false;
          for (; next < line.size() && (inner || line[next] != ')'); ++next)
            if (line[next] == '"')
              inner = !inner;
          ++next;
        }
        if (!out.empty() && out.back() == ',')
          out.pop_back(); // (`, comdat`)
        i = next - 1;
        continue;
      }
      out += c;
    }
  }
  return out;
}

std::string withUnwindTables(const std::string& text, const char* attributes);

// A module's text as its target takes it: every function with unwind tables (and the sanitizer's
// attribute, without which clang's instrumentation leaves a function out: --sanitize), and no COMDAT
// on Mach-O. On macOS the tables are compact unwind's, as clang makes them there: synchronous
// (`uwtable(sync)`, a call's place alone, which an exception needs) and every function that calls keeping its
// frame record (x29: `-mframe-pointer=non-leaf` on arm64, `all` on x86-64, Apple's ABI), so that a frame, its
// personality and LSDA are compact unwind's encoding. Asynchronous tables describe the epilogues too, which
// compact unwind cannot, and without a frame record an optimized frame is none it can: that unwind information
// was DWARF's, an FDE, which ld64.lld dropped from a ThinLTO build's objects, main's among them, whose handlers no
// exception then reached ("unhandled exception", thread O's macOS release runs, 2026-11-13).
std::string forTarget(const std::string& text, const Options& options)
{
  bool mac = options.target.mac();
  std::string attributes = mac ? " uwtable(sync)" : " uwtable";
  attributes += options.sanitizeAddress ? " sanitize_address" : options.sanitizeThread ? " sanitize_thread" : "";
  if (mac)
    attributes += options.target.arm64() ? " \"frame-pointer\"=\"non-leaf\"" : " \"frame-pointer\"=\"all\"";
  std::string out = mac ? withoutComdats(withUnwindTables(text, attributes.c_str())) : withUnwindTables(text, attributes.c_str());
  if (options.sanitizeThread) // (the vtable pointer accesses' TBAA tag, codegen::vptrAccess)
    out += vptrAccessMetadata;
  return out;
}

std::string withUnwindTables(const std::string& text, const char* attributes)
{
  std::string out;
  out.reserve(text.size() + text.size() / 64);
  size_t from = 0;
  for (size_t at = text.starts_with("define ") ? 0 : text.find("\ndefine "); at != std::string::npos; at = text.find("\ndefine ", at + 1))
  {
    size_t name = text.find('@', at + 1);
    if (name == std::string::npos)
      break;
    size_t open = text[name + 1] == '"' ? text.find('"', name + 2) : name; // (a quoted name's `\22` is no quote)
    open = open == std::string::npos ? open : text.find('(', open);
    size_t close = std::string::npos;
    for (size_t i = open, depth = 0; i != std::string::npos && i < text.size() && text[i] != '\n'; ++i)
      if (text[i] == '(')
        ++depth;
      else if (text[i] == ')' && --depth == 0)
      {
        close = i;
        break;
      }
    if (close == std::string::npos)
      continue; // (no definition kpp writes)
    out.append(text, from, close + 1 - from);
    out += attributes;
    from = close + 1;
  }
  out.append(text, from, std::string::npos);
  return out;
}

size_t reportCheckerErrors(GlobalScope& global, bool laidOut, size_t instancesChecked)
{
  for (const std::string& w : global.checkerWarnings())
    fprintf(stderr, "%s\n", global.withAliasNames(w).c_str()); // (no error: the build goes on)
  size_t errors = 0;
  for (const std::string& e : global.checkerErrors(instancesChecked))
  {
    fprintf(stderr, "%s\n", global.withAliasNames(e).c_str()); // (`EntityID`, not `ID<EntityPrototype>`: design.md 8.3)
    codegenExit = 1;
    ++errors;
  }
  if (size_t waiting = global.layoutStaticAsserts().size())
    report(laidOut ? "static_assert: %zu on a class's size or alignment, checked with codegen's layouts\n"
                   : "static_assert: %zu on a class's size or alignment, left to the code generator's layout\n",
           waiting);
  if (size_t unused = global.unusedFailedAsserts())
    report("static_assert: %zu failed in template instances' functions never emitted (unused, so not errors, as in C++)\n", unused);
  return errors;
}

void reportPassWarnings(const Options& options)
{
  if (options.zeroNullWarnings)
    for (const std::string& w : takeZeroNullWarnings())
      fprintf(stderr, "%s\n", w.c_str());
  if (options.thisWarnings)
    for (const std::string& w : takeThisWarnings())
      fprintf(stderr, "%s\n", w.c_str());
  if (options.thisReferenceWarnings)
    for (const std::string& w : takeThisReferenceWarnings())
      fprintf(stderr, "%s\n", w.c_str());
  if (options.plainEnumWarnings)
    for (const std::string& w : takePlainEnumWarnings())
      fprintf(stderr, "%s\n", w.c_str());
}

void reportReturnCopyWarnings(Build& build)
{
  if (!build.options.returnCopyWarnings || !build.global)
    return;
  std::vector<ParsedFile*> files;
  for (const std::unique_ptr<ParsedFile>& p : build.parsed)
    files.push_back(p.get());
  for (const std::string& w : build.global->returnCopyWarnings(files))
    fprintf(stderr, "%s\n", w.c_str());
}

void reportAccessWarnings(Build& build)
{
  if (!build.options.accessWarnings)
    return;
  std::vector<ParsedFile*> files;
  for (const std::unique_ptr<ParsedFile>& p : build.parsed)
    files.push_back(p.get());
  std::string summary;
  for (const std::string& w : build.global->accessReport(files, summary))
    fprintf(stderr, "%s\n", w.c_str());
  report("%s\n", summary.c_str());
}

  // KPP_OBJECT_CACHE=DIR: module objects shared by every output directory, keyed by the SHA-256 of all
  // an object is made from: the module's text and number, the optimisation level, the compile flags and
  // the compiler (clang's path, size and time, or this kpp's for LLVM's back end in process). The spec
  // suite's snippets share their standard library's modules (run_spec.py sets it): of 38 snippets'
  // module texts (--modules 4), modules 0 to 2 were the same in all and the instance pieces in about
  // half, 74% of the text; its debug run took 97 s with a cache it began empty, 143-149 s without. A
  // writer renames a file of its own into place, so a reader never sees half an object, and several
  // builds may share one directory at once. A hit is written as a new file, with a new time: link.exe's
  // incremental link tells a changed object by its time. Every few uses the directory is trimmed to the
  // most recently used 768 MB once it is past 1 GB (a hit counts as a use): the spec suite's debug run
  // leaves 122 MB.
  namespace objectcache
  {
    std::string stamp(const std::filesystem::path& file)
    {
      std::error_code ec;
      auto size = std::filesystem::file_size(file, ec);
      auto time = std::filesystem::last_write_time(file, ec);
      return file.string() + " " + std::to_string(ec ? 0 : size) + " " + std::to_string(ec ? 0 : int64_t(time.time_since_epoch().count()));
    }

    std::string key(const std::string& text, const std::string& options, const std::string& compiler)
    {
      Sha256 h;
      h.add("kpp object 1\n");
      h.add(compiler);
      h.add("\n");
      h.add(options);
      h.add("\n");
      h.add(text);
      return h.hex();
    }

    // The cached object of key written to dest, a new file; false when there is none (or it went).
    bool fetch(const std::filesystem::path& dir, const std::string& key, const std::string& dest)
    {
      std::filesystem::path cached = dir / (key + ".o");
      std::string bytes;
      if (!readFile(cached, bytes) || bytes.empty())
        return false;
      FILE* out = fopen(dest.c_str(), "wb");
      if (!out)
        return false;
      bool written = fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
      written = fclose(out) == 0 && written;
      std::error_code ec;
      if (written)
        std::filesystem::last_write_time(cached, std::filesystem::file_time_type::clock::now(), ec); // (used: trim keeps it)
      return written;
    }

    void store(const std::filesystem::path& dir, const std::string& key, const std::string& object)
    {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      std::ostringstream own;
      own << key << "." << host::processId() << "." << std::this_thread::get_id() << ".part";
      std::filesystem::path part = dir / own.str();
      std::filesystem::copy_file(object, part, std::filesystem::copy_options::overwrite_existing, ec);
      if (!ec)
        std::filesystem::rename(part, dir / (key + ".o"), ec); // (one there already: the same object)
      if (ec)
        std::filesystem::remove(part, ec);
    }

    // The most recently used objects up to `keep` bytes stay; files of builds that died half way
    // (`.part`) go once they are a day old.
    void trim(const std::filesystem::path& dir, uintmax_t limit, uintmax_t keep)
    {
      std::error_code ec;
      struct Entry { std::filesystem::path path; std::filesystem::file_time_type time; uintmax_t size; };
      std::vector<Entry> entries;
      uintmax_t total = 0;
      auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
      for (const auto& e : std::filesystem::directory_iterator(dir, ec))
      {
        std::error_code fe;
        auto time = e.last_write_time(fe);
        if (e.path().extension() == ".part")
        {
          if (!fe && time < old)
            std::filesystem::remove(e.path(), fe);
          continue;
        }
        uintmax_t size = e.file_size(fe);
        total += size;
        entries.push_back({e.path(), time, size});
      }
      if (total <= limit)
        return;
      std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
      for (const Entry& e : entries)
      {
        if (total <= keep)
          break;
        std::filesystem::remove(e.path, ec);
        total -= e.size;
      }
    }
  }

// The clang driver's one job for a response file (`clang -###`), as its arguments, the tool first: each as the
// driver quotes it, a backslash or a quote in one escaped with a backslash. None when the driver failed, or
// gave no job, or more than one.
static std::vector<std::string> driverJob(const std::string& clang, const std::string& responsePath)
{
  std::string said;
  if (FILE* pipe = host::openPipe("\"" + clang + "\" -### @\"" + responsePath + "\" 2>&1", "r"))
  {
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, pipe)) > 0)
      said.append(chunk, n);
    if (host::closePipe(pipe) != 0)
      return {};
  }
  std::vector<std::string> args;
  for (size_t at = 0; at < said.size();)
  {
    size_t end = said.find('\n', at);
    std::string_view line(said.data() + at, (end == std::string::npos ? said.size() : end) - at);
    at = end == std::string::npos ? said.size() : end + 1;
    if (line.size() < 2 || line[0] != ' ' || line[1] != '"')
      continue;
    if (!args.empty())
      return {}; // (two jobs: not one link)
    for (size_t i = 1; i < line.size();)
    {
      if (line[i] == ' ' || line[i] == '\r')
      {
        ++i;
        continue;
      }
      if (line[i] != '"')
        return {};
      std::string arg;
      for (++i; i < line.size() && line[i] != '"'; ++i)
        arg += line[i] == '\\' && i + 1 < line.size() ? line[++i] : line[i];
      if (i >= line.size())
        return {};
      ++i;
      args.push_back(std::move(arg));
    }
  }
  return args;
}

std::string directLinkCommand(const std::string& clang, const std::string& exe, const std::string& objects, const std::string& flags)
{
  Sha256 key;
  // (3: the driver is asked without the objects, and its arguments are kept with the objects' place; 2:
  // the command is kept without cmd's outer quotes, which host::shell adds since 5bf9a63c; a kept one of
  // 1 has them and was run quoted twice, '""C:\Program' is not recognized)
  key.add("kpp direct link 3\n");
  key.add(objectcache::stamp(clang));
  for (const char* name : {"PATH", "LIB", "LIBPATH", "INCLUDE", "VCINSTALLDIR", "VCToolsInstallDir", "VSINSTALLDIR", "VisualStudioVersion",
                           "WindowsSdkDir", "WindowsSDKVersion", "UniversalCRTSdkDir", "UCRTVersion", "Platform"})
  {
    const char* value = getenv(name);
    key.add(std::string("\n") + name + "=" + (value ? value : ""));
  }
  key.add("\n");
  key.add(flags);
  std::string keyText = key.hex();
  std::string cachePath = exe + ".link.direct";
  auto write = [](const std::string& path, const std::string& text) // (one that fails: the link goes through clang)
  {
    FILE* out = fopen(path.c_str(), "wb");
    if (!out)
      return false;
    bool written = fwrite(text.data(), 1, text.size(), out) == text.size();
    return fclose(out) == 0 && written;
  };
  // Where the objects go among the kept arguments: the one line that is no argument (each of those is quoted).
  static constexpr std::string_view place = "<objects>\n";
  auto placeIn = [](const std::string& arguments)
  {
    size_t at = arguments.find(place);
    while (at != std::string::npos && at != 0 && arguments[at - 1] != '\n')
      at = arguments.find(place, at + 1);
    return at;
  };
  // What is kept: the key's line, the command's, then link.exe's arguments.
  std::string command, arguments;
  std::string cached = readWholeFile(cachePath);
  if (size_t end = cached.find('\n'); end != std::string::npos && cached.compare(0, end, keyText) == 0)
    if (size_t next = cached.find('\n', end + 1); next != std::string::npos)
    {
      command = cached.substr(end + 1, next - end - 1);
      arguments = cached.substr(next + 1);
    }
  if (command.empty() || placeIn(arguments) == std::string::npos)
  {
    // The driver is asked with a linker option where the objects' lines would be: it passes that on as it
    // is, at the place an object given there goes to.
    static constexpr std::string_view standIn = "kpp-link-objects";
    std::string askPath = exe + ".link.ask.rsp";
    if (!write(askPath, "-Wl," + std::string(standIn) + "\n" + flags))
      return "";
    std::vector<std::string> args = driverJob(clang, askPath);
    std::error_code ec;
    std::filesystem::remove(askPath, ec);
    if (args.size() < 2)
      return "";
    std::string tool = std::filesystem::path(args[0]).filename().string();
    for (char& c : tool)
      c = char(tolower((unsigned char)c));
    if (tool != "link.exe")
      return "";
    arguments.clear();
    bool placed = false;
    for (size_t i = 1; i < args.size(); ++i)
    {
      if (args[i] == standIn && !placed)
      {
        placed = true;
        arguments += place;
        continue;
      }
      if (args[i] == standIn || args[i].find('"') != std::string::npos || (!args[i].empty() && args[i].back() == '\\'))
        return ""; // (the stand-in twice: not this one's alone)
      arguments += "\"" + args[i] + "\"\n";
    }
    if (!placed)
      return "";
    command = "\"" + args[0] + "\" @\"" + exe + ".link.exe.rsp\"";
    if (!write(cachePath, keyText + "\n" + command + "\n" + arguments))
      return "";
  }
  // link.exe's arguments with this build's objects, written unless the file holds them already.
  size_t at = placeIn(arguments);
  std::string linkArguments = arguments.substr(0, at) + objects + arguments.substr(at + place.size());
  std::string linkResponse = exe + ".link.exe.rsp";
  if (readWholeFile(linkResponse) != linkArguments && !write(linkResponse, linkArguments))
    return "";
  return command;
}

// A compile server's edit pieces (CodegenOptions::editPieces): the files its builds edited, in the order they were first
// edited, each of which a debug build in pieces gives an object of its own; at most maxEditPieces (the files edited later
// stay in their modules' program pieces).
static std::vector<std::string>& serverEditPieces()
{
  static std::vector<std::string> paths;
  return paths;
}
constexpr size_t maxEditPieces = 64;

void runCodegen(Build& build)
{
  std::vector<std::unique_ptr<ParsedFile>>& parsed = build.parsed;
  GlobalScope& global = *build.global;
  SymbolTable& symbols = *build.symbols;
  const Options& options = build.options;
  using Clock = std::chrono::steady_clock;
  if (!options.target.windows() && definesWideEntryOnly(parsed, symbols))
  {
    fprintf(stderr, "error: the program's entry is wmain, which is Windows'; for --target %s it defines main\n", options.target.name.c_str());
    codegenExit = 1;
    return;
  }
  // --emit-llvm wants the whole program in one file; an executable is built from several modules
  // compiled in parallel (one per group of files), unless --modules 1. A debug build has four per
  // thread: an edit compiles its module again, and the game's leaf edit through the server took
  // 1.60 to 1.91 s with 64 modules, 1.39 to 1.51 with 128, 1.28 to 1.41 with 256 (its clean build
  // 11.6, 11.4 and 14.5 s; docs/compile-speed.md, "The debug edit loop"). A release build has two
  // (more made its ThinLTO link slower).
  unsigned perThread = options.optimize ? 2 : 4;
  unsigned modules = options.modules ? options.modules : unsigned(std::min<size_t>(parsed.size(), perThread * std::max(1u, options.threads)));
  if (!options.emitLLVM.empty())
    modules = 1;
  auto start = Clock::now();
  // A compile server's incremental build: codegen's kept session emits again only what changed
  // (rebuildLLVM); it declines (an error, a change to the shared declarations) and the program is
  // generated in full, as when nothing is kept.
  CodegenResult cg;
  bool rebuilt = false;
  global.setCodeGenerated(true);
  build.codegenBehind = false;
  const unsigned pieceModules = codegenOutputModules(modules, options.debug, options.optimize); // (a debug build's modules in two pieces each)
  // A compile server's debug build in pieces: the files it edited have a program piece each (serverEditPieces), the program's
  // files alone (one taken out goes), and a file edited now gets one (its module assembled again without it).
  const bool editPieces = pieceModules != modules && servingBuild() && !options.exe.empty() && !options.check && options.emitLLVM.empty();
  std::vector<std::string>& edits = serverEditPieces();
  if (!editPieces)
    edits.clear();
  else
    std::erase_if(edits, [&](const std::string& path)
    {
      return std::none_of(parsed.begin(), parsed.end(), [&](const std::unique_ptr<ParsedFile>& p) { return p->path == path; });
    });
  const unsigned keptEdits = build.codegen ? codegenEditPieces(*build.codegen) : 0;
  build.firstEditPiece = editPieces ? pieceModules : size_t(-1);
  if (build.rebuildCodegen && build.codegen && build.moduleBytes.size() == pieceModules + keptEdits && keptEdits == (editPieces ? edits.size() : 0))
  {
    if (editPieces)
    {
      std::vector<std::string> wanted, added;
      for (const std::string& path : build.editedFiles)
        if (edits.size() + wanted.size() < maxEditPieces && std::find(edits.begin(), edits.end(), path) == edits.end() &&
            std::find(wanted.begin(), wanted.end(), path) == wanted.end())
          wanted.push_back(path);
      addEditPieces(*build.codegen, wanted, added);
      edits.insert(edits.end(), added.begin(), added.end());
      if (!added.empty())
        logBuildNote("codegen: " + std::to_string(added.size()) + " file(s) given a program piece of their own, " + std::to_string(edits.size()) + " in all");
    }
    cg = rebuildLLVM(*build.codegen, build.changedBodies, build.movedLines, build.movedPositions, build.replacedBodies, build.addedFunctions, build.removedFunctions,
                     build.changedLayouts, build.instancesOf, build.changedGlobals, build.droppedClasses, rebuilt, !options.check);
    logBuildNote(!rebuilt              ? "codegen: rebuildLLVM declined (" + cg.declined + "), the program generated in full"
                 : cg.failed()        ? "codegen: the session kept across " + std::to_string(cg.errors.size() + cg.hiddenErrors) + " error(s), " +
                                            std::to_string(cg.emittedAgain) + " function(s) emitted again"
                                      : "codegen: " + std::to_string(std::count(cg.changedModules.begin(), cg.changedModules.end(), true)) +
                                            " module(s) assembled again, " + std::to_string(cg.emittedAgain) + " function(s) emitted again" +
                                            (cg.droppedUnreached ? ", " + std::to_string(cg.droppedUnreached) + " made on demand that nothing names now dropped" : std::string()) +
                                            (cg.unreachedAsked > 0 ? ", the texts asked for those in " + std::to_string(cg.unreachedAsked).substr(0, 5) + " s" : std::string()));
  }
  build.rebuildCodegen = false;
  build.editedFiles.clear();
  const unsigned outputModules = pieceModules + unsigned(editPieces ? edits.size() : 0);
  // The objects, EXE.k.o for output module k, and what compiles them: declared before codegen, as
  // step 4's P2 compiles a debug build's program pieces while codegen emits the instances.
  // A release build of several modules is a ThinLTO build: the modules become bitcode, and the
  // link optimises them in parallel with what each needs from the others imported, so that calls
  // into another module's instantiations inline as they do in one module (optimised separately,
  // the library benchmarks in compiler/bench/stdlib ran up to 20 times slower). --lto full makes
  // them (one module too) bitcode for one optimization over the whole program in the link, with
  // the C libraries' bitcode (build.kpp's final configuration builds them with -flto).
  bool fullLTO = options.optimize && options.fullLTO;
  bool thinLTO = options.optimize && modules > 1 && !fullLTO;
  const std::string optLevel = "-O" + std::to_string(options.optLevel);
  const std::string levelText = !options.optimize ? "-O0" : fullLTO ? optLevel + " -flto" : thinLTO ? optLevel + " -flto=thin" : optLevel;
  const char* level = levelText.c_str();
  // Every function and global in its own section (compileFlags, the back end in kpp alike), but in
  // an --incremental-link build: its link (link.exe /INCREMENTAL, /OPT:NOREF) keeps every function,
  // so the sections drop nothing there and cost every edit's link (the game's 650,000 section
  // contributions in its PDB, half its .ilk: about a fifth of the link; docs/debugging.md).
  const bool functionSections = !options.incrementalLink;
  // Every function in its own section, so that the linker drops the ones nobody calls (and
  // the libraries they would need): a program pays only for what it uses. (Not in an
  // --incremental-link build, whose link keeps them all: functionSections above.)
  // (-ffp-contract=off: no multiply-add fused, every operation rounded as written; the game's
  // /fp:strict, design.md section 2)
  // -fverify-intermediate-code: LLVM's verifier checks the module, as the back end in kpp does
  // (src/backend.cpp). A release clang passes -disable-llvm-verifier by default, which took an IR
  // bug of kpp's in silently: at -O0 the module compiled (exit 0), at -O2 clang hung on it
  // (KPP_TEST_BROKEN_MODULE's use its definition does not dominate, tests\verifier.ps1).
  // macOS: compact unwind for the functions whose personality is kpp's, which LLVM gives only Darwin's own
  // personalities by itself, the others DWARF's FDEs (forTarget; ltoLinkFlags for a ThinLTO link's back end).
  std::string compileFlags = functionSections ? "-ffunction-sections -fdata-sections -ffp-contract=off -fverify-intermediate-code"
                                              : "-ffp-contract=off -fverify-intermediate-code";
  if (options.target.mac())
    compileFlags += " -femit-compact-unwind-non-canonical";
  // An unoptimized module's jumps in their long form at once (-mrelax-all), as the back end in kpp
  // assembles them (backend.cpp): no relaxation in the assembler's layout.
  if (!options.optimize)
    compileFlags += " -mrelax-all";
  auto writeFile = [&](const std::string& path, const std::string& text)
  {
    FILE* out = fopen(path.c_str(), "wb");
    if (!out)
    {
      fprintf(stderr, "cannot write %s\n", path.c_str());
      codegenExit = 1;
      return false;
    }
    fwrite(text.data(), 1, text.size(), out);
    fclose(out);
    return true;
  };
  std::vector<std::string> objects;
  for (unsigned k = 0; k < outputModules; ++k)
    objects.push_back(options.exe + "." + std::to_string(k) + ".o");
  std::atomic<int> failures{0}, cached{0}, patchedObjects{0}; // (patched: counted unchanged too, their code is)
  std::atomic<uint64_t> cppMicroseconds{0}, cppBytes{0};      // --via-cpp: the modules' writing as C++, summed over the threads
  struct ModuleFailure
  {
    unsigned module;
    int rc; // clang's exit code; 0: LLVM's back end in this process; -1: kpp's C++ back end (--via-cpp)
    std::string path, output;
  };
  std::vector<ModuleFailure> moduleFailures;
  std::mutex moduleFailuresMutex;
  // LLVM's back end in this process when kpp has it (src/backend.cpp, docs/codegen-speed.md): a
  // debug build's module for Windows (windows or windows-gnu, with --debug-info too) compiled from
  // its text in memory, no IR file written, no clang started. Its objects are clang's apart from
  // what names the compiler in CodeView (S_COMPILE3's back-end version, LF_BUILDINFO's tool and
  // command line, which kpp leaves empty). A release build (ThinLTO, lld), Linux and --clang-flags
  // (their objects not compared with clang's) stay clang's; so does any build with
  // KPP_BACKEND=clang (the comparison).
  const char* backendChoice = getenv("KPP_BACKEND");
  bool inProcess = inProcessBackend() && !options.optimize && options.target.windows() && options.clangFlags.empty() && !options.viaCpp &&
                   !(backendChoice && std::string(backendChoice) == "clang");
  // KPP_TEST_BROKEN_MODULE=K (run_tests' "rejected module"): module K gets a function LLVM's verifier
  // rejects (a use its definition does not dominate), as an IR bug of kpp's would be.
  // KPP_TEST_BROKEN_ASM=K: module K gets an instruction the assembler rejects, which the verifier
  // passes and only the back end finds (LLVM's diagnostics, not its verifier).
  const char* brokenModule = getenv("KPP_TEST_BROKEN_MODULE");
  const char* brokenAsm = getenv("KPP_TEST_BROKEN_ASM");
  // Whether module k keeps the last build's object (counted unchanged): its hash file has the hash of
  // its text (a KPP_TEST_BROKEN_* function added first), the optimisation level, --clang-flags and
  // whether its functions have sections of their own.
  // Otherwise hashText is what the hash file gets once the module is compiled.
  // The shared object cache (objectcache, KPP_OBJECT_CACHE): its directory, and what keys an object
  // besides its text: the compiler and the flags, compileFlags among them (the compile line's own: until
  // 2026-11-25 only what follows functionSections, so an object an older kpp's clang line made without
  // -mrelax-all was taken for one made with it, and a compile server's --verify-incremental reported a
  // patched kept object "not what its compile gives", spec kpp/server_moved_positions_of_sanitizer_checks
  // in a full gate's spec run whose cache an earlier kpp had filled).
  std::filesystem::path objectCacheDir;
  if (const char* dir = getenv("KPP_OBJECT_CACHE"); dir && *dir)
    objectCacheDir = dir;
  std::string objectCacheCompiler, objectCacheFlags;
  if (!objectCacheDir.empty())
  {
    objectCacheCompiler = inProcess ? "kpp " + objectcache::stamp(compilerPath()) : "clang " + objectcache::stamp(clangPath());
    if (options.viaCpp) // (the C++ is this kpp's writing of the module: another kpp writes another)
      objectCacheCompiler += " kpp " + objectcache::stamp(compilerPath());
    // (and the directory it is compiled in, which clang writes into the object's debug information as its compilation
    // directory: a snippet's object made from another directory differed from its compile there, and a compile server's
    // --verify-incremental reported the patched kept object "not what its compile gives", 2026-11-21)
    std::error_code cwdError;
    objectCacheFlags = targetFlags(options) + " " + compileFlags + " " + options.clangFlags + (functionSections ? " sections" : " no-sections") + (options.viaCpp ? " via-cpp" : "") +
                       (options.target.mac() ? " compact-unwind-non-canonical" : "") + " in " + std::filesystem::current_path(cwdError).string();
  }
  std::vector<std::string> objectCacheKeys(outputModules);
  std::atomic<int> sharedHits{0};
  auto unchangedObject = [&](size_t k, std::string& text, const char* level, std::string& hashText)
  {
    if (brokenModule && std::to_string(k) == brokenModule)
      text += "\ndefine void @kpp.test.broken() {\n  %a = add i32 %b, 1\n  %b = add i32 %a, 1\n  ret void\n}\n";
    if (brokenAsm && std::to_string(k) == brokenAsm)
      text += "\nmodule asm \"kpp_no_such_instruction\"\n";
    // A module whose text (and optimisation level) did not change keeps its object file.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text)
      h = (h ^ c) * 1099511628211ull;
    for (const char* c = level; *c; ++c)
      h = (h ^ (unsigned char)*c) * 1099511628211ull;
    for (unsigned char c : options.clangFlags)
      h = (h ^ c) * 1099511628211ull;
    if (!functionSections)
      h = (h ^ 0xff) * 1099511628211ull;
    if (options.viaCpp) // (an object made through C++ is not the one made from the IR, nor one another kpp wrote)
      for (unsigned char c : "via-cpp " + objectcache::stamp(compilerPath()))
        h = (h ^ c) * 1099511628211ull;
    hashText = std::to_string(h);
    std::error_code ec;
    if (readWholeFile(options.exe + "." + std::to_string(k) + ".hash") == hashText && std::filesystem::exists(objects[k], ec))
    {
      ++cached;
      return true;
    }
    // Another output's object of the same text, flags and compiler (the shared cache), and of the
    // same module number: an object holds its IR file's name (its .file symbol; a release build's
    // bitcode its source file name, which ThinLTO's names of internal functions are made from), so
    // that two modules of one link never share an object, nor one name (their texts alike: modules
    // with nothing in them).
    if (!objectCacheDir.empty())
    {
      objectCacheKeys[k] = objectcache::key(text, std::string(level) + " " + objectCacheFlags + " module " + std::to_string(k), objectCacheCompiler);
      if (objectcache::fetch(objectCacheDir, objectCacheKeys[k], objects[k]) && writeFile(options.exe + "." + std::to_string(k) + ".hash", hashText))
      {
        ++cached;
        ++sharedHits;
        return true;
      }
    }
    return false;
  };
  // The end of module k's compile, whichever compiler made its object (LLVM's back end in this process
  // or clang), written once for both (J's refactoring list, item 4): a module compiled gets its hash
  // file, so that the next build keeps its object, and goes into the shared cache; one that failed
  // leaves no object (none of an earlier build linked in its place) and is reported with what its
  // compiler said (code: clang's exit code, 0 for the back end's).
  auto moduleCompiled = [&](size_t k, const std::string& hashText)
  {
    writeFile(options.exe + "." + std::to_string(k) + ".hash", hashText);
    if (!objectCacheKeys[k].empty())
      objectcache::store(objectCacheDir, objectCacheKeys[k], objects[k]);
  };
  auto moduleFailed = [&](size_t k, int code, const std::string& llPath, std::string said)
  {
    ++failures;
    std::error_code removed;
    std::filesystem::remove(objects[k], removed);
    std::lock_guard lock(moduleFailuresMutex);
    moduleFailures.push_back({unsigned(k), code, llPath, std::move(said)});
  };
  // Module k compiled by LLVM's back end in this process; its text is written next to the executable
  // only when LLVM rejects it, for the report.
  auto compileInProcess = [&](size_t k, const std::string& text, const std::string& hashText)
  {
    std::string llPath = options.exe + "." + std::to_string(k) + ".ll";
    std::string object, error;
    BackendOptions backend;
    backend.path = llPath;
    backend.module = unsigned(k);
    backend.functionSections = functionSections;
    bool compiled = compileModule(text, backend, object, error);
    if (compiled && !writeFile(objects[k], object))
      error = "cannot write " + objects[k];
    if (!compiled || !error.empty())
    {
      writeFile(llPath, text); // (the text LLVM rejected, for the report)
      moduleFailed(k, 0, llPath, error);
    }
    else
      moduleCompiled(k, hashText);
  };
  // Step 4's P2 (docs/codegen-speed.md): a debug build of several modules whose objects LLVM's back end
  // in this process makes has its program pieces compiled while codegen emits the instances, on
  // backEnd's threads (at normal priority, BackEndPool): codegen hands each over at level 0's barrier
  // (PieceStream). The other modules are compiled once codegen ends, on the same threads.
  std::unique_ptr<BackEndPool> backEnd;
  std::vector<char> handedOver(outputModules);
  if (!rebuilt)
  {
    build.retiredCodegen = std::move(build.codegen);
    // (--check writes nothing: no IR assembled despite errors, so that a server keeps its session across them)
    bool despiteErrors = options.keepGoing && !options.check;
    PieceStream stream;
    // (Not in a build the checker's errors fail already, --keep-going aside: it compiles no object, as a
    // server's build that follows its kept session compiles none; a streamed piece's object was written
    // and --verify-incremental found the two builds' outputs different.)
    if (inProcess && outputModules != modules && !options.exe.empty() && options.emitLLVM.empty() && !options.check &&
        (options.keepGoing || !global.hasCheckerErrors()))
    {
      backEnd = std::make_unique<BackEndPool>(options.threads, std::max(1u, options.threads / 2)); // (half of them while codegen runs: BackEndPool)
      stream.piece = [&](unsigned k, std::string&& text)
      {
        handedOver[k] = 1;
        size_t weight = text.size(); // (the largest first: BackEndPool)
        backEnd->push([&, k, text = std::move(text)]() mutable
        {
          std::string hashText;
          if (!unchangedObject(k, text, level, hashText))
            compileInProcess(k, text, hashText);
        }, true, weight);
      };
    }
    // --debug-info: the sources the driver generated (the test runner's `<tests>.kpp`, not `<config>`) written beside the
    // executable, in kpp-generated\NAME (not NAME.something, which a script's `Remove-Item NAME.*` would meet), so that
    // the debug information names files that exist: a stack trace showed `C:\Factorio\<tests>.kpp(787): __kppRunWhole`
    // (kovarex, 2026-11-22). A file already holding the text is left as it is.
    std::string generatedSources;
    if (options.debugInfo && !options.exe.empty() && !options.check)
    {
      std::filesystem::path exe(options.exe);
      std::string directory = std::filesystem::absolute(exe.parent_path() / "kpp-generated" / exe.stem()).string();
      for (const std::unique_ptr<ParsedFile>& p : parsed)
        if (isGeneratedSource(p->path))
        {
          std::error_code ec;
          if (generatedSources.empty())
            std::filesystem::create_directories(directory, ec);
          generatedSources = directory;
          std::string file = generatedSourceFile(directory, p->path);
          if (readWholeFile(file) != p->text && !writeFile(file, std::string(p->text)))
            fprintf(stderr, "warning: cannot write %s, which the debug information names\n", file.c_str());
        }
    }
    CodegenOptions codegenOptions{.debug = options.debug,
                                  .moduleCount = modules,
                                  .despiteErrors = despiteErrors,
                                  .threads = options.codegenThreads ? options.codegenThreads : options.threads,
                                  .stubMissing = despiteErrors && options.stubMissing,
                                  .inlineTrivial = !options.noInline && !options.coverage, // (an inlined body would be counted as its caller's)
                                  .sanitizeUndefined = options.sanitizeUndefined,
                                  .sanitizeOverflow = options.sanitizeOverflow,
                                  .sanitizeThread = options.sanitizeThread,
                                  .coverage = options.coverage,
                                  .debugInfo = options.debugInfo,
                                  .generatedSources = generatedSources,
                                  .target = &options.target,
                                  .optimize = options.optimize != 0, // (resolved to 0 or 1 by now)
                                  .assemble = !options.check, // (a check writes nothing: no module text, CodegenOptions::assemble)
                                  .editPieces = editPieces ? edits : std::vector<std::string>()};
    cg = generateLLVM(parsed, global, symbols, codegenOptions, servingBuild() ? &build.codegen : nullptr, backEnd ? &stream : nullptr);
    cg.changedModules.clear();
    if (backEnd)
      backEnd->codegenEnded();
  }
  double t = seconds(Clock::now() - start);
  // Errors assemble no module, nor does a check; a session kept across them keeps the sizes of the modules the last
  // build without errors assembled (it assembles the others once the errors are gone).
  if (cg.modules.empty() && (cg.failed() || options.check) && build.codegen)
  {
    if (rebuilt && build.moduleBytes.size() < outputModules)
      build.moduleBytes.resize(outputModules, 0); // (an edit piece added)
    else if (build.moduleBytes.size() != outputModules)
      build.moduleBytes.assign(outputModules, 0);
  }
  else if (rebuilt && build.moduleBytes.size() < cg.modules.size())
    build.moduleBytes.resize(cg.modules.size(), 0); // (an edit piece added: the others keep their sizes)
  else if (build.moduleBytes.size() != cg.modules.size())
    build.moduleBytes.assign(cg.modules.size(), 0);
  size_t irBytes = cg.ir.size();
  for (size_t k = 0; k < cg.modules.size(); ++k)
  {
    if (cg.changedModules.empty() || cg.changedModules[k])
      build.moduleBytes[k] = cg.modules[k].size();
    irBytes += build.moduleBytes[k];
  }
  report("codegen: %.3f s, %zu functions, %zu structs, %zu bytes of IR in %zu module(s)\n", t, cg.functions, cg.structs, irBytes,
         std::max<size_t>(1, cg.modules.size()));
  if (options.keepGoing && options.stubMissing && !options.check)
    printf("stubs: %zu functions left out are stubs that abort when called (--stub-missing)\n", cg.stubs);
  // The checker's errors first, then codegen's: a failure of codegen that follows from one of them (one it
  // could not tell from its own) reads after its cause (2026-11-17, kovarex: "codegen does not support a member
  // access on a non-class object" came before "E has no member 'str'").
  size_t checkerErrors = reportCheckerErrors(global, true, cg.instanceAssertsChecked);
  // A failure in the test runner's own text (`<tests>.kpp`) beside the checker's errors follows from one of them: its
  // __kppEqual given a check's operands that no operator compares, which the checker reported at the check
  // (2026-11-21, kovarex: "the <tests>.kpp is weird").
  std::string runner = std::string(testRunnerPath) + ":";
  for (const std::string& e : cg.errors)
    if (!checkerErrors || e.compare(0, runner.size(), runner) != 0)
      fprintf(stderr, "%s\n", global.withAliasNames(e).c_str());
  if (!cg.errors.empty())
    codegenExit = 1;
  // A debugValue() the debugger cannot compute is the build's error, in every build and the language
  // server's check (docs/debugging.md, "debugValue").
  for (const std::string& error : debugValueErrors(global, symbols, cg.debugValueClasses))
  {
    fprintf(stderr, "%s\n", global.withAliasNames(error).c_str());
    codegenExit = 1;
  }
  reportAccessWarnings(build);
  reportReturnCopyWarnings(build);
  reportPassWarnings(options);
  if (!options.implicitUses.empty())
  {
    std::vector<ParsedFile*> files;
    for (const std::unique_ptr<ParsedFile>& p : build.parsed)
      files.push_back(p.get());
    std::string text;
    for (const std::string& line : global.implicitUses(files))
      text += line + "\n";
    if (options.implicitUses == "-")
      fputs(text.c_str(), stderr); // (`-`: with the warnings, as a snippet reads them)
    else if (FILE* out = fopen(options.implicitUses.c_str(), "wb"))
    {
      fwrite(text.data(), 1, text.size(), out);
      fclose(out);
    }
    else
      fprintf(stderr, "error: cannot write %s (--implicit-uses)\n", options.implicitUses.c_str());
  }
  if (options.check)
    return; // the errors are all it is for
  std::string clang = clangPath();
  report("toolchain: %s (%s, %s: links with %s)\n", clang.c_str(),
         bundledToolchain() ? "bundled" : getenv("KPP_CLANG") ? "KPP_CLANG" : "Visual Studio's", options.target.name.c_str(),
         !options.target.msvc() ? "lld" : bundledToolchain() || options.optimize ? "lld-link" : "link.exe");
  // --keep-going: the errors stay reported (and the exit code), but what was emitted is compiled and
  // linked; the functions errors left out are listed next to the executable for the link's report.
  bool stop = codegenExit && !options.keepGoing;
  if (options.keepGoing && !cg.failedFunctions.empty() && !options.exe.empty())
  {
    std::string list;
    for (const std::string& name : cg.failedFunctions)
      list += name + "\n";
    writeFile(options.exe + ".failed-functions.txt", list);
  }
  // --debug-info for Visual Studio: the .natvis that shows the library's values (a String's text, a
  // Vector's elements), linked into the PDB by linkFlags' /NATVIS (docs/debugging.md, "Values in
  // Visual Studio"), for the MSVC target and windows-gnu alike (both write a PDB). A rule a library
  // change broke is left out with a warning. It is made once the modules are with the back end (below), not
  // before: that was 0.08 to 0.12 s of the game's build on one thread while LLVM's threads waited for the modules.
  auto writeNatvis = [&]
  {
    if (!options.debugInfo || !options.target.windows() || options.exe.empty())
      return;
    std::vector<ParsedFile*> files;
    for (const std::unique_ptr<ParsedFile>& p : parsed)
      files.push_back(p.get());
    std::vector<std::string> problems;
    std::string natvis = makeNatvis(files, global, symbols, cg.debugClasses, problems);
    for (const std::string& problem : problems)
      fprintf(stderr, "warning: natvis: %s\n", problem.c_str());
    if (!writeFile(natvisPath(options), natvis))
      fprintf(stderr, "error: cannot write %s\n", natvisPath(options).c_str());
    // The program's own .natvis files, one path a line, for what cannot read them out of the PDB: Visual
    // Studio's kpp evaluator (docs/debugging.md, "this in the watch window").
    std::string list;
    for (const std::string& file : options.natvisFiles)
      list += std::filesystem::absolute(file).string() + "\n";
    if (!writeFile(natvisPath(options) + "-files", list))
      fprintf(stderr, "error: cannot write %s-files\n", natvisPath(options).c_str());
  };
  if (cg.modules.empty())
  {
    writeNatvis();
    std::string llPath = options.emitLLVM.empty() ? options.exe + ".ll" : options.emitLLVM;
    cg.ir = forTarget(cg.ir, options);
    if (!writeFile(llPath, cg.ir) || options.exe.empty() || stop)
      return;
    placeSanitizerRuntime(options);
    // --via-cpp: the program as one C++ translation unit (EXE.cpp), compiled and linked with the C++ runtime.
    std::string source = "\"" + llPath + "\"";
    if (options.viaCpp)
    {
      std::string cppPath = options.exe + ".cpp", cpp, error;
      if (!writeCppModule(cg.ir, CppWriterOptions{llPath, 0}, cpp, error) || !writeFile(cppPath, cpp))
      {
        fprintf(stderr, "%s: error: %s\n", llPath.c_str(), error.empty() ? ("cannot write " + cppPath).c_str() : error.c_str());
        codegenExit = 1;
        return;
      }
      source = cppCompileFlags() + (options.debugInfo ? cppDebugFlags(options) : std::string()) + " \"" + cppPath + "\" -x none" + cppLinkFlags(options);
    }
    std::string command = "\"" + clang + "\" " + level + targetFlags(options) + " -w " + compileFlags + " " + source + " -o \"" + options.exe + "\" " +
                          (fullLTO ? "-fuse-ld=lld " + ltoLinkFlags(options) + " " : std::string()) + linkFlags(options, wideEntry(parsed, symbols, options.target)) +
                          splitDebugInfoCommand(options);
    if (options.deferLink) // (one module: its compile goes with the link)
    {
      deferLinkCommand(options, command);
      report("clang:   deferred with the link\n");
      return;
    }
    start = Clock::now();
    int rc = host::shell(command);
    report("clang:   %.3f s (%s, exit %d)\n", seconds(Clock::now() - start), level, rc);
    if (rc != 0)
    {
      fprintf(stderr, "%s: error: clang failed (exit %d); its messages are above\n", llPath.c_str(), rc); // (whatever --quiet says)
      codegenExit = 1;
    }
    return;
  }
  if (stop)
  {
    writeNatvis();
    // What this build assembled is assembled again by the next, its objects compiled then (codegen's own
    // errors assembled nothing: the session has them; a build in full's modules are all new).
    if (build.codegen && (!rebuilt || std::count(cg.changedModules.begin(), cg.changedModules.end(), true)))
      unassembledAgain(*build.codegen, rebuilt ? cg.changedModules : std::vector<bool>{});
    return; // (backEnd drops what it has not started)
  }
  start = Clock::now();
  // The link's arguments, which go through a response file (64 objects' paths do not fit a command line):
  // the objects' lines, and the lines after them.
  struct LinkLines
  {
    std::string objects, flags;
  };
  auto linkLines = [&]
  {
    LinkLines lines;
    auto slashes = [](std::string path) // clang reads a backslash in a response file as an escape
    {
      for (char& c : path)
        if (c == '\\')
          c = '/';
      return path;
    };
    for (const std::string& o : objects)
    {
      std::error_code ec;
      if (options.keepGoing && !std::filesystem::exists(o, ec))
        continue; // a module clang rejected (--keep-going): the link reports what it would have defined
      lines.objects += "\"" + slashes(o) + "\"\n";
    }
    std::string& response = lines.flags;
    if (options.viaCpp)
      for (const std::string& flag : splitFlags(cppLinkFlags(options)))
        response += "\"" + slashes(flag) + "\"\n";
    response += "-o \"" + slashes(options.exe) + "\"\n";
    if (thinLTO)
    {
      // lld runs the ThinLTO back ends on the compiler's threads, and keeps their results for the
      // modules that did not change (the cache next to the executable).
      response += "-fuse-ld=lld\n-flto=thin\n" + optLevel + "\n";
      if (options.target.msvc())
      {
        response += "-Wl,/opt:lldltojobs=" + std::to_string(std::max(1u, options.threads)) + "\n";
        response += "\"-Wl,/lldltocache:" + slashes(options.exe) + ".ltocache\"\n";
      }
      else
      {
        // (ld64.lld names its cache -cache_path_lto)
        response += "-Wl,--thinlto-jobs=" + std::to_string(std::max(1u, options.threads)) + "\n";
        response += options.target.mac() ? "\"-Wl,-cache_path_lto," + slashes(options.exe) + ".ltocache\"\n"
                                         : "\"-Wl,--thinlto-cache-dir=" + slashes(options.exe) + ".ltocache\"\n";
      }
    }
    else if (fullLTO)
      response += "-fuse-ld=lld\n-flto\n" + optLevel + "\n";
    for (const std::string& flag : splitFlags(ltoLinkFlags(options)))
      response += flag + "\n";
    placeSanitizerRuntime(options);
    for (const std::string& flag : splitFlags(linkFlags(options, wideEntry(parsed, symbols, options.target))))
    {
      // A library under "C:\Program Files (x86)\..." (the Windows SDK's) stays one argument.
      std::string one = slashes(flag);
      response += (one.find(' ') != std::string::npos ? "\"" + one + "\"" : one) + "\n";
    }
    return lines;
  };
  // An MSVC link that is no LTO's runs link.exe directly (directLinkCommand). Its command is asked for now, on
  // a thread of its own beside the back end: the answer kept beside the executable is gone in a clean build, and
  // clang's driver took 0.07 to 0.09 s of the game's after the back end to give it again, with nothing else
  // running. (Not under --keep-going, whose link leaves out the object of a module clang rejects: asked then.)
  const bool directLink = options.target.msvc() && !thinLTO && !fullLTO && !bundledToolchain();
  struct LinkAsk
  {
    ~LinkAsk() { wait(); }
    void wait()
    {
      if (thread.joinable())
        thread.join();
    }
    std::thread thread;
    bool asked = false;
    LinkLines lines;
    std::string command; // (the thread's, read once it is joined)
  } linkAsk;
  if (directLink && !options.keepGoing)
  {
    linkAsk.lines = linkLines();
    linkAsk.asked = true;
    linkAsk.thread = std::thread([&linkAsk, clang, exe = options.exe] { linkAsk.command = directLinkCommand(clang, exe, linkAsk.lines.objects, linkAsk.lines.flags); });
  }
  // Output module k's object: the last build's when rebuildLLVM did not assemble it again or its hash
  // file has its text, else compiled (by LLVM's back end in this process, or by clang).
  // --verify-incremental: module k's object, its line tables patched, against its compile from the same .ll (whose name the
  // object holds), all but the COFF header's time stamp: one that differs is an error of the build (the server's check reports
  // the difference in what it printed), and the compiled object is taken.
  auto verifyPatched = [&](size_t k)
  {
    std::string llPath = options.exe + "." + std::to_string(k) + ".ll", patched, compiled, error;
    if (inProcess)
    {
      BackendOptions backend;
      backend.path = llPath;
      backend.module = unsigned(k);
      backend.functionSections = functionSections;
      if (!compileModule(cg.modules[k], backend, compiled, error))
        return;
    }
    else
    {
      std::string checkPath = objects[k] + ".verify.o";
      if (!writeFile(llPath, cg.modules[k]))
        return;
      std::string command = "\"" + clang + "\" " + level + targetFlags(options) + " -w " + compileFlags + " " + options.clangFlags + " -c \"" + llPath + "\" -o \"" +
                            checkPath + "\"";
      bool made = host::shell(command) == 0 && readFile(checkPath, compiled);
      std::error_code ec;
      std::filesystem::remove(checkPath, ec);
      if (!made)
        return;
    }
    if (!readFile(objects[k], patched))
      return;
    std::string written = compiled;
    if (patched.size() >= 8 && compiled.size() >= 8)
    {
      memset(patched.data() + 4, 0, 4);
      memset(compiled.data() + 4, 0, 4);
    }
    if (patched != compiled)
    {
      fprintf(stderr, "kpp: --verify-incremental: module %zu's object, its line tables patched, is not what its compile gives\n", k);
      writeFile(objects[k], written);
    }
  };
  auto compileEnd = [&](size_t k)
  {
    if (!cg.changedModules.empty() && !cg.changedModules[k])
    {
      ++cached; // not assembled again: its text and object are the last build's (checked before rebuildLLVM ran)
      return;
    }
    std::string hashText;
    if (!inProcess)
      cg.modules[k] = forTarget(cg.modules[k], options); // (before its hash: an object made without them is made again)
    if (unchangedObject(k, cg.modules[k], level, hashText))
      return;
    // A text that changed only in its lines (an edit moved them: debug lines, assert lines): the kept object's line tables and
    // assert lines patched, as the edit's line maps say (kovarex, 2026-11-10), its hash file written once it is.
    if (k < cg.linesOnlyModules.size() && cg.linesOnlyModules[k] && !build.lineMaps.empty() && options.target.windows() && !options.viaCpp)
    {
      std::string hashPath = options.exe + "." + std::to_string(k) + ".hash", why;
      std::error_code ec;
      std::filesystem::remove(hashPath, ec); // (patched halfway, the object is never taken for the old text's)
      if (patchLineTables(objects[k], build.lineMaps, why) && writeFile(hashPath, hashText))
      {
        if (options.verifyIncremental)
          verifyPatched(k);
        ++patchedObjects;
        ++cached;
        return;
      }
      logBuildNote("module " + std::to_string(k) + "'s line tables not patched (" + why + "): compiled");
    }
    if (inProcess)
    {
      compileInProcess(k, cg.modules[k], hashText);
      return;
    }
    std::string llPath = options.exe + "." + std::to_string(k) + ".ll";
    if (!writeFile(llPath, cg.modules[k]))
    {
      ++failures;
      return;
    }
    // clang's messages go to a file of the module's, printed with the failure below (32 clangs
    // writing to one stderr at once interleave).
    std::string errPath = options.exe + "." + std::to_string(k) + ".clang.txt";
    // --via-cpp: the module written as C++ beside its IR (EXE.k.cpp), and that compiled; what the writer does not
    // translate fails the module with its name, as a module clang rejects does.
    std::string source = llPath;
    if (options.viaCpp)
    {
      source = options.exe + "." + std::to_string(k) + ".cpp";
      std::string cpp, error;
      auto writing = Clock::now();
      bool written = writeCppModule(cg.modules[k], CppWriterOptions{llPath, unsigned(k)}, cpp, error);
      cppMicroseconds += uint64_t(seconds(Clock::now() - writing) * 1e6);
      cppBytes += cpp.size();
      if (!written || !writeFile(source, cpp))
      {
        moduleFailed(k, -1, llPath, error.empty() ? "cannot write " + source : error);
        return;
      }
    }
    std::string command = "\"" + clang + "\" " + level + targetFlags(options) + " -w " + compileFlags + " " + options.clangFlags + (options.viaCpp ? " " + cppCompileFlags() + (options.debugInfo ? cppDebugFlags(options) : std::string()) : std::string()) +
                          " -c \"" + source + "\" -o \"" + objects[k] + "\" 2> \"" + errPath + "\"";
    if (int rc = host::shell(command); rc != 0)
      moduleFailed(k, rc, source, readWholeFile(errPath));
    else
    {
      std::error_code removed;
      std::filesystem::remove(errPath, removed); // (-w: nothing in it)
      moduleCompiled(k, hashText);
    }
  };
  if (backEnd)
  {
    // Step 4's P2: the modules the stream did not compile, on its threads. A program piece it was
    // handed that the end assembled otherwise (CodegenResult::streamed) is compiled after the stream's
    // compile of it: both write its object.
    std::vector<size_t> again;
    for (size_t k = 0; k < cg.modules.size(); ++k)
      if (k < cg.streamed.size() && cg.streamed[k])
        continue;
      else if (handedOver[k])
        again.push_back(k);
      else
        backEnd->push([&, k] { compileEnd(k); }, false, cg.modules[k].size());
    writeNatvis(); // (while they compile)
    if (!again.empty())
    {
      backEnd->waitStreamed();
      {
        // (Its streamed compile's failure is not the build's: the end's text is compiled now.)
        std::lock_guard lock(moduleFailuresMutex);
        for (size_t k : again)
          for (auto f = moduleFailures.begin(); f != moduleFailures.end();)
            if (f->module == k)
            {
              f = moduleFailures.erase(f);
              --failures;
            }
            else
              ++f;
      }
      for (size_t k : again)
        backEnd->push([&, k] { compileEnd(k); }, false, cg.modules[k].size());
    }
    backEnd->wait();
  }
  else
  {
    writeNatvis();
    parallelFor(cg.modules.size(), options.threads, compileEnd);
  }
  double compileTime = seconds(Clock::now() - start);
  // --via-cpp: what the writing itself took, apart from the C++ compiler (both are in the clang line's time).
  if (options.viaCpp)
    report("cpp:     %.3f s of thread time writing %.1f MB of C++ (%u threads)\n", double(cppMicroseconds.load()) / 1e6, double(cppBytes.load()) / 1048576.0, options.threads);
  if (int patched = patchedObjects.load())
    logBuildNote(std::to_string(patched) + " object(s) of modules whose lines alone changed patched, not compiled");
  std::string during = backEnd ? ", " + std::to_string(backEnd->duringCodegen()) + " during codegen" : ""; // (step 4's P2)
  // A module clang or LLVM rejected is an error of the build whatever --quiet says, with what it said
  // (its first lines): with --keep-going the link goes on without its object and reports what it
  // would have defined, which names no cause.
  std::sort(moduleFailures.begin(), moduleFailures.end(), [](const ModuleFailure& a, const ModuleFailure& b) { return a.module < b.module; });
  for (const ModuleFailure& f : moduleFailures)
  {
    std::string said;
    size_t lines = 0;
    for (size_t at = 0; at < f.output.size() && lines < 6;)
    {
      size_t end = f.output.find('\n', at);
      if (end == std::string::npos)
        end = f.output.size();
      std::string line = f.output.substr(at, end - at);
      at = end + 1;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty() || line.starts_with(" #") || line.starts_with("Stack dump") || line.starts_with("PLEASE submit") || line.starts_with("Exception Code"))
        continue;
      said += "\n  " + line;
      ++lines;
    }
    std::string who = f.rc < 0 ? "module " + std::to_string(f.module) + " was not written as C++ (--via-cpp)"
                      : f.rc ? "clang rejected module " + std::to_string(f.module) + " (exit " + std::to_string(f.rc) + ")"
                           : "LLVM's back end rejected module " + std::to_string(f.module);
    fprintf(stderr, "%s: error: %s%s\n", f.path.c_str(), who.c_str(), said.empty() ? " and said nothing" : said.c_str());
  }
  if (failures)
  {
    report("clang:   %.3f s (%s, %zu modules, %d failed)\n", compileTime, level, cg.modules.size(), failures.load());
    codegenExit = 1;
    if (!options.keepGoing)
      return;
  }
  start = Clock::now();
  linkAsk.wait();
  LinkLines lines = linkLines();
  std::string responsePath = options.exe + ".link.rsp";
  if (!writeFile(responsePath, lines.objects + lines.flags))
    return;
  // (an MSVC link that is no LTO's: link.exe directly, directLinkCommand: asked above for these lines, or now)
  std::string linkCommand = !directLink ? std::string()
                            : linkAsk.asked && linkAsk.lines.objects == lines.objects && linkAsk.lines.flags == lines.flags
                              ? linkAsk.command
                              : directLinkCommand(clang, options.exe, lines.objects, lines.flags);
  if (linkCommand.empty())
    linkCommand = "\"" + clang + "\" @\"" + responsePath + "\"";
  std::string link = linkCommand;
  if (options.keepGoing) // the link's complaints (unresolved externals) go to a file next to the executable
    link = linkCommand + " > \"" + options.exe + ".link.txt\" 2>&1";
  link += splitDebugInfoCommand(options); // (an optimized ELF build's DWARF into NAME.debug)
  if (int shared = sharedHits.load())
    during = ", " + std::to_string(shared) + " of them from KPP_OBJECT_CACHE" + during;
  // One build in 16 trims the shared cache to the most recently used 768 MB once it is past 1 GB.
  if (!objectCacheDir.empty() && (uint64_t(host::processId()) * 2654435761u + uint64_t(Clock::now().time_since_epoch().count())) % 16 == 0)
    objectcache::trim(objectCacheDir, uintmax_t(1) << 30, uintmax_t(3) << 28);
  if (options.deferLink)
  {
    deferLinkCommand(options, link);
    report("clang:   %.3f s (%s, %zu modules on %u threads, %d unchanged%s) + link deferred\n", compileTime, level, cg.modules.size(),
           options.threads, cached.load(), during.c_str());
    return;
  }
  int rc = host::shell(link);
  report("clang:   %.3f s (%s, %zu modules on %u threads, %d unchanged%s) + link %.3f s (exit %d)\n", compileTime, level, cg.modules.size(),
         options.threads, cached.load(), during.c_str(), seconds(Clock::now() - start), rc);
  if (rc != 0)
  {
    // (the linker's words, sent to a file under --keep-going: where they are; a project build shows them)
    if (options.keepGoing)
      fprintf(stderr, "error: the link failed (exit %d); the linker's output is in %s.link.txt\n", rc, options.exe.c_str());
    codegenExit = 1;
  }
}
}
