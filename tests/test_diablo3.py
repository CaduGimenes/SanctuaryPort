import os
import sys
import tempfile
import struct
import unittest
from unittest import mock
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
        'Also in non-seasonal games', 'Challenge Rifts', 'The Darkening of Tristram',
        'Double Rift Keystones', 'Double Blood Shards')]
    OFFLINE_NAMES = {'Offline Seasons', 'Offline: no sign-in warning', *SEASONS, *EVENTS}
    SMOOTH = 'Experimental: Smooth character motion above 60 FPS'
    CHEATS = {'Cheat: Movement speed +25%', 'Cheat: Auto pickup', "Cheat: Kanai's Cube keeps materials",
              'Cheat: Unlock all difficulties'}
    MYSTIC = 'Mystic only from Act V (story order)'
    # 1.47 native of the Lua QuestRangeValid (0xa83f10): the QuestRange SNO looked up by name, then -1 checked.
    MYSTIC_SITE = bytes.fromhex('554889e55350 4889f8 bf3a000000 4889c6 e84affdbff 89c3 83fbff 7438 beffffffff')
    # 1.47 movement interpolation (0x256fc0): the original bytes around its read of the client time.
    SMOOTH_SITE = bytes.fromhex('c5f2589338020000 c5f258b33c020000 0fb793d8010000 89d6 c5fa104060'
                                '488b80780a0000 c5ea5cd0 c5ca5cc0')

    def test_extra_events_go_through_the_second_list(self):
        # The cave's own list holds 16 events; Double Rift Keystones and Double Blood Shards set a flag
        # of a second list (seasons_cave+0x4d0) that a stub (+0x4d8) appends, entered from the cave at
        # +0x85 in place of its lea rax,[rsp+0x60], and returning after it (+0x8a).
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        caves = {}
        writes = self.image_of(patches.compile_patches(
            xml, ['Offline Seasons - Season 30', 'Event: Double Rift Keystones', 'Event: Double Blood Shards'],
            '01.47', [(0, 0x1800000)], caves=caves))
        cave = caves['seasons_cave']
        hook = writes[cave + 0x85:cave + 0x8a]
        self.assertEqual(hook[:1], b'\xe9')
        self.assertEqual(cave + 0x8a + int.from_bytes(hook[1:5], 'little', signed=True), cave + 0x4d8)
        stub = writes[cave + 0x4d8:cave + 0x598]
        back = stub.index(bytes.fromhex('488d442460e9')) + 5
        self.assertEqual(0x4d8 + back + 5 + int.from_bytes(stub[back + 1:back + 5], 'little', signed=True), 0x8a)
        self.assertIn(b'CommunityBuffDoubleRiftKeystones "1"\n', stub)
        self.assertIn(b'CommunityBuffDoubleBloodShards "1"\n', stub)
        self.assertEqual(writes[cave + 0x4d0:cave + 0x4d2], b'\x01\x01')

    @staticmethod
    def image_of(writes, size=0x1800000):
        """The writes put into a zero image (eboot offsets)."""
        image = bytearray(size)
        for offset, data in writes:
            image[offset:offset + len(data)] = data
        return image

    def test_a_new_seasonal_hero_takes_the_season_number_itself(self):
        # GitHub #83: the hero digest's season_created came from the hero screen's copy (0x14bc984),
        # which only the answer to a per-player server request fills; players 2..4 got 0, a regular
        # hero. The creation (0x2f85f8) now calls a cave that reads the season number (0x8250f0) and
        # keeps rcx, the digest flags, which the next instruction uses.
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        caves = {}
        writes = dict(patches.compile_patches(xml, ['Offline Seasons - Season 30'], '01.47', [(0, 0x2000000)],
                                              caves=caves))
        base = patches.EBOOT_BASE
        site = writes[0x6f85f8 - base]
        self.assertEqual(len(site), 6)  # it replaces mov eax,[rip+disp32] whole
        self.assertEqual(site[:1] + site[5:], b'\xe8\x90')
        at = caves['season_number_cave']
        self.assertEqual(0x6f85fd - base + int.from_bytes(site[1:5], 'little', signed=True), at)
        cave = writes[at]
        self.assertEqual(cave[:2] + cave[6:], b'\x51\xe8\x59\xc3')  # push rcx; call; pop rcx; ret
        self.assertEqual(at + 6 + int.from_bytes(cave[2:6], 'little', signed=True), 0xc250f0 - base)

    def test_smooth_motion_patch_is_on_and_found_by_signature(self):
        # GitHub #24: on by default since players confirmed it at 120 FPS and above, and skipped
        # (not misapplied) on another eboot.
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        root = ET.parse(xml).getroot()
        (meta,) = [m for m in root.iter('Metadata') if m.get('Name') == self.SMOOTH]
        self.assertEqual(meta.get('isEnabled'), 'true')
        self.assertEqual(meta.get('AppVer'), '01.47')
        self.assertIn(self.SMOOTH, patches.selected_patches(xml, '01.47', ''))
        self.assert_found_by_signature(meta, self.SMOOTH)
        # A small image: the site, then the zero padding that ends the code, where the cave goes.
        site_at, used = 0x1000, 0x3005
        image = bytearray(0x6000)
        image[site_at:site_at + len(self.SMOOTH_SITE)] = self.SMOOTH_SITE
        image[used - 1] = 0xc3
        caves = {}
        writes = dict(patches.compile_patches(xml, [self.SMOOTH], '01.47', [(0, len(image))], image=bytes(image),
                                              caves=caves))
        cave = caves['smooth_cave']
        self.assertEqual(cave, 0x3010)  # the first 16-byte boundary after the last byte in use
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

    def assert_found_by_signature(self, meta, name):
        """Every Line of the patch has a signature, or writes at a Target that has one or at a cave."""
        targets = {t.get('Name') for t in meta.iter('Target')} | {c.get('Name') for c in meta.iter('Cave')}
        for target in meta.iter('Target'):
            self.assertTrue(target.get('Pattern') or target.get('Cave'), (name, target.get('Name')))
        for line in meta.iter('Line'):
            self.assertTrue(line.get('Pattern') or line.get('Target') in targets, (name, line.get('Address')))

    def cheat(self, name):
        """The cheat's Metadata, checked to be off by default and found by signature, and its writes
        compiled at the 1.47 reference Addresses: {eboot offset: bytes}."""
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        (meta,) = [m for m in ET.parse(xml).getroot().iter('Metadata') if m.get('Name') == name]
        self.assertEqual(meta.get('isEnabled'), 'false', name)
        self.assertEqual(meta.get('AppVer'), '01.47', name)
        self.assertEqual(meta.get('Author'), 'SanctuaryPort', name)
        self.assertTrue(meta.get('Note'), name)
        self.assertNotIn(name, patches.selected_patches(xml, '01.47', ''))
        self.assert_found_by_signature(meta, name)
        # Without the patch the game's bytes stay; another build (no signature found) is skipped whole.
        skipped = {}
        self.assertEqual(patches.compile_patches(xml, [name], '01.47', [(0, 0x6000)], image=bytes(0x6000),
                                                 skipped=skipped), [])
        self.assertIn(name, skipped)
        caves = {}
        writes = {offset + patches.EBOOT_BASE: data
                  for offset, data in patches.compile_patches(xml, [name], '01.47', [(0, 0x1800000)], caves=caves)}
        self.caves = {cave: offset + patches.EBOOT_BASE for cave, offset in caves.items()}
        return writes

    def test_cheat_movement_speed(self):
        # The lea of Movement_Scalar_Total's formula text (0x74dd32) points at a copy in the code
        # padding that adds 25% for team 2 (heroes) only.
        writes = self.cheat('Cheat: Movement speed +25%')
        formula_at = self.caves['ms_formula']
        self.assertEqual(set(writes), {0xb4dd35, formula_at})
        self.assertEqual(0xb4dd39 + int.from_bytes(writes[0xb4dd35], 'little', signed=True), formula_at)
        formula = writes[formula_at]
        self.assertTrue(formula.startswith(b'Movement_Scalar_Capped_Total + Movement_Scalar_Uncapped_Bonus + '))
        self.assertTrue(formula.endswith(b'0.25 * Pin(TeamID - 1, 0, 1) * Pin(3 - TeamID, 0, 1)\0'))

    def test_challenge_rifts_number_is_the_week(self):
        # GitHub #99: the game rewards a Challenge Rift only above the last rewarded challenge number,
        # so the number the cave writes into challengerift_config.dat must grow: the week since 1970
        # from the game's clock (0x191f0 FILETIME, 0x189f0 Unix seconds), not a random one per launch.
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        caves = {}
        writes = {offset + patches.EBOOT_BASE: data for offset, data in
                  patches.compile_patches(xml, ['Event: Challenge Rifts'], '01.47', [(0, 0x1800000)], caves=caves)}
        at = caves['cr_cave'] + patches.EBOOT_BASE
        cave = writes[at]
        self.assertNotIn(b'\x0f\x31', cave)  # rdtsc
        week = cave.index(bytes.fromhex('4889c7')) - 5
        self.assertEqual(cave[week], 0xe8)
        self.assertEqual(at + week + 5 + int.from_bytes(cave[week + 1:week + 5], 'little', signed=True),
                         0x4191f0)
        self.assertEqual(cave[week + 8], 0xe8)
        self.assertEqual(at + week + 13 + int.from_bytes(cave[week + 9:week + 13], 'little', signed=True),
                         0x4189f0)
        self.assertEqual(cave[week + 13:week + 22], bytes.fromhex('31d2 b9803a0900 f7f1'))  # / 604800
        # The rip-relative leas still find the cave's strings.
        for lea, text in ((0, b'challengerift_\0'), (0x46, b'/data/pubfiles/\0')):
            to = lea + 7 + int.from_bytes(cave[lea + 3:lea + 7], 'little', signed=True)
            self.assertEqual(cave[to:to + len(text)], text)

    def test_cheat_auto_pickup(self):
        # 0x583940's item branch jumps (0x58396b) to its last test, the item's No_AutoPickup (0x583b29).
        writes = self.cheat('Cheat: Auto pickup')
        (site, value), = writes.items()
        self.assertEqual(site, 0x98396b)
        self.assertEqual(value[:1], b'\xe9')
        self.assertEqual(site + 5 + int.from_bytes(value[1:], 'little', signed=True), 0x983b29)

    def test_cheat_cube_keeps_materials(self):
        # The two currency changes (call 0x4f9f60) of the ingredient commit 0x9c1f00 become 5-byte nops.
        writes = self.cheat("Cheat: Kanai's Cube keeps materials")
        self.assertEqual(writes, {0xdc2023: bytes.fromhex('0f1f440000'), 0xdc2183: bytes.fromhex('0f1f440000')})
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        meta = ET.parse(xml).getroot().find("Metadata[@Name=\"Cheat: Kanai's Cube keeps materials\"]")
        for line in meta.iter('Line'):
            pattern, at = line.get('Pattern').split(), int(line.get('At'))
            # The call replaced, its reason 0x14 (mov ecx,0x14) and the amount (sub rsi,rax) before it.
            self.assertEqual(pattern[at:at + 5], ['e8', '??', '??', '??', '??'])
            self.assertIn('b9 14 00 00 00', line.get('Pattern'))
            self.assertEqual(pattern[at - 3:at], ['48', '29', 'c6'])

    def test_cheat_unlock_all_difficulties(self):
        # The difficulty check 0x5b8ba0 returns 1 at once (mov eax,1; ret), the menus' lock check
        # 0x496370 returns 0 (xor eax,eax; ret).
        writes = self.cheat('Cheat: Unlock all difficulties')
        self.assertEqual(writes, {0x9b8ba0: bytes.fromhex('b801000000c3'), 0x896370: bytes.fromhex('31c0c3')})
    def test_mystic_patch_is_off_and_hides_only_the_act_i_unlock(self):
        # GitHub #79: off by default; the call to the cave replaces the mov ebx,eax / cmp ebx,-1 after
        # the QuestRange lookup, and the cave turns A1_MysticUnlock's SNO into -1 (no such range:
        # false) and nothing else. The SNO is looked up by name (test_mystic_cave_runs below).
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        (meta,) = [m for m in ET.parse(xml).getroot().iter('Metadata') if m.get('Name') == self.MYSTIC]
        self.assertEqual(meta.get('isEnabled'), 'false')
        self.assertEqual(meta.get('AppVer'), '01.47')
        self.assertNotIn(self.MYSTIC, patches.selected_patches(xml, '01.47', ''))
        self.assert_found_by_signature(meta, self.MYSTIC)
        caves = {}
        writes = dict(patches.compile_patches(xml, [self.MYSTIC], '01.47', [(0, 0x1400000)], caves=caves))
        cave, call = caves['mystic_cave'], 0xe83f26 - patches.EBOOT_BASE
        self.assertEqual(set(writes), {cave, call})
        self.assertEqual(writes[call][:1], b'\xe8')
        self.assertEqual(call + 5 + struct.unpack('<i', writes[call][1:])[0], cave)
        self.assertEqual(writes[cave][63:], b'A1_MysticUnlock\0')
        self.assertNotIn(struct.pack('<I', 430609), writes[cave])
        # Its cached SNO is a variable in the data region.
        lo, hi = patches.REFERENCE_CAVE_REGIONS['data']
        self.assertTrue(lo <= caves['mystic_sno'] < hi)
        # Found by its signature in a small image, the cave in its zero end; another build is skipped whole.
        site_at = 0x1000
        image = bytearray(0x6000)
        image[site_at:site_at + len(self.MYSTIC_SITE)] = self.MYSTIC_SITE
        segments = [(0, 0x5000), (0x5000, 0x5800)]
        found = dict(patches.compile_patches(xml, [self.MYSTIC], '01.47', segments, image=bytes(image)))
        self.assertEqual(set(found), {patches.align(site_at + len(self.MYSTIC_SITE)), site_at + 22})
        skipped = {}
        self.assertEqual(patches.compile_patches(xml, [self.MYSTIC], '01.47', segments,
                                                 image=bytes(0x6000), skipped=skipped), [])
        self.assertIn(self.MYSTIC, skipped)

    def test_offline_season_dates_reach_far_ahead(self):
        # The community buffs are on only while the clock is between CommunityBuffStart and End
        # (eboot 0x43470); the dates are fixed, the end far ahead (the parse is 64-bit FILETIME).
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        writes = patches.compile_patches(xml, ['Offline Seasons - Season 30'], '01.47', [(0, 0x1800000)])
        text = b''.join(value for _, value in writes)
        self.assertIn(b'Start "Sat, 09 Feb 2025 00:00:00 GMT"', text)
        self.assertIn(b'\nEnd "Thu, 31 Dec 2099 01:00:00 GMT"', text)
        self.assertIn(b'CommunityBuffStart "???, 16 Sep 2023 00:00:00 GMT"', text)
        self.assertIn(b'CommunityBuffEnd "???, 31 Dec 2099 01:00:00 GMT"', text)
        self.assertNotIn(b'2036', text)

    @unittest.skipUnless(sys.platform == 'linux' and os.uname().machine == 'x86_64', 'runs x86-64 code')
    def test_mystic_cave_runs(self):
        # The patched 1.47 native, run: its lookup is a stand-in that gives the name's SNO; the cave
        # looks A1_MysticUnlock up once (group 0x3a, through the native's own call target), keeps it
        # and hides that SNO only.
        import ctypes
        import mmap
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        site, fake, data = 0x1000, 0x3000, 0x4000  # code, the stand-in lookup, its variables
        count, group, result = data, data + 4, data + 8
        image = bytearray(0x6000)
        image[site:site + len(self.MYSTIC_SITE)] = self.MYSTIC_SITE
        caves = {}
        segments = [(0, 0x5000), (0x5000, 0x5800)]
        regions = {'code': (0x2000, 0x2800), 'data': (0x5800, 0x6000)}
        writes = patches.compile_patches(xml, [self.MYSTIC], '01.47', segments, image=bytes(image),
                                         regions=regions, caves=caves)
        for offset, value in writes:
            image[offset:offset + len(value)] = value
        memory = mmap.mmap(-1, len(image), prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
        base = ctypes.addressof(ctypes.c_char.from_buffer(memory))
        rel = lambda at, end, target: struct.pack('<i', target - end)
        # The native's call (site + 17) goes to the stand-in; after the cave: return ebx, or 0 (je).
        image[site + 18:site + 22] = rel(site + 18, site + 22, fake)
        image[site + 0x1d:site + 0x26] = bytes.fromhex('89d8 4883c408 5b 5d c3')
        image[site + 0x55:site + 0x5e] = bytes.fromhex('31c0 4883c408 5b 5d c3')
        mystic_sno, name_ptr = 430609, base + caves['mystic_cave'] + 63
        stub = bytearray()
        stub += b'\xff\x05' + rel(0, 0, 0)                     # inc dword [count]
        stub[2:6] = rel(fake + 2, fake + 6, count)
        stub += b'\x89\x3d' + rel(fake + 6, fake + 12, group)  # mov [group], edi
        stub += b'\x48\xb8' + struct.pack('<Q', name_ptr)      # movabs rax, name
        stub += bytes.fromhex('4839c6 7506') + b'\xb8' + struct.pack('<I', mystic_sno) + b'\xc3'
        stub += b'\x8b\x05' + rel(0, 0, 0) + b'\xc3'           # mov eax, [result]; ret
        stub[-5:-1] = rel(0, fake + len(stub) - 1, result)
        image[fake:fake + len(stub)] = stub
        memory[:] = bytes(image)
        native = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p)(base + site)
        value = lambda at: struct.unpack_from('<i', memory, at)[0]
        def call(sno):
            struct.pack_into('<i', memory, result, sno)
            return native(b'X1_WestmHub_MysticUnlock')
        self.assertEqual(call(1234), 1234)
        self.assertEqual(value(count), 2)  # the native's lookup, then the cave's own
        self.assertEqual(value(group), 0x3a)
        self.assertEqual(value(caves['mystic_sno']), mystic_sno)
        self.assertEqual(call(mystic_sno), 0)  # hidden: no such range
        self.assertEqual(call(-1), 0)
        self.assertEqual(call(77), 77)
        self.assertEqual(value(count), 5)  # looked up once
        del native
        memory.close()

    def test_every_patch_together_overlaps_no_other_and_its_caves_are_zero_padding(self):
        # All the built-in patches at once, with the 1.47 reference image when this checkout has it
        # (out/eboot.elf, or SANCTUARY_EBOOT_ELF; else at the reference Addresses): no two writes overlap, every cave lies in the zero padding
        # that ends the code (or the rest of the last data page), and nothing else writes there.
        # Patches that write the same sites are alternatives (one resolution, one season...): each
        # round takes the next of each such group.
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        names = sorted(patches.available_patches(xml, '01.47'))
        elf_path = Path(os.environ.get('SANCTUARY_EBOOT_ELF') or ROOT / 'out' / 'eboot.elf')
        if elf_path.is_file():
            elf = elf_path.read_bytes()
            image, segments, regions = patches.eboot_image(elf), patches.eboot_segments(elf), patches.cave_regions(elf)
        else:
            image, segments, regions = None, [(0, 0x1800000)], patches.REFERENCE_CAVE_REGIONS
        found, find = {}, patches.find_pattern  # each signature searched once in the 20 MB image
        def find_once(image, pattern, limit=2):
            key = (tuple(pattern), limit)
            if key not in found:
                found[key] = find(image, pattern, limit)
            return found[key]
        self.enterContext(mock.patch.object(patches, 'find_pattern', find_once))
        alone = {}
        for name in names:
            writes = patches.compile_patches(xml, [name], '01.47', segments, image, {}, regions)
            alone[name] = [(o, d) for o, d in writes if not any(s <= o < e for s, e in regions.values())]
        groups = []
        for name in names:
            group = next((g for g in groups if any(self.overlap(alone[name], alone[other]) for other in g)), None)
            if group is None:
                groups.append([name])
            else:
                group.append(name)
        self.assertIn('Offline Seasons - Season 30', next(g for g in groups if 'Offline Seasons - Season 14' in g))
        for round_ in range(max(len(g) for g in groups)):
            chosen = [g[round_ % len(g)] for g in groups]
            caves, skipped = {}, {}
            writes = patches.compile_patches(xml, chosen, '01.47', segments, image, skipped, regions, caves)
            self.assertEqual(skipped, {})
            self.assertGreater(len(caves), 5)  # compile_patches raised if two writes overlapped
            meta_caves = {c.get('Name'): int(c.get('Size'), 0) for c in ET.parse(xml).getroot().iter('Cave')}
            blocks = [(at, at + meta_caves[name]) for name, at in caves.items()]
            for start, end in blocks:
                self.assertTrue(any(s <= start and end <= e for s, e in regions.values()), hex(start))
                if image is not None and start < len(image):
                    self.assertEqual(image[start:end], bytes(end - start))
            for offset, data in writes:
                if any(s <= offset < e for s, e in regions.values()):
                    self.assertTrue(any(s <= offset and offset + len(data) <= e for s, e in blocks), hex(offset))

    def test_two_patches_that_write_the_same_bytes_are_refused(self):
        # Two seasons write different numbers into the one offline seasons cave; their base, chosen
        # with a season, is part of it.
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        segments = [(0, 0x2000000)]
        with self.assertRaisesRegex(ValueError, 'Season 30 writes .* which Offline Seasons - Season 29 also writes'):
            patches.compile_patches(xml, ['Offline Seasons - Season 29', 'Offline Seasons - Season 30'], '01.47',
                                    segments)
        self.assertEqual(patches.compile_patches(xml, ['Offline Seasons', 'Offline Seasons - Season 30'], '01.47',
                                                 segments),
                         patches.compile_patches(xml, ['Offline Seasons - Season 30'], '01.47', segments))

    @staticmethod
    def overlap(writes, others):
        return any(o < p + len(e) and p < o + len(d) for o, d in writes for p, e in others)

    def test_offline_patches_find_every_site_by_signature(self):
        # A different eboot is skipped, never overwritten: every Line has a signature, or writes at a
        # Target that has one or at a cave; the season entries are the shared base plus their own values.
        root = ET.parse(ROOT / 'patches' / 'Diablo_III.xml').getroot()
        metas = {m.get('Name'): m for m in root.iter('Metadata') if m.get('Name') in self.OFFLINE_NAMES}
        # The seasons are the Presets of one parameterised patch, each as the entry it stands for.
        metas.update({name: entries[0] for name, entries in
                      patches.preset_patches(ROOT / 'patches' / 'Diablo_III.xml', '01.47').items()
                      if name in self.OFFLINE_NAMES})
        self.assertEqual(set(metas), self.OFFLINE_NAMES)
        for name, meta in metas.items():
            self.assert_found_by_signature(patches.with_base(meta, list(root.iter('Metadata'))), name)
            self.assertNotIn('Claude', meta.get('Author'))
            self.assertNotIn('Local mod', meta.get('Note'))
        # GitHub #100: the two clears of the buffs 1.47 turns on by default are Optional (older builds
        # have not got them); each standalone event needs the seasons hook, so a build without
        # seasons (1.00) skips it rather than writing a cave nothing reads.
        optional = [l.get('Address') for l in metas['Offline Seasons'].iter('Line') if l.get('Optional') == 'true']
        self.assertEqual(optional, ['0x926fe6', '0x9272cf'])
        hook = next(l for l in metas['Offline Seasons'].iter('Line') if l.get('Address') == '0x443528')
        for name in self.EVENTS:
            if any(t.get('Cave') == 'seasons_cave' for t in metas[name].iter('Target')):
                guard = metas[name].find("Target[@Name='seasons']")
                self.assertEqual((guard.get('Pattern'), guard.get('At')), (hook.get('Pattern'), hook.get('At')), name)
        image = bytearray(0x4000)
        image[0x100] = 0xff
        skipped = {}
        self.assertEqual(patches.compile_patches(ROOT / 'patches' / 'Diablo_III.xml', ['Event: Sanctified Items'],
                                                 '01.47', [(0, len(image))], image=bytes(image), skipped=skipped), [])
        self.assertEqual(skipped, {'Event: Sanctified Items': 'signature not found'})

    def offline_image(self, names, older=False):
        """A small image with every signature of these patches (?? as 0), one per 0x100 bytes, then
        zero padding: as 1.47 has them, or older (the 1.46 / 1.25 forms: the second '|' form, no
        Optional site)."""
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        every = list(ET.parse(xml).getroot().iter('Metadata'))
        presets = patches.preset_patches(xml, '01.47')
        forms = []
        for name in names:
            meta = presets[name][0] if name in presets else next(m for m in every if m.get('Name') == name)
            for element in [*patches.with_base(meta, every).iter('Line'), *patches.with_base(meta, every).iter('Target')]:
                pattern = element.get('Pattern')
                if not pattern or (older and element.get('Optional') == 'true'):
                    continue
                pattern = pattern.split('|')[-1 if older else 0].strip()
                if pattern not in forms:
                    forms.append(pattern)
        image = bytearray(0x100 * (len(forms) + 1) + 0x2000)
        for i, pattern in enumerate(forms):
            data = bytes.fromhex(pattern.replace('??', '00'))
            image[0x100 * i:0x100 * i + len(data)] = data
        return bytes(image), forms

    def test_offline_seasons_on_older_builds(self):
        # GitHub #100 (1.46 = 2.7.6, 1.25 = 2.6.1): no Altar/Visions clears, and the online-state
        # check keeps the call's result in r13d. The season is applied without the clears; its
        # writes in 1.47's form are the same with them.
        name = 'Offline Seasons - Season 30'
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        for older in (False, True):
            image, forms = self.offline_image([name, 'Event: Double Rift Keystones'], older)
            segments = [(0, len(image)), (len(image) + 0x1000, len(image) + 0x1100)]
            skipped = {}
            writes = dict(patches.compile_patches(xml, [name, 'Event: Double Rift Keystones'], '01.47', segments,
                                                  image, skipped))
            self.assertEqual(skipped, {})
            clears = [w for w in writes.values() if w == bytes.fromhex('c7436000000000c7436400000000c7436800000000')]
            self.assertEqual(len(clears), 0 if older else 2)
            check = next(i for i, f in enumerate(forms) if f.startswith('bf 01 00 00 00 e8'))
            self.assertEqual(writes[0x100 * check + 5], bytes.fromhex('31c0909090'))  # the call: xor eax,eax
            self.assertEqual(writes[0x100 * forms.index(next(f for f in forms if f.startswith('8b 05 ?? ?? ?? ?? 85 c0')))],
                             bytes.fromhex('b80100000090'))  # mov eax,1: the season is confirmed

    def test_a_season_is_the_base_with_its_number_and_theme(self):
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        base = ET.parse(xml).getroot().find("Metadata[@Name='Offline Seasons']")
        merged = patches.with_base(
            patches.preset_patches(xml, '01.47')['Offline Seasons - Season 22'][0],
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
        expected |= {f'{graphics_options.LIMITER_PREFIX} - {n}' for n in ('30 FPS', '90 FPS', '120 FPS', 'Unlimited')}
        expected.add(f'{graphics_options.MSAA_PREFIX} - 4x')
        expected |= {f'{graphics_options.RENDER_TARGETS_PREFIX} - {n}' for n in (
            'Shadow Map 2048', 'Water and Fog 2048', 'PMA 2048',
            'Reflection 960x540', 'Reflection 1280x720', 'Reflection 1600x900', 'Reflection 1920x1080',
            'Reflection 1280x540', 'Reflection 1720x720', 'Reflection 1920x800', 'Reflection 2560x720',
            'Reflection 960x600', 'Reflection 1280x800')}
        expected |= self.OFFLINE_NAMES | self.CHEATS | {
            self.SMOOTH, self.MYSTIC, graphics_options.FIXED_SCALE_PATCH,
            graphics_options.FOREGROUND_CAP_PATCH, graphics_options.COOP_EFFECTS,
            graphics_options.PC_CAMERA_ZOOM, *self.TEXT_PATCHES}
        self.assertEqual(names, expected)
        # The sizes, frame limits and reflection sizes are Presets of parameterised patches: each is
        # checked as the entries (1.47, 1.00) it stands for.
        entries = [m for m in ET.parse(xml).getroot().iter('Metadata') if not m.get('Params')]
        entries += [m for found in patches.preset_patches(xml, '01.47').values() for m in found]
        for meta in entries:
            if (meta.get('Name') in self.OFFLINE_NAMES | self.CHEATS
                    | {self.SMOOTH, self.MYSTIC, graphics_options.FIXED_SCALE_PATCH,
                       graphics_options.FOREGROUND_CAP_PATCH, *self.TEXT_PATCHES}
                    or meta.get('Name').startswith(graphics_options.MSAA_PREFIX)):
                continue
            if meta.get('Name') == graphics_options.COOP_EFFECTS:
                # The 1 the game writes from the second local player on, found by its signature.
                line = meta.find('PatchList/Line')
                self.assertEqual(line.get('Pattern').split()[int(line.get('At'))], '01')
                self.assertEqual(line.get('Value'), '00')
                continue
            if meta.get('Name') == graphics_options.PC_CAMERA_ZOOM:
                if meta.get('PatchVer') == '1.2':
                    # 1.47/1.46: the camera update's one call to LookAt goes through the camdolly cave first.
                    sites = [line for line in meta.iter('Line') if line.get('Pattern')]
                    self.assertEqual(len(sites), 1)
                    pattern, at = sites[0].get('Pattern').split(), int(sites[0].get('At'))
                    self.assertEqual(pattern[at - 1], 'e8')
                    self.assertEqual(sites[0].get('Value'), '{rel:camdolly:0}')
                    self.assertEqual([c.get('Name') for c in meta.iter('Cave')], ['camdolly'])
                    continue
                # Every write replaces a GlobalSNO observer slot immediate inside `mov edi, imm32` (0xbf).
                for line in meta.iter('Line'):
                    pattern, at = line.get('Pattern').split(), int(line.get('At'))
                    self.assertEqual(pattern[at - 1], 'bf')
                    old = int.from_bytes(bytes.fromhex(''.join(pattern[at:at + 4])), 'little')
                    new = int.from_bytes(bytes.fromhex(line.get('Value')), 'little')
                    self.assertIn(old - new, (1, 2), (hex(old), hex(new)))
                continue
            if meta.get('Name').startswith(graphics_options.RENDER_TARGETS_PREFIX):
                part = meta.get('Name').split(' - ')[1]
                # Each line replaces a console target size (1536, 1408, 1728, or 800 and 450) with the PC's.
                lines = list(meta.iter('Line'))
                for index, line in enumerate(lines):
                    pattern, at = line.get('Pattern').split(), int(line.get('At'))
                    old = int.from_bytes(bytes.fromhex(''.join(pattern[at:at + 4])), 'little')
                    new = int.from_bytes(bytes.fromhex(line.get('Value')), 'little')
                    if part.startswith('Reflection '):
                        width, height = map(int, part.split()[1].split('x'))
                        self.assertEqual((old, new), (800, width) if index % 2 == 0 else (450, height), part)
                    else:
                        self.assertEqual(old, {'Shadow Map 2048': 1536, 'Water and Fog 2048': 1408,
                                               'PMA 2048': 1728}[part], part)
                        self.assertEqual(new, 2048, part)
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

    TEXT_PATCHES = (graphics_options.TEXT_LAYOUT_PATCH, graphics_options.TEXT_BOTH_SETS_PATCH)

    def test_text_is_drawn_at_the_size_its_layout_measured(self):
        # GitHub #77: 0x201ee0 picks the font size to draw: the first one above the request when none
        # matches; the layout (0x202000) measures the largest one below it. The patch makes the draw
        # keep the size below (the smallest when there is none), in the same 43 bytes.
        root = ET.parse(ROOT / 'patches' / 'Diablo_III.xml').getroot()
        (meta,) = [m for m in root.iter('Metadata') if m.get('Name') == graphics_options.TEXT_LAYOUT_PATCH]
        self.assertEqual(meta.get('isEnabled'), 'false')
        (line,) = meta.iter('Line')
        pattern = bytes.fromhex(line.get('Pattern').replace(' ', ''))
        at, value = int(line.get('At')), bytes.fromhex(line.get('Value'))
        code = pattern[:at] + value + pattern[at + len(value):]
        self.assertEqual(len(code), len(pattern))
        # The loop starts at 6 (test [rax+0x20], ebx); "xor eax, eax" and the flag check follow it unchanged.
        self.assertEqual(code[6:9], bytes.fromhex('855820'))
        self.assertEqual(at + len(value), len(pattern) - 2)
        self.assertEqual(code[-2:], bytes.fromhex('31c0'))

        def target(offset):  # short jump at offset -> its target
            return offset + 2 + struct.unpack('b', code[offset + 1:offset + 2])[0]

        self.assertEqual(code[9], 0x74)                 # mask mismatch -> next entry
        self.assertEqual(code[target(9):target(9) + 2], bytes.fromhex('ffc2'))
        self.assertEqual(code[20], 0x72)                 # size above the request
        bigger = target(20)
        # rsi (the size below) kept, else this one: test rsi, rsi; cmovz rsi, rax.
        self.assertEqual(code[bigger:bigger + 7], bytes.fromhex('4885f6480f44f0'))
        self.assertEqual(code[26:28], bytes.fromhex('7326'))  # an exact size still returns at once
        self.assertEqual(code[28:31], bytes.fromhex('4889c6'))  # a size below is remembered
        loop_end = code.index(bytes.fromhex('72dd'))
        self.assertEqual(target(loop_end), 6)
        self.assertEqual(target(loop_end + 2), len(code) - 2)

    def test_between_1080_and_2160_lines_both_font_sets_are_loaded(self):
        # GitHub #75: the four font set choices take mask 0x18 (1080p and 2160p fonts) where they took 8.
        root = ET.parse(ROOT / 'patches' / 'Diablo_III.xml').getroot()
        (meta,) = [m for m in root.iter('Metadata') if m.get('Name') == graphics_options.TEXT_BOTH_SETS_PATCH]
        self.assertEqual(meta.get('isEnabled'), 'false')
        lines = list(meta.iter('Line'))
        self.assertEqual(len(lines), 4)
        for line in lines:
            pattern, at = line.get('Pattern').split(), int(line.get('At'))
            self.assertEqual(line.get('Value'), '18')
            # mov <mask>, 8; cmp eax, 1079: the choice for 1080 to 2159 lines.
            self.assertEqual(pattern[at:at + 6], ['08', '00', '00', '00', '3d', '37'])
            self.assertIn(pattern[at - 1], ('a8', 'b8', 'bb'))

    @unittest.skipUnless((ROOT / 'out' / 'eboot.elf').is_file(), 'needs the decrypted 1.47 eboot (run.sh)')
    def test_text_size_patches_find_their_sites_in_the_game(self):
        elf = (ROOT / 'out' / 'eboot.elf').read_bytes()
        image = patches.eboot_image(elf)
        if image[0x201fa0:0x201fa5] != bytes.fromhex('855820741a'):
            self.skipTest('not the 1.47 eboot')
        writes = dict(patches.compile_patches(ROOT / 'patches' / 'Diablo_III.xml', list(self.TEXT_PATCHES), '01.47',
                                              patches.eboot_segments(elf), image=image))
        self.assertEqual(sorted(writes), [0x2015cb, 0x201ae6, 0x201f22, 0x201fa3, 0x202042])
        self.assertTrue(all(image[a] == 8 for a in (0x2015cb, 0x201ae6, 0x201f22, 0x202042)))

    def test_text_size_patches_follow_the_render_height(self):
        both = [graphics_options.TEXT_LAYOUT_PATCH, graphics_options.TEXT_BOTH_SETS_PATCH]
        for resolution, expected in (('native', []), ('4K', []), ('2560x1080', []), ('720p', both[:1]),
                                     ('1280x800', both[:1]), ('1440p', both), ('1920x1200', both),
                                     ('3840x1600', both), ('5120x2160', []), ('bogus', [])):
            self.assertEqual(graphics_options.text_size_names(['Other'], resolution), ['Other'] + expected,
                             resolution)
        # Whatever the list named: a stale choice from another resolution is dropped.
        self.assertEqual(graphics_options.text_size_names(both, 'native'), [])

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

    def test_1_00_gets_its_own_render_target_sites(self):
        # The 1.00 disc executable has the same shadow and water/fog setup with other registers, no PMA
        # RT and half-size reflections already, so Shadow Map and Water and Fog take their second entry
        # while PMA and Reflection are skipped.
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        shadow = bytes.fromhex('4183fc04751041c7854001000000060000bb19000000')
        water = bytes.fromhex('be000000004c89fab98005000041b880050000')
        fog = bytes.fromhex('c70424010000004c89fab98005000041b880050000')
        image = bytearray(0x200) + shadow + bytearray(0x40) + water + bytearray(0x40) + fog + bytearray(0x200)
        names = graphics_options.render_target_names([], {'pc_render_targets': '1'}, '1440p')
        skipped = {}
        writes = patches.compile_patches(xml, names, '01.47', [(0, len(image))], image=bytes(image), skipped=skipped)
        self.assertEqual(sorted(skipped), ['PC Render Targets - PMA 2048', 'PC Render Targets - Reflection 1280x720'])
        self.assertEqual(len(writes), 5)
        self.assertTrue(all(data == (2048).to_bytes(4, 'little') for _, data in writes))

    def test_pc_camera_zoom_matches_each_build_variant(self):
        # Three Metadata entries cover 1.47/1.46, 1.25 (2.6.1) and 1.00 (2.0.0, before Retro); an
        # unrecognised executable is skipped whole.
        xml = ROOT / 'patches' / 'Diablo_III.xml'
        metas = [m for m in ET.parse(xml).getroot().iter('Metadata')
                 if m.get('Name') == graphics_options.PC_CAMERA_ZOOM]
        self.assertEqual(len(metas), 3)
        for meta, expected_writes in zip(metas, (3, 5, 3)):
            seen = {}
            image = bytearray(0x200)
            for line in [*meta.iter('Line'), *meta.iter('Target')]:
                pat = line.get('Pattern')
                if pat and pat not in seen:
                    seen[pat] = len(image)
                    image += bytes.fromhex(' '.join('5a' if b == '??' else b for b in pat.split())) + bytearray(0x40)
            # Zero padding at the end: room for the 1.47 entry's camdolly cave.
            image += bytearray(0x200)
            skipped = {}
            writes = patches.compile_patches(xml, [graphics_options.PC_CAMERA_ZOOM], '01.47',
                                             [(0, len(image))], image=bytes(image), skipped=skipped)
            self.assertEqual(skipped, {})
            self.assertEqual(len(writes), expected_writes)
        skipped = {}
        self.assertEqual(patches.compile_patches(xml, [graphics_options.PC_CAMERA_ZOOM], '01.47',
                                                 [(0, 0x400)], image=bytes(0x400), skipped=skipped), [])
        self.assertEqual(skipped, {graphics_options.PC_CAMERA_ZOOM: 'signature not found'})


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
