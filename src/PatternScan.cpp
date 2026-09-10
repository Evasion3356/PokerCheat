#include "PatternScan.h"

#include <windows.h>
#include <vector>
#include <string>

namespace
{
	struct ParsedPattern
	{
		std::vector<std::uint8_t> bytes;
		std::vector<bool> mask; // true = must match, false = wildcard
	};

	ParsedPattern Parse(std::string_view pattern)
	{
		ParsedPattern parsed;

		size_t i = 0;
		while (i < pattern.size())
		{
			while (i < pattern.size() && pattern[i] == ' ')
				i++;
			if (i >= pattern.size())
				break;

			if (pattern[i] == '?')
			{
				parsed.bytes.push_back(0);
				parsed.mask.push_back(false);
				i++;
				// tolerate "??" as a single wildcard token
				if (i < pattern.size() && pattern[i] == '?')
					i++;
			}
			else
			{
				parsed.bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string(pattern.substr(i, 2)), nullptr, 16)));
				parsed.mask.push_back(true);
				i += 2;
			}
		}

		return parsed;
	}
}

namespace PatternScan
{
	std::optional<std::uintptr_t> FindInMainModule(std::string_view pattern)
	{
		auto base = reinterpret_cast<std::uint8_t*>(GetModuleHandle(nullptr));
		if (!base)
			return std::nullopt;

		auto dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		auto ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
		std::size_t imageSize = ntHeaders->OptionalHeader.SizeOfImage;

		ParsedPattern parsed = Parse(pattern);
		if (parsed.bytes.empty())
			return std::nullopt;

		std::size_t patternLen = parsed.bytes.size();
		if (imageSize < patternLen)
			return std::nullopt;

		for (std::size_t offset = 0; offset <= imageSize - patternLen; offset++)
		{
			bool matched = true;
			for (std::size_t j = 0; j < patternLen; j++)
			{
				if (parsed.mask[j] && base[offset + j] != parsed.bytes[j])
				{
					matched = false;
					break;
				}
			}

			if (matched)
				return reinterpret_cast<std::uintptr_t>(base + offset);
		}

		return std::nullopt;
	}

	std::uintptr_t ResolveRip(std::uintptr_t matchAddress, int operandOffset)
	{
		auto operandAddr = matchAddress + operandOffset;
		std::int32_t displacement = *reinterpret_cast<std::int32_t*>(operandAddr);
		return operandAddr + sizeof(std::int32_t) + displacement;
	}
}
