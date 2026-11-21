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

    SEASONS = [f'Offline Seasons - Season {n}' for n in range(14, 41)]
    EVENTS = [f'Event: {n}' for n in (
        'Double Treasure Goblins', 'Double Bounty Bags', 'Royal Grandeur', 'Legacy of Nightmares', "Triune's Will",
        'Pandemonium', 'Kanai Powers', 'Trials of Tempests', 'Shadow Clones', "Fourth Kanai's Cube Slot",
        'Ethereal Items', 'Soul Shards', 'Swarm Rifts', 'Sanctified Items', 'Whimsyshire (Easter Egg World)',
        'Also in non-seasonal games', 'Challenge Rifts', 'The Darkening of Tristram')]
    OFFLINE_NAMES = {'Offline Seasons', 'Offline: no sign-in warning', *SEASONS, *EVENTS}
    SMOOTH = 'Experimental: Smooth character motion above 60 FPS'
    # 1.47 movement interpolation (0x256fc0): the original bytes around its read of the client time.
    SMOOTH_SITE = bytes.fromhex('c5f2589338020000 c5f258b33c020000 0fb793d8010000 89d6 c5fa104060'
                                '488b80780a0000 c5ea5cd0 c5ca5cc0')

    def test_smooth_motion_patch_is_off_and_found_by_signature(self):
        # GitHub #24: experimental, never on by default, and skipped (not misapplied) on another eboot.
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        root = ET.parse(xml).getroot()
        (meta,) = [m for m in root.iter('Metadata') if m.get('Name') == self.SMOOTH]
        self.assertEqual(meta.get('isEnabled'), 'false')
        self.assertEqual(meta.get('AppVer'), '01.47')
        self.assertNotIn(self.SMOOTH, patches.selected_patches(xml, '01.47', ''))
        targets = {t.get('Name') for t in meta.iter('Target')}
        for target in meta.iter('Target'):
            self.assertTrue(target.get('Pattern'), target.get('Name'))
        for line in meta.iter('Line'):
            self.assertTrue(line.get('Pattern') or line.get('Target') in targets, line.get('Address'))
        # A small image: the site, and the zero padding that ends the code (the offline seasons' anchor).
        anchor = bytes.fromhex(next(t for t in meta.iter('Target')).get('Pattern').replace(' ', ''))
        site_at, anchor_at = 0x1000, 0x3000
        image = bytearray(0x6000)
        image[site_at:site_at + len(self.SMOOTH_SITE)] = self.SMOOTH_SITE
        image[anchor_at:anchor_at + 24] = anchor[:24]
        writes = dict(patches.compile_patches(xml, [self.SMOOTH], '01.47', [(0, len(image))], image=bytes(image)))
        cave = anchor_at + 4236
        call = site_at + 25
        self.assertEqual(set(writes), {cave, call})
        self.assertEqual(writes[call][:1], b'\xe8')
        self.assertEqual(call + 5 + struct.unpack('<i', writes[call][1:])[0], cave)
        code = writes[cave]
        # (W+0x54 ticks * 50 + accumulated W+0x38 - simulated W+0x40) / 3000.0, rcx saved, xmm0 out.
        self.assertEqual(code[:1] + code[-6:-4], bytes.fromhex('5159c3'))
        self.assertEqual(code[1:13], bytes.fromhex('8b48546bc9320348382b4840'))
        disp = struct.unpack('<i', code[21:25])[0]
        self.assertEqual(struct.unpack('<f', code[25 + disp:29 + disp])[0], 3000.0)
        # The game's own bytes are untouched without the patch, and another build is skipped whole.
        skipped = {}
        self.assertEqual(patches.compile_patches(xml, [self.SMOOTH], '01.47', [(0, 0x6000)],
                                                 image=bytes(0x6000), skipped=skipped), [])
        self.assertIn(self.SMOOTH, skipped)

    def test_offline_patches_find_every_site_by_signature(self):
        # A different eboot is skipped, never overwritten: every Line has a signature, or writes at a
        # Target that has one; the season entries are the shared base plus their own values.
        root = ET.parse(ROOT / 'patches' / 'Diablo_III.xml').getroot()
        metas = {m.get('Name'): m for m in root.iter('Metadata') if m.get('Name') in self.OFFLINE_NAMES}
        self.assertEqual(set(metas), self.OFFLINE_NAMES)
        for name, meta in metas.items():
            own = patches.with_base(meta, list(root.iter('Metadata')))
            targets = {t.get('Name') for t in own.iter('Target')}
            for target in own.iter('Target'):
                self.assertTrue(target.get('Pattern'), (name, target.get('Name')))
            for line in own.iter('Line'):
                self.assertTrue(line.get('Pattern') or line.get('Target') in targets, (name, line.get('Address')))
            self.assertNotIn('Claude', meta.get('Author'))
            self.assertNotIn('Local mod', meta.get('Note'))
        base = metas['Offline Seasons']
        for name in self.SEASONS:
            self.assertEqual(metas[name].get('Base'), 'Offline Seasons')
            self.assertLess(len(ET.tostring(metas[name])), len(ET.tostring(base)) // 4, name)

    def test_a_season_is_the_base_with_its_number_and_theme(self):
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        # A tiny image whose zero padding is found through the base's anchor (the 24 bytes before it).
        base = ET.parse(xml).getroot().find("Metadata[@Name='Offline Seasons']")
        anchor = bytes.fromhex(next(base.iter('Target')).get('Pattern').replace(' ', '').replace('??', '00'))
        self.assertEqual(anchor[24:], bytes(32))
        merged = patches.with_base(
            ET.parse(xml).getroot().find("Metadata[@Name='Offline Seasons - Season 22']"),
            list(ET.parse(xml).getroot().iter('Metadata')))
        lines = list(merged.iter('Line'))
        base_lines = list(base.iter('Line'))
        self.assertEqual(len(lines), len(base_lines) + 2)
        self.assertEqual([ET.tostring(l) for l in lines[:len(base_lines)]], [ET.tostring(l) for l in base_lines])
        number, theme = lines[-2:]
        self.assertEqual(bytes.fromhex(number.get('Value')), b'22')
        # Season 22: Shadow Clones (8) and Fourth Kanai's Cube Slot (9).
        self.assertEqual([i for i, b in enumerate(bytes.fromhex(theme.get('Value'))) if b], [8, 9])

    def test_a_patch_per_built_in_size(self):
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        names = patches.available_patches(xml, '01.47')
        expected = {f'{graphics_options.RESOLUTION_PREFIX} - {n}'
                    for n in ('480p', '720p', '900p', '1440p', '1800p', '4K',
                              '2560x1080', '3440x1440', '3840x1600', '5120x1440',
                              '1280x800', '1920x1200', '2560x1600')}
        expected |= {f'{graphics_options.LIMITER_PREFIX} - {n}' for n in ('30 FPS', '120 FPS', 'Unlimited')}
        expected.add(f'{graphics_options.MSAA_PREFIX} - 4x')
        expected |= self.OFFLINE_NAMES | {self.SMOOTH, graphics_options.FIXED_SCALE_PATCH,
                                          graphics_options.FOREGROUND_CAP_PATCH}
        self.assertEqual(names, expected)
        for meta in ET.parse(xml).getroot().iter('Metadata'):
            if (meta.get('Name') in self.OFFLINE_NAMES | {self.SMOOTH, graphics_options.FIXED_SCALE_PATCH,
                                                          graphics_options.FOREGROUND_CAP_PATCH}
                    or meta.get('Name').startswith(graphics_options.MSAA_PREFIX)):
                continue
            if meta.get('Name').startswith(graphics_options.LIMITER_PREFIX):
                # The frame limiter: one microsecond wait, 16683 (0x412b) in the pattern.
                for line in meta.iter('Line'):
                    pattern = line.get('Pattern').split()
                    at = int(line.get('At'))
                    self.assertEqual(pattern[at:at + 4], ['2b', '41', '00', '00'])
                continue
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

    def test_the_fps_cap_patch_asks_the_second_limiter_for_200(self):
        # The game's second limiter waits (n + 2000) / (2 * n) whole ms for the setting n (150: 7 ms,
        # about 143 FPS). The patch passes 200 in place of the setting it reads.
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        name = graphics_options.FOREGROUND_CAP_PATCH
        meta = ET.parse(xml).getroot().find(f"Metadata[@Name='{name}']")
        line, = meta.iter('Line')
        target, = meta.iter('Target')
        value, at = bytes.fromhex(line.get('Value')), int(line.get('At'))
        # mov eax, imm32 with the rate the entry is named after, in place of a call of the same
        # length (the read of the setting), between the test of the limiter's switch and the call
        # of the limiter with eax as its second argument.
        pattern = line.get('Pattern').split()
        self.assertEqual(pattern[at:at + len(value)], ['e8', '??', '??', '??', '??'])
        self.assertEqual(value[0], 0xb8)
        self.assertTrue(name.endswith(f" - {int.from_bytes(value[1:], 'little')}"))
        self.assertEqual(pattern[at - 12:at - 8], ['85', 'c0', '74', '??'])
        self.assertEqual(pattern[at + 12:at + 15], ['89', 'c6', 'e8'])
        # One entry for every build checked; they differ in the je distance and in the order of the
        # limiter's clamp of n to at least 1 (test ebx, ebx; mov ecx, 1) before the guarded arithmetic.
        layouts = {'1.46, 1.47, 1.25': (0x1b, '85 db b9 01 00 00 00'), '1.00': (0x21, 'b9 01 00 00 00 85 db')}
        for build, (skip, clamp) in layouts.items():
            site = bytearray(bytes.fromhex(' '.join('5a' if b == '??' else b for b in pattern)))
            site[at - 9] = skip
            limiter = bytes.fromhex(clamp + ' ' + target.get('Pattern'))
            image = bytearray(0x400)
            image[0x100:0x100 + len(site)] = site
            image[0x200:0x200 + len(limiter)] = limiter
            skipped = {}
            writes = patches.compile_patches(xml, [name], None, [(0, 0x400)], bytes(image), skipped)
            self.assertEqual((writes, skipped), ([(0x100 + at, value)], {}), build)
            # Another build: the call site differs, or the limiter does not compute this limit.
            for changed in (0x100 + at - 10, 0x200 + 7 + 4):
                other = bytearray(image)
                other[changed] ^= 0xff
                skipped = {}
                self.assertEqual(patches.compile_patches(xml, [name], None, [(0, 0x400)], bytes(other), skipped), [])
                self.assertEqual(skipped, {name: 'signature not found'}, build)

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

    def test_dynamic_resolution_off_takes_the_fixed_scale_branch(self):
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        line, = ET.parse(xml).getroot().find(f"Metadata[@Name='{graphics_options.FIXED_SCALE_PATCH}']").iter('Line')
        pattern, at = line.get('Pattern').split(), int(line.get('At'))
        # The je over "scale = 1.0, countdown = 0" becomes two nops. The call before it is to a
        # function that moved between 1.46 and 1.47: its displacement is not part of the signature.
        self.assertEqual((pattern[3:8], pattern[at:at + 2]), (['e8', '??', '??', '??', '??'], ['74', '10']))
        for displacement in ('72fd5100', 'f2ff5100'):
            image = bytes(0x100) + bytes.fromhex(''.join(pattern).replace('????????', displacement)) + bytes(0x100)
            self.assertEqual(patches.compile_patches(xml, [graphics_options.FIXED_SCALE_PATCH], '01.47',
                                                     [(0, len(image))], image=image),
                             [(0x100 + at, bytes.fromhex('9090'))])

    def test_both_releases_use_it(self):
        for title in ('CUSA00242', 'CUSA00243'):
            profile = patches.game_profile(title)
            self.assertEqual(profile[1].name, 'Diablo_III.xml')

    def test_game_msaa_rewrites_the_level(self):
        # The 1.47 code around each site (eboot 0xbe420, 0xbe4fe), apart in an image.
        level_a = bytes.fromhex('c7433004000000418b4754894334')
        level_b = bytes.fromhex('c7433004000000418b4654894334')
        image = bytearray(0x100)
        sites = {}
        for name, code in (('level_a', level_a), ('level_b', level_b)):
            sites[name] = len(image)
            image += code + bytearray(0x100)
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        level16 = bytes.fromhex('c7433010000000')
        four = patches.compile_patches(xml, ['Game MSAA - 4x'], '01.47', [(0, len(image))], image=bytes(image))
        self.assertEqual(sorted(four), [(sites['level_a'], level16), (sites['level_b'], level16)])
        # Each write replaces a whole instruction of the same length: movl $4 becomes movl $16.
        for meta in ET.parse(xml).getroot().iter('Metadata'):
            if meta.get('Name').startswith(graphics_options.MSAA_PREFIX):
                for line in meta.iter('Line'):
                    at, value = int(line.get('At')), bytes.fromhex(line.get('Value'))
                    old = bytes.fromhex(''.join(line.get('Pattern').split()[at:at + len(value)]))
                    self.assertEqual(old, bytes.fromhex('c7433004000000'))
        # Another executable: no signature found, nothing written, the entry reported as skipped.
        skipped = {}
        self.assertEqual(patches.compile_patches(xml, ['Game MSAA - 4x'], '01.47', [(0, 0x1000)],
                                                 image=bytes(0x1000), skipped=skipped), [])
        self.assertIn('Game MSAA - 4x', skipped)


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


class BundledModuleTests(unittest.TestCase):
    """GitHub #28: a 1.00 eboot imports /app0/dlcldr.prx, at the root of the game folder."""

    def test_modules_are_found_in_sce_module_then_at_the_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            (game / 'sce_module').mkdir()
            (game / 'sce_module' / 'libc.prx').write_bytes(b'x')
            (game / 'dlcldr.prx').write_bytes(b'x')
            self.assertEqual(game_profiles.module_path(game, 'libc.prx'), game / 'sce_module' / 'libc.prx')
            self.assertEqual(game_profiles.module_path(game, 'dlcldr.prx'), game / 'dlcldr.prx')
            self.assertIsNone(game_profiles.module_path(game, 'libSceNpToolkit2.prx'))

    def test_diablo_links_the_dlc_loader_when_the_dump_has_it(self):
        self.assertIn('dlcldr.prx', game_profiles.DIABLO3['modules'])
