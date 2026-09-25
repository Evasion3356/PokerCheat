# Journal

Chronological record of what's been built and where each piece currently
stands. Read `../CLAUDE.md` first for orientation, `PITFALLS.md` for the
lessons behind these decisions.

## Session 1 -- boilerplate

Scaffolded the mod as a sibling of `CollectorOffline`, reusing its exact
conventions (ScriptHookSDK-based ASI, F10 menu vendored from the SDK's
NativeTrainer sample, `Log::Write` file logger, `vcxproj` build/deploy
setup) rather than inventing new ones, since that project's toolchain is
already proven working on this machine (a built `.asi` exists in its
`bin\Release`).

No cheat logic yet -- `PokerCheat::OnTick()` is a no-op stub. Confirmed the
target script is already decompiled for our exact game build (1491.50) at
`D:\Backup\Stuff\RDR2 Shit\Scripts\rdr2-scripts-decompiled\1491.50\script_rel\`:
`poker_sp.ysc.c` (~58k lines), `act_gen_poker.ysc.c` (~44k lines, likely
shared card-game engine), `poker_launch_sp.ysc.c` (~17.5k lines, launcher).
Reversing/tracing that source is next session's work -- not started yet.

Reference for the eventual cheat mechanism if it needs bytecode-level
patching rather than a simple variable overwrite: HorseMenu's
`ScriptPatches`/`BytePatch` classes
(`D:\Backup\Stuff\RDR2 Shit\HorseMenu\src\game\backend\ScriptPatches.cpp`,
`src\core\memory\BytePatch.cpp`) snapshot a running script's code pages,
patch bytes in the copy located via a byte pattern, and swap
`rage::scrProgram::m_CodeBlocks` to the patched copy only while that
specific program is executing. Not ported in yet -- only relevant once we
know the win-condition logic actually needs opcode-level patching instead
of a plain variable/global overwrite.

## Session 2 -- reversing poker_sp: data model, not "always win"

Direction changed from "patch the script so the player always wins" to
"read the true game state and advise the player" (bet/fold/double-down,
who has what, what the board is) -- an information/ESP-style mod, not a
forced-outcome one. Below is what `poker_sp.ysc.c` actually looks like,
traced directly from the decompiled source (all line numbers refer to
`Scripts\rdr2-scripts-decompiled\1491.50\script_rel\poker_sp.ysc.c` unless
stated otherwise). Function numbers (`func_N`) are this decompile's own
numbering, stable for this exact 1491.50 build only.

### Card encoding (confirmed)

A card is a 2-field struct: `{ rank, suit }`.
- `rank`: **2-14** (2..10 numeric, 11=J, 12=Q, 13=K, 14=A)
- `suit`: **0-3** (which physical suit maps to which int not yet
  confirmed -- no UI/label lookup traced yet, see Open questions)

Confirmed by `func_629` (card validity check, line 26294:
`iParam0 >= 2 && iParam0 <= 14 && iParam0.f_1 >= 0 && iParam0.f_1 <= 3`)
and independently by `func_1317` (line 44515, the "build list of unseen
cards" routine: nested loop `for rank in 2..14 { for suit in 0..3 { ... } }`
emitting every `{rank, suit}` not already present in the known-card sets).
`func_628` (line 26289) is declared `struct<2>` (a 2-field return), and
returns exactly `uParam0->f_39[seat /*56*/].f_7[which /*2*/]` -- the raw
per-seat hole card getter, confirming the 2-field shape from a second,
independent call site.

### Where the live data lives (all found via one shared struct pointer)

Every function below takes a pointer to what's clearly one large,
persistent "poker table" struct (`f_2` = stakes tier 1/2/3, `f_485` = seat
count, etc.) -- called `uParam0`/`uParam1` inconsistently depending on the
function's own parameter order, not two different structs.

- **Hole cards, all seats**: `table->f_39[seat /*56*/].f_7[0..1]` -- seat
  is 0-5 (6-seat array), each `.f_7` is the 2-card hole hand. This is the
  actual finding for "read all the cards on the table": **the human
  player and every AI opponent's hole cards live in the same struct**,
  there's no separate hidden/server-only storage for opponents' cards --
  the "you can't see their cards" restriction is purely the UI layer, not
  a data-access one. `func_628(table, seat, which)` (line 26289) is the
  getter Rockstar's own UI code uses.
- **Per-seat hand-rank category**: `table->f_39[seat].f_31.f_24`, an int
  0-9. `func_1328` (line 44694) just returns this field, and the 0-9
  mapping is nailed down exactly by the stat-tracking switch at line
  29880 (`func_739`): 0=high card, 1=2-of-a-kind (pair), 2=2 pair,
  3=3-of-a-kind, 4=straight, 5=flush, 6=full house, 7=4-of-a-kind,
  8=straight flush, 9=royal flush. `func_1639` (line 52952) treats this
  field as "valid" only when it's in `[0,9]` -- i.e. it's populated
  on-demand, not always meaningful (see `func_1346` below).
- **Hand comparison**: `func_1541(&handA.f_31, &handB.f_31)` (a
  tri-state compare, sign gives the better hand) backs `func_1327`
  (A better than B), `func_1329` (A >= B), `func_1332` (tie) -- all in the
  44689-44738 range. This is how the game itself picks pot winners among
  contributing seats.
- **Recompute every active seat's hand as of a given board depth**:
  `func_1346(table, &outCopy, streetIndex)` (line 44858). `streetIndex`
  0-4 maps to board-card-count via `func_1640` (line 44960 switch):
  0->0 (preflop), 1->3 (flop), 2->4 (turn), 3 or 4->5 (river/showdown).
  For each active seat it calls `func_1641(&outCopy.f_39[seat].f_31,
  &outCopy.f_39[seat].f_7, &outCopy.f_15)` to refresh that seat's `.f_31`
  hand-eval struct from its hole cards + the board struct.
- **The actual hand evaluator is a single game native, not script math**:
  `func_1641` (line 52986) is a one-line wrapper:
  `MINIGAME::_0x32A7C216344D623B(holeCards*, board*, outHandEval*) -> BOOL`.
  Already declared (untyped, `_0x...` name) in the stock SDK's
  `natives.h` and in `rdr3-nativedb-data/natives.json` (also unnamed
  there). **This means we don't need to reimplement poker hand ranking at
  all** -- we can call this exact native ourselves from the ASI, feeding
  it any hole cards + board we've read (or hypothetical ones), and get
  Rockstar's own ranking back.
- **Win-probability / equity is also a single native, and the game
  already runs it for every AI seat every frame**: `func_690` (line
  28182) is a per-tick coroutine (`.f_8` cursor advances one seat per
  call) that, for the seat currently up, builds `hole = table->f_39[seat].f_7`,
  `board = table->f_15`, the 52-card deck, and the not-yet-seen-card list
  via `func_1317`, then calls
  `MINIGAME::_0xEC819D612038EF4B(stakesTier, board*, hole*, unseenDeck*,
  unseenCount, trialCount, &wins, &totalTrials)` (line 28217) -- a Monte
  Carlo equity simulator, `trialCount` scaling with stakes tier (20/50/100
  trials). The result is stored as `coroutine.f_1[seat] = wins/totalTrials`
  -- **a live win-probability float per seat, refreshed continuously**.
  Only one call site drives this (line 11649, `func_292`, ticking whenever
  `table.f_114.f_2011 != 0`) -- looks like the main per-frame state-machine
  driver, not something gated to only the human's turn.
  **This is the direct answer to "should I bet, fold, or double down":**
  the same native the AI itself uses for its own decisions, called with
  the human seat's hole cards, gives a precise win% against the field
  without us reimplementing any poker math.
- **Deck bookkeeping is NOT a persistent shuffled-deck array.** `func_1317`
  (line 44515) reconstructs the "unseen" cards by brute-force enumerating
  all 52 `{rank,suit}` combos and excluding ones already in the known
  hole+board card sets (`func_1633`, not yet read) every time it's called
  -- there's no `remainingDeck[]` field being consumed/shrunk over time.
  Combined with the AI using **Monte Carlo simulation** (not a known
  answer) to estimate its own equity, this is fairly strong evidence that
  **future board cards are not predetermined at deal time** -- i.e.
  "what's going to be the house cards" may not be a knowable fact before
  the game itself deals that street, not just a value we haven't located
  yet. Not proven -- see Open questions.

### Open questions / not yet done (next session)

1. **How do we actually reach this struct's address at runtime?** Traced
   `main()` (line 4848): `func_3(&(uLocal_14.f_1), &uScriptParam_0, &func_1,
   &func_2)` -- `uScriptParam_0` is poker_sp's own first script parameter,
   and given the struct's size (fields well past `f_2800`+ observed), it's
   almost certainly a **pointer** value (into a Global-backed allocation
   the launcher owns), not the struct inlined into poker_sp's local var
   space. Two ways to get it, neither attempted yet:
     - Find the actual `START_NEW_SCRIPT*` call site that launches
       `poker_sp` (searched `poker_launch_sp.ysc.c` for
       `START_NEW_SCRIPT`/`Global_` near `joaat("poker_sp")` -- only found
       `SCRIPTS::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH`
       at its line 3483, i.e. a "not already running" guard, not the
       launch itself. The actual launch call must be elsewhere -- maybe a
       camp/table interaction script that triggers `poker_launch_sp`,
       which then triggers `poker_sp`. Not chased yet.)
     - Or skip finding the launcher entirely: enumerate running script
       threads for name-hash `joaat("poker_sp")` at runtime (same
       `Pointers.ScriptPrograms[]`-style table HorseMenu's `ScriptPatches`
       uses, see Session 1 note) and read **local slot 0** of that
       specific thread directly -- script params occupy the low local
       slots by calling convention, so `uScriptParam_0`'s pointer value
       should just be sitting there. This needs `rage::scrThread`'s real
       field offsets for 1491.50 confirmed first (PITFALLS.md: don't
       hand-guess), via the IDA database CLAUDE.md references.
2. **Board struct (`table->f_15`) layout beyond the reveal-count field.**
   Only confirmed `.f_23` = revealed-card count (an int 0/3/4/5, set from
   `func_1640`). Have not yet located the field(s) holding the actual 5
   dealt community card values. Needed both to read "current house cards"
   directly and to test finding #3 below.
3. **Verify the "future board not predetermined" hypothesis directly** by
   finding the actual "reveal the flop/turn/river" function (not found
   yet) and checking whether it draws a fresh random card at that moment
   or just unmasks a value that already existed in the table struct
   before the reveal animation played. This is the deciding test for
   whether "what's going to be the house cards" is answerable at all
   before Rockstar's own code deals it.
4. **Suit int -> display suit (Hearts/Diamonds/Clubs/Spades) mapping** --
   not traced. Only matters for human-readable output, not for calling
   the hand-eval/equity natives (they just need the raw ints).
5. **Bet/pot/stack fields** -- `table->f_39[seat].f_3` appeared once
   (`func_1636`, side-pot boundary calc) as something chip-stack-shaped,
   and `table->f_376[seat]` looks like the per-seat pot-contribution/
   eligibility struct (`.f_2[]`/`.f_9` = pot index list + count, `.f_10`/
   `.f_17` = winners list + count). Not traced deeply -- needed for real
   "pot odds" advice (equity alone tells you if you're ahead, not whether
   a given bet is worth calling).
6. **`act_gen_poker.ysc.c` (44k lines) not touched this session** --
   everything found so far was self-contained in `poker_sp.ysc.c`. May be
   irrelevant to hand/equity logic (possibly a different/generic
   camp-activity framework), or may hold code poker_sp calls into some
   other way. Worth a quick pass if a needed function turns out to live
   there instead (e.g. if the flop/turn/river deal function isn't in
   poker_sp.ysc.c).

### Runtime plumbing: RDR-Classes already has real scrThread/scrProgram offsets

Checked `CollectorOffline\external\RDR-Classes\script\` before asking for
any IDA help, since that external lib is already vendored and might have
solved this already -- it has. `scrProgram.hpp` and `scrThread.hpp` give
concrete, presumably-confirmed field offsets (not guesses):
- `scrProgram`: `m_CodeBlocks` (0x10, used by HorseMenu's ScriptPatches),
  `m_NameHash` (0x58, matches by `joaat("poker_sp")`), `m_LocalData`
  (0x30) -- almost certainly the compiled script's *default/initial*
  local-variable template (used to seed a new thread's stack), not live
  per-instance state, given `scrThread` is the actual running-instance
  object.
- `scrThread`: `m_Context` (a `scrThreadContext`, 0x008-0x6B7) then
  `m_Stack` (`void*`, 0x6B8) -- the thread's actual local-variable +
  expression-stack memory. `scrThreadContext` has `m_FramePointer`/
  `m_StackPointer` (0x10/0x14, offsets *into* `m_Stack`, not pointers
  themselves) and `m_ScriptHash` (0x04, another way to match "is this the
  poker_sp thread"). `types.hpp` confirms script values are 8 bytes each
  (`alignas(8)`), so decompiler field index N should correspond to byte
  offset N*8 within whatever local/struct region it's indexing -- not
  yet proven empirically for this specific struct chain, but consistent
  with everything read so far.

What's still missing to actually use this: **where the pool of active
`scrThread*` instances lives** (a global/singleton the game's script
scheduler owns) so we can find the one running `poker_sp` by
`m_ScriptHash`/`m_Context.m_ScriptHash`. Not derivable from script source
-- this is a real-engine-code question, handed to the user's open IDA
session (see below) rather than guessed at.

### IDA asks handed to the user (session's open IDA, RDR2_Dumped.exe.i64)

Two native hashes, both already confirmed as real call sites in
`poker_sp.ysc.c` this session, asked for their actual implementation
(decompiled pseudocode, or at minimum which struct fields/offsets they
touch):
1. **`0x8E34C953364A76DD`**
   (`SCRIPTS::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH`,
   confirmed called from `poker_launch_sp.ysc.c` line 3483 as a
   not-already-running guard before it launches `poker_sp`). Its
   implementation has to enumerate the live `scrThread` pool by hash --
   reading it gives us both the pool's location and the exact iteration/
   match logic for free, resolving the "how do we find poker_sp's running
   thread" question above.
2. **`0x32A7C216344D623B`** (the hand-eval native,
   `MINIGAME::_0x32A7C216344D623B(hole*, board*, outEval*)`). Its
   implementation reads the hole/board/output structs directly, so it's
   ground truth for their real byte layout -- specifically resolves open
   question #2 above (board struct fields beyond the reveal-count byte).

### Runtime plumbing resolved (mostly) without needing more IDA time

Before asking for a third IDA decompile, checked `HorseMenu`'s own source
(a full mod menu, so it almost certainly already solved "find a running
script's thread") -- it had already solved exactly this:
- `HorseMenu\src\game\pointers\Pointers.cpp` (~line 83): a portable AOB
  signature, `"48 8D 0D ? ? ? ? E8 ? ? ? ? EB 0B 8B 0D"` (labeled
  "ScriptThreads&RunScriptThreads"), RIP-relative-resolved from the match,
  gives the address of `rage::atArray<rage::scrThread*>` -- the engine's
  live pool of running script threads. This needs no per-build offset
  math, just a byte scan of the loaded RDR2.exe image (same general
  technique CollectorOffline's `RpfMounter.cpp`/`ChangeSetActivator.cpp`
  already use).
- `HorseMenu\src\game\rdr\Scripts.cpp`'s `FindScriptThread(hash)`: walks
  that array, matching `thread->m_Context.m_ScriptHash == hash` (and
  `m_ThreadId != 0` to skip empty slots).
- `HorseMenu\src\game\rdr\ScriptLocal.cpp` / `SavedVariables.cpp`: confirm
  a script's locals are read as `reinterpret_cast<void**>(thread->m_Stack)[index]`
  -- i.e. local slot N is 8 bytes at `m_Stack + N*8`, exactly matching
  `RDR-Classes\script\types.hpp`'s `alignas(8)` convention on every script
  value type.

This means the two IDA decompiles from earlier this session weren't
actually needed to solve the "find the running thread" problem --
`GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH`'s chain
(`sub_140EE60FC`/`67DC`/`142AFEAF8`/`142AFEAF4`, also decompiled this
session via a headless `idat.exe -S` run rather than more manual
copy-paste from the user, per their suggestion) turned out to only expose
a *ref count* on the `scrProgram` pool entry (confirmed
`scrProgram::m_RefCount` at 0x5C matches `RDR-Classes\rage\fwBasePool.hpp`
field-for-field once the wrapper struct's `unk_144A5CC50` base is
accounted for), not an actual thread pointer -- a dead end for this
specific question, though it did independently confirm RDR-Classes'
`fwBasePool`/`scrProgram` offsets are accurate against the real binary.
Kept as a red-thread note in case `scrProgram::m_RefCount` is useful
later (e.g. a cheap "is poker_sp loaded at all" check without walking the
thread array).

**Confirmed `uScriptParam_0`'s real local-slot index: 4810.** Counted
directly from `poker_sp.ysc.c`'s declared-locals region: the last
`uLocal_N` is `uLocal_4809` (4810 slots, 0-4809), and `uScriptParam_0` is
declared immediately after, before `main()` -- so it occupies slot 4810.

### PokerCheat.exe changes this session: diagnostic probe added, untested

Added (builds clean, both Debug and Release, not yet run against a live
game):
- `src/PatternScan.h/.cpp` -- minimal standalone AOB scanner (own
  implementation, not HorseMenu's `Pattern`/`PointerCalculator` classes --
  kept deliberately small since we only need one signature right now).
- `src/GamePointers.h/.cpp` -- resolves `ScriptThreads` via the pattern
  above (cached after first call), `FindScriptThread(hash)`, and
  `ReadScriptLocal(thread, index)`.
- `external\RDR-Classes\` -- vendored the same copy CollectorOffline uses
  (347K, headers only) so `scrThread`/`scrProgram`/`atArray`/`joaat` are
  available without a fragile cross-project relative include.
- `PokerCheat::ProbeTableStruct()` (new, in `PokerCheat.cpp`) -- finds
  poker_sp's thread, reads local slot 4810 as the hypothesized table
  struct pointer, and logs `f_2` (expect 1-3, stakes tier) and `f_485`
  (expect 2-6, seat count) read from it at byte offsets `2*8` and
  `485*8`. Wired to a new F10 menu item, "Probe Table Struct (see log)".

### First empirical run: hypothesis wrong, root-caused, fixed (still untested)

Ran the probe in-game (user at a real poker table). Result:
`uScriptParam_0 (slot 4810) = 0x21100000020`, `f_2 = -14475230`,
`f_485 = -13684430` -- clearly garbage (not remotely 1-3 / 2-6).

Root cause, found by tracing real call sites instead of trusting
parameter-name continuity (the user's prompt to "look for known good
values, hashes that are very unique" is what triggered re-deriving this
properly rather than iterating on more guesses):
- Grepped every use of `uScriptParam_0` in the file -- it appears exactly
  once beyond its declaration, passed into `func_3` (a generic "scene
  sequencer" framework function, reused boilerplate, not poker-specific).
  **It is not a pointer we dereference to reach Table** -- that was the
  bug. Confirmed by `func_67` (called from inside func_3, receiving
  `&uScriptParam_0` as its own `uParam1`), which does `uParam1->f_12`,
  `uParam1->f_13`, `uParam1->f_2` -- i.e. **slot 4810 is used as the BASE
  of an inline struct** ("LaunchArgs"), not a pointer whose *contents*
  need reading first. The original probe read `*(slot 4810)` and treated
  that as a pointer -- one indirection too many.
- Traced Table's real origin via two direct, unambiguous call sites
  instead: `main()` -> `func_101(&(uLocal_14.f_114))` (so func_101's own
  param IS `uLocal_14.f_114`) -> func_101's body calls
  `func_276(&(uParam0->f_287))` where `func_276` is confirmed (by its own
  body: zeroes f_2..f_6, resets 6 seats' `f_39[i]`/`f_376[i]`, zeroes
  `f_485`, resets `f_15`) to be **the** Table reset function. So
  **Table = `uLocal_14.f_114.f_287`**, real local slot
  `14 + 114 + 287 = 415` -- again, inline data reached by plain address
  arithmetic on the thread's local array, no pointer dereference.
- One open ambiguity: `func_101` also resets a second same-shaped struct
  at `f_114.f_1276` (via `func_285`, also calling `func_276`) -- slot
  `14+114+1276=1404`. Not yet known which is the "live" struct read
  during actual play vs. a secondary/backup copy.

This also surfaces a broader lesson worth keeping: **don't trust a
struct pointer's identity just because two functions both use a
parameter named `uParamN` with similar-looking field access** -- the
decompiler reuses generic parameter names per-function; only an actual
traced call site (who calls this function, with what argument
expression) proves two "uParam0"s are the same underlying object. Most
of Session 2's original struct model was built by trusting field-shape
similarity across functions rather than tracing real call sites, which is
exactly how the `uScriptParam_0` mistake happened.

`PokerCheat::ProbeTableStruct()` rewritten accordingly (builds clean,
deployed, still not run against the game yet):
- Reads `LaunchArgs.f_12` (slot 4822) and checks for an **exact** match
  against one of four specific hash-like constants seen hardcoded in
  `func_67`'s stakes-tier switch (`-1150372370`, `355424894`,
  `-471827042`, `-2033178055`) -- a coincidence-probability check on the
  order of 1-in-4-billion if it matches, versus the old "is this in
  [1,3]" check that could easily false-positive on unrelated memory. This
  also directly validates whether slot-direct (no-dereference) addressing
  is the right mental model at all, independent of the Table hypothesis.
- Logs both Table candidates (slot 415 and slot 1404) side by side, each
  showing `f_2`/`f_485`.

**Still not run against the actual game.** Next concrete step: launch
RDR2, sit at a poker table, F10 -> "Probe Table Struct", check
`PokerCheat.log`. If the LaunchArgs hash check matches exactly, the
addressing model is confirmed and whichever Table candidate shows sane
`f_2`/`f_485` is very likely correct. If it doesn't match, the
addressing model itself needs rethinking (e.g. thread->m_Stack might not
be indexed the way HorseMenu's `ScriptLocal` assumes for *this* build, or
`m_Context.m_FramePointer` needs to factor in rather than a flat 0-based
index -- worth checking `scrThreadContext::m_FramePointer` if slot-direct
addressing itself turns out wrong, not just the specific slot numbers).

### Practical implication for the mod design

Given the hand evaluator and the equity simulator are both single native
calls (`MINIGAME::_0x32A7C216344D623B`,
`MINIGAME::_0xEC819D612038EF4B`) that Rockstar's own code already calls
with exactly this data, the advisor feature doesn't need HorseMenu-style
bytecode patching at all -- once we can read the table struct (open
question #1) and know the board struct's real layout (#2), the whole
feature is: read hole cards for every seat + the currently revealed board
+ (optionally) call the equity native ourselves for the human seat, and
render it. `ScriptPatches`/`BytePatch` stay relevant only if we later
decide to force an outcome rather than just advise -- kept as a reference,
not currently the plan.

### Session 3 -- Table slot math independently confirmed against real bytecode

User extracted the real `script_rel.rpf` via OpenIV (after RDR2-RPF-Tool,
rpf-rs, and CodeWalker all failed on it for various reasons -- wrong game,
no RDR2 crypto, or a decryption tag (198) not in the local tool's key
table; none of those gaps got solved, OpenIV just sidestepped the whole
problem). Real `.ysc.full` files now live at
`D:\Backup\Stuff\RDR2 Shit\SciptsCompile\script_rel\`.

Built `Githubs\GTA-V-Script-Decompiler` (`dotnet build`, .NET 9, has a CLI
mode via `Options.cs`/`CommandLine.Parser` -- `-n -v <file>` decompiles
one file headlessly, no GUI needed). First attempt failed on *every* file
including trivial ones (`"Return expected"`, an AggregateException from
`ScriptFile.cs`'s statement-tree builder) -- root cause:
`Properties.Settings.Default.IsRDR2` is a GUI-only checkbox
(`MainForm.cs`'s `isRDR2ToolStripMenuItem`) with **no CLI flag**, gating
RDR2-specific opcode/header parsing throughout `Instruction.cs`,
`ScriptFile.cs`, `Function.cs`, `NativeDB.cs` -- default `False` (GTA5
mode), so the CLI was silently parsing RDR2 bytecode with GTA5 semantics.
Fixed by flipping the compiled-in default to `True` in three places
(`App.config`, `Properties\Settings.settings`,
`Properties\Settings.Designer.cs`'s `DefaultSettingValueAttribute`) and
rebuilding -- no runtime flag needed after that.

Also independently verified (by reading `Ast\Offset.cs`,
`VariableStorage.cs`, `Function.cs`'s `GetFrameVarName`) that this
decompiler's `.f_N` field-access chains really are meant to compose
**additively back to one absolute index** (`OffsetLoad.GetGlobalIndex() =>
value.GetGlobalIndex() + offset`, recursive) -- the model this project's
slot-math has used since Session 2 was correct in principle, this wasn't
a case of "these are relative, not absolute" as originally worried.

With `IsRDR2=True`, decompiled the user's genuine `poker_sp.ysc.full`
directly and diffed the result against the community
`rdr2-scripts-decompiled` text this whole project has been citing:
**exact match**, line-for-line, on every point checked -- last declared
local (`uLocal_4809`, so `uScriptParam_0` = slot 4810, confirming
`kLaunchArgsSlot` in `PokerCheat.cpp`), and the exact call chain Table's
derivation rests on (`func_101(&(uLocal_14.f_114))` at line 5175,
`func_276(&(uParam0->f_287))` at line 6992 -- identical line numbers too).

**This closes out the "is the slot math even right" question for good --
it's confirmed against real compiled bytecode, not just decompiler
pseudocode inference.** `kTableSlotA` (uLocal_14.f_114.f_287, slot 415) is
correct. The two live-probe runs that read `f_2`/`f_485` as `0, 0` are
now almost certainly explained by **timing** -- `func_101` (Table's own
init function) simply hadn't executed yet at the moment probed, not a
wrong address. The `kF114SeatIndexSlot` check already added to
`ProbeTableStruct()` (reads `f_114.f_9`, expect -1 once func_101 has run,
0 if not) was built for exactly this and hasn't been tested yet -- next
concrete step is running the existing probe again, but with an actual
hand dealt (cards visible on screen) rather than just seated at the
table, and checking that field specifically.

### Table struct fully confirmed live, hole-card reading working end-to-end

Ran the probe with a hand dealt. `f_114.f_9` read `3` (a real seat index,
not the `-1` reset default or `0` compile default) -- `func_101` had
definitely run, yet Candidate A (`f_287`) still read `f_2`/`f_485` as
`0, 0`. Rather than keep re-deriving statically, dumped a wide raw window
(slots 405-455, offsets -10..+40 around Table's presumed base) and read
it directly -- this settled everything:

- `f_+15` (slot 430) = **11**, `f_+16..+37` (22 words) = **-1**,
  `f_+38` = **0** -- exactly matches `f_15` (the board struct): an
  11-card-slot array (2 words/card, all empty) followed by its
  reveal-count field, landing exactly at the predicted offset.
- `f_+39` (slot 454) = **6** -- exactly matches `f_39` (the seats array)
  landing at its predicted offset, holding the real seat count.

**This is what actually proves Table's base (slot 415) is correct** --
`f_2`/`f_485` were simply never the fields I thought they were (I'd
misattributed which function's "uParam1" those specific reads belonged
to back in Session 2); it doesn't matter now that real landmarks are
pinned down. The pattern also revealed a convention missed until now:
**arrays here carry a leading size/count word before the element data**
(`f_15`'s header=11, `f_39`'s header=6) -- matches
`RDR-Classes\script\types.hpp`'s `SCR_ARRAY::Size` field, and explains
the "+1" in HorseMenu's `ScriptLocal::At(offset, size)` that went
unremarked-on when first read. So seat data starts at
`Table + 39 (f_39 header) + 1 = Table+40`, each seat `56` words
(`f_39[i /*56*/]`), and within a seat, hole cards live at
`seat_base + 7 (f_7 header) + 1 = seat_base+8`, 2 words per card
({rank, suit}).

Added a full 6-seat hole-card dump to `ProbeTableStruct()` using this,
marking whichever seat matches the live `f_114.f_9` read as "YOUR SEAT"
(seat index changes every hand -- user sits randomly). **Confirmed
correct twice against the real screen:**
- Run 1: seat showed `{rank=6,...}, {rank=5,...}` -- user had a 5 and 6.
- Run 2: seat showed `{rank=7,suit=1}, {rank=13,suit=3}` -- user had 7 of
  Diamonds and King of Spades. **This also nails the suit encoding**:
  suit 1 = Diamonds, suit 3 = Spades, matching standard bridge/
  alphabetical order exactly (0=Clubs, 1=Diamonds, 2=Hearts, 3=Spades --
  0 and 2 inferred from the ordering, not yet independently confirmed by
  a real dealt card, but very high confidence given 1 and 3 fit exactly).

**Confirmed struct map so far** (all slots relative to Table's own base,
Table itself at absolute local slot 415 = `uLocal_14.f_114.f_287`):
- `Table.f_15` (board): struct at Table+15. `.header`=Table+15 (card-slot
  array size, 11), `.cards[0..10]` = Table+16..+37 (2 words each,
  {rank,suit}, -1 = empty), `.f_23` (reveal count, 0/3/4/5) = Table+38.
- `Table.f_39` (seats): array at Table+39. `.header`=Table+39 (seat
  count, 6), `.seats[0..5]` = Table+40 + i*56 (56 words each).
  - Per-seat: `.f_7` (hole cards) header at seat_base+7, real card data
    at seat_base+8 (2 words), so hole card k (0 or 1) =
    seat_base+8+2k/+8+2k+1 = {rank,suit}.
  - `.f_31` (hand-eval result, category 0-9) still believed at
    seat_base+31 per Session 2's trace -- not yet re-verified against
    real live data the way f_7 was, worth double-checking the same way
    once a hand reaches showdown.

Still open: `Table.f_2`/other small-offset fields' real meaning (stakes
tier etc. -- misattributed, not yet re-derived), `Table.f_376` (pots)
real offset (checked here to be simply Table+376, unverified),
`Table.f_485` (seat-count duplicate?) real meaning. None of these block
reading hole cards / board / calling the hand-eval native, which is
everything the original ask needed -- lower priority now.

### On-screen HUD: cards + hand strength, per user request

User asked to draw opponents' hole cards and current hand strength
on-screen live (not just at showdown, which is when the vanilla UI
normally reveals them), specifically to know when to hold vs. fold.

Added `PokerCheat::DrawOverlay()` (called from `OnTick()` when `Enabled`,
same F10-toggle gate as before): reads all 6 seats' hole cards + the
board using the confirmed struct layout, calls
`MINIGAME::_0x32A7C216344D623B` (the exact native `func_1641` wraps in
the decompile) with pointers to the *real* live hole/board structs so it
sees exactly what the game itself would -- but with a local
`uint64_t[64]` scratch buffer as the output pointer, never the game's own
memory, so nothing gets mutated. Reads the 0-9 category back from output
offset 24 (`.f_24`, confirmed mapping from the stat-tracking switch).
Renders a plain text HUD list, left side of screen: title, one line per
seat (`Seat N: <card> <card> - <hand name>`, marking the user's own live
seat with "(You)"), then a board line respecting the real reveal-count
field (shows "(preflop)" at 0, otherwise exactly that many cards).

Also refactored `PokerCheat.cpp`: the struct-layout constants and
`ReadInt` helper (previously local to `ProbeTableStruct`) are now shared
file-scope constants at the top, used by both the overlay and the probe,
so there's one source of truth for the offsets instead of two copies
that could drift.

Both configs build clean and are deployed. Not yet tested in-game (needs
a live hand + the same visual cross-check against the real screen used
for the hole-card confirmation). Two things to specifically verify once
tested: whether `MINIGAME::_0x32A7C216344D623B` is happy being called
from an unrelated ASI thread's context rather than poker_sp's own
(natives are generally thread-context-agnostic for pure logic/math
natives like this one, no reason to expect an issue, but unconfirmed),
and whether the on-screen text actually renders where expected (normalized
coordinates 0.015/0.30 chosen by eye, not measured against the real HUD).

Not yet done: pot-odds-aware advice (needs the pot/bet fields, still
unmapped) and the equity/win-probability native
(`MINIGAME::_0xEC819D612038EF4B`) for an actual bet/fold/double-down
recommendation rather than just showing hand strength -- next natural
step once the HUD itself is confirmed working.

### Fold/active state, stack/bet display, and a winning/losing verdict

User feedback after seeing the HUD: our seat numbers didn't visibly
correlate with the vanilla HUD's own per-seat picture+chip display, we
weren't distinguishing folded from active seats, and there was no
overall "am I winning" indicator. Traced the actual fields from
poker_sp.ysc.c (ground-truth decompile, not the earlier community text,
same file used for the struct-layout work):

- `seat.f_6` (state): confirmed via three sibling predicate functions --
  `func_912`/`func_475`/`func_476` check `f_6==0`, `f_6==1`, `f_6==2`
  respectively (`f_39[seat] != -1` guards all three, i.e. seat occupied
  at all). `func_475`'s only two call sites (~line 9430) draw a per-seat
  UI status icon in a `func_472 (won) -> func_475 (?) -> func_476
  (all-in)` priority chain passing a chip amount alongside it --
  strongly indicates `f_6==1` is "folded" (0=active, 2=all-in confirmed
  more directly: `func_1093`, the actual DEAL function, sets `f_6=0`
  when dealing a seat in).
- `seat.f_2` / `seat.f_3`: confirmed real (not guessed) via getters
  `func_144`/`func_469` and their only call site (`func_215` /
  the per-seat HUD draw loop at line ~9412) which draws them directly
  onto the vanilla per-seat UI element -- these are exactly the numbers
  the user sees on screen already, so showing them in our own HUD too
  lets the seat numbers be cross-checked by matching the number, without
  needing to solve seat-index-to-screen-position mapping separately.
  (f_2 looks like stack/bankroll, f_3 like current bet, based on how
  they're combined/incremented elsewhere -- not independently confirmed
  against the screen the way hole cards were.)
- Also found, in passing: `func_1093` (the deal function) draws two cards
  each from `Table.f_606` via `func_1543` per dealt-in seat -- `f_606` is
  very likely the actual deck object. Not investigated further this
  round, but directly relevant to the still-open "can house cards be
  predicted before reveal" question from Session 2 -- worth a look if
  that's revisited.

`DrawOverlay()` updated: skips genuinely empty seats (`f_39[seat]==-1`),
labels folded (`[FOLDED]`) and all-in (`[ALL-IN]`) seats, only evaluates
hand category for seats still eligible to win (state 0 or 2 -- folded
seats' real cards are still in memory and still shown, just not
hand-ranked, since a fold means they can't win this pot regardless of
hand strength), and shows `stack`/`bet` on every line for the
cross-referencing described above. Added a bottom verdict line comparing
the user's own hand category against the best ACTIVE opponent's category
-- "You're ahead"/"You're behind"/"too close to call" (category ties
aren't broken by kicker yet -- would need more of the hand-eval native's
output struct than just the category word, not just offset 24).

Both configs build clean, deployed. Not yet re-tested in-game against
this specific update.

### Real kicker comparison -- func_1541 replicated exactly

User asked why a tied category (pair of 3s vs. pair of 3s) always seemed
to go the AI's way, and what actually decides a kicker. Answer: nothing
did on our end yet -- the verdict line was only ever comparing the 0-9
category number and reporting an exact tie as "too close to call". The
AI winning wasn't suspicious, our tool just hadn't implemented the
tiebreak.

Found `func_1541` in the ground-truth decompile (line ~50232) -- this is
poker_sp's own hand comparator, and it revealed something not obvious
from the category field alone: **the hand-eval native's output buffer
starts with up to 5 "kicker" cards** (words 0,2,4,6,8 -- same {rank,suit}
pairing as hole/board cards elsewhere), with word 23 saying how many of
those 5 are actually valid, before word 24's category. `func_1541`'s own
logic: same category -> compare those kickers highest-first, first
difference wins; different category -> higher category wins outright.
This is exact, not a heuristic -- we already had this data from every
`MINIGAME::_0x32A7C216344D623B` call, just weren't reading past offset 24.

Replicated it as `CompareHands(a, b)` in `PokerCheat.cpp`, byte-for-byte
matching `func_1541`'s own comparisons. `EvaluateHand()` changed to take
a caller-provided output buffer (`kHandEvalBufWords`=64) instead of a
throwaway local one, so the buffer survives long enough to compare
against. `DrawOverlay()` now evaluates the user's own hand once up
front, then for every active opponent calls `CompareHands()` against it
directly and tags that seat's line `[you win]`/`[they win]`/`[tie]`, and
the bottom verdict line reports the real worst-case outcome ("You're
ahead of everyone" / "Tied with at least one opponent" / "You're behind
-- someone beats you") instead of the old category-only guess.

Compiles clean (verified), but RDR2 was running at the time so the
deploy copy step failed as expected (file in use) -- needs the game
closed to actually get the updated `.asi` onto disk, not yet re-tested
in-game against this specific change.

### The deck is deterministic -- found the shuffle, future cards are readable

User asked directly: is the next card predictable, is it deterministic.
Chased this down properly instead of relying on Session 2's guess (which
was wrong -- see below).

Found in the ground-truth decompile:
- `func_589` (deck init, called from `func_589(&(uParam0->f_606))`):
  builds a plain 52-card array (4 suits x ranks 2-14) directly at the
  deck struct's own base -- **no leading size/count header word** the
  way `f_15`/`f_39` have (not every array here uses that convention, has
  to be checked per-field). Sets `f_105=0` (draw cursor), `f_106=52`
  (count).
- `func_1195` (the actual shuffle): a real Fisher-Yates-style shuffle,
  **5 full passes** of `swap(card[i], card[GET_RANDOM_INT_IN_RANGE(0,
  count)])`, using the game's own native RNG. Runs once at hand start.
- The flop/turn/river reveal function (~line 39590): draws exactly
  3/1/1 cards depending on street, straight off this same deck array via
  `func_1543` (reads `card[cursor]`, increments cursor) -- **no burn
  cards**, cards go directly onto the board and into every active seat's
  incrementally-updated hand eval.

**So: yes, fully deterministic, and readable in real time.** The entire
remaining deck order is fixed and sitting in memory from the moment the
hand starts (shuffle happens before any card is dealt) -- reading ahead
of the cursor is exact, not an estimate. This directly overturns Session
2's "future board cards probably aren't predetermined" guess, which was
inferred indirectly (the AI's equity native uses Monte Carlo simulation,
which I took as evidence the future wasn't fixed) -- wrong inference: the
AI's own decision-making deliberately doesn't look at its own future
cards even though they already exist in memory, the same way a real
player estimates rather than cheats. Lesson: that earlier conclusion was
never actually traced to a real deal/shuffle function, it was inferred
from adjacent evidence -- should have been flagged as unconfirmed more
clearly at the time rather than stated as a finding.

Confirmed struct location: `Table.f_606` (absolute slot
`415+606=1021`), card array at deck+0 (52 x 2 words, slots 1021-1124,
`{rank,suit}`), cursor at `f_105` (slot 1126), count at `f_106` (slot
1127).

Added to both `DrawOverlay()` (an "Upcoming: " line showing exactly
enough cards to complete the board, i.e. `5 - revealCount` cards read
starting at the cursor) and `ProbeTableStruct()` (logs cursor/count plus
the next 8 undrawn cards, for verifying this against the real screen the
same way hole cards were confirmed earlier -- reveal a street, check the
log's *previous* prediction for that position matched what actually
came down). Both configs build and deploy clean. Not yet verified
in-game against this specific change.

### Predicted-final-board evaluation -- outcome known from preflop

User pointed out the "Board:" line correctly showing "(preflop)" wasn't
actually delivering what they wanted -- hand strength/verdict were still
only being computed against the REAL (mostly-empty, preflop) board, not
using the deterministic-deck finding to predict the actual outcome.

Added `BuildPredictedBoard()`: constructs a synthetic board buffer in the
same layout the native expects (header word, 11 {rank,suit} slots,
reveal-count at offset 23) by combining the REAL revealed cards (for
however many are actually out) with PREDICTED cards read straight off
the deck starting at the cursor for the rest, and always sets the
reveal-count to 5 -- forcing the hand-eval native to evaluate every
hand's actual FINAL 5-card board, not just what's currently shown. Both
the user's own hand and every opponent's are now evaluated against this
predicted board (`predictedBoardPtr`), not the live one -- `EvaluateHand`
calls updated accordingly, `revealCount`/`deckCursor`/`deckCount` moved
up to the top of `DrawOverlay()` so the predicted board can be built
before the per-seat loop runs.

Title line and verdict wording updated to make the predictive framing
explicit ("PokerCheat (predicted final hands)", "You WILL win/lose at
showdown") so it doesn't read as just "who's ahead right now" -- the real
"Board:" (currently revealed) and "Upcoming:" (remaining predicted
draws) lines are kept as-is for reference/sanity-checking, unchanged by
this.

Both configs build and deploy clean. Not yet verified in-game.

### HUD styling investigation -- font matching isn't possible, improved what is

User (stepping away for a while) asked whether the overlay could be made
to draw like the game's own UI instead of a generic font that doesn't
mesh with RDR2's look.

Checked for a font-selection native across three independent sources:
the stock SDK's `natives.h`, `Decompiler\rdr3-nativedb-data\natives.json`
(independent RDR2-specific reversal), and the decompiler's own bundled
`natives_rdr.json` -- **no `SET_TEXT_FONT` (or equivalent) in any of
them.** The only text-style natives that exist at all:
`SET_TEXT_SCALE`, `SET_TEXT_COLOR_RGBA`, `SET_TEXT_CENTRE`,
`SET_TEXT_DROPSHADOW`, `SET_TEXT_RENDER_ID` -- no font, no edge/outline,
no justification. Cross-checked against poker_sp.ysc.c itself: it never
calls `UI::DRAW_TEXT` for its own seat/chip/card HUD elements at all --
those go through `UISTATEMACHINE::`/`DATABINDING::` natives (e.g.
`_UIFLOWBLOCK_REQUEST`, `_DATABINDING_ADD_DATA_STRING`, seen earlier
this session), a Scaleform-style prebuilt UI-flow/widget system
completely separate from the simple legacy `UI::DRAW_TEXT` path our
overlay uses (the same path `scriptmenu.cpp`'s F10 menu was already
built on, inherited from the GTA5-era SDK sample).

**Conclusion: matching RDR2's real in-game font isn't achievable via any
exposed native** -- the legacy draw-text path apparently only ever has
one fixed font, and actually replicating the native look would mean
reimplementing/driving the real UI-flow widget system, not a reasonable
scope for this. Real per-glyph rendering worked for it in Session 4's
research; a plain `UI::DRAW_TEXT` call block just can't be persuaded to
render in RDR2's western-stencil font, full stop -- this is a hard
platform constraint, not something more digging will find a way around
unless a genuine `SET_TEXT_FONT`-equivalent hash surfaces somewhere none
of these three sources have it.

What's actually achievable and was implemented instead, in
`PokerCheat.cpp`:
- `DrawPanel()`: a solid dark backing rect (`GRAPHICS::DRAW_RECT`, same
  signature/convention `scriptmenu.cpp`'s own `DrawRect` already uses --
  confirmed against the real native signature in `natives.h`, not
  guessed) behind the whole HUD block, drawn once before any text.
  Sized generously for the max possible content (title + 6 seats + board
  + upcoming + verdict = 10 lines) since the real count varies with how
  many seats are occupied and isn't known until after the per-seat loop
  runs.
- Real drop shadow (`SET_TEXT_DROPSHADOW(1, 0, 0, 0, 200)`) -- it was
  previously called with all-zero params, which just disables it
  entirely; text had no shadow at all before this.
- Warm parchment/cream text color (`235,222,194`) instead of pure white,
  with a brighter gold title line, closer to RDR2's actual HUD palette
  than plain white-on-nothing.

Both configs build and deploy clean. Not yet visually verified in-game
(no screenshot capability available this session to check it directly;
needs the user to actually look at it).

### Card face textures traced -- real 2D texture, but a suit-mapping conflict needs an in-game test to resolve

Follow-up to the font investigation: user clarified they meant the 2D
card icons shown for community cards during the round and for everyone's
hands at showdown, not the 3D props on the physical table.

Traced this end to end in the ground-truth decompile: `func_628` (the
exact same raw hole-card getter `PokerCheat.cpp` already uses --
`Table.f_39[seat].f_7[which]`) feeds directly into `func_1325` ->
`func_1599` with **no remapping in between** (confirmed by reading the
actual call site, `func_1325(&(uParam0->f_2979), uParam0->f_114.f_2528,
0, func_628(uParam1, uParam0->f_114.f_2528, 0))` at line ~28771).
`func_1599` builds a texture NAME as `"<SUIT>_<RANK>"` (e.g.
`"DIAMONDS_7"`, `"HEARTS_A"`) inside a texture DICTIONARY named
`"card_set_N"` (`func_925` builds that name; N depends on which
table/location skin is active via `func_330`/`func_331`, not replicated
-- instead `FindLoadedCardSetDict()` just probes `card_set_1..8` for
whichever one the game itself already has streamed in, since it must
already be showing its own cards).

**Real conflict found, not yet resolved**: `func_1599`'s own suit switch
is `0=Hearts, 1=Diamonds, 2=Spades, 3=Clubs`. Agrees with our suit-1
finding (Diamonds, matches the real 7 of Diamonds confirmed earlier) but
disagrees on suit 3 -- our earlier empirical test (a real King of
Spades, confirmed directly on screen) says suit 3 = Spades, this code
says 3 = Clubs. Both are individually well-supported (one is a doubly
-verified real-screen observation, the other is traced through
unmodified real game code with no remap step found) -- genuinely
unresolved, not a case of one obviously being wrong.

Added a TEST ICON to `DrawOverlay()` (top-right corner, clearly labeled,
separate from the main text HUD): draws the real `card_set_N` texture
for the user's own first hole card using `func_1599`'s suit mapping.
Comparing this rendered icon against the real card in hand settles the
conflict directly -- whichever suit the icon actually shows for a known
card is the correct mapping. `BuildCardTextureName()`,
`FindLoadedCardSetDict()` added; both `GRAPHICS::DRAW_SPRITE` and the
`TEXTURE::REQUEST_STREAMED_TEXTURE_DICT`/`HAS_STREAMED_TEXTURE_DICT_LOADED`
pair confirmed present with matching signatures in the stock SDK's
natives.h before use, not guessed.

Both configs build and deploy clean. Not yet tested in-game -- this is
explicitly a diagnostic/test addition, not a finished feature, pending
the user comparing the drawn icon against a real card.

### Test icon confirmed suit 0=Hearts; sprite size fix; Board/Upcoming UX fixes

User tested the test icon: drew an Ace of Hearts, confirmed as their
actual real card. This directly confirms `func_1599`'s suit 0=Hearts and
rank 14=Ace are both correct (doesn't touch the still-open suit-3
Spades-vs-Clubs conflict -- that specific card happened to be suit 0,
need a suit-3 card to settle it). Two suits now independently confirmed
correct via real cards (0 and 1), both agreeing with `func_1599` -- makes
it more likely the earlier "King of Spades = suit 3" read from Session 4
was a mistake on this session's part rather than the traced game code
being wrong, though still not proven either way.

Icon rendered "huge" -- cut `DRAW_SPRITE`'s width/height from 0.09x0.14
down to 0.01x0.015 as a first correction (evidently doesn't scale the
same way `DRAW_RECT`'s dimensions do, where 0.22 reads as a normal menu
width) -- unverified, may need further tuning once actually seen.

Separately, user asked why "Board:" still said "(preflop)" -- turned out
to be a UX miss, not a bug: the predicted cards were already being shown
correctly, just on a separate "Upcoming:" line the user didn't connect
to the question. Merged both into one "Board:" line that always shows
all 5 eventual cards -- real ones plain, not-yet-revealed ones read
straight off the deck at the cursor and marked with a trailing `*`.

Follow-up question about the old "Upcoming: (none -- board already
complete)" message surfaced a real flaw worth fixing on its own: that
text conflated two genuinely different situations -- the board actually
being fully revealed (revealCount>=5, normal) vs. the deck
cursor/count reading as out of range (would mean the deck isn't
shuffled/ready yet, a real problem) -- under one generic label, and the
new merged line would have made this WORSE by silently truncating with
no explanation at all. Fixed: the merged "Board:" line now only appends
a diagnostic suffix ("(deck not ready -- cursor/count out of range)")
when cards were skipped for a reason OTHER than genuine full reveal,
distinguishing the two cases instead of hiding which one happened.
Still don't know which case the user actually saw when they first
noticed it (didn't have a saved log from that exact moment to check) --
if it recurs, the new message will say plainly which situation it is.

Game closed mid-session; all of the above (sprite resize, Board/Upcoming
merge, deck-not-ready diagnostic) built and deployed together, both
configs clean. Not yet re-tested in-game against these specific changes.

### Real-money bug: wrong verdict cost the user a hand

Test round: "(deck not ready)" persisted across multiple hands (not a
one-off), icon now "super tiny" (0.01/0.015 overcorrected from
0.09/0.14), and separately -- more seriously -- **the verdict told the
user they would win, and they lost.** Since the whole predicted-board
feature depends on deck reads being right, these are very likely the
same root cause: if the deck cursor/count read as invalid, the
predicted board built for hand-eval got filled with placeholder -1
cards for the unrevealed slots instead of real future ones (see
`BuildPredictedBoard()`'s fallback branch), which would make the
verdict wrong -- not a probabilistic miss, an actual bug feeding bad
data into a claim stated as certain.

Immediate mitigation: softened verdict wording ("Currently predicted to
win/lose... deck read unverified -- don't trust yet") rather than
"WILL win/lose", so the tool doesn't overstate confidence while this is
broken. Icon size split the difference (0.035/0.05) between "huge" and
"tiny".

Re-examined the source for the root cause rather than re-guessing:
`func_589` (deck init) is called from **two different places** -- line
10760 (context not yet traced) and line 25594 (paired directly with
`func_1195`, the actual shuffle, at line 25595). That's suspicious --
same shape as the original Table-location ambiguity (two same-shaped
structs, `f_287` vs `f_1276`, both reset by the same function) that
turned out to matter. Possible this is the same situation again: maybe
Candidate A (`f_287`, what `kTableSlot` uses) doesn't hold the *shuffled,
gameplay* deck, and Candidate B (`f_1276`) does, or vice versa -- despite
Candidate A's `f_39`/`f_15` reads being solidly confirmed correct via
real cards multiple times, which rules out Table's own base being wrong,
but doesn't by itself prove `f_606` specifically is being read from the
right instance.

Added to `ProbeTableStruct()` rather than guessing further: Candidate
B's deck cursor/count/next-8-cards logged the same way Candidate A's
already is, for direct side-by-side comparison; and a raw window dump
(offsets 95-115 relative to Candidate A's deck base) to check whether
the real cursor/count fields are just offset by a small amount rather
than the whole candidate being wrong -- same technique that pinned down
Table's own base originally.

Both configs build and deploy clean. **Next concrete step: user runs
"Probe Table Struct" while actually in a hand (cards dealt) and shares
the log** -- this is the one piece of real data needed to actually fix
this instead of guessing further. Until then, the predicted-board
feature (verdict, per-seat predicted hand strength, the `*`-marked
future board cards) should not be trusted, even with the softened
wording -- the underlying read may still be wrong.

### Root cause found: the real deck lives on Candidate B, not Candidate A

User asked when the deck actually shuffles -- chasing that traced both
`func_589` call sites to their enclosing functions instead of guessing,
which resolved the whole bug:

- Line 10760's `func_589` call (deck init, no shuffle) is inside
  `func_285`.
- Line 25594-25595's `func_589`+`func_1195` pair (init then immediately
  shuffle) is inside `func_590`.
- **Both confirmed, via their own call sites, to operate on Candidate B
  (`f_1276`)**: `func_285(&(uParam0->f_1276))` at line 7065 (the exact
  same Candidate-B reset function identified back in Session 2/3), and
  `func_590(uParam0)` called from directly inside `func_285` with that
  same `uParam0` (still Candidate B) a few lines later.

So the real, shuffled, gameplay deck has been on Candidate B this whole
time -- `kDeckSlot` was pointing at Candidate A (`f_287`), which never
gets its deck shuffled at all (only the unshuffled `func_589` init, no
`func_1195` anywhere in Candidate A's own setup path). That's the direct
cause of the persistent "(deck not ready)" message and, very likely, the
wrong "you will win" verdict from the hand that was actually lost --
reading from the wrong struct wouldn't reliably produce garbage-looking
values (an unshuffled-but-initialized 52-card array can still pass an
in-range cursor/count check), it would just produce the WRONG cards,
silently.

Fix: `kDeckSlot` now built from `kTableSlotB` instead of `kTableSlot`.
Hole cards, board, seat states, stack/bet all keep reading from
Candidate A as before (confirmed correct multiple times against the
real screen) -- only the deck-specific reads moved. `ProbeTableStruct()`
still logs both candidates' deck data side by side (now correctly
labeled) so this can be verified empirically rather than just trusted
on the strength of the trace.

**Answering the user's literal question** (when does it shuffle): the
shuffle (`func_1195`, inside `func_590`) is reached via `func_285`,
which is called once from `func_101` -- `func_101` is the table's
one-time setup (traced back in Session 3 as running once when the local
player's `f_114` state gets initialized), not something that re-runs
per hand. Combined with `func_1543`'s own exhaustion check
(`f_105 >= f_106`) existing at all, this points to the deck being
shuffled once when the table/session starts and drawn from continuously
across multiple hands until exhausted (matching how a real deck is used
at an actual table) -- not reshuffled fresh every single hand. Have not
found the specific reshuffle-on-exhaustion code path, if one exists.

Both configs build and deploy clean. This is a real, well-supported fix
(not a guess re-tried) but still unverified in-game -- next step is the
user testing again and confirming Candidate B's logged cursor/count
actually looks like a valid live deck (count=52, sane cursor) and that
predictions start matching real outcomes.

## Session 5: deck fix confirmed; suit mapping unified; verdict restored

User ran Probe Table Struct after the fix. Log (15:59:00) shows:

```
Candidate A (f_287) deck cursor=-1, count=-1 -- expected NOT to look like a valid live deck now
Candidate B (f_1276, now used for all deck reads) cursor=8, count=52 (expect count=52)
```

Confirmed: Candidate B reads a real, in-range, 52-card live deck.
Candidate A correctly reads as dead/uninitialized now that it's no
longer being (mis)used. The fix holds.

Two follow-ups, both now done:

1. **Suit-mapping conflict resolved.** `SuitLetter()` (used by the text
   HUD and the deck log) previously used a guessed mapping (0=Clubs,
   1=Diamonds, 2=Hearts, 3=Spades) based on an ambiguous early card
   read. `BuildCardTextureName()` (traced directly from func_1599, real
   game code, no remapping) uses 0=Hearts, 1=Diamonds, 2=Spades,
   3=Clubs. The TEST ICON experiment settled it: suit 0 drew a real Ace
   of Hearts on screen (user-confirmed), directly disproving
   `SuitLetter()`'s suit-0=Clubs guess. `SuitLetter()` now uses
   func_1599's mapping too, so the text HUD and the card icon agree.
   The earlier "King of Spades = suit 3" read was a misread; suit 3 is
   Clubs.

2. **Verdict wording restored to confident** ("Predicted to WIN/LOSE/CHOP
   at showdown"), now that the deck-read bug behind the "it lied to me"
   incident is fixed and confirmed. The softened "unverified -- don't
   trust yet" wording served its purpose during the investigation and
   is no longer accurate.

Both configs (`Debug`/`Release`) rebuilt clean; deploy copy for Release
was blocked only because RDR2.exe was still running (expected,
harmless -- Debug's copy succeeded since the game wasn't holding that
file locked at that moment). Needs a deploy with the game closed to
take effect in-game.

Remaining open items: verify the card icon sprite size (currently
0.035/0.05, untested since the last resize), and wire up the equity
native (`MINIGAME::_0xEC819D612038EF4B`) plus pot/bet fields for real
bet/fold/double-down advice (currently only a win/lose/chop verdict,
not stake-aware guidance).

## Session 6: the REAL bug -- deck card elements were off by one word

Session 5's fix (reading the deck from Candidate B) was necessary but
not sufficient. User played a hand after that fix and got the exact
same "predicted win, actually lost" result again. Rather than re-guess
a third time, added a `PredictionCheck` diagnostic: snapshot the
predicted final board the moment a hand starts, then automatically diff
it against the real board once revealCount hits 5, logged as
MATCH/MISMATCH with both card lists.

First result: **MISMATCH**, and worse -- the predicted cards were
largely garbage (`??`, unrecognized rank/suit), e.g. `predicted [?? ??
?? ?? 2? ] vs real [7H 8H 10H 8S QC ]`. Garbage output (not just wrong
cards) meant a real indexing bug, not bad luck or a comparator issue --
`func_589` (deck init) writes valid `{rank 2-14, suit 0-3}` pairs into
all 52 slots before `func_1195` (shuffle) ever runs, and the shuffle
only swaps whole pairs, so every slot should always hold a valid card.

Settled it empirically instead of re-deriving on paper: extended
`ProbeTableStruct()` to dump a raw 119-word window around `kDeckSlot`
(offsets -4..+114) as plain integers, then cross-referenced it against
that same probe run's already-dealt, screen-confirmed hole cards (seat
0: {8,1},{13,0}; seat 1: {6,3},{10,3}; seat 4: {11,1},{13,1}; seat 5:
{11,3},{7,2}) -- since dealt hole cards are literally `deck[0..cursor-1]`
in dealing order, their known values pin down the deck's true per-card
offset with zero ambiguity. Result: all 8 known cards appear starting
at `kDeckSlot+1` (card 0's rank), 2 words apart, in exact
2-consecutive-cards-per-seat dealing order -- not at `kDeckSlot+0` as
the code assumed. **The deck array has a 1-word field before the card
data** (`kDeckSlot+0` itself reads 52 -- the same value as the count
field at `+106`, likely a leftover/adjacent field, not investigated
further since it doesn't matter for reading cards correctly). Cursor
(`+105`) and count (`+106`) were already correct and untouched by this
fix -- only the per-card element formula was wrong.

Fix: added `kDeckCardsBaseOffset = 1`, applied to all three places that
compute a card's address from a deck index (`BuildPredictedBoard()`,
the "Board:" line's per-card loop, `ProbeTableStruct()`'s "next 8
undrawn cards" loop) -- rank now at `kDeckSlot + 1 + index*2`, suit at
`+2 + index*2`.

This is a stronger fix than Session 5's: that one was traced correctly
but never independently verified byte-for-byte before being called
done, which is exactly why it didn't catch this second, unrelated bug
stacked on top. This one comes with an exact, duplicate-free 8/8 match
against known real cards, not just a plausible trace. Still, given the
history here (two consecutive "confident but wrong" verdicts), **don't
treat this as done until a live `PredictionCheck` run logs an actual
MATCH** -- that diagnostic stays in place specifically to keep this
honest rather than declaring victory on code review alone.

Both configs build and deploy clean. Next concrete step: user plays a
hand to showdown, check the log for a `PredictionCheck: showdown --
... MATCH` line.

User confirmed: "The prediction is working perfectly."

## Session 7: community card icons at their real screen position

User asked whether the community-card icons could be drawn at the same
place the real game draws them, instead of a guessed HUD position.
Traced it: community cards during play (as opposed to the showdown
reveal, which is pure Scaleform UI via
`DATABINDING::_DATABINDING_WRITE_DATA_STRING` with no accessible
coordinate) are real 3D props -- `func_471` `CREATE_OBJECT`s one per
board slot and stores the handle at `scene.f_671.f_11[slot]`, where
`scene` = `uLocal_14.f_3310` (a sibling of `f_114`/Table, confirmed via
`func_214`'s 5 call sites all passing `&uLocal_14` as its first arg).
Checked whether opponents' hole cards have an equivalent live prop --
they don't (`func_1093`, the deal function, only writes card data, no
`CREATE_OBJECT`; the one seat-indexed object array, `f_616`, only
populates on fold/win/all-in status changes, not from the initial
deal) -- so this technique is community-cards-only for now.

`ProbeCommunityCardObjects()` (new, F10 menu) tested the traced offset
directly: read the candidate handle, call
`ENTITY::DOES_ENTITY_EXIST`/`GET_ENTITY_COORDS`, project with
`GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD`. First run: off by
exactly one word -- offset+0 read `5` (the array's own header/count,
same leading-size-word convention `Table.f_15`/`f_39` already use, this
time confirmed on `f_671.f_11` too), and the real object handles
started at +1. Fixed (`kCommunityCardObjectsBase` now includes the +1).
Confirmed clean: 3 real, existing objects (revealCount was 3) with
tightly clustered world coords (~0.05 units apart, same Z) and valid
on-screen projections; the not-yet-dealt 4th slot correctly read
handle=0/doesn't-exist.

Wired into `DrawOverlay()`: for each revealed board slot, read its live
object's world position and project to screen every frame (tracks
camera/table rotation automatically, no calibration). For not-yet-
revealed (predicted) slots, no object exists to read -- extrapolated
linearly from the last two known real slots' world-space delta (the
probe data showed consistently spaced deltas, so this should track the
fan layout reasonably well). Needs at least 2 real anchors, so this
only activates from revealCount>=3 onward (flop reveals 3 at once in a
single tick, so revealCount==1/2 as an observed state essentially never
happens) -- preflop still relies on the existing text "Board:" line
with predicted cards marked "*", unchanged. Real cards draw at full
alpha, predicted/extrapolated ones at reduced alpha (140) as a visual
"ghosted" cue, reusing the icon size from the TEST ICON (0.035x0.05,
itself still unverified/untuned).

Deliberately NOT done this session (per user's own prioritization,
chosen over doing both at once): player seat hole-card icons. Since
there's no live 3D prop for those (confirmed above), placing icons at
each seat's info panel would need either the calibration-grid approach
(drawn but not wired -- `ToggleCalibrationGrid()`/`CalibrationGridEnabled`
exist as inert scaffolding from before this pivot) or the user manually
reporting approximate panel positions.

Both configs build and deploy clean. Not yet visually confirmed
in-game -- next step is the user actually seeing the board icons
appear at the real cards' position through a hand.

**Correction**: user clarified they meant a different, 2D top-right
card strip, not the physical 3D cards on the table -- the 3D overlay
was drawing in the wrong place relative to what was actually wanted.
Removed the icon-drawing code from `DrawOverlay()`'s board loop
(reverted to text-only "Board:" line, as before this session's
addition) -- **but explicitly kept as a confirmed, working foundation
for later**, per the user ("could be a nice feature to add"), not
abandoned: `kSceneSlot`/`kCommunityCardObjectsHeader`/
`kCommunityCardObjectsBase` (all empirically confirmed correct) and
`ProbeCommunityCardObjects()` (the F10 diagnostic that proved it) are
still in the code. Whenever 3D-anchored icons (or anything else that
wants the community cards' real world/screen position -- e.g. a
future "look here" marker) comes up again, the hard part (finding the
real object handles and projecting them) is already done.

Traced the ACTUAL target instead: a persistent 2D card-icon strip,
top-right of screen, visible during play (not just at showdown).
Found its driver -- `func_1207` (gated `iParam1 < 0 || iParam1 >= 5`,
i.e. board-slot-indexed, 5 slots) calls `func_1599` (texture name) then
`func_1600`, which is `DATABINDING::_DATABINDING_WRITE_DATA_STRING`
-- same Scaleform/UI-flow mechanism as the showdown reveal
(func_1325/func_1594), confirmed via reading func_1600's body directly.
**This means there's no native-exposed coordinate for this widget
either** -- its real screen position is baked into the game's `.gfx`
movie layout, not computed by script. Unlike the 3D table cards, this
one can't be hijacked; it has to be calibrated by eye.

Added `DrawCalibrationGrid()` (new, wired to F10's "Toggle Calibration
Grid", independent of `Enabled`/poker state): thin lines every 0.05,
labeled every 0.1 along the top and left edges, normalized 0-1 UI
coordinates -- lets the user report real numbers for where that strip
sits instead of more blind guessing. `CalibrationGridEnabled`/
`ToggleCalibrationGrid()` (scaffolded earlier this session, unused
until now) are now actually wired into `OnTick()`.

Both configs compile clean; Release's deploy copy blocked only by
RDR2.exe still running (expected). Next step: user closes the game for
the deploy, then activates the grid in a hand with that top-right card
strip visible and reports its approximate (x,y) and card spacing.

## Session 8: a third "predicted win, lost" report -- narrowed to kickers

User: "it seems to have a problem figuring out when I will win or lose
based on if we have the same cards. I just dumped the hand in which it
said I would win, but I lost. We both had a two pair." Unlike the prior
two incidents, this one comes with a specific, load-bearing detail:
**same category (two pair) on both sides** -- meaning the real board
was correct (already independently confirmed working -- user: "the
prediction is working perfectly") and the category match was correct,
so the outcome was decided entirely by the KICKER comparison
(`CompareHands()`), not the board-prediction machinery those first two
bugs were in. Different bug, same symptom.

No log data exists for that specific hand (already folded/moved on by
the time it was reported), so nothing to diagnose from directly this
time. Rather than guess at `CompareHands()`'s kicker-word interpretation
again, added automatic forensic logging instead: the next time my
category exactly ties an active opponent's category AND the real board
is fully revealed (revealCount>=5), `DrawOverlay()` now logs, once per
hand (`s_kickerDiagLogged`, reset on the same new-hand trigger
`PredictionCheck` already uses), the FULL kicker breakdown for both
hands -- all 5 {rank,suit} kicker pairs plus the count field (word 23)
for each side, plus the `CompareHands()` verdict and cmp value. This
turns "it happened once and I can't reproduce it on demand" into
something that will capture itself automatically the next time the
same situation recurs, without needing the user to catch it live or
remember exact card values.

Both configs build and deploy clean. Next step: user plays until
another category-tied hand reaches a complete board (doesn't need to
be two pair specifically -- any tied category triggers it), then check
the log for a `KickerDiag:` block.

## Session 9: dropped the native entirely -- self-contained hand evaluator

The `KickerDiag:` trap fired for real: seat 0 tied my category (2, two
pair) at a complete board, and the logged kickers were damning --
`count=7` (not 0-5) on both sides, and seat 0's kicker slot read `J?`,
identical to mine, with no Jack anywhere on the board or in seat 0's
actual hand (AS 8H). That's not a wrong-offset guess, it's my own
hand's data leaking into another seat's "independent" evaluation.

Traced the real call convention in poker_sp.ysc.c to settle it for
good instead of guessing an 10th time: `func_1544` (called once per
card as hole cards are dealt and as each board card is revealed, into
the seat's persistent `f_39[seat].f_31`) builds its evaluation
incrementally, one card at a time. There's also a genuine one-shot
call site (`func_1641`, wrapping the same native as
`(holeCards, board, output)` -- the convention this file's `EvaluateHand()`
already used), so the one-shot convention itself isn't wrong -- but
between the two, and the buffer's real internal layout still being
unconfirmed after 9 rounds of guessing, continuing to reverse-engineer
the native's output buffer from the outside wasn't converging.

Sidestepped it entirely. Both real inputs -- each seat's hole cards
(direct memory read, confirmed correct against the on-screen display)
and the predicted final board (confirmed correct via the
`PredictionCheck ... MATCH` log line) -- were already known-good. There
was never a reason to hand them to an opaque native just to get the
category and kicker back; this session replaced the native call with a
self-contained 7-card poker evaluator (`ScoreFiveCards()` /
`EvaluateHand()` / `CompareHands()`, all in `PokerCheat.cpp`): tries
all 21 five-of-7 combinations, scores each with standard poker rules,
keeps the best, and returns a `{category, tiebreak[5]}` that's directly
comparable between any two hands of the same category.

Verified against the exact reported hand (board 4H 7H 7D KH 4D; seat 0
AS 8H, seat 2 8S AD, seat 3/user 9C JD) with a standalone test harness
(`cl`-compiled outside the game, same evaluator code) before trusting
it: seat 0 and seat 2 tie (both two pair 7s/4s, Ace kicker), and both
correctly beat seat 3 -- whose true best kicker turns out to be the
board's own King, not the JD hole card, since an unpaired board card
is fair game as anyone's kicker if their hole cards don't beat it. That
subtlety is exactly why this tries all 21 combinations instead of a
hand-rolled "pair ranks + best hole card" shortcut -- it would have
gotten this specific hand wrong too.

Removed the now-dead `KickerDiag` forensic dump and its
`s_kickerDiagLogged` state along with the native call -- there's
nothing left to reverse-engineer. Both configs build clean; Release
deployed and confirmed via a real run of the reported hand.

## Session 10: release-prep plan agreed; 2D community card icon strip calibrated and wired in

Starting a release-prep pass covering four items: an INI config, making
the HUD auto-show when poker_sp is running (Release builds only --
Debug keeps the F10 menu for development), real community-card icons,
and reworking the text HUD into shorter "tips" instead of the current
full dump. Plan agreed with the user; two design points resolved
up front:
- **Community card icons are two independent, separately toggleable
  features**, not one either/or choice: icons anchored to the real 3D
  table-card objects (`kCommunityCardObjectsBase`, confirmed working in
  Session 7, reverted then for a scope misunderstanding -- not a
  technical dead end) AND a fixed calibrated 2D strip (since the real
  top-right strip widget has no exposed screen position at all --
  Scaleform/DATABINDING-driven, also settled in Session 7). Each gets
  its own config flag once Config.h lands.
- **Cheat tiers**: `CheatLevel` will be Little (community cards only) /
  Lot (+ everyone's hands) / Full Tilt (+ win/lose/chop verdict)
  instead of a plain on/off, config-selected.
- **Auto-run**: Release builds auto-detect `poker_sp` running (the
  `FindScriptThread` check DrawOverlay() already does every tick) and
  draw with no manual step; the F10 menu (manual toggle, probes,
  calibration grid) is compiled Debug-only going forward.

This session implemented the first concrete piece: the 2D calibrated
community-card icon strip, replacing the old single-card "TEST ICON"
diagnostic sprite (its job -- settling the suit-mapping conflict via a
real Ace of Hearts -- is long done, see Session 4/5). Calibration data
came from the user reading the existing `DrawCalibrationGrid()` overlay
(0.05-spaced lines, labeled every 0.1) against the real in-game strip,
using the old TEST ICON sprite (drawn at x=0.90/y=0.14) as a known-close
reference point: the real first card sits about 4 grid boxes in from
the right edge (~x=0.80), and the real second card sits near the 3rd
box from the right but visibly bleeds back into the 4th -- consistent
with one full grid box (0.05) of spacing per card, given the icon's own
0.035 width. New constants in `PokerCheat.cpp`: `kCard2DIconBaseX`
(0.80), `kCard2DIconY` (0.14, carried over from the old TEST ICON y),
`kCard2DIconSpacingX` (0.05), `kCard2DIconWidth`/`kCard2DIconHeight`
(0.035/0.05, also carried over -- still not independently re-verified
since last tuned in Session 7).

New `DrawCommunityCardIcons(ranks, suits, revealCount)` draws all 5
eventual community cards at that strip -- real revealed cards at full
alpha, not-yet-revealed predicted ones (deterministic deck read, same
source as the "Board:" text line and `BuildPredictedBoard()`) ghosted
at alpha 140, matching the alpha convention the old (reverted) 3D-anchor
code used. Reuses `boardRanks`/`boardSuits` -- the predicted-final-board
arrays `DrawOverlay()` already unpacks once up front -- so there's no
duplicate memory read for this. Called right after the "Board:" text
line. The old TEST ICON block (single hardcoded sprite + its two
DrawLine labels) is gone; `FindLoadedCardSetDict`/`BuildCardTextureName`
stay, now used by the new function instead.

Both configs build and deploy clean (game was closed for both). Not
yet visually confirmed in-game -- next step is the user actually seeing
all 5 icons line up against the real strip through a hand, since the
calibration here is still an eyeball estimate, not a measured value (a
second-card overlap into the 4th box, if it looks wrong once seen, most
likely means `kCard2DIconSpacingX` needs a small tweak rather than the
whole base position being off). Config (INI), auto-run/CheatLevel
wiring, the 3D-anchored icon restoration, and the HUD tip redesign are
still open -- next sessions' work, in that order per the agreed plan.

### Live play-testing of the 2D icon strip -- three rounds of tweak, config system added mid-stream

User tested in-game and iterated directly on the 2D strip's numbers:
first "too spaced out" (`kCard2DIconSpacingX` 0.05 -> 0.025), then
"too wide" (`kCard2DIconWidth`/`kCard2DIconHeight` halved together,
0.035/0.05 -> 0.0175/0.025 -- in hindsight height shouldn't have been
touched here, see below), then "need to be twice as tall" -- i.e. the
width-only complaint got over-corrected by also halving height, and
this fixes that specifically (height 0.025 -> 0.05, width left at
0.0175).

Each of the first three rounds meant editing a constant, rebuilding,
closing RDR2, redeploying, and relaunching just to see one number
change -- user asked directly for an INI-based way to tune these
without that cycle, which is exactly the config item from this
session's opening plan, just pulled forward to now instead of done
later.

Added `src/Config.h`/`Config.cpp` (new files, registered in
`PokerCheat.vcxproj`): a thin wrapper over the plain Win32
`GetPrivateProfileString`/`WritePrivateProfileString` API (no vendored
parser needed) backing a `Config::Values` struct. Key subtlety worth
recording: unlike `Log::Write`'s relative `"PokerCheat.log"` (which
works because `fopen` resolves against the process's CWD, which happens
to be the game folder), `GetPrivateProfileString` has a documented
gotcha where a *relative* filename resolves against the **Windows
directory**, not CWD -- so `Config::ResolveIniPath()` builds an
absolute path from this DLL's own module handle
(`GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)` on
an address inside `Config.cpp` itself) instead, landing
`PokerCheat.ini` next to the `.asi` reliably regardless of what the
game's CWD actually is.

`Config::Get()` lazily loads (and `Config::Reload()` force-reloads,
wired to a new F10 menu item "Reload Config (see log)") every key this
session's tuning actually touched: `[HUD] PanelX/PanelY/TextScale/
TitleTextScale` (also previously just eyeballed constants, "chosen by
eye, not measured" per this file's own Session 3 note) and
`[CommunityCardIcons2D] BaseX/Y/SpacingX/Width/Height`. Every read
writes its value straight back (`WritePrivateProfileStringA`), so a
missing/fresh `PokerCheat.ini` self-populates with every current
default the first time the mod runs, and existing user edits round-trip
unchanged -- no separate "write defaults" step needed. `PokerCheat.cpp`
updated to pull all of these from `Config::Get()` instead of `constexpr`
literals; the old hardcoded icon-strip constants are gone.

This means the actual current answer to "how do I tune the icon
strip now" is: edit `PokerCheat.ini`'s `[CommunityCardIcons2D]`
section while the game is running, open the F10 menu, hit "Reload
Config" -- no rebuild, no relaunch.

Both configs (Debug and Release) build and deploy clean. Height fix
(0.025 -> 0.05) not yet visually re-confirmed in-game; config-driven
values not yet exercised via a live Reload Config either -- next step
is the user checking both.

### Config-driven tuning confirmed working end-to-end; a real perf bug found and fixed along the way

User noticed "Toggle Poker Cheat" caused a noticeable game hitch the
first time it was pressed after launching RDR2, but not on any toggle
after that -- asked why, and whether it had already been fixed. It
hadn't (not intentionally, at least) -- root cause: `Config::Reload()`
originally called `WritePrivateProfileStringA` for every single key on
every load, not just when a key was actually missing, so the very
first `Config::Get()` call (lazily triggered by the first `DrawOverlay()`
tick after enabling) did 9 synchronous disk writes back to back --
real, one-time, user-visible hitch, then gone because the config is
cached for the rest of the process's lifetime. Fixed in
`Config.cpp`'s `GetFloat()`: reads with an empty-string sentinel
default instead of the real default, and only writes back when the
returned length is 0 (key genuinely absent) -- a normal Reload() now
does 9 plain reads and zero writes when the file already has every
key, which is the common case after the first run.

Separately, confirmed the actual point of building Config in the first
place: user hand-tuned the 2D icon strip purely by editing
`PokerCheat.ini` and hitting "Reload Config" while the game stayed
open -- no rebuild, no relaunch. Final values, reported as "perfect":

```
[CommunityCardIcons2D]
BaseX=0.821
Y=0.078
SpacingX=0.034
Width=0.03
Height=0.075
```

These now live in the user's own `PokerCheat.ini` (not touched by this
session's source, which only ever supplies the fallback defaults for a
missing file) -- worth noting since they're meaningfully different from
the values this file's earlier "calibrated via grid + TEST ICON"
narrative arrived at (e.g. Y=0.078 vs. the old 0.14, Height=0.075 vs.
0.05), confirming direct in-game iteration beat the grid-based estimate
by a fair margin, not just fine-tuning it.

The `GetFloat` perf fix (Config.cpp) compiles clean in Debug; deploy
copy blocked by RDR2.exe still running (PDB copied, `.asi` didn't) --
needs a redeploy once the game is closed. The icon-position tuning
needed no rebuild/redeploy at all, by design.

Final calibrated 2D icon strip values, confirmed "perfect" by the user
via direct in-game Reload Config iteration (now also PokerCheat.ini's
own defaults and `Config.h`'s hardcoded fallback, so a missing/deleted
ini regenerates these instead of the old grid-estimate numbers):

```
[CommunityCardIcons2D]
BaseX=0.821
Y=0.078
SpacingX=0.034
Width=0.03
Height=0.075
```

### Lag persisted; root cause was worse than the first fix caught -- switched to mINI, and the project became an actual git repo

User reported the hitch on first "Toggle Poker Cheat" was STILL
happening, and separately noticed PokerCheat.ini wasn't appearing on
disk at all when missing. The previous fix (only write a key back when
individually absent) only helps on a file that already exists -- on a
genuinely missing file every one of the 9 keys counts as absent, so
all 9 `WritePrivateProfileStringA` calls still fired, same cost as
before. Worse, that API's writes are cached by Windows without a
guaranteed immediate flush to disk (the documented workaround is an
extra all-null call just to force a flush) -- explaining why the file
didn't show up right after toggling.

User then asked directly whether a "tried and true" INI library was in
use, and proposed switching to mINI
(https://github.com/metayeti/mINI, MIT licensed, header-only,
C++11/17). Agreed -- it reads and writes the whole file in one shot
rather than per-key, which structurally can't hit either problem above,
and it's a better foundation for the non-float config values (bools,
the planned `CheatLevel` enum) still on the agreed release-prep plan.

Per the user's request, made `PokerCheat` an actual git repo first
(previously untracked, `.gitignore` existed but no `.git`) so mINI
could be added as a real submodule rather than a plain vendored copy:
`git init`, an initial commit of the existing tree (133 files, matches
`CollectorOffline`'s own repo convention -- sibling project, same
pattern), then `git submodule add
https://github.com/metayeti/mINI.git external/mINI`.

Rewrote `Config.cpp`/`Config.h` against mINI: `Reload()` now does one
`file.read()` into an `mINI::INIStructure`, resolves each value with a
fallback default the same way as before, writes the resolved set back
with one `file.write(ini, true)` (mINI's "lazy" write -- generates a
fresh file if none exists, otherwise preserves existing
formatting/comments and only touches changed/new keys), and caches the
result the same way `Get()` already did. `ResolveIniPath()` (module-
handle-based absolute path, same reasoning as before -- avoid trusting
CWD) is unchanged. Also added `external\mINI\src\mini\ini.h` to the
vcxproj's `ClInclude` list for IDE visibility, matching how
`RDR-Classes`'s headers are listed.

Compiles clean in Debug (RDR2.exe running at the time blocked only the
`.asi` deploy copy, as usual). Both the git-init/submodule work and the
Config rewrite are committed. Not yet re-tested in-game -- next step is
confirming the first-toggle hitch is actually gone now, and that a
freshly-deleted `PokerCheat.ini` regenerates correctly via mINI.

### Feature toggles replace the tier idea; 2D icon config is Debug-only

User dropped the earlier Little/Lot/Full Tilt tier idea in favor of
three independent booleans, one per feature: `ShowCommunityCards`,
`ShowOthersCards`, `ShowWinPrediction`. Also: the 2D community-card
icon strip's config (`[CommunityCardIcons2D]`) should only be
readable/writable in Debug builds -- Release draws it with the
confirmed "perfect" values baked in as `constexpr`, no INI section for
it at all.

`Config.h`'s `Values` struct: added the three bools (default `true`,
matching current full-featured behavior); wrapped
`Card2DIconBaseX`/`Y`/`SpacingX`/`Width`/`Height` in `#ifdef _DEBUG` so
the struct itself doesn't carry those fields in Release. `Config.cpp`'s
`Reload()` now reads/writes a `[General]` section for the three bools
(`ParseBoolOr`/`SetBool`, new -- accepts `1/true/yes/on` case-
insensitively) and wraps the `[CommunityCardIcons2D]` read/write block
in the same `#ifdef _DEBUG` -- a Release-built `PokerCheat.ini` simply
never gets that section written.

`PokerCheat.cpp`:
- `DrawCommunityCardIcons()` now pulls its 5 geometry values from
  `Config::Get()` only in Debug; Release uses a parallel set of
  `constexpr kReleaseCard2DIcon*` constants (same numbers, just not
  config-backed) behind `#ifndef _DEBUG`.
- Per-seat loop: hand evaluation/comparison (category, `CompareHands`,
  `worstResult`/`anyOpponent`) still always runs for every active
  seat regardless of display settings, since the final verdict needs
  it -- only the *display* is gated. Your own seat is always shown in
  full (not hidden information in the real game). An opponent seat's
  cards/hand name are gated by `ShowOthersCards`; the per-seat
  `[you win]/[they win]/[tie]` tag is treated as its own prediction,
  gated by `ShowWinPrediction` (so it only appears when both
  `ShowOthersCards` and `ShowWinPrediction` are on).
- The "Board:" text line and the 2D icon strip call are both gated by
  `ShowCommunityCards` (skipped entirely, no line reserved, when off).
- The final verdict line is gated by `ShowWinPrediction`.

Both Debug and Release compile clean (RDR2.exe running blocked only
the deploy copy in both, as usual -- Release's single-line
`PostBuildEvent` surfaces that as an MSBuild error where Debug's
two-line one doesn't, but the actual compile step for both produced a
valid `.asi`). Committed as `60562cd` once the user asked for a commit.
Both configs subsequently rebuilt and deployed clean with the game
closed.

### Config now loads eagerly at injection, not lazily on first HUD draw

`Config::Get()`'s lazy-load meant `PokerCheat.ini` didn't exist until
`DrawOverlay()` ran for the first time -- i.e. only after actually
sitting at a poker table with the cheat enabled. User asked for the
ASI to load (or create a default) config right at injection instead,
in both Debug and Release. Added one `Config::Reload()` call at the
top of `ScriptMain()` (`script.cpp`), right after the "PokerCheat
started" log line and before `BuildMenu()` -- runs once as soon as
ScriptHookRDR2 starts the mod's script thread, regardless of whether
poker_sp ever runs that session. `Config::Get()` itself is unchanged
(still safe/idempotent to call before or after this).

Both configs build and deploy clean (game closed for both). Not yet
confirmed in-game that `PokerCheat.ini` now appears immediately on
launch without needing to reach a table first.

User immediately corrected: this needed to run in `DllMain`, not
`ScriptMain` -- `ScriptMain` only starts once ScriptHookRDR2 gets
around to scheduling the registered script thread, which isn't the
same moment as the ASI actually being injected. Moved the
`Config::Reload()` call from the top of `ScriptMain()` (`script.cpp`)
to `main.cpp`'s `DllMain`, `DLL_PROCESS_ATTACH` case, before
`scriptRegister`/`keyboardHandlerRegister` -- this runs as early as
Windows itself calls into the DLL after injection. Confirmed safe to
call this early: `Config::Reload()`'s only work is
`GetModuleHandleExA`/`GetModuleFileNameA` (both fine pre-`scriptRegister`)
and mINI's plain file I/O (`std::ifstream`/`std::ofstream`, no
`LoadLibrary` or cross-thread waits that would risk the DllMain loader
lock).

Both configs build and deploy clean (game closed for both). Not yet
confirmed in-game.

### Both crashes root-caused to mINI's <filesystem> usage; fixed by catching it properly, not by abandoning mINI

RDR2 crashed on injection with the DllMain-based `Config::Reload()`
call above. Windows Event Viewer (`Get-WinEvent` against the
Application log) showed the real evidence instead of guessing: repeated
`RDR2.exe` faults in `ntdll.dll`, exception code `0xc00000fd` --
**STATUS_STACK_OVERFLOW** -- at the exact moment of injection, and
`PokerCheat.log` didn't exist at all afterward (meaning the crash
happened before `ScriptMain`'s very first `Log::Write`, i.e. inside
`DllMain` itself). Moved the call back out of `DllMain` into
`ScriptMain` (a normal script-thread context, no loader lock held) --
this alone fixes the injection crash, matching Microsoft's own
documented DllMain restrictions (DLL_PROCESS_ATTACH holds the loader
lock; anything beyond trivial kernel32-only work risks exactly this
kind of loader-reentrancy crash).

Separately, the user reported an EARLIER incident: pressing "Reload
Config" mid-game (a completely normal context, not DllMain) had shown
a ScriptHookRDR2 "a script error occurred" MessageBox -- the game kept
running, but this is ScriptHookRDR2's own generic handler for an
UNCAUGHT C++ exception thrown from inside the script thread. Two
separate crashes, same `Config::Reload()` function, same underlying
library (`mINI`, which pulls in `<filesystem>`) -- strong evidence
`<filesystem>` itself is the common thread, not merely "called from
DllMain."

First reaction was to drop mINI back to the pre-mINI plain Win32
`GetPrivateProfileString` implementation. User pushed back hard on
this -- correctly: reverting the library doesn't explain *why* it
failed, and Config.cpp is heading toward the fuller feature set from
the original release-prep plan (bools, an eventual `CheatLevel`-style
schema) that the plain Win32 approach is more awkward for. Restored
mINI (`git checkout HEAD -- src/Config.cpp src/Config.h
PokerCheat.vcxproj`, `git submodule update --init external/mINI` after
the local `.git/modules` cache had already been deleted) and actually
read `ini.h` for real throw sites instead of reverting on suspicion.

Checked `metayeti/mINI`'s own GitHub issues first (`gh`/WebFetch,
issues #1-#47) -- nothing matches this failure class, so this needed
tracing from the actual header, not a known upstream bug. Found it:
`INIWriter::operator<<` (`ini.h`, the `write()`/`generate()` path)
calls `if (!std::filesystem::exists(filename))` -- the **single-
argument** overload, which the C++ standard specifies THROWS
`std::filesystem::filesystem_error` for any stat failure other than
"file not found" (permission denied, a sharing violation, a transient
antivirus/indexer lock, etc.), not just returning `false` the way the
two-argument `error_code&` overload would. Nothing in `Config.cpp` had
a try/catch anywhere, so any such transient stat failure became an
uncaught exception -- exactly what both incidents look like.

Fix: split `Config::Reload()`'s body into a private `ReloadImpl()`
(unchanged logic) and wrapped the actual `Reload()` in
`try { ReloadImpl(); } catch (const std::filesystem::filesystem_error&
e) { ... } catch (const std::exception& e) { ... } catch (...) { ... }`,
logging the real exception -- for `filesystem_error` specifically,
`e.what()`, `e.code().value()`/`.message()`, and both `e.path1()`/
`path2()` -- instead of letting it reach ScriptHookRDR2's generic
handler with zero detail. `g_loaded` is now set unconditionally after
the try/catch (success or failure) so a persistent failure doesn't
retry on every single `Get()` call (i.e. every `DrawOverlay()` tick),
only on an explicit "Reload Config" press.

This doesn't yet prove the exact transient condition that triggered
the exists() throw in this specific environment (needs an actual
`filesystem_error:` log line from a real recurrence to confirm) --
but it turns "the game shows a useless crash dialog" into "PokerCheat.log
has the real reason," which is the concrete fix asked for, without
giving up mINI or its one-shot whole-file read/write behavior.

Both configs build and deploy clean (game closed for both). Not yet
re-tested in-game -- next step is confirming the injection crash is
actually gone, and if "Reload Config" ever errors again, checking the
log for the new `filesystem_error:` line to see exactly what
`e.code().message()` says.

### The try/catch didn't fire either -- real root cause found via debugger, fixed with a dedicated worker thread

The try/catch fix above didn't actually resolve anything: user hit the
ScriptMain-driven crash again (a ScriptHookRDR2 "an exception occurred
while executing PokerCheat.ASI" error), but `PokerCheat.log` showed
only `"PokerCheat started"` -- none of the three new catch blocks
logged anything. That's a real, informative negative result: a genuine
thrown `std::filesystem::filesystem_error` (or any `std::exception`)
WOULD have been caught and logged. Nothing logging at all means the
failure is happening below the level C++ `catch` can even see -- a
structured/hardware exception (access violation, stack overflow),
which normal `/EHsc` `catch(...)` cannot intercept.

Deployed a Debug build and had the user attach a debugger, per their
request re-enabling the `DllMain`-based call specifically to make the
crash reproduce on demand again (temporary diagnostic-only change to
`main.cpp`/`script.cpp`, reverted immediately after). The captured
fault symbolized to **`_alloca_probe`/`__chkstk`** -- the compiler-
generated stack-probe helper inserted whenever a function's frame
needs to grow past a page boundary. Combined with the earlier Event
Viewer capture (`STATUS_STACK_OVERFLOW`, `0xc00000fd`), this is a
clean, textbook stack-overflow signature: `__chkstk` is just the
messenger reporting "no stack left to grow into," not a bug in itself.

Real root cause: ScriptHookRDR2 (same author/architecture as Alexander
Blade's ScriptHookV for GTA5) is a cooperative script-hook framework --
`scriptWait()`/`WAIT()`'s signature is the tell -- and frameworks in
this family are well known in the modding community to run each
registered script (this mod's `ScriptMain`) on a **Windows fiber with a
small, fixed stack**, not a normal ~1MB OS thread. `<filesystem>`'s
narrow-to-wide path conversion pulls in comparatively heavy locale/
codecvt machinery, especially its first-use lazy initialization --
plausible to exhaust a small fiber stack even with zero recursion on
our end. This explains both crash sites uniformly: `DllMain` (loader-
lock reentrancy is its own separate, independently-sufficient hazard)
and normal `ScriptMain`-driven menu presses (the fiber's small stack
being the actual constraint there) were never really two different
bugs -- both are downstream of running stack-heavy `<filesystem>` code
somewhere with an inadequate stack.

Fix that keeps mINI/`<filesystem>` entirely intact: `Config::Reload()`
now spawns a **dedicated worker thread** (`CreateThread`, explicit 4MB
stack) that runs `ReloadImpl()` wrapped in the try/catch (moved from
`Reload()` itself into the new `ReloadThreadProc`), and
`WaitForSingleObject`s on it before returning -- so `Reload()`/`Get()`
callers see identical synchronous behavior, but the actual
`<filesystem>`-heavy work now executes on a real, generously-sized
stack completely outside ScriptHookRDR2's fiber machinery. Reverted
the temporary `DllMain` diagnostic change back to calling
`Config::Reload()` from `ScriptMain` only -- spawning/waiting on a
thread from `DllMain` is itself one of Microsoft's documented DllMain
restrictions, so this fix specifically requires NOT being invoked from
there, independent of the stack-size fix.

Both configs build and deploy clean (game closed for both). Not yet
re-tested in-game -- next step is confirming both the injection path
and a live "Reload Config" press are actually crash-free now.

### The synchronous wait itself was the next bug -- went fully async

The worker-thread fix crashed too, but differently: Event Viewer showed
the fault now landing directly inside **`RDR2.exe`'s own code**
(`Faulting module name: RDR2.exe`, `0xc0000005`), not `ntdll.dll` and
not `PokerCheat.asi` -- a real change in signature from every crash
before it (also visible in the same Event Viewer pull: two earlier
`0xc0000005` faults *inside* `PokerCheat.asi` itself at 7:39, from
testing in between, then this new RDR2.exe-internal one at 7:45).

Best explanation: `Config::Reload()`'s `WaitForSingleObject(hThread,
INFINITE)` blocks the calling THREAD, not just our fiber. Cooperative
script-hook frameworks in this family typically multiplex many fibers
onto one real OS thread via `SwitchToFiber()` -- if that's what
ScriptHookRDR2 does here, blocking synchronously from inside one fiber
doesn't just pause that fiber, it stalls the entire shared thread,
including whatever else (other scripts, possibly the frame's own
script-processing step) was meant to run on it. That's a materially
bigger hazard than the stack-size problem the worker thread was built
to fix, and lines up with a downstream crash appearing in the game's
own code once things resume in a bad state.

Fix: made `Config::Reload()` fully fire-and-forget -- `CreateThread`,
immediately `CloseHandle` without waiting, no synchronization with the
calling fiber at all. `g_values`/`g_loaded` (now `std::atomic<bool>`,
release-stored by `ReloadThreadProc` only after every field is written)
are published asynchronously; `Get()` does a matching acquire-load
before returning `g_values` so the C++ memory model actually guarantees
visibility of a fully-written set of values rather than relying
informally on x64's strong memory ordering. Added
`g_reloadTriggered` (also atomic, `exchange`-guarded) so `Get()` only
kicks off the implicit first-load once, not on every call before it
finishes. Practical tradeoff: `Get()` can return stale/default values
for the first few frames after any `Reload()` call, including the
eager startup one -- acceptable given the HUD doesn't render until
poker_sp is detected (real playtime away) and a file read/write
finishes in milliseconds; a concurrent second load can field-by-field
race a reader for one frame, never producing a crash or a torn
individual value.

Both configs build and deploy clean (game closed for both). Not yet
re-tested in-game.

### Still crashed -- moved the call site to DllMain instead of ScriptMain

The fully-async fire-and-forget version crashed again. User's question
cut to the actual assumption worth dropping: `Config::Reload()` doesn't
touch any game natives or ScriptHookRDR2 state at all, so there was
never a real requirement that it run from `ScriptMain`'s fiber
specifically -- that was inherited from the original (pre-crash)
"eager load" request, not a hard constraint. Moved the call from
`ScriptMain()` to `main.cpp`'s `DllMain`, `DLL_PROCESS_ATTACH`.

This is safe specifically BECAUSE `Config::Reload()` is already
fire-and-forget (`CreateThread` + immediate `CloseHandle`, never
`WaitForSingleObject` -- previous entry): Microsoft's actual documented
DllMain restriction is against creating a thread and then
synchronizing with it (waiting) from `DLL_PROCESS_ATTACH`, not against
spawning one that runs independently. The worker thread itself never
calls `LoadLibrary`/`FreeLibrary` or touches anything else
loader-sensitive, just `GetModuleHandleExA`/`GetModuleFileNameA` (read-
only queries) and plain file I/O.

Added `g_reloadTriggered.store(true, ...)` at the top of
`Config::Reload()` itself (previously only set inside `Get()`'s
bootstrap check) so the later first `Get()` call (from `DrawOverlay()`,
once poker_sp is detected) doesn't redundantly kick off a second load
on top of the one already started from `DllMain`.

Both configs build and deploy clean (game closed for both). Not yet
re-tested in-game -- this is now the fourth distinct fix attempt for
the same underlying crash; if this one still fails, the next diagnostic
step should probably go back to a debugger capture (same technique
that found `_alloca_probe` before) rather than reasoning further from
Event Viewer alone.

### Crash moved into PokerCheat.asi itself -- found a real concurrency bug, plus a narrow/wide encoding fix

DllMain-triggered load crashed again, but progress: the fault this time
was inside `PokerCheat.asi`, specifically in `_Hash_find_last_result` --
MSVC STL's internal `std::unordered_map` bucket-lookup helper. mINI's
own `INIMap<T>::dataIndexMap` is exactly a
`std::unordered_map<std::string, std::size_t>`, so this is a crash
inside mINI's own index lookup, not ours.

That pointed at something concrete and real, independent of whether it
turns out to be THE cause: `Config::Reload()` can now be triggered from
more than one place with overlapping timing (`DllMain`'s eager load,
plus an explicit "Reload Config" press could in principle land while
the first load's worker thread is still running), and the previous
`ResolveIniPath()` cached its result into a plain `char[]` with **no
synchronization at all** -- two threads calling it for the first time
concurrently could race on filling the same buffer, and even past that
first call, two overlapping `Reload()`s would have two worker threads
simultaneously reading/writing the SAME `PokerCheat.ini` file. Fixed
both: `ResolveIniPath()` now uses a function-local `static` ("magic
static," C++11-guaranteed thread-safe exactly-once initialization)
instead of the hand-rolled cache, and a new `g_reloadMutex` serializes
`ReloadThreadProc` so only one reload's file I/O and `g_values` writes
ever run at a time.

Separately, user asked whether an ASCII/wide-char mismatch could be
involved. It's real, and connects to the *earlier* `_alloca_probe`
finding rather than being a competing theory: `ResolveIniPath()` was
building a narrow `char*` (via `GetModuleFileNameA`/`_splitpath_s`),
but `mINI::INIFile`'s constructor takes `std::filesystem::path`, whose
native Windows representation is `wchar_t` -- passing it a narrow
string forces an implicit narrow-to-wide conversion inside
`std::filesystem::path`'s own constructor, via the current C locale's
codecvt facet, **on every single `Reload()` call**. That's exactly the
class of comparatively heavy, locale-dependent machinery already
implicated in the original stack-overflow crash. Fixed:
`ResolveIniPath()` now resolves everything with the wide Win32 APIs
(`GetModuleFileNameW`/`_wsplitpath_s`) and returns a
`std::filesystem::path` directly, so mINI never needs to convert
anything -- not just on the first call, on every one. Log lines that
print the path switched from `%s`+narrow to `%ls`+`.c_str()`
accordingly.

Both configs build and deploy clean (game closed for both). Not yet
re-tested in-game -- this stacks three fixes since the DllMain move
(mutex serialization, thread-safe path caching, narrow/wide
elimination) without another debugger capture in between, so if it
crashes again the next step really should be a fresh capture rather
than a fifth theory.

### mINI abandoned for real, after crashing a fourth time -- switched to inipp

The mutex + wide-path fixes still crashed. After three real, verified
bugs fixed (per-key write hitching, then the loader-lock/fiber-stack
crash, then the concurrency race, then the narrow/wide conversion) and
a fourth crash on top of all of them, this stopped being "one crash" --
it was time to try a different library rather than keep patching
around the same one. Removed the `external/mINI` submodule entirely
(`git submodule deinit`/`git rm`) and added
`external/inipp` (https://github.com/mcmtroffaes/inipp) in its place,
same vendoring convention.

Read `inipp.h` in full before wiring it up (not just the README) --
concrete, structural reasons it avoids every failure mode mINI hit:
- **No `<filesystem>` at all.** `inipp::Ini<char>` operates purely over
  `std::basic_istream`/`std::basic_ostream` (`parse(is)`/`generate(os)`)
  -- file opening is entirely our own responsibility, done with MSVC's
  wide-char `ifstream`/`ofstream` constructor overloads directly. No
  `std::filesystem::path` construction anywhere, so no narrow-to-wide
  codecvt/locale-facet machinery on any call, and no
  `std::filesystem::exists()` throw site.
- **`std::map`, not `std::unordered_map`.** `Ini::Sections`/`Section`
  are plain `std::map<std::string, ...>` -- a tree, not a hash table,
  so the exact `_Hash_find_last_result` crash class can't occur here at
  all. `std::map::operator[]` also never invalidates references to
  other existing elements on insert (unlike mINI's vector-backed
  `INIMap`, where growing the outer structure could dangle previously-
  obtained section references) -- one less thing to reason carefully
  about.
- **No exceptions thrown anywhere in the header** (confirmed by reading
  it fully, not assumed) -- `parse()`/`generate()` are plain
  `std::getline`/stream/`std::map` operations.

Rewrote `Config.cpp`/`Config.h` against it. Kept both real fixes from
the mINI chase that are library-agnostic: `g_reloadMutex` (serializes
concurrent `Reload()` calls) and `ResolveIniPath()`'s function-local-
static "magic static" caching (thread-safe exactly-once init) --
both are still correct and needed regardless of which INI library sits
underneath. `GetOr<T>()` (new, generic) leans on
`inipp::get_value()`'s actual behavior -- it only writes to its
out-param on success and leaves it untouched otherwise -- so seeding
the out-param with the default and ignoring the bool return is exactly
the right fallback, for any type `inipp::extract<T>` supports.
`inipp::generate()` always writes the whole structure (no mINI-style
"lazy" partial update that preserves untouched formatting) -- fine
here since `parse()` already carried the user's existing values into
`ini.sections` before we fill in any missing defaults and generate()
the merged result.

`Config::Reload()`/`Get()`'s actual threading model (fire-and-forget
worker thread, called from `DllMain`, async publish via
`std::atomic<bool>`) is unchanged -- none of that was mINI-specific.

Both configs build and deploy clean (game closed for both). Not yet
tested in-game.

### inipp confirmed working; stripped the worker thread back out

User confirmed in-game: `PokerCheat.ini` now appears immediately on
injection, no crash. With that confirmed, asked to move back to
non-threaded -- correct call: the worker thread's entire reason to
exist was getting `<filesystem>`'s stack-heavy locale/codecvt
machinery off ScriptHookRDR2's small fiber stack, and inipp never
touches `<filesystem>` at all (plain `std::getline`/`std::map`/
`std::basic_istringstream`, all shallow stack usage). That concern
is gone, so the threading complexity it justified -- worker stack
size, `g_reloadMutex` serializing overlapping reloads, atomics with
acquire/release ordering for cross-thread publish -- no longer earns
its keep either.

`Config::Reload()` is a plain synchronous call again: `ReloadImpl()`
wrapped in the same try/catch (still just defensive insurance, inipp
throws nothing), `g_loaded` back to a plain `bool`. Removed
`ReloadThreadProc`, `CreateThread`/`CloseHandle`, `g_reloadMutex`, and
both `std::atomic<bool>`s entirely. `DllMain`'s call to
`Config::Reload()` is unchanged in location (still there, still before
`scriptRegister`) but is now genuinely ordinary DllMain work -- ordinary
file I/O, no thread creation, nothing loader-lock-adjacent to reason
about at all.

Both configs build and deploy clean (game closed for both). Not yet
re-tested in-game against this specific simplification, but low risk
given inipp's confirmed-safe call sites are unchanged -- only the
threading wrapper around them was removed.

### The toggle lag came back -- different cause this time, PatternScan's naive scan

User confirmed inipp/non-threaded config works, but reported the
"Toggle Poker Cheat" hitch again. With Config now loaded eagerly at
injection (long before any toggle press), it couldn't be the same
cause as the original hitch -- traced it fresh instead of assuming.

Real cause: `PatternScan::FindInMainModule()` (`PatternScan.cpp`) did a
naive brute-force scan -- for every byte offset in RDR2.exe's entire
mapped image (100+ MB), an inner-loop comparison against the pattern,
no skip optimization at all. Its result is cached (a function-local
`static` in `GamePointers::GetScriptThreads()`), but the cache only
gets populated on the *first* call -- and the only caller,
`GamePointers::FindScriptThread()`, is only ever reached from
`DrawOverlay()`, which only runs once `PokerCheat::Enabled` is true --
i.e. only after the first "Toggle Poker Cheat" press. So that first
toggle each session was exactly when this expensive, uncached scan ran
for the first time. This has been here since Session 2 -- not a
regression from anything done this session, just never isolated as
*the* cause before because Config's issues were coincident with it.

Two-part fix:
- Rewrote `FindInMainModule()` to anchor on the pattern's first
  non-wildcard byte and use `memchr` (CRT, typically SIMD-backed) to
  jump straight to each candidate occurrence of it, only running the
  full per-byte pattern comparison at those candidates -- instead of
  testing every single byte offset by hand. Caught and fixed a real
  bug in this rewrite before deploying it: naively starting the search
  at the image's very first byte could let `memchr` find an anchor
  close enough to the start that `candidateStart` (`anchor -
  firstConcrete`) pointed before the image, an out-of-bounds read --
  fixed by starting the search at `base + firstConcrete` instead of
  `base`.
- Added `GamePointers::GetScriptThreads();` to `main.cpp`'s `DllMain`
  (right after `Config::Reload()`), forcing the scan to run once at
  injection instead of lazily on the first toggle. Safe to do from
  `DllMain` specifically because it's pure computation over
  already-resident memory: no file I/O, no thread creation, nothing
  loader-lock-sensitive -- RDR2.exe's own image is guaranteed fully
  mapped by the time any DllMain in the process runs at all, since the
  OS maps the whole primary executable before processing any DLL's
  imports or entry point.

Both configs build and deploy clean (game closed for both). Not yet
re-tested in-game.

## Session 11: opponent seat card icons, a real RDR2 font, win-prediction moved off the panel, and a long dead-end chase for "between hands"

### Opponent hole-card icons next to their name on the real HUD

User wanted opponents' hole cards drawn as real card-face icons next to
their name on the vanilla HUD, same idea as the community-card strip but
per-seat. Seat-to-screen mapping had to be derived from scratch again:
the vanilla per-seat panel turned out to be a single **vertical list at
the bottom-left corner**, reading bottom-to-top, not a table-fan layout --
confirmed by the user reading real pixel offsets off their own screen
(2560x1440). Two real bugs found and fixed via live user reports before
the direction/spacing was right:
- First guess (`(seat - mySeat + 6) % 6`) had the rotation direction
  backwards -- a live 4-seat report (mySeat=5 at the bottom row, then
  seat 4, seat 3, seat 2 going up) showed the list actually counts
  **down** from your own seat number, `(mySeat - seat + 6) % 6`. Matches
  an idiom the game's own script already uses for turn order
  (`(f_9 + 5) % 6` for "the seat before mine", poker_sp.ysc.c ~line
  5858).
- Using the raw relative offset directly left gaps when the table wasn't
  fully occupied (the real panel compacts, an empty seat doesn't reserve
  a blank row). Fixed by computing a DENSE row index instead --
  `ComputeDenseRowForSeat()`, shared between the actual drawing code and
  a new `ProbeSeatOccupancy()` diagnostic so the two can never drift out
  of sync.
- Folded opponents' cards were still being drawn (real cards are still in
  memory after a fold, same reason the debug panel's own `[FOLDED]` line
  still lists them) -- gated the icon draw on `isActive` (seat state
  0/active or 2/all-in only) per user report.

Position/spacing calibrated via the usual live Reload Config loop; final
values baked in as both `Config.h` defaults and the Release constexpr
fallback (`SeatCardIconBaseX/Y`, `StepY`, `SpacingX`, `Width`, `Height`,
plus `LabelOffsetX/Y` for the win-label position added later).

### A real RDR2 font -- three wrong guesses, then the actual answer from real source

User asked for a `(You Win)`/`(They Win)` label under each opponent's
cards, "in the RDR font, not this boring debug font." Investigated
whether that's even possible:
- Confirmed (three independent native databases, checked twice) that
  `UI::DRAW_TEXT`'s legacy text path has exactly one font available to
  script -- no `SET_TEXT_FONT` native exists anywhere in the real,
  current RDR3 native DB.
- First attempt: `COLOR_STRING` text-template type + hand-guessed real
  font names (`"TisaOffc"`, later real names like `"RDR Lino"` pulled
  from IDA strings in Rampage Trainer's binary) as a `<FONT FACE=...>`
  tag. Built a live `DrawFontTest()` diagnostic (F10 menu) to test
  side-by-side -- every variant looked identical. Root cause found by
  grepping every real `COLOR_STRING` call site in the actual game
  scripts: it's a narrow, fixed 4-argument template (`VAR_STRING(42,
  "COLOR_STRING", _CREATE_COLOR_STRING(colorVal), text)`), not a general
  HTML/tag parser -- our 3-argument calls were malformed for that
  template from the start.
- Real answer came from inspecting an actual working open-source trainer
  (github.com/Halen84/RDR2-Native-Menu-Base, user-supplied). Its
  `Drawing.cpp` revealed the actual bug: `UI::DRAW_TEXT` (hash
  `0xD79334A4BB99BAD1`) and `UI::SET_TEXT_COLOR_RGBA` (hash
  `0x50A41AD966910F03`) are BOTH documented in that project's own
  natives.h as nullsub/no-ops since game build 1436 -- our build
  (1491.50) is long past that. The confirmed working replacement for any
  build newer than 1311 is `UIDEBUG::_BG_DISPLAY_TEXT` /
  `_BG_SET_TEXT_COLOR` (added to `ExtraNatives.h`, the stock SDK doesn't
  declare that namespace at all), still called via
  `GAMEPLAY::CREATE_STRING(10, "LITERAL_STRING", ...)` (that part was
  always right). Real tag syntax confirmed from their own
  `DrawFormattedText()`: `<TEXTFORMAT RIGHTMARGIN='0'><P
  ALIGN='Left'><FONT FACE='$token' LETTERSPACING='0'
  SIZE='N'>~s~TEXT</FONT></P><TEXTFORMAT>`. Font tokens themselves
  (`$title`, `$Font5`, etc., not real display names) cross-confirmed
  against github.com/ExpMero/rdr2_fonts, which hosts the actual
  extracted `.ttf` files named `<index>_$<token>_<Real Name>.ttf`.
  User confirmed `$Font5` ("Redemption") renders visibly differently
  once the right native pair was used -- first real font win of the
  project. Now used for both the per-opponent win/lose label and the
  standalone win-prediction status below.

### ShowWinPrediction moved off the panel entirely

Originally just another line in the debug text panel. User wanted it
fully independent: own-seat card icons/label (added mid-session, mirrored
off the opponent-row code) were removed again after actually seeing them
in play -- redundant, since the player already sees their own hand.
`ShowWinPrediction` now drives a dedicated `DrawWinPredictionStatus()`,
same real-font pipeline as the opponent labels, positioned independently
via `Config`'s `WinPredictionX/Y` (a rough screen-center starting point,
not calibrated) instead of living in the panel's line sequence.

### The "between hands" chase -- five dead ends, then abandoned for a plain rule

Long-running bug: card icons/board kept showing stale data between hands
instead of hiding like the vanilla HUD does. Chased a "round-phase state"
field through poker_sp's memory across most of the session; every
candidate was a real, traceable field that turned out NOT to answer the
actual question:
- `f_114.f_2010` -- looked like a real phase state from static analysis
  (0 = idle, matched two independent code paths), but empirically stayed
  constant the whole session.
- `f_1.f_42` -- user's own find, a real switch in `func_3`
  (`poker_sp.ysc.c:4918`). Traced it to the OUTER activity-lifecycle
  state machine (init -> waiting -> running -> cleanup), not per-hand --
  confirmed by the user: stays at one value (6) the entire session.
- `f_114.f_2011` -- user's second find. Traced a real dispatcher
  (`poker_sp.ysc.c:8151`: `f_2015[f_2010](...)`) showing `f_2011` is
  **phase-relative**, indexed by whichever of 14 separate handler
  functions `f_2010` currently selects -- same raw number means
  something different depending on context, which is why it "looked very
  inconsistent" to the user. Mapped the user's own live-observed sequence
  (0->13->94) to `func_286` exactly (the pre-seated/setup handler), and
  found the real payout sequence in the largest handler, `func_296`
  (cases 86-91). User set an empirical threshold (`f_2011` in [16, 70) =
  in progress) and wired it in for live testing -- abandoned after
  observing a brief invalid/glitch value during a real transition.
- `f_1.f_3.f_2` -- another switch the user found; traced its `func_102`/
  `func_103` targets to real in-game world coordinates (e.g. `2626.75,
  -1219.20, 52.25`) -- a table/location selector, not round state.
- `func_665` (checked while investigating `func_296`'s payout exit
  condition) turned out to just be a live count of non-folded active
  seats, not a pot-size getter as hoped.
- `Global_1360165[essParam0]` -- a switch the user found elsewhere;
  turned out to be the game's shared 1157-slot "essential ped" pool,
  referenced by 400+ scripts across the entire game (ped spawn/setup
  lifecycle), completely unrelated to poker at all.

User's call: stop hunting for a native state flag. `handInProgress`
reverted to a plain `constexpr bool = true` -- draw whenever `poker_sp`
is running, same rule the mod had before this whole detour. Removed the
now-dead diagnostic debug line and the state-reading code from the draw
path; the underlying slot constants and two new diagnostics
(`ProbeSeatOccupancy`, `DumpLocalStackRange` -- the latter logs the
script-local stack's absolute address range for pasting straight into
Cheat Engine) are left in place in case this gets picked up again later.

### Also fixed

A `PokerCheat.ini` typo (`StepY=-0.9` instead of `-0.09`) was pushing
every opponent row past the first off the top of the screen entirely --
only one card set was ever visible. Found by just reading the file.

All changes build clean in both configs; user testing throughout, not a
cold handoff.

## Session 12: C++ modernization -- spdlog logger, std::string everywhere, no C-style casts

Not a reversing session -- no new offsets, no new game-behavior findings.
User request: "C++ the shit out of" this codebase (no C-style casts,
STL/STL-adjacent only, type-safe, no CRT-buffer runtime-crash surface),
explicitly referencing the same cleanup already under way in
`../BlackjackCheat` and its logger swap to spdlog. A prior audit of this
file's own casts turned up nothing to fix -- every cast here was already
`static_cast`/`reinterpret_cast`/`const_cast` (`PatternScan.cpp`,
`GamePointers.cpp` in particular were already clean, presumably because
they were written after `PokerCheat.cpp`'s own oldest code). The real
work was two things `PokerCheat.cpp` and `Config.cpp` still had left over
from earlier sessions: fixed `char[]` buffers with `sprintf_s`/
`strcpy_s`/`strcat_s` (texture names, card-set dictionary names, the
`(You Win)`/verdict rich-text strings, every debug per-seat/board line,
the calibration-grid axis labels, `Config::SetFloat`'s INI value buffer),
and the hand-rolled `fopen_s`/`vfprintf` logger.

### Logger: spdlog, header-only, fmt call sites

Added `external/spdlog` as a submodule (same commit BlackjackCheat's own
in-progress submodule already pins, `57cb5fb7a8ff...`, i.e. its current
HEAD -- spdlog 1.17.0). `Log.h` now wraps a `spdlog::basic_logger_mt`
writing `PokerCheat.log` with pattern `"[%H:%M:%S.%e] %v"` (matches the
old hand-built `[HH:MM:SS.mmm] ` prefix exactly), opened once via a
magic-static instead of reopened with `fopen_s` on every call.
`SPDLOG_HEADER_ONLY` is defined before including it so no separate
spdlog `.cpp` needs adding to the vcxproj -- it compiles straight into
this project's own translation units, same as inipp/RDR-Classes.

The actual point of the swap: `Log::Write` used to be `(const char* fmt,
...)` straight into `vfprintf` -- a mismatched `%s`/`%d` against the
real argument list was a genuine, silent runtime-UB risk (wrong type
pulled off the `va_list`, or reading past the last real argument if the
format string claimed more). The new signature,
`Write(spdlog::format_string_t<Args...>, Args&&...)`, validates the
fmt `{}` placeholder count against `Args` at COMPILE time -- a mismatch
is now a build error instead of a runtime one. Every one of this
project's ~35 `Log::Write` call sites (`PokerCheat.cpp`, `Config.cpp`,
`GamePointers.cpp`, `script.cpp`) got converted from `%`-specifiers to
`{}` by hand: `%d`/`%u`/`%s`/`%c` -> `{}`, `%.Nf` -> `{:.Nf}`, `%+d` ->
`{:+}`, and `"0x%llX"` -> `{:#x}` (fmt supplies its own `0x` prefix, so
the literal one in the old format string had to go). Two probe log
lines (`ProbeTableStruct`'s per-seat dump, `ProbeSeatOccupancy`'s)
contain LITERAL curly braces in their own text (`card0={rank=...}`) --
those needed escaping as `{{`/`}}`, easy to get wrong silently since a
missed escape just produces a differently-garbled log line, not a build
error; verified by hand against fmt's escaping rules, not just by
compiling. Two call sites in `Config.cpp` logged a `std::wstring` path
via the old `%ls` -- fmt has no narrow-format-string-into-wide-string
conversion, so added a small `NarrowPath()` helper (`WideCharToMultiByte`
into UTF-8) rather than fighting fmt's wide/narrow split.

Compiling against the bundled fmt failed at first with a MSVC-only
static_assert ("Unicode support requires compiling with /utf-8") --
this project's `CharacterSet` is `MultiByte` and nothing previously
forced a UTF-8 source/execution charset. Fixed by adding `/utf-8` to
`AdditionalOptions` in both configs rather than disabling fmt's Unicode
support -- the more correct fix, and harmless here since this project's
literals are all plain ASCII anyway.

Verified with a standalone smoke test (a throwaway .cpp including only
`Log.h`, compiled directly with `cl /utf-8 /std:c++latest`, run outside
the game entirely) exercising one call site of each converted format
(plain string, `%s`->`{}`, `0x%llX`->`{:#x}`, the escaped-brace seat
dump, `%+d`, and the `%.Nf`/`%.Nf` float pair) -- output matched the old
printf-based formatting byte-for-byte (same `[HH:MM:SS.mmm]` prefix,
same `0x...` hex rendering, same escaped-brace text). Not a substitute
for an in-game check (not done this session), but enough to catch a
wrong fmt spec before it ever reached a live log.

### std::string/std::ostringstream, no more fixed char[] buffers

`BuildCardTextureName()` now returns `std::string` (and calls the
existing `RankName()` instead of a second, duplicate rank-name switch --
the same simplification `BlackjackCheat.cpp`'s own ported copy of this
function already made); `FindLoadedCardSetDict()` takes a `std::string&`
out-param instead of `char*, size_t`. Every debug/HUD string that used
to be built with `sprintf_s`/`strcat_s` into a fixed buffer
(`DrawCommunityCardIcons`/`DrawSeatCardIcons`'s texture names, the
`(You Win)`/`(They Win)`/`(Tie)` and win-prediction rich-text strings,
`DrawCalibrationGrid`'s axis labels via a new `FormatFixed1()` helper,
every per-seat debug line and the `Board:` line in `DrawOverlay()`, and
the `PredictionCheck` showdown real/predicted card-string diff) now
builds a `std::string`/`std::ostringstream` instead. `Config::SetFloat`
swapped its `char[64]` + `sprintf_s("%g", ...)` for an `ostringstream`
(default `operator<<` float formatting matches `%g`'s shortest-
representation/6-significant-digit behavior closely enough for an INI
round-trip). `const_cast<char*>(str.c_str())` is used only at the actual
native call boundary (`DRAW_SPRITE`, `HAS_STREAMED_TEXTURE_DICT_LOADED`,
`CREATE_STRING`), never upstream of it -- same convention
`BlackjackCheat.cpp`'s already-ported card-icon code follows, and now
documented in this file's own CLAUDE.md (Coding Conventions section,
new this session).

### Verification

Both `PokerCheat.vcxproj` configs (`Debug|x64`, `Release|x64`) build
clean with zero warnings introduced. `tests/PokerHandEvalTests.exe`
still prints `ALL PASS` (untouched by this session -- `PokerHandEval.h`
has zero string/logging code in it to begin with). Not yet verified
in-game (no live log to inspect this session) -- the smoke test above
covers the logger's format-string correctness, but not, e.g., whether
spdlog's file sink behaves correctly under ScriptHookRDR2's actual
process/fiber environment; that's the first thing to check next session
if `PokerCheat.log` doesn't show up next time the mod is actually run.

## Session 13: Dump Full Stack JSONL ported from BlackjackCheat; two new confirmed handState values; a live all-in hand traced end to end

### Dump Full Stack JSONL

Ported `BlackjackCheat`'s `GamePointers::DumpLocalStackJsonl()` (see that
project's `docs/JOURNAL.md`, Session 6) over to this project verbatim in
spirit -- two overloads (full stack, or an explicit `[startSlot,
startSlot+count)` range) that dump every script-local slot of poker_sp's
thread as one JSON object per line (`slot`/`i32`/`u32`/`i64`/`f32`/`hex`),
no assumption about what any slot means. Adapted to this project's own
(newer, stricter) conventions rather than copied byte-for-byte:
`std::ofstream`/`std::ostringstream` instead of `fopen_s`/`fprintf`, and
`const std::string&` instead of `const char*` for the output path.
`PokerCheat::DumpFullStackJsonl()` wraps it with a timestamped
`PokerCheat_stackdump_YYYYMMDD_HHMMSS.jsonl` filename (so consecutive
before/after dumps never clobber each other) and is wired to the F10
menu. While in there, also removed `ToggleFontTest()`/`DrawFontTest()`
entirely (per user request, "we're done with that") -- that diagnostic's
job was already done in Session 11 (confirmed `UIDEBUG::_BG_DISPLAY_TEXT`
as the working real-font pipeline, now used unconditionally by
`DrawSeatCardIcons()`/`DrawWinPredictionStatus()`), so it had no reason
to keep existing.

**Bug found and fixed in the new tool itself**: a garbage bit pattern
read as a `float` frequently lands on NaN/Inf, and `operator<<` prints
those as bare `nan`/`-nan`/`inf` tokens -- not valid JSON, and it broke
every dump this session until worked around with a regex sanitize pass
before parsing. Fixed by checking `std::isfinite(f32)` and writing
`null` for the `f32` field instead (matching how every real JSON
library serializes a non-finite float) when it isn't.

### A theory that real log data disproved: `PredictionCheck` re-arming

Reasoning from raw dumps alone (an early batch of 8, all taken via the
dump tool with the advisor's `Enabled` flag OFF the whole time) suggested
a real bug: `PredictionCheck`'s "new hand" snapshot
(`PokerCheat.cpp` ~line 979) only re-arms
`if (revealCount == 0 && deckCursor != s_preflopCursorSnapshot)`, and
every dump showed preflop `deckCursor` as exactly `2 * occupied seats`
(12, for a stable 6-seat table) in EVERY hand -- so the theory was that
after hand 1, the condition would never trip again and the whole
self-check would go silent forever. This was **wrong**, and the user
correctly pushed back for real evidence rather than accepting the
theory: once actually toggled on (F10 -> "Toggle Poker Cheat" is a
SEPARATE menu item from "Dump Full Stack JSONL" -- the dump tool reads
memory directly and doesn't require `Enabled`, which was the real reason
the first session's log had zero `PredictionCheck` lines despite a full
hand playing out), the real log showed `deckCursor` passing through `0`
as a brief transient between hands (deck reset, before the deal) before
landing on `12` after the deal -- so "cursor changed since last
snapshot" DOES trip every hand after all, just with an extra harmless
"new hand" log line at the transient `cursor=0` moment (that snapshot
always gets overwritten by the real `cursor=12` one before any showdown
could compare against it). Two real hands, two real showdowns, both
logged `MATCH`. Lesson: the earlier 8-dump batch never happened to
sample the `cursor=0` transient purely because manual point-in-time
dumps are not the same as watching every frame -- a sampling artifact,
not a code bug. No fix made; `PredictionCheck` works as designed.

### Two new confirmed `handState` values, traced through a live all-in hand

A run of 6 dumps (`PokerCheat_stackdump_20260911_213745` through
`_213828`, ~45 seconds apart) captured one full hand end to end,
correlated against `PokerCheat.log`'s `PredictionCheck` lines from the
same window:

1. **213745** -- `handState=5` (preflop betting), all 6 seats `active`.
   mySeat=1, hole `KC 3H`.
2. **213756** -- same hand (`subStep` 26->35): seat 1 (you) shoved --
   `stack` 99->0, `bet`->99. Seats 2/3/4 `FOLDED` (`seat.f_6`==1), seats
   0/5 still `active`.
3. **213801** -- `handState=8` (not previously confirmed): an all-in
   fast-forward/showdown runout state. `reveal=5`, real board `QH KH 3D
   10C 3S` exactly matches the predicted board (the `PredictionCheck ...
   MATCH` logged at 21:37:58). Seats 0/1/5 all `ALL-IN` (`seat.f_6`==2);
   seat 1's stack already shows 271, seats 0 and 5 dropped to 0 -- a
   3-way all-in resolved and paid out within this single state.
4. **213809** -- `handState=11` (not previously confirmed): a
   between-hands state. `deckCursor`/`reveal` both reset to 0, but the
   real board array still holds the previous hand's stale card values
   (same staleness already documented in Session 6/CLAUDE.md -- harmless
   since `BuildPredictedBoard()` gates on `revealCount`, never on the raw
   -1 sentinel). Every seat's `state` field (`seat.f_6`) reads `-1` here
   -- a distinct "no state assigned" sentinel for the between-hands gap,
   separate from `seat.f_0`'s own -1-means-empty-seat convention.
5. **213824** -- `handState=4` (hole cards just dealt, previously
   confirmed), `deckCursor=8` (2*4, not 2*6): seats 0 and 5, having
   busted to 0 chips last hand, now read `occ=-1` (`seat.f_0`) and are
   gone from the table entirely -- **busting removes a seat**, not
   previously confirmed. The remaining 4 seats get fresh hole cards
   (yours: `8C JH`).
6. **213828** -- `handState=5`/`subStep=26` again -- the new hand's
   preflop betting begins, same fixed subStep-26 marker seen at the top
   of every preflop phase so far.

`handState` values now confirmed: `1`=waiting/sat down, `4`=hole cards
dealt, `5`=preflop betting, `8`=all-in runout/showdown, `9`=postflop
betting, `11`=between hands. No code changes made from this session's
findings (nothing here contradicts current logic -- `isActive = (state
== 0 || state == 2)` already correctly excludes the between-hands `-1`
sentinel and busted-out `occ=-1` seats), purely new confirmed detail for
future reference.

## Session 14 -- how the AI actually decides fold/call/raise (pure research, no code changes)

User asked directly: how does `poker_sp` decide each AI opponent's
fold/call/raise, and is it live/deterministic-from-cards or predetermined.
Traced this end to end in the ground-truth decompile
(`poker_sp.ysc.c`); `act_gen_poker.ysc.c`/`poker_launch_sp.ysc.c` never
needed -- everything is self-contained in `poker_sp.ysc.c`, same as every
other system traced so far. No source changed this session, research only.

### The dispatcher: one seat, one personality index, up to 6 decision functions

`func_1255` (line 43972) is the per-seat decision entry point, called from
`func_666` (line 26789) as
`func_1255(&(uParam0->f_114.f_2655), uParam1, uParam2,
uParam0->f_114.f_2655.f_90[uParam1->f_6])` -- i.e. the 4th argument is
`f_2655.f_90[seat]`, a per-seat **personality index** (0-14) into a fixed
15-entry table built once by 15 back-to-back `func_1191(table, index,
p1, p2, styleCode)` calls (lines 25441-25455). `func_1255` switches on
that entry's `styleCode` (`.f_2`) and dispatches to one of six distinct
decision implementations:

- `styleCode` 1 or 4 -> `func_1623` (line 52540): **mechanically calls
  whatever's owed, full stop.** `num = tableCurrentBet - seatAlreadyIn;
  func_1704(..., num)`. No hand-eval read, no equity read, no randomness
  -- a pure "calling station" that plays every hand to showdown
  regardless of its actual cards.
- `styleCode` 2 -> `func_1624` (line 52548): pushes `stack + currentBet`
  (i.e. shoves), scaled by a stakes-tier-dependent random fraction
  (tier 0: `stack * random(0.225, 0.333)`; tiers 1-3: `stack * 1.0`, i.e.
  a full shove at real-money tables). Also card-blind -- no equity, no
  hand-rank read anywhere in this function.
- `styleCode` 3 -> `func_1625` (line 52582): `num = stack;
  func_1704(..., num)` -- **unconditional all-in, every decision point,
  no exceptions.** Also card-blind.
- `styleCode` 5 -> `func_1626` (line 52590): a short-stack push/fold
  hybrid -- if the seat's stack (`func_144`) is under 20 (chip units),
  shove it (`func_1704(..., stack)`); otherwise defers entirely to
  `func_1628` (below), the real engine.
- `styleCode` 6 -> `func_1627` (line 52602): a pot-cap/table-limit gate
  (compares seat stacks against `func_1259(table) + func_1705(table)`,
  a max-buy-in-shaped pair of limits, specifically when any seat is
  running the unconditional-shove personality 13 below) that either
  forces a plain fold/check (`func_1707`) or a plain call/check
  (`func_1706`), or falls through to `func_1628`.
- default (`styleCode` 0, i.e. every personality actually seen at a
  normal table -- see below) -> `func_1628` directly.

**`func_1628` (line 52646) is the real, hand-aware decision engine** --
the only one of the six that reads anything about the actual cards:

- `num5 = coroutine.f_1[seat]` -- the **live per-seat Monte Carlo equity
  float** `func_690` (Session 2, line 28182) refreshes continuously via
  `MINIGAME::_0xEC819D612038EF4B`. This is the same equity value this
  project's own `DrawOverlay()` win-probability advice is built on.
- `unk`/`num4` -- a personality-specific multiplier and a 10-float
  bet-sizing-range struct, looked up via
  `table_f9[personality_f44[idx]]` / `table_f13[personality_f44[idx].f_1]`
  (lines 52667-52668) -- i.e. every personality has its own fixed
  "how much do I trust my equity read" multiplier and its own randomized
  bet-size ranges, not shared constants.
- `num6 = func_1708(equity * personalityMultiplier, clamped [0,1])` --
  personality-adjusted equity, the actual "hand confidence" score.
- `endRange = func_1709(...)` (line 55269) -- **a positional factor**:
  fraction of currently-active seats that still have to act *after* this
  seat before betting returns to the dealer/last-raiser marker. Later
  position (fewer players left to act) yields a smaller "risk remaining"
  number.
- `endRange2` -- fraction of the seat's own stack that would remain after
  calling the current bet (a stack-preservation/pot-commitment ratio).
- `num7 = 0.7*random(0,endRange) + 0.3*random(0,endRange2); flag = num7 >
  0.6` -- a **randomized "loose/aggressive this decision" roll**, weighted
  mostly by table position and a little by stack commitment, not by
  hand strength at all.
- `num8` (the final confidence score fed into every action threshold
  below) is a stakes-tier-dependent (`switch(table.f_2)`, cases 0-3 --
  this also resolves the long-standing "what is `Table.f_2` really"
  question from Session 2/3: it **is** the stakes tier after all, just
  0-indexed with 0 being the zero-stakes practice table, matching
  `func_67`'s 4-constant stakes switch exactly) weighted blend of
  `endRange` (position) and `num6` (equity), with the blend ratio
  flipped by the `flag` roll above and tilted further toward pure equity
  (up to 100% at tier 3, `flag==false`) at higher stakes -- i.e. **real
  money tables lean more on actual hand strength and less on
  positional/random aggression than the free table does.**
- The actual fold/check/call/raise choice is a threshold ladder on
  `num8`, gated by opponent-count-scaled cutoff tables (`func_1712`/
  `func_1713`/`func_1718`, each a 7-float array indexed by
  `func_665(table)` = active-seat count, line 55331 onward): below the
  lowest cutoff -> fold-or-check (`func_1707`); below the next cutoff and
  call-amount is under half the stack -> a randomly-sized bet/raise using
  the personality's own size range (`func_1715`/`func_1716`, which do
  `stack_or_owed * random(rangeLo, rangeHi)`, ceil'd); a middle band gated
  by another random roll (`func_1717`, `<=0.4` -> more aggression) that
  also gets a randomized bet/raise; otherwise a plain call
  (`func_1719`->`func_1704` action 3) or, at the very top of the
  confidence ladder, the biggest randomized raise range
  (`func_1716` with the personality's own top-tier size bounds).
  `func_1704` (line 55189, the single shared bet/raise/call/check
  executor every path above eventually calls) sets the actual output
  action code: **2=raise/bet, 3=call, 4=check, 5=fold** (confirmed from
  `func_1707`/`func_1711`/`func_1719`'s own literal assignments), clamping
  the requested raise size through `func_1757` (min-raise/stack clamp,
  not traced further -- not needed for this question) first.

### Personality assignment: fixed per seat-occupancy, not per-hand

`func_185` (line 8975) is what actually assigns `f_90[seat]` (the
personality index `func_1255` looks up) -- called once when a seat is
filled, **only ever picks a value in [5, 8]** via
`GET_RANDOM_INT_IN_RANGE(5, 9)`, re-rolling until it lands on whichever of
those four indices is currently least-represented at the table (a
load-balancing loop, not a plain reroll-until-different). All four of
those (`func_1191(table, 5, 0,0,0)`, `(6, 2,0,0)`, `(7, 0,2,0)`,
`(8, 2,2,0)`, lines 25446-25449) have `styleCode 0` -- i.e. **every
regular AI opponent at a normal table uses the real equity-driven
`func_1628` engine**, differing only in which of a 2x2
tight/loose x passive/aggressive parameter combination (the `(p1,p2)`
pair feeding `f_9`/`f_13` in `func_1628`) they were randomly dealt when
they sat down. Personality indices 0-4 and 9-14 (the card-blind
call-station/all-in-shover/short-stack-hybrid archetypes decoded above)
exist in the same 15-entry table but are never reached by `func_185`'s
random-fill path -- they're presumably wired up elsewhere for
scripted/story-specific NPCs (not chased down this session, out of scope
for "how does the AI at a normal table decide").

### Direct answer

**Every ordinary AI opponent's fold/call/raise choice is computed live,
at the moment of the decision, from that hand's actual current equity**
(`func_690`'s continuously-refreshed Monte Carlo win% against the real
hole cards + whatever board is currently revealed) **blended with a
fixed-per-seat personality profile and randomized positional/aggression
noise on top -- nothing about the outcome is decided in advance.**
Randomness here is layered *on top of* the real hand-strength signal
(as tie-breaking noise in the confidence score, and as bet-size
variance within a chosen action), never a replacement for it, and
consistent with Session 5's separate finding that the AI's own equity
math deliberately doesn't peek at the deterministic future deck even
though it's sitting there in memory. The one caveat: a handful of
special-case personality slots (0-4, 9-14) are wired to be genuinely
card-blind/scripted (always-call, always-shove, stack-threshold
push/fold) -- but the normal random seat-fill path used at a real table
(`func_185`) never assigns any of those, so in practice every opponent
you actually sit against is running the reactive equity engine.

### Open questions

- Personality indices 0-4 and 9-14's actual assignment path (presumably
  a specific-NPC/mission-scripted call site somewhere, not found this
  session) -- only matters if a named story character's poker behavior
  ever needs explaining.
- `func_1757` (raise-size clamp) and `func_1259`/`func_1705` (the
  pot-cap constants `func_1627` compares stacks against) not traced in
  detail -- neither affects the fold/call/raise decision logic itself,
  only exact bet-size clamping/edge-case gating.

## Session 15 -- is the engine RNG behind fold/call/raise predictable? (pure research, no code changes)

Follow-on from Session 14: user asked whether `MISC::GET_RANDOM_INT_IN_RANGE`/
`GET_RANDOM_FLOAT_IN_RANGE` -- the two natives `func_1628`'s confidence-score
noise roll and bet-size variance are built on -- are deterministic/predictable,
and whether a seed could be recovered. This is a native's *implementation*
question, not a script-source one: `poker_sp.ysc.c`/`act_gen_poker.ysc.c` only
call the native by hash, the real logic is x64 code inside `RDR2.exe` itself.
Worked entirely in `D:\Backup\Stuff\RDR2 Shit\EXEs\1491.50\RDR2_Dumped.exe.i64`
via headless `idat.exe -A -S<script.py>` runs (IDA Professional 9.3, per
`..\CollectorOffline\CLAUDE.md`'s established workflow) -- one-off IDAPython
scripts written to the scratchpad, each writing its own output file (`idat.exe`
under `-A` batch mode prints nothing to stdout even on success, so every probe
script writes its results to a file and that file is what actually gets read).

### First: does poker_sp ever explicitly reseed? No.

Grepped both `poker_sp.ysc.c` and `act_gen_poker.ysc.c` for `SEED`/
`RANDOM_SEED` -- zero matches in either file. Whatever backs
`GET_RANDOM_INT_IN_RANGE`/`GET_RANDOM_FLOAT_IN_RANGE` is never touched by the
poker minigame itself; it's purely consuming the engine's own general-purpose
stream, not a per-hand-seeded local one.

### Dead ends chased in the binary

- Function names/named locations containing `rand`/`mth`/`twist`/`xorshift`/
  `well512`/`prng` (case-insensitive, both `idautils.Functions()` and
  `idautils.Names()`): the *only* hit is the imported `BCryptGenRandom`
  (`142ea0bc1`) plus a handful of unrelated string literals (`"RandSeed"`,
  `"uiRandomNumberBehavior"`, `"RANDOM_TO_SCRIPT_CONVERSION"`, `"random"`,
  `"GET_RANDOM_MODEL_FROM_POPULATION_SET"`, `"Random Events"`). This memory
  dump has **no recovered RTTI/symbol name for an `mthRandom`-style class** --
  unlike Session 2's IDA asks, there's no shortcut here from existing
  debug-name recovery.
- Decompiled every function each of those strings actually xrefs from,
  hoping one would be "read RandSeed cvar -> seed the RNG": all dead ends.
  `"RandSeed"` (`143658b88`) is read by `sub_142981650` alongside ~20 sibling
  keys (`BoxSize`, `VelocityMin`, `MassExpModifier`, `BounceRandom`,
  `WindMults`, ...) -- it's one field of a **particle-effect/VFX emitter
  config struct**, not the engine's actual RNG seed, just a per-effect
  override knob. `"uiRandomNumberBehavior"` (`14339ff80`) feeds a single
  global dword (`sub_14003B34C`) -- a UI behavior flag, unrelated. `"random"`
  (`1436a2eec`) is read as a named property off some data-driven object in
  `sub_142E35900` (falls back to `sub_142DCF534` if absent) -- looks like
  another FX/property-bag system, not chased further, low confidence it's
  gameplay-relevant. `"RANDOM_TO_SCRIPT_CONVERSION"` (`1435f2ae8`) is a
  telemetry/event tag string passed to what looks like a metrics-writer
  call, unrelated to actual random generation.
- Raw byte-scanned `.rdata`+`.data` for the literal QWORD native hashes
  (`0xD53343AA4FB7DD28` = `GET_RANDOM_INT_IN_RANGE`, `0xE29F927A961F8AAA` =
  `GET_RANDOM_FLOAT_IN_RANGE`, from `rdr3-nativedb-data/natives.json`): zero
  hits, either direction. Confirms (as expected, same as GTA5) the native
  hash table is **not** a plain `{hash, handler}` array stored verbatim --
  it's obfuscated/rotated per build the same way GTA5's is, and/or built at
  runtime via scattered `AddNativeHandler`-style registration calls rather
  than sitting as static data. Finding the real dispatch table would need
  the VM's `CALLNATIVE`-opcode handler traced first (to get the exact
  obfuscation/lookup algorithm for *this* build) -- not attempted this
  session, out of scope for the time available.
- Byte-scanned all of `.text`/`.rdata`/`.data` for MT19937's two most
  distinctive magic constants, `0x9908B0DF` (matrix-A twist constant) and
  `0x9D2C5680` (tempering constant b): **zero hits for both**, anywhere in
  the whole binary. This is strong negative evidence against a textbook
  Mersenne Twister implementation -- a real MT19937 (seeding or tempering)
  almost never avoids emitting these as literal immediates. (`0xEFC60000`,
  MT's third tempering constant, did get 5 hits, all in `.data`, not code --
  almost certainly coincidental floating-point/other data, not code using
  it as an immediate.) Also checked common PCG64 and a Knuth-style
  multiplicative xorshift constant -- zero hits for those too. Net result:
  **whatever this engine's PRNG is, it isn't one of the well-known
  textbook algorithms in a form recognizable by magic-constant scanning.**
  Consistent with RAGE having its own bespoke `mthRandom`-style generator
  (as multiple other RAGE-engine titles are reported to have, informally,
  in modding-community writeups) rather than a stock library implementation
  -- not confirmed by disassembly this session, just the most consistent
  explanation for "real RNG, but no known-algorithm fingerprint."
- Found and decompiled both of the binary's only two `BCryptGenRandom`
  callers (`sub_14273D42C`, `sub_142E752A0`) -- Windows CNG's actual
  cryptographic RNG, the one call worth checking as "maybe this seeds the
  fast gameplay PRNG once at boot." Both are generic
  `FillBufferWithSecureRandomBytes(buffer, size)`-shaped utility wrappers
  (`sub_142E752A0` even opens/closes its own fresh `BCryptOpenAlgorithmProvider(L"RNG")`
  handle **per call**, which no engine would do for a hot per-frame gameplay
  dice roll -- native call sites for `GET_RANDOM_INT_IN_RANGE` alone number
  in the hundreds across just `poker_sp.ysc.c`). `sub_14273D42C` has 5 call
  sites, `sub_142E752A0` has 2, all in generically-named, unremarkable
  functions -- consistent with this being a shared crypto/networking
  utility (session tokens, key material, GUIDs) rather than the gameplay
  RNG's seed source. **Ruled out** as the answer to "what seeds the dice
  roll," not confirmed as anything poker-relevant.

### What this actually establishes

Nothing here overturns Session 14's finding that the AI's *decision* isn't
predetermined -- if anything it reinforces it: there is no per-hand reseed
anywhere in the script layer, so whatever stream `func_1628`'s noise rolls
draw from is the same continuously-running engine-wide stream everything
else in the game draws from (weather, ped behavior, loot, this exact same
native used for hundreds of unrelated things) -- **one shared global stream,
not a poker-specific one**, confirmed by there being a single native
hash per random-range call with no seed/stream-selector parameter anywhere
in any of the call sites traced across two sessions now.

On the actual algorithm/predictability question, this session did not reach
a conclusive answer -- every direct lead (name-based, string-based,
magic-constant-based, hash-table-based) came up empty or ruled itself out,
and the one solid remaining path (trace the VM's native-call opcode handler
to recover this build's hash-table lookup/obfuscation scheme, then follow
the recovered handler address for the two random-range natives to their
real implementation) needs more time than this session had. **Practical
bottom line as of right now: not shown to be predictable, but not shown to
be unpredictable either** -- the architectural shape found (single shared
engine-wide stream, never explicitly reseeded per hand, real crypto-RNG
usage ruled out as irrelevant) is consistent with a typical fast
deterministic PRNG seeded once at boot from *something* not yet identified,
which -- if true -- would mean its live state is a real, readable, and
forward-computable object somewhere in process memory (same category of
target as the `ScriptThreads` array this project already reads at runtime)
once actually located, rather than a value that needs "cracking" from a
secret seed. That's a hypothesis this session raises, not a confirmed fact.

### Open questions

- The VM's `CALLNATIVE` opcode dispatcher / native-hash-to-handler lookup
  for this exact build was never traced -- this is the actual blocking
  prerequisite for reaching `GET_RANDOM_INT_IN_RANGE`/
  `GET_RANDOM_FLOAT_IN_RANGE`'s real implementation directly instead of by
  inference. Likely findable the same way Session 2/3 found `ScriptThreads`
  (an AOB signature into the interpreter loop), but no such signature was
  in hand this session and none was derived.
- If/when that handler is found: confirm the actual generator algorithm,
  whether its state is a single global object or one instance per script
  thread/context, and whether that state's memory location is fixed enough
  (or AOB-findable) to read live from an ASI -- this is what would actually
  make "predict opponent N's next roll" tractable, not the algorithm ID
  alone.
- `sub_142DCF534`/`sub_142EA5834` (the "random" named-property fallback
  pair found off `sub_142E35900`) were noted but not decompiled/chased --
  low-confidence FX/property-bag lead, not re-visited after the stronger
  `BCryptGenRandom` lead also turned out to be a dead end.

## Session 16 -- the engine RNG cracked: algorithm, seed, and live state all found

Direct follow-on to Session 15's open question. The user manually labeled
the real functions in the IDA database themselves (`GET_RANDOM_INT_IN_RANGE`,
`GET_RANDOM_FLOAT_IN_RANGE`, `GET_GAME_TIMER` as a calibration anchor) inside
`D:\Backup\Stuff\RDR2 Shit\EXEs\1491.50\RDR2_Dumped.exe.i64`, which routes
straight past Session 15's actual blocker (the obfuscated native hash table
-- no longer needed once the target function is already named). Resolved
each by name via `idc.get_name_ea_simple()` and decompiled with Hex-Rays
through headless `idat.exe -A -S<script.py>` runs, same workflow as prior
sessions. Calibration check passed first: `GET_GAME_TIMER` (RVA `0x104BF98`)
decompiles to exactly `return (unsigned int)dword_1459B3930;` -- a plain
static-DWORD read, matching the user's own description exactly, confirming
the IDA-driving script and Hex-Rays are both working correctly on this
database before trusting anything harder.

### The algorithm: a 64-bit Marsaglia multiply-with-carry generator, one global instance

`GET_RANDOM_INT_IN_RANGE` (RVA `0x104B0F8`) and `GET_RANDOM_FLOAT_IN_RANGE`
(RVA `0x104B0B4`) are not thin wrappers around some deeper native -- their
entire body **is** the RNG, inlined directly:

```c
// GET_RANDOM_INT_IN_RANGE(a1, a2):
v2 = HIDWORD(qword_143E992B8) + 1557985959LL * (unsigned int)qword_143E992B8;
qword_143E992B8 = v2;
LODWORD(v2) = v2 & 0x7FFFFFFF;
return a1 + (unsigned int)((v2 * (a2 - a1)) >> 31);

// GET_RANDOM_FLOAT_IN_RANGE(a1, a2):
qword_143E992B8 = HIDWORD(qword_143E992B8) + 1557985959LL * (unsigned int)qword_143E992B8;
return (float)(qword_143E992B8 & 0x7FFFFF) * 0.00000011920929f * (a2 - a1) + a1;
```

This is a textbook **Marsaglia multiply-with-carry (MWC)** generator: treat
the 64-bit state as `{low32, high32}`, and each step computes
`newState = low32*A + high32` as a single 64-bit add (the carry out of the
32x32 multiply naturally becomes the next `high32`) -- multiplier
`A = 1557985959` (`0x5CDCFAA7`). The int variant takes the low 31 bits of
the *new* state (sign bit cleared) and scales into `[a1,a2)`; the float
variant takes the low 23 bits (mantissa-width) and scales the same way.
**Both natives share the exact same single global state word,**
`qword_143E992B8` (RVA `0x3E992B8`, in `.data`) -- confirmed by `XrefsTo()`
on that address turning up only 10 functions total, and every single one of
them (both natives, three unrelated internal helpers at RVAs `0x104375C`/
`0x437B4`/`0x58F80`, and three "shuffle/sample without replacement"-style
statistics helpers at RVA `0x250A820`/`0x250AAB0`/`0x250AC44`) performs the
identical `HIDWORD(s) + A*LODWORD(s)` step against it. This nails down
Session 15's "single shared engine-wide stream" finding as **literally one
64-bit variable**, not just an architectural inference -- and confirms (via
that same `XrefsTo` walk) `poker_sp.ysc.c`'s randomness and, e.g., weather/
ped-behavior/loot randomness elsewhere in the game are drawing from exactly
this one word, interleaved in whatever order those systems happen to call
the native in.

### Seeding: once at boot, from `QueryPerformanceCounter`, deterministically expanded -- but re-seedable too

`sub_14014FAF0` (RVA `0x14FAF0`) is the boot-time initializer: calls
`QueryPerformanceCounter(&PerformanceCount)` once, then feeds
`PerformanceCount.LowPart` (the QPC counter's low 32 bits) into a shared
seed-expansion helper, `sub_1425B6A94` (RVA `0x25B6A94`):

```c
sub_1425B6A94(_QWORD *state, unsigned int seed32) {
    v2 = (seed32 + (seed32 == 0))
       | ((seed32 ^ ROL32(seed32, 16)) << 32);
    *state = HIDWORD(v2) + 1557985959LL * (unsigned int)v2;   // one MWC step
}
```

i.e. the 32-bit seed is deterministically expanded into a 64-bit value (with
a same-shape bit-mixing trick, never producing an all-zero low word) and then
run through **one MWC step of the exact same algorithm** to produce the
actual initial state -- fully deterministic given the seed, no extra entropy
mixed in. `sub_14014FAF0` seeds *two* separate state words this way in the
same call, `qword_143E992B8` (the shared RNG this session is chasing) and a
second, apparently-independent one, `qword_143CE3C90` -- both from the exact
same `QueryPerformanceCounter` read, not two different reads (not chased
further -- out of scope, but worth knowing a sibling stream exists).

There is also a genuine **re-seed entry point**, `sub_141058F80` (RVA
`0x1058F80`): takes a caller-supplied 32-bit seed, runs the same
`sub_1425B6A94` expansion against `qword_143E992B8` directly, and also
resets three adjacent fields (`byte_143E992C0=0`, the qword pair at
`+12`/`+16` = 0, `dword_143E992CC = 0xBF800000` i.e. **-1.0f**) -- that
-1.0f is the classic "no cached Gaussian" sentinel pattern (Box-Muller-style
generators cache one of the two independent normal deviates they produce
per pair; -1.0f flags "cache empty"). This means the real RNG object is a
small ~24-byte struct (`{u64 mwcState; u8 hasCachedGaussian; u32 pad; u64
reserved; float cachedGaussianOrSentinel;}`) at base `qword_143E992B8`, not
just a bare integer -- `GET_RANDOM_INT_IN_RANGE`/`FLOAT_IN_RANGE` only ever
touch the first 8 bytes, but a Gaussian-flavored native (not looked for this
session) almost certainly touches the rest. `sub_141058F80` has 3 real
call sites (RVAs `0x47812`, `0x62601`, one more in a region IDA didn't
resolve a containing function for) -- not traced to see whether any of
these are reachable as a public script native (e.g. a `SET_RANDOM_SEED`-
shaped one); out of scope for "is poker predictable," since Session 15
already confirmed `poker_sp.ysc.c` never calls anything seed-shaped.

### Verdict: yes, practically predictable -- with one real caveat

This resolves Session 15's open hypothesis as **confirmed, not just
plausible**: the generator is a simple, fully deterministic, linear
recurrence over one 64-bit word sitting at a fixed, already-located
address (`qword_143E992B8`, RVA `0x3E992B8` -- ASLR-relative but a plain
static global, not something requiring a runtime allocation to chase) in
the *same process* our own ASI already runs inside of. Reading that one
qword at any moment gives everything needed to compute every subsequent
`GET_RANDOM_INT_IN_RANGE`/`GET_RANDOM_FLOAT_IN_RANGE` output **exactly**,
forward, indefinitely -- the output formulas above are pure bit-masking and
scaling, fully invertible/forward-computable, nothing hashed or
one-way about them. No seed-cracking is needed at all once the game is
actually running and readable -- this is a "read the live variable" problem
(same category as the `ScriptThreads` array and the deck cursor this
project already reads), not a cryptographic one.

**The one real caveat**: because it's confirmed to be *one global stream
shared by the whole game* (Session 15's finding, now nailed down to the
literal variable), any other system that happens to call
`GET_RANDOM_INT_IN_RANGE`/`FLOAT_IN_RANGE` between the moment we read the
state and the moment `func_1628`'s noise roll actually executes (weather,
ped AI, animation timing, or poker's *own* other random draws -- e.g. the
bet-size variance rolls that fire right alongside the fold/call/raise
decision roll in the same function) will have advanced the state past
whatever we predicted. Predicting a specific opponent's specific decision
roll precisely therefore isn't just "read the qword once at hand start" --
it needs either reading it immediately before the exact native call we
care about (tight timing), or counting/replicating every intervening
consumer's own draws, which is a real practical obstacle even though the
math itself is now fully solved.

### Open questions

- `qword_143CE3C90`, the sibling state word seeded alongside
  `qword_143E992B8` in `sub_14014FAF0` -- not chased. Could be a second
  independent gameplay stream, or something narrower (replay/determinism-
  specific); irrelevant to poker unless it turns out `func_1628`'s calls
  route through it instead under some condition not yet observed.
- The 3 call sites of the re-seed entry point `sub_141058F80` were not
  decompiled -- would confirm whether it's reachable from script at all
  (a public `SET_RANDOM_SEED`-style native) or purely an internal-engine
  reset (e.g. replay/network-sync rewind).
- Not measured empirically yet: how many *other* `GET_RANDOM_INT_IN_RANGE`/
  `FLOAT_IN_RANGE` calls typically fire per frame from non-poker systems
  while sitting at a poker table specifically -- this number is what
  determines whether "read state, predict opponent's next roll" is
  practically tight enough to act on, versus needing a same-instant read
  right before the specific decision. Would need an in-game
  instrumented-read test (log the qword on every poker-relevant frame and
  diff against actual observed AI decisions), not a static-analysis
  question.

## Session 17 -- fold/call/raise prediction feature dropped

User asked (after Session 14) whether a given bet size could be predicted
to make a specific opponent fold. Sessions 14-16 chased this all the way
down: `func_1628` (Session 14) is the real per-seat decision engine, its
confidence score is a blend of live equity + personality + position + a
randomized noise roll, and that roll's RNG (Sessions 15-16) turned out to
be fully solvable in principle -- one global 64-bit Marsaglia
multiply-with-carry word (`qword_143E992B8`), forward-computable exactly
from a live read, no cryptography involved.

**Decision: not pursuing this feature.** The blocker isn't the math, it's
that `qword_143E992B8` is one stream shared by the entire game -- weather,
ped AI, animation timing, and poker's *own* other random draws (bet-size
variance rolling right alongside the fold/call/raise roll in the same
`func_1628` call) all consume from it in whatever order those systems
happen to fire. Predicting one specific opponent's specific roll requires
either reading the state in the exact instant before that specific native
call (not currently feasible from an external ASI tick, which runs at a
coarser cadence than the game's own per-frame script scheduler) or
correctly counting every intervening draw from every other system, which
is not practically tractable. User's read on this ("flipping a coin would
be more accurate than what you're proposing") is directionally right as
an engineering call even though the generator itself isn't weak -- a
correct-in-theory prediction that silently desyncs the moment any
unrelated system rolls the same global counter is worse than no
prediction at all, since it would confidently show a wrong number instead
of admitting uncertainty. Threshold-ladder constants (`func_1712`/
`func_1713`/`func_1718` cutoff tables, `func_1708`/`func_1709`/
`func_1715`-`func_1719` helpers, all noted un-traced in Session 14) were
never pulled for the same reason -- no longer useful without the RNG
timing problem solved.

Nothing to revert -- Sessions 14-16 were research-only, no code changes.
The advisor HUD (live hand/equity display, Session 3 onward) is unaffected
and remains the mod's actual feature set.

## Session 18 -- opponent personality/style label, built on Session 14's dispatcher trace

User asked for other feature ideas after Session 17 dropped fold
prediction. Proposed a short list, all grounded in already-traced game
state rather than new equity/RNG territory; user picked the cheapest one:
show each opponent's AI personality/style (Loose-Aggressive,
Tight-Passive, etc.) on the HUD -- Session 14 already found the field,
this session just wired it up and pinned down the exact tight/loose x
passive/aggressive meaning of each value.

### Address confirmed via a literal call site, no parameter-identity tracing needed

`func_185` (poker_sp.ysc.c line 8975) is the only function that ever
writes a seat's personality index, and its one call site
(line 5883) is `func_185(&(uLocal_14.f_114.f_2655), i)` -- `uLocal_14`
appears verbatim, so this needed none of the multi-hop call-chain tracing
Table's own address required back in Session 2/3. Absolute local slot:
`14 (uLocal_14) + 114 (f_114) + 2655 (f_2655) + 90 (f_90) + seat` = 2873 +
seat, seats 0-5 -> slots 2873-2878. `f_90` is a plain 0-based array (no
leading size/count header word, unlike Table.f_15/f_39) -- confirmed by
func_185's own body (`unk[uParam0->f_90[i]]`, `uParam0->f_90[iParam1] =
j`) indexing it directly with the raw seat number, and independently by
the reset loop at the end of func_584 (`for (i=0;i<6;i++)
uParam0->f_90[i] = 0;`, line 25490-25492) doing the same.

### Correcting Session 14's index range: 0-8 all route to the real engine, not just 5-8

Re-reading func_584's 15 `func_1191(table, index, p1, p2, styleCode)`
calls (lines 25441-25455) directly (rather than relying on Session 14's
paraphrase) shows styleCode is 0 for **every** index 0-8, not just 5-8 --
indices 9-14 are the ones carrying styleCodes 1-6 (the card-blind
archetypes). So the full 0-8 range are all real, equity-driven `func_1628`
personalities (Session 14 was right about which funcs the styleCodes
dispatch to, just imprecise about which indices carry styleCode 0).
`func_185` (the seat-fill assignment) still only ever rolls
`GET_RANDOM_INT_IN_RANGE(5, 8+1)` -- i.e. only indices 5-8 ever actually
reach a real seat -- so this doesn't change what a player will ever
observe, just corrects the record on indices 0-4's actual nature (real
personality-grid entries that are simply never rolled, not scripted NPC
placeholders as previously guessed).

The (p1, p2) pairs across indices 0-8 turn out to cover a complete 3x3
grid over {0,1,2}x{0,1,2}: `p1` indexes `f_9` (`{1.25, 1.0, 0.8}` for
p1={0,1,2}, set at lines 25456-25458) -- a personality-specific "how much
do I trust my own equity read" multiplier feeding directly into
`func_1628`'s confidence score (Session 14) -- so p1=0 inflates equity
(**Loose**), p1=2 discounts it (**Tight**), p1=1 is neutral. `p2` indexes
`f_13` (three 10-float bet-size-range rows, lines 25459-25488) -- every
field of row 2 > row 1 > row 0 with only one minor exception (`.f_4`), so
p2=0/1/2 is **Passive**/Neutral/**Aggressive** by bet-size-range
magnitude. Indices 5-8 (`func_185`'s only real outputs) are exactly the
four corners of that grid: 5=(0,0) Loose-Passive, 6=(2,0) Tight-Passive,
7=(0,2) Loose-Aggressive, 8=(2,2) Tight-Aggressive.

### Implementation

`PokerCheat.cpp`: `kPersonalityIndexBase` constant (the slot math above),
`PersonalityLabel()` (full 0-14 switch, though only 5-8 should ever
actually appear at a table -- the rest filled in for robustness), and a
new `Config::ShowOpponentPersonality` toggle (defaults on, `Config.h`/
`.cpp`, same General-section pattern as `ShowOthersCards` etc.).
`DrawSeatCardIcons()` gained a `personalityLabel` parameter and now
shares its one label line between the personality tag and the existing
(You Win)/(They Win)/(Tie) tag -- independently toggled, either can show
without the other; when the win/lose half isn't showing, the line falls
back to a neutral cream color instead of the win/lose green/red/yellow.
Also appended to the Debug text panel's per-seat line (`[Tight-Aggressive]`
etc.) for live verification against the real screen. Both Debug and
Release configs compile clean; not yet run against a live game (RDR2 was
running at the time, so the deploy copy step was skipped by the user's
own choice -- next session or the user's own rebuild should confirm the
label reads correctly and lands in a sane position relative to the
existing win/lose tag).

### Open questions

- Not yet confirmed live: does `PersonalityLabel()` ever actually show
  anything other than the four expected corner values (Loose-Passive/
  Tight-Passive/Loose-Aggressive/Tight-Aggressive) at a real table --
  would confirm `func_185`'s `[5,8]` range and this session's slot math
  both hold up outside the decompile.
- Indices 9-14's real assignment path (same open question carried over
  from Session 14) still not found -- only matters if a named story
  character's poker behavior needs this label to make sense.

## Session 19 -- localizing the HUD's on-screen text

Scoped down first: grepped every `DRAW_TEXT`/`_BG_DISPLAY_TEXT` call site
and found only two strings a Release build ever actually shows a player --
the (You Win)/(They Win)/(Tie) verdict wording (`DrawSeatCardIcons()`'s
tag and `DrawWinPredictionStatus()`'s standalone readout both had their
own identical hardcoded copy) and `PersonalityLabel()`'s 14 opponent
style tags. Everything else text-shaped in this file (`HandCategoryName()`,
the whole seat/board debug panel) is `#ifdef _DEBUG`-only -- confirmed via
`script.cpp`: the F10 menu itself, including the "Toggle Poker Cheat" item
that's the only way to turn the mod on/off, is Debug-only too (Release
self-enables via `PokerCheat::SetEnabled(true)` in `ScriptMain`) -- so none
of that is player-facing and stays English-only.

### Language detection

Checked `rdr3-nativedb-data/natives.json` for `LANGUAGE::_GET_CURRENT_LANGUAGE_ID()`
(hash `0xDB917DA5C6835FCC`, already correctly declared in the stock SDK's
`natives.h`): its own comment documents the exact 13 languages RDR2 ships
with (0=en-US, 1=fr-FR, 2=de-DE, 3=it-IT, 4=es-ES, 5=pt-BR, 6=pl-PL,
7=ru-RU, 8=ko-KR, 9=zh-TW, 10=ja-JP, 11=es-MX, 12=zh-CN). Used that as the
default language source -- matches whatever the player already has RDR2's
own UI set to, no config needed -- with `PokerCheat.ini`'s new `[General]
Language` key (`Config::Values::Language`, default `"auto"`) as an
explicit override for anyone who wants the HUD in a different language
than their game UI.

Side finding, not acted on: the stock SDK's neighboring stub
`LANGUAGE::_GET_USER_LANGUAGE_ID()` (hash `0x76E30B799EBEEA0F`) is
mismapped -- nativedb shows that hash is actually
`LOCALIZATION_GET_SYSTEM_DATE_TYPE`, not a language getter. Left it alone
(unused) rather than "fixing" it in `ExtraNatives.h`, since
`_GET_CURRENT_LANGUAGE_ID` alone is all this feature needs.

### Implementation

New `Localization.h/.cpp`: a `Language` enum mirroring the native's 13
index values, `Refresh()` (resolves the ini override or falls back to the
native, same "resolve once, cache" pattern `Config::Get()`'s `g_loaded`
bool already uses -- see `Current()`), `VerdictLabel(vsResult)`, and
`PersonalityLabel(personalityIndex)`. `PersonalityLabel()`'s old
derivation comment (the `func_584`/`func_1191`/`func_185` trace from
Session 14/18) moved from `PokerCheat.cpp` to sit above `Localization.cpp`'s
`kPersonalityLabels` table, which it now actually documents.

One real gotcha caught before it shipped: `Localization::Refresh()` calls
a real game native, so -- unlike `Config::Reload()`, which `main.cpp`'s
header comment explicitly notes is safe from `DllMain` because it never
touches game natives or script-hook state -- it must run from inside
ScriptHookRDR2's own script fiber. Solved the same way `Config::Get()`
avoids needing an explicit `DllMain` call at all: `Current()` lazily calls
`Refresh()` on first use, and its first-ever call will always come from
`OnTick()` (already running inside the fiber). The Debug F10 menu's
"Reload Config" item was widened to a small `ReloadConfigAndLocalization()`
wrapper in `script.cpp` so editing `PokerCheat.ini`'s new `Language` key
live re-picks the language immediately, same live-tuning workflow as
every other Config-backed value.

Translated `kPersonalityLabels`/`kVerdictLabels` for all 13 languages
in one pass (LLM-assisted, not reviewed by a native speaker per
language) rather than starting with only a few -- if a wording turns out
wrong, fix that language's row directly in `Localization.cpp`, no other
file needs to change. Kept several poker terms (Calling Station, All-In
Shover, Push/Fold) as the English loanword in languages where that's
genuinely how poker communities use them rather than forcing a literal
translation.

Confirmed the project's existing `/utf-8` `AdditionalOptions` (already
present in both configs' compiler flags, `PokerCheat.vcxproj`) makes the
Cyrillic/Korean/CJK string literals compile to correct UTF-8 bytes --
checked directly by grepping the built `.asi` for known UTF-8 byte
sequences (`Ничья`, `あなたの勝ち`, `跟注站`), all found intact. Both
Debug and Release build clean; `PokerHandEvalTests` (untouched by this
change) still `ALL PASS`.

### Open questions

- Not yet confirmed live in any non-English language: whether the fixed
  legacy font `UIDEBUG::_BG_DISPLAY_TEXT` actually renders through (see
  `DrawWinPredictionStatus()`'s header comment -- no `SET_TEXT_FONT`
  equivalent exists, and this pipeline was only ever proven out in
  English) has glyph coverage for accented Latin, Cyrillic, or CJK at
  all -- it may fall back to tofu/blank boxes for some or all of the
  non-English rows above. Needs an actual in-game check per language
  (force `PokerCheat.ini`'s `Language` override, one language at a
  time) before trusting any of this beyond English.
- Poker jargon translations (Calling Station, All-In Shover, Push/Fold,
  Pot-Cap Gate especially) are a best-effort pass, not verified against
  what real poker communities in each language actually call these --
  worth a native-speaker review pass per language before calling this
  fully "done" rather than "shipped and probably close."

## Session 20 -- restored the font test, now per-language

Directly follows Session 19's first open question: nothing had actually
confirmed any font token has non-ASCII glyph coverage, so before trusting
12 of the 13 new translations at all, brought back Session 11's font test
diagnostic (`DrawFontTest`/`ToggleFontTest`, removed in the commit
documented as "Replaced the 'Toggle Font Test' F10 diagnostic" once
`$Font5` was confirmed for English -- see `git show bbc8d9d` for the
removal, `git show 29deeab:src/PokerCheat.cpp` for the original).

Same structure as the original (one row per candidate `FONT FACE` token
-- `$title`/`$chalk`/`$ledger`/`$body1`/`$catalog1`/`$Font5`/
`$gamername` -- rendered through the confirmed-working
`UIDEBUG::_BG_DISPLAY_TEXT` pipeline, dropped the old pipeline's rows
since that native pair was already confirmed nullsub on this build), but
parameterized by language instead of hardcoding English "Face test":
`PokerCheat.h`/`.cpp` gained `FontTestEnabled`/`FontTestLanguageIndex`,
`ToggleFontTest()`, and a new `CycleFontTestLanguage()` (advances the
index with wraparound, logs the new language's code) so all 13 languages
can be checked one at a time from the same F10 menu, without needing 13
separate screens' worth of rows on-screen simultaneously. Each row's
sample text is that language's own actual `PersonalityLabel(lang, 8)`
("Tight-Aggressive", one of the four real corner values, picked for
usually being the longest of the four so a partial glyph failure is
easier to spot) plus `VerdictLabel(lang, 1)` ("(You Win)").

Required widening `Localization.h`'s API: the existing
`VerdictLabel()`/`PersonalityLabel()` only read `Current()` (the real
detected/overridden language), which isn't useful for a diagnostic that
needs to force-render a language the game itself isn't currently set to.
Added `Language`-taking overloads of both (the no-arg versions now just
forward to `Current()`), plus `LanguageCode(Language)` for the on-screen/
log-line language labels ("en-US" etc).

Both Debug and Release build clean (the whole feature is `#ifdef
_DEBUG`-gated, same as the original). Not yet run against a live game --
next step is the user cycling through all 13 languages in-game and
screenshotting which token/language combinations show real characters
vs. tofu, which will decide whether kPersonalityLabels/kVerdictLabels'
non-Latin rows need a different token wired into the real HUD calls
(`DrawSeatCardIcons()`/`DrawWinPredictionStatus()` both currently hardcode
`$Font5`) or need to fall back to English for scripts nothing can render.

### Open questions

- Still the actual question this whole session exists to answer: which
  (if any) of the 7 tokens renders real glyphs for each of the 12
  non-English languages. Nothing here can determine that without a live
  game session.
- If NO token renders a given script (plausible for CJK/Korean
  specifically, since RDR2's legacy text-draw path may only ever have
  shipped with a Latin/Cyrillic-range font atlas), that language's rows
  in `kPersonalityLabels`/`kVerdictLabels` should probably fall back to
  English rather than silently drawing invisible/tofu text -- not
  implemented yet, pending the actual in-game result.

## Session 21 -- Session 20's open question answered: it was a test methodology bug, not a font limitation

First live test (user's report): cycling the font test through all 13
languages while playing in English showed every token rendering fine for
every Latin-script language and Russian, but Korean/Japanese/both Chinese
variants came back as tofu/blank boxes on EVERY one of the 7 tokens,
including `$Font5`. Before writing an English-fallback for those four
languages (the plan Session 20 left open), checked the actual font
library those tokens draw from: pulled the file listing from
`github.com/ExpMero/rdr2_fonts` (the same repo Session 11 cross-confirmed
token names against) --

```
$chalk, $wantedPostersGeneric, $catalog4, $catalog2, $catalog1,
$RockstarTAG, $SOCIAL_CLUB_COND_REG, $body1, $gamername,
$FixedWidthNumbers, $body2, $handwritten, $catalog5, $ledger, $Debug_REG,
$title1, $Font5_limited, $catalog3
```

-- 18 fonts total, all Latin/symbol faces (RDR Lino, Droid Serif,
HelveticaNeue, Arial, etc.), zero CJK-capable entries, and `$Font5`'s
real name is literally `Font5_limited_Redemption` ("limited" character
set, right in the name). This looked at first like confirmation that the
whole legacy pipeline is CJK-incapable by construction -- but this
extraction is necessarily from an English-language game install, and a
game that genuinely ships full Chinese/Japanese/Korean localizations
obviously renders CJK somewhere. Realized the actual variable never
controlled for: RDR2 (same as GTA V) only streams a language's font/text
assets into memory for the language it's ACTUALLY configured to run in
(Steam Properties -> Language, requiring a relaunch) -- `PokerCheat.ini`'s
`Language` override only changes which of `Localization.cpp`'s strings
THIS MOD draws, it can no more make the base game load Chinese font
assets than editing a subtitle file could. Asked the user to confirm --
they had been testing entirely in English/default with only the ini
override changed, exactly this gap. Documented as a real methodology
pitfall (`docs/PITFALLS.md`) so it isn't relearned next time a
non-default-language render needs checking.

Re-tested with RDR2's REAL language actually switched to Chinese (Steam
Properties, relaunched): every token rendered CJK correctly except
`$gamername` (plausibly restricted to the fixed Latin/numeral gamertag
charset its real name, "Rockstar Gamertag Cond", implies -- never
confirmed further, not worth chasing since this mod doesn't use it
anyway). `$Font5` -- already this mod's actual choice for
`DrawSeatCardIcons()`/`DrawWinPredictionStatus()`, unchanged since
Session 11 -- was among the tokens that worked. Net result: **no code
change needed** for the real HUD; the localization work from Session 19
was correct as shipped, provided a real Chinese/Japanese/Korean player
has their own game genuinely set to that language (which, for an actual
speaker of it, they will be). Only touched comments in
`PokerCheat.h`/`.cpp` (`ToggleFontTest()`/`DrawFontTest()` header
comments) to record the confirmed result instead of the prior "unknown"
framing, plus this entry and the `PITFALLS.md` addition.

### Open questions

- None outstanding for font rendering -- closed. `$gamername`'s CJK
  failure specifically was noted but not root-caused; irrelevant unless
  this mod ever has a reason to use that token.

## Session 22 -- spot-checking the personality-label translations against real poker-community usage

User asked directly how confident the Session 19 translations actually
were. Honest answer at the time: not very, for the poker-jargon
personality labels specifically -- they were LLM-assisted guesses, never
checked against how real poker communities in each language actually
talk, and poker vocabulary is unusually loanword-heavy (many languages
just keep "tight"/"loose"/"all-in" in English rather than translating),
which is exactly the kind of thing a plausible-sounding literal
translation gets wrong silently.

Reprioritized before spending research effort: reread `PokerCheat.cpp`'s
own `kPersonalityIndexBase` comment -- of the 15 personality values, only
indices 5-8 (Loose-Passive/Tight-Passive/Loose-Aggressive/Tight-Aggressive,
the four corners of the p1xp2 grid) can ever actually appear at a real
seat; `func_185` never rolls anything else. So checked those four
specifically, per language, via web search against real poker glossaries/
forums/strategy sites, rather than spreading effort evenly across all 15
values including several (Calling Station, All-In Shover, Push/Fold,
Pot-Cap Gate) that are dead code paths in practice.

Results, by language:

- **German, Spanish/es-MX, Italian, Portuguese, Polish, Japanese,
  Korean**: confirmed correct as shipped. All seven keep "Tight"/"Loose"
  as English loanwords (e.g. German "tightes Spiel", Italian/Portuguese/
  Polish article titles literally read "Tight Aggressive" in Latin
  script, Japanese `タイト`/`ルース` and Korean `타이트`/`루즈` are
  transliterations), exactly what Session 19 had already written.
- **Russian**: confirmed correct as shipped -- real forum usage
  (`nashpoker.net`, `academypoker.ru`) fully translates/transliterates
  and hyphenates exactly the way this row already did (`Тайтово-
  агрессивный`, `Лузово-пассивный`).
- **French**: WRONG, fixed. Had translated "Tight"/"Loose" to "Serré"/
  "Lâche"; real usage (`pokerstrategy.com/fr`, `caen-poker.com`) is
  "tight-agressif" -- "Tight" kept in English same as every other
  language, only the second half Frenchified.
- **Chinese, both variants**: WRONG, fixed. Had used the generic word for
  "aggressive" (`激进`/`激進`, closer to "radical/extreme"); real poker
  terminology (`dpskill.com` for Simplified: "紧凶"/"松凶";
  `monsterstack.com.tw` for Traditional: "緊兇"/"緊積極(緊兇)") uses the
  specific poker-slang character `凶`/`兇` instead. "Passive" (`被动`/
  `被動`) was already fine -- a legitimate near-synonym of the Traditional
  source's alternate term `消極`.

Applied both fixes directly to `kPersonalityLabels` in `Localization.cpp`,
with the supporting citation left as a comment on each corrected row so
a future reviewer doesn't have to re-derive where the correction came
from. Rewrote the table's header comment to state the real confidence
picture precisely instead of a blanket "LLM-assisted" disclaimer: indices
5-8 across these ten languages are now web-search-checked, French and
Chinese were caught and corrected, and everything else (indices 0-4/9-14,
plus all 13 languages' `VerdictLabel` wording, which is plain vocabulary
rather than jargon and was left unchecked as lower-risk) remains
best-effort. Both Debug and Release rebuilt clean; spot-checked the new
Chinese/French byte sequences directly in the built `.asi`, same
verification method as Session 19.

### Open questions

- `VerdictLabel`'s wording (the "(You Win)" family) was never
  web-search-checked, on the judgment call that it's plain vocabulary
  rather than jargon and lower-risk than the personality labels -- still
  only as confident in it as in any other LLM-generated translation.
- The never-actually-shown personality values (indices 0-4, 9-14 --
  Calling Station/All-In Shover/Always All-In/Push/Fold/Pot-Cap Gate)
  remain unchecked. Low priority since `func_185` can't roll them at a
  real table, but if that ever changes (or a named story character uses
  one, the open question carried since Session 14/18), they'd need the
  same treatment.

## Session 23 -- ScriptLocal migration; the personality index was one seat off

Ported BlackjackCheat's `ScriptLocal` (itself adapted from HorseMenu's
`game/rdr/ScriptLocal.hpp`) into `src/ScriptLocal.h` and rewrote every
script-local read in `PokerCheat.cpp` as a chain in the decompile's own
terms: `.f_N` -> `At(N)`, `x[i /*S*/]` -> `At(i, S)` (skips the array's
size word itself). The flat slot constants (`kTableSlot`, `kBoardSlotB`,
`kSeatsDataBase`, `kDeckSlot` + `kDeckCardsBaseOffset`,
`kCommunityCardObjectsBase`, ...) and `ReadInt()` are gone. A block of
`static_assert`s pins each chain to the slot the old constants read
(Table A 415 / B 1404, board card 0 1420, hole card 0 1452, deck card 0
2011, cursor 2115, scene props 4007, ...), so the port can't have moved a
live-confirmed read. `kDeckCardsBaseOffset = 1` (Session 9's empirical
"there's a 1-word field before the cards") turns out to be exactly the
deck array's size word: `var uLocal_2010 = 52;`.

### Bug found: `f_2655.f_90[seat]` has a size word

Session 18 read the personality index at `2873 + seat`, on the belief
that `f_90` was a plain array with no size word ("func_185 indexes it
directly with the raw seat number"). But indexing with `[i]` is exactly
how the decompiler writes every sized array, `f_15`/`f_39` included. The
local declarations settle it: `var uLocal_2873 = 6;`, the same pattern as
`uLocal_430 = 11` (A's board), `uLocal_454 = 6` (A's seats) and
`uLocal_4006 = 5` (the scene's card props). So the old read got the size
word (6, "Tight-Passive") for seat 0 and seat N-1's personality for seat
N. Nobody noticed because `func_185` only rolls 5-8, so every shifted
value was still a real label. `PersonalityLocal(seat)` =
`f_114.f_2655.f_90.At(seat, 1)` reads `2874 + seat`. The only
`static_assert` that pins a NEW slot is this one.

Verified: all size words cross-checked against the `var uLocal_N = size`
declarations in `poker_sp.ysc.c`, Debug + Release build (the
static_asserts compile), PokerHandEvalTests and LogFallbackTests pass.
CONFIRMED LIVE (2026-09-24, `PokerCheat_stackdump_20260924_142610.jsonl`,
hand in progress, f_2010 = 4, my seat f_9 = 5). Slots 2873..2879 read
6, 8, 7, 6, 6, 0, 0. Seat occupancy (Table.f_39[i].f_0): AI at 0/1/2,
empty at 3/4, me at 5. New read (2874 + seat): 8, 7, 6 for the three AI
seats, 0 for me, 0 for seat 4 (never dealt: hole cards -1). Seat 3 reads
6, but it still holds stale hole cards (2/2, 14/2), so an AI sat there
earlier and left; nothing clears f_90[seat] on leave, and the overlay
skips empty seats anyway. The old read (2873 + seat) would have given the
never-used seat 4 personality 6 and seat 0 the size word, and labeled
the AI seats Tight-Passive/Tight-Aggressive/Loose-Aggressive instead of
the real Tight-Aggressive/Loose-Aggressive/Tight-Passive. Nothing in the
game's own UI shows a seat's personality, so this rests on the memory
reads plus the decompile's zero-init/assign-on-AI-seat logic.

## Session 24 -- bet hotkeys, ported from BlackjackCheat

Right/Left arrow move the bet 5 chips (hold to repeat, func_984's own
ramp: 7 steps/s, x1.08 per repeat, capped at 200/s), Tab jumps to the
most the game allows. Gated by the new `BetHotkeys` INI key (default on).
Covers both amount-entry UIs, all in `uLocal_14.f_2979` (static trace,
see the layout block in `PokerCheat.cpp`):

- Bet/raise, `f_2979.f_281`: opened by func_652 on your turn only
  (func_648: `f_114.f_9 == seat`), run by func_1252. Amount `f_281.f_4`
  is chips on top of the street bet `seat.f_4`; clamped to func_1614's
  [check/call, min raise, max] with anything between call and min raise
  snapping by direction -- `BetInputLimits()`/`ClampBet()` copy that.
- Buy-in, `f_2979.f_298`: func_389/func_390, amount `f_298.f_2`,
  clamped to the table's `f_114.f_10.f_12/f_13` capped at cash.
- `f_2979.f_157` is cents per chip. Both repeat structs start with the
  static `uLocal_3279`/`uLocal_3294 = 1088421888` (7f), which pins the
  slots with static_asserts.

Tab's prompt text is the game's own: `MGPKR_INFO_ALLIN_DONE` ("All-in")
when Tab would put in the whole stack, else `MGPKR_INFO_MAX_BET_DONE`
("Max Bet"). Found by extracting `mgpkr.yldb` (poker_sp's own text block,
`TEXT_BLOCK_REQUEST("MGPKR")`, not in `global.yldb`) from
`GXT\<lang>_rel.rpf` with RDR2 RPF Tool and matching every `MGPKR_*`
literal in the scripts against it with `..\ChallengeCheat\tools\
dump_labels.py`. Passed as `VAR_STRING(2, label)`, so the game localizes
it (fr "Tapis", not our old "MISE MAX").

### Why Left/Right have no prompt of their own

Established live, over several test builds:

- A prompt shows only if EVERY control on it is in the active input
  context. `_SET_CONTROL_CONTEXT`'s first argument is a layer; only the
  top non-empty layer is active. At your turn: layer 0 = OnFoot, layer 4
  = MinigamePoker (poker_sp's func_10, every frame).
- MinigamePoker has no control on the arrow keys. Every arrow control
  (GAME_MENU_, FRONTEND_, FRONTEND_NAV_, FRONTEND_MAP_NAV_LEFT/RIGHT,
  DOCUMENT_PAGE_PREV/NEXT) hides the prompt -- alone, or with an
  in-context control added in either order.
- In-context but unbound (MULTIPLAYER_INFO_PLAYERS, POKER_CHEAT_LR):
  hidden. In-context gamepad-only (HELP_PREV): shows, with a D-pad icon.
- Not a count limit: 8 prompts showed at once; an arrow prompt put in
  All-in's place (All-in unregistered) stayed hidden.
- MinigameBlackjack on layer 5 made the arrow prompt show, but replaced
  MinigamePoker: Bet/Fold/Your Cards/Community Cards vanished.
- `_UI_PROMPT_IS_ACTIVE` is 0 even for prompts on screen -- not a
  visibility test.

Instead the step goes on the game's own Amount prompt, as
"-/+$0.25 <Left><Right> Amount <Up><Down>": inline `~INPUT_...~` icons
render in LITERAL_STRING prompt text (they don't light up on a press).
The prompt handle is `Global_1945188[slot /*18*/].f_3`, slot from
`f_281.f_16` (`f_298.f_13` for buy-in), read through the new
`ScriptGlobal.h` (ScriptLocal's rules; `At(i, stride)` skips the size
word). The game draws a prompt's own icons after its text, so "Amount"
goes last. Adding the arrows to MinigamePoker's control list in memory
would allow a real prompt; not worth it.

Also: Arthur's seat portrait sometimes showing the blank `avatar_generic`
is poker_sp's own fallback (func_1197) when his headshot
(`Global_1899750`) isn't ready within 5 s (func_367) -- unrelated to the
mod.

### Open questions

- The buy-in UI's hotkeys and relabeled Amount prompt are untested live.
- Not yet confirmed that a Tab all-in places (and pays) the amount shown.
