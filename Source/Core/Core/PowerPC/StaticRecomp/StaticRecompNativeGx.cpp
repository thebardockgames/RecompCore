// RecompCore: StaticRecomp CPU core - host of the native GX renderer (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompFrameEvents.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRam.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"

#ifdef _WIN32
#include "moderngekko/native_gx/d3d12_renderer.hpp"
#include "moderngekko/native_gx/frontend.hpp"
#endif

namespace StaticRecompNativeGx
{
namespace
{
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

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
uint64_t s_last_xfb_ticks = 0;
std::function<void(uint32_t)> s_before_efb_copy;
bool s_init_attempted = false;
uint64_t s_size_mismatches = 0;
std::atomic<double> s_presented_fps{0.0};
std::deque<std::chrono::steady_clock::time_point> s_present_times;
GpHooks s_gp_hooks;
ChassisHooks s_chassis;
std::function<uint32_t()> s_ticks_per_field;

// The GX state of the native checkpoints, in the port's layout.
constexpr uint32_t kStateVersion = 1;

#ifdef _WIN32
using moderngekko::native_gx::FrontendSeed;

std::unique_ptr<moderngekko::native_gx::D3D12Renderer> s_renderer;
std::unique_ptr<moderngekko::native_gx::Frontend> s_frontend;

// A seed loaded from a checkpoint before the front end exists (it is created
// at the first command): applied then. Its TMEM bytes live here.
struct PendingSeed
{
  bool present = false;
  FrontendSeed seed;
  std::vector<uint8_t> tmem;
};
PendingSeed s_pending;

// The port's guest memory (phase S) at a physical address as the GP sees it
// (the console's rules: bits 30-31 ignored, MEM2 at 0x10000000). Null when the
// range is not backed; the front end treats that as missing data.
uint8_t* GuestPointer(uint32_t address, uint32_t size)
{
  const auto& ram = StaticRecompGuestRam::GuestRam::Instance();
  const uint32_t physical = address & 0x3FFFFFFFu;
  if (physical < ram.Mem1Size())
    return size <= ram.Mem1Size() - physical ? ram.Mem1() + physical : nullptr;
  if ((physical >> 28) == 0x1)
  {
    const uint32_t offset = physical & 0x0FFFFFFFu;
    if (offset < ram.Mem2Size() && size <= ram.Mem2Size() - offset)
      return ram.Mem2() + offset;
  }
  return nullptr;
}

std::span<const uint8_t> GuestMemory(uint32_t address, uint32_t size)
{
  const uint8_t* data = GuestPointer(address, size);
  return data ? std::span<const uint8_t>(data, size) : std::span<const uint8_t>{};
}

// The GX state after a reset, as the chassis initialises it (BPInit: all BP
// registers zero but the write mask; CP, XF, TMEM, TEV constants and the
// normal caches zero). What the front end starts from on a native boot.
FrontendSeed DefaultSeed()
{
  FrontendSeed seed;
  seed.bp[0xFE] = 0xFFFFFF;  // BP mask
  return seed;
}

// A chassis state was loaded before the first command (a .sav at boot): the
// chassis's GX state, not the reset state, is what the front end starts from.
bool s_chassis_state_loaded = false;

// Fields in which two seeds differ (diagnostic for the boot seed).
std::string SeedDifferences(const FrontendSeed& a, const FrontendSeed& b)
{
  std::string out;
  const auto note = [&](const char* what) {
    if (!out.empty())
      out += ", ";
    out += what;
  };
  if (std::memcmp(&a.layout, &b.layout, sizeof(a.layout)) != 0)
    note("layout");
  if (a.xf != b.xf)
    note("xf");
  if (a.bp != b.bp)
    note("bp");
  const std::size_t tmem = std::max(a.tmem.size(), b.tmem.size());
  for (std::size_t i = 0; i < tmem; ++i)
  {
    const uint8_t x = i < a.tmem.size() ? a.tmem[i] : 0, y = i < b.tmem.size() ? b.tmem[i] : 0;
    if (x != y)
    {
      note("tmem");
      break;
    }
  }
  if (a.tev.colors != b.tev.colors || a.tev.konst != b.tev.konst)
    note("tev");
  if (std::memcmp(a.cached_normal, b.cached_normal, sizeof(a.cached_normal)) != 0)
    note("cached_normal");
  return out;
}

// What the front end starts from: a checkpoint's state when one was loaded;
// the chassis's GX state after a chassis load; else the reset state (checked
// against the chassis's copy while the chassis exists: the port must not
// depend on it for a boot).
FrontendSeed InitialSeed(const char** source)
{
  if (s_pending.present)
  {
    *source = "native checkpoint";
    return s_pending.seed;
  }
  if (s_chassis_state_loaded && s_chassis.export_seed)
  {
    *source = "chassis state";
    return s_chassis.export_seed();
  }
  FrontendSeed seed = DefaultSeed();
  *source = "reset";
  if (s_chassis.export_seed)
  {
    const std::string differences = SeedDifferences(seed, s_chassis.export_seed());
    if (!differences.empty())
    {
      std::fprintf(stderr, "[native_gx] boot seed: the reset state differs from the chassis's in %s; "
                           "using the chassis's\n", differences.c_str());
      *source = "chassis (reset mismatch)";
      return s_chassis.export_seed();
    }
    *source = "reset (verified against the chassis)";
  }
  return seed;
}

void SerializeSeed(StaticRecompState::StateStream& p, FrontendSeed& seed, std::vector<uint8_t>& tmem)
{
  uint32_t version = kStateVersion;
  p.Do(version);
  if (version != kStateVersion)
  {
    p.SetMeasureMode();
    return;
  }
  auto& layout = seed.layout;
  p.Do(layout.vcd_low);
  p.Do(layout.vcd_high);
  p.DoArray(layout.vat_g0.data(), static_cast<uint32_t>(layout.vat_g0.size()));
  p.DoArray(layout.vat_g1.data(), static_cast<uint32_t>(layout.vat_g1.size()));
  p.DoArray(layout.vat_g2.data(), static_cast<uint32_t>(layout.vat_g2.size()));
  p.DoArray(layout.array_base.data(), static_cast<uint32_t>(layout.array_base.size()));
  p.DoArray(layout.array_stride.data(), static_cast<uint32_t>(layout.array_stride.size()));
  p.Do(layout.matrix_index_a);
  p.Do(layout.matrix_index_b);
  p.DoArray(seed.xf.data(), static_cast<uint32_t>(seed.xf.size()));
  p.DoArray(seed.bp.data(), static_cast<uint32_t>(seed.bp.size()));
  if (p.IsWriteMode())
    tmem.assign(seed.tmem.begin(), seed.tmem.end());
  p.Do(tmem);
  if (p.IsReadMode())
    seed.tmem = std::span<const uint8_t>(tmem.data(), tmem.size());
  for (auto& color : seed.tev.colors)
    p.DoArray(color.data(), static_cast<uint32_t>(color.size()));
  for (auto& konst : seed.tev.konst)
    p.DoArray(konst.data(), static_cast<uint32_t>(konst.size()));
  for (auto& normal : seed.cached_normal)
    p.DoArray(normal, 3);
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
std::atomic<bool> s_vsync{[] {
  const char* v = std::getenv("MODERNGEKKO_NATIVE_GX_VSYNC");
  return v == nullptr || v[0] != '0';
}()};
// RequestDisplay: what the next frame end applies, and what is applied.
std::mutex s_display_mutex;
std::optional<DisplaySettings> s_display_request;
std::atomic<bool> s_display_pending{false};
std::atomic<int> s_display_scale{0};
std::atomic<int> s_display_backbuffer_width{0}, s_display_backbuffer_height{0};
std::atomic<std::uint64_t> s_display_generation{0};
std::atomic<uint64_t> s_frame_number{0};
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
    const uint64_t frame = s_frame_number.load(std::memory_order_relaxed);
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
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
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
  std::vector<uint32_t> pixels;
  int width = 0, height = 0;
  if (!s_renderer->ReadXfb(image, &pixels, &width, &height))
    return;
  for (uint32_t& p : pixels)
    p |= 0xFF000000u;
  if (!s_chassis.save_image)
  {
    static bool reported = false;
    if (!reported)
    {
      reported = true;
      std::fprintf(stderr, "[native_gx] no image writer installed: dumps skipped\n");
    }
    return;
  }
  s_chassis.save_image(name, reinterpret_cast<const uint8_t*>(pixels.data()), width, height);
}

void DumpImages(bool extra)
{
  static const char* dir = std::getenv("MODERNGEKKO_NATIVE_GX_DUMP");
  if (!dir)
    return;
  static const uint64_t every = [] {
    const char* v = std::getenv("MODERNGEKKO_NATIVE_GX_DUMP_EVERY");
    const long long n = v ? std::atoll(v) : 0;
    return n >= 2 ? uint64_t(n) : uint64_t(120);
  }();
  const uint64_t frame = s_frontend->Stats().frames;
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

// Copy destinations get what real hardware would not leave there: the image
// stays on the GPU and the memory is marked instead (zero for EFB copies, YUV
// fuchsia for the XFB), so a CPU write is detected.
void MarkCopyMemory(uint32_t address, uint32_t stride, uint32_t bytes_per_row, uint32_t rows, bool xfb)
{
  for (uint32_t row = 0; row < rows; ++row)
  {
    uint8_t* data = GuestPointer(address + row * stride, bytes_per_row);
    if (!data)
      return;
    if (!xfb)
    {
      std::memset(data, 0, bytes_per_row);
      continue;
    }
    for (uint32_t i = 0; i + 1 < bytes_per_row; i += 2)
    {
      data[i] = 1;  // Y=1, U/V=254: fuchsia
      data[i + 1] = 254;
    }
  }
}

void OnPeSync(uint8_t reg, uint32_t value, uint32_t cycles)
{
  if (s_gp_hooks.pe_sync)
  {
    s_gp_hooks.pe_sync(reg, value, cycles);
    return;
  }
  if (s_chassis.pe_sync)
    s_chassis.pe_sync(reg, value, cycles);
}

uint64_t NowTicks()
{
  if (auto* clock = StaticRecompNativeClock::NativeClock::Instance())
    return clock->Now();
  return s_chassis.now_ticks ? s_chassis.now_ticks() : 0;
}

uint32_t TicksPerField()
{
  if (s_ticks_per_field)
    return s_ticks_per_field();
  return s_chassis.ticks_per_field ? s_chassis.ticks_per_field() : 0;
}

void OnFrame(const moderngekko::native_gx::XfbCopyParams&)
{
  if (OwnsGpu())
  {
    // The port's frame event; the chassis's own listeners (its statistics,
    // framebuffer and config bookkeeping) still get theirs, without echo.
    StaticRecompFrameEvents::Fire();
    if (s_chassis.after_frame)
    {
      StaticRecompFrameEvents::SuppressChassisEcho no_echo;
      s_chassis.after_frame();
    }
  }
  // Game frames lasting two VI fields (30 Hz) get an extra, interpolated image
  // shown on the first field; 60 Hz scenes are presented as they are.
  const uint64_t ticks = NowTicks();
  const uint64_t field = TicksPerField();
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
        s_renderer->Present(s_vsync.load(), 1);
        RecordPresent(1);
        CountPresented(std::chrono::steady_clock::now());
      }
      s_renderer->Present(s_vsync.load(), 0);
      RecordPresent(0);
      CountPresented(std::chrono::steady_clock::now());
    }
  }
  DumpImages(extra);
  s_renderer->EndFrame();
  {
    int width = 0, height = 0;
    s_renderer->BackbufferSize(&width, &height);
    s_display_backbuffer_width = width;
    s_display_backbuffer_height = height;
  }
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

void BindFrontend();

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
    s_renderer->StartPresentationThread(s_vsync.load(), &OnPresented);
  s_display_scale = s_renderer->EfbScale();
  moderngekko::native_gx::FrontendOptions options;
  options.record_frames = s_interpolate;
  s_frontend = std::make_unique<moderngekko::native_gx::Frontend>(*s_renderer, &GuestMemory, options);
  const char* seed_source = "";
  s_frontend->Seed(InitialSeed(&seed_source));
  s_pending = PendingSeed{};
  BindFrontend();
  std::fprintf(stderr, "[native_gx] adapter=%s presentation=%s interpolation=%s internal_resolution=%dx (%dx%d)\n",
               s_renderer->AdapterName().c_str(), s_renderer->HasWindow() ? "window" : "offscreen",
               s_interpolate ? "on" : "off", s_renderer->EfbScale(), 640 * s_renderer->EfbScale(),
               528 * s_renderer->EfbScale());
  std::fprintf(stderr, "[native_gx] command source=%s, initial state=%s\n",
               OwnsGpu() ? "native fifo" : "dolphin decoder", seed_source);
  return true;
}

void BindFrontend()
{
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
}

// RequestDisplay, applied at the end of a frame on the thread that draws.
void ApplyDisplayRequest()
{
  std::optional<DisplaySettings> request;
  {
    std::lock_guard lock(s_display_mutex);
    request.swap(s_display_request);
    s_display_pending.store(false, std::memory_order_release);
  }
  if (!request || !s_renderer)
    return;
  const bool vsync_changed = request->vsync != s_vsync.load();
  s_vsync = request->vsync;
  s_renderer->SetPresentationVsync(request->vsync);
  const int scale = std::clamp(request->efb_scale, 1, 8);
  if (s_present && s_window && scale != s_renderer->EfbScale())
  {
    // A new renderer at the new internal resolution; the front end continues
    // from the old one's GX state (TMEM copied out first) and counters. EFB
    // copies and decoded textures are made again from guest memory.
    s_renderer->Flush();
    FrontendSeed seed = s_frontend->Snapshot();
    std::vector<uint8_t> tmem(seed.tmem.begin(), seed.tmem.end());
    seed.tmem = tmem;
    const auto stats = s_frontend->Stats();
    const bool record = s_interpolate;
    s_frontend.reset();
    s_renderer.reset();
    std::string error;
    s_efb_scale = scale;
    s_renderer = moderngekko::native_gx::D3D12Renderer::Create(&error, scale);
    if (!s_renderer || !s_renderer->AttachWindow(s_window, &error))
    {
      std::fprintf(stderr, "[native_gx] fatal: renderer at internal resolution %dx: %s\n", scale, error.c_str());
      std::abort();
    }
    if (s_present_thread)
      s_renderer->StartPresentationThread(s_vsync.load(), &OnPresented);
    moderngekko::native_gx::FrontendOptions options;
    options.record_frames = record;
    s_frontend = std::make_unique<moderngekko::native_gx::Frontend>(*s_renderer, &GuestMemory, options);
    s_frontend->Seed(seed);
    s_frontend->RestoreStats(stats);
    BindFrontend();
    s_display_scale = scale;
    std::fprintf(stderr, "[native_gx] internal resolution now %dx (%dx%d)\n", scale, 640 * scale, 528 * scale);
  }
  else if (vsync_changed)
  {
    std::fprintf(stderr, "[native_gx] vsync %s\n", request->vsync ? "on" : "off");
  }
  s_display_generation.fetch_add(1);
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

void SetInitialVsync(bool vsync)
{
#ifdef _WIN32
  s_vsync = vsync;
#endif
}

void RequestDisplay(DisplaySettings settings)
{
#ifdef _WIN32
  std::lock_guard lock(s_display_mutex);
  s_display_request = settings;
  s_display_pending.store(true, std::memory_order_release);
#endif
}

DisplayStatus CurrentDisplay()
{
  DisplayStatus status;
#ifdef _WIN32
  status.efb_scale = s_display_scale.load();
  status.vsync = s_vsync.load();
  status.backbuffer_width = s_display_backbuffer_width.load();
  status.backbuffer_height = s_display_backbuffer_height.load();
  status.efb_width = 640 * status.efb_scale;
  status.efb_height = 528 * status.efb_scale;
  status.generation = s_display_generation.load();
#endif
  return status;
}

void SetBeforeEfbCopy(std::function<void(uint32_t)> callback)
{
  s_before_efb_copy = std::move(callback);
#ifdef _WIN32
  if (s_frontend)
    s_frontend->before_efb_copy = s_before_efb_copy;
#endif
}

void OnTopLevelCommand(const uint8_t* data, uint32_t size)
{
#ifdef _WIN32
  if (s_chassis.is_replaying && s_chassis.is_replaying())
    return;
  if (!s_frontend)
  {
    if (s_init_attempted || !Initialize())
      return;
  }
  if (s_frontend->Decode(std::span<const uint8_t>(data, size)) != size)
    ++s_size_mismatches;
#else
  (void)data;
  (void)size;
#endif
}

void SyncStateForSave()
{
  if (s_gp_hooks.before_save)
    s_gp_hooks.before_save();
#ifdef _WIN32
  if (!OwnsGpu() || !s_frontend || !s_chassis.import_seed)
    return;
  s_chassis.import_seed(s_frontend->Snapshot());
#endif
}

void ReseedAfterLoad()
{
  if (s_gp_hooks.after_load)
    s_gp_hooks.after_load();
#ifdef _WIN32
  s_pending = PendingSeed{};  // the chassis's state is the newest
  s_chassis_state_loaded = true;
  if (s_frontend)
    s_frontend->Seed(s_chassis.export_seed ? s_chassis.export_seed() : DefaultSeed());
#endif
}

std::vector<uint8_t> SerializeState()
{
  std::vector<uint8_t> out;
#ifdef _WIN32
  const char* source = "";
  FrontendSeed seed = s_frontend ? s_frontend->Snapshot() : InitialSeed(&source);
  std::vector<uint8_t> tmem;
  StaticRecompState::BufferStream writer;
  SerializeSeed(writer, seed, tmem);
  out = writer.Take();
#endif
  return out;
}

bool LoadState(const std::vector<uint8_t>& data)
{
#ifdef _WIN32
  PendingSeed loaded;
  StaticRecompState::BufferStream reader(data);
  SerializeSeed(reader, loaded.seed, loaded.tmem);
  if (!reader.IsReadMode())
    return false;
  loaded.present = true;
  s_pending = std::move(loaded);
  s_pending.seed.tmem = std::span<const uint8_t>(s_pending.tmem.data(), s_pending.tmem.size());
  if (s_frontend)
  {
    s_frontend->Seed(s_pending.seed);
    s_pending = PendingSeed{};
  }
  // Loading a checkpoint counts as the end of the frame it was saved in: the
  // chassis's presenter raised its frame event on every load (Presenter::
  // DoState), the frame clock of the runtime and the input scripts advanced
  // once, and every reference image set was made that way.
  StaticRecompFrameEvents::Fire();
  if (s_chassis.after_frame)
  {
    StaticRecompFrameEvents::SuppressChassisEcho no_echo;
    s_chassis.after_frame();
  }
  return true;
#else
  (void)data;
  return false;
#endif
}

std::vector<uint8_t> SerializeLegacyVideoSection()
{
  if (!s_chassis.save_legacy_video)
    return {};
  return s_chassis.save_legacy_video();
}

bool LoadLegacyVideoSection(const std::vector<uint8_t>& data)
{
  if (!s_chassis.load_legacy_video)
  {
    std::fprintf(stderr, "[native_gx] the checkpoint's graphics section is the chassis's and no chassis is here\n");
    return false;
  }
  return s_chassis.load_legacy_video(data);
}

bool OwnsGpu()
{
#ifdef _WIN32
  return s_present && !s_dolphin_decode;
#else
  return false;
#endif
}

uint32_t RunFifo(const uint8_t* data, uint32_t size, uint32_t* cycles)
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
  const std::size_t consumed = s_frontend->DecodeStream(std::span<const uint8_t>(data, size));
  const uint32_t used = s_frontend->TakeCycles();
  if (cycles)
    *cycles = used;
  // Display changes replace the renderer and the front end: only here, with
  // no front end call on the stack.
  if (s_display_pending.load(std::memory_order_acquire))
    ApplyDisplayRequest();
  return static_cast<uint32_t>(consumed);
#else
  (void)data;
  if (cycles)
    *cycles = 0;
  return size;
#endif
}

void SetGpHooks(GpHooks hooks)
{
  s_gp_hooks = std::move(hooks);
}

void SetTicksPerFieldProvider(std::function<uint32_t()> provider)
{
  s_ticks_per_field = std::move(provider);
}

void SetChassisHooks(ChassisHooks hooks)
{
  s_chassis = std::move(hooks);
}

void FlushGp()
{
  if (s_gp_hooks.flush)
    s_gp_hooks.flush();
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
}  // namespace StaticRecompNativeGx
