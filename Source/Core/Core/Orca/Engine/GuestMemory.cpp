// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Engine/GuestMemory.h"

#include "Core/Orca/Engine/Regions.h"
#include "Core/PowerPC/MMU.h"

namespace Orca::UX
{
bool GuardMemory::Valid(u32 address) const
{
  return PowerPC::MMU::HostIsRAMAddress(m_guard, address);
}
u32 GuardMemory::Read32(u32 address) const
{
  return PowerPC::MMU::HostRead<u32>(m_guard, address);
}
u16 GuardMemory::Read16(u32 address) const
{
  return PowerPC::MMU::HostRead<u16>(m_guard, address);
}
u8 GuardMemory::Read8(u32 address) const
{
  return PowerPC::MMU::HostRead<u8>(m_guard, address);
}
void GuardMemory::Write8(u32 address, u8 value)
{
  Orca::Engine::NoteWrite(address, 1);
  PowerPC::MMU::HostWrite<u8>(m_guard, value, address);
}
void GuardMemory::Write16(u32 address, u16 value)
{
  Orca::Engine::NoteWrite(address, 2);
  PowerPC::MMU::HostWrite<u16>(m_guard, value, address);
}
void GuardMemory::Write32(u32 address, u32 value)
{
  Orca::Engine::NoteWrite(address, 4);
  PowerPC::MMU::HostWrite<u32>(m_guard, value, address);
}
}  // namespace Orca::UX
