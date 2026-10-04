// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompKernel.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompMemory.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>

namespace
{
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);
}

void StaticRecompCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);
  // ABI v4 modules make direct native calls across chunks only into chunks
  // this guard has verified. The vector is sized once per module load.
  m_guest.chunk_state = m_module && !m_chunk_state.empty() ? m_chunk_state.data() : nullptr;

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (!m_module_active && m_fallback_jit)
  {
    ++m_fallback_jit_runs;
    std::fprintf(stderr, "[staticrecomp] entering JIT fallback: pc=0x%08x module_active=0\n", ppc.pc);
    m_fallback_jit->Run();
    return;
  }

  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();
    const std::string current_game_id = SConfig::GetInstance().GetGameID();
    m_module_active = m_module && (current_game_id.empty() || current_game_id == m_module->game_id);

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && DispatchableAt(ppc.pc))
      {
        SyncIn();
        ++m_bursts;
        do
        {
          const bool do_ls = m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          if (!m_native_pc_profile_path.empty() && (m_native_dispatches & m_native_pc_profile_mask) == 0)
            ++m_native_pc_samples[m_guest.pc];
          const int downcount_before_dispatch = ppc.downcount;
          m_guest.dispatch_budget = do_ls || !m_native_branch_batching ||
              m_module->cpu_abi_version < 3 ? 0 : std::max(0, ppc.downcount);
          u32 copied = 0;
          if (!do_ls && m_native_copy_enabled && StaticRecompMemory::IsCopyLoopPC(m_guest.pc))
          {
            PropagateGuestMSR();
            copied = StaticRecompMemory::TryRunCopyLoop(m_system, m_guest);
          }
          if (copied)
          {
            m_native_copy_bytes += copied;
            ++m_native_copy_batches;
          }
          else if (!(m_native_sdk && !do_ls && m_native_sdk->Run(m_guest)))
          {
            m_module->dispatch(&m_guest, m_guest.pc);
            if (m_native_sdk)
              m_native_sdk->AfterDispatch(m_guest);
          }
          ++m_native_dispatches;

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }

          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 unless a chassis hook already
          // charged the dispatch, so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher timing check. ABI3 local loops stop at a backward
          // branch when their cycle budget expires (or after 64 backedges),
          // then CoreTiming can deliver pending events. ABI2 returns at
          // every local backward branch.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          const int module_charge = static_cast<int>(
              charge > 0 ? charge : (ppc.downcount == downcount_before_dispatch ? 1 : 0));
          ppc.downcount -= module_charge;
          m_charged_cycles += static_cast<u64>(downcount_before_dispatch - ppc.downcount);

          // Idle loop skipping for configured target loops (e.g. Wii Menu OSIdleThread)
          if (m_guest.pc == m_idle_pc && m_idle_pc != 0)
          {
            m_system.GetCoreTiming().Idle();
          }

          // CoreTiming includes the consumed downcount, the TB/CPU clock
          // ratio, and any skipped idle cycles. CPU cycles cannot be added
          // directly to TB: its clock is twelve times slower.
          m_guest.timebase = m_system.GetSystemTimers().GetFakeTimeBase();
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
        } while (m_module_active && FastDispatchableAt(m_guest.pc) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
      }
      else
      {
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        if (m_module_active && StaticRecompKernel::TryRunSystemCall(
                                  m_system, {memory.GetRAM(), memory.GetRamSizeReal()}))
        {
          ++m_native_kernel_calls;
        }
        else if (m_fallback_jit)
        {
          ++m_fallback_jit_runs;
          std::fprintf(stderr, "[staticrecomp] entering JIT fallback: pc=0x%08x module_active=%d\n",
                       ppc.pc, m_module_active ? 1 : 0);
          m_fallback_jit->Run();
        }
        else
        {
          if (m_fallback_steps == 0)
          {
            std::fprintf(stderr, "[staticrecomp] entering bounded interpreter fallback: pc=0x%08x\n", ppc.pc);
            if (const char* path = std::getenv("MODERNGEKKO_NATIVE_CPU_MISS_DUMP"))
            {
              const u32 offset = ppc.pc & 0x01ffffffu;
              if (offset + 256 <= memory.GetRamSizeReal())
              {
                if (auto* file = std::fopen(path, "wb"))
                {
                  std::fwrite(memory.GetRAM() + offset, 1, 256, file);
                  std::fclose(file);
                }
              }
            }
          }
          do
          {
            if (!m_native_pc_profile_path.empty())
            {
              const u64 samples = ++m_native_fallback_samples[ppc.pc];
              // OSInit installs exception code outside DOL text. Capture the
              // actual patched vectors once, on the CPU thread, so future AOT
              // coverage uses observed instructions rather than guessed SDK
              // templates. This is diagnostic only and never changes RAM.
              if (samples == 1 && (ppc.pc == 0x500 || ppc.pc == 0x800 || ppc.pc == 0x900))
              {
                if (const char* path = std::getenv("MODERNGEKKO_NATIVE_CPU_MISS_DUMP"))
                {
                  if (ppc.pc + 256 <= memory.GetRamSizeReal())
                  {
                    char suffix[32];
                    std::snprintf(suffix, sizeof(suffix), ".vector-%08x.bin", ppc.pc);
                    const std::string vector_path = std::string(path) + suffix;
                    if (auto* file = std::fopen(vector_path.c_str(), "wb"))
                    {
                      std::fwrite(memory.GetRAM() + ppc.pc, 1, 256, file);
                      std::fclose(file);
                    }
                  }
                }
              }
            }
            ppc.downcount -= interpreter.SingleStepInner();
            ++m_fallback_steps;
          } while (!(m_module_active && DispatchableAt(ppc.pc)) && ppc.downcount > 0 &&
                   *state_ptr == CPU::State::Running);
        }
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
