// RecompCore: StaticRecomp CPU core - native NAND store (phase C3).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The native IPC layer (StaticRecompNativeFs.cpp) served /dev/fs and the
// NAND paths through Dolphin's IOS file system object (HostFileSystem). This
// class is the store itself: the same on-disk layout (the user's Wii folder,
// escaped path components, fst.bin with the FST metadata) with the same
// semantics for permissions, modes, errors, directory listing order and the
// NAND statistics, so existing saves stay valid and savestates stay loadable.
// It does not depend on the IOS kernel; the FS value types (modes, metadata,
// result codes) are the port's (StaticRecompFsTypes.h, same values).

#pragma once

#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompFsTypes.h"

namespace StaticRecompNativeNand
{
using StaticRecompFs::DirectoryStats;
using StaticRecompFs::FileAttribute;
using StaticRecompFs::FileStatus;
using StaticRecompFs::Gid;
using StaticRecompFs::Metadata;
using StaticRecompFs::Mode;
using StaticRecompFs::Modes;
using StaticRecompFs::NandStats;
using StaticRecompFs::Result;
using StaticRecompFs::ResultCode;
using StaticRecompFs::SeekMode;
using StaticRecompFs::Uid;

class NandStore
{
public:
  static constexpr u32 kMaxHandles = 16;

  // `root` is the host folder of the Wii NAND (the user's Wii directory).
  explicit NandStore(std::string root);
  ~NandStore();

  Result<u32> OpenFile(Uid uid, Gid gid, const std::string& path, Mode mode);
  ResultCode Close(u32 fd);
  Result<u32> ReadBytesFromFile(u32 fd, u8* out, u32 count);
  Result<u32> WriteBytesToFile(u32 fd, const u8* in, u32 count);
  Result<u32> SeekFile(u32 fd, u32 offset, SeekMode mode);
  Result<FileStatus> GetFileStatus(u32 fd);

  ResultCode Format(Uid uid);
  Result<NandStats> GetNandStats();
  Result<DirectoryStats> GetDirectoryStats(const std::string& path);
  ResultCode CreateFile(Uid uid, Gid gid, const std::string& path, FileAttribute attr, Modes modes);
  ResultCode CreateDirectory(Uid uid, Gid gid, const std::string& path, FileAttribute attr,
                             Modes modes);
  ResultCode Delete(Uid uid, Gid gid, const std::string& path);
  ResultCode Rename(Uid uid, Gid gid, const std::string& old_path, const std::string& new_path);
  Result<std::vector<std::string>> ReadDirectory(Uid uid, Gid gid, const std::string& path);
  Result<Metadata> GetMetadata(Uid uid, Gid gid, const std::string& path);
  ResultCode SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                         FileAttribute attr, Modes modes);

  // Savestates: the open handles (path, mode, offset) by descriptor.
  struct HandleState
  {
    bool opened = false;
    std::string path;
    Mode mode = Mode::None;
    u32 offset = 0;
  };
  HandleState GetHandleState(u32 fd) const;
  // Reopens the host file for a descriptor from a state; false if it is gone.
  bool RestoreHandle(u32 fd, const HandleState& state);
  void CloseAll();

  const std::string& Root() const { return m_root; }

private:
  struct FstEntry
  {
    std::string name;
    Metadata data{};
    std::vector<FstEntry> children;
    bool CheckPermission(Uid caller_uid, Gid caller_gid, Mode requested) const;
  };
  struct Handle
  {
    bool opened = false;
    std::string path;  // Wii path
    Mode mode = Mode::None;
    u32 offset = 0;
    std::shared_ptr<std::FILE> file;
  };
  struct HostEntry
  {
    bool is_directory = false;
    u64 size = 0;  // file length, or recursive entry count for directories
    std::string name;  // name as the Wii sees it
    std::vector<HostEntry> children;
  };

  std::string HostPath(const std::string& wii_path) const;
  void ResetFst();
  void LoadFst();
  void ReloadFst();
  void SaveFst() const;
  FstEntry* EntryForPath(const std::string& path);
  bool IsFileOpened(const std::string& path) const;
  bool IsDirectoryInUse(const std::string& path) const;
  ResultCode CreateFileOrDirectory(Uid uid, Gid gid, const std::string& path, FileAttribute attr,
                                   Modes modes, bool is_file);
  std::shared_ptr<std::FILE> OpenHostFile(const std::string& host_path);
  HostEntry Scan(const std::string& host_path, bool recursive) const;
  u64 Fixup(HostEntry* dir, bool is_root) const;
  Handle* HandleOf(u32 fd);

  std::string m_root;
  FstEntry m_root_entry;
  std::array<Handle, kMaxHandles> m_handles;
  std::map<std::string, std::weak_ptr<std::FILE>> m_open_files;
};

std::string EscapeNandName(const std::string& name);
std::string UnescapeNandName(const std::string& name);
}  // namespace StaticRecompNativeNand
