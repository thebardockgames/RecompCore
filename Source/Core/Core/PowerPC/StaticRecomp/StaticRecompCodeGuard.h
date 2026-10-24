// RecompCore: StaticRecomp CPU core - the module's code guard (phase L3).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// D4 SMC guard, verify-on-entry model. Every chunk of the module starts
// Unverified; the first native dispatch into it hashes its guest RAM against
// the module's recorded hash of the original text. An icache invalidation
// touching a chunk resets it to Unverified (code is invalidated while it is
// being loaded, so invalidation alone must not retire coverage); a hash
// mismatch (real SMC) marks it Failed, outside the module until the next
// invalidation. The chunk of an address comes from a table indexed by the
// instruction's position in MEM1 (cached and uncached) and MEM2.

#pragma once

#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"

namespace StaticRecompCodeGuard
{
enum ChunkState : u8
{
  CHUNK_UNVERIFIED = 0,
  CHUNK_VERIFIED = 1,
  CHUNK_FAILED = 2,
};

class CodeGuard
{
public:
  // A new module (nullptr: none): every chunk Unverified, table rebuilt on
  // the next InitLookup.
  void Bind(const StaticRecompModuleDesc* module);
  void InitLookup(u32 ram_size, u32 exram_size);
  int ChunkIndexOf(u32 address, bool active) const;
  // The chunk is verified (no verification here: the hot path).
  bool FastDispatchable(u32 address, u32 msr, bool active) const;
  // Verifies an Unverified chunk first. `ir`: MSR.IR of the authoritative state.
  bool Dispatchable(u32 address, bool ir, bool active);
  void Invalidate(u32 address, u32 length, u32 physical_address, bool active);
  // Everything Unverified (the core's cache clear).
  void ResetAll();

  u8* StateData() { return m_state.empty() ? nullptr : m_state.data(); }
  u32 Failed() const { return m_failed; }
  u64 Verifications() const { return m_verifications; }
  u64 ReverifyEvents() const { return m_reverify_events; }

private:
  void Verify(u32 index);
  int LookupIndex(u32 address) const;

  const StaticRecompModuleDesc* m_module = nullptr;
  std::vector<u8> m_state;
  u32 m_failed = 0;
  u64 m_verifications = 0;
  u64 m_reverify_events = 0;
  std::vector<int> m_lookup;
  u32 m_lookup_ram_size = 0;
  u32 m_lookup_exram_size = 0;
};
}  // namespace StaticRecompCodeGuard
