#include "Config.h"

// The library's side of Config: the live values and their option table. The
// INI loading (Reload) is the ASI's, in Config.cpp.
namespace PokerCheat::Config
{
	Values& Mutable()
	{
		static Values values;
		return values;
	}

	const Values& Get()
	{
		return Mutable();
	}

	std::span<const Option> Options()
	{
		using enum Option::Kind;
		Values& v = Mutable();
		static const Option options[] = {
			{ "showcommunitycards", "HUD", "Show Community Cards", "Shows the board's cards before they're dealt, read from the deck.", Bool, &v.ShowCommunityCards },
			{ "showotherscards", "HUD", "Show Others' Cards", "Shows each opponent's hole cards beside their seat.", Bool, &v.ShowOthersCards },
			{ "showwinprediction", "HUD", "Show Win Prediction", "Shows whether you win, lose or tie the hand as the deck will play out.", Bool, &v.ShowWinPrediction },
			{ "showwouldwinhandagainst", "HUD", "Show Result per Opponent", "Tags each opponent with You Win, They Win or Tie.", Bool, &v.ShowWouldWinHandAgainst },
			{ "showopponentodds", "HUD", "Show Opponent Odds", "Each opponent's odds of folding, checking, calling or raising if they acted now (the game's own AI).", Bool, &v.ShowOpponentOdds },
			{ "bethotkeys", "Controls", "Bet Hotkeys", "Right/Left arrow: bet 5 more or less. Tab: the most the game allows.", Bool, &v.BetHotkeys },
#ifdef _DEBUG
			{ "panelx", "HUD Layout", "Panel X", "Text panel position.", Float, &v.PanelX, 0.0f, 1.0f, 0.005f },
			{ "panely", "HUD Layout", "Panel Y", "Text panel position.", Float, &v.PanelY, 0.0f, 1.0f, 0.005f },
			{ "textscale", "HUD Layout", "Text Scale", "Text panel scale.", Float, &v.TextScale, 0.1f, 1.0f, 0.01f },
			{ "titletextscale", "HUD Layout", "Title Text Scale", "Text panel title scale.", Float, &v.TitleTextScale, 0.1f, 1.0f, 0.01f },
			{ "winpredictionx", "HUD Layout", "Win Prediction X", "Win/lose/tie readout position.", Float, &v.WinPredictionX, 0.0f, 1.0f, 0.005f },
			{ "winpredictiony", "HUD Layout", "Win Prediction Y", "Win/lose/tie readout position.", Float, &v.WinPredictionY, 0.0f, 1.0f, 0.005f },
			{ "card2diconbasex", "HUD Layout", "Board Icons X", "Community card strip position.", Float, &v.Card2DIconBaseX, 0.0f, 1.0f, 0.001f },
			{ "card2dicony", "HUD Layout", "Board Icons Y", "Community card strip position.", Float, &v.Card2DIconY, 0.0f, 1.0f, 0.001f },
			{ "card2diconspacingx", "HUD Layout", "Board Icons Spacing", "Community card spacing.", Float, &v.Card2DIconSpacingX, 0.0f, 0.2f, 0.001f },
			{ "card2diconwidth", "HUD Layout", "Board Icon Width", "Community card size.", Float, &v.Card2DIconWidth, 0.0f, 0.2f, 0.001f },
			{ "card2diconheight", "HUD Layout", "Board Icon Height", "Community card size.", Float, &v.Card2DIconHeight, 0.0f, 0.2f, 0.001f },
			{ "seatcardiconbasex", "HUD Layout", "Seat Icons X", "Opponent card icons, first row.", Float, &v.SeatCardIconBaseX, 0.0f, 1.0f, 0.001f },
			{ "seatcardiconbasey", "HUD Layout", "Seat Icons Y", "Opponent card icons, first row.", Float, &v.SeatCardIconBaseY, 0.0f, 1.0f, 0.001f },
			{ "seatcardiconstepy", "HUD Layout", "Seat Icons Row Step", "Vertical step between seat rows.", Float, &v.SeatCardIconStepY, -0.5f, 0.5f, 0.001f },
			{ "seatcardiconspacingx", "HUD Layout", "Seat Icons Spacing", "Space between a seat's two cards.", Float, &v.SeatCardIconSpacingX, 0.0f, 0.2f, 0.001f },
			{ "seatcardiconwidth", "HUD Layout", "Seat Icon Width", "Opponent card size.", Float, &v.SeatCardIconWidth, 0.0f, 0.2f, 0.001f },
			{ "seatcardiconheight", "HUD Layout", "Seat Icon Height", "Opponent card size.", Float, &v.SeatCardIconHeight, 0.0f, 0.2f, 0.001f },
			{ "seatcardiconlabeloffsetx", "HUD Layout", "Seat Label Offset X", "Result tag offset from the cards.", Float, &v.SeatCardIconLabelOffsetX, -0.2f, 0.2f, 0.001f },
			{ "seatcardiconlabeloffsety", "HUD Layout", "Seat Label Offset Y", "Result tag offset from the cards.", Float, &v.SeatCardIconLabelOffsetY, -0.2f, 0.2f, 0.001f },
#endif
		};
		return options;
	}
}
