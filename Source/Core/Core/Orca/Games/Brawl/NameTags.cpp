// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/NameTags.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/Orca/LocalStore.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/Games/Brawl/BrawlModule.h"

namespace Orca::UX
{
namespace
{
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GAME_GLOBAL_NAME_RECORDS = 0x28;
constexpr u32 RECORDS_TAGS = 0xE0;
constexpr u32 TAG_SIZE = 0x124;
constexpr u32 TAG_RUMBLE = 0x0C;
constexpr u32 TAG_CONTROLS = 0x14;
constexpr u32 CONTROLS_TEMPLATE = 0x80406938;
constexpr u32 CONTROLS_TEMPLATE_SIZE = CONTROLS_LAYOUT_SIZE;
// Flag byte offsets in the layout and the bits the game sets there: GameCube tap jump (0x80) and
// set up (0x70), Nunchuk shake smash (0x80), tap jump (0x40) and not set up (0x03), Classic tap
// jump (0x80). Every other byte is an action.
constexpr u32 FLAGS_GAMECUBE = 11, FLAGS_NUNCHUK = 0x1F, FLAGS_CLASSIC = 0x2C;
constexpr u8 BITS_GAMECUBE = 0xF0, BITS_NUNCHUK = 0xC3, BITS_CLASSIC = 0x80;
// The bit the game's Save on the GameCube page sets in that page's flag byte. No profile carries it
// (LayoutByte keeps only BITS_GAMECUBE) and Orca's writes leave it as it is, so a tag holding it
// was saved in the game's own menus.
constexpr u8 GAMECUBE_SAVED = 0x01;
// The game's own Controls page (Options > Controls > a tag), where a tag's buttons are set.
constexpr char CONTROLS_PAGE[] = "scButton";
// The highest action the menus can set. 0xE is "none".
constexpr u8 MAX_ACTION = 0x0E;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;
constexpr u32 TASK_AREAS = 0x44;
constexpr u32 AREA_INDEX = 0x1B0;
constexpr u32 AREA_PLAYER_KIND = 0x1B4;
constexpr u32 AREA_NAME_ID = 0x1C8;
constexpr u32 PLAYER_KIND_HUMAN = 1;
constexpr u32 NO_TAG = 0xFFFFFFFF;
constexpr char CHARACTER_SELECT[] = "scSelctCharacter";

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

bool SceneIs(const GuestMemory& m, std::string_view want)
{
  if (!Pointer(m, SCENE_MANAGER))
    return false;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + 4))
    return false;
  const u32 scene = m.Read32(manager + 4);
  if (!Pointer(m, scene))
    return false;
  const u32 name = m.Read32(scene);
  for (size_t i = 0; i <= want.size(); ++i)
  {
    if (!m.Valid(name + static_cast<u32>(i)))
      return false;
    const char c = static_cast<char>(m.Read8(name + static_cast<u32>(i)));
    if (c != (i < want.size() ? want[i] : '\0'))
      return false;
  }
  return true;
}

std::u16string TagAt(const GuestMemory& m, u32 tag)
{
  std::u16string name;
  for (u32 i = 0; i < TAG_CHARS; ++i)
  {
    const u16 c = m.Read16(tag + 2 * i);
    if (c == 0)
      break;
    name.push_back(static_cast<char16_t>(c));
  }
  return name;
}

// The tag holding this name; -1 when none does.
int FindTag(const GuestMemory& m, u32 tags, const std::u16string& name)
{
  for (int i = 0; i < TAG_SLOTS; ++i)
  {
    if (TagAt(m, tags + static_cast<u32>(i) * TAG_SIZE) == name)
      return i;
  }
  return -1;
}

// A tag character as a name reads: the game's name list types full-width forms (U+FF01-U+FF5E, so
// A is U+FF21; read from the tags it makes), Orca writes ASCII, and either case is the same name.
char16_t Folded(char16_t c)
{
  if (c >= 0xFF01 && c <= 0xFF5E)
    c = static_cast<char16_t>(c - 0xFF01 + 0x21);
  else if (c == 0x3000)
    c = u' ';
  if (c >= u'a' && c <= u'z')
    c = static_cast<char16_t>(c - u'a' + u'A');
  return c;
}

bool SameName(std::u16string_view a, std::u16string_view b)
{
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](char16_t x, char16_t y) { return Folded(x) == Folded(y); });
}

// The tag that reads as `name` (a YouGame username as BrawlTag makes it): one the player typed in
// the game's name list (the same letters in the game's characters, any case) before the one Orca
// made, so a player who made their tag in Options > Controls plays with it and never gets a second
// tag of the same name. -1 when none does.
int FindPlayerTag(const GuestMemory& m, u32 tags, const std::u16string& name)
{
  int made = -1;
  for (int i = 0; i < TAG_SLOTS; ++i)
  {
    const std::u16string tag = TagAt(m, tags + static_cast<u32>(i) * TAG_SIZE);
    if (tag == name)
    {
      if (made < 0)
        made = i;
    }
    else if (SameName(tag, name))
    {
      return i;
    }
  }
  return made;
}

// Creates a tag the way the game does: in the lowest unused slot, entry cleared, name, rumble on,
// default controls from CONTROLS_TEMPLATE. -1 when every tag is taken.
// The lowest slot, as the game's own name list does: Options > Controls stops the game for good
// when slot 0 is empty and another slot is used (through 0.3.38 Orca took the highest, slot 119).
// Slot i's entry is the TAG_SIZE bytes from records + i * TAG_SIZE, and its name is RECORDS_TAGS
// into it, so the entry starts RECORDS_TAGS before `tags + i * TAG_SIZE`. Clearing TAG_SIZE bytes
// from the name instead (through 0.3.38) cleared the next entry's first RECORDS_TAGS bytes, and for
// slot 119 the heap header and gmGlobalRecord right after the table.
int MakeTag(GuestMemory& m, u32 tags, const std::u16string& name, int unused = -1)
{
  for (int i = 0; i < TAG_SLOTS && unused < 0; ++i)
  {
    if (TagAt(m, tags + static_cast<u32>(i) * TAG_SIZE).empty())
      unused = i;
  }
  if (unused < 0 || !m.Valid(CONTROLS_TEMPLATE) ||
      !m.Valid(CONTROLS_TEMPLATE + CONTROLS_TEMPLATE_SIZE - 1))
  {
    return -1;
  }
  const u32 tag = tags + static_cast<u32>(unused) * TAG_SIZE;
  const u32 entry = tag - RECORDS_TAGS;
  for (u32 i = 0; i < TAG_SIZE; i += 2)
    m.Write16(entry + i, 0);
  for (u32 i = 0; i < name.size() && i < TAG_CHARS; ++i)
    m.Write16(tag + 2 * i, static_cast<u16>(name[i]));
  m.Write8(tag + TAG_RUMBLE, 1);
  for (u32 i = 0; i < CONTROLS_TEMPLATE_SIZE; ++i)
    m.Write8(tag + TAG_CONTROLS + i, m.Read8(CONTROLS_TEMPLATE + i));
  return unused;
}

// Whether another port already wears tag `slot`. The caller has checked the areas.
bool WornByAnotherPort(const GuestMemory& m, u32 task, int port, int slot)
{
  for (int other = 0; other < 4; ++other)
  {
    if (other == port)
      continue;
    const u32 at = task + TASK_AREAS + 4 * static_cast<u32>(other);
    const u32 area = m.Read32(at);
    if (Pointer(m, area) && Pointer(m, area + AREA_NAME_ID) &&
        m.Read32(area + AREA_NAME_ID) == static_cast<u32>(slot))
    {
      return true;
    }
  }
  return false;
}

// Two usernames can share their first five letters (sandbox-ada, sandbox-bo). Brawl never lets two
// players wear one tag, so the second port's tag ends in its port number instead (SAND2).
std::u16string Variant(std::u16string name, int port)
{
  const char16_t digit = static_cast<char16_t>(u'1' + port);
  if (name.size() < static_cast<size_t>(TAG_CHARS))
    name.push_back(digit);
  else
    name.back() = digit;
  return name;
}

// The bits a flag byte may hold; 0 for an action byte.
u8 FlagBits(u32 i)
{
  switch (i)
  {
  case FLAGS_GAMECUBE:
    return BITS_GAMECUBE;
  case FLAGS_NUNCHUK:
    return BITS_NUNCHUK;
  case FLAGS_CLASSIC:
    return BITS_CLASSIC;
  default:
    return 0;
  }
}

// Clamps a profile's layout byte to what the game's menus could set, else the default.
u8 LayoutByte(u32 i, u8 value, u8 default_value)
{
  if (const u8 bits = FlagBits(i))
    return value & bits;
  return value <= MAX_ACTION ? value : default_value;
}

// Writes rumble and layout into the tag, only bytes that differ. A flag byte's bits a profile
// doesn't carry stay as the tag has them (the GameCube page's Save sets 0x01 in its flag byte; it
// is not tap jump). The caller checked that the defaults are readable.
void WriteControls(GuestMemory& m, u32 tag, const std::vector<u8>& profile)
{
  const u8 rumble = profile[0] != 0 ? 1 : 0;
  if (m.Read8(tag + TAG_RUMBLE) != rumble)
    m.Write8(tag + TAG_RUMBLE, rumble);
  for (u32 i = 0; i < CONTROLS_LAYOUT_SIZE; ++i)
  {
    const u8 current = m.Read8(tag + TAG_CONTROLS + i);
    u8 value = LayoutByte(i, profile[1 + i], m.Read8(CONTROLS_TEMPLATE + i));
    if (const u8 bits = FlagBits(i))
      value = static_cast<u8>((current & ~bits) | value);
    if (current != value)
      m.Write8(tag + TAG_CONTROLS + i, value);
  }
}

// Whether the player saved this tag's GameCube page in the game's menus (Options > Controls). A
// game whose own new tags hold the bit (none known) never reads as saved. The caller has checked
// the template is readable.
bool SavedInGame(const GuestMemory& m, u32 tag)
{
  return (m.Read8(tag + TAG_CONTROLS + FLAGS_GAMECUBE) & GAMECUBE_SAVED) != 0 &&
         (m.Read8(CONTROLS_TEMPLATE + FLAGS_GAMECUBE) & GAMECUBE_SAVED) == 0;
}

// The tag's rumble byte and layout, as the game holds them (a profile's shape, unclamped).
std::vector<u8> TagProfile(const GuestMemory& m, u32 tag)
{
  std::vector<u8> profile;
  profile.reserve(CONTROLS_PROFILE_SIZE);
  profile.push_back(m.Read8(tag + TAG_RUMBLE));
  for (u32 i = 0; i < CONTROLS_LAYOUT_SIZE; ++i)
    profile.push_back(m.Read8(tag + TAG_CONTROLS + i));
  return profile;
}

bool TemplateReadable(const GuestMemory& m)
{
  return m.Valid(CONTROLS_TEMPLATE) && m.Valid(CONTROLS_TEMPLATE + CONTROLS_TEMPLATE_SIZE - 1);
}

// The character select's task, when that scene is running.
bool CharacterSelectTask(const GuestMemory& m, u32* task)
{
  if (!SceneIs(m, CHARACTER_SELECT))
    return false;
  const u32 scene = m.Read32(m.Read32(SCENE_MANAGER) + 4);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return false;
  *task = m.Read32(scene + SCENE_SELCHAR_TASK);
  return Pointer(m, *task + TASK_AREAS);
}

// The save's first tag, when the whole table is mapped.
bool TagTable(const GuestMemory& m, u32* tags)
{
  if (!Pointer(m, GAME_GLOBAL))
    return false;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (!Pointer(m, global + GAME_GLOBAL_NAME_RECORDS))
    return false;
  const u32 records = m.Read32(global + GAME_GLOBAL_NAME_RECORDS);
  *tags = records + RECORDS_TAGS;
  // The table is TAG_SLOTS entries of TAG_SIZE from `records` (the game's new(0x88E0)).
  return Pointer(m, records) && m.Valid(records + TAG_SLOTS * TAG_SIZE - 1);
}

// Port `port`'s player area on the character select, if a human joined it.
std::optional<u32> JoinedArea(const GuestMemory& m, u32 task, int port)
{
  const u32 at = task + TASK_AREAS + 4 * static_cast<u32>(port);
  if (!Pointer(m, at))
    return std::nullopt;
  const u32 area = m.Read32(at);
  if (!Pointer(m, area) || !Pointer(m, area + AREA_NAME_ID) ||
      m.Read32(area + AREA_INDEX) != static_cast<u32>(port) ||
      m.Read32(area + AREA_PLAYER_KIND) != PLAYER_KIND_HUMAN)
  {
    return std::nullopt;
  }
  return area;
}

// "P2" etc., for a port with controls but no showable name. Looks like the game's own label.
std::u16string PortLabel(int port)
{
  return {u'P', static_cast<char16_t>(u'1' + port)};
}
}  // namespace

std::u16string BrawlTag(std::string_view username, bool view_names)
{
  // A replay's anonymized "Player N" (view-time names) fits the 5-character tag as "PN", not "PLAYE".
  if (view_names && username.size() > 7 && username.size() <= 9 && username.starts_with("Player ") &&
      std::all_of(username.begin() + 7, username.end(), [](char c) { return c >= '0' && c <= '9'; }))
  {
    std::u16string tag = u"P";
    for (const char c : username.substr(7))
      tag.push_back(static_cast<char16_t>(c));
    return tag;
  }
  std::u16string tag;
  for (const char c : username)
  {
    if (tag.size() >= TAG_CHARS)
      break;
    if (c >= 'a' && c <= 'z')
      tag.push_back(static_cast<char16_t>(c - 'a' + 'A'));
    else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
             c == '!' || c == '?')
      tag.push_back(static_cast<char16_t>(c));
    else if (c == '_' || c == ' ')
      tag.push_back(u'-');
    // Anything else, including every byte of non-ASCII characters, is dropped.
  }
  return tag;
}

int ApplyNameTags(GuestMemory& m, const std::vector<Events::PortInfo>& ports, bool view_names)
{
  auto* frame = Net::ReplayScope::Current();
  std::vector<Net::TagAssignment>* assignments = nullptr;
  if (frame && !Net::ReplayScope::NetworkPlaying() && !Net::ReplayScope::RecordedPlaying())
  {
    // A live frame can run again after rollback with different RAM. Only this run's
    // decisions belong in the recording, including when it assigns no tags.
    frame->tag_assignments.emplace();
    assignments = &*frame->tag_assignments;
  }
  u32 task = 0, tags = 0;
  if (ports.empty() || !CharacterSelectTask(m, &task) || !TagTable(m, &tags))
    return 0;
  // In a room (more than one port in the synced list) a port's controls are the ones it joined
  // with, for the whole room, and nothing synced changes when a player edits their tag there.
  const bool room = ports.size() > 1;
  const auto tag_name = [view_names](const Events::PortInfo& p) {
    std::u16string name = BrawlTag(p.name, view_names);
    if (name.empty() && (view_names || p.controls.size() == CONTROLS_PROFILE_SIZE))
      name = PortLabel(p.port);
    return name;
  };
  int given = 0;
  for (const Events::PortInfo& p : ports)
  {
    if (p.port < 0 || p.port > 3)
      continue;
    const bool carries = p.controls.size() == CONTROLS_PROFILE_SIZE;
    const std::u16string name = tag_name(p);
    if (name.empty())
      continue;
    // Two players whose names make the same tag (sandbox-ada, sandbox-bo: SANDB) take it and its
    // variant in the order they join, so either may wear the one the other saved: theirs always get
    // their own controls.
    const bool shares_name =
        std::any_of(ports.begin(), ports.end(), [&](const Events::PortInfo& o) {
          return o.port != p.port && SameName(tag_name(o), name);
        });
    const std::optional<u32> area = JoinedArea(m, task, p.port);
    if (!area)
      continue;
    if (const u32 worn = m.Read32(*area + AREA_NAME_ID); worn != NO_TAG)
    {
      if (view_names && worn < static_cast<u32>(TAG_SLOTS))
      {
        const auto shown = shares_name ? Variant(name, p.port) : name;
        const u32 tag = tags + worn * TAG_SIZE;
        for (u32 i = 0; i < TAG_CHARS; ++i)
        {
          const u16 c = i < shown.size() ? static_cast<u16>(shown[i]) : 0;
          if (m.Read16(tag + 2 * i) != c)
            m.Write16(tag + 2 * i, c);
        }
      }
      // A worn tag is the player's menu choice, including the game's exact defaults.
      // Account controls apply only when Orca assigns an unworn tag below.
      continue;
    }
    std::u16string tag = name;
    int slot = FindPlayerTag(m, tags, tag);
    if (slot >= 0 && WornByAnotherPort(m, task, p.port, slot))
    {
      tag = Variant(name, p.port);
      slot = FindPlayerTag(m, tags, tag);
      if (slot >= 0 && WornByAnotherPort(m, task, p.port, slot))
        continue;
    }
    if (carries && !TemplateReadable(m))
      continue;
    const bool created = slot < 0;
    if (created)
      slot = MakeTag(m, tags, tag);
    if (slot < 0)
      continue;
    // A character select entered from the menus starts with the port wearing no tag, so this runs
    // again on each one. In a room, a tag the player saved in Options > Controls keeps what they
    // saved: writing the controls they joined with back into it undid every edit made in a room at
    // the next character select. Alone, the reader has already made the edit the port's own.
    // Limits: the bit means "saved in this run", not "in this room", so a friend back in the same
    // host's run keeps their earlier edit over the controls they bring, and a host who goes from
    // the menus straight into a room keeps a saved tag over newer own controls until a character
    // select alone (name-tags stage 1C makes room edits synced values).
    const u32 at = tags + static_cast<u32>(slot) * TAG_SIZE;
    const bool controls = carries && !(room && !shares_name && SavedInGame(m, at));
    if (controls)
      WriteControls(m, at, p.controls);
    m.Write32(*area + AREA_NAME_ID, static_cast<u32>(slot));
    if (assignments)
      assignments->push_back({p.port, slot, created, controls});
    ++given;
  }
  return given;
}

namespace
{
// The addresses are Brawl rev 2's (RSBE01), also booted by Project+'s launcher: the module says
// whether they hold (BrawlGame.h).
bool BrawlRev2()
{
  return Builtin().Active();
}
}  // namespace

std::string ControlsHex(const std::vector<u8>& profile)
{
  std::string hex;
  hex.reserve(profile.size() * 2);
  for (const u8 b : profile)
    hex += fmt::format("{:02x}", b);
  return hex;
}

bool ControlsValid(const std::vector<u8>& profile)
{
  if (profile.size() != CONTROLS_PROFILE_SIZE || profile[0] > 1)
    return false;
  for (u32 i = 0; i < CONTROLS_LAYOUT_SIZE; ++i)
  {
    const u8 v = profile[1 + i];
    const u8 bits = FlagBits(i);
    if (bits ? (v & ~bits) != 0 : v > MAX_ACTION)
      return false;
  }
  return true;
}

std::optional<std::vector<u8>> ParseControlsHex(std::string_view hex)
{
  if (hex.size() != CONTROLS_PROFILE_SIZE * 2)
    return std::nullopt;
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  std::vector<u8> profile;
  profile.reserve(CONTROLS_PROFILE_SIZE);
  for (size_t i = 0; i < hex.size(); i += 2)
  {
    const int hi = digit(hex[i]), lo = digit(hex[i + 1]);
    if (hi < 0 || lo < 0)
      return std::nullopt;
    profile.push_back(static_cast<u8>(hi << 4 | lo));
  }
  if (!ControlsValid(profile))
    return std::nullopt;
  return profile;
}

std::vector<u8> ReadOwnControls(const GuestMemory& m, int port, std::string_view own_name,
                                std::u16string* last_worn, std::u16string* read_tag,
                                std::u16string* last_edited)
{
  if (read_tag)
    read_tag->clear();
  u32 tags = 0;
  if (!TagTable(m, &tags))
    return {};
  int slot = -1;
  // On the character select, the tag the port wears is the one they play with.
  u32 task = 0;
  if (port >= 0 && port <= 3 && CharacterSelectTask(m, &task))
  {
    if (const std::optional<u32> area = JoinedArea(m, task, port))
    {
      const u32 id = m.Read32(*area + AREA_NAME_ID);
      if (id < static_cast<u32>(TAG_SLOTS))
      {
        if (std::u16string worn = TagAt(m, tags + id * TAG_SIZE); !worn.empty())
        {
          *last_worn = std::move(worn);
          slot = static_cast<int>(id);
          // What they wear now is what they play with, whatever they set up before.
          if (last_edited)
            last_edited->clear();
        }
      }
    }
  }
  // Elsewhere, first the tag they last set up on the game's Controls page (exactly that one, by
  // the name it has: whatever its name, those are the controls they just chose).
  if (slot < 0 && last_edited && !last_edited->empty())
    slot = FindTag(m, tags, *last_edited);
  // Then the tag they last wore, else their YouGame tag. A last-worn tag that reads as their
  // YouGame name is looked up as that name, so a tag of that name they type in Options > Controls
  // after wearing Orca's is the one read (and the one the next character select gives them).
  const std::u16string own = BrawlTag(own_name);
  if (slot < 0 && !last_worn->empty() && (own.empty() || !SameName(*last_worn, own)))
    slot = FindTag(m, tags, *last_worn);
  if (slot < 0 && !own.empty())
    slot = FindPlayerTag(m, tags, own);
  if (slot < 0)
    return {};
  const u32 tag = tags + static_cast<u32>(slot) * TAG_SIZE;
  if (read_tag)
    *read_tag = TagAt(m, tag);
  return TagProfile(m, tag);
}

std::optional<std::vector<u8>> OwnControlsWatch::Next(const std::vector<u8>& read,
                                                      std::u16string_view tag,
                                                      const std::vector<u8>& own, bool is_default)
{
  // Nothing readable (the menus before any tag exists) never clears a profile.
  if (read.empty())
    return std::nullopt;
  const bool switched = tag != m_tag;
  m_tag = tag;
  if (std::exchange(m_rebase, false))
  {
    m_last = read;
    return std::nullopt;
  }
  if (read == m_last)
    return std::nullopt;
  m_last = read;
  // Orca's own write showing up in the tag, or a player with no profile on the defaults.
  if (read == own || (own.empty() && is_default))
    return std::nullopt;
  // A switch to a tag at the game's defaults (one they made, or made again after a restart) is not
  // their controls changing, so the defaults never replace their own. Editing the tag they wear
  // back to the defaults still is a change.
  if (switched && is_default)
    return std::nullopt;
  return read;
}

namespace
{
std::atomic<bool> s_rebase_own{false};
std::atomic<bool> s_own_loaded{false};
// The app's controls came before the game's profile was known: kept once it is.
std::atomic<bool> s_keep_pending{false};

// This game's controls are kept between runs by the engine (Orca/LocalStore.h: the user folder's
// Config, one file per game), so this file does no file I/O.
void KeepOwnControls(const std::vector<u8>& profile)
{
  if (!Orca::LocalStore::KeepControls(ControlsHex(profile) + "\n"))
    WARN_LOG_FMT(ROLLBACK, "Name tags: couldn't keep this player's controls");
}

// The game's defaults with rumble on, as a tag Orca makes holds them.
bool IsDefaultProfile(const GuestMemory& m, const std::vector<u8>& profile)
{
  if (profile.size() != CONTROLS_PROFILE_SIZE || profile[0] != 1 || !TemplateReadable(m))
    return false;
  for (u32 i = 0; i < CONTROLS_LAYOUT_SIZE; ++i)
  {
    if (profile[1 + i] != m.Read8(CONTROLS_TEMPLATE + i))
      return false;
  }
  return true;
}

// Only bits and actions the menus set, so a kept profile always parses back.
std::vector<u8> Sanitized(std::vector<u8> profile)
{
  profile[0] = profile[0] != 0 ? 1 : 0;
  for (u32 i = 0; i < CONTROLS_LAYOUT_SIZE; ++i)
    profile[1 + i] = LayoutByte(i, profile[1 + i], 0x0E);
  return profile;
}
}  // namespace

std::optional<std::u16string> ControlsPageEdits::Next(const GuestMemory& m)
{
  const bool page = SceneIs(m, CONTROLS_PAGE);
  if (!page && m_seen.empty())
    return std::nullopt;
  u32 tags = 0;
  if (!TagTable(m, &tags))
  {
    m_seen.clear();
    return std::nullopt;
  }
  // The page's first frame: what every tag holds before the player sets anything.
  const bool first = m_seen.empty();
  if (first)
    m_seen.resize(TAG_SLOTS);
  std::optional<std::u16string> edited;
  for (int i = 0; i < TAG_SLOTS; ++i)
  {
    const u32 tag = tags + static_cast<u32>(i) * TAG_SIZE;
    Seen now{TagAt(m, tag), {}};
    if (!now.name.empty())
      now.profile = Sanitized(TagProfile(m, tag));
    Seen& was = m_seen[static_cast<size_t>(i)];
    // The same tag (same slot, same name) with other controls: the player set it up. A tag made,
    // renamed or deleted is not an edit, and neither is a Save that changed nothing the controls
    // play (the Save bit alone: Sanitized drops it).
    if (!first && !edited && !now.name.empty() && now.name == was.name && now.profile != was.profile)
      edited = now.name;
    was = std::move(now);
  }
  // The first frame after the page is its last look (a write that lands as the page closes).
  if (!page)
    m_seen.clear();
  return edited;
}

void LoadOwnControls()
{
  if (s_own_loaded.exchange(true))
  {
    if (s_keep_pending.exchange(false) && Orca::ActiveProfile())
      KeepOwnControls(Events::OwnControls());
    return;
  }
  std::optional<std::vector<u8>> profile;
  const char* from = "";
  bool kept = false;
  if (const char* env = std::getenv("ORCA_CONTROLS"); env && *env)
  {
    profile = ParseControlsHex(StripWhitespace(env));
    from = "the app";
    if (!profile)
      WARN_LOG_FMT(ROLLBACK, "Name tags: ORCA_CONTROLS isn't a controls profile; ignored");
  }
  if (!profile)
  {
    if (const std::optional<std::string> text = Orca::LocalStore::KeptControls())
    {
      profile = ParseControlsHex(StripWhitespace(*text));
      from = "an earlier run";
      kept = true;
    }
  }
  if (!profile)
    return;
  NOTICE_LOG_FMT(ROLLBACK, "Name tags: this player's own controls from {}", from);
  // Kept ones, not the app's: the page takes them as the player's.
  if (kept)
    Orca::Status::Line("orca controls " + ControlsHex(*profile));
  Events::SetOwnControls(std::move(*profile));
}

bool SetOwnControlsFromApp(std::string_view hex)
{
  std::optional<std::vector<u8>> profile = ParseControlsHex(hex);
  if (!profile)
    return false;
  s_own_loaded = true;
  // The tag worn now still holds the old controls; the next character select writes these.
  s_rebase_own = true;
  if (Orca::ActiveProfile())
    KeepOwnControls(*profile);
  else
    s_keep_pending = true;
  Events::SetOwnControls(std::move(*profile));
  return true;
}

std::optional<std::vector<u8>> OwnControlsReader::Read(const GuestMemory& m,
                                                       const std::vector<Events::PortInfo>& ports,
                                                       const std::vector<u8>& own, u64 resyncs)
{
  // PortInfo.remote differs per machine, so it only picks what to read here, never what to write.
  const auto mine =
      std::find_if(ports.begin(), ports.end(), [](const Events::PortInfo& p) { return !p.remote; });
  if (mine == ports.end())
    return std::nullopt;
  // After a resync (keyframe load or back to solo), a tag with the remembered name may belong to
  // someone else's save, so forget it, and take what is worn now as no change.
  if (resyncs != m_resyncs)
  {
    m_resyncs = resyncs;
    m_last_worn.clear();
    m_last_edited.clear();
    m_edits = {};
    m_watch.Rebase();
  }
  if (std::exchange(m_rebase, false))
    m_watch.Rebase();
  // A tag the player just set up on the game's Controls page is the one read from now on, whatever
  // its name, until they wear one on a character select (through 0.3.40 only the worn, last-worn or
  // YouGame-name tag was read, so buttons set on any other tag were never kept or carried).
  std::optional<std::u16string> edited = m_edits.Next(m);
  const bool set_up = edited.has_value();
  if (edited)
    m_last_edited = std::move(*edited);
  std::u16string tag;
  const std::vector<u8> read =
      ReadOwnControls(m, mine->port, mine->name, &m_last_worn, &tag, &m_last_edited);
  // A tag they just set up to the game's defaults (tap jump back on) is their choice, not a switch
  // to a tag at the defaults.
  return m_watch.Next(read.empty() ? read : Sanitized(read), tag, own,
                      !set_up && IsDefaultProfile(m, read));
}

std::optional<std::vector<u8>> NameTagsFrame(GuestMemory& m,
                                             const std::vector<Events::PortInfo>& write_ports,
                                             const std::vector<Events::PortInfo>& ports,
                                             OwnControlsReader* reader, const std::vector<u8>& own,
                                             u64 resyncs)
{
  auto* frame = Net::ReplayScope::Current();
  if (Net::ReplayScope::RecordedPlaying() && frame->tag_assignments)
  {
    u32 task = 0, tags = 0;
    if (CharacterSelectTask(m, &task) && TagTable(m, &tags))
    {
      for (const auto& assignment : *frame->tag_assignments)
      {
        const auto port = std::ranges::find(write_ports, assignment.port, &Events::PortInfo::port);
        if (port == write_ports.end() || assignment.slot < 0 || assignment.slot >= TAG_SLOTS)
          continue;
        const auto area = JoinedArea(m, task, assignment.port);
        if (!area)
          continue;
        if (assignment.created)
          MakeTag(m, tags, BrawlTag(port->name, Net::ReplayScope::ViewNamesPlaying()), assignment.slot);
        if (assignment.controls && port->controls.size() == CONTROLS_PROFILE_SIZE &&
            TemplateReadable(m))
          WriteControls(m, tags + static_cast<u32>(assignment.slot) * TAG_SIZE, port->controls);
        m.Write32(*area + AREA_NAME_ID, static_cast<u32>(assignment.slot));
      }
      if (Net::ReplayScope::ViewNamesPlaying())
      {
        auto worn_ports = write_ports;
        std::erase_if(worn_ports, [&](const auto& port) {
          const auto area = JoinedArea(m, task, port.port);
          return !area || m.Read32(*area + AREA_NAME_ID) >= static_cast<u32>(TAG_SLOTS);
        });
        ApplyNameTags(m, worn_ports, true);
      }
    }
  }
  else
  {
    ApplyNameTags(m, write_ports, Net::ReplayScope::ViewNamesPlaying());
  }
  // After the writes: a tag that just got the player's controls reads as those, never as the
  // game's defaults it held before this frame's writes.
  if (!reader)
    return std::nullopt;
  return reader->Read(m, ports, own, resyncs);
}

void NameTagsFrameHook(const Core::CPUThreadGuard& guard,
                       const std::vector<Events::PortInfo>& write_ports,
                       const std::vector<Events::PortInfo>& ports, bool read_own)
{
  if (!BrawlRev2())
    return;
  static OwnControlsReader s_reader;
  if (read_own && s_rebase_own.exchange(false))
    s_reader.Rebase();
  GuardMemory memory(guard);
  std::optional<std::vector<u8>> changed =
      read_own ? NameTagsFrame(memory, write_ports, ports, &s_reader, Events::OwnControls(),
                               Events::Resyncs()) :
                 NameTagsFrame(memory, write_ports, ports, nullptr, {}, 0);
  if (!changed)
    return;
  NOTICE_LOG_FMT(ROLLBACK, "Name tags: this player changed their controls in the game");
  Orca::Status::Line("orca controls " + ControlsHex(*changed));
  KeepOwnControls(*changed);
  Events::SetOwnControls(std::move(*changed));
}

}  // namespace Orca::UX
