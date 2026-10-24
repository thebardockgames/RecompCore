// RecompCore: StaticRecomp CPU core - chassis adapter of the native PI (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Connects the native interrupt controller to the chassis's processor
// interface, Wii IPC GPIO and PowerPC state while the chassis exists (`.sav`
// compatibility, MODERNGEKKO_NATIVE_PI=off, the lines its devices still
// raise). The controller itself includes no chassis header.

#pragma once

namespace Core
{
class System;
}

namespace StaticRecompNativePiChassis
{
void Install(Core::System& system);
}
