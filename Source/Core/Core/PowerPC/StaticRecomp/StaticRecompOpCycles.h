// RecompCore: StaticRecomp CPU core - cycle counts of the system instructions (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The cycles the core charges for the instructions it executes itself
// (mtspr/mfspr, the system call barrier, cache maintenance and its loop):
// the values of the chassis's instruction table (Core/PowerPC/PPCTables.cpp,
// Dolphin Emulator Project, GPL-2.0-or-later). The kernel adapter compares
// each one with the chassis's table.

#pragma once

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompOpCycles
{
// -1: not an instruction the core executes itself.
constexpr int Cycles(u32 raw)
{
  const u32 primary = raw >> 26;
  switch (primary)
  {
  case 14:  // addi
  case 16:  // bcx
  case 24:  // ori
    return 1;
  case 19:
    switch ((raw >> 1) & 0x3ff)
    {
    case 150:  // isync
      return 1;
    case 50:  // rfi
      return 2;
    default:
      return -1;
    }
  case 31:
    switch ((raw >> 1) & 0x3ff)
    {
    case 339:  // mfspr
      return 1;
    case 467:  // mtspr
      return 2;
    case 598:  // sync
      return 3;
    case 54:   // dcbst
    case 86:   // dcbf
    case 470:  // dcbi
    case 1014: // dcbz
      return 5;
    case 982:  // icbi
      return 4;
    default:
      return -1;
    }
  default:
    return -1;
  }
}
}  // namespace StaticRecompOpCycles
