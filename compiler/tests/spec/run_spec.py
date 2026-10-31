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
    // run: alpha "two words"                    (built and run in one step by `kppc --run snippet ARGS`)
    // test: --filter parse                      (the snippet's tests run by `kppc --test snippet -- ARGS`, from
                                                  the snippet's directory: the paths printed are its name)
    // exit: 3                                   (the program's exit code; 0 or 1 when not given)

Statuses: pass, FAIL (a regression), pending (a known gap, still failing), fixed (a pending
snippet that passes: remove its pending line). The run writes docs/spec-status.md, the
specification of what kppc can and cannot do yet, with the first compiler complaint of every gap.

A <name>.c next to a snippet is a C library of its own: compiled with clang and linked into the
snippet's executable (`--lib`), for what only a real C object shows (the variables it defines).

    python compiler\\tests\\spec\\run_spec.py [--filter TEXT] [--jobs N] [--no-doc] [--release] [--debug-info]

--release and --debug-info build every snippet with that kppc option too (the release and the
debug-info configurations, docs/test-matrix.md), into build\\spec-release or build\\spec-debug-info,
and write no spec-status.md, which is the debug build's. A snippet that prints its own configuration
has the release build's output in <name>.release.expected.
"""
import argparse, os, re, shlex, subprocess, sys, concurrent.futures, time

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

AREA_TITLES = {
  "language": "Language: statements, expressions, declarations",
  "classes": "Classes and objects",
  "templates": "Templates",
  "library": "Standard library (what std:: converts to)",
  "kpp": "Kpp's own features (design.md sections 7 and 8)",
  "interop": "C interop",
}


def header(path):
    info = {"spec": os.path.splitext(os.path.basename(path))[0], "factorio": "", "pending": None, "errors": [], "run": None, "test": None, "exit": None, "kppc": ""}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"//\s*(spec|factorio|pending|error|run|test|exit|kppc):\s*(.*)", line.strip())
            if not m:
                if line.strip() and not line.startswith("//"):
                    break
                continue
            if m.group(1) == "error":
                info["errors"].append(m.group(2).strip())
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
    result = {"area": area, "name": name, "info": info, "note": ""}
    libs = []
    c_source = os.path.splitext(path)[0] + ".c"
    if os.path.exists(c_source):
        obj = os.path.join(OUT, f"{area}.{name}.c.obj")
        cc = subprocess.run([CLANG, "-c", "-O1", "-o", obj, c_source], capture_output=True, text=True, encoding="utf-8", errors="replace")
        if cc.returncode != 0:
            result["status"] = "FAIL"; result["note"] = f"its C source did not compile: {first_complaint(cc.stderr)}"; return finish(result)
        libs = ["--lib", obj]
    if info["run"] is not None or info["test"] is not None:
        return finish(run_command(path, info, libs, expected_path, result))
    try:
        c = subprocess.run([KPPC, "--quiet", *EXTRA, *shlex.split(info["kppc"]), *libs, "--exe", exe, path], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
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
    try:
        r = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
    except subprocess.TimeoutExpired:
        result["status"] = "FAIL"; result["note"] = "program timed out"; return finish(result)
    if r.returncode != 0 and r.returncode > 1:
        result["status"] = "FAIL"; result["note"] = f"program exited with 0x{r.returncode & 0xFFFFFFFF:X}"
        return finish(result)
    compare(r.stdout, expected_path, result)
    return finish(result)


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
    args = ap.parse_args()
    global OUT
    for flag in ("release", "debug_info"):
        if getattr(args, flag):
            option = "--" + flag.replace("_", "-")
            EXTRA.append(option)
            OUT += option[1:]
            args.no_doc = True
    if not os.path.exists(KPPC):
        print("no compiler at", KPPC); return 2
    os.makedirs(OUT, exist_ok=True)
    files = sorted(os.path.join(dp, f) for dp, dn, fn in os.walk(HERE) for f in fn
                   if f.endswith(".kpp") and args.filter in os.path.relpath(os.path.join(dp, f), HERE).replace("\\", "/"))
    start = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(run_one, files))
    elapsed = time.time() - start
    for r in results:
        tag = {"pass": "ok     ", "pending": "pending", "FAIL": "FAIL   ", "fixed": "FIXED  "}[r["status"]]
        line = f"{tag} {r['area']}/{r['name']}"
        if r["status"] in ("FAIL", "pending") and r["note"]:
            line += f"  -- {r['note']}"
        print(line)
    counts = {s: sum(1 for r in results if r["status"] == s) for s in ("pass", "pending", "FAIL", "fixed")}
    print(f"{counts['pass']} pass, {counts['pending']} pending, {counts['FAIL']} failing, {counts['fixed']} fixed ({elapsed:.1f} s)")
    if not args.no_doc and not args.filter:
        write_doc(results, elapsed)
        print("wrote", os.path.relpath(DOC, ROOT))
    return 1 if counts["FAIL"] or counts["fixed"] else 0


if __name__ == "__main__":
    sys.exit(main())
