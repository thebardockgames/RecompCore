// RecompCore: StaticRecomp CPU core - host audio sink registry.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompHostAudio.h"

#include <atomic>

namespace StaticRecompNativeSdk
{
namespace
{
std::atomic<HostAudioSink*> g_host_audio{nullptr};
}

void SetHostAudioSink(HostAudioSink* sink)
{
  g_host_audio.store(sink, std::memory_order_release);
}

HostAudioSink* GetHostAudioSink()
{
  return g_host_audio.load(std::memory_order_acquire);
}
}  // namespace StaticRecompNativeSdk
