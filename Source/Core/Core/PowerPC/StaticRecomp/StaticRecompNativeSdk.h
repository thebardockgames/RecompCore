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
#include <optional>
#include <string>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "core/cpu.h"

namespace StaticRecompState
{
class StateStream;
}

namespace Core
{
class System;
}

namespace StaticRecompNativeNand
{
class NandStore;
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
// Host folder of the Wii NAND (the user's Wii directory), for the native store.
void SetNandRoot(const std::string& path);
const std::string& GetNandRoot();
const std::string& GameRoot();
// Folder of user mods (music/ ...); empty: no mods. Served by native disc reads.
void SetModsRoot(const std::string& path);
// The running title's uid and group (phase G: set by the native boot as ES
// does; the NAND layer uses them for the game's file operations).
void SetTitleIdentity(u32 uid, u16 gid);
bool GetTitleIdentity(u32* uid, u16* gid);

class NativeSdk
{
public:
  // Which chassis subsystems the native layer has replaced (set when the
  // replacement binds). The native clock retires the chassis's periodic
  // events of those subsystems: they only rescheduled themselves.
  static bool AudioNative() { return s_audio_native; }
  static bool IpcNative() { return s_ipc_native; }
  static inline bool s_audio_native = false;
  static inline bool s_ipc_native = false;

  // Legacy AOT modules can expose boot calls through the host callback.
  bool SkipBootNotice(CPUState& guest);
  // A menu extension module's boot-notice event (0xffff0040 strap,
  // 0xffff0041 controllers): SkipBootNotice at that screen's entry.
  bool SkipBootNoticeEvent(CPUState& guest, u32 event);
  // What the SDK layer needs from the chassis while it exists (installed by
  // StaticRecompNativeSdkChassis): the SI latch for the PADRead verification
  // mode, its DSP/audio DMA parked once native audio runs, the AI and DSP
  // register values a `.sav` of the chassis carries (false when the native
  // host loop left the device uninitialised: the power-on values are used),
  // the title identity from its IOS kernel (chassis boot only) and the
  // migration of file handles from states written before the own NAND store.
  struct ChassisHooks
  {
    std::function<u32()> si_status;
    std::function<void(int chan, u32& hi, u32& lo)> si_input;
    std::function<void()> park_audio;
    std::function<bool(u32& control, u32& volume, u32& interrupt_timing)> ai_seed;
    std::function<bool(u16& control)> dsp_control_seed;
    std::function<bool(u32* uid, u16* gid)> ios_title_identity;
    // Offset of a chassis FS handle (closed afterwards); nullopt = unknown.
    std::function<std::optional<u32>(u32 fs_fd)> migrate_fs_handle;
  };
  static void SetChassisHooks(ChassisHooks hooks);
  static inline ChassisHooks s_chassis;

  NativeSdk();
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
    if (m_thp && ThpVerifyPending(guest))
      ThpFinishVerify(guest);
    if (!m_trace_calls.empty())
      TraceReturns(guest);
  }

  void PrintStats() const;

  // Savestate section: pending native interrupts and the native audio state.
  // `present` is false when loading a state saved without it.
  void DoState(StaticRecompState::StateStream& p, bool present, u32 version);

  // Native audio DMA: block boundary (CoreTiming event).
  void AudioDmaEvent(s64 cycles_late);

  // Synchronous host calls dispatch a temporary CPUState. Rebind to their
  // live caller before that temporary leaves scope: timing callbacks retain
  // this reference between dispatches (including with muted host output).
  void RestoreCallbackGuest(CPUState& guest) noexcept {
    m_last_guest = &guest;
    ++m_callback_guest_restores;
  }
  void AuditCallbackGuest(const CPUState& guest);

  // Inline native replacements (StaticRecompNativeInline.cpp): fills the
  // module's table (staticrecomp_native_inline) for the functions this layer
  // runs in place of a direct call. Call after Bind.
  using InlineFunction = int (*)(CPUState*);
  void BindInline(u32 count, InlineFunction* table);
  // Verify mode: runs the guest function at `address` on a copy of `guest`
  // (same stack) and returns the state at its return.
  using InlineVerifier = std::function<std::optional<CPUState>(CPUState& guest, u32 address)>;
  static void SetInlineVerifier(InlineVerifier verifier);
  bool RunSqrt(CPUState& cpu);

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
  // SPEC2_MakeStatus on a poll response (hi, lo): fills st[0..9], updates
  // `previous`, the barrel bit and the dimming reset. True: origin request.
  static bool MakeStatusSpec2(u8* st, u32 hi, u32 lo, u32 analog_mode, u32 pad_type,
                              const u8* origin, u8* previous, u32 bit, u32& barrel_bits,
                              bool& reset_idle);

  // Native PAD library (StaticRecompNativePad.cpp): PADReset, PADRecalibrate,
  // PADRead and PADControlMotor on the SDK's own variables and the PC
  // controller, without the serial interface.
  struct PadState;

public:
  // The PAD library runs natively (no serial interface): the native VI may skip the SI poll.
  bool PadNative() const { return m_pad != nullptr; }
  // The SI register page (0xCD006400..FF) as the SDK's SIInit sees it: plain
  // storage that always reads idle (phase D closure; no emulated transfers).
  bool SiRegisterRead(u32 ea, u8 size, u64* value);
  bool SiRegisterWrite(u32 ea, u8 size, u64 value);
  // Audio interface registers (0xCD006C00: control, volume, sample counter,
  // interrupt timing). The SDK's AI functions are native; what remains are
  // the direct reads of AICR (AIGetStreamPlayState) and the few writes.
  bool AiRegisterRead(u32 ea, u8 size, u64* value);
  bool AiRegisterWrite(u32 ea, u8 size, u64 value);
  // The DSP control register (0xCC00500A, 16-bit): __OSInterruptInit reads it
  // and writes the interrupt masks at boot; the SDK's DSP functions are native.
  bool DspControlRead(u32 ea, u8 size, u64* value);
  bool DspControlWrite(u32 ea, u8 size, u64 value);

private:
  bool PadBind(const std::function<bool(u32)>& enable, bool wanted);
  bool PadRun(CPUState& guest);
  bool PadReady(const CPUState& guest);
  bool PadReadNoSi(CPUState& guest);
  void PadReset(const CPUState& guest, u32 mask, bool recalibrate);
  void PadReadOrigin(const CPUState& guest, u32 chan);
  void PadUpdateOrigin(const CPUState& guest, u32 chan);
  bool PadVar(const CPUState& guest, u32 address, u32& value) const;
  void PadSetVar(const CPUState& guest, u32 address, u32 value) const;
  bool HostPadConnected(int chan);
  bool SiInitGetType(CPUState& guest);
  void PrintPadStats() const;
  std::unique_ptr<PadState> m_pad;

  // Native WUD library (StaticRecompNativeWud.cpp): WUDInit completes at once
  // as "adapter ready, no Wii Remote" and the bte entry points wud.a reaches
  // become no-ops, so the Bluetooth stack and the IOS USB device never run.
  struct WudState;
  bool WudBind(const std::function<bool(u32)>& enable, bool wanted);
  bool WudRun(CPUState& guest);
  void PrintWudStats() const;
  std::unique_ptr<WudState> m_wud;

  bool DvdReadAbs(CPUState& guest);
  // Reads `length` bytes at partition offset `offset` from the extracted files.
  bool ReadDisc(u64 offset, u32 length, std::vector<u8>& out);
  bool LoadDiscFiles(const CPUState& guest);

  // Native THP video decoder (StaticRecompNativeThp.cpp): THPVideoDecode on
  // the host, with the SDK's arithmetic (identical planes).
  struct ThpState;
  bool ThpBind(const std::function<bool(u32)>& enable, bool wanted);
  bool ThpRun(CPUState& guest);
  bool ThpDecodeFrame(const CPUState& guest, ThpState& thp);
  bool ThpReadGuest(const CPUState& guest, u32 address, size_t size, std::vector<u8>& out) const;
  bool ThpWriteGuest(const CPUState& guest, u32 address, const std::vector<u8>& data) const;
  void ThpFinishVerify(const CPUState& guest);
  bool ThpVerifyPending(const CPUState& guest) const;
  void PrintThpStats() const;
  std::unique_ptr<ThpState> m_thp;

  // Boot patch: the Wii wrist strap and controller notice screens that
  // FirstRun_Main shows before the save check return at once
  // (MODERNGEKKO_WII_NOTICES=on keeps them).
  bool SkipNotice(CPUState& guest);
  bool m_skip_notices = false;
  u64 m_skipped_notices = 0;

  // Mods (StaticRecompNativeMods.cpp): songs in <mods>/music replace or add
  // to the game's music. The AFS index of wzs3us2.afs is corrected as it is
  // read, pointing music entries at a virtual area past the end of the disc,
  // whose reads come from the mod files.
  struct ModsState;
  bool ModsBind(const std::function<bool(u32)>& enable);
  bool ModsRun(CPUState& guest);
  void PrintModsStats() const;
  bool m_mods_hooks = false;
  void ModsInit(const CPUState& guest);
  bool ModsRead(u64 offset, u32 length, std::vector<u8>& out);
  void ModsPatchRead(u64 offset, std::vector<u8>& data);
  std::unique_ptr<ModsState> m_mods;
  bool m_mods_checked = false;

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
    ContextCall,   // call `function`(r3, r4) on a fresh OSContext, as the SDK's
                   // IPC reply handler does (the callback may use the FPU)
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
  bool EnterContextCall(CPUState& guest, const NativeCallback& call);
  bool ReturnFromContextCall(CPUState& guest);
  void ClearGuestContext(const CPUState& guest, u32 context);
  void SetGuestContext(CPUState& guest, u32 context);
  // Posts now, or keeps the callback until the OS takes interrupts.
  void PostOrDefer(const NativeCallback& call);
  std::vector<NativeCallback> m_deferred_callbacks;
  bool m_context_call_hook = false;

  // Native NAND file system (StaticRecompNativeFs.cpp): the game's IPC calls
  // for /dev/fs and NAND paths are served by the host NAND directly, without
  // the IOS kernel, device dispatch or request timing.
  struct FsState;
  bool FsBind(const std::function<bool(u32)>& enable, bool wanted);
  bool FsRun(CPUState& guest);
  void FsDoState(StaticRecompState::StateStream& p, u32 version);
  std::unique_ptr<StaticRecompNativeNand::NandStore> m_nand;  // phase C3: own NAND store
  void PrintFsStats() const;
  std::unique_ptr<FsState> m_fs;

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
  void AudioDoState(StaticRecompState::StateStream& p, u32 version);
  void PrintAudioStats() const;
  std::unique_ptr<AudioState> m_audio;
  u32 m_audio_verify_lr = 0, m_audio_verify_sp = 0;
  CPUState* m_last_guest = nullptr;
  u64 m_callback_guest_restores = 0, m_callback_guest_checks = 0, m_callback_guest_mismatches = 0;

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

  bool m_verify = false;
  bool m_pad_read = false;
  bool m_si_set_xy = false;
  bool m_si_get_type = false;         // SIGetType from SIInit completed natively
  bool m_si_code_checked = false;
  std::array<u32, 64> m_si_regs{};    // 0xCD006400..0xCD0064FF as the SDK leaves them
  u64 m_si_register_accesses = 0;
  u64 m_si_init_probes = 0;
  bool m_kpad_read = false;
  bool m_dvd_read = false;
  bool m_dvd_status_hook = false;  // verify mode: DVDGetCommandBlockStatus exits too
  std::vector<DiscFile> m_disc_files;  // sorted by offset
  std::vector<DvdCheck> m_dvd_checks;
  u64 m_dvd_bytes = 0;
  u32 m_fst_address = 0;  // guest FST the file table was built from
  u32 m_min = 1, m_max = 0;
  const u32* m_hook_addresses = nullptr;  // the module's hook list (sorted)
  u32 m_hook_count = 0;
  struct InlineStats
  {
    bool checked = false, enabled = false;
    u64 calls = 0, fallbacks = 0, budget = 0, verified = 0, mismatches = 0;
  };
  InlineStats m_inline_sqrt;
  bool m_inline_verify = false;
  void PrintInlineStats() const;
  Pending m_pending;
  Stats m_pad_stats;
  Stats m_kpad_stats;
  Stats m_dvd_stats;
};
}  // namespace StaticRecompNativeSdk
