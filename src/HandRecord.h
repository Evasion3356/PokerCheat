/*
	HandRecord.h -- the hand log's JSONL line format, shared by the mod
	(which writes PokerCheat_hands.jsonl next to PokerCheat.log, Debug build
	only -- see PokerCheat.cpp's HandRecorder section) and
	tests/PokerAiOddsTests.cpp (which replays lines copied from it into
	tests/fixtures/hands.jsonl). No game dependency. Ported from
	DominoCheat's GameRecord.h (same flat-line writer and reader), plus
	floats, written shortest-round-trip so a replay gets the exact value.

	Two line types, each a flat JSON object on its own line, so any single
	line can be copied into the fixture file as a self-contained test case.

	  {"type":"npcAction", ...}  one per opponent decision: the whole
	      PokerAiOdds input as the AI saw it (WriteAi(): the Table A fields,
	      per-seat arrays, "seat", "equity", "multiplier", "sizes"), the
	      predicted odds ("pFold", "pCheck", "pCall", "pRaise"), what it
	      really did ("played": fold/check/call/raise, "put": chips it put
	      in), "pPlayed" (the chance the model gave that), and -- when you
	      had a bet hint up for this seat on your last turn -- "hintKind"
	      ("value": the most it still wouldn't fold to, for a seat you
	      beat; "bluff": the least that folds it, for one that beats or
	      ties you), "hintBet", "hintFold" and "myBet" (what you actually
	      put in).
	  {"type":"hand", ...}       one per finished hand, written at the next
	      deal (or when the table closes, "endedBy"): every seat's hole
	      cards ("hole", 4 ints per seat, -1 if unseen), the board predicted
	      at the deal vs. the real one ("predictedBoard", "realBoard",
	      "boardMatch" -- only when all 5 came out; cards as rank,suit
	      pairs), each seat's last state,
	      chips before and after, who our evaluator says wins at showdown
	      among the seats still in ("predictedWinners") vs. who gained
	      chips ("actualWinners", "winnerMatch"), and the hand's npcAction
	      tallies.

	To turn an npcAction line into a test: copy it into
	tests/fixtures/hands.jsonl. Every npcAction line there is replayed
	through PokerAiOdds::Predict(), and the action the game really took must
	have a nonzero predicted chance.

	The reader only understands this file's own flat format (a top-level
	key's int, float, bool, string or number array) -- not general JSON.
*/

#pragma once

#include "PokerAiOdds.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace HandRecord
{
	class JsonLine
	{
	public:
		JsonLine& Add(std::string_view key, std::int64_t value)
		{
			Key(key);
			m_out << value;
			return *this;
		}

		JsonLine& Add(std::string_view key, bool value)
		{
			Key(key);
			m_out << (value ? "true" : "false");
			return *this;
		}

		JsonLine& Add(std::string_view key, float value)
		{
			Key(key);
			Float(value);
			return *this;
		}

		JsonLine& Add(std::string_view key, std::string_view value)
		{
			Key(key);
			Quoted(value);
			return *this;
		}

		JsonLine& Add(std::string_view key, const char* value)
		{
			return Add(key, std::string_view(value));
		}

		JsonLine& Add(std::string_view key, const std::int32_t* values, int count)
		{
			Key(key);
			m_out << '[';
			for (int i = 0; i < count; i++)
				m_out << (i ? "," : "") << values[i];
			m_out << ']';
			return *this;
		}

		JsonLine& Add(std::string_view key, const std::vector<std::int32_t>& values)
		{
			return Add(key, values.data(), static_cast<int>(values.size()));
		}

		JsonLine& Add(std::string_view key, const float* values, int count)
		{
			Key(key);
			m_out << '[';
			for (int i = 0; i < count; i++)
			{
				if (i)
					m_out << ',';
				Float(values[i]);
			}
			m_out << ']';
			return *this;
		}

		std::string Str() const
		{
			return m_out.str() + "}";
		}

	private:
		void Key(std::string_view key)
		{
			m_out << (m_first ? "{" : ",");
			m_first = false;
			Quoted(key);
			m_out << ':';
		}

		void Quoted(std::string_view text)
		{
			m_out << '"';
			for (char c : text)
			{
				if (c == '"' || c == '\\')
					m_out << '\\';
				m_out << c;
			}
			m_out << '"';
		}

		// Shortest text that reads back as the same float. NaN/inf (never
		// expected) become 0 so the line stays valid JSON.
		void Float(float value)
		{
			if (value != value || value - value != 0.0f)
				value = 0.0f;
			std::array<char, 32> text{};
			m_out << std::string_view(text.data(), static_cast<std::size_t>(std::to_chars(text.data(), text.data() + text.size(), value).ptr - text.data()));
		}

		std::ostringstream m_out;
		bool m_first = true;
	};

	// Position just past `"key":` (skipping spaces), or npos. Only matches
	// a key, not the same text inside a string value, since a key is
	// always directly preceded by '{' or ','.
	inline std::size_t FindValue(std::string_view line, std::string_view key)
	{
		const std::string needle = "\"" + std::string(key) + "\"";
		for (std::size_t pos = line.find(needle); pos != std::string_view::npos; pos = line.find(needle, pos + 1))
		{
			std::size_t before = pos;
			while (before > 0 && line[before - 1] == ' ')
				before--;
			if (before == 0 || (line[before - 1] != '{' && line[before - 1] != ','))
				continue;

			std::size_t after = pos + needle.size();
			while (after < line.size() && line[after] == ' ')
				after++;
			if (after >= line.size() || line[after] != ':')
				continue;
			after++;
			while (after < line.size() && line[after] == ' ')
				after++;
			return after;
		}
		return std::string_view::npos;
	}

	// One number at pos (int or float text), advancing pos past it.
	inline bool ParseFloat(std::string_view line, std::size_t& pos, float& out)
	{
		const char* begin = line.data() + pos;
		const auto [end, ec] = std::from_chars(begin, line.data() + line.size(), out);
		if (ec != std::errc())
			return false;
		pos += static_cast<std::size_t>(end - begin);
		return true;
	}

	inline bool ParseInt(std::string_view line, std::size_t& pos, std::int32_t& out)
	{
		const char* begin = line.data() + pos;
		const auto [end, ec] = std::from_chars(begin, line.data() + line.size(), out);
		if (ec != std::errc())
			return false;
		pos += static_cast<std::size_t>(end - begin);
		return true;
	}

	inline bool GetInt(std::string_view line, std::string_view key, std::int32_t& out)
	{
		std::size_t pos = FindValue(line, key);
		return pos != std::string_view::npos && ParseInt(line, pos, out);
	}

	inline bool GetFloat(std::string_view line, std::string_view key, float& out)
	{
		std::size_t pos = FindValue(line, key);
		return pos != std::string_view::npos && ParseFloat(line, pos, out);
	}

	inline bool GetString(std::string_view line, std::string_view key, std::string& out)
	{
		std::size_t pos = FindValue(line, key);
		if (pos == std::string_view::npos || line[pos] != '"')
			return false;
		out.clear();
		for (pos++; pos < line.size(); pos++)
		{
			if (line[pos] == '"')
				return true;
			if (line[pos] == '\\' && pos + 1 < line.size())
				pos++;
			out += line[pos];
		}
		return false;
	}

	template <typename T, typename Parse>
	bool GetArray(std::string_view line, std::string_view key, std::vector<T>& out, Parse parse)
	{
		std::size_t pos = FindValue(line, key);
		if (pos == std::string_view::npos || line[pos] != '[')
			return false;
		out.clear();
		pos++;
		while (pos < line.size())
		{
			while (pos < line.size() && (line[pos] == ' ' || line[pos] == ','))
				pos++;
			if (pos < line.size() && line[pos] == ']')
				return true;
			T value{};
			if (!parse(line, pos, value))
				return false;
			out.push_back(value);
		}
		return false;
	}

	inline bool GetIntArray(std::string_view line, std::string_view key, std::vector<std::int32_t>& out)
	{
		return GetArray(line, key, out, ParseInt);
	}

	inline bool GetFloatArray(std::string_view line, std::string_view key, std::vector<float>& out)
	{
		return GetArray(line, key, out, ParseFloat);
	}

	inline std::string_view ActionName(PokerAiOdds::Action action)
	{
		switch (action)
		{
			case PokerAiOdds::Action::Fold: return "fold";
			case PokerAiOdds::Action::Check: return "check";
			case PokerAiOdds::Action::Call: return "call";
			case PokerAiOdds::Action::Raise: return "raise";
			default: return "none";
		}
	}

	inline PokerAiOdds::Action ActionFromName(std::string_view name)
	{
		using PokerAiOdds::Action;
		for (Action action : { Action::Fold, Action::Check, Action::Call, Action::Raise })
			if (name == ActionName(action))
				return action;
		return Action::None;
	}

	// The predicted chance of one action.
	inline float ChanceOf(const PokerAiOdds::Odds& odds, PokerAiOdds::Action action)
	{
		switch (action)
		{
			case PokerAiOdds::Action::Fold: return odds.fold;
			case PokerAiOdds::Action::Check: return odds.check;
			case PokerAiOdds::Action::Call: return odds.call;
			case PokerAiOdds::Action::Raise: return odds.raise;
			default: return 0.0f;
		}
	}

	// PokerAiOdds::Predict()'s whole input for `seat`.
	inline void WriteAi(JsonLine& line, const PokerAiOdds::Table& table, int seat, float equity, const PokerAiOdds::Profile& profile)
	{
		using PokerAiOdds::kSeatCount;
		std::int32_t occupied[kSeatCount]{}, state[kSeatCount]{}, stack[kSeatCount]{}, streetBet[kSeatCount]{}, canRaise[kSeatCount]{};
		for (int i = 0; i < kSeatCount; i++)
		{
			const PokerAiOdds::Seat& s = table.seats[static_cast<std::size_t>(i)];
			occupied[i] = s.occupied ? 1 : 0;
			state[i] = s.state;
			stack[i] = s.stack;
			streetBet[i] = s.streetBet;
			canRaise[i] = s.canRaise ? 1 : 0;
		}
		line.Add("seat", static_cast<std::int64_t>(seat))
			.Add("tier", static_cast<std::int64_t>(table.stakesTier))
			.Add("dealer", static_cast<std::int64_t>(table.dealer))
			.Add("bigBlind", static_cast<std::int64_t>(table.bigBlind))
			.Add("callLevel", static_cast<std::int64_t>(table.callLevel))
			.Add("lastRaise", static_cast<std::int64_t>(table.lastRaise))
			.Add("openBet", static_cast<std::int64_t>(table.openBet))
			.Add("pot", static_cast<std::int64_t>(table.pot))
			.Add("occupied", occupied, kSeatCount)
			.Add("state", state, kSeatCount)
			.Add("stack", stack, kSeatCount)
			.Add("streetBet", streetBet, kSeatCount)
			.Add("canRaise", canRaise, kSeatCount)
			.Add("equity", equity)
			.Add("multiplier", profile.equityMultiplier)
			.Add("sizes", profile.sizes.data(), static_cast<int>(profile.sizes.size()));
	}

	// Inverse of WriteAi().
	inline bool ReadAi(std::string_view line, PokerAiOdds::Table& table, int& seat, float& equity, PokerAiOdds::Profile& profile)
	{
		using PokerAiOdds::kSeatCount;
		table = PokerAiOdds::Table{};
		profile = PokerAiOdds::Profile{};
		std::int32_t seatValue = -1;
		std::vector<std::int32_t> occupied, state, stack, streetBet, canRaise;
		std::vector<float> sizes;
		if (!GetInt(line, "seat", seatValue) || seatValue < 0 || seatValue >= kSeatCount ||
			!GetInt(line, "tier", table.stakesTier) || !GetInt(line, "dealer", table.dealer) || !GetInt(line, "bigBlind", table.bigBlind) ||
			!GetInt(line, "callLevel", table.callLevel) || !GetInt(line, "lastRaise", table.lastRaise) ||
			!GetInt(line, "openBet", table.openBet) || !GetInt(line, "pot", table.pot) ||
			!GetIntArray(line, "occupied", occupied) || !GetIntArray(line, "state", state) || !GetIntArray(line, "stack", stack) ||
			!GetIntArray(line, "streetBet", streetBet) || !GetIntArray(line, "canRaise", canRaise) ||
			!GetFloat(line, "equity", equity) || !GetFloat(line, "multiplier", profile.equityMultiplier) ||
			!GetFloatArray(line, "sizes", sizes) || sizes.size() != profile.sizes.size())
			return false;
		for (const std::vector<std::int32_t>* array : { &occupied, &state, &stack, &streetBet, &canRaise })
			if (array->size() != kSeatCount)
				return false;

		seat = seatValue;
		for (std::size_t i = 0; i < kSeatCount; i++)
		{
			PokerAiOdds::Seat& s = table.seats[i];
			s.occupied = occupied[i] != 0;
			s.state = state[i];
			s.stack = stack[i];
			s.streetBet = streetBet[i];
			s.canRaise = canRaise[i] != 0;
		}
		for (std::size_t i = 0; i < sizes.size(); i++)
			profile.sizes[i] = sizes[i];
		return true;
	}
}
