# convert.cmd's nullptr pass (kovarex, 2026-11-04: "writing 0 instead of nullptr is not nice"): reads
# kpp's --zero-null-warnings --diagnostic-columns output, one line per literal 0 that converts to a
# pointer or member pointer, `PATH:LINE:COL: warning: the literal 0 is no pointer: write nullptr
# (design.md 4)`, and writes nullptr for exactly that token in the converted file. The types are
# kpp's own (thread A's check), so `f(0)` choosing a pointer parameter is found as the compiler sees it.
#
#   python -I fix-null.py WARNINGS ROOT...
#
# Only files under a ROOT (the converted trees) are rewritten; a site anywhere else (the stdlib, a
# hand-written stub), or a token at LINE:COL that is no literal 0 (`0`, `0u`, `0L`, `0ull`...), fails
# the run: nothing is guessed. Prints the count of sites rewritten.
import os
import re
import sys

WARNING = re.compile(r'^(.*?):(\d+):(\d+): warning: the literal 0 is no pointer')
ZERO = re.compile(r'0[uUlL]*(?![0-9A-Za-z_.])')


def main():
    if len(sys.argv) < 3:
        print('usage: fix-null.py WARNINGS ROOT...')
        return 2
    roots = [os.path.normcase(os.path.abspath(r)) + os.sep for r in sys.argv[2:]]
    sites = {}
    with open(sys.argv[1], encoding='utf-8', errors='replace') as f:
        for line in f:
            m = WARNING.match(line.rstrip('\r\n'))
            if not m:
                continue
            path = os.path.normcase(os.path.abspath(m.group(1)))
            if not any(path.startswith(r) for r in roots):
                print('fix-null: %s:%s:%s is outside the converted trees: write nullptr there by hand' % (m.group(1), m.group(2), m.group(3)))
                return 1
            sites.setdefault(path, set()).add((int(m.group(2)), int(m.group(3))))
    count = 0
    for path, places in sorted(sites.items()):
        with open(path, 'rb') as f:
            lines = f.read().split(b'\n')
        for line, column in sorted(places, reverse=True):
            text = lines[line - 1].decode('utf-8')
            # (the column counts bytes, 1-based)
            raw = lines[line - 1]
            prefix = raw[:column - 1].decode('utf-8')
            at = len(prefix)
            m = ZERO.match(text, at)
            if not m or (at > 0 and (text[at - 1].isalnum() or text[at - 1] in '_.')):
                print('fix-null: %s:%d:%d: no literal 0 there: %s' % (path, line, column, text.strip()))
                return 1
            text = text[:at] + 'nullptr' + text[m.end():]
            lines[line - 1] = text.encode('utf-8')
            count += 1
        with open(path, 'wb') as f:
            f.write(b'\n'.join(lines))
    print('fix-null: %d literal 0s written nullptr in %d files' % (count, len(sites)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
