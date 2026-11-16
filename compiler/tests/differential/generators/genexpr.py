#!/usr/bin/env python3
# Random temporaries inside expressions: `&&` and `||` that construct their right operand only sometimes,
# `?:` branches of prvalues and lvalues (one built, the other not), the comma operator, a reference bound
# to a conditional's prvalue or to a member of a temporary (the whole temporary kept), loop conditions and
# increments (their temporaries destroyed each time), a switch's condition, default arguments, a return of
# a conditional, a throw in a conditional's branch, lambdas called at once, nested conditionals and an
# array's elements from conditionals. Each scenario runs with an exception at its 1st, 2nd, ... construction
# (as genlife), so every cleanup path of a half-evaluated expression runs. Nothing evaluates two temporaries
# unsequenced (no two makers under one + or in one argument list: clang's MSVC target goes right to left).
# One source for kpp and C++; the oracle is the other compiler. Usage: genexpr.py SEED SCENARIOS > file
import random, sys

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
  ~T() { printf("-%d ", id); }
  KEXPLICIT operator bool() const { return id % 2 != 0; }
  int val() const { return id; }
  const T& me() const { return *this; }
};
T make(int i) { printf("mk "); return T(i); }
int use(const T& t) { printf("u%d ", t.id); return t.id; }
int byval(T t) { printf("b%d ", t.id); return t.id; }
struct Holder { T a; T b; int n; };
Holder hold(int i) { printf("hold "); return Holder{T(i), T(i + 1), i}; }
struct Box { T t; Box(int i) : t(i) { tick(); printf("Box "); } ~Box() { printf("~Box "); } const T& get() const { return t; } };
int withDefault(int k, const T& t = make(90)) { printf("wd%d ", t.id + k); return t.id + k; }
int withDefault2(const T& t = T(91), int k = 1) { printf("wd%d ", t.id + k); return t.id + k; }
'''

class G:
    picks = 0
    def __init__(s, r):
        s.r, s.k, s.vars, s.fns = r, 1, [], []
    def num(s):
        s.k += 1
        return s.k
    def lit(s):
        return str(s.r.randint(0, 9))
    def var(s):
        return s.r.choice(s.vars) if s.vars else None
    def tmp(s):
        # one expression of type T making exactly one temporary
        c = s.r.randint(0, 4)
        n = s.num()
        if c == 0: return 'T(%d)' % n
        if c == 1: return 'Box(%d).get()' % n  # an lvalue of the temporary's member: lives to the end of the full-expression
        if c == 2: return 'hold(%d).b' % n
        return 'make(%d)' % n
    def tvalue(s):
        # an expression of type T, prvalue or lvalue, perhaps a conditional
        v = s.var()
        c = s.r.randint(0, 5)
        if c == 0 and v: return v
        if c == 1: return '(%s ? make(%d) : make(%d))' % (s.cond(0), s.num(), s.num())
        if c == 2 and v: return '(%s ? %s : make(%d))' % (s.cond(0), v, s.num())
        return s.tmp()
    def cond(s, depth=1):
        # a bool expression, its temporaries destroyed at the end of the full-expression it is in
        v = s.var()
        c = s.r.randint(0, 6 if depth > 0 else 3)
        if c == 0: return '(bool)%s' % s.tmp()
        if c == 1: return 'use(%s) %% 3 != %s' % (s.tmp(), s.r.randint(0, 2))
        if c == 2 and v: return '%s.val() %% 2 == 0' % v
        if c == 3: return 'byval(%s) %% 2 == 1' % s.tmp()
        if c == 4: return '(%s && %s)' % (s.cond(depth - 1), s.cond(depth - 1))
        if c == 5: return '(%s || %s)' % (s.cond(depth - 1), s.cond(depth - 1))
        return '!%s' % s.tmp()
    def ival(s, depth=1):
        # an int expression with at most one temporary at a time on each sequenced path
        c = s.r.randint(0, 5 if depth > 0 else 2)
        if c == 0: return 'use(%s)' % s.tmp()
        if c == 1: return s.lit()
        if c == 2: return '%s.val()' % s.tvalue()
        if c == 3: return '(%s ? %s : %s)' % (s.cond(), s.ival(depth - 1), s.ival(depth - 1))
        if c == 4: return '(%s, %s)' % (s.ival(depth - 1), s.ival(depth - 1))
        return '[](const T& t) { return t.id * 2; }(%s)' % s.tmp()
    def block(s, depth, ind, loop):
        mark = len(s.vars)
        lines = []
        for _ in range(s.r.randint(1, 3)):
            lines += s.stmt(depth, ind, loop)
        del s.vars[mark:]
        return lines
    def stmt(s, depth, ind, loop):
        r = s.r
        p = '  ' * ind
        k = r.randint(0, 19 if depth > 0 else 12)
        n = s.num()
        name = 'v%d' % n
        if k == 0:
            line = p + 'const T& %s = %s ? make(%d) : make(%d);' % (name, s.cond(), s.num(), s.num())
            s.vars.append(name)
            return [line]
        if k == 1:
            line = p + 'T %s = %s ? make(%d) : T(%d);' % (name, s.cond(), s.num(), s.num())
            s.vars.append(name)
            return [line]
        if k == 2:
            line = p + 'const T& %s = %s;' % (name, r.choice(['hold(%d).a' % s.num(), 'Holder{T(%d), make(%d), 3}.b' % (s.num(), s.num()), 'Box(%d).t' % s.num()]))
            s.vars.append(name)
            return [line]
        if k == 3:
            return [p + 'const int& %s = %s; printf("[%%d] ", %s);' % (name, r.choice(['make(%d).id' % s.num(), 'hold(%d).n' % s.num()]), name)]
        if k == 4:
            return [p + 'printf("[%%d] ", %s);' % s.ival(2)]
        if k == 5:
            return [p + '(void)(%s, %s);' % (s.ival(1), s.ival(1))]
        if k == 6:
            return [p + 'if (%s) printf("yes "); else printf("no ");' % s.cond(2)]
        if k == 7:
            return [p + 'printf("[%%d] ", %s);' % r.choice(['withDefault(%s)' % s.lit(), 'withDefault2()', 'withDefault(1, %s)' % s.tvalue()])]
        if k == 8:
            return [p + 'printf("[%%d] ", %s ? use(%s) : throw %d);' % (s.cond(), s.tmp(), n)]
        if k == 9:
            return [p + 'T %s[2] = { %s ? make(%d) : make(%d), %s };' % (name, s.cond(), s.num(), s.num(), s.tmp())]
        if k == 10:
            return [p + 'printf("[%%d] ", use(%s ? (%s ? make(%d) : make(%d)) : make(%d)));' % (s.cond(0), s.cond(0), s.num(), s.num(), s.num())]
        if k == 11:
            v = s.var()
            if v:
                return [p + 'printf("[%%d] ", use(%s ? %s : %s.me()));' % (s.cond(), v, s.tmp())]
            return [p + 'printf("[%%d] ", use(%s));' % s.tmp()]
        if k == 12:
            G.picks += 1
            f = 'pick%d_%d' % (G.picks, n)
            s.fns.append('T %s(int c) { printf("pick "); %s }' % (f, r.choice([
                'return c %% 2 ? make(%d) : make(%d);' % (s.num(), s.num()),
                'if (c %% 3 == 0) return T(%d); return c %% 2 ? make(%d) : T(%d);' % (s.num(), s.num(), s.num()),
                'T local(%d); return c %% 2 ? local : make(%d);' % (s.num(), s.num())])))
            line = p + 'T %s = %s(%s);' % (name, f, s.ival(0))
            s.vars.append(name)
            return [line]
        if k == 13:
            i = 'i%d' % n
            return [p + 'for (int %s = 0; %s && %s < 3; %s += use(%s) > 0 ? 1 : 2)' % (i, s.cond(0), i, i, s.tmp()), p + '{'] + \
                   [p + '  printf("it%%d ", %s);' % i] + s.block(depth - 1, ind + 1, True) + [p + '}']
        if k == 14:
            i = 'i%d' % n
            head = [p + '{', p + '  int %s = 0;' % i, p + '  while (%s < 2 && use(%s) %% 2 == %d)' % (i, s.tmp(), r.randint(0, 1)), p + '  {', p + '    ++%s;' % i]
            return head + s.block(depth - 1, ind + 2, True) + [p + '  }', p + '}']
        if k == 15:
            return [p + 'switch (use(%s) %% 3)' % s.tmp(), p + '{', p + 'case 0:', p + '{', p + '  printf("c0 ");'] + s.block(depth - 1, ind + 1, False) + \
                   [p + '  KBREAK;', p + '}', p + 'case 1:', p + '  printf("c1 ");', p + '  KFALLTHROUGH;', p + 'default:', p + '  printf("cd ");', p + '  KBREAK;', p + '}']
        if k == 16:
            x = 'w%d' % n
            head = p + 'if (T %s = %s ? make(%d) : make(%d))' % (x, s.cond(), s.num(), s.num())
            s.vars.append(x)
            body = [head, p + '{'] + s.block(depth - 1, ind + 1, loop) + [p + '}']
            s.vars.pop()
            return body
        if k == 17:
            return [p + 'try', p + '{'] + s.block(depth - 1, ind + 1, False) + [p + '}', p + 'catch (int e)', p + '{', p + '  printf("caught%d ", e);', p + r.choice(['  throw;', '', ''])] + [p + '}']
        if k == 18 and loop:
            return [p + 'if (%s) %s;' % (s.cond(), r.choice(['break', 'continue']))]
        if k == 19:
            return [p + 'if (%s) return %s;' % (s.cond(), s.ival(0))]
        return [p + 'use(%s);' % s.tvalue()]

def main():
    seed, count = int(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 8
    r = random.Random(seed)
    out = [PRELUDE]
    bodies = []
    fns = []
    for i in range(count):
        g = G(r)
        lines = ['int scenario%d()' % i, '{']
        for _ in range(r.randint(3, 6)):
            lines += g.stmt(2, 1, False)
        lines += ['  return 0;', '}']
        bodies += lines
        fns += g.fns
    out += fns + bodies
    out.append('template<class F> void run(int id, F f)')
    out.append('{')
    out.append('  for (int n = 0; n < 60; n++)')
    out.append('  {')
    out.append('    ticks = 0; limit = n; printf("[%d/%d] ", id, n);')
    out.append('    try { int r = f(); printf("=> %d", r); } catch (int e) { printf("=> threw %d", e); }')
    out.append('    printf("\\n");')
    out.append('    if (n != 0 && ticks < n) break;')
    out.append('  }')
    out.append('}')
    out.append('int main()\n{')
    for i in range(count): out.append('  run(%d, scenario%d);' % (i, i))
    out.append('  return 0;\n}')
    print('\n'.join(out))

main()
