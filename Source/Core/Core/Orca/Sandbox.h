// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

// Orca inside the YouGame app's sandbox (ORCA.md "The sandbox"). On Windows the app starts Orca
// through yougame-sandbox.exe: an AppContainer that keeps it off loopback, and off the LAN unless
// the player's setting grants it. Orca's side of that is a handful of switches, each read here:
//   YOUGAME_STDIO=1   the app's bridge is Orca's own stdin and stdout (Orca/Bridge.h)
//   --embed-adopted   Windows: the window waits to be adopted into the app's (DolphinNoGUI/Embed.h)
//   ORCA_NET          the network the sandbox gives Orca (NetMode), for the direct links
// and what every session does: Dolphin's own GameCube adapter code never starts (the app's helper
// owns the adapter), and a Mac on the stdio bridge reads no keyboard of its own (the page sends it
// as a pad). Without the sandbox's switches Orca behaves as before.
namespace Orca::Sandbox
{
enum class NetMode
{
  // Not in the sandbox: every candidate, as before.
  All,
  // The internet only: no loopback and no LAN (Windows' internetClient alone).
  Internet,
  // The internet and the LAN (privateNetworkClientServer too), never loopback.
  InternetLan,
};
// "internet" or "internet-lan"; empty is All. Anything else is Internet, the strictest, and sets
// *known to false.
NetMode ParseNetMode(std::string_view text, bool* known = nullptr);
// ORCA_NET (read each call; it never changes in a run). In a Windows AppContainer started without
// it: Internet, or InternetLan when the token holds privateNetworkClientServer.
NetMode NetModeFromEnv();
std::string_view NetModeName(NetMode mode);
bool LanAllowed(NetMode mode);
bool LoopbackAllowed(NetMode mode);
// Loopback, private (RFC 1918), link-local (IPv4 and IPv6) and unique-local (fc00::/7) addresses:
// this machine and the LAN, which a sandboxed Orca without the LAN can't reach.
bool LanAddress(std::string_view address);
bool LoopbackAddress(std::string_view address);
// Whether the direct links may offer or try a candidate at `address` (an IP address; anything else
// is left to libjuice).
bool AddressAllowed(NetMode mode, std::string_view address);

// Inside the app's sandbox: an AppContainer on Windows, or a launch that set ORCA_NET.
bool Jailed();

// Dolphin's own GameCube adapter code (libusb, UsbDk on Windows, IOKit on a Mac). Never in a
// session: the app's controller helper owns the adapter and sends its ports in the snapshots, and
// a session's ports are GameCube controllers, never the Wii U adapter (Profile.cpp).
bool AdapterAllowed(bool session);

enum class Os
{
  Windows,
  MacOS,
  Other,
};
Os ThisOs();
// Dolphin's own keyboard device. On a Mac it reads the global key state (CGEventSourceKeyState), so
// keys typed into the app's page would reach it: in a session on the stdio bridge the page sends
// the keyboard as a pad instead, and Orca reads none.
bool OsKeyboardAllowed(Os os, bool session, bool stdio);

// Error codes (orca error <code> <sentence>), with their sentences for the player.
inline constexpr const char* EMBED_ERROR = "sandbox_embed";
inline constexpr const char* EMBED_SENTENCE =
    "Orca couldn't open in the YouGame window. Try again.";
inline constexpr const char* DISC_ERROR = "sandbox_disc";
inline constexpr const char* DISC_SENTENCE =
    "Orca can't read this disc where it is. Copy it into a folder of yours, such as Documents, "
    "and choose the copy.";
inline constexpr const char* DENIED_ERROR = "sandbox_denied";
inline constexpr const char* DENIED_SENTENCE =
    "Orca isn't allowed into its own folder, so it didn't start. Reset Orca's files from YouGame's "
    "Help menu, then press Play again.";

// Opening `path` to read fails for lack of permission (ERROR_ACCESS_DENIED, EACCES or EPERM), the
// way a sandbox refuses a file it wasn't given.
bool ReadDenied(const std::string& path);
// Creating a file in `dir` fails for lack of permission. Leaves nothing behind.
bool WriteDenied(const std::string& dir);
}  // namespace Orca::Sandbox
