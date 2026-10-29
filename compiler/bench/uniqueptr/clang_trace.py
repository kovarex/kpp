# clang-cl /Zs with -ftime-trace (granularity 0) over a random sample of Factorio .cpp files (flags of
# compiler/bench/clang_syntax_only.ps1): how much of clang's front-end time goes to instantiating
# std::unique_ptr (and default_delete, make_unique, the _Compressed_pair it holds), how many
# instantiations each TU makes, and how often the same one is made again in another TU.
# Run inside the VS developer environment (INCLUDE set): clang_trace.cmd SAMPLE SEED PARALLEL OUTDIR
import sys, os, json, random, re, subprocess, collections, concurrent.futures, time

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
flags = ['/Zs', '/std:c++latest', '/EHs', '/fp:strict', '/FIwin_fp_flags.hpp', '/w', '-Wno-everything'] + ['/D' + d for d in defines] + \
        ['/I' + os.path.join(root, i) for i in includes]

files = []
for dirpath, dirnames, filenames in os.walk(os.path.join(root, 'src')):
    for n in filenames:
        if n.endswith('.cpp') and n != 'Common.cpp':
            files.append(os.path.join(dirpath, n))
files.sort()
random.Random(seed).shuffle(files)
picked = files[:sample]

CORE = re.compile(r'^std::(unique_ptr<|default_delete<|make_unique<|make_unique_for_overwrite<|_Compressed_pair<std::default_delete<|_Get_deleter_pointer_type<|_Unique_ptr_enable_default_t)')
# std templates that hold unique_ptrs (their cost is the container's, not unique_ptr's)
CONTAINERS = {'vector', '_Vector_val', '_Vector_iterator', '_Vector_const_iterator', 'allocator', 'allocator_traits',
              '_Default_allocator_traits', '_Normal_allocator_traits', '_Is_default_allocator', '_Simple_types', '_Destroy_range',
              '_Tree', '_Tree_val', '_Tree_node', '_Tree_iterator', '_Tree_const_iterator', '_Tree_simple_types', '_Tmap_traits', '_Tset_traits',
              'map', 'multimap', 'set', 'multiset', 'deque', '_Deque_val', '_Deque_iterator', '_Deque_const_iterator', 'list', '_List_val',
              '_List_node', '_List_iterator', '_List_const_iterator', '_List_simple_types', 'array', 'pair', 'unordered_map', 'unordered_set',
              '_Hash', '_Uhash_compare', '_Umap_traits', '_Uset_traits', 'optional', '_Optional_construct_base', '_Optional_destruct_base',
              'variant', 'tuple', '_Tuple_val', 'span', 'initializer_list', '_Uninitialized_backout_al', '_Uninitialized_move',
              '_Uninitialized_copy', '_Construct_in_place', '_Destroy_in_place', '_Compressed_pair', 'less', 'hash', 'equal_to',
              'reverse_iterator', 'move_iterator', '_Tidy_guard', '_Tidy_deallocate_guard', 'function', '_Func_class', '_Func_impl_no_alloc',
              'shared_ptr', '_Ptr_base', '_Ref_count_resource', 'swap', 'move', 'forward', 'exchange', 'sort', 'find_if', 'remove_if', 'erase_if'}
INST = ('InstantiateClass', 'InstantiateFunction')

def classify(d):
    """'core': unique_ptr itself (class, members, deleter, its compressed pair, make_unique);
    'traits': std type traits asked about unique_ptr/default_delete (its constraints); None otherwise."""
    if CORE.match(d):
        return 'core'
    if not d.startswith('std::'):
        return None
    if 'unique_ptr' not in d and 'default_delete' not in d and 'make_unique' not in d:
        return None
    head = d[5:].split('<', 1)[0].split('::', 1)[0]
    if head in CONTAINERS:
        return None
    return 'traits'

def analyse(trace):
    events = [e for e in trace['traceEvents'] if e.get('ph') == 'X' and not e.get('name', '').startswith('Total ')]
    total = 0
    for e in trace['traceEvents']:
        if e.get('name') == 'Total ExecuteCompiler':
            total = e['dur']
    # nesting: sort by start, longer first; a stack of open events
    events.sort(key=lambda e: (e['ts'], -e['dur']))
    up_excl = 0; inst_excl = 0
    up_classes = []; up_functions = 0; mu = 0
    stack = []
    child = collections.defaultdict(int)
    excl = {}
    for idx, e in enumerate(events):
        while stack and events[stack[-1]]['ts'] + events[stack[-1]]['dur'] <= e['ts']:
            stack.pop()
        if stack:
            child[stack[-1]] += e['dur']
        stack.append(idx)
    traits_excl = 0
    for idx, e in enumerate(events):
        if e['name'] not in INST:
            continue
        x = e['dur'] - child[idx]
        inst_excl += x
        d = e.get('args', {}).get('detail', '')
        k = classify(d)
        if k == 'core':
            up_excl += x
            if e['name'] == 'InstantiateClass' and d.startswith('std::unique_ptr<'):
                up_classes.append(d)
            elif d.startswith('std::make_unique<'):
                mu += 1
            else:
                up_functions += 1
        elif k == 'traits':
            traits_excl += x
    return total, inst_excl, up_excl, up_classes, up_functions, mu, traits_excl

def run(i, path):
    out = os.path.join(outdir, f'{i}.json')
    t0 = time.time()
    r = subprocess.run([clang] + flags + ['-Xclang', '-ftime-trace=' + out, '-Xclang', '-ftime-trace-granularity=0', path],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wall = time.time() - t0
    if r.returncode != 0 or not os.path.exists(out):
        return path, None
    with open(out, 'r', encoding='utf-8') as f:
        trace = json.load(f)
    os.remove(out)
    return path, (wall,) + analyse(trace)

results = []
with concurrent.futures.ThreadPoolExecutor(parallel) as ex:
    for path, res in ex.map(lambda a: run(*a), enumerate(picked)):
        results.append((path, res))

ok = [(p, r) for p, r in results if r]
n = len(ok)
total = sum(r[1] for p, r in ok) / 1e6
inst = sum(r[2] for p, r in ok) / 1e6
up = sum(r[3] for p, r in ok) / 1e6
classes = collections.Counter()
per_tu = []
for p, r in ok:
    classes.update(set(r[4]))
    per_tu.append(len(set(r[4])))
upf = sum(r[5] for p, r in ok)
mu = sum(r[6] for p, r in ok)
traits = sum(r[7] for p, r in ok) / 1e6
print(f'TUs compiled: {n} of {len(picked)} sampled ({len(files)} .cpp in src)')
print(f'clang front-end time (Total ExecuteCompiler): {total:.1f} s, {total / n:.3f} s per TU')
print(f'instantiation (InstantiateClass/Function, exclusive): {inst:.1f} s = {100 * inst / total:.1f}% of front-end time')
print(f'unique_ptr family (unique_ptr, default_delete, make_unique, _Compressed_pair<default_delete>): {up:.2f} s = '
      f'{100 * up / total:.2f}% of front-end time, {100 * up / max(inst, 1e-9):.2f}% of instantiation time, {1000 * up / n:.1f} ms per TU')
print(f'std type traits asked about unique_ptr/default_delete (its constraints): {traits:.2f} s = {100 * traits / total:.2f}% of front-end time, '
      f'{1000 * traits / n:.1f} ms per TU')
print(f'unique_ptr<T> class instantiations: {sum(per_tu)} in {n} TUs ({sum(per_tu) / n:.1f} per TU, max {max(per_tu)}), '
      f'{len(classes)} distinct; each distinct one made in {sum(per_tu) / max(1, len(classes)):.1f} TUs on average')
print(f'unique_ptr member/deleter function instantiations: {upf} ({upf / n:.1f} per TU); make_unique instantiations: {mu} ({mu / n:.1f} per TU)')
print('most repeated:')
for c, k in classes.most_common(10):
    print(f'  {k:5} {c[:110]}')
with open(os.path.join(outdir, 'per_tu.tsv'), 'w', encoding='utf-8') as f:
    for p, r in ok:
        f.write(f'{p}\t{r[0]:.3f}\t{r[1] / 1e6:.4f}\t{r[2] / 1e6:.4f}\t{r[3] / 1e6:.4f}\t{len(set(r[4]))}\t{r[5]}\t{r[6]}\t{r[7] / 1e6:.4f}\n')
