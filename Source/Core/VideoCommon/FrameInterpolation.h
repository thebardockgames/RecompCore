// Copyright 2026 bt3recompiled
// SPDX-License-Identifier: GPL-2.0-or-later

// Frame interpolation prototype for fixed-timestep games (game logic stays at
// its native rate; an extra image is rendered between two logic frames).
//
// Each frame's top-level GX command stream is recorded together with a copy of
// every display list it calls (games rebuild many of them every frame) and
// the video state at the start of the frame. At the frame's XFB copy the
// recorded stream is executed again from that start state, producing a second
// image for the same frame; the presenter shows it on the first VI field and
// the real image on the second.
//
// Modes (MODERNGEKKO_FRAME_INTERPOLATION):
//   replay      re-executes the frame unchanged: the extra image must equal the
//               real one (validates recording, state restore and side effects).
//   interpolate blends each transform load (modelview/normal/texture/post
//               matrices, projection) halfway with the matching load of the
//               previous frame; unmatched or implausible pairs stay unchanged.
// MODERNGEKKO_FRAME_INTERPOLATION_VERIFY=1 compares both images every 60 frames.

#pragma once

#include "Common/CommonTypes.h"

namespace FrameInterpolation
{
enum class Mode
{
  Off,
  Replay,
  Interpolate,
};

Mode GetMode();
// Set by the embedding runtime before video starts. The diagnostic
// environment variable MODERNGEKKO_FRAME_INTERPOLATION takes precedence.
void SetMode(Mode mode);
bool IsActive();
bool IsReplaying();

// Called by the opcode decoder for every executed command outside display
// lists (after it ran).
void RecordCommand(const u8* data, u32 size);

// Display list call: records a copy (normal execution) or returns the recorded
// copy to execute (replay). Returns the pointer to execute, or nullptr.
const u8* OnDisplayListCall(const u8* ram_data, u32 address, u32 size);
void OnDisplayListEnd();

// XF load: records transform loads (normal execution) or returns the data to
// load while replaying (blended, or the original pointer).
const u8* FilterXF(u16 address, u8 count, const u8* data);

// Primitive vertex data: records direct positions (normal execution) or
// returns the vertex data to draw while replaying (positions blended with the
// matching primitive of the previous frame, or the original pointer).
const u8* FilterVertices(u8 vat, u32 vertex_size, u16 num_vertices, const u8* data);
// After the vertex loader converted that primitive: restores guest RAM that
// FilterVertices patched with blended array positions.
void AfterVertices();

// BP registers whose side effects reach the emulated CPU (draw-done, tokens)
// must not fire again while replaying.
bool SuppressBPWrite(u8 reg);

// One VI field was presented (counts how long the previous frame stayed on
// screen: games that already run at the field rate get no extra image).
void OnViField();

// After the real XFB copy of a frame (and its EFB clear) has executed.
void OnXfbCopied(u8 copy_reg, u32 copy_value, u32 dest_addr, u32 dest_stride, u32 height,
                 bool in_display_list);
}  // namespace FrameInterpolation
