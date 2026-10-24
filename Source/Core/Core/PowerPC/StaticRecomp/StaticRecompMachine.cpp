// RecompCore: StaticRecomp CPU core - the native machine (phase L1).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompMachine.h"

#include <cstdio>

#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeExi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGp.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeVi.h"

namespace StaticRecompMachine
{
Machine::Machine() = default;
Machine::~Machine() = default;

bool Machine::Read(u32 ea, u8 size, u64* value)
{
  // CP/PE/PI FIFO registers of the GP front.
  if (gp && (ea >> 24) == 0xCC && gp->Read(ea, size, value))
    return true;
  if (pi && (ea & 0xFFFFF000) == 0xCC003000u && pi->Read(ea, size, value))
    return true;
  if (vi && (ea & 0xFFFFF000) == 0xCC002000u && vi->Read(ea, size, value))
    return true;
  if (sdk && (ea & 0xFFFFFF00) == 0xCD006400u && sdk->SiRegisterRead(ea, size, value))
    return true;
  if (sdk && (ea & 0xFFFFFFF0) == 0xCD006C00u && sdk->AiRegisterRead(ea, size, value))
    return true;
  if (pi && (ea & 0xFF7FF000) == 0xCD000000u && pi->HollywoodRead(ea, size, value))
    return true;
  if (pi && (ea & 0xFFFFFFC0) == 0xCC004000u && pi->MiRead(ea, size, value))
    return true;
  if (sdk && ea == 0xCC00500Au && sdk->DspControlRead(ea, size, value))
    return true;
  if (exi && (ea & 0xFFFFF000) == 0xCD006000u && exi->Read(ea, size, value))
    return true;
  return false;
}

bool Machine::Write(u32 ea, u8 size, u64 value)
{
  // The GP front owns the gather pipe and the CP/PE/PI FIFO registers.
  if (gp && (ea >> 24) == 0xCC)
  {
    if ((ea & 0xFFFFF000) == 0xCC008000u)
    {
      if (gp->TryGatherFast(value, size))
        return true;
      if (gp->Active())
      {
        gp->GatherWrite(value, size);
        return true;
      }
    }
    else if (gp->Write(ea, size, value))
    {
      return true;
    }
  }
  if (pi && (ea & 0xFFFFF000) == 0xCC003000u && pi->Write(ea, size, value))
    return true;
  if (vi && (ea & 0xFFFFF000) == 0xCC002000u && vi->Write(ea, size, value))
    return true;
  if (sdk && (ea & 0xFFFFFF00) == 0xCD006400u && sdk->SiRegisterWrite(ea, size, value))
    return true;
  if (sdk && (ea & 0xFFFFFFF0) == 0xCD006C00u && sdk->AiRegisterWrite(ea, size, value))
    return true;
  if (pi && (ea & 0xFF7FF000) == 0xCD000000u && pi->HollywoodWrite(ea, size, value))
    return true;
  if (pi && (ea & 0xFFFFFFC0) == 0xCC004000u && pi->MiWrite(ea, size, value))
    return true;
  if (sdk && ea == 0xCC00500Au && sdk->DspControlWrite(ea, size, value))
    return true;
  if (exi && (ea & 0xFFFFF000) == 0xCD006000u && exi->Write(ea, size, value))
    return true;
  return false;
}
void Machine::DoState(StaticRecompState::StateStream& p, bool present)
{
  // Version 1: the native SDK layer (pending native interrupts, native audio).
  // Version 2: + deferred callbacks and native NAND handles.
  // Version 3: + the kind of each native IPC handle (virtual IOS devices).
  // Version 4: + the native GP front (gather pipe, CP/PE/PI FIFO registers).
  // Version 5: + the native video interface.
  // Version 6: + the open handles of the native NAND store.
  // Version 7: + the native clock (decrementer) and interrupt controller; the
  //            VI keeps the exact phase of its half line in flight.
  // Version 8: + the Hollywood GPIO (native PI), the AI register page and the
  //            native EXI (channels, IPL/RTC/SRAM device).
  // Version 9: + the EXI's own SRAM bytes and RTC flags.
  u32 version = sdk ? 9 : 0;
  if (present)
    p.Do(version);
  if (sdk)
  {
    // The clock goes first: on load it clears its queue and every module
    // after it rebuilds its own events from its state.
    if (present && version >= 7)
    {
      bool has_clock = clock != nullptr;
      p.Do(has_clock);
      if (has_clock && clock)
        clock->DoState(p, version);
      else if (has_clock)
        p.SetMeasureMode();
      bool has_pi = pi != nullptr;
      p.Do(has_pi);
      if (has_pi && pi)
        pi->DoState(p, version);
      else if (has_pi)
        p.SetMeasureMode();
      if (version >= 8)
      {
        bool has_exi = exi != nullptr;
        p.Do(has_exi);
        if (has_exi && exi)
          exi->DoState(p, version);
        else if (has_exi)
          p.SetMeasureMode();
      }
    }
    else if (p.IsReadMode())
    {
      if (clock)
        clock->ResetForLoad();
      if (pi)
        pi->SeedFromDolphin();
    }
    sdk->DoState(p, present && version >= 1, version);
    if (present && version >= 4)
    {
      bool has_gp = gp != nullptr;
      p.Do(has_gp);
      if (has_gp && !gp)
      {
        std::fprintf(stderr, "[native_gp] state has native GP data but no GP front exists\n");
        p.SetMeasureMode();
      }
      else if (has_gp)
      {
        gp->DoState(p);
      }
    }
    if (present && version >= 5)
    {
      bool has_vi = vi != nullptr;
      p.Do(has_vi);
      if (has_vi && !vi)
      {
        std::fprintf(stderr, "[native_vi] state has native VI data but no native VI exists\n");
        p.SetMeasureMode();
      }
      else if (has_vi)
      {
        vi->DoState(p, version);
      }
    }
  }
  else if (p.IsReadMode() && present && version != 0)
  {
    std::fprintf(stderr, "[staticrecomp] state has native SDK data but no native module is loaded\n");
    p.SetMeasureMode();
  }
}

void Machine::SeedForCheckpoint()
{
  // A native checkpoint has no chassis copies to seed from later: the devices
  // the game has not touched yet take their state over now. Only our own
  // save calls this (the chassis writes an undo state before every load, and
  // seeding there would freeze the boot-time registers).
  if (vi)
    vi->Active();
  if (pi)
    pi->Active();
  if (gp)
    gp->Active();
}
}  // namespace StaticRecompMachine
