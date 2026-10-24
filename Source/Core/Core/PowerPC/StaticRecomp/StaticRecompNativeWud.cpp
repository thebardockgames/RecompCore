// RecompCore: StaticRecomp CPU core - native WUD library (Wii Remote link layer).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The game drives Wii Remotes through kpad.a -> wpad.a -> wud.a -> the bte
// Bluetooth stack (BTA/BTM/L2CAP/HCI/GKI) -> IOS /dev/usb/oh1/57e/305. The PC
// port has no Wii Remote: KPADRead already runs natively as "no controller",
// but KPADInit spins until WUDGetStatus() == 3, which the original reaches only
// after WUDInit's periodic alarm has read the paired devices from SYSCONF and
// BTA_EnableBluetooth -> BTA_HhEnable -> WUDiInitSub have completed over the
// emulated USB adapter. That kept the whole Bluetooth stack and the IOS USB
// device alive for nothing (about 3.5 IOS ioctls per frame).
//
// Here WUDInit completes at once in the state the original leaves after that
// chain with zero registered devices: status 3 (ready), init state finished,
// no alarms, no Bluetooth. wpad.a keeps running its own code on top (it only
// reads WUD through WUDGetStatus and the callback setters, which are plain
// memory operations). The bte entry points wud.a can still reach (visibility,
// sniff mode, link keys, device reset, sync search ...) become no-ops that
// report success, so nothing ever touches the stack or IOS. Removing the
// adapter instead (IUSB_OpenDeviceIds returning -6) stopped the game from
// drawing: every thread waited for this status (10 October 2026).

#include <algorithm>
#include <utility>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeWud.h"

namespace StaticRecompNativeSdk
{
namespace
{
// bte entry points called from wud.a (bl targets of its objects, listed by
// the game layer). All return success without doing anything: there is no
// adapter and no device.
struct NoOp
{
  u32 address;
  const char* name;
};
#include "native/wud.inc"  // the game's addresses and fingerprints
constexpr u32 kWudStateReady = 3;               // WUDGetStatus() when the stack is up
constexpr s64 kWudCycles = 200;

void Store32(u8* p, u32 value)
{
  p[0] = u8(value >> 24);
  p[1] = u8(value >> 16);
  p[2] = u8(value >> 8);
  p[3] = u8(value);
}
}  // namespace

bool NativeSdk::WudBind(const std::function<bool(u32)>& enable, bool wanted)
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_WUD");
  if (!wanted || m_verify ||
      (setting && (std::string(setting) == "off" || std::string(setting) == "0")))
    return false;
  if (!enable(kWudInit))
  {
    std::fprintf(stderr, "[native_wud] WUDInit hook missing from the module: disabled\n");
    return false;
  }
  int bound = 0;
  for (const NoOp& entry : kNoOps)
    bound += enable(entry.address) ? 1 : 0;
  m_wud = std::make_unique<WudState>();
  std::fprintf(stderr,
               "[native_wud] Bluetooth adapter ready with no Wii Remote: WUDInit native, %d/%zu "
               "bte entry points neutralised\n",
               bound, kNoOps.size());
  return true;
}

bool NativeSdk::WudRun(CPUState& guest)
{
  WudState& wud = *m_wud;
  const bool is_init = guest.pc == kWudInit;
  const NoOp* noop = nullptr;
  // WudRun sees every native hook call: reject addresses outside the no-op
  // table's range before searching it.
  static constexpr auto kNoOpRange = [] {
    u32 lo = ~0u, hi = 0;
    for (const NoOp& e : kNoOps)
    {
      lo = e.address < lo ? e.address : lo;
      hi = e.address > hi ? e.address : hi;
    }
    return std::pair<u32, u32>{lo, hi};
  }();
  if (!is_init && (guest.pc < kNoOpRange.first || guest.pc > kNoOpRange.second))
    return false;
  if (!is_init)
  {
    const auto it = std::find_if(kNoOps.begin(), kNoOps.end(),
                                 [&](const NoOp& e) { return e.address == guest.pc; });
    if (it == kNoOps.end())
      return false;
    noop = &*it;
  }
  if (wud.abandoned)
    return false;
  if (!wud.active)
  {
    if (guest.gpr[13] != kSda || !CodeMatches(guest, kWudCode))
    {
      std::fprintf(stderr, "[native_wud] wud.a differs from the original: left to it\n");
      wud.abandoned = true;
      return false;
    }
    wud.active = true;
  }
  if (noop)
  {
    ++wud.noops;
    ++wud.noop_calls[noop->address];
    guest.gpr[3] = 0;  // BTM_SUCCESS; void for the BTA_ functions
    guest.pc = guest.lr;
    guest.downcount -= kWudCycles / 4;
    return true;
  }

  // WUDInit: returns 0 when already initialised, as the original.
  u32 initialized = 0;
  u8* cb = Pointer(guest, kWudControlBlock, 0x720);
  if (!Read32(guest, kWudInitialized, initialized) || !cb)
    return false;
  if (initialized != 0)
  {
    ++wud.repeated;
    guest.gpr[3] = 0;
  }
  else
  {
    // What WUDInit zeroes, then the state the enable chain leaves behind:
    // init fiber finished (+0x10 = 4), Bluetooth "enabled" flag (+0xF = 1),
    // adapter address unknown (+0x702..+0x707 = 0), WUDiInitSub's status 3 and
    // its +0x6EA/+0x6EB flags. No alarm is created and no device registered.
    for (u32 offset : {0x0u, 0x4u, 0x8u, 0x6F0u, 0x6ECu})
      Store32(cb + offset, 0);
    cb[0x10] = 4;
    cb[0x0F] = 1;
    for (u32 offset = 0x702; offset < 0x708; ++offset)
      cb[offset] = 0;
    cb[0x708] = u8(kWudStateReady);
    cb[0x6EA] = 1;
    cb[0x6EB] = 0;
    if (u8* flag = Pointer(guest, kWudInitialized, 4))
      Store32(flag, 1);
    ++wud.inits;
    guest.gpr[3] = 1;
  }
  guest.pc = guest.lr;
  guest.downcount -= kWudCycles;
  return true;
}

void NativeSdk::PrintWudStats() const
{
  if (!m_wud)
    return;
  const WudState& wud = *m_wud;
  std::fprintf(stderr, "[native_wud] active=%d abandoned=%d inits=%llu repeated=%llu noops=%llu\n",
               wud.active, wud.abandoned, static_cast<unsigned long long>(wud.inits),
               static_cast<unsigned long long>(wud.repeated),
               static_cast<unsigned long long>(wud.noops));
  for (const auto& [address, calls] : wud.noop_calls)
  {
    const auto it = std::find_if(kNoOps.begin(), kNoOps.end(),
                                 [&](const NoOp& e) { return e.address == address; });
    std::fprintf(stderr, "[native_wud]   %s x%llu\n", it != kNoOps.end() ? it->name : "?",
                 static_cast<unsigned long long>(calls));
  }
}
}  // namespace StaticRecompNativeSdk
