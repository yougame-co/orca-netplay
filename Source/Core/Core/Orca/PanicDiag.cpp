// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/PanicDiag.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <xxh3.h>

#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/Debugger/OSThread.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/GpuDiagnostics.h"
#include "Core/Orca/JitWarm.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Orca::PanicDiag
{
const std::vector<u32> WORDS = {0x100, 0x300, 0x500, 0x504, 0x900, 0xC00,
                                0xD8,  0xDC,  0xE0,  0xE4,  0x30,  0x34};

std::string DescribeWord(u32 physical_address, const std::vector<View>& views)
{
  std::string line = fmt::format("orca diag word {:08x}:", physical_address);
  bool any = false;
  bool differ = false;
  u32 first = 0;
  for (const View& view : views)
  {
    if (!view.base || physical_address < view.physical_address ||
        physical_address - view.physical_address + 4 > view.size)
    {
      continue;
    }
    u32 raw;
    std::memcpy(&raw, view.base + (physical_address - view.physical_address), sizeof(raw));
    const u32 word = Common::swap32(raw);
    if (!any)
      first = word;
    else if (word != first)
      differ = true;
    any = true;
    line += fmt::format(" {}={:08x}", view.name, word);
  }
  if (!any)
    line += " no view";
  if (differ)
    line += " differ";
  return line;
}

std::optional<u32> GuestRam::Physical(u32 address) const
{
  // 0x8/0x9 (cached) and 0xC/0xD (uncached) segments only.
  const u32 segment = address >> 28;
  if (segment != 0x8 && segment != 0x9 && segment != 0xC && segment != 0xD)
    return std::nullopt;
  const u32 physical = address & 0x3FFFFFFF;
  if (physical < mem1.size())
    return physical;
  if (physical >= MEM2_PHYSICAL && physical - MEM2_PHYSICAL < mem2.size())
    return physical;
  return std::nullopt;
}

std::optional<u32> GuestRam::Word(u32 address) const
{
  if (address % 4 != 0)
    return std::nullopt;
  const std::optional<u32> physical = Physical(address);
  if (!physical)
    return std::nullopt;
  const u8* p;
  if (*physical < MEM2_PHYSICAL)
  {
    if (*physical + 4 > mem1.size())
      return std::nullopt;
    p = mem1.data() + *physical;
  }
  else
  {
    if (*physical - MEM2_PHYSICAL + 4 > mem2.size())
      return std::nullopt;
    p = mem2.data() + (*physical - MEM2_PHYSICAL);
  }
  u32 raw;
  std::memcpy(&raw, p, sizeof(raw));
  return Common::swap32(raw);
}

BackChain WalkBackChain(const GuestRam& ram, u32 sp, int max_depth)
{
  BackChain chain;
  if (!ram.Valid(sp))
  {
    chain.stop = "sp not ram";
    return chain;
  }
  for (;;)
  {
    if (static_cast<int>(chain.frames.size()) >= max_depth)
    {
      chain.stop = "cap";
      return chain;
    }
    const u32 next = *ram.Word(sp);
    if (next == 0)
    {
      chain.stop = "end";
      return chain;
    }
    if (!ram.Valid(next))
    {
      chain.stop = fmt::format("bad {:08x}", next);
      return chain;
    }
    if (next <= sp)
    {
      chain.stop = fmt::format("not above {:08x}", next);
      return chain;
    }
    chain.frames.push_back(StackFrame{next, ram.Word(next + 4)});
    sp = next;
  }
}

std::vector<std::string> DescribeBackChain(const BackChain& chain, u32 sp)
{
  std::vector<std::string> lines;
  std::string line = fmt::format("orca diag chain {:08x}:", sp);
  for (std::size_t i = 0; i < chain.frames.size(); ++i)
  {
    if (i > 0 && i % 5 == 0)
    {
      lines.push_back(std::move(line));
      line = "orca diag chain+";
    }
    const StackFrame& f = chain.frames[i];
    line += f.saved_lr ? fmt::format(" {:08x}>{:08x}", f.back_chain, *f.saved_lr) :
                         fmt::format(" {:08x}>?", f.back_chain);
  }
  line += " " + chain.stop;
  lines.push_back(std::move(line));
  return lines;
}

std::vector<std::string> DescribeWords(const GuestRam& ram, std::string_view label, u32 address,
                                       int count)
{
  std::vector<std::string> lines;
  address &= ~3u;
  for (int i = 0; i < count; i += 8)
  {
    const u32 at = address + 4 * i;
    std::string line = fmt::format("orca diag {} {:08x}:", label, at);
    for (int j = i; j < count && j < i + 8; ++j)
    {
      const std::optional<u32> word = ram.Word(address + 4 * j);
      line += word ? fmt::format(" {:08x}", *word) : std::string(" --------");
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

namespace
{
// OSModuleInfo: id +0x0, next +0x4, section count +0xC, section table +0x10 (8-byte entries:
// address with bit 0 = executable, size). Pointers are absolute once a module is linked.
template <typename F>
void ForEachModule(const GuestRam& ram, F&& f)
{
  std::optional<u32> module = ram.Word(OS_MODULE_LIST);
  for (int i = 0; module && *module && ram.Valid(*module) && i < 64; ++i)
  {
    if (!f(*module))
      return;
    module = ram.Word(*module + 4);
  }
}
}  // namespace

std::string ModuleOf(const GuestRam& ram, u32 address)
{
  std::string found = "-";
  ForEachModule(ram, [&](u32 module) {
    const std::optional<u32> count = ram.Word(module + 0xC);
    const std::optional<u32> table = ram.Word(module + 0x10);
    if (!count || !table || !ram.Valid(*table))
      return true;
    for (u32 s = 0; s < std::min<u32>(*count, 32); ++s)
    {
      const std::optional<u32> start = ram.Word(*table + 8 * s);
      const std::optional<u32> size = ram.Word(*table + 8 * s + 4);
      if (!start || !size)
        break;
      const u32 base = *start & ~1u;
      if (base != 0 && address >= base && address - base < *size)
      {
        found = fmt::format("m{}s{}+{:x}", *ram.Word(module), s, address - base);
        return false;
      }
    }
    return true;
  });
  return found;
}

std::string DescribeModules(const GuestRam& ram)
{
  std::string line = "orca diag mods:";
  int n = 0;
  ForEachModule(ram, [&](u32 module) {
    if (++n > 16)
    {
      line += " ...";
      return false;
    }
    line += fmt::format(" {}@{:08x}", *ram.Word(module), module);
    return true;
  });
  if (n == 0)
    line += " none";
  return line;
}

std::vector<std::string> DescribeOS(const GuestRam& ram, u32 r1)
{
  using Core::Debug::OSContext;
  using Core::Debug::OSThread;
  // u32, not size_t: MSVC's /W4 /WX rejects the narrowing.
  const auto Off = [](std::size_t offset) { return static_cast<u32>(offset); };
  const auto word = [&ram](u32 at) {
    const std::optional<u32> w = ram.Word(at);
    return w ? fmt::format("{:08x}", *w) : std::string("--------");
  };
  const auto context = [&](std::string_view label, u32 at) {
    if (!ram.Valid(at) || !ram.Valid(at + Off(offsetof(OSContext, srr1))))
      return fmt::format("orca diag os {} {:08x}: not ram", label, at);
    const std::optional<u32> state = ram.Word(at + Off(offsetof(OSContext, dummy)));
    return fmt::format(
        "orca diag os {} {:08x}: srr0 {} srr1 {} lr {} r1 {} ctr {} state {:04x}", label, at,
        word(at + Off(offsetof(OSContext, srr0))), word(at + Off(offsetof(OSContext, srr1))),
        word(at + Off(offsetof(OSContext, lr))), word(at + Off(offsetof(OSContext, gpr)) + 4),
        word(at + Off(offsetof(OSContext, ctr))), state ? *state & 0xFFFF : 0xFFFF);
  };
  std::vector<std::string> lines;
  const u32 ctx = ram.Word(OS_CURRENT_CONTEXT).value_or(0);
  const u32 thread = ram.Word(OS_CURRENT_THREAD).value_or(0);
  lines.push_back(context("ctx", ctx));
  if (!ram.Valid(thread) || !ram.Valid(thread + Off(offsetof(OSThread, specific))))
  {
    lines.push_back(fmt::format("orca diag os thread {:08x}: not ram", thread));
    return lines;
  }
  if (thread != ctx)
    lines.push_back(context("thread ctx", thread));
  const u32 state = *ram.Word(thread + Off(offsetof(OSThread, state)));
  const u32 priority = *ram.Word(thread + Off(offsetof(OSThread, effective_priority)));
  const u32 top = *ram.Word(thread + Off(offsetof(OSThread, stack_addr)));
  const u32 end = *ram.Word(thread + Off(offsetof(OSThread, stack_end)));
  const std::optional<u32> magic = ram.Word(end);
  lines.push_back(fmt::format(
      "orca diag os thread {:08x}: state {:04x} prio {} stack {:08x}-{:08x} magic {} r1 {}", thread,
      state >> 16, static_cast<s32>(priority), end, top,
      !magic                          ? "?" :
      *magic == OSThread::STACK_MAGIC ? "ok" :
                                        "bad",
      r1 > end && r1 <= top ? "in" : "out"));
  return lines;
}

u64 CodeHash(const GuestRam& ram, const Common::RangeSet<u32>& physical_ranges)
{
  u64 hash = 0x4F72636144696167ull;
  bool any = false;
  for (auto it = physical_ranges.begin(); it != physical_ranges.end(); ++it)
  {
    const u32 start = it.from(), end = it.to();
    const u8* p = nullptr;
    if (end <= start)
      return 0;
    if (end <= ram.mem1.size())
      p = ram.mem1.data() + start;
    else if (start >= GuestRam::MEM2_PHYSICAL && end - GuestRam::MEM2_PHYSICAL <= ram.mem2.size())
      p = ram.mem2.data() + (start - GuestRam::MEM2_PHYSICAL);
    if (!p)
      return 0;
    hash = XXH3_64bits_withSeed(p, end - start, hash ^ ((u64{start} << 32) | end));
    any = true;
  }
  if (!any)
    return 0;
  return hash != 0 ? hash : 1;
}

GuestRam RamOf(Core::System& system)
{
  Memory::MemoryManager& memory = system.GetMemory();
  return GuestRam{{memory.GetRAM(), memory.GetRAM() ? memory.GetRamSizeReal() : 0},
                  {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSizeReal() : 0}};
}

std::vector<std::string> DescribeExceptions(std::span<const ExceptionNote> ring, u32 count)
{
  std::vector<std::string> lines;
  const u32 size = static_cast<u32>(ring.size());
  const u32 kept = std::min(count, size);
  std::string line = fmt::format("orca diag exc {}:", count);
  for (u32 i = 0; i < kept; ++i)
  {
    if (i > 0 && i % 4 == 0)
    {
      lines.push_back(std::move(line));
      line = "orca diag exc+";
    }
    const ExceptionNote& e = ring[(count - kept + i) % size];
    line += fmt::format(" {:x}:{:08x}/{:08x}/{:08x}", e.vector, e.srr0, e.r1, e.lr);
  }
  lines.push_back(std::move(line));
  return lines;
}

namespace
{
// The scene the dump names; written and read on the CPU thread only.
std::string s_scene;
int s_scene_frame = 0;
int s_scene_changes = 0;
std::optional<JitWarm::Scene> s_scene_key;
}  // namespace

void NoteScene(const Core::CPUThreadGuard& guard, int frame)
{
#ifdef _WIN32
  GpuDiagnostics::NoteFrame(frame);
#endif
  const std::optional<JitWarm::Scene> now = JitWarm::CurrentScene(guard);
  if (!now || now == s_scene_key)
    return;
  s_scene_key = now;
  s_scene = JitWarm::SceneName(guard, now->name);
  s_scene_frame = frame;
#ifdef _WIN32
  GpuDiagnostics::NoteScene(s_scene.empty() ? "unknown" : s_scene, frame);
#endif
  ++s_scene_changes;
  NOTICE_LOG_FMT(ROLLBACK, "orca scene {} at frame {}", s_scene.empty() ? "?" : s_scene, frame);
}

namespace
{
// The `orca diag code` line for one register: the RAM words from address-8 to address+4 (the
// address's own in brackets), the module section holding it, and the JIT blocks compiled over it
// ("ok" if their RAM is what was compiled, "changed" if not, "?" if it couldn't be hashed).
struct CodeBlock
{
  u32 start;
  u32 size;
  const char* state;
};
std::string DescribeCode(const GuestRam& ram, std::string_view label, u32 address,
                         const std::vector<CodeBlock>& blocks)
{
  std::string line =
      fmt::format("orca diag code {} {:08x} {}:", label, address, ModuleOf(ram, address));
  for (int i = -2; i <= 1; ++i)
  {
    const std::optional<u32> word = ram.Word((address & ~3u) + 4 * i);
    const std::string text = word ? fmt::format("{:08x}", *word) : std::string("--------");
    line += i == 0 ? fmt::format(" [{}]", text) : " " + text;
  }
  if (blocks.empty())
    line += " jit none";
  for (const CodeBlock& b : blocks)
    line += fmt::format(" jit {:08x}+{} {}", b.start, b.size, b.state);
  return line;
}

void DumpMore(Core::System& system, std::vector<std::string>& lines)
{
  const PowerPC::PowerPCState& ppc = system.GetPPCState();
  const GuestRam ram = RamOf(system);
  // As ppcState holds them: under the JIT, a register changed earlier in the faulting block may
  // still be in a host register (block entry's value here). LR and the exception ring are exact.
  for (int first = 0; first < 32; first += 8)
  {
    std::string line = fmt::format("orca diag gpr{}:", first);
    for (int i = first; i < first + 8; ++i)
      line += fmt::format(" {:08x}", ppc.gpr[i]);
    lines.push_back(std::move(line));
  }
  const u32 r1 = ppc.gpr[1];
  for (std::string& line : DescribeWords(ram, "sp", r1 - 0x10, 32))
    lines.push_back(std::move(line));
  for (std::string& line : DescribeBackChain(WalkBackChain(ram, r1), r1))
    lines.push_back(std::move(line));
  for (std::string& line : DescribeOS(ram, r1))
    lines.push_back(std::move(line));
  lines.push_back(DescribeModules(ram));

  // The JIT's blocks over pc, lr and srr0, and every block whose RAM changed since it was
  // compiled. Only on the CPU thread, which owns the block cache. "changed" is a lead, not proof:
  // code rewritten and invalidated keeps no block, but RAM a block came from can also change
  // harmlessly (freed module memory never run again, a rollback load, which keeps the JIT).
  const std::array<std::pair<const char*, u32>, 3> regs{
      {{"pc", ppc.pc}, {"lr", ppc.spr[SPR_LR]}, {"srr0", ppc.spr[SPR_SRR0]}}};
  std::array<std::vector<CodeBlock>, 3> at;
  std::string jit_line;
  if (Core::IsCPUThread())
  {
    Core::CPUThreadGuard guard(system);
    std::size_t total = 0, changed = 0, unknown = 0;
    std::string listed;
    system.GetJitInterface().RunOnBlocks(guard, [&](const JitBlock& block) {
      ++total;
      const char* state = "?";
      if (block.orca_code_hash == 0)
        ++unknown;
      else if (CodeHash(ram, block.physical_addresses) == block.orca_code_hash)
        state = "ok";
      else
      {
        state = "changed";
        if (++changed <= 6)
          listed += fmt::format(" {:08x}+{}", block.effectiveAddress, block.originalSize);
      }
      for (std::size_t i = 0; i < regs.size(); ++i)
      {
        const std::optional<u32> physical = ram.Physical(regs[i].second & ~3u);
        if (physical && at[i].size() < 2 && block.OverlapsPhysicalRange(*physical, 4))
          at[i].push_back(CodeBlock{block.effectiveAddress, block.originalSize, state});
      }
    });
    jit_line = fmt::format("orca diag jit blocks {} unhashed {} changed {}:{}", total, unknown,
                           changed, changed ? listed : std::string(" none"));
  }
  else
  {
    jit_line = "orca diag jit: not on the CPU thread";
  }
  for (std::size_t i = 0; i < regs.size(); ++i)
    lines.push_back(DescribeCode(ram, regs[i].first, regs[i].second, at[i]));
  lines.push_back(std::move(jit_line));

  for (std::string& line : DescribeExceptions(g_exceptions, g_exception_count))
    lines.push_back(std::move(line));
  lines.push_back(s_scene_changes ?
                      fmt::format("orca diag scene {} since frame {} ({} changes)",
                                  s_scene.empty() ? "?" : s_scene, s_scene_frame, s_scene_changes) :
                      std::string("orca diag scene unknown"));
}
}  // namespace

namespace
{
std::string ViewName(Memory::MemoryManager& memory, const u8* base)
{
  if (base == memory.GetRAM())
    return "ram";
  if (base == memory.GetEXRAM())
    return "exram";
  constexpr u64 SPAN = 0x100000000ULL;
  if (const u8* physical = memory.GetPhysicalBase();
      physical && base >= physical && static_cast<u64>(base - physical) < SPAN)
  {
    return fmt::format("phys:{:08x}", static_cast<u32>(base - physical));
  }
  if (const u8* logical = memory.GetLogicalBase();
      logical && base >= logical && static_cast<u64>(base - logical) < SPAN)
  {
    return fmt::format("logical:{:08x}", static_cast<u32>(base - logical));
  }
  return "view";
}
}  // namespace

void DumpOnce(Core::System& system, u32 effective_address, bool write)
{
  static std::atomic<bool> s_done{false};
  if (s_done.exchange(true))
    return;

  const PowerPC::PowerPCState& ppc = system.GetPPCState();
  Memory::MemoryManager& memory = system.GetMemory();
  std::vector<std::string> lines;
  lines.push_back(fmt::format("orca diag first invalid {} {:08x} at pc {:08x}",
                              write ? "write to" : "read from", effective_address, ppc.pc));
  lines.push_back(fmt::format(
      "orca diag regs msr {:08x} srr0 {:08x} srr1 {:08x} lr {:08x} ctr {:08x} r1 {:08x} "
      "dec {:08x} hid4 {:08x}",
      ppc.msr.Hex, ppc.spr[SPR_SRR0], ppc.spr[SPR_SRR1], ppc.spr[SPR_LR], ppc.spr[SPR_CTR],
      ppc.gpr[1], ppc.spr[SPR_DEC], ppc.spr[SPR_HID4]));
  for (const auto& [label, first, second] :
       {std::tuple{"ibat", SPR_IBAT0U, SPR_IBAT4U}, std::tuple{"dbat", SPR_DBAT0U, SPR_DBAT4U}})
  {
    std::string line = fmt::format("orca diag {}", label);
    for (int i = 0; i < 8; ++i)
    {
      const int upper = (i < 4 ? first : second) + 2 * (i % 4);
      line += fmt::format(" {}:{:08x}/{:08x}", i, ppc.spr[upper], ppc.spr[upper + 1]);
    }
    lines.push_back(std::move(line));
  }

  std::vector<View> views;
  for (const Memory::GuestRamView& view : memory.GetGuestRamViews())
  {
    views.push_back(View{ViewName(memory, view.base), view.base, view.physical_address, view.size});
  }
  std::string list =
      fmt::format("orca diag views fastmem {}:", memory.GetPhysicalBase() ? "on" : "off");
  for (const View& view : views)
    list += fmt::format(" {}@{:08x}+{:x}", view.name, view.physical_address, view.size);
  lines.push_back(std::move(list));
  for (const u32 address : WORDS)
    lines.push_back(DescribeWord(address, views));
  // fmt is built without exceptions (FMT_EXCEPTIONS=0) and nothing below throws, as the CPU
  // thread under JIT code requires (CrashNote.h).
  DumpMore(system, lines);

  for (const std::string& line : lines)
    std::fprintf(stderr, "%s\n", line.c_str());
  std::fflush(stderr);
}
}  // namespace Orca::PanicDiag
