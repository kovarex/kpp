// The driver: codegen's modules compiled (clang or the back end in kpp, the shared object cache)
// and linked: runCodegen; the declarations, in namespace driver (Driver.hpp includes this), the
// definitions in ModuleCompile.cpp (main.cpp split, stage 2, 2026-11-05, moved without a change).
  // The errors the checker found itself (a static_assert that fails or cannot be evaluated).
  // laidOut: codegen ran and checked the asserts on sizes and alignments (its errors), and the
  // static_asserts of the first instancesChecked class template instances.
  // (how many errors it printed)
  size_t reportCheckerErrors(GlobalScope& global, bool laidOut = false, size_t instancesChecked = 0);


  // --return-copy-warnings: each `return x;` of a class object that is copied or moved instead of
  // built in the result's place (GlobalScope::namedReturnVariable), with why; warnings, the exit
  // code stays.
  void reportReturnCopyWarnings(Build& build);


  // --access-warnings: the warnings to stderr (no error: the exit code stays), the counts with the
  // statistics.
  void reportAccessWarnings(Build& build);


  // The warnings the conversion's fix passes read (--zero-null-warnings, --this-warnings,
  // --this-reference-warnings, --plain-enum-warnings), to stderr, after codegen or where a check stops
  // before it (the checker's errors).
  void reportPassWarnings(const Options& options);


  // An MSVC link that is no LTO's runs link.exe directly, the clang driver's translation of the link's
  // response file kept beside the executable: the driver, which finds MSVC's and the Windows SDK's
  // libraries before it runs link.exe, took 141 to 519 ms by itself (`clang -###` on the game's 256
  // objects, a loaded machine, 2026-11-05), every edit's link. EXE.link.direct holds the key, then
  // link.exe's command, then its arguments with a line `<objects>` where the objects go; EXE.link.exe.rsp
  // those arguments with this build's objects.
  // The driver is asked without the objects (objects: the response's lines that name them; flags: its
  // lines after them), a linker option standing in their place, which it passes on where an object given
  // there goes: the game's 312 arguments are the same asked either way (2026-11-25). So the question
  // needs no object to exist yet (runCodegen puts it beside the back end's work), and the answer stays
  // when the objects change (a compile server's first edit of a file adds an object: each asked again).
  // The key: the flags, clang's path, size and time, and the environment the driver reads for the
  // toolchain (a VsDevCmd's variables, PATH). A key that changed asks the driver again (`clang -###`,
  // one run, its answer kept); "" when the answer is not one link.exe command this can read (an argument
  // with a quote or ending in a backslash), and the link goes through clang.
  std::string directLinkCommand(const std::string& clang, const std::string& exe, const std::string& objects, const std::string& flags);


  void runCodegen(Build& build);
