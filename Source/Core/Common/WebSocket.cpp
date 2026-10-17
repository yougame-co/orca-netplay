// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/WebSocket.h"

#include <array>

#include <curl/curl.h>
#include <fmt/format.h>

#include "Common/CurlTLS.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace Common
{
namespace
{
constexpr size_t MAX_MESSAGE = 1024 * 1024;

// Waits until the socket is readable (or writable). Returns false on timeout or error.
bool WaitSocket(curl_socket_t socket, bool for_write, std::chrono::milliseconds timeout)
{
#ifdef _WIN32
  WSAPOLLFD pfd{socket, static_cast<SHORT>(for_write ? POLLWRNORM : POLLRDNORM), 0};
  return WSAPoll(&pfd, 1, static_cast<INT>(timeout.count())) > 0;
#else
  pollfd pfd{socket, static_cast<short>(for_write ? POLLOUT : POLLIN), 0};
  return poll(&pfd, 1, static_cast<int>(timeout.count())) > 0;
#endif
}
}  // namespace

struct WebSocket::Impl
{
  CURL* curl = nullptr;
  curl_socket_t socket = CURL_SOCKET_BAD;
  bool open = false;
  std::string error;
  u16 close_code = 0;
  std::string partial;
  std::string refusal;
  std::function<bool()> cancelled;

  void Fail(std::string why)
  {
    if (error.empty())
      error = std::move(why);
    open = false;
  }

  void Release()
  {
    if (curl)
      curl_easy_cleanup(curl);
    curl = nullptr;
    socket = CURL_SOCKET_BAD;
    open = false;
  }
};

WebSocket::WebSocket() : m_impl(std::make_unique<Impl>())
{
}

WebSocket::~WebSocket()
{
  Close();
}

bool WebSocket::Connect(const std::string& url, std::chrono::milliseconds timeout,
                        std::string* error, const std::function<bool()>& cancelled)
{
  m_impl->Release();
  m_impl->error.clear();
  m_impl->close_code = 0;
  m_impl->partial.clear();
  m_impl->curl = curl_easy_init();
  if (!m_impl->curl)
  {
    *error = "curl_easy_init failed";
    return false;
  }
  CURL* curl = m_impl->curl;
  ConfigureCurlTLS(curl);
  for (const auto& [option, value] : std::array<std::pair<CURLoption, const char*>, 2>{{
           {CURLOPT_URL, url.c_str()}, {CURLOPT_USERAGENT, "Orca/1"}}})
    curl_easy_setopt(curl, option, value);
  // Capture only a bounded refusal body; expose recognized reasons, never arbitrary text.
  auto& refusal = m_impl->refusal;
  refusal.clear();
  m_impl->cancelled = cancelled;
  const auto write = +[](char* data, size_t size, size_t count, void* target) -> size_t {
    auto& body = *static_cast<std::string*>(target);
    const size_t bytes = size * count;
    if (bytes > 8192 - body.size())
      return 0;
    body.append(data, bytes);
    return bytes;
  };
  const auto progress = +[](void* target, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
    const auto& cancel = *static_cast<const std::function<bool()>*>(target);
    return cancel && cancel();
  };
  for (const auto& [option, value] : std::array<std::pair<CURLoption, long>, 5>{{
           {CURLOPT_CONNECT_ONLY, 2L}, {CURLOPT_CONNECTTIMEOUT_MS, timeout.count()},
           {CURLOPT_TIMEOUT_MS, timeout.count()}, {CURLOPT_NOSIGNAL, 1L},
           {CURLOPT_NOPROGRESS, cancelled ? 0L : 1L}}})
    curl_easy_setopt(curl, option, value);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &refusal);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_impl->cancelled);

  const CURLcode result = curl_easy_perform(curl);
  if (result != CURLE_OK)
  {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    *error = fmt::format("WebSocket connect failed: {} (HTTP {})", curl_easy_strerror(result),
                         status);
    if (status == 409)
    {
      if (refusal.find("This match is complete") != std::string::npos)
        *error += " match complete";
      else if (refusal.find("Room is full") != std::string::npos)
        *error += " room full";
    }
    m_impl->Release();
    return false;
  }
  curl_socket_t socket = CURL_SOCKET_BAD;
  curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket);
  if (socket == CURL_SOCKET_BAD)
  {
    *error = "WebSocket connect left no socket";
    m_impl->Release();
    return false;
  }
  // The live socket retains no caller captures after the handshake.
  m_impl->cancelled = {};
  // The timeout was meant for the handshake only. libcurl keeps applying it afterwards, and on
  // Windows (Schannel) every send fails once the socket is older than it, so clear it.
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L);
  m_impl->socket = socket;
  m_impl->open = true;
  return true;
}

bool WebSocket::SendText(std::string_view text, std::chrono::milliseconds timeout)
{
  if (!m_impl->open)
    return false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  size_t offset = 0;
  while (offset < text.size() || text.empty())
  {
    size_t sent = 0;
    const CURLcode result =
        curl_ws_send(m_impl->curl, text.data() + offset, text.size() - offset, &sent,
                     offset == 0 ? static_cast<curl_off_t>(0) : 0, CURLWS_TEXT);
    offset += sent;
    if (result == CURLE_OK)
    {
      if (offset >= text.size())
        return true;
      continue;
    }
    if (result != CURLE_AGAIN)
    {
      m_impl->Fail(fmt::format("WebSocket send failed: {}", curl_easy_strerror(result)));
      return false;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0 || !WaitSocket(m_impl->socket, true, left))
    {
      m_impl->Fail("WebSocket send timed out");
      return false;
    }
  }
  return true;
}

std::optional<std::string> WebSocket::ReceiveText(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<char, 16384> buffer;
  while (m_impl->open)
  {
    size_t received = 0;
    const curl_ws_frame* meta = nullptr;
    const CURLcode result =
        curl_ws_recv(m_impl->curl, buffer.data(), buffer.size(), &received, &meta);
    if (result == CURLE_AGAIN)
    {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0 || !WaitSocket(m_impl->socket, false, left))
        return std::nullopt;
      continue;
    }
    if (result != CURLE_OK)
    {
      m_impl->Fail(result == CURLE_GOT_NOTHING ?
                       "WebSocket closed by the server" :
                       fmt::format("WebSocket receive failed: {}", curl_easy_strerror(result)));
      return std::nullopt;
    }
    if (!meta)
      continue;
    if (meta->flags & CURLWS_CLOSE)
    {
      if (received >= 2)
      {
        m_impl->close_code =
            static_cast<u16>(static_cast<u8>(buffer[0]) << 8 | static_cast<u8>(buffer[1]));
      }
      m_impl->Fail(m_impl->close_code ?
                       fmt::format("WebSocket closed by the server ({})", m_impl->close_code) :
                       std::string("WebSocket closed by the server"));
      return std::nullopt;
    }
    // libcurl answers pings itself. Only text frames and their continuations carry messages.
    if (!(meta->flags & (CURLWS_TEXT | CURLWS_CONT)))
      continue;
    m_impl->partial.append(buffer.data(), received);
    if (m_impl->partial.size() > MAX_MESSAGE)
    {
      m_impl->Fail("WebSocket message too large");
      return std::nullopt;
    }
    if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT))
    {
      std::string message = std::move(m_impl->partial);
      m_impl->partial.clear();
      return message;
    }
  }
  return std::nullopt;
}

bool WebSocket::IsOpen() const
{
  return m_impl->open;
}

u16 WebSocket::CloseCode() const
{
  return m_impl->close_code;
}

const std::string& WebSocket::Error() const
{
  return m_impl->error;
}

void WebSocket::Close(u16 code, std::chrono::milliseconds timeout)
{
  if (m_impl->open && m_impl->curl)
  {
    const std::array<char, 2> payload{static_cast<char>(code >> 8), static_cast<char>(code & 0xFF)};
    size_t sent = 0;
    curl_ws_send(m_impl->curl, payload.data(), payload.size(), &sent, 0, CURLWS_CLOSE);
    // Wait briefly for the server's close frame. Closing with unread data sends a TCP reset,
    // which can drop the last message we sent (such as a room "leave").
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> buffer;
    while (true)
    {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0)
        break;
      size_t got = 0;
      const curl_ws_frame* meta = nullptr;
      const CURLcode result = curl_ws_recv(m_impl->curl, buffer.data(), buffer.size(), &got, &meta);
      if (result == CURLE_AGAIN)
      {
        if (!WaitSocket(m_impl->socket, false, left))
          break;
        continue;
      }
      if (result != CURLE_OK || (meta && (meta->flags & CURLWS_CLOSE)))
        break;
    }
  }
  m_impl->Release();
}
}  // namespace Common
