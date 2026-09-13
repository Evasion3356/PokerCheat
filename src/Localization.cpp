#include "Localization.h"
#include "Config.h"
#include "Log.h"
#include "script.h" // LANGUAGE::_GET_CURRENT_LANGUAGE_ID() (natives.h, via script.h)

#include <string>

namespace
{
	constexpr int kLanguageCount = static_cast<int>(Localization::Language::Count);

	// Maps a seat's personality index (kPersonalityIndexBase[seat], 0-14)
	// to a human-readable style label. Traced from poker_sp.ysc.c's
	// func_584 -- 15 func_1191(table, index, p1, p2, styleCode) calls
	// (lines 25441-25455): styleCode is 0 for indices 0-8 (all of them
	// route to func_1628, the real equity-driven decision engine, see
	// docs/JOURNAL.md Session 14) and 1-6 for indices 9-14 (the
	// card-blind archetypes -- calling station/all-in-shover/
	// unconditional-all-in/push-fold/pot-cap-gate). For indices 0-8, p1
	// selects a "how much do I trust my equity read" multiplier from f_9
	// ({1.25, 1.0, 0.8} for p1={0,1,2} -- confirmed via func_584 lines
	// 25456-25458, so p1=0 is Loose, p1=2 is Tight) and p2 selects a
	// bet-size-range row from f_13 (three 10-float rows, lines
	// 25459-25488, each row consistently larger than the last -- so p2=0
	// is Passive, p2=2 is Aggressive). The only indices func_185 (the
	// actual seat-fill assignment, line 8975,
	// `GET_RANDOM_INT_IN_RANGE(5, 8+1)`) ever hands to a real seat are
	// 5-8 -- the four corner combinations of that grid -- so those are
	// the only labels that should ever actually appear at a normal
	// table; the rest are filled in for completeness/robustness in case
	// this index ever reads something else.
	//
	// One row per Language (see Localization.h's enum, same ordering),
	// 15 columns matching that same 0-14 index order plus the trailing
	// unused-in-practice entries. Translations beyond row 0 (English)
	// are LLM-assisted, not yet reviewed by a native speaker per
	// language -- several poker terms (Calling Station, All-In Shover,
	// Push/Fold) are kept as the English loanword in languages where
	// that's genuinely how poker communities use them; fix a row
	// directly here if a wording turns out to be wrong.
	constexpr int kPersonalityLabelCount = 15;
	const char* const kPersonalityLabels[kLanguageCount][kPersonalityLabelCount] =
	{
		// English (en-US)
		{ "Neutral", "Tight", "Loose", "Aggressive", "Passive", "Loose-Passive", "Tight-Passive", "Loose-Aggressive", "Tight-Aggressive", "Calling Station", "All-In Shover", "Always All-In", "Calling Station", "Push/Fold", "Pot-Cap Gate" },
		// French (fr-FR)
		{ "Neutre", "Serré", "Lâche", "Agressif", "Passif", "Lâche-Passif", "Serré-Passif", "Lâche-Agressif", "Serré-Agressif", "Calling Station", "Shover All-in", "Toujours All-in", "Calling Station", "Push/Fold", "Plafond de mise" },
		// German (de-DE)
		{ "Neutral", "Tight", "Loose", "Aggressiv", "Passiv", "Loose-Passiv", "Tight-Passiv", "Loose-Aggressiv", "Tight-Aggressiv", "Calling Station", "All-In-Schieber", "Immer All-In", "Calling Station", "Push/Fold", "Pot-Cap-Grenze" },
		// Italian (it-IT)
		{ "Neutrale", "Tight", "Loose", "Aggressivo", "Passivo", "Loose-Passivo", "Tight-Passivo", "Loose-Aggressivo", "Tight-Aggressivo", "Calling Station", "All-In Shover", "Sempre All-In", "Calling Station", "Push/Fold", "Limite del Piatto" },
		// Spanish (es-ES)
		{ "Neutral", "Tight", "Loose", "Agresivo", "Pasivo", "Loose-Pasivo", "Tight-Pasivo", "Loose-Agresivo", "Tight-Agresivo", "Calling Station", "All-In Shover", "Siempre All-In", "Calling Station", "Push/Fold", "Límite de Bote" },
		// Portuguese, Brazilian (pt-BR)
		{ "Neutro", "Tight", "Loose", "Agressivo", "Passivo", "Loose-Passivo", "Tight-Passivo", "Loose-Agressivo", "Tight-Agressivo", "Calling Station", "All-In Shover", "Sempre All-In", "Calling Station", "Push/Fold", "Limite de Pote" },
		// Polish (pl-PL)
		{ "Neutralny", "Tight", "Loose", "Agresywny", "Pasywny", "Loose-Pasywny", "Tight-Pasywny", "Loose-Agresywny", "Tight-Agresywny", "Calling Station", "Gracz All-In", "Zawsze All-In", "Calling Station", "Push/Fold", "Limit Puli" },
		// Russian (ru-RU)
		{ "Нейтральный", "Тайтовый", "Лузовый", "Агрессивный", "Пассивный", "Лузово-пассивный", "Тайтово-пассивный", "Лузово-агрессивный", "Тайтово-агрессивный", "Коллинг-стейшн", "Олл-ин шовер", "Всегда олл-ин", "Коллинг-стейшн", "Пуш/Фолд", "Лимит банка" },
		// Korean (ko-KR)
		{ "중립", "타이트", "루즈", "어그레시브", "패시브", "루즈-패시브", "타이트-패시브", "루즈-어그레시브", "타이트-어그레시브", "콜링 스테이션", "올인 슈버", "항상 올인", "콜링 스테이션", "푸시/폴드", "팟 캡 게이트" },
		// Chinese, Traditional (zh-TW)
		{ "中性", "緊", "鬆", "激進", "被動", "鬆被動", "緊被動", "鬆激進", "緊激進", "跟注站", "全下推手", "永遠全下", "跟注站", "推/棄", "底池上限" },
		// Japanese (ja-JP)
		{ "ニュートラル", "タイト", "ルース", "アグレッシブ", "パッシブ", "ルース・パッシブ", "タイト・パッシブ", "ルース・アグレッシブ", "タイト・アグレッシブ", "コーリングステーション", "オールインシューバー", "常にオールイン", "コーリングステーション", "プッシュ/フォールド", "ポットキャップゲート" },
		// Spanish, Mexican (es-MX) -- same poker vocabulary as es-ES,
		// no meaningful regional difference for these terms
		{ "Neutral", "Tight", "Loose", "Agresivo", "Pasivo", "Loose-Pasivo", "Tight-Pasivo", "Loose-Agresivo", "Tight-Agresivo", "Calling Station", "All-In Shover", "Siempre All-In", "Calling Station", "Push/Fold", "Límite de Bote" },
		// Chinese, Simplified (zh-CN)
		{ "中性", "紧", "松", "激进", "被动", "松被动", "紧被动", "松激进", "紧激进", "跟注站", "全下推手", "永远全下", "跟注站", "推/弃", "底池上限" },
	};

	// vsResult: 0 = you win, 1 = they win, 2 = tie (see VerdictLabel()'s
	// column mapping below) -- shared between DrawSeatCardIcons()'s
	// per-opponent tag and DrawWinPredictionStatus()'s standalone
	// readout in PokerCheat.cpp.
	constexpr int kVerdictLabelCount = 3;
	const char* const kVerdictLabels[kLanguageCount][kVerdictLabelCount] =
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

	const char* VerdictLabel(Language lang, int vsResult)
	{
		int col = (vsResult > 0) ? 0 : (vsResult < 0) ? 1 : 2;
		return kVerdictLabels[static_cast<int>(lang)][col];
	}

	const char* VerdictLabel(int vsResult)
	{
		return VerdictLabel(Current(), vsResult);
	}

	const char* PersonalityLabel(Language lang, std::int32_t personalityIndex)
	{
		if (personalityIndex < 0 || personalityIndex >= kPersonalityLabelCount)
			return "";

		return kPersonalityLabels[static_cast<int>(lang)][personalityIndex];
	}

	const char* PersonalityLabel(std::int32_t personalityIndex)
	{
		return PersonalityLabel(Current(), personalityIndex);
	}

	const char* LanguageCode(Language lang)
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
