// SPDX-License-Identifier: GPL-2.0-or-later
// Inline native replacements: functions the recompiled code calls directly
// that run natively in place of the call (DolRecomp's dolrecomp_native_inline
// table, exported by the module as staticrecomp_native_inline), with the
// guest's exact results, registers, stack writes and cycles. The guest code
// runs instead whenever the replacement cannot reproduce it exactly (other
// inputs, code that differs from the fingerprint, a call that would not fit
// in the burst's budget).
//
// MODERNGEKKO_NATIVE_INLINE=off keeps the guest code;
// MODERNGEKKO_NATIVE_INLINE_VERIFY=1 runs the guest code on a copy of the
// state for every call, compares, and lets the guest code run.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeMath.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

namespace StaticRecompNativeSdk
{
namespace
{
#include "native/math.inc"  // the game's addresses and fingerprints

NativeSdk* s_inline_owner = nullptr;
NativeSdk::InlineVerifier s_inline_verifier;

void StoreBe32(u8* p, u32 value)
{
  p[0] = u8(value >> 24);
  p[1] = u8(value >> 16);
  p[2] = u8(value >> 8);
  p[3] = u8(value);
}

void StoreBe64(u8* p, u64 value)
{
  StoreBe32(p, u32(value >> 32));
  StoreBe32(p + 4, u32(value));
}

u64 LoadBe64(const u8* p)
{
  u64 value = 0;
  for (int i = 0; i < 8; ++i)
    value = (value << 8) | p[i];
  return value;
}

u64 Bits(f64 value)
{
  u64 bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

f64 FromBits(u64 bits)
{
  f64 value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

int SqrtThunk(CPUState* ctx)
{
  return s_inline_owner && s_inline_owner->RunSqrt(*ctx) ? 1 : 0;
}
}  // namespace

void NativeSdk::SetInlineVerifier(InlineVerifier verifier)
{
  s_inline_verifier = std::move(verifier);
}

void NativeSdk::BindInline(u32 count, InlineFunction* table)
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_INLINE");
  if (!table || !count || !m_hook_addresses || (setting && std::string(setting) == "off"))
    return;
  m_inline_verify = std::getenv("MODERNGEKKO_NATIVE_INLINE_VERIFY") != nullptr;
  const u32* end = m_hook_addresses + count;
  const u32* it = std::lower_bound(m_hook_addresses, end, kSqrt);
  if (it == end || *it != kSqrt)
    return;
  s_inline_owner = this;
  table[it - m_hook_addresses] = &SqrtThunk;
  std::fprintf(stderr, "[native_inline] sqrt at 0x%08X: native in place%s\n", kSqrt,
               m_inline_verify ? " (verify: the guest code runs, results compared)" : "");
}

bool NativeSdk::RunSqrt(CPUState& cpu)
{
  InlineStats& stats = m_inline_sqrt;
  if (!stats.checked)
  {
    stats.checked = true;
    stats.enabled = cpu.gpr[13] != 0 && CodeMatches(cpu, kSqrtCode);
    if (!stats.enabled)
      std::fprintf(stderr, "[native_inline] sqrt code differs from the fingerprint: the guest code runs\n");
  }
  if (!stats.enabled)
    return false;
  const u8* probe = Pointer(cpu, cpu.gpr[2] + static_cast<u32>(kSqrtProbeOffset), 8);
  const u32 frame = cpu.gpr[1] - 32;
  u8* stack = Pointer(cpu, frame, 32);
  if (!probe || !stack)
  {
    ++stats.fallbacks;
    return false;
  }
  const u64 x = Bits(cpu.fpr[1]);
  const u64 constant = LoadBe64(probe);
  StaticRecompNativeMath::SqrtRun run;
  if (!StaticRecompNativeMath::IeeeSqrt(x, constant, &run))
  {
    ++stats.fallbacks;
    return false;
  }
  // The guest code would leave to the dispatcher at a loop's back edge if the
  // burst's budget ran out inside it: only a call that fits runs here.
  const s64 cost = s64(run.instructions) + 1;  // + the branch of sqrt()
  if (!(cpu.dispatch_budget > 0 && cpu.downcount - cost > -cpu.dispatch_budget))
  {
    ++stats.budget;
    return false;
  }
  const u32 so = (cpu.xer >> 31) & 1;
  const u32 cr0 = (run.q_odd ? 0x2u : 0x8u) | so;  // cmpwi r0,1 on q & 1
  const u32 cr1 = 0x4u | so;                        // cmpwi cr1,ix0,0: positive
  if (m_inline_verify && s_inline_verifier)
  {
    // The guest code on a copy of the state, then the guest code for real.
    if (const auto reference = s_inline_verifier(cpu, kSqrt))
    {
      const s64 consumed = cpu.downcount - reference->downcount;
      const u32 regs[] = {run.r0, run.r3, run.r4, run.r5, run.r6, run.r7, run.r8, run.r9, run.r10, run.r11, run.r12};
      const int index[] = {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
      bool same = Bits(reference->fpr[1]) == run.result && consumed == cost &&
                  (reference->cr >> 24) == ((cr0 << 4) | cr1) && (reference->xer & 0x20000000u) == 0 &&
                  (!run.rounding || Bits(reference->fpr[0]) == constant);
      for (int i = 0; i < 11; ++i)
        same = same && reference->gpr[index[i]] == regs[i];
      ++(same ? stats.verified : stats.mismatches);
      if (!same && stats.mismatches <= 8)
        std::fprintf(stderr,
                     "[native_inline] sqrt mismatch x=%016llx: guest=%016llx native=%016llx cycles guest=%lld "
                     "native=%lld cr guest=%08x\n",
                     static_cast<unsigned long long>(x), static_cast<unsigned long long>(Bits(reference->fpr[1])),
                     static_cast<unsigned long long>(run.result), static_cast<long long>(consumed),
                     static_cast<long long>(cost), reference->cr);
    }
    return false;
  }
  // The guest's stack frame: back chain, x, the probe (inexact path), result.
  StoreBe32(stack, cpu.gpr[1]);
  StoreBe64(stack + 8, x);
  if (run.rounding)
    StoreBe64(stack + 16, constant);
  StoreBe32(stack + 16, run.r4);
  StoreBe32(stack + 20, run.r5);
  cpu.fpr[1] = FromBits(run.result);
  if (run.rounding)
    cpu.fpr[0] = FromBits(constant);
  cpu.gpr[0] = run.r0;
  cpu.gpr[3] = run.r3;
  cpu.gpr[4] = run.r4;
  cpu.gpr[5] = run.r5;
  cpu.gpr[6] = run.r6;
  cpu.gpr[7] = run.r7;
  cpu.gpr[8] = run.r8;
  cpu.gpr[9] = run.r9;
  cpu.gpr[10] = run.r10;
  cpu.gpr[11] = run.r11;
  cpu.gpr[12] = run.r12;
  cpu.cr = (cpu.cr & 0x00ffffffu) | (cr0 << 28) | (cr1 << 24);
  cpu.xer &= ~0x20000000u;  // srawi: CA clear (q is positive)
  cpu.downcount -= static_cast<int>(cost);
  cpu.pc = cpu.lr;
  ++stats.calls;
  return true;
}

void NativeSdk::PrintInlineStats() const
{
  if (!m_inline_sqrt.checked)
    return;
  std::fprintf(stderr,
               "[native_inline] sqrt enabled=%d calls=%llu fallbacks=%llu budget=%llu verified=%llu mismatches=%llu\n",
               int(m_inline_sqrt.enabled), static_cast<unsigned long long>(m_inline_sqrt.calls),
               static_cast<unsigned long long>(m_inline_sqrt.fallbacks),
               static_cast<unsigned long long>(m_inline_sqrt.budget),
               static_cast<unsigned long long>(m_inline_sqrt.verified),
               static_cast<unsigned long long>(m_inline_sqrt.mismatches));
}
}  // namespace StaticRecompNativeSdk
