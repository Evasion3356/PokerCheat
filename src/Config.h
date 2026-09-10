/*
	Lightweight INI-backed config for the tunables that have been getting
	guessed-then-eyeball-corrected through play testing (HUD panel
	position, text scale, the 2D community-card icon strip's position/
	spacing/size) -- previously each tweak meant editing a constant,
	rebuilding, closing the game, redeploying, and relaunching just to
	see one number change. Reload() re-reads the file without any of
	that, wired to the F10 menu's "Reload Config" item: edit
	PokerCheat.ini while the game is running, hit the menu item, see the
	new values immediately.

	Uses the plain Win32 GetPrivateProfileString/WritePrivateProfileString
	API rather than a vendored parser -- this is exactly the built-in tool
	for a Windows-only INI file and needs no extra dependency.
*/

#pragma once

namespace Config
{
	struct Values
	{
		// HUD text panel (the seat/board/verdict text block).
		float PanelX = 0.015f;
		float PanelY = 0.30f;
		float TextScale = 0.32f;
		float TitleTextScale = 0.38f;

		// 2D community-card icon strip, top-right of screen -- see
		// PokerCheat.cpp's DrawCommunityCardIcons() header comment for
		// how these were calibrated. Values below are the user-confirmed
		// "perfect" ones from direct in-game tuning via Reload Config,
		// not the original grid/TEST-ICON estimate that seeded them.
		float Card2DIconBaseX = 0.821f;
		float Card2DIconY = 0.078f;
		float Card2DIconSpacingX = 0.034f;
		float Card2DIconWidth = 0.03f;
		float Card2DIconHeight = 0.075f;
	};

	// Returns the current config, loading it from PokerCheat.ini (next to
	// the .asi) on first call. Any key missing from the file falls back
	// to Values{}'s default and gets written back, so the file is
	// self-documenting -- a fresh PokerCheat.ini appears with every
	// tunable spelled out the first time the mod runs.
	const Values& Get();

	// Re-reads PokerCheat.ini from disk, replacing the cached values.
	// Wired to the F10 menu's "Reload Config" item.
	void Reload();
}
