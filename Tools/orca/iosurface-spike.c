// iosurface-spike: measures what option (b) of ORCA.md "Embedding" would cost on macOS: Orca
// rendering into IOSurfaces that the YouGame app shows in its own view. Two processes, as the app
// (consumer) and Orca (producer): the consumer checks in a bootstrap name (Chromium's rendezvous
// does the same), the producer looks it up, sends three 1920x1080 BGRA IOSurfaces as mach ports,
// then 600 "frame k is in surface i" messages at 60 Hz, each with a pixel it wrote into the surface.
//
//   clang -O2 -framework IOSurface -framework CoreFoundation -o iosurface-spike iosurface-spike.c
//   ./iosurface-spike
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <servers/bootstrap.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

enum
{
  SURFACES = 3,
  FRAMES = 600,
  W = 1920,
  H = 1080
};

typedef struct
{
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t surface;
  uint32_t index;
} SurfaceMsg;

typedef struct
{
  mach_msg_header_t header;
  uint32_t frame, surface;
  uint64_t sent;
} FrameMsg;

typedef struct
{
  SurfaceMsg msg;
  mach_msg_trailer_t trailer;
} SurfaceRcv;
typedef struct
{
  FrameMsg msg;
  mach_msg_trailer_t trailer;
} FrameRcv;

static double Micros(uint64_t ticks)
{
  static mach_timebase_info_data_t tb;
  if (!tb.denom)
    mach_timebase_info(&tb);
  return (double)ticks * tb.numer / tb.denom / 1000.0;
}

static int Cmp(const void* a, const void* b)
{
  const double x = *(const double*)a, y = *(const double*)b;
  return x < y ? -1 : x > y;
}

static int Producer(const char* name)
{
  mach_port_t server;
  if (bootstrap_look_up(bootstrap_port, name, &server) != KERN_SUCCESS)
  {
    fprintf(stderr, "producer: look_up failed\n");
    return 1;
  }
  IOSurfaceRef surfaces[SURFACES];
  const int bpe = 4;
  CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks);
  int w = W, h = H, b = bpe, fmt = 'BGRA';
  CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w),
              nh = CFNumberCreate(NULL, kCFNumberIntType, &h),
              nb = CFNumberCreate(NULL, kCFNumberIntType, &b),
              nf = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
  CFDictionarySetValue(props, kIOSurfaceWidth, nw);
  CFDictionarySetValue(props, kIOSurfaceHeight, nh);
  CFDictionarySetValue(props, kIOSurfaceBytesPerElement, nb);
  CFDictionarySetValue(props, kIOSurfacePixelFormat, nf);
  const uint64_t t0 = mach_absolute_time();
  for (uint32_t i = 0; i < SURFACES; ++i)
  {
    surfaces[i] = IOSurfaceCreate(props);
    SurfaceMsg m = {0};
    m.header.msgh_bits =
        MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    m.header.msgh_size = sizeof m;
    m.header.msgh_remote_port = server;
    m.body.msgh_descriptor_count = 1;
    m.surface.name = IOSurfaceCreateMachPort(surfaces[i]);
    m.surface.disposition = MACH_MSG_TYPE_MOVE_SEND;
    m.surface.type = MACH_MSG_PORT_DESCRIPTOR;
    m.index = i;
    if (mach_msg(&m.header, MACH_SEND_MSG, sizeof m, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
                 MACH_PORT_NULL) != KERN_SUCCESS)
      return 1;
  }
  fprintf(stderr, "producer: created and sent %d surfaces in %.0f us\n", SURFACES,
          Micros(mach_absolute_time() - t0));
  uint64_t next = mach_absolute_time();
  for (uint32_t f = 0; f < FRAMES; ++f)
  {
    const uint32_t s = f % SURFACES;
    IOSurfaceLock(surfaces[s], 0, NULL);
    ((uint32_t*)IOSurfaceGetBaseAddress(surfaces[s]))[0] = f;  // the "frame"
    IOSurfaceUnlock(surfaces[s], 0, NULL);
    FrameMsg m = {0};
    m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    m.header.msgh_size = sizeof m;
    m.header.msgh_remote_port = server;
    m.frame = f;
    m.surface = s;
    m.sent = mach_absolute_time();
    mach_msg(&m.header, MACH_SEND_MSG, sizeof m, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
             MACH_PORT_NULL);
    next += (uint64_t)(16666.0 / Micros(1000) * 1000);  // ~16.7 ms in ticks
    mach_wait_until(next);
  }
  return 0;
}

int main(int argc, char** argv)
{
  if (argc == 3 && strcmp(argv[1], "--producer") == 0)
    return Producer(argv[2]);

  char name[128];
  snprintf(name, sizeof name, "co.yougame.orca-spike.%d", getpid());
  mach_port_t port;
  const uint64_t c0 = mach_absolute_time();
  if (bootstrap_check_in(bootstrap_port, name, &port) != KERN_SUCCESS)
  {
    fprintf(stderr, "consumer: bootstrap_check_in failed\n");
    return 1;
  }
  fprintf(stderr, "consumer: checked in %s in %.0f us\n", name, Micros(mach_absolute_time() - c0));
  char* child_argv[] = {argv[0], "--producer", name, NULL};
  pid_t pid;
  const uint64_t s0 = mach_absolute_time();
  posix_spawn(&pid, argv[0], NULL, NULL, child_argv, environ);

  IOSurfaceRef surfaces[SURFACES] = {0};
  for (int i = 0; i < SURFACES; ++i)
  {
    SurfaceRcv r;
    if (mach_msg(&r.msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof r, port, 5000, MACH_PORT_NULL) !=
        KERN_SUCCESS)
    {
      fprintf(stderr, "consumer: no surface\n");
      return 1;
    }
    surfaces[r.msg.index] = IOSurfaceLookupFromMachPort(r.msg.surface.name);
    mach_port_deallocate(mach_task_self(), r.msg.surface.name);
  }
  fprintf(stderr, "consumer: %d surfaces mapped %.0f us after spawn (%dx%d)\n", SURFACES,
          Micros(mach_absolute_time() - s0), (int)IOSurfaceGetWidth(surfaces[0]),
          (int)IOSurfaceGetHeight(surfaces[0]));

  static double lat[FRAMES];
  int bad = 0;
  for (int f = 0; f < FRAMES; ++f)
  {
    FrameRcv r;
    if (mach_msg(&r.msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof r, port, 5000, MACH_PORT_NULL) !=
        KERN_SUCCESS)
      return 1;
    lat[f] = Micros(mach_absolute_time() - r.msg.sent);
    IOSurfaceRef s = surfaces[r.msg.surface];
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    if (((uint32_t*)IOSurfaceGetBaseAddress(s))[0] != r.msg.frame)
      bad++;
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
  }
  qsort(lat, FRAMES, sizeof lat[0], Cmp);
  printf("frame message latency over %d frames: median %.1f us, p99 %.1f us, max %.1f us; "
         "pixel mismatches %d\n",
         FRAMES, lat[FRAMES / 2], lat[FRAMES * 99 / 100], lat[FRAMES - 1], bad);
  int status;
  waitpid(pid, &status, 0);
  return 0;
}
