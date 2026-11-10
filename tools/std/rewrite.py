#!/usr/bin/env python3
"""The rewrite to std:: (docs/design.md 16) of a tree written before the move, from a compile's errors.

    kpp --no-server ... 2> ERRORS
    python tools/std/rewrite.py [--dry] ERRORS

Each "'X' is not declared; did you mean std::X?" of the compile writes std:: before X on that line, every bare X of it.
Paths under build\\ and what kpp generates (`<...>`) are left. Compile again and repeat until nothing is left: every
configuration and target counts (a false `when` is not walked). Read the diff: a line that also uses a program's own X
of the name is rewritten too.
"""
import collections
import os
import re
import sys


def main():
    arguments = sys.argv[1:]
    dry = '--dry' in arguments
    arguments = [a for a in arguments if a != '--dry']
    if arguments and arguments[0] == '--errors': # (the old spelling)
        arguments = arguments[1:]
    if len(arguments) != 1:
        sys.exit(__doc__)
    from_errors(arguments[0], dry)


def from_errors(path, dry):
    undeclared = re.compile(r"^(.+?):(\d+): error: (?:the type )?'(\w+)' is not declared; did you mean std::")
    lines = collections.defaultdict(set)
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            found = undeclared.match(line)
            if found and not found.group(1).startswith('<') and os.path.exists(found.group(1)) and 'build' not in os.path.abspath(found.group(1)).split(os.sep):
                lines[found.group(1)].add((int(found.group(2)), found.group(3)))
    total = 0
    for source, uses in sorted(lines.items()):
        with open(source, 'rb') as f:
            text = f.read().split(b'\n')
        for number, name in sorted(uses):
            bare = re.compile(rb'(?<![\w:.>])' + name.encode() + rb'(?!\w)')
            text[number - 1], count = bare.subn(b'std::' + name.encode(), text[number - 1])
            if count == 0:
                print('not there: %s:%d %s' % (source, number, name))
            total += count
            print('%s:%d %s %d' % (source, number, name, count))
        if not dry:
            with open(source, 'wb') as f:
                f.write(b'\n'.join(text))
    print('from errors: files', len(lines), 'names', total, '(dry)' if dry else '')


main()
