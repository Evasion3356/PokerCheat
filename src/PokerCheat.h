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

	// Sets Enabled directly (idempotent, unlike Toggle()). Release's
	// ScriptMain calls this instead of Toggle() -- see script.cpp -- so
	// that if ScriptHookRDR2 ever re-enters ScriptMain (observed live in
	// BlackjackCheat.log and PokerCheat.log both: a second "started" log
	// line minutes after the first, at the identical timestamp in both
	// mods' logs, pointing at a global script-VM restart rather than
	// either mod's own bug), the advisor doesn't get silently toggled
	// back off.
	void SetEnabled(bool enabled);

	// Called every ScriptMain tick regardless of Enabled state (Enabled is
	// checked internally) -- same convention as CollectorOffline's
	// MetalDetector::Update(). Draws the HUD when Enabled and poker_sp is
	// running; no-ops otherwise.
	void OnTick();

#ifdef _DEBUG
	// Everything below is wired to the F10 test menu only (see
	// script.cpp's BuildMenu(), also Debug-only) -- dev-tuning/reversing
	// tools with no caller at all in a Release build, so they don't exist
	// there.

	// Flips a diagnostic screen-space coordinate grid on/off (thin lines
	// every 0.05, labeled every 0.1 along the top/left edges, normalized
	// 0-1 UI coordinates). Wired to the F10 menu's "Toggle Calibration
	// Grid" item -- draws regardless of Enabled/poker state, so it can be
	// compared against the real seat-panel HUD positions to pin down
	// where to draw card icons over them, without needing this file's
	// author to see the screen. See docs/JOURNAL.md.
	void ToggleCalibrationGrid();

	// Diagnostic: draws a handful of comparison text lines testing
	// whether RDR2's "COLOR_STRING" text-template type (as opposed to
	// "LITERAL_STRING", which every other line this mod draws uses) and
	// embedded Scaleform-style HTML tags (<FONT FACE=...>, <B>, etc.)
	// actually render differently -- i.e. whether a real RDR2 font/style
	// is reachable from GAMEPLAY::CREATE_STRING at all, as opposed to
	// LITERAL_STRING's single fixed font every other line in this file is
	// stuck with. See docs/JOURNAL.md for the research trail (forum
	// reports only, nothing independently confirmed working yet -- this
	// is what actually settles it). Wired to the F10 menu's "Toggle Font
	// Test" item -- draws regardless of Enabled/poker state, same as the
	// calibration grid, so it can be checked anywhere in-game.
	void ToggleFontTest();

	// Diagnostic: finds poker_sp's running scrThread and logs the
	// confirmed Table struct's key fields (board header/reveal count,
	// seats header, every seat's hole cards) to PokerCheat.log. Wired to
	// the F10 menu's "Probe Table Struct" item -- useful for re-verifying
	// the struct layout if a game update shifts it.
	void ProbeTableStruct();

	// Diagnostic: logs poker_sp's script-local stack's start/end absolute
	// addresses (thread->m_Stack, and +m_StackSize*8 for the end -- same
	// 8-bytes-per-slot addressing GamePointers::ReadScriptLocal uses),
	// plus uLocal_14's own absolute address within that range -- meant to
	// be pasted straight into Cheat Engine (Memory View's "Browse Memory"
	// jump-to-address, or a manually-added address range) for live,
	// visual memory analysis/scanning, instead of only being able to
	// probe one guessed offset at a time through this mod's own F10
	// tools. Wired to the F10 menu's "Dump Local Stack Range" item.
	void DumpLocalStackRange();

	// Diagnostic: dumps, for every seat (0-5), everything the seat card
	// icon drawing logic (DrawSeatCardIcons(), fed by
	// ComputeDenseRowForSeat()) actually decides based on -- raw occupancy
	// marker, fold/all-in state, stack/bet, hole card rank/suit (and
	// whether they read as valid), and the exact dense row it gets
	// assigned. Built specifically to debug a live report of extra/
	// duplicate card sets being drawn: this shows precisely which seats
	// the drawing logic believes are occupied and why, instead of
	// guessing from the symptom alone. Wired to the F10 menu's "Probe
	// Seat Occupancy" item.
	void ProbeSeatOccupancy();

	// Diagnostic: tests whether poker_sp's own community-card reveal
	// (func_471) creates a real 3D object per board slot whose handle we
	// can read and project to screen ourselves (see PokerCheat.cpp's
	// kSceneSlot header comment) -- the mechanism this mod uses to draw
	// card icons at the community cards' exact real screen position
	// instead of a guessed/calibrated one. Wired to the F10 menu's
	// "Probe Community Card Objects" item -- run with at least the flop
	// revealed so there's a real object to test against.
	void ProbeCommunityCardObjects();
#endif
}
