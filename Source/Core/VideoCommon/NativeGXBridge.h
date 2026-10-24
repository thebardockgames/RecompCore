// The native GX host moved to the port (Core/PowerPC/StaticRecomp/
// StaticRecompNativeGx.h, phase S). This header keeps the chassis's callers
// (Fifo, OpcodeDecoding, VideoState, NativeGXVerify) compiling and declares
// the chassis adapter that installs Dolphin's hooks into the host.
#pragma once

#include "Core/PowerPC/StaticRecomp/StaticRecompNativeGx.h"

namespace NativeGXBridge
{
using namespace StaticRecompNativeGx;

// Installs the chassis's hooks (GX state export/import for .sav, the pixel
// engine when the native GP front is off, the frame event, PNG writer,
// legacy .mgs video section). Idempotent; VideoBackendBase::InitializeShared.
void InstallChassisAdapter();
}  // namespace NativeGXBridge
