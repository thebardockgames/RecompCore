// RecompCore: StaticRecomp CPU core - user mods state.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal to StaticRecompNativeSdk.cpp / StaticRecompNativeMods.cpp.

#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"

namespace StaticRecompNativeSdk
{
struct NativeSdk::ModsState
{
  // An AFS archive with replaced entries, served with a virtual layout: the
  // game's AFS reader places entry i + 1 at the end of entry i rounded to
  // 0x800 (verified for both archives), so entry sizes can change and every
  // read is mapped back to the original data or to a mod file.
  struct Archive
  {
    ~Archive();
    std::string name;
    u64 disc_offset = 0;  // original place on the disc
    u32 disc_size = 0;
    u64 virt_base = 0;  // new place, past the end of the disc
    FILE* file = nullptr;  // the extracted archive
    u32 count = 0, data_start = 0;
    std::vector<u32> orig_offset, orig_size, virt_offset, virt_size;
    std::vector<int> source;  // -1: original; >= 0: index in `paths`; -2: dynamic song slot
    std::vector<std::string> paths;
    u32 dir_offset = 0, dir_size = 0, virt_dir_offset = 0, virt_end = 0;
    std::vector<u8> index;  // the corrected index (header, entries, directory pointer)
  };
  struct Song
  {
    u32 id = 0;  // BGM Test id (20..23, 26..55)
    u32 size = 0;
    std::string path;
  };
  std::vector<Archive> archives;
  std::vector<Song> songs;
  int dynamic_song = -1;  // index in `songs` served by the dynamic slot
  bool hooks = false;     // Bgm_Play / BGM Test hooks enabled in the module
  u64 remapped_rows = 0, dynamic_plays = 0, unlock_calls = 0, mod_reads = 0;
  u32 logged_reads = 0;
};
}  // namespace StaticRecompNativeSdk
