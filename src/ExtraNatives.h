/*
	PokerCheat will likely need a handful of natives that don't exist (under
	these names) in the stock 2019 ScriptHookRDR2 SDK's natives.h -- same
	situation CollectorOffline hit (see its ExtraNatives.h for the pattern:
	reopen the native's namespace here rather than edit the vendored SDK
	header, using the invoke<> template natives.h already defines).

	Empty for now -- nothing needed until poker_sp.ysc.c has actually been
	traced and we know what, if anything, the cheat mechanism needs to call.

	Include this after natives.h/types.h (script.h does that ordering).
*/

#pragma once
