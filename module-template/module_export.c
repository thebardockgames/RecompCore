// RecompCore per-game native module export glue (game id set at build time).
//
// Wraps the DolRecomp-generated constant-time chunk dispatcher behind the
// StaticRecomp module ABI. All environment access goes through the CPUState
// hook pointers the chassis installs; this dylib has no host dependencies.

#include "generated.h"

#include "StaticRecompABI.h"

#include <stdlib.h>

static int chassis_dispatch(CPUState* ctx, u32 address)
{
    return dolrecomp_call(ctx, address);
}

static void chassis_on_state_loaded(CPUState* ctx)
{
    // Re-arm host FP rounding/flush state from the freshly loaded guest FPSCR.
    ppc_fpscr_updated(ctx);
}

#include "module_tables.inc"

static const StaticRecompModuleDesc s_desc = {
    STATICRECOMP_ABI_VERSION,
    GXRUNTIME_CPU_ABI_VERSION,
    (u32)sizeof(CPUState),
    MODULE_GAME_ID,
    DOLRECOMP_ENTRY_POINT,
    chassis_dispatch,
    chassis_on_state_loaded,
    s_code_ranges,
    MODULE_CODE_RANGE_COUNT,
    s_smc_ranges,
    MODULE_SMC_RANGE_COUNT,
    s_chunk_ranges,
    MODULE_CHUNK_RANGE_COUNT,
    s_chunk_hashes,
};

#if defined(_WIN32)
#define RECOMP_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RECOMP_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define RECOMP_MODULE_EXPORT
#endif

#ifdef DOLRECOMP_FAST_FUNCTIONS
// Fast code indexes the chassis SMC state by chunk; that index must be the
// chunk-table order the chassis sees. Enable fast code only when both tables
// agree exactly, and allow an A/B switch without rebuilding the module.
static int fast_functions_usable(void)
{
    u32 i;
    if (getenv("MODERNGEKKO_NATIVE_DISABLE_FAST_FUNCTIONS") != NULL)
        return 0;
    if (DOLRECOMP_FAST_CHUNK_COUNT != MODULE_CHUNK_RANGE_COUNT)
        return 0;
    for (i = 0; i < MODULE_CHUNK_RANGE_COUNT; ++i)
    {
        if (dolrecomp_fast_chunk_starts[i] != s_chunk_ranges[i].start)
            return 0;
    }
    return 1;
}
#endif

#if defined(DOLRECOMP_FAST_FUNCTIONS) && defined(DOLRECOMP_ENTRY_COUNTERS)
// Diagnostic builds only: write per-function entry counts when the module
// unloads, to MODERNGEKKO_NATIVE_ENTRY_COUNTS.
static void write_entry_counts(void)
{
    const char* path = getenv("MODERNGEKKO_NATIVE_ENTRY_COUNTS");
    FILE* file;
    u32 i;
    if (!path || !(file = fopen(path, "w")))
        return;
    fprintf(file, "address,entries\n");
    for (i = 0; i < DOLRECOMP_FAST_REGION_COUNT; ++i)
    {
        if (dolrecomp_entry_counts[i] == 0)
            continue;
        // Region start addresses are the first entry of each fast function.
        fprintf(file, "%08X,%llu\n", dolrecomp_fast_region_starts[i], dolrecomp_entry_counts[i]);
    }
    fclose(file);
}
#endif

#ifdef DOLRECOMP_NATIVE_HOOKS
// Guest functions the chassis may implement natively. Setting a flag makes
// native calls into that function exit to the dispatcher (see DolRecomp
// fastplan.h); `any` gates the lookup used by dynamic calls.
RECOMP_MODULE_EXPORT u32 staticrecomp_native_hooks(const u32** addresses, unsigned char** flags,
                                                   int** any)
{
    *addresses = dolrecomp_native_hook_addresses;
    *flags = dolrecomp_native_hooks;
    *any = &dolrecomp_native_hooks_any;
    return DOLRECOMP_NATIVE_HOOK_COUNT;
}
#endif

RECOMP_MODULE_EXPORT const StaticRecompModuleDesc* staticrecomp_get_module(void)
{
#if defined(DOLRECOMP_FAST_FUNCTIONS) && defined(DOLRECOMP_ENTRY_COUNTERS)
    static int counters_registered;
    if (!counters_registered)
    {
        counters_registered = 1;
        atexit(write_entry_counts);
        fprintf(stderr, "[staticrecomp] diagnostic entry counters enabled\n");
    }
#endif
#ifdef DOLRECOMP_FAST_FUNCTIONS
    dolrecomp_fast_enabled = fast_functions_usable();
    fprintf(stderr, "[staticrecomp] module fast functions: %s (%u regions)\n",
            dolrecomp_fast_enabled ? "enabled" : "disabled", DOLRECOMP_FAST_REGION_COUNT);
#endif
    return &s_desc;
}
