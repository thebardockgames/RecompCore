#include "VideoCommon/NativeGXVerify.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "Common/Image.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/FrameInterpolation.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/NativeGXBridge.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexLoaderBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/XFMemory.h"
#include "moderngekko/native_gx/command_decoder.hpp"
#include "moderngekko/native_gx/efb_copy.hpp"
#include "moderngekko/native_gx/gx_state.hpp"
#include "moderngekko/native_gx/material.hpp"
#include "moderngekko/native_gx/raster_setup.hpp"
#include "moderngekko/native_gx/texture_decoder.hpp"
#include "moderngekko/native_gx/texture_state.hpp"
#ifdef _WIN32
#include "moderngekko/native_gx/d3d12_renderer.hpp"
#endif
#include "moderngekko/native_gx/vertex_loader.hpp"

namespace NativeGXVerify
{
namespace
{
enum class Kind : u8
{
  Cp,
  Xf,
  Indexed,
  Bp,
  Primitive,
  DisplayList,
  Nop,
  Unknown,
};

struct Event
{
  Kind kind;
  u32 a;
  u32 b;
  u32 c;
  u64 hash;

  bool operator==(const Event&) const = default;
};

u64 Hash(const u8* data, size_t size)
{
  u64 hash = 14695981039346656037ULL;
  for (size_t i = 0; i < size; ++i)
    hash = (hash ^ data[i]) * 1099511628211ULL;
  return hash;
}

// MODERNGEKKO_NATIVE_GX_RENDER=<directory> additionally draws every decoded
// primitive with the native D3D12 renderer and compares its depth buffer with
// Dolphin's EFB every MODERNGEKKO_NATIVE_GX_RENDER_STRIDE frames (default 60).
const char* const s_render_dir = std::getenv("MODERNGEKKO_NATIVE_GX_RENDER");

const bool s_enabled = [] {
  const char* value = std::getenv("MODERNGEKKO_NATIVE_GX_VERIFY");
  return (value != nullptr && value[0] != '\0' && value[0] != '0') ||
         (s_render_dir != nullptr && s_render_dir[0] != '\0');
}();

std::vector<Event> s_reference;
std::vector<Event> s_native;

struct Counters
{
  u64 frames = 0;
  u64 commands = 0;
  u64 events = 0;
  u64 primitives = 0;
  u64 display_lists = 0;
  u64 mismatches = 0;
  u64 size_mismatches = 0;
} s_counters;

constexpr u64 kMaxReportedMismatches = 20;

// Vertex loading: Dolphin's output per primitive (in decoding order), checked
// when the native decoder reaches the same primitive with its own CP state.
struct ReferencePrimitive
{
  moderngekko::native_gx::VertexFormatInfo format;
  std::vector<moderngekko::native_gx::LoadedVertex> vertices;
};
std::deque<ReferencePrimitive> s_reference_vertices;
std::vector<u8> s_vertex_scratch;
std::vector<moderngekko::native_gx::LoadedVertex> s_native_vertices;

// Native XF/BP register files (decoded by the native decoder), compared with
// Dolphin's xfmem/bpmem when a frame ends.
moderngekko::native_gx::XfMemory s_xf;
moderngekko::native_gx::BpMemory s_bp;
moderngekko::native_gx::Tmem s_tmem;

// Textures: every distinct texture a draw samples (all mip levels, with its
// palette) is decoded natively and by Dolphin's TexDecoder and compared.
struct TextureCounters
{
  u64 draws_textured = 0;
  u64 checked = 0;
  u64 texels = 0;
  u64 mismatches = 0;
  u64 invalid = 0;
  u64 preloaded = 0;
  u64 tlut_loads = 0;
  u64 tmem_mismatch_frames = 0;
} s_texture_counters;
std::unordered_set<u64> s_checked_textures;
std::vector<u32> s_native_texels;
std::vector<u32> s_dolphin_texels;

// Native rendering comparison (phase 2c).
struct RenderCheck
{
  bool active = false;
  u64 stride = 60;
  u64 frame = 0;           // real XFB copies seen by the native sink
  bool dolphin_ready = false;
  std::vector<float> dolphin_depth;
  std::vector<u32> dolphin_color;  // EFB colour (RGBA8) at the same instant
  std::vector<float> native_depth;
  std::vector<u32> native_color;
  FILE* csv = nullptr;
  u64 samples = 0;
  u64 images = 0;
} s_render;


struct StateCounters
{
  u64 frames = 0;
  u64 xf_mismatches = 0;
  u64 bp_mismatches = 0;
  u64 indexed_failures = 0;
} s_state_counters;

struct VertexCounters
{
  u64 primitives = 0;
  u64 vertices = 0;
  u64 format_mismatches = 0;
  u64 value_mismatches = 0;
  u64 load_failures = 0;
} s_vertex_counters;

std::span<const u8> GuestDisplayList(u32 address, u32 size);
moderngekko::native_gx::CommandDecoder& Decoder();

const moderngekko::native_gx::VertexLoader& NativeVertexLoader()
{
  static const moderngekko::native_gx::VertexLoader loader(&GuestDisplayList);
  return loader;
}

bool SameFloat(float a, float b)
{
  return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

// Compares the loaded attributes the format declares; returns the first
// differing attribute name or nullptr.
const char* CompareVertex(const moderngekko::native_gx::VertexFormatInfo& f,
                          const moderngekko::native_gx::LoadedVertex& a,
                          const moderngekko::native_gx::LoadedVertex& b)
{
  if (f.position_matrix && a.position_matrix != b.position_matrix)
    return "posmtx";
  for (int c = 0; c < f.position_components; ++c)
    if (!SameFloat(a.position[c], b.position[c]))
      return "position";
  for (int n = 0; n < f.normal_count; ++n)
    for (int c = 0; c < 3; ++c)
      if (!SameFloat(a.normals[n][c], b.normals[n][c]))
        return "normal";
  for (int c = 0; c < 2; ++c)
    if (f.colors[c] && a.colors[c] != b.colors[c])
      return "color";
  for (int t = 0; t < 8; ++t)
    for (int c = 0; c < f.texcoord_components[t]; ++c)
      if (!SameFloat(a.texcoords[t][c], b.texcoords[t][c]))
        return "texcoord";
  return nullptr;
}

void CheckVertices(u8 vat, u16 count, const u8* data, bool loaded)
{
  (void)count;
  (void)data;
  if (s_reference_vertices.empty())
    return;
  ReferencePrimitive reference = std::move(s_reference_vertices.front());
  s_reference_vertices.pop_front();

  const auto& layout = Decoder().Layout();
  const auto format = moderngekko::native_gx::DescribeFormat(layout, vat);
  ++s_vertex_counters.primitives;
  s_vertex_counters.vertices += reference.vertices.size();

  const bool same_format =
      format.position_matrix == reference.format.position_matrix &&
      format.position_components == reference.format.position_components &&
      format.normal_count == reference.format.normal_count &&
      format.colors == reference.format.colors &&
      format.texcoord_components == reference.format.texcoord_components;
  const char* difference = nullptr;
  size_t vertex = 0;
  if (!loaded)
  {
    ++s_vertex_counters.load_failures;
    difference = "load";
  }
  else if (!same_format)
  {
    ++s_vertex_counters.format_mismatches;
    difference = "format";
  }
  else if (s_native_vertices.size() != reference.vertices.size())
  {
    ++s_vertex_counters.value_mismatches;
    difference = "count";
  }
  else
  {
    for (; vertex < s_native_vertices.size(); ++vertex)
    {
      difference = CompareVertex(format, reference.vertices[vertex], s_native_vertices[vertex]);
      if (difference)
      {
        ++s_vertex_counters.value_mismatches;
        break;
      }
    }
  }
  const u64 reported = s_vertex_counters.load_failures + s_vertex_counters.format_mismatches +
                       s_vertex_counters.value_mismatches;
  if (difference && reported <= kMaxReportedMismatches)
  {
    std::fprintf(stderr,
                 "[native_gx_verify] vertex mismatch (%s) vat=%u vcd=%08X/%08X vat_words=%08X/%08X/%08X "
                 "vertex=%zu dolphin_vertices=%zu native_vertices=%zu\n",
                 difference, vat, layout.vcd_low, layout.vcd_high, layout.vat_g0[vat & 7],
                 layout.vat_g1[vat & 7], layout.vat_g2[vat & 7], vertex, reference.vertices.size(),
                 s_native_vertices.size());
  }
}

u64 HashBytes(u64 hash, std::span<const u8> bytes)
{
  // 64-bit words with a multiply-xorshift mix; tail bytes one by one.
  size_t i = 0;
  for (; i + 8 <= bytes.size(); i += 8)
  {
    u64 word;
    std::memcpy(&word, bytes.data() + i, 8);
    hash = (hash ^ word) * 0x9E3779B97F4A7C15ULL;
    hash ^= hash >> 29;
  }
  for (; i < bytes.size(); ++i)
    hash = (hash ^ bytes[i]) * 1099511628211ULL;
  return hash;
}

// Content key of a texture: address, shape, all mip data and the palette.
// Returns false when the texture or its palette is not readable.
bool TextureKey(const moderngekko::native_gx::TextureUnit& unit, std::span<const u8>* data_out,
                std::span<const u8>* tlut_out, u64* key_out)
{
  namespace gx = moderngekko::native_gx;
  if (unit.from_tmem || !gx::IsValidTextureFormat(static_cast<u32>(unit.format)))
    return false;
  const u32 size = static_cast<u32>(unit.TotalSize());
  const std::span<const u8> data = GuestDisplayList(unit.address, size);
  const size_t palette_size = gx::PaletteSize(unit.format);
  const std::span<const u8> tlut = s_tmem.At(unit.tlut_tmem);
  if (data.size() < unit.TotalSize() || tlut.size() < palette_size)
    return false;
  u64 key = 14695981039346656037ULL;
  key = HashBytes(key, std::span<const u8>(reinterpret_cast<const u8*>(&unit.address), 4));
  const u32 shape[] = {u32(unit.width), u32(unit.height), u32(unit.format), u32(unit.tlut_format),
                       u32(unit.level_count)};
  key = HashBytes(key, std::span<const u8>(reinterpret_cast<const u8*>(shape), sizeof(shape)));
  key = HashBytes(key, data);
  if (palette_size)
    key = HashBytes(key, tlut.first(palette_size));
  *data_out = data;
  *tlut_out = tlut;
  *key_out = key;
  return true;
}

void CheckTexture(const moderngekko::native_gx::TextureUnit& unit)
{
  namespace gx = moderngekko::native_gx;
  if (unit.from_tmem)
  {
    ++s_texture_counters.preloaded;
    return;
  }
  if (!gx::IsValidTextureFormat(static_cast<u32>(unit.format)))
  {
    ++s_texture_counters.invalid;
    return;
  }
  const std::span<const u8> data = GuestDisplayList(unit.address, static_cast<u32>(unit.TotalSize()));
  const size_t palette_size = gx::PaletteSize(unit.format);
  const std::span<const u8> tlut = s_tmem.At(unit.tlut_tmem);
  if (data.size() < unit.TotalSize() || tlut.size() < palette_size)
  {
    ++s_texture_counters.invalid;
    return;
  }
  u64 key = 14695981039346656037ULL;
  key = HashBytes(key, std::span<const u8>(reinterpret_cast<const u8*>(&unit.address), 4));
  const u32 shape[] = {u32(unit.width), u32(unit.height), u32(unit.format), u32(unit.tlut_format),
                       u32(unit.level_count)};
  key = HashBytes(key, std::span<const u8>(reinterpret_cast<const u8*>(shape), sizeof(shape)));
  key = HashBytes(key, data);
  if (palette_size)
    key = HashBytes(key, tlut.first(palette_size));
  if (!s_checked_textures.insert(key).second)
    return;
  if (s_checked_textures.size() > 200000)
    s_checked_textures.clear();

  ++s_texture_counters.checked;
  for (int level = 0; level < unit.level_count; ++level)
  {
    const gx::TextureLevel& l = unit.levels[level];
    // Both decoders work on whole blocks.
    const int bw = gx::BlockWidth(unit.format), bh = gx::BlockHeight(unit.format);
    const int width = (l.width + bw - 1) / bw * bw, height = (l.height + bh - 1) / bh * bh;
    const size_t count = size_t(width) * height;
    s_native_texels.assign(count, 0);
    s_dolphin_texels.assign(count, 0);
    const std::span<const u8> level_data = data.subspan(l.offset, l.size);
    gx::DecodeTexture(level_data, width, height, unit.format, tlut, unit.tlut_format, s_native_texels.data());
    TexDecoder_Decode(reinterpret_cast<u8*>(s_dolphin_texels.data()), level_data.data(), width, height,
                      static_cast<TextureFormat>(unit.format), tlut.data(),
                      static_cast<TLUTFormat>(unit.tlut_format));
    s_texture_counters.texels += count;
    size_t first = 0;
    while (first < count && s_native_texels[first] == s_dolphin_texels[first])
      ++first;
    if (first != count)
    {
      if (++s_texture_counters.mismatches <= kMaxReportedMismatches)
      {
        std::fprintf(stderr,
                     "[native_gx_verify] texture mismatch address=%08X format=%u tlut_format=%u "
                     "size=%dx%d level=%d texel=%zu dolphin=%08X native=%08X\n",
                     unit.address, u32(unit.format), u32(unit.tlut_format), l.width, l.height, level,
                     first, s_dolphin_texels[first], s_native_texels[first]);
      }
      return;
    }
  }
}

void CheckTextures()
{
  namespace gx = moderngekko::native_gx;
  u32 units = 0;
  const int stages = gx::TevStageCount(s_bp);
  for (int stage = 0; stage < stages; ++stage)
  {
    const gx::TevStageTexture t = gx::ReadTevStageTexture(s_bp, stage);
    if (t.enabled)
      units |= 1u << t.texmap;
  }
  if (units == 0)
    return;
  ++s_texture_counters.draws_textured;
  for (int unit = 0; unit < 8; ++unit)
    if (units & (1u << unit))
      CheckTexture(gx::ReadTextureUnit(s_bp, unit));
}

void OnNativeEfbCopy(u32 copy_value);

bool RenderActive()
{
  static const bool active = [] {
    if (!s_render_dir || s_render_dir[0] == '\0')
      return false;
#ifdef _WIN32
    NativeGXBridge::SetBeforeEfbCopy(&OnNativeEfbCopy);
    if (const char* stride = std::getenv("MODERNGEKKO_NATIVE_GX_RENDER_STRIDE"))
      s_render.stride = std::max<u64>(1, std::strtoull(stride, nullptr, 10));
    s_render.csv = std::fopen((std::string(s_render_dir) + "/depth.csv").c_str(), "wb");
    if (s_render.csv)
      std::fputs("frame,pixels,exact,within_16,within_256,beyond_256,max_diff_z24,mean_diff_z24,"
                 "native_draws,color_within_2,color_mean_abs\n",
                 s_render.csv);
    std::fprintf(stderr, "[native_gx_render] stride=%llu\n",
                 static_cast<unsigned long long>(s_render.stride));
    s_render.active = true;
    return true;
#else
    std::fputs("[native_gx_render] disabled: D3D12 renderer is Windows-only\n", stderr);
    return false;
#endif
  }();
  return active;
}

// Frames compared: the real XFB copy that ends every stride-th frame.
bool SampledFrame(u64 frame)
{
  return frame > 0 && frame % s_render.stride == 0;
}

void SaveDepthImages(u64 frame)
{
  // Contrast-stretched depth of both buffers plus a difference mask (red:
  // differs by more than 256/2^24, green: native only, blue: Dolphin only).
  const size_t count = s_render.native_depth.size();
  float lo = 1.0f, hi = 0.0f;
  for (size_t i = 0; i < count; ++i)
  {
    for (float d : {s_render.native_depth[i], s_render.dolphin_depth[i]})
    {
      if (d > 0.0f)
      {
        lo = std::min(lo, d);
        hi = std::max(hi, d);
      }
    }
  }
  const float scale = hi > lo ? 255.0f / (hi - lo) : 0.0f;
  std::vector<u8> image(count * 4);
  const auto save = [&](const char* name) {
    const std::string path = std::string(s_render_dir) + "/frame-" + std::to_string(frame) + "-" + name + ".png";
    Common::SavePNG(path, image.data(), Common::ImageByteFormat::RGBA, moderngekko::native_gx::kEfbWidth,
                    moderngekko::native_gx::kEfbHeight, moderngekko::native_gx::kEfbWidth * 4);
  };
  for (const auto* depth : {&s_render.native_depth, &s_render.dolphin_depth})
  {
    for (size_t i = 0; i < count; ++i)
    {
      const float d = (*depth)[i];
      const u8 g = d > 0.0f ? static_cast<u8>(std::clamp((d - lo) * scale, 0.0f, 255.0f)) : 0;
      image[i * 4 + 0] = image[i * 4 + 1] = image[i * 4 + 2] = g;
      image[i * 4 + 3] = 255;
    }
    save(depth == &s_render.native_depth ? "native-depth" : "dolphin-depth");
  }
  for (size_t i = 0; i < count; ++i)
  {
    const float a = s_render.native_depth[i], b = s_render.dolphin_depth[i];
    const bool differs = std::fabs(a - b) * 16777216.0f > 256.0f;
    image[i * 4 + 0] = differs && a > 0.0f && b > 0.0f ? 255 : 0;
    image[i * 4 + 1] = differs && b == 0.0f ? 255 : 0;
    image[i * 4 + 2] = differs && a == 0.0f ? 255 : 0;
    image[i * 4 + 3] = 255;
  }
  save("difference");
  std::memcpy(image.data(), s_render.native_color.data(), count * 4);
  save("native-color");
  std::memcpy(image.data(), s_render.dolphin_color.data(), count * 4);
  save("dolphin-color");
}

void CompareDepth(u64 frame)
{
#ifdef _WIN32
  auto* renderer = NativeGXBridge::Renderer();
  if (!renderer || !renderer->ReadBack(&s_render.native_depth, &s_render.native_color))
    return;
  const size_t count = s_render.native_depth.size();
  u64 exact = 0, within16 = 0, within256 = 0, beyond = 0;
  double total = 0.0, max_diff = 0.0;
  for (size_t i = 0; i < count; ++i)
  {
    const double diff = std::fabs(double(s_render.native_depth[i]) - double(s_render.dolphin_depth[i])) * 16777216.0;
    total += diff;
    max_diff = std::max(max_diff, diff);
    if (diff <= 1.0)
      ++exact;
    else if (diff <= 16.0)
      ++within16;
    else if (diff <= 256.0)
      ++within256;
    else
      ++beyond;
  }
  // Colour: RGB within 2/255 per channel (the EFB alpha has 6 bits on both sides).
  u64 color_close = 0;
  double color_error = 0.0;
  for (size_t i = 0; i < count; ++i)
  {
    const u32 a = s_render.native_color[i], b = s_render.dolphin_color[i];
    int worst = 0;
    for (int c = 0; c < 3; ++c)
    {
      const int d = std::abs(int((a >> (8 * c)) & 0xFF) - int((b >> (8 * c)) & 0xFF));
      worst = std::max(worst, d);
      color_error += d;
    }
    color_close += worst <= 2;
  }
  ++s_render.samples;
  std::fprintf(stderr, "[native_gx_render] frame=%llu color within_2=%.2f%% mean_abs=%.3f shaders=%llu\n",
               static_cast<unsigned long long>(frame), 100.0 * color_close / count, color_error / (3.0 * count),
               static_cast<unsigned long long>(renderer->ShaderCount()));
  if (s_render.csv)
  {
    std::fprintf(s_render.csv, "%llu,%zu,%llu,%llu,%llu,%llu,%.0f,%.3f,%llu,%llu,%.4f\n",
                 static_cast<unsigned long long>(frame), count, static_cast<unsigned long long>(exact),
                 static_cast<unsigned long long>(within16), static_cast<unsigned long long>(within256),
                 static_cast<unsigned long long>(beyond), max_diff, total / double(count),
                 static_cast<unsigned long long>(renderer->DrawCount()),
                 static_cast<unsigned long long>(color_close), color_error / (3.0 * count));
    std::fflush(s_render.csv);
  }
  std::fprintf(stderr,
               "[native_gx_render] frame=%llu depth exact=%.2f%% within_256=%.2f%% beyond=%.2f%%\n",
               static_cast<unsigned long long>(frame), 100.0 * exact / count,
               100.0 * (exact + within16 + within256) / count, 100.0 * beyond / count);
  if (s_render.images < 40)
  {
    ++s_render.images;
    SaveDepthImages(frame);
  }
#else
  (void)frame;
#endif
}

// The native front end is about to execute an EFB copy: at sampled real XFB
// copies, compare its EFB with Dolphin's (both before the copy's clear).
void OnNativeEfbCopy(u32 copy_value)
{
  if (!moderngekko::native_gx::IsXfbCopy(copy_value))
    return;
  ++s_render.frame;
  if (SampledFrame(s_render.frame) && s_render.dolphin_ready)
    CompareDepth(s_render.frame);
  s_render.dolphin_ready = false;
}

// Dolphin's EFB depth just before a sampled real XFB copy executes.
void CaptureDolphinDepth(u32 copy_value)
{
  if (!RenderActive() || !moderngekko::native_gx::IsXfbCopy(copy_value))
    return;
  // The native sink counts this copy after Dolphin executes the command.
  if (!SampledFrame(s_render.frame + 1))
    return;
  g_vertex_manager->Flush();
  g_framebuffer_manager->InvalidatePeekCache(true);
  const int w = moderngekko::native_gx::kEfbWidth, h = moderngekko::native_gx::kEfbHeight;
  s_render.dolphin_depth.resize(size_t(w) * h);
  s_render.dolphin_color.resize(size_t(w) * h);
  for (int y = 0; y < h; ++y)
  {
    for (int x = 0; x < w; ++x)
    {
      s_render.dolphin_depth[size_t(y) * w + x] = g_framebuffer_manager->PeekEFBDepth(x, y);
      // The EFB alpha channel is not meaningful for display.
      s_render.dolphin_color[size_t(y) * w + x] = g_framebuffer_manager->PeekEFBColor(x, y) | 0xFF000000u;
    }
  }
  s_render.dolphin_ready = true;
}

class NativeSink final : public moderngekko::native_gx::CommandSink
{
public:
  void OnNop(u32 count) override { s_native.push_back({Kind::Nop, count, 0, 0, 0}); }
  void OnCp(u8 reg, u32 value) override { s_native.push_back({Kind::Cp, reg, value, 0, 0}); }
  void OnXf(u16 address, u8 count, const u8* data) override
  {
    s_native.push_back({Kind::Xf, address, count, 0, Hash(data, size_t(count) * 4)});
    s_xf.Load(address, count, data);
  }
  void OnIndexedXf(u8 array, u32 index, u16 address, u8 size) override
  {
    s_native.push_back({Kind::Indexed, array, index, (u32(address) << 8) | size, 0});
    if (!s_xf.LoadIndexed(Decoder().Layout(), &GuestDisplayList, array, index, address, size))
      ++s_state_counters.indexed_failures;
  }
  void OnBp(u8 reg, u32 value) override
  {
    s_native.push_back({Kind::Bp, reg, value, 0, 0});
    s_bp.Load(reg, value);
    if (s_tmem.OnBpWrite(s_bp, reg, &GuestDisplayList))
      ++s_texture_counters.tlut_loads;
  }
  void OnPrimitive(moderngekko::native_gx::Primitive primitive, u8 vat, u32 vertex_size, u16 count,
                   const u8* vertices) override
  {
    s_native.push_back({Kind::Primitive, (u32(primitive) << 8) | vat, vertex_size, count,
                        Hash(vertices, size_t(vertex_size) * count)});
    s_native_vertices.clear();
    const bool loaded =
        NativeVertexLoader().Load(Decoder().Layout(), vat, vertices, count, s_native_vertices);
    CheckVertices(vat, count, vertices, loaded);
    CheckTextures();
  }
  void OnDisplayList(u32 address, u32 size, bool nested) override
  {
    s_native.push_back({Kind::DisplayList, address, size, nested ? 1u : 0u, 0});
  }
  void OnUnknown(u8 opcode) override { s_native.push_back({Kind::Unknown, opcode, 0, 0, 0}); }
};

std::span<const u8> GuestDisplayList(u32 address, u32 size)
{
  auto& memory = Core::System::GetInstance().GetMemory();
  const u8* data = memory.GetPointerForRange(address, size);
  return data ? std::span<const u8>(data, size) : std::span<const u8>{};
}

moderngekko::native_gx::CommandDecoder& Decoder()
{
  static moderngekko::native_gx::CommandDecoder decoder(&GuestDisplayList);
  return decoder;
}

// A decoder starts from the command processor state at its first command
// (boot or a loaded checkpoint), like loading a savestate.
void SeedLayout(moderngekko::native_gx::CommandDecoder& decoder)
{
  auto& layout = decoder.Layout();
  const CPState& cp = g_main_cp_state;
  layout.vcd_low = cp.vtx_desc.low.Hex;
  layout.vcd_high = cp.vtx_desc.high.Hex;
  for (int i = 0; i < CP_NUM_VAT_REG; ++i)
  {
    layout.vat_g0[i] = cp.vtx_attr[i].g0.Hex;
    layout.vat_g1[i] = cp.vtx_attr[i].g1.Hex;
    layout.vat_g2[i] = cp.vtx_attr[i].g2.Hex;
  }
  layout.matrix_index_a = cp.matrix_index_a.Hex;
  layout.matrix_index_b = cp.matrix_index_b.Hex;
}

// Per-frame digest of the top-level command stream (frame = up to and
// including the command that performs the real XFB copy). NOP runs are hashed
// but not counted: their split depends on how much data was available.
struct FrameDigest
{
  u64 hash = 14695981039346656037ULL;
  u64 bytes = 0;
  u64 commands = 0;
  u64 primitives = 0;
  u64 vertices = 0;

  void AddBytes(const u8* data, size_t size)
  {
    for (size_t i = 0; i < size; ++i)
      hash = (hash ^ data[i]) * 1099511628211ULL;
    bytes += size;
    commands += data[0] != 0;
  }
  bool operator==(const FrameDigest&) const = default;
};

// Native command source: the bytes the CPU commits to the GPU FIFO through
// the write-gather pipe (CPU thread), cut into commands and frames by the
// native decoder. Its frames must equal what Dolphin's GPU side consumed.
std::mutex s_stream_mutex;
std::deque<FrameDigest> s_stream_frames;
std::vector<u8> s_stream_pending;
FrameDigest s_stream_current;
bool s_stream_synced = false;
bool s_stream_seeded = false;

class StreamSink final : public moderngekko::native_gx::CommandSink
{
public:
  u64 primitives = 0;
  u64 vertices = 0;
  bool xfb_copy = false;

  void OnCp(u8, u32) override {}
  void OnXf(u16, u8, const u8*) override {}
  void OnIndexedXf(u8, u32, u16, u8) override {}
  void OnBp(u8 reg, u32 value) override
  {
    if (reg == 0x52 && (value & (1u << 14)) != 0)  // EFB copy with copy_to_xfb
      xfb_copy = true;
  }
  void OnPrimitive(moderngekko::native_gx::Primitive, u8, u32, u16 count, const u8*) override
  {
    ++primitives;
    vertices += count;
  }
  void OnDisplayList(u32, u32, bool) override {}
  void OnUnknown(u8) override {}
};

moderngekko::native_gx::CommandDecoder& StreamDecoder()
{
  static moderngekko::native_gx::CommandDecoder decoder(&GuestDisplayList);
  return decoder;
}

void OnRawFifoBytes(const u8* data, size_t size)
{
  auto& decoder = StreamDecoder();
  if (!s_stream_seeded)
  {
    SeedLayout(decoder);
    s_stream_seeded = true;
  }
  s_stream_pending.insert(s_stream_pending.end(), data, data + size);
  size_t offset = 0;
  while (offset < s_stream_pending.size())
  {
    StreamSink sink;
    const u8* command = s_stream_pending.data() + offset;
    const size_t used = decoder.DecodeOne(
        std::span<const u8>(command, s_stream_pending.size() - offset), sink);
    if (used == 0)
      break;
    offset += used;
    if (s_stream_synced)
    {
      s_stream_current.AddBytes(command, used);
      s_stream_current.primitives += sink.primitives;
      s_stream_current.vertices += sink.vertices;
    }
    if (sink.xfb_copy)
    {
      if (s_stream_synced)
      {
        std::lock_guard lock(s_stream_mutex);
        s_stream_frames.push_back(s_stream_current);
      }
      s_stream_current = {};
      s_stream_synced = true;
    }
  }
  s_stream_pending.erase(s_stream_pending.begin(), s_stream_pending.begin() + offset);
}

const bool s_stream_registered = [] {
  if (s_enabled)
    GPFifo::SetRawFifoObserver(&OnRawFifoBytes);
  return s_enabled;
}();

// Dolphin side of the same digest (GPU thread).
FrameDigest s_gpu_current;
bool s_gpu_synced = false;
bool s_gpu_frame_end = false;

struct StreamCounters
{
  u64 frames = 0;
  u64 mismatches = 0;
  u64 missing = 0;
} s_stream_counters;

void CompareStreamFrame(const FrameDigest& dolphin)
{
  FrameDigest native;
  {
    std::lock_guard lock(s_stream_mutex);
    if (s_stream_frames.empty())
    {
      ++s_stream_counters.missing;
      return;
    }
    native = s_stream_frames.front();
    s_stream_frames.pop_front();
  }
  ++s_stream_counters.frames;
  if (native == dolphin)
    return;
  if (++s_stream_counters.mismatches <= kMaxReportedMismatches)
  {
    std::fprintf(stderr,
                 "[native_gx_verify] stream mismatch frame=%llu dolphin bytes=%llu commands=%llu "
                 "prims=%llu vertices=%llu hash=%016llX native bytes=%llu commands=%llu prims=%llu "
                 "vertices=%llu hash=%016llX\n",
                 static_cast<unsigned long long>(s_counters.frames),
                 static_cast<unsigned long long>(dolphin.bytes),
                 static_cast<unsigned long long>(dolphin.commands),
                 static_cast<unsigned long long>(dolphin.primitives),
                 static_cast<unsigned long long>(dolphin.vertices),
                 static_cast<unsigned long long>(dolphin.hash),
                 static_cast<unsigned long long>(native.bytes),
                 static_cast<unsigned long long>(native.commands),
                 static_cast<unsigned long long>(native.primitives),
                 static_cast<unsigned long long>(native.vertices),
                 static_cast<unsigned long long>(native.hash));
  }
}

void CompareRegisterFiles()
{
  ++s_state_counters.frames;
  const auto tmem = s_tmem.Bytes();
  if (std::memcmp(tmem.data(), s_tex_mem.data(), tmem.size()) != 0)
  {
    size_t first = 0;
    while (tmem[first] == s_tex_mem[first])
      ++first;
    if (++s_texture_counters.tmem_mismatch_frames <= kMaxReportedMismatches)
      std::fprintf(stderr, "[native_gx_verify] tmem mismatch frame=%llu offset=%05zX\n",
                   static_cast<unsigned long long>(s_counters.frames), first);
  }
  static_assert(sizeof(XFMemory) == sizeof(u32) * moderngekko::native_gx::XfMemory::kSize);
  const u32* xf = reinterpret_cast<const u32*>(&xfmem);
  const u32* bp = reinterpret_cast<const u32*>(&bpmem);
  u32 xf_first = ~0u;
  u32 bp_first = ~0u;
  for (u32 a = 0; a < s_xf.words.size() && xf_first == ~0u; ++a)
    if (s_xf.words[a] != xf[a])
      xf_first = a;
  for (u32 r = 0; r < s_bp.regs.size() && bp_first == ~0u; ++r)
    if (s_bp.regs[r] != bp[r])
      bp_first = r;
  if (xf_first != ~0u && ++s_state_counters.xf_mismatches <= kMaxReportedMismatches)
  {
    std::fprintf(stderr, "[native_gx_verify] xf mismatch frame=%llu address=%04X dolphin=%08X native=%08X\n",
                 static_cast<unsigned long long>(s_counters.frames), xf_first, xf[xf_first],
                 s_xf.words[xf_first]);
  }
  if (bp_first != ~0u && ++s_state_counters.bp_mismatches <= kMaxReportedMismatches)
  {
    std::fprintf(stderr, "[native_gx_verify] bp mismatch frame=%llu reg=%02X dolphin=%08X native=%08X\n",
                 static_cast<unsigned long long>(s_counters.frames), bp_first, bp[bp_first],
                 s_bp.regs[bp_first]);
  }
}

bool Recording()
{
  return s_enabled && !FrameInterpolation::IsReplaying();
}

const char* KindName(Kind kind)
{
  static constexpr const char* kNames[] = {"cp", "xf", "indexed", "bp", "prim", "dl", "nop", "unknown"};
  return kNames[static_cast<u8>(kind)];
}

void PrintEvent(const char* side, size_t index, const std::vector<Event>& events)
{
  if (index >= events.size())
  {
    std::fprintf(stderr, "[native_gx_verify]   %s[%zu] = <none>\n", side, index);
    return;
  }
  const Event& e = events[index];
  std::fprintf(stderr, "[native_gx_verify]   %s[%zu] = %s a=%08X b=%08X c=%08X hash=%016llX\n",
               side, index, KindName(e.kind), e.a, e.b, e.c, static_cast<unsigned long long>(e.hash));
}

void PrintSummary()
{
  std::fprintf(stderr,
               "[native_gx_verify] frames=%llu commands=%llu events=%llu primitives=%llu "
               "display_lists=%llu mismatches=%llu size_mismatches=%llu stream_frames=%llu "
               "stream_mismatches=%llu stream_missing=%llu vertex_primitives=%llu vertices=%llu "
               "vertex_format_mismatches=%llu vertex_value_mismatches=%llu vertex_load_failures=%llu "
               "state_frames=%llu xf_mismatches=%llu bp_mismatches=%llu indexed_failures=%llu "
               "textured_draws=%llu textures_checked=%llu texels=%llu texture_mismatches=%llu "
               "texture_invalid=%llu texture_preloaded=%llu tlut_loads=%llu tmem_mismatch_frames=%llu\n",
               static_cast<unsigned long long>(s_counters.frames),
               static_cast<unsigned long long>(s_counters.commands),
               static_cast<unsigned long long>(s_counters.events),
               static_cast<unsigned long long>(s_counters.primitives),
               static_cast<unsigned long long>(s_counters.display_lists),
               static_cast<unsigned long long>(s_counters.mismatches),
               static_cast<unsigned long long>(s_counters.size_mismatches),
               static_cast<unsigned long long>(s_stream_counters.frames),
               static_cast<unsigned long long>(s_stream_counters.mismatches),
               static_cast<unsigned long long>(s_stream_counters.missing),
               static_cast<unsigned long long>(s_vertex_counters.primitives),
               static_cast<unsigned long long>(s_vertex_counters.vertices),
               static_cast<unsigned long long>(s_vertex_counters.format_mismatches),
               static_cast<unsigned long long>(s_vertex_counters.value_mismatches),
               static_cast<unsigned long long>(s_vertex_counters.load_failures),
               static_cast<unsigned long long>(s_state_counters.frames),
               static_cast<unsigned long long>(s_state_counters.xf_mismatches),
               static_cast<unsigned long long>(s_state_counters.bp_mismatches),
               static_cast<unsigned long long>(s_state_counters.indexed_failures),
               static_cast<unsigned long long>(s_texture_counters.draws_textured),
               static_cast<unsigned long long>(s_texture_counters.checked),
               static_cast<unsigned long long>(s_texture_counters.texels),
               static_cast<unsigned long long>(s_texture_counters.mismatches),
               static_cast<unsigned long long>(s_texture_counters.invalid),
               static_cast<unsigned long long>(s_texture_counters.preloaded),
               static_cast<unsigned long long>(s_texture_counters.tlut_loads),
               static_cast<unsigned long long>(s_texture_counters.tmem_mismatch_frames));
  std::fflush(stderr);
}
}  // namespace

bool IsEnabled()
{
  return s_enabled;
}

void RefCp(u8 reg, u32 value)
{
  if (Recording())
    s_reference.push_back({Kind::Cp, reg, value, 0, 0});
}

void RefXf(u16 address, u8 count, const u8* data)
{
  if (Recording())
    s_reference.push_back({Kind::Xf, address, count, 0, Hash(data, size_t(count) * 4)});
}

void RefIndexed(u8 array, u32 index, u16 address, u8 size)
{
  if (Recording())
    s_reference.push_back({Kind::Indexed, array, index, (u32(address) << 8) | size, 0});
}

void RefBp(u8 reg, u32 value)
{
  if (!Recording())
    return;
  s_reference.push_back({Kind::Bp, reg, value, 0, 0});
  if (reg == 0x52)
    CaptureDolphinDepth(value);
}

void RefPrimitive(u8 primitive, u8 vat, u32 vertex_size, u16 count, const u8* data)
{
  if (Recording())
  {
    s_reference.push_back({Kind::Primitive, (u32(primitive) << 8) | vat, vertex_size, count,
                           Hash(data, size_t(vertex_size) * count)});
  }
}

void RefPrimitiveVertices(u8 vat, u16 count, const u8* data)
{
  if (!Recording())
    return;
  VertexLoaderBase* loader = VertexLoaderManager::RefreshLoader<false>(vat);
  const PortableVertexDeclaration& decl = loader->m_native_vtx_decl;
  s_vertex_scratch.resize(size_t(decl.stride) * count + 64);
  const int loaded = loader->RunVertices(data, s_vertex_scratch.data(), count);

  ReferencePrimitive reference;
  auto& f = reference.format;
  f.position_matrix = decl.posmtx.enable;
  f.position_components = static_cast<u8>(decl.position.components);
  f.normal_count = decl.normals[0].enable ? (decl.normals[2].enable ? 3 : 1) : 0;
  for (int c = 0; c < 2; ++c)
    f.colors[c] = decl.colors[c].enable;
  for (int t = 0; t < 8; ++t)
    f.texcoord_components[t] = decl.texcoords[t].enable ? static_cast<u8>(decl.texcoords[t].components) : 0;

  reference.vertices.resize(loaded > 0 ? loaded : 0);
  for (size_t i = 0; i < reference.vertices.size(); ++i)
  {
    const u8* v = s_vertex_scratch.data() + i * decl.stride;
    auto& out = reference.vertices[i];
    const auto floats = [&](const AttributeFormat& a, float* dst, int n) {
      std::memcpy(dst, v + a.offset, sizeof(float) * n);
    };
    if (decl.posmtx.enable)
      out.position_matrix = v[decl.posmtx.offset];
    floats(decl.position, out.position.data(), decl.position.components);
    for (int n = 0; n < f.normal_count; ++n)
      floats(decl.normals[n], out.normals[n].data(), 3);
    for (int c = 0; c < 2; ++c)
      if (decl.colors[c].enable)
        std::memcpy(out.colors[c].data(), v + decl.colors[c].offset, 4);
    for (int t = 0; t < 8; ++t)
      if (decl.texcoords[t].enable)
        floats(decl.texcoords[t], out.texcoords[t].data(), decl.texcoords[t].components);
  }
  s_reference_vertices.push_back(std::move(reference));
}

void RefDisplayList(u32 address, u32 size, bool nested)
{
  if (Recording())
    s_reference.push_back({Kind::DisplayList, address, size, nested ? 1u : 0u, 0});
}

void RefNop(u32 count)
{
  if (Recording())
    s_reference.push_back({Kind::Nop, count, 0, 0, 0});
}

void RefUnknown(u8 opcode)
{
  if (Recording())
    s_reference.push_back({Kind::Unknown, opcode, 0, 0, 0});
}

void OnTopLevelCommand(const u8* data, u32 size)
{
  // Interpolation replays run nested inside the real XFB copy command: leave
  // that command's reference events in place.
  if (!Recording())
    return;
  static const bool seeded = [] {
    SeedLayout(Decoder());
    std::memcpy(s_xf.words.data(), &xfmem, sizeof(u32) * s_xf.words.size());
    std::memcpy(s_bp.regs.data(), &bpmem, sizeof(u32) * s_bp.regs.size());
    std::memcpy(s_tmem.Bytes().data(), s_tex_mem.data(), s_tmem.Bytes().size());

    return true;
  }();
  (void)seeded;
  NativeSink sink;
  s_native.clear();
  const size_t used = Decoder().DecodeOne(std::span<const u8>(data, size), sink);

  ++s_counters.commands;
  s_counters.events += s_reference.size();
  for (const Event& e : s_reference)
  {
    s_counters.primitives += e.kind == Kind::Primitive;
    s_counters.display_lists += e.kind == Kind::DisplayList;
  }
  const bool size_ok = used == size;
  if (!size_ok)
    ++s_counters.size_mismatches;
  if (!size_ok || s_native != s_reference)
  {
    if (++s_counters.mismatches <= kMaxReportedMismatches)
    {
      size_t first = 0;
      while (first < s_reference.size() && first < s_native.size() &&
             s_reference[first] == s_native[first])
        ++first;
      std::fprintf(stderr,
                   "[native_gx_verify] mismatch frame=%llu opcode=%02X size=%u native_size=%zu "
                   "dolphin_events=%zu native_events=%zu first_difference=%zu\n",
                   static_cast<unsigned long long>(s_counters.frames), data[0], size, used,
                   s_reference.size(), s_native.size(), first);
      PrintEvent("dolphin", first, s_reference);
      PrintEvent("native", first, s_native);
    }
  }
  if (s_gpu_synced)
  {
    s_gpu_current.AddBytes(data, size);
    for (const Event& e : s_reference)
    {
      if (e.kind == Kind::Primitive)
      {
        ++s_gpu_current.primitives;
        s_gpu_current.vertices += e.c;
      }
    }
  }
  if (s_gpu_frame_end)
  {
    CompareRegisterFiles();
    if (s_gpu_synced)
      CompareStreamFrame(s_gpu_current);
    s_gpu_current = {};
    s_gpu_synced = true;
    s_gpu_frame_end = false;
  }
  s_reference.clear();
  s_reference_vertices.clear();
}

void OnXfbCopy()
{
  if (!Recording())
    return;
  s_gpu_frame_end = true;
  ++s_counters.frames;
  if (s_counters.frames % 300 == 0)
    PrintSummary();
}
}  // namespace NativeGXVerify
