add_dolphin_test(VertexLoaderTest VertexLoaderTest.cpp)
add_dolphin_test(OrcaGpuBoundsTest GpuBoundsTest.cpp)
add_dolphin_test(OrcaRenderStateTest RenderStateTest.cpp)

# Orca: the D3D FXC shader cache. D3DCommon builds only on Windows, so elsewhere the test compiles
# the cache's source itself (only the compiler DLL's digest needs Windows).
if(WIN32)
  add_dolphin_test(FxcCacheTest FxcCacheTest.cpp)
else()
  add_dolphin_test(FxcCacheTest FxcCacheTest.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/VideoBackends/D3DCommon/FxcCache.cpp)
endif()
