/*
	Copied from the ScriptHookRDR2 SDK's NativeTrainer sample (Alexander Blade,
	http://dev-c.com). One addition: IsKeyWithAlt(), for the bet hotkeys'
	Alt+Tab check. Same file CollectorOffline vendors.
*/

#pragma once

#include <windows.h>

void OnKeyboardMessage(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow);

bool IsKeyDown(DWORD key);
bool IsKeyDownLong(DWORD key);
bool IsKeyJustUp(DWORD key, bool exclusive = true);
void ResetKeyState(DWORD key);

// Whether Alt was held at the key's last message (the lParam context bit
// Windows sets per event, so it can't go stale the way Alt's own state can).
bool IsKeyWithAlt(DWORD key);
