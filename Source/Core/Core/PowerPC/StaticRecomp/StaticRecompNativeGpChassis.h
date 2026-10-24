// RecompCore: StaticRecomp CPU core - chassis adapter of the native GP front (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Connects the native GP front to the chassis's CommandProcessor FIFO and the
// PI's CPU FIFO registers (`.sav` and the previous path), its dual-core mode
// and its per-frame update. The GP front itself includes no chassis header.

#pragma once

namespace Core
{
class System;
}

namespace StaticRecompNativeGpChassis
{
void Install(Core::System& system);
}
