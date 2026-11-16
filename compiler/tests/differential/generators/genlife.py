#!/usr/bin/env python3
# Random lifetimes: statements over a traced class, each scenario run with an exception at its 1st, 2nd,
# ... construction, so every cleanup path runs. One source for kpp and C++; the oracle is the other compiler.
# Usage: genlife.py SEED SCENARIOS > file
import random, sys, os

PRELUDE = r'''
extern "C" int printf(const char* format, ...);
int ticks = 0, limit = 0;
void tick() { if (limit != 0 && ++ticks == limit) { printf("!throw "); throw 1; } }
struct T
{
  int id;
  T(int i = 0) : id(i) { tick(); printf("+%d ", id); }
  T(const T& o) : id(o.id + 100) { tick(); printf("c%d ", id); }
  T(T&& o) : id(o.id + 200) { tick(); o.id = -o.id; printf("m%d ", id); }
  T& operator=(const T& o) { tick(); printf("=%d/%d ", id, o.id); id = o.id; return *this; }
  T& operator=(T&& o) { tick(); printf("=m%d/%d ", id, o.id); id = o.id; o.id = -o.id; return *this; }
  ~T() { printf("-%d ", id); }
  explicit operator bool() const { return id % 2 != 0; }
  T operator+(const T& o) const { printf("(+) "); return T(id + o.id); }
  int val() const { return id; }
  T self() const { return *this; }
  const T& me() const { return *this; }
};
T make(int i) { printf("make "); return T(i); }
T pass(T t) { printf("pass "); return t; }
const T& same(const T& t) { return t; }
int use(const T& t) { printf("use%d ", t.id); return t.id; }
int byval(T t) { printf("byval%d ", t.id); return t.id; }
int both(const T& a, const T& b) { return a.id + b.id; }
struct Holder { T a; T b; };
struct Pair2 { T a; int n; T b; Pair2(int i) : a(i), n(i), b(make(i + 1)) { tick(); printf("P "); } ~Pair2() { printf("~P "); } };
class Base { public: T t; Base(int i) : t(i) { printf("B "); } virtual ~Base() { printf("~B "); } };
class Derived : public Base { public: T u; T v; Derived(int i) : Base(i), u(i + 1), v(make(i + 2)) { tick(); printf("D "); } ~Derived() { printf("~D "); } };
struct Deleg { T t; Deleg() : t(1) { printf("d0 "); } Deleg(int i) : Deleg() { tick(); t.id = i; printf("d1 "); } ~Deleg() { printf("~d "); } };
struct Mem { T a = T(1); T b{ 2 }; int n = 3; };
struct WithDtor { T t; WithDtor(int i) : t(i) {} ~WithDtor() { printf("~W "); } };
'''

class G:
    def __init__(s, r):
        s.r, s.n, s.vars, s.k, s.consts = r, 0, [], 1, set()
    def name(s):
        s.n += 1
        return 'v%d' % s.n
    def num(s):
        s.k += 1
        return s.k
    def var(s):
        return s.r.choice(s.vars) if s.vars else None
    def value(s):
        # an expression of type T, at most one temporary maker beside named objects
        r = s.r
        v = s.var()
        c = r.randint(0, 7 if os.environ.get("NOTERNARY") else 8)
        if c <= 2 or not v: return 'make(%d)' % s.num()
        if c == 3: return 'T(%d)' % s.num()
        if c == 4: return 'pass(%s)' % v
        if c == 5: return '%s + make(%d)' % (v, s.num())
        if c == 6: return 'make(%d).self()' % s.num()
        if c == 7: return 'pass(make(%d))' % s.num()
        return '(%s.val() > 0 ? make(%d) : %s)' % (v, s.num(), v)
    def cond(s):
        v = s.var()
        c = s.r.randint(0, 3)
        if c == 0 and v: return '%s.val() %% 3 == 1' % v
        if c == 1: return 'use(make(%d)) %% 2 == 1' % s.num()
        if c == 2 and v: return '(bool)%s' % v
        return '(bool)make(%d)' % s.num()
    def block(s, depth, indent, loop=False):
        mark = len(s.vars)
        lines = []
        for _ in range(s.r.randint(1, 4)):
            lines += s.stmt(depth, indent, loop)
        del s.vars[mark:]
        return lines
    def stmt(s, depth, ind, loop):
        r = s.r
        p = '  ' * ind
        k = r.randint(0, 27)
        n = s.num()
        if depth <= 0: k = r.randint(0, 15)
        v = s.var()
        if k == 0:
            x = s.name(); s.vars.append(x); return [p + 'T %s(%d);' % (x, n)]
        if k == 1:
            x = s.name(); line = p + 'T %s = %s;' % (x, s.value()); s.vars.append(x); return [line]
        if k == 2 and v:
            x = s.name(); s.vars.append(x); return [p + 'T %s = %s;' % (x, v)]
        if k == 3 and v and v not in s.consts:
            x = s.name(); s.vars.append(x); return [p + 'T %s = static_cast<T&&>(%s);' % (x, v)]
        if k == 4:
            x = s.name(); line = p + 'const T& %s = %s;' % (x, s.value()); s.vars.append(x); s.consts.add(x); return [line]
        if k == 5:
            x = s.name(); line = p + 'T&& %s = %s;' % (x, s.value()); s.vars.append(x); return [line]
        if k == 6 and v and v not in s.consts:
            return [p + '%s = %s;' % (v, s.value())]
        if k == 7: return [p + 'use(%s);' % s.value()]
        if k == 8: return [p + 'byval(%s);' % s.value()]
        if k == 9: return [p + '%s;' % s.value()]
        if k == 10 and v: return [p + 'printf("%%d ", both(%s, make(%d)));' % (v, n)]
        if k == 11:
            x = s.name(); return [p + 'Holder %s{ make(%d), make(%d) };' % (x, s.num(), s.num())]
        if k == 12:
            x = s.name(); return [p + 'T %s[3] = { make(%d), make(%d) };' % (x, s.num(), s.num())]
        if k == 13:
            x = s.name(); return [p + r.choice(['Pair2 %s(%d);', 'Derived %s(%d);', 'Deleg %s(%d);', 'WithDtor %s(%d);']) % (x, n)]
        if k == 14:
            x = s.name(); return [p + 'T* %s = new T(%s); delete %s;' % (x, s.value(), x)]
        if k == 15:
            x = s.name(); return [p + r.choice(['Base* %s = new Derived(%d); delete %s;', 'Pair2* %s = new Pair2(%d); delete %s;', 'Deleg* %s = new Deleg(%d); delete %s;']) % (x, n, x)]
        if k == 16:
            return [p + 'if (%s)' % s.cond(), p + '{'] + s.block(depth - 1, ind + 1, loop) + [p + '}', p + 'else', p + '{'] + s.block(depth - 1, ind + 1, loop) + [p + '}']
        if k == 17:
            x = s.name(); line = p + 'if (T %s = %s)' % (x, s.value())
            s.vars.append(x)
            body = [line, p + '{'] + s.block(depth - 1, ind + 1, loop) + [p + '}', p + 'else', p + '{'] + s.block(depth - 1, ind + 1, loop) + [p + '}']
            s.vars.pop()
            return body
        if k == 18:
            i = s.name()
            return [p + 'for (int %s = 0; %s < 2; %s++)' % (i, i, i), p + '{'] + s.block(depth - 1, ind + 1, True) + [p + '}']
        if k == 19:
            x = s.name(); i = s.name()
            head = [p + 'int %s = 0;' % i, p + 'while (T %s = make(%d + %s * 2))' % (x, 2 * n + 1, i), p + '{', p + '  if (++%s == 2) break;' % i]
            s.vars.append(x)
            body = s.block(depth - 1, ind + 1, True)
            s.vars.pop()
            return ['{'] + head + body + [p + '}', '}']
        if k == 20:
            x = s.name(); y = s.name()
            head = [p + 'T %s[2] = { T(%d), T(%d) };' % (y, n, n + 1), p + 'for (T& %s : %s)' % (x, y), p + '{']
            s.vars.append(x)
            body = s.block(depth - 1, ind + 1, True)
            s.vars.pop()
            return ['{'] + head + body + [p + '}', '}']
        if k == 21:
            return [p + 'try', p + '{'] + s.block(depth - 1, ind + 1, False) + [p + '}', p + 'catch (int e)', p + '{', p + '  printf("caught%d ", e);'] + s.block(depth - 1, ind + 1, False) + [p + r.choice(['  throw;', '', ''])] + [p + '}']
        if k == 22:
            return [p + 'switch (%d)' % r.randint(0, 2), p + '{', p + 'case 0:', p + '{'] + s.block(depth - 1, ind + 1, False) + [p + '}', p + '  [[fallthrough]];', p + 'case 1:', p + '{'] + s.block(depth - 1, ind + 1, False) + [p + '  break;', p + '}', p + 'default:', p + '  break;', p + '}']
        if k == 23 and loop:
            return [p + 'if (%s) %s;' % (s.cond(), r.choice(['break', 'continue']))]
        if k == 24:
            return [p + 'if (%s) return %d;' % (s.cond(), n)]
        if k == 25 and v:
            return [p + 'printf("%%d ", [%s] { return %s.val(); }());' % (v, v)]
        if k == 26:
            return [p + 'if (%s) throw %d;' % (s.cond(), n)]
        if k == 27:
            x = s.name(); return [p + 'Mem %s;' % x]
        return [p + 'use(make(%d));' % n]

def main():
    seed, count = int(sys.argv[1]), int(sys.argv[2])
    r = random.Random(seed)
    out = [PRELUDE]
    for i in range(count):
        g = G(r)
        lines = ['int scenario%d()' % i, '{']
        for _ in range(r.randint(3, 7)):
            lines += g.stmt(3, 1, False)
        lines += ['  return 0;', '}']
        out += lines
    out.append('template<class F> void run(int id, F f)')
    out.append('{')
    out.append('  for (int n = 0; n < 40; n++)')
    out.append('  {')
    out.append('    ticks = 0; limit = n; printf("[%d/%d] ", id, n); bool thrown = false;')
    out.append('    try { int r = f(); printf("=> %d", r); } catch (int e) { printf("=> threw %d", e); thrown = true; }')
    out.append('    printf("\\n");')
    out.append('    if (n != 0 && ticks < n) break;')
    out.append('  }')
    out.append('}')
    out.append('int main()\n{')
    for i in range(count): out.append('  run(%d, scenario%d);' % (i, i))
    out.append('  return 0;\n}')
    print('\n'.join(out))

main()
