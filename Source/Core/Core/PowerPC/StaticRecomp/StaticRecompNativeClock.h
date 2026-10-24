// RecompCore: StaticRecomp CPU core - native clock (phase F, phase S step 2).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The clock of the native runtime: the cycle counter the recompiled code
// moves through its downcount, the slice loop, the event queue of every timed
// thing (video interface, audio DMA, GP syncs, the decrementer, native
// callbacks) and the real-time limiter. The chassis's CoreTiming is a facade
// over this object while the chassis exists: its ScheduleEvent/RemoveEvent,
// GetTicks, Advance, Idle and ForceExceptionCheck forward here, so the
// chassis's remaining devices keep their callbacks and nothing of the
// chassis's own queue runs. External interrupts are delivered here too.
//
// MODERNGEKKO_NATIVE_CLOCK_COUNTER=off keeps the previous arrangement (the
// chassis counts cycles and runs one bridge event for our queue), for
// bisection only. MODERNGEKKO_NATIVE_CLOCK_EXCEPTIONS=off leaves the delivery
// of external interrupts to the chassis.
//
// Everything runs on the CPU thread except Schedule(..., false), which the GP
// thread uses: those events are queued aside and moved on the CPU thread.

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRegs.h"

namespace StaticRecompState
{
class StateStream;
}
struct CPUState;
namespace CoreTiming
{
struct EventType;
}

namespace StaticRecompNativeClock
{
struct EventType
{
  std::string name;
  std::function<void(u64 userdata, s64 cycles_late)> callback;
  CoreTiming::EventType* chassis = nullptr;  // set for the chassis's own events (facade)
  u8 retire_when = 0;  // chassis event of a replaced subsystem: 1 native audio, 2 native IPC
  u64 fired = 0;  // diagnostic: printed by PrintStats
  u64 retired = 0;
};

// A chassis event as the chassis's savestate and queue describe it.
struct ChassisEvent
{
  s64 time;
  u64 order;
  u64 userdata;
  CoreTiming::EventType* type;
};
struct ChassisSnapshot
{
  s64 ticks = 0;
  int slice_length = 0;
  s64 idled = 0;
  std::vector<ChassisEvent> events;
};

class NativeClock
{
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  static constexpr int kMaxSliceLength = 20000;  // the chassis's MAX_SLICE_LENGTH

  // What the clock needs from the chassis while it exists (installed by
  // StaticRecompNativeClockChassis): the facade of its CoreTiming and its
  // decrementer/limiter forwarding, its events (name and callback), its
  // counter in bridge mode, its time base and clock rate, its configuration,
  // performance metrics and PowerPC state. Without an adapter the clock runs
  // alone: 729 MHz, time base = cycles / 12, speed 1.0, its own registers.
  struct ChassisHooks
  {
    // Facade: on construction (returns whether the chassis's CoreTiming now
    // forwards to this clock) and on destruction.
    std::function<bool(NativeClock& clock)> install;
    std::function<void(NativeClock& clock, bool facade_installed)> uninstall;
    std::function<std::string(CoreTiming::EventType* type)> chassis_event_name;
    std::function<void(CoreTiming::EventType* type, u64 userdata, s64 late)> run_chassis_event;
    std::function<u64()> chassis_ticks;  // bridge mode and before the clock exists
    std::function<u32()> ticks_per_second;
    std::function<u64()> time_base;
    // Bridge mode: wake the CPU thread now / arm the single bridge event.
    std::function<void()> bridge_wake;
    std::function<void(bool armed, s64 delay)> bridge_arm;
    std::function<void()> check_config;
    std::function<double()> emulation_speed;
    std::function<double()> max_fallback_ms;
    std::function<bool()> correct_drift;
    std::function<void(DT slept)> throttle_sleep;
    std::function<void(s64 cycle, u32 ticks_per_second)> performance_marker;
    std::function<void()> idle_sync;
    std::function<void(int downcount)> performance_monitor;
    // External interrupts on the chassis's PowerPC state (no module state set,
    // or MODERNGEKKO_NATIVE_CLOCK_EXCEPTIONS=off).
    std::function<StaticRecompGuestRegs::RegsView()> chassis_regs;
    std::function<u32()> chassis_msr;
    std::function<void(u32 vector)> deliver_on_chassis;
    std::function<void()> chassis_check_external;
  };
  static void SetChassisHooks(ChassisHooks hooks);

  NativeClock();
  ~NativeClock();

  static NativeClock* Instance() { return s_instance; }

  // Event types live as long as the clock; the pointer is the handle.
  EventType* Register(std::string name, std::function<void(u64, s64)> callback);
  // Cycles (CPU clock) from now. A late deadline (cycles <= 0) fires at the
  // next slice. From another thread (from_cpu_thread = false) the deadline is
  // resolved when the CPU thread picks it up.
  void Schedule(s64 cycles_into_future, EventType* type, u64 userdata = 0,
                bool from_cpu_thread = true);
  void Remove(EventType* type);
  bool Pending(const EventType* type) const;

  // ---- counter mode (the facade of the chassis's CoreTiming calls these) ----
  bool OwnsCounter() const { return m_owns_counter; }
  bool OwnsExceptions() const { return m_owns_exceptions; }
  // The slice loop: account the cycles the CPU consumed, run the due events,
  // set the next slice in the CPU's downcount and deliver pending external
  // interrupts. Idle(): the CPU sits in its idle loop, skip the slice.
  void Slice();
  void Idle();
  void ForceExceptionCheck(s64 cycles);
  void MoveCrossThread();
  // External interrupts are delivered on this state when set (the module's
  // CPUState during native execution), else on the chassis's PowerPC state.
  void SetCpuState(CPUState* cpu, StaticRecompGuestRegs::GuestRegs* regs)
  {
    m_cpu = cpu;
    m_regs = regs;
  }
  // The DEC register, the slice counter and the pending lines in the
  // authoritative storage (the port's while the guest runs, the chassis's
  // otherwise).
  u32& Dec();
  int& Downcount();
  int Downcount() const { return const_cast<NativeClock*>(this)->Downcount(); }
  u32& Exceptions();
  s64 IdledCycles() const { return m_idled_cycles; }
  // Chassis events: schedule/remove by the chassis's type.
  void ScheduleChassis(s64 cycles_into_future, CoreTiming::EventType* type, u64 userdata,
                       bool from_cpu_thread);
  void RemoveChassis(CoreTiming::EventType* type);
  // Counter and chassis events, as the chassis's savestate holds them: import
  // on install and on loading a chassis state, export when writing one.
  void ImportChassis(const ChassisSnapshot& snapshot);
  ChassisSnapshot ExportChassis() const;
  // Loading a native checkpoint: the counter continues from `ticks`, the
  // slice restarts and every event is dropped (the modules rebuild theirs).
  void LoadCounter(s64 ticks);
  void ClearAll();

  u64 Now() const;  // CPU cycles
  u32 TicksPerSecond() const;
  u64 TimeBase() const;  // the PowerPC time base
  // The same through the chassis when no clock exists yet (boot): what the
  // native modules call, so none of them needs CoreTiming or SystemTimers.
  static u64 CurrentTicks();
  static u32 CurrentTicksPerSecond();
  static u64 CurrentTimeBase();
  // The time base's origin (value at a cycle) when no chassis provides it: the
  // time base is value + (cycles - ticks) / 12.
  void SetTimeBaseStart(u64 value, u64 ticks)
  {
    m_tb_start_value = value;
    m_tb_start_ticks = ticks;
  }
  void GetTimeBaseStart(u64* value, u64* ticks) const
  {
    *value = m_tb_start_value;
    *ticks = m_tb_start_ticks;
  }

  // Decrementer: the chassis's SystemTimers forwards mtdec/mfdec here.
  void DecrementerSet();
  u32 Decrementer() const;
  // After a state load restored DEC and the saved start of the count: schedule
  // the remaining part (nothing if DEC is negative, i.e. no alarm armed).
  void RearmDecrementer();

  // Real-time limiter: called at the end of each video field with the field's
  // cycle; GetTargetHostTime of the chassis forwards to TargetHostTime.
  void Throttle(s64 target_cycle);
  TimePoint TargetHostTime(s64 target_cycle);
  bool OwnsThrottle() const { return m_owns_throttle; }
  bool OwnsDecrementer() const { return m_owns_dec; }

  void DoState(StaticRecompState::StateStream& p, u32 version);
  // Loading a state without a clock section: the queue is rebuilt by the
  // modules and the decrementer restarts from the restored register.
  void ResetForLoad();
  void PrintStats() const;

  struct Stats
  {
    u64 scheduled = 0, fired = 0, cross_thread = 0, slices = 0, bridge_runs = 0, dec_sets = 0,
        dec_fired = 0, throttles = 0, throttle_resets = 0, sleeps = 0, chassis_events = 0,
        external_interrupts = 0, idles = 0, retired = 0;
  };
  const Stats& GetStats() const { return m_stats; }
  // Bridge mode: the chassis's bridge event runs our due events.
  void AdvanceBridge();

private:
  struct Event
  {
    s64 time;  // absolute cycles; for cross-thread entries, cycles from pick-up
    u64 order;
    u64 userdata;
    EventType* type;
    bool operator>(const Event& o) const
    {
      return time != o.time ? time > o.time : order > o.order;
    }
  };

  void Push(Event e);
  void ClearNativeEvents();
  EventType* ChassisType(CoreTiming::EventType* type, bool create);
  void RunDueEvents(s64 now);
  void DeliverExternalExceptions();
  // Bridge mode (counter owned by the chassis).
  void Arm();
  bool IsChassisPatchEngine(CoreTiming::EventType* type) const;

  void SleepUntil(TimePoint target);
  double ConfiguredSpeed() const;
  void UpdateSpeedLimit(s64 cycle, double speed);
  void ResetThrottle(s64 cycle);

  std::vector<std::unique_ptr<EventType>> m_types;
  std::unordered_map<CoreTiming::EventType*, EventType*> m_chassis_types;
  std::vector<Event> m_queue;  // min-heap by (time, order)
  u64 m_next_order = 0;
  std::mutex m_cross_mutex;
  std::vector<Event> m_cross;
  bool m_in_advance = false;

  // Counter mode.
  bool m_owns_counter = true;
  bool m_owns_exceptions = true;
  CPUState* m_cpu = nullptr;
  StaticRecompGuestRegs::GuestRegs* m_regs = nullptr;  // the port's registers while the guest runs
  bool m_facade_installed = false;
  s64 m_now = 0;
  int m_slice_length = 0;
  s64 m_idled_cycles = 0;
  // The chassis's PowerPC registers (or ours without a chassis) while the
  // guest does not hold them.
  StaticRecompGuestRegs::GuestRegs m_own_regs{};
  StaticRecompGuestRegs::RegsView m_idle_view{};

  bool m_owns_dec = true;
  bool m_owns_throttle = true;
  EventType* m_dec_event = nullptr;
  u32 m_dec_start_value = 0xFFFFFFFF;
  u64 m_dec_start_ticks = 0;

  s64 m_ref_cycle = 0;
  TimePoint m_ref_time{};
  u32 m_adj_clock_per_sec = 0;  // 0 = unlimited
  double m_speed = 1.0;
  bool m_throttle_started = false;
  std::chrono::nanoseconds m_max_fallback{};
  bool m_correct_drift = false;
  void* m_timer = nullptr;  // Windows high-resolution waitable timer

  u64 m_tb_start_value = 0;
  u64 m_tb_start_ticks = 0;
  Stats m_stats;
  static NativeClock* s_instance;
  static ChassisHooks s_chassis;
};
}  // namespace StaticRecompNativeClock
