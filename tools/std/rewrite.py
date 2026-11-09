#!/usr/bin/env python3
"""The rewrite to std:: (docs/design.md 16): applies what kpp recorded.

    KPP_STD_USES=DIR kpp --no-server ...      each compile leaves a file of records in DIR
    python tools/std/rewrite.py [--dry] DIR

Only what the compiler says reached the standard library through the bridge is touched, by byte offset: a local `count`, a
member `data`, a program's own `Map` never are. Run it again and nothing changes: a name written std:: is no record.
Every compile of the tree counts (each configuration, each target): a false `when` is not walked.

Each record is `kind, path, offset, name`:
B    a bare name: std:: goes before it
G    `::name`: `::` becomes std::
S    a bare specialization (`template<> struct Hash<Key>`): std:: goes before its name, found from the declaration
M    a bare call of a function the global scope also declares, which took the library's: std:: goes before it, and it is listed
U    the same call, decided per template instance: left alone, and listed (for a reader)

A record whose text is not there is listed and not applied.
"""
import collections
import glob
import os
import sys


def main():
    arguments = sys.argv[1:]
    dry = '--dry' in arguments
    arguments = [a for a in arguments if a != '--dry']
    if len(arguments) != 1:
        sys.exit(__doc__)
    records = collections.defaultdict(set)
    for path in glob.glob(os.path.join(arguments[0], '*.txt')):
        with open(path, encoding='utf-8') as f:
            for line in f:
                kind, source, offset, name = line.rstrip('\n').split('\t')
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


def is_word(data, at, word):
    end = at + len(word)
    return data[at:end] == word and not (end < len(data) and (chr(data[end]).isalnum() or data[end] == 95))


def line_of(data, offset):
    return data.count(b'\n', 0, offset) + 1


main()
