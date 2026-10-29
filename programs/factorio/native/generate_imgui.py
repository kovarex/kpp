"""Generates dear imgui for Kpp: the game's imgui 1.53 (libraries/imgui) through a thin C interface.

imgui is C++ (namespace ImGui with overloads and default arguments, structs with methods, the
ImVector template) and Kpp calls C only (docs/design.md, "Interop: C only"). This script reads
imgui.h and writes both sides of the interface:

- native/imgui_c.cpp: one extern "C" function per function of namespace ImGui (kppImGui_<Name>,
  numbered from the second overload on; imgui.h's, and those of imgui_internal.h the game calls,
  INTERNAL_FUNCTIONS) and per method of the structs Kpp mirrors
  (kpp<Struct>_<Name>, kpp<Struct>_construct and kpp<Struct>_destruct), each calling imgui. Classes
  cross by pointer (an ImVec2 argument, a reference, a returned ImVec2 through an out pointer); a C
  variadic function formats through imgui's va_list variant. It also asserts the layout this
  script gives Kpp: every mirrored struct's size and the offset of each of its fields.
- overrides/added/src/Debug/ImGui.kpp: imgui's types for Kpp, with imgui's names: the typedefs,
  the enums, the structs with imgui's layouts (the game reads and writes ImGuiIO, ImGuiStyle and
  the draw lists imgui owns), ImVec2, ImVec4, ImColor and ImVector written out (their bodies are
  imgui's inline code), the structs' methods and namespace ImGui with imgui's signatures and default
  arguments, calling the C functions, and imgui_internal.h's ImVec2/ImVec4 operators
  (IMGUI_DEFINE_MATH_OPERATORS, which DebugTools.cpp turns on). static_asserts check the sizes.

Structs Kpp declares without a definition (only pointers to them cross): ImDrawListSharedData,
ImGuiContext, ImGuiStorage, ImGuiTextFilter, ImGuiTextBuffer and ImGuiOnceUponAFrame. The va_list
variants (TextV, ...) are left out: Kpp has no va_list.

    python generate_imgui.py [imgui directory] [--shim FILE] [--kpp FILE]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FACTORIO = os.path.dirname(HERE)

args = sys.argv[1:]
IMGUI = r'C:\Factorio\libraries\imgui'
SHIM = os.path.join(HERE, 'imgui_c.cpp')
KPP = os.path.join(FACTORIO, 'overrides', 'added', 'src', 'Debug', 'ImGui.kpp')
while args:
    a = args.pop(0)
    if a == '--shim':
        SHIM = args.pop(0)
    elif a == '--kpp':
        KPP = args.pop(0)
    else:
        IMGUI = a

# The structs Kpp only declares, and the ones written out below instead of generated.
OPAQUE = ['ImDrawListSharedData', 'ImGuiContext', 'ImGuiStorage', 'ImGuiTextFilter', 'ImGuiTextBuffer', 'ImGuiOnceUponAFrame']
WRITTEN = ['ImVec2', 'ImVec4', 'ImColor', 'ImVector']
# The macros the shim's build defines, for imgui.h's #if lines (clang-cl, imconfig.h's ImDrawIdx).
DEFINED = {'_MSC_VER', '_WIN32', '__clang__', 'ImDrawIdx'}

SCALARS = {'bool': 1, 'char': 1, 'signed char': 1, 'unsigned char': 1, 'short': 2, 'unsigned short': 2, 'int': 4, 'unsigned int': 4,
           'float': 4, 'double': 8, 'size_t': 8, 'long long': 8, 'unsigned long long': 8}
TYPE_WORDS = set(SCALARS) | {'void', 'unsigned', 'signed', 'long', 'const'}


def fail(message):
    sys.exit('generate_imgui: ' + message)


def read(path):
    with open(path, encoding='utf-8-sig') as f:
        return f.read()


def strip_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith('//', i):
            while i < n and text[i] != '\n':
                i += 1
        elif text.startswith('/*', i):
            end = text.find('*/', i + 2)
            end = n if end < 0 else end + 2
            out.append(''.join('\n' if c == '\n' else ' ' for c in text[i:end]))
            i = end
        elif text[i] in '"\'':
            j = i + 1
            while j < n and text[j] != text[i]:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def evaluate(condition, defined):
    expr = re.sub(r'defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)', lambda m: '1' if (m.group(1) or m.group(2)) in defined else '0', condition)
    expr = expr.replace('&&', ' and ').replace('||', ' or ')
    expr = re.sub(r'!(?!=)', ' not ', expr)
    if re.search(r'[A-Za-z_]\w*', re.sub(r'\b(and|or|not)\b', '', expr)):
        fail('an #if this script cannot evaluate: ' + condition)
    return bool(eval(expr))


def preprocess(text, defined):
    """The lines the configuration keeps (the others blank), and the object-like #defines."""
    out, macros, stack, active = [], {}, [], True
    for line in text.split('\n'):
        m = re.match(r'\s*#\s*(\w+)\s*(.*)', line)
        if not m:
            out.append(line if active else '')
            continue
        directive, rest = m.group(1), m.group(2).strip()
        if directive in ('if', 'ifdef', 'ifndef'):
            if directive == 'ifdef':
                value = rest.split()[0] in defined
            elif directive == 'ifndef':
                value = rest.split()[0] not in defined
            else:
                value = evaluate(rest, defined)
            stack.append((active, value))
            active = active and value
        elif directive == 'else':
            parent, taken = stack[-1]
            stack[-1] = (parent, True)
            active = parent and not taken
        elif directive == 'endif':
            active = stack.pop()[0]
        elif directive == 'elif':
            fail('#elif is not handled')
        elif directive == 'define' and active:
            d = re.match(r'(\w+)(\()?\s*(.*)', rest)
            defined.add(d.group(1))
            if not d.group(2):
                macros[d.group(1)] = d.group(3).strip()
        out.append('')
    return '\n'.join(out), macros


def matching(text, i):
    """The index of the bracket closing the one at i."""
    pairs = {'(': ')', '{': '}', '[': ']', '<': '>'}
    opening, closing, depth = text[i], pairs[text[i]], 0
    for j in range(i, len(text)):
        if text[j] == opening:
            depth += 1
        elif text[j] == closing:
            depth -= 1
            if depth == 0:
                return j
    fail('unbalanced ' + opening)


def split_statements(text):
    """Declarations: up to a ';' outside brackets, or a '}' closing a body (with its ';')."""
    statements, start, i, parens, n = [], 0, 0, 0, len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            i = j + 1
            continue
        if c == '(':
            parens += 1
        elif c == ')':
            parens -= 1
        elif c == ';' and parens == 0:
            statements.append(text[start:i].strip())
            start = i + 1
        elif c == '{' and parens == 0:
            end = matching(text, i)
            k = end + 1
            while k < n and text[k] in ' \t\r\n':
                k += 1
            if k < n and text[k] == ';':
                end = k
            statements.append(text[start:end + 1].strip().rstrip(';').strip())
            start = i = end + 1
            continue
        i += 1
    if text[start:].strip():
        fail('a declaration without its end: ' + text[start:].strip()[:80])
    return [s for s in statements if s]


def split_commas(text):
    parts, depth, current = [], 0, ''
    for c in text:
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == ',' and depth == 0:
            parts.append(current.strip())
            current = ''
        else:
            current += c
    if current.strip():
        parts.append(current.strip())
    return parts


def normalize(type_text):
    t = re.sub(r'\s+', ' ', type_text).strip()
    t = re.sub(r'\s*\*\s*', '*', t)
    t = re.sub(r'\s*&\s*', '&', t)
    return t.replace('*', '* ').replace('&', '& ').replace('* *', '**').replace('* &', '*&').strip()


class Param:
    def __init__(self, text, index):
        self.default = None
        self.variadic = text == '...'
        self.function = None  # (return type, parameter list) of a function pointer parameter
        if self.variadic:
            self.type, self.name = '...', 'args'
            return
        depth = 0
        for k, c in enumerate(text):
            if c in '([{':
                depth += 1
            elif c in ')]}':
                depth -= 1
            elif c == '=' and depth == 0:
                self.default = text[k + 1:].strip()
                text = text[:k].strip()
                break
        m = re.match(r'^(.*?)\(\s*\*\s*(\w*)\s*\)\s*\((.*)\)$', text, re.S)
        if m:
            self.function = (normalize(m.group(1)), m.group(3).strip())
            self.name = m.group(2) or 'arg%d' % index
            self.type = '%s (*)(%s)' % self.function
            return
        m = re.match(r'^(.*?)\b(\w+)\s*\[[^\]]*\]$', text)
        if m:
            self.type, self.name = normalize(m.group(1)) + '*', m.group(2)  # an array parameter is a pointer
            return
        m = re.match(r'^(.*[\s*&])(\w+)$', text)
        if m and m.group(2) not in TYPE_WORDS and not re.match(r'^(const|unsigned|signed)\s*$', m.group(1).strip()):
            self.type, self.name = normalize(m.group(1)), m.group(2)
        else:
            self.type, self.name = normalize(text), 'arg%d' % index

    def declarator(self, type_text=None):
        """The parameter as C++ and Kpp write it, with the given type in place of its own."""
        if self.function:
            return '%s (*%s)(%s)' % (self.function[0], self.name, self.function[1])
        return '%s %s' % (type_text or self.type, self.name)


class Function:
    """A function of namespace ImGui, or a method, constructor or destructor of a struct."""

    def __init__(self, statement, owner=None):
        text = statement
        brace = None
        depth = 0
        for k, c in enumerate(text):
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            elif c == '{' and depth == 0:
                brace = k
                break
        if brace is not None:
            text = text[:brace]
        text = re.sub(r'\bIM_FMT(ARGS|LIST)\s*\(\s*\d+\s*\)', '', text)
        self.static = bool(re.search(r'\bstatic\b', text)) and owner is not None
        text = re.sub(r'\b(IMGUI_API|inline|static)\b', ' ', text).strip()
        open_ = None
        for m in re.finditer(r'(~?\s*\w+)\s*\(', text):
            open_ = m.end() - 1
            break
        if open_ is None:
            fail('no parameter list: ' + statement[:80])
        close = matching(text, open_)
        head, params, tail = text[:open_].strip(), text[open_ + 1:close].strip(), text[close + 1:].strip()
        m = re.match(r'^(.*?)(~?)\s*(\w+)$', head, re.S)
        self.returns, self.name = normalize(m.group(1)), m.group(3)
        self.destructor = bool(m.group(2))
        self.constructor = owner is not None and not self.destructor and self.returns == '' and self.name == owner.name
        self.const = tail.startswith('const')
        if tail not in ('', 'const'):
            fail('unexpected text after the parameters of %s: %s' % (self.name, tail))
        self.params = [] if params in ('', 'void') else [Param(p, k) for k, p in enumerate(split_commas(params))]
        self.owner = owner

    def variadic(self):
        return bool(self.params) and self.params[-1].variadic

    def takes_va_list(self):
        return any(p.type == 'va_list' for p in self.params)


class Struct:
    def __init__(self, name, body, outer=None):
        self.name, self.outer = name, outer
        self.fields = []  # (type, name, array size or None), or ('fn', name, (return type, parameters))
        self.functions, self.nested, self.typedefs = [], [], []
        for statement in split_statements(body):
            statement = re.sub(r'^(public|private|protected)\s*:\s*', '', statement)
            if statement.startswith('typedef '):
                m = re.match(r'typedef\s+(.*?)\s*(\w+)$', statement, re.S)
                self.typedefs.append((m.group(2), normalize(m.group(1))))
                continue
            m = re.match(r'^struct\s+(\w+)\s*\{(.*)\}$', statement, re.S)
            if m:
                self.nested.append(Struct(m.group(1), m.group(2), self))
                continue
            m = re.match(r'^(.*?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$', statement, re.S)
            if m:
                self.fields.append(('fn', m.group(2), (normalize(m.group(1)), m.group(3).strip())))
                continue
            if '(' in statement:
                self.functions.append(Function(statement, self))
                continue
            m = re.match(r'^(.*?)\s*\b(\w+\s*(\[[^\]]*\])?(\s*,\s*\w+\s*(\[[^\]]*\])?)*)$', statement, re.S)
            if not m:
                fail('a member this script does not read: ' + statement[:80])
            base = normalize(statement[:m.start(2)])
            for declarator in split_commas(m.group(2)):
                d = re.match(r'(\w+)\s*(?:\[([^\]]*)\])?$', declarator)
                self.fields.append((base, d.group(1), d.group(2)))

    def qualified(self):
        return self.name if not self.outer else self.outer.qualified() + '::' + self.name

    def c_name(self):
        return self.qualified().replace('::', '_')


# ---------------------------------------------------------------------------------------------
# Reading imgui.h

header = strip_comments(read(os.path.join(IMGUI, 'imgui.h')))
config = strip_comments(read(os.path.join(IMGUI, 'imconfig.h')))
m = re.search(r'#\s*define\s+ImDrawIdx\s+([^\n]+)', config)
DRAW_IDX = m.group(1).strip() if m else None
text, MACROS = preprocess(header, set(DEFINED))

typedefs = []        # (name, statement)
typedef_types = {}   # name -> underlying type (a function pointer type: '*')
enums = []           # (name, [(enumerator, value text)])
structs = {}         # name -> Struct
struct_order = []
functions = []       # namespace ImGui, in order
forward = []

for statement in split_statements(text):
    if statement.startswith('typedef '):
        typedefs.append(statement)
        fp = re.match(r'typedef\s+.*\(\s*\*\s*(\w+)\s*\)', statement, re.S)
        if fp:
            typedef_types[fp.group(1)] = '*'
        else:
            m = re.match(r'typedef\s+(.*?)\s*(\w+)$', statement, re.S)
            typedef_types[m.group(2)] = normalize(m.group(1))
        continue
    m = re.match(r'^struct\s+(\w+)$', statement)
    if m:
        forward.append(m.group(1))
        continue
    m = re.match(r'^enum\s+(\w+)\s*\{(.*)\}$', statement, re.S)
    if m:
        values = []
        for e in split_commas(m.group(2)):
            n = re.match(r'(\w+)\s*(?:=\s*(.*))?$', e.strip(), re.S)
            values.append((n.group(1), re.sub(r'\s+', ' ', n.group(2)).strip() if n.group(2) else None))
        enums.append((m.group(1), values))
        continue
    m = re.match(r'^struct\s+(\w+)\s*\{(.*)\}$', statement, re.S)
    if m:
        if m.group(1) not in WRITTEN and m.group(1) not in OPAQUE:
            structs[m.group(1)] = Struct(m.group(1), m.group(2))
            struct_order.append(m.group(1))
        continue
    m = re.match(r'^namespace\s+ImGui\s*\{(.*)\}$', statement, re.S)
    if m:
        functions += [Function(s) for s in split_statements(m.group(1))]
        continue
    if re.match(r'^template\s*<\s*typename\s+T\s*>\s*class\s+ImVector\b', statement):
        continue
    fail('a declaration this script does not read: ' + statement[:80])

if DRAW_IDX:
    typedef_types['ImDrawIdx'] = DRAW_IDX
    typedefs.append('typedef %s ImDrawIdx' % DRAW_IDX)

# Enumerator values, for array sizes.
ENUM_VALUES = {}
for name, values in enums:
    nxt = 0
    for enumerator, value in values:
        if value is not None:
            nxt = eval(re.sub(r'\b[A-Za-z_]\w*\b', lambda m: str(ENUM_VALUES[m.group(0)]), value))
        ENUM_VALUES[enumerator] = nxt
        nxt += 1

# The math operators of imgui_internal.h.
internal = strip_comments(read(os.path.join(IMGUI, 'imgui_internal.h')))
m = re.search(r'#ifdef IMGUI_DEFINE_MATH_OPERATORS\n(.*?)\n#endif', internal, re.S)
OPERATORS = [re.sub(r'^\s*static\s+inline\s+', '', l).rstrip() for l in m.group(1).split('\n') if l.strip()]

# The functions of imgui_internal.h's namespace ImGui that the game calls, with imgui.h's types
# only (DebugTools.cpp's EndColumns, closing what Columns() opened).
INTERNAL_FUNCTIONS = ['EndColumns']
for name in INTERNAL_FUNCTIONS:
    m = re.search(r'IMGUI_API[^;{}]*?\b%s\s*\([^;]*?\)\s*;' % name, internal)
    if not m:
        fail('imgui_internal.h declares no ' + name)
    functions.append(Function(m.group(0).rstrip(';').strip()))

ALL_STRUCTS = []


def collect(s):
    ALL_STRUCTS.append(s)
    for n in s.nested:
        collect(n)


for name in struct_order:
    collect(structs[name])
NESTED = {n.name: n for s in ALL_STRUCTS for n in s.nested}
CLASS_NAMES = set(WRITTEN) | {s.name for s in ALL_STRUCTS}

# ---------------------------------------------------------------------------------------------
# Layouts: C's rules, which Kpp's and clang's both follow.

LAYOUTS = {'ImVec2': (8, 4, [('x', 0), ('y', 4)]), 'ImVec4': (16, 4, [('x', 0), ('y', 4), ('z', 8), ('w', 12)]),
           'ImColor': (16, 4, [('Value', 0)])}


def size_align(type_text, scope):
    t = type_text.replace('const ', '').strip()
    if t.endswith('*') or t == '*':
        return 8, 8
    if t.startswith('ImVector<'):
        return 16, 8
    if t in typedef_types:
        return size_align(typedef_types[t], scope)
    if t in SCALARS:
        return SCALARS[t], SCALARS[t]
    if scope and t in {n.name for n in scope.nested}:
        return layout(next(n for n in scope.nested if n.name == t))[:2]
    if t in structs:
        return layout(structs[t])[:2]
    if t in LAYOUTS:
        return LAYOUTS[t][:2]
    fail('the size of %s is unknown' % type_text)


def layout(s):
    offset, align, offsets = 0, 1, []
    for kind, name, extra in s.fields:
        if kind == 'fn':
            size, a, count = 8, 8, 1
        else:
            size, a = size_align(kind, s)
            count = eval(re.sub(r'\b[A-Za-z_]\w*\b', lambda m: str(ENUM_VALUES[m.group(0)]), extra)) if extra else 1
        offset = (offset + a - 1) // a * a
        offsets.append((name, offset))
        offset += size * count
        align = max(align, a)
    return (offset + align - 1) // align * align, align, offsets


# ---------------------------------------------------------------------------------------------
# The interface: how each parameter and result crosses.

def core_of(type_text):
    t = type_text.rstrip('&').strip()
    const = t.startswith('const ')
    return (t[6:].strip() if const else t), const


def crossing(p):
    """'class': a class by reference or value (a pointer in C), 'ref': a reference to a scalar (a
    pointer in C), 'plain' otherwise."""
    if p.variadic or p.function:
        return 'plain'
    core, _ = core_of(p.type)
    if core in CLASS_NAMES and not core.endswith('*'):
        return 'class'
    if p.type.endswith('&'):
        return 'ref'
    return 'plain'


def c_type(p, qualify):
    """The parameter's type in the C interface."""
    kind = crossing(p)
    core, const = core_of(p.type)
    if kind == 'class':
        return qualify('const %s*' % core if const or not p.type.endswith('&') else '%s*' % core)
    if kind == 'ref':
        return qualify(('const %s*' if const else '%s*') % core)
    return qualify(p.type)


def returns_class(f):
    return f.returns in CLASS_NAMES


def returns_ref(f):
    return f.returns.endswith('&')


def c_result(f, qualify):
    if returns_ref(f):
        core, const = core_of(f.returns)
        return qualify(('const %s*' if const else '%s*') % core)
    if returns_class(f):
        return 'void'
    return qualify(f.returns) if f.returns else 'void'


def kpp_default(value):
    if value is None:
        return ''
    value = re.sub(r'\bNULL\b', 'nullptr', value)
    value = re.sub(r'\bFLT_MAX\b', 'Limits<float>::max()', value)
    return ' = ' + value


def kpp_type(p):
    return p.type


def qualifier_for(owner):
    """Qualifies the nested struct names of owner (CustomRect inside ImFontAtlas) for global scope."""
    names = {}
    s = owner
    while s:
        for n in s.nested:
            names[n.name] = n.qualified()
        s = s.outer

    def qualify(t):
        return re.sub(r'\b(\w+)\b', lambda m: names.get(m.group(1), m.group(1)), t) if names else t
    return qualify


def va_list_variant(f, pool):
    """The function of pool that takes f's parameters with a va_list in place of the `...`."""
    fixed = [normalize(p.type) for p in f.params[:-1]]
    for g in pool:
        if g.name == f.name + 'V' and [normalize(p.type) for p in g.params] == fixed + ['va_list']:
            return g
    return None


# ---------------------------------------------------------------------------------------------
# Writing

shim = []    # C++ definitions
decls = []   # Kpp's extern "C" declarations
counts = {'functions': 0, 'methods': 0}


def emit(f, c_name, pool):
    """The C function for f, its Kpp declaration, and the Kpp body that calls it."""
    owner = f.owner
    if f.static:
        fail('a static method: %s::%s' % (owner.name, f.name))
    qualify = qualifier_for(owner)
    c_params, c_args, k_args = [], [], []
    if returns_class(f) and not f.constructor:
        c_params.append('%s* out' % qualify(f.returns))
        k_args.append('&result')
    if owner:
        self_type = ('const %s*' if f.const else '%s*') % owner.qualified()
        c_params.append('%s self' % self_type)
        k_args.append('this')
    for p in f.params:
        if p.variadic:
            c_params.append('...')
            k_args.append('args...')
            continue
        kind = crossing(p)
        c_params.append(p.declarator(c_type(p, qualify)) if not p.function else
                        '%s (*%s)(%s)' % (qualify(p.function[0]), p.name, qualify(p.function[1])))
        c_args.append('*' + p.name if kind in ('class', 'ref') else p.name)
        k_args.append('&' + p.name if kind in ('class', 'ref') else p.name)
    result = 'void' if f.constructor or f.destructor else c_result(f, qualify)
    signature = '%s %s(%s)' % (result, c_name, ', '.join(c_params))
    if f.constructor:
        call = 'new (self) %s(%s)' % (owner.qualified(), ', '.join(c_args))
    elif f.destructor:
        call = 'self->~%s()' % owner.name
    elif owner:
        call = 'self->%s(%s)' % (f.name, ', '.join(c_args))
    else:
        call = 'ImGui::%s(%s)' % (f.name, ', '.join(c_args))
    if f.variadic():
        g = va_list_variant(f, pool)
        fixed = c_args
        last = f.params[-2].name
        if g:
            target = ('self->%s' if owner else 'ImGui::%s') % g.name
            body = ['va_list args;', 'va_start(args, %s);' % last]
            body.append(('%s result = ' % result if result != 'void' else '') + '%s(%s);' % (target, ', '.join(fixed + ['args'])))
            body.append('va_end(args);')
            if result != 'void':
                body.append('return result;')
        else:
            # No va_list variant (LogText): the text formatted here, then passed through "%s".
            target = ('self->%s' if owner else 'ImGui::%s') % f.name
            body = ['va_list args;', 'va_start(args, %s);' % last, 'int length = vsnprintf(nullptr, 0, %s, args);' % last, 'va_end(args);',
                    'char* text = (char*)malloc((size_t)length + 1);', 'va_start(args, %s);' % last,
                    'vsnprintf(text, (size_t)length + 1, %s, args);' % last, 'va_end(args);']
            fixed_without_format = fixed[:-1]
            body.append(('%s result = ' % result if result != 'void' else '') + '%s(%s);' % (target, ', '.join(fixed_without_format + ['"%s"', 'text'])))
            body.append('free(text);')
            if result != 'void':
                body.append('return result;')
        shim.append('%s\n{\n%s\n}' % (signature, '\n'.join('  ' + b for b in body)))
    elif f.destructor:
        # C++'s destructor destroys the members too; cleared, the storage gives the Kpp members'
        # destructors that run next nothing to free.
        shim.append('%s\n{\n  %s;\n  memset((void*)self, 0, sizeof(*self));\n}' % (signature, call))
    elif returns_class(f):
        shim.append('%s { *out = %s; }' % (signature, call))
    elif returns_ref(f):
        shim.append('%s { return &%s; }' % (signature, call))
    elif result == 'void':
        shim.append('%s { %s; }' % (signature, call))
    else:
        shim.append('%s { return %s; }' % (signature, call))
    decls.append('extern "C" %s;' % signature)
    # The Kpp side.
    params = []
    for p in f.params:
        if p.variadic:
            params.append('Args... args')
        else:
            params.append(p.declarator(kpp_type(p)) + kpp_default(p.default))
    shim_call = '%s(%s)' % (c_name, ', '.join(k_args))
    if f.constructor:
        body = '{ %s; }' % shim_call
    elif f.destructor:
        body = '{ %s; }' % shim_call
    elif returns_class(f):
        body = '{ %s result; %s; return result; }' % (f.returns, shim_call)
    elif returns_ref(f):
        body = '{ return *%s; }' % shim_call
    elif f.returns == 'void':
        body = '{ %s; }' % shim_call
    else:
        body = '{ return %s; }' % shim_call
    template = 'template<typename... Args> ' if f.variadic() else ''
    if f.constructor:
        single = len(f.params) >= 1 and all(p.default is not None for p in f.params[1:])
        head = '%s%s(%s)' % ('implicit ' if single else '', owner.name, ', '.join(params))
    elif f.destructor:
        head = '~%s()' % owner.name
    else:
        head = '%s %s(%s)%s' % (f.returns, f.name, ', '.join(params), ' const' if f.const else '')
    return '%s%s %s' % (template, head, body)


def c_names(items, prefix):
    """C names: prefix + name, numbered from the second overload on."""
    seen, names = {}, []
    for f in items:
        base = 'construct' if f.constructor else 'destruct' if f.destructor else f.name
        seen[base] = seen.get(base, 0) + 1
        names.append(prefix + base + (str(seen[base]) if seen[base] > 1 else ''))
    return names


namespace = []
callable_functions = [f for f in functions if not f.takes_va_list()]
for f, c_name in zip(callable_functions, c_names(callable_functions, 'kppImGui_')):
    namespace.append(emit(f, c_name, functions))
    counts['functions'] += 1

struct_methods = {}
for s in ALL_STRUCTS:
    usable = [f for f in s.functions if not f.takes_va_list()]
    struct_methods[s.qualified()] = [emit(f, n, s.functions) for f, n in zip(usable, c_names(usable, 'kpp%s_' % s.c_name()))]
    counts['methods'] += len(usable)


def kpp_struct(s, indent):
    lines = ['%sstruct %s' % (indent, s.name), indent + '{']
    inner = indent + '  '
    for n in s.nested:
        lines += kpp_struct(n, inner)
    for name, t in s.typedefs:
        lines.append('%susing %s = %s;' % (inner, name, t))
    for kind, name, extra in s.fields:
        if kind == 'fn':
            lines.append('%s%s (*%s)(%s);' % (inner, extra[0], name, extra[1]))
        else:
            lines.append('%s%s %s%s;' % (inner, kind, name, '[%s]' % extra if extra else ''))
    for m in struct_methods[s.qualified()]:
        lines.append(inner + m)
    lines.append(indent + '};')
    return lines


def kpp_enum(name, values):
    body = ',\n'.join('  %s%s' % (e, ' = ' + v if v is not None else '') for e, v in values)
    return 'enum %s\n{\n%s\n};' % (name, body)


def constant(name, value):
    """An object-like macro of imgui.h as a Kpp constant, or None."""
    if re.match(r'^"[^"]*"$', value):
        return 'const char* const %s = %s;' % (name, value)
    if re.match(r'^(0x[0-9A-Fa-f]+|\d+)$', value):
        return 'const %s %s = %s;' % ('ImU32' if int(value, 0) > 0x7FFFFFFF else 'int', name, value)
    m = re.match(r'^IM_COL32\((\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)$', value)
    if m:
        r, g, b, a = (int(x) for x in m.groups())
        shifts = {k: int(MACROS['IM_COL32_%s_SHIFT' % k]) for k in 'RGBA'}
        rgba = (a << shifts['A']) | (b << shifts['B']) | (g << shifts['G']) | (r << shifts['R'])
        return 'const ImU32 %s = 0x%08X; // %s' % (name, rgba, value)
    return None


constants = []
for name, value in MACROS.items():
    if name == 'IMGUI_ONCE_UPON_A_FRAME':
        continue  # a statement (a static local and an if), obsolete; ImGuiOnceUponAFrame is opaque here
    c = constant(name, value)
    if c is None:
        if value:
            fail('the macro %s (%s) has no Kpp constant' % (name, value))
        continue
    constants.append(c)

kpp = []
kpp.append('''// dear imgui 1.53 (C:\\Factorio\\libraries\\imgui, the game's copy) for Kpp. Generated by
// programs/factorio/native/generate_imgui.py from imgui.h; do not edit. imgui is C++, so Kpp calls it
// through native/imgui_c.cpp, its thin C interface (built into factorio-native.lib with the game's
// imgui.lib, link.cmd): the functions of namespace ImGui and the structs' methods here are imgui's,
// with its signatures and default arguments, and each calls its C function. The structs have
// imgui's layouts (imgui owns ImGuiIO, ImGuiStyle and the draw lists, and the game reads and writes
// their fields): the C side asserts each field's offset, the static_asserts here the sizes.
// ImVec2, ImVec4, ImColor and ImVector are imgui's inline code, written in Kpp. Declared only,
// as pointers to them is all that crosses: ImDrawListSharedData, ImGuiContext, ImGuiStorage,
// ImGuiTextFilter, ImGuiTextBuffer, ImGuiOnceUponAFrame. Left out: the va_list variants (TextV,
// ...); Kpp has no va_list. imgui.h's function-like macros (IM_COL32, IM_ARRAYSIZE) the converter
// expands where the game uses them (overrides/expand-macros.txt).
extern "C" import "string.h";''')
kpp.append('\n'.join('struct %s;' % n for n in OPAQUE))
kpp.append('\n'.join(t + ';' for t in typedefs))
kpp.append('\n'.join(constants))
kpp.append(r'''struct ImVec2
{
  float x, y;
  ImVec2() { this->x = this->y = 0.0f; }
  ImVec2(float _x, float _y) { this->x = _x; this->y = _y; }
};

struct ImVec4
{
  float x, y, z, w;
  ImVec4() { this->x = this->y = this->z = this->w = 0.0f; }
  ImVec4(float _x, float _y, float _z, float _w) { this->x = _x; this->y = _y; this->z = _z; this->w = _w; }
};

// imgui_internal.h's operators (IMGUI_DEFINE_MATH_OPERATORS).
''' + '\n'.join(OPERATORS) + r'''

// Lightweight std::vector-like class: imgui's, which does not call constructors or destructors
// of its elements (imgui.h: "Do not use this class as a straight std::vector replacement").
template<typename T>
class ImVector
{
public:
  int Size;
  int Capacity;
  T* Data;

  using value_type = T;
  using iterator = value_type*;
  using const_iterator = const value_type*;

  ImVector() { this->Size = this->Capacity = 0; this->Data = nullptr; }
  ~ImVector() { if (this->Data) ImGui::MemFree(this->Data); }

  bool empty() const { return this->Size == 0; }
  int size() const { return this->Size; }
  int capacity() const { return this->Capacity; }

  value_type& operator[](int i) { assert(i < this->Size); return this->Data[i]; }
  const value_type& operator[](int i) const { assert(i < this->Size); return this->Data[i]; }

  void clear() { if (this->Data) { this->Size = this->Capacity = 0; ImGui::MemFree(this->Data); this->Data = nullptr; } }
  iterator begin() { return this->Data; }
  const_iterator begin() const { return this->Data; }
  iterator end() { return this->Data + this->Size; }
  const_iterator end() const { return this->Data + this->Size; }
  value_type& front() { assert(this->Size > 0); return this->Data[0]; }
  const value_type& front() const { assert(this->Size > 0); return this->Data[0]; }
  value_type& back() { assert(this->Size > 0); return this->Data[this->Size - 1]; }
  const value_type& back() const { assert(this->Size > 0); return this->Data[this->Size - 1]; }
  void swap(ImVector<T>& rhs)
  {
    int rhs_size = rhs.Size; rhs.Size = this->Size; this->Size = rhs_size;
    int rhs_cap = rhs.Capacity; rhs.Capacity = this->Capacity; this->Capacity = rhs_cap;
    value_type* rhs_data = rhs.Data; rhs.Data = this->Data; this->Data = rhs_data;
  }

  int _grow_capacity(int size) const { int new_capacity = this->Capacity ? (this->Capacity + this->Capacity / 2) : 8; return new_capacity > size ? new_capacity : size; }

  void resize(int new_size) { if (new_size > this->Capacity) this->reserve(this->_grow_capacity(new_size)); this->Size = new_size; }
  void resize(int new_size, const T& v)
  {
    if (new_size > this->Capacity)
      this->reserve(this->_grow_capacity(new_size));
    if (new_size > this->Size)
      for (int n = this->Size; n < new_size; n++)
        this->Data[n] = v;
    this->Size = new_size;
  }
  void reserve(int new_capacity)
  {
    if (new_capacity <= this->Capacity)
      return;
    T* new_data = (value_type*)ImGui::MemAlloc((size_t)new_capacity * sizeof(T));
    if (this->Data)
      memcpy(new_data, this->Data, (size_t)this->Size * sizeof(T));
    ImGui::MemFree(this->Data);
    this->Data = new_data;
    this->Capacity = new_capacity;
  }

  void push_back(const value_type& v) { if (this->Size == this->Capacity) this->reserve(this->_grow_capacity(this->Size + 1)); this->Data[this->Size++] = v; }
  void pop_back() { assert(this->Size > 0); this->Size--; }
  void push_front(const value_type& v) { if (this->Size == 0) this->push_back(v); else this->insert(this->Data, v); }

  iterator erase(const_iterator it)
  {
    assert(it >= this->Data && it < this->Data + this->Size);
    const ptrdiff_t off = it - this->Data;
    memmove(this->Data + off, this->Data + off + 1, ((size_t)this->Size - (size_t)off - 1) * sizeof(value_type));
    this->Size--;
    return this->Data + off;
  }
  iterator insert(const_iterator it, const value_type& v)
  {
    assert(it >= this->Data && it <= this->Data + this->Size);
    const ptrdiff_t off = it - this->Data;
    if (this->Size == this->Capacity)
      this->reserve(this->_grow_capacity(this->Size + 1));
    if (off < (int)this->Size)
      memmove(this->Data + off + 1, this->Data + off, ((size_t)this->Size - (size_t)off) * sizeof(value_type));
    this->Data[off] = v;
    this->Size++;
    return this->Data + off;
  }
  bool contains(const value_type& v) const
  {
    const T* data = this->Data;
    const T* data_end = this->Data + this->Size;
    while (data < data_end)
      if (*data++ == v)
        return true;
    return false;
  }
};

// A color as ImU32 (packed) or ImVec4 (floats), converting to either.
struct ImColor
{
  ImVec4 Value;

  ImColor() { this->Value.x = this->Value.y = this->Value.z = this->Value.w = 0.0f; }
  ImColor(int r, int g, int b, int a = 255)
  {
    float sc = 1.0f / 255.0f;
    this->Value.x = (float)r * sc; this->Value.y = (float)g * sc; this->Value.z = (float)b * sc; this->Value.w = (float)a * sc;
  }
  implicit ImColor(ImU32 rgba)
  {
    float sc = 1.0f / 255.0f;
    this->Value.x = (float)((rgba >> IM_COL32_R_SHIFT) & 0xFF) * sc;
    this->Value.y = (float)((rgba >> IM_COL32_G_SHIFT) & 0xFF) * sc;
    this->Value.z = (float)((rgba >> IM_COL32_B_SHIFT) & 0xFF) * sc;
    this->Value.w = (float)((rgba >> IM_COL32_A_SHIFT) & 0xFF) * sc;
  }
  ImColor(float r, float g, float b, float a = 1.0f) { this->Value.x = r; this->Value.y = g; this->Value.z = b; this->Value.w = a; }
  implicit ImColor(const ImVec4& col) { this->Value = col; }
  implicit operator ImU32() const { return ImGui::ColorConvertFloat4ToU32(this->Value); }
  implicit operator ImVec4() const { return this->Value; }

  void SetHSV(float h, float s, float v, float a = 1.0f) { ImGui::ColorConvertHSVtoRGB(h, s, v, this->Value.x, this->Value.y, this->Value.z); this->Value.w = a; }
  static ImColor HSV(float h, float s, float v, float a = 1.0f) { float r, g, b; ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b); return ImColor(r, g, b, a); }
};''')
kpp += [kpp_enum(n, v) for n, v in enums]
for name in struct_order:
    kpp.append('\n'.join(kpp_struct(structs[name], '')))
kpp.append('static_assert(sizeof(ImVec2) == 8);\nstatic_assert(sizeof(ImVec4) == 16);\nstatic_assert(sizeof(ImColor) == 16);\n'
           'static_assert(sizeof(ImVector<int>) == 16);\n' +
           '\n'.join('static_assert(sizeof(%s) == %d);' % (s.qualified(), layout(s)[0]) for s in ALL_STRUCTS))
kpp.append('namespace ImGui\n{\n' + '\n'.join('  ' + f for f in namespace) + '\n}')
kpp.append('// The C interface (native/imgui_c.cpp).\n' + '\n'.join(decls))

asserts = ['static_assert(sizeof(ImVec2) == 8, "");', 'static_assert(sizeof(ImVec4) == 16, "");', 'static_assert(sizeof(ImColor) == 16, "");',
           'static_assert(sizeof(ImVector<int>) == 16, "");']
for s in ALL_STRUCTS:
    size, align, offsets = layout(s)
    asserts.append('static_assert(sizeof(%s) == %d, "%s: the size ImGui.kpp gives it");' % (s.qualified(), size, s.qualified()))
    for name, offset in offsets:
        asserts.append('static_assert(offsetof(%s, %s) == %d, "%s::%s");' % (s.qualified(), name, offset, s.qualified(), name))

c = ['''// dear imgui 1.53's thin C interface, for Kpp (overrides/added/src/Debug/ImGui.kpp). Generated by
// generate_imgui.py from C:\\Factorio\\libraries\\imgui\\imgui.h; do not edit. Built by build.cmd
// into factorio-native.lib, linked with the game's imgui.lib.
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <new>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The layouts ImGui.kpp gives the structs.''']
c.append('\n'.join(asserts))
c.append('extern "C"\n{\n' + '\n'.join(shim) + '\n}')

os.makedirs(os.path.dirname(KPP), exist_ok=True)
with open(KPP, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n\n'.join(kpp) + '\n')
with open(SHIM, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n\n'.join(c) + '\n')
print('%s: %d functions, %d methods of %d structs; %s' % (os.path.relpath(KPP, FACTORIO), counts['functions'], counts['methods'], len(ALL_STRUCTS),
                                                           os.path.relpath(SHIM, FACTORIO)))
