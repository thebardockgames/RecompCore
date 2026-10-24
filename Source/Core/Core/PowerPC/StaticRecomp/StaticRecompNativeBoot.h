// RecompCore: StaticRecomp CPU core - native boot (phase G).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Boots the game from the extracted disc folder the way the console's BS2,
// the IOS and the game's apploader leave memory, without running any of them:
// disc header and globals in low MEM1, the IOS memory values, the exception
// vectors, setting.txt, bi2 and the FST at the top of MEM1, the DOL sections,
// the system menu's NAND files and the title's NAND directories and uid, and
// the CPU registers (MSR, HID, BATs, stack, entry point). The result is
// compared byte for byte with the chassis's boot (MODERNGEKKO_BOOT_DUMP).
//
// The memory is the port's (GuestMemory); the CPU state comes back as
// BootCpuState and the caller applies it (StaticRecompNativeBootChassis puts
// it on the chassis's PowerPC state while the chassis exists).

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompNativeBoot
{
struct BootPaths
{
  std::string game_root;  // extracted disc: sys/, files/, tmd.bin
  std::string nand_root;  // the user's Wii folder
  std::string sysmenu_root;  // bundled system menu files (Sys/Wii next to the runner); empty: none
};

enum class Region
{
  NtscJ,
  NtscU,
  Pal,
  NtscK,
};

// The CPU as the BS2 and the apploader leave it.
struct BootCpuState
{
  u32 msr = 0;
  u32 hid0_set = 0, hid2_set = 0, hid4_set = 0;  // bits set on top of the reset values
  std::vector<std::pair<u32, u32>> sprs;         // (SPR number, value): the BATs
  u32 gpr1 = 0, gpr3 = 0, pc = 0;
};

// What the boot needs from the chassis while it exists.
struct ChassisHooks
{
  // Config "override region settings" (keep setting.txt's region values).
  std::function<bool()> override_region_settings;
  // The chassis's IOS kernel exists (it wrote the network defaults itself).
  std::function<bool()> has_ios;
  // The running title, for the chassis's configuration and its IOS object.
  std::function<void(const std::string& game_id, u64 title_id, u8 revision, Region region)> title;
  std::function<void(u16 ios_number)> boot_ios;
};
void SetChassisHooks(ChassisHooks hooks);

// True when memory is set up and `cpu` holds the registers to start at the
// DOL's entry point. On failure nothing can be assumed about memory.
bool Boot(const BootPaths& paths, BootCpuState* cpu);

// What the IOS leaves in the NAND before any title runs, for a runner with
// no IOS of the chassis: ES's top-level directories and the SYSCONF the
// configuration writes (StaticRecompSysConf::WriteBootSettings).
bool PrepareNand(const std::string& nand_root);

// Enabled unless MODERNGEKKO_NATIVE_BOOT=off.
bool Enabled();
}  // namespace StaticRecompNativeBoot
