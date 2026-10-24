// RecompCore: StaticRecomp CPU core - native expansion interface (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeExi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <chrono>
#include <ctime>


#include "Core/PowerPC/StaticRecomp/StaticRecompGuestMemory.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"

namespace Core
{
class System;
}

namespace StaticRecompNativeExi
{
namespace
{
constexpr u32 kExiBase = 0xCD006800u;
constexpr u32 kChannelStride = 0x14;
constexpr u32 kDiConfig = 0xCD006024u;
constexpr u32 kDiConfigWii = 0x1;  // CONFIG = 1: bootrom descrambler disabled (the chassis adds 8 only on Triforce)

// UEXI_STATUS bits.
constexpr u32 kExiIntMask = 1u << 0, kExiInt = 1u << 1, kTcIntMask = 1u << 2, kTcInt = 1u << 3;
constexpr u32 kClk = 7u << 4, kChipSelect = 7u << 7, kExtIntMask = 1u << 10, kExtInt = 1u << 11;
constexpr u32 kRomDis = 1u << 13;
// UEXI_CONTROL bits.
constexpr u32 kTStart = 1u << 0, kDma = 1u << 1;
constexpr u32 kIplChipSelect = 2;  // channel 0, device 1 (the MaskROM/IPL device)

// The IPL device's address map (CEXIIPL).
constexpr u32 kRomSize = 0x200000;
constexpr u32 kSramBase = 0x800000, kSramSize = 0x44;
constexpr u32 kUartBase = 0x800400, kUartSize = 0x50;
constexpr u32 kWiiRtcBase = 0x840000, kWiiRtcSize = 0x40;
constexpr u32 kEuartBase = 0xc00000, kEuartSize = 8;
}  // namespace

NativeExi::NativeExi(const std::string& game_id, const u8* sram_seed,
                     size_t sram_seed_size, Paths paths)
    : m_sram_path(std::move(paths.sram_path)), m_font_dir(std::move(paths.font_dir))
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_EXI");
  m_disabled = setting && (std::string(setting) == "off" || std::string(setting) == "0");
  // The IPL header wording follows the game's region (fourth character of the
  // game ID: P/D/F/S/I/X/Y are PAL).
  m_pal = game_id.size() >= 4 && std::string("PDFSIXYZ").find(game_id[3]) != std::string::npos;
  LoadSram(sram_seed, sram_seed_size);
}

NativeExi::~NativeExi()
{
  if (m_sram_dirty)
    SaveSram();
}

void NativeExi::LoadSram(const u8* seed, size_t seed_size)
{
  if (std::ifstream file(m_sram_path, std::ios::binary); file)
  {
    file.read(reinterpret_cast<char*>(m_sram.data()), m_sram.size());
    if (file.gcount() == static_cast<std::streamsize>(m_sram.size()))
      return;
  }
  // First run: the chassis's SRAM (its defaults and the GC language setting).
  if (seed && seed_size >= m_sram.size())
    std::memcpy(m_sram.data(), seed, m_sram.size());
  m_sram_dirty = true;
}

void NativeExi::SaveSram() const
{
  std::ofstream file(m_sram_path, std::ios::binary | std::ios::trunc);
  if (file)
    file.write(reinterpret_cast<const char*>(m_sram.data()), m_sram.size());
}

u32 NativeExi::RtcNow() const
{
  // The console's RTC counts seconds of local time since 2000-01-01 (what the
  // chassis computes without a custom RTC): the host's local wall clock.
  const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
  const std::chrono::zoned_seconds local(std::chrono::current_zone(), now);
  s64 seconds = local.get_local_time().time_since_epoch().count();
  if (const char* offset = std::getenv("MODERNGEKKO_RTC_OFFSET"))
    seconds += std::atoll(offset);
  constexpr s64 kGcEpoch = 0x386D4380;  // 2000-01-01 00:00:00
  return static_cast<u32>(seconds - kGcEpoch);
}

bool NativeExi::Read(u32 ea, u8 size, u64* value)
{
  if (m_disabled || size != 4)
    return false;
  if (ea == kDiConfig)
  {
    ++m_stats.di_config_reads;
    *value = kDiConfigWii;
    return true;
  }
  if (ea < kExiBase || ea >= kExiBase + 3 * kChannelStride)
    return false;
  const u32 index = (ea - kExiBase) / kChannelStride;
  const u32 reg = (ea - kExiBase) % kChannelStride;
  Channel& ch = m_channels[index];
  ++m_stats.reads;
  switch (reg)
  {
  case 0x00:
    // EXT: an external device in the memory card slot (device 0 of channels
    // 0 and 1). The Wii has none.
    *value = ch.status & ~(1u << 12);
    return true;
  case 0x04:
    *value = ch.dma_address;
    return true;
  case 0x08:
    *value = ch.dma_length;
    return true;
  case 0x0C:
    *value = ch.control;
    return true;
  case 0x10:
    *value = ch.imm_data;
    return true;
  }
  return false;
}

bool NativeExi::Write(u32 ea, u8 size, u64 value)
{
  if (m_disabled || size != 4)
    return false;
  if (ea < kExiBase || ea >= kExiBase + 3 * kChannelStride)
    return false;
  const u32 index = (ea - kExiBase) / kChannelStride;
  const u32 reg = (ea - kExiBase) % kChannelStride;
  Channel& ch = m_channels[index];
  const u32 word = static_cast<u32>(value);
  ++m_stats.writes;
  switch (reg)
  {
  case 0x00:
    WriteStatus(ch, static_cast<int>(index), word);
    return true;
  case 0x04:
    ch.dma_address = word;
    return true;
  case 0x08:
    ch.dma_length = word;
    return true;
  case 0x0C:
    WriteControl(ch, static_cast<int>(index), word);
    return true;
  case 0x10:
    ch.imm_data = word;
    return true;
  }
  return false;
}

void NativeExi::WriteStatus(Channel& ch, int index, u32 value)
{
  // As the chassis: masks are written, interrupt flags clear on 1, the clock
  // and the chip select are state; channels 0/1 own EXTINT, channel 0 ROMDIS.
  u32 status = ch.status;
  status = (status & ~kExiIntMask) | (value & kExiIntMask);
  if (value & kExiInt)
    status &= ~kExiInt;
  status = (status & ~kTcIntMask) | (value & kTcIntMask);
  if (value & kTcInt)
    status &= ~kTcInt;
  status = (status & ~kClk) | (value & kClk);
  if (index == 0 || index == 1)
  {
    status = (status & ~kExtIntMask) | (value & kExtIntMask);
    if (value & kExtInt)
      status &= ~kExtInt;
  }
  if (index == 0)
    status = (status & ~kRomDis) | (value & kRomDis);
  const u32 old_cs = (status & kChipSelect) >> 7;
  const u32 new_cs = (value & kChipSelect) >> 7;
  status = (status & ~kChipSelect) | (value & kChipSelect);
  ch.status = status;
  // The device whose select changed is told; the IPL device restarts its
  // command when it is selected or deselected.
  if (index == 0 && ((old_cs ^ new_cs) & kIplChipSelect) && new_cs)
  {
    m_ipl.command_bytes = 0;
    m_ipl.cursor = 0;
  }
  UpdateInterrupts();
}

void NativeExi::WriteControl(Channel& ch, int index, u32 value)
{
  ch.control = value;
  if (!(value & kTStart))
    return;
  const u32 cs = (ch.status & kChipSelect) >> 7;
  // One device per select line (1, 2, 4); with no line selected the chassis
  // has no device and the transfer never completes (TSTART stays set).
  if (cs != 1 && cs != 2 && cs != 4)
  {
    ++m_stats.unknown_device;
    return;
  }
  ++m_stats.transfers;
  const u32 rw = (value >> 2) & 3;
  const u32 length = ((value >> 4) & 3) + 1;
  const bool ipl = index == 0 && cs == kIplChipSelect;
  if (!ipl)
  {
    // The memory card slots and the serial ports hold no device on the Wii:
    // the chassis's empty device completes the transfer with nothing (an
    // immediate read gives 0, writes and DMA do nothing).
    ++m_stats.empty_device;
    if (!(value & kDma) && rw == 0)
      ch.imm_data = 0;
  }
  else if (value & kDma)
  {
    // DMA to or from the IPL device, byte by byte through the same protocol
    // (the SDK reads the 64 bytes of SRAM this way in __OSInitSram).
    const auto memory = StaticRecompGuestMemory::GuestMemory::Own();
    ++m_stats.dma_transfers;
    if (rw == 0)
    {
      for (u32 i = 0; i < ch.dma_length; ++i)
      {
        u8 byte = 0;
        IplTransferByte(byte);
        memory.Write_U8(byte, ch.dma_address + i);
      }
    }
    else if (rw == 1)
    {
      for (u32 i = 0; i < ch.dma_length; ++i)
      {
        u8 byte = memory.Read_U8(ch.dma_address + i);
        IplTransferByte(byte);
      }
    }
  }
  else
  {
    switch (rw)
    {
    case 0:  // read
      ch.imm_data = IplImmRead(length);
      break;
    case 1:  // write
      IplImmWrite(ch.imm_data, length);
      break;
    default:  // read-write: the chassis's IPL device does nothing
      break;
    }
  }
  ch.control &= ~kTStart;
  ch.status |= kTcInt;
  UpdateInterrupts();
}

void NativeExi::UpdateInterrupts()
{
  bool cause = false;
  for (const Channel& ch : m_channels)
  {
    cause |= (ch.status & kExiInt) && (ch.status & kExiIntMask);
    cause |= (ch.status & kTcInt) && (ch.status & kTcIntMask);
    cause |= (ch.status & kExtInt) && (ch.status & kExtIntMask);
  }
  StaticRecompNativePi::NativePi::Raise(StaticRecompNativePi::Line::kExi, cause);
}

// ---- the IPL device (CEXIIPL): command word, then ROM / SRAM+RTC / UART ----

void NativeExi::IplTransferByte(u8& data)
{
  if (m_ipl.command_bytes < 4)
  {
    m_ipl.command = (m_ipl.command << 8) | data;
    data = 0xff;
    if (++m_ipl.command_bytes == 4)
    {
      // The RTC is latched when a command completes (first 4 bytes of the SRAM
      // region, big-endian).
      ++m_stats.rtc_latches;
      const u32 rtc = RtcNow();
      m_sram[0] = static_cast<u8>(rtc >> 24);
      m_sram[1] = static_cast<u8>(rtc >> 16);
      m_sram[2] = static_cast<u8>(rtc >> 8);
      m_sram[3] = static_cast<u8>(rtc);
    }
    return;
  }
  const bool is_write = (m_ipl.command >> 31) & 1;
  const u32 address = (m_ipl.command >> 6) & 0x1ffffff;
  if (address < kRomSize)
  {
    // The IPL mask ROM as the chassis builds it without a dump: the copyright
    // header the OS checks (NTSC or PAL wording) and the bundled fonts the OS
    // reads with __OSReadROM (OSInitFont: 48 DMA reads of 256 bytes at boot;
    // with zeros there the boot never reached the first frame).
    EnsureRom();
    const u32 dev_addr = (address + m_ipl.cursor++) % kRomSize;
    if (!is_write)
      data = m_rom[dev_addr];
    ++m_stats.rom_bytes;
  }
  else if (address >= kSramBase && address < kSramBase + kSramSize)
  {
    const u32 dev_addr = (address - kSramBase + m_ipl.cursor++) % kSramSize;
    if (is_write)
    {
      m_sram[dev_addr] = data;
      m_sram_dirty = true;
      ++m_stats.sram_writes;
    }
    else
    {
      data = m_sram[dev_addr];
    }
    ++m_stats.imm_bytes;
  }
  else if (address >= kUartBase && address < kUartBase + kUartSize)
  {
    if (!is_write && address - kUartBase == 0)
      data = 0;  // queue length: instant
  }
  else if (address >= kWiiRtcBase && address < kWiiRtcBase + kWiiRtcSize &&
           address - kWiiRtcBase == 0x20)
  {
    if (is_write)
      m_rtc_flags = data;
    else
      data = m_rtc_flags;
  }
  else if (address >= kEuartBase && address < kEuartBase + kEuartSize)
  {
    if (!is_write && address - kEuartBase == 4)
      data = 0;
  }
  else
  {
    if (m_stats.unknown_device < 8)
      std::fprintf(stderr, "[native_exi] IPL device: unknown region %s 0x%08x\n",
                   is_write ? "write" : "read", address);
    ++m_stats.unknown_device;
  }
}

void NativeExi::EnsureRom()
{
  if (!m_rom.empty())
    return;
  m_rom.assign(kRomSize, 0);
  static const char kHeaderNtsc[] =
      "(C) 1999-2001 Nintendo.  All rights reserved.(C) 1999 ArtX Inc.  All rights reserved.";
  static const char kHeaderPal[] =
      "(C) 1999-2001 Nintendo.  All rights reserved.(C) 1999 ArtX Inc.  All rights "
      "reserved.PAL  Revision 1.0  ";
  const char* header = m_pal ? kHeaderPal : kHeaderNtsc;
  std::memcpy(m_rom.data(), header, std::strlen(header));
  // The same font files, at the same offsets, as the chassis (Sys/GC).
  const std::string& sys = m_font_dir;
  const auto load = [&](const std::string& path, u32 offset) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
      std::fprintf(stderr, "[native_exi] font %s not found: ROM region left blank\n", path.c_str());
      return;
    }
    file.read(reinterpret_cast<char*>(m_rom.data() + offset), kRomSize - offset);
  };
  load(sys + "font_japanese.bin", 0x1aff00);  // Shift-JIS IPL font
  load(sys + "font_western.bin", 0x1fcf00);   // Windows-1252 IPL font
}

u32 NativeExi::IplImmRead(u32 size)
{
  u32 result = 0;
  u32 position = 0;
  while (size--)
  {
    u8 byte = 0;
    IplTransferByte(byte);
    result |= u32(byte) << (24 - (position++ * 8));
  }
  return result;
}

void NativeExi::IplImmWrite(u32 data, u32 size)
{
  while (size--)
  {
    u8 byte = static_cast<u8>(data >> 24);
    IplTransferByte(byte);
    data <<= 8;
  }
}

void NativeExi::DoState(StaticRecompState::StateStream& p, u32 version)
{
  // Version 9: + the SRAM bytes and the RTC flags (own storage).
  if (version >= 9)
  {
    p.DoArray(m_sram.data(), static_cast<u32>(m_sram.size()));
    p.Do(m_rtc_flags);
    if (p.IsReadMode())
      m_sram_dirty = true;
  }
  for (Channel& ch : m_channels)
  {
    p.Do(ch.status);
    p.Do(ch.dma_address);
    p.Do(ch.dma_length);
    p.Do(ch.control);
    p.Do(ch.imm_data);
  }
  p.Do(m_ipl.command);
  p.Do(m_ipl.command_bytes);
  p.Do(m_ipl.cursor);
}

void NativeExi::PrintStats() const
{
  std::fprintf(stderr,
               "[native_exi] reads=%llu writes=%llu transfers=%llu sram_bytes=%llu sram_writes=%llu "
               "rtc_latches=%llu di_config_reads=%llu rom_bytes=%llu dma=%llu empty_device=%llu unknown=%llu%s\n",
               static_cast<unsigned long long>(m_stats.reads),
               static_cast<unsigned long long>(m_stats.writes),
               static_cast<unsigned long long>(m_stats.transfers),
               static_cast<unsigned long long>(m_stats.imm_bytes),
               static_cast<unsigned long long>(m_stats.sram_writes),
               static_cast<unsigned long long>(m_stats.rtc_latches),
               static_cast<unsigned long long>(m_stats.di_config_reads),
               static_cast<unsigned long long>(m_stats.rom_bytes),
               static_cast<unsigned long long>(m_stats.dma_transfers),
               static_cast<unsigned long long>(m_stats.empty_device),
               static_cast<unsigned long long>(m_stats.unknown_device), m_disabled ? " (off)" : "");
}
}  // namespace StaticRecompNativeExi
