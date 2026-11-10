"""Applies the fixes kpp's explicit-by-default errors name to the converted files (convert.cmd).

kpp's explicit by default (design.md "Explicit by default", kovarex 2026-11-07) refuses what C++ took
implicitly, and its errors end with the fix in design.md 23e's form: "write `NEW`, not `OLD`" or
"remove `OLD`", OLD being the source's text at the message's line and column. A braced function argument
gets the parameter's type as the compiler names it in kpp (`write `Vector{x, y}`, not `{x, y}``), which
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


def alignment_moves(data, placed):
    """The continuation lines the fixes move to keep their alignment, as the converter's AlignContinuations.inl moves
    them (kovarex 2026-11-10): a line whose first character stood at the column of a token on an earlier line of the
    same open parenthesis, bracket or braced list moves by what the fixes changed on that line before that token; a
    block's lines never move. placed: (start, end, new bytes), not overlapping. Returns more of them."""
    starts = [0] + [i + 1 for i, b in enumerate(data) if b == 10]
    tokens = [[] for _ in starts]
    group_at_start = [-1] * len(starts)
    groups, stack = [], []
    line = 0
    i, n = 0, len(data)
    first_of_line = True
    while i < n:
        while line + 1 < len(starts) and starts[line + 1] <= i:
            line += 1
            group_at_start[line] = stack[-1] if stack else -1
        c = data[i:i + 1]
        if c == b'\n':
            first_of_line = True
            i += 1
            continue
        if c in (b' ', b'\t', b'\r'):
            i += 1
            continue
        if first_of_line and c == b'#':
            while i < n and not (data[i] == 10 and data[i - 1] != 92):
                i += 1
            continue
        first_of_line = False
        if data.startswith(b'//', i) or data.startswith(b'/*', i):
            tokens[line].append((i, i - starts[line]))
            if data.startswith(b'//', i):
                e = data.find(b'\n', i)
                i = n if e < 0 else e
            else:
                e = data.find(b'*/', i + 2)
                i = n if e < 0 else e + 2
            continue
        tokens[line].append((i, i - starts[line]))
        if c in (b'"', b"'"):
            e = i + 1
            while e < n and data[e:e + 1] != c and data[e] != 10:
                e += 2 if data[e] == 92 else 1
            i = min(e + 1, n)
            continue
        if c.isalnum() or c == b'_':
            while i < n and (data[i:i + 1].isalnum() or data[i:i + 1] == b'_'):
                i += 1
            continue
        if c in (b'(', b'[', b'{'):
            block = False
            if c == b'{':
                p = i
                while p > starts[line] and data[p - 1:p] in (b' ', b'\t'):
                    p -= 1
                before = data[p - 1:p] if p > starts[line] else b''
                block = not (before in (b'=', b'(', b',', b'{', b'['))
            groups.append((block, line))
            stack.append(len(groups) - 1)
        elif c in (b')', b']', b'}') and stack:
            stack.pop()
        i += 1
    while line + 1 < len(starts):
        line += 1
        group_at_start[line] = stack[-1] if stack else -1
    ordered = sorted(placed)

    def delta(line_start, anchor):
        d = 0
        for start, end, new in ordered:
            if start >= line_start and (start < anchor or (start == anchor and end == start)):
                if end > anchor or b'\n' in new:
                    return None
                d += len(new) - (end - start)
            elif start < line_start < end:
                return None
        return d

    moved = [0] * len(starts)
    more = []
    for j in range(1, len(starts)):
        g = group_at_start[j]
        if g < 0 or groups[g][0] or not tokens[j]:
            continue
        first = tokens[j][0][0]
        column = first - starts[j]
        anchor = None
        for k in range(j - 1, groups[g][1] - 1, -1):
            hit = [t for t in tokens[k] if t[1] == column]
            if hit:
                anchor = (k, hit[0][0])
                break
        if anchor is None:
            continue
        k, at = anchor
        if tokens[k][0][0] == at and k != groups[g][1]:
            moved[j] = moved[k]
        else:
            d = delta(starts[k], at)
            if d is None:
                continue
            moved[j] = moved[k] + d
        if moved[j] == 0 or any(starts[j] <= s <= first or s < starts[j] < e for s, e, _ in ordered) or column + moved[j] < 0:
            moved[j] = 0
            continue
        if moved[j] > 0:
            more.append((starts[j], starts[j], b' ' * moved[j]))
        elif data[starts[j]:starts[j] - moved[j]] == b' ' * -moved[j]:
            more.append((starts[j], starts[j] - moved[j], b''))
        else:
            moved[j] = 0
    return more


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
    moved = 0
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
        moves = alignment_moves(data, placed)
        placed += moves
        placed.sort(reverse=True)
        for i in range(1, len(placed)):
            if placed[i][1] > placed[i - 1][0]:
                failed.append(f'{path}: two fixes overlap at byte {placed[i - 1][0]}')
        if any(f.startswith(path + ':') for f in failed):
            continue
        for start, end, text in placed:
            data = data[:start] + text + data[end:]
            applied += 1
        applied -= len(moves)
        moved += len(moves)
        open(path, 'wb').write(data)
    for f in failed:
        print('apply-fixes: ' + f)
    print(f'apply-fixes: {applied} fixes applied in {len(by_file)} files ({moved} continuation lines moved to keep their alignment), {len(unfixable)} explicit-by-default errors name no fix')
    for line in unfixable[:20]:
        print('  ' + line)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
