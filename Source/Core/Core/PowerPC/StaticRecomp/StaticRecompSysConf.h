// RecompCore: StaticRecomp CPU core - the Wii's SYSCONF (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// /shared2/sys/SYSCONF, the system settings file the SDK's SC library reads
// (language, country, aspect ratio, progressive scan, sound mode, sensor bar,
// Wii Remote pairings...): the same format, default entries and write
// procedure (/tmp/SYSCONF renamed into place) as Dolphin's SysConf
// (Core/Wii/SysConf.cpp, Dolphin Emulator Project, GPL-2.0-or-later), over the
// port's NAND store.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"

namespace StaticRecompNativeNand
{
class NandStore;
}

namespace StaticRecompSysConf
{
class SysConf
{
public:
  struct Entry
  {
    enum class Type : u8
    {
      BigArray = 1,
      SmallArray = 2,
      Byte = 3,
      Short = 4,
      Long = 5,
      LongLong = 6,
      ByteBool = 7,
    };
    Entry(Type type, std::string name);
    Entry(Type type, std::string name, std::vector<u8> bytes);
    Type type;
    std::string name;
    std::vector<u8> bytes;
  };

  // Loads the NAND's file, or the default entries when it is missing or invalid.
  explicit SysConf(StaticRecompNativeNand::NandStore& nand);

  bool Save() const;
  Entry* GetEntry(std::string_view key);
  Entry* GetOrAddEntry(std::string_view key, Entry::Type type);
  void SetByte(std::string_view key, Entry::Type type, u8 value);
  void SetLong(std::string_view key, Entry::Type type, u32 value);
  const std::vector<Entry>& Entries() const { return m_entries; }

private:
  void Load();
  bool LoadFromFile(u32 fd);
  void InsertDefaultEntries();

  StaticRecompNativeNand::NandStore& m_nand;
  std::vector<Entry> m_entries;
};

// What the oracle leaves in a fresh NAND before the game runs (its IOS
// configuration writer): the existing file or the defaults, IPL.SND (sound
// mode) and IPL.CB set, saved. `sound_mode`: 1 = stereo.
bool WriteBootSettings(StaticRecompNativeNand::NandStore& nand, u8 sound_mode = 1);
}  // namespace StaticRecompSysConf
