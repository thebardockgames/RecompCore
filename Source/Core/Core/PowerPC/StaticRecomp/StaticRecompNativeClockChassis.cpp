// RecompCore: StaticRecomp CPU core - chassis adapter of the native clock (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClockChassis.h"

#include "Common/Config/Config.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompKernelChassis.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/PerformanceMetrics.h"

namespace StaticRecompNativeClockChassis
{
using StaticRecompNativeClock::NativeClock;

namespace
{
CoreTiming::EventType* s_bridge = nullptr;

void BridgeEvent(Core::System&, u64, s64)
{
  if (auto* clock = NativeClock::Instance())
    clock->AdvanceBridge();
}

CoreTiming::EventType* Bridge(Core::System& system)
{
  if (!s_bridge)
    s_bridge = system.GetCoreTiming().RegisterEvent("NativeClock", &BridgeEvent);
  return s_bridge;
}
}  // namespace

void Install(Core::System& system)
{
  static_assert(SystemTimers::TIMER_RATIO == 12);
  static_assert(SPR_DEC == 22);
  static_assert(EXCEPTION_DECREMENTER == 0x1 && EXCEPTION_EXTERNAL_INT == 0x4 &&
                EXCEPTION_PERFORMANCE_MONITOR == 0x100);
  s_bridge = nullptr;
  NativeClock::ChassisHooks hooks;
  hooks.install = [&system](NativeClock& clock) {
    auto& timing = system.GetCoreTiming();
    if (clock.OwnsDecrementer())
    {
      system.GetSystemTimers().SetNativeDecrementer([&clock] { clock.DecrementerSet(); },
                                                    [&clock] { return clock.Decrementer(); });
    }
    if (clock.OwnsThrottle())
      timing.SetNativeTargetHostTime([&clock](s64 cycle) { return clock.TargetHostTime(cycle); });
    if (!clock.OwnsCounter())
      return false;
    clock.ImportChassis(timing.DetachForNativeClock());
    timing.SetNativeClock(&clock);
    return true;
  };
  hooks.uninstall = [&system](NativeClock& clock, bool facade_installed) {
    auto& timing = system.GetCoreTiming();
    system.GetSystemTimers().SetNativeDecrementer(nullptr, nullptr);
    timing.SetNativeTargetHostTime(nullptr);
    if (facade_installed && timing.GetNativeClock() == &clock)
    {
      timing.SetNativeClock(nullptr);
      timing.AttachFromNativeClock(clock.ExportChassis());
    }
    if (s_bridge)
      timing.RemoveAllEvents(s_bridge);
  };
  hooks.chassis_event_name = [](CoreTiming::EventType* type) {
    return type->name ? *type->name : std::string("chassis");
  };
  hooks.run_chassis_event = [&system](CoreTiming::EventType* type, u64 userdata, s64 late) {
    type->callback(system, userdata, late);
  };
  hooks.chassis_ticks = [&system] { return system.GetCoreTiming().GetTicks(); };
  hooks.ticks_per_second = [&system] { return system.GetSystemTimers().GetTicksPerSecond(); };
  hooks.time_base = [&system] { return system.GetSystemTimers().GetFakeTimeBase(); };
  hooks.bridge_wake = [&system] {
    system.GetCoreTiming().ScheduleEvent(0, Bridge(system), 0, CoreTiming::FromThread::NON_CPU);
  };
  hooks.bridge_arm = [&system](bool armed, s64 delay) {
    auto& timing = system.GetCoreTiming();
    CoreTiming::EventType* bridge = Bridge(system);
    timing.RemoveAllEvents(bridge);
    if (armed)
      timing.ScheduleEvent(delay, bridge, 0, CoreTiming::FromThread::CPU);
  };
  hooks.check_config = [] { CPUThreadConfigCallback::CheckForConfigChanges(); };
  hooks.emulation_speed = [] { return static_cast<double>(Config::Get(Config::MAIN_EMULATION_SPEED)); };
  hooks.max_fallback_ms = [] { return static_cast<double>(Config::Get(Config::MAIN_MAX_FALLBACK)); };
  hooks.correct_drift = [] { return Config::Get(Config::MAIN_CORRECT_TIME_DRIFT); };
  hooks.throttle_sleep = [&system](DT slept) { system.GetPerfMetrics().CountThrottleSleep(slept); };
  hooks.performance_marker = [&system](s64 cycle, u32 ticks_per_second) {
    system.GetPerfMetrics().CountPerformanceMarker(cycle, ticks_per_second);
  };
  hooks.idle_sync = [&system] {
    if (system.GetCoreTiming().UseSyncOnSkipIdle())
      system.GetFifo().FlushGpu();
  };
  hooks.performance_monitor = [&system](int downcount) {
    PowerPC::UpdatePerformanceMonitor(downcount, 0, 0, system.GetPPCState());
  };
  hooks.chassis_regs = [&system] { return StaticRecompKernelChassis::ChassisRegs(system); };
  hooks.chassis_msr = [&system] { return system.GetPPCState().msr.Hex; };
  hooks.deliver_on_chassis = [&system](u32 vector) {
    auto& power_pc = system.GetPowerPC();
    auto& ppc = power_pc.GetPPCState();
    SRR0(ppc) = ppc.npc;
    SRR1(ppc) = ppc.msr.Hex & 0x87C0FFFF;
    ppc.msr.LE = ppc.msr.ILE;
    ppc.msr.Hex &= ~0x04EF36;
    ppc.pc = ppc.npc = vector;
    power_pc.MSRUpdated();
  };
  hooks.chassis_check_external = [&system] { system.GetPowerPC().CheckExternalExceptions(); };
  NativeClock::SetChassisHooks(std::move(hooks));
}
}  // namespace StaticRecompNativeClockChassis
