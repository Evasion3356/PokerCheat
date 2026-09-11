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

	Backed by inipp (external/inipp, vendored as a git submodule --
	https://github.com/mcmtroffaes/inipp), the second library tried here.
	The first, mINI, was reverted after multiple real crashes traced back
	to its <filesystem> dependency (a stack overflow when its heavy
	locale/codecvt machinery ran on ScriptHookRDR2's small fiber stack,
	and separately a crash inside its own std::unordered_map lookup) --
	see docs/JOURNAL.md for the full chase. inipp parses/generates over
	plain std::istream/std::ostream (no <filesystem>, no exceptions
	thrown anywhere in its header, std::map instead of
	std::unordered_map), and this file opens the actual file itself via
	MSVC's wide-char ifstream/ofstream constructor overloads -- avoiding
	the narrow/wide conversion issue entirely rather than working around
	it.

	Reload() is a plain synchronous call, same as any ordinary function --
	no worker thread. An earlier version of this file (while still on
	mINI) ran the actual load on a dedicated worker thread specifically
	to get <filesystem>'s stack-heavy machinery off ScriptHookRDR2's
	small fiber stack; inipp doesn't touch <filesystem> at all, so that
	concern doesn't apply here and the threading complexity (worker
	stack size, a mutex to serialize overlapping reloads, atomics for
	safe cross-thread publish) was removed along with it.
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

	// Returns the current config, triggering the very first load
	// automatically on first call (blocking, like any ordinary function
	// call -- see Reload()).
	const Values& Get();

	// Re-reads PokerCheat.ini from disk, replacing the cached values.
	// Wired to the F10 menu's "Reload Config" item; also called once,
	// eagerly, from DllMain (see main.cpp) so the file exists as soon as
	// the ASI is injected.
	void Reload();
}
