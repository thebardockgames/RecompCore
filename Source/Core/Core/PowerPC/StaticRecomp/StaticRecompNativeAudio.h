// RecompCore: StaticRecomp CPU core - native audio state.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal to StaticRecompNativeSdk.cpp / StaticRecompNativeAudio.cpp.

#pragma once

#include <deque>
#include <memory>
#include <utility>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeAx.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

namespace StaticRecompNativeClock
{
struct EventType;
}

namespace StaticRecompNativeSdk
{
struct NativeSdk::AudioState
{
  ~AudioState();

  enum class Mode
  {
    Off,     // not enabled (or abandoned before activation)
    Native,  // hooks replace the DSP and audio DMA hardware
    Verify,  // original (emulated) path; the mixer runs alongside and is compared
  };
  enum class Dsp
  {
    Halted,           // before DSPInit
    Rom,              // boot ROM: waiting for the microcode parameters
    WaitingCmdSize,   // AX microcode: 0xBABE | size
    WaitingCmdAddr,   // AX microcode: command list address
    WaitingNextTask,  // after DSP_YIELD: CONTINUE / RESUME / RESET
  };

  Mode mode = Mode::Off;
  bool active = false;     // first hook seen and checks passed
  bool abandoned = false;  // checks failed: the original code runs
  std::unique_ptr<StaticRecompNativeAudio::NativeAxWii> mixer;

  // DSP: mail queue to the CPU (mail, raise an interrupt when it is read),
  // the semantics of Dolphin's CMailHandler.
  Dsp dsp = Dsp::Halted;
  std::deque<std::pair<u32, bool>> mails;
  u32 last_mail = 0;
  u32 rom_parameter = 0;
  u32 ucode_address = 0, ucode_length = 0, ucode_crc = 0;
  u16 cmdlist_size = 0;
  u32 pending_dsp_interrupts = 0;  // could not be posted yet

  // Audio interface DMA registers (as the SDK writes them) and the block
  // being played.
  u16 dma_hi = 0, dma_lo = 0, dma_control = 0;
  u32 play_address = 0, play_blocks = 0;
  u64 block_start = 0;     // clock cycles at the start of the block
  u64 ticks_per_32_bytes = 0;
  bool dma_running = false;
  StaticRecompNativeClock::EventType* dma_event = nullptr;
  u32 pending_ai_interrupts = 0;

  // The AI register page as the game sees it (seeded from the chassis once).
  bool ai_regs_seeded = false;
  u32 ai_control = 0, ai_volume = 0, ai_interrupt_timing = 0;
  bool dsp_control_seeded = false;
  u16 dsp_control = 0;

  // Statistics.
  u64 command_lists = 0, command_list_errors = 0, dsp_interrupts = 0, ai_interrupts = 0;
  u64 dma_blocks = 0, samples_out = 0, unknown_mails = 0;
  // FNV-1a of the first samples sent to the output (identity check of the
  // mixer; MODERNGEKKO_AUDIO_HASH_SAMPLES, default 640000 = 20 s at 32 kHz,
  // so runs of different lengths compare).
  u64 output_hash = 0xcbf29ce484222325ull;
  u64 output_hash_samples = 0;
  u64 verified = 0, mismatches = 0;
};
}  // namespace StaticRecompNativeSdk
