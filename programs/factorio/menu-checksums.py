"""Every menu simulation the game ships (C:\\Factorio\\data\\*\\menu-simulations\\*.zip, 45 maps) through
a build of the converted game's --benchmark, and the checksum it prints after the ticks
(Scenario::getCrc of the complete-mode save, the state a multiplayer desync check compares) against the
C++ build's, which menu-checksums.txt keeps (docs/path-to-factorio.md, "The menu simulations against
the C++ build").

  python -I menu-checksums.py GAME_DIR [--only NAME[,NAME...]]
  python -I menu-checksums.py --baseline [--ticks N]

GAME_DIR is laid out as run.cmd lays out C:\\factorio-in-kpp: bin\\x64\\factorio.exe, with the
config-path.cfg next to it that names its write directory. The games run one at a time (a write
directory's lock allows one game). The mods of that write directory count: menu-checksums.txt also
keeps the C++ run's prototype list checksum, and a map whose run printed another one is reported as
not comparable instead of as different (run.cmd's test_mod, a scratch mod).

--baseline runs the C++ Debug build, C:\\Factorio\\bin\\Debugx64vs2022\\factorio-run.exe (the C++
release build on disk is older than the source; "The release build"), with a config of its own whose
write directory is kpp\\menu-checksums\\cpp (never C:\\Factorio), and writes menu-checksums.txt; it
takes about 6 minutes. Every run's output is kept in kpp\\menu-checksums\\logs. For a map that
differs: bisect the ticks, then diff tagged saves of the first differing tick, as the section says.
"""
import argparse, glob, os, re, subprocess, sys, time

# The Factorio source tree (data, its menu simulations): C:\Factorio unless the environment's FACTORIO names another.
FACTORIO = os.environ.get('FACTORIO', r'C:\Factorio')

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'kpp', 'menu-checksums')
REFERENCE = os.path.join(HERE, 'menu-checksums.txt')
CPP = r'C:\Factorio\bin\Debugx64vs2022\factorio-run.exe'
MAPS = sorted(glob.glob(os.path.join(FACTORIO, 'data', '*', 'menu-simulations', '*.zip')), key=lambda p: os.path.basename(p))


def name_of(path):
    return os.path.splitext(os.path.basename(path))[0]


def run(command, cwd, log):
    start = time.time()
    p = subprocess.run(command, cwd=cwd, capture_output=True)
    text = (p.stdout + p.stderr).decode('utf-8', errors='replace').replace('\r', '')
    with open(log, 'w', encoding='utf-8') as f:
        f.write(text)
    checksum = re.search(r'^  checksum: (\d+)$', text, re.M)
    prototypes = re.search(r'Prototype list checksum: (\d+)', text)
    version = re.search(r'Factorio \d+\.\d+\.\d+ \(build \d+[^)]*\)', text)
    return (checksum.group(1) if checksum else None, prototypes.group(1) if prototypes else None,
            version.group(0) if version else None, time.time() - start, p.returncode)


def baseline(ticks):
    cpp = os.path.join(OUT, 'cpp')
    os.makedirs(os.path.join(cpp, 'config'), exist_ok=True)
    os.makedirs(os.path.join(cpp, 'run'), exist_ok=True)
    config = os.path.join(cpp, 'config', 'config.ini')
    with open(config, 'w') as f:
        f.write('; version=13\n[path]\nread-data=%s/data\nwrite-data=%s\n\n[other]\ncheck-updates=false\n'
                % (FACTORIO.replace('\\', '/'), os.path.join(cpp, 'run').replace('\\', '/')))
    lines, prototypes, version = [], set(), None
    for path in MAPS:
        checksum, protos, ver, seconds, code = run([CPP, '--config', config, '--benchmark', path, '--benchmark-ticks', str(ticks)],
                                                   cpp, os.path.join(OUT, 'logs', 'cpp-' + name_of(path) + '.txt'))
        print('%-44s %-11s %5.1f s' % (name_of(path), checksum, seconds), flush=True)
        if not checksum:
            sys.exit('the C++ build printed no checksum for %s (exit %d): kpp\\menu-checksums\\logs\\cpp-%s.txt' % (path, code, name_of(path)))
        lines.append('%s %s' % (name_of(path), checksum))
        prototypes.add(protos)
        version = version or ver
    if len(prototypes) != 1:
        sys.exit('the C++ runs printed different prototype list checksums: %s' % sorted(prototypes))
    with open(REFERENCE, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# The C++ build\'s checksums after --benchmark-ticks %d of every menu simulation, measured by\n' % ticks)
        f.write('# `menu-checksums.py --baseline` on %s with %s, %s.\n' % (time.strftime('%Y-%m-%d'), os.path.basename(CPP), version))
        f.write('ticks %d\nprototype-list %s\n' % (ticks, prototypes.pop()))
        f.write('\n'.join(lines) + '\n')
    print('%d maps into %s' % (len(lines), REFERENCE))


def check(game, only):
    reference, ticks, prototypes = {}, None, None
    for line in open(REFERENCE, encoding='utf-8'):
        words = line.split()
        if not words or words[0].startswith('#'):
            continue
        if words[0] == 'ticks':
            ticks = int(words[1])
        elif words[0] == 'prototype-list':
            prototypes = words[1]
        else:
            reference[words[0]] = words[1]
    exe_dir = os.path.join(game, 'bin', 'x64')
    exe = os.path.join(exe_dir, 'factorio.exe')
    if not os.path.exists(exe):
        sys.exit('no game at %s' % exe)
    maps = [p for p in MAPS if not only or name_of(p) in only]
    same, differ, other = [], [], []
    for path in maps:
        name = name_of(path)
        checksum, protos, _, seconds, code = run([exe, '--no-crash-handler', '--benchmark', path, '--benchmark-ticks', str(ticks)],
                                                 exe_dir, os.path.join(OUT, 'logs', 'kpp-' + name + '.txt'))
        expected = reference.get(name)
        if not checksum or not expected:
            state = 'no checksum (exit %d)' % code if not checksum else 'not in menu-checksums.txt'
            other.append(name)
        elif protos != prototypes:
            state = 'not comparable: prototype list %s, C++ %s (other mods?)' % (protos, prototypes)
            other.append(name)
        elif checksum == expected:
            state = 'same'
            same.append(name)
        else:
            state = 'DIFFERENT'
            differ.append(name)
        print('%-44s %-11s %-11s %-9s %5.1f s' % (name, checksum, expected, state, seconds), flush=True)
    print('%d maps, %d ticks: %d same, %d different%s%s' % (len(maps), ticks, len(same), len(differ),
                                                          ' (' + ', '.join(differ) + ')' if differ else '',
                                                          ', %d not compared (%s)' % (len(other), ', '.join(other)) if other else ''))
    return 0 if not differ and not other else 1


def main():
    parser = argparse.ArgumentParser(description='The menu simulations\' checksums against the C++ build.')
    parser.add_argument('game', nargs='?', help='the converted game\'s directory (bin\\x64\\factorio.exe)')
    parser.add_argument('--baseline', action='store_true', help='measure the C++ build into menu-checksums.txt')
    parser.add_argument('--ticks', type=int, default=600, help='ticks per map for --baseline (default 600)')
    parser.add_argument('--only', help='comma-separated map names')
    args = parser.parse_args()
    os.makedirs(os.path.join(OUT, 'logs'), exist_ok=True)
    if args.baseline:
        baseline(args.ticks)
        return 0
    if not args.game:
        parser.error('give the game directory, or --baseline')
    return check(args.game, set(args.only.split(',')) if args.only else None)


if __name__ == '__main__':
    sys.exit(main())
