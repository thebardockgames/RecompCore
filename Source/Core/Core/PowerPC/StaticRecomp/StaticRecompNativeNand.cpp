// RecompCore: StaticRecomp CPU core - native NAND store (phase C3).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeNand.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unordered_map>

namespace StaticRecompNativeNand
{
namespace
{
namespace fs = std::filesystem;

constexpr size_t kMaxPathLength = 64;
constexpr size_t kMaxFilenameLength = 12;
constexpr size_t kMaxPathDepth = 8;
constexpr u16 kClusterSize = 16384;
constexpr u16 kTotalClusters = 0x7EC0;
constexpr u16 kReservedClusters = 0x0300;
constexpr u16 kUsableClusters = kTotalClusters - kReservedClusters;
constexpr u16 kTotalInodes = 0x17FF;

bool IsValidNonRootPath(const std::string& path)
{
  return path.length() > 1 && path.length() <= kMaxPathLength && path[0] == '/' &&
         path.back() != '/';
}

bool IsValidPath(const std::string& path)
{
  return path == "/" || IsValidNonRootPath(path);
}

bool IsValidFilename(const std::string& name)
{
  return name.length() <= kMaxFilenameLength && name.find('/') == std::string::npos;
}

bool IsPrintable(char c)
{
  return static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F;
}

struct SplitPath
{
  std::string parent, file_name;
};

SplitPath SplitPathAndBasename(const std::string& path)
{
  const size_t last = path.rfind('/');
  return {path.substr(0, std::max<size_t>(1, last)), path.substr(last + 1)};
}

std::vector<std::string> SplitComponents(const std::string& path)
{
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= path.size())
  {
    const size_t end = path.find('/', start);
    out.push_back(path.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return out;
}

bool IsIllegalCharacter(char c)
{
  static constexpr char illegal[] = {'\"', '*', '/', ':', '<', '>', '?', '\\', '|', '\x7f'};
  return static_cast<unsigned char>(c) <= 0x1F ||
         std::find(std::begin(illegal), std::end(illegal), c) != std::end(illegal);
}

std::string ReplaceAll(std::string text, const std::string& from, const std::string& to)
{
  size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos)
  {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

u32 Be32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}
u16 Be16(const u8* p)
{
  return static_cast<u16>((p[0] << 8) | p[1]);
}
void PutBe32(u8* p, u32 v)
{
  p[0] = u8(v >> 24);
  p[1] = u8(v >> 16);
  p[2] = u8(v >> 8);
  p[3] = u8(v);
}
void PutBe16(u8* p, u16 v)
{
  p[0] = u8(v >> 8);
  p[1] = u8(v);
}

u64 FileSizeOf(const std::string& host_path)
{
  std::error_code ec;
  const auto size = fs::file_size(fs::path(host_path), ec);
  return ec ? 0 : size;
}

bool Exists(const std::string& host_path)
{
  std::error_code ec;
  return fs::exists(fs::path(host_path), ec);
}
bool IsFile(const std::string& host_path)
{
  std::error_code ec;
  return fs::is_regular_file(fs::path(host_path), ec);
}
bool IsDirectory(const std::string& host_path)
{
  std::error_code ec;
  return fs::is_directory(fs::path(host_path), ec);
}
}  // namespace

// Same escaping as Dolphin's NAND paths: every component with illegal
// characters becomes __xx__, double underscores are escaped first, and a
// name made only of dots is spelled out.
std::string EscapeNandName(const std::string& name)
{
  if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return c == '.'; }))
    return ReplaceAll(name, ".", "__2e__");
  const std::string escaped_underscores = ReplaceAll(name, "__", "__5f____5f__");
  std::string result;
  for (char c : escaped_underscores)
  {
    if (IsIllegalCharacter(c))
    {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "__%02x__", static_cast<unsigned char>(c));
      result += buffer;
    }
    else
    {
      result.push_back(c);
    }
  }
  return result;
}

std::string UnescapeNandName(const std::string& name)
{
  std::string result = name;
  size_t pos = 0;
  while ((pos = result.find("__", pos)) != std::string::npos)
  {
    if (pos + 6 <= result.size() && result[pos + 4] == '_' && result[pos + 5] == '_')
    {
      const std::string hex = result.substr(pos + 2, 2);
      char* end = nullptr;
      const unsigned long value = std::strtoul(hex.c_str(), &end, 16);
      if (end == hex.c_str() + 2)
        result.replace(pos, 6, std::string(1, static_cast<char>(value)));
    }
    ++pos;
  }
  return result;
}

bool NandStore::FstEntry::CheckPermission(Uid caller_uid, Gid caller_gid, Mode requested) const
{
  if (caller_uid == 0)
    return true;
  Mode file_mode = data.modes.other;
  if (data.uid == caller_uid)
    file_mode = data.modes.owner;
  else if (data.gid == caller_gid)
    file_mode = data.modes.group;
  return (u8(requested) & u8(file_mode)) == u8(requested);
}

NandStore::NandStore(std::string root) : m_root(std::move(root))
{
  while (!m_root.empty() && (m_root.back() == '/' || m_root.back() == '\\'))
    m_root.pop_back();
  std::error_code ec;
  fs::create_directories(fs::path(m_root), ec);
  ResetFst();
  LoadFst();
}

NandStore::~NandStore() = default;

std::string NandStore::HostPath(const std::string& wii_path) const
{
  // Wii paths start with '/', so the first component is empty: every
  // component after it is appended with its own slash.
  std::string escaped;
  const std::vector<std::string> components = SplitComponents(wii_path);
  for (size_t i = 1; i < components.size(); ++i)
    escaped += '/' + EscapeNandName(components[i]);
  return m_root + escaped;
}

// ---- FST (fst.bin, 32-byte big-endian entries in depth-first order) ----

void NandStore::ResetFst()
{
  m_root_entry = {};
  m_root_entry.name = "/";
  m_root_entry.data.modes = {Mode::None, Mode::Read, Mode::Read};
}

void NandStore::LoadFst()
{
  std::FILE* file = std::fopen((m_root + "/fst.bin").c_str(), "rb");
  if (!file)
    return;
  std::vector<u8> bytes;
  u8 chunk[4096];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
    bytes.insert(bytes.end(), chunk, chunk + n);
  std::fclose(file);
  size_t offset = 0;
  const auto parse = [&](const auto& self, size_t depth) -> std::optional<FstEntry> {
    if (depth > kMaxPathDepth || offset + 32 > bytes.size())
      return std::nullopt;
    const u8* e = bytes.data() + offset;
    offset += 32;
    FstEntry entry;
    entry.name.assign(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 12));
    entry.data.uid = Be32(e + 12);
    entry.data.gid = Be16(e + 16);
    entry.data.is_file = e[18] != 0;
    entry.data.modes = {static_cast<Mode>(e[19]), static_cast<Mode>(e[20]), static_cast<Mode>(e[21])};
    entry.data.attribute = e[22];
    const u32 children = Be32(e + 28);
    for (u32 i = 0; i < children; ++i)
    {
      auto child = self(self, depth + 1);
      if (!child)
        return std::nullopt;
      entry.children.push_back(std::move(*child));
    }
    return entry;
  };
  if (auto root = parse(parse, 0))
    m_root_entry = std::move(*root);
  else
    std::fprintf(stderr, "[native_nand] fst.bin could not be parsed; using defaults\n");
}

void NandStore::SaveFst() const
{
  std::vector<u8> bytes;
  const auto collect = [&](const auto& self, const FstEntry& entry) -> void {
    u8 e[32] = {};
    std::memcpy(e, entry.name.data(), std::min<size_t>(12, entry.name.size()));
    PutBe32(e + 12, entry.data.uid);
    PutBe16(e + 16, entry.data.gid);
    e[18] = entry.data.is_file ? 1 : 0;
    e[19] = static_cast<u8>(entry.data.modes.owner);
    e[20] = static_cast<u8>(entry.data.modes.group);
    e[21] = static_cast<u8>(entry.data.modes.other);
    e[22] = entry.data.attribute;
    PutBe32(e + 28, static_cast<u32>(entry.children.size()));
    bytes.insert(bytes.end(), e, e + 32);
    for (const FstEntry& child : entry.children)
      self(self, child);
  };
  collect(collect, m_root_entry);
  const std::string dest = m_root + "/fst.bin", temp = dest + ".tmp";
  std::FILE* file = std::fopen(temp.c_str(), "wb");
  if (!file)
    return;
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  std::fclose(file);
  std::error_code ec;
  if (ok)
    fs::rename(fs::path(temp), fs::path(dest), ec);
  if (!ok || ec)
    std::fprintf(stderr, "[native_nand] could not write fst.bin\n");
}

// Dolphin's IOS writes a few entries of its own at boot (uid.sys when the
// title launches) through its file system object, which shares fst.bin with
// this store. Re-reading the file before every change keeps both in step as
// long as writes do not interleave (they happen at boot, before the game).
void NandStore::ReloadFst()
{
  ResetFst();
  LoadFst();
}

NandStore::FstEntry* NandStore::EntryForPath(const std::string& path)
{
  if (path == "/")
    return &m_root_entry;
  if (!IsValidNonRootPath(path))
    return nullptr;
  const std::string host = HostPath(path);
  if (!Exists(host))
    return nullptr;
  FstEntry* entry = &m_root_entry;
  const std::vector<std::string> components = SplitComponents(path.substr(1));
  for (const std::string& component : components)
  {
    auto next = std::find_if(entry->children.begin(), entry->children.end(),
                             [&](const FstEntry& c) { return c.name == component; });
    if (next != entry->children.end())
    {
      entry = &*next;
    }
    else
    {
      // Default entry for files the host folder has but the FST does not know
      // (also the path taken when creating; metadata is filled afterwards).
      entry = &entry->children.emplace_back();
      entry->name = component;
      entry->data.modes = {Mode::ReadWrite, Mode::ReadWrite, Mode::ReadWrite};
    }
  }
  entry->data.is_file = IsFile(host);
  if (entry->data.is_file && !entry->children.empty())
    entry->children.clear();
  return entry;
}

// ---- files ----

std::shared_ptr<std::FILE> NandStore::OpenHostFile(const std::string& host_path)
{
  const auto it = m_open_files.find(host_path);
  if (it != m_open_files.end())
  {
    if (auto existing = it->second.lock())
      return existing;
  }
  std::FILE* raw = std::fopen(host_path.c_str(), "r+b");
  if (!raw)
    return nullptr;
  std::shared_ptr<std::FILE> file(raw, [this, host_path](std::FILE* f) {
    std::fclose(f);
    m_open_files.erase(host_path);
  });
  m_open_files[host_path] = file;
  return file;
}

NandStore::Handle* NandStore::HandleOf(u32 fd)
{
  if (fd >= kMaxHandles || !m_handles[fd].opened)
    return nullptr;
  return &m_handles[fd];
}

Result<u32> NandStore::OpenFile(Uid, Gid, const std::string& path, Mode mode)
{
  const auto free = std::find_if(m_handles.begin(), m_handles.end(),
                                 [](const Handle& h) { return !h.opened; });
  if (free == m_handles.end())
    return std::unexpected{ResultCode::NoFreeHandle};
  const std::string host = HostPath(path);
  if (IsDirectory(host))
    return std::unexpected{ResultCode::Invalid};
  if (!IsFile(host))
    return std::unexpected{ResultCode::NotFound};
  auto file = OpenHostFile(host);
  if (!file)
    return std::unexpected{ResultCode::AccessDenied};
  *free = Handle{};
  free->opened = true;
  free->path = path;
  free->mode = mode;
  free->offset = 0;
  free->file = std::move(file);
  return static_cast<u32>(free - m_handles.begin());
}

ResultCode NandStore::Close(u32 fd)
{
  Handle* handle = HandleOf(fd);
  if (!handle)
    return ResultCode::Invalid;
  *handle = Handle{};
  return ResultCode::Success;
}

Result<u32> NandStore::ReadBytesFromFile(u32 fd, u8* out, u32 count)
{
  Handle* handle = HandleOf(fd);
  if (!handle || !handle->file)
    return std::unexpected{ResultCode::Invalid};
  if ((u8(handle->mode) & u8(Mode::Read)) == 0)
    return std::unexpected{ResultCode::AccessDenied};
  std::fseek(handle->file.get(), 0, SEEK_END);
  const u32 size = static_cast<u32>(std::ftell(handle->file.get()));
  if (count + handle->offset > size)
    count = size - handle->offset;
  std::fseek(handle->file.get(), static_cast<long>(handle->offset), SEEK_SET);
  const u32 done = static_cast<u32>(std::fread(out, 1, count, handle->file.get()));
  if (done != count && std::ferror(handle->file.get()))
    return std::unexpected{ResultCode::AccessDenied};
  handle->offset += done;
  return done;
}

Result<u32> NandStore::WriteBytesToFile(u32 fd, const u8* in, u32 count)
{
  Handle* handle = HandleOf(fd);
  if (!handle || !handle->file)
    return std::unexpected{ResultCode::Invalid};
  if ((u8(handle->mode) & u8(Mode::Write)) == 0)
    return std::unexpected{ResultCode::AccessDenied};
  std::fseek(handle->file.get(), static_cast<long>(handle->offset), SEEK_SET);
  if (std::fwrite(in, 1, count, handle->file.get()) != count)
    return std::unexpected{ResultCode::AccessDenied};
  std::fflush(handle->file.get());
  handle->offset += count;
  return count;
}

Result<u32> NandStore::SeekFile(u32 fd, u32 offset, SeekMode mode)
{
  Handle* handle = HandleOf(fd);
  if (!handle || !handle->file)
    return std::unexpected{ResultCode::Invalid};
  std::fseek(handle->file.get(), 0, SEEK_END);
  const u32 size = static_cast<u32>(std::ftell(handle->file.get()));
  u32 position;
  switch (mode)
  {
  case SeekMode::Set: position = offset; break;
  case SeekMode::Current: position = handle->offset + offset; break;
  case SeekMode::End: position = size + offset; break;
  default: return std::unexpected{ResultCode::Invalid};
  }
  if (size < position)
    return std::unexpected{ResultCode::Invalid};
  handle->offset = position;
  return position;
}

Result<FileStatus> NandStore::GetFileStatus(u32 fd)
{
  Handle* handle = HandleOf(fd);
  if (!handle || !handle->file)
    return std::unexpected{ResultCode::Invalid};
  std::fseek(handle->file.get(), 0, SEEK_END);
  FileStatus status;
  status.size = static_cast<u32>(std::ftell(handle->file.get()));
  status.offset = handle->offset;
  return status;
}

bool NandStore::IsFileOpened(const std::string& path) const
{
  return std::any_of(m_handles.begin(), m_handles.end(),
                     [&](const Handle& h) { return h.opened && h.path == path; });
}

bool NandStore::IsDirectoryInUse(const std::string& path) const
{
  return std::any_of(m_handles.begin(), m_handles.end(),
                     [&](const Handle& h) { return h.opened && h.path.starts_with(path); });
}

// ---- directories and metadata ----

ResultCode NandStore::Format(Uid uid)
{
  if (uid != 0 || m_root.empty())
    return ResultCode::AccessDenied;
  std::error_code ec;
  fs::remove_all(fs::path(m_root), ec);
  fs::create_directories(fs::path(m_root), ec);
  if (ec)
    return ResultCode::UnknownError;
  ResetFst();
  SaveFst();
  m_handles = {};
  return ResultCode::Success;
}

ResultCode NandStore::CreateFileOrDirectory(Uid uid, Gid gid, const std::string& path,
                                            FileAttribute attr, Modes modes, bool is_file)
{
  ReloadFst();
  if (!IsValidNonRootPath(path) || !std::all_of(path.begin(), path.end(), IsPrintable))
    return ResultCode::Invalid;
  if (!is_file && static_cast<size_t>(std::count(path.begin(), path.end(), '/')) > kMaxPathDepth)
    return ResultCode::TooManyPathComponents;
  const SplitPath split = SplitPathAndBasename(path);
  const std::string host = HostPath(path);
  FstEntry* parent = EntryForPath(split.parent);
  if (!parent)
    return ResultCode::NotFound;
  if (!parent->CheckPermission(uid, gid, Mode::Write))
    return ResultCode::AccessDenied;
  if (Exists(host))
    return ResultCode::AlreadyExists;
  bool ok;
  if (is_file)
  {
    std::FILE* f = std::fopen(host.c_str(), "wb");
    ok = f != nullptr;
    if (f)
      std::fclose(f);
  }
  else
  {
    std::error_code ec;
    ok = fs::create_directory(fs::path(host), ec) && !ec;
  }
  if (!ok)
    return ResultCode::UnknownError;
  FstEntry* child = EntryForPath(path);
  *child = {};
  child->name = split.file_name;
  child->data.is_file = is_file;
  child->data.modes = modes;
  child->data.uid = uid;
  child->data.gid = gid;
  child->data.attribute = attr;
  SaveFst();
  return ResultCode::Success;
}

ResultCode NandStore::CreateFile(Uid uid, Gid gid, const std::string& path, FileAttribute attr,
                                 Modes modes)
{
  return CreateFileOrDirectory(uid, gid, path, attr, modes, true);
}

ResultCode NandStore::CreateDirectory(Uid uid, Gid gid, const std::string& path,
                                      FileAttribute attr, Modes modes)
{
  return CreateFileOrDirectory(uid, gid, path, attr, modes, false);
}

ResultCode NandStore::Delete(Uid uid, Gid gid, const std::string& path)
{
  ReloadFst();
  if (!IsValidNonRootPath(path))
    return ResultCode::Invalid;
  const std::string host = HostPath(path);
  const SplitPath split = SplitPathAndBasename(path);
  FstEntry* parent = EntryForPath(split.parent);
  if (!parent)
    return ResultCode::NotFound;
  if (!parent->CheckPermission(uid, gid, Mode::Write))
    return ResultCode::AccessDenied;
  if (!Exists(host))
    return ResultCode::NotFound;
  std::error_code ec;
  if (IsFile(host) && !IsFileOpened(path))
    fs::remove(fs::path(host), ec);
  else if (IsDirectory(host) && !IsDirectoryInUse(path))
    fs::remove_all(fs::path(host), ec);
  else
    return ResultCode::InUse;
  const auto it = std::find_if(parent->children.begin(), parent->children.end(),
                               [&](const FstEntry& c) { return c.name == split.file_name; });
  if (it != parent->children.end())
    parent->children.erase(it);
  SaveFst();
  return ResultCode::Success;
}

ResultCode NandStore::Rename(Uid uid, Gid gid, const std::string& old_path,
                             const std::string& new_path)
{
  ReloadFst();
  if (!IsValidNonRootPath(old_path) || !IsValidNonRootPath(new_path))
    return ResultCode::Invalid;
  const SplitPath split_old = SplitPathAndBasename(old_path), split_new = SplitPathAndBasename(new_path);
  FstEntry* old_parent = EntryForPath(split_old.parent);
  FstEntry* new_parent = EntryForPath(split_new.parent);
  if (!old_parent || !new_parent)
    return ResultCode::NotFound;
  if (!old_parent->CheckPermission(uid, gid, Mode::Write) ||
      !new_parent->CheckPermission(uid, gid, Mode::Write))
    return ResultCode::AccessDenied;
  FstEntry* entry = EntryForPath(old_path);
  if (!entry)
    return ResultCode::NotFound;
  if (entry->data.is_file && split_old.file_name != split_new.file_name)
    return ResultCode::Invalid;
  if ((!entry->data.is_file && IsDirectoryInUse(old_path)) ||
      (entry->data.is_file && IsFileOpened(old_path)))
    return ResultCode::InUse;
  const std::string host_old = HostPath(old_path), host_new = HostPath(new_path);
  std::error_code ec;
  if (Exists(host_new))
  {
    const bool old_is_file = IsFile(host_old), new_is_file = IsFile(host_new);
    if (old_is_file && new_is_file)
      fs::remove(fs::path(host_new), ec);
    else if (!old_is_file && !new_is_file)
      fs::remove_all(fs::path(host_new), ec);
    else
      return ResultCode::Invalid;
  }
  fs::rename(fs::path(host_old), fs::path(host_new), ec);
  if (ec)
    return ResultCode::NotFound;
  FstEntry* new_entry = EntryForPath(new_path);
  new_entry->name = split_new.file_name;
  // old_parent may have been invalidated by EntryForPath's emplace; look it up again.
  old_parent = EntryForPath(split_old.parent);
  if (old_parent)
  {
    const auto it = std::find_if(old_parent->children.begin(), old_parent->children.end(),
                                 [&](const FstEntry& c) { return c.name == split_old.file_name; });
    if (it != old_parent->children.end() && &*it != new_entry)
    {
      new_entry->data = it->data;
      new_entry->children = it->children;
      old_parent->children.erase(it);
    }
  }
  SaveFst();
  return ResultCode::Success;
}

NandStore::HostEntry NandStore::Scan(const std::string& host_path, bool recursive) const
{
  HostEntry dir;
  dir.is_directory = true;
  std::error_code ec;
  for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(host_path), ec))
  {
    HostEntry child;
    child.name = entry.path().filename().string();
    child.is_directory = entry.is_directory(ec);
    if (child.is_directory)
    {
      if (recursive)
      {
        child = Scan(entry.path().string(), true);
        child.is_directory = true;
        child.name = entry.path().filename().string();
        dir.size += child.size;
      }
    }
    else
    {
      child.size = entry.file_size(ec);
      if (ec)
        child.size = 0;
    }
    dir.children.push_back(std::move(child));
    ++dir.size;
  }
  return dir;
}

u64 NandStore::Fixup(HostEntry* dir, bool is_root) const
{
  u64 removed = 0;
  for (auto it = dir->children.begin(); it != dir->children.end();)
  {
    if (is_root && !it->is_directory)
    {
      ++removed;
      it = dir->children.erase(it);
      continue;
    }
    if (it->name.find("__") != std::string::npos)
      it->name = UnescapeNandName(it->name);
    if (!IsValidFilename(it->name))
    {
      if (it->is_directory)
        removed += it->size;
      ++removed;
      it = dir->children.erase(it);
      continue;
    }
    if (dir->is_directory)
      removed += Fixup(&*it, false);
    ++it;
  }
  dir->size -= removed;
  return removed;
}

Result<std::vector<std::string>> NandStore::ReadDirectory(Uid uid, Gid gid, const std::string& path)
{
  ReloadFst();
  if (!IsValidPath(path))
    return std::unexpected{ResultCode::Invalid};
  const FstEntry* entry = EntryForPath(path);
  if (!entry)
    return std::unexpected{ResultCode::NotFound};
  if (!entry->CheckPermission(uid, gid, Mode::Read))
    return std::unexpected{ResultCode::AccessDenied};
  if (entry->data.is_file)
    return std::unexpected{ResultCode::Invalid};
  HostEntry host = Scan(HostPath(path), false);
  Fixup(&host, path == "/");
  // Dolphin's order: FST order reversed (newest first), unknown names after.
  std::unordered_map<std::string, int> keys;
  for (size_t i = 0; i < entry->children.size(); ++i)
    keys.emplace(entry->children[i].name, static_cast<int>(i));
  const auto key = [&](const std::string& name) {
    const auto it = keys.find(name);
    return it != keys.end() ? it->second : -1;
  };
  std::sort(host.children.begin(), host.children.end(), [&](const HostEntry& a, const HostEntry& b) {
    const int ka = key(a.name), kb = key(b.name);
    if (ka != kb)
      return ka > kb;
    return a.name > b.name;
  });
  std::vector<std::string> names;
  for (const HostEntry& child : host.children)
    names.push_back(child.name);
  return names;
}

Result<Metadata> NandStore::GetMetadata(Uid uid, Gid gid, const std::string& path)
{
  ReloadFst();
  const FstEntry* entry = nullptr;
  if (path == "/")
  {
    entry = &m_root_entry;
  }
  else
  {
    if (!IsValidNonRootPath(path))
      return std::unexpected{ResultCode::Invalid};
    const SplitPath split = SplitPathAndBasename(path);
    const FstEntry* parent = EntryForPath(split.parent);
    if (!parent)
      return std::unexpected{ResultCode::NotFound};
    if (!parent->CheckPermission(uid, gid, Mode::Read))
      return std::unexpected{ResultCode::AccessDenied};
    entry = EntryForPath(path);
  }
  if (!entry)
    return std::unexpected{ResultCode::NotFound};
  Metadata metadata = entry->data;
  metadata.size = static_cast<u32>(FileSizeOf(HostPath(path)));
  return metadata;
}

ResultCode NandStore::SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                                  FileAttribute attr, Modes modes)
{
  ReloadFst();
  if (!IsValidPath(path))
    return ResultCode::Invalid;
  FstEntry* entry = EntryForPath(path);
  if (!entry)
    return ResultCode::NotFound;
  if (caller_uid != 0 && caller_uid != entry->data.uid)
    return ResultCode::AccessDenied;
  if (caller_uid != 0 && uid != entry->data.uid)
    return ResultCode::AccessDenied;
  const bool is_empty = FileSizeOf(HostPath(path)) == 0;
  if (entry->data.uid != uid && entry->data.is_file && !is_empty)
    return ResultCode::FileNotEmpty;
  if (entry->data.gid != gid || entry->data.uid != uid || entry->data.attribute != attr ||
      !(entry->data.modes == modes))
  {
    entry->data.gid = gid;
    entry->data.uid = uid;
    entry->data.attribute = attr;
    entry->data.modes = modes;
    SaveFst();
  }
  return ResultCode::Success;
}

Result<DirectoryStats> NandStore::GetDirectoryStats(const std::string& path)
{
  if (!IsValidPath(path))
    return std::unexpected{ResultCode::Invalid};
  const std::string host = HostPath(path);
  if (!Exists(host))
    return std::unexpected{ResultCode::NotFound};
  if (!IsDirectory(host))
    return std::unexpected{ResultCode::Invalid};
  HostEntry tree = Scan(host, true);
  Fixup(&tree, path == "/");
  const auto clusters = [](const auto& self, const HostEntry& dir) -> u64 {
    u64 total = 0;
    for (const HostEntry& child : dir.children)
      total += child.is_directory ? self(self, child) : (child.size + kClusterSize - 1) / kClusterSize;
    return total;
  };
  DirectoryStats stats{};
  stats.used_inodes = static_cast<u32>(std::min<u64>(1 + tree.size, kTotalInodes));
  stats.used_clusters = static_cast<u32>(std::min<u64>(clusters(clusters, tree), kUsableClusters));
  return stats;
}

Result<NandStats> NandStore::GetNandStats()
{
  const auto root = GetDirectoryStats("/");
  if (!root)
    return std::unexpected{root.error()};
  NandStats stats{};
  stats.cluster_size = kClusterSize;
  stats.free_clusters = kUsableClusters - root->used_clusters;
  stats.used_clusters = root->used_clusters;
  stats.bad_clusters = 0;
  stats.reserved_clusters = kReservedClusters;
  stats.free_inodes = kTotalInodes - root->used_inodes;
  stats.used_inodes = root->used_inodes;
  return stats;
}

// ---- savestates ----

NandStore::HandleState NandStore::GetHandleState(u32 fd) const
{
  HandleState state;
  if (fd < kMaxHandles && m_handles[fd].opened)
  {
    state.opened = true;
    state.path = m_handles[fd].path;
    state.mode = m_handles[fd].mode;
    state.offset = m_handles[fd].offset;
  }
  return state;
}

bool NandStore::RestoreHandle(u32 fd, const HandleState& state)
{
  if (fd >= kMaxHandles)
    return false;
  m_handles[fd] = Handle{};
  if (!state.opened)
    return true;
  auto file = OpenHostFile(HostPath(state.path));
  if (!file)
    return false;
  m_handles[fd].opened = true;
  m_handles[fd].path = state.path;
  m_handles[fd].mode = state.mode;
  m_handles[fd].offset = state.offset;
  m_handles[fd].file = std::move(file);
  return true;
}

void NandStore::CloseAll()
{
  m_handles = {};
}
}  // namespace StaticRecompNativeNand
