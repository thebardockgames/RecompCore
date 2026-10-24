// RecompCore: StaticRecomp CPU core - native clock (phase F, phase S step 2).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
#include "core/cpu.h"

namespace StaticRecompNativeClock
{
NativeClock* NativeClock::s_instance = nullptr;
NativeClock::ChassisHooks NativeClock::s_chassis;

namespace
{
// The 750CL: SPR number of DEC, the pending-exception bits (the chassis's
// Gekko.h values) and the time base / decrementer rate (bus clock / 4 = CPU
// clock / 12, kTimerRatio).
constexpr u32 kSprDec = 22;
constexpr u32 kExceptionDecrementer = 0x00000001;
constexpr u32 kExceptionExternalInt = 0x00000004;
constexpr u32 kExceptionPerformanceMonitor = 0x00000100;
constexpr u64 kTimerRatio = 12;
constexpr u32 kWiiTicksPerSecond = 729000000;

bool EnvOff(const char* name)
{
  const char* value = std::getenv(name);
  return value && (std::string(value) == "off" || std::string(value) == "0");
}
}  // namespace

void NativeClock::SetChassisHooks(ChassisHooks hooks)
{
  s_chassis = std::move(hooks);
}

NativeClock::NativeClock()
{
  s_instance = this;
  m_idle_view = s_chassis.chassis_regs ? s_chassis.chassis_regs() : m_own_regs.View();
  m_dec_event = Register("NativeDecrementer", [this](u64, s64) {
    Dec() = 0xFFFFFFFF;
    Exceptions() |= kExceptionDecrementer;
    ++m_stats.dec_fired;
  });
#ifdef _WIN32
  m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
  if (!m_timer)
    m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
#endif
  // Bisection toggles: MODERNGEKKO_NATIVE_CLOCK_DEC=off leaves the decrementer
  // to the chassis, _THROTTLE=off its limiter, _COUNTER=off its cycle counter
  // and queue (bridge mode), _EXCEPTIONS=off the delivery of external
  // interrupts.
  m_owns_dec = !EnvOff("MODERNGEKKO_NATIVE_CLOCK_DEC");
  m_owns_throttle = !EnvOff("MODERNGEKKO_NATIVE_CLOCK_THROTTLE");
  m_owns_counter = !EnvOff("MODERNGEKKO_NATIVE_CLOCK_COUNTER");
  m_owns_exceptions = m_owns_counter && !EnvOff("MODERNGEKKO_NATIVE_CLOCK_EXCEPTIONS");
  m_max_fallback = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::milli>(
      s_chassis.max_fallback_ms ? s_chassis.max_fallback_ms() : 100.0));
  m_correct_drift = s_chassis.correct_drift ? s_chassis.correct_drift() : false;
  // The chassis adopts the decrementer/limiter forwarding and, with the
  // counter ours, hands over its counter and the events its devices
  // scheduled during the boot (ImportChassis); from then on it forwards
  // everything here.
  if (s_chassis.install)
    m_facade_installed = s_chassis.install(*this);
  if (m_owns_counter)
  {
    std::fprintf(stderr, "[native_clock] counter: native (slice %d, %zu chassis events adopted)%s\n",
                 m_slice_length, m_queue.size(),
                 m_owns_exceptions ? ", external interrupts: native" : "");
  }
  else
  {
    std::fprintf(stderr, "[native_clock] counter: chassis (bridge mode)\n");
  }
}

NativeClock::~NativeClock()
{
  if (s_chassis.uninstall)
    s_chassis.uninstall(*this, m_facade_installed);
#ifdef _WIN32
  if (m_timer)
    CloseHandle(static_cast<HANDLE>(m_timer));
#endif
  if (s_instance == this)
    s_instance = nullptr;
}

EventType* NativeClock::Register(std::string name, std::function<void(u64, s64)> callback)
{
  for (const auto& type : m_types)
  {
    if (type->name == name && !type->chassis)
    {
      type->callback = std::move(callback);
      return type.get();
    }
  }
  m_types.push_back(std::make_unique<EventType>(EventType{std::move(name), std::move(callback)}));
  return m_types.back().get();
}

// The chassis's per-frame patch engine (game .ini patches, Action Replay and
// Gecko codes) is not part of the native runtime: it reads the chassis's
// PowerPC state, which is only a mirror while native code runs, and with the
// mirror stale it retried every 1000 cycles (10 M events in 50 s). Nothing of
// the game needs it; the HLE function patches are applied at boot, not here.
bool NativeClock::IsChassisPatchEngine(CoreTiming::EventType* type) const
{
  return type && s_chassis.chassis_event_name && s_chassis.chassis_event_name(type) == "PatchEngine";
}

EventType* NativeClock::ChassisType(CoreTiming::EventType* type, bool create)
{
  if (!type)
    return nullptr;
  if (const auto it = m_chassis_types.find(type); it != m_chassis_types.end())
    return it->second;
  if (!create)
    return nullptr;
  auto wrapper = std::make_unique<EventType>();
  wrapper->name = s_chassis.chassis_event_name ? s_chassis.chassis_event_name(type) : std::string("chassis");
  wrapper->chassis = type;
  // The chassis's audio DMA and DSP callbacks return at once with native
  // audio, and its IOS device update has nothing opened with native IPC: once
  // the replacement is bound, the event is not run (nor rescheduled) again.
  if (wrapper->name == "AudioDMACallback" || wrapper->name == "DSPCallback")
    wrapper->retire_when = 1;
  else if (wrapper->name == "IPC_HLE_UpdateCallback")
    wrapper->retire_when = 2;
  EventType* raw = wrapper.get();
  wrapper->callback = [this, type, raw](u64 userdata, s64 late) {
    if ((raw->retire_when == 1 && StaticRecompNativeSdk::NativeSdk::AudioNative()) ||
        (raw->retire_when == 2 && StaticRecompNativeSdk::NativeSdk::IpcNative()))
    {
      ++raw->retired;
      ++m_stats.retired;
      return;
    }
    ++m_stats.chassis_events;
    if (s_chassis.run_chassis_event)
      s_chassis.run_chassis_event(type, userdata, late);
  };
  m_types.push_back(std::move(wrapper));
  m_chassis_types.emplace(type, m_types.back().get());
  return m_types.back().get();
}

// ---- time ----

u64 NativeClock::Now() const
{
  if (!m_owns_counter)
    return s_chassis.chassis_ticks ? s_chassis.chassis_ticks() : static_cast<u64>(m_now);
  s64 ticks = m_now;
  if (!m_in_advance)
    ticks += m_slice_length - Downcount();
  return static_cast<u64>(ticks);
}

u32 NativeClock::TicksPerSecond() const
{
  return CurrentTicksPerSecond();
}

u64 NativeClock::CurrentTicks()
{
  if (s_instance)
    return s_instance->Now();
  return s_chassis.chassis_ticks ? s_chassis.chassis_ticks() : 0;
}

u32 NativeClock::CurrentTicksPerSecond()
{
  return s_chassis.ticks_per_second ? s_chassis.ticks_per_second() : kWiiTicksPerSecond;
}

u64 NativeClock::CurrentTimeBase()
{
  if (s_chassis.time_base)
    return s_chassis.time_base();
  if (!s_instance)
    return 0;
  return s_instance->m_tb_start_value + (s_instance->Now() - s_instance->m_tb_start_ticks) / kTimerRatio;
}

u64 NativeClock::TimeBase() const
{
  return CurrentTimeBase();
}

// ---- queue ----

void NativeClock::Push(Event e)
{
  m_queue.push_back(e);
  std::push_heap(m_queue.begin(), m_queue.end(), std::greater<Event>{});
}

void NativeClock::Schedule(s64 cycles_into_future, EventType* type, u64 userdata,
                           bool from_cpu_thread)
{
  if (!type)
    return;
  if (!from_cpu_thread)
  {
    {
      std::lock_guard lock(m_cross_mutex);
      m_cross.push_back(Event{cycles_into_future, 0, userdata, type});
    }
    ++m_stats.cross_thread;
    if (!m_owns_counter)
    {
      // Wake the bridge on the CPU thread; the deadline is resolved there.
      if (s_chassis.bridge_wake)
        s_chassis.bridge_wake();
    }
    return;
  }
  ++m_stats.scheduled;
  Push(Event{static_cast<s64>(Now()) + cycles_into_future, m_next_order++, userdata, type});
  if (m_in_advance)
    return;
  if (m_owns_counter)
    ForceExceptionCheck(cycles_into_future);  // the slice ends in time for it
  else
    Arm();
}

void NativeClock::Remove(EventType* type)
{
  const auto it = std::remove_if(m_queue.begin(), m_queue.end(),
                                 [type](const Event& e) { return e.type == type; });
  if (it != m_queue.end())
  {
    m_queue.erase(it, m_queue.end());
    std::make_heap(m_queue.begin(), m_queue.end(), std::greater<Event>{});
    if (!m_in_advance && !m_owns_counter)
      Arm();
  }
}

bool NativeClock::Pending(const EventType* type) const
{
  return std::any_of(m_queue.begin(), m_queue.end(),
                     [type](const Event& e) { return e.type == type; });
}

void NativeClock::ScheduleChassis(s64 cycles_into_future, CoreTiming::EventType* type,
                                  u64 userdata, bool from_cpu_thread)
{
  if (IsChassisPatchEngine(type))
    return;
  Schedule(cycles_into_future, ChassisType(type, true), userdata, from_cpu_thread);
}

void NativeClock::RemoveChassis(CoreTiming::EventType* type)
{
  if (EventType* wrapper = ChassisType(type, false))
    Remove(wrapper);
}

void NativeClock::MoveCrossThread()
{
  std::vector<Event> moved;
  {
    std::lock_guard lock(m_cross_mutex);
    if (m_cross.empty())
      return;
    moved.swap(m_cross);
  }
  // As the chassis did: relative to the counter at the slice boundary.
  const s64 now = m_owns_counter ? m_now : static_cast<s64>(Now());
  for (Event& e : moved)
  {
    e.time += now;
    e.order = m_next_order++;
    Push(e);
    ++m_stats.scheduled;
  }
}

void NativeClock::RunDueEvents(s64 now)
{
  while (!m_queue.empty() && m_queue.front().time <= now)
  {
    std::pop_heap(m_queue.begin(), m_queue.end(), std::greater<Event>{});
    const Event e = m_queue.back();
    m_queue.pop_back();
    ++m_stats.fired;
    ++e.type->fired;
    e.type->callback(e.userdata, now - e.time);
  }
}

void NativeClock::ClearNativeEvents()
{
  const auto it = std::remove_if(m_queue.begin(), m_queue.end(),
                                 [](const Event& e) { return e.type->chassis == nullptr; });
  m_queue.erase(it, m_queue.end());
  std::make_heap(m_queue.begin(), m_queue.end(), std::greater<Event>{});
  std::lock_guard lock(m_cross_mutex);
  const auto cit = std::remove_if(m_cross.begin(), m_cross.end(),
                                  [](const Event& e) { return e.type->chassis == nullptr; });
  m_cross.erase(cit, m_cross.end());
}

void NativeClock::ClearAll()
{
  m_queue.clear();
  std::lock_guard lock(m_cross_mutex);
  m_cross.clear();
}

// ---- counter mode: the slice loop ----

void NativeClock::Slice()
{
  ++m_stats.slices;
  if (s_chassis.check_config)
    s_chassis.check_config();
  int& downcount = Downcount();

  MoveCrossThread();  // before the counter moves, as the chassis did
  const int executed = m_slice_length - downcount;
  m_now += executed;
  m_slice_length = kMaxSliceLength;

  m_in_advance = true;
  RunDueEvents(m_now);
  m_in_advance = false;

  if (!m_queue.empty())
    m_slice_length = static_cast<int>(std::min<s64>(m_queue.front().time - m_now, kMaxSliceLength));
  downcount = m_slice_length;

  // After the events, so an interrupt they raised is not a slice late.
  if (Exceptions() != 0)
    DeliverExternalExceptions();
}

void NativeClock::DeliverExternalExceptions()
{
  if (!m_owns_exceptions)
  {
    if (s_chassis.chassis_check_external)
      s_chassis.chassis_check_external();
    return;
  }
  u32& pending = Exceptions();  // the pending lines (NativePi, decrementer)
  const u32 exceptions = pending;
  const u32 msr = m_cpu ? m_cpu->msr : s_chassis.chassis_msr ? s_chassis.chassis_msr() : 0;
  if ((msr & 0x8000u) == 0)
    return;  // held until the game enables external interrupts (MSR.EE)
  u32 vector = 0;
  if (exceptions & kExceptionExternalInt)
  {
    vector = 0x00000500;
    pending &= ~kExceptionExternalInt;
  }
  else if (exceptions & kExceptionPerformanceMonitor)
  {
    vector = 0x00000F00;
    pending &= ~kExceptionPerformanceMonitor;
  }
  else if (exceptions & kExceptionDecrementer)
  {
    vector = 0x00000900;
    pending &= ~kExceptionDecrementer;
  }
  else
  {
    return;  // a synchronous exception: the dispatcher delivers it
  }
  ++m_stats.external_interrupts;
  // The 750CL takes an external interrupt between instructions: SRR0 is the
  // next instruction, SRR1 keeps the MSR bits the handler restores, the MSR
  // drops EE/PR/FP/FE0/SE/BE/FE1/IR/DR/RI, LE takes ILE and the handler runs
  // at the vector.
  if (m_cpu)
  {
    // On the module's state (a native burst just ended at m_cpu->pc). The
    // chassis's MSR mirror follows lazily (PropagateGuestMSR before any MMU use).
    m_cpu->srr0 = m_cpu->pc;
    m_cpu->srr1 = msr & 0x87C0FFFFu;
    m_cpu->msr = ((msr & ~1u) | ((msr >> 16) & 1u)) & ~0x04EF36u;
    m_cpu->pc = vector;
    return;
  }
  if (s_chassis.deliver_on_chassis)
    s_chassis.deliver_on_chassis(vector);
}

void NativeClock::Idle()
{
  ++m_stats.idles;
  // While the GP is processing, do not jump ahead: the video interface would
  // desynchronise from it (the chassis's sync-on-skip-idle GPU flush).
  if (s_chassis.idle_sync)
    s_chassis.idle_sync();
  int& downcount = Downcount();
  if (s_chassis.performance_monitor)
    s_chassis.performance_monitor(downcount);
  m_idled_cycles += downcount;
  downcount = 0;
}

void NativeClock::ForceExceptionCheck(s64 cycles)
{
  cycles = std::max<s64>(0, cycles);
  int& downcount = Downcount();
  if (downcount > cycles)
  {
    // Account the cycles already executed in this slice, then end it earlier.
    m_slice_length -= downcount - static_cast<int>(cycles);
    downcount = static_cast<int>(cycles);
  }
}

void NativeClock::ImportChassis(const ChassisSnapshot& snapshot)
{
  m_now = snapshot.ticks;
  m_slice_length = snapshot.slice_length;
  m_idled_cycles = snapshot.idled;
  // Drop the chassis events we already hold (a reload), keep the native ones.
  const auto it = std::remove_if(m_queue.begin(), m_queue.end(),
                                 [](const Event& e) { return e.type->chassis != nullptr; });
  m_queue.erase(it, m_queue.end());
  for (const ChassisEvent& e : snapshot.events)
  {
    if (IsChassisPatchEngine(e.type))
      continue;
    EventType* type = ChassisType(e.type, true);
    m_queue.push_back(Event{e.time, m_next_order++, e.userdata, type});
  }
  std::make_heap(m_queue.begin(), m_queue.end(), std::greater<Event>{});
  m_throttle_started = false;
}

ChassisSnapshot NativeClock::ExportChassis() const
{
  ChassisSnapshot snapshot;
  snapshot.ticks = m_now;
  snapshot.slice_length = m_slice_length;
  snapshot.idled = m_idled_cycles;
  for (const Event& e : m_queue)
  {
    if (e.type->chassis)
      snapshot.events.push_back(ChassisEvent{e.time, e.order, e.userdata, e.type->chassis});
  }
  std::sort(snapshot.events.begin(), snapshot.events.end(),
            [](const ChassisEvent& a, const ChassisEvent& b) {
              return a.time != b.time ? a.time < b.time : a.order < b.order;
            });
  return snapshot;
}

void NativeClock::LoadCounter(s64 ticks)
{
  ClearAll();
  m_now = ticks;
  m_slice_length = 0;
  Downcount() = 0;
  m_throttle_started = false;
}

// ---- bridge mode (the chassis owns the counter) ----

void NativeClock::Arm()
{
  if (!s_chassis.bridge_arm)
    return;
  if (m_queue.empty())
    s_chassis.bridge_arm(false, 0);
  else
    s_chassis.bridge_arm(true, m_queue.front().time - static_cast<s64>(Now()));
}

void NativeClock::AdvanceBridge()
{
  ++m_stats.bridge_runs;
  m_in_advance = true;
  MoveCrossThread();
  RunDueEvents(static_cast<s64>(Now()));
  m_in_advance = false;
  Arm();
}

// ---- decrementer ----

u32& NativeClock::Dec()
{
  return m_regs ? m_regs->spr[kSprDec] : m_idle_view.spr[kSprDec];
}

int& NativeClock::Downcount()
{
  return m_regs ? m_regs->downcount : *m_idle_view.downcount;
}

u32& NativeClock::Exceptions()
{
  return m_regs ? m_regs->exceptions : *m_idle_view.exceptions;
}

void NativeClock::DecrementerSet()
{
  const u32 value = Dec();
  ++m_stats.dec_sets;
  Remove(m_dec_event);
  if ((value & 0x80000000u) == 0)
  {
    m_dec_start_ticks = Now();
    m_dec_start_value = value;
    Schedule(static_cast<s64>(value) * kTimerRatio, m_dec_event);
  }
}

u32 NativeClock::Decrementer() const
{
  return m_dec_start_value -
         static_cast<u32>((Now() - m_dec_start_ticks) / kTimerRatio);
}

// ---- real-time limiter ----

double NativeClock::ConfiguredSpeed() const
{
  // MODERNGEKKO_SPEED=<factor> overrides the configured limit (0 = unlimited:
  // the game runs as fast as the CPU allows, a throughput measurement).
  static const double override_speed = [] {
    const char* value = std::getenv("MODERNGEKKO_SPEED");
    return value ? std::atof(value) : -1.0;
  }();
  if (override_speed >= 0.0)
    return override_speed;
  return s_chassis.emulation_speed ? s_chassis.emulation_speed() : 1.0;
}

void NativeClock::UpdateSpeedLimit(s64 cycle, double speed)
{
  m_speed = speed;
  const u32 new_clock_per_sec = static_cast<u32>(std::lround(TicksPerSecond() * speed));
  if (m_adj_clock_per_sec != 0)
  {
    const s64 ticks = cycle - m_ref_cycle;
    const s64 new_ticks = ticks * new_clock_per_sec / m_adj_clock_per_sec;
    m_ref_cycle = cycle - new_ticks;
  }
  m_adj_clock_per_sec = new_clock_per_sec;
}

void NativeClock::ResetThrottle(s64 cycle)
{
  m_ref_cycle = cycle;
  m_ref_time = Clock::now();
  ++m_stats.throttle_resets;
}

NativeClock::TimePoint NativeClock::TargetHostTime(s64 target_cycle)
{
  if (!m_throttle_started || m_adj_clock_per_sec == 0)
    return Clock::now();
  const s64 elapsed = target_cycle - m_ref_cycle;
  return m_ref_time + Clock::duration{std::chrono::seconds{elapsed}} / m_adj_clock_per_sec;
}

void NativeClock::SleepUntil(TimePoint target)
{
  const TimePoint start = Clock::now();
  if (target <= start)
    return;
  ++m_stats.sleeps;
#ifdef _WIN32
  // Timer for all but the last half millisecond, then a short spin: the
  // presenter paces on this.
  const auto coarse = target - std::chrono::microseconds(500);
  if (m_timer && coarse > start)
  {
    LARGE_INTEGER due;
    due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(coarse - start).count() / 100;
    if (SetWaitableTimer(static_cast<HANDLE>(m_timer), &due, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(static_cast<HANDLE>(m_timer), INFINITE);
    else
      std::this_thread::sleep_until(coarse);
  }
  while (Clock::now() < target)
    YieldProcessor();
#else
  std::this_thread::sleep_until(target);
#endif
  if (s_chassis.throttle_sleep)
    s_chassis.throttle_sleep(Clock::now() - start);
}

void NativeClock::Throttle(s64 target_cycle)
{
  ++m_stats.throttles;
  const TimePoint time = Clock::now();
  const double speed = ConfiguredSpeed();
  if (!m_throttle_started)
  {
    m_throttle_started = true;
    m_adj_clock_per_sec = 0;
    UpdateSpeedLimit(target_cycle, speed);
    ResetThrottle(target_cycle);
  }
  else if (speed != m_speed)
  {
    UpdateSpeedLimit(target_cycle, speed);
  }
  // The same performance marker Dolphin counted: the runner's speed figure.
  if (s_chassis.performance_marker)
    s_chassis.performance_marker(target_cycle, TicksPerSecond());
  if (m_adj_clock_per_sec == 0)
  {
    ResetThrottle(target_cycle);
    return;
  }
  // Push the reference forward by whole seconds so rounding does not drift.
  {
    const s64 sec_adj = (target_cycle - m_ref_cycle) / m_adj_clock_per_sec;
    m_ref_cycle += sec_adj * m_adj_clock_per_sec;
    m_ref_time += std::chrono::seconds{sec_adj};
  }
  TimePoint target_time = TargetHostTime(target_cycle);
  const TimePoint min_target = time - m_max_fallback;
  if (!m_correct_drift && target_time < min_target)
  {
    // The game fell behind real time by more than the allowed fallback: let
    // it go rather than racing to catch up.
    const auto adjustment = min_target - target_time;
    m_ref_time += adjustment;
    target_time += adjustment;
  }
  SleepUntil(target_time);
}

// ---- savestates ----

void NativeClock::DoState(StaticRecompState::StateStream& p, u32 version)
{
  (void)version;
  p.Do(m_dec_start_value);
  p.Do(m_dec_start_ticks);
  if (p.IsReadMode())
  {
    // Each native module rebuilds its own events from its state; the chassis
    // events of a chassis savestate were imported by the facade before this
    // section. The decrementer comes from the register the PowerPC state
    // restored.
    ClearNativeEvents();
    // A chassis state restored the registers before this section; a native
    // checkpoint restores them after it and calls RearmDecrementer() then.
    RearmDecrementer();
    if (!m_owns_counter)
      Arm();
    m_throttle_started = false;
  }
}

void NativeClock::RearmDecrementer()
{
  if (!m_owns_dec)
    return;
  Remove(m_dec_event);
  if ((Dec() & 0x80000000u) != 0)
    return;
  const u64 remaining_tb =
      m_dec_start_value - static_cast<u32>((Now() - m_dec_start_ticks) / kTimerRatio);
  Schedule(static_cast<s64>(remaining_tb) * kTimerRatio, m_dec_event);
}

void NativeClock::ResetForLoad()
{
  ClearNativeEvents();
  m_throttle_started = false;
  if (m_owns_dec)
    DecrementerSet();
}

void NativeClock::PrintStats() const
{
  std::fprintf(stderr,
               "[native_clock] counter=%s slices=%llu idles=%llu scheduled=%llu fired=%llu "
               "chassis_events=%llu cross_thread=%llu bridge_runs=%llu external_interrupts=%llu "
               "dec_sets=%llu dec_fired=%llu throttles=%llu throttle_resets=%llu sleeps=%llu "
               "retired=%llu\n",
               m_owns_counter ? "native" : "chassis",
               static_cast<unsigned long long>(m_stats.slices),
               static_cast<unsigned long long>(m_stats.idles),
               static_cast<unsigned long long>(m_stats.scheduled),
               static_cast<unsigned long long>(m_stats.fired),
               static_cast<unsigned long long>(m_stats.chassis_events),
               static_cast<unsigned long long>(m_stats.cross_thread),
               static_cast<unsigned long long>(m_stats.bridge_runs),
               static_cast<unsigned long long>(m_stats.external_interrupts),
               static_cast<unsigned long long>(m_stats.dec_sets),
               static_cast<unsigned long long>(m_stats.dec_fired),
               static_cast<unsigned long long>(m_stats.throttles),
               static_cast<unsigned long long>(m_stats.throttle_resets),
               static_cast<unsigned long long>(m_stats.sleeps),
               static_cast<unsigned long long>(m_stats.retired));
  for (const auto& type : m_types)
  {
    if (type->fired)
      std::fprintf(stderr, "[native_clock] event %s%s fired=%llu retired=%llu\n", type->name.c_str(),
                   type->chassis ? " (chassis)" : "", static_cast<unsigned long long>(type->fired),
                   static_cast<unsigned long long>(type->retired));
  }
}
}  // namespace StaticRecompNativeClock
