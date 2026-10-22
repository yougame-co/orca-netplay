// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/BroadcastFrame.h"
#include <chrono>
#include <cmath>
#include "Common/BroadcastRing.h"
#include "Core/BroadcastSink.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"

BroadcastFrame::BroadcastFrame()
{
  m_worker = std::thread([this] {
    while (true)
    {
      if (m_stop.load(std::memory_order_acquire))
        return;
      if (m_job.load(std::memory_order_acquire) == 1)
      {
        Broadcast::Video(m_pointer, m_stride, m_work->timestamp);
        m_job.store(2, std::memory_order_release);
      }
      else
      {
        // Producer does only atomic stores. Event::Set takes a mutex, which could let a
        // descheduled worker stall rendering; bounded polling keeps that cost off capture.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  });
}
BroadcastFrame::~BroadcastFrame()
{
  m_stop.store(true, std::memory_order_release);
  m_worker.join();  // Shutdown only; Capture never joins or waits.
  if (m_work)
    m_work->texture->Unmap();
}
void BroadcastFrame::Capture(const AbstractTexture* texture, const MathUtil::Rectangle<int>& rect,
                             float aspect_ratio)
{
  if (m_job.load(std::memory_order_acquire) == 2)
  {
    m_work->texture->Unmap();
    m_work->timestamp = 0;
    m_work = nullptr;
    m_job.store(0, std::memory_order_release);
  }
  if (!Broadcast::Active())
    return;
  if (!g_gfx->SupportsUtilityDrawing())
  {
    Broadcast::VideoDrop();
    return;
  }
  const u64 now = Broadcast::NowMicros();
  Readback* newest = nullptr;
  for (auto& readback : m_readbacks)
  {
    if (&readback == m_work || !readback.timestamp)
      continue;
    // Age is independent of readiness: a fence that never signals must not occupy every slot.
    if (now - readback.timestamp > 100000)
    {
      Broadcast::VideoDrop();
      readback.timestamp = 0;
      // Backends defer GPU resource destruction. Do not reuse a still-pending readback.
      readback.texture.reset();
      continue;
    }
    if (readback.texture->TryFlush())
    {
      if (!newest || readback.timestamp > newest->timestamp)
      {
        if (newest)
        {
          Broadcast::VideoDrop();
          newest->timestamp = 0;
        }
        newest = &readback;
      }
      else
      {
        Broadcast::VideoDrop();
        readback.timestamp = 0;
      }
    }
  }
  if (newest)
  {
    if (m_job.load(std::memory_order_acquire) == 0 && newest->texture->Map())
    {
      // Map/Unmap stay on the GPU thread. No pixel copy here; the worker owns this slot
      // until its release store, so neither reuse nor age expiry can invalidate the pointer.
      m_work = newest;
      m_pointer = reinterpret_cast<const u8*>(newest->texture->GetMappedPointer());
      m_stride = newest->texture->GetMappedStride();
      m_job.store(1, std::memory_order_release);
    }
    else
    {
      Broadcast::VideoDrop();
      newest->texture->Unmap();
      newest->timestamp = 0;
    }
  }

  if (now < m_next)
    return;
  // Advance the cadence, rather than rounding each deadline up to the next emulated frame.
  // The latter can turn 60fps with a little jitter into only 20 captured frames per second.
  m_next = m_next ? m_next + 1000000 / Broadcast::FPS : now + 1000000 / Broadcast::FPS;
  if (m_next <= now)
    m_next = now + 1000000 / Broadcast::FPS;
  const TextureConfig config(Broadcast::WIDTH, Broadcast::HEIGHT, 1, 1, 1,
                             AbstractTextureFormat::RGBA8, AbstractTextureFlag_RenderTarget,
                             AbstractTextureType::Texture_2DArray);
  if (!m_target)
  {
    m_target = g_gfx->CreateTexture(config, "Broadcast render texture");
    if (m_target)
      m_framebuffer = g_gfx->CreateFramebuffer(m_target.get(), nullptr);
  }
  if (!m_framebuffer)
  {
    Broadcast::VideoDrop();
    return;
  }
  for (auto& readback : m_readbacks)
  {
    if (readback.timestamp)
      continue;
    if (!readback.texture)
      readback.texture = g_gfx->CreateStagingTexture(StagingTextureType::Readback, config);
    if (!readback.texture)
      break;
    // Preserve the VI/display aspect, including 4:3 games, without depending on window size.
    if (!std::isfinite(aspect_ratio) || aspect_ratio <= 0)
      aspect_ratio = static_cast<float>(Broadcast::WIDTH) / Broadcast::HEIGHT;
    int width = Broadcast::WIDTH, height = Broadcast::HEIGHT;
    if (aspect_ratio < static_cast<float>(width) / height)
      width = std::clamp(static_cast<int>(std::lround(height * aspect_ratio)), 1, width);
    else
      height = std::clamp(static_cast<int>(std::lround(width / aspect_ratio)), 1, height);
    const int left = (Broadcast::WIDTH - width) / 2, top = (Broadcast::HEIGHT - height) / 2;
    const MathUtil::Rectangle<int> target_rect(left, top, left + width, top + height);
    if (width != Broadcast::WIDTH || height != Broadcast::HEIGHT)
    {
      g_gfx->BeginUtilityDrawing();
      g_gfx->SetAndClearFramebuffer(m_framebuffer.get(), {{0.0f, 0.0f, 0.0f, 1.0f}});
      g_gfx->EndUtilityDrawing();
    }
    g_gfx->ScaleTexture(m_framebuffer.get(), target_rect, texture, rect);
    readback.texture->CopyFromTexture(m_target.get(), m_target->GetRect(), 0, 0,
                                      readback.texture->GetRect());
    readback.timestamp = now;
    return;
  }
  Broadcast::VideoDrop();
}
