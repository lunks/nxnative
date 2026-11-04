"""insn_census.py and nso_code.py's undecodable count: decoding runs of words (and a large text in shares on every core) gives what a word-by-word sweep gives. Synthetic words only."""
import collections
from pathlib import Path
import random
import re
import struct
import sys
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import capstone
import insn_census
import nso_code

MD = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
TEMPLATES = [(0x91000000, 0x3FFFFF), (0xF9400000, 0x3FFFFF), (0x94000000, 0x3FFFFFF), (0xAA000000, 0x1FFFFF), (0xB4000000, 0xFFFFFF)]   # add, ldr, bl, orr, cbz with random fields
SITES = [0xD40003E1, 0xD53BD040, 0xD51BD040, 0xD53BE020, 0xD53BD060, 0xCB2063FF]   # svc #0x1f, mrs / msr tpidr_el0, mrs cntpct_el0, mrs tpidrro_el0, sub sp, sp, x0


def words(n, seed, junk=0.05):   # n words of code, with junk words that do not decode and the sites the census counts
    rnd = random.Random(seed); out = []
    for _ in range(n):
        r = rnd.random()
        if r < junk: out.append(rnd.getrandbits(32))
        elif r < junk + 0.01: out.append(rnd.choice(SITES))
        else: b, m = rnd.choice(TEMPLATES); out.append(b | rnd.getrandbits(32) & m)
    return bytearray(struct.pack(f'<{n}I', *out))


def word_by_word(text, base, data):   # the census as insn_census.py made it before: one disasm_lite call per word
    c, svcs, sites, total, undecoded, skip = collections.Counter(), collections.Counter(), [], 0, 0, set()
    for s, e in data: skip.update(range(s - base, e - base, 4))
    for off in range(0, len(text) - 3, 4):
        if off in skip: continue
        r = next(MD.disasm_lite(bytes(text[off:off + 4]), base + off), None)
        if r is None: undecoded += 1; continue
        total += 1; addr, _, mn, ops = r; kind = None
        if mn == 'svc': kind = 'svc'; svcs[ops] += 1
        elif mn in ('mrs', 'msr'):
            for reg in ('tpidrro_el0', 'tpidr_el0', 'cntpct_el0', 'cntvct_el0', 'cntfrq_el0', 'cntpctss_el0', 'cntvctss_el0'):
                if reg in ops: kind = '%s %s' % (mn, reg); break
        if kind is None and insn_census.X18.search(ops): kind = 'x18 use'
        if kind is None and mn == 'sub' and re.fullmatch(r'sp, sp, x\d+', ops): kind = 'sub sp, sp, xN'
        if kind: c[kind] += 1; sites.append((addr, kind, mn, ops))
    return total, undecoded, list(c.items()), list(svcs.items()), sites


class Census(unittest.TestCase):
    def check(self, text, data, base=0):
        ranges = insn_census.code_ranges(len(text) & ~3, data, base)
        total, undecoded, c, svcs, sites = insn_census.census(MD, memoryview(text), base, ranges)
        self.assertEqual((total, undecoded, list(c.items()), list(svcs.items()), sites), word_by_word(text, base, data))   # Counter order too: most_common breaks ties by it

    def test_runs_of_words_and_data_blocks(self):
        text = words(0x30000, 1, junk=0.2)
        self.check(text, [])
        self.check(text, [(0x8, 0x80), (0x1000, 0x23000), (0x2f000, 0x30000)])   # the NRO header, a stretch over several chunks, the end
        self.check(text + b'\x01\x02', [(0, 0x10000)])   # a text that does not end on a word

    def test_shares_on_every_core_give_one_sweep(self):
        text = words(0x40000, 2)
        with mock.patch.object(insn_census, 'PARALLEL_MIN', 0), mock.patch.object(insn_census.os, 'cpu_count', return_value=4):
            self.check(text, [(0x20000, 0x31000)], base=0)

    def test_ranges_leave_out_the_data_blocks_in_chunks(self):
        r = insn_census.code_ranges(0x30000, [(0x100, 0x200), (0x21000, 0x22000)], 0)
        self.assertEqual(r[:2], [(0, 0x100), (0x200, 0x10200)])
        self.assertTrue(all(e - s <= nso_code.CHUNK for s, e in r))
        self.assertEqual(sum(e - s for s, e in r), 0x30000 - 0x100 - 0x1000)


class Undecodable(unittest.TestCase):
    def test_runs_count_as_word_by_word(self):
        for seed, junk in ((3, 0.0), (4, 0.3), (5, 1.0)):
            img = words(0x14000, seed, junk)
            for s, e in ((0, len(img)), (0x40, len(img) - 0x40), (0x100, 0x100)):
                want = sum(1 for a in range(s, e, 4) if next(MD.disasm_lite(bytes(img[a:a + 4]), a), None) is None)
                self.assertEqual(nso_code.undecodable(img, s, e, MD), want)
                self.assertEqual(nso_code.undecodable(bytes(img), s, e, MD), want)   # nso_code.py on its own reads a file's bytes


if __name__ == '__main__':
    unittest.main()
