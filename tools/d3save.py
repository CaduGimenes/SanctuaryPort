#!/usr/bin/env python3
"""Diablo III console saves (account.dat, heroes/*.dat, profile.dat): rolling XOR obfuscation.

The key starts at 0x305F92D82EC9A01B; each byte is XORed with the key's low byte and the
encrypted byte is shifted into the key's top. The plain data is protobuf: the game embeds its
.proto descriptors (D3.Account.SavedDefinition is account.dat's root); `descriptors` writes
them as a FileDescriptorSet for protoc --descriptor_set_in.

usage: d3save.py decode|encode IN OUT
       d3save.py descriptors EBOOT OUT.bin
"""
import re
import sys

KEY = 0x305F92D82EC9A01B


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


def _varint(b, i):
    v = s = 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7F) << s
        s += 7
        if x < 0x80:
            return v, i


def _extent(b, i):
    """End of the FileDescriptorProto at i: its fields are 1..14, varint or length-delimited."""
    while i < len(b):
        tag, j = _varint(b, i)
        field, wire = tag >> 3, tag & 7
        if not 1 <= field <= 14 or wire not in (0, 2):
            return i
        if wire == 0:
            _, j = _varint(b, j)
        else:
            n, j = _varint(b, j)
            j += n
        i = j
    return i


def descriptors(eboot):
    out = bytearray()
    for m in re.finditer(rb'\x0a([\x05-\x40])([A-Za-z0-9_/]+\.proto)', eboot):
        if len(m.group(2)) != m.group(1)[0]:
            continue
        blob = eboot[m.start():_extent(eboot, m.start())]
        n, header = len(blob), bytearray([0x0A])
        while True:
            x, n = n & 0x7F, n >> 7
            header.append(x | (0x80 if n else 0))
            if not n:
                break
        out += header + blob
    return bytes(out)


if __name__ == '__main__':
    mode, src, dst = sys.argv[1:4]
    data = open(src, 'rb').read()
    result = {'decode': decode, 'encode': encode, 'descriptors': descriptors}[mode](data)
    open(dst, 'wb').write(result)
