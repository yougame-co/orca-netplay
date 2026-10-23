// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Jit64 against the interpreter for the double-precision ops that write ps0 and keep the
// destination's ps1: every FPR (both halves) must match after a short guest program, with a
// sentinel in each register's ps1. The cases vary which operands share a register with d and
// whether the operands are already in host registers (a "bound" prelude) or read from memory.

#include <array>
#include <bit>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"

#include <gtest/gtest.h>

namespace
{
constexpr u32 BRANCH_TO_SELF = 0x48000000;
// Each program gets its own address, so neither core reuses a block or a cache line.
constexpr u32 CODE_BASE = 0x00010000;
constexpr u32 CODE_STRIDE = 0x100;

constexpr u32 A_FORM(u32 xo, u32 d, u32 a, u32 b, u32 c)
{
  return (63u << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1);
}
constexpr u32 X_FORM(u32 xo, u32 d, u32 a, u32 b)
{
  return (63u << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1);
}
// ps_merge01 r,r,r leaves r as it is but makes Jit64 bind it to a host register.
constexpr u32 BIND(u32 r)
{
  return (4u << 26) | (r << 21) | (r << 16) | (r << 11) | (560u << 1);
}

u64 Ps0(int reg)
{
  return std::bit_cast<u64>(1.5 + reg * 0.25);
}
// A sentinel that no op computes: a different normal double per register.
u64 Ps1(int reg)
{
  return std::bit_cast<u64>(-1000.0 - reg) | 0x5a5;
}
constexpr u64 QUIET_NAN = 0x7ff8'0000'0000'1234;

struct Fprs
{
  std::array<u64, 32> ps0;
  std::array<u64, 32> ps1;
};

enum class Inputs
{
  Memory,  // operands are read from PowerPCState
  Bound,   // a prelude puts d and the operands in host registers first
};

class Jit64Ps1 : public testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    s_user_dir = File::CreateTempDir();
    Core::DeclareAsCPUThread();
    UICommon::SetUserDirectory(s_user_dir);
    Config::Init();
    SConfig::Init();
    // Plain loads and stores: no fastmem arena or fault handler in a unit test.
    Config::SetCurrent(Config::MAIN_FASTMEM, false);
    Config::SetCurrent(Config::MAIN_FASTMEM_ARENA, false);
    auto& system = Core::System::GetInstance();
    system.GetMemory().Init();
    system.GetPowerPC().Init(PowerPC::CPUCore::JIT64);
    system.GetCoreTiming().Init();
    s_host_avx = cpu_info.bAVX;
    s_host_fma = cpu_info.bFMA;
  }

  static void TearDownTestSuite()
  {
    auto& system = Core::System::GetInstance();
    cpu_info.bAVX = s_host_avx;
    cpu_info.bFMA = s_host_fma;
    system.GetCoreTiming().Shutdown();
    system.GetPowerPC().Shutdown();
    system.GetMemory().Shutdown();
    SConfig::Shutdown();
    Config::Shutdown();
    Core::UndeclareAsCPUThread();
    File::DeleteDirRecursively(s_user_dir);
  }

  // Applies the settings and drops every compiled block, so the next run compiles with them.
  // use_fma without host_fma is Jit64's software FMA (a call to std::fma).
  static void Configure(bool accurate_nans, bool use_fma, bool avx, bool host_fma)
  {
    Config::SetCurrent(Config::MAIN_ACCURATE_NANS, accurate_nans);
    Config::SetCurrent(Config::SESSION_USE_FMA, use_fma);
    cpu_info.bAVX = avx;
    cpu_info.bFMA = host_fma;
    auto& system = Core::System::GetInstance();
    const Core::CPUThreadGuard guard(system);
    system.GetJitInterface().ClearCache(guard);
  }

  static Fprs Run(const std::vector<u32>& code, u32 address, bool jit,
                  const std::array<u64, 32>& ps0)
  {
    auto& system = Core::System::GetInstance();
    auto& memory = system.GetMemory();
    auto& power_pc = system.GetPowerPC();
    auto& ppc = system.GetPPCState();

    for (std::size_t i = 0; i < code.size(); ++i)
      memory.Write_U32(code[i], address + static_cast<u32>(i * 4));
    memory.Write_U32(BRANCH_TO_SELF, address + static_cast<u32>(code.size() * 4));

    for (int reg = 0; reg < 32; ++reg)
    {
      ppc.ps[reg].SetPS0(ps0[reg]);
      ppc.ps[reg].SetPS1(Ps1(reg));
    }
    ppc.fpscr.Hex = 0;
    ppc.msr.Hex = 0;
    ppc.msr.FP = 1;
    HID2(ppc).PSE = 1;
    power_pc.MSRUpdated();
    ppc.pc = address;
    ppc.npc = address + 4;

    if (jit)
    {
      // The CPU isn't "running", so the dispatcher returns once the slice ends: the block runs
      // once and the branch to itself idles out the rest.
      power_pc.SetMode(PowerPC::CoreMode::JIT);
      power_pc.SingleStep();
      // Proof that Jit64 compiled the program rather than something else running it.
      auto* const jit_core = static_cast<JitBase*>(system.GetJitInterface().GetCore());
      EXPECT_NE(jit_core->GetBlockCache()->GetBlockFromStartAddress(address, ppc.feature_flags),
                nullptr)
          << fmt::format("no Jit64 block at {:#x}", address);
    }
    else
    {
      power_pc.SetMode(PowerPC::CoreMode::Interpreter);
      for (std::size_t i = 0; i < code.size(); ++i)
        power_pc.SingleStep();
    }

    Fprs out;
    for (int reg = 0; reg < 32; ++reg)
    {
      out.ps0[reg] = ppc.ps[reg].PS0AsU64();
      out.ps1[reg] = ppc.ps[reg].PS1AsU64();
    }
    return out;
  }

  // Runs `op` on both cores after an optional binding prelude and compares every FPR.
  void Check(const std::string& name, u32 op, std::vector<u32> bind, Inputs inputs,
             const std::array<u64, 32>& ps0, bool compare_ps0 = true)
  {
    std::vector<u32> code;
    if (inputs == Inputs::Bound)
    {
      for (const u32 reg : bind)
        code.push_back(BIND(reg));
    }
    code.push_back(op);

    const u32 address = CODE_BASE + m_next_program++ * CODE_STRIDE;
    const Fprs expected = Run(code, address, false, ps0);
    const Fprs actual = Run(code, address, true, ps0);

    const std::string where = fmt::format("{} ({}, accurate NaNs {}, FMA {}, host FMA {}, AVX {})",
                                          name, inputs == Inputs::Bound ? "bound" : "memory",
                                          m_accurate_nans, m_use_fma, cpu_info.bFMA, cpu_info.bAVX);
    for (int reg = 0; reg < 32; ++reg)
    {
      EXPECT_EQ(expected.ps1[reg], actual.ps1[reg])
          << where << ": f" << reg << " ps1 (interpreter vs Jit64)";
      if (compare_ps0)
      {
        EXPECT_EQ(expected.ps0[reg], actual.ps0[reg])
            << where << ": f" << reg << " ps0 (interpreter vs Jit64)";
      }
    }
  }

  void CheckAll(bool accurate_nans, bool use_fma, bool avx, bool host_fma)
  {
    m_accurate_nans = accurate_nans;
    m_use_fma = use_fma;
    Configure(accurate_nans, use_fma, avx, host_fma);

    std::array<u64, 32> ps0;
    for (int reg = 0; reg < 32; ++reg)
      ps0[reg] = Ps0(reg);

    for (const Inputs inputs : {Inputs::Memory, Inputs::Bound})
    {
      // fadd, fsub, fdiv: d = a op b. fmul: d = a * c. fmadd: d = a * c + b.
      // Registers: d = 1, a = 2, b = 3, c = 4 unless shared.
      struct Binary
      {
        const char* name;
        u32 xo;
        bool uses_c;
      };
      for (const Binary& op : {Binary{"fadd", 21, false}, Binary{"fsub", 20, false},
                               Binary{"fdiv", 18, false}, Binary{"fmul", 25, true}})
      {
        const auto encode = [&](u32 d, u32 a, u32 x) {
          return op.uses_c ? A_FORM(op.xo, d, a, 0, x) : A_FORM(op.xo, d, a, x, 0);
        };
        Check(fmt::format("{} d!=a,d!=x", op.name), encode(1, 2, 3), {1, 2, 3}, inputs, ps0);
        // a from memory, x in a register: the reversible ops' AVX form with the operands swapped.
        Check(fmt::format("{} d!=a,d!=x, x bound", op.name), encode(1, 2, 3), {1, 3}, inputs, ps0);
        Check(fmt::format("{} d==a", op.name), encode(2, 2, 3), {2, 3}, inputs, ps0);
        Check(fmt::format("{} d==x", op.name), encode(3, 2, 3), {2, 3}, inputs, ps0);
        Check(fmt::format("{} d==a==x", op.name), encode(2, 2, 2), {2}, inputs, ps0);
      }
      // The interpreter always fuses fmadd; with FMA off Jit64 rounds the product first, so ps0
      // is compared only when both fuse (ps1 always).
      Check("fmadd d!=a,b,c", A_FORM(29, 1, 2, 3, 4), {1, 2, 3, 4}, inputs, ps0, use_fma);
      Check("fmadd d==a", A_FORM(29, 2, 2, 3, 4), {2, 3, 4}, inputs, ps0, use_fma);
      Check("fmadd d==b", A_FORM(29, 3, 2, 3, 4), {2, 3, 4}, inputs, ps0, use_fma);
      Check("fmadd d==c", A_FORM(29, 4, 2, 3, 4), {2, 3, 4}, inputs, ps0, use_fma);

      // d = op(b): d = 1, b = 3, or d == b.
      struct Unary
      {
        const char* name;
        u32 instruction_d1_b3;
        u32 instruction_d3_b3;
      };
      for (const Unary& op : {Unary{"fneg", X_FORM(40, 1, 0, 3), X_FORM(40, 3, 0, 3)},
                              Unary{"fabs", X_FORM(264, 1, 0, 3), X_FORM(264, 3, 0, 3)},
                              Unary{"fnabs", X_FORM(136, 1, 0, 3), X_FORM(136, 3, 0, 3)},
                              Unary{"fctiw", X_FORM(14, 1, 0, 3), X_FORM(14, 3, 0, 3)},
                              Unary{"fctiwz", X_FORM(15, 1, 0, 3), X_FORM(15, 3, 0, 3)},
                              Unary{"frsqrte", A_FORM(26, 1, 0, 3, 0), A_FORM(26, 3, 0, 3, 0)}})
      {
        Check(fmt::format("{} d!=b", op.name), op.instruction_d1_b3, {1, 3}, inputs, ps0);
        Check(fmt::format("{} d==b", op.name), op.instruction_d3_b3, {3}, inputs, ps0);
      }

      Check("mffs", X_FORM(583, 1, 0, 0), {1}, inputs, ps0);
    }

    // With accurate NaNs, a NaN result takes HandleNaNs' path; ps1 must survive it too.
    if (accurate_nans)
    {
      std::array<u64, 32> nan_a = ps0;
      nan_a[2] = QUIET_NAN;
      for (const Inputs inputs : {Inputs::Memory, Inputs::Bound})
      {
        Check("fmul NaN a", A_FORM(25, 1, 2, 0, 4), {1, 2, 4}, inputs, nan_a);
        Check("fdiv NaN a", A_FORM(18, 1, 2, 3, 0), {1, 2, 3}, inputs, nan_a);
        Check("fmadd NaN a", A_FORM(29, 1, 2, 3, 4), {1, 2, 3, 4}, inputs, nan_a, m_use_fma);
        Check("fmul NaN a, d==c", A_FORM(25, 4, 2, 0, 4), {2, 4}, inputs, nan_a);
      }
    }
  }

  bool m_accurate_nans = false;
  bool m_use_fma = false;
  u32 m_next_program = 0;

  static inline std::string s_user_dir;
  static inline bool s_host_avx = false;
  static inline bool s_host_fma = false;
};
}  // namespace

TEST_F(Jit64Ps1, MatchesInterpreter)
{
  fmt::print("Jit64Ps1: host AVX {}, host FMA {}\n", s_host_avx, s_host_fma);
  // AVX on and off where the host has it; SSE only otherwise (Rosetta 2 has no AVX).
  std::vector<bool> avx_modes{false};
  if (s_host_avx)
    avx_modes.push_back(true);
  // FMA off (Jit64 multiplies then adds), software FMA (Jit64 calls std::fma: SESSION_USE_FMA on
  // a host without FMA3, pretended here on any host), and hardware FMA where the host has it.
  struct FmaMode
  {
    bool use_fma;
    bool host_fma;
  };
  std::vector<FmaMode> fma_modes{{false, false}, {true, false}};
  if (s_host_fma)
    fma_modes.push_back({true, true});
  for (const bool avx : avx_modes)
  {
    for (const bool accurate_nans : {false, true})
    {
      for (const FmaMode& fma : fma_modes)
      {
        // Hardware FMA is a VEX encoding; it comes only with AVX.
        if (fma.host_fma && !avx)
          continue;
        CheckAll(accurate_nans, fma.use_fma, avx, fma.host_fma);
      }
    }
  }
}
