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
	  - Table.f_15 (board): header at Table+15 (card-slot array size, 11),
	    cards at Table+16..+37 (2 words each, {rank,suit}, -1=empty),
	    reveal count at Table+38 (0/3/4/5).
	  - Table.f_39 (seats): header at Table+39 (seat count, 6), seat data
	    at Table+40 + i*56 (56 words/seat). Per seat: hole cards header at
	    seat_base+7, real card data at seat_base+8 (2 words each).
	  - Card = {rank: 2-14 (11=J,12=Q,13=K,14=A), suit: 0-3}. Suit mapping
	    confirmed against two real dealt cards (a 7 of Diamonds and a King
	    of Spades): 0=Clubs, 1=Diamonds, 2=Hearts, 3=Spades (standard
	    bridge/alphabetical order -- 0 and 2 inferred from the ordering,
	    not yet independently confirmed by a real card).

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
#include "Config.h"
#include "Localization.h"
#include "script.h"

#include <sstream>
#include <iomanip>
#include <string>

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
	// Confirmed struct layout (see header comment above and docs/JOURNAL.md
	// for the derivation/confirmation trail).
	// ------------------------------------------------------------------
	constexpr std::uint32_t kLocalStructIndex = 14;    // uLocal_14
	constexpr std::uint32_t kFieldOffsetF114 = 114;    // uLocal_14.f_114
	constexpr std::uint32_t kFieldOffsetTableA = 287;  // .f_287 -- Table
	constexpr std::uint32_t kFieldOffsetTableB = 1276; // .f_1276 -- holds the REAL shuffled deck (see kDeckSlot below); hole cards/board/seats still come from Candidate A (kTableSlot)
	constexpr std::uint32_t kTableSlot = kLocalStructIndex + kFieldOffsetF114 + kFieldOffsetTableA;
	constexpr std::uint32_t kTableSlotB = kLocalStructIndex + kFieldOffsetF114 + kFieldOffsetTableB;

	constexpr std::uint32_t kF114SeatIndexSlot = kLocalStructIndex + kFieldOffsetF114 + 9; // uLocal_14.f_114.f_9 (local player's seat)

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
	constexpr std::uint32_t kF114HandStateSlot = kLocalStructIndex + kFieldOffsetF114 + 2010;

	// uLocal_14.f_1.f_42 -- user-suggested alternate candidate for the
	// round-phase state (f_1 is the same generic framework struct
	// kFrameworkHashSlot already reads f_1.f_39 from, base offset
	// kLocalStructIndex+1 -- see that constant). f_42 doesn't turn up
	// directly in poker_sp.ysc.c itself (searched; no `.f_42`/`->f_42`
	// hits), which fits it living in the shared act_gen_poker.ysc.c
	// framework layer instead, same as f_39 -- not yet traced there.
	// Logged alongside kF114HandStateSlot in both the on-screen debug
	// line and ProbeTableStruct so the two candidates can be directly
	// compared against a real between-hands window rather than swapped
	// in blind a third time.
	constexpr std::uint32_t kF1StateSlot = kLocalStructIndex + 1 + 42;

	// uLocal_14.f_114.f_2011 -- the SUB-STEP field func_213 always sets
	// alongside f_2010 (same call, same function -- see kF114HandStateSlot's
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
	constexpr std::uint32_t kF114SubStepSlot = kLocalStructIndex + kFieldOffsetF114 + 2011;

	// Raw read of uLocal_14 itself (slot kLocalStructIndex, no nested
	// field offset at all) -- whatever the very first word of the whole
	// local struct holds. Not tied to any specific traced meaning; added
	// purely as a cheap, broad diagnostic alongside the real candidates.
	constexpr std::uint32_t kLocalRawSlot = kLocalStructIndex;

	constexpr std::uint32_t kBoardHeaderOffset = 15;              // Table.f_15 header (board array size, confirmed = 11)
	constexpr std::uint32_t kBoardSlot = kTableSlot + kBoardHeaderOffset;

	constexpr std::uint32_t kSeatsHeaderOffset = 39;               // Table.f_39 header (seat count, confirmed = 6)
	constexpr std::uint32_t kSeatsDataBase = kTableSlot + kSeatsHeaderOffset + 1; // Table+40, past the header word
	constexpr std::uint32_t kSeatStride = 56;
	constexpr std::uint32_t kHoleCardsHeaderOffset = 7;             // seat.f_7 header
	constexpr std::uint32_t kHoleCardsDataOffset = kHoleCardsHeaderOffset + 1; // seat.f_7 real card data
	constexpr std::uint32_t kSeatCount = 6;

	// uLocal_14.f_114.f_2655.f_90[seat] -- each seat's AI personality
	// index (0-14, into a fixed 15-entry style table func_584 builds once
	// via 15 func_1191() calls). Confirmed via the ONE literal call site
	// that actually assigns it, func_185 at poker_sp.ysc.c line 5883:
	// `func_185(&(uLocal_14.f_114.f_2655), i)` -- no parameter-identity
	// tracing needed here, uLocal_14 appears in the call verbatim. f_90
	// is a plain 0-based array (no leading size/count header word, unlike
	// Table.f_15/f_39) -- confirmed by func_185's own body indexing it
	// directly with the raw seat number. See docs/JOURNAL.md Session 14
	// (func_1628, the real decision engine this index feeds) and the
	// kPersonalityLabels header comment in Localization.cpp for the
	// index->label mapping. Purely a fixed-per-seat read, no RNG/prediction involved
	// (unlike the abandoned fold-prediction feature, Session 17).
	constexpr std::uint32_t kPersonalityIndexBase = kLocalStructIndex + kFieldOffsetF114 + 2655 + 90;

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
	// (f_1276), not Candidate A (f_287) that everything else in this file
	// reads from. Traced both func_589 call sites to their enclosing
	// functions: the unshuffled init (no func_1195 paired with it) is
	// inside func_285, and the shuffled init (func_589 immediately
	// followed by func_1195) is inside func_590 -- both of those, via
	// their own confirmed call sites (func_285(&(uParam0->f_1276)) at
	// line 7065, func_590(uParam0) called right after within func_285
	// itself with the same uParam0), operate on Candidate B, not A. Hole
	// cards/board/seat data are read from Candidate A throughout this
	// file and confirmed correct multiple times against the real screen
	// -- almost certainly synced/copied from Candidate B at some point,
	// since B is where dealing actually happens (func_1093, the deal
	// function, also touches f_606 on whatever struct it's given). Deck
	// reads specifically use kTableSlotB here; everything else keeps
	// using kTableSlot (Candidate A) as before.
	constexpr std::uint32_t kDeckOffset = 606;                 // .f_606
	constexpr std::uint32_t kDeckSlot = kTableSlotB + kDeckOffset;
	constexpr std::uint32_t kDeckCursorOffset = 105;           // f_606.f_105 -- index of the next undrawn card
	constexpr std::uint32_t kDeckCountOffset = 106;            // f_606.f_106 -- total cards (52)
	// The 52 card elements do NOT start at kDeckSlot+0 -- there's a
	// 1-word field before them (kDeckSlot+0 itself reads 52, i.e. count,
	// mirrored/leftover from whatever field precedes the array in the
	// Table struct). Nailed down empirically: raw-dumped kDeckSlot+1..+16
	// against 4 already-dealt, screen-confirmed hole cards (2 consecutive
	// draws per seat) and got an exact, duplicate-free match starting at
	// +1 (card 0's rank), not +0 -- see docs/JOURNAL.md. Every prior read
	// of card element k (rank at kDeckSlot+k*2) was off by one word,
	// which is why predicted cards were reading as garbage/invalid ranks
	// and suits instead of just "wrong but valid-looking" ones.
	constexpr std::uint32_t kDeckCardsBaseOffset = 1;

	// LaunchArgs (uScriptParam_0) and the framework-hash landmark used to
	// independently validate the uLocal_14-relative addressing chain --
	// kept here for ProbeTableStruct(), not used by the overlay itself.
	constexpr std::uint32_t kLaunchArgsSlot = 4810;
	constexpr std::uint32_t kLaunchArgsStakesTierField = 12;
	constexpr std::uint32_t kFrameworkHashSlot = 14 + 1 + 39; // uLocal_14.f_1.f_39
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
	constexpr std::uint32_t kSceneSlot = kLocalStructIndex + 3310; // uLocal_14.f_3310
	// f_11 is itself an array with the usual leading size/count header
	// word (same convention as Table.f_15/f_39, confirmed empirically
	// this time via ProbeCommunityCardObjects(): offset+0 read exactly
	// 5 -- the board's own slot count, not a card -- and the real object
	// handles started one word later. Element 0's real handle is at
	// kCommunityCardObjectsHeader+1, not +0.
	constexpr std::uint32_t kCommunityCardObjectsHeader = kSceneSlot + 671 + 11; // scene.f_671.f_11 header (confirmed = 5)
	constexpr std::uint32_t kCommunityCardObjectsBase = kCommunityCardObjectsHeader + 1; // scene.f_671.f_11[0], real object handle
	constexpr std::uint32_t kCommunityCardObjectCount = 5;

	namespace
	{
		std::int32_t ReadInt(rage::scrThread* thread, std::uint32_t slot)
		{
			void* raw = GamePointers::ReadScriptLocal(thread, slot);
			// alignas(8) int -- the real value lives in the low 4 bytes of
			// the 8-byte slot, so truncating a pointer-sized read is correct.
			return static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(raw));
		}


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
		// std::string, not a fixed char[]/sprintf_s -- no manual buffer
		// size to get wrong. Reuses RankName() above instead of a second,
		// duplicate rank-name switch (the original version of this
		// function predated RankName() and never got consolidated with
		// it); this is the same simplification BlackjackCheat.cpp's own
		// ported copy of this function already made.
		std::string BuildCardTextureName(std::int32_t rank, std::int32_t suit)
		{
			const char* suitName;
			switch (suit)
			{
				case 0: suitName = "HEARTS_"; break;
				case 1: suitName = "DIAMONDS_"; break;
				case 2: suitName = "SPADES_"; break;
				case 3: suitName = "CLUBS_"; break;
				default: suitName = ""; break;
			}

			return std::string(suitName) + RankName(rank);
		}

		// The real card_set_N number depends on which table/location skin
		// is active (func_330/func_331's selection logic, not replicated
		// here) -- instead of reimplementing that, this just probes which
		// card_set_N dictionary is ALREADY streamed in, since the game
		// itself must have already loaded the correct one to be showing
		// its own cards right now. Falls back to requesting card_set_1
		// if none are found loaded yet (e.g. called before the table has
		// finished setting up).
		constexpr int kCardSetProbeLo = 1;
		constexpr int kCardSetProbeHi = 8;

		bool FindLoadedCardSetDict(std::string& outDict)
		{
			for (int n = kCardSetProbeLo; n <= kCardSetProbeHi; n++)
			{
				std::string candidate = "card_set_" + std::to_string(n);
				if (TEXTURE::HAS_STREAMED_TEXTURE_DICT_LOADED(const_cast<char*>(candidate.c_str())))
				{
					outDict = candidate;
					return true;
				}
			}

			return false;
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
		// not-yet-revealed predicted cards (deterministic, see kDeckSlot's
		// header comment) ghosted at reduced alpha, same real/predicted
		// split as the text "Board:" line. Takes the already-unpacked
		// predicted-final-board ranks/suits DrawOverlay() computes once up
		// front (see predictedBoardBuf/boardRanks/boardSuits below) rather
		// than re-reading memory itself.
		void DrawCommunityCardIcons(const std::int32_t* ranks, const std::int32_t* suits, std::int32_t revealCount)
		{
			std::string cardSetDict;
			if (!FindLoadedCardSetDict(cardSetDict))
			{
				TEXTURE::REQUEST_STREAMED_TEXTURE_DICT(const_cast<char*>("card_set_1"), false);
				return;
			}

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

				std::string textureName = BuildCardTextureName(ranks[i], suits[i]);

				bool predicted = i >= revealCount;
				int alpha = predicted ? kCard2DPredictedAlpha : 255;
				float x = baseX + static_cast<float>(i) * spacingX;

				GRAPHICS::DRAW_SPRITE(const_cast<char*>(cardSetDict.c_str()), const_cast<char*>(textureName.c_str()), x, iconY, width, height, 0.0f, 255, 255, 255, alpha, 0);
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
		void DrawSeatCardIcons(int relOffset, std::int32_t rank0, std::int32_t suit0, std::int32_t rank1, std::int32_t suit1, int vsMeResult, const char* personalityLabel)
		{
			std::string cardSetDict;
			if (!FindLoadedCardSetDict(cardSetDict))
			{
				TEXTURE::REQUEST_STREAMED_TEXTURE_DICT(const_cast<char*>("card_set_1"), false);
				return;
			}

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

				std::string textureName = BuildCardTextureName(ranks[i], suits[i]);

				GRAPHICS::DRAW_SPRITE(const_cast<char*>(cardSetDict.c_str()), const_cast<char*>(textureName.c_str()), x + static_cast<float>(i) * spacingX, y, width, height, 0.0f, 255, 255, 255, 255, 0);
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
			bool showPersonality = Config::Get().ShowOpponentPersonality && personalityLabel && personalityLabel[0] != '\0';
			bool showVsMe = Config::Get().ShowWouldWinHandAgainst && vsMeResult != 2;
			if (showPersonality || showVsMe)
			{
				const char* vsLabel = Localization::VerdictLabel(vsMeResult);
				int labelR = showVsMe ? ((vsMeResult > 0) ? 140 : (vsMeResult < 0) ? 255 : 255) : 235;
				int labelG = showVsMe ? ((vsMeResult > 0) ? 255 : (vsMeResult < 0) ? 110 : 230) : 222;
				int labelB = showVsMe ? ((vsMeResult > 0) ? 140 : (vsMeResult < 0) ? 110 : 140) : 194;

				std::string label = showPersonality ? personalityLabel : "";
				if (showPersonality && showVsMe)
					label += " - ";
				if (showVsMe)
					label += vsLabel;

				float labelX = x + labelOffsetX;
				float labelY = y + height + labelOffsetY;

				std::string formatText = "<TEXTFORMAT RIGHTMARGIN='0'><P ALIGN='Left'><FONT FACE='$Font5' LETTERSPACING='0' SIZE='30'>~s~"
					+ label + "</FONT></P><TEXTFORMAT>";

				UIDEBUG::_BG_SET_TEXT_COLOR(labelR, labelG, labelB, 255);
				UIDEBUG::_BG_DISPLAY_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(formatText.c_str())), labelX, labelY);
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
			const char* label = Localization::VerdictLabel(result);
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
			std::string formatText = "<TEXTFORMAT RIGHTMARGIN='0'><P ALIGN='Left'><FONT FACE='$Font5' LETTERSPACING='0' SIZE='40'>~s~"
				+ std::string(label) + "</FONT></P><TEXTFORMAT>";

			UIDEBUG::_BG_SET_TEXT_COLOR(r, g, b, 255);
			UIDEBUG::_BG_DISPLAY_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(formatText.c_str())), winPredictionX, winPredictionY);
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
		// kCommunityCardObjectsBase -- confirmed working, just not what's
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
		// $Font5 was only ever confirmed against plain ASCII; nothing
		// established it (or any other token) actually has glyphs for
		// accented Latin, Cyrillic, Korean, or CJK.
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
			std::string sampleText = std::string(Localization::PersonalityLabel(lang, 8)) + " - " + Localization::VerdictLabel(lang, 1);

			// Same token list Session 11 tried (see the header comment
			// above) -- $Font5 is the one already confirmed to render a
			// real RDR2 font for ASCII text; the rest are re-tested here
			// on the theory that a different token could have broader
			// Unicode glyph coverage even if $Font5 turns out not to.
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
				+ " (" + Localization::LanguageCode(lang) + ") -- which rows below show real characters, not boxes?";
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

		constexpr std::size_t kHandEvalBufWords = 64;

		// Hand-scoring/comparison logic lives in its own header
		// (PokerHandEval.h) with zero game dependencies, specifically so
		// tests/PokerHandEvalTests.cpp can link against the exact same
		// code this file uses instead of a hand-copied duplicate that can
		// silently drift out of sync.
		using PokerHandEval::HandScore;
		using PokerHandEval::EvaluateHand;
		using PokerHandEval::CompareHands;

		// Builds a synthetic board buffer combining the REAL revealed
		// cards with PREDICTED future cards read straight from the deck
		// (see kDeckOffset's header comment -- the deck is fully shuffled
		// and fixed from hand start, so this isn't a guess), formatted to
		// match Table.f_15's real layout exactly (header word, up to 11
		// {rank,suit} card slots, reveal-count field at offset 23) so the
		// hand-eval native accepts it exactly like the real board. Always
		// sets the reveal-count to 5, so every hand gets evaluated
		// against the predicted FINAL board, not just what's currently
		// shown on screen -- this is what lets the overlay show the
		// predicted outcome from the very first frame of preflop.
		void BuildPredictedBoard(rage::scrThread* thread, std::int32_t revealCount, std::int32_t deckCursor, std::int32_t deckCount, std::uint64_t* outBuf)
		{
			for (std::size_t i = 0; i < kHandEvalBufWords; i++)
				outBuf[i] = 0;

			outBuf[0] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(ReadInt(thread, kBoardSlot))); // real header (=11)

			std::int32_t deckIndex = deckCursor;
			for (std::int32_t i = 0; i < 5; i++)
			{
				std::int32_t rank;
				std::int32_t suit;

				if (i < revealCount)
				{
					rank = ReadInt(thread, kBoardSlot + 1 + i * 2);
					suit = ReadInt(thread, kBoardSlot + 1 + i * 2 + 1);
				}
				else if (deckIndex >= 0 && deckIndex < deckCount)
				{
					rank = ReadInt(thread, kDeckSlot + kDeckCardsBaseOffset + static_cast<std::uint32_t>(deckIndex) * 2);
					suit = ReadInt(thread, kDeckSlot + kDeckCardsBaseOffset + static_cast<std::uint32_t>(deckIndex) * 2 + 1);
					deckIndex++;
				}
				else
				{
					rank = -1;
					suit = -1;
				}

				outBuf[1 + static_cast<std::size_t>(i) * 2] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(rank));
				outBuf[1 + static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(suit));
			}

			for (std::int32_t i = 5; i < 11; i++)
			{
				outBuf[1 + static_cast<std::size_t>(i) * 2] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(-1));
				outBuf[1 + static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(-1));
			}

			outBuf[23] = 5; // force "5 valid board cards" -- predicted-final board
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
				std::uint32_t otherSeatBase = kSeatsDataBase + static_cast<std::uint32_t>(otherSeat) * kSeatStride;
				std::int32_t otherOccupiedMarker = ReadInt(thread, otherSeatBase + 0);
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

			void* boardPtr = GamePointers::GetScriptLocalAddress(thread, kBoardSlot);
			if (!boardPtr)
				return;

			std::int32_t mySeat = ReadInt(thread, kF114SeatIndexSlot);

			// Between-hands suppression via poker_sp's internal state
			// fields was abandoned in Session 11 -- four candidates tried,
			// none held up live -- and reinstated in Session 13 via a
			// hole-card-validity heuristic instead (see git history for
			// that version). That heuristic is retired now that
			// kF114HandStateSlot's own value range is confirmed directly
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
			std::int32_t handState = ReadInt(thread, kF114HandStateSlot);
			bool handInProgress = (handState >= 4 && handState <= 7);

			// Read once up front, reused for the predicted board, the
			// "Upcoming" line, and the real "Board" line below.
			std::int32_t revealCount = ReadInt(thread, kBoardSlot + 23); // f_15.f_23
			std::int32_t deckCursor = ReadInt(thread, kDeckSlot + kDeckCursorOffset);
			std::int32_t deckCount = ReadInt(thread, kDeckSlot + kDeckCountOffset);

			std::uint64_t predictedBoardBuf[kHandEvalBufWords] = {};
			BuildPredictedBoard(thread, revealCount, deckCursor, deckCount, predictedBoardBuf);

			// Plain rank/suit ints for the predicted final board's 5 cards,
			// pulled straight out of predictedBoardBuf -- this is the same
			// data BuildPredictedBoard already computed (real revealed
			// cards + deterministic future cards off the deck), just
			// unpacked once here so EvaluateHand() (below) can score every
			// seat's best 7-card hand directly, with no native call
			// involved.
			std::int32_t boardRanks[5];
			std::int32_t boardSuits[5];
			for (int i = 0; i < 5; i++)
			{
				boardRanks[i] = static_cast<std::int32_t>(predictedBoardBuf[1 + static_cast<std::size_t>(i) * 2]);
				boardSuits[i] = static_cast<std::int32_t>(predictedBoardBuf[1 + static_cast<std::size_t>(i) * 2 + 1]);
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
			// cards actually match what gets dealt.
			static std::int32_t s_preflopCursorSnapshot = -999;
			static std::int32_t s_preflopPredRank[5] = {};
			static std::int32_t s_preflopPredSuit[5] = {};
			static bool s_showdownLogged = false;
			if (revealCount == 0 && deckCursor != s_preflopCursorSnapshot && deckCursor >= 0)
			{
				for (int i = 0; i < 5; i++)
				{
					s_preflopPredRank[i] = static_cast<std::int32_t>(predictedBoardBuf[1 + static_cast<std::size_t>(i) * 2]);
					s_preflopPredSuit[i] = static_cast<std::int32_t>(predictedBoardBuf[1 + static_cast<std::size_t>(i) * 2 + 1]);
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
			if (revealCount >= 5 && !s_showdownLogged && s_preflopCursorSnapshot != -999)
			{
				bool allMatch = true;
				std::string realStr;
				std::string predStr;
				for (int i = 0; i < 5; i++)
				{
					std::int32_t realRank = ReadInt(thread, kBoardSlot + 1 + i * 2);
					std::int32_t realSuit = ReadInt(thread, kBoardSlot + 1 + i * 2 + 1);
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

			DrawLine(x, y, revealCount >= 5 ? "PokerCheat" : "PokerCheat (predicted final hands)", true);
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
				std::uint32_t myBase = kSeatsDataBase + static_cast<std::uint32_t>(mySeat) * kSeatStride;
				std::uint32_t myCardsBase = myBase + kHoleCardsDataOffset;
				std::int32_t myCard0Rank = ReadInt(thread, myCardsBase + 0);
				std::int32_t myCard0Suit = ReadInt(thread, myCardsBase + 1);
				std::int32_t myCard1Rank = ReadInt(thread, myCardsBase + 2);
				std::int32_t myCard1Suit = ReadInt(thread, myCardsBase + 3);
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

			// 0 means "you, or no seat maps here" -- see
			// ComputeDenseRowForSeat()'s header comment.
			int denseRowForSeat[kSeatCount];
			ComputeDenseRowForSeat(thread, mySeat, denseRowForSeat);

			for (std::uint32_t seat = 0; seat < kSeatCount; seat++)
			{
				std::uint32_t seatBase = kSeatsDataBase + seat * kSeatStride;

				// func_143's own check (Table.f_39[seat] != -1, word 0 of
				// the seat struct) -- is anyone seated here at all.
				std::int32_t occupiedMarker = ReadInt(thread, seatBase + 0);
				if (occupiedMarker == -1)
					continue; // empty seat, don't draw a line for it

				// seat.f_6: confirmed via poker_sp.ysc.c func_912/func_475/
				// func_476 (-1=empty [already excluded above], 0=active,
				// 1=folded, 2=all-in) -- func_475's call site (line ~9430)
				// draws a "folded" status icon exactly when f_6==1,
				// confirming the mapping.
				std::int32_t state = ReadInt(thread, seatBase + 6);
				bool isActive = (state == 0 || state == 2); // still eligible to win the pot

#ifdef _DEBUG
				// seat.f_2 (stack) / seat.f_3 (current bet) / the fold/
				// all-in tag -- debug text panel only, not used by any of
				// the four Release overlay elements.
				const char* stateLabel = (state == 1) ? " [FOLDED]" : (state == 2) ? " [ALL-IN]" : "";
				std::int32_t stack = ReadInt(thread, seatBase + 2);
				std::int32_t bet = ReadInt(thread, seatBase + 3);
#endif

				std::uint32_t cardsBase = seatBase + kHoleCardsDataOffset;
				std::int32_t card0Rank = ReadInt(thread, cardsBase + 0);
				std::int32_t card0Suit = ReadInt(thread, cardsBase + 1);
				std::int32_t card1Rank = ReadInt(thread, cardsBase + 2);
				std::int32_t card1Suit = ReadInt(thread, cardsBase + 3);

				std::int32_t personalityIndex = ReadInt(thread, kPersonalityIndexBase + seat);
				const char* personalityLabel = Localization::PersonalityLabel(personalityIndex);

				bool isMe = (static_cast<std::int32_t>(seat) == mySeat);

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
					if (!isMe && personalityLabel[0] != '\0')
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
					if (denseRow != 0)
						DrawSeatCardIcons(denseRow, card0Rank, card0Suit, card1Rank, card1Suit, vsMeResult, personalityLabel);
				}
			}

			// Board (community cards) -- ONE line, always showing all 5
			// eventual cards, not just the currently-revealed ones (this
			// used to be two separate lines -- "Board:" showing only the
			// real revealed cards, correctly reading "(preflop)" with
			// nothing revealed yet, and a separate "Upcoming:" line for
			// the prediction -- which read correctly but wasn't where
			// the prediction was expected to show up). Already-revealed
			// cards are read from the real board (kBoardSlot); anything
			// not revealed yet is the exact future card read straight off
			// the deck at the current cursor (see kDeckOffset's header
			// comment -- deterministic, not a guess) and marked with a
			// trailing "*" so it's clear which cards are real right now
			// vs. predicted. Gated by ShowCommunityCards -- both the text
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
				// icon strip below draws -- boardRanks/boardSuits (used by
				// DrawCommunityCardIcons()) are computed unconditionally
				// above, independent of this text line entirely.
				std::ostringstream boardLine;
				boardLine << "Board: ";
				std::int32_t deckIdx = deckCursor;
				int cardsShown = 0;
				for (int i = 0; i < 5; i++)
				{
					std::int32_t rank;
					std::int32_t suit;
					bool predicted;

					if (i < revealCount)
					{
						rank = ReadInt(thread, kBoardSlot + 1 + i * 2);
						suit = ReadInt(thread, kBoardSlot + 1 + i * 2 + 1);
						predicted = false;
					}
					else if (deckIdx >= 0 && deckIdx < deckCount)
					{
						rank = ReadInt(thread, kDeckSlot + kDeckCardsBaseOffset + static_cast<std::uint32_t>(deckIdx) * 2);
						suit = ReadInt(thread, kDeckSlot + kDeckCardsBaseOffset + static_cast<std::uint32_t>(deckIdx) * 2 + 1);
						deckIdx++;
						predicted = true;
					}
					else
					{
						break; // deck cursor/count read as invalid -- see below
					}

					boardLine << RankName(rank) << SuitLetter(suit) << (predicted ? "*" : "") << ' ';
					cardsShown++;
				}

				// If the loop above stopped early for a reason OTHER than "the
				// board is genuinely fully revealed already" (revealCount>=5
				// needs no deck reads at all, so cardsShown==5 there with no
				// deck involvement), that means deckCursor/deckCount read as
				// out of range -- e.g. the deck hasn't been shuffled/dealt
				// yet at the moment this ran. That's a real, different
				// situation from "nothing left to predict" and shouldn't look
				// the same on screen.
				if (cardsShown < 5 && revealCount < 5)
					boardLine << "(deck not ready -- cursor/count out of range)";

				DrawLine(x, y, boardLine.str().c_str());
				y += kLineHeight;
#endif

				// Real card-face icons at the calibrated top-right strip
				// position, using the same real/predicted split as the text
				// "Board:" line above (boardRanks/boardSuits already hold the
				// predicted-final-board's 5 cards, computed once up front).
				DrawCommunityCardIcons(boardRanks, boardSuits, revealCount);
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
			// live shuffled deck. Fixed (kDeckSlot now built from
			// kTableSlotB) and confirmed via ProbeTableStruct: Candidate B
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
				DrawWinPredictionStatus(haveMyHand, anyOpponent, worstResult);
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
			return;

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

		std::int32_t stakesTierArg = ReadInt(thread, kLaunchArgsSlot + kLaunchArgsStakesTierField);
		Log::Write("ProbeTableStruct: LaunchArgs.f_12 (slot {}) = {}",
			kLaunchArgsSlot + kLaunchArgsStakesTierField, stakesTierArg);

		std::int32_t frameworkHash = ReadInt(thread, kFrameworkHashSlot);
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
			kFrameworkHashSlot, frameworkHash,
			hashMatch ? "EXACT MATCH to a known stakes hash" : "no match");

		std::int32_t seatIndex = ReadInt(thread, kF114SeatIndexSlot);
		Log::Write("ProbeTableStruct: uLocal_14.f_114.f_9 (your seat) = {}", seatIndex);

		std::int32_t handState = ReadInt(thread, kF114HandStateSlot);
		std::int32_t f1State = ReadInt(thread, kF1StateSlot);
		std::int32_t subStep = ReadInt(thread, kF114SubStepSlot);
		std::int32_t localRaw = ReadInt(thread, kLocalRawSlot);
		Log::Write("ProbeTableStruct: round-phase candidates -- uLocal_14.f_114.f_2010 = {}, uLocal_14.f_1.f_42 = {}, uLocal_14.f_114.f_2011 = {}, uLocal_14 (raw) = {}",
			handState, f1State, subStep, localRaw);

		std::int32_t boardHeader = ReadInt(thread, kBoardSlot);
		std::int32_t revealCount = ReadInt(thread, kBoardSlot + 23);
		Log::Write("ProbeTableStruct: Table.f_15 (board) header={} (expect 11), reveal count={} (expect 0/3/4/5)",
			boardHeader, revealCount);

		// Traced func_589's (deck init) two call sites to their enclosing
		// functions (func_285/func_590) and confirmed, via their own
		// call sites, that BOTH operate on Candidate B (f_1276) -- the
		// real shuffled gameplay deck lives there, not Candidate A
		// (f_287) that hole cards/board/seats are read from. kDeckSlot
		// now points at Candidate B accordingly. Still logging Candidate
		// A's deck area too, purely to confirm empirically that it does
		// NOT look like a valid live deck (expected: stale/unshuffled or
		// simply not count=52) now that we're not relying on it.
		std::uint32_t deckSlotA = kTableSlot + kDeckOffset;
		std::int32_t deckCursorA = ReadInt(thread, deckSlotA + kDeckCursorOffset);
		std::int32_t deckCountA = ReadInt(thread, deckSlotA + kDeckCountOffset);
		Log::Write("ProbeTableStruct: Candidate A (f_287) deck cursor={}, count={} -- expected NOT to look like a valid live deck now",
			deckCursorA, deckCountA);

		std::int32_t deckCursor = ReadInt(thread, kDeckSlot + kDeckCursorOffset);
		std::int32_t deckCount = ReadInt(thread, kDeckSlot + kDeckCountOffset);
		Log::Write("ProbeTableStruct: Candidate B (f_1276, now used for all deck reads) cursor={}, count={} (expect count=52) -- next 8 undrawn cards:",
			deckCursor, deckCount);
		for (std::int32_t i = 0; i < 8; i++)
		{
			std::int32_t idx = deckCursor + i;
			if (idx < 0 || idx >= deckCount)
				break;
			std::int32_t rank = ReadInt(thread, kDeckSlot + kDeckCardsBaseOffset + static_cast<std::uint32_t>(idx) * 2);
			std::int32_t suit = ReadInt(thread, kDeckSlot + kDeckCardsBaseOffset + static_cast<std::uint32_t>(idx) * 2 + 1);
			Log::Write("  deck[{}]: {}{}", idx, RankName(rank), SuitLetter(suit));
		}

		std::int32_t seatsHeader = ReadInt(thread, kTableSlot + kSeatsHeaderOffset);
		Log::Write("ProbeTableStruct: Table.f_39 (seats) header={} (expect 6)", seatsHeader);

		Log::Write("ProbeTableStruct: hole cards per seat (rank 2-14, suit 0-3 [C/D/H/S], -1 = no card):");
		for (std::uint32_t seat = 0; seat < kSeatCount; seat++)
		{
			std::uint32_t seatBase = kSeatsDataBase + seat * kSeatStride;
			std::uint32_t cardsBase = seatBase + kHoleCardsDataOffset;

			std::int32_t card0Rank = ReadInt(thread, cardsBase + 0);
			std::int32_t card0Suit = ReadInt(thread, cardsBase + 1);
			std::int32_t card1Rank = ReadInt(thread, cardsBase + 2);
			std::int32_t card1Suit = ReadInt(thread, cardsBase + 3);

			Log::Write("  seat {} (base slot {}): card0={{rank={},suit={}}} card1={{rank={},suit={}}}{}",
				seat, seatBase, card0Rank, card0Suit, card1Rank, card1Suit,
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
		// raw window around kDeckSlot as plain integers so the ALREADY
		// dealt hole cards above (independently confirmed correct many
		// times against the real screen) can be located by eye in this
		// dump -- since dealt hole cards are literally deck[0..cursor-1],
		// finding exactly where each known {rank,suit} pair starts here
		// pins down the deck's true per-card offset with zero guessing.
		Log::Write("ProbeTableStruct: raw deck window around kDeckSlot, offsets -4..+114 (compare against the hole cards logged above):");
		for (std::int32_t off = -4; off <= 114; off++)
		{
			std::int32_t slot = static_cast<std::int32_t>(kDeckSlot) + off;
			std::int32_t value = ReadInt(thread, static_cast<std::uint32_t>(slot));
			Log::Write("  deckraw[{:+}] (slot {}) = {}", off, slot, value);
		}
	}

	// Logs poker_sp's script-local stack's absolute address range so it
	// can be pasted into Cheat Engine directly, for live/visual memory
	// analysis (watching values change in real time, "find what writes
	// to this address", etc.) instead of only ever probing one guessed
	// offset at a time through this mod's own F10 tools. Same addressing
	// GamePointers::ReadScriptLocal/GetScriptLocalAddress already use --
	// thread->m_Stack is the base, m_Context.m_StackSize is the slot
	// count, 8 bytes/slot -- so end = base + m_StackSize*8. Also logs
	// uLocal_14's own absolute address within that range (slot
	// kLocalStructIndex), as a landmark for locating it by eye once
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
		std::uintptr_t localBase = base + static_cast<std::uintptr_t>(kLocalStructIndex) * 8u;

		Log::Write("DumpLocalStackRange: start={:#x} end={:#x} (size={} slots, {} bytes)",
			base, end, stackSizeSlots, end - base);
		Log::Write("DumpLocalStackRange: uLocal_14 (slot {}) starts at {:#x}",
			kLocalStructIndex, localBase);
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

		std::int32_t mySeat = ReadInt(thread, kF114SeatIndexSlot);
		std::int32_t handState = ReadInt(thread, kF114HandStateSlot);
		std::int32_t f1State = ReadInt(thread, kF1StateSlot);
		std::int32_t subStep = ReadInt(thread, kF114SubStepSlot);
		std::int32_t localRaw = ReadInt(thread, kLocalRawSlot);
		Log::Write("ProbeSeatOccupancy: mySeat={}, f_114.f_2010={}, f_1.f_42={}, f_114.f_2011={}, uLocal_14 (raw)={}", mySeat, handState, f1State, subStep, localRaw);

		int denseRow[kSeatCount];
		ComputeDenseRowForSeat(thread, mySeat, denseRow);

		for (std::uint32_t seat = 0; seat < kSeatCount; seat++)
		{
			std::uint32_t seatBase = kSeatsDataBase + seat * kSeatStride;
			std::int32_t occupiedMarker = ReadInt(thread, seatBase + 0);
			std::int32_t state = ReadInt(thread, seatBase + 6);
			std::int32_t stack = ReadInt(thread, seatBase + 2);
			std::int32_t bet = ReadInt(thread, seatBase + 3);

			std::uint32_t cardsBase = seatBase + kHoleCardsDataOffset;
			std::int32_t card0Rank = ReadInt(thread, cardsBase + 0);
			std::int32_t card0Suit = ReadInt(thread, cardsBase + 1);
			std::int32_t card1Rank = ReadInt(thread, cardsBase + 2);
			std::int32_t card1Suit = ReadInt(thread, cardsBase + 3);
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
	// stores the handle at scene.f_671.f_11[slot] (see kSceneSlot's
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

		std::int32_t revealCount = ReadInt(thread, kBoardSlot + 23);
		Log::Write("ProbeCommunityCardObjects: reveal count={}, testing scene.f_671.f_11[0..4] (candidate absolute slot {}):",
			revealCount, kCommunityCardObjectsBase);

		for (std::uint32_t j = 0; j < kCommunityCardObjectCount; j++)
		{
			std::int32_t handle = ReadInt(thread, kCommunityCardObjectsBase + j);
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
						std::int32_t slot = static_cast<std::int32_t>(kCommunityCardObjectsBase) + off;
						std::int32_t value = ReadInt(thread, static_cast<std::uint32_t>(slot));
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
