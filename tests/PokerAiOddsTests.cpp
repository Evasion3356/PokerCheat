// Unit tests for src/PokerAiOdds.h -- the port of poker_sp's AI decision
// engine (func_1628) that turns each opponent's live inputs into
// Fold/Check/Call/Raise odds. Links the same header the mod uses, like
// PokerHandEvalTests.cpp. Each expected value is worked out by hand from
// the decompile in the comment above its case.
//
//   MSBuild.exe tests\PokerAiOddsTests.vcxproj /p:Configuration=Debug /p:Platform=x64
//   bin\Debug\PokerAiOddsTests.exe
// Exits 0 and prints "ALL PASS" if every case passes, 1 otherwise.

#include "../src/PokerAiOdds.h"

#include <cmath>
#include <cstdio>

namespace
{
	using PokerAiOdds::Odds;
	using PokerAiOdds::Predict;
	using PokerAiOdds::Profile;
	using PokerAiOdds::Table;

	int g_failures = 0;

	void Check(bool condition, const char* testName)
	{
		std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", testName);
		if (!condition)
			g_failures++;
	}

	bool Near(float a, float b, float tolerance = 0.0001f)
	{
		return std::fabs(a - b) <= tolerance;
	}

	void PrintOdds(const Odds& odds)
	{
		std::printf("        fold=%.4f check=%.4f call=%.4f raise=%.4f valid=%d\n", odds.fold, odds.check, odds.call, odds.raise, odds.valid ? 1 : 0);
	}

	// func_584's three f_13 rows (p2 = 0 passive .. 2 aggressive).
	Profile MakeProfile(int row, float multiplier = 1.0f)
	{
		Profile profile;
		profile.equityMultiplier = multiplier;
		switch (row)
		{
			case 0: profile.sizes = { 0.05f, 0.35f, 0.1f, 0.55f, 1.0f, 1.0f, 1.0f, 1.5f, 1.5f, 3.0f }; break;
			case 1: profile.sizes = { 0.1f, 0.7f, 0.2f, 1.1f, 1.0f, 1.0f, 1.0f, 2.5f, 2.0f, 5.0f }; break;
			default: profile.sizes = { 0.4f, 1.0f, 0.5f, 1.4f, 0.2f, 1.2f, 2.0f, 3.5f, 3.0f, 6.0f }; break;
		}
		return profile;
	}

	// Two players heads-up, seats 0 and 1, 200 chips each, seat 1 the
	// dealer: seat 0 has seat 1 still to act, so func_1709 gives
	// (2 - 1) / 2 = 0.5.
	Table HeadsUp(int stakesTier, int callLevel)
	{
		Table table;
		table.stakesTier = stakesTier;
		table.dealer = 1;
		table.bigBlind = 1;
		table.callLevel = callLevel;
		table.lastRaise = callLevel;
		table.openBet = 10;
		table.pot = 100;
		for (int seat = 0; seat < 2; seat++)
		{
			table.seats[seat].occupied = true;
			table.seats[seat].state = 0;
			table.seats[seat].stack = 200;
		}
		return table;
	}

	bool SumsToOne(const Odds& odds)
	{
		return Near(odds.fold + odds.check + odds.call + odds.raise, 1.0f, 0.001f);
	}
}

int main()
{
	std::printf("PokerAiOdds tests\n");

	// Nothing to call, equity 1 at the dealer seat of a tier-3 table:
	// score 1 skips both low bands, func_1717's roll is skipped there
	// (always "not <= 0.4"), so it bets pot * random(0.1, 0.55) = 10-55,
	// never under the 10-chip open bet -- always a bet.
	{
		Table table = HeadsUp(3, 0);
		Odds odds = Predict(table, 1, 1.0f, MakeProfile(0));
		PrintOdds(odds);
		Check(odds.valid && Near(odds.raise, 1.0f), "monster hand, dealer, nothing owed -> always bets");
	}

	// Equity 0 facing a 20-chip bet: score is at most 0.5 * 0.5 = 0.25,
	// under heads-up's 0.35 fold line either way the flag rolls.
	{
		Table table = HeadsUp(3, 20);
		Odds odds = Predict(table, 0, 0.0f, MakeProfile(0));
		PrintOdds(odds);
		Check(odds.valid && Near(odds.fold, 1.0f), "no equity facing a bet -> always folds");
	}

	// Same, but nothing owed: func_1707 checks instead of folding.
	{
		Table table = HeadsUp(3, 0);
		Odds odds = Predict(table, 0, 0.0f, MakeProfile(0));
		PrintOdds(odds);
		Check(odds.valid && Near(odds.check, 1.0f), "no equity, nothing owed -> always checks");
	}

	// Equity 0.5, position 0.5: score 0.5 with or without the flag -- the
	// low band. The passive row's "raise" is owed * random(1, 1) = exactly
	// the call, which func_1757 leaves as the call -- so it calls.
	{
		Table table = HeadsUp(3, 20);
		Odds odds = Predict(table, 0, 0.5f, MakeProfile(0));
		PrintOdds(odds);
		Check(odds.valid && Near(odds.call, 1.0f), "low band, passive sizing -> the raise clamps to a call");
	}

	// Same, bet owed at least half the stack: func_1714 fails -> fold.
	{
		Table table = HeadsUp(3, 100);
		Odds odds = Predict(table, 0, 0.5f, MakeProfile(0));
		PrintOdds(odds);
		Check(odds.valid && Near(odds.fold, 1.0f), "low band, bet >= half the stack -> folds");
	}

	// Equity 0.72: score 0.61 (flag) or 0.72 -- the middle band. func_1717
	// rolls random(0, 0.5) <= 0.4 on 38 of 48 grid points -> call; the
	// other 10 raise owed * random(2, 3.5) = 40-70, at least the 40-chip
	// minimum raise.
	{
		Table table = HeadsUp(3, 20);
		Odds odds = Predict(table, 0, 0.72f, MakeProfile(2));
		PrintOdds(odds);
		Check(odds.valid && Near(odds.call, 38.0f / 48.0f, 0.001f) && Near(odds.raise, 10.0f / 48.0f, 0.001f), "middle band -> 79% call / 21% raise");
		Check(SumsToOne(odds), "middle band odds sum to 1");
	}

	// Personality multiplier: 0.5 equity * 1.25 (loose) = 0.625 moves the
	// same spot from the all-call low band into the middle band.
	{
		Table table = HeadsUp(3, 20);
		Odds tight = Predict(table, 0, 0.5f, MakeProfile(2, 0.8f));
		Odds loose = Predict(table, 0, 0.5f, MakeProfile(2, 1.25f));
		PrintOdds(tight);
		PrintOdds(loose);
		Check(tight.valid && loose.valid && loose.raise > tight.raise, "a loose multiplier raises more often than a tight one");
	}

	// No prediction: a folded seat, and a table with no dealer marker
	// (func_1709's walk would never end).
	{
		Table table = HeadsUp(3, 20);
		table.seats[0].state = 1;
		Check(!Predict(table, 0, 0.5f, MakeProfile(0)).valid, "folded seat -> no prediction");

		Table noDealer = HeadsUp(3, 20);
		noDealer.dealer = -1;
		Check(!Predict(noDealer, 0, 0.5f, MakeProfile(0)).valid, "no dealer marker -> no prediction");
	}

	// Deterministic: same inputs, same odds (the HUD must not flicker).
	{
		Table table = HeadsUp(1, 20);
		Odds a = Predict(table, 0, 0.6f, MakeProfile(1));
		Odds b = Predict(table, 0, 0.6f, MakeProfile(1));
		Check(a.valid && a.fold == b.fold && a.call == b.call && a.raise == b.raise && a.check == b.check, "same inputs -> identical odds");
		Check(SumsToOne(a), "tier-1 odds sum to 1");
	}

	if (g_failures == 0)
	{
		std::printf("ALL PASS\n");
		return 0;
	}

	std::printf("%d FAILED\n", g_failures);
	return 1;
}
