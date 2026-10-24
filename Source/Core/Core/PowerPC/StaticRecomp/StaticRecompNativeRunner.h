// RecompCore: StaticRecomp CPU core - the port's own CPU runner (phase L4).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Runs the recompiled module with nothing of the chassis: the module and its
// code guard, the native machine (clock, PI, EXI, SDK layer, GP front, VI),
// the module's CPUState and the port's register file, the burst loop, the
// synchronous and external exceptions, the hardware-register hooks and the
// system instructions. A PC outside verified module code stops the run (there
// is no interpreter behind it). The oracle (the chassis's StaticRecompCore)
// runs the same pieces; this runner is what a port links.

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompBurst.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCodeGuard.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCpuHost.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRegs.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompKernel.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompMachine.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleLoader.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeBat.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompPowerOn.h"
#include "core/cpu.h"

namespace StaticRecompNativeRunner
{
struct Config
{
  std::string module_path;
  std::string game_id;           // checked against the module's
  std::string sram_path;         // the EXI's SRAM (sram.bin in the Wii user folder)
  std::string font_dir;          // the IPL fonts (Sys/GC, ending in a separator)
  std::vector<std::string> dsp_coef_paths;  // the AX resampling table
  u32 idle_pc = 0;               // the OS idle loop to skip (0: none)
};

class Runner final : public StaticRecompCpuHost::CpuHost
{
public:
  Runner();
  ~Runner() override;

  // Loads and validates the module and creates the machine. false: `error`.
  bool Init(const Config& config, std::string* error);
  // The registers the boot left (power-on state plus the boot's).
  void SetRegisters(const StaticRecompPowerOn::Registers& registers);
  // A native checkpoint (.mgs); false when it does not load.
  bool LoadCheckpoint(const std::string& path);
  bool SaveCheckpoint(const std::string& path);
  // From another thread: the CPU thread saves a checkpoint between two slices.
  void RequestSave(const std::string& path);

  // Runs slices until `stop` is set (another thread) or the run fails.
  // Returns false when it stopped on an error (a PC outside the module, an
  // unsupported instruction).
  bool Run(const std::atomic<bool>& stop);

  // CpuHost.
  u32 Dbat(u32 ea) const override { return m_dbat.Entry(ea); }
  void UpdateNativeBats() override;
  void DoNativeState(StaticRecompState::StateStream& p, bool present) override;
  void SeedNativeDevicesForNativeState() override;

  StaticRecompMachine::Machine& GetMachine() { return m_machine; }

  // Menu extension modules (tools/build_pc_menu_module.py) report points of
  // the game's controllers through CPUState::host_call: version 1..3, or 0
  // for an ordinary module. Their boot-notice events (0xffff0040/41) go to
  // the SDK layer; the others to this handler (on the CPU thread, inside the
  // module; true: the event replaced the guest code, cpu.pc says where to go).
  using HostCallHandler = std::function<bool(CPUState& cpu, u32 event)>;
  void SetHostCallHandler(HostCallHandler handler) { m_host_call_handler = std::move(handler); }
  u32 MenuExtensionVersion() const { return m_menu_extension_version; }
  // Runs a guest function to its return on a copy of `caller` (stack below
  // the caller's, return to a sentinel); nullopt when it leaves the module,
  // faults or runs too long. For host code called from a host_call event.
  std::optional<u32> InvokeGuest(CPUState& caller, u32 address, std::span<const u32> arguments);
  std::optional<CPUState> RunGuestCopy(CPUState& guest, u32 address);
  const CPUState& Guest() const { return m_guest; }
  void PrintStats() const;

private:
  static u64 HookRead(CPUState* cpu, u32 ea, u8 size);
  static void HookWrite(CPUState* cpu, u32 ea, u64 value, u8 size);
  static u32 HookRead32(CPUState* cpu, u32 ea, u8 rid);
  static void HookWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid);
  static void* HookPointer(CPUState* cpu, u32 ea, u32 size);
  static void HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia);
  static bool HookHostCall(CPUState* cpu, u32 event);
  void Fail(const char* what, u32 address);
  void SetHostRoundingMode() const;
  void InstallStateHooks();

  StaticRecompModuleLoader::ModuleLibrary m_library;
  const StaticRecompModuleDesc* m_module = nullptr;
  StaticRecompCodeGuard::CodeGuard m_code_guard;
  StaticRecompMachine::Machine m_machine;
  StaticRecompNativeBat::NativeBat m_dbat;
  CPUState m_guest{};
  StaticRecompGuestRegs::GuestRegs m_regs{};
  StaticRecompKernel::KernelHost m_kernel_host;
  StaticRecompBurst::Context m_burst;
  int m_run_flag = 0;  // 0 running, 1 stop
  std::mutex m_save_mutex;
  std::string m_save_request;
  std::atomic<bool> m_save_pending{false};
  bool m_failed = false;
  u32 m_menu_extension_version = 0;
  HostCallHandler m_host_call_handler;

  u64 m_dispatches = 0, m_copy_bytes = 0, m_copy_batches = 0, m_charged_cycles = 0;
  u64 m_exceptions = 0, m_kernel_calls = 0, m_sync_exceptions = 0, m_slices = 0;
  u64 m_unhandled_reads = 0, m_unhandled_writes = 0, m_fallbacks = 0;
  std::unordered_map<u32, u64> m_pc_samples;
};
}  // namespace StaticRecompNativeRunner
