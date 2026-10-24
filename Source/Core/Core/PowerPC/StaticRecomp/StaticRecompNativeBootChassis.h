// RecompCore: StaticRecomp CPU core - chassis adapter of the native boot (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Runs the native boot for the chassis's boot hook: its configuration (region
// override, running-title metadata), its IOS object (network defaults, the
// title's IOS for its savestates) and the CPU registers put on its PowerPC
// state. Checks the boot's own constants (IOS memory values, MSR/HID bits)
// against the chassis's. The boot itself includes no chassis header.

#pragma once

namespace Core
{
class System;
}

namespace StaticRecompNativeBoot
{
struct BootPaths;
}

namespace StaticRecompNativeBootChassis
{
bool Boot(Core::System& system, const StaticRecompNativeBoot::BootPaths& paths);
}
