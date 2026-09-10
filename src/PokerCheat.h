#pragma once

// Poker advisor: reads live hole cards for every seat + the board directly
// out of poker_sp's own script memory, and calls the game's own hand-rank
// native on them to draw an on-screen HUD showing everyone's cards and
// hand strength. See the header comment in PokerCheat.cpp for the full
// confirmed struct layout and docs/JOURNAL.md for how it was derived.
namespace PokerCheat
{
	extern bool Enabled;

	// Flips Enabled and logs the new state. Wired to the F10 menu's
	// "Toggle Poker Cheat" item -- when on, draws the card/hand-strength
	// HUD every tick via OnTick(); when off, does nothing.
	void Toggle();

	// Flips a diagnostic screen-space coordinate grid on/off (thin lines
	// every 0.05, labeled every 0.1 along the top/left edges, normalized
	// 0-1 UI coordinates). Wired to the F10 menu's "Toggle Calibration
	// Grid" item -- draws regardless of Enabled/poker state, so it can be
	// compared against the real seat-panel HUD positions to pin down
	// where to draw card icons over them, without needing this file's
	// author to see the screen. See docs/JOURNAL.md.
	void ToggleCalibrationGrid();

	// Called every ScriptMain tick regardless of Enabled state (Enabled is
	// checked internally) -- same convention as CollectorOffline's
	// MetalDetector::Update(). Draws the HUD when Enabled and poker_sp is
	// running; no-ops otherwise.
	void OnTick();

	// Diagnostic: finds poker_sp's running scrThread and logs the
	// confirmed Table struct's key fields (board header/reveal count,
	// seats header, every seat's hole cards) to PokerCheat.log. Wired to
	// the F10 menu's "Probe Table Struct" item -- useful for re-verifying
	// the struct layout if a game update shifts it.
	void ProbeTableStruct();

	// Diagnostic: tests whether poker_sp's own community-card reveal
	// (func_471) creates a real 3D object per board slot whose handle we
	// can read and project to screen ourselves (see PokerCheat.cpp's
	// kSceneSlot header comment) -- the mechanism this mod uses to draw
	// card icons at the community cards' exact real screen position
	// instead of a guessed/calibrated one. Wired to the F10 menu's
	// "Probe Community Card Objects" item -- run with at least the flop
	// revealed so there's a real object to test against.
	void ProbeCommunityCardObjects();
}
