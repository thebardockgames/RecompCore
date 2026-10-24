// RecompCore: StaticRecomp CPU core - the port's end-of-frame event (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompFrameEvents.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace StaticRecompFrameEvents
{
namespace
{
std::mutex s_mutex;
// Called in place: listeners keep state between frames (mutable lambdas).
std::vector<std::pair<int, std::shared_ptr<std::function<void()>>>> s_listeners;
int s_next_id = 1;
thread_local int s_suppress = 0;
std::atomic<unsigned long long> s_own{0}, s_forwarded{0}, s_echoes{0};

void Unregister(int id)
{
  std::lock_guard lock(s_mutex);
  std::erase_if(s_listeners, [id](const auto& entry) { return entry.first == id; });
}

void Dispatch()
{
  std::vector<std::shared_ptr<std::function<void()>>> listeners;
  {
    std::lock_guard lock(s_mutex);
    listeners.reserve(s_listeners.size());
    for (const auto& [id, listener] : s_listeners)
      listeners.push_back(listener);
  }
  for (const auto& listener : listeners)
    (*listener)();
}
}  // namespace

Hook& Hook::operator=(Hook&& other) noexcept
{
  if (this != &other)
  {
    reset();
    m_id = other.m_id;
    other.m_id = -1;
  }
  return *this;
}

Hook::~Hook()
{
  reset();
}

void Hook::reset()
{
  if (m_id >= 0)
    Unregister(m_id);
  m_id = -1;
}

Hook Register(std::function<void()> listener)
{
  std::lock_guard lock(s_mutex);
  const int id = s_next_id++;
  s_listeners.emplace_back(id, std::make_shared<std::function<void()>>(std::move(listener)));
  return Hook(id);
}

void Fire()
{
  ++s_own;
  Dispatch();
}

void FireFromChassis()
{
  if (s_suppress > 0)
  {
    ++s_echoes;
    return;
  }
  ++s_forwarded;
  Dispatch();
}

SuppressChassisEcho::SuppressChassisEcho()
{
  ++s_suppress;
}

SuppressChassisEcho::~SuppressChassisEcho()
{
  --s_suppress;
}

Stats GetStats()
{
  return Stats{s_own.load(), s_forwarded.load(), s_echoes.load()};
}
}  // namespace StaticRecompFrameEvents
