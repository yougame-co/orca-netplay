// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/Relabel.h"

#include <algorithm>
#include <atomic>
#include <string_view>

#include "Common/Logging/Log.h"
#include "Core/Orca/Games/Brawl/BrawlStages.h"
#include "Core/Orca/Games/Brawl/CharOrder.h"
#include "Core/Orca/Games/Brawl/MenuText.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "Core/Orca/Games/Brawl/PPlus/RankedPPlus.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Orca/Games/Brawl/StageCursors.h"

namespace Orca::UX::Relabel
{
namespace
{
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x4;
constexpr u32 CSS_ARCHIVE = 0x410;  // scSelctCharacter's gfArchive* (CssTitle.h)
// scSelStage's gfArchive offset is unknown, so the scene's first words are scanned for it.
constexpr u32 SSS_SCAN_WORDS = 0x800 / 4;
// Tried before the scan: +0x450 in both Brawl rev 2 and Project+, as observed by the scan.
constexpr u32 SSS_ARCHIVE_WORD = 0x450 / 4;
constexpr u32 ARCHIVE_WORDS = 0x80 / 4;
constexpr u32 ARC_MAGIC = 0x41524300;  // "ARC\0"
constexpr u32 ARC_NAME = 0x10;
constexpr u32 ARC_FIRST = 0x40;
constexpr u32 CSS_FILE = 30;  // character select bres (MiscData 30)
constexpr u32 SSS_FILE = 20;  // stage select bres (MiscData 20)

constexpr u32 BRES_MAGIC = 0x62726573;  // "bres"
constexpr u32 BRES_ROOT = 0xC;
constexpr u32 ROOT_GROUP = 0x8;
constexpr u32 GROUP_COUNT = 0x4;
constexpr u32 GROUP_ENTRIES = 0x8;
constexpr u32 ENTRY_SIZE = 0x10;
constexpr u32 ENTRY_NAME = 0x8;
constexpr u32 ENTRY_DATA = 0xC;
constexpr u32 MAX_ENTRIES = 1024;
constexpr u32 TEX0_MAGIC = 0x54455830;  // "TEX0"
constexpr u32 TEX0_DATA = 0x10;
constexpr u32 TEX0_WIDTH = 0x1C;
constexpr u32 TEX0_HEIGHT = 0x1E;
constexpr u32 TEX0_FORMAT = 0x20;
constexpr u32 PLT0_MAGIC = 0x504C5430;  // "PLT0"
constexpr u32 PLT0_DATA = 0x10;
constexpr u32 PLT0_FORMAT = 0x18;
constexpr u32 PLT0_COUNT = 0x1C;
constexpr u32 FORMAT_I4 = 0;
constexpr u32 FORMAT_IA8 = 3;
constexpr u32 PALETTE_RGB5A3 = 2;

constexpr std::string_view TEXTURES = "Textures(NW4R)";
constexpr std::string_view PALETTES = "Palettes(NW4R)";

std::atomic<u32> s_shown{0};  // packed Shown for Current(); bit 31 means set

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

bool StringAt(const GuestMemory& m, u32 at, std::string_view want)
{
  for (size_t i = 0; i <= want.size(); ++i)
  {
    const u32 a = at + static_cast<u32>(i);
    if (!m.Valid(a) || m.Read8(a) != (i < want.size() ? static_cast<u8>(want[i]) : 0))
      return false;
  }
  return true;
}

// The running scene, if its name is `name`.
u32 Scene(const GuestMemory& m, std::string_view name)
{
  if (!Pointer(m, SCENE_MANAGER))
    return 0;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return 0;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  return Pointer(m, scene) && StringAt(m, m.Read32(scene), name) ? scene : 0;
}

// The archive image named `name` among a gfArchive's first words, or 0.
u32 ImageIn(const GuestMemory& m, u32 archive, std::string_view name)
{
  if (!Pointer(m, archive) || !m.Valid(archive + ARCHIVE_WORDS * 4 - 1))
    return 0;
  for (u32 i = 0; i < ARCHIVE_WORDS; ++i)
  {
    const u32 image = m.Read32(archive + 4 * i);
    if (image % 32 == 0 && Pointer(m, image) && m.Valid(image + ARC_FIRST - 1) &&
        m.Read32(image) == ARC_MAGIC && StringAt(m, image + ARC_NAME, name))
    {
      return image;
    }
  }
  return 0;
}

// The bres in MiscData `file` of the image, or 0.
u32 Bres(const GuestMemory& m, u32 image, u32 file)
{
  if (!image)
    return 0;
  const u32 bres = FindArchiveFile(m, image, file);
  return Pointer(m, bres) && m.Valid(bres + 0x10 - 1) && m.Read32(bres) == BRES_MAGIC ? bres : 0;
}

u32 CssBres(const GuestMemory& m)
{
  const u32 scene = Scene(m, "scSelctCharacter");
  if (!scene || !Pointer(m, scene + CSS_ARCHIVE))
    return 0;
  return Bres(m, ImageIn(m, m.Read32(scene + CSS_ARCHIVE), "sc_selcharacter_en"), CSS_FILE);
}

// The stage select's bres. `word` gets the scene word that held its archive.
u32 SssBres(const GuestMemory& m, u32* word = nullptr)
{
  const u32 scene = Scene(m, "scSelStage");
  if (!scene || !m.Valid(scene + SSS_SCAN_WORDS * 4 - 1))
    return 0;
  // No cache: the result must depend on emulated memory alone, and a stale archive's bytes can
  // outlive it in the heap.
  if (const u32 image = ImageIn(m, m.Read32(scene + 4 * SSS_ARCHIVE_WORD), "sc_selmap_en"))
  {
    if (word)
      *word = SSS_ARCHIVE_WORD;
    return Bres(m, image, SSS_FILE);
  }
  for (u32 i = 1; i < SSS_SCAN_WORDS; ++i)
  {
    if (const u32 image = ImageIn(m, m.Read32(scene + 4 * i), "sc_selmap_en"))
    {
      if (word)
        *word = i;
      return Bres(m, image, SSS_FILE);
    }
  }
  return 0;
}

// The data of the entry named `name` in the bres index group at `group`, or 0.
u32 GroupEntry(const GuestMemory& m, u32 group, std::string_view name)
{
  if (!Pointer(m, group) || !m.Valid(group + GROUP_ENTRIES - 1))
    return 0;
  const u32 count = m.Read32(group + GROUP_COUNT);
  if (count > MAX_ENTRIES || !m.Valid(group + GROUP_ENTRIES + ENTRY_SIZE * (count + 1) - 1))
    return 0;
  for (u32 i = 1; i <= count; ++i)
  {
    const u32 entry = group + GROUP_ENTRIES + ENTRY_SIZE * i;
    const u32 at = group + m.Read32(entry + ENTRY_NAME);
    // A name is preceded by its length.
    if (!Pointer(m, at - 4) || m.Read32(at - 4) != name.size() || !StringAt(m, at, name))
      continue;
    return group + m.Read32(entry + ENTRY_DATA);
  }
  return 0;
}

// Folder `folder`'s entry `name` in the bres.
u32 Resource(const GuestMemory& m, u32 bres, std::string_view folder, std::string_view name)
{
  if (!bres)
    return 0;
  const u32 root = bres + m.Read16(bres + BRES_ROOT);
  return GroupEntry(m, GroupEntry(m, root + ROOT_GROUP, folder), name);
}

// A texture's data, if it is `name` in `bres` at `width` x `height` in `format`.
u32 TextureData(const GuestMemory& m, u32 bres, std::string_view name, u16 width, u16 height,
                u32 format)
{
  const u32 tex = Resource(m, bres, TEXTURES, name);
  if (!Pointer(m, tex) || !m.Valid(tex + 0x24 - 1) || m.Read32(tex) != TEX0_MAGIC ||
      m.Read16(tex + TEX0_WIDTH) != width || m.Read16(tex + TEX0_HEIGHT) != height ||
      m.Read32(tex + TEX0_FORMAT) != format)
  {
    return 0;
  }
  const u32 data = tex + m.Read32(tex + TEX0_DATA);
  return m.Valid(data) && m.Valid(data + 1) ? data : 0;
}

// The slot if its current value is the game's or one of its marks, else an empty slot.
Slot Checked(Slot slot, const GuestMemory& m)
{
  if (slot.kind == Slot::Kind::None || !slot.address || !slot.LabelOf(slot.Read(m)))
    return {};
  return slot;
}

}  // namespace

u16 Slot::ValueFor(int label, u16 now) const
{
  switch (kind)
  {
  case Kind::PaletteEntry:
    return static_cast<u16>(game + label);
  case Kind::HighNibble:
    return static_cast<u16>((((game + label) & 0xF) << 4) | (now & 0xF));
  case Kind::Byte:
    return static_cast<u16>((game + label) & 0xFF);
  case Kind::None:
    break;
  }
  return now;
}

std::optional<int> Slot::LabelOf(u16 now) const
{
  for (int label = 0; label <= labels; ++label)
  {
    if (ValueFor(label, now) == now)
      return label;
  }
  return std::nullopt;
}

u16 Slot::Read(const GuestMemory& m) const
{
  return kind == Kind::PaletteEntry ? m.Read16(address) : m.Read8(address);
}

void Slot::Write(GuestMemory& m, u16 value) const
{
  if (kind == Kind::PaletteEntry)
    m.Write16(address, value);
  else
    m.Write8(address, static_cast<u8>(value));
}

Slot FindBand(const GuestMemory& m)
{
  // READY TO FIGHT! is C4 352x36 with a 16-entry RGB5A3 palette. The mark is a transparent entry's
  // blue, one step per label. Brawl uses entry 0 (0x0222), Project+ entry 15 (0x0000); the other
  // checked entries tell the two apart.
  const u32 bres = CssBres(m);
  const u32 palette = Resource(m, bres, PALETTES, "MenSelchrReady01_2");
  if (!Pointer(m, palette) || !m.Valid(palette + 0x20 - 1) || m.Read32(palette) != PLT0_MAGIC ||
      m.Read32(palette + PLT0_FORMAT) != PALETTE_RGB5A3 || m.Read16(palette + PLT0_COUNT) != 16 ||
      !TextureData(m, bres, "MenSelchrReady01_2", 352, 36, 8))
  {
    return {};
  }
  const u32 data = palette + m.Read32(palette + PLT0_DATA);
  if (!m.Valid(data) || !m.Valid(data + 31))
    return {};
  if (m.Read16(data + 2) == 0x0FFF && m.Read16(data + 28) == 0xFFFF)
  {
    if (const Slot brawl = Checked({Slot::Kind::PaletteEntry, data, 0x0222, 3}, m);
        brawl.kind != Slot::Kind::None)
    {
      return brawl;
    }
  }
  if (m.Read16(data) == 0x9084)
    return Checked({Slot::Kind::PaletteEntry, data + 30, 0x0000, 3}, m);
  return {};
}

namespace
{
Slot LineIn(const GuestMemory& m, u32 bres)
{
  // STAGE SELECT (I4): Brawl's is 128x16 with first texel 1, Project+'s 189x30 with first texel 0.
  if (const u32 data = TextureData(m, bres, "MenSelmapBack07", 128, 16, FORMAT_I4))
    return Checked({Slot::Kind::HighNibble, data, 1, LINE_PICK_A_STAGE}, m);
  if (const u32 data = TextureData(m, bres, "MenSelmapBack07", 189, 30, FORMAT_I4))
    return Checked({Slot::Kind::HighNibble, data, 0, LINE_PICK_A_STAGE}, m);
  return {};
}

Slot TitleIn(const GuestMemory& m, u32 bres)
{
  // BRAWL / VERSUS (I4 120x24), first texel 0 in both games.
  const u32 data = TextureData(m, bres, "MenSelchrRtitle.30", 120, 24, FORMAT_I4);
  return data ? Checked({Slot::Kind::HighNibble, data, 0, 2}, m) : Slot{};
}

std::array<Slot, 2> LegendIn(const GuestMemory& m, u32 bres)
{
  // Project+'s legend: words (CornerInfo, I4 245x53) and buttons (CornerButtons, IA8 150x50, mark
  // in the first texel's intensity).
  std::array<Slot, 2> out{};
  if (const u32 data = TextureData(m, bres, "CornerInfo", 245, 53, FORMAT_I4))
    out[0] = Checked({Slot::Kind::HighNibble, data, 0, 1}, m);
  if (const u32 data = TextureData(m, bres, "CornerButtons", 150, 50, FORMAT_IA8);
      data && m.Valid(data + 1) && m.Read8(data) == 0)
  {
    out[1] = Checked({Slot::Kind::Byte, data + 1, 0, 1}, m);
  }
  return out;
}
}  // namespace

Slot FindLine(const GuestMemory& m)
{
  return LineIn(m, SssBres(m));
}

Slot FindTitle(const GuestMemory& m)
{
  return TitleIn(m, SssBres(m));
}

std::array<Slot, 2> FindLegend(const GuestMemory& m)
{
  return LegendIn(m, SssBres(m));
}

Band WantedBand(const GuestMemory& m, const std::vector<Events::PortInfo>& ports)
{
  // Ranked games after the first follow the character order. In Free order the loser is asked
  // first and the winner may lock in any time, so the winner's turn has no label.
  const CharOrder::CssView css = CharOrder::ReadCss(m);
  const CharOrder::State order = CharOrder::ReadState(m);
  if (css.on_css && order.game != 0)
  {
    const bool free = CharOrder::ReadSet(m).order == CharOrder::Order::Free;
    if (order.step == CharOrder::Step::First)
      return free ? Band::LoserNow : Band::WinnerFirst;
    if (order.step == CharOrder::Step::Second)
      return free ? Band::Game : Band::LoserNow;
  }
  // A queue room's ready step.
  const Queue::View view = Queue::ReadView(m, ports);
  if (view.queue2 && view.css && !view.solo && (view.plugged & 3) == 3 &&
      Queue::ReadyPhase(view) && !Queue::OrderLockedIn(view))
  {
    const Queue::State state = Queue::ReadState(m);
    // Not once both are locked in, the match is starting, or time ran out.
    if ((state.ready & 3) != 3 && !(state.flags & (Queue::FLAG_GO | Queue::FLAG_TIMED_OUT)))
      return Band::LockIn;
  }
  return Band::Game;
}

Line LineFor(u8 kind, u8 port, int left)
{
  const int p = port & 1;
  switch (static_cast<Ranked::StepKind>(kind))
  {
  case Ranked::StepKind::Strike:
    return static_cast<Line>(1 + 3 * p + (std::clamp(left, 1, 3) - 1));
  case Ranked::StepKind::Ban:
    return static_cast<Line>(7 + 2 * p + (std::clamp(left, 1, 2) - 1));
  case Ranked::StepKind::Pick:
    return port == Ranked::BOTH ? LINE_PICK_A_STAGE : static_cast<Line>(11 + p);
  case Ranked::StepKind::Prefer:
    return LINE_PICK_A_STAGE;
  }
  return LINE_GAME;
}

Line WantedLine(const GuestMemory& m)
{
  const Rules::Ruleset ruleset = Rules::ProfileRuleset();
  const std::optional<Ranked::Turn> turn =
      ruleset == Rules::Ruleset::Brawl ? BrawlStages::CurrentTurn(m) :
      ruleset == Rules::Ruleset::PPlus ? RankedPPlus::CurrentTurn(m) :
                                         std::nullopt;
  return turn ? LineFor(static_cast<u8>(turn->kind), turn->port, turn->left) : LINE_GAME;
}

Title WantedTitle(const GuestMemory& m)
{
  const Rules::Header header = Rules::ReadHeader(m);
  if (header.present && header.mode == Rules::Mode::Ranked)
    return Title::Ranked;
  if (header.present && header.mode == Rules::Mode::Casual)
    return Title::Casual;
  return Title::Game;
}

bool WantedLegend(const GuestMemory& m)
{
  // Only in a ranked stage select with Orca's two cursors (StageCursors.h). Casual has no strikes.
  const Rules::Header header = Rules::ReadHeader(m);
  return header.present && header.mode == Rules::Mode::Ranked && StageCursors::Read(m).has_value();
}

bool BandUp(const GuestMemory& m, const std::vector<Events::PortInfo>& ports)
{
  const Queue::View view = Queue::ReadView(m, ports);
  return view.css && view.ports[0].human && view.ports[0].placed && view.ports[1].human &&
         view.ports[1].placed;
}

bool ClockInBand(const GuestMemory& m, const std::vector<Events::PortInfo>& ports)
{
  return FindBand(m).kind != Slot::Kind::None && WantedBand(m, ports) != Band::Game &&
         BandUp(m, ports);
}

int Apply(GuestMemory& m, const std::vector<Events::PortInfo>& ports, Shown* shown)
{
  Shown now;
  int written = 0;
  const auto put = [&](const Slot& slot, int label) {
    if (slot.kind == Slot::Kind::None)
      return false;
    const u16 before = slot.Read(m);
    const u16 value = slot.ValueFor(label, before);
    if (value != before)
    {
      slot.Write(m, value);
      ++written;
    }
    return true;
  };
  if (const Slot band = FindBand(m); band.kind != Slot::Kind::None)
  {
    now.band = WantedBand(m, ports);
    now.band_up = BandUp(m, ports);
    put(band, static_cast<int>(now.band));
  }
  // Found once for the stage select's three textures.
  const u32 sss = SssBres(m);
  if (const Slot line = LineIn(m, sss); line.kind != Slot::Kind::None)
  {
    now.line = WantedLine(m);
    put(line, now.line);
  }
  if (const Slot title = TitleIn(m, sss); title.kind != Slot::Kind::None)
  {
    now.title = WantedTitle(m);
    put(title, static_cast<int>(now.title));
  }
  const std::array<Slot, 2> legend = LegendIn(m, sss);
  if (legend[0].kind != Slot::Kind::None || legend[1].kind != Slot::Kind::None)
  {
    now.legend = WantedLegend(m);
    put(legend[0], now.legend ? 1 : 0);
    put(legend[1], now.legend ? 1 : 0);
  }
  if (shown)
    *shown = now;
  return written;
}

Shown Frame(const Core::CPUThreadGuard& guard, bool resimulating,
            const std::vector<Events::PortInfo>& ports)
{
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return {};
  GuardMemory memory(guard);
  Shown shown;
  const int written = Apply(memory, ports, &shown);
  if (resimulating)
    return shown;
  // Logs which scene word held the stage select archive (source of SSS_ARCHIVE_WORD).
  static u32 s_logged_word = 0;
  if (u32 word = 0; written && SssBres(memory, &word) && word != s_logged_word)
  {
    s_logged_word = word;
    NOTICE_LOG_FMT(ROLLBACK, "Orca: relabels: the stage select's archive is its scene's word {} "
                   "(+0x{:x})", word, 4 * word);
  }
  s_shown.store(0x80000000u | (shown.band_up ? 1u << 14 : 0u) | (u32(shown.band) << 12) |
                    (u32(shown.line) << 4) | (u32(shown.title) << 1) | (shown.legend ? 1u : 0u),
                std::memory_order_relaxed);
  // Only logged outside re-simulation; a rollback re-run writes the same values again.
  if (written)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Orca: relabels band {} line {} title {} legend {}",
                   static_cast<int>(shown.band), shown.line, static_cast<int>(shown.title),
                   shown.legend);
  }
  return shown;
}

Shown Current()
{
  const u32 v = s_shown.load(std::memory_order_relaxed);
  if (!(v & 0x80000000u))
    return {};
  Shown s;
  s.band = static_cast<Band>((v >> 12) & 3);
  s.line = static_cast<Line>((v >> 4) & 0xF);
  s.title = static_cast<Title>((v >> 1) & 3);
  s.legend = v & 1;
  s.band_up = (v >> 14) & 1;
  return s;
}
}  // namespace Orca::UX::Relabel
