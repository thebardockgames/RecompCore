// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <algorithm>
#include <span>

#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "core/cpu.h"

namespace StaticRecompKernel
{
// Rare SDK initialization operations use the chassis state after SyncOut.
// Keep their cache/MMU/feature side effects without entering the interpreter.
inline bool TryRunSystemRegister(Core::System& system, u32 raw, u32 cia)
{
  const u32 xo = (raw >> 1) & 0x3ff;
  const u32 index = ((raw >> 16) & 31) | ((raw >> 6) & 0x3e0);
  const bool ibat = (index >= SPR_IBAT0U && index <= SPR_IBAT3L) ||
                    (index >= SPR_IBAT4U && index <= SPR_IBAT7L);
  const bool dbat = (index >= SPR_DBAT0U && index <= SPR_DBAT3L) ||
                    (index >= SPR_DBAT4U && index <= SPR_DBAT7L);
  const bool plain = index == SPR_L2CR || index == SPR_PMC1 || index == SPR_PMC2 ||
                     index == SPR_PMC3 || index == SPR_PMC4;
  auto& ppc = system.GetPPCState();
  if ((raw >> 26) != 31 || (xo != 339 && xo != 467) || ppc.msr.PR ||
      (!ibat && !dbat && !plain && index != SPR_HID0 && index != SPR_HID4 &&
       index != SPR_MMCR0 && index != SPR_MMCR1))
    return false;
  const u32 reg = (raw >> 21) & 31;
  if (xo == 339)
    ppc.gpr[reg] = ppc.spr[index];
  else
  {
    const u32 old = ppc.spr[index];
    ppc.spr[index] = ppc.gpr[reg];
    if (ibat && old != ppc.spr[index]) system.GetMMU().IBATUpdated();
    else if (dbat && old != ppc.spr[index]) system.GetMMU().DBATUpdated();
    else if (index == SPR_HID4 && old != ppc.spr[index])
    {
      system.GetMMU().IBATUpdated();
      system.GetMMU().DBATUpdated();
    }
    else if (index == SPR_HID0 && HID0(ppc).ICFI)
    {
      HID0(ppc).ICFI = 0;
      ppc.iCache.Reset(system.GetJitInterface());
    }
    else if (index == SPR_MMCR0 || index == SPR_MMCR1)
      PowerPC::MMCRUpdated(ppc);
  }
  ppc.pc = cia + 4;
  ppc.npc = cia + 4;
  ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles;
  return true;
}

inline bool TryRunDmaRegister(Core::System& system, CPUState& cpu, u32 raw, u32 cia)
{
  const u32 xo = (raw >> 1) & 0x3ff;
  const u32 index = ((raw >> 16) & 31) | ((raw >> 6) & 0x3e0);
  if ((raw >> 26) != 31 || (xo != 339 && xo != 467) ||
      (index != SPR_DMAU && index != SPR_DMAL) || (cpu.msr & 0x4000))
    return false;
  auto& ppc = system.GetPPCState();
  const u32 reg = (raw >> 21) & 31;
  if (xo == 339)
    cpu.gpr[reg] = ppc.spr[index];
  else
  {
    UReg_DMAL lower(cpu.gpr[reg]);
    const UReg_DMAU upper(ppc.spr[SPR_DMAU]);
    u32 blocks = (upper.DMA_LEN_U << 2) | lower.DMA_LEN_L;
    if (!blocks) blocks = 128;
    const u32 cache = lower.LC_ADDR << 5;
    if (index == SPR_DMAL && lower.DMA_T &&
        (cache & 0x3ffffu) + 32 * blocks > system.GetMemory().GetL1CacheSize())
      return false;
    ppc.downcount += static_cast<int>(cpu.downcount);
    cpu.downcount = 0;
    ppc.spr[index] = cpu.gpr[reg];
    if (index == SPR_DMAL)
    {
      if (lower.DMA_T)
      {
        const u32 address = upper.MEM_ADDR << 5;
        if (lower.DMA_LD)
          system.GetMMU().DMA_MemoryToLC(cache, address, blocks);
        else
          system.GetMMU().DMA_LCToMemory(address, cache, blocks);
      }
      DMAL(ppc).DMA_T = 0;
    }
  }
  ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles;
  cpu.pc = cia + 4;
  return true;
}

// Reads preserve the interpreter's expired-value latch; only nonnegative
// DEC values are refreshed from the live CoreTiming clock. Writes still use
// the chassis path because they schedule decrementer interrupts.
inline bool TryRunDecrementerRead(Core::System& system, CPUState& cpu, u32 raw, u32 cia)
{
  const u32 index = ((raw >> 16) & 31) | ((raw >> 6) & 0x3e0);
  if ((raw >> 26) != 31 || ((raw >> 1) & 0x3ff) != 339 ||
      index != SPR_DEC || (cpu.msr & 0x4000))
    return false;
  auto& ppc = system.GetPPCState();
  // Match SyncOut before the interpreter read: GetTicks includes downcount,
  // so charges accumulated earlier in this generated block must be visible.
  ppc.downcount += static_cast<int>(cpu.downcount);
  cpu.downcount = 0;
  if ((ppc.spr[SPR_DEC] & 0x80000000u) == 0)
    ppc.spr[SPR_DEC] = system.GetSystemTimers().GetFakeDecrementer();
  cpu.gpr[(raw >> 21) & 31] = ppc.spr[SPR_DEC];
  ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles;
  cpu.pc = cia + 4;
  return true;
}

inline bool TryRunDecrementerWrite(Core::System& system, CPUState& cpu, u32 raw, u32 cia)
{
  const u32 index = ((raw >> 16) & 31) | ((raw >> 6) & 0x3e0);
  if ((raw >> 26) != 31 || ((raw >> 1) & 0x3ff) != 467 ||
      index != SPR_DEC || (cpu.msr & 0x4000))
    return false;
  auto& ppc = system.GetPPCState();
  ppc.downcount += static_cast<int>(cpu.downcount);
  cpu.downcount = 0;
  const u32 old_value = ppc.spr[SPR_DEC];
  const u32 value = cpu.gpr[(raw >> 21) & 31];
  ppc.spr[SPR_DEC] = value;
  if ((old_value & 0x80000000u) == 0 && (value & 0x80000000u) != 0)
    ppc.Exceptions |= EXCEPTION_DECREMENTER;
  // Use the same scheduler as mtspr: remove the prior event, preserve expired
  // baselines, and schedule a new event only for a nonnegative value.
  system.GetSystemTimers().DecrementerSet();
  ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles;
  cpu.pc = cia + 4;
  return true;
}

// The SDK exception prologue saves r3/r4 through SPRG0. These scratch SPRs
// have no MMU/timer side effects, but remain privileged architectural state.
inline bool TryRunScratchRegister(Core::System& system, CPUState& cpu, u32 raw, u32 cia)
{
  const u32 xo = (raw >> 1) & 0x3ff;
  const u32 index = ((raw >> 16) & 31) | ((raw >> 6) & 0x3e0);
  if ((raw >> 26) != 31 || (xo != 339 && xo != 467) ||
      index < SPR_SPRG0 || index > SPR_SPRG3 || (cpu.msr & 0x4000))
    return false;
  const u32 reg = (raw >> 21) & 31;
  auto& ppc = system.GetPPCState();
  if (xo == 339) cpu.gpr[reg] = ppc.spr[index];
  else ppc.spr[index] = cpu.gpr[reg];
  ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles;
  cpu.pc = cia + 4;
  return true;
}

// WPAR controls the gather pipe used while building and submitting GX lists.
// Keep the chassis FIFO side effects, without invoking an instruction decoder.
inline bool TryRunGatherPipeRegister(Core::System& system, CPUState& cpu, u32 raw, u32 cia)
{
  const u32 xo = (raw >> 1) & 0x3ff;
  const u32 index = ((raw >> 16) & 31) | ((raw >> 6) & 0x3e0);
  if ((raw >> 26) != 31 || (xo != 339 && xo != 467) || index != SPR_WPAR ||
      (cpu.msr & 0x4000))
    return false;
  const u32 reg = (raw >> 21) & 31;
  if (xo == 467 && cpu.gpr[reg] != GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS)
    return false;
  auto& ppc = system.GetPPCState();
  if (xo == 339)
  {
    ppc.spr[SPR_WPAR] = (ppc.spr[SPR_WPAR] & ~1u) |
                        u32(system.GetGPFifo().IsBNE());
    cpu.gpr[reg] = ppc.spr[SPR_WPAR];
  }
  else
  {
    ppc.spr[SPR_WPAR] = cpu.gpr[reg];
    system.GetGPFifo().ResetGatherPipe();
  }
  ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles;
  cpu.pc = cia + 4;
  return true;
}

// OSInitSystemCall installs this SDK barrier outside the original DOL text.
// This is its native implementation, guarded against replacement guest code.
inline constexpr std::array<u32, 7> SDK_SYSTEM_CALL = {
    0x7d30faa6, 0x612a0008, 0x7d50fba6, 0x4c00012c,
    0x7c0004ac, 0x7d30fba6, 0x4c000064};

inline bool MatchesSystemCall(std::span<const u8> ram, u32 pc)
{
  if ((pc != 0x00000c00 && pc != 0x80000c00) || ram.size() < 0xc1c)
    return false;
  for (size_t i = 0; i < SDK_SYSTEM_CALL.size(); ++i)
  {
    const size_t offset = 0xc00 + i * 4;
    const u32 word = (u32(ram[offset]) << 24) | (u32(ram[offset + 1]) << 16) |
                     (u32(ram[offset + 2]) << 8) | ram[offset + 3];
    if (word != SDK_SYSTEM_CALL[i])
      return false;
  }
  return true;
}

inline bool TryRunSystemCall(Core::System& system, std::span<const u8> ram)
{
  auto& ppc = system.GetPPCState();
  if (ppc.msr.PR || !MatchesSystemCall(ram, ppc.pc))
    return false;

  ppc.gpr[9] = ppc.spr[SPR_HID0];
  ppc.gpr[10] = ppc.gpr[9] | 8;
  const auto write_hid0 = [&](u32 value) {
    ppc.spr[SPR_HID0] = value;
    if (HID0(ppc).ICFI)
    {
      HID0(ppc).ICFI = 0;
      ppc.iCache.Reset(system.GetJitInterface());
    }
  };
  write_hid0(ppc.gpr[10]);
  // isync/sync have no additional effect in the chassis memory model.
  write_hid0(ppc.gpr[9]);
  constexpr u32 mask = 0x87c0ffff;
  ppc.msr.Hex = ((ppc.msr.Hex & ~mask) | (ppc.spr[SPR_SRR1] & mask)) & 0xfffbffff;
  ppc.pc = ppc.npc = ppc.spr[SPR_SRR0];
  system.GetPowerPC().MSRUpdated();

  for (u32 instruction : SDK_SYSTEM_CALL)
    ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(instruction), 0xc00)->num_cycles;
  return true;
}

// Native cache maintenance with the chassis's coherent-memory semantics.
// Coalesce an exact dcb*/addi/bdnz loop only within the current timing slice.
// Returning zero leaves privileged/dcache-enabled cases to the interpreter.
inline u32 TryRunCacheOperation(Core::System& system, CPUState& cpu, u32 raw, u32 cia)
{
  auto& ppc = system.GetPPCState();
  const u32 xo = (raw >> 1) & 0x3ff;
  if ((raw >> 26) != 31 || ppc.m_enable_dcache ||
      (xo != 86 && xo != 54 && xo != 982 && xo != 470) ||
      (xo == 470 && (cpu.msr & 0x4000)))
    return 0;
  const u32 ra = (raw >> 16) & 31;
  const u32 rb = (raw >> 11) & 31;
  const u32 ea = (ra ? cpu.gpr[ra] : 0) + cpu.gpr[rb];
  const int cache_cycles = PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles;
  const u32 offset = cia & 0x3fffffff;
  if (ra == 0 && rb != 0 && cpu.ctr != 0 && cpu.ram &&
      u64(offset) + 12 <= cpu.ram_size)
  {
    const auto read = [&](u32 address) {
      return (u32(cpu.ram[address]) << 24) | (u32(cpu.ram[address + 1]) << 16) |
             (u32(cpu.ram[address + 2]) << 8) | cpu.ram[address + 3];
    };
    const u32 addi = 0x38000020 | (rb << 21) | (rb << 16);
    if (read(offset) == raw && read(offset + 4) == addi &&
        read(offset + 8) == 0x4200fff8)
    {
      const int iteration_cycles = cache_cycles +
          PPCTables::GetOpInfo(UGeckoInstruction(addi), cia + 4)->num_cycles +
          PPCTables::GetOpInfo(UGeckoInstruction(0x4200fff8), cia + 8)->num_cycles;
      const s64 available = s64(ppc.downcount) + cpu.downcount;
      const u32 count = std::min(cpu.ctr, u32(std::max<s64>(1, available / iteration_cycles)));
      system.GetJitInterface().InvalidateICacheLines(ea, count);
      cpu.gpr[rb] += 32 * count;
      cpu.ctr -= count;
      cpu.pc = cpu.ctr ? cia : cia + 12;
      ppc.downcount -= int(count * iteration_cycles);
      return count;
    }
  }
  // All four operations invalidate translated code with dcache emulation
  // disabled, including dcbf/dcbst/dcbi, as in the reference interpreter.
  system.GetJitInterface().InvalidateICacheLine(ea);
  ppc.downcount -= cache_cycles;
  cpu.pc = cia + 4;
  return 1;
}
}  // namespace StaticRecompKernel
