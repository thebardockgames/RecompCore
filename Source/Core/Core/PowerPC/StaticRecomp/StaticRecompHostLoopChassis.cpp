// RecompCore: StaticRecomp CPU core - chassis adapter of the emulation thread (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The bring-up the chassis still needs around the port's emulation thread:
// game metadata, configuration and SYSCONF, the chassis's devices (HW::Init,
// piece by piece), its video backend (the Null backend hosting NativeGX, or
// its renderer as the oracle), its boot entry, its savestate at boot, the
// mirror of the process state in its Core, and its CPU state machine as the
// run flag of the port's CPU loop.

#include "Core/PowerPC/StaticRecomp/StaticRecompHostLoopChassis.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompFrameEvents.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompHostLoop.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeBoot.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGx.h"

#include <cstdio>
#include <future>
#include <thread>

#include "AudioCommon/AudioCommon.h"
#include "Common/CPUDetect.h"
#include "Common/ScopeGuard.h"
#include "Common/Thread.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/ConfigLoaders/BaseConfigLoader.h"
#include "Core/Core.h"
#include "DiscIO/Enums.h"
#include "Core/DSPEmulator.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/HSP/HSP.h"
#include "Core/HW/HW.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/MemoryInterface.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WII_IPC.h"
#include "Core/IOS/IOS.h"
#include "Core/Cheats/PatchEngine.h"
#include "Core/Movie.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/Wii/WiiRoot.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoBackendBase.h"

namespace StaticRecompHostLoop
{
namespace
{
std::thread s_thread;

// The chassis's enum for its mirror.
Core::State ChassisState(RunState state)
{
  switch (state)
  {
  case RunState::Starting:
    return Core::State::Starting;
  case RunState::Running:
    return Core::State::Running;
  case RunState::Paused:
    return Core::State::Paused;
  case RunState::Stopping:
    return Core::State::Stopping;
  default:
    return Core::State::Uninitialized;
  }
}

void InstallHooks()
{
  // Pause is the CPU's stepping state in the chassis's model, not a core
  // state: its mirror keeps Running while paused (Core::GetState derives it).
  SetStateMirror([](RunState state) {
    Core::NativeSetCoreState(ChassisState(state == RunState::Paused ? RunState::Running : state));
  });
  ThreadHooks hooks;
  hooks.fallback = [](std::function<void()> function) {
    Core::RunOnCPUThread(Core::System::GetInstance(), std::move(function));
  };
  hooks.is_cpu_thread = [] { return Core::IsCPUThread(); };
  hooks.running = [] { return Core::System::GetInstance().GetCPU().GetState() == CPU::State::Running; };
  hooks.stepping = [] { return Core::System::GetInstance().GetCPU().GetState() == CPU::State::Stepping; };
  hooks.set_stepping = [](bool stepping) { Core::System::GetInstance().GetCPU().SetStepping(stepping); };
  SetThreadHooks(std::move(hooks));
}

// The CPU loop's run flag is the chassis's CPU state (Running / Stepping =
// paused / PowerDown); the core (StaticRecompCore::Run) runs through its
// PowerPC manager until the state changes.
CpuLoopHooks LoopHooks(Core::System& system)
{
  CpuLoopHooks hooks;
  hooks.state = [&system] {
    switch (system.GetCPU().GetState())
    {
    case CPU::State::PowerDown:
      return CpuRun::Off;
    case CPU::State::Running:
      return CpuRun::Running;
    default:
      return CpuRun::Paused;
    }
  };
  hooks.run = [&system] { system.GetPowerPC().RunLoop(); };
  return hooks;
}
}  // namespace

// The chassis's devices this thread initialises (HW::Init, piece by piece).
// MODERNGEKKO_HW_SKIP=<list> leaves some out (diagnostic / port default
// below): state, ai, vi, si, pi, exi, hsp, mi, dsp, dvd, gpfifo, wiiipc, ios.
// Always: CoreTiming (facade of the native clock), SystemTimers, Memory (the
// lent buffers), AddressSpace, CPU (pause/stop flag), the MMIO map.

struct HwInitResult
{
  std::string skipped;
};

HwInitResult NativeHwInit(Core::System& system)
{
  HwInitResult result;
  const auto use = [&](const char* name) {
    if (!SkipsDevice(name))
      return true;
    result.skipped += result.skipped.empty() ? name : std::string(",") + name;
    return false;
  };
  system.GetCoreTiming().Init();
  system.GetSystemTimers().PreInit();
  if (use("state"))
    State::Init(system);
  if (use("ai"))
    system.GetAudioInterface().Init();
  if (use("vi"))
    system.GetVideoInterface().Init();
  if (use("si"))
    system.GetSerialInterface().Init();
  if (use("pi"))
    system.GetProcessorInterface().Init();
  if (use("exi"))
    system.GetExpansionInterface().Init(nullptr);
  if (use("hsp"))
    system.GetHSP().Init();
  system.GetMemory().Init();
  AddressSpace::Init();
  if (use("mi"))
    system.GetMemoryInterface().Init();
  if (use("dsp"))
    system.GetDSP().Init(Config::Get(Config::MAIN_DSP_HLE));
  if (use("dvd"))
    system.GetDVDInterface().Init();
  if (use("gpfifo"))
    system.GetGPFifo().Init();
  system.GetCPU().Init(Config::Get(Config::MAIN_CPU_CORE));
  system.GetSystemTimers().Init();
  // The chassis's periodic events of devices not initialised: its VI event
  // updates the SI devices, its DSP event steps the DSP emulator. The native
  // VI and audio replace both (they retired them later anyway).
  if (SkipsDevice("vi") && SkipsDevice("si"))
    system.GetCoreTiming().RemoveAllEvents(system.GetSystemTimers().GetVIEventType());
  if (SkipsDevice("dsp"))
    system.GetCoreTiming().RemoveAllEvents(system.GetSystemTimers().GetDSPEventType());
  if (system.IsWii())
  {
    if (use("wiiipc"))
      system.GetWiiIPC().Init();
    if (use("ios"))
      IOS::HLE::Init(system);
  }
  system.GetMemory().InitMMIO(system);
  return result;
}

void NativeHwShutdown(Core::System& system)
{
  if (!SkipsDevice("ios"))
    IOS::HLE::Shutdown(system);
  if (!SkipsDevice("wiiipc"))
    system.GetWiiIPC().Shutdown();
  system.GetSystemTimers().Shutdown();
  system.GetCPU().Shutdown();
  if (!SkipsDevice("dvd"))
    system.GetDVDInterface().Shutdown();
  if (!SkipsDevice("dsp"))
    system.GetDSP().Shutdown();
  if (!SkipsDevice("mi"))
    system.GetMemoryInterface().Shutdown();
  AddressSpace::Shutdown();
  system.GetMemory().Shutdown();
  if (!SkipsDevice("hsp"))
    system.GetHSP().Shutdown();
  if (!SkipsDevice("exi"))
    system.GetExpansionInterface().Shutdown();
  if (!SkipsDevice("si"))
    system.GetSerialInterface().Shutdown();
  if (!SkipsDevice("ai"))
    system.GetAudioInterface().Shutdown();
  if (!SkipsDevice("state"))
    State::Shutdown();
  system.GetCoreTiming().Shutdown();
}

void EmuBody(Core::System& system, std::unique_ptr<BootParameters> boot, const WindowSystemInfo& wsi)
{
  Common::SetCurrentThreadName(system.IsDualCoreMode() ? "Port CPU thread" : "Port CPU-GPU thread");
  BootSessionData boot_session_data = std::move(boot->boot_session_data);
  const std::optional<std::string> savestate_path = boot_session_data.GetSavestatePath();

  // What the chassis's thread set up before the hardware, kept while the
  // chassis exists: the input window of its controller interface (Wii Remote
  // rows), its pad/keyboard configuration, its input recording (inactive).
  const bool skip_input = Skips("input"), skip_movie = Skips("movie"), skip_audio = Skips("audio"),
             skip_dsp = Skips("dsp"), skip_wiifs = Skips("wiifs");
  if (!skip_input)
  {
    if (g_controller_interface.IsInit())
      g_controller_interface.ChangeWindow(wsi.render_window);
    Pad::LoadConfig();
    Keyboard::LoadConfig();
  }
  if (!skip_movie)
    system.GetMovie().Init(*boot);
  Common::ScopeGuard movie_guard([&system, skip_movie] {
    if (!skip_movie)
      system.GetMovie().Shutdown();
  });
  // The chassis's sound stream (No Audio Output here: the native sink plays)
  // still exists for the objects that reference its mixer.
  if (!skip_audio)
    AudioCommon::InitSoundStream(system);
  Common::ScopeGuard audio_guard([&system, skip_audio] {
    if (!skip_audio)
      AudioCommon::ShutdownSoundStream(system);
  });

  // The chassis's devices: still the objects the remaining chassis paths and
  // the lent memory live in.
  const HwInitResult hw = NativeHwInit(system);
  if (!hw.skipped.empty())
    std::fprintf(stderr, "[host_loop] chassis devices not initialised: %s\n", hw.skipped.c_str());
  Common::ScopeGuard hw_guard{[&system] {
    NativeHwShutdown(system);
    BootManager::RestoreConfig();
    PatchEngine::Shutdown();
    HLE::Clear();
    Core::CPUThreadGuard guard(system);
    system.GetPowerPC().GetDebugInterface().Clear(guard);
  }};

  // Video: the Null backend hosts the native renderer's adapter (NativeGX) or
  // the chassis's renderer (the oracle). Two threads: the chassis's GPU loop
  // on its own thread, as before.
  Core::DeclareAsGPUThread();
  AsyncRequests::GetInstance()->SetPassthrough(!system.IsDualCoreMode());
  std::thread gpu_thread;
  bool video_ok = false;
  if (system.IsDualCoreMode())
  {
    std::promise<bool> init_from_thread;
    gpu_thread = std::thread([&] {
      Common::SetCurrentThreadName("Video thread");
      const bool is_init = g_video_backend->Initialize(wsi);
      init_from_thread.set_value(is_init);
      if (!is_init)
        return;
      system.GetFifo().RunGpuLoop();
      OSD::ClearMessages();
      g_video_backend->Shutdown();
    });
    video_ok = init_from_thread.get_future().get();
    if (!video_ok)
      gpu_thread.join();
  }
  else
  {
    video_ok = g_video_backend->Initialize(wsi);
  }
  if (!video_ok)
  {
    std::fprintf(stderr, "[host_loop] the video backend did not initialise\n");
    return;
  }
  Common::ScopeGuard video_guard{[&] {
    if (system.IsDualCoreMode())
    {
      system.GetFifo().ExitGpuLoop();
      gpu_thread.join();
    }
    else
    {
      OSD::ClearMessages();
      g_video_backend->Shutdown();
    }
  }};

  // The chassis's DSP emulator object (inactive with native audio) and its
  // sound stream post-initialisation.
  if (!skip_dsp)
  {
    if (cpu_info.HTT)
      Config::SetBaseOrCurrent(Config::MAIN_DSP_THREAD, cpu_info.num_cores > 4);
    else
      Config::SetBaseOrCurrent(Config::MAIN_DSP_THREAD, cpu_info.num_cores > 2);
    if (!system.GetDSP().GetDSPEmulator()->Initialize(system.IsWii(), Config::Get(Config::MAIN_DSP_THREAD)))
    {
      std::fprintf(stderr, "[host_loop] the DSP emulator did not initialise\n");
      return;
    }
  }
  if (!skip_audio)
    AudioCommon::PostInitSoundStream(system);

  // The boot (phase G: our own, through the chassis's entry).
  system.GetCPU().Break();
  system.GetPowerPC().SetMode(PowerPC::CoreMode::Interpreter);
  {
    Core::CPUThreadGuard guard(system);
    if (!CBoot::BootUp(system, guard, std::move(boot)))
    {
      std::fprintf(stderr, "[host_loop] the boot failed\n");
      return;
    }
  }
  // The NAND's system files (the same folder the native store uses).
  Common::ScopeGuard wiifs_guard{[&boot_session_data] {
    Core::CleanUpWiiFileSystemContents(boot_session_data);
    boot_session_data.InvokeWiiSyncCleanup();
  }};
  if (system.IsWii() && !skip_wiifs)
    Core::InitializeWiiFileSystemContents(std::nullopt, boot_session_data);
  else
    wiifs_guard.Dismiss();

  system.GetFifo().Prepare();
  if (Config::Get(Config::MAIN_CPU_CORE) != PowerPC::CPUCore::Interpreter)
    system.GetPowerPC().SetMode(PowerPC::CoreMode::JIT);
  else
    system.GetPowerPC().SetMode(PowerPC::CoreMode::Interpreter);
  Core::UpdateTitle(system);

  // The CPU thread: a chassis savestate given for the boot, then the run.
  if (savestate_path)
    State::LoadAs(system, *savestate_path);
  SetState(RunState::Running);
  // The CPU's own state (the chassis's CPUManager, the flag the core polls).
  Core::SetState(system, SConfig::GetInstance().bBootToPause ? Core::State::Paused : Core::State::Running,
                 false, true);
  if (SConfig::GetInstance().bBootToPause)
    SetState(RunState::Paused);
  std::fprintf(stderr, "[host_loop] emulation thread and CPU loop owned natively (%s)%s%s%s%s%s\n",
               system.IsDualCoreMode() ? "two threads" : "one thread", skip_input ? ", no chassis input" : "",
               skip_movie ? ", no movie" : "", skip_audio ? ", no chassis sound stream" : "",
               skip_dsp ? ", no DSP emulator" : "", skip_wiifs ? ", no Wii FS init" : "");
  CpuLoop(LoopHooks(system));
  {
    const auto frames = StaticRecompFrameEvents::GetStats();
    std::fprintf(stderr, "[frame_events] own=%llu forwarded=%llu echoes=%llu\n", frames.own,
                 frames.forwarded, frames.echoes);
  }
  std::fprintf(stderr, "[host_loop] loops=%llu jobs=%llu pauses=%llu\n", GetStats().loops, GetStats().jobs,
               GetStats().pauses);
}

namespace
{
// The emulation thread: the chassis's guards around EmuBody.
void EmuThread(Core::System* system, std::unique_ptr<BootParameters> boot, WindowSystemInfo wsi)
{
  Common::ScopeGuard end_guard{[] { SetState(RunState::Uninitialized); }};
  Core::DeclareAsCPUThread();
  CPUThreadConfigCallback::CheckForConfigChanges();
  EmuBody(*system, std::move(boot), wsi);
}
}  // namespace

bool Boot(Core::System& system, std::unique_ptr<BootParameters> boot, const WindowSystemInfo& wsi)
{
  if (!boot || s_thread.joinable())
    return false;
  // BootManager::BootCore for a disc or DOL of this port: the game's metadata
  // and configuration layers, the region's language and country, the Wii root
  // and the SYSCONF the configuration writes to the NAND. (No NetPlay,
  // Triforce, movie, Riivolution or GameCube IPL here.)
  SConfig& startup = SConfig::GetInstance();
  if (!startup.SetPathsAndGameMetadata(system, *boot))
    return false;
  if (!Config::Get(Config::MAIN_OVERRIDE_REGION_SETTINGS))
  {
    Config::SetCurrent(Config::MAIN_GC_LANGUAGE,
                       DiscIO::ToGameCubeLanguage(startup.GetLanguageAdjustedForRegion(false, startup.m_region)));
    if (system.IsWii())
    {
      const u32 wii_language = static_cast<u32>(startup.GetLanguageAdjustedForRegion(true, startup.m_region));
      if (wii_language != Config::Get(Config::SYSCONF_LANGUAGE))
        Config::SetCurrent(Config::SYSCONF_LANGUAGE, wii_language);
      const u8 country_code = static_cast<u8>(Config::Get(Config::SYSCONF_COUNTRY));
      if (startup.m_region != DiscIO::SysConfCountryToRegion(country_code))
      {
        switch (startup.m_region)
        {
        case DiscIO::Region::NTSC_J:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x01);
          break;
        case DiscIO::Region::NTSC_U:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x31);
          break;
        case DiscIO::Region::PAL:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x6c);
          break;
        case DiscIO::Region::NTSC_K:
          Config::SetCurrent(Config::SYSCONF_COUNTRY, 0x88);
          break;
        default:
          break;
        }
      }
    }
  }
  if (system.IsWii() && DiscIO::IsNTSC(startup.m_region) && Config::Get(Config::SYSCONF_PAL60))
    Config::SetCurrent(Config::SYSCONF_PAL60, false);
  system.Initialize();
  Core::UpdateWantDeterminism(system, true);
  ConfigLoaders::TransferSYSCONFControlToGuest();
  if (system.IsWii())
  {
    Core::InitializeWiiRoot(Core::WantsDeterminism());
    if (!Core::WantsDeterminism())
    {
      Core::BackupWiiSettings();
      ConfigLoaders::SaveToSYSCONF(Config::LayerType::Meta, ConfigLoaders::SkipIfControlledByGuest::No);
    }
    else
    {
      ConfigLoaders::SaveToSYSCONF(Config::LayerType::Meta, ConfigLoaders::SkipIfControlledByGuest::No,
                                   [](const Config::Location& location) {
                                     return Config::GetActiveLayerForConfig(location) >=
                                            Config::LayerType::Movie;
                                   });
    }
  }
  // Core::Init: the video backend's window, then the thread.
  Core::HostDispatchJobs(system);
  VideoBackendBase::PopulateBackendInfo(wsi);
  WindowSystemInfo prepared_wsi(wsi);
  g_video_backend->PrepareWindow(prepared_wsi);
  SetState(RunState::Starting);
  s_thread = std::thread(EmuThread, &system, std::move(boot), prepared_wsi);
  return true;
}

void Stop(Core::System& system)
{
  const RunState state = State();
  if (state == RunState::Stopping || state == RunState::Uninitialized)
    return;
  SetState(RunState::Stopping);
  Core::HostDispatchJobs(system);
  system.GetFifo().EmulatorState(false);
  system.GetCPU().Stop();  // PowerDown: the CPU loop ends and the thread finishes
}

void Join(Core::System& system)
{
  if (s_thread.joinable())
    s_thread.join();
  Core::HostDispatchJobs(system);
}

void Pause(Core::System& system)
{
  if (State() != RunState::Running)
    return;
  Core::SetState(system, Core::State::Paused, false);
  SetState(RunState::Paused);
}

void Resume(Core::System& system)
{
  if (State() != RunState::Paused)
    return;
  Core::SetState(system, Core::State::Running, false);
  SetState(RunState::Running);
}
}  // namespace StaticRecompHostLoop

namespace StaticRecompHostLoopChassis
{
void Install()
{
  StaticRecompHostLoop::InstallHooks();
}
}  // namespace StaticRecompHostLoopChassis
