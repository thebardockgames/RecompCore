// RecompCore: StaticRecomp CPU core - chassis adapter of the native VI (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeViChassis.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Movie.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeVi.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoEvents.h"

namespace StaticRecompNativeViChassis
{
using StaticRecompNativeVi::NativeVi;
using StaticRecompNativeVi::RegisterFile;
using namespace StaticRecompNativeVi::Reg;

namespace
{
u32 Get32(const RegisterFile& r, u32 hi_offset)
{
  return (u32(r[hi_offset / 2]) << 16) | r[hi_offset / 2 + 1];
}

void Set32(RegisterFile& r, u32 hi_offset, u32 value)
{
  r[hi_offset / 2] = static_cast<u16>(value >> 16);
  r[hi_offset / 2 + 1] = static_cast<u16>(value);
}
}  // namespace

struct Access
{
  static void Seed(Core::System& system, RegisterFile& r, u64& ticks_last_line_start,
                   u32& half_line_count)
  {
    const auto& vi = system.GetVideoInterface();
    r.fill(0);
    r[kVerticalTiming / 2] = vi.m_vertical_timing_register.Hex;
    r[kControl / 2] = vi.m_display_control_register.Hex;
    Set32(r, kHTiming0Hi, vi.m_h_timing_0.Hex);
    Set32(r, kHTiming1Hi, vi.m_h_timing_1.Hex);
    Set32(r, kVBlankOddHi, vi.m_vblank_timing_odd.Hex);
    Set32(r, kVBlankEvenHi, vi.m_vblank_timing_even.Hex);
    Set32(r, kBurstOddHi, vi.m_burst_blanking_odd.Hex);
    Set32(r, kBurstEvenHi, vi.m_burst_blanking_even.Hex);
    Set32(r, kFbLeftTopHi, vi.m_xfb_info_top.Hex);
    Set32(r, kFbRightTopHi, vi.m_xfb_3d_info_top.Hex);
    Set32(r, kFbLeftBottomHi, vi.m_xfb_info_bottom.Hex);
    Set32(r, kFbRightBottomHi, vi.m_xfb_3d_info_bottom.Hex);
    for (u32 i = 0; i < 4; ++i)
      Set32(r, kDisplayInterrupt0Hi + 4 * i, vi.m_interrupt_register[i].Hex);
    Set32(r, kLatch0Hi, vi.m_latch_register[0].Hex);
    Set32(r, kLatch1Hi, vi.m_latch_register[1].Hex);
    r[kHScaleW / 2] = vi.m_picture_configuration.Hex;
    r[kHScaling / 2] = vi.m_horizontal_scaling.Hex;
    for (u32 i = 0; i < 3; ++i)
      Set32(r, kFilterTable0Hi + 4 * i, vi.m_filter_coef_tables.Tables02[i].Hex);
    for (u32 i = 0; i < 4; ++i)
      Set32(r, kFilterTable3Hi + 4 * i, vi.m_filter_coef_tables.Tables36[i].Hex);
    Set32(r, kUnknownAaHi, vi.m_unknown_aa_register);
    r[kClock / 2] = vi.m_clock;
    r[kDtvStatus / 2] = vi.m_dtv_status.Hex;
    r[kFbWidth / 2] = vi.m_fb_width.Hex;
    r[kBorderHBlankLo / 2] = vi.m_border_hblank.Lo;
    r[kBorderHBlankHi / 2] = vi.m_border_hblank.Hi;
    ticks_last_line_start = vi.m_ticks_last_line_start;
    half_line_count = vi.m_half_line_count;
  }
  static void Copy(Core::System& system, const RegisterFile& r, u64 ticks_last_line_start,
                   u32 half_line_count)
  {
    auto& vi = system.GetVideoInterface();
    vi.m_vertical_timing_register.Hex = r[kVerticalTiming / 2];
    vi.m_display_control_register.Hex = r[kControl / 2];
    vi.m_h_timing_0.Hex = Get32(r, kHTiming0Hi);
    vi.m_h_timing_1.Hex = Get32(r, kHTiming1Hi);
    vi.m_vblank_timing_odd.Hex = Get32(r, kVBlankOddHi);
    vi.m_vblank_timing_even.Hex = Get32(r, kVBlankEvenHi);
    vi.m_burst_blanking_odd.Hex = Get32(r, kBurstOddHi);
    vi.m_burst_blanking_even.Hex = Get32(r, kBurstEvenHi);
    vi.m_xfb_info_top.Hex = Get32(r, kFbLeftTopHi);
    vi.m_xfb_3d_info_top.Hex = Get32(r, kFbRightTopHi);
    vi.m_xfb_info_bottom.Hex = Get32(r, kFbLeftBottomHi);
    vi.m_xfb_3d_info_bottom.Hex = Get32(r, kFbRightBottomHi);
    for (u32 i = 0; i < 4; ++i)
      vi.m_interrupt_register[i].Hex = Get32(r, kDisplayInterrupt0Hi + 4 * i);
    vi.m_latch_register[0].Hex = Get32(r, kLatch0Hi);
    vi.m_latch_register[1].Hex = Get32(r, kLatch1Hi);
    vi.m_picture_configuration.Hex = r[kHScaleW / 2];
    vi.m_horizontal_scaling.Hex = r[kHScaling / 2];
    for (u32 i = 0; i < 3; ++i)
      vi.m_filter_coef_tables.Tables02[i].Hex = Get32(r, kFilterTable0Hi + 4 * i);
    for (u32 i = 0; i < 4; ++i)
      vi.m_filter_coef_tables.Tables36[i].Hex = Get32(r, kFilterTable3Hi + 4 * i);
    vi.m_unknown_aa_register = Get32(r, kUnknownAaHi);
    vi.m_clock = r[kClock / 2];
    vi.m_dtv_status.Hex = r[kDtvStatus / 2];
    vi.m_fb_width.Hex = r[kFbWidth / 2];
    vi.m_border_hblank.Lo = r[kBorderHBlankLo / 2];
    vi.m_border_hblank.Hi = r[kBorderHBlankHi / 2];
    vi.m_ticks_last_line_start = ticks_last_line_start;
    vi.m_half_line_count = half_line_count;
    vi.UpdateParameters();
  }
};

void Install(Core::System& system)
{
  NativeVi::ChassisHooks hooks;
  hooks.remove_chassis_events = [&system] {
    system.GetCoreTiming().RemoveAllEvents(system.GetSystemTimers().GetVIEventType());
  };
  hooks.seed = [&system](RegisterFile& r, u64& ticks_last_line_start, u32& half_line_count) {
    Access::Seed(system, r, ticks_last_line_start, half_line_count);
  };
  hooks.copy = [&system](const RegisterFile& r, u64 ticks_last_line_start, u32 half_line_count) {
    Access::Copy(system, r, ticks_last_line_start, half_line_count);
  };
  hooks.preset_config = [&system] {
    // What VideoInterfaceManager::Init passes to Preset (always NTSC there).
    NativeVi::PresetConfig config;
    const DiscIO::Region region = SConfig::GetInstance().m_region;
    config.ntsc = true;
    config.clock_54mhz = DiscIO::IsNTSC(region);
    config.component = Config::Get(Config::SYSCONF_PROGRESSIVE_SCAN);
    config.ntsc_j = system.IsTriforce() || region == DiscIO::Region::NTSC_J;
    return config;
  };
  hooks.field_boundary = [&system] { system.GetMovie().FrameUpdate(); };
  hooks.new_field = [&system] { Core::Callback_NewField(system); };
  hooks.output_xfb = [](u32 xfb, u32 width, u32 stride, u32 height, u64 ticks) {
    g_video_backend->Video_OutputXFB(xfb, width, stride, height, ticks);
  };
  hooks.force_progressive = [] { return Config::Get(Config::GFX_HACK_FORCE_PROGRESSIVE); };
  hooks.early_xfb_output = [] { return Config::Get(Config::GFX_HACK_EARLY_XFB_OUTPUT); };
  hooks.immediate_xfb = [] { return Config::Get(Config::GFX_HACK_IMMEDIATE_XFB); };
  hooks.throttle = [&system](u64 ticks) { system.GetCoreTiming().Throttle(ticks); };
  hooks.end_field = [&system] {
    system.GetPerfMetrics().CountVBlank();
    system.GetVideoEvents().vi_end_field_event.Trigger();
    Core::OnFrameEnd(system);
  };
  NativeVi::SetChassisHooks(std::move(hooks));
}
}  // namespace StaticRecompNativeViChassis
