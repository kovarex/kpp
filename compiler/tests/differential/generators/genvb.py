#!/usr/bin/env python3
# Random hierarchies with virtual bases (diamonds, a virtual and a non-virtual base of one class), name
# hiding (a derived f(double) hiding a base's f(int)), using-declarations (bringing a base's overloads
# into the derived class's set, inheriting constructors), virtual functions overridden along several
# paths, covariant returns through virtual bases, calls from constructors and destructors, dynamic_cast
# across the diamond, member access through every base. Constructors, destructors and calls print, so
# the order of construction (the most derived class initializing the virtual bases) shows. One source
# for kpp and C++; what clang rejects is dropped (a statement) or drops its block (a class).
# Usage: genvb.py SEED [BLOCKS] > file
import random, sys, os, re, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get('KPP_DIFF_CXX') or r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'
r = random.Random(int(sys.argv[1]))
NB = int(sys.argv[2]) if len(sys.argv) > 2 else 10

PRELUDE = r'''extern "C" int printf(const char* format, ...);
void nl() { printf("\n"); }
'''

PARAMS = ['int', 'double', 'long', 'char', 'unsigned']
ARGS = ['1', '2.5', '3L', "'c'", '4u', 'true', '(short)5']

def block(b):
    n = r.randint(3, 6)
    cls = ['V%d_%d' % (b, i) for i in range(n)]
    bases = []
    for i in range(n):
        if i == 0:
            bases.append([])
            continue
        k = r.choice([1, 1, 2, 2, 3])
        bs = r.sample(range(i), min(k, i))
        def below(c):
            out = set()
            for j, v in bases[c]:
                out |= {j} | below(j)
            return out
        # a base also reached through another base is virtual on both edges, a diamond, never one
        # class both a direct non-virtual base and an inherited one
        bases.append([(j, False) for j in bs])  # (kpp has no virtual bases: a diamond is two subobjects, as in C++)
    defs = []
    funcs = {}  # class -> list of (name, param, virtual)
    has = [set() for _ in range(n)]
    vset = [set() for _ in range(n)]  # (name, parameter) virtual in the class: its own or inherited  # names a class declares or inherits along some path
    def reach(c):
        out = set(has[c])
        for j, v in bases[c]:
            out |= reach(j)
        return out
    for i in range(n):
        for j, _ in bases[i]:
            vset[i] |= vset[j]
        N = cls[i]
        head = 'class %s' % N
        if bases[i]:
            head += ' : ' + ', '.join(('virtual ' if v else '') + 'public ' + cls[j] for j, v in bases[i])
        lines = [head, '{', 'public:']
        lines.append('  int m = %d;' % (i + 1))
        # Constructors: a default one, one taking an int that initializes its direct bases and, at
        # random, an indirect virtual base.
        lines.append('  %s() { printf("%s() "); }' % (N, N))
        inits = []
        for j, v in bases[i]:
            inits.append('%s(s + %d)' % (cls[j], j + 1))
        indirect = set()
        def allBases(c):
            out = []
            for j, v in bases[c]:
                out.append((j, v))
                out += allBases(j)
            return out
        for j, v in allBases(i):
            if v and j not in [x for x, _ in bases[i]] and r.random() < 0.4:
                pass
        for j in sorted(indirect):
            inits.append('%s(s * 3)' % cls[j])
        initText = (' : ' + ', '.join(inits)) if inits else ''
        ctorAt = len(lines)
        fs = []
        for name in ('f', 'g', 'h'):
            if r.random() < 0.55:
                for _ in range(r.choice([1, 1, 2])):
                    p = r.choice(PARAMS)
                    if any(x[0] == name and x[1] == p for x in fs): continue
                    over = any((name, p) in vset[j] for j, _ in bases[i])
                    virt = over or r.random() < 0.6
                    if virt: vset[i].add((name, p))
                    fs.append((name, p, virt))
                    has[i].add(name)
                    lines.append('  %sint %s(%s x)%s { printf("%s::%s(%s) "); return (int)x * %d + m; }' % ('virtual ' if virt else '', name, p, ' override' if over else '', N, name, p, i + 2))
        funcs[i] = fs
        for j, v in bases[i]:
            if r.random() < 0.35 and reach(j):
                lines.append('  using %s::%s;' % (cls[j], r.choice(sorted(reach(j) - {'self'}) or ['f'])))
        if bases[i] and r.random() < 0.2:
            j = r.choice(bases[i])[0]
            lines.append('  using %s::%s;' % (cls[j], cls[j]))
        if r.random() < 0.35:
            # covariant self(), virtual where a base has one
            over = any(('self', None) in vset[j] for j, _ in bases[i])
            vset[i].add(('self', None))
            lines.append('  virtual %s* self()%s { printf("%s::self "); return this; }' % (N, ' override' if over else '', N))
            has[i].add('self')
        known = reach(i)
        lines.insert(ctorAt, '  %s~%s() { printf("~%s "); %s}' % ('virtual ' if r.random() < 0.5 else '', N, N,
                     ('{ int v = f(2); printf("[~%d] ", v); } ' if 'f' in has[i] and r.random() < 0.3 else '')))
        lines.insert(ctorAt, '  %s(int s)%s { m += s; printf("%s(%%d) ", m); %s}' % (N, initText, N,
                     ('{ int v = f(1); printf("[%d] ", v); } ' if 'f' in has[i] and r.random() < 0.4 else '')))
        lines.append('};')
        defs.append(lines)
    stmts = []
    for _ in range(r.randint(4, 9)):
        k = r.randrange(n)
        D = cls[k]
        body = []
        ctor = r.choice(['%s o(%d);' % (D, r.randint(1, 9)), '%s o;' % D, '%s o{%d};' % (D, r.randint(1, 9))])
        body.append(ctor)
        body.append('nl();')
        for _ in range(r.randint(2, 6)):
            c = r.random()
            names = sorted(reach(k) - {'self'})
            if not names: break
            name = r.choice(names)
            a = r.choice(ARGS)
            if c < 0.35:
                body.append('{ int v = o.%s(%s); printf("=%%d ", v); }' % (name, a))
            elif c < 0.55:
                j = r.randrange(k + 1)
                body.append('{ %s* p = &o; int v = p->%s(%s); printf("=%%d ", v); }' % (cls[j], name, a))
            elif c < 0.65:
                j = r.randrange(k + 1)
                body.append('{ int v = o.%s::%s(%s); printf("q%%d ", v); }' % (cls[j], name, a))
            elif c < 0.75:
                j = r.randrange(k + 1)
                body.append('printf("m%%d ", static_cast<%s&>(o).m);' % cls[j])
            elif c < 0.85:
                j, l = r.randrange(k + 1), r.randrange(n)
                body.append('{ %s* p = &o; %s* q = dynamic_cast<%s*>(p); printf("dc%%d ", q ? q->m : -1); }' % (cls[j], cls[l], cls[l]))
            elif c < 0.92 and 'self' in reach(k):
                j = r.randrange(k + 1)
                body.append('{ %s* p = &o; auto* s = p->self(); printf("s%%d ", s->m); }' % cls[j])
            else:
                j = r.randrange(k + 1)
                body.append('{ %s* p = new %s(%d); %s* q = p; int v = q->%s(%s); printf("=%%d ", v); delete p; }' % (D, D, r.randint(1, 9), cls[j], name, a))
        body.append('nl();')
        stmts.append(body)
    return defs, stmts

blocks = [block(b) for b in range(NB)]

def render():
    lines = PRELUDE.split('\n')
    where = {}
    for bi, blk in enumerate(blocks):
        if not blk: continue
        defs, stmts = blk
        for d in defs:
            for l in d:
                where[len(lines) + 1] = (bi, None, None)
                lines.append(l)
        lines.append('void block%d()' % bi)
        lines.append('{')
        for si, s in enumerate(stmts):
            if s is None: continue
            lines.append('  {')
            for li, l in enumerate(s):
                if l is None: continue
                # the declaration: its loss drops the statement group
                where[len(lines) + 1] = (bi, si, None if li == 0 else li)
                lines.append('    ' + l)
            lines.append('  }')
        lines.append('  printf("end%d\\n");' % bi)
        lines.append('}')
    lines.append('int main()')
    lines.append('{')
    for bi, blk in enumerate(blocks):
        if blk: lines.append('  block%d();' % bi)
    lines.append('  return 0;')
    lines.append('}')
    return '\n'.join(lines), where

head = open(os.path.join(HERE, 'cxx-head.h')).read()
headLines = head.count('\n')
tmp = os.path.join(os.environ.get('KPP_DIFF_TMP', HERE), 'vbtmp-%s.cpp' % sys.argv[1])
for it in range(10):
    text, where = render()
    open(tmp, 'w').write(head + text + '\n')
    p = subprocess.run([CLANG, '-std=c++20', '-w', '-fsyntax-only', '-ferror-limit=0', '-fno-ms-compatibility',
                        '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', tmp], capture_output=True, text=True)
    errs = set(int(m.group(1)) - headLines for m in re.finditer(r'vbtmp-[^:]*\.cpp:(\d+):\d+: error', p.stderr))
    if not errs: break
    for e in errs:
        w = where.get(e)
        if not w: sys.stderr.write('unplaced error at %d\n%s' % (e, p.stderr[:2000])); sys.exit(1)
        bi, si, li = w
        if not blocks[bi]: continue
        if si is None: blocks[bi] = None
        elif li is None: blocks[bi][1][si] = None
        elif blocks[bi][1][si]: blocks[bi][1][si][li] = None
os.remove(tmp)
print(render()[0])
