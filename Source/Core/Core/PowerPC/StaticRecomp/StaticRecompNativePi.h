// RecompCore: StaticRecomp CPU core - native interrupt controller (phase F).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The processor interface's interrupt cause and mask registers (0xCC003000
// and 0xCC003004), the reset code and the revision word, owned by the native
// runtime. Every interrupt line of the native modules (VI, GP, audio, the
// native callback line) is raised here; the lines the chassis still owns
// (EXI and SI at boot) are forwarded here by ProcessorInterfaceManager. The
// FIFO registers of the same page belong to the native GP front.
//
// Until phase S the pending external interrupt is signalled through the
// chassis's PowerPC state flag, which its slice loop turns into the 0x500
// exception; the registers themselves never reach the chassis.

#pragma once

#include <functional>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRegs.h"

namespace StaticRecompState
{
class StateStream;
}
namespace StaticRecompNativePi
{
// The interrupt lines of the processor interface (cause/mask bits).
namespace Line
{
constexpr u32 kPi = 0x1;
constexpr u32 kResetSwitch = 0x2;
constexpr u32 kDi = 0x4;
constexpr u32 kSi = 0x8;
constexpr u32 kExi = 0x10;
constexpr u32 kAi = 0x20;
constexpr u32 kDsp = 0x40;
constexpr u32 kMemory = 0x80;
constexpr u32 kVi = 0x100;
constexpr u32 kPeToken = 0x200;
constexpr u32 kPeFinish = 0x400;
constexpr u32 kCp = 0x800;
constexpr u32 kDebug = 0x1000;
constexpr u32 kHsp = 0x2000;
constexpr u32 kWiiIpc = 0x4000;
}  // namespace Line

class NativePi
{
public:
  // What the native modules call: the native controller when it is active,
  // the chassis's otherwise (MODERNGEKKO_NATIVE_PI=off). No module needs the
  // chassis's ProcessorInterface for its lines.
  static void Raise(u32 line, bool set);
  static void EnableLines(u32 lines);

  // What the controller needs from the chassis while it exists (installed by
  // StaticRecompNativePiChassis, the oracle build's adapter): its controller
  // when ours is off, the lines it still raises, the register values a `.sav`
  // carries, its PowerPC exception word before the guest owns the registers,
  // and the Hollywood GPIO values at boot. Without an adapter the controller
  // starts from the hardware's reset values.
  struct ChassisHooks
  {
    std::function<void(u32 line, bool set)> raise;
    std::function<void(u32 lines)> enable_lines;
    std::function<void(std::function<bool(u32, bool)> forward)> set_forward;
    // False when the chassis has no initialised controller to copy (the
    // native host loop skips it): the power-on values are used instead.
    std::function<bool(u32& cause, u32& mask, u32& reset_code)> seed;
    std::function<void(u32 cause, u32 mask, u32 reset_code)> copy;
    std::function<u32*()> exceptions;
    std::function<void(u32& out, u32& dir)> gpio_seed;
  };
  static void SetChassisHooks(ChassisHooks hooks);
  // The port's registers while the guest has the authority (the pending lines
  // are raised there); nullptr = the chassis's mirror.
  void SetGuestRegs(StaticRecompGuestRegs::GuestRegs* regs) { m_regs = regs; }

  NativePi();
  ~NativePi();

  static NativePi* Instance() { return s_instance; }

  bool Active();
  bool Read(u32 ea, u8 size, u64* value);
  bool Write(u32 ea, u8 size, u64 value);

  void SetInterrupt(u32 cause_mask, bool set);
  void SetMaskBits(u32 bits);
  // Hollywood GPIO (0xCD8000C0 OUT, C4 DIR, C8 IN): the only Hollywood
  // registers the game touches (the I2C to the AV encoder in __VISendI2CData,
  // ~7500 accesses per boot). OUT/DIR hold what the PPC may own; IN reports
  // the disc in the slot (the SDA line reads low: the encoder acknowledges).
  bool HollywoodRead(u32 ea, u8 size, u64* value);
  bool HollywoodWrite(u32 ea, u8 size, u64 value);
  // The Flipper memory interface (0xCC004000, 16-bit registers): the OS
  // writes the protection type, the IRQ mask and one more word at boot and
  // never reads them back; they are plain state here.
  bool MiRead(u32 ea, u8 size, u64* value);
  bool MiWrite(u32 ea, u8 size, u64 value);
  u32 Cause() const { return m_cause; }
  u32 Mask() const { return m_mask; }

  void DoState(StaticRecompState::StateStream& p, u32 version);
  void SeedFromDolphin();
  void CopyToDolphin();
  void PrintStats() const;

  struct Stats
  {
    u64 reads = 0, writes = 0, raised = 0, cleared = 0, forwarded = 0, seeds = 0;
  };

private:
  void UpdateException();

  u32 m_exceptions_fallback = 0;
  StaticRecompGuestRegs::GuestRegs* m_regs = nullptr;
  bool m_disabled = false;
  bool m_seeded = false;
  u32 m_cause = 0;
  u32 m_mask = 0;
  u32 m_reset_code = 0;
  bool m_gpio_seeded = false;
  u32 m_gpio_out = 0;
  u32 m_gpio_dir = 0;
  // IPC control (PPC view: IY2 IY1 X2 Y1 Y2 X1) and PPC IRQ mask (0xCD000004/34).
  u32 m_ipc_ctrl = 0;
  u32 m_ipc_irq_mask = 0;
  u16 m_mi_regs[32] = {};
  Stats m_stats;
  static NativePi* s_instance;
  static ChassisHooks s_chassis;
};
}  // namespace StaticRecompNativePi
