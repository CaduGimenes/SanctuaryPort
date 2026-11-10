#!/usr/bin/env python3
"""Downloads MSYS2 CLANG64 packages with their dependencies and unpacks them into a sysroot, for
cross-compiling the Windows build from Linux (packaging/windows/cross-build.sh).

usage: msys2_fetch.py DB_FILE CACHE_DIR SYSROOT pkg [pkg ...]
"""
import io
import re
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

REPO = 'https://repo.msys2.org/mingw/clang64/'
PREFIX = 'mingw-w64-clang-x86_64-'


def read_db(path):
    raw = subprocess.run(['zstd', '-dc', path], check=True, stdout=subprocess.PIPE).stdout
    pkgs, provides = {}, {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        for member in tar.getmembers():
            if not member.name.endswith('/desc'):
                continue
            text = tar.extractfile(member).read().decode()
            fields, key = {}, None
            for line in text.splitlines():
                if line.startswith('%') and line.endswith('%'):
                    key = line.strip('%')
                    fields[key] = []
                elif line and key:
                    fields[key].append(line)
            name = fields['NAME'][0]
            pkgs[name] = fields
            for p in fields.get('PROVIDES', []):
                provides.setdefault(re.split(r'[<>=]', p)[0], name)
    return pkgs, provides


def resolve(pkgs, provides, wanted):
    order, seen = [], set()
    stack = list(wanted)
    while stack:
        dep = re.split(r'[<>=]', stack.pop())[0]
        name = dep if dep in pkgs else provides.get(dep)
        if name is None:
            print(f'warning: {dep} not found', file=sys.stderr)
            continue
        if name in seen:
            continue
        seen.add(name)
        order.append(name)
        stack.extend(pkgs[name].get('DEPENDS', []))
    return order


def main():
    db, cache, root = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    pkgs, provides = read_db(db)
    names = resolve(pkgs, provides, [PREFIX + n if not n.startswith(PREFIX) else n for n in sys.argv[4:]])
    cache.mkdir(parents=True, exist_ok=True)
    root.mkdir(parents=True, exist_ok=True)
    for name in sorted(names):
        filename = pkgs[name]['FILENAME'][0]
        target = cache / filename
        if not target.exists():
            print('fetch', filename, flush=True)
            with urllib.request.urlopen(REPO + filename) as response:
                target.write_bytes(response.read())
        subprocess.run(['bsdtar', '-xf', str(target), '-C', str(root), '--exclude', '.*'], check=True)
    print(len(names), 'packages in', root)


if __name__ == '__main__':
    main()
