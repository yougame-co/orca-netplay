// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "VideoCommon/CPMemory.h"

class NativeVertexFormat;
struct PortableVertexDeclaration;

namespace OpcodeDecoder
{
enum class Primitive : u8;
}

namespace VertexLoaderManager
{
using NativeVertexFormatMap =
    std::unordered_map<PortableVertexDeclaration, std::unique_ptr<NativeVertexFormat>>;

void Init();
void Clear();

void MarkAllDirty();

// Creates or obtains a pointer to a VertexFormat representing decl.
// If this results in a VertexFormat being created, if the game later uses a matching vertex
// declaration, the one that was previously created will be used.
NativeVertexFormat* GetOrCreateMatchingFormat(const PortableVertexDeclaration& decl);

// For vertex ubershaders, all attributes need to be present, even when the vertex
// format does not contain them. This function returns a vertex format with dummy
// offsets set to the unused attributes.
NativeVertexFormat* GetUberVertexFormat(const PortableVertexDeclaration& decl);

// Returns -1 if buf_size is insufficient, else the amount of bytes consumed
template <bool IsPreprocess = false>
int RunVertices(int vtx_attr_group, OpcodeDecoder::Primitive primitive, int count, const u8* src);

namespace detail
{
// This will look for an existing loader in the global hashmap or create a new one if there is none.
// It should not be used directly because RefreshLoaders() has another cache for fast lookups.
template <bool IsPreprocess = false>
VertexLoaderBase* GetOrCreateLoader(int vtx_attr_group);
}  // namespace detail

NativeVertexFormat* GetCurrentVertexFormat();

// Resolved pointers to array bases. Used by vertex loaders.
extern Common::EnumMap<u8*, CPArray::TexCoord7> cached_arraybases;
void UpdateVertexArrayPointers();

// Orca: indexed vertex reads stay inside guest RAM. The guest picks each array's base, stride and
// every index, so an element can lie past the end of MEM1 or MEM2; such a draw is dropped (no
// emulated state depends on vertex loading). A draw whose elements are all in RAM is unaffected.
struct IndexedRead
{
  CPArray array = CPArray::Position;
  u8 index_bytes = 1;    // 1 or 2
  u8 src_offset = 0;     // where the index is in a raw vertex
  u8 extent = 0;         // bytes read from index * stride on
  bool skip_max = false;  // Position: the largest index skips the vertex, unread by the JITs
  bool reads_before = false;  // RGBA6666: the JIT loaders read from one byte before the element
};
constexpr std::size_t MAX_INDEXED_READS = 14;  // a position, 3 normals, 2 colors, 8 coordinates
// Every indexed read a vertex with this description makes, and (vertex_bytes) the raw vertex's
// size as these offsets count it. nullopt for a layout the loaders don't agree on: a color in one
// of the two invalid formats (no game uses them; Dolphin raises a panic alert for them), whose
// draws are dropped.
std::optional<std::size_t> GetIndexedReads(const TVtxDesc& vtx_desc, const VAT& vtx_attr,
                                           std::array<IndexedRead, MAX_INDEXED_READS>* reads,
                                           u32* vertex_bytes = nullptr);
// The loaders read whole words: past the last element's bytes, at most this many more.
constexpr u32 INDEXED_OVERREAD = 4;
// Whether `count` raw vertices at `src` read only within each array's room: `room` bytes from its
// base to the end of what is readable, over-read included; `before` bytes of RAM before the base.
bool IndexedReadsFit(std::span<const IndexedRead> reads, const u8* src, u32 count, u32 vertex_size,
                     const Common::EnumMap<u32, CPArray::XF_D>& strides,
                     const Common::EnumMap<u32, CPArray::TexCoord7>& room,
                     const Common::EnumMap<u32, CPArray::TexCoord7>& before);

// Position cache for zfreeze (3 vertices, 4 floats each to allow SIMD overwrite).
// These arrays are in reverse order.
extern std::array<std::array<float, 4>, 3> position_cache;
extern std::array<u32, 3> position_matrix_index_cache;
// Needed for the game "LIT", which has text that has lighting enabled, but doesn't have normal
// vectors.  The normals from the last drawn object are used instead.
// See https://bugs.dolphin-emu.org/issues/13635
extern std::array<float, 4> normal_cache;
// Store the tangent and binormal vectors for games that use emboss texgens when the vertex format
// doesn't include them (e.g. RS2 and RS3).  These too are 4 floats each for SIMD overwrites.
extern std::array<float, 4> tangent_cache;
extern std::array<float, 4> binormal_cache;

// VB_HAS_X. Bitmask telling what vertex components are present.
extern u32 g_current_components;

extern BitSet8 g_main_vat_dirty;
extern BitSet8 g_preprocess_vat_dirty;
extern bool g_bases_dirty;  // Main only
extern std::array<VertexLoaderBase*, CP_NUM_VAT_REG> g_main_vertex_loaders;
extern std::array<VertexLoaderBase*, CP_NUM_VAT_REG> g_preprocess_vertex_loaders;
extern bool g_needs_cp_xf_consistency_check;

template <bool IsPreprocess = false>
VertexLoaderBase* RefreshLoader(int vtx_attr_group)
{
  constexpr const BitSet8& attr_dirty = IsPreprocess ? g_preprocess_vat_dirty : g_main_vat_dirty;
  constexpr const auto& vertex_loaders =
      IsPreprocess ? g_preprocess_vertex_loaders : g_main_vertex_loaders;

  VertexLoaderBase* loader;
  if (!attr_dirty[vtx_attr_group]) [[likely]]
  {
    loader = vertex_loaders[vtx_attr_group];
  }
  else [[unlikely]]
  {
    loader = detail::GetOrCreateLoader<IsPreprocess>(vtx_attr_group);
  }

  // Lookup pointers for any vertex arrays.
  if constexpr (!IsPreprocess)
    UpdateVertexArrayPointers();

  return loader;
}

}  // namespace VertexLoaderManager
