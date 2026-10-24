// RecompCore: StaticRecomp CPU core - chassis adapter of the copy loop (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The copy loop with the chassis's state as its environment: the core's DBAT
// table and slice counter when a core exists, else (the unit tests) a table
// built from the chassis's registers and the chassis's counter; the
// chassis's data cache emulation and memory checks.

#pragma once

#include <array>

#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompMemory.h"
#include "Core/System.h"

namespace StaticRecompMemoryChassis
{
// The memory sizes the harness's CPUState declares (the table is built from them).
inline u32 s_table_ram_size = 0;
inline u32 s_table_exram_size = 0;

inline u32 ChassisDbat(void* context, u32 address)
{
  if (g_static_recomp_core)
    return g_static_recomp_core->Dbat(address);
  auto& system = *static_cast<Core::System*>(context);
  static StaticRecompNativeBat::NativeBat table;
  static std::array<u32, 17> seen{};
  const auto& ppc = system.GetPPCState();
  std::array<u32, 17> now{};
  for (int i = 0; i < 8; ++i)
  {
    now[i] = ppc.spr[SPR_DBAT0U + i];
    now[8 + i] = ppc.spr[SPR_DBAT4U + i];
  }
  now[16] = ppc.spr[SPR_HID4];
  if (now != seen || table.Updates() == 0)
  {
    table.Update(ppc.spr, ((ppc.spr[SPR_HID4] >> 25) & 1) != 0, s_table_ram_size, s_table_exram_size,
                 0x4000);
    seen = now;
  }
  return table.Entry(address);
}

inline StaticRecompMemory::CopyLoopEnv Env(Core::System& system, const CPUState& cpu)
{
  s_table_ram_size = cpu.ram_size;
  s_table_exram_size = cpu.exram_size;
  StaticRecompMemory::CopyLoopEnv env;
  env.dbat = &ChassisDbat;
  env.dbat_context = &system;
  env.downcount = g_static_recomp_core ? &g_static_recomp_core->Downcount() : &system.GetPPCState().downcount;
  env.dcache_enabled = system.GetPPCState().m_enable_dcache;
  env.mem_checks = system.GetPowerPC().GetMemChecks().HasAny();
  return env;
}

inline u32 TryRunCopyLoop(Core::System& system, CPUState& cpu)
{
  return StaticRecompMemory::TryRunCopyLoop(cpu, Env(system, cpu));
}
}  // namespace StaticRecompMemoryChassis
