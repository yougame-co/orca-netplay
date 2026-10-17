// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <functional>

#include "Common/Arm64Emitter.h"
#include "Common/CommonTypes.h"

#include <gtest/gtest.h>

namespace
{
using namespace Arm64Gen;

// Runs MOVI(64, D0, start), then `op` on D0, and returns D0's 64 bits.
class TestVectorModImm : public ARM64CodeBlock
{
public:
  TestVectorModImm() { AllocCodeSpace(4096); }

  u64 Run(u64 start, const std::function<void(ARM64FloatEmitter&)>& op)
  {
    ResetCodePtr();

    const u8* fn = GetCodePtr();
    {
      const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
      ARM64FloatEmitter float_emit(this);
      float_emit.MOVI(64, ARM64Reg::D0, start);
      op(float_emit);
      float_emit.FMOV(ARM64Reg::X0, ARM64Reg::D0);
      RET();
    }

    FlushIcacheSection(const_cast<u8*>(fn), const_cast<u8*>(GetCodePtr()));

    return std::bit_cast<u64 (*)()>(fn)();
  }
};
}  // namespace

// The 16-bit lane forms of ORR/BIC (vector, immediate) must act on every halfword. Encoding them
// with a 32-bit cmode would act on every word instead.
TEST(JitArm64, VectorModImm_16BitLanes)
{
  TestVectorModImm test;

  // JitArm64::fctiwx builds the upper word of its result this way.
  EXPECT_EQ(0xFFF8'0000'0000'0000ULL,
            test.Run(0xFFFF'0000'0000'0000ULL, [](auto& e) { e.BIC(16, ARM64Reg::D0, 0x7); }));
  EXPECT_EQ(0xFFF8'FFF8'FFF8'FFF8ULL,
            test.Run(0xFFFF'FFFF'FFFF'FFFFULL, [](auto& e) { e.BIC(16, ARM64Reg::D0, 0x7); }));
  EXPECT_EQ(0xF8FF'F8FF'F8FF'F8FFULL,
            test.Run(0xFFFF'FFFF'FFFF'FFFFULL, [](auto& e) { e.BIC(16, ARM64Reg::D0, 0x7, 8); }));
  EXPECT_EQ(0x0007'0007'0007'0007ULL,
            test.Run(0, [](auto& e) { e.ORR(16, ARM64Reg::D0, 0x7); }));
  EXPECT_EQ(0x0700'0700'0700'0700ULL,
            test.Run(0, [](auto& e) { e.ORR(16, ARM64Reg::D0, 0x7, 8); }));
}

TEST(JitArm64, VectorModImm_32BitLanes)
{
  TestVectorModImm test;

  EXPECT_EQ(0xFFFF'FFF8'FFFF'FFF8ULL,
            test.Run(0xFFFF'FFFF'FFFF'FFFFULL, [](auto& e) { e.BIC(32, ARM64Reg::D0, 0x7); }));
  EXPECT_EQ(0xF8FF'FFFF'F8FF'FFFFULL,
            test.Run(0xFFFF'FFFF'FFFF'FFFFULL, [](auto& e) { e.BIC(32, ARM64Reg::D0, 0x7, 24); }));
  EXPECT_EQ(0x0000'0007'0000'0007ULL,
            test.Run(0, [](auto& e) { e.ORR(32, ARM64Reg::D0, 0x7); }));
  EXPECT_EQ(0x0007'0000'0007'0000ULL,
            test.Run(0, [](auto& e) { e.ORR(32, ARM64Reg::D0, 0x7, 16); }));
}
