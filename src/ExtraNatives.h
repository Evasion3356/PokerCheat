/*
	PokerCheat will likely need a handful of natives that don't exist (under
	these names) in the stock 2019 ScriptHookRDR2 SDK's natives.h -- same
	situation CollectorOffline hit (see its ExtraNatives.h for the pattern:
	reopen the native's namespace here rather than edit the vendored SDK
	header, using the invoke<> template natives.h already defines).

	Include this after natives.h/types.h (script.h does that ordering).
*/

#pragma once

// UIDEBUG::_BG_DISPLAY_TEXT / _BG_SET_TEXT_SCALE / _BG_SET_TEXT_COLOR --
// missing from the stock SDK entirely (no UIDEBUG namespace at all).
//
// Needed because the natives this file's DrawLine()/DrawFontTest()
// otherwise use turned out to be the WRONG generation for our build:
// UI::DRAW_TEXT (hash 0xD79334A4BB99BAD1) is HUD::_DISPLAY_TEXT under a
// different name, and UI::SET_TEXT_COLOR_RGBA (hash 0x50A41AD966910F03,
// natives.h:4750) is HUD::_SET_TEXT_COLOR -- both are documented
// (github.com/Halen84/RDR2-Native-Menu-Base, inc/natives.h, comments
// directly above these same two hashes) as nullsub/no-ops since game
// build 1436. Our build is 1491.50, long past that. That same project
// defaults BUILD_1311_COMPATIBLE to 0 (i.e. NOT the pair above) and uses
// the UIDEBUG pair below (build 1355+) instead for any build newer than
// 1311 -- confirmed as their actual working, shipped mechanism, not a
// guess. Per that project's DrawFormattedText(), this pair is also what
// makes Scaleform-style rich text tags (<FONT FACE=...>, <P ALIGN=...>,
// <TEXTFORMAT...>) embedded in a LITERAL_STRING actually get parsed
// instead of printed literally -- the mechanism PokerCheat.cpp's
// DrawFontTest() is testing. "Note: you must use VAR_STRING" per that
// project's own comment on _BG_DISPLAY_TEXT -- i.e. still call it via
// MISC::VAR_STRING/GAMEPLAY::CREATE_STRING(flags, template, text) first,
// same as every other DRAW_TEXT call in this file already does, just
// passing the result to _BG_DISPLAY_TEXT instead of UI::DRAW_TEXT.
namespace UIDEBUG
{
	static void _BG_DISPLAY_TEXT(char* text, float x, float y) { invoke<Void>(0x16794E044C9EFB58, text, x, y); }
	static void _BG_SET_TEXT_SCALE(float scaleX, float scaleY) { invoke<Void>(0xA1253A3C870B6843, scaleX, scaleY); }
	static void _BG_SET_TEXT_COLOR(int red, int green, int blue, int alpha) { invoke<Void>(0x16FA5CE47F184F1E, red, green, blue, alpha); }
}
