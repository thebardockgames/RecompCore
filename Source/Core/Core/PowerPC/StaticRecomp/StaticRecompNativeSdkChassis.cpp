// RecompCore: StaticRecomp CPU core - chassis adapter of the native SDK layer (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdkChassis.h"

#include <cstdio>

#include "Core/HW/AudioInterface.h"
#include "Core/HW/DSP.h"
#include "Core/HW/SI/SI.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompHostLoop.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
#include "Core/System.h"

namespace StaticRecompNativeSdkChassis
{
using StaticRecompNativeSdk::NativeSdk;

void Install(Core::System& system)
{
  if (system.GetAudioInterface().Get32KHzSampleRateDivisor() != 1125 * 2 * 3 / 2)
    std::fprintf(stderr, "[native_sdk] the chassis's AI divisor differs from the Wii's\n");
  NativeSdk::ChassisHooks hooks;
  hooks.si_status = [&system] { return system.GetSerialInterface().PeekStatus(); };
  hooks.si_input = [&system](int chan, u32& hi, u32& lo) {
    auto& si = system.GetSerialInterface();
    hi = si.PeekInHi(chan);
    lo = si.PeekInLo(chan);
  };
  hooks.park_audio = [&system] { system.GetDSP().SetNativeAudio(true); };
  hooks.ai_seed = [&system](u32& control, u32& volume, u32& interrupt_timing) {
    if (StaticRecompHostLoop::SkipsDevice("ai"))
      return false;
    const auto& ai = system.GetAudioInterface();
    control = ai.GetControlRegister();
    volume = ai.GetVolumeRegister();
    interrupt_timing = ai.GetInterruptTiming();
    return true;
  };
  hooks.dsp_control_seed = [&system](u16& control) {
    if (StaticRecompHostLoop::SkipsDevice("dsp"))
      return false;
    control = system.GetDSP().GetControlRegister();
    return true;
  };
  hooks.ios_title_identity = [&system](u32* uid, u16* gid) {
    auto ios = system.GetIOS();
    if (!ios)
      return false;
    *uid = ios->GetUidForPPC();
    *gid = ios->GetGidForPPC();
    return true;
  };
  hooks.migrate_fs_handle = [&system](u32 fs_fd) -> std::optional<u32> {
    auto ios = system.GetIOS();
    const auto old_fs = ios ? ios->GetFS() : nullptr;
    if (!old_fs)
      return std::nullopt;
    std::optional<u32> offset;
    if (const auto status = old_fs->GetFileStatus(fs_fd))
      offset = status->offset;
    old_fs->Close(fs_fd);
    return offset;
  };
  NativeSdk::SetChassisHooks(std::move(hooks));
}
}  // namespace StaticRecompNativeSdkChassis
