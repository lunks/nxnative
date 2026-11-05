"""BNSH programs of NVN2 (bnsh_shaders), without game data: the task and mesh shaders in stage slots 7 and 8 past the six NVN stages are read, with their stage from the
control section's stage byte, and words in those slots that are not NVN2 stage records (an NVN program's, or none) are left alone."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def ctl(stage, n):
    c = bytearray(0xe0)
    c[:4] = split.CTL_MAGIC; c[4:8] = b'\xc9\0\0\0'
    struct.pack_into('<II', c, 0x40, n, n)
    c[0x56] = stage
    return bytes(c)


def bnsh(slots):
    """One variation whose program has the given {slot: (stage byte, code byte)} records, and raw words where the value is an int"""
    d = bytearray(0x2000)
    d[:4] = b'BNSH'; d[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', d, 0x7c, 1, 0x100)          # one variation, its records at +0x100
    struct.pack_into('<Q', d, 0x100 + 0x10, 0x200)      # its program at +0x200
    at = 0x400
    for si, rec in slots.items():
        if isinstance(rec, int):
            struct.pack_into('<Q', d, 0x200 + 8 + 8 * si, rec); continue
        stage, b = rec
        code = bytes([b]) * 0x100
        c = ctl(stage, len(code))
        sc, pc, pk = at, at + 0x20, at + 0x100
        d[pc:pc + len(c)] = c; d[pk:pk + len(code)] = code
        struct.pack_into('<QQII', d, sc + 8, pc, pk, len(code), len(c))
        struct.pack_into('<Q', d, 0x200 + 8 + 8 * si, sc)
        at += 0x300
    return bytes(d)


class Nvn2SlotTests(unittest.TestCase):
    def test_task_and_mesh_slots(self):
        found = split.bnsh_shaders(bnsh({4: (0, 1), 7: (6, 2), 8: (7, 3)}), 0, 'Shader/x.bfsha')
        self.assertEqual(sorted(st for _, st, _, _ in found), ['ms', 'ps', 'ts'])
        self.assertEqual({st: code[0] for _, st, _, code in found}, {'ps': 1, 'ts': 2, 'ms': 3})

    def test_other_words_left_alone(self):
        """past slot 5, a word that points outside the file or at no NVN2 control section is not a stage record"""
        d = bytearray(bnsh({4: (0, 1), 6: 0x10_0000, 8: 0x40}))
        found = split.bnsh_shaders(bytes(d), 0, 'x')
        self.assertEqual([st for _, st, _, _ in found], ['ps'])

    def test_mesh_variation_gives_a_triple(self):
        found = split.bnsh_shaders(bnsh({4: (0, 1), 7: (6, 2), 8: (7, 3)}), 0, 'x')
        key = {st: split.skey(c, code) for _, st, c, code in found}
        self.assertEqual(split.var_pairs(found), {(key['ts'], key['ms'], key['ps'], 'mesh')})
        found = split.bnsh_shaders(bnsh({4: (0, 1), 8: (7, 3)}), 0, 'x')   # a mesh shader with no task shader
        self.assertEqual(split.var_pairs(found), {('-', key['ms'], key['ps'], 'mesh')})

    def test_mesh_program_of_an_agl_file(self):
        """BAHS records of kind 4 and 5 are a program's task and mesh binaries: its n-th fragment, task and mesh binary are one variation, here two with a task and one without"""
        kinds = [1, 4, 5, 1, 4, 5, 1, 5]
        arr = 0x20; d = bytearray(0x400)
        struct.pack_into('<II', d, arr, 8 + 0x20 * len(kinds), len(kinds))
        for i, k in enumerate(kinds): struct.pack_into('<II', d, arr + 8 + 0x20 * i, 0x20, k)
        progs = arr + 8 + 0x20 * len(kinds)
        struct.pack_into('<II', d, progs, 8 + 16, 1); struct.pack_into('<IIII', d, progs + 8, 16, 0, 0x32, 0)
        entries = [f'y#BAHS@{arr + 0x18 + 0x20 * i:#x}' for i in range(len(kinds))]
        split.bahs_variations(bytes(d), 'y', entries)
        stage = {1: ('ps', 0), 4: ('ts', 2), 5: ('ms', 3)}
        found = [(e, stage[k][0], ctl(stage[k][1], 1), bytes([i])) for i, (e, k) in enumerate(zip(entries, kinds))]
        key = [split.skey(c, code) for _, _, c, code in found]
        self.assertEqual(split.var_pairs(found), {(key[1], key[2], key[0], 'mesh'), (key[4], key[5], key[3], 'mesh'), ('-', key[7], key[6], 'mesh')})

    def test_six_stages_as_before(self):
        found = split.bnsh_shaders(bnsh({0: (0, 4), 4: (0, 5)}), 0, 'x')
        self.assertEqual([st for _, st, _, _ in found], ['vs', 'ps'])


if __name__ == '__main__':
    unittest.main()
