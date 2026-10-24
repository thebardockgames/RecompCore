// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/Config/Config.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleLoader.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGp.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeVi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeExi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeAx.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClockChassis.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGpChassis.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePiChassis.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdkChassis.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeViChassis.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"
#include "Core/System.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif
#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif


StaticRecompCore* g_static_recomp_core = nullptr;

namespace
{
}  // namespace

bool StaticRecompCore::IsModuleActive() const
{
  return m_module_active;
}

StaticRecompCore::StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source)
    : JitBase(system), m_module_source(std::move(module_source))
{
}

StaticRecompCore::~StaticRecompCore() = default;

void StaticRecompCore::Init()
{
  g_static_recomp_core = this;
  StaticRecompCpuHost::g_cpu_host = this;
  RefreshConfig();
  jo.enableBlocklink = false;
  jo.fastmem = false;
  jo.fastmem_arena = false;

  m_block_cache.Init();

  m_guest = CPUState{};
  m_guest.external_read = HookExternalRead;
  m_guest.external_write = HookExternalWrite;
  m_guest.external_read32 = HookExternalRead32;
  m_guest.external_write32 = HookExternalWrite32;
  m_guest.external_pointer = HookExternalPointer;
  m_guest.instruction_fallback = HookInstructionFallback;
  m_guest.host_call = nullptr;
  m_guest.external_user_data = this;

  std::fprintf(stderr, "[staticrecomp] core init\n");

  LoadModule();
  m_idle_pc = Config::Get(Config::MAIN_STATICRECOMP_IDLE_PC);
  std::fprintf(stderr, "[staticrecomp] configured idle pc: 0x%08x\n", m_idle_pc);
  if (const char* path = std::getenv("MODERNGEKKO_NATIVE_CPU_PC_PROFILE"))
    m_native_pc_profile_path = path;
  if (std::getenv("MODERNGEKKO_NATIVE_CPU_PC_PROFILE_EVERY"))
    m_native_pc_profile_mask = 0;
  m_native_copy_enabled = std::getenv("MODERNGEKKO_NATIVE_COPY_DISABLE") == nullptr;
  m_native_branch_batching = std::getenv("MODERNGEKKO_NATIVE_DISABLE_BRANCH_BATCH") == nullptr;
  m_lockstep_verifier = std::make_unique<StaticRecompLockstep::StaticRecompLockstepVerifier>(*this);
  m_lockstep_verifier->Init();

  const bool disable_jit_fallback = std::getenv("MODERNGEKKO_STATICRECOMP_NO_JIT") != nullptr;
#ifdef _M_ARM_64
  if (!disable_jit_fallback)
    m_fallback_jit = std::make_unique<JitArm64>(m_system);
#elif defined(_M_X86_64)
  if (!disable_jit_fallback)
    m_fallback_jit = std::make_unique<Jit64>(m_system);
#endif
  if (m_fallback_jit)
    m_fallback_jit->Init();
}

void StaticRecompCore::Shutdown()
{
  if (!m_native_pc_profile_path.empty())
  {
    if (auto* file = std::fopen(m_native_pc_profile_path.c_str(), "wb"))
    {
      std::fprintf(file, "pc,samples\n");
      for (const auto& [pc, count] : m_native_pc_samples)
        std::fprintf(file, "%08x,%llu\n", pc, static_cast<unsigned long long>(count));
      std::fclose(file);
    }
    const std::string fallback_path = m_native_pc_profile_path + ".fallback.csv";
    if (auto* file = std::fopen(fallback_path.c_str(), "wb"))
    {
      std::fprintf(file, "kind,pc,instructions\n");
      for (const auto& [pc, count] : m_native_fallback_samples)
        std::fprintf(file, "uncovered,%08x,%llu\n", pc, static_cast<unsigned long long>(count));
      for (const auto& [pc, count] : m_native_hook_samples)
        std::fprintf(file, "hook,%08x,%llu\n", pc, static_cast<unsigned long long>(count));
      std::fclose(file);
    }
  }
  g_static_recomp_core = nullptr;
  StaticRecompCpuHost::g_cpu_host = nullptr;
  if (m_machine.sdk)
    m_machine.sdk->PrintStats();
  if (m_machine.gp)
    m_machine.gp->PrintStats();
  if (m_machine.vi)
    m_machine.vi->PrintStats();
  if (m_machine.pi)
    m_machine.pi->PrintStats();
  if (m_machine.exi)
    m_machine.exi->PrintStats();
  if (m_machine.clock)
    m_machine.clock->PrintStats();
  m_machine.vi.reset();
  m_machine.gp.reset();
  m_machine.sdk.reset();
  m_machine.exi.reset();
  m_machine.pi.reset();
  m_machine.clock.reset();
  std::fprintf(stderr,
               "[staticrecomp] shutdown: native=%llu fallback=%llu native_exc=%llu hook_fb=%llu "
               "smc_failed=%u verifications=%llu reverify_events=%llu jit_fallback_runs=%llu "
               "native_kernel_calls=%llu native_cache_instructions=%llu native_cache_batches=%llu "
               "native_copy_bytes=%llu native_copy_batches=%llu native_copy_enabled=%d "
               "native_gather_register_instructions=%llu native_scratch_register_instructions=%llu "
               "native_decrementer_reads=%llu native_decrementer_writes=%llu "
               "native_dma_register_instructions=%llu native_system_register_instructions=%llu "
               "branch_batching_enabled=%d bursts=%llu sync_in=%llu sync_out=%llu sync_exceptions_native=%llu physical_accesses=%llu\n",
               (unsigned long long)m_native_dispatches, (unsigned long long)m_fallback_steps,
               (unsigned long long)m_native_exceptions,
               (unsigned long long)m_hook_fallback_instructions, m_code_guard.Failed(),
               (unsigned long long)m_code_guard.Verifications(), (unsigned long long)m_code_guard.ReverifyEvents(),
               (unsigned long long)m_fallback_jit_runs,
               (unsigned long long)m_native_kernel_calls,
               (unsigned long long)m_native_cache_instructions,
               (unsigned long long)m_native_cache_batches,
               (unsigned long long)m_native_copy_bytes,
               (unsigned long long)m_native_copy_batches, m_native_copy_enabled ? 1 : 0,
               (unsigned long long)m_native_gather_register_instructions,
               (unsigned long long)m_native_scratch_register_instructions,
               (unsigned long long)m_native_decrementer_reads,
               (unsigned long long)m_native_decrementer_writes,
               (unsigned long long)m_native_dma_register_instructions,
               (unsigned long long)m_native_system_register_instructions,
               m_native_branch_batching && m_module && m_module->cpu_abi_version >= 3 ? 1 : 0,
               (unsigned long long)m_bursts, (unsigned long long)m_sync_in,
               (unsigned long long)m_sync_out,
               (unsigned long long)m_native_sync_exceptions_delivered, (unsigned long long)m_native_physical_accesses);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: shutdown. native_dispatches={} fallback_steps={} "
                 "native_exceptions={} hook_fallback_instructions={} smc_failed_chunks={} "
                 "verifications={} reverify_events={}",
                 m_native_dispatches, m_fallback_steps, m_native_exceptions,
                 m_hook_fallback_instructions, m_code_guard.Failed(), m_code_guard.Verifications(),
                 m_code_guard.ReverifyEvents());
  m_lockstep_verifier.reset();
  m_block_cache.Shutdown();
  m_module = nullptr;
  if (m_library.IsOpen())
    m_library.Close();

  if (m_fallback_jit)
  {
    m_fallback_jit->Shutdown();
    m_fallback_jit.reset();
  }
}

void StaticRecompCore::LoadModule()
{
  if (m_module_source.kind == StaticRecompModuleSource::Kind::None)
  {
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: no explicit module source; interpreter-only.");
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  std::string path = m_module_source.path;
  const StaticRecompModuleDesc* desc = nullptr;
  if (m_module_source.kind == StaticRecompModuleSource::Kind::AttachedDescriptor)
  {
    desc = m_module_source.descriptor;
  }
  else
  {
    if (path.empty() || !m_library.Open(path))
    {
      ERROR_LOG_FMT(POWERPC, "StaticRecomp: failed to open explicit module '{}'.", path);
      return;
    }
    const auto get_module = reinterpret_cast<StaticRecompGetModuleFn>(
        m_library.Symbol(STATICRECOMP_GET_MODULE_SYMBOL));
    desc = get_module ? get_module() : nullptr;
  }

  const auto reject = [&](const std::string& why) {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: rejecting module '{}': {}. Interpreter-only.", path, why);
    m_module = nullptr;
    if (m_library.IsOpen())
      m_library.Close();
  };

  if (!desc)
    return reject("missing or null " STATICRECOMP_GET_MODULE_SYMBOL);
  if (const std::optional<std::string> error = StaticRecompModuleLoader::ValidateModule(*desc, game_id))
    return reject(*error);

  m_module = desc;
  m_module_active = (desc != nullptr);
  m_code_guard.Bind(desc);

  std::fprintf(stderr, "[staticrecomp] module loaded: %s entry=0x%08X\n", path.c_str(),
               desc->entry_point);
  m_machine.vi.reset();
  m_machine.gp.reset();
  m_machine.sdk.reset();
  using NativeHooksFn = u32 (*)(const u32**, unsigned char**, int**);
  using MenuVersionFn=u32 (*)();
  const auto menu_version=m_library.IsOpen()?reinterpret_cast<MenuVersionFn>(m_library.Symbol("staticrecomp_menu_extension_version")):nullptr;
  m_menu_extension_version=menu_version?menu_version():0;
  m_guest.host_call=m_menu_extension_version>=1&&m_menu_extension_version<=3?HookMenuExtension:nullptr;
  if(m_guest.host_call)std::fprintf(stderr,"[pc_menu] original-controller extension module v%u\n",m_menu_extension_version);
  const auto native_hooks = m_library.IsOpen() ?
      reinterpret_cast<NativeHooksFn>(m_library.Symbol("staticrecomp_native_hooks")) :
      nullptr;
  // Phase F: the event clock and the interrupt controller come first (the SDK
  // layer registers its audio DMA event at bind time); the other native modules schedule on the clock and raise lines on the PI.
  StaticRecompNativeAudio::NativeAxWii::SetCoefficientPaths(
      {File::GetUserPath(D_GCUSER_IDX) + "dsp_coef.bin", File::GetSysDirectory() + "/GC/dsp_coef.bin"});
  StaticRecompNativeClockChassis::Install(m_system);
  m_machine.clock = std::make_unique<StaticRecompNativeClock::NativeClock>();
  StaticRecompNativePiChassis::Install(m_system);
  m_machine.pi = std::make_unique<StaticRecompNativePi::NativePi>();
  m_machine.exi = std::make_unique<StaticRecompNativeExi::NativeExi>(
      SConfig::GetInstance().GetGameID(),
      reinterpret_cast<const u8*>(&m_system.GetSRAM()), 0x44,
      StaticRecompNativeExi::NativeExi::Paths{File::GetUserPath(D_WIIROOT_IDX) + "sram.bin",
                                               File::GetSysDirectory() + GC_SYS_DIR + DIR_SEP});
  if (native_hooks)
  {
    const u32* addresses = nullptr;
    unsigned char* flags = nullptr;
    int* any = nullptr;
    const u32 count = native_hooks(&addresses, &flags, &any);
    std::fprintf(stderr, "[staticrecomp] module native hooks: %u\n", count);
    if (count && addresses && flags && any)
    {
      StaticRecompNativeSdkChassis::Install(m_system);
      m_machine.sdk = std::make_unique<StaticRecompNativeSdk::NativeSdk>();
      m_machine.sdk->Bind(count, addresses, flags, any);
    }
  }
  // Phase E: the GX stream front (active only with the NativeGX renderer).
  StaticRecompNativeGpChassis::Install(m_system);
  m_machine.gp = std::make_unique<StaticRecompNativeGp::NativeGp>();
  // Phase D: the video interface, once the native PAD library replaces the SI poll.
  if (m_machine.sdk && m_machine.sdk->PadNative())
  {
    StaticRecompNativeViChassis::Install(m_system);
    m_machine.vi = std::make_unique<StaticRecompNativeVi::NativeVi>();
  }
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: loaded module '{}' (game_id={} entry=0x{:08X} "
                 "code_ranges={} smc_ranges={})",
                 path, desc->game_id, desc->entry_point, desc->num_code_ranges,
                 desc->num_smc_ranges);
}

namespace
{
// The chassis's PointerWrap behind our serializer interface: same bytes, the
// chassis's buffer and its measure/bounds handling.
class PointerWrapStream final : public StaticRecompState::StateStream
{
public:
  // The chassis's Measure pass is a write that only advances the pointer: the
  // modules take their write path and every byte still goes through the
  // PointerWrap, so the measured size equals the written one. Our Measure mode
  // is only the bail-out, which also stops the chassis.
  explicit PointerWrapStream(PointerWrap& p)
      : StateStream(p.IsReadMode() ? Mode::Read : Mode::Write), m_p(p)
  {
  }
  void Bytes(void* data, size_t size) override
  {
    if (IsMeasureMode())
      return;
    m_p.DoArray(static_cast<u8*>(data), static_cast<u32>(size));
    if (m_p.IsReadMode() == false && m_p.IsWriteMode() == false && !m_was_measure)
      StateStream::SetMeasureMode();  // the chassis ran out of buffer while writing/reading
  }
  void SetMeasureMode() override
  {
    StateStream::SetMeasureMode();
    if (!m_was_measure)
      m_p.SetMeasureMode();
  }

private:
  PointerWrap& m_p;
  const bool m_was_measure = m_p.IsMeasureMode();
};
}  // namespace

void StaticRecompCore::DoExtraState(PointerWrap& p, bool present)
{
  PointerWrapStream stream(p);
  DoNativeState(stream, present);
}

void StaticRecompCore::DoNativeState(StaticRecompState::StateStream& p, bool present)
{
  m_machine.DoState(p, present);
  if (p.IsReadMode())
    UpdateNativeBats();
}

u32* StaticRecompCore::Sprs()
{
  return m_authority == StateAuthority::Guest ? m_regs.spr : m_system.GetPPCState().spr;
}

int& StaticRecompCore::Downcount()
{
  return m_authority == StateAuthority::Guest ? m_regs.downcount : m_system.GetPPCState().downcount;
}

u32& StaticRecompCore::PendingExceptions()
{
  return m_authority == StateAuthority::Guest ? m_regs.exceptions :
                                               m_system.GetPPCState().Exceptions;
}

void StaticRecompCore::FoldChassisExceptions()
{
  if (m_authority != StateAuthority::Guest)
    return;
  auto& ppc = m_system.GetPPCState();
  m_regs.exceptions |= ppc.Exceptions;
  ppc.Exceptions = 0;
}

void StaticRecompCore::UpdateNativeBats()
{
  const u32* spr = Sprs();
  auto& memory = m_system.GetMemory();
  m_native_dbat.Update(spr, ((spr[SPR_HID4] >> 25) & 1) != 0, memory.GetRamSizeReal(),
                       memory.GetExRamSizeReal(), memory.GetL1CacheSize());
}

void StaticRecompCore::SeedNativeDevicesForNativeState()
{
  m_machine.SeedForCheckpoint();
}

void StaticRecompCore::ClearCache()
{
  if (m_fallback_jit)
    m_fallback_jit->ClearCache();

  if (!m_module)
    return;
  m_code_guard.ResetAll();
}
