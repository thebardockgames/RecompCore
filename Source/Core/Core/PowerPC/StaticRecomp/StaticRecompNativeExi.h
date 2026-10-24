// RecompCore: StaticRecomp CPU core - native expansion interface (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The EXI as this game uses it: the three channels' registers (status,
// DMA address/length, control, immediate data at 0xCD006800 + 0x14 per
// channel) and the one device the Wii game talks to, the IPL/RTC/SRAM device
// on channel 0 chip select 2 (`__OSInitSram`, the RTC reads of the OS clock:
// ~6000 immediate transfers per boot). Memory card slots and serial ports have
// no device (the Wii has none): a transfer to an empty chip select does
// nothing, as in the chassis. The SRAM is the runtime's own: 68 bytes (RTC
// counter + settings) seeded once from the chassis's copy and persisted in the
// Wii user folder (sram.bin); the RTC is the host's local time since 2000
// (MODERNGEKKO_RTC_OFFSET=<seconds> shifts it). The DI configuration word
// (0xCD006024, read ~1500 times per boot) is served here too (0x1 on a Wii).
//
// MODERNGEKKO_NATIVE_EXI=off leaves the chassis's EXI in place.

#pragma once

#include <array>
#include <string>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace Core
{
class System;
}
namespace StaticRecompState
{
class StateStream;
}

namespace StaticRecompNativeExi
{
class NativeExi
{
public:
  // `game_id` picks the IPL header wording (PAL regions); `sram_seed` is the
  // chassis's SRAM copy, used only when no sram.bin exists yet. `sram_path`:
  // where the SRAM persists (the Wii user folder); `font_dir`: the bundled
  // IPL fonts (Sys/GC, ending in a separator).
  struct Paths
  {
    std::string sram_path;
    std::string font_dir;
  };
  NativeExi(const std::string& game_id, const u8* sram_seed,
            size_t sram_seed_size, Paths paths);
  ~NativeExi();

  bool Read(u32 ea, u8 size, u64* value);
  bool Write(u32 ea, u8 size, u64 value);

  void DoState(StaticRecompState::StateStream& p, u32 version);
  void PrintStats() const;

  struct Stats
  {
    u64 reads = 0, writes = 0, transfers = 0, imm_bytes = 0, rtc_latches = 0, sram_writes = 0,
        di_config_reads = 0, rom_bytes = 0, dma_transfers = 0, empty_device = 0, unknown_device = 0;
  };

private:
  struct Channel
  {
    u32 status = 0;   // UEXI_STATUS
    u32 dma_address = 0;
    u32 dma_length = 0;
    u32 control = 0;  // UEXI_CONTROL
    u32 imm_data = 0;
  };
  struct IplDevice
  {
    u32 command = 0;
    u32 command_bytes = 0;
    u32 cursor = 0;
  };

  void WriteStatus(Channel& ch, int index, u32 value);
  void WriteControl(Channel& ch, int index, u32 value);
  void UpdateInterrupts();
  void EnsureRom();
  void LoadSram(const u8* seed, size_t seed_size);
  void SaveSram() const;
  u32 RtcNow() const;
  void IplTransferByte(u8& data);
  u32 IplImmRead(u32 size);
  void IplImmWrite(u32 data, u32 size);

  bool m_disabled = false;
  bool m_pal = false;
  std::vector<u8> m_rom;  // header + bundled fonts, built on first ROM access
  std::array<u8, 0x44> m_sram{};  // RTC counter (4, big-endian) + settings (0x40)
  std::string m_sram_path;
  std::string m_font_dir;
  bool m_sram_dirty = false;
  u8 m_rtc_flags = 0;  // the Wii RTC flags byte (0x840020)
  Channel m_channels[3];
  IplDevice m_ipl;
  Stats m_stats;
};
}  // namespace StaticRecompNativeExi
