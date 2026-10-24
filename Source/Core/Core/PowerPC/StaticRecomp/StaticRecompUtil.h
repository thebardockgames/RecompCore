// RecompCore: StaticRecomp CPU core - small utilities of the port (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Byte swaps, saturation and the two checksums the native modules compare
// with what the chassis computed (Adler-32 of the resampling table, the
// "Ector" hash Dolphin uses to identify DSP microcode).

#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompUtil
{
constexpr u16 Swap16(u16 v)
{
  return static_cast<u16>((v >> 8) | (v << 8));
}

constexpr u32 Swap32(u32 v)
{
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

template <typename T, typename From>
constexpr T SaturatingCast(From value)
{
  return static_cast<T>(std::clamp<From>(value, static_cast<From>(std::numeric_limits<T>::min()),
                                         static_cast<From>(std::numeric_limits<T>::max())));
}

inline u32 Adler32(const u8* data, size_t len)
{
  constexpr u32 kMod = 65521;
  u32 a = 1, b = 0;
  for (size_t i = 0; i < len; ++i)
  {
    a = (a + data[i]) % kMod;
    b = (b + a) % kMod;
  }
  return (b << 16) | a;
}

inline u32 HashEctor(const u8* data, size_t len)
{
  u32 crc = 0;
  for (size_t i = 0; i < len; ++i)
  {
    crc ^= data[i];
    crc = (crc << 3) | (crc >> 29);
  }
  return crc;
}
}  // namespace StaticRecompUtil
