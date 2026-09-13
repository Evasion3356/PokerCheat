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
#include "Localization.h"

namespace
{
#ifdef _DEBUG
	MenuController g_menuController;
	MenuBase* g_mainMenu = nullptr;

	// Re-picks the active HUD language immediately after an ini edit,
	// same live-tuning workflow as every other Config-backed value here
	// -- Localization::Refresh() itself can't just be called from
	// Config::Reload() directly (see Localization.h's header comment):
	// it invokes a real game native, so it must run from inside
	// ScriptHookRDR2's script fiber, same as this menu action already
	// does.
	void ReloadConfigAndLocalization()
	{
		Config::Reload();
		Localization::Refresh();
	}

	void BuildMenu()
	{
		g_mainMenu = new MenuBase(new MenuItemTitle("PokerCheat"));
		g_mainMenu->AddItem(new MenuItemAction("Toggle Poker Cheat (see log)", PokerCheat::Toggle));
		g_mainMenu->AddItem(new MenuItemAction("Dump Local Stack Range (see log)", PokerCheat::DumpLocalStackRange));
		g_mainMenu->AddItem(new MenuItemAction("Probe Table Struct (see log)", PokerCheat::ProbeTableStruct));
		g_mainMenu->AddItem(new MenuItemAction("Probe Seat Occupancy (see log)", PokerCheat::ProbeSeatOccupancy));
		g_mainMenu->AddItem(new MenuItemAction("Probe Community Card Objects (see log)", PokerCheat::ProbeCommunityCardObjects));
		g_mainMenu->AddItem(new MenuItemAction("Toggle Calibration Grid", PokerCheat::ToggleCalibrationGrid));
		g_mainMenu->AddItem(new MenuItemAction("Toggle Font Test", PokerCheat::ToggleFontTest));
		g_mainMenu->AddItem(new MenuItemAction("Cycle Font Test Language (see log)", PokerCheat::CycleFontTestLanguage));
		g_mainMenu->AddItem(new MenuItemAction("Dump Full Stack JSONL", PokerCheat::DumpFullStackJsonl));
		g_mainMenu->AddItem(new MenuItemAction("Reload Config (see log)", ReloadConfigAndLocalization));
		g_menuController.RegisterMenu(g_mainMenu);
	}
#endif
}

void ScriptMain()
{
	Log::Write("PokerCheat started");

	// Config is loaded from DllMain now, not here -- see main.cpp.

#ifdef _DEBUG
	// Debug-only -- the F10 test menu (toggle, probes, calibration/font
	// test tools) is a dev-tuning surface, not something an end user
	// should ever see. Release has no menu to toggle the cheat from at
	// all, so it enables itself unconditionally below instead.
	BuildMenu();
#else
	// No menu in Release to flip this from, so start already polling.
	// SetEnabled(true), not Toggle() -- idempotent against ScriptMain
	// ever being re-entered (see PokerCheat.h's SetEnabled comment).
	PokerCheat::SetEnabled(true);
#endif

	while (true)
	{
#ifdef _DEBUG
		if (!g_menuController.HasActiveMenu() && MenuInput::MenuSwitchPressed())
			g_menuController.PushMenu(g_mainMenu);

		g_menuController.Update();
#endif
		PokerCheat::OnTick();

		WAIT(0);
	}
}
