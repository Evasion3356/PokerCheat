#pragma once

#include <format>
#include <string_view>
#include <utility>

// Logging for the PokerCheat library (PokerCheatLib.vcxproj). The library has no log
// file of its own: whoever links it (this repo's ASI, or Rampagio) sets a
// sink and decides where lines go. Lines written before a sink is set are
// dropped. Separate from the ASI's spdlog Log.h so a host with its own
// inline Log::detail doesn't get two definitions in one binary.
namespace PokerCheat::Log
{
	using Sink = void (*)(std::string_view line);

	void SetSink(Sink sink);
	void Emit(std::string_view line);

	template <typename... Args>
	void Write(std::format_string<Args...> fmt, Args&&... args)
	{
		Emit(std::format(fmt, std::forward<Args>(args)...));
	}
}
