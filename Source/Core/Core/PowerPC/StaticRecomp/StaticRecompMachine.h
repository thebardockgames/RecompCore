// RecompCore: StaticRecomp CPU core - the native machine (phase L1).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The devices of the port (event clock, interrupt controller, EXI, the SDK
// layer with audio/PAD/NAND/disc, the GX stream front, the video interface)
// and the dispatch of the hardware registers the generated code reaches to
// them. The CPU core (the chassis's StaticRecompCore, or the port's own
// runner) owns one machine; nothing here needs the chassis.

#pragma once

#include <memory>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompNativeSdk
{
class NativeSdk;
}
namespace StaticRecompNativeGp
{
class NativeGp;
}
namespace StaticRecompNativeVi
{
class NativeVi;
}
namespace StaticRecompNativeClock
{
class NativeClock;
}
namespace StaticRecompNativePi
{
class NativePi;
}
namespace StaticRecompNativeExi
{
class NativeExi;
}

namespace StaticRecompState
{
class StateStream;
}

namespace StaticRecompMachine
{
class Machine
{
public:
  Machine();
  ~Machine();
  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;

  // A hardware register access (effective address, 1/2/4/8 bytes) served by
  // a native device. false: no native device takes it.
  bool Read(u32 ea, u8 size, u64* value);
  bool Write(u32 ea, u8 size, u64 value);

  // The native sections of a savestate/checkpoint (version 9 layout).
  void DoState(StaticRecompState::StateStream& p, bool present);
  // Before writing a native checkpoint: the devices the game has not touched
  // yet take their state over now.
  void SeedForCheckpoint();

  // Declaration order is the destruction order's reverse: the EXI, PI and
  // clock outlive the modules that raise lines and schedule on them.
  std::unique_ptr<StaticRecompNativeSdk::NativeSdk> sdk;
  std::unique_ptr<StaticRecompNativeGp::NativeGp> gp;
  std::unique_ptr<StaticRecompNativeVi::NativeVi> vi;
  std::unique_ptr<StaticRecompNativeClock::NativeClock> clock;
  std::unique_ptr<StaticRecompNativePi::NativePi> pi;
  std::unique_ptr<StaticRecompNativeExi::NativeExi> exi;
};
}  // namespace StaticRecompMachine
