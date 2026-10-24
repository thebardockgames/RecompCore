// RecompCore: StaticRecomp CPU core - the port's own CPU runner (phase L4).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeRunner.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <xmmintrin.h>

#include "Core/PowerPC/StaticRecomp/StaticRecompExceptions.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompGuestRam.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompMemory.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeAx.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeClock.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeExi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGp.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGx.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativePi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeState.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeVi.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompStateStream.h"

namespace StaticRecompNativeRunner
{
namespace
{
constexpr u32 kLockedCacheBase = 0xE0000000u;
// SPR numbers of the fields CPUState keeps itself.
constexpr u32 kSprDsisr = 18, kSprDar = 19, kSprLr = 8, kSprCtr = 9, kSprSrr0 = 26, kSprSrr1 = 27;
constexpr u32 kSprEar = 282, kSprHid2 = 920, kSprGqr0 = 912, kSprHid4 = 1011;

// The chassis's condition-register field representation (the checkpoint's).
u64 CrFieldToInternal(u32 value)
{
  u64 cr_val = 0x100000000ull;
  cr_val |= static_cast<u64>((value & 1) != 0) << 59;   // SO
  cr_val |= static_cast<u64>((value & 2) == 0);         // EQ
  cr_val |= static_cast<u64>((value & 4) == 0) << 63;   // GT
  cr_val |= static_cast<u64>((value & 8) != 0) << 62;   // LT
  return cr_val;
}

u32 CrFieldFromInternal(u64 cr_val)
{
  u32 ppc_cr = static_cast<u32>((cr_val >> 59) & (8 | 1));  // LT, SO
  ppc_cr |= static_cast<u32>((cr_val & 0xFFFFFFFFull) == 0) << 1;
  ppc_cr |= static_cast<u32>(static_cast<s64>(cr_val) > 0) << 2;
  return ppc_cr;
}

u8* PhysicalRam(const CPUState& cpu, u32 ea, u32 size)
{
  if ((cpu.msr & 0x10u) != 0)
    return nullptr;
  if (cpu.ram && ea < cpu.ram_size && u64(ea) + size <= cpu.ram_size)
    return cpu.ram + ea;
  if (cpu.exram && (ea >> 28) == 0x1)
  {
    const u32 offset = ea & 0x0FFFFFFFu;
    if (offset < cpu.exram_size && u64(offset) + size <= cpu.exram_size)
      return cpu.exram + offset;
  }
  return nullptr;
}

u8* LockedCache(u32 ea, u32 size)
{
  const auto& guest_ram = StaticRecompGuestRam::GuestRam::Instance();
  const u32 offset = ea - kLockedCacheBase;
  if (ea < kLockedCacheBase || !guest_ram.L1() || offset + size > guest_ram.L1Size())
    return nullptr;
  return guest_ram.L1() + offset;
}

u64 ReadBe(const u8* p, u8 size)
{
  u64 value = 0;
  for (u8 i = 0; i < size; ++i)
    value = (value << 8) | p[i];
  return value;
}

void WriteBe(u8* p, u64 value, u8 size)
{
  for (u8 i = 0; i < size; ++i)
    p[i] = static_cast<u8>(value >> (8 * (size - 1 - i)));
}

Runner* Self(CPUState* cpu)
{
  return static_cast<Runner*>(cpu->external_user_data);
}
}  // namespace

Runner::Runner() = default;

bool Runner::HookHostCall(CPUState* cpu, u32 event)
{
  Runner& self = *Self(cpu);
  if (event == 0xffff0040u || event == 0xffff0041u)
  {
    // The Wii strap and controller notices (the oracle core does the same).
    return self.m_machine.sdk && self.m_machine.sdk->SkipBootNoticeEvent(*cpu, event);
  }
  return self.m_host_call_handler && self.m_host_call_handler(*cpu, event);
}

std::optional<u32> Runner::InvokeGuest(CPUState& caller, u32 address, std::span<const u32> arguments)
{
  const bool valid_stack = (caller.gpr[1] >= 0x80010000u && caller.gpr[1] < 0x82000000u) ||
                           (caller.gpr[1] >= 0x90010000u && caller.gpr[1] < 0x94000000u);
  if (!m_module || arguments.size() > 8 || !valid_stack)
    return std::nullopt;
  constexpr u32 kReturnSentinel = 0xFFFFFFFCu;
  CPUState invoke = caller;
  // Gather-pipe stores of the copy take the slow path (the burst count is the caller's).
  invoke.gather_buffer = nullptr;
  invoke.gpr[1] -= 0x800;
  invoke.pc = address;
  invoke.lr = kReturnSentinel;
  invoke.downcount = 0;
  invoke.dispatch_budget = 0;
  invoke.exception = 0;
  invoke.instruction_fallback = [](CPUState* state, u32, u32) { state->exception = 1; };
  for (size_t n = 0; n < arguments.size(); ++n)
    invoke.gpr[3 + n] = arguments[n];
  bool ok = true;
  for (int step = 0; step < 50000 && invoke.pc != kReturnSentinel && !invoke.exception; ++step)
  {
    if (!m_code_guard.Dispatchable(invoke.pc, (invoke.msr & 0x20) != 0, true) ||
        (!(m_machine.sdk && m_machine.sdk->Run(invoke)) && !m_module->dispatch(&invoke, invoke.pc)))
    {
      ok = false;
      break;
    }
  }
  // The SDK layer may have kept a pointer to the copy: back to the caller.
  if (m_machine.sdk)
    m_machine.sdk->RestoreCallbackGuest(caller);
  if (m_module->on_state_loaded)
    m_module->on_state_loaded(&caller);
  if (!ok || invoke.pc != kReturnSentinel || invoke.exception)
    return std::nullopt;
  caller.downcount += invoke.downcount;
  return invoke.gpr[3];
}

// The guest function at `address` on a copy of `guest`, at the same stack
// (the inline verifier: the real call writes the same frame afterwards).
std::optional<CPUState> Runner::RunGuestCopy(CPUState& guest, u32 address)
{
  constexpr u32 kReturnSentinel = 0xFFFFFFFCu;
  CPUState copy = guest;
  copy.gather_buffer = nullptr;
  copy.pc = address;
  copy.lr = kReturnSentinel;
  copy.dispatch_budget = 0;
  copy.exception = 0;
  copy.instruction_fallback = [](CPUState* state, u32, u32) { state->exception = 1; };
  for (int step = 0; step < 50000 && copy.pc != kReturnSentinel && !copy.exception; ++step)
  {
    if (!m_code_guard.Dispatchable(copy.pc, (copy.msr & 0x20) != 0, true) || !m_module->dispatch(&copy, copy.pc))
      break;
  }
  if (m_machine.sdk)
    m_machine.sdk->RestoreCallbackGuest(guest);
  if (copy.pc != kReturnSentinel || copy.exception)
    return std::nullopt;
  return copy;
}

Runner::~Runner()
{
  if (StaticRecompCpuHost::g_cpu_host == this)
    StaticRecompCpuHost::g_cpu_host = nullptr;
}

bool Runner::Init(const Config& config, std::string* error)
{
  if (!m_library.Open(config.module_path))
  {
    *error = "cannot open the module " + config.module_path;
    return false;
  }
  using GetModuleFn = const StaticRecompModuleDesc* (*)();
  const auto get_module = reinterpret_cast<GetModuleFn>(m_library.Symbol(STATICRECOMP_GET_MODULE_SYMBOL));
  m_module = get_module ? get_module() : nullptr;
  if (!m_module)
  {
    *error = "the module has no " STATICRECOMP_GET_MODULE_SYMBOL;
    return false;
  }
  if (auto reason = StaticRecompModuleLoader::ValidateModule(*m_module, config.game_id))
  {
    *error = *reason;
    return false;
  }
  m_code_guard.Bind(m_module);
  StaticRecompCpuHost::g_cpu_host = this;

  auto& ram = StaticRecompGuestRam::GuestRam::Instance();
  m_guest = CPUState{};
  m_guest.external_read = HookRead;
  m_guest.external_write = HookWrite;
  m_guest.external_read32 = HookRead32;
  m_guest.external_write32 = HookWrite32;
  m_guest.external_pointer = HookPointer;
  m_guest.instruction_fallback = HookInstructionFallback;
  m_guest.host_call = nullptr;
  m_guest.external_user_data = this;
  m_guest.ram = ram.Mem1();
  m_guest.ram_size = ram.Mem1Size();
  m_guest.exram = ram.Mem2();
  m_guest.exram_size = ram.Mem2Size();
  m_code_guard.InitLookup(m_guest.ram_size, m_guest.exram_size);
  m_guest.chunk_state = m_code_guard.StateData();
  using MenuVersionFn = unsigned (*)();
  const auto menu_version =
      reinterpret_cast<MenuVersionFn>(m_library.Symbol("staticrecomp_menu_extension_version"));
  m_menu_extension_version = menu_version ? menu_version() : 0;
  if (m_menu_extension_version >= 1 && m_menu_extension_version <= 3)
  {
    m_guest.host_call = HookHostCall;
    std::fprintf(stderr, "[native] menu extension module v%u\n", m_menu_extension_version);
  }

  // The machine, in the oracle's order: clock and PI first (the SDK layer
  // registers its audio DMA event at bind time), then EXI, the SDK layer,
  // the GP front and the VI.
  StaticRecompNativeAudio::NativeAxWii::SetCoefficientPaths(config.dsp_coef_paths);
  m_machine.clock = std::make_unique<StaticRecompNativeClock::NativeClock>();
  m_machine.pi = std::make_unique<StaticRecompNativePi::NativePi>();
  m_machine.exi = std::make_unique<StaticRecompNativeExi::NativeExi>(
      config.game_id, nullptr, 0,
      StaticRecompNativeExi::NativeExi::Paths{config.sram_path, config.font_dir});
  using NativeHooksFn = u32 (*)(const u32**, unsigned char**, int**);
  const auto native_hooks = reinterpret_cast<NativeHooksFn>(m_library.Symbol("staticrecomp_native_hooks"));
  if (native_hooks)
  {
    const u32* addresses = nullptr;
    unsigned char* flags = nullptr;
    int* any = nullptr;
    const u32 count = native_hooks(&addresses, &flags, &any);
    if (count && addresses && flags && any)
    {
      m_machine.sdk = std::make_unique<StaticRecompNativeSdk::NativeSdk>();
      m_machine.sdk->Bind(count, addresses, flags, any);
      // Inline native replacements: a direct call runs natively in place.
      using NativeInlineFn = u32 (*)(StaticRecompNativeSdk::NativeSdk::InlineFunction**);
      const auto native_inline = reinterpret_cast<NativeInlineFn>(m_library.Symbol("staticrecomp_native_inline"));
      StaticRecompNativeSdk::NativeSdk::InlineFunction* table = nullptr;
      if (native_inline && native_inline(&table) == count && table)
      {
        StaticRecompNativeSdk::NativeSdk::SetInlineVerifier(
            [this](CPUState& guest, u32 address) { return RunGuestCopy(guest, address); });
        m_machine.sdk->BindInline(count, table);
      }
    }
  }
  m_machine.gp = std::make_unique<StaticRecompNativeGp::NativeGp>();
  // CPU ABI v5: the generated code appends gather-pipe stores to the burst
  // itself (older modules ignore the fields and keep using HookWrite).
  m_machine.gp->BindGather(&m_guest.gather_buffer, &m_guest.gather_count);
  if (m_machine.sdk && m_machine.sdk->PadNative())
  {
    m_machine.vi = std::make_unique<StaticRecompNativeVi::NativeVi>();
    // The renderer measures 30 Hz frames (two fields) against the field
    // length to interpolate them; the VI installs this in Seed(), which a
    // checkpoint load does not run.
    StaticRecompNativeGx::SetTicksPerFieldProvider(
        [vi = m_machine.vi.get()] { return vi->TicksPerField(); });
  }
  // The guest holds the registers for the whole run.
  m_machine.clock->SetCpuState(&m_guest, &m_regs);
  m_machine.pi->SetGuestRegs(&m_regs);

  m_kernel_host.native_bats_updated = [this] { UpdateNativeBats(); };
  // dcbf/dcbi/dcbst/icbi: the code guard re-verifies the chunks they touch
  // (the oracle's block cache does the same through its invalidations).
  const auto physical = [this](u32 address) {
    const u32 bat = m_dbat.Entry(address);
    if ((bat & StaticRecompNativeBat::kMapped) == 0)
      return address & 0x3FFFFFFFu;
    return (bat & StaticRecompNativeBat::kResultMask) | (address & (StaticRecompNativeBat::kPageSize - 1));
  };
  m_kernel_host.invalidate_icache_line = [this, physical](u32 address) {
    address &= ~31u;
    m_code_guard.Invalidate(address, 32, physical(address), true);
  };
  m_kernel_host.invalidate_icache_lines = [this, physical](u32 address, u32 lines) {
    // One range per translation page, as the oracle's block cache does.
    address &= ~31u;
    u64 remaining = u64(lines) * 32;
    while (remaining)
    {
      const u32 page_end = (address | (StaticRecompNativeBat::kPageSize - 1)) + 1;
      const u32 length = static_cast<u32>(std::min<u64>(remaining, u64(page_end) - address));
      m_code_guard.Invalidate(address, length, physical(address), true);
      address += length;
      remaining -= length;
    }
  };

  m_burst.module = m_module;
  m_burst.guest = &m_guest;
  m_burst.regs = &m_regs;
  m_burst.machine = &m_machine;
  m_burst.guard = &m_code_guard;
  m_burst.idle_pc = config.idle_pc;
  m_burst.sync_exception_mask = StaticRecompExceptions::kSynchronousMask;
  m_burst.dispatches = &m_dispatches;
  m_burst.copy_bytes = &m_copy_bytes;
  m_burst.copy_batches = &m_copy_batches;
  m_burst.charged_cycles = &m_charged_cycles;
  m_burst.exceptions = &m_exceptions;
  m_burst.kernel_calls = &m_kernel_calls;
  m_burst.pc_samples = &m_pc_samples;
  m_burst.copy_env = [this] {
    StaticRecompMemory::CopyLoopEnv env;
    env.dbat = [](void* self, u32 address) { return static_cast<Runner*>(self)->Dbat(address); };
    env.dbat_context = this;
    env.downcount = &m_regs.downcount;
    return env;
  };
  InstallStateHooks();
  std::fprintf(stderr, "[native] module %s entry=0x%08X (%u chunks)%s\n", config.module_path.c_str(),
               m_module->entry_point, m_module->num_chunk_ranges,
               m_machine.sdk ? ", native SDK layer bound" : "");
  return true;
}

void Runner::SetRegisters(const StaticRecompPowerOn::Registers& r)
{
  std::memcpy(m_guest.gpr, r.gpr.data(), sizeof(m_guest.gpr));
  std::memcpy(m_regs.spr, r.spr.data(), sizeof(m_regs.spr));
  for (int i = 0; i < 16; ++i)
    m_guest.sr[i] = r.sr[i];
  m_guest.pc = r.pc;
  m_guest.msr = r.msr;
  m_guest.lr = m_regs.spr[kSprLr];
  m_guest.ctr = m_regs.spr[kSprCtr];
  m_guest.srr0 = m_regs.spr[kSprSrr0];
  m_guest.srr1 = m_regs.spr[kSprSrr1];
  m_guest.dar = m_regs.spr[kSprDar];
  m_guest.dsisr = m_regs.spr[kSprDsisr];
  m_guest.ear = m_regs.spr[kSprEar];
  m_guest.hid2 = m_regs.spr[kSprHid2];
  for (int i = 0; i < 8; ++i)
    m_guest.gqr[i] = m_regs.spr[kSprGqr0 + i];
  m_regs.exceptions = 0;
  m_regs.downcount = 0;
  if (m_machine.clock)
    m_machine.clock->RearmDecrementer();
  UpdateNativeBats();
  SetHostRoundingMode();
}

void Runner::UpdateNativeBats()
{
  const auto& ram = StaticRecompGuestRam::GuestRam::Instance();
  m_dbat.Update(m_regs.spr, ((m_regs.spr[kSprHid4] >> 25) & 1) != 0, ram.Mem1Size(), ram.Mem2Size(),
                ram.L1Size());
}

void Runner::DoNativeState(StaticRecompState::StateStream& p, bool present)
{
  m_machine.DoState(p, present);
  if (p.IsReadMode())
    UpdateNativeBats();
}

void Runner::SeedNativeDevicesForNativeState()
{
  m_machine.SeedForCheckpoint();
}

void Runner::SetHostRoundingMode() const
{
  // FPSCR.RN -> MXCSR.RC, FPSCR.NI -> flush-to-zero; exceptions masked.
  static const u32 rounding[] = {0u << 13, 3u << 13, 2u << 13, 1u << 13};
  u32 csr = rounding[m_guest.fpscr & 3] | 0x1F80u;
  if (m_guest.fpscr & 4)
    csr |= 0x8000u;
  _mm_setcsr(csr);
}

void Runner::InstallStateHooks()
{
  StaticRecompNativeState::ChassisHooks hooks;
  hooks.game_id = [this] { return std::string(m_module ? m_module->game_id : ""); };
  hooks.get_clock = [this](StaticRecompNativeState::ClockRecord& clock) {
    clock.ticks = static_cast<s64>(m_machine.clock->Now());
    m_machine.clock->GetTimeBaseStart(&clock.tb_start_value, &clock.tb_start_ticks);
  };
  hooks.set_clock = [this](const StaticRecompNativeState::ClockRecord& clock) {
    m_machine.clock->LoadCounter(clock.ticks);
    m_regs.downcount = 0;
    m_machine.clock->SetTimeBaseStart(clock.tb_start_value, clock.tb_start_ticks);
  };
  hooks.get_cpu = [this](StaticRecompNativeState::CpuRecord& cpu) {
    cpu.pc = m_guest.pc;
    cpu.npc = m_guest.pc;
    std::memcpy(cpu.gpr, m_guest.gpr, sizeof(cpu.gpr));
    for (int i = 0; i < 32; ++i)
    {
      std::memcpy(&cpu.ps[i][0], &m_guest.fpr[i], sizeof(u64));
      std::memcpy(&cpu.ps[i][1], &m_guest.ps1[i], sizeof(u64));
    }
    for (int i = 0; i < 8; ++i)
      cpu.cr[i] = CrFieldToInternal((m_guest.cr >> (28 - 4 * i)) & 0xF);
    cpu.msr = m_guest.msr;
    cpu.fpscr = m_guest.fpscr;
    cpu.exceptions = m_regs.exceptions;
    cpu.xer_stringctrl = (m_guest.xer & 0x7F) | (((m_guest.xer >> 8) & 0xFF) << 8);
    cpu.xer_ca = (m_guest.xer >> 29) & 1;
    cpu.xer_so_ov = (((m_guest.xer >> 31) & 1) << 1) | ((m_guest.xer >> 30) & 1);
    cpu.reserve_address = m_guest.reserve_addr;
    cpu.reserve = m_guest.reserve_valid ? 1 : 0;
    for (int i = 0; i < 16; ++i)
      cpu.sr[i] = m_guest.sr[i];
    std::memcpy(cpu.spr, m_regs.spr, sizeof(cpu.spr));
    cpu.spr[kSprLr] = m_guest.lr;
    cpu.spr[kSprCtr] = m_guest.ctr;
    cpu.spr[kSprSrr0] = m_guest.srr0;
    cpu.spr[kSprSrr1] = m_guest.srr1;
    cpu.spr[kSprDar] = m_guest.dar;
    cpu.spr[kSprDsisr] = m_guest.dsisr;
    cpu.spr[kSprEar] = m_guest.ear;
    cpu.spr[kSprHid2] = m_guest.hid2;
    for (int i = 0; i < 8; ++i)
      cpu.spr[kSprGqr0 + i] = m_guest.gqr[i];
  };
  hooks.set_cpu = [this](const StaticRecompNativeState::CpuRecord& cpu) {
    m_guest.pc = cpu.pc;
    std::memcpy(m_guest.gpr, cpu.gpr, sizeof(m_guest.gpr));
    for (int i = 0; i < 32; ++i)
    {
      std::memcpy(&m_guest.fpr[i], &cpu.ps[i][0], sizeof(u64));
      std::memcpy(&m_guest.ps1[i], &cpu.ps[i][1], sizeof(u64));
    }
    u32 cr = 0;
    for (int i = 0; i < 8; ++i)
      cr |= CrFieldFromInternal(cpu.cr[i]) << (28 - 4 * i);
    m_guest.cr = cr;
    m_guest.msr = cpu.msr;
    m_guest.fpscr = cpu.fpscr;
    m_regs.exceptions = cpu.exceptions;
    m_guest.xer = cpu.xer_stringctrl | (cpu.xer_ca << 29) | (cpu.xer_so_ov << 30);
    m_guest.reserve_addr = cpu.reserve_address;
    m_guest.reserve_valid = cpu.reserve != 0;
    for (int i = 0; i < 16; ++i)
      m_guest.sr[i] = cpu.sr[i];
    std::memcpy(m_regs.spr, cpu.spr, sizeof(m_regs.spr));
    m_guest.lr = m_regs.spr[kSprLr];
    m_guest.ctr = m_regs.spr[kSprCtr];
    m_guest.srr0 = m_regs.spr[kSprSrr0];
    m_guest.srr1 = m_regs.spr[kSprSrr1];
    m_guest.dar = m_regs.spr[kSprDar];
    m_guest.dsisr = m_regs.spr[kSprDsisr];
    m_guest.ear = m_regs.spr[kSprEar];
    m_guest.hid2 = m_regs.spr[kSprHid2];
    for (int i = 0; i < 8; ++i)
      m_guest.gqr[i] = m_regs.spr[kSprGqr0 + i];
    SetHostRoundingMode();
  };
  StaticRecompNativeState::SetChassisHooks(std::move(hooks));
}

bool Runner::LoadCheckpoint(const std::string& path)
{
  if (!StaticRecompNativeState::Load(path))
    return false;
  m_code_guard.ResetAll();  // reverify the module against the loaded memory
  if (m_module->on_state_loaded)
    m_module->on_state_loaded(&m_guest);
  return true;
}

bool Runner::SaveCheckpoint(const std::string& path)
{
  // The same hooks as for loading (they read the module's state).
  if (!StaticRecompNativeState::Save(path))
    return false;
  return true;
}

void Runner::RequestSave(const std::string& path)
{
  std::lock_guard lock(m_save_mutex);
  m_save_request = path;
  m_save_pending = true;
}

void Runner::Fail(const char* what, u32 address)
{
  if (!m_failed)
    std::fprintf(stderr, "[native] stopping: %s at 0x%08X (pc=0x%08X)\n", what, address, m_guest.pc);
  m_failed = true;
  m_run_flag = 1;
}

bool Runner::Run(const std::atomic<bool>& stop)
{
  auto& clock = *m_machine.clock;
  m_run_flag = 0;
  while (!m_failed && !stop.load(std::memory_order_relaxed))
  {
    if (m_save_pending.exchange(false))
    {
      std::string path;
      {
        std::lock_guard lock(m_save_mutex);
        path = m_save_request;
      }
      if (!SaveCheckpoint(path))
        std::fprintf(stderr, "[native] checkpoint not saved: %s\n", path.c_str());
    }
    clock.Slice();
    ++m_slices;
    do
    {
      if (!m_code_guard.Dispatchable(m_guest.pc, (m_guest.msr & 0x20) != 0, true))
      {
        Fail("no verified module code", m_guest.pc);
        break;
      }
      m_guest.exception = 0;
      m_guest.program_exception = 0;
      m_guest.downcount = 0;
      m_guest.dispatch_budget = 0;
      m_guest.timebase = StaticRecompNativeClock::NativeClock::CurrentTimeBase();
      StaticRecompBurst::Run(m_burst, true, &m_run_flag, 0);
      if ((m_regs.exceptions & StaticRecompExceptions::kSynchronousMask) != 0 &&
          StaticRecompExceptions::DeliverSynchronous(m_guest, m_regs.exceptions))
        ++m_sync_exceptions;
    } while (m_regs.downcount > 0 && m_run_flag == 0 && !stop.load(std::memory_order_relaxed));
  }
  return !m_failed;
}

void Runner::PrintStats() const
{
  std::fprintf(stderr,
               "[native] slices=%llu dispatches=%llu exceptions=%llu sync_exceptions=%llu "
               "kernel_calls=%llu copy_bytes=%llu fallbacks=%llu unhandled_reads=%llu "
               "unhandled_writes=%llu smc_failed=%u verifications=%llu\n",
               static_cast<unsigned long long>(m_slices), static_cast<unsigned long long>(m_dispatches),
               static_cast<unsigned long long>(m_exceptions),
               static_cast<unsigned long long>(m_sync_exceptions),
               static_cast<unsigned long long>(m_kernel_calls),
               static_cast<unsigned long long>(m_copy_bytes), static_cast<unsigned long long>(m_fallbacks),
               static_cast<unsigned long long>(m_unhandled_reads),
               static_cast<unsigned long long>(m_unhandled_writes), m_code_guard.Failed(),
               static_cast<unsigned long long>(m_code_guard.Verifications()));
  // The devices' own counters, as the oracle's core prints them (audio output
  // hash, interrupts, GP, VI, PI, EXI, clock).
  if (m_machine.sdk)
    m_machine.sdk->PrintStats();
  if (m_machine.gp)
    m_machine.gp->PrintStats();
  if (m_machine.vi)
    m_machine.vi->PrintStats();
  if (m_machine.pi)
    m_machine.pi->PrintStats();
  if (m_machine.exi)
    m_machine.exi->PrintStats();
  if (m_machine.clock)
    m_machine.clock->PrintStats();
}

// ---- hooks from the generated code ----

u64 Runner::HookRead(CPUState* cpu, u32 ea, u8 size)
{
  if (const u8* cache = LockedCache(ea, size))
    return ReadBe(cache, size);
  if (const u8* ram = PhysicalRam(*cpu, ea, size))
    return ReadBe(ram, size);
  Runner* self = Self(cpu);
  u64 value = 0;
  if (self->m_machine.Read(ea, size, &value))
    return value;
  if (self->m_unhandled_reads++ < 16)
    std::fprintf(stderr, "[native] unhandled read%u at 0x%08X (pc=0x%08X)\n", size * 8, ea, cpu->pc);
  return 0;
}

void Runner::HookWrite(CPUState* cpu, u32 ea, u64 value, u8 size)
{
  // The gather pipe first: the most frequent write that leaves the RAM.
  if ((ea & 0xFFFFF000u) == 0xCC008000u)
  {
    Runner* self = Self(cpu);
    if (self->m_machine.gp && self->m_machine.gp->TryGatherFast(value, size))
      return;
  }
  if (u8* cache = LockedCache(ea, size))
  {
    WriteBe(cache, value, size);
    return;
  }
  if (u8* ram = PhysicalRam(*cpu, ea, size))
  {
    WriteBe(ram, value, size);
    return;
  }
  Runner* self = Self(cpu);
  if (self->m_machine.Write(ea, size, value))
    return;
  if (self->m_unhandled_writes++ < 16)
    std::fprintf(stderr, "[native] unhandled write%u of 0x%llx at 0x%08X (pc=0x%08X)\n", size * 8,
                 static_cast<unsigned long long>(value), ea, cpu->pc);
}

u32 Runner::HookRead32(CPUState* cpu, u32 ea, u8)
{
  Self(cpu)->Fail("eciwx", ea);
  return 0;
}

void Runner::HookWrite32(CPUState* cpu, u32 ea, u32, u8)
{
  Self(cpu)->Fail("ecowx", ea);
}

void* Runner::HookPointer(CPUState*, u32 ea, u32 size)
{
  const auto& guest_ram = StaticRecompGuestRam::GuestRam::Instance();
  if (ea >= kLockedCacheBase && size != 0 && (ea - kLockedCacheBase) + size <= guest_ram.L1Size())
    return guest_ram.L1() + (ea - kLockedCacheBase);
  return nullptr;
}

void Runner::HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia)
{
  Runner* self = Self(cpu);
  const StaticRecompGuestRegs::RegsView regs = self->m_regs.View();
  const StaticRecompKernel::KernelHost& kernel = self->m_kernel_host;
  if (StaticRecompKernel::TryRunDmaRegister(kernel, *cpu, regs, raw, cia) ||
      StaticRecompKernel::TryRunDecrementerWrite(kernel, *cpu, regs, raw, cia) ||
      StaticRecompKernel::TryRunDecrementerRead(kernel, *cpu, regs, raw, cia) ||
      StaticRecompKernel::TryRunCacheOperation(kernel, *cpu, regs, raw, cia) ||
      StaticRecompKernel::TryRunGatherPipeRegister(kernel, *cpu, regs, raw, cia) ||
      StaticRecompKernel::TryRunScratchRegister(kernel, *cpu, regs, raw, cia) ||
      StaticRecompKernel::TryRunSystemRegister(kernel, *cpu, regs, raw, cia))
    return;
  ++self->m_fallbacks;
  char what[64];
  std::snprintf(what, sizeof(what), "instruction %08x the module does not execute", raw);
  self->Fail(what, cia);
  cpu->pc = cia;
}
}  // namespace StaticRecompNativeRunner
