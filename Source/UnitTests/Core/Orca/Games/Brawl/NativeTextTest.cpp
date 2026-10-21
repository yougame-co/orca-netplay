// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Games/Brawl/CharOrder.h"
#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/NativeText.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "Core/Orca/Games/Brawl/Relabel.h"
#include "Core/Orca/Games/Brawl/SetBlock.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Rollback/Rollback.h"

using namespace Orca::UX;
namespace NT = Orca::UX::NativeText;
namespace MB = Orca::UX::MatchBlock;

namespace
{
class FakeMemory final : public GuestMemory
{
public:
  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override
  {
    bytes[a] = v;
    ++writes;
  }
  void Write16(u32 a, u16 v) override
  {
    Write8(a, static_cast<u8>(v >> 8));
    Write8(a + 1, static_cast<u8>(v));
  }
  void Write32(u32 a, u32 v) override
  {
    Write16(a, static_cast<u16>(v >> 16));
    Write16(a + 2, static_cast<u16>(v));
  }
  void Fill(u32 a, u32 n, u8 v = 0)
  {
    for (u32 i = 0; i < n; ++i)
      bytes[a + i] = v;
  }
  void Put32(u32 a, u32 v)
  {
    for (int i = 0; i < 4; ++i)
      bytes[a + i] = static_cast<u8>(v >> (24 - 8 * i));
  }
  void PutBytes(u32 a, const std::vector<u8>& v)
  {
    for (size_t i = 0; i < v.size(); ++i)
      bytes[a + static_cast<u32>(i)] = v[i];
  }
  void PutString(u32 a, std::string_view s)
  {
    for (size_t i = 0; i < s.size(); ++i)
      bytes[a + static_cast<u32>(i)] = static_cast<u8>(s[i]);
    bytes[a + static_cast<u32>(s.size())] = 0;
  }
  std::map<u32, u8> bytes;
  int writes = 0;
};

// The game's own 43-byte set-ups (from RAM dumps): a name plate's and the rules bar's.
const std::vector<u8> kPlateSetup{0x17, 0xff, 0xbf, 0x00, 0x04, 0x00, 0x3e, 0xff, 0xf0, 0x18, 0x01,
                                  0x03, 0x18, 0x10, 0x30, 0x1d, 0x20, 0x1c, 0x00, 0xcc, 0x0d, 0x40,
                                  0x40, 0x0f, 0xff, 0x00, 0x05, 0x01, 0x02, 0x02, 0x0c, 0xed, 0xce,
                                  0xd0, 0xff, 0x04, 0xed, 0xce, 0xd0, 0xff, 0x03, 0x00, 0x00};
const std::vector<u8> kRulesSetup{0x17, 0xff, 0x48, 0x00, 0x12, 0x00, 0xb8, 0xff, 0xee, 0x18, 0x00,
                                  0x03, 0x18, 0x10, 0x30, 0x1d, 0x20, 0x1c, 0x00, 0xcc, 0x0d, 0x40,
                                  0x40, 0x0f, 0xff, 0x00, 0x05, 0x01, 0x02, 0x01, 0x0c, 0xff, 0xff,
                                  0xff, 0xff, 0x04, 0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00};
// The rules bar's stock-battle message: width and colour codes around the words.
const std::vector<u8> kRulesBefore{0x11, 0x29, 0xff, 0xff, 0xff, 0x12, 0x02, 0x0c, 0x56, 0x54, 0x4f, 0xff};
const std::vector<u8> kRulesAfter{0x13, 0x11, 0x00, 0x00, 0x00, 0x00};

constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER = 0x805B8BA0;
constexpr u32 SCENE = 0x90FF6340;
constexpr u32 NAMES = 0x80701000;
constexpr u32 TASK = 0x815E75E0;
constexpr u32 AREAS = 0x81400000;  // area p at AREAS + p * 0x500
constexpr u32 BOXES = 0x81500000;  // box k at BOXES + k * 0x200 (its buffer at +0x80)
constexpr u32 RULES_BOX = 4;

u32 BoxAt(u32 k)
{
  return BOXES + k * 0x200;
}

// Message buffer object `k` holding `bytes` (set-up plus message), followed by the end byte.
void PutBox(FakeMemory& m, u32 k, const std::vector<u8>& bytes, u32 capacity = 256)
{
  const u32 object = BoxAt(k);
  const u32 buffer = object + 0x80;
  m.Fill(object, 0x80);
  m.Fill(buffer, capacity, 0xCC);
  m.Put32(object, NT::MESSAGE_BUFFER_VTABLE);
  m.Put32(object + NT::BUFFER_CAPACITY, capacity);
  m.Put32(object + NT::BUFFER_LENGTH, static_cast<u32>(bytes.size()));
  m.Put32(object + NT::BUFFER_DATA, buffer);
  m.PutBytes(buffer, bytes);
  m.bytes[buffer + static_cast<u32>(bytes.size())] = NT::END;
}

std::vector<u8> Concat(std::initializer_list<std::vector<u8>> parts)
{
  std::vector<u8> out;
  for (const auto& p : parts)
    out.insert(out.end(), p.begin(), p.end());
  return out;
}

std::vector<u8> Bytes(std::string_view s)
{
  return {s.begin(), s.end()};
}

std::vector<u8> RulesMessage(std::string_view words)
{
  return Concat({kRulesSetup, kRulesBefore, Bytes(words), kRulesAfter});
}

// The Versus character select: four player areas (bit p of `joined` = a human in area p), each
// with its plate ("PLAYER n" or "  NONE"), plus the rules bar.
FakeMemory Css(u8 joined)
{
  FakeMemory m;
  m.Put32(SCENE_MANAGER, MANAGER);
  m.Fill(MANAGER, 0x40);
  m.Put32(MANAGER + 4, SCENE);
  m.Fill(SCENE, 0x420);
  m.Put32(SCENE, NAMES);
  m.PutString(NAMES, "scSelctCharacter");
  m.Put32(SCENE + 0x400, TASK);
  m.Fill(TASK, 0x660);
  for (u32 p = 0; p < 4; ++p)
  {
    const u32 area = AREAS + p * 0x500;
    m.Fill(area, 0x448);
    m.Put32(TASK + 0x44 + 4 * p, area);
    m.Put32(area + 0x1B0, p);
    const bool human = (joined >> p) & 1;
    m.Put32(area + 0x1B4, human ? 1 : 0);
    m.Put32(area + 0x1B8, 0x28);  // no character
    m.Put32(area + 0x174, BoxAt(p));
    PutBox(m, p,
           Concat({kPlateSetup, Bytes(human ? "PLAYER " + std::to_string(p + 1) : "  NONE")}));
  }
  m.Put32(TASK + 0x538, BoxAt(RULES_BOX));
  PutBox(m, RULES_BOX, RulesMessage("-man survival test!"));
  return m;
}

// The READY TO FIGHT! band (Relabel.h): the scene's archive (+0x410) holds MiscData 30, a bres
// with the band's texture and palette. Returns the palette data's address.
constexpr u32 ARCHIVE_OBJECT = 0x934CDD80;
constexpr u32 IMAGE = 0x92F67FA0;  // 32-byte aligned
u32 PutReadyBand(FakeMemory& m)
{
  const u32 bres = IMAGE + 0x60, root = bres + 0x10, textures = bres + 0x100, palettes = bres + 0x180,
            tex = bres + 0x200, plt = bres + 0x280;
  const auto group = [&m](u32 at, std::vector<std::pair<std::string_view, u32>> entries) {
    const u32 count = static_cast<u32>(entries.size());
    m.Fill(at, 8 + 16 * (count + 1));
    m.Put32(at + 4, count);
    u32 names = at + 8 + 16 * (count + 1);
    for (u32 i = 1; i <= count; ++i)
    {
      const auto& [name, data] = entries[i - 1];
      m.Put32(names, static_cast<u32>(name.size()));
      m.PutString(names + 4, name);
      m.Put32(at + 8 + 16 * i + 8, names + 4 - at);
      m.Put32(at + 8 + 16 * i + 12, data);
      names = (names + 4 + static_cast<u32>(name.size()) + 1 + 3) & ~3u;
    }
  };
  m.Put32(SCENE + 0x410, ARCHIVE_OBJECT);
  m.Fill(ARCHIVE_OBJECT, 0x80);
  m.Put32(ARCHIVE_OBJECT + 0x20, IMAGE);
  m.Fill(IMAGE, 0x60);
  m.Put32(IMAGE, 0x41524300);
  m.Put32(IMAGE + 4, 1);  // one entry
  m.PutString(IMAGE + 0x10, "sc_selcharacter_en");
  m.Put32(IMAGE + 0x40, (1u << 16) | 30);  // MiscData 30
  m.Put32(IMAGE + 0x44, 0x300);
  m.Fill(bres, 0x300);
  m.Put32(bres, 0x62726573);
  m.bytes[bres + 0xD] = 0x10;
  group(root + 8, {{"Textures(NW4R)", textures - (root + 8)}, {"Palettes(NW4R)", palettes - (root + 8)}});
  group(textures, {{"MenSelchrReady01_2", tex - textures}});
  group(palettes, {{"MenSelchrReady01_2", plt - palettes}});
  m.Put32(tex, 0x54455830);
  m.Put32(tex + 0x10, 0x40);
  m.Put32(tex + 0x1C, (352u << 16) | 36);
  m.Put32(tex + 0x20, 8);  // C4
  m.Put32(plt, 0x504C5430);
  m.Put32(plt + 0x10, 0x40);
  m.Put32(plt + 0x18, 2);         // RGB5A3
  m.Put32(plt + 0x1C, 16u << 16);  // 16 entries
  for (u32 i = 0; i < 16; ++i)
  {
    const u16 v = i == 0 ? 0x0222 : i == 1 ? 0x0FFF : i == 14 ? 0xFFFF : 0x8421;
    m.bytes[plt + 0x40 + 2 * i] = static_cast<u8>(v >> 8);
    m.bytes[plt + 0x41 + 2 * i] = static_cast<u8>(v);
  }
  return plt + 0x40;
}

// Ports 1 and 2 have placed their tokens on a character.
void PlaceTokens(FakeMemory& m)
{
  for (u32 p = 0; p < 2; ++p)
    m.Put32(AREAS + p * 0x500 + 0x1B8, 3);
}

// A match block with a queue header (`flags`: MB::FLAG_QUEUE2, FLAG_SOLO), casual or ranked.
void PutHeader(FakeMemory& m, u8 flags, bool ranked)
{
  for (u32 i = 0; i < MB::FULL_SIZE; ++i)
    m.bytes[MB::BASE + i] = 0;
  m.Write32(MB::MAGIC, MB::MAGIC_VALUE);
  m.Write8(MB::VERSION, MB::VERSION_VALUE);
  m.Write8(MB::MODE, ranked ? MB::MODE_RANKED : MB::MODE_CASUAL);
  m.Write8(MB::RULESET, 1);
  m.Write8(MB::FLAGS, flags);
}

Orca::Events::PortInfo Port(int port, std::string name, int rating = -1, bool remote = false)
{
  Orca::Events::PortInfo p;
  p.port = port;
  p.name = std::move(name);
  p.remote = remote;
  if (rating >= 0)
  {
    Queue::Identity id;
    id.rating = rating;
    p.queue = Queue::EncodeIdentity(id);
  }
  return p;
}

std::string WordsOf(const FakeMemory& m, u32 k)
{
  const auto box = NT::ReadBox(m, BoxAt(k));
  return box ? NT::Words(m, *box) : "<no box>";
}
}  // namespace

TEST(OrcaNativeText, ReadsThePlatesAndTheRulesBarAsTheGameFormatsThem)
{
  const FakeMemory m = Css(0b0011);
  EXPECT_EQ(WordsOf(m, 0), "PLAYER 1");
  EXPECT_EQ(WordsOf(m, 1), "PLAYER 2");
  EXPECT_EQ(WordsOf(m, 2), "  NONE");
  // The rules bar's codes around its words are not words.
  const auto rules = NT::ReadBox(m, BoxAt(RULES_BOX));
  ASSERT_TRUE(rules);
  EXPECT_EQ(rules->words, kRulesSetup.size() + kRulesBefore.size());
  EXPECT_EQ(NT::Words(m, *rules), "-man survival test!");
}

TEST(OrcaNativeText, RejectsAnythingElse)
{
  FakeMemory m = Css(1);
  EXPECT_FALSE(NT::ReadBox(m, 0));
  EXPECT_FALSE(NT::ReadBox(m, BoxAt(0) + 2));  // unaligned
  FakeMemory wrong_vtable = m;
  wrong_vtable.Put32(BoxAt(0), 0x80454000);
  EXPECT_FALSE(NT::ReadBox(wrong_vtable, BoxAt(0)));
  FakeMemory no_end = m;
  no_end.bytes[BoxAt(0) + 0x80 + 51] = 0xCC;
  EXPECT_FALSE(NT::ReadBox(no_end, BoxAt(0)));
  FakeMemory not_a_setup = m;
  not_a_setup.bytes[BoxAt(0) + 0x80] = 0x18;
  EXPECT_FALSE(NT::ReadBox(not_a_setup, BoxAt(0)));
  FakeMemory too_long = m;
  too_long.Put32(BoxAt(0) + NT::BUFFER_LENGTH, 256);
  EXPECT_FALSE(NT::ReadBox(too_long, BoxAt(0)));
}

TEST(OrcaNativeText, SetWordsKeepsTheCodesAndTheLength)
{
  FakeMemory m = Css(1);
  const auto box = NT::ReadBox(m, BoxAt(RULES_BOX));
  ASSERT_TRUE(box);
  EXPECT_EQ(NT::SetWords(m, *box, "Press Start to lock in  0:27"), 1);
  const u32 buffer = BoxAt(RULES_BOX) + 0x80;
  const std::vector<u8> want = RulesMessage("Press Start to lock in  0:27");
  for (size_t i = 0; i < want.size(); ++i)
    EXPECT_EQ(m.Read8(buffer + static_cast<u32>(i)), want[i]) << i;
  EXPECT_EQ(m.Read8(buffer + static_cast<u32>(want.size())), NT::END);
  EXPECT_EQ(m.Read32(BoxAt(RULES_BOX) + NT::BUFFER_LENGTH), want.size());
  // Re-reading finds the new words; writing the same words again is a no-op.
  const auto again = NT::ReadBox(m, BoxAt(RULES_BOX));
  ASSERT_TRUE(again);
  EXPECT_EQ(NT::Words(m, *again), "Press Start to lock in  0:27");
  const int writes = m.writes;
  EXPECT_EQ(NT::SetWords(m, *again, "Press Start to lock in  0:27"), 0);
  EXPECT_EQ(m.writes, writes);
  // Shorter words: the length shrinks and the trailing codes move up.
  EXPECT_EQ(NT::SetWords(m, *again, "Both locked in"), 1);
  EXPECT_EQ(WordsOf(m, RULES_BOX), "Both locked in");
  EXPECT_EQ(m.Read32(BoxAt(RULES_BOX) + NT::BUFFER_LENGTH), RulesMessage("Both locked in").size());
}

TEST(OrcaNativeText, SetWordsNeverPassesTheCapacity)
{
  FakeMemory m;
  PutBox(m, 0, Concat({kPlateSetup, Bytes("PLAYER 1")}), 56);
  const auto box = NT::ReadBox(m, BoxAt(0));
  ASSERT_TRUE(box);
  EXPECT_EQ(NT::SetWords(m, *box, "ADA 1532"), 1);  // 43 + 8 + the end: 52
  const auto now = NT::ReadBox(m, BoxAt(0));
  ASSERT_TRUE(now);
  const int writes = m.writes;
  EXPECT_EQ(NT::SetWords(m, *now, "SANDBOX-ADA 1532"), 0);  // 43 + 16 + 1 > 56
  EXPECT_EQ(m.writes, writes);
  EXPECT_EQ(WordsOf(m, 0), "ADA 1532");
}

TEST(OrcaNativeText, TextTheFontCanDraw)
{
  EXPECT_EQ(NT::Ascii("ada · 1532"), "ada - 1532");
  EXPECT_EQ(NT::Ascii("1–0"), "1-0");
  EXPECT_EQ(NT::Ascii("Searching…"), "Searching...");
  EXPECT_EQ(NT::Ascii("café ✓!"), "caf !");
  EXPECT_EQ(NT::Ascii("abcdef", 4), "abcd");
  EXPECT_EQ(NT::PlateName("sandbox_ada"), "SANDBOX-AD");
  EXPECT_EQ(NT::PlateName("bo"), "BO");
  EXPECT_EQ(NT::PlateName("éé"), "");
  EXPECT_EQ(NT::Clock(30 * 60), "0:30");
  EXPECT_EQ(NT::Clock(30 * 60 - 1), "0:30");
  EXPECT_EQ(NT::Clock(29 * 60), "0:29");
  EXPECT_EQ(NT::Clock(0), "0:00");
  EXPECT_EQ(NT::Clock(-5), "0:00");
  EXPECT_EQ(NT::Clock(45 * 60), "0:45");
}

TEST(OrcaNativeText, AFriendsRoomNamesItsPlayersOnTheirPlates)
{
  // No queue header: joined, named ports get their names; the rules bar stays the game's.
  FakeMemory m = Css(0b0011);
  const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada"), Port(1, "bo", -1, true),
                                                  Port(2, "cy")};
  u8 shown = 0;
  EXPECT_EQ(NT::Apply(m, ports, &shown), 2);
  EXPECT_EQ(shown, 0b0011);
  EXPECT_EQ(WordsOf(m, 0), "ADA");
  EXPECT_EQ(WordsOf(m, 1), "BO");
  EXPECT_EQ(WordsOf(m, 2), "  NONE");  // port 3 hasn't joined its panel
  EXPECT_EQ(WordsOf(m, RULES_BOX), "-man survival test!");
  // Idempotent: a second pass writes nothing.
  const int writes = m.writes;
  EXPECT_EQ(NT::Apply(m, ports, &shown), 0);
  EXPECT_EQ(m.writes, writes);
  EXPECT_EQ(shown, 0b0011);
  // The game reformats a plate (tag picked, port rejoined): the next pass puts the name back.
  PutBox(m, 0, Concat({kPlateSetup, Bytes("PLAYER 1")}));
  EXPECT_EQ(NT::Apply(m, ports, &shown), 1);
  EXPECT_EQ(WordsOf(m, 0), "ADA");
}

TEST(OrcaNativeText, NothingOffTheCharacterSelect)
{
  FakeMemory m = Css(0b0011);
  m.PutString(NAMES, "scSelStage");
  u8 shown = 0xFF;
  EXPECT_EQ(NT::Apply(m, {Port(0, "ada")}, &shown), 0);
  EXPECT_EQ(shown, 0);
}

TEST(OrcaNativeText, TheSameWordsOnBothMachines)
{
  // Each machine sees the other player as remote, yet both must write identical bytes (no "you"),
  // since the text lives in emulated memory, which must stay identical on both.
  const auto room = [](bool host) {
    FakeMemory m = Css(0b0011);
    PutHeader(m, MB::FLAG_QUEUE2, true);
    m.Write8(MB::PLUGGED, 3);
    m.Write8(Queue::READY, 1);
    m.Write16(Queue::TIMER, 6 * 60);
    std::vector<Orca::Events::PortInfo> ports{Port(0, "ada", 1532, !host),
                                              Port(1, "bo", 1490, host)};
    NT::Apply(m, ports);
    return m;
  };
  const FakeMemory a = room(true);
  const FakeMemory b = room(false);
  EXPECT_EQ(a.bytes, b.bytes);
  // Plates show names only, no rating.
  EXPECT_EQ(WordsOf(a, 0), "ADA");
  EXPECT_EQ(WordsOf(a, 1), "BO");
  EXPECT_EQ(WordsOf(a, RULES_BOX), "Game 1: ADA locked in, BO to press Start  0:24");
}

TEST(OrcaNativeText, TheBandTakesTheTimerWhenItSaysLockIn)
{
  // Ranked ready step with both tokens down: the band reads PRESS START TO LOCK IN! and shows
  // the timer, so the rules bar drops its own.
  const auto room = [](bool host) {
    FakeMemory m = Css(0b0011);
    PutReadyBand(m);
    PutHeader(m, MB::FLAG_QUEUE2, true);
    m.Write8(MB::PLUGGED, 3);
    m.Write8(Queue::READY, 1);
    m.Write16(Queue::TIMER, 6 * 60);
    PlaceTokens(m);
    const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada", 1532, !host),
                                                    Port(1, "bo", 1490, host)};
    Relabel::Apply(m, ports);
    NT::Apply(m, ports);
    return m;
  };
  FakeMemory a = room(true);
  const FakeMemory b = room(false);
  EXPECT_EQ(a.bytes, b.bytes);
  const u32 band = Relabel::FindBand(a).address;
  EXPECT_EQ(a.Read16(band), 0x0223);  // LockIn
  EXPECT_EQ(WordsOf(a, RULES_BOX), "Game 1: ADA locked in, BO to press Start");
  const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada", 1532), Port(1, "bo", 1490, true)};
  EXPECT_TRUE(Relabel::ClockInBand(a, ports));
  // The frame hook passes Relabel's answer to NativeText; it must match.
  Relabel::Shown labels;
  Relabel::Apply(a, ports, &labels);
  EXPECT_TRUE(Relabel::ClockInBand(labels));
  const int writes = a.writes;
  EXPECT_EQ(Relabel::Apply(a, ports), 0);
  EXPECT_EQ(NT::Apply(a, ports), 0);
  EXPECT_EQ(a.writes, writes);
  // BO picks the token up: the band reverts to the game's and the bar shows the timer again.
  a.Write8(AREAS + 0x500 + 0x1F8, 1);
  EXPECT_FALSE(Relabel::ClockInBand(a, ports));
  Relabel::Apply(a, ports, &labels);
  EXPECT_FALSE(Relabel::ClockInBand(labels));
  NT::Apply(a, ports);
  EXPECT_EQ(WordsOf(a, RULES_BOX), "Game 1: ADA locked in, BO to press Start  0:24");
  a.Write8(AREAS + 0x500 + 0x1F8, 0);
  // Time's up: the band goes back to the game's words.
  a.Write8(Queue::FLAGS, Queue::FLAG_TIMED_OUT);
  a.Write8(Queue::TIMEOUT_WHO, 2);
  Relabel::Apply(a, ports);
  EXPECT_EQ(a.Read16(band), 0x0222);
  EXPECT_FALSE(Relabel::ClockInBand(a, ports));
}

TEST(OrcaNativeText, TheRulesBarFollowsTheQueue)
{
  const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada", 1532), Port(1, "bo", 1490, true)};
  const auto line = [&](auto setup) {
    FakeMemory m = Css(0b0011);
    PutHeader(m, MB::FLAG_QUEUE2, false);
    m.Write8(MB::PLUGGED, 3);
    setup(m);
    NT::Apply(m, ports);
    return WordsOf(m, RULES_BOX);
  };
  EXPECT_EQ(line([](FakeMemory&) {}), "Press Start to lock in  0:30");
  EXPECT_EQ(line([](FakeMemory& m) { m.Write8(Queue::READY, 2); }),
            "BO locked in, ADA to press Start  0:30");
  EXPECT_EQ(line([](FakeMemory& m) { m.Write8(Queue::READY, 3); }), "Both locked in");
  EXPECT_EQ(line([](FakeMemory& m) { m.Write8(Queue::FLAGS, Queue::FLAG_GO); }),
            "Both locked in, here we go!");
  EXPECT_EQ(line([](FakeMemory& m) {
              m.Write8(Queue::FLAGS, Queue::FLAG_TIMED_OUT);
              m.Write8(Queue::TIMEOUT_WHO, 2);
            }),
            "Time's up, BO didn't lock in");
  EXPECT_EQ(line([](FakeMemory& m) { m.Write8(MB::PLUGGED, 1); }),
            "Opponent found, waiting for BO...");
  // Later casual games: pick first, no timer.
  EXPECT_EQ(line([](FakeMemory& m) { m.Write8(MB::FOUGHT, 1); }),
            "Pick with A, then Start to lock in");
  // Plates show names only.
  FakeMemory m = Css(0b0011);
  PutHeader(m, MB::FLAG_QUEUE2, false);
  NT::Apply(m, ports);
  EXPECT_EQ(WordsOf(m, 0), "ADA");
}

TEST(OrcaNativeText, TheQueuesOwnCharacterSelect)
{
  FakeMemory m = Css(0b0001);
  PutHeader(m, MB::FLAG_QUEUE2 | MB::FLAG_SOLO, true);
  const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada", 1532)};
  u8 shown = 0;
  NT::Apply(m, ports, &shown);
  EXPECT_EQ(WordsOf(m, RULES_BOX), "Ranked: pick, then press Start to search");
  EXPECT_EQ(WordsOf(m, 0), "ADA");
  EXPECT_EQ(WordsOf(m, 1), "  NONE");
  EXPECT_EQ(shown, 0x01 | NT::SHOWN_RULES);
  // Searching: the opponent's empty seat says SEARCHING; the others stay the game's.
  m.Write8(Queue::READY, 1);
  NT::Apply(m, ports, &shown);
  EXPECT_EQ(WordsOf(m, RULES_BOX), "Ranked: searching for an opponent...  B to cancel");
  EXPECT_EQ(WordsOf(m, 1), "SEARCHING");
  EXPECT_EQ(WordsOf(m, 2), "  NONE");
  EXPECT_EQ(shown, 0x03 | NT::SHOWN_RULES);
  const int writes = m.writes;
  EXPECT_EQ(NT::Apply(m, ports, &shown), 0);
  EXPECT_EQ(m.writes, writes);
  // Cancelled: back to the game's own NONE.
  m.Write8(Queue::READY, 0);
  NT::Apply(m, ports, &shown);
  EXPECT_EQ(WordsOf(m, 1), "  NONE");
  EXPECT_EQ(shown, 0x01 | NT::SHOWN_RULES);
  EXPECT_EQ(NT::Apply(m, ports, &shown), 0);
}

// Project+: the plates take the names, but the rules bar keeps the game's words (its message has
// no width code, so the overlay shows those lines instead).
TEST(OrcaNativeText, ProjectPlusKeepsItsRulesBar)
{
  FakeMemory m = Css(0b0011);
  PutHeader(m, MB::FLAG_QUEUE2, true);
  const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada"), Port(1, "bo", 1532, true)};
  u8 shown = 0;
  EXPECT_EQ(NT::Apply(m, ports, &shown, false), 2);
  EXPECT_EQ(WordsOf(m, 0), "ADA");
  EXPECT_EQ(WordsOf(m, 1), "BO");
  EXPECT_EQ(WordsOf(m, RULES_BOX), "-man survival test!");
  EXPECT_EQ(shown, 0b0011);
  // Idempotent.
  EXPECT_EQ(NT::Apply(m, ports, &shown, false), 0);
}

TEST(OrcaNativeText, TheCharacterOrderAndTheSetInTheThirdPerson)
{
  // Ranked game 2 after ada won game 1: the winner picks first.
  const auto room = [](CharOrder::Step step, int elapsed) {
    FakeMemory m = Css(0b0011);
    PutHeader(m, MB::FLAG_QUEUE2, true);
    m.Write8(MB::PLUGGED, 3);
    m.Write8(MB::FOUGHT, 1);
    m.Write8(MB::BASE + SetBlock::SET_GAMES, 1);
    m.Write8(MB::BASE + SetBlock::SET_WINS, 1);
    m.Write8(MB::BASE + SetBlock::SET_WINNER, SetBlock::NO_PORT);
    m.Write8(MB::BASE + SetBlock::SET_LAST_WINNER, 0);
    CharOrder::State st;
    st.game = 2;
    st.step = step;
    st.first = 0;
    st.elapsed = static_cast<u16>(elapsed);
    CharOrder::WriteState(m, st);
    return m;
  };
  const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada", 1532), Port(1, "bo", 1490, true)};
  FakeMemory first = room(CharOrder::Step::First, 2 * 60);
  NT::Apply(first, ports);
  EXPECT_EQ(WordsOf(first, RULES_BOX), "ADA 1-0 BO: ADA picks, A then Start  0:43");
  FakeMemory second = room(CharOrder::Step::Second, 0);
  NT::Apply(second, ports);
  EXPECT_EQ(WordsOf(second, RULES_BOX), "ADA 1-0 BO: BO picks, A then Start  0:45");
  // The band names whose turn it is (Relabel.h). With both tokens down the band has the timer and
  // the bar doesn't.
  for (const auto& [step, label] : {std::pair{CharOrder::Step::First, 0x0224},
                                    std::pair{CharOrder::Step::Second, 0x0225}})
  {
    FakeMemory m = room(step, 0);
    const u32 band = PutReadyBand(m);
    EXPECT_EQ(Relabel::Apply(m, ports), 1);
    EXPECT_EQ(m.Read16(band), label);
    PlaceTokens(m);
    NT::Apply(m, ports);
    EXPECT_EQ(WordsOf(m, RULES_BOX), step == CharOrder::Step::First ?
                                         "ADA 1-0 BO: ADA picks, Start locks it in" :
                                         "ADA 1-0 BO: BO picks, Start locks it in");
  }
  // Set over: only the winner.
  FakeMemory over = room(CharOrder::Step::Idle, 0);
  over.Write8(MB::BASE + SetBlock::SET_WINS, 2);
  over.Write8(MB::BASE + SetBlock::SET_DONE, 1);
  over.Write8(MB::BASE + SetBlock::SET_WINNER, 0);
  NT::Apply(over, ports);
  EXPECT_EQ(WordsOf(over, RULES_BOX), "ADA wins the set 2-0");
}

TEST(OrcaNativeText, RecordingLabelsReplaceRealNames)
{
  auto live = Css(15);
  auto played = live;
  const std::vector<Orca::Events::PortInfo> ports{{0, "Alice", false}, {1, "Bob", true},
                                                {2, "Carol", true}, {3, "Dan", true}};
  const auto stored = Orca::Net::RecordingPorts(ports);
  for (int frame = 0; frame < 60; ++frame)
  {
    u8 live_shown = 0, replay_shown = 0;
    NT::Apply(live, ports, &live_shown);
    NT::Apply(played, stored, &replay_shown);
    EXPECT_EQ(live_shown, replay_shown);
  }
  const auto words = NT::Wanted(played, stored);
  EXPECT_EQ(words.plates[0], "P1");
  EXPECT_EQ(words.plates[3], "P4");
  EXPECT_EQ(WordsOf(live, 0), "ALICE");
  EXPECT_NE(live.bytes, played.bytes);
  NT::Apply(played, ports);
  EXPECT_EQ(WordsOf(played, 0), "ALICE");
}
