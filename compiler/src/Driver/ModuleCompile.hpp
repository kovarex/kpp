// The driver: codegen's modules compiled (clang or the back end in kppc, the shared object cache)
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


  // An MSVC link that is no LTO's runs link.exe directly, the clang driver's translation of the link's
  // response file kept beside the executable: the driver, which finds MSVC's and the Windows SDK's
  // libraries before it runs link.exe, took 141 to 519 ms by itself (`clang -###` on the game's 256
  // objects, a loaded machine, 2026-11-05), every edit's link. EXE.link.direct holds the key, then
  // link.exe's command; EXE.link.exe.rsp its arguments. The key: the response file's text, clang's
  // path, size and time, and the environment the driver reads for the toolchain (a VsDevCmd's
  // variables, PATH). A key that changed asks the driver again (`clang -###`, one run, its command
  // kept); "" when its answer is not one link.exe command this can read (an argument with a quote or
  // ending in a backslash), and the link goes through clang.
  std::string directLinkCommand(const std::string& clang, const std::string& responsePath, const std::string& response, const std::string& exe);


  void runCodegen(Build& build);
