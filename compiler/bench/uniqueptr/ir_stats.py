# Statistics of kppc's textual IR: function definitions grouped by the template (or class) they belong
# to, their size, and the call sites of each UniquePtr member.
# Usage: python -X utf8 -I ir_stats.py full.ll [topN]
import sys, re, collections

path = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 30

def match_angle(s, i):
    """s[i] == '<'; return the index just after the matching '>'."""
    depth = 0
    j = i
    while j < len(s):
        c = s[j]
        if c == '<':
            depth += 1
        elif c == '>':
            depth -= 1
            if depth == 0:
                return j + 1
        elif c == '(' and depth > 0:
            # a function type inside template args: skip to the matching ')'
            d = 0
            while j < len(s):
                if s[j] == '(':
                    d += 1
                elif s[j] == ')':
                    d -= 1
                    if d == 0:
                        break
                j += 1
        j += 1
    return len(s)

def owner_of(name):
    """'UniquePtr<Color>::reset(Color*)' -> ('UniquePtr', 'Color', 'reset');
    'makeUnique<Foo, int>(int&&)' -> ('makeUnique', 'Foo, int', '');
    'Foo::bar(int)' -> ('Foo', '', 'bar') (the first qualifier)."""
    m = re.match(r'[A-Za-z_][A-Za-z_0-9]*(?:::[A-Za-z_][A-Za-z_0-9]*)*', name)
    if not m:
        return (name[:30], '', '')
    head = m.group(0)
    rest = name[m.end():]
    args = ''
    if rest.startswith('<') and not head.endswith('operator'):
        e = match_angle(rest, 0)
        args = rest[1:e - 1]
        rest = rest[e:]
    member = ''
    if rest.startswith('::'):
        mm = re.match(r'::(~?[A-Za-z_][A-Za-z_0-9]*(?:\s*[^\sA-Za-z_0-9(][^(]*)?)', rest)
        if mm:
            member = mm.group(1).strip()
    # template is the last component of the head (namespaces before it), but keep qualification
    return (head, args, member)

fn_bytes = collections.Counter()
fn_count = collections.Counter()
fn_instr = collections.Counter()
inst_args = collections.defaultdict(set)
up_calls = collections.Counter()          # UniquePtr member -> call sites outside UniquePtr's own members
up_calls_internal = collections.Counter()
up_defs = collections.Counter()           # UniquePtr member -> definitions (instantiations)
mu_calls = 0
callee_re = re.compile(r'(?:call|invoke)\b[^@]*@("([^"]*)"|[A-Za-z_.$0-9]+)')
total_bytes = 0
total_fn_bytes = 0
total_fns = 0
total_instr = 0
cur = None
cur_bytes = 0
cur_instr = 0
cur_owner = None
with open(path, 'r', encoding='utf-8', errors='replace') as f:
    for line in f:
        total_bytes += len(line)
        if cur is None:
            if line.startswith('define '):
                at = line.find(' @')
                if at < 0:
                    continue
                if line[at + 2] == '"':
                    end = line.find('"', at + 3)
                    name = line[at + 3:end]
                else:
                    end = at + 2
                    while end < len(line) and (line[end].isalnum() or line[end] in '_.$'):
                        end += 1
                    name = line[at + 2:end]
                cur = name
                cur_bytes = len(line)
                cur_instr = 0
                cur_owner = owner_of(name)
            continue
        cur_bytes += len(line)
        if line.startswith('}'):
            head, args, member = cur_owner
            fn_bytes[head] += cur_bytes
            fn_count[head] += 1
            fn_instr[head] += cur_instr
            if args:
                inst_args[head].add(args)
            if head == 'UniquePtr':
                up_defs[member] += 1
            total_fn_bytes += cur_bytes
            total_fns += 1
            total_instr += cur_instr
            cur = None
            continue
        if line.startswith('  ') and not line.startswith('  ;'):
            cur_instr += 1
            if 'call ' in line or 'invoke ' in line:
                m = callee_re.search(line)
                if m:
                    callee = m.group(2) if m.group(2) is not None else m.group(1)
                    if callee.startswith('UniquePtr<'):
                        h, a, mem = owner_of(callee)
                        if cur_owner[0] == 'UniquePtr':
                            up_calls_internal[mem] += 1
                        else:
                            up_calls[mem] += 1
                    elif callee.startswith('makeUnique<'):
                        mu_calls += 1

print(f'IR bytes {total_bytes:,}; {total_fns:,} function definitions, {total_fn_bytes:,} bytes, {total_instr:,} instruction lines')
print(f'{"owner":40} {"defs":>8} {"distinct args":>13} {"bytes":>13} {"% bytes":>7} {"instr":>11} {"% instr":>7}')
for head, b in fn_bytes.most_common(top):
    print(f'{head[:40]:40} {fn_count[head]:8,} {len(inst_args[head]):13,} {b:13,} {100.0*b/total_fn_bytes:7.2f} {fn_instr[head]:11,} {100.0*fn_instr[head]/max(1,total_instr):7.2f}')
for name in ['UniquePtr', 'makeUnique', 'UniqueArray', 'makeUniqueArray', 'SharedPtr', 'makeShared', 'Optional', 'Vector', 'HashMap', 'Function']:
    b = fn_bytes[name]
    print(f'== {name}: {fn_count[name]:,} definitions for {len(inst_args[name]):,} argument lists, {b:,} bytes ({100.0*b/max(1,total_fn_bytes):.2f}%), {fn_instr[name]:,} instruction lines ({100.0*fn_instr[name]/max(1,total_instr):.2f}%)')
print('UniquePtr members: definitions / call sites outside UniquePtr / calls inside UniquePtr members')
for mem in sorted(set(up_defs) | set(up_calls) | set(up_calls_internal), key=lambda m: -(up_calls[m] + up_calls_internal[m])):
    print(f'  {mem:30} {up_defs[mem]:7,} {up_calls[mem]:9,} {up_calls_internal[mem]:9,}')
print(f'calls to makeUnique instantiations: {mu_calls:,}')
print(f'UniquePtr call sites outside its members: {sum(up_calls.values()):,}; inside: {sum(up_calls_internal.values()):,}')
