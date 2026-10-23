// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "JuicePollHarness.h"

#include "agent.h"
#include "log.h"
#include "udp.h"

static int test_recvfrom(socket_t sock, char* buffer, size_t size, addr_record_t* src);
static int test_agent_recv(juice_agent_t* agent, char* buffer, size_t size,
                          const addr_record_t* src);
static int test_agent_update(juice_agent_t* agent, timestamp_t* next);
static int test_agent_fail(juice_agent_t* agent);
static void test_log(juice_log_level_t level, const char* file, int line, const char* format, ...);

#define udp_recvfrom test_recvfrom
#define agent_conn_recv test_agent_recv
#define agent_conn_update test_agent_update
#define agent_conn_fail test_agent_fail
#define juice_log_write test_log
#define conn_poll_change_tcp_fail orca_test_conn_poll_change_tcp_fail
#define conn_poll_change_tcp_state orca_test_conn_poll_change_tcp_state
#define conn_poll_cleanup orca_test_conn_poll_cleanup
#define conn_poll_get_addrs orca_test_conn_poll_get_addrs
#define conn_poll_init orca_test_conn_poll_init
#define conn_poll_interrupt orca_test_conn_poll_interrupt
#define conn_poll_lock orca_test_conn_poll_lock
#define conn_poll_prepare orca_test_conn_poll_prepare
#define conn_poll_process orca_test_conn_poll_process
#define conn_poll_process_tcp orca_test_conn_poll_process_tcp
#define conn_poll_process_udp orca_test_conn_poll_process_udp
#define conn_poll_recv_udp orca_test_conn_poll_recv_udp
#define conn_poll_registry_cleanup orca_test_conn_poll_registry_cleanup
#define conn_poll_registry_init orca_test_conn_poll_registry_init
#define conn_poll_run orca_test_conn_poll_run
#define conn_poll_send orca_test_conn_poll_send
#define conn_poll_tcp_connect orca_test_conn_poll_tcp_connect
#define conn_poll_unlock orca_test_conn_poll_unlock

#include "conn_poll.c"

static OrcaJuicePollResult result;
static int remaining_drops;
static int receive_fatal_error;

static int test_recvfrom(socket_t sock, char* buffer, size_t size, addr_record_t* src)
{
  (void)sock;
  (void)size;
  ++result.receive_calls;
  int error;
  if (remaining_drops > 0)
  {
    --remaining_drops;
    error = SEMSGSIZE;
  }
  else if (receive_fatal_error)
  {
    error = SEACCES;
  }
  else if (result.delivered == 0)
  {
    memset(src, 0, sizeof(*src));
    src->addr.ss_family = AF_INET;
    src->len = sizeof(struct sockaddr_in);
    buffer[0] = 'z';
    return 1;
  }
  else
  {
    error = SEAGAIN;
  }
#ifdef _WIN32
  WSASetLastError(error);
#else
  errno = error;
#endif
  return -1;
}

static int test_agent_recv(juice_agent_t* agent, char* buffer, size_t size,
                          const addr_record_t* src)
{
  (void)agent;
  (void)src;
  if (size != 1 || buffer[0] != 'z')
    return -1;
  ++result.delivered;
  return 0;
}

static int test_agent_update(juice_agent_t* agent, timestamp_t* next)
{
  (void)agent;
  (void)next;
  ++result.updates;
  return 0;
}

static int test_agent_fail(juice_agent_t* agent)
{
  (void)agent;
  ++result.failures;
  return 0;
}

static void test_log(juice_log_level_t level, const char* file, int line, const char* format, ...)
{
  (void)file;
  (void)line;
  if (level >= JUICE_LOG_LEVEL_ERROR)
    ++result.error_logs;
  if (level == JUICE_LOG_LEVEL_VERBOSE && strcmp(format, "Dropping oversized datagram") == 0)
    ++result.drop_logs;
}

OrcaJuicePollResult OrcaJuicePollRun(int dropped, int fatal_error, int passes)
{
  memset(&result, 0, sizeof(result));
  remaining_drops = dropped;
  receive_fatal_error = fatal_error;
  conn_impl_t conn = {0};
  conn.state = CONN_STATE_READY;
  juice_agent_t agent = {0};
  agent.conn_impl = &conn;
  struct pollfd pfd = {0};
  pfd.revents = POLLIN;
  for (int i = 0; i < passes && conn.state == CONN_STATE_READY; ++i)
    conn_poll_process_udp(&agent, &pfd);
  result.ready = conn.state == CONN_STATE_READY;
  return result;
}
