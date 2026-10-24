// RecompCore: StaticRecomp CPU core - native audio.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The game's audio stack (CRI ADX, its own sound effects and the HOME menu
// sound all play through Nintendo's AX library; THP movies through the audio
// DMA directly) ends in two pieces of hardware: the DSP, which runs the AXWii
// microcode on the command lists AX builds every 3 ms, and the audio
// interface DMA, which plays the mixed buffers and interrupts at the end of
// each. Both are replaced here:
//
//  * DSP: the SDK's dsp.a code still runs (task list, boot protocol, the
//    interrupt handler __DSPHandler that calls AX's task callbacks), but its
//    four mailbox accessors and DSPInit are native. Mails to the DSP drive a
//    host model of the boot ROM and of the AXWii microcode, whose command
//    lists execute at once on guest memory (NativeAxWii). Its answers are
//    queued as mails and raise a native interrupt that enters __DSPHandler
//    past its register acknowledgement.
//  * Audio DMA: AIInit/AIInitDMA/AIStartDMA and the register getters are
//    native. A native clock event at the end of each block sends the block to
//    the host mixer and raises a native interrupt that enters __AIDHandler
//    past its acknowledgement; the SDK's handler calls the game's callback.
//  * OS audio init/stop (__OSInitAudioSystem, __OSStopAudioSystem), which
//    program the hardware directly, are native too.
//
// Dolphin's DSP and audio DMA devices are told to stay idle. Every replaced
// function, and every SDK routine the protocol relies on, is checked against
// the original DOL bytes before anything is replaced; native audio starts only
// from boot (the first hook must be __OSInitAudioSystem), and is all or
// nothing. Verify mode leaves the emulated path in place and runs the native
// mixer on the same command lists, comparing what it would write.

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeAudio.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

#include "Core/PowerPC/StaticRecomp/StaticRecompHostAudio.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompUtil.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestMemory.h"

namespace StaticRecompNativeSdk
{
namespace
{
#include "native/audio.inc"  // the game's addresses and fingerprints

constexpr u32 kBusClock = 0x800000F8;
// Both SDK interrupt handlers open with stwu r1,-0x2E0(r1).
constexpr u32 kHandlerFrame = 0x2E0;



// DSP mail protocol (Dolphin UCodes.h / AX.h).
constexpr u32 kRomReady = 0x8071FEED;
constexpr u32 kDspInitMail = 0xDCD10000;
constexpr u32 kDspResume = 0xDCD10001;
constexpr u32 kDspYield = 0xDCD10002;
constexpr u32 kDspSync = 0xDCD10004;
constexpr u32 kMailCmdList = 0xBABE0000;
constexpr u32 kMailResume = 0xCDD10000;
constexpr u32 kMailNewUcode = 0xCDD10001;
constexpr u32 kMailReset = 0xCDD10002;
constexpr u32 kMailContinue = 0xCDD10003;

constexpr s64 kAudioHookCycles = 20;
constexpr s64 kCommandListCycles = 200;


u64 Fnv1a(const u8* data, u32 size)
{
  u64 hash = 0xcbf29ce484222325ull;
  for (u32 i = 0; i < size; ++i)
    hash = (hash ^ data[i]) * 0x100000001b3ull;
  return hash;
}

void Store32(u8* p, u32 value)
{
  p[0] = static_cast<u8>(value >> 24);
  p[1] = static_cast<u8>(value >> 16);
  p[2] = static_cast<u8>(value >> 8);
  p[3] = static_cast<u8>(value);
}

u32 Load32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}

}  // namespace

NativeSdk::AudioState::~AudioState() = default;

namespace
{
constexpr u32 kAiRegisterBase = 0xCD006C00u;
// Power-on values (the chassis's AudioInterfaceManager::Init and
// DSPManager::Init): AICR with AISFR = 48 kHz and AIDFR = 32 kHz, the DSP
// halted.
constexpr u32 kAiControlPowerOn = (1u << 1) | (1u << 6);
constexpr u16 kDspControlPowerOn = 1u << 2;
constexpr u32 kAid32kHzDivisorWii = 1125 * 2 * 3 / 2;
}  // namespace

bool NativeSdk::AudioBind(const std::function<bool(u32)>& enable, bool wanted)
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_AUDIO");
  if (!wanted || (setting && (std::string(setting) == "off" || std::string(setting) == "0")))
    return false;
  m_audio = std::make_unique<AudioState>();
  const bool verify = m_verify || std::getenv("MODERNGEKKO_NATIVE_AUDIO_VERIFY") != nullptr;
  if (verify)
  {
    // Only observe the mails AX sends; the emulated DSP does the work.
    if (!enable(kDspSendMailToDsp))
    {
      m_audio.reset();
      return false;
    }
    m_audio->mode = AudioState::Mode::Verify;
    std::fprintf(stderr, "[native_audio] verify: emulated DSP runs, native mixer compared\n");
    return true;
  }
  // All or nothing: every hook must exist in the module.
  for (u32 address : kHooks)
  {
    if (!enable(address))
    {
      std::fprintf(stderr, "[native_audio] hook 0x%08X missing from the module: disabled\n",
                   address);
      m_audio.reset();
      return false;
    }
  }
  m_audio->mode = AudioState::Mode::Native;
  if (auto* clock = StaticRecompNativeClock::NativeClock::Instance())
    m_audio->dma_event = clock->Register("NativeAudioDMA", [this](u64, s64 late) { AudioDmaEvent(late); });
  std::fprintf(stderr, "[native_audio] native DSP (AX mixer) and audio DMA: %zu hooks\n",
               std::size(kHooks));
  return true;
}

bool NativeSdk::AudioActivate(const CPUState& guest)
{
  AudioState& a = *m_audio;
  bool code_matches = guest.gpr[13] == kSda;
  for (const auto& code : kAudioCode)
  {
    const u8* p = Pointer(guest, code.address, code.size);
    if (!p || Fnv1a(p, code.size) != code.fnv1a)
    {
      std::fprintf(stderr, "[native_audio] SDK code at 0x%08X differs from the original\n",
                   code.address);
      code_matches = false;
    }
  }
  if (!code_matches)
    return false;
  a.mixer = std::make_unique<StaticRecompNativeAudio::NativeAxWii>(StaticRecompGuestMemory::GuestMemory::Own());
  if (a.mode == AudioState::Mode::Native)
    AudioConfigureHost();
  a.active = true;
  return true;
}

void NativeSdk::AudioConfigureHost()
{
  AudioState& a = *m_audio;
  // Nothing emulated runs from here on: Dolphin's DSP and audio DMA stay idle.
  if (s_chassis.park_audio)
    s_chassis.park_audio();
  s_audio_native = true;
  // The audio DMA clock: 108 MHz / divisor samples per second (32 kHz on Wii).
  constexpr u64 kAudioClockHz = 54000000ull * 2;
  // The AI's 32 kHz divisor on Wii: 1125 * 2 (48 kHz) * 3 / 2.
  constexpr u32 divisor = kAid32kHzDivisorWii;
  if (HostAudioSink* sink = GetHostAudioSink())
    sink->SetInputSampleRate(static_cast<u32>((kAudioClockHz + divisor / 2) / divisor));
  // Same cadence as the hardware DMA: 32 bytes = 8 stereo samples.
  a.ticks_per_32_bytes = u64(StaticRecompNativeClock::NativeClock::CurrentTicksPerSecond()) * divisor /
                         (kAudioClockHz * 4 / 32);
}

void NativeSdk::AudioDoState(StaticRecompState::StateStream& p, u32 version)
{
  AudioState& a = *m_audio;
  u32 saved_mode = static_cast<u32>(a.mode);
  p.Do(saved_mode);
  p.Do(a.active);
  p.Do(a.abandoned);
  p.Do(a.dsp);
  p.Do(a.mails);
  p.Do(a.last_mail);
  p.Do(a.rom_parameter);
  p.Do(a.ucode_address);
  p.Do(a.ucode_length);
  p.Do(a.ucode_crc);
  p.Do(a.cmdlist_size);
  p.Do(a.pending_dsp_interrupts);
  p.Do(a.dma_hi);
  p.Do(a.dma_lo);
  p.Do(a.dma_control);
  p.Do(a.play_address);
  p.Do(a.play_blocks);
  p.Do(a.block_start);
  p.Do(a.dma_running);
  p.Do(a.pending_ai_interrupts);
  if (version >= 8)
  {
    p.Do(a.ai_regs_seeded);
    p.Do(a.ai_control);
    p.Do(a.ai_volume);
    p.Do(a.ai_interrupt_timing);
    p.Do(a.dsp_control_seeded);
    p.Do(a.dsp_control);
  }
  bool has_mixer = a.mixer != nullptr;
  p.Do(has_mixer);
  if (has_mixer)
  {
    if (!a.mixer)
      a.mixer = std::make_unique<StaticRecompNativeAudio::NativeAxWii>(StaticRecompGuestMemory::GuestMemory::Own());
    a.mixer->DoState(p);
  }
  if (!p.IsReadMode())
    return;
  const bool saved_native = saved_mode == static_cast<u32>(AudioState::Mode::Native) && a.active;
  if (a.mode == AudioState::Mode::Native)
  {
    if (saved_native)
    {
      AudioConfigureHost();
      // The clock's events are rebuilt on load: the block in flight ends at
      // the same cycle the saved event had.
      if (auto* clock = StaticRecompNativeClock::NativeClock::Instance(); clock && a.dma_event)
      {
        clock->Remove(a.dma_event);
        if (a.dma_running)
        {
          const s64 end = static_cast<s64>(a.block_start) +
                          static_cast<s64>(std::max<u32>(a.play_blocks, 1) * a.ticks_per_32_bytes);
          clock->Schedule(end - static_cast<s64>(clock->Now()), a.dma_event);
        }
      }
      std::fprintf(stderr, "[native_audio] state loaded: native audio continues (DMA %s)\n",
                   a.dma_running ? "running" : "stopped");
    }
    else
    {
      // The state's audio runs on the emulated DSP: keep it for this session.
      a.active = false;
      a.abandoned = true;
      std::fprintf(stderr, "[native_audio] state saved with emulated audio: original audio code runs\n");
    }
  }
  else if (saved_native)
  {
    std::fprintf(stderr,
                 "[native_audio] state needs native audio, which is not enabled: load refused\n");
    p.SetMeasureMode();
  }
}

void NativeSdk::AudioPostInterrupt(CallbackKind kind)
{
  AudioState& a = *m_audio;
  NativeCallback call{0, 0, 0};
  call.kind = kind;
  u32& pending = kind == CallbackKind::DspInterrupt ? a.pending_dsp_interrupts :
                                                      a.pending_ai_interrupts;
  if (m_last_guest && PostCallback(*m_last_guest, call))
  {
    ++(kind == CallbackKind::DspInterrupt ? a.dsp_interrupts : a.ai_interrupts);
    return;
  }
  // The OS is not taking interrupts yet: post at the next chance.
  ++pending;
}

void NativeSdk::AudioPushMail(u32 mail, bool interrupt)
{
  // CMailHandler::PushMail: an interrupt is raised now if the queue is empty,
  // otherwise when the mail before this one is read.
  AudioState& a = *m_audio;
  if (interrupt)
  {
    if (a.mails.empty())
    {
      if (a.mode == AudioState::Mode::Native)
        AudioPostInterrupt(CallbackKind::DspInterrupt);
    }
    else
    {
      a.mails.front().second = true;
    }
  }
  a.mails.emplace_back(mail, false);
}

void NativeSdk::AudioHandleMail(u32 mail)
{
  AudioState& a = *m_audio;
  const bool native = a.mode == AudioState::Mode::Native;
  switch (a.dsp)
  {
  case AudioState::Dsp::Halted:
    ++a.unknown_mails;
    break;
  case AudioState::Dsp::Rom:
    // ROMUCode::HandleMail: parameter pairs, then boot at 0x80F3D001.
    if (a.rom_parameter == 0)
    {
      if ((mail & 0xFFFF0000) != 0x80F30000)
        AudioPushMail(0xFEEE0000 | (mail & 0xFFFF), false);
      else
        a.rom_parameter = mail;
      break;
    }
    switch (a.rom_parameter)
    {
    case 0x80F3A001:
      a.ucode_address = mail;
      break;
    case 0x80F3A002:
      a.ucode_length = mail & 0xFFFF;
      break;
    case 0x80F3D001:
    {
      const u8* code = StaticRecompGuestMemory::GuestMemory::Own().GetPointerForRange(a.ucode_address, a.ucode_length);
      a.ucode_crc = code ? StaticRecompUtil::HashEctor(code, a.ucode_length) : 0;
      std::fprintf(stderr, "[native_audio] DSP task boots microcode crc=%08x (%s)\n", a.ucode_crc,
                   StaticRecompNativeAudio::NativeAxWii::SupportsCrc(a.ucode_crc) ?
                       "AXWii: native mixer" :
                       "UNSUPPORTED");
      a.rom_parameter = 0;
      a.dsp = AudioState::Dsp::WaitingCmdSize;
      // AXUCode::Initialize.
      AudioPushMail(kDspInitMail, true);
      return;
    }
    default:
      break;
    }
    a.rom_parameter = 0;
    break;
  case AudioState::Dsp::WaitingCmdSize:
    if ((mail & 0xFFFF0000) == kMailCmdList)
    {
      a.cmdlist_size = static_cast<u16>(mail & 0xFFFF);
      a.dsp = AudioState::Dsp::WaitingCmdAddr;
    }
    else
    {
      ++a.unknown_mails;
    }
    break;
  case AudioState::Dsp::WaitingCmdAddr:
  {
    ++a.command_lists;
    if (!native)
      a.mixer->SetOverlay(true);
    if (!a.mixer->ExecuteCommandList(mail, a.cmdlist_size))
    {
      if (++a.command_list_errors <= 4)
        std::fprintf(stderr, "[native_audio] command list %08x (%u words) not executed\n", mail,
                     a.cmdlist_size);
    }
    // OutputSamples sends DSP_SYNC; the end of the list sends DSP_YIELD.
    for (u32 i = 0; i < a.mixer->SyncMails(); ++i)
      AudioPushMail(kDspSync, true);
    AudioPushMail(kDspYield, true);
    a.cmdlist_size = 0;
    a.dsp = AudioState::Dsp::WaitingNextTask;
    if (!native)
      a.mails.clear();  // the emulated DSP answers; these were only bookkeeping
    break;
  }
  case AudioState::Dsp::WaitingNextTask:
    switch (0xCDD10000 | (mail & 0xFFFF))
    {
    case kMailResume:
      AudioPushMail(kDspResume, true);
      a.dsp = AudioState::Dsp::WaitingCmdSize;
      break;
    case kMailContinue:
      a.dsp = AudioState::Dsp::WaitingCmdSize;
      break;
    case kMailReset:
      // Back to the boot ROM (DSPHLE::SetUCode(UCODE_ROM)).
      a.dsp = AudioState::Dsp::Rom;
      a.rom_parameter = 0;
      AudioPushMail(kRomReady, false);
      break;
    case kMailNewUcode:
    default:
      // Task switching to another microcode: not used by this game.
      ++a.unknown_mails;
      a.dsp = AudioState::Dsp::WaitingCmdSize;
      break;
    }
    break;
  }
  if (!native)
    a.mails.clear();
}

bool NativeSdk::AudioRun(CPUState& guest)
{
  AudioState& a = *m_audio;
  if (a.abandoned || std::find(std::begin(kHooks), std::end(kHooks), guest.pc) == std::end(kHooks))
    return false;
  if (!a.active)
  {
    // Native audio replaces the hardware from boot: the first audio call must
    // be the OS initialising it. A state loaded mid-game keeps the original.
    if ((a.mode == AudioState::Mode::Native && guest.pc != kOsInitAudioSystem) ||
        !AudioActivate(guest))
    {
      std::fprintf(stderr,
                   "[native_audio] not activated (first call 0x%08X): original audio code runs\n",
                   guest.pc);
      a.abandoned = true;
      return false;
    }
    if (a.mode == AudioState::Mode::Verify)
    {
      // The emulated DSP is past DSPInit when the first mail is seen: the
      // boot parameters from boot, a command list from a saved state.
      a.dsp = (guest.gpr[3] & 0xFFFF0000) == kMailCmdList ? AudioState::Dsp::WaitingCmdSize :
                                                            AudioState::Dsp::Rom;
    }
  }

  if (a.mode == AudioState::Mode::Verify)
  {
    // Observe only: the emulated DSP handles the mail synchronously when the
    // original writes it; compare at the original's return.
    if (guest.pc == kDspSendMailToDsp)
    {
      const bool is_list = a.dsp == AudioState::Dsp::WaitingCmdAddr;
      AudioHandleMail(guest.gpr[3]);
      if (is_list)
      {
        m_audio_verify_lr = guest.lr;
        m_audio_verify_sp = guest.gpr[1];
      }
    }
    return false;
  }

  // Deliver interrupts that could not be posted earlier.
  for (; a.pending_dsp_interrupts && m_last_guest; --a.pending_dsp_interrupts)
  {
    if (!PostCallback(*m_last_guest, {0, 0, 0, 0, 0, CallbackKind::DspInterrupt}))
      break;
    ++a.dsp_interrupts;
  }
  for (; a.pending_ai_interrupts && m_last_guest; --a.pending_ai_interrupts)
  {
    if (!PostCallback(*m_last_guest, {0, 0, 0, 0, 0, CallbackKind::AiInterrupt}))
      break;
    ++a.ai_interrupts;
  }

  const auto write32 = [&](u32 address, u32 value) {
    if (u8* p = Pointer(guest, address, 4))
      Store32(p, value);
  };
  const auto read32 = [&](u32 address) {
    const u8* p = Pointer(guest, address, 4);
    return p ? Load32(p) : 0u;
  };
  auto* clock = StaticRecompNativeClock::NativeClock::Instance();
  switch (guest.pc)
  {
  case kOsInitAudioSystem:
    // The original runs a DSP initialisation program and programs the audio
    // clock; neither exists here. Its RAM save/restore leaves memory as it was.
    a.dsp = AudioState::Dsp::Halted;
    a.mails.clear();
    break;
  case kOsStopAudioSystem:
    if (a.dma_running && clock)
      clock->Remove(a.dma_event);
    a.dma_running = false;
    a.dma_control &= 0x7FFF;
    a.dsp = AudioState::Dsp::Halted;
    a.mails.clear();
    break;
  case kDspInit:
    if (read32(kDspInitFlag) != 1)
    {
      for (u32 address : kDspTaskGlobals)
        write32(address, 0);
      write32(kDspInitFlag, 1);
      // The reset DSP starts its boot ROM, which announces itself.
      a.dsp = AudioState::Dsp::Rom;
      a.rom_parameter = 0;
      a.mails.clear();
      AudioPushMail(kRomReady, false);
    }
    break;
  case kDspCheckMailToDsp:
    guest.gpr[3] = 0;  // the DSP takes every mail at once
    break;
  case kDspCheckMailFromDsp:
    guest.gpr[3] = a.mails.empty() ? 0 : 1;
    break;
  case kDspReadMailFromDsp:
    // CMailHandler: a pending mail reads back whole; afterwards (or with no
    // mail) the mailbox repeats the last one with its top bit cleared.
    if (!a.mails.empty())
    {
      const auto [mail, interrupt] = a.mails.front();
      a.mails.pop_front();
      guest.gpr[3] = mail;
      a.last_mail = mail & 0x7FFFFFFF;
      if (interrupt)
        AudioPostInterrupt(CallbackKind::DspInterrupt);
    }
    else
    {
      guest.gpr[3] = a.last_mail;
    }
    break;
  case kDspSendMailToDsp:
    AudioHandleMail(guest.gpr[3]);
    guest.downcount -= kCommandListCycles;
    break;
  case kAiInit:
    if (read32(kAiInitFlag) != 1)
    {
      // The original measures the audio clock (__AI_SRC_INIT) with constants
      // derived from the bus clock; they are kept, the measurement is not.
      const u32 r10 = u32((u64(0x431BDE83) * (read32(kBusClock) >> 2)) >> 32) >> 15;
      const auto scaled = [&](u32 factor) {
        return u32((u64(0x10624DD3) * u32(r10 * factor)) >> 32) >> 9;
      };
      write32(kSda - 0x62F4, scaled(0x7B24));
      write32(kSda - 0x62FC, scaled(0xA428));
      write32(kSda - 0x6304, scaled(0xA410));
      write32(kSda - 0x630C, scaled(0xF618));
      write32(kSda - 0x6314, scaled(0xBB8));
      for (u32 offset : {0x62F8u, 0x6300u, 0x6308u, 0x6310u, 0x6318u})
        write32(kSda - offset, 0);
      write32(kAiCallback, 0);
      write32(kAiCallbackStack, guest.gpr[3]);
      write32(kAiInitFlag, 1);
    }
    break;
  case kAiInitDma:
  {
    const u32 address = guest.gpr[3], length = guest.gpr[4];
    a.dma_hi = static_cast<u16>((a.dma_hi & 0xE000) | (address >> 16));
    a.dma_lo = static_cast<u16>((a.dma_lo & 0x001F) | (address & 0xFFFF));
    a.dma_control = static_cast<u16>((a.dma_control & 0x8000) | ((length >> 5) & 0x7FFF));
    break;
  }
  case kAiStartDma:
    a.dma_control |= 0x8000;
    if (!a.dma_running)
      AudioStartDma();
    break;
  case kAiGetDmaBytesLeft:
  {
    u32 left = 0;
    if (a.dma_running && a.ticks_per_32_bytes)
    {
      const u64 done = clock ? (clock->Now() - a.block_start) / a.ticks_per_32_bytes : 0;
      left = done < a.play_blocks ? (a.play_blocks - static_cast<u32>(done)) * 32 : 0;
    }
    guest.gpr[3] = left;
    break;
  }
  case kAiGetDmaStartAddr:
    guest.gpr[3] = (a.dma_lo & 0xFFE0u) | ((a.dma_hi & 0x1FFFu) << 16);
    break;
  case kAiGetDspSampleRate:
    guest.gpr[3] = 0;  // AI_SAMPLERATE_32KHZ (AIInit leaves the DSP at 32 kHz)
    break;
  default:
    return false;
  }
  guest.pc = guest.lr;
  guest.downcount -= kAudioHookCycles;
  return true;
}

void NativeSdk::AudioStartDma()
{
  AudioState& a = *m_audio;
  // As the hardware does when the enable bit is set: latch the registers.
  a.play_address = ((u32(a.dma_hi) << 16) | a.dma_lo) & ~0x1Fu;
  a.play_blocks = a.dma_control & 0x7FFF;
  auto* clock = StaticRecompNativeClock::NativeClock::Instance();
  a.block_start = clock ? clock->Now() : 0;
  a.dma_running = true;
  if (clock)
    clock->Schedule(static_cast<s64>(std::max<u32>(a.play_blocks, 1) * a.ticks_per_32_bytes),
                    a.dma_event);
}

void NativeSdk::AudioDmaEvent(s64 cycles_late)
{
  if (!m_audio || !m_audio->dma_running)
    return;
  AudioState& a = *m_audio;
  // The block just played goes to the host mixer (big-endian stereo s16).
  if (a.play_blocks)
  {
    const auto memory = StaticRecompGuestMemory::GuestMemory::Own();
    if (const u8* samples = memory.GetPointerForRange(a.play_address, a.play_blocks * 32))
    {
      // Our own output (shared mode) when the runtime registered one; muted otherwise.
      if (HostAudioSink* sink = GetHostAudioSink())
        sink->PushSamples(samples, a.play_blocks * 8);
      a.samples_out += a.play_blocks * 8;
      static const u64 hash_limit = [] {
        const char* value = std::getenv("MODERNGEKKO_AUDIO_HASH_SAMPLES");
        return value ? std::strtoull(value, nullptr, 10) : 640000ull;
      }();
      for (u32 i = 0; i < a.play_blocks * 8 && a.output_hash_samples < hash_limit; ++i)
      {
        for (u32 b = 0; b < 4; ++b)
          a.output_hash = (a.output_hash ^ samples[i * 4 + b]) * 0x100000001b3ull;
        ++a.output_hash_samples;
      }
    }
  }
  ++a.dma_blocks;
  if (!(a.dma_control & 0x8000))
  {
    a.dma_running = false;
    return;
  }
  // Reload from the registers (set by the callback for this next block) and
  // interrupt, as the hardware does at the end of a block.
  a.play_address = ((u32(a.dma_hi) << 16) | a.dma_lo) & ~0x1Fu;
  a.play_blocks = a.dma_control & 0x7FFF;
  auto* clock = StaticRecompNativeClock::NativeClock::Instance();
  a.block_start = (clock ? clock->Now() : 0) - cycles_late;
  if (clock)
    clock->Schedule(static_cast<s64>(std::max<u32>(a.play_blocks, 1) * a.ticks_per_32_bytes) -
                        cycles_late,
                    a.dma_event);
  AudioPostInterrupt(CallbackKind::AiInterrupt);
}

void NativeSdk::AudioFinishVerify()
{
  AudioState& a = *m_audio;
  m_audio_verify_lr = 0;
  u32 first = 0;
  const u32 differences = a.mixer->CompareOverlay(&first);
  if (differences == 0)
  {
    ++a.verified;
  }
  else if (++a.mismatches <= 8)
  {
    std::fprintf(stderr,
                 "[native_audio] verify: command list %llu differs in %u bytes, first at %08x\n",
                 static_cast<unsigned long long>(a.command_lists), differences, first);
  }
  a.mixer->ClearOverlay();
}

bool NativeSdk::AudioEnterHandler(CPUState& guest, CallbackKind kind)
{
  return EnterInterruptHandler(guest, kHandlerFrame,
                               kind == CallbackKind::DspInterrupt ? kDspHandlerResume :
                                                                    kAiHandlerResume);
}

bool NativeSdk::EnterInterruptHandler(CPUState& guest, u32 frame_size, u32 resume)
{
  // Prologue of __DSPHandler / __AIDHandler:
  //   stwu r1,-size(r1); mflr r0; stw r0,size+4(r1); stw r31,size-4(r1);
  //   mr r31,r4; addi r3,r1,8
  const u32 old_sp = guest.gpr[1];
  const u32 new_sp = old_sp - frame_size;
  u8* frame = Pointer(guest, new_sp, 4);
  u8* link = Pointer(guest, old_sp + 4, 4);
  u8* saved = Pointer(guest, new_sp + frame_size - 4, 4);
  if (!frame || !link || !saved)
    return false;
  Store32(frame, old_sp);
  Store32(link, guest.lr);
  Store32(saved, guest.gpr[31]);
  guest.gpr[1] = new_sp;
  guest.gpr[31] = guest.gpr[4];
  guest.gpr[3] = new_sp + 8;
  guest.pc = resume;
  return true;
}

void NativeSdk::PrintAudioStats() const
{
  if (!m_audio)
    return;
  const AudioState& a = *m_audio;
  std::fprintf(stderr,
               "[native_audio] mode=%s active=%d abandoned=%d ucode=%08x command_lists=%llu "
               "errors=%llu dsp_interrupts=%llu ai_interrupts=%llu dma_blocks=%llu "
               "samples_out=%llu unknown_mails=%llu pending=%u/%u verified=%llu mismatches=%llu output_hash=%016llx/%llu\n",
               a.mode == AudioState::Mode::Native ? "native" :
               a.mode == AudioState::Mode::Verify ? "verify" :
                                                    "off",
               a.active, a.abandoned, a.ucode_crc,
               static_cast<unsigned long long>(a.command_lists),
               static_cast<unsigned long long>(a.command_list_errors),
               static_cast<unsigned long long>(a.dsp_interrupts),
               static_cast<unsigned long long>(a.ai_interrupts),
               static_cast<unsigned long long>(a.dma_blocks),
               static_cast<unsigned long long>(a.samples_out),
               static_cast<unsigned long long>(a.unknown_mails), a.pending_dsp_interrupts,
               a.pending_ai_interrupts, static_cast<unsigned long long>(a.verified),
               static_cast<unsigned long long>(a.mismatches),
               static_cast<unsigned long long>(a.output_hash),
               static_cast<unsigned long long>(a.output_hash_samples));
}

bool NativeSdk::AiRegisterRead(u32 ea, u8 size, u64* value)
{
  if (!m_audio)
    return false;
  AudioState& a = *m_audio;
  if (a.mode != AudioState::Mode::Native || size != 4 || (ea & 0xFFFFFFF0u) != kAiRegisterBase)
    return false;
  if (!a.ai_regs_seeded)
  {
    if (!s_chassis.ai_seed || !s_chassis.ai_seed(a.ai_control, a.ai_volume, a.ai_interrupt_timing))
    {
      a.ai_control = kAiControlPowerOn;
      a.ai_volume = 0;
      a.ai_interrupt_timing = 0;
    }
    a.ai_regs_seeded = true;
  }
  switch (ea & 0xF)
  {
  case 0x0:
    *value = a.ai_control;
    return true;
  case 0x4:
    *value = a.ai_volume;
    return true;
  case 0x8:
    // The AIS sample counter: disc audio streaming is not used by the game
    // (PSTAT never set); it reads as the stopped counter.
    *value = 0;
    return true;
  case 0xC:
    *value = a.ai_interrupt_timing;
    return true;
  }
  return false;
}

bool NativeSdk::AiRegisterWrite(u32 ea, u8 size, u64 value)
{
  if (!m_audio)
    return false;
  AudioState& a = *m_audio;
  if (a.mode != AudioState::Mode::Native || size != 4 || (ea & 0xFFFFFFF0u) != kAiRegisterBase)
    return false;
  if (!a.ai_regs_seeded)
  {
    if (!s_chassis.ai_seed || !s_chassis.ai_seed(a.ai_control, a.ai_volume, a.ai_interrupt_timing))
    {
      a.ai_control = kAiControlPowerOn;
      a.ai_volume = 0;
      a.ai_interrupt_timing = 0;
    }
    a.ai_regs_seeded = true;
  }
  const u32 word = static_cast<u32>(value);
  switch (ea & 0xF)
  {
  case 0x0:
  {
    // AICR: PSTAT, AISFR, AIINTMSK, AIINTVLD and AIDFR are state; writing 1
    // to AIINT (bit 3) clears it; SCRESET (bit 5) is a pulse.
    const u32 keep = 0x01u | 0x02u | 0x04u | 0x10u | 0x40u;
    u32 control = (a.ai_control & ~keep) | (word & keep);
    if (word & 0x08u)
      control &= ~0x08u;
    control &= ~0x20u;
    a.ai_control = control;
    return true;
  }
  case 0x4:
    a.ai_volume = word & 0xFFFFu;
    return true;
  case 0x8:
    return true;  // read-only counter
  case 0xC:
    a.ai_interrupt_timing = word;
    return true;
  }
  return false;
}
namespace
{
constexpr u32 kDspControl = 0xCC00500Au;
}  // namespace

bool NativeSdk::DspControlRead(u32 ea, u8 size, u64* value)
{
  if (!m_audio)
    return false;
  AudioState& a = *m_audio;
  if (a.mode != AudioState::Mode::Native || size != 2 || ea != kDspControl)
    return false;
  if (!a.dsp_control_seeded)
  {
    u16 control = 0;
    a.dsp_control = s_chassis.dsp_control_seed && s_chassis.dsp_control_seed(control) ? control :
                                                                                        kDspControlPowerOn;
    a.dsp_control_seeded = true;
  }
  *value = a.dsp_control;
  return true;
}

bool NativeSdk::DspControlWrite(u32 ea, u8 size, u64 value)
{
  if (!m_audio)
    return false;
  AudioState& a = *m_audio;
  if (a.mode != AudioState::Mode::Native || size != 2 || ea != kDspControl)
    return false;
  if (!a.dsp_control_seeded)
  {
    u16 control = 0;
    a.dsp_control = s_chassis.dsp_control_seed && s_chassis.dsp_control_seed(control) ? control :
                                                                                        kDspControlPowerOn;
    a.dsp_control_seeded = true;
  }
  // As the chassis: the interrupt flags (AID 0x08, ARAM 0x20, DSP 0x80) clear
  // when written with 1, DMAState (0x200) is read-only, the reset (0x01) is a
  // pulse the DSP (native) completes at once; the rest is state.
  const u16 word = static_cast<u16>(value);
  constexpr u16 kInts = 0x08 | 0x20 | 0x80;
  constexpr u16 kReadOnly = 0x200;
  u16 control = a.dsp_control;
  control = (control & (kInts | kReadOnly)) | (word & ~(kInts | kReadOnly | 0x01));
  control &= ~(word & kInts);
  a.dsp_control = control;
  return true;
}
}  // namespace StaticRecompNativeSdk
