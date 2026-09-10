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

	Backed by mINI (external/mINI, vendored as a git submodule --
	https://github.com/metayeti/mINI), not the raw Win32
	GetPrivateProfileString/WritePrivateProfileString API this started
	on: that API rewrites/rescans the whole file on every single key
	access and caches writes without a guaranteed immediate flush to
	disk, which was the real cause of a hitch (and an INI that didn't
	reliably appear on disk) the first time the HUD was toggled on. mINI
	reads and writes the whole file in one shot instead.
*/

#pragma once

namespace Config
{
	struct Values
	{
		// Independent per-feature toggles -- replaces an earlier "cheat
		// level" tier idea (Little/Lot/Full Tilt) with plain booleans so
		// each one can be turned on/off on its own.
		bool ShowCommunityCards = true;
		bool ShowOthersCards = true;
		bool ShowWinPrediction = true;

		// HUD text panel (the seat/board/verdict text block).
		float PanelX = 0.015f;
		float PanelY = 0.30f;
		float TextScale = 0.32f;
		float TitleTextScale = 0.38f;

#ifdef _DEBUG
		// 2D community-card icon strip, top-right of screen -- see
		// PokerCheat.cpp's DrawCommunityCardIcons() header comment for
		// how these were calibrated. Debug-only: these are dev-tuning
		// values, not something an end user should need to calibrate, so
		// Release doesn't read/write this INI section at all and instead
		// draws with the fixed values baked in directly (see
		// DrawCommunityCardIcons()'s Release branch) -- the same
		// user-confirmed "perfect" numbers below, just not
		// config-overridable outside Debug.
		float Card2DIconBaseX = 0.821f;
		float Card2DIconY = 0.078f;
		float Card2DIconSpacingX = 0.034f;
		float Card2DIconWidth = 0.03f;
		float Card2DIconHeight = 0.075f;
#endif
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
