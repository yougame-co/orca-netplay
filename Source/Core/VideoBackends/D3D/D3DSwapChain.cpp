// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DSwapChain.h"

#include "Common/Logging/Log.h"

#include "VideoBackends/D3D/DXTexture.h"

namespace DX11
{
SwapChain::SwapChain(const WindowSystemInfo& wsi, IDXGIFactory* dxgi_factory,
                     ID3D11Device* d3d_device)
    : D3DCommon::SwapChain(wsi, dxgi_factory, d3d_device)
{
}

SwapChain::~SwapChain() = default;

std::unique_ptr<SwapChain> SwapChain::Create(const WindowSystemInfo& wsi)
{
  std::unique_ptr<SwapChain> swap_chain =
      std::make_unique<SwapChain>(wsi, D3D::dxgi_factory.Get(), D3D::device.Get());
  if (!swap_chain->CreateSwapChain(WantsStereo(), WantsHDR()))
    return nullptr;

  return swap_chain;
}

bool SwapChain::CreateSwapChainBuffers()
{
  ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&texture));
  // Orca: no ASSERT (it ends a release build). After a failed resize the swap chain has no buffers;
  // the callers handle false (D3DCommon::SwapChain::ResizeSwapChain, CreateSwapChain).
  if (FAILED(hr))
  {
    WARN_LOG_FMT(VIDEO, "Failed to get swap chain buffer: {}", DX11HRWrap(hr));
    m_buffers_error = hr;
    return false;
  }

  m_texture = DXTexture::CreateAdopted(std::move(texture));
  if (!m_texture)
    return false;

  m_framebuffer = DXFramebuffer::Create(m_texture.get(), nullptr, {});
  if (!m_framebuffer)
    return false;

  return true;
}

void SwapChain::DestroySwapChainBuffers()
{
  m_framebuffer.reset();
  m_texture.reset();
}
}  // namespace DX11
