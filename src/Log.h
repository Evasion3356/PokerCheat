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
*/

#pragma once

#define SPDLOG_HEADER_ONLY
#include "..\external\spdlog\include\spdlog\spdlog.h"
#include "..\external\spdlog\include\spdlog\sinks\basic_file_sink.h"

#include <memory>

namespace Log
{
	namespace detail
	{
		// nullptr (rather than throwing) if the log file can't be opened --
		// matches the old version's "if (!f) return" fallback: logging is
		// diagnostic, never something a failure to open a file should be
		// allowed to crash or otherwise disrupt the mod over.
		inline std::shared_ptr<spdlog::logger> CreateLogger()
		{
			try
			{
				auto logger = spdlog::basic_logger_mt("PokerCheat", "PokerCheat.log");
				logger->set_pattern("[%H:%M:%S.%e] %v");
				logger->flush_on(spdlog::level::info);
				return logger;
			}
			catch (const spdlog::spdlog_ex&)
			{
				return nullptr;
			}
		}

		// Function-local static ("magic static") for thread-safe
		// exactly-once initialization, same convention as Config.cpp's
		// ResolveIniPath().
		inline const std::shared_ptr<spdlog::logger>& GetLogger()
		{
			static std::shared_ptr<spdlog::logger> logger = CreateLogger();
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
