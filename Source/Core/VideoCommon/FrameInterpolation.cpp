// Copyright 2026 bt3recompiled
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/FrameInterpolation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BPStructs.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/TMEM.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

namespace FrameInterpolation
{
namespace
{
// XF memory layout (words): position matrices 0x00-0x77, texture matrices
// 0x78-0xFF, normal matrices 0x400-0x45F, post-transform matrices
// 0x500-0x5FF; projection registers 0x1020-0x1026 (6 floats + type).
constexpr u16 XF_TEX_MATRIX_START = 0x78;
constexpr u16 XF_MATRIX_END = 0x100;
constexpr u16 XF_NORMAL_START = 0x400;
constexpr u16 XF_NORMAL_END = 0x460;
constexpr u16 XF_POST_START = 0x500;
constexpr u16 XF_POST_END = 0x600;
constexpr u16 XF_PROJECTION = 0x1020;

struct FrameRecord
{
  std::vector<u8> commands;                // top-level command bytes, in execution order
  std::vector<std::vector<u8>> dlists;     // display list contents, in call order
  std::vector<u8> start_state;             // video state when the frame started
  bool complete_start = false;             // recording began exactly at a frame start
};

// Identifies one transform load across consecutive frames: the display list
// it lives in and the load's byte offset inside the list. Games rebuild their
// dynamic lists every frame in alternating buffers, so a list is identified by
// its size and how many lists of that size were already called this frame,
// not by its address. Loads outside display lists use their order among
// top-level transform loads.
struct LoadContext
{
  u32 dl_size = 0;
  const u8* dl_base = nullptr;
  std::unordered_map<u32, u32> dl_calls;
  u32 dl_ordinal = 0;
  u32 top_level_loads = 0;

  void Reset()
  {
    dl_size = 0;
    dl_base = nullptr;
    dl_calls.clear();
    dl_ordinal = 0;
    top_level_loads = 0;
  }
  u64 Key(u16 xf_address, u8 count, const u8* data)
  {
    u64 key;
    if (dl_base)
      key = (u64(dl_size) << 32) ^ (u64(dl_ordinal) << 20) ^ u64(data - dl_base);
    else
      key = (u64(0xFFFFFFFFu) << 32) ^ top_level_loads++;
    return key * 0x9E3779B97F4A7C15ull ^ (u64(xf_address) << 8) ^ count;
  }
};

using LoadMap = std::unordered_map<u64, std::vector<float>>;

struct Stats
{
  u32 blended = 0, unmatched = 0, rejected = 0;
  // Primitives whose vertex data changed since the previous frame.
  u32 prims_blended = 0, prims_unmatched = 0, prims_rejected = 0, prims_unsupported = 0;
  u32 vertices_blended = 0;
  u32 indexed_prims_blended = 0, indexed_vertices_blended = 0;
};

// Location and encoding of the position attribute inside one vertex.
struct PositionLayout
{
  u32 offset = 0;          // bytes before the position (matrix indices)
  u32 components = 0;      // 2 or 3
  ComponentFormat format = ComponentFormat::Float;
  float scale = 1.0f;      // fixed-point: value = raw / 2^frac
  u32 bytes = 0;           // bytes per component
  u32 index_bytes = 0;     // 0: position inline in the vertex; 1/2: index into the array
  u32 array_base = 0;      // physical address of the position array (indexed)
  u32 array_stride = 0;
};

Mode s_mode = [] {
  const char* value = std::getenv("MODERNGEKKO_FRAME_INTERPOLATION");
  if (value && std::string_view(value) == "replay")
    return Mode::Replay;
  if (value && std::string_view(value) == "interpolate")
    return Mode::Interpolate;
  return Mode::Off;
}();
const bool s_verify = std::getenv("MODERNGEKKO_FRAME_INTERPOLATION_VERIFY") != nullptr;
// Optional directory: every 60 frames, raw RGBA dumps of the real and the
// extra image (<frame>-real-<w>x<h>.rgba / <frame>-extra-<w>x<h>.rgba).
const char* const s_dump_dir = std::getenv("MODERNGEKKO_FRAME_INTERPOLATION_DUMP");

FrameRecord s_frame;
bool s_replaying = false;
u32 s_fields_since_copy = 0;
u32 s_field_rate_frames = 0;  // frames in the current 60-frame window shown 1 field
bool s_skip_next_command = false;
size_t s_replay_dlist = 0;
u64 s_frame_index = 0;

LoadContext s_context;
LoadMap s_previous_loads;  // transform loads of the previous frame
LoadMap s_current_loads;   // transform loads of the frame being recorded
Stats s_stats;
std::array<u8, 64 * 4> s_blended_data;
LoadMap s_previous_positions;  // vertex positions of the previous frame's primitives
LoadMap s_current_positions;
std::vector<u8> s_blended_vertices;

// Inline positions, or indices into the position array in guest RAM.
bool GetPositionLayout(u8 vat, PositionLayout* layout)
{
  const TVtxDesc& desc = g_main_cp_state.vtx_desc;
  switch (desc.low.Position)
  {
  case VertexComponentFormat::Direct:
    layout->index_bytes = 0;
    break;
  case VertexComponentFormat::Index8:
    layout->index_bytes = 1;
    break;
  case VertexComponentFormat::Index16:
    layout->index_bytes = 2;
    break;
  default:
    return false;
  }
  layout->array_base = g_main_cp_state.array_bases[CPArray::Position];
  layout->array_stride = g_main_cp_state.array_strides[CPArray::Position];
  u32 offset = desc.low.PosMatIdx ? 1 : 0;
  for (u32 i = 0; i < 8; ++i)
    offset += desc.low.TexMatIdx[i] ? 1 : 0;
  const auto& g0 = g_main_cp_state.vtx_attr[vat & 7].g0;
  layout->offset = offset;
  layout->components = g0.PosElements == CoordComponentCount::XYZ ? 3 : 2;
  layout->format = g0.PosFormat;
  switch (layout->format)
  {
  case ComponentFormat::Float:
    layout->bytes = 4;
    layout->scale = 1.0f;
    return true;
  case ComponentFormat::Short:
  case ComponentFormat::UShort:
    layout->bytes = 2;
    break;
  case ComponentFormat::Byte:
  case ComponentFormat::UByte:
    layout->bytes = 1;
    break;
  default:
    return false;
  }
  layout->scale = 1.0f / static_cast<float>(1u << g0.PosFrac);
  return true;
}

float ReadComponent(const u8* p, const PositionLayout& layout)
{
  switch (layout.format)
  {
  case ComponentFormat::Float:
  {
    const u32 bits = Common::swap32(p);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  }
  case ComponentFormat::Short:
    return static_cast<s16>(Common::swap16(p)) * layout.scale;
  case ComponentFormat::UShort:
    return Common::swap16(p) * layout.scale;
  case ComponentFormat::Byte:
    return static_cast<s8>(*p) * layout.scale;
  default:
    return *p * layout.scale;
  }
}

void WriteComponent(u8* p, float value, const PositionLayout& layout)
{
  switch (layout.format)
  {
  case ComponentFormat::Float:
  {
    u32 bits;
    std::memcpy(&bits, &value, 4);
    const u32 be = Common::swap32(bits);
    std::memcpy(p, &be, 4);
    return;
  }
  case ComponentFormat::Short:
  case ComponentFormat::UShort:
  {
    const long raw = std::lround(value / layout.scale);
    const u16 be = Common::swap16(static_cast<u16>(raw));
    std::memcpy(p, &be, 2);
    return;
  }
  default:
    *p = static_cast<u8>(std::lround(value / layout.scale));
    return;
  }
}

// Pointer to vertex v's position: inline in the vertex data, or in the guest
// position array for indexed formats (nullptr if outside guest memory).
u8* PositionPointer(const u8* data, u32 vertex_size, u32 v, const PositionLayout& layout)
{
  const u8* field = data + v * vertex_size + layout.offset;
  if (layout.index_bytes == 0)
    return const_cast<u8*>(field);
  const u32 index = layout.index_bytes == 1 ? *field : Common::swap16(field);
  auto& memory = Core::System::GetInstance().GetMemory();
  return memory.GetPointerForRange(layout.array_base + index * layout.array_stride,
                                   layout.components * layout.bytes);
}

bool ReadPositions(const u8* data, u32 vertex_size, u16 count, const PositionLayout& layout,
                   std::vector<float>* positions)
{
  positions->resize(static_cast<size_t>(count) * layout.components);
  for (u32 v = 0; v < count; ++v)
  {
    const u8* p = PositionPointer(data, vertex_size, v, layout);
    if (!p)
      return false;
    for (u32 c = 0; c < layout.components; ++c)
      (*positions)[v * layout.components + c] = ReadComponent(p + c * layout.bytes, layout);
  }
  return true;
}

// Guest RAM bytes overwritten with blended array positions for the primitive
// being converted; restored right after the vertex loader has read them.
struct RamPatch
{
  u8* address;
  std::array<u8, 12> original;
  u32 size;
};
std::vector<RamPatch> s_ram_patches;

// A primitive whose vertices moved further than twice its own size in one
// frame is treated as a different object (or a respawned particle).
bool PlausibleMotion(const std::vector<float>& a, const std::vector<float>& b, u32 components)
{
  float extent = 0.0f, motion = 0.0f;
  for (u32 c = 0; c < components; ++c)
  {
    float lo = b[c], hi = b[c];
    for (size_t i = c; i < b.size(); i += components)
    {
      lo = std::min(lo, b[i]);
      hi = std::max(hi, b[i]);
    }
    extent = std::max(extent, hi - lo);
  }
  for (size_t i = 0; i < a.size(); i += components)
  {
    float d = 0.0f;
    for (u32 c = 0; c < components; ++c)
      d += (a[i + c] - b[i + c]) * (a[i + c] - b[i + c]);
    motion = std::max(motion, std::sqrt(d));
  }
  return motion <= 2.0f * extent + 1e-3f;
}

// Inline-position primitives are mostly effect particles that the game
// respawns every frame; the same slot can hold a different particle. Accept
// only near-rigid motion: every vertex moves almost like the primitive's mean
// displacement, by no more than the primitive's own size.
bool RigidMotion(const std::vector<float>& a, const std::vector<float>& b, u32 components)
{
  const size_t count = b.size() / components;
  float mean[3] = {}, lo[3], hi[3];
  for (u32 c = 0; c < components; ++c)
  {
    lo[c] = hi[c] = b[c];
    for (size_t v = 0; v < count; ++v)
    {
      const size_t i = v * components + c;
      mean[c] += (b[i] - a[i]) / static_cast<float>(count);
      lo[c] = std::min(lo[c], b[i]);
      hi[c] = std::max(hi[c], b[i]);
    }
  }
  float extent = 0.0f, mean_length = 0.0f;
  for (u32 c = 0; c < components; ++c)
  {
    extent = std::max(extent, hi[c] - lo[c]);
    mean_length += mean[c] * mean[c];
  }
  if (std::sqrt(mean_length) > extent + 1e-3f)
    return false;
  for (size_t v = 0; v < count; ++v)
  {
    float deviation = 0.0f;
    for (u32 c = 0; c < components; ++c)
    {
      const size_t i = v * components + c;
      const float d = (b[i] - a[i]) - mean[c];
      deviation += d * d;
    }
    if (std::sqrt(deviation) > 0.25f * extent + 1e-3f)
      return false;
  }
  return true;
}

bool IsTracked(u16 address, u8 count)
{
  return (address < XF_MATRIX_END && count >= 8) ||
         (address >= XF_NORMAL_START && address < XF_NORMAL_END && count == 9) ||
         (address >= XF_POST_START && address < XF_POST_END && count >= 8) ||
         (address == XF_PROJECTION && count >= 7);
}

std::vector<float> ReadFloats(const u8* data, u8 count)
{
  std::vector<float> values(count);
  for (u8 i = 0; i < count; ++i)
  {
    const u32 bits = Common::swap32(data + i * 4);
    std::memcpy(&values[i], &bits, 4);
  }
  return values;
}

void WriteFloats(const std::vector<float>& values, u8* out)
{
  for (size_t i = 0; i < values.size(); ++i)
  {
    u32 bits;
    std::memcpy(&bits, &values[i], 4);
    const u32 be = Common::swap32(bits);
    std::memcpy(out + i * 4, &be, 4);
  }
}

float Lerp(float a, float b, float t)
{
  return a + (b - a) * t;
}

struct Quat
{
  float w, x, y, z;
};

Quat FromMatrix(const float r[3][3])
{
  Quat q;
  const float trace = r[0][0] + r[1][1] + r[2][2];
  if (trace > 0.0f)
  {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q = {0.25f * s, (r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s};
  }
  else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
  {
    const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
    q = {(r[2][1] - r[1][2]) / s, 0.25f * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s};
  }
  else if (r[1][1] > r[2][2])
  {
    const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
    q = {(r[0][2] - r[2][0]) / s, (r[0][1] + r[1][0]) / s, 0.25f * s, (r[1][2] + r[2][1]) / s};
  }
  else
  {
    const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
    q = {(r[1][0] - r[0][1]) / s, (r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25f * s};
  }
  return q;
}

void ToMatrix(Quat q, float r[3][3])
{
  const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  q = {q.w / n, q.x / n, q.y / n, q.z / n};
  r[0][0] = 1 - 2 * (q.y * q.y + q.z * q.z);
  r[0][1] = 2 * (q.x * q.y - q.z * q.w);
  r[0][2] = 2 * (q.x * q.z + q.y * q.w);
  r[1][0] = 2 * (q.x * q.y + q.z * q.w);
  r[1][1] = 1 - 2 * (q.x * q.x + q.z * q.z);
  r[1][2] = 2 * (q.y * q.z - q.x * q.w);
  r[2][0] = 2 * (q.x * q.z - q.y * q.w);
  r[2][1] = 2 * (q.y * q.z + q.x * q.w);
  r[2][2] = 1 - 2 * (q.x * q.x + q.y * q.y);
}

// Splits a linear 3x3 part into rotation and per-column scale. Fails for
// degenerate or sheared matrices, which are then blended per element.
bool Decompose(const float m[3][3], float rotation[3][3], float scale[3])
{
  for (int c = 0; c < 3; ++c)
  {
    scale[c] = std::sqrt(m[0][c] * m[0][c] + m[1][c] * m[1][c] + m[2][c] * m[2][c]);
    if (scale[c] < 1e-6f)
      return false;
    for (int r = 0; r < 3; ++r)
      rotation[r][c] = m[r][c] / scale[c];
  }
  const float det = rotation[0][0] * (rotation[1][1] * rotation[2][2] - rotation[1][2] * rotation[2][1]) -
                    rotation[0][1] * (rotation[1][0] * rotation[2][2] - rotation[1][2] * rotation[2][0]) +
                    rotation[0][2] * (rotation[1][0] * rotation[2][1] - rotation[1][1] * rotation[2][0]);
  if (det < 0.0f)
  {
    // Mirrored transform: carry the reflection in the first scale.
    scale[0] = -scale[0];
    for (int r = 0; r < 3; ++r)
      rotation[r][0] = -rotation[r][0];
  }
  for (int a = 0; a < 3; ++a)
  {
    for (int b = a + 1; b < 3; ++b)
    {
      const float dot = rotation[0][a] * rotation[0][b] + rotation[1][a] * rotation[1][b] +
                        rotation[2][a] * rotation[2][b];
      if (std::abs(dot) > 0.05f)
        return false;
    }
  }
  return true;
}

// Blends an affine transform (rows of `stride` floats; translation in column
// 3 when stride is 4). Returns false when the pair is implausible.
bool BlendTransform(const std::vector<float>& a, const std::vector<float>& b, size_t stride,
                    float t, std::vector<float>* out)
{
  float ma[3][3], mb[3][3];
  for (int r = 0; r < 3; ++r)
  {
    for (int c = 0; c < 3; ++c)
    {
      ma[r][c] = a[r * stride + c];
      mb[r][c] = b[r * stride + c];
    }
  }
  float ra[3][3], rb[3][3], sa[3], sb[3];
  *out = b;
  if (!Decompose(ma, ra, sa) || !Decompose(mb, rb, sb))
    return false;
  for (int c = 0; c < 3; ++c)
  {
    if (sa[c] * sb[c] <= 0.0f)
      return false;
    const float ratio = sa[c] / sb[c];
    if (ratio < 0.67f || ratio > 1.5f)
      return false;
  }
  Quat qa = FromMatrix(ra), qb = FromMatrix(rb);
  float dot = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z;
  if (dot < 0.0f)
  {
    qa = {-qa.w, -qa.x, -qa.y, -qa.z};
    dot = -dot;
  }
  if (dot < 0.7071f)  // more than 90 degrees apart in one frame: a cut
    return false;
  if (stride == 4)
  {
    const float da = std::sqrt(a[3] * a[3] + a[7] * a[7] + a[11] * a[11]);
    const float db = std::sqrt(b[3] * b[3] + b[7] * b[7] + b[11] * b[11]);
    const float dx = a[3] - b[3], dy = a[7] - b[7], dz = a[11] - b[11];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > 0.35f * std::max(da, db) + 1.0f)
      return false;
  }
  // Normalized linear blend is close to slerp for nearby rotations.
  const Quat q{Lerp(qa.w, qb.w, t), Lerp(qa.x, qb.x, t), Lerp(qa.y, qb.y, t), Lerp(qa.z, qb.z, t)};
  float rotation[3][3];
  ToMatrix(q, rotation);
  for (int r = 0; r < 3; ++r)
  {
    for (int c = 0; c < 3; ++c)
      (*out)[r * stride + c] = rotation[r][c] * Lerp(sa[c], sb[c], t);
    if (stride == 4)
      (*out)[r * stride + 3] = Lerp(a[r * stride + 3], b[r * stride + 3], t);
  }
  return true;
}

bool BlendElements(const std::vector<float>& a, const std::vector<float>& b, size_t count,
                   float t, std::vector<float>* out)
{
  *out = b;
  for (size_t i = 0; i < count; ++i)
  {
    if (std::abs(a[i] - b[i]) > 0.5f * std::max(std::abs(a[i]), std::abs(b[i])) + 0.05f)
      return false;
  }
  for (size_t i = 0; i < count; ++i)
    (*out)[i] = Lerp(a[i], b[i], t);
  return true;
}

bool Blend(u16 address, const std::vector<float>& previous, const std::vector<float>& current,
           float t, std::vector<float>* out)
{
  if (address == XF_PROJECTION)
  {
    u32 type_a, type_b;
    std::memcpy(&type_a, &previous[6], 4);
    std::memcpy(&type_b, &current[6], 4);
    return type_a == type_b && BlendElements(previous, current, 6, t, out);
  }
  if (address < XF_TEX_MATRIX_START && current.size() == 12)
    return BlendTransform(previous, current, 4, t, out);
  if (address >= XF_NORMAL_START && address < XF_NORMAL_END && current.size() == 9)
    return BlendTransform(previous, current, 3, t, out);
  return BlendElements(previous, current, current.size(), t, out);
}

// The subset of VideoCommon_DoState that describes GPU register/memory state
// (not the FIFO, presenter or caches). Order must stay identical for
// save/restore.
void DoGpuState(PointerWrap& p)
{
  auto& system = Core::System::GetInstance();
  p.Do(bpmem);
  p.Do(g_main_cp_state);
  p.Do(xfmem);
  p.DoArray(s_tex_mem);
  TMEM::DoState(p);
  system.GetPixelShaderManager().DoState(p);
  system.GetVertexShaderManager().DoState(p);
  system.GetGeometryShaderManager().DoState(p);
  system.GetXFStateManager().DoState(p);
}

std::vector<u8> SaveGpuState()
{
  g_vertex_manager->Flush();
  u8* measure = nullptr;
  PointerWrap p_measure(&measure, 0, PointerWrap::Mode::Measure);
  DoGpuState(p_measure);
  std::vector<u8> buffer(reinterpret_cast<size_t>(measure));
  u8* ptr = buffer.data();
  PointerWrap p(&ptr, buffer.size(), PointerWrap::Mode::Write);
  DoGpuState(p);
  return buffer;
}

void RestoreGpuState(std::vector<u8>& buffer)
{
  g_vertex_manager->Flush();
  u8* ptr = buffer.data();
  PointerWrap p(&ptr, buffer.size(), PointerWrap::Mode::Read);
  DoGpuState(p);
  CopyPreprocessCPStateFromMain();
  BPReload();
  VertexLoaderManager::MarkAllDirty();
}

bool ReadTexture(const AbstractTexture* texture, const MathUtil::Rectangle<int>& rect,
                 std::vector<u8>* out)
{
  if (!texture)
    return false;
  auto staging = g_gfx->CreateStagingTexture(StagingTextureType::Readback, texture->GetConfig());
  if (!staging)
    return false;
  staging->CopyFromTexture(texture, rect, 0, 0, rect);
  staging->Flush();
  const size_t row = static_cast<size_t>(rect.GetWidth()) * staging->GetTexelSize();
  out->resize(row * rect.GetHeight());
  staging->ReadTexels(rect, out->data(), static_cast<u32>(row));
  return true;
}

void DumpImage(const RcTcacheEntry& entry, const MathUtil::Rectangle<int>& rect, const char* kind)
{
  std::vector<u8> pixels;
  if (!entry || !ReadTexture(entry->texture.get(), rect, &pixels))
    return;
  const std::string path = std::string(s_dump_dir) + "/" + std::to_string(s_frame_index) + "-" +
                           kind + "-" + std::to_string(rect.GetWidth()) + "x" +
                           std::to_string(rect.GetHeight()) + ".rgba";
  if (FILE* file = std::fopen(path.c_str(), "wb"))
  {
    std::fwrite(pixels.data(), 1, pixels.size(), file);
    std::fclose(file);
  }
}

void VerifyReplay(const RcTcacheEntry& real, const RcTcacheEntry& replayed,
                  const MathUtil::Rectangle<int>& rect)
{
  std::vector<u8> a, b;
  if (!real || !replayed || !ReadTexture(real->texture.get(), rect, &a) ||
      !ReadTexture(replayed->texture.get(), rect, &b) || a.size() != b.size())
  {
    std::fprintf(stderr, "[frameinterp] verify frame %llu: could not read both images\n",
                 static_cast<unsigned long long>(s_frame_index));
    return;
  }
  size_t differing = 0;
  for (size_t i = 0; i < a.size(); ++i)
    differing += a[i] != b[i];
  std::fprintf(stderr, "[frameinterp] verify frame %llu: %s (%zu of %zu bytes differ)\n",
               static_cast<unsigned long long>(s_frame_index),
               differing == 0 ? "identical" : "DIFFERENT", differing, a.size());
}

void StartFrame(std::vector<u8> state)
{
  s_frame.commands.clear();
  s_frame.dlists.clear();
  s_frame.start_state = std::move(state);
  s_frame.complete_start = true;
  s_context.Reset();
}
}  // namespace

Mode GetMode()
{
  return s_mode;
}

void SetMode(Mode mode)
{
  if (std::getenv("MODERNGEKKO_FRAME_INTERPOLATION") == nullptr)
    s_mode = mode;
}

bool IsActive()
{
  return s_mode != Mode::Off;
}

bool IsReplaying()
{
  return s_replaying;
}

void RecordCommand(const u8* data, u32 size)
{
  if (s_mode == Mode::Off || s_replaying)
    return;
  if (s_skip_next_command)
  {
    s_skip_next_command = false;
    return;
  }
  s_frame.commands.insert(s_frame.commands.end(), data, data + size);
}

const u8* OnDisplayListCall(const u8* ram_data, u32 address, u32 size)
{
  const u8* data = ram_data;
  if (s_replaying)
  {
    data = nullptr;
    if (s_replay_dlist < s_frame.dlists.size() && s_frame.dlists[s_replay_dlist].size() == size)
      data = s_frame.dlists[s_replay_dlist].data();
    ++s_replay_dlist;
  }
  else if (s_mode != Mode::Off && ram_data)
  {
    s_frame.dlists.emplace_back(ram_data, ram_data + size);
  }
  s_context.dl_size = size;
  s_context.dl_base = data;
  s_context.dl_ordinal = s_context.dl_calls[size]++;
  return data;
}

void OnDisplayListEnd()
{
  s_context.dl_base = nullptr;
  s_context.dl_size = 0;
}

const u8* FilterVertices(u8 vat, u32 vertex_size, u16 num_vertices, const u8* data)
{
  if (s_mode != Mode::Interpolate || num_vertices == 0 || !s_context.dl_base)
    return data;
  PositionLayout layout;
  if (!GetPositionLayout(vat, &layout))
  {
    if (s_replaying)
      ++s_stats.prims_unsupported;
    return data;
  }
  const u32 field_bytes = layout.index_bytes ? layout.index_bytes : layout.components * layout.bytes;
  if (layout.offset + field_bytes > vertex_size)
    return data;
  const u64 key = s_context.Key(0xFFFF, static_cast<u8>(vat), data) ^
                  (u64(num_vertices) << 40) ^ (u64(vertex_size) << 56);
  std::vector<float> current;
  if (!ReadPositions(data, vertex_size, num_vertices, layout, &current))
    return data;
  if (!s_replaying)
  {
    s_current_positions[key] = std::move(current);
    return data;
  }
  const auto previous = s_previous_positions.find(key);
  if (previous == s_previous_positions.end() || previous->second.size() != current.size())
  {
    ++s_stats.prims_unmatched;
    return data;
  }
  if (previous->second == current)
    return data;  // static geometry
  const bool plausible = layout.index_bytes == 0 ?
                             RigidMotion(previous->second, current, layout.components) :
                             PlausibleMotion(previous->second, current, layout.components);
  if (!plausible)
  {
    ++s_stats.prims_rejected;
    return data;
  }
  if (layout.index_bytes != 0)
  {
    // Indexed: write blended positions into the guest array for the duration
    // of this primitive's conversion (the CPU waits for GXDrawDone meanwhile).
    const u32 size = layout.components * layout.bytes;
    for (u32 v = 0; v < num_vertices; ++v)
    {
      u8* p = PositionPointer(data, vertex_size, v, layout);
      RamPatch patch{p, {}, size};
      std::memcpy(patch.original.data(), p, size);
      s_ram_patches.push_back(patch);
      for (u32 c = 0; c < layout.components; ++c)
      {
        const size_t i = v * layout.components + c;
        WriteComponent(p + c * layout.bytes, Lerp(previous->second[i], current[i], 0.5f), layout);
      }
    }
    ++s_stats.indexed_prims_blended;
    s_stats.indexed_vertices_blended += num_vertices;
    return data;
  }
  const size_t bytes = static_cast<size_t>(vertex_size) * num_vertices;
  s_blended_vertices.assign(data, data + bytes);
  for (u32 v = 0; v < num_vertices; ++v)
  {
    u8* p = s_blended_vertices.data() + v * vertex_size + layout.offset;
    for (u32 c = 0; c < layout.components; ++c)
    {
      const size_t i = v * layout.components + c;
      WriteComponent(p + c * layout.bytes, Lerp(previous->second[i], current[i], 0.5f), layout);
    }
  }
  ++s_stats.prims_blended;
  s_stats.vertices_blended += num_vertices;
  return s_blended_vertices.data();
}

void AfterVertices()
{
  // Restore in reverse so vertices sharing an array entry end with the
  // original bytes.
  for (auto it = s_ram_patches.rbegin(); it != s_ram_patches.rend(); ++it)
    std::memcpy(it->address, it->original.data(), it->size);
  s_ram_patches.clear();
}

const u8* FilterXF(u16 address, u8 count, const u8* data)
{
  if (s_mode != Mode::Interpolate || !IsTracked(address, count) || count > 64)
    return data;
  const u64 key = s_context.Key(address, count, data);
  std::vector<float> current = ReadFloats(data, count);
  if (!s_replaying)
  {
    s_current_loads[key] = std::move(current);
    return data;
  }
  const auto previous = s_previous_loads.find(key);
  if (previous == s_previous_loads.end() || previous->second.size() != current.size())
  {
    ++s_stats.unmatched;
    return data;
  }
  std::vector<float> blended;
  if (!Blend(address, previous->second, current, 0.5f, &blended))
  {
    ++s_stats.rejected;
    return data;
  }
  ++s_stats.blended;
  WriteFloats(blended, s_blended_data.data());
  return s_blended_data.data();
}

bool SuppressBPWrite(u8 reg)
{
  return s_replaying && (reg == BPMEM_SETDRAWDONE || reg == BPMEM_PE_TOKEN_ID ||
                         reg == BPMEM_PE_TOKEN_INT_ID);
}

void OnViField()
{
  ++s_fields_since_copy;
}

void OnXfbCopied(u8 copy_reg, u32 copy_value, u32 dest_addr, u32 dest_stride, u32 height,
                 bool in_display_list)
{
  if (s_mode == Mode::Off || s_replaying)
    return;

  // The previous frame stayed on screen for this many fields. Scenes that
  // already render one frame per field (menus at 60 FPS) need no extra image,
  // and showing one there would hide every real frame.
  const u32 fields_shown = s_fields_since_copy;
  s_fields_since_copy = 0;
  const bool scene_at_field_rate = fields_shown == 1;
  s_field_rate_frames += scene_at_field_rate ? 1 : 0;

  // End of frame: the XFB copy and its EFB clear have run. This state is also
  // the start state of the next frame.
  std::vector<u8> end_state = SaveGpuState();

  // Only top-level copies end a recordable frame (the copy command is
  // appended to the top-level stream below).
  if (s_frame.complete_start && !in_display_list && !scene_at_field_rate && dest_stride != 0 &&
      height != 0)
  {
    MathUtil::Rectangle<int> rect;
    RcTcacheEntry real =
        g_texture_cache->GetXFBTexture(dest_addr, dest_stride / 2, height, dest_stride, &rect);

    // The XFB copy command itself has not been recorded yet (commands are
    // recorded after they execute): append it so the replay ends with it.
    const u8 bp_command[5] = {static_cast<u8>(OpcodeDecoder::Opcode::GX_LOAD_BP_REG), copy_reg,
                              static_cast<u8>(copy_value >> 16), static_cast<u8>(copy_value >> 8),
                              static_cast<u8>(copy_value)};
    s_frame.commands.insert(s_frame.commands.end(), bp_command, bp_command + sizeof(bp_command));

    RestoreGpuState(s_frame.start_state);
    s_replaying = true;
    s_replay_dlist = 0;
    s_context.Reset();
    OpcodeDecoder::RunReplay(s_frame.commands.data(), static_cast<u32>(s_frame.commands.size()));
    g_vertex_manager->Flush();
    s_replaying = false;
    RestoreGpuState(end_state);

    MathUtil::Rectangle<int> replay_rect;
    RcTcacheEntry replayed = g_texture_cache->GetXFBTexture(dest_addr, dest_stride / 2, height,
                                                            dest_stride, &replay_rect);
    if (s_verify && s_frame_index % 60 == 0)
      VerifyReplay(real, replayed, rect);
    if (s_dump_dir && s_frame_index % 60 == 0)
    {
      DumpImage(real, rect, "real");
      DumpImage(replayed, rect, "extra");
    }
    else if (s_dump_dir && s_frame_index % 60 == 59)
    {
      // The previous real frame, to check the extra image lies between both.
      DumpImage(real, rect, "real");
    }

    // First VI field shows the replayed image (now at dest_addr in the cache);
    // the presenter swaps the real one in for the following field.
    if (real && replayed && real != replayed)
      g_presenter->SetPendingXFB(dest_addr, std::move(real), rect);
  }

  if (s_mode == Mode::Interpolate && s_frame_index % 60 == 0)
  {
    std::fprintf(stderr, "[frameinterp] frame %llu: %u of the last 60 frames at field rate (no extra image)\n",
                 static_cast<unsigned long long>(s_frame_index), s_field_rate_frames);
    s_field_rate_frames = 0;
    std::fprintf(stderr,
                 "[frameinterp] frame %llu: transforms %u blended, %u unmatched, %u rejected; "
                 "moving primitives %u inline + %u indexed blended (%u + %u vertices), "
                 "%u unmatched, %u rejected, %u unsupported\n",
                 static_cast<unsigned long long>(s_frame_index), s_stats.blended,
                 s_stats.unmatched, s_stats.rejected, s_stats.prims_blended,
                 s_stats.indexed_prims_blended, s_stats.vertices_blended,
                 s_stats.indexed_vertices_blended, s_stats.prims_unmatched,
                 s_stats.prims_rejected, s_stats.prims_unsupported);
  }
  s_stats = {};
  s_previous_loads = std::move(s_current_loads);
  s_current_loads.clear();
  s_previous_positions = std::move(s_current_positions);
  s_current_positions.clear();

  ++s_frame_index;
  StartFrame(std::move(end_state));
  // The current XFB copy command is reported to RecordCommand next; it
  // belongs to the finished frame, so drop it from the new recording.
  s_skip_next_command = !in_display_list;
}
}  // namespace FrameInterpolation
