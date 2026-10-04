// RecompCore: StaticRecomp CPU core - Memory and instruction fallback HLE hooks.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompKernel.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Core/HW/CPU.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace
{
constexpr u32 LOCKED_CACHE_BASE = 0xE0000000u;

// Diagnostics: MODERNGEKKO_NATIVE_EXTERNAL_STATS=1 counts the accesses that
// leave generated code for the MMU (by 4 KiB page and pc) and prints the
// busiest every 2^22 accesses.
struct ExternalStats
{
  bool enabled = [] {
    const char* v = std::getenv("MODERNGEKKO_NATIVE_EXTERNAL_STATS");
    return v != nullptr && v[0] == '1';
  }();
  std::unordered_map<u64, u64> counts;  // (write << 63) | (page << 32) | pc
  u64 total = 0;

  void Count(bool write, u32 ea, u32 pc)
  {
    ++counts[(u64(write) << 63) | (u64(ea >> 12) << 32) | pc];
    if ((++total & ((1u << 22) - 1)) != 0)
      return;
    std::vector<std::pair<u64, u64>> top(counts.begin(), counts.end());
    std::partial_sort(top.begin(), top.begin() + std::min<std::size_t>(12, top.size()), top.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "[external_stats] total=%llu\n", static_cast<unsigned long long>(total));
    for (std::size_t i = 0; i < std::min<std::size_t>(12, top.size()); ++i)
      std::fprintf(stderr, "[external_stats] %s page=0x%05X000 pc=0x%08X count=%llu\n",
                   (top[i].first >> 63) ? "write" : "read", static_cast<u32>((top[i].first >> 32) & 0x7FFFFFFF),
                   static_cast<u32>(top[i].first), static_cast<unsigned long long>(top[i].second));
    std::fflush(stderr);
    counts.clear();
  }
};
ExternalStats s_external_stats;

// The locked L1 cache (0xE0000000, 16 KiB): what MMU::ReadFromHardware /
// WriteToHardware do for it (big-endian bytes in Memory's L1 buffer), without
// the hardware dispatch around it. The THP movie decoder stores there millions
// of times per second; through the MMU the intro movie ran at 63 % speed.
// Same address assumption as HookExternalPointer (games use 0xE0000000).
u8* LockedCache(Memory::MemoryManager& memory, u32 ea, u32 size)
{
  const u32 offset = ea - LOCKED_CACHE_BASE;
  if (ea < LOCKED_CACHE_BASE || !memory.GetL1Cache() || offset + size > memory.GetL1CacheSize())
    return nullptr;
  return memory.GetL1Cache() + offset;
}
}

void StaticRecompCore::RefreshDispatchBudget(CPUState& cpu)
{
  if (!m_native_branch_batching || !m_module || m_module->cpu_abi_version < 3)
  {
    cpu.dispatch_budget = 0;
    return;
  }
  const auto& ppc = m_system.GetPPCState();
  constexpr u32 synchronous = ~static_cast<u32>(
      EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);
  cpu.dispatch_budget = (ppc.Exceptions & synchronous) ||
      m_lockstep_verifier->m_ls_journaling ||
      m_system.GetCPU().GetState() != CPU::State::Running ? 0 : std::max(0, ppc.downcount);
}

u64 StaticRecompCore::HookExternalRead(CPUState* cpu, u32 ea, u8 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  if (!core->m_lockstep_verifier->m_ls_journaling)
  {
    if (const u8* cache = LockedCache(core->m_system.GetMemory(), ea, size))
    {
      u64 value = 0;
      for (u8 i = 0; i < size; ++i)
        value = (value << 8) | cache[i];
      return value;
    }
  }
  const Common::ScopeGuard budget([&] { core->RefreshDispatchBudget(*cpu); });
  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  if (s_external_stats.enabled)
    s_external_stats.Count(false, ea, cpu->pc);
  u64 value;
  switch (size)
  {
  case 1:
    value = mmu.Read<u8>(ea);
    break;
  case 2:
    value = mmu.Read<u16>(ea);
    break;
  case 4:
    value = mmu.Read<u32>(ea);
    break;
  case 8:
    value = mmu.Read<u64>(ea);
    break;
  default:
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: external read of bad size {} at 0x{:08X}", size, ea);
    return 0;
  }
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_reads.push_back({ea, static_cast<u32>(value), size});
  }
  return value;
}

void StaticRecompCore::HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  if (!core->m_lockstep_verifier->m_ls_journaling)
  {
    if (u8* cache = LockedCache(core->m_system.GetMemory(), ea, size))
    {
      for (u8 i = 0; i < size; ++i)
        cache[i] = static_cast<u8>(value >> (8 * (size - 1 - i)));
      return;
    }
  }
  const Common::ScopeGuard budget([&] { core->RefreshDispatchBudget(*cpu); });

  // Gather-pipe fast path: stores to the write-gather pipe page at effective
  // 0xCC008000 go straight to GPFifo, mirroring the MMU's masked-write
  // special case without an MMU round trip. Keying on the effective page is
  // the same shortcut Dolphin's JITs take (optimizeGatherPipe). GPFifo
  // maintains ppc_state.gather_pipe_ptr internally.
  if ((ea & 0xFFFFF000) == 0xCC008000u)
  {
    if (core->m_lockstep_verifier->m_ls_journaling)
      core->m_lockstep_verifier->m_journal.native_mmio.push_back({ea, static_cast<u32>(value), size});
    auto& gpfifo = core->m_system.GetGPFifo();
    switch (size)
    {
    case 1:
      gpfifo.Write8(static_cast<u8>(value));
      return;
    case 2:
      gpfifo.Write16(static_cast<u16>(value));
      return;
    case 4:
      gpfifo.Write32(static_cast<u32>(value));
      return;
    default:
      for (u32 i = size * 8u; i > 0;)
      {
        i -= 8;
        gpfifo.Write8(static_cast<u8>(value >> i));
      }
      return;
    }
  }

  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  if (s_external_stats.enabled)
    s_external_stats.Count(true, ea, cpu->pc);
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_mmio.push_back({ea, static_cast<u32>(value), size});
  }
  switch (size)
  {
  case 1:
    mmu.Write<u8>(static_cast<u8>(value), ea);
    break;
  case 2:
    mmu.Write<u16>(static_cast<u16>(value), ea);
    break;
  case 4:
    mmu.Write<u32>(static_cast<u32>(value), ea);
    break;
  case 8:
    mmu.Write<u64>(value, ea);
    break;
  default:
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: external write of bad size {} at 0x{:08X}", size, ea);
    break;
  }
}

u32 StaticRecompCore::HookExternalRead32(CPUState* cpu, u32 ea, u8 rid)
{
  // eciwx external-control read. EAR-enable and alignment were checked by the
  // generated helper; Dolphin's interpreter services the access as a plain
  // MMU read (the rid is carried in EAR only).
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  const Common::ScopeGuard budget([&] { core->RefreshDispatchBudget(*cpu); });
  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  const u32 value = mmu.Read<u32>(ea);
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_reads.push_back({ea, value, 4});
  }
  return value;
}

void StaticRecompCore::HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid)
{
  // ecowx external-control write; see HookExternalRead32.
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  const Common::ScopeGuard budget([&] { core->RefreshDispatchBudget(*cpu); });
  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_mmio.push_back({ea, value, 4});
  }
  mmu.Write<u32>(value, ea);
}

void* StaticRecompCore::HookExternalPointer(CPUState* cpu, u32 ea, u32 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  // Lockstep journals locked-cache stores through the MMU: no direct access.
  if (core->m_lockstep_verifier->m_ls_journaling)
    return nullptr;
  auto& memory = core->m_system.GetMemory();
  if (ea >= LOCKED_CACHE_BASE && size != 0 &&
      (ea - LOCKED_CACHE_BASE) + size <= memory.GetL1CacheSize())
  {
    return memory.GetL1Cache() + (ea - LOCKED_CACHE_BASE);
  }
  // Everything else stays on the per-access MMU hooks: this hook receives
  // *effective* addresses, and whether one maps to RAM depends on live
  // MSR/BAT state that only the MMU can answer. Handing out a raw pointer
  // here would bypass MMIO and translation. (Memory::GetPointerForRange was
  // considered and rejected for exactly that reason.)
  return nullptr;
}

void StaticRecompCore::HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);

  // Lockstep: a block that fell back to the interpreter for an unmodeled
  // instruction (DMA mtspr, cache op, ...) performed side effects not captured
  // by the RAM journal / MMIO hooks, so re-running it on the shadow would
  // double-issue them. Mark it unsafe to differentially check.
  if (core->m_lockstep_verifier->m_ls_journaling)
    core->m_lockstep_verifier->m_ls_fallback_seen = true;

  auto& system = core->m_system;
  auto& ppc = system.GetPPCState();

  core->PropagateGuestMSR();
  if (StaticRecompKernel::TryRunDmaRegister(system, *cpu, raw, cia))
  {
    ++core->m_native_dma_register_instructions;
    return;
  }
  if (StaticRecompKernel::TryRunDecrementerWrite(system, *cpu, raw, cia))
  {
    ++core->m_native_decrementer_writes;
    return;
  }
  if (StaticRecompKernel::TryRunDecrementerRead(system, *cpu, raw, cia))
  {
    ++core->m_native_decrementer_reads;
    return;
  }
  if (const u32 count = StaticRecompKernel::TryRunCacheOperation(system, *cpu, raw, cia))
  {
    core->m_native_cache_instructions += count;
    ++core->m_native_cache_batches;
    return;
  }

  if (StaticRecompKernel::TryRunGatherPipeRegister(system, *cpu, raw, cia))
  {
    ++core->m_native_gather_register_instructions;
    return;
  }

  if (StaticRecompKernel::TryRunScratchRegister(system, *cpu, raw, cia))
  {
    ++core->m_native_scratch_register_instructions;
    return;
  }
  core->SyncOut();
  if (StaticRecompKernel::TryRunSystemRegister(system, raw, cia))
  {
    ++core->m_native_system_register_instructions;
    core->SyncIn();
    return;
  }
  ++core->m_hook_fallback_instructions;
  if (!core->m_native_pc_profile_path.empty())
    ++core->m_native_hook_samples[cia];
  // The recompiled segment resumes via the dispatcher at the PC this leaves
  // behind, so this must execute exactly the instruction at cia via
  // Dolphin's interpreter and hand the register state back.
  ppc.pc = cia;
  ppc.npc = cia + 4;
  ppc.downcount -= system.GetInterpreter().SingleStepInner();
  core->SyncIn();
}
