#include "VideoCommon/NativeGXBridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "Common/Image.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/VideoEvents.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/FrameInterpolation.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/XFMemory.h"

#ifdef _WIN32
#include "moderngekko/native_gx/d3d12_renderer.hpp"
#include "moderngekko/native_gx/frontend.hpp"
#endif

namespace NativeGXBridge
{
namespace
{
const bool s_offscreen = [] {
  const char* dir = std::getenv("MODERNGEKKO_NATIVE_GX_RENDER");
  return dir != nullptr && dir[0] != '\0';
}();

void* s_window = nullptr;
bool s_present = false;
bool s_interpolate = false;
int s_efb_scale = 1;
const bool s_dolphin_decode = [] {
  const char* v = std::getenv("MODERNGEKKO_NATIVE_GX_DOLPHIN_DECODE");
  return v != nullptr && v[0] == '1';
}();
u64 s_last_xfb_ticks = 0;
std::function<void(u32)> s_before_efb_copy;
bool s_init_attempted = false;
u64 s_size_mismatches = 0;
std::atomic<double> s_presented_fps{0.0};
std::deque<std::chrono::steady_clock::time_point> s_present_times;

#ifdef _WIN32
std::unique_ptr<moderngekko::native_gx::D3D12Renderer> s_renderer;
std::unique_ptr<moderngekko::native_gx::Frontend> s_frontend;

std::span<const u8> GuestMemory(u32 address, u32 size)
{
  auto& memory = Core::System::GetInstance().GetMemory();
  const u8* data = memory.GetPointerForRange(address, size);
  return data ? std::span<const u8>(data, size) : std::span<const u8>{};
}

// The front end starts from Dolphin's GX state at the first command (boot or
// a loaded savestate).
moderngekko::native_gx::FrontendSeed CurrentState()
{
  moderngekko::native_gx::FrontendSeed seed;
  const CPState& cp = g_main_cp_state;
  seed.layout.vcd_low = cp.vtx_desc.low.Hex;
  seed.layout.vcd_high = cp.vtx_desc.high.Hex;
  for (int i = 0; i < CP_NUM_VAT_REG; ++i)
  {
    seed.layout.vat_g0[i] = cp.vtx_attr[i].g0.Hex;
    seed.layout.vat_g1[i] = cp.vtx_attr[i].g1.Hex;
    seed.layout.vat_g2[i] = cp.vtx_attr[i].g2.Hex;
  }
  for (int i = 0; i < CP_NUM_ARRAYS; ++i)
  {
    seed.layout.array_base[i] = cp.array_bases[static_cast<CPArray>(i)];
    seed.layout.array_stride[i] = cp.array_strides[static_cast<CPArray>(i)];
  }
  seed.layout.matrix_index_a = cp.matrix_index_a.Hex;
  seed.layout.matrix_index_b = cp.matrix_index_b.Hex;
  std::memcpy(seed.xf.data(), &xfmem, sizeof(u32) * seed.xf.size());
  std::memcpy(seed.bp.data(), &bpmem, sizeof(u32) * seed.bp.size());
  seed.tmem = std::span<const u8>(s_tex_mem.data(), s_tex_mem.size());
  const auto& ps = Core::System::GetInstance().GetPixelShaderManager().constants;
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 4; ++c)
    {
      seed.tev.colors[i][c] = ps.colors[i][c];
      seed.tev.konst[i][c] = ps.kcolors[i][c];
    }
  for (int c = 0; c < 3; ++c)
  {
    seed.cached_normal[0][c] = VertexLoaderManager::normal_cache[c];
    seed.cached_normal[1][c] = VertexLoaderManager::tangent_cache[c];
    seed.cached_normal[2][c] = VertexLoaderManager::binormal_cache[c];
  }
  return seed;
}

// Diagnostics: MODERNGEKKO_NATIVE_GX_PRESENTS=<csv> logs every present
// (frame, image 0 real / 1 interpolated, host time after Present returned);
// MODERNGEKKO_NATIVE_GX_DUMP=<dir> saves, every 120 frames, the previous real
// image, the interpolated one and the real one (reading back stalls the GPU).
// Presentation runs on the renderer's presentation thread unless
// MODERNGEKKO_NATIVE_GX_PRESENT_THREAD=0 (presents inline, the old path).
const bool s_present_thread = [] {
  const char* v = std::getenv("MODERNGEKKO_NATIVE_GX_PRESENT_THREAD");
  return v == nullptr || v[0] != '0';
}();
const bool s_vsync = [] {
  const char* v = std::getenv("MODERNGEKKO_NATIVE_GX_VSYNC");
  return v == nullptr || v[0] != '0';
}();
std::atomic<u64> s_frame_number{0};
std::mutex s_present_times_mutex;

FILE* PresentLog()
{
  static FILE* file = [] {
    const char* path = std::getenv("MODERNGEKKO_NATIVE_GX_PRESENTS");
    FILE* f = path ? std::fopen(path, "wb") : nullptr;
    if (f)
      std::fputs("frame,image,host_ms,present_count,refresh_count,sync_qpc\n", f);
    return f;
  }();
  return file;
}

// Presented images in the last second, for the window title.
void CountPresented(std::chrono::steady_clock::time_point now)
{
  std::lock_guard lock(s_present_times_mutex);
  s_present_times.push_back(now);
  while (now - s_present_times.front() > std::chrono::seconds(1))
    s_present_times.pop_front();
  s_presented_fps.store(static_cast<double>(s_present_times.size()), std::memory_order_relaxed);
}

// Presentation thread: one call per image shown.
void OnPresented(const moderngekko::native_gx::D3D12Renderer::PresentInfo& info)
{
  CountPresented(std::chrono::steady_clock::now());
  if (FILE* file = PresentLog())
  {
    const u64 frame = s_frame_number.load(std::memory_order_relaxed);
    std::fprintf(file, "%llu,%d,%.3f,%u,%u,%lld\n", static_cast<unsigned long long>(frame), info.image,
                 info.host_ms, info.present_count, info.refresh_count, static_cast<long long>(info.sync_qpc));
    if (frame % 60 == 0)
      std::fflush(file);
  }
}

void RecordPresent(int image)
{
  FILE* file = PresentLog();
  if (!file)
    return;
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  std::uint32_t presents = 0, refresh = 0;
  std::int64_t qpc = 0;
  s_renderer->PresentStatistics(&presents, &refresh, &qpc);
  std::fprintf(file, "%llu,%d,%.3f,%u,%u,%lld\n", static_cast<unsigned long long>(s_frontend->Stats().frames), image,
               ms, presents, refresh, static_cast<long long>(qpc));
  if (s_frontend->Stats().frames % 60 == 0)
    std::fflush(file);
}

void SaveXfb(int image, const std::string& name)
{
  std::vector<u32> pixels;
  int width = 0, height = 0;
  if (!s_renderer->ReadXfb(image, &pixels, &width, &height))
    return;
  for (u32& p : pixels)
    p |= 0xFF000000u;
  Common::SavePNG(name, reinterpret_cast<const u8*>(pixels.data()), Common::ImageByteFormat::RGBA, width, height,
                  width * 4);
}

void DumpImages(bool extra)
{
  static const char* dir = std::getenv("MODERNGEKKO_NATIVE_GX_DUMP");
  if (!dir)
    return;
  static const u64 every = [] {
    const char* v = std::getenv("MODERNGEKKO_NATIVE_GX_DUMP_EVERY");
    const long long n = v ? std::atoll(v) : 0;
    return n >= 2 ? u64(n) : u64(120);
  }();
  const u64 frame = s_frontend->Stats().frames;
  const std::string base = std::string(dir) + "/frame-";
  if ((frame + 1) % every == 0)
    SaveXfb(0, base + std::to_string(frame + 1) + "-a-previous.png");
  if (frame % every == 0 && frame > 0)
  {
    if (extra)
      SaveXfb(1, base + std::to_string(frame) + "-b-interpolated.png");
    SaveXfb(0, base + std::to_string(frame) + "-c-current.png");
  }
  // MODERNGEKKO_NATIVE_GX_BISECT=<frame>,<first>,<last>,<parts>: that frame's
  // interpolated image again with only draws of each part of [first, last)
  // interpolated (frame-F-bisect-<from>-<to>.png).
  static const char* bisect = std::getenv("MODERNGEKKO_NATIVE_GX_BISECT");
  unsigned long long at = 0, first = 0, last = 0, parts = 0;
  if (extra && bisect && std::sscanf(bisect, "%llu,%llu,%llu,%llu", &at, &first, &last, &parts) == 4 &&
      at == frame && parts > 0 && last > first)
  {
    last = std::min<unsigned long long>(last, s_frontend->RecordedDrawCount());
    for (unsigned long long k = 0; k < parts; ++k)
    {
      const unsigned long long from = first + (last - first) * k / parts;
      const unsigned long long to = first + (last - first) * (k + 1) / parts;
      s_frontend->SetInterpolationRange(from, to);
      s_frontend->RenderInterpolated(0.5f, 1);
      SaveXfb(1, base + std::to_string(frame) + "-bisect-" + std::to_string(from) + "-" + std::to_string(to) +
                     ".png");
    }
    s_frontend->SetInterpolationRange(0, static_cast<std::size_t>(-1));
  }
}

// Copy destinations get what Dolphin's texture cache wrote without copies to
// RAM (TextureCacheBase::UninitializeEFBMemory / UninitializeXFBMemory).
void MarkCopyMemory(u32 address, u32 stride, u32 bytes_per_row, u32 rows, bool xfb)
{
  auto& memory = Core::System::GetInstance().GetMemory();
  for (u32 row = 0; row < rows; ++row)
  {
    u8* data = memory.GetPointerForRange(address + row * stride, bytes_per_row);
    if (!data)
      return;
    if (!xfb)
    {
      std::memset(data, 0, bytes_per_row);
      continue;
    }
    for (u32 i = 0; i + 1 < bytes_per_row; i += 2)
    {
      data[i] = 1;  // Y=1, U/V=254: fuchsia
      data[i + 1] = 254;
    }
  }
}

void OnPeSync(u8 reg, u32 value, u32 cycles)
{
  auto& pixel_engine = Core::System::GetInstance().GetPixelEngine();
  if (reg == 0x45)
    pixel_engine.SetFinish(static_cast<int>(cycles));
  else
    pixel_engine.SetToken(static_cast<u16>(value & 0xFFFF), reg == 0x48, static_cast<int>(cycles));
}

void OnFrame(const moderngekko::native_gx::XfbCopyParams&)
{
  if (OwnsGpu())
  {
    auto& system = Core::System::GetInstance();
    system.GetVideoEvents().after_frame_event.Trigger(system);
  }
  // Game frames lasting two VI fields (30 Hz) get an extra, interpolated image
  // shown on the first field; 60 Hz scenes are presented as they are.
  auto& system = Core::System::GetInstance();
  const u64 ticks = system.GetCoreTiming().GetTicks();
  const u64 field = system.GetVideoInterface().GetTicksPerField();
  const bool two_fields = s_last_xfb_ticks != 0 && field != 0 && ticks - s_last_xfb_ticks >= field * 3 / 2;
  s_last_xfb_ticks = ticks;
  const bool extra = s_interpolate && two_fields && s_frontend->RenderInterpolated(0.5f, 1);
  s_frame_number.store(s_frontend->Stats().frames, std::memory_order_relaxed);
  if (s_renderer->HasWindow())
  {
    // With vsync each image holds one refresh: two images per 30 Hz frame.
    if (s_present_thread)
    {
      // Queued: the game never waits for the display.
      if (extra)
        s_renderer->QueuePresentation(1);
      s_renderer->QueuePresentation(0);
    }
    else
    {
      if (extra)
      {
        s_renderer->Present(s_vsync, 1);
        RecordPresent(1);
        CountPresented(std::chrono::steady_clock::now());
      }
      s_renderer->Present(s_vsync, 0);
      RecordPresent(0);
      CountPresented(std::chrono::steady_clock::now());
    }
  }
  DumpImages(extra);
  s_renderer->EndFrame();
  const auto& stats = s_frontend->Stats();
  if (stats.frames % 600 == 0)
  {
    std::fprintf(stderr,
                 "[native_gx] frames=%llu primitives=%llu efb_copies=%llu copy_reads=%llu copy_fallbacks=%llu "
                 "palettes=%llu textures_decoded=%llu shaders=%llu size_mismatches=%llu interpolated_frames=%llu "
                 "interpolated_draws=%llu camera_draws=%llu still_draws=%llu dropped_presentations=%llu\n",
                 static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.primitives),
                 static_cast<unsigned long long>(stats.efb_copies),
                 static_cast<unsigned long long>(stats.efb_copy_reads),
                 static_cast<unsigned long long>(stats.efb_copy_fallbacks),
                 static_cast<unsigned long long>(stats.efb_copy_palettes),
                 static_cast<unsigned long long>(stats.textures_decoded),
                 static_cast<unsigned long long>(s_renderer->ShaderCount()),
                 static_cast<unsigned long long>(s_size_mismatches),
                 static_cast<unsigned long long>(stats.interpolated_frames),
                 static_cast<unsigned long long>(stats.interpolated_draws),
                 static_cast<unsigned long long>(stats.camera_draws),
                 static_cast<unsigned long long>(stats.still_draws),
                 static_cast<unsigned long long>(s_renderer->DroppedPresentations()));
    std::fflush(stderr);
  }
}

bool Initialize()
{
  s_init_attempted = true;
  std::string error;
  s_renderer = moderngekko::native_gx::D3D12Renderer::Create(&error, s_present ? s_efb_scale : 1);
  if (!s_renderer)
  {
    std::fprintf(stderr, "[native_gx] disabled: %s\n", error.c_str());
    return false;
  }
  if (s_present && s_window && !s_renderer->AttachWindow(s_window, &error))
    std::fprintf(stderr, "[native_gx] presentation unavailable: %s\n", error.c_str());
  if (s_renderer->HasWindow() && s_present_thread)
    s_renderer->StartPresentationThread(s_vsync, &OnPresented);
  moderngekko::native_gx::FrontendOptions options;
  options.record_frames = s_interpolate;
  s_frontend = std::make_unique<moderngekko::native_gx::Frontend>(*s_renderer, &GuestMemory, options);
  s_frontend->Seed(CurrentState());
  s_frontend->before_efb_copy = s_before_efb_copy;
  s_frontend->on_xfb_copy = &OnFrame;
  if (OwnsGpu())
  {
    s_frontend->on_pe_sync = &OnPeSync;
    s_frontend->mark_copy_memory = &MarkCopyMemory;
    s_frontend->on_unknown_opcode = [](std::uint8_t opcode) {
      static int reported = 0;
      if (reported++ < 16)
        std::fprintf(stderr, "[native_gx] unknown GX opcode 0x%02X\n", opcode);
    };
  }
  std::fprintf(stderr, "[native_gx] adapter=%s presentation=%s interpolation=%s internal_resolution=%dx (%dx%d)\n",
               s_renderer->AdapterName().c_str(), s_renderer->HasWindow() ? "window" : "offscreen",
               s_interpolate ? "on" : "off", s_renderer->EfbScale(), 640 * s_renderer->EfbScale(),
               528 * s_renderer->EfbScale());
  std::fprintf(stderr, "[native_gx] command source=%s\n", OwnsGpu() ? "native fifo" : "dolphin decoder");
  return true;
}
#endif
}  // namespace

bool IsEnabled()
{
  return s_offscreen || s_present;
}

void RequestPresentation(void* window, bool interpolate, int efb_scale)
{
  s_window = window;
  s_present = true;
  s_interpolate = interpolate;
  s_efb_scale = efb_scale;
}

void SetBeforeEfbCopy(std::function<void(u32)> callback)
{
  s_before_efb_copy = std::move(callback);
#ifdef _WIN32
  if (s_frontend)
    s_frontend->before_efb_copy = s_before_efb_copy;
#endif
}

void OnTopLevelCommand(const u8* data, u32 size)
{
#ifdef _WIN32
  if (FrameInterpolation::IsReplaying())
    return;
  if (!s_frontend)
  {
    if (s_init_attempted || !Initialize())
      return;
  }
  if (s_frontend->Decode(std::span<const u8>(data, size)) != size)
    ++s_size_mismatches;
#else
  (void)data;
  (void)size;
#endif
}

void SyncStateForSave()
{
#ifdef _WIN32
  if (!OwnsGpu() || !s_frontend)
    return;
  const moderngekko::native_gx::FrontendSeed state = s_frontend->Snapshot();
  CPState& cp = g_main_cp_state;
  cp.LoadCPReg(MATINDEX_A, state.layout.matrix_index_a);
  cp.LoadCPReg(MATINDEX_B, state.layout.matrix_index_b);
  cp.LoadCPReg(VCD_LO, state.layout.vcd_low);
  cp.LoadCPReg(VCD_HI, state.layout.vcd_high);
  for (u8 i = 0; i < CP_NUM_VAT_REG; ++i)
  {
    cp.LoadCPReg(CP_VAT_REG_A + i, state.layout.vat_g0[i]);
    cp.LoadCPReg(CP_VAT_REG_B + i, state.layout.vat_g1[i]);
    cp.LoadCPReg(CP_VAT_REG_C + i, state.layout.vat_g2[i]);
  }
  for (u8 i = 0; i < CP_NUM_ARRAYS; ++i)
  {
    cp.LoadCPReg(ARRAY_BASE + i, state.layout.array_base[i]);
    cp.LoadCPReg(ARRAY_STRIDE + i, state.layout.array_stride[i]);
  }
  std::memcpy(&xfmem, state.xf.data(), sizeof(u32) * state.xf.size());
  std::memcpy(&bpmem, state.bp.data(), sizeof(u32) * state.bp.size());
  std::memcpy(s_tex_mem.data(), state.tmem.data(), std::min(state.tmem.size(), s_tex_mem.size()));
  auto& ps = Core::System::GetInstance().GetPixelShaderManager().constants;
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 4; ++c)
    {
      ps.colors[i][c] = state.tev.colors[i][c];
      ps.kcolors[i][c] = state.tev.konst[i][c];
    }
#endif
}

void ReseedAfterLoad()
{
#ifdef _WIN32
  if (s_frontend)
    s_frontend->Seed(CurrentState());
#endif
}

bool OwnsGpu()
{
#ifdef _WIN32
  return s_present && !s_dolphin_decode;
#else
  return false;
#endif
}

u32 RunFifo(const u8* data, u32 size, u32* cycles)
{
#ifdef _WIN32
  if (!s_frontend && (s_init_attempted || !Initialize()))
  {
    // Without the renderer nothing can consume the stream; drop it so the
    // CPU does not wait on a GPU that never runs.
    if (cycles)
      *cycles = size;
    return size;
  }
  const std::size_t consumed = s_frontend->DecodeStream(std::span<const u8>(data, size));
  const u32 used = s_frontend->TakeCycles();
  if (cycles)
    *cycles = used;
  return static_cast<u32>(consumed);
#else
  (void)data;
  if (cycles)
    *cycles = 0;
  return size;
#endif
}

double PresentedFps()
{
  return s_presented_fps.load(std::memory_order_relaxed);
}

moderngekko::native_gx::D3D12Renderer* Renderer()
{
#ifdef _WIN32
  return s_renderer.get();
#else
  return nullptr;
#endif
}

const moderngekko::native_gx::FrontendStats* Stats()
{
#ifdef _WIN32
  return s_frontend ? &s_frontend->Stats() : nullptr;
#else
  return nullptr;
#endif
}
}  // namespace NativeGXBridge
