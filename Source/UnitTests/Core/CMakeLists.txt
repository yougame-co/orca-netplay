add_dolphin_test(MMIOTest MMIOTest.cpp)
add_dolphin_test(PageFaultTest PageFaultTest.cpp)
add_dolphin_test(CoreTimingTest CoreTimingTest.cpp)
add_dolphin_test(PatchAllowlistTest PatchAllowlistTest.cpp)

add_dolphin_test(DSPAcceleratorTest DSP/DSPAcceleratorTest.cpp)
add_dolphin_test(DSPAssemblyTest
  DSP/DSPAssemblyTest.cpp
  DSP/DSPTestBinary.cpp
  DSP/DSPTestText.cpp
  DSP/HermesBinary.cpp
  DSP/HermesText.cpp
)

add_dolphin_test(ESFormatsTest IOS/ES/FormatsTest.cpp)

add_dolphin_test(FileSystemTest IOS/FS/FileSystemTest.cpp)

add_dolphin_test(OrcaSessionTest Orca/SessionTest.cpp)
add_dolphin_test(OrcaRoomTest Orca/RoomTest.cpp)
add_dolphin_test(OrcaDirectLinkTest Orca/DirectLinkTest.cpp)
# The stdio bridge and the sandbox's switches; Embed.cpp for --embed-adopted and the stdin reader.
add_dolphin_test(OrcaSandboxTest Orca/SandboxTest.cpp
  ${CMAKE_SOURCE_DIR}/Source/Core/DolphinNoGUI/Embed.cpp)
add_dolphin_test(OrcaDiscTest Orca/DiscTest.cpp)
add_dolphin_test(OrcaDiscVerifyTest Orca/DiscVerifyTest.cpp)
add_dolphin_test(OrcaPanicDiagTest Orca/PanicDiagTest.cpp)
add_dolphin_test(OrcaCrashNoteTest Orca/CrashNoteTest.cpp)
add_dolphin_test(OrcaBootLogsTest Orca/BootLogsTest.cpp)
add_dolphin_test(OrcaLaunchTest Orca/LaunchTest.cpp)
add_dolphin_test(OrcaCowTest Orca/CowTest.cpp)
add_dolphin_test(OrcaKeyframeTest Orca/KeyframeTest.cpp)
add_dolphin_test(OrcaFreshStartTest Orca/FreshStartTest.cpp)
add_dolphin_test(OrcaUXTest Orca/UXTest.cpp)
# The overlay draw tests set up ImGui frames of their own.
target_link_libraries(OrcaUXTest PRIVATE imgui)
add_dolphin_test(OrcaKitTest Orca/KitTest.cpp)
target_link_libraries(OrcaKitTest PRIVATE imgui)
add_dolphin_test(OrcaOnlineMenuTest Orca/OnlineMenuTest.cpp)
add_dolphin_test(OrcaGamePatchesTest Orca/GamePatchesTest.cpp)
add_dolphin_test(OrcaPauseTest Orca/PauseTest.cpp)
add_dolphin_test(OrcaJitWarmTest Orca/JitWarmTest.cpp)
add_dolphin_test(OrcaMusicTest Orca/MusicTest.cpp)
add_dolphin_test(OrcaActivityTest Orca/ActivityTest.cpp)
add_dolphin_test(OrcaResultsTest Orca/ResultsTest.cpp)
add_dolphin_test(OrcaRankedSetTest Orca/RankedSetTest.cpp)
add_dolphin_test(OrcaInputGateTest Orca/InputGateTest.cpp)
add_dolphin_test(OrcaOnlineRulesTest Orca/OnlineRulesTest.cpp)
add_dolphin_test(OrcaOnlineSeatsTest Orca/OnlineSeatsTest.cpp)
add_dolphin_test(OrcaQueueTest Orca/QueueTest.cpp)
add_dolphin_test(OrcaRankedTest Orca/RankedTest.cpp)
add_dolphin_test(OrcaBrawlStagesTest Orca/BrawlStagesTest.cpp)
add_dolphin_test(OrcaCharOrderTest Orca/CharOrderTest.cpp)
add_dolphin_test(OrcaCssTitleTest Orca/CssTitleTest.cpp)
add_dolphin_test(OrcaRelabelTest Orca/RelabelTest.cpp)
add_dolphin_test(OrcaSetEndTest Orca/SetEndTest.cpp)
# Its draw test sets up ImGui frames of its own.
target_link_libraries(OrcaSetEndTest PRIVATE imgui)
add_dolphin_test(OrcaNativeTextTest Orca/NativeTextTest.cpp)
add_dolphin_test(OrcaLayoutTest Orca/LayoutTest.cpp)
add_dolphin_test(OrcaPresentPacingTest Orca/PresentPacingTest.cpp)
add_dolphin_test(OrcaPerfTest Orca/PerfTest.cpp)
add_dolphin_test(OrcaGraphicsSettingsTest Orca/GraphicsSettingsTest.cpp)

add_dolphin_test(SkylandersTest IOS/USB/SkylandersTest.cpp)

if(_M_X86_64)
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
    PowerPC/Jit64Common/ConvertDoubleToSingle.cpp
    PowerPC/Jit64Common/Fres.cpp
    PowerPC/Jit64Common/Frsqrte.cpp
    PowerPC/Jit64Common/Ps1.cpp
    PowerPC/JitFloatSemantics.cpp
  )
elseif(_M_ARM_64)
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
    PowerPC/JitArm64/ConvertSingleDouble.cpp
    PowerPC/JitArm64/FPRF.cpp
    PowerPC/JitArm64/Fres.cpp
    PowerPC/JitArm64/Frsqrte.cpp
    PowerPC/JitArm64/MovI2R.cpp
    PowerPC/JitArm64/VectorModImm.cpp
    PowerPC/JitFloatSemantics.cpp
  )
else()
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
  )
endif()

target_sources(PowerPCTest PRIVATE
  PowerPC/TestValues.h
  StubJit.h
)
