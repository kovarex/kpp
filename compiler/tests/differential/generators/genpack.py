#!/usr/bin/env python3
# Random parameter packs: fold expressions of every operator (unary and binary, left and right, empty packs,
# compound assignments) over packs of mixed arithmetic types, each called at compile time (constexpr, kpp's
# interpreter) and at run time on volatile values (codegen), the result's type shown by overload; folds over
# `,`, `&&` and `||` with side effects (order, short circuit); expansion patterns (a call per element, two
# expansions in one pattern, arrays, sizeof..., index sequences, pack bases and their constructors, lambda
# captures of packs, recursion with if constexpr, class templates over type packs); forwarding of every value
# category through Ts&&... and auto&&... to an overload set of int&, const int&, int&&, const int&&.
# Nothing with a side effect sits in a function call's arguments (clang's MSVC target goes right to left).
# One source for kpp and C++; the oracle is the other compiler. Usage: genpack.py SEED TESTS > file
import random, sys, os
# GENPACK_SKIP: comma-separated known gaps left out (assign: compound-assignment folds; cnested, clambda:
# the constexpr column of a nested expansion, of a generic lambda's pack; leftcomma: a fold of `,` but `(args , ...)`
# (the parser reads `(init , ... , args)` and `(args , ... , init)` as comma expressions);
# twopacks: a fold over a function pack and an int... pack together; ccapture, crecurse, cptr: the constexpr
# column of lambda captures of a pack, of a pack passed on to (T, Ts...), of pointers to a pack's elements)
SKIP = set(os.environ.get('GENPACK_SKIP', '').split(','))

class Ty:
    def __init__(s, name, kind, tag):
        s.name, s.kind, s.tag = name, kind, tag
BOOL = Ty('bool', 'b', 'b')
CHAR = Ty('char', 'i', 'c')
SHORT = Ty('short', 'i', 's')
USHORT = Ty('unsigned short', 'i', 'us')
INT = Ty('int', 'i', 'i')
UINT = Ty('unsigned', 'i', 'u')
LL = Ty('long long', 'i', 'll')
ULL = Ty('unsigned long long', 'i', 'ull')
FLOAT = Ty('float', 'f', 'f')
DOUBLE = Ty('double', 'f', 'd')
ALL = [BOOL, CHAR, SHORT, USHORT, INT, INT, UINT, UINT, LL, ULL, FLOAT, DOUBLE, DOUBLE]
INTEGRAL = [t for t in ALL if t.kind != 'f']
UNSIGNED = (UINT, ULL, USHORT)

PRELUDE = r'''extern "C" int printf(const char* format, ...);
static volatile int vz = 0;
void show(int id, char kind, bool v) { printf("%d %c b %d\n", id, kind, (int)v); }
void show(int id, char kind, char v) { printf("%d %c c %d\n", id, kind, (int)v); }
void show(int id, char kind, short v) { printf("%d %c s %d\n", id, kind, (int)v); }
void show(int id, char kind, unsigned short v) { printf("%d %c us %d\n", id, kind, (int)v); }
void show(int id, char kind, int v) { printf("%d %c i %d\n", id, kind, v); }
void show(int id, char kind, unsigned v) { printf("%d %c u %u\n", id, kind, v); }
void show(int id, char kind, long long v) { printf("%d %c ll %lld\n", id, kind, v); }
void show(int id, char kind, unsigned long long v) { printf("%d %c ull %llu\n", id, kind, v); }
void show(int id, char kind, float v) { printf("%d %c f %a\n", id, kind, (double)v); }
void show(int id, char kind, double v) { printf("%d %c d %a\n", id, kind, v); }
template<typename T> bool pr(T v) { printf("p%g ", (double)v); return v != 0; }
void cat(int&) { printf("L "); }
void cat(const int&) { printf("CL "); }
void cat(int&&) { printf("R "); }
void cat(const int&&) { printf("CR "); }
struct Holder { int m; const int c; };
template<int K> struct Part
{
  int v;
  Part(int x) : v(x * K) { printf("+P%d ", K); }
  ~Part() { printf("-P%d ", K); }
  int get() const { return v; }
};
template<int... Is> struct Seq {};
template<int N, int... Is> struct MakeSeq : MakeSeq<N - 1, N - 1, Is...> {};
template<int... Is> struct MakeSeq<0, Is...> { using type = Seq<Is...>; };
template<typename A, typename B> struct Same { static constexpr bool v = false; };
template<typename A> struct Same<A, A> { static constexpr bool v = true; };
template<typename... Ts> struct Count { static constexpr int value = 0; };
template<typename T, typename... Ts> struct Count<T, Ts...> { static constexpr int value = (int)sizeof(T) * 10 + Count<Ts...>::value; };
template<typename... Ts> constexpr auto sumAll(Ts... a) { return (0 + ... + a); }
'''

def lit(t, v):
    if t is BOOL: return 'true' if v else 'false'
    if t.kind == 'f':
        s = repr(float(v))
        s = s if t is DOUBLE else s + 'f'
        return '(' + s + ')' if v < 0 else s
    suf = {'u': 'u', 'll': 'll', 'ull': 'ull'}.get(t.tag)
    if suf: return '(%d%s)' % (v, suf) if v < 0 else '%d%s' % (v, suf)
    return '(%s)%s' % (t.name, '(%d)' % v if v < 0 else str(v))

def vol(t, v):
    # the same value, read through a volatile: no constant folding
    if t is BOOL: return '(bool)(%d + vz)' % v
    return '(%s)(%s + vz)' % (t.name, lit(t, v))

class Gen:
    def __init__(s, r):
        s.r = r
    def value(s, t, nonzero=False, small=False, shift=False, big=True):
        r = s.r
        if shift: return r.randint(0, 3)
        if t is BOOL: return 1 if nonzero else r.randint(0, 1)
        if t is CHAR or t is USHORT: v = r.randint(0, 9)
        elif t in UNSIGNED:
            v = r.randint(0, 20)
            if big and not small and r.random() < 0.15: v = (1 << (64 if t is ULL else 32 if t is UINT else 16)) - r.randint(1, 9)
        elif t.kind == 'f':
            v = r.choice([r.randint(-20, 20), r.randint(-40, 40) / 4.0])
        else: v = r.randint(-9, 9) if small else r.randint(-20, 20)
        if nonzero and v == 0: v = 1
        if small and t in UNSIGNED: v = min(v, 9)
        return v

    def fold(s, n):
        r = s.r
        op = r.choice(['+', '-', '*', '/', '%', '<<', '>>', '&', '|', '^', '&&', '||', ',', '==', '!=', '<', '>', '<=', '>=',
                       ] + ([] if 'assign' in SKIP else ['+=', '-=', '*=', '/=', '%=', '<<=', '>>=', '&=', '|=', '^=']))
        assign = op.endswith('=') and op not in ('==', '!=', '<=', '>=')
        base = op[:-1] if assign else op
        intOnly = base in ('%', '<<', '>>', '&', '|', '^')
        count = r.randint(0, 5)
        # forms: 0 (args op ...), 1 (... op args), 2 (init op ... op args), 3 (args op ... op init)
        if base == ',' and 'leftcomma' in SKIP: form = 0
        elif base in ('/', '%', '<<', '>>'): form = r.choice([1, 2])
        elif assign: form = r.choice([0, 2])
        else: form = r.randint(0, 3)
        if form in (0, 1) and count == 0 and base not in ('&&', '||'): count = 1
        if assign and form == 0 and base not in ('+', '-', '*', '&', '|', '^'): form = 2
        pool = INTEGRAL if intOnly else ALL
        types = [r.choice(pool) for _ in range(count)]
        hasFloat = any(t.kind == 'f' for t in types)
        if assign and hasFloat:
            # a floating value converted to a target: keep targets that hold every such result
            safe = [INT, LL, DOUBLE, FLOAT]
            if form == 0: types = [t if t.kind == 'f' or t in safe else r.choice(safe) for t in types]
            if form == 1 and types[0] not in safe: types[0] = r.choice(safe)
        initT = r.choice(pool)
        if assign and hasFloat: initT = r.choice([INT, LL, DOUBLE, FLOAT])
        if assign and initT is BOOL and base in ('-', '+', '*'): initT = INT
        if base == '*' or base in ('<<', '>>'):
            small = True
        else:
            small = False
        shift = base in ('<<', '>>')
        nonzero = base in ('/', '%')
        vals = []
        bigUsed = False
        for i, t in enumerate(types):
            isShiftAmount = shift and (form == 2 or i > 0)
            isDivisor = nonzero and (form == 2 or i > 0)
            v = s.value(t, nonzero=isDivisor, small=small, shift=isShiftAmount, big=not bigUsed)
            if t in UNSIGNED and v > 1000: bigUsed = True
            vals.append(v)
        initV = s.value(initT, small=True, big=False) if form >= 2 else None
        if form == 2 and shift: initV = s.value(initT, small=True, big=False)
        if form == 0: e = '(args %s ...)' % op
        elif form == 1: e = '(... %s args)' % op
        elif form == 2:
            e = '(init %s ... %s args)' % (op, op)
        else: e = '(args %s ... %s init)' % (op, op)
        return op, form, types, vals, initT, initV, e

    def test_fold(s, i):
        op, form, types, vals, initT, initV, e = s.fold(0)
        defs = []
        if form >= 2:
            defs.append('template<typename I, typename... Ts> constexpr auto fold%d([[maybe_unused]] I init, Ts... args) { return %s; }' % (i, e))
            cargs = [lit(initT, initV)] + [lit(t, v) for t, v in zip(types, vals)]
            rargs = [vol(initT, initV)] + [vol(t, v) for t, v in zip(types, vals)]
        else:
            defs.append('template<typename... Ts> constexpr auto fold%d(Ts... args) { return %s; }' % (i, e))
            cargs = [lit(t, v) for t, v in zip(types, vals)]
            rargs = [vol(t, v) for t, v in zip(types, vals)]
        body = ['  { constexpr auto c = fold%d(%s); show(%d, \'c\', c); }' % (i, ', '.join(cargs), i),
                '  show(%d, \'r\', fold%d(%s));' % (i, i, ', '.join(rargs))]
        return defs, body

    def test_effects(s, i):
        # folds whose elements print: order and short circuit
        r = s.r
        op = r.choice(['&&', '||', ','])
        form = r.randint(0, 3)
        if op == ',' and 'leftcomma' in SKIP: form = 0
        count = r.randint(0, 5)
        types = [r.choice(ALL) for _ in range(count)]
        vals = []
        for t in types:
            v = s.value(t, big=False)
            if r.random() < 0.4: v = 0 if t is not BOOL else 0
            vals.append(v)
        el = r.choice(['pr(args)', 'pr(args * 2)', 'pr(args) %s pr(args + 1)' % r.choice(['&&', '||']), '(pr(args), args)'])
        el = '(' + el + ')' if ' ' in el else el
        if form == 0: e = '(%s %s ...)' % (el, op)
        elif form == 1: e = '(... %s %s)' % (op, el)
        elif form == 2: e = '(pr(init) %s ... %s %s)' % (op, op, el)
        else: e = '(%s %s ... %s pr(init))' % (el, op, op)
        if op == ',' and form < 2 and count == 0: count, types, vals = 1, [INT], [3]
        ret = 'auto' if op != ',' else 'void'
        stmt = 'return %s;' % e if op != ',' else '%s;' % e
        defs = ['template<typename I, typename... Ts> %s eff%d([[maybe_unused]] I init, Ts... args) { %s }' % (ret, i, stmt)]
        initV = s.value(INT, big=False)
        args = ', '.join([str(initV)] + [vol(t, v) for t, v in zip(types, vals)])
        if op == ',':
            body = ['  printf("%d e ");' % i, '  eff%d(%s);' % (i, args), '  printf("\\n");']
        else:
            body = ['  printf("%d e ");' % i, '  { auto v = eff%d(%s); printf("= %%d\\n", (int)v); }' % (i, args)]
        return defs, body

    def elements(s, lo=1, hi=5, pool=None):
        r = s.r
        n = r.randint(lo, hi)
        types = [r.choice(pool or [INT, INT, UINT, LL, SHORT, CHAR, DOUBLE]) for _ in range(n)]
        vals = [s.value(t, small=True, big=False) for t in types]
        return types, vals

    def test_pattern(s, i):
        r = s.r
        k = r.randint(0, 9)
        if k == 3 and 'twopacks' in SKIP: k = 0
        types, vals = s.elements(0 if k in (0, 1) else 1)
        constArgs = ', '.join(lit(t, v) for t, v in zip(types, vals))
        volArgs = ', '.join(vol(t, v) for t, v in zip(types, vals))
        defs = []
        pure = True
        if k == 0:
            f = r.choice(['args * 2 + 1', 'args - 3', '-args', 'args * args', '(args > 2 ? args : -args)', 'sumAll(args, 1)'])
            defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { return sumAll((%s)...); }' % (i, f))
        elif k == 1:
            defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { return sumAll(sumAll(args, args...)...) + (int)sizeof...(Ts) * 1000 + (int)sizeof...(args) * 100; }' % i)
        elif k == 2:
            # array from the pack: order of elements
            defs.append('template<typename... Ts> constexpr long long pat%d(Ts... args) { long long a[] = {(long long)args..., 7, (long long)(args * 3)...}; long long h = 0; for (long long x : a) h = h * 31 + x; return h; }' % i)
        elif k == 3:
            # an index sequence: weights by position
            defs.append('template<int... Is, typename... Ts> constexpr auto weigh%d(Seq<Is...>, Ts... args) { return (0 + ... + (args * (Is + 1))) * 10 + (Is + ... + 0); }' % i)
            defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { return weigh%d(typename MakeSeq<sizeof...(Ts)>::type{}, args...); }' % (i, i))
        elif k == 4:
            # recursion with if constexpr: last, first and alternating sums
            defs.append('template<typename T, typename... Ts> constexpr auto alt%d(T a, Ts... rest) { if constexpr (sizeof...(rest) == 0) return a; else return a - alt%d(rest...); }' % (i, i))
            defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { return alt%d(args...); }' % (i, i))
        elif k == 5:
            # lambda captures of a pack, by value and by reference
            form = r.randint(0, 2)
            if form == 0:
                defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { auto l = [args...]() { return (args + ... + 0); }; return l() * 2; }' % i)
            elif form == 1:
                defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { auto l = [&args...]() { ((args = args + 1), ...); }; l(); l(); return sumAll(args...); }' % i)
            else:
                defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { auto l = [...xs = args * 2]() { return sumAll(xs...); }; return l(); }' % i)
        elif k == 6:
            # class templates over the type pack
            defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { return Count<Ts...>::value * 100 + Count<Ts..., char, Ts...>::value + (Same<Ts, int>::v + ... + 0) * 1000000; }' % i)
        elif k == 7:
            # pointers to the pack's elements
            defs.append('template<typename... Ts> constexpr auto deref%d(Ts*... ps) { return (*ps + ...); }' % i)
            defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { return deref%d(&args...); }' % (i, i))
        elif k == 8:
            # a generic lambda's pack, then a nested pack expansion inside a lambda
            defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { auto g = [](auto... xs) { return (xs * ... * 1); }; return g(args...) + g((args + 1)...); }' % i)
        else:
            # a pack of functional casts to other types
            to = r.choice(['int', 'long long', 'double', 'short'] + ([] if DOUBLE in types else ['unsigned']))
            defs.append('template<typename... Ts> constexpr auto pat%d(Ts... args) { return ((%s)(args) + ... + (%s)(1)); }' % (i, to, to))
        body = []
        if {1: 'cnested', 8: 'clambda', 5: 'ccapture', 4: 'crecurse', 7: 'cptr'}.get(k) in SKIP: pure = False
        if not pure:
            body.append("  show(%d, 'r', pat%d(%s));" % (i, i, volArgs))
        if pure:
            body.append('  { constexpr auto c = pat%d(%s); show(%d, \'c\', c); }' % (i, constArgs, i))
            body.append('  show(%d, \'r\', pat%d(%s));' % (i, i, volArgs))
        return defs, body

    def test_bases(s, i):
        r = s.r
        ks = [r.randint(1, 9) for _ in range(r.randint(1, 4))]
        ks = list(dict.fromkeys(ks))
        bases = ', '.join('Part<%d>' % k for k in ks)
        x = r.randint(1, 9)
        defs = ['template<typename... Bs> struct Multi%d : Bs...' % i, '{',
                '  int extra;',
                '  Multi%d(int x) : Bs(x + (int)sizeof...(Bs))..., extra((Bs::get() + ... + 0)) { printf("+M "); }' % i,
                '  ~Multi%d() { printf("-M "); }' % i,
                '  int total() const { return (0 + ... + Bs::get()) * 10 + extra; }',
                '};']
        body = ['  printf("%d m ");' % i,
                '  { Multi%d<%s> m(%d); printf("= %%d ", m.total()); }' % (i, bases, x),
                '  printf("\\n");']
        return defs, body

    def test_forward(s, i):
        r = s.r
        kinds = ['x', 'cx', '1', 'move(x)', 'move(cx)', 'h.m', 'h.c', 'move(h).m', 'move(h).c', 'Holder{4, 5}.m', 'Holder{4, 5}.c',
                 'static_cast<const int&&>(x)', 'd', 'rx', 'crx', 'x + 0', '(x)', 'ix[1]', 'move(ix[2])']
        args = [r.choice(kinds) for _ in range(r.randint(0, 5))]
        via = r.randint(0, 3)
        if via == 0:
            defs = ['template<typename... Ts> void fwd%d(Ts&&... args) { (cat(forward<Ts>(args)), ...); printf("| "); (cat(args), ...); }' % i]
            call = 'fwd%d(%s)' % (i, ', '.join(args))
        elif via == 1:
            defs = ['template<typename... Ts> void fwd%d(Ts&&... args) { auto l = [](auto&&... xs) { (cat(forward<decltype(xs)>(xs)), ...); }; l(forward<Ts>(args)...); printf("| "); l(args...); }' % i]
            call = 'fwd%d(%s)' % (i, ', '.join(args))
        elif via == 2:
            defs = ['template<typename T> decltype(auto) pass%d(T&& a) { return forward<T>(a); }' % i,
                    'template<typename... Ts> void fwd%d(Ts&&... args) { (cat(pass%d(forward<Ts>(args))), ...); }' % (i, i)]
            call = 'fwd%d(%s)' % (i, ', '.join(args))
        else:
            defs = ['template<typename... Ts> void inner%d(Ts&&... args) { (cat(forward<Ts>(args)), ...); }' % i,
                    'template<typename... Ts> void fwd%d(Ts&&... args) { inner%d(forward<Ts>(args)...); printf("| "); inner%d(args...); }' % (i, i, i)]
            call = 'fwd%d(%s)' % (i, ', '.join(args))
        body = ['  {',
                '    int x = 1; const int cx = 2; Holder h{3, 4}; double d = 2.5; int& rx = x; const int& crx = cx; int ix[3] = {1, 2, 3};',
                '    (void)x; (void)cx; (void)h; (void)d; (void)rx; (void)crx; (void)ix;',  # (each call names some: kpp's unused names are errors)
                '    printf("%d f ");' % i,
                '    %s;' % call,
                '    printf("\\n");',
                '  }']
        return defs, body

def main():
    seed, count = int(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 30
    r = random.Random(seed)
    g = Gen(r)
    out = [PRELUDE]
    tests = []
    for i in range(count):
        k = r.choice(['fold'] * 5 + ['effects'] * 2 + ['pattern'] * 4 + ['bases', 'forward', 'forward'])
        defs, body = getattr(g, 'test_' + k)(i)
        out += defs
        out.append('void t%d()' % i)
        out.append('{')
        out += body
        out.append('}')
        tests.append(i)
    out.append('int main()')
    out.append('{')
    for i in tests: out.append('  t%d();' % i)
    out.append('  return 0;')
    out.append('}')
    print('\n'.join(out))

main()
