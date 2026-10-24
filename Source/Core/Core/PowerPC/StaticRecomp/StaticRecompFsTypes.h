// RecompCore: StaticRecomp CPU core - NAND file system value types (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The value types of the Wii file system the native NAND store and the
// native /dev/fs use: result codes in IOS order, owner/group ids, access
// modes, metadata and statistics. Same values and layout as Dolphin's
// IOS::HLE::FS (Core/IOS/FS/FileSystem.h, Dolphin Emulator Project,
// GPL-2.0-or-later), so saves, savestates and the guest-visible codes match.

#pragma once

#include <expected>
#include <tuple>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompFs
{
// IOS order: the guest code is -(100 + code) (ConvertResult).
enum class ResultCode
{
  Success,
  Invalid,
  AccessDenied,
  SuperblockWriteFailed,
  SuperblockInitFailed,
  AlreadyExists,
  NotFound,
  FstFull,
  NoFreeSpace,
  NoFreeHandle,
  TooManyPathComponents,
  InUse,
  BadBlock,
  EccError,
  CriticalEccError,
  FileNotEmpty,
  CheckFailed,
  UnknownError,
  ShortRead,
};

template <typename T>
using Result = std::expected<T, ResultCode>;

using Uid = u32;
using Gid = u16;
using Fd = u32;

enum class Mode : u8
{
  None = 0,
  Read = 1,
  Write = 2,
  ReadWrite = 3,
};

enum class SeekMode : u32
{
  Set = 0,
  Current = 1,
  End = 2,
};

using FileAttribute = u8;

struct Modes
{
  Mode owner, group, other;
};
inline bool operator==(const Modes& lhs, const Modes& rhs)
{
  return std::tie(lhs.owner, lhs.group, lhs.other) == std::tie(rhs.owner, rhs.group, rhs.other);
}

struct Metadata
{
  Uid uid;
  Gid gid;
  FileAttribute attribute;
  Modes modes;
  bool is_file;
  u32 size;
  u16 fst_index;
};

struct NandStats
{
  u32 cluster_size;
  u32 free_clusters;
  u32 used_clusters;
  u32 bad_clusters;
  u32 reserved_clusters;
  u32 free_inodes;
  u32 used_inodes;
};

struct DirectoryStats
{
  u32 used_clusters;
  u32 used_inodes;
};

struct FileStatus
{
  u32 offset;
  u32 size;
};

// The IOS return code the guest sees (IPC_SUCCESS = 0, FS errors from -100).
constexpr s32 ConvertResult(ResultCode code)
{
  return code == ResultCode::Success ? 0 : -(static_cast<s32>(code) + 100);
}
}  // namespace StaticRecompFs
