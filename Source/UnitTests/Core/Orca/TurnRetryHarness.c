// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string.h>

#include "stun.h"

// The C STUN API has parameter names that are C++ keywords.
int OrcaTestTurnStaleNonceReply(void* data, int size, void* reply, int capacity)
{
  stun_message_t request = {0};
  if (stun_read(data, size, &request) < 0)
    return -1;
  if (request.msg_method != STUN_METHOD_ALLOCATE || !request.has_integrity)
    return 0;
  stun_message_t response = {0};
  response.msg_class = STUN_CLASS_RESP_ERROR;
  response.msg_method = STUN_METHOD_ALLOCATE;
  response.error_code = 438;
  memcpy(response.transaction_id, request.transaction_id, STUN_TRANSACTION_ID_SIZE);
  response.credentials = request.credentials;
  response.credentials.password_algorithm = STUN_PASSWORD_ALGORITHM_UNSET;
  return stun_write(reply, capacity, &response, NULL);
}
