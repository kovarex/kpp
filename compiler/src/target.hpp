#pragma once
// The platform a build is for (--target; docs/design.md, "Targets and hosts"): what the code
// generator writes (the triple and data layout, the exception model, the runtime's calls) and how
// the driver compiles and links it. Windows x64 is the default and what kppc built before targets.
#include <string>
#include <string_view>

struct Target
{
  enum class Os
  {
    Windows,
    Linux,
    FreeBSD,
  };

  bool windows() const { return os == Os::Windows; }
  bool msvc() const { return os == Os::Windows && !gnu; } // Visual Studio's headers, libraries and link.exe
  bool posix() const { return os != Os::Windows; }
  // ELF's data layout differs from COFF's only in the name mangling (`m:e`).
  static constexpr const char* elfX86_64Layout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";

  Os os = Os::Windows;
  std::string name = "windows"; // the short name (--target's, the reports')
  std::string triple = "x86_64-pc-windows-msvc";
  std::string dataLayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";
  // Windows in the GNU dialect (windows-gnu, design.md 17): the same ABI, data layout and SEH as
  // MSVC's, but MinGW-w64's headers and import libraries (the C runtime is the UCRT's ucrtbase.dll)
  // and lld's GNU driver: what the bundled toolchain (llvm-mingw) builds, with no Visual Studio.
  bool gnu = false;
};

// The platforms' names, one table (thread J's refactoring list, 2026-11-05): config::platform's
// enumerator (Platform::FreeBSD, the enumeration <config> declares in this order) and the short name
// --target, the sysroots (sysroots\freebsd-x86_64), build.kpp's systemLibrary(NAME, PLATFORM) and the
// reports use (freebsd). Project.kpp reads the short name through config::platformName, which <config>
// writes from this table too (main.cpp, configSource). Mac is no --target yet.
struct PlatformName
{
  const char* enumerator;
  const char* name;
};
inline constexpr PlatformName platformNames[] = {{"Windows", "windows"}, {"Linux", "linux"}, {"Mac", "mac"}, {"FreeBSD", "freebsd"}};
inline const PlatformName& platformOf(Target::Os os)
{
  return platformNames[os == Target::Os::Linux ? 1 : os == Target::Os::FreeBSD ? 3 : 0];
}

// --target's value: a short name or a triple. False, with why, for one kppc does not build for (yet).
inline bool targetNamed(std::string_view name, Target& out, std::string& why)
{
  Target t;
  if (name == "windows" || name == "x86_64-pc-windows-msvc")
  {
    out = t;
    return true;
  }
  if (name == "windows-gnu" || name == "x86_64-w64-windows-gnu" || name == "x86_64-w64-mingw32")
  {
    t.name = "windows-gnu";
    t.triple = "x86_64-w64-windows-gnu";
    t.gnu = true;
    out = t;
    return true;
  }
  if (name == "linux" || name == "x86_64-unknown-linux-gnu" || name == "x86_64-linux-gnu" || name == "x86_64-pc-linux-gnu")
  {
    t.os = Target::Os::Linux;
    t.name = "linux";
    t.triple = "x86_64-unknown-linux-gnu";
    t.dataLayout = Target::elfX86_64Layout;
    out = t;
    return true;
  }
  if (name == "freebsd" || name.starts_with("x86_64-unknown-freebsd"))
  {
    t.os = Target::Os::FreeBSD;
    t.name = "freebsd";
    // FreeBSD 14: kpp.net's host runs 14.3-RELEASE (design.md, Q4), whose base system is the
    // sysroot (tools/sysroot/freebsd.ps1).
    t.triple = name == "freebsd" ? std::string("x86_64-unknown-freebsd14") : std::string(name);
    t.dataLayout = Target::elfX86_64Layout;
    out = t;
    return true;
  }
  if (name == "linux-arm64" || name.starts_with("aarch64") || name.starts_with("arm64") || name == "macos" || name == "macos-x64" || name.find("apple") != std::string_view::npos)
    why = "comes with a later stage (docs/design.md, \"Targets and hosts\": AArch64 is stage 7, macOS stage 8)";
  else
    why = "is no target kppc knows (windows, windows-gnu, linux, freebsd)";
  return false;
}

// The platform kppc runs on, --target's default (design.md, "Targets and hosts" 1): windows on
// Windows, linux on Linux, freebsd on FreeBSD (thread E's Linux host, 2026-11-06).
inline Target hostTarget()
{
  Target t;
  std::string why;
#if defined(__linux__)
  targetNamed("linux", t, why);
#elif defined(__FreeBSD__)
  targetNamed("freebsd", t, why);
#endif
  return t;
}
