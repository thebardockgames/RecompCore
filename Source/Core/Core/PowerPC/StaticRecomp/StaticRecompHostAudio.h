// RecompCore: StaticRecomp CPU core - host audio output interface.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The native audio DMA (StaticRecompNativeAudio.cpp) hands each finished block
// to this sink instead of Dolphin's AudioCommon mixer. The runtime registers
// one provider (its own shared-mode WASAPI output) or none, which mutes the
// game. Depends on nothing from Dolphin (docs/NATIVE_PORT_ROADMAP.md, H1).

#pragma once

#include <cstddef>
#include <cstdint>

namespace StaticRecompNativeSdk
{
class HostAudioSink
{
public:
  virtual ~HostAudioSink() = default;
  // Rate of the samples that follow (the audio DMA clock: 32 kHz on Wii).
  virtual void SetInputSampleRate(std::uint32_t hz) = 0;
  // Stereo s16 frames as they sit in guest memory: big-endian, left then right.
  virtual void PushSamples(const std::uint8_t* big_endian_stereo, std::size_t frames) = 0;
};

void SetHostAudioSink(HostAudioSink* sink);
HostAudioSink* GetHostAudioSink();
}  // namespace StaticRecompNativeSdk
