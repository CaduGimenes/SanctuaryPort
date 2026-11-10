"""Extract a PS4 game from the decrypted outer PFS image of its (fake) PKG.

LibOrbisPkg 0.2 (PkgTool) has two bugs that corrupt game files when it runs on .NET 6 or newer:
  * PFSCReader inflates each 64 KiB sector with one DeflateStream.Read, which may now return a
    partial sector, so most compressed sectors end in zeros (pkg_extract);
  * pkg_extractinnerpfs reads only 7 of every 8 MiB and shares one decryptor between threads.
`PkgTool pkg_extractouterpfs --passcode 000...0` decrypts sequentially and is correct; this tool
takes its output, finds uroot/pfs_image.dat, inflates every PFSC sector fully and writes the files.

usage: pfsc_extract.py outer_pfs.dat OUT_DIR [--verify EXISTING_DIR] [--only SUBSTR] [--list]
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

PFSC_MAGIC = 0x43534650
PFS_MAGIC = 20130315


class FileReader:
    def __init__(self, path):
        self.f = open(path, 'rb')

    def read(self, offset, size):
        self.f.seek(offset)
        data = self.f.read(size)
        if len(data) != size:
            raise ValueError(f'short read at {offset:#x}')
        return data


class BlockReader:
    """A file stored in PFS blocks (possibly non-contiguous), read by byte offset."""

    def __init__(self, base, block_size, blocks, size):
        self.base, self.bs, self.blocks, self.size = base, block_size, blocks, size

    def read(self, offset, size):
        out = bytearray()
        while size > 0:
            index, inside = divmod(offset, self.bs)
            n = min(size, self.bs - inside)
            out += self.base.read(self.blocks[index] * self.bs + inside, n)
            offset += n
            size -= n
        return bytes(out)


class Pfsc:
    """Random access to the uncompressed contents of a PFSC file."""

    def __init__(self, base):
        self.base = base
        magic, unk4, _unk8, block, block2, offsets, _data_start, length = struct.unpack('<iiiiqqQq', base.read(0, 0x30))
        if magic != PFSC_MAGIC or unk4 != 0 or block != block2:
            raise ValueError('not a PFSC image')
        self.block, self.length = block, length
        count = length // block
        self.map = struct.unpack(f'<{count + 1}q', base.read(offsets, 8 * (count + 1)))
        self.cache_index, self.cache = -1, b''
        self.stats = {'raw': 0, 'deflate': 0, 'zero': 0}

    def sector(self, index):
        if index == self.cache_index:
            return self.cache
        start, end = self.map[index], self.map[index + 1]
        size = end - start
        if size == self.block:
            data = self.base.read(start, self.block)
            self.stats['raw'] += 1
        elif size > self.block:
            data = bytes(self.block)
            self.stats['zero'] += 1
        else:
            inflater = zlib.decompressobj(-15)          # raw deflate after the 2-byte zlib header
            data = inflater.decompress(self.base.read(start + 2, size - 2), self.block)
            if len(data) < self.block:
                data += inflater.flush()
            if len(data) != self.block:
                raise ValueError(f'sector {index}: inflated {len(data)} of {self.block} bytes')
            self.stats['deflate'] += 1
        self.cache_index, self.cache = index, data
        return data

    def read(self, offset, size):
        out = bytearray()
        while size > 0:
            index, inside = divmod(offset, self.block)
            chunk = self.sector(index)[inside:inside + size]
            out += chunk
            offset += len(chunk)
            size -= len(chunk)
        return bytes(out)


class Pfs:
    def __init__(self, image):
        self.img = image
        hdr = image.read(0, 0x400)
        version, magic = struct.unpack_from('<qq', hdr, 0)
        if version != 1 or magic != PFS_MAGIC:
            raise ValueError('not a PFS image')
        (self.mode,) = struct.unpack_from('<H', hdr, 0x1c)
        (self.bs,) = struct.unpack_from('<I', hdr, 0x20)
        self.dinode_count, _ndblock, self.dinode_blocks = struct.unpack_from('<qqq', hdr, 0x30)
        self.signed = bool(self.mode & 1)
        size = 0x2C8 if self.signed else 0xA8
        self.inodes = []
        for i in range(self.dinode_blocks):
            data = image.read(self.bs * (1 + i), self.bs)
            for j in range(self.bs // size):
                if len(self.inodes) == self.dinode_count:
                    break
                self.inodes.append(self._inode(data, j * size))

    def _inode(self, data, o):
        size, csize = struct.unpack_from('<qq', data, o + 8)
        (blocks,) = struct.unpack_from('<I', data, o + 0x60)
        if self.signed:
            db = [struct.unpack_from('<i', data, o + 0x64 + 36 * i + 32)[0] for i in range(12)]
            ib = [struct.unpack_from('<i', data, o + 0x214 + 36 * i + 32)[0] for i in range(5)]
        else:
            db = list(struct.unpack_from('<12i', data, o + 0x64))
            ib = list(struct.unpack_from('<5i', data, o + 0x94))
        return {'size': size, 'csize': csize, 'blocks': blocks, 'db': db, 'ib': ib}

    def block_list(self, node):
        """All data blocks of a file (PfsReader.LoadFile)."""
        n = node['blocks']
        if n <= 1 or node['db'][1] == -1:
            return [node['db'][0] + i for i in range(n)]
        if not self.signed:
            raise ValueError('unsigned PFS with non-contiguous blocks')
        blocks = node['db'][:min(12, n)]
        per = self.bs // 36

        def entry(block, i):
            return struct.unpack('<i', self.img.read(block * self.bs + i * 36 + 32, 4))[0]
        remaining = n - 12
        for i in range(min(remaining, per)):
            blocks.append(entry(node['ib'][0], i))
        remaining -= per
        j = 0
        while j * per < remaining:
            table = entry(node['ib'][1], j)
            for i in range(min(per, remaining - j * per)):
                blocks.append(entry(table, i))
            j += 1
        return blocks

    def walk(self, ino=0, path=''):
        node = self.inodes[ino]
        subdirs = []
        start = node['db'][0]
        for b in range(start, start + node['blocks']):
            data = self.img.read(self.bs * b, self.bs)
            o = 0
            while o < self.bs:
                number, kind, name_len, ent = struct.unpack_from('<Iiii', data, o)
                if ent == 0:
                    break
                name = data[o + 16:o + 16 + name_len].decode('ascii')
                if kind == 2:
                    yield path + '/' + name, self.inodes[number]
                elif kind == 3:
                    subdirs.append((number, path + '/' + name))
                o += ent
        for number, sub in subdirs:
            yield from self.walk(number, sub)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('outer', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--verify', type=Path, help='earlier extraction to compare with')
    parser.add_argument('--only', default='', help='only files whose path contains this text')
    parser.add_argument('--list', action='store_true', help='list files, write nothing')
    args = parser.parse_args()

    base = FileReader(args.outer)
    outer = Pfs(base)
    image = next(node for path, node in outer.walk() if path == '/uroot/pfs_image.dat')
    pfsc = Pfsc(BlockReader(base, outer.bs, outer.block_list(image), image['size']))
    inner = Pfs(pfsc)
    print(f'outer: block {outer.bs}, signed={outer.signed}; inner: {len(pfsc.map) - 1} PFSC sectors, '
          f'{inner.dinode_count} inodes', flush=True)
    total = 0
    for path, node in inner.walk():
        if not path.startswith('/uroot/') or args.only not in path:
            continue
        relative = path[len('/uroot/'):]
        size = node['size']
        if args.list:
            print(f'{relative}: {size}')
            continue
        reader = BlockReader(pfsc, inner.bs, inner.block_list(node), size)
        target = args.out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        old = open(args.verify / relative, 'rb') if args.verify and (args.verify / relative).is_file() else None
        differ = done = 0
        with open(target, 'wb') as out:
            while done < size:
                n = min(0x10000, size - done)
                data = reader.read(done, n)
                out.write(data)
                if old is not None and old.read(n) != data:
                    differ += 1
                done += n
        if old is not None:
            old.close()
        total += size
        note = f', {differ} of {(size + 0xffff) // 0x10000} sectors differ' if old is not None else ''
        print(f'{relative}: {size} bytes{note}', flush=True)
    print(f'done: {total} bytes; PFSC sectors {pfsc.stats}')


if __name__ == '__main__':
    sys.exit(main())
