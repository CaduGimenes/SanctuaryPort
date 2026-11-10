"""Add the NID -> name pairs a game's imports need to src/import_names.inc.

The runtime finds most functions by name (runtime_lookup), so an import whose NID is missing from
import_names.inc stops the game even when the function is implemented. Names come from:
  * other ports' import_names.inc files (--names), already NID -> name pairs;
  * the names in this runtime's export tables ({"name", fn}), hashed to their NID.
Only imports of the given game (eboot.bin and the bundled sce_module PRX files) are added.

usage: add_import_names.py GAME_DIR [--names other/import_names.inc ...]
"""
import argparse
import base64
import hashlib
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from link_modules import module  # noqa: E402

SALT = bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')
ENTRY = re.compile(r'\{"([A-Za-z0-9+_-]{11})#([A-Za-z0-9_]+)","([^"]+)"\}')
EXPORT = re.compile(r'\{\s*"([A-Za-z_][A-Za-z0-9_]{2,})"\s*,')


def nid(name):
    digest = hashlib.sha1(name.encode() + SALT).digest()[:8]
    value = struct.pack('<Q', struct.unpack('>Q', digest)[0])
    return base64.b64encode(value).decode().rstrip('=').replace('/', '-')


def canonical(library):
    # runtime.c canonical_name: libScePosix shares libkernel's contracts and key.
    return 'libkernel' if library == 'libScePosix' else library


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('game', type=Path)
    parser.add_argument('--names', type=Path, action='append', default=[])
    args = parser.parse_args()

    target = ROOT / 'src' / 'import_names.inc'
    existing = {f'{m[0]}#{m[1]}' for m in ENTRY.findall(target.read_text())}

    by_nid = {}
    for path in args.names:
        for n, _lib, name in ENTRY.findall(path.read_text(errors='ignore')):
            by_nid.setdefault(n, name)
    for path in (ROOT / 'src').rglob('*.[ch]'):
        if path.name == 'import_names.inc':
            continue
        for name in EXPORT.findall(path.read_text(errors='ignore')):
            by_nid.setdefault(nid(name), name)

    files = [args.game / 'eboot.bin'] + sorted((args.game / 'sce_module').glob('*.prx'))
    wanted = set()
    for path in files:
        try:
            m = module(path)
        except Exception as error:  # noqa: BLE001 - report and continue with the other files
            print(f'skip {path.name}: {error}', file=sys.stderr)
            continue
        for symbol in m['symbols']:
            identity = symbol['identity']
            if identity and symbol['section'] == 0:
                wanted.add((identity[0], canonical(identity[1][0])))

    added, unknown = [], []
    for n, library in sorted(wanted):
        key = f'{n}#{library}'
        if key in existing:
            continue
        if n in by_nid:
            added.append(f'{{"{key}","{by_nid[n]}"}},')
        else:
            unknown.append(key)
    if added:
        with target.open('a') as out:
            out.write(f'/* Added by tools/add_import_names.py for {args.game.name} */\n')
            out.write('\n'.join(added) + '\n')
    print(f'added {len(added)} names; {len(unknown)} imports still without a name')
    for key in unknown:
        print('  ', key)


if __name__ == '__main__':
    main()
