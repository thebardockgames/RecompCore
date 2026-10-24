// Chassis adapter of the native GX host (phase S): what Dolphin still provides
// to StaticRecompNativeGx while the chassis exists. Installed from
// VideoBackendBase::InitializeShared. Nothing here runs once the chassis is gone.

#include "VideoCommon/NativeGXBridge.h"

#include <cstring>
#include <functional>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/Image.h"
#include "Core/CoreTiming.h"
#include "Core/HW/VideoInterface.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompFrameEvents.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/FrameInterpolation.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/XFMemory.h"

#ifdef _WIN32
#include "moderngekko/native_gx/frontend.hpp"
#endif

namespace NativeGXBridge
{
namespace
{
#ifdef _WIN32
// Dolphin's GX state as a seed for the native front end (the first command
// after a boot, or after a .sav load).
moderngekko::native_gx::FrontendSeed ExportSeed()
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

// The native front end's state into Dolphin's structures (before a .sav save).
void ImportSeed(const moderngekko::native_gx::FrontendSeed& state)
{
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
}
#endif

void PeSync(u8 reg, u32 value, u32 cycles)
{
  auto& pixel_engine = Core::System::GetInstance().GetPixelEngine();
  if (reg == 0x45)
    pixel_engine.SetFinish(static_cast<int>(cycles));
  else
    pixel_engine.SetToken(static_cast<u16>(value & 0xFFFF), reg == 0x48, static_cast<int>(cycles));
}

std::vector<u8> SaveLegacyVideo()
{
  u8* measure_ptr = nullptr;
  PointerWrap measure(&measure_ptr, 0, PointerWrap::Mode::Measure);
  g_video_backend->DoState(measure);
  const size_t size = measure.GetOffsetFromPreviousPosition(nullptr);
  std::vector<u8> buffer(size);
  u8* ptr = buffer.data();
  PointerWrap write(&ptr, buffer.size(), PointerWrap::Mode::Write);
  g_video_backend->DoState(write);
  if (!write.IsWriteMode())
    buffer.clear();
  return buffer;
}

bool LoadLegacyVideo(const std::vector<u8>& data)
{
  std::vector<u8> blob = data;
  u8* ptr = blob.data();
  PointerWrap read(&ptr, blob.size(), PointerWrap::Mode::Read);
  g_video_backend->DoState(read);
  return read.IsReadMode();
}

bool s_installed = false;
Common::EventHook s_frame_forward;
}  // namespace

void InstallChassisAdapter()
{
  if (s_installed)
    return;
  s_installed = true;
  StaticRecompNativeGx::ChassisHooks hooks;
#ifdef _WIN32
  hooks.export_seed = &ExportSeed;
  hooks.import_seed = &ImportSeed;
#endif
  hooks.pe_sync = &PeSync;
  hooks.after_frame = [] {
    auto& system = Core::System::GetInstance();
    system.GetVideoEvents().after_frame_event.Trigger(system);
  };
  hooks.now_ticks = [] { return Core::System::GetInstance().GetCoreTiming().GetTicks(); };
  hooks.ticks_per_field = [] { return Core::System::GetInstance().GetVideoInterface().GetTicksPerField(); };
  hooks.is_replaying = [] { return FrameInterpolation::IsReplaying(); };
  hooks.save_image = [](const std::string& path, const u8* rgba, int width, int height) {
    return Common::SavePNG(path, rgba, Common::ImageByteFormat::RGBA, width, height, width * 4);
  };
  hooks.load_legacy_video = &LoadLegacyVideo;
  hooks.save_legacy_video = &SaveLegacyVideo;
  // The chassis's end of frame into the port's event (its renderer in the
  // oracle path, its presenter on its own state loads).
  s_frame_forward = GetVideoEvents().after_frame_event.Register(
      [](Core::System&) { StaticRecompFrameEvents::FireFromChassis(); });
  StaticRecompNativeGx::SetChassisHooks(std::move(hooks));
}
}  // namespace NativeGXBridge
