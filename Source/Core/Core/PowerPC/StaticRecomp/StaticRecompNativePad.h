// RecompCore: StaticRecomp CPU core - native PAD library state.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal to StaticRecompNativeSdk.cpp / StaticRecompNativePad.cpp.

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

namespace StaticRecompNativeSdk
{
struct NativeSdk::PadState
{
  bool active = false, abandoned = false;
  std::array<int, 4> rumble{-1, -1, -1, -1};  // last strength sent to the PC controller
  u64 reads = 0, resets = 0, recalibrates = 0, motors = 0, enabled = 0, disconnects = 0;
  u64 origin_requests = 0, chained = 0, deferred = 0;
};
}  // namespace StaticRecompNativeSdk
