add_library(videod3dcommon
  D3DCommon.cpp
  D3DCommon.h
  DeviceLost.cpp
  DeviceLost.h
  FxcCache.cpp
  FxcCache.h
  Shader.cpp
  Shader.h
  SwapChain.cpp
  SwapChain.h
)

target_link_libraries(videod3dcommon
PUBLIC
  common
  videocommon
  spirv_cross
)

if(MSVC)
  # Add precompiled header
  target_link_libraries(videod3dcommon PRIVATE use_pch)
endif()
