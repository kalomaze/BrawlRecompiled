using System;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

public class FloatingLifterTests
{
    private static PpcRegisterOperand Fpr(int index) => new($"f{index}", index);

    private static PpcInstruction FloatingInstruction(uint raw, string mnemonic, params PpcOperand[] operands)
        => PpcInstruction.Synthetic(0x80000000, raw, mnemonic, operands);

    [Fact]
    public void LiftsFloatingStatusRegisterHelpersFromRawBits()
    {
        var lifter = new PpcLifter();

        var mffs = FloatingInstruction((63u << 26) | (5u << 21), "mffs");
        var mffsIr = Assert.Single(lifter.Lift(new[] { mffs })).Ir;
        var mffsCall = Assert.IsType<IrCall>(Assert.Single(mffsIr));
        Assert.Equal("PPC_Mffs", mffsCall.Target);
        Assert.Equal("f5", mffsCall.Destination);
        Assert.Empty(mffsCall.Arguments);

        var mtfsf = FloatingInstruction((63u << 26) | (0xAAu << 17) | (7u << 11), "mtfsf");
        var mtfsfIr = Assert.Single(lifter.Lift(new[] { mtfsf })).Ir;
        var mtfsfCall = Assert.IsType<IrCall>(Assert.Single(mtfsfIr));
        Assert.Equal("PPC_Mtfsf", mtfsfCall.Target);
        Assert.Equal(0xAA, mtfsfCall.Arguments[0].Constant);
        Assert.Equal("f7", mtfsfCall.Arguments[1].RegisterName);

        var mtfsb1 = FloatingInstruction((63u << 26) | (9u << 21), "mtfsb1");
        var mtfsb1Ir = Assert.Single(lifter.Lift(new[] { mtfsb1 })).Ir;
        var mtfsb1Call = Assert.IsType<IrCall>(Assert.Single(mtfsb1Ir));
        Assert.Equal("PPC_Mtfsb1", mtfsb1Call.Target);
        Assert.Equal(9, Assert.Single(mtfsb1Call.Arguments).Constant);

        var mtfsb0 = FloatingInstruction((63u << 26) | (10u << 21), "mtfsb0");
        var mtfsb0Ir = Assert.Single(lifter.Lift(new[] { mtfsb0 })).Ir;
        var mtfsb0Call = Assert.IsType<IrCall>(Assert.Single(mtfsb0Ir));
        Assert.Equal("PPC_Mtfsb0", mtfsb0Call.Target);
        Assert.Equal(10, Assert.Single(mtfsb0Call.Arguments).Constant);

        var mtfsfi = FloatingInstruction((63u << 26) | (3u << 23) | (0xCu << 12), "mtfsfi");
        var mtfsfiIr = Assert.Single(lifter.Lift(new[] { mtfsfi })).Ir;
        var mtfsfiCall = Assert.IsType<IrCall>(Assert.Single(mtfsfiIr));
        Assert.Equal("PPC_Mtfsfi", mtfsfiCall.Target);
        Assert.Equal(new long?[] { 3, 0xC }, mtfsfiCall.Arguments.Select(a => a.Constant).ToArray());
    }

    [Theory]
    [InlineData("fadd", "add")]
    [InlineData("fsub", "sub")]
    [InlineData("fmul", "mul")]
    [InlineData("fdiv", "fdiv")]
    public void LiftsFloatingBinaryMnemonics(string mnemonic, string op)
    {
        var instruction = FloatingInstruction(0, mnemonic, Fpr(1), Fpr(2), Fpr(3));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
        Assert.Equal("f1", binary.Destination);
        Assert.Equal(op, binary.Op);
        Assert.Equal("f2", binary.Left.RegisterName);
        Assert.Equal("f3", binary.Right.RegisterName);
    }

    [Theory]
    [InlineData("fadd.", "add")]
    [InlineData("fsub.", "sub")]
    [InlineData("fmul.", "mul")]
    [InlineData("fdiv.", "fdiv")]
    public void LiftsFloatingBinaryDotMnemonics(string mnemonic, string op)
    {
        var instruction = FloatingInstruction(0, mnemonic, Fpr(1), Fpr(2), Fpr(3));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
        Assert.Equal("f1", binary.Destination);
        Assert.Equal(op, binary.Op);
    }

    [Theory]
    [InlineData("frsp", "frsp")]
    [InlineData("fneg", "fneg")]
    [InlineData("fabs", "fabs")]
    [InlineData("fctiw", "fctiw")]
    [InlineData("fctiwz", "fctiwz")]
    public void LiftsFloatingUnaryMnemonics(string mnemonic, string op)
    {
        var instruction = FloatingInstruction(0, mnemonic, Fpr(4), Fpr(7));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
        Assert.Equal("f4", binary.Destination);
        Assert.Equal(op, binary.Op);
        Assert.Equal("f7", binary.Left.RegisterName);
        Assert.Equal(0, binary.Right.Constant);
    }

    [Theory]
    [InlineData("frsp.", "frsp")]
    [InlineData("fneg.", "fneg")]
    [InlineData("fabs.", "fabs")]
    [InlineData("fctiw.", "fctiw")]
    [InlineData("fctiwz.", "fctiwz")]
    public void LiftsFloatingUnaryDotMnemonics(string mnemonic, string op)
    {
        var instruction = FloatingInstruction(0, mnemonic, Fpr(4), Fpr(7));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
        Assert.Equal("f4", binary.Destination);
        Assert.Equal(op, binary.Op);
    }

    [Fact]
    public void LiftsFmrAsRegisterAssign()
    {
        var instruction = FloatingInstruction(0, "fmr", Fpr(6), Fpr(2));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var assign = Assert.IsType<IrAssign>(Assert.Single(ir));
        Assert.Equal("f6", assign.Destination);
        Assert.Equal("f2", assign.Value.RegisterName);
    }

    [Fact]
    public void LiftsFnabsAsNegativeAbsoluteValue()
    {
        var instruction = FloatingInstruction(0, "fnabs", Fpr(6), Fpr(2));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        Assert.Equal("fabs", Assert.IsType<IrBinary>(ir[0]).Op);
        Assert.Equal("fneg", Assert.IsType<IrBinary>(ir[1]).Op);
    }

    [Fact]
    public void DecodesAndLiftsMultiplyForEveryMultiplierRegister()
    {
        for (uint c = 0; c < 32; ++c)
        for (uint record = 0; record < 2; ++record)
        {
            var instruction = PpcDecoder.Decode(0x80000000,
                (63u << 26) | (3u << 21) | (1u << 16) | (c << 6) | (25u << 1) | record);
            var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
            var multiply = Assert.IsType<IrBinary>(Assert.Single(ir));
            Assert.Equal("mul", multiply.Op);
            Assert.Equal("f3", multiply.Destination);
            Assert.Equal("f1", multiply.Left.RegisterName);
            Assert.Equal($"f{c}", multiply.Right.RegisterName);
        }
    }

    [Theory]
    [InlineData(18u, "fdiv")]
    [InlineData(20u, "fsub")]
    [InlineData(21u, "fadd")]
    [InlineData(26u, "frsqrte")]
    public void AFormDecodingDoesNotTreatUnusedCFieldAsOpcode(uint xo, string mnemonic)
    {
        for (uint c = 0; c < 32; ++c)
        {
            var instruction = PpcDecoder.Decode(0x80000000,
                (63u << 26) | (3u << 21) | (1u << 16) | (2u << 11) | (c << 6) | (xo << 1));
            Assert.Equal(mnemonic, instruction.Mnemonic);
            Assert.Single(Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir);
        }
    }

    [Theory]
    [InlineData(0u)]
    [InlineData(32u)]
    public void DecodesAndLiftsCompareIntoConditionRegister(uint xo)
    {
        var instruction = PpcDecoder.Decode(0x80000000,
            (63u << 26) | (6u << 23) | (2u << 16) | (3u << 11) | (xo << 1));
        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var compare = Assert.IsType<IrSetCrField>(Assert.Single(ir));
        Assert.Equal(6, compare.FieldIndex);
        Assert.Equal("f2", compare.Left.RegisterName);
        Assert.Equal("f3", compare.Right.RegisterName);
    }

    [Theory]
    [InlineData(59u, 28u, "PPC_Fmsubs")]
    [InlineData(59u, 29u, "PPC_Fmadds")]
    [InlineData(59u, 30u, "PPC_Fnmsubs")]
    [InlineData(59u, 31u, "PPC_Fnmadds")]
    [InlineData(63u, 28u, "PPC_Fmsub")]
    [InlineData(63u, 29u, "PPC_Fmadd")]
    [InlineData(63u, 30u, "PPC_Fnmsub")]
    [InlineData(63u, 31u, "PPC_Fnmadd")]
    public void DecodesAndLiftsFusedOperandOrder(uint primary, uint xo, string helper)
    {
        var instruction = PpcDecoder.Decode(0x80000000,
            (primary << 26) | (4u << 21) | (1u << 16) | (2u << 11) | (3u << 6) | (xo << 1));
        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var call = Assert.IsType<IrCall>(Assert.Single(ir));
        Assert.Equal(helper, call.Target);
        Assert.Equal("f4", call.Destination);
        Assert.Equal(new[] { "f1", "f3", "f2" }, call.Arguments.Select(a => a.RegisterName));
    }

    [Theory]
    [InlineData(59u, 22u)] // fsqrts is not a Gekko instruction.
    [InlineData(59u, 23u)] // fsel belongs to primary 63.
    [InlineData(59u, 26u)] // frsqrte belongs to primary 63.
    [InlineData(63u, 22u)] // fsqrt is not a Gekko instruction.
    [InlineData(63u, 24u)] // fres belongs to primary 59.
    [InlineData(63u, 200u)] // Low five bits must not turn unknown X-form instructions into fcmp.
    [InlineData(63u, 204u)] // Nor into frsp.
    public void UnknownFloatingInstructionsRemainUndefined(uint primary, uint xo)
    {
        var word = (primary << 26) | (4u << 21) | (2u << 11) | (xo << 1);
        var instruction = PpcDecoder.Decode(0x80000000, word);
        var lifter = new PpcLifter();
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { instruction }));
        var ir = Assert.Single(lifter.Lift(new[] { instruction }, allowUnsupported: true)).Ir;
        Assert.IsType<IrUndefined>(Assert.Single(ir));
    }
}
