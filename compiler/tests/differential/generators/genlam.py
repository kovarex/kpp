#!/usr/bin/env python3
# Random lambdas: captures by value and by reference, init-captures, mutable state and copies of it,
# generic lambdas (their result type shown through an overload set), lambdas in templates, nested
# lambdas, `this` captures, recursion through Function, conversion to a function pointer, closures
# owning a traced class. One source for kpp and C++ (cxx-head.h gives the C++ side Function and move);
# blocks clang rejects are dropped. Usage: genlam.py SEED [BLOCKS] > file
import random, sys, os, re, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get('KPP_DIFF_CXX') or r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'
r = random.Random(int(sys.argv[1]))
NB = int(sys.argv[2]) if len(sys.argv) > 2 else 30

PRELUDE = r'''extern "C" int printf(const char* format, ...);
struct T
{
  int id;
  T(int i = 0) : id(i) { printf("+%d ", id); }
  T(const T& o) : id(o.id + 100) { printf("c%d ", id); }
  T(T&& o) : id(o.id + 200) { o.id = -o.id; printf("m%d ", id); }
  T& operator=(const T& o) { printf("=%d/%d ", id, o.id); id = o.id; return *this; }
  ~T() { printf("-%d ", id); }
  int val() const { return id; }
};
void show(int v) { printf("i%d ", v); }
void show(unsigned v) { printf("u%u ", v); }
void show(long long v) { printf("L%lld ", v); }
void show(unsigned long long v) { printf("U%llu ", v); }
void show(double v) { printf("d%.3f ", v); }
void show(float v) { printf("f%.3f ", (double)v); }
void show(char v) { printf("c%d ", (int)v); }
void show(bool v) { printf("b%d ", (int)v); }
void show(short v) { printf("s%d ", (int)v); }
void show(const T& v) { printf("T%d ", v.id); }
template<class F> int apply1(F f, int x) { return f(x); }
template<class F> int applyRef(F& f, int x) { return f(x); }
template<class F> int twice(F f, int x) { return f(f(x)); }
template<class F, class... A> auto callWith(F f, A... a) { return f(a...); }
template<class T> auto adder(T k) { return [k](T v) { return v + k; }; }
template<class T> auto counter(T start) { return [start]() mutable { start = start + 1; return start; }; }
template<class T> T sumWith(T a, T b) { auto l = [&](T c) { return a + b + c; }; return l(a); }
template<class T, class F> T fold3(T a, T b, T c, F f) { return f(f(a, b), c); }
template<int N> int scaled(int x) { auto l = [x](int y) { return x * N + y; }; return l(N); }
template<class F> auto compose(F f) { return [f](int x) { return f(x) + 1; }; }
int callFp(int (*fp)(int), int x) { return fp(x); }
int callFn(const Function<int(int)>& f, int x) { return f(x); }
struct S
{
  int v = 0;
  int run(int k) { auto l = [this](int d) { v += d; return v; }; l(k); return l(1); }
  int all(int k) { auto l = [&](int d) { v = v * 2 + d + k; return v; }; return l(3); }
  int copied(int k) { auto l = [s = *this](int d) mutable { s.v += d; return s.v; }; int a = l(k); return a * 100 + v; }
  auto getter() { return [this]() { return v; }; }
};
struct Pt { int x; int y; };
template<class... A> auto packCap(A... a) { return [...xs = a]() { return (0 + ... + xs); }; }
template<class... A> auto packRef(A&... a) { return [&...xs = a]() { ((xs += 1), ...); return (0 + ... + xs); }; }
int gInit = [] { int s = 0; for (int i = 0; i < 5; ++i) s += i; return s; }();
auto gLam = [](int x) { return x * 3 + gInit; };
'''

class G:
    def __init__(s):
        s.k = 0
    def num(s, lo=1, hi=9):
        return r.randint(lo, hi)

g = G()

def intexpr(names, depth=0):
    # an int expression of small numbers over the names (each an int), no overflow at these sizes
    c = r.randint(0, 5 if depth < 2 and names else 1)
    if c == 0 or not names: return str(g.num())
    if c == 1: return r.choice(names)
    a, b = intexpr(names, depth + 1), intexpr(names, depth + 1)
    op = r.choice(['+', '-', '*', '+', '-'])
    if op == '*': b = str(g.num(1, 4))
    return '(%s %s %s)' % (a, op, b)

def blockCaptures():
    # ints a, b, c; a lambda with a capture list; the variables change after; two calls
    caps = r.choice(['a', '&a', '=', '&', '=, &b', '&, a', 'a, &b', 'x = a * 2', '&ra = a', 'a, b, &c', '&, x = b + 1', '=, &r = c', 'a, x = a + b'])
    mut = r.random() < 0.4
    byval, byref = set(), set()
    parts = [p.strip() for p in caps.split(',')]
    dflt = None
    for p in parts:
        if p == '=': dflt = 'val'
        elif p == '&': dflt = 'ref'
        elif '=' in p:
            n = p.split('=')[0].strip()
            if n.startswith('&'): byref.add(n[1:])
            else: byval.add(n)
        elif p.startswith('&'): byref.add(p[1:])
        else: byval.add(p)
    names = list(byval | byref)
    explicit = sorted(names)  # (the captures written: each read in the body, as kpp's unused captures are errors)
    if dflt:
        for n in 'abc':
            if n not in byval and n not in byref:
                (byval if dflt == 'val' else byref).add(n); names.append(n)
    names = sorted(set(names))
    readable = names + ['p']
    body = []
    for _ in range(r.randint(1, 3)):
        writable = [n for n in names if n in byref or mut]
        if writable and r.random() < 0.6:
            w = r.choice(writable)
            body.append('%s = %s;' % (w, intexpr(readable)))
    body.append('return %s;' % intexpr(readable))
    after = []
    for n in 'abc':
        if r.random() < 0.6: after.append('%s = %s;' % (n, intexpr(['a', 'b', 'c'])))
    calls = r.randint(1, 3)
    lines = ['{',
             '  int a = %d, b = %d, c = %d;' % (g.num(), g.num(), g.num()),
             '  auto f = [%s](int p)%s { %s };' % (caps, ' mutable' if mut else '', ' '.join(['(void)p;'] + ['(void)%s;' % n for n in explicit] + body))]
    if r.random() < 0.5: lines.append('  ' + ' '.join(after))
    lines.append('  int r0 = f(1);')
    if r.random() < 0.4: lines.append('  auto f2 = f;')
    else: lines.append('  auto& f2 = f;')
    lines.append('  int r1 = f(2); r1 += f2(3) * 10;')
    if r.random() < 0.5: lines.append('  ' + ' '.join(after))
    lines.append('  int r2 = %s(f, 4);' % r.choice(['apply1', 'applyRef', 'twice']))
    lines.append('  printf("cap %d %d %d %d %d %d\\n", r0, r1, r2, a, b, c);')
    lines.append('}')
    return lines

def blockMutable():
    n0 = g.num()
    step = g.num(1, 3)
    lines = ['{',
             '  auto c = [n = %d]() mutable { n += %d; return n; };' % (n0, step)]
    seq = []
    vs = ['c']
    for i in range(r.randint(3, 7)):
        k = r.randint(0, 4)
        if k == 0:
            nm = 'c%d' % i
            lines.append('  auto %s = %s;' % (nm, r.choice(vs)))
            vs.append(nm)
        elif k == 1:
            lines.append('  Function<int()> fn%d = %s;' % (i, r.choice(vs)))
            lines.append('  int x%d = fn%d(); x%d += 10 * fn%d();' % (i, i, i, i))
            seq.append('x%d' % i)
            vs.append('fn%d' % i)
        else:
            lines.append('  int x%d = %s();' % (i, r.choice(vs)))
            seq.append('x%d' % i)
    for j, v in enumerate(vs):
        lines.append('  int e%d = %s();' % (j, v)); seq.append('e%d' % j)
    lines.append('  printf("mut' + ' %d' * len(seq) + '\\n", ' + ', '.join(seq) + ');')
    lines.append('}')
    return lines

TYPES = [('int', ['3', '-4', '7']), ('double', ['2.5', '-1.25']), ('long long', ['5000000000LL', '-7LL']), ('char', ["'a'", "(char)3"]),
         ('unsigned', ['4u', '9u']), ('float', ['1.5f', '0.25f']), ('bool', ['true', 'false']), ('short', ['(short)6']), ('unsigned long long', ['8ull'])]

def blockGeneric():
    k = g.num()
    bodies = ['return x + y;', 'return x * y + k;', 'return x < y ? x : y;', 'auto s = x + y; return s * k;', 'return x - y * k;',
              'return k > 3 ? x : y;', 'return (x + k) / 2;']
    body = r.choice(bodies)
    params = r.choice(['auto x, auto y', 'auto x, auto y', 'const auto& x, auto y', 'auto x, int y', 'auto&& x, auto y'])
    params = ', '.join('[[maybe_unused]] ' + q for q in params.split(', '))  # (the body may read either)
    cap = r.choice(['k', '&k', '=', '&'])
    lines = ['{',
             '  [[maybe_unused]] int k = %d;' % k,
             '  auto gl = [%s](%s) { %s%s };' % (cap, params, '(void)k; ' if cap in ('k', '&k') else '', body)]
    for _ in range(r.randint(2, 5)):
        t1, v1 = r.choice(TYPES); t2, v2 = r.choice(TYPES)
        a, b = r.choice(v1), r.choice(v2)
        if r.random() < 0.3:
            lines.append('  show(callWith(gl, %s, %s));' % (a, b))
        else:
            lines.append('  show(gl(%s, %s));' % (a, b))
    lines.append('  printf("gen\\n");')
    lines.append('}')
    return lines

def blockTemplates():
    lines = ['{']
    out = []
    for i in range(r.randint(2, 5)):
        c = r.randint(0, 7)
        if c == 0:
            t, vs = r.choice(TYPES[:5])
            lines.append('  auto ad%d = adder<%s>(%s);' % (i, t, r.choice(vs)))
            lines.append('  show(ad%d(%s));' % (i, r.choice(vs)))
        elif c == 1:
            t, vs = r.choice([TYPES[0], TYPES[1], TYPES[2]])
            lines.append('  auto cn%d = counter(%s);' % (i, r.choice(vs)))
            lines.append('  cn%d(); auto cc%d = cn%d; cn%d();' % (i, i, i, i))
            lines.append('  show(cn%d()); show(cc%d()); show(cn%d());' % (i, i, i))
        elif c == 2:
            t, vs = r.choice(TYPES[:5])
            lines.append('  show(sumWith<%s>(%s, %s));' % (t, r.choice(vs), r.choice(vs)))
        elif c == 3:
            t, vs = r.choice([TYPES[0], TYPES[1], TYPES[2]])
            op = r.choice(['a + b', 'a * 2 - b', 'a < b ? b : a'])
            cap = r.choice(['', '&', '='])
            lines.append('  show(fold3(%s, %s, %s, [%s](auto a, auto b) { return %s; }));' % (r.choice(vs), r.choice(vs), r.choice(vs), cap, op))
        elif c == 4:
            lines.append('  show(scaled<%d>(%d));' % (g.num(), g.num()))
        elif c == 5:
            lines.append('  int z%d = %d;' % (i, g.num()))
            lines.append('  auto cp%d = compose([&z%d](int x) { return x * z%d; });' % (i, i, i))
            lines.append('  z%d += 1; show(cp%d(3)); show(compose(cp%d)(2));' % (i, i, i))
        elif c == 6:
            lines.append('  show(callWith([](int a, double b, char c) { return a + b + c; }, %d, %s, (char)%d));' % (g.num(), r.choice(['0.5', '1.75']), g.num()))
        else:
            lines.append('  int w%d = %d;' % (i, g.num()))
            lines.append('  show(callWith([w%d](auto... xs) { return (w%d + ... + xs); }, %s));' % (i, i, ', '.join(str(g.num()) for _ in range(r.randint(0, 4)))))
    lines.append('  printf("tpl\\n");')
    lines.append('}')
    return lines

def blockRecursion():
    c = r.randint(0, 4)
    n = r.randint(3, 14)
    if c == 0:
        body = ['  Function<int(int)> fib = [&fib](int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); };',
                '  printf("rec %%d\\n", fib(%d));' % n]
    elif c == 1:
        body = ['  Function<int(int)> even;', '  Function<int(int)> odd;',
                '  even = [&](int n) { return n == 0 ? 1 : odd(n - 1); };',
                '  odd = [&](int n) { return n == 0 ? 0 : even(n - 1); };',
                '  printf("rec %%d %%d\\n", even(%d), odd(%d));' % (n, n + 1)]
    elif c == 2:
        body = ['  int calls = 0;',
                '  Function<long long(int)> fact = [&](int n) -> long long { ++calls; return n <= 1 ? 1 : n * fact(n - 1); };',
                '  long long v = fact(%d);' % n,
                '  int c1 = calls;', '  int w = callFn([&](int k) { return (int)fact(k) %% 1000; }, %d);' % (n % 9), '  printf("rec %lld %d %d %d\\n", v, c1, w, calls);']
    elif c == 3:
        body = ['  Function<int(int)> f = [](int x) { return x + 1; };',
                '  int a = f(%d);' % n,
                '  Function<int(int)> g2 = f;',
                '  f = [g2](int x) { return g2(x) * 2; };',
                '  Function<int(int)> h = move(f);',
                '  int b = g2(1); int c = h(%d); int d = (bool)f ? 1 : 0;' % n, '  printf("rec %d %d %d %d\\n", a, b, c, d);']
    else:
        body = ['  auto self = [](auto& me, int n) -> int { return n <= 0 ? 0 : n + me(me, n - 1); };',
                '  printf("rec %%d\\n", self(self, %d));' % n]
    return ['{'] + body + ['}']

def blockTraced():
    c = r.randint(0, 6)
    i = g.num(1, 9)
    if c == 0:
        body = ['  T t(%d);' % i, '  auto f = [t]() { return t.val(); };', '  auto g2 = f;', '  int x = f(); int y = g2();', '  printf("| %d %d ", x, y);']
    elif c == 1:
        body = ['  T t(%d);' % i, '  auto f = [u = move(t)]() { return u.val(); };', '  int x = f(); int y = t.val();', '  printf("| %d %d ", x, y);']
    elif c == 2:
        body = ['  T t(%d);' % i, '  auto f = [&t]() { t.id += 5; return t.val(); };', '  int x = f(); int y = t.val();', '  printf("| %d %d ", x, y);']
    elif c == 3:
        body = ['  auto f = [t = T(%d)]() mutable { t.id++; return t.val(); };' % i, '  auto g2 = f;', '  int x = f(); int y = g2(); int z = f();', '  printf("| %d %d %d ", x, y, z);']
    elif c == 4:
        body = ['  T a(%d), b(%d);' % (i, i + 1), '  auto f = [b, a]() { return a.val() * 1000 + b.val(); };', '  auto fn = f;', '  auto fn2 = fn;', '  int x = fn(); int y = fn2();', '  printf("| %d %d ", x, y);']
    elif c == 5:
        body = ['  auto mk = [](int k) { return T(k); };', '  T x = mk(%d);' % i, '  show(mk(%d));' % (i + 1), '  int y = x.val();', '  printf("| %d ", y);']
    else:
        body = ['  T t(%d);' % i, '  auto outer = [t](int k) { return [t, k]() { return t.val() + k; }; };', '  auto in = outer(%d);' % i, '  int y = in();', '  printf("| %d ", y);']
    return ['{', '  printf("trc ");'] + body + ['}', 'printf("\\n");']

def blockNested():
    a = g.num()
    c = r.randint(0, 3)
    if c == 0:
        body = ['  int a = %d;' % a, '  auto outer = [a](int x) { return [a, x](int y) { return a * x + y; }; };', '  a = 100;',
                '  printf("nest %%d %%d\\n", outer(%d)(%d), outer(1)(a));' % (g.num(), g.num())]
    elif c == 1:
        body = ['  int a = %d;' % a, '  auto outer = [&a](int x) { auto in = [&a, x]() { a += x; return a; }; in(); return in(); };',
                '  int v = outer(%d);' % g.num(), '  printf("nest %d %d\\n", v, a);']
    elif c == 2:
        body = ['  int a = %d, b = %d;' % (a, g.num()), '  auto outer = [=]() mutable { auto in = [&]() { a += b; return a; }; in(); return in() + a; };',
                '  int v = outer(); int w = outer();', '  printf("nest %d %d %d\\n", v, w, a);']
    else:
        body = ['  int a = %d;' % a, '  auto l = [a]() { return [=]() { return [&]() { return a * 3; }(); }(); };', '  printf("nest %d\\n", l());']
    return ['{'] + body + ['}']

def blockMember():
    c = r.randint(0, 3)
    k = g.num()
    if c == 0: e = 's.run(%d)' % k
    elif c == 1: e = 's.all(%d)' % k
    elif c == 2: e = 's.copied(%d)' % k
    else: e = 's.getter()()'
    return ['{', '  S s; s.v = %d;' % g.num(), '  int r0 = %s;' % e, '  auto gt = s.getter(); s.v += 1;', '  printf("mem %d %d %d\\n", r0, s.v, gt());', '}']

def blockPointer():
    k = g.num()
    c = r.randint(0, 2)
    if c == 0:
        body = ['  int (*fp)(int) = [](int x) { return x * %d; };' % k, '  printf("fp %%d %%d\\n", fp(3), callFp(fp, 4));']
    elif c == 1:
        body = ['  auto fp = +[](int x) { return x - %d; };' % k, '  printf("fp %%d %%d\\n", fp(3), callFp([](int x) { return x * x; }, %d));' % k]
    else:
        body = ['  static int total = 0;', '  auto f = [](int x) { total += x; static int calls = 0; return ++calls * 100 + total; };',
                '  int a = f(%d); int b = f(1);' % k, '  printf("fp %d %d\\n", a, b);']
    return ['{'] + body + ['}']

def blockTemplateLambda():
    # C++20 lambdas with a template parameter list: deduced from the call, a pack, an array bound
    k = g.num()
    lines = ['{', '  [[maybe_unused]] int k = %d;' % k]  # (read or not: kpp's unused names are errors)
    for i in range(r.randint(2, 4)):
        c = r.randint(0, 4)
        if c == 0:
            lines.append('  auto mx%d = [k]<class X>(X a, X b) { return a < b ? b + k : a + k; };' % i)
            t, vs = r.choice(TYPES[:5])
            lines.append('  show(mx%d(%s, %s));' % (i, r.choice(vs), r.choice(vs)))
        elif c == 1:
            lines.append('  auto sz%d = []<class... Xs>(Xs... xs) { return (0 + ... + (int)sizeof(Xs)); };' % i)
            lines.append('  show(sz%d(%s));' % (i, ', '.join(r.choice(r.choice(TYPES)[1]) for _ in range(r.randint(0, 4)))))
        elif c == 2:
            n = r.randint(1, 5)
            lines.append('  int arr%d[%d] = {};' % (i, n))
            lines.append('  auto bound%d = []<class X, int N>(X (&)[N]) { return N * 10 + (int)sizeof(X); };' % i)
            lines.append('  show(bound%d(arr%d));' % (i, i))
        elif c == 3:
            lines.append('  auto first%d = [&]<class X, class... Rest>(X x, Rest... rest) { return x * k + (int)sizeof...(Rest); };' % i)
            lines.append('  show(first%d(%s));' % (i, ', '.join(str(g.num()) for _ in range(r.randint(1, 4)))))
        else:
            lines.append('  auto mk%d = []<class X>(X x) { return [x]<class Y>(Y y) { return x + y; }; };' % i)
            lines.append('  show(mk%d(%s)(%s));' % (i, r.choice(['3', '2.5', "'a'", '7LL']), r.choice(['1', '0.5', '2u'])))
    lines.append('  printf("tlam\\n");')
    lines.append('}')
    return lines

def blockMore():
    k = g.num()
    lines = ['{', '  [[maybe_unused]] int k = %d;' % k]
    for i in range(r.randint(2, 4)):
        c = r.randint(0, 13)
        if c == 0:
            lines += ['  auto [a%d, b%d] = Pt{k, %d};' % (i, i, g.num()),
                      '  auto sb%d = [a%d, &b%d]() { b%d += a%d; return a%d * 10 + b%d; };' % (i, i, i, i, i, i, i),
                      '  show(sb%d()); show(sb%d()); show(b%d);' % (i, i, i)]
        elif c == 1:
            lines += ['  auto mk%d = [](int base) { return [base]() { static int n = 0; return base + ++n * 100; }; };' % i,
                      '  auto f%d = mk%d(1); auto g%d = mk%d(%d);' % (i, i, i, i, g.num(10, 19)),
                      '  int x%d = f%d(); int y%d = g%d(); int z%d = f%d();' % (i, i, i, i, i, i),
                      '  show(x%d); show(y%d); show(z%d);' % (i, i, i)]
        elif c == 2:
            lines += ['  auto da%d = [k](int a, int b = %d) { return a * b + k; };' % (i, g.num()),
                      '  show(da%d(%d)); show(da%d(%d, %d));' % (i, g.num(), i, g.num(), g.num())]
        elif c == 3:
            n = g.num(1, 4)
            lines += ['  constexpr auto sq%d = [](int x) { return x * x; };' % i,
                      '  int arr%d[sq%d(%d)] = {};' % (i, i, n),
                      '  static_assert(sq%d(%d) == %d);' % (i, n, n * n),
                      '  show((int)(sizeof(arr%d) / sizeof(int)));' % i]
        elif c == 4:
            lines += ['  using L%d = decltype([](int x) { return x * 2 + 1; });' % i,
                      '  L%d l%d; L%d m%d = l%d;' % (i, i, i, i, i),
                      '  show(l%d(k) + m%d(%d));' % (i, i, g.num())]
        elif c == 5:
            lines += ['  int x%d = k;' % i,
                      '  auto outer%d = [x%d]() mutable { ++x%d; auto inner = [x%d]() mutable { return ++x%d; }; int a = inner(); int b = inner(); return a * 100 + b * 10 + x%d; };' % (i, i, i, i, i, i),
                      '  show(outer%d()); show(outer%d()); show(x%d);' % (i, i, i)]
        elif c == 6:
            lines += ['  auto byRef%d = [](int& r) { return [r]() mutable { return ++r; }; };' % i,
                      '  int z%d = k; auto f%d = byRef%d(z%d);' % (i, i, i, i),
                      '  show(f%d()); show(f%d()); show(z%d);' % (i, i, i)]
        elif c == 7:
            lines += ['  int v%d = k;' % i,
                      '  auto rc%d = [&r = v%d, c = v%d * 2]() mutable { r += c; c += 1; return r + c; };' % (i, i, i),
                      '  show(rc%d()); show(rc%d()); show(v%d);' % (i, i, i)]
        elif c == 8:
            args = ', '.join(str(g.num()) for _ in range(r.randint(0, 4)))
            lines += ['  auto pc%d = packCap(%s);' % (i, args), '  show(pc%d());' % i]
            lines += ['  int p%d = k, q%d = %d;' % (i, i, g.num()), '  auto pr%d = packRef(p%d, q%d);' % (i, i, i),
                      '  show(pr%d()); show(p%d * 100 + q%d);' % (i, i, i)]
        elif c == 9:
            lines += ['  int w%d = k;' % i,
                      '  auto ref%d = [&w%d]() -> int& { return w%d; };' % (i, i, i),
                      '  ref%d() += %d; ref%d() *= 2;' % (i, g.num(), i),
                      '  show(w%d);' % i]
        elif c == 10:
            lines += ['  auto id%d = [](auto&& x) -> decltype(auto) { return (x); };' % i,
                      '  int w%d = k; id%d(w%d) = %d; show(w%d);' % (i, i, i, g.num(), i)]
        elif c == 11:
            lines += ['  int (*gp%d)(int) = [](auto x) { return x * %d; };' % (i, g.num()),
                      '  double (*hp%d)(double) = [](auto x) { return x / 2; };' % i,
                      '  show(gp%d(k)); show(hp%d(k));' % (i, i)]
        elif c == 12:
            lines += ['  show(gInit); show(gLam(k));']
        else:
            lines += ['  auto cnt%d = [n = 0]() mutable { return ++n; };' % i,
                      '  auto cp%d = cnt%d; cnt%d(); cnt%d();' % (i, i, i, i),
                      '  show(cnt%d()); show(cp%d());' % (i, i)]
    lines.append('  printf("more\\n");')
    lines.append('}')
    return lines

KINDS = [blockMore, blockMore, blockMore, blockMore, blockCaptures, blockCaptures, blockCaptures, blockMutable, blockGeneric, blockGeneric, blockTemplates, blockTemplates,
         blockRecursion, blockTraced, blockTraced, blockNested, blockMember, blockPointer, blockTemplateLambda, blockTemplateLambda]
blocks = [r.choice(KINDS)() for _ in range(NB)]

def render():
    lines = PRELUDE.split('\n')
    where = {}
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
tmp = os.path.join(os.environ.get('KPP_DIFF_TMP', HERE), 'lamtmp-%s.cpp' % sys.argv[1])
for it in range(6):
    text, where = render()
    open(tmp, 'w').write(head + text + '\n')
    p = subprocess.run([CLANG, '-std=c++20', '-Wno-everything', '-Werror=writable-strings', '-fsyntax-only', '-ferror-limit=0', '-fno-ms-compatibility',
                        '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', tmp], capture_output=True, text=True)
    errs = set(int(m.group(1)) - headLines for m in re.finditer(r'lamtmp-[^:]*\.cpp:(\d+):\d+: error', p.stderr))
    if not errs: break
    for e in errs:
        if e in where: blocks[where[e]] = None
        else: sys.stderr.write('unplaced error at %d\n%s' % (e, p.stderr[:2000])); sys.exit(1)
os.remove(tmp)
print(render()[0])
