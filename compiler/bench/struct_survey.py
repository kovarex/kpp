"""struct and class in the game's C++ sources, their Kpp conversion and the Kpp standard library
(docs/design.md, "struct and class"): definitions by keyword and what the structs hold (access
sections, constructors, virtual functions, bases), plus the game's structs that a "struct is public
data" rule would make classes. A regex survey with brace matching: right in proportion, not exact.
Usage: python compiler/bench/struct_survey.py [--offenders] (the conversion is programs\\factorio\\kpp,
from convert.cmd; the game's sources are read from C:\\Factorio)."""
import os, re, sys
from collections import Counter

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME = r"C:\Factorio"
ROOTS = {
    "game C++ (src, CommonUtil, Agui)": ([os.path.join(GAME, "src"), os.path.join(GAME, "libraries", "CommonUtil"), os.path.join(GAME, "libraries", "Agui")], (".cpp", ".hpp", ".h")),
    "game converted to Kpp (the same)": ([os.path.join(REPO, "programs", "factorio", "kpp", d) for d in ("src", "CommonUtil", "Agui")], (".kpp",)),
    "Kpp standard library": ([os.path.join(REPO, "stdlib")], (".kpp",)),
}
HEAD = re.compile(r"(?<![\w:])(?<!enum )(?<!enum  )(struct|class)\s+(?:\[\[[^\]]*\]\]\s*)?(?:alignas\([^)]*\)\s*)?([A-Za-z_]\w*)(\s*<[^;{()]*?>)?\s*(final\s*)?(:[^;{]*)?\{")
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
STRING = re.compile(r'"(?:\\.|[^"\\])*"')

def body_at(text, open_brace):
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:i]
    return text[open_brace + 1:]

def top_level(body):
    """The class body without what nested braces hold (function bodies, nested classes)."""
    out, depth = [], 0
    for c in body:
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif depth == 0:
            out.append(c)
    return "".join(out)

def definitions(dirs, exts):
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                if f.endswith(exts):
                    path = os.path.join(root, f)
                    text = STRING.sub('""', COMMENT.sub(" ", open(path, encoding="utf-8", errors="replace").read()))
                    for m in HEAD.finditer(text):
                        yield path, m, top_level(body_at(text, m.end() - 1))

def survey(dirs, exts):
    s = Counter()
    sizes = {"struct": [], "class": []}
    for _, m, body in definitions(dirs, exts):
        kw, name = m.group(1), m.group(2)
        s[kw] += 1
        sizes[kw].append(len([x for x in re.split(r";", body) if x.strip()]))
        private = bool(re.search(r"\b(private|protected)\s*:", body))
        public = bool(re.search(r"\bpublic\s*:", body))
        ctor = bool(re.search(r"(?<![\w~])" + re.escape(name) + r"\s*\(", body))
        virtual = "virtual" in body or "override" in body
        if kw == "struct":
            s["struct: plain data (no constructor, access section or virtual)"] += not private and not ctor and not virtual
            s["struct: a constructor"] += ctor
            s["struct: a private or protected section"] += private
            s["struct: virtual functions"] += virtual
            s["struct: bases"] += bool(m.group(5))
        else:
            s["class: virtual functions"] += virtual
            s["class: public members only"] += public and not private
    return s, sizes

if "--offenders" in sys.argv:
    dirs, exts = ROOTS["game C++ (src, CommonUtil, Agui)"]
    for path, m, body in definitions(dirs, exts):
        if m.group(1) == "struct":
            why = [w for w, hit in (("private/protected section", re.search(r"\b(private|protected)\s*:", body)), ("virtual", "virtual" in body or "override" in body)) if hit]
            if why:
                print(f"{m.group(2)} ({os.path.relpath(path, GAME)}): {', '.join(why)}")
    sys.exit(0)
for label, (dirs, exts) in ROOTS.items():
    s, sizes = survey(dirs, exts)
    print("==", label)
    for kw in ("struct", "class"):
        median = sorted(sizes[kw])[len(sizes[kw]) // 2] if sizes[kw] else 0
        print(f"  {kw}: {s[kw]} definitions, median {median} members")
    for k, v in s.items():
        if ":" in k:
            print(f"  {k}: {v} ({100.0 * v / max(s[k.split(':')[0]], 1):.0f}%)")
