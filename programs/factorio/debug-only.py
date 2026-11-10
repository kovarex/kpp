"""The game's debug-only code is config::debug's in the converted tree (docs/design.md 12, "The game's
DEBUG as config::debug", and 19): each name below, debug-only in the C++ (under #ifdef DEBUG or #ifndef
NDEBUG), occurs in its converted file only in a `when (config::debug)` declaration or inside the
debug branch of an `if constexpr (config::debug)` (or the release branch's opposite, `if constexpr
(!config::debug)`'s else), so the release build has none of it; and a constant whose value differs
is one choosing by the configuration (`IsDebug = config::debug ? true : false`). convert.cmd compares
the output with tests\\configurations.expected.

    python -I debug-only.py KPPDIR
"""
import os, re, sys

CHECKS = [
    ('src/Belt/TransportLine.kpp', 'referenceSet', "TransportLine's referenceSet (an unordered_set in every TransportLine)"),
    ('src/Util/Rcu.kpp', 'writeLockCount', "Rcu's writeLockCount (an atomic counter)"),
]
# (a constant whose value the configuration chooses: the choice must be written so)
CHOICES = [
    ('src/BuildConstants.kpp', r'IsDebug\s*=\s*config::debug\s*\?\s*true\s*:\s*false', "BuildConstants' IsDebug = config::debug ? true : false"),
]

def code_only(text):
    # comments and literals blanked, offsets kept
    out = list(text)
    i = 0
    while i < len(text):
        if text.startswith('//', i):
            e = text.find('\n', i)
            e = len(text) if e < 0 else e
        elif text.startswith('/*', i):
            e = text.find('*/', i + 2)
            e = len(text) if e < 0 else e + 2
        elif text[i] == '"' or (text[i] == "'" and not (i > 0 and text[i - 1].isalnum())):
            e = i + 1
            while e < len(text) and text[e] != text[i] and text[e] != '\n':
                e += 2 if text[e] == '\\' else 1
            e += 1
        else:
            i += 1
            continue
        for k in range(i, min(e, len(text))):
            if out[k] != '\n':
                out[k] = ' '
        i = e
    return ''.join(out)

def debug_ranges(code):
    """The [begin, end) of each block the debug configuration alone compiles."""
    ranges = []
    stack = []        # (kind, start) of open blocks
    last_closed = {}  # depth -> kind of the block last closed there
    statement_start = 0
    for i, c in enumerate(code):
        if c == '{':
            head = code[statement_start:i]
            depth = len(stack)
            if re.search(r'if\s+constexpr\s*\(\s*config::debug\s*\)\s*$', head):
                kind = 'debug'
            elif re.search(r'if\s+constexpr\s*\(\s*!\s*config::debug\s*\)\s*$', head):
                kind = 'release'
            elif re.fullmatch(r'\s*else\s*', head) and last_closed.get(depth) in ('debug', 'release'):
                kind = 'release' if last_closed[depth] == 'debug' else 'debug'
            else:
                kind = 'other'
            stack.append((kind, i))
            statement_start = i + 1
        elif c == '}':
            if stack:
                kind, start = stack.pop()
                if kind == 'debug':
                    ranges.append((start, i + 1))
                last_closed[len(stack)] = kind
            statement_start = i + 1
        elif c == ';':
            statement_start = i + 1
    # a `when (config::debug)` declaration, to its `;` (or its body's `}`) at its own depth
    for m in re.finditer(r'\bwhen\s*\(\s*config::debug\s*\)', code):
        depth, k = 0, m.end()
        while k < len(code):
            if code[k] in '([{':
                depth += 1
            elif code[k] in ')]}':
                depth -= 1
                if depth == 0 and code[k] == '}':
                    break
            elif code[k] == ';' and depth == 0:
                break
            k += 1
        ranges.append((m.start(), k + 1))
    # an `if constexpr (config::debug)` of one statement without braces (the converter's form for one
    # short statement), to the statement's `;`
    for m in re.finditer(r'\bif\s+constexpr\s*\(\s*config::debug\s*\)\s*(?=[^\s{])', code):
        depth, k = 0, m.end()
        while k < len(code):
            if code[k] in '([{':
                depth += 1
            elif code[k] in ')]}':
                depth -= 1
            elif code[k] == ';' and depth == 0:
                break
            k += 1
        ranges.append((m.start(), k + 1))
    # an assert's expression too: kpp's assert is nothing, its expression unchecked, where
    # config::debug is false (TransportLine's `assert(line->seen == (this->referenceSet.count(line) > 0))`)
    for m in re.finditer(r'\bassert\s*\(', code):
        depth, k = 0, m.end() - 1
        while k < len(code):
            depth += code[k] == '('
            depth -= code[k] == ')'
            if depth == 0:
                break
            k += 1
        ranges.append((m.start(), k + 1))
    return ranges

kpp = sys.argv[1]
for path, name, what in CHECKS:
    text = open(os.path.join(kpp, path), encoding='utf-8').read()
    code = code_only(text)
    ranges = debug_ranges(code)
    uses = [m.start() for m in re.finditer(re.escape(name), code)]
    outside = [u for u in uses if not any(b <= u < e for b, e in ranges)]
    if not uses:
        print('%s: not found in %s' % (what, path))
    elif outside:
        line = code.count('\n', 0, outside[0]) + 1
        print('%s: %d of %d uses outside config::debug (%s:%d)' % (what, len(outside), len(uses), path, line))
    else:
        print('%s: config::debug only' % what)
for path, pattern, what in CHOICES:
    code = code_only(open(os.path.join(kpp, path), encoding='utf-8').read())
    print('%s: %s' % (what, 'chosen by the configuration' if re.search(pattern, code) else 'not found in ' + path))
