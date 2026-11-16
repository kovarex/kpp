#!/usr/bin/env python3
# Random exception flow: nested try blocks, handlers of every kind of type, rethrow, a new throw from a
# handler, destructors that catch inside, a destructor that throws when nothing is unwinding, a
# constructor's function-try-block, loops with break and continue, returns, lambdas. Each function runs
# for several arguments so each branch is taken. One source for kpp and C++ (cxx-head.h maps
# uncaught_exceptions); copies print nothing (MSVC copies a by-value handler parameter before unwinding,
# Itanium after it). The oracle is the other compiler. Usage: genexc.py SEED [FUNCTIONS] > file
import random, sys

r = random.Random(int(sys.argv[1]))
NF = int(sys.argv[2]) if len(sys.argv) > 2 else r.randint(3, 6)
ARGS = 6

PRELUDE = r'''extern "C" int printf(const char* format, ...);
class E0 { public: int id; E0(int i) : id(i) { printf("+E%d ", id); } E0(const E0& o) : id(o.id + 1000) {} virtual ~E0() { printf("-E%d ", id); } virtual int kind() const { return 0; } };
class E1 : public E0 { public: E1(int i) : E0(i) {} int kind() const override { return 1; } };
class E2 : public E0 { public: E2(int i) : E0(i) {} int kind() const override { return 2; } };
class E3 : public E1 { public: E3(int i) : E1(i) {} int kind() const override { return 3; } };
class E4 { public: int id; E4(int i) : id(i) { printf("+F%d ", id); } E4(const E4& o) : id(o.id + 1000) {} ~E4() { printf("-F%d ", id); } };
class T { public: int id; T(int i) : id(i) { printf("+T%d ", id); } T(const T& o) : id(o.id + 1000) {} ~T() { printf("-T%d ", id); } };
class G { public: int id; G(int i) : id(i) { printf("+G%d ", id); } ~G() { printf("-G%d(%d) ", id, uncaught_exceptions()); try { throw E4(id); } catch (E4& e) { printf("g%d(%d) ", e.id, uncaught_exceptions()); } } };
class D { public: int id; bool fire; D(int i, bool f) : id(i), fire(f) { printf("+D%d ", id); } ~D() noexcept(false) { printf("-D%d ", id); if (fire && uncaught_exceptions() == 0) throw E2(id); } };
int thr(int n) { if (n % 3 == 0) { printf("thr "); throw E1(n); } return n; }
class W { public: T a; T b; W(int n) try : a(n), b(thr(n)) { printf("W "); } catch (const E0& e) { printf("Wc%d ", e.kind()); } ~W() { printf("~W "); } };
class M { public: T a; D d; T b; M(int n, bool f) : a(n), d(n + 1, f), b(n + 2) { printf("M "); } ~M() { printf("~M "); } };
'''

HANDLERS = [('E0& e', 'E'), ('const E0& e', 'E'), ('E1& e', 'E'), ('const E2& e', 'E'), ('E3& e', 'E'),
            ('E0 e', 'E'), ('E1 e', 'E'), ('E4& e', 'F'), ('E4 e', 'F'), ('int e', 'I'), ('const T& e', 'F')]

class Gen:
    def __init__(s):
        s.k = 0
        s.fuel = 0
    def num(s):
        s.k += 1
        return s.k
    def cond(s):
        return 'a == %d' % r.randrange(ARGS)
    def throwExpr(s):
        n = s.num()
        c = r.randrange(8)
        if c < 4: return 'throw E%d(%d)' % (c, n)
        if c == 4: return 'throw E4(%d)' % n
        if c == 5: return 'throw %d' % n
        if c == 6: return 'throw T(%d)' % n
        return 'throw E%d(a + %d)' % (r.randrange(4), n)
    def block(s, f, depth, inHandler, inLoop, ind):
        out = []
        n = r.randint(1, 4)
        for _ in range(n):
            if s.fuel <= 0: break
            s.fuel -= 1
            st, ends = s.stmt(f, depth, inHandler, inLoop, ind)
            out += st
            if ends: break
        return out
    def stmt(s, f, depth, inHandler, inLoop, ind):
        p = ' ' * ind
        c = r.randrange(20)
        n = s.num()
        if c <= 2: return [p + 'T t%d(%d);' % (n, n)], False
        if c == 3: return [p + 'G g%d(%d);' % (n, n)], False
        if c == 4: return [p + 'D d%d(%d, %s);' % (n, n, s.cond())], False
        if c == 5: return [p + 'W w%d(a + %d);' % (n, r.randrange(3))], False
        if c == 6: return [p + 'M m%d(%d, %s);' % (n, n, s.cond())], False
        if c == 7: return [p + 'if (%s) %s;' % (s.cond(), s.throwExpr())], False
        if c == 8 and r.random() < 0.3: return [p + '%s;' % s.throwExpr()], True
        if c == 9 and inHandler: return [p + 'if (%s) throw;' % s.cond()], False
        if c == 10: return [p + 'if (%s) return %d;' % (s.cond(), n)], False
        if c == 11 and f + 1 < NF:
            j = r.randrange(f + 1, NF)
            return [p + '{ int v = F%d(a + %d); printf("v%%d ", v); }' % (j, r.randrange(3))], False
        if c == 12 and inLoop:
            return [p + 'if (i == %d && %s) %s;' % (r.randrange(2), s.cond(), r.choice(['break', 'continue']))], False
        if c == 13 and depth < 4:
            out = [p + 'for (int i = 0; i < 2; i++)', p + '{', p + '  printf("i%d ", i);']
            out += s.block(f, depth + 1, inHandler, True, ind + 2)
            out += [p + '}']
            return out, False
        if c == 14 and depth < 4:
            return [p + '{'] + s.block(f, depth + 1, inHandler, inLoop, ind + 2) + [p + '}'], False
        if c == 15:
            return [p + 'printf("u%d ", uncaught_exceptions());'], False
        if c == 16:
            return [p + '{ auto l = [a]() -> int { if (%s) %s; return %d; }; int v = l(); printf("l%%d ", v); }' % (s.cond(), s.throwExpr(), n)], False
        if depth < 4:
            return s.tryStmt(f, depth, inHandler, inLoop, ind), False
        return [p + 'T t%d(%d);' % (n, n)], False
    def tryStmt(s, f, depth, inHandler, inLoop, ind):
        p = ' ' * ind
        out = [p + 'try', p + '{']
        out += s.block(f, depth + 1, inHandler, inLoop, ind + 2)
        out += [p + '}']
        hs = r.sample(HANDLERS, r.randint(1, 3))
        if r.random() < 0.4:
            hs.append(('...', None))
        for h, kind in hs:
            n = s.num()
            out += [p + 'catch (%s)' % h, p + '{']
            q = ' ' * (ind + 2)
            if kind == 'E':
                out.append(q + 'printf("c%d:%%d:%%d ", e.kind(), e.id);' % n)
                if '&' in h and 'const' not in h and r.random() < 0.3:
                    out.append(q + 'e.id += 5;')
            elif kind == 'F':
                out.append(q + 'printf("c%d:%%d ", e.id);' % n)
            elif kind == 'I':
                out.append(q + 'printf("c%d:%%d ", e);' % n)
            else:
                out.append(q + 'printf("c%d:... ", 0);' % n)
            if r.random() < 0.6:
                out += s.block(f, depth + 1, True, inLoop, ind + 2)
            out += [p + '}']
        return out

g = Gen()
out = [PRELUDE]
for f in range(NF - 1, -1, -1):
    out.append('int F%d(int a);' % f)
for f in range(NF):
    g.fuel = r.randint(6, 18)
    out.append('int F%d(int a)' % f)
    out.append('{')
    out.append('  printf("F%d(%%d) ", a);' % f)
    out += g.block(f, 0, False, False, 2)
    out.append('  return %d;' % g.num())
    out.append('}')
out.append('int main()')
out.append('{')
for f in range(NF):
    for a in range(ARGS):
        out.append('  try { int v = F%d(%d); printf("ret%%d", v); }' % (f, a))
        out.append('  catch (const E0& e) { printf("top%d:%d", e.kind(), e.id); }')
        out.append('  catch (const E4& e) { printf("topF%d", e.id); }')
        out.append('  catch (int e) { printf("topI%d", e); }')
        out.append('  catch (...) { printf("top..."); }')
        out.append('  printf(" u%d\\n", uncaught_exceptions());')
out.append('  return 0;')
out.append('}')
print('\n'.join(out))
