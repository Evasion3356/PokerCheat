#pragma once

// Natives that alloc8or's DB (https://alloc8or.re/rdr3/nativedb/), and so the
// generated external/ScriptHookSDK/inc/natives.h, doesn't have (or types too
// loosely to call), declared in their usual namespace. If alloc8or has a
// hash with a usable signature, call its name instead. Never edit the
// vendored SDK header. All entries checked on 2026-10-07.

#include "..\external\ScriptHookSDK\inc\nativeCaller.h"
#include "..\external\ScriptHookSDK\inc\types.h"

namespace HUD
{
	// Old stock-SDK UI:: text natives; alloc8or has none of these hashes.
	inline void SET_TEXT_SCALE(float unk, float scale) { invoke<Void>(0x4170B650590B3B00, unk, scale); }
	inline void SET_TEXT_CENTRE(BOOL align) { invoke<Void>(0xBE5261939FBECB8C, align); }
	inline void SET_TEXT_DROPSHADOW(int distance, int r, int g, int b, int a) { invoke<Void>(0x1BE39DBAA7263CA5, distance, r, g, b, a); }
}
