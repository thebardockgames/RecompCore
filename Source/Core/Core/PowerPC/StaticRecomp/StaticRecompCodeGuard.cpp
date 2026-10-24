// RecompCore: StaticRecomp CPU core - the module's code guard (phase L3).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCodeGuard.h"

#include <algorithm>
#include <cstdio>

#include "Core/PowerPC/StaticRecomp/StaticRecompAddress.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRam.h"

namespace StaticRecompCodeGuard
{
void CodeGuard::Bind(const StaticRecompModuleDesc* module)
{
  m_module = module;
  m_state.assign(module ? module->num_chunk_ranges : 0, CHUNK_UNVERIFIED);
  m_failed = 0;
  m_lookup_ram_size = 0;
  m_lookup_exram_size = 0;
  m_lookup.clear();
}

int CodeGuard::LookupIndex(u32 address) const
{
  return StaticRecompAddress::LookupIndex(address, m_lookup_ram_size, m_lookup_exram_size);
}

void CodeGuard::InitLookup(u32 ram_size, u32 exram_size)
{
  if (m_lookup_ram_size == ram_size && m_lookup_exram_size == exram_size)
    return;
  m_lookup_ram_size = ram_size;
  m_lookup_exram_size = exram_size;
  if (!m_module)
  {
    m_lookup.clear();
    return;
  }
  const u32 total_instructions = (2 * ram_size + exram_size) >> 2;
  m_lookup.assign(total_instructions, -1);
  for (u32 i = 0; i < m_module->num_chunk_ranges; ++i)
  {
    const auto& chunk = m_module->chunk_ranges[i];
    const int start_idx = LookupIndex(chunk.start);
    const int end_idx = LookupIndex(chunk.end - 1);
    if (start_idx >= 0 && end_idx >= start_idx)
    {
      for (int idx = start_idx; idx <= end_idx; ++idx)
        m_lookup[idx] = static_cast<int>(i);
    }
  }
}

int CodeGuard::ChunkIndexOf(u32 address, bool active) const
{
  if (!active || m_lookup.empty())
    return -1;
  const int idx = LookupIndex(address);
  if (idx < 0 || idx >= static_cast<int>(m_lookup.size()))
    return -1;
  return m_lookup[idx];
}

bool CodeGuard::FastDispatchable(u32 address, u32 msr, bool active) const
{
  if (address < m_lookup_ram_size && (msr & 0x20))
    return false;  // A low effective PC under IR may map to different bytes.
  const int index = ChunkIndexOf(address, active);
  return index >= 0 && m_state[index] == CHUNK_VERIFIED;
}

bool CodeGuard::Dispatchable(u32 address, bool ir, bool active)
{
  if (address < m_lookup_ram_size && ir)
    return false;
  const int index = ChunkIndexOf(address, active);
  if (index < 0)
    return false;
  if (m_state[index] == CHUNK_UNVERIFIED)
    Verify(static_cast<u32>(index));
  return m_state[index] == CHUNK_VERIFIED;
}

void CodeGuard::Verify(u32 index)
{
  const auto& chunk = m_module->chunk_ranges[index];
  const auto& guest_ram = StaticRecompGuestRam::GuestRam::Instance();
  const u32 ram_size = guest_ram.Mem1Size();
  const u32 length = chunk.end - chunk.start;
  const auto offset = StaticRecompAddress::RAMOffset(chunk.start, length, ram_size);
  ++m_verifications;
  if (!offset)
  {
    m_state[index] = CHUNK_FAILED;
    ++m_failed;
    std::fprintf(stderr, "[staticrecomp] chunk [0x%08X,0x%08X) outside guest RAM\n", chunk.start,
                 chunk.end);
    return;
  }
  // FNV-1a 64, matching gen_module_tables.py.
  const u8* bytes = guest_ram.Mem1() + *offset;
  u64 hash = 0xCBF29CE484222325ull;
  for (u32 i = 0; i < length; ++i)
  {
    hash ^= bytes[i];
    hash *= 0x100000001B3ull;
  }
  if (hash == m_module->chunk_hashes[index])
  {
    m_state[index] = CHUNK_VERIFIED;
  }
  else
  {
    m_state[index] = CHUNK_FAILED;
    ++m_failed;
    std::fprintf(stderr,
                 "[staticrecomp] SMC: chunk [0x%08X,0x%08X) hash mismatch; interpreter until "
                 "next invalidation (%u failed)\n",
                 chunk.start, chunk.end, m_failed);
  }
}

void CodeGuard::Invalidate(u32 address, u32 length, u32 physical_address, bool active)
{
  if (!active || length == 0 || !m_module)
    return;
  StaticRecompAddress::ForEachInvalidatedChunk(
      {m_module->chunk_ranges, m_module->num_chunk_ranges}, address, length, physical_address,
      m_lookup_ram_size, [&](size_t i) {
        if (m_state[i] != CHUNK_UNVERIFIED)
        {
          if (m_state[i] == CHUNK_FAILED)
            --m_failed;
          m_state[i] = CHUNK_UNVERIFIED;
          ++m_reverify_events;
        }
      });
}

void CodeGuard::ResetAll()
{
  std::fill(m_state.begin(), m_state.end(), u8{CHUNK_UNVERIFIED});
  m_failed = 0;
  ++m_reverify_events;
}
}  // namespace StaticRecompCodeGuard
