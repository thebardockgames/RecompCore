// RecompCore: StaticRecomp CPU core - data BAT translation table (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The block address translation the native layer uses to turn the guest's
// effective addresses into MEM1/MEM2 pointers (SDK hooks, native copy loop),
// built from the game's DBAT registers with the chassis's rules: one entry
// per 128 KiB page holding the physical page base plus MAPPED (a BAT covers
// it), PHYSICAL (the physical page is RAM, MEM2 or the locked cache) and WI
// (write-through or cache-inhibited: not a plain memory page). Rebuilt when
// the game writes a BAT or HID4 (extended BATs), at run start and after a
// state load. Debugger memchecks and the chassis's fake MMU regions are not
// modelled (neither exists in the port).

#pragma once

#include <array>
#include <cstdint>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompNativeBat
{
constexpr int kIndexShift = 17;
constexpr u32 kPageSize = 1u << kIndexShift;
constexpr u32 kPageCount = 1u << (32 - kIndexShift);
constexpr u32 kMapped = 0x1;
constexpr u32 kPhysical = 0x2;
constexpr u32 kWi = 0x4;
constexpr u32 kResultMask = ~0x7u;

class NativeBat
{
public:
  NativeBat() { m_table.fill(0); }

  // `spr` is the 1024-entry SPR file; DBAT0-3 at 536.., DBAT4-7 at 568.. when
  // `extended` (HID4.SBE) is set.
  void Update(const u32* spr, bool extended, u32 ram_size, u32 exram_size, u32 l1_size)
  {
    m_table.fill(0);
    UpdateRange(spr, 536, ram_size, exram_size, l1_size);
    if (extended)
      UpdateRange(spr, 568, ram_size, exram_size, l1_size);
    ++m_updates;
  }
  u32 Entry(u32 ea) const { return m_table[ea >> kIndexShift]; }
  u64 Updates() const { return m_updates; }

private:
  void UpdateRange(const u32* spr, u32 base_spr, u32 ram_size, u32 exram_size, u32 l1_size)
  {
    for (u32 i = 0; i < 4; ++i)
    {
      const u32 batu = spr[base_spr + i * 2];
      const u32 batl = spr[base_spr + i * 2 + 1];
      const u32 vp = batu & 1, vs = (batu >> 1) & 1;
      const u32 bl = (batu >> 2) & 0x7FF;
      const u32 bepi = batu >> 17;
      const u32 brpn = batl >> 17;
      const u32 wimg = (batl >> 3) & 0xF;
      if (vs == 0 && vp == 0)
        continue;
      for (u32 j = 0; j <= bl; ++j)
      {
        if ((j & bl) != j)
          continue;
        const u32 physical = (brpn | j) << kIndexShift;
        const u32 effective = (bepi | j) << kIndexShift;
        u32 valid = kMapped;
        const bool wi = (wimg & 0b1100) != 0;
        if (wi)
          valid |= kWi;
        else if (physical < ram_size)
          valid |= kPhysical;
        else if (exram_size && (physical >> 28) == 0x1 && (physical & 0x0FFFFFFF) < exram_size)
          valid |= kPhysical;
        else if ((physical >> 28) == 0xE && physical < 0xE0000000u + l1_size)
          valid |= kPhysical;
        m_table[effective >> kIndexShift] = physical | valid;
      }
    }
  }

  std::array<u32, kPageCount> m_table;
  u64 m_updates = 0;
};
}  // namespace StaticRecompNativeBat
