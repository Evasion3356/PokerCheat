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
#include "../src/HandRecord.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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

	// Replays tests/fixtures/hands.jsonl -- npcAction lines copied out of
	// the mod's PokerCheat_hands.jsonl (Debug build). The action the game
	// really took must be one the model gives a nonzero chance. No file or
	// no lines yet is fine.
	void TestRecordedActions()
	{
		std::ifstream file;
		for (const std::filesystem::path& path : {
			std::filesystem::path(__FILE__).parent_path() / "fixtures" / "hands.jsonl",
			std::filesystem::path("tests") / "fixtures" / "hands.jsonl",
			std::filesystem::path("fixtures") / "hands.jsonl" })
		{
			file.open(path);
			if (file.is_open())
				break;
		}
		if (!file.is_open())
		{
			std::printf("  (no tests/fixtures/hands.jsonl -- skipping recorded actions)\n");
			return;
		}

		int replayed = 0;
		std::string line;
		while (std::getline(file, line))
		{
			std::string type, played, id;
			if (!HandRecord::GetString(line, "type", type) || type != "npcAction" || !HandRecord::GetString(line, "played", played))
				continue;
			HandRecord::GetString(line, "id", id);

			Table table;
			PokerAiOdds::Profile profile;
			int seat = -1;
			float equity = 0.0f;
			const std::string name = "recorded " + id + " (" + played + ")";
			if (!HandRecord::ReadAi(line, table, seat, equity, profile))
			{
				Check(false, (name + " parses").c_str());
				continue;
			}

			const Odds odds = Predict(table, seat, equity, profile);
			const float chance = HandRecord::ChanceOf(odds, HandRecord::ActionFromName(played));
			if (chance <= 0.0f)
				PrintOdds(odds);
			Check(odds.valid && chance > 0.0f, (name + " was predicted possible").c_str());
			replayed++;
		}
		std::printf("  %d recorded action(s) replayed\n", replayed);
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

	// FindFoldBet: you (seat 1, 300 chips, the dealer) to act with nobody
	// having bet; the opponent (seat 0, 200 chips) acts next.
	const auto yourTurn = []()
	{
		Table table = HeadsUp(3, 0);
		table.seats[1].stack = 300;
		return table;
	};

	// Equity 0.5 at position 0.5: score 0.5 either way, the low band. It
	// folds exactly once it owes half its stack -- 100 chips.
	{
		PokerAiOdds::FoldBet fold = PokerAiOdds::FindFoldBet(yourTurn(), 1, 0, 0.5f, MakeProfile(0), 10, 300);
		std::printf("        found=%d bet=%d fold=%.4f\n", fold.found ? 1 : 0, fold.bet, fold.foldChance);
		Check(fold.found && fold.bet == 100 && Near(fold.foldChance, 1.0f), "low band -> folds from a bet of half its stack");
	}

	// Regression (live report: "Raise $22.07: Fold" -- all it had -- then
	// Call 100% after the shove). Equity 0.9 * tight 0.8 = 0.72, the middle
	// band, which never folds on bet size. An all-in raise leaves your seat
	// ACTIVE with a 0 stack (func_1104 never sets f_6 = 2), so the active
	// count and the cutoffs don't move either: no bet folds it.
	{
		PokerAiOdds::FoldBet fold = PokerAiOdds::FindFoldBet(yourTurn(), 1, 0, 0.9f, MakeProfile(2, 0.8f), 10, 300);
		Check(!fold.found, "middle band -> not even an all-in folds it");

		const Table shoved = PokerAiOdds::WithBet(yourTurn(), 1, 300);
		Check(shoved.seats[1].state == 0 && shoved.seats[1].stack == 0 && shoved.callLevel == 300, "an all-in raise keeps your seat active with a 0 stack");
		Odds odds = Predict(shoved, 0, 0.9f, MakeProfile(2, 0.8f));
		PrintOdds(odds);
		Check(odds.valid && Near(odds.call, 1.0f), "after the shove it calls 100%, as seen live");
	}

	// FindValueBet, for a seat you beat: the most it still won't fold to.
	// Low band: safe up to 99, folds from 100 (half its 200 stack).
	{
		PokerAiOdds::FoldBet value = PokerAiOdds::FindValueBet(yourTurn(), 1, 0, 0.5f, MakeProfile(0), 10, 300);
		std::printf("        found=%d bet=%d fold=%.4f\n", value.found ? 1 : 0, value.bet, value.foldChance);
		Check(value.found && value.bet == 99 && Near(value.foldChance, 0.0f), "value bet -> the last bet under half its stack");

		// Middle band: nothing folds it, so everything up to all-in is safe.
		PokerAiOdds::FoldBet strong = PokerAiOdds::FindValueBet(yourTurn(), 1, 0, 0.9f, MakeProfile(2, 0.8f), 10, 300);
		Check(strong.found && strong.bet == 300, "value bet vs. a strong hand -> all-in is safe");

		// No equity: even the minimum bet folds it -- no value bet.
		Check(!PokerAiOdds::FindValueBet(yourTurn(), 1, 0, 0.0f, MakeProfile(0), 10, 300).found, "value bet vs. nothing -> none");
	}

	// Equity 0.9 * loose 1.25 -> 1.0: not below even the all-in fold line,
	// and the flag can't fire (it owes more than its stack, so the
	// commitment term is 0 and 0.7 * 0.5 < 0.6) -- nothing folds it.
	{
		PokerAiOdds::FoldBet fold = PokerAiOdds::FindFoldBet(yourTurn(), 1, 0, 0.9f, MakeProfile(2, 1.25f), 10, 300);
		Check(!fold.found, "maxed-out confidence -> no bet folds it");
	}

	// Three-way: you seat 0, seat 1 between, seat 2 the target (dealer =
	// 2). If seat 1 folds first the active count drops and the cutoffs
	// with it, so the hint must hold either way.
	{
		Table table = HeadsUp(3, 0);
		table.dealer = 2;
		for (int seat = 0; seat < 3; seat++)
		{
			table.seats[seat].occupied = true;
			table.seats[seat].state = 0;
			table.seats[seat].stack = 200;
		}
		const float between = PokerAiOdds::FoldChance(PokerAiOdds::WithSeatsBetweenFolded(table, 0, 2), 2, 0.3f, MakeProfile(0));
		Check(PokerAiOdds::WithSeatsBetweenFolded(table, 0, 2).seats[1].state == PokerAiOdds::kSeatFolded && between >= 0.0f, "seats between you and the target are folded in the worst case");

		PokerAiOdds::FoldBet fold = PokerAiOdds::FindFoldBet(table, 0, 2, 0.5f, MakeProfile(0), 10, 200);
		std::printf("        found=%d bet=%d fold=%.4f\n", fold.found ? 1 : 0, fold.bet, fold.foldChance);
		Check(!fold.found || PokerAiOdds::FoldChanceAgainstBet(table, 0, 2, 0.5f, MakeProfile(0), fold.bet) == fold.foldChance, "three-way hint uses the worst case");
	}

	// HandRecord round trip: an npcAction line reads back to the exact
	// same model input, floats included.
	{
		Table table = HeadsUp(2, 20);
		table.seats[1].streetBet = 20;
		table.pot = 137;
		const PokerAiOdds::Profile profile = MakeProfile(1, 1.25f);
		const float equity = 0.61803398f;

		HandRecord::JsonLine line;
		line.Add("type", "npcAction");
		HandRecord::WriteAi(line, table, 0, equity, profile);
		line.Add("played", HandRecord::ActionName(PokerAiOdds::Action::Call));
		const std::string text = line.Str();

		Table readTable;
		PokerAiOdds::Profile readProfile;
		int readSeat = -1;
		float readEquity = 0.0f;
		std::string played;
		Check(HandRecord::ReadAi(text, readTable, readSeat, readEquity, readProfile) && readTable == table && readProfile == profile &&
			readSeat == 0 && readEquity == equity && HandRecord::GetString(text, "played", played) && played == "call", "hand record round trip is exact");
	}

	TestRecordedActions();

	if (g_failures == 0)
	{
		std::printf("ALL PASS\n");
		return 0;
	}

	std::printf("%d FAILED\n", g_failures);
	return 1;
}
