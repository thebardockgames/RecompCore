// RecompCore: StaticRecomp CPU core - native NAND file system state.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal to StaticRecompNativeSdk.cpp / StaticRecompNativeFs.cpp.

#pragma once

#include <array>
#include <map>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

namespace StaticRecompNativeSdk
{
struct NativeSdk::FsState
{
  // An IPC file descriptor the game holds for a native file (or /dev/fs).
  struct Handle
  {
    bool device = false;  // /dev/fs: ioctls only
    u32 fs_fd = 0;        // FS::FileSystem handle of a file
    u32 uid = 0;
    u16 gid = 0;
    std::array<char, 64> path{};
  };
  // Native descriptors start above the IOS ones (IOS hands out 0..31).
  static constexpr u32 kFirstFd = 0x100;

  bool active = false;     // checks passed at the first call
  bool abandoned = false;  // checks failed: the original IPC code runs
  std::map<u32, Handle> handles;
  u32 next_fd = kFirstFd;

  u64 opens = 0, opens_original = 0, reads = 0, writes = 0, seeks = 0, ioctls = 0, ioctlvs = 0;
  u64 closes = 0, read_bytes = 0, written_bytes = 0, async = 0, errors = 0;
  u32 logged_paths = 0;
};
}  // namespace StaticRecompNativeSdk
