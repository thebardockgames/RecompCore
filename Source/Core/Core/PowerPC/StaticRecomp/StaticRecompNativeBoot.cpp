// RecompCore: StaticRecomp CPU core - native boot (phase G).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeBoot.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestMemory.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompIosMemoryValues.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeNand.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompSysConf.h"

namespace StaticRecompNativeBoot
{
namespace
{
ChassisHooks s_chassis;

using StaticRecompNativeNand::Mode;
using StaticRecompNativeNand::Modes;
using StaticRecompNativeNand::NandStore;
using StaticRecompNativeNand::ResultCode;

constexpr u32 kMem1Size = 0x01800000;
constexpr u32 kMem1End = 0x81800000;
constexpr u64 kSystemMenuTitle = 0x0000000100000002ull;
constexpr u32 kKernelUid = 0;
constexpr u32 kSystemMenuUid = 0x1000;
constexpr u16 kSystemMenuGid = 1;
constexpr u32 kFirstPpcUid = 0x1000;
constexpr Modes kTitleDirModes{Mode::ReadWrite, Mode::ReadWrite, Mode::Read};
constexpr Modes kContentDirModes{Mode::ReadWrite, Mode::ReadWrite, Mode::None};
constexpr Modes kDataDirModes{Mode::ReadWrite, Mode::None, Mode::None};
constexpr Modes kSysmenuFileModes{Mode::ReadWrite, Mode::ReadWrite, Mode::ReadWrite};
constexpr Modes kInternalFileModes{Mode::ReadWrite, Mode::ReadWrite, Mode::None};

bool ReadFile(const std::string& path, std::vector<u8>& out)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

u32 Be32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}
u16 Be16(const u8* p)
{
  return static_cast<u16>((u16(p[0]) << 8) | p[1]);
}
u64 Be64(const u8* p)
{
  return (u64(Be32(p)) << 32) | Be32(p + 4);
}

std::string TitlePath(u64 title_id)
{
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "/title/%08x/%08x", static_cast<u32>(title_id >> 32),
                static_cast<u32>(title_id));
  return buffer;
}

// ---- Nintendo's setting.txt codec (rotating XOR, as the SDK decodes it) ----

constexpr u32 kSettingsSeed = 0x73B5DBFA;

class SettingsWriter
{
public:
  void Add(std::string_view key, std::string_view value)
  {
    std::string line(key);
    line += '=';
    line += value;
    line += "\r\n";
    WriteLine(line);
  }
  const std::array<u8, 0x100>& Bytes() const { return m_buffer; }

private:
  void WriteLine(const std::string& line)
  {
    const u32 old_position = m_position, old_key = m_key;
    for (char c : line)
      WriteByte(static_cast<u8>(c));
    // The console's reader stops at a zero byte: when the encoding produced
    // one, Nintendo prods the stream with an LF before the line.
    const auto begin = m_buffer.begin() + old_position, end = m_buffer.begin() + m_position;
    if (std::find(begin, end, 0) != end)
    {
      m_key = old_key;
      m_position = old_position;
      WriteByte('\n');
      WriteLine(line);
    }
  }
  void WriteByte(u8 b)
  {
    if (m_position >= m_buffer.size())
      return;
    m_buffer[m_position++] = static_cast<u8>(b ^ m_key);
    m_key = (m_key >> 31) | (m_key << 1);
  }
  std::array<u8, 0x100> m_buffer{};
  u32 m_position = 0;
  u32 m_key = kSettingsSeed;
};

std::string SettingsValue(const std::array<u8, 0x100>& encoded, std::string_view key)
{
  std::string decoded;
  u32 k = kSettingsSeed;
  for (u8 b : encoded)
  {
    decoded.push_back(static_cast<char>(b ^ k));
    k = (k >> 31) | (k << 1);
  }
  // "KEY=" at the start of the buffer or right after a line feed, as the
  // system menu's reader looks it up; the value runs to the end of the line.
  const std::string needle = std::string(key).append("=");
  size_t value_start = std::string::npos;
  if (decoded.compare(0, needle.size(), needle) == 0)
    value_start = needle.size();
  else if (const size_t pos = decoded.find("\n" + needle); pos != std::string::npos)
    value_start = pos + 1 + needle.size();
  if (value_start == std::string::npos)
    return {};
  const size_t end = decoded.find_first_of("\r\n", value_start);
  return decoded.substr(value_start, end == std::string::npos ? std::string::npos : end - value_start);
}

// ---- NAND helpers with the semantics of ES and the system menu ----

ResultCode CreateFullPath(NandStore& nand, u32 uid, u16 gid, const std::string& path, Modes modes)
{
  // Every directory on the way to the last component (a path ending in '/'
  // creates the whole thing); existing ones are left alone.
  size_t position = 1;
  while ((position = path.find('/', position)) != std::string::npos)
  {
    const std::string prefix = path.substr(0, position);
    if (!nand.GetMetadata(uid, gid, prefix))
    {
      const ResultCode result = nand.CreateDirectory(uid, gid, prefix, 0, modes);
      if (result != ResultCode::Success && result != ResultCode::AlreadyExists)
        return result;
    }
    ++position;
  }
  return ResultCode::Success;
}

class UidSys
{
public:
  explicit UidSys(NandStore& nand) : m_nand(nand)
  {
    if (const auto fd = m_nand.OpenFile(kKernelUid, 0, kPath, Mode::Read))
    {
      std::array<u8, 12> entry;
      while (true)
      {
        const auto read = m_nand.ReadBytesFromFile(*fd, entry.data(), static_cast<u32>(entry.size()));
        if (!read || *read != entry.size())
          break;
        const u64 title = Be64(entry.data());
        const u32 uid = Be32(entry.data() + 8);
        if (!title && !uid)
          break;
        m_entries[uid] = title;
      }
      m_nand.Close(*fd);
    }
    if (m_entries.empty())
      GetOrInsert(kSystemMenuTitle);
  }

  u32 GetOrInsert(u64 title_id)
  {
    for (const auto& [uid, title] : m_entries)
      if (title == title_id)
        return uid;
    const u32 uid = m_entries.empty() ? kFirstPpcUid : m_entries.rbegin()->first + 1;
    m_entries[uid] = title_id;
    if (!m_nand.GetMetadata(kKernelUid, 0, kPath))
      m_nand.CreateFile(kKernelUid, 0, kPath, 0, kInternalFileModes);
    const auto fd = m_nand.OpenFile(kKernelUid, 0, kPath, Mode::ReadWrite);
    if (!fd)
      return 0;
    std::array<u8, 12> entry{};
    for (int i = 0; i < 8; ++i)
      entry[i] = static_cast<u8>(title_id >> (8 * (7 - i)));
    for (int i = 0; i < 4; ++i)
      entry[8 + i] = static_cast<u8>(uid >> (8 * (3 - i)));
    (void)m_nand.SeekFile(*fd, 0, StaticRecompNativeNand::SeekMode::End);
    (void)m_nand.WriteBytesToFile(*fd, entry.data(), static_cast<u32>(entry.size()));
    m_nand.Close(*fd);
    return uid;
  }

private:
  static constexpr const char* kPath = "/sys/uid.sys";
  NandStore& m_nand;
  std::map<u32, u64> m_entries;
};

bool CreateTitleDirectories(NandStore& nand, UidSys& uid_sys, u64 title_id, u16 group_id)
{
  // ES::CreateTitleDirectories: content dir with the title dir modes on the
  // way, then its own modes; data dir owned by the title's uid and group.
  const std::string title = TitlePath(title_id);
  const std::string content = title + "/content";
  if (CreateFullPath(nand, kKernelUid, 0, content + '/', kTitleDirModes) != ResultCode::Success)
    return false;
  if (nand.SetMetadata(kKernelUid, content, kKernelUid, 0, 0, kContentDirModes) != ResultCode::Success)
    return false;
  const std::string data = title + "/data";
  if (!nand.ReadDirectory(kKernelUid, 0, data))
  {
    if (nand.CreateDirectory(kKernelUid, 0, data, 0, kDataDirModes) != ResultCode::Success)
      return false;
  }
  const u32 uid = uid_sys.GetOrInsert(title_id);
  return nand.SetMetadata(0, data, uid, group_id, 0, kDataDirModes) == ResultCode::Success;
}

bool WriteWholeFile(NandStore& nand, u32 uid, u16 gid, const std::string& path, Modes modes,
                    const u8* data, u32 size, bool only_if_missing)
{
  if (nand.GetMetadata(uid, gid, path))
  {
    if (only_if_missing)
      return true;
  }
  else if (nand.CreateFile(uid, gid, path, 0, modes) != ResultCode::Success)
  {
    return false;
  }
  const auto fd = nand.OpenFile(uid, gid, path, Mode::ReadWrite);
  if (!fd)
    return false;
  const auto written = nand.WriteBytesToFile(*fd, data, size);
  nand.Close(*fd);
  return written && *written == size;
}

// The system menu's files the console's NAND has and a fresh one lacks
// (WiiConnect24 mailboxes and lists under /shared2/wc24): the files of the
// bundled Sys/Wii folder, copied into the NAND root in directory order,
// public read/write, never over an existing file (what the chassis's
// CopySysmenuFilesToFS did after every boot).
// What ES creates when the IOS starts (ESCore's directory list): the NAND's
// top-level directories with their modes and owners. Existing ones only get
// their metadata set again, as ES does.
void CreateEsDirectories(NandStore& nand)
{
  constexpr u32 kPidUnknown = 19;
  constexpr Modes kPublic{Mode::ReadWrite, Mode::ReadWrite, Mode::ReadWrite};
  struct Directory
  {
    const char* path;
    Modes modes;
    u32 uid;
    u16 gid;
  };
  static const Directory directories[] = {
      {"/sys", {Mode::ReadWrite, Mode::ReadWrite, Mode::None}, kKernelUid, 0},
      {"/ticket", {Mode::ReadWrite, Mode::ReadWrite, Mode::None}, kKernelUid, 0},
      {"/title", {Mode::ReadWrite, Mode::ReadWrite, Mode::Read}, kKernelUid, 0},
      {"/shared1", {Mode::ReadWrite, Mode::ReadWrite, Mode::None}, kKernelUid, 0},
      {"/shared2", kPublic, kKernelUid, 0},
      {"/tmp", kPublic, kKernelUid, 0},
      {"/import", {Mode::ReadWrite, Mode::ReadWrite, Mode::None}, kKernelUid, 0},
      {"/meta", kPublic, kSystemMenuUid, kSystemMenuGid},
      {"/wfs", {Mode::ReadWrite, Mode::None, Mode::None}, kPidUnknown, static_cast<u16>(kPidUnknown)},
  };
  for (const Directory& d : directories)
  {
    nand.CreateDirectory(kKernelUid, 0, d.path, 0, d.modes);
    nand.SetMetadata(0, d.path, d.uid, d.gid, 0, d.modes);
  }
}

bool CopySysmenuFiles(NandStore& nand, const std::filesystem::path& host_dir, const std::string& nand_dir,
                      u32* copied)
{
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(host_dir, error))
  {
    const std::string name = entry.path().filename().string();
    const std::string nand_path = nand_dir + '/' + name;
    if (entry.is_directory())
    {
      nand.CreateDirectory(kSystemMenuUid, kSystemMenuGid, nand_path, 0, kSysmenuFileModes);
      if (!CopySysmenuFiles(nand, entry.path(), nand_path, copied))
        return false;
      continue;
    }
    if (nand.GetMetadata(kSystemMenuUid, static_cast<u16>(kSystemMenuUid), nand_path))
      continue;
    std::ifstream file(entry.path(), std::ios::binary);
    std::vector<u8> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!WriteWholeFile(nand, kSystemMenuUid, kSystemMenuGid, nand_path, kSysmenuFileModes, data.data(),
                        static_cast<u32>(data.size()), true))
      return false;
    ++*copied;
  }
  return !error;
}

// What the IOS network services leave on the NAND when they start and find
// no configuration: the WiiConnect24 message config (KD, pid 12) and the
// network connection config (NCD, pid 13), in that order, each only when it
// cannot be read. The same bytes and owners as the console's IOS defaults.
constexpr u32 kPidKd = 12;
constexpr u32 kPidNcd = 13;

bool ReadableWhole(NandStore& nand, u32 uid, const std::string& path, u32 size)
{
  const auto fd = nand.OpenFile(uid, static_cast<u16>(uid), path, Mode::Read);
  if (!fd)
    return false;
  std::vector<u8> data(size);
  const auto read = nand.ReadBytesFromFile(*fd, data.data(), size);
  nand.Close(*fd);
  return read && *read == size;
}

void WriteNetworkDefaults(NandStore& nand)
{
  // nwc24msg.cfg: 'WcCf', version 8, "@wii.com", the five service URLs,
  // creation stage 0, booting off, and the sum of the first 255 words.
  {
    const std::string path = "/shared2/wc24/nwc24msg.cfg";
    if (!ReadableWhole(nand, kPidKd, path, 0x400))
    {
      std::array<u8, 0x400> config{};
      const auto put32 = [&](u32 offset, u32 value) {
        config[offset] = u8(value >> 24);
        config[offset + 1] = u8(value >> 16);
        config[offset + 2] = u8(value >> 8);
        config[offset + 3] = u8(value);
      };
      put32(0x00, 0x57634366);
      put32(0x04, 8);
      std::memcpy(&config[0x18], "@wii.com", 8);
      constexpr const char* urls[5] = {
          "https://amw.wc24.wii.com/cgi-bin/account.cgi", "http://rcw.wc24.wii.com/cgi-bin/check.cgi",
          "http://mtw.wc24.wii.com/cgi-bin/receive.cgi",  "http://mtw.wc24.wii.com/cgi-bin/delete.cgi",
          "http://mtw.wc24.wii.com/cgi-bin/send.cgi"};
      for (int i = 0; i < 5; ++i)
        std::memcpy(&config[0x9C + 0x80 * i], urls[i], std::strlen(urls[i]));
      u32 sum = 0;
      for (u32 i = 0; i < 0xFF; ++i)
        sum += Be32(config.data() + 4 * i);
      put32(0x3FC, sum);
      nand.Delete(kPidKd, static_cast<u16>(kPidKd), path);
      CreateFullPath(nand, kPidKd, static_cast<u16>(kPidKd), path, kSysmenuFileModes);
      WriteWholeFile(nand, kPidKd, static_cast<u16>(kPidKd), path, kSysmenuFileModes, config.data(),
                     static_cast<u32>(config.size()), false);
    }
  }
  // config.dat: wired connection (type 2) and the first connection's flags
  // (wired, DNS and IP by DHCP, test passed, selected: 0xA7); 7004 bytes.
  {
    const std::string path = "/shared2/sys/net/02/config.dat";
    constexpr u32 kSize = 7004;
    if (!ReadableWhole(nand, kPidNcd, path, kSize))
    {
      std::vector<u8> config(kSize);
      config[4] = 2;
      config[8] = 0xA7;
      nand.Delete(kPidNcd, static_cast<u16>(kPidNcd), path);
      CreateFullPath(nand, kPidNcd, static_cast<u16>(kPidNcd), path, kSysmenuFileModes);
      WriteWholeFile(nand, kPidNcd, static_cast<u16>(kPidNcd), path, kSysmenuFileModes, config.data(), kSize,
                     false);
    }
  }
}

struct RegionSetting
{
  const char* area;
  const char* video;
  const char* game;
  const char* code;
};

Region RegionFromCountry(char country)
{
  switch (country)
  {
  case 'J':
    return Region::NtscJ;
  case 'K':
  case 'Q':
  case 'T':
    return Region::NtscK;
  case 'E':
  case 'N':
  case 'W':
    return Region::NtscU;
  default:
    return Region::Pal;
  }
}

const RegionSetting& SettingFor(Region region)
{
  static const RegionSetting jpn{"JPN", "NTSC", "JP", "LJH"}, usa{"USA", "NTSC", "US", "LU"},
      eur{"EUR", "PAL", "EU", "LEH"}, kor{"KOR", "NTSC", "KR", "LKH"};
  switch (region)
  {
  case Region::NtscJ:
    return jpn;
  case Region::Pal:
    return eur;
  case Region::NtscK:
    return kor;
  default:
    return usa;
  }
}

// setting.txt as the system menu leaves it: previous serial number and model
// kept, region values from the disc (or the file when they agree), rewritten.
bool WriteSettings(NandStore& nand, Region region, std::array<u8, 0x100>& encoded)
{
  const std::string path = TitlePath(kSystemMenuTitle) + "/data/setting.txt";
  RegionSetting setting = SettingFor(region);
  std::string serno;
  std::string model = std::string("RVL-001(") + setting.area + ")";
  if (const auto fd = nand.OpenFile(kSystemMenuUid, kSystemMenuGid, path, Mode::Read))
  {
    std::array<u8, 0x100> previous{};
    const auto read = nand.ReadBytesFromFile(*fd, previous.data(), static_cast<u32>(previous.size()));
    nand.Close(*fd);
    if (read && *read == previous.size())
    {
      serno = SettingsValue(previous, "SERNO");
      model = SettingsValue(previous, "MODEL");
      const std::string code = SettingsValue(previous, "CODE");
      const bool region_matches = (s_chassis.override_region_settings && s_chassis.override_region_settings()) ||
                                  (code.size() >= 2 && RegionFromCountry(code[1]) == region);
      if (region_matches)
      {
        static std::string area, video, game, code_value;
        area = SettingsValue(previous, "AREA");
        video = SettingsValue(previous, "VIDEO");
        game = SettingsValue(previous, "GAME");
        code_value = code;
        setting = RegionSetting{area.c_str(), video.c_str(), game.c_str(), code_value.c_str()};
      }
      else if (const size_t paren = model.find('('); paren != std::string::npos)
      {
        model = model.substr(0, paren) + '(' + setting.area + ')';
      }
    }
  }
  nand.Delete(kSystemMenuUid, kSystemMenuGid, path);
  if (serno.empty() || serno == "000000000")
  {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%09lld",
                  static_cast<long long>(std::time(nullptr) % 1000000000LL));
    serno = buffer;
  }
  SettingsWriter writer;
  writer.Add("AREA", setting.area);
  writer.Add("MODEL", model);
  writer.Add("DVD", "0");
  writer.Add("MPCH", "0x7FFE");
  writer.Add("CODE", setting.code);
  writer.Add("SERNO", serno);
  writer.Add("VIDEO", setting.video);
  writer.Add("GAME", setting.game);
  encoded = writer.Bytes();
  return WriteWholeFile(nand, kSystemMenuUid, kSystemMenuGid, path, kSysmenuFileModes,
                        encoded.data(), static_cast<u32>(encoded.size()), false);
}

// state.dat: flags 0xc1, disc title, disc inserted, with the system menu's
// checksum (sum of the seven words after it, in the console's own layout as
// Dolphin wrote it: host order).
void UpdateStateFlags(NandStore& nand)
{
  const std::string path = TitlePath(kSystemMenuTitle) + "/data/state.dat";
  std::array<u8, 32> state{};
  if (const auto fd = nand.OpenFile(kSystemMenuUid, kSystemMenuGid, path, Mode::Read))
  {
    const auto status = nand.GetFileStatus(*fd);
    if (status && status->size == state.size())
      (void)nand.ReadBytesFromFile(*fd, state.data(), static_cast<u32>(state.size()));
    nand.Close(*fd);
  }
  state[4] = 0xc1;  // flags
  state[5] = 0xff;  // type
  state[6] = 0x01;  // discstate
  u32 checksum = 0;
  for (size_t i = 4; i < state.size(); i += 4)
  {
    u32 word;
    std::memcpy(&word, state.data() + i, 4);
    checksum += word;
  }
  std::memcpy(state.data(), &checksum, 4);
  WriteWholeFile(nand, kSystemMenuUid, kSystemMenuGid, path, kSysmenuFileModes, state.data(),
                 static_cast<u32>(state.size()), false);
}
}  // namespace

bool Enabled()
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_BOOT");
  return !(setting && (std::string(setting) == "off" || std::string(setting) == "0"));
}

bool PrepareNand(const std::string& nand_root)
{
  NandStore nand(nand_root);
  CreateEsDirectories(nand);
  return StaticRecompSysConf::WriteBootSettings(nand);
}

void SetChassisHooks(ChassisHooks hooks)
{
  s_chassis = std::move(hooks);
}

bool Boot(const BootPaths& paths, BootCpuState* cpu)
{
  const std::string sys = paths.game_root + "/sys/";
  std::vector<u8> boot_bin, bi2, fst, dol, tmd;
  if (!ReadFile(sys + "boot.bin", boot_bin) || !ReadFile(sys + "bi2.bin", bi2) ||
      !ReadFile(sys + "fst.bin", fst) || !ReadFile(sys + "main.dol", dol) ||
      !ReadFile(paths.game_root + "/tmd.bin", tmd))
  {
    std::fprintf(stderr, "[native_boot] missing sys/boot.bin, bi2.bin, fst.bin, main.dol or tmd.bin\n");
    return false;
  }
  if (boot_bin.size() < 0x20 || bi2.size() != 0x2000 || fst.size() < 12 || dol.size() < 0x100 ||
      tmd.size() < 0x1e4)
  {
    std::fprintf(stderr, "[native_boot] disc system files have unexpected sizes\n");
    return false;
  }
  const auto memory = StaticRecompGuestMemory::GuestMemory::Own();
  const u32 ram_size = memory.GetRamSizeReal();

  // ---- the title ----
  const std::string game_id(reinterpret_cast<const char*>(boot_bin.data()), 6);
  const u8 disc_revision = boot_bin[7];
  const u64 title_id = Be64(tmd.data() + 0x18c);
  const u16 group_id = Be16(tmd.data() + 0x198);
  const u16 ios_number = static_cast<u16>(Be64(tmd.data() + 0x184) & 0xffff);
  const Region region = RegionFromCountry(game_id[3]);
  if (s_chassis.title)
    s_chassis.title(game_id, title_id, disc_revision, region);

  // ---- the NAND, as the system menu, ES and the disc launch leave it ----
  NandStore nand(paths.nand_root);
  // The chassis's IOS used to write these when it started (before the boot);
  // without it the native boot does (phase S step 27).
  if (!(s_chassis.has_ios && s_chassis.has_ios()))
  {
    // The IOS start: /dev/fs clears /tmp (FSCore), ES creates its directories,
    // KD and NCD write their defaults.
    if (nand.Delete(kKernelUid, 0, "/tmp") == ResultCode::Success)
      nand.CreateDirectory(kKernelUid, 0, "/tmp", 0, {Mode::ReadWrite, Mode::ReadWrite, Mode::ReadWrite});
    CreateEsDirectories(nand);
    WriteNetworkDefaults(nand);
  }
  UidSys uid_sys(nand);
  CreateTitleDirectories(nand, uid_sys, kSystemMenuTitle, kSystemMenuGid);
  // Same order as the console's launch (and the chassis): play record, state
  // flags, then setting.txt, so the FST lists them the same way.
  {
    const std::vector<u8> empty_record(0x80);
    WriteWholeFile(nand, kSystemMenuUid, kSystemMenuGid, TitlePath(kSystemMenuTitle) + "/data/play_rec.dat",
                   kSysmenuFileModes, empty_record.data(), 0x80, true);
  }
  UpdateStateFlags(nand);
  std::array<u8, 0x100> settings{};
  if (!WriteSettings(nand, region, settings))
  {
    std::fprintf(stderr, "[native_boot] could not write setting.txt\n");
    return false;
  }
  {
    // ES_DIVerify: the disc's TMD stored under the title, the title's uid and
    // group, and its data directory.
    const std::string content = TitlePath(title_id) + "/content";
    const std::string tmd_path = content + "/title.tmd";
    if (!nand.GetMetadata(kKernelUid, 0, tmd_path))
    {
      CreateFullPath(nand, kKernelUid, 0, tmd_path, kTitleDirModes);
      nand.SetMetadata(kKernelUid, content, kKernelUid, 0, 0, kInternalFileModes);
      WriteWholeFile(nand, kKernelUid, 0, tmd_path, kInternalFileModes, tmd.data(),
                     static_cast<u32>(tmd.size()), true);
    }
    const u32 uid = uid_sys.GetOrInsert(title_id);
    const std::string data = TitlePath(title_id) + "/data";
    nand.CreateDirectory(kKernelUid, 0, data, 0, kDataDirModes);
    nand.SetMetadata(0, data, uid, group_id, 0, kDataDirModes);
    StaticRecompNativeSdk::SetTitleIdentity(uid, group_id);
  }
  if (!paths.sysmenu_root.empty())
  {
    u32 copied = 0;
    const std::filesystem::path sysmenu = paths.sysmenu_root;
    if (!CopySysmenuFiles(nand, sysmenu, "", &copied))
      std::fprintf(stderr, "[native_boot] could not copy the system menu files from %s\n",
                   sysmenu.string().c_str());
    else if (copied)
      std::fprintf(stderr, "[native_boot] system menu files copied to the NAND: %u\n", copied);
  }

  // Chassis state only: its IOS kernel object must be the title's IOS so that
  // the savestates of the chassis (whose IOS section lists that IOS's device
  // set) keep loading, and its file system object must load the FST after the
  // NAND work above (the chassis's Wii file-system initialisation still runs
  // after the boot and rewrites fst.bin from that copy). The game never talks
  // to it. Phase S removes it.
  if (s_chassis.boot_ios)
    s_chassis.boot_ios(ios_number);

  // ---- low memory: disc header and the OS globals ----
  memory.CopyToEmu(0, boot_bin.data(), 0x20);
  memory.Write_U32(0x0D15EA5E, 0x20);
  memory.Write_U32(0x00000001, 0x24);
  memory.Write_U32(ram_size, 0x28);
  memory.Write_U32(0x23, 0x2c);  // board model: retail Wii (RVL_Retail3)
  memory.Write_U32(0x00000000, 0x30);
  // The apploader places bi2 and the FST at the top of MEM1 and points the
  // arena high and the FST globals at them.
  const u32 fst_size = static_cast<u32>(fst.size());
  const u32 fst_address = (kMem1End - fst_size) & ~0x1Fu;
  const u32 bi2_address = fst_address - 0x2000;
  memory.Write_U32(fst_address, 0x34);
  memory.Write_U32(fst_address, 0x38);
  memory.Write_U32(fst_size, 0x3c);
  memory.Write_U32(region == Region::Pal ? 1 : 0, 0xcc);
  memory.Write_U32(0x8008f7b8, 0xe4);  // thread init
  memory.Write_U32(kMem1End, 0xec);
  memory.Write_U32(ram_size, 0xf0);
  memory.Write_U32(bi2_address, 0xf4);
  memory.Write_U32(0x0e7be2c0, 0xf8);  // bus speed
  memory.Write_U32(0x2b73a840, 0xfc);  // CPU speed
  memory.Write_U32(0x00000000, 0x30c0);  // EXI
  memory.Write_U32(0x00000000, 0x30c4);
  memory.Write_U32(0x00000000, 0x30dc);  // time
  memory.Write_U32(0xffffffff, 0x30d8);
  memory.Write_U32(0x003afce0, 0x30d4);  // left by this game's apploader (same in every boot)
  memory.Write_U16(0x0000, 0x30e0);  // PADInit
  memory.Write_U16(0x8201, 0x30e6);  // dev console / debug capable
  memory.Write_U32(0x00000000, 0x30f0);  // apploader
  memory.Write_U8(0x80, 0x315c);         // boot flag (OSInit)
  memory.Write_U8(0x80, 0x315d);         // apploader flag
  memory.Write_U16(0x0113, 0x315e);      // devkit boot program version
  memory.Write_U32(0x80000000, 0x3184);  // game id address
  memory.CopyToEmu(0x3180, boot_bin.data(), 4);
  memory.Write_U32(0x00150202, 0x3188);  // written by the apploader
  memory.Write_U32(0, 0x3194);
  memory.Write_U32(0xF800000 >> 2, 0x3198);  // data partition offset
  memory.CopyToEmu(0x3800, settings.data(), settings.size());

  // ---- IOS memory values: the system menu IOS at power-on, then the title's
  // IOS reloaded (version and MEM2 layout), as IOS does ----
  const auto& values = GetIosMemoryValues();
  const auto sysmenu_ios = std::find_if(values.begin(), values.end(),
                                        [](const IosMemoryValues& v) { return v.ios_number == 80; });
  const auto title_ios = std::find_if(values.begin(), values.end(), [ios_number](const IosMemoryValues& v) {
    return v.ios_number == ios_number;
  });
  if (sysmenu_ios == values.end() || title_ios == values.end())
  {
    std::fprintf(stderr, "[native_boot] no memory values for IOS%u\n", ios_number);
    return false;
  }
  constexpr u32 kPlaceholder = 0xDEADBEEF;
  memory.Write_U32(sysmenu_ios->mem1_physical_size, 0x3100);
  memory.Write_U32(sysmenu_ios->mem1_simulated_size, 0x3104);
  memory.Write_U32(sysmenu_ios->mem1_end, 0x3108);
  memory.Write_U32(sysmenu_ios->mem1_arena_begin, 0x310c);
  memory.Write_U32(fst_address, 0x3110);  // arena end, moved by the apploader
  memory.Write_U32(kPlaceholder, 0x3114);
  memory.Write_U32(title_ios->mem2_physical_size, 0x3118);
  memory.Write_U32(title_ios->mem2_simulated_size, 0x311c);
  memory.Write_U32(title_ios->mem2_end, 0x3120);
  memory.Write_U32(title_ios->mem2_arena_begin, 0x3124);
  memory.Write_U32(title_ios->mem2_arena_end, 0x3128);
  memory.Write_U32(kPlaceholder, 0x312c);
  memory.Write_U32(title_ios->ipc_buffer_begin, 0x3130);
  memory.Write_U32(title_ios->ipc_buffer_end, 0x3134);
  memory.Write_U32(sysmenu_ios->hollywood_revision, 0x3138);
  memory.Write_U32(kPlaceholder, 0x313c);
  memory.Write_U32(title_ios->ios_version, 0x3140);
  memory.Write_U32(sysmenu_ios->ios_date, 0x3144);
  memory.Write_U32(title_ios->ios_reserved_begin, 0x3148);
  memory.Write_U32(title_ios->ios_reserved_end, 0x314c);
  memory.Write_U32(kPlaceholder, 0x3150);
  memory.Write_U32(kPlaceholder, 0x3154);
  memory.Write_U32(sysmenu_ios->ram_vendor, 0x3158);
  memory.Write_U32(sysmenu_ios->sysmenu_sync, 0x3160);

  // ---- exception vectors the BS2 leaves (the OS installs its own) ----
  constexpr u32 kRfi = 0x4c000064;
  memory.Write_U32(kRfi, 0x300);
  memory.Write_U32(kRfi, 0x800);
  memory.Write_U32(kRfi, 0xc00);

  // ---- bi2, FST and the DOL ----
  memory.CopyToEmu(bi2_address, bi2.data(), bi2.size());
  memory.CopyToEmu(fst_address, fst.data(), fst.size());
  for (u32 i = 0; i < 18; ++i)
  {
    const u32 offset = Be32(dol.data() + 4 * i);
    const u32 address = Be32(dol.data() + 0x48 + 4 * i);
    const u32 size = Be32(dol.data() + 0x90 + 4 * i);
    if (!size)
      continue;
    if (u64(offset) + size > dol.size() || (address & 0x7fffffff) + u64(size) > kMem1Size)
    {
      std::fprintf(stderr, "[native_boot] DOL section %u out of range\n", i);
      return false;
    }
    memory.CopyToEmu(address, dol.data() + offset, size);
  }
  const u32 entry = Be32(dol.data() + 0xe0);

  // ---- the CPU as the BS2 and the apploader leave it ----
  // MSR: RI, DR, IR, FP. HID0: BHT, BTIC, DCFA, SPD, DCFI, DCE, ICE, NHR, DPM.
  // HID2: PSE, WPE, LSQE. HID4: L2CFI, LPE, ST0, SBE and bit 31.
  BootCpuState state;
  state.msr = (1u << 1) | (1u << 4) | (1u << 5) | (1u << 13);
  state.hid0_set = (1u << 2) | (1u << 5) | (1u << 6) | (1u << 9) | (1u << 10) | (1u << 14) |
                   (1u << 15) | (1u << 16) | (1u << 20);
  state.hid2_set = (1u << 29) | (1u << 30) | (1u << 31);
  state.hid4_set = (1u << 20) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 31);
  state.sprs = {
      {528, 0x80001fff}, {529, 0x00000002},  // IBAT0
      {536, 0x80001fff}, {537, 0x00000002},  // DBAT0
      {538, 0xc0001fff}, {539, 0x0000002a},  // DBAT1
      {560, 0x90001fff}, {561, 0x10000002},  // IBAT4
      {568, 0x90001fff}, {569, 0x10000002},  // DBAT4
      {570, 0xd0001fff}, {571, 0x1000002a},  // DBAT5
  };
  state.gpr1 = 0x816ffff0;  // stack
  state.gpr3 = entry;       // the apploader's close returns it
  state.pc = entry;
  *cpu = state;

  std::fprintf(stderr,
               "[native_boot] %s title %016llx IOS%u: DOL entry %08x, FST %08x (%u bytes), bi2 %08x, "
               "uid %08x gid %04x\n",
               game_id.c_str(), static_cast<unsigned long long>(title_id), ios_number, entry,
               fst_address, fst_size, bi2_address, uid_sys.GetOrInsert(title_id), group_id);
  return true;
}
}  // namespace StaticRecompNativeBoot
