// RecompCore: StaticRecomp CPU core - native THP video decoder.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// THPVideoDecode (thp.a, 0x80305C20) turns one THP frame (baseline JPEG
// without byte stuffing, 4:2:0) into Y, U and V planes in GX I8 tile layout.
// The SDK version decodes through the locked cache with paired-single code;
// recompiled, it ran the intro movie at 0.70-0.84 of full speed. This is the
// same algorithm on the host: marker parsing, Huffman decoding, the AAN
// quantization tables, and both passes of the inverse DCT with the SDK's
// operations in the SDK's order (the row shortcuts for DC-only, one and two
// coefficient rows included). Each paired-single operation is rounded to
// single precision on its own, as the recompiled code does, and the output
// is quantized like psq_st with GQR6 (u8, scale 2^-3, clamp, truncate), so
// the planes are the bytes the original writes.
//
// Reference for the structure: the RVL_SDK THPDec.c decompilation in the
// SMGCommunity/Petari project (CC0), checked against this game's code.
// MODERNGEKKO_NATIVE_THP=off keeps the original; =verify runs the original and
// compares its planes with the native ones at its return.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeThp.h"

// Each operation rounds to single precision on its own (no contraction into
// fused multiply-add), as the recompiled paired-single code does.
#pragma float_control(precise, on)
#pragma fp_contract(off)

namespace StaticRecompNativeSdk
{
namespace
{
constexpr u32 kSda = 0x806370A0;
constexpr u32 kThpVideoDecode = 0x80305C20;
constexpr u32 kThpInitFlag = kSda - 0x5CE0;  // __THPInitFlag
constexpr u32 kAanScaleFactor = 0x8036D178;  // f64 __THPAANScaleFactor[8]
constexpr u32 kJpegNaturalOrder = 0x8036D128;  // u8 __THPJpegNaturalOrder[80]
constexpr u32 kHid2LockedCache = 0x10000000;
constexpr s64 kThpCycles = 20000;

constexpr std::array<CodeFingerprint, 15> kThpCode = {{
    {0x80305C20u, 704u, 0xE546D871D0109609ull},   // THPVideoDecode
    {0x80305EE0u, 308u, 0xEA866E4AC699898Cull},   // __THPReadFrameHeader
    {0x80306014u, 276u, 0x371FE742EBCBF27Aull},   // __THPReadScaneHeader
    {0x80306128u, 920u, 0x48EAAAAE993E79A2ull},   // __THPReadQuantizationTable
    {0x803064C0u, 964u, 0x6217AE6F43C909DBull},   // __THPReadHuffmanTableSpecification
    {0x80306884u, 584u, 0x25617532293A6A40ull},   // __THPPrepBitStream
    {0x80306ACCu, 260u, 0x52FBDA71CC1600CAull},   // __THPDecompressYUV
    {0x80306BD0u, 588u, 0xB86B968ED60C5A80ull},   // __THPDecompressiMCURow512x448
    {0x80306E1Cu, 1164u, 0x1A7596E91AFA62FCull},  // __THPInverseDCTNoYPos
    {0x803072A8u, 1172u, 0x26649259000CC70Cull},  // __THPInverseDCTY8
    {0x8030773Cu, 592u, 0xD901E63AE3605DFDull},   // __THPDecompressiMCURow640x480
    {0x8030798Cu, 604u, 0xEAA33AD589FE49E7ull},   // __THPDecompressiMCURowNxN
    {0x80307BE8u, 1628u, 0x52CDC352AE8A9BABull},  // __THPHuffDecodeDCTCompY
    {0x80308244u, 1672u, 0xC0DA93510BE11707ull},  // __THPHuffDecodeDCTCompU
    {0x803088CCu, 1672u, 0x2CBB4F0FBDF0EC57ull},  // __THPHuffDecodeDCTCompV
}};

struct Pair
{
  float a, b;
};
inline Pair Add(Pair x, Pair y)
{
  return {x.a + y.a, x.b + y.b};
}
inline Pair Sub(Pair x, Pair y)
{
  return {x.a - y.a, x.b - y.b};
}
inline Pair Mul(Pair x, Pair c)
{
  return {x.a * c.a, x.b * c.b};
}
// ps_madd d = a * c + b, ps_msub d = a * c - b, ps_nmsub d = -(a * c - b):
// the product is rounded before the addition.
inline Pair Madd(Pair x, Pair c, Pair y)
{
  float p0 = x.a * c.a;
  float p1 = x.b * c.b;
  p0 += y.a;
  p1 += y.b;
  return {p0, p1};
}
inline Pair Msub(Pair x, Pair c, Pair y)
{
  float p0 = x.a * c.a;
  float p1 = x.b * c.b;
  p0 -= y.a;
  p1 -= y.b;
  return {p0, p1};
}
inline Pair Nmsub(Pair x, Pair c, Pair y)
{
  const Pair m = Msub(x, c, y);
  return {-m.a, -m.b};
}
inline Pair M00(Pair x, Pair y)
{
  return {x.a, y.a};
}
inline Pair M01(Pair x, Pair y)
{
  return {x.a, y.b};
}
inline Pair M10(Pair x, Pair y)
{
  return {x.b, y.a};
}
inline Pair M11(Pair x, Pair y)
{
  return {x.b, y.b};
}
inline Pair Splat(float v)
{
  return {v, v};
}

// psq_st with GQR6: u8, scale 2^-3, clamped in f32, truncated (NaN gives 0).
inline u8 QuantizeU8(float v)
{
  const float conv = v * 0.125f;
  if (conv != conv)
    return 0;
  if (conv <= 0.0f)
    return 0;
  if (conv >= 255.0f)
    return 255;
  return static_cast<u8>(static_cast<s32>(conv));
}

struct HuffmanTable
{
  bool valid = false;
  std::array<s32, 18> max_code{};
  std::array<s32, 18> val_ptr{};
  const u8* values = nullptr;  // Vij, inside the frame
};

struct Component
{
  u8 quant = 0, dc_table = 0, ac_table = 0;
  s16 pred_dc = 0;
};

// MSB-first reader over the frame (no byte stuffing in THP).
struct BitReader
{
  const u8* data = nullptr;
  size_t size = 0;
  size_t bit = 0;

  u32 Bit()
  {
    const size_t byte = bit >> 3;
    const u32 value = byte < size ? (data[byte] >> (7 - (bit & 7))) & 1 : 0;
    ++bit;
    return value;
  }
  u32 Bits(u32 n)
  {
    u32 value = 0;
    for (u32 i = 0; i < n; ++i)
      value = (value << 1) | Bit();
    return value;
  }
  void AlignByte() { bit = (bit + 7) & ~size_t(7); }
};

s32 Decode(BitReader& r, const HuffmanTable& h)
{
  s32 code = 0;
  for (int l = 1; l <= 17; ++l)
  {
    code = (code << 1) | static_cast<s32>(r.Bit());
    if (code <= h.max_code[l])
      return h.values[code + h.val_ptr[l]];
  }
  return 0;
}
}  // namespace

struct ThpFrame
{
  u16 width = 0, height = 0;
  bool rst = false;
  u16 n_mcu = 0;
  std::array<std::array<float, 64>, 4> quant{};
  std::array<HuffmanTable, 8> huffman{};
  u32 valid_tables = 0;
  std::array<Component, 3> comp{};
  size_t scan = 0;  // offset of the entropy-coded data
};

namespace
{
// __THPReadFrameHeader / ScaneHeader / QuantizationTable / HuffmanTable and
// the marker loop of THPVideoDecode. False: anything the SDK would reject (the
// original then returns its error code).
bool ParseFrame(const u8* f, size_t size, const double* aan, const u8* natural, ThpFrame& out)
{
  size_t c = 0;
  const auto need = [&](size_t n) { return c + n <= size; };
  for (;;)
  {
    if (!need(2) || f[c++] != 0xFF)
      return false;
    while (need(1) && f[c] == 0xFF)
      ++c;
    if (!need(1))
      return false;
    const u8 status = f[c++];
    if (status == 0xC4)  // DHT
    {
      if (!need(2))
        return false;
      s32 length = ((f[c] << 8) | f[c + 1]) - 2;
      c += 2;
      while (length > 0)
      {
        if (!need(17))
          return false;
        const u8 i = f[c++];
        const u8 index = static_cast<u8>(((i & 15) << 1) + (i >> 4));
        if (index >= 8)
          return false;
        const u8* bits = f + c;
        u32 count = 0;
        for (int k = 0; k < 16; ++k)
          count += f[c++];
        if (!need(count))
          return false;
        HuffmanTable& h = out.huffman[index];
        h.values = f + c;
        c += count;
        // Size and code tables, then the decoder tables.
        std::array<u16, 257> codes{};
        std::array<u8, 257> sizes{};
        u32 p = 0;
        for (int l = 1; l <= 16; ++l)
          for (int n = bits[l - 1]; n-- > 0 && p < 256;)
            sizes[p++] = static_cast<u8>(l);
        sizes[p] = 0;
        u16 code = 0;
        u8 si = sizes[0];
        p = 0;
        while (sizes[p])
        {
          while (sizes[p] == si)
            codes[p++] = code++;
          code = static_cast<u16>(code << 1);
          ++si;
        }
        s32 q = 0;
        for (int l = 1; l <= 16; ++l)
        {
          if (bits[l - 1])
          {
            h.val_ptr[l] = q - codes[q];
            q += bits[l - 1];
            h.max_code[l] = codes[q - 1];
          }
          else
          {
            h.max_code[l] = -1;
            h.val_ptr[l] = -1;
          }
        }
        h.max_code[17] = 0xFFFFF;
        h.valid = true;
        out.valid_tables |= 1u << index;
        length -= 17 + static_cast<s32>(count);
      }
      if (length != 0)
        return false;
    }
    else if (status == 0xC0)  // SOF0
    {
      if (!need(8) || f[c + 2] != 8 || f[c + 7] != 3)
        return false;
      out.height = static_cast<u16>((f[c + 3] << 8) | f[c + 4]);
      out.width = static_cast<u16>((f[c + 5] << 8) | f[c + 6]);
      c += 8;
      for (int i = 0; i < 3; ++i)
      {
        if (!need(3))
          return false;
        const u8 sampling = f[c + 1];
        if ((i == 0 && sampling != 0x22) || (i > 0 && sampling != 0x11))
          return false;
        out.comp[i].quant = f[c + 2];
        if (out.comp[i].quant >= 4)
          return false;
        c += 3;
      }
    }
    else if (status == 0xDD)  // DRI
    {
      if (!need(4))
        return false;
      out.rst = true;
      out.n_mcu = static_cast<u16>((f[c + 2] << 8) | f[c + 3]);
      c += 4;
    }
    else if (status == 0xDB)  // DQT
    {
      if (!need(2))
        return false;
      s32 length = ((f[c] << 8) | f[c + 1]) - 2;
      c += 2;
      while (length > 0)
      {
        if (!need(65))
          return false;
        const u8 id = f[c++];
        if (id >= 4)
          return false;
        std::array<float, 64> q_temp{};
        for (int i = 0; i < 64; ++i)
          q_temp[natural[i]] = static_cast<float>(f[c++]);
        int i = 0;
        for (int row = 0; row < 8; ++row)
          for (int col = 0; col < 8; ++col, ++i)
            out.quant[id][i] =
                static_cast<float>(aan[col] * (static_cast<double>(q_temp[i]) * aan[row]));
        length -= 65;
      }
      if (length != 0)
        return false;
    }
    else if (status == 0xDA)  // SOS
    {
      if (!need(3) || f[c + 2] != 3)
        return false;
      c += 3;
      for (int i = 0; i < 3; ++i)
      {
        if (!need(2))
          return false;
        const u8 sel = f[c + 1];
        out.comp[i].dc_table = static_cast<u8>(sel >> 4);
        out.comp[i].ac_table = static_cast<u8>(sel & 15);
        if (!(out.valid_tables & (1u << ((sel >> 4) << 1))) ||
            !(out.valid_tables & (1u << (((sel & 15) << 1) + 1))))
          return false;
        c += 2;
      }
      c += 3;
      out.scan = c;
      return out.width != 0 && out.height != 0;
    }
    else if (status == 0xD8)
    {
    }
    else if ((status >= 0xE0 && status <= 0xEF) || status == 0xFE)
    {
      if (!need(2))
        return false;
      c += (f[c] << 8) | f[c + 1];
    }
    else
    {
      return false;
    }
  }
}

// __THPHuffDecodeDCTComp*: DC difference and AC run/size pairs.
void DecodeBlock(BitReader& r, ThpFrame& fr, Component& comp, const u8* natural, s16* block)
{
  std::memset(block, 0, 64 * sizeof(s16));
  const HuffmanTable& dc = fr.huffman[comp.dc_table << 1];
  const HuffmanTable& ac = fr.huffman[(comp.ac_table << 1) + 1];
  const s32 t = Decode(r, dc);
  s16 diff = 0;
  if (t)
  {
    diff = static_cast<s16>(r.Bits(static_cast<u32>(t)));
    if (static_cast<u32>(std::countl_zero(static_cast<u32>(static_cast<s32>(diff)))) >
        static_cast<u32>(32 - t))
      diff = static_cast<s16>(diff + static_cast<s32>((0xFFFFFFFFu << t) + 1));
  }
  comp.pred_dc = static_cast<s16>(comp.pred_dc + diff);
  block[0] = comp.pred_dc;
  for (s32 k = 1; k < 64; ++k)
  {
    const s32 symbol = Decode(r, ac);
    const s32 size = symbol & 15;
    const s32 run = symbol >> 4;
    if (size == 0)
    {
      if (run != 15)
        break;
      k += 15;
      continue;
    }
    k += run;
    s32 value = static_cast<s32>(r.Bits(static_cast<u32>(size)));
    if (static_cast<u32>(std::countl_zero(static_cast<u32>(value))) > static_cast<u32>(32 - size))
      value += static_cast<s32>((0xFFFFFFFFu << size) + 1);
    block[natural[std::min(k, 79)]] = static_cast<s16>(value);
  }
}

// __THPInverseDCTNoYPos / __THPInverseDCTY8: rows (with the SDK's shortcuts)
// into the workspace, then columns quantized into 8x4 I8 tiles at `out`
// (`wid` pixels per line; out + wid * 4 is the next tile row).
void InverseDct(const s16* in, const float* q, u8* out, u32 wid)
{
  const Pair cc4 = Splat(1.414213562F);
  const Pair cc2 = Splat(1.847759065F);
  const Pair cc2c6s = Splat(1.082392200F);
  const Pair cc2c6a = Splat(-2.613125930F);
  const Pair bias = Splat(1024.0F);
  float ws[64];
  const auto ld16 = [](const s16* p) { return Pair{static_cast<float>(p[0]), static_cast<float>(p[1])}; };
  const auto ldf = [](const float* p) { return Pair{p[0], p[1]}; };

  float* row = ws;
  for (int r = 0; r < 8; ++r, in += 8, q += 8, row += 8)
  {
    const auto st = [&](int pair, Pair v) {
      row[2 * pair] = v.a;
      row[2 * pair + 1] = v.b;
    };
    Pair tmp10 = Mul(ld16(in), ldf(q));
    const bool high = (in[4] | in[5] | in[6] | in[7]) != 0;
    const bool mid = (in[2] | in[3]) != 0;
    const bool one = in[1] != 0;
    if (high)
    {
      // _regularIDCT
      Pair tmp9 = Mul(ld16(in + 2), ldf(q + 2));
      Pair tmp2 = ld16(in + 4);
      Pair tmp6 = ldf(q + 4);
      Pair tmp0 = M01(tmp10, tmp9);
      Pair tmp3 = ld16(in + 6);
      Pair tmp1 = M01(tmp9, tmp10);
      Pair tmp7 = ldf(q + 6);
      Pair tmp4 = Madd(tmp2, tmp6, tmp0);
      Pair tmp5 = Nmsub(tmp2, tmp6, tmp0);
      tmp6 = Madd(tmp3, tmp7, tmp1);
      tmp7 = Nmsub(tmp3, tmp7, tmp1);
      tmp0 = Add(tmp4, tmp6);
      tmp3 = Sub(tmp4, tmp6);
      tmp2 = Msub(tmp7, cc4, tmp6);
      Pair tmp8 = Sub(tmp7, tmp5);
      tmp1 = Add(tmp5, tmp2);
      tmp2 = Sub(tmp5, tmp2);
      tmp8 = Mul(tmp8, cc2);
      tmp1 = M00(tmp0, tmp1);
      tmp6 = Nmsub(tmp5, cc2c6a, tmp8);
      tmp4 = Msub(tmp7, cc2c6s, tmp8);
      tmp6 = Sub(tmp6, tmp0);
      tmp2 = M00(tmp2, tmp3);
      tmp5 = Madd(tmp3, cc4, tmp6);
      tmp7 = M11(tmp0, tmp6);
      tmp4 = Sub(tmp4, tmp5);
      tmp3 = Add(tmp1, tmp7);
      tmp4 = M11(tmp5, tmp4);
      tmp0 = Sub(tmp1, tmp7);
      tmp5 = Add(tmp2, tmp4);
      tmp6 = Sub(tmp2, tmp4);
      tmp5 = M10(tmp5, tmp5);
      tmp0 = M10(tmp0, tmp0);
      st(0, tmp3);
      st(1, tmp6);
      st(2, tmp5);
      st(3, tmp0);
    }
    else if (mid)
    {
      // _halfIDCT
      Pair tmp1 = Mul(ld16(in + 2), ldf(q + 2));
      Pair tmp3 = Sub(tmp10, tmp1);
      Pair tmp2 = Add(tmp10, tmp1);
      Pair tmp4 = Madd(tmp1, cc4, tmp3);
      Pair tmp5 = Nmsub(tmp1, cc4, tmp2);
      Pair tmp8 = Mul(tmp3, cc2);
      tmp4 = M00(tmp2, tmp4);
      Pair tmp6 = Nmsub(tmp1, cc2c6a, tmp8);
      tmp5 = M00(tmp5, tmp3);
      tmp6 = Sub(tmp6, tmp2);
      Pair tmp7 = Nmsub(tmp10, cc2c6s, tmp8);
      tmp2 = M11(tmp2, tmp6);
      tmp8 = Msub(tmp3, cc4, tmp6);
      Pair tmp9 = Add(tmp4, tmp2);
      tmp7 = Sub(tmp7, tmp8);
      tmp3 = M11(tmp8, tmp7);
      tmp4 = Sub(tmp4, tmp2);
      Pair tmp0 = Add(tmp5, tmp3);
      tmp1 = Sub(tmp5, tmp3);
      tmp1 = M10(tmp1, tmp1);
      tmp4 = M10(tmp4, tmp4);
      st(0, tmp9);
      st(1, tmp0);
      st(2, tmp1);
      st(3, tmp4);
    }
    else if (one)
    {
      // _quarterIDCT
      Pair tmp2 = Msub(tmp10, cc2, tmp10);
      Pair tmp9 = M00(tmp10, tmp10);
      Pair tmp1 = Sub(cc2, cc2c6s);
      Pair tmp3 = Msub(tmp10, cc4, tmp2);
      Pair tmp5 = M11(tmp10, tmp2);
      Pair tmp4 = Nmsub(tmp10, tmp1, tmp3);
      Pair tmp7 = Add(tmp9, tmp5);
      Pair tmp6 = M11(tmp3, tmp4);
      tmp5 = Sub(tmp9, tmp5);
      Pair tmp8 = Add(tmp9, tmp6);
      tmp6 = Sub(tmp9, tmp6);
      tmp6 = M10(tmp6, tmp6);
      tmp5 = M10(tmp5, tmp5);
      st(0, tmp7);
      st(1, tmp8);
      st(2, tmp6);
      st(3, tmp5);
    }
    else
    {
      // DC only: the row is the dequantized DC.
      const Pair tmp0 = M00(tmp10, tmp10);
      for (int pair = 0; pair < 4; ++pair)
        st(pair, tmp0);
    }
  }

  // Columns, two at a time: rows k of columns (2j, 2j+1) are ws[8k + 2j].
  const auto col = [&](int k, int j) { return Pair{ws[8 * k + 2 * j], ws[8 * k + 2 * j + 1]}; };
  u8* out1 = out + wid * 4;
  const auto put = [](u8* p, Pair v) {
    p[0] = QuantizeU8(v.a);
    p[1] = QuantizeU8(v.b);
  };
  for (int j = 0; j < 4; ++j)
  {
    const Pair tmp10 = col(0, j), tmp11 = col(4, j), tmp12 = col(2, j), tmp13 = col(6, j);
    Pair tmp6 = Add(tmp10, tmp11);
    Pair tmp8 = Sub(tmp10, tmp11);
    tmp6 = Add(tmp6, bias);
    Pair tmp7 = Add(tmp12, tmp13);
    Pair tmp9 = Sub(tmp12, tmp13);
    const Pair tmp0 = Add(tmp6, tmp7);
    tmp8 = Add(tmp8, bias);

    Pair tmp4 = col(1, j);
    tmp9 = Msub(tmp9, cc4, tmp7);
    Pair tmp5 = col(3, j);
    const Pair tmp3 = Sub(tmp6, tmp7);
    const Pair tmp1 = Add(tmp8, tmp9);
    tmp6 = col(5, j);
    const Pair tmp2 = Sub(tmp8, tmp9);
    tmp7 = col(7, j);
    tmp8 = Add(tmp6, tmp5);
    tmp6 = Sub(tmp6, tmp5);
    tmp9 = Add(tmp4, tmp7);
    tmp4 = Sub(tmp4, tmp7);
    tmp7 = Add(tmp9, tmp8);
    tmp5 = Sub(tmp9, tmp8);
    tmp8 = Add(tmp6, tmp4);
    Pair out0 = Add(tmp0, tmp7);
    tmp8 = Mul(tmp8, cc2);
    const Pair tmp23 = Sub(tmp0, tmp7);
    tmp6 = Madd(tmp6, cc2c6a, tmp8);
    tmp6 = Sub(tmp6, tmp7);
    put(out + 2 * j, out0);
    tmp4 = Msub(tmp4, cc2c6s, tmp8);
    out0 = Add(tmp1, tmp6);
    tmp5 = Msub(tmp5, cc4, tmp6);
    const Pair tmp22 = Sub(tmp1, tmp6);
    put(out + 8 + 2 * j, out0);
    out0 = Add(tmp2, tmp5);
    tmp4 = Add(tmp4, tmp5);
    put(out + 16 + 2 * j, out0);
    out0 = Sub(tmp3, tmp4);
    const Pair tmp20 = Add(tmp3, tmp4);
    put(out + 24 + 2 * j, out0);
    const Pair tmp21 = Sub(tmp2, tmp5);
    put(out1 + 2 * j, tmp20);
    put(out1 + 8 + 2 * j, tmp21);
    put(out1 + 16 + 2 * j, tmp22);
    put(out1 + 24 + 2 * j, tmp23);
  }
}

// __THPDecompressYUV and the MCU row functions: the whole frame into host
// planes (tile layout, as the SDK stores each 16-line row).
void DecodeFrame(const u8* f, size_t size, ThpFrame& fr, const u8* natural, std::vector<u8>& y,
                 std::vector<u8>& u, std::vector<u8>& v)
{
  const u32 w = fr.width, h = fr.height;
  y.assign(size_t(w) * h, 0);
  u.assign(size_t(w / 2) * (h / 2), 0);
  v.assign(size_t(w / 2) * (h / 2), 0);
  BitReader r{f + fr.scan, size - fr.scan, 0};
  for (Component& c : fr.comp)
    c.pred_dc = 0;
  u32 curr_mcu = fr.n_mcu;
  alignas(32) s16 blocks[6][64];
  const u32 mcus_per_row = (w + 15) / 16;
  for (u32 row = 0; row < h; row += 16)
  {
    u8* ybase = y.data() + size_t(row) * w;
    u8* ubase = u.data() + size_t(row / 2) * (w / 2);
    u8* vbase = v.data() + size_t(row / 2) * (w / 2);
    for (u32 m = 0; m < mcus_per_row; ++m)
    {
      for (int b = 0; b < 4; ++b)
        DecodeBlock(r, fr, fr.comp[0], natural, blocks[b]);
      DecodeBlock(r, fr, fr.comp[1], natural, blocks[4]);
      DecodeBlock(r, fr, fr.comp[2], natural, blocks[5]);
      const u32 x = m * 16;
      const float* qy = fr.quant[fr.comp[0].quant].data();
      InverseDct(blocks[0], qy, ybase + x * 4, w);
      InverseDct(blocks[1], qy, ybase + (x + 8) * 4, w);
      InverseDct(blocks[2], qy, ybase + w * 8 + x * 4, w);
      InverseDct(blocks[3], qy, ybase + w * 8 + (x + 8) * 4, w);
      InverseDct(blocks[4], fr.quant[fr.comp[1].quant].data(), ubase + (x / 2) * 4, w / 2);
      InverseDct(blocks[5], fr.quant[fr.comp[2].quant].data(), vbase + (x / 2) * 4, w / 2);
      if (fr.rst && --curr_mcu == 0)
      {
        curr_mcu = fr.n_mcu;
        r.AlignByte();
        for (Component& c : fr.comp)
          c.pred_dc = 0;
      }
    }
  }
}
}  // namespace

bool NativeSdk::ThpBind(const std::function<bool(u32)>& enable, bool wanted)
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_THP");
  const std::string mode = setting ? setting : "";
  if (!wanted || mode == "off" || mode == "0" || !enable(kThpVideoDecode))
    return false;
  m_thp = std::make_unique<ThpState>();
  m_thp->verify = mode == "verify";
  std::fprintf(stderr, "[native_thp] THP video decoder%s\n",
               m_thp->verify ? " (verify: original runs, planes compared)" : "");
  return true;
}

bool NativeSdk::ThpReadGuest(const CPUState& guest, u32 address, size_t size, std::vector<u8>& out) const
{
  out.resize(size);
  size_t done = 0;
  while (done < size)
  {
    const u32 at = address + static_cast<u32>(done);
    const u32 page_left = PowerPC::BAT_PAGE_SIZE - (at & (PowerPC::BAT_PAGE_SIZE - 1));
    const u32 n = static_cast<u32>(std::min<size_t>(size - done, page_left));
    const u8* p = Pointer(guest, at, n);
    if (!p)
      return false;
    std::memcpy(out.data() + done, p, n);
    done += n;
  }
  return true;
}

bool NativeSdk::ThpWriteGuest(const CPUState& guest, u32 address, const std::vector<u8>& data) const
{
  size_t done = 0;
  while (done < data.size())
  {
    const u32 at = address + static_cast<u32>(done);
    const u32 page_left = PowerPC::BAT_PAGE_SIZE - (at & (PowerPC::BAT_PAGE_SIZE - 1));
    const u32 n = static_cast<u32>(std::min<size_t>(data.size() - done, page_left));
    u8* p = Pointer(guest, at, n);
    if (!p)
      return false;
    std::memcpy(p, data.data() + done, n);
    done += n;
  }
  return true;
}

bool NativeSdk::ThpDecodeFrame(const CPUState& guest, ThpState& thp)
{
  const u32 file = guest.gpr[3];
  if (!file || !guest.gpr[4] || !guest.gpr[5] || !guest.gpr[6] || !guest.gpr[7] ||
      !(guest.hid2 & kHid2LockedCache))
    return false;
  u32 init = 0;
  if (!Read32(guest, kThpInitFlag, init) || !init)
    return false;
  // The frame size is not passed: read up to the end of its page run, which
  // covers any THP frame the game buffers (the decoder never reads past it).
  constexpr size_t kMaxFrame = 0x100000;
  if (!ThpReadGuest(guest, file, kMaxFrame, thp.frame))
  {
    size_t size = kMaxFrame;
    while (size > 0x1000 && !ThpReadGuest(guest, file, size, thp.frame))
      size /= 2;
    if (size <= 0x1000)
      return false;
  }
  ThpFrame fr;
  if (!ParseFrame(thp.frame.data(), thp.frame.size(), thp.aan.data(), thp.natural.data(), fr))
    return false;
  if (fr.width % 16 || fr.height % 16 || fr.width > 1024 || fr.height > 1024)
    return false;
  DecodeFrame(thp.frame.data(), thp.frame.size(), fr, thp.natural.data(), thp.y, thp.u, thp.v);
  thp.width = fr.width;
  thp.height = fr.height;
  return true;
}

bool NativeSdk::ThpRun(CPUState& guest)
{
  if (guest.pc != kThpVideoDecode)
    return false;
  ThpState& thp = *m_thp;
  if (thp.abandoned)
    return false;
  if (!thp.ready)
  {
    // The decoder's code and tables must be the original ones.
    bool ok = guest.gpr[13] == kSda;
    std::vector<u8> table;
    if (ok && ThpReadGuest(guest, kAanScaleFactor, 64, table))
    {
      for (int i = 0; i < 8; ++i)
      {
        u64 bits = 0;
        for (int b = 0; b < 8; ++b)
          bits = (bits << 8) | table[i * 8 + b];
        std::memcpy(&thp.aan[i], &bits, 8);
      }
    }
    else
    {
      ok = false;
    }
    if (ok && ThpReadGuest(guest, kJpegNaturalOrder, 80, table))
      std::copy(table.begin(), table.end(), thp.natural.begin());
    else
      ok = false;
    if (!ok || !CodeMatches(guest, kThpCode))
    {
      std::fprintf(stderr, "[native_thp] THP library differs from the original: left to it\n");
      thp.abandoned = true;
      return false;
    }
    thp.ready = true;
  }
  if (!ThpDecodeFrame(guest, thp))
  {
    ++thp.deferred;
    return false;
  }
  if (thp.verify)
  {
    thp.verify_lr = guest.lr;
    thp.verify_sp = guest.gpr[1];
    thp.tiles = {guest.gpr[4], guest.gpr[5], guest.gpr[6]};
    return false;
  }
  if (!ThpWriteGuest(guest, guest.gpr[4], thp.y) || !ThpWriteGuest(guest, guest.gpr[5], thp.u) ||
      !ThpWriteGuest(guest, guest.gpr[6], thp.v))
  {
    ++thp.deferred;
    return false;
  }
  ++thp.frames;
  guest.gpr[3] = 0;
  guest.pc = guest.lr;
  guest.downcount -= kThpCycles;
  return true;
}

void NativeSdk::ThpFinishVerify(const CPUState& guest)
{
  ThpState& thp = *m_thp;
  thp.verify_lr = 0;
  const std::vector<u8>* planes[3] = {&thp.y, &thp.u, &thp.v};
  bool same = guest.gpr[3] == 0;
  for (int i = 0; i < 3 && same; ++i)
  {
    std::vector<u8> original;
    if (!ThpReadGuest(guest, thp.tiles[i], planes[i]->size(), original))
    {
      same = false;
      break;
    }
    for (size_t k = 0; k < original.size(); ++k)
    {
      if (original[k] != (*planes[i])[k])
      {
        if (thp.mismatches < 8)
          std::fprintf(stderr,
                       "[native_thp] frame %llu plane %d byte %zu: original %u native %u\n",
                       static_cast<unsigned long long>(thp.verified + thp.mismatches), i, k,
                       original[k], (*planes[i])[k]);
        same = false;
        break;
      }
    }
  }
  ++(same ? thp.verified : thp.mismatches);
}

void NativeSdk::PrintThpStats() const
{
  if (!m_thp)
    return;
  const ThpState& t = *m_thp;
  std::fprintf(stderr,
               "[native_thp] frames=%llu verified=%llu mismatches=%llu deferred=%llu size=%ux%u "
               "abandoned=%d\n",
               static_cast<unsigned long long>(t.frames), static_cast<unsigned long long>(t.verified),
               static_cast<unsigned long long>(t.mismatches),
               static_cast<unsigned long long>(t.deferred), t.width, t.height, t.abandoned);
}
}  // namespace StaticRecompNativeSdk
