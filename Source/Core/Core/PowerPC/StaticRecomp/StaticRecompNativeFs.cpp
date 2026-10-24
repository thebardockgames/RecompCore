// RecompCore: StaticRecomp CPU core - native NAND file system.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The game reaches the Wii NAND (saves, SYSCONF, banner) through the SDK's
// NAND/ISFS libraries, which all end in the IPC calls IOS_Open/Close/
// Read/Write/Seek/Ioctl/Ioctlv (and their async forms) to the IOS. Those
// calls are intercepted here: /dev/fs and NAND paths are served directly by
// the host NAND (Dolphin's FS::FileSystem, the same folder and metadata the
// emulated IOS uses, so saves stay compatible), with no IOS kernel, device
// dispatch, IPC request or simulated FS timing. Other devices (/dev/di,
// /dev/es, Bluetooth...) and descriptors the IOS handed out keep the
// original code.
//
// Each request completes at once. Async callbacks run through a native
// interrupt on a fresh OSContext, as the SDK's IPC reply handler does. The
// argument handling and result codes follow Dolphin's /dev/fs proxy
// (Core/IOS/FS/FileSystemProxy.cpp).

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeFs.h"

#include <algorithm>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestMemory.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompFsTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeNand.h"

namespace StaticRecompNativeSdk
{
namespace
{
using StaticRecompFs::ResultCode;
namespace FS = StaticRecompFs;

#include "native/fs.inc"  // the game's addresses and fingerprints



// ISFS ioctls (Core/IOS/FS/FileSystemProxy.h).
enum : u32
{
  ISFS_IOCTL_FORMAT = 1,
  ISFS_IOCTL_GETSTATS = 2,
  ISFS_IOCTL_CREATEDIR = 3,
  ISFS_IOCTLV_READDIR = 4,
  ISFS_IOCTL_SETATTR = 5,
  ISFS_IOCTL_GETATTR = 6,
  ISFS_IOCTL_DELETE = 7,
  ISFS_IOCTL_RENAME = 8,
  ISFS_IOCTL_CREATEFILE = 9,
  ISFS_IOCTL_SETFILEVERCTRL = 10,
  ISFS_IOCTL_GETFILESTATS = 11,
  ISFS_IOCTLV_GETUSAGE = 12,
  ISFS_IOCTL_SHUTDOWN = 13,
};
constexpr u32 kIsfsParamsSize = 4 + 2 + 64 + 3 + 1;  // uid, gid, path, modes, attribute
constexpr s64 kFsCycles = 300;

u64 Fnv1a(const u8* data, u32 size)
{
  u64 hash = 0xcbf29ce484222325ull;
  for (u32 i = 0; i < size; ++i)
    hash = (hash ^ data[i]) * 0x100000001b3ull;
  return hash;
}

s32 Convert(ResultCode code)
{
  return FS::ConvertResult(code);
}

// Whether a guest address range is backed by MEM1/MEM2 (no panic dialogs).
bool ValidRange(const StaticRecompGuestMemory::GuestMemory& memory, u32 address, u32 size)
{
  const u32 physical = address & 0x3FFFFFFF;
  if (u64(physical) + size <= memory.GetRamSizeReal())
    return true;
  return (physical >> 28) == 1 && memory.GetEXRAM() &&
         u64(physical & 0x0FFFFFFF) + size <= memory.GetExRamSizeReal();
}

std::string ReadString(const StaticRecompGuestMemory::GuestMemory& memory, u32 address, u32 max)
{
  std::string result;
  for (u32 i = 0; i < max && ValidRange(memory, address + i, 1); ++i)
  {
    const char c = static_cast<char>(memory.Read_U8(address + i));
    if (c == 0)
      break;
    result.push_back(c);
  }
  return result;
}

// The IOS devices answered natively besides /dev/fs (see IpcDeviceKind).
std::optional<IpcDeviceKind> VirtualDeviceKind(const std::string& path)
{
  using Kind = IpcDeviceKind;
  if (path == "/dev/stm/immediate") return Kind::StmImmediate;
  if (path == "/dev/stm/eventhook") return Kind::StmEventHook;
  if (path == "/dev/es") return Kind::Es;
  if (path == "/dev/net/kd/request") return Kind::KdRequest;
  if (path == "/dev/net/kd/time") return Kind::KdTime;
  if (path == "/dev/di") return Kind::Di;
  return std::nullopt;
}

const char* KindName(IpcDeviceKind kind)
{
  using Kind = IpcDeviceKind;
  switch (kind)
  {
  case Kind::File: return "file";
  case Kind::Fs: return "/dev/fs";
  case Kind::StmImmediate: return "/dev/stm/immediate";
  case Kind::StmEventHook: return "/dev/stm/eventhook";
  case Kind::Es: return "/dev/es";
  case Kind::KdRequest: return "/dev/net/kd/request";
  case Kind::KdTime: return "/dev/net/kd/time";
  case Kind::Di: return "/dev/di";
  }
  return "?";
}

bool IsNandPath(const std::string& path)
{
  return path == "/dev/fs" || (!path.empty() && path[0] == '/' && path.rfind("/dev/", 0) != 0);
}
}  // namespace

bool NativeSdk::FsBind(const std::function<bool(u32)>& enable, bool wanted)
{
  const char* setting = std::getenv("MODERNGEKKO_NATIVE_FS");
  if (!wanted || (setting && (std::string(setting) == "off" || std::string(setting) == "0")))
    return false;
  if (!m_context_call_hook)
  {
    std::fprintf(stderr, "[native_fs] needs native interrupts and the context call hook: disabled\n");
    return false;
  }
  if (GetNandRoot().empty())
  {
    std::fprintf(stderr, "[native_fs] no NAND root given by the runtime: disabled\n");
    return false;
  }
  m_nand = std::make_unique<StaticRecompNativeNand::NandStore>(GetNandRoot());
  for (u32 address : kHooks)
  {
    if (!enable(address))
    {
      std::fprintf(stderr, "[native_fs] hook 0x%08X missing from the module: disabled\n", address);
      return false;
    }
  }
  m_fs = std::make_unique<FsState>();
  std::fprintf(stderr, "[native_fs] NAND file system: %zu IPC hooks\n", std::size(kHooks));
  s_ipc_native = true;
  return true;
}

bool NativeSdk::FsRun(CPUState& guest)
{
  FsState& fs = *m_fs;
  if (fs.abandoned || std::find(std::begin(kHooks), std::end(kHooks), guest.pc) == std::end(kHooks))
    return false;
  if (!fs.active)
  {
    // The chassis's IOS kernel is not needed: the title identity comes from
    // the native boot (or, under the chassis's boot, from that kernel).
    u32 uid = 0;
    u16 gid = 0;
    bool ok = guest.gpr[13] == kSda && m_nand != nullptr &&
              (GetTitleIdentity(&uid, &gid) ||
               (s_chassis.ios_title_identity && s_chassis.ios_title_identity(&uid, &gid)));
    for (const auto& code : kIpcCode)
    {
      const u8* p = Pointer(guest, code.address, code.size);
      if (!p || Fnv1a(p, code.size) != code.fnv1a)
      {
        std::fprintf(stderr, "[native_fs] SDK code at 0x%08X differs from the original\n",
                     code.address);
        ok = false;
      }
    }
    if (!ok)
    {
      fs.abandoned = true;
      return false;
    }
    fs.active = true;
  }

  const StaticRecompGuestMemory::GuestMemory memory(guest);
  StaticRecompNativeNand::NandStore* const file_system = m_nand.get();  // own store, not IOS
  const auto handle_of = [&](u32 fd) -> FsState::Handle* {
    const auto it = fs.handles.find(fd);
    return it == fs.handles.end() ? nullptr : &it->second;
  };
  // The IPC call returns `result` (sync) or calls back cb(result, usrdata).
  const auto finish = [&](s32 result, u32 callback, u32 usrdata, bool is_async) {
    if (result < 0 && ++fs.errors <= 24)
    {
      const FsState::Handle* handle = handle_of(guest.gpr[3]);
      std::fprintf(stderr, "[native_fs] call 0x%08X (%s, request %u) -> %d\n", guest.pc,
                   handle ? handle->path.data() : ReadString(memory, guest.gpr[3], 64).c_str(),
                   guest.gpr[4], result);
    }
    if (is_async && callback)
    {
      ++fs.async;
      PostOrDefer({callback, static_cast<u32>(result), usrdata, 0, 0, CallbackKind::ContextCall});
      guest.gpr[3] = 0;  // IPC_OK: request queued
    }
    else
    {
      // An async call without callback waits like a sync one (__ios_Ipc2).
      guest.gpr[3] = static_cast<u32>(result);
    }
    guest.pc = guest.lr;
    guest.downcount -= kFsCycles;
    return true;
  };

  switch (guest.pc)
  {
  case kIosOpen:
  case kIosOpenAsync:
  {
    const bool is_async = guest.pc == kIosOpenAsync;
    const std::string path = ReadString(memory, guest.gpr[3], 64);
    // A native descriptor completes async calls through native interrupts:
    // before the OS takes them, the original keeps this handle.
    const std::optional<IpcDeviceKind> virtual_kind = VirtualDeviceKind(path);
    if ((!IsNandPath(path) && !virtual_kind) || !InstallNativeInterrupt(guest))
    {
      if (++fs.opens_original <= 24)
        std::fprintf(stderr, "[native_fs] open %s mode %u: original IOS\n", path.c_str(),
                     guest.gpr[4] & 3);
      return false;
    }
    ++fs.opens;
    if (fs.logged_paths < 24)
    {
      ++fs.logged_paths;
      std::fprintf(stderr, "[native_fs] open %s mode %u\n", path.c_str(), guest.gpr[4] & 3);
    }
    FsState::Handle handle;
    // The title's uid and group: from the native boot (as ES assigns them)
    // or, under the chassis's boot, from its IOS title context.
    u32 title_uid = 0;
    u16 title_gid = 0;
    if (GetTitleIdentity(&title_uid, &title_gid))
    {
      handle.uid = title_uid;
      handle.gid = title_gid;
    }
    else if (s_chassis.ios_title_identity)
    {
      s_chassis.ios_title_identity(&handle.uid, &handle.gid);
    }
    std::strncpy(handle.path.data(), path.c_str(), handle.path.size() - 1);
    s32 result;
    if (fs.handles.size() >= 16)
    {
      result = Convert(ResultCode::NoFreeHandle);
    }
    else if (path == "/dev/fs")
    {
      handle.device = true;
      handle.kind = IpcDeviceKind::Fs;
      result = static_cast<s32>(fs.next_fd);
    }
    else if (virtual_kind)
    {
      handle.device = true;
      handle.kind = *virtual_kind;
      ++fs.virtual_opens;
      result = static_cast<s32>(fs.next_fd);
    }
    else
    {
      auto opened = file_system->OpenFile(handle.uid, handle.gid, path,
                                          static_cast<FS::Mode>(guest.gpr[4] & 3));
      if (opened)
      {
        handle.fs_fd = *opened;
        result = static_cast<s32>(fs.next_fd);
      }
      else
      {
        result = Convert(opened.error());
      }
    }
    if (result >= 0)
      fs.handles[fs.next_fd++] = handle;
    return finish(result, is_async ? guest.gpr[5] : 0, is_async ? guest.gpr[6] : 0, is_async);
  }
  case kIosClose:
  case kIosCloseAsync:
  {
    const bool is_async = guest.pc == kIosCloseAsync;
    FsState::Handle* handle = handle_of(guest.gpr[3]);
    if (!handle)
      return false;
    ++fs.closes;
    s32 result = 0;
    if (!handle->device)
    {
      const ResultCode code = file_system->Close(handle->fs_fd);
      if (code != ResultCode::Success)
        result = Convert(code);
    }
    fs.handles.erase(guest.gpr[3]);
    return finish(result, is_async ? guest.gpr[4] : 0, is_async ? guest.gpr[5] : 0, is_async);
  }
  case kIosReadAsync:
  case kIosWriteAsync:
  {
    FsState::Handle* handle = handle_of(guest.gpr[3]);
    if (!handle)
      return false;
    const bool is_write = guest.pc == kIosWriteAsync;
    const u32 address = guest.gpr[4], length = guest.gpr[5];
    s32 result;
    if (handle->device || !ValidRange(memory, address, length))
    {
      result = Convert(ResultCode::Invalid);
    }
    else
    {
      u8* data = length ? memory.GetPointerForRange(address, length) : nullptr;
      const FS::Result<u32> done =
          is_write ? file_system->WriteBytesToFile(handle->fs_fd, data, length) :
                     file_system->ReadBytesFromFile(handle->fs_fd, data, length);
      result = done ? static_cast<s32>(*done) : Convert(done.error());
      if (done)
        (is_write ? fs.written_bytes : fs.read_bytes) += *done;
    }
    ++(is_write ? fs.writes : fs.reads);
    return finish(result, guest.gpr[6], guest.gpr[7], true);
  }
  case kIosSeekAsync:
  {
    FsState::Handle* handle = handle_of(guest.gpr[3]);
    if (!handle)
      return false;
    ++fs.seeks;
    s32 result;
    if (handle->device)
    {
      result = Convert(ResultCode::Invalid);
    }
    else
    {
      const FS::Result<u32> pos =
          file_system->SeekFile(handle->fs_fd, guest.gpr[4], static_cast<FS::SeekMode>(guest.gpr[5]));
      result = pos ? static_cast<s32>(*pos) : Convert(pos.error());
    }
    return finish(result, guest.gpr[6], guest.gpr[7], true);
  }
  case kIosIoctl:
  case kIosIoctlAsync:
  {
    const bool is_async = guest.pc == kIosIoctlAsync;
    FsState::Handle* handle = handle_of(guest.gpr[3]);
    if (!handle)
      return false;
    ++fs.ioctls;
    const u32 request = guest.gpr[4];
    const u32 in = guest.gpr[5], in_size = guest.gpr[6];
    const u32 out = guest.gpr[7], out_size = guest.gpr[8];
    const bool in_ok = ValidRange(memory, in, in_size);
    const bool out_ok = ValidRange(memory, out, out_size);
    s32 result = Convert(ResultCode::Invalid);
    // ISFSParams: be32 uid, be16 gid, char path[64], u8 modes[3], u8 attribute.
    const auto params_path = [&] { return ReadString(memory, in + 6, 64); };
    const auto params_modes = [&] {
      return FS::Modes{static_cast<FS::Mode>(memory.Read_U8(in + 70)),
                       static_cast<FS::Mode>(memory.Read_U8(in + 71)),
                       static_cast<FS::Mode>(memory.Read_U8(in + 72))};
    };
    const auto params_attribute = [&] { return memory.Read_U8(in + 73); };
    if (handle->kind != IpcDeviceKind::File && handle->kind != IpcDeviceKind::Fs)
    {
      ++fs.virtual_calls;
      switch (handle->kind)
      {
      case IpcDeviceKind::StmImmediate:
        // IOCTL_STM_VIDIMMING (0x5001) and IOCTL_STM_LEDMODE (0x6002): nothing
        // to dim or light on a PC; Dolphin answers IPC_SUCCESS for every STM
        // immediate request too.
        result = 0;
        break;
      case IpcDeviceKind::StmEventHook:
        // IOCTL_STM_EVENTHOOK (0x1000) waits for the reset or power button,
        // which the port never presses: the request stays pending forever.
        if (request == 0x1000 && is_async)
        {
          ++fs.virtual_pending;
          guest.gpr[3] = 0;  // IPC_OK: request queued
          guest.pc = guest.lr;
          guest.downcount -= kFsCycles;
          return true;
        }
        result = request == 0x1000 ? -17 /* IPC_EEXIST */ : -4 /* IPC_EINVAL */;
        break;
      case IpcDeviceKind::KdTime:
        // IOCTL_NW24_SET_RTC_COUNTER (0x17): Dolphin stores the counter and
        // writes a zero common result to the output buffer.
        if (request == 0x17 && out_ok && out_size >= 4)
        {
          memory.Write_U32(0, out);
          result = 0;
        }
        else
        {
          ++fs.virtual_unknown;
          result = -4;
        }
        break;
      default:
        ++fs.virtual_unknown;
        result = -4;
        break;
      }
      if (result < 0 && fs.virtual_unknown <= 8)
        std::fprintf(stderr, "[native_fs] %s ioctl 0x%X unsupported natively -> %d\n",
                     KindName(handle->kind), request, result);
      return finish(result, is_async ? guest.gpr[9] : 0, is_async ? guest.gpr[10] : 0, is_async);
    }
    if (!handle->device)
    {
      if (request == ISFS_IOCTL_GETFILESTATS && out_ok && out_size >= 8)
      {
        const FS::Result<FS::FileStatus> status = file_system->GetFileStatus(handle->fs_fd);
        if (status)
        {
          memory.Write_U32(status->size, out);
          memory.Write_U32(status->offset, out + 4);
          result = 0;
        }
        else
        {
          result = Convert(status.error());
        }
      }
    }
    else if (in_ok && out_ok)
    {
      switch (request)
      {
      case ISFS_IOCTL_FORMAT:
        result = handle->uid != 0 ? Convert(ResultCode::AccessDenied) :
                                    Convert(file_system->Format(handle->uid));
        break;
      case ISFS_IOCTL_GETSTATS:
        if (out_size >= 28)
        {
          const FS::Result<FS::NandStats> stats = file_system->GetNandStats();
          if (stats)
          {
            const u32 values[] = {stats->cluster_size,      stats->free_clusters,
                                  stats->used_clusters,     stats->bad_clusters,
                                  stats->reserved_clusters, stats->free_inodes,
                                  stats->used_inodes};
            for (u32 i = 0; i < 7; ++i)
              memory.Write_U32(values[i], out + i * 4);
            result = 0;
          }
          else
          {
            result = Convert(stats.error());
          }
        }
        break;
      case ISFS_IOCTL_CREATEDIR:
        if (in_size >= kIsfsParamsSize)
          result = Convert(file_system->CreateDirectory(handle->uid, handle->gid, params_path(),
                                                        params_attribute(), params_modes()));
        break;
      case ISFS_IOCTL_SETATTR:
        if (in_size >= kIsfsParamsSize)
          result = Convert(file_system->SetMetadata(handle->uid, params_path(),
                                                    memory.Read_U32(in), memory.Read_U16(in + 4),
                                                    params_attribute(), params_modes()));
        break;
      case ISFS_IOCTL_GETATTR:
        if (in_size >= 64 && out_size >= kIsfsParamsSize)
        {
          const std::string path = ReadString(memory, in, 64);
          const FS::Result<FS::Metadata> metadata =
              file_system->GetMetadata(handle->uid, handle->gid, path);
          if (metadata)
          {
            u8* p = memory.GetPointerForRange(out, kIsfsParamsSize);
            std::memset(p, 0, kIsfsParamsSize);
            memory.Write_U32(metadata->uid, out);
            memory.Write_U16(metadata->gid, out + 4);
            memory.Write_U8(static_cast<u8>(metadata->modes.owner), out + 70);
            memory.Write_U8(static_cast<u8>(metadata->modes.group), out + 71);
            memory.Write_U8(static_cast<u8>(metadata->modes.other), out + 72);
            memory.Write_U8(metadata->attribute, out + 73);
            result = 0;
          }
          else
          {
            result = Convert(metadata.error());
          }
        }
        break;
      case ISFS_IOCTL_DELETE:
        if (in_size >= 64)
          result = Convert(file_system->Delete(handle->uid, handle->gid, ReadString(memory, in, 64)));
        break;
      case ISFS_IOCTL_RENAME:
        if (in_size >= 128)
          result = Convert(file_system->Rename(handle->uid, handle->gid, ReadString(memory, in, 64),
                                               ReadString(memory, in + 64, 64)));
        break;
      case ISFS_IOCTL_CREATEFILE:
        if (in_size >= kIsfsParamsSize)
          result = Convert(file_system->CreateFile(handle->uid, handle->gid, params_path(),
                                                   params_attribute(), params_modes()));
        break;
      case ISFS_IOCTL_SETFILEVERCTRL:
        if (in_size >= kIsfsParamsSize)
          result = 0;  // accepted and ignored, as by the IOS proxy
        break;
      case ISFS_IOCTL_SHUTDOWN:
        result = 0;
        break;
      default:
        break;
      }
    }
    return finish(result, is_async ? guest.gpr[9] : 0, is_async ? guest.gpr[10] : 0, is_async);
  }
  case kIosIoctlv:
  case kIosIoctlvAsync:
  {
    const bool is_async = guest.pc == kIosIoctlvAsync;
    FsState::Handle* handle = handle_of(guest.gpr[3]);
    if (!handle)
      return false;
    ++fs.ioctlvs;
    const u32 request = guest.gpr[4], in_count = guest.gpr[5], io_count = guest.gpr[6];
    const u32 vectors = guest.gpr[7];
    s32 result = Convert(ResultCode::Invalid);
    // Vectors: in_count inputs then io_count in/out, each {u32 address, u32 size}.
    const auto vec_addr = [&](u32 i) { return memory.Read_U32(vectors + i * 8); };
    const auto vec_size = [&](u32 i) { return memory.Read_U32(vectors + i * 8 + 4); };
    const bool vectors_ok = in_count + io_count <= 8 &&
                            ValidRange(memory, vectors, (in_count + io_count) * 8) &&
                            [&] {
                              for (u32 i = 0; i < in_count + io_count; ++i)
                                if (vec_size(i) && !ValidRange(memory, vec_addr(i), vec_size(i)))
                                  return false;
                              return true;
                            }();
    if (handle->kind != IpcDeviceKind::File && handle->kind != IpcDeviceKind::Fs)
    {
      ++fs.virtual_calls;
      // The running title: 00010000-<game id> for a disc game.
      const u64 title_id = (0x00010000ull << 32) | memory.Read_U32(0x80000000);
      if (handle->kind == IpcDeviceKind::Es && vectors_ok && request == 0x20 /* GETTITLEID */ &&
          in_count == 0 && io_count == 1 && vec_size(0) >= 8)
      {
        memory.Write_U64(title_id, vec_addr(0));
        result = 0;
      }
      else if (handle->kind == IpcDeviceKind::Es && vectors_ok && request == 0x1D /* GETTITLEDIR */ &&
               in_count == 1 && io_count == 1 && vec_size(0) >= 8)
      {
        const u64 asked = memory.Read_U64(vec_addr(0));
        char path[64];
        const int length = std::snprintf(path, sizeof(path), "/title/%08x/%08x/data",
                                         static_cast<u32>(asked >> 32), static_cast<u32>(asked));
        if (length > 0 && vec_size(1) >= static_cast<u32>(length))
        {
          memory.CopyToEmu(vec_addr(1), path, static_cast<size_t>(length));
          result = 0;
        }
      }
      else
      {
        ++fs.virtual_unknown;
        result = -4;
      }
      if (result < 0 && fs.virtual_unknown <= 8)
        std::fprintf(stderr, "[native_fs] %s ioctlv 0x%X unsupported natively -> %d\n",
                     KindName(handle->kind), request, result);
      return finish(result, is_async ? guest.gpr[8] : 0, is_async ? guest.gpr[9] : 0, is_async);
    }
    if (handle->device && vectors_ok && request == ISFS_IOCTLV_READDIR && in_count >= 1 &&
        in_count == io_count && in_count <= 2 && vec_size(0) == 64)
    {
      u32 list_address = 0, count_address = 0, max_count = 0;
      bool ok = true;
      if (in_count == 2)
      {
        ok = vec_size(1) == 4 && vec_size(3) == 4;
        if (ok)
        {
          max_count = memory.Read_U32(vec_addr(1));
          count_address = vec_addr(3);
          list_address = vec_addr(2);
          ok = vec_size(2) == 13 * max_count;
          if (ok)
            memory.Write_U32(max_count, count_address);
        }
      }
      else
      {
        ok = vec_size(1) == 4;
        if (ok)
        {
          max_count = memory.Read_U32(vec_addr(1));
          count_address = vec_addr(1);
        }
      }
      if (ok)
      {
        const std::string directory = ReadString(memory, vec_addr(0), 64);
        const FS::Result<std::vector<std::string>> list =
            file_system->ReadDirectory(handle->uid, handle->gid, directory);
        if (!list)
        {
          result = Convert(list.error());
        }
        else if (!list_address)
        {
          memory.Write_U32(static_cast<u32>(list->size()), count_address);
          result = 0;
        }
        else
        {
          for (size_t i = 0; i < list->size() && i < max_count; ++i)
          {
            std::memset(memory.GetPointerForRange(list_address, 13), 0, 13);
            const std::string& name = (*list)[i];
            memory.CopyToEmu(list_address, name.data(), std::min<size_t>(name.size(), 12));
            list_address += static_cast<u32>(name.size()) + 1;
          }
          memory.Write_U32(std::min(max_count, static_cast<u32>(list->size())), count_address);
          result = 0;
        }
      }
    }
    else if (handle->device && vectors_ok && request == ISFS_IOCTLV_GETUSAGE && in_count == 1 &&
             io_count == 2 && vec_size(0) == 64 && vec_size(1) == 4 && vec_size(2) == 4)
    {
      const FS::Result<FS::DirectoryStats> stats =
          file_system->GetDirectoryStats(ReadString(memory, vec_addr(0), 64));
      if (stats)
      {
        memory.Write_U32(stats->used_clusters, vec_addr(1));
        memory.Write_U32(stats->used_inodes, vec_addr(2));
        result = 0;
      }
      else
      {
        result = Convert(stats.error());
      }
    }
    return finish(result, is_async ? guest.gpr[8] : 0, is_async ? guest.gpr[9] : 0, is_async);
  }
  default:
    return false;
  }
}

void NativeSdk::FsDoState(StaticRecompState::StateStream& p, u32 version)
{
  FsState& fs = *m_fs;
  p.Do(fs.active);
  p.Do(fs.abandoned);
  p.Do(fs.next_fd);
  u32 count = static_cast<u32>(fs.handles.size());
  p.Do(count);
  if (p.IsReadMode())
    fs.handles.clear();
  auto it = fs.handles.begin();
  for (u32 i = 0; i < count; ++i)
  {
    u32 fd = 0;
    FsState::Handle handle;
    if (!p.IsReadMode())
    {
      fd = it->first;
      handle = it->second;
      ++it;
    }
    p.Do(fd);
    p.Do(handle.device);
    if (version >= 3)
      p.Do(handle.kind);
    else if (p.IsReadMode())
      handle.kind = handle.device ? IpcDeviceKind::Fs : IpcDeviceKind::File;
    p.Do(handle.fs_fd);
    p.Do(handle.uid);
    p.Do(handle.gid);
    p.Do(handle.path);
    if (p.IsReadMode())
      fs.handles[fd] = handle;
  }
  if (!m_nand)
    return;
  if (version >= 6)
  {
    for (u32 fd = 0; fd < StaticRecompNativeNand::NandStore::kMaxHandles; ++fd)
    {
      StaticRecompNativeNand::NandStore::HandleState state = m_nand->GetHandleState(fd);
      u8 mode = static_cast<u8>(state.mode);
      p.Do(state.opened);
      p.Do(state.path);
      p.Do(mode);
      p.Do(state.offset);
      if (p.IsReadMode())
      {
        state.mode = static_cast<StaticRecompNativeNand::Mode>(mode);
        if (!m_nand->RestoreHandle(fd, state) && state.opened)
          std::fprintf(stderr, "[native_fs] state handle %u (%s) could not be reopened\n", fd,
                       state.path.c_str());
      }
    }
  }
  else if (p.IsReadMode())
  {
    // A state from before the own store: its file descriptors index Dolphin's
    // IOS file system, which the HW chunk restored. Re-open each file through
    // the store at the same offset (mode unknown there: read/write).
    m_nand->CloseAll();
    for (auto& [fd, handle] : fs.handles)
    {
      if (handle.device)
        continue;
      // The chassis's store reopened the host file with write-deny sharing
      // when the HW chunk was restored; the hook closes it before the own
      // store opens the same file, or fopen("r+b") fails.
      u32 offset = 0;
      if (s_chassis.migrate_fs_handle)
        offset = s_chassis.migrate_fs_handle(handle.fs_fd).value_or(0);
      StaticRecompNativeNand::NandStore::HandleState state;
      state.opened = true;
      state.path = handle.path.data();
      state.mode = StaticRecompNativeNand::Mode::ReadWrite;
      state.offset = offset;
      u32 slot = 0;
      while (slot < StaticRecompNativeNand::NandStore::kMaxHandles && m_nand->GetHandleState(slot).opened)
        ++slot;
      if (slot < StaticRecompNativeNand::NandStore::kMaxHandles && m_nand->RestoreHandle(slot, state))
        handle.fs_fd = slot;
      else
        std::fprintf(stderr, "[native_fs] old state handle %u (%s) could not be migrated\n", fd,
                     handle.path.data());
    }
  }
}

void NativeSdk::PrintFsStats() const
{
  if (!m_fs)
    return;
  const FsState& fs = *m_fs;
  std::fprintf(stderr,
               "[native_fs] active=%d abandoned=%d opens=%llu opens_original=%llu closes=%llu "
               "reads=%llu writes=%llu seeks=%llu ioctls=%llu ioctlvs=%llu async=%llu "
               "read_bytes=%llu written_bytes=%llu errors=%llu open_handles=%zu\n",
               fs.active, fs.abandoned, static_cast<unsigned long long>(fs.opens),
               static_cast<unsigned long long>(fs.opens_original),
               static_cast<unsigned long long>(fs.closes), static_cast<unsigned long long>(fs.reads),
               static_cast<unsigned long long>(fs.writes), static_cast<unsigned long long>(fs.seeks),
               static_cast<unsigned long long>(fs.ioctls),
               static_cast<unsigned long long>(fs.ioctlvs), static_cast<unsigned long long>(fs.async),
               static_cast<unsigned long long>(fs.read_bytes),
               static_cast<unsigned long long>(fs.written_bytes),
               static_cast<unsigned long long>(fs.errors), fs.handles.size());
  std::fprintf(stderr,
               "[native_fs] virtual devices: opens=%llu calls=%llu pending=%llu unsupported=%llu\n",
               static_cast<unsigned long long>(fs.virtual_opens),
               static_cast<unsigned long long>(fs.virtual_calls),
               static_cast<unsigned long long>(fs.virtual_pending),
               static_cast<unsigned long long>(fs.virtual_unknown));
}
}  // namespace StaticRecompNativeSdk
