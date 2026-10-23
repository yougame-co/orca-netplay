// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca: the host's JIT (JitArm64 or Jit64) against the interpreter with an Orca session's float
// setup: accurate NaNs, the guest's non-IEEE mode (FPSCR.NI) and the session's guards for a CPU
// that flushes denormal inputs (JitArm64::DenormalInputsFlushed). Every FPR, CR and stored word
// must match the interpreter bit for bit (but where a case says otherwise), and some results are
// pinned to PowerPC's answer whatever the host:
// - a NaN an operation makes from no NaN input (inf * 0, inf - inf, 0 / 0) is positive,
//   0x7FF8000000000000; x86-64 and an ARM CPU with FPCR.AH (Apple M4 and later) make it negative;
// - the first NaN input in PowerPC's order (a, b, c) wins, made quiet, and is never negated;
// - a denormal single that was loaded (lfs, psq_l) counts as itself in arithmetic, compares and
//   conversions, where an ARM CPU without FEAT_AFP (Apple M1 to M3) flushes it as an input;
// - fctiw(z) of a NaN is 0x80000000; frsqrte of a negative number is the default NaN.
// The FPSCR bits the JITs write themselves (ZX and VXSQRT, with FX) must match too: mffs and an OS
// context save carry them into RAM. Address translation is on (a BAT maps the first 256 MB as
// itself), as in games, so Jit64 compiles psq_l and psq_st instead of falling back.
// On an Apple M4 or later, run it a second time with ORCA_TEST_NO_AFP=1 to cover an M1 to M3.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FPURoundMode.h"
#include "Common/FileUtil.h"
#include "Common/FloatUtils.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"

#include <gtest/gtest.h>

namespace
{
#if defined(_M_X86_64)
constexpr PowerPC::CPUCore HOST_JIT = PowerPC::CPUCore::JIT64;
constexpr bool ARM_JIT = false;
#else
constexpr PowerPC::CPUCore HOST_JIT = PowerPC::CPUCore::JITARM64;
constexpr bool ARM_JIT = true;
#endif

constexpr u32 BRANCH_TO_SELF = 0x48000000;
// Each program gets its own address across the whole suite, so neither core reuses a block or a
// cache line.
constexpr u32 CODE_BASE = 0x00010000;
constexpr u32 CODE_STRIDE = 0x40;
constexpr u32 DATA_BASE = 0x00100000;  // r5
constexpr u32 DATA_WORDS = 20;

constexpr u32 A_FORM(u32 op, u32 xo, u32 d, u32 a, u32 b, u32 c)
{
  return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1);
}
constexpr u32 X_FORM(u32 op, u32 xo, u32 d, u32 a, u32 b)
{
  return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1);
}
constexpr u32 D_FORM(u32 op, u32 d, u32 a, u32 offset)
{
  return (op << 26) | (d << 21) | (a << 16) | (offset & 0xFFFF);
}
constexpr u32 LFS(u32 d, u32 offset)
{
  return D_FORM(48, d, 5, offset);
}
constexpr u32 STFS(u32 s, u32 offset)
{
  return D_FORM(52, s, 5, offset);
}
constexpr u32 STFD(u32 s, u32 offset)
{
  return D_FORM(54, s, 5, offset);
}
constexpr u32 FMR(u32 d, u32 b)
{
  return (63u << 26) | (d << 21) | (b << 11) | (72u << 1);
}
// GQR0 is zero: float pairs, no scale.
constexpr u32 PSQ_L(u32 d, u32 offset)
{
  return (56u << 26) | (d << 21) | (5u << 16) | (offset & 0xFFF);
}
constexpr u32 PSQ_ST(u32 s, u32 offset, u32 w = 0)
{
  return (60u << 26) | (s << 21) | (5u << 16) | (w << 15) | (offset & 0xFFF);
}
// psq_lx/psq_stx with rA = 0: the address is r5 alone.
constexpr u32 PSQ_LX_RA0(u32 d)
{
  return (4u << 26) | (d << 21) | (5u << 11) | (6u << 1);
}
constexpr u32 PSQ_STX_RA0(u32 s)
{
  return (4u << 26) | (s << 21) | (5u << 11) | (7u << 1);
}

// Opcodes and extended opcodes
constexpr u32 OP_DOUBLE = 63, OP_SINGLE = 59, OP_PAIRED = 4;
constexpr u32 XO_DIV = 18, XO_SUB = 20, XO_ADD = 21, XO_SEL = 23, XO_RES = 24, XO_MUL = 25,
              XO_RSQRTE = 26, XO_MSUB = 28, XO_MADD = 29, XO_NMSUB = 30, XO_NMADD = 31;
constexpr u32 XO_SUM0 = 10, XO_SUM1 = 11, XO_MULS0 = 12, XO_MULS1 = 13, XO_MADDS0 = 14,
              XO_MADDS1 = 15;

constexpr u64 ZERO = 0;
constexpr u64 NEG_ZERO = 0x8000'0000'0000'0000;
constexpr u64 ONE = 0x3FF0'0000'0000'0000;
constexpr u64 TWO = 0x4000'0000'0000'0000;
constexpr u64 MINUS_ONE = 0xBFF0'0000'0000'0000;
constexpr u64 INF = 0x7FF0'0000'0000'0000;
constexpr u64 NINF = 0xFFF0'0000'0000'0000;
constexpr u64 PPC_NAN = 0x7FF8'0000'0000'0000;
// NaNs whose payloads survive rounding to single, so single and paired ops keep them whole.
constexpr u64 QNAN_A = 0x7FF8'1000'0000'0000;
constexpr u64 QNAN_B = 0xFFF8'2000'0000'0000;  // negative
constexpr u64 QNAN_C = 0x7FF8'4000'0000'0000;
constexpr u64 SNAN_C = 0x7FF0'8000'0000'0000;  // quiet: 0x7FF8'8000'0000'0000
constexpr u64 SNAN_C_QUIET = 0x7FF8'8000'0000'0000;

// Singles in memory
constexpr u32 S_DENORMAL = 0x0040'0000;      // 2^-127
constexpr u32 S_NEG_DENORMAL = 0x8040'0000;  // -2^-127
constexpr u32 S_2_100 = 0x7180'0000;         // 2^100
constexpr u32 S_ONE = 0x3F80'0000;
constexpr u32 S_SNAN = 0x7F80'0001;
constexpr u32 S_INF = 0x7F80'0000;
constexpr u64 D_2_M27 = 0x3E40'0000'0000'0000;   // 2^-127 * 2^100
constexpr u64 D_M2_M27 = 0xBE40'0000'0000'0000;  // -2^-127 * 2^100

void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
  _putenv_s(name, value);
#else
  if (*value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

bool IsNaN(u64 value)
{
  return std::isnan(std::bit_cast<double>(value));
}

u64 Fill(int reg)
{
  return std::bit_cast<u64>(100.0 + reg);
}

struct Result
{
  std::array<u64, 32> ps0{};
  std::array<u64, 32> ps1{};
  u32 cr = 0;
  u32 fpscr = 0;
  std::array<u32, DATA_WORDS> data{};
};

struct Case
{
  std::string name;
  std::vector<u32> code;
  // Registers set before the run as {reg, ps0, ps1}; the others hold Fill(reg) in both halves.
  std::vector<std::array<u64, 3>> fprs;
  // Words at DATA_BASE (r5)
  std::vector<std::pair<u32, u32>> data;
  u32 rounding_mode = 0;  // FPSCR.RN
  u32 gqr0 = 0;           // GQR0: float pairs, no scale, unless a case says
  // Pinned results, checked on the JIT's run.
  std::function<void(const Result&, const std::string&)> expect;
  // Stored words (by index) the JIT may store differently from the interpreter.
  std::vector<u32> data_differs_from_interpreter;
  // Registers the JIT may hold differently from the interpreter (both halves).
  std::vector<int> fprs_differ_from_interpreter;
  // The interpreter's own answer depends on the host here: on a CPU that flushes denormal inputs
  // (DenormalInputsFlushed) its C++ compares read a double denormal as zero, where the JIT, Jit64
  // and the interpreter elsewhere don't. The pinned expectation still holds.
  bool interpreter_flushes_here = false;
};

// The FPSCR exception bits a JIT sets itself in an Orca session (fres, frsqrte and their paired
// forms). The JITs leave the others (VXISI, VXIMZ, VXSNAN, ZX of fdiv...) alone with float
// exceptions off, where the interpreter sets them, so only these are compared.
constexpr u32 JIT_FPSCR_BITS = FPSCR_ZX | FPSCR_VXSQRT;

class JitFloatSemantics : public testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    // The JIT reads it (JitBase::RefreshConfig): an Orca session's JIT output.
    s_had_session = std::getenv("ORCA_SESSION") != nullptr;
    if (!s_had_session)
      SetEnv("ORCA_SESSION", "1");
    s_user_dir = File::CreateTempDir();
    Core::DeclareAsCPUThread();
    UICommon::SetUserDirectory(s_user_dir);
    Config::Init();
    SConfig::Init();
    Config::SetCurrent(Config::MAIN_FASTMEM, false);
    Config::SetCurrent(Config::MAIN_FASTMEM_ARENA, false);
    Config::SetCurrent(Config::MAIN_ACCURATE_NANS, true);
    auto& system = Core::System::GetInstance();
    system.GetMemory().Init();
    system.GetPowerPC().Init(HOST_JIT);
    system.GetCoreTiming().Init();
    const Core::CPUThreadGuard guard(system);
    system.GetJitInterface().ClearCache(guard);
  }

  static void TearDownTestSuite()
  {
    auto& system = Core::System::GetInstance();
    system.GetCoreTiming().Shutdown();
    system.GetPowerPC().Shutdown();
    system.GetMemory().Shutdown();
    SConfig::Shutdown();
    Config::Shutdown();
    Common::FPU::LoadDefaultSIMDState();
    Core::UndeclareAsCPUThread();
    File::DeleteDirRecursively(s_user_dir);
    if (!s_had_session)
      SetEnv("ORCA_SESSION", "");
  }

  static Result Run(const Case& c, u32 address, bool jit)
  {
    auto& system = Core::System::GetInstance();
    auto& memory = system.GetMemory();
    auto& power_pc = system.GetPowerPC();
    auto& ppc = system.GetPPCState();

    for (std::size_t i = 0; i < c.code.size(); ++i)
      memory.Write_U32(c.code[i], address + static_cast<u32>(i * 4));
    memory.Write_U32(BRANCH_TO_SELF, address + static_cast<u32>(c.code.size() * 4));
    for (u32 i = 0; i < DATA_WORDS; ++i)
      memory.Write_U32(0xDEAD0000 | i, DATA_BASE + i * 4);
    for (const auto& [index, word] : c.data)
      memory.Write_U32(word, DATA_BASE + index * 4);

    for (int reg = 0; reg < 32; ++reg)
      ppc.ps[reg].SetBoth(Fill(reg), Fill(reg));
    for (const auto& [reg, ps0, ps1] : c.fprs)
      ppc.ps[reg].SetBoth(ps0, ps1);
    ppc.gpr[5] = DATA_BASE;
    ppc.cr.Set(0);
    ppc.spr[SPR_GQR0] = c.gqr0;
    ppc.fpscr.Hex = 0;
    ppc.fpscr.NI = 1;
    ppc.fpscr.RN = static_cast<Common::FPU::RoundMode>(c.rounding_mode);
    PowerPC::RoundingModeUpdated(ppc);
    // DBAT0: effective 0x00000000-0x0FFFFFFF to the same physical addresses, read/write.
    ppc.spr[SPR_DBAT0U] = 0x0000'1FFF;
    ppc.spr[SPR_DBAT0L] = 0x0000'0002;
    system.GetMMU().DBATUpdated();
    ppc.msr.Hex = 0;
    ppc.msr.FP = 1;
    ppc.msr.DR = 1;
    HID2(ppc).PSE = 1;
    HID2(ppc).LSQE = 1;
    power_pc.MSRUpdated();
    ppc.pc = address;
    ppc.npc = address + 4;

    if (jit)
    {
      power_pc.SetMode(PowerPC::CoreMode::JIT);
      power_pc.SingleStep();
      auto* const jit_core = static_cast<JitBase*>(system.GetJitInterface().GetCore());
      EXPECT_NE(jit_core->GetBlockCache()->GetBlockFromStartAddress(address, ppc.feature_flags),
                nullptr)
          << fmt::format("{}: no JIT block at {:#x}", c.name, address);
    }
    else
    {
      power_pc.SetMode(PowerPC::CoreMode::Interpreter);
      for (std::size_t i = 0; i < c.code.size(); ++i)
        power_pc.SingleStep();
    }

    Result out;
    for (int reg = 0; reg < 32; ++reg)
    {
      out.ps0[reg] = ppc.ps[reg].PS0AsU64();
      out.ps1[reg] = ppc.ps[reg].PS1AsU64();
    }
    out.cr = ppc.cr.Get();
    out.fpscr = ppc.fpscr.Hex;
    for (u32 i = 0; i < DATA_WORDS; ++i)
      out.data[i] = memory.Read_U32(DATA_BASE + i * 4);

    ppc.fpscr.Hex = 0;
    PowerPC::RoundingModeUpdated(ppc);
    return out;
  }

  void Check(const Case& c)
  {
    const u32 address = CODE_BASE + s_next_program++ * CODE_STRIDE;
    ASSERT_LT(address, DATA_BASE);
    ASSERT_LE(c.code.size(), CODE_STRIDE / 4 - 1);
    const Result expected = Run(c, address, false);
    const Result actual = Run(c, address, true);
    const std::string where = fmt::format("{} (AFP {}, input flush guard {})", c.name,
                                          cpu_info.bAFP, ARM_JIT && !cpu_info.bAFP);
    const bool skip_fprs = c.interpreter_flushes_here && ARM_JIT && !cpu_info.bAFP;
    for (int reg = 0; reg < 32 && !skip_fprs; ++reg)
    {
      if (std::find(c.fprs_differ_from_interpreter.begin(), c.fprs_differ_from_interpreter.end(),
                    reg) != c.fprs_differ_from_interpreter.end())
      {
        continue;
      }
      EXPECT_EQ(expected.ps0[reg], actual.ps0[reg])
          << where
          << fmt::format(": f{} ps0 interpreter {:016x}, JIT {:016x}", reg, expected.ps0[reg],
                         actual.ps0[reg]);
      EXPECT_EQ(expected.ps1[reg], actual.ps1[reg])
          << where
          << fmt::format(": f{} ps1 interpreter {:016x}, JIT {:016x}", reg, expected.ps1[reg],
                         actual.ps1[reg]);
    }
    EXPECT_EQ(expected.cr, actual.cr) << where << ": CR";
    if (!skip_fprs)
    {
      EXPECT_EQ(expected.fpscr & JIT_FPSCR_BITS, actual.fpscr & JIT_FPSCR_BITS)
          << where
          << fmt::format(": FPSCR interpreter {:08x}, JIT {:08x}", expected.fpscr, actual.fpscr);
    }
    if (actual.fpscr & JIT_FPSCR_BITS)
      EXPECT_NE(0u, actual.fpscr & FPSCR_FX) << where << fmt::format(": FPSCR {:08x}", actual.fpscr);
    for (u32 i = 0; i < DATA_WORDS; ++i)
    {
      bool skip = false;
      for (const u32 index : c.data_differs_from_interpreter)
        skip = skip || index == i;
      if (!skip)
      {
        EXPECT_EQ(expected.data[i], actual.data[i])
            << where
            << fmt::format(": word {} interpreter {:08x}, JIT {:08x}", i, expected.data[i],
                           actual.data[i]);
      }
    }
    if (c.expect)
      c.expect(actual, where);
  }

  static inline u32 s_next_program = 0;
  static inline std::string s_user_dir;
  static inline bool s_had_session = false;
};

// d = 1, a = 2, b = 3, c = 4
std::function<void(const Result&, const std::string&)> ExpectF1(u64 ps0, u64 ps1)
{
  return [ps0, ps1](const Result& r, const std::string& where) {
    EXPECT_EQ(ps0, r.ps0[1]) << where << fmt::format(": f1 ps0 {:016x}", r.ps0[1]);
    EXPECT_EQ(ps1, r.ps1[1]) << where << fmt::format(": f1 ps1 {:016x}", r.ps1[1]);
  };
}
std::function<void(const Result&, const std::string&)> ExpectF1Ps0(u64 ps0)
{
  return [ps0](const Result& r, const std::string& where) {
    EXPECT_EQ(ps0, r.ps0[1]) << where << fmt::format(": f1 ps0 {:016x}", r.ps0[1]);
  };
}
}  // namespace

// A NaN made from no NaN input is PowerPC's positive default, in every scalar form, and fnmadd and
// fnmsub never negate a NaN (Project+ runs fnmsubs with inf * 0 as a match starts).
TEST_F(JitFloatSemantics, GeneratedNaNIsPositive)
{
  struct Op
  {
    const char* name;
    u32 xo;
    u64 a, b, c;
  };
  const Op ops[] = {
      {"add inf + -inf", XO_ADD, INF, NINF, ONE},
      {"sub inf - inf", XO_SUB, INF, INF, ONE},
      {"mul inf * 0", XO_MUL, INF, ONE, ZERO},
      {"div 0 / 0", XO_DIV, ZERO, ZERO, ONE},
      {"div inf / inf", XO_DIV, INF, INF, ONE},
      {"madd inf * 0 + 1", XO_MADD, INF, ONE, ZERO},
      {"madd 1 * inf + -inf", XO_MADD, ONE, NINF, INF},
      {"msub inf * 0 - 1", XO_MSUB, INF, ONE, ZERO},
      {"nmadd inf * 0 + 1", XO_NMADD, INF, ONE, ZERO},
      {"nmsub inf * 0 - 1", XO_NMSUB, INF, ONE, ZERO},
      {"nmsub 1 * inf - inf", XO_NMSUB, ONE, INF, INF},
  };
  for (const Op& op : ops)
  {
    for (const u32 opcode : {OP_DOUBLE, OP_SINGLE})
    {
      const bool single = opcode == OP_SINGLE;
      // Each register aliasing: d apart, d == a, d == b (where there is a b), d == c.
      for (const u32 d : {1u, 2u, 3u, 4u})
      {
        Case c;
        const bool uses_b = op.xo != XO_MUL;
        const bool uses_c = op.xo == XO_MUL || op.xo >= XO_MSUB;
        if (d == 3 && !uses_b)
          continue;
        c.name = fmt::format("f{}{} d=f{}", op.name, single ? " (single)" : "", d);
        c.code = {A_FORM(opcode, op.xo, d, 2, uses_b ? 3 : 0, uses_c ? 4 : 0)};
        c.fprs = {{2, op.a, op.a}, {3, op.b, op.b}, {4, op.c, op.c}};
        c.expect = [d, single](const Result& r, const std::string& where) {
          EXPECT_EQ(PPC_NAN, r.ps0[d]) << where << fmt::format(": ps0 {:016x}", r.ps0[d]);
          if (single)
            EXPECT_EQ(PPC_NAN, r.ps1[d]) << where << fmt::format(": ps1 {:016x}", r.ps1[d]);
        };
        Check(c);
      }
    }
  }
}

// The same from singles: loaded (lfs, psq_l), which an AFP CPU computes in single precision, and
// made by single-precision arithmetic (store-safe), which every ARM CPU does.
TEST_F(JitFloatSemantics, GeneratedNaNFromSingles)
{
  const std::vector<std::pair<u32, u32>> data = {{0, S_INF}, {1, S_ONE}, {2, 0}, {3, S_ONE}};
  struct Op
  {
    const char* name;
    u32 xo;
  };
  // a = inf, b = 1, c = 0: inf * 0 makes the NaN in every op with c.
  for (const Op& op : {Op{"madds", XO_MADD}, Op{"msubs", XO_MSUB}, Op{"nmadds", XO_NMADD},
                       Op{"nmsubs", XO_NMSUB}, Op{"muls", XO_MUL}})
  {
    const u32 b = op.xo == XO_MUL ? 0 : 3;
    Case loaded;
    loaded.name = fmt::format("f{} of lfs inputs", op.name);
    loaded.code = {LFS(2, 0), LFS(3, 4), LFS(4, 8), A_FORM(OP_SINGLE, op.xo, 1, 2, b, 4)};
    loaded.data = data;
    loaded.expect = ExpectF1(PPC_NAN, PPC_NAN);
    Check(loaded);

    // f2 = inf * 1, f4 = 0 * 1, f3 = 1 * 1, all single-precision results.
    Case computed;
    computed.name = fmt::format("f{} of single results", op.name);
    computed.code = {A_FORM(OP_SINGLE, XO_MUL, 2, 5, 0, 6), A_FORM(OP_SINGLE, XO_MUL, 4, 7, 0, 6),
                     A_FORM(OP_SINGLE, XO_MUL, 3, 6, 0, 6), A_FORM(OP_SINGLE, op.xo, 1, 2, b, 4)};
    computed.fprs = {{5, INF, INF}, {6, ONE, ONE}, {7, ZERO, ZERO}};
    computed.expect = ExpectF1(PPC_NAN, PPC_NAN);
    Check(computed);

    Case paired;
    paired.name = fmt::format("ps_{} of psq_l inputs, NaN in ps0", op.name);
    // a = {inf, 1}, b = {1, 1}... from words 0-1 and 1-2 and 2-3: a {inf, 1}, b {1, 0}, c {0, 1}
    paired.code = {PSQ_L(2, 0), PSQ_L(3, 4), PSQ_L(4, 8),
                   A_FORM(OP_PAIRED, op.xo == XO_MUL ? XO_MUL : op.xo, 1, 2, b, 4)};
    paired.data = data;
    paired.expect = [](const Result& r, const std::string& where) {
      EXPECT_EQ(PPC_NAN, r.ps0[1]) << where << fmt::format(": ps0 {:016x}", r.ps0[1]);
      EXPECT_FALSE(IsNaN(r.ps1[1])) << where << fmt::format(": ps1 {:016x}", r.ps1[1]);
    };
    Check(paired);
  }
}

// The first NaN input in PowerPC's order (a, b, c, though fmadd computes a * c + b), made quiet,
// never negated.
TEST_F(JitFloatSemantics, NaNPropagationOrder)
{
  struct Inputs
  {
    const char* name;
    u64 a, b, c, result;
  };
  const Inputs inputs[] = {
      {"a b c NaN", QNAN_A, QNAN_B, QNAN_C, QNAN_A}, {"b c NaN", ONE, QNAN_B, QNAN_C, QNAN_B},
      {"c NaN", ONE, ONE, QNAN_C, QNAN_C},           {"c SNaN", ONE, ONE, SNAN_C, SNAN_C_QUIET},
      {"a c NaN", QNAN_A, ONE, QNAN_C, QNAN_A},
  };
  for (const u32 xo : {XO_MADD, XO_MSUB, XO_NMADD, XO_NMSUB})
  {
    for (const u32 opcode : {OP_DOUBLE, OP_SINGLE})
    {
      for (const Inputs& in : inputs)
      {
        Case c;
        c.name = fmt::format("xo {} op {} {}", xo, opcode, in.name);
        c.code = {A_FORM(opcode, xo, 1, 2, 3, 4)};
        c.fprs = {{2, in.a, in.a}, {3, in.b, in.b}, {4, in.c, in.c}};
        c.expect = ExpectF1Ps0(in.result);
        Check(c);
      }
    }
  }
  // fmul: a then c; fadd, fsub, fdiv: a then b.
  for (const u32 opcode : {OP_DOUBLE, OP_SINGLE})
  {
    Case mul;
    mul.name = fmt::format("fmul op {} c NaN", opcode);
    mul.code = {A_FORM(opcode, XO_MUL, 1, 2, 0, 4)};
    mul.fprs = {{2, ONE, ONE}, {4, QNAN_C, QNAN_C}};
    mul.expect = ExpectF1Ps0(QNAN_C);
    Check(mul);
    mul.name = fmt::format("fmul op {} a c NaN", opcode);
    mul.fprs = {{2, QNAN_B, QNAN_B}, {4, QNAN_C, QNAN_C}};
    mul.expect = ExpectF1Ps0(QNAN_B);
    Check(mul);
    for (const u32 xo : {XO_ADD, XO_SUB, XO_DIV})
    {
      Case op;
      op.name = fmt::format("xo {} op {} b NaN", xo, opcode);
      op.code = {A_FORM(opcode, xo, 1, 2, 3, 0)};
      op.fprs = {{2, ONE, ONE}, {3, QNAN_B, QNAN_B}};
      op.expect = ExpectF1Ps0(QNAN_B);
      Check(op);
      op.name = fmt::format("xo {} op {} a b NaN", xo, opcode);
      op.fprs = {{2, QNAN_A, QNAN_A}, {3, QNAN_B, QNAN_B}};
      op.expect = ExpectF1Ps0(QNAN_A);
      Check(op);
    }
  }
}

// Paired ops, lane by lane: a NaN made in ps0 alone, in ps1 alone, in both, and NaN inputs; the
// lanes that stay numbers are still computed (and negated for ps_nmadd/ps_nmsub).
TEST_F(JitFloatSemantics, PairedNaNs)
{
  struct Op
  {
    const char* name;
    u32 xo;
    bool uses_b, uses_c, negates;
  };
  const Op ops[] = {
      {"ps_add", XO_ADD, true, false, false},      {"ps_sub", XO_SUB, true, false, false},
      {"ps_mul", XO_MUL, false, true, false},      {"ps_div", XO_DIV, true, false, false},
      {"ps_madd", XO_MADD, true, true, false},     {"ps_msub", XO_MSUB, true, true, false},
      {"ps_nmadd", XO_NMADD, true, true, true},    {"ps_nmsub", XO_NMSUB, true, true, true},
      {"ps_muls0", XO_MULS0, false, true, false},  {"ps_muls1", XO_MULS1, false, true, false},
      {"ps_madds0", XO_MADDS0, true, true, false}, {"ps_madds1", XO_MADDS1, true, true, false},
  };
  for (const Op& op : ops)
  {
    // Inputs that make a NaN: add (inf, -inf), sub (inf, inf), div (inf, inf), anything with c:
    // a = inf, c = 0, b = 1. muls0/madds0 take c.ps0 for both lanes, muls1/madds1 c.ps1.
    const auto lane_inputs = [&](bool nan) -> std::array<u64, 3> {
      if (!nan)
        return {TWO, ONE, ONE};
      switch (op.xo)
      {
      case XO_ADD:
        return {INF, NINF, ONE};
      case XO_SUB:
      case XO_DIV:
        return {INF, INF, ONE};
      default:
        return {INF, ONE, ZERO};
      }
    };
    for (const auto& [nan0, nan1] :
         {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}})
    {
      std::array<u64, 3> l0 = lane_inputs(nan0);
      std::array<u64, 3> l1 = lane_inputs(nan1);
      // A shared c lane can only make one of the two lanes NaN through a: zero c everywhere then.
      if (op.xo == XO_MULS0 || op.xo == XO_MADDS0 || op.xo == XO_MULS1 || op.xo == XO_MADDS1)
      {
        l0[2] = ZERO;
        l1[2] = ZERO;
        l0[0] = nan0 ? INF : TWO;
        l1[0] = nan1 ? INF : TWO;
      }
      for (const u32 d : {1u, 2u, 4u})
      {
        Case c;
        c.name =
            fmt::format("{} NaN in{}{} d=f{}", op.name, nan0 ? " ps0" : "", nan1 ? " ps1" : "", d);
        c.code = {A_FORM(OP_PAIRED, op.xo, d, 2, op.uses_b ? 3 : 0, op.uses_c ? 4 : 0)};
        c.fprs = {{2, l0[0], l1[0]}, {3, l0[1], l1[1]}, {4, l0[2], l1[2]}};
        c.expect = [d, nan0, nan1](const Result& r, const std::string& where) {
          if (nan0)
            EXPECT_EQ(PPC_NAN, r.ps0[d]) << where << fmt::format(": ps0 {:016x}", r.ps0[d]);
          else
            EXPECT_FALSE(IsNaN(r.ps0[d])) << where << fmt::format(": ps0 {:016x}", r.ps0[d]);
          if (nan1)
            EXPECT_EQ(PPC_NAN, r.ps1[d]) << where << fmt::format(": ps1 {:016x}", r.ps1[d]);
          else
            EXPECT_FALSE(IsNaN(r.ps1[d])) << where << fmt::format(": ps1 {:016x}", r.ps1[d]);
        };
        Check(c);
      }
    }
    // NaN inputs: a's lane 0, b's lane 1 (c's where there is no b).
    Case in;
    in.name = fmt::format("{} NaN inputs", op.name);
    in.code = {A_FORM(OP_PAIRED, op.xo, 1, 2, op.uses_b ? 3 : 0, op.uses_c ? 4 : 0)};
    in.fprs = {{2, QNAN_A, ONE}, {3, QNAN_B, QNAN_B}, {4, QNAN_C, op.uses_b ? ONE : QNAN_C}};
    Check(in);
  }
}

// ps_sum0/1: a.ps0 + b.ps1 by PowerPC's rules, the other lane from c, whatever d aliases.
TEST_F(JitFloatSemantics, PairedSum)
{
  struct Inputs
  {
    const char* name;
    u64 a0, b1, sum;
  };
  const Inputs inputs[] = {
      {"inf + -inf", INF, NINF, PPC_NAN}, {"NaN + NaN", QNAN_A, QNAN_B, QNAN_A},
      {"1 + NaN", ONE, QNAN_B, QNAN_B},   {"NaN + 1", QNAN_C, ONE, QNAN_C},
      {"1 + 1", ONE, ONE, TWO},
  };
  for (const u32 xo : {XO_SUM0, XO_SUM1})
  {
    for (const Inputs& in : inputs)
    {
      for (const u32 d : {1u, 2u, 3u, 4u})
      {
        Case c;
        c.name = fmt::format("ps_sum{} {} d=f{}", xo - XO_SUM0, in.name, d);
        c.code = {A_FORM(OP_PAIRED, xo, d, 2, 3, 4)};
        c.fprs = {{2, in.a0, MINUS_ONE}, {3, MINUS_ONE, in.b1}, {4, TWO, ONE}};
        const u64 sum = in.sum;
        const bool upper = xo == XO_SUM1;
        c.expect = [d, sum, upper](const Result& r, const std::string& where) {
          EXPECT_EQ(sum, upper ? r.ps1[d] : r.ps0[d])
              << where << fmt::format(": sum {:016x}", upper ? r.ps1[d] : r.ps0[d]);
          EXPECT_EQ(upper ? TWO : ONE, upper ? r.ps0[d] : r.ps1[d]) << where << ": c lane";
        };
        Check(c);
      }
    }
  }
}

// frsqrte of a negative number, fctiw(z) of a NaN.
TEST_F(JitFloatSemantics, EstimatesAndConversions)
{
  // Negative numbers below and above -2 (JitArm64's routine took those apart), -inf: the default
  // NaN, and FPSCR.VXSQRT set as Jit64 and the interpreter set it.
  for (const u64 b : {MINUS_ONE, std::bit_cast<u64>(-5.0), std::bit_cast<u64>(-1e300), NINF,
                      std::bit_cast<u64>(-1e-300), u64{0x8000'0000'0000'0001}})
  {
    Case c;
    c.name = fmt::format("frsqrte {:016x}", b);
    c.code = {A_FORM(OP_DOUBLE, XO_RSQRTE, 1, 0, 3, 0)};
    c.fprs = {{3, b, b}};
    c.expect = [](const Result& r, const std::string& where) {
      EXPECT_EQ(PPC_NAN, r.ps0[1]) << where << fmt::format(": f1 ps0 {:016x}", r.ps0[1]);
      EXPECT_NE(0u, r.fpscr & FPSCR_VXSQRT) << where << fmt::format(": FPSCR {:08x}", r.fpscr);
    };
    c.interpreter_flushes_here = (b & 0x7FF0'0000'0000'0000) == 0;
    Check(c);
    c.name = fmt::format("ps_rsqrte {:016x}", b);
    c.code = {A_FORM(OP_PAIRED, XO_RSQRTE, 1, 0, 3, 0)};
    c.fprs = {{3, b, std::bit_cast<u64>(4.0)}};
    c.expect = ExpectF1Ps0(PPC_NAN);
    c.interpreter_flushes_here = (b & 0x7FF0'0000'0000'0000) == 0;
    Check(c);
  }
  // frsqrte of a double denormal: an M1 to M3 reads it as zero in FRSQRTE.
  for (const u64 b : {u64{1}, u64{0x0000'0000'0100'0000}, u64{0x000F'FFFF'FFFF'FFFF}})
  {
    Case c;
    c.name = fmt::format("frsqrte denormal {:016x}", b);
    c.code = {A_FORM(OP_DOUBLE, XO_RSQRTE, 1, 0, 3, 0)};
    c.fprs = {{3, b, b}};
    c.expect = ExpectF1Ps0(
        std::bit_cast<u64>(Common::ApproximateReciprocalSquareRoot(std::bit_cast<double>(b))));
    c.interpreter_flushes_here = true;
    Check(c);
  }
  for (const u32 xo : {14u, 15u})
  {
    for (const u64 b : {QNAN_A, QNAN_B, SNAN_C, INF, NINF})
    {
      Case c;
      c.name = fmt::format("fctiw{} {:016x}", xo == 15 ? "z" : "", b);
      c.code = {X_FORM(OP_DOUBLE, xo, 1, 0, 3)};
      c.fprs = {{3, b, b}};
      const u32 low = b == INF ? 0x7FFF'FFFFu : 0x8000'0000u;
      c.expect = [low](const Result& r, const std::string& where) {
        EXPECT_EQ(low, static_cast<u32>(r.ps0[1])) << where << fmt::format(": {:016x}", r.ps0[1]);
      };
      Check(c);
    }
  }
}

// Loaded denormal singles count as themselves (PowerPC never flushes inputs; Jit64 computes them
// in double precision, where they are normal), and results below the smallest normal flush to zero
// in the non-IEEE mode.
TEST_F(JitFloatSemantics, DenormalSingles)
{
  const std::vector<std::pair<u32, u32>> data = {
      {0, S_DENORMAL}, {1, S_NEG_DENORMAL}, {2, S_2_100},    {3, S_2_100}, {4, 0},
      {5, 0},          {6, S_ONE},          {7, S_DENORMAL}, {8, S_SNAN},  {9, S_ONE}};
  const auto make = [&](std::string name, std::vector<u32> code) {
    Case c;
    c.name = std::move(name);
    c.code = std::move(code);
    c.data = data;
    return c;
  };

  // denormal * 2^100 = 2^-27, scalar and paired (a flushed input would give 0)
  Case muls = make("fmuls lfs denormal * 2^100",
                   {LFS(2, 0), LFS(4, 8), A_FORM(OP_SINGLE, XO_MUL, 1, 2, 0, 4)});
  muls.expect = ExpectF1(D_2_M27, D_2_M27);
  Check(muls);
  Case madds = make("fmadds lfs denormal * 2^100 + 0",
                    {LFS(2, 0), LFS(3, 16), LFS(4, 8), A_FORM(OP_SINGLE, XO_MADD, 1, 2, 3, 4)});
  madds.expect = ExpectF1(D_2_M27, D_2_M27);
  Check(madds);
  Case nmsubs = make("fnmsubs lfs denormal * 2^100 - 0",
                     {LFS(2, 0), LFS(3, 16), LFS(4, 8), A_FORM(OP_SINGLE, XO_NMSUB, 1, 2, 3, 4)});
  nmsubs.expect = ExpectF1(D_M2_M27, D_M2_M27);
  Check(nmsubs);
  Case ps_mul = make("ps_mul psq_l denormals * 2^100",
                     {PSQ_L(2, 0), PSQ_L(4, 8), A_FORM(OP_PAIRED, XO_MUL, 1, 2, 0, 4)});
  ps_mul.expect = ExpectF1(D_2_M27, D_M2_M27);
  Check(ps_mul);
  Case ps_madd =
      make("ps_madd psq_l denormals * 2^100 + 0",
           {PSQ_L(2, 0), PSQ_L(3, 16), PSQ_L(4, 8), A_FORM(OP_PAIRED, XO_MADD, 1, 2, 3, 4)});
  ps_madd.expect = ExpectF1(D_2_M27, D_M2_M27);
  Check(ps_madd);
  Case ps_muls1 = make("ps_muls1 2^100 * psq_l denormal",
                       {PSQ_L(2, 8), PSQ_L(4, 0), A_FORM(OP_PAIRED, XO_MULS1, 1, 2, 0, 4)});
  ps_muls1.expect = ExpectF1(D_M2_M27, D_M2_M27);
  Check(ps_muls1);

  // ps_sum0: d.ps1 is c.ps1 rounded to single, which flushes a denormal in the non-IEEE mode.
  Case sum = make("ps_sum0 c lane denormal",
                  {PSQ_L(2, 8), PSQ_L(3, 24), PSQ_L(4, 0), A_FORM(OP_PAIRED, XO_SUM0, 1, 2, 3, 4)});
  sum.expect = ExpectF1(0x4630'0000'0000'0000, 0x8000'0000'0000'0000);  // 2^100, -0
  Check(sum);

  // Compares and selects: -2^-127 is less than zero.
  Case fsel = make("fsel lfs -denormal",
                   {LFS(2, 4), LFS(3, 24), LFS(4, 8), A_FORM(OP_DOUBLE, XO_SEL, 1, 2, 3, 4)});
  fsel.expect = ExpectF1Ps0(ONE);
  Check(fsel);
  Case ps_sel = make("ps_sel psq_l {-denormal, denormal}", {PSQ_L(2, 4), PSQ_L(3, 24), PSQ_L(4, 8),
                                                            A_FORM(OP_PAIRED, XO_SEL, 1, 2, 3, 4)});
  ps_sel.expect = ExpectF1(ONE, 0x4630'0000'0000'0000);  // b.ps0 = 1, c.ps1 = 2^100
  Check(ps_sel);
  Case fcmpu =
      make("fcmpu lfs denormal, 0", {LFS(2, 0), LFS(3, 16), X_FORM(OP_DOUBLE, 0, 0, 2, 3)});
  fcmpu.expect = [](const Result& r, const std::string& where) {
    EXPECT_EQ(0x4000'0000u, r.cr & 0xF000'0000u) << where << ": cr0 should be GT";
  };
  Check(fcmpu);
  Case ps_cmpu1 = make("ps_cmpu1 psq_l denormal, 0",
                       {PSQ_L(2, 24), PSQ_L(3, 16), X_FORM(OP_PAIRED, 64, 0, 2, 3)});
  ps_cmpu1.expect = fcmpu.expect;
  Check(ps_cmpu1);

  // fctiw of 2^-127 rounding toward +inf is 1.
  Case fctiw = make("fctiw lfs denormal toward +inf", {LFS(3, 0), X_FORM(OP_DOUBLE, 14, 1, 0, 3)});
  fctiw.rounding_mode = 2;
  fctiw.expect = [](const Result& r, const std::string& where) {
    EXPECT_EQ(1u, static_cast<u32>(r.ps0[1])) << where << fmt::format(": {:016x}", r.ps0[1]);
  };
  Check(fctiw);

  // Stores: psq_st rounds to single (a denormal flushes); stfs of an lfs'd single is bit exact.
  Case psq_st = make("psq_st of psq_l denormals", {PSQ_L(2, 0), PSQ_ST(2, 40)});
  psq_st.expect = [](const Result& r, const std::string& where) {
    EXPECT_EQ(0u, r.data[10]) << where;
    EXPECT_EQ(0x8000'0000u, r.data[11]) << where;
  };
  Check(psq_st);
  Case stfs = make("stfs of lfs denormal", {LFS(2, 0), STFS(2, 48)});
  stfs.expect = [](const Result& r, const std::string& where) {
    EXPECT_EQ(S_DENORMAL, r.data[12]) << where;
  };
  Check(stfs);
  // An SNaN that psq_l loads goes quiet, as Jit64 loads it (CVTPS2PD), whatever stores it next:
  // psq_st, stfs, stfd, or stfs after fmr. The interpreter keeps it signalling.
  {
    Case snan = make("psq_l SNaN, then psq_st, stfs, stfd, fmr + stfs",
                     {PSQ_L(2, 32), PSQ_ST(2, 40), STFS(2, 48), STFD(2, 56), FMR(6, 2),
                      STFS(6, 64)});
    snan.data_differs_from_interpreter = {10, 12, 14, 15, 16};
    snan.fprs_differ_from_interpreter = {2, 6};
    snan.expect = [](const Result& r, const std::string& where) {
      EXPECT_EQ(0x7FC0'0001u, r.data[10]) << where << fmt::format(": psq_st {:08x}", r.data[10]);
      EXPECT_EQ(S_ONE, r.data[11]) << where;
      EXPECT_EQ(0x7FC0'0001u, r.data[12]) << where << fmt::format(": stfs {:08x}", r.data[12]);
      EXPECT_EQ(0x7FF8'0000u, r.data[14]) << where << fmt::format(": stfd {:08x}", r.data[14]);
      EXPECT_EQ(0x2000'0000u, r.data[15]) << where << fmt::format(": stfd {:08x}", r.data[15]);
      EXPECT_EQ(0x7FC0'0001u, r.data[16]) << where << fmt::format(": fmr {:08x}", r.data[16]);
      EXPECT_EQ(0x7FF8'0000'2000'0000u, r.ps0[2]) << where << fmt::format(": {:016x}", r.ps0[2]);
    };
    Check(snan);
  }

  // A NaN stored as an integer: Jit64's answer (the interpreter casts a NaN to an integer, which
  // has none). A pair saturates as 65535 does, a single becomes the type's minimum.
  {
    struct Quantized
    {
      const char* name;
      u32 type;
      u32 pair_word;    // word 10 after storing {NaN, 2.0} at byte 40
      u32 single_word;  // word 12 after storing NaN at byte 48
    };
    for (const Quantized& q : {Quantized{"u8", 4, 0xFF02'000A, 0x00AD'000C},
                               Quantized{"u16", 5, 0xFFFF'0002, 0x0000'000C},
                               Quantized{"s8", 6, 0x7F02'000A, 0x80AD'000C},
                               Quantized{"s16", 7, 0x7FFF'0002, 0x8000'000C}})
    {
      Case quantized = make(fmt::format("psq_st {} of NaN", q.name),
                            {PSQ_L(2, 64), PSQ_ST(2, 40), PSQ_ST(2, 48, 1)});
      quantized.data.push_back({16, 0x7FC0'0000});
      quantized.data.push_back({17, 0x4000'0000});
      quantized.gqr0 = q.type;
      quantized.data_differs_from_interpreter = {10, 12};
      const u32 pair_word = q.pair_word, single_word = q.single_word;
      quantized.expect = [pair_word, single_word](const Result& r, const std::string& where) {
        EXPECT_EQ(pair_word, r.data[10]) << where << fmt::format(": pair {:08x}", r.data[10]);
        EXPECT_EQ(single_word, r.data[12]) << where << fmt::format(": single {:08x}", r.data[12]);
      };
      Check(quantized);
    }
  }

  // Outputs below the smallest normal flush: 2^-70 * 2^-70 (single), 2^-600 * 2^-600 (double).
  Case tiny_single;
  tiny_single.name = "fmuls tiny";
  tiny_single.code = {A_FORM(OP_SINGLE, XO_MUL, 1, 2, 0, 4)};
  tiny_single.fprs = {{2, std::bit_cast<u64>(0x1p-70), 0}, {4, std::bit_cast<u64>(0x1p-70), 0}};
  tiny_single.expect = ExpectF1(0, 0);
  Check(tiny_single);
  Case tiny_double;
  tiny_double.name = "fmul tiny";
  tiny_double.code = {A_FORM(OP_DOUBLE, XO_MUL, 1, 2, 0, 4)};
  tiny_double.fprs = {{2, std::bit_cast<u64>(0x1p-600), 0}, {4, std::bit_cast<u64>(0x1p-600), 0}};
  tiny_double.expect = ExpectF1Ps0(0);
  Check(tiny_double);
  // A double denormal input: the JIT and the interpreter agree on one host, but this is where
  // hosts still differ (the residual risk): an ARM CPU without FEAT_AFP flushes it (0), x86-64 and
  // an AFP CPU don't (2^-74).
  Case double_denormal;
  double_denormal.name = "fmul double denormal * 2^1000";
  double_denormal.code = {A_FORM(OP_DOUBLE, XO_MUL, 1, 2, 0, 4)};
  double_denormal.fprs = {{2, 1, 1}, {4, std::bit_cast<u64>(0x1p1000), 0}};
  double_denormal.expect = [](const Result& r, const std::string& where) {
    fmt::print("{}: {:016x}\n", where, r.ps0[1]);
  };
  Check(double_denormal);
}

// fres, ps_res, frsqrte and ps_rsqrte of +0 and -0: the infinity of the input's sign, and
// FPSCR.ZX with FX. -0.0 counts as zero here; a missed bit would reach RAM via an OS context save.
TEST_F(JitFloatSemantics, EstimatesOfZero)
{
  for (const u64 zero : {ZERO, NEG_ZERO})
  {
    const u64 inf = zero == ZERO ? INF : NINF;
    for (const auto& [name, opcode, xo] :
         {std::tuple{"fres", OP_SINGLE, XO_RES}, std::tuple{"ps_res", OP_PAIRED, XO_RES},
          std::tuple{"frsqrte", OP_DOUBLE, XO_RSQRTE},
          std::tuple{"ps_rsqrte", OP_PAIRED, XO_RSQRTE}})
    {
      Case c;
      c.name = fmt::format("{} {:016x}", name, zero);
      c.code = {A_FORM(opcode, xo, 1, 0, 3, 0)};
      c.fprs = {{3, zero, zero}};
      c.expect = [inf](const Result& r, const std::string& where) {
        EXPECT_EQ(inf, r.ps0[1]) << where << fmt::format(": f1 ps0 {:016x}", r.ps0[1]);
        EXPECT_EQ(FPSCR_ZX | FPSCR_FX, r.fpscr & (FPSCR_ZX | FPSCR_FX))
            << where << fmt::format(": FPSCR {:08x}", r.fpscr);
      };
      Check(c);
    }
  }
}

// fmadds and ps_madd of loaded singles whose exact product lies on a tie between two singles, which
// b, far smaller, decides (Mario Strikers Charged): 50 * c + b is 0xBF55BF17. An AFP CPU runs a
// single-precision FMADD on these; an M1 to M3 (they could be denormals there) and Jit64 the
// double-precision error-free transformation. Both must round once.
TEST_F(JitFloatSemantics, FmaddsTie)
{
  const std::vector<std::pair<u32, u32>> data = {{0, 0x4248'0000}, {1, 0x4248'0000},
                                                 {2, 0x1B1C'72A0}, {3, 0x1B1C'72A0},
                                                 {4, 0xBC88'CC38}, {5, 0xBC88'CC38}};
  Case fmadds;
  fmadds.name = "fmadds tie (lfs)";
  fmadds.code = {LFS(2, 0), LFS(3, 8), LFS(4, 16), A_FORM(OP_SINGLE, XO_MADD, 1, 2, 3, 4),
                 STFS(1, 40)};
  fmadds.data = data;
  fmadds.expect = [](const Result& r, const std::string& where) {
    EXPECT_EQ(0xBF55'BF17u, r.data[10]) << where << fmt::format(": {:08x}", r.data[10]);
  };
  Check(fmadds);
  Case ps_madd;
  ps_madd.name = "ps_madd tie (psq_l)";
  ps_madd.code = {PSQ_L(2, 0), PSQ_L(3, 8), PSQ_L(4, 16), A_FORM(OP_PAIRED, XO_MADD, 1, 2, 3, 4),
                  PSQ_ST(1, 40)};
  ps_madd.data = data;
  ps_madd.expect = [](const Result& r, const std::string& where) {
    EXPECT_EQ(0xBF55'BF17u, r.data[10]) << where << fmt::format(": ps0 {:08x}", r.data[10]);
    EXPECT_EQ(0xBF55'BF17u, r.data[11]) << where << fmt::format(": ps1 {:08x}", r.data[11]);
  };
  Check(ps_madd);
}

// The interpreter, which the JITs fall back to (Rc = 1 forms, among others), runs under the
// guest's host float mode too. On an ARM CPU with FEAT_AFP that sets FPCR.AH, where the host's
// FABS and FNEG leave a NaN's sign alone and Clang's std::copysign of a double (a mask built with
// FNEG) loses the sign. The interpreter's fabs, fres and fctiw must not depend on it.
TEST_F(JitFloatSemantics, InterpreterSignsUnderHostMode)
{
  // fabs, fnabs, fneg of a negative NaN, an SNaN and -0.
  for (const u64 b : {QNAN_B, SNAN_C, NEG_ZERO, MINUS_ONE})
  {
    for (const auto& [name, xo, result] :
         {std::tuple{"fabs", 264u, b & ~NEG_ZERO}, std::tuple{"fnabs", 136u, b | NEG_ZERO},
          std::tuple{"fneg", 40u, b ^ NEG_ZERO}})
    {
      Case c;
      c.name = fmt::format("{} {:016x}", name, b);
      c.code = {X_FORM(OP_DOUBLE, xo, 1, 0, 3)};
      c.fprs = {{3, b, b}};
      c.expect = ExpectF1Ps0(result);
      Check(c);
    }
  }
  // fres of a negative number too small, too large, or infinite for the table.
  for (const auto& [b, result] :
       {std::pair{std::bit_cast<u64>(-1e-300), std::bit_cast<u64>(-double(3.4028234663852886e38))},
        std::pair{std::bit_cast<u64>(-1e300), NEG_ZERO}, std::pair{NINF, NEG_ZERO}})
  {
    Case c;
    c.name = fmt::format("fres {:016x}", b);
    c.code = {A_FORM(OP_SINGLE, XO_RES, 1, 0, 3, 0)};
    c.fprs = {{3, b, b}};
    c.expect = ExpectF1(result, result);
    Check(c);
  }
  // fctiw of negative numbers in every rounding mode (the interpreter's round-to-nearest adds
  // 2^52 with the input's sign). A result of -0 is the one place the JITs and the interpreter
  // still differ, alike on every host: PowerPC (and the interpreter) set the upper word to
  // 0xFFF80001 there, both JITs to 0xFFF80000.
  struct Conversion
  {
    double b;
    std::array<s32, 4> by_mode;  // nearest, toward zero, toward +inf, toward -inf
  };
  for (const Conversion& conversion :
       {Conversion{-1.5, {-2, -1, -1, -2}}, Conversion{-3.5, {-4, -3, -3, -4}},
        Conversion{-2.5, {-2, -2, -2, -3}}, Conversion{-0.75, {-1, 0, 0, -1}},
        Conversion{-0.4, {0, 0, 0, -1}}, Conversion{2.5, {2, 2, 3, 2}}})
  {
    for (u32 rn = 0; rn < 4; ++rn)
    {
      const s32 value = conversion.by_mode[rn];
      Case c;
      c.name = fmt::format("fctiw {} RN {}", conversion.b, rn);
      c.code = {X_FORM(OP_DOUBLE, 14, 1, 0, 3)};
      c.fprs = {{3, std::bit_cast<u64>(conversion.b), std::bit_cast<u64>(conversion.b)}};
      c.rounding_mode = rn;
      if (value == 0 && conversion.b < 0)
        c.fprs_differ_from_interpreter = {1};
      const u64 result = 0xFFF8'0000'0000'0000 | static_cast<u32>(value);
      c.expect = ExpectF1Ps0(result);
      Check(c);
    }
  }
}

// psq_lx and psq_stx with rA = 0, which Project+'s codes use: both JITs leave them to the
// interpreter in a session, so every host stores the same truncated single (the interpreter's
// psq_st truncates; the JITs round). Also covers JitArm64's register cache across the fallback.
TEST_F(JitFloatSemantics, PairedRa0FallsBack)
{
  Case store;
  store.name = "psq_stx rA = 0 of 1/3";
  store.code = {PSQ_STX_RA0(2), PSQ_LX_RA0(3)};
  store.fprs = {{2, 0x3FD5'5555'5555'5555, ONE}};
  store.expect = [](const Result& r, const std::string& where) {
    EXPECT_EQ(0x3EAA'AAAAu, r.data[0]) << where << fmt::format(": {:08x}", r.data[0]);
    EXPECT_EQ(S_ONE, r.data[1]) << where;
  };
  Check(store);
}
