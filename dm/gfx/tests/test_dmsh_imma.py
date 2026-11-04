#!/usr/bin/env python3
"""Ampere IMMA (0x237, InstEmitAmpereTensor.cs AImma), the integer matrix multiply-accumulate of Star Fox's character networks, through the offline translator (dmsh_xlate, arch sm86):
a program that loads A, B and C from shared memory, runs one IMMA and stores D is translated, its shared memory routed to a device buffer, and on this Mac's GPU each lane's D is
checked bit for bit against a CPU reference of PTX mma.sync .row.col.s32 on random fragments, which places every element by the PTX ISA's fragment formulas for its shape (64 warps a
form): the game's encodings (IMMA.16832.S8.S8, D over A, B or C), the other signs, .SAT with C near the s32 bounds, m16n8k16 and m8n8k16, and the 4-bit shapes. Needs
DMSH_TEST_XLATE (the translator) like the other translator tests."""
import re, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_mem import EXIT, Translated, inst

METAL = ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance']   # the chain's Mac flags (dmsh_build.py MAC_METAL)
RZ = 255
TID = 30   # the lane's register: S2R R30, SR_TID.X (Mario Kart World's encoding, write barrier 0)
S2R = (0x000e260000002100, 0x0000000000007919 | TID << 16)
# Star Fox's IMMA.16832.S8.S8 (navi_archive_..._b1.bin): R40 = R40 x R46 + RZ (13914e8a0ea86163), D over A; R24 = R44 x R24 + RZ (09e46021dd6eb679), D over B;
# R0 = R48 x R88 + R0 (22f4b97ce367af2d), D over C
GAME_A = (0x002fe20000405cff, 0x0000002e28287237)
GAME_B = (0x001fe40000405cff, 0x000000182c187237)
GAME_C = (0x001fe20000405c00, 0x0000005830007237)
# Shared memory, words a warp: A at 0, B at 128, C at 256 (4 a lane each), D at 384
SHARED = 0x800


def waiting(i): return (i[0] | 0x3f << 52, i[1])   # waits on every scoreboard


def lds(rd, offset, size):   # LDS.<size> Rd, [R30 * 16 + offset]: size 5 (64 bits) or 6 (128), stride 3 (16 bytes)
    return waiting((0x000fe20000000000 | size << 9 | 3 << 14, offset << 40 | TID << 24 | rd << 16 | 0x7984))


def sts(rb, offset, size):   # STS.<size> [R30 * 16 + offset], Rb
    return waiting((0x000fe80000000000 | size << 9 | 3 << 14, offset << 40 | rb << 32 | TID << 24 | 0x7388))


def imma(rd, ra, rb, rc, size=5, a_signed=1, b_signed=1, sat=0, nibbles=0):   # the fields as Mesa NAK's OpImma writes them, the game's control bits
    return (0x001fe20000000000 | 1 << 10 | (size & 1) << 11 | a_signed << 12 | b_signed << 14 | sat << 18 | nibbles << 19 | nibbles << 20 | (size >> 1) << 21 | rc,
            rb << 32 | ra << 24 | rd << 16 | 0x7237)


# The run: argv metallib, m, k, element bits, A signed, B signed, .SAT, C from Rc (0: RZ), C near the s32 bounds. Exit 2: no Metal device
RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
static long long elem(uint32_t w, int i, int bits, int sgn) {   // element i of a register, the lowest first
    uint32_t v = (w >> (bits * i)) & ((1u << bits) - 1);
    return sgn && (v >> (bits - 1)) ? (long long)v - (1 << bits) : (long long)v;
}
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil;
        if (!dev) { printf("no Metal device\n"); return 2; }
        int m = atoi(argv[2]), k = atoi(argv[3]), bits = atoi(argv[4]), as = atoi(argv[5]), bs = atoi(argv[6]), sat = atoi(argv[7]), has_c = atoi(argv[8]), edge = atoi(argv[9]);
        NSData *lib_data = [NSData dataWithContentsOfFile:@(argv[1])];
        id<MTLLibrary> lib = [dev newLibraryWithData:dispatch_data_create(lib_data.bytes, lib_data.length, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT) error:&e];
        if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&e];
        if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
        const int groups = 64, words = 512;
        id<MTLBuffer> mem = [dev newBufferWithLength:groups * words * 4 options:MTLResourceStorageModeShared];
        uint32_t *w = mem.contents; srand(23);
        for (int i = 0; i < groups * words; i++) w[i] = (uint32_t)rand() << 16 ^ (uint32_t)rand();   // every bit pattern, NaNs and denormals among them as the registers pass as floats
        if (edge) for (int g = 0; g < groups; g++) for (int i = 256; i < 384; i++) w[g * words + i] = i % 2 ? 0x7fffffffu - (w[g * words + i] & 0xfffff) : 0x80000000u + (w[g * words + i] & 0xfffff);
        for (int g = 0; g < groups; g++) for (int i = 384; i < 512; i++) w[g * words + i] = 0xdeadbeef;
        uint32_t *in = malloc(groups * words * 4); memcpy(in, w, groups * words * 4);
        id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps]; [ce setBuffer:mem offset:0 atIndex:30];
        [ce dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        long bad = 0, clamped = 0, wrapped = 0;
        for (int grp = 0; grp < groups; grp++) {
            const uint32_t *f = in + grp * words, *o = w + grp * words;
            long long A[16][64], B[64][8];
            for (int L = 0; L < 32; L++) {
                int g = L / 4, t = L % 4, e = 32 / bits;
                for (int i = 0; i < m * k / 32; i++) {   // PTX ISA, "Matrix Fragments for mma.mMnNkK": row and column of a_i
                    int row, col;
                    if (m == 8) { row = g; col = e * t + i; }                                                                     // m8n8k16 .s8, m8n8k32 .s4
                    else if (bits == 8 && k == 16) { row = i < 4 ? g : g + 8; col = 4 * t + (i & 3); }                           // m16n8k16 .s8
                    else if (bits == 8) { row = i < 4 || (i >= 8 && i < 12) ? g : g + 8; col = 4 * t + (i & 3) + (i >= 8 ? 16 : 0); }   // m16n8k32 .s8
                    else if (k == 32) { row = i < 8 ? g : g + 8; col = 8 * t + (i & 7); }                                        // m16n8k32 .s4
                    else { row = i < 8 || (i >= 16 && i < 24) ? g : g + 8; col = 8 * t + (i & 7) + (i >= 16 ? 32 : 0); }        // m16n8k64 .s4
                    A[row][col] = elem(f[4 * L + i / e], i % e, bits, as);
                }
                for (int i = 0; i < k * 8 / 32; i++) {   // b_i: row (k), column g
                    int row;
                    if (bits == 8 && k == 32) row = 4 * t + (i & 3) + (i >= 4 ? 16 : 0);                                         // m16n8k32 .s8
                    else if (bits == 8) row = 4 * t + i;                                                                         // m8n8k16, m16n8k16 .s8
                    else if (k == 64) row = 8 * t + (i & 7) + (i >= 8 ? 32 : 0);                                                 // m16n8k64 .s4
                    else row = 8 * t + i;                                                                                        // m8n8k32, m16n8k32 .s4
                    B[row][g] = elem(f[128 + 4 * L + i / e], i % e, bits, bs);
                }
            }
            for (int L = 0; L < 32; L++) {
                int g = L / 4, t = L % 4;
                for (int i = 0; i < m / 4; i++) {   // c_i / d_i: row g (+ 8 for i >= 2 on m16), column 2t + (i & 1)
                    int row = i < 2 ? g : g + 8, col = 2 * t + (i & 1);
                    long long ref = has_c ? (int32_t)f[256 + 4 * L + i] : 0;
                    for (int kk = 0; kk < k; kk++) ref += A[row][kk] * B[kk][col];
                    int32_t want;
                    if (ref > INT32_MAX || ref < INT32_MIN) { if (sat) { want = ref > 0 ? INT32_MAX : INT32_MIN; clamped++; } else { want = (int32_t)(uint32_t)ref; wrapped++; } }
                    else want = (int32_t)ref;
                    if ((int32_t)o[384 + 4 * L + i] != want) {
                        if (!bad) printf("group %d lane %d d%d: %d, want %d\n", grp, L, i, (int32_t)o[384 + 4 * L + i], want);
                        bad++;
                    }
                }
            }
        }
        printf("%d warps, %ld outputs wrong, %ld clamped, %ld wrapped\n", groups, bad, clamped, wrapped);
        return bad != 0;
    }
}
'''


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode')
class Imma(Translated):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory(prefix='imma_')
        d = Path(cls.dir.name)
        (d / 'run.m').write_text(RUNNER)
        subprocess.run(['clang', '-fobjc-arc', '-O1', str(d / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(d / 'run')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def program(self, mma, m):
        lo = mma[1]
        rd, ra, rb, rc = lo >> 16 & 0xff, lo >> 24 & 0xff, lo >> 32 & 0xff, mma[0] & 0xff
        code = [S2R, lds(ra, 0, 6), lds(rb, 0x200, 5)] + ([lds(rc, 0x400, 6)] if rc != RZ else []) + [mma, sts(rd, 0x600, 6 if m == 16 else 5), EXIT]
        return b''.join(inst(*c) for c in code), rc != RZ

    def msl_on_device(self, mma, m):
        """The translated program, its shared memory a device buffer of its threadgroup's own 512 words (buffer 30)"""
        code, has_c = self.program(mma, m)
        msl = self.translate(code, shared=SHARED)
        msl, n = re.subn(r'threadgroup spvUnsafeArray<uint, 512> (\w+);',
                         r'device spvUnsafeArray<uint, 512>& \1 = *(device spvUnsafeArray<uint, 512>*)(dmsh_mem + 512 * dmsh_group.x);', msl)
        self.assertEqual(n, 1, msl)
        msl = msl.replace('kernel void main0(', 'kernel void main0(device uint* dmsh_mem [[buffer(30)]], uint3 dmsh_group [[threadgroup_position_in_grid]], ', 1)
        return msl, has_c

    def check(self, mma, m, k, bits=8, a_signed=1, b_signed=1, sat=0, edge=False):
        msl, has_c = self.msl_on_device(mma, m)
        with tempfile.TemporaryDirectory(prefix='imma_run_') as d:
            d = Path(d)
            (d / 'k.metal').write_text(msl)
            subprocess.run([*METAL, str(d / 'k.metal'), '-o', str(d / 'k.air')], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(d / 'k.air'), '-o', str(d / 'k.metallib')], check=True)
            r = subprocess.run([str(Path(self.dir.name) / 'run'), str(d / 'k.metallib'), *map(str, (m, k, bits, a_signed, b_signed, sat, int(has_c), int(edge)))],
                               capture_output=True, text=True)
        if r.returncode == 2: self.skipTest(r.stdout.strip())
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return {name: int(n) for n, name in re.findall(r'(\d+) (clamped|wrapped)', r.stdout)}

    def test_game_d_over_a(self):   # C is RZ
        self.check(GAME_A, 16, 32)

    def test_game_d_over_b(self):
        self.check(GAME_B, 16, 32)

    def test_game_d_over_c(self):
        self.check(GAME_C, 16, 32)

    def test_signs(self):
        for a_signed, b_signed in ((0, 1), (1, 0), (0, 0)):
            with self.subTest(a_signed=a_signed, b_signed=b_signed):
                self.check(imma(16, 4, 8, 12, a_signed=a_signed, b_signed=b_signed), 16, 32, a_signed=a_signed, b_signed=b_signed)

    def test_overflow_wraps(self):
        self.assertGreater(self.check(imma(16, 4, 8, 12), 16, 32, edge=True)['wrapped'], 0)

    def test_sat_clamps(self):
        self.assertGreater(self.check(imma(16, 4, 8, 12, sat=1), 16, 32, sat=1, edge=True)['clamped'], 0)
        self.assertGreater(self.check(imma(16, 4, 8, 12, a_signed=0, b_signed=0, sat=1), 16, 32, a_signed=0, b_signed=0, sat=1, edge=True)['clamped'], 0)

    def test_other_8_bit_shapes(self):
        self.check(imma(16, 4, 8, 12, size=4), 16, 16)    # m16n8k16
        self.check(imma(16, 4, 8, 12, size=0), 8, 16)     # m8n8k16

    def test_4_bit_shapes(self):
        for size, m, k in ((2, 8, 32), (5, 16, 32), (6, 16, 64)):
            for a_signed, b_signed in ((1, 1), (0, 1)):
                with self.subTest(size=size, a_signed=a_signed):
                    self.check(imma(16, 4, 8, 12, size=size, a_signed=a_signed, b_signed=b_signed, nibbles=1), m, k, bits=4, a_signed=a_signed, b_signed=b_signed)

    def test_forms_ptx_lacks_are_refused(self):
        for hi, lo in (imma(16, 4, 8, 12, size=6),                                   # m16n8k64 on bytes
                       (imma(16, 4, 8, 12)[0] | 1 << 19, imma(16, 4, 8, 12)[1]),    # 4-bit A, 8-bit B
                       (imma(16, 4, 8, 12)[0] & ~(1 << 10), imma(16, 4, 8, 12)[1])):  # B row-major
            with self.subTest(form=f'{hi:016x}{lo:016x}'):
                with self.assertRaises(AssertionError) as e: self.translate(self.program((hi, lo), 16)[0], shared=SHARED)
                self.assertIn('IMMA size', str(e.exception))


if __name__ == '__main__':
    unittest.main()
