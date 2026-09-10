#include "Config.h"
#include "Log.h"

#include "..\external\mINI\src\mini\ini.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>

namespace
{
	Config::Values g_values;
	bool g_loaded = false;
	char g_iniPath[MAX_PATH] = {};

	// Resolves PokerCheat.ini next to this DLL's own .asi, from the
	// DLL's own module handle rather than trusting the process's CWD to
	// match the game folder (same reasoning as before mINI: don't
	// assume, just ask the loader directly).
	const char* ResolveIniPath()
	{
		if (g_iniPath[0])
			return g_iniPath;

		HMODULE hModule = nullptr;
		GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&ResolveIniPath),
			&hModule);

		char modulePath[MAX_PATH] = {};
		GetModuleFileNameA(hModule, modulePath, MAX_PATH);

		char drive[_MAX_DRIVE], dir[_MAX_DIR];
		_splitpath_s(modulePath, drive, _MAX_DRIVE, dir, _MAX_DIR, nullptr, 0, nullptr, 0);
		sprintf_s(g_iniPath, "%s%sPokerCheat.ini", drive, dir);

		return g_iniPath;
	}

	float ParseFloatOr(const std::string& value, float def)
	{
		if (value.empty())
			return def;

		return static_cast<float>(atof(value.c_str()));
	}

	void SetFloat(mINI::INIMap<std::string>& section, const char* key, float value)
	{
		char buf[64];
		sprintf_s(buf, "%g", value);
		section[key] = buf;
	}

	bool ParseBoolOr(std::string value, bool def)
	{
		if (value.empty())
			return def;

		std::transform(value.begin(), value.end(), value.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		return value == "1" || value == "true" || value == "yes" || value == "on";
	}

	void SetBool(mINI::INIMap<std::string>& section, const char* key, bool value)
	{
		section[key] = value ? "true" : "false";
	}
}

namespace Config
{
	void Reload()
	{
		mINI::INIFile file(ResolveIniPath());
		mINI::INIStructure ini;
		file.read(ini); // fine if the file doesn't exist yet -- ini just stays empty

		Values defaults;
		auto& general = ini["General"];
		auto& hud = ini["HUD"];

		g_values.ShowCommunityCards = ParseBoolOr(general["ShowCommunityCards"], defaults.ShowCommunityCards);
		g_values.ShowOthersCards = ParseBoolOr(general["ShowOthersCards"], defaults.ShowOthersCards);
		g_values.ShowWinPrediction = ParseBoolOr(general["ShowWinPrediction"], defaults.ShowWinPrediction);

		g_values.PanelX = ParseFloatOr(hud["PanelX"], defaults.PanelX);
		g_values.PanelY = ParseFloatOr(hud["PanelY"], defaults.PanelY);
		g_values.TextScale = ParseFloatOr(hud["TextScale"], defaults.TextScale);
		g_values.TitleTextScale = ParseFloatOr(hud["TitleTextScale"], defaults.TitleTextScale);

		// Write the resolved values (file's own, or the default that was
		// just substituted for anything missing) back in one shot.
		// file.write() does a "lazy" write: preserves existing formatting/
		// comments and only touches keys that are new or changed, and
		// generates a fresh file if none exists yet -- either way, one
		// real file write, not up to 9 like the old per-key Win32 version.
		SetBool(general, "ShowCommunityCards", g_values.ShowCommunityCards);
		SetBool(general, "ShowOthersCards", g_values.ShowOthersCards);
		SetBool(general, "ShowWinPrediction", g_values.ShowWinPrediction);
		SetFloat(hud, "PanelX", g_values.PanelX);
		SetFloat(hud, "PanelY", g_values.PanelY);
		SetFloat(hud, "TextScale", g_values.TextScale);
		SetFloat(hud, "TitleTextScale", g_values.TitleTextScale);

#ifdef _DEBUG
		// Debug-only -- see Config.h's header comment on
		// Card2DIconBaseX etc. Release never reads or writes this
		// section at all, so a Release-built PokerCheat.ini simply won't
		// have a [CommunityCardIcons2D] section.
		auto& icons = ini["CommunityCardIcons2D"];
		g_values.Card2DIconBaseX = ParseFloatOr(icons["BaseX"], defaults.Card2DIconBaseX);
		g_values.Card2DIconY = ParseFloatOr(icons["Y"], defaults.Card2DIconY);
		g_values.Card2DIconSpacingX = ParseFloatOr(icons["SpacingX"], defaults.Card2DIconSpacingX);
		g_values.Card2DIconWidth = ParseFloatOr(icons["Width"], defaults.Card2DIconWidth);
		g_values.Card2DIconHeight = ParseFloatOr(icons["Height"], defaults.Card2DIconHeight);

		SetFloat(icons, "BaseX", g_values.Card2DIconBaseX);
		SetFloat(icons, "Y", g_values.Card2DIconY);
		SetFloat(icons, "SpacingX", g_values.Card2DIconSpacingX);
		SetFloat(icons, "Width", g_values.Card2DIconWidth);
		SetFloat(icons, "Height", g_values.Card2DIconHeight);
#endif

		file.write(ini, true);

		g_loaded = true;
		Log::Write("Config::Reload -- loaded from %s (ShowCommunityCards=%d ShowOthersCards=%d ShowWinPrediction=%d PanelX=%.4f PanelY=%.4f)",
			ResolveIniPath(), g_values.ShowCommunityCards, g_values.ShowOthersCards, g_values.ShowWinPrediction,
			g_values.PanelX, g_values.PanelY);
	}

	const Values& Get()
	{
		if (!g_loaded)
			Reload();

		return g_values;
	}
}
