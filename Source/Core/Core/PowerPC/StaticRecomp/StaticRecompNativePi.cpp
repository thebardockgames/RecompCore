// RecompCore: StaticRecomp CPU core - native interrupt controller (phase F).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativePi.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"

namespace StaticRecompNativePi
{
namespace
{
constexpr u32 kPiBase = 0xCC003000;
constexpr u32 kCause = 0x00;
constexpr u32 kMask = 0x04;
constexpr u32 kResetCode = 0x24;
constexpr u32 kRevision = 0x2C;
constexpr u32 kFlipperRevC = 0x246500B1;
// Pressed = 0 in the cause register; the state bit is not maskable.
constexpr u32 kResetButtonBit = 0x10000;
constexpr u32 kPowerOnCause = kResetButtonBit | 0x100;  // RST released, VI
// The 750CL's pending external interrupt (Gekko EXCEPTION_EXTERNAL_INT).
constexpr u32 kExceptionExternalInt = 0x00000004;
}  // namespace

NativePi* NativePi::s_instance = nullptr;
NativePi::ChassisHooks NativePi::s_chassis;

void NativePi::SetChassisHooks(ChassisHooks hooks)
{
  s_chassis = std::move(hooks);
}

void NativePi::Raise(u32 line, bool set)
{
  if (s_instance && s_instance->Active())
    s_instance->SetInterrupt(line, set);
  else if (s_chassis.raise)
    s_chassis.raise(line, set);
}

void NativePi::EnableLines(u32 lines)
{
  if (s_instance && s_instance->Active())
    s_instance->SetMaskBits(lines);
  else if (s_chassis.enable_lines)
    s_chassis.enable_lines(lines);
}

NativePi::NativePi()
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_PI");
  m_disabled = setting && (std::string(setting) == "off" || std::string(setting) == "0");
  s_instance = this;
  if (!m_disabled && s_chassis.set_forward)
  {
    // Lines the chassis still raises (EXI, SI at boot, reset button) land here.
    s_chassis.set_forward([this](u32 cause, bool set) {
      if (!Active())
        return false;
      ++m_stats.forwarded;
      SetInterrupt(cause, set);
      return true;
    });
  }
}

NativePi::~NativePi()
{
  if (s_chassis.set_forward)
    s_chassis.set_forward(nullptr);
  if (s_instance == this)
    s_instance = nullptr;
}

bool NativePi::Active()
{
  if (m_disabled)
    return false;
  if (!m_seeded)
  {
    SeedFromDolphin();
    std::fprintf(stderr, "[native_pi] interrupt controller owned natively (cause %08x mask %08x)\n",
                 m_cause, m_mask);
  }
  return true;
}

void NativePi::SeedFromDolphin()
{
  if (s_chassis.seed && s_chassis.seed(m_cause, m_mask, m_reset_code))
  {
  }
  else if (!m_seeded)
  {
    // Power-on: reset button released, the VI line up, nothing enabled, cold
    // reset code (the chassis's ProcessorInterfaceManager::Init).
    m_cause = kPowerOnCause;
    m_mask = 0;
    m_reset_code = 0;
  }
  m_seeded = true;
  ++m_stats.seeds;
}

void NativePi::CopyToDolphin()
{
  if (s_chassis.copy)
    s_chassis.copy(m_cause, m_mask, m_reset_code);
}

void NativePi::UpdateException()
{
  u32* chassis = !m_regs && s_chassis.exceptions ? s_chassis.exceptions() : nullptr;
  u32& exceptions = m_regs ? m_regs->exceptions : chassis ? *chassis : m_exceptions_fallback;
  if ((m_cause & m_mask) != 0)
    exceptions |= kExceptionExternalInt;
  else
    exceptions &= ~kExceptionExternalInt;
}

void NativePi::SetInterrupt(u32 cause_mask, bool set)
{
  if (set)
  {
    if (!(m_cause & cause_mask))
      ++m_stats.raised;
    m_cause |= cause_mask;
  }
  else
  {
    if (m_cause & cause_mask)
      ++m_stats.cleared;
    m_cause &= ~cause_mask;
  }
  UpdateException();
}

void NativePi::SetMaskBits(u32 bits)
{
  m_mask |= bits;
  UpdateException();
}

bool NativePi::Read(u32 ea, u8 size, u64* value)
{
  if ((ea & 0xFFFFF000) != kPiBase || !Active())
    return false;
  const u32 reg = (ea & 0xFFF) & ~3u;
  u32 word;
  switch (reg)
  {
  case kCause:
    word = m_cause;
    break;
  case kMask:
    word = m_mask;
    break;
  case kResetCode:
    word = m_reset_code;
    break;
  case kRevision:
    word = kFlipperRevC;
    break;
  default:
    return false;  // FIFO registers (GP front) or unmapped: not ours
  }
  ++m_stats.reads;
  if (size == 4 && (ea & 3) == 0)
    *value = word;
  else if (size == 2 && (ea & 1) == 0)
    *value = (ea & 2) ? (word & 0xFFFF) : (word >> 16);
  else if (size == 1)
    *value = (word >> (8 * (3 - (ea & 3)))) & 0xFF;
  else
    return false;
  return true;
}

bool NativePi::Write(u32 ea, u8 size, u64 value)
{
  if ((ea & 0xFFFFF000) != kPiBase || size != 4 || (ea & 3) != 0 || !Active())
    return false;
  const u32 reg = ea & 0xFFF;
  const u32 word = static_cast<u32>(value);
  switch (reg)
  {
  case kCause:
    // Writing a bit acknowledges it; the device lines stay as the devices
    // hold them and the reset-button state bit is not clearable.
    ++m_stats.writes;
    m_cause &= ~(word & ~kResetButtonBit);
    UpdateException();
    return true;
  case kMask:
    ++m_stats.writes;
    m_mask = word;
    UpdateException();
    return true;
  case kResetCode:
    ++m_stats.writes;
    m_reset_code = word;
    return true;
  default:
    return false;
  }
}

void NativePi::DoState(StaticRecompState::StateStream& p, u32 version)
{
  p.Do(m_seeded);
  p.Do(m_cause);
  p.Do(m_mask);
  p.Do(m_reset_code);
  if (version >= 8)
  {
    p.Do(m_gpio_seeded);
    p.Do(m_gpio_out);
    p.Do(m_gpio_dir);
    p.Do(m_ipc_ctrl);
    p.Do(m_ipc_irq_mask);
    p.DoArray(m_mi_regs, 32);
  }
  if (p.IsReadMode())
  {
    if (m_seeded)
      UpdateException();
  }
  else if (m_seeded)
  {
    CopyToDolphin();  // the HW chunk stays consistent for older loaders
  }
}

void NativePi::PrintStats() const
{
  if (!m_seeded)
    return;
  std::fprintf(stderr,
               "[native_pi] reads=%llu writes=%llu raised=%llu cleared=%llu forwarded=%llu "
               "seeds=%llu cause=%08x mask=%08x\n",
               static_cast<unsigned long long>(m_stats.reads),
               static_cast<unsigned long long>(m_stats.writes),
               static_cast<unsigned long long>(m_stats.raised),
               static_cast<unsigned long long>(m_stats.cleared),
               static_cast<unsigned long long>(m_stats.forwarded),
               static_cast<unsigned long long>(m_stats.seeds), m_cause, m_mask);
}
namespace
{
// The Hollywood registers appear at 0xCD000000 (the IPC base the SDK uses
// for GPIO from the WPAD library) and at the 0xCD800000 alias.
constexpr u32 kHollywoodPageMask = 0xFF7FF000u, kHollywoodPage = 0xCD000000u;
constexpr u32 kIpcPpcCtrl = 0x04, kIpcPpcIrqMask = 0x34;
constexpr u32 kGpioOut = 0xC0, kGpioDir = 0xC4, kGpioIn = 0xC8;
constexpr u32 kMiBase = 0xCC004000u;
// Pins the PPC may drive (the chassis's gpio_owner): SLOT_LED, SLOT_IN,
// SENSOR_BAR, DO_EJECT, AVE_SCL, AVE_SDA.
constexpr u32 kGpioOwner = 0x20u | 0x80u | 0x100u | 0x200u | 0x4000u | 0x8000u;
constexpr u32 kGpioSlotIn = 0x80u;
}  // namespace

bool NativePi::HollywoodRead(u32 ea, u8 size, u64* value)
{
  if (m_disabled || size != 4 || (ea & kHollywoodPageMask) != kHollywoodPage)
    return false;
  const u32 reg = ea & 0xFFF;
  if (reg == kIpcPpcCtrl)
  {
    ++m_stats.reads;
    *value = m_ipc_ctrl;
    return true;
  }
  if (reg != kGpioOut && reg != kGpioDir && reg != kGpioIn)
    return false;
  if (!m_gpio_seeded)
  {
    if (s_chassis.gpio_seed)
      s_chassis.gpio_seed(m_gpio_out, m_gpio_dir);
    m_gpio_seeded = true;
  }
  ++m_stats.reads;
  *value = reg == kGpioOut ? m_gpio_out : reg == kGpioDir ? m_gpio_dir : kGpioSlotIn;
  return true;
}

bool NativePi::HollywoodWrite(u32 ea, u8 size, u64 value)
{
  if (m_disabled || size != 4 || (ea & kHollywoodPageMask) != kHollywoodPage)
    return false;
  const u32 reg = ea & 0xFFF;
  const u32 word32 = static_cast<u32>(value);
  if (reg == kIpcPpcCtrl)
  {
    // PPC control of the IPC: X1 sends a request to the IOS, which only the
    // chassis's kernel could take (native IPC never reaches this register
    // with X1). Y1/Y2 are acknowledged by writing 1; IY1/IY2 and X2 are state.
    if (word32 & 1u)
      return false;
    u32 ctrl = m_ipc_ctrl;
    ctrl = (ctrl & ~0x08u) | (word32 & 0x08u);  // X2
    if (word32 & 0x04u)
      ctrl &= ~0x04u;  // Y1 acknowledged
    if (word32 & 0x02u)
      ctrl &= ~0x02u;  // Y2 acknowledged
    ctrl = (ctrl & ~0x30u) | (word32 & 0x30u);  // IY1, IY2
    ctrl &= ~0x01u;
    m_ipc_ctrl = ctrl;
    ++m_stats.writes;
    return true;
  }
  if (reg == kIpcPpcIrqMask)
  {
    m_ipc_irq_mask = word32;
    ++m_stats.writes;
    return true;
  }
  if (reg != kGpioOut && reg != kGpioDir)
    return false;
  if (!m_gpio_seeded)
  {
    if (s_chassis.gpio_seed)
      s_chassis.gpio_seed(m_gpio_out, m_gpio_dir);
    m_gpio_seeded = true;
  }
  ++m_stats.writes;
  const u32 word = static_cast<u32>(value);
  u32& target = reg == kGpioOut ? m_gpio_out : m_gpio_dir;
  target = (word & kGpioOwner) | (target & ~kGpioOwner);
  return true;
}

bool NativePi::MiRead(u32 ea, u8 size, u64* value)
{
  if (m_disabled || size != 2 || (ea & 0xFFFFFFC0u) != kMiBase)
    return false;
  ++m_stats.reads;
  *value = m_mi_regs[(ea & 0x3F) / 2];
  return true;
}

bool NativePi::MiWrite(u32 ea, u8 size, u64 value)
{
  if (m_disabled || size != 2 || (ea & 0xFFFFFFC0u) != kMiBase)
    return false;
  ++m_stats.writes;
  m_mi_regs[(ea & 0x3F) / 2] = static_cast<u16>(value);
  return true;
}
}  // namespace StaticRecompNativePi
