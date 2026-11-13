"""Read-only access to Diablo III's console archives (CPKs/*.cpk, Blizzard's own format, version 7).

Layout (after github.com/RedMadKnight/D3CPKUnpack): a 72-byte little-endian header, then bit-packed
tables (most significant bit first): per file a 64-bit FNV-1a hash of the lower-case path, its size,
copy count and first location; the decompressed-stream offset of each location; the decompressed
offset of each compressed sector; the compressed sector of each decompressed one. Then one u32 name
offset per file and the names. Data: 16 KiB compressed sectors from header_sector * 64 KiB, each a
list of zlib chunks (u16 size low, u16 size high, u16 stored size; 0 ends the sector).

Only what the port needs: listing names and reading a file. Nothing is written.
"""
import os
import struct
import zlib

MAGIC = 0xA1B2C3D4


def _unpack(data, pos, count, width):
    """count unsigned fields of width bits, MSB first, from data[pos:]; returns (values, next pos)."""
    out = []
    bit = pos * 8
    mask = (1 << width) - 1
    span = (width + 7) // 8 + 1
    for _ in range(count):
        byte = bit >> 3
        chunk = int.from_bytes(data[byte:byte + span].ljust(span, b'\0'), 'big')
        out.append((chunk >> (span * 8 - (bit & 7) - width)) & mask)
        bit += width
    return out, (bit + 7) >> 3


def fnv1a64(name):
    h = 0xCBF29CE484222325
    for c in name.lower().encode('latin-1'):
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Archive:
    def __init__(self, path):
        self.path = path
        self.file = open(path, 'rb')
        head = self.file.read(72)
        (magic, version, decomp_size, layout, files, locations, header_sector, size_bits, count_bits,
         index_bits, location_bits, cs2do_bits, ds2cs_bits, _crc, read_sector, self.sector_size,
         _reserved) = struct.unpack('<IIQ14I', head)
        if magic != MAGIC or version != 7:
            raise ValueError(f'{path}: not a version 7 Diablo III archive')
        self.first = header_sector if layout == 10 else header_sector * read_sector
        self.file.seek(0)
        data = self.file.read(self.first)
        entry_bits = 64 + size_bits + count_bits + index_bits
        raw, pos = _unpack(data, 72, files, entry_bits)
        self.entries = []
        for value in raw:
            index = value & ((1 << index_bits) - 1)
            value >>= index_bits + count_bits
            size = value & ((1 << size_bits) - 1)
            self.entries.append((value >> size_bits, size, index))
        self.locations, pos = _unpack(data, pos, locations, location_bits)
        sectors = (os.path.getsize(path) - self.first) // self.sector_size
        self.cs2do, pos = _unpack(data, pos, sectors, cs2do_bits)
        self.ds2cs, pos = _unpack(data, pos, (decomp_size + self.sector_size - 1) // self.sector_size,
                                  ds2cs_bits)
        offsets = struct.unpack_from(f'<{files}I', data, pos)
        pos += 4 * files
        self.names = {}
        for i, offset in enumerate(offsets):
            end = data.index(b'\0', pos + offset)
            self.names[data[pos + offset:end].decode('latin-1')] = i

    def close(self):
        self.file.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _sector(self, index):
        self.file.seek(self.first + index * self.sector_size)
        blob = self.file.read(self.sector_size)
        out = bytearray()
        pos = 0
        while pos + 6 <= len(blob):
            low, high, stored = struct.unpack_from('<3H', blob, pos)
            if stored == 0:
                break
            if stored & 0x8000:
                raise ValueError('encrypted chunk')
            size = low | high << 16
            chunk = zlib.decompressobj().decompress(blob[pos + 6:pos + 6 + (stored & 0x7FFF)])
            out += chunk.ljust(size, b'\0')[:size] if size else chunk
            pos += 6 + (stored & 0x7FFF)
        return out

    def _read(self, offset, size):
        sector = self.ds2cs[offset // self.sector_size]
        while sector > 0 and self.cs2do[sector] > offset:
            sector -= 1
        start = self.cs2do[sector]
        buf = bytearray()
        while start + len(buf) < offset + size:
            buf += self._sector(sector)
            sector += 1
            if sector < len(self.cs2do):  # the table decides where the next sector starts
                gap = self.cs2do[sector] - (start + len(buf))
                if gap > 0:
                    buf += b'\0' * gap
                elif gap < 0:
                    del buf[gap:]
        return bytes(buf[offset - start:offset - start + size])

    def read(self, name):
        """The file's bytes; KeyError when the archive has no such name."""
        h, size, index = self.entries[self.names[name]]
        if h != fnv1a64(name):
            raise ValueError(f'{name}: hash mismatch')
        return self._read(self.locations[index], size)
