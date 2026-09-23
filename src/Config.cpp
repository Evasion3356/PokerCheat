#include "Config.h"
#include "Log.h"
#include "LogFallback.h"

#include "..\external\inipp\inipp\inipp.h"

#include <windows.h>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <string>
#include <exception>

namespace
{
	using Section = inipp::Ini<char>::Section;

	Config::Values g_values;
	bool g_loaded = false; // true once a load has actually finished and published g_values

	// Where PokerCheat.ini is loaded from and saved to: next to the .asi, or
	// %LOCALAPPDATA%\RDR2ASIMods\PokerCheat.ini when the game folder isn't
	// writable -- starting from the game folder's copy if there is one (see
	// LogFallback::ResolveSettings). Resolved once per session.
	const LogFallback::SettingsPaths& IniPaths()
	{
		static const LogFallback::SettingsPaths paths = LogFallback::ResolveSettings(
			LogFallback::ModuleDirectory(), L"PokerCheat.ini", LogFallback::FallbackDirectory());
		return paths;
	}

	// Log::Write's format strings are narrow (fmt/spdlog, not wide) --
	// this narrows the INI's std::wstring path for the log lines that
	// mention it, via the real Win32 conversion API rather than a
	// naive per-character truncation (which would mangle any non-ASCII
	// byte in the game's install path).
	std::string NarrowPath(const std::wstring& wide)
	{
		if (wide.empty())
			return {};

		int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (size <= 0)
			return {};

		std::string narrow(static_cast<std::size_t>(size - 1), '\0'); // size includes the null terminator
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, narrow.data(), size, nullptr, nullptr);
		return narrow;
	}

	// Generic "read or default" on top of inipp::get_value(): that
	// function only writes into its out-param on success (key present
	// and parses as T), and leaves it untouched otherwise -- so seeding
	// the out-param with the default and ignoring the bool return is
	// exactly the desired fallback behavior, for any T extract<T>
	// supports (float, bool via std::boolalpha, std::string, ...).
	template <typename T>
	T GetOr(const Section& sec, const char* key, T def)
	{
		inipp::get_value(sec, key, def);
		return def;
	}

	void SetFloat(Section& sec, const char* key, float value)
	{
		// std::ostringstream's default (defaultfloat) formatting matches
		// printf's "%g" closely enough for an INI value round-trip --
		// shortest representation, 6 significant digits by default --
		// with no fixed-size buffer to size wrong.
		std::ostringstream oss;
		oss << value;
		sec[key] = oss.str();
	}

	void SetBool(Section& sec, const char* key, bool value)
	{
		// Matches inipp's own extract<bool>, which parses with
		// std::boolalpha -- i.e. it accepts "true"/"false" text, not
		// "1"/"0".
		sec[key] = value ? "true" : "false";
	}
}

namespace
{
	// Actual work, split out from Config::Reload() below so it can run on
	// a dedicated worker thread (see Reload()) instead of directly on
	// whatever thread/fiber called in.
	void ReloadImpl()
	{
		inipp::Ini<char> ini;
		{
			std::ifstream is(IniPaths().read); // MSVC extension: ifstream accepts a wide filename directly
			if (is)
				ini.parse(is);
			// fine if the file doesn't exist yet (is fails to open) --
			// ini.sections just stays empty, every value below falls
			// back to its compiled-in default.
		}

		Config::Values defaults;
		auto& general = ini.sections["General"];

		g_values.ShowCommunityCards = GetOr(general, "ShowCommunityCards", defaults.ShowCommunityCards);
		g_values.ShowOthersCards = GetOr(general, "ShowOthersCards", defaults.ShowOthersCards);
		g_values.ShowWinPrediction = GetOr(general, "ShowWinPrediction", defaults.ShowWinPrediction);
		g_values.ShowWouldWinHandAgainst = GetOr(general, "ShowWouldWinHandAgainst", defaults.ShowWouldWinHandAgainst);
		g_values.ShowOpponentPersonality = GetOr(general, "ShowOpponentPersonality", defaults.ShowOpponentPersonality);
		g_values.Language = GetOr(general, "Language", defaults.Language);

#ifdef _DEBUG
		// Debug-only -- see Config.h's header comment on PanelX etc.
		// Release never reads or writes this section at all, so a
		// Release-built PokerCheat.ini simply won't have a [HUD] section.
		auto& hud = ini.sections["HUD"];
		g_values.PanelX = GetOr(hud, "PanelX", defaults.PanelX);
		g_values.PanelY = GetOr(hud, "PanelY", defaults.PanelY);
		g_values.TextScale = GetOr(hud, "TextScale", defaults.TextScale);
		g_values.TitleTextScale = GetOr(hud, "TitleTextScale", defaults.TitleTextScale);
		g_values.WinPredictionX = GetOr(hud, "WinPredictionX", defaults.WinPredictionX);
		g_values.WinPredictionY = GetOr(hud, "WinPredictionY", defaults.WinPredictionY);
		SetFloat(hud, "PanelX", g_values.PanelX);
		SetFloat(hud, "PanelY", g_values.PanelY);
		SetFloat(hud, "TextScale", g_values.TextScale);
		SetFloat(hud, "TitleTextScale", g_values.TitleTextScale);
		SetFloat(hud, "WinPredictionX", g_values.WinPredictionX);
		SetFloat(hud, "WinPredictionY", g_values.WinPredictionY);
#endif

		// Write the resolved values (file's own, or the default that was
		// just substituted for anything missing) back into the in-memory
		// structure, then generate() the whole file fresh in one write
		// below. Unlike mINI, inipp has no "lazy" partial-file-update
		// mode that preserves untouched formatting -- generate() always
		// writes the full structure -- which is fine here since we don't
		// rely on preserving any custom comments/formatting, and
		// `sections`/`ini.parse()` already carried forward whatever the
		// user had actually set.
		SetBool(general, "ShowCommunityCards", g_values.ShowCommunityCards);
		SetBool(general, "ShowOthersCards", g_values.ShowOthersCards);
		SetBool(general, "ShowWinPrediction", g_values.ShowWinPrediction);
		SetBool(general, "ShowWouldWinHandAgainst", g_values.ShowWouldWinHandAgainst);
		SetBool(general, "ShowOpponentPersonality", g_values.ShowOpponentPersonality);
		general["Language"] = g_values.Language;

#ifdef _DEBUG
		// Debug-only -- see Config.h's header comment on
		// Card2DIconBaseX etc. Release never reads or writes this
		// section at all, so a Release-built PokerCheat.ini simply won't
		// have a [CommunityCardIcons2D] section.
		auto& icons = ini.sections["CommunityCardIcons2D"];
		g_values.Card2DIconBaseX = GetOr(icons, "BaseX", defaults.Card2DIconBaseX);
		g_values.Card2DIconY = GetOr(icons, "Y", defaults.Card2DIconY);
		g_values.Card2DIconSpacingX = GetOr(icons, "SpacingX", defaults.Card2DIconSpacingX);
		g_values.Card2DIconWidth = GetOr(icons, "Width", defaults.Card2DIconWidth);
		g_values.Card2DIconHeight = GetOr(icons, "Height", defaults.Card2DIconHeight);

		SetFloat(icons, "BaseX", g_values.Card2DIconBaseX);
		SetFloat(icons, "Y", g_values.Card2DIconY);
		SetFloat(icons, "SpacingX", g_values.Card2DIconSpacingX);
		SetFloat(icons, "Width", g_values.Card2DIconWidth);
		SetFloat(icons, "Height", g_values.Card2DIconHeight);

		// Debug-only -- see Config.h's header comment on SeatCardIconBaseX
		// etc. A base position (dense row 1 -- the first opponent row
		// above you) plus a per-row Y step, not a per-row lookup table.
		auto& seatIcons = ini.sections["SeatCardIcons2D"];
		g_values.SeatCardIconBaseX = GetOr(seatIcons, "BaseX", defaults.SeatCardIconBaseX);
		g_values.SeatCardIconBaseY = GetOr(seatIcons, "BaseY", defaults.SeatCardIconBaseY);
		g_values.SeatCardIconStepY = GetOr(seatIcons, "StepY", defaults.SeatCardIconStepY);
		g_values.SeatCardIconSpacingX = GetOr(seatIcons, "SpacingX", defaults.SeatCardIconSpacingX);
		g_values.SeatCardIconWidth = GetOr(seatIcons, "Width", defaults.SeatCardIconWidth);
		g_values.SeatCardIconHeight = GetOr(seatIcons, "Height", defaults.SeatCardIconHeight);
		g_values.SeatCardIconLabelOffsetX = GetOr(seatIcons, "LabelOffsetX", defaults.SeatCardIconLabelOffsetX);
		g_values.SeatCardIconLabelOffsetY = GetOr(seatIcons, "LabelOffsetY", defaults.SeatCardIconLabelOffsetY);
		SetFloat(seatIcons, "BaseX", g_values.SeatCardIconBaseX);
		SetFloat(seatIcons, "BaseY", g_values.SeatCardIconBaseY);
		SetFloat(seatIcons, "StepY", g_values.SeatCardIconStepY);
		SetFloat(seatIcons, "SpacingX", g_values.SeatCardIconSpacingX);
		SetFloat(seatIcons, "Width", g_values.SeatCardIconWidth);
		SetFloat(seatIcons, "Height", g_values.SeatCardIconHeight);
		SetFloat(seatIcons, "LabelOffsetX", g_values.SeatCardIconLabelOffsetX);
		SetFloat(seatIcons, "LabelOffsetY", g_values.SeatCardIconLabelOffsetY);
#endif

		if (IniPaths().usedFallback)
			Log::Write("Config::Reload -- the game folder isn't writable, so settings are saved to {}",
				LogFallback::ToUtf8(IniPaths().write));

		{
			std::ofstream os(IniPaths().write, std::ios::trunc);
			if (os)
				ini.generate(os);
			else
				Log::Write("Config::Reload -- failed to open {} for writing", NarrowPath(IniPaths().write));
		}

		Log::Write("Config::Reload -- loaded from {} (ShowCommunityCards={} ShowOthersCards={} ShowWinPrediction={})",
			NarrowPath(IniPaths().read), g_values.ShowCommunityCards, g_values.ShowOthersCards, g_values.ShowWinPrediction);
	}
}

namespace Config
{
	void Reload()
	{
		// Plain synchronous call -- no worker thread. The worker thread
		// (see docs/JOURNAL.md) existed specifically to get <filesystem>'s
		// stack-heavy locale/codecvt machinery off ScriptHookRDR2's small
		// fiber stack; inipp doesn't touch <filesystem> at all (plain
		// std::getline/std::map/std::basic_istringstream, all shallow,
		// unremarkable stack usage), so that whole concern no longer
		// applies. Threading also meant real complexity that's no longer
		// earning its keep: a worker-thread stack size to pick, a mutex
		// to serialize overlapping reloads, atomics with explicit
		// acquire/release ordering to publish results safely across
		// threads. None of that is needed once this runs to completion
		// on the calling thread/fiber before returning, same as any
		// ordinary function call.
		try
		{
			ReloadImpl();
		}
		catch (const std::exception& e)
		{
			Log::Write("Config::Reload -- std::exception: {} -- keeping previous config values", e.what());
		}
		catch (...)
		{
			Log::Write("Config::Reload -- unknown non-std exception -- keeping previous config values");
		}

		g_loaded = true;
	}

	const Values& Get()
	{
		if (!g_loaded)
			Reload();

		return g_values;
	}
}
