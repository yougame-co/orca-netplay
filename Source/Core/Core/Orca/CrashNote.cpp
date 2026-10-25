// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/CrashNote.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <array>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>

#include <windows.h>

#include <fmt/format.h>

#include "Core/Core.h"
#include "Core/Orca/GpuLost.h"
#endif

namespace Orca::CrashNote
{
bool WriteLine(std::FILE* out, std::string_view text) noexcept
{
  if (!out)
    return false;
  // One call (one lock), so another thread's line can't land between the text and its newline.
  const bool wrote =
      std::fprintf(out, "%.*s\n", static_cast<int>(text.size()), text.data()) >= 0;
  return std::fflush(out) == 0 && wrote;
}

FixedLine::FixedLine(char* out, size_t size) : m_out(out), m_size(size)
{
  if (m_size)
    m_out[0] = '\0';
}

FixedLine& FixedLine::operator<<(std::string_view text)
{
  if (!m_size)
    return *this;
  const size_t n = std::min(text.size(), m_size - 1 - m_length);
  std::copy_n(text.data(), n, m_out + m_length);
  m_length += n;
  m_out[m_length] = '\0';
  return *this;
}

FixedLine& FixedLine::Hex(u64 value, int digits)
{
  char text[18] = {'0', 'x'};
  int count = 1;
  while (count < 16 && (count < digits || (value >> (4 * count)) != 0))
    ++count;
  for (int i = 0; i < count; ++i)
    text[2 + i] = "0123456789ABCDEF"[(value >> (4 * (count - 1 - i))) & 0xF];
  return *this << std::string_view(text, 2 + count);
}

FixedLine& FixedLine::Dec(u64 value)
{
  char text[20];
  int at = sizeof(text);
  do
  {
    text[--at] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value && at > 0);
  return *this << std::string_view(text + at, sizeof(text) - at);
}

const char* FaultName(u32 code)
{
  // winnt.h's STATUS_* for the exceptions that end a process nobody catches them in.
  switch (code)
  {
  case 0xC0000005:
    return "access violation";
  case 0xC0000006:
    return "in-page error";
  case 0xC000001D:
    return "illegal instruction";
  case 0xC0000096:
    return "privileged instruction";
  case 0xC0000094:
    return "integer divide by zero";
  case 0xC0000095:
    return "integer overflow";
  case 0xC00000FD:
    return "stack overflow";
  case 0xC000008C:
    return "array bounds exceeded";
  case 0xC0000025:
    return "noncontinuable exception";
  case 0xC0000374:
    return "heap corruption";
  case 0x80000002:
    return "misaligned data";
  case 0x80000003:
    return "breakpoint";
  default:
    return nullptr;
  }
}

size_t FormatFault(char* out, size_t size, const Fault& fault)
{
  FixedLine line(out, size);
  line << "Orca: exception ";
  line.Hex(fault.code, 8);
  if (const char* name = FaultName(fault.code))
  {
    line << " (" << name;
    if (fault.has_access)
    {
      line << (fault.access_kind == 0 ? " reading " :
               fault.access_kind == 1 ? " writing " :
               fault.access_kind == 8 ? " executing " :
                                        " at ");
      line.Hex(fault.access_address);
    }
    line << ")";
  }
  if (fault.first_chance)
    line << " (first chance)";
  line << " at ";
  if (fault.module.empty())
  {
    line.Hex(fault.address);
    line << " (in no module)";
  }
  else
  {
    line << fault.module << "+";
    line.Hex(fault.offset);
  }
  line << " on thread ";
  line.Dec(fault.thread);
  line << "; graphics device: " << (fault.graphics.empty() ? "none" : fault.graphics);
  return line.Length();
}

size_t FormatFaultStack(char* out, size_t size, std::span<const StackFrame> frames) noexcept
{
  const size_t capacity = std::min(size, MAX_FAULT_STACK_LINE);
  FixedLine line(out, capacity);
  line << "Orca: exception stack";
  if (frames.empty())
    line << " unavailable";
  size_t shown = 0;
  for (const StackFrame& frame : frames.first(std::min(frames.size(), MAX_FAULT_STACK_FRAMES)))
  {
    std::string_view module = frame.module;
    if (const size_t slash = module.find_last_of("/\\"); slash != std::string_view::npos)
      module.remove_prefix(slash + 1);
    if (module.empty())
      module = "?";
    // Reserve enough space for the entire token and a possible omission marker. Never leave an
    // offset cut in half: it would look valid but resolve to the wrong address in the PDB.
    if (module.size() >= MAX_FAULT_STACK_LINE)
      break;
    char token[MAX_FAULT_STACK_LINE];
    FixedLine next(token, sizeof(token));
    next << " ";
    for (const unsigned char c : module)
    {
      const char safe = c > ' ' && c < 0x7F ? static_cast<char>(c) : '?';
      next << std::string_view(&safe, 1);
    }
    next << "+";
    next.Hex(frame.offset);
    const size_t reserve = shown + 1 < frames.size() ? 4 : 0;
    if (!capacity || line.Length() + next.Length() + reserve >= capacity ||
        next.Length() == sizeof(token) - 1)
      break;
    line << std::string_view(token, next.Length());
    ++shown;
  }
  if (shown < frames.size())
    line << " ...";
  return line.Length();
}

std::string TypeName(std::string_view decorated)
{
  std::string_view s = decorated;
  // Class (V) or struct (U), then "name@scope@...@@"; anything else is left as it is.
  if (!s.starts_with(".?AV") && !s.starts_with(".?AU"))
    return std::string(decorated);
  s.remove_prefix(4);
  if (s.size() < 3 || !s.ends_with("@@") || s.find_first_of("?$") != std::string_view::npos)
    return std::string(decorated);
  s.remove_suffix(2);
  std::string name;
  while (!s.empty())
  {
    const size_t at = s.rfind('@');
    const std::string_view part = at == std::string_view::npos ? s : s.substr(at + 1);
    if (part.empty())
      return std::string(decorated);
    if (!name.empty())
      name += "::";
    name += part;
    s = at == std::string_view::npos ? std::string_view{} : s.substr(0, at);
  }
  return name;
}

namespace
{
// MSVC's throw info, x64 and ARM64 (ehdata.h): every pointer is an offset from the image base.
//   _ThrowInfo         { u32 attributes; s32 unwind, forward_compat, catchable_type_array; }
//   _CatchableTypeArray { s32 count; s32 types[count]; }
//   _CatchableType     { u32 properties; s32 type; s32 mdisp, pdisp, vdisp; s32 size; s32 copy; }
//   TypeDescriptor     { void* vftable; void* spare; char name[]; }
constexpr size_t THROW_INFO_TYPES = 12;
constexpr size_t CATCHABLE_TYPE_TYPE = 4;
constexpr size_t CATCHABLE_TYPE_MDISP = 8;
constexpr size_t CATCHABLE_TYPE_PDISP = 12;
constexpr size_t TYPE_DESCRIPTOR_NAME = 2 * sizeof(void*);
constexpr s32 MAX_TYPES = 16;
constexpr size_t MAX_NAME = 255;

s32 ReadS32(const u8* at)
{
  s32 value;
  std::memcpy(&value, at, sizeof(value));
  return value;
}
}  // namespace

ThrownTypes ReadThrowInfo(const u8* image_base, const u8* throw_info)
{
  ThrownTypes out;
  if (!image_base || !throw_info)
    return out;
  const s32 array_offset = ReadS32(throw_info + THROW_INFO_TYPES);
  if (array_offset <= 0)
    return out;
  const u8* array = image_base + array_offset;
  const s32 count = ReadS32(array);
  for (s32 i = 0; i < count && i < MAX_TYPES; ++i)
  {
    const s32 type_offset = ReadS32(array + 4 + 4 * i);
    if (type_offset <= 0)
      break;
    const u8* catchable = image_base + type_offset;
    const s32 descriptor_offset = ReadS32(catchable + CATCHABLE_TYPE_TYPE);
    if (descriptor_offset <= 0)
      break;
    const char* name =
        reinterpret_cast<const char*>(image_base + descriptor_offset + TYPE_DESCRIPTOR_NAME);
    std::string decorated(name, strnlen(name, MAX_NAME));
    // A non-virtual base (pdisp -1) sits at mdisp; std::exception is never a virtual base.
    if (decorated == ".?AVexception@std@@" && ReadS32(catchable + CATCHABLE_TYPE_PDISP) == -1)
      out.std_exception_offset = ReadS32(catchable + CATCHABLE_TYPE_MDISP);
    out.decorated.push_back(std::move(decorated));
  }
  return out;
}

#ifdef _WIN32
namespace
{
constexpr DWORD MSVC_CXX_EXCEPTION = 0xE06D7363;
// The record's first parameter for a C++ throw (ehdata.h EH_MAGIC_NUMBER1 and its later forms).
constexpr ULONG_PTR EH_MAGIC_NUMBERS[] = {0x19930520, 0x19930521, 0x19930522};
constexpr int MAX_REPORTS = 16;
constexpr size_t MAX_WHAT = 300;
constexpr int MAX_FRAMES = 8;

std::mutex s_mutex;
std::array<size_t, MAX_REPORTS> s_reported{};
int s_reports = 0;
thread_local bool t_reporting = false;

// [base, base + size) of a loaded module, from its PE header; size 0 if it can't be read.
std::pair<uintptr_t, size_t> ModuleRange(HMODULE module)
{
  if (!module)
    return {0, 0};
  const auto base = reinterpret_cast<uintptr_t>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return {base, 0};
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return {base, 0};
  return {base, nt->OptionalHeader.SizeOfImage};
}

// Orca's own code: Orca.exe, and the C++ library DLL whose helpers throw for it (std::vector::at,
// std::stoi). Drivers and system DLLs throw and catch their own; those are left out.
bool OwnThrow(ULONG_PTR image_base)
{
  if (image_base == reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr)))
    return true;
  wchar_t path[MAX_PATH];
  const DWORD length =
      GetModuleFileNameW(reinterpret_cast<HMODULE>(image_base), path, static_cast<DWORD>(MAX_PATH));
  if (length == 0 || length >= MAX_PATH)
    return false;
  const wchar_t* file = path;
  for (const wchar_t* c = path; *c; ++c)
  {
    if (*c == L'\\' || *c == L'/')
      file = c + 1;
  }
  return _wcsnicmp(file, L"msvcp", 5) == 0;
}

std::string Report(const EXCEPTION_RECORD& record)
{
  const auto* image_base = reinterpret_cast<const u8*>(record.ExceptionInformation[3]);
  const auto* throw_info = reinterpret_cast<const u8*>(record.ExceptionInformation[2]);
  const ThrownTypes types = ReadThrowInfo(image_base, throw_info);
  std::string line = fmt::format("Orca: C++ exception {} on thread {}",
                                 types.decorated.empty() ? "of an unknown type" :
                                                           TypeName(types.decorated.front()),
                                 GetCurrentThreadId());
  if (types.std_exception_offset && record.ExceptionInformation[1])
  {
    const auto* object = reinterpret_cast<const std::exception*>(
        reinterpret_cast<const u8*>(record.ExceptionInformation[1]) + *types.std_exception_offset);
    if (const char* what = object->what())
    {
      std::string text(what, strnlen(what, MAX_WHAT));
      for (char& c : text)
      {
        if (c == '\n' || c == '\r')
          c = ' ';
      }
      line += ": " + text;
    }
  }
  // Where it was thrown: Orca.exe's frames on this stack past the exception dispatch (this
  // handler's own frames come first, then ntdll's and the throw's; JIT frames end the walk).
  void* frames[48];
  const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  const auto [base, size] = ModuleRange(GetModuleHandleW(nullptr));
  bool past_dispatch = false;
  int shown = 0;
  std::string where;
  for (USHORT i = 0; i < count && shown < MAX_FRAMES; ++i)
  {
    const auto at = reinterpret_cast<uintptr_t>(frames[i]);
    const bool own = size && at >= base && at - base < size;
    if (!own)
    {
      past_dispatch = true;
      continue;
    }
    if (!past_dispatch)
      continue;
    where += fmt::format("{}Orca.exe+{:#x}", where.empty() ? "" : " ", at - base);
    ++shown;
  }
  if (!where.empty())
    line += " (at " + where + ")";
  return line;
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* pointers)
{
  const EXCEPTION_RECORD* record = pointers ? pointers->ExceptionRecord : nullptr;
  if (!record || record->ExceptionCode != MSVC_CXX_EXCEPTION || record->NumberParameters < 4 ||
      std::find(std::begin(EH_MAGIC_NUMBERS), std::end(EH_MAGIC_NUMBERS),
                record->ExceptionInformation[0]) == std::end(EH_MAGIC_NUMBERS) ||
      t_reporting || !OwnThrow(record->ExceptionInformation[3]))
  {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  t_reporting = true;
  try
  {
    std::lock_guard lock(s_mutex);
    if (s_reports < MAX_REPORTS)
    {
      std::string line = Report(*record);
      // Once per type, message and place, whichever thread threw it.
      std::string key = line;
      if (const size_t thread = key.find(" on thread "); thread != std::string::npos)
        key.erase(thread, key.find_first_of(":(", thread) - thread);
      const size_t hash = std::hash<std::string>{}(key);
      if (std::find(s_reported.begin(), s_reported.begin() + s_reports, hash) ==
          s_reported.begin() + s_reports)
      {
        s_reported[s_reports++] = hash;
        line += '\n';
        // Straight to the handle: the C runtime's stderr may be the stream that failed.
        DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), line.data(), static_cast<DWORD>(line.size()),
                  &written, nullptr);
      }
    }
  }
  catch (...)
  {
  }
  t_reporting = false;
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallThrowReporter()
{
  static std::atomic<bool> s_installed{false};
  if (s_installed.exchange(true))
    return;
  // Called last, after the fastmem handler (MemTools.cpp), which never takes a C++ exception.
  AddVectoredExceptionHandler(FALSE, OnException);
}

namespace
{
constexpr int MAX_FAULTS = 8;

// The handler's state is static: a fault may come with the stack nearly gone or the heap broken.
// One fault at a time; another thread's meanwhile goes unrecorded rather than wait in a handler.
std::atomic_flag s_fault_busy = ATOMIC_FLAG_INIT;
std::array<u64, MAX_FAULTS> s_faults{};
int s_fault_count = 0;
char s_fault_line[768];
char s_fault_graphics[256];
char s_fault_module[MAX_PATH];
// Resolve the native query before installing handlers. Higher-level filename APIs may allocate
// temporary buffers; MemoryMappedFilenameInformation writes into this caller-owned buffer.
using QueryVirtualMemory = LONG(NTAPI*)(HANDLE, const void*, int, void*, SIZE_T, SIZE_T*);
QueryVirtualMemory s_query_virtual_memory = nullptr;
struct MappedName
{
  USHORT length;
  USHORT maximum_length;
  wchar_t* name;
  wchar_t buffer[MAX_PATH];
};
MappedName s_fault_mapping{};
// Protected by the same non-waiting flag as the fault line. Keep the module-name storage off an
// already exhausted stack; the capture context, addresses and output line have fixed stack space.
char s_stack_modules[MAX_FAULT_STACK_FRAMES][MAX_PATH];

u64 FaultImage(const void* address)
{
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.Type != MEM_IMAGE)
    return 0;
  return reinterpret_cast<u64>(memory.AllocationBase);
}

// The file name of the module `address` is in (a path would name the player's folders), as ASCII,
// and its base; empty when it is in none (JIT code).
std::string_view FaultModule(const void* address, u64* base)
{
  *base = FaultImage(address);
  if (!*base)
    return {};
  // Query the mapping, not the loader's module list: no loader lock in the exception path.
  constexpr int MEMORY_MAPPED_FILENAME_INFORMATION = 2;
  if (!s_query_virtual_memory ||
      s_query_virtual_memory(GetCurrentProcess(), address, MEMORY_MAPPED_FILENAME_INFORMATION,
                             &s_fault_mapping, sizeof(s_fault_mapping), nullptr) < 0)
    return "?";
  const size_t length = s_fault_mapping.length / sizeof(wchar_t);
  const auto start = reinterpret_cast<uintptr_t>(s_fault_mapping.name);
  const auto end = reinterpret_cast<uintptr_t>(&s_fault_mapping) + sizeof(s_fault_mapping);
  if (!length || start < reinterpret_cast<uintptr_t>(s_fault_mapping.buffer) || start >= end ||
      s_fault_mapping.length > end - start)
    return "?";
  size_t file = 0;
  for (size_t i = 0; i < length; ++i)
  {
    if (s_fault_mapping.name[i] == L'\\' || s_fault_mapping.name[i] == L'/')
      file = i + 1;
  }
  size_t n = 0;
  for (; file + n < length && n < MAX_PATH - 1; ++n)
  {
    const wchar_t c = s_fault_mapping.name[file + n];
    s_fault_module[n] = c > L' ' && c < 0x7F ? static_cast<char>(c) : '?';
  }
  return {s_fault_module, n};
}

// Only native image unwind tables. Avoid DbgHelp's symbol state/locks and dynamic function-table
// callbacks (including JIT code). The containing CaptureFaultStack SEH guard bounds bad reads.
PRUNTIME_FUNCTION FaultFunction(u64 pc, u64 base)
{
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return nullptr;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
    return nullptr;
  const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (!directory.VirtualAddress || directory.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
      directory.Size > nt->OptionalHeader.SizeOfImage - directory.VirtualAddress)
    return nullptr;
  auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(base + directory.VirtualAddress);
  const size_t count = directory.Size / sizeof(*table);
  // PE runtime function entries are sorted by BeginAddress. Find the last start <= pc.
  size_t low = 0;
  size_t high = count;
  const u64 rva = pc - base;
  while (low < high)
  {
    const size_t middle = low + (high - low) / 2;
    if (table[middle].BeginAddress <= rva)
      low = middle + 1;
    else
      high = middle;
  }
  if (!low)
    return nullptr;
  PRUNTIME_FUNCTION entry = &table[low - 1];
#if defined(_M_X64)
  return rva < entry->EndAddress ? entry : nullptr;
#elif defined(_M_ARM64)
  // ARM64 packed entries carry FunctionLength (11 bits, in instructions); unpacked .xdata
  // headers carry FunctionLength in their low 18 bits. Flag 3 is reserved.
  const u32 flag = entry->UnwindData & 3;
  if (flag == 3)
    return nullptr;
  const u64 length = flag ? ((entry->UnwindData >> 2) & 0x7FF) * 4 :
                           (*reinterpret_cast<const u32*>(base + entry->UnwindData) & 0x3FFFF) * 4;
  return rva - entry->BeginAddress < length ? entry : nullptr;
#endif
}

size_t CaptureFaultStack(const CONTEXT* original, u64* addresses) noexcept
{
  size_t count = 0;
  if (!original)
    return count;
  // RtlCaptureStackBackTrace would start in this handler. Unwind a COPY of the fault context so
  // frame zero is the fault, and Windows can still dispatch the untouched original afterwards.
  __try
  {
    CONTEXT context = *original;
    ULONG_PTR stack_low = 0;
    ULONG_PTR stack_high = 0;
    GetCurrentThreadStackLimits(&stack_low, &stack_high);
    while (count < MAX_FAULT_STACK_FRAMES)
    {
#if defined(_M_X64)
      const u64 pc = context.Rip;
      const u64 sp = context.Rsp;
#elif defined(_M_ARM64)
      const u64 pc = context.Pc;
      const u64 sp = context.Sp;
#endif
      if (!pc)
        break;
      addresses[count++] = pc;
      if (sp < stack_low || sp >= stack_high || sp % sizeof(u64))
        break;
      const u64 base = FaultImage(reinterpret_cast<const void*>(pc));
      if (!base)
        break;  // JIT/unknown code has no native image table. Do not guess return addresses.
      if (PRUNTIME_FUNCTION entry = FaultFunction(pc, base))
      {
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, entry, &context, &handler_data, &establisher,
                         nullptr);
      }
      else
      {
        // Native leaf functions have no table entry. Their return address is on the stack (x64)
        // or in the link register (ARM64).
#if defined(_M_X64)
        if (stack_high - sp < sizeof(u64))
          break;
        context.Rip = *reinterpret_cast<const u64*>(sp);
        context.Rsp += sizeof(u64);
#elif defined(_M_ARM64)
        context.Pc = context.Lr;
        context.Lr = 0;
#endif
      }
#if defined(_M_X64)
      if (context.Rsp <= sp)
        break;
#elif defined(_M_ARM64)
      if (context.Sp < sp || (context.Sp == sp && context.Pc == pc))
        break;
#endif
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    // Damaged stack/unwind data: retain every frame already captured; never fault recursively.
  }
  return count;
}

void WriteFaultStack(const CONTEXT* context)
{
  u64 addresses[MAX_FAULT_STACK_FRAMES];
  const size_t count = CaptureFaultStack(context, addresses);
  StackFrame frames[MAX_FAULT_STACK_FRAMES];
  for (size_t i = 0; i < count; ++i)
  {
    u64 base = 0;
    const std::string_view module = FaultModule(reinterpret_cast<const void*>(addresses[i]), &base);
    std::copy(module.begin(), module.end(), s_stack_modules[i]);
    frames[i] = {{s_stack_modules[i], module.size()}, addresses[i] - base};
  }
  char line[MAX_FAULT_STACK_LINE + 1];
  size_t length = FormatFaultStack(line, sizeof(line), std::span(frames, count));
  line[length++] = '\n';
  DWORD written;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(length), &written, nullptr);
}

// Prints `record`'s line once (at most MAX_FAULTS different ones). A first-chance line and the
// unhandled filter's line for the same fault are different lines, so a fault the CPU thread's
// handler saw first and that then ended Orca is said both ways.
void RecordFault(const EXCEPTION_RECORD& record, const CONTEXT* context, bool first_chance)
{
  if (s_fault_busy.test_and_set(std::memory_order_acquire))
    return;
  const u64 address = reinterpret_cast<u64>(record.ExceptionAddress);
  const u64 key = ((static_cast<u64>(record.ExceptionCode) << 32) ^ address) ^
                  (first_chance ? 1ull << 63 : 0);
  if (s_fault_count < MAX_FAULTS &&
      std::find(s_faults.begin(), s_faults.begin() + s_fault_count, key) ==
          s_faults.begin() + s_fault_count)
  {
    s_faults[s_fault_count++] = key;
    Fault fault;
    fault.first_chance = first_chance;
    fault.code = record.ExceptionCode;
    fault.address = address;
    u64 base = 0;
    fault.module = FaultModule(record.ExceptionAddress, &base);
    fault.offset = address - base;
    if ((fault.code == EXCEPTION_ACCESS_VIOLATION || fault.code == EXCEPTION_IN_PAGE_ERROR) &&
        record.NumberParameters >= 2)
    {
      fault.has_access = true;
      fault.access_kind = record.ExceptionInformation[0];
      fault.access_address = record.ExceptionInformation[1];
    }
    fault.thread = GetCurrentThreadId();
    fault.graphics = std::string_view(
        s_fault_graphics, GpuLost::DeviceState(s_fault_graphics, sizeof(s_fault_graphics)));
    size_t length = FormatFault(s_fault_line, sizeof(s_fault_line) - 1, fault);
    s_fault_line[length++] = '\n';
    // Straight to the handle, like the throw reporter: no C runtime lock in a fault.
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), s_fault_line, static_cast<DWORD>(length), &written,
              nullptr);
    WriteFaultStack(context);
  }
  s_fault_busy.clear(std::memory_order_release);
}

const EXCEPTION_RECORD* ReportedFault(const EXCEPTION_POINTERS* pointers)
{
  const EXCEPTION_RECORD* record = pointers ? pointers->ExceptionRecord : nullptr;
  return record && FaultName(record->ExceptionCode) ? record : nullptr;
}

// The CPU thread's faults, as they are raised: its JIT frames have no unwind tables, so a fault
// under them never reaches the unhandled-exception filter (the way a C++ throw from there can't be
// dispatched). Other threads' faults may still be caught (a driver catches its own), so only the
// filter says them.
LONG CALLBACK OnCpuThreadFault(EXCEPTION_POINTERS* pointers)
{
  if (const EXCEPTION_RECORD* record = ReportedFault(pointers); record && Core::IsCPUThread())
    RecordFault(*record, pointers->ContextRecord, true);
  return EXCEPTION_CONTINUE_SEARCH;
}

LPTOP_LEVEL_EXCEPTION_FILTER s_previous_filter = nullptr;

// Nothing caught it: say it, then on to what Windows did before (the C runtime's filter, then
// Windows Error Reporting, which ends Orca with the exception's code).
LONG WINAPI OnUnhandledFault(EXCEPTION_POINTERS* pointers)
{
  if (const EXCEPTION_RECORD* record = ReportedFault(pointers))
    RecordFault(*record, pointers->ContextRecord, false);
  return s_previous_filter ? s_previous_filter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallFaultReporter()
{
  static std::atomic<bool> s_installed{false};
  if (s_installed.exchange(true))
    return;
  s_query_virtual_memory = reinterpret_cast<QueryVirtualMemory>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryVirtualMemory"));
  // Last, after the fastmem and rollback-snapshot handler (MemTools.cpp, installed first), which
  // takes the access violations that are Orca's own business and resumes them.
  AddVectoredExceptionHandler(FALSE, OnCpuThreadFault);
  s_previous_filter = SetUnhandledExceptionFilter(OnUnhandledFault);
}
#else
void InstallThrowReporter()
{
}

void InstallFaultReporter()
{
}
#endif
}  // namespace Orca::CrashNote
