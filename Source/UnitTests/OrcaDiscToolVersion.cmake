# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
file(READ "${SOURCE}/Source/Core/Core/Orca/DiscLayout.h" format_header)
string(REGEX MATCH "FORMAT_VERSION = ([0-9]+)" format_line "${format_header}")
set(format_version "${CMAKE_MATCH_1}")
execute_process(COMMAND "${TOOL}" --version
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT errors STREQUAL "" OR
   NOT output STREQUAL "orca-disc-tool ${VERSION} layout ${format_version}\n")
  message(FATAL_ERROR "Unexpected --version: exit ${result}, stdout '${output}', stderr '${errors}'")
endif()
