"""kpp's code style checker (docs/code-style.md): reports, by file and line, the rules a program can
check in .kpp files, and with --fix corrects the empty lines and the spaces at line ends.

  python tools/style/kppstyle.py [--fix] [--summary] [--rule NAME] [--fixable] [--tracked] PATH...

PATH is a .kpp file or a directory searched for them. The exit code is 1 when something was found
(after --fix: when something is left that --fix does not correct).
"""
import argparse, os, re, sys

MAX_LINE = 130
BACKSLASH = chr(92)
RULES = {
    'line-length': 'longer than %d characters' % MAX_LINE,
    'tab': 'a tab',
    'trailing-space': 'spaces at the end of the line',
    'final-newline': 'no line end at the end of the file',
    'double-empty': 'two empty lines in a row',
    'empty-after-open': 'an empty line after `{`',
    'empty-before-close': 'an empty line before `}`',
    'expanded-before': 'no empty line before an expanded definition',
    'expanded-after': 'no empty line after an expanded definition',
    'egyptian-brace': '`{` at the end of a line: on a line of its own',
    'keyword-space': 'no space between the keyword and `(`',
    'empty-constructor': '`X() {}`: `X() = default;`, or not written',
}
FIXABLE = {'trailing-space', 'final-newline', 'double-empty', 'empty-after-open', 'empty-before-close', 'expanded-before', 'expanded-after'}


def code_of(lines):
    """Each line with its comments and string and character literals blanked (raw strings too),
    so that braces and keywords are found in code alone."""
    out = []
    inside = []  # per line: it starts inside a string or a comment that spans lines
    in_block = False
    raw_end = None  # the end of a raw string spanning lines: )delim"
    quote = None  # the quote of a literal continued by a backslash at the end of the line before
    spliced_comment = False  # a // comment continued so
    spliced = False  # the line before ends with a backslash: this one is its continuation
    for line in lines:
        inside.append(in_block or raw_end is not None or quote is not None or spliced_comment or spliced)
        spliced = line.endswith(BACKSLASH)
        s = list(line)
        i = 0
        n = len(s)
        if spliced_comment:
            spliced_comment = line.endswith(BACKSLASH)
            out.append(' ' * n)
            continue
        while i < n:
            if quote is not None:
                j = i
                while j < n and line[j] != quote:
                    j += 2 if line[j] == BACKSLASH else 1
                for k in range(i, min(j + 1, n)):
                    s[k] = ' '
                if j >= n and not line.endswith(BACKSLASH):
                    quote = None  # (unterminated: C++ refuses it; the line ends it here)
                elif j < n:
                    quote = None
                i = j + 1
                continue
            if raw_end is not None:
                j = line.find(raw_end, i)
                if j < 0:
                    for k in range(i, n):
                        s[k] = ' '
                    i = n
                    break
                for k in range(i, j + len(raw_end)):
                    s[k] = ' '
                i = j + len(raw_end)
                raw_end = None
                continue
            if in_block:
                j = line.find('*/', i)
                if j < 0:
                    for k in range(i, n):
                        s[k] = ' '
                    i = n
                    break
                for k in range(i, j + 2):
                    s[k] = ' '
                i = j + 2
                in_block = False
                continue
            c = line[i]
            if line.startswith('//', i):
                for k in range(i, n):
                    s[k] = ' '
                spliced_comment = line.endswith(BACKSLASH)
                break
            if line.startswith('/*', i):
                in_block = True
                continue
            m = re.match(r'(?:u8|u|U|L)?R"([^(\s]{0,16})\(', line[i:])
            if m and (i == 0 or not (line[i - 1].isalnum() or line[i - 1] == '_')):
                raw_end = ')' + m.group(1) + '"'
                for k in range(i, i + m.end()):
                    s[k] = ' '
                i += m.end()
                continue
            if c == '"' or (c == "'" and not re.search(r'\b\d\w*$', line[:i])):  # (1'000 digit separators; L'x' a literal)
                q = c
                j = i + 1
                while j < n and line[j] != q:
                    j += 2 if line[j] == '\\' else 1
                for k in range(i, min(j + 1, n)):
                    s[k] = ' '
                if j >= n and line.endswith(BACKSLASH):
                    quote = q  # the literal goes on on the next line
                i = j + 1
                continue
            i += 1
        out.append(''.join(s))
    return out, inside


def indent(line):
    return len(line) - len(line.lstrip(' '))


def check(path, text):
    """The findings in one file's text: (line number from 1, rule)."""
    found = []
    lines = text.split('\n')
    final = text.endswith('\n') or text == ''
    if final and lines and lines[-1] == '':
        lines = lines[:-1]
    else:
        found.append((max(1, len(lines)), 'final-newline'))
    code, inside = code_of(lines)
    for i, line in enumerate(lines):
        if len(line) > MAX_LINE:
            found.append((i + 1, 'line-length'))
        if '\t' in line:
            found.append((i + 1, 'tab'))
        if line != line.rstrip(' \t\r'):
            found.append((i + 1, 'trailing-space'))
        if i > 0 and line.strip() == '' and lines[i - 1].strip() == '':
            found.append((i + 1, 'double-empty'))
        c = code[i]
        if re.search(r'\b(if|for|while|switch|catch)\(', c):
            found.append((i + 1, 'keyword-space'))
        if re.match(r'\s*(?:implicit\s+|explicit\s+)?([A-Z]\w*)\(\)\s*\{\}', c):
            found.append((i + 1, 'empty-constructor'))
    # Blocks: each `{` with its `}` and the kind of block: a container of declarations, code, or an
    # expression's braces (a lambda's body or a brace initialisation inside a declaration, which are
    # no definitions of their own). A statement starts after the last `;`, `{` or `}` (an
    # expression's braces aside) outside parentheses: what a `{` opens is read from there.
    opens = []   # stack of (line, kind, head line, column, parentheses open at it, the statement's start before it)
    blocks = []  # (open line, close line, enclosing kind, own kind, head line, column)
    paren = 0
    stmt = (0, 0)  # where the current statement starts: (line, column)
    for i, c in enumerate(code):
        if re.match(r'\s*(public|private|protected)\s*:\s*$', c):
            stmt = (i + 1, 0)
            continue
        for j, ch in enumerate(c):
            relative = paren - (opens[-1][4] if opens else 0)
            if ch in '([':
                paren += 1
            elif ch in ')]':
                paren = max(0, paren - 1)
            elif ch == ';' and relative == 0:
                stmt = (i, j + 1)
            elif ch == '{':
                enclosing = opens[-1][1] if opens else 'declarations'
                text_head = ' '.join([code[stmt[0]][stmt[1]:]] + [code[k] for k in range(stmt[0] + 1, i)] + [c[:j]]) if stmt[0] < i else c[stmt[1]:j]
                own = brace_kind(text_head, relative)
                if enclosing != 'declarations' and own == 'declarations':
                    own = 'code'
                head = stmt[0]
                while head < i and (code[head][stmt[1] if head == stmt[0] else 0:].strip() == ''):
                    head += 1  # (the first line with code: past empty lines and comments)
                if head == stmt[0] and code[head][:stmt[1]].strip():
                    head = -1  # (it starts on a line after other code, `};  when (...) enum class Type`: no line of its own)
                opens.append((i, own, head, j, paren, stmt))
                stmt = (i, j + 1)
            elif ch == '}':
                if opens:
                    oi, own, head, oj, _, before = opens.pop()
                    enclosing = opens[-1][1] if opens else 'declarations'
                    blocks.append((oi, i, enclosing, own, head, oj))
                    # (An expression's braces are part of the statement around them, which goes on.)
                    stmt = before if own == 'expression' else (i, j + 1)
    for (oi, ci, enclosing, own, head, oj) in blocks:
        if oi == ci:
            continue
        # An empty line after `{`, before `}`.
        if oi + 1 < len(lines) and lines[oi + 1].strip() == '' and code[oi].rstrip().endswith('{'):
            found.append((oi + 2, 'empty-after-open'))
        if ci - 1 > oi and lines[ci - 1].strip() == '' and code[ci].lstrip().startswith('}'):
            found.append((ci, 'empty-before-close'))
        # A brace at the end of a line that has code before it.
        before = code[oi][:oj].strip()
        # (Not an initializer's: `= {`, `f({`, `{a, {`, `return {`.)
        # (Nor a brace initialisation opening a raw string that runs over lines: `x{R"(<div>`.)
        if before and code[oi].rstrip().endswith('{') and not re.search(r'(=|\(|,|\{|\breturn|\bextern)$', before) \
                and not re.match(r'\s*(?:u8|u|U|L)?R"', lines[oi][oj + 1:]):
            found.append((oi + 1, 'egyptian-brace'))
        # An expanded definition among declarations: an empty line around it.
        if enclosing != 'declarations' or own == 'expression':
            continue
        # (the comment right above it is its own, a `/** ... */` as a `//`: a line of comment and nothing else)
        top = head
        while top > 0 and lines[top - 1].strip() != '' and code[top - 1].strip() == '':
            top -= 1
        if top > 0:  # (head -1: no line of its own)
            prev = code[top - 1].strip()
            if lines[top - 1].strip() != '' and not prev.endswith('{') and not re.match(r'(public|private|protected)\s*:$', prev):
                found.append((top + 1, 'expanded-before'))
        if ci + 1 < len(lines):
            nxt = code[ci + 1].strip()
            # (Not when code goes on after its `}` on that line: `};  when (...) enum class Type`.)
            if lines[ci + 1].strip() != '' and not nxt.startswith('}') and re.match(r'\s*\}\s*;?\s*$', code[ci]):
                found.append((ci + 1, 'expanded-after'))
    # Nothing inside a string or a comment that spans lines: its lines are its text.
    found = {(n, rule) for (n, rule) in found if rule == 'line-length' or rule == 'tab' or not inside[min(n, len(lines)) - 1]}
    # One missing empty line between two expanded definitions is one finding.
    found -= {(n + 1, 'expanded-before') for (n, rule) in found if rule == 'expanded-after'}
    return sorted(found), lines, final


def brace_kind(head, parentheses):
    """What a `{` opens, from the code of its statement before it (head) and the parentheses open
    at it since the enclosing block's `{`: 'declarations' (a class, struct, union, enum, namespace,
    extern "C"), 'expression' (inside parentheses, after `=`, `,`, `return`, `(` or a name: a lambda's
    body, a brace initialisation) or 'code' (a function's body, a statement's block)."""
    if parentheses > 0:
        return 'expression'
    t = head
    while re.search(r'<[^<>]*>', t):  # (template arguments and parameters: `template<class T>`)
        t = re.sub(r'<[^<>]*>', ' ', t)
    t = t.strip()
    if re.search(r'\b(class|struct|union|enum|namespace)\b|\bextern\s*$', t):
        return 'declarations'
    flat = t
    while re.search(r'\([^()]*\)|\{[^{}]*\}|\[[^\[\]]*\]', flat):  # (parameters with defaults, captures, braces)
        flat = re.sub(r'\([^()]*\)|\{[^{}]*\}|\[[^\[\]]*\]', ' ', flat)
    if re.search(r'(?<![=!<>])=(?!=)', flat) and 'operator' not in flat and not re.search(r'=\s*0\s*$', flat):  # (`f() = 0 {`)
        return 'expression'
    if re.search(r'(,|\(|\breturn|\])$', t):
        return 'expression'
    if re.search(r'\w$', t) and '->' not in t and \
            not re.search(r'\b(const|override|noexcept|final|mutable|volatile|try|else|do)$', t) and not re.match(r'^[\w:~]+$', t):
        return 'expression'  # `Pair{a, b}` (not a bare name: `else`, a label-less block)
    return 'code'


def head_of(lines, code, i, j):
    """The first line of what the `{` at line i, column j opens: its own line when code stands before
    it, else the lines above back to the end of the previous statement or declaration."""
    if code[i][:j].strip():
        k = i
    else:
        k = i - 1
        if k < 0:
            return i
    while k > 0:
        prev = code[k - 1].strip()
        if prev == '' or prev.endswith(';') or prev.endswith('}') or prev.endswith('{') or re.match(r'(public|private|protected)\s*:$', prev) \
                or lines[k - 1].strip().startswith('//'):
            break
        k -= 1
    return k


def fix(lines, findings):
    """The empty lines and line ends corrected: the fixable findings, from the bottom up."""
    by = {}
    for n, rule in findings:
        by.setdefault(n, set()).add(rule)
    out = list(lines)
    for n in sorted(by, reverse=True):
        rules = by[n]
        i = n - 1
        if 'expanded-after' in rules:
            out.insert(i + 1, '')
        if 'empty-after-open' in rules or 'empty-before-close' in rules:
            if out[i].strip() == '':
                del out[i]
                continue
        if 'expanded-before' in rules:
            out.insert(i, '')
        if 'double-empty' in rules and i < len(out) and out[i].strip() == '':
            del out[i]
            continue
    return [l.rstrip(' \t\r') for l in out]


def files_under(paths, tracked):
    if tracked:  # the files git tracks under the paths
        import subprocess
        out = subprocess.run(['git', 'ls-files', '-z', '--'] + paths, capture_output=True, check=True).stdout.decode('utf-8')
        for name in sorted(out.split('\0')):
            if name.endswith('.kpp'):
                yield name
        return
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, names in os.walk(p):
                dirs[:] = [d for d in dirs if d not in ('build', '.git', 'node_modules')]
                for name in sorted(names):
                    if name.endswith('.kpp'):
                        yield os.path.join(root, name)
        elif p.endswith('.kpp'):
            yield p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--fix', action='store_true')
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--rule', action='append', default=[])
    ap.add_argument('--fixable', action='store_true', help='only the rules --fix corrects (run_tests checks these)')
    ap.add_argument('--tracked', action='store_true', help='only the files git tracks')
    args = ap.parse_args()
    if args.fixable:
        args.rule += sorted(FIXABLE)
    counts = {}
    files_with = {}
    left = 0
    checked = 0
    for path in files_under(args.paths, args.tracked):
        checked += 1
        with open(path, encoding='utf-8', errors='surrogateescape', newline='') as f:  # (bytes that are no UTF-8 kept as they are)
            raw = f.read()
        crlf = '\r\n' in raw
        text = raw.replace('\r\n', '\n')
        findings, lines, final = check(path, text)
        if args.rule:
            findings = [f for f in findings if f[1] in args.rule]
        if args.fix and any(r in FIXABLE for _, r in findings):
            for _ in range(5):  # (a fix can make another: an empty line added before a `}`)
                fixable = [f for f in findings if f[1] in FIXABLE and (not args.rule or f[1] in args.rule)]
                if not fixable:
                    break
                lines = fix(lines, fixable)
                text = '\n'.join(lines) + '\n'
                findings, lines, final = check(path, text)
            with open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
                f.write(text.replace('\n', '\r\n') if crlf else text)
            if args.rule:
                findings = [f for f in findings if f[1] in args.rule]
        for n, rule in findings:
            counts[rule] = counts.get(rule, 0) + 1
            files_with.setdefault(rule, set()).add(path)
            if not args.summary:
                print('%s:%d: %s' % (path, n, RULES[rule]))
            left += 1
    if args.summary or left:
        for rule in RULES:
            if rule in counts:
                print('%-20s %6d in %d files' % (rule, counts[rule], len(files_with[rule])), file=sys.stderr if not args.summary else sys.stdout)
    if not left and not args.summary:
        print('ok   code style (%d files)' % checked)
    return 1 if left else 0


if __name__ == '__main__':
    sys.exit(main())
