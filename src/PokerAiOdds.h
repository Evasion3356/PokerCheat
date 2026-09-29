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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace PokerAiOdds
{
	constexpr int kSeatCount = 6;

	// Seat state (seat.f_6): -1 empty, 0 active, 1 folded, 2 all-in.
	constexpr std::int32_t kSeatActive = 0;
	constexpr std::int32_t kSeatFolded = 1;
	constexpr std::int32_t kSeatAllIn = 2;

	struct Seat
	{
		bool occupied = false;        // seat.f_0 != -1
		std::int32_t state = -1;      // seat.f_6
		std::int32_t stack = 0;       // seat.f_2
		std::int32_t streetBet = 0;   // seat.f_4
		bool canRaise = true;         // seat.f_5

		bool operator==(const Seat&) const = default;
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

		bool operator==(const Table&) const = default;
	};

	// One personality's row of func_584's tables: f_9[p1] and f_13[p2].
	struct Profile
	{
		float equityMultiplier = 1.0f;
		std::array<float, 10> sizes{};

		bool operator==(const Profile&) const = default;
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

		// Everything func_1628 settles before its first threshold: the
		// personality-scaled equity, position, and the chance of the
		// "aggressive this decision" flag. valid == false: no prediction.
		struct Inputs
		{
			bool valid = false;
			float confidence = 0.0f;
			float position = 0.0f;
			float flagChance = 0.0f;
		};

		inline Inputs Prepare(const Table& table, int seat, float equity, const Profile& profile)
		{
			Inputs in;
			if (!IsActive(table, seat) || table.seats[seat].stack <= 0 || table.stakesTier < 0 || table.stakesTier > 3)
				return in;

			const Seat& s = table.seats[seat];

			// func_1708: equity * multiplier, clamped to [0, 1].
			in.confidence = equity * profile.equityMultiplier;
			if (in.confidence < 0.0f)
				in.confidence = 0.0f;
			if (in.confidence > 1.0f)
				in.confidence = 1.0f;

			in.position = Position(table, seat);
			if (in.position < 0.0f)
				return in;

			float commitment = (static_cast<float>(s.stack) - static_cast<float>(table.callLevel)) / static_cast<float>(s.stack);
			if (commitment < 0.0f)
				commitment = 0.0f;

			// flag = 0.7 * random(0, position) + 0.3 * random(0, commitment) > 0.6
			int flagHits = 0;
			for (int i = 0; i < kGrid; i++)
			{
				for (int j = 0; j < kGrid; j++)
				{
					if (GridPoint(i) * in.position * 0.7f + GridPoint(j) * commitment * 0.3f > 0.6f)
						flagHits++;
				}
			}
			in.flagChance = static_cast<float>(flagHits) / static_cast<float>(kGrid * kGrid);
			in.valid = true;
			return in;
		}
	}

	// Odds for `seat` acting now, against the table as it stands. equity is
	// f_2655.f_1[seat]. Only for the ordinary engine (personality styleCode
	// 0, func_1255's default -- every personality func_185 ever seats);
	// callers skip the card-blind special styles.
	inline Odds Predict(const Table& table, int seat, float equity, const Profile& profile)
	{
		using namespace Detail;

		Odds odds;
		const Inputs in = Prepare(table, seat, equity, profile);
		if (!in.valid)
			return odds;

		const float cells = static_cast<float>(kGrid * kGrid);
		float total = 0.0f;
		for (int f = 0; f < 2; f++)
		{
			const bool flag = (f == 1);
			const float weight = (flag ? in.flagChance : 1.0f - in.flagChance) / cells;
			if (weight <= 0.0f)
				continue;

			for (int i = 0; i < kGrid; i++)
			{
				for (int j = 0; j < kGrid; j++)
				{
					switch (DecideGivenFlag(table, seat, in.confidence, in.position, flag, profile, GridPoint(i), GridPoint(j)))
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

	// Chance `seat` folds if it acted now. Unlike Predict() this needs no
	// grid over the aggression/bet-size rolls: func_1628 only folds from
	// its two threshold tests, which run before either roll, so only the
	// flag's chance matters. Negative: no prediction.
	inline float FoldChance(const Table& table, int seat, float equity, const Profile& profile)
	{
		using namespace Detail;

		const Inputs in = Prepare(table, seat, equity, profile);
		if (!in.valid)
			return -1.0f;

		float fold = 0.0f;
		if (DecideGivenFlag(table, seat, in.confidence, in.position, true, profile, 0.5f, 0.5f) == Action::Fold)
			fold += in.flagChance;
		if (DecideGivenFlag(table, seat, in.confidence, in.position, false, profile, 0.5f, 0.5f) == Action::Fold)
			fold += 1.0f - in.flagChance;
		return fold;
	}

	// The table after `me` raises by putting `bet` more chips in this
	// street -- func_1104's raise case (poker_sp.ysc.c line ~39993). Going
	// all-in does NOT make `me` all-in here: that case never touches
	// seat.f_6, so the seat stays active (0) with a 0 stack until func_468
	// marks all-in seats (2) once the betting round ends. It still counts
	// in func_665's active count (the AI's cutoffs), just not in
	// func_1710's "has chips" count. Assuming state 2 here once predicted a
	// fold that the live game (Call 100%) didn't make.
	inline Table WithBet(const Table& table, int me, std::int32_t bet)
	{
		Table after = table;
		Seat& s = after.seats[me];
		s.streetBet += bet;
		s.stack -= bet;
		if (s.stack < 0)
			s.stack = 0;
		after.pot += bet;

		const std::int32_t raise = s.streetBet - after.callLevel;
		if (raise <= 0)
			return after;

		after.callLevel = s.streetBet;
		after.lastRaise = (after.lastRaise > raise) ? after.lastRaise : raise; // func_1552
		for (int i = 0; i < kSeatCount; i++)
		{
			if (!after.seats[i].occupied)
				continue;
			if (i == me)
				after.seats[i].canRaise = false;
			else if (raise >= after.lastRaise)
				after.seats[i].canRaise = after.seats[i].stack != 0;
		}
		return after;
	}

	// The same table with every active seat that acts after `me` and
	// before `seat` (seats act in increasing order, see func_1709) folded.
	inline Table WithSeatsBetweenFolded(const Table& table, int me, int seat)
	{
		Table after = table;
		for (int walk = (me + 1) % kSeatCount; walk != seat && walk != me; walk = (walk + 1) % kSeatCount)
		{
			if (Detail::IsActive(after, walk))
				after.seats[walk].state = kSeatFolded;
		}
		return after;
	}

	// A bet hint, as the chips `me` would enter in the bet box (on top of
	// its street bet), and the fold chance it leaves -- see FindFoldBet()
	// and FindValueBet().
	struct FoldBet
	{
		bool found = false;
		std::int32_t bet = 0;
		float foldChance = 0.0f;
	};

	// Fold chance against `bet`, taking the worse of the seats between
	// `me` and `seat` all staying in or all folding (folds change the
	// active count, and with it the AI's cutoffs). Heads-up both are the
	// same table.
	inline float FoldChanceAgainstBet(const Table& table, int me, int seat, float equity, const Profile& profile, std::int32_t bet)
	{
		const Table after = WithBet(table, me, bet);
		const float stay = FoldChance(after, seat, equity, profile);
		const float fold = FoldChance(WithSeatsBetweenFolded(after, me, seat), seat, equity, profile);
		return (stay < fold) ? stay : fold;
	}

	// Fold chance at an even spread of bets from minBet to maxBet, plus the
	// bet where the owed amount reaches half the seat's stack exactly: the
	// fold chance jumps there (func_1714) and otherwise only moves with the
	// flag's chance, so the spread plus that bet catches every step.
	struct BetScan
	{
		static constexpr std::int32_t kSteps = 32;
		std::array<std::int32_t, kSteps + 2> bets{};
		std::array<float, kSteps + 2> chances{};
		std::size_t count = 0;
	};

	inline BetScan ScanBets(const Table& table, int me, int seat, float equity, const Profile& profile, std::int32_t minBet, std::int32_t maxBet)
	{
		BetScan scan;
		const Seat& target = table.seats[seat];
		const std::int32_t halfStackBet = target.streetBet + target.stack / 2 - table.seats[me].streetBet;
		for (std::int32_t i = 0; i <= BetScan::kSteps; i++)
			scan.bets[scan.count++] = minBet + static_cast<std::int32_t>((static_cast<std::int64_t>(maxBet - minBet) * i) / BetScan::kSteps);
		if (halfStackBet > minBet && halfStackBet < maxBet)
			scan.bets[scan.count++] = halfStackBet;
		std::sort(scan.bets.begin(), scan.bets.begin() + static_cast<std::ptrdiff_t>(scan.count));
		for (std::size_t i = 0; i < scan.count; i++)
			scan.chances[i] = FoldChanceAgainstBet(table, me, seat, equity, profile, scan.bets[i]);
		return scan;
	}

	// For a seat that beats you (or ties): the smallest bet that makes it
	// fold -- a bluff. Keeps the smallest bet within half a percent of the
	// best fold chance seen; not found if no bet folds it more often than
	// it already would.
	inline FoldBet FindFoldBet(const Table& table, int me, int seat, float equity, const Profile& profile, std::int32_t minBet, std::int32_t maxBet)
	{
		FoldBet result;
		if (me == seat || minBet <= 0 || minBet > maxBet)
			return result;

		const float now = FoldChance(table, seat, equity, profile);
		if (now < 0.0f || now >= 0.995f)
			return result;

		const BetScan scan = ScanBets(table, me, seat, equity, profile, minBet, maxBet);
		float best = -1.0f;
		for (std::size_t i = 0; i < scan.count; i++)
			best = (scan.chances[i] > best) ? scan.chances[i] : best;
		if (best < now + 0.01f)
			return result;

		for (std::size_t i = 0; i < scan.count; i++)
		{
			if (scan.chances[i] < best - 0.005f)
				continue;

			// Narrow the step before it down to the exact smallest bet.
			std::int32_t low = (i == 0) ? scan.bets[0] : scan.bets[i - 1];
			std::int32_t high = scan.bets[i];
			while (low < high)
			{
				const std::int32_t mid = low + (high - low) / 2;
				if (FoldChanceAgainstBet(table, me, seat, equity, profile, mid) >= best - 0.005f)
					high = mid;
				else
					low = mid + 1;
			}

			result.found = true;
			result.bet = high;
			result.foldChance = FoldChanceAgainstBet(table, me, seat, equity, profile, high);
			return result;
		}
		return result;
	}

	// For a seat you beat: the biggest bet it still won't fold to (under
	// half a percent) -- as much as it will pay you. Not found if even the
	// minimum raise risks a fold, or if it's out of the hand.
	inline FoldBet FindValueBet(const Table& table, int me, int seat, float equity, const Profile& profile, std::int32_t minBet, std::int32_t maxBet)
	{
		FoldBet result;
		if (me == seat || minBet <= 0 || minBet > maxBet || FoldChance(table, seat, equity, profile) < 0.0f)
			return result;

		constexpr float kSafe = 0.005f;
		const BetScan scan = ScanBets(table, me, seat, equity, profile, minBet, maxBet);
		if (scan.chances[0] < 0.0f || scan.chances[0] >= kSafe)
			return result;

		// The last safe bet of the leading safe run, then narrowed toward
		// the next (unsafe) scanned bet.
		std::size_t last = 0;
		while (last + 1 < scan.count && scan.chances[last + 1] >= 0.0f && scan.chances[last + 1] < kSafe)
			last++;
		std::int32_t low = scan.bets[last];
		if (last + 1 < scan.count)
		{
			std::int32_t high = scan.bets[last + 1] - 1;
			while (low < high)
			{
				const std::int32_t mid = low + (high - low + 1) / 2;
				const float chance = FoldChanceAgainstBet(table, me, seat, equity, profile, mid);
				if (chance >= 0.0f && chance < kSafe)
					low = mid;
				else
					high = mid - 1;
			}
		}

		result.found = true;
		result.bet = low;
		result.foldChance = FoldChanceAgainstBet(table, me, seat, equity, profile, low);
		return result;
	}
}
