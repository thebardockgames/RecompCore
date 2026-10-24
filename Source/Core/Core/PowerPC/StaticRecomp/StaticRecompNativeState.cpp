// RecompCore: StaticRecomp CPU core - native checkpoints (phase S, step 1).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeState.h"

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "Core/PowerPC/StaticRecomp/StaticRecompTypes.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestMemory.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCpuHost.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGx.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"

namespace StaticRecompNativeState
{
namespace
{
constexpr u32 kMagic = 0x534B474D;  // "MGKS"
constexpr u32 kVersion = 1;
// Section tags: a reader skips tags it does not know by their length.
constexpr u32 kTagCpu = 0x20555043;     // "CPU "
constexpr u32 kTagClock = 0x4B4C4F43;   // "COLK"
constexpr u32 kTagMem1 = 0x314D454D;    // "MEM1"
constexpr u32 kTagMem2 = 0x324D454D;    // "MEM2"
constexpr u32 kTagL1 = 0x43314C20;      // " L1C"
constexpr u32 kTagVideo = 0x4F444956;   // "VIDO" (the chassis's layout: states before the GX section)
constexpr u32 kTagGx = 0x20205847;      // "GX  " (the port's GX state, phase S)
constexpr u32 kTagNative = 0x5654414E;  // "NATV"
constexpr u32 kTagEnd = 0x20444E45;     // "END "

std::mutex s_pending_mutex;
std::string s_pending_load;
ChassisHooks s_chassis;

class Writer
{
public:
  explicit Writer(const std::string& path) : m_file(std::fopen(path.c_str(), "wb")) {}
  ~Writer()
  {
    if (m_file)
      std::fclose(m_file);
  }
  bool Ok() const { return m_file && !m_failed; }
  void Bytes(const void* data, size_t size)
  {
    if (m_file && std::fwrite(data, 1, size, m_file) != size)
      m_failed = true;
  }
  template <typename T>
  void Pod(const T& value)
  {
    Bytes(&value, sizeof(value));
  }
  void Section(u32 tag, const void* data, u64 size)
  {
    Pod(tag);
    Pod(size);
    Bytes(data, static_cast<size_t>(size));
  }

private:
  std::FILE* m_file;
  bool m_failed = false;
};

struct Section
{
  u32 tag = 0;
  std::vector<u8> data;
};

bool ReadAll(const std::string& path, std::vector<u8>& out)
{
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return false;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  out.resize(size > 0 ? static_cast<size_t>(size) : 0);
  const bool ok = size >= 0 && std::fread(out.data(), 1, out.size(), file) == out.size();
  std::fclose(file);
  return ok;
}

// The native sections (the core's DoNativeState), in the same bytes the
// chassis's PointerWrap produced for them.
std::vector<u8> SerializeNative(bool* ok)
{
  StaticRecompState::BufferStream stream;
  if (StaticRecompCpuHost::g_cpu_host)
    StaticRecompCpuHost::g_cpu_host->DoNativeState(stream, true);
  *ok = *ok && StaticRecompCpuHost::g_cpu_host && stream.IsWriteMode();
  return stream.Buffer();
}

bool DeserializeNative(const std::vector<u8>& blob)
{
  if (!StaticRecompCpuHost::g_cpu_host)
    return false;
  StaticRecompState::BufferStream stream(blob);
  StaticRecompCpuHost::g_cpu_host->DoNativeState(stream, true);
  return stream.IsReadMode();
}
}  // namespace

void SetChassisHooks(ChassisHooks hooks)
{
  s_chassis = std::move(hooks);
}

bool IsNativeStatePath(const std::string& path)
{
  return path.size() >= 4 && path.compare(path.size() - 4, 4, ".mgs") == 0;
}

void SetPendingLoad(std::string path)
{
  std::lock_guard lock(s_pending_mutex);
  s_pending_load = std::move(path);
}

bool TakePendingLoad(std::string* path)
{
  std::lock_guard lock(s_pending_mutex);
  if (s_pending_load.empty())
    return false;
  *path = std::move(s_pending_load);
  s_pending_load.clear();
  return true;
}

bool Save(const std::string& path)
{
  const auto memory = StaticRecompGuestMemory::GuestMemory::Own();
  bool ok = true;
  if (!s_chassis.get_cpu || !s_chassis.get_clock)
  {
    std::fprintf(stderr, "[native_state] no CPU/clock source installed\n");
    return false;
  }
  CpuRecord cpu{};
  s_chassis.get_cpu(cpu);
  ClockRecord clock{};
  s_chassis.get_clock(clock);

  if (StaticRecompCpuHost::g_cpu_host)
    StaticRecompCpuHost::g_cpu_host->SeedNativeDevicesForNativeState();
  // The GX state in the port's layout when the native renderer owns the GPU
  // (the GP front's registers are in the native section), and, while the
  // chassis exists, its layout too: a run where the chassis renders needs its
  // command processor and pixel engine from there, and reads nothing of ours.
  const bool own_gpu = StaticRecompNativeGx::OwnsGpu();
  const std::vector<u8> gx = own_gpu ? StaticRecompNativeGx::SerializeState() : std::vector<u8>{};
  const std::vector<u8> video = StaticRecompNativeGx::SerializeLegacyVideoSection();
  const std::vector<u8> native = SerializeNative(&ok);
  if (!ok || (gx.empty() && video.empty()))
  {
    std::fprintf(stderr, "[native_state] could not serialise the graphics or native sections\n");
    return false;
  }

  Writer out(path);
  out.Pod(kMagic);
  out.Pod(kVersion);
  const std::string game_id = s_chassis.game_id ? s_chassis.game_id() : std::string();
  const u32 game_id_size = static_cast<u32>(game_id.size());
  out.Pod(game_id_size);
  out.Bytes(game_id.data(), game_id.size());
  out.Section(kTagCpu, &cpu, sizeof(cpu));
  out.Section(kTagClock, &clock, sizeof(clock));
  out.Section(kTagMem1, memory.GetRAM(), memory.GetRamSizeReal());
  out.Section(kTagMem2, memory.GetEXRAM(), memory.GetExRamSizeReal());
  out.Section(kTagL1, memory.GetL1Cache(), memory.GetL1CacheSize());
  if (!gx.empty())
    out.Section(kTagGx, gx.data(), gx.size());
  if (!video.empty())
    out.Section(kTagVideo, video.data(), video.size());
  out.Section(kTagNative, native.data(), native.size());
  out.Section(kTagEnd, nullptr, 0);
  if (!out.Ok())
  {
    std::fprintf(stderr, "[native_state] could not write %s\n", path.c_str());
    return false;
  }
  std::fprintf(stderr, "[native_state] saved %s: pc=%08x ticks=%lld gx=%zu video=%zu native=%zu bytes\n",
               path.c_str(), cpu.pc, static_cast<long long>(clock.ticks), gx.size(), video.size(),
               native.size());
  return true;
}

bool Load(const std::string& path)
{
  if (!s_chassis.set_cpu || !s_chassis.set_clock)
  {
    std::fprintf(stderr, "[native_state] no CPU/clock target installed\n");
    return false;
  }
  const std::string running = s_chassis.game_id ? s_chassis.game_id() : std::string();
  std::vector<u8> file;
  if (!ReadAll(path, file))
  {
    std::fprintf(stderr, "[native_state] cannot read %s\n", path.c_str());
    return false;
  }
  size_t pos = 0;
  const auto take = [&](void* dst, size_t size) {
    if (pos + size > file.size())
      return false;
    std::memcpy(dst, file.data() + pos, size);
    pos += size;
    return true;
  };
  u32 magic = 0, version = 0, game_id_size = 0;
  if (!take(&magic, 4) || !take(&version, 4) || magic != kMagic || version != kVersion ||
      !take(&game_id_size, 4) || game_id_size > 64)
  {
    std::fprintf(stderr, "[native_state] %s is not a native checkpoint of this version\n", path.c_str());
    return false;
  }
  std::string game_id(game_id_size, '\0');
  if (!take(game_id.data(), game_id_size))
    return false;
  if (game_id != running)
  {
    std::fprintf(stderr, "[native_state] %s was saved from %s, not %s\n", path.c_str(),
                 game_id.c_str(), running.c_str());
    return false;
  }
  std::vector<Section> sections;
  while (true)
  {
    Section section;
    u64 size = 0;
    if (!take(&section.tag, 4) || !take(&size, 8) || pos + size > file.size())
    {
      std::fprintf(stderr, "[native_state] %s is truncated\n", path.c_str());
      return false;
    }
    if (section.tag == kTagEnd)
      break;
    section.data.assign(file.begin() + pos, file.begin() + pos + size);
    pos += size;
    sections.push_back(std::move(section));
  }
  const auto find = [&](u32 tag) -> Section* {
    for (Section& s : sections)
      if (s.tag == tag)
        return &s;
    return nullptr;
  };
  Section* cpu_section = find(kTagCpu);
  Section* clock_section = find(kTagClock);
  Section* mem1 = find(kTagMem1);
  Section* mem2 = find(kTagMem2);
  Section* l1 = find(kTagL1);
  Section* video = find(kTagVideo);
  Section* gx = find(kTagGx);
  Section* native = find(kTagNative);
  const auto memory = StaticRecompGuestMemory::GuestMemory::Own();
  if (!cpu_section || cpu_section->data.size() != sizeof(CpuRecord) || !clock_section ||
      clock_section->data.size() != sizeof(ClockRecord) || !mem1 || !mem2 || !l1 || (!video && !gx) || !native ||
      mem1->data.size() != memory.GetRamSizeReal() || mem2->data.size() != memory.GetExRamSizeReal() ||
      l1->data.size() != memory.GetL1CacheSize())
  {
    std::fprintf(stderr, "[native_state] %s: sections missing or sized for another memory layout\n",
                 path.c_str());
    return false;
  }

  // Memory first (the graphics and native sections may look at it), then the
  // graphics state (its load hook reseeds the GP front unless our section
  // follows), then our sections, then the registers.
  std::memcpy(memory.GetRAM(), mem1->data.data(), mem1->data.size());
  std::memcpy(memory.GetEXRAM(), mem2->data.data(), mem2->data.size());
  std::memcpy(memory.GetL1Cache(), l1->data.data(), l1->data.size());

  ClockRecord clock{};
  std::memcpy(&clock, clock_section->data.data(), sizeof(clock));
  // The cycle counter continues from the saved value and the slice restarts.
  // The chassis's events from the boot (its periodic device callbacks) would
  // otherwise catch up through the whole jump of the cycle counter; the native
  // sections below reschedule every event that matters.
  s_chassis.set_clock(clock);

  // The GX state: the port's section when the native renderer owns the GPU
  // (seeds the front end now or when it is created); the chassis's layout
  // through the host's hook when the chassis renders, or for an older file.
  const bool use_own = gx && StaticRecompNativeGx::OwnsGpu();
  if (!use_own && !video)
  {
    std::fprintf(stderr,
                 "[native_state] %s: the chassis renders but the checkpoint has only the port's GX state\n",
                 path.c_str());
    return false;
  }
  const bool graphics_ok = use_own ? StaticRecompNativeGx::LoadState(gx->data) :
                                     StaticRecompNativeGx::LoadLegacyVideoSection(video->data);
  if (!graphics_ok)
  {
    std::fprintf(stderr, "[native_state] %s: graphics section rejected\n", path.c_str());
    return false;
  }
  if (!DeserializeNative(native->data))
  {
    std::fprintf(stderr, "[native_state] %s: native section rejected\n", path.c_str());
    return false;
  }

  CpuRecord cpu{};
  std::memcpy(&cpu, cpu_section->data.data(), sizeof(cpu));
  s_chassis.set_cpu(cpu);
  if (StaticRecompCpuHost::g_cpu_host)
    StaticRecompCpuHost::g_cpu_host->UpdateNativeBats();
  // The registers came after the native sections: the decrementer event is
  // armed now that DEC holds the saved count (without this the OS alarm chain
  // never restarted: 0 mtdec in a run from a native checkpoint).
  if (auto* native_clock = StaticRecompNativeClock::NativeClock::Instance())
    native_clock->RearmDecrementer();
  // The same line the chassis prints: the benchmark confirms the load with it.
  std::fprintf(stderr, "[savestate] loaded=%s pc=0x%08x\n", path.c_str(), cpu.pc);
  std::fprintf(stderr, "[native_state] loaded %s: ticks=%lld %s=%zu native=%zu bytes\n",
               path.c_str(), static_cast<long long>(clock.ticks), use_own ? "gx" : "video",
               use_own ? gx->data.size() : video->data.size(),
               native->data.size());
  return true;
}
}  // namespace StaticRecompNativeState
