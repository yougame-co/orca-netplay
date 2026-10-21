// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include "Common/FileUtil.h"
#include "Core/Orca/DiscVerify.h"

namespace
{
using Orca::DiscVerify::Problem;
using Severity = DiscIO::VolumeVerifier::Severity;

std::optional<std::string> Take(std::vector<std::string> args, std::vector<std::string>* kept,
                                std::string* error)
{
  std::vector<char*> argv;
  for (std::string& arg : args)
    argv.push_back(arg.data());
  const std::optional<std::string> path = Orca::DiscVerify::TakeArgument(&argv, error);
  kept->clear();
  for (const char* arg : argv)
    kept->emplace_back(arg);
  return path;
}

// Runs the verifier on `path` and returns what it printed.
std::string RunOn(const std::string& path, int* code)
{
  std::FILE* const out = std::tmpfile();
  if (!out)
    return {};
  *code = Orca::DiscVerify::Run(path, out);
  std::rewind(out);
  std::string text;
  char buffer[256];
  while (const size_t n = std::fread(buffer, 1, sizeof(buffer), out))
    text.append(buffer, n);
  std::fclose(out);
  return text;
}
}  // namespace

TEST(OrcaDiscVerify, NoProblemsIsOk)
{
  EXPECT_EQ(Orca::DiscVerify::CountDamage({}), 0u);
  EXPECT_EQ(Orca::DiscVerify::ResultLines({}), std::vector<std::string>{"orca verify ok"});
  EXPECT_EQ(Orca::DiscVerify::ExitCode(0), 0);
}

// What a known-good scrubbed Brawl Rev 2 .wbfs reports: all Low, so it is ok.
TEST(OrcaDiscVerify, GoodScrubbedBrawlIsOk)
{
  std::vector<Problem> problems = {
      {Severity::Low, "The update partition is missing."},
      {Severity::Low, "The format that the disc image is saved in does not store the size of the "
                      "disc image."},
      {Severity::Low, "Errors were found in 8088 unused blocks in the DATA partition."},
  };
  for (int i = 0; i < 6; ++i)
  {
    problems.push_back(
        {Severity::Low,
         "Errors were found in 1 unused blocks in the HBxE (Masterpiece) partition."});
  }
  const std::size_t damage = Orca::DiscVerify::CountDamage(problems);
  EXPECT_EQ(damage, 0u);
  const std::vector<std::string> lines = Orca::DiscVerify::ResultLines(problems);
  ASSERT_EQ(lines.size(), 10u);
  EXPECT_EQ(lines[0], "orca verify problem low The update partition is missing.");
  EXPECT_EQ(lines[9], "orca verify ok");
  EXPECT_EQ(Orca::DiscVerify::ExitCode(damage), 0);
}

TEST(OrcaDiscVerify, MediumOrHighIsDamaged)
{
  const std::vector<Problem> problems = {
      {Severity::High, "Content 00000001 is corrupt."},
      {Severity::Medium, "Errors were found in 2 blocks in the DATA partition."},
      {Severity::Low, "The update partition is missing."},
  };
  const std::size_t damage = Orca::DiscVerify::CountDamage(problems);
  EXPECT_EQ(damage, 2u);
  EXPECT_EQ(Orca::DiscVerify::ResultLines(problems),
            (std::vector<std::string>{
                "orca verify problem high Content 00000001 is corrupt.",
                "orca verify damage Content 00000001 is corrupt.",
                "orca verify problem medium Errors were found in 2 blocks in the DATA partition.",
                "orca verify damage Errors were found in 2 blocks in the DATA partition.",
                "orca verify problem low The update partition is missing.",
                "orca verify damaged 2",
            }));
  EXPECT_EQ(Orca::DiscVerify::ExitCode(damage), 2);

  const std::vector<Problem> one = {{Severity::Medium, "Some of the data could not be read."}};
  EXPECT_EQ(Orca::DiscVerify::VerdictLine(Orca::DiscVerify::CountDamage(one)),
            "orca verify damaged 1");
}

// Dolphin's Medium problems that only concern Brawl's Masterpiece partitions, as
// DiscIO/VolumeVerifier.cpp words them: CheckPartitions, CheckPartition and Finish, with
// GetPartitionName's "<id> (Masterpiece)". Orca and Project+ never read those partitions.
const std::vector<Problem> MASTERPIECE_ONLY = {
    {Severity::Medium, "The Masterpiece partitions are missing."},
    {Severity::Medium, "The HBAE (Masterpiece) partition does not seem to contain valid data."},
    {Severity::Medium, "Errors were found in 2 blocks in the HBAE (Masterpiece) partition."},
};

TEST(OrcaDiscVerify, MasterpieceOnlyProblemsDontCount)
{
  for (const Problem& problem : MASTERPIECE_ONLY)
  {
    EXPECT_FALSE(Orca::DiscVerify::Counts(problem)) << problem.text;
    const std::vector<Problem> alone = {problem};
    EXPECT_EQ(Orca::DiscVerify::ResultLines(alone),
              (std::vector<std::string>{Orca::DiscVerify::ProblemLine(problem), "orca verify ok"}));
  }
  EXPECT_EQ(Orca::DiscVerify::CountDamage(MASTERPIECE_ONLY), 0u);
  const std::vector<std::string> lines = Orca::DiscVerify::ResultLines(MASTERPIECE_ONLY);
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines[0], "orca verify problem medium The Masterpiece partitions are missing.");
  EXPECT_EQ(lines[3], "orca verify ok");
}

TEST(OrcaDiscVerify, DataDamageCountsBesideMasterpieces)
{
  const std::vector<Problem> problems = {
      {Severity::Medium, "Errors were found in 3 blocks in the DATA partition."},
      MASTERPIECE_ONLY[2],
      {Severity::Low, "The update partition is missing."},
  };
  EXPECT_EQ(Orca::DiscVerify::CountDamage(problems), 1u);
  EXPECT_EQ(
      Orca::DiscVerify::ResultLines(problems),
      (std::vector<std::string>{
          "orca verify problem medium Errors were found in 3 blocks in the DATA partition.",
          "orca verify damage Errors were found in 3 blocks in the DATA partition.",
          "orca verify problem medium Errors were found in 2 blocks in the HBAE (Masterpiece) "
          "partition.",
          "orca verify problem low The update partition is missing.",
          "orca verify damaged 1",
      }));
}

TEST(OrcaDiscVerify, ProblemLines)
{
  EXPECT_EQ(Orca::DiscVerify::ProblemLine({Severity::Low, "The update partition is missing."}),
            "orca verify problem low The update partition is missing.");
  EXPECT_EQ(Orca::DiscVerify::ProblemLine({Severity::Medium, "x"}), "orca verify problem medium x");
  EXPECT_EQ(Orca::DiscVerify::ProblemLine({Severity::High, "x"}), "orca verify problem high x");
}

TEST(OrcaDiscVerify, ProblemTextGoesOnOneLine)
{
  const auto text = [](std::string t) {
    return Orca::DiscVerify::ProblemLine({Severity::Low, std::move(t)});
  };
  EXPECT_EQ(text("first\nsecond"), "orca verify problem low first second");
  EXPECT_EQ(text("first\r\n\r\nsecond\n"), "orca verify problem low first second");
  EXPECT_EQ(text("first \n second\tthird"), "orca verify problem low first second third");
  EXPECT_EQ(text("\nfirst"), "orca verify problem low first");
  EXPECT_EQ(text("first  second"), "orca verify problem low first second");
}

TEST(OrcaDiscVerify, ProgressAndUnreadableLines)
{
  EXPECT_EQ(Orca::DiscVerify::ProgressLine(0, 7962402816), "orca verify progress 0 7962402816");
  EXPECT_EQ(Orca::DiscVerify::ProgressLine(1u << 20, 2u << 20),
            "orca verify progress 1048576 2097152");
  EXPECT_EQ(Orca::DiscVerify::UnreadableLine("/discs/My  Brawl.wbfs"),
            "orca verify unreadable /discs/My  Brawl.wbfs");
  EXPECT_EQ(Orca::DiscVerify::UnreadableLine("/discs/a\nb.wbfs"),
            "orca verify unreadable /discs/a b.wbfs");
}

TEST(OrcaDiscVerify, ProgressAtMostOnceASecond)
{
  using std::chrono::milliseconds;
  const auto start = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  Orca::DiscVerify::ProgressPacer pacer;
  EXPECT_TRUE(pacer.Due(start));
  EXPECT_FALSE(pacer.Due(start + milliseconds(10)));
  EXPECT_FALSE(pacer.Due(start + milliseconds(999)));
  EXPECT_TRUE(pacer.Due(start + milliseconds(1000)));
  EXPECT_FALSE(pacer.Due(start + milliseconds(1500)));
  EXPECT_TRUE(pacer.Due(start + milliseconds(2600)));
}

TEST(OrcaDiscVerify, TakesTheFlagAndItsPath)
{
  std::vector<std::string> kept;
  std::string error;
  EXPECT_EQ(Take({"Orca", "--verify", "/d/brawl.wbfs", "-u", "/u"}, &kept, &error),
            "/d/brawl.wbfs");
  EXPECT_EQ(kept, (std::vector<std::string>{"Orca", "-u", "/u"}));

  EXPECT_EQ(Take({"Orca", "-u", "/u", "--verify", "/d/brawl.wbfs"}, &kept, &error),
            "/d/brawl.wbfs");
  EXPECT_EQ(kept, (std::vector<std::string>{"Orca", "-u", "/u"}));
}

// Without the flag nothing changes, so a normal boot is unaffected.
TEST(OrcaDiscVerify, NoFlagKeepsEverything)
{
  std::vector<std::string> kept;
  std::string error;
  const std::vector<std::string> args = {"Orca", "-u", "/u", "-e", "/d/brawl.wbfs", "-v", "Null"};
  EXPECT_EQ(Take(args, &kept, &error), "");
  EXPECT_EQ(kept, args);
  EXPECT_TRUE(error.empty());
}

TEST(OrcaDiscVerify, FlagWithoutPathIsRefused)
{
  std::vector<std::string> kept;
  std::string error;
  EXPECT_FALSE(Take({"Orca", "--verify"}, &kept, &error).has_value());
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_FALSE(Take({"Orca", "--verify", ""}, &kept, &error).has_value());
  EXPECT_FALSE(error.empty());
}

TEST(OrcaDiscVerify, NonDiscIsUnreadable)
{
  const std::string dir = File::CreateTempDir();
  const std::string path = dir + "/notes.txt";
  ASSERT_TRUE(File::WriteStringToFile(path, "not a disc\n"));
  int code = -1;
  EXPECT_EQ(RunOn(path, &code), "orca verify unreadable " + path + "\n");
  EXPECT_EQ(code, 1);
  File::DeleteDirRecursively(dir);

  EXPECT_EQ(RunOn("/nonexistent/brawl.wbfs", &code),
            "orca verify unreadable /nonexistent/brawl.wbfs\n");
  EXPECT_EQ(code, 1);
}

// A blank GameCube image opens but has no file system: the whole check runs and finds it damaged.
TEST(OrcaDiscVerify, BlankGameCubeImageIsDamaged)
{
  const std::string dir = File::CreateTempDir();
  const std::string path = dir + "/blank.iso";
  std::string image(4 << 20, '\0');
  const char gc_magic[] = {'\xC2', '\x33', '\x9F', '\x3D'};
  image.replace(0x1C, 4, gc_magic, 4);
  ASSERT_TRUE(File::WriteStringToFile(path, image));
  int code = -1;
  const std::string text = RunOn(path, &code);
  File::DeleteDirRecursively(dir);

  EXPECT_EQ(code, 2);
  EXPECT_TRUE(text.starts_with("orca verify progress 0 4194304\n")) << text;
  EXPECT_NE(text.find("\norca verify problem high "), std::string::npos) << text;
  const size_t last = text.rfind('\n', text.size() - 2);
  ASSERT_NE(last, std::string::npos) << text;
  EXPECT_TRUE(text.substr(last + 1).starts_with("orca verify damaged ")) << text;
}

// A verify that was killed leaves its NAND folder; the next one removes it, and only it: not a
// verify that is still running, not a name without a pid, not a file, not a symlink, not another
// program's temp folder.
TEST(OrcaDiscVerify, ClearsTheNandsOfKilledVerifies)
{
  const std::string dir = File::CreateTempDir();
  ASSERT_FALSE(dir.empty());
  for (const char* name : {"orca-verify-nand-111-a", "orca-verify-nand-222-b",
                           "orca-verify-nand-x-c", "orca-verify-nand-333", "DolphinWii.abc123"})
  {
    ASSERT_TRUE(File::CreateDir(dir + "/" + name)) << name;
  }
  ASSERT_TRUE(File::CreateDir(dir + "/orca-verify-nand-111-a/sys"));
  ASSERT_TRUE(File::WriteStringToFile(dir + "/orca-verify-nand-111-a/sys/x", "nand"));
  ASSERT_TRUE(File::WriteStringToFile(dir + "/orca-verify-nand-444-file", "not a folder"));
  std::filesystem::path target = std::filesystem::path(dir) / "DolphinWii.abc123";
  std::error_code ec;
  std::filesystem::create_directory_symlink(
      target, std::filesystem::path(dir) / "orca-verify-nand-555-link", ec);
  const bool linked = !ec;

  std::vector<u64> asked;
  const auto alive = [&asked](u64 pid) {
    asked.push_back(pid);
    return pid == 222;
  };
  EXPECT_EQ(Orca::DiscVerify::ClearStaleNands(dir, alive), 1u);

  EXPECT_FALSE(File::Exists(dir + "/orca-verify-nand-111-a"));
  EXPECT_TRUE(File::IsDirectory(dir + "/orca-verify-nand-222-b"));
  EXPECT_TRUE(File::IsDirectory(dir + "/orca-verify-nand-x-c"));
  EXPECT_TRUE(File::IsDirectory(dir + "/orca-verify-nand-333"));
  EXPECT_TRUE(File::Exists(dir + "/orca-verify-nand-444-file"));
  EXPECT_TRUE(File::IsDirectory(dir + "/DolphinWii.abc123"));
  if (linked)
    EXPECT_TRUE(
        std::filesystem::is_symlink(std::filesystem::path(dir) / "orca-verify-nand-555-link"));
  std::sort(asked.begin(), asked.end());
  EXPECT_EQ(asked, (std::vector<u64>{111, 222}));
  File::DeleteDirRecursively(dir);
}

TEST(OrcaDiscVerify, ThisProcessIsAliveAndANandIsItsOwn)
{
#ifdef _WIN32
  const u64 pid = GetCurrentProcessId();
#else
  const u64 pid = static_cast<u64>(getpid());
#endif
  EXPECT_TRUE(Orca::DiscVerify::ProcessAlive(pid));
  EXPECT_FALSE(Orca::DiscVerify::ProcessAlive(0));

  const std::string nand = Orca::DiscVerify::MakeNand();
  ASSERT_FALSE(nand.empty());
  EXPECT_TRUE(File::IsDirectory(nand));
  const std::string name = std::filesystem::path(nand).filename().string();
  EXPECT_TRUE(name.starts_with(fmt::format("orca-verify-nand-{}-", pid))) << name;
  // A second verify while this one runs leaves this one's folder.
  const std::string second = Orca::DiscVerify::MakeNand();
  EXPECT_TRUE(File::IsDirectory(nand));
  EXPECT_NE(second, nand);
  File::DeleteDirRecursively(second);
  File::DeleteDirRecursively(nand);
}
