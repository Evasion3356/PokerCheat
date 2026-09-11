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

		// (You Win)/(They Win)/(Tie) label drawn under each opponent's
		// seat card icons (see DrawSeatCardIcons() in PokerCheat.cpp) --
		// separate toggle from ShowWinPrediction, which only gates the
		// equivalent tag in the debug text panel.
		bool ShowWouldWinHandAgainst = true;

		// HUD text panel (the seat/board/verdict text block).
		float PanelX = 0.015f;
		float PanelY = 0.30f;
		float TextScale = 0.32f;
		float TitleTextScale = 0.38f;

		// Standalone win/lose/tie status readout (gated by
		// ShowWinPrediction) -- see PokerCheat.cpp's
		// DrawWinPredictionStatus() header comment. Independent of the
		// text panel entirely, not an offset from it -- an absolute
		// position, same convention as PanelX/PanelY. Default is a rough
		// "center of the screen" starting point (per user request, "for
		// now"), left-aligned rather than truly centered (no
		// SET_TEXT_CENTRE equivalent exists for the real-font pipeline
		// this uses -- see that function's header comment), so nudge
		// WinPredictionX left/right to actually center a given label.
		float WinPredictionX = 0.48f;
		float WinPredictionY = 0.5f;

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

		// Opponent hole-card icons, one pair per occupied seat other than
		// the player -- see PokerCheat.cpp's DrawSeatCardIcons() header
		// comment for why these are indexed by RELATIVE seat offset
		// ((mySeat - seat + 6) % 6, 0 = you) rather than raw seat index --
		// confirmed via a live report, see that comment for the direction.
		//
		// User-reported real layout (not a table-fan arrangement like
		// this file first guessed): the vanilla per-seat panels are a
		// single VERTICAL LIST at the bottom-left of the screen, reading
		// bottom-to-top -- Arthur's own panel (relative offset 0) is the
		// bottom-most entry, ~50/50px in from the bottom-left corner at
		// the user's reported 2560x1440 output resolution, then each
		// further seat stacks ~200px higher, repeating. Since every row
		// sits at the same X and steps by a constant Y, this is a base
		// position (BaseX/BaseY, for relative offset/dense row 1 -- the
		// first opponent row above you) plus a per-row Y step, not a
		// per-row lookup table. Converted to this file's normalized 0-1
		// screen space (x/width, y/height, matching every other
		// calibrated value in this file):
		// Initial pixel-derived estimate was BaseX=0.0195/BaseY=0.8264/
		// StepY=-0.1389 -- confirmed and refined by the user via live
		// Reload Config tuning to the values below (BaseX shifted right
		// so icons sit next to the name text instead of overlapping it;
		// StepY/SpacingX/Width/Height all hand-tuned against the real
		// panels).
		float SeatCardIconBaseX = 0.18f;
		float SeatCardIconBaseY = 0.83f;
		float SeatCardIconStepY = -0.09f;
		float SeatCardIconSpacingX = 0.02f;
		float SeatCardIconWidth = 0.02f;
		float SeatCardIconHeight = 0.045f;

		// (You Win)/(They Win)/(Tie) label position, as an offset from
		// the icon pair's own position (icon left edge + LabelOffsetX,
		// icon bottom edge + LabelOffsetY) -- was hardcoded (0, height +
		// 0.006f) before this was made tunable.
		float SeatCardIconLabelOffsetX = -0.01f;
		float SeatCardIconLabelOffsetY = -0.02f;
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
