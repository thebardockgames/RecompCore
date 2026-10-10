// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/Config/Config.h"
#include "Common/DynamicLibrary.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
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
bool RangesAreSorted(const StaticRecompRange* ranges, u32 count)
{
  if (!ranges || count == 0)
    return false;
  for (u32 i = 0; i < count; ++i)
  {
    if (ranges[i].start >= ranges[i].end ||
        (i != 0 && ranges[i - 1].end > ranges[i].start))
      return false;
  }
  return true;
}

bool AddressIsCovered(const StaticRecompRange* ranges, u32 count, u32 address)
{
  for (u32 i = 0; i < count; ++i)
  {
    if (address >= ranges[i].start && address < ranges[i].end)
      return true;
  }
  return false;
}

bool ChunksTileCode(const StaticRecompModuleDesc& desc)
{
  if (!RangesAreSorted(desc.chunk_ranges, desc.num_chunk_ranges) || !desc.chunk_hashes)
    return false;
  u32 chunk = 0;
  for (u32 code = 0; code < desc.num_code_ranges; ++code)
  {
    u32 cursor = desc.code_ranges[code].start;
    while (chunk < desc.num_chunk_ranges && desc.chunk_ranges[chunk].start < desc.code_ranges[code].end)
    {
      if (desc.chunk_ranges[chunk].start != cursor ||
          desc.chunk_ranges[chunk].end > desc.code_ranges[code].end)
        return false;
      cursor = desc.chunk_ranges[chunk++].end;
    }
    if (cursor != desc.code_ranges[code].end)
      return false;
  }
  return chunk == desc.num_chunk_ranges;
}
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
  if (m_native_sdk)
    m_native_sdk->PrintStats();
  m_native_sdk.reset();
  std::fprintf(stderr,
               "[staticrecomp] shutdown: native=%llu fallback=%llu native_exc=%llu hook_fb=%llu "
               "smc_failed=%u verifications=%llu reverify_events=%llu jit_fallback_runs=%llu "
               "native_kernel_calls=%llu native_cache_instructions=%llu native_cache_batches=%llu "
               "native_copy_bytes=%llu native_copy_batches=%llu native_copy_enabled=%d "
               "native_gather_register_instructions=%llu native_scratch_register_instructions=%llu "
               "native_decrementer_reads=%llu native_decrementer_writes=%llu "
               "native_dma_register_instructions=%llu native_system_register_instructions=%llu "
               "branch_batching_enabled=%d\n",
               (unsigned long long)m_native_dispatches, (unsigned long long)m_fallback_steps,
               (unsigned long long)m_native_exceptions,
               (unsigned long long)m_hook_fallback_instructions, m_failed_chunks,
               (unsigned long long)m_verifications, (unsigned long long)m_reverify_events,
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
               m_native_branch_batching && m_module && m_module->cpu_abi_version >= 3 ? 1 : 0);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: shutdown. native_dispatches={} fallback_steps={} "
                 "native_exceptions={} hook_fallback_instructions={} smc_failed_chunks={} "
                 "verifications={} reverify_events={}",
                 m_native_dispatches, m_fallback_steps, m_native_exceptions,
                 m_hook_fallback_instructions, m_failed_chunks, m_verifications,
                 m_reverify_events);
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
    if (path.empty() || !File::Exists(path) || !m_library.Open(path.c_str()))
    {
      ERROR_LOG_FMT(POWERPC, "StaticRecomp: failed to open explicit module '{}'.", path);
      return;
    }
    const auto get_module = reinterpret_cast<StaticRecompGetModuleFn>(
        m_library.GetSymbolAddress(STATICRECOMP_GET_MODULE_SYMBOL));
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
  if (desc->abi_version != STATICRECOMP_ABI_VERSION)
    return reject(fmt::format("abi_version {} != {}", desc->abi_version, STATICRECOMP_ABI_VERSION));
  // CPU ABIs only append fields: older modules see an exact prefix (v2 ends
  // before dispatch_budget, v3 before chunk_state).
  size_t expected_state_size = sizeof(CPUState);
  if (desc->cpu_abi_version == 2u)
    expected_state_size = offsetof(CPUState, dispatch_budget);
  else if (desc->cpu_abi_version == 3u)
    expected_state_size = offsetof(CPUState, chunk_state);
  else if (desc->cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION)
    return reject(fmt::format("cpu_abi_version {} != {}", desc->cpu_abi_version,
                              GXRUNTIME_CPU_ABI_VERSION));
  if (desc->cpu_state_size != expected_state_size)
    return reject(fmt::format("cpu_state_size {} != expected {} for CPU ABI {}",
                              desc->cpu_state_size, expected_state_size, desc->cpu_abi_version));
  if (!desc->dispatch || !desc->code_ranges || desc->num_code_ranges == 0)
    return reject("no dispatch entry or empty code ranges");
  if (!std::memchr(desc->game_id, '\0', sizeof(desc->game_id)) || desc->game_id[0] == '\0')
    return reject("invalid game_id");
  if (!RangesAreSorted(desc->code_ranges, desc->num_code_ranges))
    return reject("malformed or overlapping code ranges");
  if (desc->num_smc_ranges != 0 && !RangesAreSorted(desc->smc_ranges, desc->num_smc_ranges))
    return reject("malformed or overlapping SMC ranges");
  if (!desc->chunk_ranges || desc->num_chunk_ranges == 0 || !desc->chunk_hashes)
    return reject("no chunk ranges/hashes (required for the SMC guard)");
  if (!ChunksTileCode(*desc))
    return reject("chunk ranges do not exactly tile code ranges");
  if (!AddressIsCovered(desc->code_ranges, desc->num_code_ranges, desc->entry_point))
    return reject("entry point is not covered by the module");
  if (!game_id.empty() && game_id != desc->game_id)
    return reject(fmt::format("module game_id '{}' != running game '{}'", desc->game_id, game_id));

  m_module = desc;
  m_module_active = (desc != nullptr);
  m_chunk_state.assign(desc->num_chunk_ranges, CHUNK_UNVERIFIED);
  m_failed_chunks = 0;
  m_lookup_ram_size = 0;
  m_lookup_exram_size = 0;
  m_chunk_lookup_table.clear();

  std::fprintf(stderr, "[staticrecomp] module loaded: %s entry=0x%08X\n", path.c_str(),
               desc->entry_point);
  m_native_sdk.reset();
  using NativeHooksFn = u32 (*)(const u32**, unsigned char**, int**);
  const auto native_hooks = m_library.IsOpen() ?
      reinterpret_cast<NativeHooksFn>(m_library.GetSymbolAddress("staticrecomp_native_hooks")) :
      nullptr;
  if (native_hooks)
  {
    const u32* addresses = nullptr;
    unsigned char* flags = nullptr;
    int* any = nullptr;
    const u32 count = native_hooks(&addresses, &flags, &any);
    std::fprintf(stderr, "[staticrecomp] module native hooks: %u\n", count);
    if (count && addresses && flags && any)
    {
      m_native_sdk = std::make_unique<StaticRecompNativeSdk::NativeSdk>(m_system);
      m_native_sdk->Bind(count, addresses, flags, any);
    }
  }
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: loaded module '{}' (game_id={} entry=0x{:08X} "
                 "code_ranges={} smc_ranges={})",
                 path, desc->game_id, desc->entry_point, desc->num_code_ranges,
                 desc->num_smc_ranges);
}

void StaticRecompCore::DoExtraState(PointerWrap& p, bool present)
{
  // Version 1: the native SDK layer (pending native interrupts, native audio).
  u32 version = m_native_sdk ? 1 : 0;
  if (present)
    p.Do(version);
  if (m_native_sdk)
  {
    m_native_sdk->DoState(p, present && version == 1);
  }
  else if (p.IsReadMode() && present && version != 0)
  {
    std::fprintf(stderr, "[staticrecomp] state has native SDK data but no native module is loaded\n");
    p.SetMeasureMode();
  }
}

void StaticRecompCore::ClearCache()
{
  if (m_fallback_jit)
    m_fallback_jit->ClearCache();

  if (!m_module)
    return;
  std::fill(m_chunk_state.begin(), m_chunk_state.end(), u8{CHUNK_UNVERIFIED});
  m_failed_chunks = 0;
  ++m_reverify_events;
}
