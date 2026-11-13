import sys
import tempfile
import struct
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))

import game_profiles  # noqa: E402
import gamepad  # noqa: E402
import graphics_options  # noqa: E402
import patches  # noqa: E402


class Diablo3PatchTests(unittest.TestCase):
    """patches/Diablo_III.xml: one resolution patch per size: width and height, and for the wider
    sizes the camera's aspect ratio (float width / height)."""

    def test_a_patch_per_built_in_size(self):
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        names = patches.available_patches(xml, '01.47')
        expected = {f'{graphics_options.RESOLUTION_PREFIX} - {n}'
                    for n in ('480p', '720p', '900p', '1440p', '1800p', '4K',
                              '2560x1080', '3440x1440', '3840x1600', '5120x1440',
                              '1280x800', '1920x1200', '2560x1600')}
        self.assertEqual(names, expected)
        for meta in ET.parse(xml).getroot().iter('Metadata'):
            size = meta.get('Name').split(' - ')[1]
            width, height = graphics_options.resolution_size(size)
            # US 1.00 entries also grow the display buffers' heap (32 -> 224 MiB) above 900p.
            heap = [line for line in meta.iter('Line') if line.get('Value') == '0000000e']
            # Below 1080 lines the game has no fonts of its own size: 4 jumps make it use the 1080p ones.
            fonts = [line for line in meta.iter('Line') if line.get('Value') == 'eb']
            self.assertEqual(len(fonts), 4 if height < 1080 else 0, size)
            for line in fonts:
                self.assertIn('3d 37 04 00 00 77', line.get('Pattern'))
                self.assertEqual(line.get('Pattern').split()[int(line.get('At'))], '77')
            lines = [line for line in meta.iter('Line') if line not in heap and line not in fonts]
            if heap:
                self.assertEqual(len(heap), 3, size)
                self.assertIn('DMEM to 256 MB', meta.get('Note'))
            values = [int.from_bytes(bytes.fromhex(line.get('Value')), 'little') for line in lines[:2]]
            self.assertEqual(values, [width, height], size)
            if graphics_options.aspect_of(size) == '16:9':
                self.assertEqual(len(lines), 2, size)
            else:
                self.assertEqual(len(lines), 3, size)
                aspect = struct.unpack('<f', bytes.fromhex(lines[2].get('Value')))[0]
                self.assertAlmostEqual(aspect, width / height, places=5, msg=size)

    def test_us_1_00_gets_its_own_sites(self):
        # GitHub #16: the 1.47 signatures are not in the US 1.00 eboot; its own entry is used.
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        size_code = bytes.fromhex('41c74648800700004 1c7464c3804000041c74650398ee33f'.replace(' ', ''))
        heap_code = (bytes.fromhex('ba00000002b90000010041b80300000049 89d9'.replace(' ', '')) + bytes(16)
                     + bytes.fromhex('be00000002ba3300000031c9') + bytes(16)
                     + bytes.fromhex('b90000000241b8001000004531c9'))
        # The four font-size choices (below 1080 lines -> the 1080p fonts).
        font_code = b''.join(bytes.fromhex(h) + bytes(8) for h in (
            'c43d37040000771b', '3d37040000771d', '850200bb080000008b45d03d37040000771b',
            '840200bb080000008b45d03d37040000771b'))
        image = (bytearray(0x1000) + size_code + bytearray(0x100) + heap_code + bytearray(0x100)
                 + font_code)
        segments = [(0, len(image))]
        writes = patches.compile_patches(xml, ['Resolution Patch - 1440p'], '01.47', segments,
                                         image=bytes(image))
        at = 0x1000
        self.assertIn((at + 4, (2560).to_bytes(4, 'little')), writes)
        self.assertIn((at + 12, (1440).to_bytes(4, 'little')), writes)
        self.assertEqual(sum(data == bytes.fromhex('0000000e') for _, data in writes), 3)
        # 720p fits the 1.00 heap: sizes, and the jumps to the 1080p fonts.
        small = patches.compile_patches(xml, ['Resolution Patch - 720p'], '01.47', segments,
                                        image=bytes(image))
        self.assertEqual(len(small), 6)
        self.assertEqual(sum(data == b'\xeb' for _, data in small), 4)
        # The 1.47 code still gets the 1.47 entry.
        new = bytearray(0x800) + bytes.fromhex('b8000f0000b9800700000f45c841894f4c') + bytearray(0x40) \
            + bytes.fromhex('b870080000b93804000049 8dbf100d00000f45c841894f50'.replace(' ', '')) + bytearray(0x800)
        writes = patches.compile_patches(xml, ['Resolution Patch - 1440p'], '01.47', [(0, len(new))],
                                         image=bytes(new))
        self.assertEqual(writes, [(0x800 + 6, (2560).to_bytes(4, 'little')),
                                  (0x800 + 17 + 0x40 + 6, (1440).to_bytes(4, 'little'))])

    def test_both_releases_use_it(self):
        for title in ('CUSA00242', 'CUSA00243'):
            profile = patches.game_profile(title)
            self.assertEqual(profile[1].name, 'Diablo_III.xml')


class GamepadTests(unittest.TestCase):
    def test_auto_follows_the_connected_gamepad(self):
        xbox = 'Gamepad: type=xbox360 south=a east=b west=x north=y name=Xbox Controller\n'
        self.assertEqual(gamepad.resolve('auto', xbox)[0], 'xbox')
        ds = 'Gamepad: type=ps5 south=cross east=circle west=square north=triangle name=DualSense\n'
        self.assertEqual(gamepad.resolve('auto', ds)[0], 'playstation')
        self.assertEqual(gamepad.resolve('auto', 'Gamepad: none\n')[0], 'keyboard')
        self.assertEqual(gamepad.resolve('xbox', '')[0], 'xbox')
        self.assertEqual(gamepad.resolve('off', '')[0], 'playstation')


class InlineAddonTests(unittest.TestCase):
    def test_a_dump_with_add_on_folders_inside_is_found(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'NECROMANCER00003' / 'sce_sys').mkdir(parents=True)
            self.assertTrue(game_profiles.inline_addons(folder))

    def test_the_games_own_folders_are_not_add_ons(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'sce_sys').mkdir()
            (Path(folder) / 'sce_module').mkdir()
            self.assertFalse(game_profiles.inline_addons(folder))

    def test_a_16_character_folder_without_sce_sys_is_not_one(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'NECROMANCER00003').mkdir()
            self.assertFalse(game_profiles.inline_addons(folder))


if __name__ == '__main__':
    unittest.main()
