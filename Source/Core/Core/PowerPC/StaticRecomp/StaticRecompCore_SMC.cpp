// RecompCore: StaticRecomp CPU core - SMC and chunk validation.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompAddress.h"
#include "Core/System.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/JitInterface.h"
#include "Common/Logging/Log.h"
#include <algorithm>
#include <cstdio>

int StaticRecompCore::GetAddressLookupIndex(u32 address) const
{
  return StaticRecompAddress::LookupIndex(address, m_lookup_ram_size, m_lookup_exram_size);
}

void StaticRecompCore::InitLookupTable(u32 ram_size, u32 exram_size)
{
  if (m_lookup_ram_size == ram_size && m_lookup_exram_size == exram_size)
    return;

  m_lookup_ram_size = ram_size;
  m_lookup_exram_size = exram_size;

  if (!m_module)
  {
    m_chunk_lookup_table.clear();
    return;
  }

  u32 total_instructions = (2 * ram_size + exram_size) >> 2;
  m_chunk_lookup_table.assign(total_instructions, -1);

  for (u32 i = 0; i < m_module->num_chunk_ranges; ++i)
  {
    const auto& chunk = m_module->chunk_ranges[i];
    int start_idx = GetAddressLookupIndex(chunk.start);
    int end_idx = GetAddressLookupIndex(chunk.end - 1);

    if (start_idx >= 0 && end_idx >= start_idx)
    {
      for (int idx = start_idx; idx <= end_idx; ++idx)
      {
        m_chunk_lookup_table[idx] = static_cast<int>(i);
      }
    }
  }
}

int StaticRecompCore::ChunkIndexOf(u32 address) const
{
  if (!m_module_active || m_chunk_lookup_table.empty())
    return -1;

  int idx = GetAddressLookupIndex(address);
  if (idx < 0 || idx >= static_cast<int>(m_chunk_lookup_table.size()))
    return -1;

  return m_chunk_lookup_table[idx];
}

bool StaticRecompCore::FastDispatchableAt(u32 address) const
{
  if (address < m_lookup_ram_size && (m_guest.msr & 0x20))
    return false;  // A low effective PC under IR may map to different bytes.
  const int index = ChunkIndexOf(address);
  return index >= 0 && m_chunk_state[index] == CHUNK_VERIFIED;
}

bool StaticRecompCore::DispatchableAt(u32 address)
{
  if (address < m_lookup_ram_size && m_system.GetPPCState().msr.IR)
    return false;
  const int index = ChunkIndexOf(address);
  if (index < 0)
    return false;
  if (m_chunk_state[index] == CHUNK_UNVERIFIED)
    VerifyChunk(static_cast<u32>(index));
  return m_chunk_state[index] == CHUNK_VERIFIED;
}

void StaticRecompCore::VerifyChunk(u32 index)
{
  const auto& chunk = m_module->chunk_ranges[index];
  auto& memory = m_system.GetMemory();
  const u32 ram_size = memory.GetRamSizeReal();
  const u32 length = chunk.end - chunk.start;
  const auto offset = StaticRecompAddress::RAMOffset(chunk.start, length, ram_size);
  ++m_verifications;

  if (!offset)
  {
    m_chunk_state[index] = CHUNK_FAILED;
    ++m_failed_chunks;
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: chunk [0x{:08X},0x{:08X}) outside guest RAM",
                  chunk.start, chunk.end);
    return;
  }

  // FNV-1a 64, matching gen_module_tables.py.
  const u8* bytes = memory.GetRAM() + *offset;
  u64 hash = 0xCBF29CE484222325ull;
  for (u32 i = 0; i < length; ++i)
  {
    hash ^= bytes[i];
    hash *= 0x100000001B3ull;
  }

  if (hash == m_module->chunk_hashes[index])
  {
    m_chunk_state[index] = CHUNK_VERIFIED;
  }
  else
  {
    m_chunk_state[index] = CHUNK_FAILED;
    ++m_failed_chunks;
    std::fprintf(stderr,
                 "[staticrecomp] SMC: chunk [0x%08X,0x%08X) hash mismatch; interpreter until "
                 "next invalidation (%u failed)\n",
                 chunk.start, chunk.end, m_failed_chunks);
    WARN_LOG_FMT(POWERPC,
                 "StaticRecomp: chunk [0x{:08X},0x{:08X}) failed verification (guest code "
                 "differs from module); interpreter until next invalidation",
                 chunk.start, chunk.end);
  }
}

void StaticRecompCore::OnICacheInvalidate(u32 address, u32 length, u32 physical_address)
{
  if (m_fallback_jit)
  {
    m_fallback_jit->GetBlockCache()->InvalidateICache(address, length, false);
  }

  if (!m_module_active || length == 0)
    return;
  StaticRecompAddress::ForEachInvalidatedChunk(
      {m_module->chunk_ranges, m_module->num_chunk_ranges}, address, length,
      physical_address, m_lookup_ram_size, [&](size_t i) {
        if (m_chunk_state[i] != CHUNK_UNVERIFIED)
        {
          if (m_chunk_state[i] == CHUNK_FAILED)
            --m_failed_chunks;
          m_chunk_state[i] = CHUNK_UNVERIFIED;
          ++m_reverify_events;
        }
      });
}
