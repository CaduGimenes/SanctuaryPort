"""Unlock Diablo III's cosmetics (wings, pets, pennants, portrait frames) and transmog
appearances in the account save.

The console save user/savedata/<user>/<TITLE>/autosave/account.dat is protobuf
(D3.Account.SavedDefinition) behind a rolling XOR (tools/d3save.py). Field 24,
account_wide_cosmetic_item_data, holds a BitPackedGbidArray: explicit gbids (1), a bitfield (2)
with one bit per entry of the game's cosmetic table, and the number of leading zero bytes left
out of it (3). assets/cosmetics/diablo3.json lists that table; Blizzard's art tests (Art_Test_*)
and unnamed entries are never unlocked. Transmog appearances are the same kind of bitfield in each
partition's (game mode's) crafter data, over the game's transmog table; only the appearances the
game itself keeps are set. Everything else in the save is copied byte for byte, and
the file is backed up next to it first. The game keeps the bits when it saves again.

usage: cosmetics.py status|unlock SAVEDATA_DIR
"""
import json
import shutil
import sys
import time
from pathlib import Path

KEY = 0x305F92D82EC9A01B
TABLE = Path(__file__).resolve().parent.parent / 'assets' / 'cosmetics' / 'diablo3.json'
COSMETICS_FIELD = 24  # SavedDefinition.account_wide_cosmetic_item_data
ARRAY_FIELD = 1       # CosmeticItemSavedData.cosmetic_items (BitPackedGbidArray)
PARTITIONS_FIELD = 20  # SavedDefinition.partitions (normal, hardcore, seasonal...)
CRAFTER_FIELD = 4      # AccountPartition.crafter_data (CrafterSavedData: the artisans)
TRANSMOG_FIELD = 2     # CrafterSavedData.transmog_data (BitPackedGbidArray)
BITFIELD, LEADING_NULLS = 2, 3


def decode(data):
    key, out = KEY, bytearray()
    for b in data:
        out.append(b ^ (key & 0xFF))
        key = ((b << 56) | (key >> 8)) & 0xFFFFFFFFFFFFFFFF
    return bytes(out)


def encode(data):
    key, out = KEY, bytearray()
    for p in data:
        c = p ^ (key & 0xFF)
        out.append(c)
        key = ((c << 56) | (key >> 8)) & 0xFFFFFFFFFFFFFFFF
    return bytes(out)


def _varint(data, i):
    value = shift = 0
    while True:
        if i >= len(data):
            raise ValueError('truncated varint')
        b = data[i]
        i += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if b < 0x80:
            return value, i
        if shift > 63:
            raise ValueError('varint too long')


def _put_varint(value):
    out = bytearray()
    while True:
        b, value = value & 0x7F, value >> 7
        out.append(b | (0x80 if value else 0))
        if not value:
            return bytes(out)


def fields(data):
    """[(number, wire type, value or bytes, raw bytes)] of one message, in order."""
    out, i = [], 0
    while i < len(data):
        start = i
        tag, i = _varint(data, i)
        number, wire = tag >> 3, tag & 7
        if wire == 0:
            value, i = _varint(data, i)
        elif wire == 1:
            value, i = data[i:i + 8], i + 8
        elif wire == 2:
            n, i = _varint(data, i)
            value, i = data[i:i + n], i + n
        elif wire == 5:
            value, i = data[i:i + 4], i + 4
        else:
            raise ValueError(f'wire type {wire}')
        if i > len(data):
            raise ValueError('truncated field')
        out.append((number, wire, value, data[start:i]))
    return out


def _length_field(number, payload):
    return _put_varint(number << 3 | 2) + _put_varint(len(payload)) + payload


def unlockable_bits():
    items = json.loads(TABLE.read_text(encoding='utf-8'))['items']
    return [x['bit'] for x in items if x['name'] and not x['name'].startswith('Art_Test')]


def transmog_bits():
    return [x['bit'] for x in json.loads(TABLE.read_text(encoding='utf-8'))['transmog'] if x['valid']]


def _bits(array):
    leading, bitfield = 0, b''
    for number, wire, value, _ in fields(array):
        if number == BITFIELD and wire == 2:
            bitfield = value
        elif number == LEADING_NULLS and wire == 0:
            leading = value
    return bytes(leading) + bitfield


def unlocked(plain):
    """The cosmetic table bits set in a decoded account save."""
    for number, wire, value, _ in fields(plain):
        if number == COSMETICS_FIELD and wire == 2:
            for n2, w2, v2, _ in fields(value):
                if n2 == ARRAY_FIELD and w2 == 2:
                    bits = _bits(v2)
                    return {i for i in range(len(bits) * 8) if bits[i // 8] >> (i % 8) & 1}
    return set()


def _with_bits(array, bits):
    """A BitPackedGbidArray with `bits` added; its explicit gbids and other fields are kept."""
    current = bytearray(_bits(array))
    size = max(len(current), max(bits) // 8 + 1)
    current += bytes(size - len(current))
    for b in bits:
        current[b // 8] |= 1 << (b % 8)
    leading = len(current) - len(current.lstrip(b'\0'))
    kept = b''.join(raw for n, _, _, raw in fields(array) if n not in (BITFIELD, LEADING_NULLS))
    out = kept + _length_field(BITFIELD, bytes(current[leading:]))
    if leading:
        out += _put_varint(LEADING_NULLS << 3) + _put_varint(leading)
    return out


def _edit(message, number, edit, create=True):
    """`message` with its first length-delimited field `number` replaced by edit(value) (added
    when missing and `create`); every other field is copied as it is."""
    out, done = bytearray(), False
    for n, wire, value, raw in fields(message):
        if n == number and wire == 2 and not done:
            out += _length_field(number, edit(value))
            done = True
        else:
            out += raw
    if not done and create:
        out += _length_field(number, edit(b''))
    return bytes(out)


def unlock(plain, bits):
    """The decoded save with `bits` added to the cosmetic bitfield; nothing else changes."""
    return _edit(plain, COSMETICS_FIELD,
                 lambda data: _edit(data, ARRAY_FIELD, lambda array: _with_bits(array, bits)))


def _partitions(plain):
    return [value for n, wire, value, _ in fields(plain) if n == PARTITIONS_FIELD and wire == 2]


def transmog_unlocked(plain):
    """Per partition that has crafter data (the Mystic's), the transmog table bits set."""
    out = []
    for partition in _partitions(plain):
        for n, wire, crafter, _ in fields(partition):
            if n == CRAFTER_FIELD and wire == 2:
                arrays = [v for n2, w2, v, _ in fields(crafter) if n2 == TRANSMOG_FIELD and w2 == 2]
                bits = _bits(arrays[0]) if arrays else b''
                out.append({i for i in range(len(bits) * 8) if bits[i // 8] >> (i % 8) & 1})
    return out


def unlock_transmog(plain, bits):
    """`bits` added to the transmog data of every partition that has crafter data."""
    def partition(data):
        return _edit(data, CRAFTER_FIELD,
                     lambda crafter: _edit(crafter, TRANSMOG_FIELD, lambda a: _with_bits(a, bits)),
                     create=False)
    out = bytearray()
    for n, wire, value, raw in fields(plain):
        out += _length_field(n, partition(value)) if n == PARTITIONS_FIELD and wire == 2 else raw
    return bytes(out)


def account_files(savedata):
    return sorted(Path(savedata).glob('*/*/autosave/account.dat'))


def main(argv):
    if len(argv) != 3 or argv[1] not in ('status', 'unlock'):
        print(__doc__.strip().splitlines()[-1])
        return 2
    files = account_files(argv[2])
    if not files:
        print(f'Cosmetics: no account save in {argv[2]} (play once first).', flush=True)
        return 1
    wanted, looks = set(unlockable_bits()), set(transmog_bits())
    for path in files:
        plain = decode(path.read_bytes())
        before = unlocked(plain) & wanted
        partitions = transmog_unlocked(plain)
        missing = sum(len(looks - p) for p in partitions)
        if argv[1] == 'unlock' and (before != wanted or missing):
            backup = path.with_name(f'account.dat.{time.strftime("%Y%m%d-%H%M%S")}.bak')
            shutil.copy2(path, backup)
            changed = unlock_transmog(unlock(plain, wanted), looks)
            if unlocked(changed) & wanted != wanted or any(
                    looks - p for p in transmog_unlocked(changed)):
                raise SystemExit(f'{path}: the edited save does not read back')
            path.write_bytes(encode(changed))
            print(f'Cosmetics: {path}: {len(wanted) - len(before)} cosmetics and {missing} '
                  f'transmog appearances unlocked ({len(wanted)} cosmetics, {len(looks)} '
                  f'appearances in each of {len(partitions)} modes); backup {backup.name}', flush=True)
        else:
            shown = ', '.join(f'{len(p & looks)}' for p in partitions) or 'no mode played yet'
            print(f'Cosmetics: {path}: {len(before)} of {len(wanted)} cosmetics; transmog '
                  f'appearances {shown} of {len(looks)}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
