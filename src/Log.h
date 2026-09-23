/*
	Minimal file logger, now backed by spdlog (see external/spdlog)
	instead of a hand-rolled fopen_s/vfprintf pair. Still writes
	PokerCheat.log next to the .asi (i.e. in the game's root directory,
	same place ScriptHookRDR2.log lives) -- one line per Log::Write call,
	timestamped -- but the timestamp now comes from spdlog's own pattern
	formatter instead of a manually-built SYSTEMTIME string, and the file
	handle is opened once and kept (spdlog's basic_file_sink), not
	reopened via fopen_s/fclose on every single call.

	Header-only mode (SPDLOG_HEADER_ONLY) is used deliberately: the whole
	reason to pull spdlog in is call-site type safety (see below), not a
	build-system change, so there's no separate spdlog .cpp to add to
	this project -- it compiles straight into PokerCheat.cpp's/etc. own
	translation units, same as the rest of this project's external/
	dependencies (inipp, RDR-Classes).

	Call sites use fmt {}-style placeholders now, e.g.
	Log::Write("seat={} rank={}", seat, rank) -- NOT printf %-style
	(Log::Write("seat=%u rank=%d", seat, rank), the old signature). This
	is a real type-safety improvement, not just cosmetic: spdlog's
	logger::info<Args...>(format_string_t<Args...>, Args&&...) (which
	Write() below forwards to) validates the format string's placeholder
	count against Args at COMPILE time via fmt -- a mismatched %s/%d
	between the format string and the actual argument list was a real,
	silent runtime UB risk with the old vfprintf-based version (wrong
	type read off the va_list, or reading past the last supplied arg
	entirely); a mismatched {} now fails to compile instead.

	Synchronous in both Debug and Release, deliberately -- an async
	logger was tried (a background worker thread + queue in Release
	builds only) and reverted: the ASI can get unloaded/ejected by
	ScriptHookRDR2 at any point (see CLAUDE.md), and a queued-but-not-
	yet-written log line racing that unload is a real risk an async
	logger introduces for no real benefit here -- this mod logs at most
	a handful of lines per second, nowhere near enough for the blocking
	file write to be a measurable per-frame cost. Not worth trading a
	small, unproven performance win for a lost-log-lines-on-eject risk.

	Where the file goes: next to the .asi when that folder is writable,
	otherwise %LOCALAPPDATA%\RDR2ASIMods\ (see LogFallback.h), with a first
	line saying which path was rejected. Creating the logger never throws --
	if nothing is writable, Log::Write silently does nothing. It used to throw
	spdlog_ex out of the first Log::Write, which runs early enough in game
	load to crash RDR2 when the install folder is read-only (e.g. a Rockstar
	Launcher install under C:\Program Files). tests/LogFallbackTests covers
	exactly that scenario.
*/

#pragma once

#define SPDLOG_HEADER_ONLY
#define SPDLOG_WCHAR_FILENAMES

#include "..\external\spdlog\include\spdlog\spdlog.h"
#include "..\external\spdlog\include\spdlog\sinks\basic_file_sink.h"

#include "LogFallback.h"

#include <memory>
#include <string>
#include <utility>

namespace Log
{
	namespace detail
	{
		// Logs to preferredDir + fileName, or to fallbackDir + fileName when
		// that can't be written. Never throws; nullptr if neither works.
		// Not registered with spdlog's global registry, so a second call with
		// the same name (tests, a hot-reload) can't throw "already exists".
		inline std::shared_ptr<spdlog::logger> CreateLogger(const std::string& name, const std::wstring& preferredDir,
			const std::wstring& fileName, const std::wstring& fallbackDir)
		{
			try
			{
				const LogFallback::Resolved resolved = LogFallback::Resolve(preferredDir, fileName, fallbackDir);

				// The preferred path can still fail inside spdlog after passing
				// Resolve()'s probe (e.g. locked in between) -- retry at the
				// fallback before giving up.
				const std::wstring candidates[2] = {
					resolved.path,
					resolved.usedFallback || fallbackDir.empty() ? std::wstring() : fallbackDir + fileName };
				for (int i = 0; i < 2; i++)
				{
					if (candidates[i].empty())
						continue;
					try
					{
						if (i == 1)
							LogFallback::EnsureDirectory(fallbackDir);
						auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(candidates[i], false);
						auto logger = std::make_shared<spdlog::logger>(name, std::move(sink));
						logger->set_pattern("[%H:%M:%S.%e] %v");
						logger->flush_on(spdlog::level::trace);
						if (resolved.usedFallback || i == 1)
							logger->info("Log redirected here: could not write {}",
								LogFallback::ToUtf8(resolved.usedFallback ? resolved.rejectedPath : preferredDir + fileName));
						return logger;
					}
					catch (...)
					{
					}
				}
			}
			catch (...)
			{
			}
			return nullptr;
		}

		inline const std::shared_ptr<spdlog::logger>& GetLogger()
		{
			static const std::shared_ptr<spdlog::logger> logger = CreateLogger(
				"PokerCheat", LogFallback::ModuleDirectory(), L"PokerCheat.log", LogFallback::FallbackDirectory());
			return logger;
		}
	}

	template <typename... Args>
	void Write(spdlog::format_string_t<Args...> fmt, Args&&... args)
	{
		if (const auto& logger = detail::GetLogger())
			logger->info(fmt, std::forward<Args>(args)...);
	}
}
