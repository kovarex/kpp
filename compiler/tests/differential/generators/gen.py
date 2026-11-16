#!/usr/bin/env python3
# Random arithmetic programs, one source for kpp and C++: every test is a constexpr function called at
# compile time (kpp's interpreter) and at run time (kpp's code generator) on volatile arguments, and
# clang or g++ prints the same lines. The generator models C++ values only to stay clear of undefined
# behaviour (signed overflow, bad shifts, float to int out of range): the oracle is the other compiler.
# Usage: gen.py SEED TESTS MODE > file   (MODE: int, float, mixed)
import random, struct, sys, math

class Ty:
    def __init__(s, name, kind, bits=0, signed=True, rank=0, tag=''):
        s.name, s.kind, s.bits, s.signed, s.rank, s.tag = name, kind, bits, signed, rank, tag
    def __repr__(s): return s.name
    def lo(s): return -(1 << (s.bits - 1)) if s.signed else 0
    def hi(s): return (1 << (s.bits - 1)) - 1 if s.signed else (1 << s.bits) - 1

BOOL = Ty('bool', 'b', 1, False, 0, 'b')
CHAR = Ty('char', 'i', 8, True, 1, 'c')
SCHAR = Ty('signed char', 'i', 8, True, 1, 'sc')
UCHAR = Ty('unsigned char', 'i', 8, False, 1, 'uc')
SHORT = Ty('short', 'i', 16, True, 2, 's')
USHORT = Ty('unsigned short', 'i', 16, False, 2, 'us')
INT = Ty('int', 'i', 32, True, 3, 'i')
UINT = Ty('unsigned', 'i', 32, False, 3, 'u')
LL = Ty('long long', 'i', 64, True, 5, 'll')
ULL = Ty('unsigned long long', 'i', 64, False, 5, 'ull')
FLOAT = Ty('float', 'f', 32, True, 7, 'f')
DOUBLE = Ty('double', 'f', 64, True, 8, 'd')
INTS = [CHAR, SCHAR, UCHAR, SHORT, USHORT, INT, UINT, LL, ULL]
ALL = [BOOL] + INTS + [FLOAT, DOUBLE]

class UB(Exception): pass

def f32(x):
    try:
        return struct.unpack('f', struct.pack('f', x))[0]
    except OverflowError:
        raise UB()

def int_to_f32(n):
    m = abs(n)
    s = m.bit_length() - 24
    if s > 0:
        q = m >> s
        r = m & ((1 << s) - 1)
        half = 1 << (s - 1)
        if r > half or (r == half and q & 1):
            q += 1
        m = q << s
    v = float(m)
    return -v if n < 0 else v

def promote(t):
    if t.kind == 'f': return t
    if t.rank < INT.rank: return INT
    return t

def common(a, b):
    if a is DOUBLE or b is DOUBLE: return DOUBLE
    if a is FLOAT or b is FLOAT: return FLOAT
    a, b = promote(a), promote(b)
    if a is b: return a
    if a.signed == b.signed: return a if a.rank > b.rank else b
    u, s = (a, b) if not a.signed else (b, a)
    if u.rank >= s.rank: return u
    if s.bits > u.bits: return s
    return ULL if s is LL else UINT

def wrap(v, t):
    v &= (1 << t.bits) - 1
    if t.signed and v >> (t.bits - 1): v -= 1 << t.bits
    return v

def conv(v, f, t):
    if t is f: return v
    if t is BOOL: return 1 if v != 0 else 0
    if t.kind == 'i':
        if f.kind == 'f':
            if v != v or abs(v) > 1e30: raise UB()
            n = int(v)
            if n < t.lo() or n > t.hi(): raise UB()
            return n
        return wrap(v, t)
    if t is FLOAT:
        if f.kind == 'f': return check(f32(v), FLOAT)
        return int_to_f32(v)
    return float(v)

def check(v, t):
    if v != v: raise UB()
    if abs(v) > (1e30 if t is FLOAT else 1e300): raise UB()
    return v

class N:
    def __init__(s, text, ty, val): s.text, s.ty, s.val = text, ty, val

def par(s): return '(' + s + ')'

def arith(op, a, b, t):
    if t.kind == 'f':
        if op == '+': r = a + b
        elif op == '-': r = a - b
        elif op == '*': r = a * b
        elif op == '/':
            if b == 0: raise UB()
            r = a / b
        if t is FLOAT: r = f32(r)
        return check(r, t)
    if op == '+': r = a + b
    elif op == '-': r = a - b
    elif op == '*': r = a * b
    elif op in '/%':
        if b == 0: raise UB()
        q = abs(a) // abs(b)
        if (a < 0) != (b < 0): q = -q
        r = q if op == '/' else a - q * b
        if t.signed and (q > t.hi() or q < t.lo()): raise UB()
    elif op == '&': r = a & b
    elif op == '|': r = a | b
    elif op == '^': r = a ^ b
    if t.signed:
        if r < t.lo() or r > t.hi(): raise UB()
        return r
    return wrap(r, t)

class Gen:
    def __init__(s, rng, mode):
        s.r, s.mode = rng, mode
        if mode == 'int': s.types = [BOOL] + INTS
        elif mode == 'float': s.types = [INT, UINT, LL, ULL, UCHAR, SHORT, FLOAT, DOUBLE, FLOAT, DOUBLE, FLOAT, DOUBLE]
        else: s.types = ALL
        s.params = []

    def edge(s, t):
        r = s.r
        if t is BOOL: return r.randint(0, 1)
        if t.kind == 'i':
            c = r.randint(0, 7)
            if c == 0: return r.choice([t.lo(), t.hi()])
            if c == 1: return r.choice([0, 1, t.hi() // 2 + 1, t.hi() // 2])
            if c == 2: return wrap(-1, t)
            if c == 3: return wrap(1 << r.randint(0, t.bits - 1), t)
            if c == 4: return r.randint(max(t.lo(), -9), min(t.hi(), 9))
            return r.randint(t.lo(), t.hi())
        c = r.randint(0, 6)
        if c == 0: v = r.choice([0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5, -2.5, 3.5, 0.1, 0.2, 0.3, 1e-3, 16777216.0, 16777217.0, 2147483648.0, 4294967296.0, 9007199254740993.0, 1e10, 1e-10, 1e-40, 3e38 if t is DOUBLE else 1e20, 0.99999999, 255.5, 127.9, -128.9])
        elif c == 1: v = r.randint(-1000, 1000) / r.choice([1, 2, 4, 8, 3, 7, 10, 100])
        elif c == 2: v = float(r.randint(-(1 << 40), 1 << 40))
        elif c == 3: v = r.uniform(-1, 1)
        elif c == 4: v = r.uniform(-1e6, 1e6)
        elif c == 5: v = math.ldexp(r.uniform(1, 2), r.randint(-60, 60))
        else: v = r.uniform(-300, 300)
        return f32(v) if t is FLOAT else v

    def lit_text(s, v, t):
        if t is BOOL: return 'true' if v else 'false'
        if t.kind == 'f':
            if v == 0 and math.copysign(1, v) < 0: return '(-0.0)' if t is DOUBLE else '(-0.0f)'
            return repr(v) if t is DOUBLE else '(float)' + par(repr(v))
        if t is LL and v == LL.lo(): return '(-9223372036854775807ll-1)'
        if t is INT and v == INT.lo(): return '(-2147483647-1)'
        suffix = {'ull': 'ull', 'll': 'll', 'u': 'u'}.get(t.tag, '')
        if t.tag in ('i', 'u', 'll', 'ull'):
            return (par(str(v) + suffix) if v < 0 else str(v) + suffix)
        return '(' + t.name + ')' + par(str(v))

    def leaf(s, t=None):
        r = s.r
        t = t or r.choice(s.types)
        if r.random() < 0.6 and len(s.params) < 5:
            v = s.edge(t)
            name = 'a%d' % len(s.params)
            s.params.append((name, t, v))
            return N(name, t, v)
        if s.params and r.random() < 0.5:
            name, pt, v = r.choice(s.params)
            return N(name, pt, v)
        k = r.randint(0, 5)
        if k == 0:
            v = r.randint(0, 200)
            return N(hex(v), INT, v)
        if k == 1 and s.mode != 'float':
            v = r.choice([0x7fffffff, 0x80000000, 0xffffffff, 0xff, 0xffff])
            return N(hex(v), INT if v <= 0x7fffffff else UINT, v)
        if k == 2 and s.mode != 'float':
            c = r.choice(['a', 'z', '0', ' ', '\\x7f', '\\xff', '\\x80', '\\n', '\\0'])
            v = {'\\x7f': 127, '\\xff': -1, '\\x80': -128, '\\n': 10, '\\0': 0}.get(c)
            if v is None: v = ord(c)
            return N("'" + c + "'", CHAR, v)
        v = s.edge(t)
        return N(s.lit_text(v, t), t, v)

    def as_int(s, n):
        if n.ty.kind != 'f': return n
        t = s.r.choice([INT, LL, UINT])
        return N('(' + t.name + ')' + par(n.text), t, conv(n.val, n.ty, t))

    def cast(s, n):
        t = s.r.choice(s.types)
        v = conv(n.val, n.ty, t)
        form = s.r.randint(0, 2)
        if form == 0: text = 'static_cast<' + t.name + '>' + par(n.text)
        elif form == 1 and ' ' not in t.name: text = t.name + par(par(n.text))
        else: text = '(' + t.name + ')' + par(n.text)
        return N(text, t, v)

    def expr(s, d):
        r = s.r
        for _ in range(30):
            mark = len(s.params)
            try:
                return s.expr1(d)
            except UB:
                del s.params[mark:]
        return s.safe_leaf()

    def safe_leaf(s):
        v = s.r.randint(0, 100)
        return N(str(v), INT, v)

    def expr1(s, d):
        r = s.r
        if d <= 0 or r.random() < 0.12: return s.leaf()
        k = r.randint(0, 19)
        if k <= 8:
            op = r.choice(['+', '-', '*', '/', '%', '&', '|', '^'] if s.mode != 'float' else ['+', '-', '*', '/', '+', '-', '*', '/', '%'])
            a, b = s.expr(d - 1), s.expr(d - 1)
            if op in '%&|^':
                a, b = s.as_int(a), s.as_int(b)
            t = common(a.ty, b.ty)
            v = arith(op, conv(a.val, a.ty, t), conv(b.val, b.ty, t), t)
            return N(par(a.text + ' ' + op + ' ' + b.text), t, v)
        if k <= 10:
            op = r.choice(['<<', '>>'])
            a, b = s.as_int(s.expr(d - 1)), s.as_int(s.expr(d - 1))
            t = promote(a.ty)
            mask = t.bits - 1
            bt = common(b.ty, INT)
            c = conv(b.val, b.ty, bt) & mask
            count = par(b.text + ' & ' + str(mask))
            av = conv(a.val, a.ty, t)
            v = wrap(av << c, t) if op == '<<' else av >> c
            return N(par(a.text + ' ' + op + ' ' + count), t, v)
        if k <= 12:
            op = r.choice(['<', '<=', '>', '>=', '==', '!='])
            a, b = s.expr(d - 1), s.expr(d - 1)
            t = common(a.ty, b.ty)
            x, y = conv(a.val, a.ty, t), conv(b.val, b.ty, t)
            v = {'<': x < y, '<=': x <= y, '>': x > y, '>=': x >= y, '==': x == y, '!=': x != y}[op]
            return N(par(a.text + ' ' + op + ' ' + b.text), BOOL, int(v))
        if k == 13:
            op = r.choice(['&&', '||'])
            a, b = s.expr(d - 1), s.expr(d - 1)
            x, y = a.val != 0, b.val != 0
            return N(par(a.text + ' ' + op + ' ' + b.text), BOOL, int((x and y) if op == '&&' else (x or y)))
        if k == 14:
            a = s.expr(d - 1)
            op = r.choice(['-', '+', '~', '!'])
            if op == '!': return N(par('!' + a.text), BOOL, int(a.val == 0))
            if op == '~': a = s.as_int(a)
            t = promote(a.ty)
            v = conv(a.val, a.ty, t)
            if op == '~': v = wrap(~v, t)
            elif op == '-':
                if t.kind == 'f': v = -v
                elif t.signed:
                    if v == t.lo(): raise UB()
                    v = -v
                else: v = wrap(-v, t)
            return N(par(op + ' ' + a.text), t, v)
        if k == 15:
            c, a, b = s.expr(d - 1), s.expr(d - 1), s.expr(d - 1)
            t = a.ty if a.ty is b.ty else common(a.ty, b.ty)
            v = conv(a.val if c.val != 0 else b.val, a.ty if c.val != 0 else b.ty, t)
            # (both arms convert in C++, but only the taken one is evaluated)
            return N(par(c.text + ' ? ' + a.text + ' : ' + b.text), t, v)
        return s.cast(s.expr(d - 1))

    # one test: a constexpr function body and the values of its parameters
    def test(s, d):
        s.params = []
        r = s.r
        k = r.randint(0, 9)
        try:
            if k <= 4:
                e = s.expr(d)
                return ['  return ' + e.text + ';'], s.params
            if k <= 7:
                return s.compound(d)
            return s.incdec(d)
        except UB:
            return ['  return 0;'], []

    def compound(s, d):
        r = s.r
        t = r.choice(s.types)
        init = s.edge(t)
        lines = ['  ' + t.name + ' x = ' + s.lit_text(init, t) + ';']
        v = init
        for i in range(r.randint(1, 3)):
            ops = ['+=', '-=', '*=', '/=']
            if t.kind != 'f': ops += ['%=', '&=', '|=', '^=', '<<=', '>>=']
            op = r.choice(ops)
            e = s.expr(d - 1)
            if op[:-1] in ('%', '&', '|', '^', '<<', '>>') or t is BOOL: e = s.as_int(e)
            if op in ('<<=', '>>='):
                p = promote(t)
                mask = p.bits - 1
                bt = common(e.ty, INT)
                c = conv(e.val, e.ty, bt) & mask
                pv = conv(v, t, p)
                res = wrap(pv << c, p) if op == '<<=' else pv >> c
                nv = conv(res, p, t)
                text = par(e.text + ' & ' + str(mask))
            else:
                c = common(t, e.ty)
                res = arith(op[0], conv(v, t, c), conv(e.val, e.ty, c), c)
                nv = conv(res, c, t)
                text = e.text
            lines.append('  x ' + op + ' ' + text + ';')
            v = nv
        lines.append('  return x;')
        return lines, s.params

    def incdec(s, d):
        r = s.r
        t = r.choice([x for x in s.types if x is not BOOL])
        init = s.edge(t)
        if t.kind == 'i' and t.bits >= 32 and t.signed and init in (t.lo(), t.hi()): init = 5
        if t.kind == 'f' and abs(init) > 1e15: init = 2.5
        lines = ['  ' + t.name + ' x = ' + s.lit_text(init, t) + ';']
        lines.append('  ' + t.name + ' y = ' + r.choice(['x++', '++x', 'x--', '--x']) + ';')
        lines.append('  return ' + r.choice(['x', 'y']) + ';')
        return lines, []

def main():
    seed, count, mode = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    r = random.Random(seed)
    g = Gen(r, mode)
    out = ['extern "C" int printf(const char* format, ...);']
    for t in [BOOL, CHAR, SCHAR, UCHAR, SHORT, USHORT, INT]:
        out.append('void show(int id, char kind, %s v) { printf("%%d %%c %s %%d\\n", id, kind, (int)v); }' % (t.name, t.tag))
    out.append('void show(int id, char kind, unsigned v) { printf("%d %c u %u\\n", id, kind, v); }')
    out.append('void show(int id, char kind, long long v) { printf("%d %c ll %lld\\n", id, kind, v); }')
    out.append('void show(int id, char kind, unsigned long long v) { printf("%d %c ull %llu\\n", id, kind, v); }')
    out.append('void show(int id, char kind, float v) { printf("%d %c f %a\\n", id, kind, (double)v); }')
    out.append('void show(int id, char kind, double v) { printf("%d %c d %a\\n", id, kind, v); }')
    calls = []
    for i in range(count):
        lines, params = g.test(r.randint(2, 5))
        out.append('constexpr auto f%d(%s)' % (i, ', '.join(t.name + ' ' + n for n, t, v in params)))
        out.append('{')
        out += lines
        out.append('}')
        args = ', '.join(g.lit_text(v, t) for n, t, v in params)
        out.append('constexpr auto c%d = f%d(%s);' % (i, i, args))
        calls.append('  show(%d, \'c\', c%d);' % (i, i))
        decl = ''.join('  volatile %s v%d_%d = %s;\n' % (t.name, i, j, g.lit_text(v, t)) for j, (n, t, v) in enumerate(params))
        calls.append('  {\n' + decl + '  show(%d, \'r\', f%d(%s));\n  }' % (i, i, ', '.join('v%d_%d' % (i, j) for j in range(len(params)))))
    n = 0
    for start in range(0, len(calls), 200):
        out.append('void part%d()\n{' % n)
        out += calls[start:start + 200]
        out.append('}')
        n += 1
    out.append('int main()\n{')
    out += ['  part%d();' % i for i in range(n)]
    out.append('  return 0;\n}')
    print('\n'.join(out))

main()
