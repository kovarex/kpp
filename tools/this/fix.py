"""Applies kpp's fix clauses to the files they name: each `PATH:LINE:COLUMN: warning|error: ... write `NEW`, not `OLD``
line of a compile's output (kpp --diagnostic-columns), only where OLD is exactly at that line and column; a fix
overlapping another of the same file is left (run again after the next compile). Used for `this` as a reference
(design.md 23c, 23d) on code written with C++'s meaning:

    kpp --this-pointer --this-warnings --this-reference-warnings --diagnostic-columns --check FILES... 2> w.txt
    python tools/this/fix.py ROOT w.txt           (ROOT: only files below it are written)

then compile without --this-pointer: an error with a fix clause left (in a `false` branch of another configuration, a
requires-expression) goes the same way, its output given to fix.py. Prints how many were applied and mismatched.
"""
import os, re, sys

LINE = re.compile(r'^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+): (?:warning|error): (?P<msg>.*)$')
FIX = re.compile(r'write `(?P<new>.*?)`, not `(?P<old>.*?)`(?: \(design\.md [^)]*\))?$')

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    root = os.path.normcase(os.path.abspath(sys.argv[1]))
    fixes = {}
    for name in sys.argv[2:]:
        for raw in open(name, encoding='utf-8', errors='replace'):
            m = LINE.match(raw.rstrip('\r\n'))
            f = FIX.search(m['msg']) if m else None
            if not f or m['path'].startswith('<'):
                continue
            path = os.path.normcase(os.path.abspath(m['path']))
            if path.startswith(root + os.sep):
                fixes.setdefault(path, set()).add((int(m['line']), int(m['col']), f['old'], f['new']))
    applied = bad = 0
    for path, wanted in fixes.items():
        data = open(path, 'rb').read().decode('utf-8')
        starts = [0]
        for l in data.split('\n')[:-1]:
            starts.append(starts[-1] + len(l) + 1)
        edits = []
        for line, col, old, new in wanted:
            at = starts[line - 1] + col - 1 if line - 1 < len(starts) else -1
            if at >= 0 and data[at:at + len(old)] == old:
                edits.append((at, old, new))
            else:
                bad += 1
                print('mismatched', path, line, col, repr(old))
        last = None
        for at, old, new in sorted(edits, reverse=True):
            if last is not None and at + len(old) > last:
                bad += 1
                continue
            data = data[:at] + new + data[at + len(old):]
            last = at
            applied += 1
        open(path, 'wb').write(data.encode('utf-8'))
    print('applied', applied, 'mismatched', bad, 'files', len(fixes))
    return 0

if __name__ == '__main__':
    sys.exit(main())
