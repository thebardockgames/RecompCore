// RecompCore: StaticRecomp CPU core - host controller interface.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The native PAD library (StaticRecompNativePad.cpp) and the native PADRead
// read the PC controller through this interface instead of Dolphin's
// GCPad/InputCommon. The runtime registers one provider before booting
// (a device reader, or the input-script player used by the reproducible
// tests). The header depends on nothing from Dolphin so it can move with the
// native runtime (docs/NATIVE_PORT_ROADMAP.md, phase H1).

#pragma once

#include <cstdint>

namespace StaticRecompNativeSdk
{
// Bits of PADStatus::button, as the GameCube controller reports them.
enum HostPadButton : std::uint16_t
{
  kHostPadLeft = 0x0001,
  kHostPadRight = 0x0002,
  kHostPadDown = 0x0004,
  kHostPadUp = 0x0008,
  kHostPadZ = 0x0010,
  kHostPadR = 0x0020,
  kHostPadL = 0x0040,
  kHostPadA = 0x0100,
  kHostPadB = 0x0200,
  kHostPadX = 0x0400,
  kHostPadY = 0x0800,
  kHostPadStart = 0x1000,
};

constexpr std::uint8_t kHostPadStickCenter = 0x80;

// One channel's state, in controller units (the bytes of the poll response).
struct HostPadStatus
{
  bool connected = false;
  std::uint16_t buttons = 0;
  std::uint8_t stick_x = kHostPadStickCenter;
  std::uint8_t stick_y = kHostPadStickCenter;
  std::uint8_t substick_x = kHostPadStickCenter;
  std::uint8_t substick_y = kHostPadStickCenter;
  std::uint8_t trigger_left = 0;
  std::uint8_t trigger_right = 0;
  std::uint8_t analog_a = 0;
  std::uint8_t analog_b = 0;
};

class HostGamePad
{
public:
  virtual ~HostGamePad() = default;
  // Called from the CPU thread on every PADRead / PADReset; must be cheap.
  virtual HostPadStatus Poll(int chan) = 0;
  virtual void Rumble(int /*chan*/, bool /*on*/) {}
};

// The provider the core uses; nullptr means "no controllers connected".
void SetHostGamePad(HostGamePad* pad);
HostGamePad* GetHostGamePad();
}  // namespace StaticRecompNativeSdk
