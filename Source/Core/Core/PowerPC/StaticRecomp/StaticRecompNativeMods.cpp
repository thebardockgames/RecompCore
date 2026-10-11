// RecompCore: StaticRecomp CPU core - user mods served by native disc reads.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// tools/bt3_mods_build.py turns <mods> into <mods>/.cache/manifest.txt:
//   afs <archive> <entry> <file>   replace an AFS entry (songs 00..19, the
//                                  menu packs with a BGM list and its name
//                                  sheets, ...)
//   song <id> <file>               new song with BGM Test id <id> (20..23, 26..)
//
// AFS entries: an archive with replaced entries is served with a virtual
// layout. The game's AFS reader places entry i + 1 at the end of entry i
// rounded to 0x800 (true for both archives), so the virtual archive keeps that
// layout with the new sizes, and its index is rebuilt to match. The archive
// moves past the end of the disc (its FST offset and length change in game
// memory) because it may grow; every read maps back to the extracted archive
// or to a mod file.
//
// New songs: all play through one dynamic slot (zs3_29_US.adx, an unused 30
// KB placeholder): Bgm_Play of a new id selects the song the slot serves and
// plays the slot. BgmList_ApplyUnlocks keeps new ids unlocked (the save is
// not written). Ids are the game's reserved slots 20..23 ("********"), then
// 26.. (24 "Random" and 25 "???" are kept). The BGM Test draws a name from
// sheet id / 8, row id % 8; the mods pack writes the names there, and
// ClipSetUv draws ids >= 32 (no sheet of their own) from row id - 24 of
// sheet 3, which the pack widens. The stage selects' music selectors use the
// same list (their packs are rebuilt too) and Bgm_Play, so new songs can be
// picked for a battle.
//
// The disc files are never modified. Mods apply from boot (a state saved
// without them keeps the index it already read).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeMods.h"

namespace StaticRecompNativeSdk
{
namespace
{
std::string s_mods_root;

constexpr u32 kSda = 0x806370A0;
constexpr u32 kFstPointer = 0x80000038;
const char* const kMusicArchive = "wzs3us2.afs";
constexpr u32 kDynamicSlot = 65003;     // zs3_29_US.adx
constexpr u32 kSongFileBase = 0x10CA6;  // Bgm_Play id of zs3_00 (BGM Test: id + base)
constexpr u32 kDynamicSlotId = kSongFileBase + 29;
// New song ids: the reserved slots 20..23, then 26..55 (24 is "Random", 25
// the locked "???"); names of ids >= 32 come from row id - 24 of sheet 3.
bool IsNewSongId(u32 id)
{
  return (id >= 20 && id < 24) || (id >= 26 && id < 56);
}
// Game functions hooked for mods (tools/native-sdk/RDSE70-native-hooks.txt).
constexpr u32 kClipSetUv = 0x80023228;
constexpr u32 kBgmPlay = 0x80028000;
constexpr u32 kBgmListApplyUnlocks = 0x8002A8F4;
// Draw functions that show song names (uv sheet id / 8, row id % 8): the
// BGM Test and the music selectors of the stage selects.
struct CodeRange
{
  u32 begin, end;
};
constexpr CodeRange kSongNameDraws[] = {
    {0x800ED280, 0x800EF308},  // Option_Draw (BGM Test)
    {0x8006F18C, 0x8006FDF8},  // character/stage select ("mc_bgm_now")
    {0x800B1C68, 0x800B2E9C},  // stage select ("mc_bgm_now")
    {0x801E4BA8, 0x801E5790},  // stage select ("mc_bgm_now")
};
constexpr u32 kProgress = kSda - 0x6D30;  // gProgress pointer (mode at +0x18)
constexpr u32 kSaveData = kSda - 0x6C40;  // gSaveData pointer (bgmBits at +0xC30)
constexpr u32 kHighNameSheet = 3;

u32 LoadBe32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}
void StoreBe32(u8* p, u32 v)
{
  p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v);
}
}  // namespace

void SetModsRoot(const std::string& path)
{
  s_mods_root = path;
}

bool NativeSdk::ModsBind(const std::function<bool(u32)>& enable)
{
  if (s_mods_root.empty())
    return false;
  // Game hooks exist only in modules built with them; songs still replace
  // the game's own without them.
  const bool hooks = enable(kBgmPlay) && enable(kBgmListApplyUnlocks) && enable(kClipSetUv);
  std::fprintf(stderr, "[mods] %s; BGM Test hooks %s\n", s_mods_root.c_str(),
               hooks ? "on" : "missing from the module (new songs not listed)");
  return hooks;
}

NativeSdk::ModsState::Archive::~Archive()
{
  if (file)
    std::fclose(file);
}

void NativeSdk::ModsInit(const CPUState& guest)
{
  if (m_mods_checked)
    return;
  m_mods_checked = true;
  if (s_mods_root.empty() || m_verify)
    return;
  namespace fs = std::filesystem;
  const fs::path manifest = fs::path(s_mods_root) / ".cache" / "manifest.txt";
  std::ifstream in(manifest);
  if (!in)
  {
    std::fprintf(stderr, "[mods] no %s (run tools/bt3_mods_build.py)\n", manifest.string().c_str());
    return;
  }
  auto state = std::make_unique<ModsState>();
  state->hooks = m_mods_hooks;
  // Archive pointers stay valid: at most 4 archives, reserved up front.
  state->archives.reserve(4);
  // The original index of an archive, from the extracted file.
  const auto archive_of = [&](const std::string& name) -> ModsState::Archive* {
    for (auto& a : state->archives)
      if (a.name == name)
        return &a;
    const auto disc = std::find_if(m_disc_files.begin(), m_disc_files.end(),
                                   [&](const DiscFile& f) { return f.path == name; });
    if (disc == m_disc_files.end() || state->archives.size() == 4)
      return nullptr;
    FILE* f = std::fopen((GameRoot() + "/files/" + name).c_str(), "rb");
    u8 head[8];
    if (!f || std::fread(head, 1, 8, f) != 8 || std::memcmp(head, "AFS", 3) != 0)
    {
      if (f)
        std::fclose(f);
      return nullptr;
    }
    ModsState::Archive& a = state->archives.emplace_back();
    a.name = name;
    a.disc_offset = disc->offset;
    a.disc_size = disc->size;
    a.file = f;
    a.count = u32(head[4]) | (u32(head[5]) << 8) | (u32(head[6]) << 16) | (u32(head[7]) << 24);
    std::vector<u8> table(a.count <= 200000 ? size_t(a.count) * 8 + 8 : 0);
    if (a.count == 0 || table.empty() || std::fread(table.data(), 1, table.size(), f) != table.size())
    {
      state->archives.pop_back();
      return nullptr;
    }
    const auto le = [&](size_t at) {
      return u32(table[at]) | (u32(table[at + 1]) << 8) | (u32(table[at + 2]) << 16) |
             (u32(table[at + 3]) << 24);
    };
    for (u32 i = 0; i < a.count; ++i)
    {
      a.orig_offset.push_back(le(size_t(i) * 8));
      a.orig_size.push_back(le(size_t(i) * 8 + 4));
    }
    a.dir_offset = le(size_t(a.count) * 8);
    a.dir_size = le(size_t(a.count) * 8 + 4);
    a.data_start = a.orig_offset[0];
    a.source.assign(a.count, -1);
    a.virt_size = a.orig_size;
    return &a;
  };
  std::error_code ec;
  std::string line;
  while (std::getline(in, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::istringstream words(line);
    std::string kind;
    words >> kind;
    if (kind == "afs")
    {
      std::string name, path;
      u32 index = 0;
      words >> name >> index;
      std::getline(words >> std::ws, path);
      ModsState::Archive* archive = archive_of(name);
      const u64 size = fs::file_size(path, ec);
      if (!archive || index >= archive->count || ec || size == 0 || size > 0x40000000)
      {
        std::fprintf(stderr, "[mods] cannot use %s entry %u: %s\n", name.c_str(), index, path.c_str());
        continue;
      }
      archive->source[index] = static_cast<int>(archive->paths.size());
      archive->paths.push_back(path);
      archive->virt_size[index] = static_cast<u32>(size);
    }
    else if (kind == "song")
    {
      ModsState::Song song;
      words >> song.id;
      std::getline(words >> std::ws, song.path);
      const u64 size = fs::file_size(song.path, ec);
      if (ec || size == 0 || size > 0x40000000 || !IsNewSongId(song.id) ||
          std::any_of(state->songs.begin(), state->songs.end(),
                      [&](const ModsState::Song& s) { return s.id == song.id; }))
      {
        std::fprintf(stderr, "[mods] cannot use song %u: %s\n", song.id, song.path.c_str());
        continue;
      }
      song.size = static_cast<u32>(size);
      state->songs.push_back(song);
    }
  }
  // New songs (with the BGM Test hooks) share the dynamic slot, sized for
  // the largest of them.
  bool slot = false;
  if (!state->songs.empty() && state->hooks)
  {
    u32 largest = 0;
    for (const auto& s : state->songs)
      largest = std::max(largest, s.size);
    if (ModsState::Archive* music = archive_of(kMusicArchive); music && kDynamicSlot < music->count)
    {
      music->source[kDynamicSlot] = -2;
      music->virt_size[kDynamicSlot] = largest;
      slot = true;
    }
  }
  if (!slot)
    state->songs.clear();

  // Virtual layouts (the AFS reader's cumulative 0x800 layout) and indexes.
  for (auto& a : state->archives)
  {
    a.virt_offset.resize(a.count);
    u64 at = a.data_start;
    for (u32 i = 0; i < a.count; ++i)
    {
      a.virt_offset[i] = static_cast<u32>(at);
      at = (at + a.virt_size[i] + 0x7FF) & ~u64(0x7FF);
    }
    a.virt_dir_offset = static_cast<u32>(at);
    const u64 end = at + a.dir_size;
    if (end > 0xFFFF0000ull)
    {
      std::fprintf(stderr, "[mods] %s grows past 4 GB: mods off\n", a.name.c_str());
      return;
    }
    a.virt_end = static_cast<u32>(end);
    a.index.assign(size_t(a.count) * 8 + 16, 0);
    std::memcpy(a.index.data(), "AFS", 4);
    const auto put = [&](size_t pos, u32 v) {
      for (int k = 0; k < 4; ++k)
        a.index[pos + k] = u8(v >> (8 * k));
    };
    put(4, a.count);
    for (u32 i = 0; i < a.count; ++i)
    {
      put(8 + size_t(i) * 8, a.virt_offset[i]);
      put(12 + size_t(i) * 8, a.virt_size[i]);
    }
    put(8 + size_t(a.count) * 8, a.virt_dir_offset);
    put(12 + size_t(a.count) * 8, a.dir_size);
  }
  // Archives move past the end of the disc (they may grow): FST offset and
  // length change in game memory; reads at the old place are still served.
  u64 disc_end = 0;
  for (const DiscFile& f : m_disc_files)
    disc_end = std::max<u64>(disc_end, f.offset + f.size);
  u64 base = (disc_end + 0xFFFFF) & ~u64(0xFFFFF);
  for (auto& a : state->archives)
  {
    a.virt_base = base;
    base = (base + a.virt_end + 0xFFFFF) & ~u64(0xFFFFF);
  }
  u32 fst = 0;
  if (Read32(guest, kFstPointer, fst) && fst)
  {
    const u8* root = Pointer(guest, fst, 12);
    const u32 count = root ? LoadBe32(root + 8) : 0;
    for (u32 i = 1; i < count && i < 65536; ++i)
    {
      u8* entry = Pointer(guest, fst + i * 12, 12);
      if (!entry || (entry[0] & 1))
        continue;
      for (const auto& archive : state->archives)
        if (u64(LoadBe32(entry + 4)) * 4 == archive.disc_offset)
        {
          StoreBe32(entry + 4, static_cast<u32>(archive.virt_base / 4));
          StoreBe32(entry + 8, archive.virt_end);
        }
    }
  }
  size_t replaced = 0;
  for (const auto& archive : state->archives)
    replaced += archive.paths.size();
  std::fprintf(stderr, "[mods] %zu AFS entries replaced, %zu new songs\n", replaced,
               state->songs.size());
  m_mods = std::move(state);
}

bool NativeSdk::ModsRead(u64 offset, u32 length, std::vector<u8>& out)
{
  if (!m_mods)
    return false;
  ModsState& m = *m_mods;
  for (auto& a : m.archives)
  {
    u64 first;
    if (offset >= a.virt_base && offset < a.virt_base + a.virt_end)
      first = offset - a.virt_base;
    else if (offset >= a.disc_offset && offset < a.disc_offset + a.disc_size)
      first = offset - a.disc_offset;  // opened before the move
    else
      continue;
    out.assign(length, 0);
    const auto read_file = [](FILE* f, u64 at, u8* dst, u64 n) {
      if (f && n && _fseeki64(f, static_cast<long long>(at), SEEK_SET) == 0)
        std::fread(dst, 1, static_cast<size_t>(n), f);
    };
    const u64 end = first + length;
    u64 p = first;
    while (p < end)
    {
      u8* dst = out.data() + (p - first);
      if (p < a.index.size())
      {
        const u64 n = std::min<u64>(end, a.index.size()) - p;
        std::memcpy(dst, a.index.data() + p, static_cast<size_t>(n));
        p += n;
      }
      else if (p < a.data_start)
      {
        const u64 n = std::min<u64>(end, a.data_start) - p;
        read_file(a.file, p, dst, n);
        p += n;
      }
      else if (p >= a.virt_dir_offset)
      {
        if (p < a.virt_end)
          read_file(a.file, a.dir_offset + (p - a.virt_dir_offset), dst, std::min<u64>(end, a.virt_end) - p);
        p = end;
      }
      else
      {
        // The entry containing p (or the padding after it).
        const auto it = std::upper_bound(a.virt_offset.begin(), a.virt_offset.end(), static_cast<u32>(p));
        const u32 i = static_cast<u32>(it - a.virt_offset.begin()) - 1;
        const u64 start = a.virt_offset[i];
        const u64 next = i + 1 < a.count ? a.virt_offset[i + 1] : a.virt_dir_offset;
        const u64 chunk_end = std::min<u64>(end, next);
        const u64 data_end = std::min<u64>(chunk_end, start + a.virt_size[i]);
        if (p < data_end)
        {
          const u64 n = data_end - p;
          const int source = a.source[i];
          const std::string* path = nullptr;
          if (source >= 0)
            path = &a.paths[source];
          else if (source == -2 && m.dynamic_song >= 0)
            path = &m.songs[m.dynamic_song].path;
          if (path)
          {
            FILE* f = std::fopen(path->c_str(), "rb");
            read_file(f, p - start, dst, n);
            if (f)
              std::fclose(f);
            ++m.mod_reads;
            if (m.logged_reads < 12)
            {
              ++m.logged_reads;
              std::fprintf(stderr, "[mods] read %s entry %u +0x%llx len 0x%llx\n", a.name.c_str(), i,
                           static_cast<unsigned long long>(p - start), static_cast<unsigned long long>(n));
            }
          }
          else
          {
            // Original data (the dynamic slot before any new song: its own).
            const u64 rel = p - start;
            const u64 avail = a.orig_size[i] > rel ? a.orig_size[i] - rel : 0;
            read_file(a.file, a.orig_offset[i] + rel, dst, std::min(n, avail));
          }
        }
        p = chunk_end;
      }
    }
    return true;
  }
  return false;
}

void NativeSdk::ModsPatchRead(u64, std::vector<u8>&)
{
  // Archives with mods are served whole by ModsRead, index included.
}

bool NativeSdk::ModsRun(CPUState& guest)
{
  if (!m_mods || !m_mods->hooks)
    return false;
  ModsState& m = *m_mods;
  const auto new_song = [&](u32 id) -> int {
    for (size_t i = 0; i < m.songs.size(); ++i)
      if (m.songs[i].id == id)
        return static_cast<int>(i);
    return -1;
  };
  switch (guest.pc)
  {
  case kBgmPlay:
  {
    // Bgm_Play(id): a new song plays through the dynamic slot.
    if (const int song = new_song(guest.gpr[3] - kSongFileBase); song >= 0)
    {
      m.dynamic_song = song;
      guest.gpr[3] = kDynamicSlotId;
      ++m.dynamic_plays;
    }
    return false;  // the original plays (the slot, for a new song)
  }
  case kBgmListApplyUnlocks:
  {
    // BgmList_ApplyUnlocks(count*, ids*): the original rules (bit id of the
    // save's BGM mask, "Random" hidden in mode 0x3E; locked ids become the
    // "???" id 0x19), with new songs always unlocked.
    ++m.unlock_calls;
    u32 progress = 0, save = 0, mode = 0, bits = 0, count = 0;
    if (!Read32(guest, kProgress, progress) || !Read32(guest, progress + 0x18, mode) ||
        !Read32(guest, kSaveData, save) || !Read32(guest, save + 0xC30, bits) ||
        !Read32(guest, guest.gpr[3], count) || count > 4096)
      return false;
    const bool hide_random = mode == 0x3E;
    for (u32 i = 0; i < count; ++i)
    {
      u8* p = Pointer(guest, guest.gpr[4] + i * 4, 4);
      if (!p)
        return false;
      const u32 id = LoadBe32(p);
      if (id == 0x18)
      {
        if (hide_random)
          StoreBe32(p, 0x19);
      }
      else if (new_song(id) >= 0)
      {
      }
      else if (id >= 32 || !(bits & (1u << id)))
      {
        StoreBe32(p, 0x19);
      }
    }
    guest.pc = guest.lr;
    guest.downcount -= 40;
    return true;
  }
  case kClipSetUv:
  {
    // Song names (BGM Test rows, the stage selects' music selector): uv =
    // {x0, y0, x1, y1, sheet} with sheet id / 8 and row id % 8 (ClipSetTex
    // then reads the sheet from it). Ids >= 32 have no sheet: row id - 24 of
    // sheet 3. The rewrite maps an id to itself, so a second label with the
    // same uv is left as it is.
    if (std::none_of(std::begin(kSongNameDraws), std::end(kSongNameDraws),
                     [&](const CodeRange& r) { return guest.lr >= r.begin && guest.lr < r.end; }))
      return false;
    u8* p = Pointer(guest, guest.gpr[5], 20);
    if (!p)
      return false;
    const u32 id = LoadBe32(p + 16) * 8 + LoadBe32(p + 4) / 32;
    if (id >= 32 && new_song(id) >= 0)
    {
      const u32 row = id - 24;
      StoreBe32(p + 4, row * 32);
      StoreBe32(p + 12, row * 32 + 32);
      StoreBe32(p + 16, kHighNameSheet);
      ++m.remapped_rows;
    }
    return false;
  }
  default:
    return false;
  }
}

void NativeSdk::PrintModsStats() const
{
  if (!m_mods)
    return;
  const ModsState& m = *m_mods;
  std::fprintf(stderr,
               "[mods] mod_reads=%llu new_songs=%zu dynamic_plays=%llu unlock_calls=%llu "
               "remapped_rows=%llu\n",
               static_cast<unsigned long long>(m.mod_reads), m.songs.size(),
               static_cast<unsigned long long>(m.dynamic_plays),
               static_cast<unsigned long long>(m.unlock_calls),
               static_cast<unsigned long long>(m.remapped_rows));
}
}  // namespace StaticRecompNativeSdk
