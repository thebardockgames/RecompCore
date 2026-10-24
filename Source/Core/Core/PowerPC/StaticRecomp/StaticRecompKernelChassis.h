// RecompCore: StaticRecomp CPU core - chassis adapter of the kernel (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The KernelHost of the chassis (its MMU tables, instruction cache,
// performance monitor, gather pipe, translated code and decrementer), the
// two operations that run on the chassis's PowerPC state when it holds the
// registers (system registers, the SDK system call barrier), and the
// System-based forms the unit tests use. Checks the kernel's cycle counts
// against the chassis's instruction table once.

#pragma once

#include <span>

#include "Core/PowerPC/StaticRecomp/StaticRecompKernel.h"

namespace Core
{
class System;
}

namespace StaticRecompKernelChassis
{
// The host for `system` (built on first use).
const StaticRecompKernel::KernelHost& Host(Core::System& system);
// The chassis's PowerPCState as the register file (chassis authority and the
// unit tests, whose harness has no core).
StaticRecompKernel::RegsView ChassisRegs(Core::System& system);
// On the chassis's state (the registers are the chassis's).
bool TryRunSystemRegister(Core::System& system, u32 raw, u32 cia);
bool TryRunSystemCall(Core::System& system, std::span<const u8> ram);
// The kernel's cycle table against the chassis's (true when identical).
bool CyclesMatchChassis();
}  // namespace StaticRecompKernelChassis

// The System-based forms (unit tests).
namespace StaticRecompKernel
{
inline RegsView ChassisRegs(Core::System& system)
{
  return StaticRecompKernelChassis::ChassisRegs(system);
}
inline bool TryRunSystemRegister(Core::System& system, u32 raw, u32 cia)
{
  return StaticRecompKernelChassis::TryRunSystemRegister(system, raw, cia);
}
inline bool TryRunSystemCall(Core::System& system, std::span<const u8> ram)
{
  return StaticRecompKernelChassis::TryRunSystemCall(system, ram);
}
#define SR_KERNEL_SYSTEM_FORM(name)                                                                \
  inline auto name(Core::System& system, CPUState& cpu, RegsView regs, u32 raw, u32 cia)          \
  {                                                                                                \
    return name(StaticRecompKernelChassis::Host(system), cpu, regs, raw, cia);                     \
  }
SR_KERNEL_SYSTEM_FORM(TryRunSystemRegister)
SR_KERNEL_SYSTEM_FORM(TryRunDmaRegister)
SR_KERNEL_SYSTEM_FORM(TryRunDecrementerRead)
SR_KERNEL_SYSTEM_FORM(TryRunDecrementerWrite)
SR_KERNEL_SYSTEM_FORM(TryRunScratchRegister)
SR_KERNEL_SYSTEM_FORM(TryRunGatherPipeRegister)
SR_KERNEL_SYSTEM_FORM(TryRunCacheOperation)
#undef SR_KERNEL_SYSTEM_FORM
}  // namespace StaticRecompKernel
