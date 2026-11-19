#!/usr/bin/env python3
# Random aggregates and lifetimes: structs of ints, doubles, chars, arrays, nested structs, a base,
# a traced class and default member initializers, initialized by brace lists (partial, brace
# elision) and designated initializers, each printed member by member; then lifetime blocks:
# temporaries bound to references (local, member of an aggregate, through a cast, a conditional,
# a member of a temporary, arrays), destruction order of members, arrays and bases, and copy
# elision. One source for kpp and C++; what clang rejects is dropped. Usage: geninit.py SEED [N] > file
import random, sys, os, re, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get('KPP_DIFF_CXX') or r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'
r = random.Random(int(sys.argv[1]))
NB = int(sys.argv[2]) if len(sys.argv) > 2 else 24

PRELUDE = r'''extern "C" int printf(const char* format, ...);
struct T
{
  int id;
  IMPLICIT T(int i = 0) : id(i) { printf("+%d ", id); } // (an aggregate member copy-initialised from an int: `Arr{T(1), 2}`)
  T(const T& o) : id(o.id + 100) { printf("c%d ", id); }
  T(T&& o) : id(o.id + 200) { o.id = -o.id; printf("m%d ", id); }
  T& operator=(const T& o) { printf("=%d/%d ", id, o.id); id = o.id; return *this; }
  ~T() { printf("-%d ", id); }
};
void print(int v) { printf("%d ", v); }
void print(double v) { printf("%.2f ", v); }
void print(char v) { printf("'%d' ", (int)v); }
void print(bool v) { printf("%s ", v ? "t" : "f"); }
void print(const T& v) { printf("T%d ", v.id); }
template<class X, int N> void print(const X (&a)[N]) { printf("["); for (int i = 0; i < N; ++i) print(a[i]); printf("] "); }
T make(int i) { return T(i); }
struct Wrap { T t; T get() const { return t; } const T& ref() const { return t; } };
Wrap makeWrap(int i) { return Wrap{T(i)}; }
struct RefHolder { const T& r; int n; };
struct RRefHolder { T&& r; };
struct Two { T a; T b; };
struct Derived2 : Two { T c; };
struct Arr { T a[3]; };
Arr makeArr(int i) { return Arr{T(i), T(i + 1), T(i + 2)}; }
union U { int i; double d; };
struct Boom { int v; Boom(int x) : v(x) { if (x > 0) throw x; } };
struct TB { T a; Boom b; T c; };
T throwT(int x) { if (x > 0) throw x; return T(x); }
struct DM { T a{1}; T b{a.id + 10}; int n = b.id * 2; };
struct Solo { T z; };
struct MB : Two, Solo { T w; };
int dflt(int k, const T& t = T(77)) { return t.id + k; }
int counted(int k) { static T s(500); return s.id + k; }
'''

FIELD_KINDS = ['int', 'int', 'double', 'char', 'bool', 'T', 'T', 'arr', 'nested', 'carr']

class Struct:
    def __init__(s, name, fields, base):
        s.name, s.fields, s.base = name, fields, base   # fields: (type text, name, kind, extra, default or None)

structs = []

def newStruct(i):
    base = r.choice(structs) if structs and r.random() < 0.2 else None
    fields = []
    for j in range(r.randint(1, 5)):
        k = r.choice(FIELD_KINDS)
        if k == 'nested' and not structs: k = 'int'
        if k == 'arr':
            et = r.choice(['int', 'T', 'double'])
            n = r.randint(1, 3)
            fields.append((et, 'f%d' % j, 'arr', (et, n), None))
        elif k == 'carr':
            fields.append(('char', 'f%d' % j, 'carr', 6, None))
        elif k == 'nested':
            sub = r.choice(structs)
            fields.append((sub.name, 'f%d' % j, 'nested', sub, None))
        else:
            d = None
            if r.random() < 0.3:
                d = {'int': str(r.randint(1, 9)), 'double': '1.5', 'char': "'z'", 'bool': 'true', 'T': 'T(%d)' % r.randint(50, 59)}[k]
            fields.append((k, 'f%d' % j, k, None, d))
    s = Struct('A%d' % i, fields, base)
    structs.append(s)
    return s

def structText(s):
    lines = ['struct %s%s' % (s.name, ' : %s' % s.base.name if s.base else ''), '{']
    for t, n, k, extra, d in s.fields:
        if k == 'arr': lines.append('  %s %s[%d];' % (extra[0], n, extra[1]))
        elif k == 'carr': lines.append('  char %s[%d];' % (n, extra))
        else: lines.append('  %s %s%s;' % (t, n, (' = %s' % d) if d else ''))
    lines.append('};')
    body = []
    if s.base: body.append('print(static_cast<const %s&>(v));' % s.base.name)
    for t, n, k, extra, d in s.fields:
        if k == 'carr': body.append('printf("\\"%%s\\" ", v.%s);' % n)
        else: body.append('print(v.%s);' % n)
    lines.append('void print(const %s& v) { printf("{ "); %s printf("} "); }' % (s.name, ' '.join(body)))
    return lines

def scalar(k, depth):
    if k == 'int': return str(r.randint(-9, 99))
    if k == 'double': return r.choice(['2.25', '-1.5', '3.0', '0.125'])
    if k == 'char': return r.choice(["'a'", "'B'", "65", "'0'"])
    if k == 'bool': return r.choice(['true', 'false'])
    if k == 'T':
        return r.choice(['T(%d)' % r.randint(1, 49), 'make(%d)' % r.randint(1, 49), str(r.randint(1, 49)), '{%d}' % r.randint(1, 49), '{}'])
    return '0'

def fieldInit(f, depth):
    t, n, k, extra, d = f
    if k == 'arr':
        et, cnt = extra
        m = r.randint(0, cnt)
        return '{%s}' % ', '.join(scalar(et, depth) for _ in range(m))
    if k == 'carr': return r.choice(['"abc"', '"hello"', '{}', "{'x', 'y'}", '""'])
    if k == 'nested': return init(extra, depth + 1)
    return scalar(k, depth)

def init(s, depth=0):
    # a brace list for s: positional (maybe partial), designated (C++20, in order) when s has no base
    c = r.randint(0, 3)
    if c == 0: return '{}'
    if c == 1 and not s.base:
        chosen = [f for f in s.fields if r.random() < 0.6]
        return '{%s}' % ', '.join('.%s = %s' % (f[1], fieldInit(f, depth)) for f in chosen)
    items = []
    if s.base: items.append(init(s.base, depth + 1) if r.random() < 0.8 else '{}')
    m = r.randint(0, len(s.fields))
    for f in s.fields[:m]: items.append(fieldInit(f, depth))
    return '{%s}' % ', '.join(items)

def aggregateBlock(i):
    s = r.choice(structs)
    form = r.randint(0, 4)
    lines = ['{', '  printf("agg%d ");' % i]
    if form == 0: lines.append('  %s v%s;' % (s.name, init(s)))
    elif form == 1: lines.append('  %s v = %s;' % (s.name, init(s)))
    elif form == 2: lines.append('  const %s& v = %s%s;' % (s.name, s.name, init(s)))
    elif form == 3:
        lines.append('  %s arr[2] = {%s, %s};' % (s.name, init(s), init(s)))
        lines.append('  const %s& v = arr[1]; print(arr[0]);' % s.name)
    else:
        lines.append('  auto mk = []() { return %s%s; };' % (s.name, init(s)))
        lines.append('  %s v = mk();' % s.name)
    lines.append('  printf("| "); print(v); printf("| ");')
    lines.append('}')
    lines.append('printf("\\n");')
    return lines

def lifetimeBlock(i):
    a, b = r.randint(1, 9), r.randint(10, 19)
    c = r.choice(['true', 'false'])
    bodies = [
        ['const T& x = T(%d);' % a, 'printf("| %d ", x.id);'],
        ['T&& x = make(%d);' % a, 'x.id += 1; printf("| %d ", x.id);'],
        ['const T& x = %s ? T(%d) : make(%d);' % (c, a, b), 'printf("| %d ", x.id);'],
        ['const T& x = makeWrap(%d).t;' % a, 'printf("| %d ", x.id);'],
        ['const T& x = static_cast<const T&>(T(%d));' % a, 'printf("| %d ", x.id);'],
        ['RefHolder h{T(%d), %d};' % (a, b), 'printf("| %d %d ", h.r.id, h.n);'],
        ['RefHolder h = {make(%d), %d};' % (a, b), 'printf("| %d %d ", h.r.id, h.n);'],
        ['RRefHolder h{T(%d)};' % a, 'printf("| %d ", h.r.id);'],
        ['T t = T(T(T(%d)));' % a, 'printf("| %d ", t.id);'],
        ['T t = %s ? T(%d) : make(%d);' % (c, a, b), 'printf("| %d ", t.id);'],
        ['Two two{make(%d), T(%d)};' % (a, b), 'printf("| %d %d ", two.a.id, two.b.id);'],
        ['Derived2 d{{T(%d), %d}, make(%d)};' % (a, b, a + 20), 'printf("| %d %d %d ", d.a.id, d.b.id, d.c.id);'],
        ['Arr arr{T(%d), %d};' % (a, b), 'printf("| %d %d %d ", arr.a[0].id, arr.a[1].id, arr.a[2].id);'],
        ['const T& x = Arr{T(%d), %d}.a[1];' % (a, b), 'printf("| %d ", x.id);'],
        ['int n = makeWrap(%d).get().id;' % a, 'n += make(%d).id;' % b, 'printf("| %d ", n);'],
        ['const T& x = makeWrap(%d).ref();' % a, 'printf("| dangling-free %d ", 0);'],
        ['for (const T& x : Arr{T(%d), %d}.a)' % (a, b), '  printf("<%d> ", x.id);'],
        ['for (const T& x : makeArr(%d).a)' % a, '  printf("<%d> ", x.id);'],
        ['if (const T& x = make(%d); x.id %% 2 == 1)' % a, '  printf("odd %d ", x.id);', 'else', '  printf("even %d ", x.id);'],
        ['switch (T t = T(%d); t.id %% 3)' % a, '{', 'case 0: printf("z%d ", t.id); break;', 'default: printf("nz%d ", t.id); break;', '}'],
        ['U u{.d = 2.5};', 'printf("| %.1f ", u.d);'],
        ['int n = (make(%d), make(%d)).id;' % (a, b), 'printf("| %d ", n);'],
        ['const T& x = (make(%d), T(%d));' % (a, b), 'printf("| %d ", x.id);'],
        ['try { TB tb{T(%d), %d, T(%d)}; printf("built "); } catch (int e) { printf("caught %%d ", e); }' % (a, r.choice([0, 3]), b)],
        ['try { T arr[3] = {T(%d), make(%d), throwT(%d)}; printf("built "); } catch (int e) { printf("caught %%d ", e); }' % (a, b, r.choice([0, 4]))],
        ['try { Two two{make(%d), throwT(%d)}; printf("built %%d ", two.a.id); } catch (int e) { printf("caught %%d ", e); }' % (a, r.choice([0, 5]))],
        ['bool ok = make(%d).id > 0 && T(%d).id > 0;' % (a, b), 'printf("| %d ", ok);'],
        ['bool ok = make(%d).id < 0 || make(%d).id > 0;' % (a, b), 'printf("| %d ", ok);'],
        ['int n = %s ? make(%d).id : T(%d).id;' % (c, a, b), 'printf("| %d ", n);'],
        ['int n = dflt(%d);' % a, 'n += dflt(%d, T(%d));' % (a, b), 'printf("| %d ", n);'],
        ['int n = counted(%d);' % a, 'n += counted(%d);' % b, 'printf("| %d ", n);'],
        ['const T& x = Wrap{T(%d)}.*(&Wrap::t);' % a, 'printf("| %d ", x.id);'],
        ['MB mb{{T(%d), make(%d)}, {T(%d)}, T(%d)};' % (a, b, a + 30, b + 30), 'printf("| %d %d %d %d ", mb.a.id, mb.b.id, mb.z.id, mb.w.id);'],
        ['DM dm{T(%d)};' % a, 'printf("| %d %d %d ", dm.a.id, dm.b.id, dm.n);'],
        ['DM dm{};', 'printf("| %d %d %d ", dm.a.id, dm.b.id, dm.n);'],
        ['T arr[3] = {T(%d)};' % a, 'printf("| %d %d %d ", arr[0].id, arr[1].id, arr[2].id);'],
        ['Two* p = new Two{T(%d), make(%d)};' % (a, b), 'printf("| %d %d ", p->a.id, p->b.id);', 'delete p;'],
        ['Arr* p = new Arr{T(%d)};' % a, 'printf("| %d %d ", p->a[0].id, p->a[2].id);', 'delete p;'],
        ['int i = 0;', 'while (make(i).id < 2) ++i;', 'printf("| %d ", i);'],
        ['T t = make(make(%d).id + 1);' % a, 'printf("| %d ", t.id);'],
    ]
    k = r.randrange(len(bodies))
    body = bodies[k]
    if k == 15: body = ['int n = makeWrap(%d).ref().id;' % a, 'printf("| %d ", n);']
    return ['{', '  printf("life%d ");' % i] + ['  ' + l for l in body] + ['  printf("; ");', '}', 'printf("\\n");']

for i in range(r.randint(3, 7)):
    newStruct(i)
blocks = []
for i in range(NB):
    blocks.append(aggregateBlock(i) if r.random() < 0.6 else lifetimeBlock(i))

def render():
    lines = PRELUDE.split('\n')
    where = {}
    for s in structs:
        lines += structText(s)
    for bi, b in enumerate(blocks):
        if not b: continue
        where[len(lines) + 1] = bi
        lines.append('void block%d()' % bi)
        lines.append('{')
        for l in b:
            where[len(lines) + 1] = bi
            lines.append('  ' + l)
        lines.append('}')
    lines.append('int main()')
    lines.append('{')
    for bi, b in enumerate(blocks):
        if b: lines.append('  block%d();' % bi)
    lines.append('  return 0;')
    lines.append('}')
    return '\n'.join(lines), where

head = open(os.path.join(HERE, 'cxx-head.h')).read()
headLines = head.count('\n')
tmp = os.path.join(os.environ.get('KPP_DIFF_TMP', HERE), 'inittmp-%s.cpp' % sys.argv[1])
for it in range(8):
    text, where = render()
    open(tmp, 'w').write(head + text + '\n')
    p = subprocess.run([CLANG, '-std=c++20', '-Wno-everything', '-Werror=writable-strings', '-Werror=dangling', '-fsyntax-only', '-ferror-limit=0', '-fno-ms-compatibility',
                        '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', tmp], capture_output=True, text=True)
    errs, cur = set(), None
    for m in re.finditer(r'inittmp-[^:]*\.cpp:(\d+):\d+: (error|note|warning)', p.stderr):
        line, kind = int(m.group(1)) - headLines, m.group(2)
        if kind == 'error':
            if cur is not None: errs.add(cur)
            cur = line
        elif cur is not None and line in where:
            cur = line
    if cur is not None: errs.add(cur)
    if not errs: break
    for e in errs:
        if e in where: blocks[where[e]] = None
        else: sys.stderr.write('unplaced error at %d\n%s' % (e, p.stderr[:3000])); sys.exit(1)
os.remove(tmp)
print(render()[0])
