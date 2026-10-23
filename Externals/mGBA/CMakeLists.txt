set(LIBMGBA_ONLY ON)
set(USE_LZMA ON)
add_subdirectory(mgba EXCLUDE_FROM_ALL)
dolphin_disable_warnings(mgba)

target_compile_definitions(mgba PUBLIC HAVE_CRC32 ENABLE_VFS ENABLE_DIRECTORIES)
target_link_libraries(mgba ZLIB::ZLIB)

# mGBA sets COMPILE_OPTIONS outright, which drops the global /utf-8; without it MSVC
# reads its UTF-8 sources in the system code page and fails on non-Latin locales.
if(MSVC)
  target_compile_options(mgba PRIVATE /utf-8)
endif()

if(NOT MSVC)
  target_compile_options(mgba PRIVATE -Wno-unused-parameter -Wno-unused-result -Wno-unused-variable)
endif()

if(ANDROID)
  target_compile_definitions(mgba PUBLIC ENABLE_VFS_FD)
  target_compile_definitions(mgba PRIVATE -Dfutimes=futimens)
endif()

add_library(mGBA::mgba ALIAS mgba)
