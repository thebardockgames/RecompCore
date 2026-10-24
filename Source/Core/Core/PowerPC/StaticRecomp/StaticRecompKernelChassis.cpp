// RecompCore: StaticRecomp CPU core - chassis adapter of the kernel (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompKernelChassis.h"

#include <cstdio>

#include "Common/Swap.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"

namespace StaticRecompKernelChassis
{
namespace
{
static_assert(SPR_DEC == StaticRecompKernel::Spr::kDec && SPR_SRR0 == StaticRecompKernel::Spr::kSrr0 &&
              SPR_SRR1 == StaticRecompKernel::Spr::kSrr1 && SPR_SPRG0 == StaticRecompKernel::Spr::kSprg0 &&
              SPR_SPRG3 == StaticRecompKernel::Spr::kSprg3 && SPR_IBAT0U == StaticRecompKernel::Spr::kIbat0U &&
              SPR_IBAT3L == StaticRecompKernel::Spr::kIbat3L && SPR_DBAT0U == StaticRecompKernel::Spr::kDbat0U &&
              SPR_DBAT3L == StaticRecompKernel::Spr::kDbat3L && SPR_IBAT4U == StaticRecompKernel::Spr::kIbat4U &&
              SPR_IBAT7L == StaticRecompKernel::Spr::kIbat7L && SPR_DBAT4U == StaticRecompKernel::Spr::kDbat4U &&
              SPR_DBAT7L == StaticRecompKernel::Spr::kDbat7L && SPR_HID2 == StaticRecompKernel::Spr::kHid2 &&
              SPR_WPAR == StaticRecompKernel::Spr::kWpar && SPR_DMAU == StaticRecompKernel::Spr::kDmaU &&
              SPR_DMAL == StaticRecompKernel::Spr::kDmaL && SPR_MMCR0 == StaticRecompKernel::Spr::kMmcr0 &&
              SPR_PMC1 == StaticRecompKernel::Spr::kPmc1 && SPR_PMC2 == StaticRecompKernel::Spr::kPmc2 &&
              SPR_MMCR1 == StaticRecompKernel::Spr::kMmcr1 && SPR_PMC3 == StaticRecompKernel::Spr::kPmc3 &&
              SPR_PMC4 == StaticRecompKernel::Spr::kPmc4 && SPR_HID0 == StaticRecompKernel::Spr::kHid0 &&
              SPR_HID4 == StaticRecompKernel::Spr::kHid4 && SPR_L2CR == StaticRecompKernel::Spr::kL2cr);
static_assert(EXCEPTION_DECREMENTER == StaticRecompKernel::kExceptionDecrementer);
static_assert(GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS == StaticRecompKernel::kGatherPipePhysicalAddress);

Core::System* s_system = nullptr;
StaticRecompKernel::KernelHost s_kernel_host;

int ChassisCycles(u32 raw)
{
  return static_cast<int>(PPCTables::GetOpInfo(UGeckoInstruction(raw), 0)->num_cycles);
}
}  // namespace

bool CyclesMatchChassis()
{
  // One encoding of every instruction the kernel times.
  static const u32 samples[] = {
      0x38000020,  // addi
      0x4200fff8,  // bdnz
      0x612a0008,  // ori
      0x4c00012c,  // isync
      0x4c000064,  // rfi
      0x7d30faa6,  // mfspr
      0x7d50fba6,  // mtspr
      0x7c0004ac,  // sync
      0x7c00006c,  // dcbst
      0x7c0000ac,  // dcbf
      0x7c0003ac,  // dcbi
      0x7c0007ec,  // dcbz
      0x7c0007ac,  // icbi
  };
  bool same = true;
  for (u32 raw : samples)
  {
    if (StaticRecompOpCycles::Cycles(raw) != ChassisCycles(raw))
    {
      std::fprintf(stderr, "[staticrecomp] cycle table: %08x own %d chassis %d\n", raw,
                   StaticRecompOpCycles::Cycles(raw), ChassisCycles(raw));
      same = false;
    }
  }
  return same;
}

const StaticRecompKernel::KernelHost& Host(Core::System& system)
{
  if (s_system == &system)
    return s_kernel_host;
  s_system = &system;
  CyclesMatchChassis();
  StaticRecompKernel::KernelHost host;
  host.spr_written = [&system](u32 index, u32 value) {
    auto& ppc = system.GetPPCState();
    ppc.spr[index] = value;
    const bool ibat = (index >= SPR_IBAT0U && index <= SPR_IBAT3L) ||
                      (index >= SPR_IBAT4U && index <= SPR_IBAT7L);
    const bool dbat = (index >= SPR_DBAT0U && index <= SPR_DBAT3L) ||
                      (index >= SPR_DBAT4U && index <= SPR_DBAT7L);
    if (ibat)
      system.GetMMU().IBATUpdated();
    else if (dbat)
      system.GetMMU().DBATUpdated();
    else if (index == SPR_HID4)
    {
      system.GetMMU().IBATUpdated();
      system.GetMMU().DBATUpdated();
    }
    else if (index == SPR_HID0)
      ppc.iCache.Reset(system.GetJitInterface());  // the ICFI flush
    else if (index == SPR_MMCR0 || index == SPR_MMCR1)
      PowerPC::MMCRUpdated(ppc);
  };
  host.native_bats_updated = [] {
    if (g_static_recomp_core)
      g_static_recomp_core->UpdateNativeBats();
  };
  host.gather_pipe_not_empty = [&system] { return system.GetGPFifo().IsBNE(); };
  host.gather_pipe_reset = [&system] { system.GetGPFifo().ResetGatherPipe(); };
  host.dma_special = [&system](bool to_cache, u32 cache, u32 address, u32 blocks) {
    if (to_cache)
      system.GetMMU().DMA_MemoryToLC(cache, address, blocks);
    else
      system.GetMMU().DMA_LCToMemory(address, cache, blocks);
  };
  host.dcache_enabled = [&system] { return system.GetPPCState().m_enable_dcache; };
  host.invalidate_icache_line = [&system](u32 address) {
    system.GetJitInterface().InvalidateICacheLine(address);
  };
  host.invalidate_icache_lines = [&system](u32 address, u32 lines) {
    system.GetJitInterface().InvalidateICacheLines(address, lines);
  };
  host.read_decrementer = [&system] { return system.GetSystemTimers().GetFakeDecrementer(); };
  host.decrementer_written = [&system](u32 value) {
    system.GetPPCState().spr[SPR_DEC] = value;  // the chassis schedules from its copy
    system.GetSystemTimers().DecrementerSet();
  };
  s_kernel_host = std::move(host);
  return s_kernel_host;
}

StaticRecompKernel::RegsView ChassisRegs(Core::System& system)
{
  auto& ppc = system.GetPPCState();
  return StaticRecompKernel::RegsView{ppc.spr, &ppc.downcount, &ppc.Exceptions};
}

bool TryRunSystemRegister(Core::System& system, u32 raw, u32 cia)
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
    if ((dbat || index == SPR_HID4) && old != ppc.spr[index] && g_static_recomp_core)
      g_static_recomp_core->UpdateNativeBats();
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
  ppc.downcount -= StaticRecompOpCycles::Cycles(raw);
  return true;
}

bool TryRunSystemCall(Core::System& system, std::span<const u8> ram)
{
  auto& ppc = system.GetPPCState();
  if (ppc.msr.PR || !StaticRecompKernel::MatchesSystemCall(ram, ppc.pc))
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
  ppc.downcount -= StaticRecompKernel::SystemCallCycles();
  return true;
}
}  // namespace StaticRecompKernelChassis
