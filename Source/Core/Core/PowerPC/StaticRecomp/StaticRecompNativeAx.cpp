// RecompCore: StaticRecomp CPU core - native AX mixer.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Ported from Dolphin's AXWii HLE (Core/HW/DSPHLE/UCodes/AXWii.cpp, AX.cpp,
// AXVoice.h; Dolphin Emulator Project, GPL-2.0-or-later). The arithmetic is
// kept identical so the output matches the microcode the HLE reproduces.

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeAx.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IOFile.h"
#include "Common/MathUtil.h"
#include "Common/Swap.h"
#include "Core/DSP/DSPAccelerator.h"
#include "Core/HW/DSPHLE/UCodes/AXStructs.h"
#include "Core/HW/Memmap.h"

namespace StaticRecompNativeAudio
{
namespace
{
using DSP::HLE::AXPBWii;
using DSP::HLE::PBBiquadFilter;
using DSP::HLE::PBLowPassFilter;
using DSP::HLE::PBUpdatesWii;
using DSP::HLE::VolumeData;

#define HILO_TO_32(name) ((u32(name##_hi) << 16) | name##_lo)

// AXMixControl (Core/HW/DSPHLE/UCodes/AX.h).
enum MixControl : u32
{
  MIX_MAIN_L = 0x000001,
  MIX_MAIN_L_RAMP = 0x000002,
  MIX_MAIN_R = 0x000004,
  MIX_MAIN_R_RAMP = 0x000008,
  MIX_MAIN_S = 0x000010,
  MIX_MAIN_S_RAMP = 0x000020,
  MIX_AUXA_L = 0x000040,
  MIX_AUXA_L_RAMP = 0x000080,
  MIX_AUXA_R = 0x000100,
  MIX_AUXA_R_RAMP = 0x000200,
  MIX_AUXA_S = 0x000400,
  MIX_AUXA_S_RAMP = 0x000800,
  MIX_AUXB_L = 0x001000,
  MIX_AUXB_L_RAMP = 0x002000,
  MIX_AUXB_R = 0x004000,
  MIX_AUXB_R_RAMP = 0x008000,
  MIX_AUXB_S = 0x010000,
  MIX_AUXB_S_RAMP = 0x020000,
  MIX_AUXC_L = 0x040000,
  MIX_AUXC_L_RAMP = 0x080000,
  MIX_AUXC_R = 0x100000,
  MIX_AUXC_R_RAMP = 0x200000,
  MIX_AUXC_S = 0x400000,
  MIX_AUXC_S_RAMP = 0x800000,
};

// AXWii (new) command list opcodes.
enum Command : u16
{
  CMD_SETUP = 0x00,
  CMD_ADD_TO_LR = 0x01,
  CMD_SUB_TO_LR = 0x02,
  CMD_ADD_SUB_TO_LR = 0x03,
  CMD_PROCESS = 0x04,
  CMD_MIX_AUXA = 0x05,
  CMD_MIX_AUXB = 0x06,
  CMD_MIX_AUXC = 0x07,
  CMD_UPL_AUXA_MIX_LRSC = 0x08,
  CMD_UPL_AUXB_MIX_LRSC = 0x09,
  CMD_COMPRESSOR = 0x0A,
  CMD_OUTPUT = 0x0B,
  CMD_OUTPUT_DPL2 = 0x0C,
  CMD_WM_OUTPUT = 0x0D,
  CMD_END = 0x0E,
};

enum
{
  SRCTYPE_POLYPHASE = 0,
  SRCTYPE_LINEAR = 1,
};

s16 ClampS16(s64 sample)
{
  return static_cast<s16>(std::clamp<s64>(sample, -0x8000, 0x7FFF));
}

u32 ConvertMixerControl(u32 mixer_control)
{
  u32 ret = 0;
  if (mixer_control & 0x00000001)
    ret |= MIX_MAIN_L;
  if (mixer_control & 0x00000002)
    ret |= MIX_MAIN_R;
  if (mixer_control & 0x00000004)
    ret |= MIX_MAIN_L | MIX_MAIN_R | MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP;
  if (mixer_control & 0x00000008)
    ret |= MIX_MAIN_S;
  if (mixer_control & 0x00000010)
    ret |= MIX_MAIN_S | MIX_MAIN_S_RAMP;
  if (mixer_control & 0x00010000)
    ret |= MIX_AUXA_L;
  if (mixer_control & 0x00020000)
    ret |= MIX_AUXA_R;
  if (mixer_control & 0x00040000)
    ret |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
  if (mixer_control & 0x00080000)
    ret |= MIX_AUXA_S;
  if (mixer_control & 0x00100000)
    ret |= MIX_AUXA_S | MIX_AUXA_S_RAMP;
  if (mixer_control & 0x00200000)
    ret |= MIX_AUXB_L;
  if (mixer_control & 0x00400000)
    ret |= MIX_AUXB_R;
  if (mixer_control & 0x00800000)
    ret |= MIX_AUXB_L | MIX_AUXB_R | MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
  if (mixer_control & 0x01000000)
    ret |= MIX_AUXB_S;
  if (mixer_control & 0x02000000)
    ret |= MIX_AUXB_S | MIX_AUXB_S_RAMP;
  if (mixer_control & 0x04000000)
    ret |= MIX_AUXC_L;
  if (mixer_control & 0x08000000)
    ret |= MIX_AUXC_R;
  if (mixer_control & 0x10000000)
    ret |= MIX_AUXC_L | MIX_AUXC_R | MIX_AUXC_L_RAMP | MIX_AUXC_R_RAMP;
  if (mixer_control & 0x20000000)
    ret |= MIX_AUXC_S;
  if (mixer_control & 0x40000000)
    ret |= MIX_AUXC_S | MIX_AUXC_S_RAMP;
  return ret;
}

void GenerateVolumeRamp(u16* output, u16 vol1, u16 vol2, size_t nvals)
{
  float curr = vol1;
  for (size_t i = 0; i < nvals; ++i)
  {
    curr += (vol2 - vol1) / (float)nvals;
    output[i] = (u16)curr;
  }
}

// Resampling (AXVoice.h ResampleAudio): input -> count output samples.
template <typename Input>
u32 ResampleAudio(Input&& input_callback, s16* output, u32 count, s16* last_samples, u32 curr_pos,
                  u32 ratio, int srctype, const s16* coeffs)
{
  int read_samples_count = 0;
  if (coeffs && srctype == SRCTYPE_POLYPHASE)
  {
    s16 temp[4];
    u32 idx = 0;
    temp[idx++ & 3] = last_samples[0];
    temp[idx++ & 3] = last_samples[1];
    temp[idx++ & 3] = last_samples[2];
    temp[idx++ & 3] = last_samples[3];
    for (u32 i = 0; i < count; ++i)
    {
      curr_pos += ratio;
      while (curr_pos >= 0x10000)
      {
        temp[idx++ & 3] = input_callback(read_samples_count++);
        curr_pos -= 0x10000;
      }
      u16 curr_pos_frac = ((curr_pos & 0xFFFF) >> 9) << 2;
      const s16* c = &coeffs[curr_pos_frac];
      s64 t0 = temp[idx++ & 3];
      s64 t1 = temp[idx++ & 3];
      s64 t2 = temp[idx++ & 3];
      s64 t3 = temp[idx++ & 3];
      s64 samp = (t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15;
      output[i] = MathUtil::SaturatingCast<s16>(samp);
    }
    last_samples[3] = temp[--idx & 3];
    last_samples[2] = temp[--idx & 3];
    last_samples[1] = temp[--idx & 3];
    last_samples[0] = temp[--idx & 3];
  }
  else if (srctype == SRCTYPE_LINEAR || srctype == SRCTYPE_POLYPHASE)
  {
    s16 temp[4];
    u32 idx = 0;
    temp[idx++ & 3] = last_samples[0];
    temp[idx++ & 3] = last_samples[1];
    temp[idx++ & 3] = last_samples[2];
    temp[idx++ & 3] = last_samples[3];
    for (u32 i = 0; i < count; ++i)
    {
      curr_pos += ratio;
      while (curr_pos >= 0x10000)
      {
        temp[idx++ & 3] = input_callback(read_samples_count++);
        curr_pos -= 0x10000;
      }
      u16 curr_frac = curr_pos & 0xFFFF;
      u16 inv_curr_frac = -curr_frac;
      s16 sample;
      if (curr_frac)
      {
        s32 s0 = temp[idx++ & 3];
        s32 s1 = temp[idx++ & 3];
        sample = ((s0 * inv_curr_frac) + (s1 * curr_frac)) >> 16;
        idx += 2;
      }
      else
      {
        sample = temp[idx++ & 3];
        idx += 3;
      }
      output[i] = sample;
    }
    last_samples[3] = temp[--idx & 3];
    last_samples[2] = temp[--idx & 3];
    last_samples[1] = temp[--idx & 3];
    last_samples[0] = temp[--idx & 3];
  }
  else  // SRCTYPE_NEAREST
  {
    for (u32 i = 0; i < count; ++i)
      output[i] = input_callback(i);
    std::memcpy(last_samples, output + count - 4, 4 * sizeof(u16));
  }
  return curr_pos;
}

void MixAdd(int* out, const s16* input, u32 count, VolumeData* vd, s16* dpop, bool ramp)
{
  u16& volume = vd->volume;
  u16 volume_delta = ramp ? vd->volume_delta : 0;
  for (u32 i = 0; i < count; ++i)
  {
    s64 sample = input[i];
    sample *= volume;
    sample >>= 15;
    s16 sample16 = ClampS16((s32)sample);
    out[i] += sample16;
    volume += volume_delta;
    *dpop = sample16;
  }
}

void LowPassFilter(s16* samples, u32 count, PBLowPassFilter& f)
{
  for (u32 i = 0; i < count; ++i)
    f.yn1 = samples[i] = ClampS16((f.a0 * (s32)samples[i] + f.b0 * (s32)f.yn1) >> 15);
}

void BiquadFilter(s16* samples, u32 count, PBBiquadFilter& f)
{
  for (u32 i = 0; i < count; ++i)
  {
    s16 xn0 = samples[i];
    s64 tmp = 0;
    tmp += f.b0 * s32(xn0);
    tmp += f.b1 * s32(f.xn1);
    tmp += f.b2 * s32(f.xn2);
    tmp += f.a1 * s32(f.yn1);
    tmp += f.a2 * s32(f.yn2);
    tmp <<= 2;
    if (tmp & 0x10000)
      tmp += 0x8000;
    else
      tmp += 0x7FFF;
    tmp >>= 16;
    s16 yn0 = ClampS16(tmp);
    f.xn2 = f.xn1;
    f.yn2 = f.yn1;
    f.xn1 = xn0;
    f.yn1 = yn0;
    samples[i] = yn0;
  }
}

struct Buffers
{
  int* main_left;
  int* main_right;
  int* main_surround;
  int* auxA_left;
  int* auxA_right;
  int* auxA_surround;
  int* auxB_left;
  int* auxB_right;
  int* auxB_surround;
  int* auxC_left;
  int* auxC_right;
  int* auxC_surround;
  int* wm_main[4];
  int* wm_aux[4];
};
}  // namespace

// The sample decoder of the DSP (ADPCM / PCM), reading guest memory the way
// Wii AX addresses it (bit 28: MEM2).
class NativeAxWii::Accelerator final : public DSP::Accelerator
{
public:
  explicit Accelerator(const NativeAxWii& owner) : m_owner(owner) {}
  AXPBWii* acc_pb = nullptr;

protected:
  void OnRawReadEndException() override {}
  void OnRawWriteEndException() override {}
  void OnSampleReadEndException() override
  {
    if (acc_pb->audio_addr.looping)
    {
      SetPredScale(acc_pb->adpcm_loop_info.pred_scale);
      if (acc_pb->is_stream != 1)
      {
        SetYn1(acc_pb->adpcm_loop_info.yn1);
        SetYn2(acc_pb->adpcm_loop_info.yn2);
      }
      else
      {
        SetYn1(GetYn1());
        SetYn2(GetYn2());
      }
    }
    else
    {
      acc_pb->running = 0;
    }
  }
  u8 ReadMemory(u32 address) override { return m_owner.ReadSampleMemory(address); }
  void WriteMemory(u32, u8) override {}

private:
  const NativeAxWii& m_owner;
};

NativeAxWii::NativeAxWii(Memory::MemoryManager& memory) : m_memory(memory)
{
  LoadResamplingCoefficients();
}

NativeAxWii::~NativeAxWii() = default;

void NativeAxWii::DoState(PointerWrap& p)
{
  p.Do(m_samples_main_left);
  p.Do(m_samples_main_right);
  p.Do(m_samples_main_surround);
  p.Do(m_samples_auxA_left);
  p.Do(m_samples_auxA_right);
  p.Do(m_samples_auxA_surround);
  p.Do(m_samples_auxB_left);
  p.Do(m_samples_auxB_right);
  p.Do(m_samples_auxB_surround);
  p.Do(m_samples_auxC_left);
  p.Do(m_samples_auxC_right);
  p.Do(m_samples_auxC_surround);
  p.Do(m_samples_wm0);
  p.Do(m_samples_aux0);
  p.Do(m_samples_wm1);
  p.Do(m_samples_aux1);
  p.Do(m_samples_wm2);
  p.Do(m_samples_aux2);
  p.Do(m_samples_wm3);
  p.Do(m_samples_aux3);
  p.Do(m_last_main_volume);
  p.Do(m_last_aux_volumes);
  p.Do(m_compressor_pos);
}

bool NativeAxWii::SupportsCrc(u32 crc)
{
  return crc == 0x347112ba || crc == 0x4cc52064;
}

void NativeAxWii::LoadResamplingCoefficients()
{
  // Same files and order as Dolphin's HLE (AXUCode::LoadResamplingCoefficients).
  constexpr size_t raw_coeffs_size = 0x800 * 2;
  const std::string filenames[] = {
      File::GetUserPath(D_GCUSER_IDX) + "dsp_coef.bin",
      File::GetSysDirectory() + "/GC/dsp_coef.bin",
  };
  for (const std::string& filename : filenames)
  {
    if (File::GetSize(filename) != raw_coeffs_size)
      continue;
    File::IOFile fp(filename, "rb");
    std::array<u8, raw_coeffs_size> raw_coeffs;
    fp.ReadBytes(raw_coeffs.data(), raw_coeffs_size);
    m_coeffs_checksum = Common::HashAdler32(raw_coeffs.data(), raw_coeffs_size);
    std::memcpy(m_coeffs.data(), raw_coeffs.data(), raw_coeffs_size);
    for (auto& coef : m_coeffs)
      coef = Common::swap16(coef);
    return;
  }
}

void NativeAxWii::SetOverlay(bool enabled)
{
  m_use_overlay = enabled;
  m_overlay.clear();
}

u32 NativeAxWii::CompareOverlay(u32* first) const
{
  u32 differences = 0;
  for (const auto& [address, value] : m_overlay)
  {
    const u8* p = m_memory.GetPointerForRange(address, 1);
    if (!p || *p != value)
    {
      if (differences++ == 0 && first)
        *first = address;
    }
  }
  return differences;
}

u8 NativeAxWii::Read8(u32 address) const
{
  if (m_use_overlay)
  {
    const auto it = m_overlay.find(address);
    if (it != m_overlay.end())
      return it->second;
  }
  const u8* p = m_memory.GetPointerForRange(address, 1);
  return p ? *p : 0;
}

void NativeAxWii::Write8(u32 address, u8 value)
{
  if (m_use_overlay)
  {
    m_overlay[address] = value;
    return;
  }
  if (u8* p = m_memory.GetPointerForRange(address, 1))
    *p = value;
}

u16 NativeAxWii::Read16(u32 address) const
{
  if (!m_use_overlay)
  {
    const u8* p = m_memory.GetPointerForRange(address, 2);
    return p ? static_cast<u16>((p[0] << 8) | p[1]) : 0;
  }
  return static_cast<u16>((Read8(address) << 8) | Read8(address + 1));
}

void NativeAxWii::Write16(u32 address, u16 value)
{
  if (!m_use_overlay)
  {
    if (u8* p = m_memory.GetPointerForRange(address, 2))
    {
      p[0] = static_cast<u8>(value >> 8);
      p[1] = static_cast<u8>(value);
    }
    return;
  }
  Write8(address, static_cast<u8>(value >> 8));
  Write8(address + 1, static_cast<u8>(value));
}

u32 NativeAxWii::Read32(u32 address) const
{
  return (u32(Read16(address)) << 16) | Read16(address + 2);
}

void NativeAxWii::Write32(u32 address, u32 value)
{
  Write16(address, static_cast<u16>(value >> 16));
  Write16(address + 2, static_cast<u16>(value));
}

u8 NativeAxWii::ReadSampleMemory(u32 address) const
{
  // DSPManager::ReadARAM in Wii mode.
  if (address & 0x10000000)
    return m_memory.GetEXRAM()[address & m_memory.GetExRamMask()];
  return m_memory.GetRAM()[address & m_memory.GetRamMask()];
}

bool NativeAxWii::ExecuteCommandList(u32 address, u16 size)
{
  m_sync_mails = 0;
  if (size >= std::size(m_cmdlist) || !m_memory.GetPointerForRange(address, size * 2u))
    return false;
  for (u32 i = 0; i < size; ++i)
    m_cmdlist[i] = Read16(address + i * 2);

  u32 curr_idx = 0;
  const auto next = [&] { return curr_idx < size ? m_cmdlist[curr_idx++] : u16(CMD_END); };
  const auto next32 = [&] {
    const u32 hi = next();
    return (hi << 16) | next();
  };
  while (true)
  {
    const u16 cmd = next();
    switch (cmd)
    {
    case CMD_SETUP:
      SetupProcessing(next32());
      break;
    case CMD_ADD_TO_LR:
    case CMD_SUB_TO_LR:
      AddToLR(next32(), cmd == CMD_SUB_TO_LR);
      break;
    case CMD_ADD_SUB_TO_LR:
      AddSubToLR(next32());
      break;
    case CMD_PROCESS:
      ProcessPBList(next32());
      break;
    case CMD_MIX_AUXA:
    case CMD_MIX_AUXB:
    case CMD_MIX_AUXC:
    {
      const u16 volume = next();
      const u32 write_addr = next32();
      const u32 read_addr = next32();
      MixAUXSamples(cmd - CMD_MIX_AUXA, write_addr, read_addr, volume);
      break;
    }
    case CMD_UPL_AUXA_MIX_LRSC:
    case CMD_UPL_AUXB_MIX_LRSC:
    {
      const u16 volume = next();
      u32 addresses[6];
      for (u32& a : addresses)
        a = next32();
      UploadAUXMixLRSC(cmd == CMD_UPL_AUXB_MIX_LRSC, addresses, volume);
      break;
    }
    case CMD_COMPRESSOR:
    {
      const u16 threshold = next();
      const u16 frames = next();
      RunCompressor(threshold, frames, next32(), 3);
      break;
    }
    case CMD_OUTPUT:
    case CMD_OUTPUT_DPL2:
    {
      const u16 volume = next();
      const u32 surround_addr = next32();
      const u32 lr_addr = next32();
      OutputSamples(lr_addr, surround_addr, volume, cmd == CMD_OUTPUT_DPL2);
      break;
    }
    case CMD_WM_OUTPUT:
    {
      u32 addresses[4];
      for (u32& a : addresses)
        a = next32();
      OutputWMSamples(addresses);
      break;
    }
    case CMD_END:
      return true;
    default:
      // The HLE skips unknown commands without arguments; an unknown command
      // here means an unsupported microcode or a corrupted list.
      return false;
    }
  }
}

void NativeAxWii::SetupProcessing(u32 init_addr)
{
  // InitMixingBuffers<3 ms>: per buffer, a 32-bit start value and a delta.
  struct Desc
  {
    int* ptr;
    int samples_per_milli;
  };
  const std::array<Desc, 20> buffers = {{
      {m_samples_main_left, 32}, {m_samples_main_right, 32}, {m_samples_main_surround, 32},
      {m_samples_auxA_left, 32}, {m_samples_auxA_right, 32}, {m_samples_auxA_surround, 32},
      {m_samples_auxB_left, 32}, {m_samples_auxB_right, 32}, {m_samples_auxB_surround, 32},
      {m_samples_auxC_left, 32}, {m_samples_auxC_right, 32}, {m_samples_auxC_surround, 32},
      {m_samples_wm0, 6},        {m_samples_aux0, 6},        {m_samples_wm1, 6},
      {m_samples_aux1, 6},       {m_samples_wm2, 6},         {m_samples_aux2, 6},
      {m_samples_wm3, 6},        {m_samples_aux3, 6},
  }};
  for (size_t i = 0; i < buffers.size(); ++i)
  {
    const u32 at = init_addr + static_cast<u32>(i) * 6;
    const s32 value = static_cast<s32>((u32(Read16(at)) << 16) | Read16(at + 2));
    const s16 delta = static_cast<s16>(Read16(at + 4));
    const Desc& buf = buffers[i];
    if (value == 0)
    {
      std::memset(buf.ptr, 0, 3 * buf.samples_per_milli * sizeof(int));
    }
    else
    {
      for (int j = 0; j < 3 * buf.samples_per_milli; ++j)
        buf.ptr[j] = value + j * delta;
    }
  }
}

void NativeAxWii::AddToLR(u32 val_addr, bool neg)
{
  for (int i = 0; i < 32 * 3; ++i)
  {
    int val = static_cast<int>(Read32(val_addr + i * 4));
    if (neg)
      val = -val;
    m_samples_main_left[i] += val;
    m_samples_main_right[i] += val;
  }
}

void NativeAxWii::AddSubToLR(u32 val_addr)
{
  for (int i = 0; i < 32 * 3; ++i)
    m_samples_main_left[i] += static_cast<int>(Read32(val_addr + i * 4));
  for (int i = 0; i < 32 * 3; ++i)
    m_samples_main_right[i] -= static_cast<int>(Read32(val_addr + (96 + i) * 4));
}

void NativeAxWii::ProcessPBList(u32 pb_addr)
{
  constexpr size_t updates_begin = offsetof(AXPBWii, updates);
  constexpr size_t updates_end = updates_begin + sizeof(PBUpdatesWii);
  static_assert(sizeof(AXPBWii) % 2 == 0);

  Accelerator accelerator(*this);
  const s16* coeffs = m_coeffs_checksum ? m_coeffs.data() : nullptr;
  // Guard against a corrupted list: the DSP would loop forever too, but the
  // host must not.
  for (u32 guard = 0; pb_addr && guard < 4096; ++guard)
  {
    // Microcode 0x347112ba: the guest PB has no `updates` field.
    AXPBWii pb;
    u16* words = reinterpret_cast<u16*>(&pb);
    for (size_t i = 0; i < updates_begin / 2; ++i)
      words[i] = Read16(pb_addr + static_cast<u32>(i * 2));
    std::memset(reinterpret_cast<u8*>(&pb) + updates_begin, 0, sizeof(PBUpdatesWii));
    for (size_t i = updates_end / 2; i < sizeof(pb) / 2; ++i)
      words[i] = Read16(pb_addr + static_cast<u32>(i * 2 - sizeof(PBUpdatesWii)));

    if (pb.running == 1)
    {
      Buffers buffers{m_samples_main_left,  m_samples_main_right,  m_samples_main_surround,
                      m_samples_auxA_left,  m_samples_auxA_right,  m_samples_auxA_surround,
                      m_samples_auxB_left,  m_samples_auxB_right,  m_samples_auxB_surround,
                      m_samples_auxC_left,  m_samples_auxC_right,  m_samples_auxC_surround,
                      {m_samples_wm0, m_samples_wm1, m_samples_wm2, m_samples_wm3},
                      {m_samples_aux0, m_samples_aux1, m_samples_aux2, m_samples_aux3}};
      constexpr u32 count = 96;
      const u32 mctrl = ConvertMixerControl(HILO_TO_32(pb.mixer_control));

      // GetInputSamples: accelerator setup, resampling, PB update.
      accelerator.acc_pb = &pb;
      accelerator.SetStartAddress(HILO_TO_32(pb.audio_addr.loop_addr));
      accelerator.SetEndAddress(HILO_TO_32(pb.audio_addr.end_addr));
      accelerator.SetCurrentAddress(HILO_TO_32(pb.audio_addr.cur_addr));
      accelerator.SetSampleFormat(pb.audio_addr.sample_format);
      accelerator.SetYn1(pb.adpcm.yn1);
      accelerator.SetYn2(pb.adpcm.yn2);
      accelerator.SetGain(pb.adpcm.gain);
      accelerator.SetPredScale(pb.adpcm.pred_scale);
      const s16* voice_coeffs = coeffs ? coeffs + pb.coef_select * 0x200 : nullptr;
      s16 samples[96];
      const u32 curr_pos = ResampleAudio(
          [&](u32) { return static_cast<s16>(accelerator.ReadSample(pb.adpcm.coefs)); }, samples,
          count, pb.src.last_samples, pb.src.cur_addr_frac, HILO_TO_32(pb.src.ratio), pb.src_type,
          voice_coeffs);
      pb.src.cur_addr_frac = (curr_pos & 0xFFFF);
      pb.audio_addr.cur_addr_hi = static_cast<u16>(accelerator.GetCurrentAddress() >> 16);
      pb.audio_addr.cur_addr_lo = static_cast<u16>(accelerator.GetCurrentAddress());
      pb.adpcm.yn1 = accelerator.GetYn1();
      pb.adpcm.yn2 = accelerator.GetYn2();
      pb.adpcm.pred_scale = accelerator.GetPredScale();

      // Volume envelope (unsigned on Wii).
      for (u32 i = 0; i < count; ++i)
      {
        const s32 volume = (u16)pb.vol_env.cur_volume;
        const s32 sample = ((s32)samples[i] * volume) >> 15;
        samples[i] = ClampS16(sample);
        pb.vol_env.cur_volume += pb.vol_env.cur_volume_delta;
      }
      if (pb.lpf.on != 0)
        LowPassFilter(samples, count, pb.lpf);
      if (pb.biquad.on != 0)
        BiquadFilter(samples, count, pb.biquad);

#define MIX(C, buffer, volume_field, dpop_field)                                                                \
  if (mctrl & MIX_##C)                                                                             \
    MixAdd(buffer, samples, count, &pb.mixer.volume_field, &pb.dpop.dpop_field, (mctrl & MIX_##C##_RAMP) != 0);
      MIX(MAIN_L, buffers.main_left, main_left, main_left)
      MIX(MAIN_R, buffers.main_right, main_right, main_right)
      MIX(MAIN_S, buffers.main_surround, main_surround, main_surround)
      MIX(AUXA_L, buffers.auxA_left, auxA_left, auxA_left)
      MIX(AUXA_R, buffers.auxA_right, auxA_right, auxA_right)
      MIX(AUXA_S, buffers.auxA_surround, auxA_surround, auxA_surround)
      MIX(AUXB_L, buffers.auxB_left, auxB_left, auxB_left)
      MIX(AUXB_R, buffers.auxB_right, auxB_right, auxB_right)
      MIX(AUXB_S, buffers.auxB_surround, auxB_surround, auxB_surround)
      MIX(AUXC_L, buffers.auxC_left, auxC_left, auxC_left)
      MIX(AUXC_R, buffers.auxC_right, auxC_right, auxC_right)
      MIX(AUXC_S, buffers.auxC_surround, auxC_surround, auxC_surround)
#undef MIX

      // Wii Remote speaker mixing.
      if (pb.remote)
      {
        if (pb.remote_iir.on != 0)
        {
          if (pb.remote_iir.on == 2)
            BiquadFilter(samples, count, pb.remote_iir.biquad);
          else
            LowPassFilter(samples, count, pb.remote_iir.lpf);
        }
        constexpr u16 wm_count = 18;
        s16 wm_samples[18];
        const u32 wm_pos = ResampleAudio([&samples](u32 i) { return samples[i]; }, wm_samples,
                                         wm_count, pb.remote_src.last_samples,
                                         pb.remote_src.cur_addr_frac, 0x55555, SRCTYPE_POLYPHASE,
                                         coeffs);
        pb.remote_src.cur_addr_frac = wm_pos & 0xFFFF;
        VolumeData* const wm_volumes[8] = {&pb.remote_mixer.main0, &pb.remote_mixer.aux0,
                                           &pb.remote_mixer.main1, &pb.remote_mixer.aux1,
                                           &pb.remote_mixer.main2, &pb.remote_mixer.aux2,
                                           &pb.remote_mixer.main3, &pb.remote_mixer.aux3};
        s16* const wm_dpops[8] = {&pb.remote_dpop.main0, &pb.remote_dpop.aux0,
                                  &pb.remote_dpop.main1, &pb.remote_dpop.aux1,
                                  &pb.remote_dpop.main2, &pb.remote_dpop.aux2,
                                  &pb.remote_dpop.main3, &pb.remote_dpop.aux3};
        for (int n = 0; n < 8; ++n)
        {
          const u32 control = (pb.remote_mixer_control >> (2 * n)) & 3;
          if (control == 0)
            continue;
          int* out = (n & 1) ? buffers.wm_aux[n / 2] : buffers.wm_main[n / 2];
          MixAdd(out, wm_samples, wm_count, wm_volumes[n], wm_dpops[n], (control & 2) != 0);
        }
      }
    }

    // WritePB (same layout, still without `updates`).
    for (size_t i = 0; i < updates_begin / 2; ++i)
      Write16(pb_addr + static_cast<u32>(i * 2), words[i]);
    for (size_t i = updates_end / 2; i < sizeof(pb) / 2; ++i)
      Write16(pb_addr + static_cast<u32>(i * 2 - sizeof(PBUpdatesWii)), words[i]);
    pb_addr = HILO_TO_32(pb.next_pb);
  }
}

void NativeAxWii::MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr, u16 volume)
{
  std::array<u16, 96> volume_ramp;
  GenerateVolumeRamp(volume_ramp.data(), m_last_aux_volumes[aux_id], volume, volume_ramp.size());
  m_last_aux_volumes[aux_id] = volume;

  int* const main_buffers[3] = {m_samples_main_left, m_samples_main_right, m_samples_main_surround};
  const int* buffers[3] = {};
  switch (aux_id)
  {
  case 0:
    buffers[0] = m_samples_auxA_left, buffers[1] = m_samples_auxA_right,
    buffers[2] = m_samples_auxA_surround;
    break;
  case 1:
    buffers[0] = m_samples_auxB_left, buffers[1] = m_samples_auxB_right,
    buffers[2] = m_samples_auxB_surround;
    break;
  default:
    buffers[0] = m_samples_auxC_left, buffers[1] = m_samples_auxC_right,
    buffers[2] = m_samples_auxC_surround;
    break;
  }
  // Aux buffers to the CPU (its aux callback processes them next frame)...
  if (write_addr)
  {
    for (const int* buffer : buffers)
    {
      for (u32 j = 0; j < 96; ++j)
        Write32(write_addr + j * 4, static_cast<u32>(buffer[j]));
      write_addr += 96 * 4;
    }
  }
  // ...and the CPU's processed buffers back into the main mix.
  u32 at = read_addr;
  for (int* main_buffer : main_buffers)
  {
    for (u32 j = 0; j < 96; ++j, at += 4)
    {
      s64 sample = static_cast<s64>(static_cast<s32>(Read32(at)));
      sample *= volume_ramp[j];
      main_buffer[j] += static_cast<s32>(sample >> 15);
    }
  }
}

void NativeAxWii::UploadAUXMixLRSC(int aux_id, const u32* addresses, u16 volume)
{
  int* aux_left = aux_id ? m_samples_auxB_left : m_samples_auxA_left;
  int* aux_right = aux_id ? m_samples_auxB_right : m_samples_auxA_right;
  int* aux_surround = aux_id ? m_samples_auxB_surround : m_samples_auxA_surround;
  int* auxc_buffer = aux_id ? m_samples_auxC_surround : m_samples_auxC_right;

  for (u32 i = 0; i < 96; ++i)
  {
    Write32(addresses[0] + i * 4, static_cast<u32>(aux_left[i]));
    Write32(addresses[0] + (96 + i) * 4, static_cast<u32>(aux_right[i]));
    Write32(addresses[0] + (192 + i) * 4, static_cast<u32>(aux_surround[i]));
    Write32(addresses[1] + i * 4, static_cast<u32>(auxc_buffer[i]));
  }

  u16 volume_ramp[96];
  GenerateVolumeRamp(volume_ramp, m_last_aux_volumes[aux_id], volume, 96);
  m_last_aux_volumes[aux_id] = volume;

  int* mix_dest[4] = {m_samples_main_left, m_samples_main_right, m_samples_main_surround,
                      m_samples_auxC_left};
  for (u32 mix_i = 0; mix_i < 4; ++mix_i)
  {
    // The HLE reuses aux_left as the download buffer.
    for (u32 i = 0; i < 96; ++i)
      aux_left[i] = static_cast<int>(Read32(addresses[2 + mix_i] + i * 4));
    for (u32 i = 0; i < 96; ++i)
    {
      s64 sample = static_cast<s64>(static_cast<s32>(aux_left[i]));
      sample *= volume_ramp[i];
      mix_dest[mix_i][i] += static_cast<s32>(sample >> 15);
    }
  }
}

void NativeAxWii::RunCompressor(u16 threshold, u16 release_frames, u32 table_addr, u32 millis)
{
  bool triggered = false;
  for (u32 i = 0; i < 32 * millis; ++i)
  {
    if (std::abs(m_samples_main_left[i]) > int(threshold) ||
        std::abs(m_samples_main_right[i]) > int(threshold))
    {
      triggered = true;
      break;
    }
  }
  const u32 frame_byte_size = 32 * millis * sizeof(s16);
  u32 table_offset = 0;
  if (triggered)
  {
    table_offset = m_compressor_pos * frame_byte_size;
    m_compressor_pos = release_frames;
  }
  else if (m_compressor_pos)
  {
    --m_compressor_pos;
    constexpr u32 ATTACK_ENTRY_COUNT = 11;
    table_offset = (ATTACK_ENTRY_COUNT + m_compressor_pos) * frame_byte_size;
  }
  else
  {
    return;
  }
  for (u32 i = 0; i < 32 * millis; ++i)
  {
    const u16 coef = Read16(table_addr + table_offset + i * 2);
    m_samples_main_left[i] = (s64(m_samples_main_left[i]) * coef) >> 15;
    m_samples_main_right[i] = (s64(m_samples_main_right[i]) * coef) >> 15;
  }
}

void NativeAxWii::OutputSamples(u32 lr_addr, u32 surround_addr, u16 volume, bool upload_auxc)
{
  std::array<u16, 96> volume_ramp;
  GenerateVolumeRamp(volume_ramp.data(), m_last_main_volume, volume, volume_ramp.size());
  m_last_main_volume = volume;

  for (u32 i = 0; i < 96; ++i)
    Write32(surround_addr + i * 4, static_cast<u32>(m_samples_main_surround[i]));
  if (upload_auxc)
  {
    for (u32 i = 0; i < 96; ++i)
      Write32(surround_addr + (96 + i) * 4, static_cast<u32>(m_samples_auxC_left[i]));
  }
  for (size_t i = 0; i < volume_ramp.size(); ++i)
  {
    s64 left = m_samples_main_left[i];
    s64 right = m_samples_main_right[i];
    left = (left * volume_ramp[i]) >> 15;
    right = (right * volume_ramp[i]) >> 15;
    m_samples_main_left[i] = ClampS16(left);
    m_samples_main_right[i] = ClampS16(right);
  }
  // Interleaved big-endian s16, right first (the HLE's byte layout).
  for (u32 i = 0; i < 96; ++i)
  {
    Write16(lr_addr + i * 4, static_cast<u16>(static_cast<s16>(m_samples_main_right[i])));
    Write16(lr_addr + i * 4 + 2, static_cast<u16>(static_cast<s16>(m_samples_main_left[i])));
  }
  ++m_sync_mails;
}

void NativeAxWii::OutputWMSamples(const u32* addresses)
{
  int* buffers[] = {m_samples_wm0, m_samples_wm1, m_samples_wm2, m_samples_wm3};
  for (u32 i = 0; i < 4; ++i)
  {
    for (u32 j = 0; j < 3 * 6; ++j)
      Write16(addresses[i] + j * 2, static_cast<u16>(ClampS16(buffers[i][j])));
  }
}
}  // namespace StaticRecompNativeAudio
