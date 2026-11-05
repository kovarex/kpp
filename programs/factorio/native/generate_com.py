"""Generates the COM interfaces the converted game calls as kpp classes with virtual functions.

kpp's vtables are MSVC's for single inheritance (the vtable pointer at offset 0, the functions in
declaration order from slot 0, `this` first), so a COM object the system hands out is called
through a kpp class whose virtual functions are the interface's methods in vtable order. This
script reads that order and the methods' C signatures from the Windows SDK headers' C
definitions (the `typedef struct IFooVtbl { ... }` each interface has for C), the bases from their
C++ definitions (`MIDL_INTERFACE("...") IFoo : public IBar`), and writes:

- overrides/added/src/Graphics/DX11/ComInterfaces.kpp: one class per interface, its own methods
  as pure virtual functions (no destructor: COM objects are released, not deleted).
- native/include/kpp_directx.h: the header the converter imports for the DirectX includes; it
  defines each generated interface's __IFoo_INTERFACE_DEFINED__ guard first, so that the import
  keeps the SDK's types, functions and constants but not the C structs of these interfaces (their
  forward declarations stay, and name the kpp classes), then the other declarations those guards
  hold (IUnknown's LPUNKNOWN, IStream's LPSTREAM and STATSTG, ...), and the interfaces' IIDs, which
  the guards also left out, for `__uuidof(IFoo)` (dxguid.lib and uuid.lib define them). (In C:
  an `extern "C"` variable a kpp source declares is defined by codegen yet,
  interop/extern_variable_declaration.)

    python generate_com.py [SDK include directory]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FACTORIO = os.path.dirname(HERE)
SDK = sys.argv[1] if len(sys.argv) > 1 else r'C:\Program Files (x86)\Windows Kits\10\Include\10.0.19041.0'

# Headers whose interfaces are all generated, and single interfaces from the bigger ones.
WHOLE = [r'um\d3dcommon.h', r'shared\dxgi.h', r'shared\dxgi1_2.h', r'shared\dxgi1_3.h', r'shared\dxgi1_4.h', r'shared\dxgi1_5.h',
         r'um\d3d11.h', r'um\d3d11_1.h', r'um\d3d11sdklayers.h']
SINGLE = {r'um\Unknwn.h': ['IUnknown'], r'um\ObjIdl.h': ['ISequentialStream', 'IStream'],
          r'um\wincodec.h': ['IWICImagingFactory', 'IWICBitmapSource', 'IWICBitmapDecoder', 'IWICBitmapFrameDecode', 'IWICFormatConverter',
                             'IWICStream'],
          r'um\WbemCli.h': ['IWbemLocator', 'IWbemServices', 'IWbemClassObject', 'IEnumWbemClassObject']}
# Interfaces of headers that declare them with DECLARE_INTERFACE_ (d3d9.h), not MIDL's C structs:
# GraphicsInterfaceDX11::logDeviceInfo asks IDirect3D9Ex for the adapters' driver versions.
DECLARED = {r'shared\d3d9.h': ['IDirect3D9', 'IDirect3D9Ex']}
# IIDs the game names with __uuidof without calling the interface.
EXTRA_IIDS = ['ID3D10Device']
# What the import must include, in order (the SDK's own includes bring the rest).
IMPORTED = ['d3d11.h', 'd3d11_1.h', 'd3d11sdklayers.h', 'dxgi1_5.h', 'd3d9.h', 'd3dcompiler.h', 'wincodec.h', 'wbemidl.h']


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)


def strip_annotations(text):
    """Removes SAL and RPC annotations (`_In_reads_opt_(...)`, `__RPC__in`), with their argument lists."""
    out = []
    i = 0
    while i < len(text):
        m = re.match(r'(_[A-Z][A-Za-z0-9_]*_|__RPC__[A-Za-z0-9_]+)\b', text[i:]) if (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')) else None
        if m:
            i += m.end()
            j = i
            while j < len(text) and text[j] in ' \t\r\n':
                j += 1
            if j < len(text) and text[j] == '(':
                depth = 0
                while j < len(text):
                    if text[j] == '(':
                        depth += 1
                    elif text[j] == ')':
                        depth -= 1
                        if depth == 0:
                            j += 1
                            break
                    j += 1
                i = j
            continue
        out.append(text[i])
        i += 1
    return ''.join(out)


def split_top(params):
    parts, depth, current = [], 0, ''
    for c in params:
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
        if c == ',' and depth == 0:
            parts.append(current)
            current = ''
        else:
            current += c
    if current.strip():
        parts.append(current)
    return [' '.join(p.split()) for p in parts]


# Macros of guiddef.h that C reads as pointers (the import makes constants of object-like macros,
# not types).
TYPE_MACROS = {'REFIID': 'const IID*', 'REFGUID': 'const GUID*', 'REFCLSID': 'const CLSID*', 'REFFMTID': 'const FMTID*'}


def parameter(p):
    for macro, written in TYPE_MACROS.items():
        p = re.sub(r'\b' + macro + r'\b', written, p)
    # `const FLOAT ColorRGBA[ 4 ]` is a pointer, as C makes it.
    m = re.match(r'(.*?)(\w+)\s*\[\s*\w*\s*\]$', p)
    if m:
        p = m.group(1).rstrip() + '* ' + m.group(2)
    return re.sub(r'\s*\*\s*', '* ', p).replace('* *', '**').replace('*  ', '* ').strip()


def interfaces_of(path, wanted=None):
    text = strip_comments(open(path, encoding='utf-8', errors='replace').read())
    bases = {}
    for m in re.finditer(r'MIDL_INTERFACE\("([^"]+)"\)\s*(\w+)\s*(?::\s*public\s+(\w+))?\s*\{', text):
        bases[m.group(2)] = m.group(3)
    found = {}
    for m in re.finditer(r'typedef\s+struct\s+(\w+)Vtbl\s*\{(.*?)\}\s*\1Vtbl\s*;', text, flags=re.S):
        name, body = m.group(1), m.group(2)
        if wanted is not None and name not in wanted:
            continue
        body = body.replace('BEGIN_INTERFACE', ' ').replace('END_INTERFACE', ' ')
        body = re.sub(r'DECLSPEC_XFGVIRT\s*\([^)]*\)', ' ', body)
        body = strip_annotations(body)
        methods = []
        for e in re.finditer(r'([\w\s\*]+?)\(\s*STDMETHODCALLTYPE\s*\*\s*(\w+)\s*\)\s*\((.*?)\)\s*;', body, flags=re.S):
            result, method, params = ' '.join(e.group(1).split()), e.group(2), split_top(e.group(3))
            if not params or not re.match(r'(const\s+)?' + name + r'\s*\*\s*This$', params[0]):
                raise SystemExit(f'{path}: {name}::{method}: no `This` first')
            methods.append((result, method, [parameter(p) for p in params[1:]]))
        names = [x[1] for x in methods]
        if len(set(names)) != len(names):
            raise SystemExit(f'{path}: {name} has overloads, whose vtable order this script does not handle')
        if name not in bases and name != 'IUnknown':
            raise SystemExit(f'{path}: no C++ definition for the base of {name}')
        found[name] = (bases.get(name), methods)
    return found


def declared_interfaces_of(path, wanted):
    """The interfaces a header declares with `DECLARE_INTERFACE_(IFoo, IBar) { STDMETHOD(Method)(THIS_
    ...) PURE; STDMETHOD_(Result, Method)(THIS) PURE; ... };`, each listing its bases' methods first,
    as MIDL's C vtables do."""
    text = strip_comments(open(path, encoding='utf-8', errors='replace').read())
    found = {}
    for m in re.finditer(r'DECLARE_INTERFACE_\(\s*(\w+)\s*,\s*(\w+)\s*\)\s*\{(.*?)\}\s*;', text, flags=re.S):
        name, base, body = m.group(1), m.group(2), m.group(3)
        if name not in wanted:
            continue
        methods = []
        for e in re.finditer(r'STDMETHOD(?:_\(\s*([^,]+?)\s*,\s*(\w+)\s*\)|\(\s*(\w+)\s*\))\s*\((.*?)\)\s*PURE\s*;', body, flags=re.S):
            result = ' '.join(e.group(1).split()) if e.group(1) else 'HRESULT'
            method = e.group(2) or e.group(3)
            params = ' '.join(re.sub(r'\bCONST\b', 'const', e.group(4)).replace('FAR', ' ').split())
            if params == 'THIS':
                params = ''
            elif params.startswith('THIS_'):
                params = params[len('THIS_'):]
            else:
                raise SystemExit(f'{path}: {name}::{method}: no THIS first')
            methods.append((result, method, [parameter(x) for x in split_top(params)]))
        names = [x[1] for x in methods]
        if len(set(names)) != len(names):
            raise SystemExit(f'{path}: {name} has overloads, whose vtable order this script does not handle')
        found[name] = (base, methods)
    return found


def guarded_declarations(path, names):
    """The C declarations an interface's guard holds besides the interface (`typedef IUnknown
    *LPUNKNOWN;`, IStream's STATSTG and enums): the text between `#define __IFoo_INTERFACE_DEFINED__`
    and the first preprocessor line or the IID's declaration. Defining the guard leaves them out of
    the import, so kpp_directx.h declares them again after the SDK's headers."""
    text = strip_comments(open(path, encoding='utf-8', errors='replace').read())
    out = []
    for m in re.finditer(r'#define\s+__(\w+)_INTERFACE_DEFINED__[ \t]*\n(.*?)(?=^\s*#|EXTERN_C\s+const\s+IID\s+IID_\1\s*;)', text, flags=re.S | re.M):
        if m.group(1) in names and m.group(2).strip():
            body = [' '.join(line.split()) for line in m.group(2).strip().split('\n') if line.strip()]
            out.append((m.group(1), '\n'.join(body)))
    return out


def main():
    interfaces = {}
    for header in WHOLE:
        interfaces.update(interfaces_of(os.path.join(SDK, header)))
    for header, names in SINGLE.items():
        got = interfaces_of(os.path.join(SDK, header), set(names))
        missing = set(names) - set(got)
        if missing:
            raise SystemExit(f'{header}: {sorted(missing)} not found')
        interfaces.update(got)
    for header, names in DECLARED.items():
        got = declared_interfaces_of(os.path.join(SDK, header), set(names))
        missing = set(names) - set(got)
        if missing:
            raise SystemExit(f'{header}: {sorted(missing)} not found')
        interfaces.update(got)
    # Bases first, each after its base, then by name.
    order, done = [], set()

    def visit(name):
        if name in done:
            return
        base = interfaces[name][0]
        if base:
            if base not in interfaces:
                raise SystemExit(f'{name}: base {base} is not generated')
            visit(base)
        done.add(name)
        order.append(name)
    for name in sorted(interfaces):
        visit(name)

    def count(name):
        base = interfaces[name][0]
        return len(interfaces[name][1]) if not base else len(interfaces[name][1])

    sdk = os.path.basename(SDK.rstrip('\\/'))
    lines = ["requires config::platform == Platform::Windows; // (Windows' alone: the game's Windows code uses it)",
             '// Generated by programs/factorio/native/generate_com.py from the Windows SDK ' + sdk + ' headers: do not edit.',
             '// The COM interfaces the converted game calls (the DirectX backend, DXGI, WIC, IStream), as kpp classes with',
             '// virtual functions in vtable order: kpp\'s single-inheritance vtables are MSVC\'s, so an object the system',
             '// hands out is called through them directly. Each has only its own methods (the base\'s come first in the',
             '// vtable) and no destructor (COM objects are released). __uuidof(IFoo) is &IID_IFoo, which kpp_directx.h',
             '// declares (dxguid.lib, uuid.lib).',
             '']
    for name in order:
        base, methods = interfaces[name]
        own = methods
        if base:
            inherited = len(interfaces[base][1])
            if [m[1] for m in methods[:inherited]] != [m[1] for m in interfaces[base][1]]:
                raise SystemExit(f'{name}: its vtable does not start with {base}\'s')
            own = methods[inherited:]
        lines.append(f'class {name}' + (f' : public {base}' if base else ''))
        lines.append('{')
        lines.append('public:')
        # __uuidof(IFoo) as IFoo::iid(), for IUnknown's QueryInterface template; a template itself, so
        # that only the interfaces asked for need their IIDs' libraries at link time.
        lines.append('  template<int = 0>')
        lines.append(f'  static const IID* iid() {{ return &IID_{name}; }}')
        for result, method, params in own:
            lines.append(f'  virtual {result} {method}({", ".join(params)}) = 0;')
        if name == 'IUnknown':
            # unknwn.h's C++ overload: the interface Q asked for by Q's own id.
            lines.append('  template<typename Q>')
            lines.append('  HRESULT QueryInterface(Q** pp) { return this->QueryInterface(Q::iid(), (void**)pp); }')
        lines.append('};')
        lines.append('')
    out = os.path.join(FACTORIO, 'overrides', 'added', 'src', 'Graphics', 'DX11', 'ComInterfaces.kpp')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    header = ['// Generated by programs/factorio/native/generate_com.py: do not edit.',
              '// The DirectX, DXGI and WIC headers as the converted game imports them: the interfaces kpp declares as',
              '// classes (overrides/added/src/Graphics/DX11/ComInterfaces.kpp) are marked defined first, so that the import',
              '// keeps the SDK\'s types, functions and constants but not those interfaces\' C structs.',
              '#pragma once']
    header += [f'#define __{name}_INTERFACE_DEFINED__' for name in order]
    for h in IMPORTED:
        if h == 'd3d9.h':
            # Its interfaces have no guards: each DECLARE_INTERFACE_ becomes a forward declaration,
            # which names the kpp class where one is generated, and its method table a struct
            # nothing uses.
            header += ['#pragma push_macro("DECLARE_INTERFACE_")', '#undef DECLARE_INTERFACE_',
                       '#define DECLARE_INTERFACE_(iface, baseiface) typedef struct iface iface; struct iface##KppUnusedVtbl',
                       f'#include <{h}>', '#pragma pop_macro("DECLARE_INTERFACE_")']
        else:
            header.append(f'#include <{h}>')
    for path in [os.path.join(SDK, h) for h in WHOLE + list(SINGLE)]:
        for name, declarations in guarded_declarations(path, set(order)):
            header.append(f'// What the guard of {name} also holds ({os.path.basename(path)}).')
            header.append(declarations)
    header += [f'extern const IID IID_{name};' for name in order + EXTRA_IIDS]
    hpath = os.path.join(HERE, 'include', 'kpp_directx.h')
    os.makedirs(os.path.dirname(hpath), exist_ok=True)
    open(hpath, 'w', encoding='utf-8', newline='\n').write('\n'.join(header) + '\n')
    print(f'{len(order)} interfaces, {sum(len(interfaces[n][1]) for n in order)} vtable slots -> {out}, {hpath}')


main()
