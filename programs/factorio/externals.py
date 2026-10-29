"""Why each unresolved external symbol of the game's link has no definition (link.cmd runs it).

    python externals.py kpp\\factorio.exe.link.txt kpp\\link-errors.txt [GROUP]

Three kinds, each counted (docs/path-to-factorio.md, "The link line"):
- left out because its code generation failed: a function the code generator reported an error in
  (`(in @"F")`), or one it left out because a function it asked for failed, a lambda in it most
  often (`requested by @"F"`). These follow the headline; split by the owner of the error (the
  same rules as the headline's breakdown).
- never emitted, with no error: a function or variable declared, used, and never defined, with
  nothing in the errors to say why. The headline does not count these.
- C names: a function of a C library the link line does not have, or an inline function of a C
  header the import declares and does not define.
With a GROUP (one of the printed names), its symbols are listed too.
"""
import collections
import re
import sys

OWNERS = [
    ('codegen: multiple inheritance layout', r'a second polymorphic base'),
    ('interpreter: constants', r'constant the compiler could not evaluate|condition the compiler cannot evaluate|static assertion whose condition|a template parameter whose value is not known'),
    ('codegen: closures', r'lambda used before its closure|closure of .* could not be built|closure of .* could not be laid out|by-reference init-capture|capture that is not a local'),
    ('codegen: range-for / iterators', r'a range whose iterators are not pointers|an iterator without operator'),
    ('checker: implicit moves and deleted special members', r'copying .*(deleted|no copy constructor|move constructor but no copy)|with a deleted constructor|through its deleted operator=|call to the deleted function'),
    ('checker: comparisons', r'has no operator|defaulted comparison|an operator with a class on the right'),
    ('checker: unresolved calls/members/names', r'a call that did not resolve|a member access that did not resolve|an unresolved name|a call to .* that did not resolve'),
    ('checker: unknown types', r'the type <unknown>|of unknown type|the type auto of|has an unknown type|the type \'.*\' is not declared|which is not declared'),
    ('checker: conversions and overloads', r'a conversion from|converting .* to|a constructor of .* taking|binding an unrelated class|assigning an unrelated class|passing .* where'),
    ('codegen: other constructs', r'statement kind|expression kind|a case label that is not a constant|pointer to a virtual member function|a member used outside a method|a method call on a non-class'),
]


def owner_of(line):
    for owner, pattern in OWNERS:
        if re.search(pattern, line):
            return owner
    return 'other'


def normalize(name):
    # Lambda numbers and hashes vary between runs.
    return re.sub(r'#[0-9a-f]{6,16}', '', re.sub(r'@\d+', '', name))


def main():
    link_txt, errors_txt = sys.argv[1], sys.argv[2]
    symbols = collections.OrderedDict()
    for line in open(link_txt, encoding='utf-8', errors='replace'):
        m = re.search(r'unresolved external symbol (.*?)(?: referenced in function (.*))?$', line.rstrip())
        if m:
            symbols.setdefault(m.group(1), set())
            if m.group(2):
                symbols[m.group(1)].add(m.group(2))
    failed = {}
    requested = {}
    for line in open(errors_txt, encoding='utf-8', errors='replace'):
        line = line.rstrip()
        m = re.search(r'\(in @"(.*?)"(?:, requested by @.*)?\)$', line) or re.search(r'\(in @([^ ,)]+)', line)
        if m:
            failed.setdefault(normalize(m.group(1)), owner_of(line))
        m = re.search(r', requested by @"?(.*?)"?\)$', line)
        if m:
            requested.setdefault(normalize(m.group(1)), owner_of(line))
    for name, owner in requested.items():
        failed.setdefault(name, owner)
    groups = collections.defaultdict(list)
    for symbol in symbols:
        if normalize(symbol) in failed:
            groups['left out, codegen failed: ' + failed[normalize(symbol)]].append(symbol)
        elif '::' not in symbol and '(' not in symbol:
            groups['C names'].append(symbol)
        else:
            groups['never emitted, no error'].append(symbol)
    failed_total = sum(len(v) for k, v in groups.items() if k.startswith('left out'))
    print(f'{len(symbols)} unresolved symbols: {failed_total} left out (codegen failed), '
          f'{len(groups["never emitted, no error"])} never emitted with no error, {len(groups["C names"])} C names')
    for group, members in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print(f'{len(members):6} {group}')
    if len(sys.argv) > 3:
        for symbol in groups.get(sys.argv[3], []):
            referrer = sorted(symbols[symbol])[0] if symbols[symbol] else ''
            print('   ', symbol[:200], '<-', referrer[:120])


main()
