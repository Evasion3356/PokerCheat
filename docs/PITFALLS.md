# Pitfalls, dead ends, and lessons learned

Read this before touching native calls or script-patching in this project.
Nothing has actually gone wrong yet -- this file is seeded with lessons
already paid for in the sibling `CollectorOffline` project (same toolchain,
same machine, same general MP-vs-SP hazards), so they aren't relearned the
hard way here too.

## Carried over from CollectorOffline

- **MP-only content wall.** Rockstar's own compiled scripts and some game
  systems are gated to real multiplayer sessions in ways that aren't
  obvious from the outside, and the failure mode is usually a **crash**,
  not a clean error. `poker_sp` is explicitly the single-player script
  (there's a separate MP poker script), so this project should be safe by
  construction -- but if the trace ever leads into `act_gen_poker.ysc.c`
  code paths shared with an MP variant, check for
  `NETWORK_IS_GAME_IN_PROGRESS`/session-only global-array guards before
  assuming a code path is safe to trigger from SP.
- **Trace decompiled source precisely, don't reconstruct from memory.**
  CollectorOffline hit two real bugs from paraphrasing a decompiled
  function instead of re-reading it. If a cheat's exact condition/branch
  matters, `Read`/`grep` the actual `.ysc.c` file again before writing
  code or claiming a variable's purpose.
- **Log before and after every native call whose success isn't visually
  obvious.** This project's `Log::Write` (see `src/Log.h`) is already
  wired up -- use it liberally once real cheat logic exists, same
  "read the log, not the screen" methodology as CollectorOffline.
- **Don't hand-guess struct offsets for this exact game build.** If the
  cheat mechanism ends up needing `rage::scrProgram` field offsets (see
  JOURNAL.md's HorseMenu reference), confirm them for 1491.50 specifically
  rather than trusting a value from another build/game. CollectorOffline's
  `CLAUDE.md` documents a already-analyzed IDA database for this exact
  build (`EXEs\1491.50\RDR2_Dumped.exe.i64`) that's the right tool for
  that confirmation.

Add new entries above this line as real mistakes happen.
