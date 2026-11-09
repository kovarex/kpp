r"""overrides/implicit-dropped.txt from kpp's --implicit-uses files of the game's builds: the `implicit` marks the
converter wrote that no build asks about (design.md "Explicit by default"; kovarex 2026-11-09: drop the unneeded `implicit`).

  python -I implicit-dropped.py CHECKOUT OUT USES...

CHECKOUT is the factorio-in-kpp checkout the builds compiled, OUT the list to write, each USES one build's
--implicit-uses file (implicit-dropped.cmd makes them: Windows, Linux and FreeBSD, debug, release and final,
with and without the tests, but final with them). A mark goes only where every build lists it with `asked 0`: code a configuration
leaves out (a false `when`) asks nothing in that build, so one asked about on one platform alone stays. A
conditional mark (`implicit(cond)`) stays. The list is by FileConverter::implicitKey, the converted file's key
(its path below libraries\ or the checkout) and `Class::Class(a, b)` or `Class::operator()`, its parameters'
names as the declaration writes them: where two marks of one file share a key, it goes only if both may go.
Marks in hand-written text (overrides\added's files, an override appended to a converted file) are no
converter's and are left out of the list.
"""
import os
import re
import sys

MARK = re.compile(r'^(.*):(\d+)\t(constructor|conversion)\t([^\t]+)\tuses (\d+)\tasked (\d+)(\t.*)?$')
IDENT = re.compile(r'[A-Za-z_]\w*')
NOT_A_NAME = {'const', 'volatile', 'signed', 'unsigned', 'struct', 'class', 'enum', 'typename', 'mut'}


def tokens(text):
    return re.findall(r'[A-Za-z_]\w*|::|&&|\.\.\.|\S', text)


def parameter_name(param):
    """The name a parameter's text declares, `_` for none (a type alone)."""
    depth = 0
    cut = len(param)
    for i, c in enumerate(param):
        if c in '<([{':
            depth += 1
        elif c in '>)]}':
            depth -= 1
        elif c == '=' and depth == 0 and param[i + 1:i + 2] != '=' and param[i - 1:i] not in '=!<>':
            cut = i
            break
    toks = tokens(param[:cut])
    if len(toks) >= 2 and IDENT.fullmatch(toks[-1]):
        before = toks[-2]
        if (IDENT.fullmatch(before) and before not in NOT_A_NAME) or before in ('*', '&', '&&', '>', '...'):
            return toks[-1]
    return '_'


def split_parameters(text):
    params, depth, start = [], 0, 0
    for i, c in enumerate(text):
        if c in '<([{':
            depth += 1
        elif c in '>)]}':
            depth -= 1
        elif c == ',' and depth == 0:
            params.append(text[start:i])
            start = i + 1
    if text[start:].strip():
        params.append(text[start:])
    return params


def declaration_key(lines, line, kind, qualified):
    """The mark's key from the converted declaration at LINE (1-based), None where it is not plainly there."""
    parts = qualified.split('::')
    if len(parts) < 2:
        return None
    cls = parts[-2]
    text = '\n'.join(lines[line - 1:line + 12])
    m = re.search(r'\bimplicit\b(?!\s*\()', text)
    if not m:
        return None
    open_paren = text.find('(', m.end())
    if open_paren < 0:
        return None
    depth = 0
    for close in range(open_paren, len(text)):
        if text[close] == '(':
            depth += 1
        elif text[close] == ')':
            depth -= 1
            if depth == 0:
                break
    else:
        return None
    names = [parameter_name(p) for p in split_parameters(text[open_paren + 1:close])]
    name = 'operator' if kind == 'conversion' else cls
    return f'{cls}::{name}({", ".join(names)})'


def main():
    checkout = os.path.abspath(sys.argv[1])
    out = sys.argv[2]
    added = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'overrides', 'added')
    asked = {}       # (file, key) -> highest asked count of any build, None for a conditional mark
    unread = 0
    texts = {}
    for uses in sys.argv[3:]:
        with open(uses, encoding='utf-8', errors='replace') as f:
            for row in f:
                m = MARK.match(row.rstrip('\n'))
                if not m:
                    continue
                path, line, kind, qualified, _, count, rest = m.groups()
                full = os.path.abspath(path)
                rel = os.path.relpath(full, checkout).replace('\\', '/')
                if rel.startswith('..'):
                    continue  # the standard library's
                if os.path.exists(os.path.join(added, rel.replace('/', os.sep))):
                    continue  # a hand-written file
                key_file = rel[len('libraries/'):] if rel.startswith('libraries/') else rel
                if full not in texts:
                    with open(full, encoding='utf-8', errors='replace') as k:
                        texts[full] = k.read().split('\n')
                lines = texts[full]
                if any(l.startswith('// --- Hand-written, from ') for l in lines[:int(line) - 1]):
                    continue  # an override's text
                key = declaration_key(lines, int(line), kind, qualified)
                if key is None:
                    unread += 1
                    continue
                k = (key_file, key)
                if rest and 'conditional' in rest:
                    asked[k] = None
                elif k not in asked:
                    asked[k] = int(count)
                elif asked[k] is not None:
                    asked[k] = max(asked[k], int(count))
    dropped = sorted(f'{f}: {k}' for (f, k), n in asked.items() if n == 0)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# The `implicit` marks the converter leaves out (FILE: KEY, Converter/ConvertToKpp.inl): converting constructors\n'
                '# and conversion operators about which no build of the game asks whether they are explicit, written without\n'
                '# the word as kpp is explicit by default (design.md "Explicit by default"; kovarex 2026-11-09). Written by implicit-dropped.cmd\n'
                '# (implicit-dropped.py) from kpp --implicit-uses of every build: Windows, Linux and FreeBSD, debug, release and\n'
                '# final, with and without the tests (final only without). A mark asked about by any build stays.\n')
        for d in dropped:
            f.write(d + '\n')
    kept = sum(1 for n in asked.values() if n != 0)
    print(f'implicit-dropped: {len(dropped)} keys dropped, {kept} kept, {unread} marks whose declaration was not read (kept)')


main()
