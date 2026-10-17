// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Common
{
// A WebSocket client for text messages (ws:// and wss://), built on libcurl's WebSocket support.
// Not thread-safe: connect, send and receive from a single thread.
class WebSocket final
{
public:
  WebSocket();
  ~WebSocket();
  WebSocket(const WebSocket&) = delete;
  WebSocket& operator=(const WebSocket&) = delete;

  // Opens the connection and completes the upgrade. On failure returns false and sets *error.
  bool Connect(const std::string& url, std::chrono::milliseconds timeout, std::string* error,
               const std::function<bool()>& cancelled = {});

  // Sends one text message, waiting at most `timeout` for the socket to take it.
  bool SendText(std::string_view text, std::chrono::milliseconds timeout);

  // Returns the next complete text message, waiting at most `timeout`. Returns nullopt on timeout
  // or when the connection closed (check IsOpen).
  std::optional<std::string> ReceiveText(std::chrono::milliseconds timeout);

  bool IsOpen() const;
  // Why the connection closed or failed (empty while open).
  const std::string& Error() const;
  // The status code from the server's close frame, or 0 if none arrived.
  u16 CloseCode() const;

  // Sends a close frame (best effort) and releases the connection.
  void Close(u16 code = 1000, std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}  // namespace Common
