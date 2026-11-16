#!/usr/bin/env python3
# Random constexpr functions of statements (loops, switch, arrays, a struct with member functions,
# pointers into arrays, references, lambdas, calls of the other functions, recursion), each called at
# compile time (`constexpr int c = f(...)`, kpp's interpreter) and at run time on volatile arguments
# (kpp's code generator); clang drops each call its constant evaluator rejects (undefined behaviour)
# and each function it rejects. Both columns must match each other and clang's.
# Usage: genconst.py SEED [FUNCTIONS] > file
import random, sys, subprocess, os, re

r = random.Random(int(sys.argv[1]))
NF = int(sys.argv[2]) if len(sys.argv) > 2 else 8
CLANG = os.environ.get('KPP_DIFF_CXX') or r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'
HERE = os.path.dirname(os.path.abspath(__file__))

PRELUDE = r'''extern "C" int printf(const char* format, ...);
class P
{
public:
  int x;
  int y;
  constexpr int sum() const { return x + y; }
  constexpr void bump(int k) { x += k; y ^= k; }
  constexpr P swapped() const { return P{y, x}; }
};
'''

class Gen:
    def __init__(s, f):
        s.f = f
        s.n = 0
        s.ints = ['a', 'b']   # int variables in scope
        s.uns = []            # unsigned variables
        s.arrs = []           # (name, size) int arrays
        s.ps = []             # P variables
        s.loopvars = []
    def name(s, p):
        s.n += 1
        return '%s%d' % (p, s.n)
    def small(s):
        return str(r.randint(-9, 20))
    def iexpr(s, d=0):
        # an int expression; small values only from masks to keep clear of overflow
        c = r.randrange(14 if d < 2 else 4)
        if c == 0: return s.small()
        if c in (1, 2, 3): return r.choice(s.ints + s.loopvars)
        if c == 4: return '(%s %s %s)' % (s.iexpr(d + 1), r.choice(['+', '-', '&', '|', '^']), s.iexpr(d + 1))
        if c == 5: return '((%s) %% 1000 * (%s %% 1000))' % (s.iexpr(d + 1), s.iexpr(d + 1))
        if c == 6: return '(%s / ((%s) | 1))' % (s.iexpr(d + 1), s.iexpr(d + 1))
        if c == 7: return '(%s %% ((%s) | 1))' % (s.iexpr(d + 1), s.iexpr(d + 1))
        if c == 8: return '(%s < %s ? %s : %s)' % (s.iexpr(d + 1), s.iexpr(d + 1), s.iexpr(d + 1), s.iexpr(d + 1))
        if c == 9 and s.arrs:
            a, n = r.choice(s.arrs)
            return '%s[(%s & 0x7fffffff) %% %d]' % (a, s.iexpr(d + 1), n)
        if c == 10 and s.ps:
            p = r.choice(s.ps)
            return r.choice(['%s.x', '%s.y', '%s.sum()', '%s.swapped().x']) % p
        if c == 11 and s.uns:
            return 'int(%s %% 4096u)' % r.choice(s.uns)
        if c == 12 and s.f > 0:
            return 'f%d((%s) %% 50, (%s) %% 50)' % (r.randrange(s.f), s.iexpr(d + 1), s.iexpr(d + 1))
        if c == 13: return '((%s) >> %d)' % (s.iexpr(d + 1), r.randrange(5))
        return r.choice(s.ints)
    def uexpr(s):
        c = r.randrange(5)
        if c == 0: return '%du' % r.randint(0, 4000000000)
        if c == 1 and s.uns: return '(%s * %du)' % (r.choice(s.uns), r.randint(1, 100000))
        if c == 2 and s.uns: return '(%s << %d)' % (r.choice(s.uns), r.randrange(32))
        if c == 3: return 'unsigned(%s)' % s.iexpr(1)
        return '(%s - unsigned(%s))' % (r.choice(s.uns) if s.uns else '7u', s.iexpr(1))
    def block(s, ind, depth, fuel):
        out = []
        saved = (list(s.ints), list(s.uns), list(s.arrs), list(s.ps))
        for _ in range(r.randint(1, 4)):
            if fuel[0] <= 0: break
            fuel[0] -= 1
            out += s.stmt(ind, depth, fuel)
        s.ints, s.uns, s.arrs, s.ps = saved
        return out
    def target(s):
        c = r.randrange(4)
        if c == 0 and s.arrs:
            a, n = r.choice(s.arrs)
            return '%s[%d]' % (a, r.randrange(n))
        if c == 1 and s.ps:
            return '%s.%s' % (r.choice(s.ps), r.choice('xy'))
        return r.choice(s.ints)
    def stmt(s, ind, depth, fuel):
        p = ' ' * ind
        c = r.randrange(18)
        if c <= 1:
            v = s.name('i')
            init = s.iexpr()
            s.ints.append(v)
            return [p + '[[maybe_unused]] int %s = %s;' % (v, init)]  # (read or not: kpp's unused names are errors)
        if c == 2:
            v = s.name('u')
            init = s.uexpr()
            s.uns.append(v)
            return [p + '[[maybe_unused]] unsigned %s = %s;' % (v, init)]
        if c == 3 and s.uns:
            return [p + '%s %s %s;' % (r.choice(s.uns), r.choice(['+=', '-=', '*=', '^=', '|=', '&=']), s.uexpr())]
        if c == 4:
            v = s.name('arr')
            n = r.randint(2, 6)
            init = ', '.join(s.iexpr(1) for _ in range(r.randint(0, n)))
            s.arrs.append((v, n))  # (in scope after its initializer: kpp's interpreter refuses a read of itself there)
            return [p + '[[maybe_unused]] int %s[%d] = {%s};' % (v, n, init)]
        if c == 5:
            v = s.name('p')
            init = (s.iexpr(1), s.iexpr(1))
            s.ps.append(v)
            return [p + '[[maybe_unused]] P %s{%s, %s};' % (v, init[0], init[1])]
        if c == 6:
            t = s.target()
            return [p + '%s = (%s %s %s) %% 100000;' % (t, t, r.choice(['+', '-', '^', '|', '&']), s.iexpr(1))]
        if c == 7 and s.ps:
            return [p + '%s.bump(%s %% 100);' % (r.choice(s.ps), s.iexpr(1))]
        if c == 8 and s.arrs:
            a, n = r.choice(s.arrs)
            q = s.name('q')
            k = r.randrange(n)
            return [p + '{ int* %s = %s + %d; *%s += %s %% 100; %s[0] -= 1; if (%s - %s == %d) %s[0] ^= 3; }' % (q, a, k, q, s.iexpr(1), q, q, a, k, q)]
        if c == 9:
            t = s.target()
            ref = s.name('ref')
            return [p + '{ int& %s = %s; %s = %s %% 1000 * 3; }' % (ref, t, ref, ref)]
        if c == 10 and depth < 3:
            iv = s.name('k')
            out = [p + 'for (int %s = 0; %s < %d; %s++)' % (iv, iv, r.randint(1, 6), iv), p + '{']
            s.loopvars.append(iv)
            if r.random() < 0.3:
                out.append(p + '  if (%s == %d) %s;' % (iv, r.randrange(4), r.choice(['break', 'continue'])))
            out += s.block(ind + 2, depth + 1, fuel)
            s.loopvars.pop()
            out.append(p + '}')
            return out
        if c == 11 and depth < 3:
            w = s.name('w')
            out = [p + '{', p + '  int %s = %d;' % (w, r.randint(1, 5)), p + '  while (%s-- > 0)' % w, p + '  {']
            s.loopvars.append(w)
            out += s.block(ind + 4, depth + 1, fuel)
            s.loopvars.pop()
            out += [p + '  }', p + '}']
            return out
        if c == 12 and depth < 3:
            out = [p + 'switch ((%s & 0x7fffffff) %% 4)' % s.iexpr(1), p + '{']
            for k in r.sample(range(4), r.randint(1, 3)):
                out.append(p + 'case %d:' % k)
                out.append(p + '{')
                out += s.block(ind + 2, depth + 1, fuel)
                out += [p + '  break;', p + '}']
            out += [p + 'default:', p + '  break;', p + '}']
            return out
        if c == 13 and depth < 3:
            out = [p + 'if (%s %s %s)' % (s.iexpr(1), r.choice(['<', '>', '==', '!=', '<=']), s.iexpr(1)), p + '{']
            out += s.block(ind + 2, depth + 1, fuel)
            out.append(p + '}')
            if r.random() < 0.5:
                out += [p + 'else', p + '{'] + s.block(ind + 2, depth + 1, fuel) + [p + '}']
            return out
        if c == 14:
            l = s.name('l')
            t = r.choice(s.ints)
            return [p + '{ auto %s = [&](int z) { %s = (%s + z) %% 10000; return z * 2; }; %s = %s(%s %% 100) %% 10000; }' % (l, t, t, r.choice(s.ints), l, s.iexpr(1))]
        if c == 15 and s.ps:
            v = s.name('p')
            src = r.choice(s.ps)
            s.ps.append(v)
            return [p + '[[maybe_unused]] P %s = %s;' % (v, src), p + '%s.x += 1;' % v]
        if c == 16 and s.arrs:
            a, n = r.choice(s.arrs)
            return [p + 'for (int& e : %s) e = (e * 3 + %s) %% 1000;' % (a, s.iexpr(1))]
        if c == 17:
            return [p + 'if (%s > %s) return (%s) %% 100000;' % (s.iexpr(1), s.iexpr(1), s.iexpr())]
        return [p + 'a = (a + %s) %% 100000;' % s.iexpr(1)]

funcs = []
for f in range(NF):
    g = Gen(f)
    body = g.block(2, 0, [r.randint(5, 16)])
    ret = g.iexpr()
    lines = ['constexpr int f%d([[maybe_unused]] int a, [[maybe_unused]] int b)' % f, '{'] + body + ['  return (%s) %% 100000;' % ret, '}']
    funcs.append([lines, [(r.randint(-20, 40), r.randint(-20, 40)) for _ in range(4)], True])

def render():
    out = PRELUDE.split('\n')
    where = {}
    for fi, (lines, calls, ok) in enumerate(funcs):
        for l in (lines if ok else ['constexpr int f%d(int a, int b) { return a - b; }' % fi]):
            where[len(out) + 1] = ('def', fi)
            out.append(l)
    out += ['int main()', '{']
    for fi, (lines, calls, ok) in enumerate(funcs):
        if not ok: continue
        for ci, (x, y) in enumerate(calls):
            where[len(out) + 1] = ('call', fi, ci)
            out.append('  { constexpr int c = f%d(%d, %d); volatile int x = %d, y = %d; int rt = f%d(x, y); printf("f%d(%d,%d) %%d %%d\\n", c, rt); }'
                       % (fi, x, y, x, y, fi, fi, x, y))
    out += ['  return 0;', '}']
    return '\n'.join(out), where

head = open(os.path.join(HERE, 'cxx-head.h')).read()
headLines = head.count('\n')
tmp = os.path.join(os.environ.get('KPP_DIFF_TMP', HERE), 'cxtmp-%s.cpp' % sys.argv[1])
for it in range(10):
    text, where = render()
    open(tmp, 'w').write(head + text + '\n')
    p = subprocess.run([CLANG, '-std=c++20', '-w', '-fsyntax-only', '-ferror-limit=0', '-fconstexpr-steps=10000000',
                        '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', tmp], capture_output=True, text=True)
    errs = set(int(m.group(1)) - headLines for m in re.finditer(r'cxtmp-[^:]*\.cpp:(\d+):\d+: error', p.stderr))
    if not errs: break
    for e in sorted(errs, reverse=True):
        w = where.get(e)
        if not w:
            sys.stderr.write('unplaced error at %d\n' % e)
            continue
        if w[0] == 'def': funcs[w[1]][2] = False
        elif w[2] < len(funcs[w[1]][1]): del funcs[w[1]][1][w[2]]
os.remove(tmp)
print(render()[0])
