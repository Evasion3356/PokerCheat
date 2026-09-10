#pragma once

// Matches the ScriptHookRDR2 SDK sample convention: script.h is the common
// header that pulls in the native/type/enum declarations plus the script
// registration macros, so anything that includes script.h (scriptmenu.h/cpp
// included) gets UI::/GRAPHICS::/AUDIO::/etc. for free. Same pattern as
// CollectorOffline's script.h.
#include "..\..\ScriptHookSDK\inc\natives.h"
#include "..\..\ScriptHookSDK\inc\types.h"
#include "..\..\ScriptHookSDK\inc\enums.h"
#include "..\..\ScriptHookSDK\inc\main.h"

// Natives missing from (or untyped in) the stock 2019 SDK header go here --
// see ExtraNatives.h. Must come after natives.h (needs invoke<>) and types.h
// (needs Any/Hash/Ped/BOOL).
#include "ExtraNatives.h"

void ScriptMain();
