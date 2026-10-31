// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Orca/BootPhase.h"
#include "Core/Orca/Sandbox.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/OverlayKey.h"
#include "Core/Orca/UX/YgOrb.h"
#include "Core/System.h"

#include <windows.h>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <dwmapi.h>
#include <set>
#include <string>
#include <string_view>
#include <windowsx.h>

#include "InputCommon/Shortcuts.h"
#include "VideoCommon/Present.h"
#include "resource.h"

namespace
{
// The desktop launcher can retry a late SetParent.
constexpr auto ADOPTION_TIMEOUT = std::chrono::seconds(20);

bool IsAdoptedBy(HWND self, HWND parent)
{
  // Match the launcher's parent check; GetParent can return a popup's owner instead.
  return GetAncestor(self, GA_PARENT) == parent;
}

// Prints a line from Orca/UX/OverlayKey.h, if any.
void OutKey(std::string_view line)
{
  if (!line.empty())
    Embed::Out(std::string(line));
}

class PlatformWin32 final : public Platform
{
public:
  ~PlatformWin32() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

protected:
  std::optional<Embed::WindowBounds> EmbedGetBounds() const override;
  void EmbedSetRect(const Embed::Rect& rect) override;
  void EmbedSetVisible(bool visible) override;
  void EmbedSetFocus(bool focus) override;

private:
  static constexpr TCHAR WINDOW_CLASS_NAME[] = _T("Orca");

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  static bool RegisterRenderWindowClass(bool embedded);
  bool CreateRenderWindow();
  bool CreateAdoptedWindow();
  void UpdateWindowPosition();
  void ProcessEvents();
  void UpdateEmbedFocus();
  void KeepAdoptedPlaced();

  HWND m_hwnd{};
  // --embed-adopted: the app's launcher made the window a child of the app's (Embed.h).
  bool m_adopted = false;
  int m_misplaced_notes = 0;
  // Embed mode: the app's "show" / "hide". A rect while hidden must not show the window again.
  bool m_embed_visible = true;
  // The app's "focus" / "blur": whether the game gets input. On by default, as on macOS.
  bool m_embed_focus = true;
  // Modifiers come only from this window's messages; never query system keyboard state.
  bool m_shift_down = false;
  bool m_control_down = false;
  bool m_alt_down = false;
  std::set<WPARAM> m_modifier_keys;

  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
};

PlatformWin32::~PlatformWin32()
{
  if (m_hwnd)
    DestroyWindow(m_hwnd);
}

bool PlatformWin32::RegisterRenderWindowClass(bool embedded)
{
  WNDCLASSEX wc = {};
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = 0;
  wc.lpfnWndProc = WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hIcon = LoadIcon(wc.hInstance, IDI_ICON1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // When embedded, show black until the first frame instead of a white box.
  wc.hbrBackground = embedded ? static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)) :
                                reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszMenuName = nullptr;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  wc.hIconSm = LoadIcon(wc.hInstance, IDI_ICON1);

  if (!RegisterClassEx(&wc))
  {
    if (embedded)
      fprintf(stderr, "Orca: window registration failed (%lu)\n", GetLastError());
    else
      MessageBox(nullptr, _T("Window registration failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  return true;
}

// --embed-adopted (the sandbox): a sandboxed process may not create a window under the app's, so
// the window starts top-level and hidden, and yougame-sandbox.exe (outside the sandbox) makes it a
// child of the app's window, finding it by its class and caption, both "Orca", which never change.
// Messages are answered while waiting; after ADOPTION_TIMEOUT, Orca gives up (sandbox_embed).
bool PlatformWin32::CreateAdoptedWindow()
{
  const HWND parent = reinterpret_cast<HWND>(static_cast<uintptr_t>(m_embed.parent));
  const Embed::Rect& r = m_embed.rect;
  // A tool window, so it never shows on the taskbar while it waits. No WM_PARENTNOTIFY once
  // adopted: each one is a synchronous call into the app's UI thread. Made at the app's rect: the
  // launcher's SetParent keeps a window's coordinates as they are and reads them in the parent's
  // client area, so the window lands at its rect even when SetParent's own move comes after ours.
  m_hwnd = CreateWindowEx(WS_EX_NOPARENTNOTIFY | WS_EX_TOOLWINDOW, WINDOW_CLASS_NAME, _T("Orca"),
                          WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, r.x, r.y, r.w, r.h, nullptr,
                          nullptr, GetModuleHandle(nullptr), this);
  if (!m_hwnd)
  {
    fprintf(stderr, "Orca: CreateWindowEx for adoption failed (%lu)\n", GetLastError());
    return false;
  }
  Orca::BootPhase::Mark(Orca::BootPhase::Phase::WindowCreated);
  const auto deadline = std::chrono::steady_clock::now() + ADOPTION_TIMEOUT;
  while (!IsAdoptedBy(m_hwnd, parent) && std::chrono::steady_clock::now() < deadline)
  {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    Sleep(5);
  }
  if (!IsAdoptedBy(m_hwnd, parent))
  {
    fprintf(stderr, "Orca: nothing adopted the window into the app's in 10 s\n");
    Orca::Status::Error(Orca::Sandbox::EMBED_ERROR, Orca::Sandbox::EMBED_SENTENCE);
    return false;
  }
  fprintf(stderr, "Orca: adopted into the app's window\n");
  m_adopted = true;
  Orca::BootPhase::Mark(Orca::BootPhase::Phase::Window);
  // SetParent does not change window styles; make the adopted window a child here too.
  const LONG_PTR style = GetWindowLongPtr(m_hwnd, GWL_STYLE);
  SetWindowLongPtr(m_hwnd, GWL_STYLE,
                   (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD | WS_CLIPSIBLINGS |
                       WS_CLIPCHILDREN);
  SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  // Placed only now: a window moved before the launcher's SetParent stays where it was on screen.
  m_embed_visible = !m_embed.too_small;  // Hidden until a big enough rect (Embed.h).
  SetWindowPos(m_hwnd, HWND_TOP, r.x, r.y, r.w, r.h,
               SWP_NOACTIVATE | (m_embed_visible ? SWP_SHOWWINDOW : 0));
  // GA_PARENT can say "adopted" while the launcher's SetParent is still on its way out: once more,
  // a moment later, so the window is where it belongs for the rest of the boot too (MainLoop keeps
  // it there after that).
  const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (std::chrono::steady_clock::now() < settle)
  {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    Sleep(5);
  }
  KeepAdoptedPlaced();
  UpdateEmbedFocus();
  return true;
}

bool PlatformWin32::CreateRenderWindow()
{
  if (IsEmbedded() && m_embed.adopted)
    return CreateAdoptedWindow();
  if (IsEmbedded())
  {
    // A child window of the app's window, placed at the player box in the parent's client pixels,
    // so it moves and clips with the app.
    const HWND parent = reinterpret_cast<HWND>(static_cast<uintptr_t>(m_embed.parent));
    const Embed::Rect& r = m_embed.rect;
    // No WM_PARENTNOTIFY: each one is a synchronous call into the app's UI thread.
    m_hwnd = CreateWindowEx(WS_EX_NOPARENTNOTIFY, WINDOW_CLASS_NAME, _T("Orca"),
                            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, r.x, r.y, r.w, r.h,
                            parent, nullptr, GetModuleHandle(nullptr), this);
    if (!m_hwnd)
    {
      fprintf(stderr, "Orca: CreateWindowEx under the app's window failed (%lu)\n", GetLastError());
      EmbedFallback(!parent ? "no-parent" : !IsWindow(parent) ? "parent-gone" : "setparent");
      return CreateRenderWindow();
    }
    m_embed_visible = !m_embed.too_small;  // Hidden until a big enough rect (Embed.h).
    if (m_embed_visible)
    {
      ShowWindow(m_hwnd, SW_SHOWNA);
      if (IsWindowVisible(m_hwnd))
        EmbedSucceeded();
    }
    UpdateEmbedFocus();
    return true;
  }

  // Standalone: the caption is the name players see (the embedded windows above keep "Orca").
  m_hwnd = CreateWindowEx(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, _T("YG Dolphin"),
                          WS_OVERLAPPEDWINDOW, m_window_x < 0 ? CW_USEDEFAULT : m_window_x,
                          m_window_y < 0 ? CW_USEDEFAULT : m_window_y, m_window_width,
                          m_window_height, nullptr, nullptr, GetModuleHandle(nullptr), this);
  if (!m_hwnd)
  {
    MessageBox(nullptr, _T("CreateWindowEx failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  ShowWindow(m_hwnd, SW_SHOW);
  UpdateWindow(m_hwnd);
  return true;
}

bool PlatformWin32::Init()
{
  // The app (Electron) sends rects in physical pixels, so be per-monitor DPI aware too, or Windows
  // would scale the child window and its swap chain.
  if (IsEmbedded())
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  if (!RegisterRenderWindowClass(IsEmbedded()) || !CreateRenderWindow())
    return false;
  InputCommon::Shortcuts::Get().SetPolling(IsEmbedded());
  Orca::Status::SetShortcutsAvailable(true);
  if (IsEmbedded())
    return true;

  // TODO: Enter fullscreen if enabled.
  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    ProcessEvents();
  }

  if (Config::Get(Config::MAIN_DISABLE_SCREENSAVER))
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

  UpdateWindowPosition();
  return true;
}

void PlatformWin32::SetTitle(const std::string& string)
{
  // An adopted window keeps the caption the launcher knows it by.
  if (m_embed.adopted)
    return;
  SetWindowTextW(m_hwnd, UTF8ToWString(string).c_str());
}

void PlatformWin32::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    UpdateWindowPosition();
    if (IsEmbedded())
      UpdateEmbedFocus();
    if (m_adopted)
      KeepAdoptedPlaced();

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformWin32::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Windows;
  wsi.render_window = reinterpret_cast<void*>(m_hwnd);
  wsi.render_surface = reinterpret_cast<void*>(m_hwnd);
  return wsi;
}

void PlatformWin32::UpdateWindowPosition()
{
  if (m_window_fullscreen || IsEmbedded())
    return;

  RECT rc = {};
  if (!GetWindowRect(m_hwnd, &rc))
    return;

  m_window_x = rc.left;
  m_window_y = rc.top;
  m_window_width = rc.right - rc.left;
  m_window_height = rc.bottom - rc.top;
}

// An adopted window can end up somewhere else in the app's window (Windows keeps a window's screen
// position across SetParent): put it back at the app's rect. A couple of pixels of display-scaling
// rounding are left alone.
void PlatformWin32::KeepAdoptedPlaced()
{
  const HWND parent = GetAncestor(m_hwnd, GA_PARENT);
  RECT rc{};
  if (!m_embed_visible || !parent || !GetWindowRect(m_hwnd, &rc))
    return;
  MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rc), 2);
  const Embed::Rect& r = m_embed.rect;
  const auto off = [](long a, int b) { return std::abs(a - static_cast<long>(b)) > 2; };
  if (!off(rc.left, r.x) && !off(rc.top, r.y) && !off(rc.right - rc.left, r.w) &&
      !off(rc.bottom - rc.top, r.h))
  {
    return;
  }
  if (m_misplaced_notes++ < 3)
  {
    fprintf(stderr, "Orca: the window was at %ld %ld %ld %ld, put back at %d %d %d %d\n", rc.left,
            rc.top, rc.right - rc.left, rc.bottom - rc.top, r.x, r.y, r.w, r.h);
  }
  SetWindowPos(m_hwnd, HWND_TOP, r.x, r.y, r.w, r.h, SWP_NOACTIVATE);
}

std::optional<Embed::WindowBounds> PlatformWin32::EmbedGetBounds() const
{
  RECT rect{};
  if (!GetWindowRect(m_hwnd, &rect))
    return std::nullopt;
  return Embed::WindowBounds{static_cast<double>(rect.left), static_cast<double>(rect.top),
                             static_cast<double>(rect.right - rect.left),
                             static_cast<double>(rect.bottom - rect.top),
                             GetDpiForWindow(m_hwnd) / 96.0};
}

void PlatformWin32::EmbedSetRect(const Embed::Rect& r)
{
  if (!IsEmbedded())
    return;
  if (SetWindowPos(m_hwnd, HWND_TOP, r.x, r.y, r.w, r.h,
                   SWP_NOACTIVATE | (m_embed_visible ? SWP_SHOWWINDOW : SWP_NOREDRAW)) &&
      IsWindowVisible(m_hwnd))
  {
    EmbedSucceeded();
  }
}

void PlatformWin32::EmbedSetVisible(bool visible)
{
  if (!IsEmbedded())
    return;
  // A hidden window gets no key-up: a Shift+Tab press still down ends here.
  if (!visible)
    OutKey(Orca::UX::OverlayKey::Keys().Cancel());
  m_embed_visible = visible;
  ShowWindow(m_hwnd, visible ? SW_SHOWNA : SW_HIDE);
  if (IsWindowVisible(m_hwnd))
    EmbedSucceeded();
  UpdateEmbedFocus();
}

void PlatformWin32::EmbedSetFocus(bool focus)
{
  if (!IsEmbedded())
    return;
  m_embed_focus = focus;
  UpdateEmbedFocus();
}

// When embedded, the app keeps keyboard focus and this window never takes it. Input counts while
// the app's window is in the foreground, the game is shown, and the app's last word was "focus".
void PlatformWin32::UpdateEmbedFocus()
{
  if (!EmbedResultDecided() && IsWindowVisible(m_hwnd))
    EmbedSucceeded();
  const HWND root = GetAncestor(m_hwnd, GA_ROOT);
  m_window_focus = m_embed_visible && m_embed_focus && root && GetForegroundWindow() == root;
  InputCommon::Shortcuts::Get().SetFocus(m_window_focus);
  if (!m_window_focus)
    m_modifier_keys.clear();
}

void PlatformWin32::ProcessEvents()
{
  POINT point;
  const bool down = (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
  if (GetCursorPos(&point) && WindowFromPoint(point) == m_hwnd && ScreenToClient(m_hwnd, &point))
    Orca::UX::YgOrb::UpdateControlsPointer(static_cast<float>(point.x), static_cast<float>(point.y),
                                           down);
  else
    Orca::UX::YgOrb::UpdateControlsPointer(-1, -1, down);
  MSG msg;
  while (PeekMessage(&msg, m_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  // Child visibility alone does not reflect a minimized app window. Publish the surface's
  // state as on macOS/X11, so a broadcast can omit window work while its XFB keeps flowing.
  const HWND root = GetAncestor(m_hwnd, GA_ROOT);
  RECT client{};
  const bool visible = IsWindowVisible(m_hwnd) && root && !IsIconic(root) &&
                       GetClientRect(m_hwnd, &client) && client.right > client.left &&
                       client.bottom > client.top;
  VideoCommon::Presenter::SetSurfaceVisible(visible);
  InputCommon::Shortcuts::Get().SetFocus(IsWindowFocused() && visible);
}

LRESULT PlatformWin32::WndProc(const HWND hwnd, const UINT msg, const WPARAM wParam,
                               const LPARAM lParam)
{
  PlatformWin32* platform = reinterpret_cast<PlatformWin32*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  if (platform &&
      (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP))
  {
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    WPARAM side = wParam;
    if (side == VK_SHIFT)
      side = ((lParam >> 16) & 0xff) == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    else if (side == VK_CONTROL)
      side = (lParam & (1 << 24)) ? VK_RCONTROL : VK_LCONTROL;
    else if (side == VK_MENU)
      side = (lParam & (1 << 24)) ? VK_RMENU : VK_LMENU;
    const bool modifier = side == VK_LSHIFT || side == VK_RSHIFT || side == VK_LCONTROL ||
                          side == VK_RCONTROL || side == VK_LMENU || side == VK_RMENU ||
                          side == VK_LWIN || side == VK_RWIN;
    if (modifier)
    {
      if (down)
        platform->m_modifier_keys.insert(side);
      else
        platform->m_modifier_keys.erase(side);
    }
    const auto held = [&](WPARAM left, WPARAM right) {
      return platform->m_modifier_keys.contains(left) || platform->m_modifier_keys.contains(right);
    };
    platform->m_shift_down = held(VK_LSHIFT, VK_RSHIFT);
    platform->m_control_down = held(VK_LCONTROL, VK_RCONTROL);
    platform->m_alt_down = held(VK_LMENU, VK_RMENU);
    auto& shortcuts = InputCommon::Shortcuts::Get();
    shortcuts.SetFocus(platform->IsWindowFocused());
    const unsigned mods = (platform->m_shift_down ? InputCommon::Shortcuts::Shift : 0u) |
                          (platform->m_control_down ? InputCommon::Shortcuts::Ctrl : 0u) |
                          (platform->m_alt_down ? InputCommon::Shortcuts::Alt : 0u) |
                          (held(VK_LWIN, VK_RWIN) ? 8u : 0u);
    const auto key = InputCommon::Shortcuts::ScanCodeKey(
        static_cast<unsigned>((lParam >> 16) & 0xff) | ((lParam & (1 << 24)) ? 0x80u : 0u));
    const bool aggregate_down = key == "Shift" ? platform->m_shift_down :
                                key == "Ctrl"  ? platform->m_control_down :
                                key == "Alt"   ? platform->m_alt_down :
                                                 down;
    if (shortcuts.KeyEvent(key, mods, aggregate_down, down && (lParam & (1 << 30))))
      return 0;
  }
  switch (msg)
  {
  case WM_NCCREATE:
  {
    platform = static_cast<PlatformWin32*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(platform));
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  case WM_CREATE:
  {
    if (hwnd)
    {
      // Remove rounded corners from the render window on Windows 11
      constexpr DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DONOTROUND;
      DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_preference,
                            sizeof(corner_preference));
    }
  }
  break;

  case WM_SIZE:
  {
    if (g_presenter)
      g_presenter->ResizeSurface();
  }
  break;

  case WM_KEYDOWN:
    // The embedded app owns keyboard focus and relays feedback / host-menu on stdin.
    if (!platform->IsEmbedded() && !(lParam & (1 << 30)))
    {
      namespace Orb = Orca::UX::YgOrb;
      if (wParam == VK_F8 && Orb::HandleHostKey(Orb::HostKey::Feedback))
        return 0;
    }
    // Keep the same press/release semantics for the page, with or without a native window.
    if (!InputCommon::Shortcuts::Get().Configured() &&
        (platform->IsEmbedded() || Orca::UX::YgOrb::HostEnabled()) && !(lParam & (1 << 30)) &&
        wParam == VK_TAB && platform->m_shift_down && !platform->m_control_down &&
        !platform->m_alt_down)
    {
      Orca::UX::OverlayKey::Emit(
          Orca::UX::OverlayKey::Keys().Down(Orca::Status::Cap(Orca::Status::TALK_CAP)),
          platform->IsEmbedded() ? OutKey : nullptr);
      return 0;
    }
    if (platform->IsEmbedded())
    {
      // Ignore auto-repeat, so a held F11 doesn't toggle full screen over and over.
      if (lParam & (1 << 30))
        break;
      // Forward the keys the page cares about, since it can't see them here. Shift+Tab is
      // YouGame's overlay key: "orca orb" opens the overlay, the same as clicking the YouGame
      // button; with the app's "talk" cap its press and release go instead, and holding it talks
      // (Orca/UX/OverlayKey.h).
      if (wParam == VK_ESCAPE)
        Embed::Out("key escape");
      else if (wParam == VK_F11)
        Embed::Out("key fullscreen");
    }
    else if (wParam == VK_ESCAPE)
    {
      platform->RequestShutdown();
    }
    break;

  // The end of a Shift+Tab press: Tab or Shift let go.
  case WM_KEYUP:
    if (platform && (wParam == VK_TAB || wParam == VK_SHIFT))
      Orca::UX::OverlayKey::Emit(Orca::UX::OverlayKey::Keys().Up(),
                                 platform->IsEmbedded() ? OutKey : nullptr);
    return DefWindowProc(hwnd, msg, wParam, lParam);

  // The keyboard went elsewhere, so a Shift+Tab press still down never sees its key-up here.
  case WM_SETFOCUS:
    if (platform)
    {
      for (const WPARAM side :
           {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN})
      {
        if (GetKeyState(static_cast<int>(side)) & 0x8000)
          platform->m_modifier_keys.insert(side);
        else
          platform->m_modifier_keys.erase(side);
      }
      std::set<std::string, std::less<>> held_keys;
      for (unsigned scan = 1; scan < 256; ++scan)
      {
        const auto key = InputCommon::Shortcuts::ScanCodeKey(scan);
        const UINT vk =
            MapVirtualKeyW((scan & 0x80) ? (0xe000u | (scan & 0x7f)) : scan, MAPVK_VSC_TO_VK_EX);
        if (!key.empty() && vk && (GetKeyState(static_cast<int>(vk)) & 0x8000))
          held_keys.insert(key);
      }
      InputCommon::Shortcuts::Get().SynchronizeHeldKeys(held_keys);
      platform->m_window_focus = true;
      InputCommon::Shortcuts::Get().SetFocus(true);
      if (platform->IsEmbedded())
        if (const HWND root = GetAncestor(hwnd, GA_ROOT); root && root != hwnd)
          SetFocus(root);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);

  case WM_KILLFOCUS:
    InputCommon::Shortcuts::Get().FocusLost();
    if (platform)
    {
      platform->m_window_focus = false;
      platform->m_modifier_keys.clear();
      platform->m_shift_down = false;
      platform->m_control_down = false;
      platform->m_alt_down = false;
    }
    if (platform)
      Orca::UX::OverlayKey::Emit(Orca::UX::OverlayKey::Keys().Cancel(),
                                 platform->IsEmbedded() ? OutKey : nullptr);
    return DefWindowProc(hwnd, msg, wParam, lParam);

  // When embedded, Alt and F10 are game keys, so don't let them open the app's window menu.
  // Alt+F4 still closes the app.
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP:
  case WM_SYSCHAR:
    if (platform && platform->IsEmbedded() && wParam != VK_F4)
      return 0;
    return DefWindowProc(hwnd, msg, wParam, lParam);

  // When embedded, the app's window was destroyed along with ours, so stop.
  case WM_DESTROY:
    if (platform && platform->IsEmbedded())
      platform->Stop();
    return DefWindowProc(hwnd, msg, wParam, lParam);

  // When embedded, clicks on the game land here, not in the app's page. Report them; the app
  // decides whether to give the game focus. A left click on the YouGame button Orca draws
  // (UX/YgOrb.h) is reported as "orca orb". No CS_DBLCLKS, so a double click arrives as two
  // presses.
  case WM_LBUTTONUP:
    Orca::UX::YgOrb::UpdateControlsPointer(static_cast<float>(GET_X_LPARAM(lParam)),
                                           static_cast<float>(GET_Y_LPARAM(lParam)), false);
    return DefWindowProc(hwnd, msg, wParam, lParam);

  case WM_LBUTTONDOWN:
  case WM_RBUTTONDOWN:
  case WM_MBUTTONDOWN:
    if (msg == WM_LBUTTONDOWN)
      Orca::UX::YgOrb::UpdateControlsPointer(static_cast<float>(GET_X_LPARAM(lParam)),
                                             static_cast<float>(GET_Y_LPARAM(lParam)), true);
    if (msg == WM_LBUTTONDOWN &&
        Orca::UX::YgOrb::ClickControls(static_cast<float>(GET_X_LPARAM(lParam)),
                                       static_cast<float>(GET_Y_LPARAM(lParam)),
                                       platform && platform->IsEmbedded() ? OutKey : nullptr))
      return 0;
    if (platform && platform->IsEmbedded())
    {
      const bool orb = msg == WM_LBUTTONDOWN &&
                       Orca::UX::YgOrb::Current().Hit(static_cast<float>(GET_X_LPARAM(lParam)),
                                                      static_cast<float>(GET_Y_LPARAM(lParam)));
      Embed::Out(orb ? "orca orb" : "orca click");
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);

  case WM_CLOSE:
    // When embedded, only the app ends the game ("quit" or a closed stdin).
    if (!platform->IsEmbedded())
      platform->RequestShutdown();
    break;

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  return 0;
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWin32Platform()
{
  return std::make_unique<PlatformWin32>();
}
