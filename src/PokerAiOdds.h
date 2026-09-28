#pragma once

// Odds of each action an AI opponent would take if it acted right now --
// a line-by-line port of poker_sp's own decision engine, func_1628
// (poker_sp.ysc.c line 52646; docs/JOURNAL.md Sessions 14 and 25),
// with every input read from the game and only its random draws left open.
//
// Why odds and not one answer: func_1628 rolls MISC::GET_RANDOM_FLOAT_IN_RANGE
// up to three times per decision, off the game's one shared RNG stream,
// which every other system also consumes -- the exact roll can't be known
// ahead (Session 17). Everything else is fixed by the time the seat acts:
//  - equity: f_2655.f_1[seat], stored once per street, not re-rolled per
//    frame -- preflop func_618 writes a fixed hole-card rating (func_1223),
//    and at stakes tiers 1-3 func_692/func_690 replace it with a Monte Carlo
//    win rate once per new street. So the value read is the value used.
//  - the personality's equity multiplier and bet-size ranges (func_584's
//    f_9/f_13 tables), position, stack, the bet to call, the pot.
// So each outcome's chance is integrated over the rolls (uniform, with known
// ranges) on a fixed grid -- deterministic, so the numbers don't flicker.
//
// No game dependency, so tests/PokerAiOddsTests.cpp links this same header
// (same convention as PokerHandEval.h).

#include <array>
#include <cmath>
#include <cstdint>

namespace PokerAiOdds
{
	constexpr int kSeatCount = 6;

	// Seat state (seat.f_6): -1 empty, 0 active, 1 folded, 2 all-in.
	constexpr std::int32_t kSeatActive = 0;

	struct Seat
	{
		bool occupied = false;        // seat.f_0 != -1
		std::int32_t state = -1;      // seat.f_6
		std::int32_t stack = 0;       // seat.f_2
		std::int32_t streetBet = 0;   // seat.f_4
		bool canRaise = true;         // seat.f_5
	};

	// The table as the AI sees it: Table A (f_114.f_287), which is what
	// func_654 hands func_1255/func_1628.
	struct Table
	{
		std::int32_t stakesTier = -1; // f_2 (0 = the free table)
		std::int32_t dealer = -1;     // f_3
		std::int32_t bigBlind = -1;   // f_5
		std::int32_t callLevel = 0;   // f_7 -- the street's highest bet
		std::int32_t lastRaise = 0;   // f_8
		std::int32_t openBet = 0;     // f_10 -- the minimum bet when nobody bet yet
		std::int32_t pot = 0;         // func_870: the pots (f_376) + every seat's street bet
		std::array<Seat, kSeatCount> seats{};
	};

	// One personality's row of func_584's tables: f_9[p1] and f_13[p2].
	struct Profile
	{
		float equityMultiplier = 1.0f;
		std::array<float, 10> sizes{};
	};

	enum class Action { None, Fold, Check, Call, Raise };

	// Chance of each action, summing to 1 (valid == false: no prediction).
	// raise is a bet when nobody has bet yet (callLevel == 0).
	struct Odds
	{
		bool valid = false;
		float fold = 0.0f;
		float check = 0.0f;
		float call = 0.0f;
		float raise = 0.0f;
	};

	namespace Detail
	{
		// func_472 / func_665's per-seat test.
		inline bool IsActive(const Table& table, int seat)
		{
			return seat >= 0 && seat < kSeatCount && table.seats[seat].occupied && table.seats[seat].state == kSeatActive;
		}

		// func_665
		inline int ActiveCount(const Table& table)
		{
			int count = 0;
			for (int i = 0; i < kSeatCount; i++)
			{
				if (IsActive(table, i))
					count++;
			}
			return count;
		}

		// func_1710 -- active and with chips behind.
		inline int ActiveWithChipsCount(const Table& table)
		{
			int count = 0;
			for (int i = 0; i < kSeatCount; i++)
			{
				if (IsActive(table, i) && table.seats[i].stack > 0)
					count++;
			}
			return count;
		}

		// func_1709: 1 at the marker seat, else the share of active seats
		// NOT still to act between this seat and the marker (the big blind
		// on the free table, the dealer otherwise). Returns -1 where the
		// game's own loop would never end (no valid marker).
		inline float Position(const Table& table, int seat)
		{
			const std::int32_t marker = (table.stakesTier == 0) ? table.bigBlind : table.dealer;
			if (marker < 0 || marker >= kSeatCount)
				return -1.0f;

			const int active = ActiveCount(table);
			if (seat == marker)
				return 1.0f;

			int walk = seat;
			int after = 0;
			while (walk != marker)
			{
				walk = (walk + 1) % kSeatCount;
				if (IsActive(table, walk))
					after++;
			}
			return static_cast<float>(active - after) / static_cast<float>(active);
		}

		// func_1757 (func_1614 with a blank settings struct -- the AI's
		// clamp ignores the table cap), then func_1704: the one executor
		// every bet/raise path ends in.
		inline Action Execute(const Table& table, int seat, std::int32_t amount)
		{
			const Seat& s = table.seats[seat];
			std::int32_t min = table.callLevel;
			std::int32_t minRaise = (table.callLevel == 0) ? table.openBet : table.callLevel + table.lastRaise;
			std::int32_t max = s.stack + s.streetBet;
			if (!s.canRaise && max > table.callLevel)
				max = table.callLevel;
			if (min > max)
				min = max;
			if (minRaise > max)
				minRaise = max;
			min -= s.streetBet;
			minRaise -= s.streetBet;
			max -= s.streetBet;

			if (amount < min)
				amount = min;
			else if (amount > min && amount < minRaise)
				amount = (static_cast<float>(amount) < static_cast<float>(min) + static_cast<float>(minRaise) / 2.0f) ? min : minRaise;
			else if (amount > max)
				amount = max;

			const std::int32_t owed = table.callLevel - s.streetBet;
			if (amount > owed && ActiveWithChipsCount(table) > 1)
				return Action::Raise;
			return (owed != 0) ? Action::Call : Action::Check;
		}

		// func_1715 (base = the pot) / func_1716 (base = the bet owed):
		// ceil(base * random(lo, hi)), then Execute. roll is in [0, 1).
		inline Action SizedBet(const Table& table, int seat, std::int32_t base, float lo, float hi, float roll)
		{
			const float fraction = lo + roll * (hi - lo);
			const std::int32_t amount = static_cast<std::int32_t>(std::ceil(static_cast<float>(base) * fraction));
			return Execute(table, seat, amount);
		}

		// func_1707: check if nothing is owed, else fold.
		inline Action FoldOrCheck(const Table& table, int seat)
		{
			return (table.callLevel - table.seats[seat].streetBet == 0) ? Action::Check : Action::Fold;
		}

		// func_1712 / func_1713 / func_1718's cutoffs, by active-seat count.
		constexpr std::array<float, 7> kFoldBelow = { 1.0f, 1.0f, 0.35f, 0.4f, 0.45f, 0.5f, 0.5f };
		constexpr std::array<float, 7> kLowBandBelow = { 1.0f, 1.0f, 0.6f, 0.65f, 0.7f, 0.75f, 0.75f };
		constexpr std::array<float, 7> kBigRaiseFrom = { 1.0f, 1.0f, 0.8f, 0.8f, 0.85f, 0.85f, 0.85f };

		// func_1628 from the `flag` roll on. aggressionRoll is func_1717's
		// draw and sizeRoll the bet-size draw, both in [0, 1).
		inline Action DecideGivenFlag(const Table& table, int seat, float confidence, float position, bool flag,
			const Profile& profile, float aggressionRoll, float sizeRoll)
		{
			const Seat& s = table.seats[seat];
			const std::int32_t owed = table.callLevel - s.streetBet;

			float score = 0.0f;
			switch (table.stakesTier)
			{
				case 0: score = flag ? 0.5f * position + 0.5f * confidence : 0.3f * position + 0.7f * confidence; break;
				case 1: score = flag ? 0.6f * position + 0.4f * confidence : 0.15f * position + 0.85f * confidence; break;
				case 2: score = flag ? 0.5f * position + 0.5f * confidence : 0.1f * position + 0.9f * confidence; break;
				case 3: score = flag ? 0.5f * position + 0.5f * confidence : 0.0f * position + 1.0f * confidence; break;
				default: return Action::None;
			}

			const int active = ActiveCount(table);
			if (ActiveWithChipsCount(table) < 2 && table.callLevel == 0)
				return (owed == 0) ? Action::Check : Action::None; // func_1711
			if (score < kFoldBelow[active])
				return FoldOrCheck(table, seat);
			if (score < kLowBandBelow[active])
			{
				if (owed >= s.stack / 2) // func_1714
					return FoldOrCheck(table, seat);
				if (table.callLevel == 0)
					return SizedBet(table, seat, table.pot, profile.sizes[0], profile.sizes[1], sizeRoll);
				return SizedBet(table, seat, owed, profile.sizes[4], profile.sizes[5], sizeRoll);
			}

			// func_1717: at the dealer seat of a tier-3 table the roll is
			// skipped (always 1, so never "<= 0.4").
			float aggression = 1.0f;
			if (!(position == 1.0f && table.stakesTier == 3))
				aggression = aggressionRoll * ((position > 0.0f) ? position : 1.0f);
			if (!(aggression <= 0.4f))
			{
				if (owed == 0)
					return SizedBet(table, seat, table.pot, profile.sizes[2], profile.sizes[3], sizeRoll);
				return SizedBet(table, seat, owed, profile.sizes[6], profile.sizes[7], sizeRoll);
			}
			if (owed == 0)
				return Action::Check; // func_1711
			if (!(score >= kBigRaiseFrom[active]))
				return (s.stack == 0) ? Action::None : Action::Call; // func_1719
			return SizedBet(table, seat, owed, profile.sizes[8], profile.sizes[9], sizeRoll);
		}

		// Midpoints of an even grid over [0, 1), one per random draw.
		constexpr int kGrid = 48;
		inline float GridPoint(int i) { return (static_cast<float>(i) + 0.5f) / static_cast<float>(kGrid); }
	}

	// Odds for `seat` acting now, against the table as it stands. equity is
	// f_2655.f_1[seat]. Only for the ordinary engine (personality styleCode
	// 0, func_1255's default -- every personality func_185 ever seats);
	// callers skip the card-blind special styles.
	inline Odds Predict(const Table& table, int seat, float equity, const Profile& profile)
	{
		using namespace Detail;

		Odds odds;
		if (!IsActive(table, seat) || table.seats[seat].stack <= 0 || table.stakesTier < 0 || table.stakesTier > 3)
			return odds;

		const Seat& s = table.seats[seat];

		// func_1708: equity * multiplier, clamped to [0, 1].
		float confidence = equity * profile.equityMultiplier;
		if (confidence < 0.0f)
			confidence = 0.0f;
		if (confidence > 1.0f)
			confidence = 1.0f;

		const float position = Position(table, seat);
		if (position < 0.0f)
			return odds;

		float commitment = (static_cast<float>(s.stack) - static_cast<float>(table.callLevel)) / static_cast<float>(s.stack);
		if (commitment < 0.0f)
			commitment = 0.0f;

		// flag = 0.7 * random(0, position) + 0.3 * random(0, commitment) > 0.6
		int flagHits = 0;
		for (int i = 0; i < kGrid; i++)
		{
			for (int j = 0; j < kGrid; j++)
			{
				if (GridPoint(i) * position * 0.7f + GridPoint(j) * commitment * 0.3f > 0.6f)
					flagHits++;
			}
		}
		const float cells = static_cast<float>(kGrid * kGrid);
		const float flagChance = static_cast<float>(flagHits) / cells;

		float total = 0.0f;
		for (int f = 0; f < 2; f++)
		{
			const bool flag = (f == 1);
			const float weight = (flag ? flagChance : 1.0f - flagChance) / cells;
			if (weight <= 0.0f)
				continue;

			for (int i = 0; i < kGrid; i++)
			{
				for (int j = 0; j < kGrid; j++)
				{
					switch (DecideGivenFlag(table, seat, confidence, position, flag, profile, GridPoint(i), GridPoint(j)))
					{
						case Action::Fold: odds.fold += weight; break;
						case Action::Check: odds.check += weight; break;
						case Action::Call: odds.call += weight; break;
						case Action::Raise: odds.raise += weight; break;
						case Action::None: continue;
					}
					total += weight;
				}
			}
		}

		if (total <= 0.0f)
			return odds;

		odds.fold /= total;
		odds.check /= total;
		odds.call /= total;
		odds.raise /= total;
		odds.valid = true;
		return odds;
	}
}
