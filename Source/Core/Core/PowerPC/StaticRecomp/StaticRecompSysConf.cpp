// RecompCore: StaticRecomp CPU core - the Wii's SYSCONF (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompSysConf.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeNand.h"

namespace StaticRecompSysConf
{
namespace
{
using StaticRecompNativeNand::Mode;
using StaticRecompNativeNand::Modes;
using StaticRecompNativeNand::ResultCode;
using StaticRecompNativeNand::SeekMode;

constexpr size_t kSysconfSize = 0x4000;
constexpr u32 kSystemMenuUid = 0x1000;
constexpr u16 kSystemMenuGid = 1;

size_t NonArraySize(SysConf::Entry::Type type)
{
  switch (type)
  {
  case SysConf::Entry::Type::Byte:
  case SysConf::Entry::Type::ByteBool:
    return 1;
  case SysConf::Entry::Type::Short:
    return 2;
  case SysConf::Entry::Type::Long:
    return 4;
  case SysConf::Entry::Type::LongLong:
    return 8;
  default:
    return 0;
  }
}

void AppendBe16(std::vector<u8>* out, u16 value)
{
  out->push_back(static_cast<u8>(value >> 8));
  out->push_back(static_cast<u8>(value));
}
}  // namespace

SysConf::Entry::Entry(Type type_, std::string name_) : type(type_), name(std::move(name_))
{
  if (type != Type::SmallArray && type != Type::BigArray)
    bytes.resize(NonArraySize(type));
}

SysConf::Entry::Entry(Type type_, std::string name_, std::vector<u8> bytes_)
    : type(type_), name(std::move(name_)), bytes(std::move(bytes_))
{
}

SysConf::SysConf(StaticRecompNativeNand::NandStore& nand) : m_nand(nand)
{
  Load();
}

void SysConf::Load()
{
  m_entries.clear();
  const auto fd = m_nand.OpenFile(0, 0, "/shared2/sys/SYSCONF", Mode::Read);
  bool ok = false;
  if (fd)
  {
    const auto status = m_nand.GetFileStatus(*fd);
    ok = status && status->size == kSysconfSize && LoadFromFile(*fd);
    m_nand.Close(*fd);
  }
  if (!ok)
  {
    m_entries.clear();
    InsertDefaultEntries();
  }
}

bool SysConf::LoadFromFile(u32 fd)
{
  std::vector<u8> file(kSysconfSize);
  (void)m_nand.SeekFile(fd, 0, SeekMode::Set);
  const auto read = m_nand.ReadBytesFromFile(fd, file.data(), static_cast<u32>(file.size()));
  if (!read || *read != file.size())
    return false;
  const auto be16 = [&](size_t at) { return static_cast<u16>((file[at] << 8) | file[at + 1]); };
  const u16 count = be16(4);
  for (u16 i = 0; i < count; ++i)
  {
    size_t at = be16(6 + 2 * i);
    if (at >= file.size())
      return false;
    const u8 description = file[at++];
    const auto type = static_cast<Entry::Type>((description & 0xe0) >> 5);
    const size_t name_length = (description & 0x1f) + 1;
    if (at + name_length > file.size())
      return false;
    std::string name(reinterpret_cast<const char*>(&file[at]), name_length);
    at += name_length;
    size_t data_length = 0;
    switch (type)
    {
    case Entry::Type::BigArray:
      data_length = be16(at) + 1;  // the stored u16 is length - 1
      at += 2;
      break;
    case Entry::Type::SmallArray:
      data_length = file[at] + 1u;
      at += 1;
      break;
    case Entry::Type::Byte:
    case Entry::Type::ByteBool:
    case Entry::Type::Short:
    case Entry::Type::Long:
    case Entry::Type::LongLong:
      data_length = NonArraySize(type);
      break;
    default:
      std::fprintf(stderr, "[sysconf] unknown entry type %u for %s\n", static_cast<u8>(type),
                   name.c_str());
      return false;
    }
    if (at + data_length > file.size())
      return false;
    m_entries.emplace_back(type, std::move(name),
                           std::vector<u8>(file.begin() + at, file.begin() + at + data_length));
  }
  return true;
}

bool SysConf::Save() const
{
  std::vector<u8> buffer;
  buffer.reserve(kSysconfSize);
  for (char c : {'S', 'C', 'v', '0'})
    buffer.push_back(static_cast<u8>(c));
  AppendBe16(&buffer, static_cast<u16>(m_entries.size()));
  const size_t entries_begin = buffer.size() + sizeof(u16) * (m_entries.size() + 1);
  std::vector<u8> entries;
  for (const auto& item : m_entries)
  {
    AppendBe16(&buffer, static_cast<u16>(entries_begin + entries.size()));
    entries.push_back(static_cast<u8>((static_cast<u8>(item.type) << 5) |
                                      (static_cast<u8>(item.name.size()) - 1)));
    entries.insert(entries.end(), item.name.begin(), item.name.end());
    switch (item.type)
    {
    case Entry::Type::BigArray:
      AppendBe16(&entries, static_cast<u16>(item.bytes.size() - 1));  // length - 1
      entries.insert(entries.end(), item.bytes.begin(), item.bytes.end());
      break;
    case Entry::Type::SmallArray:
      entries.push_back(static_cast<u8>(item.bytes.size() - 1));
      entries.insert(entries.end(), item.bytes.begin(), item.bytes.end());
      break;
    default:
      entries.insert(entries.end(), item.bytes.begin(), item.bytes.end());
      break;
    }
  }
  AppendBe16(&buffer, static_cast<u16>(entries_begin + entries.size()));  // past-the-end
  buffer.insert(buffer.end(), entries.begin(), entries.end());
  buffer.resize(kSysconfSize);
  const char footer[] = {'S', 'C', 'e', 'd'};
  std::memcpy(buffer.data() + buffer.size() - 4, footer, 4);

  // Written to /tmp/SYSCONF, then renamed into place.
  constexpr Modes rw{Mode::ReadWrite, Mode::ReadWrite, Mode::ReadWrite};
  const std::string temp = "/tmp/SYSCONF";
  const ResultCode created = m_nand.CreateFile(kSystemMenuUid, kSystemMenuGid, temp, 0, rw);
  if (created != ResultCode::Success && created != ResultCode::AlreadyExists)
    return false;
  const auto fd = m_nand.OpenFile(kSystemMenuUid, kSystemMenuGid, temp, Mode::ReadWrite);
  if (!fd)
    return false;
  const auto written = m_nand.WriteBytesToFile(*fd, buffer.data(), static_cast<u32>(buffer.size()));
  m_nand.Close(*fd);
  if (!written || *written != buffer.size())
    return false;
  m_nand.CreateDirectory(kSystemMenuUid, kSystemMenuGid, "/shared2/sys", 0, rw);
  return m_nand.Rename(kSystemMenuUid, kSystemMenuGid, temp, "/shared2/sys/SYSCONF") ==
         ResultCode::Success;
}

SysConf::Entry* SysConf::GetEntry(std::string_view key)
{
  const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                               [&](const Entry& e) { return e.name == key; });
  return it != m_entries.end() ? &*it : nullptr;
}

SysConf::Entry* SysConf::GetOrAddEntry(std::string_view key, Entry::Type type)
{
  if (Entry* entry = GetEntry(key))
    return entry;
  return &m_entries.emplace_back(type, std::string(key));
}

void SysConf::SetByte(std::string_view key, Entry::Type type, u8 value)
{
  Entry* entry = GetOrAddEntry(key, type);
  entry->bytes.resize(1);
  entry->bytes[0] = value;
}

void SysConf::SetLong(std::string_view key, Entry::Type type, u32 value)
{
  Entry* entry = GetOrAddEntry(key, type);
  entry->bytes = {static_cast<u8>(value >> 24), static_cast<u8>(value >> 16),
                  static_cast<u8>(value >> 8), static_cast<u8>(value)};
}

void SysConf::InsertDefaultEntries()
{
  using T = Entry::Type;
  m_entries.emplace_back(T::BigArray, "BT.DINF", std::vector<u8>(0x460 + 1));
  m_entries.emplace_back(T::BigArray, "BT.CDIF", std::vector<u8>(0x204 + 1));
  m_entries.emplace_back(T::Long, "BT.SENS", std::vector<u8>{0, 0, 0, 3});
  m_entries.emplace_back(T::Byte, "BT.BAR", std::vector<u8>{1});
  m_entries.emplace_back(T::Byte, "BT.SPKV", std::vector<u8>{0x58});
  m_entries.emplace_back(T::Byte, "BT.MOT", std::vector<u8>{1});
  // 22 bytes: 2 per character (10 at most), a null terminator, the length.
  std::vector<u8> nick = {0, 'd', 0, 'o', 0, 'l', 0, 'p', 0, 'h', 0, 'i', 0, 'n'};
  nick.resize(22);
  nick[21] = 7;
  m_entries.emplace_back(T::SmallArray, "IPL.NIK", std::move(nick));
  m_entries.emplace_back(T::Byte, "IPL.LNG", std::vector<u8>{1});
  std::vector<u8> sadr(0x1007 + 1);
  sadr[0] = 0x6c;
  m_entries.emplace_back(T::BigArray, "IPL.SADR", std::move(sadr));
  std::vector<u8> pc(0x49 + 1);
  pc[1] = 0x04;
  pc[2] = 0x14;
  m_entries.emplace_back(T::SmallArray, "IPL.PC", std::move(pc));
  m_entries.emplace_back(T::Long, "IPL.CB", std::vector<u8>{0, 0, 0, 0});
  m_entries.emplace_back(T::Byte, "IPL.AR", std::vector<u8>{1});
  m_entries.emplace_back(T::Byte, "IPL.SSV", std::vector<u8>{1});
  m_entries.emplace_back(T::ByteBool, "IPL.CD", std::vector<u8>{0});
  m_entries.emplace_back(T::ByteBool, "IPL.CD2", std::vector<u8>{0});
  m_entries.emplace_back(T::ByteBool, "IPL.EULA", std::vector<u8>{1});
  m_entries.emplace_back(T::Byte, "IPL.UPT", std::vector<u8>{2});
  m_entries.emplace_back(T::Byte, "IPL.PGS", std::vector<u8>{0});
  m_entries.emplace_back(T::Byte, "IPL.E60", std::vector<u8>{1});
  m_entries.emplace_back(T::Byte, "IPL.DH", std::vector<u8>{0});
  m_entries.emplace_back(T::Long, "IPL.INC", std::vector<u8>{0, 0, 0, 8});
  m_entries.emplace_back(T::Long, "IPL.FRC", std::vector<u8>{0, 0, 0, 0x28});
  m_entries.emplace_back(T::SmallArray, "IPL.IDL", std::vector<u8>{0, 1});
  m_entries.emplace_back(T::Long, "NET.WCFG", std::vector<u8>{0, 0, 0, 1});
  m_entries.emplace_back(T::Long, "NET.CTPC", std::vector<u8>(4));
  m_entries.emplace_back(T::Byte, "WWW.RST", std::vector<u8>{0});
  m_entries.emplace_back(T::ByteBool, "MPLS.MOVIE", std::vector<u8>{1});
}

bool WriteBootSettings(StaticRecompNativeNand::NandStore& nand, u8 sound_mode)
{
  SysConf sysconf(nand);
  sysconf.SetByte("IPL.SND", SysConf::Entry::Type::Byte, sound_mode);
  sysconf.SetLong("IPL.CB", SysConf::Entry::Type::Long, 0);
  return sysconf.Save();
}
}  // namespace StaticRecompSysConf
