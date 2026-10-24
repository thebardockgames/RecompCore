// RecompCore: StaticRecomp CPU core - the port's emulation thread (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The body of the emulation thread and the CPU loop belong to the port: what
// the chassis's Core::EmuThread/CPUManager::Run did (hardware and video
// initialisation, the boot, the savestate at boot, the run loop with its
// pause/stop states and the jobs other threads hand to the CPU thread) is
// done here with only the pieces the port still needs. The chassis's devices
// are still constructed underneath (HW::Init) and its CPU state machine is
// still the shared pause/stop flag, so Core::Stop and Core::SetState keep
// working; the jobs for the CPU thread are the port's (RunOnCpuThread), the
// chassis's queue is not served.
//
// Boot/Stop/Join replace the chassis's BootManager::BootCore, Core::Init,
// Core::Stop and Core::Shutdown: game metadata and configuration, Wii root
// and SYSCONF, the emulation thread and its end. The process state is the
// port's (RunState); the chassis's copy is mirrored for the chassis paths
// that still read it (State::SaveAs, the reset button).
//
// MODERNGEKKO_HOST_LOOP=chassis keeps the chassis's thread (also forced while a
// movie is recorded or played: that path lives in the chassis's thread).

#pragma once

#include <functional>
#include <memory>

struct BootParameters;
struct WindowSystemInfo;
namespace Core
{
class System;
}

namespace StaticRecompHostLoop
{
bool Enabled();

enum class RunState
{
  Uninitialized,
  Starting,
  Running,
  Paused,
  Stopping,
};
RunState State();
// Sets the state and tells the mirror and the listeners.
void SetState(RunState state);
// Who mirrors the state elsewhere (the chassis's Core while it exists).
void SetStateMirror(std::function<void(RunState)> mirror);
// Listeners run on the thread that changes the state. Returns an id.
int AddStateListener(std::function<void(RunState)> listener);
void RemoveStateListener(int id);

// The whole start: configuration and metadata of the game, Wii root and
// SYSCONF, the video backend's window preparation and the emulation thread
// (EmuBody). false when the game cannot be booted.
bool Boot(Core::System& system, std::unique_ptr<BootParameters> boot, const WindowSystemInfo& wsi);
// Asks the CPU loop to end (the thread finishes on its own); Join waits for it.
void Stop(Core::System& system);
void Join(Core::System& system);
void Pause(Core::System& system);
// The run loads or saves a chassis savestate (.sav, the oracle's format):
// every chassis device is initialised (HW::DoState serialises all of them).
void SetChassisStatesUsed(bool used);
// Whether the chassis device `name` (state, ai, vi, si, pi, ...) is left
// uninitialised by the native host loop.
bool SkipsDevice(const char* device);
void Resume(Core::System& system);

// The emulation thread body (the chassis's bring-up around the CPU loop;
// StaticRecompHostLoopChassis.cpp). Returns when the CPU loop ends.
void EmuBody(Core::System& system, std::unique_ptr<BootParameters> boot, const WindowSystemInfo& wsi);

// The CPU loop. `state` is the shared run flag, `run` runs the core until it
// changes; jobs for the CPU thread run between the two.
enum class CpuRun
{
  Running,
  Paused,
  Off,
};
struct CpuLoopHooks
{
  std::function<CpuRun()> state;
  std::function<void()> run;
};
void CpuLoop(const CpuLoopHooks& hooks);

// What RunOnCpuThread needs from the thread owner: the chassis's own queue
// when its thread runs (fallback), whether this is the CPU thread, and the
// run flag to make the core return to the loop.
struct ThreadHooks
{
  std::function<void(std::function<void()>)> fallback;
  std::function<bool()> is_cpu_thread;
  std::function<bool()> running;
  std::function<bool()> stepping;
  std::function<void(bool)> set_stepping;
};
void SetThreadHooks(ThreadHooks hooks);
// The host loop's pieces of the chassis set-up left out (input, movie, audio,
// dsp, wiifs).
bool Skips(const char* piece);

// Runs `function` on the CPU thread between two bursts and waits for it (runs
// it at once when called from the CPU thread). The chassis's job queue when
// the chassis's thread runs.
void RunOnCpuThread(std::function<void()> function);

struct Stats
{
  unsigned long long loops = 0;        // CPU loop iterations
  unsigned long long jobs = 0;         // jobs run for other threads
  unsigned long long pauses = 0;       // iterations spent paused
};
const Stats& GetStats();
}  // namespace StaticRecompHostLoop
