using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitAluHelper;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    // Ampere warp-cooperative matrix instructions (HMMA, IMMA, LDSM, MOVM) and packed-half compares, translated per thread. A matrix fragment is spread over the warp's 32
    // lanes in NVIDIA's documented layouts (PTX mma.m16n8k16 / m16n8k32 / ldmatrix / movmatrix): lane L holds rows L / 4 (and L / 4 + 8) at columns 2 (L % 4) and + 1. Each
    // lane gathers what it needs from the lanes that hold it with subgroup shuffles (Metal SIMD-groups are 32 wide, as the warp), and computes its own part. Field
    // positions: Mesa NAK sm70_encode.rs (OpHmma, OpImma, OpLdsm, OpMovm, OpHSetP2, OpHSet2, OpHMnMx2).
    static partial class InstEmit
    {
        public static void RegisterAmpereTensor(AmpereRegistrar x)
        {
            const InstProps N = InstProps.None;

            x(0x23c, InstName.Hfma2, AHmma, N);
            x(0x237, InstName.Idp, AImma, N);
            x(0x83b, InstName.Lds, ALdsm, N);
            x(0x23a, InstName.Mov, AMovm, N);
            x(0x3a1, InstName.Vote, AMatch, N);
            x(0x3c4, InstName.Vote, ARedux, N, writesRd: false);
        }

        // REDUX URd, Ra (Mesa NAK OpRedux): Ra of the warp's active lanes reduced into a uniform register; 78..81: and, or, xor, sum, min, max; 73: signed min / max
        public static void ARedux(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int op = x.Bits(78, 81);
            bool signed = x.Bit(73);

            if (op > 5)
            {
                throw AFail(context, $"REDUX operation {op}");
            }

            Operand src = AGpr(x.Ra);
            Operand active = context.Ballot(Const(IrConsts.True), 0);
            Operand acc = null;

            for (int lane = 0; lane < 32; lane++)
            {
                Operand v = context.Shuffle(src, Const(lane));
                Operand r = acc == null ? v : op switch
                {
                    0 => context.BitwiseAnd(acc, v),
                    1 => context.BitwiseOr(acc, v),
                    2 => context.BitwiseExclusiveOr(acc, v),
                    3 => context.IAdd(acc, v),
                    4 => signed ? context.IMinimumS32(acc, v) : context.IMinimumU32(acc, v),
                    _ => signed ? context.IMaximumS32(acc, v) : context.IMaximumU32(acc, v),
                };
                Operand on = context.ICompareNotEqual(context.BitwiseAnd(context.ShiftRightU32(active, Const(lane)), Const(1)), Const(0));

                // The first active lane starts the reduction; lanes that are not active leave it as it is
                acc = acc == null ? context.ConditionalSelect(on, v, Const(op == 0 ? -1 : op == 4 ? (signed ? int.MaxValue : -1) : op == 5 ? (signed ? int.MinValue : 0) : 0))
                                  : context.ConditionalSelect(on, r, acc);
            }

            if (x.Rd != RegisterConsts.UniformZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Uniform), acc);
            }
        }

        // MATCH.ANY / .ALL Rd, Ra (Mesa NAK OpMatch: .U64 73, .ANY 79, the predicate of .ALL 81..84): the active lanes whose value (Ra, or Ra and Ra + 1) equals this lane's,
        // as a lane mask; .ALL gives the active mask only when every active lane agrees, and 0 otherwise, and sets the predicate to whether they do
        public static void AMatch(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u64 = x.Bit(73), any = x.Bit(79);
            Operand lo = AGpr(x.Ra), hi = u64 ? AGpr(x.Ra + 1) : null;
            Operand active = context.Ballot(Const(IrConsts.True), 0);
            Operand same = Const(0);

            for (int lane = 0; lane < 32; lane++)
            {
                Operand eq = context.ICompareEqual(context.Shuffle(lo, Const(lane)), lo);

                if (u64)
                {
                    eq = context.BitwiseAnd(eq, context.ICompareEqual(context.Shuffle(hi, Const(lane)), hi));
                }

                same = context.BitwiseOr(same, context.ConditionalSelect(eq, Const(1 << lane), Const(0)));
            }

            same = context.BitwiseAnd(same, active);
            Operand all = context.ICompareEqual(same, active);
            Operand res = any ? same : context.ConditionalSelect(all, active, Const(0));

            if (x.Rd < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Gpr), res);
            }

            if (!any)
            {
                ASetPred(context, x.Bits(81, 84), all);
            }
        }

        public static void RegisterAmpereHalfCompare(AmpereAluRegistrar alu)
        {
            alu(0x034, InstName.Hsetp2, AHsetp2, writesRd: false);
            alu(0x033, InstName.Hset2, AHset2);
            alu(0x040, InstName.Hadd2, AHmnmx2);
        }

        private static Operand ALaneId(EmitterContext context) => EmitLoadSubgroupLaneId(context);

        private static Operand AGpr(int r) => r >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(r, RegisterType.Gpr);

        // The low (0) or high (1) half of a packed pair, as a float
        private static Operand AHalf(EmitterContext context, Operand packed, int half)
        {
            return half == 0 ? context.UnpackHalf2x16Low(packed) : context.UnpackHalf2x16High(packed);
        }

        // HMMA.16816 / .1688 Rd, Ra, Rb, Rc: D (16 x 8) = A (16 x 16, or 16 x 8) * B (16 x 8, or 8 x 8) + C. Size 75 / 78 (0 m16n8k8, 1 m16n8k16), F32 accumulator and result
        // (76; else packed halves), source type 82..84 (0 f16). Lane L, g = L / 4, t = L % 4: A regs (row g / g + 8) x (columns 2t, 2t + 1; + 8 in the second half of k), B regs
        // (k 2t, 2t + 1; + 8) x (column g), C / D (row g / g + 8) x (columns 2t, 2t + 1).
        public static void AHmma(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int size = x.Bits(75, 76) | (x.Bits(78, 79) << 1);
            bool f32 = x.Bit(76);

            if (size > 1 || x.Bits(82, 84) != 0)
            {
                throw AFail(context, $"HMMA size {size} source type {x.Bits(82, 84)}");
            }

            // nxnative: HMMA.16816 with an f16 accumulator in a compute shader (agl's upscaler networks: every HMMA of Donkey Kong Bananza's agl_ndsr.pck) becomes
            // MatrixMultiplyAdd, then a call of dmsh_hmma16816_f16 (TensorPass), which the shader build turns into simdgroup_matrix code. Emulated per lane, it took 32
            // shuffles and 64 FMAs a lane per HMMA, 2.3-6.5 ms a frame for the upscaler's dispatch on an M4 Pro, five times the simdgroup_matrix version's cost
            ShaderDefinitions defs = context.TranslatorContext.Definitions;

            if (size == 1 && !f32 && defs.Stage == ShaderStage.Compute)
            {
                int threads = defs.ComputeLocalSizeX * defs.ComputeLocalSizeY * defs.ComputeLocalSizeZ;
                int memoryId = context.ResourceManager.GetHmmaScratchMemoryId(System.Math.Max(1, (threads + 31) / 32));

                Operand tidX = context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(0));
                Operand tidY = context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(1));
                Operand tidZ = context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(2));
                Operand flat = context.IAdd(tidX, context.IAdd(context.IMultiply(tidY, Const(defs.ComputeLocalSizeX)),
                                                              context.IMultiply(tidZ, Const(defs.ComputeLocalSizeX * defs.ComputeLocalSizeY))));
                Operand block = context.ShiftLeft(context.ShiftRightU32(flat, Const(5)), Const(8));   // the subgroup's 256 words (Metal SIMD-groups are 32 wide, as the warp)

                (Operand d0, Operand d1) = context.Add(Instruction.MatrixMultiplyAdd, (Local(), Local()), Const(memoryId), ALaneId(context), block,
                    AGpr(x.Ra), AGpr(x.Ra + 1), AGpr(x.Ra + 2), AGpr(x.Ra + 3), AGpr(x.Rb), AGpr(x.Rb + 1), AGpr(x.Rc), AGpr(x.Rc + 1));

                if (x.Rd < RegisterConsts.RegisterZeroIndex)
                {
                    context.Copy(Register(x.Rd, RegisterType.Gpr), d0);
                }

                if (x.Rd + 1 < RegisterConsts.RegisterZeroIndex)
                {
                    context.Copy(Register(x.Rd + 1, RegisterType.Gpr), d1);
                }

                return;
            }

            int k = size == 1 ? 16 : 8;
            int aRegs = k / 4;   // 4 (k16) or 2 (k8): (row g, k lo), (row g + 8, k lo), (row g, k hi), (row g + 8, k hi)
            int bRegs = k / 8;   // 2 or 1

            Operand lane = ALaneId(context);
            Operand g = context.ShiftRightU32(lane, Const(2));
            Operand t = context.BitwiseAnd(lane, Const(3));

            // A rows g and g + 8, every k: from lanes 4g + j (j = 0..3), which hold k = 2j, 2j + 1 (+ 8)
            Operand[,] a = new Operand[2, 16];

            for (int j = 0; j < 4; j++)
            {
                Operand src = context.IAdd(context.ShiftLeft(g, Const(2)), Const(j));

                for (int r = 0; r < aRegs; r++)
                {
                    Operand v = context.Shuffle(AGpr(x.Ra + r), src);
                    int row = r & 1, kb = (r >> 1) * 8 + 2 * j;
                    a[row, kb] = AHalf(context, v, 0);
                    a[row, kb + 1] = AHalf(context, v, 1);
                }
            }

            // B columns 2t and 2t + 1, every k: from lanes 4 col + j, which hold k = 2j, 2j + 1 (+ 8)
            Operand[,] b = new Operand[2, 16];

            for (int c = 0; c < 2; c++)
            {
                Operand col = context.IAdd(context.ShiftLeft(t, Const(1)), Const(c));

                for (int j = 0; j < 4; j++)
                {
                    Operand src = context.IAdd(context.ShiftLeft(col, Const(2)), Const(j));

                    for (int r = 0; r < bRegs; r++)
                    {
                        Operand v = context.Shuffle(AGpr(x.Rb + r), src);
                        int kb = r * 8 + 2 * j;
                        b[c, kb] = AHalf(context, v, 0);
                        b[c, kb + 1] = AHalf(context, v, 1);
                    }
                }
            }

            // C: (row, column) of this lane's four outputs
            Operand[] acc = new Operand[4];

            for (int i = 0; i < 4; i++)
            {
                int row = i >> 1, c = i & 1;
                acc[i] = f32 ? AGpr(x.Rc + i) : AHalf(context, AGpr(x.Rc + row), c);

                for (int kk = 0; kk < k; kk++)
                {
                    acc[i] = context.FPFusedMultiplyAdd(a[row, kk], b[c, kk], acc[i]);
                }
            }

            if (f32)
            {
                for (int i = 0; i < 4 && x.Rd + i < RegisterConsts.RegisterZeroIndex; i++)
                {
                    context.Copy(Register(x.Rd + i, RegisterType.Gpr), acc[i]);
                }
            }
            else
            {
                for (int row = 0; row < 2 && x.Rd + row < RegisterConsts.RegisterZeroIndex; row++)
                {
                    context.Copy(Register(x.Rd + row, RegisterType.Gpr), context.PackHalf2x16(acc[row * 2], acc[row * 2 + 1]));
                }
            }
        }

        // IMMA Rd, Ra, Rb, Rc: D (m x 8, s32) = A (m x k) * B (k x 8) + C in integers, PTX mma.sync .row.col.s32 on .s8 / .u8 (or .s4 / .u4) sources. Shape at 75 and 85..86
        // (0 m8n8k16, 2 m8n8k32, 4 m16n8k16, 5 m16n8k32, 6 m16n8k64), A and B signed (76, 78), 4-bit elements (83, 84), .SAT (82: PTX .satfinite, the sum clamped to the s32 range;
        // else it wraps); 74 is B column-major, PTX's only form. Lane L, g = L / 4, t = L % 4, e elements a register (4 bytes or 8 nibbles, element i at bits 32 / e * i): A regs
        // (row g, and g + 8 on m16, alternating) x (k e t .. e t + e - 1, + 4e for each further pair), B regs (k the same, + 4e for each further one) x (column g), C / D (row g / g + 8)
        // x (columns 2t, 2t + 1).
        // nxnative: Star Fox's character networks (navi_archive_2604ki8_mixedtrqlrki8_mixedtrqllrki8_nvn2_glsl_b1.bin, 84 of its 165 compute shaders, 3,550 IMMAs, up to 216 a
        // shader) use IMMA.16832.S8.S8 alone, without .SAT: 001fe40000405cff000000182c187237 is R24, R44, R24, RZ (09e46021dd6eb679), and D lies over A, B or C in others (every
        // source is gathered before D is written). Each product fits in 17 bits and a dot product of k of them in s32, so the sum is exact and only C + sum can overflow.
        public static void AImma(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int size = x.Bits(75, 76) | (x.Bits(85, 87) << 1);
            int bits = x.Bit(83) ? 4 : 8;
            bool aSigned = x.Bit(76), bSigned = x.Bit(78), sat = x.Bit(82);

            // The shapes and element sizes PTX has: 8 bits on m8n8k16, m16n8k16 and m16n8k32; 4 bits on m8n8k32, m16n8k32 and m16n8k64; A and B of one size
            if (!x.Bit(74) || x.Bit(83) != x.Bit(84) || (size, bits) is not ((0, 8) or (4, 8) or (5, 8) or (2, 4) or (5, 4) or (6, 4)))
            {
                throw AFail(context, $"IMMA size {size}, {(x.Bit(83) ? 4 : 8)}-bit A, {(x.Bit(84) ? 4 : 8)}-bit B, B column-major {x.Bit(74)}");
            }

            int rows = size >= 4 ? 2 : 1;                                     // row blocks of 8: g, and g + 8 on m16
            int k = size switch { 0 or 4 => 16, 2 or 5 => 32, _ => 64 };
            int e = 32 / bits;                                                  // elements a register
            int span = 4 * e;                                                   // the k one register covers over a lane group of 4
            int aRegs = rows * k / span;
            int bRegs = k / span;

            Operand lane = ALaneId(context);
            Operand g = context.ShiftRightU32(lane, Const(2));
            Operand t = context.BitwiseAnd(lane, Const(3));

            Operand Element(Operand v, int i, bool signed)
            {
                return signed ? context.BitfieldExtractS32(v, Const(bits * i), Const(bits)) : context.BitfieldExtractU32(v, Const(bits * i), Const(bits));
            }

            // A rows g (and g + 8), every k: from lanes 4g + j (j = 0..3), which hold k = e j .. e j + e - 1 (+ span for each further register or pair)
            Operand[,] a = new Operand[rows, k];

            for (int j = 0; j < 4; j++)
            {
                Operand src = context.IAdd(context.ShiftLeft(g, Const(2)), Const(j));

                for (int r = 0; r < aRegs; r++)
                {
                    Operand v = context.Shuffle(AGpr(x.Ra + r), src);
                    int row = r % rows, kb = r / rows * span + e * j;

                    for (int i = 0; i < e; i++)
                    {
                        a[row, kb + i] = Element(v, i, aSigned);
                    }
                }
            }

            // B columns 2t and 2t + 1, every k: from lanes 4 col + j
            Operand[,] b = new Operand[2, k];

            for (int c = 0; c < 2; c++)
            {
                Operand col = context.IAdd(context.ShiftLeft(t, Const(1)), Const(c));

                for (int j = 0; j < 4; j++)
                {
                    Operand src = context.IAdd(context.ShiftLeft(col, Const(2)), Const(j));

                    for (int r = 0; r < bRegs; r++)
                    {
                        Operand v = context.Shuffle(AGpr(x.Rb + r), src);
                        int kb = r * span + e * j;

                        for (int i = 0; i < e; i++)
                        {
                            b[c, kb + i] = Element(v, i, bSigned);
                        }
                    }
                }
            }

            // C / D: (row, column) of this lane's outputs, 2 rows x 2 columns on m16, 1 x 2 on m8
            Operand[] d = new Operand[2 * rows];

            for (int i = 0; i < d.Length; i++)
            {
                int row = i >> 1, c = i & 1;
                Operand sum = context.IMultiply(a[row, 0], b[c, 0]);

                for (int kk = 1; kk < k; kk++)
                {
                    sum = context.IAdd(sum, context.IMultiply(a[row, kk], b[c, kk]));
                }

                Operand acc = AGpr(x.Rc + i);
                Operand res = context.IAdd(acc, sum);

                if (sat)
                {
                    // Signed overflow: C and the sum of one sign, the result of the other; then the bound on C's side
                    Operand overflow = context.ICompareLess(context.BitwiseAnd(context.BitwiseExclusiveOr(acc, res), context.BitwiseExclusiveOr(sum, res)), Const(0));
                    Operand bound = context.ConditionalSelect(context.ICompareLess(acc, Const(0)), Const(int.MinValue), Const(int.MaxValue));
                    res = context.ConditionalSelect(overflow, bound, res);
                }

                d[i] = res;
            }

            for (int i = 0; i < d.Length && x.Rd + i < RegisterConsts.RegisterZeroIndex; i++)
            {
                context.Copy(Register(x.Rd + i, RegisterType.Gpr), d[i]);
            }
        }

        // LDSM.16.M88.n / .MT88.n Rd, [Ra + UR + offset]: n (72..74: 1, 2, 4) 8 x 8 matrices of halves from shared memory; lanes 8i .. 8i + 7 give the addresses of
        // matrix i's rows. Lane L receives, of each matrix, row L / 4 at columns 2 (L % 4), + 1 (one register); transposed (78..80 = 1), column L / 4 at rows 2 (L % 4), + 1.
        public static void ALdsm(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int count = 1 << x.Bits(72, 74);
            int mode = x.Bits(78, 80);

            if (count > 4 || mode > 1)
            {
                throw AFail(context, $"LDSM mode {mode} count {count}");
            }

            Operand addr = context.IAdd(ALocalAddress(context, x, 32, stride: false), Const((int)x.SignedField(40, 64)));
            Operand lane = ALaneId(context);
            Operand g = context.ShiftRightU32(lane, Const(2));
            Operand t = context.BitwiseAnd(lane, Const(3));
            int id = context.ResourceManager.SharedMemoryId;
            Operand[] results = new Operand[count];

            for (int m = 0; m < count; m++)
            {
                if (mode == 0)
                {
                    Operand rowAddr = context.Shuffle(addr, context.IAdd(g, Const(8 * m)));
                    Operand byteOffset = context.IAdd(rowAddr, context.ShiftLeft(t, Const(2)));
                    results[m] = context.Load(StorageKind.SharedMemory, id, context.ShiftRightU32(byteOffset, Const(2)));
                }
                else
                {
                    Operand[] h = new Operand[2];

                    for (int j = 0; j < 2; j++)
                    {
                        Operand row = context.IAdd(context.ShiftLeft(t, Const(1)), Const(j));
                        Operand rowAddr = context.Shuffle(addr, context.IAdd(row, Const(8 * m)));
                        Operand byteOffset = context.IAdd(rowAddr, context.ShiftLeft(g, Const(1)));
                        Operand word = context.Load(StorageKind.SharedMemory, id, context.ShiftRightU32(byteOffset, Const(2)));
                        h[j] = context.BitfieldExtractU32(word, context.ShiftLeft(context.BitwiseAnd(byteOffset, Const(2)), Const(3)), Const(16));
                    }

                    results[m] = context.BitwiseOr(h[0], context.ShiftLeft(h[1], Const(16)));
                }
            }

            for (int m = 0; m < count && x.Rd + m < RegisterConsts.RegisterZeroIndex; m++)
            {
                context.Copy(Register(x.Rd + m, RegisterType.Gpr), results[m]);
            }
        }

        // MOVM.16.MT88 Rd, Ra: the 8 x 8 matrix of halves the warp holds (lane L: row L / 4, columns 2 (L % 4), + 1), transposed into the same layout
        public static void AMovm(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bits(78, 80) != 0)
            {
                throw AFail(context, $"MOVM mode {x.Bits(78, 80)}");
            }

            Operand lane = ALaneId(context);
            Operand r = context.ShiftRightU32(lane, Const(2));
            Operand t = context.BitwiseAnd(lane, Const(3));
            Operand src = AGpr(x.Ra);
            Operand[] h = new Operand[2];

            for (int j = 0; j < 2; j++)
            {
                // out (r, c = 2t + j) = in (c, r): held by lane 4c + r / 2, half r % 2
                Operand c = context.IAdd(context.ShiftLeft(t, Const(1)), Const(j));
                Operand v = context.Shuffle(src, context.IAdd(context.ShiftLeft(c, Const(2)), context.ShiftRightU32(r, Const(1))));
                h[j] = context.BitfieldExtractU32(v, context.ShiftLeft(context.BitwiseAnd(r, Const(1)), Const(4)), Const(16));
            }

            if (x.Rd < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Gpr), context.BitwiseOr(h[0], context.ShiftLeft(h[1], Const(16))));
            }
        }

        // HSETP2 Pd0, Pd1, a, b, P: each half's comparison (76..80) combined with P (87..90) by the operation at 69..71; with .H_AND (71) the two halves' results ANDed
        // into Pd0 and its negation into Pd1
        public static void AHsetp2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand[] a, Operand[] b) = AHalfSecond(context, x);

            Operand lo = GetFPComparison(context, (FComp)x.Bits(76, 80), a[0], b[0]);
            Operand hi = GetFPComparison(context, (FComp)x.Bits(76, 80), a[1], b[1]);

            if (x.Bit(71))
            {
                lo = context.BitwiseAnd(lo, hi);
                hi = context.BitwiseNot(lo);
            }

            Operand p = APred(context, x, 87, 90);
            BoolOp bop = (BoolOp)x.Bits(69, 71);

            ASetPred(context, x.Bits(81, 84), GetPredLogicalOp(context, bop, lo, p));
            ASetPred(context, x.Bits(84, 87), GetPredLogicalOp(context, bop, hi, p));
        }

        // HSET2 Rd, a, b, P: each half 1.0 (.BF, 71) or all ones where (comparison bop P) holds, else 0
        public static void AHset2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand[] a, Operand[] b) = AHalfSecond(context, x);
            Operand p = APred(context, x, 87, 90);
            BoolOp bop = (BoolOp)x.Bits(69, 71);
            Operand res = Const(0);

            for (int i = 0; i < 2; i++)
            {
                Operand cond = GetPredLogicalOp(context, bop, GetFPComparison(context, (FComp)x.Bits(76, 80), a[i], b[i]), p);
                Operand v = context.ConditionalSelect(cond, Const(x.Bit(71) ? 0x3c00 : 0xffff), Const(0));
                res = context.BitwiseOr(res, i == 0 ? v : context.ShiftLeft(v, Const(16)));
            }

            ASet(context, x, res);
        }

        // HMNMX2 Rd, a, b, P: each half's minimum where P (87..90) holds, else maximum
        public static void AHmnmx2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bit(78) || x.Bit(81) || x.Bit(82) || x.Bit(85))
            {
                throw AFail(context, "HMNMX2 .F32 / .NAN / .XORSIGN / .BF16");
            }

            (Operand[] a, Operand[] b) = AHalfSecond(context, x);
            Operand p = APred(context, x, 87, 90);
            Operand[] r = new Operand[2];

            for (int i = 0; i < 2; i++)
            {
                r[i] = context.ConditionalSelect(p, context.FPMinimum(a[i], b[i]), context.FPMaximum(a[i], b[i]));
            }

            ASet(context, x, context.PackHalf2x16(r[0], r[1]));
        }
    }
}
