// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later
// Local test fixture using the vendored TURN server. No installed server or external service.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "server.h"

static juice_server_t* fixture;

int OrcaTestTurnStart(const char* address)
{
  // Public throwaway fixture identity, confined to this local server.
  juice_server_credentials_t identity = {"orca-fixture", "orca-fixture-only", 32};
  juice_server_config_t config;
  memset(&config, 0, sizeof(config));
  config.credentials = &identity;
  config.credentials_count = 1;
  config.max_allocations = 32;
  config.max_peers = 16;
  config.bind_address = address;
  config.external_address = address;
  config.realm = "orca-fixture";
  fixture = server_create(&config);
  return fixture ? server_get_port(fixture) : 0;
}

void OrcaTestTurnStop(void)
{
  if (fixture)
    server_destroy(fixture);
  fixture = NULL;
}

#ifdef ORCA_TURN_FIXTURE_MAIN
static volatile sig_atomic_t stopped;
static void stop_fixture(int signal_number)
{
  (void)signal_number;
  stopped = 1;
}
int main(int argc, char** argv)
{
  signal(SIGINT, stop_fixture);
  signal(SIGTERM, stop_fixture);
  const int port = OrcaTestTurnStart(argc > 1 ? argv[1] : "127.0.0.1");
  if (!port)
    return 1;
  printf("%d\n", port);
  fflush(stdout);
  while (!stopped)
  {
#ifdef _WIN32
    Sleep(100);
#else
    const struct timespec pause = {0, 100000000};
    nanosleep(&pause, NULL);
#endif
  }
  OrcaTestTurnStop();
  return 0;
}
#endif
