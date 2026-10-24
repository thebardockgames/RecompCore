// RecompCore: StaticRecomp CPU core - system instructions on the module's state.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The privileged instructions the generated code hands to the core (mtspr/
// mfspr of the BATs, HID0/HID4, MMCR, DMA, DEC, WPAR, SPRG; the SDK's system
// call barrier; cache maintenance), executed on the module's CPUState and the
// port's register file. Cycle counts are the 750CL's as the chassis's tables
// give them (StaticRecompOpCycles.h). What still lives in the chassis (its MMU
// tables, instruction cache, performance monitor, gather pipe, translated
// code) is reached through KernelHost (StaticRecompKernelChassis builds it).

#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <span>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestMemory.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRam.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRegs.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompOpCycles.h"
#include "core/cpu.h"

namespace StaticRecompKernel
{
using StaticRecompGuestRegs::RegsView;

// SPR numbers (the 750CL's).
namespace Spr
{
constexpr u32 kDec = 22;
constexpr u32 kSrr0 = 26;
constexpr u32 kSrr1 = 27;
constexpr u32 kSprg0 = 272;
constexpr u32 kSprg3 = 275;
constexpr u32 kIbat0U = 528;
constexpr u32 kIbat3L = 535;
constexpr u32 kDbat0U = 536;
constexpr u32 kDbat3L = 543;
constexpr u32 kIbat4U = 560;
constexpr u32 kIbat7L = 567;
constexpr u32 kDbat4U = 568;
constexpr u32 kDbat7L = 575;
constexpr u32 kHid2 = 920;
constexpr u32 kWpar = 921;
constexpr u32 kDmaU = 922;
constexpr u32 kDmaL = 923;
constexpr u32 kMmcr0 = 952;
constexpr u32 kPmc1 = 953;
constexpr u32 kPmc2 = 954;
constexpr u32 kMmcr1 = 956;
constexpr u32 kPmc3 = 957;
constexpr u32 kPmc4 = 958;
constexpr u32 kHid0 = 1008;
constexpr u32 kHid4 = 1011;
constexpr u32 kL2cr = 1017;
}  // namespace Spr

constexpr u32 kExceptionDecrementer = 0x00000001;
constexpr u32 kGatherPipePhysicalAddress = 0x0C008000;
constexpr u32 kHid0Icfi = 0x800u;

// What the kernel still needs from the chassis.
struct KernelHost
{
  // A BAT/HID4 write that changed the value, an HID0 write with ICFI (the
  // flush; the stored value has ICFI clear) or an MMCR write: the chassis
  // keeps its copy and the state it derives from it.
  std::function<void(u32 index, u32 value)> spr_written;
  // The port's DBAT table (StaticRecompNativeBat) after a DBAT/HID4 change.
  std::function<void()> native_bats_updated;
  std::function<bool()> gather_pipe_not_empty;  // WPAR.BNE
  std::function<void()> gather_pipe_reset;
  // Locked-cache DMA to/from the EFB or MMIO windows (0x08/0x0C...).
  std::function<void(bool to_cache, u32 cache, u32 address, u32 blocks)> dma_special;
  std::function<bool()> dcache_enabled;
  std::function<void(u32 address)> invalidate_icache_line;
  std::function<void(u32 address, u32 lines)> invalidate_icache_lines;
  // The decrementer when the native clock does not own it (bridge mode,
  // unit tests).
  std::function<u32()> read_decrementer;
  std::function<void(u32 value)> decrementer_written;
};

inline u32 SprIndex(u32 raw)
{
  return ((raw >> 16) & 31) | ((raw >> 6) & 0x3e0);
}

inline u32 ReadDecrementer(const KernelHost& host)
{
  if (auto* native_clock = StaticRecompNativeClock::NativeClock::Instance();
      native_clock && native_clock->OwnsDecrementer())
    return native_clock->Decrementer();
  return host.read_decrementer ? host.read_decrementer() : 0;
}

inline void DecrementerWritten(const KernelHost& host, u32 value)
{
  if (auto* native_clock = StaticRecompNativeClock::NativeClock::Instance();
      native_clock && native_clock->OwnsDecrementer())
  {
    native_clock->DecrementerSet();
    return;
  }
  if (host.decrementer_written)
    host.decrementer_written(value);
}

// The BATs, HID0/HID4, MMCR, L2CR and PMCs on the module's state: `spr` is the
// port's register file while the guest runs. The chassis derives its MMU
// table (BATs, HID4), its instruction cache flush (HID0.ICFI) and its
// performance monitor (MMCR0/1) from its own copy, so those writes go through
// to it as well while the chassis exists.
inline bool TryRunSystemRegister(const KernelHost& host, CPUState& cpu, RegsView regs, u32 raw, u32 cia)
{
  using namespace Spr;
  const u32 xo = (raw >> 1) & 0x3ff;
  const u32 index = SprIndex(raw);
  const bool ibat = (index >= kIbat0U && index <= kIbat3L) || (index >= kIbat4U && index <= kIbat7L);
  const bool dbat = (index >= kDbat0U && index <= kDbat3L) || (index >= kDbat4U && index <= kDbat7L);
  const bool plain = index == kL2cr || index == kPmc1 || index == kPmc2 || index == kPmc3 ||
                     index == kPmc4;
  if ((raw >> 26) != 31 || (xo != 339 && xo != 467) || (cpu.msr & 0x4000) ||
      (!ibat && !dbat && !plain && index != kHid0 && index != kHid4 && index != kMmcr0 &&
       index != kMmcr1))
    return false;
  const u32 reg = (raw >> 21) & 31;
  if (xo == 339)
    cpu.gpr[reg] = regs.spr[index];
  else
  {
    const u32 old = regs.spr[index];
    regs.spr[index] = cpu.gpr[reg];
    if ((ibat || dbat || index == kHid4) && old != regs.spr[index])
    {
      if (host.spr_written)
        host.spr_written(index, regs.spr[index]);
      if ((dbat || index == kHid4) && host.native_bats_updated)
        host.native_bats_updated();
    }
    else if (index == kHid0 && (regs.spr[index] & kHid0Icfi))
    {
      regs.spr[index] &= ~kHid0Icfi;  // ICFI: the flush happens now
      if (host.spr_written)
        host.spr_written(index, regs.spr[index]);
    }
    else if (index == kMmcr0 || index == kMmcr1)
    {
      if (host.spr_written)
        host.spr_written(index, regs.spr[index]);
    }
  }
  (*regs.downcount) -= StaticRecompOpCycles::Cycles(raw);
  cpu.pc = cia + 4;
  return true;
}

// DMAU/DMAL: the locked-cache DMA (the transfer completes at once).
inline bool TryRunDmaRegister(const KernelHost& host, CPUState& cpu, RegsView regs, u32 raw, u32 cia)
{
  using namespace Spr;
  const u32 xo = (raw >> 1) & 0x3ff;
  const u32 index = SprIndex(raw);
  if ((raw >> 26) != 31 || (xo != 339 && xo != 467) || (index != kDmaU && index != kDmaL) ||
      (cpu.msr & 0x4000))
    return false;
  const u32 reg = (raw >> 21) & 31;
  if (xo == 339)
    cpu.gpr[reg] = regs.spr[index];
  else
  {
    const u32 lower = cpu.gpr[reg];
    const u32 upper = regs.spr[kDmaU];
    const bool trigger = (lower >> 1) & 1;   // DMA_T
    const bool load = (lower >> 4) & 1;      // DMA_LD
    u32 blocks = ((upper & 0x1f) << 2) | ((lower >> 2) & 3);  // DMA_LEN_U:DMA_LEN_L
    if (!blocks)
      blocks = 128;
    const u32 cache = (lower >> 5) << 5;     // LC_ADDR
    const u32 l1_size = StaticRecompGuestRam::GuestRam::Instance().L1Size();
    if (index == kDmaL && trigger && (cache & 0x3ffffu) + 32 * blocks > l1_size)
      return false;
    (*regs.downcount) += static_cast<int>(cpu.downcount);
    cpu.downcount = 0;
    regs.spr[index] = cpu.gpr[reg];
    if (index == kDmaL)
    {
      if (trigger)
      {
        const u32 address = (upper >> 5) << 5;  // MEM_ADDR
        const u32 window = address & 0x0F000000u;
        if (window == 0x08000000u || window == 0x0C000000u)
        {
          if (host.dma_special)
            host.dma_special(load, cache, address, blocks);
        }
        else
        {
          const auto memory = StaticRecompGuestMemory::GuestMemory::Own();
          u8* l1 = memory.GetL1Cache() + (cache & 0x3FFFF);
          if (load)
            memory.CopyFromEmu(l1, address, 32 * blocks);
          else
            memory.CopyToEmu(address, l1, 32 * blocks);
        }
      }
      regs.spr[kDmaL] &= ~0x2u;  // DMA_T: the transfer is complete
    }
  }
  (*regs.downcount) -= StaticRecompOpCycles::Cycles(raw);
  cpu.pc = cia + 4;
  return true;
}

// Reads preserve the interpreter's expired-value latch; only nonnegative
// DEC values are refreshed from the live clock.
inline bool TryRunDecrementerRead(const KernelHost& host, CPUState& cpu, RegsView regs, u32 raw, u32 cia)
{
  const u32 index = SprIndex(raw);
  if ((raw >> 26) != 31 || ((raw >> 1) & 0x3ff) != 339 || index != Spr::kDec || (cpu.msr & 0x4000))
    return false;
  // GetTicks includes downcount, so charges accumulated earlier in this
  // generated block must be visible.
  (*regs.downcount) += static_cast<int>(cpu.downcount);
  cpu.downcount = 0;
  if ((regs.spr[Spr::kDec] & 0x80000000u) == 0)
    regs.spr[Spr::kDec] = ReadDecrementer(host);
  cpu.gpr[(raw >> 21) & 31] = regs.spr[Spr::kDec];
  (*regs.downcount) -= StaticRecompOpCycles::Cycles(raw);
  cpu.pc = cia + 4;
  return true;
}

inline bool TryRunDecrementerWrite(const KernelHost& host, CPUState& cpu, RegsView regs, u32 raw, u32 cia)
{
  const u32 index = SprIndex(raw);
  if ((raw >> 26) != 31 || ((raw >> 1) & 0x3ff) != 467 || index != Spr::kDec || (cpu.msr & 0x4000))
    return false;
  (*regs.downcount) += static_cast<int>(cpu.downcount);
  cpu.downcount = 0;
  const u32 old_value = regs.spr[Spr::kDec];
  const u32 value = cpu.gpr[(raw >> 21) & 31];
  regs.spr[Spr::kDec] = value;
  if ((old_value & 0x80000000u) == 0 && (value & 0x80000000u) != 0)
    *regs.exceptions |= kExceptionDecrementer;
  // Remove the prior event, preserve expired baselines, and schedule a new
  // event only for a nonnegative value.
  DecrementerWritten(host, value);
  (*regs.downcount) -= StaticRecompOpCycles::Cycles(raw);
  cpu.pc = cia + 4;
  return true;
}

// The SDK exception prologue saves r3/r4 through SPRG0. These scratch SPRs
// have no MMU/timer side effects, but remain privileged architectural state.
inline bool TryRunScratchRegister(const KernelHost&, CPUState& cpu, RegsView regs, u32 raw, u32 cia)
{
  const u32 xo = (raw >> 1) & 0x3ff;
  const u32 index = SprIndex(raw);
  if ((raw >> 26) != 31 || (xo != 339 && xo != 467) || index < Spr::kSprg0 || index > Spr::kSprg3 ||
      (cpu.msr & 0x4000))
    return false;
  const u32 reg = (raw >> 21) & 31;
  if (xo == 339)
    cpu.gpr[reg] = regs.spr[index];
  else
    regs.spr[index] = cpu.gpr[reg];
  (*regs.downcount) -= StaticRecompOpCycles::Cycles(raw);
  cpu.pc = cia + 4;
  return true;
}

// WPAR controls the gather pipe used while building and submitting GX lists.
inline bool TryRunGatherPipeRegister(const KernelHost& host, CPUState& cpu, RegsView regs, u32 raw, u32 cia)
{
  const u32 xo = (raw >> 1) & 0x3ff;
  const u32 index = SprIndex(raw);
  if ((raw >> 26) != 31 || (xo != 339 && xo != 467) || index != Spr::kWpar || (cpu.msr & 0x4000))
    return false;
  const u32 reg = (raw >> 21) & 31;
  if (xo == 467 && cpu.gpr[reg] != kGatherPipePhysicalAddress)
    return false;
  if (xo == 339)
  {
    const bool not_empty = host.gather_pipe_not_empty && host.gather_pipe_not_empty();
    regs.spr[Spr::kWpar] = (regs.spr[Spr::kWpar] & ~1u) | u32(not_empty);
    cpu.gpr[reg] = regs.spr[Spr::kWpar];
  }
  else
  {
    regs.spr[Spr::kWpar] = cpu.gpr[reg];
    if (host.gather_pipe_reset)
      host.gather_pipe_reset();
  }
  (*regs.downcount) -= StaticRecompOpCycles::Cycles(raw);
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

// Cycles charged for the seven instructions of the barrier.
inline int SystemCallCycles()
{
  int total = 0;
  for (u32 instruction : SDK_SYSTEM_CALL)
    total += StaticRecompOpCycles::Cycles(instruction);
  return total;
}

// The same barrier on the module's own state, inside a native burst. HID0
// comes from the port's register file; the barrier leaves it unchanged, and a
// HID0 with ICFI already set (icache flush pending) goes to the chassis path.
inline bool TryRunSystemCallNative(CPUState& cpu, const u32* spr, std::span<const u8> ram)
{
  if ((cpu.msr & 0x4000u) != 0 || !MatchesSystemCall(ram, cpu.pc))
    return false;
  const u32 hid0 = spr[Spr::kHid0];
  if (hid0 & kHid0Icfi)
    return false;
  cpu.gpr[9] = hid0;
  cpu.gpr[10] = hid0 | 8;
  constexpr u32 mask = 0x87c0ffff;
  cpu.msr = ((cpu.msr & ~mask) | (cpu.srr1 & mask)) & 0xfffbffff;
  cpu.pc = cpu.srr0;
  return true;
}

// Native cache maintenance with the chassis's coherent-memory semantics.
// Coalesce an exact dcb*/addi/bdnz loop only within the current timing slice.
// Returning zero leaves privileged/dcache-enabled cases to the interpreter.
inline u32 TryRunCacheOperation(const KernelHost& host, CPUState& cpu, RegsView regs, u32 raw, u32 cia)
{
  const u32 xo = (raw >> 1) & 0x3ff;
  if ((raw >> 26) != 31 || (host.dcache_enabled && host.dcache_enabled()) ||
      (xo != 86 && xo != 54 && xo != 982 && xo != 470) ||
      (xo == 470 && (cpu.msr & 0x4000)))
    return 0;
  const u32 ra = (raw >> 16) & 31;
  const u32 rb = (raw >> 11) & 31;
  const u32 ea = (ra ? cpu.gpr[ra] : 0) + cpu.gpr[rb];
  const int cache_cycles = StaticRecompOpCycles::Cycles(raw);
  const u32 offset = cia & 0x3fffffff;
  if (ra == 0 && rb != 0 && cpu.ctr != 0 && cpu.ram && u64(offset) + 12 <= cpu.ram_size)
  {
    const auto read = [&](u32 address) {
      return (u32(cpu.ram[address]) << 24) | (u32(cpu.ram[address + 1]) << 16) |
             (u32(cpu.ram[address + 2]) << 8) | cpu.ram[address + 3];
    };
    const u32 addi = 0x38000020 | (rb << 21) | (rb << 16);
    if (read(offset) == raw && read(offset + 4) == addi && read(offset + 8) == 0x4200fff8)
    {
      const int iteration_cycles = cache_cycles + StaticRecompOpCycles::Cycles(addi) +
                                   StaticRecompOpCycles::Cycles(0x4200fff8);
      const s64 available = s64((*regs.downcount)) + cpu.downcount;
      const u32 count = std::min(cpu.ctr, u32(std::max<s64>(1, available / iteration_cycles)));
      if (host.invalidate_icache_lines)
        host.invalidate_icache_lines(ea, count);
      cpu.gpr[rb] += 32 * count;
      cpu.ctr -= count;
      cpu.pc = cpu.ctr ? cia : cia + 12;
      (*regs.downcount) -= int(count * iteration_cycles);
      return count;
    }
  }
  // All four operations invalidate translated code with dcache emulation
  // disabled, including dcbf/dcbst/dcbi, as in the reference interpreter.
  if (host.invalidate_icache_line)
    host.invalidate_icache_line(ea);
  (*regs.downcount) -= cache_cycles;
  cpu.pc = cia + 4;
  return 1;
}
}  // namespace StaticRecompKernel
