#include "Config.h"
#include "Log.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
	Config::Values g_values;
	bool g_loaded = false;
	char g_iniPath[MAX_PATH] = {};

	// Resolves PokerCheat.ini next to this DLL's own .asi -- NOT a
	// relative "PokerCheat.ini" the way Log.h uses a relative
	// "PokerCheat.log" (which relies on fopen resolving against the
	// process's current directory). GetPrivateProfileString/
	// WritePrivateProfileString have a documented gotcha: a relative
	// filename is resolved against the Windows directory, not the
	// process's CWD -- so this builds an absolute path from the DLL's
	// own module handle instead of trusting CWD to match the game
	// folder.
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

	// Only writes back a single key when it's individually absent from an
	// otherwise-existing file (e.g. a key added in a future update) --
	// NOT the common first-run case, which WriteDefaultIniIfMissing()
	// below now handles in one shot before this is ever called.
	// GetPrivateProfileString's default-string argument can't
	// distinguish "key absent" from "key present and equal to the
	// default", so this passes an empty sentinel default instead and
	// checks the returned length to tell the two apart.
	float GetFloat(const char* section, const char* key, float def)
	{
		char buf[64];
		DWORD len = GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), ResolveIniPath());
		if (len == 0)
		{
			char defStr[64];
			sprintf_s(defStr, "%g", def);
			WritePrivateProfileStringA(section, key, defStr, ResolveIniPath());
			return def;
		}

		return static_cast<float>(atof(buf));
	}

	// If PokerCheat.ini doesn't exist yet, write the entire default file
	// in one plain stdio call -- same fopen/fprintf approach Log.h
	// already uses successfully in this project, deliberately NOT
	// WritePrivateProfileStringA. Two real problems with going through
	// the profile API for this: (1) calling it once per key (9 calls)
	// on a fresh/missing file meant 9 separate synchronous whole-file
	// rewrites -- this is what was still causing the hitch even after
	// the previous fix, since that fix only skipped writes for keys
	// that already existed, and on a truly missing file every key
	// counts as missing; (2) WritePrivateProfileString's writes are
	// cached by Windows and not guaranteed to flush to disk immediately
	// (the documented fix is calling it once more with all-null
	// arguments just to force a flush) -- which is exactly why checking
	// the folder right after toggling found no file at all. A direct
	// fopen/fwrite has neither problem: one real write, visible on disk
	// as soon as it returns.
	void WriteDefaultIniIfMissing()
	{
		const char* path = ResolveIniPath();
		DWORD attrs = GetFileAttributesA(path);
		if (attrs != INVALID_FILE_ATTRIBUTES)
			return; // already exists -- GetFloat's per-key fallback covers any individually-missing key

		Config::Values d; // defaults

		FILE* f = nullptr;
		fopen_s(&f, path, "w");
		if (!f)
		{
			Log::Write("Config: failed to create default %s", path);
			return;
		}

		fprintf(f,
			"[HUD]\n"
			"PanelX=%g\n"
			"PanelY=%g\n"
			"TextScale=%g\n"
			"TitleTextScale=%g\n"
			"\n"
			"[CommunityCardIcons2D]\n"
			"BaseX=%g\n"
			"Y=%g\n"
			"SpacingX=%g\n"
			"Width=%g\n"
			"Height=%g\n",
			d.PanelX, d.PanelY, d.TextScale, d.TitleTextScale,
			d.Card2DIconBaseX, d.Card2DIconY, d.Card2DIconSpacingX, d.Card2DIconWidth, d.Card2DIconHeight);

		fclose(f);

		Log::Write("Config: PokerCheat.ini not found -- wrote defaults in one shot to %s", path);
	}
}

namespace Config
{
	void Reload()
	{
		WriteDefaultIniIfMissing();

		Values defaults;

		g_values.PanelX = GetFloat("HUD", "PanelX", defaults.PanelX);
		g_values.PanelY = GetFloat("HUD", "PanelY", defaults.PanelY);
		g_values.TextScale = GetFloat("HUD", "TextScale", defaults.TextScale);
		g_values.TitleTextScale = GetFloat("HUD", "TitleTextScale", defaults.TitleTextScale);

		g_values.Card2DIconBaseX = GetFloat("CommunityCardIcons2D", "BaseX", defaults.Card2DIconBaseX);
		g_values.Card2DIconY = GetFloat("CommunityCardIcons2D", "Y", defaults.Card2DIconY);
		g_values.Card2DIconSpacingX = GetFloat("CommunityCardIcons2D", "SpacingX", defaults.Card2DIconSpacingX);
		g_values.Card2DIconWidth = GetFloat("CommunityCardIcons2D", "Width", defaults.Card2DIconWidth);
		g_values.Card2DIconHeight = GetFloat("CommunityCardIcons2D", "Height", defaults.Card2DIconHeight);

		g_loaded = true;
		Log::Write("Config::Reload -- loaded from %s (PanelX=%.4f PanelY=%.4f Card2DIconBaseX=%.4f Card2DIconY=%.4f Card2DIconSpacingX=%.4f Card2DIconWidth=%.4f Card2DIconHeight=%.4f)",
			ResolveIniPath(), g_values.PanelX, g_values.PanelY,
			g_values.Card2DIconBaseX, g_values.Card2DIconY, g_values.Card2DIconSpacingX,
			g_values.Card2DIconWidth, g_values.Card2DIconHeight);
	}

	const Values& Get()
	{
		if (!g_loaded)
			Reload();

		return g_values;
	}
}
