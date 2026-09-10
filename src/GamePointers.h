/*
	Resolves the small set of raw engine pointers ScriptHookRDR2's SDK
	doesn't expose but this mod needs -- specifically the live pool of
	running rage::scrThread instances, so we can find poker_sp's own
	running thread and read its script-local variables directly (that's
	where the game's actual table-state struct pointer lives -- see
	PokerCheat.cpp's header comment and docs/JOURNAL.md's Session 2 entry).
*/

#pragma once

#include "..\external\RDR-Classes\script\scrThread.hpp"
#include "..\external\RDR-Classes\rage\atArray.hpp"
#include "..\external\RDR-Classes\rage\joaat.hpp"

namespace GamePointers
{
	// Lazily resolves and caches the address of RDR2.exe's live script
	// thread pool, via the same AOB signature HorseMenu uses (its
	// "ScriptThreads&RunScriptThreads" pattern -- see PatternScan.h and
	// GamePointers.cpp). Returns nullptr if the pattern isn't found (e.g.
	// a game update changed the surrounding code).
	rage::atArray<rage::scrThread*>* GetScriptThreads();

	// Finds the running scrThread for the given script name hash (e.g.
	// rage::Joaat("poker_sp")), or nullptr if it's not currently running.
	// Same matching logic as HorseMenu's Scripts::FindScriptThread.
	rage::scrThread* FindScriptThread(rage::joaat_t scriptHash);

	// Reads script-local slot `index` of `thread` as a raw pointer/value --
	// i.e. *(void**)(thread->m_Stack + index * 8), matching HorseMenu's
	// ScriptLocal addressing. Returns nullptr if the thread has no stack
	// or the index is out of its declared stack size.
	void* ReadScriptLocal(rage::scrThread* thread, std::uint32_t index);

	// Returns the ADDRESS of script-local slot `index` -- i.e.
	// thread->m_Stack + index*8 itself, not what's stored there. Needed
	// to build pointers for native calls that take a script struct by
	// reference (e.g. MINIGAME::_0x32A7C216344D623B's hole/board
	// pointers), since those structs live inline in the thread's own
	// local array, not behind a separately-stored pointer.
	void* GetScriptLocalAddress(rage::scrThread* thread, std::uint32_t index);
}
