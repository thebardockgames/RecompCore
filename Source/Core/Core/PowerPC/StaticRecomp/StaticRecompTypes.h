// RecompCore: StaticRecomp CPU core - the port's basic types (phase S).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The fixed-width integer names and the host clock aliases the runtime uses.
// They are the same aliases the chassis declares (a redeclaration of an alias
// to the same type is allowed), so the port's code does not need the
// chassis's header for them.

#pragma once

#include <chrono>
#include <cstdint>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using DT = Clock::duration;
