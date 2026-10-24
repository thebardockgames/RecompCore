// RecompCore: StaticRecomp CPU core - host of the native GX renderer (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The port's side of the graphics: receives the GX command stream (from the
// native GP front, or from the chassis's FIFO loop while that path exists),
// decodes it with moderngekko::native_gx, presents the native XFB in the
// window and keeps the GX state for the native checkpoints (.mgs). It has no
// Dolphin dependency: what the chassis still provides while it exists (its GX
// state for a .sav, its pixel engine when the native GP front is off, its
// frame event, its PNG writer, its frame interpolation flag) comes through
// ChassisHooks, installed by VideoCommon/NativeGXBridge.cpp, the adapter.
//
// Enabled by MODERNGEKKO_NATIVE_GX_RENDER (offscreen, for the comparison in
// NativeGXVerify) or by RequestPresentation (the NativeGX graphics mode, with
// Dolphin's video backend set to Null).

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace moderngekko::native_gx
{
class D3D12Renderer;
struct FrontendStats;
struct FrontendSeed;
}  // namespace moderngekko::native_gx

namespace StaticRecompNativeGx
{
bool IsEnabled();

// Presents in this window (HWND); call before video starts. interpolate: an
// extra image between game frames when the game runs at 30 Hz (60 images/s).
// efb_scale: internal resolution (1 = the console's 640 x 528 EFB).
void RequestPresentation(void* window, bool interpolate, int efb_scale);

// Display settings that can change while the game runs (the PC settings
// service). They apply at the end of the next frame on the thread that draws:
// vsync at once; a new internal resolution recreates the renderer and the
// front end, which continues from the previous one's GX state and counters.
struct DisplaySettings
{
  int efb_scale = 1;
  bool vsync = true;
};
struct DisplayStatus
{
  int efb_scale = 0;
  bool vsync = true;
  int backbuffer_width = 0, backbuffer_height = 0;
  int efb_width = 0, efb_height = 0;
  std::uint64_t generation = 0;  // display changes applied so far
};
void SetInitialVsync(bool vsync);  // before video starts
void RequestDisplay(DisplaySettings settings);
DisplayStatus CurrentDisplay();

// Called before every EFB copy executes (see Frontend::before_efb_copy).
void SetBeforeEfbCopy(std::function<void(std::uint32_t copy_value)> callback);

// A top-level command the chassis's decoder finished (not during its
// interpolation replays): the Dolphin-decode path.
void OnTopLevelCommand(const std::uint8_t* data, std::uint32_t size);

// Whether the native renderer is the GPU (NativeGX graphics mode): the FIFO
// bytes go to RunFifo and nothing of the chassis decodes commands, loads
// vertices or tracks GX state. MODERNGEKKO_NATIVE_GX_DOLPHIN_DECODE=1 keeps
// the chassis decoding (Null backend) and feeds the native renderer from it.
bool OwnsGpu();

// Decodes the complete commands at the start of a FIFO burst (Pixel Engine
// draw-done/tokens, copy memory and end-of-frame events included). Returns
// the bytes consumed; *cycles receives the GPU time.
std::uint32_t RunFifo(const std::uint8_t* data, std::uint32_t size, std::uint32_t* cycles);

// The chassis's savestates keep its GX state layout: when the native renderer
// owns the GPU that state is its own, copied to the chassis before a save
// (import_seed hook) and taken back from it after a load (export_seed hook).
void SyncStateForSave();
void ReseedAfterLoad();

// The native checkpoints (.mgs) carry the GX state in the port's own layout.
// SerializeState: the front end's state (or the state waiting to seed it).
// LoadState: keeps the state and seeds the front end now or when it is created.
std::vector<std::uint8_t> SerializeState();
bool LoadState(const std::vector<std::uint8_t>& data);
// A .mgs written before the own section, or by a run where the chassis owns
// the GPU: its video section is the chassis's (served through the
// load/save_legacy_video hooks while the chassis exists).
bool LoadLegacyVideoSection(const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> SerializeLegacyVideoSection();

// Phase E: when the native GP front (StaticRecompNativeGp) owns the gather
// pipe and the CP/PE registers, it receives the pixel-engine sync events
// (tokens, draw-done) and is told around savestate save/load so its registers
// stay in step with the chassis's copies.
struct GpHooks
{
  std::function<void(std::uint8_t reg, std::uint32_t value, std::uint32_t cycles)> pe_sync;
  std::function<void()> before_save;
  std::function<void()> after_load;
  // Fifo::FlushGpu: the CPU is about to skip idle time (CoreTiming::Idle with
  // SyncOnSkipIdle) and must first let the GP finish what it has, or
  // draw-done would land after the skipped time and the game would see it late.
  std::function<void()> flush;
};
void SetGpHooks(GpHooks hooks);
void FlushGp();
// Phase D: the native video interface provides the field length used to tell
// 30 Hz frames (two fields) from 60 Hz ones.
void SetTicksPerFieldProvider(std::function<std::uint32_t()> provider);

// What the chassis provides while it exists (VideoCommon/NativeGXBridge.cpp
// installs these; none is required: without them the host uses its defaults).
struct ChassisHooks
{
  // The chassis's GX state as a seed (boot before any command, .sav load).
  std::function<moderngekko::native_gx::FrontendSeed()> export_seed;
  // The front end's state into the chassis's structures (before a .sav save).
  std::function<void(const moderngekko::native_gx::FrontendSeed&)> import_seed;
  // Pixel engine sync when the native GP front is off.
  std::function<void(std::uint8_t reg, std::uint32_t value, std::uint32_t cycles)> pe_sync;
  // The chassis's end-of-frame event (its listeners: the runtime's hooks).
  std::function<void()> after_frame;
  // The cycle counter and the field length when no native clock / VI exists.
  std::function<std::uint64_t()> now_ticks;
  std::function<std::uint32_t()> ticks_per_field;
  // The chassis's own frame interpolation is replaying (Dolphin-decode path).
  std::function<bool()> is_replaying;
  // PNG writer for the diagnostic dumps (RGBA, width*4 stride).
  std::function<bool(const std::string& path, const std::uint8_t* rgba, int width, int height)> save_image;
  // Loads / writes a .mgs video section in the chassis's layout.
  std::function<bool(const std::vector<std::uint8_t>& data)> load_legacy_video;
  std::function<std::vector<std::uint8_t>()> save_legacy_video;
};
void SetChassisHooks(ChassisHooks hooks);

// Images the native renderer presented per second (last second); 0 when off.
double PresentedFps();

// Null until the first command created them.
moderngekko::native_gx::D3D12Renderer* Renderer();
const moderngekko::native_gx::FrontendStats* Stats();
}  // namespace StaticRecompNativeGx
