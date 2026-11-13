from paths import ROOT
import os
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

import d3_cpk
import d3_glyphs

GAME = Path(os.environ.get('SANCTUARY_GAME_DIR', Path.home() / 'Games/PS4/CUSA00243'))


def tile(linear, width, height, element):
    """The inverse of d3_glyphs.detile (rows to the tiled order)."""
    out = bytearray(len(linear))
    for y in range(height):
        for x in range(width):
            src = ((y >> 3) * (width // 8) + (x >> 3)) * 64 + d3_glyphs._morton(x & 7, y & 7)
            dst = y * width + x
            out[src * element:(src + 1) * element] = linear[dst * element:(dst + 1) * element]
    return bytes(out)


class Unpack(unittest.TestCase):
    def test_msb_first_fields(self):
        # 3-bit fields 5, 2, 7, 1 -> 101 010 111 001 (+ padding)
        values, end = d3_cpk._unpack(bytes([0b10101011, 0b10010000]), 0, 4, 3)
        self.assertEqual(values, [5, 2, 7, 1])
        self.assertEqual(end, 2)

    def test_fnv1a64_lower_case(self):
        self.assertEqual(d3_cpk.fnv1a64(''), 0xCBF29CE484222325)
        self.assertEqual(d3_cpk.fnv1a64('Textures\\A.tex'), d3_cpk.fnv1a64('textures\\a.tex'))


class Glyphs(unittest.TestCase):
    def test_detile_inverts_the_tiled_order(self):
        linear = bytes(i % 251 for i in range(16 * 16 * 2))
        self.assertEqual(d3_glyphs.detile(tile(linear, 16, 16, 2), 16, 16, 2), linear)

    def test_bc3_block_endpoints(self):
        # alpha 255/0, indices 0 and 1; colour 0xF800 (red) / 0x001F (blue), indices 0 and 1
        block = bytes([255, 0]) + (0b001000).to_bytes(6, 'little') + struct.pack('<2H', 0xF800, 0x001F) \
            + (0b0100).to_bytes(4, 'little')
        texels = d3_glyphs._bc3_block(block)
        self.assertEqual(texels[0], (255, 0, 0, 255))
        self.assertEqual(texels[1], (0, 0, 255, 0))

    def test_cells_centre_the_icon_and_clear_the_rest(self):
        # An 8x2-block Xbox atlas whose blocks hold their own index.
        xb1 = b''.join(bytes([i]) * 16 for i in range(16))
        saved = d3_glyphs.ATLAS_CELLS
        try:
            d3_glyphs.ATLAS_CELLS = [((0, 0, 16, 8), (6, 2, 9, 5))]  # icon in blocks 1..2 x 0..1 (+2 room)
            (x, y, w, h, data), = d3_glyphs.atlas_cells(xb1, 8)
        finally:
            d3_glyphs.ATLAS_CELLS = saved
        self.assertEqual((x, y, w, h), (0, 0, 16, 8))
        blocks = [data[i * 16] for i in range(8)]
        self.assertEqual(blocks, [0, 1, 2, 0, 0, 9, 10, 0])

    def test_bc3_encoder_round_trip(self):
        texels = [(200, 150, 90, 255)] * 8 + [(20, 10, 8, 255)] * 8
        decoded = d3_glyphs._bc3_block(d3_glyphs.encode_bc3_block(texels))
        for got, want in zip(decoded, texels):
            self.assertLessEqual(max(abs(g - w) for g, w in zip(got, want)), 8)

    def test_spg2_names_its_texture_by_crc32(self):
        data = d3_glyphs.spg(4, 4, d3_glyphs.BC3, b'guest', [(0, 0, 4, 4, b'\0' * 16)])
        magic, w, h, fmt, crc, count = struct.unpack_from('<4sIIIQI', data)
        self.assertEqual((magic, w, h, fmt, crc, count), (b'SPG2', 4, 4, 137, zlib.crc32(b'guest'), 1))
        self.assertEqual(len(data), 28 + 20 + 16)

    def test_no_archive_no_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(d3_glyphs.build(tmp, Path(tmp) / 'out'), [])

    @unittest.skipUnless((GAME / 'CPKs' / 'ClientCommon.cpk').is_file(), 'needs the Diablo III dump')
    def test_from_the_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            files = d3_glyphs.build(GAME, Path(tmp))
            self.assertEqual([f.name for f in files], ['d3_xb1_atlas.spg', 'd3_xb1_controller.spg', 'd3_xb1_prompt.spg'])
            atlas = files[0].read_bytes()
            self.assertEqual(atlas[:4], b'SPG2')
            self.assertEqual(struct.unpack_from('<IIII', atlas, 4)[:3], (256, 1024, 137))
            self.assertEqual(struct.unpack_from('<I', atlas, 24)[0], len(d3_glyphs.ATLAS_CELLS))
            self.assertEqual(d3_glyphs.build(GAME, Path(tmp)), files)  # cached


if __name__ == '__main__':
    unittest.main()
