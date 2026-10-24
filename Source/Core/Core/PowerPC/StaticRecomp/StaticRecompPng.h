// RecompCore: StaticRecomp CPU core - a minimal PNG writer (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// RGBA8 images for the diagnostic dumps, with no compression library: the
// zlib stream uses stored (uncompressed) deflate blocks. The pixels are the
// same as any other encoder's; the file bytes are not, so dumps are compared
// by decoded pixels (build/native-cpu/compare_pixels.py), not by file hash.

#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompPng
{
namespace Detail
{
inline const std::array<u32, 256>& CrcTable()
{
  static const std::array<u32, 256> table = [] {
    std::array<u32, 256> t{};
    for (u32 n = 0; n < 256; ++n)
    {
      u32 c = n;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  return table;
}

inline void Be32(std::vector<u8>& out, u32 v)
{
  out.push_back(static_cast<u8>(v >> 24));
  out.push_back(static_cast<u8>(v >> 16));
  out.push_back(static_cast<u8>(v >> 8));
  out.push_back(static_cast<u8>(v));
}

inline void Chunk(std::vector<u8>& out, const char type[4], const std::vector<u8>& data)
{
  Be32(out, static_cast<u32>(data.size()));
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  u32 crc = 0xFFFFFFFFu;
  for (size_t i = start; i < out.size(); ++i)
    crc = CrcTable()[(crc ^ out[i]) & 0xFF] ^ (crc >> 8);
  Be32(out, crc ^ 0xFFFFFFFFu);
}
}  // namespace Detail

// `rgba`: width * height pixels, 4 bytes each (R, G, B, A), rows top to bottom.
inline bool WriteRgba(const std::string& path, const u8* rgba, int width, int height)
{
  std::vector<u8> raw;
  raw.reserve(static_cast<size_t>(height) * (1 + 4 * static_cast<size_t>(width)));
  for (int y = 0; y < height; ++y)
  {
    raw.push_back(0);  // filter: none
    const u8* row = rgba + static_cast<size_t>(y) * width * 4;
    raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 4);
  }
  std::vector<u8> zlib = {0x78, 0x01};
  u32 a = 1, b = 0;
  for (u8 byte : raw)
  {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t pos = 0; pos < raw.size() || raw.empty();)
  {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n >= raw.size();
    zlib.push_back(last ? 1 : 0);
    zlib.push_back(static_cast<u8>(n));
    zlib.push_back(static_cast<u8>(n >> 8));
    zlib.push_back(static_cast<u8>(~n));
    zlib.push_back(static_cast<u8>(~n >> 8));
    zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
    if (last)
      break;
  }
  Detail::Be32(zlib, (b << 16) | a);

  std::vector<u8> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<u8> ihdr;
  Detail::Be32(ihdr, static_cast<u32>(width));
  Detail::Be32(ihdr, static_cast<u32>(height));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, no filter set, no interlace
  Detail::Chunk(png, "IHDR", ihdr);
  Detail::Chunk(png, "IDAT", zlib);
  Detail::Chunk(png, "IEND", {});
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file)
    return false;
  const bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
  std::fclose(file);
  return ok;
}
}  // namespace StaticRecompPng
