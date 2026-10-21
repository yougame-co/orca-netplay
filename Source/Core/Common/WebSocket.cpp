// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/WebSocket.h"

#include <algorithm>
#include <array>

#include <curl/curl.h>
#include <fmt/format.h>
#include <picojson.h>

#include "Common/CurlTLS.h"
#include "Common/Network.h"

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

bool IsVersion(std::string_view version)
{
  if (version.empty() || version.size() > 32 || version.front() == '.' || version.back() == '.')
    return false;
  return std::count(version.begin(), version.end(), '.') == 2 &&
         version.find("..") == std::string_view::npos &&
         std::all_of(version.begin(), version.end(), [](char c) {
           return (c >= '0' && c <= '9') || c == '.';
         });
}

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
  std::string loss_class;
  std::optional<long> os_error;
  std::string edge_colo;
  std::function<bool()> cancelled;

  void Fail(std::string why, std::string loss = {})
  {
    if (error.empty())
    {
      error = std::move(why);
      loss_class = std::move(loss);
      long socket_error = 0;
      if (curl && !loss_class.empty() &&
          curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &socket_error) == CURLE_OK)
        os_error = socket_error;
    }
    open = false;
  }

  void Header(std::string_view header)
  {
    if (header.starts_with("HTTP/"))
      edge_colo.clear();
    constexpr std::string_view name = "cf-ray:";
    if (header.size() < name.size() ||
        !std::equal(name.begin(), name.end(), header.begin(), [](char lower, char c) {
          return c == lower || (lower >= 'a' && lower <= 'z' && c == lower - ('a' - 'A'));
        }))
      return;
    const auto end = header.find_last_not_of(" \t\r\n");
    const auto dash = header.rfind('-');
    if (dash == std::string_view::npos || end == std::string_view::npos || end <= dash)
      return;
    const auto colo = header.substr(dash + 1, end - dash);
    if (colo.size() == 3 && std::all_of(colo.begin(), colo.end(), [](char c) {
          return c >= 'A' && c <= 'Z';
        }))
      edge_colo = colo;
  }

  void Release()
  {
    if (curl)
      curl_easy_cleanup(curl);
    curl = nullptr;
    socket = CURL_SOCKET_BAD;
    open = false;
  }

  bool SendClose(std::string_view payload, std::chrono::steady_clock::time_point deadline)
  {
    size_t offset = 0;
    while (true)
    {
      size_t sent = 0;
      const CURLcode result = curl_ws_send(curl, payload.data() + offset, payload.size() - offset,
                                           &sent, 0, CURLWS_CLOSE);
      offset += sent;
      if (result == CURLE_OK && offset == payload.size())
        return true;
      if (result != CURLE_OK && result != CURLE_AGAIN)
        return false;
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0 || !WaitSocket(socket, true, left))
        return false;
    }
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
  m_impl->loss_class.clear();
  m_impl->os_error.reset();
  m_impl->edge_colo.clear();
  m_impl->partial.clear();
  if (OfflineReplay())
  {
    *error = "network off for file replay";
    return false;
  }
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
  const auto header = +[](char* data, size_t size, size_t count, void* target) -> size_t {
    const size_t bytes = size * count;
    static_cast<Impl*>(target)->Header(std::string_view(data, bytes));
    return bytes;
  };
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, m_impl.get());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &refusal);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_impl->cancelled);

  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (result != CURLE_OK || status != 101)
  {
    *error = fmt::format("WebSocket connect failed: {} (HTTP {})",
                         result == CURLE_OK ? "upgrade refused" : curl_easy_strerror(result), status);
    picojson::value body;
    const bool object = picojson::parse(body, refusal).empty() && body.is<picojson::object>();
    const auto field = [&](const char* name) {
      return object && body.get(name).is<std::string>() ? body.get(name).get<std::string>() :
                                                        std::string();
    };
    const std::string code = field("code");
    if (status == 409)
    {
      if (code == "seat_in_use")
        *error += " seat in use";
      else if (refusal.find("This match is complete") != std::string::npos)
        *error += " match complete";
      else if (refusal.find("Room is full") != std::string::npos)
        *error += " room full";
    }
    else if (status == 403 &&
             (code == "host_older" || code == "host_newer" || code == "lobby_mismatch"))
    {
      *error += " " + code;
      const std::string version = field("host_version");
      if (code == "host_older" && IsVersion(version))
        *error += " host_version=" + version;
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
      m_impl->Fail(fmt::format("WebSocket send failed: {}", curl_easy_strerror(result)), "send");
      return false;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0 || !WaitSocket(m_impl->socket, true, left))
    {
      m_impl->Fail("WebSocket send timed out", "send");
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
                       fmt::format("WebSocket receive failed: {}", curl_easy_strerror(result)),
                   result == CURLE_GOT_NOTHING ? "eof" : "recv");
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
      // Complete the close handshake while the connection is still open. Echo only the code;
      // an empty close gets an empty reply rather than a status the server never sent.
      m_impl->SendClose(std::string_view(buffer.data(), received >= 2 ? 2 : 0),
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
      m_impl->Fail(m_impl->close_code ?
                       fmt::format("WebSocket closed by the server ({})", m_impl->close_code) :
                       std::string("WebSocket closed by the server"), "eof");
      m_impl->Release();
      return std::nullopt;
    }
    // libcurl answers pings itself. Only text frames and their continuations carry messages.
    if (!(meta->flags & (CURLWS_TEXT | CURLWS_CONT)))
      continue;
    m_impl->partial.append(buffer.data(), received);
    if (m_impl->partial.size() > MAX_MESSAGE)
    {
      m_impl->Fail("WebSocket message too large", "recv");
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

std::string_view WebSocket::LossClass() const
{
  return m_impl->loss_class;
}

std::optional<long> WebSocket::OsError() const
{
  return m_impl->os_error;
}

const std::string& WebSocket::EdgeColo() const
{
  return m_impl->edge_colo;
}

void WebSocket::Close(u16 code, std::chrono::milliseconds timeout)
{
  if (m_impl->open && m_impl->curl)
  {
    const std::array<char, 2> payload{static_cast<char>(code >> 8), static_cast<char>(code & 0xFF)};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // A busy socket may not take the close immediately. Share the same bounded deadline
    // between sending it and waiting for the acknowledgement.
    if (!m_impl->SendClose(std::string_view(payload.data(), payload.size()), deadline))
    {
      m_impl->Release();
      return;
    }
    // Wait briefly for the server's close frame. Closing with unread data sends a TCP reset,
    // which can drop the last message we sent (such as a room "leave").
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
