// RecompCore: StaticRecomp CPU core - the burst loop (phase L3).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompBurst.h"

#include <algorithm>

#include "Core/PowerPC/StaticRecomp/StaticRecompCodeGuard.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompKernel.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompMachine.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

namespace StaticRecompBurst
{
void Run(Context& c, bool module_active, const volatile int* run_flag, int running)
{
  CPUState& guest = *c.guest;
  StaticRecompGuestRegs::GuestRegs& regs = *c.regs;
  StaticRecompNativeSdk::NativeSdk* const sdk = c.machine->sdk.get();
  do
  {
    const bool do_ls = c.lockstep && c.lockstep_should_check(guest.pc);
    if (do_ls)
      c.lockstep_prepare(guest);

    if (c.pc_profile && (*c.dispatches & c.pc_profile_mask) == 0)
      ++(*c.pc_samples)[guest.pc];
    const int downcount_before_dispatch = regs.downcount;
    guest.dispatch_budget = do_ls || !c.branch_batching || c.module->cpu_abi_version < 3 ?
                                0 :
                                std::max(0, regs.downcount);
    u32 copied = 0;
    if (!do_ls && c.copy_enabled && StaticRecompMemory::IsCopyLoopPC(guest.pc))
    {
      if (c.before_copy)
        c.before_copy();
      copied = StaticRecompMemory::TryRunCopyLoop(guest, c.copy_env());
    }
    if (copied)
    {
      *c.copy_bytes += copied;
      ++*c.copy_batches;
    }
    else if (!(sdk && !do_ls && sdk->Run(guest)))
    {
      c.module->dispatch(&guest, guest.pc);
      if (sdk)
        sdk->AfterDispatch(guest);
    }
    ++*c.dispatches;

    if (do_ls)
      c.lockstep_verify(guest);

    // Flush the module's per-block cycle charges into the slice counter. A
    // dispatch that charged nothing (PC-switch default, pure embedded data)
    // still costs 1 unless a hook already charged the dispatch, so the burst
    // always makes progress; this per-dispatch flush is also the timing
    // check. ABI3 local loops stop at a backward branch when their cycle
    // budget expires (or after 64 backedges), then the clock can deliver
    // pending events. ABI2 returns at every local backward branch.
    const s64 charge = -guest.downcount;
    guest.downcount = 0;
    const int module_charge =
        static_cast<int>(charge > 0 ? charge : (regs.downcount == downcount_before_dispatch ? 1 : 0));
    regs.downcount -= module_charge;
    *c.charged_cycles += static_cast<u64>(downcount_before_dispatch - regs.downcount);

    // Idle loop skipping for configured target loops (the OS idle thread).
    if (guest.pc == c.idle_pc && c.idle_pc != 0)
    {
      auto* clock = StaticRecompNativeClock::NativeClock::Instance();
      if (clock && clock->OwnsCounter())
        clock->Idle();
      else if (c.idle_fallback)
        c.idle_fallback();
    }

    // The time base includes the consumed downcount, the TB/CPU clock ratio
    // and any skipped idle cycles (TB runs twelve times slower than the CPU).
    guest.timebase = StaticRecompNativeClock::NativeClock::CurrentTimeBase();
    if (guest.exception)
    {
      // The module's runtime already redirected pc/msr/srr to the guest
      // exception vector; the flag only signals that it happened.
      guest.exception = 0;
      guest.program_exception = 0;
      ++*c.exceptions;
      // The SDK's system-call barrier (the one guest exception of the combat
      // loop, ~36 000 per second) runs here on the module's state.
      if (guest.pc == 0xC00 && (regs.exceptions & c.sync_exception_mask) == 0 &&
          StaticRecompKernel::TryRunSystemCallNative(guest, regs.spr, {guest.ram, guest.ram_size}))
      {
        const int cycles = StaticRecompKernel::SystemCallCycles();
        regs.downcount -= cycles;
        *c.charged_cycles += static_cast<u64>(cycles);
        ++*c.kernel_calls;
      }
    }
    if ((regs.exceptions & c.sync_exception_mask) != 0)
      break;  // A hook-raised synchronous exception: the owner delivers it.
  } while (module_active && c.guard->FastDispatchable(guest.pc, guest.msr, module_active) &&
           regs.downcount > 0 && *run_flag == running);
}
}  // namespace StaticRecompBurst
