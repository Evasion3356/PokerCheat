/*
	PokerCheat -- ScriptHookRDR2 ASI mod that cheats the "poker_sp"
	single-player minigame so the player always wins.

	This session only sets up the mod skeleton (DllMain registration, F10
	menu, logging) -- see PokerCheat.h/.cpp for the (currently empty) cheat
	module and its header comment for the reversing plan, and CLAUDE.md for
	where the already-decompiled poker_sp.ysc.c source lives.

	Press F10 in-game to open the test menu (NUMPAD 8/2 to move, NUMPAD 5 to
	select, NUMPAD 0/Backspace/F10 to back out -- same controls as the
	ScriptHookRDR2 SDK's own NativeTrainer sample menu, which this is built
	on, same as CollectorOffline).
*/

#include "scriptmenu.h" // pulls in script.h (natives/types/enums/main) and keyboard.h
#include "Log.h"
#include "PokerCheat.h"
#include "Config.h"

namespace
{
	MenuController g_menuController;
	MenuBase* g_mainMenu = nullptr;

	void BuildMenu()
	{
		g_mainMenu = new MenuBase(new MenuItemTitle("PokerCheat"));
		g_mainMenu->AddItem(new MenuItemAction("Toggle Poker Cheat (see log)", PokerCheat::Toggle));
		g_mainMenu->AddItem(new MenuItemAction("Probe Table Struct (see log)", PokerCheat::ProbeTableStruct));
		g_mainMenu->AddItem(new MenuItemAction("Probe Community Card Objects (see log)", PokerCheat::ProbeCommunityCardObjects));
		g_mainMenu->AddItem(new MenuItemAction("Toggle Calibration Grid", PokerCheat::ToggleCalibrationGrid));
		g_mainMenu->AddItem(new MenuItemAction("Reload Config (see log)", Config::Reload));
		g_menuController.RegisterMenu(g_mainMenu);
	}
}

void ScriptMain()
{
	Log::Write("PokerCheat started");

	BuildMenu();

	while (true)
	{
		if (!g_menuController.HasActiveMenu() && MenuInput::MenuSwitchPressed())
			g_menuController.PushMenu(g_mainMenu);

		g_menuController.Update();
		PokerCheat::OnTick();

		WAIT(0);
	}
}
