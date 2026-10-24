// RecompCore: StaticRecomp CPU core - the port's emulation thread (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompHostLoop.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeBoot.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGx.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace StaticRecompHostLoop
{
namespace
{
Stats s_stats;
bool s_chassis_states_used = false;

std::atomic<RunState> s_state{RunState::Uninitialized};
std::mutex s_listeners_mutex;
std::vector<std::pair<int, std::function<void(RunState)>>> s_listeners;
int s_next_listener = 1;
std::function<void(RunState)> s_mirror;
ThreadHooks s_thread_hooks;

std::mutex s_jobs_mutex;
std::condition_variable s_jobs_done;
std::deque<std::function<void()>> s_jobs;
unsigned long long s_jobs_submitted = 0, s_jobs_finished = 0;
bool s_loop_alive = false;

void RunPendingJobs()
{
  for (;;)
  {
    std::function<void()> job;
    {
      std::lock_guard lock(s_jobs_mutex);
      if (s_jobs.empty())
        return;
      job = std::move(s_jobs.front());
      s_jobs.pop_front();
    }
    job();
    ++s_stats.jobs;
    {
      std::lock_guard lock(s_jobs_mutex);
      ++s_jobs_finished;
    }
    s_jobs_done.notify_all();
  }
}
}  // namespace

void SetStateMirror(std::function<void(RunState)> mirror)
{
  s_mirror = std::move(mirror);
}

void SetThreadHooks(ThreadHooks hooks)
{
  s_thread_hooks = std::move(hooks);
}

void SetState(RunState state)
{
  s_state.store(state);
  if (s_mirror)
    s_mirror(state);
  std::vector<std::function<void(RunState)>> listeners;
  {
    std::lock_guard lock(s_listeners_mutex);
    for (const auto& [id, listener] : s_listeners)
      listeners.push_back(listener);
  }
  for (const auto& listener : listeners)
    listener(state);
}

// The CPU loop: `hooks.state` is the shared run flag (running / paused /
// off); `hooks.run` runs the core until that flag changes.
void CpuLoop(const CpuLoopHooks& hooks)
{
  {
    std::lock_guard lock(s_jobs_mutex);
    s_loop_alive = true;
  }
  for (;;)
  {
    ++s_stats.loops;
    const CpuRun state = hooks.state();
    if (state == CpuRun::Off)
      break;
    RunPendingJobs();
    if (state == CpuRun::Running)
    {
      hooks.run();
      continue;
    }
    ++s_stats.pauses;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  {
    std::lock_guard lock(s_jobs_mutex);
    s_loop_alive = false;
  }
  RunPendingJobs();  // nothing may stay queued behind a thread that is gone
  s_jobs_done.notify_all();
}

bool SkipsDevice(const char* device)
{
  // Port default (phase S step 25): every device the native layer replaces is
  // left uninitialised; MODERNGEKKO_HW_SKIP=<list> overrides, =none keeps all.
  // The chassis's GPFifo only goes when the native renderer owns the GPU (the
  // chassis's renderer reads its FIFO); its IOS kernel and IPC go together
  // (the kernel schedules IPC events).
  static const std::string skip = [] {
    if (const char* v = std::getenv("MODERNGEKKO_HW_SKIP"))
      return "," + std::string(v) + ",";
    if (s_chassis_states_used)
      return std::string();
    // Still needed: State (saving a chassis .sav, the oracle's format). The
    // IOS kernel and IPC go with the native boot, which writes what they wrote
    // to the NAND at start and gives the native IPC layer the title identity.
    // SI and DSP go once their periodic chassis events are retired.
    std::string list = ",ai,vi,si,pi,exi,hsp,mi,dsp,dvd,";
    if (StaticRecompNativeBoot::Enabled())
      list += "wiiipc,ios,";
    if (StaticRecompNativeGx::OwnsGpu())
      list += "gpfifo,";
    return list;
  }();
  return skip.find("," + std::string(device) + ",") != std::string::npos;
}

// Chassis set-up pieces this thread leaves out by default (the port does not
// use them: the DSP emulator with native audio, the chassis's pad/keyboard
// configuration with the host input, its input recording) and the ones it
// still does (its sound stream: the audio interface initialisation
// dereferences its mixer; the NAND system files at boot). MODERNGEKKO_HOST_LOOP_SKIP
// and MODERNGEKKO_HOST_LOOP_KEEP=<list> change the set (diagnostic): pieces are
// input, movie, audio, dsp, wiifs.
bool Skips(const char* piece)
{
  static const std::string skip = [] {
    const char* v = std::getenv("MODERNGEKKO_HOST_LOOP_SKIP");
    return std::string(v ? v : "");
  }();
  static const std::string keep = [] {
    const char* v = std::getenv("MODERNGEKKO_HOST_LOOP_KEEP");
    return std::string(v ? v : "");
  }();
  const std::string name(piece);
  if (keep.find(name) != std::string::npos)
    return false;
  if (skip.find(name) != std::string::npos)
    return true;
  // The NAND's system menu files are the native boot's (phase S step 24).
  if (name == "wiifs")
    return StaticRecompNativeBoot::Enabled();
  return name == "dsp" || name == "input" || name == "movie";
}

bool Enabled()
{
  static const bool enabled = [] {
    if (const char* setting = std::getenv("MODERNGEKKO_HOST_LOOP"))
    {
      const std::string value(setting);
      if (value == "chassis" || value == "off" || value == "0")
        return false;
    }
    // Movies live in the chassis's thread (Movie::Init in Core::EmuThread).
    if (std::getenv("MODERNGEKKO_MOVIE_RECORD") || std::getenv("MODERNGEKKO_MOVIE_PLAY"))
      return false;
    return true;
  }();
  return enabled;
}

void RunOnCpuThread(std::function<void()> function)
{
  const ThreadHooks& hooks = s_thread_hooks;
  if (!Enabled() && hooks.fallback)
  {
    hooks.fallback(std::move(function));
    return;
  }
  if (hooks.is_cpu_thread && hooks.is_cpu_thread())
  {
    function();
    return;
  }
  unsigned long long ticket = 0;
  {
    std::lock_guard lock(s_jobs_mutex);
    if (!s_loop_alive)
    {
      // No CPU loop to hand it to (before the boot, or after the stop).
      function();
      return;
    }
    s_jobs.push_back(std::move(function));
    ticket = ++s_jobs_submitted;
  }
  // Make the core return to the loop: pause, let the job run, resume.
  const bool was_running = hooks.running && hooks.running();
  if (was_running && hooks.set_stepping)
    hooks.set_stepping(true);
  {
    std::unique_lock lock(s_jobs_mutex);
    s_jobs_done.wait(lock, [&] { return s_jobs_finished >= ticket || !s_loop_alive; });
  }
  if (was_running && hooks.stepping && hooks.stepping() && hooks.set_stepping)
    hooks.set_stepping(false);
}

void SetChassisStatesUsed(bool used)
{
  s_chassis_states_used = used;
}

const Stats& GetStats()
{
  return s_stats;
}

RunState State()
{
  return s_state.load();
}

int AddStateListener(std::function<void(RunState)> listener)
{
  std::lock_guard lock(s_listeners_mutex);
  const int id = s_next_listener++;
  s_listeners.emplace_back(id, std::move(listener));
  return id;
}

void RemoveStateListener(int id)
{
  std::lock_guard lock(s_listeners_mutex);
  std::erase_if(s_listeners, [id](const auto& entry) { return entry.first == id; });
}
}  // namespace StaticRecompHostLoop
