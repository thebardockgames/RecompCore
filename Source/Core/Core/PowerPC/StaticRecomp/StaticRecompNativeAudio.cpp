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
//    native. A CoreTiming event at the end of each block sends the block to
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

#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Common/ChunkFile.h"
#include "Common/Hash.h"
#include "Core/CoreTiming.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

namespace StaticRecompNativeSdk
{
namespace
{
constexpr u32 kSda = 0x806370A0;  // r13

// dsp.a
constexpr u32 kDspCheckMailToDsp = 0x80228A58;
constexpr u32 kDspCheckMailFromDsp = 0x80228A68;
constexpr u32 kDspReadMailFromDsp = 0x80228A78;
constexpr u32 kDspSendMailToDsp = 0x80228A8C;
constexpr u32 kDspInit = 0x80228AA0;
constexpr u32 kDspHandlerResume = 0x80228D14;  // __DSPHandler after the DSPCR acknowledgement
constexpr u32 kDspInitFlag = kSda - 0x6190;
constexpr u32 kDspTaskGlobals[] = {kSda - 0x6180, kSda - 0x6174, kSda - 0x617C, kSda - 0x6178};
// ai.a
constexpr u32 kAiInit = 0x8022089C;
constexpr u32 kAiInitDma = 0x802207C8;
constexpr u32 kAiStartDma = 0x80220844;
constexpr u32 kAiGetDmaBytesLeft = 0x80220858;
constexpr u32 kAiGetDmaStartAddr = 0x80220868;
constexpr u32 kAiGetDspSampleRate = 0x80220888;
constexpr u32 kAiHandlerResume = 0x80220A4C;  // __AIDHandler after the DSPCR acknowledgement
constexpr u32 kAiInitFlag = kSda - 0x6320;
constexpr u32 kAiCallback = kSda - 0x62E8;
constexpr u32 kAiCallbackStack = kSda - 0x62EC;
// OS
constexpr u32 kOsInitAudioSystem = 0x80207B30;
constexpr u32 kOsStopAudioSystem = 0x80207CF8;
constexpr u32 kBusClock = 0x800000F8;
// Both SDK interrupt handlers open with stwu r1,-0x2E0(r1).
constexpr u32 kHandlerFrame = 0x2E0;

constexpr u32 kHooks[] = {kOsInitAudioSystem, kOsStopAudioSystem, kAiInitDma,
                          kAiStartDma,        kAiGetDmaBytesLeft, kAiGetDmaStartAddr,
                          kAiGetDspSampleRate, kAiInit,           kDspCheckMailToDsp,
                          kDspCheckMailFromDsp, kDspReadMailFromDsp, kDspSendMailToDsp,
                          kDspInit};

// The replaced functions and the SDK code the replacement relies on.
constexpr std::array<CodeFingerprint, 19> kAudioCode = {{
    {0x80228A58u, 0x10u, 0xC27236368ABFDBBAull},   // DSPCheckMailToDSP
    {0x80228A68u, 0x10u, 0x742444C7E62E105Eull},   // DSPCheckMailFromDSP
    {0x80228A78u, 0x14u, 0xE18609E1983444DEull},   // DSPReadMailFromDSP
    {0x80228A8Cu, 0x14u, 0x3346D4294872614Full},   // DSPSendMailToDSP
    {0x80228AA0u, 0xC0u, 0xB5F6B3343A34D1E8ull},   // DSPInit
    {0x80228B68u, 0x70u, 0xBEF505A1EC4396C3ull},   // DSPAddTask
    {0x80228CE4u, 0x434u, 0x1F486721CB15192Bull},  // __DSPHandler
    {0x802292BCu, 0x18Cu, 0x13924A6231AEACDFull},  // __DSP_boot_task
    {0x80229448u, 0x9Cu, 0x7898FA0DA40DF7DDull},   // __DSP_insert_task
    {0x80220784u, 0x44u, 0xDFC616B599D0C3BDull},   // AIRegisterDMACallback
    {0x802207C8u, 0x7Cu, 0x198E5D4F118EF335ull},   // AIInitDMA
    {0x80220844u, 0x14u, 0xA8A5BBE315AEECEBull},   // AIStartDMA
    {0x80220858u, 0x10u, 0xF608C1142EDF3E0Aull},   // AIGetDMABytesLeft
    {0x80220868u, 0x18u, 0x10C4B245F256A0ABull},   // AIGetDMAStartAddr
    {0x80220888u, 0x14u, 0x2510F640197BE948ull},   // AIGetDSPSampleRate
    {0x8022089Cu, 0x180u, 0x530A0669A6857648ull},  // AIInit
    {0x80220A1Cu, 0xA8u, 0xAF0D8ED7A1055CA2ull},   // __AIDHandler
    {0x80207B30u, 0x1C8u, 0x2B39EB215298DF34ull},  // __OSInitAudioSystem
    {0x80207CF8u, 0xCCu, 0x4CB25AFE0D455DDBull},   // __OSStopAudioSystem
}};

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

NativeSdk* s_audio_owner = nullptr;

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

void DmaCallback(Core::System&, u64, s64 cycles_late)
{
  if (s_audio_owner)
    s_audio_owner->AudioDmaEvent(cycles_late);
}
}  // namespace

NativeSdk::AudioState::~AudioState()
{
  if (mode == Mode::Native)
    s_audio_owner = nullptr;
}

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
  s_audio_owner = this;
  m_audio->dma_event = m_system.GetCoreTiming().RegisterEvent("NativeAudioDMA", DmaCallback);
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
  a.mixer = std::make_unique<StaticRecompNativeAudio::NativeAxWii>(m_system.GetMemory());
  if (a.mode == AudioState::Mode::Native)
    AudioConfigureHost();
  a.active = true;
  return true;
}

void NativeSdk::AudioConfigureHost()
{
  AudioState& a = *m_audio;
  // Nothing emulated runs from here on: Dolphin's DSP and audio DMA stay idle.
  m_system.GetDSP().SetNativeAudio(true);
  const u32 divisor = m_system.GetAudioInterface().Get32KHzSampleRateDivisor();
  if (SoundStream* stream = m_system.GetSoundStream())
    stream->GetMixer()->SetDMAInputSampleRateDivisor(divisor);
  // Same rate as Dolphin's audio DMA: 32 bytes = 8 stereo samples at 32 kHz.
  a.ticks_per_32_bytes = u64(m_system.GetSystemTimers().GetTicksPerSecond()) * divisor /
                         (Mixer::FIXED_SAMPLE_RATE_DIVIDEND * 4 / 32);
}

void NativeSdk::AudioDoState(PointerWrap& p)
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
  bool has_mixer = a.mixer != nullptr;
  p.Do(has_mixer);
  if (has_mixer)
  {
    if (!a.mixer)
      a.mixer = std::make_unique<StaticRecompNativeAudio::NativeAxWii>(m_system.GetMemory());
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
      const u8* code = m_system.GetMemory().GetPointerForRange(a.ucode_address, a.ucode_length);
      a.ucode_crc = code ? Common::HashEctor(code, a.ucode_length) : 0;
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
  auto& core_timing = m_system.GetCoreTiming();
  switch (guest.pc)
  {
  case kOsInitAudioSystem:
    // The original runs a DSP initialisation program and programs the audio
    // clock; neither exists here. Its RAM save/restore leaves memory as it was.
    a.dsp = AudioState::Dsp::Halted;
    a.mails.clear();
    break;
  case kOsStopAudioSystem:
    if (a.dma_running)
      core_timing.RemoveEvent(a.dma_event);
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
      const u64 done = (core_timing.GetTicks() - a.block_start) / a.ticks_per_32_bytes;
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
  a.block_start = m_system.GetCoreTiming().GetTicks();
  a.dma_running = true;
  m_system.GetCoreTiming().ScheduleEvent(
      static_cast<s64>(std::max<u32>(a.play_blocks, 1) * a.ticks_per_32_bytes), a.dma_event);
}

void NativeSdk::AudioDmaEvent(s64 cycles_late)
{
  if (!m_audio || !m_audio->dma_running)
    return;
  AudioState& a = *m_audio;
  // The block just played goes to the host mixer (big-endian stereo s16).
  if (a.play_blocks)
  {
    auto& memory = m_system.GetMemory();
    if (const u8* samples = memory.GetPointerForRange(a.play_address, a.play_blocks * 32))
    {
      AudioCommon::SendAIBuffer(m_system, reinterpret_cast<const short*>(samples),
                                a.play_blocks * 8);
      a.samples_out += a.play_blocks * 8;
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
  a.block_start = m_system.GetCoreTiming().GetTicks() - cycles_late;
  m_system.GetCoreTiming().ScheduleEvent(
      static_cast<s64>(std::max<u32>(a.play_blocks, 1) * a.ticks_per_32_bytes) - cycles_late,
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
               "samples_out=%llu unknown_mails=%llu pending=%u/%u verified=%llu mismatches=%llu\n",
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
               static_cast<unsigned long long>(a.mismatches));
}
}  // namespace StaticRecompNativeSdk
