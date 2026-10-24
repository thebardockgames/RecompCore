// RecompCore: StaticRecomp CPU core - synchronous exceptions on the module's state (phase L4).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The delivery the 750CL makes for a pending synchronous exception (ISI,
// program, system call, FP unavailable, DSI, alignment) on the module's
// CPUState and the port's pending lines, as the chassis's
// PowerPCManager::CheckExceptions does: SRR0/SRR1, the MSR the handler runs
// with, the vector. After a burst the PC is the faulting instruction's, so
// SRR0 is the PC for every type.

#pragma once

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "core/cpu.h"

namespace StaticRecompExceptions
{
// Pending-exception bits (the chassis's Gekko.h values).
constexpr u32 kDecrementer = 0x00000001;
constexpr u32 kSyscall = 0x00000002;
constexpr u32 kExternalInt = 0x00000004;
constexpr u32 kDsi = 0x00000008;
constexpr u32 kIsi = 0x00000010;
constexpr u32 kAlignment = 0x00000020;
constexpr u32 kFpuUnavailable = 0x00000040;
constexpr u32 kProgram = 0x00000080;
constexpr u32 kPerformanceMonitor = 0x00000100;
constexpr u32 kFakeMemcheckHit = 0x00000200;
// Everything that is not an external interrupt: delivered after the burst.
constexpr u32 kSynchronousMask = ~(kExternalInt | kDecrementer | kPerformanceMonitor);

// false when nothing synchronous is pending.
inline bool DeliverSynchronous(CPUState& cpu, u32& pending)
{
  const u32 exceptions = pending;
  const u32 msr = cpu.msr;
  u32 srr1 = msr & 0x87C0FFFFu;
  u32 vector = 0;
  if (exceptions & kIsi)
  {
    vector = 0x00000400;
    srr1 |= 1u << 30;  // page fault
    pending &= ~kIsi;
  }
  else if (exceptions & kProgram)
  {
    vector = 0x00000700;
    srr1 |= cpu.srr1;  // partially set by the raiser (GenerateProgramException)
    pending &= ~kProgram;
  }
  else if (exceptions & kSyscall)
  {
    vector = 0x00000C00;
    pending &= ~kSyscall;
  }
  else if (exceptions & kFpuUnavailable)
  {
    vector = 0x00000800;
    pending &= ~kFpuUnavailable;
  }
  else if (exceptions & kFakeMemcheckHit)
  {
    pending &= ~kDsi & ~kFakeMemcheckHit;
    return true;
  }
  else if (exceptions & kDsi)
  {
    vector = 0x00000300;  // DSISR/DAR were set by the raiser
    pending &= ~kDsi;
  }
  else if (exceptions & kAlignment)
  {
    vector = 0x00000600;
    pending &= ~kAlignment;
  }
  else
  {
    return false;
  }
  cpu.srr0 = cpu.pc;
  cpu.srr1 = srr1;
  cpu.msr = ((msr & ~1u) | ((msr >> 16) & 1u)) & ~0x04EF36u;  // LE = ILE, clear as the 750CL
  cpu.pc = vector;
  return true;
}
}  // namespace StaticRecompExceptions
