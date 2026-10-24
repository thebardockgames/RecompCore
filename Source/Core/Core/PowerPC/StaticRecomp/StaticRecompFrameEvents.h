// RecompCore: StaticRecomp CPU core - the port's end-of-frame event (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// "A game frame ended" (an XFB copy, or a checkpoint load, which counts as
// the end of the frame it was saved in). The runtime's frame clock, input
// scripts, captures and metrics listen here. Sources: the native GX host
// (NativeGX owns the GPU) and, while the chassis renders or loads its own
// states, the chassis's after_frame_event forwarded by its adapter. While the
// port itself fires, the chassis's echo of the same frame is not forwarded.

#pragma once

#include <functional>

namespace StaticRecompFrameEvents
{
// Unregisters on destruction (movable, like the chassis's EventHook).
class Hook
{
public:
  Hook() = default;
  explicit Hook(int id) : m_id(id) {}
  Hook(Hook&& other) noexcept : m_id(other.m_id) { other.m_id = -1; }
  Hook& operator=(Hook&& other) noexcept;
  Hook(const Hook&) = delete;
  Hook& operator=(const Hook&) = delete;
  ~Hook();
  void reset();

private:
  int m_id = -1;
};

[[nodiscard]] Hook Register(std::function<void()> listener);

// The port's own source.
void Fire();
// The chassis's event, forwarded unless the port is firing the same frame.
void FireFromChassis();

// While alive, the chassis's echo is not forwarded (the port fired already).
class SuppressChassisEcho
{
public:
  SuppressChassisEcho();
  ~SuppressChassisEcho();
  SuppressChassisEcho(const SuppressChassisEcho&) = delete;
  SuppressChassisEcho& operator=(const SuppressChassisEcho&) = delete;
};

struct Stats
{
  unsigned long long own = 0, forwarded = 0, echoes = 0;
};
Stats GetStats();
}  // namespace StaticRecompFrameEvents
