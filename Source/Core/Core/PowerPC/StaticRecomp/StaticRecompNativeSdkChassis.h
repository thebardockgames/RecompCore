// RecompCore: StaticRecomp CPU core - chassis adapter of the native SDK layer (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Connects the native SDK layer (PAD verification, audio, /dev/fs) to the
// chassis's serial interface latch, DSP and audio interface, IOS kernel and
// file system while the chassis exists. The SDK layer itself includes no
// chassis header.

#pragma once

namespace Core
{
class System;
}

namespace StaticRecompNativeSdkChassis
{
void Install(Core::System& system);
}
