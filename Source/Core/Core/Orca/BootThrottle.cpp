// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/BootThrottle.h"

#include <utility>

#include "Core/Core.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Rollback/Diag.h"
#include "Core/Rollback/Harness.h"
#include "Core/Rollback/SessionPort.h"

namespace Orca::BootThrottle
{
namespace
{
bool s_owned = false;
}

void Begin()
{
  s_owned = SessionActive() && !Core::GetIsThrottlerTempDisabled();
  if (s_owned)
    Core::SetIsThrottlerTempDisabled(true);
}

void Finish(bool pacing_owned_elsewhere)
{
  if (std::exchange(s_owned, false) && !pacing_owned_elsewhere)
    Core::SetIsThrottlerTempDisabled(false);
}
void FinishAtFirstFrame()
{
  Finish(Rollback::RingPort::Active() ||
         (Rollback::Harness::Active() && !Online::Enabled() && !Rollback::Diag::KeepThrottle()));
}
}  // namespace Orca::BootThrottle
