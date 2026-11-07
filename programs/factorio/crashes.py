"""What stopped each suite that suites.py ran without getting to its summary line, tallied: the
failed assertion (the converted file, line and text), the stub (--stub-missing: a function the code
generator left out), or where the crash happened: the function that faulted and its caller (the
frames after the exception's dispatch; the game's handler turns an access violation into abort()),
the function that threw an exception nobody caught, or the function that called abort().

    python crashes.py [LOGS]

LOGS is suites.py's logs directory (kpp\\tests-suites\\logs by default). The groups come most
frequent first, each with one of its suites: the first lines of the list are what to fix first.
A faulting function can be one the linker folded with identical ones (`ID<A>::operator bool()` for
any ID), which is why its caller is named too.
"""
import collections, glob, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
logs = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, 'kpp', 'tests-suites', 'logs')
# The crash report's own frames and those of the exception's dispatch, above the frame that crashed.
machinery = re.compile(r'(raise|abort|handleSignal|kpp_|_seh_filter|`__scrt|__C_specific_handler|_chkstk|Rtl|KiUser|'
                       r'_CxxThrowException|RaiseException|_CxxFrameHandler|__)')


def crashed(report):
    """The crash report's frames (`  10  module!function+0x32`), read from the frame that crashed."""
    frames = [m.group(1) for m in re.finditer(r'^\s*\d+\s+(?:\S+?!)?([^\n]*?)(?:\+0x[0-9a-f]+)?$', report, re.M)]
    for anchor, kind in (('KiUserExceptionDispatcher', 'fault'), ('_CxxThrowException', 'uncaught throw'), ('abort', 'abort')):
        at = next((i for i, frame in enumerate(frames) if frame.startswith(anchor)), None)
        if at is None:
            continue
        own = [frame for frame in frames[at + 1:] if not machinery.match(frame)]
        if not own:
            return kind + ' (no frames of the program)'
        if own[0] == '?' and len(own) > 1:
            return '%s calling a bad address from %s' % (kind, own[1][:90])
        return '%s in %s%s' % (kind, own[0][:90], ', from ' + own[1][:70] if kind == 'fault' and len(own) > 1 else '')
    return 'crash (no frames)'


causes = collections.Counter()
examples = {}
for f in sorted(glob.glob(os.path.join(logs, '*.txt'))):
    text = open(f, encoding='utf-8', errors='replace').read()
    if re.search(r'Success: \d+ tests passed', text) or re.search(r'FAILURE: \d+ out of', text):
        continue
    cause = None
    m = re.search(r'([\w.]+\.kpp):(\d+):\d+: assertion failed: ([^\n]*)', text)
    if m:
        cause = 'assertion %s:%s %s' % (m.group(1), m.group(2), m.group(3)[:70])
    if not cause:
        m = re.search(r'kpp [^:\n]*: (.+?) at (\S+?):(\d+)', text)
        if m:
            cause = 'stub ' + m.group(1)
    if not cause:
        m = re.search(r'kpp crash report: [^\n]*', text)
        if m:
            cause = crashed(text[m.end():])
    if not cause:
        cause = 'no summary line (a timeout, or an exit without one)'
    causes[cause] += 1
    examples.setdefault(cause, os.path.basename(f)[:-4])
for cause, n in causes.most_common():
    print('%4d  %s   (e.g. %s)' % (n, cause, examples[cause]))
