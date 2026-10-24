// RecompCore: StaticRecomp CPU core - the CPU at power-on (phase L4).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Broadway's registers after reset, as the chassis's PowerPCManager::Reset
// leaves them for a Wii (PVR of the retail Broadway, HID1 for the 3x bus
// clock, the ECID words, DEC expired; everything else zero), and the native
// boot's result applied on top (MSR, HID bits, BATs, stack, entry point).

#pragma once

#include <array>

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeBoot.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompPowerOn
{
namespace Spr
{
constexpr u32 kDec = 22;
constexpr u32 kPvr = 287;
constexpr u32 kHid0 = 1008;
constexpr u32 kHid1 = 1009;
constexpr u32 kHid2 = 920;
constexpr u32 kHid4 = 1011;
constexpr u32 kEcidU = 924;
constexpr u32 kEcidM = 925;
constexpr u32 kEcidL = 926;
}  // namespace Spr

struct Registers
{
  u32 pc = 0;
  u32 msr = 0;
  std::array<u32, 32> gpr{};
  std::array<u32, 1024> spr{};
  std::array<u32, 16> sr{};
};

inline Registers BroadwayReset()
{
  Registers r;
  r.spr[Spr::kPvr] = 0x00087102;  // Broadway, retail
  r.spr[Spr::kHid1] = 0x80000000;  // 3x the bus clock
  r.spr[Spr::kEcidU] = 0x0d96e200;
  r.spr[Spr::kEcidM] = 0x1840c00d;
  r.spr[Spr::kEcidL] = 0x82bb08e8;
  r.spr[Spr::kDec] = 0xFFFFFFFF;
  return r;
}

inline void ApplyBoot(Registers& r, const StaticRecompNativeBoot::BootCpuState& cpu)
{
  r.msr = cpu.msr;
  r.spr[Spr::kHid0] |= cpu.hid0_set;
  r.spr[Spr::kHid2] |= cpu.hid2_set;
  r.spr[Spr::kHid4] |= cpu.hid4_set;
  for (const auto& [index, value] : cpu.sprs)
    r.spr[index] = value;
  r.gpr[1] = cpu.gpr1;
  r.gpr[3] = cpu.gpr3;
  r.pc = cpu.pc;
}
}  // namespace StaticRecompPowerOn
