// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <span>
#include <algorithm>
#include "Common/CommonTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"

namespace StaticRecompAddress
{
// Physical and virtual code may share bytes but have different branch PCs.
// Keep their dispatch entries in separate banks.
inline int LookupIndex(u32 address, u32 ram_size, u32 exram_size)
{
  if (address >= 0x80000000u && address - 0x80000000u < ram_size)
    return static_cast<int>((address - 0x80000000u) >> 2);
  if (address >= 0x90000000u && address - 0x90000000u < exram_size)
    return static_cast<int>((ram_size >> 2) + ((address - 0x90000000u) >> 2));
  if (address < ram_size)
    return static_cast<int>(((ram_size + exram_size) >> 2) + (address >> 2));
  return -1;
}

inline std::optional<u32> RAMOffset(u32 address, u32 length, u32 ram_size)
{
  const u32 offset = address >= 0x80000000u ? address - 0x80000000u : address;
  if (offset >= ram_size || length > ram_size - offset)
    return std::nullopt;
  return offset;
}

template <typename Callback>
void ForEachInvalidatedChunk(std::span<const StaticRecompRange> chunks, u32 address,
                             u32 length, u32 physical_address, u32 ram_size,
                             Callback callback)
{
  if (!length) return;
  const auto range = [&](u32 first, u64 size) {
    const u64 last = u64(first) + size - 1;
    size_t lo = 0, hi = chunks.size();
    while (lo < hi)
    {
      const size_t mid = lo + (hi - lo) / 2;
      if (chunks[mid].end <= first) lo = mid + 1;
      else hi = mid;
    }
    for (size_t i = lo; i < chunks.size() && chunks[i].start <= last; ++i)
      callback(i);
  };
  range(address, length);
  if (physical_address < ram_size)
  {
    const u64 size = std::min<u64>(length, ram_size - physical_address);
    if (physical_address != address) range(physical_address, size);
    if (0x80000000u + physical_address != address)
      range(0x80000000u + physical_address, size);
  }
}
}
