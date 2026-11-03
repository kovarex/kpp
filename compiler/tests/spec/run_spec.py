"""The feature specification suite of kppc.

Every compiler/tests/spec/<area>/<name>.kpp is one small program showing one construct; it is
compiled with build/kppc.exe, run, and its output compared with <name>.expected. The header of a
snippet says what it is about:

    // spec: default arguments
    // factorio: 32235 uses in 4539 files        (from the construct survey, optional)
    // pending: no generic lambdas yet           (a known gap: the snippet is expected to fail)
    // kppc: --release                           (kppc options this snippet is always built with: a
                                                  gap that only one configuration shows)
    // error: static assertion failed            (the program must not compile, and the compiler's
                                                  output must say this; one line per expected error)
    // warning: long is C's type                 (the program compiles, runs and prints its .expected, and the
                                                  compiler's output says this; one line per expected warning)
    // run: alpha "two words"                    (built and run in one step by `kppc --run snippet ARGS`)
    // test: --filter parse                      (the snippet's tests run by `kppc --test snippet -- ARGS`, from
                                                  the snippet's directory: the paths printed are its name)
    // exit: 3                                   (the program's exit code; 0 or 1 when not given)
    // target: windows (why)                     (the snippet is about that target alone: skipped for others)

Statuses: pass, FAIL (a regression), pending (a known gap, still failing), fixed (a pending
snippet that passes: remove its pending line). The run writes docs/spec-status.md, the
specification of what kppc can and cannot do yet, with the first compiler complaint of every gap.

A <name>.c next to a snippet is a C library of its own: compiled with clang and linked into the
snippet's executable (`--lib`), for what only a real C object shows (the variables it defines).
A <name>.files directory next to a snippet holds more kpp files of its program, compiled with it,
for what only several files show (a name private to its file); they are no snippets themselves.
A <name>.library.NAME directory is a library of the snippet's program, scoped by NAME (`--library-root
NAME DIR`, design.md 16), and a <name>.unscoped.NAME directory an unscoped one (`--unscoped-library-root`).

    python compiler\\tests\\spec\\run_spec.py [--filter TEXT] [--jobs N] [--no-doc] [--release] [--debug-info] [--shard I/N]

Each snippet is built in a directory of its own, build\\spec\\<area>.<name>\\, deleted as soon as the snippet
passes: what a run leaves is the failing and pending snippets' builds (and the FreeBSD bundle). A
snippet is built with --modules 4 unless its `kppc:` line says --modules: cross-module code and a
release build's ThinLTO are still exercised, in about 12 files of objects and IR where kppc's default
(4 modules per thread) made about 385. Every build of a whole run kept in the worktree, 380 to 600 files a
snippet, came to 2.6 million files in one worktree, and the kernel kept a record of each in its pool
(2026-11-03).

--out DIR and --snippets DIR replace build\\spec and compiler\\tests\\spec (tests\\specfiles.ps1 runs the
runner itself on a snippet that passes and one that fails).

--shard I/N runs the I-th of N parts (1 <= I <= N): every N-th snippet of the sorted list, starting at
the I-th, so each part gets a share of every area. CI runs the parts as parallel jobs (docs/build.md,
"Continuous integration"); a part writes no spec-status.md, and its summary line names it.

--release and --debug-info build every snippet with that kppc option too (the release and the
debug-info configurations, docs/test-matrix.md), into build\\spec-release or build\\spec-debug-info,
and write no spec-status.md, which is the debug build's. A snippet that prints its own configuration
has the release build's output in <name>.release.expected.

--target linux builds every snippet for Linux x86-64 (docs/design.md, "Targets and hosts") against a
glibc sysroot (KPP_SYSROOT_LINUX, else build\\sysroots\\linux-x86_64, which tests\\targets.ps1 makes from
WSL) and runs it under WSL, into build\\spec-linux, writing no spec-status.md. Skipped: the `run:` and
`test:` snippets (kppc runs those itself, on this machine) and those with another `target:`. Output
that differs on Linux goes in <name>.linux.expected.

--target windows-gnu builds every snippet for Windows in the GNU dialect (design.md 17: MinGW-w64's
headers and import libraries, the bundled toolchain's) with the llvm-mingw clang KPP_CLANG names, into
build\\spec-windows-gnu, and runs it here, `run:` and `test:` snippets too; a `target: windows` snippet
is one of its. Output that differs goes in <name>.windows-gnu.expected.

--target freebsd builds and links every snippet for FreeBSD x86-64 (design.md, "Targets and hosts",
stage 6) against FreeBSD 14.3's base system (KPP_SYSROOT_FREEBSD, else build\\sysroots\\freebsd-x86_64,
which tools\\sysroot\\freebsd.ps1 makes), into build\\spec-freebsd, writing no spec-status.md. Nothing
runs FreeBSD here: a snippet that links is "built", and build\\spec-freebsd is a bundle that runs on
FreeBSD (run.sh there compares each program's output with its expected output, which is
<name>.freebsd.expected, else <name>.linux.expected (LP64, a C library that is not the UCRT), else
<name>.expected). Skipped as for Linux.
"""
import argparse, os, re, shlex, shutil, subprocess, sys, concurrent.futures, time

# Streaming's barrier check in codegen (KPP_CHECK_BARRIER, docs/codegen-speed.md), on for every
# snippet built here, spec.cmd's or not, until program pieces rely on it.
os.environ.setdefault("KPP_CHECK_BARRIER", "1")

HERE = os.path.dirname(os.path.abspath(__file__))
COMPILER = os.path.dirname(os.path.dirname(HERE))
ROOT = os.path.dirname(COMPILER)
KPPC = os.path.join(COMPILER, "build", "kppc.exe")
# The game's SDL3 headers, for the interop snippets that import them (skipped when the Factorio
# checkout is not on this machine; the snippet then shows as failing to find the header). CI sets
# KPP_SDL_INCLUDE to upstream SDL 3.4.18's include directory (.github/workflows/ci.yml).
SDL_INCLUDE = os.environ.get("KPP_SDL_INCLUDE", r"C:\Factorio\libraries\SDL3\include")
EXTRA = ["--cinclude", SDL_INCLUDE] if os.path.isdir(SDL_INCLUDE) else []
# Headers written for the interop snippets themselves (C constructs the system headers lack).
EXTRA += ["--cinclude", os.path.join(HERE, "interop")]
OUT = os.path.join(COMPILER, "build", "spec")
SNIPPETS = HERE
DOC = os.path.join(ROOT, "docs", "spec-status.md")
# The clang kppc itself uses (main.cpp's clangPath), for the snippets' own C sources.
CLANG = os.environ.get("KPP_CLANG", r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang.exe")
# --target: the target the snippets are built for (None: this machine's, Windows), the clang flags
# for their C sources, and how a program runs (under WSL for linux).
TARGET = None
C_TARGET_FLAGS = []
SYSROOT_LINUX = os.environ.get("KPP_SYSROOT_LINUX", os.path.join(COMPILER, "build", "sysroots", "linux-x86_64"))
SYSROOT_FREEBSD = os.environ.get("KPP_SYSROOT_FREEBSD", os.path.join(COMPILER, "build", "sysroots", "freebsd-x86_64"))
# FreeBSD's known failures (freebsd-known-failures.txt): `build area/name  why` for a snippet that does
# not build for FreeBSD yet, `run area/name  why` for one that builds and does not run right. A known
# failure is reported as known, not as a failure; a known one that works fails the run (its line must
# go), and so does a line naming no snippet. The pending snippets are known failures of their own.
KNOWN_FREEBSD = os.path.join(HERE, "freebsd-known-failures.txt")
# run.sh, written into the FreeBSD bundle: on FreeBSD, `sh run.sh` runs every built snippet (60 s and
# 4 GB of address space each), compares its output, normalised as normalise() does, with its .expected, and ends with one
# summary line; its exit code says the same. A program's exit code above 1 fails, as here. known.txt
# beside it lists the programs expected to fail (the run lines and the pending snippets).
FREEBSD_RUN_SH = r'''#!/bin/sh
# The spec snippets built for FreeBSD by compiler\tests\spec\run_spec.py --target freebsd: each runs
# and its output is compared with its .expected (normalised: no trailing spaces, no blank lines at
# either end). A program known.txt names is expected to fail: its failure is "known", and its passing
# fails the run (freebsd-known-failures.txt's line, or the snippet's pending line, must go). The last
# line sums it up: `spec freebsd: ALL PASSED (N pass, K known)` or `spec freebsd: N FAILED (...)`.
cd "$(dirname "$0")" || exit 2
# (copied from Windows, as a CI artifact for one, the programs have lost their executable bit)
chmod +x ./*.elf
normalise() { awk '{ sub(/[ \t\r\v\f]+$/, ""); line[NR] = $0 } END { s = 1; while (s <= NR && line[s] == "") s++; e = NR; while (e >= s && line[e] == "") e--; for (i = s; i <= e; i++) print line[i] }'; }
pass=0; failed=0; known=0
isknown() { grep -qx "$1" known.txt 2>/dev/null; }
for exe in *.elf; do
  name=${exe%.elf}
  # (4 GB of address space each: a snippet that allocates until it fails, new_failure_throws_bad_alloc,
  # took the whole VM's memory and the VM with it in CI, run 41638108414)
  ( ulimit -v 4194304; exec timeout 60 "./$exe" ) > "$name.out" 2> "$name.err"
  code=$?
  normalise < "$name.out" > "$name.got"
  why=""
  if [ $code -gt 1 ]; then
    why="exit code $code: $(head -n 1 "$name.err")"
  elif ! cmp -s "$name.got" "$name.expected"; then
    why="output differs: $(diff "$name.expected" "$name.got" | sed -n 2p)"
  fi
  if [ -z "$why" ] && isknown "$name"; then
    echo "FIXED   $name  -- passes: remove its line from freebsd-known-failures.txt (or its pending line)"; failed=$((failed + 1))
  elif [ -z "$why" ]; then
    echo "ok      $name"; pass=$((pass + 1))
  elif isknown "$name"; then
    echo "known   $name  -- $why"; known=$((known + 1))
  else
    echo "FAIL    $name  -- $why"; failed=$((failed + 1))
  fi
done
if [ $failed -gt 0 ]; then echo "spec freebsd: $failed FAILED ($pass pass, $known known)"; exit 1; fi
echo "spec freebsd: ALL PASSED ($pass pass, $known known)"
'''


def run_program(exe, arguments=()):
    if TARGET == "linux":
        drive, rest = os.path.splitdrive(os.path.abspath(exe))
        return ["wsl.exe", "-e", "/mnt/" + drive[0].lower() + rest.replace("\\", "/"), *arguments]
    return [exe, *arguments]

AREA_TITLES = {
  "language": "Language: statements, expressions, declarations",
  "classes": "Classes and objects",
  "templates": "Templates",
  "library": "Standard library (what std:: converts to)",
  "kpp": "kpp's own features (design.md sections 7 and 8)",
  "interop": "C interop",
}


def header(path):
    info = {"spec": os.path.splitext(os.path.basename(path))[0], "factorio": "", "pending": None, "errors": [], "warnings": [], "run": None, "test": None,
            "exit": None, "kppc": "", "target": None}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"//\s*(spec|factorio|pending|error|warning|run|test|exit|kppc|target):\s*(.*)", line.strip())
            if not m:
                if line.strip() and not line.startswith("//"):
                    break
                continue
            if m.group(1) in ("error", "warning"):
                info[m.group(1) + "s"].append(m.group(2).strip())
            else:
                info[m.group(1)] = m.group(2).strip()
    return info


def normalise(text):
    return "\n".join(l.rstrip() for l in text.replace("\r\n", "\n").split("\n")).strip("\n")


def first_complaint(text):
    for line in text.replace("\r\n", "\n").split("\n"):
        s = line.strip()
        if s and not s.startswith(("files:", "bytes:", "lines:", "tokens:", "symbols:", "errors:", "threads:", "read:", "lex:", "parse:", "index:", "resolve:", "check:", "instantiate:", "codegen:", "clang:", "imports:")):
            return s
    return text.strip().split("\n")[0] if text.strip() else ""


def remove_tree(directory):
    """Deletes a snippet's build directory; a program that just exited may hold its file for a moment."""
    for attempt in range(5):
        shutil.rmtree(directory, ignore_errors=True)
        if not os.path.exists(directory):
            return
        time.sleep(0.2)


def run_one(path):
    """Builds and runs a snippet in its own directory, which goes once the snippet passes (or, for
    FreeBSD, once its program and expected output are in the bundle)."""
    area = os.path.basename(os.path.dirname(path))
    name = os.path.splitext(os.path.basename(path))[0]
    work = os.path.join(OUT, f"{area}.{name}")
    remove_tree(work)
    result = build_and_run(path, area, name, work)
    if result["status"] in ("pass", "built", "skip"):
        remove_tree(work)
    return result


def build_and_run(path, area, name, work):
    info = header(path)
    exe = os.path.join(work, f"{area}.{name}.exe")
    # (a snippet's own --modules wins: the ones about splitting a program ask for 64 or 128)
    modules = [] if "--modules" in shlex.split(info["kppc"]) else ["--modules", "4"]
    expected_path = os.path.splitext(path)[0] + ".expected"
    # A snippet that prints its own configuration has the release build's output next to the debug one's.
    if "--release" in EXTRA and os.path.exists(os.path.splitext(path)[0] + ".release.expected"):
        expected_path = os.path.splitext(path)[0] + ".release.expected"
    for variant in ([TARGET, "linux"] if TARGET == "freebsd" else [TARGET] if TARGET else []):
        if os.path.exists(os.path.splitext(path)[0] + f".{variant}.expected"):
            expected_path = os.path.splitext(path)[0] + f".{variant}.expected"
            break
    result = {"area": area, "name": name, "info": info, "note": ""}
    target = info["target"].split()[0] if info["target"] else None # (`// target: windows (why)`)
    # (windows-gnu is Windows: a snippet about Windows is about it too, and its programs run here)
    on_target = (target or TARGET) == TARGET or (TARGET == "windows-gnu" and target == "windows")
    if TARGET and (not on_target or (TARGET != "windows-gnu" and (info["run"] is not None or info["test"] is not None))):
        result["status"] = "skip"; return result
    if not TARGET and target not in (None, "windows"):
        result["status"] = "skip"; return result
    os.makedirs(work, exist_ok=True)
    libs = []
    c_source = os.path.splitext(path)[0] + ".c"
    if os.path.exists(c_source):
        obj = os.path.join(work, f"{area}.{name}.c.obj")
        cc = subprocess.run([CLANG, *C_TARGET_FLAGS, "-c", "-O1", "-o", obj, c_source], capture_output=True, text=True, encoding="utf-8", errors="replace")
        if cc.returncode != 0:
            result["status"] = "FAIL"; result["note"] = f"its C source did not compile: {first_complaint(cc.stderr)}"; return finish(result)
        libs = ["--lib", obj]
    if info["run"] is not None or info["test"] is not None:
        return finish(run_command(path, info, modules + libs, expected_path, result))
    companions = os.path.splitext(path)[0] + ".files"
    sources = [path, companions] if os.path.isdir(companions) else [path]
    sources += library_roots(path)
    try:
        c = subprocess.run([KPPC, "--quiet", *EXTRA, *shlex.split(info["kppc"]), *modules, *libs, "--exe", exe, *sources], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
    except subprocess.TimeoutExpired:
        result["status"] = "FAIL"; result["note"] = "compiler timed out"; return finish(result)
    if info["errors"]:
        # A snippet that must not compile: every expected error is in the compiler's output.
        output = c.stdout + c.stderr
        missing = [e for e in info["errors"] if e not in output]
        if c.returncode == 0:
            result["status"] = "FAIL"; result["note"] = f"compiled, but the error `{info['errors'][0]}` was expected"
        elif c.returncode != 1:
            result["status"] = "FAIL"; result["note"] = f"the compiler crashed (exit 0x{c.returncode & 0xFFFFFFFF:X})"
        elif missing:
            result["status"] = "FAIL"; result["note"] = f"no error `{missing[0]}`; the first complaint: " + (first_complaint(output) or "none")
        else:
            result["status"] = "pass"
        return finish(result)
    if c.returncode != 0 or not os.path.exists(exe):
        result["status"] = "FAIL"
        if c.returncode not in (0, 1):
            result["note"] = f"the compiler crashed (exit 0x{c.returncode & 0xFFFFFFFF:X})"
        else:
            result["note"] = first_complaint(c.stdout + c.stderr) or "compilation failed"
        return finish(result)
    # A build that reported an error is no pass, whatever its exit code says.
    reported = next((l.strip() for l in (c.stdout + c.stderr).splitlines() if ": error:" in l), None)
    if reported:
        result["status"] = "FAIL"; result["note"] = "the build exited 0 but reported " + reported[reported.find(": error:") + 2:]
        return finish(result)
    missing = [w for w in info["warnings"] if w not in c.stdout + c.stderr]
    if missing:
        result["status"] = "FAIL"; result["note"] = f"no warning `{missing[0]}`"
        return finish(result)
    if TARGET == "freebsd":
        # Into the bundle, beside run.sh: the program as <area>.<name>.elf, and its expected output,
        # normalised (the rest of its build goes with its directory).
        bundled = os.path.join(OUT, f"{area}.{name}")
        os.replace(exe, bundled + ".elf")
        expected = open(expected_path, encoding="utf-8").read() if os.path.exists(expected_path) else ""
        with open(bundled + ".expected", "w", encoding="utf-8", newline="\n") as f:
            f.write(normalise(expected) + "\n" if normalise(expected) else "")
        result["status"] = "built"
        return result
    try:
        r = subprocess.run(run_program(exe), capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
    except subprocess.TimeoutExpired:
        result["status"] = "FAIL"; result["note"] = "program timed out"; return finish(result)
    if r.returncode != 0 and r.returncode > 1:
        result["status"] = "FAIL"; result["note"] = f"program exited with 0x{r.returncode & 0xFFFFFFFF:X}"
        return finish(result)
    compare(r.stdout, expected_path, result)
    return finish(result)


def library_roots(path):
    """The kppc options of a snippet's library directories, <name>.library.NAME and <name>.unscoped.NAME."""
    folder, stem = os.path.split(os.path.splitext(path)[0])
    options = []
    for entry in sorted(os.listdir(folder)):
        for kind, flag in ((".library.", "--library-root"), (".unscoped.", "--unscoped-library-root")):
            if entry.startswith(stem + kind) and os.path.isdir(os.path.join(folder, entry)):
                options += [flag, entry[len(stem) + len(kind):], os.path.join(folder, entry)]
    return options


def is_companion(directory):
    """Whether a directory is (in) a snippet's companion: its .files, or one of its libraries."""
    return any(re.search(r"\.(files|library\.\w+|unscoped\.\w+)$", part) for part in os.path.relpath(directory, SNIPPETS).split(os.sep))


def run_command(path, info, libs, expected_path, result):
    """A `// run:` snippet: `kppc --run` builds and runs it in one step; its exit code is the program's.
    A `// test:` snippet: `kppc --test` builds its tests and runs them, from the snippet's directory."""
    extra = EXTRA + shlex.split(info["kppc"])
    if info["test"] is not None:
        command, cwd = [KPPC, *extra, *libs, "--no-server", "--test", os.path.basename(path), "--", *shlex.split(info["test"])], os.path.dirname(path)
    else:
        command, cwd = [KPPC, *extra, *libs, "--no-server", "--run", path, *shlex.split(info["run"])], None
    try:
        r = subprocess.run(command, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=360)
    except subprocess.TimeoutExpired:
        result["status"] = "FAIL"; result["note"] = f"kppc {command[len(extra) + len(libs) + 2]} timed out"; return result
    want = int(info["exit"]) if info["exit"] is not None else None
    if want is not None and r.returncode != want or want is None and r.returncode not in (0, 1):
        result["status"] = "FAIL"
        result["note"] = f"exit code {r.returncode & 0xFFFFFFFF}" + (f", expected {want}" if want is not None else "")
        complaint = first_complaint(r.stderr)
        if complaint:
            result["note"] += f": {complaint}"
        return result
    compare(r.stdout, expected_path, result)
    return result


def compare(stdout, expected_path, result):
    expected = open(expected_path, encoding="utf-8").read() if os.path.exists(expected_path) else ""
    if normalise(stdout) == normalise(expected):
        result["status"] = "pass"
    else:
        result["status"] = "FAIL"
        got, want = normalise(stdout).split("\n"), normalise(expected).split("\n")
        for i in range(max(len(got), len(want))):
            g = got[i] if i < len(got) else "<end>"
            w = want[i] if i < len(want) else "<end>"
            if g != w:
                result["note"] = f"line {i + 1}: got `{g}`, expected `{w}`"
                break


def finish(result):
    pending = result["info"]["pending"]
    if result["status"] == "FAIL" and pending is not None:
        result["status"] = "pending"
    elif result["status"] == "pass" and pending is not None:
        result["status"] = "fixed"
    return result


def write_doc(results, elapsed):
    counts = {s: sum(1 for r in results if r["status"] == s) for s in ("pass", "pending", "FAIL", "fixed")}
    lines = ["# What kppc compiles: the specification suite", "",
             "Generated by `compiler\\tests\\spec.cmd` from `compiler/tests/spec/<area>/*.kpp`: one small program per",
             "construct, compiled, run and compared with its `.expected` output. The Factorio column is the",
             "construct's count in the game's sources (`src`, `CommonUtil`, `Agui`; 7,371 files), so the gaps are",
             "ordered by what the conversion needs. A pending row is a known gap and shows the compiler's first",
             "complaint; it turns into a failure of the suite the day it starts passing, so the row gets removed.", "",
             f"**{counts['pass']} pass, {counts['pending']} pending, {counts['FAIL']} failing, {counts['fixed']} fixed** "
             f"({len(results)} snippets, {elapsed:.1f} s).", ""]
    for area in sorted({r["area"] for r in results}, key=lambda a: list(AREA_TITLES).index(a) if a in AREA_TITLES else 99):
        rows = sorted((r for r in results if r["area"] == area), key=lambda r: (r["status"] == "pass", r["name"]))
        lines += [f"## {AREA_TITLES.get(area, area)}", "", "| Snippet | Construct | Status | Factorio | Note |", "|---|---|---|---|---|"]
        for r in rows:
            note = r["info"]["pending"] or ""
            if r["status"] in ("pending", "FAIL") and r["note"]:
                note = (note + "; " if note else "") + r["note"].replace("|", "\\|")
            if r["status"] == "fixed":
                note = "passes now: remove the pending line"
            lines.append(f"| `{area}/{r['name']}.kpp` | {r['info']['spec']} | {r['status']} | {r['info']['factorio']} | {note} |")
        lines.append("")
    os.makedirs(os.path.dirname(DOC), exist_ok=True)
    with open(DOC, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def apply_known_freebsd(results, whole):
    """FreeBSD's known failures: a known build failure becomes "known", a known one that builds
    "fixed"; known.txt in the bundle lists the run lines and the pending snippets that built. Returns
    the lines naming no snippet (checked on a whole run only)."""
    known = {}
    if os.path.exists(KNOWN_FREEBSD):
        for line in open(KNOWN_FREEBSD, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if line:
                kind, name, *why = line.split(None, 2)
                known[name] = (kind, why[0] if why else "")
    seen = set()
    runs = []
    for r in results:
        key = f"{r['area']}/{r['name']}"
        seen.add(key)
        kind, why = known.get(key, ("", ""))
        if kind == "build" and r["status"] == "FAIL":
            r["status"] = "known"; r["note"] = why + (f"; {r['note']}" if r["note"] else "")
        elif kind == "build" and r["status"] in ("built", "pass"):
            r["status"] = "fixed"; r["note"] = "builds now: remove its build line from freebsd-known-failures.txt"
        if r["status"] == "built" and (kind == "run" or r["info"]["pending"] is not None):
            runs.append(f"{r['area']}.{r['name']}")
    with open(os.path.join(OUT, "known.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(name + "\n" for name in sorted(runs)))
    return sorted(name for name in known if name not in seen) if whole else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--no-doc", action="store_true")
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--debug-info", action="store_true")
    ap.add_argument("--target", choices=["linux", "freebsd", "windows-gnu"])
    ap.add_argument("--shard", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--snippets", default="")
    args = ap.parse_args()
    global OUT, TARGET, C_TARGET_FLAGS, SNIPPETS
    if args.out:
        OUT = os.path.abspath(args.out)
    if args.snippets:
        SNIPPETS = os.path.abspath(args.snippets)
        args.no_doc = True
    shard = re.fullmatch(r"(\d+)/(\d+)", args.shard) if args.shard else None
    if args.shard and not (shard and 1 <= int(shard.group(1)) <= int(shard.group(2))):
        print("--shard takes I/N with 1 <= I <= N, not", args.shard)
        print("spec: FAILED: bad --shard"); return 2
    if shard:
        args.no_doc = True
    for flag in ("release", "debug_info"):
        if getattr(args, flag):
            option = "--" + flag.replace("_", "-")
            EXTRA.append(option)
            OUT += option[1:]
            args.no_doc = True
    if args.target == "windows-gnu":
        # Windows in the GNU dialect (design.md 17): the bundled toolchain's llvm-mingw, named by KPP_CLANG.
        TARGET = args.target
        if "KPP_CLANG" not in os.environ:
            print("--target windows-gnu: KPP_CLANG names no llvm-mingw clang")
            print("spec: FAILED: no llvm-mingw"); return 2
        EXTRA.extend(["--target", "windows-gnu"])
        C_TARGET_FLAGS = ["--target=x86_64-w64-windows-gnu"]
        OUT += "-" + TARGET
        args.no_doc = True
    elif args.target == "freebsd":
        TARGET = args.target
        if not os.path.exists(os.path.join(SYSROOT_FREEBSD, "usr", "lib", "libc.a")):
            print("no FreeBSD sysroot at", SYSROOT_FREEBSD, "(tools\\sysroot\\freebsd.ps1 makes one)")
            print("spec: FAILED: no FreeBSD sysroot"); return 2
        EXTRA.extend(["--target", "freebsd", "--sysroot", SYSROOT_FREEBSD])
        C_TARGET_FLAGS = ["--target=x86_64-unknown-freebsd14", "--sysroot=" + SYSROOT_FREEBSD]
        OUT += "-" + TARGET
        args.no_doc = True
    elif args.target:
        TARGET = args.target
        if not os.path.exists(os.path.join(SYSROOT_LINUX, "usr", "lib", "x86_64-linux-gnu", "libc.so.6")):
            print("no Linux sysroot at", SYSROOT_LINUX, "(tests\\targets.ps1 makes one from WSL)")
            print("spec: FAILED: no Linux sysroot"); return 2
        # (WSL started once first: its virtual machine, stopped when idle, took more than a program's
        # 60 s to start under a pool of programs at once)
        try:
            subprocess.run(["wsl.exe", "-e", "true"], capture_output=True, timeout=300)
        except subprocess.TimeoutExpired:
            print("spec: FAILED: WSL did not answer in 300 s"); return 2
        EXTRA.extend(["--target", "linux", "--sysroot", SYSROOT_LINUX])
        C_TARGET_FLAGS = ["--target=x86_64-unknown-linux-gnu", "--sysroot=" + SYSROOT_LINUX]
        OUT += "-" + TARGET
        args.no_doc = True
    if not os.path.exists(KPPC):
        print("no compiler at", KPPC)
        print("spec: FAILED: no compiler"); return 2
    os.makedirs(OUT, exist_ok=True)
    # Files directly in OUT are an earlier run's: the FreeBSD bundle, or every build's objects from
    # before the builds had a directory each. A whole run also clears the directories it leaves (a
    # snippet since removed or renamed); one with --filter or --shard clears its own snippets' only.
    whole = not args.filter and not shard
    for entry in os.listdir(OUT):
        if os.path.isfile(os.path.join(OUT, entry)):
            os.remove(os.path.join(OUT, entry))
        elif whole:
            remove_tree(os.path.join(OUT, entry))
    if TARGET == "freebsd":
        with open(os.path.join(OUT, "run.sh"), "w", encoding="utf-8", newline="\n") as f:
            f.write(FREEBSD_RUN_SH)
    files = sorted(os.path.join(dp, f) for dp, dn, fn in os.walk(SNIPPETS) for f in fn
                   if f.endswith(".kpp") and not is_companion(dp) and args.filter in os.path.relpath(os.path.join(dp, f), SNIPPETS).replace("\\", "/"))
    if shard:
        files = files[int(shard.group(1)) - 1::int(shard.group(2))]
    start = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(run_one, files))
    elapsed = time.time() - start
    skipped = sum(1 for r in results if r["status"] == "skip")
    results = [r for r in results if r["status"] != "skip"]
    stale = []
    if TARGET == "freebsd":
        stale = apply_known_freebsd(results, not args.filter)
    for r in results:
        tag = {"pass": "ok     ", "built": "built  ", "known": "known  ", "pending": "pending", "FAIL": "FAIL   ", "fixed": "FIXED  "}[r["status"]]
        line = f"{tag} {r['area']}/{r['name']}"
        if r["status"] in ("FAIL", "pending", "known", "fixed") and r["note"]:
            line += f"  -- {r['note']}"
        print(line)
    counts = {s: sum(1 for r in results if r["status"] == s) for s in ("pass", "built", "known", "pending", "FAIL", "fixed")}
    for entry in stale:
        print(f"FAIL    {entry}  -- freebsd-known-failures.txt names no such snippet")
    print((f"{counts['built']} built (run.sh runs them on FreeBSD: {os.path.relpath(OUT, ROOT)}), {counts['known']} known not to build, " if TARGET == "freebsd" else "") + f"{counts['pass']} pass, {counts['pending']} pending, {counts['FAIL']} failing, {counts['fixed']} fixed" +
          (f", {skipped} skipped for {TARGET or 'windows'}" if skipped else "") + f" ({elapsed:.1f} s)")
    if not args.no_doc and not args.filter:
        write_doc(results, elapsed)
        print("wrote", os.path.relpath(DOC, ROOT))
    # The one line that says whether everything passed (a pending snippet passing is a failure: its
    # mark must go), and the exit code says the same:
    #   spec: ALL PASSED (898 pass, 2 pending)
    #   spec: 2 FAILED: types/enum_size, fixed lambdas/capture (897 pass, 1 pending, 1 failing, 1 fixed)
    summary = (f"{counts['built']} built, {counts['known']} known not to build, " if TARGET == "freebsd" else "") + f"{counts['pass']} pass, {counts['pending']} pending" + (f", {skipped} skipped for {TARGET or 'windows'}" if skipped else "") + \
              (f"; shard {args.shard}" if shard else "")
    bad = [f"{r['area']}/{r['name']}" for r in results if r["status"] == "FAIL"] + \
          [f"fixed {r['area']}/{r['name']}" for r in results if r["status"] == "fixed"] + \
          [f"stale {entry}" for entry in stale]
    if bad:
        shown = ", ".join(bad[:10]) + (f", and {len(bad) - 10} more" if len(bad) > 10 else "")
        print(f"spec: {len(bad)} FAILED: {shown} ({summary}, {counts['FAIL']} failing, {counts['fixed']} fixed)")
        return 1
    print(f"spec: ALL PASSED ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
