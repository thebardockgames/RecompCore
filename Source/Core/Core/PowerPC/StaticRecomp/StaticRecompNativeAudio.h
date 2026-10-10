// RecompCore: StaticRecomp CPU core - native audio state.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal to StaticRecompNativeSdk.cpp / StaticRecompNativeAudio.cpp.

#pragma once

#include <deque>
#include <memory>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeAx.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

namespace CoreTiming
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
  u64 block_start = 0;     // CoreTiming ticks at the start of the block
  u64 ticks_per_32_bytes = 0;
  bool dma_running = false;
  CoreTiming::EventType* dma_event = nullptr;
  u32 pending_ai_interrupts = 0;

  // Statistics.
  u64 command_lists = 0, command_list_errors = 0, dsp_interrupts = 0, ai_interrupts = 0;
  u64 dma_blocks = 0, samples_out = 0, unknown_mails = 0;
  u64 verified = 0, mismatches = 0;
};
}  // namespace StaticRecompNativeSdk
