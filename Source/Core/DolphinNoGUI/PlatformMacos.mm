// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/OverlayKey.h"
#include "Core/Orca/UX/YgOrb.h"
#include "Core/State.h"
#include "Core/System.h"
#include "DolphinNoGUI/EmbedMacTiming.h"
#include "InputCommon/Shortcuts.h"
#include "VideoCommon/EFBInterface.h"
#include "VideoCommon/Present.h"

#include <AppKit/AppKit.h>
#include <Carbon/Carbon.h>
#include <CoreGraphics/CoreGraphics.h>
#include <Foundation/Foundation.h>
#include <array>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

@interface Application : NSApplication
@property Platform* platform;
- (void)shutdown;
- (void)togglePause;
- (void)saveScreenShot;
- (void)loadLastSaved;
- (void)undoLoadState;
- (void)undoSaveState;
- (void)loadState:(id)sender;
- (void)saveState:(id)sender;
@end

@implementation Application
- (void)shutdown;
{
  [self platform]->RequestShutdown();
  [self stop:nil];
}

- (void)togglePause
{
  auto& system = Core::System::GetInstance();
  if (Core::GetState(system) == Core::State::Running)
    Core::SetState(system, Core::State::Paused);
  else
    Core::SetState(system, Core::State::Running);
}

- (void)saveScreenShot
{
  Core::SaveScreenShot();
}

- (void)loadLastSaved
{
  State::LoadLastSaved(Core::System::GetInstance());
}

- (void)undoLoadState
{
  State::UndoLoadState(Core::System::GetInstance());
}

- (void)undoSaveState
{
  State::UndoSaveState(Core::System::GetInstance());
}

- (void)loadState:(id)sender
{
  State::Load(Core::System::GetInstance(), [sender tag]);
}

- (void)saveState:(id)sender
{
  State::Save(Core::System::GetInstance(), [sender tag]);
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate>

@property(readonly) Platform* platform;

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender;
- (id)initWithPlatform:(Platform*)platform;
@end

@implementation AppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
  return YES;
}

- (id)initWithPlatform:(Platform*)platform
{
  self = [super init];
  if (self)
  {
    _platform = platform;
  }
  return self;
}
@end

// Embed mode: Orca's window floats over the YouGame app's player box but never becomes key. Clicks
// fall through to the app's window, and pads and keys are read from HID state while the app says
// the game has focus.
@interface EmbedWindow : NSPanel
@end

@implementation EmbedWindow
- (BOOL)canBecomeKeyWindow
{
  return ([self styleMask] & NSWindowStyleMaskTitled) != 0;
}

- (BOOL)canBecomeMainWindow
{
  return ([self styleMask] & NSWindowStyleMaskTitled) != 0;
}
@end

@interface WindowDelegate : NSObject <NSWindowDelegate>

- (void)windowDidResize:(NSNotification*)notification;
@end

@implementation WindowDelegate

- (void)windowDidResize:(NSNotification*)notification
{
  if (g_presenter)
    g_presenter->ResizeSurface();
}
@end

namespace
{
void OutKey(std::string_view line)
{
  Embed::Out(std::string(line));
}

std::string ShortcutKey(unsigned short code)
{
  switch (code)
  {
  case kVK_ANSI_A:
    return "A";
  case kVK_ANSI_B:
    return "B";
  case kVK_ANSI_C:
    return "C";
  case kVK_ANSI_D:
    return "D";
  case kVK_ANSI_E:
    return "E";
  case kVK_ANSI_F:
    return "F";
  case kVK_ANSI_G:
    return "G";
  case kVK_ANSI_H:
    return "H";
  case kVK_ANSI_I:
    return "I";
  case kVK_ANSI_J:
    return "J";
  case kVK_ANSI_K:
    return "K";
  case kVK_ANSI_L:
    return "L";
  case kVK_ANSI_M:
    return "M";
  case kVK_ANSI_N:
    return "N";
  case kVK_ANSI_O:
    return "O";
  case kVK_ANSI_P:
    return "P";
  case kVK_ANSI_Q:
    return "Q";
  case kVK_ANSI_R:
    return "R";
  case kVK_ANSI_S:
    return "S";
  case kVK_ANSI_T:
    return "T";
  case kVK_ANSI_U:
    return "U";
  case kVK_ANSI_V:
    return "V";
  case kVK_ANSI_W:
    return "W";
  case kVK_ANSI_X:
    return "X";
  case kVK_ANSI_Y:
    return "Y";
  case kVK_ANSI_Z:
    return "Z";
  case kVK_F1:
    return "F1";
  case kVK_F2:
    return "F2";
  case kVK_F3:
    return "F3";
  case kVK_F4:
    return "F4";
  case kVK_F5:
    return "F5";
  case kVK_F6:
    return "F6";
  case kVK_F7:
    return "F7";
  case kVK_F8:
    return "F8";
  case kVK_F9:
    return "F9";
  case kVK_F10:
    return "F10";
  case kVK_F11:
    return "F11";
  case kVK_F12:
    return "F12";
  case kVK_Tab:
    return "Tab";
  case kVK_Return:
    return "Enter";
  case kVK_ANSI_Grave:
    return "Backquote";
  case kVK_Shift:
  case kVK_RightShift:
    return "Shift";
  case kVK_Control:
  case kVK_RightControl:
    return "Ctrl";
  case kVK_Option:
  case kVK_RightOption:
    return "Alt";
  default:
    return {};
  }
}

class PlatformMacOS : public Platform
{
public:
  ~PlatformMacOS() override;

  bool Init() override;
  void SetTitle(const std::string& title) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

protected:
  std::optional<Embed::WindowBounds> EmbedGetBounds() const override;
  void EmbedSetRect(const Embed::Rect& rect) override;
  void EmbedSetVisible(bool visible) override;
  void EmbedSetFocus(bool focus) override;

private:
  bool InitEmbedded();
  void BeginGameActivity();
  // Embed mode, every main loop pass: keeps the window on top of the parent as it moves, resizes,
  // hides or changes Space, and decides whether the game gets input. Returns false once the parent
  // window is gone.
  bool TrackParent(bool force = false);
  void ProcessEvents();
  // Tells the video backend whether any of the window is on screen: not when minimized, ordered out
  // or fully covered.
  void UpdateSurfaceVisible();
  void ObserveOcclusion();
  void UpdateWindowPosition();
  void HandleSaveStates(NSUInteger key, NSUInteger flags);
  void SetupMenu();

  NSRect m_window_rect;
  NSWindow* m_window;
  NSMenu* menuBar;
  AppDelegate* m_app_delegate;
  WindowDelegate* m_window_delegate;
  id m_game_activity = nil;

  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  unsigned int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  unsigned int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
  bool m_window_fullscreen = Config::Get(Config::MAIN_FULLSCREEN);

  // Embed mode.
  bool m_embed_visible = true;  // "show" / "hide"
  bool m_embed_focus = true;    // "focus" / "blur"
  bool m_embed_placed = false;
  bool m_embed_on_screen = false;  // Ordered in, over the parent.
  bool m_embed_controls_hit = false;
  bool m_left_down = false;  // From this window's left mouse down/up events.
  Embed::Mac::QueryTimer m_parent_query{Embed::Mac::PARENT_QUERY_INTERVAL};
  Embed::Mac::QueryTimer m_above_query{Embed::Mac::ABOVE_QUERY_INTERVAL};
  Embed::Mac::SettleGate m_parent_settle;
  bool m_settle_wait_logged = false;
  bool m_parent_exists = false;
  bool m_parent_on_screen = false;
  pid_t m_parent_owner = 0;
  CGRect m_parent_bounds = CGRectZero;
  id m_activation_observer = nil;
  // When the parent left the window list. Stop after a grace period: a new window, or one moving
  // between Spaces, can be missing from the list for a moment.
  std::optional<std::chrono::steady_clock::time_point> m_parent_missing_since;

  id m_occlusion_observer = nil;
  bool m_surface_visible = true;
  int m_surface_refresh_rate = -1;
};

PlatformMacOS::~PlatformMacOS()
{
  if (m_game_activity)
    [[NSProcessInfo processInfo] endActivity:m_game_activity];
  if (m_occlusion_observer)
    [[NSNotificationCenter defaultCenter] removeObserver:m_occlusion_observer];
  if (m_activation_observer)
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:m_activation_observer];
  [m_window close];
}

// finishLaunching must run once, whether the disc picker or Init gets there first.
static bool s_launched = false;

static void FinishLaunching()
{
  [Application sharedApplication];
  if (s_launched)
    return;
  s_launched = true;
  [Application.sharedApplication finishLaunching];
}

bool PlatformMacOS::Init()
{
  if (IsEmbedded())
  {
    const bool initialized = InitEmbedded();
    InputCommon::Shortcuts::Get().SetPolling(IsEmbedded());
    Orca::Status::SetShortcutsAvailable(initialized);
    return initialized;
  }

  BeginGameActivity();
  [Application sharedApplication];

  m_app_delegate = [[AppDelegate alloc] initWithPlatform:this];
  [NSApp setDelegate:m_app_delegate];

  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp setPlatform:this];
  FinishLaunching();

  unsigned long styleMask =
      NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable;

  m_window_rect = CGRectMake(m_window_x, m_window_y, m_window_width, m_window_height);
  m_window = [NSWindow alloc];
  m_window = [m_window initWithContentRect:m_window_rect
                                 styleMask:styleMask
                                   backing:NSBackingStoreBuffered
                                     defer:NO];
  m_window_delegate = [[WindowDelegate alloc] init];
  [m_window setDelegate:m_window_delegate];

  NSNotificationCenter* c = [NSNotificationCenter defaultCenter];
  [c addObserver:NSApp
        selector:@selector(shutdown)
            name:NSWindowWillCloseNotification
          object:m_window];

  if (m_window == nil)
  {
    NSLog(@"Window is %@\n", m_window);
    return false;
  }

  if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never)
    [NSCursor hide];

  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    m_window_fullscreen = true;
    [m_window toggleFullScreen:m_window];
  }

  [m_window makeKeyAndOrderFront:NSApp];
  [m_window makeMainWindow];
  [NSApp activateIgnoringOtherApps:YES];
  [m_window setTitle:@"YG Dolphin"];
  ObserveOcclusion();

  SetupMenu();
  InputCommon::Shortcuts::Get().SetPolling(IsEmbedded());
  Orca::Status::SetShortcutsAvailable(true);

  return true;
}

void PlatformMacOS::SetTitle(const std::string& title)
{
  @autoreleasepool
  {
    NSWindow* window = m_window;
    NSString* str = [NSString stringWithUTF8String:title.c_str()];
    dispatch_async(dispatch_get_main_queue(), ^{
      [window setTitle:str];
    });
  }
}

void PlatformMacOS::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    if (IsEmbedded())
      TrackParent();
    else
      UpdateWindowPosition();
    UpdateSurfaceVisible();
  }
}

void PlatformMacOS::ObserveOcclusion()
{
  if (m_occlusion_observer)
    return;
  // React right away, even while the main loop waits for events.
  m_occlusion_observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowDidChangeOcclusionStateNotification
                  object:m_window
                   queue:nil
              usingBlock:^(NSNotification*) {
                UpdateSurfaceVisible();
              }];
  UpdateSurfaceVisible();
}

void PlatformMacOS::UpdateSurfaceVisible()
{
  // When embedded, check m_embed_on_screen too: the occlusion state lags behind ordering out.
  const bool visible = (!IsEmbedded() || m_embed_on_screen) &&
                       ([m_window occlusionState] & NSWindowOcclusionStateVisible) != 0;
  if (visible != m_surface_visible)
  {
    m_surface_visible = visible;
    VideoCommon::Presenter::SetSurfaceVisible(visible);
    NOTICE_LOG_FMT(VIDEO, "Orca: the game's window is {}", visible ? "visible" : "out of sight");
  }

  // Report the screen's refresh rate to the video backend; below 59 Hz it can't keep up with the
  // game. Polled every pass because it is cheap and catches screen moves and mode changes. A window
  // with no screen keeps the last value.
  int hz = m_surface_refresh_rate < 0 ? 0 : m_surface_refresh_rate;
  if (@available(macOS 12.0, *))
  {
    if (NSScreen* screen = [m_window screen])
      hz = static_cast<int>([screen maximumFramesPerSecond]);
  }
  if (hz != m_surface_refresh_rate)
  {
    m_surface_refresh_rate = hz;
    VideoCommon::Presenter::SetSurfaceRefreshRate(hz);
    NOTICE_LOG_FMT(VIDEO, "Orca: the game's screen shows up to {} frames a second", hz);
  }
}

void PlatformMacOS::BeginGameActivity()
{
  if (!Orca::SessionActive() || m_game_activity)
    return;
  m_game_activity = [[NSProcessInfo processInfo]
      beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical)
                        reason:@"YG Dolphin game"];
  if (m_game_activity)
    NOTICE_LOG_FMT(CORE, "Orca: game activity holds user-initiated, latency-critical priority");
}

bool PlatformMacOS::InitEmbedded()
{
  BeginGameActivity();
  [Application sharedApplication];
  m_app_delegate = [[AppDelegate alloc] initWithPlatform:this];
  [NSApp setDelegate:m_app_delegate];
  // No Dock icon, app switcher entry or menu bar: Orca is part of the YouGame app.
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  [NSApp setPlatform:this];
  FinishLaunching();

  const Embed::Rect& r = m_embed.rect;
  m_window = [[EmbedWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, r.w, r.h)
                styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                  backing:NSBackingStoreBuffered
                    defer:NO];
  if (m_window == nil)
  {
    EmbedFallback("track-failed");
    return Init();
  }
  [m_window setReleasedWhenClosed:NO];
  // NSPanel must follow the host even when another app is active, like the old borderless window.
  [(NSPanel*)m_window setHidesOnDeactivate:NO];
  [m_window setOpaque:YES];
  [m_window setBackgroundColor:[NSColor blackColor]];
  [m_window setHasShadow:NO];
  [m_window setIgnoresMouseEvents:YES];
  [m_window setAnimationBehavior:NSWindowAnimationBehaviorNone];
  [m_window setLevel:NSNormalWindowLevel];
  // Join only the active desktop Space. The host uses simple full screen; this panel must never
  // accompany another app into a native full-screen Space.
  [m_window setCollectionBehavior:NSWindowCollectionBehaviorMoveToActiveSpace |
                                  NSWindowCollectionBehaviorIgnoresCycle |
                                  NSWindowCollectionBehaviorTransient];
  [m_window setTitle:@"YG Dolphin"];
  m_window_delegate = [[WindowDelegate alloc] init];
  [m_window setDelegate:m_window_delegate];
  [[m_window contentView] setWantsLayer:YES];

  m_window_focus = false;
  InputCommon::Shortcuts::Get().FocusLost();
  m_embed_visible = !m_embed.too_small;  // Hidden until a big enough rect (Embed.h).
  m_activation_observer = [NSWorkspace.sharedWorkspace.notificationCenter
      addObserverForName:NSWorkspaceDidActivateApplicationNotification
                  object:nil
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification* notification) {
                NSRunningApplication* app = notification.userInfo[NSWorkspaceApplicationKey];
                if (IsEmbedded() && m_parent_owner != 0 && app.processIdentifier == m_parent_owner)
                {
                  m_above_query.Reset();
                  TrackParent();
                }
              }];
  // A window the app just created may not be in the window list yet.
  Embed::Mac::StartupBackoff backoff;
  while (TrackParent(true) && m_parent_missing_since)
    std::this_thread::sleep_for(backoff.Next());
  if (!IsEmbedded())
    return true;
  if (!IsRunning())
    return false;
  ObserveOcclusion();
  return true;
}

std::optional<Embed::WindowBounds> PlatformMacOS::EmbedGetBounds() const
{
  @autoreleasepool
  {
    if (!m_window)
      return std::nullopt;
    const NSRect frame = [m_window frame];
    return Embed::WindowBounds{frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
                               [m_window backingScaleFactor]};
  }
}

void PlatformMacOS::EmbedSetRect(const Embed::Rect&)
{
  if (IsEmbedded())
    TrackParent(true);
}

void PlatformMacOS::EmbedSetVisible(bool visible)
{
  if (!IsEmbedded())
    return;
  m_embed_visible = visible;
  if (!visible)
  {
    m_parent_settle.Observe(std::chrono::steady_clock::now(), false, std::nullopt);
    m_settle_wait_logged = false;
  }
  TrackParent(true);
  UpdateSurfaceVisible();
}

void PlatformMacOS::EmbedSetFocus(bool focus)
{
  if (!IsEmbedded())
    return;
  m_embed_focus = focus;
  TrackParent(true);
}

bool PlatformMacOS::TrackParent(bool force)
{
  @autoreleasepool
  {
    const CGWindowID parent = static_cast<CGWindowID>(m_embed.parent);
    const auto now = std::chrono::steady_clock::now();
    if (force)
      m_above_query.Request();
    if (force ? m_parent_query.Force(now) : m_parent_query.Due(now))
    {
      bool exists = false;
      bool on_screen = false;
      bool closed = false;
      pid_t owner = 0;
      CGRect bounds = CGRectZero;  // Points, origin at the main display's top left.
      if (CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, parent))
      {
        if (CFArrayGetCount(list) > 0)
        {
          auto* const info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0));
          auto* const number =
              static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowNumber));
          int id = 0;
          if (number && CFNumberGetValue(number, kCFNumberIntType, &id) &&
              static_cast<CGWindowID>(id) == parent)
          {
            exists = true;
            auto* const onscreen =
                static_cast<CFBooleanRef>(CFDictionaryGetValue(info, kCGWindowIsOnscreen));
            on_screen = onscreen && CFBooleanGetValue(onscreen);
            // A closed window can linger in the list, fully transparent. Treat it as gone.
            auto* const alpha =
                static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowAlpha));
            double alpha_value = 1;
            if (alpha && CFNumberGetValue(alpha, kCFNumberDoubleType, &alpha_value) &&
                alpha_value <= 0)
            {
              exists = false;
              closed = true;
            }
            auto* const pid =
                static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowOwnerPID));
            if (pid)
              CFNumberGetValue(pid, kCFNumberIntType, &owner);
            auto* const rect =
                static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
            if (!rect || !CGRectMakeWithDictionaryRepresentation(rect, &bounds))
              exists = false;
          }
        }
        CFRelease(list);
      }
      // On macOS 26 a minimized window is missing from that query but still in the full list.
      if (!exists && !closed)
      {
        if (CFArrayRef all = CGWindowListCreate(kCGWindowListOptionAll, kCGNullWindowID))
        {
          const CFIndex count = CFArrayGetCount(all);
          for (CFIndex i = 0; i < count && !exists; ++i)
          {
            exists = static_cast<CGWindowID>(
                         reinterpret_cast<uintptr_t>(CFArrayGetValueAtIndex(all, i))) == parent;
          }
          CFRelease(all);
        }
        on_screen = false;
      }
      m_parent_exists = exists;
      m_parent_on_screen = on_screen;
      m_parent_owner = owner;
      m_parent_bounds = bounds;
      std::optional<Embed::Mac::SettleGate::Bounds> settle_bounds;
      if (exists)
        settle_bounds = Embed::Mac::SettleGate::Bounds{bounds.origin.x, bounds.origin.y,
                                                       bounds.size.width, bounds.size.height};
      // Time spent waiting for WindowServer is not evidence that the returned bounds were stable.
      m_parent_settle.Observe(std::chrono::steady_clock::now(), m_embed_visible && on_screen,
                              settle_bounds);
    }

    if (!m_parent_exists)
    {
      if (m_embed_on_screen)
        [m_window orderOut:nil];
      m_embed_on_screen = false;
      m_settle_wait_logged = false;
      m_window_focus = false;
      InputCommon::Shortcuts::Get().FocusLost();
      // The app's window is gone without a "quit". Stop rather than run unseen with sound on.
      if (!m_parent_missing_since)
        m_parent_missing_since = now;
      if (now - *m_parent_missing_since < std::chrono::seconds(2))
        return true;
      if (!m_embed_placed)
      {
        EmbedFallback("parent-gone");
        // Keep the same render surface even if this happens after a hidden boot.
        // Replacing the mask also removes NonactivatingPanel for the regular-window fallback.
        [m_window setStyleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                               NSWindowStyleMaskResizable];
        [m_window setIgnoresMouseEvents:NO];
        [m_window setCollectionBehavior:NSWindowCollectionBehaviorDefault];
        [m_window setFrame:NSMakeRect(m_window_x, m_window_y, m_window_width, m_window_height)
                   display:YES];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [m_window makeKeyAndOrderFront:NSApp];
        [m_window makeMainWindow];
        [NSApp activateIgnoringOtherApps:YES];
        [[NSNotificationCenter defaultCenter] addObserver:NSApp
                                                 selector:@selector(shutdown)
                                                     name:NSWindowWillCloseNotification
                                                   object:m_window];
        SetupMenu();
        ObserveOcclusion();
        return false;
      }
      if (IsRunning())
        fprintf(stderr, "Orca: the app's window %u is gone, stopping\n", parent);
      Stop();
      return false;
    }
    m_parent_missing_since.reset();

    // Apply rect commands immediately using cached bounds, even between WindowServer queries.
    const CGRect bounds = m_parent_bounds;
    bool show = m_embed_visible && m_parent_on_screen;
    if (!show)
      m_settle_wait_logged = false;
    // MainLoop polls TrackParent even without new events. Only ordering in waits for settling;
    // movement while visible and ordering out retain their immediate behavior.
    if (show && !m_embed_on_screen && !m_parent_settle.Ready(now))
    {
      if (!m_settle_wait_logged)
      {
        fprintf(stderr, "Orca: waiting for the app's window to settle\n");
        m_settle_wait_logged = true;
      }
      show = false;
    }
    if (show)
    {
      // The rect is in pixels from the top left of the app window's frame, title bar included.
      // Convert with the backing scale of the screen holding most of the window, the same screen
      // Electron's getDisplayMatching picks.
      NSArray<NSScreen*>* const screens = [NSScreen screens];
      const CGFloat primary_height = screens.count ? screens[0].frame.size.height : 0;
      const NSRect cocoa_bounds =
          NSMakeRect(bounds.origin.x, primary_height - bounds.origin.y - bounds.size.height,
                     bounds.size.width, bounds.size.height);
      CGFloat scale = screens.count ? screens[0].backingScaleFactor : 1;
      CGFloat best_area = 0;
      for (NSScreen* screen in screens)
      {
        const NSRect overlap = NSIntersectionRect(cocoa_bounds, screen.frame);
        const CGFloat area = overlap.size.width * overlap.size.height;
        if (area > best_area)
        {
          best_area = area;
          scale = screen.backingScaleFactor;
        }
      }

      const Embed::Rect& r = m_embed.rect;
      const CGFloat w = r.w / scale;
      const CGFloat h = r.h / scale;
      const CGFloat top = bounds.origin.y + r.y / scale;
      const NSRect frame =
          NSMakeRect(bounds.origin.x + r.x / scale, primary_height - top - h, w, h);
      if (!NSEqualRects([m_window frame], frame))
        [m_window setFrame:frame display:YES animate:NO];

      // If the app's window got above ours (the app was brought to the front), move back directly
      // above it. Anything else over the app's window, like its menus, stays over Orca.
      bool below = !m_embed_on_screen || ![m_window isVisible];
      if (!below && m_above_query.Due(now))
      {
        if (CFArrayRef above = CGWindowListCreate(kCGWindowListOptionOnScreenAboveWindow,
                                                  static_cast<CGWindowID>([m_window windowNumber])))
        {
          const CFIndex count = CFArrayGetCount(above);
          for (CFIndex i = 0; i < count; ++i)
          {
            if (static_cast<CGWindowID>(
                    reinterpret_cast<uintptr_t>(CFArrayGetValueAtIndex(above, i))) == parent)
            {
              below = true;
              break;
            }
          }
          CFRelease(above);
        }
      }
      if (below)
      {
        [m_window orderWindow:NSWindowAbove relativeTo:static_cast<NSInteger>(parent)];
        m_embed_on_screen = true;
        m_settle_wait_logged = false;
      }
      if ([m_window isVisible])
      {
        m_embed_placed = true;
        EmbedSucceeded();
      }
    }
    else if (m_embed_on_screen)
    {
      [m_window orderOut:nil];
      m_embed_on_screen = false;
    }

    // Input counts only while the app is in front, shows the game and gave it focus.
    const bool app_in_front =
        m_parent_owner != 0 &&
        NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == m_parent_owner;
    m_window_focus = show && m_embed_focus && app_in_front;
    InputCommon::Shortcuts::Get().SetFocus(m_window_focus);
  }
  return true;
}

WindowSystemInfo PlatformMacOS::GetWindowSystemInfo() const
{
  @autoreleasepool
  {
    WindowSystemInfo wsi;
    wsi.type = WindowSystemType::MacOS;
    wsi.render_window = (void*)CFBridgingRetain([m_window contentView]);
    wsi.render_surface = wsi.render_window;
    return wsi;
  }
}

void PlatformMacOS::ProcessEvents()
{
  @autoreleasepool
  {
    // Track each native control separately. The union below only controls event passthrough.
    // The press state comes from this window's own mouse events (m_left_down), not
    // +[NSEvent pressedMouseButtons]: that asks WindowServer on every pass (0.3.56's budget).
    if (IsEmbedded() || Orca::UX::YgOrb::HostEnabled())
    {
      NSView* view = [m_window contentView];
      const NSPoint point = [view convertPoint:[m_window mouseLocationOutsideOfEventStream]
                                      fromView:nil];
      const NSPoint pixel = [view convertPointToBacking:point];
      const NSRect bounds = [view convertRectToBacking:[view bounds]];
      Orca::UX::YgOrb::UpdateControlsPointer(pixel.x, bounds.size.height - pixel.y, m_left_down);
      const bool hit = Orca::UX::YgOrb::HitControls(pixel.x, bounds.size.height - pixel.y);
      if (IsEmbedded() && hit != m_embed_controls_hit)
      {
        m_embed_controls_hit = hit;
        [m_window setIgnoresMouseEvents:!hit];
      }
    }
    // The game renders on its own thread; this loop only follows the host window.
    NSDate* expiration = [NSDate
        dateWithTimeIntervalSinceNow:IsEmbedded() || Orca::UX::YgOrb::HostEnabled() ? 1.0 / 60 : 1];
    NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:expiration
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];

    bool handled = false;
    if (event && [event window] == m_window)
    {
      namespace Orb = Orca::UX::YgOrb;
      auto& shortcuts = InputCommon::Shortcuts::Get();
      shortcuts.SetFocus(IsEmbedded() ? IsWindowFocused() :
                                        ([m_window isKeyWindow] && [NSApp isActive]));
      const auto type = [event type];
      if (type == NSEventTypeKeyDown || type == NSEventTypeKeyUp || type == NSEventTypeFlagsChanged)
      {
        const auto flags = [event modifierFlags];
        const unsigned mods =
            ((flags & NSEventModifierFlagShift) ? InputCommon::Shortcuts::Shift : 0u) |
            ((flags & NSEventModifierFlagControl) ? InputCommon::Shortcuts::Ctrl : 0u) |
            ((flags & NSEventModifierFlagOption) ? InputCommon::Shortcuts::Alt : 0u) |
            ((flags & NSEventModifierFlagCommand) ? 8u : 0u);
        const auto key = ShortcutKey([event keyCode]);
        const bool down = type == NSEventTypeFlagsChanged ?
                              ((key == "Shift" && (mods & InputCommon::Shortcuts::Shift)) ||
                               (key == "Ctrl" && (mods & InputCommon::Shortcuts::Ctrl)) ||
                               (key == "Alt" && (mods & InputCommon::Shortcuts::Alt))) :
                              type == NSEventTypeKeyDown;
        handled =
            shortcuts.KeyEvent(key, mods, down, type == NSEventTypeKeyDown && [event isARepeat]);
      }
      if (!handled &&
          ([event type] == NSEventTypeLeftMouseDown || [event type] == NSEventTypeLeftMouseUp))
      {
        NSView* view = [m_window contentView];
        const NSPoint point = [view convertPoint:[event locationInWindow] fromView:nil];
        const NSPoint pixel = [view convertPointToBacking:point];
        const NSRect bounds = [view convertRectToBacking:[view bounds]];
        const bool down = [event type] == NSEventTypeLeftMouseDown;
        m_left_down = down;
        Orb::UpdateControlsPointer(pixel.x, bounds.size.height - pixel.y, down);
        handled = down ? Orb::ClickControls(pixel.x, bounds.size.height - pixel.y,
                                            IsEmbedded() ? OutKey : nullptr) :
                         Orb::HitControls(pixel.x, bounds.size.height - pixel.y);
      }
      // The embedded app owns keyboard focus and relays feedback / host-menu on stdin.
      else if (!handled && !IsEmbedded() && [event type] == NSEventTypeKeyDown &&
               ![event isARepeat])
      {
        switch ([event keyCode])
        {
        case kVK_F8:
          handled = Orb::HandleHostKey(Orb::HostKey::Feedback);
          break;
        case kVK_Tab:
          if (!InputCommon::Shortcuts::Get().Configured() && Orb::HostEnabled() &&
              ([event modifierFlags] & NSEventModifierFlagShift) &&
              !([event modifierFlags] & (NSEventModifierFlagControl | NSEventModifierFlagOption |
                                         NSEventModifierFlagCommand)))
          {
            Orca::UX::OverlayKey::Emit(
                Orca::UX::OverlayKey::Keys().Down(Orca::Status::Cap(Orca::Status::TALK_CAP)));
            handled = true;
          }
          break;
        }
      }
      else if (!IsEmbedded() && (([event type] == NSEventTypeKeyUp && [event keyCode] == kVK_Tab) ||
                                 ([event type] == NSEventTypeFlagsChanged &&
                                  !([event modifierFlags] & NSEventModifierFlagShift))))
      {
        Orca::UX::OverlayKey::Emit(Orca::UX::OverlayKey::Keys().Up());
      }
    }
    if (event && !handled)
      [NSApp sendEvent:event];
    InputCommon::Shortcuts::Get().SetFocus(
        IsEmbedded() ? IsWindowFocused() : ([m_window isKeyWindow] && [NSApp isActive]));
    if (IsEmbedded())
      return;
    if (![m_window isKeyWindow] || ![NSApp isActive])
      Orca::UX::OverlayKey::Emit(Orca::UX::OverlayKey::Keys().Cancel());

    // Need to update if m_window becomes fullscreen
    m_window_fullscreen = [m_window styleMask] & NSWindowStyleMaskFullScreen;

    if ([m_window isMainWindow])
    {
      m_window_focus = true;
      if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never &&
          Core::GetState(Core::System::GetInstance()) != Core::State::Paused)
      {
        [NSCursor unhide];
      }
    }
    else
    {
      m_window_focus = false;
      InputCommon::Shortcuts::Get().FocusLost();
      if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never)
        [NSCursor hide];
    }
  }
}

void PlatformMacOS::UpdateWindowPosition()
{
  if (m_window_fullscreen)
    return;

  NSRect win = [m_window frame];
  m_window_x = win.origin.x;
  m_window_y = win.origin.y;
  m_window_width = win.size.width;
  m_window_height = win.size.height;
}

void PlatformMacOS::SetupMenu()
{
  @autoreleasepool
  {
    menuBar = [NSMenu new];

    NSMenu* appMenu = [NSMenu new];
    NSMenu* stateMenu = [[NSMenu alloc] initWithTitle:@"States"];
    NSMenu* loadStateMenu = [[NSMenu alloc] initWithTitle:@"Load"];
    NSMenu* saveStateMenu = [[NSMenu alloc] initWithTitle:@"Save"];
    NSMenu* miscMenu = [[NSMenu alloc] initWithTitle:@"Misc"];

    NSMenuItem* appMenuItem = [NSMenuItem new];
    NSMenuItem* miscMenuItem = [NSMenuItem new];
    NSMenuItem* stateMenuItem = [NSMenuItem new];
    NSMenuItem* loadStateItem = [[NSMenuItem alloc] initWithTitle:@"Load"
                                                           action:nil
                                                    keyEquivalent:@""];
    NSMenuItem* saveStateItem = [[NSMenuItem alloc] initWithTitle:@"Save"
                                                           action:nil
                                                    keyEquivalent:@""];
    [menuBar addItem:appMenuItem];
    // Orca: sessions refuse save states, so hide the menu.
    if (!Orca::SessionActive())
      [menuBar addItem:stateMenuItem];
    [menuBar addItem:miscMenuItem];

    // Quit
    NSString* quitTitle = @"Quit YG Dolphin";
    NSMenuItem* quitMenuItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                          action:@selector(shutdown)
                                                   keyEquivalent:@"q"];

    // Fullscreen
    NSString* fullScreenItemTitle = @"Toggle Fullscreen";
    NSMenuItem* fullScreenItem = [[NSMenuItem alloc] initWithTitle:fullScreenItemTitle
                                                            action:@selector(toggleFullScreen:)
                                                     keyEquivalent:@"f"];
    [fullScreenItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];

    // Screenshot
    NSString* ScreenShotTitle = @"Take Screenshot";
    unichar c = NSF9FunctionKey;
    NSString* f9 = [NSString stringWithCharacters:&c length:1];
    NSMenuItem* ScreenShotItem = [[NSMenuItem alloc] initWithTitle:ScreenShotTitle
                                                            action:@selector(saveScreenShot)
                                                     keyEquivalent:f9];
    [ScreenShotItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];

    // Pause game
    NSString* pauseTitle = @"Toggle pause";
    c = NSF10FunctionKey;
    NSString* f10 = [NSString stringWithCharacters:&c length:1];
    NSMenuItem* pauseItem = [[NSMenuItem alloc] initWithTitle:pauseTitle
                                                       action:@selector(togglePause)
                                                keyEquivalent:f10];
    [pauseItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];

    // Load last save
    NSString* loadLastTitle = @"Load Last Saved";
    c = NSF11FunctionKey;
    NSString* f11 = [NSString stringWithCharacters:&c length:1];
    NSMenuItem* loadLastItem = [[NSMenuItem alloc] initWithTitle:loadLastTitle
                                                          action:@selector(loadLastSaved)
                                                   keyEquivalent:f11];
    [loadLastItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];

    // Undo Load State
    NSString* undoLoadTitle = @"Undo Load";
    c = NSF12FunctionKey;
    NSString* f12 = [NSString stringWithCharacters:&c length:1];
    NSMenuItem* undoLoadItem = [[NSMenuItem alloc] initWithTitle:undoLoadTitle
                                                          action:@selector(undoLoadState)
                                                   keyEquivalent:f12];
    [undoLoadItem setKeyEquivalentModifierMask:NSEventModifierFlagShift];

    // Undo Save State
    NSString* undoSaveTitle = @"Undo Save";
    NSMenuItem* undoSaveItem = [[NSMenuItem alloc] initWithTitle:undoSaveTitle
                                                          action:@selector(undoSaveState)
                                                   keyEquivalent:f12];
    [undoSaveItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];

    // Load and Save States
    for (unichar i = NSF1FunctionKey; i <= NSF8FunctionKey; i++)
    {
      NSInteger stateNum = i - NSF1FunctionKey + 1;
      NSString* lstateTitle = [NSString stringWithFormat:@"Load State %ld", (long)stateNum];
      c = i;
      NSString* t = [NSString stringWithCharacters:&c length:1];
      NSMenuItem* lstateItem = [[NSMenuItem alloc] initWithTitle:lstateTitle
                                                          action:@selector(loadState:)
                                                   keyEquivalent:t];
      [lstateItem setTag:stateNum];
      [lstateItem setKeyEquivalentModifierMask:NSEventModifierFlagFunction];
      [loadStateMenu addItem:lstateItem];

      NSString* sstateTitle = [NSString stringWithFormat:@"Save State %ld", (long)stateNum];
      c = i;
      NSMenuItem* sstateItem = [[NSMenuItem alloc] initWithTitle:sstateTitle
                                                          action:@selector(saveState:)
                                                   keyEquivalent:t];
      [sstateItem setKeyEquivalentModifierMask:NSEventModifierFlagShift];
      [sstateItem setTag:stateNum];
      [saveStateMenu addItem:sstateItem];
    }

    // App Main menu
    [appMenu addItem:quitMenuItem];

    // State Menu
    [loadStateItem setSubmenu:loadStateMenu];
    [saveStateItem setSubmenu:saveStateMenu];

    [stateMenu addItem:loadLastItem];
    [stateMenu addItem:undoLoadItem];
    [stateMenu addItem:undoSaveItem];
    [stateMenu addItem:loadStateItem];
    [stateMenu addItem:saveStateItem];

    // Misc Menu
    [miscMenu addItem:fullScreenItem];
    [miscMenu addItem:ScreenShotItem];
    [miscMenu addItem:pauseItem];

    [appMenuItem setSubmenu:appMenu];
    [stateMenuItem setSubmenu:stateMenu];
    [miscMenuItem setSubmenu:miscMenu];

    [NSApp setMainMenu:menuBar];
  }
}

}  // namespace

std::unique_ptr<Platform> Platform::CreateMacOSPlatform()
{
  return std::make_unique<PlatformMacOS>();
}

namespace
{
// The dialogs run before PlatformMacOS::Init, so they create the shared application (as the
// Application class Init expects) and bring this process to the front.
void BringToFront()
{
  [Application sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  FinishLaunching();
  // Copy and paste in the panel's text fields need an Edit menu. Init replaces this menu.
  if ([NSApp mainMenu] == nil || [[NSApp mainMenu] numberOfItems] == 0)
  {
    NSMenu* bar = [NSMenu new];
    NSMenuItem* app_item = [NSMenuItem new];
    NSMenu* app_menu = [NSMenu new];
    [app_menu addItemWithTitle:@"Quit YG Dolphin" action:@selector(terminate:) keyEquivalent:@"q"];
    [app_item setSubmenu:app_menu];
    [bar addItem:app_item];
    NSMenuItem* edit_item = [NSMenuItem new];
    NSMenu* edit_menu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit_menu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit_menu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit_menu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit_menu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    [edit_item setSubmenu:edit_menu];
    [bar addItem:edit_item];
    [NSApp setMainMenu:bar];
  }
  [NSApp activateIgnoringOtherApps:YES];
}
}  // namespace

std::optional<std::string> Platform::ChooseFileMacOS(const std::string& message)
{
  @autoreleasepool
  {
    BringToFront();
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];
    [panel setTitle:@"YG Dolphin"];
    [panel setMessage:[NSString stringWithUTF8String:message.c_str()]];
    [panel setPrompt:@"Choose"];
    // Stay above other windows even if macOS won't let this process take focus from the app.
    [panel setLevel:NSModalPanelWindowLevel];
    if ([panel runModal] != NSModalResponseOK || panel.URL == nil || panel.URL.path == nil)
      return std::nullopt;
    return std::string(panel.URL.path.UTF8String);
  }
}

void Platform::ShowErrorMacOS(const std::string& title, const std::string& message)
{
  @autoreleasepool
  {
    BringToFront();
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:[NSString stringWithUTF8String:title.c_str()]];
    [alert setInformativeText:[NSString stringWithUTF8String:message.c_str()]];
    [alert addButtonWithTitle:@"OK"];
    [[alert window] setLevel:NSModalPanelWindowLevel];
    [alert runModal];
  }
}
