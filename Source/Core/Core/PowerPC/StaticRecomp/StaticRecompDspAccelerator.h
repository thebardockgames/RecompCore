// RecompCore: StaticRecomp CPU core - the DSP accelerator's sample decoder (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The ADPCM/PCM decoder of the DSP accelerator as the AX microcode drives it,
// ported from Dolphin's DSP::Accelerator (Core/DSP/DSPAccelerator.cpp, Dolphin
// Emulator Project, GPL-2.0-or-later) with the same arithmetic. Only the
// sample path the native AX mixer uses: raw ARAM reads/writes are not part
// of it.

#pragma once

#include <algorithm>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompDsp
{
class Accelerator
{
public:
  virtual ~Accelerator() = default;

  u16 ReadSample(const s16* coefs)
  {
    if (m_reads_stopped)
      return 0x0000;
    const u16 size = m_sample_format & 3;
    const u16 decode = (m_sample_format >> 2) & 3;
    const u16 gain_scale = (m_sample_format >> 4) & 3;
    u16 val = 0;
    u8 step_size = 0;
    s16 raw_sample;
    if (decode == kMmioPcmNoInc || decode == kMmioPcmInc)
      raw_sample = static_cast<s16>(m_input);  // the addresses can be nonsense here
    else
      raw_sample = static_cast<s16>(GetCurrentSample(size));

    const int coef_idx = (m_pred_scale >> 4) & 0x7;
    const s32 coef1 = coefs[coef_idx * 2 + 0];
    const s32 coef2 = coefs[coef_idx * 2 + 1];

    if (decode == kAdpcm)
    {
      // Only 4-bit decoding: larger accesses ignore the upper 12 bits.
      raw_sample &= 0xF;
      const int scale = 1 << (m_pred_scale & 0xF);
      if (raw_sample >= 8)
        raw_sample -= 16;
      const s32 val32 = (scale * raw_sample) + ((0x400 + coef1 * m_yn1 + coef2 * m_yn2) >> 11);
      val = static_cast<u16>(static_cast<s16>(std::clamp<s32>(val32, -0x7FFF, 0x7FFF)));
      step_size = 2;
      m_yn2 = m_yn1;
      m_yn1 = static_cast<s16>(val);
      m_current_address += 1;
      // End address 16-byte aligned: no ACCOV, no predscale update, the DSP
      // loops to start + 1 (or start).
      if ((m_end_address & 0xf) == 0x0 && m_current_address == m_end_address)
      {
        m_current_address = m_start_address + 1;
      }
      else if ((m_end_address & 0xf) == 0x1 && m_current_address == m_end_address - 1)
      {
        m_current_address = m_start_address;
      }
      else if ((m_current_address & 15) == 0)
      {
        m_pred_scale = ReadMemory((m_current_address & ~15u) >> 1);
        m_current_address += 2;
        step_size += 2;
      }
    }
    else
    {
      // PCM (the gain applies only here).
      u8 gain_shift = 0;
      if (gain_scale == 0)
        gain_shift = 11;
      else if (gain_scale == 2)
        gain_shift = 16;
      const s32 val32 = ((static_cast<s32>(m_gain) * raw_sample) >> gain_shift) +
                        (((coef1 * m_yn1) >> gain_shift) + ((coef2 * m_yn2) >> gain_shift));
      val = static_cast<u16>(static_cast<s16>(val32));
      m_yn2 = m_yn1;
      m_yn1 = static_cast<s16>(val);
      step_size = 2;
      if (decode != kMmioPcmNoInc)
        m_current_address += 1;
    }

    // Loop: YN1/YN2 take their loop values in the exception handler.
    if (m_current_address == (m_end_address + step_size - 1))
    {
      m_current_address = m_start_address;
      m_reads_stopped = true;
      OnSampleReadEndException();
    }
    SetCurrentAddress(m_current_address);
    return val;
  }

  u32 GetCurrentAddress() const { return m_current_address; }
  s16 GetYn1() const { return m_yn1; }
  s16 GetYn2() const { return m_yn2; }
  u16 GetPredScale() const { return m_pred_scale; }
  void SetStartAddress(u32 address) { m_start_address = address & kStartEndMask; }
  void SetEndAddress(u32 address) { m_end_address = address & kStartEndMask; }
  void SetCurrentAddress(u32 address) { m_current_address = address & kCurrentMask; }
  void SetSampleFormat(u16 format) { m_sample_format = format; }
  void SetGain(s16 gain) { m_gain = gain; }
  void SetYn1(s16 yn1) { m_yn1 = yn1; }
  void SetYn2(s16 yn2)
  {
    m_yn2 = yn2;
    m_reads_stopped = false;
  }
  void SetPredScale(u16 pred_scale) { m_pred_scale = pred_scale & 0x7f; }
  void SetInput(u16 input) { m_input = input; }

protected:
  virtual void OnSampleReadEndException() = 0;
  virtual u8 ReadMemory(u32 address) = 0;

private:
  static constexpr u16 kAdpcm = 0, kMmioPcmNoInc = 1, kMmioPcmInc = 3;
  static constexpr u32 kStartEndMask = 0x3fffffff;
  static constexpr u32 kCurrentMask = 0xbfffffff;

  u16 GetCurrentSample(u16 size)
  {
    switch (size)
    {
    case 0:  // 4-bit
    {
      u16 val = ReadMemory(m_current_address >> 1);
      return (m_current_address & 1) ? (val & 0xf) : (val >> 4);
    }
    case 1:  // 8-bit
      return ReadMemory(m_current_address);
    case 2:  // 16-bit
      return static_cast<u16>((ReadMemory(m_current_address * 2) << 8) |
                              ReadMemory(m_current_address * 2 + 1));
    default:  // invalid: garbage on hardware
      return 0;
    }
  }

  u32 m_start_address = 0;
  u32 m_end_address = 0;
  u32 m_current_address = 0;
  u16 m_sample_format = 0;
  s16 m_gain = 0;
  s16 m_yn1 = 0;
  s16 m_yn2 = 0;
  u16 m_pred_scale = 0;
  u16 m_input = 0;
  // After an ACCOV the accelerator returns nothing until YN2 is written.
  bool m_reads_stopped = false;
};
}  // namespace StaticRecompDsp
