// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <dxgi1_5.h>
#include <wrl/client.h>

#include "Common/CommonTypes.h"
#include "Common/WindowSystemInfo.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/TextureConfig.h"

namespace D3DCommon
{
class SwapChain
{
public:
  SwapChain(const WindowSystemInfo& wsi, IDXGIFactory* dxgi_factory, IUnknown* d3d_device);
  virtual ~SwapChain();

  // Sufficient buffers for triple buffering.
  static const u32 SWAP_CHAIN_BUFFER_COUNT = 3;

  // Returns true if the stereo mode is quad-buffering.
  static bool WantsStereo();

  static bool WantsHDR();

  IDXGISwapChain* GetDXGISwapChain() const { return m_swap_chain.Get(); }
  AbstractTextureFormat GetFormat() const
  {
    return m_hdr ? m_texture_format_hdr : m_texture_format;
  }
  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }

  // Mode switches.
  bool GetFullscreen() const;
  void SetFullscreen(bool request);

  // Checks for loss of exclusive fullscreen.
  bool CheckForFullscreenChange();

  // Presents the swap chain to the screen.
  virtual bool Present();

  bool ChangeSurface(void* native_handle);
  bool ResizeSwapChain();

  // Orca: with no buffers after a refused resize, whether this frame asks for the resize again: the
  // first frame after it, then every RESIZE_RETRY_FRAMES (a window resize asks at once anyway).
  bool ResizeRetryDue() { return m_frames_without_buffers++ % RESIZE_RETRY_FRAMES == 0; }
  static constexpr u32 RESIZE_RETRY_FRAMES = 30;

  void SetStereo(bool stereo);
  void SetHDR(bool hdr);

protected:
  u32 GetSwapChainFlags() const;
  bool CreateSwapChain(bool stereo = false, bool hdr = false);
  void DestroySwapChain();

  virtual bool CreateSwapChainBuffers() = 0;
  virtual void DestroySwapChainBuffers() = 0;

  WindowSystemInfo m_wsi;
  Microsoft::WRL::ComPtr<IDXGIFactory> m_dxgi_factory;
  Microsoft::WRL::ComPtr<IDXGISwapChain> m_swap_chain;
  Microsoft::WRL::ComPtr<IUnknown> m_d3d_device;
  const AbstractTextureFormat m_texture_format = AbstractTextureFormat::RGB10_A2;
  const AbstractTextureFormat m_texture_format_hdr = AbstractTextureFormat::RGBA16F;

  u32 m_width = 1;
  u32 m_height = 1;

  bool m_stereo = false;
  bool m_hdr = false;
  bool m_allow_tearing_supported = false;
  bool m_has_fullscreen = false;
  bool m_fullscreen_request = false;

  // Orca: the last resize's outcome, so the game's log notes each change once (ResizeSwapChain),
  // frames since buffers were last there (ResizeRetryDue), and what the backend's
  // CreateSwapChainBuffers last failed with (CreateSwapChain says it).
  bool m_buffers_ok = true;
  HRESULT m_resize_error = S_OK;
  u32 m_frames_without_buffers = 0;
  HRESULT m_buffers_error = S_OK;
};

}  // namespace D3DCommon
