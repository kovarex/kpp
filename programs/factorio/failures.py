"""The failed checks of the suites that suites.py ran to their summary line, one per line: suite,
test, the file and line of the check, its message; then how many share each message (numbers
masked), most frequent first.

    python failures.py [LOGS] [OUT]

LOGS is suites.py's logs directory (kpp\\tests-suites\\logs by default); OUT, when given, gets the
failed checks as tab-separated lines. docs/path-to-factorio.md, "The failing tests".
"""
import collections, glob, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
logs = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, 'kpp', 'tests-suites', 'logs')
out = sys.argv[2] if len(sys.argv) > 2 else None
rows = []
for f in sorted(glob.glob(os.path.join(logs, '*.txt'))):
    text = open(f, encoding='utf-8', errors='replace').read()
    if not re.search(r'FAILURE: \d+ out of', text):
        continue
    suite = os.path.basename(f)[:-4]
    test = None
    # The runner's list at the end: `[Suite/Test]`, then ` File.cpp:line: message` per failed check.
    for line in text[text.find('Failed tests:'):].split('\n')[1:]:
        m = re.match(r'^\[([^/\]]+)/([^\]]+)\]\s*$', line)
        if m:
            test = m.group(2)
            continue
        m = re.match(r'^\s+(\S+?):(\d+): (.*)$', line)
        if m and test:
            rows.append((suite, test, os.path.basename(m.group(1).replace('\\', '/')) + ':' + m.group(2), m.group(3)))
if out:
    with open(out, 'w', encoding='utf-8') as w:
        for r in rows:
            w.write('\t'.join(r) + '\n')
print('%d failed checks in %d tests of %d suites' % (len(rows), len({(r[0], r[1]) for r in rows}), len({r[0] for r in rows})))
for message, n in collections.Counter(re.sub(r'\d+', 'N', r[3])[:100] for r in rows).most_common():
    print('%4d  %s' % (n, message.encode('ascii', 'replace').decode()))
