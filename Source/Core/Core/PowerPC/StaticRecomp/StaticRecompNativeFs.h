// RecompCore: StaticRecomp CPU core - native NAND file system state.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal to StaticRecompNativeSdk.cpp / StaticRecompNativeFs.cpp.

#pragma once

#include <array>
#include <map>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

namespace StaticRecompNativeSdk
{
// Devices served natively besides /dev/fs: the small IOS services the game
// uses that have nothing behind them on a PC (STM, ES, NetKD, DI). Their
// replies reproduce Dolphin's IOS HLE for the requests the game makes.
enum class IpcDeviceKind : u8
{
  File = 0,
  Fs,            // /dev/fs
  StmImmediate,  // /dev/stm/immediate: VI dimming, LED mode
  StmEventHook,  // /dev/stm/eventhook: reset/power events, never answered
  Es,            // /dev/es: title id, title data directory
  KdRequest,     // /dev/net/kd/request: opened only
  KdTime,        // /dev/net/kd/time: RTC counter
  Di,            // /dev/di: opened only (reads are native through DVDRead)
};

struct NativeSdk::FsState
{
  // An IPC file descriptor the game holds for a native file (or /dev/fs).
  struct Handle
  {
    bool device = false;  // not a file: ioctls only
    IpcDeviceKind kind = IpcDeviceKind::File;
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
  u64 virtual_opens = 0, virtual_calls = 0, virtual_pending = 0, virtual_unknown = 0;
  u32 logged_paths = 0;
};
}  // namespace StaticRecompNativeSdk
