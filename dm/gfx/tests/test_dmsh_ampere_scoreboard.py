#!/usr/bin/env python3
"""Ampere scoreboards through the offline translator (dmsh_xlate, arch sm86): a variable-latency instruction's writes land at the first instruction that waits on its write
barrier, and a guarded one's only in the threads that ran it. Star Fox's pixel shader cc2de495b90ddf70 (+0x8c0..+0xa30) loads its light cluster's two words with
@P0 LDC.64 R40, c[8][R39 + 8] on barrier 1 and @!P0 LDC.64 R40, c[8][R39] on barrier 2, then waits on both at a DEPBAR: the second write cancelled the first, the odd
clusters kept stale registers as their light range and count, and the GPU hung. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import re, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_alu import STORE, s2r_tid, waiting
from test_dmsh_ampere_mem import EXIT, Translated, inst
import test_dmsh_control_flow as control_flow   # its helpers only: a name of its test class here would run its tests again
from test_dmsh_control_flow import PT, bits, bra

LOP3_P0 = (0x000fc4000780c0ff, 0x0000000128ff7812)   # LOP3.LUT P0, RZ, R40, 0x1, RZ, 0xc0, !PT (+0x8c0): P0 = the cluster index is odd
LDC_ODD = (0x0000620000000a00, 0x0200020027280b82)   # @P0 LDC.64 R40, c[8][R39 + 0x8] (+0x9e0), write barrier 1, waits on none
LDC_EVEN = (0x0000a20000000a00, 0x0200000027288b82)  # @!P0 LDC.64 R40, c[8][R39] (+0xa00), write barrier 2, waits on none


def store(reg, offset=8):   # STS.U32 [R9 * 16 + offset], reg, unguarded, after waiting on every barrier
    return (STORE[0] | 0x3f << 52, STORE[1] & ~(0xff << 32) & ~(0xffffff << 40) & ~0xf000 | PT << 12 | reg << 32 | offset << 40)


class GuardedWrites(Translated):
    def test_both_cluster_words_reach_their_threads(self):
        code = b''.join(inst(*s2r_tid(r)) for r in (9, 39, 40)) + b''.join(inst(*i) for i in (waiting(LOP3_P0), LDC_ODD, LDC_EVEN, store(40), store(41, 12), EXIT))
        msl = self.translate(code, shared=0x1000)
        self.assertRegex(msl, r'int (_\d+) = as_type<int>\(_\d+\) \+ 8;\s*uint _\d+ = uint\(int\(uint\(\1\)')   # the odd clusters' load, at +8, is still there
        self.assertEqual(len(re.findall(r'cp_c8\)\._m0', msl)), 4)                                   # two words for each parity
        self.assertEqual(len(re.findall(r'if \(_\d+\)\s*\{\s*_\d+ = _\d+;\s*\}', msl)), 4)                  # each lands in the threads that ran it


class GuardedLastInBlock(Translated):
    """A guarded variable-latency instruction last in its block (the next is a branch target): its skipped path went straight to the next block, past the copies that land
    the block's pending writes, so an earlier unguarded load landed only in the threads that ran the guarded one"""
    @staticmethod
    def ldc(rd, offset, guard=PT, inv=0, barrier=1):   # [@[!]Pguard] LDC Rd, c1[R40 + offset] on that write barrier, waiting on none
        return bits((0, 12, 0xb82), (12, 3, guard), (15, 1, inv), (16, 8, rd), (24, 8, 40), (38, 16, offset), (54, 5, 1), (73, 3, 4), (110, 3, barrier))

    def test_the_earlier_write_lands_on_both_paths(self):
        program = [s2r_tid(9), s2r_tid(40), s2r_tid(6), waiting(LOP3_P0), self.ldc(6, 0), self.ldc(7, 4, guard=0, inv=1, barrier=2), store(6), bra(7, 6, pred=1), EXIT]
        msl = control_flow.ControlFlow.translate(self, program)
        load = re.search(r'float (_\d+) = \(\*spvDescriptorSet0\.cp_c1\)\._m0\[int\(_\d+\)\]\[_\d+\];', msl)
        self.assertTrue(load, msl)
        self.assertRegex(msl, r'_24\[int\(_\d+\)\] = as_type<uint>\(' + load.group(1) + r'\);')   # stored as loaded, not merged with R6's old value on one path


if __name__ == '__main__':
    unittest.main()
