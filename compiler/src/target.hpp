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
  Os os = Os::Windows;
  std::string name = "windows"; // the short name (--target's, the reports')
  std::string triple = "x86_64-pc-windows-msvc";
  std::string dataLayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";

  bool windows() const { return os == Os::Windows; }
  bool posix() const { return os != Os::Windows; }
  // ELF's data layout differs from COFF's only in the name mangling (`m:e`).
  static constexpr const char* elfX86_64Layout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";
};

// --target's value: a short name or a triple. False, with why, for one kppc does not build for (yet).
inline bool targetNamed(std::string_view name, Target& out, std::string& why)
{
  Target t;
  if (name == "windows" || name == "x86_64-pc-windows-msvc")
  {
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
    // FreeBSD 14 until the version kpp.net's host runs is known (design.md, Q4).
    t.triple = name == "freebsd" ? std::string("x86_64-unknown-freebsd14") : std::string(name);
    t.dataLayout = Target::elfX86_64Layout;
    out = t;
    return true;
  }
  if (name == "linux-arm64" || name.starts_with("aarch64") || name.starts_with("arm64") || name == "macos" || name == "macos-x64" || name.find("apple") != std::string_view::npos)
    why = "comes with a later stage (docs/design.md, \"Targets and hosts\": AArch64 is stage 7, macOS stage 8)";
  else
    why = "is no target kppc knows (windows, linux, freebsd)";
  return false;
}
