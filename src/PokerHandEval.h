#pragma once

// Self-contained 7-card poker hand evaluator -- category + full kicker
// tiebreak, no dependency on the game's own hand-rank native or any
// ScriptHookRDR2/game-memory code. See PokerCheat.cpp's file header comment
// for why this exists (the native's output buffer proved impossible to use
// correctly from the outside after 9 sessions of trying).
//
// Deliberately its own header (rather than living inline in PokerCheat.cpp)
// so it has exactly one copy used by both the mod itself and
// tests/PokerHandEvalTests.cpp -- a hand evaluator that's only ever
// eyeballed against real hands, with no automated check, is exactly how
// the original native-buffer bug went undetected for 9 sessions.
//
// All functions are `inline` so this header can be included from more than
// one translation unit (the mod's PokerCheat.cpp and the standalone test
// project) without violating the one-definition rule.

#include <cstdint>

namespace PokerHandEval
{
	// Category + kicker tiebreak for one best-5-of-N poker hand. The
	// tiebreak slots' meaning depends on category (e.g. two pair ->
	// [highPairRank, lowPairRank, kicker, 0, 0]; flush/high card -> all 5
	// ranks descending) -- CompareHands() only ever compares two
	// HandScores that already have equal category, so both sides' slots
	// line up by construction. Unused trailing slots are 0.
	//
	// Category numbering (0=high card .. 9=royal flush) matches
	// poker_sp.ysc.c's own STATS switch (ground-truth decompile, line
	// ~29880), purely so PokerCheat.cpp's HandCategoryName() labels keep
	// working.
	struct HandScore
	{
		std::int32_t category = -1;
		std::int32_t tiebreak[5] = { 0, 0, 0, 0, 0 };
	};

	// Descending insertion sort for 5 ints -- small and fixed-size enough
	// that a hand-rolled sort is simpler than pulling in <algorithm> for
	// one call site.
	inline void SortDescending5(std::int32_t (&v)[5])
	{
		for (int i = 1; i < 5; i++)
		{
			std::int32_t key = v[i];
			int j = i - 1;
			while (j >= 0 && v[j] < key)
			{
				v[j + 1] = v[j];
				j--;
			}
			v[j + 1] = key;
		}
	}

	// Ranks come straight out of game memory (and PokerCheat.cpp uses -1 as
	// its own "no card" sentinel), and ScoreFiveCards() indexes a count[15]
	// array by rank -- anything outside 2-14 would write out of bounds.
	inline bool IsValidRank(std::int32_t rank)
	{
		return rank >= 2 && rank <= 14;
	}

	// Scores exactly 5 cards (ranks 2-14, suits whatever encoding -- only
	// equality matters here) using standard poker hand rules. Returns
	// category -1 (no hand) if any rank is outside 2-14.
	inline HandScore ScoreFiveCards(std::int32_t ranks[5], std::int32_t suits[5])
	{
		for (int i = 0; i < 5; i++)
		{
			if (!IsValidRank(ranks[i]))
				return HandScore{};
		}

		std::int32_t sorted[5] = { ranks[0], ranks[1], ranks[2], ranks[3], ranks[4] };
		SortDescending5(sorted);

		bool isFlush = suits[0] == suits[1] && suits[1] == suits[2] && suits[2] == suits[3] && suits[3] == suits[4];

		bool isWheel = sorted[0] == 14 && sorted[1] == 5 && sorted[2] == 4 && sorted[3] == 3 && sorted[4] == 2;
		bool isStraight = isWheel;
		if (!isStraight)
		{
			isStraight = true;
			for (int i = 0; i < 4; i++)
			{
				if (sorted[i] - sorted[i + 1] != 1)
				{
					isStraight = false;
					break;
				}
			}
		}
		std::int32_t straightHigh = isWheel ? 5 : sorted[0];

		std::int32_t count[15] = {};
		for (int i = 0; i < 5; i++)
			count[sorted[i]]++;

		std::int32_t quadRank = 0, tripRank = 0;
		std::int32_t pairRanks[2] = { 0, 0 };
		int pairCount = 0;
		std::int32_t kickers[5] = {};
		int kickerCount = 0;

		for (std::int32_t r = 14; r >= 2; r--)
		{
			if (count[r] == 4)
				quadRank = r;
			else if (count[r] == 3)
				tripRank = r;
			else if (count[r] == 2 && pairCount < 2)
				pairRanks[pairCount++] = r;
			else if (count[r] == 1)
				kickers[kickerCount++] = r;
		}

		HandScore result;

		if (isStraight && isFlush)
		{
			result.category = (straightHigh == 14) ? 9 : 8; // royal vs. straight flush
			result.tiebreak[0] = straightHigh;
		}
		else if (quadRank)
		{
			result.category = 7;
			result.tiebreak[0] = quadRank;
			result.tiebreak[1] = kickerCount > 0 ? kickers[0] : 0;
		}
		else if (tripRank && pairCount >= 1)
		{
			result.category = 6;
			result.tiebreak[0] = tripRank;
			result.tiebreak[1] = pairRanks[0];
		}
		else if (isFlush)
		{
			result.category = 5;
			for (int i = 0; i < 5; i++)
				result.tiebreak[i] = sorted[i];
		}
		else if (isStraight)
		{
			result.category = 4;
			result.tiebreak[0] = straightHigh;
		}
		else if (tripRank)
		{
			result.category = 3;
			result.tiebreak[0] = tripRank;
			result.tiebreak[1] = kickerCount > 0 ? kickers[0] : 0;
			result.tiebreak[2] = kickerCount > 1 ? kickers[1] : 0;
		}
		else if (pairCount == 2)
		{
			result.category = 2;
			result.tiebreak[0] = pairRanks[0];
			result.tiebreak[1] = pairRanks[1];
			result.tiebreak[2] = kickerCount > 0 ? kickers[0] : 0;
		}
		else if (pairCount == 1)
		{
			result.category = 1;
			result.tiebreak[0] = pairRanks[0];
			result.tiebreak[1] = kickerCount > 0 ? kickers[0] : 0;
			result.tiebreak[2] = kickerCount > 1 ? kickers[1] : 0;
			result.tiebreak[3] = kickerCount > 2 ? kickers[2] : 0;
		}
		else
		{
			result.category = 0;
			for (int i = 0; i < 5; i++)
				result.tiebreak[i] = sorted[i];
		}

		return result;
	}

	inline bool IsBetterHand(const HandScore& a, const HandScore& b)
	{
		if (a.category != b.category)
			return a.category > b.category;
		for (int i = 0; i < 5; i++)
		{
			if (a.tiebreak[i] != b.tiebreak[i])
				return a.tiebreak[i] > b.tiebreak[i];
		}
		return false;
	}

	// Evaluates the best 5-card hand out of exactly 7 cards (2 hole + a
	// 5-card board) by trying all 21 5-of-7 combinations and keeping the
	// best. Deliberately brute-force rather than "pair ranks + best hole
	// kicker" -- an unpaired board card is fair game as anyone's kicker
	// if their hole cards don't beat it, which a hole-cards-only shortcut
	// would get wrong (see tests/PokerHandEvalTests.cpp's BoardKicker
	// case, taken from a real reported hand). Returns category -1 (no hand)
	// if ANY of the 7 ranks is outside 2-14 -- checked up front rather
	// than left to ScoreFiveCards(), since the combos that happen to skip
	// the bad card would otherwise still score and the result would
	// silently be a best-of-6 hand.
	inline HandScore EvaluateHand(std::int32_t ranks[7], std::int32_t suits[7])
	{
		for (int i = 0; i < 7; i++)
		{
			if (!IsValidRank(ranks[i]))
				return HandScore{};
		}

		static const int kCombos5of7[21][5] = {
			{ 0, 1, 2, 3, 4 }, { 0, 1, 2, 3, 5 }, { 0, 1, 2, 3, 6 }, { 0, 1, 2, 4, 5 }, { 0, 1, 2, 4, 6 },
			{ 0, 1, 2, 5, 6 }, { 0, 1, 3, 4, 5 }, { 0, 1, 3, 4, 6 }, { 0, 1, 3, 5, 6 }, { 0, 1, 4, 5, 6 },
			{ 0, 2, 3, 4, 5 }, { 0, 2, 3, 4, 6 }, { 0, 2, 3, 5, 6 }, { 0, 2, 4, 5, 6 }, { 0, 3, 4, 5, 6 },
			{ 1, 2, 3, 4, 5 }, { 1, 2, 3, 4, 6 }, { 1, 2, 3, 5, 6 }, { 1, 2, 4, 5, 6 }, { 1, 3, 4, 5, 6 },
			{ 2, 3, 4, 5, 6 }
		};

		HandScore best;
		for (const auto& combo : kCombos5of7)
		{
			std::int32_t r[5], s[5];
			for (int i = 0; i < 5; i++)
			{
				r[i] = ranks[combo[i]];
				s[i] = suits[combo[i]];
			}

			HandScore candidate = ScoreFiveCards(r, s);
			if (best.category < 0 || IsBetterHand(candidate, best))
				best = candidate;
		}

		return best;
	}

	// >0 if a beats b, <0 if b beats a, 0 for a genuine tie (chops the
	// pot) -- same category and identical tiebreak slots all the way
	// through.
	inline int CompareHands(const HandScore& a, const HandScore& b)
	{
		if (a.category != b.category)
			return (a.category > b.category) ? 1 : -1;

		for (int i = 0; i < 5; i++)
		{
			if (a.tiebreak[i] != b.tiebreak[i])
				return (a.tiebreak[i] > b.tiebreak[i]) ? 1 : -1;
		}

		return 0;
	}
}
