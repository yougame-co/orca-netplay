#pragma once

#include <cstddef>

#include "Common/Assert.h"

// Release builds log a failed ImGui assert and carry on instead of trapping. ImGui only draws
// overlays, so a bad assert there must not end a player's game. Debug builds still stop.
#if defined(_DEBUG) || defined(DEBUGFAST)
#define IM_ASSERT(_EXPR) ASSERT(_EXPR)
#else
#define IM_ASSERT(_EXPR)                                                                           \
  do                                                                                               \
  {                                                                                                \
    if (!(_EXPR)) [[unlikely]]                                                                     \
      ::ImGuiAssertFailed(#_EXPR, __FILE__, __LINE__);                                             \
  } while (0)
#endif

// Counts a failed IM_ASSERT and logs the first failure at each file and line. Any thread.
void ImGuiAssertFailed(const char* expression, const char* file, int line);
// Number of failed ImGui asserts so far in a release build (for tests).
std::size_t ImGuiAssertFailures();

#define IMGUI_DISABLE_DEMO_WINDOWS
