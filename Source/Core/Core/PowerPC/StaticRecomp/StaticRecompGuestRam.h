// RecompCore: StaticRecomp CPU core - the port's guest memory (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// MEM1, MEM2 and the locked L1 cache of the console, allocated once per
// process by the runtime (page-aligned, zeroed, power-of-two sizes so the
// address masks of the module ABI and of the chassis work unchanged). The
// chassis's MemoryManager maps its regions onto these buffers while it exists
// (`SetExternalBuffers`): the recompiled code, the native modules and the
// chassis all see the same bytes, but the owner is this class, not the arena.
//
// A core that runs without the runtime having lent memory (tools, tests)
// adopts the chassis's buffers instead (`Adopt`), non-owning.

#pragma once

#include <cstddef>
#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompGuestRam
{
constexpr u32 kMem1SizeRetail = 0x01800000;  // 24 MiB
constexpr u32 kMem2SizeRetail = 0x04000000;  // 64 MiB
constexpr u32 kL1CacheSize = 0x00040000;     // the chassis's locked-cache window (256 KiB)

class GuestRam
{
public:
  // Inline and without a function-local static: it is read on every external
  // memory access of the generated code (locked cache, real-mode RAM).
  static GuestRam& Instance() { return s_instance; }

  // Allocates (or reallocates when the layout changes). The real sizes are
  // the console's; the buffers are the next power of two, as the chassis.
  bool Allocate(u32 mem1_real = kMem1SizeRetail, u32 mem2_real = kMem2SizeRetail,
                u32 l1_size = kL1CacheSize);
  // Uses buffers someone else owns (the chassis's, when nothing was lent).
  void Adopt(u8* mem1, u32 mem1_real, u8* mem2, u32 mem2_real, u8* l1, u32 l1_size);
  void Release();

  bool Allocated() const { return m_mem1 != nullptr; }
  bool Owned() const { return m_owned; }

  u8* Mem1() const { return m_mem1; }
  u32 Mem1Size() const { return m_mem1_real; }      // what the console has
  u32 Mem1Alloc() const { return m_mem1_alloc; }    // what is mapped (power of two)
  u8* Mem2() const { return m_mem2; }
  u32 Mem2Size() const { return m_mem2_real; }
  u32 Mem2Alloc() const { return m_mem2_alloc; }
  u8* L1() const { return m_l1; }
  u32 L1Size() const { return m_l1_size; }

  void Clear();

private:
  GuestRam() = default;
  ~GuestRam();
  GuestRam(const GuestRam&) = delete;
  GuestRam& operator=(const GuestRam&) = delete;

  static u8* AllocatePages(size_t size);
  static void FreePages(u8* pages, size_t size);

  u8* m_mem1 = nullptr;
  u8* m_mem2 = nullptr;
  u8* m_l1 = nullptr;
  u32 m_mem1_real = 0, m_mem1_alloc = 0;
  u32 m_mem2_real = 0, m_mem2_alloc = 0;
  u32 m_l1_size = 0;
  bool m_owned = false;
  static GuestRam s_instance;
};
}  // namespace StaticRecompGuestRam
