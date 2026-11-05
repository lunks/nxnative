#!/usr/bin/env python3
"""Ampere texture handles through the offline translator (dmsh_xlate, arch sm86), in the encodings of Star Fox's pixel shader 0396b3632ac7ea61 (here in a compute shader, the
result stored to shared memory so that it is kept): a handle ORed together under a guard that VOTEU computes, whose phi must not make the bare sampler word a texture of its own
(fp_t_cb2_170, "has no resolved slot number": 62 of the game's pixel shaders did not load); TXD's handle in a uniform register (0xf6c); and NVN2 compute's separate texture and
sampler entries in c[0] (1002069a331ceded). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import re, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_alu import STORE, Alu, inst, s2r_tid, waiting
from test_dmsh_ampere_mem import EXIT


def uldc(ur, bank, offset): return (0x000fe20000000800, 0x7ab9 | ur << 16 | offset << 38 | bank << 54)   # ULDC URd, c[bank][offset] (the game's +0x5690 with its fields)
def with_rd(i, rd): return (i[0], i[1] & ~(0xff << 16) | rd << 16)
def with_ur(i, ur): return (i[0], i[1] & ~(0x3f << 40) | ur << 40)   # a bindless TEX's handle register (40..46)


ISETP = (0x000fe20003f06070, 0x000000254800720c)   # ISETP.GE.U32.AND P0, PT, R72, R37, PT (+0x56c0)
VOTEU = (0x000fe20000000000, 0x00000000003f7886)   # VOTEU.ALL URZ, UP0, P0 (+0x5710)
OR_GUARDED = (0x000fc8000f8efc3f, 0x0000000b0a0a8292)   # @!UP0 ULOP3.LUT UR10, UR10, UR11, URZ, 0xfc (+0x5730): UR10 |= UR11
OR_UR4 = (0x000fe4000f8efc3f, 0x0000000504047292)   # ULOP3.LUT UR4, UR4, UR5, URZ, 0xfc (+0x5620)
TEX_GUARDED = with_rd((0x000142000c9e0f30, 0x30000a35342e8f60), 6)   # @!P0 TEX.LZ R6, R52, UR10, 2D (+0x5760; there R46)
TXD = with_rd((0x004142000c1e0f34, 0xa000041c18320f6c | 0x7000), 6)   # TXD R6, R24, R28, UR4, 2D array (+0x3260 unguarded; there @P0 R50)


class GuardedHandle(Alu):
    """UR10 holds the separate sampler word (c2 0x5c0, sampler slot 11) and takes the texture word (c2 0x248, separate texture slot 28) only in a warp where some thread samples
    (@!UP0 after VOTEU.ALL UP0, P0); the read runs @!P0. Its handle is phi(sampler word, sampler | texture), and the sampler word alone names no texture"""
    def msl(self):
        return self.kept(uldc(10, 2, 0x5c0), uldc(11, 2, 0x248), ISETP, VOTEU, OR_GUARDED, TEX_GUARDED, srcs=(72, 37))

    def test_the_read_takes_the_texture_and_sampler_words(self):
        msl = self.msl()
        self.assertIn('cp_t_cb196610_105C0092', msl)   # the texture word 0x92 (c2 0x248) and the sampler word 0x170 (c2 0x5c0), handle type 1
        self.assertNotIn('cp_t_cb2_170', msl)            # the sampler word as a texture handle: no texture slot holds it

    def test_voteu_leaves_the_thread_predicate(self):   # the vote goes to UP0: written to P0, it took the place of each thread's own P0 at the read and at the store (@!P0 STS)
        msl = self.msl()
        m = re.search(r'bool (_\d+) = simd_all\((_\d+)\);', msl)
        self.assertTrue(m, msl)
        self.assertIn(f'if (!{m.group(2)})', msl)
        self.assertNotIn(f'if (!{m.group(1)})', msl)


class BindlessTxd(Alu):
    """TXD with its handle in a uniform register (0xf6c): the handle comes first, before the coordinates (without it the x coordinate stood in its place and the read was removed)"""
    def test_the_read_takes_its_handle(self):
        msl = self.kept(uldc(4, 2, 0x228), uldc(5, 2, 0x5c0), OR_UR4, TXD)
        self.assertIn('cp_t_cb196610_105C008A', msl)   # separate texture slot 24 (c2 0x228) with sampler slot 11 (c2 0x5c0)
        self.assertIn('gradient2d', msl)


class ComputeSeparateHandles(Alu):
    """NVN2 compute's c[0]: after the combined texture entries come the separate texture (+0xb4) and separate sampler (+0xb5) entries, then the images. Here none of the others: separate
    textures at 0x10 and 0x18, samplers at 0x20 and 0x28, size 0x30 (job nvn2_cs: uniform, storage, texture entries, size, push bytes, separate textures, separate samplers)"""
    TEX = with_ur(with_rd((0x000150000c9e03ff, 0x30000835342c7f60), 6), 4)   # TEX.LZ R6, R52, UR4, 2D (+0x5750; there R44 through UR8)

    def test_mapped_to_the_separate_tables(self):
        code = inst(*s2r_tid(9)) + b''.join(inst(*waiting(i)) for i in (uldc(4, 0, 0x18), uldc(5, 0, 0x28), OR_UR4, self.TEX, STORE)) + inst(*EXIT)
        msl = self.translate(code, shared=0x1000, nvn2_cs=[0, 0, 0, 0x30, 0, 2, 2])
        self.assertIn('cp_t_cb196610_1057005C', msl)   # texture word 0x5c (c2 0x170, separate texture 1), sampler word 0x15c (c2 0x570, separate sampler 1)


if __name__ == '__main__':
    unittest.main()
