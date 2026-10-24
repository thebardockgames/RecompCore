// RecompCore: StaticRecomp CPU core - SMC and chunk validation (the code guard).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"

bool StaticRecompCore::FastDispatchableAt(u32 address) const
{
  return m_code_guard.FastDispatchable(address, m_guest.msr, m_module_active);
}

bool StaticRecompCore::DispatchableAt(u32 address)
{
  // Phase S: the MSR of the state that is authoritative right now (the
  // chassis mirror is stale while native code runs).
  const bool ir = m_authority == StateAuthority::Guest ? (m_guest.msr & 0x20) != 0 :
                                                          m_system.GetPPCState().msr.IR != 0;
  return m_code_guard.Dispatchable(address, ir, m_module_active);
}

void StaticRecompCore::OnICacheInvalidate(u32 address, u32 length, u32 physical_address)
{
  if (m_fallback_jit)
    m_fallback_jit->GetBlockCache()->InvalidateICache(address, length, false);
  m_code_guard.Invalidate(address, length, physical_address, m_module_active);
}
