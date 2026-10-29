"""How much of a kppc program's IR is calls to trivial functions (docs/compile-speed.md, "Design:
inlining trivial functions"): a function is trivial here when its body is one basic block of at
most MAX instructions with no call. Counts the trivial functions, the call sites (call and invoke)
that name one, and all call sites, from IR as `kppc --emit-llvm FILE` writes it.
Usage: python compiler/bench/trivial_calls.py FILE.ll [MAX] (MAX defaults to 12)."""
import re, sys
from collections import Counter

path = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 12
DEFINE = re.compile(r'^define [^@]*(@"[^"]+"|@[\w.$]+)\(')
CALLEE = re.compile(r'\b(?:call|invoke)\b[^@]*?(@"[^"]+"|@[\w.$]+)\(')

trivial = {}          # name -> instruction count
calls = Counter()     # callee -> call sites
total_calls = 0
functions = 0
name, body = None, []

def finish():
    global name, body
    if name is None:
        return
    instructions = [l for l in body if l.startswith("  ") and not l.strip().startswith(";")]
    blocks = sum(1 for l in body if l and not l.startswith(" ") and l.endswith(":"))
    has_call = any(re.search(r"\b(call|invoke)\b", l) for l in instructions)
    if not has_call and blocks <= 1 and len(instructions) <= limit:
        trivial[name] = len(instructions)
    name, body = None, []

with open(path, encoding="utf-8", errors="replace") as f:
    for line in f:
        line = line.rstrip("\n")
        if name is None:
            m = DEFINE.match(line)
            if m:
                name, body = m.group(1), []
                functions += 1
            continue
        if line == "}":
            finish()
            continue
        body.append(line)
        for m in CALLEE.finditer(line):
            callee = m.group(1)
            if not callee.startswith("@llvm."):
                calls[callee] += 1
                total_calls += 1

to_trivial = sum(n for c, n in calls.items() if c in trivial)
print(f"{functions} functions defined, {len(trivial)} trivial (one block, at most {limit} instructions, no call)")
print(f"{total_calls} call sites, {to_trivial} ({100.0 * to_trivial / max(total_calls, 1):.1f}%) to a trivial function")
by_name = Counter()
for c, n in calls.items():
    if c in trivial:
        stem = re.sub(r"<.*?>", "<>", c.strip('@"'))
        by_name[re.sub(r"\(.*", "", stem)] += n
print("most called trivial functions (template arguments folded):")
for stem, n in by_name.most_common(25):
    print(f"  {n:8d}  {stem}")
