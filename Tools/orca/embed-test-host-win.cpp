// embed-test-host-win: plays the YouGame desktop app's part for `DolphinNoGUI --embed` on Windows
// (ORCA.md "Embedding"), as Tools/orca/embed-test-host.m does on macOS. A top-level window with a
// menu bar (Electron's default), a player box drawn in it with a red frame just outside the box;
// Orca is started as a child of the window at the box and steered over its stdin.
//
// Build, from a Developer PowerShell:
//   cl /nologo /EHsc /std:c++20 /utf-8 /W4 Tools\orca\embed-test-host-win.cpp ^
//      /Fe:embed-test-host-win.exe user32.lib gdi32.lib gdiplus.lib
// Run:
//   embed-test-host-win.exe [--commands <file>] [--stderr <file>] [--adopt 0|1] <DolphinNoGUI.exe>
//       [orca args...]
// The host adds `--embed --parent <its HWND> --rect X Y W H` in front of the orca arguments; pass
// the rest (-u, -v D3D, -C "Dolphin.DSP.Backend=No Audio Output", -e <disc>). Set ORCA_SESSION=0 in
// the environment for solo play. `--adopt 1` plays the sandbox's way (ORCA.md "Adopted window"):
// Orca gets `--embed-adopted` too, and this host adopts its window as yougame-sandbox.exe does
// with --embed-keep-style (WS_POPUP becomes WS_CHILD, then SetParent); `--adopt 0` starts it with
// `--embed-adopted` and never adopts it (Orca must give up with sandbox_embed after 10 s).
//
// Commands, one per line, on this program's stdin or appended to the --commands file (polled):
//   send LINE        a raw line for Orca's stdin (rect, hide, show, focus, blur, pause, quit...)
//   size W H         the window's client size in DIPs (the box follows: a rect is sent)
//   move X Y         the window's top left, in screen pixels
//   front            bring this window to the front (needed before key, click and shot)
//   topmost 0|1      keep the window above others (for screenshots)
//   where            print the window, client and box rectangles in screen pixels
//   click [FX FY]    a left click at a point of the box, as fractions (default 0.5 0.5)
//   clickhost        a left click on the window outside the box (the page)
//   key NAME tap|down|up|hold N   keys by name (x, z, enter, escape, f11, f10, alt, f4, up...);
//                    hold N sends N key-downs (the first one plus N-1 auto-repeats), then the up
//   altf4            Alt+F4
//   shot PATH        a PNG of the window as it is on screen (the window must be visible)
//   wait MS          pause the command stream
//   close            close Orca's stdin (the app died)
//   closewin         destroy the window but keep Orca's stdin open
//   exit             quit this program (Orca's stdin closes with it)
// Every line printed is "<ms since start> <source>> <text>": "orca>" for Orca's stdout, "host>"
// for what the window saw (keys, menu activation, focus, close), "cmd>" for commands.
// key, click and shot refuse to run unless this window is the foreground window, so stray input
// never lands in another program.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr UINT WM_APP_COMMAND = WM_APP + 1;
constexpr int MENU_FILE_QUIT = 1;

const auto s_start = std::chrono::steady_clock::now();
std::mutex s_print_mutex;
HWND s_window;
HANDLE s_orca_process;
HANDLE s_to_orca = INVALID_HANDLE_VALUE;
std::atomic<long long> s_quit_sent_ms{-1};

long long Ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               s_start)
      .count();
}

void Print(const char* source, const char* format, ...)
{
  char text[2048];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  std::lock_guard lock(s_print_mutex);
  std::printf("%7lld %s> %s\n", Ms(), source, text);
  std::fflush(stdout);
}

std::wstring Widen(const std::string& s)
{
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string Narrow(const std::wstring& w)
{
  if (w.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string s(static_cast<size_t>(n), ' ');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                      nullptr);
  return s;
}

double Scale()
{
  return GetDpiForWindow(s_window) / 96.0;
}

// The player box in client pixels: a 20 DIP margin, a 40 DIP "header" above, 16:9.
RECT Box()
{
  RECT c{};
  GetClientRect(s_window, &c);
  const double s = Scale();
  const int margin = static_cast<int>(20 * s), header = static_cast<int>(40 * s);
  int w = c.right - 2 * margin;
  int h = w * 9 / 16;
  if (h > c.bottom - header - margin)
  {
    h = c.bottom - header - margin;
    w = h * 16 / 9;
  }
  return RECT{margin, header, margin + std::max(w, 1), header + std::max(h, 1)};
}

void SendToOrca(const std::string& line)
{
  if (s_to_orca == INVALID_HANDLE_VALUE)
  {
    Print("host", "not sent (Orca's stdin is closed): %s", line.c_str());
    return;
  }
  const std::string data = line + "\n";
  DWORD written = 0;
  if (!WriteFile(s_to_orca, data.data(), static_cast<DWORD>(data.size()), &written, nullptr))
    Print("host", "write to Orca failed (%lu): %s", GetLastError(), line.c_str());
  if (line == "quit")
    s_quit_sent_ms = Ms();
}

std::string RectArgs()
{
  const RECT b = Box();
  return std::to_string(b.left) + " " + std::to_string(b.top) + " " +
         std::to_string(b.right - b.left) + " " + std::to_string(b.bottom - b.top);
}

bool IsForeground(const char* what)
{
  if (GetForegroundWindow() == s_window)
    return true;
  Print("host", "%s refused: this window is not the foreground window", what);
  return false;
}

WORD VirtualKey(const std::string& name)
{
  static const struct
  {
    const char* name;
    int vk;
  } keys[] = {
      {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"f11", VK_F11},    {"f10", VK_F10},
      {"f4", VK_F4},         {"alt", VK_MENU},   {"enter", VK_RETURN}, {"return", VK_RETURN},
      {"space", VK_SPACE},   {"up", VK_UP},      {"down", VK_DOWN},  {"left", VK_LEFT},
      {"right", VK_RIGHT},   {"tab", VK_TAB},    {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL}};
  for (const auto& key : keys)
  {
    if (name == key.name)
      return static_cast<WORD>(key.vk);
  }
  if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
    return static_cast<WORD>(std::toupper(static_cast<unsigned char>(name[0])));
  return 0;
}

void SendKey(WORD vk, bool down)
{
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = vk;
  in.ki.wScan = static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
  in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
  if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)
    in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  SendInput(1, &in, sizeof(in));
}

void ClickScreen(POINT p)
{
  const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  const int x0 = GetSystemMetrics(SM_XVIRTUALSCREEN), y0 = GetSystemMetrics(SM_YVIRTUALSCREEN);
  INPUT in[3]{};
  for (INPUT& i : in)
  {
    i.type = INPUT_MOUSE;
    i.mi.dx = static_cast<LONG>((p.x - x0) * 65535.0 / (w - 1));
    i.mi.dy = static_cast<LONG>((p.y - y0) * 65535.0 / (h - 1));
    i.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  }
  in[0].mi.dwFlags |= MOUSEEVENTF_MOVE;
  in[1].mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
  in[2].mi.dwFlags |= MOUSEEVENTF_LEFTUP;
  SendInput(3, in, sizeof(INPUT));
}

bool EncoderClsid(const wchar_t* mime, CLSID* clsid)
{
  UINT count = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&count, &size);
  std::vector<BYTE> buffer(size);
  auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
  Gdiplus::GetImageEncoders(count, size, encoders);
  for (UINT i = 0; i < count; ++i)
  {
    if (wcscmp(encoders[i].MimeType, mime) == 0)
    {
      *clsid = encoders[i].Clsid;
      return true;
    }
  }
  return false;
}

// The window as it is on screen (Orca's swap chain included), from the screen's own pixels.
void Shot(const std::string& path)
{
  RECT r{};
  GetWindowRect(s_window, &r);
  const int w = r.right - r.left, h = r.bottom - r.top;
  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(memory, bitmap);
  BitBlt(memory, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
  SelectObject(memory, old);
  CLSID png{};
  bool ok = false;
  if (EncoderClsid(L"image/png", &png))
  {
    Gdiplus::Bitmap image(bitmap, nullptr);
    ok = image.Save(Widen(path).c_str(), &png, nullptr) == Gdiplus::Ok;
  }
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  Print("host", "shot %s %s (%dx%d at %d,%d)", path.c_str(), ok ? "saved" : "FAILED", w, h, r.left,
        r.top);
}

void BringToFront()
{
  // A background process can't take the foreground by itself; borrowing the foreground thread's
  // input state for the call is the usual way around that.
  const HWND fg = GetForegroundWindow();
  const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  const DWORD me = GetCurrentThreadId();
  if (fg_thread && fg_thread != me)
    AttachThreadInput(me, fg_thread, TRUE);
  ShowWindow(s_window, SW_RESTORE);
  SetWindowPos(s_window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  SetForegroundWindow(s_window);
  SetActiveWindow(s_window);
  if (fg_thread && fg_thread != me)
    AttachThreadInput(me, fg_thread, FALSE);
  Print("host", "front: foreground is %s", GetForegroundWindow() == s_window ? "this window" :
                                                                                 "another window");
}

void RunCommand(const std::string& line)
{
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;
  if (cmd.empty() || cmd[0] == '#' || !s_window)
    return;
  Print("cmd", "%s", line.c_str());
  if (cmd == "send")
  {
    std::string rest;
    std::getline(in >> std::ws, rest);
    SendToOrca(rest);
  }
  else if (cmd == "size")
  {
    int w = 0, h = 0;
    in >> w >> h;
    const double s = Scale();
    RECT r{0, 0, static_cast<LONG>(w * s), static_cast<LONG>(h * s)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, TRUE, 0, GetDpiForWindow(s_window));
    SetWindowPos(s_window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  else if (cmd == "move")
  {
    int x = 0, y = 0;
    in >> x >> y;
    SetWindowPos(s_window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  else if (cmd == "front")
  {
    BringToFront();
  }
  else if (cmd == "topmost")
  {
    int on = 1;
    in >> on;
    SetWindowPos(s_window, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  else if (cmd == "where")
  {
    RECT w{}, c{};
    GetWindowRect(s_window, &w);
    GetClientRect(s_window, &c);
    POINT origin{0, 0};
    ClientToScreen(s_window, &origin);
    const RECT b = Box();
    HWND child = FindWindowExW(s_window, nullptr, L"Orca", nullptr);
    RECT o{};
    if (child)
      GetWindowRect(child, &o);
    Print("host",
          "dpi %u; window %ld,%ld %ldx%ld; client at %ld,%ld %ldx%ld; box %ld,%ld %ldx%ld; orca "
          "child %s %ld,%ld %ldx%ld",
          GetDpiForWindow(s_window), w.left, w.top, w.right - w.left, w.bottom - w.top, origin.x,
          origin.y, c.right, c.bottom, origin.x + b.left, origin.y + b.top, b.right - b.left,
          b.bottom - b.top, child ? (IsWindowVisible(child) ? "shown" : "hidden") : "none", o.left,
          o.top, o.right - o.left, o.bottom - o.top);
  }
  else if (cmd == "click" || cmd == "clickhost")
  {
    if (!IsForeground(cmd.c_str()))
      return;
    POINT p{};
    if (cmd == "click")
    {
      double fx = 0.5, fy = 0.5;
      in >> fx >> fy;
      const RECT b = Box();
      p = {b.left + static_cast<LONG>((b.right - b.left) * fx),
           b.top + static_cast<LONG>((b.bottom - b.top) * fy)};
    }
    else
    {
      p = {Box().left / 2, Box().top / 2};
    }
    ClientToScreen(s_window, &p);
    ClickScreen(p);
  }
  else if (cmd == "key")
  {
    if (!IsForeground("key"))
      return;
    std::string name, how = "tap";
    int count = 1;
    in >> name >> how >> count;
    const WORD vk = VirtualKey(name);
    if (!vk)
    {
      Print("host", "unknown key %s", name.c_str());
      return;
    }
    if (how == "down")
      SendKey(vk, true);
    else if (how == "up")
      SendKey(vk, false);
    else if (how == "hold")
    {
      for (int i = 0; i < std::max(count, 1); ++i)
      {
        SendKey(vk, true);
        Sleep(40);
      }
      SendKey(vk, false);
    }
    else
    {
      SendKey(vk, true);
      Sleep(60);
      SendKey(vk, false);
    }
  }
  else if (cmd == "altf4")
  {
    if (!IsForeground("altf4"))
      return;
    SendKey(VK_MENU, true);
    SendKey(VK_F4, true);
    SendKey(VK_F4, false);
    SendKey(VK_MENU, false);
  }
  else if (cmd == "shot")
  {
    std::string path;
    std::getline(in >> std::ws, path);
    Shot(path);
  }
  else if (cmd == "close")
  {
    if (s_to_orca != INVALID_HANDLE_VALUE)
    {
      CloseHandle(s_to_orca);
      s_to_orca = INVALID_HANDLE_VALUE;
      s_quit_sent_ms = Ms();
    }
  }
  else if (cmd == "closewin")
  {
    DestroyWindow(s_window);
  }
  else if (cmd == "exit")
  {
    PostQuitMessage(0);
  }
  else
  {
    Print("host", "unknown command: %s", line.c_str());
  }
}

void PostCommand(const std::string& line)
{
  if (line.rfind("wait ", 0) == 0)
  {
    Sleep(static_cast<DWORD>(std::max(0, std::atoi(line.c_str() + 5))));
    return;
  }
  // Commands run on the window's thread, one at a time.
  auto* copy = new std::string(line);
  if (!PostMessageW(s_window, WM_APP_COMMAND, 0, reinterpret_cast<LPARAM>(copy)))
    delete copy;
  else
    Sleep(20);
}

void ReadCommandFile(const std::string& path)
{
  std::streamoff offset = 0;
  std::string partial;
  while (true)
  {
    std::ifstream file(path, std::ios::binary);
    if (file)
    {
      file.seekg(0, std::ios::end);
      const std::streamoff size = file.tellg();
      if (size > offset)
      {
        file.seekg(offset);
        std::string chunk(static_cast<size_t>(size - offset), '\0');
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        offset = size;
        partial += chunk;
        size_t newline;
        while ((newline = partial.find('\n')) != std::string::npos)
        {
          std::string line = partial.substr(0, newline);
          partial.erase(0, newline + 1);
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          PostCommand(line);
        }
      }
    }
    Sleep(50);
  }
}

void PaintWindow(HWND hwnd)
{
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  RECT c{};
  GetClientRect(hwnd, &c);
  HBRUSH page = CreateSolidBrush(RGB(48, 48, 56));
  FillRect(dc, &c, page);
  DeleteObject(page);
  // A red frame 3 px outside the box: the game should sit exactly inside it, with no red inside
  // and no gap between the game and the red.
  const RECT b = Box();
  HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
  RECT frame{b.left - 3, b.top - 3, b.right + 3, b.bottom + 3};
  FillRect(dc, &frame, red);
  DeleteObject(red);
  HBRUSH blue = CreateSolidBrush(RGB(0, 64, 255));
  FillRect(dc, &b, blue);  // what shows where the game doesn't cover the box
  DeleteObject(blue);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(220, 220, 220));
  const std::string label = "embed-test-host  dpi " + std::to_string(GetDpiForWindow(hwnd)) +
                            "  box " + RectArgs();
  TextOutA(dc, b.left, std::max(2L, b.top / 4), label.c_str(), static_cast<int>(label.size()));
  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  case WM_APP_COMMAND:
  {
    std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(lp));
    RunCommand(*line);
    return 0;
  }
  case WM_PAINT:
    PaintWindow(hwnd);
    return 0;
  case WM_SIZE:
    InvalidateRect(hwnd, nullptr, FALSE);
    if (s_orca_process)
      SendToOrca("rect " + RectArgs());
    return 0;
  case WM_DPICHANGED:
  {
    const RECT* suggested = reinterpret_cast<const RECT*>(lp);
    Print("host", "dpi changed to %u", HIWORD(wp));
    SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                 suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
  }
  case WM_KEYDOWN:
  case WM_KEYUP:
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP:
    Print("host", "%s vk 0x%02llx%s", msg == WM_KEYDOWN ? "keydown" : msg == WM_KEYUP ? "keyup" :
                                      msg == WM_SYSKEYDOWN ? "syskeydown" : "syskeyup",
          static_cast<unsigned long long>(wp), (lp & (1 << 30)) && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? " (repeat)" : "");
    break;
  case WM_SYSCOMMAND:
    Print("host", "syscommand 0x%04llx", static_cast<unsigned long long>(wp & 0xFFF0));
    break;
  case WM_ENTERMENULOOP:
    Print("host", "menu opened");
    break;
  case WM_EXITMENULOOP:
    Print("host", "menu closed");
    break;
  case WM_SETFOCUS:
    Print("host", "window got the keyboard focus");
    break;
  case WM_KILLFOCUS:
    Print("host", "window lost the keyboard focus");
    break;
  case WM_ACTIVATE:
    Print("host", "%s", LOWORD(wp) == WA_INACTIVE ? "deactivated" : "activated");
    break;
  case WM_COMMAND:
    if (LOWORD(wp) == MENU_FILE_QUIT)
      DestroyWindow(hwnd);
    return 0;
  case WM_CLOSE:
    Print("host", "WM_CLOSE: destroying the window");
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    Print("host", "window destroyed (Orca's stdin stays open)");
    s_window = nullptr;
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring QuoteArg(const std::wstring& arg)
{
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
    return arg;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t ch : arg)
  {
    if (ch == L'\\')
    {
      ++backslashes;
      continue;
    }
    out.append(ch == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    backslashes = 0;
    out += ch;
  }
  out.append(backslashes * 2, L'\\');
  return out + L"\"";
}
}  // namespace

// --adopt: Orca's top-level "Orca" window (class and caption) of process `pid`, as the launcher
// finds it.
struct Adoption
{
  DWORD pid;
  HWND found;
};
BOOL CALLBACK FindOrcaWindow(HWND h, LPARAM l)
{
  auto* a = reinterpret_cast<Adoption*>(l);
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  wchar_t cls[64], title[64];
  if (pid == a->pid && GetClassNameW(h, cls, 64) && std::wstring(cls) == L"Orca" &&
      GetWindowTextW(h, title, 64) && std::wstring(title) == L"Orca")
  {
    a->found = h;
    return FALSE;
  }
  return TRUE;
}

// What yougame-sandbox.exe does with --embed-parent and --embed-keep-style, from outside Orca.
void Adopt(HANDLE process, DWORD pid, HWND parent)
{
  const long long start = Ms();
  while (WaitForSingleObject(process, 5) == WAIT_TIMEOUT)
  {
    Adoption a{pid, nullptr};
    EnumWindows(FindOrcaWindow, reinterpret_cast<LPARAM>(&a));
    if (!a.found)
      continue;
    LONG_PTR style = GetWindowLongPtrW(a.found, GWL_STYLE);
    style = (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD;
    const LONG_PTR ex = GetWindowLongPtrW(a.found, GWL_EXSTYLE);
    if (!(ex & WS_EX_NOPARENTNOTIFY))
      SetWindowLongPtrW(a.found, GWL_EXSTYLE, ex | WS_EX_NOPARENTNOTIFY);
    SetWindowLongPtrW(a.found, GWL_STYLE, style);
    if (SetParent(a.found, parent))
      Print("host", "adopted Orca's window %lld ms after the start", Ms() - start);
    else
      Print("host", "could not adopt Orca's window (%lu)", GetLastError());
    return;
  }
}

int wmain(int argc, wchar_t** argv)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  std::string command_file, stderr_file;
  int adopt = -1;  // --adopt: 1 adopt the window, 0 never; -1 plain --embed
  int first = 1;
  while (first + 1 < argc && argv[first][0] == L'-' && argv[first][1] == L'-')
  {
    const std::wstring flag = argv[first];
    const std::wstring value = argv[first + 1];
    const std::string narrow = Narrow(value);
    if (flag == L"--commands")
      command_file = narrow;
    else if (flag == L"--stderr")
      stderr_file = narrow;
    else if (flag == L"--adopt")
      adopt = narrow == "1" ? 1 : 0;
    else
      break;
    first += 2;
  }
  if (first >= argc)
  {
    std::fprintf(stderr, "usage: embed-test-host-win [--commands FILE] [--stderr FILE] "
                         "[--adopt 0|1] <DolphinNoGUI.exe> [orca args...]\n");
    return 2;
  }

  Gdiplus::GdiplusStartupInput gdiplus_input;
  ULONG_PTR gdiplus_token = 0;  // never shut down: the process ends with ExitProcess
  Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr);

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"OrcaEmbedTestHost";
  RegisterClassExW(&wc);
  HMENU menu = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, MENU_FILE_QUIT, L"&Quit");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
  AppendMenuW(menu, MF_STRING, 0, L"&View");
  // AGENT TEST: a test window must never be taken for a player's.
  s_window = CreateWindowExW(0, wc.lpszClassName, L"AGENT TEST Orca embed test host",
                             WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, menu, wc.hInstance,
                             nullptr);
  // 1280x800 DIPs of client area.
  RunCommand("size 1280 800");
  ShowWindow(s_window, SW_SHOW);
  UpdateWindow(s_window);

  // Orca: stdin and stdout piped, stderr to a file (or this console).
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE orca_stdin_read, orca_stdout_read, orca_stdout_write;
  CreatePipe(&orca_stdin_read, &s_to_orca, &inherit, 0);
  SetHandleInformation(s_to_orca, HANDLE_FLAG_INHERIT, 0);
  CreatePipe(&orca_stdout_read, &orca_stdout_write, &inherit, 0);
  SetHandleInformation(orca_stdout_read, HANDLE_FLAG_INHERIT, 0);
  HANDLE orca_stderr = GetStdHandle(STD_ERROR_HANDLE);
  if (!stderr_file.empty())
  {
    orca_stderr = CreateFileW(Widen(stderr_file).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }

  std::wstring cmdline = QuoteArg(argv[first]);
  const RECT b = Box();
  cmdline += L" --embed --parent " + std::to_wstring(reinterpret_cast<uintptr_t>(s_window)) +
             L" --rect " + std::to_wstring(b.left) + L" " + std::to_wstring(b.top) + L" " +
             std::to_wstring(b.right - b.left) + L" " + std::to_wstring(b.bottom - b.top);
  if (adopt >= 0)
    cmdline += L" --embed-adopted";
  for (int i = first + 1; i < argc; ++i)
    cmdline += L" " + QuoteArg(argv[i]);

  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = orca_stdin_read;
  si.hStdOutput = orca_stdout_write;
  si.hStdError = orca_stderr;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi))
  {
    Print("host", "CreateProcess failed (%lu)", GetLastError());
    return 1;
  }
  CloseHandle(pi.hThread);
  CloseHandle(orca_stdin_read);
  CloseHandle(orca_stdout_write);
  s_orca_process = pi.hProcess;
  Print("host", "started Orca (pid %lu): %ls", pi.dwProcessId, cmdline.c_str());
  if (adopt == 1)
  {
    const DWORD pid = pi.dwProcessId;
    std::thread([pid] { Adopt(s_orca_process, pid, s_window); }).detach();
  }

  std::thread([orca_stdout_read] {
    std::string pending;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(orca_stdout_read, buffer, sizeof(buffer), &got, nullptr) && got > 0)
    {
      pending.append(buffer, got);
      size_t newline;
      while ((newline = pending.find('\n')) != std::string::npos)
      {
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        Print("orca", "%s", line.c_str());
      }
    }
    Print("host", "Orca's stdout closed");
  }).detach();

  std::thread([] {
    WaitForSingleObject(s_orca_process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(s_orca_process, &code);
    const long long quit = s_quit_sent_ms.load();
    if (quit >= 0)
      Print("host", "Orca exited with code %lu, %lld ms after quit/close", code, Ms() - quit);
    else
      Print("host", "Orca exited with code %lu", code);
  }).detach();

  if (!command_file.empty())
    std::thread(ReadCommandFile, command_file).detach();
  else
  {
    std::thread([] {
      std::string line;
      while (std::getline(std::cin, line))
        PostCommand(line);
    }).detach();
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // `exit`: the app goes away, its pipe with it.
  if (s_window && s_to_orca != INVALID_HANDLE_VALUE)
  {
    CloseHandle(s_to_orca);
    s_to_orca = INVALID_HANDLE_VALUE;
    s_quit_sent_ms = Ms();
  }
  // The window is gone (closed, Alt+F4, closewin): give Orca 10 s to notice by itself, keeping its
  // stdin open so the window's destruction is what stops it.
  if (WaitForSingleObject(s_orca_process, 10000) == WAIT_TIMEOUT)
  {
    Print("host", "Orca still running 10 s after the window closed: closing its stdin");
    if (s_to_orca != INVALID_HANDLE_VALUE)
      CloseHandle(s_to_orca);
    s_to_orca = INVALID_HANDLE_VALUE;
    if (WaitForSingleObject(s_orca_process, 10000) == WAIT_TIMEOUT)
    {
      Print("host", "Orca HUNG: still running 10 s after its stdin closed; terminating it");
      TerminateProcess(s_orca_process, 99);
    }
  }
  Sleep(200);  // let the exit line print
  std::fflush(stdout);
  // At once, without GdiplusShutdown or the C runtime's exit: the command and pipe threads are
  // still blocked in their reads.
  ExitProcess(0);
}
