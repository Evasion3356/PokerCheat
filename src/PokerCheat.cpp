/*
	Poker advisor module for the "poker_sp" single-player minigame.

	STATUS: reads live hole cards for every seat and the board directly out
	of poker_sp's own script-local memory (confirmed correct against the
	real screen -- see docs/JOURNAL.md), predicts the final board (real
	revealed cards + the exact future cards sitting at the current deck
	cursor -- deterministic, not a guess, confirmed via the
	"PredictionCheck ... MATCH" log line), and evaluates every hand with
	its OWN self-contained 7-card poker evaluator (category + full kicker
	breakdown -- see ScoreFiveCards()/EvaluateHand() below). Draws the
	result as an on-screen HUD list, gated behind Enabled.

	Earlier versions of this file called the game's own hand-rank native
	(MINIGAME::_0x32A7C216344D623B) instead of scoring hands itself, on the
	theory that replicating the game's own comparator (poker_sp.ysc.c's
	func_1541) exactly would guarantee identical results. That native's
	output buffer turned out to be effectively impossible to use correctly
	from the outside: real captures showed a "valid kicker count" field
	reading 7 (not the assumed 0-5), and, most damning, two DIFFERENT
	players' "kicker" data coming back byte-identical for cards that don't
	overlap (see docs/JOURNAL.md, Session 8 and the session after). Nine
	rounds of guessing at that buffer's layout across multiple sessions
	never converged. Since both inputs we actually need -- each seat's real
	hole cards, and the predicted final board -- were ALREADY independently
	confirmed correct by direct memory reads, there was never a reason to
	depend on the native for the category or the kicker at all: this file
	now computes both itself from those same confirmed-correct cards.

	Struct layout this all depends on (see docs/JOURNAL.md for the full
	derivation/confirmation trail -- traced via real call sites in the
	decompiled poker_sp.ysc.c, then independently confirmed against a live
	probe's raw memory dump):
	  - Table = uLocal_14.f_114.f_287, at absolute script-local slot 415 in
	    poker_sp's own thread (thread->m_Stack[415], no pointer
	    indirection -- it's inline data, not a stored pointer).
	  - Arrays here carry a leading size/count word before the element
	    data (matches RDR-Classes\script\types.hpp's SCR_ARRAY::Size).
	    Every read goes through a ScriptLocal chain (ScriptLocal.h)
	    written in the decompile's own `.f_N`/`[i]` terms, which skips
	    those size words itself -- see the layout block below.
	  - Table.f_15 (board): cards f_15[i] (stride 2, {rank,suit},
	    -1=empty, 11-slot array), reveal count f_15.f_23 (0/3/4/5).
	  - Table.f_39[seat] (seats, 6, stride 56). Per seat: hole cards
	    seat.f_7[i] (stride 2).
	  - Card = {rank: 2-14 (11=J,12=Q,13=K,14=A), suit: 0-3}. Suit mapping
	    0=Hearts, 1=Diamonds, 2=Spades, 3=Clubs -- poker_sp's own func_1599
	    mapping, confirmed via the TEST ICON experiment (see SuitLetter()).
	    An earlier 0=Clubs/3=Spades reading here was a misread.

	Hand ranking: ScoreFiveCards()/EvaluateHand() below implement standard
	poker hand evaluation directly (best 5 of the 2 hole + 5 board cards,
	full category + kicker tiebreak), independent of the game's own
	hand-rank native. Category numbering (0-9, high card..royal flush)
	still matches the decompile's STATS switch at poker_sp.ysc.c line
	~29880, purely so HandCategoryName()'s existing labels keep working.

	Not yet done: pot/bet fields (for real pot-odds advice, not just "who's
	ahead"), and calling the equity/win-probability native
	(MINIGAME::_0xEC819D612038EF4B) the same way, for an actual bet/fold/
	double-down recommendation rather than just showing hand strength.
*/

#include "PokerCheat.h"
#include "PokerHandEval.h"
#include "Log.h"
#include "GamePointers.h"
#include "ScriptLocal.h"
#include "ScriptGlobal.h"
#include "Config.h"
#include "Localization.h"
#include "script.h"
#include "keyboard.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <array>
#include <charconv>
#include <initializer_list>
#include <string>
#include <string_view>

namespace PokerCheat
{
	bool Enabled = false;

	void Toggle()
	{
		Enabled = !Enabled;
		Log::Write("PokerCheat::Toggle -> {}", Enabled ? "ON" : "OFF");
	}

	void SetEnabled(bool enabled)
	{
		Enabled = enabled;
		Log::Write("PokerCheat::SetEnabled -> {}", Enabled ? "ON" : "OFF");
	}

#ifdef _DEBUG
	bool CalibrationGridEnabled = false;

	void ToggleCalibrationGrid()
	{
		CalibrationGridEnabled = !CalibrationGridEnabled;
		Log::Write("PokerCheat::ToggleCalibrationGrid -> {}", CalibrationGridEnabled ? "ON" : "OFF");
	}

	bool FontTestEnabled = false;
	int FontTestLanguageIndex = 0; // index into Localization::Language, cycled by CycleFontTestLanguage()

	void ToggleFontTest()
	{
		FontTestEnabled = !FontTestEnabled;
		Log::Write("PokerCheat::ToggleFontTest -> {}", FontTestEnabled ? "ON" : "OFF");
	}

	void CycleFontTestLanguage()
	{
		constexpr int kLanguageCount = static_cast<int>(Localization::Language::Count);
		FontTestLanguageIndex = (FontTestLanguageIndex + 1) % kLanguageCount;
		Localization::Language lang = static_cast<Localization::Language>(FontTestLanguageIndex);
		Log::Write("PokerCheat::CycleFontTestLanguage -> {} ({})", FontTestLanguageIndex, Localization::LanguageCode(lang));
	}
#endif

	// ------------------------------------------------------------------
	// Script-local layout, as ScriptLocal chains (ScriptLocal.h) that
	// mirror poker_sp.ysc.c's own field syntax: `.f_N` -> At(N),
	// `x[i /*S*/]` -> At(i, S). Every offset below is the DECOMPILED field
	// number -- no hand-applied "+1 for the size word" anywhere. See the
	// header comment above and docs/JOURNAL.md for the derivation/
	// confirmation trail. The static_asserts after the chain functions pin
	// every chain to the slot the old flat constants read (all live-
	// confirmed), so the conversion can't have moved a read -- except the
	// personality index, which the old flat math read one word early (see
	// kPersonalityField).
	// ------------------------------------------------------------------
	constexpr std::uint32_t kRootLocalIndex = 14;      // uLocal_14
	constexpr std::uint32_t kF114Field = 114;          // uLocal_14.f_114
	constexpr std::uint32_t kTableAField = 287;        // f_114.f_287 -- the UI's copy of the table (see below)
	constexpr std::uint32_t kTableBField = 1276;       // f_114.f_1276 -- the authoritative game engine (see below)

	// How Candidate A and Candidate B relate -- traced in poker_sp.ysc.c:
	//
	//  - B (f_1276) is the actual poker engine. Its own state machine
	//    (func_468, switching on B.f_535) shuffles (func_590), deals hole
	//    cards (func_1093) and deals each street (func_1091), drawing every
	//    card off B's deck (func_1543: card = deck[f_105], f_105++) and
	//    appending it to B's own board/seat arrays in the same call
	//    (func_1540, which also bumps the array's f_23 count). So B's
	//    board, reveal count, hole cards and deck cursor are ALWAYS
	//    mutually consistent from outside the script.
	//  - After each step the engine calls func_435(B, eventHash), which
	//    snapshots B's first 486 words and posts them as a minigame event
	//    (MINIGAME::_0xE1F365C4C8F259D8). func_137 pops one queued event
	//    into a staging copy (f_114.f_773), and it only reaches A when the
	//    UI's per-state handler decides to consume it -- func_141 ->
	//    func_400 -> `f_287 = { *f_773 }`. A is therefore a 486-word copy of
	//    B's head (f_287 + 486 == f_773, f_773 + 486 == f_1259, the event
	//    header), with the same layout for the board (f_15) and seats (f_39),
	//    but NOT the deck: B's deck is at +606, past the snapshot, which is
	//    why "Candidate A's deck" never looked like a live deck (A+606 is
	//    actually inside the f_773 staging copy).
	//  - The UI deliberately holds events back while animations play. The
	//    state-5 handler (func_291) only PEEKS at a pending "street dealt"
	//    event (func_376) and moves f_2010 to 6; the state-6 handler
	//    (func_292) then applies it only once func_616 reports the dealer
	//    is no longer mid-animation. Likewise the state-4 handler (func_290)
	//    applies the new hand's hole-card deal only after the dealer and
	//    both blinds are idle and func_367 passes (up to a 5 s timeout).
	//    Throughout those windows f_2010 is already 4 or 6 (inside
	//    DrawOverlay()'s handInProgress range), B has already dealt, and
	//    A still shows the previous street -- or the previous HAND.
	//
	// So anything the prediction/evaluation depends on (board, reveal
	// count, hole cards, fold state, deck) is read from B, where it is
	// always consistent with the deck cursor. A is only used where the
	// overlay has to line up with what the game is currently SHOWING: seat
	// occupancy (the Scaleform seat-panel layout, see
	// ComputeDenseRowForSeat()) and the visible reveal count (which board
	// icons are drawn solid vs. ghosted).

	constexpr std::uint32_t kMySeatField = 9; // uLocal_14.f_114.f_9 (local player's seat)

	// uLocal_14.f_114.f_2010 -- poker_sp's own round-phase state machine,
	// set exclusively through func_213 (which also stamps f_2011, a
	// sub-step, and resets a timer at f_2012). Traced two independent
	// pieces of evidence for what 0 means: func_1502's per-seat "who to
	// highlight" switch (poker_sp.ysc.c line ~37697) treats every state
	// it doesn't explicitly list (its `default` case) as "highlight
	// nobody" -- 0 isn't one of its explicit cases (1-11 are) -- and the
	// ONLY call site that sets f_2010 back to 0 (line ~13269, inside a
	// state-85 step of a separate per-seat animation-step machine) does
	// so specifically once every seated player's own "reacting to the
	// last hand" animation (func_799) has finished. Both point the same
	// way: 0 is the idle/between-hands state, after the previous hand's
	// resolution has fully played out and before the next deal begins --
	// exactly the window the user reported still seeing stale card icons
	// in. Non-zero (1 upward) covers the actual deal/betting/showdown
	// steps. Not independently confirmed against a live memory read yet
	// (see docs/JOURNAL.md if this needs re-deriving).
	constexpr std::uint32_t kHandStateField = 2010;

	// uLocal_14.f_1.f_42 -- user-suggested alternate candidate for the
	// round-phase state (f_1 is the same generic framework struct
	// kFrameworkHashField already reads f_1.f_39 from -- see that
	// constant). f_42 doesn't turn up
	// directly in poker_sp.ysc.c itself (searched; no `.f_42`/`->f_42`
	// hits), which fits it living in the shared act_gen_poker.ysc.c
	// framework layer instead, same as f_39 -- not yet traced there.
	// Logged alongside kHandStateField in both the on-screen debug
	// line and ProbeTableStruct so the two candidates can be directly
	// compared against a real between-hands window rather than swapped
	// in blind a third time.
	constexpr std::uint32_t kF1Field = 1;              // uLocal_14.f_1
	constexpr std::uint32_t kF1StateField = 42;        // f_1.f_42

	// uLocal_14.f_114.f_2011 -- the SUB-STEP field func_213 always sets
	// alongside f_2010 (same call, same function -- see kHandStateField's
	// header comment), but which this file had the address for and never
	// actually read. User traced a real, active callback (func_286,
	// registered via func_287(uParam0, 1, &func_286) inside func_101,
	// confirmed to receive uLocal_14 itself when invoked -- its own field
	// usage, f_9/f_3310/f_4583/f_1, all match uLocal_14's top-level fields
	// exactly, and its case 94 calls func_213(&(uParam0->f_114), 3, 0),
	// i.e. it's a real f_2010/f_2011-owning state machine) that switches on
	// exactly this field: case 0 -> sets f_2011=13, case 13 -> sets
	// f_2011=94, case 94 -> transitions f_2010 to 3 once a condition
	// clears. A genuinely active sub-step machine, unlike the other two
	// candidates (both confirmed dead ends -- constant for the whole
	// session). Logged alongside them for direct comparison.
	constexpr std::uint32_t kSubStepField = 2011;

	// Table fields (shared by A and B -- A is a copy of B's head).
	constexpr std::uint32_t kBoardField = 15;             // Table.f_15 -- board struct: cards[i /*2*/] (size word confirmed = 11, `uLocal_430`/`uLocal_1419 = 11`), then f_23
	constexpr std::uint32_t kBoardCardsField = 0;         // board[i /*2*/]
	constexpr std::uint32_t kBoardRevealCountField = 23;  // f_15.f_23 -- how many board slots are filled (func_1540)
	constexpr std::uint32_t kSeatsField = 39;             // Table.f_39[seat /*56*/] (size word confirmed = 6, `uLocal_454`/`uLocal_1443 = 6`)
	constexpr std::uint32_t kSeatStride = 56;
	constexpr std::uint32_t kSeatCount = 6;

	// Seat fields (Table.f_39[seat]).
	constexpr std::uint32_t kSeatOccupiedField = 0;       // seat.f_0 -- func_143's `Table.f_39[seat] != -1`
	constexpr std::uint32_t kSeatStackField = 2;          // seat.f_2 -- stack
	constexpr std::uint32_t kSeatBetField = 3;            // seat.f_3 -- current bet
	constexpr std::uint32_t kSeatStateField = 6;          // seat.f_6 -- -1 empty, 0 active, 1 folded, 2 all-in
	constexpr std::uint32_t kSeatHoleCardsField = 7;      // seat.f_7[i /*2*/] -- hole cards

	// Card fields (board[i], hole cards[i], deck[i]).
	constexpr std::uint32_t kCardStride = 2;
	constexpr std::uint32_t kCardRankField = 0;
	constexpr std::uint32_t kCardSuitField = 1;

	// uLocal_14.f_114.f_2655.f_90[seat] -- each seat's AI personality
	// index (0-14, into a fixed 15-entry style table func_584 builds once
	// via 15 func_1191() calls). Confirmed via the ONE literal call site
	// that actually assigns it, func_185 at poker_sp.ysc.c line 5883:
	// `func_185(&(uLocal_14.f_114.f_2655), i)` -- no parameter-identity
	// tracing needed here, uLocal_14 appears in the call verbatim. f_90
	// is an ordinary script array WITH a leading size word, like
	// Table.f_15/f_39: the decompile writes it `f_90[i]` (brackets), and
	// its size word is declared at the old flat slot itself --
	// `var uLocal_2873 = 6;` (14 + 114 + 2655 + 90 = 2873). The old flat
	// read (2873 + seat, on the belief that f_90 had no size word) got 6
	// for seat 0 and seat N-1's personality for seat N. At(seat, 1) reads
	// 2874 + seat. See docs/JOURNAL.md Session 14 (func_1628, the real
	// decision engine this index feeds) and the kPersonalityLabels header
	// comment in Localization.cpp for the index->label mapping. Purely a
	// fixed-per-seat read, no RNG/prediction involved (unlike the
	// abandoned fold-prediction feature, Session 17).
	constexpr std::uint32_t kAiField = 2655;              // f_114.f_2655
	constexpr std::uint32_t kPersonalityField = 90;       // f_2655.f_90[seat]

	// .f_606 -- the deck. Confirmed via func_589 (builds a plain,
	// UN-prefixed 52-card array directly at the deck's own base -- 4
	// suits x ranks 2-14, no leading size/count word the way f_15/f_39
	// have; NOT all arrays here use that convention, this one doesn't),
	// func_1195 (the actual shuffle -- 5 full passes of a Fisher-Yates
	// swap using MISC::GET_RANDOM_INT_IN_RANGE), and the flop/turn/river
	// reveal function (draws 3/1/1 cards straight off this same array via
	// func_1543, which just reads card[f_105] and increments the cursor
	// -- no burn cards).
	//
	// IMPORTANT: the real, shuffled, gameplay deck lives on CANDIDATE B
	// (f_1276), not Candidate A (f_287). Traced both func_589 call sites to their enclosing
	// functions: the unshuffled init (no func_1195 paired with it) is
	// inside func_285, and the shuffled init (func_589 immediately
	// followed by func_1195) is inside func_590 -- both of those, via
	// their own confirmed call sites (func_285(&(uParam0->f_1276)) at
	// line 7065, func_590(uParam0) called right after within func_285
	// itself with the same uParam0), operate on Candidate B, not A. A is
	// the UI's delayed copy of B's head and doesn't contain the deck at
	// all -- see the A/B comment next to kTableBField.
	constexpr std::uint32_t kDeckField = 606;                  // Table.f_606
	constexpr std::uint32_t kDeckCardsField = 0;               // deck[i /*2*/] -- func_1543's `uParam0->[f_105 /*2*/]`
	constexpr std::uint32_t kDeckCursorField = 105;            // f_606.f_105 -- index of the next undrawn card
	constexpr std::uint32_t kDeckCountField = 106;             // f_606.f_106 -- total cards (52)
	// The 52 card elements do NOT start at deck+0: deck+0 is the card
	// array's size word (reads 52; declared `var uLocal_2010 = 52;`),
	// which At(i, kCardStride) skips. Nailed down empirically before the
	// ScriptLocal conversion: raw-dumped deck+1..+16 against 4
	// already-dealt, screen-confirmed hole cards (2 consecutive draws per
	// seat) and got an exact, duplicate-free match starting at +1 (card
	// 0's rank), not +0 -- see docs/JOURNAL.md. Every earlier read of card
	// element k (rank at deck+k*2) was off by one word, which is why
	// predicted cards were reading as garbage/invalid ranks and suits
	// instead of just "wrong but valid-looking" ones.

	// LaunchArgs (uScriptParam_0) and the framework-hash landmark used to
	// independently validate the uLocal_14-relative addressing chain --
	// kept here for ProbeTableStruct(), not used by the overlay itself.
	constexpr std::uint32_t kLaunchArgsIndex = 4810;           // uScriptParam_0 (declared right after the last uLocal)
	constexpr std::uint32_t kLaunchArgsStakesTierField = 12;
	constexpr std::uint32_t kFrameworkHashField = 39;          // uLocal_14.f_1.f_39
	constexpr std::int32_t kKnownStakesHashes[] = {-1150372370, 355424894, -471827042, -2033178055};

	// The "scene" struct (uLocal_14.f_3310, a sibling of f_114/Table, NOT
	// nested inside it -- confirmed via func_214's own call sites, all 5
	// of which pass `&uLocal_14` as its first arg and
	// `&(uLocal_14.f_114.f_287)` as its second, i.e. func_214(scene,
	// table, ...)). This is the UI/scene driver poker_sp uses to place
	// REAL 3D objects for the community cards during play (as opposed to
	// the showdown reveal icons, which are pure Scaleform UI with no
	// accessible screen position -- func_1325/func_1594 write the card
	// texture name via DATABINDING::_DATABINDING_WRITE_DATA_STRING, no
	// coordinate involved). func_471 (the flop/turn/river reveal)
	// confirms this: it CREATE_OBJECTs a real prop per board slot and
	// stores the handle at scene.f_671.f_11[slot] -- so unlike the
	// showdown icons, community-card-during-play positions ARE readable:
	// get the live object handle, call ENTITY::GET_ENTITY_COORDS on it
	// ourselves, then GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD to
	// project to screen, and draw our own card-face icon exactly there --
	// no guessed/calibrated coordinates needed. Offset below is a traced
	// candidate, not yet empirically confirmed (see
	// ProbeCommunityCardObjects()).
	constexpr std::uint32_t kSceneField = 3310;                // uLocal_14.f_3310
	constexpr std::uint32_t kSceneCardPropsField = 671;        // scene.f_671
	// f_11 is itself an array with the usual leading size/count header
	// word (same convention as Table.f_15/f_39, confirmed empirically
	// via ProbeCommunityCardObjects(): the size word read exactly 5 --
	// the board's own slot count, not a card -- and the real object
	// handles started one word later; declared `var uLocal_4006 = 5;`).
	constexpr std::uint32_t kCommunityCardObjectsField = 11;   // scene.f_671.f_11[slot], real object handle
	constexpr std::uint32_t kCommunityCardObjectCount = 5;

	constexpr ScriptLocal RootLocal(rage::scrThread* thread) { return ScriptLocal(thread, kRootLocalIndex); }
	constexpr ScriptLocal F114Local(rage::scrThread* thread) { return RootLocal(thread).At(kF114Field); }
	constexpr ScriptLocal MySeatLocal(rage::scrThread* thread) { return F114Local(thread).At(kMySeatField); }
	constexpr ScriptLocal HandStateLocal(rage::scrThread* thread) { return F114Local(thread).At(kHandStateField); }
	constexpr ScriptLocal SubStepLocal(rage::scrThread* thread) { return F114Local(thread).At(kSubStepField); }
	constexpr ScriptLocal F1StateLocal(rage::scrThread* thread) { return RootLocal(thread).At(kF1Field).At(kF1StateField); }
	constexpr ScriptLocal FrameworkHashLocal(rage::scrThread* thread) { return RootLocal(thread).At(kF1Field).At(kFrameworkHashField); }
	constexpr ScriptLocal LaunchArgsLocal(rage::scrThread* thread) { return ScriptLocal(thread, kLaunchArgsIndex); }

	constexpr ScriptLocal TableALocal(rage::scrThread* thread) { return F114Local(thread).At(kTableAField); }
	constexpr ScriptLocal TableBLocal(rage::scrThread* thread) { return F114Local(thread).At(kTableBField); }
	constexpr ScriptLocal BoardLocal(const ScriptLocal& table) { return table.At(kBoardField); }
	constexpr ScriptLocal BoardCardLocal(const ScriptLocal& table, std::uint32_t card) { return BoardLocal(table).At(kBoardCardsField).At(card, kCardStride); }
	constexpr ScriptLocal SeatLocal(const ScriptLocal& table, std::uint32_t seat) { return table.At(kSeatsField).At(seat, kSeatStride); }
	constexpr ScriptLocal HoleCardLocal(const ScriptLocal& seat, std::uint32_t card) { return seat.At(kSeatHoleCardsField).At(card, kCardStride); }
	constexpr ScriptLocal DeckLocal(const ScriptLocal& table) { return table.At(kDeckField); }
	constexpr ScriptLocal DeckCardLocal(rage::scrThread* thread, std::int32_t card) { return DeckLocal(TableBLocal(thread)).At(kDeckCardsField).At(static_cast<std::uint32_t>(card), kCardStride); }
	constexpr ScriptLocal PersonalityLocal(rage::scrThread* thread, std::uint32_t seat) { return F114Local(thread).At(kAiField).At(kPersonalityField).At(seat, 1); }
	constexpr ScriptLocal CommunityCardObjectLocal(rage::scrThread* thread, std::uint32_t slot) { return RootLocal(thread).At(kSceneField).At(kSceneCardPropsField).At(kCommunityCardObjectsField).At(slot, 1); }

	// Pinned to the absolute slots the old flat constants read, every one
	// live-confirmed (ProbeTableStruct/ProbeSeatOccupancy/
	// ProbeCommunityCardObjects logs and the on-screen cards; see
	// docs/JOURNAL.md).
	static_assert(TableALocal(nullptr).Index() == 415 && TableBLocal(nullptr).Index() == 1404);
	static_assert(MySeatLocal(nullptr).Index() == 137 && HandStateLocal(nullptr).Index() == 2138 && SubStepLocal(nullptr).Index() == 2139);
	static_assert(F1StateLocal(nullptr).Index() == 57 && FrameworkHashLocal(nullptr).Index() == 54);
	static_assert(BoardLocal(TableALocal(nullptr)).Index() == 430 && BoardLocal(TableBLocal(nullptr)).Index() == 1419);                  // board size word (`= 11`)
	static_assert(BoardCardLocal(TableBLocal(nullptr), 0).Index() == 1420 && BoardCardLocal(TableBLocal(nullptr), 4).At(kCardSuitField).Index() == 1429);
	static_assert(BoardLocal(TableBLocal(nullptr)).At(kBoardRevealCountField).Index() == 1442);
	static_assert(SeatLocal(TableALocal(nullptr), 0).Index() == 455 && SeatLocal(TableBLocal(nullptr), 5).Index() == 1444 + 5 * 56);  // old kSeatsDataBase(B) + seat*56
	static_assert(HoleCardLocal(SeatLocal(TableBLocal(nullptr), 0), 0).Index() == 1452 && HoleCardLocal(SeatLocal(TableBLocal(nullptr), 0), 1).Index() == 1454);
	static_assert(DeckLocal(TableBLocal(nullptr)).Index() == 2010 && DeckCardLocal(nullptr, 0).Index() == 2011);                        // deck size word (`= 52`), card 0
	static_assert(DeckLocal(TableBLocal(nullptr)).At(kDeckCursorField).Index() == 2115 && DeckLocal(TableBLocal(nullptr)).At(kDeckCountField).Index() == 2116);
	static_assert(CommunityCardObjectLocal(nullptr, 0).Index() == 4007);                                                              // old kCommunityCardObjectsBase
	// Deliberately NOT the old slot (2873 + seat, which was f_90's size
	// word for seat 0) -- see kPersonalityField.
	static_assert(PersonalityLocal(nullptr, 0).Index() == 2874);

	// The two amount-entry UIs, both in the prompt/HUD struct
	// uLocal_14.f_2979. Static trace only:
	//  - Bet/raise (f_2979.f_281): func_652 opens it (func_1248) on your
	//    turn only -- it's guarded by func_648, `f_114.f_9 == seat` -- and
	//    func_653 runs it every frame (func_1252) until f_281.f_2 holds
	//    the chosen action. f_281.f_4 is the amount in chips, ON TOP of
	//    what your seat already put in this street (seat.f_4), so 0 is
	//    check. func_1252 moves it by func_984's steps (one chip per step)
	//    and clamps it to func_1614's three limits: [0 or the call, the
	//    minimum raise, the most you can put in] -- anything between the
	//    call and the minimum raise snaps to one of them by direction.
	//  - Buy-in (f_2979.f_298): func_389 opens it when you sit down or are
	//    broke; func_390 runs it. f_298.f_2 is the amount in chips, clamped
	//    by func_985 to [f_114.f_10.f_12, f_114.f_10.f_13] (the table's
	//    buy-in limits), each capped at the cash you carry, in chips.
	// Both hold func_984's hold-to-repeat struct (f_281.f_5 / f_298.f_3),
	// whose first float is the static initial value 7f
	// (`uLocal_3279`/`uLocal_3294 = 1088421888`), which pins the slots
	// below. f_2979.f_157 is cents per chip (func_987: the prompt shows
	// `chips * f_157`; func_982: cash / f_157 = chips). The bet hotkeys
	// (UpdateBetHotkeys()) write f_281.f_4 / f_298.f_2 -- the mod's only
	// memory write -- and copy that repeat curve for Right/Left.
	constexpr std::uint32_t kPromptHudField = 2979;           // uLocal_14.f_2979
	constexpr std::uint32_t kChipValueField = 157;            // f_2979.f_157 -- cents per chip
	constexpr std::uint32_t kBetInputField = 281;             // f_2979.f_281 -- 1 while the bet/raise UI is up
	constexpr std::uint32_t kBetInputResultField = 2;         // f_281.f_2 -- 0 until an action is chosen
	constexpr std::uint32_t kBetInputAmountField = 4;         // f_281.f_4 -- chips on top of seat.f_4
	constexpr std::uint32_t kBetInputRepeatField = 5;         // f_281.f_5 -- func_984's hold-to-repeat struct
	constexpr std::uint32_t kBuyInInputField = 298;           // f_2979.f_298 -- 1 while the buy-in UI is up
	constexpr std::uint32_t kBuyInInputResultField = 1;       // f_298.f_1 -- 0 until confirmed
	constexpr std::uint32_t kBuyInInputAmountField = 2;       // f_298.f_2 -- chips
	constexpr std::uint32_t kBuyInInputRepeatField = 3;       // f_298.f_3
	constexpr std::uint32_t kBetInputAlterPromptField = 16;   // f_281.f_16 -- the game's "Amount" prompt, as a prompt-pool slot (func_1252)
	constexpr std::uint32_t kBuyInInputAlterPromptField = 13; // f_298.f_13 -- same, buy-in (func_390; only when min != max)

	// The prompt pool every func_450 prompt lives in: Global_1945188[slot
	// /*18*/], slots 1-48 (func_450 hands out the first free one > 0, 0 =
	// none; func_119 rejects > 48). .f_3 is the real Prompt handle -- what
	// func_988/func_451 pass to _UI_PROMPT_SET_TEXT/_SET_ATTRIBUTE.
	constexpr std::uint32_t kPromptPoolGlobal = 1945188;
	constexpr std::uint32_t kPromptPoolStride = 18;
	constexpr std::uint32_t kPromptPoolHandleField = 3;
	constexpr std::int32_t kPromptPoolLastSlot = 48;

	// func_1614's inputs: the table (Table A, f_114.f_287 -- func_653
	// passes that copy), your seat, and the table settings f_114.f_10.
	constexpr std::uint32_t kTableCallField = 7;              // Table.f_7 -- the street's highest bet (0 = nobody bet yet)
	constexpr std::uint32_t kTableRaiseField = 8;             // Table.f_8 -- the last raise's size
	constexpr std::uint32_t kTableOpenBetField = 10;          // Table.f_10 -- the minimum bet when nobody bet yet
	constexpr std::uint32_t kSeatHandTotalField = 3;          // seat.f_3 -- put in this hand (vs. the table cap)
	constexpr std::uint32_t kSeatStreetBetField = 4;          // seat.f_4 -- put in this street
	constexpr std::uint32_t kSeatCanRaiseField = 5;           // seat.f_5 -- 0 = may only call, not raise
	constexpr std::uint32_t kSettingsField = 10;              // f_114.f_10
	constexpr std::uint32_t kSettingsLimitTypeField = 3;      // f_10.f_3 -- kCappedLimitType = a capped table
	constexpr std::uint32_t kSettingsBuyInMinField = 12;      // f_10.f_12
	constexpr std::uint32_t kSettingsBuyInMaxField = 13;      // f_10.f_13
	constexpr std::uint32_t kSettingsCapField = 16;           // f_10.f_16 -- most a seat can put in per hand, if > 0
	constexpr std::int32_t kCappedLimitType = 1717653435;

	constexpr ScriptLocal PromptHudLocal(rage::scrThread* thread) { return RootLocal(thread).At(kPromptHudField); }
	constexpr ScriptLocal BetInputLocal(rage::scrThread* thread) { return PromptHudLocal(thread).At(kBetInputField); }
	constexpr ScriptLocal BuyInInputLocal(rage::scrThread* thread) { return PromptHudLocal(thread).At(kBuyInInputField); }
	constexpr ScriptLocal SettingsLocal(rage::scrThread* thread) { return F114Local(thread).At(kSettingsField); }

	static_assert(BetInputLocal(nullptr).At(kBetInputRepeatField).Index() == 3279);     // `uLocal_3279 = 1088421888` (7f), static trace
	static_assert(BuyInInputLocal(nullptr).At(kBuyInInputRepeatField).Index() == 3294); // `uLocal_3294 = 1088421888` (7f), static trace

	constexpr ScriptGlobal PromptHandleGlobal(std::uint32_t slot) { return ScriptGlobal(kPromptPoolGlobal).At(slot, kPromptPoolStride).At(kPromptPoolHandleField); }
	static_assert(PromptHandleGlobal(0).Index() == 1945188 + 1 + 3 && PromptHandleGlobal(2).Index() == 1945188 + 1 + 2 * 18 + 3); // the size word is skipped

	namespace
	{

		const char* RankName(std::int32_t rank)
		{
			switch (rank)
			{
				case 2: return "2";
				case 3: return "3";
				case 4: return "4";
				case 5: return "5";
				case 6: return "6";
				case 7: return "7";
				case 8: return "8";
				case 9: return "9";
				case 10: return "10";
				case 11: return "J";
				case 12: return "Q";
				case 13: return "K";
				case 14: return "A";
				default: return "?";
			}
		}

		char SuitLetter(std::int32_t suit)
		{
			// Mapping settled by the TEST ICON experiment: the icon drew a
			// real Ace of Hearts for suit 0 using BuildCardTextureName()'s
			// (func_1599-traced) mapping, directly disproving this
			// function's old guess of suit 0 = Clubs. func_1599 is real,
			// untouched game code with no remapping between it and the raw
			// card value this file already reads, so its mapping is now
			// used everywhere: 0=Hearts, 1=Diamonds, 2=Spades, 3=Clubs.
			// The earlier "suit 3 = Spades" claim was a misread.
			switch (suit)
			{
				case 0: return 'H'; // Hearts -- confirmed via TEST ICON (real Ace of Hearts)
				case 1: return 'D'; // Diamonds -- confirmed against a real 7 of Diamonds
				case 2: return 'S'; // Spades
				case 3: return 'C'; // Clubs
				default: return '?';
			}
		}

		// Opponent personality/style labels ("Tight-Aggressive" etc.) now
		// live in Localization.cpp's kPersonalityLabels (one row per
		// supported language) -- see that file for the full derivation
		// comment (traced from poker_sp.ysc.c's func_584/func_1191/
		// func_185) this function used to carry. Callers use
		// Localization::PersonalityLabel(personalityIndex) directly.

		const char* HandCategoryName(std::int32_t category)
		{
			// Mapping confirmed from poker_sp.ysc.c's stat-tracking switch
			// (line ~29880 in the ground-truth decompile).
			switch (category)
			{
				case 0: return "High Card";
				case 1: return "Pair";
				case 2: return "Two Pair";
				case 3: return "Three of a Kind";
				case 4: return "Straight";
				case 5: return "Flush";
				case 6: return "Full House";
				case 7: return "Four of a Kind";
				case 8: return "Straight Flush";
				case 9: return "Royal Flush";
				default: return "";
			}
		}

		// Card FACE textures (as opposed to the 3D card props on the
		// physical table) are real 2D textures -- traced end to end
		// through the ground-truth decompile: func_628 (the exact same
		// raw hole-card getter this file already uses) feeds straight
		// into func_1325 -> func_1599 with no remapping, and func_1599
		// builds a texture NAME as "<SUIT>_<RANK>" (e.g. "DIAMONDS_7",
		// "HEARTS_A") inside a texture DICTIONARY named "card_set_N"
		// (func_925 builds that name; N depends on which table/location
		// skin is active, via func_330/func_331 -- not replicated here).
		// func_1599's own suit mapping: 0=Hearts, 1=Diamonds, 2=Spades,
		// 3=Clubs -- confirmed correct via the TEST ICON (a real Ace of
		// Hearts drawn for suit 0), and SuitLetter() now uses the same
		// mapping.
		// Called per card per frame from the Release HUD, so it builds into
		// one reused buffer (same convention as BgFormatText() below) rather
		// than returning a fresh std::string -- no heap allocation once the
		// buffer's capacity has grown. The returned pointer is valid until
		// the next call. Reuses RankName() above instead of a second,
		// duplicate rank-name switch.
		const char* BuildCardTextureName(std::int32_t rank, std::int32_t suit)
		{
			std::string_view suitName;
			switch (suit)
			{
				case 0: suitName = "HEARTS_"; break;
				case 1: suitName = "DIAMONDS_"; break;
				case 2: suitName = "SPADES_"; break;
				case 3: suitName = "CLUBS_"; break;
				default: break;
			}

			static std::string buffer;
			buffer.assign(suitName);
			buffer.append(RankName(rank));
			return buffer.c_str();
		}

		// The real card_set_N number depends on which table/location skin
		// is active (func_330/func_331's selection logic, not replicated
		// here) -- instead of reimplementing that, this just probes which
		// card_set_N dictionary is ALREADY streamed in, since the game
		// itself must have already loaded the correct one to be showing
		// its own cards right now. Returns an empty view if none are
		// loaded yet (e.g. called before the table has finished setting
		// up) -- see DrawOverlay() for the card_set_1 request fallback.
		//
		// Fixed table of string literals rather than building
		// "card_set_" + std::to_string(n) per probe: this runs every frame
		// in Release. Every entry views a whole string literal, so .data()
		// is null-terminated and safe to hand to the native directly.
		constexpr std::string_view kCardSetDicts[] = {
			"card_set_1", "card_set_2", "card_set_3", "card_set_4",
			"card_set_5", "card_set_6", "card_set_7", "card_set_8"
		};

		std::string_view FindLoadedCardSetDict()
		{
			for (const std::string_view dict : kCardSetDicts)
			{
				if (TEXTURE::HAS_STREAMED_TEXTURE_DICT_LOADED(const_cast<char*>(dict.data())))
					return dict;
			}

			return {};
		}

		// 2D community-card icon strip, top-right of screen -- calibrated
		// by eye against the real game's own card strip using
		// DrawCalibrationGrid() (thin lines every 0.05, labeled every
		// 0.1), then hand-tuned further via play testing. Debug builds
		// read position/spacing/size from Config (PokerCheat.ini's
		// [CommunityCardIcons2D] section) so they can keep being tuned
		// live via Reload Config; Release bakes in the user-confirmed
		// "perfect" values directly as constexpr instead -- an end user
		// shouldn't need to calibrate pixel positions themselves, and
		// Release's PokerCheat.ini never gets a [CommunityCardIcons2D]
		// section written to it at all (see Config.cpp).
		constexpr int kCard2DIconCount = 5;
		constexpr int kCard2DPredictedAlpha = 140; // ghosted, matches the old 3D-anchor alpha convention

#ifndef _DEBUG
		constexpr float kReleaseCard2DIconBaseX = 0.821f;
		constexpr float kReleaseCard2DIconY = 0.078f;
		constexpr float kReleaseCard2DIconSpacingX = 0.034f;
		constexpr float kReleaseCard2DIconWidth = 0.03f;
		constexpr float kReleaseCard2DIconHeight = 0.075f;
#endif

		// Draws all 5 eventual community card icons at the calibrated
		// strip position above -- real revealed cards at full alpha,
		// not-yet-revealed predicted cards (deterministic, see kDeckField's
		// header comment) ghosted at reduced alpha, same real/predicted
		// split as the text "Board:" line. Takes the predicted-final-board
		// ranks/suits DrawOverlay() reads once up front (see
		// ReadPredictedBoard()) rather than re-reading memory itself, and
		// the card-set dictionary DrawOverlay() already looked up for this
		// frame (non-empty -- see FindLoadedCardSetDict()).
		void DrawCommunityCardIcons(std::string_view cardSetDict, const std::int32_t* ranks, const std::int32_t* suits, std::int32_t revealCount)
		{
#ifdef _DEBUG
			const Config::Values& cfg = Config::Get();
			float baseX = cfg.Card2DIconBaseX;
			float iconY = cfg.Card2DIconY;
			float spacingX = cfg.Card2DIconSpacingX;
			float width = cfg.Card2DIconWidth;
			float height = cfg.Card2DIconHeight;
#else
			float baseX = kReleaseCard2DIconBaseX;
			float iconY = kReleaseCard2DIconY;
			float spacingX = kReleaseCard2DIconSpacingX;
			float width = kReleaseCard2DIconWidth;
			float height = kReleaseCard2DIconHeight;
#endif

			for (int i = 0; i < kCard2DIconCount; i++)
			{
				if (ranks[i] < 2)
					continue;

				bool predicted = i >= revealCount;
				int alpha = predicted ? kCard2DPredictedAlpha : 255;
				float x = baseX + static_cast<float>(i) * spacingX;

				GRAPHICS::DRAW_SPRITE(const_cast<char*>(cardSetDict.data()), const_cast<char*>(BuildCardTextureName(ranks[i], suits[i])), x, iconY, width, height, 0.0f, 255, 255, 255, alpha, 0);
			}
		}

		// Opponent hole-card icons, one pair per occupied opponent seat
		// (relOffset 1+ -- see DrawOverlay()'s call site; per user
		// request this is opponents only, never your own seat, since you
		// can already see your own cards) -- same 2D calibrated-strip
		// technique as DrawCommunityCardIcons() above, since the per-seat
		// name/stack panel is Scaleform/
		// DATABINDING-driven (func_214/func_470 in the ground-truth
		// decompile bind seat data by RAW seat index, no coordinate
		// involved) and has no readable screen position, exactly like the
		// top-right community-card strip before it was calibrated.
		//
		// Indexed by RELATIVE seat offset -- (mySeat - seat + 6) % 6, so
		// 0 is always you -- not raw seat index. Confirmed necessary, not
		// just convenient: docs/JOURNAL.md records "seat index changes
		// every hand -- user sits randomly", yet the user always sees
		// themselves in the same on-screen spot ("bottom"). Since the
		// script binds seat data by raw index unrotated, that only works
		// if the Scaleform movie itself re-arranges panels relative to
		// whichever seat is the local player's -- so the panel positions
		// this file calibrates against must be relative-offset positions
		// too, not per-raw-seat ones.
		//
		// Direction confirmed via a live 4-seat report: mySeat=5 (bottom
		// row), then going UP the vertical list: seat 4, seat 3, seat 2 --
		// i.e. the list counts DOWN from your own seat number (wrapping
		// mod 6) as it goes up the screen, matching (mySeat - seat), not
		// (seat - mySeat) as this file originally guessed. Also matches
		// an idiom the game's own script already uses elsewhere for turn
		// order: `(uLocal_14.f_114.f_9 + 5) % 6`, i.e. `(mySeat - 1) % 6`,
		// for "the seat before mine" (poker_sp.ysc.c line ~5858).
		//
		// Calibrated via live Reload Config tuning (see Config.h's
		// SeatCardIconBaseX/Y/StepY header comment for the pixel-derived
		// starting estimate this was tuned from).
		constexpr int kSeatCardIconCount = 2;

		// Wraps `parts` (concatenated) in the rich-text tags
		// UIDEBUG::_BG_DISPLAY_TEXT needs to render in $Font5. Builds into one
		// reused buffer, so the per-frame HUD text does no heap allocation once
		// its capacity has grown. The returned pointer is valid until the next call.
		const char* BgFormatText(int fontSize, std::initializer_list<std::string_view> parts)
		{
			static std::string buffer;
			std::array<char, 12> digits{};
			const auto sizeEnd = std::to_chars(digits.data(), digits.data() + digits.size(), fontSize).ptr;

			buffer.assign("<TEXTFORMAT RIGHTMARGIN='0'><P ALIGN='Left'><FONT FACE='$Font5' LETTERSPACING='0' SIZE='");
			buffer.append(digits.data(), sizeEnd);
			buffer.append("'>~s~");
			for (const std::string_view part : parts)
				buffer.append(part);
			buffer.append("</FONT></P><TEXTFORMAT>");
			return buffer.c_str();
		}

#ifndef _DEBUG
		constexpr float kReleaseSeatCardIconBaseX = 0.18f;
		constexpr float kReleaseSeatCardIconBaseY = 0.83f;
		constexpr float kReleaseSeatCardIconStepY = -0.0915f;
		constexpr float kReleaseSeatCardIconSpacingX = 0.02f;
		constexpr float kReleaseSeatCardIconWidth = 0.02f;
		constexpr float kReleaseSeatCardIconHeight = 0.045f;
		constexpr float kReleaseSeatCardIconLabelOffsetX = -0.01f;
		constexpr float kReleaseSeatCardIconLabelOffsetY = -0.02f;
#endif

		// relOffset must be 1+ (one dense row per occupied opponent seat,
		// see the header comment above) -- 0 would be your own seat-list
		// row but is never actually called with this file's own drawing
		// logic. Row position is a base (relOffset==1, the first opponent
		// row above you) plus (relOffset-1) steps -- not a per-row lookup
		// table, since every row shares the same X and steps by the same
		// Y. vsMeResult: 1 = you win, -1 = they win, 0 = tie, 2 = no
		// comparison available (e.g. opponent inactive/folded, or you
		// have no hand) -- suppresses the win/lose half of the label.
		// personalityLabel: empty string suppresses the personality half
		// (see Localization::PersonalityLabel()/ShowOpponentPersonality) -- the two
		// halves are independent, either can show without the other.
		// cardSetDict: this frame's already-looked-up card-set dictionary
		// (non-empty), same as DrawCommunityCardIcons().
		void DrawSeatCardIcons(std::string_view cardSetDict, int relOffset, std::int32_t rank0, std::int32_t suit0, std::int32_t rank1, std::int32_t suit1, int vsMeResult, std::string_view personalityLabel)
		{
#ifdef _DEBUG
			const Config::Values& cfg = Config::Get();
			float baseX = cfg.SeatCardIconBaseX;
			float baseY = cfg.SeatCardIconBaseY;
			float stepY = cfg.SeatCardIconStepY;
			float spacingX = cfg.SeatCardIconSpacingX;
			float width = cfg.SeatCardIconWidth;
			float height = cfg.SeatCardIconHeight;
			float labelOffsetX = cfg.SeatCardIconLabelOffsetX;
			float labelOffsetY = cfg.SeatCardIconLabelOffsetY;
#else
			float baseX = kReleaseSeatCardIconBaseX;
			float baseY = kReleaseSeatCardIconBaseY;
			float stepY = kReleaseSeatCardIconStepY;
			float spacingX = kReleaseSeatCardIconSpacingX;
			float width = kReleaseSeatCardIconWidth;
			float height = kReleaseSeatCardIconHeight;
			float labelOffsetX = kReleaseSeatCardIconLabelOffsetX;
			float labelOffsetY = kReleaseSeatCardIconLabelOffsetY;
#endif

			float x = baseX;
			float y = baseY + static_cast<float>(relOffset - 1) * stepY;

			const std::int32_t ranks[kSeatCardIconCount] = { rank0, rank1 };
			const std::int32_t suits[kSeatCardIconCount] = { suit0, suit1 };
			for (int i = 0; i < kSeatCardIconCount; i++)
			{
				if (ranks[i] < 2)
					continue;

				GRAPHICS::DRAW_SPRITE(const_cast<char*>(cardSetDict.data()), const_cast<char*>(BuildCardTextureName(ranks[i], suits[i])), x + static_cast<float>(i) * spacingX, y, width, height, 0.0f, 255, 255, 255, 255, 0);
			}

			// (You Win)/(They Win)/(Tie) label directly below the icons,
			// in a REAL RDR2 font -- $Font5 ("Redemption"), confirmed
			// working by the user via the now-removed DrawFontTest() F10
			// diagnostic (see docs/JOURNAL.md for the full derivation: the
			// plain UI::DRAW_TEXT/SET_TEXT_COLOR_RGBA natives this file
			// uses everywhere else turned out to be nullsub on our build,
			// UIDEBUG::_BG_DISPLAY_TEXT/_BG_SET_TEXT_COLOR -- added to
			// ExtraNatives.h -- is the working replacement, and rich text
			// tags embedded in a LITERAL_STRING through THAT pipeline
			// actually get parsed). No SET_TEXT_CENTRE equivalent exists
			// for this pipeline -- alignment comes from the <P ALIGN=...>
			// tag inside the string itself, so this is left-aligned
			// starting at the icon pair's left edge rather than centered.
			//
			// The personality half (e.g. "Tight-Aggressive") and the
			// win/lose half share this one line, independently toggled --
			// ShowOpponentPersonality/ShowWouldWinHandAgainst -- so either
			// can appear without the other. When the win/lose half is
			// showing, its color drives the whole line (matches the
			// existing behavior exactly); otherwise a neutral cream tone
			// is used, same tone DrawLine()'s Debug-only text panel uses
			// elsewhere in this file, kept local here since that function
			// isn't compiled into Release builds.
			bool showPersonality = Config::Get().ShowOpponentPersonality && !personalityLabel.empty();
			bool showVsMe = Config::Get().ShowWouldWinHandAgainst && vsMeResult != 2;
			if (showPersonality || showVsMe)
			{
				const std::string_view vsLabel = Localization::VerdictLabel(vsMeResult);
				int labelR = showVsMe ? ((vsMeResult > 0) ? 140 : (vsMeResult < 0) ? 255 : 255) : 235;
				int labelG = showVsMe ? ((vsMeResult > 0) ? 255 : (vsMeResult < 0) ? 110 : 230) : 222;
				int labelB = showVsMe ? ((vsMeResult > 0) ? 140 : (vsMeResult < 0) ? 110 : 140) : 194;

				float labelX = x + labelOffsetX;
				float labelY = y + height + labelOffsetY;

				const char* formatText = BgFormatText(30, {
					showPersonality ? personalityLabel : std::string_view(),
					showPersonality && showVsMe ? std::string_view(" - ") : std::string_view(),
					showVsMe ? vsLabel : std::string_view() });

				UIDEBUG::_BG_SET_TEXT_COLOR(labelR, labelG, labelB, 255);
				UIDEBUG::_BG_DISPLAY_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(formatText)), labelX, labelY);
			}
		}

		// Standalone win-prediction status position -- Debug builds read
		// this from Config (PokerCheat.ini's [HUD] section) for live
		// Reload Config tuning; Release bakes in the user-confirmed
		// values directly as constexpr, same convention as the
		// community-card/seat-card icon strips above (see Config.h's
		// header comment on WinPredictionX etc.) -- an end user shouldn't
		// need to calibrate HUD text placement themselves, and Release's
		// PokerCheat.ini never gets a [HUD] section written to it at all
		// (see Config.cpp). This is the only HUD text position Release
		// still needs -- the debug text panel below (PanelX/Y, TextScale,
		// TitleTextScale) has no Release use at all now that the panel
		// itself is Debug-only (see DrawOverlay()).
#ifndef _DEBUG
		constexpr float kReleaseWinPredictionX = 0.48f;
		constexpr float kReleaseWinPredictionY = 0.5f;
#endif

		// Standalone "are you predicted to win" status -- deliberately
		// separate from both the seat card icons (which now only ever
		// show opponents, per user request) and the text panel. Same
		// real-font ($Font5) UIDEBUG pipeline as DrawSeatCardIcons()'s
		// label, positioned independently via Config's WinPredictionX/Y
		// (a rough screen-center starting point, "for now" per the user --
		// not calibrated against anything). Reuses the exact win/lose/tie
		// wording style already established for the opponent labels
		// rather than the old panel verdict's full sentences, so the two
		// read consistently; "not in this hand" simply shows nothing,
		// same as an opponent row with no comparison available.
		void DrawWinPredictionStatus(bool haveMyHand, bool anyOpponent, int worstResult)
		{
			if (!haveMyHand)
				return;

			int result = !anyOpponent ? 1 : (worstResult > 0 ? 1 : worstResult == 0 ? 0 : -1);
			const std::string_view label = Localization::VerdictLabel(result);
			int r = (result > 0) ? 140 : (result < 0) ? 255 : 255;
			int g = (result > 0) ? 255 : (result < 0) ? 110 : 230;
			int b = (result > 0) ? 140 : (result < 0) ? 110 : 140;

#ifdef _DEBUG
			const Config::Values& cfg = Config::Get();
			float winPredictionX = cfg.WinPredictionX;
			float winPredictionY = cfg.WinPredictionY;
#else
			float winPredictionX = kReleaseWinPredictionX;
			float winPredictionY = kReleaseWinPredictionY;
#endif
			const char* formatText = BgFormatText(40, { label });

			UIDEBUG::_BG_SET_TEXT_COLOR(r, g, b, 255);
			UIDEBUG::_BG_DISPLAY_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(formatText)), winPredictionX, winPredictionY);
		}

#ifdef _DEBUG
		// Checked three independent native databases (the stock SDK's
		// natives.h, rdr3-nativedb-data/natives.json, and the decompiler's
		// own bundled natives_rdr.json) for a SET_TEXT_FONT equivalent --
		// none exist. Combined with poker_sp's own HUD elements being
		// driven through UISTATEMACHINE::/DATABINDING:: (a Scaleform-style
		// UI-flow system, confirmed via _UIFLOWBLOCK_REQUEST/
		// _DATABINDING_ADD_DATA_STRING calls elsewhere in the decompile)
		// rather than raw UI::DRAW_TEXT calls, this is strong evidence
		// RDR2's simple legacy text-draw path only ever has one fixed
		// font -- matching the real in-game font exactly isn't achievable
		// this way without reimplementing that whole UI-flow system,
		// which is out of scope. What IS achievable: a real background
		// panel (RDR2's own menus/prompts use a dark, semi-transparent
		// backing behind text, not floating text on nothing), a real
		// drop shadow (was previously called with all-zero params, which
		// just disables it), and a warm parchment/cream text color
		// instead of pure white to at least sit closer to RDR2's actual
		// HUD palette. Debug-only -- this backs the raw-data text panel
		// (DrawOverlay()'s seat-list/board text), which has no Release
		// use at all (see that function's header comment).
		constexpr int kPanelR = 22, kPanelG = 18, kPanelB = 14, kPanelA = 205;
		constexpr int kTextR = 235, kTextG = 222, kTextB = 194, kTextA = 235;
		constexpr int kTitleR = 255, kTitleG = 238, kTitleB = 180, kTitleA = 255;

		void DrawLine(float x, float y, const char* text, bool title = false)
		{
			const Config::Values& cfg = Config::Get();
			float textScale = title ? cfg.TitleTextScale : cfg.TextScale;
			UI::SET_TEXT_SCALE(0.0f, textScale);
			if (title)
				UI::SET_TEXT_COLOR_RGBA(kTitleR, kTitleG, kTitleB, kTitleA);
			else
				UI::SET_TEXT_COLOR_RGBA(kTextR, kTextG, kTextB, kTextA);
			UI::SET_TEXT_CENTRE(0);
			UI::SET_TEXT_DROPSHADOW(1, 0, 0, 0, 200);
			UI::DRAW_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(text)), x, y);
		}

		// Solid backing panel, same convention scriptmenu.cpp's own
		// DrawRect uses (center-x, center-y, width, height, rgba, 0, 0).
		// Sized generously for the maximum possible content (title + 6
		// seats + board + upcoming + verdict) since the real line count
		// varies with how many seats are occupied and isn't known until
		// after the per-seat loop runs -- drawn once, before any text.
		void DrawPanel(float x, float y, float width, float height)
		{
			GRAPHICS::DRAW_RECT(x + width * 0.5f, y + height * 0.5f, width, height, kPanelR, kPanelG, kPanelB, kPanelA, 0, 0);
		}

		// Everything below, through the end of DrawCalibrationGrid(), is
		// Debug-only dev-tuning diagnostics wired to the F10 menu (see
		// PokerCheat.h) -- no caller left anywhere in Release.

		// The real community-card icon strip the user actually wants icons
		// drawn over (top-right, "look like 2D images") is a Scaleform/
		// DATABINDING-driven widget (func_1207/func_1600 ->
		// DATABINDING::_DATABINDING_WRITE_DATA_STRING, same mechanism as
		// the showdown hole-card reveal) -- its real screen position is
		// baked into the game's own .gfx movie layout and isn't exposed to
		// script at all, unlike the 3D community-card props on the table
		// (which DO have a readable world position, see
		// kCommunityCardObjectsField -- confirmed working, just not what's
		// wanted here). No native gives us this coordinate, so it has to
		// be read off the real screen by eye. This draws a normalized
		// (0-1) coordinate grid -- thin lines every 0.05, labeled every
		// 0.1 along the top and left edges -- so that can be done with
		// actual numbers instead of blind guessing.
		// Type-safe replacement for the old fixed char[8] + sprintf_s("%.1f",
		// x) label buffer -- std::ostringstream, no manual buffer size to
		// get wrong.
		std::string FormatFixed1(float value)
		{
			std::ostringstream oss;
			oss << std::fixed << std::setprecision(1) << value;
			return oss.str();
		}

		void DrawCalibrationGrid()
		{
			constexpr float kGridStep = 0.05f;
			constexpr float kLineThickness = 0.0015f;
			constexpr int kGridR = 0, kGridG = 255, kGridB = 120, kGridA = 110;

			for (int i = 0; i <= 20; i++)
			{
				float x = i * kGridStep;
				GRAPHICS::DRAW_RECT(x, 0.5f, kLineThickness, 1.0f, kGridR, kGridG, kGridB, kGridA, 0, 0);
			}
			for (int i = 0; i <= 20; i++)
			{
				float y = i * kGridStep;
				GRAPHICS::DRAW_RECT(0.5f, y, 1.0f, kLineThickness, kGridR, kGridG, kGridB, kGridA, 0, 0);
			}

			UI::SET_TEXT_SCALE(0.0f, 0.25f);
			UI::SET_TEXT_COLOR_RGBA(255, 255, 0, 255);
			UI::SET_TEXT_CENTRE(0);
			UI::SET_TEXT_DROPSHADOW(1, 0, 0, 0, 200);
			for (int i = 0; i <= 10; i++)
			{
				float x = i * 0.1f;
				std::string label = FormatFixed1(x);
				UI::DRAW_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(label.c_str())), x, 0.008f);
			}
			for (int i = 0; i <= 10; i++)
			{
				float y = i * 0.1f;
				std::string label = FormatFixed1(y);
				UI::DRAW_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(label.c_str())), 0.008f, y);
			}
		}

		// See PokerCheat.h's ToggleFontTest() header comment for the full
		// history -- this is the same font test built for Session 11's
		// "a real RDR2 font" investigation (see docs/JOURNAL.md), which
		// tested a fixed English "Face test" string against every
		// candidate FONT FACE token to find $Font5 -- restored here,
		// parameterized by FontTestLanguageIndex (advanced via
		// CycleFontTestLanguage()) so each of Localization.cpp's 13
		// languages' OWN translated text can be tested the same way.
		// RESULT (Session 20/21): every token except $gamername renders
		// all 13 languages correctly, including Chinese/Japanese/Korean --
		// PROVIDED RDR2's own actual configured language matches what's
		// being tested (see docs/PITFALLS.md; testing a CJK language
		// while the game itself still runs in English/whatever shows
		// tofu regardless of token, since the CJK font/text assets are
		// never streamed in at all otherwise). This mod's real HUD calls
		// (DrawSeatCardIcons()/DrawWinPredictionStatus()) already use
		// $Font5, so no change was needed there -- this tool stays wired
		// up for re-verifying after any future game update.
		void DrawFontTest()
		{
			Localization::Language lang = static_cast<Localization::Language>(FontTestLanguageIndex);

			// Personality index 8 ("Tight-Aggressive" in English) is one
			// of the four real corner values ever shown at an actual
			// table (see Localization.cpp's kPersonalityLabels header
			// comment) and, in most languages, the longest of the four --
			// picked so a PARTIAL glyph failure (some characters render,
			// some show as tofu) is easier to spot than with a shorter
			// sample. Paired with the "you win" verdict wording so both
			// of this mod's actual on-screen strings get covered by one
			// sample line.
			std::string sampleText = std::string(Localization::PersonalityLabel(lang, 8)) + " - " + std::string(Localization::VerdictLabel(lang, 1));

			// Same token list Session 11 tried (see the header comment
			// above). $gamername is the sole exception found in Session
			// 20/21's per-language pass -- confirmed NOT to render CJK
			// (likely intended only for the fixed Latin/numeral gamertag
			// charset its real name, "Rockstar Gamertag Cond", implies) --
			// kept in this list anyway so a future run of this tool
			// re-confirms that instead of silently assuming it.
			constexpr const char* kFaceTokens[] = { "$title", "$chalk", "$ledger", "$body1", "$catalog1", "$Font5", "$gamername" };
			constexpr int kRowCount = sizeof(kFaceTokens) / sizeof(kFaceTokens[0]);
			constexpr float kFontTestX = 0.28f;
			constexpr float kFontTestLabelWidth = 0.12f;
			constexpr float kFontTestY = 0.15f;
			constexpr float kFontTestLineHeight = 0.045f;
			constexpr float kFontTestPanelWidth = 0.66f;
			constexpr float kFontTestPanelPadding = 0.012f;

			DrawPanel(kFontTestX - kFontTestPanelPadding, kFontTestY - kFontTestPanelPadding,
				kFontTestPanelWidth + kFontTestPanelPadding * 2.0f,
				static_cast<float>(kRowCount + 1) * kFontTestLineHeight + kFontTestPanelPadding * 2.0f);

			UI::SET_TEXT_SCALE(0.0f, 0.28f);
			UI::SET_TEXT_COLOR_RGBA(kTitleR, kTitleG, kTitleB, kTitleA);
			UI::SET_TEXT_CENTRE(0);
			UI::SET_TEXT_DROPSHADOW(1, 0, 0, 0, 200);
			std::string title = "Font Test " + std::to_string(FontTestLanguageIndex + 1) + "/" + std::to_string(static_cast<int>(Localization::Language::Count))
				+ " (" + std::string(Localization::LanguageCode(lang)) + ") -- which rows below show real characters, not boxes?";
			UI::DRAW_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(title.c_str())), kFontTestX, kFontTestY);

			float y = kFontTestY + kFontTestLineHeight;
			for (int i = 0; i < kRowCount; i++)
			{
				UI::SET_TEXT_SCALE(0.0f, 0.26f);
				UI::SET_TEXT_COLOR_RGBA(kTextR, kTextG, kTextB, kTextA);
				UI::SET_TEXT_CENTRE(0);
				UI::SET_TEXT_DROPSHADOW(1, 0, 0, 0, 200);
				std::string label = std::string(kFaceTokens[i]) + ":";
				UI::DRAW_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(label.c_str())), kFontTestX, y);

				// New (UIDEBUG) pipeline only -- the old UI::DRAW_TEXT/
				// SET_TEXT_COLOR_RGBA pair was already confirmed nullsub
				// on this build back in Session 11 (see ExtraNatives.h),
				// no reason to re-test it here.
				std::string formatText = "<TEXTFORMAT RIGHTMARGIN='0'><P ALIGN='Left'><FONT FACE='" + std::string(kFaceTokens[i]) + "' LETTERSPACING='0' SIZE='30'>~s~" + sampleText + "</FONT></P><TEXTFORMAT>";

				UIDEBUG::_BG_SET_TEXT_COLOR(140, 220, 255, 255);
				UIDEBUG::_BG_DISPLAY_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(formatText.c_str())), kFontTestX + kFontTestLabelWidth, y);

				y += kFontTestLineHeight;
			}
		}

#endif // _DEBUG

		// Hand-scoring/comparison logic lives in its own header
		// (PokerHandEval.h) with zero game dependencies, specifically so
		// tests/PokerHandEvalTests.cpp can link against the exact same
		// code this file uses instead of a hand-copied duplicate that can
		// silently drift out of sync.
		using PokerHandEval::HandScore;
		using PokerHandEval::EvaluateHand;
		using PokerHandEval::CompareHands;

		constexpr int kBoardCardCount = 5;

		// Reads the predicted FINAL board: the cards the engine has REALLY
		// dealt so far (the first engineRevealCount slots of B's Table.f_15)
		// followed by PREDICTED future cards read straight off the deck at
		// the current cursor (see kDeckField's header comment -- the deck
		// is fully shuffled and fixed from hand start, so this isn't a
		// guess). This is what lets every hand be evaluated against the
		// eventual board from the very first frame of preflop.
		//
		// The dealt cards, engineRevealCount and deckCursor must all come
		// from B: B deals a street by drawing from the deck and appending to
		// its own board in the same call, so they always agree. The UI copy
		// (A) can lag several frames behind B (see the A/B comment next to
		// kTableBField) -- pairing A's reveal count with B's cursor made the
		// prediction skip the just-dealt cards and pull in cards that are
		// never dealt, for as long as the dealer's animation held the UI
		// back.
		//
		// Slots that can't be filled because deckCursor/deckCount read as
		// out of range are set to -1/-1 (EvaluateHand() rejects those).
		// Returns how many slots were actually filled (5 unless the deck
		// ran out) -- the one source for the hand evaluation, the
		// PredictionCheck snapshot, and the Debug "Board:" text line.
		int ReadPredictedBoard(rage::scrThread* thread, std::int32_t engineRevealCount, std::int32_t deckCursor, std::int32_t deckCount,
			std::int32_t (&outRanks)[kBoardCardCount], std::int32_t (&outSuits)[kBoardCardCount])
		{
			const ScriptLocal tableB = TableBLocal(thread);
			int filled = 0;
			std::int32_t deckIndex = deckCursor;
			for (std::int32_t i = 0; i < kBoardCardCount; i++)
			{
				if (i < engineRevealCount)
				{
					const ScriptLocal card = BoardCardLocal(tableB, static_cast<std::uint32_t>(i));
					outRanks[i] = card.At(kCardRankField).AsInt32();
					outSuits[i] = card.At(kCardSuitField).AsInt32();
					filled++;
				}
				else if (deckIndex >= 0 && deckIndex < deckCount)
				{
					const ScriptLocal card = DeckCardLocal(thread, deckIndex);
					outRanks[i] = card.At(kCardRankField).AsInt32();
					outSuits[i] = card.At(kCardSuitField).AsInt32();
					deckIndex++;
					filled++;
				}
				else
				{
					outRanks[i] = -1;
					outSuits[i] = -1;
				}
			}

			return filled;
		}

		// Maps each OTHER (non-you) seat to a DENSE row index (1, 2, 3...
		// with no gaps) for DrawSeatCardIcons() -- walks the table in the
		// confirmed direction (decreasing raw seat number from mySeat,
		// wrapping mod 6 -- see DrawSeatCardIcons()'s header comment) but
		// SKIPS unoccupied seats entirely rather than giving them a
		// reserved row, since the real vanilla panel list compacts (an
		// empty seat doesn't leave a blank row -- see the user report this
		// was built to match). outDenseRow must have kSeatCount entries;
		// entries stay 0 for mySeat itself and for any seat with no row
		// (occupiedMarker == -1). Pulled out of DrawOverlay() into its own
		// function specifically so ProbeSeatOccupancy() (the diagnostic
		// this backs) reads the EXACT same logic the actual icon drawing
		// uses, instead of a hand-copied duplicate that could quietly
		// drift out of sync with what's really on screen.
		void ComputeDenseRowForSeat(rage::scrThread* thread, std::int32_t mySeat, int (&outDenseRow)[kSeatCount])
		{
			for (std::uint32_t i = 0; i < kSeatCount; i++)
				outDenseRow[i] = 0;

			if (mySeat < 0 || mySeat >= static_cast<std::int32_t>(kSeatCount))
				return;

			int nextRow = 1;
			for (int rawOffset = 1; rawOffset < static_cast<int>(kSeatCount); rawOffset++)
			{
				int otherSeat = (mySeat - rawOffset + static_cast<int>(kSeatCount)) % static_cast<int>(kSeatCount);
				std::int32_t otherOccupiedMarker = SeatLocal(TableALocal(thread), static_cast<std::uint32_t>(otherSeat)).At(kSeatOccupiedField).AsInt32();
				if (otherOccupiedMarker != -1)
				{
					outDenseRow[otherSeat] = nextRow;
					nextRow++;
				}
			}
		}

		void DrawOverlay()
		{
			auto thread = GamePointers::FindScriptThread(rage::Joaat("poker_sp"));
			if (!thread)
				return;

			const ScriptLocal tableA = TableALocal(thread);
			const ScriptLocal tableB = TableBLocal(thread);
			if (!GamePointers::IsScriptLocalInRange(thread, BoardLocal(tableA).Index()))
				return;

			std::int32_t mySeat = MySeatLocal(thread).AsInt32();

			// Between-hands suppression via poker_sp's internal state
			// fields was abandoned in Session 11 -- four candidates tried,
			// none held up live -- and reinstated in Session 13 via a
			// hole-card-validity heuristic instead (see git history for
			// that version). That heuristic is retired now that
			// kHandStateField's own value range is confirmed directly
			// from poker_sp.ysc.c's state-machine setter (func_213) and
			// cross-checked against live dumps -- see docs/JOURNAL.md,
			// Session 13. States 0-3 are one-time table-entry/launch
			// states (never revisited once a session's first hand
			// starts); 4-10 are the per-hand cycle (4=hole cards dealt,
			// 5=preflop betting, 8=all-in runout, 9=postflop betting all
			// confirmed live; 6/7/10 unconfirmed but grouped with 8/9 by
			// poker_sp's own per-seat eligibility switch); 11-14 are
			// between-hands/settlement (11=between hands confirmed live,
			// 14=payout transitions directly into 11 in the decompile).
			// [4, 10] would be the full "a hand is actually being played"
			// range, but the upper bound is deliberately narrowed to 7
			// here, excluding 8/9/10 (all-in runout/postflop betting) --
			// user-confirmed live that this is exactly when the game
			// shows its OWN hand-resolution text/UI, which this mod's
			// overlay was sitting on top of and obscuring. Cutting the
			// overlay at state 8 reveals that text, and it reappears
			// showing the new hand's prediction the moment state 4 hits
			// again -- confirmed live, this is the desired behavior, not
			// a placeholder.
			std::int32_t handState = HandStateLocal(thread).AsInt32();
			bool handInProgress = (handState >= 4 && handState <= 7);

			// Two reveal counts, on purpose (see the A/B comment next to
			// kTableBField): revealCount is what the game is currently
			// SHOWING (A) and only decides which icons are drawn solid vs.
			// ghosted; engineRevealCount is what has really been dealt (B)
			// and is what the prediction is built from, together with B's
			// deck cursor. They differ only while the UI is holding a
			// "street dealt" event back for the dealer's animation.
			std::int32_t revealCount = BoardLocal(tableA).At(kBoardRevealCountField).AsInt32();
			std::int32_t engineRevealCount = BoardLocal(tableB).At(kBoardRevealCountField).AsInt32();
			std::int32_t deckCursor = DeckLocal(tableB).At(kDeckCursorField).AsInt32();
			std::int32_t deckCount = DeckLocal(tableB).At(kDeckCountField).AsInt32();

#ifdef _DEBUG
			// Live confirmation of the UI-lags-engine window traced from
			// the decompile: logs every change to the (UI reveal count,
			// engine reveal count, f_2010) triple, so the log shows exactly
			// how long -- and in which UI states -- the two counts disagree.
			static std::int32_t s_lastUiReveal = -999;
			static std::int32_t s_lastEngineReveal = -999;
			static std::int32_t s_lastHandState = -999;
			if (revealCount != s_lastUiReveal || engineRevealCount != s_lastEngineReveal || handState != s_lastHandState)
			{
				Log::Write("SyncCheck: UI (A) reveal={} engine (B) reveal={} deck cursor={} f_2010={}{}",
					revealCount, engineRevealCount, deckCursor, handState,
					revealCount != engineRevealCount ? "  <-- UI lagging engine" : "");
				s_lastUiReveal = revealCount;
				s_lastEngineReveal = engineRevealCount;
				s_lastHandState = handState;
			}
#endif

			// Predicted final board (real dealt cards + deterministic
			// future cards off the deck), read once and reused by
			// EvaluateHand() for every seat, the PredictionCheck snapshot,
			// the community-card icons, and the Debug "Board:" line.
			std::int32_t boardRanks[kBoardCardCount];
			std::int32_t boardSuits[kBoardCardCount];
			const int boardCardsFilled = ReadPredictedBoard(thread, engineRevealCount, deckCursor, deckCount, boardRanks, boardSuits);

			// Looked up once per frame and shared by every icon draw below,
			// instead of re-probing all 8 card_set_N dictionaries per call.
			// Only needed while a hand is in progress (no icons draw
			// otherwise). If none is loaded yet, request card_set_1 and
			// skip this frame's icons.
			std::string_view cardSetDict;
			if (handInProgress)
			{
				cardSetDict = FindLoadedCardSetDict();
				if (cardSetDict.empty())
					TEXTURE::REQUEST_STREAMED_TEXTURE_DICT(const_cast<char*>("card_set_1"), false);
			}

			// PREDICTION-VS-REALITY CHECK: two back-to-back "predicted win,
			// actually lost" reports mean the deck-cursor prediction itself
			// needs to be verified empirically, not just re-traced on
			// paper. Snapshot the predicted final
			// board the moment a new hand starts (revealCount==0, deck
			// cursor changed since last snapshot), then diff it against
			// the REAL board once revealCount reaches 5 -- see the matching
			// block after the per-seat loop below. This settles, with a
			// log line instead of memory/guesswork, whether the predicted
			// cards actually match what gets dealt. Both ends use the
			// engine's (B's) board and reveal count, the same source as
			// the prediction: comparing against the UI copy (A) would, in
			// the new-hand window where A still holds the previous hand's
			// full board, log a spurious MISMATCH against the wrong hand.
			static std::int32_t s_preflopCursorSnapshot = -999;
			static std::int32_t s_preflopPredRank[5] = {};
			static std::int32_t s_preflopPredSuit[5] = {};
			static bool s_showdownLogged = false;
			if (engineRevealCount == 0 && deckCursor != s_preflopCursorSnapshot && deckCursor >= 0)
			{
				for (int i = 0; i < kBoardCardCount; i++)
				{
					s_preflopPredRank[i] = boardRanks[i];
					s_preflopPredSuit[i] = boardSuits[i];
				}
				s_preflopCursorSnapshot = deckCursor;
				s_showdownLogged = false;
				Log::Write("PredictionCheck: new hand at deck cursor={} -- predicted final board: {}{} {}{} {}{} {}{} {}{}",
					deckCursor,
					RankName(s_preflopPredRank[0]), SuitLetter(s_preflopPredSuit[0]),
					RankName(s_preflopPredRank[1]), SuitLetter(s_preflopPredSuit[1]),
					RankName(s_preflopPredRank[2]), SuitLetter(s_preflopPredSuit[2]),
					RankName(s_preflopPredRank[3]), SuitLetter(s_preflopPredSuit[3]),
					RankName(s_preflopPredRank[4]), SuitLetter(s_preflopPredSuit[4]));
			}
			if (engineRevealCount >= 5 && !s_showdownLogged && s_preflopCursorSnapshot != -999)
			{
				bool allMatch = true;
				std::string realStr;
				std::string predStr;
				for (int i = 0; i < 5; i++)
				{
					const ScriptLocal realCard = BoardCardLocal(tableB, static_cast<std::uint32_t>(i));
					std::int32_t realRank = realCard.At(kCardRankField).AsInt32();
					std::int32_t realSuit = realCard.At(kCardSuitField).AsInt32();
					realStr += RankName(realRank);
					realStr += SuitLetter(realSuit);
					realStr += ' ';
					predStr += RankName(s_preflopPredRank[i]);
					predStr += SuitLetter(s_preflopPredSuit[i]);
					predStr += ' ';
					if (realRank != s_preflopPredRank[i] || realSuit != s_preflopPredSuit[i])
						allMatch = false;
				}
				Log::Write("PredictionCheck: showdown -- predicted [{}] vs real [{}] -- {}",
					predStr, realStr, allMatch ? "MATCH" : "MISMATCH");
				s_showdownLogged = true;
			}

#ifdef _DEBUG
			// The whole raw-data text panel below (backing rect, title,
			// one "Seat N: rank suit - category (stack, bet) [tag]" line
			// per seat, the "Board: ..." text line) is a dev debugging
			// surface, not one of the four user-facing overlay elements
			// (community card icons/ShowCommunityCards, opponent card
			// icons/ShowOthersCards, the standalone win-prediction status/
			// ShowWinPrediction, the per-opponent win/lose label/
			// ShowWouldWinHandAgainst) -- Debug-only, matching the F10
			// menu it was designed alongside. A live report caught this
			// still drawing in Release even after the menu/keyboard strip,
			// since it was never actually gated by any of those four
			// Config toggles (or _DEBUG) to begin with -- only individual
			// PIECES of each line were (e.g. opponent cards behind
			// ShowOthersCards), never the panel/line itself.
			float x = Config::Get().PanelX;
			float y = Config::Get().PanelY;
			constexpr float kLineHeight = 0.028f;

			// Backing panel first, before any text -- sized generously
			// for the maximum possible content (title + 6 seats + board +
			// upcoming + verdict = 10 lines), since the real line count
			// depends on how many seats are occupied and isn't known
			// until after the per-seat loop below runs.
			constexpr float kPanelPadding = 0.012f;
			constexpr int kMaxLines = 10;
			DrawPanel(x - kPanelPadding, y - kPanelPadding,
				0.36f + kPanelPadding * 2.0f,
				kMaxLines * kLineHeight + kPanelPadding * 2.0f);

			DrawLine(x, y, engineRevealCount >= 5 ? "PokerCheat" : "PokerCheat (predicted final hands)", true);
			y += kLineHeight;
#endif

			// Evaluate my own hand first (if I have one) so every
			// opponent can be compared against it as the main loop goes,
			// instead of only comparing category numbers. Evaluated
			// against the PREDICTED final board, not just the
			// currently-revealed one.
			HandScore myHandScore;
			std::int32_t myCategory = -1;
			bool haveMyHand = false;
			if (handInProgress && mySeat >= 0 && mySeat < static_cast<std::int32_t>(kSeatCount))
			{
				// Hole cards from the engine (B), same source as the board --
				// see the A/B comment next to kTableBField.
				const ScriptLocal mySeatLocal = SeatLocal(tableB, static_cast<std::uint32_t>(mySeat));
				std::int32_t myCard0Rank = HoleCardLocal(mySeatLocal, 0).At(kCardRankField).AsInt32();
				std::int32_t myCard0Suit = HoleCardLocal(mySeatLocal, 0).At(kCardSuitField).AsInt32();
				std::int32_t myCard1Rank = HoleCardLocal(mySeatLocal, 1).At(kCardRankField).AsInt32();
				std::int32_t myCard1Suit = HoleCardLocal(mySeatLocal, 1).At(kCardSuitField).AsInt32();
				if (myCard0Rank >= 2 && myCard1Rank >= 2)
				{
					std::int32_t myRanks[7] = { myCard0Rank, myCard1Rank, boardRanks[0], boardRanks[1], boardRanks[2], boardRanks[3], boardRanks[4] };
					std::int32_t mySuits[7] = { myCard0Suit, myCard1Suit, boardSuits[0], boardSuits[1], boardSuits[2], boardSuits[3], boardSuits[4] };
					myHandScore = EvaluateHand(myRanks, mySuits);
					myCategory = myHandScore.category;
					haveMyHand = myCategory >= 0;
				}
			}

			// Worst-case result across every active opponent: 1 = beating
			// everyone so far, 0 = tied with someone (and losing to no
			// one), -1 = losing to at least one opponent.
			int worstResult = 1;
			bool anyOpponent = false;
			// An opponent still in the pot whose hand couldn't be evaluated
			// (hole cards not readable yet / mid-update). Without this, a
			// table where no opponent was comparable fell through to the
			// "no opponents left" case and showed a confident (You Win).
			bool anyOpponentUnreadable = false;

			// 0 means "you, or no seat maps here" -- see
			// ComputeDenseRowForSeat()'s header comment.
			int denseRowForSeat[kSeatCount];
			ComputeDenseRowForSeat(thread, mySeat, denseRowForSeat);

			for (std::uint32_t seat = 0; seat < kSeatCount; seat++)
			{
				// Occupancy (and the Debug stack/bet text) from the UI copy
				// (A), so rows line up with the seat panels the game is
				// actually showing -- the same source ComputeDenseRowForSeat()
				// uses. Fold state and hole cards from the engine (B), the
				// same source as the predicted board, so every seat is
				// evaluated against cards from the same moment -- see the A/B
				// comment next to kTableBField.
				const ScriptLocal uiSeat = SeatLocal(tableA, seat);
				const ScriptLocal engineSeat = SeatLocal(tableB, seat);

				// func_143's own check (Table.f_39[seat] != -1, word 0 of
				// the seat struct) -- is anyone seated here at all.
				std::int32_t occupiedMarker = uiSeat.At(kSeatOccupiedField).AsInt32();
				if (occupiedMarker == -1)
					continue; // empty seat, don't draw a line for it

				// seat.f_6: confirmed via poker_sp.ysc.c func_912/func_475/
				// func_476 (-1=empty [already excluded above], 0=active,
				// 1=folded, 2=all-in) -- func_475's call site (line ~9430)
				// draws a "folded" status icon exactly when f_6==1,
				// confirming the mapping.
				std::int32_t state = engineSeat.At(kSeatStateField).AsInt32();
				bool isActive = (state == 0 || state == 2); // still eligible to win the pot

#ifdef _DEBUG
				// seat.f_2 (stack) / seat.f_3 (current bet) / the fold/
				// all-in tag -- debug text panel only, not used by any of
				// the four Release overlay elements.
				const char* stateLabel = (state == 1) ? " [FOLDED]" : (state == 2) ? " [ALL-IN]" : "";
				std::int32_t stack = uiSeat.At(kSeatStackField).AsInt32();
				std::int32_t bet = uiSeat.At(kSeatBetField).AsInt32();
#endif

				std::int32_t card0Rank = HoleCardLocal(engineSeat, 0).At(kCardRankField).AsInt32();
				std::int32_t card0Suit = HoleCardLocal(engineSeat, 0).At(kCardSuitField).AsInt32();
				std::int32_t card1Rank = HoleCardLocal(engineSeat, 1).At(kCardRankField).AsInt32();
				std::int32_t card1Suit = HoleCardLocal(engineSeat, 1).At(kCardSuitField).AsInt32();

				std::int32_t personalityIndex = PersonalityLocal(thread, seat).AsInt32();
				const std::string_view personalityLabel = Localization::PersonalityLabel(personalityIndex);

				bool isMe = (static_cast<std::int32_t>(seat) == mySeat);

				if (handInProgress && isActive && !isMe && (card0Rank < 2 || card1Rank < 2))
					anyOpponentUnreadable = true;

				if (!handInProgress || card0Rank < 2 || card1Rank < 2)
				{
#ifdef _DEBUG
					std::ostringstream line;
					line << "Seat " << seat << ": --- (stack " << stack << ", bet " << bet << ")" << stateLabel;
					DrawLine(x, y, line.str().c_str());
					y += kLineHeight;
#endif
					continue; // no valid hand here -- nothing else to compute for this seat
				}

				std::int32_t category = -1;
				const char* vsMe = "";
				int vsMeResult = 2; // 2 = no comparison available -- see DrawSeatCardIcons()'s header comment

				// Hand evaluation/comparison always runs regardless of
				// display settings below -- the final verdict line
				// (ShowWinPrediction) needs every active opponent's
				// worst-case comparison even when their individual
				// cards/hand aren't being shown (ShowOthersCards off).
				if (isActive)
				{
					if (isMe)
					{
						category = myCategory;
					}
					else
					{
						std::int32_t oppRanks[7] = { card0Rank, card1Rank, boardRanks[0], boardRanks[1], boardRanks[2], boardRanks[3], boardRanks[4] };
						std::int32_t oppSuits[7] = { card0Suit, card1Suit, boardSuits[0], boardSuits[1], boardSuits[2], boardSuits[3], boardSuits[4] };
						HandScore oppScore = EvaluateHand(oppRanks, oppSuits);
						category = oppScore.category;

						if (haveMyHand && category >= 0)
						{
							anyOpponent = true;
							int cmp = CompareHands(myHandScore, oppScore); // >0 I win, <0 they win, 0 tie
							vsMe = (cmp > 0) ? " [you win]" : (cmp < 0) ? " [they win]" : " [tie]";
							vsMeResult = (cmp > 0) ? 1 : (cmp < 0) ? -1 : 0;
							if (cmp < 0)
								worstResult = -1;
							else if (cmp == 0 && worstResult > 0)
								worstResult = 0;
						}
						else if (category < 0)
						{
							anyOpponentUnreadable = true;
						}
					}
				}

				const Config::Values& cfg = Config::Get();

#ifdef _DEBUG
				// Your own seat is always shown in full -- that's your
				// own hand, not hidden information. Opponents' cards/
				// hand name are gated by ShowOthersCards; the per-seat
				// win/lose/tie tag is itself a prediction, so it's
				// additionally gated by ShowWinPrediction, same as the
				// final verdict line below.
				std::ostringstream line;
				if (!isMe && !cfg.ShowOthersCards)
				{
					line << "Seat " << seat << ": (stack " << stack << ", bet " << bet << ")" << stateLabel;
				}
				else
				{
					const char* shownVsMe = (isMe || !cfg.ShowWinPrediction) ? "" : vsMe;
					line << "Seat " << seat << ": "
						<< RankName(card0Rank) << SuitLetter(card0Suit) << ' '
						<< RankName(card1Rank) << SuitLetter(card1Suit) << " - "
						<< (category >= 0 ? HandCategoryName(category) : "?")
						<< " (stack " << stack << ", bet " << bet << ")"
						<< stateLabel << shownVsMe << (isMe ? "  (You)" : "");
					if (!isMe && !personalityLabel.empty())
						line << "  [" << personalityLabel << "]";
				}
				DrawLine(x, y, line.str().c_str());
				y += kLineHeight;
#endif

				// Icons next to the opponent's own name/panel on the REAL
				// vanilla HUD -- see DrawSeatCardIcons()'s header comment
				// for the relative-seat-offset mapping. Uses the DENSE row
				// computed above (denseRowForSeat), not the raw seat-number
				// offset directly, so an unoccupied seat doesn't leave a
				// gap in the list. vsMeResult carries the (You Win)/(They
				// Win)/(Tie) comparison down into the icon draw itself.
				// Gated on isActive -- a folded seat's real cards are still
				// in memory, but per user report the icon overlay
				// shouldn't keep revealing a folded opponent's cards once
				// they're out of the pot. Gated directly on ShowOthersCards
				// here (this used to be implicit, nested inside the debug
				// text panel's own ShowOthersCards branch above -- now that
				// the text panel is Debug-only, this is the one and only
				// place left enforcing that toggle for the actual HUD
				// icons in Release).
				if (!isMe && isActive && cfg.ShowOthersCards)
				{
					int denseRow = denseRowForSeat[seat];
					if (denseRow != 0 && !cardSetDict.empty())
						DrawSeatCardIcons(cardSetDict, denseRow, card0Rank, card0Suit, card1Rank, card1Suit, vsMeResult, personalityLabel);
				}
			}

			// Board (community cards) -- ONE line, always showing all 5
			// eventual cards, not just the currently-revealed ones (this
			// used to be two separate lines -- "Board:" showing only the
			// real revealed cards, correctly reading "(preflop)" with
			// nothing revealed yet, and a separate "Upcoming:" line for
			// the prediction -- which read correctly but wasn't where
			// the prediction was expected to show up). Already-dealt
			// cards are read from the engine's board (B's Table.f_15); anything
			// not dealt yet is the exact future card read straight off
			// the deck at the current cursor (see kDeckField's header
			// comment -- deterministic, not a guess). Anything the game
			// isn't SHOWING yet (index >= the UI copy's revealCount) is
			// marked with a trailing "*" / drawn ghosted. Gated by ShowCommunityCards -- both the text
			// line and the 2D icon strip; boardRanks/boardSuits (used for
			// hand evaluation above) are computed unconditionally either
			// way. Also gated by handInProgress -- between hands the
			// board slots can still hold the previous hand's stale data
			// (see handInProgress's header comment above), and the
			// vanilla HUD itself hides during that window too.
			if (Config::Get().ShowCommunityCards && handInProgress)
			{
#ifdef _DEBUG
				// Debug-only text rendering of the same board data the
				// icon strip below draws. ReadPredictedBoard() fills the
				// revealed slots first and stops filling at the first deck
				// read that's out of range, so the first boardCardsFilled
				// slots are exactly the cards there are to show.
				std::ostringstream boardLine;
				boardLine << "Board: ";
				for (int i = 0; i < boardCardsFilled; i++)
				{
					bool predicted = i >= revealCount;
					boardLine << RankName(boardRanks[i]) << SuitLetter(boardSuits[i]) << (predicted ? "*" : "") << ' ';
				}

				// Fewer than 5 filled means deckCursor/deckCount read as out
				// of range (a fully dealt board always fills all 5 without
				// touching the deck) -- e.g. the deck hasn't been
				// shuffled/dealt yet at the moment this ran. That's a real,
				// different situation from "nothing left to predict" and
				// shouldn't look the same on screen.
				if (boardCardsFilled < kBoardCardCount)
					boardLine << "(deck not ready -- cursor/count out of range)";

				DrawLine(x, y, boardLine.str().c_str());
				y += kLineHeight;
#endif

				// Real card-face icons at the calibrated top-right strip
				// position, using the same real/predicted split as the text
				// "Board:" line above (boardRanks/boardSuits already hold the
				// predicted-final-board's 5 cards, computed once up front).
				if (!cardSetDict.empty())
					DrawCommunityCardIcons(cardSetDict, boardRanks, boardSuits, revealCount);
			}

			// Real verdict, predicted to showdown -- CompareHands() above
			// scores both hands itself (category, then kicker-by-kicker;
			// see the file header comment for why this is no longer
			// delegated to the game's own hand-rank native), and both
			// hands were evaluated against the PREDICTED final board (real
			// revealed cards + exact future cards read off the deck), so
			// this is the actual eventual outcome given nobody folds/the
			// hand runs to showdown, not just "who's ahead right now".
			// A user report of "it said I'd win and I lost" turned out to
			// be a real bug, not noise: the deck read was pulling from
			// Candidate A (uLocal_14.f_114.f_287) when poker_sp's own
			// func_285/func_590 both use Candidate B (f_1276) for the
			// live shuffled deck. Fixed (deck reads now go through
			// Candidate B) and confirmed via ProbeTableStruct: Candidate B
			// now logs cursor=8, count=52 mid-hand, a sane live deck --
			// see docs/JOURNAL.md. Verdict wording restored to confident.
			// Standalone center-screen status -- NOT part of the seat
			// list/text panel at all (per user request: opponents only
			// get card icons + win/lose labels near their own cards; your
			// own predicted result is a separate, independent readout).
			// Gated by ShowWinPrediction -- worstResult/anyOpponent above
			// are still always computed regardless (needed so the
			// per-seat comparison flows through correctly either way).
			if (handInProgress && Config::Get().ShowWinPrediction)
				DrawWinPredictionStatus(haveMyHand && !anyOpponentUnreadable, anyOpponent, worstResult);
		}
	}

	// Bet hotkeys, ported from ..\BlackjackCheat's: Right/Left arrow move
	// the amount 5 chips (5x what one press of the game's own Up/Down
	// does), Tab jumps to the most the game allows -- all-in or the table
	// cap on your turn, the table's max buy-in (or all your cash) on the
	// buy-in prompt. Keys are read raw (keyboard.h). All three write the
	// open UI's amount directly (see BetInputLocal()'s comment) -- the
	// mod's only memory write -- clamped exactly like the game's own
	// func_1252/func_985, so every value is one the game could have reached
	// itself. The game then labels its own prompt from it (Check/Call/Bet/
	// Raise/All In) and places the bet from it.
	//
	// Prompts: Tab gets one of ours next to the game's own (same
	// priority, no group), on INPUT_MINIGAME_POKER_SKIP -- which the game
	// itself only prompts outside your turn, plus Strauss's hint in his
	// mission -- labelled with the game's own "All-in"/"Max Bet" (see
	// kAllInLabel). Registered once, on the UI's first frame:
	// BlackjackCheat found prompts registered later never show.
	//
	// Left/Right get NO prompt of their own -- the step goes on the game's
	// own "Amount" prompt instead (RelabelAlterPrompt()). Why, all live:
	// a prompt shows only if EVERY control on it is in the active input
	// context, and the active context is just the TOP layer of
	// _SET_CONTROL_CONTEXT's stack (its first argument is a layer, not a
	// control type): at your turn layer 0 = OnFoot, layer 4 = MinigamePoker
	// (poker_sp's func_10 sets it every frame), and OnFoot's controls are
	// NOT active. No MinigamePoker control is on the arrow keys, so:
	//  - every arrow control (GAME_MENU_, FRONTEND_, FRONTEND_NAV_,
	//    FRONTEND_MAP_NAV_LEFT/RIGHT, DOCUMENT_PAGE_PREV/NEXT) -> hidden,
	//    alone or with an in-context control added, in either order;
	//  - an in-context control with no binding at all
	//    (MULTIPLAYER_INFO_PLAYERS, POKER_CHEAT_LR) -> hidden;
	//  - HELP_PREV (in context, gamepad-only) -> shows, but with its
	//    gamepad D-pad-left icon even on keyboard;
	//  - not a visible-prompt limit: 8 showed at once, and an arrow prompt
	//    registered in All-in's place, with All-in gone, stayed hidden;
	//  - MinigameBlackjack on layer 5 (every frame): the arrow prompt
	//    SHOWED -- but it replaced MinigamePoker, hiding Bet/Fold/Your
	//    Cards/Community Cards and breaking poker's controls.
	// The only way left would be patching MinigamePoker's control list in
	// RDR2.exe's memory -- judged not worth it.
	namespace
	{
		constexpr Hash kBetMaxPromptControl = rage::Joaat("INPUT_MINIGAME_POKER_SKIP");
		static_assert(kBetMaxPromptControl == 0x646A7792);

		// Tab's prompt text: the game's own labels from its MGPKR text block
		// (poker_sp loads it -- TEXT_BLOCK_REQUEST("MGPKR") -- before any
		// prompt of ours can exist), so it's already in the player's
		// language. The plain forms of what func_1252 shows as "All-in
		// (~1$~)"/"Max Bet (~1$~)" (MGPKR_UI_ALLIN/MGPKR_UI_MAX_BET),
		// found in mgpkr.yldb from each <lang>_rel.rpf: en "All-in"/"Max
		// Bet", fr "Tapis"/"Mise maximum", de "All-in"/"Maximaler Einsatz".
		// All-in when Tab puts in your whole stack, like func_1252's own
		// choice; Max Bet when the table cap or a call-only turn is lower,
		// and on the buy-in prompt.
		constexpr const char* kAllInLabel = "MGPKR_INFO_ALLIN_DONE";
		constexpr const char* kMaxBetLabel = "MGPKR_INFO_MAX_BET_DONE";

		constexpr std::int32_t kBetHotkeySteps = 5;
		constexpr int kPromptPriority = 3; // the game's own bet prompts' (func_450's 11th argument)

		enum class AmountInput
		{
			None,
			Bet,
			BuyIn,
		};

		struct BetHotkeyState
		{
			AmountInput input = AmountInput::None; // the UI the prompts were registered for
			Prompt max = 0;
			bool alterRelabeled = false;

			// Right/Left hold-to-repeat, func_984's state (see BetRepeat).
			int heldDirection = 0;  // +1 Right, -1 Left, 0 neither
			float accumulated = 0.0f;
			float rate = 0.0f;      // steps per second
		};
		BetHotkeyState g_betHotkeys;

		// `text` is a VAR_STRING result: a game label, or LITERAL_STRING.
		Prompt RegisterBetPrompt(std::initializer_list<Hash> controls, const char* text)
		{
			Prompt prompt = HUD::_UI_PROMPT_REGISTER_BEGIN();
			for (Hash control : controls)
				HUD::_UI_PROMPT_SET_CONTROL_ACTION(prompt, control);
			HUD::_UI_PROMPT_SET_TEXT(prompt, text);
			// Same setup as the game's own prompts (func_450 -> func_1076):
			// priority 3, transport mode 0, attribute 18, standard mode, then
			// visible + enabled after registering (func_1531/func_1532).
			HUD::_UI_PROMPT_SET_PRIORITY(prompt, kPromptPriority);
			HUD::_UI_PROMPT_SET_TRANSPORT_MODE(prompt, 0);
			HUD::_UI_PROMPT_SET_ATTRIBUTE(prompt, 18, true);
			HUD::_UI_PROMPT_SET_STANDARD_MODE(prompt, false);
			HUD::_UI_PROMPT_REGISTER_END(prompt);
			HUD::_UI_PROMPT_SET_VISIBLE(prompt, true);
			HUD::_UI_PROMPT_SET_ENABLED(prompt, true);
			return prompt;
		}

		void ResetBetHotkeys()
		{
			if (g_betHotkeys.max != 0 && HUD::_UI_PROMPT_IS_VALID(g_betHotkeys.max))
				HUD::_UI_PROMPT_DELETE(g_betHotkeys.max);
			g_betHotkeys = BetHotkeyState{};
		}

		// func_984's hold-to-repeat, with the game's own values for both
		// UIs (the static initial values of f_281.f_5/f_298.f_3, the same
		// as blackjack's): a press moves one step at once, holding repeats
		// at 7 steps/s and speeds up x1.08 per repeat, capped at 200/s.
		// Here a step is the 5-chip jump.
		constexpr float kRepeatStartRate = 7.0f;
		constexpr float kRepeatMaxRate = 200.0f;
		constexpr float kRepeatRateGrowth = 1.08f;

		// Steps to move this frame for the held direction (+1/-1/0).
		std::int32_t BetRepeat(int direction)
		{
			BetHotkeyState& state = g_betHotkeys;
			if (direction != state.heldDirection)
			{
				state.heldDirection = direction;
				state.accumulated = 0.0f;
				state.rate = kRepeatStartRate;
				return direction;
			}
			if (direction == 0)
				return 0;

			state.accumulated += MISC::GET_FRAME_TIME() * state.rate;
			if (state.accumulated < 1.0f)
				return 0;

			const std::int32_t steps = static_cast<std::int32_t>(std::lround(state.accumulated));
			state.accumulated = 0.0f;
			state.rate = (std::min)(state.rate * kRepeatRateGrowth, kRepeatMaxRate);
			return direction * steps;
		}

		// The bet/raise UI's limits, in chips on top of seat.f_4 --
		// func_1614(table, settings, seat, ..., true): `min` is 0 (check)
		// or the call, `minRaise` the smallest raise, `max` the most you
		// can put in (your stack, the table cap, or just the call if you
		// may not raise).
		struct BetLimits
		{
			std::int32_t min = 0;
			std::int32_t minRaise = 0;
			std::int32_t max = -1;
			std::int32_t stack = 0; // seat.f_2 -- max == stack is all-in (func_1252's MGPKR_UI_ALLIN check)
		};

		BetLimits BetInputLimits(rage::scrThread* thread)
		{
			const std::int32_t seat = MySeatLocal(thread).AsInt32();
			if (seat < 0 || seat >= static_cast<std::int32_t>(kSeatCount))
				return {};

			const ScriptLocal table = TableALocal(thread);
			const ScriptLocal seatLocal = SeatLocal(table, static_cast<std::uint32_t>(seat));
			const ScriptLocal settings = SettingsLocal(thread);
			const std::int32_t call = table.At(kTableCallField).AsInt32();
			const std::int32_t streetBet = seatLocal.At(kSeatStreetBetField).AsInt32();

			BetLimits limits;
			limits.min = call;
			limits.minRaise = (call == 0) ? table.At(kTableOpenBetField).AsInt32() : call + table.At(kTableRaiseField).AsInt32();
			limits.stack = seatLocal.At(kSeatStackField).AsInt32();
			limits.max = limits.stack + streetBet;

			const std::int32_t cap = settings.At(kSettingsCapField).AsInt32();
			if (settings.At(kSettingsLimitTypeField).AsInt32() == kCappedLimitType && cap > 0)
				limits.max = (std::min)(limits.max, cap - seatLocal.At(kSeatHandTotalField).AsInt32() + streetBet);
			if (seatLocal.At(kSeatCanRaiseField).AsInt32() == 0)
				limits.max = (std::min)(limits.max, call);

			limits.min = (std::min)(limits.min, limits.max) - streetBet;
			limits.minRaise = (std::min)(limits.minRaise, limits.max) - streetBet;
			limits.max -= streetBet;
			return limits;
		}

		// func_1252's clamp: below the call -> the call; between the call
		// and the minimum raise -> whichever of the two the move heads to.
		std::int32_t ClampBet(const BetLimits& limits, std::int32_t wanted, std::int32_t delta)
		{
			if (wanted < limits.min)
				return limits.min;
			if (wanted > limits.min && wanted < limits.minRaise)
				return (delta < 0) ? limits.min : limits.minRaise;
			if (wanted > limits.max)
				return limits.max;
			return wanted;
		}

		// Writes a new amount, with func_986's sound.
		void SetAmount(const ScriptLocal& amountLocal, std::int32_t value, bool atLimit, std::string_view key)
		{
			const std::int32_t amount = amountLocal.AsInt32();
			if (value == amount)
				return;

			const bool written = amountLocal.SetInt32(value);
			AUDIO::_STOP_SOUND_WITH_NAME("BET_AMOUNT", "HUD_POKER");
			AUDIO::PLAY_SOUND_FRONTEND(atLimit ? "BET_MIN_MAX" : "BET_AMOUNT", "HUD_POKER", true, 0);
			Log::Write("BetHotkeys: {} set the amount {} -> {} chips{}", key, amount, value, written ? "" : " (write FAILED)");
		}

		// "$2.50" -- cents as dollars.
		void AppendDollars(std::string& out, std::int32_t cents)
		{
			std::array<char, 12> digits{};
			out.push_back('$');
			out.append(digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), cents / 100).ptr);
			out.push_back('.');
			out.push_back(static_cast<char>('0' + (cents % 100) / 10));
			out.push_back(static_cast<char>('0' + cents % 10));
		}

		// Puts the step on the game's own "Amount" prompt (MGPKR_UI_ALTER,
		// its Up/Down): "-/+$1.25 <Left><Right> Amount", the arrows as inline
		// control icons (`~INPUT_...~`, as the game's own help text does).
		// The game draws a prompt's own icons AFTER its text, so this reads
		// "-/+$1.25 <Left><Right> Amount <Up><Down>" -- each label before its
		// keys, like every other line ("Fold F"). Live: the inline icons
		// render; being text, they don't light up on a press -- only the
		// prompt's own Up/Down does.
		// "Amount" is read back from the game in the player's language. The
		// game sets that prompt's text only when it creates it, and deletes
		// it with the UI, so nothing to restore. Returns false until the
		// game has created it.
		bool RelabelAlterPrompt(rage::scrThread* thread, AmountInput input, std::int32_t chipValue)
		{
			const std::int32_t slot = (input == AmountInput::Bet)
				? BetInputLocal(thread).At(kBetInputAlterPromptField).AsInt32()
				: BuyInInputLocal(thread).At(kBuyInInputAlterPromptField).AsInt32();
			if (slot <= 0 || slot > kPromptPoolLastSlot)
				return false;
			const Prompt prompt = PromptHandleGlobal(static_cast<std::uint32_t>(slot)).AsInt32();
			if (!HUD::_UI_PROMPT_IS_VALID(prompt))
				return false;

			std::string label("-/+");
			AppendDollars(label, kBetHotkeySteps * chipValue);
			label.append(" ~INPUT_GAME_MENU_LEFT~~INPUT_GAME_MENU_RIGHT~ ");
			label.append(HUD::GET_STRING_FROM_HASH_KEY(rage::Joaat("MGPKR_UI_ALTER")));
			HUD::_UI_PROMPT_SET_TEXT(prompt, MISC::VAR_STRING(10, "LITERAL_STRING", label.c_str()));
			Log::Write("BetHotkeys: relabeled the game's Amount prompt (slot {}, handle {}) to \"{}\"", slot, prompt, label);
			return true;
		}

		AmountInput OpenAmountInput(rage::scrThread* thread)
		{
			const ScriptLocal bet = BetInputLocal(thread);
			if (bet.AsInt32() != 0 && bet.At(kBetInputResultField).AsInt32() == 0)
				return AmountInput::Bet;
			const ScriptLocal buyIn = BuyInInputLocal(thread);
			if (buyIn.AsInt32() != 0 && buyIn.At(kBuyInInputResultField).AsInt32() == 0)
				return AmountInput::BuyIn;
			return AmountInput::None;
		}

		void UpdateBetHotkeys(rage::scrThread* thread)
		{
			const AmountInput input = thread ? OpenAmountInput(thread) : AmountInput::None;
			if (input != g_betHotkeys.input)
				ResetBetHotkeys();
			if (input == AmountInput::None)
				return;

			const std::int32_t chipValue = PromptHudLocal(thread).At(kChipValueField).AsInt32();
			if (chipValue <= 0)
				return;

			if (g_betHotkeys.input == AmountInput::None)
			{
				bool allIn = false;
				if (input == AmountInput::Bet)
				{
					const BetLimits limits = BetInputLimits(thread);
					allIn = limits.max == limits.stack;
				}
				g_betHotkeys.input = input;
				g_betHotkeys.max = RegisterBetPrompt({ kBetMaxPromptControl }, MISC::VAR_STRING(2, allIn ? kAllInLabel : kMaxBetLabel));
			}
			if (!g_betHotkeys.alterRelabeled)
				g_betHotkeys.alterRelabeled = RelabelAlterPrompt(thread, input, chipValue);

			// Right wins if both are held, like func_1252's INCREASE-first
			// else-if. IsKeyDown stays true while Windows keeps sending
			// auto-repeat keydowns.
			const bool rightHeld = IsKeyDown(VK_RIGHT);
			const bool leftHeld = IsKeyDown(VK_LEFT);
			const std::int32_t steps = BetRepeat(rightHeld ? 1 : leftHeld ? -1 : 0);
			// A Tab released with Alt held (Alt+Tab, or Tab during Alt's
			// free-look camera) isn't a max bet. Uses the Alt flag on the Tab
			// event itself: Alt's own release can be lost when Alt+Tab takes
			// focus away (BlackjackCheat, live). Consumed either way.
			bool tab = false;
			if (IsKeyJustUp(VK_TAB, false))
			{
				tab = !IsKeyWithAlt(VK_TAB);
				ResetKeyState(VK_TAB);
				if (!tab)
					Log::Write("BetHotkeys: ignored Tab released with Alt held");
			}
			if (steps == 0 && !tab)
				return;

			const std::string_view key = tab ? "Tab" : (steps > 0) ? "Right" : "Left";
			const std::int32_t delta = steps * kBetHotkeySteps;
			if (input == AmountInput::Bet)
			{
				const BetLimits limits = BetInputLimits(thread);
				if (limits.max < limits.min)
					return;
				const ScriptLocal amountLocal = BetInputLocal(thread).At(kBetInputAmountField);
				const std::int32_t value = tab ? limits.max : ClampBet(limits, amountLocal.AsInt32() + delta, delta);
				SetAmount(amountLocal, value, value == limits.min || value == limits.max, key);
			}
			else
			{
				// func_390: each buy-in limit capped at your cash, in chips.
				const std::int32_t cashChips = MONEY::_MONEY_GET_CASH_BALANCE() / chipValue;
				const ScriptLocal settings = SettingsLocal(thread);
				const std::int32_t min = (std::min)(settings.At(kSettingsBuyInMinField).AsInt32(), cashChips);
				const std::int32_t max = (std::min)(settings.At(kSettingsBuyInMaxField).AsInt32(), cashChips);
				if (max < min)
					return;
				const ScriptLocal amountLocal = BuyInInputLocal(thread).At(kBuyInInputAmountField);
				const std::int32_t value = tab ? max : (std::max)(min, (std::min)(amountLocal.AsInt32() + delta, max));
				SetAmount(amountLocal, value, value == min || value == max, key);
			}
		}
	}

	void OnTick()
	{
#ifdef _DEBUG
		if (CalibrationGridEnabled)
			DrawCalibrationGrid();
		if (FontTestEnabled)
			DrawFontTest();
#endif

		if (!Enabled)
		{
			ResetBetHotkeys();
			return;
		}

		if (Config::Get().BetHotkeys)
			UpdateBetHotkeys(GamePointers::FindScriptThread(rage::Joaat("poker_sp")));
		else
			ResetBetHotkeys();

		DrawOverlay();
	}

#ifdef _DEBUG
	void ProbeTableStruct()
	{
		auto thread = GamePointers::FindScriptThread(rage::Joaat("poker_sp"));
		if (!thread)
		{
			Log::Write("ProbeTableStruct: poker_sp is not currently running");
			return;
		}

		Log::Write("ProbeTableStruct: poker_sp thread found (id={}, stack={:#x}, stackSize={})",
			thread->m_Context.m_ThreadId,
			reinterpret_cast<std::uintptr_t>(thread->m_Stack),
			thread->m_Context.m_StackSize);

		Log::Write("ProbeTableStruct: thread->m_ArgsPointer = {}, m_ArgsSize = {}",
			thread->m_ArgsPointer, thread->m_ArgsSize);

		const ScriptLocal stakesTierLocal = LaunchArgsLocal(thread).At(kLaunchArgsStakesTierField);
		std::int32_t stakesTierArg = stakesTierLocal.AsInt32();
		Log::Write("ProbeTableStruct: LaunchArgs.f_12 (slot {}) = {}",
			stakesTierLocal.Index(), stakesTierArg);

		std::int32_t frameworkHash = FrameworkHashLocal(thread).AsInt32();
		bool hashMatch = false;
		for (std::int32_t known : kKnownStakesHashes)
		{
			if (frameworkHash == known)
			{
				hashMatch = true;
				break;
			}
		}

		Log::Write("ProbeTableStruct: uLocal_14.f_1.f_39 (slot {}) = {} -- {}",
			FrameworkHashLocal(thread).Index(), frameworkHash,
			hashMatch ? "EXACT MATCH to a known stakes hash" : "no match");

		std::int32_t seatIndex = MySeatLocal(thread).AsInt32();
		Log::Write("ProbeTableStruct: uLocal_14.f_114.f_9 (your seat) = {}", seatIndex);

		// uLocal_14 (raw) is the root struct's first word itself -- not
		// tied to any traced meaning, just a cheap, broad diagnostic
		// alongside the real round-phase candidates.
		std::int32_t handState = HandStateLocal(thread).AsInt32();
		std::int32_t f1State = F1StateLocal(thread).AsInt32();
		std::int32_t subStep = SubStepLocal(thread).AsInt32();
		std::int32_t localRaw = RootLocal(thread).AsInt32();
		Log::Write("ProbeTableStruct: round-phase candidates -- uLocal_14.f_114.f_2010 = {}, uLocal_14.f_1.f_42 = {}, uLocal_14.f_114.f_2011 = {}, uLocal_14 (raw) = {}",
			handState, f1State, subStep, localRaw);

		const ScriptLocal tableA = TableALocal(thread);
		const ScriptLocal tableB = TableBLocal(thread);
		std::int32_t boardHeader = BoardLocal(tableA).AsInt32();
		std::int32_t revealCount = BoardLocal(tableA).At(kBoardRevealCountField).AsInt32();
		Log::Write("ProbeTableStruct: Table.f_15 (board) header={} (expect 11), reveal count={} (expect 0/3/4/5)",
			boardHeader, revealCount);

		// Traced func_589's (deck init) two call sites to their enclosing
		// functions (func_285/func_590) and confirmed, via their own
		// call sites, that BOTH operate on Candidate B (f_1276) -- the
		// real shuffled gameplay deck lives there, not Candidate A
		// (f_287) that the UI copy's board/seats are read from. Deck reads
		// use Candidate B accordingly. Still logging Candidate
		// A's deck area too, purely to confirm empirically that it does
		// NOT look like a valid live deck (expected: stale/unshuffled or
		// simply not count=52) now that we're not relying on it.
		std::int32_t deckCursorA = DeckLocal(tableA).At(kDeckCursorField).AsInt32();
		std::int32_t deckCountA = DeckLocal(tableA).At(kDeckCountField).AsInt32();
		Log::Write("ProbeTableStruct: Candidate A (f_287) deck cursor={}, count={} -- expected NOT to look like a valid live deck now",
			deckCursorA, deckCountA);

		std::int32_t deckCursor = DeckLocal(tableB).At(kDeckCursorField).AsInt32();
		std::int32_t deckCount = DeckLocal(tableB).At(kDeckCountField).AsInt32();
		Log::Write("ProbeTableStruct: Candidate B (f_1276, now used for all deck reads) cursor={}, count={} (expect count=52) -- next 8 undrawn cards:",
			deckCursor, deckCount);
		for (std::int32_t i = 0; i < 8; i++)
		{
			std::int32_t idx = deckCursor + i;
			if (idx < 0 || idx >= deckCount)
				break;
			const ScriptLocal card = DeckCardLocal(thread, idx);
			std::int32_t rank = card.At(kCardRankField).AsInt32();
			std::int32_t suit = card.At(kCardSuitField).AsInt32();
			Log::Write("  deck[{}]: {}{}", idx, RankName(rank), SuitLetter(suit));
		}

		std::int32_t seatsHeader = tableA.At(kSeatsField).AsInt32();
		Log::Write("ProbeTableStruct: Table.f_39 (seats) header={} (expect 6)", seatsHeader);

		Log::Write("ProbeTableStruct: hole cards per seat (rank 2-14, suit 0-3 [C/D/H/S], -1 = no card):");
		for (std::uint32_t seat = 0; seat < kSeatCount; seat++)
		{
			const ScriptLocal seatLocal = SeatLocal(tableA, seat);
			std::int32_t card0Rank = HoleCardLocal(seatLocal, 0).At(kCardRankField).AsInt32();
			std::int32_t card0Suit = HoleCardLocal(seatLocal, 0).At(kCardSuitField).AsInt32();
			std::int32_t card1Rank = HoleCardLocal(seatLocal, 1).At(kCardRankField).AsInt32();
			std::int32_t card1Suit = HoleCardLocal(seatLocal, 1).At(kCardSuitField).AsInt32();

			Log::Write("  seat {} (base slot {}): card0={{rank={},suit={}}} card1={{rank={},suit={}}}{}",
				seat, seatLocal.Index(), card0Rank, card0Suit, card1Rank, card1Suit,
				(static_cast<std::int32_t>(seat) == seatIndex) ? "  <-- YOUR SEAT" : "");
		}

		// DECK OFFSET RE-VERIFICATION: PredictionCheck logged a MISMATCH
		// with garbage-looking predicted cards, which points to a real
		// offset bug in the deck's card-element indexing (see
		// docs/JOURNAL.md) rather than a content problem -- func_589
		// (deck init) writes valid {rank 2-14, suit 0-3} pairs into every
		// one of the 52 slots before func_1195 (shuffle) ever runs, so a
		// shuffled slot showing an invalid rank/suit means we're reading
		// the wrong words, not that the deck itself is wrong. Dump a wide
		// raw window around B's deck (f_606) as plain integers so the ALREADY
		// dealt hole cards above (independently confirmed correct many
		// times against the real screen) can be located by eye in this
		// dump -- since dealt hole cards are literally deck[0..cursor-1],
		// finding exactly where each known {rank,suit} pair starts here
		// pins down the deck's true per-card offset with zero guessing.
		Log::Write("ProbeTableStruct: raw deck window around Candidate B's f_606, offsets -4..+114 (compare against the hole cards logged above):");
		const std::int32_t deckIndex = static_cast<std::int32_t>(DeckLocal(tableB).Index());
		for (std::int32_t off = -4; off <= 114; off++)
		{
			std::int32_t slot = deckIndex + off;
			std::int32_t value = ScriptLocal(thread, static_cast<std::uint32_t>(slot)).AsInt32();
			Log::Write("  deckraw[{:+}] (slot {}) = {}", off, slot, value);
		}
	}

	// Logs poker_sp's script-local stack's absolute address range so it
	// can be pasted into Cheat Engine directly, for live/visual memory
	// analysis (watching values change in real time, "find what writes
	// to this address", etc.) instead of only ever probing one guessed
	// offset at a time through this mod's own F10 tools. Same addressing
	// GamePointers::ReadScriptLocal already uses --
	// thread->m_Stack is the base, m_Context.m_StackSize is the slot
	// count, 8 bytes/slot -- so end = base + m_StackSize*8. Also logs
	// uLocal_14's own absolute address within that range (slot
	// kRootLocalIndex), as a landmark for locating it by eye once
	// browsing the dumped range live.
	void DumpLocalStackRange()
	{
		auto thread = GamePointers::FindScriptThread(rage::Joaat("poker_sp"));
		if (!thread)
		{
			Log::Write("DumpLocalStackRange: poker_sp is not currently running");
			return;
		}

		auto base = reinterpret_cast<std::uintptr_t>(thread->m_Stack);
		std::uint32_t stackSizeSlots = thread->m_Context.m_StackSize;
		std::uintptr_t end = base + static_cast<std::uintptr_t>(stackSizeSlots) * 8u;
		std::uintptr_t localBase = base + static_cast<std::uintptr_t>(kRootLocalIndex) * 8u;

		Log::Write("DumpLocalStackRange: start={:#x} end={:#x} (size={} slots, {} bytes)",
			base, end, stackSizeSlots, end - base);
		Log::Write("DumpLocalStackRange: uLocal_14 (slot {}) starts at {:#x}",
			kRootLocalIndex, localBase);
	}

	// Built to debug a live report of the seat card icons drawing extra/
	// duplicate sets when only a handful of seats are actually occupied.
	// Dumps everything DrawOverlay()'s per-seat loop and
	// ComputeDenseRowForSeat() (the exact function the real drawing uses,
	// not a re-implementation of it -- see that function's header
	// comment) actually see for each of the 6 seats, so a wrong dense-row
	// assignment or an unexpectedly-non-(-1) occupancy marker shows up
	// directly instead of being inferred from the on-screen symptom.
	// Wired to the F10 menu's "Probe Seat Occupancy" item.
	void ProbeSeatOccupancy()
	{
		auto thread = GamePointers::FindScriptThread(rage::Joaat("poker_sp"));
		if (!thread)
		{
			Log::Write("ProbeSeatOccupancy: poker_sp is not currently running");
			return;
		}

		std::int32_t mySeat = MySeatLocal(thread).AsInt32();
		std::int32_t handState = HandStateLocal(thread).AsInt32();
		std::int32_t f1State = F1StateLocal(thread).AsInt32();
		std::int32_t subStep = SubStepLocal(thread).AsInt32();
		std::int32_t localRaw = RootLocal(thread).AsInt32();
		Log::Write("ProbeSeatOccupancy: mySeat={}, f_114.f_2010={}, f_1.f_42={}, f_114.f_2011={}, uLocal_14 (raw)={}", mySeat, handState, f1State, subStep, localRaw);

		int denseRow[kSeatCount];
		ComputeDenseRowForSeat(thread, mySeat, denseRow);

		for (std::uint32_t seat = 0; seat < kSeatCount; seat++)
		{
			const ScriptLocal seatLocal = SeatLocal(TableALocal(thread), seat);
			std::int32_t occupiedMarker = seatLocal.At(kSeatOccupiedField).AsInt32();
			std::int32_t state = seatLocal.At(kSeatStateField).AsInt32();
			std::int32_t stack = seatLocal.At(kSeatStackField).AsInt32();
			std::int32_t bet = seatLocal.At(kSeatBetField).AsInt32();

			std::int32_t card0Rank = HoleCardLocal(seatLocal, 0).At(kCardRankField).AsInt32();
			std::int32_t card0Suit = HoleCardLocal(seatLocal, 0).At(kCardSuitField).AsInt32();
			std::int32_t card1Rank = HoleCardLocal(seatLocal, 1).At(kCardRankField).AsInt32();
			std::int32_t card1Suit = HoleCardLocal(seatLocal, 1).At(kCardSuitField).AsInt32();
			bool cardsValid = (card0Rank >= 2 && card1Rank >= 2);

			Log::Write("  seat {}: occupiedMarker={} state={} stack={} bet={} cards={{{},{}}}/{{{},{}}} (valid={}) denseRow={}{}",
				seat, occupiedMarker, state, stack, bet,
				card0Rank, card0Suit, card1Rank, card1Suit, cardsValid ? "yes" : "no",
				denseRow[seat],
				(static_cast<std::int32_t>(seat) == mySeat) ? "  <-- YOUR SEAT" : "");
		}
	}

	// Tests the traced hypothesis that poker_sp's own community-card
	// reveal (func_471) creates a REAL 3D object per board slot and
	// stores the handle at scene.f_671.f_11[slot] (see kSceneField's
	// header comment) -- if this offset is right, reading that handle
	// and calling ENTITY::GET_ENTITY_COORDS/GRAPHICS::GET_SCREEN_COORD_
	// FROM_WORLD_COORD on it ourselves gives the exact real screen
	// position of that card, no calibration needed. Run this with at
	// least the flop revealed (3+ board cards showing) so slots 0-2 have
	// a real object to test against.
	void ProbeCommunityCardObjects()
	{
		auto thread = GamePointers::FindScriptThread(rage::Joaat("poker_sp"));
		if (!thread)
		{
			Log::Write("ProbeCommunityCardObjects: poker_sp is not currently running");
			return;
		}

		std::int32_t revealCount = BoardLocal(TableALocal(thread)).At(kBoardRevealCountField).AsInt32();
		const std::uint32_t objectsIndex = CommunityCardObjectLocal(thread, 0).Index();
		Log::Write("ProbeCommunityCardObjects: reveal count={}, testing scene.f_671.f_11[0..4] (candidate absolute slot {}):",
			revealCount, objectsIndex);

		for (std::uint32_t j = 0; j < kCommunityCardObjectCount; j++)
		{
			std::int32_t handle = CommunityCardObjectLocal(thread, j).AsInt32();
			BOOL exists = ENTITY::DOES_ENTITY_EXIST(handle);
			if (exists)
			{
				Vector3 pos = ENTITY::GET_ENTITY_COORDS(handle, true, false);
				float screenX = 0.0f, screenY = 0.0f;
				BOOL onScreen = GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(pos.x, pos.y, pos.z, &screenX, &screenY);
				Log::Write("  slot[{}]: handle={} EXISTS, world=({:.3f}, {:.3f}, {:.3f}), screen=({:.4f}, {:.4f}), onScreen={}",
					j, handle, pos.x, pos.y, pos.z, screenX, screenY, onScreen);
			}
			else
			{
				Log::Write("  slot[{}]: handle={} does NOT exist (raw window -4..+9 around it, for re-deriving the offset if this is wrong):",
					j, handle);
				if (j == 0)
				{
					for (std::int32_t off = -4; off <= 9; off++)
					{
						std::int32_t slot = static_cast<std::int32_t>(objectsIndex) + off;
						std::int32_t value = ScriptLocal(thread, static_cast<std::uint32_t>(slot)).AsInt32();
						Log::Write("    scenecardraw[{:+}] (slot {}) = {}", off, slot, value);
					}
				}
			}
		}
	}

	void DumpFullStackJsonl()
	{
		auto thread = GamePointers::FindScriptThread(rage::Joaat("poker_sp"));
		if (!thread)
		{
			Log::Write("DumpFullStackJsonl: poker_sp is not currently running");
			return;
		}

		// Timestamped so consecutive dumps (e.g. "before the flop" / "after
		// the flop") each land in their own file instead of the later one
		// clobbering the one a diff needs to compare against.
		SYSTEMTIME t;
		GetLocalTime(&t);
		std::ostringstream pathStream;
		pathStream << "PokerCheat_stackdump_"
			<< std::setfill('0')
			<< std::setw(4) << t.wYear << std::setw(2) << t.wMonth << std::setw(2) << t.wDay
			<< '_'
			<< std::setw(2) << t.wHour << std::setw(2) << t.wMinute << std::setw(2) << t.wSecond
			<< ".jsonl";
		std::string outPath = pathStream.str();

		if (GamePointers::DumpLocalStackJsonl(thread, outPath))
			Log::Write("DumpFullStackJsonl: wrote {} -- grep/jq it for a known real value (e.g. a visible card's rank/suit, a bet amount) to find where it actually lives, then diff against a prior dump's file to see what actually changed", outPath);
		else
			Log::Write("DumpFullStackJsonl: failed, see prior log line for why");
	}
#endif // _DEBUG
}
