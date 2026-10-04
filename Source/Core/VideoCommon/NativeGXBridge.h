// Host for the native GX renderer (moderngekko::native_gx) inside the video
// thread: receives each top-level GX command Dolphin delimited, renders it with
// the native front end and, when a window was given, presents the native XFB.
//
// Enabled by MODERNGEKKO_NATIVE_GX_RENDER (offscreen, for the comparison in
// NativeGXVerify) or by RequestPresentation (the NativeGX graphics mode, with
// Dolphin's video backend set to Null).
#pragma once

#include <cstdint>
#include <functional>

#include "Common/CommonTypes.h"

namespace moderngekko::native_gx
{
class D3D12Renderer;
struct FrontendStats;
}  // namespace moderngekko::native_gx

namespace NativeGXBridge
{
bool IsEnabled();

// Presents in this window (HWND); call before video starts. interpolate: an
// extra image between game frames when the game runs at 30 Hz (60 images/s).
// efb_scale: internal resolution (1 = the console's 640 x 528 EFB).
void RequestPresentation(void* window, bool interpolate, int efb_scale);

// Called before every EFB copy executes (see Frontend::before_efb_copy).
void SetBeforeEfbCopy(std::function<void(u32 copy_value)> callback);

// A top-level command Dolphin finished decoding (not during interpolation replays).
void OnTopLevelCommand(const u8* data, u32 size);

// Whether the native renderer is the GPU (NativeGX graphics mode): the FIFO
// bytes go to RunFifo instead of Dolphin's OpcodeDecoder, so Dolphin no
// longer decodes commands, loads vertices or tracks GX state. Set
// MODERNGEKKO_NATIVE_GX_DOLPHIN_DECODE=1 to keep Dolphin decoding (Null
// backend) and feed the native renderer from it, as before.
bool OwnsGpu();

// Decodes the complete commands at the start of a FIFO burst with the native
// front end (Pixel Engine draw-done/tokens, copy memory and end-of-frame
// events included). Returns the bytes consumed; *cycles receives the GPU time.
u32 RunFifo(const u8* data, u32 size, u32* cycles);

// Savestates keep Dolphin's GX state layout (BP, CP, XF, TMEM, TEV colours).
// When the native renderer owns the GPU that state is its own: copied into
// Dolphin's structures before a save, and reloaded from them after a load.
void SyncStateForSave();
void ReseedAfterLoad();

// Images the native renderer presented per second (last second); 0 when off.
double PresentedFps();

// Null until the first command created them.
moderngekko::native_gx::D3D12Renderer* Renderer();
const moderngekko::native_gx::FrontendStats* Stats();
}  // namespace NativeGXBridge
