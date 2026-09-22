// Unit tests for src/PokerHandEval.h -- the self-contained poker hand
// evaluator that replaced a call into the game's own hand-rank native
// (see PokerCheat.cpp's file header comment and docs/JOURNAL.md, Session 9,
// for why: that native's output buffer proved impossible to use correctly
// from the outside after 9 sessions of guessing at its layout).
//
// A hand evaluator that's only ever eyeballed against real hands, with no
// automated check, is exactly how that original bug went undetected for so
// long. This links against the SAME header the mod itself uses (not a
// hand-copied duplicate), so a future change to PokerHandEval.h gets
// checked here before it ships.
//
// Build & run (from the PokerCheat directory, in a Developer Command
// Prompt / after vcvars64.bat, or via MSBuild -- see this folder's
// PokerHandEvalTests.vcxproj):
//   MSBuild.exe tests\PokerHandEvalTests.vcxproj /p:Configuration=Release /p:Platform=x64
//   bin\Release\PokerHandEvalTests.exe
// Exits 0 and prints "ALL PASS" if every case passes, exits 1 and lists
// failures otherwise.

#include "../src/PokerHandEval.h"

#include <cstdio>

namespace
{
	using PokerHandEval::HandScore;
	using PokerHandEval::EvaluateHand;
	using PokerHandEval::CompareHands;

	// Suit encoding is arbitrary here -- only equality matters to the
	// evaluator -- but named to make test cases readable.
	enum Suit { H = 0, D = 1, S = 2, C = 3 };

	int g_failures = 0;

	void Check(bool condition, const char* testName, const char* detail)
	{
		if (condition)
		{
			std::printf("  [PASS] %s\n", testName);
		}
		else
		{
			std::printf("  [FAIL] %s -- %s\n", testName, detail);
			g_failures++;
		}
	}

	HandScore Eval7(std::int32_t r0, std::int32_t s0, std::int32_t r1, std::int32_t s1,
		std::int32_t br[5], std::int32_t bs[5])
	{
		std::int32_t ranks[7] = { r0, r1, br[0], br[1], br[2], br[3], br[4] };
		std::int32_t suits[7] = { s0, s1, bs[0], bs[1], bs[2], bs[3], bs[4] };
		return EvaluateHand(ranks, suits);
	}

	// ------------------------------------------------------------------
	// Regression case: the exact hand from the "predicted CHOP, actually
	// lost" bug report. Board 4H 7H 7D KH 4D; seat 0 AS 8H; seat 2 8S AD;
	// seat 3 (the user) 9C JD. Real result: seat 0 and seat 2 tie (both
	// two pair 7s/4s, Ace kicker) and both beat seat 3. Seat 3's true
	// kicker turns out to be the board's own King (13), not the JD hole
	// card (11) -- an unpaired board card is fair game as anyone's
	// kicker if their hole cards don't beat it, which is exactly the
	// subtlety a "pair ranks + best hole kicker" shortcut would miss.
	// ------------------------------------------------------------------
	void TestReportedHand_BoardKicker()
	{
		std::printf("TestReportedHand_BoardKicker:\n");

		std::int32_t board[5] = { 4, 7, 7, 13, 4 };
		std::int32_t boardSuits[5] = { H, H, D, H, D };

		HandScore seat0 = Eval7(14, S, 8, H, board, boardSuits);  // AS 8H
		HandScore seat2 = Eval7(8, S, 14, D, board, boardSuits);  // 8S AD
		HandScore seat3 = Eval7(9, C, 11, D, board, boardSuits);  // 9C JD (user)

		Check(seat0.category == 2, "seat0 category is Two Pair", "expected category 2");
		Check(seat2.category == 2, "seat2 category is Two Pair", "expected category 2");
		Check(seat3.category == 2, "seat3 category is Two Pair", "expected category 2");

		Check(seat0.tiebreak[0] == 7 && seat0.tiebreak[1] == 4 && seat0.tiebreak[2] == 14,
			"seat0 tiebreak is [7,4,Ace]", "seat0 should be two pair 7s/4s with an Ace kicker");
		Check(seat3.tiebreak[0] == 7 && seat3.tiebreak[1] == 4 && seat3.tiebreak[2] == 13,
			"seat3 tiebreak is [7,4,King]", "seat3's real kicker is the board's King, not the JD hole card");

		Check(CompareHands(seat0, seat2) == 0, "seat0 ties seat2", "both have identical two pair + Ace kicker");
		Check(CompareHands(seat0, seat3) > 0, "seat0 beats seat3", "Ace kicker beats King kicker");
		Check(CompareHands(seat2, seat3) > 0, "seat2 beats seat3", "Ace kicker beats King kicker");
	}

	void TestHighCardKicker()
	{
		std::printf("TestHighCardKicker:\n");
		std::int32_t board[5] = { 2, 6, 9, 12, 3 }; // no pairs, no flush, no straight
		std::int32_t boardSuits[5] = { H, D, S, C, H };

		HandScore a = Eval7(14, D, 5, S, board, boardSuits); // Ace high
		HandScore b = Eval7(11, D, 5, S, board, boardSuits); // Jack high

		Check(a.category == 0 && b.category == 0, "both High Card", "no pair/straight/flush should be present");
		Check(CompareHands(a, b) > 0, "Ace-high beats Jack-high", "higher top card should win with no other hand present");
	}

	void TestTwoPairHighPairDecides()
	{
		std::printf("TestTwoPairHighPairDecides:\n");
		// a: pocket Aces + a 2 on the board pairs -> AA 22
		// b: pocket Kings + a 2 on the board pairs -> KK 22, plus a Queen kicker
		// Higher PAIR (Aces vs Kings) must decide it even though b's kicker (Q)
		// beats a's kicker (J) -- pair rank compares before kicker.
		std::int32_t board[5] = { 2, 2, 5, 8, 12 }; // pairs the 2s, adds a Queen
		std::int32_t boardSuits[5] = { H, D, S, C, H };

		HandScore a = Eval7(14, D, 14, S, board, boardSuits); // AA -> AA 22 Q kicker
		HandScore b = Eval7(13, D, 13, S, board, boardSuits); // KK -> KK 22 Q kicker

		Check(a.category == 2 && b.category == 2, "both Two Pair", "board pairs the 2s for everyone");
		Check(CompareHands(a, b) > 0, "AA22 beats KK22", "higher top pair decides before any kicker");
	}

	void TestTripsBeatsTwoPair()
	{
		std::printf("TestTripsBeatsTwoPair:\n");
		// Board has exactly one pair (5s) and three unrelated singles, so
		// neither hand can accidentally back into a full house.
		std::int32_t board[5] = { 5, 5, 9, 2, 7 };
		std::int32_t boardSuits[5] = { H, D, S, C, H };

		HandScore trips = Eval7(5, S, 3, H, board, boardSuits);   // third 5 -> trip 5s, kickers 9/7
		HandScore twoPair = Eval7(9, D, 2, S, board, boardSuits); // pairs the 9 and the 2 -> two pair 9s/5s, 7 kicker

		Check(trips.category == 3, "trip fives is Three of a Kind", "third 5 from hole card should make trips");
		Check(twoPair.category == 2, "other hand is Two Pair", "hole cards pair the board's 9 and 2");
		Check(CompareHands(trips, twoPair) > 0, "trips beats two pair", "category order: trips > two pair");
	}

	void TestFullHouseBeatsFlush()
	{
		std::printf("TestFullHouseBeatsFlush:\n");
		std::int32_t board[5] = { 4, 4, 9, 2, 7 };
		std::int32_t boardSuits[5] = { H, D, H, H, H };

		HandScore fullHouse = Eval7(4, S, 9, C, board, boardSuits); // 4H 4D 4S + 9C 9H -> full house
		HandScore flush = Eval7(11, H, 3, H, board, boardSuits);   // JH 3H + 9H 2H 7H -> hearts flush

		Check(fullHouse.category == 6, "full house category", "trip 4s + pair 9s");
		Check(flush.category == 5, "flush category", "five hearts");
		Check(CompareHands(fullHouse, flush) > 0, "full house beats flush", "category order: full house > flush");
	}

	void TestWheelStraightIsLowest()
	{
		std::printf("TestWheelStraightIsLowest:\n");
		std::int32_t board[5] = { 3, 4, 5, 9, 12 };
		std::int32_t boardSuits[5] = { H, D, S, C, H };

		HandScore wheel = Eval7(14, D, 2, S, board, boardSuits); // A-2-3-4-5
		HandScore sixHigh = Eval7(6, D, 2, S, board, boardSuits); // 2-3-4-5-6

		Check(wheel.category == 4 && sixHigh.category == 4, "both Straight", "A2345 and 23456 are both straights");
		Check(wheel.tiebreak[0] == 5, "wheel straight high card is 5", "A-2-3-4-5 plays as 5-high, not Ace-high");
		Check(CompareHands(sixHigh, wheel) > 0, "6-high straight beats the wheel", "5-high (wheel) is the lowest straight");
	}

	void TestStraightFlushBeatsQuads()
	{
		std::printf("TestStraightFlushBeatsQuads:\n");

		std::int32_t sfBoard[5] = { 5, 6, 7, 8, 9 };
		std::int32_t sfBoardSuits[5] = { H, H, H, H, D };
		HandScore straightFlush = Eval7(4, H, 2, S, sfBoard, sfBoardSuits); // 4H-5H-6H-7H-8H

		std::int32_t quadBoard[5] = { 9, 9, 2, 3, 4 };
		std::int32_t quadBoardSuits[5] = { H, D, S, C, H };
		HandScore quads = Eval7(9, S, 9, C, quadBoard, quadBoardSuits); // board's two 9s + hole's two 9s -> four 9s

		Check(straightFlush.category == 8, "straight flush category", "4H through 8H are all hearts and consecutive");
		Check(quads.category == 7, "four of a kind category", "board pair of 9s plus two more 9s in the hole");
		Check(CompareHands(straightFlush, quads) > 0, "straight flush beats quads", "category order: straight flush > four of a kind");
	}

	void TestRoyalFlushCategory()
	{
		std::printf("TestRoyalFlushCategory:\n");
		std::int32_t board[5] = { 10, 11, 12, 2, 3 };
		std::int32_t boardSuits[5] = { H, H, H, D, S };

		HandScore royal = Eval7(14, H, 13, H, board, boardSuits); // AH KH + 10H JH QH
		Check(royal.category == 9, "royal flush is category 9", "ace-high straight flush should be distinct from a plain straight flush");
	}

	void TestGenuineTieChopsThePot()
	{
		std::printf("TestGenuineTieChopsThePot:\n");
		// Board itself is a full straight -- both hole-card pairs play the
		// board exactly, no kicker distinguishes them (both hole cards are
		// lower than every board card, so neither plays as a kicker).
		std::int32_t board[5] = { 5, 6, 7, 8, 9 };
		std::int32_t boardSuits[5] = { H, D, S, C, D };

		HandScore a = Eval7(2, H, 3, D, board, boardSuits);
		HandScore b = Eval7(2, S, 3, C, board, boardSuits);

		Check(a.category == 4 && b.category == 4, "both play the board straight", "5-6-7-8-9 on board beats either hand's hole cards");
		Check(CompareHands(a, b) == 0, "identical board-straight hands chop", "neither hole card improves on the board's own straight");
	}

	// PokerCheat.cpp's ReadPredictedBoard() deliberately fills a board slot
	// with rank -1 when the deck cursor/count read out of range, and every
	// rank comes from raw game memory -- a wrong offset after a game update
	// reads arbitrary values. ScoreFiveCards() indexes a count[15] array by
	// rank, so any rank outside 2-14 must be rejected up front (category
	// -1, which every call site already treats as "no hand") instead of
	// writing out of bounds. Also must not silently score the remaining
	// valid cards as if the bad one weren't there.
	void TestInvalidRankRejected()
	{
		std::printf("TestInvalidRankRejected:\n");
		std::int32_t board[5] = { 14, 14, 14, 13, -1 };
		std::int32_t boardSuits[5] = { H, D, S, C, -1 };

		HandScore sentinel = Eval7(14, C, 13, H, board, boardSuits);
		Check(sentinel.category == -1, "-1 board rank (deck-not-ready sentinel) rejected", "must not score the other 6 cards as quad Aces");

		std::int32_t garbageBoard[5] = { 2, 5, 9, 11, 1000 };
		std::int32_t garbageSuits[5] = { H, D, S, C, H };
		HandScore garbage = Eval7(14, C, 13, H, garbageBoard, garbageSuits);
		Check(garbage.category == -1, "out-of-range high rank rejected", "rank 1000 would index far past count[15]");

		std::int32_t lowBoard[5] = { 2, 5, 9, 11, 12 };
		std::int32_t lowSuits[5] = { H, D, S, C, H };
		HandScore zeroHole = Eval7(0, C, 13, H, lowBoard, lowSuits);
		Check(zeroHole.category == -1, "rank 0 hole card rejected", "0 is what an out-of-stack-range ReadInt returns");

		std::int32_t r5[5] = { 14, 13, 12, 11, 15 };
		std::int32_t s5[5] = { H, H, H, H, H };
		Check(PokerHandEval::ScoreFiveCards(r5, s5).category == -1, "ScoreFiveCards rejects rank 15 directly", "count[15] has no index 15");
	}
}

int main()
{
	TestReportedHand_BoardKicker();
	TestHighCardKicker();
	TestTwoPairHighPairDecides();
	TestTripsBeatsTwoPair();
	TestFullHouseBeatsFlush();
	TestWheelStraightIsLowest();
	TestStraightFlushBeatsQuads();
	TestRoyalFlushCategory();
	TestGenuineTieChopsThePot();
	TestInvalidRankRejected();

	if (g_failures == 0)
	{
		std::printf("\nALL PASS\n");
		return 0;
	}

	std::printf("\n%d FAILURE(S)\n", g_failures);
	return 1;
}
