// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unistd.h>

// X.h defines None to be 0L, but other parts of Dolphin undef that so that
// None can be used in enums.  Work around that here by copying the definition
// before it is undefined.
#include <X11/X.h>
static constexpr auto X_None = None;

#include "DolphinNoGUI/Platform.h"

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/State.h"
#include "Core/System.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#ifdef HAVE_XSHAPE
#include <X11/extensions/shape.h>
#endif
#include "UICommon/UICommon.h"
#include "UICommon/X11Utils.h"
#include "VideoCommon/Present.h"

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX _POSIX_HOST_NAME_MAX
#endif

namespace
{
// Embed mode: X errors seen so far (LogXError).
std::atomic<int> s_x_errors{0};

// Embed mode: an X error is logged instead of ending Orca. The app's window can go away between two
// requests that name it, and Xlib's default handler would exit at once, without the protocol's
// last line. A lost connection to the X server still ends the process (Xlib's I/O error handler).
int LogXError(Display* display, XErrorEvent* event)
{
  // Once the app's window is gone, every frame and every mouse move can answer one: the log keeps
  // the first 20, then one in a thousand.
  const int count = s_x_errors.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count > 20 && count % 1000 != 0)
    return 0;
  char text[160] = "";
  XGetErrorText(display, event->error_code, text, sizeof(text));
  std::fprintf(stderr, "Orca: X error %d (%s) on request %d.%d, resource 0x%lx (%d so far)\n",
               event->error_code, text, event->request_code, event->minor_code,
               static_cast<unsigned long>(event->resourceid), count);
  return 0;
}

class PlatformX11 : public Platform
{
public:
  ~PlatformX11() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

protected:
  void EmbedSetRect(const Embed::Rect& rect) override;
  void EmbedSetVisible(bool visible) override;
  void EmbedSetFocus(bool focus) override;

private:
  bool InitEmbedded();
  void CloseDisplay();
  void UpdateWindowPosition();
  void ProcessEvents();
  void ProcessEmbeddedEvent(const XEvent& event);
  bool AppHasFocus();
  void UpdateAppState();
  void UpdateEmbedFocus();
  void UpdateSurfaceVisible();

  Display* m_display = nullptr;
  Window m_window = {};
  Cursor m_blank_cursor = X_None;
#ifdef HAVE_XRANDR
  X11Utils::XRRConfiguration* m_xrr_config = nullptr;
#endif
  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  unsigned int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  unsigned int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);

  // Embed mode (Embed.h): the app's window, an X11 window id (--parent), which Orca's window is a
  // child of, and the root window of its screen.
  Window m_parent = {};
  Window m_root = {};
  // The app's "show" / "hide". A rect while hidden must not show the window again.
  bool m_embed_visible = true;
  // The app's "focus" / "blur": whether the game gets input. On by default, as on the other
  // platforms.
  bool m_embed_focus = true;
  // The keyboard is in the app's window (its FocusIn and FocusOut, checked with the X server).
  bool m_app_focused = false;
  // The app's window is viewable: it and its window manager's frame are mapped, so not minimized
  // and on the workspace shown.
  bool m_app_viewable = true;
  bool m_surface_visible = true;
  // Orca's window's size, and whether the presenter has yet to resize to it: a move alone needs no
  // resize, and one that came before the presenter existed is made once it does.
  int m_embed_width = 0;
  int m_embed_height = 0;
  bool m_resize_pending = false;
  std::chrono::steady_clock::time_point m_next_app_check{};
};

PlatformX11::~PlatformX11()
{
#ifdef HAVE_XRANDR
  delete m_xrr_config;
#endif

  if (m_display)
  {
    if (m_blank_cursor != X_None)
      XFreeCursor(m_display, m_blank_cursor);

    XCloseDisplay(m_display);
  }
}

bool PlatformX11::Init()
{
  XInitThreads();
  m_display = XOpenDisplay(nullptr);
  if (!m_display)
  {
    const char* display = std::getenv("DISPLAY");
    PanicAlertFmt("No X11 display found (DISPLAY is {})", display ? display : "unset");
    return false;
  }

  if (IsEmbedded())
    return InitEmbedded();

  m_window = XCreateSimpleWindow(m_display, DefaultRootWindow(m_display), m_window_x, m_window_y,
                                 m_window_width, m_window_height, 0, 0, BlackPixel(m_display, 0));
  XSelectInput(m_display, m_window, StructureNotifyMask | KeyPressMask | FocusChangeMask);
  Atom wmProtocols[1];
  wmProtocols[0] = XInternAtom(m_display, "WM_DELETE_WINDOW", True);
  XSetWMProtocols(m_display, m_window, wmProtocols, 1);
  pid_t pid = getpid();
  XChangeProperty(m_display, m_window, XInternAtom(m_display, "_NET_WM_PID", False), XA_CARDINAL,
                  32, PropModeReplace, reinterpret_cast<unsigned char*>(&pid), 1);
  char host_name[HOST_NAME_MAX] = "";
  if (!gethostname(host_name, sizeof(host_name)))
  {
    XTextProperty wmClientMachine = {reinterpret_cast<unsigned char*>(host_name), XA_STRING, 8,
                                     strlen(host_name)};
    XSetWMClientMachine(m_display, m_window, &wmClientMachine);
  }
  XMapRaised(m_display, m_window);
  XFlush(m_display);
  XSync(m_display, True);
  ProcessEvents();

  if (Config::Get(Config::MAIN_DISABLE_SCREENSAVER))
    UICommon::InhibitScreenSaver(true);

#ifdef HAVE_XRANDR
  m_xrr_config = new X11Utils::XRRConfiguration(m_display, m_window);
#endif

  if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never)
  {
    // make a blank cursor
    Pixmap Blank;
    XColor DummyColor;
    char ZeroData[1] = {0};
    Blank = XCreateBitmapFromData(m_display, m_window, ZeroData, 1, 1);
    m_blank_cursor = XCreatePixmapCursor(m_display, Blank, Blank, &DummyColor, &DummyColor, 0, 0);
    XFreePixmap(m_display, Blank);
    XDefineCursor(m_display, m_window, m_blank_cursor);
  }

  // Enter fullscreen if enabled.
  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    m_window_fullscreen = X11Utils::ToggleFullscreen(m_display, m_window);
#ifdef HAVE_XRANDR
    m_xrr_config->ToggleDisplayMode(True);
#endif
    ProcessEvents();
  }

  UpdateWindowPosition();
  return true;
}

// Embed mode: Orca's window is a child of the app's window (the YouGame app runs as an X11 client,
// through XWayland on a Wayland desktop), placed at the player box in the window's pixels, so it
// moves, clips and hides with the app and no window manager ever sees it. It takes neither the
// keyboard nor the mouse: presses go up to the app's window and its page, as on macOS (ORCA.md,
// "Focus"). No full screen, cursor, screen saver or title of its own: those are the app's.
bool PlatformX11::InitEmbedded()
{
  XSetErrorHandler(LogXError);
  m_parent = static_cast<Window>(m_embed.parent);
  XWindowAttributes app = {};
  if (m_parent == X_None || !XGetWindowAttributes(m_display, m_parent, &app))
  {
    std::fprintf(stderr, "Orca: the app's window %llu is not an X11 window on this display\n",
                 m_embed.parent);
    return false;
  }
  m_root = app.root;

  // A 24-bit TrueColor visual of its own, with its own colormap and border, so it may sit in an app
  // window of any depth (in a 32-bit one the game's alpha would reach the compositor); the app's
  // own visual where the server has none. Black until the first frame. Dolphin's GL context makes a
  // child of this window in the visual EGL picks (Common/GL/GLX11Window.cpp).
  const int errors = s_x_errors.load(std::memory_order_relaxed);
  XSetWindowAttributes attributes = {};
  attributes.border_pixel = 0;
  XVisualInfo own = {};
  Visual* visual = app.visual;
  int depth = app.depth;
  if (XMatchVisualInfo(m_display, XScreenNumberOfScreen(app.screen), 24, TrueColor, &own))
  {
    visual = own.visual;
    depth = own.depth;
    attributes.colormap = XCreateColormap(m_display, m_parent, visual, AllocNone);
    attributes.background_pixel = 0;  // TrueColor black
  }
  else
  {
    attributes.colormap = app.colormap;
    attributes.background_pixel =
        app.depth == 32 ? 0xff000000UL : BlackPixelOfScreen(app.screen);  // opaque black
  }
  const Embed::Rect& r = m_embed.rect;
  m_window = XCreateWindow(m_display, m_parent, r.x, r.y, static_cast<unsigned int>(r.w),
                           static_cast<unsigned int>(r.h), 0, depth, InputOutput, visual,
                           CWBackPixel | CWBorderPixel | CWColormap, &attributes);
  m_embed_width = r.w;
  m_embed_height = r.h;
#ifdef HAVE_XSHAPE
  // An empty input shape: the pointer passes through Orca's window, and the GL child inside it, to
  // the app's window, which so never sees it leave for a child over its own picture box.
  int shape_event = 0, shape_error = 0;
  if (XShapeQueryExtension(m_display, &shape_event, &shape_error))
    XShapeCombineRectangles(m_display, m_window, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
#endif
  // Only the structure of Orca's window: no keys and no mouse, so every press goes up the tree to
  // the app's window. Dolphin's GL context selects the same on it.
  XSelectInput(m_display, m_window, StructureNotifyMask);
  // The app's window: whether it has the keyboard, whether it is mapped, and its end. Any number of
  // clients may watch a window's structure and focus.
  XSelectInput(m_display, m_parent, StructureNotifyMask | FocusChangeMask);
  m_embed_visible = !m_embed.too_small;  // Hidden until a big enough rect (Embed.h).
  if (m_embed_visible)
    XMapRaised(m_display, m_window);
  XSync(m_display, False);
  if (s_x_errors.load(std::memory_order_relaxed) != errors)
  {
    std::fprintf(stderr, "Orca: couldn't open a window inside the app's window %llu\n",
                 m_embed.parent);
    return false;
  }

  m_app_viewable = app.map_state == IsViewable;
  m_app_focused = AppHasFocus();
  UpdateEmbedFocus();
  UpdateSurfaceVisible();
  return true;
}

void PlatformX11::SetTitle(const std::string& string)
{
  // Embedded, the window title is the app's.
  if (!IsEmbedded())
    XStoreName(m_display, m_window, string.c_str());
}

void PlatformX11::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    UpdateWindowPosition();
    if (IsEmbedded())
    {
      // The app's window reports its own focus and mapping; a window manager that moves the
      // keyboard to its frame, or unmaps the frame (another workspace), sends it nothing, so the
      // server is asked again twice a second too.
      const auto now = std::chrono::steady_clock::now();
      if (now >= m_next_app_check)
      {
        m_next_app_check = now + std::chrono::milliseconds(500);
        UpdateAppState();
      }
      if (m_resize_pending && g_presenter)
      {
        m_resize_pending = false;
        g_presenter->ResizeSurface();
      }
      UpdateEmbedFocus();
    }

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformX11::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::X11;
  wsi.display_connection = static_cast<void*>(m_display);
  wsi.render_window = reinterpret_cast<void*>(m_window);
  wsi.render_surface = reinterpret_cast<void*>(m_window);
  return wsi;
}

void PlatformX11::UpdateWindowPosition()
{
  if (m_window_fullscreen || IsEmbedded())
    return;

  Window winDummy;
  unsigned int borderDummy, depthDummy;
  XGetGeometry(m_display, m_window, &winDummy, &m_window_x, &m_window_y, &m_window_width,
               &m_window_height, &borderDummy, &depthDummy);
}

// The window commands act only when embedded: ORCA_TEST_COMMANDS alone sends them too, and the
// window then is Dolphin's own top-level one, the window manager's to place.
void PlatformX11::EmbedSetRect(const Embed::Rect& r)
{
  if (!IsEmbedded())
    return;
  XMoveResizeWindow(m_display, m_window, r.x, r.y, static_cast<unsigned int>(r.w),
                    static_cast<unsigned int>(r.h));
  // Back above the app's own children, as Windows' HWND_TOP: Chromium's GPU process may make a new
  // one of its own (after a GPU reset), stacked over the game.
  XRaiseWindow(m_display, m_window);
  XFlush(m_display);
}

void PlatformX11::EmbedSetVisible(bool visible)
{
  if (!IsEmbedded())
    return;
  m_embed_visible = visible;
  if (visible)
    XMapRaised(m_display, m_window);
  else
    XUnmapWindow(m_display, m_window);
  XFlush(m_display);
  UpdateEmbedFocus();
  UpdateSurfaceVisible();
}

void PlatformX11::EmbedSetFocus(bool focus)
{
  if (!IsEmbedded())
    return;
  m_embed_focus = focus;
  // A "focus" follows a click or a key in the app's window: ask the server now rather than wait for
  // the window's event.
  m_app_focused = AppHasFocus();
  UpdateEmbedFocus();
}

// When embedded, the app keeps the keyboard and this window never takes it. Input counts while the
// keyboard is in the app's window, the game is shown, and the app's last word was "focus".
void PlatformX11::UpdateEmbedFocus()
{
  m_window_focus = m_embed_visible && m_embed_focus && m_app_focused && m_app_viewable;
}

// What the server says about the app's window now: whether it is viewable and has the keyboard.
void PlatformX11::UpdateAppState()
{
  XWindowAttributes app = {};
  m_app_viewable = XGetWindowAttributes(m_display, m_parent, &app) && app.map_state == IsViewable;
  m_app_focused = AppHasFocus();
  UpdateSurfaceVisible();
  // Kept on top of the app's other children here too: a GPU-process window Chromium makes without a
  // rect from the page (a GPU reset while the box stays put) would otherwise hide the game.
  if (m_embed_visible)
    XRaiseWindow(m_display, m_window);
}

// Whether the X server has the keyboard in the app's window: on it, on a window inside it, or on
// the window manager's frame around it. With PointerRoot focus the keyboard follows the pointer, so
// the top-level window under the pointer stands for it.
bool PlatformX11::AppHasFocus()
{
  Window focus = X_None;
  int revert = 0;
  XGetInputFocus(m_display, &focus, &revert);
  if (focus == PointerRoot)
  {
    Window root = X_None;
    Window child = X_None;
    int root_x = 0, root_y = 0, x = 0, y = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(m_display, m_root, &root, &child, &root_x, &root_y, &x, &y, &mask))
      return false;
    focus = child;
  }
  if (focus == X_None || focus == m_root)
    return false;
  // Whether `from` is `target` or inside it, walking up to the root.
  const auto inside = [this](Window from, Window target) {
    for (Window w = from; w != X_None && w != m_root;)
    {
      if (w == target)
        return true;
      Window root = X_None;
      Window parent = X_None;
      Window* children = nullptr;
      unsigned int count = 0;
      if (!XQueryTree(m_display, w, &root, &parent, &children, &count))
        return false;
      if (children)
        XFree(children);
      w = parent;
    }
    return false;
  };
  return inside(focus, m_parent) || inside(m_parent, focus);
}

// Out of sight (hidden by the app, or the app's window not viewable), the presenter skips what only
// a visible window needs (VideoCommon::Presenter::SetSurfaceVisible).
void PlatformX11::UpdateSurfaceVisible()
{
  const bool visible = m_embed_visible && m_app_viewable;
  if (visible == m_surface_visible)
    return;
  m_surface_visible = visible;
  VideoCommon::Presenter::SetSurfaceVisible(visible);
  NOTICE_LOG_FMT(VIDEO, "Orca: the game's window is {}", visible ? "visible" : "out of sight");
}

void PlatformX11::ProcessEvents()
{
  XEvent event;
  KeySym key;
  // Asked again before each read: nothing else on this connection takes events off the queue, so
  // XNextEvent never waits.
  while (XPending(m_display) > 0)
  {
    XNextEvent(m_display, &event);
    if (IsEmbedded())
    {
      ProcessEmbeddedEvent(event);
      continue;
    }
    switch (event.type)
    {
    case KeyPress:
      key = XLookupKeysym((XKeyEvent*)&event, 0);
      if (key == XK_Escape)
      {
        RequestShutdown();
      }
      else if (key == XK_F10)
      {
        if (Core::GetState(Core::System::GetInstance()) == Core::State::Running)
        {
          if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never)
            XUndefineCursor(m_display, m_window);
          Core::SetState(Core::System::GetInstance(), Core::State::Paused);
        }
        else
        {
          if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never)
            XDefineCursor(m_display, m_window, m_blank_cursor);
          Core::SetState(Core::System::GetInstance(), Core::State::Running);
        }
      }
      else if ((key == XK_Return) && (event.xkey.state & Mod1Mask))
      {
        m_window_fullscreen = !m_window_fullscreen;
        X11Utils::ToggleFullscreen(m_display, m_window);
#ifdef HAVE_XRANDR
        m_xrr_config->ToggleDisplayMode(m_window_fullscreen);
#endif
        UpdateWindowPosition();
      }
      else if (key >= XK_F1 && key <= XK_F8)
      {
        int slot_number = key - XK_F1 + 1;
        if (event.xkey.state & ShiftMask)
          State::Save(Core::System::GetInstance(), slot_number);
        else
          State::Load(Core::System::GetInstance(), slot_number);
      }
      else if (key == XK_F9)
        Core::SaveScreenShot();
      else if (key == XK_F11)
        State::LoadLastSaved(Core::System::GetInstance());
      else if (key == XK_F12)
      {
        if (event.xkey.state & ShiftMask)
          State::UndoLoadState(Core::System::GetInstance());
        else
          State::UndoSaveState(Core::System::GetInstance());
      }
      break;
    case FocusIn:
    {
      m_window_focus = true;
      if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never &&
          Core::GetState(Core::System::GetInstance()) != Core::State::Paused)
      {
        XDefineCursor(m_display, m_window, m_blank_cursor);
      }
    }
    break;
    case FocusOut:
    {
      m_window_focus = false;
      if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never)
        XUndefineCursor(m_display, m_window);
    }
    break;
    case ClientMessage:
    {
      if ((unsigned long)event.xclient.data.l[0] ==
          XInternAtom(m_display, "WM_DELETE_WINDOW", False))
        Stop();
    }
    break;
    case ConfigureNotify:
    {
      if (g_presenter)
        g_presenter->ResizeSurface();
    }
    break;
    }
  }
}

// Embed mode: Orca's own window reports its size; the app's window its focus, mapping and end.
// Neither sends keys or clicks (InitEmbedded), so none of Dolphin's hotkeys exist here: the page
// has the keyboard.
void PlatformX11::ProcessEmbeddedEvent(const XEvent& event)
{
  const Window window = event.xany.window;
  switch (event.type)
  {
  case ConfigureNotify:
    // Sent for a move or a restack too: the presenter resizes only to a new size (MainLoop).
    if (window == m_window &&
        (event.xconfigure.width != m_embed_width || event.xconfigure.height != m_embed_height))
    {
      m_embed_width = event.xconfigure.width;
      m_embed_height = event.xconfigure.height;
      m_resize_pending = true;
    }
    break;
  case FocusIn:
  case FocusOut:
    if (window == m_parent)
      m_app_focused = AppHasFocus();
    break;
  case MapNotify:
  case UnmapNotify:
    if (window == m_parent)
      UpdateAppState();
    break;
  case DestroyNotify:
    // The app's window was destroyed, and Orca's with it: stop, as on Windows. The app also says
    // "quit" or closes stdin when it can.
    if (event.xdestroywindow.window == m_parent || event.xdestroywindow.window == m_window)
      Stop();
    break;
  default:
    break;
  }
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateX11Platform()
{
  return std::make_unique<PlatformX11>();
}
