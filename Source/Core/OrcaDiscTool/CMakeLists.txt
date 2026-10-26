add_library(orca-disc-tools STATIC
  ../Core/Orca/DiscLayout.cpp
  ../Core/Orca/RebuildStandard.cpp
  ../Core/Orca/StandardBlob.cpp
  OutputFile.cpp
  OutputFile.h
)
target_link_libraries(orca-disc-tools PUBLIC core PRIVATE fmt::fmt)

add_executable(orca-disc-tool Main.cpp ../DolphinTool/ToolHeadlessPlatform.cpp)
target_link_libraries(orca-disc-tool PRIVATE orca-disc-tools uicommon)
