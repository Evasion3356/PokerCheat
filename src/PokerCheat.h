#pragma once

// Poker advisor: reads live hole cards for every seat + the board directly
// out of poker_sp's own script memory, scores every hand with its own
// evaluator (PokerHandEval.h -- not the game's hand-rank native), and draws
// an on-screen HUD showing everyone's cards and the predicted result. See the header comment in PokerCheat.cpp for the full
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

	// Diagnostic: draws every candidate real-font FACE token
	// ($title/$chalk/$ledger/$body1/$catalog1/$Font5/$gamername, the
	// same token list DrawFontTest() originally tested back when this
	// mod was first figuring out which token rendered a real RDR2 font
	// at all -- see docs/JOURNAL.md, "a real RDR2 font") against one
	// currently-selected language's own actual translated sample text
	// (a personality label + the verdict wording, from
	// Localization.cpp), instead of a fixed English "Face test" string.
	// Restored to check whether $Font5 (already confirmed for plain
	// ASCII) or any other token has glyphs for accented Latin/Cyrillic/
	// CJK -- Session 20/21 confirmed all 13 languages, including
	// Chinese/Japanese/Korean, render correctly through $Font5 (every
	// token except $gamername, in fact) PROVIDED RDR2's own actual
	// configured language matches -- see docs/PITFALLS.md, testing this
	// with the game itself still set to English/whatever and only this
	// mod's ini Language override changed shows tofu for CJK regardless
	// of token, since the game never streams in that language's font/
	// text assets at all otherwise. Wired to the F10 menu's "Toggle Font
	// Test" item -- draws regardless of Enabled/poker state, same
	// convention as the calibration grid above, so it can be checked
	// without a hand in progress.
	void ToggleFontTest();

	// Advances DrawFontTest()'s currently-selected language by one (with
	// wraparound), logging the new language's code so it's clear from
	// PokerCheat.log which screenshot corresponds to which language.
	// Wired to the F10 menu's "Cycle Font Test Language" item.
	void CycleFontTestLanguage();

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

	// Diagnostic: dumps EVERY script-local slot of poker_sp's running
	// thread to a timestamped PokerCheat_stackdump_YYYYMMDD_HHMMSS.jsonl
	// file (see GamePointers::DumpLocalStackJsonl) -- one JSON object per
	// slot, all of i32/u32/i64/f32/hex, no assumption about what any slot
	// means. Ported from BlackjackCheat's equivalent tool (see that
	// project's docs/JOURNAL.md, Session 6) for the same reason it was
	// built there: when a probe shows a field reading garbage, grepping/
	// jq-ing a wide raw dump for the expected value (a known rank/suit,
	// bet amount, etc.) converges faster than re-guessing one candidate
	// offset at a time -- and a before/after diff across a known state
	// change (a card dealt, a bet placed) separates the one real
	// persistent field from short-lived look-alike scratch values
	// elsewhere on the stack. Wired to the F10 menu's "Dump Full Stack
	// JSONL" item. Filenames are timestamped so consecutive dumps (e.g.
	// "before"/"after" a known change) each land in their own file
	// instead of the later one clobbering the one a diff needs.
	void DumpFullStackJsonl();
#endif
}
