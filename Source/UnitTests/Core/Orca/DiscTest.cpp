// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Core/Orca/Disc.h"
#include "Core/Orca/Profile.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWii.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace
{
class FakeDevice final : public ciface::Core::Device
{
public:
  FakeDevice(std::string source, std::string name, int id)
      : m_source(std::move(source)), m_name(std::move(name))
  {
    SetId(id);
  }
  std::string GetName() const override { return m_name; }
  std::string GetSource() const override { return m_source; }

private:
  std::string m_source, m_name;
};
}  // namespace

TEST(OrcaDisc, MissingFileIsRefused)
{
  const Orca::DiscCheck check = Orca::CheckDisc("/nonexistent/brawl.iso", Orca::APP_GAME_ID);
  EXPECT_FALSE(check.ok);
  EXPECT_EQ(check.code, "disc_revision");
  EXPECT_FALSE(check.sentence.empty());
  EXPECT_FALSE(Orca::CheckDisc("", Orca::APP_GAME_ID).ok);
}

TEST(OrcaDisc, NonDiscFileIsRefused)
{
  const std::string path = File::CreateTempDir() + "/not-a-disc.iso";
  ASSERT_TRUE(File::WriteStringToFile(path, std::string(64 * 1024, 'x')));
  const Orca::DiscCheck check = Orca::CheckDisc(path, Orca::APP_GAME_ID);
  EXPECT_FALSE(check.ok);
  EXPECT_EQ(check.code, "disc_revision");
  File::DeleteDirRecursively(path.substr(0, path.rfind('/')));
}

// The -e file is checked before the boot reads it: a path with nothing there, or a file that won't
// open, is named on one line instead of a panic alert that waits forever.
TEST(OrcaDisc, BootFileThatIsGoneIsNamed)
{
  const Orca::DiscCheck gone = Orca::CheckBootFile("/nonexistent/brawl.iso");
  EXPECT_FALSE(gone.ok);
  EXPECT_EQ(gone.code, "disc_missing");
  EXPECT_EQ(gone.sentence, "/nonexistent/brawl.iso");
  EXPECT_EQ(Orca::CheckBootFile("").code, "disc_missing");
}

TEST(OrcaDisc, BootFileThatOpensPasses)
{
  const std::string dir = File::CreateTempDir();
  const std::string path = dir + "/brawl.iso";
  ASSERT_TRUE(File::WriteStringToFile(path, "x"));
  EXPECT_TRUE(Orca::CheckBootFile(path).ok);
  // A folder is the boot's own business (an extracted disc), not a missing file.
  EXPECT_TRUE(Orca::CheckBootFile(dir).ok);
  File::DeleteDirRecursively(dir);
}

#ifndef _WIN32
TEST(OrcaDisc, BootFileThatWontOpenIsNamed)
{
  if (geteuid() == 0)
    GTEST_SKIP() << "root reads any file";
  const std::string dir = File::CreateTempDir();
  const std::string path = dir + "/brawl.iso";
  ASSERT_TRUE(File::WriteStringToFile(path, "x"));
  ASSERT_EQ(chmod(path.c_str(), 0), 0);
  const Orca::DiscCheck locked = Orca::CheckBootFile(path);
  EXPECT_FALSE(locked.ok);
  EXPECT_EQ(locked.code, "disc_unreadable");
  EXPECT_EQ(locked.sentence, path);
  chmod(path.c_str(), 0600);
  File::DeleteDirRecursively(dir);
}
#endif

// With ORCA_TEST_DISC pointing at a Brawl disc image: it passes, and no other game ID matches it.
TEST(OrcaDisc, TheRightDiscPasses)
{
  const char* disc = std::getenv("ORCA_TEST_DISC");
  if (!disc || !File::Exists(File::GetSysDirectory() + "Orca/RSBE01.ini"))
    GTEST_SKIP() << "set ORCA_TEST_DISC to a Brawl (USA, Rev 2) image";
  const Orca::DiscCheck check = Orca::CheckDisc(disc, Orca::APP_GAME_ID);
  EXPECT_TRUE(check.ok) << check.code << " " << check.sentence;
  const Orca::DiscCheck other = Orca::CheckDisc(disc, "RMCE01");
  EXPECT_FALSE(other.ok);
  EXPECT_EQ(other.code, "disc_revision");
}

// A read patch lands only in its own partition and only when its byte is inside the read.
TEST(OrcaDisc, ReadPatchesLandInTheirRangeOnly)
{
  const DiscIO::Partition game(0xF800000);
  const std::vector<DiscIO::VolumeDisc::ReadPatch> patches = {{game, 99, 0, 1},
                                                              {game, 100, 0, 2},
                                                              {game, 115, 0, 3},
                                                              {game, 116, 0, 4},
                                                              {DiscIO::PARTITION_NONE, 101, 0, 5}};
  std::array<u8, 16> buffer{};
  DiscIO::VolumeWii::ApplyReadPatches(patches, 100, buffer.size(), buffer.data(), game);
  std::array<u8, 16> expected{};
  expected[0] = 2;
  expected[15] = 3;
  EXPECT_EQ(buffer, expected);

  buffer.fill(0);
  DiscIO::VolumeWii::ApplyReadPatches(patches, 100, buffer.size(), buffer.data(),
                                      DiscIO::PARTITION_NONE);
  expected.fill(0);
  expected[1] = 5;
  EXPECT_EQ(buffer, expected);

  buffer.fill(0);
  DiscIO::VolumeWii::ApplyReadPatches(patches, 117, buffer.size(), buffer.data(), game);
  expected.fill(0);
  EXPECT_EQ(buffer, expected);
}

namespace
{
std::vector<u8> ReadDisc(const DiscIO::VolumeDisc& volume, u64 offset, u64 length,
                         const DiscIO::Partition& partition)
{
  std::vector<u8> data(length);
  EXPECT_TRUE(volume.Read(offset, length, data.data(), partition));
  return data;
}
}  // namespace

// With ORCA_TEST_DISC (Rev 2): one byte that doesn't hold its original refuses the whole list, and
// no read changes.
TEST(OrcaDisc, ReadPatchesWithAWrongOriginalPatchNothing)
{
  const char* path = std::getenv("ORCA_TEST_DISC");
  if (!path)
    GTEST_SKIP() << "set ORCA_TEST_DISC to a Brawl (USA, Rev 2) image";
  const std::unique_ptr<DiscIO::VolumeDisc> disc = DiscIO::CreateDisc(path);
  ASSERT_TRUE(disc);
  const DiscIO::Partition game = disc->GetGamePartition();
  ASSERT_EQ(disc->GetRevision(game), 2);
  const std::vector<u8> header = ReadDisc(*disc, 0, 0x20, DiscIO::PARTITION_NONE);
  const std::vector<u8> boot = ReadDisc(*disc, 0, 0x20, game);

  // The first byte holds its original (2); the second doesn't (2, not 1).
  EXPECT_FALSE(disc->SetReadPatches({{DiscIO::PARTITION_NONE, 7, 2, 9}, {game, 7, 1, 9}}));
  EXPECT_EQ(ReadDisc(*disc, 0, 0x20, DiscIO::PARTITION_NONE), header);
  EXPECT_EQ(ReadDisc(*disc, 0, 0x20, game), boot);
  EXPECT_EQ(disc->GetRevision(), 2);
  EXPECT_EQ(disc->GetRevision(game), 2);
  EXPECT_TRUE(Orca::SessionDiscReady(*disc));
}

// With ORCA_TEST_DISC_REV1 pointing at a Brawl (USA) (Rev 1) image: Orca plays it, and it reads as
// Rev 2. With ORCA_TEST_DISC (Rev 2) too: the disc header and everything from the start of the game
// partition to the end of its file table (boot.bin, bi2.bin, the apploader, main.dol, fst.bin) read
// the same on both, and a Rev 2 disc is never patched.
TEST(OrcaDisc, BrawlRev1ReadsAsRev2)
{
  const char* rev1_path = std::getenv("ORCA_TEST_DISC_REV1");
  if (!rev1_path || !File::Exists(File::GetSysDirectory() + "Orca/RSBE01.ini"))
    GTEST_SKIP() << "set ORCA_TEST_DISC_REV1 to a Brawl (USA) (Rev 1) image";

  const std::unique_ptr<DiscIO::VolumeDisc> rev1 = DiscIO::CreateDisc(rev1_path);
  ASSERT_TRUE(rev1);
  const DiscIO::Partition game = rev1->GetGamePartition();
  ASSERT_EQ(rev1->GetRevision(game), 1);
  // Unaliased, a session refuses to insert it.
  EXPECT_FALSE(Orca::SessionDiscReady(*rev1));
  ASSERT_TRUE(Orca::AliasRevision(*rev1));
  EXPECT_TRUE(Orca::SessionDiscReady(*rev1));
  EXPECT_EQ(rev1->GetRevision(), 2);
  EXPECT_EQ(rev1->GetRevision(game), 2);
  EXPECT_EQ(rev1->GetGameID(game), "RSBE01");
  // The disc ID version main.dol builds at 0x8001BC9C: li r7,2.
  EXPECT_EQ(rev1->ReadSwapped<u32>(0x3DD00 + 0x11A1C, game), 0x38E00002u);
  // Once is enough: the bytes no longer hold Rev 1's.
  EXPECT_FALSE(Orca::AliasRevision(*rev1));

  const Orca::DiscCheck check = Orca::CheckDisc(rev1_path, Orca::APP_GAME_ID);
  EXPECT_TRUE(check.ok) << check.code << " " << check.sentence;

  const char* rev2_path = std::getenv("ORCA_TEST_DISC");
  if (!rev2_path)
    return;
  const std::unique_ptr<DiscIO::VolumeDisc> rev2 = DiscIO::CreateDisc(rev2_path);
  ASSERT_TRUE(rev2);
  ASSERT_EQ(rev2->GetRevision(rev2->GetGamePartition()), 2);
  EXPECT_FALSE(Orca::AliasRevision(*rev2));
  ASSERT_EQ(rev2->GetGamePartition(), game);

  EXPECT_EQ(ReadDisc(*rev1, 0, 0x100, DiscIO::PARTITION_NONE),
            ReadDisc(*rev2, 0, 0x100, DiscIO::PARTITION_NONE));
  const u64 fst_offset = *rev2->ReadSwappedAndShifted(0x424, game);
  const u64 fst_size = *rev2->ReadSwappedAndShifted(0x428, game);
  const u64 end = fst_offset + fst_size;
  ASSERT_GT(end, 0x3DD00u + 0x11A20u);
  EXPECT_TRUE(ReadDisc(*rev1, 0, end, game) == ReadDisc(*rev2, 0, end, game));
}

TEST(OrcaDisc, SessionSaveSeedingCanBeTurnedOff)
{
  EXPECT_TRUE(Orca::SeedSessionSave());
  Orca::SetSeedSessionSave(false);
  EXPECT_FALSE(Orca::SeedSessionSave());
  Orca::SetSeedSessionSave(true);
}

// No save, or an empty data folder (what ES leaves at boot), is hash 0; any file makes it non-zero.
TEST(OrcaDisc, EmptySessionSaveHashesToZero)
{
  const std::string root = File::CreateTempDir() + "/";
  const std::string old_root = File::GetUserPath(D_SESSION_WIIROOT_IDX);
  File::SetUserPath(D_SESSION_WIIROOT_IDX, root);
  constexpr u64 brawl = 0x0001000052534245ull;
  EXPECT_EQ(Orca::HashSessionSave(brawl), 0u);
  const std::string data = root + "title/00010000/52534245/data/";
  ASSERT_TRUE(File::CreateFullPath(data));
  EXPECT_EQ(Orca::HashSessionSave(brawl), 0u);
  ASSERT_TRUE(File::WriteStringToFile(data + "save.bin", "x"));
  EXPECT_NE(Orca::HashSessionSave(brawl), 0u);
  File::SetUserPath(D_SESSION_WIIROOT_IDX, old_root);
  File::DeleteDirRecursively(root);
}

// The default pad mapping names "SDL/0/*": any model's first gamepad, never another source or id.
TEST(OrcaInput, AnyNameMatchesSourceAndId)
{
  ciface::Core::DeviceQualifier any;
  any.FromString("SDL/0/*");
  EXPECT_TRUE(any == static_cast<const ciface::Core::Device*>(
                         std::make_unique<FakeDevice>("SDL", "Xbox Wireless Controller", 0).get()));
  EXPECT_TRUE(any == static_cast<const ciface::Core::Device*>(
                         std::make_unique<FakeDevice>("SDL", "DualSense", 0).get()));
  EXPECT_FALSE(any == static_cast<const ciface::Core::Device*>(
                          std::make_unique<FakeDevice>("SDL", "DualSense", 1).get()));
  EXPECT_FALSE(any == static_cast<const ciface::Core::Device*>(
                          std::make_unique<FakeDevice>("Quartz", "Keyboard & Mouse", 0).get()));

  ciface::Core::DeviceQualifier exact;
  exact.FromString("SDL/0/DualSense");
  const auto xbox = std::make_unique<FakeDevice>("SDL", "Xbox Wireless Controller", 0);
  EXPECT_FALSE(exact == static_cast<const ciface::Core::Device*>(xbox.get()));
}
