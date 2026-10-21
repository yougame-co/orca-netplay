// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <random>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <cstdio>

#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/MemArena.h"
#include "Core/MemTools.h"
#include "Core/Rollback/Cow.h"
#include "Core/Rollback/Rollback.h"
#include "Core/Rollback/UndoLog.h"

namespace
{
// ---- UndoLog over a fake memory, writes reported explicitly ----

constexpr std::size_t PAGE = 64;

struct FakeMemory
{
  explicit FakeMemory(std::size_t pages) : bytes(pages * PAGE), log(PAGE, pages) {}

  // What a fault handler does before the write lands.
  void Write(std::size_t page, std::size_t offset, u8 value)
  {
    log.Record(page, &bytes[page * PAGE]);
    bytes[page * PAGE + offset] = value;
  }

  // What a load does: pre-images back, then the log rewinds.
  void Restore(u64 id)
  {
    log.ForEachPreImage(id, [this](std::size_t page, const u8* data) {
      std::memcpy(&bytes[page * PAGE], data, PAGE);
    });
    ASSERT_TRUE(log.RewindTo(id));
  }

  std::vector<u8> Rebuilt(u64 id) const
  {
    std::vector<u8> copy = bytes;
    log.ForEachPreImage(id, [&copy](std::size_t page, const u8* data) {
      std::memcpy(&copy[page * PAGE], data, PAGE);
    });
    return copy;
  }

  std::vector<u8> bytes;
  Rollback::UndoLog log;
};

TEST(OrcaUndoLog, RestoresEverySnapshotNewestFirst)
{
  FakeMemory memory(8);
  std::map<u64, std::vector<u8>> expected;
  for (int s = 0; s < 4; ++s)
  {
    const u64 id = memory.log.Open();
    expected[id] = memory.bytes;
    // Overlapping writes: page 1 in every interval, others in some.
    memory.Write(1, static_cast<std::size_t>(s), static_cast<u8>(10 + s));
    memory.Write(static_cast<std::size_t>(2 + s), 0, static_cast<u8>(20 + s));
    memory.Write(1, 7, static_cast<u8>(30 + s));  // second write: no new record
  }
  EXPECT_EQ(memory.log.PagesHeld(), 8u);  // page 1 four times, pages 2-5 once
  for (const auto& [id, bytes] : expected)
    EXPECT_EQ(memory.Rebuilt(id), bytes) << "snapshot " << id;

  // Load the second snapshot: the two newer ones are dropped, its own log is empty again.
  const u64 second = std::next(expected.begin())->first;
  memory.Restore(second);
  EXPECT_EQ(memory.bytes, expected[second]);
  EXPECT_EQ(memory.log.SnapshotCount(), 2u);
  EXPECT_EQ(memory.log.Newest(), second);
  EXPECT_FALSE(memory.log.LoggedInNewest(1));
  EXPECT_EQ(memory.Rebuilt(expected.begin()->first), expected.begin()->second);

  // Writes after the load are recorded against it again.
  memory.Write(1, 0, 99);
  EXPECT_TRUE(memory.log.LoggedInNewest(1));
  memory.Restore(second);
  EXPECT_EQ(memory.bytes, expected[second]);
}

TEST(OrcaUndoLog, DropMergesIntoTheOlderSnapshot)
{
  FakeMemory memory(6);
  const u64 a = memory.log.Open();
  const std::vector<u8> at_a = memory.bytes;
  memory.Write(0, 0, 1);
  const u64 b = memory.log.Open();
  const std::vector<u8> at_b = memory.bytes;
  memory.Write(0, 1, 2);  // also in a's log: b's pre-image is dropped by the merge
  memory.Write(3, 0, 3);  // only in b's log: moves to a's
  const u64 c = memory.log.Open();
  const std::vector<u8> at_c = memory.bytes;
  memory.Write(3, 1, 4);
  memory.Write(5, 0, 5);

  memory.log.Drop(b);
  EXPECT_FALSE(memory.log.Has(b));
  EXPECT_EQ(memory.Rebuilt(a), at_a);
  EXPECT_EQ(memory.Rebuilt(c), at_c);

  // Dropping the newest: its pages now count as recorded in the one before.
  memory.log.Drop(c);
  EXPECT_EQ(memory.log.Newest(), a);
  EXPECT_TRUE(memory.log.LoggedInNewest(5));
  EXPECT_EQ(memory.Rebuilt(a), at_a);

  // Dropping the oldest throws its log away.
  const u64 d = memory.log.Open();
  const std::vector<u8> at_d = memory.bytes;
  memory.Write(4, 0, 6);
  memory.log.Drop(a);
  EXPECT_EQ(memory.log.SnapshotCount(), 1u);
  EXPECT_EQ(memory.Rebuilt(d), at_d);
  memory.Restore(d);
  EXPECT_EQ(memory.bytes, at_d);
}

// A ring's life, at random: saves, overwriting the oldest, re-saves, loads, against full copies.
TEST(OrcaUndoLog, RandomRingMatchesFullCopies)
{
  constexpr std::size_t PAGES = 32;
  constexpr std::size_t RING = 5;
  FakeMemory memory(PAGES);
  std::mt19937 rng(1234);
  std::vector<std::pair<u64, std::vector<u8>>> ring;  // oldest first
  for (int step = 0; step < 4000; ++step)
  {
    const int writes = static_cast<int>(rng() % 12);
    for (int w = 0; w < writes; ++w)
    {
      memory.Write(rng() % PAGES, rng() % PAGE, static_cast<u8>(rng()));
    }
    const u32 action = rng() % 10;
    if (action < 6 || ring.empty())
    {
      if (ring.size() == RING)
      {
        memory.log.Drop(ring.front().first);
        ring.erase(ring.begin());
      }
      ring.emplace_back(memory.log.Open(), memory.bytes);
    }
    else if (action < 7)
    {
      // Re-save the newest frame: drop it and save again.
      memory.log.Drop(ring.back().first);
      ring.pop_back();
      ring.emplace_back(memory.log.Open(), memory.bytes);
    }
    else
    {
      const std::size_t target = rng() % ring.size();
      memory.Restore(ring[target].first);
      ASSERT_EQ(memory.bytes, ring[target].second) << "step " << step;
      ring.resize(target + 1);
    }
    for (const auto& [id, bytes] : ring)
      ASSERT_EQ(memory.Rebuilt(id), bytes) << "step " << step << " snapshot " << id;
    ASSERT_EQ(memory.log.SnapshotCount(), ring.size());
  }
}

// ---- The tracker over real shared memory and real write faults ----

// Two areas (like MEM1 and MEM2), each mapped twice like Dolphin's RAM view and a fastmem view,
// plus the tracker's private aliases.
class CowMemory
{
public:
  CowMemory()
  {
    m_page = Rollback::Cow::PageSize();
    m_sizes = {16 * m_page, 24 * m_page};
    m_arena.GrabSHMSegment(m_sizes[0] + m_sizes[1], "orca-cow-test");
    for (int a = 0; a < 2; ++a)
    {
      const s64 offset = a == 0 ? 0 : static_cast<s64>(m_sizes[0]);
      for (int v = 0; v < 3; ++v)
        m_views[a][v] = static_cast<u8*>(m_arena.CreateView(offset, m_sizes[a]));
    }
  }

  ~CowMemory()
  {
    for (int a = 0; a < 2; ++a)
    {
      for (u8* view : m_views[a])
        m_arena.ReleaseView(view, m_sizes[a]);
    }
    m_arena.ReleaseSHMSegment();
  }

  bool Arm()
  {
    const std::vector<Rollback::Cow::Area> areas{
        {m_views[0][2], 0, static_cast<u32>(m_sizes[0])},
        {m_views[1][2], 0x10000000, static_cast<u32>(m_sizes[1])}};
    std::vector<Rollback::Cow::View> views;
    for (int a = 0; a < 2; ++a)
    {
      for (int v = 0; v < 2; ++v)
        views.push_back({m_views[a][v], areas[a].physical_address, areas[a].size, true});
    }
    return Rollback::Cow::Arm(this, areas, views);
  }

  // A write through view `v` (0 or 1), as a plain store or a memcpy that may cross pages.
  void Write(std::mt19937& rng)
  {
    const int a = static_cast<int>(rng() % 2);
    const int v = static_cast<int>(rng() % 2);
    u8* const base = m_views[a][v];
    const std::size_t offset = rng() % m_sizes[a];
    if (rng() % 2)
    {
      *reinterpret_cast<volatile u8*>(base + offset) = static_cast<u8>(rng());
    }
    else
    {
      u8 buffer[300];
      for (u8& b : buffer)
        b = static_cast<u8>(rng());
      const std::size_t length = std::min(sizeof(buffer), m_sizes[a] - offset);
      std::memcpy(base + offset, buffer, length);
    }
  }

  std::vector<u8> Copy() const
  {
    std::vector<u8> copy(m_views[0][2], m_views[0][2] + m_sizes[0]);
    copy.insert(copy.end(), m_views[1][2], m_views[1][2] + m_sizes[1]);
    return copy;
  }

  u64 Hash() const
  {
    return Rollback::RamChecksum({m_views[0][2], m_sizes[0]}, {m_views[1][2], m_sizes[1]});
  }

  u8* View(int area, int view) { return m_views[area][view]; }
  std::size_t Size(int area) const { return m_sizes[area]; }

private:
  Common::MemArena m_arena;
  std::size_t m_page = 0;
  std::array<std::size_t, 2> m_sizes{};
  std::array<std::array<u8*, 3>, 2> m_views{};
};

TEST(OrcaCow, WriteFaultsRestoreAndChecksum)
{
  if (!EMM::IsExceptionHandlerSupported())
    GTEST_SKIP() << "no fault handler on this platform";
  EMM::InstallExceptionHandler();
  {
    CowMemory memory;
    ASSERT_TRUE(memory.Arm());
    std::mt19937 rng(99);
    std::vector<std::pair<u64, std::vector<u8>>> ring;
    std::vector<u64> hashes;
    for (int step = 0; step < 300; ++step)
    {
      const int writes = static_cast<int>(rng() % 40);
      for (int w = 0; w < writes; ++w)
        memory.Write(rng);
      if (step % 7 == 6 && ring.size() > 1)
      {
        const std::size_t target = rng() % ring.size();
        std::size_t changed = 0;
        ASSERT_TRUE(Rollback::Cow::Restore(ring[target].first,
                                           [&changed](u32, u32 length) { changed += length; }));
        ASSERT_EQ(memory.Copy(), ring[target].second) << "step " << step;
        ring.resize(target + 1);
        hashes.resize(target + 1);
        continue;
      }
      if (ring.size() == 6)
      {
        Rollback::Cow::Drop(ring.front().first);
        ring.erase(ring.begin());
        hashes.erase(hashes.begin());
      }
      ring.emplace_back(Rollback::Cow::Snapshot(), memory.Copy());
      hashes.push_back(memory.Hash());
      for (std::size_t i = 0; i < ring.size(); ++i)
        ASSERT_EQ(Rollback::Cow::Checksum(ring[i].first), hashes[i]) << "step " << step;
    }

    // A write from another thread faults on that thread (macOS: the signal fallback).
    const u64 before = Rollback::Cow::Snapshot();
    const std::vector<u8> at_before = memory.Copy();
    std::thread writer([&memory] { std::memset(memory.View(1, 0), 0x5a, 3 * 4096); });
    writer.join();
    ASSERT_EQ(memory.View(1, 1)[100], 0x5a);
    ASSERT_TRUE(Rollback::Cow::Restore(before, [](u32, u32) {}));
    EXPECT_EQ(memory.Copy(), at_before);

    // Snapshots with nothing written let every page go cold (read-only) again.
    u64 cold = 0;
    for (int i = 0; i < 260; ++i)
      cold = Rollback::Cow::Snapshot();
    const std::vector<u8> at_cold = memory.Copy();
    u8* const target = memory.View(0, 0) + 10;
    const std::size_t length = 2 * Rollback::Cow::PageSize();
#ifndef _WIN32
    // A system call writing a protected page fails instead of faulting...
    std::vector<u8> source(4096, 7);
    FILE* const file = std::tmpfile();
    ASSERT_NE(file, nullptr);
    const int fd = fileno(file);
    ASSERT_EQ(pwrite(fd, source.data(), source.size(), 0), 4096);
    EXPECT_EQ(pread(fd, target, 4096, 0), -1);
    EXPECT_EQ(errno, EFAULT);
    // ...unless prepared first.
    Rollback::Cow::PrepareHostWrite(target, length);
    EXPECT_EQ(pread(fd, target, 4096, 0), 4096);
    std::fclose(file);
    EXPECT_EQ(memory.View(0, 1)[10 + 4095], 7);
#else
    Rollback::Cow::PrepareHostWrite(target, length);
#endif
    std::memset(target, 1, length);
    // A store straddling a writable page and a protected one.
    const std::size_t page = Rollback::Cow::PageSize();
    memory.View(1, 1)[page - 1] = 3;
    const u8 wide[32] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
                         9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    std::memcpy(memory.View(1, 1) + page - 16, wide, sizeof(wide));
    EXPECT_EQ(memory.View(1, 0)[page + 15], 9);
    ASSERT_TRUE(Rollback::Cow::Restore(cold, [](u32, u32) {}));
    EXPECT_EQ(memory.Copy(), at_cold);
    ASSERT_TRUE(Rollback::Cow::Restore(before, [](u32, u32) {}));
    EXPECT_EQ(memory.Copy(), at_before);

    Rollback::Cow::Disarm(&memory);
    // Writable everywhere again: no fault handler involved.
    memory.View(0, 0)[0] = 1;
    memory.View(1, 1)[0] = 2;
    EXPECT_FALSE(Rollback::Cow::Has(before));
  }
  EMM::UninstallExceptionHandler();
}
}  // namespace
