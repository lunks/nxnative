#!/usr/bin/env python3
"""Ampere ALU instructions through the offline translator (dmsh_xlate, arch sm86), on registers the translator cannot fold, their result stored to shared memory so
that it is kept: BREV, and HADD2 / HMUL2 with an immediate (test_dmsh_ampere_mem.py's Pokopia class has those two on constants, a negative immediate's sign, F2FP.MERGE_C,
F2I from a half, UP2UR, MEMBAR and ERRBAR). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_mem import EXIT, Translated, inst

STORE = (0x000fe8000000c800, 0x0000080609008388)   # STS.U32 [R9 * 16 + 8], R6 (Mario Kart World f94552a2a2bff505): the sink, R9 the thread index


def waiting(i): return (i[0] | 0x3f << 52, i[1])   # the instruction waits on every scoreboard (control bits 116..121): it reads what an S2R before it wrote
def s2r_tid(rd): return (0x000e260000002100, 0x0000000000007919 | rd << 16)   # S2R Rd, SR_TID.X (Mario Kart World's encoding, write barrier 0)


class Alu(Translated):
    def kept(self, *code, srcs=()):
        """code between S2R R9 (and each register of srcs) = the thread index and the store of R6"""
        pre = b''.join(inst(*s2r_tid(r)) for r in (9, *srcs))
        body = b''.join(inst(*waiting(i)) for i in code)
        return self.translate(pre + body + inst(*waiting(STORE)) + inst(*EXIT), shared=0x1000)


class Brev(Alu):
    BREV = (0x001e240000000000, 0x0000001300067301)   # BREV R6, R19 (Pokémon Pokopia 6e8db04c25a53d8c, whose destination is R3)

    def test_reverses_the_bits(self):
        self.assertIn('reverse_bits', self.kept(self.BREV, srcs=(19,)))


class Bmsk(Alu):
    """BMSK R6, R4, R5 (0x01b, form 1: two registers; Star Fox's pixel shader encodes it as R5, R4, R5): ((1 << R5) - 1) << R4, clamped to 32 without .W (bit 75)"""
    BMSK = (0x000fe40000000000, 0x000000050406721b)

    def test_builds_the_mask(self):
        msl = self.kept(self.BMSK, srcs=(4, 5))
        self.assertRegex(msl, r'= 1 << ')            # the width's ones
        self.assertRegex(msl, r'\? \w+ : \(-1\)')   # all ones for a width of 32 or more
        self.assertRegex(msl, r'< 32u')               # a position of 32 or more gives 0

    def test_wrap_masks_the_fields(self):
        msl = self.kept((self.BMSK[0] | 1 << 11, self.BMSK[1]), srcs=(4, 5))   # .W
        self.assertRegex(msl, r'& 31')


class HalfImmediate(Alu):
    """HADD2 / HMUL2 Rd, Ra, imm (form 2): the second source is the immediate at bits 32..63, not the register field at 64 that HFMA2's B uses in that form. Pokopia's
    blur 85446a6753300fc8 steps its loop with HADD2 R6, R6, 2.0h; reading R0 there, the loop never ended and the GPU hung"""
    HADD2 = (0x000fe20000000800, 0x4000400006067430)   # HADD2 R6, R6, 2, 2 (field 64 = R0)
    HMUL2 = (HADD2[0], HADD2[1] & ~0xfff | 0x432)      # HMUL2 R6, R6, 2, 2: the same fields (no game encoding met)

    def test_hadd2_adds_the_immediate(self):
        msl = self.kept(self.HADD2, srcs=(6,))
        self.assertIn('spvFAdd', msl)
        self.assertRegex(msl, r'\b2\.0\b')   # read from R0 (never written) it was 0, and the add folded away

    def test_hmul2_multiplies_by_the_immediate(self):
        msl = self.kept(self.HMUL2, srcs=(6,))
        self.assertIn('spvFMul', msl)
        self.assertRegex(msl, r'\b2\.0\b')


class CompareFtz(Alu):
    """FSETP / FSET .FTZ (bit 80): a denormal operand compares as zero, as on NVIDIA; Apple's GPUs keep denormals. Mario Kart World's deferred lighting 635061c90d75a8d1
    normalises its four depth-weighted samples by 1 / sum only where FSETP.NEU.FTZ says sum != 0: a denormal sum passed, its reciprocal overflowed, and the NaN came out
    of a min(x, 1.0) as a fully lit dot (sparkles at thin far geometry)"""
    FSETP = (0x000fca0003f1d000, 0x800000141300720b)   # FSETP.NEU.FTZ.AND P0, PT, R19, -R20, PT (the game's +0x10a0, there on R35 and -R10)
    FSEL = (0x000fe40000000000, 0x0000001413067208)    # FSEL R6, R19, R20, P0: keeps the predicate
    FLUSH = '1.1754943508222875'                       # FLT_MIN, the bound under which an operand is flushed

    def test_ftz_flushes_denormal_operands(self):
        self.assertIn(self.FLUSH, self.kept(self.FSETP, self.FSEL, srcs=(19, 20)))

    def test_without_ftz_compares_as_is(self):
        self.assertNotIn(self.FLUSH, self.kept((self.FSETP[0] & ~(1 << 16), self.FSETP[1]), self.FSEL, srcs=(19, 20)))


class HalfSecond(Translated):
    """Two-source half ops use the operand at bit 32, including constants.

    A shared store keeps the result live; R0 deliberately remains undefined.
    """
    def translate_with_live_result(self, opcode, second_operand):
        thread_id = inst(0x000e260000002100, 0x0000000000067919)
        operand_bits = second_operand << 32
        source_register = 6 << 24
        destination_register = 6 << 16
        half_instruction = operand_bits | source_register | destination_register | 0x7000 | opcode
        half = inst(0x03ffe20000000800, half_instruction)
        stored_register = 6 << 32
        zero_base_register = 255 << 24
        store = inst(0x03ffe80000000800, stored_register | zero_base_register | 0x7388)
        shader = thread_id + half + store + inst(*EXIT)
        return self.translate(shader, shared=256)

    def test_immediate_is_used_by_add_and_multiply(self):
        for opcode in (0x430, 0x432):
            with self.subTest(opcode=opcode):
                self.assertRegex(self.translate_with_live_result(opcode, 0x40004000), r'\b2\.0\b')

    def test_constant_buffer_second_operand_is_read(self):
        for opcode in (0x630, 0x632):
            with self.subTest(opcode=opcode):
                constant_buffer_operand = (3 << 22) | 0x20
                self.assertIn('cp_c3', self.translate_with_live_result(opcode, constant_buffer_operand))


if __name__ == '__main__':
    unittest.main()
