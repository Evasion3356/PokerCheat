/*
	Entry point. Registers ScriptMain as a ScriptHookRDR2 script thread and
	wires up the keyboard handler, same pattern as the SDK's NativeTrainer
	sample and CollectorOffline's main.cpp.
*/

#include "..\..\ScriptHookSDK\inc\main.h"
#include "script.h"
#include "keyboard.h"
#include "Config.h"
#include "GamePointers.h"

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		// Config::Reload() is called from here, not from ScriptMain's
		// fiber -- there's no actual reason it needs to run on
		// ScriptHookRDR2's cooperative script fiber at all (it doesn't
		// touch any game natives or script-hook state). It's a plain
		// synchronous call (no worker thread -- see Config.cpp/
		// Config.h's header comment for why an earlier, mINI-based
		// version of this file needed one and this one doesn't): just
		// ordinary file I/O via inipp, nothing that touches
		// LoadLibrary/FreeLibrary or otherwise risks the loader lock.
		Config::Reload();

		// Eagerly resolve the live scrThread pool (an AOB scan over
		// RDR2.exe's whole mapped image, cached afterward -- see
		// GamePointers.cpp/PatternScan.cpp) here too, instead of letting
		// it happen lazily on whatever tick first calls
		// GamePointers::FindScriptThread(). That lazy path meant the
		// scan's one-time cost landed on the first "Toggle Poker Cheat"
		// press each session (DrawOverlay() -- the only caller -- only
		// ever runs once Enabled is true), which is exactly what showed
		// up as a hitch there. Safe to do here: RDR2.exe's own image is
		// already fully mapped by the time ANY DllMain in this process
		// runs (the OS maps the whole primary executable before
		// processing DLL imports/entry points at all), this only reads
		// already-resident memory (no file I/O, no thread creation,
		// nothing loader-lock-sensitive), and after PatternScan.cpp's
		// memchr-based rewrite it's fast enough not to meaningfully
		// delay injection.
		GamePointers::GetScriptThreads();

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
