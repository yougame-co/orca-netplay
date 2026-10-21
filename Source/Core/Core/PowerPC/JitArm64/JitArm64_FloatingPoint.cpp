// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitArm64/Jit.h"

#include "Common/Arm64Emitter.h"
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/SmallVector.h"
#include "Common/StringUtil.h"

#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/JitArm64/JitArm64_RegCache.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"

using namespace Arm64Gen;

void JitArm64::SetFPRFIfNeeded(bool single, ARM64Reg reg)
{
  if (!m_fprf || !js.op->wantsFPRF)
    return;

  gpr.Lock(ARM64Reg::W0, ARM64Reg::W1, ARM64Reg::W2, ARM64Reg::W3, ARM64Reg::W4, ARM64Reg::W30);

  const ARM64Reg routine_input_reg = single ? ARM64Reg::W0 : ARM64Reg::X0;
  if (IsVector(reg))
  {
    m_float_emit.FMOV(routine_input_reg, single ? EncodeRegToSingle(reg) : EncodeRegToDouble(reg));
  }
  else if (reg != routine_input_reg)
  {
    MOV(routine_input_reg, reg);
  }

  BL(single ? GetAsmRoutines()->fprf_single : GetAsmRoutines()->fprf_double);

  gpr.Unlock(ARM64Reg::W0, ARM64Reg::W1, ARM64Reg::W2, ARM64Reg::W3, ARM64Reg::W4, ARM64Reg::W30);
}

// Emulate the odd truncation/rounding that the PowerPC does on the RHS operand before
// a single precision multiply. To be precise, it drops the low 28 bits of the mantissa,
// rounding to nearest as it does.
void JitArm64::Force25BitPrecision(ARM64Reg output, ARM64Reg input)
{
  if (IsQuad(input))
  {
    m_float_emit.URSHR(64, output, input, 28);
    m_float_emit.SHL(64, output, output, 28);
  }
  else
  {
    m_float_emit.URSHR(output, input, 28);
    m_float_emit.SHL(output, output, 28);
  }
}

// Orca: PowerPC's fneg, fabs and fnabs (their paired forms and fnmadd/fnmsub's final negation too)
// change only the sign bit, NaNs included, as Jit64 does with XORPD/ANDPD/ORPD. AArch64's FNEG and
// FABS leave a NaN's sign alone while FPCR.AH is set (the guest's non-IEEE mode), which made Macs
// and PCs compute NaNs of opposite sign and desync.
// size: 32 or 64 (the lanes). packed: Vd and Vn are D (two singles) or Q (two doubles) registers
// and every lane changes; otherwise only the low lane, and the rest of Vd is zeroed as FNEG did.
void JitArm64::EmitSignOp(SignOp op, u8 size, ARM64Reg Vd, ARM64Reg Vn, bool packed,
                          ARM64Reg scratch_gpr, ARM64Reg scratch_fpr)
{
  const u64 sign = size == 64 ? 0x8000000000000000ULL : 0x80000000ULL;
  const u64 all = size == 64 ? ~0ULL : 0xFFFFFFFFULL;
  if (!packed)
  {
    const bool wide = size == 64;
    const ARM64Reg RA = wide ? EncodeRegTo64(scratch_gpr) : EncodeRegTo32(scratch_gpr);
    const GPRSize gpr_size = wide ? GPRSize::B64 : GPRSize::B32;
    const ARM64Reg FD = wide ? EncodeRegToDouble(Vd) : EncodeRegToSingle(Vd);
    const ARM64Reg FN = wide ? EncodeRegToDouble(Vn) : EncodeRegToSingle(Vn);
    m_float_emit.FMOV(RA, FN);
    switch (op)
    {
    case SignOp::Negate:
      EOR(RA, RA, LogicalImm(sign, gpr_size));
      break;
    case SignOp::Abs:
      AND(RA, RA, LogicalImm(all & ~sign, gpr_size));
      break;
    case SignOp::NegativeAbs:
      ORR(RA, RA, LogicalImm(sign, gpr_size));
      break;
    }
    m_float_emit.FMOV(FD, RA);
    return;
  }

  const ARM64Reg mask = IsQuad(Vd) ? EncodeRegToQuad(scratch_fpr) : EncodeRegToDouble(scratch_fpr);
  if (size == 64)
  {
    MOVI2R(EncodeRegTo64(scratch_gpr), sign);
    m_float_emit.DUP(64, mask, EncodeRegTo64(scratch_gpr));
  }
  else
  {
    m_float_emit.MOVI(32, mask, 0x80, 24);
  }
  switch (op)
  {
  case SignOp::Negate:
    m_float_emit.EOR(Vd, Vn, mask);
    break;
  case SignOp::Abs:
    m_float_emit.BIC(Vd, Vn, mask);
    break;
  case SignOp::NegativeAbs:
    m_float_emit.ORR(Vd, Vn, mask);
    break;
  }
}

void JitArm64::EmitPPCDefaultNaN(ARM64Reg Vd, bool single)
{
  const ARM64Reg VD = EncodeRegToDouble(Vd);
  if (single)
  {
    m_float_emit.MOVI(32, VD, 0x7F, 24);  // 0x7F000000
    m_float_emit.ORR(32, VD, 0xC0, 16);   // 0x7FC00000
  }
  else
  {
    m_float_emit.MOVI(64, VD, 0xFFFF'0000'0000'0000ULL);
    m_float_emit.BIC(16, VD, 0x80, 8);  // 0x7FFF'0000'0000'0000
    m_float_emit.BIC(16, VD, 0x07, 0);  // 0x7FF8'0000'0000'0000
  }
}

void JitArm64::fp_arith(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || (jo.div_by_zero_exceptions && inst.SUBOP5 == 18));

  const u32 a = inst.FA;
  const u32 b = inst.FB;
  const u32 c = inst.FC;
  const u32 d = inst.FD;
  const u32 op5 = inst.SUBOP5;

  const bool use_c = op5 >= 25;  // fmul and all kinds of fmaddXX
  const bool use_b = op5 != 25;  // fmul uses no B
  const bool fma = use_b && use_c;
  const bool negate_result = (op5 & ~0x1) == 30;
  const bool negate_b = op5 == 28 || op5 == 30;

  const bool output_is_single = inst.OPCD == 59;
  const bool nonfused_requested = fma && !Config::Get(Config::SESSION_USE_FMA);
  const bool error_free_transformation_requested = fma && m_accurate_fmadds;
  const bool round_c = use_c && output_is_single && !js.op->fprIsSingle[c];

  const auto inputs_are_singles_func = [&] {
    return IsSingleForArithmetic(a, true) && (!use_b || IsSingleForArithmetic(b, true)) &&
           (!use_c || IsSingleForArithmetic(c, true));
  };

  const bool single = inputs_are_singles_func() && output_is_single &&
                      (error_free_transformation_requested || !nonfused_requested);
  const RegType type = single ? RegType::LowerPairSingle : RegType::LowerPair;
  const RegType type_out = output_is_single ?
                               (single ? RegType::DuplicatedSingle : RegType::Duplicated) :
                               RegType::LowerPair;
  const auto reg_encoder = single ? EncodeRegToSingle : EncodeRegToDouble;

  const bool nonfused = nonfused_requested && !single;
  const bool error_free_transformation =
      error_free_transformation_requested && !single && output_is_single;

  if (error_free_transformation)
  {
    gpr.Lock(ARM64Reg::W0, ARM64Reg::W1, ARM64Reg::W30);
    fpr.Lock(ARM64Reg::Q0, ARM64Reg::Q1, ARM64Reg::Q2);
  }

  const ARM64Reg VA = reg_encoder(fpr.R(a, type));
  const ARM64Reg VB = use_b ? reg_encoder(fpr.R(b, type)) : ARM64Reg::INVALID_REG;
  const ARM64Reg VC = use_c ? reg_encoder(fpr.R(c, type)) : ARM64Reg::INVALID_REG;
  const ARM64Reg VD = reg_encoder(fpr.RW(d, type_out));

  {
    Arm64FPRCache::ScopedARM64Reg V0Q = ARM64Reg::INVALID_REG;

    ARM64Reg rounded_c_reg = VC;
    if (round_c)
    {
      V0Q = fpr.GetScopedReg();
      rounded_c_reg = reg_encoder(V0Q);
    }

    ARM64Reg result_reg = VD;
    ARM64Reg nonfused_reg = VD;
    if (error_free_transformation)
    {
      result_reg = reg_encoder(ARM64Reg::Q0);
      nonfused_reg = reg_encoder(ARM64Reg::Q0);

      if (nonfused && V0Q == ARM64Reg::INVALID_REG)
        V0Q = fpr.GetScopedReg();
    }
    else
    {
      const bool preserve_d =
          m_accurate_nans && (VD == VA || (use_b && VD == VB) || (use_c && VD == VC));
      if (preserve_d)
      {
        if (V0Q == ARM64Reg::INVALID_REG)
          V0Q = fpr.GetScopedReg();
        result_reg = reg_encoder(V0Q);
        nonfused_reg = reg_encoder(V0Q);
      }
      else if (fma && nonfused && VD == VB)
      {
        if (V0Q == ARM64Reg::INVALID_REG)
          V0Q = fpr.GetScopedReg();
        nonfused_reg = reg_encoder(V0Q);
      }
    }

    if (round_c)
    {
      ASSERT_MSG(DYNA_REC, !single, "Tried to apply 25-bit precision to single");
      Force25BitPrecision(rounded_c_reg, VC);
    }

    switch (op5)
    {
    case 18:
      m_float_emit.FDIV(result_reg, VA, VB);
      break;
    case 20:
      m_float_emit.FSUB(result_reg, VA, VB);
      break;
    case 21:
      m_float_emit.FADD(result_reg, VA, VB);
      break;
    case 25:
      m_float_emit.FMUL(result_reg, VA, rounded_c_reg);
      break;
    // While it may seem like PowerPC's nmadd/nmsub map to AArch64's nmadd/msub [sic],
    // the subtly different definitions affect how signed zeroes are handled.
    // Also, PowerPC's nmadd/nmsub perform rounding before the final negation.
    // So, we negate using a separate FNEG instruction instead of using AArch64's nmadd/msub.
    case 28:  // fmsub: "D = A*C - B" vs "Vd = (-Va) + Vn*Vm"
    case 30:  // fnmsub: "D = -(A*C - B)" vs "Vd = -((-Va) + Vn*Vm)"
      if (nonfused)
      {
        m_float_emit.FMUL(nonfused_reg, VA, rounded_c_reg);
        m_float_emit.FSUB(result_reg, nonfused_reg, VB);
      }
      else
      {
        m_float_emit.FNMSUB(result_reg, VA, rounded_c_reg, VB);
      }
      break;
    case 29:  // fmadd: "D = A*C + B" vs "Vd = Va + Vn*Vm"
    case 31:  // fnmadd: "D = -(A*C + B)" vs "Vd = -(Va + Vn*Vm)"
      if (nonfused)
      {
        m_float_emit.FMUL(nonfused_reg, VA, rounded_c_reg);
        m_float_emit.FADD(result_reg, nonfused_reg, VB);
      }
      else
      {
        m_float_emit.FMADD(result_reg, VA, rounded_c_reg, VB);
      }
      break;
    default:
      ASSERT_MSG(DYNA_REC, 0, "fp_arith");
      break;
    }

    // Taken before the accurate-NaN branch, which skips the negation (EmitSignOp).
    Arm64GPRCache::ScopedARM64Reg negate_gpr = ARM64Reg::INVALID_REG;
    if (negate_result)
      negate_gpr = gpr.GetScopedReg();

    Common::SmallVector<FixupBranch, 4> nan_fixups;
    if (m_accurate_nans)
    {
      // Check if we need to handle NaNs
      m_float_emit.FCMP(result_reg);
      FixupBranch no_nan = B(CCFlags::CC_VC);
      FixupBranch nan = B();
      SetJumpTarget(no_nan);

      SwitchToFarCode();
      SetJumpTarget(nan);

      Common::SmallVector<ARM64Reg, 3> inputs;
      inputs.push_back(VA);
      if (use_b && VA != VB)
        inputs.push_back(VB);
      if (use_c && VA != VC && (!use_b || VB != VC))
        inputs.push_back(VC);

      // If any inputs are NaNs, pick the first NaN of them and set its quiet bit: PowerPC's order,
      // a, b, c, with c as it is before its 25-bit rounding. Orca: check the last input too, so a
      // result with no NaN input gets PowerPC's default NaN, not the host's (sign varies with AH).
      for (const ARM64Reg input : inputs)
      {
        m_float_emit.FCMP(input);
        FixupBranch skip = B(CCFlags::CC_VC);

        // Make the NaN quiet
        m_float_emit.FADD(VD, input, input);

        nan_fixups.push_back(B());

        SetJumpTarget(skip);
      }

      // No NaN input: PowerPC's positive default NaN. The NaN result is never negated
      // (fnmadd/fnmsub), as on PowerPC.
      EmitPPCDefaultNaN(VD, single);
      nan_fixups.push_back(B());

      SwitchToNearCode();
    }

    // Read the comment in the interpreter function NI_madd_msub to find out what's going on here
    if (error_free_transformation)
    {
      // We've calculated s := a + b (with a = VA * rounded_c_reg, b = negate_b ? -VB : VB)

      // a' := s - b
      if (negate_b)
        m_float_emit.FADD(ARM64Reg::D1, result_reg, VB);
      else
        m_float_emit.FSUB(ARM64Reg::D1, result_reg, VB);

      // b' := s - a'
      m_float_emit.FSUB(ARM64Reg::D2, result_reg, ARM64Reg::D1);

      // da := a - a'
      if (nonfused)
      {
        m_float_emit.FMUL(EncodeRegToDouble(V0Q), VA, rounded_c_reg);
        m_float_emit.FSUB(ARM64Reg::D1, EncodeRegToDouble(V0Q), ARM64Reg::D1);
      }
      else
      {
        m_float_emit.FNMSUB(ARM64Reg::D1, VA, rounded_c_reg, ARM64Reg::D1);
      }

      // db := b - b'
      // (Transformed into -db := b' - b)
      if (negate_b)
        m_float_emit.FADD(ARM64Reg::D2, ARM64Reg::D2, VB);
      else
        m_float_emit.FSUB(ARM64Reg::D2, ARM64Reg::D2, VB);

      BL(GetAsmRoutines()->fmadds_eft);
    }

    // PowerPC's nmadd/nmsub perform rounding before the final negation, which is not the case
    // for any of AArch64's FMA instructions, so we negate using a separate instruction.
    if (negate_result)
      EmitSignOp(SignOp::Negate, IsSingle(VD) ? 32 : 64, VD, result_reg, false, negate_gpr,
                 ARM64Reg::INVALID_REG);
    else if (result_reg != VD)
      m_float_emit.MOV(EncodeRegToDouble(VD), EncodeRegToDouble(result_reg));

    for (FixupBranch fixup : nan_fixups)
      SetJumpTarget(fixup);
  }

  if (output_is_single)
  {
    ASSERT_MSG(DYNA_REC, single == inputs_are_singles_func(),
               "Register allocation turned singles into doubles in the middle of fp_arith");

    fpr.FixSinglePrecision(d);
  }

  if (error_free_transformation)
    gpr.Unlock(ARM64Reg::W0, ARM64Reg::W1, ARM64Reg::W30);

  SetFPRFIfNeeded(output_is_single, VD);

  if (error_free_transformation)
    fpr.Unlock(ARM64Reg::Q0, ARM64Reg::Q1, ARM64Reg::Q2);
}

void JitArm64::fp_logic(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);

  const u32 b = inst.FB;
  const u32 d = inst.FD;
  const u32 op10 = inst.SUBOP10;

  bool packed = inst.OPCD == 4;

  // MR with source === dest => no-op
  if (op10 == 72 && b == d)
    return;

  const bool single = fpr.IsSingle(b, !packed);
  const u8 size = single ? 32 : 64;

  // EmitSignOp's scratch registers (every case but fmr).
  Arm64GPRCache::ScopedARM64Reg sign_gpr = ARM64Reg::INVALID_REG;
  Arm64FPRCache::ScopedARM64Reg sign_fpr = ARM64Reg::INVALID_REG;
  if (op10 != 72)
  {
    sign_gpr = gpr.GetScopedReg();
    if (packed)
      sign_fpr = fpr.GetScopedReg();
  }

  if (packed)
  {
    const RegType type = single ? RegType::Single : RegType::Register;
    const auto reg_encoder = single ? EncodeRegToDouble : EncodeRegToQuad;

    const ARM64Reg VB = reg_encoder(fpr.R(b, type));
    const ARM64Reg VD = reg_encoder(fpr.RW(d, type));

    switch (op10)
    {
    case 40:
      EmitSignOp(SignOp::Negate, size, VD, VB, true, sign_gpr, sign_fpr);
      break;
    case 72:
      m_float_emit.ORR(VD, VB, VB);
      break;
    case 136:
      EmitSignOp(SignOp::NegativeAbs, size, VD, VB, true, sign_gpr, sign_fpr);
      break;
    case 264:
      EmitSignOp(SignOp::Abs, size, VD, VB, true, sign_gpr, sign_fpr);
      break;
    default:
      ASSERT_MSG(DYNA_REC, 0, "fp_logic");
      break;
    }
  }
  else
  {
    const RegType type = single ? RegType::LowerPairSingle : RegType::LowerPair;

    const ARM64Reg VB = fpr.R(b, type);
    const ARM64Reg VD = fpr.RW(d, type);

    switch (op10)
    {
    case 40:
      EmitSignOp(SignOp::Negate, size, VD, VB, false, sign_gpr, sign_fpr);
      break;
    case 72:
      m_float_emit.INS(size, VD, 0, VB, 0);
      break;
    case 136:
      EmitSignOp(SignOp::NegativeAbs, size, VD, VB, false, sign_gpr, sign_fpr);
      break;
    case 264:
      EmitSignOp(SignOp::Abs, size, VD, VB, false, sign_gpr, sign_fpr);
      break;
    default:
      ASSERT_MSG(DYNA_REC, 0, "fp_logic");
      break;
    }
  }

  ASSERT_MSG(DYNA_REC, single == fpr.IsSingle(b, !packed),
             "Register allocation turned singles into doubles in the middle of fp_logic");
}

void JitArm64::fselx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);

  const u32 a = inst.FA;
  const u32 b = inst.FB;
  const u32 c = inst.FC;
  const u32 d = inst.FD;

  // a compares as a single only where that can't flush a denormal (IsSingleForArithmetic). b and c
  // are only selected, bit for bit, so they stay singles unless one of them is a.
  const bool a_single_ok = IsSingleForArithmetic(a, true);
  const bool b_and_c_singles =
      fpr.IsSingle(b, true) && fpr.IsSingle(c, true) && (a_single_ok || (a != b && a != c));
  const RegType b_and_c_type = b_and_c_singles ? RegType::LowerPairSingle : RegType::LowerPair;
  const auto b_and_c_reg_encoder = b_and_c_singles ? EncodeRegToSingle : EncodeRegToDouble;

  const bool a_single = a_single_ok && (b_and_c_singles || (a != b && a != c));
  const RegType a_type = a_single ? RegType::LowerPairSingle : RegType::LowerPair;
  const auto a_reg_encoder = a_single ? EncodeRegToSingle : EncodeRegToDouble;

  const ARM64Reg VA = fpr.R(a, a_type);
  const ARM64Reg VB = fpr.R(b, b_and_c_type);
  const ARM64Reg VC = fpr.R(c, b_and_c_type);

  // If a == d, the RW call below may change the type of a to double. This is okay, because the
  // actual value in the register is not altered by RW. So let's just assert before calling RW.
  ASSERT_MSG(DYNA_REC, a_single == fpr.IsSingle(a, true),
             "Register allocation turned singles into doubles in the middle of fselx");

  const ARM64Reg VD = fpr.RW(d, b_and_c_type);

  m_float_emit.FCMPE(a_reg_encoder(VA));
  m_float_emit.FCSEL(b_and_c_reg_encoder(VD), b_and_c_reg_encoder(VC), b_and_c_reg_encoder(VB),
                     CC_GE);

  ASSERT_MSG(DYNA_REC, b_and_c_singles == (fpr.IsSingle(b, true) && fpr.IsSingle(c, true)),
             "Register allocation turned singles into doubles in the middle of fselx");
}

void JitArm64::frspx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  const u32 b = inst.FB;
  const u32 d = inst.FD;

  const bool single = fpr.IsSingle(b, true);
  if (single && js.fpr_is_store_safe[b])
  {
    // Source is already in single precision, so no need to do anything but to copy to PSR1.
    const ARM64Reg VB = fpr.R(b, RegType::LowerPairSingle);
    const ARM64Reg VD = fpr.RW(d, RegType::DuplicatedSingle);

    if (b != d)
      m_float_emit.FMOV(EncodeRegToSingle(VD), EncodeRegToSingle(VB));

    ASSERT_MSG(DYNA_REC, fpr.IsSingle(b, true),
               "Register allocation turned singles into doubles in the middle of frspx");

    SetFPRFIfNeeded(true, VD);
  }
  else
  {
    const ARM64Reg VB = fpr.R(b, RegType::LowerPair);
    const ARM64Reg VD = fpr.RW(d, RegType::DuplicatedSingle);

    m_float_emit.FCVT(32, 64, EncodeRegToDouble(VD), EncodeRegToDouble(VB));

    SetFPRFIfNeeded(true, VD);
  }
}

void JitArm64::FloatCompare(UGeckoInstruction inst, bool upper)
{
  const bool fprf = m_fprf && js.op->wantsFPRF;

  const u32 a = inst.FA;
  const u32 b = inst.FB;
  const int crf = inst.CRFD;

  // On the GC/Wii CPU, outputs are flushed to zero if FPSCR.NI is set, and inputs are never
  // flushed to zero. Ideally we would emulate FPSCR.NI by setting FPCR.FZ and FPCR.AH, but
  // unfortunately FPCR.AH is a very new feature that we can't rely on (as of 2021). For CPUs
  // without FPCR.AH, the best we can do (without killing the performance by explicitly flushing
  // outputs using bitwise operations) is to only set FPCR.FZ, which flushes both inputs and
  // outputs. This may cause problems in some cases, and one such case is Pokémon Battle Revolution,
  // which does not progress past the title screen if a denormal single compares equal to zero.
  // Workaround: Perform the comparison using a double operation instead. This ensures that denormal
  // singles behave correctly in comparisons, but we still have a problem with denormal doubles.
  const bool input_ftz_workaround =
      !cpu_info.bAFP && (!js.fpr_is_store_safe[a] || !js.fpr_is_store_safe[b]);

  const bool singles = fpr.IsSingle(a, !upper) && fpr.IsSingle(b, !upper) && !input_ftz_workaround;
  const RegType lower_type = singles ? RegType::LowerPairSingle : RegType::LowerPair;
  const RegType upper_type = singles ? RegType::Single : RegType::Register;
  const auto reg_encoder = singles ? EncodeRegToSingle : EncodeRegToDouble;
  const auto paired_reg_encoder = singles ? EncodeRegToDouble : EncodeRegToQuad;

  const bool upper_a = upper && !js.op->fprIsDuplicated[a];
  const bool upper_b = upper && !js.op->fprIsDuplicated[b];
  ARM64Reg VA = reg_encoder(fpr.R(a, upper_a ? upper_type : lower_type));
  ARM64Reg VB = reg_encoder(fpr.R(b, upper_b ? upper_type : lower_type));

  gpr.BindCRToRegister(crf, false);
  const ARM64Reg XA = gpr.CR(crf);

  Arm64GPRCache::ScopedARM64Reg fpscr_reg = ARM64Reg::INVALID_REG;
  if (fprf)
  {
    fpscr_reg = gpr.GetScopedReg();
    LDR(IndexType::Unsigned, fpscr_reg, PPC_REG, PPCSTATE_OFF(fpscr));
    AND(fpscr_reg, fpscr_reg, LogicalImm(~FPCC_MASK, GPRSize::B32));
  }

  {
    Arm64FPRCache::ScopedARM64Reg V0Q;
    Arm64FPRCache::ScopedARM64Reg V1Q;
    if (upper_a)
    {
      V0Q = fpr.GetScopedReg();
      m_float_emit.DUP(singles ? 32 : 64, paired_reg_encoder(V0Q), paired_reg_encoder(VA), 1);
      VA = reg_encoder(V0Q);
    }
    if (upper_b)
    {
      if (a == b)
      {
        VB = VA;
      }
      else
      {
        V1Q = fpr.GetScopedReg();
        m_float_emit.DUP(singles ? 32 : 64, paired_reg_encoder(V1Q), paired_reg_encoder(VB), 1);
        VB = reg_encoder(V1Q);
      }
    }

    m_float_emit.FCMP(VA, VB);
  }

  FixupBranch pNaN, pLesser, pGreater;
  FixupBranch continue1, continue2, continue3;

  if (a != b)
  {
    // if B > A goto Greater's jump target
    pGreater = B(CC_GT);
    // if B < A, goto Lesser's jump target
    pLesser = B(CC_MI);
  }

  pNaN = B(CC_VS);

  // A == B
  MOVI2R(XA, 0);
  if (fprf)
    ORR(fpscr_reg, fpscr_reg, LogicalImm(PowerPC::CR_EQ << FPRF_SHIFT, GPRSize::B32));

  continue1 = B();

  SetJumpTarget(pNaN);
  MOVI2R(XA, ~(1ULL << PowerPC::CR_EMU_LT_BIT));
  if (fprf)
    ORR(fpscr_reg, fpscr_reg, LogicalImm(PowerPC::CR_SO << FPRF_SHIFT, GPRSize::B32));

  if (a != b)
  {
    continue2 = B();

    SetJumpTarget(pGreater);
    MOVI2R(XA, 1);
    if (fprf)
      ORR(fpscr_reg, fpscr_reg, LogicalImm(PowerPC::CR_GT << FPRF_SHIFT, GPRSize::B32));

    continue3 = B();

    SetJumpTarget(pLesser);
    MOVI2R(XA, ~(1ULL << PowerPC::CR_EMU_SO_BIT));
    if (fprf)
      ORR(fpscr_reg, fpscr_reg, LogicalImm(PowerPC::CR_LT << FPRF_SHIFT, GPRSize::B32));

    SetJumpTarget(continue2);
    SetJumpTarget(continue3);
  }
  SetJumpTarget(continue1);

  ASSERT_MSG(DYNA_REC, singles == (fpr.IsSingle(a, true) && fpr.IsSingle(b, true)),
             "Register allocation turned singles into doubles in the middle of fcmpX");

  if (fprf)
  {
    STR(IndexType::Unsigned, fpscr_reg, PPC_REG, PPCSTATE_OFF(fpscr));
  }
}

void JitArm64::fcmpX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(jo.fp_exceptions);

  FloatCompare(inst);
}

void JitArm64::fctiwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  const u32 b = inst.FB;
  const u32 d = inst.FD;

  // With accurate NaNs (every Orca session), always from a double: its NaN check below, and no
  // flushed denormal single (IsSingleForArithmetic) under a directed rounding mode.
  const bool single = fpr.IsSingle(b, true) && !m_accurate_nans;
  const bool is_fctiwzx = inst.SUBOP10 == 15;

  const ARM64Reg VB = fpr.R(b, single ? RegType::LowerPairSingle : RegType::LowerPair);
  const ARM64Reg VD = fpr.RW(d, RegType::LowerPair);

  // TODO: The upper 32 bits of the result are set to 0xfff80000, except for -0.0 where should be
  // set to 0xfff80001 (TODO).

  if (single)
  {
    const auto V0 = fpr.GetScopedReg();

    if (is_fctiwzx)
    {
      m_float_emit.FCVTS(EncodeRegToSingle(VD), EncodeRegToSingle(VB), RoundingMode::Z);
    }
    else
    {
      m_float_emit.FRINTI(EncodeRegToSingle(VD), EncodeRegToSingle(VB));
      m_float_emit.FCVTS(EncodeRegToSingle(VD), EncodeRegToSingle(VD), RoundingMode::Z);
    }

    // Generate 0xFFF8'0000'0000'0000ULL
    m_float_emit.MOVI(64, EncodeRegToDouble(V0), 0xFFFF'0000'0000'0000ULL);
    m_float_emit.BIC(16, EncodeRegToDouble(V0), 0x7);

    m_float_emit.ORR(EncodeRegToDouble(VD), EncodeRegToDouble(VD), EncodeRegToDouble(V0));
  }
  else
  {
    const auto WA = gpr.GetScopedReg();
    Arm64GPRCache::ScopedARM64Reg WB = ARM64Reg::INVALID_REG;
    if (m_accurate_nans)
      WB = gpr.GetScopedReg();

    const ARM64Reg converted = is_fctiwzx ? EncodeRegToDouble(VB) : EncodeRegToDouble(VD);
    if (!is_fctiwzx)
      m_float_emit.FRINTI(EncodeRegToDouble(VD), EncodeRegToDouble(VB));
    m_float_emit.FCVTS(WA, converted, RoundingMode::Z);

    if (m_accurate_nans)
    {
      // Orca: a NaN converts to 0x80000000 on PowerPC (the interpreter) and Jit64; FCVTZS makes
      // it 0. FRINTI keeps a NaN a NaN, so test its result for one.
      m_float_emit.FCMP(converted);
      CSET(WB, CCFlags::CC_VS);
      ORR(WA, WA, WB, ArithOption(WB, ShiftType::LSL, 31));
    }

    ORR(EncodeRegTo64(WA), EncodeRegTo64(WA), LogicalImm(0xFFF8'0000'0000'0000ULL, GPRSize::B64));
    m_float_emit.FMOV(EncodeRegToDouble(VD), EncodeRegTo64(WA));
  }

  ASSERT_MSG(DYNA_REC, b == d || single == fpr.IsSingle(b, true),
             "Register allocation turned singles into doubles in the middle of fctiwzx");
}

void JitArm64::fresx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || jo.div_by_zero_exceptions);

  const u32 b = inst.FB;
  const u32 d = inst.FD;
  fpr.Lock(ARM64Reg::Q0);

  const ARM64Reg VB = fpr.R(b, RegType::LowerPair);
  const ARM64Reg VD = fpr.RW(d, RegType::Duplicated);

  gpr.Lock(ARM64Reg::W0, ARM64Reg::W1, ARM64Reg::W2, ARM64Reg::W3, ARM64Reg::W4, ARM64Reg::W30);

  m_float_emit.FMOV(ARM64Reg::X1, EncodeRegToDouble(VB));
  m_float_emit.FRECPE(ARM64Reg::D0, EncodeRegToDouble(VB));

  BL(GetAsmRoutines()->fres);

  m_float_emit.FMOV(EncodeRegToDouble(VD), ARM64Reg::X0);

  SetFPRFIfNeeded(false, ARM64Reg::X0);

  gpr.Unlock(ARM64Reg::W0, ARM64Reg::W1, ARM64Reg::W2, ARM64Reg::W3, ARM64Reg::W4, ARM64Reg::W30);
  fpr.Unlock(ARM64Reg::Q0);
}

void JitArm64::frsqrtex(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || jo.div_by_zero_exceptions);

  const u32 b = inst.FB;
  const u32 d = inst.FD;

  fpr.Lock(ARM64Reg::Q0);

  const ARM64Reg VB = fpr.R(b, RegType::LowerPair);
  const ARM64Reg VD = fpr.RW(d, RegType::LowerPair);

  gpr.Lock(ARM64Reg::W0, ARM64Reg::W1, ARM64Reg::W2, ARM64Reg::W3, ARM64Reg::W30);

  m_float_emit.FMOV(ARM64Reg::X1, EncodeRegToDouble(VB));
  m_float_emit.FRSQRTE(ARM64Reg::D0, EncodeRegToDouble(VB));

  BL(GetAsmRoutines()->frsqrte);

  m_float_emit.FMOV(EncodeRegToDouble(VD), ARM64Reg::X0);

  SetFPRFIfNeeded(false, ARM64Reg::X0);

  gpr.Unlock(ARM64Reg::W0, ARM64Reg::W1, ARM64Reg::W2, ARM64Reg::W3, ARM64Reg::W30);
  fpr.Unlock(ARM64Reg::Q0);
}

// Since the following float conversion functions are used in non-arithmetic PPC float
// instructions, they must convert floats bitexact and never flush denormals to zero or turn SNaNs
// into QNaNs. This means we can't just use FCVT/FCVTL/FCVTN.

void JitArm64::ConvertDoubleToSingleLower(size_t guest_reg, ARM64Reg dest_reg, ARM64Reg src_reg)
{
  if (js.fpr_is_store_safe[guest_reg] && js.op->fprIsSingle[guest_reg])
  {
    m_float_emit.FCVT(32, 64, EncodeRegToDouble(dest_reg), EncodeRegToDouble(src_reg));
    return;
  }

  FlushCarry();

  const BitSet32 gpr_saved = gpr.GetCallerSavedUsed() & BitSet32{0, 1, 2, 3, 30};
  ABI_PushRegisters(gpr_saved);

  m_float_emit.FMOV(ARM64Reg::X0, EncodeRegToDouble(src_reg));
  BL(cdts);
  m_float_emit.FMOV(EncodeRegToSingle(dest_reg), ARM64Reg::W1);

  ABI_PopRegisters(gpr_saved);
}

void JitArm64::ConvertDoubleToSinglePair(size_t guest_reg, ARM64Reg dest_reg, ARM64Reg src_reg)
{
  if (js.fpr_is_store_safe[guest_reg] && js.op->fprIsSingle[guest_reg])
  {
    m_float_emit.FCVTN(32, EncodeRegToDouble(dest_reg), EncodeRegToDouble(src_reg));
    return;
  }

  FlushCarry();

  const BitSet32 gpr_saved = gpr.GetCallerSavedUsed() & BitSet32{0, 1, 2, 3, 30};
  ABI_PushRegisters(gpr_saved);

  m_float_emit.FMOV(ARM64Reg::X0, EncodeRegToDouble(src_reg));
  BL(cdts);
  m_float_emit.UMOV(64, ARM64Reg::X0, src_reg, 1);
  m_float_emit.FMOV(EncodeRegToSingle(dest_reg), ARM64Reg::W1);
  BL(cdts);
  m_float_emit.INS(32, dest_reg, 1, ARM64Reg::W1);

  ABI_PopRegisters(gpr_saved);
}

void JitArm64::ConvertSingleToDoubleLower(size_t guest_reg, ARM64Reg dest_reg, ARM64Reg src_reg,
                                          ARM64Reg scratch_reg)
{
  ASSERT(scratch_reg != src_reg);

  if (js.fpr_is_store_safe[guest_reg])
  {
    m_float_emit.FCVT(64, 32, EncodeRegToDouble(dest_reg), EncodeRegToDouble(src_reg));
    return;
  }

  const bool switch_to_farcode = !IsInFarCode();

  FlushCarry();

  // Do we know that the input isn't NaN, and that the input isn't denormal or FPCR.FZ is not set?
  // (This check unfortunately also catches zeroes)

  FixupBranch fast;
  if (scratch_reg != ARM64Reg::INVALID_REG)
  {
    m_float_emit.FABS(EncodeRegToSingle(scratch_reg), EncodeRegToSingle(src_reg));
    m_float_emit.FCMP(EncodeRegToSingle(scratch_reg));
    fast = B(CCFlags::CC_GT);

    if (switch_to_farcode)
    {
      FixupBranch slow = B();

      SwitchToFarCode();
      SetJumpTarget(slow);
    }
  }

  // If no (or if we don't have a scratch register), call the bit-exact routine

  const BitSet32 gpr_saved = gpr.GetCallerSavedUsed() & BitSet32{0, 1, 2, 3, 4, 30};
  ABI_PushRegisters(gpr_saved);

  m_float_emit.FMOV(ARM64Reg::W0, EncodeRegToSingle(src_reg));
  BL(cstd);
  m_float_emit.FMOV(EncodeRegToDouble(dest_reg), ARM64Reg::X1);

  ABI_PopRegisters(gpr_saved);

  // If yes, do a fast conversion with FCVT

  if (scratch_reg != ARM64Reg::INVALID_REG)
  {
    FixupBranch continue1 = B();

    if (switch_to_farcode)
      SwitchToNearCode();

    SetJumpTarget(fast);

    m_float_emit.FCVT(64, 32, EncodeRegToDouble(dest_reg), EncodeRegToDouble(src_reg));

    SetJumpTarget(continue1);
  }
}

void JitArm64::ConvertSingleToDoublePair(size_t guest_reg, ARM64Reg dest_reg, ARM64Reg src_reg,
                                         ARM64Reg scratch_reg)
{
  ASSERT(scratch_reg != src_reg);

  if (js.fpr_is_store_safe[guest_reg])
  {
    m_float_emit.FCVTL(64, EncodeRegToDouble(dest_reg), EncodeRegToDouble(src_reg));
    return;
  }

  const bool switch_to_farcode = !IsInFarCode();

  FlushCarry();

  // Do we know that neither input is NaN, and that neither input is denormal or FPCR.FZ is not set?
  // (This check unfortunately also catches zeroes)

  FixupBranch fast;
  if (scratch_reg != ARM64Reg::INVALID_REG)
  {
    // Set each 32-bit element of scratch_reg to 0x0000'0000 or 0xFFFF'FFFF depending on whether
    // the absolute value of the corresponding element in src_reg compares greater than 0
    m_float_emit.MOVI(64, EncodeRegToDouble(scratch_reg), 0);
    m_float_emit.FACGT(32, EncodeRegToDouble(scratch_reg), EncodeRegToDouble(src_reg),
                       EncodeRegToDouble(scratch_reg));

    // 0x0000'0000'0000'0000 (zero)     -> 0x0000'0000'0000'0000 (zero)
    // 0x0000'0000'FFFF'FFFF (denormal) -> 0xFF00'0000'FFFF'FFFF (normal)
    // 0xFFFF'FFFF'0000'0000 (NaN)      -> 0x00FF'FFFF'0000'0000 (normal)
    // 0xFFFF'FFFF'FFFF'FFFF (NaN)      -> 0xFFFF'FFFF'FFFF'FFFF (NaN)
    m_float_emit.INS(8, EncodeRegToDouble(scratch_reg), 7, EncodeRegToDouble(scratch_reg), 0);

    // Is scratch_reg a NaN (0xFFFF'FFFF'FFFF'FFFF)?
    m_float_emit.FCMP(EncodeRegToDouble(scratch_reg));
    fast = B(CCFlags::CC_VS);

    if (switch_to_farcode)
    {
      FixupBranch slow = B();

      SwitchToFarCode();
      SetJumpTarget(slow);
    }
  }

  // If no (or if we don't have a scratch register), call the bit-exact routine

  const BitSet32 gpr_saved = gpr.GetCallerSavedUsed() & BitSet32{0, 1, 2, 3, 4, 30};
  ABI_PushRegisters(gpr_saved);

  m_float_emit.FMOV(ARM64Reg::W0, EncodeRegToSingle(src_reg));
  BL(cstd);
  m_float_emit.UMOV(32, ARM64Reg::W0, src_reg, 1);
  m_float_emit.FMOV(EncodeRegToDouble(dest_reg), ARM64Reg::X1);
  BL(cstd);
  m_float_emit.INS(64, dest_reg, 1, ARM64Reg::X1);

  ABI_PopRegisters(gpr_saved);

  // If yes, do a fast conversion with FCVTL

  if (scratch_reg != ARM64Reg::INVALID_REG)
  {
    FixupBranch continue1 = B();

    if (switch_to_farcode)
      SwitchToNearCode();

    SetJumpTarget(fast);
    m_float_emit.FCVTL(64, EncodeRegToDouble(dest_reg), EncodeRegToDouble(src_reg));

    SetJumpTarget(continue1);
  }
}

bool JitArm64::IsFPRStoreSafe(size_t guest_reg) const
{
  return js.fpr_is_store_safe[guest_reg];
}

bool JitArm64::DenormalInputsFlushed() const
{
  // cpu_info.bAFP is false on a CPU without FEAT_AFP and with ORCA_TEST_NO_AFP; FPCR.AH is set only
  // where it is true (Common/ArmFPURoundMode.cpp).
  return m_orca_session && !cpu_info.bAFP;
}

bool JitArm64::IsSingleForArithmetic(size_t guest_reg, bool lower_only) const
{
  // A store-safe value came out of a single-precision arithmetic operation in this block, which
  // flushed it if it was a denormal (PPCAnalyst, fprIsStoreSafe); with FPSCR.NI clear nothing is
  // flushed at all.
  return fpr.IsSingle(guest_reg, lower_only) &&
         (!DenormalInputsFlushed() || js.fpr_is_store_safe[guest_reg]);
}
