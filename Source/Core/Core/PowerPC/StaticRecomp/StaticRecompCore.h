// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>
#include <functional>
#include <span>
#include <optional>

#include "Core/PowerPC/StaticRecomp/StaticRecompCodeGuard.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCpuHost.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompMachine.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleLoader.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRegs.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeBat.h"

namespace Core
{
class System;
}

namespace PowerPC
{
struct PowerPCState;
}

namespace StaticRecompLockstep
{
class StaticRecompLockstepVerifier;
}

namespace StaticRecompNativeSdk
{
class NativeSdk;
}

// Executes statically recompiled per-game native code when the PC is covered by
// a loaded module; falls back to Dolphin's interpreter for everything else.
// With no module loaded this core is exactly an interpreter loop.
namespace StaticRecompNativeGp
{
class NativeGp;
}
namespace StaticRecompNativeVi
{
class NativeVi;
}
namespace StaticRecompNativeClock
{
class NativeClock;
}
namespace StaticRecompNativePi
{
class NativePi;
}
namespace StaticRecompNativeExi
{
class NativeExi;
}
namespace StaticRecompState
{
class StateStream;
}

class StaticRecompCore : public JitBase, public StaticRecompCpuHost::CpuHost
{
public:
  friend class StaticRecompLockstep::StaticRecompLockstepVerifier;

  explicit StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source);
  StaticRecompCore(const StaticRecompCore&) = delete;
  StaticRecompCore(StaticRecompCore&&) = delete;
  StaticRecompCore& operator=(const StaticRecompCore&) = delete;
  StaticRecompCore& operator=(StaticRecompCore&&) = delete;
  ~StaticRecompCore() override;

  void Init() override;
  void Shutdown() override;

  void Run() override;
  void SingleStep() override;
  bool IsModuleActive() const;
  bool DispatchableAt(u32 address);
  bool FastDispatchableAt(u32 address) const;
  using MenuExtensionHook=std::function<bool(CPUState&,u32)>;
  void SetMenuExtensionHook(std::shared_ptr<MenuExtensionHook> hook){m_menu_extension_hook.store(std::move(hook));}
  // Synchronous original AOT utility calls, only from a menu hook on the CPU thread.
  std::optional<u32> InvokeMenuFunction(CPUState& caller,u32 address,std::span<const u32> arguments);
  u32 MenuExtensionVersion()const{return m_menu_extension_version;}

  void ClearCache() override;
  void DoExtraState(PointerWrap& p, bool present) override;
  void DoNativeState(StaticRecompState::StateStream& p, bool present) override;  // the native sections
  void SeedNativeDevicesForNativeState() override;  // phase S: before writing a native checkpoint
  // Phase S: the data BAT table of the native layer (built from the game's DBAT
  // registers); rebuilt by the kernel on BAT/HID4 writes and after state loads.
  void UpdateNativeBats() override;
  u32 Dbat(u32 ea) const override { return m_native_dbat.Entry(ea); }
  // The authoritative SPR array (phase S): the port's while the guest runs,
  // the chassis's mirror while the chassis acts.
  u32* Sprs();
  int& Downcount();           // the slice counter (authoritative copy)
  u32& PendingExceptions();   // the pending lines (authoritative copy)
  // Lines the chassis raised on its mirror while the guest had the authority
  // (its MMU hooks): moved into the port's copy.
  void FoldChassisExceptions();
  // The chassis's CheckExceptions on the module's state; false when nothing
  // synchronous is pending.
  bool DeliverSynchronousException();
  const u32* Sprs() const { return const_cast<StaticRecompCore*>(this)->Sprs(); }
  void Jit(u32 em_address) override {}
  bool HandleFault(uintptr_t access_address, SContext* ctx) override { return false; }

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }
  void EraseSingleBlock(const JitBlock& block) override {}
  std::vector<MemoryStats> GetMemoryStats() const override { return {}; }
  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }
  const char* GetName() const override { return "StaticRecomp"; }

private:
  // JitBaseBlockCache with no generated blocks; exists so generic
  // icache-invalidation plumbing in JitInterface has a real object to talk
  // to, and to feed every invalidation into the SMC demotion guard (D4).
  class EmptyBlockCache : public JitBaseBlockCache
  {
  public:
    explicit EmptyBlockCache(StaticRecompCore& core) : JitBaseBlockCache(core), m_core(core) {}
    void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override {}

  protected:
    void InvalidateICacheInternal(u32 physical_address, u32 address, u32 length,
                                  bool forced) override
    {
      m_core.OnICacheInvalidate(address, length, physical_address);
    }

  private:
    StaticRecompCore& m_core;
  };

  void LoadModule();

  // The D4 SMC guard (StaticRecompCodeGuard); the chassis's icache
  // invalidations reach it through the block cache above.
  void OnICacheInvalidate(u32 address, u32 length, u32 physical_address);

  static void SetPPCStateFromGuestState(const CPUState& s, PowerPC::PowerPCState& ppc);

  // D1 state residency: registers live in m_guest while native code runs;
  // full sync at every native-burst boundary.
  void SyncIn();   // Dolphin PowerPCState -> m_guest
  void SyncOut();  // m_guest -> Dolphin PowerPCState
  // Phase S: the module's CPUState is the CPU state while the core runs; the
  // chassis's PowerPCState is a mirror refreshed only where the chassis acts
  // (interpreter fallback, its exception delivery, savestates, Run() entry
  // and exit). EnterGuest() before native code, EnterChassis() before a
  // chassis path; both are no-ops when the authority already matches.
  enum class StateAuthority : u8
  {
    Chassis,
    Guest,
  };
  void EnterGuest();
  void EnterChassis();
  StateAuthority m_authority = StateAuthority::Chassis;
  bool m_sync_every_burst = false;  // lockstep, chassis exception delivery or MODERNGEKKO_NATIVE_SYNC_EVERY_BURST
  u64 m_sync_in = 0;
  u64 m_sync_out = 0;
  bool m_native_sync_exceptions = true;  // MODERNGEKKO_NATIVE_SYNC_EXCEPTIONS=off: the chassis delivers
  u64 m_native_sync_exceptions_delivered = 0;

  // CPUState hooks (module -> chassis environment). `cpu->external_user_data`
  // is the StaticRecompCore*.
  static u64 HookExternalRead(CPUState* cpu, u32 ea, u8 size);
  static void HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size);
  static u32 HookExternalRead32(CPUState* cpu, u32 ea, u8 rid);
  static void HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid);
  static void* HookExternalPointer(CPUState* cpu, u32 ea, u32 size);
  static void HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia);
  static bool HookMenuExtension(CPUState* cpu,u32 event);
  std::atomic<std::shared_ptr<MenuExtensionHook>> m_menu_extension_hook;
  u32 m_menu_extension_version=0;

  // Keep Dolphin's MSR-derived state (translation mode, feature flags) in step
  // with the guest MSR before any MMU access or exception delivery.
  void PropagateGuestMSR();
  void RefreshDispatchBudget(CPUState& cpu);

  std::unique_ptr<StaticRecompLockstep::StaticRecompLockstepVerifier> m_lockstep_verifier;
  // The port's devices and their register dispatch (phase L1).
  StaticRecompMachine::Machine m_machine;

  EmptyBlockCache m_block_cache{*this};
  StaticRecompNativeBat::NativeBat m_native_dbat;

  CPUState m_guest{};
  StaticRecompGuestRegs::GuestRegs m_regs{};  // phase S: SPRs outside CPUState
  StaticRecompModuleLoader::ModuleLibrary m_library;
  StaticRecompModuleSource m_module_source;
  const StaticRecompModuleDesc* m_module = nullptr;
  bool m_module_active = false;
  std::unique_ptr<JitBase> m_fallback_jit;

  bool m_boot_dumped = false;  // MODERNGEKKO_BOOT_DUMP written (phase G diagnostic)
  bool m_state_converted = false;  // MODERNGEKKO_CONVERT_STATE written (phase S)
  u64 m_native_dispatches = 0;
  u64 m_fallback_steps = 0;
  u64 m_fallback_jit_runs = 0;
  u64 m_native_kernel_calls = 0;
  u64 m_native_cache_instructions = 0;
  u64 m_native_cache_batches = 0;
  u64 m_native_copy_bytes = 0;
  u64 m_native_copy_batches = 0;
  bool m_native_copy_enabled = true;
  bool m_native_branch_batching = true;
  std::string m_native_pc_profile_path;
  // Dispatch sampling mask: 1023 samples one dispatch in 1024; 0 records every dispatch.
  u64 m_native_pc_profile_mask = 1023;
  std::unordered_map<u32, u64> m_native_pc_samples;
  std::unordered_map<u32, u64> m_native_fallback_samples;
  std::unordered_map<u32, u64> m_native_hook_samples;
  u64 m_native_exceptions = 0;
  u64 m_hook_fallback_instructions = 0;
  u64 m_native_gather_register_instructions = 0;
  u64 m_native_scratch_register_instructions = 0;
  u64 m_native_decrementer_reads = 0;
  u64 m_native_decrementer_writes = 0;
  u64 m_native_dma_register_instructions = 0;
  u64 m_native_system_register_instructions = 0;
  u64 m_native_physical_accesses = 0;  // real-mode RAM accesses served without the chassis MMU
  u64 m_bursts = 0;          // SyncIn..SyncOut native runs (diagnostic)
  u64 m_charged_cycles = 0;  // cycles flushed from module charges (diagnostic)

  StaticRecompCodeGuard::CodeGuard m_code_guard;  // D4: per-chunk verification state


  u32 m_idle_pc = 0;
};

extern StaticRecompCore* g_static_recomp_core;
