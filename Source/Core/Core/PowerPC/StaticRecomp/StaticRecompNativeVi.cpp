// RecompCore: StaticRecomp CPU core - native video interface (phase D).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeVi.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGx.h"

namespace StaticRecompNativeVi
{
namespace
{
constexpr u32 kViBase = 0xCC002000;
// Register offsets (16-bit halves).
using namespace Reg;
constexpr u32 kVerticalBeam = 0x2C;
constexpr u32 kHorizontalBeam = 0x2E;
constexpr u32 kLast = 0x76;
constexpr std::array<u32, 2> kClockFrequencies{{27000000, 54000000}};
constexpr u32 kNumHalfLinesForSiPoll = (7 * 2) + 1;

u32 Bits(u32 value, u32 shift, u32 count)
{
  return (value >> shift) & ((1u << count) - 1);
}
}  // namespace

NativeVi* NativeVi::s_instance = nullptr;
NativeVi::ChassisHooks NativeVi::s_chassis;

void NativeVi::SetChassisHooks(ChassisHooks hooks)
{
  s_chassis = std::move(hooks);
}

void NativeVi::PresetRegisters(RegisterFile& regs, const PresetConfig& config)
{
  // The chassis's VideoInterfaceManager::Preset, field by field.
  regs.fill(0);
  const auto set32 = [&](u32 hi_offset, u32 value) {
    regs[hi_offset / 2] = static_cast<u16>(value >> 16);
    regs[hi_offset / 2 + 1] = static_cast<u16>(value);
  };
  regs[kVerticalTiming / 2] = 6;                       // EQU 6, ACV 0
  regs[kControl / 2] = 1 | ((config.ntsc ? 0 : 1) << 8);  // ENB, FMT
  set32(kHTiming0Hi, 429u | (105u << 16) | (71u << 24));  // HLW, HCE, HCS
  set32(kHTiming1Hi, 64u | (162u << 7) | (373u << 17));   // HSY, HBE640, HBS640
  set32(kVBlankOddHi, 502u | (5u << 16));                 // PRB, PSB
  set32(kVBlankEvenHi, 503u | (4u << 16));
  set32(kBurstOddHi, 12u | (520u << 5) | (12u << 16) | (520u << 21));  // BS0 BE0 BS2 BE2
  set32(kBurstEvenHi, 13u | (519u << 5) | (13u << 16) | (519u << 21));
  set32(kDisplayInterrupt0Hi, 430u | (263u << 16) | (1u << 28));  // HCT VCT IR_MASK
  set32(kDisplayInterrupt0Hi + 4, 1u | (1u << 16) | (1u << 28));
  regs[kHScaleW / 2] = 40 | (40 << 8);  // STD, WPL
  regs[kClock / 2] = config.clock_54mhz ? 1 : 0;
  regs[kDtvStatus / 2] = (config.component ? 1 : 0) | (config.ntsc_j ? 2 : 0);
}

NativeVi::NativeVi()
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_VI");
  m_disabled = setting && (std::string(setting) == "off" || std::string(setting) == "0");
  const char* lazy = std::getenv("MODERNGEKKO_NATIVE_VI_LAZY");
  m_lazy = lazy && std::string(lazy) == "1";
  s_instance = this;
}

NativeVi::~NativeVi()
{
  if (m_event_registered)
  {
    if (auto* clock = StaticRecompNativeClock::NativeClock::Instance())
      clock->Remove(m_event);
  }
  if (s_instance == this)
    s_instance = nullptr;
}

bool NativeVi::Enabled()
{
  return !m_disabled;
}

bool NativeVi::Active()
{
  if (!Enabled())
    return false;
  if (!m_seeded)
    Seed();
  return true;
}

void NativeVi::Seed()
{
  m_seeded = true;
  ++m_stats.seeds;
  SeedRegisters();
  auto* clock = StaticRecompNativeClock::NativeClock::Instance();
  if (!m_event_registered && clock)
  {
    m_event = clock->Register("NativeVI", [this](u64, s64) { OnEvent(); });
    m_event_registered = true;
  }
  // Dolphin's half-line event stops; ours takes over at the same cadence,
  // waking only at the half lines where something happens.
  if (s_chassis.remove_chassis_events)
    s_chassis.remove_chassis_events();
  m_count_time = Now();
  m_current_hl_ticks = TicksPerHalfLine();
  Reschedule();
  StaticRecompNativeGx::SetTicksPerFieldProvider([this] { return TicksPerField(); });
  std::fprintf(stderr,
               "[native_vi] video interface owned natively: registers, display interrupts and the "
               "%s clock (%u ticks per half line, %u half lines per field)\n",
               m_lazy ? "lazy field" : "half-line", TicksPerHalfLine(), HalfLinesPerEvenField());
}

u32 NativeVi::Reg32(u32 hi_offset) const
{
  return (u32(m_regs[hi_offset / 2]) << 16) | m_regs[hi_offset / 2 + 1];
}

u32 NativeVi::HalfLinesPerEvenField() const
{
  const u32 vt = m_regs[kVerticalTiming / 2];
  const u32 even = Reg32(kVBlankEvenHi);
  return 3 * Bits(vt, 0, 4) + Bits(even, 0, 10) + 2 * Bits(vt, 4, 10) + Bits(even, 16, 10);
}

u32 NativeVi::HalfLinesPerOddField() const
{
  const u32 vt = m_regs[kVerticalTiming / 2];
  const u32 odd = Reg32(kVBlankOddHi);
  return 3 * Bits(vt, 0, 4) + Bits(odd, 0, 10) + 2 * Bits(vt, 4, 10) + Bits(odd, 16, 10);
}

u32 NativeVi::TicksPerHalfLine() const
{
  const u32 ticks_per_sample =
      2 * StaticRecompNativeClock::NativeClock::CurrentTicksPerSecond() / kClockFrequencies[m_regs[kClock / 2] & 1];
  return ticks_per_sample * Bits(Reg32(kHTiming0Hi), 0, 10);  // HLW
}

u32 NativeVi::TicksPerField() const
{
  return TicksPerHalfLine() * HalfLinesPerEvenField();
}

void NativeVi::UpdateParameters()
{
  const u32 vt = m_regs[kVerticalTiming / 2];
  const u32 equ_hl = 3 * Bits(vt, 0, 4);
  const u32 acv_hl = 2 * Bits(vt, 4, 10);
  const u32 odd = Reg32(kVBlankOddHi), even = Reg32(kVBlankEvenHi);
  const u32 odd_prb = Bits(odd, 0, 10), odd_psb = Bits(odd, 16, 10);
  const u32 even_prb = Bits(even, 0, 10), even_psb = Bits(even, 16, 10);
  m_odd_field_first_hl = equ_hl + odd_prb;
  const u32 odd_field_end = m_odd_field_first_hl + acv_hl;
  m_odd_field_last_hl = odd_field_end - 1;
  const s32 odd_even_psb_diff = static_cast<s32>(odd_psb) - static_cast<s32>(even_psb);
  m_even_field_first_hl = odd_field_end + odd_psb + equ_hl + even_prb - odd_even_psb_diff;
  m_even_field_last_hl = m_even_field_first_hl + acv_hl - 1;
}

// ---- registers ----

bool NativeVi::Read(u32 ea, u8 size, u64* value)
{
  if ((ea & 0xFFFFF000) != kViBase || !Active())
    return false;
  const u32 offset = ea & 0xFFF;
  if (offset > kLast + 1)
    return false;
  ++m_stats.reads;
  switch (size)
  {
  case 1:
  {
    const u16 half = ReadHalf(offset & ~1u);
    *value = (offset & 1) ? (half & 0xFF) : (half >> 8);
    return true;
  }
  case 2:
    *value = ReadHalf(offset & ~1u);
    return true;
  case 4:
    *value = (u32(ReadHalf(offset & ~3u)) << 16) | ReadHalf((offset & ~3u) + 2);
    return true;
  default:
    return false;
  }
}

bool NativeVi::Write(u32 ea, u8 size, u64 value)
{
  if ((ea & 0xFFFFF000) != kViBase || !Active())
    return false;
  const u32 offset = ea & 0xFFF;
  if (offset > kLast + 1)
    return false;
  ++m_stats.writes;
  switch (size)
  {
  case 2:
    WriteHalf(offset & ~1u, static_cast<u16>(value));
    return true;
  case 4:
    WriteHalf(offset & ~3u, static_cast<u16>(value >> 16));
    WriteHalf((offset & ~3u) + 2, static_cast<u16>(value));
    return true;
  default:
    return false;  // 8-bit writes are invalid on the hardware too
  }
}

u16 NativeVi::ReadHalf(u32 reg)
{
  switch (reg)
  {
  case kVerticalBeam:
    Materialize(Now());
    return static_cast<u16>(1 + m_half_line_count / 2);
  case kHorizontalBeam:
  {
    const u32 hlw = Bits(Reg32(kHTiming0Hi), 0, 10);
    const u64 ticks = Now();
    Materialize(ticks);
    const u32 per_half_line = std::max<u32>(TicksPerHalfLine(), 1);
    const u16 value = static_cast<u16>(1 + hlw * (ticks - m_ticks_last_line_start) / per_half_line);
    return std::clamp<u16>(value, 1, static_cast<u16>(hlw * 2));
  }
  default:
    return m_regs[reg / 2];
  }
}

void NativeVi::WriteHalf(u32 reg, u16 value)
{
  switch (reg)
  {
  case kVerticalTiming:
  case kHTiming0Hi:
  case kHTiming0Hi + 2:
  case kVBlankOddHi:
  case kVBlankOddHi + 2:
  case kVBlankEvenHi:
  case kVBlankEvenHi + 2:
  case kClock:
    Materialize(Now());
    m_regs[reg / 2] = value;
    UpdateParameters();
    Reschedule();
    return;
  case kFbLeftTopHi:
  case kFbLeftBottomHi:
  case kFbRightTopHi:
  case kFbRightBottomHi:
    // Setting a CLRPOFF bit (29..31 of the 32-bit register) clears POFF (bit 28).
    m_regs[reg / 2] = (value & 0xE000) ? (value & ~0x1000) : value;
    return;
  case kVerticalBeam:
  case kHorizontalBeam:
    return;  // not writable
  case kDisplayInterrupt0Hi:
  case kDisplayInterrupt0Hi + 4:
  case kDisplayInterrupt0Hi + 8:
  case kDisplayInterrupt0Hi + 12:
    // VCT, mask (bit 12) and status (bit 15): the handler clears the status by
    // writing the half back without it.
    Materialize(Now());
    m_regs[reg / 2] = value;
    UpdateInterrupts();
    Reschedule();
    return;
  case kControl:
  {
    // Only ENB, NIN, DLR, LE0, LE1 and FMT are kept; RST clears the display
    // interrupts and enters idle.
    const u16 kept = value & 0x03FD;
    Materialize(Now());
    m_regs[reg / 2] = kept;
    if (value & 0x0002)
    {
      for (u32 i = 0; i < 4; ++i)
      {
        m_regs[(kDisplayInterrupt0Hi + 4 * i) / 2] = 0;
        m_regs[(kDisplayInterrupt0Hi + 4 * i) / 2 + 1] = 0;
      }
      UpdateInterrupts();
    }
    UpdateParameters();
    Reschedule();
    return;
  }
  default:
    m_regs[reg / 2] = value;
    return;
  }
}

void NativeVi::UpdateInterrupts()
{
  bool any = false;
  for (u32 i = 0; i < 4; ++i)
  {
    const u16 hi = m_regs[(kDisplayInterrupt0Hi + 4 * i) / 2];
    if ((hi & 0x8000) && (hi & 0x1000))
      any = true;
  }
  if (any && !m_interrupt_set)
    ++m_stats.interrupts;
  m_interrupt_set = any;
  StaticRecompNativePi::NativePi::Raise(StaticRecompNativePi::Line::kVi, any);
}

// ---- the field clock ----
//
// Dolphin ran a callback every half line. The same state machine runs here,
// but only the boundaries where something happens get an event (field starts
// and ends, the two field boundaries, the four display interrupt lines); the
// boundaries in between are settled lazily, at the next event or at a beam
// position read, with the same arithmetic, so every action keeps its cycle.

u64 NativeVi::Now() const
{
  return StaticRecompNativeClock::NativeClock::CurrentTicks();
}

bool NativeVi::InterestingBoundary(u32 count) const
{
  // The boundary where Dolphin processed index `count` and moved to count + 1.
  if (!m_lazy)
    return true;  // every half line, as Dolphin's callback
  if (count == 0 || count == HalfLinesPerOddField() || count == m_odd_field_first_hl ||
      count == m_even_field_first_hl || count == m_odd_field_last_hl ||
      count == m_even_field_last_hl)
  {
    return true;
  }
  const u32 total = HalfLinesPerEvenField() + HalfLinesPerOddField();
  u32 next = count + 1;
  if (next == total)
    next = 0;
  const u32 hlw = Bits(Reg32(kHTiming0Hi), 0, 10);
  for (u32 i = 0; i < 4; ++i)
  {
    const u32 reg = Reg32(kDisplayInterrupt0Hi + 4 * i);
    const u32 hct = Bits(reg, 0, 11), vct = Bits(reg, 16, 11);
    const u32 target_halfline = hct > hlw ? 1 : 0;
    if (1 + next / 2 == vct && (next & 1) == target_halfline)
      return true;
  }
  return false;
}

void NativeVi::ProcessBoundary(u64 ticks)
{
  // Dolphin's Update(ticks) with the current count.
  ++m_stats.half_lines;
  constexpr u32 odd_field_begin = 0;
  const u32 even_field_begin = HalfLinesPerOddField();
  const bool is_at_field_boundary =
      m_half_line_count == odd_field_begin || m_half_line_count == even_field_begin;
  if (is_at_field_boundary && s_chassis.field_boundary)
    s_chassis.field_boundary();
  if (m_half_line_count == m_odd_field_first_hl)
    BeginField(Field::Odd, ticks);
  else if (m_half_line_count == m_even_field_first_hl)
    BeginField(Field::Even, ticks);
  else if (m_half_line_count == m_odd_field_last_hl)
    EndField(Field::Odd, ticks);
  else if (m_half_line_count == m_even_field_last_hl)
    EndField(Field::Even, ticks);
  if (is_at_field_boundary && s_chassis.new_field)
    s_chassis.new_field();
  // No serial-interface poll: the native PAD library reads the PC controller.
  ++m_half_line_count;
  if (m_half_line_count == HalfLinesPerEvenField() + HalfLinesPerOddField())
    m_half_line_count = 0;
  m_count_time = ticks;
  m_current_hl_ticks = TicksPerHalfLine();
  if (!(m_half_line_count & 1))
    m_ticks_last_line_start = ticks;
  // Display interrupts at their line (half-line granularity, as Dolphin).
  const u32 hlw = Bits(Reg32(kHTiming0Hi), 0, 10);
  for (u32 i = 0; i < 4; ++i)
  {
    const u32 reg = Reg32(kDisplayInterrupt0Hi + 4 * i);
    const u32 hct = Bits(reg, 0, 11), vct = Bits(reg, 16, 11);
    const u32 target_halfline = hct > hlw ? 1 : 0;
    if (1 + m_half_line_count / 2 == vct && (m_half_line_count & 1) == target_halfline)
      m_regs[(kDisplayInterrupt0Hi + 4 * i) / 2] |= 0x8000;
  }
  UpdateInterrupts();
}

void NativeVi::Materialize(u64 now)
{
  // Settle the boundaries already passed that carry no action; one with an
  // action waits for its event (which may already be due), as Dolphin's late
  // callback did.
  if (!m_seeded || m_current_hl_ticks == 0)
    return;
  while (true)
  {
    const u64 boundary = m_count_time + m_current_hl_ticks;
    if (boundary > now || InterestingBoundary(m_half_line_count))
      break;
    ProcessBoundary(boundary);
  }
}

void NativeVi::Reschedule()
{
  auto* clock = StaticRecompNativeClock::NativeClock::Instance();
  if (!clock || !m_event || !m_seeded)
    return;
  clock->Remove(m_event);
  const u32 total = HalfLinesPerEvenField() + HalfLinesPerOddField();
  const u32 per_half_line = std::max<u32>(TicksPerHalfLine(), 1);
  u32 count = m_half_line_count;
  u64 time = m_count_time + std::max<u32>(m_current_hl_ticks, 1);
  for (u32 i = 0; i <= total; ++i)
  {
    if (InterestingBoundary(count))
      break;
    count = (count + 1 == total) ? 0 : count + 1;
    time += per_half_line;
  }
  clock->Schedule(static_cast<s64>(time) - static_cast<s64>(clock->Now()), m_event);
}

void NativeVi::OnEvent()
{
  ++m_stats.events;
  const u64 now = Now();
  Materialize(now);
  const u64 boundary = m_count_time + m_current_hl_ticks;
  if (boundary <= now && InterestingBoundary(m_half_line_count))
    ProcessBoundary(boundary);
  Reschedule();
}

void NativeVi::OutputField(Field field, u64 ticks)
{
  // Same as Dolphin's OutputField: the presenter of the Dolphin renderer gets
  // the XFB; with NativeGX (Null backend) it is a no-op.
  const u16 hsw = m_regs[kHScaleW / 2];
  const u32 std_words = Bits(hsw, 0, 8), wpl = Bits(hsw, 8, 7);
  const bool potentially_interlaced_xfb = wpl != 0 && (std_words / wpl) == 2;
  const bool interlaced_video_mode = (HalfLinesPerEvenField() & 1) == 1;
  u32 fb_stride = std_words * 16;
  const u32 fb_width = wpl * 16;
  u32 fb_height = Bits(m_regs[kVerticalTiming / 2], 4, 10);
  const u32 top = Reg32(kFbLeftTopHi), bottom = Reg32(kFbLeftBottomHi);
  const bool poff = (top & (1u << 28)) != 0;  // bottom's POFF is wired to top's
  u32 xfb = field == Field::Even ? Bits(bottom, 0, 24) : Bits(top, 0, 24);
  if (poff)
    xfb <<= 5;
  fb_stride *= 2;
  if (potentially_interlaced_xfb && interlaced_video_mode && s_chassis.force_progressive &&
      s_chassis.force_progressive())
  {
    const u32 odd_prb = Bits(Reg32(kVBlankOddHi), 0, 10), even_prb = Bits(Reg32(kVBlankEvenHi), 0, 10);
    fb_stride /= 2;
    fb_height *= 2;
    if (field == Field::Odd && odd_prb == even_prb + 1 && xfb)
      xfb -= fb_stride;
    if (field == Field::Even && odd_prb == even_prb - 1 && xfb)
      xfb -= fb_stride;
  }
  if (xfb && s_chassis.output_xfb)
    s_chassis.output_xfb(xfb, fb_width, fb_stride, fb_height, ticks);
}

void NativeVi::BeginField(Field field, u64 ticks)
{
  if (s_chassis.early_xfb_output && s_chassis.early_xfb_output())
    OutputField(field, ticks);
}

void NativeVi::EndField(Field field, u64 ticks)
{
  ++m_stats.fields;
  if (!(s_chassis.early_xfb_output && s_chassis.early_xfb_output()))
    OutputField(field, ticks);
  // The real-time limiter of the native clock.
  if (!(s_chassis.immediate_xfb && s_chassis.immediate_xfb()))
  {
    auto* clock = StaticRecompNativeClock::NativeClock::Instance();
    if (clock && clock->OwnsThrottle())
      clock->Throttle(static_cast<s64>(ticks));
    else if (s_chassis.throttle)
      s_chassis.throttle(ticks);
  }
  if (s_chassis.end_field)
    s_chassis.end_field();
}

// ---- initial registers: the chassis's copy while it exists, else power-on ----

void NativeVi::SeedRegisters()
{
  RegisterFile preset{};
  const PresetConfig config = s_chassis.preset_config ? s_chassis.preset_config() : PresetConfig{};
  PresetRegisters(preset, config);
  if (s_chassis.seed)
  {
    s_chassis.seed(m_regs, m_ticks_last_line_start, m_half_line_count);
    if (m_stats.seeds == 1 && m_half_line_count == 0 && m_ticks_last_line_start == 0)
    {
      // A fresh boot: the chassis holds its power-on values; ours must match.
      u32 differ = 0;
      for (size_t i = 0; i < preset.size(); ++i)
        differ += preset[i] != m_regs[i];
      std::fprintf(stderr, "[native_vi] power-on registers: %s (%u halves differ from the chassis)\n",
                   differ ? "MISMATCH" : "identical to the chassis", differ);
    }
  }
  else
  {
    m_regs = preset;
    m_ticks_last_line_start = 0;
    m_half_line_count = 0;
  }
  m_interrupt_set = false;
  UpdateParameters();
  UpdateInterrupts();
}

void NativeVi::DoState(StaticRecompState::StateStream& p, u32 version)
{
  p.Do(m_seeded);
  p.Do(m_regs);
  p.Do(m_ticks_last_line_start);
  p.Do(m_half_line_count);
  p.Do(m_interrupt_set);
  if (version >= 7)
  {
    p.Do(m_count_time);
    p.Do(m_current_hl_ticks);
  }
  if (p.IsReadMode())
  {
    UpdateParameters();
    if (version < 7)
    {
      // The previous path rescheduled a whole half line from the load.
      m_count_time = Now();
      m_current_hl_ticks = TicksPerHalfLine();
    }
    if (m_seeded && Enabled())
    {
      // Our events are rebuilt from this state; Dolphin's restored VI events
      // (if the state had them) are removed.
      auto* clock = StaticRecompNativeClock::NativeClock::Instance();
      if (!m_event_registered && clock)
      {
        m_event = clock->Register("NativeVI", [this](u64, s64) { OnEvent(); });
        m_event_registered = true;
      }
      if (s_chassis.remove_chassis_events)
        s_chassis.remove_chassis_events();
      Reschedule();
    }
  }
  else if (m_seeded)
  {
    if (s_chassis.copy)  // states stay loadable by the previous path
      s_chassis.copy(m_regs, m_ticks_last_line_start, m_half_line_count);
  }
}

void NativeVi::PrintStats() const
{
  if (!m_seeded)
    return;
  std::fprintf(stderr,
               "[native_vi] reads=%llu writes=%llu half_lines=%llu fields=%llu interrupts=%llu seeds=%llu events=%llu\n",
               static_cast<unsigned long long>(m_stats.reads),
               static_cast<unsigned long long>(m_stats.writes),
               static_cast<unsigned long long>(m_stats.half_lines),
               static_cast<unsigned long long>(m_stats.fields),
               static_cast<unsigned long long>(m_stats.interrupts),
               static_cast<unsigned long long>(m_stats.seeds),
               static_cast<unsigned long long>(m_stats.events));
}
}  // namespace StaticRecompNativeVi
