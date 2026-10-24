// RecompCore: StaticRecomp CPU core - native WUD library state.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal to StaticRecompNativeSdk.cpp / StaticRecompNativeWud.cpp.

#pragma once

#include <map>

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

namespace StaticRecompNativeSdk
{
struct NativeSdk::WudState
{
  bool active = false, abandoned = false;
  u64 inits = 0, repeated = 0, noops = 0;
  std::map<u32, u64> noop_calls;  // bte entry point -> calls answered natively
};
}  // namespace StaticRecompNativeSdk
