import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))

import gamepad  # noqa: E402
import game_profiles  # noqa: E402
import graphics_options  # noqa: E402
import patches  # noqa: E402


class Diablo3PatchTests(unittest.TestCase):
    """patches/Diablo_III.xml: one resolution patch per 16:9 size, two writes each (width, height)."""

    def test_a_patch_per_built_in_16_9_size(self):
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        names = patches.available_patches(xml, '01.47')
        expected = {f'{graphics_options.RESOLUTION_PREFIX} - {n}'
                    for n in ('480p', '720p', '900p', '1440p', '1800p', '4K')}
        self.assertEqual(names, expected)
        for meta in ET.parse(xml).getroot().iter('Metadata'):
            size = meta.get('Name').split(' - ')[1]
            width, height = graphics_options.resolution_size(size)
            lines = list(meta.iter('Line'))
            self.assertEqual(len(lines), 2)
            values = [int.from_bytes(bytes.fromhex(line.get('Value')), 'little') for line in lines]
            self.assertEqual(values, [width, height], size)

    def test_both_releases_use_it(self):
        for title in ('CUSA00242', 'CUSA00243'):
            profile = patches.game_profile(title)
            self.assertEqual(profile[1].name, 'Diablo_III.xml')


class Diablo3AddonTests(unittest.TestCase):
    """The Diablo profile reports the known entitlement-only add-ons as installed."""

    def test_diablo_profile_advertises_all_addons(self):
        self.assertEqual(game_profiles.DIABLO3['env'].get('SANCTUARY_ADDCONT_ALL'), '1')

    def test_known_addon_labels_are_16_characters(self):
        self.assertTrue(game_profiles.DIABLO3_ADDONS)
        for label in game_profiles.DIABLO3_ADDONS:
            self.assertEqual(len(label), 16)


class GamepadTests(unittest.TestCase):
    def test_auto_follows_the_connected_gamepad(self):
        xbox = 'Gamepad: type=xbox360 south=a east=b west=x north=y name=Xbox Controller\n'
        self.assertEqual(gamepad.resolve('auto', xbox)[0], 'xbox')
        ds = 'Gamepad: type=ps5 south=cross east=circle west=square north=triangle name=DualSense\n'
        self.assertEqual(gamepad.resolve('auto', ds)[0], 'playstation')
        self.assertEqual(gamepad.resolve('auto', 'Gamepad: none\n')[0], 'keyboard')
        self.assertEqual(gamepad.resolve('xbox', '')[0], 'xbox')
        self.assertEqual(gamepad.resolve('off', '')[0], 'playstation')


if __name__ == '__main__':
    unittest.main()
