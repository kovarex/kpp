"""Keeps the unused names kpp's --unused-checks reports, as kovarex decided (2026-11-11: "just keep the (void)name; or
[[maybe_unused]] for now"): each `PATH:LINE:COLUMN: error: the local X is never used...`, `...the local X is assigned but
never read...` or `...the parameter X of F is never used...` line of a compile's output (kpp --diagnostic-columns) gets
`[[maybe_unused]] ` at its line and column, the declaration's first token, where X is declared after it on that line.
A lambda's capture nothing uses is listed, not written: remove it from the captures by hand.

    kpp --unused-checks --diagnostic-columns --check FILES... 2> errors.txt
    python tools/unused/fix.py ROOT errors.txt       (ROOT: only files below it are written)

Prints how many were applied, left (outside ROOT, a capture) and mismatched (the name not after the column: nothing
written there). A file is written byte for byte as it was apart from the insertions: kpp's column counts bytes from
the line's start, and a converted C library's file may be in another encoding than UTF-8 (minizip's, Latin-1).
"""
import os, re, sys

LINE = re.compile(r'^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+): error: (?P<msg>.*)$')
KEEP = re.compile(r'^the (?:local (?P<local>\S+) is (?:never used|assigned but never read)|parameter (?P<param>\S+) of .* is never used): ')
CAPTURE = re.compile(r'^the lambda captures (?P<name>\S+) and never uses it')


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    root = os.path.normcase(os.path.abspath(sys.argv[1]))
    edits = {}
    left = []
    for name in sys.argv[2:]:
        for raw in open(name, encoding='utf-8', errors='replace'):
            m = LINE.match(raw.rstrip('\r\n'))
            if not m or m['path'].startswith('<'):
                continue
            k = KEEP.match(m['msg'])
            if not k:
                if CAPTURE.match(m['msg']):
                    left.append(raw.rstrip('\r\n'))
                continue
            path = os.path.normcase(os.path.abspath(m['path']))
            if not path.startswith(root + os.sep):
                left.append(raw.rstrip('\r\n'))
                continue
            edits.setdefault(path, set()).add((int(m['line']), int(m['col']), k['local'] or k['param']))
    applied = mismatched = 0
    for path, wanted in sorted(edits.items()):
        # (lines as the lexer counts them, each \n; a \r stays at its line's end)
        lines = open(path, 'rb').read().split(b'\n')
        # (the last first: an insertion moves what follows it on its line)
        for line, col, name in sorted(wanted, reverse=True):
            text = lines[line - 1] if line - 1 < len(lines) else b''
            at = col - 1
            if at > len(text) or not re.search(rb'\b' + re.escape(name.encode('utf-8')) + rb'\b', text[at:]) or text[at:].startswith(b'[[maybe_unused]]'):
                mismatched += 1
                print('mismatched: %s:%d:%d %s' % (path, line, col, name))
                continue
            lines[line - 1] = text[:at] + b'[[maybe_unused]] ' + text[at:]
            applied += 1
        open(path, 'wb').write(b'\n'.join(lines))
    for l in left:
        print('left: ' + l)
    print('unused: %d applied, %d left, %d mismatched' % (applied, len(left), mismatched))
    return 1 if mismatched else 0


if __name__ == '__main__':
    sys.exit(main())
