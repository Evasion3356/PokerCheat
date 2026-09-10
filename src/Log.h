/*
	Minimal file logger. Writes PokerCheat.log next to the .asi (i.e. in
	the game's root directory, same place ScriptHookRDR2.log/CollectorOffline.log
	live). Identical pattern to CollectorOffline's Log.h.
*/

#pragma once

#include <cstdio>
#include <cstdarg>
#include <windows.h>

namespace Log
{
	inline void Write(const char* fmt, ...)
	{
		FILE* f = nullptr;
		fopen_s(&f, "PokerCheat.log", "a");
		if (!f)
			return;

		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

		va_list args;
		va_start(args, fmt);
		vfprintf(f, fmt, args);
		va_end(args);

		fprintf(f, "\n");
		fclose(f);
	}
}
