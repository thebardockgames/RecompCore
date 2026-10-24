// RecompCore: StaticRecomp CPU core - what the native modules ask of the CPU (phase L4).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The CPU that runs the recompiled module, as the native modules see it: its
// data BAT table, its slice counter, and the native sections of its machine
// for checkpoints. The chassis's StaticRecompCore implements it (the oracle
// build) and so does the port's own runner; the modules never name either.

#pragma once

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompState
{
class StateStream;
}

namespace StaticRecompCpuHost
{
class CpuHost
{
public:
  virtual ~CpuHost() = default;
  // The data BAT entry for an effective address (StaticRecompNativeBat).
  virtual u32 Dbat(u32 ea) const = 0;
  // Rebuilds the data BAT table from the current BAT/HID4 registers.
  virtual void UpdateNativeBats() = 0;
  // The native sections of a checkpoint (the machine's DoState + the BATs).
  virtual void DoNativeState(StaticRecompState::StateStream& p, bool present) = 0;
  // Before writing a native checkpoint.
  virtual void SeedNativeDevicesForNativeState() = 0;
};

// The running CPU (nullptr before one exists).
inline CpuHost* g_cpu_host = nullptr;
}  // namespace StaticRecompCpuHost
