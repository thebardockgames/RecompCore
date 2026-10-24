// RecompCore: StaticRecomp CPU core - native graphics processor front (phase E).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// With the NativeGX renderer the GX command stream was still routed through
// Dolphin: the write-gather pipe (GPFifo), the command processor registers and
// FIFO loop (CommandProcessor, Fifo) and the pixel engine registers
// (PixelEngine) fed NativeGXBridge::RunFifo. This class owns those pieces:
//
//  - the CPU's write-gather pipe at 0xCC008000 (32-byte bursts into the FIFO
//    the SDK placed in RAM through the PI FIFO registers);
//  - the CP registers at 0xCC000000 (status, control, clear, FIFO base/end,
//    watermarks, distance, read/write pointers, breakpoint) with the semantics
//    the SDK relies on (GXSetCPUFifo, GXSetGPFifo, __GXSaveFifo, the CP
//    interrupt handler);
//  - the PI FIFO registers at 0xCC00300C-0xCC003018;
//  - the PE registers at 0xCC001000 (control with token/finish acknowledge and
//    enable bits, the token value, the poke registers);
//  - the GP itself: whenever the linked FIFO holds data and reads are enabled,
//    the bytes go to the native front end. With one emulation thread that
//    happens synchronously on the CPU thread (deterministic); with two
//    (MAIN_CPU_THREAD, the play configuration) a GP worker thread consumes the
//    FIFO like the hardware does, and the CPU only waits on tokens, draw-done
//    and the FIFO watermarks. Those become PE/CP state and PI interrupts on
//    the CPU thread through the native clock, as Dolphin's PixelEngine did.
//
// Dolphin's CommandProcessor, Fifo, GPFifo and PixelEngine never see these
// accesses while this is active; ProcessorInterface still delivers the
// interrupts (phase F) and the renderer stays in NativeGXBridge.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompState
{
class StateStream;
}
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestMemory.h"

namespace StaticRecompNativeClock
{
struct EventType;
}

namespace StaticRecompNativeGp
{
// Command processor register offsets in the 0xCC000000 page (16-bit).
namespace CpReg
{
constexpr u32 kStatus = 0x00;
constexpr u32 kCtrl = 0x02;
constexpr u32 kClear = 0x04;
constexpr u32 kPerfSelect = 0x06;
constexpr u32 kFifoBaseLo = 0x20;
constexpr u32 kFifoBaseHi = 0x22;
constexpr u32 kFifoEndLo = 0x24;
constexpr u32 kFifoEndHi = 0x26;
constexpr u32 kFifoHiWatermarkLo = 0x28;
constexpr u32 kFifoHiWatermarkHi = 0x2a;
constexpr u32 kFifoLoWatermarkLo = 0x2c;
constexpr u32 kFifoLoWatermarkHi = 0x2e;
constexpr u32 kFifoRwDistanceLo = 0x30;
constexpr u32 kFifoRwDistanceHi = 0x32;
constexpr u32 kFifoWritePointerLo = 0x34;
constexpr u32 kFifoWritePointerHi = 0x36;
constexpr u32 kFifoReadPointerLo = 0x38;
constexpr u32 kFifoReadPointerHi = 0x3A;
constexpr u32 kFifoBpLo = 0x3C;
constexpr u32 kFifoBpHi = 0x3E;
}  // namespace CpReg

// CP control bits (UCPCtrlReg).
namespace CpCtrl
{
constexpr u16 kGpReadEnable = 1 << 0;
constexpr u16 kBpEnable = 1 << 1;
constexpr u16 kOverflowInt = 1 << 2;
constexpr u16 kUnderflowInt = 1 << 3;
constexpr u16 kGpLinkEnable = 1 << 4;
constexpr u16 kBpInt = 1 << 5;
}  // namespace CpCtrl

class NativeGp
{
public:
  // The FIFO registers a `.sav` of the chassis carries (its CommandProcessor
  // FIFO and the PI's CPU FIFO), for seeding and for keeping its copy in step.
  struct FifoState
  {
    u32 base = 0, end = 0, hi_watermark = 0, lo_watermark = 0, distance = 0;
    u32 write_pointer = 0, read_pointer = 0, breakpoint = 0;
    bool breakpoint_hit = false;
    u16 ctrl = 0;  // CpCtrl bits
    u32 pi_base = 0, pi_end = 0, pi_write_pointer = 0;
  };
  // What the GP front needs from the chassis while it exists (installed by
  // StaticRecompNativeGpChassis): its FIFO registers (seed, then parked
  // idle; copy back before its savestate), whether it runs two emulation
  // threads, and its per-frame update on a draw-done. Without an adapter the
  // FIFO starts at zero (as after a boot) and runs on the CPU thread.
  struct ChassisHooks
  {
    std::function<void(FifoState& state)> seed_and_park;
    std::function<void(const FifoState& state)> copy;
    std::function<bool()> dual_core;
    std::function<void()> frame_update;
    // Called on the GP worker thread when it starts (the host names it).
    std::function<void()> worker_started;
  };
  static void SetChassisHooks(ChassisHooks hooks);

  NativeGp();
  ~NativeGp();

  // True when this object serves the GX stream (NativeGX renderer owns the
  // GPU and MODERNGEKKO_NATIVE_GP is not "off").
  bool Active();

  // Memory-mapped register accesses from generated code (effective address in
  // the 0xCC00xxxx hardware window). Return true when handled.
  bool Read(u32 ea, u8 size, u64* value);
  bool Write(u32 ea, u8 size, u64 value);
  // The gather pipe's common case, inline: an active front with room left in
  // the current 32-byte burst takes the bytes (big-endian, store order)
  // without the general path. False: use Active() + GatherWrite().
  bool TryGatherFast(u64 value, u8 size)
  {
    if (m_enabled_known != 1 || !m_seeded || *m_gather_count + size >= m_gather.size())
      return false;
    u8* out = m_gather.data() + *m_gather_count;
    for (u32 i = size * 8u, n = 0; i > 0; ++n)
    {
      i -= 8;
      out[n] = static_cast<u8>(value >> i);
    }
    *m_gather_count += size;
    return true;
  }
  // CPU ABI v5: the generated code appends gather-pipe stores to the current
  // burst itself (CPUState::gather_buffer/gather_count) while they fit. The
  // count lives in `count` from now on; the buffer is published in `buffer`
  // once the front is active (first slow-path gather write) and withdrawn
  // when this object goes away.
  void BindGather(u8** buffer, u32* count)
  {
    *count = *m_gather_count;
    m_gather_count = count;
    m_gather_buffer_field = buffer;
  }
  // A store to the write-gather pipe page (0xCC008000).
  void GatherWrite(u64 value, u8 size);

  // Savestates: own section, and the Dolphin structures kept in step so states
  // stay loadable by the previous path.
  void DoState(StaticRecompState::StateStream& p);
  void CopyToDolphin();
  void SeedFromDolphin();

  void PrintStats() const;

  struct CpRegs
  {
    std::atomic<u16> ctrl{0};  // UCPCtrlReg
    u16 perf_select = 0, unk_0a = 0;
    u32 base = 0, end = 0, hi_watermark = 0, lo_watermark = 0;
    std::atomic<u32> distance{0};
    u32 write_pointer = 0;
    std::atomic<u32> read_pointer{0};
    u32 breakpoint = 0;
    std::atomic<bool> breakpoint_hit{false}, hi_watermark_hit{false}, lo_watermark_hit{false};
    bool interrupt_set = false;
  };
  struct PeRegs
  {
    u16 ctrl = 0;   // bit0 token enable, bit1 finish enable (ack bits are write-only)
    u16 token = 0;  // PE_TOKEN_REG
    std::array<u16, 5> poke{};  // zconf, alphaconf, dstalphaconf, alphamode, alpharead
    bool signal_token = false, signal_finish = false;
    // Set by the GP (worker or CPU thread), consumed on the CPU thread.
    std::atomic<u16> token_pending{0};
    std::atomic<bool> token_interrupt_pending{false}, finish_pending{false}, event_raised{false};
  };
  struct PiFifo
  {
    u32 base = 0, end = 0, write_pointer = 0;
  };
  struct Stats
  {
    u64 bursts = 0, bursts_linked = 0;
    std::atomic<u64> gp_runs{0}, bytes_decoded{0}, tokens{0}, finishes{0};
    u64 cp_reads = 0, cp_writes = 0, pe_reads = 0, pe_writes = 0, pi_reads = 0, pi_writes = 0;
    u64 cp_interrupts = 0, seeds = 0, waits = 0;
    std::atomic<u64> cp_events_skipped{0};
  };
  // Set once the CP interrupt line has been (or was asked to be) high: until
  // then a status update from the GP thread can only apply "low" over "low",
  // which does nothing, so it needs no event on the CPU thread.
  std::atomic<bool> m_cp_interrupt_seen{false};
  bool m_cp_event_skip = true;  // MODERNGEKKO_NATIVE_GP_CP_EVENT_SKIP=off: always schedule

private:
  bool Enabled();
  void EnsureHooks();
  void Seed();
  u16 ReadCp(u32 reg);
  void WriteCp(u32 reg, u16 value);
  u16 ReadPe(u32 reg);
  void WritePe(u32 reg, u16 value);
  u32 ReadPi(u32 reg);
  void WritePi(u32 reg, u32 value);
  void BurstToFifo(const u8* burst);
  // Starts or wakes the GP for the data in the FIFO.
  void RunGp();
  // Consumes the FIFO until it is empty, reads are off or the breakpoint hits.
  void GpLoop();
  bool GpCanRun() const;
  void StartWorker();
  void StopWorker();
  // Blocks until the worker has consumed everything it can (no-op when
  // synchronous). Used before register writes that move the read side and
  // around savestates.
  void WaitIdle();
  void SetCpStatusFromCpu();
  void SetCpStatusFromGpu();
  void UpdateCpInterrupt();
  void ApplyCpInterrupt(bool set);
  void UpdatePeInterrupts();
  void OnPeSync(u8 reg, u32 value, u32 cycles);
  void RaisePeEvent(u32 cycles);
  void PeEvent();

  bool DualCore() const { return s_chassis.dual_core && s_chassis.dual_core(); }
  // The CPU thread is every thread but the GP worker (the only other thread
  // that runs this object).
  bool OnCpuThread() const { return !m_threaded || std::this_thread::get_id() != m_worker.get_id(); }

  bool m_disabled = false;
  int m_enabled_known = -1;  // -1 unknown, else 0/1 (see Enabled)
  bool m_seeded = false;
  bool m_hooks_set = false;
  bool m_running_gp = false;
  bool m_state_has_section = false;  // a loaded state carried our registers
  bool m_threaded = false;           // GP on its own thread (two emulation threads)
  CpRegs m_cp;
  PeRegs m_pe;
  PiFifo m_pi;
  std::array<u8, 32> m_gather{};
  u32 m_gather_count_own = 0;
  u32* m_gather_count = &m_gather_count_own;  // see BindGather
  u8** m_gather_buffer_field = nullptr;
  std::vector<u8> m_staging;  // linear copy of the FIFO bytes not yet consumed (GP side)
  size_t m_staging_read = 0, m_staging_write = 0;
  StaticRecompGuestMemory::GuestMemory m_memory;  // the game's memory, the chassis's buffers for now
  StaticRecompNativeClock::EventType* m_pe_event = nullptr;
  StaticRecompNativeClock::EventType* m_cp_event = nullptr;
  Stats m_stats;

  // Worker thread state.
  std::thread m_worker;
  std::mutex m_mutex;
  std::condition_variable m_wake, m_idle;
  bool m_work_pending = false;
  bool m_worker_busy = false;
  bool m_stop = false;

  static NativeGp* s_instance;
  static ChassisHooks s_chassis;
};
}  // namespace StaticRecompNativeGp
