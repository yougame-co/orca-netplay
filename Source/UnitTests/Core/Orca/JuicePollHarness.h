// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OrcaJuicePollResult
{
  int receive_calls;
  int delivered;
  int failures;
  int updates;
  int ready;
  int error_logs;
  int drop_logs;
} OrcaJuicePollResult;

OrcaJuicePollResult OrcaJuicePollRun(int dropped, int fatal_error, int passes);

#ifdef __cplusplus
}
#endif
