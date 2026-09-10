/*
	Entry point. Registers ScriptMain as a ScriptHookRDR2 script thread and
	wires up the keyboard handler, same pattern as the SDK's NativeTrainer
	sample and CollectorOffline's main.cpp.
*/

#include "..\..\ScriptHookSDK\inc\main.h"
#include "script.h"
#include "keyboard.h"

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
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
