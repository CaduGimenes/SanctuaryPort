"""Value parameters of the patch format (scripts/patches.py) and the Diablo III patches that use them.

The equivalence tests compile every entry the parameterised patches replaced (tests/data/
diablo3_families_before.xml, the former one-entry-per-value patches) beside the parameterised patch with
the values run.py works out, and expect the same writes: at the reference build's addresses, and with
the game's image when one is there (out/eboot.elf, or SANCTUARY_TEST_EBOOT_ELF).
"""
import os
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import paths  # noqa: F401 (puts scripts/ on sys.path)
import graphics_options as g
import patches

ROOT = paths.ROOT
XML = ROOT / 'patches' / 'Diablo_III.xml'
BEFORE = ROOT / 'tests' / 'data' / 'diablo3_families_before.xml'
SEGMENTS = [(0, 0x2000000)]
# The parameterised patch each former family became.
FAMILIES = (('Resolution Patch - ', g.RESOLUTION_PATCH), ('Frame Limiter - ', g.LIMITER_PATCH),
            ('PC Render Targets - Reflection ', g.REFLECTION_PATCH),
            ('Offline Seasons - Season ', 'Offline Seasons - Season'))


def reference_elf():
    for candidate in (os.environ.get('SANCTUARY_TEST_EBOOT_ELF'), ROOT / 'out' / 'eboot.elf'):
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def before_names():
    names = []
    for meta in ET.parse(BEFORE).getroot().iter('Metadata'):
        if meta.get('Name') not in names:
            names.append(meta.get('Name'))
    return names


class EquivalenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The current file with the former entries added back (their names are Presets now, and a plain
        # Metadata of a name wins over a Preset): a season entry then sits on the current base.
        cls.dir = tempfile.TemporaryDirectory()
        tree = ET.parse(XML)
        for meta in ET.parse(BEFORE).getroot().iter('Metadata'):
            tree.getroot().append(meta)
        cls.old_xml = Path(cls.dir.name) / 'old.xml'
        tree.write(cls.old_xml, encoding='utf-8', xml_declaration=True)
        cls.presets = patches.preset_choices(XML, '01.47')

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def values_for(self, name):
        """The patch and values run.py hands patches.py for a former name (a season: its Preset's)."""
        names, values, chosen_as = g.with_values([name], self.presets)
        if names == [name]:
            return self.presets[name]
        self.assertEqual(chosen_as, {names[0]: name})
        return names[0], values

    def test_every_former_entry_is_a_preset_of_its_family(self):
        names = before_names()
        self.assertEqual(len(names), 13 + 4 + 10 + 27)
        for name in names:
            patch = next(p for prefix, p in FAMILIES if name.startswith(prefix))
            self.assertEqual(self.presets[name][0], patch, name)
        self.assertEqual(sorted(n for n in self.presets), sorted(names))

    def test_the_worked_out_values_are_the_presets(self):
        for name in before_names():
            patch, values = self.values_for(name)
            self.assertEqual(self.presets[name], (patch, values), name)

    def check_writes(self, segments, image):
        for name in before_names():
            old = patches.compile_patches(self.old_xml, [name], '01.47', segments, image)
            patch, values = self.values_for(name)
            new = patches.compile_patches(XML, [patch], '01.47', segments, image, params=values)
            by_name = patches.compile_patches(XML, [name], '01.47', segments, image)
            self.assertTrue(old, name)
            self.assertEqual(new, old, name)
            self.assertEqual(by_name, old, name)

    def test_same_writes_at_the_reference_addresses(self):
        self.check_writes(SEGMENTS, None)

    @unittest.skipUnless(reference_elf(), 'needs the decrypted eboot.elf (out/ or SANCTUARY_TEST_EBOOT_ELF)')
    def test_same_writes_found_by_signature_in_the_game(self):
        elf = reference_elf().read_bytes()
        self.check_writes(patches.eboot_segments(elf), patches.eboot_image(elf))

    def test_each_build_entry_writes_the_same_lines(self):
        # The 1.00 entries too (no 1.00 image here): the written Lines, Value bytes included.
        def lines(meta):
            return [(l.get('Type'), l.get('Address'), l.get('Pattern'), l.get('At'), l.get('Target'),
                     patches.encode(l) if '{' not in l.get('Value') else l.get('Value')) for l in meta.iter('Line')]
        expanded = patches.preset_patches(XML, '01.47')
        before = {}
        for meta in ET.parse(BEFORE).getroot().iter('Metadata'):
            before.setdefault(meta.get('Name'), []).append(meta)
        for name, olds in before.items():
            self.assertEqual([lines(m) for m in expanded[name]], [lines(m) for m in olds], name)
            self.assertEqual([m.get('Base') for m in expanded[name]], [m.get('Base') for m in olds], name)

    def test_memory_needs_follow_the_values(self):
        # The 1.00 entries above 900p asked for 256 MB more direct memory; they still do, and only they.
        for name in before_names():
            if not name.startswith('Resolution Patch - '):
                continue
            old = patches.patch_requirements(self.old_xml, [name], '01.47')
            patch, values = self.values_for(name)
            self.assertEqual(patches.patch_requirements(XML, [patch], '01.47', values), old, name)
            self.assertEqual(patches.patch_requirements(XML, [name], '01.47'), old, name)


class FrameWaitTests(unittest.TestCase):
    def test_the_game_rounding(self):
        self.assertEqual(g.frame_wait(60), 16683)
        self.assertEqual(g.frame_wait(90), 11122)
        self.assertEqual(g.frame_wait(120), 8341)
        self.assertEqual(g.frame_wait(30), 33367)
        self.assertEqual(g.frame_wait(0), 0)
        self.assertEqual(g.frame_wait(144), 6951)  # not offered: possible once the launcher has it


class SyntaxTests(unittest.TestCase):
    FILE = '''<Patch>
  <Metadata Name="Sized" Params="w h wide tag" AppVer="01.47">
    <PatchList>
      <Line Type="bytes" Address="0x400010" Value="b8{param:w:u32}"/>
      <Line Type="bytes" Address="0x400020" Value="{param:h:u16}"/>
      <Line Type="bytes" Address="0x400030" Value="{param:tag:a2}" If="wide"/>
    </PatchList>
    <Presets><Preset Name="Sized - Small" Note="n" w="0x10" h="2" wide="0" tag="ab"/></Presets>
  </Metadata>
  <Metadata Name="Float" Params="f" AppVer="01.47">
    <PatchList><Line Type="bytes" Address="0x400040" Value="{param:f:f32}"/></PatchList>
  </Metadata>
  <Metadata Name="Raw" Params="raw" AppVer="01.47">
    <PatchList><Line Type="bytes" Address="0x400060" Value="{param:raw:2}c3"/></PatchList>
  </Metadata>
  <Metadata Name="Plain" AppVer="01.47">
    <PatchList><Line Type="bytes" Address="0x400050" Value="90"/></PatchList>
  </Metadata>
</Patch>'''

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.xml = Path(self.dir.name) / 'p.xml'
        self.xml.write_text(self.FILE)

    def tearDown(self):
        self.dir.cleanup()

    def compile(self, names, params=None):
        return patches.compile_patches(self.xml, names, '01.47', SEGMENTS, params=params)

    def test_values_are_filled_in(self):
        writes = self.compile(['Sized'], {'w': '640', 'h': '480', 'wide': '1', 'tag': 'xy'})
        self.assertEqual(writes, [(0x10, bytes.fromhex('b880020000')), (0x20, (480).to_bytes(2, 'little')),
                                  (0x30, b'xy')])
        # If="wide" 0 leaves the line out; a number type is an integer, N bytes are hex.
        self.assertEqual(len(self.compile(['Sized'], {'w': '1', 'h': '1', 'wide': 'false', 'tag': 'xy'})), 2)
        self.assertEqual(self.compile(['Float'], {'f': '1.6'}), [(0x40, bytes.fromhex('cdcccc3f'))])
        self.assertEqual(self.compile(['Raw'], {'raw': '33 30'}), [(0x60, bytes.fromhex('3330c3'))])

    def test_a_preset_selects_its_patch_with_its_values(self):
        self.assertEqual(self.compile(['Sized - Small', 'Plain']),
                         [(0x10, bytes.fromhex('b810000000')), (0x20, bytes.fromhex('0200')), (0x50, b'\x90')])
        self.assertIn('Sized - Small', patches.available_patches(self.xml, '01.47'))
        self.assertNotIn('Sized', patches.available_patches(self.xml, '01.47'))
        self.assertEqual(patches.preset_choices(self.xml, '01.47'),
                         {'Sized - Small': ('Sized', {'w': '0x10', 'h': '2', 'wide': '0', 'tag': 'ab'})})

    def test_unknown_missing_and_bad_values_fail_clearly(self):
        with self.assertRaisesRegex(ValueError, r"unknown patch parameter\(s\) \['hieght'\]"):
            self.compile(['Float'], {'f': '1', 'hieght': '2'})
        with self.assertRaisesRegex(ValueError, r"Sized: missing patch parameter\(s\) \['tag'\]"):
            self.compile(['Sized'], {'w': '1', 'h': '1', 'wide': '0'})
        with self.assertRaisesRegex(ValueError, r'h=.70000. is not a valid u16'):
            self.compile(['Sized'], {'w': '1', 'h': '70000', 'wide': '0', 'tag': 'ab'})
        with self.assertRaisesRegex(ValueError, r'tag=.abc. is 3 bytes, not 2'):
            self.compile(['Sized'], {'w': '1', 'h': '1', 'wide': '1', 'tag': 'abc'})
        with self.assertRaisesRegex(ValueError, r'wide=.yes. must be 1/true or 0/false'):
            self.compile(['Sized'], {'w': '1', 'h': '1', 'wide': 'yes', 'tag': 'ab'})
        with self.assertRaisesRegex(ValueError, r'expected NAME=VALUE'):
            patches.parse_params(['frame_wait'])
        self.assertEqual(patches.parse_params(['a=1', ' b = x=y ']), {'a': '1', 'b': 'x=y'})

    def test_a_placeholder_must_be_declared(self):
        self.xml.write_text(self.FILE.replace('Params="f"', 'Params=""'))
        with self.assertRaisesRegex(ValueError, r'Float: \{param:f:f32\} is not declared'):
            self.compile(['Float'], {})


if __name__ == '__main__':
    unittest.main()
