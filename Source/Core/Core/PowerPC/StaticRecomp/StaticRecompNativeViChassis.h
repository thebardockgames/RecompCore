// RecompCore: StaticRecomp CPU core - chassis adapter of the native VI (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Connects the native video interface to the chassis's VideoInterface
// (`.sav` and the previous path), its half-line event, movie and field
// callbacks, the presenter of its renderer, its graphics hacks, its limiter
// and frame-end bookkeeping. The VI itself includes no chassis header.

#pragma once

namespace Core
{
class System;
}

namespace StaticRecompNativeViChassis
{
void Install(Core::System& system);
}
