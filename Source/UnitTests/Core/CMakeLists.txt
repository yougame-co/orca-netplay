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
add_dolphin_test(OrcaStatusTest Orca/StatusTest.cpp Orca/BootMemoryTest.cpp)
if(NOT MSVC)
  set_property(SOURCE Orca/BootMemoryTest.cpp APPEND PROPERTY COMPILE_OPTIONS -fexceptions)
endif()
add_dolphin_test(OrcaRoomTest Orca/RoomTest.cpp Orca/OfflineReplayTest.cpp)
add_dolphin_test(OrcaDirectLinkTest Orca/DirectLinkTest.cpp)
# TURN stays test-only; the production libjuice target keeps NO_SERVER.
set(ORCA_TURN_SOURCE ${CMAKE_SOURCE_DIR}/Tools/orca/turn-fixture.c)
set(ORCA_JUICE_SOURCE ${CMAKE_SOURCE_DIR}/Externals/libjuice/libjuice)
target_sources(OrcaDirectLinkTest PRIVATE ${ORCA_TURN_SOURCE} ${ORCA_JUICE_SOURCE}/src/server.c)
target_include_directories(OrcaDirectLinkTest PRIVATE
  ${ORCA_JUICE_SOURCE}/src ${ORCA_JUICE_SOURCE}/include/juice)
target_compile_definitions(OrcaDirectLinkTest PRIVATE USE_NETTLE=0 RELEASE=1)
target_link_libraries(OrcaDirectLinkTest PRIVATE juice)
add_dolphin_test(OrcaJuicePollTest Orca/JuicePollTest.cpp Orca/JuicePollHarness.c)
set_target_properties(OrcaJuicePollTest PROPERTIES C_STANDARD 11)
target_include_directories(OrcaJuicePollTest PRIVATE
  ${CMAKE_BINARY_DIR}/Externals/libjuice
  ${ORCA_JUICE_SOURCE}/src ${ORCA_JUICE_SOURCE}/include/juice)
target_compile_definitions(OrcaJuicePollTest PRIVATE USE_NETTLE=0 RELEASE=1)
target_link_libraries(OrcaJuicePollTest PRIVATE juice)
add_executable(orca-turn-fixture EXCLUDE_FROM_ALL ${ORCA_TURN_SOURCE} ${ORCA_JUICE_SOURCE}/src/server.c)
target_include_directories(orca-turn-fixture PRIVATE
  ${ORCA_JUICE_SOURCE}/src ${ORCA_JUICE_SOURCE}/include/juice)
target_compile_definitions(orca-turn-fixture PRIVATE ORCA_TURN_FIXTURE_MAIN USE_NETTLE=0 RELEASE=1)
target_link_libraries(orca-turn-fixture PRIVATE juice)
set_target_properties(orca-turn-fixture PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Binaries/Tests)
add_dependencies(tests orca-turn-fixture)
# The stdio bridge and the sandbox's switches; Embed.cpp for --embed-adopted and the stdin reader.
add_dolphin_test(OrcaSandboxTest Orca/SandboxTest.cpp
  ${CMAKE_SOURCE_DIR}/Source/Core/DolphinNoGUI/Embed.cpp)
add_dolphin_test(OrcaDiscTest Orca/DiscTest.cpp)
add_dolphin_test(OrcaDiscVerifyTest Orca/DiscVerifyTest.cpp)
add_dolphin_test(OrcaPanicDiagTest Orca/PanicDiagTest.cpp)
add_dolphin_test(OrcaCrashNoteTest Orca/CrashNoteTest.cpp)
add_dolphin_test(OrcaGpuLostTest Orca/GpuLostTest.cpp)
add_dolphin_test(OrcaBootLogsTest Orca/BootLogsTest.cpp)
add_dolphin_test(OrcaBrandingTest Orca/BrandingTest.cpp)
add_dolphin_test(OrcaLaunchTest Orca/LaunchTest.cpp)
add_dolphin_test(OrcaLocalStoreTest Orca/LocalStoreTest.cpp)
add_dolphin_test(OrcaModTest Orca/ModTest.cpp)
# ModTest includes xxh3.h; core links xxhash PRIVATE, so the test needs it directly (MSVC has no
# system copy on its include path to hide that).
target_link_libraries(OrcaModTest PRIVATE xxhash::xxhash)
add_dolphin_test(OrcaBundleTest Orca/BundleTest.cpp)
# BundleTest signs bundles with Externals/ed25519 (core links it PRIVATE).
target_link_libraries(OrcaBundleTest PRIVATE ed25519 xxhash::xxhash)
# The keys the build pins (Source/Core/Core/CMakeLists.txt), so PinnedKey checks Bundle.cpp got them.
target_compile_definitions(OrcaBundleTest PRIVATE ${ORCA_BUNDLE_DEFINITIONS})
add_dolphin_test(OrcaCowTest Orca/CowTest.cpp)
add_dolphin_test(OrcaKeyframeTest Orca/KeyframeTest.cpp)
add_dolphin_test(OrcaFreshStartTest Orca/FreshStartTest.cpp)
add_dolphin_test(OrcaSnapshotTest Orca/SnapshotTest.cpp)
add_dolphin_test(OrcaGenericModuleTest Orca/GenericModuleTest.cpp)
add_dolphin_test(OrcaUXTest Orca/UXTest.cpp)
# The overlay draw tests set up ImGui frames of their own.
target_link_libraries(OrcaUXTest PRIVATE imgui)
add_dolphin_test(OrcaKitTest Orca/KitTest.cpp)
target_link_libraries(OrcaKitTest PRIVATE imgui)
add_dolphin_test(OrcaOnlineMenuTest Orca/Games/Brawl/OnlineMenuTest.cpp)
add_dolphin_test(OrcaGamePatchesTest Orca/GamePatchesTest.cpp)
add_dolphin_test(OrcaPauseTest Orca/PauseTest.cpp)
add_dolphin_test(OrcaJitWarmTest Orca/JitWarmTest.cpp)
# JitWarmTest includes xxh3.h too (see OrcaModTest).
target_link_libraries(OrcaJitWarmTest PRIVATE xxhash::xxhash)
add_dolphin_test(OrcaMusicTest Orca/MusicTest.cpp)
add_dolphin_test(OrcaActivityTest Orca/ActivityTest.cpp)
add_dolphin_test(OrcaResultsTest Orca/Games/Brawl/ResultsTest.cpp)
add_dolphin_test(OrcaRankedSetTest Orca/Games/Brawl/RankedSetTest.cpp)
add_dolphin_test(OrcaInputGateTest Orca/InputGateTest.cpp)
add_dolphin_test(OrcaOnlineRulesTest Orca/Games/Brawl/OnlineRulesTest.cpp)
add_dolphin_test(OrcaOnlineSeatsTest Orca/Games/Brawl/OnlineSeatsTest.cpp)
add_dolphin_test(OrcaQueueTest Orca/Games/Brawl/QueueTest.cpp)
add_dolphin_test(OrcaRankedTest Orca/RankedTest.cpp)
add_dolphin_test(OrcaRulesetTest Orca/RulesetTest.cpp)
add_dolphin_test(OrcaRulesetsTest Orca/Games/Brawl/RulesetsTest.cpp)
add_dolphin_test(OrcaBrawlStagesTest Orca/Games/Brawl/BrawlStagesTest.cpp)
add_dolphin_test(OrcaCharOrderTest Orca/Games/Brawl/CharOrderTest.cpp)
add_dolphin_test(OrcaCssTitleTest Orca/Games/Brawl/CssTitleTest.cpp)
add_dolphin_test(OrcaRelabelTest Orca/Games/Brawl/RelabelTest.cpp)
add_dolphin_test(OrcaSetEndTest Orca/SetEndTest.cpp)
# Its draw test sets up ImGui frames of its own.
target_link_libraries(OrcaSetEndTest PRIVATE imgui)
add_dolphin_test(OrcaNativeTextTest Orca/Games/Brawl/NativeTextTest.cpp)
add_dolphin_test(OrcaLayoutTest Orca/LayoutTest.cpp)
add_dolphin_test(OrcaPresentPacingTest Orca/PresentPacingTest.cpp)
add_dolphin_test(OrcaPerfTest Orca/PerfTest.cpp)
add_dolphin_test(OrcaGraphicsSettingsTest Orca/GraphicsSettingsTest.cpp)
add_dolphin_test(OrcaRumbleTest Orca/RumbleTest.cpp)
add_dolphin_test(OrcaDeviceBudgetTest Orca/DeviceBudgetTest.cpp)
add_dolphin_test(OrcaNandQuotaTest Orca/NandQuotaTest.cpp)

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
if(_M_X86_64 OR _M_ARM_64)
  target_sources(PowerPCTest PRIVATE PowerPC/JitHookCompileTest.cpp)
endif()
