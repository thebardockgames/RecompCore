// RecompCore: StaticRecomp CPU core - the port's supervisor registers (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The special-purpose registers the module ABI does not carry in CPUState
// (HID0/HID1/HID4, the BATs, SDR1, SPRG0-3, DEC, WPAR, DMAU/DMAL, the
// performance monitor, L2CR...), the pending exception lines and the cycle
// counter of the current slice. While the core runs they live here; the
// chassis's PowerPCState copy is written only when the chassis takes over
// (EnterChassis) and read only when the guest does (EnterGuest), like the
// rest of the CPU state. The registers the chassis derives state from while
// it still exists (BATs and HID4 for its MMU table, HID0.ICFI for its
// instruction cache, MMCR0/1) are written through at the same time.

#pragma once

#include <cstdint>

namespace StaticRecompGuestRegs
{
// A view over whichever storage is authoritative (the kernel works on it).
struct RegsView
{
  std::uint32_t* spr;
  int* downcount;
  std::uint32_t* exceptions;
};

struct GuestRegs
{
  std::uint32_t spr[1024]{};  // indexed by SPR number, as the chassis's array
  std::uint32_t exceptions = 0;  // pending lines (the chassis's EXCEPTION_* bits)
  int downcount = 0;             // cycles left in the clock's current slice

  RegsView View() { return RegsView{spr, &downcount, &exceptions}; }
};
}  // namespace StaticRecompGuestRegs
