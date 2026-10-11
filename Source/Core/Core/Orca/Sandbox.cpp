// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Sandbox.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <sddl.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Random.h"
#ifdef _WIN32
#include "Common/StringUtil.h"
#endif
#include "Core/Orca/Profile.h"

namespace Orca::Sandbox
{
NetMode ParseNetMode(std::string_view text, bool* known)
{
  if (known)
    *known = true;
  if (text.empty())
    return NetMode::All;
  if (text == "internet")
    return NetMode::Internet;
  if (text == "internet-lan")
    return NetMode::InternetLan;
  if (known)
    *known = false;
  return NetMode::Internet;
}

#ifdef _WIN32
namespace
{
// Whether this process runs in an AppContainer (the app's sandbox on Windows).
bool AppContainer()
{
  static const bool app_container = [] {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
      return false;
    DWORD is_app_container = 0;
    DWORD length = 0;
    const bool ok = GetTokenInformation(token, TokenIsAppContainer, &is_app_container,
                                        sizeof(is_app_container), &length) != 0;
    CloseHandle(token);
    return ok && is_app_container != 0;
  }();
  return app_container;
}

// Whether the AppContainer was given a capability, by its fixed SID (S-1-15-3-<n>).
bool HasCapability(const wchar_t* sid_text)
{
  PSID wanted = nullptr;
  if (!ConvertStringSidToSidW(sid_text, &wanted))
    return false;
  bool found = false;
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
  {
    DWORD length = 0;
    GetTokenInformation(token, TokenCapabilities, nullptr, 0, &length);
    if (length > 0 && length < 1024 * 1024)
    {
      std::vector<u8> buffer(length);
      if (GetTokenInformation(token, TokenCapabilities, buffer.data(), length, &length))
      {
        const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
        for (DWORD i = 0; i < groups->GroupCount && !found; ++i)
          found = EqualSid(groups->Groups[i].Sid, wanted) != 0;
      }
    }
    CloseHandle(token);
  }
  LocalFree(wanted);
  return found;
}
}  // namespace
#endif

NetMode NetModeFromEnv()
{
  const std::string text = GetEnv("ORCA_NET");
#ifdef _WIN32
  // An AppContainer the app's launcher didn't describe: what its token allows. Loopback never;
  // the LAN with privateNetworkClientServer (S-1-15-3-3).
  if (text.empty() && AppContainer())
    return HasCapability(L"S-1-15-3-3") ? NetMode::InternetLan : NetMode::Internet;
#endif
  return ParseNetMode(text);
}

std::string_view NetModeName(NetMode mode)
{
  switch (mode)
  {
  case NetMode::Internet:
    return "internet";
  case NetMode::InternetLan:
    return "internet-lan";
  case NetMode::All:
    break;
  }
  return "all";
}

bool LanAllowed(NetMode mode)
{
  return mode != NetMode::Internet;
}

bool LoopbackAllowed(NetMode mode)
{
  return mode == NetMode::All;
}

bool LanAddress(std::string_view address)
{
  const std::string text(address);
  in_addr v4{};
  if (inet_pton(AF_INET, text.c_str(), &v4) == 1)
  {
    const auto* b = reinterpret_cast<const u8*>(&v4);
    return b[0] == 127 || b[0] == 10 || (b[0] == 172 && (b[1] & 0xf0) == 16) ||
           (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
  }
  in6_addr v6{};
  if (inet_pton(AF_INET6, text.c_str(), &v6) == 1)
  {
    const auto* b = reinterpret_cast<const u8*>(&v6);
    static constexpr std::array<u8, 16> LOOPBACK{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (std::memcmp(b, LOOPBACK.data(), LOOPBACK.size()) == 0)
      return true;
    // fe80::/10 link-local, fc00::/7 unique-local.
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
      return true;
    if ((b[0] & 0xfe) == 0xfc)
      return true;
    // An IPv4-mapped address (::ffff:a.b.c.d) is its IPv4 address.
    static constexpr std::array<u8, 12> MAPPED{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(b, MAPPED.data(), MAPPED.size()) == 0)
      return LanAddress(fmt::format("{}.{}.{}.{}", b[12], b[13], b[14], b[15]));
    return false;
  }
  return false;
}

bool LoopbackAddress(std::string_view address)
{
  const std::string text(address);
  in_addr v4{};
  if (inet_pton(AF_INET, text.c_str(), &v4) == 1)
    return reinterpret_cast<const u8*>(&v4)[0] == 127;
  in6_addr v6{};
  if (inet_pton(AF_INET6, text.c_str(), &v6) == 1)
  {
    static constexpr std::array<u8, 16> LOOPBACK{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto* b = reinterpret_cast<const u8*>(&v6);
    static constexpr std::array<u8, 12> MAPPED{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return std::memcmp(b, LOOPBACK.data(), LOOPBACK.size()) == 0 ||
           (std::memcmp(b, MAPPED.data(), MAPPED.size()) == 0 && b[12] == 127);
  }
  return false;
}

bool AddressAllowed(NetMode mode, std::string_view address)
{
  if (mode == NetMode::All)
    return true;
  if (LoopbackAddress(address))
    return false;
  return LanAllowed(mode) || !LanAddress(address);
}

bool Jailed()
{
#ifdef _WIN32
  if (AppContainer())
    return true;
#endif
  return !GetEnv("ORCA_NET").empty();
}

bool AdapterAllowed(bool session)
{
  return !session;
}

Os ThisOs()
{
#if defined(_WIN32)
  return Os::Windows;
#elif defined(__APPLE__)
  return Os::MacOS;
#else
  return Os::Other;
#endif
}

bool OsKeyboardAllowed(Os os, bool session, bool stdio)
{
  return !(os == Os::MacOS && session && stdio);
}

bool ReadDenied(const std::string& path)
{
#ifdef _WIN32
  const HANDLE file = CreateFileW(UTF8ToWString(path).c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file != INVALID_HANDLE_VALUE)
  {
    CloseHandle(file);
    return false;
  }
  return GetLastError() == ERROR_ACCESS_DENIED;
#else
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd >= 0)
  {
    close(fd);
    return false;
  }
  return errno == EACCES || errno == EPERM;
#endif
}

bool WriteDenied(const std::string& dir)
{
  u64 nonce = 0;
  Common::Random::Generate(&nonce, sizeof(nonce));
  const std::string path = fmt::format("{}/.orca-write-check-{:016x}", dir, nonce);
#ifdef _WIN32
  const HANDLE file =
      CreateFileW(UTF8ToWString(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file != INVALID_HANDLE_VALUE)
  {
    CloseHandle(file);
    return false;
  }
  return GetLastError() == ERROR_ACCESS_DENIED;
#else
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd >= 0)
  {
    close(fd);
    unlink(path.c_str());
    return false;
  }
  return errno == EACCES || errno == EPERM;
#endif
}
}  // namespace Orca::Sandbox
