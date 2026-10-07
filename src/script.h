#pragma once

// Matches the ScriptHookRDR2 SDK sample convention: script.h is the common
// header that pulls in the native/type/enum declarations plus the script
// registration macros, so anything that includes script.h (scriptmenu.h/cpp
// included) gets HUD::/GRAPHICS::/AUDIO::/etc. for free. Same pattern as
// CollectorOffline's script.h.
//
// natives.h here is generated from the allocatr/alloc8or.re native DB by
// the fork's tools/gen_natives.py (submodule external/ScriptHookSDK, not
// the stock 2019 SDK dump), with nothing merged in. Natives it lacks go in
// src/ExtraNatives.h.
#include "..\external\ScriptHookSDK\inc\natives.h"
#include "..\external\ScriptHookSDK\inc\types.h"
#include "..\external\ScriptHookSDK\inc\enums.h"
#include "..\external\ScriptHookSDK\inc\main.h"
#include "ExtraNatives.h"

void ScriptMain();
