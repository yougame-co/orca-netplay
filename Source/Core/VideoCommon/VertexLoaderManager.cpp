// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexLoaderManager.h"

#include <algorithm>
#include <bit>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/Logging/Log.h"

#include "Core/DolphinAnalytics.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderBase.h"
#include "VideoCommon/VertexLoader_Color.h"
#include "VideoCommon/VertexLoader_Normal.h"
#include "VideoCommon/VertexLoader_Position.h"
#include "VideoCommon/VertexLoader_TextCoord.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace VertexLoaderManager
{
// Used by zfreeze
std::array<u32, 3> position_matrix_index_cache;
// 3 vertices, 4 floats each to allow SIMD overwrite
alignas(sizeof(std::array<float, 4>)) std::array<std::array<float, 4>, 3> position_cache;
alignas(sizeof(std::array<float, 4>)) std::array<float, 4> normal_cache;
alignas(sizeof(std::array<float, 4>)) std::array<float, 4> tangent_cache;
alignas(sizeof(std::array<float, 4>)) std::array<float, 4> binormal_cache;

static NativeVertexFormatMap s_native_vertex_map;
static NativeVertexFormat* s_current_vtx_fmt;
u32 g_current_components;

typedef std::unordered_map<VertexLoaderUID, std::unique_ptr<VertexLoaderBase>> VertexLoaderMap;
static std::mutex s_vertex_loader_map_lock;
static VertexLoaderMap s_vertex_loader_map;
// TODO - change into array of pointers. Keep a map of all seen so far.

Common::EnumMap<u8*, CPArray::TexCoord7> cached_arraybases;
// Orca: for each cached base, the bytes readable from it and the bytes of RAM before it
// (IndexedReadsFit).
static Common::EnumMap<u32, CPArray::TexCoord7> s_array_room;
static Common::EnumMap<u32, CPArray::TexCoord7> s_array_before;

BitSet8 g_main_vat_dirty;
BitSet8 g_preprocess_vat_dirty;
bool g_bases_dirty;  // Main only
std::array<VertexLoaderBase*, CP_NUM_VAT_REG> g_main_vertex_loaders;
std::array<VertexLoaderBase*, CP_NUM_VAT_REG> g_preprocess_vertex_loaders;
bool g_needs_cp_xf_consistency_check;

void Init()
{
  MarkAllDirty();
  g_main_vertex_loaders.fill(nullptr);
  g_preprocess_vertex_loaders.fill(nullptr);
  SETSTAT(g_stats.num_vertex_loaders, 0);
}

void Clear()
{
  std::lock_guard<std::mutex> lk(s_vertex_loader_map_lock);
  s_vertex_loader_map.clear();
  s_native_vertex_map.clear();
}

// Orca: what a base at `address` (known valid) may read: to the end of its RAM, plus as much of the
// loaders' word over-read past it as the host mapping covers (MEM1 is mapped to the next power of
// two, MEM2 exactly), so an element is checked against RAM's end exactly and the over-read never
// leaves the mapping.
static void SetArrayRoom(Memory::MemoryManager& memory, CPArray array, u32 address)
{
  const std::span<u8> span = memory.GetSpanForAddress(address);
  const bool mem1 = (address & 0x3FFFFFFF) < memory.GetRamSizeReal();
  const u32 mapped_tail = mem1 ? memory.GetRamSize() - memory.GetRamSizeReal() :
                                 memory.GetExRamSize() - memory.GetExRamSizeReal();
  s_array_room[array] = static_cast<u32>(span.size()) + std::min(mapped_tail, INDEXED_OVERREAD);
  // Both regions start at a multiple of 256 MiB (MEM1 at 0, MEM2 at 0x10000000).
  s_array_before[array] = address & 0x0FFFFFFF;
}

void UpdateVertexArrayPointers()
{
  // Anything to update?
  if (!g_bases_dirty) [[likely]]
    return;

  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();

  // Some games such as Burnout 2 can put invalid addresses into
  // the array base registers. (see issue 8591)
  // But the vertex arrays with invalid addresses aren't actually enabled.
  // Note: Only array bases 0 through 11 are used by the Vertex loaders.
  //       12 through 15 are used for loading data into xfmem.
  // We also only update the array base if the vertex description states we are going to use it.
  const auto update = [&memory](CPArray array) {
    const u32 address = g_main_cp_state.array_bases[array];
    u8* const base_pointer = memory.GetPointerForRange(address, sizeof(u32));
    if (base_pointer != nullptr)
    {
      cached_arraybases[array] = base_pointer;
      SetArrayRoom(memory, array, address);
    }
  };

  if (IsIndexed(g_main_cp_state.vtx_desc.low.Position))
    update(CPArray::Position);

  if (IsIndexed(g_main_cp_state.vtx_desc.low.Normal))
    update(CPArray::Normal);

  for (u8 i = 0; i < g_main_cp_state.vtx_desc.low.Color.Size(); i++)
  {
    if (IsIndexed(g_main_cp_state.vtx_desc.low.Color[i]))
      update(CPArray::Color0 + i);
  }

  for (u8 i = 0; i < g_main_cp_state.vtx_desc.high.TexCoord.Size(); i++)
  {
    if (IsIndexed(g_main_cp_state.vtx_desc.high.TexCoord[i]))
      update(CPArray::TexCoord0 + i);
  }

  g_bases_dirty = false;
}

std::optional<std::size_t> GetIndexedReads(const TVtxDesc& vtx_desc, const VAT& vtx_attr,
                                           std::array<IndexedRead, MAX_INDEXED_READS>* reads,
                                           u32* vertex_bytes)
{
  std::size_t count = 0;
  // Matrix indices come first, a byte each.
  u32 offset = std::popcount(vtx_desc.low.Hex & 0x1FF);
  const auto add = [&](CPArray array, VertexComponentFormat format, u32 extent, bool skip_max,
                       bool reads_before) {
    IndexedRead& read = (*reads)[count++];
    read.array = array;
    read.index_bytes = format == VertexComponentFormat::Index8 ? 1 : 2;
    read.src_offset = static_cast<u8>(offset);
    read.extent = static_cast<u8>(extent);
    read.skip_max = skip_max;
    read.reads_before = reads_before;
  };

  const VertexComponentFormat position = vtx_desc.low.Position;
  if (IsIndexed(position))
  {
    const u32 elements = vtx_attr.g0.PosElements == CoordComponentCount::XYZ ? 3 : 2;
    add(CPArray::Position, position, elements * GetElementSize(vtx_attr.g0.PosFormat), true,
        false);
  }
  offset += VertexLoader_Position::GetSize(position, vtx_attr.g0.PosFormat,
                                           vtx_attr.g0.PosElements);

  const VertexComponentFormat normal = vtx_desc.low.Normal;
  if (IsIndexed(normal))
  {
    const u32 vector = 3 * GetElementSize(vtx_attr.g0.NormalFormat);
    if (vtx_attr.g0.NormalElements == NormalComponentCount::NTB && vtx_attr.g0.NormalIndex3)
    {
      // An index each for the normal, tangent and binormal, read at their own offsets.
      const u32 index_bytes = normal == VertexComponentFormat::Index8 ? 1 : 2;
      for (u32 k = 0; k < 3; ++k)
      {
        add(CPArray::Normal, normal, (k + 1) * vector, false, false);
        offset += index_bytes;
      }
      offset -= 3 * index_bytes;
    }
    else
    {
      const u32 vectors = vtx_attr.g0.NormalElements == NormalComponentCount::NTB ? 3 : 1;
      add(CPArray::Normal, normal, vectors * vector, false, false);
    }
  }
  offset += VertexLoader_Normal::GetSize(normal, vtx_attr.g0.NormalFormat,
                                         vtx_attr.g0.NormalElements, vtx_attr.g0.NormalIndex3);

  for (u32 i = 0; i < vtx_desc.low.Color.Size(); i++)
  {
    const VertexComponentFormat color = vtx_desc.low.Color[i];
    const ColorFormat format = vtx_attr.GetColorFormat(i);
    // An invalid format: the raw vertex size counts no bytes for it, while the JIT loaders consume
    // its index (and read nothing direct), so no offset here would match what they read.
    if (color != VertexComponentFormat::NotPresent && format > ColorFormat::RGBA8888)
      return std::nullopt;
    if (IsIndexed(color))
    {
      u32 extent = 4;
      switch (format)
      {
      case ColorFormat::RGB565:
      case ColorFormat::RGBA4444:
        extent = 2;
        break;
      case ColorFormat::RGB888:
      case ColorFormat::RGBA6666:
        extent = 3;
        break;
      default:
        break;
      }
      add(CPArray::Color0 + i, color, extent, false, format == ColorFormat::RGBA6666);
    }
    if (color != VertexComponentFormat::NotPresent)
      offset += VertexLoader_Color::GetSize(color, format);
  }

  for (u32 i = 0; i < vtx_desc.high.TexCoord.Size(); i++)
  {
    const VertexComponentFormat coord = vtx_desc.high.TexCoord[i];
    const ComponentFormat format = vtx_attr.GetTexFormat(i);
    const TexComponentCount elements = vtx_attr.GetTexElements(i);
    if (IsIndexed(coord))
    {
      add(CPArray::TexCoord0 + i, coord,
          (elements == TexComponentCount::ST ? 2 : 1) * GetElementSize(format), false, false);
    }
    offset += VertexLoader_TextCoord::GetSize(coord, format, elements);
  }
  if (vertex_bytes)
    *vertex_bytes = offset;
  return count;
}

bool IndexedReadsFit(std::span<const IndexedRead> reads, const u8* src, u32 count, u32 vertex_size,
                     const Common::EnumMap<u32, CPArray::XF_D>& strides,
                     const Common::EnumMap<u32, CPArray::TexCoord7>& room,
                     const Common::EnumMap<u32, CPArray::TexCoord7>& before)
{
  for (const IndexedRead& read : reads)
  {
    const u64 stride = strides[read.array];
    const u64 limit = room[read.array];
    const u64 tail = u64{read.extent} + INDEXED_OVERREAD;
    if (read.reads_before && before[read.array] == 0)
    {
      // Index 0 would read the byte before RAM.
      for (u32 v = 0; v < count; ++v)
      {
        const u8* p = src + std::size_t{v} * vertex_size + read.src_offset;
        const u32 index = read.index_bytes == 1 ? p[0] : (u32{p[0]} << 8 | p[1]);
        if (index * stride == 0)
          return false;
      }
    }
    const u32 max_index = read.index_bytes == 1 ? 0xFF : 0xFFFF;
    if (max_index * stride + tail <= limit) [[likely]]
      continue;  // every index fits
    for (u32 v = 0; v < count; ++v)
    {
      const u8* p = src + std::size_t{v} * vertex_size + read.src_offset;
      const u32 index = read.index_bytes == 1 ? p[0] : (u32{p[0]} << 8 | p[1]);
      if (read.skip_max && index == max_index)
        continue;
      if (index * stride + tail > limit)
        return false;
    }
  }
  return true;
}

void MarkAllDirty()
{
  g_bases_dirty = true;
  g_main_vat_dirty = BitSet8::AllTrue(8);
  g_preprocess_vat_dirty = BitSet8::AllTrue(8);
  g_needs_cp_xf_consistency_check = true;
}

NativeVertexFormat* GetOrCreateMatchingFormat(const PortableVertexDeclaration& decl)
{
  auto iter = s_native_vertex_map.find(decl);
  if (iter == s_native_vertex_map.end())
  {
    std::unique_ptr<NativeVertexFormat> fmt = g_gfx->CreateNativeVertexFormat(decl);
    auto ipair = s_native_vertex_map.emplace(decl, std::move(fmt));
    iter = ipair.first;
  }

  return iter->second.get();
}

NativeVertexFormat* GetUberVertexFormat(const PortableVertexDeclaration& decl)
{
  // The padding in the structs can cause the memcmp() in the map to create duplicates.
  // Avoid this by initializing the padding to zero.
  PortableVertexDeclaration new_decl;
  static_assert(std::is_trivially_copyable_v<PortableVertexDeclaration>);
  std::memset(static_cast<void*>(&new_decl), 0, sizeof(new_decl));
  new_decl.stride = decl.stride;

  auto MakeDummyAttribute = [](AttributeFormat& attr, ComponentFormat type, int components,
                               bool integer) {
    attr.type = type;
    attr.components = components;
    attr.offset = 0;
    attr.enable = true;
    attr.integer = integer;
  };
  auto CopyAttribute = [](AttributeFormat& attr, const AttributeFormat& src) {
    attr.type = src.type;
    attr.components = src.components;
    attr.offset = src.offset;
    attr.enable = src.enable;
    attr.integer = src.integer;
  };

  if (decl.position.enable)
    CopyAttribute(new_decl.position, decl.position);
  else
    MakeDummyAttribute(new_decl.position, ComponentFormat::Float, 1, false);
  for (size_t i = 0; i < std::size(new_decl.normals); i++)
  {
    if (decl.normals[i].enable)
      CopyAttribute(new_decl.normals[i], decl.normals[i]);
    else
      MakeDummyAttribute(new_decl.normals[i], ComponentFormat::Float, 1, false);
  }
  for (size_t i = 0; i < std::size(new_decl.colors); i++)
  {
    if (decl.colors[i].enable)
      CopyAttribute(new_decl.colors[i], decl.colors[i]);
    else
      MakeDummyAttribute(new_decl.colors[i], ComponentFormat::UByte, 4, false);
  }
  for (size_t i = 0; i < std::size(new_decl.texcoords); i++)
  {
    if (decl.texcoords[i].enable)
      CopyAttribute(new_decl.texcoords[i], decl.texcoords[i]);
    else
      MakeDummyAttribute(new_decl.texcoords[i], ComponentFormat::Float, 1, false);
  }
  if (decl.posmtx.enable)
    CopyAttribute(new_decl.posmtx, decl.posmtx);
  else
    MakeDummyAttribute(new_decl.posmtx, ComponentFormat::UByte, 1, true);

  return GetOrCreateMatchingFormat(new_decl);
}

namespace detail
{
template <bool IsPreprocess>
VertexLoaderBase* GetOrCreateLoader(int vtx_attr_group)
{
  constexpr CPState* state = IsPreprocess ? &g_preprocess_cp_state : &g_main_cp_state;
  constexpr BitSet8& attr_dirty = IsPreprocess ? g_preprocess_vat_dirty : g_main_vat_dirty;
  constexpr auto& vertex_loaders =
      IsPreprocess ? g_preprocess_vertex_loaders : g_main_vertex_loaders;

  VertexLoaderBase* loader;

  // We are not allowed to create a native vertex format on preprocessing as this is on the wrong
  // thread
  bool check_for_native_format = !IsPreprocess;

  VertexLoaderUID uid(state->vtx_desc, state->vtx_attr[vtx_attr_group]);
  std::lock_guard<std::mutex> lk(s_vertex_loader_map_lock);
  VertexLoaderMap::iterator iter = s_vertex_loader_map.find(uid);
  if (iter != s_vertex_loader_map.end())
  {
    loader = iter->second.get();
    check_for_native_format &= !loader->m_native_vertex_format;
  }
  else
  {
    auto [it, added] = s_vertex_loader_map.try_emplace(
        uid,
        VertexLoaderBase::CreateVertexLoader(state->vtx_desc, state->vtx_attr[vtx_attr_group]));
    loader = it->second.get();
    INCSTAT(g_stats.num_vertex_loaders);
  }
  if (check_for_native_format)
  {
    // search for a cached native vertex format
    loader->m_native_vertex_format = GetOrCreateMatchingFormat(loader->m_native_vtx_decl);
  }
  vertex_loaders[vtx_attr_group] = loader;
  attr_dirty[vtx_attr_group] = false;
  return loader;
}

}  // namespace detail

static void CheckCPConfiguration(int vtx_attr_group)
{
  // Validate that the XF input configuration matches the CP configuration
  u32 num_cp_colors = std::count_if(
      g_main_cp_state.vtx_desc.low.Color.begin(), g_main_cp_state.vtx_desc.low.Color.end(),
      [](auto format) { return format != VertexComponentFormat::NotPresent; });
  u32 num_cp_tex_coords = std::count_if(
      g_main_cp_state.vtx_desc.high.TexCoord.begin(), g_main_cp_state.vtx_desc.high.TexCoord.end(),
      [](auto format) { return format != VertexComponentFormat::NotPresent; });

  u32 num_cp_normals;
  if (g_main_cp_state.vtx_desc.low.Normal == VertexComponentFormat::NotPresent)
    num_cp_normals = 0;
  else if (g_main_cp_state.vtx_attr[vtx_attr_group].g0.NormalElements == NormalComponentCount::NTB)
    num_cp_normals = 3;
  else
    num_cp_normals = 1;

  std::optional<u32> num_xf_normals;
  switch (xfmem.invtxspec.numnormals)
  {
  case NormalCount::None:
    num_xf_normals = 0;
    break;
  case NormalCount::Normal:
    num_xf_normals = 1;
    break;
  case NormalCount::NormalTangentBinormal:
  case NormalCount::Invalid:  // see https://bugs.dolphin-emu.org/issues/13070
    num_xf_normals = 3;
    break;
  }

  if (num_cp_colors != xfmem.invtxspec.numcolors || num_cp_normals != num_xf_normals ||
      num_cp_tex_coords != xfmem.invtxspec.numtextures) [[unlikely]]
  {
    PanicAlertFmt("Mismatched configuration between CP and XF stages - {}/{} colors, {}/{} "
                  "normals, {}/{} texture coordinates. Please report on the issue tracker.\n\n"
                  "VCD: {:08x} {:08x}\nVAT {}: {:08x} {:08x} {:08x}\nXF vertex spec: {:08x}",
                  num_cp_colors, xfmem.invtxspec.numcolors, num_cp_normals,
                  num_xf_normals.has_value() ? fmt::to_string(num_xf_normals.value()) : "invalid",
                  num_cp_tex_coords, xfmem.invtxspec.numtextures, g_main_cp_state.vtx_desc.low.Hex,
                  g_main_cp_state.vtx_desc.high.Hex, vtx_attr_group,
                  g_main_cp_state.vtx_attr[vtx_attr_group].g0.Hex,
                  g_main_cp_state.vtx_attr[vtx_attr_group].g1.Hex,
                  g_main_cp_state.vtx_attr[vtx_attr_group].g2.Hex, xfmem.invtxspec.hex);

    // Analytics reporting so we can discover which games have this problem, that way when we
    // eventually simulate the behavior we have test cases for it.
    if (num_cp_colors != xfmem.invtxspec.numcolors) [[unlikely]]
    {
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUColorsBetweenCPAndXF);
    }
    if (num_cp_normals != num_xf_normals) [[unlikely]]
    {
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUNormalsBetweenCPAndXF);
    }
    if (num_cp_tex_coords != xfmem.invtxspec.numtextures) [[unlikely]]
    {
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUTexCoordsBetweenCPAndXF);
    }

    // Don't bail out, though; we can still render something successfully
    // (real hardware seems to hang in this case, though)
  }

  if (g_main_cp_state.matrix_index_a.Hex != xfmem.MatrixIndexA.Hex ||
      g_main_cp_state.matrix_index_b.Hex != xfmem.MatrixIndexB.Hex) [[unlikely]]
  {
    WARN_LOG_FMT(VIDEO,
                 "Mismatched matrix index configuration between CP and XF stages - "
                 "index A: {:08x}/{:08x}, index B {:08x}/{:08x}.",
                 g_main_cp_state.matrix_index_a.Hex, xfmem.MatrixIndexA.Hex,
                 g_main_cp_state.matrix_index_b.Hex, xfmem.MatrixIndexB.Hex);
    DolphinAnalytics::Instance().ReportGameQuirk(
        GameQuirk::MismatchedGPUMatrixIndicesBetweenCPAndXF);
  }

  if (g_main_cp_state.vtx_attr[vtx_attr_group].g0.PosFormat >= ComponentFormat::InvalidFloat5)
  {
    WARN_LOG_FMT(VIDEO, "Invalid position format {} for VAT {} - {:08x} {:08x} {:08x}",
                 g_main_cp_state.vtx_attr[vtx_attr_group].g0.PosFormat, vtx_attr_group,
                 g_main_cp_state.vtx_attr[vtx_attr_group].g0.Hex,
                 g_main_cp_state.vtx_attr[vtx_attr_group].g1.Hex,
                 g_main_cp_state.vtx_attr[vtx_attr_group].g2.Hex);
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::InvalidPositionComponentFormat);
  }
  if (g_main_cp_state.vtx_attr[vtx_attr_group].g0.NormalFormat >= ComponentFormat::InvalidFloat5)
  {
    WARN_LOG_FMT(VIDEO, "Invalid normal format {} for VAT {} - {:08x} {:08x} {:08x}",
                 g_main_cp_state.vtx_attr[vtx_attr_group].g0.NormalFormat, vtx_attr_group,
                 g_main_cp_state.vtx_attr[vtx_attr_group].g0.Hex,
                 g_main_cp_state.vtx_attr[vtx_attr_group].g1.Hex,
                 g_main_cp_state.vtx_attr[vtx_attr_group].g2.Hex);
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::InvalidNormalComponentFormat);
  }
  for (size_t i = 0; i < 8; i++)
  {
    if (g_main_cp_state.vtx_attr[vtx_attr_group].GetTexFormat(i) >= ComponentFormat::InvalidFloat5)
    {
      WARN_LOG_FMT(VIDEO,
                   "Invalid texture coordinate {} format {} for VAT {} - {:08x} {:08x} {:08x}", i,
                   g_main_cp_state.vtx_attr[vtx_attr_group].GetTexFormat(i), vtx_attr_group,
                   g_main_cp_state.vtx_attr[vtx_attr_group].g0.Hex,
                   g_main_cp_state.vtx_attr[vtx_attr_group].g1.Hex,
                   g_main_cp_state.vtx_attr[vtx_attr_group].g2.Hex);
      DolphinAnalytics::Instance().ReportGameQuirk(
          GameQuirk::InvalidTextureCoordinateComponentFormat);
    }
  }
  for (size_t i = 0; i < 2; i++)
  {
    if (g_main_cp_state.vtx_attr[vtx_attr_group].GetColorFormat(i) > ColorFormat::RGBA8888)
    {
      WARN_LOG_FMT(VIDEO, "Invalid color {} format {} for VAT {} - {:08x} {:08x} {:08x}", i,
                   g_main_cp_state.vtx_attr[vtx_attr_group].GetColorFormat(i), vtx_attr_group,
                   g_main_cp_state.vtx_attr[vtx_attr_group].g0.Hex,
                   g_main_cp_state.vtx_attr[vtx_attr_group].g1.Hex,
                   g_main_cp_state.vtx_attr[vtx_attr_group].g2.Hex);
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::InvalidColorComponentFormat);
    }
  }
}

static bool CanSplit(OpcodeDecoder::Primitive primitive)
{
  // Splitting is currently only implemented for the easy cases (individual lines/points/triangles)
  switch (primitive)
  {
  case OpcodeDecoder::Primitive::GX_DRAW_QUADS:
  case OpcodeDecoder::Primitive::GX_DRAW_QUADS_2:
  case OpcodeDecoder::Primitive::GX_DRAW_TRIANGLES:
  case OpcodeDecoder::Primitive::GX_DRAW_LINES:
  case OpcodeDecoder::Primitive::GX_DRAW_POINTS:
    return true;
  default:
    return false;
  }
}

template <bool IsPreprocess>
int RunVertices(int vtx_attr_group, OpcodeDecoder::Primitive primitive, int count, const u8* src)
{
  if (count == 0) [[unlikely]]
    return 0;
  ASSERT(count > 0);

  VertexLoaderBase* loader = RefreshLoader<IsPreprocess>(vtx_attr_group);

  int size = count * loader->m_vertex_size;

  if constexpr (!IsPreprocess)
  {
    // Doing early return for the opposite case would be cleaner
    // but triggers a false unreachable code warning in MSVC debug builds.

    // Orca: a draw with an element outside guest RAM is dropped (UpdateVertexArrayPointers).
    if (!loader->m_indexed_reads_known ||
        (loader->m_num_indexed_reads != 0 &&
         !IndexedReadsFit(std::span(loader->m_indexed_reads.data(), loader->m_num_indexed_reads),
                         src, static_cast<u32>(count),
                         loader->m_vertex_size, g_main_cp_state.array_strides, s_array_room,
                         s_array_before))) [[unlikely]]
    {
      static bool s_logged = false;
      if (!std::exchange(s_logged, true))
        NOTICE_LOG_FMT(VIDEO, "Dropped a draw whose vertex arrays reach past guest RAM");
      return size;
    }

    if (g_needs_cp_xf_consistency_check) [[unlikely]]
    {
      CheckCPConfiguration(vtx_attr_group);
      g_needs_cp_xf_consistency_check = false;
    }

    // If the native vertex format changed, force a flush.
    if (loader->m_native_vertex_format != s_current_vtx_fmt ||
        loader->m_native_components != g_current_components) [[unlikely]]
    {
      g_vertex_manager->Flush();

      s_current_vtx_fmt = loader->m_native_vertex_format;
      g_current_components = loader->m_native_components;
      auto& system = Core::System::GetInstance();
      auto& vertex_shader_manager = system.GetVertexShaderManager();
      vertex_shader_manager.SetVertexFormat(loader->m_native_components,
                                            loader->m_native_vertex_format->GetVertexDeclaration());
    }

    // CPUCull's performance increase comes from encoding fewer GPU commands, not sending less data
    // Therefore it's only useful to check if culling could remove a flush
    bool can_cpu_cull = g_ActiveConfig.bCPUCull &&
                        primitive < OpcodeDecoder::Primitive::GX_DRAW_LINES &&
                        !g_vertex_manager->HasSendableVertices();

    // if cull mode is CULL_ALL, tell VertexManager to skip triangles and quads.
    // They still need to go through vertex loading, because we need to calculate a zfreeze
    // reference slope.
    const bool cullall = (bpmem.genMode.cull_mode == CullMode::All &&
                          primitive < OpcodeDecoder::Primitive::GX_DRAW_LINES);

    const int stride = loader->m_native_vtx_decl.stride;
    do
    {
      const int max_vertices = 16380;  // Max is 16383, but 16380 is divisible by both 4 and 3
      const int run = CanSplit(primitive) && count > max_vertices ? max_vertices : count;
      count -= run;
      DataReader dst = g_vertex_manager->PrepareForAdditionalData(primitive, run, stride,
                                                                  cullall || can_cpu_cull);

      const int num_loaded = loader->RunVertices(src, dst.GetPointer(), run);
      src += loader->m_vertex_size * max_vertices;

      if (can_cpu_cull && !cullall)
      {
        const bool all_culled =
            g_vertex_manager->AreAllVerticesCulled(loader, primitive, dst.GetPointer(), num_loaded);
        if (!all_culled)
        {
          DataReader new_dst = g_vertex_manager->DisableCullAll(stride);
          memmove(new_dst.GetPointer(), dst.GetPointer(), num_loaded * stride);
          can_cpu_cull = false;
        }
      }

      g_vertex_manager->AddIndices(primitive, num_loaded);
      g_vertex_manager->FlushData(num_loaded, stride);

      ADDSTAT(g_stats.this_frame.num_prims, num_loaded);
    } while (count);

    INCSTAT(g_stats.this_frame.num_primitive_joins);
  }
  return size;
}

template int RunVertices<false>(int vtx_attr_group, OpcodeDecoder::Primitive primitive, int count,
                                const u8* src);
template int RunVertices<true>(int vtx_attr_group, OpcodeDecoder::Primitive primitive, int count,
                               const u8* src);

NativeVertexFormat* GetCurrentVertexFormat()
{
  return s_current_vtx_fmt;
}

}  // namespace VertexLoaderManager
