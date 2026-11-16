#!/usr/bin/env python3
# Random exceptions in partial construction and in handler matching: a constructor that throws after
# some bases and members are built (which of them are destroyed, in what order), arrays and new[] whose
# k-th element throws, member initializers that throw, a function-try-block of a constructor that
# rethrows; then handlers matching a hierarchy (by value, by reference to a base, by pointer to a base,
# a base reached twice, which no handler of it matches, catch (...), handler order), and rethrow of a
# caught object modified in the handler. Every constructor and destructor prints. One source for kpp and
# C++ (cxx-head.h); what clang rejects is dropped. Usage: genexc2.py SEED [BLOCKS] > file
import random, sys, os, re, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get('KPP_DIFF_CXX') or r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'
r = random.Random(int(sys.argv[1]))
NB = int(sys.argv[2]) if len(sys.argv) > 2 else 12

PRELUDE = r'''extern "C" int printf(const char* format, ...);
int budget = 1000;
void nl() { printf("\n"); }
class T
{
public:
  int id;
  T(int i) : id(i) { if (--budget == 0) { printf("T%d! ", id); throw id; } printf("+T%d ", id); }
  T(const T& o) : id(o.id + 100) { printf("cT%d ", id); }
  ~T() { printf("-T%d ", id); }
};
class E0 { public: int v; E0(int x) : v(x) { printf("+E0.%d ", v); } E0(const E0& o) : v(o.v) { printf("cE0.%d ", v); } virtual ~E0() { printf("-E0.%d ", v); } virtual int kind() const { return 0; } };
class E1 : public E0 { public: E1(int x) : E0(x) { printf("+E1 "); } E1(const E1& o) : E0(o) { printf("cE1 "); } ~E1() override { printf("-E1 "); } int kind() const override { return 1; } };
class E2 : public E0 { public: E2(int x) : E0(x) { printf("+E2 "); } E2(const E2& o) : E0(o) { printf("cE2 "); } ~E2() override { printf("-E2 "); } int kind() const override { return 2; } };
class E3 : public E1 { public: E3(int x) : E1(x) { printf("+E3 "); } E3(const E3& o) : E1(o) { printf("cE3 "); } ~E3() override { printf("-E3 "); } int kind() const override { return 3; } };
class EL : public E0 { public: EL(int x) : E0(x) { printf("+EL "); } EL(const EL& o) : E0(o) { printf("cEL "); } ~EL() override { printf("-EL "); } };
class ER : public E0 { public: ER(int x) : E0(x) { printf("+ER "); } ER(const ER& o) : E0(o) { printf("cER "); } ~ER() override { printf("-ER "); } };
class EB : public EL, public ER { public: EB(int x) : EL(x), ER(x + 1) { printf("+EB "); } EB(const EB& o) : EL(o), ER(o) { printf("cEB "); } ~EB() override { printf("-EB "); } };
'''

EXC = ['E0', 'E1', 'E2', 'E3', 'EB', 'int', 'long']
HANDLERS = ['E0', 'const E0&', 'E1&', 'const E1&', 'E2', 'const E3&', 'EL&', 'ER&', 'E0*', 'E1*', 'const E0*', 'int', 'long', 'unsigned', '...']

def throwExpr(k, n):
    if k == 'int': return 'throw %d' % n
    if k == 'long': return 'throw %dL' % n
    if r.random() < 0.25:
        return 'throw new %s(%d)' % (k, n)  # a pointer
    return 'throw %s(%d)' % (k, n)

def handlerBody(h, rethrow):
    if h == '...':
        body = 'printf("[...] ");'
    elif h.endswith('*'):
        body = 'printf("[p%%d] ", e->v); delete e;' if not rethrow else 'printf("[p%%d] ", e->v);'
    elif h in ('int', 'long', 'unsigned'):
        body = 'printf("[n%%d] ", (int)e);'
    else:
        body = 'printf("[%%d:%%d] ", e.kind(), e.v);'
        if '&' in h and 'const' not in h and r.random() < 0.4:
            body = 'e.v += 50; ' + body
    if rethrow:
        body += ' throw;'
    return body

def block(b):
    lines, defs = [], []
    c = r.random()
    if c < 0.35:
        # partial construction: bases and members, the constructor or a member initializer throws
        nb = r.randint(0, 2); nm = r.randint(1, 3)
        name = 'P%d' % b
        bases = ['PB%d_%d' % (b, i) for i in range(nb)]
        for bi, bn in enumerate(bases):
            defs.append('class %s { public: T t; %s(int x) : t(x) { printf("+%s "); } ~%s() { printf("-%s "); } };' % (bn, bn, bn, bn, bn))
        members = ['T m%d;' % i for i in range(nm)]
        inits = ['%s(%d)' % (bn, 10 * (bi + 1)) for bi, bn in enumerate(bases)] + ['m%d(%d)' % (i, 100 + i) for i in range(nm)]
        throwsInBody = r.random() < 0.5
        fty = r.random() < 0.3
        body = '{ printf("+%s "); %s}' % (name, 'throw E1(%d); ' % b if throwsInBody else '')
        head = 'class %s%s { public: %s %s([[maybe_unused]] int k)' % (name, (' : ' + ', '.join('public ' + x for x in bases)) if bases else '', ' '.join(members), name)
        if fty:
            defs.append(head + ' try : %s %s catch (...) { printf("[ctor-catch] "); } ~%s() { printf("-%s "); } };' % (', '.join(inits), body, name, name))
        else:
            defs.append(head + ' : %s %s ~%s() { printf("-%s "); } };' % (', '.join(inits), body, name, name))
        total = nb + nm + 1
        for _ in range(r.randint(2, 4)):
            k = r.randint(1, total + 1)
            lines.append('budget = %d; try { %s p(1); printf("built "); } catch (const E0& e) { printf("[E%%d] ", e.kind()); } catch (int n) { printf("[T%%d] ", n); } nl();' % (k, name))
        return defs, lines
    if c < 0.55:
        # arrays and new[]
        n = r.randint(2, 5)
        for _ in range(r.randint(2, 4)):
            k = r.randint(1, n + 1)
            form = r.choice(['T a[%d] = {%s};' % (n, ', '.join(str(i + 1) for i in range(n))),
                             'T* a = new T[%d]{%s}; delete[] a;' % (n, ', '.join(str(i + 1) for i in range(n))),
                             'T a[%d] = {%s}; T b(99);' % (n, ', '.join(str(i + 1) for i in range(n)))])
            lines.append('budget = %d; try { %s printf("built "); } catch (int n) { printf("[T%%d] ", n); } nl();' % (k, form))
        return defs, lines
    # handler matching
    for _ in range(r.randint(3, 6)):
        k = r.choice(EXC)
        hs = r.sample(HANDLERS, r.randint(1, 4))
        if '...' in hs:
            hs.remove('...'); hs.append('...')
        rethrow = r.random() < 0.3
        inner = 'try { %s; } ' % throwExpr(k, r.randint(1, 9)) + ' '.join('catch (%s%s) { %s }' % (h, '' if h == '...' else ' e', handlerBody(h, rethrow)) for h in hs)
        outer = 'try { %s } catch (const E0& e) { printf("{%%d:%%d} ", e.kind(), e.v); } catch (E0* e) { printf("{p%%d} ", e->v); delete e; } catch (...) { printf("{...} "); }' % inner
        lines.append(outer + ' nl();')
    return defs, lines

blocks = [block(b) for b in range(NB)]

def render():
    out = PRELUDE.split('\n')
    where = {}
    for bi, blk in enumerate(blocks):
        if not blk: continue
        defs, lines = blk
        for d in defs:
            where[len(out) + 1] = (bi, None)
            out.append(d)
        out.append('void block%d()' % bi)
        out.append('{')
        for li, l in enumerate(lines):
            if l is None: continue
            where[len(out) + 1] = (bi, li)
            out.append('  ' + l)
        out.append('  printf("end%d\\n");' % bi)
        out.append('}')
    out.append('int main()')
    out.append('{')
    for bi, blk in enumerate(blocks):
        if blk: out.append('  block%d();' % bi)
    out.append('  return 0;')
    out.append('}')
    return '\n'.join(out), where

head = open(os.path.join(HERE, 'cxx-head.h')).read()
headLines = head.count('\n')
tmp = os.path.join(os.environ.get('KPP_DIFF_TMP', HERE), 'exc2tmp-%s.cpp' % sys.argv[1])
for it in range(8):
    text, where = render()
    open(tmp, 'w').write(head + text + '\n')
    p = subprocess.run([CLANG, '-std=c++20', '-w', '-fsyntax-only', '-ferror-limit=0', '-fno-ms-compatibility',
                        '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', tmp], capture_output=True, text=True)
    errs = set(int(m.group(1)) - headLines for m in re.finditer(r'exc2tmp-[^:]*\.cpp:(\d+):\d+: error', p.stderr))
    if not errs: break
    for e in errs:
        w = where.get(e)
        if not w: sys.stderr.write('unplaced error at %d\n%s' % (e, p.stderr[:2000])); sys.exit(1)
        bi, li = w
        if not blocks[bi]: continue
        if li is None: blocks[bi] = None
        else: blocks[bi][1][li] = None
os.remove(tmp)
print(render()[0])
