// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/RenderState.h"

// Orca: on GPUs without D3D11.1 logic ops (older Intel integrated graphics), logic ops are
// approximated with blending. D3D11 refuses a *_COLOR factor in the alpha blend factors, so the
// approximation must only ever put the ALPHA forms there.

static BlendingState LogicOpState(u32 mode, bool alpha_update)
{
  BlendingState state;
  state.color_update = true;
  state.alpha_update = alpha_update;
  state.logic_op_enable = true;
  state.logic_mode = static_cast<LogicOp>(mode);
  return state;
}

static bool IsColorFactor(SrcBlendFactor factor)
{
  return factor == SrcBlendFactor::DstClr || factor == SrcBlendFactor::InvDstClr;
}

static bool IsColorFactor(DstBlendFactor factor)
{
  return factor == DstBlendFactor::SrcClr || factor == DstBlendFactor::InvSrcClr;
}

TEST(OrcaLogicOpApproximation, AlphaFactorsNeverUseColorFactors)
{
  for (u32 mode = 0; mode < 16; ++mode)
  {
    for (const bool alpha_update : {true, false})
    {
      BlendingState state = LogicOpState(mode, alpha_update);
      state.ApproximateLogicOpWithBlending();
      EXPECT_FALSE(state.logic_op_enable) << "mode " << mode;
      EXPECT_FALSE(IsColorFactor(state.src_factor_alpha.Value())) << "mode " << mode;
      EXPECT_FALSE(IsColorFactor(state.dst_factor_alpha.Value())) << "mode " << mode;
    }
  }
}

TEST(OrcaLogicOpApproximation, AlphaFactorsMatchTheColorFactors)
{
  // XOR: InvDstClr / InvSrcClr for color, InvDstAlpha / InvSrcAlpha for alpha.
  BlendingState xor_state = LogicOpState(static_cast<u32>(LogicOp::Xor), true);
  xor_state.ApproximateLogicOpWithBlending();
  EXPECT_TRUE(xor_state.blend_enable);
  EXPECT_EQ(xor_state.src_factor.Value(), SrcBlendFactor::InvDstClr);
  EXPECT_EQ(xor_state.dst_factor.Value(), DstBlendFactor::InvSrcClr);
  EXPECT_EQ(xor_state.src_factor_alpha.Value(), SrcBlendFactor::InvDstAlpha);
  EXPECT_EQ(xor_state.dst_factor_alpha.Value(), DstBlendFactor::InvSrcAlpha);

  // AND: DstClr / Zero becomes DstAlpha / Zero; factors without a color form are kept.
  BlendingState and_state = LogicOpState(static_cast<u32>(LogicOp::And), true);
  and_state.ApproximateLogicOpWithBlending();
  EXPECT_EQ(and_state.src_factor_alpha.Value(), SrcBlendFactor::DstAlpha);
  EXPECT_EQ(and_state.dst_factor_alpha.Value(), DstBlendFactor::Zero);

  // OR_REVERSE keeps its alpha factors as they are.
  BlendingState or_reverse = LogicOpState(static_cast<u32>(LogicOp::OrReverse), true);
  or_reverse.ApproximateLogicOpWithBlending();
  EXPECT_EQ(or_reverse.src_factor_alpha.Value(), SrcBlendFactor::One);
  EXPECT_EQ(or_reverse.dst_factor_alpha.Value(), DstBlendFactor::InvDstAlpha);
}

TEST(OrcaLogicOpApproximation, AlphaNotWrittenKeepsAlphaUntouched)
{
  BlendingState state = LogicOpState(static_cast<u32>(LogicOp::Nand), false);
  state.ApproximateLogicOpWithBlending();
  EXPECT_EQ(state.src_factor_alpha.Value(), SrcBlendFactor::Zero);
  EXPECT_EQ(state.dst_factor_alpha.Value(), DstBlendFactor::One);
}
