// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Core/Orca/DiscReport.h"
#include "DiscIO/Volume.h"

namespace
{
namespace Report = Orca::DiscReport;

void Write32(std::string* data, size_t offset, u32 value)
{
  for (size_t i = 0; i < 4; ++i)
    (*data)[offset + i] = static_cast<char>(value >> (24 - i * 8));
}

std::string Digest(std::string_view bytes)
{
  std::string hash = Common::SHA1::DigestToString(Common::SHA1::CalculateDigest(bytes));
  for (char& c : hash)
  {
    if (c >= 'A' && c <= 'F')
      c += 'a' - 'A';
  }
  return hash;
}

// A complete 64 KiB disc with only invented bytes: one root file, a directory and an empty file.
std::string GameCubeImage()
{
  std::string image(0x10000, '\0');
  image.replace(0, 6, "GTEE01");
  image[7] = 2;
  Write32(&image, 0x1c, 0xc2339f3d);
  image.replace(0x20, 20, "Invented test title.");
  Write32(&image, 0x420, 0x3000);
  Write32(&image, 0x424, 0x4000);
  Write32(&image, 0x428, 0x80);
  Write32(&image, 0x42c, 0x80);
  Write32(&image, 0x2440 + 0x14, 0x10);
  Write32(&image, 0x2440 + 0x18, 0x10);
  image.replace(0x2460, 0x20, 0x20, 'a');
  Write32(&image, 0x3000, 0x100);
  Write32(&image, 0x3090, 4);
  image.replace(0x3100, 4, "code");

  // Root (0), hello.txt (1), folder (2), data.bin (3), empty (4).
  Write32(&image, 0x4000, 0x01000000);
  Write32(&image, 0x4008, 5);
  Write32(&image, 0x400c, 0);
  Write32(&image, 0x4010, 0x5000);
  Write32(&image, 0x4014, 5);
  Write32(&image, 0x4018, 0x0100000a);
  Write32(&image, 0x401c, 0);
  Write32(&image, 0x4020, 5);
  Write32(&image, 0x4024, 17);
  Write32(&image, 0x4028, 0x6000);
  Write32(&image, 0x402c, 3);
  Write32(&image, 0x4030, 26);
  Write32(&image, 0x4034, 0x7000);
  Write32(&image, 0x4038, 0);
  const char names[] = "hello.txt\0folder\0data.bin\0empty\0";
  image.replace(0x403c, sizeof(names), names, sizeof(names));
  image.replace(0x5000, 5, "hello");
  image.replace(0x6000, 3, "abc");
  return image;
}

std::string ReferenceJson(const Report::Reference& reference)
{
  picojson::array files;
  for (const auto& file : reference.files)
  {
    picojson::value value;
    EXPECT_TRUE(picojson::parse(value, Report::FileLine(file)).empty());
    files.push_back(value);
  }
  picojson::object object{{"name", picojson::value(reference.name)},
                          {"files", picojson::value(files)}};
  if (reference.partition_offset)
    object["partitionOffset"] = picojson::value(static_cast<double>(*reference.partition_offset));
  return picojson::value(object).serialize();
}

class DiscReportTest : public testing::Test
{
protected:
  void SetUp() override
  {
    m_directory = File::CreateTempDir();
    ASSERT_FALSE(m_directory.empty());
    m_path = m_directory + "/invented.iso";
    ASSERT_TRUE(File::WriteStringToFile(m_path, GameCubeImage()));
  }

  void TearDown() override { File::DeleteDirRecursively(m_directory); }

  std::string Run(const std::optional<Report::Reference>& reference, int* code)
  {
    std::optional<std::string> ref_path;
    if (reference)
    {
      ref_path = m_directory + "/reference.json";
      EXPECT_TRUE(File::WriteStringToFile(*ref_path, ReferenceJson(*reference)));
    }
    std::FILE* out = std::tmpfile();
    EXPECT_NE(out, nullptr);
    if (!out)
      return {};
    *code = Report::Run(m_path, ref_path, out);
    std::rewind(out);
    std::string result;
    char buffer[1024];
    while (const size_t length = std::fread(buffer, 1, sizeof(buffer), out))
      result.append(buffer, length);
    std::fclose(out);
    return result;
  }

  std::string m_directory;
  std::string m_path;
};

TEST_F(DiscReportTest, BootTitleMaskRespectsRangeLength)
{
  const auto volume = DiscIO::CreateVolume(m_path);
  ASSERT_TRUE(volume);
  const std::string image = GameCubeImage();
  for (const u64 length : {0u, 0x1fu, 0x20u, 0x5fu, 0x60u, 0x61u, 0x440u})
  {
    SCOPED_TRACE(length);
    std::string expected = image.substr(0, length);
    if (length >= 0x60)
      std::fill(expected.begin() + 0x20, expected.begin() + 0x60, 0);
    EXPECT_EQ(Report::Detail::HashRange(*volume, DiscIO::PARTITION_NONE, "sys/boot.bin", 0,
                                      length),
              Digest(expected));
    EXPECT_EQ(Report::Detail::HashRange(*volume, DiscIO::PARTITION_NONE, "other.bin", 0, length),
              Digest(image.substr(0, length)));
  }
}

TEST_F(DiscReportTest, EntriesIncludeEveryFileAndSystemFile)
{
  std::string error;
  const auto report = Report::Generate(m_path, &error);
  ASSERT_TRUE(report) << error;
  EXPECT_EQ(report->header.id, "GTEE01");
  EXPECT_EQ(report->header.revision, 2);
  EXPECT_FALSE(report->header.partition_offset);
  EXPECT_FALSE(report->header.tmd_content_sha1);
  EXPECT_FALSE(report->header.h3_sha1);
  const std::string image = GameCubeImage();
  std::string boot = image.substr(0, 0x440);
  std::fill(boot.begin() + 0x20, boot.begin() + 0x60, 0);
  const std::vector<Report::FileEntry> expected = {
      {"folder/data.bin", 3, "a9993e364706816aba3e25717850c26c9cd0d89d"},
      {"folder/empty", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
      {"hello.txt", 5, "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d"},
      {"sys/apploader.img", 0x40, Digest(image.substr(0x2440, 0x40))},
      {"sys/bi2.bin", 0x2000, Digest(image.substr(0x440, 0x2000))},
      {"sys/boot.bin", 0x440, Digest(boot)},
      {"sys/fst.bin", 0x80, Digest(image.substr(0x4000, 0x80))},
      {"sys/main.dol", 0x104, Digest(image.substr(0x3000, 0x104))},
  };
  ASSERT_EQ(report->files.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i)
  {
    EXPECT_EQ(report->files[i].path, expected[i].path);
    EXPECT_EQ(report->files[i].size, expected[i].size);
    EXPECT_EQ(report->files[i].sha1, expected[i].sha1) << expected[i].path;
  }
}

TEST_F(DiscReportTest, SameFilesPassAndOutputIsJsonLinesThenVerdict)
{
  std::string error;
  const auto report = Report::Generate(m_path, &error);
  ASSERT_TRUE(report) << error;
  const Report::Reference reference{"invented reference", report->files, std::nullopt};
  int code = -1;
  const std::string text = Run(reference, &code);
  EXPECT_EQ(code, 0);
  EXPECT_TRUE(text.starts_with(Report::HeaderLine(report->header) + '\n'));
  EXPECT_TRUE(text.ends_with("PASS: files match invented reference\n"));
  EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), report->files.size() + 2);
  for (const auto& file : report->files)
    EXPECT_NE(text.find(Report::FileLine(file) + '\n'), std::string::npos);
  EXPECT_TRUE(Run(std::nullopt, &code).ends_with("PASS: files match GTEE01\n"));
  EXPECT_EQ(code, 0);
}

TEST_F(DiscReportTest, ChangedByteDiffers)
{
  std::string error;
  const auto report = Report::Generate(m_path, &error);
  ASSERT_TRUE(report) << error;
  const Report::Reference reference{"original", report->files, std::nullopt};
  std::string image = GameCubeImage();
  image[0x6001] ^= 1;
  ASSERT_TRUE(File::WriteStringToFile(m_path, image));
  int code = -1;
  EXPECT_TRUE(Run(reference, &code).ends_with("DIFF: 1 files differ (folder/data.bin)\n"));
  EXPECT_EQ(code, 2);
}

TEST_F(DiscReportTest, MissingAndExtraFilesAreBothDifferences)
{
  std::string error;
  const auto report = Report::Generate(m_path, &error);
  ASSERT_TRUE(report) << error;
  Report::Reference reference{"original", report->files, std::nullopt};
  reference.files.erase(reference.files.begin());
  int code = -1;
  EXPECT_TRUE(Run(reference, &code).ends_with("DIFF: 1 files differ (folder/data.bin)\n"));
  EXPECT_EQ(code, 2);
  reference.files = report->files;
  reference.files.push_back({"missing.bin", 0, Digest("")});
  EXPECT_TRUE(Run(reference, &code).ends_with("DIFF: 1 files differ (missing.bin)\n"));
  EXPECT_EQ(code, 2);
}

TEST_F(DiscReportTest, MasksOnlyTheBootTitle)
{
  std::string error;
  const auto original = Report::Generate(m_path, &error);
  ASSERT_TRUE(original) << error;
  const Report::Reference reference{"original", original->files, std::nullopt};
  std::string image = GameCubeImage();
  std::fill(image.begin() + 0x20, image.begin() + 0x60, 't');
  ASSERT_TRUE(File::WriteStringToFile(m_path, image));
  auto changed = Report::Generate(m_path, &error);
  ASSERT_TRUE(changed) << error;
  EXPECT_TRUE(Report::Compare(*changed, reference).empty());
  image[0x60] ^= 1;
  ASSERT_TRUE(File::WriteStringToFile(m_path, image));
  changed = Report::Generate(m_path, &error);
  ASSERT_TRUE(changed) << error;
  EXPECT_EQ(Report::Compare(*changed, reference), std::vector<std::string>{"sys/boot.bin"});
}

TEST_F(DiscReportTest, UnreadableAndTruncatedImagesFail)
{
  int code = -1;
  ASSERT_TRUE(File::WriteStringToFile(m_path, "not a disc"));
  EXPECT_EQ(Run(std::nullopt, &code), "FAIL: cannot open a GameCube or Wii disc image\n");
  EXPECT_EQ(code, 1);
  std::string error;
  EXPECT_FALSE(Report::Generate(m_directory + "/missing.iso", &error));
  EXPECT_FALSE(error.empty());
  auto image = GameCubeImage();
  image.resize(0x6002);
  ASSERT_TRUE(File::WriteStringToFile(m_path, image));
  EXPECT_EQ(Run(std::nullopt, &code), "FAIL: cannot read folder/data.bin\n");
  EXPECT_EQ(code, 1);
}

TEST_F(DiscReportTest, InvalidFilesystemAndTruncatedSystemFileFail)
{
  int code = -1;
  auto image = GameCubeImage();
  Write32(&image, 0x428, 0);
  ASSERT_TRUE(File::WriteStringToFile(m_path, image));
  EXPECT_EQ(Run(std::nullopt, &code), "FAIL: cannot read game partition filesystem\n");
  EXPECT_EQ(code, 1);
  image = GameCubeImage();
  Write32(&image, 0x3090, 0x10000);
  ASSERT_TRUE(File::WriteStringToFile(m_path, image));
  EXPECT_EQ(Run(std::nullopt, &code), "FAIL: cannot read sys/main.dol\n");
  EXPECT_EQ(code, 1);
}

TEST_F(DiscReportTest, InvalidReferenceFailsBeforeReadingTheDisc)
{
  const Report::Reference invalid{"invalid", {{"a", 0, "not a sha1"}}, {}};
  int code = -1;
  EXPECT_TRUE(Run(invalid, &code).starts_with("FAIL: reference file needs"));
  EXPECT_EQ(code, 1);
}

// Wii metadata/offset comparison is tested independently of encrypted game fixtures.
TEST(OrcaDiscReport, WiiHeaderAndReferenceRoundTrip)
{
  const std::string sha1(40, 'a');
  Report::Report report{{"RTEE01", 2, sha1, std::string(40, 'b'), 0xf800000},
                        {{"sys/main.dol", 123, sha1}}};
  Report::Reference reference{"invented Wii", report.files, 0xf800000};
  std::string error;
  auto parsed = Report::ParseReference(ReferenceJson(reference), &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_TRUE(Report::Compare(report, *parsed).empty());
  picojson::value header;
  ASSERT_TRUE(picojson::parse(header, Report::HeaderLine(report.header)).empty());
  EXPECT_EQ(header.get("tmdContentSha1").get<std::string>(), sha1);
  EXPECT_EQ(header.get("h3Sha1").get<std::string>(), std::string(40, 'b'));
  EXPECT_EQ(header.get("partitionOffset").get<double>(), 0xf800000);
  report.header.partition_offset = 0x50000;
  EXPECT_EQ(Report::Compare(report, *parsed), std::vector<std::string>{"partitionOffset"});
  report.header.partition_offset.reset();
  EXPECT_EQ(Report::Compare(report, *parsed), std::vector<std::string>{"partitionOffset"});
  parsed->partition_offset.reset();
  EXPECT_TRUE(Report::Compare(report, *parsed).empty());
}

TEST(OrcaDiscReport, InvalidReferencesAreRefused)
{
  std::string error;
  const std::vector<std::string> invalid = {
      "{}",
      "[]",
      "{",
      R"({"name":"x","files":{}})",
      R"({"name":"x","files":[],"partitionOffset":-1})",
      R"({"name":"x","files":[],"partitionOffset":1.5})",
      R"({"name":"x","files":[],"partitionOffset":9007199254740992})",
      R"({"name":"x","files":[],"partitionOffset":1e9999})",
      R"({"name":"x","files":[{"path":"a","size":0,"sha1":"bad"}]})",
      R"({"name":"x","files":[{"path":"../a","size":0,"sha1":"bad"}]})",
      R"({"name":"x","files":[]} {})",
  };
  for (const auto& json : invalid)
  {
    EXPECT_FALSE(Report::ParseReference(json, &error)) << json;
    EXPECT_FALSE(error.empty());
  }
  Report::Reference duplicate{"duplicate", {{"a", 0, Digest("")}, {"a", 0, Digest("")}}, {}};
  EXPECT_FALSE(Report::ParseReference(ReferenceJson(duplicate), &error));
}

TEST(OrcaDiscReport, SizeChangesAndUppercaseReferenceHashes)
{
  Report::Report report{{}, {{"a", 0, Digest("")}}};
  Report::Reference reference{"sizes", report.files, {}};
  std::string error;
  for (char& c : reference.files[0].sha1)
  {
    if (c >= 'a' && c <= 'f')
      c -= 'a' - 'A';
  }
  auto parsed = Report::ParseReference(ReferenceJson(reference), &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_TRUE(Report::Compare(report, *parsed).empty());
  parsed->files[0].size = 1;
  EXPECT_EQ(Report::Compare(report, *parsed), std::vector<std::string>{"a"});
}

TEST(OrcaDiscReport, VerdictsStayOnOneLineAndShowOnlyFivePaths)
{
  Report::Reference reference{"test\r\nname", {}, {}};
  EXPECT_EQ(Report::VerdictLine(reference, {}), "PASS: files match test  name");
  EXPECT_EQ(Report::VerdictLine(reference, {"a\nb", "c", "d", "e", "f", "g"}),
            "DIFF: 6 files differ (a b, c, d, e, f)");
  EXPECT_EQ(Report::FailureLine("cannot\nread\rfile"), "FAIL: cannot read file");
  const Report::FileEntry file{"quotes\"and\nnewline", 0, Digest("")};
  picojson::value json;
  ASSERT_TRUE(picojson::parse(json, Report::FileLine(file)).empty());
  EXPECT_EQ(json.get("path").get<std::string>(), file.path);
}
}  // namespace
