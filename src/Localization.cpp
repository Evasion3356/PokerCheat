#include "Localization.h"
#include "Config.h"
#include "Log.h"
#include "script.h" // LANGUAGE::_GET_CURRENT_LANGUAGE_ID() (natives.h, via script.h)

#include <string>
#include <string_view>

namespace
{
	constexpr int kLanguageCount = static_cast<int>(Localization::Language::Count);

	// The action words for PokerCheat.cpp's opponent odds line ("Fold
	// 62%"): the game's own text, from poker_sp's text block (mgpkr.yldb,
	// one per language) -- its action prompts MGPKR_UI_FOLD/_CHECK/_CALL/
	// _BET/_RAISE, minus the " (~1$~)" amount the prompts carry. Baked in
	// rather than fetched at runtime because the HUD line is one
	// LITERAL_STRING with rich-text tags, not a label reference.
	constexpr int kActionLabelCount = static_cast<int>(Localization::PokerAction::Count);
	constexpr std::string_view kActionLabels[kLanguageCount][kActionLabelCount] =
	{
		{ "Fold", "Check", "Call", "Bet", "Raise" },                                              // en-US
		{ "Se coucher", "Checker", "Suivre", "Miser", "Relancer" },                               // fr-FR
		{ "Passen", "Schieben", "Mitgehen", "Setzen", "Erhöhen" },                               // de-DE
		{ "Lascia", "Passa", "Vedi", "Punta", "Rilancia" },                                       // it-IT
		{ "Retirarse", "Pasar", "Ver apuesta", "Apostar", "Subir apuesta" },                      // es-ES
		{ "Desistir", "Passar", "Pagar", "Apostar", "Aumentar" },                                  // pt-BR
		{ "Spasuj", "Czekaj", "Sprawdź", "Postaw", "Podbij" },                                 // pl-PL
		{ "Пас", "Чек", "Ответить на ставку", "Сделать ставку", "Поднять ставку" }, // ru-RU
		{ "폴드", "체크", "콜", "베팅", "레이즈" },                                              // ko-KR
		{ "蓋牌", "過牌", "跟注", "下注", "加注" },                                         // zh-TW
		{ "フォールド", "チェック", "コール", "ベット", "レイズ" },                           // ja-JP
		{ "Retirarse", "Pasar", "Igualar", "Apostar", "Subir" },                                   // es-MX
		{ "弃牌", "过牌", "跟注", "下注", "加注" },                                         // zh-CN
	};

	// vsResult: 0 = you win, 1 = they win, 2 = tie (see VerdictLabel()'s
	// column mapping below) -- shared between DrawSeatCardIcons()'s
	// per-opponent tag and DrawWinPredictionStatus()'s standalone
	// readout in PokerCheat.cpp.
	constexpr int kVerdictLabelCount = 3;
	constexpr std::string_view kVerdictLabels[kLanguageCount][kVerdictLabelCount] =
	{
		{ "(You Win)", "(They Win)", "(Tie)" },                    // en-US
		{ "(Vous gagnez)", "(Ils gagnent)", "(Égalité)" },          // fr-FR
		{ "(Du gewinnst)", "(Sie gewinnen)", "(Unentschieden)" },   // de-DE
		{ "(Vinci Tu)", "(Vincono Loro)", "(Pareggio)" },           // it-IT
		{ "(Ganas Tú)", "(Ganan Ellos)", "(Empate)" },              // es-ES
		{ "(Você Vence)", "(Eles Vencem)", "(Empate)" },            // pt-BR
		{ "(Wygrywasz)", "(Oni Wygrywają)", "(Remis)" },            // pl-PL
		{ "(Вы выигрываете)", "(Они выигрывают)", "(Ничья)" },      // ru-RU
		{ "(당신 승리)", "(상대 승리)", "(무승부)" },                // ko-KR
		{ "(你贏)", "(對手贏)", "(平手)" },                          // zh-TW
		{ "(あなたの勝ち)", "(相手の勝ち)", "(引き分け)" },          // ja-JP
		{ "(Ganas Tú)", "(Ganan Ellos)", "(Empate)" },              // es-MX
		{ "(你赢)", "(对手赢)", "(平局)" },                          // zh-CN
	};

	Localization::Language g_current = Localization::Language::English;
	bool g_resolved = false; // true once Refresh() has actually run at least once

	Localization::Language ClampLanguage(std::int32_t raw)
	{
		if (raw < 0 || raw >= kLanguageCount)
			return Localization::Language::English;
		return static_cast<Localization::Language>(raw);
	}

	// PokerCheat.ini's [General] Language override -- "auto" (the
	// default) defers to the game's own current UI language; anything
	// else must match one of these exact codes, the same ones
	// LANGUAGE::_GET_CURRENT_LANGUAGE_ID()'s own return-value mapping
	// uses (confirmed against rdr3-nativedb-data/natives.json's comment
	// on native hash 0xDB917DA5C6835FCC). Unrecognized text (a typo, or
	// "auto" itself) falls back to English via Refresh()'s caller.
	bool TryParseOverride(const std::string& code, Localization::Language& out)
	{
		if (code == "en-US") { out = Localization::Language::English; return true; }
		if (code == "fr-FR") { out = Localization::Language::French; return true; }
		if (code == "de-DE") { out = Localization::Language::German; return true; }
		if (code == "it-IT") { out = Localization::Language::Italian; return true; }
		if (code == "es-ES") { out = Localization::Language::Spanish; return true; }
		if (code == "pt-BR") { out = Localization::Language::PortugueseBrazilian; return true; }
		if (code == "pl-PL") { out = Localization::Language::Polish; return true; }
		if (code == "ru-RU") { out = Localization::Language::Russian; return true; }
		if (code == "ko-KR") { out = Localization::Language::Korean; return true; }
		if (code == "zh-TW") { out = Localization::Language::ChineseTraditional; return true; }
		if (code == "ja-JP") { out = Localization::Language::Japanese; return true; }
		if (code == "es-MX") { out = Localization::Language::SpanishMexican; return true; }
		if (code == "zh-CN") { out = Localization::Language::ChineseSimplified; return true; }
		return false;
	}
}

namespace Localization
{
	void Refresh()
	{
		const std::string& languageOverride = Config::Get().Language;

		Language resolved;
		if (TryParseOverride(languageOverride, resolved))
		{
			g_current = resolved;
		}
		else
		{
			// Covers "auto" (the documented default) and any typo'd
			// override alike -- both should fall back to the game's own
			// current language rather than silently forcing English.
			std::int32_t raw = LANGUAGE::_GET_CURRENT_LANGUAGE_ID();
			g_current = ClampLanguage(raw);
		}

		g_resolved = true;
		Log::Write("Localization::Refresh -> language index {} (ini override='{}')", static_cast<int>(g_current), languageOverride);
	}

	Language Current()
	{
		if (!g_resolved)
			Refresh();

		return g_current;
	}

	std::string_view VerdictLabel(Language lang, int vsResult)
	{
		int col = (vsResult > 0) ? 0 : (vsResult < 0) ? 1 : 2;
		return kVerdictLabels[static_cast<int>(lang)][col];
	}

	std::string_view VerdictLabel(int vsResult)
	{
		return VerdictLabel(Current(), vsResult);
	}

	std::string_view ActionLabel(Language lang, PokerAction action)
	{
		return kActionLabels[static_cast<int>(lang)][static_cast<int>(action)];
	}

	std::string_view ActionLabel(PokerAction action)
	{
		return ActionLabel(Current(), action);
	}

	std::string_view LanguageCode(Language lang)
	{
		switch (lang)
		{
			case Language::English: return "en-US";
			case Language::French: return "fr-FR";
			case Language::German: return "de-DE";
			case Language::Italian: return "it-IT";
			case Language::Spanish: return "es-ES";
			case Language::PortugueseBrazilian: return "pt-BR";
			case Language::Polish: return "pl-PL";
			case Language::Russian: return "ru-RU";
			case Language::Korean: return "ko-KR";
			case Language::ChineseTraditional: return "zh-TW";
			case Language::Japanese: return "ja-JP";
			case Language::SpanishMexican: return "es-MX";
			case Language::ChineseSimplified: return "zh-CN";
			default: return "?";
		}
	}
}
