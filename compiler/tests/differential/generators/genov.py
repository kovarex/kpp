#!/usr/bin/env python3
# Random overload sets (plain functions and templates, one or two parameters of arithmetic, enum,
# pointer, class and reference types, default arguments) called with arguments of every kind of
# expression; clang -fsyntax-only drops each call it rejects (ambiguous, no viable function) and each
# set it rejects, the rest print which overload each call chose. One source for kpp and C++ (the C++
# side gets cxx-head.h: nullptr_t). Literal 0 and long double are left out: kpp differs by design.
# Usage: genov.py SEED [SETS] > file
import random, sys, subprocess, os, re

r = random.Random(int(sys.argv[1]))
NS = int(sys.argv[2]) if len(sys.argv) > 2 else 12
CLANG = os.environ.get('KPP_DIFF_CXX') or r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'
HERE = os.path.dirname(os.path.abspath(__file__))

PRELUDE = r'''extern "C" int printf(const char* format, ...);
enum E1 { e1a = 1, e1b = 300 };
enum E2 : unsigned char { e2a = 2 };
enum class E3 { a = 3 };
class B { public: int v = 1; };
class D : public B { public: int w = 2; };
class DD : public D { public: int x = 3; };
class Conv { public: int v; IMPLICIT Conv(int x) : v(x) {} };
class ConvD { public: double v; IMPLICIT ConvD(double x) : v(x) {} };
class ToInt { public: IMPLICIT operator int() const { return 3; } };
class ToDouble { public: IMPLICIT operator double() const { return 4.5; } };
class ToB { public: IMPLICIT operator B() const { return B(); } };
class ToPtr { public: IMPLICIT operator int*() const { return nullptr; } };
int vi = 1; long vl = 2; unsigned vu = 3; short vs = 4; char vc = 'c'; signed char vsc = 5; unsigned char vuc = 6;
bool vb = true; float vf = 1.5f; double vd = 2.5; long long vll = 7; unsigned long long vull = 8; unsigned short vus = 9;
const int ci = 10; const double cd = 3.5; E1 ve1 = e1b; E2 ve2 = e2a; E3 ve3 = E3::a;
int* pi = &vi; const int* cpi = &vi; void* pv = &vi; const char* str = "s"; char buf[4] = "ab";
B b; D d; DD dd; const D cdd; B* pb = &b; D* pd = &d; const D* cpd = &d; DD* pdd = &dd;
Conv conv{5}; ToInt toi; ToDouble tod; ToB tob; ToPtr top; int arr[3] = {1, 2, 3};
int fun(int x) { return x; }
'''

ARGS = ['1', '2u', '3l', '4ll', '5ull', "'a'", '1.5f', '2.5', 'true', 'nullptr', '"lit"', 'vi', 'vl', 'vu', 'vs',
        'vc', 'vsc', 'vuc', 'vb', 'vf', 'vd', 'vll', 'vull', 'vus', 'ci', 'cd', 've1', 've2', 've3', 'e1a', 'e2a',
        'E3::a', 'pi', 'cpi', 'pv', 'str', 'buf', 'b', 'd', 'dd', 'cdd', 'pb', 'pd', 'cpd', 'pdd', 'conv', 'toi',
        'tod', 'tob', 'top', 'arr', 'int(2)', 'B()', 'D()', 'Conv(7)', 'static_cast<short>(1)', 'vi + vl',
        'vc + vc', 'vb ? vi : vl', 'vf * 2', '-vu', '&vi', '&d', 'fun', '*pd', 'vi++', '++vi', 'vd = 1.0',
        'static_cast<int&&>(vi)', 'static_cast<D&&>(d)', 'static_cast<const int&>(vi)']
PARAMS = ['int', 'long', 'unsigned', 'short', 'char', 'signed char', 'unsigned char', 'bool', 'float', 'double',
          'long long', 'unsigned long long', 'unsigned short', 'E1', 'E2', 'E3', 'int*', 'const int*', 'void*',
          'const void*', 'B*', 'D*', 'const B*', 'const D*', 'B', 'const B&', 'B&', 'B&&', 'D', 'const D&', 'D&',
          'Conv', 'const Conv&', 'ConvD', 'int&', 'const int&', 'int&&', 'double&', 'const double&', 'long&',
          'const long&', 'const char*', 'char*', 'nullptr_t', 'ToInt', 'const B* const&']
TEMPLATES = ['T', 'T*', 'const T&', 'T&&', 'T&', 'const T*']

def overload(name, k, used):
    for _ in range(20):
        c = r.random()
        if c < 0.2:
            n = r.choice([1, 1, 2])
            ps = [r.choice(TEMPLATES) for _ in range(n)]
            sig = 'template<class T> ' + ','.join(ps)
            if sig in used: continue
            used.add(sig)
            params = ', '.join(ps)  # (unnamed: the overload reads none, and kpp's unused names are errors)
            return 'template<class T> int %s(%s) { return %d; }' % (name, params, k)
        n = r.choice([1, 1, 1, 2, 2])
        ps = [r.choice(PARAMS) for _ in range(n)]
        dflt = n == 2 and r.random() < 0.15 and ps[1] in ('int', 'double', 'long', 'bool', 'char')
        key = ','.join(ps)
        if key in used or (dflt and ps[0] in used): continue
        used.add(key)
        if dflt: used.add(ps[0])
        params = ', '.join('%s%s' % (p, ' = 1' if dflt and i == 1 else '') for i, p in enumerate(ps))
        return 'int %s(%s) { return %d; }' % (name, params, k)
    return None

sets = []   # (name, [definition lines], [call lines])
for s in range(NS):
    name = 'ov%d' % s
    used = set()
    defs = [x for x in (overload(name, 10 * s + k, used) for k in range(r.randint(2, 5))) if x]
    calls = []
    for _ in range(25):
        n = r.choice([1, 1, 2])
        calls.append('%s(%s)' % (name, ', '.join(r.choice(ARGS) for _ in range(n))))
    sets.append([name, defs, sorted(set(calls))])

def render():
    lines = PRELUDE.split('\n')
    where = {}
    for si, (name, defs, calls) in enumerate(sets):
        for dl in defs:
            where[len(lines) + 1] = ('def', si)
            lines.append(dl)
    lines.append('int main()')
    lines.append('{')
    for si, (name, defs, calls) in enumerate(sets):
        if not defs: continue
        lines.append('  printf("%s:");' % name)
        for ci, c in enumerate(calls):
            where[len(lines) + 1] = ('call', si, ci)
            lines.append('  { int r = %s; printf(" %%d", r); }' % c)
        lines.append('  printf("\\n");')
    lines.append('  return 0;')
    lines.append('}')
    return '\n'.join(lines), where

head = open(os.path.join(HERE, 'cxx-head.h')).read()
headLines = head.count('\n')
tmp = os.path.join(os.environ.get('KPP_DIFF_TMP', HERE), 'ovtmp-%s.cpp' % sys.argv[1])
for it in range(8):
    text, where = render()
    open(tmp, 'w').write(head + text + '\n')
    p = subprocess.run([CLANG, '-std=c++20', '-Wno-everything', '-Werror=writable-strings', '-fsyntax-only', '-ferror-limit=0', '-fno-ms-compatibility', '-Werror=writable-strings', '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', tmp],
                       capture_output=True, text=True)
    errs = set()
    for m in re.finditer(r'ovtmp-[^:]*\.cpp:(\d+):\d+: error', p.stderr):
        errs.add(int(m.group(1)) - headLines)
    if not errs:
        break
    dropCalls, dropSets = set(), set()
    for e in errs:
        w = where.get(e)
        if not w:
            sys.stderr.write('unplaced error at %d\n' % e)
            dropSets = set(range(len(sets)))
            break
        if w[0] == 'def': dropSets.add(w[1])
        else: dropCalls.add((w[1], w[2]))
    for si in dropSets:
        sets[si][1] = []
    for si, ci in sorted(dropCalls, reverse=True):
        if ci < len(sets[si][2]): del sets[si][2][ci]
os.remove(tmp)
text, where = render()
print(text)
