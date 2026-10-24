// RecompCore: StaticRecomp CPU core - guest memory accessor (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The game's memory as the native modules see it: MEM1, MEM2 and the locked
// cache behind the pointers the module ABI already carries (CPUState::ram,
// CPUState::exram), with the fixed Wii map the SDK programs in the BATs
// (0x8000_0000/0xC000_0000 -> MEM1, 0x9000_0000/0xD000_0000 -> MEM2, and the
// physical addresses the boot writes). The method names and argument orders
// are those of the chassis's MemoryManager so a module switches by changing
// one declaration; the bytes are big-endian in memory, native in the API.
//
// The buffers are the port's (`StaticRecompGuestRam`, allocated by the runtime
// and lent to the chassis while it exists): Own() returns them.

#pragma once

#include <cstddef>
#include <cstring>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "core/cpu.h"

namespace Core
{
class System;
}

namespace StaticRecompGuestMemory
{
class GuestMemory
{
public:
  GuestMemory() = default;
  GuestMemory(u8* ram, u32 ram_size, u8* exram, u32 exram_size, u8* l1 = nullptr,
              u32 l1_size = 0)
      : m_ram(ram), m_ram_size(ram_size), m_exram(exram), m_exram_size(exram_size), m_l1(l1),
        m_l1_size(l1_size)
  {
  }
  // The module's own view (CPUState::ram/exram).
  explicit GuestMemory(const CPUState& cpu)
      : m_ram(cpu.ram), m_ram_size(cpu.ram_size), m_exram(cpu.exram), m_exram_size(cpu.exram_size)
  {
  }
  // The port's buffers (boot, checkpoints, devices): implemented in the .cpp.
  static GuestMemory Own();

  u8* GetRAM() const { return m_ram; }
  u32 GetRamSizeReal() const { return m_ram_size; }
  // As the chassis's masks: the size rounded up to a power of two, minus one
  // (MEM1 is 24 MiB, so its mask is 0x01FFFFFF, not 0x017FFFFF).
  u32 GetRamMask() const { return PowerOfTwoMask(m_ram_size); }
  u8* GetEXRAM() const { return m_exram; }
  u32 GetExRamSizeReal() const { return m_exram_size; }
  u32 GetExRamMask() const { return PowerOfTwoMask(m_exram_size); }
  static constexpr u32 PowerOfTwoMask(u32 size)
  {
    if (size == 0)
      return 0;
    u32 mask = size - 1;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
    return mask;
  }
  u8* GetL1Cache() const { return m_l1; }
  u32 GetL1CacheSize() const { return m_l1_size; }

  // The chassis's logical map: bits 30-31 ignored (0x8/0xC and 0x9/0xD
  // aliases), MEM1 below its size, MEM2 at 0x1000_0000.
  u8* GetPointerForRange(u32 address, size_t size) const
  {
    const u32 masked = address & 0x3FFFFFFFu;
    if (masked < m_ram_size && u64(masked) + size <= m_ram_size)
      return m_ram + masked;
    if (m_exram && (masked >> 28) == 0x1)
    {
      const u32 offset = masked & 0x0FFFFFFFu;
      if (offset < m_exram_size && u64(offset) + size <= m_exram_size)
        return m_exram + offset;
    }
    return nullptr;
  }
  bool Valid(u32 address, size_t size) const { return GetPointerForRange(address, size) != nullptr; }

  void CopyFromEmu(void* data, u32 address, size_t size) const
  {
    if (size == 0)
      return;
    if (const u8* p = GetPointerForRange(address, size))
      std::memcpy(data, p, size);
  }
  void CopyToEmu(u32 address, const void* data, size_t size) const
  {
    if (size == 0)
      return;
    if (u8* p = GetPointerForRange(address, size))
      std::memcpy(p, data, size);
  }
  void Memset(u32 address, u8 value, size_t size) const
  {
    if (size == 0)
      return;
    if (u8* p = GetPointerForRange(address, size))
      std::memset(p, value, size);
  }

  u8 Read_U8(u32 address) const
  {
    const u8* p = GetPointerForRange(address, 1);
    return p ? *p : 0;
  }
  u16 Read_U16(u32 address) const
  {
    const u8* p = GetPointerForRange(address, 2);
    return p ? static_cast<u16>((p[0] << 8) | p[1]) : 0;
  }
  u32 Read_U32(u32 address) const
  {
    const u8* p = GetPointerForRange(address, 4);
    return p ? (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3] : 0;
  }
  u64 Read_U64(u32 address) const
  {
    const u8* p = GetPointerForRange(address, 8);
    if (!p)
      return 0;
    u64 value = 0;
    for (int i = 0; i < 8; ++i)
      value = (value << 8) | p[i];
    return value;
  }
  void Write_U8(u8 value, u32 address) const
  {
    if (u8* p = GetPointerForRange(address, 1))
      *p = value;
  }
  void Write_U16(u16 value, u32 address) const
  {
    if (u8* p = GetPointerForRange(address, 2))
    {
      p[0] = static_cast<u8>(value >> 8);
      p[1] = static_cast<u8>(value);
    }
  }
  void Write_U32(u32 value, u32 address) const
  {
    if (u8* p = GetPointerForRange(address, 4))
    {
      p[0] = static_cast<u8>(value >> 24);
      p[1] = static_cast<u8>(value >> 16);
      p[2] = static_cast<u8>(value >> 8);
      p[3] = static_cast<u8>(value);
    }
  }
  void Write_U64(u64 value, u32 address) const
  {
    if (u8* p = GetPointerForRange(address, 8))
    {
      for (int i = 7; i >= 0; --i)
      {
        p[i] = static_cast<u8>(value);
        value >>= 8;
      }
    }
  }

private:
  u8* m_ram = nullptr;
  u32 m_ram_size = 0;
  u8* m_exram = nullptr;
  u32 m_exram_size = 0;
  u8* m_l1 = nullptr;
  u32 m_l1_size = 0;
};
}  // namespace StaticRecompGuestMemory
