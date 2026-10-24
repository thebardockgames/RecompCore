// RecompCore: StaticRecomp CPU core - chassis adapter of the native checkpoints (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeStateChassis.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Common/ChunkFile.h"
#include "Core/Config/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeState.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"
#include "Core/System.h"

namespace StaticRecompNativeStateChassis
{
namespace
{
using StaticRecompNativeState::ClockRecord;
using StaticRecompNativeState::CpuRecord;

void Install(Core::System& system)
{
  StaticRecompNativeState::ChassisHooks hooks;
  hooks.get_cpu = [&system](CpuRecord& cpu) {
    const auto& ppc = system.GetPPCState();
    cpu.pc = ppc.pc;
    cpu.npc = ppc.npc;
    std::memcpy(cpu.gpr, ppc.gpr, sizeof(cpu.gpr));
    for (int i = 0; i < 32; ++i)
    {
      cpu.ps[i][0] = ppc.ps[i].ps0;
      cpu.ps[i][1] = ppc.ps[i].ps1;
    }
    std::memcpy(cpu.cr, ppc.cr.fields, sizeof(cpu.cr));
    cpu.msr = ppc.msr.Hex;
    cpu.fpscr = ppc.fpscr.Hex;
    cpu.exceptions = ppc.Exceptions;
    cpu.xer_ca = ppc.xer_ca;
    cpu.xer_so_ov = ppc.xer_so_ov;
    cpu.xer_stringctrl = ppc.xer_stringctrl;
    cpu.reserve_address = ppc.reserve_address;
    cpu.reserve = ppc.reserve ? 1 : 0;
    for (int i = 0; i < 16; ++i)
      cpu.sr[i] = ppc.sr[i];
    std::memcpy(cpu.spr, ppc.spr, sizeof(cpu.spr));
  };
  hooks.set_cpu = [&system](const CpuRecord& cpu) {
    auto& ppc = system.GetPPCState();
    ppc.pc = cpu.pc;
    ppc.npc = cpu.npc;
    std::memcpy(ppc.gpr, cpu.gpr, sizeof(cpu.gpr));
    for (int i = 0; i < 32; ++i)
    {
      ppc.ps[i].ps0 = cpu.ps[i][0];
      ppc.ps[i].ps1 = cpu.ps[i][1];
    }
    std::memcpy(ppc.cr.fields, cpu.cr, sizeof(cpu.cr));
    ppc.msr.Hex = cpu.msr;
    ppc.fpscr.Hex = cpu.fpscr;
    ppc.Exceptions = cpu.exceptions;
    ppc.xer_ca = static_cast<u8>(cpu.xer_ca);
    ppc.xer_so_ov = static_cast<u8>(cpu.xer_so_ov);
    ppc.xer_stringctrl = static_cast<u16>(cpu.xer_stringctrl);
    ppc.reserve_address = cpu.reserve_address;
    ppc.reserve = cpu.reserve != 0;
    for (int i = 0; i < 16; ++i)
      ppc.sr[i] = cpu.sr[i];
    std::memcpy(ppc.spr, cpu.spr, sizeof(cpu.spr));
    ppc.iCache.Reset(system.GetJitInterface());
    system.GetPowerPC().MSRUpdated();
    PowerPC::RoundingModeUpdated(ppc);
    PowerPC::RecalculateAllFeatureFlags(ppc);
    auto& mmu = system.GetMMU();
    mmu.DBATUpdated();
    mmu.IBATUpdated();
    mmu.SDRUpdated();
  };
  hooks.get_clock = [&system](ClockRecord& clock) {
    auto& timing = system.GetCoreTiming();
    clock.ticks = static_cast<s64>(timing.GetTicks());
    clock.tb_start_value = timing.GetFakeTBStartValue();
    clock.tb_start_ticks = timing.GetFakeTBStartTicks();
  };
  hooks.set_clock = [&system](const ClockRecord& clock) {
    auto& timing = system.GetCoreTiming();
    timing.ResetCounterForLoad(clock.ticks);
    system.GetPPCState().downcount = 0;
    timing.SetFakeTBStartValue(clock.tb_start_value);
    timing.SetFakeTBStartTicks(clock.tb_start_ticks);
  };
  hooks.game_id = [] { return SConfig::GetInstance().GetGameID(); };
  StaticRecompNativeState::SetChassisHooks(std::move(hooks));
}


// MODERNGEKKO_STATE_VERIFY=1: the native sections written by the port's
// stream against the chassis's PointerWrap (the writer before phase S step
// 38), byte for byte.
void VerifyNativeSections(Core::System& system)
{
  if (!std::getenv("MODERNGEKKO_STATE_VERIFY") || !g_static_recomp_core)
    return;
  StaticRecompState::BufferStream own;
  g_static_recomp_core->DoNativeState(own, true);
  u8* measure_ptr = nullptr;
  PointerWrap measure(&measure_ptr, 0, PointerWrap::Mode::Measure);
  system.GetJitInterface().DoExtraState(measure, true);
  std::vector<u8> chassis(measure.GetOffsetFromPreviousPosition(nullptr));
  u8* ptr = chassis.data();
  PointerWrap write(&ptr, chassis.size(), PointerWrap::Mode::Write);
  system.GetJitInterface().DoExtraState(write, true);
  const bool same = write.IsWriteMode() && chassis == own.Buffer();
  std::fprintf(stderr, "[native_state] verify: native sections %s (%zu bytes own, %zu chassis)\n",
               same ? "identical to the chassis's writer" : "DIFFER from the chassis's writer",
               own.Buffer().size(), chassis.size());
}
}  // namespace

bool Save(Core::System& system, const std::string& path)
{
  Install(system);
  VerifyNativeSections(system);
  return StaticRecompNativeState::Save(path);
}

bool Load(Core::System& system, const std::string& path)
{
  Install(system);
  return StaticRecompNativeState::Load(path);
}
}  // namespace StaticRecompNativeStateChassis
