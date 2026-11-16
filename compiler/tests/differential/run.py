#!/usr/bin/env python3
"""The differential tests: programs and generated programs built by kpp and by clang++ (the MSVC
target, MSVC's STL), run, and their outputs compared. A difference is a failure: kpp gives another
answer than C++ for the same program. README.md has the layout and how to add a program.

    python -I compiler\\tests\\differential\\run.py [--release] [--jobs N] [--filter TEXT]
        [--programs-only | --generators-only] [--seeds N] [--first-seed N] [--keep] [--shard I/N]

--release builds both optimized (kpp --release, clang -O2). --seeds N runs each generator's first N
seeds (its own count by default), from --first-seed (1): a seed beyond the fixed ones is a search for
new bugs, not a check. The last line says ALL PASSED or how many failed; the exit code is 0 only then.
"""
import argparse, concurrent.futures, hashlib, os, re, shutil, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
KPP = os.environ.get('KPP') or os.path.join(ROOT, 'compiler', 'build', 'kpp.exe')
TIMEOUT = 120

# Each generator: its script, the kind of program it writes, its arguments after the seed and the seeds
# run by default. 'same': one source for both, the C++ side after generators/cxx-head.h; 'conv': marks
# that become C++'s or kpp's keywords; 'abi': a C file and a kpp file linked together, self-checking
# (prints "done"); 'sse': the intrinsics with each side's header.
GENERATORS = [
    ('arith-int', 'gen.py', 'same', ['300', 'int'], 4),
    ('arith-float', 'gen.py', 'same', ['300', 'float'], 4),
    ('arith-mixed', 'gen.py', 'same', ['300', 'mixed'], 4),
    ('abi', 'genabi.py', 'abi', ['60'], 6),
    ('const', 'genconst.py', 'same', [], 8),
    ('conv', 'genconv.py', 'conv', [], 8),
    ('exc', 'genexc.py', 'same', [], 8),
    ('exc2', 'genexc2.py', 'same', [], 8),
    ('expr', 'genexpr.py', 'conv', [], 8),
    ('inh', 'geninh.py', 'same', [], 8),
    ('init', 'geninit.py', 'same', [], 8),
    ('lam', 'genlam.py', 'same', [], 8),
    ('life', 'genlife.py', 'same', ['20'], 8),
    ('ov', 'genov.py', 'same', [], 8),
    ('pack', 'genpack.py', 'same', [], 8),
    ('sse', 'gensse.py', 'sse', [], 1),
    ('tpl', 'gentpl.py', 'same', [], 8),
    ('vb', 'genvb.py', 'same', [], 8),
]

def findCxx():
    if os.environ.get('KPP_DIFF_CXX'):
        return os.environ['KPP_DIFF_CXX']
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                           'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    out = subprocess.run([vswhere, '-all', '-products', '*', '-find', r'VC\Tools\Llvm\x64\bin\clang++.exe'],
                         capture_output=True, text=True).stdout.split('\n')
    # every installation's (-latest is VS 18's Build Tools here, which has none); VS 2022's first
    found = sorted((line.strip() for line in out if line.strip()), key=lambda p: '\\2022\\' not in p)
    if not found:
        sys.exit('differential: no clang++ (VS 2022\'s "C++ Clang tools" component, or KPP_DIFF_CXX)')
    return found[0]

def findCxx2():
    """The second reference: llvm-mingw's clang++ (the windows-gnu target: Itanium's ABI, libc++), from
    KPP_DIFF_CXX2 or tools/sysroot/llvm-mingw.ps1's build/llvm-mingw; none, and only clang's MSVC target counts."""
    if os.environ.get('KPP_DIFF_CXX2'):
        return os.environ['KPP_DIFF_CXX2']
    root = os.path.join(ROOT, 'build', 'llvm-mingw')
    for d in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        exe = os.path.join(root, d, 'bin', 'clang++.exe')
        if os.path.exists(exe):
            return exe
    return None

def flagsOf(text, tag):
    """`// kpp: FLAGS` and `// cxx: FLAGS` lines among a program's first 20 lines."""
    flags = []
    for line in text.split('\n')[:20]:
        m = re.match(r'\s*//\s*' + tag + r':\s*(.*)$', line)
        if m:
            flags += m.group(1).split()
    return flags

STD_NAMES = open(os.path.join(HERE, 'std-names.kpp'), encoding='utf-8').read()

def outsideStrings(line, pattern, replacement):
    """re.sub on the parts of a line outside its string and character literals."""
    parts = re.split(r'("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')', line)
    return ''.join(p if i % 2 else re.sub(pattern, replacement, p) for i, p in enumerate(parts))

def markers(text):
    """The generators' marks of a switch: KBREAK a case's last break, KFALLTHROUGH a case running on."""
    return text.replace('KBREAK;', 'break;').replace('KFALLTHROUGH;', '[[fallthrough]];')

def toKpp(text):
    """A program's kpp side, compiled as a .cpp file: kpp reads C++ text with C++'s switch (a case runs
    on only by [[fallthrough]], design.md "No implicit fallthrough"), the rest kpp's own rules. So the
    C++ spellings kpp refuses are rewritten (docs/differences-from-cpp.md), and what no rule can tell is
    marked: KEXPLICIT is C++'s explicit (kpp's default), IMPLICIT kpp's `implicit` (nothing in C++)."""
    out = []
    for line in markers(text).split('\n'):
        line = line.replace('KEXPLICIT ', '').replace('IMPLICIT ', 'implicit ')
        line = outsideStrings(line, r'\bexplicit\s+(?=[A-Za-z_~])', '')
        out.append(line)
    return STD_NAMES + '\n'.join(out)

def toCxx(text):
    return markers(text).replace('KEXPLICIT ', 'explicit ').replace('IMPLICIT ', '')

def readExpected(base, config):
    """NAME.expected (NAME.release.expected for --release when there is one): the output a program must
    give where C++'s is not the answer. Its `#` lines say why (an unspecified order kpp fixes, a
    deliberate difference, clang's own mistake); the rest is the output, its last line `exit N`."""
    for path in (base + '.' + config + '.expected', base + '.expected'):
        if os.path.exists(path):
            lines = open(path, encoding='utf-8').read().split('\n')
            return '\n'.join(line for line in lines if not line.startswith('#'))
    return None

def readPending():
    """pending.txt: `NAME[ CONFIG]: why`, NAME a program (group/name) or a generator's seed
    (gen/NAME/SEED); CONFIG debug or release when only that one differs."""
    pending = {}
    path = os.path.join(HERE, 'pending.txt')
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, _, why = line.partition(':')
        parts = key.split()
        pending[(parts[0], parts[1] if len(parts) > 1 else None)] = why.strip()
    return pending

def run(cmd, cwd, timeout=TIMEOUT, env=None, stdoutOnly=False):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, timeout=timeout, env=env)
        out = (p.stdout if stdoutOnly else p.stdout + p.stderr).decode('utf-8', 'replace').replace('\r\n', '\n')
        return p.returncode, out
    except subprocess.TimeoutExpired:
        return 'timeout', ''

def generate(cmd, cwd, env):
    """A generator's program: its standard output (its standard error is its own notes)."""
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, env=env)
    out = p.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
    return p.returncode, out if p.returncode == 0 else p.stderr.decode('utf-8', 'replace')

def tidy(work, name):
    """kpp leaves its modules' .ll, .o and .hash files beside the executable."""
    for f in os.listdir(work):
        if f.startswith(name + '-k.exe.') or (f.startswith(name + '-k') and f.endswith(('.ilk', '.pdb'))):
            p = os.path.join(work, f)
            shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)

class Runner:
    def __init__(self, args):
        self.args = args
        self.cxx = findCxx()
        self.config = 'release' if args.release else 'debug'
        self.build = os.path.join(ROOT, 'compiler', 'build', 'differential', self.config)
        self.cache = os.path.join(ROOT, 'compiler', 'build', 'differential', 'cxx-cache')
        os.makedirs(self.cache, exist_ok=True)
        self.lock = threading.Lock()
        self.cxx2 = findCxx2()
        self.versions = {c: subprocess.run([c, '--version'], capture_output=True, text=True).stdout
                         for c in (self.cxx, self.cxx2) if c}
        self.second = 0

    def cxxOutput(self, work, name, cppText, cflags, extraObjs=(), cxx=None):
        """The C++ build's output, cached by its source, flags and compiler (its output never changes)."""
        cxx = cxx or self.cxx
        key = hashlib.sha256((cppText + '\0' + ' '.join(cflags) + '\0' + self.versions[cxx]).encode()).hexdigest()
        cached = os.path.join(self.cache, key + '.out')
        if os.path.exists(cached) and not extraObjs:
            return open(cached, encoding='utf-8').read(), None
        cpp = os.path.join(work, name + '.cpp')
        open(cpp, 'w', encoding='utf-8', newline='\n').write(cppText)
        exe = os.path.join(work, name + '-c.exe')
        code, log = run([cxx, '-std=c++20', '-w', '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH'] + cflags
                        + (['-O2'] if self.args.release else [])
                        + (['-static'] if cxx == self.cxx2 else [])  # libc++'s DLL is not on PATH
                        + [cpp, '-o', exe], work)
        if code != 0:
            return None, 'C++ build failed: ' + log.strip()[:600]
        code, out = run([exe], work, stdoutOnly=True)  # a terminate's message is kpp's own, on stderr
        out = out + 'exit %s\n' % code
        if not extraObjs:
            open(cached, 'w', encoding='utf-8', newline='\n').write(out)
        if not self.args.keep:
            for f in (cpp, exe):
                if os.path.exists(f): os.remove(f)
        return out, None

    def kppOutput(self, work, name, kppText, kflags, libs=()):
        src = os.path.join(work, name + '-k.cpp')
        open(src, 'w', encoding='utf-8', newline='\n').write(kppText)
        exe = os.path.join(work, name + '-k.exe')
        # The programs are C++'s text: `this` is C++'s pointer, a bare name a member (design.md 23c, 23d).
        cmd = [KPP, '--this-pointer'] + (['--release'] if self.args.release else []) + kflags
        for lib in libs:
            cmd += ['--lib', lib]
        code, log = run(cmd + ['--exe', exe, src], work, timeout=300)
        if code != 0:
            tidy(work, name)
            errors = [l for l in log.split('\n') if 'error' in l.lower() and not l.startswith('errors:')]
            return None, 'kpp build failed (%s): %s' % (code, '\n  '.join(errors[:int(os.environ.get('KPP_DIFF_ERRORS', '6'))] or log.strip().split('\n')[-6:]))
        code, out = run([exe], work, stdoutOnly=True)  # a terminate's message is kpp's own, on stderr
        tidy(work, name)
        if not self.args.keep:
            for f in (src, exe):
                if os.path.exists(f): os.remove(f)
        return out + 'exit %s\n' % code, None

    def compare(self, work, name, cpp, kpp, kflags, cflags, recorded=None):
        if recorded is not None:
            expected = recorded
        else:
            expected, err = self.cxxOutput(work, name, cpp, cflags)
            if err: return err
        actual, err = self.kppOutput(work, name, kpp, kflags)
        if err: return err
        if expected == actual:
            return None
        if recorded is None and self.cxx2:
            # Another implementation's answer is as good: what C++ leaves unspecified (argument order:
            # MSVC's ABI evaluates right to left, the other targets and kpp left to right) or to the
            # implementation (layout, the library's growth) may differ between the two.
            # Each line one of the two's: a program may meet both kinds of difference, on different lines.
            second, err = self.cxxOutput(work, name, cpp, cflags, cxx=self.cxx2)
            e, s, a = expected.split('\n'), (second or '').split('\n'), actual.split('\n')
            if second is not None and len(e) == len(s) == len(a) and all(x in (y, z) for x, y, z in zip(a, e, s)):
                with self.lock:
                    self.second += 1
                return None
        # both outputs beside the work, for a diff
        open(os.path.join(work, name + '.expected.out'), 'w', encoding='utf-8', newline='\n').write(expected)
        open(os.path.join(work, name + '.kpp.out'), 'w', encoding='utf-8', newline='\n').write(actual)
        e, a = expected.split('\n'), actual.split('\n')
        i = next((i for i in range(min(len(e), len(a))) if e[i] != a[i]), min(len(e), len(a)))
        return 'DIFFERENT at line %d of %d: C++ "%s", kpp "%s"' % (i + 1, len(e), (e[i] if i < len(e) else '')[:200],
                                                                    (a[i] if i < len(a) else '')[:200])

    def program(self, group, name):
        d = os.path.join(HERE, 'programs', group)
        if os.path.exists(os.path.join(d, name + '.body')):
            body = open(os.path.join(d, name + '.body'), encoding='utf-8').read()
            cpp = open(os.path.join(d, 'prelude.h'), encoding='utf-8').read() + body
            kpp = open(os.path.join(d, 'prelude.kpp'), encoding='utf-8').read() + body
        else:
            cpp = open(os.path.join(d, name + '.cxx'), encoding='utf-8').read()
            kpp = open(os.path.join(d, name + '.kpps'), encoding='utf-8').read()
        work = os.path.join(self.build, 'programs', group)
        os.makedirs(work, exist_ok=True)
        return self.compare(work, name, toCxx(cpp), toKpp(kpp), flagsOf(kpp, 'kpp'), flagsOf(cpp, 'cxx'),
                            readExpected(os.path.join(d, name), self.config))

    def generated(self, label, script, kind, gargs, seed):
        gdir = os.path.join(HERE, 'generators')
        work = os.path.join(self.build, 'generators', label)
        os.makedirs(work, exist_ok=True)
        name = '%s-%d' % (label, seed)
        env = dict(os.environ, KPP_DIFF_CXX=self.cxx, KPP_DIFF_TMP=work)
        py = [sys.executable, '-I', os.path.join(gdir, script), str(seed)]
        head = open(os.path.join(gdir, 'cxx-head.h'), encoding='utf-8').read()
        if kind == 'abi':
            prefix = os.path.join(work, name)
            code, log = run(py + gargs + [prefix], work, env=env)
            if code != 0: return 'generator failed: ' + log[:400]
            obj = prefix + '.obj'
            clang = self.cxx.replace('clang++.exe', 'clang.exe')
            code, log = run([clang, '-w', '-c', '-O1', prefix + '.c', '-o', obj], work)
            if code != 0: return 'C build failed: ' + log[:400]
            out, err = self.kppOutput(work, name, open(prefix + '.kpp', encoding='utf-8').read(), [], [obj])
            if not self.args.keep:
                for f in (prefix + '.c', obj): os.remove(f)
            if err: return err
            return None if out.strip() == 'done\nexit 0' else 'self-check failed: ' + out[:400]
        if kind == 'sse':
            skip = os.path.join(gdir, 'sse.skip')
            code, kpp = generate(py + ['intrin.h', skip], work, env)
            code2, body = generate(py + ['cxx', skip], work, env)
            if code != 0 or code2 != 0: return 'generator failed'
            kpp = kpp.replace('\n', '\nextern "C" void* memcpy(void* d, const void* s, unsigned long long n);\n', 1)
            cpp = ('#include <initializer_list>\n#include <intrin.h>\n#include <immintrin.h>\n#include <string.h>\n'
                   '#include <stdio.h>\n') + body
            return self.compare(work, name, cpp, toKpp(kpp), [], ['-msse2'])
        code, src = generate(py + gargs, work, env)
        if code != 0: return 'generator failed: ' + src[:400]
        cflags = ['-fbracket-depth=2000'] if script == 'gen.py' else []
        return self.compare(work, name, head + toCxx(src), toKpp(src), [], cflags)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--release', action='store_true')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--filter', default='')
    ap.add_argument('--programs-only', action='store_true')
    ap.add_argument('--generators-only', action='store_true')
    ap.add_argument('--seeds', type=int, default=0)
    ap.add_argument('--first-seed', type=int, default=1)
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--shard', default='')
    args = ap.parse_args()
    r = Runner(args)
    pending = readPending()
    tasks = []
    if not args.generators_only:
        for group in sorted(os.listdir(os.path.join(HERE, 'programs'))):
            d = os.path.join(HERE, 'programs', group)
            for f in sorted(os.listdir(d)):
                base, ext = os.path.splitext(f)
                if ext in ('.body', '.cxx'):
                    tasks.append(('%s/%s' % (group, base), lambda g=group, b=base: r.program(g, b)))
    if not args.programs_only:
        for label, script, kind, gargs, seeds in GENERATORS:
            n = args.seeds or seeds
            for seed in range(args.first_seed, args.first_seed + n):
                tasks.append(('gen/%s/%d' % (label, seed),
                              lambda l=label, s=script, k=kind, g=gargs, sd=seed: r.generated(l, s, k, g, sd)))
    tasks = [t for t in tasks if args.filter in t[0]]
    if args.shard:
        # --shard I/N: every Nth test from the Ith, so each shard gets programs and generators alike
        i, n = (int(x) for x in args.shard.split('/'))
        tasks = tasks[i - 1::n]
    start = time.time()
    failed, fixed, pend, passed = [], [], 0, 0
    def one(task):
        try:
            return task[0], task[1]()
        except Exception as e:
            return task[0], 'runner error: %r' % e
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for name, err in pool.map(one, tasks):
            why = pending.get((name, r.config)) or pending.get((name, None))
            if why is not None:
                if err is None:
                    fixed.append(name); print('FIXED %s (pending: %s): remove it from pending.txt' % (name, why))
                else:
                    pend += 1
            elif err is None:
                passed += 1
            else:
                failed.append(name); print('FAIL %s: %s' % (name, err))
            sys.stdout.flush()
    print('differential %s: %d passed (%d of them as the second reference, %s), %d pending, %d failed, %d fixed (%d programs, %.1f s)'
          % (r.config, passed, r.second, r.cxx2 or 'none', pend, len(failed), len(fixed), len(tasks), time.time() - start))
    if failed or fixed:
        print('%d FAILED' % (len(failed) + len(fixed)))
        sys.exit(1)
    print('ALL PASSED')

if __name__ == '__main__':
    main()
