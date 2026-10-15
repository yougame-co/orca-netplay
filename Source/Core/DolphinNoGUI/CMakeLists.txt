add_executable(dolphin-nogui
  Embed.cpp
  Embed.h
  Platform.cpp
  Platform.h
  PlatformHeadless.cpp
  MainNoGUI.cpp
)

if(X11_FOUND)
  target_sources(dolphin-nogui PRIVATE PlatformX11.cpp)
  target_link_libraries(dolphin-nogui PRIVATE PkgConfig::XRANDR PkgConfig::X11)
  # Orca: the embedded view's empty input shape (PlatformX11.cpp InitEmbedded).
  pkg_check_modules(XEXT xext IMPORTED_TARGET)
  if(XEXT_FOUND)
    target_compile_definitions(dolphin-nogui PRIVATE HAVE_XSHAPE=1)
    target_link_libraries(dolphin-nogui PRIVATE PkgConfig::XEXT)
  endif()
endif()

if(WIN32)
  target_sources(dolphin-nogui PRIVATE PlatformWin32.cpp)
endif()

if(APPLE)
  target_sources(dolphin-nogui PRIVATE PlatformMacos.mm)
  target_compile_options(dolphin-nogui PRIVATE -fobjc-arc)
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  target_sources(dolphin-nogui PRIVATE PlatformFBDev.cpp)
endif()

if(APPLE)
  option(ORCA_MACOS_BUNDLE "Build the NoGUI frontend as Orca.app, a self-contained bundle" OFF)
endif()
set(ORCA_VERSION "0.3.44" CACHE STRING "The Orca version (macOS Info.plist, Windows version resource)")

if (WIN32)
  target_sources(dolphin-nogui PRIVATE DolphinNoGUI.exe.manifest DolphinNoGUI.rc)
  # The program's version resource (DolphinNoGUI.rc), what Windows and antivirus show: ORCA_VERSION
  # as a string and as the four numbers VERSIONINFO wants. The release packages DolphinNoGUI.exe as
  # "YG Dolphin/yg-dolphin.exe" from 0.3.42 ("YG Baiji/yg-baiji.exe" in 0.3.40 and 0.3.41;
  # "Orca/Orca.exe" before).
  string(REGEX MATCHALL "[0-9]+" ORCA_VERSION_PARTS "${ORCA_VERSION}")
  list(APPEND ORCA_VERSION_PARTS 0 0 0 0)
  list(SUBLIST ORCA_VERSION_PARTS 0 4 ORCA_VERSION_PARTS)
  list(JOIN ORCA_VERSION_PARTS "," ORCA_VERSION_NUM)
  configure_file(orca-version.h.in "${CMAKE_CURRENT_BINARY_DIR}/orca-version.h" @ONLY)
  target_include_directories(dolphin-nogui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

  set_target_properties(dolphin-nogui PROPERTIES
    DEBUG_POSTFIX    D
    OUTPUT_NAME      DolphinNoGUI
  )
elseif(APPLE AND ORCA_MACOS_BUNDLE)
  # Orca.app: runs a session by default, with its own user dir and a disc picker (MainNoGUI.cpp).
  # Tools/orca-package-macos.sh ships it as "YG Dolphin.app"; the program inside stays
  # Contents/MacOS/Orca (CFBundleExecutable, Orca-Info.plist.in).
  target_compile_definitions(dolphin-nogui PRIVATE ORCA_MACOS_BUNDLE)
  set_target_properties(dolphin-nogui PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Orca-Info.plist.in"
    OUTPUT_NAME Orca
  )
else()
  set_target_properties(dolphin-nogui PROPERTIES OUTPUT_NAME dolphin-emu-nogui)
endif()

target_link_libraries(dolphin-nogui
PRIVATE
  core
  uicommon
  cpp-optparse
)

if(APPLE)
  target_link_libraries(dolphin-nogui
    PRIVATE
      ${APPKIT_LIBRARY}
      ${COREFOUNDATION_LIBRARY}
      ${IOK_LIBRARY}
  )
endif()

if(WIN32)
  # needed for adjusting window decorations with DwmSetWindowAttribute
  target_link_libraries(dolphin-nogui PRIVATE dwmapi.lib)
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(dolphin-nogui PRIVATE use_pch)
endif()

if(USE_DISCORD_PRESENCE)
  target_compile_definitions(dolphin-nogui PRIVATE -DUSE_DISCORD_PRESENCE)
endif()

if(APPLE AND ORCA_MACOS_BUNDLE)
  # Copy Sys into Contents/Resources, minus the images and themes only the Qt UI uses, plus the
  # license texts.
  file(GLOB_RECURSE orca_sys RELATIVE "${CMAKE_SOURCE_DIR}/Data" "${CMAKE_SOURCE_DIR}/Data/Sys/*")
  list(FILTER orca_sys EXCLUDE REGEX "^Sys/Themes/")
  list(FILTER orca_sys EXCLUDE REGEX
       "^Sys/Resources/(Flag_|Platform_|achievements_|dolphin_logo|isoproperties_|nobanner)")
  foreach(res ${orca_sys})
    target_sources(dolphin-nogui PRIVATE "${CMAKE_SOURCE_DIR}/Data/${res}")
    get_filename_component(resdir "${res}" DIRECTORY)
    set_source_files_properties("${CMAKE_SOURCE_DIR}/Data/${res}" PROPERTIES
      MACOSX_PACKAGE_LOCATION "Resources/${resdir}")
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Data/Sys")
  set(orca_resources "${CMAKE_SOURCE_DIR}/COPYING" "${CMAKE_SOURCE_DIR}/Data/Orca.icns")
  target_sources(dolphin-nogui PRIVATE ${orca_resources})
  set_source_files_properties(${orca_resources} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  file(GLOB orca_licenses "${CMAKE_SOURCE_DIR}/LICENSES/*")
  target_sources(dolphin-nogui PRIVATE ${orca_licenses})
  set_source_files_properties(${orca_licenses} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/LICENSES)
elseif(APPLE)
  # A plain macOS build still looks for Sys at <exe dir>/Contents/Resources/Sys, so link the source
  # tree's Data/Sys there for development builds.
  add_custom_command(TARGET dolphin-nogui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dolphin-nogui>/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:dolphin-nogui>/Contents/Resources/Sys"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/Data/Sys"
            "$<TARGET_FILE_DIR:dolphin-nogui>/Contents/Resources/Sys")
elseif(NOT ANDROID AND NOT ENABLE_QT)
  # On Windows and Linux, Sys must sit next to the executable, and normally DolphinQt's build puts
  # it there. Without Qt, link or copy it here. With Qt on, leave it to DolphinQt to avoid a race.
  add_custom_command(TARGET dolphin-nogui POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -Dtarget="${CMAKE_SOURCE_DIR}/Data/Sys"
      -Dlink="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Sys"
      -P="${CMAKE_SOURCE_DIR}/CMake/DolphinSymlinkOrCopy.cmake"
  )
endif()

set(CPACK_PACKAGE_EXECUTABLES ${CPACK_PACKAGE_EXECUTABLES} dolphin-nogui)
if (NOT WIN32 AND NOT ORCA_MACOS_BUNDLE)
  install(TARGETS dolphin-nogui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
