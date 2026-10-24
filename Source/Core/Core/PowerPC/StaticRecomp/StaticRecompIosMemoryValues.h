// RecompCore: StaticRecomp CPU core - IOS memory values (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// What each IOS version writes to low memory when it starts (version, date,
// MEM1/MEM2 layout, IPC buffer, Hollywood revision): the values Dolphin's
// IOS::HLE took from the IOS binaries (Core/IOS/VersionInfo.cpp, Dolphin
// Emulator Project 2017, GPL-2.0-or-later), copied unchanged. The native boot
// writes them as the system menu's IOS and the title's IOS leave them; the
// boot adapter checks the copy against the chassis's table.

#pragma once

#include <array>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompNativeBoot
{
struct IosMemoryValues
{
  u16 ios_number;
  u32 ios_version;
  u32 ios_date;
  u32 mem1_physical_size;
  u32 mem1_simulated_size;
  u32 mem1_end;
  u32 mem1_arena_begin;
  u32 mem1_arena_end;
  u32 mem2_physical_size;
  u32 mem2_simulated_size;
  u32 mem2_end;
  u32 mem2_arena_begin;
  u32 mem2_arena_end;
  u32 ipc_buffer_begin;
  u32 ipc_buffer_end;
  u32 hollywood_revision;
  u32 ram_vendor;
  u32 ios_reserved_begin;
  u32 ios_reserved_end;
  u32 sysmenu_sync;
};

namespace IosValues
{
constexpr u32 MEM1_SIZE = 0x01800000;
constexpr u32 MEM1_END = 0x81800000;
constexpr u32 MEM1_ARENA_BEGIN = 0x00000000;
constexpr u32 MEM1_ARENA_END = 0x81800000;
constexpr u32 MEM2_SIZE = 0x4000000;
constexpr u32 MEM2_ARENA_BEGIN = 0x90000800;
constexpr u32 HOLLYWOOD_REVISION = 0x00000011;
constexpr u32 PLACEHOLDER = 0xDEADBEEF;
constexpr u32 RAM_VENDOR = 0x0000FF01;
constexpr u32 RAM_VENDOR_MIOS = 0xCAFEBABE;

// These values were manually extracted from the relevant IOS binaries.
// The writes are usually contained in a single function that
// mostly writes raw literals to the relevant locations.
// e.g. IOS9, version 1034, content id 0x00000006, function at 0xffff6884
constexpr std::array<IosMemoryValues, 41> kIosMemoryValues = {
    {{
         4,          0x40003,     0x81006,          MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         9,          0x9040a,     0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         11,         0xb000a,     0x102506,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         12,         0xc020e,     0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         13,         0xd0408,     0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         14,         0xe0408,     0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         15,         0xf0408,     0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         17,         0x110408,    0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         20,         0x14000c,    0x102506,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         21,         0x15040f,    0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         22,         0x16050e,    0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,    MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,   0x93400000,       MEM2_ARENA_BEGIN,
         0x933E0000, 0x933E0000,  0x93400000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, PLACEHOLDER, PLACEHOLDER,      0,
     },
     {
         28,         0x1c070f,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93800000,       MEM2_ARENA_BEGIN,
         0x937E0000, 0x937E0000, 0x93800000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93800000, 0x93820000,       0,
     },
     {
         30,         0x1e0a10,   0x40308,          MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         31,         0x1f0e18,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         33,         0x210e18,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         34,         0x220e18,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         35,         0x230e18,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         36,         0x240e18,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         37,         0x25161f,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         38,         0x26101c,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         40,         0x280911,   0x022308,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         41,         0x290e17,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         43,         0x2b0e17,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         45,         0x2d0e17,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         46,         0x2e0e17,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         48,         0x30101c,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         50,         0x321319,   0x101008,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         51,         0x331219,   0x071108,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         52,         0x34161d,   0x101008,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         53,         0x35161f,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         55,         0x37161f,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         56,         0x38161e,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         57,         0x39171f,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         58,         0x3a1820,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         59,         0x3b2421,   0x101811,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         60,         0x3c181e,   0x112408,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         61,         0x3d161e,   0x030110,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         62,         0x3e191e,   0x022712,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         70,         0x461a1f,   0x060309,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         80,         0x501b20,   0x030310,         MEM1_SIZE,
         MEM1_SIZE,  MEM1_END,   MEM1_ARENA_BEGIN, MEM1_ARENA_END,
         MEM2_SIZE,  MEM2_SIZE,  0x93600000,       MEM2_ARENA_BEGIN,
         0x935E0000, 0x935E0000, 0x93600000,       HOLLYWOOD_REVISION,
         RAM_VENDOR, 0x93600000, 0x93620000,       0,
     },
     {
         257,
         0x707,
         0x82209,
         MEM1_SIZE,
         MEM1_SIZE,
         MEM1_END,
         MEM1_ARENA_BEGIN,
         MEM1_ARENA_END,
         MEM2_SIZE,
         MEM2_SIZE,
         0x93600000,
         MEM2_ARENA_BEGIN,
         0x935E0000,
         0x935E0000,
         0x93600000,
         HOLLYWOOD_REVISION,
         RAM_VENDOR_MIOS,
         PLACEHOLDER,
         PLACEHOLDER,
         PLACEHOLDER,
     }}};
}  // namespace IosValues

inline const std::array<IosMemoryValues, 41>& GetIosMemoryValues()
{
  return IosValues::kIosMemoryValues;
}
}  // namespace StaticRecompNativeBoot
