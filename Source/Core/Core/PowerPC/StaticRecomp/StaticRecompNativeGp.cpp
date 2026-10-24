// RecompCore: StaticRecomp CPU core - native graphics processor front (phase E).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGx.h"

namespace StaticRecompNativeGp
{
namespace
{
constexpr u32 kCpBase = 0xCC000000;
constexpr u32 kPeBase = 0xCC001000;
constexpr u32 kPiFifoBase = 0xCC00300C;
constexpr u32 kPiFifoEnd = 0xCC003010;
constexpr u32 kPiFifoWrite = 0xCC003014;
constexpr u32 kPiFifoReset = 0xCC003018;
constexpr u32 kBurst = 32;
// Dolphin's single-core minimum between a token/finish and its interrupt.
constexpr u32 kMinPeDelayCycles = 500;
constexpr size_t kStagingSize = 1024 * 1024;


constexpr u16 kCtrlGpReadEnable = CpCtrl::kGpReadEnable;
constexpr u16 kCtrlBpEnable = CpCtrl::kBpEnable;
constexpr u16 kCtrlOverflowInt = CpCtrl::kOverflowInt;
constexpr u16 kCtrlUnderflowInt = CpCtrl::kUnderflowInt;
constexpr u16 kCtrlGpLinkEnable = CpCtrl::kGpLinkEnable;
constexpr u16 kCtrlBpInt = CpCtrl::kBpInt;
// PE control bits (UPECtrlReg).
constexpr u16 kPeTokenEnable = 1 << 0;
constexpr u16 kPeFinishEnable = 1 << 1;
constexpr u16 kPeTokenAck = 1 << 2;
constexpr u16 kPeFinishAck = 1 << 3;

constexpr u32 kIntPeToken = StaticRecompNativePi::Line::kPeToken;
constexpr u32 kIntPeFinish = StaticRecompNativePi::Line::kPeFinish;
constexpr u32 kIntCp = StaticRecompNativePi::Line::kCp;

u32 Lo(u32 value)
{
  return value & 0xFFFF;
}
u32 Hi(u32 value)
{
  return value >> 16;
}
u32 WithLo(u32 value, u16 lo)
{
  return (value & 0xFFFF0000u) | (lo & 0xFFE0);
}
u32 WithHi(u32 value, u16 hi)
{
  return (value & 0x0000FFFFu) | (u32(hi & 0x03FF) << 16);
}
}  // namespace

NativeGp* NativeGp::s_instance = nullptr;
NativeGp::ChassisHooks NativeGp::s_chassis;

void NativeGp::SetChassisHooks(ChassisHooks hooks)
{
  s_chassis = std::move(hooks);
}

NativeGp::NativeGp() : m_memory(StaticRecompGuestMemory::GuestMemory::Own())
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_GP");
  m_disabled = setting && (std::string(setting) == "off" || std::string(setting) == "0");
  m_staging.resize(kStagingSize);
  const char* cp_skip = std::getenv("MODERNGEKKO_NATIVE_GP_CP_EVENT_SKIP");
  m_cp_event_skip = !(cp_skip && (std::string(cp_skip) == "off" || std::string(cp_skip) == "0"));
  s_instance = this;
}

NativeGp::~NativeGp()
{
  if (m_gather_buffer_field)
    *m_gather_buffer_field = nullptr;
  StopWorker();
  if (m_hooks_set)
    StaticRecompNativeGx::SetGpHooks({});
  if (s_instance == this)
    s_instance = nullptr;
}

bool NativeGp::Enabled()
{
  // Asked on every MMIO access; the answer is fixed once the video backend
  // exists (it does before the first access), so it is evaluated once.
  if (m_enabled_known < 0)
    m_enabled_known = (!m_disabled && StaticRecompNativeGx::OwnsGpu()) ? 1 : 0;
  return m_enabled_known == 1;
}

bool NativeGp::Active()
{
  if (!Enabled())
    return false;
  if (!m_seeded)
    Seed();
  return true;
}

void NativeGp::EnsureHooks()
{
  if (m_hooks_set)
    return;
  StaticRecompNativeGx::GpHooks hooks;
  hooks.pe_sync = [this](u8 reg, u32 value, u32 cycles) { OnPeSync(reg, value, cycles); };
  hooks.before_save = [this] {
    WaitIdle();
    CopyToDolphin();
  };
  hooks.after_load = [this] {
    // A state from the previous path (no GP section): take Dolphin's CP/PI
    // values. One saved by this path already restored ours in DoState.
    WaitIdle();
    if (!m_state_has_section)
      SeedFromDolphin();
    m_state_has_section = false;
  };
  hooks.flush = [this] { WaitIdle(); };
  StaticRecompNativeGx::SetGpHooks(std::move(hooks));
  if (auto* clock = StaticRecompNativeClock::NativeClock::Instance())
  {
    m_pe_event = clock->Register("NativeGpPeSync", [this](u64, s64) { PeEvent(); });
    m_cp_event = clock->Register("NativeGpCpInterrupt",
                                 [this](u64 userdata, s64) { ApplyCpInterrupt(userdata != 0); });
  }
  m_hooks_set = true;
}

void NativeGp::Seed()
{
  m_seeded = true;
  ++m_stats.seeds;
  m_threaded = DualCore();
  SeedFromDolphin();
  EnsureHooks();
  std::fprintf(stderr,
               "[native_gp] GX stream owned natively: gather pipe, CP/PE/PI FIFO registers and the "
               "GP loop on %s (ctrl=0x%02X base=%08X end=%08X rptr=%08X wptr=%08X dist=%u)\n",
               m_threaded ? "its own thread" : "the CPU thread", m_cp.ctrl.load(), m_cp.base, m_cp.end,
               m_cp.read_pointer.load(), m_cp.write_pointer, m_cp.distance.load());
}

void NativeGp::SeedFromDolphin()
{
  // The state Dolphin's CP/PI hold: zero after a boot, or what a savestate
  // written by the previous path (or by CopyToDolphin) carried. Once ours,
  // Dolphin's FIFO loop must stay idle: reads disabled, nothing pending.
  FifoState fifo;  // zero after a boot without a chassis
  if (s_chassis.seed_and_park)
    s_chassis.seed_and_park(fifo);  // the chassis's FIFO loop stays idle from here
  m_cp.base = fifo.base;
  m_cp.end = fifo.end;
  m_cp.hi_watermark = fifo.hi_watermark;
  m_cp.lo_watermark = fifo.lo_watermark;
  m_cp.distance.store(fifo.distance);
  m_cp.write_pointer = fifo.write_pointer;
  m_cp.read_pointer.store(fifo.read_pointer);
  m_cp.breakpoint = fifo.breakpoint;
  m_cp.breakpoint_hit.store(fifo.breakpoint_hit);
  m_cp.ctrl.store(fifo.ctrl);
  m_pi.base = fifo.pi_base;
  m_pi.end = fifo.pi_end;
  m_pi.write_pointer = fifo.pi_write_pointer;
  // The PE enables are set once by __GXPEInit and never cleared by the game.
  m_pe.ctrl = kPeTokenEnable | kPeFinishEnable;
  m_pe.signal_token = m_pe.signal_finish = false;
  m_pe.event_raised.store(false);
  m_pe.token_interrupt_pending.store(false);
  m_pe.finish_pending.store(false);
  *m_gather_count = 0;
  m_staging_read = m_staging_write = 0;
  SetCpStatusFromCpu();
}

void NativeGp::CopyToDolphin()
{
  if (!s_chassis.copy)
    return;
  FifoState fifo;
  fifo.base = m_cp.base;
  fifo.end = m_cp.end;
  fifo.hi_watermark = m_cp.hi_watermark;
  fifo.lo_watermark = m_cp.lo_watermark;
  fifo.distance = m_cp.distance.load();
  fifo.write_pointer = m_cp.write_pointer;
  fifo.read_pointer = m_cp.read_pointer.load();
  fifo.breakpoint = m_cp.breakpoint;
  fifo.breakpoint_hit = m_cp.breakpoint_hit.load();
  fifo.ctrl = m_cp.ctrl.load();
  fifo.pi_base = m_pi.base;
  fifo.pi_end = m_pi.end;
  fifo.pi_write_pointer = m_pi.write_pointer;
  s_chassis.copy(fifo);
}

// ---- registers ----

bool NativeGp::Read(u32 ea, u8 size, u64* value)
{
  if (!Active())
    return false;
  const u32 page = ea & 0xFFFFF000;
  if (page == kCpBase && size == 2)
  {
    ++m_stats.cp_reads;
    *value = ReadCp(ea & 0xFFF);
    return true;
  }
  if (page == kPeBase && size == 2)
  {
    ++m_stats.pe_reads;
    *value = ReadPe(ea & 0xFFF);
    return true;
  }
  if (ea >= kPiFifoBase && ea <= kPiFifoReset && size == 4)
  {
    ++m_stats.pi_reads;
    *value = ReadPi(ea);
    return true;
  }
  return false;
}

bool NativeGp::Write(u32 ea, u8 size, u64 value)
{
  if (!Active())
    return false;
  const u32 page = ea & 0xFFFFF000;
  if (page == kCpBase && size == 2)
  {
    ++m_stats.cp_writes;
    WriteCp(ea & 0xFFF, static_cast<u16>(value));
    return true;
  }
  if (page == kPeBase && size == 2)
  {
    ++m_stats.pe_writes;
    WritePe(ea & 0xFFF, static_cast<u16>(value));
    return true;
  }
  if (ea >= kPiFifoBase && ea <= kPiFifoReset && size == 4)
  {
    ++m_stats.pi_writes;
    WritePi(ea, static_cast<u32>(value));
    return true;
  }
  return false;
}

u16 NativeGp::ReadCp(u32 reg)
{
  switch (reg)
  {
  case CpReg::kStatus:
  {
    const u16 ctrl = m_cp.ctrl.load();
    const u32 distance = m_cp.distance.load();
    const u32 read_pointer = m_cp.read_pointer.load();
    u16 status = 0;
    if (m_cp.hi_watermark_hit.load())
      status |= 1 << 0;
    if (m_cp.lo_watermark_hit.load())
      status |= 1 << 1;
    const bool read_idle = distance == 0 || read_pointer == m_cp.write_pointer;
    const bool at_breakpoint = (ctrl & kCtrlBpEnable) && read_pointer == m_cp.breakpoint;
    const bool command_idle = distance == 0 || at_breakpoint || !(ctrl & kCtrlGpReadEnable);
    if (read_idle)
      status |= 1 << 2;
    if (command_idle)
      status |= 1 << 3;
    if (m_cp.breakpoint_hit.load())
      status |= 1 << 4;
    return status;
  }
  case CpReg::kCtrl:
    return m_cp.ctrl.load();
  case CpReg::kClear:
    return 0;
  case CpReg::kPerfSelect:
    return m_cp.perf_select;
  case 0x0A:
    return m_cp.unk_0a;
  case CpReg::kFifoBaseLo: return Lo(m_cp.base);
  case CpReg::kFifoBaseHi: return Hi(m_cp.base);
  case CpReg::kFifoEndLo: return Lo(m_cp.end);
  case CpReg::kFifoEndHi: return Hi(m_cp.end);
  case CpReg::kFifoHiWatermarkLo: return Lo(m_cp.hi_watermark);
  case CpReg::kFifoHiWatermarkHi: return Hi(m_cp.hi_watermark);
  case CpReg::kFifoLoWatermarkLo: return Lo(m_cp.lo_watermark);
  case CpReg::kFifoLoWatermarkHi: return Hi(m_cp.lo_watermark);
  case CpReg::kFifoRwDistanceLo: return Lo(m_cp.distance.load());
  case CpReg::kFifoRwDistanceHi: return Hi(m_cp.distance.load());
  case CpReg::kFifoWritePointerLo: return Lo(m_cp.write_pointer);
  case CpReg::kFifoWritePointerHi: return Hi(m_cp.write_pointer);
  case CpReg::kFifoReadPointerLo: return Lo(m_cp.read_pointer.load());
  case CpReg::kFifoReadPointerHi: return Hi(m_cp.read_pointer.load());
  case CpReg::kFifoBpLo: return Lo(m_cp.breakpoint);
  case CpReg::kFifoBpHi: return Hi(m_cp.breakpoint);
  default:
    return 0;
  }
}

void NativeGp::WriteCp(u32 reg, u16 value)
{
  switch (reg)
  {
  case CpReg::kCtrl:
  {
    const u16 previous = m_cp.ctrl.load();
    const u16 next = value & 0x3F;
    // Just before disabling reads, let the GP catch up (as Dolphin).
    if ((previous & kCtrlGpReadEnable) && !(next & kCtrlGpReadEnable))
      WaitIdle();
    m_cp.ctrl.store(next);
    SetCpStatusFromCpu();
    RunGp();
    return;
  }
  case CpReg::kClear:
    // Overflow/underflow flags clear; metrics are not kept (as Dolphin).
    if (value & 1)
      m_cp.hi_watermark_hit.store(false);
    if (value & 2)
      m_cp.lo_watermark_hit.store(false);
    UpdateCpInterrupt();
    RunGp();
    return;
  case CpReg::kPerfSelect:
    m_cp.perf_select = value & 7;
    return;
  case 0x0A:
    m_cp.unk_0a = value & 0xFF;
    return;
  case CpReg::kFifoBaseLo: m_cp.base = WithLo(m_cp.base, value); return;
  case CpReg::kFifoBaseHi: m_cp.base = WithHi(m_cp.base, value); return;
  case CpReg::kFifoEndLo: m_cp.end = WithLo(m_cp.end, value); return;
  case CpReg::kFifoEndHi: m_cp.end = WithHi(m_cp.end, value); return;
  case CpReg::kFifoHiWatermarkLo: m_cp.hi_watermark = WithLo(m_cp.hi_watermark, value); return;
  case CpReg::kFifoHiWatermarkHi: m_cp.hi_watermark = WithHi(m_cp.hi_watermark, value); return;
  case CpReg::kFifoLoWatermarkLo: m_cp.lo_watermark = WithLo(m_cp.lo_watermark, value); return;
  case CpReg::kFifoLoWatermarkHi: m_cp.lo_watermark = WithHi(m_cp.lo_watermark, value); return;
  case CpReg::kFifoRwDistanceLo:
    WaitIdle();
    m_cp.distance.store(WithLo(m_cp.distance.load(), value));
    return;
  case CpReg::kFifoRwDistanceHi:
    WaitIdle();
    m_cp.distance.store(WithHi(m_cp.distance.load(), value));
    RunGp();
    return;
  case CpReg::kFifoWritePointerLo: m_cp.write_pointer = WithLo(m_cp.write_pointer, value); return;
  case CpReg::kFifoWritePointerHi: m_cp.write_pointer = WithHi(m_cp.write_pointer, value); return;
  case CpReg::kFifoReadPointerLo:
    WaitIdle();
    m_cp.read_pointer.store(WithLo(m_cp.read_pointer.load(), value));
    m_staging_read = m_staging_write = 0;  // a moved read pointer invalidates what was prefetched
    return;
  case CpReg::kFifoReadPointerHi:
    WaitIdle();
    m_cp.read_pointer.store(WithHi(m_cp.read_pointer.load(), value));
    m_staging_read = m_staging_write = 0;
    return;
  case CpReg::kFifoBpLo: m_cp.breakpoint = WithLo(m_cp.breakpoint, value); return;
  case CpReg::kFifoBpHi: m_cp.breakpoint = WithHi(m_cp.breakpoint, value); return;
  default:
    return;
  }
}

u16 NativeGp::ReadPe(u32 reg)
{
  switch (reg)
  {
  case 0x00: case 0x02: case 0x04: case 0x06: case 0x08:
    return m_pe.poke[reg / 2];
  case 0x0A:
    return m_pe.ctrl;
  case 0x0E:
    return m_pe.token;
  default:
    return 0;  // bounding box and performance counters: not kept
  }
}

void NativeGp::WritePe(u32 reg, u16 value)
{
  switch (reg)
  {
  case 0x00: case 0x02: case 0x04: case 0x06: case 0x08:
    m_pe.poke[reg / 2] = value;  // GXPoke*: pixel-engine state the renderer does not model
    return;
  case 0x0A:
    if (value & kPeTokenAck)
      m_pe.signal_token = false;
    if (value & kPeFinishAck)
      m_pe.signal_finish = false;
    m_pe.ctrl = value & (kPeTokenEnable | kPeFinishEnable);
    UpdatePeInterrupts();
    return;
  default:
    return;
  }
}

u32 NativeGp::ReadPi(u32 reg)
{
  switch (reg)
  {
  case kPiFifoBase: return m_pi.base;
  case kPiFifoEnd: return m_pi.end;
  case kPiFifoWrite: return m_pi.write_pointer;
  default: return 0;
  }
}

void NativeGp::WritePi(u32 reg, u32 value)
{
  switch (reg)
  {
  case kPiFifoBase: m_pi.base = value & 0xFFFFFFE0; return;
  case kPiFifoEnd: m_pi.end = value & 0xFFFFFFE0; return;
  case kPiFifoWrite: m_pi.write_pointer = value & 0xFFFFFFE0; return;
  case kPiFifoReset:
    // GXAbortFrame: the hardware resets the FIFO; the SDK then rewrites every
    // pointer, so nothing else is needed here (Dolphin only logs it too).
    return;
  default: return;
  }
}

// ---- write-gather pipe ----

void NativeGp::GatherWrite(u64 value, u8 size)
{
  // Bytes arrive big-endian in the order the CPU stored them.
  if (m_gather_buffer_field && !*m_gather_buffer_field)
    *m_gather_buffer_field = m_gather.data();
  u32& count = *m_gather_count;
  for (u32 i = size * 8u; i > 0;)
  {
    i -= 8;
    m_gather[count++] = static_cast<u8>(value >> i);
    if (count == kBurst)
    {
      BurstToFifo(m_gather.data());
      count = 0;
    }
  }
}

void NativeGp::BurstToFifo(const u8* burst)
{
  const auto& memory = m_memory;
  ++m_stats.bursts;
  memory.CopyToEmu(m_pi.write_pointer, burst, kBurst);
  if (m_pi.write_pointer == m_pi.end)
    m_pi.write_pointer = m_pi.base;
  else
    m_pi.write_pointer += kBurst;
  // Not linked: the SDK is building a display list in RAM (GXBeginDisplayList).
  if (!(m_cp.ctrl.load() & kCtrlGpLinkEnable))
    return;
  ++m_stats.bursts_linked;
  if (m_cp.write_pointer == m_cp.end)
    m_cp.write_pointer = m_cp.base;
  else
    m_cp.write_pointer += kBurst;
  // Release: the bytes are in RAM before the GP can see the distance grow.
  m_cp.distance.fetch_add(kBurst, std::memory_order_release);
  SetCpStatusFromCpu();
  RunGp();
}

// ---- the GP ----

bool NativeGp::GpCanRun() const
{
  const u16 ctrl = m_cp.ctrl.load(std::memory_order_acquire);
  return (ctrl & kCtrlGpReadEnable) && m_cp.distance.load(std::memory_order_acquire) != 0 &&
         !((ctrl & kCtrlBpEnable) && m_cp.read_pointer.load() == m_cp.breakpoint);
}

void NativeGp::RunGp()
{
  if (m_threaded)
  {
    if (!m_worker.joinable())
      StartWorker();
    {
      std::lock_guard lock(m_mutex);
      m_work_pending = true;
    }
    m_wake.notify_one();
    return;
  }
  if (m_running_gp)
    return;  // re-entered from a register write inside a callback
  m_running_gp = true;
  GpLoop();
  m_running_gp = false;
}

void NativeGp::GpLoop()
{
  const auto& memory = m_memory;
  while (GpCanRun())
  {
    // Linear staging of the ring, as Dolphin's video buffer: compact when the
    // tail runs out of room, then append one burst from the read pointer.
    if (m_staging_write + kBurst > m_staging.size())
    {
      const size_t pending = m_staging_write - m_staging_read;
      std::memmove(m_staging.data(), m_staging.data() + m_staging_read, pending);
      m_staging_read = 0;
      m_staging_write = pending;
    }
    const u32 read_pointer = m_cp.read_pointer.load();
    memory.CopyFromEmu(m_staging.data() + m_staging_write, read_pointer, kBurst);
    m_staging_write += kBurst;
    m_cp.read_pointer.store(read_pointer == m_cp.end ? m_cp.base : read_pointer + kBurst);
    m_cp.distance.fetch_sub(kBurst, std::memory_order_acq_rel);
    u32 cycles = 0;
    const u32 consumed = StaticRecompNativeGx::RunFifo(
        m_staging.data() + m_staging_read, static_cast<u32>(m_staging_write - m_staging_read), &cycles);
    m_staging_read += consumed;
    m_stats.bytes_decoded.fetch_add(consumed, std::memory_order_relaxed);
    m_stats.gp_runs.fetch_add(1, std::memory_order_relaxed);
    if (m_staging_read == m_staging_write)
      m_staging_read = m_staging_write = 0;
  }
  SetCpStatusFromGpu();
}

void NativeGp::StartWorker()
{
  m_stop = false;
  const char* spin_setting = std::getenv("MODERNGEKKO_NATIVE_GP_SPIN");
  const int spin_iterations = spin_setting ? std::atoi(spin_setting) : 20000;
  m_worker = std::thread([this, spin_iterations] {
    if (s_chassis.worker_started)
      s_chassis.worker_started();
    std::unique_lock lock(m_mutex);
    for (;;)
    {
      // The CPU feeds the FIFO in 32-byte bursts; sleeping on every empty gap
      // would deliver tokens and draw-done late. Spin briefly first (as
      // Dolphin's GPU loop does), then sleep.
      for (int i = 0; i < spin_iterations && !m_work_pending && !m_stop; ++i)
      {
        lock.unlock();
        std::this_thread::yield();
        lock.lock();
      }
      m_wake.wait(lock, [this] { return m_work_pending || m_stop; });
      if (m_stop)
        return;
      m_work_pending = false;
      m_worker_busy = true;
      lock.unlock();
      GpLoop();
      lock.lock();
      m_worker_busy = false;
      m_idle.notify_all();
    }
  });
}

void NativeGp::StopWorker()
{
  if (!m_worker.joinable())
    return;
  {
    std::lock_guard lock(m_mutex);
    m_stop = true;
  }
  m_wake.notify_all();
  m_worker.join();
}

void NativeGp::WaitIdle()
{
  if (!m_threaded || !m_worker.joinable())
    return;
  ++m_stats.waits;
  std::unique_lock lock(m_mutex);
  m_idle.wait(lock, [this] { return !m_worker_busy && !m_work_pending; });
}

void NativeGp::SetCpStatusFromCpu()
{
  const u32 distance = m_cp.distance.load();
  m_cp.hi_watermark_hit.store(distance > m_cp.hi_watermark);
  m_cp.lo_watermark_hit.store(distance < m_cp.lo_watermark);
  UpdateCpInterrupt();
}

void NativeGp::SetCpStatusFromGpu()
{
  const u16 ctrl = m_cp.ctrl.load();
  m_cp.breakpoint_hit.store((ctrl & kCtrlBpEnable) && m_cp.read_pointer.load() == m_cp.breakpoint);
  const u32 distance = m_cp.distance.load();
  m_cp.hi_watermark_hit.store(distance > m_cp.hi_watermark);
  m_cp.lo_watermark_hit.store(distance < m_cp.lo_watermark);
  UpdateCpInterrupt();
}

void NativeGp::UpdateCpInterrupt()
{
  const u16 ctrl = m_cp.ctrl.load();
  const bool bp = m_cp.breakpoint_hit.load() && (ctrl & kCtrlBpInt);
  const bool ovf = m_cp.hi_watermark_hit.load() && (ctrl & kCtrlOverflowInt);
  const bool undf = m_cp.lo_watermark_hit.load() && (ctrl & kCtrlUnderflowInt);
  const bool interrupt = (bp || ovf || undf) && (ctrl & kCtrlGpReadEnable);
  if (interrupt)
    m_cp_interrupt_seen.store(true);
  if (OnCpuThread())
  {
    ApplyCpInterrupt(interrupt);
  }
  else
  {
    // From the GP thread: applied on the CPU thread, as Dolphin's CP did.
    if (m_cp_event_skip && !interrupt && !m_cp_interrupt_seen.load())
    {
      m_stats.cp_events_skipped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (auto* clock = StaticRecompNativeClock::NativeClock::Instance())
      clock->Schedule(0, m_cp_event, interrupt ? 1 : 0, false);
  }
}

void NativeGp::ApplyCpInterrupt(bool set)
{
  if (set == m_cp.interrupt_set)
    return;
  m_cp.interrupt_set = set;
  if (set)
  {
    ++m_stats.cp_interrupts;
    m_cp_interrupt_seen.store(true);
  }
  StaticRecompNativePi::NativePi::Raise(kIntCp, set);
}


// ---- pixel engine: tokens and draw-done ----

void NativeGp::OnPeSync(u8 reg, u32 value, u32 cycles)
{
  if (reg == 0x45)
  {
    m_pe.finish_pending.store(true);
    m_stats.finishes.fetch_add(1, std::memory_order_relaxed);
  }
  else
  {
    m_pe.token_pending.store(static_cast<u16>(value & 0xFFFF));
    if (reg == 0x48)
      m_pe.token_interrupt_pending.store(true);
    m_stats.tokens.fetch_add(1, std::memory_order_relaxed);
  }
  RaisePeEvent(cycles);
}

void NativeGp::RaisePeEvent(u32 cycles)
{
  if (m_pe.event_raised.exchange(true))
    return;
  auto* clock = StaticRecompNativeClock::NativeClock::Instance();
  if (!clock)
    return;
  if (OnCpuThread())
  {
    clock->Schedule(std::max(kMinPeDelayCycles, cycles), m_pe_event, 0, true);
  }
  else
  {
    // From the GP thread: no emulated delay, as Dolphin in two-thread mode.
    clock->Schedule(0, m_pe_event, 0, false);
  }
}

void NativeGp::PeEvent()
{
  m_pe.event_raised.store(false);
  m_pe.token = m_pe.token_pending.load();
  if (m_pe.token_interrupt_pending.exchange(false))
  {
    m_pe.signal_token = true;
    UpdatePeInterrupts();
  }
  if (m_pe.finish_pending.exchange(false))
  {
    m_pe.signal_finish = true;
    UpdatePeInterrupts();
    if (s_chassis.frame_update)
      s_chassis.frame_update();
  }
}

void NativeGp::UpdatePeInterrupts()
{
  StaticRecompNativePi::NativePi::Raise(kIntPeToken,
                                        m_pe.signal_token && (m_pe.ctrl & kPeTokenEnable));
  StaticRecompNativePi::NativePi::Raise(kIntPeFinish,
                                        m_pe.signal_finish && (m_pe.ctrl & kPeFinishEnable));
}

// ---- savestates ----

void NativeGp::DoState(StaticRecompState::StateStream& p)
{
  WaitIdle();
  p.Do(m_seeded);
  u16 ctrl = m_cp.ctrl.load();
  u32 distance = m_cp.distance.load(), read_pointer = m_cp.read_pointer.load();
  bool bp_hit = m_cp.breakpoint_hit.load(), hi_hit = m_cp.hi_watermark_hit.load(),
       lo_hit = m_cp.lo_watermark_hit.load();
  p.Do(ctrl);
  p.Do(m_cp.perf_select);
  p.Do(m_cp.unk_0a);
  p.Do(m_cp.base);
  p.Do(m_cp.end);
  p.Do(m_cp.hi_watermark);
  p.Do(m_cp.lo_watermark);
  p.Do(distance);
  p.Do(m_cp.write_pointer);
  p.Do(read_pointer);
  p.Do(m_cp.breakpoint);
  p.Do(bp_hit);
  p.Do(hi_hit);
  p.Do(lo_hit);
  p.Do(m_cp.interrupt_set);
  m_cp.ctrl.store(ctrl);
  m_cp.distance.store(distance);
  m_cp.read_pointer.store(read_pointer);
  m_cp.breakpoint_hit.store(bp_hit);
  m_cp.hi_watermark_hit.store(hi_hit);
  m_cp.lo_watermark_hit.store(lo_hit);
  if (m_cp.interrupt_set)
    m_cp_interrupt_seen.store(true);
  u16 token_pending = m_pe.token_pending.load();
  bool token_int = m_pe.token_interrupt_pending.load(), finish = m_pe.finish_pending.load(),
       raised = m_pe.event_raised.load();
  p.Do(m_pe.ctrl);
  p.Do(m_pe.token);
  p.Do(m_pe.poke);
  p.Do(m_pe.signal_token);
  p.Do(m_pe.signal_finish);
  p.Do(token_pending);
  p.Do(token_int);
  p.Do(finish);
  p.Do(raised);
  m_pe.token_pending.store(token_pending);
  m_pe.token_interrupt_pending.store(token_int);
  m_pe.finish_pending.store(finish);
  m_pe.event_raised.store(raised);
  p.Do(m_pi.base);
  p.Do(m_pi.end);
  p.Do(m_pi.write_pointer);
  p.Do(m_gather);
  p.Do(*m_gather_count);
  u32 pending = static_cast<u32>(m_staging_write - m_staging_read);
  p.Do(pending);
  if (p.IsReadMode())
  {
    m_staging_read = 0;
    m_staging_write = pending;
  }
  if (pending)
    p.DoArray(m_staging.data() + m_staging_read, pending);
  if (p.IsReadMode() && m_seeded)
  {
    // A state saved by this path: our fields are authoritative. The video
    // state's after-load hook must not reseed them from Dolphin's parked copy,
    // whichever order the chunks load in.
    m_state_has_section = true;
    if (Enabled())
    {
      m_threaded = DualCore();
      EnsureHooks();
    }
    if (auto* clock = StaticRecompNativeClock::NativeClock::Instance(); clock && m_pe_event)
    {
      clock->Remove(m_pe_event);
      clock->Remove(m_cp_event);
      if (raised)
        clock->Schedule(0, m_pe_event, 0, true);
    }
  }
}

void NativeGp::PrintStats() const
{
  if (!m_seeded)
    return;
  std::fprintf(stderr,
               "[native_gp] thread=%s bursts=%llu linked=%llu gp_runs=%llu bytes=%llu tokens=%llu "
               "finishes=%llu cp_interrupts=%llu waits=%llu cp_rw=%llu/%llu pe_rw=%llu/%llu "
               "pi_rw=%llu/%llu seeds=%llu cp_events_skipped=%llu\n",
               m_threaded ? "own" : "cpu", static_cast<unsigned long long>(m_stats.bursts),
               static_cast<unsigned long long>(m_stats.bursts_linked),
               static_cast<unsigned long long>(m_stats.gp_runs.load()),
               static_cast<unsigned long long>(m_stats.bytes_decoded.load()),
               static_cast<unsigned long long>(m_stats.tokens.load()),
               static_cast<unsigned long long>(m_stats.finishes.load()),
               static_cast<unsigned long long>(m_stats.cp_interrupts),
               static_cast<unsigned long long>(m_stats.waits),
               static_cast<unsigned long long>(m_stats.cp_reads),
               static_cast<unsigned long long>(m_stats.cp_writes),
               static_cast<unsigned long long>(m_stats.pe_reads),
               static_cast<unsigned long long>(m_stats.pe_writes),
               static_cast<unsigned long long>(m_stats.pi_reads),
               static_cast<unsigned long long>(m_stats.pi_writes),
               static_cast<unsigned long long>(m_stats.seeds),
               static_cast<unsigned long long>(m_stats.cp_events_skipped.load()));
}
}  // namespace StaticRecompNativeGp
