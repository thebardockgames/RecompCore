// RecompCore: StaticRecomp CPU core - chassis adapter of the native checkpoints (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Saves and loads `.mgs` checkpoints with the registers in the chassis's
// PowerPC state and the counter / time base in its CoreTiming facade (where
// they live while the chassis exists). The format itself includes no chassis
// header.

#pragma once

#include <string>

namespace Core
{
class System;
}

namespace StaticRecompNativeStateChassis
{
bool Save(Core::System& system, const std::string& path);
bool Load(Core::System& system, const std::string& path);
}
