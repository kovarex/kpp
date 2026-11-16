#!/usr/bin/env python3
# Random templates: class templates with a primary and several partial specializations (each one's
# value also names what it deduced, through Name<T>), queried with many argument lists, so the most
# specialized one is chosen as C++ chooses it; function template overloads ordered the same way;
# default template arguments that name earlier parameters; non-type parameters (int, auto, a class
# type); alias templates in patterns and arguments. One source for kpp and C++; definitions and
# queries clang rejects (ambiguous, no match) are dropped. Usage: gentpl.py SEED [SETS] > file
import random, sys, os, re, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get('KPP_DIFF_CXX') or r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'
r = random.Random(int(sys.argv[1]))
NS = int(sys.argv[2]) if len(sys.argv) > 2 else 8

PRELUDE = r'''extern "C" int printf(const char* format, ...);
template<class T> struct Box { T value; };
template<class A, class B> struct Two { A a; B b; };
template<class T> using Ptr = T*;
template<class T> using Same = T;
template<class T> using BoxOf = Box<T>;
template<class T> using PairOf = Two<T, T>;
struct Pt { int x; int y; };
template<class T> struct Name { static constexpr int v = 99; };
template<> struct Name<int> { static constexpr int v = 1; };
template<> struct Name<char> { static constexpr int v = 2; };
template<> struct Name<const int> { static constexpr int v = 3; };
template<> struct Name<int*> { static constexpr int v = 4; };
template<> struct Name<const int*> { static constexpr int v = 5; };
template<> struct Name<Box<int>> { static constexpr int v = 6; };
template<> struct Name<double> { static constexpr int v = 7; };
template<> struct Name<int&> { static constexpr int v = 8; };
template<> struct Name<void> { static constexpr int v = 9; };
template<> struct Name<int**> { static constexpr int v = 10; };
template<> struct Name<Box<int*>> { static constexpr int v = 11; };
template<> struct Name<long long> { static constexpr int v = 12; };
template<> struct Name<unsigned> { static constexpr int v = 13; };
template<> struct Name<bool> { static constexpr int v = 14; };
template<> struct Name<int* const> { static constexpr int v = 15; };
template<> struct Name<Two<int, char>> { static constexpr int v = 16; };
template<> struct Name<const char*> { static constexpr int v = 17; };
template<> struct Name<short> { static constexpr int v = 18; };
template<> struct Name<long long*> { static constexpr int v = 19; };
template<> struct Name<const char> { static constexpr int v = 20; };
template<class T> concept Small = sizeof(T) <= 4;
template<class T> concept Tiny = Small<T> && sizeof(T) <= 2;
template<class T> concept Deref = requires(T t) { *t; };
template<class T> concept SmallDeref = Deref<T> && requires(T t) { { *t } -> Small; };
'''

ARGS = ['int', 'char', 'const int', 'int*', 'const int*', 'int* const', 'int**', 'Box<int>', 'Box<int*>', 'Box<char>', 'Box<Box<int>>', 'double',
        'int&', 'const int&', 'Two<int, char>', 'Two<int, int>', 'Two<char, int>', 'void', 'long long', 'unsigned', 'bool', 'const char*',
        'Ptr<int>', 'Same<int>', 'BoxOf<int>', 'PairOf<int>', 'Ptr<const int>', 'int[3]', 'int[]', 'void(*)(int)', 'int(char)', 'short', 'volatile int']

# Two-parameter type patterns: (template header params, the specialization's arguments, the value's names)
PAT2 = [
    ('class T, class U', 'T*, U', 'T'), ('class T, class U', 'T, U*', 'U'), ('class T', 'T, T', 'T'), ('class T', 'T*, T', 'T'),
    ('class T', 'T, T*', 'T'), ('class T, class U', 'const T, U', 'T'), ('class T, class U', 'T&, U', 'T'), ('class T', 'T*, T*', 'T'),
    ('class T, class U', 'Box<T>, U', 'T'), ('class T, class U', 'T, Box<U>', 'U'), ('class T', 'Box<T>, Box<T>', 'T'),
    ('class T', 'Box<T>, T', 'T'), ('class T, class U', 'Two<T, U>, T', 'U'), ('class T', 'Two<T, T>, T', 'T'), ('class U', 'int, U', 'U'),
    ('class T', 'T, int', 'T'), ('class T', 'const T*, T', 'T'), ('class T', 'T* const, T', 'T'), ('class T, class U', 'Ptr<T>, U', 'T'),
    ('class T', 'BoxOf<T>, PairOf<T>', 'T'), ('class T, int N', 'T[N], T', 'T'), ('class T, class U', 'T[], U', 'T'),
    ('class R, class A', 'R(*)(A), A', 'R'), ('class R, class A', 'R(A), R', 'A'), ('class T, class U', 'T**, U', 'T'),
    ('class T, class U', 'const T&, U', 'T'), ('class T, class U', 'Box<T*>, U', 'T'), ('class T', 'Box<const T>, T', 'T'),
    ('class T, class U', 'volatile T, U', 'T'), ('class T', 'T, const T', 'T'), ('', 'int, int', ''), ('', 'int*, char', ''),
]
PAT1 = [  # variadic primary: template<class... Ts> struct V
    ('class T', 'T', 'T'), ('class T, class... Ts', 'T, Ts...', 'T'), ('class T', 'T, T', 'T'), ('class... Ts', 'int, Ts...', ''),
    ('class T, class... Ts', 'T*, Ts...', 'T'), ('class T, class U, class... Ts', 'T, U, Ts...', 'U'), ('class T', 'T, T, T', 'T'),
    ('class... Ts', 'Box<Ts>...', ''), ('class T, class... Ts', 'Box<T>, Ts...', 'T'), ('', '', ''), ('class... Ts', 'Ts*...', ''),
    ('class T, class... Ts', 'T, T, Ts...', 'T'), ('class T', 'T, int', 'T'),
]

sets = []   # (definition lines, query lines)

def valueOf(k, names):
    parts = [str(k * 100)]
    for n in names.split(',') if names else []:
        parts.append('Name<%s>::v' % n.strip())
    if len(parts) > 1:
        return '%s + %s' % (parts[0], ' * 1000 + '.join(parts[1:]) if len(parts) > 2 else parts[1])
    return parts[0]

def classSet(si):
    name = 'S%d' % si
    defs = ['template<class A, class B> struct %s { static int v() { return 0; } };' % name]
    used = set()
    for k in range(1, r.randint(2, 6)):
        hdr, args, names = r.choice(PAT2)
        if args in used: continue
        used.add(args)
        defs.append('template<%s> struct %s<%s> { static int v() { return %s; } };' % (hdr, name, args, valueOf(k, names)))
    queries = set()
    for _ in range(14):
        queries.add('%s<%s, %s>::v()' % (name, r.choice(ARGS), r.choice(ARGS)))
    return defs, sorted(queries)

def variadicSet(si):
    name = 'V%d' % si
    defs = ['template<class... Ts> struct %s { static int v() { return sizeof...(Ts); } };' % name]
    used = set()
    for k in range(1, r.randint(2, 6)):
        hdr, args, names = r.choice(PAT1)
        if args in used: continue
        used.add(args)
        size = ' + (int)sizeof...(Ts) * 10' if 'Ts' in hdr else ''
        defs.append('template<%s> struct %s<%s> { static int v() { return %s%s; } };' % (hdr, name, args, valueOf(k, names), size))
    queries = set()
    for _ in range(12):
        queries.add('%s<%s>::v()' % (name, ', '.join(r.choice(ARGS[:22]) for _ in range(r.randint(1, 4)))))
    return defs, sorted(queries)

def nttpSet(si):
    c = r.randint(0, 3)
    name = 'N%d' % si
    if c == 0:
        defs = ['template<int N, class T> struct %s { static int v() { return N * 10 + Name<T>::v; } };' % name,
                'template<class T> struct %s<0, T> { static int v() { return 5000 + Name<T>::v; } };' % name,
                'template<int N> struct %s<N, int> { static int v() { return 6000 + N; } };' % name,
                'template<int N, class T> struct %s<N, T*> { static int v() { return 7000 + N * 10 + Name<T>::v; } };' % name]
        if r.random() < 0.5: defs.append('template<> struct %s<0, int> { static int v() { return 8000; } };' % name)
        queries = set('%s<%d, %s>::v()' % (name, r.choice([0, 1, 2, 7, -3]), r.choice(ARGS[:22])) for _ in range(10))
    elif c == 1:
        defs = ['template<auto V> struct %s { static int v() { return 1; } };' % name,
                'template<int I> struct %s<I> { static int v() { return 1000 + I; } };' % name,
                'template<char C> struct %s<C> { static int v() { return 2000 + C; } };' % name,
                'template<bool B> struct %s<B> { static int v() { return 3000 + B; } };' % name]
        if r.random() < 0.5: defs.append('template<unsigned U> struct %s<U> { static int v() { return 4000 + (int)U; } };' % name)
        if r.random() < 0.5: defs.append('template<> struct %s<5> { static int v() { return 9005; } };' % name)
        queries = set('%s<%s>::v()' % (name, r.choice(['5', '7', "'a'", 'true', 'false', '5u', '3LL', '(short)2', '(char)66', 'nullptr', '-1'])) for _ in range(9))
    elif c == 2:
        defs = ['template<Pt P> struct %s { static int v() { return P.x * 10 + P.y; } };' % name,
                'template<Pt P, int K> int %sf() { return %s<P>::v() * 100 + K; }' % (name, name)]
        queries = set()
        for _ in range(6):
            x, y = r.randint(0, 9), r.randint(0, 9)
            queries.add(r.choice(['%s<Pt{%d, %d}>::v()' % (name, x, y), '%sf<Pt{%d, %d}, %d>()' % (name, x, y, r.randint(0, 9))]))
    else:
        defs = ['template<class T, class U = T*, int N = sizeof(U)> struct %s { static int v() { return Name<T>::v * 1000 + Name<U>::v * 10 + N; } };' % name,
                'template<class T> struct %s<T, T> { static int v() { return 7000 + Name<T>::v; } };' % name,
                'template<class T = int, class U = Box<T>> int %sf([[maybe_unused]] T a = T()) { return Name<T>::v * 100 + Name<U>::v; }' % name]
        queries = set()
        for _ in range(10):
            k = r.randint(0, 4)
            a = r.choice(ARGS[:22]); b = r.choice(ARGS[:22])
            if k == 0: queries.add('%s<%s>::v()' % (name, a))
            elif k == 1: queries.add('%s<%s, %s>::v()' % (name, a, b))
            elif k == 2: queries.add('%s<%s, %s, %d>::v()' % (name, a, b, r.randint(0, 9)))
            elif k == 3: queries.add('%sf()' % name)
            else: queries.add('%sf<%s>()' % (name, r.choice(['int', 'char', 'double', 'int*', 'bool'])))
    return defs, sorted(queries)

FPAT = ['T', 'T*', 'const T*', 'T&', 'const T&', 'Box<T>', 'Box<T*>', 'T**', 'Two<T, T>', 'Two<T, int>', 'T* const&', 'Ptr<T>', 'BoxOf<T>&']
FARGS = ['i', 'pi', 'cpi', 'ci', 'bi', 'bpi', 'ppi', 'ti', 'tic', '&i', '&pi', 'cbi', '(int*)nullptr', '3', "'c'", 'Box<int>{4}']

def funcSet(si):
    name = 'f%d' % si
    defs, used = [], set()
    for k in range(1, r.randint(3, 6)):
        p = r.choice(FPAT)
        if p in used: continue
        used.add(p)
        defs.append('template<class T> int %s([[maybe_unused]] %s x) { return %d + Name<T>::v; }' % (name, p, k * 100))
    if r.random() < 0.3:
        defs.append('int %s([[maybe_unused]] int* x) { return 900; }' % name)
    queries = sorted(set('%s(%s)' % (name, r.choice(FARGS)) for _ in range(10)))
    return defs, queries

def varSet(si):
    # a variable template with partial specializations, chosen as a class template's are
    name = 'W%d' % si
    defs = ['template<class A, class B> constexpr int %s = 0;' % name]
    used = set()
    for k in range(1, r.randint(2, 6)):
        hdr, args, names = r.choice(PAT2)
        if args in used or not hdr: continue
        used.add(args)
        defs.append('template<%s> constexpr int %s<%s> = %s;' % (hdr, name, args, valueOf(k, names)))
    queries = set('%s<%s, %s>' % (name, r.choice(ARGS), r.choice(ARGS)) for _ in range(12))
    return defs, sorted(queries)

TTPAT = [
    ('template<class> class TT, class T', 'TT<T>', '100 + Name<T>::v'),
    ('template<class, class> class TT, class A, class B', 'TT<A, B>', '200 + Name<A>::v * 1000 + Name<B>::v'),
    ('class T', 'Box<T>', '300 + Name<T>::v'),
    ('template<class...> class TT, class... Ts', 'TT<Ts...>', '400 + (int)sizeof...(Ts) * 10'),
    ('template<class> class TT, class T', 'TT<T*>', '500 + Name<T>::v'),
    ('template<class, class> class TT, class T', 'TT<T, T>', '600 + Name<T>::v'),
    ('class A, class B', 'Two<A, B>', '700 + Name<A>::v * 1000 + Name<B>::v'),
    ('template<class> class TT', 'TT<int>', '800'),
    ('template<class> class TT, class T', 'TT<TT<T>>', '900 + Name<T>::v'),
]
TTARGS = ['int', 'Box<int>', 'Box<int*>', 'Box<char>', 'Box<Box<int>>', 'Two<int, char>', 'Two<int, int>', 'Two<char, int>', 'Name<int>',
          'Name<int*>', 'BoxOf<char>', 'PairOf<int>', 'Two<Box<int>, int>', 'Box<Two<int, int>>', 'Name<Name<int>>', 'int*']

def ttSet(si):
    # template template parameters: a partial specialization matching any class template's
    # instance, and function templates taking one, ordered against the fixed ones
    name = 'T%d' % si
    used = set()
    if r.random() < 0.6:
        defs = ['template<class X> struct %s { static int v() { return 0; } };' % name]
        for _ in range(r.randint(2, 5)):
            hdr, args, val = r.choice(TTPAT)
            if args in used: continue
            used.add(args)
            defs.append('template<%s> struct %s<%s> { static int v() { return %s; } };' % (hdr, name, args, val))
        queries = set('%s<%s>::v()' % (name, r.choice(TTARGS)) for _ in range(10))
    else:
        defs = []
        for _ in range(r.randint(2, 5)):
            hdr, args, val = r.choice(TTPAT)
            if args in used: continue
            used.add(args)
            defs.append('template<%s> int %sf(const %s&) { return %s; }' % (hdr, name, args, val))
        defs.append('template<class X> int %sf(const X&) { return 1; }' % name)
        queries = set('%sf(%s{})' % (name, r.choice(TTARGS[1:15])) for _ in range(10))
    return defs, sorted(queries)

CPAT = [
    ('Small T', 'T', '100'), ('Tiny T', 'T', '200'), ('Deref T', 'T', '300'), ('SmallDeref T', 'T', '400'),
    ('class T', 'T*', '500 + Name<T>::v'), ('Small T', 'T*', '600 + Name<T>::v'), ('Tiny T', 'T*', '700 + Name<T>::v'),
    ('Small T', 'Box<T>', '800 + Name<T>::v'), ('Tiny T', 'Box<T>', '900 + Name<T>::v'), ('class T', 'Box<T>', '1000 + Name<T>::v'),
    ('class T> requires Small<T> && Deref<T*', 'T', '1100'), ('class T> requires Tiny<T> || Deref<T>', 'T', '1200'),
]
CARGS = ['int', 'char', 'short', 'double', 'long long', 'int*', 'char*', 'const char*', 'long long*', 'Box<int>', 'Box<char>', 'Box<double>',
         'Box<short>', 'Two<char, char>', 'bool', 'int**', 'Box<int*>', 'const int', 'const char']
CVALS = ['i', "'c'", '(short)2', '2.5', '3LL', 'pi', 'cpi', 'ppi', 'bi', 'Box<char>{1}', 'Box<short>{2}', 'true', '"s"', 'Box<double>{1}']

def conceptSet(si):
    # constrained partial specializations and overloads: the one whose constraints subsume the
    # other's ([temp.func.order] 6.2.2), patterns first
    name = 'C%d' % si
    used = set()
    if r.random() < 0.6:
        defs = ['template<class T> struct %s { static int v() { return 0; } };' % name]
        for _ in range(r.randint(2, 5)):
            hdr, args, val = r.choice(CPAT)
            if (hdr, args) in used: continue
            used.add((hdr, args))
            defs.append('template<%s> struct %s<%s> { static int v() { return %s; } };' % (hdr, name, args, val))
        queries = set('%s<%s>::v()' % (name, r.choice(CARGS)) for _ in range(10))
    else:
        defs = ['template<class T> int %sf(T) { return 1; }' % name]
        for _ in range(r.randint(2, 5)):
            hdr, args, val = r.choice(CPAT)
            if (hdr, args) in used: continue
            used.add((hdr, args))
            defs.append('template<%s> int %sf(%s) { return %s; }' % (hdr, name, args, val))
        queries = set('%sf(%s)' % (name, r.choice(CVALS)) for _ in range(10))
    return defs, sorted(queries)

GUIDES = [
    'template<class U> %s(U) -> %s<U>;', 'template<class U> %s(U*) -> %s<const U*>;', 'template<class U> %s(Box<U>) -> %s<U>;',
    '%s(int) -> %s<long long>;', 'template<class U> %s(const U*) -> %s<U>;', 'template<Small U> %s(U) -> %s<Box<U>>;',
    'template<class U> %s(U**) -> %s<U>;', '%s(const char*) -> %s<char>;', 'template<class A, class B> %s(Two<A, B>) -> %s<B>;',
]

def ctadSet(si):
    # class template argument deduction through deduction guides only (the constructor deduces
    # nothing): the guides overload and order as function templates do
    name = 'D%d' % si
    defs = ['template<class T> struct %s { int k; template<class U> %s(U) : k(100 + Name<T>::v) {} };' % (name, name)]
    used = set()
    for _ in range(r.randint(2, 5)):
        g = r.choice(GUIDES)
        if g in used: continue
        used.add(g)
        defs.append(g % (name, name))
    queries = set('%s(%s).k' % (name, r.choice(FARGS + ['"s"', '2.5', '(short)3', 'Two<int, char>{1, 2}'])) for _ in range(10))
    return defs, sorted(queries)

for s in range(NS):
    kind = r.choice([classSet, classSet, variadicSet, nttpSet, funcSet, varSet, ttSet, ttSet, conceptSet, conceptSet, ctadSet, ctadSet])
    d, q = kind(s)
    sets.append([d, q])

def render():
    lines = PRELUDE.split('\n')
    where = {}
    for si, (defs, qs) in enumerate(sets):
        if not defs: continue
        for dl in defs:
            where[len(lines) + 1] = ('def', si)
            lines.append(dl)
    lines.append('int main()')
    lines.append('{')
    lines.append('  int i = 1; int* pi = &i; const int* cpi = &i; const int ci = 2; Box<int> bi{3}; const Box<int> cbi{4}; Box<int*> bpi{pi}; int** ppi = &pi;')
    lines.append('  Two<int, int> ti{5, 6}; Two<int, char> tic{7, 8};')
    lines.append('  (void)i; (void)pi; (void)cpi; (void)ci; (void)bi; (void)cbi; (void)bpi; (void)ppi; (void)ti; (void)tic;')  # (each query names some)
    for si, (defs, qs) in enumerate(sets):
        if not defs: continue
        lines.append('  printf("s%d:");' % si)
        for qi, q in enumerate(qs):
            where[len(lines) + 1] = ('q', si, qi)
            lines.append('  { int r = %s; printf(" %%d", r); }' % q)
        lines.append('  printf("\\n");')
    lines.append('  return 0;')
    lines.append('}')
    return '\n'.join(lines), where

head = open(os.path.join(HERE, 'cxx-head.h')).read()
headLines = head.count('\n')
tmp = os.path.join(os.environ.get('KPP_DIFF_TMP', HERE), 'tpltmp-%s.cpp' % sys.argv[1])
for it in range(8):
    text, where = render()
    open(tmp, 'w').write(head + text + '\n')
    p = subprocess.run([CLANG, '-std=c++20', '-Wno-everything', '-Werror=writable-strings', '-fsyntax-only', '-ferror-limit=0', '-fno-ms-compatibility',
                        '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', tmp], capture_output=True, text=True)
    # an error inside an instantiation belongs to the query whose note says it was requested there
    errs, cur = set(), None
    for m in re.finditer(r'tpltmp-[^:]*\.cpp:(\d+):\d+: (error|note|warning)', p.stderr):
        line, kind = int(m.group(1)) - headLines, m.group(2)
        if kind == 'error':
            if cur is not None: errs.add(cur)
            cur = line
        elif cur is not None and where.get(line, ('',))[0] == 'q':
            cur = line
    if cur is not None: errs.add(cur)
    if not errs: break
    dropQ, dropS = set(), set()
    for e in errs:
        w = where.get(e)
        if not w:
            sys.stderr.write('unplaced error at %d\n%s' % (e, p.stderr[:3000])); sys.exit(1)
        if w[0] == 'def': dropS.add(w[1])
        else: dropQ.add((w[1], w[2]))
    for si in dropS: sets[si][0] = []
    for si, qi in sorted(dropQ, reverse=True):
        if qi < len(sets[si][1]): del sets[si][1][qi]
os.remove(tmp)
print(render()[0])
