// embed-test-host: plays the YouGame desktop app's part for `Orca --embed` on macOS (ORCA.md
// "Embedding"). A titled window like Electron's default, with a player box drawn in it; Orca is
// started with the window's number and the box's rectangle and steered over its stdin.
//
//   clang -fobjc-arc -framework AppKit -o embed-test-host Tools/orca/embed-test-host.m
//   ./embed-test-host <orca binary> [orca arguments...]  < commands
//
// Commands, one per line on this program's stdin:
//   move X Y        the window's top left, in global display points (as screencapture -R)
//   size W H        the window's content size, in points (the box follows: rect sent to Orca)
//   mini | unmini   minimize / restore the window
//   front           bring this program to the front
//   where           print the window frame and the box, in global display points
//   panel N         the YouGame menu's panel over the box's left N points, as the page does it:
//                   `view <box>` then `rect <box less the panel>`; `panel 0`: `view <box>` and
//                   `rect <box>`. After the first `panel` every resize sends both, as the page
//                   does (the page's `dim` goes with `send`)
//   send LINE       a raw line for Orca's stdin (rect, pause, hide, show, focus, blur, quit...)
//   close           close Orca's stdin (the app died)
//   closewin        close the window but keep Orca's stdin open
//   exit            quit this program (Orca's stdin closes with it)
// Orca's stdout is echoed to this program's stdout, each line prefixed with "orca> ". Mouse presses
// in the window (the page's clicks, the box included) print as "host> mouse-down ...".
#import <AppKit/AppKit.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <unistd.h>

extern char** environ;

static NSWindow* g_window;
static FILE* g_to_orca;
static pid_t g_orca;

// The player box: a margin around it, 16:9 in the content area below a 40 pt "header".
static NSRect BoxInContent(void)
{
  const NSSize c = [[g_window contentView] bounds].size;
  const CGFloat margin = 20, header = 40;
  CGFloat w = c.width - 2 * margin;
  CGFloat h = w * 9 / 16;
  if (h > c.height - header - margin)
  {
    h = c.height - header - margin;
    w = h * 16 / 9;
  }
  return NSMakeRect(margin, header, w, h);  // top-left origin
}

// The points of the box's left the menu's panel covers (`panel N`; 0: shut).
static CGFloat g_panel = 0;
// Whether a `panel` has sent a `view`: Orca keeps the last one across later rects, so from then on
// every rect goes with the box's view, as the page sends them.
static BOOL g_view_sent = NO;

// What the app sends: the box in the window's pixels, from the top left of its frame, less `cut`
// points on its left.
static NSString* BoxLine(CGFloat cut)
{
  const CGFloat scale = [g_window backingScaleFactor];
  const NSRect frame = [g_window frame];
  const CGFloat inset = frame.size.height - [g_window contentRectForFrameRect:frame].size.height;
  const NSRect b = BoxInContent();
  cut = MIN(MAX(cut, 0), b.size.width);
  return [NSString stringWithFormat:@"%d %d %d %d", (int)lround((b.origin.x + cut) * scale),
                                    (int)lround((b.origin.y + inset) * scale),
                                    (int)lround((b.size.width - cut) * scale),
                                    (int)lround(b.size.height * scale)];
}

static NSString* RectLine(void)
{
  return BoxLine(0);
}

static void Send(NSString* line)
{
  if (!g_to_orca)
    return;
  fprintf(g_to_orca, "%s\n", line.UTF8String);
  fflush(g_to_orca);
  fprintf(stdout, "host> %s\n", line.UTF8String);
  fflush(stdout);
}

static NSRect GlobalTopLeft(NSRect cocoa)
{
  const CGFloat primary = [NSScreen screens][0].frame.size.height;
  return NSMakeRect(cocoa.origin.x, primary - cocoa.origin.y - cocoa.size.height,
                    cocoa.size.width, cocoa.size.height);
}

@interface BoxView : NSView
@end
@implementation BoxView
- (BOOL)isFlipped
{
  return YES;
}
- (void)drawRect:(NSRect)dirty
{
  [[NSColor colorWithCalibratedWhite:0.12 alpha:1] setFill];
  NSRectFill(self.bounds);
  [[NSColor colorWithCalibratedRed:0.9 green:0.2 blue:0.6 alpha:1] setFill];
  NSRectFill(NSInsetRect(BoxInContent(), -3, -3));  // a magenta edge around the box
  [[NSColor colorWithCalibratedRed:0.1 green:0.6 blue:0.3 alpha:1] setFill];
  NSRectFill(BoxInContent());  // green: Orca should cover all of it
  [@"YouGame (embed test host)" drawAtPoint:NSMakePoint(20, 12)
                             withAttributes:@{NSForegroundColorAttributeName : NSColor.whiteColor}];
}
// A press in this window, as the page would get it. Orca's view lets clicks through to the window
// under it (ORCA.md "Focus"), so a press on the game's picture shows up here "in the box".
- (void)logPress:(NSEvent*)event
{
  const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  printf("host> mouse-down button %ld at %.0f,%.0f, %s the box\n", (long)event.buttonNumber, p.x,
         p.y, NSPointInRect(p, BoxInContent()) ? "in" : "outside");
  fflush(stdout);
}
- (void)mouseDown:(NSEvent*)event
{
  [self logPress:event];
}
- (void)rightMouseDown:(NSEvent*)event
{
  [self logPress:event];
}
- (void)otherMouseDown:(NSEvent*)event
{
  [self logPress:event];
}
@end

@interface Delegate : NSObject <NSWindowDelegate>
@end
@implementation Delegate
- (void)windowDidResize:(NSNotification*)n
{
  [[g_window contentView] setNeedsDisplay:YES];
  // As the page: the game laid out in the whole box (`view`, after the first `panel`), the window
  // cut by the panel while it is open.
  if (g_view_sent)
    Send([@"view " stringByAppendingString:BoxLine(0)]);
  Send([@"rect " stringByAppendingString:BoxLine(g_panel)]);
}
@end

static void Command(NSString* line)
{
  NSArray<NSString*>* p = [line componentsSeparatedByString:@" "];
  NSString* cmd = p.firstObject;
  if ([cmd isEqualToString:@"move"] && p.count == 3)
  {
    const CGFloat primary = [NSScreen screens][0].frame.size.height;
    [g_window setFrameTopLeftPoint:NSMakePoint(p[1].doubleValue, primary - p[2].doubleValue)];
  }
  else if ([cmd isEqualToString:@"size"] && p.count == 3)
  {
    [g_window setContentSize:NSMakeSize(p[1].doubleValue, p[2].doubleValue)];
  }
  else if ([cmd isEqualToString:@"mini"])
    [g_window miniaturize:nil];
  else if ([cmd isEqualToString:@"unmini"])
    [g_window deminiaturize:nil];
  else if ([cmd isEqualToString:@"front"])
  {
    [NSApp activateIgnoringOtherApps:YES];
    [g_window makeKeyAndOrderFront:nil];
    [g_window orderFrontRegardless];  // in front even when macOS refuses the activation
  }
  else if ([cmd isEqualToString:@"where"])
  {
    const NSRect f = GlobalTopLeft([g_window frame]);
    const NSRect content = GlobalTopLeft([g_window contentRectForFrameRect:[g_window frame]]);
    const NSRect b = BoxInContent();
    printf("where window %.0f,%.0f,%.0f,%.0f box %.0f,%.0f,%.0f,%.0f scale %.0f number %ld\n",
           f.origin.x, f.origin.y, f.size.width, f.size.height, content.origin.x + b.origin.x,
           content.origin.y + b.origin.y, b.size.width, b.size.height,
           [g_window backingScaleFactor], (long)[g_window windowNumber]);
    fflush(stdout);
  }
  else if ([cmd isEqualToString:@"panel"] && p.count == 2)
  {
    // As YouGame's OrcaEmbed measure(): the view is always the whole box, sent before the rect.
    g_panel = MAX(p[1].doubleValue, 0);
    g_view_sent = YES;
    Send([@"view " stringByAppendingString:BoxLine(0)]);
    Send([@"rect " stringByAppendingString:BoxLine(g_panel)]);
  }
  else if ([cmd isEqualToString:@"send"] && line.length > 5)
    Send([line substringFromIndex:5]);
  else if ([cmd isEqualToString:@"close"])
  {
    if (g_to_orca)
      fclose(g_to_orca);
    g_to_orca = NULL;
    printf("host> (stdin closed)\n");
    fflush(stdout);
  }
  else if ([cmd isEqualToString:@"closewin"])
  {
    [g_window setDelegate:nil];
    [g_window close];
    g_window = nil;  // released: gone from the window server, as Electron's would be
  }
  else if ([cmd isEqualToString:@"exit"])
    [NSApp terminate:nil];
}

int main(int argc, char** argv)
{
  @autoreleasepool
  {
    if (argc < 2)
    {
      fprintf(stderr, "usage: embed-test-host <orca> [args...] < commands\n");
      return 2;
    }
    // A line for an Orca that already exited fails instead of ending this program.
    signal(SIGPIPE, SIG_IGN);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    const NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    g_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 900, 600)
                                           styleMask:style
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    [g_window setTitle:@"YouGame"];
    [g_window setContentView:[[BoxView alloc] init]];
    static Delegate* delegate;
    delegate = [Delegate new];
    [g_window setDelegate:delegate];
    [g_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    // Orca: <orca> --embed --parent <window number> --rect X Y W H <args...>
    NSMutableArray<NSString*>* args = [NSMutableArray array];
    [args addObject:@(argv[1])];
    [args addObjectsFromArray:@[ @"--embed", @"--parent",
                                 [NSString stringWithFormat:@"%ld", (long)[g_window windowNumber]],
                                 @"--rect" ]];
    [args addObjectsFromArray:[RectLine() componentsSeparatedByString:@" "]];
    for (int i = 2; i < argc; ++i)
      [args addObject:@(argv[i])];
    int in_pipe[2], out_pipe[2];
    pipe(in_pipe);
    pipe(out_pipe);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    char** cargs = calloc(args.count + 1, sizeof(char*));
    for (NSUInteger i = 0; i < args.count; ++i)
      cargs[i] = strdup(args[i].UTF8String);
    printf("host> %s\n", [args componentsJoinedByString:@" "].UTF8String);
    fflush(stdout);
    if (posix_spawn(&g_orca, cargs[0], &actions, NULL, cargs, environ) != 0)
    {
      perror("posix_spawn");
      return 1;
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    g_to_orca = fdopen(in_pipe[1], "w");

    const CFAbsoluteTime start = CFAbsoluteTimeGetCurrent();
    const int from_orca = out_pipe[0];
    // Orca's lines, echoed with the time since launch.
    [NSThread detachNewThreadWithBlock:^{
      FILE* from = fdopen(from_orca, "r");
      char buf[4096];
      while (fgets(buf, sizeof buf, from))
      {
        printf("orca> [%6.2f] %s", CFAbsoluteTimeGetCurrent() - start, buf);
        fflush(stdout);
      }
      int status = 0;
      waitpid(g_orca, &status, 0);
      printf("host> orca exited: %s %d after %.2f s\n",
             WIFEXITED(status) ? "code" : "signal",
             WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status),
             CFAbsoluteTimeGetCurrent() - start);
      fflush(stdout);
    }];
    // Commands, run on the main thread.
    [NSThread detachNewThreadWithBlock:^{
      char buf[4096];
      while (fgets(buf, sizeof buf, stdin))
      {
        NSString* line = [[NSString stringWithUTF8String:buf]
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        dispatch_async(dispatch_get_main_queue(), ^{
          Command(line);
        });
      }
    }];
    [NSApp run];
  }
  return 0;
}
