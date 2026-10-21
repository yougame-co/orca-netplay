// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/Jit64/Jit.h"

#include <optional>

#include "Common/MsgHandler.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"

using namespace Gen;

alignas(16) static const u64 psDefaultQNaN[2] = {0x7FF8000000000000ULL, 0x7FF8000000000000ULL};

void Jit64::ps_mr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);

  int d = inst.FD;
  int b = inst.FB;
  if (d == b)
    return;

  RCOpArg Rb = fpr.Use(b, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(Rb, Rd);
  MOVAPD(Rd, Rb);
}

void Jit64::ps_sum(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  int d = inst.FD;
  int a = inst.FA;
  int b = inst.FB;
  int c = inst.FC;

  RCOpArg Ra = fpr.Use(a, RCMode::Read);
  RCOpArg Rb = fpr.Use(b, RCMode::Read);
  RCOpArg Rc = fpr.Use(c, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(Ra, Rb, Rc, Rd);

  X64Reg tmp = XMM1;
  MOVDDUP(tmp, Ra);  // {a.ps0, a.ps0}
  ADDPD(tmp, Rb);    // {a.ps0 + b.ps0, a.ps0 + b.ps1}
  if (m_accurate_nans)
  {
    // Orca: x86-64 picks among NaN inputs as PowerPC does for an addition (a.ps0 first), but
    // inf - inf makes x86's default NaN, which is negative, where PowerPC's is positive. So a NaN
    // a.ps0 + b.ps1 is redone by PowerPC's rules: a.ps0 or else b.ps1 made quiet, or the positive
    // default NaN. (HandleNaNs can't: its paired form pairs each output lane with the same lane of
    // the inputs.)
    MOVHLPS(XMM0, tmp);
    UCOMISD(XMM0, R(XMM0));
    FixupBranch handle_nan = J_CC(CC_P, Jump::Near);
    SwitchToFarCode();
    SetJumpTarget(handle_nan);
    MOVDDUP(XMM0, Ra);
    UCOMISD(XMM0, R(XMM0));
    FixupBranch a_nan = J_CC(CC_P);
    MOVAPD(XMM0, Rb);
    UNPCKHPD(XMM0, R(XMM0));
    UCOMISD(XMM0, R(XMM0));
    FixupBranch b_nan = J_CC(CC_P);
    XORPD(XMM0, R(XMM0));  // finished into the default NaN below
    SetJumpTarget(a_nan);
    SetJumpTarget(b_nan);
    ORPD(XMM0, MConst(psDefaultQNaN));  // quiet
    UNPCKLPD(tmp, R(XMM0));             // {a.ps0 + b.ps0, the NaN}
    FixupBranch done = J(Jump::Near);
    SwitchToNearCode();
    SetJumpTarget(done);
  }
  switch (inst.SUBOP5)
  {
  case 10:  // ps_sum0: {a.ps0 + b.ps1, c.ps1}
    UNPCKHPD(tmp, Rc);
    break;
  case 11:  // ps_sum1: {c.ps0, a.ps0 + b.ps1}
    if (Rc.IsSimpleReg())
      MOVSD(tmp, Rc);
    else
      MOVLPD(tmp, Rc);
    break;
  default:
    PanicAlertFmt("ps_sum WTF!!!");
  }
  // Not HandleNaNs: see above.
  FinalizeSingleResult(Rd, R(tmp));
}

void Jit64::ps_muls(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  int d = inst.FD;
  int a = inst.FA;
  int c = inst.FC;
  bool round_input = !js.op->fprIsSingle[c];

  RCOpArg Ra = fpr.Use(a, RCMode::Read);
  RCOpArg Rc = fpr.Use(c, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RCX64Reg Rc_duplicated = m_accurate_nans ? fpr.Scratch() : fpr.Scratch(XMM1);
  RegCache::Realize(Ra, Rc, Rd, Rc_duplicated);

  switch (inst.SUBOP5)
  {
  case 12:  // ps_muls0
    MOVDDUP(Rc_duplicated, Rc);
    break;
  case 13:  // ps_muls1
    avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, Rc_duplicated, Rc, Rc, 3);
    break;
  default:
    PanicAlertFmt("ps_muls WTF!!!");
  }

  if (round_input)
    Force25BitPrecision(XMM1, R(Rc_duplicated), XMM0);
  else if (XMM1 != Rc_duplicated)
    MOVAPD(XMM1, Rc_duplicated);
  MULPD(XMM1, Ra);

  if (m_accurate_nans)
  {
    const FixupBranch handled_nans = HandleNaNs(inst, XMM1, XMM0, Ra, std::nullopt, Rc_duplicated);
    SetJumpTarget(handled_nans);
  }

  FinalizeSingleResult(Rd, R(XMM1));
}

void Jit64::ps_mergeXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);

  int d = inst.FD;
  int a = inst.FA;
  int b = inst.FB;

  RCOpArg Ra = fpr.Use(a, RCMode::Read);
  RCOpArg Rb = fpr.Use(b, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(Ra, Rb, Rd);

  switch (inst.SUBOP10)
  {
  case 528:
    avx_op(&XEmitter::VUNPCKLPD, &XEmitter::UNPCKLPD, Rd, Ra, Rb);
    break;  // 00
  case 560:
    if (d != b)
      avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, Rd, Ra, Rb, 2);
    else if (Ra.IsSimpleReg())
      MOVSD(Rd, Ra);
    else
      MOVLPD(Rd, Ra);
    break;  // 01
  case 592:
    avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, Rd, Ra, Rb, 1);
    break;  // 10
  case 624:
    avx_op(&XEmitter::VUNPCKHPD, &XEmitter::UNPCKHPD, Rd, Ra, Rb);
    break;  // 11
  default:
    ASSERT_MSG(DYNA_REC, 0, "ps_merge - invalid op");
  }
}

void Jit64::ps_rsqrte(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || jo.div_by_zero_exceptions);
  int b = inst.FB;
  int d = inst.FD;

  RCX64Reg scratch_guard = gpr.Scratch(RSCRATCH_EXTRA);
  RCX64Reg Rb = fpr.Bind(b, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(scratch_guard, Rb, Rd);

  MOVSD(XMM0, Rb);
  CALL(asm_routines.frsqrte);
  MOVSD(Rd, XMM0);

  MOVHLPS(XMM0, Rb);
  CALL(asm_routines.frsqrte);
  MOVLHPS(Rd, XMM0);

  FinalizeSingleResult(Rd, Rd);
}

void Jit64::ps_res(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || jo.div_by_zero_exceptions);
  int b = inst.FB;
  int d = inst.FD;

  RCX64Reg scratch_guard = gpr.Scratch(RSCRATCH_EXTRA);
  RCX64Reg Rb = fpr.Bind(b, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(scratch_guard, Rb, Rd);

  MOVSD(XMM0, Rb);
  CALL(asm_routines.fres);
  MOVSD(Rd, XMM0);

  MOVHLPS(XMM0, Rb);
  CALL(asm_routines.fres);
  MOVLHPS(Rd, XMM0);

  FinalizeSingleResult(Rd, Rd);
}

void Jit64::ps_cmpXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(jo.fp_exceptions);

  FloatCompare(inst, !!(inst.SUBOP10 & 64));
}
