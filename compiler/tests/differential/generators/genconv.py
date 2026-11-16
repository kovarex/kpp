#!/usr/bin/env python3
# Random user-defined conversion sequences and overload ranking with them: classes with converting
# constructors (from scalars, from each other, by value, const& and &&, two arguments, explicit ones)
# and conversion operators (to scalars, to references, to each other, const, ref-qualified, explicit
# ones); then copy, direct and list initialization, calls of overload sets mixing class, reference and
# scalar parameters, a standard conversion after a user-defined one, reference binding to a conversion's
# result (its lifetime), built-in operators through conversions, conditions, returns and casts.
# Every constructor, conversion and destructor prints, so the sequence chosen shows, not only the value.
# One source for both sides: KEXPLICIT marks C++'s explicit (nothing in kpp), IMPLICIT kpp's implicit
# (nothing in C++); genconv.sh rewrites them. Statements clang rejects are dropped, classes it rejects
# drop their block. Usage: genconv.py SEED [BLOCKS] > file
import random, sys, os, re, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get('KPP_DIFF_CXX') or r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'
r = random.Random(int(sys.argv[1]))
NB = int(sys.argv[2]) if len(sys.argv) > 2 else 14

PRELUDE = r'''extern "C" int printf(const char* format, ...);
void show(int v) { printf("i%d ", v); }
void show(long v) { printf("l%ld ", v); }
void show(unsigned v) { printf("u%u ", v); }
void show(long long v) { printf("L%lld ", v); }
void show(double v) { printf("d%.2f ", v); }
void show(float v) { printf("f%.2f ", (double)v); }
void show(char v) { printf("c%d ", (int)v); }
void show(bool v) { printf("b%d ", (int)v); }
void show(short v) { printf("s%d ", (int)v); }
void nl() { printf("\n"); }
'''

SCALARS = ['int', 'double', 'long', 'bool', 'char', 'unsigned', 'short', 'float', 'long long']
SCALAR_VALUE = {'int': '7', 'double': '2.5', 'long': '9L', 'bool': 'true', 'char': "'a'", 'unsigned': '5u',
                'short': '(short)4', 'float': '1.5f', 'long long': '11LL'}

def explicit():
    return 'KEXPLICIT ' if r.random() < 0.25 else 'IMPLICIT '

class Cls:
    def __init__(s, name, others, b):
        s.name, s.b = name, b
        s.members = []
        s.ctorFrom = set()     # what the constructors take
        s.convTo = set()       # what the conversion operators give
        N = name
        s.members.append('int v;')
        s.members.append('%s() : v(0) { printf("%s() "); }' % (N, N))
        s.members.append('%s(const %s& o) : v(o.v) { printf("%s(c%%d) ", v); }' % (N, N, N))
        if r.random() < 0.7:
            s.members.append('%s(%s&& o) : v(o.v) { o.v = -o.v; printf("%s(m%%d) ", v); }' % (N, N, N))
        s.members.append('~%s() { printf("~%s%%d ", v); }' % (N, N))
        s.members.append('%s& operator=(const %s& o) { v = o.v; printf("%s=%%d ", v); return *this; }' % (N, N, N))
        for _ in range(r.randint(1, 4)):
            c = r.random()
            if c < 0.55:
                t = r.choice(SCALARS)
                if t in s.ctorFrom: continue
                s.ctorFrom.add(t)
                s.members.append('%s%s(%s x) : v((int)x + 1) { printf("%s(%s%%d) ", v); }' % (explicit(), N, t, N, t.replace(' ', '')))
            elif c < 0.75 and others:
                o = r.choice(others)
                form = r.choice(['const %s&', '%s', '%s&&', '%s&'])
                key = form % o
                if o in s.ctorFrom: continue
                s.ctorFrom.add(o)
                s.members.append('%s%s(%s o) : v(o.v + 10) { printf("%s(%s%%d) ", v); }' % (explicit(), N, key, N, key.replace(' ', '').replace('&', 'r')))
            elif c < 0.85:
                if 'two' in s.ctorFrom: continue
                s.ctorFrom.add('two')
                s.members.append('%s%s(int a, int b) : v(a * 10 + b) { printf("%s(%%d,%%d) ", a, b); }' % (explicit(), N, N))
            else:
                if 'dflt' in s.ctorFrom: continue
                s.ctorFrom.add('dflt')
                s.members.append('%s%s(int a, double b = 0.5) : v(a + (int)(b * 10)) { printf("%s(%%d;%%.1f) ", a, b); }' % (explicit(), N, N))
        for _ in range(r.randint(0, 3)):
            c = r.random()
            if c < 0.5:
                t = r.choice(SCALARS)
                if t in s.convTo: continue
                s.convTo.add(t)
                q = r.choice([' const', ' const', '', ' &', ' const&'])
                body = 'operator %s()%s { printf("%s>%s "); return (%s)(v + 2); }' % (t, q, N, t.replace(' ', ''), t)
                if q == ' &' and r.random() < 0.5:
                    s.members.append('%s%s' % (explicit(), body))
                    s.members.append('%soperator %s() && { printf("%s>>%s "); return (%s)(v + 3); }' % (explicit(), t, N, t.replace(' ', ''), t))
                else:
                    s.members.append('%s%s' % (explicit(), body))
            elif c < 0.7:
                if 'int&' in s.convTo: continue
                s.convTo.add('int&')
                s.members.append('%soperator int&() { printf("%s>int& "); return v; }' % (explicit(), N))
            elif c < 0.9 and others:
                o = r.choice(others)
                if o in s.convTo: continue
                s.convTo.add(o)
                s.members.append('%soperator %s() const { printf("%s>%s "); %s t; t.v = v + 20; return t; }' % (explicit(), o, N, o, o))
            else:
                if 'bool' in s.convTo: continue
                s.convTo.add('bool')
                s.members.append('%soperator bool() const { printf("%s>?"); return v != 0; }' % (explicit(), N))

    def define(s):
        return ['struct %s' % s.name, '{'] + ['  ' + m for m in s.members] + ['};']

class G:
    def num(s, lo=1, hi=9): return r.randint(lo, hi)
g = G()

PARAMS_SCALAR = ['int', 'double', 'long', 'bool', 'char', 'unsigned', 'short', 'float', 'long long', 'int&', 'const int&',
                 'int&&', 'const double&', 'double&&', 'const long&']

def block(b):
    names = ['A%d' % b, 'B%d' % b, 'C%d' % b][:r.randint(2, 3)]
    classes = []
    # Each class may name only the ones before it in its constructors and conversions to them (whole
    # types), and its conversion operators to the earlier ones.
    for i, n in enumerate(names):
        classes.append(Cls(n, names[:i], b))
    defs = []
    for c in classes:
        defs += c.define()
    # Overload sets: 2-4 functions each.
    allParams = PARAMS_SCALAR + [p % n for n in names for p in ('%s', 'const %s&', '%s&&', '%s&')]
    sets = []
    for k in range(r.randint(1, 3)):
        fname = 'f%d_%d' % (b, k)
        used = set()
        for j in range(r.randint(2, 4)):
            p = r.choice(allParams)
            if r.random() < 0.15:
                p2 = r.choice(allParams)
                sig = (p, p2)
            else:
                sig = (p,)
            if sig in used: continue
            used.add(sig)
            params = ', '.join(sig)  # (unnamed: the overload reads none, and kpp's unused names are errors)
            defs.append('int %s(%s) { return %d; }' % (fname, params, j + 1))
        if r.random() < 0.2:
            defs.append('template<class T> int %s(T) { return 9; }' % fname)
        sets.append(fname)
    # Values: lvalues and prvalues of each class, scalars.
    stmts = []
    lv = {}
    for n in names:
        lv[n] = 'o' + n
        stmts.append('%s o%s; o%s.v = %d;' % (n, n, n, g.num()))
        stmts.append('const %s k%s;' % (n, n))
    stmts.append('nl();')
    def expr():
        c = r.random()
        n = r.choice(names)
        if c < 0.3: return lv[n]
        if c < 0.4: return 'k' + n
        if c < 0.55: return '%s{}' % n
        if c < 0.65: return 'move(%s)' % lv[n]
        if c < 0.75: return 'static_cast<const %s&>(%s)' % (n, lv[n])
        t = r.choice(SCALARS)
        return SCALAR_VALUE[t]
    body = []
    for _ in range(r.randint(6, 14)):
        c = r.random()
        if c < 0.3 and sets:
            f = r.choice(sets)
            args = expr()  # (one: clang's MSVC target evaluates arguments right to left, an unspecified order)
            body.append('{ int r = %s(%s); printf("=%%d", r); nl(); }' % (f, args))
        elif c < 0.5:
            n = r.choice(names)
            e = expr()
            form = r.choice(['%s x = %s;', '%s x(%s);', '%s x{%s};', '%s x = {%s};', 'const %s& x = %s;', '%s&& x = %s;'])
            body.append('{ ' + form % (n, e) + ' printf("[%d] ", x.v); } nl();')
        elif c < 0.62:
            n = r.choice(names)
            form = r.choice(['%s x{%d, %d};', '%s x = {%d, %d};', '%s x(%d, %d);', 'const %s& x = {%d, %d};'])
            body.append('{ ' + form % (n, g.num(), g.num()) + ' printf("[%d] ", x.v); } nl();')
        elif c < 0.77:
            t = r.choice(SCALARS + ['const int&', 'int&&', 'int&', 'const double&'])
            src = r.choice([lv[r.choice(names)], 'k' + r.choice(names), r.choice(names) + '{}', 'move(%s)' % lv[r.choice(names)]])
            form = r.choice(['%s x = %s;', '%s x(%s);', '%s x{%s};'])
            show = 'show(x);'
            body.append('{ ' + form % (t, src) + ' ' + show + ' } nl();')
        elif c < 0.87:
            a = expr()
            bb = expr()
            op = r.choice(['+', '-', '*', '==', '<', '!=', '&&', '||'])
            body.append('show(%s %s %s); nl();' % (a, op, bb))
        elif c < 0.92:
            a = r.choice([lv[r.choice(names)], 'k' + r.choice(names), r.choice(names) + '{}'])
            body.append(r.choice(['if (%s) printf("yes "); else printf("no "); nl();', 'show(!%s); nl();', 'show(%s ? 1 : 2); nl();']) % a)
        elif c < 0.96:
            n = r.choice(names)
            e = expr()
            body.append('{ auto l = [&]() -> %s { return %s; }; %s x = l(); printf("[%%d] ", x.v); } nl();' % (n, e, n))
        else:
            n = r.choice(names)
            t = r.choice(SCALARS)
            src = r.choice([lv[n], 'k' + n, n + '{}'])
            body.append('show(static_cast<%s>(%s)); nl();' % (t, src))
    return defs, stmts, body

blocks = [block(b) for b in range(NB)]

def render():
    lines = PRELUDE.split('\n')
    where = {}
    for bi, blk in enumerate(blocks):
        if not blk: continue
        defs, stmts, body = blk
        for d in defs:
            where[len(lines) + 1] = (bi, None)
            lines.append(d)
        where[len(lines) + 1] = (bi, None)
        lines.append('void block%d()' % bi)
        lines.append('{')
        for s in stmts:
            where[len(lines) + 1] = (bi, None)
            lines.append('  ' + s)
        for si, s in enumerate(body):
            if s is None: continue
            where[len(lines) + 1] = (bi, si)
            lines.append('  ' + s)
        lines.append('  printf("end%d\\n");' % bi)
        lines.append('}')
    lines.append('int main()')
    lines.append('{')
    for bi, blk in enumerate(blocks):
        if blk: lines.append('  block%d();' % bi)
    lines.append('  return 0;')
    lines.append('}')
    return '\n'.join(lines), where

def cxx(text):
    return text.replace('KEXPLICIT ', 'explicit ').replace('IMPLICIT ', '')

head = open(os.path.join(HERE, 'cxx-head.h')).read()
headLines = head.count('\n')
tmp = os.path.join(os.environ.get('KPP_DIFF_TMP', HERE), 'convtmp-%s.cpp' % sys.argv[1])
for it in range(8):
    text, where = render()
    open(tmp, 'w').write(head + cxx(text) + '\n')
    p = subprocess.run([CLANG, '-std=c++20', '-w', '-fsyntax-only', '-ferror-limit=0', '-fno-ms-compatibility',
                        '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', tmp], capture_output=True, text=True)
    errs = set(int(m.group(1)) - headLines for m in re.finditer(r'convtmp-[^:]*\.cpp:(\d+):\d+: error', p.stderr))
    if not errs: break
    for e in errs:
        w = where.get(e)
        if not w: sys.stderr.write('unplaced error at %d\n%s' % (e, p.stderr[:2000])); sys.exit(1)
        bi, si = w
        if si is None: blocks[bi] = None
        elif blocks[bi]: blocks[bi][2][si] = None
os.remove(tmp)
print(render()[0])
