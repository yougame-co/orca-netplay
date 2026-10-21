// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/BootLogs.h"

#include "Common/Config/Config.h"
#include "Common/Logging/LogManager.h"

namespace Orca::BootLogs
{
std::string EndAtFirstFrame()
{
  auto* const logs = Common::Log::LogManager::GetInstance();
  if (!logs)
    return {};
  const auto base = Config::GetLayer(Config::LayerType::Base);
  std::string ended;
  for (const auto type : TYPES)
  {
    // Logger.ini keys are the categories' short names (MEMMAP is "MI").
    const char* const name = logs->GetShortName(type);
    const Config::Info<bool> info{{Config::System::Logger, "Logs", name}, false};
    if (!logs->IsEnabled(type) || (base && base->Get(info)))
      continue;
    logs->SetEnable(type, false);
    if (!ended.empty())
      ended += ", ";
    ended += name;
  }
  return ended;
}
}  // namespace Orca::BootLogs
