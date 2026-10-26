add_library(orca-disc-tools STATIC
  ../Core/Orca/DiscLayout.cpp
  ../Core/Orca/RebuildStandard.cpp
  ../Core/Orca/StandardBlob.cpp
  OutputFile.cpp
  OutputFile.h
)
target_link_libraries(orca-disc-tools PUBLIC core PRIVATE fmt::fmt)

add_executable(orca-disc-tool Main.cpp ../DolphinTool/ToolHeadlessPlatform.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # GNU ld scans each archive once; core (via orca-disc-tools) calls into uicommon, which the tool's own code never references first.
  target_link_libraries(orca-disc-tool PRIVATE orca-disc-tools "$<LINK_LIBRARY:WHOLE_ARCHIVE,uicommon>")
else()
  target_link_libraries(orca-disc-tool PRIVATE orca-disc-tools uicommon)
endif()
