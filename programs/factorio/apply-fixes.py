"""Applies the fixes kpp's explicit-by-default errors name to the converted files (convert.cmd).

kpp's explicit by default (design.md "Explicit by default", kovarex 2026-11-07) refuses what C++ took
implicitly, and its errors end with the fix in design.md 23e's form: "write `NEW`, not `OLD`" or
"remove `OLD`", OLD being the source's text at the message's line and column. A braced function argument
gets the parameter's type as the compiler names it in kpp (`write `Vector2D{x, y}`, not `{x, y}``), which
the converter, reading C++, cannot spell itself: so convert.cmd checks the converted game with
--diagnostic-columns in each configuration it builds and applies the fixes here.

    python apply-fixes.py ERRORS.txt [ERRORS.txt ...]
    python apply-fixes.py --this WARNINGS.txt ...   required `this.`'s fixes, from --this-warnings

Only the explicit-by-default errors' fixes are applied, each only where OLD is exactly at its line and
column; one whose text is not there is an error (exit 1), never a guess. Prints how many it applied.
Only the conversion is written: a file of this repository outside programs/factorio/kpp (the stdlib,
whose `explicit` words the flag reports) is left as it is, and its errors are listed as not applied.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPOSITORY = os.path.dirname(os.path.dirname(HERE))
CONVERSION = os.path.join(HERE, 'kpp')


def converted(path):
    """Whether apply-fixes may write the file: the conversion (kpp/, a checkout), not the repository's own."""
    full = os.path.normcase(os.path.abspath(path))
    inside = lambda root: full.startswith(os.path.normcase(root) + os.sep)
    return inside(CONVERSION) or not inside(REPOSITORY)

LOCATION = re.compile(r'^(?P<path>.+?):(?P<line>\d+):(?P<column>\d+): error: (?P<message>.*)$')
# (--this: required `this.`'s warnings, design.md 23c: a bare member's fix, `this.x` or `This::x`, nothing else)
THIS_LOCATION = re.compile(r'^(?P<path>.+?):(?P<line>\d+):(?P<column>\d+): warning: (?P<message>.* is a (data member|member function|static member).*)$')
THIS_MODE = False
TAIL = ' (design.md "Explicit by default")'
WRITE = re.compile(r'write `(?P<new>.*)`, not `(?P<old>.*)`$')
REMOVE = re.compile(r'remove `(?P<old>.*)`$')


def fixes_of(paths):
    found = {}
    unfixable = []
    conflicts = set()
    for path in paths:
        for line in open(path, encoding='utf-8', errors='replace').read().splitlines():
            m = (THIS_LOCATION if THIS_MODE else LOCATION).match(line)
            # (a template's instance adds which one it is: ` (in T = ScriptPosition)`)
            message = re.sub(r' \(in [^()]*(\([^()]*\)[^()]*)*\)$', '', m.group('message')) if m else ''
            if not m or not (THIS_MODE or message.endswith(TAIL)):
                continue
            if not THIS_MODE:
                message = message[:-len(TAIL)]
            w = WRITE.search(message)
            r = REMOVE.search(message) if not w else None
            if not w and not r:
                unfixable.append(line)
                continue
            if THIS_MODE and w and w.group('old').startswith('__'):
                continue  # (what the parser wrote for a union enum: `return __tag;`, at the enum's line)
            key = (m.group('path'), int(m.group('line')), int(m.group('column')))
            fix = (w.group('old'), w.group('new')) if w else (r.group('old'), '')
            if key in found and found[key] != fix:
                # (two configurations may name one type from two scopes: `Arm{...}` and
                # `AgriculturalCraneState::Arm{...}`; the qualified one is right in both)
                (old_a, new_a), (old_b, new_b) = found[key], fix
                if old_a == old_b and new_b.endswith(new_a) and new_b[:-len(new_a)].endswith('::'):
                    found[key] = fix
                elif not (old_a == old_b and new_a.endswith(new_b) and new_a[:-len(new_b)].endswith('::')):
                    # (a template's instances that need two types: no one fix, the error stays)
                    conflicts.add(key)
                continue
            found[key] = fix
    for key in conflicts:
        unfixable.append(f'{key[0]}:{key[1]}:{key[2]}: two types for one list (a template\'s instances): {found.pop(key)}')
    return found, unfixable


def main():
    global THIS_MODE
    arguments = sys.argv[1:]
    if arguments and arguments[0] == '--this':
        THIS_MODE = True
        arguments = arguments[1:]
    found, unfixable = fixes_of(arguments)
    by_file = {}
    for (path, line, column), fix in found.items():
        by_file.setdefault(path, []).append((line, column, fix))
    applied = 0
    failed = []
    for path in [p for p in by_file if not converted(p)]:
        for line, column, (old, new) in by_file.pop(path):
            unfixable.append(f'{path}:{line}:{column}: outside the conversion, not applied: ' + (f'write `{new}`, not `{old}`' if new else f'remove `{old}`'))
    for path, edits in sorted(by_file.items()):
        data = open(path, 'rb').read()
        starts = [0]
        for i, b in enumerate(data):
            if b == 10:
                starts.append(i + 1)
        placed = []
        for line, column, (old, new) in edits:
            if line > len(starts):
                failed.append(f'{path}:{line}:{column}: no such line')
                continue
            offset = starts[line - 1] + column - 1
            old_bytes, new_bytes = old.encode('utf-8'), new.encode('utf-8')
            if data[offset:offset + len(old_bytes)] != old_bytes:
                failed.append(f'{path}:{line}:{column}: `{old}` is not there')
                continue
            if not new_bytes:
                # (a removed word takes the space after it)
                end = offset + len(old_bytes)
                while end < len(data) and data[end:end + 1] == b' ':
                    end += 1
                placed.append((offset, end, b''))
            else:
                placed.append((offset, offset + len(old_bytes), new_bytes))
        placed.sort(reverse=True)
        for i in range(1, len(placed)):
            if placed[i][1] > placed[i - 1][0]:
                failed.append(f'{path}: two fixes overlap at byte {placed[i - 1][0]}')
        if any(f.startswith(path + ':') for f in failed):
            continue
        for start, end, text in placed:
            data = data[:start] + text + data[end:]
            applied += 1
        open(path, 'wb').write(data)
    for f in failed:
        print('apply-fixes: ' + f)
    print(f'apply-fixes: {applied} fixes applied in {len(by_file)} files, {len(unfixable)} explicit-by-default errors name no fix')
    for line in unfixable[:20]:
        print('  ' + line)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
