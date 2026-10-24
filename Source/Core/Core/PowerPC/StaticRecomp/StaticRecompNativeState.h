// RecompCore: StaticRecomp CPU core - native checkpoints (phase S, step 1).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Our own checkpoint format (".mgs"): the CPU registers, the cycle counter
// and time base, MEM1/MEM2 and the locked cache, the graphics state and the
// native sections (SDK layer, GP front, VI, clock, PI). Nothing of the
// chassis's device emulation is stored: with phases H1-G every device the
// game touches is native, and what the chassis still constructs at boot is
// left as the boot leaves it. Loading needs no inserted disc, no IOS kernel
// of a particular version and no Dolphin savestate code.
//
// The registers and the counter come from / go to whoever holds them through
// ChassisHooks (StaticRecompNativeStateChassis: the chassis's PowerPC state
// and CoreTiming while the chassis exists); the native sections are written
// with the port's state stream.

#pragma once

#include <functional>
#include <string>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompNativeState
{
// ".mgs" paths are ours; anything else is the chassis's savestate.
bool IsNativeStatePath(const std::string& path);

// The registers of the CPU, field by field (the file's layout).
struct CpuRecord
{
  u32 pc, npc;
  u32 gpr[32];
  u64 ps[32][2];
  u64 cr[8];
  u32 msr, fpscr, exceptions;
  u32 xer_ca, xer_so_ov, xer_stringctrl;
  u32 reserve_address, reserve;
  u32 sr[16];
  u32 spr[1024];
};

struct ClockRecord
{
  s64 ticks;  // cycle counter at the save point
  u64 tb_start_value, tb_start_ticks;
};

struct ChassisHooks
{
  std::function<void(CpuRecord& cpu)> get_cpu;
  // Also restarts the slice and updates everything derived from the
  // registers (MSR, rounding, BATs).
  std::function<void(const CpuRecord& cpu)> set_cpu;
  std::function<void(ClockRecord& clock)> get_clock;
  std::function<void(const ClockRecord& clock)> set_clock;
  std::function<std::string()> game_id;
};
void SetChassisHooks(ChassisHooks hooks);

// Both run on the CPU thread between slices.
bool Save(const std::string& path);
bool Load(const std::string& path);

// A load requested before the CPU loop runs (the runner's --load-state); the
// core takes it at its first iteration.
void SetPendingLoad(std::string path);
bool TakePendingLoad(std::string* path);
}  // namespace StaticRecompNativeState
