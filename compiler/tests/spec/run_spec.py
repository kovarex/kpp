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
"""
import argparse, os, re, shlex, subprocess, sys, concurrent.futures, time

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
DOC = os.path.join(ROOT, "docs", "spec-status.md")
# The clang kppc itself uses (main.cpp's clangPath), for the snippets' own C sources.
CLANG = os.environ.get("KPP_CLANG", r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang.exe")
# --target: the target the snippets are built for (None: this machine's, Windows), the clang flags
# for their C sources, and how a program runs (under WSL for linux).
TARGET = None
C_TARGET_FLAGS = []
SYSROOT_LINUX = os.environ.get("KPP_SYSROOT_LINUX", os.path.join(COMPILER, "build", "sysroots", "linux-x86_64"))


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


def run_one(path):
    area = os.path.basename(os.path.dirname(path))
    name = os.path.splitext(os.path.basename(path))[0]
    info = header(path)
    exe = os.path.join(OUT, f"{area}.{name}.exe")
    expected_path = os.path.splitext(path)[0] + ".expected"
    # A snippet that prints its own configuration has the release build's output next to the debug one's.
    if "--release" in EXTRA and os.path.exists(os.path.splitext(path)[0] + ".release.expected"):
        expected_path = os.path.splitext(path)[0] + ".release.expected"
    if TARGET and os.path.exists(os.path.splitext(path)[0] + f".{TARGET}.expected"):
        expected_path = os.path.splitext(path)[0] + f".{TARGET}.expected"
    result = {"area": area, "name": name, "info": info, "note": ""}
    target = info["target"].split()[0] if info["target"] else None # (`// target: windows (why)`)
    # (windows-gnu is Windows: a snippet about Windows is about it too, and its programs run here)
    on_target = (target or TARGET) == TARGET or (TARGET == "windows-gnu" and target == "windows")
    if TARGET and (not on_target or (TARGET != "windows-gnu" and (info["run"] is not None or info["test"] is not None))):
        result["status"] = "skip"; return result
    if not TARGET and target not in (None, "windows"):
        result["status"] = "skip"; return result
    libs = []
    c_source = os.path.splitext(path)[0] + ".c"
    if os.path.exists(c_source):
        obj = os.path.join(OUT, f"{area}.{name}.c.obj")
        cc = subprocess.run([CLANG, *C_TARGET_FLAGS, "-c", "-O1", "-o", obj, c_source], capture_output=True, text=True, encoding="utf-8", errors="replace")
        if cc.returncode != 0:
            result["status"] = "FAIL"; result["note"] = f"its C source did not compile: {first_complaint(cc.stderr)}"; return finish(result)
        libs = ["--lib", obj]
    if info["run"] is not None or info["test"] is not None:
        return finish(run_command(path, info, libs, expected_path, result))
    companions = os.path.splitext(path)[0] + ".files"
    sources = [path, companions] if os.path.isdir(companions) else [path]
    sources += library_roots(path)
    try:
        c = subprocess.run([KPPC, "--quiet", *EXTRA, *shlex.split(info["kppc"]), *libs, "--exe", exe, *sources], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
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
    return any(re.search(r"\.(files|library\.\w+|unscoped\.\w+)$", part) for part in os.path.relpath(directory, HERE).split(os.sep))


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--no-doc", action="store_true")
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--debug-info", action="store_true")
    ap.add_argument("--target", choices=["linux", "windows-gnu"])
    ap.add_argument("--shard", default="")
    args = ap.parse_args()
    shard = re.fullmatch(r"(\d+)/(\d+)", args.shard) if args.shard else None
    if args.shard and not (shard and 1 <= int(shard.group(1)) <= int(shard.group(2))):
        print("--shard takes I/N with 1 <= I <= N, not", args.shard)
        print("spec: FAILED: bad --shard"); return 2
    if shard:
        args.no_doc = True
    global OUT, TARGET, C_TARGET_FLAGS
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
    elif args.target:
        TARGET = args.target
        if not os.path.exists(os.path.join(SYSROOT_LINUX, "usr", "lib", "x86_64-linux-gnu", "libc.so.6")):
            print("no Linux sysroot at", SYSROOT_LINUX, "(tests\\targets.ps1 makes one from WSL)")
            print("spec: FAILED: no Linux sysroot"); return 2
        # (WSL started once first: its virtual machine, stopped when idle, took more than a program's
        # 60 s to start under a pool of programs at once)
        subprocess.run(["wsl.exe", "-e", "true"], capture_output=True, timeout=300)
        EXTRA.extend(["--target", "linux", "--sysroot", SYSROOT_LINUX])
        C_TARGET_FLAGS = ["--target=x86_64-unknown-linux-gnu", "--sysroot=" + SYSROOT_LINUX]
        OUT += "-" + TARGET
        args.no_doc = True
    if not os.path.exists(KPPC):
        print("no compiler at", KPPC)
        print("spec: FAILED: no compiler"); return 2
    os.makedirs(OUT, exist_ok=True)
    files = sorted(os.path.join(dp, f) for dp, dn, fn in os.walk(HERE) for f in fn
                   if f.endswith(".kpp") and not is_companion(dp) and args.filter in os.path.relpath(os.path.join(dp, f), HERE).replace("\\", "/"))
    if shard:
        files = files[int(shard.group(1)) - 1::int(shard.group(2))]
    start = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(run_one, files))
    elapsed = time.time() - start
    skipped = sum(1 for r in results if r["status"] == "skip")
    results = [r for r in results if r["status"] != "skip"]
    for r in results:
        tag = {"pass": "ok     ", "pending": "pending", "FAIL": "FAIL   ", "fixed": "FIXED  "}[r["status"]]
        line = f"{tag} {r['area']}/{r['name']}"
        if r["status"] in ("FAIL", "pending") and r["note"]:
            line += f"  -- {r['note']}"
        print(line)
    counts = {s: sum(1 for r in results if r["status"] == s) for s in ("pass", "pending", "FAIL", "fixed")}
    print(f"{counts['pass']} pass, {counts['pending']} pending, {counts['FAIL']} failing, {counts['fixed']} fixed" +
          (f", {skipped} skipped for {TARGET or 'windows'}" if skipped else "") + f" ({elapsed:.1f} s)")
    if not args.no_doc and not args.filter:
        write_doc(results, elapsed)
        print("wrote", os.path.relpath(DOC, ROOT))
    # The one line that says whether everything passed (a pending snippet passing is a failure: its
    # mark must go), and the exit code says the same:
    #   spec: ALL PASSED (898 pass, 2 pending)
    #   spec: 2 FAILED: types/enum_size, fixed lambdas/capture (897 pass, 1 pending, 1 failing, 1 fixed)
    summary = f"{counts['pass']} pass, {counts['pending']} pending" + (f", {skipped} skipped for {TARGET or 'windows'}" if skipped else "") + \
              (f"; shard {args.shard}" if shard else "")
    bad = [f"{r['area']}/{r['name']}" for r in results if r["status"] == "FAIL"] + \
          [f"fixed {r['area']}/{r['name']}" for r in results if r["status"] == "fixed"]
    if bad:
        shown = ", ".join(bad[:10]) + (f", and {len(bad) - 10} more" if len(bad) > 10 else "")
        print(f"spec: {len(bad)} FAILED: {shown} ({summary}, {counts['FAIL']} failing, {counts['fixed']} fixed)")
        return 1
    print(f"spec: ALL PASSED ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
