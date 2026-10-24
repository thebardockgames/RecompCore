// RecompCore: StaticRecomp CPU core - the burst loop (phase L3).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// One native burst: dispatches into the recompiled module on the module's
// CPUState and the port's register file until the PC leaves verified code,
// the slice's downcount runs out, a synchronous exception is raised or the
// run flag stops being "running". Per dispatch: the SDK's byte-copy loop or
// a native SDK function instead of the generated code when they apply, the
// module's cycle charge, the idle-loop skip, the time base, and the SDK's
// system call barrier on the module's state. The lockstep oracle and what
// still lives in the chassis are optional hooks.

#pragma once

#include <functional>
#include <unordered_map>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRegs.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompMemory.h"

namespace StaticRecompMachine
{
class Machine;
}
namespace StaticRecompCodeGuard
{
class CodeGuard;
}

namespace StaticRecompBurst
{
struct Context
{
  const StaticRecompModuleDesc* module = nullptr;
  CPUState* guest = nullptr;
  StaticRecompGuestRegs::GuestRegs* regs = nullptr;
  StaticRecompMachine::Machine* machine = nullptr;
  StaticRecompCodeGuard::CodeGuard* guard = nullptr;

  bool branch_batching = true;
  bool copy_enabled = true;
  bool pc_profile = false;
  u64 pc_profile_mask = 1023;
  u32 idle_pc = 0;
  u32 sync_exception_mask = 0;

  // Counters (the owner's).
  u64* dispatches = nullptr;
  u64* copy_bytes = nullptr;
  u64* copy_batches = nullptr;
  u64* charged_cycles = nullptr;
  u64* exceptions = nullptr;
  u64* kernel_calls = nullptr;
  std::unordered_map<u32, u64>* pc_samples = nullptr;

  // The lockstep oracle (only when `lockstep`).
  bool lockstep = false;
  std::function<bool(u32 pc)> lockstep_should_check;
  std::function<void(CPUState&)> lockstep_prepare;
  std::function<void(CPUState&)> lockstep_verify;
  // Before the copy loop: the chassis's MSR mirror kept in step.
  std::function<void()> before_copy;
  // The copy loop's environment (DBAT table, slice counter, data cache and
  // memory checks).
  std::function<StaticRecompMemory::CopyLoopEnv()> copy_env;
  // The idle skip when the native clock does not own the counter.
  std::function<void()> idle_fallback;
};

// `run_flag`: the run state the loop polls; it stops when *run_flag != running.
void Run(Context& context, bool module_active, const volatile int* run_flag, int running);
}  // namespace StaticRecompBurst
