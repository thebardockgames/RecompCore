// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Native replacements of the game's C library math, run in place of the
// recompiled code (DolRecomp inline native calls). Each one reproduces the
// guest function exactly: the result, the registers it leaves, and the guest
// instructions it executes (the cycles the generated code charges, one per
// instruction), so timing and state are those of the original.
#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompNativeMath
{
// fdlibm's __ieee754_sqrt as the Wii SDK's MSL compiles it (CodeWarrior),
// block by block, for x positive, normal and finite. `constant` is the
// double the compiled code loads and compares with itself where fdlibm does
// `z = one - tiny; if (z >= one)` (its rounding-mode probe).
struct SqrtRun
{
  u64 result = 0;
  u32 instructions = 0;   // guest instructions executed, from the entry
  bool rounding = false;  // the inexact path (loads and stores `constant`)
  // The registers the compiled code leaves (volatile by the ABI, kept exact).
  u32 r0 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0;
  bool q_odd = false;  // the last compare (cr0): q & 1 against 1
};
// false: an input the guest code handles another way (zero, negative,
// subnormal, infinity, NaN); the guest code then runs.
bool IeeeSqrt(u64 x, u64 constant, SqrtRun* run);
}  // namespace StaticRecompNativeMath
