#pragma once
// The platform a build is for (--target; docs/design.md, "Targets and hosts"): what the code
// generator writes (the triple and data layout, the exception model, the runtime's calls) and how
// the driver compiles and links it. Windows x64 is the default and what kpp built before targets.
#include <string>
#include <string_view>

struct Target
{
  enum class Os
  {
    Windows,
    Linux,
    FreeBSD,
    Mac,
  };
  // The processor: config::arch (design.md, "Targets and hosts" 1).
  enum class Arch
  {
    X86_64,
    Arm64,
  };

  bool windows() const { return os == Os::Windows; }
  bool msvc() const { return os == Os::Windows && !gnu; } // Visual Studio's headers, libraries and link.exe
  bool posix() const { return os != Os::Windows; }
  bool mac() const { return os == Os::Mac; } // Mach-O, ld64.lld, the SDK (--sysroot or SDKROOT)
  bool arm64() const { return arch == Arch::Arm64; }
  // Whether the target's C reads a char, short or bool argument or result from its whole 32-bit
  // register, extended by the caller (an argument) or the callee (a result): SysV x86-64, as clang
  // reads it, and Apple's arm64, as Apple's ABI says (LLVM's `signext`/`zeroext`). Not MS x64, and not
  // Linux's AAPCS64, whose callee extends what it reads.
  bool extendsNarrowIntegers() const { return !windows() && (!arm64() || mac()); }
  const char* archName() const { return arch == Arch::Arm64 ? "arm64" : "x86_64"; } // sysroots' suffix, the SDK's .tbd name
  // ELF's data layout differs from COFF's only in the name mangling (`m:e`).
  static constexpr const char* elfX86_64Layout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";

  Os os = Os::Windows;
  Arch arch = Arch::X86_64;
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
// writes from this table too (main.cpp, configSource). Mac's --target names are macos and macos-x64.
struct PlatformName
{
  const char* enumerator;
  const char* name;
};
inline constexpr PlatformName platformNames[] = {{"Windows", "windows"}, {"Linux", "linux"}, {"Mac", "mac"}, {"FreeBSD", "freebsd"}};
inline const PlatformName& platformOf(Target::Os os)
{
  return platformNames[os == Target::Os::Linux ? 1 : os == Target::Os::Mac ? 2 : os == Target::Os::FreeBSD ? 3 : 0];
}
// config::arch's enumeration (Arch::X86_64, <config> declares it in this order).
inline constexpr const char* archEnumerators[] = {"X86_64", "Arm64"};

// --target's value: a short name or a triple. False, with why, for one kpp does not build for (yet).
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
  // macOS (stage 8): arm64 from macOS 11.0, x86-64 from 10.15 (design.md Q6), built against a macOS SDK
  // the user supplies (--sysroot or SDKROOT, checked by the driver: checkMacSdk). A triple names the
  // architecture; the oldest system is kpp's whatever version it says.
  bool apple = name.find("-apple-macos") != std::string_view::npos || name.find("-apple-darwin") != std::string_view::npos;
  if (name == "macos" || (apple && (name.starts_with("arm64-") || name.starts_with("aarch64-"))))
  {
    t.os = Target::Os::Mac;
    t.arch = Target::Arch::Arm64;
    t.name = "macos";
    t.triple = "arm64-apple-macosx11.0.0";
    t.dataLayout = "e-m:o-i64:64-i128:128-n32:64-S128-Fn32";
    out = t;
    return true;
  }
  if (name == "macos-x64" || (apple && name.starts_with("x86_64-")))
  {
    t.os = Target::Os::Mac;
    t.name = "macos-x64";
    t.triple = "x86_64-apple-macosx10.15.0";
    t.dataLayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";
    out = t;
    return true;
  }
  // Linux on AArch64 (stage 7): glibc 2.31's, as linux's (Debian 11 arm64: tools/sysroot/linux.ps1 -Arch arm64).
  if (name == "linux-arm64" || name == "aarch64-unknown-linux-gnu" || name == "aarch64-linux-gnu" || name == "arm64-unknown-linux-gnu")
  {
    t.os = Target::Os::Linux;
    t.arch = Target::Arch::Arm64;
    t.name = "linux-arm64";
    t.triple = "aarch64-unknown-linux-gnu";
    t.dataLayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32";
    out = t;
    return true;
  }
  why = "is no target kpp knows (windows, windows-gnu, linux, linux-arm64, freebsd, macos, macos-x64)";
  return false;
}

// The platform kpp runs on, --target's default (design.md, "Targets and hosts" 1): windows on
// Windows, linux on Linux, freebsd on FreeBSD (thread E's Linux host, 2026-11-06).
inline Target hostTarget()
{
  Target t;
  std::string why;
#if defined(__linux__) && defined(__aarch64__)
  targetNamed("linux-arm64", t, why);
#elif defined(__linux__)
  targetNamed("linux", t, why);
#elif defined(__FreeBSD__)
  targetNamed("freebsd", t, why);
#elif defined(__APPLE__) && defined(__aarch64__)
  targetNamed("macos", t, why);
#elif defined(__APPLE__)
  targetNamed("macos-x64", t, why);
#endif
  return t;
}
