// RecompCore: StaticRecomp CPU core - native video interface (phase D).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The SDK's vi.a programs the Video Interface registers at 0xCC002000
// (__VIInit, VIConfigure, VIFlush, VISetNextFrameBuffer) and reacts to its
// retrace interrupts (__VIRetraceHandler: display-interrupt status bits,
// VIWaitForRetrace, VIGetCurrentLine, VIGetNextField). Dolphin emulated that
// with VideoInterface plus a half-line event from SystemTimers that also
// polled the serial interface, throttled emulation to real time and presented
// the XFB. This class owns the registers and the half-line clock:
//
//  - register file with the hardware semantics the SDK relies on (timing
//    registers, framebuffer addresses with the page-offset bit, the four
//    display interrupts with mask and status, beam position reads, control);
//  - one event per half line (CoreTiming until phase F) that raises the
//    display interrupts at their line, ends fields (throttle, VI field event,
//    XFB output to the presenter for the Dolphin renderer) and keeps the
//    movie/frame-step callbacks of the chassis;
//  - no serial-interface polling: the native PAD library (phase A) reads the
//    PC controller directly, so SI stays idle (SISetXY becomes a no-op in the
//    native SDK layer).
//
// Dolphin's VideoInterface never sees a register access while this is active
// and its half-line event is removed; ProcessorInterface still carries the
// interrupt (phase F).

#pragma once

#include <array>
#include <functional>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompState
{
class StateStream;
}
namespace StaticRecompNativeClock
{
struct EventType;
}

namespace StaticRecompNativeVi
{
// Register offsets in the 0xCC002000 page (16-bit halves: index = offset / 2).
namespace Reg
{
constexpr u32 kVerticalTiming = 0x00;
constexpr u32 kControl = 0x02;
constexpr u32 kHTiming0Hi = 0x04;  // HLW in the low half (0x06)
constexpr u32 kHTiming1Hi = 0x08;
constexpr u32 kVBlankOddHi = 0x0C;
constexpr u32 kVBlankEvenHi = 0x10;
constexpr u32 kBurstOddHi = 0x14;
constexpr u32 kBurstEvenHi = 0x18;
constexpr u32 kFbLeftTopHi = 0x1C;
constexpr u32 kFbRightTopHi = 0x20;
constexpr u32 kFbLeftBottomHi = 0x24;
constexpr u32 kFbRightBottomHi = 0x28;
constexpr u32 kDisplayInterrupt0Hi = 0x30;  // 0x30, 0x34, 0x38, 0x3C
constexpr u32 kLatch0Hi = 0x40;
constexpr u32 kLatch1Hi = 0x44;
constexpr u32 kHScaleW = 0x48;
constexpr u32 kHScaling = 0x4A;
constexpr u32 kFilterTable0Hi = 0x4C;  // 0x4C..0x54
constexpr u32 kFilterTable3Hi = 0x58;  // 0x58..0x64
constexpr u32 kUnknownAaHi = 0x68;
constexpr u32 kClock = 0x6C;
constexpr u32 kDtvStatus = 0x6E;
constexpr u32 kFbWidth = 0x70;
constexpr u32 kBorderHBlankLo = 0x72;
constexpr u32 kBorderHBlankHi = 0x74;
}  // namespace Reg

using RegisterFile = std::array<u16, 0x40>;

class NativeVi
{
public:
  // The hardware's state at power-on (the values the IPL leaves before the
  // game's VIInit), from the board's region and cabling.
  struct PresetConfig
  {
    bool ntsc = true;          // display format NTSC (else PAL)
    bool clock_54mhz = true;   // 54 MHz clock (progressive-capable, NTSC discs)
    bool component = false;    // component cable plugged (progressive scan setting)
    bool ntsc_j = false;       // DTV NTSC-J flag
  };
  static void PresetRegisters(RegisterFile& regs, const PresetConfig& config);

  // What the VI needs from the chassis while it exists (installed by
  // StaticRecompNativeViChassis): its VI's registers (`.sav` and the
  // previous path), its half-line event to retire, the movie/field
  // callbacks, the presenter of its renderer, its graphics hacks, its
  // limiter and frame-end bookkeeping. Without an adapter the VI starts from
  // PresetRegisters and outputs nothing (NativeGX presents by itself).
  struct ChassisHooks
  {
    std::function<void()> remove_chassis_events;
    std::function<void(RegisterFile& regs, u64& ticks_last_line_start, u32& half_line_count)> seed;
    std::function<void(const RegisterFile& regs, u64 ticks_last_line_start, u32 half_line_count)> copy;
    std::function<PresetConfig()> preset_config;
    std::function<void()> field_boundary;
    std::function<void()> new_field;
    std::function<void(u32 xfb, u32 width, u32 stride, u32 height, u64 ticks)> output_xfb;
    std::function<bool()> force_progressive;
    std::function<bool()> early_xfb_output;
    std::function<bool()> immediate_xfb;
    std::function<void(u64 ticks)> throttle;
    std::function<void()> end_field;
  };
  static void SetChassisHooks(ChassisHooks hooks);

  NativeVi();
  ~NativeVi();

  // Serves the VI once the native PAD library is active (the SI poll is the
  // only thing the VI did for the serial interface); MODERNGEKKO_NATIVE_VI=off
  // keeps Dolphin's.
  bool Active();

  // Register accesses from generated code (effective address in the
  // 0xCC002000 page, 8/16/32-bit). Return true when handled.
  bool Read(u32 ea, u8 size, u64* value);
  bool Write(u32 ea, u8 size, u64 value);

  void DoState(StaticRecompState::StateStream& p, u32 version);
  void SeedRegisters();

  u32 TicksPerField() const;
  u32 TicksPerHalfLine() const;
  void PrintStats() const;

  struct Stats
  {
    u64 reads = 0, writes = 0, half_lines = 0, fields = 0, interrupts = 0, seeds = 0, events = 0;
  };

private:
  enum class Field
  {
    Odd,
    Even
  };
  bool Enabled();
  void Seed();
  u16 ReadHalf(u32 reg);
  void WriteHalf(u32 reg, u16 value);
  u32 Reg32(u32 hi_offset) const;
  u32 HalfLinesPerOddField() const;
  u32 HalfLinesPerEvenField() const;
  void UpdateParameters();
  void UpdateInterrupts();
  u64 Now() const;
  bool InterestingBoundary(u32 count) const;
  void ProcessBoundary(u64 ticks);
  void Materialize(u64 now);
  void Reschedule();
  void OnEvent();
  void OutputField(Field field, u64 ticks);
  void BeginField(Field field, u64 ticks);
  void EndField(Field field, u64 ticks);

  bool m_disabled = false;
  // Lazy mode (MODERNGEKKO_NATIVE_VI_LAZY=1): events only at the half lines
  // with an action; beam reads settle the count. Exact to the hardware model
  // but not to Dolphin's, whose count lagged until its half-line callback ran
  // after the slice: the reference images differ, so the default stays exact.
  bool m_lazy = false;
  bool m_seeded = false;
  bool m_event_registered = false;
  // Raw 16-bit halves, index = offset / 2 (0x00..0x76).
  std::array<u16, 0x40> m_regs{};
  u64 m_ticks_last_line_start = 0;
  u32 m_half_line_count = 0;
  // Dolphin's count took its value at m_count_time; the half line in flight
  // lasts m_current_hl_ticks (timing writes change the following ones only).
  u64 m_count_time = 0;
  u32 m_current_hl_ticks = 0;
  u32 m_odd_field_first_hl = 0, m_odd_field_last_hl = 0;
  u32 m_even_field_first_hl = 0, m_even_field_last_hl = 0;
  bool m_interrupt_set = false;
  StaticRecompNativeClock::EventType* m_event = nullptr;
  Stats m_stats;
  static NativeVi* s_instance;
  static ChassisHooks s_chassis;
};
}  // namespace StaticRecompNativeVi
