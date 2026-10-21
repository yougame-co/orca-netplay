// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "Common/ScopeGuard.h"
#include "Core/Orca/BootMemory.h"
#include "Core/Orca/Status.h"

namespace
{
void EnableOrcaSession()
{
#ifdef _WIN32
  _putenv_s("ORCA_SESSION", "1");
#else
  setenv("ORCA_SESSION", "1", 1);
#endif
}
}  // namespace

TEST(OrcaBootMemoryDeathTest, ThrowingBootStepReportsLowMemoryAndEnds)
{
  // Isolate the process-wide first-frame/error state, just like a fresh NoGUI boot.
  ASSERT_EXIT(
      {
        EnableOrcaSession();
        std::FILE* const out = std::tmpfile();
        if (!out)
          std::_Exit(2);
        Orca::Status::SetOutput(out);
        bool cleaned = false;
        const bool ran = Orca::BootMemory::Run([&] {
          Common::ScopeGuard cleanup([&] { cleaned = true; });
          throw std::bad_alloc{};
        });
        // A later failure must not replace or repeat the boot error.
        Orca::Status::Error("boot", "Could not start");
        const int code = Orca::Status::Finish();
        std::rewind(out);
        std::string lines;
        char buffer[256];
        while (std::fgets(buffer, sizeof(buffer), out))
          lines += buffer;
        const bool ok = !ran && cleaned && code == 1 &&
                        lines == "orca error low_memory Your PC ran out of memory while starting "
                                 "the game. Close other apps first.\n"
                                 "orca state ended\n";
        std::fclose(out);
        std::_Exit(ok ? 0 : 3);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(OrcaBootMemoryDeathTest, AllocationFailureAfterFirstFramePropagates)
{
  ASSERT_EXIT(
      {
        Orca::BootMemory::FirstFrame();
        bool propagated = false;
        try
        {
          Orca::BootMemory::Run([] { throw std::bad_alloc{}; });
        }
        catch (const std::bad_alloc&)
        {
          propagated = true;
        }
        std::_Exit(propagated && Orca::Status::ExitCode() == 0 ? 0 : 2);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(OrcaBootMemory, SuccessAndOtherExceptionsKeepTheirBehavior)
{
  bool called = false;
  EXPECT_TRUE(Orca::BootMemory::Run([&] { called = true; }));
  EXPECT_TRUE(called);
  EXPECT_THROW(Orca::BootMemory::Run([] { throw std::runtime_error("boot"); }), std::runtime_error);
}
