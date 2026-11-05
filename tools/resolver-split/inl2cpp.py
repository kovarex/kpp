"""Stage 2 of the resolver split: a class's inline member functions become declarations in its header
and out-of-class definitions in a .cpp (`auto Q::f(params) const -> R { ... }`), a pure move.

  python inl2cpp.py IN.inl CLASS_LINE QUALIFIER INDENT OUT_HEADER OUT_CPP CPP_PRELUDE

IN.inl holds the class (its line `CLASS_LINE`, e.g. "  struct Walker", then "  {" ... "  };") among other
text, or with CLASS_LINE `-` is a part of a class's body (Impl's own parts, QUALIFIER GlobalScope::Impl); INDENT is its members' indentation ("    "). The header gets everything of IN.inl with the class's
functions reduced to declarations; the .cpp gets CPP_PRELUDE and the definitions. Kept inline (in the
header, untouched): templates, constexpr functions, deduced `auto` returns, one-line bodies, and what
is no function (data, nested types, lambdas in initializers)."""
import re, sys

src, class_line, qualifier, indent, out_header, out_cpp, prelude = sys.argv[1:8]
lines = open(src, 'rb').read().decode().split('\n')
if class_line == '-':
    start, end = -2, len(lines) # (a part of a class's body, its members at INDENT: Impl's own parts)
else:
    start = lines.index(class_line)
    assert lines[start + 1] == indent[:-2] + '{', lines[start + 1]
    close = indent[:-2] + '};'
    end = next(i for i in range(start + 2, len(lines)) if lines[i] == close)

def depth_scan(s):
    """Yields (i, ch, depth) over s skipping string and char literals; depth counts (), [], {}."""
    d = 0
    i = 0
    while i < len(s):
        c = s[i]
        if c in '"\'':
            q = c
            i += 1
            while i < len(s) and s[i] != q:
                i += 2 if s[i] == '\\' else 1
            i += 1
            continue
        if c in '([{':
            d += 1
        elif c in ')]}':
            d -= 1
        yield i, c, d
        i += 1

def split_top(s, sep):
    parts, last, angle = [], 0, 0
    for i, c, d in depth_scan(s):
        if c == '<' and d == 0 and i + 1 < len(s) and s[i + 1] not in '<=' and (i == 0 or s[i - 1] not in '<'):
            angle += 1
        elif c == '>' and d == 0 and angle > 0 and (i + 1 >= len(s) or s[i + 1] != '>') and s[i - 1] != '-':
            angle -= 1
        elif c == '>' and d == 0 and angle > 1 and i + 1 < len(s) and s[i + 1] == '>':
            angle -= 1
        if c == sep and d == 0 and angle == 0:
            parts.append(s[last:i])
            last = i + 1
    parts.append(s[last:])
    return parts

def without_default(param):
    for i, c, d in depth_scan(param):
        if c == '=' and d == 0 and param[i - 1] not in '=!<>' and (i + 1 >= len(param) or param[i + 1] != '='):
            return param[:i].rstrip()
    return param

SPEC = re.compile(r'^(?:(?:static|virtual|inline|explicit|friend)\s+)+')

def parse(header):
    """(ret, name, params, after) of a function's header, or None when it is no function."""
    h = header.strip()
    m = re.search(r'\boperator\s*(\(\)|\[\]|[^\s(]+)\s*\(', h)
    if m:
        name_start, paren = m.start(), m.end() - 1
    else:
        paren = None
        for i, c, d in depth_scan(h):
            if c == '(' and d == 1:
                before = h[:i].rstrip()
                if re.search(r'(?:^|[^\w])(decltype|sizeof|alignof|noexcept)$', before):
                    continue
                if not re.search(r'[\w~]$', before):
                    return None
                paren = i
                break
        if paren is None:
            return None
        name_start = re.search(r'[~\w]+$', h[:paren].rstrip()).start()
    head = h[:name_start]
    if '=' in re.sub(r'operator\s*\S+', '', head):
        return None # (a data member initialized with a call or a lambda)
    close_at = None
    for i, c, d in depth_scan(h):
        if i > paren and c == ')' and d == 0:
            close_at = i
            break
    if close_at is None:
        return None
    return head.strip(), h[name_start:paren].strip(), h[paren + 1:close_at], h[close_at + 1:].strip()

header_out, cpp_out = lines[:start + 2], []
i = start + 2
pending = [] # comment lines above a member
converted = kept = 0
while i < end:
    line = lines[i]
    if not line.startswith(indent) or line.startswith(indent + ' ') or line.strip() == '':
        header_out.append(line); i += 1; continue
    if line.strip().startswith('//'):
        header_out.append(line); i += 1; continue
    # A member: its header lines up to a body's "{" alone on its line (a function, a nested type, a
    # data member's lambda), or up to the next line at the members' indentation.
    j = i
    while j < end:
        if lines[j] == indent + '{':
            break
        if j > i and lines[j].startswith(indent) and not lines[j].startswith(indent + ' ') and not lines[j - 1].strip().startswith('template'):
            break
        j += 1
    if j < end and lines[j] == indent + '{':
        body_end = next(k for k in range(j + 1, end) if lines[k] == indent + '}' or lines[k] == indent + '};')
        header_text = ' '.join(l.strip() for l in lines[i:j])
        p = parse(header_text) if lines[body_end] == indent + '}' else None
        simple = p and not header_text.startswith('template') and 'constexpr' not in header_text and \
                 not (re.match(r'^(?:(?:static|virtual|inline|explicit|friend)\s+)*auto\b', p[0]) and '->' not in p[3])
        if not simple:
            header_out.extend(lines[i:body_end + 1]); kept += 1
        else:
            ret, name, params, after = p
            ret_clean = SPEC.sub('', ret)
            init = ''
            for k2, c, d in depth_scan(after):
                if c == ':' and d == 0 and (k2 + 1 >= len(after) or after[k2 + 1] != ':') and (k2 == 0 or after[k2 - 1] != ':'):
                    init = after[k2:]
                    after = after[:k2].strip()
                    break
            decl = (ret + ' ' if ret else '') + name + '(' + params + ')' + (' ' + after if after else '') + ';'
            header_out.append(indent + decl)
            qualifiers = re.sub(r'\b(override|final)\b', '', after).strip()
            trailing = ''
            m = re.search(r'->\s*(.*)$', qualifiers)
            if m:
                trailing, qualifiers = m.group(1).strip(), qualifiers[:m.start()].strip()
            nodefault = ', '.join(without_default(x).strip() for x in split_top(params, ',')) if params.strip() else ''
            if ret_clean or trailing:
                defn = 'auto ' + qualifier + '::' + name + '(' + nodefault + ')' + (' ' + qualifiers if qualifiers else '') + ' -> ' + (trailing or ret_clean)
            else:
                defn = qualifier + '::' + name + '(' + nodefault + ')' + (' ' + qualifiers if qualifiers else '') + (' ' + init if init else '')
            cpp_out.append(defn)
            for b in lines[j:body_end + 1]:
                cpp_out.append(b[len(indent):] if b.startswith(indent) else b.lstrip())
            cpp_out.append('')
            converted += 1
        i = body_end + 1
        continue
    header_out.extend(lines[i:j]); i = j
header_out.extend(lines[end:])
open(out_header, 'wb').write('\n'.join(header_out).encode())
open(out_cpp, 'wb').write((open(prelude, 'rb').read().decode() + '\n'.join(cpp_out)).encode())
print('converted', converted, 'kept inline', kept)
