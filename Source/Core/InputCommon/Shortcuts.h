// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace InputCommon::Shortcuts
{
enum Modifiers : unsigned
{
  Shift = 1,
  Ctrl = 2,
  Alt = 4
};
struct Combo
{
  unsigned modifiers = 0;
  std::string key;
  bool operator==(const Combo&) const = default;
};
struct Bindings
{
  std::optional<Combo> menu;
  std::optional<Combo> chat;
  bool operator==(const Bindings&) const = default;
};
// Canonical physical key identities (US positions), independent of the active layout.
std::string NormalizeKey(std::string_view key);
std::string ScanCodeKey(unsigned scan);
std::string EvdevKey(unsigned code, std::string_view name);
std::string XkbPositionKey(std::string_view position);
// Strict, whole-line parse. An empty binding set is a successful bare command.
std::optional<Bindings> ParseLine(std::string_view line);

// Driven by own-window events, or existing controller snapshots in embedded mode.
// Controller mappings consult GetWindowKeyState.
// Suppression latches until each chord key is released, even if a modifier drops first.
class Manager
{
public:
  using Output = std::function<void(std::string_view)>;
  void SetOutput(Output output);
  bool Configure(std::string_view line);
  bool Configured() const;
  bool KeyEvent(std::string_view key, unsigned modifiers, bool down, bool repeat = false);
  void SetFocus(bool focused);
  // Embedded windows retain parent focus. Use the existing controller backend snapshots,
  // aggregated across all devices before any mapping is evaluated.
  void SetPolling(bool enabled);
  bool UsesPolling() const;
  void SynchronizeHeldKeys(const std::set<std::string, std::less<>>& held);
  void PollKeys(const std::set<std::string, std::less<>>& held);
  void FocusLost();
  bool IsSuppressed(std::string_view key) const;
  // Configured base keys use own-window events in standalone mode, avoiding a
  // hardware poll seeing Enter before the window has classified its chord.
  std::optional<bool> GetWindowKeyState(std::string_view key) const;

private:
  bool ProcessKey(std::string_view key, unsigned modifiers, bool down, bool repeat);
  void End(bool chat, bool cancel);
  void SwallowHeldBindings();
  void Emit(bool chat, std::string_view edge);
  mutable std::mutex m_mutex;
  Bindings m_bindings;
  Output m_output;
  std::set<std::string, std::less<>> m_suppressed;
  std::set<std::string, std::less<>> m_pressed;
  unsigned m_modifiers = 0;
  bool m_poll_rearm = false;
  bool m_polling = false;
  bool m_configured = false;
  bool m_focused = true;
  bool m_menu_down = false;
  bool m_chat_down = false;
};
Manager& Get();
}  // namespace InputCommon::Shortcuts
