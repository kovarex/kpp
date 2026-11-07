# Call sites of UniquePtr members in kpp's IR, by member signature with the class argument written
# as T (and a converting member's own argument as U). Usage: python -X utf8 -I ir_calls.py full.ll
import sys, re, collections

def match_angle(s, i):
    depth = 0
    for j in range(i, len(s)):
        if s[j] == '<':
            depth += 1
        elif s[j] == '>':
            depth -= 1
            if depth == 0:
                return j + 1
    return len(s)

callee_re = re.compile(r'(?:call|invoke)\b[^@]*@"(UniquePtr<[^"]*)"')
define_re = re.compile(r'^define [^@]*@"([^"]*)"')
calls = collections.Counter()
calls_in_cleanup = collections.Counter()
defs = collections.Counter()
caller_is_up = False
in_cleanup = False
def sig(name):
    e = match_angle(name, len('UniquePtr'))
    t = name[len('UniquePtr') + 1:e - 1]
    rest = name[e:]
    if rest.startswith('::'):
        rest = rest[2:]
    rest = rest.replace(t + '*', 'T*').replace('UniquePtr<' + t + '>', 'UniquePtr<T>').replace('<' + t + '>', '<T>')
    # a converting member's own template argument
    rest = re.sub(r'UniquePtr<(?!T>)[^()]*?>(?=(::|\(|&|$))', 'UniquePtr<U>', rest)
    rest = re.sub(r'^operator(==|!=|=)<[^()]*>', r'operator\1<U>', rest)
    rest = re.sub(r'const (?!T\*)[A-Za-z_][A-Za-z_0-9:<>, ]*\*', 'const U*', rest)
    return rest

with open(sys.argv[1], 'r', encoding='utf-8', errors='replace') as f:
    for line in f:
        if line.startswith('define '):
            m = define_re.match(line)
            name = m.group(1) if m else ''
            caller_is_up = name.startswith('UniquePtr<')
            if caller_is_up:
                defs[sig(name)] += 1
            in_cleanup = False
            continue
        if line and not line.startswith(' ') and line.rstrip().endswith(':'):
            label = line.strip()[:-1]
            in_cleanup = label.startswith(('cleanup', 'ehcleanup', 'unwind', 'lpad', 'cl')) or 'cleanup' in label
            continue
        if ('call ' in line or 'invoke ' in line) and '@"UniquePtr<' in line and not caller_is_up:
            m = callee_re.search(line)
            if m:
                s = sig(m.group(1))
                calls[s] += 1
                if 'cleanuppad' in line or '"funclet"' in line:
                    calls_in_cleanup[s] += 1
total = sum(calls.values())
print(f'{total:,} call sites of UniquePtr members outside UniquePtr itself')
print(f'{"member":45} {"calls":>8} {"in funclets":>11} {"definitions":>11}')
for s, k in calls.most_common():
    print(f'{s[:45]:45} {k:8,} {calls_in_cleanup[s]:11,} {defs[s]:11,}')
print('defined but never called from outside:')
for s, k in defs.most_common():
    if s not in calls:
        print(f'  {s[:60]:60} {k:6,}')
