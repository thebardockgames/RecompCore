// RecompCore: StaticRecomp CPU core - loading a recompiled module (phase L2).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompModuleLoader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <type_traits>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace StaticRecompModuleLoader
{
namespace
{
using u32 = std::uint32_t;

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

std::string Format(const char* pattern)
{
  return pattern;
}

template <typename T, typename... Rest>
std::string Format(const char* pattern, const T& value, const Rest&... rest)
{
  const char* hole = std::strstr(pattern, "{}");
  if (!hole)
    return pattern;
  std::string text(pattern, hole);
  if constexpr (std::is_arithmetic_v<T>)
    text += std::to_string(value);
  else
    text += std::string(value);
  return text + Format(hole + 2, rest...);
}

std::optional<std::string> Reject(std::string why)
{
  return why;
}
}  // namespace

ModuleLibrary::~ModuleLibrary()
{
  Close();
}

bool ModuleLibrary::Open(const std::string& path)
{
  Close();
  std::error_code error;
  if (!std::filesystem::exists(path, error))
    return false;
#ifdef _WIN32
  m_handle = LoadLibraryW(std::filesystem::path(path).wstring().c_str());
#else
  m_handle = dlopen(path.c_str(), RTLD_NOW);
#endif
  return m_handle != nullptr;
}

void ModuleLibrary::Close()
{
  if (!m_handle)
    return;
#ifdef _WIN32
  FreeLibrary(static_cast<HMODULE>(m_handle));
#else
  dlclose(m_handle);
#endif
  m_handle = nullptr;
}

void* ModuleLibrary::Symbol(const char* name) const
{
  if (!m_handle)
    return nullptr;
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
  return dlsym(m_handle, name);
#endif
}

std::optional<std::string> ValidateModule(const StaticRecompModuleDesc& desc, const std::string& game_id)
{
  if (desc.abi_version != STATICRECOMP_ABI_VERSION)
    return Reject(Format("abi_version {} != {}", desc.abi_version, STATICRECOMP_ABI_VERSION));
  // CPU ABIs only append fields: older modules see an exact prefix (v2 ends
  // before dispatch_budget, v3 before chunk_state, v4 before gather_buffer).
  size_t expected_state_size = sizeof(CPUState);
  if (desc.cpu_abi_version == 2u)
    expected_state_size = offsetof(CPUState, dispatch_budget);
  else if (desc.cpu_abi_version == 3u)
    expected_state_size = offsetof(CPUState, chunk_state);
  else if (desc.cpu_abi_version == 4u)
    expected_state_size = offsetof(CPUState, gather_buffer);
  else if (desc.cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION)
    return Reject(Format("cpu_abi_version {} != {}", desc.cpu_abi_version,
                              GXRUNTIME_CPU_ABI_VERSION));
  if (desc.cpu_state_size != expected_state_size)
    return Reject(Format("cpu_state_size {} != expected {} for CPU ABI {}",
                              desc.cpu_state_size, expected_state_size, desc.cpu_abi_version));
  if (!desc.dispatch || !desc.code_ranges || desc.num_code_ranges == 0)
    return Reject("no dispatch entry or empty code ranges");
  if (!std::memchr(desc.game_id, '\0', sizeof(desc.game_id)) || desc.game_id[0] == '\0')
    return Reject("invalid game_id");
  if (!RangesAreSorted(desc.code_ranges, desc.num_code_ranges))
    return Reject("malformed or overlapping code ranges");
  if (desc.num_smc_ranges != 0 && !RangesAreSorted(desc.smc_ranges, desc.num_smc_ranges))
    return Reject("malformed or overlapping SMC ranges");
  if (!desc.chunk_ranges || desc.num_chunk_ranges == 0 || !desc.chunk_hashes)
    return Reject("no chunk ranges/hashes (required for the SMC guard)");
  if (!ChunksTileCode(desc))
    return Reject("chunk ranges do not exactly tile code ranges");
  if (!AddressIsCovered(desc.code_ranges, desc.num_code_ranges, desc.entry_point))
    return Reject("entry point is not covered by the module");
  if (!game_id.empty() && game_id != desc.game_id)
    return Reject(Format("module game_id '{}' != running game '{}'", desc.game_id, game_id));

  return std::nullopt;
}
}  // namespace StaticRecompModuleLoader
