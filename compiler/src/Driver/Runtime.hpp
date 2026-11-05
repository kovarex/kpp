// The driver: what a program links besides its objects: the C runtime's libraries, windows-gnu's
// and the Itanium model's runtimes; the declarations, in namespace driver (Driver.hpp includes
// this), the definitions in Runtime.cpp (main.cpp split, stage 2, 2026-11-05, moved without a
// change).
  // What the executable links against: legacy_stdio_definitions (the printf family the UCRT
  // headers define inline), the Windows libraries programs commonly reach, --lib files; unused
  // functions go (/OPT:REF).
  // wideEntry: the program defines wmain and no main, so the link names wmainCRTStartup itself. Left to
  // infer the entry, lld-link takes a main that any library has when the program's objects are
  // bitcode (a ThinLTO release build): the game's opus.lib carries opus_compare.obj, and the release
  // game ran opus_compare's main, "found both wmain and main; using latter" (docs/path-to-factorio.md,
  // "The release build").
  // A debug build's link folds no identical functions (/OPT:NOICF): folding took 1.5 s of the game's
  // 4.5 s link, and folded functions share an address, so a crash report or a debugger names one of
  // them for the other. --incremental-link keeps every function instead (/OPT:NOREF, which
  // /INCREMENTAL needs) and relinks only the objects that changed (docs/compile-speed.md, "The link in
  // the edit loop").
  // What makes clang compile for the target: nothing for Windows' MSVC dialect (Visual Studio's
  // clang's default), the triple otherwise (a module's own triple does not decide clang's: clang
  // overrides it with its default).
  std::string targetFlags(const Options& options);


  std::string madeOnce(const std::string& stem, const std::string& ext, const std::string& text, const Options& options,
                       const std::function<std::string(const std::string&, const std::string&)>& command);


  // What a program in Windows' GNU dialect (windows-gnu) links besides MinGW-w64's libraries, made
  // once by the tools next to clang into the import cache (%TEMP%\kpp-imports), under a name with the
  // hash of what they are made from (another compiler making one at once makes the same):
  // - an import library of ucrtbase.dll's functions that MinGW-w64's leave out: the UCRT's C++
  //   runtime's (its own is libc++'s) and its long double ones, which are doubles as MSVC's are;
  // - those of math.h's long double functions on doubles: kpp's long double is a double, as MSVC's
  //   (docs/build.md, "The toolchain"), and libmingwex's are x87's 80 bits.
  // The files, quoted for the link; empty, with an error printed, when one cannot be made.
  std::string gnuRuntime(const Options& options);


  // A file a program links besides its own objects, made once from text by command(input, output)
  // into the import cache (%TEMP%\kpp-imports), under `stem-HASH.ext`: the hash of the text, the
  // tools' path and the flags (another compiler making one at once makes the same). The text is
  // written to `stem-PID.in` first. The file's path, or "" with an error printed.
  std::string madeOnce(const std::string& stem, const std::string& ext, const std::string& text, const Options& options,
                       const std::function<std::string(const std::string&, const std::string&)>& command);


  // What a program for an Itanium target (every one but Windows) links besides its own objects:
  // kpp's personality, __kpp_personality_v0, which the unwinder calls for each frame of a function
  // with landing pads (docs/design.md, "Targets and hosts", stage 3), in C, made once per target.
  // - Phase 1, the search, matches the thrown descriptor against each catch clause's (the LSDA's type
  //   table) with kpp_eh_offset. A noexcept function's empty exception specification ends the
  //   program there, through kpp_eh_terminate, with the throw site still on the stack (kovarex,
  //   2026-11-03).
  // - Phase 2 installs the landing pads: the handler's with the clause's index, the others' for their
  //   cleanups.
  // Only kpp's exceptions ("KPP\0KPP\0") are caught; another language's pass, through the cleanups.
  // kpp_eh_offset and kpp_eh_terminate are the runtime's that codegen writes (finishExceptionRuntime),
  // weak here: a program without exceptions has neither, and its link drops this (--gc-sections).
  std::string itaniumRuntime(const Options& options);
