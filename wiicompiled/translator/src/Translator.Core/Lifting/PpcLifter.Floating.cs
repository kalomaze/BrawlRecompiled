using System.Collections.Generic;
using Translator.Core.Disassembly;
using Translator.Core.Ir;

namespace Translator.Core.Lifting;

public sealed partial class PpcLifter
{
    private static IReadOnlyList<IrInstruction>? TryLiftFloating(
        PpcInstruction ins,
        string mnemonic,
        IReadOnlyList<string> operands)
    {
        switch (mnemonic)
        {
            case "mffs":
            case "mffs.":
                return LiftMffs(ins);

            case "mtfsf":
            case "mtfsf.":
                return LiftMtfsf(ins);

            case "mtfsfi":
            case "mtfsfi.":
                return LiftMtfsfi(ins);

            case "mtfsb0":
            case "mtfsb0.":
                return LiftMtfsb0(ins);

            case "mtfsb1":
            case "mtfsb1.":
                return LiftMtfsb1(ins);

            case "fadd" when operands.Count == 3:
            case "fadd." when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "add") };

            case "fsub" when operands.Count == 3:
            case "fsub." when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "sub") };

            case "fmul" when operands.Count == 3:
            case "fmul." when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "mul") };

            case "fdiv" when operands.Count == 3:
            case "fdiv." when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "fdiv") };

            case "fadds" when operands.Count == 3:
            case "fadds." when operands.Count == 3:
                return new[] { new IrCall(operands[0], "PPC_Fadds", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]) }) };

            case "fsubs" when operands.Count == 3:
            case "fsubs." when operands.Count == 3:
                return new[] { new IrCall(operands[0], "PPC_Fsubs", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]) }) };

            case "fmuls" when operands.Count == 3:
            case "fmuls." when operands.Count == 3:
                return new[] { new IrCall(operands[0], "PPC_Fmuls", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]) }) };

            case "fdivs" when operands.Count == 3:
            case "fdivs." when operands.Count == 3:
                return new[] { new IrCall(operands[0], "PPC_Fdivs", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]) }) };

            case "fres" when operands.Count == 2:
            case "fres." when operands.Count == 2:
                return new[] { new IrCall(operands[0], "PPC_Fres", new[] { IrValue.Register(operands[1]) }) };

            case "fmsubs" when operands.Count == 4:
            case "fmsubs." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fmsubs", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fmadds" when operands.Count == 4:
            case "fmadds." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fmadds", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fnmsubs" when operands.Count == 4:
            case "fnmsubs." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fnmsubs", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fnmadds" when operands.Count == 4:
            case "fnmadds." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fnmadds", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "frsp" when operands.Count == 2:
            case "frsp." when operands.Count == 2:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "frsp") };

            case "fneg" when operands.Count == 2:
            case "fneg." when operands.Count == 2:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "fneg") };

            case "fabs" when operands.Count == 2:
            case "fabs." when operands.Count == 2:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "fabs") };

            case "fnabs" when operands.Count == 2:
            case "fnabs." when operands.Count == 2:
                return new[]
                {
                    new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "fabs"),
                    new IrBinary(operands[0], IrValue.Register(operands[0]), IrValue.Imm(0), "fneg")
                };

            case "fmr" when operands.Count == 2:
            case "fmr." when operands.Count == 2:
                return new[] { new IrAssign(operands[0], IrValue.Register(operands[1])) };

            case "fctiw" when operands.Count == 2:
            case "fctiw." when operands.Count == 2:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "fctiw") };

            case "fctiwz" when operands.Count == 2:
            case "fctiwz." when operands.Count == 2:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "fctiwz") };

            case "fsel" when operands.Count == 4:
            case "fsel." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fsel", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[3]), IrValue.Register(operands[2]) }) };

            case "frsqrte" when operands.Count == 2:
            case "frsqrte." when operands.Count == 2:
                return new[] { new IrCall(operands[0], "PPC_Frsqrte", new[] { IrValue.Register(operands[1]) }) };

            case "fmsub" when operands.Count == 4:
            case "fmsub." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fmsub", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fmadd" when operands.Count == 4:
            case "fmadd." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fmadd", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fnmsub" when operands.Count == 4:
            case "fnmsub." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fnmsub", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fnmadd" when operands.Count == 4:
            case "fnmadd." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fnmadd", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fcmpu" when operands.Count == 3:
            case "fcmpo" when operands.Count == 3:
                {
                    // Model the CR write explicitly so leaf/resident register caches update
                    // the authoritative CR storage instead of a stale CpuContext shadow.
                    var destCr = ParseCrFieldIndex(operands[0]);
                    return new[]
                    {
                        new IrSetCrField(destCr, IrValue.Register(operands[1]),
                            IrValue.Register(operands[2]), false)
                    };
                }
        }

        return null;
    }
}
