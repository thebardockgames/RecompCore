// Equivalence check between Dolphin's GX command decoder and the native one
// (moderngekko::native_gx, phase 1 of tools/NATIVE_RENDERER_PLAN.md).
//
// Enabled with MODERNGEKKO_NATIVE_GX_VERIFY=1. Dolphin's decoder callbacks
// record what they decoded; after each top-level command Dolphin delimited, the
// native decoder decodes the same bytes (following display lists in guest RAM
// itself) and both event sequences must match, including vertex sizes and a
// hash of every payload. Counters and the first mismatches go to stderr as
// "[native_gx_verify]" lines. Diagnostic only: not active by default.
#pragma once

#include "Common/CommonTypes.h"

namespace NativeGXVerify
{
bool IsEnabled();

// Reference events from Dolphin's decoder (GPU thread, not during frame
// interpolation replays). Data pointers are the bytes Dolphin decoded.
void RefCp(u8 reg, u32 value);
void RefXf(u16 address, u8 count, const u8* data);
void RefIndexed(u8 array, u32 index, u16 address, u8 size);
void RefBp(u8 reg, u32 value);
void RefPrimitive(u8 primitive, u8 vat, u32 vertex_size, u16 count, const u8* data);
// Dolphin's loaded vertices for the primitive just recorded (runs Dolphin's
// vertex loader into a scratch buffer; call before the real RunVertices).
void RefPrimitiveVertices(u8 vat, u16 count, const u8* data);
void RefDisplayList(u32 address, u32 size, bool nested);
void RefNop(u32 count);
void RefUnknown(u8 opcode);

// A top-level command Dolphin finished decoding (after any display list it
// called). Decodes it with the native decoder and compares.
void OnTopLevelCommand(const u8* data, u32 size);

// Real XFB copy: frame boundary for the periodic summary.
void OnXfbCopy();
}  // namespace NativeGXVerify
