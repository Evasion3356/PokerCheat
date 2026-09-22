# PokerCheat

A ScriptHookRDR2 ASI mod that makes the player always win at RDR2's
single-player poker minigame (`poker_sp`). Built as a sibling of
`../CollectorOffline`, reusing its exact toolchain and conventions (see
that project's `CLAUDE.md` for the full backstory on why this stack --
ScriptHookRDR2 + native C++, not an injected mod-menu framework -- was
chosen).

**Current status: advisor HUD, not an auto-win cheat.** Reads live hole
cards for every seat and the board directly out of `poker_sp`'s own
script memory, predicts the final board (real revealed cards + the
deterministic future cards sitting at the current deck cursor), and shows
an on-screen HUD with everyone's hand and a WIN/LOSE/CHOP verdict for the
player. Hand scoring is fully self-contained (`src/PokerHandEval.h`, unit
tested -- see Tests below) rather than delegated to the game's own
hand-rank native. Read `docs/JOURNAL.md` for the full derivation/bugfix
history, `docs/PITFALLS.md` for lessons carried over from
`CollectorOffline`, and `src/PokerCheat.cpp`'s header comment for the
current struct-layout/hand-eval design.

## Coding conventions

**No C-style casts, no C-style strings/buffers.** Every cast in this
project's own code is `static_cast`/`reinterpret_cast`/`const_cast` --
never a C-style `(Type)value` cast. No `char buf[N]` locals, `sprintf_s`,
`strcpy_s`/`strcat_s`, or any other hand-rolled size-tracked buffer --
`std::string`/`std::ostringstream` only for building text (see
`FormatFixed1` and the per-seat/board debug lines in `PokerCheat.cpp`, and `SetFloat`/
`NarrowPath` in `Config.cpp`, for the established pattern). The one
unavoidable exception is the literal call-site boundary into a
ScriptHookRDR2 native that requires `char*` (e.g. `GRAPHICS::DRAW_SPRITE`,
`TEXTURE::HAS_STREAMED_TEXTURE_DICT_LOADED`, `GAMEPLAY::CREATE_STRING`) --
build the value as `std::string` and pass
`const_cast<char*>(str.c_str())` only at that call, never a manual
fixed-size buffer upstream of it. Same convention `../BlackjackCheat`
is converging on (see that project's own CLAUDE.md) -- don't reintroduce
either pattern when porting code between the two.

**Strings: `std::string_view` for read-only text.** `Localization`'s
tables are `constexpr std::string_view` and its getters return
`std::string_view`. Per-frame Release HUD text must not allocate: build it
into a reused `static std::string` (`BgFormatText`, `BuildCardTextureName`)
rather than
concatenating temporaries or using `ostringstream`; fixed name sets are a
`constexpr std::string_view` table of whole literals (`kCardSetDicts`, whose
`.data()` is null-terminated and can go straight to a native). Debug-only text can keep
`std::string`/`ostringstream`.

**Logging goes through spdlog, fmt-style, not printf-style.**
`Log::Write` (`src/Log.h`) is a thin template wrapper around a
file-backed `spdlog::logger` (see `external/spdlog`, header-only mode --
`SPDLOG_HEADER_ONLY` is defined before including it, so there's no
separate spdlog `.cpp` to add to the project). Call sites use fmt
`{}`-style placeholders (`Log::Write("seat={} rank={}", seat, rank)`),
not printf `%`-style (`Log::Write("seat=%u rank=%d", seat, rank)`) -- the
old signature took `const char*, ...` and forwarded straight to
`vfprintf`, so a mismatched `%s`/`%d` against the actual argument list
was a real, silent runtime-UB risk (wrong type read off the `va_list`,
or reading past the last supplied argument); `Log::Write`'s
`spdlog::format_string_t<Args...>` parameter validates the placeholder
count against `Args` at COMPILE time instead, so a mismatch is now a
build error. A literal `{`/`}` in a log line's own text (not a
placeholder) must be escaped as `{{`/`}}` -- see the `seat {} (base slot
{}): card0={{rank={},suit={}}}...` style probe lines in
`ProbeTableStruct`/`ProbeSeatOccupancy` for the pattern. Hex addresses
use fmt's `{:#x}` (which supplies its own `0x` prefix) instead of a
literal `"0x%llX"`; fixed-precision floats use `{:.Nf}`.

## Build & deploy

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" PokerCheat.vcxproj /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
```

The project's `PostBuildEvent` auto-locates the RDR2 install directory
(`BuildTools\Find-RDR2GameDir.ps1` -- vendored identically into every
sibling project, since each is its own separate git repo; see that
file's own header comment) and copies the built `.asi` straight into it,
on every build regardless of whether the build itself
was up to date. `DisableFastUpToDateCheck` is set in the `.vcxproj.user`
so this also holds for Visual Studio IDE builds, not just command-line
MSBuild. **RDR2.exe must be closed first** or the copy fails with a
file-in-use error -- check `tasklist //FI "IMAGENAME eq RDR2.exe"` before
every build.

A `Debug|x64` configuration also exists (`/p:Configuration=Debug` in the
same command) for when a crash needs a real live-debugger trace instead of
guessing from static analysis: `/MTd` static debug CRT, optimizations
disabled, and a PDB deployed alongside the `.asi` by the same
`PostBuildEvent`.

Runtime log: `<game folder>\PokerCheat.log`, written by `Log::Write` (see
`src/Log.h`).

## Tests

`tests/PokerHandEvalTests.vcxproj` unit-tests `src/PokerHandEval.h` (the
poker hand scorer/comparator) in complete isolation from the game --
plain console app, no ScriptHookRDR2/game dependency, links against the
exact same header the mod itself includes rather than a hand-copied
duplicate that could silently drift out of sync:

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" tests\PokerHandEvalTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /nologo /v:minimal
bin\Debug\PokerHandEvalTests.exe
```

Exits 0 and prints `ALL PASS` if every case passes; nonzero with a
`[FAIL]` line per failing case otherwise. Add a new case here (and run it)
before changing hand-scoring logic in `PokerHandEval.h` -- that logic went
unverified against real hands for 9 sessions previously (see
`docs/JOURNAL.md`, Session 9) specifically because there was no
automated check on it.

In-game: press F10 for the test menu (NUMPAD 8/2 move, NUMPAD 5 select,
NUMPAD 0/Backspace/F10 back -- same controls as `CollectorOffline` and the
ScriptHookRDR2 SDK's own NativeTrainer sample).

## Source layout

- `src/main.cpp` -- `DllMain`, registers `ScriptMain` with ScriptHookRDR2
  and the keyboard handler.
- `src/script.h` / `script.cpp` -- entry point (`ScriptMain`) and the F10
  menu shell (currently one item: "Toggle Poker Cheat").
- `src/PokerCheat.h` / `.cpp` -- the actual cheat module. `Enabled` flag,
  `Toggle()`, and `OnTick()` (called every frame, currently a no-op). Its
  header comment in `.cpp` is the concrete reversing/implementation plan --
  read it before starting the poker_sp trace.
- `src/scriptmenu.h/.cpp`, `src/keyboard.h/.cpp` -- vendored unchanged from
  `CollectorOffline` (itself adapted from the ScriptHookRDR2 SDK's
  NativeTrainer sample).
- `src/Log.h` -- minimal timestamped file logger (`PokerCheat.log`),
  backed by spdlog (`external/spdlog`, header-only) instead of a
  hand-rolled `fopen_s`/`vfprintf` pair -- see Coding Conventions above
  for the fmt `{}`-style call-site convention this requires.
- `src/ExtraNatives.h` -- empty stub, same purpose as CollectorOffline's:
  reopen a native's namespace here (never edit the vendored SDK header)
  once a needed native turns out to be missing/mistyped in the stock
  `natives.h`.

## External resources

- `D:\Backup\Stuff\RDR2 Shit\Scripts\rdr2-scripts-decompiled\1491.50\script_rel\poker_sp.ysc.c`
  -- the actual target, already decompiled for our exact game build
  (1491.50), ~58k lines. No decompilation work needed, just tracing.
- Same folder: `act_gen_poker.ysc.c` (~44k lines) -- likely the shared
  generic card-game engine `poker_sp` calls into (Rockstar's `act_gen_*`
  naming pattern is reused activity logic across minigames); check here if
  the real hand-evaluation/outcome logic isn't in `poker_sp.ysc.c` itself.
  `poker_launch_sp.ysc.c` (~17.5k lines) is the launcher/wrapper.
- `..\ScriptHookSDK\` -- local copy of Alexander Blade's ScriptHookRDR2 SDK
  (`main.h`/`nativeCaller.h`/`natives.h`/`types.h`/`enums.h`,
  `ScriptHookRDR2.lib`), plus the `NativeTrainer`/`Pools` samples this
  project's menu/keyboard code is adapted from (via CollectorOffline).
  Note the `ScriptHookRDR2.dll` runtime itself (not redistributed here,
  per its terms of use) must be downloaded from
  http://www.dev-c.com/rdr2/scripthookrdr2/ matching game build 1491.50
  and dropped into the game folder alongside the `.asi` -- it isn't part
  of this repo.
- `..\HorseMenu\` -- local clone of the YimMenu/HorseMenu mod-menu source.
  Not a build dependency of this project -- referenced only for its
  `ScriptPatches`/`BytePatch` technique (`src\game\backend\ScriptPatches.cpp`,
  `src\core\memory\BytePatch.cpp`) as a worked example of run-time script
  bytecode patching, in case the poker win-condition can't be cheated with
  a simple variable overwrite. See `docs/JOURNAL.md`.
- `..\CollectorOffline\CLAUDE.md` -- documents
  `D:\Backup\Stuff\RDR2 Shit\EXEs\1491.50\RDR2_Dumped.exe.i64`, an
  already-analyzed IDA database for a memory-dumped RDR2.exe matching this
  exact build, and IDA usage notes (`idat.exe`, IDA Professional 9.3) --
  the right tool if `rage::scrProgram` offsets or an internal (non-native)
  function ever need confirming for 1491.50 specifically.
- `..\Decompiler\rdr3-nativedb-data\natives.json` -- independent
  RDR2-specific native hash/name/param database, useful if a native
  encountered while tracing poker_sp shows up only as `_0xHASH`.
