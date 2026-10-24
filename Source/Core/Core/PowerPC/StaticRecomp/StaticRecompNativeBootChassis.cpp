// RecompCore: StaticRecomp CPU core - chassis adapter of the native boot (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeBootChassis.h"

#include <cstdio>
#include <cstring>

#include "Common/Config/Config.h"
#include "Core/CommonTitles.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/VersionInfo.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompIosMemoryValues.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeBoot.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"

namespace StaticRecompNativeBootChassis
{
namespace
{
using namespace StaticRecompNativeBoot;

DiscIO::Region ToDisc(Region region)
{
  switch (region)
  {
  case Region::NtscJ:
    return DiscIO::Region::NTSC_J;
  case Region::NtscK:
    return DiscIO::Region::NTSC_K;
  case Region::NtscU:
    return DiscIO::Region::NTSC_U;
  default:
    return DiscIO::Region::PAL;
  }
}

bool SameIosValues()
{
  const auto& own = GetIosMemoryValues();
  const auto& chassis = IOS::HLE::GetMemoryValues();
  static_assert(sizeof(IosMemoryValues) == sizeof(IOS::HLE::MemoryValues));
  if (own.size() != chassis.size())
    return false;
  for (size_t i = 0; i < own.size(); ++i)
  {
    if (std::memcmp(&own[i], &chassis[i], sizeof(IosMemoryValues)) != 0)
      return false;
  }
  return true;
}

// The chassis's bitfield writes on a scratch state: the reference for the
// boot's own bit masks.
bool SameCpuBits(const BootCpuState& cpu, const PowerPC::PowerPCState& reference)
{
  PowerPC::PowerPCState ppc = reference;
  ppc.msr.Hex = 0;
  ppc.msr.RI = 1;
  ppc.msr.DR = 1;
  ppc.msr.IR = 1;
  ppc.msr.FP = 1;
  const u32 hid0 = HID0(ppc).Hex, hid2 = HID2(ppc).Hex, hid4 = HID4(ppc).Hex;
  HID0(ppc).BHT = 1;
  HID0(ppc).BTIC = 1;
  HID0(ppc).DCFA = 1;
  HID0(ppc).SPD = 1;
  HID0(ppc).DCFI = 1;
  HID0(ppc).DCE = 1;
  HID0(ppc).ICE = 1;
  HID0(ppc).NHR = 1;
  HID0(ppc).DPM = 1;
  HID2(ppc).PSE = 1;
  HID2(ppc).WPE = 1;
  HID2(ppc).LSQE = 1;
  HID4(ppc).L2CFI = 1;
  HID4(ppc).LPE = 1;
  HID4(ppc).ST0 = 1;
  HID4(ppc).SBE = 1;
  HID4(ppc).reserved_3 = 1;
  return ppc.msr.Hex == cpu.msr && HID0(ppc).Hex == (hid0 | cpu.hid0_set) &&
         HID2(ppc).Hex == (hid2 | cpu.hid2_set) && HID4(ppc).Hex == (hid4 | cpu.hid4_set);
}
}  // namespace

bool Boot(Core::System& system, const BootPaths& paths)
{
  static_assert(SPR_IBAT0U == 528 && SPR_IBAT0L == 529 && SPR_DBAT0U == 536 && SPR_DBAT0L == 537 &&
                SPR_DBAT1U == 538 && SPR_DBAT1L == 539 && SPR_IBAT4U == 560 && SPR_IBAT4L == 561 &&
                SPR_DBAT4U == 568 && SPR_DBAT4L == 569 && SPR_DBAT5U == 570 && SPR_DBAT5L == 571);
  if (!SameIosValues())
    std::fprintf(stderr, "[native_boot] IOS memory values differ from the chassis's table\n");
  ChassisHooks hooks;
  hooks.override_region_settings = [] { return Config::Get(Config::MAIN_OVERRIDE_REGION_SETTINGS); };
  hooks.has_ios = [&system] { return system.GetIOS() != nullptr; };
  hooks.title = [](const std::string& game_id, u64 title_id, u8 revision, Region region) {
    SConfig::GetInstance().SetRunningGameMetadata(game_id, game_id, title_id, revision, ToDisc(region));
  };
  hooks.boot_ios = [&system](u16 ios_number) {
    if (auto* ios = system.GetIOS())
      ios->BootIOS(Titles::IOS(ios_number));
  };
  SetChassisHooks(std::move(hooks));

  BootCpuState cpu;
  if (!StaticRecompNativeBoot::Boot(paths, &cpu))
    return false;
  auto& ppc = system.GetPPCState();
  if (!SameCpuBits(cpu, ppc))
    std::fprintf(stderr, "[native_boot] MSR/HID bits differ from the chassis's fields\n");
  ppc.msr.Hex = cpu.msr;
  system.GetPowerPC().MSRUpdated();
  HID0(ppc).Hex |= cpu.hid0_set;
  HID2(ppc).Hex |= cpu.hid2_set;
  HID4(ppc).Hex |= cpu.hid4_set;
  for (const auto& [spr, value] : cpu.sprs)
    ppc.spr[spr] = value;
  auto& mmu = system.GetMMU();
  mmu.DBATUpdated();
  mmu.IBATUpdated();
  ppc.gpr[1] = cpu.gpr1;
  ppc.gpr[3] = cpu.gpr3;
  ppc.pc = cpu.pc;
  ppc.npc = cpu.pc + 4;
  return true;
}
}  // namespace StaticRecompNativeBootChassis
