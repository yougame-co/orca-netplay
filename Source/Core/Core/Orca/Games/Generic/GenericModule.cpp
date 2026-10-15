// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Generic/GenericModule.h"

#include "Core/Orca/Profile.h"
#include "Core/Rollback/OnlineMatch.h"

namespace Orca::UX
{
bool GenericModuleEligible(const Profile* profile, Mod::BundleKind kind, bool jailed,
                           bool test_build)
{
  if (!profile || !profile->mod || profile->mod->manifest.builtin != Mod::Builtin::None)
    return false;
  return (kind == Mod::BundleKind::Flat && jailed) ||
         (kind == Mod::BundleKind::Tree && test_build);
}

GenericModule& Generic()
{
  static GenericModule s_module;
  return s_module;
}

Engine::Manifest GenericModule::Describe()
{
  return {"generic", Engine::Ruleset::None, false, false, GENERIC_COMPAT};
}

bool GenericModule::SetWanted(Engine::Mode mode, u8 flags, std::string_view code)
{
  if (m_mode == mode && m_flags == flags && m_code == code)
    return false;
  m_mode = mode;
  m_flags = flags;
  m_code = code;
  ++m_generation;
  return true;
}

bool GenericModule::FreshSupported()
{
  const auto* mod = Mod::ActiveMod();
  return mod && Rollback::OnlineMatch::SelectOriginRule(&mod->manifest, false,
                                                       Mod::GuardsFailed()) ==
                    Rollback::OnlineMatch::OriginRule::Declared;
}
}  // namespace Orca::UX
