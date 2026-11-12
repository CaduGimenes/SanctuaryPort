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
                              '2560x1080', '3440x1440', '3840x1600', '5120x1440')}
        self.assertEqual(names, expected)
        for meta in ET.parse(xml).getroot().iter('Metadata'):
            size = meta.get('Name').split(' - ')[1]
            width, height = graphics_options.resolution_size(size)
            lines = list(meta.iter('Line'))
            values = [int.from_bytes(bytes.fromhex(line.get('Value')), 'little') for line in lines[:2]]
            self.assertEqual(values, [width, height], size)
            if graphics_options.aspect_of(size) == '16:9':
                self.assertEqual(len(lines), 2, size)
            else:
                self.assertEqual(len(lines), 3, size)
                aspect = struct.unpack('<f', bytes.fromhex(lines[2].get('Value')))[0]
                self.assertAlmostEqual(aspect, width / height, places=5, msg=size)

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
