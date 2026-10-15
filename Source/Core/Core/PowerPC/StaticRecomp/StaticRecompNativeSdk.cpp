// RecompCore: StaticRecomp CPU core - native SDK functions.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeAudio.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeFs.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeMods.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePad.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeThp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdexcept>

#include "Common/ChunkFile.h"

#include "Common/Swap.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"

namespace StaticRecompNativeSdk
{
namespace
{
// RDSE70 (Budokai Tenkaichi 3, NTSC-U). Addresses come from the SDK code of
// that DOL; CodeMatches() checks the code still has the original bytes.
constexpr u32 kSda = 0x806370A0;  // r13
constexpr u32 kPadRead = 0x8021FAB8;
constexpr u32 kSpec2MakeStatus = 0x80220080;

// pad.a globals (r13-relative in the SDK code).
constexpr u32 kPendingBits = kSda - 25408;
constexpr u32 kCheckingBits = kSda - 25404;
constexpr u32 kResettingBits = kSda - 25392;
constexpr u32 kEnabledBits = kSda - 25388;
constexpr u32 kBarrelBits = kSda - 25412;
constexpr u32 kResettingChan = kSda - 32356;
constexpr u32 kAnalogMode = kSda - 32348;
constexpr u32 kMakeStatus = kSda - 32340;
constexpr u32 kSiIdleCount = kSda - 25488;  // cleared by __VIResetSIIdle
constexpr u32 kOrigin = 0x805658D0;         // PADStatus Origin[4]
constexpr u32 kPadType = 0x80565900;        // u32 Type[4]
constexpr u32 kPadPrevious = 0x80565920;    // PADStatus previous[4]
// si.a globals.
constexpr u32 kSiPacketChan = 0x805807D0;   // Packet[chan].chan, stride 32
constexpr u32 kSiChan = 0x80391170;         // Si.chan
constexpr u32 kSiPoll = 0x80391174;         // Si.poll
constexpr u32 kSiType = 0x80391188;         // u32 Type[4]
constexpr u32 kSiTypeTime = 0x80580870;     // OSTime TypeTime[4]
constexpr u32 kSiInputBuffer = 0x80580950;  // u32 InputBuffer[4][2]
constexpr u32 kSiInputValid = 0x80580970;   // BOOL InputBufferValid[4]

constexpr u32 kSiErrorNoResponse = 0x08;
constexpr u32 kSiNewData = 0x20;
constexpr u32 kSiTypeNoMotor = 0x20000000;
constexpr u16 kPadUseOrigin = 0x0080;
constexpr u16 kPadOriginRequest = 0x2000;
// Approximate cost of the replaced SDK path, charged to keep CoreTiming moving.
constexpr s64 kPadReadCycles = 600;

// kpad.a / wpad.a.
constexpr u32 kKpadRead = 0x80258F94;
constexpr u32 kKpadStates = 0x8058B2A0;      // per-channel KPAD state, stride 1316
constexpr u32 kKpadStateSize = 1316;
constexpr u32 kWpadControlBlocks = 0x8058C770;  // WPAD control block pointer per channel
constexpr u32 kWpadStatusOffset = 2236;        // s32 status (-1: no controller)
constexpr u32 kWpadExtensionCallbackOffset = 2216;
constexpr u32 kKpadExtensionCallback = 0x80259AD4;  // KPAD's WPAD extension callback
constexpr s64 kKpadReadCycles = 300;

// dvd.a: reads complete at once from the extracted files (like a ROM DMA in
// N64 recomp ports) instead of going through the emulated drive.
constexpr u32 kDvdReadAbsAsyncPrio = 0x80217678;
constexpr u32 kDvdGetCommandBlockStatus = 0x80217834;
constexpr u32 kDvdCbForReadAsync = 0x80213AF0;  // calls DVDFileInfo::callback if set
constexpr u32 kDvdCbForReadSync = 0x80213C30;   // wakes threads sleeping in DVDReadPrio
constexpr u32 kFstPointer = 0x80000038;         // OS globals: FST in memory
constexpr u32 kPpcBlr = 0x4E800020;
constexpr s64 kDvdReadCycles = 400;
// DVDCommandBlock / DVDFileInfo fields.
constexpr u32 kBlockCommand = 8, kBlockState = 12, kBlockOffset = 16, kBlockLength = 20,
              kBlockAddress = 24, kBlockTransferred = 32, kBlockCallback = 40,
              kFileInfoCallback = 56;

std::string s_game_root;

// Native interrupt line (see NativeSdk::PostCallback).
constexpr u32 kNativeInterruptHandler = 0x80246EC8;  // TRKNubMainLoop: debugger only
// Return address of native context calls: TRKGetNextEvent, only called by
// TRKNubMainLoop, which never runs either.
constexpr u32 kNativeCallReturn = 0x802470DC;
// SDK interrupt handler frame: OSContext at +8.
constexpr u32 kContextFrame = 0x2E0;
constexpr u32 kOsInterruptHandlerTable = kSda - 25992;  // pointer to the handler table
constexpr u32 kOsInterruptMask = 0x800000C4;
constexpr u32 kOsUserInterruptMask = 0x800000C8;
constexpr u32 kNativeInterrupt = 25;  // __OS_INTERRUPT_PI_DEBUG
constexpr u32 kNativeInterruptBit = 0x80000000u >> kNativeInterrupt;
constexpr u32 kViInterrupt = 24;  // __OS_INTERRUPT_PI_VI
constexpr u32 kIpcInterrupt = 27;  // __OS_INTERRUPT_PI_ACR (IPC), installed by IPCCltInit

// PADRead and every routine whose effect it reproduces (FNV-1a 64 of the
// original DOL bytes).
constexpr std::array<CodeFingerprint, 7> kPadReadCode = {{
    {0x8021FAB8u, 736u, 0xE29BD7CAD6F7242Dull},   // PADRead
    {0x80220080u, 1408u, 0x738BBC6D95C958DDull},  // SPEC2_MakeStatus
    {0x8021CC18u, 16u, 0xEDA382FD2139495Bull},    // __VIResetSIIdle
    {0x80245884u, 56u, 0x97723B6A5E0FC38Full},    // SIIsChanBusy
    {0x8024636Cu, 120u, 0xFAA4BFDE3C9F80D0ull},   // SIGetStatus
    {0x80246558u, 292u, 0x5043DAD0B5189B9Cull},   // SIGetResponse
    {0x80246B24u, 436u, 0x8C9B8D5CFE6CF893ull},   // SIGetType
}};
// FirstRun_Main (0x8019EF50) shows the wrist strap screen (0x8019E6F0, 1200
// frames) and the controller notice (0x8019EB2C, 600 frames) before the save
// check; neither result is used.
constexpr u32 kStrapScreen = 0x8019E6F0, kStrapScreenReturn = 0x8019F010;
constexpr u32 kNoticeScreen = 0x8019EB2C, kNoticeScreenReturn = 0x8019F034;
constexpr std::array<CodeFingerprint, 1> kFirstRunCode = {{
    {0x8019EF50u, 296u, 0x8DD6FF5950D0E6A4ull},  // FirstRun_Main
}};
constexpr std::array<CodeFingerprint, 3> kKpadReadCode = {{
    {0x80258F94u, 1816u, 0xC9D9811811C6C749ull},  // KPADRead
    {0x8025CF34u, 140u, 0x26A5987C1C4B5155ull},   // WPADProbe
    {0x8025CFC0u, 104u, 0xBF40E5F9C0F7C07Eull},   // WPADSetExtensionCallback
}};
constexpr std::array<CodeFingerprint, 4> kDvdReadCode = {{
    {0x80217678u, 228u, 0x3E6440DD66B859FCull},   // DVDReadAbsAsyncPrio
    {0x80213AF0u, 24u, 0x0D6079C9AA14B3F0ull},    // cbForReadAsync
    {0x80213C30u, 8u, 0x305F319A5DA69584ull},     // cbForReadSync
    {0x80217834u, 200u, 0x9A1401310802F6F8ull},   // DVDGetCommandBlockStatus
}};

u64 Fnv1a(const u8* data, u32 size)
{
  u64 hash = 0xcbf29ce484222325ull;
  for (u32 i = 0; i < size; ++i)
    hash = (hash ^ data[i]) * 0x100000001b3ull;
  return hash;
}

u32 Load32(const u8* p)
{
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}

void Store32(u8* p, u32 value)
{
  p[0] = u8(value >> 24);
  p[1] = u8(value >> 16);
  p[2] = u8(value >> 8);
  p[3] = u8(value);
}

// SPEC2_MakeStatus clamps: remove the origin, keeping the result in range.
u8 ClampS8(u8 value, u8 origin)
{
  s32 v = s8(value);
  const s32 org = s8(origin);
  if (org > 0)
    v = std::max(v, s32(s8(u8(origin - 128))));
  else if (org < 0)
    v = std::min(v, s32(s8(u8(origin + 127))));
  return u8(v - org);
}

u8 ClampU8(u8 value, u8 origin)
{
  return u8(std::max(value, origin) - origin);
}

bool Moved(s32 current, s32 previous)
{
  return std::abs(current * current - previous * previous) >= 9;
}
}  // namespace

void SetGameRoot(const std::string& path)
{
  s_game_root = path;
}

const std::string& GameRoot()
{
  return s_game_root;
}

NativeSdk::NativeSdk(Core::System& system) : m_system(system)
{
}

NativeSdk::~NativeSdk()
{
  if (m_trace_file)
    std::fclose(m_trace_file);
}

void NativeSdk::TraceCallEntry(const CPUState& guest)
{
  const u64 now = m_system.GetSystemTimers().GetFakeTimeBase();
  std::fprintf(m_trace_file, "call,%08x,%llu,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n", guest.pc,
               static_cast<unsigned long long>(now), guest.gpr[3], guest.gpr[4], guest.gpr[5],
               guest.gpr[6], guest.gpr[7], guest.gpr[8], guest.lr);
  // Calls that never return (or return elsewhere) are dropped after a while.
  if (m_trace_calls.size() >= 64)
    m_trace_calls.erase(m_trace_calls.begin());
  m_trace_calls.push_back({guest.pc, guest.lr, guest.gpr[1], now});
}

void NativeSdk::TraceReturns(const CPUState& guest)
{
  // A blocking call may return after other threads ran: match pc and stack.
  for (size_t i = 0; i < m_trace_calls.size(); ++i)
  {
    const TraceCall& call = m_trace_calls[i];
    if (guest.pc != call.lr || guest.gpr[1] != call.sp)
      continue;
    const u64 now = m_system.GetSystemTimers().GetFakeTimeBase();
    std::fprintf(m_trace_file, "return,%08x,%llu,%08x,%llu\n", call.pc,
                 static_cast<unsigned long long>(now), guest.gpr[3],
                 static_cast<unsigned long long>(now - call.timebase));
    m_trace_calls.erase(m_trace_calls.begin() + i);
    return;
  }
}

void NativeSdk::Bind(u32 count, const u32* addresses, u8* flags, int* any)
{
  const char* selection = std::getenv("MODERNGEKKO_NATIVE_SDK");
  const std::string names = selection ? selection : "all";
  if (names == "off" || names == "0")
  {
    std::fprintf(stderr, "[native_sdk] disabled (MODERNGEKKO_NATIVE_SDK=%s)\n", names.c_str());
    return;
  }
  m_verify = std::getenv("MODERNGEKKO_NATIVE_SDK_VERIFY") != nullptr;
  const auto wanted = [&](const char* name) {
    return names == "all" || ("," + names + ",").find(std::string(",") + name + ",") != std::string::npos;
  };
  const u32* end = addresses + count;
  const auto enable = [&](u32 address, const char* name) {
    const u32* it = std::lower_bound(addresses, end, address);
    if (it == end || *it != address || !wanted(name))
      return false;
    flags[it - addresses] = 1;
    *any = 1;
    m_min = std::min(m_min == 1 && m_max == 0 ? address : m_min, address);
    m_max = std::max(m_max, address);
    std::fprintf(stderr, "[native_sdk] %s at 0x%08X: native%s\n", name, address,
                 m_verify ? " (verify: original runs, results compared)" : "");
    return true;
  };
  m_pad_read = enable(kPadRead, "PADRead");
  m_kpad_read = enable(kKpadRead, "KPADRead");
  PadBind([&](u32 address) { return enable(address, "PAD"); }, wanted("PAD"));
  m_dvd_read = !s_game_root.empty() && enable(kDvdReadAbsAsyncPrio, "DVDRead");
  AudioBind([&](u32 address) { return enable(address, "Audio"); }, wanted("Audio"));
  // The native interrupt handler entry must exit to the dispatcher.
  if (std::getenv("MODERNGEKKO_NATIVE_INTERRUPTS_OFF") == nullptr)
  {
    const u32* it = std::lower_bound(addresses, end, kNativeInterruptHandler);
    if (it != end && *it == kNativeInterruptHandler)
    {
      flags[it - addresses] = 1;
      *any = 1;
      m_interrupt_hook = true;
      m_min = std::min(m_min == 1 && m_max == 0 ? kNativeInterruptHandler : m_min,
                       kNativeInterruptHandler);
      m_max = std::max(m_max, kNativeInterruptHandler);
      std::fprintf(stderr, "[native_sdk] native interrupts: OS interrupt %u, handler 0x%08X\n",
                   kNativeInterrupt, kNativeInterruptHandler);
      const u32* ret = std::lower_bound(addresses, end, kNativeCallReturn);
      if (ret != end && *ret == kNativeCallReturn)
      {
        flags[ret - addresses] = 1;
        m_context_call_hook = true;
        m_min = std::min(m_min, kNativeCallReturn);
        m_max = std::max(m_max, kNativeCallReturn);
      }
    }
  }
  FsBind([&](u32 address) { return enable(address, "NAND"); }, wanted("NAND"));
  m_mods_hooks = ModsBind([&](u32 address) { return enable(address, "Mods"); });
  ThpBind([&](u32 address) { return enable(address, "THP"); }, wanted("THP") && !m_verify);
  {
    const char* notices = std::getenv("MODERNGEKKO_WII_NOTICES");
    if (!m_verify && !(notices && std::string(notices) == "on") &&
        enable(kStrapScreen, "SkipNotices") && enable(kNoticeScreen, "SkipNotices"))
      m_skip_notices = true;
  }
  if (m_dvd_read && m_verify)
  {
    // The comparison runs when the game polls the block it read.
    const u32* it = std::lower_bound(addresses, end, kDvdGetCommandBlockStatus);
    if (it != end && *it == kDvdGetCommandBlockStatus)
    {
      flags[it - addresses] = 1;
      m_dvd_status_hook = true;
      m_max = std::max(m_max, kDvdGetCommandBlockStatus);
    }
  }

  const char* trace = std::getenv("MODERNGEKKO_NATIVE_SDK_TRACE");
  const char* trace_path = std::getenv("MODERNGEKKO_NATIVE_SDK_TRACE_FILE");
  if (trace && trace_path && (m_trace_file = std::fopen(trace_path, "w")))
  {
    std::fprintf(m_trace_file, "kind,pc,timebase,r3,r4,r5,r6,r7,r8,lr\n");
    for (const char* p = trace; *p;)
    {
      char* next;
      const u32 address = static_cast<u32>(std::strtoul(p, &next, 16));
      if (next == p)
        break;
      const u32* it = std::lower_bound(addresses, end, address);
      if (it != end && *it == address)
      {
        flags[it - addresses] = 1;
        *any = 1;
        m_trace.push_back(address);
        m_min = std::min(m_min == 1 && m_max == 0 ? address : m_min, address);
        m_max = std::max(m_max, address);
      }
      else
      {
        std::fprintf(stderr, "[native_sdk] trace: 0x%08X is not a native hook\n", address);
      }
      p = *next ? next + 1 : next;
    }
    std::sort(m_trace.begin(), m_trace.end());
    std::fprintf(stderr, "[native_sdk] tracing %zu functions to %s\n", m_trace.size(), trace_path);
  }
}

bool NativeSdk::RunSlow(CPUState& guest)
{
  m_last_guest = &guest;
  if (!m_deferred_callbacks.empty() && InstallNativeInterrupt(guest))
  {
    for (const NativeCallback& call : m_deferred_callbacks)
      PostCallback(guest, call);
    m_deferred_callbacks.clear();
  }
  if (guest.pc == kNativeCallReturn && m_context_call_hook)
    return ReturnFromContextCall(guest);
  if (m_trace_file && std::binary_search(m_trace.begin(), m_trace.end(), guest.pc))
    TraceCallEntry(guest);
  if (guest.pc == kPadRead && m_pad_read)
    return m_pad ? PadReadNoSi(guest) : PadRead(guest);
  if (m_pad && PadRun(guest))
    return true;
  if (guest.pc == kNativeInterruptHandler && m_interrupt_hook)
    return DeliverCallback(guest);
  if (guest.pc == kDvdReadAbsAsyncPrio && m_dvd_read)
    return DvdReadAbs(guest);
  if (guest.pc == kDvdGetCommandBlockStatus && m_dvd_status_hook)
  {
    CheckDvdReads(guest);
    return false;
  }
  if (guest.pc == kKpadRead && m_kpad_read)
    return KpadRead(guest);
  if (m_mods_hooks && ModsRun(guest))
    return true;
  if (m_skip_notices && SkipNotice(guest))
    return true;
  if (m_thp && ThpRun(guest))
    return true;
  if (m_fs && FsRun(guest))
    return true;
  if (m_audio)
    return AudioRun(guest);
  return false;
}

u8* NativeSdk::Pointer(const CPUState& guest, u32 address, u32 size) const
{
  // Data translation through the DBATs, as the guest sees it.
  if ((guest.msr & 0x10) == 0 || u64(address) + size > 0x100000000ull)
    return nullptr;
  constexpr u32 page_mask = PowerPC::BAT_PAGE_SIZE - 1;
  if ((address & page_mask) + size > PowerPC::BAT_PAGE_SIZE)
    return nullptr;
  const u32 bat = m_system.GetMMU().GetDBATTable()[address >> PowerPC::BAT_INDEX_SHIFT];
  constexpr u32 mapped = PowerPC::BAT_MAPPED_BIT | PowerPC::BAT_PHYSICAL_BIT;
  if ((bat & (mapped | PowerPC::BAT_WI_BIT)) != mapped)
    return nullptr;
  const u32 physical = (bat & PowerPC::BAT_RESULT_MASK) | (address & page_mask);
  if (guest.ram && u64(physical) + size <= guest.ram_size)
    return guest.ram + physical;
  if (physical >= 0x10000000 && guest.exram && u64(physical - 0x10000000) + size <= guest.exram_size)
    return guest.exram + physical - 0x10000000;
  return nullptr;
}

bool NativeSdk::Read32(const CPUState& guest, u32 address, u32& value) const
{
  const u8* p = Pointer(guest, address, 4);
  if (!p)
    return false;
  value = Load32(p);
  return true;
}

template <size_t N>
bool NativeSdk::CodeMatches(const CPUState& guest, const std::array<CodeFingerprint, N>& fingerprints) const
{
  for (const auto& code : fingerprints)
  {
    const u8* p = Pointer(guest, code.address, code.size);
    if (!p || Fnv1a(p, code.size) != code.fnv1a)
      return false;
  }
  return true;
}
// Fingerprint list sizes used by the other native modules.
template bool NativeSdk::CodeMatches(const CPUState&, const std::array<CodeFingerprint, 7>&) const;
template bool NativeSdk::CodeMatches(const CPUState&, const std::array<CodeFingerprint, 15>&) const;

bool NativeSdk::ThpVerifyPending(const CPUState& guest) const
{
  return m_thp->verify_lr != 0 && guest.pc == m_thp->verify_lr && guest.gpr[1] == m_thp->verify_sp;
}

bool NativeSdk::GetHostPadData(int chan, u32 analog_mode, u32& hi, u32& lo)
{
  // The PC controller mapped to this GameCube port (InputCommon), packed as
  // the controller's poll response so SPEC2_MakeStatus sees the same words.
  if (m_system.GetSerialInterface().GetDeviceType(chan) != SerialInterface::SIDEVICE_GC_CONTROLLER)
    return false;
  const GCPadStatus pad = Pad::GetStatus(chan);
  if (!pad.isConnected)
    return false;
  hi = pad.stickY | (u32(pad.stickX) << 8) | (u32(u16(pad.button | kPadUseOrigin)) << 16);
  switch ((analog_mode >> 8) & 7)
  {
  case 1:
    lo = (pad.analogB >> 4) | ((pad.analogA >> 4) << 4) | (u32(pad.triggerRight) << 8) |
         (u32(pad.triggerLeft) << 16) | (u32(pad.substickY >> 4) << 24) |
         (u32(pad.substickX >> 4) << 28);
    break;
  case 2:
    lo = pad.analogB | (u32(pad.analogA) << 8) | (u32(pad.triggerRight >> 4) << 16) |
         (u32(pad.triggerLeft >> 4) << 20) | (u32(pad.substickY >> 4) << 24) |
         (u32(pad.substickX >> 4) << 28);
    break;
  case 3:
    lo = pad.triggerRight | (u32(pad.triggerLeft) << 8) | (u32(pad.substickY) << 16) |
         (u32(pad.substickX) << 24);
    break;
  case 4:
    lo = pad.analogB | (u32(pad.analogA) << 8) | (u32(pad.substickY) << 16) |
         (u32(pad.substickX) << 24);
    break;
  default:  // 0, 5, 6, 7
    lo = (pad.analogB >> 4) | ((pad.analogA >> 4) << 4) | ((pad.triggerRight >> 4) << 8) |
         ((pad.triggerLeft >> 4) << 12) | (u32(pad.substickY) << 16) |
         (u32(pad.substickX) << 24);
    break;
  }
  return true;
}

bool NativeSdk::MakeStatusSpec2(u8* st, u32 hi, u32 lo, u32 analog_mode, u32 pad_type,
                                const u8* origin, u8* previous, u32 bit, u32& barrel_bits,
                                bool& reset_idle)
{
  // SPEC2_MakeStatus: PADStatus from the controller's poll response. Returns
  // whether the controller asks for its origin to be read again.
  const u16 button = u16((hi >> 16) & 0x3FFF);
  st[0] = u8(button >> 8);
  st[1] = u8(button);
  st[2] = u8(hi >> 8);
  st[3] = u8(hi);
  switch (analog_mode & 0x700)
  {
  case 0x100:
    st[4] = u8((lo >> 24) & 0xF0);
    st[5] = u8((lo >> 20) & 0xF0);
    st[6] = u8(lo >> 16);
    st[7] = u8(lo >> 8);
    st[8] = u8(lo & 0xF0);
    st[9] = u8((lo << 4) & 0xF0);
    break;
  case 0x200:
    st[4] = u8((lo >> 24) & 0xF0);
    st[5] = u8((lo >> 20) & 0xF0);
    st[6] = u8((lo >> 16) & 0xF0);
    st[7] = u8((lo >> 12) & 0xF0);
    st[8] = u8(lo >> 8);
    st[9] = u8(lo);
    break;
  case 0x300:
    st[4] = u8(lo >> 24);
    st[5] = u8(lo >> 16);
    st[6] = u8(lo >> 8);
    st[7] = u8(lo);
    st[8] = 0;
    st[9] = 0;
    break;
  case 0x400:
    st[4] = u8(lo >> 24);
    st[5] = u8(lo >> 16);
    st[6] = 0;
    st[7] = 0;
    st[8] = u8(lo >> 8);
    st[9] = u8(lo);
    break;
  default:  // 0x000, 0x500, 0x600, 0x700
    st[4] = u8(lo >> 24);
    st[5] = u8(lo >> 16);
    st[6] = u8((lo >> 8) & 0xF0);
    st[7] = u8((lo >> 4) & 0xF0);
    st[8] = u8(lo & 0xF0);
    st[9] = u8((lo << 4) & 0xF0);
    break;
  }
  for (int i = 2; i < 6; ++i)
    st[i] = u8(st[i] - 128);
  if ((pad_type & 0xFFFF0000u) == 0x09000000u && !(button & kPadUseOrigin))
  {
    // Standard controller that has not sent its origin yet: centred sticks.
    barrel_bits |= bit;
    std::memset(st + 2, 0, 4);
  }
  else
  {
    barrel_bits &= ~bit;
    for (int i = 2; i < 6; ++i)
      st[i] = ClampS8(st[i], origin[i]);
    st[6] = ClampU8(st[6], origin[6]);
    st[7] = ClampU8(st[7], origin[7]);
    // Any input change restarts the screen-dimming idle count.
    u8* prev = previous;
    bool moved = false;
    for (int i = 2; i < 6; ++i)
      moved = moved || Moved(s8(st[i]), s8(prev[i]));
    moved = moved || Moved(st[6], prev[6]) || Moved(st[7], prev[7]);
    if (moved || st[0] != prev[0] || st[1] != prev[1])
      reset_idle = true;
    std::memcpy(prev, st, 12);
  }
  return (button & kPadOriginRequest) != 0;
}

bool NativeSdk::ComputePadRead(const CPUState& guest, bool from_si_latch, PadResult& out)
{
  if (guest.gpr[13] != kSda || !CodeMatches(guest, kPadReadCode))
    return false;
  u32 pending, resetting, resetting_chan, enabled, analog_mode, make_status, si_chan, si_poll;
  if (!Read32(guest, kPendingBits, pending) || !Read32(guest, kResettingBits, resetting) ||
      !Read32(guest, kResettingChan, resetting_chan) || !Read32(guest, kEnabledBits, enabled) ||
      !Read32(guest, kAnalogMode, analog_mode) || !Read32(guest, kMakeStatus, make_status) ||
      !Read32(guest, kBarrelBits, out.barrel_bits) || !Read32(guest, kSiChan, si_chan) ||
      !Read32(guest, kSiPoll, si_poll))
    return false;
  // A game-installed MakeStatus would need its own implementation.
  if (make_status != kSpec2MakeStatus)
    return false;
  const u8* status = Pointer(guest, guest.gpr[3], 48);
  const u8* previous = Pointer(guest, kPadPrevious, 48);
  const u8* origins = Pointer(guest, kOrigin, 48);
  const u8* pad_types = Pointer(guest, kPadType, 16);
  if (!status || !previous || !origins || !pad_types)
    return false;
  std::memcpy(out.status.data(), status, 48);
  std::memcpy(out.previous.data(), previous, 48);
  const u32 si_status = m_system.GetSerialInterface().PeekStatus();

  for (int chan = 0; chan < 4; ++chan)
  {
    const u32 bit = 0x80000000u >> chan;
    u8* st = &out.status[12 * chan];
    const auto fail = [&](s8 error) {
      std::memset(st, 0, 10);
      st[10] = u8(error);
    };
    // A pending reset would call PADReset: not modelled.
    if (pending & bit)
      return false;
    if ((resetting & bit) || resetting_chan == u32(chan))
    {
      fail(-2);  // PAD_ERR_NOT_READY
      continue;
    }
    if (!(enabled & bit))
    {
      fail(-1);  // PAD_ERR_NO_CONTROLLER
      continue;
    }
    u32 packet_chan;
    if (!Read32(guest, kSiPacketChan + 32 * chan, packet_chan))
      return false;
    if (packet_chan != 0xFFFFFFFFu || si_chan == u32(chan))
    {
      fail(-3);  // SIIsChanBusy: PAD_ERR_TRANSFER
      continue;
    }
    const u32 sr = si_status >> (8 * (3 - chan));
    // No response (controller gone) disables the channel: not modelled.
    if (from_si_latch && (sr & kSiErrorNoResponse))
      return false;
    // SIGetType on a polled channel with a known type only refreshes TypeTime.
    u32 type;
    if (!(si_poll & (0x80u >> chan)) || !Read32(guest, kSiType + 4 * chan, type) || type == 8)
      return false;
    out.type_read[chan] = true;
    if (!(type & kSiTypeNoMotor))
      out.motor |= bit;

    u32 hi, lo;
    bool have_data = true;
    if (!from_si_latch)
    {
      if (!GetHostPadData(chan, analog_mode, hi, lo))
        return false;
    }
    else if (sr & kSiNewData)
    {
      auto& si = m_system.GetSerialInterface();
      hi = si.PeekInHi(chan);
      lo = si.PeekInLo(chan);
    }
    else
    {
      u32 valid;
      if (!Read32(guest, kSiInputBuffer + 8 * chan, hi) ||
          !Read32(guest, kSiInputBuffer + 8 * chan + 4, lo) ||
          !Read32(guest, kSiInputValid + 4 * chan, valid))
        return false;
      have_data = valid != 0;
    }
    out.data_read[chan] = true;
    out.input_buffer_hi[chan] = hi;
    out.input_buffer_lo[chan] = lo;
    if (!have_data || (hi & 0x80000000u))
    {
      fail(-3);
      continue;
    }

    // An origin request starts an SI origin read: not modelled.
    if (MakeStatusSpec2(st, hi, lo, analog_mode, Load32(pad_types + 4 * chan), origins + 12 * chan,
                        &out.previous[12 * chan], bit, out.barrel_bits, out.reset_idle))
      return false;
    st[10] = 0;  // PAD_ERR_NONE
    st[1] = u8(st[1] & ~kPadUseOrigin);
  }
  return true;
}

void NativeSdk::Verify(const CPUState& guest, Function function, bool computed)
{
  // Verify mode: the original runs; FinishVerify compares at its return.
  if (!computed)
  {
    m_pending.function = Function::None;
    ++StatsFor(function).verify_skipped;
    return;
  }
  m_pending.function = function;
  m_pending.lr = guest.lr;
  m_pending.sp = guest.gpr[1];
  m_pending.status = guest.gpr[3];
  m_pending.chan = guest.gpr[3];
  if (!Read32(guest, kSiIdleCount, m_pending.idle_before))
    m_pending.idle_before = 0;
}

bool NativeSdk::PadRead(CPUState& guest)
{
  if (m_verify)
  {
    Verify(guest, Function::PadRead, ComputePadRead(guest, true, m_pending.pad));
    return false;
  }

  PadResult result;
  if (!ComputePadRead(guest, false, result))
  {
    ++m_pad_stats.deferred;
    return false;
  }
  u8* status = Pointer(guest, guest.gpr[3], 48);
  u8* previous = Pointer(guest, kPadPrevious, 48);
  u8* barrel = Pointer(guest, kBarrelBits, 4);
  u8* idle = Pointer(guest, kSiIdleCount, 4);
  if (!status || !previous || !barrel || !idle)
  {
    ++m_pad_stats.deferred;
    return false;
  }
  std::memcpy(status, result.status.data(), 48);
  std::memcpy(previous, result.previous.data(), 48);
  Store32(barrel, result.barrel_bits);
  if (result.reset_idle)
    Store32(idle, 0);
  const u64 now = m_system.GetSystemTimers().GetFakeTimeBase();
  for (int chan = 0; chan < 4; ++chan)
  {
    if (result.type_read[chan])
    {
      if (u8* time = Pointer(guest, kSiTypeTime + 8 * chan, 8))
      {
        Store32(time, u32(now >> 32));
        Store32(time + 4, u32(now));
      }
    }
    if (result.data_read[chan])
    {
      u8* buffer = Pointer(guest, kSiInputBuffer + 8 * chan, 8);
      u8* valid = Pointer(guest, kSiInputValid + 4 * chan, 4);
      if (buffer && valid)
      {
        Store32(buffer, result.input_buffer_hi[chan]);
        Store32(buffer + 4, result.input_buffer_lo[chan]);
        Store32(valid, 0);
      }
    }
  }
  guest.gpr[3] = result.motor;
  guest.pc = guest.lr;
  guest.downcount -= kPadReadCycles;
  ++m_pad_stats.native;
  return true;
}

bool NativeSdk::ComputeKpadRead(const CPUState& guest, KpadResult& out)
{
  const u32 chan = guest.gpr[3];
  if (guest.gpr[13] != kSda || chan > 3 || !CodeMatches(guest, kKpadReadCode))
    return false;
  const u8* kpad = Pointer(guest, kKpadStates + kKpadStateSize * chan, kKpadStateSize);
  if (!kpad)
    return false;
  // Re-entered while a read is in progress: returns 0 and touches nothing.
  if (kpad[1308] != 0)
  {
    out.busy = true;
    return true;
  }
  u32 status;
  if (!Read32(guest, kWpadControlBlocks + 4 * chan, out.control_block) ||
      !Read32(guest, out.control_block + kWpadStatusOffset, status) ||
      !Pointer(guest, out.control_block + kWpadExtensionCallbackOffset, 4))
    return false;
  const auto unmodelled = [&](const char* why) {
    static bool reported[4];
    if (!reported[chan])
    {
      reported[chan] = true;
      std::fprintf(stderr, "[native_sdk] KPADRead chan %u left to the original: %s (status %08x)\n",
                   chan, why, status);
    }
    return false;
  };
  // WPADProbe != WPAD_ERR_NO_CONTROLLER: a Wii Remote is connected.
  if (status != 0xFFFFFFFFu)
    return unmodelled("Wii Remote connected");
  // Pending disconnect callback, KPAD reset request, or queued samples.
  if (Load32(kpad + 1240) != 0 && kpad[1314] != 0 && kpad[1315] == 0)
    return unmodelled("disconnect callback pending");
  if (kpad[1309] != 0)
    return unmodelled("KPAD reset pending");
  if (kpad[271] != 0 && guest.gpr[4] != 0 && guest.gpr[5] != 0)
    return unmodelled("samples queued");
  return true;
}

bool NativeSdk::KpadRead(CPUState& guest)
{
  if (m_verify)
  {
    Verify(guest, Function::KpadRead, ComputeKpadRead(guest, m_pending.kpad = KpadResult{}));
    return false;
  }
  KpadResult result;
  if (!ComputeKpadRead(guest, result))
  {
    ++m_kpad_stats.deferred;
    return false;
  }
  if (!result.busy)
  {
    // WPADSetExtensionCallback(chan, KPAD's callback): the only lasting effect.
    Store32(Pointer(guest, result.control_block + kWpadExtensionCallbackOffset, 4),
            kKpadExtensionCallback);
  }
  guest.gpr[3] = 0;  // no samples
  guest.pc = guest.lr;
  guest.downcount -= kKpadReadCycles;
  ++m_kpad_stats.native;
  return true;
}

bool NativeSdk::KpadMatches(const CPUState& guest)
{
  const KpadResult& expected = m_pending.kpad;
  if (guest.gpr[3] != 0)
    return false;
  if (expected.busy)
    return true;
  const u8* kpad = Pointer(guest, kKpadStates + kKpadStateSize * m_pending.chan, kKpadStateSize);
  u32 callback = 0;
  return kpad && kpad[1308] == 0 &&
         Read32(guest, expected.control_block + kWpadExtensionCallbackOffset, callback) &&
         callback == kKpadExtensionCallback;
}

bool NativeSdk::PadMatches(const CPUState& guest)
{
  const auto& expected = m_pending.pad;
  const u8* status = Pointer(guest, m_pending.status, 48);
  const u8* previous = Pointer(guest, kPadPrevious, 48);
  u32 barrel = 0, idle = 0;
  const bool readable = status && previous && Read32(guest, kBarrelBits, barrel) &&
                        Read32(guest, kSiIdleCount, idle);
  const bool same = readable && std::memcmp(status, expected.status.data(), 48) == 0 &&
                    std::memcmp(previous, expected.previous.data(), 48) == 0 &&
                    barrel == expected.barrel_bits && guest.gpr[3] == expected.motor &&
                    // PADRead re-enables interrupts before it returns, so a
                    // pending VI retrace may already have counted one field.
                    (expected.reset_idle ? idle <= 2 : idle >= m_pending.idle_before);
  if (same || m_pad_stats.mismatches >= 8)
    return same;
  std::fprintf(stderr,
               "[native_sdk] PADRead mismatch: motor %08x/%08x barrel %08x/%08x status_same=%d "
               "previous_same=%d idle=%08x reset_expected=%d\n",
               guest.gpr[3], expected.motor, barrel, expected.barrel_bits,
               readable && std::memcmp(status, expected.status.data(), 48) == 0,
               readable && std::memcmp(previous, expected.previous.data(), 48) == 0, idle,
               expected.reset_idle);
  for (int i = 0; readable && i < 48; i += 12)
  {
    std::fprintf(stderr, "[native_sdk]   chan %d guest", i / 12);
    for (int j = 0; j < 12; ++j)
      std::fprintf(stderr, " %02x", status[i + j]);
    std::fprintf(stderr, " | native");
    for (int j = 0; j < 12; ++j)
      std::fprintf(stderr, " %02x", expected.status[i + j]);
    std::fprintf(stderr, "\n");
  }
  return false;
}

void NativeSdk::FinishVerify(const CPUState& guest)
{
  const Function function = m_pending.function;
  m_pending.function = Function::None;
  const bool same = function == Function::PadRead ? PadMatches(guest) : KpadMatches(guest);
  Stats& stats = StatsFor(function);
  if (same)
  {
    ++stats.verified;
    return;
  }
  if (++stats.mismatches <= 8 && function == Function::KpadRead)
    std::fprintf(stderr, "[native_sdk] KPADRead mismatch: returned %u\n", guest.gpr[3]);
}

bool NativeSdk::LoadDiscFiles(const CPUState& guest)
{
  // The FST the game uses (Dolphin lays out an extracted folder its own way,
  // so sys/fst.bin offsets do not apply): offsets are in 4-byte units.
  u32 fst;
  if (!Read32(guest, kFstPointer, fst) || fst == 0)
    return false;
  if (fst == m_fst_address && !m_disc_files.empty())
    return true;
  const u8* root = Pointer(guest, fst, 12);
  if (!root)
    return false;
  const u32 count = Load32(root + 8);
  if (count == 0 || count > 65536)
    return false;
  const u8* table = Pointer(guest, fst, count * 12);
  if (!table)
    return false;
  const u32 names = fst + count * 12;
  const auto name_of = [&](u32 entry, std::string& out) {
    const u32 offset = Load32(table + entry * 12) & 0x00FFFFFF;
    out.clear();
    for (u32 i = 0; i < 256; ++i)
    {
      const u8* c = Pointer(guest, names + offset + i, 1);
      if (!c)
        return false;
      if (*c == 0)
        return true;
      out.push_back(static_cast<char>(*c));
    }
    return false;
  };
  std::vector<DiscFile> files;
  struct Dir
  {
    u32 end;
    std::string prefix;
  };
  std::vector<Dir> dirs{{count, ""}};
  std::string name;
  for (u32 i = 1; i < count; ++i)
  {
    while (dirs.size() > 1 && i >= dirs.back().end)
      dirs.pop_back();
    const u8* entry = table + i * 12;
    if (!name_of(i, name))
      return false;
    if (entry[0] & 1)
      dirs.push_back({Load32(entry + 8), dirs.back().prefix + name + "/"});
    else
      files.push_back({u64(Load32(entry + 4)) * 4, Load32(entry + 8), dirs.back().prefix + name});
  }
  std::sort(files.begin(), files.end(),
            [](const DiscFile& a, const DiscFile& b) { return a.offset < b.offset; });
  m_disc_files = std::move(files);
  m_fst_address = fst;
  std::fprintf(stderr, "[native_sdk] DVDRead: %zu files from the game's FST at 0x%08X\n",
               m_disc_files.size(), fst);
  return true;
}

bool NativeSdk::ReadDisc(u64 offset, u32 length, std::vector<u8>& out)
{
  if (ModsRead(offset, length, out))
    return true;
  const auto it = std::upper_bound(
      m_disc_files.begin(), m_disc_files.end(), offset,
      [](u64 value, const DiscFile& file) { return value < file.offset; });
  if (it == m_disc_files.begin())
    return false;
  const DiscFile& file = *(it - 1);
  const u64 start = offset - file.offset;
  // MODERNGEKKO_NATIVE_DVD_LOG=1: which file and range each native read takes.
  static const bool log_reads = std::getenv("MODERNGEKKO_NATIVE_DVD_LOG") != nullptr;
  if (log_reads)
    std::fprintf(stderr, "[dvd_read] %s +0x%llx len 0x%x\n", file.path.c_str(),
                 static_cast<unsigned long long>(start), length);
  // Reads are rounded up to 32 bytes; the layout pads files with zeros.
  if (start + length > u64(file.size) + 32)
    return false;
  out.assign(length, 0);
  const u32 available = start < file.size ? static_cast<u32>(std::min<u64>(length, file.size - start)) : 0;
  if (available == 0)
    return true;
  const std::string path = s_game_root + "/files/" + file.path;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  const bool ok = _fseeki64(f, static_cast<long long>(start), SEEK_SET) == 0 &&
                  std::fread(out.data(), 1, available, f) == available;
  std::fclose(f);
  if (ok)
    ModsPatchRead(offset, out);
  return ok;
}

bool NativeSdk::CopyToGuest(const CPUState& guest, u32 address, const u8* data, u32 length) const
{
  // DMA-like copy: page by page through the DBATs (cached or uncached views).
  constexpr u32 page_mask = PowerPC::BAT_PAGE_SIZE - 1;
  constexpr u32 mapped = PowerPC::BAT_MAPPED_BIT | PowerPC::BAT_PHYSICAL_BIT;
  if ((guest.msr & 0x10) == 0 || u64(address) + length > 0x100000000ull)
    return false;
  std::vector<std::pair<u8*, u32>> pieces;
  for (u32 done = 0; done < length;)
  {
    const u32 ea = address + done;
    const u32 chunk = std::min(length - done, PowerPC::BAT_PAGE_SIZE - (ea & page_mask));
    const u32 bat = m_system.GetMMU().GetDBATTable()[ea >> PowerPC::BAT_INDEX_SHIFT];
    if ((bat & mapped) != mapped)
      return false;
    const u32 physical = (bat & PowerPC::BAT_RESULT_MASK) | (ea & page_mask);
    u8* host = nullptr;
    if (guest.ram && u64(physical) + chunk <= guest.ram_size)
      host = guest.ram + physical;
    else if (physical >= 0x10000000 && guest.exram &&
             u64(physical - 0x10000000) + chunk <= guest.exram_size)
      host = guest.exram + physical - 0x10000000;
    if (!host)
      return false;
    pieces.emplace_back(host, chunk);
    done += chunk;
  }
  u32 done = 0;
  for (const auto& [host, chunk] : pieces)
  {
    std::memcpy(host, data + done, chunk);
    done += chunk;
  }
  return true;
}

bool NativeSdk::DvdReadAbs(CPUState& guest)
{
  // DVDReadAbsAsyncPrio(block, addr, length, offset >> 2, callback, prio).
  const u32 block = guest.gpr[3], address = guest.gpr[4], length = guest.gpr[5];
  const u32 offset_words = guest.gpr[6], callback = guest.gpr[7];
  const auto defer = [&] {
    ++m_dvd_stats.deferred;
    return false;
  };
  if (guest.gpr[13] != kSda || !CodeMatches(guest, kDvdReadCode) || length == 0 ||
      length > 0x4000000 || !Pointer(guest, block, 64))
    return defer();
  // With native interrupts the read completes in an interrupt, like the drive's
  // (state, size and callback together). Before the OS handles interrupts it
  // completes at once, which must not skip a callback with an effect.
  const bool interrupt = InstallNativeInterrupt(guest);
  if (interrupt || callback == 0)
  {
  }
  else if (callback == kDvdCbForReadAsync)
  {
    u32 user_callback, first = kPpcBlr;
    if (!Read32(guest, block + kFileInfoCallback, user_callback) ||
        (user_callback != 0 && (!Read32(guest, user_callback, first) || first != kPpcBlr)))
      return defer();
  }
  else if (callback != kDvdCbForReadSync)
  {
    return defer();
  }
  std::vector<u8> data;
  if (LoadDiscFiles(guest))
    ModsInit(guest);
  if (!LoadDiscFiles(guest) || !ReadDisc(u64(offset_words) * 4, length, data))
    return defer();
  if (m_verify)
  {
    // The original reads; CheckDvdReads compares when the block completes.
    if (m_dvd_checks.size() < 64)
      m_dvd_checks.push_back({block, address, std::move(data)});
    else
      ++m_dvd_stats.verify_skipped;
    return false;
  }
  if (!CopyToGuest(guest, address, data.data(), length))
    return defer();
  u8* b = Pointer(guest, block, 64);
  Store32(b + kBlockCommand, 1);  // READ
  Store32(b + kBlockAddress, address);
  Store32(b + kBlockLength, length);
  Store32(b + kBlockOffset, offset_words);
  Store32(b + kBlockCallback, callback);
  m_dvd_bytes += length;
  if (interrupt)
  {
    // Waiting until the interrupt: DVDCancel on it dequeues (no-op with null
    // links), marks it cancelled and runs the callbacks itself.
    Store32(b + 0, 0);  // next
    Store32(b + 4, 0);  // prev
    Store32(b + kBlockTransferred, 0);
    Store32(b + kBlockState, 2);  // DVD_STATE_WAITING
    NativeCallback call{callback, length, block};  // callback(result, block)
    call.dvd_block = block;
    call.dvd_length = length;
    PostCallback(guest, call);
  }
  else
  {
    Store32(b + kBlockTransferred, length);
    Store32(b + kBlockState, 0);  // DVD_STATE_END
  }
  guest.gpr[3] = 1;  // TRUE: request accepted
  guest.pc = guest.lr;
  guest.downcount -= kDvdReadCycles;
  ++m_dvd_stats.native;
  return true;
}

void NativeSdk::CheckDvdReads(const CPUState& guest)
{
  for (size_t i = 0; i < m_dvd_checks.size();)
  {
    const DvdCheck& check = m_dvd_checks[i];
    u32 state, transferred;
    if (!Read32(guest, check.block + kBlockState, state) ||
        !Read32(guest, check.block + kBlockTransferred, transferred))
    {
      m_dvd_checks.erase(m_dvd_checks.begin() + i);
      continue;
    }
    if (state != 0)
    {
      // Still busy; a cancelled or failed read is not compared.
      if (state == 0xFFFFFFFFu || state == 10)
      {
        ++m_dvd_stats.verify_skipped;
        m_dvd_checks.erase(m_dvd_checks.begin() + i);
        continue;
      }
      ++i;
      continue;
    }
    // Compare page by page against guest memory.
    bool same = transferred == check.data.size();
    std::vector<u8> copy(check.data.size());
    u32 first_difference = 0xFFFFFFFFu;
    for (u32 done = 0; same && done < check.data.size();)
    {
      const u32 ea = check.address + done;
      const u32 chunk = std::min<u32>(static_cast<u32>(check.data.size()) - done,
                                      PowerPC::BAT_PAGE_SIZE - (ea & (PowerPC::BAT_PAGE_SIZE - 1)));
      const u8* p = Pointer(guest, ea, chunk);
      if (!p)
      {
        // Uncached view: compare through the cached mapping of the same page.
        p = Pointer(guest, (ea & 0x3FFFFFFF) | 0x80000000, chunk);
      }
      if (!p)
      {
        same = false;
        break;
      }
      for (u32 j = 0; j < chunk; ++j)
      {
        if (p[j] != check.data[done + j])
        {
          same = false;
          first_difference = done + j;
          break;
        }
      }
      done += chunk;
    }
    if (same)
    {
      ++m_dvd_stats.verified;
    }
    else if (++m_dvd_stats.mismatches <= 8)
    {
      std::fprintf(stderr,
                   "[native_sdk] DVDRead mismatch: block %08x addr %08x length %zu transferred %u "
                   "first difference at %d\n",
                   check.block, check.address, check.data.size(), transferred,
                   static_cast<int>(first_difference));
    }
    m_dvd_checks.erase(m_dvd_checks.begin() + i);
  }
}

bool NativeSdk::InstallNativeInterrupt(const CPUState& guest)
{
  // Handler table entry for the native interrupt, and the OS masks. The game
  // never uses this line, so the entry is free; the OS keeps the PI mask in
  // step with these masks when it recomputes it.
  u32 table;
  if (!m_interrupt_hook || guest.gpr[13] != kSda || !Read32(guest, kOsInterruptHandlerTable, table))
    return false;
  u8* entry = Pointer(guest, table + 4 * kNativeInterrupt, 4);
  u8* os_mask = Pointer(guest, kOsInterruptMask, 4);
  u8* user_mask = Pointer(guest, kOsUserInterruptMask, 4);
  // Only once the OS handles interrupts: its external interrupt vector is in
  // place and an SDK driver installed its handler (the table holds leftovers
  // from the loader before that): VI, or IPC, which is installed before any
  // IOS call (SYSCONF is read before VIInit). On Wii the disc drive belongs
  // to IOS, so there is no DI handler to look for.
  const u8* vector = Pointer(guest, 0x80000500, 16);
  const u8* vi_entry = Pointer(guest, table + 4 * kViInterrupt, 4);
  const u8* ipc_entry = Pointer(guest, table + 4 * kIpcInterrupt, 4);
  const auto sdk_code = [](u32 address) { return address >= 0x80200000 && address < 0x802A0000; };
  const bool ready = entry && os_mask && user_mask && vector && vi_entry &&
                     Load32(vector) == 0x7C9043A6 && Load32(vector + 4) == 0x808000C0 &&
                     Load32(vector + 8) == 0x9064000C && Load32(vector + 12) == 0x7C7042A6 &&
                     (sdk_code(Load32(vi_entry)) || (ipc_entry && sdk_code(Load32(ipc_entry))));
  if (!ready)
  {
    if (!m_interrupt_not_ready_reported && vector && vi_entry)
    {
      m_interrupt_not_ready_reported = true;
      std::fprintf(stderr, "[native_sdk] native interrupts not ready yet: vector %08x vi handler %08x\n",
                   Load32(vector), Load32(vi_entry));
    }
    return false;
  }
  const u32 handler = Load32(entry);
  if (handler != kNativeInterruptHandler)
  {
    if (handler != 0)
      return false;  // the game installed its own handler: leave it alone
    Store32(entry, kNativeInterruptHandler);
  }
  Store32(os_mask, Load32(os_mask) & ~kNativeInterruptBit);
  Store32(user_mask, Load32(user_mask) & ~kNativeInterruptBit);
  auto& pi = m_system.GetProcessorInterface();
  pi.m_interrupt_mask |= ProcessorInterface::INT_CAUSE_DEBUG;
  return true;
}

bool NativeSdk::PostCallback(const CPUState& guest, const NativeCallback& call)
{
  if (!InstallNativeInterrupt(guest))
    return false;
  m_callbacks.push_back(call);
  ++m_callbacks_posted;
  // Taken at the next interrupt check with MSR.EE set.
  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_DEBUG, true);
  return true;
}

bool NativeSdk::DeliverCallback(CPUState& guest)
{
  // Entered as the OS interrupt handler: handler(interrupt, context), with
  // lr pointing back into the OS dispatcher.
  auto& pi = m_system.GetProcessorInterface();
  if (m_callbacks.empty())
  {
    pi.SetInterrupt(ProcessorInterface::INT_CAUSE_DEBUG, false);
    if (guest.gpr[3] != kNativeInterrupt)
    {
      // Not our interrupt: a real call of the debugger function.
      return false;
    }
    ++m_spurious_interrupts;
    guest.pc = guest.lr;
    return true;
  }
  const NativeCallback call = m_callbacks.front();
  m_callbacks.erase(m_callbacks.begin());
  if (m_callbacks.empty())
    pi.SetInterrupt(ProcessorInterface::INT_CAUSE_DEBUG, false);
  ++m_callbacks_delivered;
  if (call.kind == CallbackKind::ContextCall)
  {
    if (!EnterContextCall(guest, call))
      guest.pc = guest.lr;
    return true;
  }
  if (call.kind != CallbackKind::Call)
  {
    // A native audio interrupt: the SDK's own handler takes it from here.
    if (!AudioEnterHandler(guest, call.kind))
      guest.pc = guest.lr;
    return true;
  }
  if (call.dvd_block)
  {
    // The drive finishing: state and size are set in the interrupt, right
    // before the callback, as the hardware does. A read cancelled meanwhile
    // (DVDCancel dequeued it and ran the callbacks) is dropped.
    u8* b = Pointer(guest, call.dvd_block, 64);
    if (!b || Load32(b + kBlockState) != 2)
    {
      ++m_dvd_cancelled;
      guest.pc = guest.lr;
      return true;
    }
    Store32(b + kBlockTransferred, call.dvd_length);
    Store32(b + kBlockState, 0);  // DVD_STATE_END
    if (call.function == 0)
    {
      guest.pc = guest.lr;
      return true;
    }
  }
  guest.gpr[3] = call.r3;
  guest.gpr[4] = call.r4;
  guest.pc = call.function;  // returns to the OS dispatcher through lr
  return true;
}

void NativeSdk::PostOrDefer(const NativeCallback& call)
{
  if (m_last_guest && PostCallback(*m_last_guest, call))
    return;
  m_deferred_callbacks.push_back(call);
}

void NativeSdk::ClearGuestContext(const CPUState& guest, u32 context)
{
  // OSClearContext: mode = state = 0; drop it as the FPU owner.
  if (u8* p = Pointer(guest, context + 0x1A0, 4))
    std::memset(p, 0, 4);
  u8* owner = Pointer(guest, 0x800000D8, 4);
  if (owner && Load32(owner) == context)
    Store32(owner, 0);
}

void NativeSdk::SetGuestContext(CPUState& guest, u32 context)
{
  // OSSetCurrentContext: current context (cached and physical), and MSR.FP
  // on only if this context owns the FPU (lazy FP save/restore).
  if (u8* p = Pointer(guest, 0x800000D4, 4))
    Store32(p, context);
  if (u8* p = Pointer(guest, 0x800000C0, 4))
    Store32(p, context & 0x3FFFFFFF);
  const u8* owner = Pointer(guest, 0x800000D8, 4);
  u8* srr1 = Pointer(guest, context + 0x19C, 4);
  const bool owns_fpu = owner && Load32(owner) == context;
  if (srr1)
    Store32(srr1, owns_fpu ? (Load32(srr1) | 0x2000) : (Load32(srr1) & ~0x2000u));
  guest.msr = (owns_fpu ? (guest.msr | 0x2000) : (guest.msr & ~0x2000u)) | 0x2;
}

bool NativeSdk::EnterContextCall(CPUState& guest, const NativeCallback& call)
{
  // As IpcReplyHandler: a frame holding an OSContext, made current around
  // the callback; the callback returns to kNativeCallReturn.
  const u32 old_sp = guest.gpr[1];
  const u32 new_sp = old_sp - kContextFrame;
  u8* frame = Pointer(guest, new_sp, 4);
  u8* link = Pointer(guest, old_sp + 4, 4);
  u8* saved = Pointer(guest, new_sp + kContextFrame - 8, 8);
  if (!frame || !link || !saved || !Pointer(guest, new_sp + 8, 0x2C8))
    return false;
  Store32(frame, old_sp);
  Store32(link, guest.lr);
  Store32(saved, guest.gpr[30]);
  Store32(saved + 4, guest.gpr[31]);
  guest.gpr[1] = new_sp;
  guest.gpr[31] = guest.gpr[4];  // the interrupted context (OS handler argument)
  ClearGuestContext(guest, new_sp + 8);
  SetGuestContext(guest, new_sp + 8);
  guest.gpr[3] = call.r3;
  guest.gpr[4] = call.r4;
  guest.lr = kNativeCallReturn;
  guest.pc = call.function;
  return true;
}

bool NativeSdk::ReturnFromContextCall(CPUState& guest)
{
  const u32 sp = guest.gpr[1];
  const u8* frame = Pointer(guest, sp, 4);
  const u8* saved = Pointer(guest, sp + kContextFrame - 8, 8);
  if (!frame || !saved)
    return false;
  ClearGuestContext(guest, sp + 8);
  SetGuestContext(guest, guest.gpr[31]);
  const u32 old_sp = Load32(frame);
  const u8* link = Pointer(guest, old_sp + 4, 4);
  if (!link)
    return false;
  guest.gpr[30] = Load32(saved);
  guest.gpr[31] = Load32(saved + 4);
  guest.gpr[1] = old_sp;
  guest.lr = Load32(link);
  guest.pc = guest.lr;
  return true;
}

void NativeSdk::DoState(PointerWrap& p, bool present, u32 version)
{
  if (!present)
  {
    // A state saved without the native layer: no native interrupt is pending
    // in it, and its audio (if any) is the emulated one.
    m_callbacks.clear();
    if (m_audio && m_audio->mode == AudioState::Mode::Native)
    {
      m_audio->active = false;
      m_audio->abandoned = true;
      std::fprintf(stderr, "[native_audio] state without native data: original audio code runs\n");
    }
    return;
  }
  p.Do(m_callbacks);
  if (version >= 2)
    p.Do(m_deferred_callbacks);
  bool has_audio = m_audio != nullptr;
  p.Do(has_audio);
  if (!has_audio)
    return;
  if (!m_audio)
  {
    std::fprintf(stderr, "[native_audio] state has native audio data, audio hooks are off: load refused\n");
    p.SetMeasureMode();
    return;
  }
  AudioDoState(p);
  if (version < 2)
    return;
  bool has_fs = m_fs != nullptr;
  p.Do(has_fs);
  if (has_fs && !m_fs)
  {
    std::fprintf(stderr, "[native_fs] state has native NAND handles, NAND hooks are off: load refused\n");
    p.SetMeasureMode();
    return;
  }
  if (has_fs)
    FsDoState(p);
}

bool NativeSdk::SkipBootNotice(CPUState& guest)
{
  const char* selection=std::getenv("MODERNGEKKO_NATIVE_SDK");
  const std::string names=selection?selection:"all";
  const char* notices=std::getenv("MODERNGEKKO_WII_NOTICES");
  if(m_verify || (notices && std::string(notices)=="on") ||
     !(names=="all" || (","+names+",").find(",SkipNotices,")!=std::string::npos))return false;
  if(!SkipNotice(guest))return false;
  m_skip_notices=true;
  std::fprintf(stderr,"[native_sdk] boot notice omitted, return=0x%08X\n",guest.pc);
  return true;
}

bool NativeSdk::SkipNotice(CPUState& guest)
{
  if (!((guest.pc == kStrapScreen && guest.lr == kStrapScreenReturn) ||
        (guest.pc == kNoticeScreen && guest.lr == kNoticeScreenReturn)) ||
      guest.gpr[13] != kSda || !CodeMatches(guest, kFirstRunCode))
    return false;
  guest.pc = guest.lr;
  ++m_skipped_notices;
  return true;
}

void NativeSdk::AuditCallbackGuest(const CPUState& guest)
{
  ++m_callback_guest_checks;
  if(m_last_guest && m_last_guest != &guest)
  {
    ++m_callback_guest_mismatches;
    throw std::runtime_error("Native SDK callback retained a temporary CPU state");
  }
}

void NativeSdk::PrintStats() const
{
  std::fprintf(stderr,"[native_sdk] callback guest lifetime: restores=%llu checks=%llu mismatches=%llu\n",
               static_cast<unsigned long long>(m_callback_guest_restores),
               static_cast<unsigned long long>(m_callback_guest_checks),
               static_cast<unsigned long long>(m_callback_guest_mismatches));
  if (m_skip_notices)
    std::fprintf(stderr, "[native_sdk] Wii notice screens skipped=%llu\n",
                 static_cast<unsigned long long>(m_skipped_notices));
  const auto print = [](const char* name, const Stats& s) {
    std::fprintf(stderr,
                 "[native_sdk] %s native=%llu deferred=%llu verified=%llu mismatches=%llu "
                 "verify_skipped=%llu\n",
                 name, static_cast<unsigned long long>(s.native),
                 static_cast<unsigned long long>(s.deferred),
                 static_cast<unsigned long long>(s.verified),
                 static_cast<unsigned long long>(s.mismatches),
                 static_cast<unsigned long long>(s.verify_skipped));
  };
  if (m_pad_read)
    print("PADRead", m_pad_stats);
  if (m_kpad_read)
    print("KPADRead", m_kpad_stats);
  if (m_interrupt_hook)
  {
    std::fprintf(stderr,
                 "[native_sdk] native interrupts: posted=%llu delivered=%llu pending=%zu spurious=%llu "
                 "dvd_cancelled=%llu\n",
                 static_cast<unsigned long long>(m_callbacks_posted),
                 static_cast<unsigned long long>(m_callbacks_delivered), m_callbacks.size(),
                 static_cast<unsigned long long>(m_spurious_interrupts),
                 static_cast<unsigned long long>(m_dvd_cancelled));
  }
  PrintPadStats();
  PrintThpStats();
  PrintAudioStats();
  PrintFsStats();
  PrintModsStats();
  if (m_dvd_read)
  {
    print("DVDRead", m_dvd_stats);
    std::fprintf(stderr, "[native_sdk] DVDRead bytes=%llu pending_checks=%zu\n",
                 static_cast<unsigned long long>(m_dvd_bytes), m_dvd_checks.size());
  }
}
}  // namespace StaticRecompNativeSdk
