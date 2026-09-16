#pragma once

// Matches the ScriptHookRDR2 SDK sample convention: script.h is the common
// header that pulls in the native/type/enum declarations plus the script
// registration macros, so anything that includes script.h (scriptmenu.h/cpp
// included) gets UI::/GRAPHICS::/AUDIO::/etc. for free. Same pattern as
// CollectorOffline's script.h.
//
// natives.h here is the canonical allocatr/alloc8or.re-generated header
// (submodule external/ScriptHookSDK, not the stock 2019 SDK dump) --
// project-specific natives missing from it get added to that header
// directly rather than a per-project ExtraNatives.h.
#include "..\external\ScriptHookSDK\inc\natives.h"
#include "..\external\ScriptHookSDK\inc\types.h"
#include "..\external\ScriptHookSDK\inc\enums.h"
#include "..\external\ScriptHookSDK\inc\main.h"

void ScriptMain();
