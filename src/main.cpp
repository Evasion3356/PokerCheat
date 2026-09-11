/*
	Entry point. Registers ScriptMain as a ScriptHookRDR2 script thread and
	wires up the keyboard handler, same pattern as the SDK's NativeTrainer
	sample and CollectorOffline's main.cpp.
*/

#include "..\..\ScriptHookSDK\inc\main.h"
#include "script.h"
#include "keyboard.h"
#include "Config.h"

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
		scriptRegister(hInstance, ScriptMain);
		keyboardHandlerRegister(OnKeyboardMessage);
		break;
	case DLL_PROCESS_DETACH:
		scriptUnregister(hInstance);
		keyboardHandlerUnregister(OnKeyboardMessage);
		break;
	}
	return TRUE;
}
