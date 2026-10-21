// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca::PrioritizeThread (Profile.h): raises the priority of the threads on the input path. Kept
// out of Profile.cpp so the platform headers stay out of there.

#include "Core/Orca/Profile.h"

#if defined(_WIN32)
#include <windows.h>
#include <processthreadsapi.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

#include "Common/Logging/Log.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Rollback/Diag.h"
#include "Core/Rollback/Harness.h"

namespace Orca
{
namespace
{
[[maybe_unused]] const char* Name(LatencyThread thread)
{
  switch (thread)
  {
  case LatencyThread::Cpu:
    return "CPU";
  case LatencyThread::Room:
    return "room";
  case LatencyThread::Controllers:
    return "controller stream";
  }
  return "?";
}

bool Wanted(LatencyThread thread)
{
  if (!SessionActive())
    return false;
  const std::string knob = GetEnv("ORCA_THREAD_QOS");
  if (knob == "0")
    return false;
  if (knob == "1" || thread != LatencyThread::Cpu)
    return true;
  // An unpaced harness run (no online match, no YG_THROTTLE) runs flat out, so it must not hold a
  // core at interactive priority.
  return !Rollback::Harness::Active() || Online::Enabled() || Rollback::Diag::KeepThrottle();
}
}  // namespace

void PrioritizeThread(LatencyThread thread)
{
  if (!Wanted(thread))
    return;
#if defined(_WIN32)
  // The CPU thread runs flat out, so one notch up; the network threads mostly wait and should wake
  // at once. EcoQoS is also turned off per thread: Orca's window is a child of the app's, so
  // Windows never treats Orca as the foreground process.
  const bool ok = SetThreadPriority(GetCurrentThread(), thread == LatencyThread::Cpu ?
                                                            THREAD_PRIORITY_ABOVE_NORMAL :
                                                            THREAD_PRIORITY_HIGHEST) != 0;
  THREAD_POWER_THROTTLING_STATE throttling{};
  throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  throttling.StateMask = 0;
  SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof(throttling));
  NOTICE_LOG_FMT(CORE, "Orca: {} thread at raised priority{}", Name(thread),
                 ok ? "" : " (refused)");
#elif defined(__APPLE__)
  const bool ok = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
  NOTICE_LOG_FMT(CORE, "Orca: {} thread at interactive QoS{}", Name(thread),
                 ok ? "" : " (refused)");
#endif
}
}  // namespace Orca
