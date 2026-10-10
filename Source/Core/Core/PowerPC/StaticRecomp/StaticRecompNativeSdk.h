// RecompCore: StaticRecomp CPU core - native SDK functions.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host implementations of Nintendo SDK functions the game calls, replacing the
// recompiled originals (the port is code recompiled from the game plus a
// native SDK/hardware layer, as in N64Recomp ports). A module built with a
// native hook list (DolRecomp --native-hooks) exports one flag per SDK function
// start; setting it makes native calls into that function exit to the
// dispatcher, which runs the host implementation here.
//
// Each implementation first checks that the guest code it replaces (and every
// SDK routine whose effect it reproduces) still has the bytes of the original
// DOL, and hands the call back to the recompiled code whenever the SDK is in a
// state the implementation does not model. Verify mode runs the original and
// compares its result with what the implementation computed from the same
// inputs.

#pragma once

#include <array>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "core/cpu.h"

class PointerWrap;

namespace Core
{
class System;
}

namespace StaticRecompNativeSdk
{
// Original guest code a native implementation stands in for (FNV-1a 64 of the
// DOL bytes).
struct CodeFingerprint
{
  u32 address;
  u32 size;
  u64 fnv1a;
};

// Folder of the extracted game (sys/, files/); set by the runtime before boot.
// Native disc reads come from these files.
void SetGameRoot(const std::string& path);

class NativeSdk
{
public:
  explicit NativeSdk(Core::System& system);
  ~NativeSdk();

  // `addresses`/`flags`/`any` come from the module's staticrecomp_native_hooks
  // export. MODERNGEKKO_NATIVE_SDK=off|all|<name,...> selects implementations
  // (default all); MODERNGEKKO_NATIVE_SDK_VERIFY=1 selects verify mode.
  void Bind(u32 count, const u32* addresses, u8* flags, int* any);

  // Dispatcher hook: runs the host implementation for a function start.
  // Returns true when it handled the call (guest pc is the return address).
  bool Run(CPUState& guest)
  {
    if (guest.pc < m_min || guest.pc > m_max)
      return false;
    return RunSlow(guest);
  }

  // Verify mode: called after every dispatch; compares at the original's return.
  void AfterDispatch(const CPUState& guest)
  {
    if (m_pending.function != Function::None && guest.pc == m_pending.lr &&
        guest.gpr[1] == m_pending.sp)
      FinishVerify(guest);
    if (m_audio_verify_lr != 0 && guest.pc == m_audio_verify_lr &&
        guest.gpr[1] == m_audio_verify_sp)
      AudioFinishVerify();
    if (!m_trace_calls.empty())
      TraceReturns(guest);
  }

  void PrintStats() const;

  // Savestate section: pending native interrupts and the native audio state.
  // `present` is false when loading a state saved without it.
  void DoState(PointerWrap& p, bool present);

  // Native audio DMA: block boundary (CoreTiming event).
  void AudioDmaEvent(s64 cycles_late);

private:
  struct PadResult
  {
    std::array<u8, 48> status{};    // PADStatus[4]
    std::array<u8, 48> previous{};  // SDK copy of the last status (dimming)
    std::array<u32, 4> input_buffer_hi{}, input_buffer_lo{};
    std::array<bool, 4> data_read{};
    std::array<bool, 4> type_read{};
    u32 barrel_bits = 0;
    u32 motor = 0;
    bool reset_idle = false;
  };
  struct KpadResult
  {
    bool busy = false;          // KPADRead re-entered: returns 0, writes nothing
    u32 control_block = 0;      // WPAD control block whose extension callback is set
  };
  struct Stats
  {
    u64 native = 0, deferred = 0, verified = 0, mismatches = 0, verify_skipped = 0;
  };
  enum class Function
  {
    None,
    PadRead,
    KpadRead,
    DvdRead,
  };
  // A file of the extracted disc (sys/fst.bin), in partition data offsets.
  struct DiscFile
  {
    u64 offset;
    u32 size;
    std::string path;
  };
  // Verify mode: a read the original performs, with the bytes the native
  // implementation would have copied.
  struct DvdCheck
  {
    u32 block;
    u32 address;
    std::vector<u8> data;
  };
  struct Pending
  {
    Function function = Function::None;
    u32 lr = 0, sp = 0, status = 0;
    u32 idle_before = 0;  // dimming idle count at the call
    u32 chan = 0;
    PadResult pad;
    KpadResult kpad;
  };

  bool RunSlow(CPUState& guest);
  bool PadRead(CPUState& guest);
  // Computes PADRead's effects from host input (or, in verify mode, from the
  // emulated SI latch the original reads). False: state not modelled.
  bool ComputePadRead(const CPUState& guest, bool from_si_latch, PadResult& out);
  bool GetHostPadData(int chan, u32 analog_mode, u32& hi, u32& lo);
  bool KpadRead(CPUState& guest);
  // KPADRead with no Wii Remote connected (the PC port has none): returns no
  // samples. False: a Wii Remote is connected or the state is not modelled.
  bool ComputeKpadRead(const CPUState& guest, KpadResult& out);
  void Verify(const CPUState& guest, Function function, bool computed);
  void FinishVerify(const CPUState& guest);
  bool PadMatches(const CPUState& guest);
  bool KpadMatches(const CPUState& guest);
  bool DvdReadAbs(CPUState& guest);
  // Reads `length` bytes at partition offset `offset` from the extracted files.
  bool ReadDisc(u64 offset, u32 length, std::vector<u8>& out);
  bool LoadDiscFiles(const CPUState& guest);

  // Native interrupts: completions of native operations reach the game the
  // way hardware ones do. A callback is queued and an otherwise unused PI line
  // (DEBUG, OS interrupt 25) is raised; the game's OS takes the interrupt and
  // calls its handler table entry, which points at a never-executed debugger
  // function the dispatcher intercepts. The handler jumps to the callback,
  // which returns into the OS dispatcher (scheduler, context restore).
  enum class CallbackKind : u8
  {
    Call,          // jump to `function` with (r3, r4)
    DspInterrupt,  // enter the SDK's DSP interrupt handler (__DSPHandler)
    AiInterrupt,   // enter the SDK's audio DMA interrupt handler (__AIDHandler)
  };
  struct NativeCallback
  {
    u32 function, r3, r4;
    u32 dvd_block = 0;  // DVD read completing in this interrupt (state set here)
    u32 dvd_length = 0;
    CallbackKind kind = CallbackKind::Call;
  };
  bool PostCallback(const CPUState& guest, const NativeCallback& call);
  bool InstallNativeInterrupt(const CPUState& guest);
  bool DeliverCallback(CPUState& guest);
  std::vector<NativeCallback> m_callbacks;  // FIFO
  bool m_interrupt_hook = false;
  bool m_interrupt_not_ready_reported = false;
  u64 m_callbacks_posted = 0, m_callbacks_delivered = 0, m_spurious_interrupts = 0;
  u64 m_dvd_cancelled = 0;
  bool CopyToGuest(const CPUState& guest, u32 address, const u8* data, u32 length) const;
  void CheckDvdReads(const CPUState& guest);
  // Enters an SDK interrupt handler after its hardware acknowledgement:
  // pushes the handler's frame as its prologue does and resumes at `resume`.
  bool EnterInterruptHandler(CPUState& guest, u32 frame_size, u32 resume);

  // Native audio (StaticRecompNativeAudio.cpp): the DSP is replaced by a host
  // AX mixer and the audio interface DMA by a host engine. The SDK's own
  // dsp.a/ai.a code still runs, except the functions that touch registers.
  struct AudioState;
  bool AudioBind(const std::function<bool(u32)>& enable, bool wanted);
  bool AudioRun(CPUState& guest);
  bool AudioActivate(const CPUState& guest);
  bool AudioEnterHandler(CPUState& guest, CallbackKind kind);
  void AudioPushMail(u32 mail, bool interrupt);
  void AudioPostInterrupt(CallbackKind kind);
  void AudioHandleMail(u32 mail);
  void AudioFinishVerify();
  void AudioStartDma();
  void AudioConfigureHost();
  void AudioDoState(PointerWrap& p);
  void PrintAudioStats() const;
  std::unique_ptr<AudioState> m_audio;
  u32 m_audio_verify_lr = 0, m_audio_verify_sp = 0;
  CPUState* m_last_guest = nullptr;

  u8* Pointer(const CPUState& guest, u32 address, u32 size) const;
  bool Read32(const CPUState& guest, u32 address, u32& value) const;
  template <size_t N>
  bool CodeMatches(const CPUState& guest, const std::array<CodeFingerprint, N>& code) const;
  Stats& StatsFor(Function function)
  {
    return function == Function::PadRead ? m_pad_stats :
           function == Function::KpadRead ? m_kpad_stats : m_dvd_stats;
  }

  // Tracing (MODERNGEKKO_NATIVE_SDK_TRACE=<hex,...>, output to
  // MODERNGEKKO_NATIVE_SDK_TRACE_FILE): logs each call to a hooked function
  // with its arguments and, at its return, the result and elapsed time. The
  // original code still runs.
  struct TraceCall
  {
    u32 pc, lr, sp;
    u64 timebase;
  };
  void TraceCallEntry(const CPUState& guest);
  void TraceReturns(const CPUState& guest);
  std::vector<u32> m_trace;
  std::vector<TraceCall> m_trace_calls;
  FILE* m_trace_file = nullptr;

  Core::System& m_system;
  bool m_verify = false;
  bool m_pad_read = false;
  bool m_kpad_read = false;
  bool m_dvd_read = false;
  bool m_dvd_status_hook = false;  // verify mode: DVDGetCommandBlockStatus exits too
  std::vector<DiscFile> m_disc_files;  // sorted by offset
  std::vector<DvdCheck> m_dvd_checks;
  u64 m_dvd_bytes = 0;
  u32 m_fst_address = 0;  // guest FST the file table was built from
  u32 m_min = 1, m_max = 0;
  Pending m_pending;
  Stats m_pad_stats;
  Stats m_kpad_stats;
  Stats m_dvd_stats;
};
}  // namespace StaticRecompNativeSdk
