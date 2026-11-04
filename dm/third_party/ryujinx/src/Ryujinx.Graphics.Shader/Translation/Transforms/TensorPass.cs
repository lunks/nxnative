using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation.Optimizations;
using System.Collections.Generic;
using System.Diagnostics;

using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Transforms
{
    // nxnative: MatrixMultiplyAdd (Ampere HMMA.16816 with an f16 accumulator, InstEmitAmpereTensor.AHmma) becomes a call of the helper dmsh_hmma16816_f16, whose body the
    // shader build replaces with simdgroup_matrix code in the Metal source. Sources: the scratch shared memory id (constant), the lane, the subgroup's block offset, A (4), B (2),
    // C (2); destinations: D (2), the first as the call's result, the second through its out argument (as ShufflePass does with its valid flag).
    // IntegerMatrixMultiply (the products of Ampere IMMA.16832 on 8-bit elements of one sign in a TensorOps source, InstEmitAmpereTensor.AImma) becomes a call of the helper
    // dmsh_imma16832_s8 or _u8 the same way. Sources: the scratch shared memory id and the signedness (constants), the lane, the block offset, A (4), B (2); destinations: P (4)
    class TensorPass : ITransformPass
    {
        public static bool IsEnabled(IGpuAccessor gpuAccessor, ShaderStage stage, TargetLanguage targetLanguage, FeatureFlags usedFeatures)
        {
            return true;
        }

        public static LinkedListNode<INode> RunPass(TransformContext context, LinkedListNode<INode> node)
        {
            Operation operation = (Operation)node.Value;

            if (operation.Inst == Instruction.IntegerMatrixMultiply)
            {
                return RunImma(context, node, operation);
            }

            if (operation.Inst != Instruction.MatrixMultiplyAdd)
            {
                return node;
            }

            Operand memoryId = operation.GetSource(0);

            Debug.Assert(memoryId.Type == OperandType.Constant);

            int functionId = context.Hfm.GetOrCreateFunctionId(HelperFunctionName.Hmma16816F16, memoryId.Value);

            Operand d0 = operation.GetDest(0);
            Operand d1 = operation.GetDest(1);

            operation.Dest = null;

            Operand[] callArgs = new Operand[12];
            callArgs[0] = Const(functionId);

            for (int i = 0; i < 10; i++)
            {
                callArgs[1 + i] = operation.GetSource(1 + i);
            }

            callArgs[11] = d1;

            LinkedListNode<INode> newNode = node.List.AddBefore(node, new Operation(Instruction.Call, 0, d0, callArgs));

            Utils.DeleteNode(node, operation);

            return newNode;
        }

        private static LinkedListNode<INode> RunImma(TransformContext context, LinkedListNode<INode> node, Operation operation)
        {
            Operand memoryId = operation.GetSource(0);
            Operand signed = operation.GetSource(1);

            Debug.Assert(memoryId.Type == OperandType.Constant && signed.Type == OperandType.Constant);

            int functionId = context.Hfm.GetOrCreateFunctionId(signed.Value != 0 ? HelperFunctionName.Imma16832S8 : HelperFunctionName.Imma16832U8, memoryId.Value);

            Operand[] p = [operation.GetDest(0), operation.GetDest(1), operation.GetDest(2), operation.GetDest(3)];

            operation.Dest = null;

            Operand[] callArgs = new Operand[12];
            callArgs[0] = Const(functionId);

            for (int i = 0; i < 8; i++)
            {
                callArgs[1 + i] = operation.GetSource(2 + i);
            }

            callArgs[9] = p[1];
            callArgs[10] = p[2];
            callArgs[11] = p[3];

            LinkedListNode<INode> newNode = node.List.AddBefore(node, new Operation(Instruction.Call, 0, p[0], callArgs));

            Utils.DeleteNode(node, operation);

            return newNode;
        }
    }
}
