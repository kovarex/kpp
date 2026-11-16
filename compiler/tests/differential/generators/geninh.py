#!/usr/bin/env python3
# Random class hierarchies with multiple (non-virtual) inheritance: every subobject reached along every
# path of static_casts, then virtual calls, qualified calls, covariant returns, dynamic_casts to every
# class, static downcasts, member pointers, slices and deletion through a base. One source for kpp and
# C++; the oracle is the other compiler. Usage: geninh.py SEED [CLASSES] > file
import random, sys

r = random.Random(int(sys.argv[1]))
N = int(sys.argv[2]) if len(sys.argv) > 2 else r.randint(4, 8)
FUNCS = ['f0', 'f1', 'f2', 'f3']

bases = []      # bases[i]: list of direct bases
decl = []       # decl[i]: {name: 'pure'|'virtual'|'plain'}
selfdecl = []   # class declares virtual C_i* self()
vdtor = []      # destructor declared virtual
final = []

def subpaths(c):
    # every path from c to a subobject, as lists of classes starting with c
    out = [[c]]
    for b in bases[c]:
        for p in subpaths(b):
            out.append([c] + p)
    return out

def count(c, x):
    return sum(1 for p in subpaths(c) if p[-1] == x)

def lookup(c, name):
    # C++ member name lookup without virtual bases: the declarations found, each as a path
    if name in decl[c] or (name == 'self' and selfdecl[c]):
        return [[c]]
    out = []
    for b in bases[c]:
        for p in lookup(b, name):
            out.append([c] + p)
    return out

def declares(c, name):
    return selfdecl[c] if name == 'self' else name in decl[c]

def virtualIn(c, name):
    # the name is virtual in c's own declaration or in any declaration it hides
    for p in subpaths(c):
        k = p[-1]
        if name == 'self':
            if selfdecl[k]: return True
        elif decl[k].get(name) in ('virtual', 'pure'):
            return True
    return False

def polymorphic(c):
    for p in subpaths(c):
        k = p[-1]
        if vdtor[k] or selfdecl[k] or any(v in ('virtual', 'pure') for v in decl[k].values()):
            return True
    return False

def virtualDtor(c):
    return any(vdtor[p[-1]] for p in subpaths(c))

def abstract(c):
    for p in subpaths(c):
        s = p[-1]
        for name, kind in decl[s].items():
            if kind == 'plain': continue
            for k in p:
                if name in decl[k]:
                    if decl[k][name] == 'pure': return True
                    break
    return False

for i in range(N):
    nb = 0 if i == 0 else r.choice([0, 1, 1, 1, 2, 2, 3])
    cand = [j for j in range(i) if not final[j]]
    bs = r.sample(cand, min(nb, len(cand)))
    bases.append(bs)
    d = {}
    selfdecl.append(False)
    vdtor.append(False)
    final.append(False)
    decl.append(d)
    for f in FUNCS:
        if r.random() < 0.4:
            inherited = any(lookup(b, f) for b in bs)
            virt = any(virtualIn(b, f) for b in bs)
            if virt:
                d[f] = 'virtual'
            elif inherited and r.random() < 0.3:
                d[f] = 'plain'   # hides a non-virtual one
            else:
                d[f] = r.choice(['virtual', 'virtual', 'virtual', 'pure', 'plain']) if not inherited else 'virtual'
    # self(): covariant, needs every overridden declaration's class to be an unambiguous base
    over = set(p[-1] for p in subpaths(i)[1:] if selfdecl[p[-1]])
    if r.random() < 0.4 and all(count(i, k) == 1 for k in over):
        selfdecl[i] = True
    vdtor[i] = r.random() < 0.5
    final[i] = r.random() < 0.1

out = []
w = out.append
w('extern "C" int printf(const char* format, ...);')
for i in range(N):
    w('class C%d;' % i)
for i in range(N):
    head = 'class C%d%s' % (i, ' final' if final[i] else '')
    if bases[i]:
        head += ' : ' + ', '.join('public C%d' % b for b in bases[i])
    w(head)
    w('{')
    w('public:')
    w('  int m%d;' % i)
    inits = ['C%d(s + %d)' % (b, k + 1) for k, b in enumerate(bases[i])] + ['m%d(s * 7 + %d)' % (i, i)]
    body = ['printf("C%d(%%d) ", m%d);' % (i, i)]
    for f in FUNCS:
        l = lookup(i, f)
        if len(l) == 1 and r.random() < 0.5:
            k = l[0][-1]
            if decl[k][f] != 'pure':
                body.append('{ int v = %s(%d); printf("[%%d] ", v); }' % (f, i))
    w('  C%d(int s) : %s { %s }' % (i, ', '.join(inits), ' '.join(body)))
    dbody = ['printf("~C%d(%%d) ", m%d);' % (i, i)]
    for f in FUNCS:
        l = lookup(i, f)
        if len(l) == 1 and r.random() < 0.3:
            k = l[0][-1]
            if decl[k][f] != 'pure':
                dbody.append('{ int v = %s(%d); printf("[~%%d] ", v); }' % (f, 10 + i))
    w('  %s~C%d() { %s }' % ('virtual ' if vdtor[i] else '', i, ' '.join(dbody)))
    for f, kind in decl[i].items():
        k = FUNCS.index(f)
        ov = any(virtualIn(b, f) for b in bases[i])
        if kind == 'pure':
            w('  virtual int %s(int x) = 0;' % f)
        else:
            spec = ('virtual ' if kind == 'virtual' and not ov else '')
            suffix = (' override' if ov else '')
            w('  %sint %s(int x)%s { printf("C%d::%s "); return x * %d + m%d + %d; }' % (spec, f, suffix, i, f, k + 2, i, 100 * i))
    if selfdecl[i]:
        ov = any(virtualIn(b, 'self') for b in bases[i])
        w('  %sC%d* self()%s { printf("C%d::self "); return this; }' % ('' if ov else 'virtual ', i, ' override' if ov else '', i))
    w('};')

def pathExpr(p, start):
    e = start
    for k in p[1:]:
        e = 'static_cast<C%d*>(%s)' % (k, e)
    return e

w('int main()')
w('{')
concrete = [i for i in range(N) if not abstract(i)]
for d in concrete:
    w('  {')
    w('    printf("\\n== C%d: ");' % d)
    w('    C%d* p = new C%d(%d);' % (d, d, 10 * d))
    w('    printf("\\n");')
    paths = [q for q in subpaths(d) if all(count(q[j], q[j + 1]) == 1 for j in range(len(q) - 1))]
    if len(paths) > 14:
        paths = [paths[0]] + r.sample(paths[1:], 13)
    deleter = None
    for pi, p in enumerate(paths):
        x = p[-1]
        w('    {')
        w('      C%d* x = %s;' % (x, pathExpr(p, 'p')))
        w('      printf("%s: %%d ", x->m%d);' % ('>'.join('C%d' % k for k in p), x))
        for f in FUNCS:
            l = lookup(x, f)
            if len(l) != 1: continue
            k = l[0][-1]
            w('      { int v = x->%s(3); printf("=%%d ", v); }' % f)
            if decl[k][f] != 'pure' and r.random() < 0.5:
                w('      { int v = x->C%d::%s(4); printf("q%%d ", v); }' % (k, f))
            if r.random() < 0.5:
                w('      { int (C%d::*pm)(int) = &C%d::%s; int v = (x->*pm)(5); printf("pm%%d ", v); }' % (k, x, f))
                if count(d, k) == 1:
                    w('      { int (C%d::*pm)(int) = &C%d::%s; int (C%d::*pd)(int) = pm; int v = (p->*pd)(6); printf("pd%%d ", v); }' % (k, x, f, d))
        l = lookup(x, 'self')
        if len(l) == 1:
            k = l[0][-1]
            w('      { C%d* s = x->self(); printf("s%%d %%d ", s->m%d, dynamic_cast<void*>(s) == (void*)p); }' % (k, k))
        if count(d, x) == 1 and x != d:
            w('      printf("eq%%d ", x == p);')
            w('      { C%d* back = static_cast<C%d*>(x); printf("dn%%d ", back->m%d); }' % (d, d, d))
        for j in range(1, len(p) - 1):
            mid = p[j]
            if count(mid, x) == 1:
                w('      { C%d* mid = static_cast<C%d*>(x); printf("up%%d ", mid->m%d); }' % (mid, mid, mid))
                break
        if polymorphic(x):
            w('      printf("dv%d ", dynamic_cast<void*>(x) == (void*)p);')
            for y in range(N):
                if r.random() < 0.6 and count(x, y) <= 1:
                    w('      { C%d* y = dynamic_cast<C%d*>(x); printf("d%d:%%d ", y ? y->m%d : -1); }' % (y, y, y, y))
            if r.random() < 0.3:
                w('      printf("t%%d ", typeid(*x) == typeid(C%d));' % d)
        k = r.randrange(N)
        if count(x, k) == 1 and r.random() < 0.5:
            w('      { int C%d::*dm = &C%d::m%d; printf("dm%%d ", x->*dm); }' % (k, k, k))
        if count(d, x) == 1 and r.random() < 0.4:
            w('      { int C%d::*dm = &C%d::m%d; int C%d::*dd = dm; printf("dd%%d ", p->*dd); }' % (x, x, x, d))
        if not abstract(x) and r.random() < 0.3:
            w('      { C%d sl = *x; printf("sl%%d ", sl.m%d); for (int i = 0; i < 1; i++) { int v = sl.m%d; printf("%%d ", v); } }' % (x, x, x))
        w('      printf("\\n");')
        w('    }')
        if pi > 0 and virtualDtor(x) and deleter is None and r.random() < 0.5:
            deleter = p
    if deleter:
        w('    delete %s;' % pathExpr(deleter, 'p'))
    else:
        w('    delete p;')
    w('    printf("\\n");')
    w('  }')
    w('  {')
    w('    C%d local(%d);' % (d, 3 * d))
    w('    printf("\\n");')
    for f in FUNCS:
        if len(lookup(d, f)) == 1:
            w('    { int v = local.%s(7); printf("L%%d ", v); }' % f)
    w('    printf("\\n");')
    w('  }')
    w('  printf("\\n");')
w('  return 0;')
w('}')
print('\n'.join(out))
