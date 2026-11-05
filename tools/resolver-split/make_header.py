"""Stage 2 of the resolver split: resolver.cpp's prologue and GlobalScope::Impl's definition become
Resolver/GlobalScopeImpl.hpp, shared by every part that is a translation unit of its own. File-scope
statics become inline (one object for all the translation units, as there was one for the one), the
anonymous namespaces resolver_detail (the types Impl's members use, the same in every unit).
  python make_header.py SRC_DIR PART_THAT_IS_A_HEADER..."""
import re, sys
src = sys.argv[1]
headers = set(sys.argv[2:]) # parts whose .inl became a .hpp (the class with declarations)
path = src + '/resolver.cpp'
lines = open(path, 'rb').read().decode().split('\n')
add_at = lines.index('void ResolveStats::add(const ResolveStats& o)')
scope_kind = lines.index('enum class ScopeKind : uint8_t { Namespace, Class, Enum };')
impl = lines.index('struct GlobalScope::Impl')
assert lines[impl + 1] == '{'
close = lines.index('};', impl)
prologue = lines[:add_at]
shared = lines[scope_kind:impl]
own = lines[add_at:scope_kind] # ResolveStats::add, stringLiteralArray, whyReport: resolver.hpp's, defined once

out = []
in_namespace = False
for k, line in enumerate(shared):
    if line == 'namespace':
        out.append('namespace resolver_detail')
        in_namespace = True
        continue
    if in_namespace and line == '}':
        out.append('}')
        out.append('using namespace resolver_detail;')
        in_namespace = False
        continue
    if line.startswith('static '):
        line = 'inline ' + line[len('static '):]
    elif in_namespace and re.match(r'^  [A-Za-z_][\w:<>,* ]*\s[\w]+\(.*\)( const)?$', line) and k + 1 < len(shared) and shared[k + 1] == '  {' \
            and not line.lstrip().startswith(('struct', 'class', 'enum', 'template', 'return', 'if', 'for', 'while', 'switch')):
        line = '  inline ' + line[2:]
    out.append(line)

parts = [l for l in lines[impl + 2:close]]
assert all(re.match(r'#include "Resolver/\w+\.inl"$', p) for p in parts), parts
parts = [p.replace('.inl"', '.hpp"') if re.search(r'/(\w+)\.inl"', p).group(1) in headers else p for p in parts]
header = ['#pragma once', '// GlobalScope::Impl, the resolver\'s state and work (resolver.hpp\'s GlobalScope), and what its parts share: a',
          '// header since 2026-11-05 (resolver.cpp split, stage 2), the parts each a translation unit of its own as they convert.'] + \
         prologue + [''] + out + ['struct GlobalScope::Impl', '{'] + [p.replace('"Resolver/', '"') for p in parts] + ['};', '']
open(src + '/Resolver/GlobalScopeImpl.hpp', 'wb').write('\n'.join(header).encode())
rest = lines[close + 1:]
new = ['#include "Resolver/GlobalScopeImpl.hpp"', ''] + own + rest
open(path, 'wb').write('\n'.join(new).encode())
print('header', len(header), 'lines; resolver.cpp', len(new))
