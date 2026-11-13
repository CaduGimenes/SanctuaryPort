import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
import cosmetics  # noqa: E402


def field(number, payload):
    return cosmetics._length_field(number, payload)


def varint_field(number, value):
    return cosmetics._put_varint(number << 3) + cosmetics._put_varint(value)


class CosmeticsTest(unittest.TestCase):
    def test_the_table_leaves_out_the_art_tests(self):
        bits = cosmetics.unlockable_bits()
        self.assertEqual(len(bits), 291)
        self.assertNotIn(7, bits)   # Art_Test_Pennant_6
        self.assertIn(33, bits)     # Cosmetic_Wings_bat
        self.assertNotIn(77, bits)  # unnamed

    def test_unlock_keeps_everything_else(self):
        elements = b''.join(b'\x0d' + n.to_bytes(4, 'little', signed=True) for n in (-5, 7))
        array = elements + field(2, b'\x00\x10') + varint_field(3, 1)  # bit 4 of byte 2 -> bit 20
        save = (varint_field(1, 116) + field(2, b'\x08\x74') + field(24, field(1, array))
                + field(27, b'\x10\x03'))
        self.assertEqual(cosmetics.unlocked(save), {20})
        bits = cosmetics.unlockable_bits()
        changed = cosmetics.unlock(save, bits)
        self.assertEqual(cosmetics.unlocked(changed), set(bits) | {20})
        before = {n: raw for n, _, _, raw in cosmetics.fields(save) if n != 24}
        after = {n: raw for n, _, _, raw in cosmetics.fields(changed) if n != 24}
        self.assertEqual(before, after)
        (inner,) = [v for n, _, v, _ in cosmetics.fields(changed) if n == 24]
        (new_array,) = [v for n, _, v, _ in cosmetics.fields(inner) if n == 1]
        self.assertTrue(new_array.startswith(elements))
        self.assertEqual(cosmetics.unlock(changed, bits), changed)

    def test_a_save_without_cosmetics_gets_them(self):
        save = varint_field(1, 116)
        changed = cosmetics.unlock(save, [0, 9])
        self.assertEqual(cosmetics.unlocked(changed), {0, 9})

    def test_transmog_goes_to_every_partition_with_crafter_data(self):
        looks = cosmetics.transmog_bits()
        self.assertEqual(len(looks), 764)
        crafter = field(1, b'\x08\x00') + field(2, field(2, b'\x10') + varint_field(3, 76))
        played = varint_field(1, 0) + field(2, b'') + field(4, crafter) + varint_field(8, 3)
        unplayed = varint_field(1, 1) + field(2, b'')
        save = varint_field(1, 116) + field(20, played) + field(20, unplayed) + field(27, b'')
        self.assertEqual(cosmetics.transmog_unlocked(save), [{612}])
        changed = cosmetics.unlock_transmog(save, looks)
        (got,) = cosmetics.transmog_unlocked(changed)
        self.assertEqual(got, set(looks) | {612})
        partitions = [v for n, _, v, _ in cosmetics.fields(changed) if n == 20]
        self.assertEqual(partitions[1], unplayed)
        (new_crafter,) = [v for n, _, v, _ in cosmetics.fields(partitions[0]) if n == 4]
        self.assertTrue(new_crafter.startswith(field(1, b'\x08\x00')))
        self.assertTrue(partitions[0].endswith(varint_field(8, 3)))
        self.assertEqual(cosmetics.unlock_transmog(changed, looks), changed)

    def test_xor_round_trip(self):
        data = bytes(range(256)) * 3
        self.assertEqual(cosmetics.decode(cosmetics.encode(data)), data)
        self.assertEqual(cosmetics.encode(b'\x08\x74')[:1], bytes([0x08 ^ 0x1B]))


if __name__ == '__main__':
    unittest.main()
