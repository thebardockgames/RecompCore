// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompKernelChassis.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRam.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePi.h"
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
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeState.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeStateChassis.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompMemory.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompMemoryChassis.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompBurst.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <string>

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
  const bool audit_callback_guest = std::getenv("MODERNGEKKO_NATIVE_CALLBACK_GUEST_AUDIT") != nullptr;

  // Phase S: the guest memory is the port's. If the runtime lent nothing (a
  // tool booting the chassis alone), adopt the chassis's buffers, non-owning.
  auto& guest_ram = StaticRecompGuestRam::GuestRam::Instance();
  if (!guest_ram.Allocated() || guest_ram.Mem1() != memory.GetRAM())
  {
    std::fprintf(stderr, "[guest_ram] %s: adopting the chassis's buffers\n",
                 guest_ram.Allocated() ? "the chassis did not map the lent memory" :
                                         "no memory was lent");
    guest_ram.Adopt(memory.GetRAM(), memory.GetRamSizeReal(), memory.GetEXRAM(),
                    memory.GetExRamSizeReal(), memory.GetL1Cache(), memory.GetL1CacheSize());
  }
  m_guest.ram = guest_ram.Mem1();
  m_guest.ram_size = guest_ram.Mem1Size();
  m_guest.exram = guest_ram.Mem2();
  m_guest.exram_size = guest_ram.Mem2Size();
  m_code_guard.InitLookup(m_guest.ram_size, m_guest.exram_size);
  // ABI v4 modules make direct native calls across chunks only into chunks
  // this guard has verified. The vector is sized once per module load.
  m_guest.chunk_state = m_module ? m_code_guard.StateData() : nullptr;

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (!m_module_active && m_fallback_jit)
  {
    ++m_fallback_jit_runs;
    std::fprintf(stderr, "[staticrecomp] entering JIT fallback: pc=0x%08x module_active=0\n", ppc.pc);
    m_fallback_jit->Run();
    return;
  }

  // Phase G diagnostic: MODERNGEKKO_BOOT_DUMP=<prefix> writes MEM1 and the CPU
  // registers as the boot left them, the first time the CPU loop runs, so a
  // native boot can be compared byte for byte with the chassis's.
  if (const char* dump = std::getenv("MODERNGEKKO_BOOT_DUMP"); dump && !m_boot_dumped)
  {
    m_boot_dumped = true;
    const std::string prefix(dump);
    if (auto* file = std::fopen((prefix + ".mem1").c_str(), "wb"))
    {
      std::fwrite(m_guest.ram, 1, m_guest.ram_size, file);
      std::fclose(file);
    }
    if (auto* file = std::fopen((prefix + ".regs.txt").c_str(), "wb"))
    {
      std::fprintf(file, "pc %08x npc %08x msr %08x\n", ppc.pc, ppc.npc, ppc.msr.Hex);
      for (int i = 0; i < 32; ++i)
        std::fprintf(file, "gpr%d %08x\n", i, ppc.gpr[i]);
      for (int i = 0; i < 1024; ++i)
        if (ppc.spr[i])
          std::fprintf(file, "spr%d %08x\n", i, ppc.spr[i]);
      for (int i = 0; i < 16; ++i)
        std::fprintf(file, "sr%d %08x\n", i, ppc.sr[i]);
      std::fclose(file);
    }
    std::fprintf(stderr, "[staticrecomp] boot dump written to %s.mem1 (pc=%08x)\n", dump, ppc.pc);
  }
  // Phase S: a native checkpoint requested at start (--load-state x.mgs) is
  // loaded here, on the CPU thread before anything runs; the chassis's
  // savestate machinery is not involved.
  if (std::string pending; StaticRecompNativeState::TakePendingLoad(&pending))
  {
    if (StaticRecompNativeStateChassis::Load(m_system, pending))
    {
      ClearCache();  // reverify the module against the loaded memory
      m_guest.ram = guest_ram.Mem1();
    }
    else
    {
      std::fprintf(stderr, "[native_state] load failed: the game continues from the boot\n");
    }
  }
  // MODERNGEKKO_CONVERT_STATE=<out.mgs>: write a native checkpoint of the state
  // the run starts from (a chassis .sav loaded at boot), at the same instant,
  // so both describe the same frame.
  if (const char* convert = std::getenv("MODERNGEKKO_CONVERT_STATE"); convert && !m_state_converted)
  {
    m_state_converted = true;
    if (StaticRecompNativeStateChassis::Save(m_system, convert))
      std::fprintf(stderr, "[native_state] converted the starting state to %s\n", convert);
  }

  // Phase S: the chassis's PowerPCState is authoritative on entry (boot,
  // savestate load, debugger); native bursts run on m_guest and the mirror is
  // refreshed only at chassis boundaries. Lockstep and the chassis's own
  // exception delivery need the mirror current after every burst.
  m_sync_every_burst = m_lockstep_verifier->IsEnabled() ||
                       !(m_machine.clock && m_machine.clock->OwnsExceptions()) ||
                       std::getenv("MODERNGEKKO_NATIVE_SYNC_EVERY_BURST") != nullptr;
  m_authority = StateAuthority::Chassis;
  if (m_machine.clock)
    m_machine.clock->SetCpuState(nullptr, nullptr);
  if (m_machine.pi)
    m_machine.pi->SetGuestRegs(nullptr);
  if (const char* setting = std::getenv("MODERNGEKKO_NATIVE_SYNC_EXCEPTIONS"))
    m_native_sync_exceptions = !(std::string(setting) == "off" || std::string(setting) == "0");

  UpdateNativeBats();  // the BATs the boot (or the loaded state) left

  // The burst loop (phase L3), with the chassis's pieces as its hooks.
  static_assert(sizeof(CPU::State) == sizeof(int));
  StaticRecompBurst::Context burst;
  burst.module = m_module;
  burst.guest = &m_guest;
  burst.regs = &m_regs;
  burst.machine = &m_machine;
  burst.guard = &m_code_guard;
  burst.branch_batching = m_native_branch_batching;
  burst.copy_enabled = m_native_copy_enabled;
  burst.pc_profile = !m_native_pc_profile_path.empty();
  burst.pc_profile_mask = m_native_pc_profile_mask;
  burst.idle_pc = m_idle_pc;
  burst.sync_exception_mask = SYNC_EXCEPTION_MASK;
  burst.dispatches = &m_native_dispatches;
  burst.copy_bytes = &m_native_copy_bytes;
  burst.copy_batches = &m_native_copy_batches;
  burst.charged_cycles = &m_charged_cycles;
  burst.exceptions = &m_native_exceptions;
  burst.kernel_calls = &m_native_kernel_calls;
  burst.pc_samples = &m_native_pc_samples;
  burst.lockstep = m_lockstep_verifier->IsEnabled();
  burst.lockstep_should_check = [this](u32 pc) { return m_lockstep_verifier->ShouldCheck(pc); };
  burst.lockstep_prepare = [this](CPUState& cpu) { m_lockstep_verifier->Prepare(cpu); };
  burst.lockstep_verify = [this](CPUState& cpu) { m_lockstep_verifier->Verify(cpu); };
  burst.before_copy = [this] { PropagateGuestMSR(); };
  burst.copy_env = [this] { return StaticRecompMemoryChassis::Env(m_system, m_guest); };
  burst.idle_fallback = [this] { m_system.GetCoreTiming().Idle(); };

  while (*state_ptr == CPU::State::Running)
  {
    if(audit_callback_guest && m_machine.sdk)m_machine.sdk->AuditCallbackGuest(m_guest);
    core_timing.Advance();
    const std::string current_game_id = SConfig::GetInstance().GetGameID();
    m_module_active = m_module && (current_game_id.empty() || current_game_id == m_module->game_id);

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      const u32 current_pc = m_authority == StateAuthority::Guest ? m_guest.pc : ppc.pc;
      if (m_module_active && DispatchableAt(current_pc))
      {
        EnterGuest();
        ++m_bursts;
        StaticRecompBurst::Run(burst, m_module_active, reinterpret_cast<const volatile int*>(state_ptr),
                               static_cast<int>(CPU::State::Running));
        if ((m_regs.exceptions & SYNC_EXCEPTION_MASK) != 0)
        {
          // On the module's state (phase S); the chassis's delivery as the A/B.
          if (!m_native_sync_exceptions || !DeliverSynchronousException())
          {
            EnterChassis();
            power_pc.CheckExceptions();
          }
        }
        else if (m_sync_every_burst)
        {
          EnterChassis();
        }
      }
      else
      {
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        EnterChassis();
        if (m_module_active && StaticRecompKernelChassis::TryRunSystemCall(
                                  m_system, {m_guest.ram, m_guest.ram_size}))
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
              if (offset + 256 <= m_guest.ram_size)
              {
                if (auto* file = std::fopen(path, "wb"))
                {
                  std::fwrite(m_guest.ram + offset, 1, 256, file);
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
                  if (ppc.pc + 256 <= m_guest.ram_size)
                  {
                    char suffix[32];
                    std::snprintf(suffix, sizeof(suffix), ".vector-%08x.bin", ppc.pc);
                    const std::string vector_path = std::string(path) + suffix;
                    if (auto* file = std::fopen(vector_path.c_str(), "wb"))
                    {
                      std::fwrite(m_guest.ram + ppc.pc, 1, 256, file);
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
    } while (Downcount() > 0 && *state_ptr == CPU::State::Running);
  }
  // Leaving the core (pause, savestate, shutdown): the chassis sees the state.
  EnterChassis();
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
