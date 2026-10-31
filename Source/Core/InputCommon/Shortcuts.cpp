// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later
#include "InputCommon/Shortcuts.h"

#include <array>

namespace InputCommon::Shortcuts
{
namespace
{
bool IsKey(std::string_view key)
{
  if (key == "Tab" || key == "Enter" || key == "Backquote")
    return true;
  if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z')
    return true;
  constexpr std::array function_keys = {"F1", "F2", "F3", "F4",  "F5",  "F6",
                                        "F7", "F8", "F9", "F10", "F11", "F12"};
  for (const auto* function_key : function_keys)
    if (key == function_key)
      return true;
  return false;
}
std::optional<Combo> ParseCombo(std::string_view text)
{
  Combo combo;
  while (true)
  {
    const auto plus = text.find('+');
    if (plus == std::string_view::npos)
      break;
    const auto modifier = text.substr(0, plus);
    const unsigned bit = modifier == "Shift" ? Shift :
                         modifier == "Ctrl"  ? Ctrl :
                         modifier == "Alt"   ? Alt :
                                               0;
    if (!bit || (combo.modifiers & bit))
      return std::nullopt;
    combo.modifiers |= bit;
    text.remove_prefix(plus + 1);
  }
  if (!IsKey(text))
    return std::nullopt;
  combo.key = text;
  return combo;
}
}  // namespace
std::string NormalizeKey(std::string_view key)
{
  if (key == "Return" || key == "RETURN" || key == "ENTER")
    return "Enter";
  if (key == "TAB")
    return "Tab";
  if (key == "Tilde" || key == "GRAVE" || key == "grave")
    return "Backquote";
  if (key == "LSHIFT" || key == "RSHIFT" || key == "Shift_L" || key == "Shift_R" ||
      key == "Right Shift" || key == "Left Shift" || key == "LEFTSHIFT" || key == "RIGHTSHIFT")
    return "Shift";
  if (key == "LCONTROL" || key == "RCONTROL" || key == "Control_L" || key == "Control_R" ||
      key == "Right Ctrl" || key == "Left Control" || key == "Right Control" || key == "LEFTCTRL" ||
      key == "RIGHTCTRL")
    return "Ctrl";
  if (key == "LMENU" || key == "RMENU" || key == "Alt_L" || key == "Alt_R" || key == "Right Alt" ||
      key == "Left Alt" || key == "LEFTALT" || key == "RIGHTALT")
    return "Alt";
  if (key == "LWIN" || key == "RWIN" || key == "Super_L" || key == "Super_R" || key == "LEFTMETA" ||
      key == "RIGHTMETA" || key == "Command" || key == "Left Command" || key == "Right Command")
    return "Meta";
  std::string result(key);
  if (result.size() == 1 && result[0] >= 'a' && result[0] <= 'z')
    result[0] -= 'a' - 'A';
  return result;
}
std::string EvdevKey(unsigned code, std::string_view name)
{
  // Linux EV_KEY values below BTN_MISC (0x100) describe keyboard keys.
  return code < 0x100 ? NormalizeKey(name) : std::string{};
}
std::string ScanCodeKey(unsigned scan)
{
  constexpr std::array<std::pair<unsigned, const char*>, 39> keys = {
      {{15, "Tab"},   {28, "Enter"}, {41, "Backquote"}, {42, "Shift"}, {54, "Shift"}, {29, "Ctrl"},
       {157, "Ctrl"}, {56, "Alt"},   {184, "Alt"},      {219, "Meta"}, {220, "Meta"}, {16, "Q"},
       {17, "W"},     {18, "E"},     {19, "R"},         {20, "T"},     {21, "Y"},     {22, "U"},
       {23, "I"},     {24, "O"},     {25, "P"},         {30, "A"},     {31, "S"},     {32, "D"},
       {33, "F"},     {34, "G"},     {35, "H"},         {36, "J"},     {37, "K"},     {38, "L"},
       {44, "Z"},     {45, "X"},     {46, "C"},         {47, "V"},     {48, "B"},     {49, "N"},
       {50, "M"},     {87, "F11"},   {88, "F12"}}};
  if (scan >= 59 && scan <= 68)
    return "F" + std::to_string(scan - 58);
  for (const auto& [code, name] : keys)
    if (scan == code)
      return name;
  return {};
}
std::string XkbPositionKey(std::string_view position)
{
  for (const auto& [row, letters] : std::array<std::pair<std::string_view, std::string_view>, 3>{
           {{"AD", "QWERTYUIOP"}, {"AC", "ASDFGHJKL"}, {"AB", "ZXCVBNM"}}})
    for (size_t i = 0; i < letters.size(); ++i)
      if (position == std::string(row) + (i < 9 ? "0" : "") + std::to_string(i + 1))
        return std::string(1, letters[i]);
  if (position.starts_with("FK") && position.size() == 4)
  {
    unsigned n = (position[2] - '0') * 10 + position[3] - '0';
    if (n >= 1 && n <= 12)
      return "F" + std::to_string(n);
  }
  for (const auto& [pos, key] :
       std::array<std::pair<const char*, const char*>, 11>{{{"TAB", "Tab"},
                                                            {"RTRN", "Enter"},
                                                            {"TLDE", "Backquote"},
                                                            {"LFSH", "Shift"},
                                                            {"RTSH", "Shift"},
                                                            {"LCTL", "Ctrl"},
                                                            {"RCTL", "Ctrl"},
                                                            {"LALT", "Alt"},
                                                            {"RALT", "Alt"},
                                                            {"LWIN", "Meta"},
                                                            {"RWIN", "Meta"}}})
    if (position == pos)
      return key;
  return {};
}
std::optional<Bindings> ParseLine(std::string_view line)
{
  constexpr std::string_view prefix = "embed shortcuts";
  if (!line.starts_with(prefix))
    return std::nullopt;
  line.remove_prefix(prefix.size());
  Bindings result;
  while (!line.empty())
  {
    if (line.front() != ' ')
      return std::nullopt;
    line.remove_prefix(1);
    const auto space = line.find(' ');
    const auto token = line.substr(0, space);
    auto* destination = token.starts_with("menu=") ? &result.menu :
                        token.starts_with("chat=") ? &result.chat :
                                                     nullptr;
    if (!destination || *destination)
      return std::nullopt;
    const auto combo = ParseCombo(token.substr(5));
    if (!combo)
      return std::nullopt;
    *destination = *combo;
    if (space == std::string_view::npos)
      break;
    line.remove_prefix(space);
  }
  return result;
}
void Manager::SetOutput(Output output)
{
  std::lock_guard lock(m_mutex);
  m_output = std::move(output);
}
void Manager::Emit(bool chat, std::string_view edge)
{
  if (m_output)
  {
    const std::string line =
        std::string("yg {\"") + (chat ? "chat" : "overlay") + "\":\"" + std::string(edge) + "\"}";
    m_output(line);
  }
}
void Manager::End(bool chat, bool cancel)
{
  auto& down = chat ? m_chat_down : m_menu_down;
  if (down)
  {
    down = false;
    Emit(chat, chat && cancel ? "cancel" : "up");
  }
}
void Manager::SwallowHeldBindings()
{
  // Suppression is independent of relay presses: a base key held before its modifiers,
  // or a modifier released and pressed again after cancel, must never reach the game.
  for (const auto& combo : {m_bindings.menu, m_bindings.chat})
  {
    if (combo && m_pressed.contains(combo->key) && m_modifiers == combo->modifiers)
    {
      m_suppressed.insert(combo->key);
      if (combo->modifiers & Shift)
        m_suppressed.insert("Shift");
      if (combo->modifiers & Ctrl)
        m_suppressed.insert("Ctrl");
      if (combo->modifiers & Alt)
        m_suppressed.insert("Alt");
    }
  }
}
bool Manager::Configure(std::string_view line)
{
  const auto bindings = ParseLine(line);
  if (!bindings)
    return false;
  std::lock_guard lock(m_mutex);
  if (m_bindings != *bindings)
  {
    End(false, true);
    End(true, true);
    m_bindings = *bindings;
    m_suppressed.clear();
    // Swallow a newly bound chord already held, without a synthetic press.
    SwallowHeldBindings();
  }
  m_configured = true;
  return true;
}
bool Manager::Configured() const
{
  std::lock_guard lock(m_mutex);
  return m_configured;
}
bool Manager::KeyEvent(std::string_view key, unsigned modifiers, bool down, bool repeat)
{
  std::lock_guard lock(m_mutex);
  if (m_polling)
    return m_focused && m_suppressed.contains(key);
  return ProcessKey(key, modifiers, down, repeat);
}
bool Manager::ProcessKey(std::string_view key, unsigned modifiers, bool down, bool repeat)
{
  if (!m_focused)
    return false;
  bool consumed = m_suppressed.contains(key);
  const bool fresh_press = down && !repeat && !key.empty() && !m_pressed.contains(key);
  m_modifiers = modifiers;
  // Unknown platform keys carry modifier updates but have no canonical key state.
  // Repeats may reveal a dropped modifier, but must never begin a new press.
  if (!key.empty() && !repeat)
  {
    if (down)
      m_pressed.insert(std::string(key));
    else
      m_pressed.erase(std::string(key));
  }
  for (const auto& [name, bit] : std::array<std::pair<const char*, unsigned>, 3>{
           {{"Shift", Shift}, {"Ctrl", Ctrl}, {"Alt", Alt}}})
  {
    if (modifiers & bit)
      m_pressed.insert(name);
    else
      m_pressed.erase(name);
  }
  SwallowHeldBindings();
  consumed = consumed || m_suppressed.contains(key);
  // A base-key release is a normal up; losing any required modifier drops the hold.
  const auto update = [&](bool chat, const std::optional<Combo>& combo) {
    if (!combo)
      return;
    auto& held = chat ? m_chat_down : m_menu_down;
    if (held && !down && !repeat && key == combo->key)
      End(chat, false);
    else if (held && modifiers != combo->modifiers)
      End(chat, true);
    if (fresh_press && key == combo->key && modifiers == combo->modifiers && !held)
    {
      held = true;
      m_suppressed.insert(combo->key);
      if (modifiers & Shift)
        m_suppressed.insert("Shift");
      if (modifiers & Ctrl)
        m_suppressed.insert("Ctrl");
      if (modifiers & Alt)
        m_suppressed.insert("Alt");
      consumed = true;
      Emit(chat, "down");
    }
  };
  update(false, m_bindings.menu);
  update(true, m_bindings.chat);
  if (!down)
    m_suppressed.erase(std::string(key));
  for (const auto& [name, bit] : std::array<std::pair<const char*, unsigned>, 3>{
           {{"Shift", Shift}, {"Ctrl", Ctrl}, {"Alt", Alt}}})
  {
    const bool needed = (m_bindings.menu && m_suppressed.contains(m_bindings.menu->key) &&
                         (m_bindings.menu->modifiers & bit)) ||
                        (m_bindings.chat && m_suppressed.contains(m_bindings.chat->key) &&
                         (m_bindings.chat->modifiers & bit));
    if (!needed)
      m_suppressed.erase(name);
  }
  return consumed;
}
void Manager::SetFocus(bool focused)
{
  std::lock_guard lock(m_mutex);
  m_focused = focused;
  if (!focused)
  {
    End(false, true);
    End(true, true);
    m_suppressed.clear();
    m_pressed.clear();  // FocusIn snapshots reconstruct keys that still require release.
    m_modifiers = 0;
  }
}
void Manager::FocusLost()
{
  SetFocus(false);
}
bool Manager::IsSuppressed(std::string_view key) const
{
  std::lock_guard lock(m_mutex);
  return m_focused && m_suppressed.contains(key);
}
std::optional<bool> Manager::GetWindowKeyState(std::string_view key) const
{
  std::lock_guard lock(m_mutex);
  if (!m_configured)
    return std::nullopt;
  if (m_focused && m_suppressed.contains(key))
    return false;
  // Only bound base keys need event classification; every other input keeps its raw state.
  const bool bound = (m_bindings.menu && m_bindings.menu->key == key) ||
                     (m_bindings.chat && m_bindings.chat->key == key);
  if (!bound || m_polling)
    return std::nullopt;
  return m_focused && m_pressed.contains(key) && !m_suppressed.contains(key);
}
void Manager::SetPolling(bool enabled)
{
  std::lock_guard lock(m_mutex);
  m_polling = enabled;
}
bool Manager::UsesPolling() const
{
  std::lock_guard lock(m_mutex);
  return m_polling && m_configured;
}
void Manager::SynchronizeHeldKeys(const std::set<std::string, std::less<>>& held)
{
  std::lock_guard lock(m_mutex);
  m_pressed = held;
  m_modifiers = (held.contains("Shift") ? Shift : 0u) | (held.contains("Ctrl") ? Ctrl : 0u) |
                (held.contains("Alt") ? Alt : 0u) | (held.contains("Meta") ? 8u : 0u);
  SwallowHeldBindings();
}
void Manager::PollKeys(const std::set<std::string, std::less<>>& held)
{
  std::lock_guard lock(m_mutex);
  if (!m_polling || !m_configured)
    return;
  unsigned mods = (held.contains("Shift") ? Shift : 0u) | (held.contains("Ctrl") ? Ctrl : 0u) |
                  (held.contains("Alt") ? Alt : 0u) | (held.contains("Meta") ? 8u : 0u);
  if (!m_focused)
  {
    m_pressed = held;  // rearm snapshot, no relay while unfocused
    return;
  }
  const auto previous = m_pressed;
  auto changed = previous;
  changed.insert(held.begin(), held.end());
  const auto base_release = [&](const std::string& key) {
    return previous.contains(key) && !held.contains(key) && key != "Shift" && key != "Ctrl" &&
           key != "Alt" && key != "Meta";
  };
  // A snapshot can contain both base and modifier releases. End the base normally first,
  // rather than reporting a modifier cancellation for a fully released chord.
  for (const auto& key : changed)
    if (base_release(key))
      ProcessKey(key, mods, false, false);
  for (const auto& key : changed)
    if (held.contains(key) != previous.contains(key) && !base_release(key))
      ProcessKey(key, mods, held.contains(key), false);
  m_pressed = held;
  m_modifiers = mods;
  SwallowHeldBindings();
}
Manager& Get()
{
  static Manager manager;
  return manager;
}
}  // namespace InputCommon::Shortcuts
