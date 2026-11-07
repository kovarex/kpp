"""Builds and runs the standard library benchmarks: bench.kpp with kpp --release (default IR
module split, and one module, which shows what cross-module inlining would give), bench.cpp with
clang -O2 against the MSVC STL and, when WSL has the compilers, against libstdc++ and libc++.
Every benchmark runs in a process of its own (the heap state one leaves behind changes the next
one's malloc times). Prints a markdown table of milliseconds, best of 7 rounds; a benchmark
that takes over a minute is cut off. `run.cmd DIR` adds a column for the standard library in DIR
(an older version, say, from `git archive`).

Run through run.cmd, which sets up the Visual Studio environment for clang."""
import os
import shutil
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.normpath(os.path.join(here, '..', '..', 'build', 'bench', 'stdlib'))
kpp = os.path.normpath(os.path.join(here, '..', '..', 'build', 'kpp.exe'))
os.makedirs(out, exist_ok=True)


def build():
    programs = {}
    if len(sys.argv) > 1:
        std = os.path.abspath(sys.argv[1])
        subprocess.run([kpp, '--quiet', '--release', '--std', std, '--exe', os.path.join(out, 'kpp_other.exe'), 'bench.kpp'], cwd=here, check=True)
        programs['kpp, ' + os.path.basename(os.path.normpath(std))] = [os.path.join(out, 'kpp_other.exe')]
    subprocess.run([kpp, '--quiet', '--release', '--exe', os.path.join(out, 'kpp.exe'), 'bench.kpp'], cwd=here, check=True)
    programs['kpp'] = [os.path.join(out, 'kpp.exe')]
    subprocess.run([kpp, '--quiet', '--release', '--modules', '1', '--exe', os.path.join(out, 'kpp1.exe'), 'bench.kpp'], cwd=here, check=True)
    programs['kpp, 1 module'] = [os.path.join(out, 'kpp1.exe')]
    subprocess.run(['clang++', '-O2', '-std=c++20', 'bench.cpp', '-o', os.path.join(out, 'msvc.exe')], cwd=here, check=True)
    programs['MSVC STL'] = [os.path.join(out, 'msvc.exe')]
    if shutil.which('wsl') and subprocess.run(['wsl', '-e', 'sh', '-c', 'command -v clang++ && command -v g++'], capture_output=True).returncode == 0:
        subprocess.run(['wsl', '-e', 'g++', '-O2', '-std=c++20', 'bench.cpp', '-o', '/tmp/kpp_bench_libstdcxx'], cwd=here, check=True)
        programs['libstdc++'] = ['wsl', '-e', '/tmp/kpp_bench_libstdcxx']
        subprocess.run(['wsl', '-e', 'clang++', '-O2', '-std=c++20', '-stdlib=libc++', 'bench.cpp', '-o', '/tmp/kpp_bench_libcxx'], cwd=here, check=True)
        programs['libc++'] = ['wsl', '-e', '/tmp/kpp_bench_libcxx']
    return programs


def measure(command):
    results = []
    index = 0
    while True:
        try:
            lines = subprocess.run(command + [str(index)], capture_output=True, text=True, check=True, timeout=60).stdout.split()
        except subprocess.TimeoutExpired:
            results.append((None, None, None))
            index += 1
            continue
        if not lines:
            return results
        # "name words... 12.34 ms checksum"
        ms = lines.index('ms')
        results.append((' '.join(lines[:ms - 1]), float(lines[ms - 1]), lines[ms + 1]))
        index += 1


def main():
    programs = build()
    columns = {}
    for name, command in programs.items():
        print('running', name, file=sys.stderr)
        columns[name] = measure(command)
    names = list(programs)
    rows = columns['kpp']
    print('| benchmark | ' + ' | '.join(names) + ' |')
    print('|---|' + '---:|' * len(names))
    for i, (bench, _, checksum) in enumerate(rows):
        cells = []
        for name in names:
            other, ms, other_checksum = columns[name][i]
            if other is None:
                cells.append('> 60 s')
                continue
            assert other == bench, (other, bench)
            cells.append(f'{ms:.1f}' + ('' if other_checksum == checksum else ' (checksum differs!)'))
        print(f'| {bench} | ' + ' | '.join(cells) + ' |')


main()
