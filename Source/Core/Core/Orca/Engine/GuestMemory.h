// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// The emulated memory every game module reads and writes (engine side of the Orca split, S2).
// Until S2 gives the module API its own namespace, it stays in Orca::UX, where the modules are.
namespace Orca::UX
{
// Emulated memory access (tests substitute a map).
class GuestMemory
{
public:
  virtual ~GuestMemory() = default;
  virtual bool Valid(u32 address) const = 0;  // a mapped, readable RAM address
  virtual u32 Read32(u32 address) const = 0;
  virtual u16 Read16(u32 address) const = 0;
  virtual u8 Read8(u32 address) const = 0;
  virtual void Write8(u32 address, u8 value) = 0;
  virtual void Write16(u32 address, u16 value) = 0;
  virtual void Write32(u32 address, u32 value) = 0;
};

// The CPU thread's emulated memory, through the guard.
class GuardMemory final : public GuestMemory
{
public:
  explicit GuardMemory(const Core::CPUThreadGuard& guard) : m_guard(guard) {}
  bool Valid(u32 address) const override;
  u32 Read32(u32 address) const override;
  u16 Read16(u32 address) const override;
  u8 Read8(u32 address) const override;
  void Write8(u32 address, u8 value) override;
  void Write16(u32 address, u16 value) override;
  void Write32(u32 address, u32 value) override;

private:
  const Core::CPUThreadGuard& m_guard;
};
}  // namespace Orca::UX
