// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeBat.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "core/cpu.h"

namespace StaticRecompMemory
{
inline bool IsCopyLoopPC(u32 pc)
{
  return pc == 0x80004350 || pc == 0x80004374;
}

// What the copy loop needs from its caller: the data BAT entry of an
// address, the slice counter, and whether the data cache is emulated or memory
// checks are set (both keep the PowerPC path).
struct CopyLoopEnv
{
  u32 (*dbat)(void* context, u32 address) = nullptr;
  void* dbat_context = nullptr;
  int* downcount = nullptr;
  bool dcache_enabled = false;
  bool mem_checks = false;
};

// Port the exact SDK byte-copy loop in bounded pieces. Preserve every live
// register and condition bit; return at the same branch boundary as PPC.
inline u32 TryRunCopyLoop(CPUState& cpu, const CopyLoopEnv& env)
{
  // MSR.DR (data translation) from the module's state.
  if (!IsCopyLoopPC(cpu.pc) || (cpu.msr & 0x10u) == 0 || env.dcache_enabled || env.mem_checks ||
      cpu.gpr[5] == 0 || !cpu.ram || cpu.ram_size < 0x4384 || !env.dbat || !env.downcount)
    return 0;
  const bool forward = cpu.pc == 0x80004350;
  const std::array<u32, 4> code = {
      forward ? 0x8c040001u : 0x8c04ffffu,
      forward ? 0x9c060001u : 0x9c06ffffu, 0x34a5ffff, 0x4082fff4};
  const u32 offset = cpu.pc & 0x3fffffff;
  for (size_t i = 0; i < code.size(); ++i)
  {
    const u8* bytes = cpu.ram + offset + i * 4;
    const u32 raw = (u32(bytes[0]) << 24) | (u32(bytes[1]) << 16) |
                    (u32(bytes[2]) << 8) | bytes[3];
    if (raw != code[i])
      return 0;
  }
  // Each of lbzu/stbu/addic./bc costs one cycle in the reference tables.
  int& downcount = *env.downcount;
  const s64 available = s64(downcount) + cpu.downcount;
  u32 count = std::min(cpu.gpr[5], u32(std::max<s64>(1, available / 4)));
  const u32 src_first = cpu.gpr[4] + (forward ? 1 : u32(-1));
  const u32 dst_first = cpu.gpr[6] + (forward ? 1 : u32(-1));
  constexpr u32 page_mask = StaticRecompNativeBat::kPageSize - 1;
  count = std::min(count, forward ? StaticRecompNativeBat::kPageSize - (src_first & page_mask) :
                                  (src_first & page_mask) + 1);
  count = std::min(count, forward ? StaticRecompNativeBat::kPageSize - (dst_first & page_mask) :
                                  (dst_first & page_mask) + 1);
  if (!forward && (cpu.gpr[4] < count || cpu.gpr[6] < count))
    return 0;
  const u32 src_start = forward ? src_first : cpu.gpr[4] - count;
  const u32 dst_start = forward ? dst_first : cpu.gpr[6] - count;
  const auto resolve = [&](u32 address) -> u8* {
    if (u64(address) + count > 0x100000000ull)
      return nullptr;
    const u32 bat = env.dbat(env.dbat_context, address);
    constexpr u32 flags = StaticRecompNativeBat::kMapped | StaticRecompNativeBat::kPhysical;
    if ((bat & (flags | StaticRecompNativeBat::kWi)) != flags)
      return nullptr;
    const u32 physical = (bat & StaticRecompNativeBat::kResultMask) | (address & page_mask);
    // Do not bypass instruction fetch semantics for self-modifying loops.
    if (address == dst_start && physical < offset + 16 && u64(physical) + count > offset)
      return nullptr;
    if (u64(physical) + count <= cpu.ram_size)
      return cpu.ram + physical;
    if (physical >= 0x10000000 && cpu.exram &&
        u64(physical - 0x10000000) + count <= cpu.exram_size)
      return cpu.exram + physical - 0x10000000;
    return nullptr;
  };
  u8* source = resolve(src_start);
  u8* destination = resolve(dst_start);
  if (!source || !destination)
    return 0;
  const auto src = reinterpret_cast<std::uintptr_t>(source);
  const auto dst = reinterpret_cast<std::uintptr_t>(destination);
  // Guest aliases can choose a direction that differs from host overlap.
  // Preserve such unusual sequential-copy behavior by keeping the PPC path.
  if ((forward && dst > src && dst - src < count) ||
      (!forward && src > dst && src - dst < count))
    return 0;
  const u8 last = source[forward ? count - 1 : 0];
  std::memmove(destination, source, count);
  const u32 delta = forward ? count : 0u - count;
  cpu.gpr[0] = last;
  cpu.gpr[4] += delta;
  cpu.gpr[6] += delta;
  cpu.gpr[5] -= count;
  cpu.xer |= 0x20000000;
  const u32 condition = cpu.gpr[5] == 0 ? 2 : (s32(cpu.gpr[5]) < 0 ? 8 : 4);
  cpu.cr = (cpu.cr & 0x0fffffff) | ((condition | (cpu.xer >> 31)) << 28);
  cpu.pc += cpu.gpr[5] == 0 ? 16 : 0;
  downcount -= int(count * 4);
  return count;
}
}  // namespace StaticRecompMemory
