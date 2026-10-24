// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_CPU_H
#define DOLRECOMP_CPU_H

#include "types.h"
// more msvc shit fuck msvc
#if defined(_MSC_VER)
#define GXRUNTIME_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define GXRUNTIME_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define GXRUNTIME_ALWAYS_INLINE inline
#endif

// Minimal CPU support ABI for generated code and CPU tests.
//
// ABI status:
// - Fields up through `ram_size` intentionally mirror DolRecomp's current
//   generated-code CPUState contract.
// - GXRuntime extends that contract only at the tail with `external_pointer`,
//   used as a fast path for host-backed external memory such as locked cache.
// - Do not insert fields into the mirrored prefix without coordinating a
//   generated-code ABI bump with DolRecomp.
// - ABI v2 adds `downcount` at the tail: a guest-cycle charge accumulator.
//   Generated code subtracts each basic block's Gekko cycle cost (mirroring
//   Dolphin's PPCTables costs) at block entry; the embedding environment
//   consumes and resets it (Dolphin chassis: per-dispatch flush into
//   ppc_state.downcount). Hosts that do not meter guest time may ignore it
//   (s64: it cannot wrap in any realistic session).
#define GXRUNTIME_CPU_ABI_VERSION 5u
#define GXRUNTIME_CPU_ABI_DOLRECOMP_PREFIX 1u
#define GXRUNTIME_CPU_ABI_EXTERNAL_POINTER_EXTENSION 1u

#define GC_MAIN_RAM_SIZE    (24 * 1024 * 1024)
#define GC_RAM_BASE         0x80000000u
#define GC_RAM_UNCACHED     0xC0000000u

#define PPC_EXC_PROGRAM        0x00000001u
#define PPC_EXC_DSI            0x00000002u
#define PPC_EXC_ALIGNMENT      0x00000004u
#define PPC_EXC_SYSTEM_CALL    0x00000008u
#define PPC_EXC_MACHINE_CHECK  0x00000010u
#define PPC_EXC_FP_UNAVAILABLE 0x00000020u

#define PPC_PROGRAM_FP        0x00100000u
#define PPC_PROGRAM_ILLEGAL   0x00080000u
#define PPC_PROGRAM_PRIV      0x00040000u
#define PPC_PROGRAM_TRAP      0x00020000u

#define PPC_DSI_EAR_DISABLED  0x00100000u

#define PPC_VECTOR_MACHINE_CHECK  0x00200u
#define PPC_VECTOR_DSI            0x00300u
#define PPC_VECTOR_ALIGNMENT      0x00600u
#define PPC_VECTOR_PROGRAM        0x00700u
#define PPC_VECTOR_FP_UNAVAILABLE 0x00800u
#define PPC_VECTOR_SYSTEM_CALL    0x00C00u

#define PPC_HID2_LSQE   0x80000000u
#define PPC_HID2_PSE    0x20000000u
#define PPC_HID2_LCE    0x10000000u
#define PPC_HID2_DCHERR 0x00800000u
#define PPC_HID2_DCHEE  0x00080000u

#define PPC_BIT(n) (1u << (31u - (n)))
#define PPC_MSR_RFI_MASK 0x87C0FFFFu
#define PPC_MSR_POW PPC_BIT(13)
#define PPC_MSR_ILE PPC_BIT(15)
#define PPC_MSR_EE  PPC_BIT(16)
#define PPC_MSR_PR  PPC_BIT(17)
#define PPC_MSR_FP  PPC_BIT(18)
#define PPC_MSR_ME  PPC_BIT(19)
#define PPC_MSR_FE0 PPC_BIT(20)
#define PPC_MSR_SE  PPC_BIT(21)
#define PPC_MSR_BE  PPC_BIT(22)
#define PPC_MSR_FE1 PPC_BIT(23)
#define PPC_MSR_IP  PPC_BIT(25)
#define PPC_MSR_IR  PPC_BIT(26)
#define PPC_MSR_DR  PPC_BIT(27)
#define PPC_MSR_PM  PPC_BIT(29)
#define PPC_MSR_RI  PPC_BIT(30)
#define PPC_MSR_LE  PPC_BIT(31)

#define PPC_EAR_ENABLE 0x80000000u
#define PPC_SRR1_MACHINE_CHECK_DCBZL PPC_BIT(10)

typedef struct CPUState CPUState;

typedef u64 (*PPCExternalRead)(CPUState* cpu, u32 ea, u8 size);
typedef void (*PPCExternalWrite)(CPUState* cpu, u32 ea, u64 value, u8 size);
typedef u32 (*PPCExternalRead32)(CPUState* cpu, u32 ea, u8 rid);
typedef void (*PPCExternalWrite32)(CPUState* cpu, u32 ea, u32 value, u8 rid);
typedef void* (*PPCExternalPointer)(CPUState* cpu, u32 ea, u32 size);
typedef void (*PPCInstructionFallback)(CPUState* cpu, u32 raw, u32 cia);
typedef bool (*PPCHostCall)(CPUState* cpu, u32 address);

struct CPUState {
    u32 gpr[32];
    f64 fpr[32];
    f64 ps1[32];
    u32 pc;
    u32 lr;
    u32 ctr;
    u32 cr;
    u32 xer;
    u32 fpscr;
    u32 msr;
    u32 srr0;
    u32 srr1;
    u32 dar;
    u32 dsisr;
    u32 ear;
    u32 hid2;
    u64 timebase;
    u32 sr[16];
    u32 gqr[8];
    u32 exception;
    u32 program_exception;
    u32 tlb_last_vps;
    u32 tlb_last_index;
    u32 tlb_invalidate_count;
    u32 external_addr;
    u32 external_value;
    u8 external_rid;
    u8 external_read_count;
    u8 external_write_count;
    u32 reserve_addr;
    bool reserve_valid;
    u32 locked_cache_tag[512];
    bool locked_cache_valid[512];
    PPCExternalRead external_read;
    PPCExternalWrite external_write;
    PPCExternalRead32 external_read32;
    PPCExternalWrite32 external_write32;
    PPCInstructionFallback instruction_fallback;
    PPCHostCall host_call;
    void* external_user_data;

    u8* ram;
    u32 ram_size;
    PPCExternalPointer external_pointer;
    s64 downcount;
    u8* exram;
    u32 exram_size;
    // ABI v3 appends a live chassis cycle budget; the ABI v2 prefix is intact.
    s64 dispatch_budget;
    // ABI v4 appends the chassis SMC chunk state (1 = verified), indexed like
    // the module chunk table; native direct calls check it before entering
    // another chunk. NULL disables direct calls. The ABI v3 prefix is intact.
    const u8* chunk_state;
    // ABI v5 appends the GP write-gather pipe's current 32-byte burst, owned
    // by the runtime: stores to the gather pipe (0xCC008000) are appended to
    // it here while they leave room in the burst; the store that completes a
    // burst, and every store while gather_buffer is NULL, still goes through
    // external_write. The ABI v4 prefix is intact.
    u8* gather_buffer;
    u32 gather_count;
};

#include <stdio.h>

typedef void (*PPCMemWriteJournal)(u32 offset, u32 size, void* user);
extern PPCMemWriteJournal g_mem_write_journal;
extern void* g_mem_write_journal_user;

static GXRUNTIME_ALWAYS_INLINE u8* get_ram_ptr(CPUState* cpu, u32 addr, u32 size, u32* out_offset) {
    u32 masked_addr = addr & ~0x40000000u;
    
    // Check MEM2 (EXRAM) first as it is much more common in Wii titles
    if (cpu->exram) {
        u32 offset = masked_addr - 0x90000000u;
        if (offset <= cpu->exram_size - size) {
            if (out_offset) *out_offset = (u32)-1;
            return cpu->exram + offset;
        }
    }
    
    // Check MEM1
    u32 offset = masked_addr - 0x80000000u;
    if (offset <= cpu->ram_size - size) {
        if (out_offset) *out_offset = offset;
        return cpu->ram + offset;
    }
    
    return NULL;
}

// ABI v5: a gather-pipe store that leaves room in the current burst, appended
// big-endian in store order (the runtime flushes full bursts).
static GXRUNTIME_ALWAYS_INLINE bool gather_append(CPUState* cpu, u32 addr, u64 value, u32 size) {
    if ((addr & 0xFFFFF000u) != 0xCC008000u || cpu->gather_buffer == NULL || cpu->gather_count + size >= 32u)
        return false;
    u8* out = cpu->gather_buffer + cpu->gather_count;
    for (u32 i = 0; i < size; ++i)
        out[i] = (u8)(value >> (8u * (size - 1u - i)));
    cpu->gather_count += size;
    return true;
}

static GXRUNTIME_ALWAYS_INLINE u64 mem_read64(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 8, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return cpu->external_read(cpu, addr, 8);
        return 0;
    }
    return read_be64(ptr);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write64(CPUState* cpu, u32 addr, u64 value) {
    u32 offset;
    u8* ptr = get_ram_ptr(cpu, addr, 8, &offset);
    if (ptr == NULL) {
        if (gather_append(cpu, addr, value, 8))
            return;
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 8);
        }
        return;
    }
    if (g_mem_write_journal && offset != (u32)-1) g_mem_write_journal(offset, 8, g_mem_write_journal_user);
    write_be64(ptr, value);
}

static GXRUNTIME_ALWAYS_INLINE u32 mem_read32(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 4, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return (u32)cpu->external_read(cpu, addr, 4);
        return 0;
    }
    return read_be32(ptr);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write32(CPUState* cpu, u32 addr, u32 value) {
    u32 offset;
    u8* ptr = get_ram_ptr(cpu, addr, 4, &offset);
    if (ptr == NULL) {
        if (gather_append(cpu, addr, value, 4))
            return;
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 4);
        }
        return;
    }
    if (g_mem_write_journal && offset != (u32)-1) g_mem_write_journal(offset, 4, g_mem_write_journal_user);
    write_be32(ptr, value);
}

static GXRUNTIME_ALWAYS_INLINE u16 mem_read16(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 2, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return (u16)cpu->external_read(cpu, addr, 2);
        return 0;
    }
    return read_be16(ptr);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write16(CPUState* cpu, u32 addr, u16 value) {
    u32 offset;
    u8* ptr = get_ram_ptr(cpu, addr, 2, &offset);
    if (ptr == NULL) {
        if (gather_append(cpu, addr, value, 2))
            return;
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 2);
        }
        return;
    }
    if (g_mem_write_journal && offset != (u32)-1) g_mem_write_journal(offset, 2, g_mem_write_journal_user);
    write_be16(ptr, value);
}

static GXRUNTIME_ALWAYS_INLINE u8 mem_read8(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 1, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return (u8)cpu->external_read(cpu, addr, 1);
        return 0;
    }
    return *ptr;
}

static GXRUNTIME_ALWAYS_INLINE void mem_write8(CPUState* cpu, u32 addr, u8 value) {
    u32 offset;
    u8* ptr = get_ram_ptr(cpu, addr, 1, &offset);
    if (ptr == NULL) {
        if (gather_append(cpu, addr, value, 1))
            return;
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 1);
        }
        return;
    }
    if (g_mem_write_journal && offset != (u32)-1) g_mem_write_journal(offset, 1, g_mem_write_journal_user);
    *ptr = value;
}

/* Paired-single quantized loads and stores (psq_l/psq_st and their indexed and
 * update forms), inline so generated code specializes each site (register,
 * w, GQR index, indexed are constants there). Semantics mirror Dolphin's
 * interpreter, the chassis lockstep oracle, exactly:
 *  - psq_l/psq_st (non-indexed) require only HID2.LSQE; the indexed forms
 *    are never gated. PSE is not checked, and no alignment exceptions are
 *    raised (unaligned accesses go straight to memory).
 *  - Invalid GQR types 1-3 load 0.0 into both lanes and store nothing.
 *  - Dequantization: f32(int) * f32 power-of-two, rounded once to f32.
 *  - Quantization: round the lane to f32 first, multiply by the f32
 *    power-of-two scale, clamp in f32, truncate. NaN quantizes to 0
 *    (matching SType(NaN-after-clamp) in release Dolphin on arm64).
 * Scales are exact powers of two (-32..31) built in the exponent field: the
 * values ldexp/ldexpf return. In memory that is not RAM (the locked L1 cache)
 * both lanes go through one host pointer when the host provides it. */
void ppc_program_exception(CPUState* cpu, u32 cause, u32 cia);

static GXRUNTIME_ALWAYS_INLINE f64 psq_pow2_f64(s32 e) {
    const u64 bits = (u64)(1023 + e) << 52;
    f64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static GXRUNTIME_ALWAYS_INLINE f32 psq_pow2_f32(s32 e) {
    const u32 bits = (u32)(127 + e) << 23;
    f32 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static GXRUNTIME_ALWAYS_INLINE u32 psq_type_size(u8 type) {
    return type == 0 ? 4u : (type == 4 || type == 6) ? 1u : (type == 5 || type == 7) ? 2u : 0u;
}

static GXRUNTIME_ALWAYS_INLINE f64 psq_dequant(f64 value, s32 scale) {
    /* value is an integer below 2^16: scaling by 2^-scale is exact in f64. */
    return scale == 0 ? (f64)(f32)value : (f64)(f32)(value * psq_pow2_f64(-scale));
}

static GXRUNTIME_ALWAYS_INLINE s64 psq_quantize_int(f64 value, s64 min_value, s64 max_value, s32 scale) {
    const f32 conv = (f32)value * psq_pow2_f32(scale);
    if (conv != conv)
        return 0;
    if (conv <= (f32)min_value)
        return min_value;
    if (conv >= (f32)max_value)
        return max_value;
    return (s64)conv;
}

static GXRUNTIME_ALWAYS_INLINE f64 psq_from_raw(u32 raw, u8 type, s32 scale) {
    switch (type) {
    case 0: return f64_value(convert_to_double(raw));
    case 4: return psq_dequant((f64)(u8)raw, scale);
    case 5: return psq_dequant((f64)(u16)raw, scale);
    case 6: return psq_dequant((f64)(s8)(u8)raw, scale);
    default: return psq_dequant((f64)(s16)(u16)raw, scale);
    }
}

static GXRUNTIME_ALWAYS_INLINE u32 psq_to_raw(f64 value, u8 type, s32 scale) {
    switch (type) {
    case 0: return convert_to_single_ftz(f64_bits(value));
    case 4: return (u32)(u8)psq_quantize_int(value, 0, 255, scale);
    case 5: return (u32)(u16)psq_quantize_int(value, 0, 65535, scale);
    case 6: return (u32)(u8)(s8)psq_quantize_int(value, -128, 127, scale);
    default: return (u32)(u16)(s16)psq_quantize_int(value, -32768, 32767, scale);
    }
}

static GXRUNTIME_ALWAYS_INLINE u32 psq_read_raw(CPUState* cpu, const u8* direct, u32 ea, u32 size) {
    if (direct)
        return size == 4 ? read_be32(direct) : size == 2 ? read_be16(direct) : direct[0];
    return size == 4 ? mem_read32(cpu, ea) : size == 2 ? mem_read16(cpu, ea) : mem_read8(cpu, ea);
}

static GXRUNTIME_ALWAYS_INLINE void psq_write_raw(CPUState* cpu, u8* direct, u32 ea, u32 size, u32 raw) {
    if (direct) {
        if (size == 4) write_be32(direct, raw);
        else if (size == 2) write_be16(direct, (u16)raw);
        else direct[0] = (u8)raw;
        return;
    }
    if (size == 4) mem_write32(cpu, ea, raw);
    else if (size == 2) mem_write16(cpu, ea, (u16)raw);
    else mem_write8(cpu, ea, (u8)raw);
}

static GXRUNTIME_ALWAYS_INLINE u8* psq_direct(CPUState* cpu, u32 ea, u32 bytes) {
    if (get_ram_ptr(cpu, ea, bytes, NULL) || !cpu->external_pointer)
        return NULL;
    return (u8*)cpu->external_pointer(cpu, ea, bytes);
}

// The quantized load/store with the paired-single lanes as values: generated
// code that keeps guest registers in locals (DolRecomp register cache) passes
// them directly. On an exception nothing is written.
static GXRUNTIME_ALWAYS_INLINE bool ppc_psq_load_to(CPUState* cpu, f64* ps0, f64* ps1, u32 ea, bool w,
                                                    u8 gqr_index, bool indexed, u32 cia) {
    if (!indexed && (cpu->hid2 & PPC_HID2_LSQE) == 0) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return false;
    }
    const u32 gqr = cpu->gqr[gqr_index & 7u];
    if (((gqr >> 16) & 7u) == 0) {
        /* Fast path, same result: single-precision lanes (the scale does not
           apply to floats) straight from RAM; anything else takes the
           general path below. */
        const u8* ram = get_ram_ptr(cpu, ea, w ? 4u : 8u, NULL);
        if (ram) {
            *ps0 = f64_value(convert_to_double(read_be32(ram)));
            *ps1 = w ? 1.0 : f64_value(convert_to_double(read_be32(ram + 4)));
            return true;
        }
    }
    const s32 scale = sign_extend((gqr >> 24) & 0x3Fu, 6);
    const u8 type = (u8)((gqr >> 16) & 7u);
    const u32 size = psq_type_size(type);
    if (size == 0) { /* invalid GQR type: both lanes read as 0.0 */
        *ps0 = 0.0;
        *ps1 = 0.0;
        return true;
    }
    const u8* direct = psq_direct(cpu, ea, w ? size : size * 2);
    *ps0 = psq_from_raw(psq_read_raw(cpu, direct, ea, size), type, scale);
    *ps1 = w ? 1.0 : psq_from_raw(psq_read_raw(cpu, direct ? direct + size : NULL, ea + size, size), type, scale);
    return true;
}

static GXRUNTIME_ALWAYS_INLINE bool ppc_psq_store_from(CPUState* cpu, f64 ps0, f64 ps1, u32 ea, bool w,
                                                       u8 gqr_index, bool indexed, u32 cia) {
    if (!indexed && (cpu->hid2 & PPC_HID2_LSQE) == 0) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return false;
    }
    const u32 gqr = cpu->gqr[gqr_index & 7u];
    if ((gqr & 7u) == 0) {
        /* Fast path, same result: single-precision lanes straight to RAM. */
        u32 offset;
        u8* ram = get_ram_ptr(cpu, ea, w ? 4u : 8u, &offset);
        if (ram) {
            /* As two mem_write32 calls: journal and write lane by lane. */
            if (g_mem_write_journal && offset != (u32)-1)
                g_mem_write_journal(offset, 4, g_mem_write_journal_user);
            write_be32(ram, convert_to_single_ftz(f64_bits(ps0)));
            if (!w) {
                if (g_mem_write_journal && offset != (u32)-1)
                    g_mem_write_journal(offset + 4, 4, g_mem_write_journal_user);
                write_be32(ram + 4, convert_to_single_ftz(f64_bits(ps1)));
            }
            return true;
        }
    }
    const s32 scale = sign_extend((gqr >> 8) & 0x3Fu, 6);
    const u8 type = (u8)(gqr & 7u);
    const u32 size = psq_type_size(type);
    if (size == 0) /* invalid GQR type: nothing is stored */
        return true;
    u8* direct = psq_direct(cpu, ea, w ? size : size * 2);
    psq_write_raw(cpu, direct, ea, size, psq_to_raw(ps0, type, scale));
    if (!w)
        psq_write_raw(cpu, direct ? direct + size : NULL, ea + size, size, psq_to_raw(ps1, type, scale));
    return true;
}

static GXRUNTIME_ALWAYS_INLINE bool ppc_psq_load_inline(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr_index,
                                                        bool indexed, u32 cia) {
    return ppc_psq_load_to(cpu, &cpu->fpr[frD], &cpu->ps1[frD], ea, w, gqr_index, indexed, cia);
}

static GXRUNTIME_ALWAYS_INLINE bool ppc_psq_store_inline(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr_index,
                                                         bool indexed, u32 cia) {
    return ppc_psq_store_from(cpu, cpu->fpr[frS], cpu->ps1[frS], ea, w, gqr_index, indexed, cia);
}

#undef GXRUNTIME_ALWAYS_INLINE

bool cpu_init(CPUState* cpu);
void cpu_free(CPUState* cpu);
void cpu_reset(CPUState* cpu);

f64 ppc_approx_reciprocal(f64 value);
f64 ppc_approx_rsqrt(f64 value);
bool ppc_fres(CPUState* cpu, f64 value, f64* result);
bool ppc_frsqrte(CPUState* cpu, f64 value, f64* result);
void ppc_ps_res(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
void ppc_ps_rsqrte(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
             bool subtract, bool negative, f64* output);
bool ppc_fctiw(CPUState* cpu, f64 value, bool toward_zero, u64* result);

/* Instruction-shaped FP unit mirroring Dolphin's interpreter bit-exactly
 * (Interpreter_FloatingPoint/Paired + Interpreter_FPUtils NI_* semantics:
 * PPC NaN propagation, Force25Bit frC rounding, single-precision Fill of
 * both PS lanes, FPSCR FPRF/FI/FR/exception updates, VE/ZE write gating).
 * Generated code calls these; the register indices select CPUState lanes. */
void ppc_fadds(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fsubs(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmuls(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_fdivs(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fadd(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fsub(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmul(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_fdiv(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmadd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b,
                  bool single, bool subtract, bool negative);
void ppc_frsp(CPUState* cpu, u8 d, u8 b);
void ppc_fres_op(CPUState* cpu, u8 d, u8 b);
void ppc_frsqrte_op(CPUState* cpu, u8 d, u8 b);
void ppc_fctiw_op(CPUState* cpu, u8 d, u8 b, bool toward_zero);
void ppc_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered);
void ppc_ps_add_op(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_sub_op(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_mul_op(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_div_op(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_madd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b,
                    bool subtract, bool negative);
void ppc_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_res_op(CPUState* cpu, u8 d, u8 b);
void ppc_ps_rsqrte_op(CPUState* cpu, u8 d, u8 b);
/* FP loads/stores with Dolphin's alignment exception + lfs/stfs ConvertTo*
 * bit repack. Return false when an exception was taken (callers skip the
 * update-form RA write-back). */
bool ppc_lfs_op(CPUState* cpu, u8 d, u32 ea, u32 cia);
bool ppc_lfd_op(CPUState* cpu, u8 d, u32 ea, u32 cia);
bool ppc_stfs_op(CPUState* cpu, u8 s, u32 ea, u32 cia);
bool ppc_stfd_op(CPUState* cpu, u8 s, u32 ea, u32 cia);
bool ppc_lwarx_op(CPUState* cpu, u8 d, u32 ea, u32 cia);
void ppc_stwcx_op(CPUState* cpu, u8 s, u32 ea, u32 cia);
/* stswi/stswx store-string: mirrors Dolphin's Helper_StoreString word-based
 * read-modify-write of the head/tail partial words. n = byte count, r =
 * first source GPR (wraps). */
void ppc_stsw(CPUState* cpu, u32 ea, u32 n, u8 r, u32 cia);
/* FPSCR control write (mtfsf/mtfsb/mtfsfi/mcrfs): recompute VX/FEX and re-arm
 * the host FPU rounding/flush mode from RN/NI (Dolphin FPSCRUpdated chain). */
void ppc_fpscr_control_updated(CPUState* cpu);
void ppc_mtfsb0_op(CPUState* cpu, u8 bit);
void ppc_mtfsb1_op(CPUState* cpu, u8 bit);

bool ppc_add_overflowed(u32 a, u32 b, u32 result);
bool ppc_trap_condition(u8 to, u32 a, u32 b);
void ppc_set_xer_ov(CPUState* cpu, bool ov);
void ppc_take_exception(CPUState* cpu, u32 exception, u32 vector, u32 srr0, u32 srr1_info);
void ppc_program_exception(CPUState* cpu, u32 cause, u32 cia);

/* Generic mfspr/mtspr dispatch used by generated code for every SPR number
 * (DolRecomp's non-inlining codegen path routes all mfspr/mtspr through
 * these instead of inlining known ones). Known architectural SPRs map to
 * their named CPUState fields; everything else falls back to the spr[]
 * scratch array (or raises an illegal-instruction exception if that SPR
 * isn't in the allow-list). */
u32 ppc_mfspr(CPUState* cpu, u16 spr, u32 cia);
void ppc_mtspr(CPUState* cpu, u16 spr, u32 value, u32 cia);

/* Gekko lazy FP: generated FPU instructions call this first. Returns true
 * when MSR[FP] is set; otherwise raises the FP-unavailable exception (srr0 =
 * cia so the instruction retries after the OS restores the FP context) and
 * returns false. ppc_lazy_fp_set_enabled(false) restores the historical
 * execute-regardless behavior for hosts that eagerly restore FP state
 * themselves (StrikersRecomp standalone; see recomp-codegen.md Lazy FPU). */
bool ppc_fp_available(CPUState* cpu, u32 cia);
void ppc_lazy_fp_set_enabled(bool enabled);
void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia);
bool ppc_host_call(CPUState* cpu, u32 address);
void ppc_system_call_exception(CPUState* cpu, u32 cia);
void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr);
void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia);
u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia);
void ppc_rfi(CPUState* cpu, u32 cia);
void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia);
bool ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
bool ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia);
void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia);
void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia);
void ppc_fpscr_updated(CPUState* cpu);
void ppc_memory_fence(void);

#endif /* DOLRECOMP_CPU_H */
