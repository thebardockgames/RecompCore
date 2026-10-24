// RecompCore: StaticRecomp CPU core - chassis adapter of the native clock (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Connects the native clock to the chassis's CoreTiming (the facade that
// forwards its scheduling, counter and slice loop here, its events and the
// bridge event of the bisection mode), its SystemTimers (decrementer
// forwarding, time base, clock rate), its configuration, performance
// metrics, GPU flush and PowerPC state. The clock itself includes no chassis
// header.

#pragma once

namespace Core
{
class System;
}

namespace StaticRecompNativeClockChassis
{
void Install(Core::System& system);
}
