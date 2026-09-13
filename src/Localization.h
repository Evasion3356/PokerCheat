/*
	Localizes the handful of strings this mod actually draws on screen in
	a Release build: the (You Win)/(They Win)/(Tie) verdict wording (see
	PokerCheat.cpp's DrawSeatCardIcons()/DrawWinPredictionStatus()) and
	the opponent personality/style tag ("Tight-Aggressive" etc., see
	PersonalityLabel()'s original header comment, now moved to
	Localization.cpp above kPersonalityLabels). Everything else this file
	draws (HandCategoryName(), the seat/board debug text panel) is
	#ifdef _DEBUG-only -- a dev diagnostic surface, never shown to an end
	user -- and stays English-only; not worth translating.

	Language is auto-detected from the game's own current UI language via
	LANGUAGE::_GET_CURRENT_LANGUAGE_ID() (see Localization.cpp), so a
	player sees this mod's HUD in whatever language they already have
	RDR2 itself set to, with no config needed. PokerCheat.ini's [General]
	Language key can override that per Config.h's header comment on
	Config::Values::Language, for anyone who wants the HUD in a different
	language than their game UI.

	Translations beyond English are LLM-assisted, not yet reviewed by a
	native speaker per language -- if a wording is wrong for a given
	language, fix the corresponding row in Localization.cpp's
	kPersonalityLabels/kVerdictLabels tables directly, no other file
	needs to change.
*/

#pragma once

#include <cstdint>

namespace Localization
{
	// Matches LANGUAGE::_GET_CURRENT_LANGUAGE_ID()'s own return value
	// mapping exactly (confirmed against rdr3-nativedb-data/natives.json's
	// comment on native hash 0xDB917DA5C6835FCC) -- these are the 13
	// languages RDR2 itself ships with, not an arbitrary list.
	enum class Language : std::int32_t
	{
		English = 0,             // en-US
		French = 1,               // fr-FR
		German = 2,               // de-DE
		Italian = 3,              // it-IT
		Spanish = 4,              // es-ES
		PortugueseBrazilian = 5,  // pt-BR
		Polish = 6,               // pl-PL
		Russian = 7,              // ru-RU
		Korean = 8,               // ko-KR
		ChineseTraditional = 9,   // zh-TW
		Japanese = 10,            // ja-JP
		SpanishMexican = 11,      // es-MX
		ChineseSimplified = 12,   // zh-CN

		Count = 13
	};

	// Re-resolves the active language from PokerCheat.ini's [General]
	// Language override (Config::Get().Language) if set to anything
	// other than "auto", else from the game's own current UI language.
	// MUST be called from within ScriptHookRDR2's script fiber (i.e.
	// from OnTick() or a menu action running inside ScriptMain's loop),
	// never from DllMain -- unlike Config::Reload(), this calls a real
	// game native (LANGUAGE::_GET_CURRENT_LANGUAGE_ID()) and natives
	// aren't safe to invoke outside the registered script thread's own
	// cooperative fiber. Current() below lazily calls this on first use
	// instead, the same "g_loaded" pattern Config::Get() already uses,
	// so nothing needs to call this explicitly except the Debug F10
	// menu's "Reload Config" item (to re-pick the language immediately
	// after an ini edit, without waiting for the next natural call) --
	// see script.cpp.
	void Refresh();

	// Returns the cached language, resolving it via Refresh() on first
	// call if nothing has resolved it yet.
	Language Current();

	// (You Win)/(They Win)/(Tie) verdict wording, from the player's own
	// perspective -- vsResult: >0 you win, <0 they win, 0 tie. Shared by
	// DrawSeatCardIcons()'s per-opponent tag and
	// DrawWinPredictionStatus()'s standalone readout in PokerCheat.cpp,
	// which previously each had their own identical hardcoded English
	// copy of this wording.
	const char* VerdictLabel(int vsResult);

	// Opponent personality/style label -- see PokerCheat.cpp's
	// kPersonalityIndexBase header comment for what personalityIndex
	// (0-14) means and how it's read. Returns "" for any index outside
	// that range, same as the original PersonalityLabel()'s default
	// case (suppresses the personality half of the on-screen tag).
	const char* PersonalityLabel(std::int32_t personalityIndex);
}
