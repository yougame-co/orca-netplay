// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Metal/MTLGfx.h"

#include "Common/Logging/Log.h"

#include "VideoBackends/Metal/MTLBoundingBox.h"
#include "VideoBackends/Metal/MTLObjectCache.h"
#include "VideoBackends/Metal/MTLPipeline.h"
#include "VideoBackends/Metal/MTLStateTracker.h"
#include "VideoBackends/Metal/MTLTexture.h"
#include "VideoBackends/Metal/MTLUtil.h"
#include "VideoBackends/Metal/MTLVertexFormat.h"
#include "VideoBackends/Metal/MTLVertexManager.h"

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoBackendBase.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <mutex>

Metal::Gfx::Gfx(MRCOwned<CAMetalLayer*> layer) : m_layer(std::move(layer))
{
  UpdateActiveConfig();
  [m_layer setDisplaySyncEnabled:g_ActiveConfig.bVSyncActive];

  SetupSurface();
  g_state_tracker->FlushEncoders();
}

Metal::Gfx::~Gfx() = default;

bool Metal::Gfx::IsHeadless() const
{
  return m_layer == nullptr;
}

// MARK: Texture Creation

static MTLTextureType FromAbstract(AbstractTextureType type, bool multisample)
{
  switch (type)
  {
  case AbstractTextureType::Texture_2D:
    return multisample ? MTLTextureType2DMultisample : MTLTextureType2D;
  case AbstractTextureType::Texture_2DArray:
    return multisample ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
  case AbstractTextureType::Texture_CubeMap:
    return MTLTextureTypeCube;
  }

  ASSERT(false);
  return MTLTextureType2DArray;
}

std::unique_ptr<AbstractTexture> Metal::Gfx::CreateTexture(const TextureConfig& config,
                                                           std::string_view name)
{
  @autoreleasepool
  {
    MRCOwned<MTLTextureDescriptor*> desc = MRCTransfer([MTLTextureDescriptor new]);
    [desc setTextureType:FromAbstract(config.type, config.samples > 1)];
    [desc setPixelFormat:Util::FromAbstract(config.format)];
    [desc setWidth:config.width];
    [desc setHeight:config.height];
    [desc setMipmapLevelCount:config.levels];
    [desc setArrayLength:config.layers];
    [desc setSampleCount:config.samples];
    [desc setStorageMode:MTLStorageModePrivate];
    MTLTextureUsage usage = MTLTextureUsageShaderRead;
    if (config.IsRenderTarget())
      usage |= MTLTextureUsageRenderTarget;
    if (config.IsComputeImage())
      usage |= MTLTextureUsageShaderWrite;
    [desc setUsage:usage];
    id<MTLTexture> texture = [g_device newTextureWithDescriptor:desc];
    if (!texture)
      return nullptr;

    if (name.empty())
      [texture setLabel:[NSString stringWithFormat:@"Texture %d", m_texture_counter++]];
    else
      [texture setLabel:MRCTransfer([[NSString alloc] initWithBytes:name.data()
                                                             length:name.size()
                                                           encoding:NSUTF8StringEncoding])];
    return std::make_unique<Texture>(MRCTransfer(texture), config);
  }
}

std::unique_ptr<AbstractStagingTexture>
Metal::Gfx::CreateStagingTexture(StagingTextureType type, const TextureConfig& config)
{
  @autoreleasepool
  {
    const size_t stride = config.GetStride();
    const size_t buffer_size = stride * static_cast<size_t>(config.height);

    MTLResourceOptions options = MTLStorageModeShared;
    if (type == StagingTextureType::Upload)
      options |= MTLResourceCPUCacheModeWriteCombined;

    id<MTLBuffer> buffer = [g_device newBufferWithLength:buffer_size options:options];
    if (!buffer)
      return nullptr;
    [buffer
        setLabel:[NSString stringWithFormat:@"Staging Texture %d", m_staging_texture_counter++]];
    return std::make_unique<StagingTexture>(MRCTransfer(buffer), type, config);
  }
}

std::unique_ptr<AbstractFramebuffer>
Metal::Gfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                              std::vector<AbstractTexture*> additional_color_attachments)
{
  AbstractTexture* const either_attachment = color_attachment ? color_attachment : depth_attachment;
  return std::make_unique<Framebuffer>(
      color_attachment, depth_attachment, std::move(additional_color_attachments),
      either_attachment->GetWidth(), either_attachment->GetHeight(), either_attachment->GetLayers(),
      either_attachment->GetSamples());
}

// MARK: Pipeline Creation

std::unique_ptr<AbstractShader>
Metal::Gfx::CreateShaderFromSource(ShaderStage stage, std::string_view source,
                                   VideoCommon::ShaderIncluder* shader_includer,
                                   std::string_view name)
{
  std::optional<std::string> msl = Util::TranslateShaderToMSL(stage, source, shader_includer);
  if (!msl.has_value())
  {
    PanicAlertFmt("Failed to convert shader {} to MSL", name);
    return nullptr;
  }

  return CreateShaderFromMSL(stage, std::move(*msl), source, name);
}

std::unique_ptr<AbstractShader> Metal::Gfx::CreateShaderFromBinary(ShaderStage stage,
                                                                   const void* data, size_t length,
                                                                   std::string_view name)
{
  return CreateShaderFromMSL(stage, std::string(static_cast<const char*>(data), length), {}, name);
}

// clang-format off

static const char* StageFilename(ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex:   return "vs";
  case ShaderStage::Geometry: return "gs";
  case ShaderStage::Pixel:    return "ps";
  case ShaderStage::Compute:  return "cs";
  }
}

static NSString* GenericShaderName(ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex:   return @"Vertex shader %d";
  case ShaderStage::Geometry: return @"Geometry shader %d";
  case ShaderStage::Pixel:    return @"Pixel shader %d";
  case ShaderStage::Compute:  return @"Compute shader %d";
  }
}

// clang-format on

std::unique_ptr<AbstractShader> Metal::Gfx::CreateShaderFromMSL(ShaderStage stage, std::string msl,
                                                                std::string_view glsl,
                                                                std::string_view name)
{
  @autoreleasepool
  {
    NSError* err = nullptr;
    auto DumpBadShader = [&](std::string_view msg) {
      static int counter = 0;
      std::string filename = VideoBackendBase::BadShaderFilename(StageFilename(stage), counter++);
      std::ofstream stream(filename);
      if (stream.good())
      {
        stream << msl << std::endl;
        stream << "/*" << std::endl;
        stream << msg << std::endl;
        stream << "Error:" << std::endl;
        stream << [[err localizedDescription] UTF8String] << std::endl;
        if (!glsl.empty())
        {
          stream << "Original GLSL:" << std::endl;
          stream << glsl << std::endl;
        }
        else
        {
          stream << "Shader was created with cached MSL so no GLSL is available." << std::endl;
        }
      }

      stream << std::endl;
      stream << "Dolphin Version: " << Common::GetScmRevStr() << std::endl;
      stream << "Video Backend: " << g_video_backend->GetDisplayName() << std::endl;
      stream << "*/" << std::endl;
      stream.close();

      PanicAlertFmt("{} (written to {})\n", msg, filename);
    };

    auto lib = MRCTransfer([g_device newLibraryWithSource:[NSString stringWithUTF8String:msl.data()]
                                                  options:nil
                                                    error:&err]);
    if (err)
    {
      DumpBadShader(fmt::format("Failed to compile {}", name));
      return nullptr;
    }
    auto fn = MRCTransfer([lib newFunctionWithName:@"main0"]);
    if (!fn)
    {
      DumpBadShader(fmt::format("Shader {} is missing its main0 function", name));
      return nullptr;
    }
    if (!name.empty())
      [fn setLabel:MRCTransfer([[NSString alloc] initWithBytes:name.data()
                                                        length:name.size()
                                                      encoding:NSUTF8StringEncoding])];
    else
      [fn setLabel:[NSString stringWithFormat:GenericShaderName(stage),
                                              m_shader_counter[static_cast<u32>(stage)]++]];
    [lib setLabel:[fn label]];
    if (stage == ShaderStage::Compute)
    {
      MTLComputePipelineReflection* reflection = nullptr;
      auto desc = [MTLComputePipelineDescriptor new];
      [desc setComputeFunction:fn];
      [desc setLabel:[fn label]];
      MRCOwned<id<MTLComputePipelineState>> pipeline =
          MRCTransfer([g_device newComputePipelineStateWithDescriptor:desc
                                                              options:MTLPipelineOptionArgumentInfo
                                                           reflection:&reflection
                                                                error:&err]);
      if (err)
      {
        DumpBadShader(fmt::format("Failed to compile compute pipeline {}", name));
        return nullptr;
      }
      return std::make_unique<ComputePipeline>(stage, reflection, std::move(msl), std::move(fn),
                                               std::move(pipeline));
    }
    return std::make_unique<Shader>(stage, std::move(msl), std::move(fn));
  }
}

std::unique_ptr<NativeVertexFormat>
Metal::Gfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  @autoreleasepool
  {
    return std::make_unique<VertexFormat>(vtx_decl);
  }
}

std::unique_ptr<AbstractPipeline> Metal::Gfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                             const void* cache_data,
                                                             size_t cache_data_length)
{
  return g_object_cache->CreatePipeline(config);
}

void Metal::Gfx::Flush()
{
  @autoreleasepool
  {
    g_state_tracker->FlushEncoders();
  }
}

void Metal::Gfx::WaitForGPUIdle()
{
  @autoreleasepool
  {
    g_state_tracker->FlushEncoders();
    g_state_tracker->WaitForFlushedEncoders();
  }
}

void Metal::Gfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  if (bits & CONFIG_CHANGE_BIT_VSYNC)
    [m_layer setDisplaySyncEnabled:g_ActiveConfig.bVSyncActive];

  if (bits & CONFIG_CHANGE_BIT_ANISOTROPY)
  {
    g_object_cache->ReloadSamplers();
    g_state_tracker->ReloadSamplers();
  }
}

void Metal::Gfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable,
                             bool alpha_enable, bool z_enable, u32 color, u32 z)
{
  u32 framebuffer_width = m_current_framebuffer->GetWidth();
  u32 framebuffer_height = m_current_framebuffer->GetHeight();
  // All Metal render passes are fullscreen, so we can only run a fast clear if the target is too
  if (target_rc == MathUtil::Rectangle<int>(0, 0, framebuffer_width, framebuffer_height))
  {
    // Determine whether the EFB has an alpha channel. If it doesn't, we can clear the alpha
    // channel to 0xFF. This hopefully allows us to use the fast path in most cases.
    if (bpmem.zcontrol.pixel_format == PixelFormat::RGB565_Z16 ||
        bpmem.zcontrol.pixel_format == PixelFormat::RGB8_Z24 ||
        bpmem.zcontrol.pixel_format == PixelFormat::Z24)
    {
      // Force alpha writes, and clear the alpha channel. This is different from the other backends,
      // where the existing values of the alpha channel are preserved.
      alpha_enable = true;
      color &= 0x00FFFFFF;
    }

    bool c_ok = (color_enable && alpha_enable) ||
                g_state_tracker->GetCurrentFramebuffer()->GetColorFormat() ==
                    AbstractTextureFormat::Undefined;
    bool z_ok = z_enable || g_state_tracker->GetCurrentFramebuffer()->GetDepthFormat() ==
                                AbstractTextureFormat::Undefined;
    if (c_ok && z_ok)
    {
      @autoreleasepool
      {
        // clang-format off
        MTLClearColor clear_color = MTLClearColorMake(
            static_cast<double>((color >> 16) & 0xFF) / 255.0,
            static_cast<double>((color >>  8) & 0xFF) / 255.0,
            static_cast<double>((color >>  0) & 0xFF) / 255.0,
            static_cast<double>((color >> 24) & 0xFF) / 255.0);
        // clang-format on
        float z_normalized = static_cast<float>(z & 0xFFFFFF) / 16777216.0f;
        if (!g_backend_info.bSupportsReversedDepthRange)
          z_normalized = 1.f - z_normalized;
        g_state_tracker->BeginClearRenderPass(clear_color, z_normalized);
        return;
      }
    }
  }

  g_state_tracker->EnableEncoderLabel(false);
  AbstractGfx::ClearRegion(target_rc, color_enable, alpha_enable, z_enable, color, z);
  g_state_tracker->EnableEncoderLabel(true);
}

void Metal::Gfx::SetPipeline(const AbstractPipeline* pipeline)
{
  g_state_tracker->SetPipeline(static_cast<const Pipeline*>(pipeline));
}

void Metal::Gfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  // Shouldn't be bound as a texture.
  if (AbstractTexture* color = framebuffer->GetColorAttachment())
    g_state_tracker->UnbindTexture(static_cast<Texture*>(color)->GetMTLTexture());
  if (AbstractTexture* depth = framebuffer->GetDepthAttachment())
    g_state_tracker->UnbindTexture(static_cast<Texture*>(depth)->GetMTLTexture());

  m_current_framebuffer = framebuffer;
  g_state_tracker->SetCurrentFramebuffer(static_cast<Framebuffer*>(framebuffer));
}

void Metal::Gfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  @autoreleasepool
  {
    SetFramebuffer(framebuffer);
    g_state_tracker->BeginRenderPass(MTLLoadActionDontCare);
  }
}

void Metal::Gfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer,
                                        const ClearColor& color_value, float depth_value)
{
  @autoreleasepool
  {
    SetFramebuffer(framebuffer);
    MTLClearColor color =
        MTLClearColorMake(color_value[0], color_value[1], color_value[2], color_value[3]);
    g_state_tracker->BeginClearRenderPass(color, depth_value);
  }
}

void Metal::Gfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  g_state_tracker->SetScissor(rc);
}

void Metal::Gfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  g_state_tracker->SetTexture(
      index, texture ? static_cast<const Texture*>(texture)->GetMTLTexture() : nullptr);
}

void Metal::Gfx::SetSamplerState(u32 index, const SamplerState& state)
{
  g_state_tracker->SetSampler(index, state);
}

void Metal::Gfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write)
{
  g_state_tracker->SetTexture(index + VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS,
                              texture ? static_cast<const Texture*>(texture)->GetMTLTexture() :
                                        nullptr);
}

void Metal::Gfx::UnbindTexture(const AbstractTexture* texture)
{
  g_state_tracker->UnbindTexture(static_cast<const Texture*>(texture)->GetMTLTexture());
}

void Metal::Gfx::SetViewport(float x, float y, float width, float height, float near_depth,
                             float far_depth)
{
  g_state_tracker->SetViewport(x, y, width, height, near_depth, far_depth);
}

void Metal::Gfx::Draw(u32 base_vertex, u32 num_vertices)
{
  @autoreleasepool
  {
    g_state_tracker->Draw(base_vertex, num_vertices);
  }
}

void Metal::Gfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  @autoreleasepool
  {
    g_state_tracker->DrawIndexed(base_index, num_indices, base_vertex);
  }
}

void Metal::Gfx::DispatchComputeShader(const AbstractShader* shader,  //
                                       u32 groupsize_x, u32 groupsize_y, u32 groupsize_z,
                                       u32 groups_x, u32 groups_y, u32 groups_z)
{
  @autoreleasepool
  {
    g_state_tracker->SetPipeline(static_cast<const ComputePipeline*>(shader));
    g_state_tracker->DispatchComputeShader(groupsize_x, groupsize_y, groupsize_z,  //
                                           groups_x, groups_y, groups_z);
  }
}

// Orca: nextDrawable can block for up to a second when the layer isn't shown (window minimized
// or covered) or the screen is slower than the game. In a single-core session that blocks the CPU
// thread, stalling this game and the opponent's. So no drawable is taken while the surface is
// hidden, and when waiting would slow the game, drawables come from a background acquire one frame
// ahead instead: a frame with no drawable ready is simply not shown. Emulated GPU work is the same
// either way. Reasons for background acquires (Late), and when each ends:
// - Covered: a nextDrawable waited longer than LATE_DRAWABLE. Ends when the frontend next reports
//   the surface hidden; quick acquires prove nothing, since covered windows return some promptly.
// - SlowScreen: the screen refreshes below MIN_SCREEN_HZ, so waiting would slow the game to the
//   screen's rate. Ends when the window is on a screen that keeps up.
// - Paced: PACED_COUNT of the last PACED_WINDOW calls waited PACED_WAIT or more. Ends after
//   PROMPT_ACQUIRES prompt background acquires in a row, doubling each time Paced recurs (up to
//   16x). A single slow wait changes nothing.
static constexpr std::chrono::milliseconds LATE_DRAWABLE{50};
static constexpr int MIN_SCREEN_HZ = 59;
static constexpr std::chrono::microseconds PACED_WAIT{4000};
static constexpr int PACED_WINDOW = 16;
static constexpr int PACED_COUNT = 8;
static constexpr int PROMPT_ACQUIRES = 120;  // about two seconds

struct Metal::Gfx::DrawableProbe
{
  std::mutex mutex;
  bool running = false;
  // The last background acquire's drawable (null after a timeout) until taken, and how long its
  // nextDrawable waited.
  bool done = false;
  MRCOwned<id<CAMetalDrawable>> drawable;
  std::chrono::steady_clock::duration waited{};
};

void Metal::Gfx::StartDrawableProbe()
{
  std::lock_guard lock(m_drawable_probe->mutex);
  if (m_drawable_probe->running || m_drawable_probe->done)
    return;
  m_drawable_probe->running = true;
  std::shared_ptr<DrawableProbe> probe = m_drawable_probe;
  CAMetalLayer* layer = m_layer;  // retained by the block
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool
    {
      const auto start = std::chrono::steady_clock::now();
      id<CAMetalDrawable> drawable = [layer nextDrawable];
      const auto waited = std::chrono::steady_clock::now() - start;
      std::lock_guard probe_lock(probe->mutex);
      probe->running = false;
      probe->done = true;
      probe->drawable = MRCRetain(drawable);
      probe->waited = waited;
    }
  });
}

void Metal::Gfx::DropProbedDrawable()
{
  if (!m_drawable_probe)
    return;
  std::lock_guard lock(m_drawable_probe->mutex);
  m_drawable_probe->drawable.Reset();
  m_drawable_probe->done = false;
}

void Metal::Gfx::SetLate(Late late)
{
  if (late == m_late)
    return;
  const char* why = "";
  switch (late)
  {
  case Late::No:
    why = "from nextDrawable again";
    break;
  case Late::Covered:
    why = "from a background acquire: a wait that long means the window is not shown";
    break;
  case Late::SlowScreen:
    why = "from a background acquire: the screen is slower than the game";
    break;
  case Late::Paced:
    why = "from a background acquire: nextDrawable keeps waiting";
    break;
  }
  NOTICE_LOG_FMT(VIDEO, "Metal: drawables {} ({} Hz screen)", why,
                 VideoCommon::Presenter::SurfaceRefreshRate());
  m_late = late;
  m_paced_waits = 0;
  m_prompt_acquires = 0;
  if (late == Late::No)
    return;
  if (!m_drawable_probe)
    m_drawable_probe = std::make_shared<DrawableProbe>();
  StartDrawableProbe();
}

MRCOwned<id<CAMetalDrawable>> Metal::Gfx::AcquireDrawable()
{
  if (!m_layer)
    return nullptr;
  if (!VideoCommon::Presenter::IsSurfaceVisible())
  {
    // Nothing to show it on. Back to plain nextDrawable once the surface is visible again.
    if (m_late != Late::No)
      NOTICE_LOG_FMT(VIDEO, "Metal: the surface is hidden; nextDrawable again once it shows");
    m_late = Late::No;
    m_paced_waits = 0;
    m_prompt_acquires = 0;
    DropProbedDrawable();
    return nullptr;
  }

  const int hz = VideoCommon::Presenter::SurfaceRefreshRate();
  const bool slow_screen = hz > 0 && hz < MIN_SCREEN_HZ;
  if (slow_screen && (m_late == Late::No || m_late == Late::Paced))
    SetLate(Late::SlowScreen);
  else if (!slow_screen && m_late == Late::SlowScreen)
    SetLate(Late::No);

  if (m_late != Late::No)
  {
    MRCOwned<id<CAMetalDrawable>> drawable;
    std::chrono::steady_clock::duration waited{};
    {
      std::lock_guard lock(m_drawable_probe->mutex);
      if (m_drawable_probe->done)
      {
        drawable = std::move(m_drawable_probe->drawable);
        waited = m_drawable_probe->waited;
        m_drawable_probe->done = false;
      }
    }
    // Drop a drawable sized for the old surface.
    if (drawable && ([[drawable texture] width] != m_backbuffer->GetWidth() ||
                     [[drawable texture] height] != m_backbuffer->GetHeight()))
    {
      drawable.Reset();
    }
    if (m_late == Late::Paced)
    {
      m_prompt_acquires = drawable && waited < PACED_WAIT ? m_prompt_acquires + 1 : 0;
      if (m_prompt_acquires >= PROMPT_ACQUIRES << std::min(m_paced_rounds - 1, 4))
      {
        SetLate(Late::No);
        return drawable;
      }
    }
    StartDrawableProbe();
    return drawable;
  }

  if (m_drawable_probe)
  {
    std::lock_guard lock(m_drawable_probe->mutex);
    // Only one nextDrawable at a time: an earlier background acquire may still be waiting, so
    // this frame isn't shown.
    if (m_drawable_probe->running)
      return nullptr;
    // Drop any drawable the background acquire got after it was no longer needed.
    m_drawable_probe->drawable.Reset();
    m_drawable_probe->done = false;
  }
  const auto start = std::chrono::steady_clock::now();
  MRCOwned<id<CAMetalDrawable>> drawable = MRCRetain([m_layer nextDrawable]);
  const auto waited = std::chrono::steady_clock::now() - start;
  if (waited > LATE_DRAWABLE)
  {
    NOTICE_LOG_FMT(VIDEO, "Metal: nextDrawable waited {} ms{}",
                   std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(),
                   drawable ? "" : " for nothing");
    SetLate(Late::Covered);
    return drawable;
  }
  m_paced_waits = (m_paced_waits << 1 | (waited >= PACED_WAIT ? 1u : 0u)) &
                  ((1u << PACED_WINDOW) - 1);
  if (std::popcount(m_paced_waits) >= PACED_COUNT)
  {
    ++m_paced_rounds;
    SetLate(Late::Paced);
  }
  return drawable;
}

bool Metal::Gfx::BindBackbuffer(const ClearColor& clear_color)
{
  @autoreleasepool
  {
    CheckForSurfaceChange();
    CheckForSurfaceResize();
    m_drawable = AcquireDrawable();
    // Orca: no drawable, no render pass (a pass with no texture is invalid).
    if (!m_drawable)
      return false;
    m_backbuffer->UpdateBackbufferTexture([m_drawable texture]);
    SetAndClearFramebuffer(m_backbuffer.get(), clear_color);
    return true;
  }
}

void Metal::Gfx::PresentBackbuffer()
{
  @autoreleasepool
  {
    g_state_tracker->EndRenderPass();
    if (m_drawable)
    {
      // PresentDrawable refuses to allow Dolphin to present faster than the display's refresh rate
      // when windowed (or fullscreen with vsync enabled, but that's more understandable).
      // On the other hand, it helps Xcode's GPU captures start and stop on frame boundaries
      // which is convenient.  Put it here as a default-off config, which we can override in Xcode.
      // It also seems to improve frame pacing, so enable it by default with vsync
      if (g_ActiveConfig.iUsePresentDrawable == TriState::On ||
          (g_ActiveConfig.iUsePresentDrawable == TriState::Auto && g_ActiveConfig.bVSyncActive))
        [g_state_tracker->GetRenderCmdBuf() presentDrawable:m_drawable];
      else
        [g_state_tracker->GetRenderCmdBuf()
            addScheduledHandler:[drawable = std::move(m_drawable)](id) { [drawable present]; }];
      m_backbuffer->UpdateBackbufferTexture(nullptr);
      m_drawable = nullptr;
    }
    g_state_tracker->FlushEncoders();
  }
}

void Metal::Gfx::CheckForSurfaceChange()
{
  if (!g_presenter->SurfaceChangedTestAndClear())
    return;
  m_layer = MRCRetain(static_cast<CAMetalLayer*>(g_presenter->GetNewSurfaceHandle()));
  // Orca: drop any background acquire still running on the old layer.
  m_drawable_probe.reset();
  m_late = Late::No;
  m_paced_waits = 0;
  m_prompt_acquires = 0;
  m_paced_rounds = 0;
  SetupSurface();
}

void Metal::Gfx::CheckForSurfaceResize()
{
  if (!g_presenter->SurfaceResizedTestAndClear())
    return;
  SetupSurface();
}

void Metal::Gfx::SetupSurface()
{
  auto info = GetSurfaceInfo();

  [m_layer setDrawableSize:{static_cast<double>(info.width), static_cast<double>(info.height)}];

  TextureConfig cfg(info.width, info.height, 1, 1, 1, info.format, AbstractTextureFlag_RenderTarget,
                    AbstractTextureType::Texture_2DArray);
  m_bb_texture = std::make_unique<Texture>(nullptr, cfg);
  m_backbuffer = std::make_unique<Framebuffer>(
      m_bb_texture.get(), nullptr, std::vector<AbstractTexture*>{}, info.width, info.height, 1, 1);

  if (g_presenter)
    g_presenter->SetBackbuffer(info);
}

SurfaceInfo Metal::Gfx::GetSurfaceInfo() const
{
  if (!m_layer)  // Headless
    return {};

  CGSize size = [m_layer bounds].size;
  const float scale = [m_layer contentsScale];

  return {static_cast<u32>(size.width * scale), static_cast<u32>(size.height * scale), scale,
          Util::ToAbstract([m_layer pixelFormat])};
}
