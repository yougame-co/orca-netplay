// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Sandbox.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
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
#ifdef __APPLE__
#include <dlfcn.h>
#endif
#endif

#ifdef __linux__
#include <dirent.h>
#include <poll.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/un.h>
#endif

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Random.h"
#ifdef _WIN32
#include "Common/StringUtil.h"
#endif
#include "Core/Orca/PackJail.h"
#include "Core/Orca/Profile.h"
#ifdef __linux__
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Core/NetPlayProto.h"
#include "Core/Orca/Session/Online.h"
#endif

namespace Orca::Sandbox
{
// BEGIN Linux pack jail policy (also compiled verbatim by the standalone device harness).
namespace
{
std::optional<std::string> JailRead(const std::string& path)
{
  std::ifstream file(path);
  if (!file)
    return std::nullopt;
  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad() || text.fail())
    return std::nullopt;
  return text.str();
}

bool JailNumber(std::string_view text, unsigned long long* value, int base = 10)
{
  if (text.empty())
    return false;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *value, base);
  return error == std::errc{} && end == text.data() + text.size();
}

std::string JailLogName(std::string_view name)
{
  std::string safe;
  for (const unsigned char c : name)
    safe += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':') ?
                static_cast<char>(c) : '_';
  return safe;
}
}  // namespace

std::string PackJailResult::Status() const
{
  const std::string comm = " pid1=" + JailLogName(pid1) +
                           (passwd_present ? " passwd: <present>" : " passwd: none");
  if (!jailed)
    return "orca pack jail refused " + failed + comm;
  std::string names;
  for (const auto& name : interfaces)
    names += (names.empty() ? "" : ",") + JailLogName(name);
  return "orca pack jail passed NoNewPrivs CapEff CapBnd CapPrm uid_map root-fs "
         "host-home sensitive-home HOME DISPLAY x11-abstract x11-path x11-own network interfaces=" +
         names + comm;
}

PackJailResult CheckLinuxPackJail(const std::string& root, const PackJailProbes& probes, bool online)
{
  PackJailResult result;
  const auto passwd_home = probes.passwd_home();
  result.passwd_present = passwd_home.has_value();
  // PID 1 is diagnostic only: bwrap, sh and other names all qualify. Read failures fail closed.
  const auto comm = JailRead(root + "/proc/1/comm");
  if (comm)
  {
    result.pid1 = *comm;
    if (!result.pid1.empty() && result.pid1.back() == '\n')
      result.pid1.pop_back();
  }
  const auto fail = [&](const std::string& name) {
    result.failed = name;
    return result;
  };
  const auto status = JailRead(root + "/proc/self/status");
  for (const char* field : {"NoNewPrivs", "CapEff", "CapBnd", "CapPrm"})
  {
    if (!status)
      return fail(field);
    std::istringstream lines(*status);
    std::string line;
    bool seen = false;
    while (std::getline(lines, line))
    {
      if (line.compare(0, std::strlen(field) + 1, std::string(field) + ':') != 0)
        continue;
      if (seen)
        return fail(field);
      seen = true;
      std::istringstream values(line.substr(std::strlen(field) + 1));
      std::string value, extra;
      unsigned long long number = 0;
      const bool nnp = std::string_view(field) == "NoNewPrivs";
      if (!(values >> value) || (values >> extra) ||
          !JailNumber(value, &number, nnp ? 10 : 16) || number != (nnp ? 1u : 0u))
        return fail(field);
    }
    if (!seen)
      return fail(field);
  }

  const auto mapping = JailRead(root + "/proc/self/uid_map");
  if (!mapping)
    return fail("uid_map");
  std::istringstream maps(*mapping);
  std::string inside, outside, count;
  unsigned int rows = 0;
  bool identity = false;
  while (maps >> inside)
  {
    unsigned long long a = 0, b = 0, n = 0;
    if (!(maps >> outside >> count) || !JailNumber(inside, &a) || !JailNumber(outside, &b) ||
        !JailNumber(count, &n) || a > 4294967295ULL || b > 4294967295ULL || n == 0 ||
        n > 4294967295ULL || a + n > 4294967296ULL || b + n > 4294967296ULL)
      return fail("uid_map");
    ++rows;
    identity = a == 0 && b == 0 && n == 4294967295ULL;
  }
  if (rows == 0 || (rows == 1 && identity))
    return fail("uid_map");

  const auto root_type = probes.root_type();
  if (!root_type || (*root_type != 0x01021994 && *root_type != 0x794c7630))
    return fail("root-fs");
  std::vector<std::string> homes;
  const auto placeholder = [&](const std::string& path) {
    // Profile binds create parents on the jail root. A host bind, symlink, regular file,
    // or unreadable entry must never qualify as one of those placeholder directories.
    const auto directory = probes.directory(path);
    const auto root_device = probes.root_device();
    return directory && root_device && directory->device == *root_device &&
           (directory->fs_type == 0x01021994 || directory->fs_type == 0x794c7630);
  };
  for (const char* base : {"/home", "/var/home"})
  {
    const auto entries = probes.home_entries(base);
    if (!entries)
      return fail("host-home");
    if (entries->symlink)
      return fail("host-home symlink");
    for (const auto& entry : entries->names)
    {
      const std::string path = std::string(base) + '/' + entry;
      if (!placeholder(path))
        return fail("host-home");
      homes.push_back(path);
    }
  }
  if (passwd_home)
  {
    if (passwd_home->empty() || passwd_home->front() != '/')
      return fail("host-home");
    if (*passwd_home != "/home" && passwd_home->compare(0, 6, "/home/") != 0 &&
        *passwd_home != "/var/home" && passwd_home->compare(0, 10, "/var/home/") != 0)
    {
      const auto visible = probes.exists(*passwd_home);
      if (!visible || (*visible && !placeholder(*passwd_home)))
        return fail("host-home");
    }
    if (std::find(homes.begin(), homes.end(), *passwd_home) == homes.end())
      homes.push_back(*passwd_home);
  }
  for (const auto& home : homes)
  {
    for (const char* relative : {".ssh", ".gnupg", ".local/share/keyrings", ".mozilla",
                                 ".config/google-chrome", ".config/chromium",
                                 ".config/BraveSoftware", ".local/share/Steam"})
    {
      const auto sensitive = probes.exists(home + '/' + relative);
      if (!sensitive || *sensitive)
        return fail(std::string("sensitive-home ") + relative);
    }
  }
  if (!probes.home || probes.home->empty() ||
      std::find(homes.begin(), homes.end(), *probes.home) != homes.end())
    return fail("HOME");

  if (!probes.display || probes.display->empty() || probes.display->front() != ':')
    return fail("DISPLAY");
  const std::string_view display(*probes.display);
  const auto dot = display.find('.');
  const auto number = display.substr(1, dot == std::string_view::npos ? dot : dot - 1);
  unsigned long long own = 0, screen = 0;
  if (!JailNumber(number, &own) || own > 2147483647ULL ||
      (dot != std::string_view::npos && !JailNumber(display.substr(dot + 1), &screen)))
    return fail("DISPLAY");
  for (int n = 0; n <= 9; ++n)
  {
    if (n == static_cast<int>(own))
      continue;
    const auto abstract = probes.connect_x(n, true);
    if (!abstract || *abstract)
      return fail("x11-abstract");
    const auto path = probes.connect_x(n, false);
    if (!path || *path)
      return fail("x11-path");
  }
  const auto connected = probes.connect_x(static_cast<int>(own), false);
  if (!connected || !*connected)
    return fail("x11-own");

  // The jail binds the host's sysfs for GPU enumeration, so /sys/class/net shows host
  // interfaces. Its fresh procfs reports this process's own network namespace instead.
  const auto network = JailRead(root + "/proc/self/net/dev");
  if (!network)
    return fail("network");
  std::istringstream devices(*network);
  std::string line;
  if (!std::getline(devices, line) || !std::getline(devices, line))
    return fail("network");
  while (std::getline(devices, line))
  {
    const auto colon = line.find(':');
    if (colon == std::string::npos)
      return fail("network");
    const auto begin = line.find_first_not_of(" \t");
    const auto end = line.find_last_not_of(" \t", colon - 1);
    if (begin == std::string::npos || begin >= colon || end == std::string::npos || end >= colon)
      return fail("network");
    const auto name = line.substr(begin, end - begin + 1);
    if (name.size() > 15 || name == "." || name == ".." ||
        name.find_first_of(" \t\r\n/:") != std::string::npos ||
        name.find('\0') != std::string::npos)
      return fail("network");
    std::istringstream counters(line.substr(colon + 1));
    std::string value;
    for (int column = 0; column < 16; ++column)
    {
      unsigned long long counter = 0;
      if (!(counters >> value) || !JailNumber(value, &counter))
        return fail("network");
    }
    if (counters >> value)
      return fail("network");
    result.interfaces.push_back(name);
  }
  std::sort(result.interfaces.begin(), result.interfaces.end());
  if (result.interfaces.empty() ||
      std::adjacent_find(result.interfaces.begin(), result.interfaces.end()) != result.interfaces.end() ||
      (!online && result.interfaces != std::vector<std::string>{"lo"}))
    return fail("network");
  if (!comm)
    return fail("pid1-comm");
  result.jailed = true;
  return result;
}

#ifdef __linux__
PackJailProbes LinuxPackJailProbes()
{
  PackJailProbes probes;
  probes.root_type = []() -> std::optional<long> {
    struct statfs fs{};
    if (statfs("/", &fs) != 0)
      return std::nullopt;
    return fs.f_type;
  };
  probes.passwd_home = []() -> std::optional<std::string> {
    const auto* entry = getpwuid(getuid());
    if (!entry || !entry->pw_dir)
      return std::nullopt;
    return entry->pw_dir;
  };
  probes.home_entries = [](const std::string& path) -> std::optional<PackJailHomeEntries> {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0)
    {
      if (errno == ENOENT)
        return PackJailHomeEntries{};
      return std::nullopt;
    }
    if (S_ISLNK(st.st_mode))
      return PackJailHomeEntries{true, {}};
    if (!S_ISDIR(st.st_mode))
      return std::nullopt;
    DIR* directory = opendir(path.c_str());
    if (!directory)
      return std::nullopt;
    PackJailHomeEntries entries;
    int error = 0;
    while (true)
    {
      errno = 0;
      const auto* entry = readdir(directory);
      if (!entry)
      {
        error = errno;
        break;
      }
      const std::string name = entry->d_name;
      if (name != "." && name != "..")
        entries.names.push_back(name);
    }
    if (closedir(directory) != 0 || error != 0)
      return std::nullopt;
    std::sort(entries.names.begin(), entries.names.end());
    return entries;
  };
  probes.root_device = []() -> std::optional<unsigned long long> {
    char* root = realpath("/", nullptr);
    if (!root)
      return std::nullopt;
    struct stat st{};
    const int error = stat(root, &st);
    free(root);
    if (error != 0)
      return std::nullopt;
    return st.st_dev;
  };
  probes.directory = [](const std::string& path) -> std::optional<PackJailDirectory> {
    struct stat entry{}, st{};
    struct statfs fs{};
    if (lstat(path.c_str(), &entry) != 0 || !S_ISDIR(entry.st_mode) ||
        stat(path.c_str(), &st) != 0 || statfs(path.c_str(), &fs) != 0)
      return std::nullopt;
    return PackJailDirectory{static_cast<unsigned long long>(st.st_dev), fs.f_type};
  };
  probes.exists = [](const std::string& path) -> std::optional<bool> {
    struct stat st{};
    if (lstat(path.c_str(), &st) == 0)
      return true;
    if (errno == ENOENT || errno == ENOTDIR)
      return false;
    return std::nullopt;
  };
  probes.connect_x = [](int number, bool abstract) -> std::optional<bool> {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return std::nullopt;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string name = "/tmp/.X11-unix/X" + std::to_string(number);
    std::memcpy(address.sun_path + (abstract ? 1 : 0), name.data(), name.size());
    const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size() + 1);
    std::optional<bool> connected;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), length) == 0)
    {
      connected = true;
    }
    else if (errno == EINPROGRESS)
    {
      pollfd event{fd, POLLOUT, 0};
      const int ready = poll(&event, 1, 100);
      int socket_error = 0;
      socklen_t size = sizeof(socket_error);
      if (ready > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &size) == 0)
        connected = socket_error == 0;
      else if (ready == 0)
        connected = false;
    }
    else if (errno == ENOENT || errno == ECONNREFUSED || errno == EACCES || errno == ENETUNREACH)
    {
      connected = false;
    }
    close(fd);
    return connected;
  };
  if (const char* home = std::getenv("HOME"))
    probes.home = home;
  if (const char* display = std::getenv("DISPLAY"))
    probes.display = display;
  return probes;
}
#endif
// END Linux pack jail policy.

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

bool PackJailed()
{
#ifdef _WIN32
  return AppContainer();
#elif defined(__APPLE__)
  // The app applies the sandbox before launching Orca. Ask the OS about this process, including
  // inherited profiles. A null operation queries whether a sandbox is applied. Resolve the
  // system entry point at runtime; an unavailable query cannot authorize a pack.
  void* library = dlopen("/usr/lib/libsandbox.dylib", RTLD_LAZY | RTLD_LOCAL);
  if (!library)
    return false;
  using Check = int (*)(pid_t, const char*, int, ...);
  const auto check = reinterpret_cast<Check>(dlsym(library, "sandbox_check"));
  const bool jailed = check && check(getpid(), nullptr, 0) == 1;
  dlclose(library);
  return jailed;
#elif defined(__linux__)
  // Enabled() is the boot's room intent, before Start() creates a transport. File replays
  // never start a room; SessionActive() alone also includes unsigned local packs.
  const bool online = NetPlay::IsNetPlayRunning() ||
                      (Online::Enabled() && !Common::OfflineReplay());
  const auto result = CheckLinuxPackJail("", LinuxPackJailProbes(), online);
  NOTICE_LOG_FMT(BOOT, "{}", result.Status());
  return result.jailed;
#else
  return false;
#endif
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
