/*
	Chainable script-global field/array accessor -- ScriptLocal.h's
	counterpart for `Global_N`, adapted the same way from HorseMenu's
	`game/rdr/ScriptGlobal.hpp` (D:\Backup\Stuff\RDR2 Shit\HorseMenu\src\
	game\rdr\), but reading through ScriptHookRDR2's own getGlobalPtr()
	instead of a pattern-scanned globals table.

	Same mechanical rule as ScriptLocal, read straight off the decompile:
	  - `something.f_N` -> At(N). No size word implied.
	  - `something[i]` with element stride S -> At(i, S). Skips the array's leading size
	    word automatically, then advances i * S.
	so `Global_1945188[slot]` (stride 18) `.f_3` is
	ScriptGlobal(1945188).At(slot, 18).At(3) -- global 1945188 + 1 + slot*18
	+ 3, never a hand-flattened offset that can miss the size word.
*/

#pragma once

#include "script.h" // getGlobalPtr (main.h)

#include <cstdint>

class ScriptGlobal
{
	std::uint32_t m_Index;

public:
	constexpr explicit ScriptGlobal(std::uint32_t index) :
		m_Index(index)
	{
	}

	// Plain nested-struct field access -- the decompile shows `.f_N`.
	constexpr ScriptGlobal At(std::uint32_t fieldOffset) const
	{
		return ScriptGlobal(m_Index + fieldOffset);
	}

	// Array-element access -- the decompile shows `something[i]` (stride S).
	// Skips the array's size word, then advances index * elementStride.
	constexpr ScriptGlobal At(std::uint32_t index, std::uint32_t elementStride) const
	{
		return ScriptGlobal(m_Index + 1 + index * elementStride);
	}

	constexpr std::uint32_t Index() const { return m_Index; }

	// The value lives in the low 4 bytes of the 8-byte slot (the high 4
	// can be unrelated -- see ..\FishingFix's history). 0 if the global
	// isn't mapped.
	std::int32_t AsInt32() const
	{
		const UINT64* slot = getGlobalPtr(static_cast<int>(m_Index));
		return slot ? static_cast<std::int32_t>(*slot & 0xFFFFFFFFu) : 0;
	}
};
