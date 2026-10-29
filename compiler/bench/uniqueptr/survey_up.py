# Survey of std::unique_ptr (and CommonUtil's UniquePointer) in the Factorio corpus: src, tests,
# libraries/CommonUtil, libraries/Agui. Comments and string literals are stripped first.
# Usage: python -X utf8 -I survey_up.py [corpus root] > out.txt
import sys, os, re, collections

root = sys.argv[1] if len(sys.argv) > 1 else r'C:\Factorio'
dirs = ['src', 'tests', r'libraries\CommonUtil', r'libraries\Agui']
exts = ('.cpp', '.hpp', '.h', '.inl')

def strip(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            seg = text[i:(n if j < 0 else j + 2)]
            out.append('\n' * seg.count('\n'))
            i = n if j < 0 else j + 2
            continue
        if c == 'R' and i + 1 < n and text[i + 1] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            if m:
                end = text.find(')' + m.group(1) + '"', i)
                seg = text[i:(n if end < 0 else end + len(m.group(1)) + 2)]
                out.append('""' + '\n' * seg.count('\n'))
                i = n if end < 0 else end + len(m.group(1)) + 2
                continue
        if c == '"' or (c == "'" and not (i > 0 and (text[i - 1].isalnum()))):  # skip digit separators 1'000
            q = c
            j = i + 1
            while j < n and text[j] != q:
                if text[j] == '\\':
                    j += 1
                elif text[j] == '\n':
                    break
                j += 1
            out.append(q + q)
            i = j + 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)

def template_args(text, i):
    """text[i] == '<': returns (args list, end index after '>')."""
    depth = 0
    j = i
    start = i + 1
    args = []
    paren = 0
    while j < len(text):
        c = text[j]
        if c in '([':
            paren += 1
        elif c in ')]':
            paren -= 1
        elif paren == 0 and c == '<':
            depth += 1
        elif paren == 0 and c == '>':
            depth -= 1
            if depth == 0:
                args.append(text[start:j].strip())
                return args, j + 1
        elif paren == 0 and c == ',' and depth == 1:
            args.append(text[start:j].strip())
            start = j + 1
        elif c == ';' or c == '{':
            return None, j
        j += 1
    return None, j

def norm(s):
    return re.sub(r'\s+', ' ', s).replace(' *', '*').replace(' &', '&').strip()

def enclosing_template(text, i):
    """Name of the template whose argument list the position i is inside, if any (innermost)."""
    depth = 0
    paren = 0
    j = i - 1
    while j >= 0 and i - j < 400:
        c = text[j]
        if c in ';{}':
            return None
        if c == ')':
            paren += 1
        elif c == '(':
            if paren == 0:
                return None
            paren -= 1
        elif paren == 0 and c == '>':
            if j > 0 and text[j - 1] == '-':
                j -= 1
            else:
                depth += 1
        elif paren == 0 and c == '<':
            if depth == 0:
                m = re.search(r'([A-Za-z_][A-Za-z_0-9:]*)\s*$', text[max(0, j - 80):j])
                return m.group(1) if m else '?'
            depth -= 1
        j -= 1
    return None

files = []
for d in dirs:
    for dp, dn, fn in os.walk(os.path.join(root, d)):
        for f in fn:
            if f.endswith(exts):
                files.append(os.path.join(dp, f))

c = collections.Counter()
types = collections.Counter()           # T of unique_ptr<T>
array_types = collections.Counter()
deleters = collections.Counter()        # unique_ptr<T, D>: D
deleter_types = collections.Counter()   # unique_ptr<T, D>: T
make_types = collections.Counter()
enclosing = collections.Counter()
up_files = set()
uptr_deleters = collections.Counter()   # UniquePointer<T, D>: D
uptr_types = collections.Counter()
examples = collections.defaultdict(list)
fwd_files = 0
fwd_uses = 0
const_types = 0
clone_like = collections.Counter()

def example(key, path, text, i):
    if len(examples[key]) < 6:
        line = text.count('\n', 0, i) + 1
        ls = text.rfind('\n', 0, i) + 1
        le = text.find('\n', i)
        examples[key].append(f'{os.path.relpath(path, root)}:{line}: {text[ls:le].strip()[:160]}')

for path in files:
    try:
        raw = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        continue
    text = strip(raw)
    fwd = set(re.findall(r'^\s*(?:class|struct)\s+([A-Za-z_][A-Za-z_0-9]*)\s*;', text, re.M))
    file_fwd_uses = 0
    for m in re.finditer(r'\bunique_ptr\b', text):
        c['unique_ptr tokens'] += 1
        up_files.add(path)
        i = m.end()
        while i < len(text) and text[i] in ' \t\n':
            i += 1
        if i >= len(text) or text[i] != '<':
            c['unique_ptr without template args'] += 1
            continue
        args, end = template_args(text, i)
        if not args:
            continue
        c['unique_ptr<...> spelled'] += 1
        t = norm(args[0])
        if t.endswith('[]'):
            array_types[t[:-2].strip()] += 1
            c['unique_ptr<T[]>'] += 1
            example('array', path, text, m.start())
        else:
            types[t] += 1
        if t.startswith('const '):
            const_types += 1
        if len(args) > 1:
            deleters[norm(args[1])] += 1
            deleter_types[t] += 1
            c['unique_ptr<T, D>'] += 1
            example('deleter', path, text, m.start())
        base = t.split('<')[0].split('::')[-1]
        if base in fwd:
            file_fwd_uses += 1
        enc = enclosing_template(text, m.start())
        enclosing[enc.split('::')[-1] if enc else '(none)'] += 1
        after = text[end:end + 60]
        if re.match(r'\s*&&', after):
            c['unique_ptr<..>&& (rvalue reference)'] += 1
        elif re.match(r'\s*&', after):
            if re.search(r'const\s+(std::)?$', text[max(0, m.start() - 12):m.start()]):
                c['const unique_ptr<..>& (const lvalue reference)'] += 1
            else:
                c['unique_ptr<..>& (lvalue reference)'] += 1
                example('lref', path, text, m.start())
        elif re.match(r'\s*\*', after):
            c['unique_ptr<..>* (pointer to)'] += 1
        mm = re.match(r'\s*([A-Za-z_][A-Za-z_0-9]*)\s*\(', after)
        if mm and not enc:
            c['unique_ptr<..> name( (function returning or variable with ctor args)'] += 1
            if mm.group(1) in ('clone', 'copy', 'duplicate', 'cloneUnique'):
                clone_like[mm.group(1)] += 1
                example('clone', path, text, m.start())
        if re.match(r'\s*\(\s*new\b', after):
            c['unique_ptr<T>(new ...)'] += 1
        if re.match(r'\s*\(\s*\)', after):
            c['unique_ptr<T>() (empty temporary)'] += 1
        if re.match(r'\s*\(\s*nullptr\s*\)', after):
            c['unique_ptr<T>(nullptr)'] += 1
    if file_fwd_uses:
        fwd_files += 1
        fwd_uses += file_fwd_uses
    for m in re.finditer(r'\bmake_unique\s*<', text):
        c['make_unique<...>'] += 1
        args, end = template_args(text, m.end() - 1)
        if args:
            t = norm(args[0])
            if t.endswith('[]'):
                c['make_unique<T[]>'] += 1
            make_types[t] += 1
    c['make_unique_for_overwrite'] += len(re.findall(r'\bmake_unique_for_overwrite\b', text))
    for m in re.finditer(r'\b(UniquePointer|ThrowingUniquePointer)\s*<', text):
        c[m.group(1) + '<...>'] += 1
        args, end = template_args(text, m.end() - 1)
        if args:
            uptr_types[norm(args[0])] += 1
            if len(args) > 1:
                uptr_deleters[norm(args[1])] += 1
                example('UniquePointer deleter', path, text, m.start())
    c['make_unique_pointer'] += len(re.findall(r'\bmake_unique_pointer\b', text))
    c['.reset(new'] += len(re.findall(r'\.\s*reset\s*\(\s*new\b', text))
    c['.release()'] += len(re.findall(r'\.\s*release\s*\(\s*\)', text))
    c['.get_deleter()'] += len(re.findall(r'\bget_deleter\s*\(', text))
    for m in re.finditer(r'\bdynamic_cast\s*<[^;]*?\.\s*get\s*\(\s*\)', text):
        c['dynamic_cast<...>(x.get())'] += 1
        example('dynamic_cast get', path, text, m.start())
    for m in re.finditer(r'\bstatic_cast\s*<[^;>]*\*\s*>\s*\([^;()]*\.\s*get\s*\(\s*\)\s*\)', text):
        c['static_cast<T*>(x.get())'] += 1
        example('static_cast get', path, text, m.start())
    for m in re.finditer(r'\bdynamic_cast\s*<', text):
        c['dynamic_cast (all)'] += 1
    for m in re.finditer(r'\b(shared_ptr)\s*<[^;]*?>\s*\(\s*std::move\s*\(', text):
        c['shared_ptr<T>(std::move(...)) (possibly from unique_ptr)'] += 1
    c['std::move('] += len(re.findall(r'\bstd::move\s*\(', text))
    c['owner_less / hash<unique_ptr>'] += len(re.findall(r'\bhash\s*<\s*(std::)?unique_ptr', text))
    for m in re.finditer(r'\bunique_ptr\s*<[^;{}]*>\s*\(\s*(std::)?move\s*\(', text):
        c['unique_ptr<T>(std::move(...)) (explicit conversion)'] += 1
    for m in re.finditer(r'\bis_unique_ptr\w*|\bIsUniquePtr\w*|\bisUniquePtr\w*', text):
        c['is_unique_ptr-like traits'] += 1
        example('trait', path, text, m.start())

print(f'files scanned: {len(files)}; files mentioning unique_ptr: {len(up_files)}')
for k, v in sorted(c.items(), key=lambda kv: -kv[1]):
    print(f'{v:8,}  {k}')
print(f'const T: {const_types}')
print(f'distinct T in unique_ptr<T> (as spelled): {len(types):,}; array element types: {len(array_types)}; make_unique distinct T: {len(make_types):,}')
print(f'unique_ptr<T> of a class forward-declared in the same file (incomplete there): {fwd_uses:,} in {fwd_files:,} files')
print('deleters (unique_ptr<T, D>):')
for d, k in deleters.most_common(40):
    print(f'  {k:5}  {d}')
print('types with a deleter:')
for d, k in deleter_types.most_common(40):
    print(f'  {k:5}  {d}')
print('array element types:')
for d, k in array_types.most_common(30):
    print(f'  {k:5}  {d}')
print('UniquePointer deleters:')
for d, k in uptr_deleters.most_common(30):
    print(f'  {k:5}  {d}')
print('UniquePointer types (top 30):')
for d, k in uptr_types.most_common(30):
    print(f'  {k:5}  {d}')
print('enclosing template of unique_ptr<...> (top 40):')
for d, k in enclosing.most_common(40):
    print(f'  {k:6}  {d}')
print('most used T (top 30):')
for d, k in types.most_common(30):
    print(f'  {k:5}  {d}')
print('clone-like functions returning unique_ptr:', dict(clone_like))
for key, ex in examples.items():
    print(f'examples: {key}')
    for e in ex:
        print('   ', e)
