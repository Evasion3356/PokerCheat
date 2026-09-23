#include "GamePointers.h"
#include "PatternScan.h"
#include "Log.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cmath>

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
		// Only a successful scan is cached. A miss is retried (at most every
		// 5 s, logged once) rather than cached forever, so a scan that runs
		// before RDR2.exe has finished unpacking can't leave the mod dead
		// for the whole session. Script-fiber only (see main.cpp).
		static rage::atArray<rage::scrThread*>* cached = nullptr;
		static ULONGLONG nextAttemptMs = 0;
		static bool loggedMiss = false;
		if (cached)
			return cached;

		const ULONGLONG nowMs = GetTickCount64();
		if (nowMs < nextAttemptMs)
			return nullptr;
		nextAttemptMs = nowMs + 5000;

		auto match = PatternScan::FindInMainModule(kScriptThreadsPattern);
		if (!match)
		{
			if (!loggedMiss)
				Log::Write("GamePointers::GetScriptThreads: pattern not found -- retrying every 5 s");
			loggedMiss = true;
			return nullptr;
		}

		auto resolved = PatternScan::ResolveRip(*match, kScriptThreadsOperandOffset);
		Log::Write("GamePointers::GetScriptThreads: pattern matched at {:#x}, resolved to {:#x}",
			static_cast<unsigned long long>(*match), static_cast<unsigned long long>(resolved));
		cached = reinterpret_cast<rage::atArray<rage::scrThread*>*>(resolved);
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

	bool IsScriptLocalInRange(rage::scrThread* thread, std::uint32_t index)
	{
		return thread && thread->m_Stack && index < thread->m_Context.m_StackSize;
	}

	void* ReadScriptLocal(rage::scrThread* thread, std::uint32_t index)
	{
		if (!IsScriptLocalInRange(thread, index))
			return nullptr;

		return reinterpret_cast<void**>(thread->m_Stack)[index];
	}

	bool DumpLocalStackJsonl(rage::scrThread* thread, std::uint32_t startSlot, std::uint32_t count, const std::string& outPath)
	{
		if (!thread || !thread->m_Stack)
		{
			Log::Write("GamePointers::DumpLocalStackJsonl: no thread/stack");
			return false;
		}

		std::uint32_t stackSize = thread->m_Context.m_StackSize;
		std::uint32_t end = startSlot + count; // count is caller-controlled and small in practice; overflow would only make this MORE conservative via the clamp below, never read out of bounds
		if (end > stackSize || end < startSlot)
			end = stackSize;

		if (startSlot >= end)
		{
			Log::Write("GamePointers::DumpLocalStackJsonl: startSlot {} >= stack size {}, nothing to dump", startSlot, stackSize);
			return false;
		}

		std::ofstream f(outPath);
		if (!f)
		{
			Log::Write("GamePointers::DumpLocalStackJsonl: failed to open {}", outPath);
			return false;
		}

		// Every slot is 8 raw bytes (same addressing ReadScriptLocal uses)
		// -- dumped as every plausible interpretation rather than picking
		// one, since the whole point is not yet knowing which is right.
		const std::uint64_t* slots = reinterpret_cast<const std::uint64_t*>(thread->m_Stack);
		for (std::uint32_t i = startSlot; i < end; i++)
		{
			std::uint64_t raw = slots[i];
			std::uint32_t u32 = static_cast<std::uint32_t>(raw);
			std::int32_t i32 = static_cast<std::int32_t>(u32);
			std::int64_t i64 = static_cast<std::int64_t>(raw);
			float f32;
			std::memcpy(&f32, &u32, sizeof(f32));

			// A garbage bit pattern (this dump has no idea which slots hold
			// real floats) often lands on NaN/Inf -- operator<< prints those
			// as bare `nan`/`-nan`/`inf` tokens, which isn't valid JSON and
			// breaks any parser (jq, Python's json module, etc.) reading the
			// file whole. JSON has no NaN/Infinity literal, so `null` is the
			// correct representation, same as every JSON library's own
			// float-to-JSON serializer does for a non-finite value.
			// i64 is quoted: JSON numbers are only interoperably safe up to
			// 2^53 (RFC 8259's note on IEEE-754 double range), and a raw
			// 64-bit reinterpretation of garbage stack bytes routinely
			// exceeds that -- a bare number here would silently lose
			// precision under double-based parsers (JS JSON.parse, older
			// jq). slot/i32/u32 stay bare numbers since their full range
			// fits well within 2^53.
			std::ostringstream line;
			line << "{\"slot\":" << i
				<< ",\"i32\":" << i32
				<< ",\"u32\":" << u32
				<< ",\"i64\":\"" << i64 << "\""
				<< ",\"f32\":";
			if (std::isfinite(f32))
				line << f32;
			else
				line << "null";
			line << ",\"hex\":\"" << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << raw << "\"}";
			f << line.str() << "\n";
		}

		f.close();
		Log::Write("GamePointers::DumpLocalStackJsonl: wrote slots [{}, {}) to {}", startSlot, end, outPath);
		return true;
	}

	bool DumpLocalStackJsonl(rage::scrThread* thread, const std::string& outPath)
	{
		if (!thread || !thread->m_Stack)
			return false;

		return DumpLocalStackJsonl(thread, 0, thread->m_Context.m_StackSize, outPath);
	}
}
