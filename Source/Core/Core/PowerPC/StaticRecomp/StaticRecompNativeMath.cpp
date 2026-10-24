// SPDX-License-Identifier: GPL-2.0-or-later
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeMath.h"

#include <cstring>

namespace StaticRecompNativeMath
{
// Each block below is a basic block of the compiled function; `n` adds its
// instruction count before it runs (the order the generated code charges).
// Register names follow the compiled code: r6 ix0, r0 ix1, r11 s0, r9 s1,
// r12 q, r10 q1, r7 r, r3 the biased exponent.
bool IeeeSqrt(u64 x, u64 constant, SqrtRun* run)
{
  u32 n = 8;  // stwu, stfd, lwz x2, rlwinm, addis, cmplwi, bc
  s32 ix0 = static_cast<s32>(x >> 32);
  u32 ix1 = static_cast<u32>(x);
  if ((ix0 & 0x7ff00000) == 0x7ff00000)
    return false;  // infinity or NaN
  n += 2;  // cmpwi cr1, bc
  if (ix0 <= 0)
    return false;  // zero or negative
  n += 2;  // srawi., bc
  const s32 m = ix0 >> 20;
  if (m == 0)
    return false;  // subnormal
  n += 5;  // addi, rlwinm, rlwinm., oris, bc
  ix0 = (ix0 & 0x000fffff) | 0x00100000;
  if (((m - 1023) & 1) != 0)
  {
    n += 4;
    ix0 += ix0 + static_cast<s32>(ix1 >> 31);
    ix1 += ix1;
  }
  n += 10;  // shift, four clears, lis, b
  ix0 += ix0 + static_cast<s32>(ix1 >> 31);
  ix1 += ix1;
  s32 q = 0, s0 = 0;
  u32 q1 = 0, s1 = 0, r = 0x00200000;
  for (;;)
  {
    n += 2;  // cmpwi r7, bc
    if (r == 0)
      break;
    n += 3;  // add, cmpw, bc
    const s32 t = s0 + static_cast<s32>(r);
    if (!(t > ix0))
    {
      n += 3;
      s0 = t + static_cast<s32>(r);
      ix0 -= t;
      q += static_cast<s32>(r);
    }
    n += 5;
    ix0 += ix0 + static_cast<s32>(ix1 >> 31);
    ix1 += ix1;
    r >>= 1;
  }
  n += 2;  // lis r7, b
  r = 0x80000000u;
  u32 t1 = 0;
  for (;;)
  {
    n += 2;  // cmpwi r7, bc
    if (r == 0)
      break;
    n += 4;  // cmpw, mr, add, bc
    const s32 t = s0;
    t1 = s1 + r;
    bool take = t < ix0;
    if (!take)
    {
      n += 1;  // bc ne
      if (t == ix0)
      {
        n += 2;  // cmplw, bc gt
        take = !(t1 > ix1);
      }
    }
    if (take)
    {
      n += 5;
      s1 = t1 + r;
      if ((t1 & 0x80000000u) != 0)
      {
        n += 2;
        if ((s1 & 0x80000000u) == 0)
        {
          n += 1;
          s0 += 1;
        }
      }
      n += 3;  // cmplw, subf, bc
      ix0 -= t;
      if (ix1 < t1)
      {
        n += 1;
        ix0 -= 1;
      }
      n += 2;
      ix1 -= t1;
      q1 += r;
    }
    n += 5;
    ix0 += ix0 + static_cast<s32>(ix1 >> 31);
    ix1 += ix1;
    r >>= 1;
  }
  n += 2;  // or., bc
  bool rounding = false;
  if ((static_cast<u32>(ix0) | ix1) != 0)
  {
    rounding = true;
    n += 5;  // lfd, fcmpo, stfd, cror, bc
    double probe;
    std::memcpy(&probe, &constant, sizeof(probe));
    if (probe == probe)  // fcmpo f0,f0: equal unless NaN (then no rounding)
    {
      n += 4;  // addis, stfd, cmplwi, bc
      if (q1 == 0xffffffffu)
      {
        n += 3;
        q1 = 0;
        q += 1;
      }
      else
      {
        n += 2;  // fcmpo, bc: f0 > f0 is never true
        n += 2;
        q1 += q1 & 1;
      }
    }
  }
  n += 6;  // rlwinm, srawi, cmpwi, rlwinm, addis, bc
  u32 hi = static_cast<u32>((q >> 1) + 0x3fe00000);
  u32 lo = q1 >> 1;
  const bool odd = (q & 1) == 1;
  if (odd)
  {
    n += 1;
    lo |= 0x80000000u;
  }
  n += 6;  // addi, stw, rlwinm, add, stw, lfd
  const u32 r0 = (static_cast<u32>(m - 1023) << 19) & 0xfff00000u;
  hi += r0;
  n += 2;  // addi r1, blr
  run->result = (u64(hi) << 32) | lo;
  run->instructions = n;
  run->rounding = rounding;
  run->r0 = r0;
  run->r3 = static_cast<u32>(m);
  run->r4 = hi;
  run->r5 = lo;
  run->r6 = static_cast<u32>(ix0);
  run->r7 = 0;
  run->r8 = t1;
  run->r9 = s1;
  run->r10 = q1;
  run->r11 = static_cast<u32>(s0);
  run->r12 = static_cast<u32>(q);
  run->q_odd = odd;
  return true;
}
}  // namespace StaticRecompNativeMath
