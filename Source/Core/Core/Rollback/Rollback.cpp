// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Rollback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <span>
#include <tuple>
#include <unordered_set>

#include <xxh3.h>

#if defined(_M_X86_64) || defined(__x86_64__)
#include <xmmintrin.h>
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
#include <intrin.h>
#endif

#include "Common/FPURoundMode.h"
#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/Orca/Activity.h"
#include "Core/Orca/JitWarm.h"
#include "Core/Orca/Perf.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/OrbCombo.h"
#include "Core/Orca/UX/Controllers.h"
#include "Core/Orca/UX/OverlayKey.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/Cow.h"
#include "Core/Rollback/Diag.h"
#include "Core/Rollback/Harness.h"
#include "Core/Rollback/InputGate.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/Rollback/SessionPort.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/LocalInput.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{
namespace
{
std::atomic<bool> s_in_snapshot{false};
std::atomic<bool> s_resimulating{false};
// Whether the frame now running is rendered, for the overlay's frame meter. CPU thread only.
bool s_frame_shown = true;
// Emulated ticks and host time when the frame now running began (after any load at its boundary),
// ticks 0 if unknown; how long the game held the shown frame that just ended (Events::GameHeldMs),
// and the fraction of a frame held over to the next hold. CPU thread only.
u64 s_frame_start_ticks = 0;
std::chrono::steady_clock::time_point s_frame_start_wall{};
double s_frame_held_ms = 0;
int s_frame_held_frames = 0;
double s_frame_held_carry = 0;

// The emulated length of the frame that just ended, past one video frame, if it was shown.
void TimeEndedFrame(Core::System& system)
{
  s_frame_held_ms = 0;
  s_frame_held_frames = 0;
  const u64 ticks = system.GetCoreTiming().GetTicks();
  if (!s_frame_shown || s_frame_start_ticks == 0 || ticks <= s_frame_start_ticks)
    return;
  const double rate = system.GetVideoInterface().GetTargetRefreshRate();
  const double frame_ms = 1000 / (rate > 0 ? rate : 59.94);
  const double emulated_ms = static_cast<double>(ticks - s_frame_start_ticks) * 1000 /
                             system.GetSystemTimers().GetTicksPerSecond();
  const double wall_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - s_frame_start_wall)
                             .count();
  s_frame_held_ms = Orca::Events::GameHeldMs(emulated_ms, wall_ms, frame_ms);
  s_frame_held_frames = Orca::Events::GameHeldFrames(s_frame_held_ms, frame_ms, s_frame_held_carry);
}

constexpr std::size_t RESTORE_PAGE = 4096;
constexpr u32 MEM1_VIRTUAL = 0x80000000u;
constexpr u32 MEM2_VIRTUAL = 0x90000000u;
// Effective to physical for MEM1/MEM2 and their uncached mirrors (0x8/0xC -> 0x0, 0x9/0xD -> 0x1).
constexpr u32 PHYSICAL_MASK = 0x1FFFFFFFu;

// Copies saved RAM back one page at a time, skipping pages that already match. Only changed pages
// have their JIT blocks invalidated; clearing the whole JIT (as a normal state load does) would
// recompile the hot code after every rollback. Changed pages are collected by physical address.
void RestoreRam(Core::System& system, u8* live, const std::vector<u8>& saved, u32 virtual_base,
                std::unordered_set<u32>* changed_pages)
{
  auto& jit = system.GetJitInterface();
  const std::size_t size = saved.size();
  for (std::size_t offset = 0; offset < size; offset += RESTORE_PAGE)
  {
    const std::size_t length = std::min(RESTORE_PAGE, size - offset);
    if (std::memcmp(live + offset, saved.data() + offset, length) == 0)
      continue;
    std::memcpy(live + offset, saved.data() + offset, length);
    const u32 address = virtual_base + static_cast<u32>(offset);
    jit.InvalidateICache(address, static_cast<u32>(length), true);
    changed_pages->insert(address & PHYSICAL_MASK);
  }
}

// The JIT also remembers per-instruction facts (GPU FIFO writes, paired quantization, no
// speculative constants). A restored page may hold different instructions, so forget its entries.
// Done here in bulk because the JIT's own per-word invalidation is too slow for thousands of pages.
void ForgetLearnedJitAddresses(Core::System& system, const std::unordered_set<u32>& changed_pages)
{
  // JitInterface only holds a JitBase (null for the interpreter). static_cast because MSVC builds
  // have RTTI off.
  auto* jit = static_cast<JitBase*>(system.GetJitInterface().GetCore());
  if (!jit || changed_pages.empty())
    return;
  const auto in_changed_page = [&changed_pages](u32 address) {
    return changed_pages.contains(address & PHYSICAL_MASK & ~static_cast<u32>(RESTORE_PAGE - 1));
  };
  std::erase_if(jit->js.fifoWriteAddresses, in_changed_page);
  std::erase_if(jit->js.pairedQuantizeAddresses, in_changed_page);
  std::erase_if(jit->js.noSpeculativeConstantsAddresses, in_changed_page);
}

// The NAND file system, which is always the host backend. Its journal undoes NAND writes.
IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}
}  // namespace

u64 RamChecksum(std::span<const u8> mem1, std::span<const u8> mem2)
{
  XXH3_state_t* const state = XXH3_createState();
  XXH3_64bits_reset(state);
  XXH3_64bits_update(state, mem1.data(), mem1.size());
  XXH3_64bits_update(state, mem2.data(), mem2.size());
  const u64 hash = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  return hash;
}

bool InSnapshotDoState()
{
  return s_in_snapshot.load(std::memory_order_relaxed);
}

bool IsResimulating()
{
  return s_resimulating.load(std::memory_order_relaxed);
}

void SetResimulating(bool resimulating)
{
  s_resimulating.store(resimulating, std::memory_order_relaxed);
}

SnapshotScope::SnapshotScope()
{
  s_in_snapshot.store(true, std::memory_order_relaxed);
  // In a session, host GPU state (EFB, texture cache, bounding box) never reaches emulated RAM, so
  // snapshots skip it: no GPU readback on save, no texture-cache reload on load.
  VideoCommon_SetRollbackSnapshot(true);
}

SnapshotScope::~SnapshotScope()
{
  VideoCommon_SetRollbackSnapshot(false);
  s_in_snapshot.store(false, std::memory_order_relaxed);
}

SnapshotRing::SnapshotRing(std::size_t slots) : m_slots(std::max<std::size_t>(slots, 2))
{
}

SnapshotRing::~SnapshotRing()
{
  Cow::Disarm(this);
}

void SnapshotRing::Forget(Slot* slot)
{
  if (slot->cow_id != 0)
    Cow::Drop(slot->cow_id);
  slot->cow_id = 0;
  slot->frame = -1;
}

bool SnapshotRing::UseCow(Core::System& system, bool* armed_now)
{
  *armed_now = false;
  // If tracking stopped under us (emulation stopped, or an unsupported RAM mapping), the
  // copy-on-write snapshots are gone.
  if (m_cow.value_or(false) && !Cow::IsArmedFor(this))
  {
    for (Slot& slot : m_slots)
    {
      if (slot.cow_id != 0)
      {
        slot.cow_id = 0;
        slot.frame = -1;
      }
    }
    m_cow.reset();
  }
  if (!m_cow)
  {
    m_cow = Orca::SessionActive() && !Orca::TestFullSnapshots() && Cow::ArmForSystem(system, this);
    *armed_now = *m_cow;
  }
  return *m_cow;
}

SnapshotRing::Slot* SnapshotRing::Find(s64 frame)
{
  for (Slot& slot : m_slots)
  {
    if (slot.frame == frame)
      return &slot;
  }
  return nullptr;
}

const SnapshotRing::Slot* SnapshotRing::Find(s64 frame) const
{
  for (const Slot& slot : m_slots)
  {
    if (slot.frame == frame)
      return &slot;
  }
  return nullptr;
}

bool SnapshotRing::Has(s64 frame) const
{
  const Slot* slot = Find(frame);
  return slot && (slot->cow_id == 0 || Cow::Has(slot->cow_id));
}

void SnapshotRing::Reset(Core::System& system)
{
  for (Slot& slot : m_slots)
  {
    slot.frame = -1;
    slot.cow_id = 0;
  }
  Cow::Disarm(this);
  m_cow.reset();
  m_next = 0;
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
    nand->JournalStop();
}

bool SnapshotRing::Save(Core::System& system, s64 frame)
{
  // Re-saving a frame after a rewind reuses its slot; otherwise take the next one.
  Slot* slot = Find(frame);
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  m_last = static_cast<std::size_t>(slot - m_slots.data());
  Forget(slot);
  bool armed_now;
  const bool cow = UseCow(system, &armed_now);
  const auto buffers = [slot] {
    return std::tuple{slot->state.data(), slot->mem1.capacity(), slot->mem2.capacity()};
  };
  const auto buffers_before = buffers();

  // Non-RAM state first, so anything DoState writes into RAM is in the RAM snapshot.
  const auto t0 = std::chrono::steady_clock::now();
  {
    SnapshotScope scope;
    slot->state_size = State::SaveToBufferForRollback(system, slot->state);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto ms = [](auto from, auto to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
  };

  auto& memory = system.GetMemory();
  // Copy-on-write: write-protect RAM again and open this snapshot's undo log. A 0 id means
  // tracking stopped, so fall back to a full copy.
  if (cow)
    slot->cow_id = Cow::Snapshot();
  if (slot->cow_id != 0)
  {
    if (Diag::SnapTime())
      Diag::AddCowSave(ms(t0, t1), ms(t1, std::chrono::steady_clock::now()));
  }
  else
  {
    Diag::CopyRam(&slot->mem1, memory.GetRAM(), memory.GetRamSize());
    const auto t2 = std::chrono::steady_clock::now();
    if (memory.GetEXRAM())
      Diag::CopyRam(&slot->mem2, memory.GetEXRAM(), memory.GetExRamSize());
    else
      slot->mem2.clear();
    if (Diag::SnapTime())
      Diag::AddSnapshotSave(ms(t0, t1), ms(t1, t2), ms(t2, std::chrono::steady_clock::now()));
  }
  slot->l1_cache.assign(memory.GetL1Cache(), memory.GetL1Cache() + memory.GetL1CacheSize());
  slot->frame = frame;

  // NAND changes older than the oldest snapshot can never be undone, so trim them.
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
  {
    slot->nand_journal_mark = nand->LastJournalMark();
    u64 oldest = slot->nand_journal_mark;
    for (const Slot& other : m_slots)
    {
      if (other.frame >= 0)
        oldest = std::min(oldest, other.nand_journal_mark);
    }
    nand->JournalTrim(oldest);
  }
  // Arming write-protects all of RAM once; that one-off cost counts as an allocation.
  return armed_now || buffers() != buffers_before;
}

bool SnapshotRing::Load(Core::System& system, s64 frame)
{
  return LoadSlot(system, frame, false);
}

bool SnapshotRing::LoadSlot(Core::System& system, s64 frame, bool redisplay)
{
  Slot* slot = Find(frame);
  if (!slot || slot->state_size == 0)
  {
    ERROR_LOG_FMT(CORE, "Rollback: no snapshot for frame {}", frame);
    return false;
  }

  // Restore RAM before device state: some devices (emulated Bluetooth) re-parse pending IOS
  // requests from RAM while loading, and live RAM may already hold a newer reply. The device-state
  // load writes no RAM in a session, since EFB copies stay on the GPU.
  const auto start = std::chrono::steady_clock::now();
  auto& memory = system.GetMemory();
  std::unordered_set<u32> changed_pages;
  if (slot->cow_id != 0)
  {
    // The undo logs of this snapshot and every newer one list exactly the pages written since.
    std::vector<u32> changed_blocks;
    if (!Cow::Restore(slot->cow_id, [&changed_blocks](u32 physical, u32 length) {
          for (u32 offset = 0; offset < length; offset += static_cast<u32>(RESTORE_PAGE))
            changed_blocks.push_back(physical + offset);
        }))
    {
      ERROR_LOG_FMT(CORE, "Rollback: the copy-on-write snapshot for frame {} is gone", frame);
      return false;
    }
    // MEM1 is at 0x80000000 and MEM2 at 0x90000000: both are physical | 0x80000000.
    auto& jit = system.GetJitInterface();
    for (const u32 physical : changed_blocks)
    {
      jit.InvalidateICache(physical | MEM1_VIRTUAL, static_cast<u32>(RESTORE_PAGE), true);
      changed_pages.insert(physical);
    }
  }
  else
  {
    RestoreRam(system, memory.GetRAM(), slot->mem1, MEM1_VIRTUAL, &changed_pages);
    if (memory.GetEXRAM() && !slot->mem2.empty())
      RestoreRam(system, memory.GetEXRAM(), slot->mem2, MEM2_VIRTUAL, &changed_pages);
  }
  std::memcpy(memory.GetL1Cache(), slot->l1_cache.data(), slot->l1_cache.size());
  ForgetLearnedJitAddresses(system, changed_pages);

  bool ok;
  {
    SnapshotScope scope;
    VideoCommon_SetSnapshotRedisplays(redisplay);
    ok = State::LoadFromBufferForRollback(
        system, std::span<u8>(slot->state.data(), slot->state_size));
    VideoCommon_SetSnapshotRedisplays(false);
  }
  if (!ok)
  {
    ERROR_LOG_FMT(CORE, "Rollback: snapshot for frame {} did not load", frame);
    return false;
  }
  // The NAND journal must have undone every change since this snapshot.
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system); nand && nand->TakeJournalError())
  {
    ERROR_LOG_FMT(CORE, "Rollback: the NAND could not be restored to frame {}", frame);
    return false;
  }

  // Later frames are about to be re-run and re-saved, so drop their snapshots.
  for (Slot& other : m_slots)
  {
    if (other.frame > frame || (other.cow_id != 0 && !Cow::Has(other.cow_id)))
      Forget(&other);
  }
  m_next = (static_cast<std::size_t>(slot - m_slots.data()) + 1) % m_slots.size();
  if (Diag::SnapTime())
  {
    Diag::AddRingLoad(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
        changed_pages.size());
  }
  return true;
}

bool SnapshotRing::Capture(Core::System& system, MachineImage* image, bool stop_journal)
{
  Common::UniqueBuffer<u8> state;
  std::size_t state_size;
  {
    SnapshotScope scope;
    state_size = State::SaveToBufferForRollback(system, state);
  }
  if (state_size == 0)
    return false;
  image->state.assign(state.data(), state.data() + state_size);
  auto& memory = system.GetMemory();
  image->mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
  if (memory.GetEXRAM())
    image->mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
  else
    image->mem2.clear();
  image->l1_cache.assign(memory.GetL1Cache(), memory.GetL1Cache() + memory.GetL1CacheSize());
  if (stop_journal)
  {
    if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
      nand->JournalStop();
  }
  return true;
}

bool SnapshotRing::LoadMachineImage(Core::System& system, MachineImage image, s64 frame)
{
  auto& memory = system.GetMemory();
  if (image.state.empty() || image.mem1.size() != memory.GetRamSize() ||
      image.mem2.size() != (memory.GetEXRAM() ? memory.GetExRamSize() : 0) ||
      image.l1_cache.size() != memory.GetL1CacheSize())
  {
    ERROR_LOG_FMT(CORE, "Rollback: a keyframe for frame {} doesn't fit this machine", frame);
    return false;
  }
  Slot* slot = Find(frame);
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  // Store as a full-copy slot (cow_id 0), so LoadSlot restores mem1/mem2 directly.
  Forget(slot);
  slot->state.reset(image.state.size());
  std::memcpy(slot->state.data(), image.state.data(), image.state.size());
  slot->state_size = image.state.size();
  slot->mem1 = std::move(image.mem1);
  slot->mem2 = std::move(image.mem2);
  slot->l1_cache = std::move(image.l1_cache);
  slot->frame = frame;
  slot->nand_journal_mark = 0;
  // A joining machine has nothing on screen yet, so show the keyframe's frame.
  return LoadSlot(system, frame, true);
}

std::optional<u64> SnapshotRing::RamChecksum(s64 frame) const
{
  const Slot* slot = Find(frame);
  if (!slot || slot->state_size == 0)
    return std::nullopt;
  // Copy-on-write snapshots keep no full copy: rebuild RAM from live RAM plus the undo logs.
  if (slot->cow_id != 0)
    return Cow::Checksum(slot->cow_id);
  return Rollback::RamChecksum(slot->mem1, slot->mem2);
}

std::span<const u8> SnapshotRing::LastState() const
{
  const Slot& slot = m_slots[m_last];
  return {slot.state.data(), slot.state_size};
}

std::optional<GCPadStatus> InputOverride(int port)
{
  if (const auto pad = RawInputOverride(port))
    return InputGate::GatePad(port, *pad);
  return std::nullopt;
}

std::optional<GCPadStatus> RawInputOverride(int port)
{
  if (const RingPort* session_port = RingPort::Active())
    return session_port->Pad(port);
  if (auto pad = Harness::InputOverride(port))
    return pad;
  // In a session, local controllers never reach the game, even during boot before the session
  // supplies pads: the game would latch a host pad's status (its origin, say) and desync later.
  if (Orca::SessionActive())
    return Orca::Net::DecodePad(Orca::Net::Pad{});
  return std::nullopt;
}

namespace
{
// The host's floating-point control register: FPCR on ARM64, MXCSR on x86-64, 0 elsewhere.
u64 HostFloatControl()
{
#if defined(_M_X86_64) || defined(__x86_64__)
  return _mm_getcsr();
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
  return _ReadStatusReg(ARM64_FPCR);
#elif defined(__aarch64__)
  u64 fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  return fpcr;
#else
  return 0;
#endif
}

// The CPU thread runs in the guest's floating-point mode (rounding, flush-to-zero, and FPCR.AH on
// ARM64), which also affects compiled C++. Under FPCR.AH, FNEG leaves a NaN's sign alone, and clang
// builds some integer constants with MOVI + FNEG, so they come out wrong. The hook therefore runs
// in the host's default mode and restores the guest's from FPSCR on exit (a load may change it).
class HostFloatScope
{
public:
  explicit HostFloatScope(PowerPC::PowerPCState& ppc_state) : m_ppc_state(ppc_state)
  {
    const u64 guest = HostFloatControl();
    Common::FPU::LoadDefaultSIMDState();
    static bool s_logged = false;
    if (!s_logged && guest != HostFloatControl())
    {
      s_logged = true;
      NOTICE_LOG_FMT(ROLLBACK,
                     "Frame hook: the CPU thread arrived in the guest's floating-point mode "
                     "({:#x}); the hook runs in the host's ({:#x})",
                     guest, HostFloatControl());
    }
  }
  ~HostFloatScope() { PowerPC::RoundingModeUpdated(m_ppc_state); }
  HostFloatScope(const HostFloatScope&) = delete;
  HostFloatScope& operator=(const HostFloatScope&) = delete;

private:
  PowerPC::PowerPCState& m_ppc_state;
};
}  // namespace

u64 HostFloatControlForTests()
{
  return HostFloatControl();
}

double LastFrameHeldMs()
{
  return s_frame_held_ms;
}

int LastFrameHeldFrames()
{
  return s_frame_held_frames;
}

void OnFrameBoundary(const Core::CPUThreadGuard& guard)
{
  HostFloatScope host_float(guard.GetSystem().GetPPCState());
  // A launcher profile's hook is installed before its loader puts the game in RAM; until then,
  // whatever runs at that address is not the game's frame loop.
  if (const Orca::Profile* profile = Orca::ActiveProfile();
      profile && !Orca::FrameHookLive(guard, *profile))
  {
    InputGate::Clear();
    s_frame_start_ticks = 0;
    s_frame_held_carry = 0;
    return;
  }
  Orca::Status::GameStarted();
  // A loader's codes rewrite game code without invalidating the JIT, so clear it once at the
  // profile's jit_clear_frame. Count only first runs (a joiner's catch-up counts; a rollback's
  // re-run does not) so every machine clears at the same frame.
  const RingPort* const port = RingPort::Active();
  if (!(port ? port->Resimulating() : IsResimulating()) && Orca::JitClearDue())
  {
    if (Diag::g_jit_code_log)
      Diag::JitCodeNote("the profile's JitClearFrame");
    guard.GetSystem().GetJitInterface().ClearSafe();
    NOTICE_LOG_FMT(ROLLBACK, "JIT cleared once: the loader's codes have landed");
  }
  // Report the frame that just ended to the overlay's frame meter. Host time only.
  TimeEndedFrame(guard.GetSystem());
  Orca::Events::NotifyBoundary(s_frame_shown, s_frame_held_ms, s_frame_held_frames);
  // Inside a JIT block pc and npc are stale, and a snapshot would record and later resume at a
  // wrong address. Pin both to the hook address so every snapshot resumes here; the rest of the
  // block runs normally and updates them on exit.
  if (const Orca::Profile* profile = Orca::ActiveProfile(); profile && profile->frame_hook)
  {
    auto& ppc_state = guard.GetSystem().GetPPCState();
    ppc_state.pc = *profile->frame_hook;
    ppc_state.npc = *profile->frame_hook;
  }

  // An online match runs its session here.
  if (Orca::Online::Enabled())
  {
    const auto local_pad = [](int local_seat) {
      const bool lead = local_seat == OnlineMatch::LocalSeat();
      int ordinal = lead ? 0 : Orca::Online::LocalIndex(local_seat);
      if (!lead && ordinal < 0 && !Orca::Online::InRoom())
        ordinal = OnlineMatch::LocalOrdinal(local_seat);
      if (ordinal < 0)
        return Orca::Net::EncodePad(Orca::UX::NeutralPad());
      static std::array<Orca::UX::OrbCombo, Orca::UX::MAX_LOCAL> co_combos{};
      const auto filter = [&](GCPadStatus pad, bool first, bool live) {
        return lead ? Orca::UX::FilterLocalPad(pad, first, live) :
            Orca::UX::FilterCoSeatPad(pad, co_combos[std::min(ordinal, Orca::UX::MAX_LOCAL - 1)],
                                    first, live);
      };
      // The local pad comes from a test script, the app's input stream, or the host's first
      // controller, and the YouGame shortcut (Up + Start) is filtered out before it is sent.
      // `first_run` uses the session's flag, which counts a joiner's catch-up as a first run; the
      // app stream times its input only on frames that are neither re-runs nor catch-up.
      const RingPort* const ring = RingPort::Active();
      const bool first_run = ring ? !ring->Resimulating() : !IsResimulating();
      // The activity lines (Orca/Activity.h) also see a script's or Dolphin's own pad; the
      // app's controllers they read from its stream themselves.
      if (const auto scripted = Harness::InputOverride(local_seat))
      {
        if (!IsResimulating())
          Orca::Activity::NoteLocalPad(
              Orca::Activity::PAD_SCRIPT | static_cast<u32>(local_seat), *scripted);
        return Orca::Net::EncodePad(filter(*scripted, first_run, true));
      }
      const bool live = ControlReference::GetInputGate();
      if (const auto from_app = Orca::Events::LocalPad(ordinal, !IsResimulating()))
        return Orca::Net::EncodePad(filter(
            *from_app, first_run, live || InputCommon::VirtualPadOnly()));
      if (!lead)
        return Orca::Net::EncodePad(Orca::UX::NeutralPad());
      g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
      g_controller_interface.UpdateInput();
      const GCPadStatus pad = Pad::GetStatus(0);
      if (!IsResimulating())
        Orca::Activity::NoteLocalPad(Orca::Activity::PAD_DOLPHIN, pad);
      return Orca::Net::EncodePad(filter(pad, first_run, live));
    };
    // While the session holds this frame on a friend's input, the same pad read again, only so a
    // talk press ends when Start is let go (UX/OrbCombo.h LocalPadWaiting). Host only: nothing
    // read here is recorded, sent, timed or noted, and it runs only while a talk press is down.
    const auto local_pad_waiting = [](int local_seat) {
      if (!Orca::UX::OverlayKey::Pad().IsDown())
        return;
      // A test script's pad changes only with frames.
      if (Harness::InputOverride(local_seat))
        return;
      // No focus check: the input gate is this thread's and moves only at the SI poll, so it is
      // the last frame's here. Focus lost mid-stall is handled when frames resume.
      if (const auto from_app = Orca::Events::LocalPad(false))
      {
        Orca::UX::LocalPadWaiting(from_app->button);
        return;
      }
      g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
      g_controller_interface.UpdateInput();
      Orca::UX::LocalPadWaiting(Pad::GetStatus(0).button);
    };
    const auto on_frame = [&guard](int frame, bool resimulating,
                                   const std::vector<Orca::Events::PortInfo>& ports, bool alone) {
      Orca::Events::NotifyFrame(guard, frame, resimulating, ports, alone);
    };
    if (const auto rewound_to =
            OnlineMatch::OnBoundary(guard.GetSystem(), local_pad, on_frame, local_pad_waiting))
      Harness::RewindTo(*rewound_to);
  }

  Harness::OnFrameBoundary(guard);

  // Compute the input gate's masks for the next frame from current RAM, after any snapshot load,
  // on every pass. Must precede the relatch below. See ORCA.md, "Input gate".
  InputGate::OnBoundary(guard);

  // The SI's last poll ran just before this hook, so relatch now or the game reads the previous
  // frame's pads (a full frame of added latency). Done on every pass, after any snapshot save or
  // load, so every machine relatches the same pads. See ORCA.md, "Input latency".
  if (Orca::SessionActive())
    guard.GetSystem().GetSerialInterface().RelatchInputs();

  // Presentation diagnostics include menus, loads and transitions without match warm-up.
  if (Orca::SessionActive() && Orca::Perf::Measuring() && !IsResimulating())
    Orca::Perf::NotePaceScene(guard);

  // JIT warm-up: precompile blocks earlier matches needed and record this match's for next time.
  // Only on first runs, and not while catching up, whose frames run back to back.
  if (Orca::SessionActive() && Orca::JitWarm::ScenesReadable() && !IsResimulating() &&
      !(RingPort::Active() && RingPort::Active()->CatchingUp()))
  {
    Orca::JitWarm::OnBoundary(guard);
    // This machine's speed counts frames only once a match is under way (Orca/Perf.h).
    if (Orca::Perf::Measuring())
      Orca::Perf::NoteScene(Orca::JitWarm::SceneIsMatch(guard));
  }

  // Skip host rendering for re-run frames. The emulated GPU still runs, so RAM and timing match.
  VideoCommon_SetSkipRender(IsResimulating() || Orca::TestSkipRender());
  s_frame_shown = !IsResimulating();
  // The app's activity lines read this machine's controllers before a frame that runs for the
  // first time in real time. Host-only: nothing it does reaches emulation.
  if (Orca::SessionActive())
    Orca::Activity::OnBoundary(!IsResimulating());
  s_frame_start_ticks = guard.GetSystem().GetCoreTiming().GetTicks();
  s_frame_start_wall = std::chrono::steady_clock::now();
}
}  // namespace Rollback
