// RecompCore: StaticRecomp CPU core - chassis adapter of the native PI (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativePiChassis.h"

#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/WII_IPC.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompHostLoop.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePi.h"
#include "Core/System.h"

namespace StaticRecompNativePiChassis
{
void Install(Core::System& system)
{
  using StaticRecompNativePi::NativePi;
  static_assert(ProcessorInterface::INT_CAUSE_RST_BUTTON == 0x10000);
  static_assert(EXCEPTION_EXTERNAL_INT == 0x00000004);
  NativePi::ChassisHooks hooks;
  hooks.raise = [&system](u32 line, bool set) {
    system.GetProcessorInterface().SetInterrupt(line, set);
  };
  hooks.enable_lines = [&system](u32 lines) {
    system.GetProcessorInterface().m_interrupt_mask |= lines;
  };
  hooks.set_forward = [&system](std::function<bool(u32, bool)> forward) {
    system.GetProcessorInterface().SetNativeInterruptHook(std::move(forward));
  };
  hooks.seed = [&system](u32& cause, u32& mask, u32& reset_code) {
    if (StaticRecompHostLoop::SkipsDevice("pi"))
      return false;
    const auto& pi = system.GetProcessorInterface();
    cause = pi.GetCause();
    mask = pi.GetMask();
    reset_code = pi.m_reset_code;
    return true;
  };
  hooks.copy = [&system](u32 cause, u32 mask, u32 reset_code) {
    auto& pi = system.GetProcessorInterface();
    pi.m_interrupt_cause = cause;
    pi.m_interrupt_mask = mask;
    pi.m_reset_code = reset_code;
  };
  hooks.exceptions = [&system]() -> u32* { return &system.GetPPCState().Exceptions; };
  hooks.gpio_seed = [&system](u32& out, u32& dir) {
    out = system.GetWiiIPC().GetGPIOOutFlags().m_hex;
    dir = system.GetWiiIPC().GetGPIODirFlags().m_hex;
  };
  NativePi::SetChassisHooks(std::move(hooks));
}
}  // namespace StaticRecompNativePiChassis
