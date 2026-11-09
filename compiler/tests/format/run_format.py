"""The formatter's test suite (docs/design.md, `kpp format`; F-formatter-design.md 7a).

Each configuration (one value for each of the formatter's settings) is tested over a corpus of
.kpp files already in the project style, kpp's default. For a configuration C and a file x:

    canonical    format(x, default) = x, byte for byte (in the default configuration)
    round trip   format(format(x, C), default) = x: a personal checkout and its commit
    idempotent   format(format(x, C), C) = format(x, C)
    unchanged    the formatter refused nothing: it checks on every run that its output has x's
                 tokens and comments, and writes nothing for a file that fails
    whitespace   format(p, C) = format(x, C) for every perturbed copy p of x (spaces and line
                 breaks moved between tokens by `kpp format --perturb SEED`, the five kept facts kept)
    line ends    each file keeps its line ends (kovarex 2026-11-09, Q11): format(x, C) has x's, and
                 format(l, C) is format(x, C) with the other line ends, l being x with the other
                 ones (CRLF for LF, LF for CRLF), so every file is tested both ways in every
                 configuration, whatever git does to a snippet's line ends
    check        `kpp format --check` agrees with formatting: it names no file of format(x, C), and
                 of the first perturbed copy p exactly those that format(p, C) changes
    golden       settings/NAME.kpp in NAME=VALUE is settings/NAME.VALUE.expected, and
                 combinations/NAME.kpp in combinations/NAME.kppstyle is combinations/NAME.expected
    settings     (once) a style giving a setting a value --list-settings does not list, or naming no
                 setting, is refused: nothing written, an exit code other than 0 and 1. Only the
                 tested values are accepted (kovarex 2026-11-09, Q13, said of `columns`)

The configurations: kpp's default; each value of each setting other than its default, every other
setting at its default (from `kpp format --list-settings`, so a new setting or value is tested at
once and needs its golden file); and each combinations/NAME.kppstyle, the settings that interact.

The corpus: `snippets` (this directory's *.kpp, settings/*.kpp, combinations/*.kpp: one construct or
difficulty each), `ours` (the .kpp files git tracks in stdlib, libraries, programs, kppnet and projectlib, each
first formatted in the default style, in a copy: our code is not reformatted until wrapping lands
and kovarex has seen the counts, Q9), or a directory.

run_tests' layout check becomes `kpp format --check` over our code once it is reformatted (Q10);
tools\\style\\kppstyle.py keeps the rules that are not layout.

A snippet's header says what it is about:

    // format: braced initializer lists written one item to a line
    // format: pending why                       (a known gap: its failures are expected; one that
                                                  passes every configuration fails the run: remove the line)

    python compiler\\tests\\format\\run_format.py [--corpus snippets|ours|DIR]... [--filter TEXT]
        [--configuration NAME]... [--allow-refused TEXT] [--jobs N] [--shard I/N] [--perturbations N] [--update] [--kpp EXE] [--out DIR]

--allow-refused TEXT: a file the first formatting refuses with TEXT in its message leaves the corpus, listed
and counted, and is no failure (a line splice between tokens: format.md, never formatted by design).
--configuration limits the run to the default and the configurations named, as the failures name them
(`indent=4`, `combination kr`): programs\\factorio\\format-check.cmd's round trip on the converted game.

What the runner asks of kpp format (F's side):
    kpp format --list-settings                     one setting a line: `NAME DEFAULT VALUE...`
    kpp format --style FILE --from IN --to OUT     each .kpp under IN formatted into OUT at its
                                                   path under IN; IN unchanged. Exit 0, or 1 when
                                                   some files were refused (their errors on stderr,
                                                   each naming its file), any other code a failure
    kpp format --perturb SEED --from IN --to OUT   the perturbed copies, the same way
    kpp format --check --style FILE IN             (format.md's --check PATH..., in a style) writes
                                                   nothing; exit 0 when every .kpp under IN is
                                                   formatted, 1 when some are not, each listed on
                                                   stdout or stderr; any other code a failure
A path it prints is IN's path joined with the file's, or the file's path under IN. format.md lists the
ones not built yet (2026-11-09: all but --check, which has no --style yet).

Each configuration runs in a directory of its own under --out (build\\tests\\format by default),
deleted when it passes. --update writes the golden files from the current output, for review in the
diff; it never runs in a gate. The last line says `format: ALL PASSED (...)` or `format: N FAILED:
...`, and the exit code is 0 only for the first.
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OURS = ["stdlib", "libraries", "programs", "kppnet", "projectlib"]


class Failure(Exception):
    pass


def command(kpp):
    """kpp, or a Python stand-in for it (the runner's own test)."""
    return [sys.executable, "-I", kpp] if kpp.endswith(".py") else [kpp]


def run_kpp_format(kpp, arguments, what):
    """Runs `kpp format ARGUMENTS`; returns the completed process, whatever its exit code."""
    try:
        return subprocess.run([*command(kpp), "format", *arguments], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
    except subprocess.TimeoutExpired:
        raise Failure(f"{what}: kpp format timed out")
    except OSError as e:
        raise Failure(f"{what}: kpp format did not start: {e}")


def kpp_format(kpp, arguments, what):
    """Runs `kpp format ARGUMENTS`; returns its exit code and stderr. A code other than 0 or 1 fails."""
    r = run_kpp_format(kpp, arguments, what)
    if r.returncode not in (0, 1):
        raise Failure(f"{what}: kpp format exit code {r.returncode & 0xFFFFFFFF}: {first_line(r.stderr)}")
    return r.returncode, r.stderr


def first_line(text):
    lines = [line for line in text.splitlines() if line.strip()]
    return lines[0][:300] if lines else "(nothing on stderr)"


def list_settings(kpp):
    """{name: [default, other values...]} from --list-settings."""
    try:
        r = subprocess.run([*command(kpp), "format", "--list-settings"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise Failure(f"kpp format --list-settings: {e}")
    if r.returncode != 0:
        raise Failure(f"kpp format --list-settings: exit code {r.returncode & 0xFFFFFFFF}: {first_line(r.stderr)}")
    settings = {}
    for line in r.stdout.splitlines():
        words = line.split()
        if not words:
            continue
        if len(words) < 3 or words[0] in settings or len(set(words[1:])) != len(words) - 1:
            raise Failure(f"kpp format --list-settings: not `NAME DEFAULT VALUE...` with values of their own: {line!r}")
        settings[words[0]] = words[1:]
    if not settings:
        raise Failure("kpp format --list-settings printed no setting")
    return settings


def read_style(path, settings, shown):
    """A .kppstyle's `key = value` lines, checked against the settings: the runner's own reading of
    a combination, to name it and to refuse one that names nothing kpp format has."""
    style = {}
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            text = line.split("#", 1)[0].strip()
            if not text:
                continue
            key, sep, value = (part.strip() for part in text.partition("="))
            if not sep or key not in settings or value not in settings[key] or key in style:
                raise Failure(f"{shown}:{number}: not a setting of kpp format given once: {line.strip()!r}")
            style[key] = value
    if not style:
        raise Failure(f"{shown}: sets nothing")
    return style


def configurations(settings, here):
    """[(name, {setting: value}, golden)]: golden is [(input, expected)] relative to the corpus."""
    result = [("default", {}, [])]
    for name, values in settings.items():
        for value in values[1:]:
            result.append((f"{name}={value}", {name: value}, [(f"settings/{name}.kpp", f"settings/{name}.{value}.expected")]))
    combinations = os.path.join(here, "combinations")
    if os.path.isdir(combinations):
        for file in sorted(os.listdir(combinations)):
            if file.endswith(".kppstyle"):
                stem = file[:-len(".kppstyle")]
                style = read_style(os.path.join(combinations, file), settings, f"combinations/{file}")
                result.append((f"combination {stem}", style, [(f"combinations/{stem}.kpp", f"combinations/{stem}.expected")]))
    return result


def layout_problems(settings, here, update):
    """What settings/ and combinations/ lack or hold for nothing: an input for each setting, a golden
    file for each value but the default, none for a value or a setting that does not exist."""
    problems = []
    directory = os.path.join(here, "settings")
    present = set(os.listdir(directory)) if os.path.isdir(directory) else set()
    wanted = set()
    for name, values in settings.items():
        wanted.add(f"{name}.kpp")
        wanted.update(f"{name}.{value}.expected" for value in values[1:])
    for file in sorted(wanted - present):
        if file.endswith(".kpp"):  # (a missing golden file is each configuration's failure)
            problems.append(f"settings/{file} is missing")
    for file in sorted(present - wanted):
        problems.append(f"settings/{file} is for no setting or value of kpp format --list-settings" +
                        (" (the default's text is the input itself)" if file.endswith(".expected") else ""))
    directory = os.path.join(here, "combinations")
    if os.path.isdir(directory):
        files = set(os.listdir(directory))
        for file in sorted(files):
            stem, ext = os.path.splitext(file)
            if ext not in (".kpp", ".kppstyle", ".expected"):
                problems.append(f"combinations/{file}: not a .kpp, .kppstyle or .expected")
            elif any(f"{stem}{other}" not in files for other in (".kpp", ".kppstyle") + (() if update else (".expected",))):
                problems.append(f"combinations/{stem}: needs {stem}.kpp, {stem}.kppstyle and {stem}.expected")
    return sorted(set(problems))


def corpus_files(corpus, here, filter_text):
    """[(relative path, source path, as is)] of each corpus, the relative paths unique. As is: a
    snippet, in the project style already; the others are formatted in the default style first."""
    files = []
    for name in corpus:
        if name == "snippets":
            for sub in ("", "settings", "combinations"):
                directory = os.path.join(here, sub)
                if os.path.isdir(directory):
                    files += [(os.path.join(sub, f).replace("\\", "/"), os.path.join(directory, f), True) for f in sorted(os.listdir(directory)) if f.endswith(".kpp")]
        elif name == "ours":
            r = subprocess.run(["git", "ls-files", "-z", "--", *[f"{d}/*.kpp" for d in OURS]], cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
            if r.returncode != 0:
                raise Failure(f"git ls-files: {first_line(r.stderr)}")
            files += [(f"ours/{p}", os.path.join(ROOT, p), False) for p in r.stdout.split("\0") if p]
        else:
            directory = os.path.abspath(name)
            if not os.path.isdir(directory):
                raise Failure(f"--corpus {name}: not snippets, ours or a directory")
            base = os.path.basename(directory.rstrip("\\/")) or "corpus"
            for parent, _, names in os.walk(directory):
                for f in sorted(names):
                    if f.endswith(".kpp"):
                        path = os.path.join(parent, f)
                        files.append((f"{base}/" + os.path.relpath(path, directory).replace("\\", "/"), path, False))
    files = [f for f in files if filter_text in f[0]]
    seen = set()
    for rel, _, _ in files:
        if rel.lower() in seen:
            raise Failure(f"the corpus has {rel} twice")
        seen.add(rel.lower())
    return files


def pending_reason(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("//"):
                break
            text = line[2:].strip()
            if text.startswith("format: pending"):
                return text[len("format: pending"):].strip() or "(no reason given)"
    return None


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def first_difference(want, got):
    """Where got departs from want: the line, and both versions of it."""
    a = want.split(b"\n")
    b = got.split(b"\n")
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else None
        y = b[i] if i < len(b) else None
        if x != y:
            show = lambda s: "(no line)" if s is None else repr(s.decode("utf-8", "replace"))[:120]
            return f"line {i + 1}: expected {show(x)}, got {show(y)}"
    return "the same lines, different bytes"


def write_style(path, style):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(f"{key} = {value}\n" for key, value in style.items()))


def named_files(lines, source, rels):
    """{rel: the first line naming it}: a line names a file by its path, under source or relative to it,
    as a whole word (crlf.kpp is not settings/crlf.kpp). Linear in the lines: the game has 4,574 files."""
    wanted = {rel.lower(): rel for rel in rels}
    prefix = source.replace("\\", "/").rstrip("/").lower() + "/"
    result = {}
    for line in lines:
        for word in re.split(r"[\s:'\"`,()]+", line.replace("\\", "/").lower().replace(prefix, " ")):
            if word in wanted:
                result.setdefault(wanted[word], line)
    return result


def refusals(stderr, source, missing):
    """Each missing file's line of stderr (the formatter names the file in its errors)."""
    said = named_files(stderr.splitlines(), source, missing)
    return {rel: said[rel][:300] if rel in said else "refused, and stderr does not name it" for rel in missing}


def line_ends(text):
    """LF, CRLF, mixed, or None for a text of no line end."""
    lf = text.count(b"\n")
    crlf = text.count(b"\r\n")
    if lf == 0:
        return None
    if crlf == lf and text.count(b"\r") == crlf:
        return "CRLF"
    return "LF" if b"\r" not in text else "mixed"


def other_line_ends(text):
    return text.replace(b"\r\n", b"\n") if line_ends(text) == "CRLF" else text.replace(b"\n", b"\r\n")


def unlisted_values(settings):
    """[(setting, value)] a style must not be given: a setting that does not exist, and for each one
    a value it does not list; for a setting with numbers among its values, a number just below and
    above them and one between that is not listed."""
    result = [("no-such-setting", settings[next(iter(settings))][0])]
    for name, values in settings.items():
        result.append((name, "unlisted"))
        numbers = sorted(int(v) for v in values if v.isdigit())
        if numbers:
            between = [n for n in range(numbers[0] + 1, numbers[-1]) if str(n) not in values]
            result += [(name, str(n)) for n in [numbers[0] - 1, numbers[-1] + 1] + between[:1] if str(n) not in values]
    return result


def refuse_unlisted(args, kpp, settings, xdir, rel):
    """The settings check: each unlisted value refused, nothing written. [(file, check, detail)]."""
    work = os.path.join(args.out, "unlisted")
    source = os.path.join(work, "in")
    os.makedirs(os.path.dirname(os.path.join(source, rel)), exist_ok=True)
    shutil.copyfile(os.path.join(xdir, rel), os.path.join(source, rel))
    failures = []
    for i, (name, value) in enumerate(unlisted_values(settings)):
        style = os.path.join(work, f"s{i}.kppstyle")
        with open(style, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"{name} = {value}\n")
        target = os.path.join(work, f"o{i}")
        r = run_kpp_format(kpp, ["--style", style, "--from", source, "--to", target], f"`{name} = {value}`")
        written = os.path.isdir(target) and any(files for _, _, files in os.walk(target))
        if r.returncode in (0, 1) or written:
            failures.append(("", "settings", f"`{name} = {value}` accepted (exit code {r.returncode & 0xFFFFFFFF}" +
                             (", files written" if written else "") + "): only the values --list-settings lists are"))
    if not failures:
        shutil.rmtree(work, ignore_errors=True)
    return failures


def run_configuration(args, kpp, index, name, style, golden, xdir, rels, default_style, perturbed, ldir, swapped):
    """[(file, check, detail)] of one configuration; its directory is deleted when it passes."""
    work = os.path.join(args.out, f"c{index:03d}")
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    style_file = os.path.join(work, "style.kppstyle")
    write_style(style_file, style)
    failures = []

    def run(style_path, source, target, check):
        """Formats a tree; returns {rel: bytes} for the files written, recording refusals and files
        written that were not asked for."""
        code, stderr = kpp_format(kpp, ["--style", style_path, "--from", source, "--to", target], f"{name} {check}")
        written = {}
        missing = []
        for rel in rels:
            path = os.path.join(target, rel)
            if os.path.isfile(path):
                written[rel] = read_bytes(path)
            elif os.path.isfile(os.path.join(source, rel)):
                missing.append(rel)
        for rel, said in refusals(stderr, source, missing).items():
            failures.append((rel, check, f"refused: {said}"))
        if code == 1 and not missing:
            failures.append(("", check, f"exit code 1, yet every file was written: {first_line(stderr)}"))
        if code == 0 and missing:
            failures.append(("", check, f"exit code 0, yet {len(missing)} files were not written"))
        return written

    x = {rel: read_bytes(os.path.join(xdir, rel)) for rel in rels}
    adir = os.path.join(work, "A")
    a = run(style_file, xdir, adir, "format")
    if name == "default":
        for rel in rels:
            if rel in a and a[rel] != x[rel]:
                failures.append((rel, "canonical", first_difference(x[rel], a[rel])))
    if name != "default":
        again = run(style_file, adir, os.path.join(work, "AA"), "idempotent")
        for rel in rels:
            if rel in a and rel in again and again[rel] != a[rel]:
                failures.append((rel, "idempotent", first_difference(a[rel], again[rel])))
        back = run(default_style, adir, os.path.join(work, "B"), "round trip")
        for rel in rels:
            if rel in a and rel in back and back[rel] != x[rel]:
                failures.append((rel, "round trip", first_difference(x[rel], back[rel])))
    first_perturbed = None
    for seed, pdir in perturbed:
        p = run(style_file, pdir, os.path.join(work, f"AP{seed}"), f"whitespace {seed}")
        for rel in rels:
            if rel in a and rel in p and p[rel] != a[rel]:
                failures.append((rel, f"whitespace {seed}", first_difference(a[rel], p[rel])))
        if first_perturbed is None:
            first_perturbed = (pdir, p)
    for rel in rels:
        if rel in a:
            want, got = line_ends(x[rel]), line_ends(a[rel])
            if want in ("LF", "CRLF") and got != want:
                failures.append((rel, "line ends", f"{want} in, {got or 'no line end'} out"))
    if swapped:
        swapped_out = run(style_file, ldir, os.path.join(work, "AL"), "line ends")
        for rel in swapped:
            if rel in a and rel in swapped_out and swapped_out[rel] != other_line_ends(a[rel]):
                failures.append((rel, "line ends", f"its {line_ends(other_line_ends(x[rel]))} copy: " + first_difference(other_line_ends(a[rel]), swapped_out[rel])))

    def check(source, formatted, what):
        """`kpp format --check` over source: the files it names, each one compared with formatted."""
        r = run_kpp_format(kpp, ["--check", "--style", style_file, source], f"{name} check")
        code, stderr = r.returncode, r.stdout + r.stderr
        if code not in (0, 1):
            raise Failure(f"{name} check: kpp format --check exit code {code & 0xFFFFFFFF}: {first_line(r.stderr)}")
        lines = stderr.splitlines()
        named = named_files(lines, source, formatted)
        for rel in sorted(formatted):
            changed = formatted[rel] != read_bytes(os.path.join(source, rel))
            if changed and rel not in named:
                failures.append((rel, "check", f"{what}: not named, yet formatting changes it"))
            elif rel in named and not changed:
                failures.append((rel, "check", f"{what}: named, yet formatting leaves it as it is"))
        if code == 0 and named:
            failures.append(("", "check", f"{what}: exit code 0, yet it names {len(named)} files"))
        if code == 1 and not named:
            failures.append(("", "check", f"{what}: exit code 1, naming no file: {first_line(stderr)}"))

    check(adir, a, "the formatted files")
    if first_perturbed:
        check(first_perturbed[0], first_perturbed[1], "the first perturbed copy")
    for source, expected in golden:
        if source not in x:
            continue  # (filtered out)
        path = os.path.join(args.here, expected)
        if source not in a:
            continue  # (its refusal is reported)
        if args.update:
            with open(path, "wb") as f:
                f.write(a[source])
        elif not os.path.isfile(path):
            failures.append((source, "golden", f"{expected} is missing (--update writes it, for review)"))
        elif read_bytes(path) != a[source]:
            failures.append((source, "golden", f"{expected}: " + first_difference(read_bytes(path), a[source])))
    if not failures:
        shutil.rmtree(work, ignore_errors=True)
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", action="append", default=[])
    ap.add_argument("--filter", default="")
    ap.add_argument("--configuration", action="append", default=[])
    ap.add_argument("--allow-refused", default="")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--shard", default="")
    ap.add_argument("--perturbations", type=int, default=3)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--kpp", default=os.path.join(ROOT, "compiler", "build", "kpp.exe" if os.name == "nt" else "kpp"))
    ap.add_argument("--out", default=os.path.join(ROOT, "compiler", "build", "tests", "format"))
    # --here DIR: the snippets, settings/ and combinations/ of another directory (the runner's own test)
    ap.add_argument("--here", default=HERE)
    args = ap.parse_args()
    args.here = os.path.abspath(args.here)
    args.out = os.path.abspath(args.out)
    corpus = args.corpus or ["snippets"]
    started = time.time()
    try:
        settings = list_settings(args.kpp)
        problems = layout_problems(settings, args.here, args.update) if "snippets" in corpus and not args.filter else []
        configs = configurations(settings, args.here)
        files = corpus_files(corpus, args.here, args.filter)
    except Failure as e:
        print(f"format: FAILED: {e}")
        return 1
    for problem in problems:
        print(f"FAIL    {problem}")
    if args.configuration:
        unknown = sorted(set(args.configuration) - {name for name, _, _ in configs})
        if unknown:
            print(f"format: FAILED: --configuration {unknown[0]}: not a configuration of this kpp format")
            return 1
        chosen = {"default", *args.configuration}
        configs = [c for c in configs if c[0] in chosen]
    if args.shard:
        part, count = (int(n) for n in args.shard.split("/"))
        configs = [c for i, c in enumerate(configs) if i % count == part - 1]
    if not files:
        print("format: FAILED: the corpus has no file")
        return 1
    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out)
    # x: the corpus as it is, or (ours and a directory) formatted in the default style first
    sdir = os.path.join(args.out, "source")
    for rel, src, _ in files:
        os.makedirs(os.path.dirname(os.path.join(sdir, rel)), exist_ok=True)
        shutil.copyfile(src, os.path.join(sdir, rel))
    default_style = os.path.join(args.out, "default.kppstyle")
    write_style(default_style, {})
    xdir = sdir
    failures = []
    allowed = []
    if not all(as_is for _, _, as_is in files):
        xdir = os.path.join(args.out, "x")
        try:
            code, stderr = kpp_format(args.kpp, ["--style", default_style, "--from", sdir, "--to", xdir], "the corpus in the default style")
        except Failure as e:
            print(f"format: FAILED: {e}")
            return 1
        for rel in [rel for rel, _, as_is in files if as_is]:  # (the snippets as they are)
            os.makedirs(os.path.dirname(os.path.join(xdir, rel)), exist_ok=True)
            shutil.copyfile(os.path.join(sdir, rel), os.path.join(xdir, rel))
        missing = [rel for rel, _, _ in files if not os.path.isfile(os.path.join(xdir, rel))]
        for rel, said in refusals(stderr, sdir, missing).items():
            if args.allow_refused and args.allow_refused in said:
                allowed.append(rel)
                print(f"refused {rel}: {said}")
            else:
                failures.append(("default, first", rel, "format", f"refused: {said}"))
        files = [f for f in files if f[0] not in missing]
        if not files:
            for name, rel, check, detail in failures:
                print(f"FAIL    {rel}  [{name}] {check}: {detail}")
            print("format: FAILED: no file is left after the first formatting")
            return 1
    rels = [rel for rel, _, _ in files]
    # the perturbed copies of x, made once for every configuration
    perturbed = []
    for seed in range(1, args.perturbations + 1):
        pdir = os.path.join(args.out, f"P{seed}")
        try:
            code, stderr = kpp_format(args.kpp, ["--perturb", str(seed), "--from", xdir, "--to", pdir], f"perturb {seed}")
        except Failure as e:
            print(f"format: FAILED: {e}")
            return 1
        if code != 0:
            failures.append(("perturb", "", f"seed {seed}", f"exit code {code}: {first_line(stderr)}"))
            continue
        for rel in rels:
            if not os.path.isfile(os.path.join(pdir, rel)):
                failures.append(("perturb", rel, f"seed {seed}", "no perturbed copy written"))
        perturbed.append((seed, pdir))
    # x with the other line ends, each file that has one kind
    ldir = os.path.join(args.out, "L")
    swapped = []
    for rel in rels:
        text = read_bytes(os.path.join(xdir, rel))
        if line_ends(text) in ("LF", "CRLF"):
            os.makedirs(os.path.dirname(os.path.join(ldir, rel)), exist_ok=True)
            with open(os.path.join(ldir, rel), "wb") as f:
                f.write(other_line_ends(text))
            swapped.append(rel)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        jobs = [(name, pool.submit(run_configuration, args, args.kpp, index, name, style, golden, xdir, rels, default_style, perturbed, ldir, swapped))
                for index, (name, style, golden) in enumerate(configs)]
        if rels and (not args.shard or args.shard.split("/")[0] == "1"):
            jobs.append(("unlisted", pool.submit(refuse_unlisted, args, args.kpp, settings, xdir, rels[0])))
        for name, job in jobs:
            try:
                failures += [(name, rel, check, detail) for rel, check, detail in job.result()]
            except Failure as e:
                failures.append((name, "", "run", str(e)))
    pending = {rel: pending_reason(src) for rel, src, _ in files}
    pending = {rel: why for rel, why in pending.items() if why is not None}
    failed_files = {rel for _, rel, _, _ in failures}
    shown = []
    expected_failures = 0
    for name, rel, check, detail in sorted(failures, key=lambda f: (f[1], f[0], f[2])):
        if rel in pending:
            expected_failures += 1
            continue
        shown.append(f"FAIL    {rel or '(all)'}  [{name}] {check}: {detail}")
    fixed = sorted(rel for rel in pending if rel not in failed_files)
    for line in shown[:200]:
        print(line)
    if len(shown) > 200:
        print(f"... and {len(shown) - 200} more")
    for rel in fixed:
        print(f"FIXED   {rel}: passes every configuration now: remove its `format: pending` line")
    for rel, why in sorted(pending.items()):
        if rel in failed_files:
            print(f"pending {rel}: {why}")
    count = len(shown) + len(fixed) + len(problems)
    summary = f"{len(files)} files x {len(configs)} configurations, {args.perturbations} perturbed copies each, {len(pending)} pending, {time.time() - started:.1f} s"
    if allowed:
        summary += f", {len(allowed)} refused as allowed"
    if args.update:
        summary += ", golden files written"
    if count == 0:
        shutil.rmtree(args.out, ignore_errors=True)
        print(f"format: ALL PASSED ({summary})")
        return 0
    print(f"format: {count} FAILED ({summary}); kept: {args.out}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
