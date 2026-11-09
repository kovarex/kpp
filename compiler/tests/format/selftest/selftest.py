"""run_format.py's own test: it runs the runner against standin.py, a toy `kpp format`, made wrong one
way at a time (STANDIN_BREAK), and checks that each way is reported, by its check's name, with exit
code 1, and that the right formatter passes. The snippets are written here, into
build\\tests\\format-selftest, and their golden files by the runner's --update from the right stand-in.
run_tests.cmd runs it; the last line is `format selftest: ALL PASSED` or names what was missed.
"""
import concurrent.futures
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
RUNNER = os.path.join(HERE, "..", "run_format.py")
STANDIN = os.path.join(HERE, "standin.py")
WORK = os.path.join(ROOT, "compiler", "build", "tests", "format-selftest")

SNIPPET = """// format: a class, a function and a namespace
namespace game
{
  class Inserter
  {
    i32 getX() const { return this.x; }

    void update()
    {
      if (this.x < 3)
      {
        this.x = this.x + 1;
      }
    }
  };
}
"""
CRLF = "// format: a CRLF file\r\nvoid f()\r\n{\r\n  g();\r\n}\r\n"
REFUSED = "// format: a file the broken stand-in refuses (refuse-me)\nvoid h() {}\n"
SETTINGS = ["indent", "columns", "braces", "case-indent", "namespace-indent", "access-label", "wrap-operator",
            "continuation", "pointer", "braced-list-spaces", "template-space", "initializer-commas", "blank-lines-between-functions"]


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def make_snippets(directory):
    if os.path.isdir(directory):
        shutil.rmtree(directory)
    write(os.path.join(directory, "classes.kpp"), SNIPPET)
    write(os.path.join(directory, "crlf.kpp"), CRLF)
    write(os.path.join(directory, "refused.kpp"), REFUSED)
    for name in SETTINGS:
        write(os.path.join(directory, "settings", f"{name}.kpp"), SNIPPET.replace("a class, a function and a namespace", f"what {name} touches"))
    write(os.path.join(directory, "combinations", "tabs-end.kpp"), SNIPPET)
    write(os.path.join(directory, "combinations", "tabs-end.kppstyle"), "indent = tab\nbraces = end-of-line\n")


def run(directory, extra=(), env_break="", corpus=()):
    env = dict(os.environ)
    env.pop("STANDIN_BREAK", None)
    if env_break:
        env["STANDIN_BREAK"] = env_break
    command = [sys.executable, "-I", RUNNER, "--kpp", STANDIN, "--here", directory, "--out", directory + ".out", "--jobs", "4", "--perturbations", "1", *extra]
    for c in corpus:
        command += ["--corpus", c]
    r = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    return r.returncode, r.stdout + r.stderr


def remove(directory, name):
    os.remove(os.path.join(directory, name))


def messy(directory):
    write(os.path.join(directory + ".messy", "a.kpp"), "void   f() {\n      g();\n}\n\n\n\nvoid k() {}   \n")


# (what, change to the snippets before the run, the runner's options, STANDIN_BREAK, exit code, texts the output must have)
CASES = [
    ("the right formatter passes", None, [], "", 0, ["17 files x 20 configurations, 1 perturbed copies each"]),
    ("a formatter that changes canonical text", None, [], "canonical", 1, ["[default] canonical:"]),
    ("a formatter that is not idempotent", None, [], "idempotent", 1, ["] idempotent:"]),
    ("a formatter that loses an empty line", None, [], "roundtrip", 1, ["] round trip:"]),
    ("a formatter that keeps the input's spaces", None, [], "whitespace", 1, ["] whitespace 1:"]),
    ("a value rendered differently", None, [], "golden", 1, ["settings/indent.kpp  [indent=4] golden: settings/indent.4.expected"]),
    ("a refused file", None, [], "refuse", 1, ["refused.kpp  [default] format: refused:", "tokens differ"]),
    ("a refused file with exit code 0", None, [], "exit0", 1, ["exit code 0, yet 1 files were not written"]),
    ("a crash", None, [], "crash", 1, ["kpp format exit code 3"]),
    ("a bad --list-settings", None, [], "settings", 1, ["--list-settings: not `NAME DEFAULT VALUE...`"]),
    ("a formatter that writes LF for a CRLF file", None, [], "lineends", 1,
     ["crlf.kpp  [default] line ends: CRLF in, LF out", "classes.kpp  [indent=4] line ends: its CRLF copy:"]),
    ("an unlisted number of columns accepted", None, [], "accept", 1,
     ["[unlisted] settings: `columns = 79` accepted", "`columns = 161` accepted", "`columns = 81` accepted"]),
    ("--check that names nothing", None, [], "check", 1, ["] check: the first perturbed copy: not named, yet formatting changes it"]),
    ("--check that names every file", None, [], "checkall", 1, ["] check: the formatted files: named, yet formatting leaves it as it is"]),
    ("a filtered run", None, ["--filter", "crlf"], "", 0, ["1 files x 20 configurations"]),
    ("a shard", None, ["--shard", "2/4"], "", 0, ["17 files x 5 configurations"]),
    ("a missing golden file", lambda d: remove(d, os.path.join("settings", "pointer.both.expected")), [], "", 1,
     ["settings/pointer.both.expected is missing"]),
    ("a golden file for the default", lambda d: write(os.path.join(d, "settings", "indent.2.expected"), SNIPPET), [], "", 1,
     ["settings/indent.2.expected is for no setting or value"]),
    ("a combination of no setting", lambda d: write(os.path.join(d, "combinations", "tabs-end.kppstyle"), "indent = 3\n"), [], "", 1,
     ["combinations/tabs-end.kppstyle:1: not a setting"]),
    ("a pending snippet still failing", lambda d: write(os.path.join(d, "refused.kpp"), "// format: pending the formatter refuses it\n" + REFUSED),
     [], "refuse", 0, ["pending refused.kpp: the formatter refuses it"]),
    ("a pending snippet that passes", lambda d: write(os.path.join(d, "refused.kpp"), "// format: pending the formatter refuses it\n" + REFUSED),
     [], "", 1, ["FIXED   refused.kpp"]),
    ("a directory corpus, formatted first", messy, ["corpus"], "", 0, ["1 files x 20 configurations"]),
]


def main():
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    base = os.path.join(WORK, "base")
    make_snippets(base)
    failed = []

    def expect(what, code, output, want_code, texts):
        last = output.strip().splitlines()[-1] if output.strip() else ""
        missing = [t for t in texts if t not in output]
        if code != want_code or missing or (want_code == 0) != last.startswith("format: ALL PASSED"):
            failed.append(what)
            return f"FAIL    {what}: exit {code} (expected {want_code}), missing {missing}\n" + "\n".join("        " + line for line in output.splitlines()[-12:])
        return f"ok      {what}"

    code, output = run(base, ["--update"])
    print(expect("--update writes the golden files", code, output, 0, ["golden files written"]))
    goldens = [f for f in os.listdir(os.path.join(base, "settings")) if f.endswith(".expected")]
    if len(goldens) != 18:  # (the stand-in's 13 settings have 18 values besides their defaults)
        failed.append("--update's golden files")
        print(f"FAIL    --update wrote {len(goldens)} golden files under settings/, not 18")

    def case(index, what, change, extra, env_break, want_code, texts):
        directory = os.path.join(WORK, f"case{index:02d}")
        shutil.copytree(base, directory)
        if change:
            change(directory)
        corpus = [directory + ".messy"] if extra == ["corpus"] else []
        code, output = run(directory, [] if corpus else extra, env_break, corpus)
        return expect(what, code, output, want_code, texts)

    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        for line in pool.map(lambda c: case(*c[0:1], *c[1]), enumerate(CASES)):
            print(line)
    shutil.rmtree(WORK, ignore_errors=True)
    if failed:
        print(f"format selftest: {len(failed)} FAILED: {', '.join(failed)}")
        return 1
    print(f"format selftest: ALL PASSED ({len(CASES) + 1} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
