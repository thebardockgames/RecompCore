// RecompCore: StaticRecomp CPU core - native PAD library (GameCube controllers).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The game drives GameCube controllers through the SDK's pad.a: PADReset /
// PADRecalibrate start an asynchronous chain on the serial interface (type
// probe, origin read, polling), PADRead reads the polled responses and
// PADControlMotor sends rumble commands. The game resets the empty ports every
// frame, so the emulated SI was busy all the time. Here those four functions
// work directly on the SDK's own variables (EnabledBits, ResettingBits,
// Origin[], Type[] ...) and the PC controller, the way N64Recomp ports replace
// libultra's controller calls: a reset completes at once, with the type and
// origin a GameCube controller reports (Dolphin's CSIDevice_GCController), and
// the SI is never used. The PC controller comes through HostGamePad
// (StaticRecompHostInput.h), registered by the runtime; Dolphin's GCPad and
// InputCommon are not involved. State stays in guest memory, so savestates need nothing
// extra. A reset chain the original left running (a savestate taken mid-chain)
// is left to finish on the SI.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>

#include "Core/PowerPC/StaticRecomp/StaticRecompHostInput.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePad.h"

namespace Core
{
class System;
}

namespace StaticRecompNativeSdk
{
namespace
{
#include "native/pad.inc"  // the game's addresses and fingerprints
constexpr u32 kSramFlags = 0x800030E3;      // OSSram flags: 0x20 no rumble, 0x40 no recalibration
constexpr u32 kGcControllerType = 0x09000000;  // SI type of a standard controller
constexpr u32 kTypeNoMotor = 0x20000000;
constexpr s64 kPadCycles = 400;


u32 Load32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}

void Store32(u8* p, u32 value)
{
  p[0] = u8(value >> 24);
  p[1] = u8(value >> 16);
  p[2] = u8(value >> 8);
  p[3] = u8(value);
}

u32 ChanBit(u32 chan)
{
  return 0x80000000u >> chan;
}
}  // namespace

bool NativeSdk::PadBind(const std::function<bool(u32)>& enable, bool wanted)
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_PAD");
  if (!wanted || m_verify || !m_pad_read ||
      (setting && (std::string(setting) == "off" || std::string(setting) == "0")))
    return false;
  if (!enable(kPadReset) || !enable(kPadRecalibrate) || !enable(kPadControlMotor))
  {
    std::fprintf(stderr, "[native_pad] hooks missing from the module: disabled\n");
    return false;
  }
  m_pad = std::make_unique<PadState>();
  std::fprintf(stderr, "[native_pad] PAD library without the SI (PADRead, PADReset, "
                       "PADRecalibrate, PADControlMotor)\n");
  return true;
}

bool NativeSdk::HostPadConnected(int chan)
{
  HostGamePad* host = GetHostGamePad();
  return host != nullptr && host->Poll(chan).connected;
}

bool NativeSdk::PadReady(const CPUState& guest)
{
  PadState& pad = *m_pad;
  if (pad.abandoned)
    return false;
  if (!pad.active)
  {
    u32 make_status = 0;
    if (guest.gpr[13] != kSda || !CodeMatches(guest, kPadCode) ||
        !Read32(guest, kMakeStatus, make_status) || make_status != kSpec2MakeStatus)
    {
      std::fprintf(stderr, "[native_pad] pad.a differs from the original: left to it\n");
      pad.abandoned = true;
      return false;
    }
    pad.active = true;
  }
  return true;
}

bool NativeSdk::PadVar(const CPUState& guest, u32 address, u32& value) const
{
  return Read32(guest, address, value);
}

void NativeSdk::PadSetVar(const CPUState& guest, u32 address, u32 value) const
{
  if (u8* p = Pointer(guest, address, 4))
    Store32(p, value);
}

void NativeSdk::PadUpdateOrigin(const CPUState& guest, u32 chan)
{
  // UpdateOrigin (0x8021EEEC): drop the bits the analog mode does not report,
  // centre the sticks, and ignore a skewed X origin on standard controllers.
  u8* origin = Pointer(guest, kOrigin + 12 * chan, 12);
  u32 analog_mode = 0, xpatch = 0, type = 0;
  if (!origin || !PadVar(guest, kAnalogMode, analog_mode) || !PadVar(guest, kXPatchBits, xpatch) ||
      !PadVar(guest, kPadType + 4 * chan, type))
    return;
  const auto low_nibbles_off = [&](std::initializer_list<int> bytes) {
    for (int i : bytes)
      origin[i] &= 0xF0;
  };
  switch (analog_mode & 0x700)
  {
  case 0x000:
  case 0x500:
  case 0x600:
  case 0x700:
    low_nibbles_off({6, 7, 8, 9});
    break;
  case 0x100:
    low_nibbles_off({4, 5, 8, 9});
    break;
  case 0x200:
    low_nibbles_off({4, 5, 6, 7});
    break;
  default:
    break;
  }
  for (int i = 2; i < 6; ++i)
    origin[i] = u8(origin[i] - 0x80);
  if ((xpatch & ChanBit(chan)) && s8(origin[2]) > 0x40 &&
      (type & 0xFFFF0000u) == kGcControllerType)
    origin[2] = 0;
}

void NativeSdk::PadReadOrigin(const CPUState& guest, u32 chan)
{
  // Origin (0x41) or recalibrate (0x42) response of Dolphin's standard
  // controller, which reports perfectly centred values; the SDK reads it
  // straight into Origin[chan].
  u8* origin = Pointer(guest, kOrigin + 12 * chan, 10);
  if (!origin)
    return;
  const u8 response[10] = {0,
                           0,
                           kHostPadStickCenter,
                           kHostPadStickCenter,
                           kHostPadStickCenter,
                           kHostPadStickCenter,
                           0,
                           0,
                           0,
                           0};
  std::memcpy(origin, response, sizeof(response));
}

void NativeSdk::PadReset(const CPUState& guest, u32 mask, bool recalibrate)
{
  // PADReset / PADRecalibrate bit updates (SIDisablePolling has nothing to
  // stop: the SI is not polled).
  u32 pending = 0, waiting = 0, checking = 0, resetting = 0, enabled = 0, barrel = 0;
  u32 recal = 0, spec = 0, resetting_chan = 0;
  PadVar(guest, kPendingBits, pending);
  PadVar(guest, kWaitingBits, waiting);
  PadVar(guest, kCheckingBits, checking);
  PadVar(guest, kResettingBits, resetting);
  PadVar(guest, kEnabledBits, enabled);
  PadVar(guest, kBarrelBits, barrel);
  PadVar(guest, kRecalibrateBits, recal);
  PadVar(guest, kSpec, spec);
  PadVar(guest, kResettingChan, resetting_chan);
  mask = (mask | pending) & ~(waiting | checking);
  PadSetVar(guest, kPendingBits, 0);
  resetting |= mask;
  PadSetVar(guest, kResettingBits, resetting);
  PadSetVar(guest, kEnabledBits, enabled & ~mask);
  PadSetVar(guest, kBarrelBits, barrel & ~mask);
  const u8* sram = Pointer(guest, kSramFlags, 1);
  if (recalibrate ? (sram && !(*sram & 0x40)) : spec == 4)
    PadSetVar(guest, kRecalibrateBits, recal | mask);
  // A chain the original started (savestate) finishes on the SI.
  if (resetting_chan != 32)
  {
    ++m_pad->chained;
    return;
  }

  // DoReset → SIGetTypeAsync → PADTypeAndStatusCallback → origin read →
  // PADOriginCallback, for each channel in turn, completed at once.
  for (;;)
  {
    PadVar(guest, kResettingBits, resetting);
    const u32 chan = static_cast<u32>(std::countl_zero(resetting));
    PadSetVar(guest, kResettingChan, chan);
    if (chan == 32)
      break;
    const u32 bit = ChanBit(chan);
    PadSetVar(guest, kResettingBits, resetting & ~bit);
    if (u8* origin = Pointer(guest, kOrigin + 12 * chan, 12))
      std::memset(origin, 0, 12);
    PadVar(guest, kRecalibrateBits, recal);
    PadSetVar(guest, kRecalibrateBits, recal & ~bit);
    // No controller: the type probe gets no response.
    if (!HostPadConnected(static_cast<int>(chan)))
      continue;
    PadSetVar(guest, kPadType + 4 * chan, kGcControllerType);
    PadVar(guest, kSpec, spec);
    if (spec >= 2)
    {
      PadReadOrigin(guest, chan);
      PadUpdateOrigin(guest, chan);
    }
    PadVar(guest, kEnabledBits, enabled);
    PadSetVar(guest, kEnabledBits, enabled | bit);
    ++m_pad->enabled;
  }
}

bool NativeSdk::PadReadNoSi(CPUState& guest)
{
  if (!PadReady(guest))
  {
    ++m_pad->deferred;
    return false;
  }
  u8* status = Pointer(guest, guest.gpr[3], 48);
  u8* previous = Pointer(guest, kPadPrevious, 48);
  const u8* origins = Pointer(guest, kOrigin, 48);
  if (!status || !previous || !origins)
  {
    ++m_pad->deferred;
    return false;
  }
  u32 motor = 0;
  bool reset_idle = false;
  for (u32 chan = 0; chan < 4; ++chan)
  {
    const u32 bit = ChanBit(chan);
    u8* st = status + 12 * chan;
    const auto fail = [&](s8 error) {
      std::memset(st, 0, 10);
      st[10] = u8(error);
    };
    u32 pending = 0, resetting = 0, resetting_chan = 0, enabled = 0;
    PadVar(guest, kPendingBits, pending);
    if (pending & bit)
    {
      PadReset(guest, 0, false);
      fail(-2);  // PAD_ERR_NOT_READY
      continue;
    }
    PadVar(guest, kResettingBits, resetting);
    PadVar(guest, kResettingChan, resetting_chan);
    if ((resetting & bit) || resetting_chan == chan)
    {
      fail(-2);
      continue;
    }
    PadVar(guest, kEnabledBits, enabled);
    if (!(enabled & bit))
    {
      fail(-1);  // PAD_ERR_NO_CONTROLLER
      continue;
    }
    // A transfer the original left running (savestate): SIIsChanBusy.
    u32 packet_chan = 0xFFFFFFFFu, si_chan = 0xFFFFFFFFu;
    PadVar(guest, kSiPacketChan + 32 * chan, packet_chan);
    PadVar(guest, kSiChan, si_chan);
    if (packet_chan != 0xFFFFFFFFu || si_chan == chan)
    {
      fail(-3);  // PAD_ERR_TRANSFER
      continue;
    }
    u32 analog_mode = 0, waiting = 0;
    PadVar(guest, kAnalogMode, analog_mode);
    u32 hi = 0, lo = 0;
    if (!GetHostPadData(static_cast<int>(chan), analog_mode, hi, lo))
    {
      // The controller stopped answering.
      PadVar(guest, kWaitingBits, waiting);
      if (waiting & bit)
      {
        fail(0);
        continue;
      }
      u32 checking = 0, barrel = 0;
      PadVar(guest, kCheckingBits, checking);
      PadVar(guest, kBarrelBits, barrel);
      PadVar(guest, kPendingBits, pending);
      PadSetVar(guest, kEnabledBits, enabled & ~bit);
      PadSetVar(guest, kWaitingBits, waiting & ~bit);
      PadSetVar(guest, kCheckingBits, checking & ~bit);
      PadSetVar(guest, kPendingBits, pending & ~bit);
      PadSetVar(guest, kBarrelBits, barrel & ~bit);
      ++m_pad->disconnects;
      fail(-1);
      continue;
    }
    u32 type = 0;
    PadVar(guest, kPadType + 4 * chan, type);
    if (!(type & kTypeNoMotor))
      motor |= bit;
    u32 barrel = 0;
    PadVar(guest, kBarrelBits, barrel);
    const bool origin_request =
        MakeStatusSpec2(st, hi, lo, analog_mode, type, origins + 12 * chan, previous + 12 * chan,
                        bit, barrel, reset_idle);
    PadSetVar(guest, kBarrelBits, barrel);
    if (origin_request)
    {
      // The SDK reads the origin again (PADOriginUpdateCallback).
      fail(-3);
      PadReadOrigin(guest, chan);
      PadUpdateOrigin(guest, chan);
      ++m_pad->origin_requests;
      continue;
    }
    st[10] = 0;  // PAD_ERR_NONE
    st[1] = u8(st[1] & ~0x80);
  }
  if (reset_idle)
    PadSetVar(guest, kSiIdleCount, 0);
  ++m_pad->reads;
  guest.gpr[3] = motor;
  guest.pc = guest.lr;
  guest.downcount -= kPadCycles;
  return true;
}

bool NativeSdk::PadRun(CPUState& guest)
{
  if (guest.pc != kPadReset && guest.pc != kPadRecalibrate && guest.pc != kPadControlMotor)
    return false;
  if (!PadReady(guest))
    return false;
  PadState& pad = *m_pad;
  if (guest.pc == kPadControlMotor)
  {
    // PADControlMotor(chan, cmd): rumble on the PC controller.
    const u32 chan = guest.gpr[3];
    u32 cmd = guest.gpr[4];
    u32 enabled = 0, type = 0, spec = 0;
    PadVar(guest, kEnabledBits, enabled);
    if (chan < 4 && (enabled & ChanBit(chan)) && PadVar(guest, kPadType + 4 * chan, type) &&
        !(type & kTypeNoMotor))
    {
      PadVar(guest, kSpec, spec);
      if (spec < 2 && cmd == 2)
        cmd = 0;
      const u8* sram = Pointer(guest, kSramFlags, 1);
      if (sram && (*sram & 0x20))
        cmd = 0;
      // 1 rumbles; 0 stops, 2 stops hard (as Dolphin's standard controller).
      const int strength = (cmd & 3) == 1 ? 1 : 0;
      if (pad.rumble[chan] != strength)
      {
        pad.rumble[chan] = strength;
        if (HostGamePad* host = GetHostGamePad())
          host->Rumble(static_cast<int>(chan), strength != 0);
      }
    }
    ++pad.motors;
  }
  else
  {
    const bool recalibrate = guest.pc == kPadRecalibrate;
    PadReset(guest, guest.gpr[3], recalibrate);
    ++(recalibrate ? pad.recalibrates : pad.resets);
    guest.gpr[3] = 1;
  }
  guest.pc = guest.lr;
  guest.downcount -= kPadCycles;
  return true;
}

void NativeSdk::PrintPadStats() const
{
  if (!m_pad)
    return;
  const PadState& pad = *m_pad;
  std::fprintf(stderr,
               "[native_pad] active=%d abandoned=%d reads=%llu resets=%llu recalibrates=%llu "
               "motors=%llu enabled=%llu disconnects=%llu origin_requests=%llu chained=%llu "
               "deferred=%llu\n",
               pad.active, pad.abandoned, static_cast<unsigned long long>(pad.reads),
               static_cast<unsigned long long>(pad.resets),
               static_cast<unsigned long long>(pad.recalibrates),
               static_cast<unsigned long long>(pad.motors),
               static_cast<unsigned long long>(pad.enabled),
               static_cast<unsigned long long>(pad.disconnects),
               static_cast<unsigned long long>(pad.origin_requests),
               static_cast<unsigned long long>(pad.chained),
               static_cast<unsigned long long>(pad.deferred));
  if (m_si_init_probes || m_si_register_accesses)
  {
    std::fprintf(stderr, "[native_pad] SI: init probes completed natively=%llu register accesses=%llu\n",
                 static_cast<unsigned long long>(m_si_init_probes),
                 static_cast<unsigned long long>(m_si_register_accesses));
  }
}

// ---- the serial interface at boot (phase D closure) ----
//
// SIInit (0x802460FC) registers the SI interrupt handler, waits for the SI
// command register to idle, and probes the four channels with SIGetType, whose
// transfers the emulated SI answered with the controller type (0x09000000 for
// a GameCube controller, SI_ERROR_NO_RESPONSE for an empty port) through
// GetTypeCallback. With the native PAD library nobody else uses the SI, so the
// probes complete in the act with the PC controller's state and the SI
// registers are plain storage that always reads idle.

namespace
{
constexpr u32 kSiTypeGcController = 0x09000000;
constexpr u32 kSiErrorNoResponse = 0x00000008;
constexpr u32 kSiErrorBusy = 0x00000080;
constexpr u32 kSiRegisterBase = 0xCD006400;
}  // namespace

bool NativeSdk::SiInitGetType(CPUState& guest)
{
  if (std::find(kSiInitReturns.begin(), kSiInitReturns.end(), guest.lr) == kSiInitReturns.end())
    return false;  // a real SIGetType from somewhere else: the original runs
  if (!m_si_code_checked)
  {
    m_si_code_checked = true;
    if (!CodeMatches(guest, kSiCode))
    {
      std::fprintf(stderr, "[native_pad] si.a differs from the original: SIInit left to it\n");
      m_si_get_type = false;
      return false;
    }
  }
  const u32 chan = guest.gpr[3];
  if (chan > 3)
    return false;
  // What GetTypeCallback leaves after the probe: the type and its time.
  const u32 type = HostPadConnected(static_cast<int>(chan)) ? kSiTypeGcController : kSiErrorNoResponse;
  const u64 now = StaticRecompNativeClock::NativeClock::CurrentTimeBase();
  u8* type_ptr = Pointer(guest, kSiType + 4 * chan, 4);
  u8* time_ptr = Pointer(guest, kSiTypeTime + 8 * chan, 8);
  if (!type_ptr || !time_ptr)
    return false;
  Store32(type_ptr, type);
  Store32(time_ptr, static_cast<u32>(now >> 32));
  Store32(time_ptr + 4, static_cast<u32>(now));
  ++m_si_init_probes;
  // The original returned the previous type (busy, on the first probe).
  guest.gpr[3] = kSiErrorBusy;
  guest.pc = guest.lr;
  guest.downcount -= 120;
  return true;
}

bool NativeSdk::SiRegisterRead(u32 ea, u8 size, u64* value)
{
  if (!m_pad || (ea & 0xFFFFFF00u) != kSiRegisterBase || size != 4 || (ea & 3))
    return false;
  ++m_si_register_accesses;
  // Command register (0x34): the transfer-start bit never stays set, nothing
  // transfers. Everything else reads back what the SDK wrote.
  u32 word = m_si_regs[(ea & 0xFF) / 4];
  if ((ea & 0xFF) == 0x34)
    word &= ~1u;
  *value = word;
  return true;
}

bool NativeSdk::SiRegisterWrite(u32 ea, u8 size, u64 value)
{
  if (!m_pad || (ea & 0xFFFFFF00u) != kSiRegisterBase || size != 4 || (ea & 3))
    return false;
  ++m_si_register_accesses;
  u32 word = static_cast<u32>(value);
  if ((ea & 0xFF) == 0x34)
    word &= ~0x80000001u;  // interrupt acknowledge and transfer start are not state
  m_si_regs[(ea & 0xFF) / 4] = word;
  return true;
}
}  // namespace StaticRecompNativeSdk
