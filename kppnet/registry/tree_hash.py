"""The registry's content hash of a library release, and the store's write-once layout (layout.md).

    python -I tree_hash.py hash DIR                  print DIR's tree hash and its manifest
    python -I tree_hash.py pack DIR STORE            write DIR's archive and manifest into STORE
    python -I tree_hash.py verify STORE              recompute every archive's tree hash in STORE
    python -I tree_hash.py mirror FILE STORE         add a third-party file under STORE/mirror/

This is the reference definition kppc's implementation must agree with byte for byte (the hash goes
into build.lock). The tree hash of a directory:
1. Every regular file under it, by its path relative to it with '/' separators, in UTF-8. A
   symbolic link, a device or another special file is an error; empty directories do not count.
   Two paths that differ only in letter case are an error (the archive must unpack on Windows).
2. The files sorted by the bytes of their paths.
3. The manifest: one line per file, `<sha256 of the content, 64 lowercase hex> <size in bytes>
   <path>\n`. The content is the bytes as stored (line endings are not normalized).
4. The tree hash: `sha256-tree:` and the SHA-256 of the manifest's bytes, in lowercase hex.
"""
import hashlib
import os
import shutil
import stat
import sys
import zipfile

PREFIX = 'sha256-tree:'


def files_of(root):
    found = []
    for directory, dirs, names in os.walk(root, followlinks=False):
        for name in dirs:
            if os.path.islink(os.path.join(directory, name)):
                raise SystemExit(f'error: {os.path.join(directory, name)} is a symbolic link')
        for name in names:
            full = os.path.join(directory, name)
            mode = os.lstat(full).st_mode
            if not stat.S_ISREG(mode):
                raise SystemExit(f'error: {full} is not a regular file')
            found.append((os.path.relpath(full, root).replace(os.sep, '/'), full))
    found.sort(key=lambda item: item[0].encode('utf-8'))
    seen = {}
    for path, _ in found:
        other = seen.setdefault(path.lower(), path)
        if other != path:
            raise SystemExit(f'error: {other} and {path} differ only in letter case')
    return found


def file_sha256(full):
    digest = hashlib.sha256()
    with open(full, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def manifest_of(root):
    lines = []
    for path, full in files_of(root):
        lines.append(f'{file_sha256(full)} {os.path.getsize(full)} {path}\n')
    return ''.join(lines).encode('utf-8')


def tree_hash(manifest):
    return PREFIX + hashlib.sha256(manifest).hexdigest()


def store_paths(store, digest_hex):
    base = os.path.join(store, 'archives', digest_hex[:2])
    return os.path.join(base, digest_hex + '.zip'), os.path.join(base, digest_hex + '.manifest')


def pack(root, store):
    """A zip with sorted entries, fixed times and no extra fields, so packing twice gives the same
    bytes; kppc checks the tree hash after unpacking, never the zip's own bytes."""
    manifest = manifest_of(root)
    digest_hex = tree_hash(manifest)[len(PREFIX):]
    archive, manifest_path = store_paths(store, digest_hex)
    if os.path.exists(archive):
        print(f'{PREFIX}{digest_hex} is already in the store: {archive}')
        return
    os.makedirs(os.path.dirname(archive), exist_ok=True)
    temporary = archive + '.partial'
    with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path, full in files_of(root):
            info = zipfile.ZipInfo(path, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(full, 'rb') as f:
                z.writestr(info, f.read())
    with open(manifest_path + '.partial', 'wb') as f:
        f.write(manifest)
    os.replace(manifest_path + '.partial', manifest_path)
    os.replace(temporary, archive)
    print(f'{PREFIX}{digest_hex} {archive} sha256:{file_sha256(archive)} {os.path.getsize(archive)}')


def verify(store):
    bad = 0
    count = 0
    for directory, _, names in os.walk(os.path.join(store, 'archives')):
        for name in sorted(names):
            if not name.endswith('.zip'):
                continue
            count += 1
            expected = name[:-len('.zip')]
            scratch = os.path.join(store, '.verify')
            shutil.rmtree(scratch, ignore_errors=True)
            with zipfile.ZipFile(os.path.join(directory, name)) as z:
                z.extractall(scratch)
            got = tree_hash(manifest_of(scratch))[len(PREFIX):]
            shutil.rmtree(scratch, ignore_errors=True)
            if got != expected:
                bad += 1
                print(f'MISMATCH {os.path.join(directory, name)}: contents hash to {got}')
    print(f'{count} archives, {bad} mismatches')
    return 1 if bad else 0


def mirror(path, store):
    digest_hex = file_sha256(path)
    target_dir = os.path.join(store, 'mirror', digest_hex)
    target = os.path.join(target_dir, os.path.basename(path))
    if not os.path.exists(target):
        os.makedirs(target_dir, exist_ok=True)
        shutil.copyfile(path, target + '.partial')
        os.replace(target + '.partial', target)
    print(f'https://kpp.net/registry/mirror/{digest_hex}/{os.path.basename(path)}')


def main(args):
    if len(args) == 2 and args[0] == 'hash':
        manifest = manifest_of(args[1])
        sys.stdout.write(manifest.decode('utf-8'))
        print(tree_hash(manifest))
        return 0
    if len(args) == 3 and args[0] == 'pack':
        pack(args[1], args[2])
        return 0
    if len(args) == 2 and args[0] == 'verify':
        return verify(args[1])
    if len(args) == 3 and args[0] == 'mirror':
        mirror(args[1], args[2])
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
