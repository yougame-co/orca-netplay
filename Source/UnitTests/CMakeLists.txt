enable_testing()
if(ORCA_BUILD_DISC_TOOLS)
  add_test(NAME OrcaDiscToolVersion
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:orca-disc-tool> -DVERSION=${ORCA_VERSION}
      -DSOURCE=${CMAKE_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/OrcaDiscToolVersion.cmake)
endif()
add_custom_target(unittests)
add_custom_command(TARGET unittests POST_BUILD COMMAND ${CMAKE_CTEST_COMMAND} "--output-on-failure")

string(APPEND CMAKE_RUNTIME_OUTPUT_DIRECTORY "/Tests")

add_executable(tests EXCLUDE_FROM_ALL UnitTestsMain.cpp StubHost.cpp)
set_target_properties(tests PROPERTIES FOLDER Tests)
target_link_libraries(tests PRIVATE fmt::fmt gtest::gtest core uicommon)
add_test(NAME tests COMMAND tests)
add_custom_command(TARGET tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Sys"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Data/Sys" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Sys"
)
add_dependencies(unittests tests)

macro(add_dolphin_test target)
  add_library(${target} OBJECT ${ARGN})
  target_link_libraries(${target} PUBLIC fmt::fmt gtest::gtest PRIVATE core uicommon)
  target_link_libraries(tests PRIVATE ${target})
endmacro()

add_subdirectory(Common)
add_subdirectory(Core)
add_subdirectory(VideoCommon)
add_dolphin_test(LocalInputTest InputCommon/LocalInputTest.cpp)

add_dolphin_test(ShortcutsTest InputCommon/ShortcutsTest.cpp)
add_dolphin_test(ShortcutMappingTest InputCommon/ShortcutMappingTest.cpp)
