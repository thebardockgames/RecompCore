// RecompCore: StaticRecomp CPU core - native THP video decoder state.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal to StaticRecompNativeSdk.cpp / StaticRecompNativeThp.cpp.

#pragma once

#include <array>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

namespace StaticRecompNativeSdk
{
struct NativeSdk::ThpState
{
  bool verify = false, ready = false, abandoned = false;
  std::array<double, 8> aan{};    // __THPAANScaleFactor
  std::array<u8, 80> natural{};   // __THPJpegNaturalOrder
  std::vector<u8> frame, y, u, v;  // last frame and its planes
  u32 width = 0, height = 0;
  u64 frames = 0, deferred = 0, verified = 0, mismatches = 0;
  u32 verify_lr = 0, verify_sp = 0;
  std::array<u32, 3> tiles{};
};
}  // namespace StaticRecompNativeSdk
