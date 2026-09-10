#include "GamePointers.h"
#include "PatternScan.h"
#include "Log.h"

namespace
{
	// HorseMenu's "ScriptThreads&RunScriptThreads" signature -- see
	// D:\Backup\Stuff\RDR2 Shit\HorseMenu\src\game\pointers\Pointers.cpp,
	// line ~83. The matched instruction is `LEA reg, [rip+disp32]`
	// (48 8D 0D), so the RIP-relative operand starts right after that
	// 3-byte opcode.
	constexpr const char* kScriptThreadsPattern = "48 8D 0D ? ? ? ? E8 ? ? ? ? EB 0B 8B 0D";
	constexpr int kScriptThreadsOperandOffset = 3;
}

namespace GamePointers
{
	rage::atArray<rage::scrThread*>* GetScriptThreads()
	{
		static rage::atArray<rage::scrThread*>* cached = []() -> rage::atArray<rage::scrThread*>*
		{
			auto match = PatternScan::FindInMainModule(kScriptThreadsPattern);
			if (!match)
			{
				Log::Write("GamePointers::GetScriptThreads: pattern not found");
				return nullptr;
			}

			auto resolved = PatternScan::ResolveRip(*match, kScriptThreadsOperandOffset);
			Log::Write("GamePointers::GetScriptThreads: pattern matched at 0x%llX, resolved to 0x%llX",
				static_cast<unsigned long long>(*match), static_cast<unsigned long long>(resolved));
			return reinterpret_cast<rage::atArray<rage::scrThread*>*>(resolved);
		}();

		return cached;
	}

	rage::scrThread* FindScriptThread(rage::joaat_t scriptHash)
	{
		auto threads = GetScriptThreads();
		if (!threads)
			return nullptr;

		for (auto& thread : *threads)
		{
			if (thread && thread->m_Context.m_ThreadId && thread->m_Context.m_ScriptHash == scriptHash)
				return thread;
		}

		return nullptr;
	}

	void* ReadScriptLocal(rage::scrThread* thread, std::uint32_t index)
	{
		if (!thread || !thread->m_Stack)
			return nullptr;

		if (thread->m_Context.m_StackSize <= index)
			return nullptr;

		return reinterpret_cast<void**>(thread->m_Stack)[index];
	}

	void* GetScriptLocalAddress(rage::scrThread* thread, std::uint32_t index)
	{
		if (!thread || !thread->m_Stack)
			return nullptr;

		if (thread->m_Context.m_StackSize <= index)
			return nullptr;

		return reinterpret_cast<void**>(thread->m_Stack) + index;
	}
}
