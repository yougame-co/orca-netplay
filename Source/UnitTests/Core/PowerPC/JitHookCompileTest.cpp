// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca: a compile from the frame boundary's hook (JitBase::CompileFromHook, the JIT warm-up) runs
// inside a JIT block, so it must never clear the code cache: the hook's caller returns into it. A
// block too big for the free code space is dropped instead, and nothing else is touched. The guest
// writes the code, and the warm-up list names it (a player's own list learns it from a match), so
// a block of tens of thousands of instructions is within a hostile game's reach.

#include <cstdlib>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FPURoundMode.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"

#include <gtest/gtest.h>

namespace
{
#if defined(_M_X86_64)
constexpr PowerPC::CPUCore HOST_JIT = PowerPC::CPUCore::JIT64;
#else
constexpr PowerPC::CPUCore HOST_JIT = PowerPC::CPUCore::JITARM64;
#endif

constexpr u32 STMW_R0_0_R5 = 0xBC050000;  // stmw r0, 0(r5): 32 stores
constexpr u32 BRANCH_TO_SELF = 0x48000000;
constexpr u32 CODE_BASE = 0x00100000;
constexpr u32 CODE_STRIDE = 0x00020000;  // room for 32,000 instructions
constexpr u32 COPIES = (0x01800000 - CODE_BASE) / CODE_STRIDE;  // all in MEM1
constexpr std::size_t BIG_BLOCK = 2 * 1024 * 1024;

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

class OrcaJitHookCompile : public testing::Test
{
protected:
  static void SetUpTestSuite()
  {
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

  static JitBase& Jit()
  {
    return *static_cast<JitBase*>(Core::System::GetInstance().GetJitInterface().GetCore());
  }

  static JitBlock* Find(u32 address)
  {
    auto& ppc = Core::System::GetInstance().GetPPCState();
    return Jit().GetBlockCache()->GetBlockFromStartAddress(address, ppc.feature_flags);
  }

  // `count` stmw instructions at `address`, then a branch to itself (the block's end).
  static void WriteBlock(u32 address, u32 count)
  {
    auto& memory = Core::System::GetInstance().GetMemory();
    for (u32 i = 0; i < count; ++i)
      memory.Write_U32(STMW_R0_0_R5, address + i * 4);
    memory.Write_U32(BRANCH_TO_SELF, address + count * 4);
  }

  static std::size_t HostBytes(const JitBlock& block)
  {
    return static_cast<std::size_t>(block.near_end - block.near_begin) +
           static_cast<std::size_t>(block.far_end - block.far_begin);
  }

  static inline bool s_had_session = false;
  static inline std::string s_user_dir;
};
}  // namespace

TEST_F(OrcaJitHookCompile, ABlockTooBigForTheFreeSpaceIsDroppedNotACacheClear)
{
  auto& system = Core::System::GetInstance();
  auto& ppc = system.GetPPCState();
  ppc.msr.Hex = 0;  // real mode: effective addresses are physical
  system.GetPowerPC().MSRUpdated();
  ppc.gpr[5] = 0x00F00000;
  const Core::CPUThreadGuard guard(system);
  system.GetJitInterface().ClearCache(guard);

  // A block of at least BIG_BLOCK host bytes: twice the room CanCompileFromHook asks for.
  u32 count = 1000;
  std::size_t bytes = 0;
  while (true)
  {
    WriteBlock(CODE_BASE, count);
    ASSERT_TRUE(Jit().CanCompileFromHook());
    ASSERT_TRUE(Jit().CompileFromHook(CODE_BASE));
    const JitBlock* block = Find(CODE_BASE);
    ASSERT_NE(block, nullptr);
    bytes = HostBytes(*block);
    system.GetJitInterface().ClearCache(guard);
    if (bytes >= BIG_BLOCK || count >= 30000)
      break;
    count = std::min<u32>(30000, count * 2);
  }
  ASSERT_GE(bytes, BIG_BLOCK) << count << " stmw make only " << bytes << " host bytes";

  // Fill the code space with copies from the hook until one doesn't fit while the JIT still says
  // a hook compile may go ahead.
  std::vector<u32> compiled;
  bool dropped = false;
  for (u32 i = 0; i < COPIES && Jit().CanCompileFromHook(); ++i)
  {
    const u32 address = CODE_BASE + i * CODE_STRIDE;
    WriteBlock(address, count);
    if (!Jit().CompileFromHook(address))
    {
      dropped = true;
      // Dropped: no block (no lookup may find unfinished code) ...
      EXPECT_EQ(Find(address), nullptr);
      // ... and no cache clear: every earlier block is still there.
      for (const u32 earlier : compiled)
        EXPECT_NE(Find(earlier), nullptr) << fmt::format("{:#x}", earlier);
      // The dispatcher's own compile of it still works: there a full cache may be cleared.
      Jit().Jit(address);
      EXPECT_NE(Find(address), nullptr);
      break;
    }
    ASSERT_NE(Find(address), nullptr);
    // A compile from the hook never clears the cache, even when one more block only fit after one.
    if (!compiled.empty())
    {
      ASSERT_NE(Find(compiled.front()), nullptr)
          << fmt::format("the hook's compile of {:#x} cleared the code cache", address);
    }
    compiled.push_back(address);
  }
  EXPECT_TRUE(dropped) << compiled.size() << " blocks of " << bytes << " bytes all fit";
  system.GetJitInterface().ClearCache(guard);
}
