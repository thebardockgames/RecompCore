// RecompCore: StaticRecomp CPU core - host controller provider registry.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompHostInput.h"

#include <atomic>

namespace StaticRecompNativeSdk
{
namespace
{
std::atomic<HostGamePad*> g_host_pad{nullptr};
}

void SetHostGamePad(HostGamePad* pad)
{
  g_host_pad.store(pad, std::memory_order_release);
}

HostGamePad* GetHostGamePad()
{
  return g_host_pad.load(std::memory_order_acquire);
}
}  // namespace StaticRecompNativeSdk
