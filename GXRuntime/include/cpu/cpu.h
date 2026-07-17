#pragma once
/* DolRecomp's emitted C always includes "cpu/cpu.h" (see DolRecomp's
 * src/backend/emitter.c), but GXRuntime exposes this header under
 * include/core/cpu.h instead. This shim bridges the two so generated
 * chunks compile against GXRuntime without patching DolRecomp's emitter
 * or GXRuntime's own layout. */
#include "core/cpu.h"
