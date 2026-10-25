// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

// What Orca says on stderr as it fails, so the app's crash report (desktop orca-crash.ts, which
// sends the run's stderr) says why. Nothing here may throw: it runs where a C++ exception can't
// unwind. On Windows that is anything the CPU thread calls from JIT code (an invalid access's
// panic alert, a CoreTiming event): the JIT registers no unwind tables, so the exception can't be
// dispatched and Windows ends Orca with 0xE06D7363 at once, without std::terminate and without a
// word on stderr (bug report b5ef88b5, Orca 0.3.29, 2026-10-07).
namespace Orca::CrashNote
{
// Writes `text` and a newline to `out` and flushes it. Unlike fmt::print, which throws
// fmt::system_error when the stream takes fewer bytes than it was given, it never throws; returns
// false when the write failed.
bool WriteLine(std::FILE* out, std::string_view text) noexcept;

// A thrown type as written in MSVC's throw info (".?AVsystem_error@std@@"), readable
// ("std::system_error"). Names it can't read (templates, other forms) come back unchanged.
std::string TypeName(std::string_view decorated);

// What MSVC's throw info (x64 and ARM64: image-relative offsets) says about a thrown object: every
// type a catch could take it as, most derived first, as decorated names, and where its
// std::exception base sits in the object when it has one.
struct ThrownTypes
{
  std::vector<std::string> decorated;
  std::optional<s32> std_exception_offset;
};
// `image_base` is the throwing module's base (the exception record's fourth parameter) and
// `throw_info` its _ThrowInfo (the third). Reads at most 16 types and 255 characters of each name.
ThrownTypes ReadThrowInfo(const u8* image_base, const u8* throw_info);

// Builds a line in a fixed buffer, always NUL-terminated, cutting what doesn't fit. No allocation
// and no throw, for exception handlers and the CPU thread (above).
class FixedLine
{
public:
  FixedLine(char* out, size_t size);
  FixedLine& operator<<(std::string_view text);
  // "0x" and upper-case digits, at least `digits` of them ("0x887A0006" for 8).
  FixedLine& Hex(u64 value, int digits = 1);
  FixedLine& Dec(u64 value);
  size_t Length() const { return m_length; }

private:
  char* m_out;
  size_t m_size;
  size_t m_length = 0;
};

// A fault that ends a run (InstallFaultReporter's line). `module` is the faulting module's file name,
// empty when the address is in none (JIT code); `graphics` the last graphics device state
// (Orca/GpuLost.h), empty when no backend gave one.
struct Fault
{
  u32 code = 0;
  u64 address = 0;
  std::string_view module;
  u64 offset = 0;
  // An access violation's or in-page error's access (0 read, 1 write, 8 execute) and address.
  bool has_access = false;
  u64 access_kind = 0;
  u64 access_address = 0;
  u32 thread = 0;
  std::string_view graphics;
  // Printed as it was raised, before any handler ran (the CPU thread's vectored handler): the run
  // may go on, as when a graphics driver catches its own access violation. The line says
  // "(first chance)" so it doesn't read as what ended the run.
  bool first_chance = false;
};
// What the fault reporter prints for these exception codes ("access violation"); nullptr for the
// others, which it leaves alone.
const char* FaultName(u32 code);
//   Orca: exception 0xC0000005 (access violation reading 0x10) at nvwgf2umx.dll+0x1A2B3C on
//   thread 4242; graphics device: lost (D3D11: device hung, 0x887A0006) at a 16x16x1 texture
// ("Orca: exception 0xC0000005 (access violation reading 0x10) (first chance) at ..." for a
// first-chance line)
// into `out`; returns its length.
size_t FormatFault(char* out, size_t size, const Fault& fault);

constexpr size_t MAX_FAULT_STACK_FRAMES = 24;
// Includes the newline added by the writer; the app retains at most 300 bytes per line.
constexpr size_t MAX_FAULT_STACK_LINE = 300;
struct StackFrame
{
  std::string_view module;
  // Module-relative, or the absolute address when module is empty.
  u64 offset = 0;
};
// Fixed storage only. Basenames, ASCII, complete frame tokens, at most 24 frames and 299 text
// bytes (plus NUL). An empty capture says "unavailable"; omitted frames are marked " ...".
size_t FormatFaultStack(char* out, size_t size, std::span<const StackFrame> frames) noexcept;

// Windows: from here on, a fault that ends Orca (an access violation outside fastmem and the
// rollback snapshots, an illegal instruction, a failed assert's breakpoint) prints its line
// (FormatFault) on stderr, then goes on as before: Windows ends Orca with it. No dump is written.
// On the CPU thread the line is printed as the fault is raised (a vectored handler after
// MemTools.cpp's), tagged "(first chance)" since a handler may still take it (single core runs the
// D3D driver on the CPU thread, and a driver catches some of its own): a fault under its JIT frames,
// which have no unwind tables, never reaches the unhandled-exception filter, the way a C++ throw
// from there can't be dispatched. Other threads'
// faults may still be caught (a driver catches its own), so the unhandled-exception filter prints
// those, then hands them to the filter before it. At most 8 different lines per run, each once.
// Each reported fault is followed by FormatFaultStack's module/offset line, unwound from its
// CONTEXT (up to 24 native frames; stops at missing/damaged unwind data, including JIT code).
// Elsewhere it does nothing (macOS writes its own crash report).
void InstallFaultReporter();

// Windows: from here on, each C++ exception Orca's own code throws prints one line on stderr as it
// is thrown, before any catch:
//   Orca: C++ exception <type> on thread <id>: <what> (at Orca.exe+<offset> ...)
// Up to 16 different ones per run, each once. A caught one costs a line; one that kills Orca is
// the last line of its log. Elsewhere it does nothing: Mac and Linux builds have no exceptions
// (-fno-exceptions), so a failure that would throw aborts with its own message.
void InstallThrowReporter();
}  // namespace Orca::CrashNote
