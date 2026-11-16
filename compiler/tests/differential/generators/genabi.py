#!/usr/bin/env python3
# Random structs passed by value across the C boundary, both ways, self-checking: a C file (clang or gcc)
# and a kpp file share the type definitions, every function checks its parameters and its result.
# Usage: genabi.py SEED FUNCTIONS OUT_PREFIX  -> OUT_PREFIX.c, OUT_PREFIX.kpp
import random, sys

SCALARS = ['char', 'signed char', 'unsigned char', 'short', 'unsigned short', 'int', 'unsigned', 'long long', 'unsigned long long', 'float', 'double', 'bool', 'void*']
WEIGHTS = [3, 1, 2, 2, 1, 4, 2, 3, 1, 5, 5, 1, 2]

class St:
    def __init__(s, name, union, fields): s.name, s.union, s.fields = name, union, fields  # fields: (name, type, count, bits)

def leaves(t, prefix=''):
    # every (path, scalar type, bits) of a value
    if isinstance(t, St):
        out = []
        for name, ft, count, bits in (t.fields[:1] if t.union else t.fields):
            if name is None: continue
            if count:
                for i in range(count):
                    out += leaves(ft, prefix + '.' + name + '[%d]' % i)
            elif bits:
                out.append((prefix + '.' + name, ft, bits))
            else:
                out += leaves(ft, prefix + '.' + name)
        return out
    return [(prefix, t, 0)]

def tname(t): return t.name if isinstance(t, St) else t

def gen_struct(r, name, known):
    union = r.random() < 0.1
    fields = []
    for i in range(r.randint(1, 3 if union else 6)):
        k = r.random()
        fname = 'f%d' % i
        if k < 0.15 and known:
            fields.append((fname, r.choice(known), 0, 0))
        elif k < 0.3:
            fields.append((fname, r.choices(SCALARS, WEIGHTS)[0], r.randint(1, 4), 0))
        elif k < 0.38 and not union:
            base = r.choice(['unsigned', 'int', 'unsigned char', 'unsigned short', 'unsigned long long'])
            w = {'unsigned': 31, 'int': 30, 'unsigned char': 7, 'unsigned short': 15, 'unsigned long long': 40}[base]
            fields.append((fname, base, 0, r.randint(1, w)))
        else:
            fields.append((fname, r.choices(SCALARS, WEIGHTS)[0], 0, 0))
    return St(name, union, fields)

def struct_text(s):
    body = ' '.join('%s %s%s%s;' % (tname(ft), n, '[%d]' % c if c else '', ' : %d' % b if b else '') for n, ft, c, b in s.fields)
    return 'typedef %s %s { %s } %s;' % ('union' if s.union else 'struct', s.name, body, s.name)

def struct_text_kpp(s):
    body = ' '.join('%s %s%s%s;' % (tname(ft), n, '[%d]' % c if c else '', ' : %d' % b if b else '') for n, ft, c, b in s.fields)
    return '%s %s { %s };' % ('union' if s.union else 'struct', s.name, body)

def value(t, seed, bits):
    v = seed % 100 + 1
    if bits: return '(%s)(%d)' % (t, v % (1 << min(bits - 1, 6)) if bits > 1 else 1)
    if t == 'bool': return '((seed + %d) %% 2 != 0)' % seed
    if t == 'void*': return '(void*)(long long)(seed + %d)' % seed
    return '(%s)(seed + %d)' % (t, seed)

def setter(t, prefix):
    n = t.name if isinstance(t, St) else t.replace(' ', '_').replace('*', 'p')
    lines = ['static %s %s_make_%s(int seed)' % (tname(t), prefix, n), '{', '  (void)seed;']  # (C's and kpp's: a struct of no leaf reads none)
    if isinstance(t, St):
        lines.append('  %s s;' % t.name)
        lines.append('  memset(&s, 0, sizeof(s));')
        for i, (path, lt, bits) in enumerate(leaves(t)):
            lines.append('  s%s = %s;' % (path, value(lt, i, bits)))
        lines.append('  return s;')
    else:
        lines.append('  return %s;' % value(t, 0, 0))
    lines.append('}')
    lines.append('static int %s_same_%s(%s s, int seed)' % (prefix, n, tname(t)))
    lines.append('{')
    lines.append('  (void)s; (void)seed;')
    if isinstance(t, St):
        for i, (path, lt, bits) in enumerate(leaves(t)):
            lines.append('  if (s%s != %s) return 0;' % (path, value(lt, i, bits)))
    else:
        lines.append('  if (s != %s) return 0;' % value(t, 0, 0))
    lines.append('  return 1;')
    lines.append('}')
    return lines

def sname(t): return t.name if isinstance(t, St) else t.replace(' ', '_').replace('*', 'p')

def main():
    seed, count, prefix = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    r = random.Random(seed)
    structs = []
    for i in range(r.randint(10, 24)):
        structs.append(gen_struct(r, 'S%d' % i, structs))
    types = structs + SCALARS
    c = ['#include <stdio.h>', '#include <string.h>', '#include <stdbool.h>']
    k = ['extern "C" int printf(const char* format, ...);', 'extern "C" void* memset(void* p, int v, unsigned long long n);']
    c += [struct_text(s) for s in structs]
    k += [struct_text_kpp(s) for s in structs]
    for t in types:
        c += setter(t, 'c')
        k += setter(t, 'k')
    for s in structs:
        c.append('unsigned long long c_size_%s() { return sizeof(%s) * 100 + _Alignof(%s); }' % (s.name, s.name, s.name))
        c.append('int c_at_%s(const %s* p, int seed) { return c_same_%s(*p, seed); }' % (s.name, s.name, s.name))
        k.append('extern "C" unsigned long long c_size_%s();' % s.name)
        k.append('extern "C" int c_at_%s(const %s* p, int seed);' % (s.name, s.name))
    calls_k, calls_c = [], []
    for s in structs:
        calls_k.append('  if (c_size_%s() != sizeof(%s) * 100 + alignof(%s)) printf("FAIL size %s %%d %%d\\n", (int)sizeof(%s), (int)c_size_%s());' % (s.name, s.name, s.name, s.name, s.name, s.name))
        calls_k.append('  { %s v = k_make_%s(7); if (!c_at_%s(&v, 7)) printf("FAIL layout %s\\n"); }' % (s.name, s.name, s.name, s.name))
    for i in range(count):
        n = r.choice([0, 1, 1, 2, 2, 3, 4, 5, 7, 9])
        ps = [r.choice(structs) if r.random() < 0.7 else r.choices(SCALARS, WEIGHTS)[0] for _ in range(n)]
        ret = r.choice(structs) if r.random() < 0.7 else r.choices(SCALARS + ['void'], WEIGHTS + [3])[0]
        sig = lambda name: '%s %s(%s)' % (tname(ret), name, ', '.join('%s p%d' % (tname(t), j) for j, t in enumerate(ps)) if ps else ('void'))
        sigk = lambda name: '%s %s(%s)' % (tname(ret), name, ', '.join('%s p%d' % (tname(t), j) for j, t in enumerate(ps)))
        def body(pfx, who):
            lines = []
            for j, t in enumerate(ps):
                lines.append('  if (!%s_same_%s(p%d, %d)) printf("FAIL %s%d param %d\\n");' % (pfx, sname(t), j, i + j, who, i, j))
            if ret != 'void':
                lines.append('  return %s_make_%s(%d);' % (pfx, sname(ret), i * 3))
            return lines
        def call(pfx, name, who):
            args = ', '.join('%s_make_%s(%d)' % (pfx, sname(t), i + j) for j, t in enumerate(ps))
            if ret == 'void':
                return ['  %s(%s);' % (name, args)]
            return ['  { %s r = %s(%s); if (!%s_same_%s(r, %d)) printf("FAIL %s%d result\\n"); }' % (tname(ret), name, args, pfx, sname(ret), i * 3, who, i)]
        # C defines cf, kpp calls it; kpp defines kf, C calls it; through a pointer both ways
        c.append(sig('cf%d' % i) + '\n{')
        c += body('c', 'cf')
        c.append('}')
        k.append('extern "C" ' + sigk('cf%d' % i) + ';')
        k.append('extern "C" ' + sigk('kf%d' % i) + '\n{')
        k += body('k', 'kf')
        k.append('}')
        c.append(sig('kf%d' % i) + ';')
        calls_k += call('k', 'cf%d' % i, 'callcf')
        calls_c += call('c', 'kf%d' % i, 'callkf')
    c.append('void c_drive(void)\n{')
    c += calls_c
    c.append('}')
    k.append('extern "C" void c_drive();')
    n = 0
    for start in range(0, len(calls_k), 100):
        k.append('void part%d()\n{' % n)
        k += calls_k[start:start + 100]
        k.append('}')
        n += 1
    k.append('int main()\n{')
    k += ['  part%d();' % i for i in range(n)]
    k.append('  c_drive();\n  printf("done\\n");\n  return 0;\n}')
    open(prefix + '.c', 'w').write('\n'.join(c) + '\n')
    open(prefix + '.kpp', 'w').write('\n'.join(k) + '\n')

main()
