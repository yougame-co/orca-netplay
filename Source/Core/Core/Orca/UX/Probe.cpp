// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Probe.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/Games/Brawl/RankedSet.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// Research tool for reverse-engineering Brawl's menus. Never active in a real session.
// ORCA_UX_PROBE=<file> runs one command per line, at a frame counted from entry into a scene:
//   @<scene> <frame> write <addr> <hex bytes>      e.g. @muMenuMain 20 write 90180c10 0000
//   @<scene> <frame> utf16 <addr> <text>           UTF-16BE text with a 0 terminator
//   @<scene> <frame> dump <addr> <length> <name>   writes <user dir>/probe-<name>.bin
//   @* 0 watch <addr> <name>                       logs the u32 there whenever it changes
//   @* 0 fighters - -                              logs both fighters' status kinds on change
//   @<scene> <frame> stall <ms> 0                  blocks the CPU thread like a session stall
// @<scene>:<n> runs only on the n-th entry into the scene. <addr> is hex, or [hex]+hex for one
// pointer hop (e.g. [805a00e0]+28).
namespace Orca::UX
{
namespace
{
struct Command
{
  std::string scene;
  int visit = 0;  // nonzero: only on that entry into the scene (1 is the first)
  int frame = 0;
  std::string verb;
  std::string addr;
  std::string arg;
  std::string arg2;
  bool done = false;
  u32 last = 0;
};

std::vector<Command> Load()
{
  std::vector<Command> out;
  const char* path = std::getenv("ORCA_UX_PROBE");
  if (!path)
    return out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream s(line);
    Command c;
    s >> c.scene >> c.frame >> c.verb >> c.addr >> c.arg;
    std::getline(s, c.arg2);
    if (!c.arg2.empty() && c.arg2[0] == ' ')
      c.arg2.erase(0, 1);
    if (c.scene.starts_with("@"))
      c.scene.erase(0, 1);
    if (const auto colon = c.scene.find(':'); colon != std::string::npos)
    {
      c.visit = std::atoi(c.scene.c_str() + colon + 1);
      c.scene.resize(colon);
    }
    out.push_back(c);
  }
  return out;
}

bool Mapped(const Core::CPUThreadGuard& guard, u32 address)
{
  return PowerPC::MMU::HostIsRAMAddress(guard, address);
}

std::string SceneName(const Core::CPUThreadGuard& guard)
{
  const u32 manager = PowerPC::MMU::HostRead<u32>(guard, 0x805a0060);
  if (!Mapped(guard, manager + 0x4))
    return {};
  const u32 scene = PowerPC::MMU::HostRead<u32>(guard, manager + 0x4);
  if (!Mapped(guard, scene))
    return {};
  const u32 name_ptr = PowerPC::MMU::HostRead<u32>(guard, scene);
  if (!Mapped(guard, name_ptr))
    return {};
  std::string name;
  for (u32 i = 0; i < 32 && Mapped(guard, name_ptr + i); ++i)
  {
    const char c = static_cast<char>(PowerPC::MMU::HostRead<u8>(guard, name_ptr + i));
    if (c == '\0')
      break;
    name.push_back(c);
  }
  return name;
}

// expr := hex | '[' expr ']' [ '+' hex ]  (each [..] reads the u32 there).
u32 Address(const Core::CPUThreadGuard& guard, std::string_view expr)
{
  if (expr.starts_with("["))
  {
    int depth = 0;
    size_t close = 0;
    for (size_t i = 0; i < expr.size(); ++i)
    {
      if (expr[i] == '[')
        ++depth;
      else if (expr[i] == ']' && --depth == 0)
      {
        close = i;
        break;
      }
    }
    if (close == 0)
      return 0;  // unbalanced brackets: no address
    const u32 at = Address(guard, expr.substr(1, close - 1));
    u32 offset = 0;
    if (close + 1 < expr.size() && expr[close + 1] == '+')
      offset = static_cast<u32>(std::strtoul(std::string(expr.substr(close + 2)).c_str(), nullptr, 16));
    return Mapped(guard, at) ? PowerPC::MMU::HostRead<u32>(guard, at) + offset : 0;
  }
  return static_cast<u32>(std::strtoul(std::string(expr).c_str(), nullptr, 16));
}
}  // namespace

bool TestKnobsAllowed()
{
  const char* dev = std::getenv("ORCA_TEST_DEV_GAME");
  return !Orca::Online::Enabled() || (dev && dev[0] != '\0');
}

void ProbeFrame(const Core::CPUThreadGuard& guard, int frame)
{
  // ORCA_UX_PROBE_WATCHDOG=1: if no frame arrives for 2 s, log the guest's pc and lr from a host
  // thread. The reads are racy and only show roughly where the game is waiting.
  static std::atomic<int> s_last_frame{0};
  s_last_frame = frame;
  static const bool s_dog = [] {
    if (!std::getenv("ORCA_UX_PROBE_WATCHDOG") || !TestKnobsAllowed())
      return false;
    std::thread([] {
      auto& ppc = Core::System::GetInstance().GetPPCState();
      int seen = -1, quiet = 0;
      for (;;)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const int f = s_last_frame;
        quiet = f == seen ? quiet + 1 : 0;
        seen = f;
        if (quiet >= 8 && quiet % 4 == 0)
          NOTICE_LOG_FMT(ROLLBACK, "Watchdog: no frame since {}: pc {:08x} lr {:08x}", f, ppc.pc,
                         ppc.spr[SPR_LR]);
      }
    }).detach();
    return true;
  }();
  (void)s_dog;
  static std::vector<Command> commands = Load();
  // Checked every frame because a solo host can be joined by a real player later.
  if (commands.empty() || !TestKnobsAllowed())
    return;
  static std::string scene;
  static int scene_start = 0;
  static std::map<std::string, int> visits;  // entries per scene, rollback re-runs included
  const std::string now = SceneName(guard);
  if (now != scene)
  {
    scene = now;
    scene_start = frame;
    ++visits[scene];
  }
  const int rel = frame - scene_start;
  for (Command& c : commands)
  {
    if (c.verb == "fighters")
    {
      const GuardMemory memory(guard);
      const RankedSet::Live live = RankedSet::ReadLiveFight(memory);
      const u32 value = live.valid ? live.status[0] << 16 | (live.status[1] & 0xFFFF) : 0xDEADDEAD;
      if (!c.done || value != c.last)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Fighters frame {} ({} {}): p1 {:x} p2 {:x}", frame, scene, rel,
                       value >> 16, value & 0xFFFF);
        c.last = value;
        c.done = true;
      }
      continue;
    }
    if (c.verb == "watch")
    {
      const u32 at = Address(guard, c.addr);
      const u32 value = at && Mapped(guard, at) ? PowerPC::MMU::HostRead<u32>(guard, at) : 0xDEADDEAD;
      if (!c.done || value != c.last)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Watch frame {} ({} {}): {} = {:08x}", frame, scene, rel, c.arg,
                       value);
        c.last = value;
        c.done = true;
      }
      continue;
    }
    if (c.done || c.scene != scene || c.frame != rel || (c.visit && c.visit != visits[scene]))
      continue;
    c.done = true;
    if (c.verb == "stall")
    {
      const int ms = std::atoi(c.addr.c_str());
      for (int waited = 0; waited < ms; waited += 10)
      {
        RepresentDuringStall(waited);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      NOTICE_LOG_FMT(ROLLBACK, "Probe {} {}: stalled {} ms", scene, rel, ms);
      continue;
    }
    const u32 addr = Address(guard, c.addr);
    if (!Mapped(guard, addr))
    {
      NOTICE_LOG_FMT(ROLLBACK, "Probe {} {}: {} is not mapped", scene, rel, c.addr);
      continue;
    }
    if (c.verb == "write")
    {
      const std::string hex = c.arg + c.arg2;
      std::string bytes;
      for (char ch : hex)
        if (std::isxdigit(static_cast<unsigned char>(ch)))
          bytes.push_back(ch);
      for (size_t i = 0; i + 1 < bytes.size(); i += 2)
        PowerPC::MMU::HostWrite<u8>(
            guard, static_cast<u8>(std::strtoul(bytes.substr(i, 2).c_str(), nullptr, 16)),
            addr + static_cast<u32>(i / 2));
    }
    else if (c.verb == "utf16")
    {
      const std::string text = c.arg + (c.arg2.empty() ? "" : " " + c.arg2);
      u32 a = addr;
      for (unsigned char ch : text)
      {
        PowerPC::MMU::HostWrite<u16>(guard, ch, a);
        a += 2;
      }
      PowerPC::MMU::HostWrite<u16>(guard, 0, a);
    }
    else if (c.verb == "dump")
    {
      const u32 length = static_cast<u32>(std::strtoul(c.arg.c_str(), nullptr, 16));
      std::vector<u8> data(length);
      for (u32 i = 0; i < length; ++i)
        data[i] = PowerPC::MMU::HostRead<u8>(guard, addr + i);
      File::IOFile(File::GetUserPath(D_USER_IDX) + fmt::format("probe-{}.bin", c.arg2), "wb")
          .WriteBytes(data.data(), data.size());
    }
    NOTICE_LOG_FMT(ROLLBACK, "Probe {} {}: {} {:08x} {} {}", scene, rel, c.verb, addr, c.arg,
                   c.arg2);
  }
}
}  // namespace Orca::UX
