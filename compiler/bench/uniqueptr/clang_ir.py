# clang-cl /Od on a sample of Factorio .cpp files to textual LLVM IR: how many std::unique_ptr
# functions (members, default_delete, make_unique, _Compressed_pair of a default_delete) each TU
# emits (linkonce_odr, again in every TU) and their share of the TU's IR.
# Run inside the VS developer environment: clang_ir.cmd SAMPLE SEED PARALLEL OUTDIR
import sys, os, random, re, subprocess, concurrent.futures

sample = int(sys.argv[1]); seed = int(sys.argv[2]); parallel = int(sys.argv[3]); outdir = sys.argv[4]
os.makedirs(outdir, exist_ok=True)
root = r'C:\Factorio'
clang = r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe'
includes = ['src', 'libraries', r'libraries\StackWalker', r'libraries\curl\include', r'libraries\curl\lib', r'libraries\re2',
            r'libraries\trio', r'libraries\imgui', r'libraries\glad', r'libraries\stb_image', r'libraries\nvapi', r'libraries\SDL3\include',
            r'libraries\SDL_Mixer\include', r'libraries\glad\include', r'libraries\fontstash', r'libraries\steam', r'libraries\Win10SDK',
            r'libraries\libbacktrace', r'libraries\WinSDKhotfix']
defines = ['WIN32_LEAN_AND_MEAN', 'CRT_NONSTDC_NO_DEPRECATE', '_CRT_SECURE_NO_WARNINGS', '_SCL_SECURE_NO_WARNINGS', '_WINDOWS',
           '_USE_MATH_DEFINES', 'USE_THREADS', 'WIN32', '_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS', 'LUA_USE_LONGLONG', 'FACTORIO_USE_D3D',
           'SDL_MAIN_HANDLED', 'SDL_SENSOR_DISABLED', 'WIN64', 'SPACE_AGE', 'DEBUG', 'NOMINMAX']
flags = ['/c', '/Od', '/std:c++latest', '/EHs', '/fp:strict', '/FIwin_fp_flags.hpp', '/w', '-Wno-everything'] + \
        ['/D' + d for d in defines] + ['/I' + os.path.join(root, i) for i in includes]

files = []
for dirpath, dirnames, filenames in os.walk(os.path.join(root, 'src')):
    for n in filenames:
        if n.endswith('.cpp') and n != 'Common.cpp':
            files.append(os.path.join(dirpath, n))
files.sort()
random.Random(seed).shuffle(files)
picked = files[:sample]

# MSVC-mangled names: a member of unique_ptr is ...?$unique_ptr@..., the deleter ?$default_delete@, make_unique ??$make_unique@
UP = re.compile(r'^\?(?:\?[0-9A-Z_]|\?\$\?[0-9A-Z_][^@]*@(?:[^@]*@)*?|[A-Za-z_]\w*@)\?\$(?:unique_ptr|default_delete|_Compressed_pair@U\?\$default_delete)@|^\?\?\$make_unique@')

def run(i, path):
    # clang-cl writes the .ll next to the working directory (it ignores /Fo with /clang:-S)
    work = os.path.join(outdir, str(i))
    os.makedirs(work, exist_ok=True)
    out = os.path.join(work, os.path.splitext(os.path.basename(path))[0] + '.ll')
    r = subprocess.run([clang] + flags + ['/clang:-S', '/clang:-emit-llvm', path], cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0 or not os.path.exists(out):
        return path, None
    fns = 0; up = 0; fn_lines = 0; up_lines = 0
    cur_up = None
    with open(out, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('define '):
                fns += 1
                m = re.search(r'@"?([^"(\s]+)', line)
                name = m.group(1) if m else ''
                # the function's own name: only the part before the first argument list counts as "owner"
                cur_up = bool(UP.search(name))
                if cur_up:
                    up += 1
                continue
            if cur_up is not None:
                if line.startswith('}'):
                    cur_up = None
                    continue
                fn_lines += 1
                if cur_up:
                    up_lines += 1
    os.remove(out)
    return path, (fns, up, fn_lines, up_lines)

with concurrent.futures.ThreadPoolExecutor(parallel) as ex:
    results = list(ex.map(lambda a: run(*a), enumerate(picked)))
ok = [r for p, r in results if r]
n = len(ok)
fns = sum(r[0] for r in ok); up = sum(r[1] for r in ok); fl = sum(r[2] for r in ok); ul = sum(r[3] for r in ok)
print(f'TUs: {n} of {len(picked)}')
print(f'functions defined at /Od: {fns} ({fns / n:.0f} per TU); unique_ptr family: {up} ({up / n:.0f} per TU, {100 * up / fns:.1f}%)')
print(f'IR body lines: {fl} ({fl / n:.0f} per TU); unique_ptr family: {ul} ({100 * ul / fl:.2f}%)')
