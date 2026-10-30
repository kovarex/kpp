"""Runs the converted game's unit tests one suite per process: what each suite does in the test
binary tests.cmd builds (kpp\\tests-run\\factorio-test.exe), whatever stops it.

    python suites.py [EXE] [OUT] [--jobs N] [--timeout SECONDS] [--only Suite1,Suite2]

A stub (--stub-missing) or a failed assertion ends the process it is reached in, so a run of the
whole suite stops at the first one; one process per suite lets every suite say how far it gets.
Each of the suites of C:\\Factorio\\tests (their SUITE(Name) declarations) runs as
`EXE --config ... --write-data ... --no-colors --suite Name`, N at a time (12 by default), each in a
worker directory of its own under OUT (config.ini reading C:/Factorio/data, writes in its run\\),
for at most the timeout (600 s); --only runs just the suites it names. OUT (kpp\\tests-suites by default) gets each suite's output in
logs\\<suite>.txt and suites.tsv, one line per suite: its exit code, the runner's test and failure
counts when it got to its summary line, and the stub that stopped it. crashes.py then says what
stopped the others. docs/path-to-factorio.md, "The unit tests".
"""
import concurrent.futures, glob, os, queue, re, subprocess, sys, time

# The Factorio source tree (tests, data): C:\Factorio unless the environment's FACTORIO names another.
FACTORIO = os.environ.get('FACTORIO', r'C:\Factorio')

here = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:]]
jobs, timeout, only = 12, 600, None
if '--jobs' in args:
    i = args.index('--jobs')
    jobs = int(args[i + 1])
    del args[i:i + 2]
if '--timeout' in args:
    i = args.index('--timeout')
    timeout = int(args[i + 1])
    del args[i:i + 2]
if '--only' in args:
    i = args.index('--only')
    only = set(args[i + 1].split(','))
    del args[i:i + 2]
exe = os.path.abspath(args[0]) if len(args) > 0 else os.path.join(here, 'kpp', 'tests-run', 'factorio-test.exe')
out = os.path.abspath(args[1]) if len(args) > 1 else os.path.join(here, 'kpp', 'tests-suites')

suites = set()
for f in glob.glob(os.path.join(FACTORIO, 'tests', '**', '*.cpp'), recursive=True):
    text = open(f, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'^\s*SUITE\s*\(\s*(\w+)\s*\)', text, re.M):  # `SUITE (Movement)` too
        suites.add(m.group(1))
if only:
    unknown = only - suites
    if unknown:
        sys.exit('no such suite: ' + ', '.join(sorted(unknown)))
    suites = only
suites = sorted(suites)

workers = queue.Queue()
for w in range(jobs):
    wdir = os.path.join(out, 'w%d' % w)
    run = os.path.join(wdir, 'run')
    os.makedirs(run, exist_ok=True)
    with open(os.path.join(wdir, 'config.ini'), 'w') as cfg:
        cfg.write('[path]\nread-data=%s/data\nwrite-data=%s\n' % (FACTORIO.replace('\\', '/'), run.replace('\\', '/')))
    workers.put(wdir)
os.makedirs(os.path.join(out, 'logs'), exist_ok=True)


def one(suite):
    wdir = workers.get()
    try:
        run = os.path.join(wdir, 'run')
        command = [exe, '--config', os.path.join(wdir, 'config.ini'), '--write-data', run, '--no-colors', '--suite', suite]
        start = time.time()
        try:
            p = subprocess.run(command, cwd=run, capture_output=True, timeout=timeout)
            output = (p.stdout + p.stderr).decode('utf-8', errors='replace')
            code = p.returncode
        except subprocess.TimeoutExpired as e:
            output = ((e.stdout or b'') + (e.stderr or b'')).decode('utf-8', errors='replace')
            code = 'timeout'
        open(os.path.join(out, 'logs', suite + '.txt'), 'w', encoding='utf-8').write(output)
        total = failed = None
        m = re.search(r'Success: (\d+) tests passed', output)
        if m:
            total, failed = int(m.group(1)), 0
        m = re.search(r'FAILURE: (\d+) out of (\d+) tests failed', output)
        if m:
            failed, total = int(m.group(1)), int(m.group(2))
        m = re.search(r'kppc [^:\n]*: (.+?) at (\S+?):(\d+)', output)
        stub = m.group(1) if m else ''
        return suite, code, total, failed, stub, time.time() - start
    finally:
        workers.put(wdir)


results = []
with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
    for r in pool.map(one, suites):
        results.append(r)
        print('%-40s exit %-8s tests %-5s failed %-5s %s' % (r[0], r[1], r[2], r[3], r[4]), flush=True)
with open(os.path.join(out, 'suites.tsv'), 'w', encoding='utf-8') as f:
    f.write('suite\texit\ttests\tfailed\tstub\tseconds\n')
    for r in results:
        f.write('\t'.join('' if x is None else ('%.1f' % x if isinstance(x, float) else str(x)) for x in r) + '\n')
finished = [r for r in results if r[2] is not None]
print('%d suites: %d got to their summary line (%d tests, %d passed, %d failed); crashes.py %s says what stopped the other %d'
      % (len(results), len(finished), sum(r[2] for r in finished), sum(r[2] - r[3] for r in finished), sum(r[3] for r in finished),
         os.path.join(out, 'logs'), len(results) - len(finished)))
