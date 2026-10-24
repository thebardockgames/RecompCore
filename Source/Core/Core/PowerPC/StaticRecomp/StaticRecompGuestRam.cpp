// RecompCore: StaticRecomp CPU core - the port's guest memory (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRam.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/mman.h>
#endif

namespace StaticRecompGuestRam
{
namespace
{
u32 NextPowerOfTwo(u32 value)
{
  u32 result = 1;
  while (result < value)
    result <<= 1;
  return result;
}
}  // namespace

GuestRam GuestRam::s_instance;

GuestRam::~GuestRam()
{
  Release();
}

u8* GuestRam::AllocatePages(size_t size)
{
#ifdef _WIN32
  return static_cast<u8*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
  void* pages = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return pages == MAP_FAILED ? nullptr : static_cast<u8*>(pages);
#endif
}

void GuestRam::FreePages(u8* pages, size_t size)
{
  if (!pages)
    return;
#ifdef _WIN32
  (void)size;
  VirtualFree(pages, 0, MEM_RELEASE);
#else
  munmap(pages, size);
#endif
}

bool GuestRam::Allocate(u32 mem1_real, u32 mem2_real, u32 l1_size)
{
  const u32 mem1_alloc = NextPowerOfTwo(mem1_real);
  const u32 mem2_alloc = NextPowerOfTwo(mem2_real);
  if (m_owned && m_mem1 && m_mem1_alloc == mem1_alloc && m_mem2_alloc == mem2_alloc &&
      m_l1_size == l1_size)
  {
    m_mem1_real = mem1_real;
    m_mem2_real = mem2_real;
    return true;
  }
  Release();
  m_mem1 = AllocatePages(mem1_alloc);
  m_mem2 = AllocatePages(mem2_alloc);
  m_l1 = AllocatePages(l1_size);
  if (!m_mem1 || !m_mem2 || !m_l1)
  {
    std::fprintf(stderr, "[guest_ram] allocation failed (MEM1 %u, MEM2 %u, L1 %u bytes)\n",
                 mem1_alloc, mem2_alloc, l1_size);
    FreePages(m_mem1, mem1_alloc);
    FreePages(m_mem2, mem2_alloc);
    FreePages(m_l1, l1_size);
    m_mem1 = m_mem2 = m_l1 = nullptr;
    return false;
  }
  m_mem1_real = mem1_real;
  m_mem1_alloc = mem1_alloc;
  m_mem2_real = mem2_real;
  m_mem2_alloc = mem2_alloc;
  m_l1_size = l1_size;
  m_owned = true;
  return true;
}

void GuestRam::Adopt(u8* mem1, u32 mem1_real, u8* mem2, u32 mem2_real, u8* l1, u32 l1_size)
{
  Release();
  m_mem1 = mem1;
  m_mem1_real = mem1_real;
  m_mem1_alloc = NextPowerOfTwo(mem1_real);
  m_mem2 = mem2;
  m_mem2_real = mem2_real;
  m_mem2_alloc = NextPowerOfTwo(mem2_real);
  m_l1 = l1;
  m_l1_size = l1_size;
  m_owned = false;
}

void GuestRam::Release()
{
  if (m_owned)
  {
    FreePages(m_mem1, m_mem1_alloc);
    FreePages(m_mem2, m_mem2_alloc);
    FreePages(m_l1, m_l1_size);
  }
  m_mem1 = m_mem2 = m_l1 = nullptr;
  m_mem1_real = m_mem1_alloc = m_mem2_real = m_mem2_alloc = m_l1_size = 0;
  m_owned = false;
}

void GuestRam::Clear()
{
  if (m_mem1)
    std::memset(m_mem1, 0, m_mem1_alloc);
  if (m_mem2)
    std::memset(m_mem2, 0, m_mem2_alloc);
  if (m_l1)
    std::memset(m_l1, 0, m_l1_size);
}
}  // namespace StaticRecompGuestRam
