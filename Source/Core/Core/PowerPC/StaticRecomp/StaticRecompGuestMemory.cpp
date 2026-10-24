// RecompCore: StaticRecomp CPU core - guest memory accessor (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompGuestMemory.h"

#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRam.h"

namespace StaticRecompGuestMemory
{
GuestMemory GuestMemory::Own()
{
  const auto& ram = StaticRecompGuestRam::GuestRam::Instance();
  return GuestMemory(ram.Mem1(), ram.Mem1Size(), ram.Mem2(), ram.Mem2Size(), ram.L1(),
                     ram.L1Size());
}
}  // namespace StaticRecompGuestMemory
