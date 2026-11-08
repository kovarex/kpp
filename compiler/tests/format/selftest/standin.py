"""A stand-in for `kpp format`, for run_format.py's own test (selftest.py): the interface run_format.py
uses, with a toy formatter behind it. It indents by braces (the `indent` setting), puts an expanded
body's `{` on its own line or at the end of the line before (`braces`), keeps one space between words
and drops trailing spaces; every other setting renders as the default. Its perturbed copies move
leading, trailing and inner spaces and an own-line `{`, never an empty line.

STANDIN_BREAK=WHAT makes it wrong in one way, for the test to see run_format.py report it:
canonical, idempotent, roundtrip, whitespace, golden, refuse, crash, exit0, settings.
"""
import os
import random
import sys
import zlib

SETTINGS = [
    ("indent", ["2", "4", "tab"]),
    ("columns", ["130", "100", "80", "160"]),
    ("braces", ["own-line", "end-of-line"]),
    ("case-indent", ["yes", "no"]),
    ("namespace-indent", ["yes", "no"]),
    ("access-label", ["class", "member"]),
    ("wrap-operator", ["end", "start"]),
    ("continuation", ["align", "4", "8"]),
    ("pointer", ["type", "name", "both"]),
    ("braced-list-spaces", ["no", "yes"]),
    ("template-space", ["no", "yes"]),
    ("initializer-commas", ["leading", "trailing"]),
    ("blank-lines-between-functions", ["1", "2"]),
]
BREAK = os.environ.get("STANDIN_BREAK", "")


def read_style(path):
    known = dict(SETTINGS)
    style = {}
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            text = line.strip()
            if not text:
                continue
            key, _, value = (part.strip() for part in text.partition("="))
            if key not in known or value not in known[key]:
                sys.stderr.write(f"{path}:{number}: error: unknown setting or value: {text}\n")
                sys.exit(2)
            style[key] = value
    return style


def format_text(text, style):
    newline = "\r\n" if "\r\n" in text.split("\n", 1)[0] + "\n" else "\n"
    unit = {"2": "  ", "4": "    ", "tab": "\t"}[style.get("indent", "2")]
    if BREAK == "golden" and style.get("indent") == "4":
        unit = "   "
    words = [" ".join(line.split()) if BREAK != "whitespace" else line.strip() for line in text.replace("\r\n", "\n").split("\n")]
    # an own-line `{` and one at the end of the line before are the same: split it off first
    lines = []
    for line in words:
        if line.endswith(" {") and not line.startswith("//"):
            lines += [line[:-2], "{"]
        else:
            lines.append(line)
    out = []
    depth = 0
    blank = False
    for line in lines:
        if not line:
            blank = bool(out)
            continue
        if line.startswith("}"):
            depth -= 1
        if blank and not (BREAK == "roundtrip" and style):
            out.append("")
        blank = False
        if line == "{" and style.get("braces") == "end-of-line" and out and out[-1]:
            out[-1] += " {"
        else:
            out.append(unit * max(depth, 0) + line)
        depth += line.count("{") - line.count("}") + (1 if line.startswith("}") else 0)
    if BREAK == "canonical" and not style and out:
        out[0] += " "
    if BREAK == "idempotent" and style and out:
        first = text.replace("\r\n", "\n").split("\n", 1)[0]
        out[0] += " " * (len(first) - len(first.rstrip(" ")) + 1)
    return newline.join(out) + newline


def perturb_text(text, rng):
    newline = "\r\n" if "\r\n" in text.split("\n", 1)[0] + "\n" else "\n"
    out = []
    for line in text.replace("\r\n", "\n").split("\n"):
        if not line.strip():
            out.append(line)
            continue
        if line.strip() == "{" and out and out[-1].strip() and rng.random() < 0.5:
            out[-1] += " " * rng.randint(1, 3) + "{"
            continue
        inner = (" " * rng.randint(1, 3)).join(line.split())
        out.append(rng.choice(["", " ", "\t", "      "]) + inner + " " * rng.randint(0, 2))
    return newline.join(out)


def each_file(source):
    for parent, _, names in os.walk(source):
        for name in sorted(names):
            if name.endswith(".kpp"):
                path = os.path.join(parent, name)
                yield path, os.path.relpath(path, source)


def main(argv):
    if argv[:1] != ["format"]:
        sys.stderr.write("standin: only `format`\n")
        return 2
    argv = argv[1:]
    if argv == ["--list-settings"]:
        if BREAK == "settings":
            print("indent")
        for name, values in SETTINGS:
            print(name, *values)
        return 0
    options = dict(zip(argv[::2], argv[1::2]))
    if len(argv) % 2 or not {"--from", "--to"} <= options.keys() or len(options.keys() & {"--style", "--perturb"}) != 1:
        sys.stderr.write(f"standin: not --style FILE or --perturb SEED, with --from IN --to OUT: {argv}\n")
        return 2
    if BREAK == "crash":
        return 3
    style = read_style(options["--style"]) if "--style" in options else None
    refused = 0
    for path, rel in each_file(options["--from"]):
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
        if style is None:
            rng = random.Random(int(options["--perturb"]) * 1000003 + zlib.crc32(rel.replace("\\", "/").encode()))
            result = perturb_text(text, rng)
        elif BREAK in ("refuse", "exit0") and "refuse-me" in text:
            refused += 1
            if BREAK == "refuse":
                sys.stderr.write(f"{path}: error: the output's tokens differ from the input's: nothing written\n")
            continue
        else:
            result = format_text(text, style)
        target = os.path.join(options["--to"], rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "w", encoding="utf-8", newline="") as f:
            f.write(result)
    return 1 if refused and BREAK != "exit0" else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
