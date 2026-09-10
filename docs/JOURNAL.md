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
