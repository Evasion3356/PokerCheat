/*
	Entry point. Registers ScriptMain as a ScriptHookRDR2 script thread and
	wires up the keyboard handler, same pattern as the SDK's NativeTrainer
	sample and CollectorOffline's main.cpp.
*/

#include "..\external\ScriptHookSDK\inc\main.h"
#include "script.h"
#include "keyboard.h"

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		// Nothing but registration here. Config::Reload() (file I/O) and
		// GamePointers::GetScriptThreads() (a scan of RDR2.exe's whole image)
		// used to run here to keep their one-time cost off the first
		// "Toggle Poker Cheat" press, but DllMain runs under the loader lock
		// and, with an early ASI loader, before RDR2.exe has finished
		// unpacking -- a failed scan was then cached for the whole session.
		// ScriptMain does both first thing instead (see script.cpp), which
		// still keeps the cost off the first toggle.
		scriptRegister(hInstance, ScriptMain);
#ifdef _DEBUG
		// Release has no menu to drive with keystrokes at all (see
		// script.cpp) -- registering this hook there would just mean
		// every keypress in the game writes into keyboard.cpp's key-state
		// array for absolutely nothing to ever read. Debug-only.
		keyboardHandlerRegister(OnKeyboardMessage);
#endif
		break;
	case DLL_PROCESS_DETACH:
		scriptUnregister(hInstance);
#ifdef _DEBUG
		keyboardHandlerUnregister(OnKeyboardMessage);
#endif
		break;
	}
	return TRUE;
}
