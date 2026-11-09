#!/usr/bin/env python3
"""The rewrite to std:: (docs/design.md 16): applies what kpp recorded.

    KPP_STD_USES=DIR kpp --no-server ...      each compile leaves a file of records in DIR
    python tools/std/rewrite.py [--dry] [--only PREFIX]... DIR
    KPP_NO_BRIDGE=1 kpp --no-server ... 2> ERRORS
    python tools/std/rewrite.py [--dry] --errors ERRORS

The second form is for the forms the first does not see (a C-style cast of a library type, the parser's cast candidates): each
"'X' is not declared; did you mean std::X?" of a compile without the bridge writes std:: before X on that line, every bare X
of it.

Only what the compiler says reached the standard library through the bridge is touched, by byte offset: a local `count`, a
member `data`, a program's own `Map` never are. Run it again and nothing changes: a name written std:: is no record.
Every compile of the tree counts (each configuration, each target): a false `when` is not walked.

Each record is `kind, path, offset, name`:
B    a bare name: std:: goes before it
G    `::name`: `::` becomes std::
S    a bare specialization (`template<> struct Hash<Key>`): std:: goes before its name, found from the declaration
M    a bare call of a function the global scope also declares, which took the library's: listed and left, for a reader (--calls
     writes std:: before them too: what the code does now)
U    the same call, decided per template instance: left alone, and listed (for a reader)

A record whose text is not there is listed and not applied. KPP_NO_BRIDGE=1 compiles without the bridge: what is left bare is an error.
"""
import collections
import glob
import os
import re
import sys


def main():
    arguments = sys.argv[1:]
    dry = '--dry' in arguments
    calls = '--calls' in arguments
    arguments = [a for a in arguments if a != '--dry' and a != '--calls']
    only = []
    while '--only' in arguments:
        at = arguments.index('--only')
        only.append(os.path.abspath(arguments[at + 1]))
        del arguments[at:at + 2]
    if '--errors' in arguments:
        at = arguments.index('--errors')
        from_errors(arguments[at + 1], dry)
        return
    if len(arguments) != 1:
        sys.exit(__doc__)
    records = collections.defaultdict(set)
    for path in glob.glob(os.path.join(arguments[0], '*.txt')):
        with open(path, encoding='utf-8') as f:
            for line in f:
                kind, source, offset, name = line.rstrip('\n').split('\t')
                parts = os.path.abspath(source).split(os.sep)
                if 'build' not in parts and (not only or any(os.path.abspath(source).startswith(prefix) for prefix in only)): # (build\: what kpp generates)
                    records[source].add((int(offset), kind, name))
    counts = collections.Counter()
    listed = []
    for source, uses in sorted(records.items()):
        with open(source, 'rb') as f:
            data = f.read()
        edits = {}
        for offset, kind, name in sorted(uses):
            word = name.encode()
            if kind == 'U':
                listed.append('decided per instance, left: %s:%d %s' % (source, line_of(data, offset), name))
                counts['U'] += 1
                continue
            at = offset
            if kind == 'S':
                at = data.find(word, offset, offset + 400)
                while at >= 0 and not is_word(data, at, word):
                    at = data.find(word, at + 1, offset + 400)
            if kind == 'G':
                if data[at:at + 2] != b'::' or not is_word(data, at + 2, word):
                    listed.append('not there: %s:%d ::%s' % (source, line_of(data, offset), name))
                    counts['missing'] += 1
                    continue
                edits[at] = (2, b'std::')
                counts['G'] += 1
                continue
            if at < 0 or not is_word(data, at, word):
                listed.append('not there: %s:%d %s' % (source, line_of(data, offset), name))
                counts['missing'] += 1
                continue
            if data[max(at - 2, 0):at] == b'::':
                counts['already'] += 1
                continue
            if kind == 'M':
                listed.append('took the library over a global of the name: %s:%d %s' % (source, line_of(data, at), name))
                if not calls:
                    counts['M left'] += 1
                    continue
            edits[at] = (0, b'std::')
            counts[kind] += 1
        if edits and not dry:
            for at in sorted(edits, reverse=True):
                length, text = edits[at]
                data = data[:at] + text + data[at + length:]
            with open(source, 'wb') as f:
                f.write(data)
    for line in listed:
        print(line)
    print('files', len(records), ' '.join('%s %d' % item for item in sorted(counts.items())), '(dry)' if dry else '')


def from_errors(path, dry):
    undeclared = re.compile(r"^(.+?):(\d+): error: (?:the type )?'(\w+)' is not declared")
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


def is_word(data, at, word):
    end = at + len(word)
    return data[at:end] == word and not (end < len(data) and (chr(data[end]).isalnum() or data[end] == 95))


def line_of(data, offset):
    return data.count(b'\n', 0, offset) + 1


main()
