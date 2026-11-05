"""The movie library's NDVE shader pack (ndve_shaders), without game data: programs laid out raw (control section, code area at +0x30 from it),
programs named only by the NDSR record table in the header, the pack let through the split's prefilter, and sections that are not programs left out."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def ctl(version, rel, n, group=(16, 2, 1), length=0xe0):
    c = bytearray(length)
    c[:4] = split.CTL_MAGIC; c[4:8] = b'\xc9\0\0\0'
    struct.pack_into('<I', c, 8, version)
    struct.pack_into('<Q', c, 0x30, rel)
    struct.pack_into('<II', c, 0x40, n, n)
    c[0x56] = 5   # compute
    struct.pack_into('<3I', c, 0x58, *group)
    return c


def pack():
    """NDVE header with an NDSR table at +0x28 (one record among records of other sizes), a table-named program, two raw programs and a section without code"""
    d = bytearray(0x400)
    d[:4] = b'NDVE'; d[0x28:0x2c] = b'NDSR'
    t = 0x28
    code_t = bytes([0x19, 0x79]) + bytes(0x17e)   # 0x180 bytes, the table-named program's code area
    d[0x100:0x100 + len(code_t)] = code_t
    c_t = 0x300
    d[c_t:c_t + 0xe0] = ctl(0x11, 0, len(code_t))
    words = [0x0c000001, 0x104f00, c_t - t, 0xe0 << 4 | 6, 0x100 - t, len(code_t) // 16 << 8 | 7, 0x49]
    struct.pack_into(f'<{len(words)}I', d, 0x30, *words)
    for i in range(2):   # raw programs: section, then its code area 0x100 further on
        c = len(d); code = bytes([0x19, 0x79, i]) + bytes(0xfd)
        d += ctl(0x14, 0x100, len(code), group=(32, 1, 1)) + bytes(0x100 - 0xe0) + code
    d += ctl(0x11, 0, 0x40)   # no code area of its own
    return bytes(d)


def ndsr_pack():
    """agl's variable-ratio NDSR pack: 8-byte entries from +0x14, a program, 4 bytes of data (type 9), then a second program that lands on an odd entry, and a third on an even one"""
    d = bytearray(0x800)
    d[:4] = b'NDSR'; struct.pack_into('<I', d, 4, 0x10100)
    words, ctl_at, code_at = [], 0x200, 0x400
    for i in range(3):
        code = bytes([0x19, 0x79, i]) + bytes(0xfd)
        d[code_at:code_at + len(code)] = code
        d[ctl_at:ctl_at + 0xe0] = ctl(0x11, 0, len(code))
        words += [ctl_at, 0xe0 << 4 | 6, code_at, len(code) << 4 | 7]
        if i == 0: words += [0x1f0, 4 << 4 | 9]
        ctl_at += 0xe0; code_at += 0x100
    struct.pack_into(f'<{len(words)}I', d, 0x14, *words)
    return bytes(d)


class NdsrTests(unittest.TestCase):
    def test_a_program_on_an_odd_entry(self):
        found = split.ndsr_shaders(ndsr_pack(), 'Lib/agl/x.pck')
        self.assertEqual(len(found), 3)
        self.assertEqual({st for _, st, _, _ in found}, {'cs'})
        self.assertEqual([code[2] for _, _, _, code in found], [0, 1, 2])


class NdveTests(unittest.TestCase):
    def test_raw_and_table_programs(self):
        found = split.ndve_shaders(pack(), 'Movie/Ndve/x.dat')
        self.assertEqual(len(found), 3)
        self.assertEqual({st for _, st, _, _ in found}, {'cs'})
        self.assertEqual(sum('#NDVE#NDSR@' in w for w, _, _, _ in found), 1)
        for _, st, c, code in found:
            self.assertEqual(struct.unpack_from('<I', c, 0x44)[0], len(code))
            self.assertIsNone(split.check('x', st, c, code))
        self.assertEqual(len({split.skey(c, code) for _, _, c, code in found}), 3)

    def test_key_ignores_the_offset_word(self):
        """the host's key takes the first 0x48 bytes with +0x30 zeroed: a section's code offset does not change its key"""
        found = split.ndve_shaders(pack(), 'x')
        _, _, c, code = next(f for f in found if '#NDVE@' in f[0])
        moved = bytearray(c); struct.pack_into('<Q', moved, 0x30, 0x9999)
        self.assertEqual(split.skey(c, code), split.skey(bytes(moved), code))

    def test_not_a_pack(self):
        self.assertEqual(split.ndve_shaders(b'NDSR' + bytes(0x100), 'x'), [])

    def test_scan_blob_reads_it(self):
        found = []
        split.scan_blob(pack(), 'Movie/Ndve/x.dat', found)
        self.assertEqual(len(found), 3)


if __name__ == '__main__':
    unittest.main()
