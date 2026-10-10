// RecompCore: StaticRecomp CPU core - native AX mixer.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host implementation of the work the AXWii DSP microcode does for the game's
// AX library: it executes a command list (voice parameter blocks, mixing,
// aux buffers, output) directly on guest memory, with no DSP, mailbox or DMA.
// The arithmetic is ported from Dolphin's AXWii HLE (AXWii.cpp, AX.cpp,
// AXVoice.h), which reproduces the microcode; only the new-filter command set
// of microcode CRC 0x347112ba / 0x4cc52064 is implemented.
//
// In verify mode writes go to an overlay instead of guest memory, so the
// result can be compared with what the emulated DSP wrote.

#pragma once

#include <array>
#include <map>
#include <optional>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace Memory
{
class MemoryManager;
}

namespace StaticRecompNativeAudio
{
class NativeAxWii
{
public:
  explicit NativeAxWii(Memory::MemoryManager& memory);
  ~NativeAxWii();

  // Microcode the game booted (Dolphin's ector CRC). False: not supported.
  static bool SupportsCrc(u32 crc);

  // Executes the command list at `address` (`size` 16-bit words). Returns
  // false if the list could not be read or holds an unknown command.
  bool ExecuteCommandList(u32 address, u16 size);
  // Number of DSP_SYNC mails the list produced (one per output command).
  u32 SyncMails() const { return m_sync_mails; }

  // Verify mode: writes are kept in an overlay (not in guest memory).
  void SetOverlay(bool enabled);
  // Overlay bytes that differ from guest memory (count of differing bytes;
  // first differing address in `first`).
  u32 CompareOverlay(u32* first) const;
  void ClearOverlay() { m_overlay.clear(); }

  void DoState(PointerWrap& p);

private:
  class Accelerator;
  friend class Accelerator;

  u8 Read8(u32 address) const;
  void Write8(u32 address, u8 value);
  u16 Read16(u32 address) const;
  void Write16(u32 address, u16 value);
  u32 Read32(u32 address) const;
  void Write32(u32 address, u32 value);
  u8 ReadSampleMemory(u32 address) const;

  void SetupProcessing(u32 init_addr);
  void AddToLR(u32 val_addr, bool neg);
  void AddSubToLR(u32 val_addr);
  void ProcessPBList(u32 pb_addr);
  void MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr, u16 volume);
  void UploadAUXMixLRSC(int aux_id, const u32* addresses, u16 volume);
  void RunCompressor(u16 threshold, u16 release_frames, u32 table_addr, u32 millis);
  void OutputSamples(u32 lr_addr, u32 surround_addr, u16 volume, bool upload_auxc);
  void OutputWMSamples(const u32* addresses);
  void LoadResamplingCoefficients();

  Memory::MemoryManager& m_memory;
  bool m_use_overlay = false;
  std::map<u32, u8> m_overlay;

  u16 m_cmdlist[512]{};
  u32 m_sync_mails = 0;

  int m_samples_main_left[32 * 3]{};
  int m_samples_main_right[32 * 3]{};
  int m_samples_main_surround[32 * 3]{};
  int m_samples_auxA_left[32 * 3]{};
  int m_samples_auxA_right[32 * 3]{};
  int m_samples_auxA_surround[32 * 3]{};
  int m_samples_auxB_left[32 * 3]{};
  int m_samples_auxB_right[32 * 3]{};
  int m_samples_auxB_surround[32 * 3]{};
  int m_samples_auxC_left[32 * 3]{};
  int m_samples_auxC_right[32 * 3]{};
  int m_samples_auxC_surround[32 * 3]{};
  int m_samples_wm0[6 * 3]{};
  int m_samples_aux0[6 * 3]{};
  int m_samples_wm1[6 * 3]{};
  int m_samples_aux1[6 * 3]{};
  int m_samples_wm2[6 * 3]{};
  int m_samples_aux2[6 * 3]{};
  int m_samples_wm3[6 * 3]{};
  int m_samples_aux3[6 * 3]{};

  u16 m_last_main_volume = 0x8000;
  std::array<u16, 3> m_last_aux_volumes{0x8000, 0x8000, 0x8000};
  u32 m_compressor_pos = 0;
  std::optional<u32> m_coeffs_checksum;
  std::array<s16, 0x800> m_coeffs{};
};
}  // namespace StaticRecompNativeAudio
