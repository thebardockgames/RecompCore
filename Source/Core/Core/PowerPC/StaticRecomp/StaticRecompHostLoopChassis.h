// RecompCore: StaticRecomp CPU core - chassis adapter of the emulation thread (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Boot/Stop/Join/Pause/Resume/EmuBody of StaticRecompHostLoop are implemented
// against the chassis in StaticRecompHostLoopChassis.cpp; Install() connects
// the port's process state and job queue to the chassis's Core and CPU state.

#pragma once

namespace StaticRecompHostLoopChassis
{
void Install();
}
