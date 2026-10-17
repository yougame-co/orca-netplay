// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitArm64/Jit.h"

#include "Common/Arm64Emitter.h"
#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/JitArm64/JitArm64_RegCache.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"

using namespace Arm64Gen;

void JitArm64::psq_lXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStorePairedOff);

  // If fastmem is enabled, the asm routines assume address translation is on.
  FALLBACK_IF(!js.assumeNoPairedQuantize && jo.fastmem &&
              !(m_ppc_state.feature_flags & FEATURE_FLAG_MSR_DR));
  // Orca: in a session, send to the interpreter exactly the psq_l and psq_st that Jit64 does
  // (address translation off, or rA = 0), so a Mac and a PC run each one on the same core. The
  // interpreter's results differ from both JITs' (SNaNs, denormal flushing). Rarely hit in
  // practice.
  FALLBACK_IF(m_orca_session &&
              (inst.RA == 0 || !(m_ppc_state.feature_flags & FEATURE_FLAG_MSR_DR)));

  // X30 is LR
  // X0 is a temporary
  // X1 is the address
  // X2 is the scale
  // Q0 is the return register
  // Q1 is a temporary
  const s32 offset = inst.SIMM_12;
  const bool indexed = inst.OPCD == 4;
  const bool update = inst.OPCD == 57 || (inst.OPCD == 4 && !!(inst.SUBOP6 & 32));
  const int i = indexed ? inst.Ix : inst.I;
  const int w = indexed ? inst.Wx : inst.W;

  gpr.Lock(ARM64Reg::W1, ARM64Reg::W30);
  fpr.Lock(ARM64Reg::Q0);
  if (!js.assumeNoPairedQuantize)
  {
    gpr.Lock(ARM64Reg::W0, ARM64Reg::W2, ARM64Reg::W3);
    fpr.Lock(ARM64Reg::Q1);
  }
  else if (jo.memcheck || !jo.fastmem)
  {
    gpr.Lock(ARM64Reg::W0);
  }

  constexpr ARM64Reg type_reg = ARM64Reg::W0;
  constexpr ARM64Reg addr_reg = ARM64Reg::W1;
  constexpr ARM64Reg scale_reg = ARM64Reg::W2;
  ARM64Reg VS = fpr.RW(inst.RS, RegType::Single, false);

  if (inst.RA || update)  // Always uses the register on update
  {
    if (indexed)
      ADD(addr_reg, gpr.R(inst.RA), gpr.R(inst.RB));
    else
      ADDI2R(addr_reg, gpr.R(inst.RA), offset, addr_reg);
  }
  else
  {
    if (indexed)
      MOV(addr_reg, gpr.R(inst.RB));
    else
      MOVI2R(addr_reg, (u32)offset);
  }

  const bool early_update = !jo.memcheck;
  if (update && early_update)
  {
    gpr.BindToRegister(inst.RA, false);
    MOV(gpr.R(inst.RA), addr_reg);
  }

  if (js.assumeNoPairedQuantize)
  {
    BitSet32 gprs_in_use = gpr.GetCallerSavedUsed();
    BitSet32 fprs_in_use = fpr.GetCallerSavedUsed();

    // Wipe the registers we are using as temporaries
    if (!update || early_update)
      gprs_in_use[DecodeReg(ARM64Reg::W1)] = false;
    if (jo.memcheck || !jo.fastmem)
      gprs_in_use[DecodeReg(ARM64Reg::W0)] = false;
    fprs_in_use[DecodeReg(ARM64Reg::Q0)] = false;
    if (!jo.memcheck)
      fprs_in_use[DecodeReg(VS)] = false;

    u32 flags = BackPatchInfo::FLAG_LOAD | BackPatchInfo::FLAG_FLOAT | BackPatchInfo::FLAG_SIZE_32;
    if (!w)
      flags |= BackPatchInfo::FLAG_PAIR;

    EmitBackpatchRoutine(flags, MemAccessMode::Auto, VS, EncodeRegTo64(addr_reg), gprs_in_use,
                         fprs_in_use);
  }
  else
  {
    LDR(IndexType::Unsigned, scale_reg, PPC_REG, PPCSTATE_OFF_SPR(SPR_GQR0 + i));

    FlushPPCStateBeforeSlowAccess(ARM64Reg::W30, ARM64Reg::Q1);

    UBFM(type_reg, scale_reg, 16, 18);   // Type
    UBFM(scale_reg, scale_reg, 24, 29);  // Scale

    MOVP2R(ARM64Reg::X30, w ? single_load_quantized : paired_load_quantized);
    LDR(EncodeRegTo64(type_reg), ARM64Reg::X30, ArithOption(EncodeRegTo64(type_reg), true));
    BLR(EncodeRegTo64(type_reg));

    WriteConditionalExceptionExit(EXCEPTION_DSI, ARM64Reg::W30, ARM64Reg::Q1);

    m_float_emit.ORR(EncodeRegToDouble(VS), ARM64Reg::D0, ARM64Reg::D0);
  }

  if (w)
  {
    m_float_emit.FMOV(ARM64Reg::S0, 0x70);  // 1.0 as a Single
    m_float_emit.INS(32, VS, 1, ARM64Reg::Q0, 0);
  }

  if (m_orca_session)
  {
    // Orca: Jit64 widens psq_l's singles with CVTPS2PD, which quiets an SNaN and keeps everything
    // else exact. JitArm64 keeps the raw bits, so in a session set the quiet bit of each NaN lane
    // here to match. FCMEQ only compares (a denormal still equals itself under FPCR.FZ), so other
    // values stay bit for bit. Only a float type can load a NaN.
    const ARM64Reg VS_d = EncodeRegToDouble(VS);
    const auto quiet_reg = fpr.GetScopedReg();
    const ARM64Reg quiet = EncodeRegToDouble(quiet_reg);
    m_float_emit.FCMEQ(32, ARM64Reg::D0, VS_d, VS_d);  // the lanes that are numbers: all ones
    m_float_emit.ORR(quiet, VS_d, VS_d);
    m_float_emit.ORR(32, quiet, 0x40, 16);        // every lane with the quiet bit (0x00400000)
    m_float_emit.BIF(VS_d, quiet, ARM64Reg::D0);  // which the NaN lanes take
  }

  const ARM64Reg VS_again = fpr.RW(inst.RS, RegType::Single, true);
  ASSERT(VS == VS_again);

  if (update && !early_update)
  {
    gpr.BindToRegister(inst.RA, false);
    MOV(gpr.R(inst.RA), addr_reg);
  }

  gpr.Unlock(ARM64Reg::W1, ARM64Reg::W30);
  fpr.Unlock(ARM64Reg::Q0);
  if (!js.assumeNoPairedQuantize)
  {
    gpr.Unlock(ARM64Reg::W0, ARM64Reg::W2, ARM64Reg::W3);
    fpr.Unlock(ARM64Reg::Q1);
  }
  else if (jo.memcheck || !jo.fastmem)
  {
    gpr.Unlock(ARM64Reg::W0);
  }
}

void JitArm64::psq_stXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStorePairedOff);

  // If fastmem is enabled, the asm routines assume address translation is on.
  FALLBACK_IF(!js.assumeNoPairedQuantize && jo.fastmem &&
              !(m_ppc_state.feature_flags & FEATURE_FLAG_MSR_DR));
  // Orca: as Jit64 does it, in a session (see psq_lXX).
  FALLBACK_IF(m_orca_session &&
              (inst.RA == 0 || !(m_ppc_state.feature_flags & FEATURE_FLAG_MSR_DR)));

  // X30 is LR
  // X0 is a temporary
  // X1 is the scale
  // X2 is the address
  // Q0 is the store register

  const s32 offset = inst.SIMM_12;
  const bool indexed = inst.OPCD == 4;
  const bool update = inst.OPCD == 61 || (inst.OPCD == 4 && !!(inst.SUBOP6 & 32));
  const int i = indexed ? inst.Ix : inst.I;
  const int w = indexed ? inst.Wx : inst.W;

  fpr.Lock(ARM64Reg::Q0);
  if (!js.assumeNoPairedQuantize)
    fpr.Lock(ARM64Reg::Q1);

  // Orca: in a session, a single is stored bit for bit only when it is store-safe (an arithmetic
  // result). Any other (loaded by psq_l, say) is rounded to single as Jit64 does it (CVTPD2PS):
  // a denormal flushes in the guest's non-IEEE mode and an SNaN goes quiet.
  const bool have_single =
      fpr.IsSingle(inst.RS) && (!m_orca_session || js.fpr_is_store_safe[inst.RS]);

  Arm64FPRCache::ScopedARM64Reg VS =
      fpr.R(inst.RS, have_single ? RegType::Single : RegType::Register);

  if (js.assumeNoPairedQuantize)
  {
    if (!have_single)
    {
      auto single_reg = fpr.GetScopedReg();

      if (w)
        m_float_emit.FCVT(32, 64, EncodeRegToDouble(single_reg), EncodeRegToDouble(VS));
      else
        m_float_emit.FCVTN(32, EncodeRegToDouble(single_reg), EncodeRegToDouble(VS));

      VS = std::move(single_reg);
    }
  }
  else
  {
    if (have_single)
    {
      m_float_emit.ORR(ARM64Reg::D0, VS, VS);
    }
    else
    {
      if (w)
        m_float_emit.FCVT(32, 64, ARM64Reg::D0, VS);
      else
        m_float_emit.FCVTN(32, ARM64Reg::D0, VS);
    }
  }

  gpr.Lock(ARM64Reg::W1, ARM64Reg::W2, ARM64Reg::W30);
  if (!js.assumeNoPairedQuantize || !jo.fastmem)
    gpr.Lock(ARM64Reg::W0);
  if (!js.assumeNoPairedQuantize && !jo.fastmem)
    gpr.Lock(ARM64Reg::W3);

  constexpr ARM64Reg type_reg = ARM64Reg::W0;
  constexpr ARM64Reg scale_reg = ARM64Reg::W1;
  constexpr ARM64Reg addr_reg = ARM64Reg::W2;

  if (inst.RA || update)  // Always uses the register on update
  {
    if (indexed)
      ADD(addr_reg, gpr.R(inst.RA), gpr.R(inst.RB));
    else
      ADDI2R(addr_reg, gpr.R(inst.RA), offset, addr_reg);
  }
  else
  {
    if (indexed)
      MOV(addr_reg, gpr.R(inst.RB));
    else
      MOVI2R(addr_reg, (u32)offset);
  }

  const bool early_update = !jo.memcheck;
  if (update && early_update)
  {
    gpr.BindToRegister(inst.RA, false);
    MOV(gpr.R(inst.RA), addr_reg);
  }

  if (js.assumeNoPairedQuantize)
  {
    BitSet32 gprs_in_use = gpr.GetCallerSavedUsed();
    BitSet32 fprs_in_use = fpr.GetCallerSavedUsed();

    // Wipe the registers we are using as temporaries
    gprs_in_use[DecodeReg(ARM64Reg::W1)] = false;
    if (!update || early_update)
      gprs_in_use[DecodeReg(ARM64Reg::W2)] = false;
    if (!jo.fastmem)
      gprs_in_use[DecodeReg(ARM64Reg::W0)] = false;

    u32 flags = BackPatchInfo::FLAG_STORE | BackPatchInfo::FLAG_FLOAT | BackPatchInfo::FLAG_SIZE_32;
    if (!w)
      flags |= BackPatchInfo::FLAG_PAIR;

    EmitBackpatchRoutine(flags, MemAccessMode::Auto, VS, EncodeRegTo64(addr_reg), gprs_in_use,
                         fprs_in_use);
  }
  else
  {
    LDR(IndexType::Unsigned, scale_reg, PPC_REG, PPCSTATE_OFF_SPR(SPR_GQR0 + i));

    FlushPPCStateBeforeSlowAccess(ARM64Reg::W30, ARM64Reg::Q1);

    UBFM(type_reg, scale_reg, 0, 2);    // Type
    UBFM(scale_reg, scale_reg, 8, 13);  // Scale

    MOVP2R(ARM64Reg::X30, w ? single_store_quantized : paired_store_quantized);
    LDR(EncodeRegTo64(type_reg), ARM64Reg::X30, ArithOption(EncodeRegTo64(type_reg), true));
    BLR(EncodeRegTo64(type_reg));

    WriteConditionalExceptionExit(EXCEPTION_DSI, ARM64Reg::W30, ARM64Reg::Q1);
  }

  if (update && !early_update)
  {
    gpr.BindToRegister(inst.RA, false);
    MOV(gpr.R(inst.RA), addr_reg);
  }

  gpr.Unlock(ARM64Reg::W1, ARM64Reg::W2, ARM64Reg::W30);
  fpr.Unlock(ARM64Reg::Q0);
  if (!js.assumeNoPairedQuantize || !jo.fastmem)
    gpr.Unlock(ARM64Reg::W0);
  if (!js.assumeNoPairedQuantize && !jo.fastmem)
    gpr.Unlock(ARM64Reg::W3);
  if (!js.assumeNoPairedQuantize)
    fpr.Unlock(ARM64Reg::Q1);
}
