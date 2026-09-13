# Changelog

All notable user-facing changes to PokerCheat are recorded here. Format
loosely follows [Keep a Changelog](https://keepachangelog.com/); this
tracks what an end user experiences, not internal implementation history
(see `JOURNAL.md` for the full session-by-session derivation/bugfix log).

## [1.2.0] - 2026-09-13

### Added
- Opponent personality/style HUD label: each opponent's fixed
  Loose/Tight x Passive/Aggressive personality (read directly from
  the seat's personality index, not predicted or RNG-derived) is now
  shown next to their card icons. Gated by a new
  `ShowOpponentPersonality` toggle in `PokerCheat.ini`.

### Debug build
- `Dump Full Stack JSONL` no longer risks silent precision loss on
  64-bit values in its output -- raw `i64` stack reinterpretations are
  now emitted as quoted strings instead of bare JSON numbers, since
  they routinely exceed the 2^53 range double-based JSON parsers can
  represent exactly.

## [1.1.0] - 2026-09-11

### Fixed
- Release advisor no longer gets silently switched off if
  ScriptHookRDR2 restarts the script mid-session -- previously this
  left the HUD off for the rest of the session with no menu to
  re-enable it from.
- HUD (predicted board, per-seat icons, win/lose/tie readout) now
  correctly hides itself during all-in runout and postflop betting
  resolution instead of overlapping the game's own hand-resolution
  text, and reappears the instant the next hand's hole cards are
  dealt. Driven directly off the game's own hand-state field instead
  of an indirect heuristic.

### Debug build
- Replaced the "Toggle Font Test" F10 diagnostic (its job was already
  done -- confirmed the real-font rendering pipeline now used
  unconditionally by the HUD) with "Dump Full Stack JSONL", a raw
  script-memory dump tool for future reverse-engineering work.

## [1.0.0] - 2026-09-11

Initial public release.

### Added
- Live hole-card reading for every seat at the table, not just the
  player's own.
- Predicted final board: combines currently-revealed community cards
  with the exact cards still coming off the (already-shuffled,
  deterministic) deck, so every seat's hand is evaluated against the
  real showdown board from as early as preflop.
- Self-contained 7-card poker hand evaluator (category + full kicker
  tiebreak), independent of the game's own hand-rank native and
  unit-tested against known hands.
- Per-opponent hole-card icons drawn on the real HUD next to each
  seat's name, tagged `(You Win)` / `(They Win)` / `(Tie)`.
- Standalone win/lose/tie prediction readout, positioned independently
  of the per-seat icons.
- Folded and all-in seats correctly distinguished and excluded from
  win-eval where appropriate.
- `PokerCheat.ini` with independent per-feature toggles
  (`ShowCommunityCards`, `ShowOthersCards`, `ShowWinPrediction`,
  `ShowWouldWinHandAgainst`) under `[General]`, auto-created on first
  run.
- Zero-setup release build: overlay is enabled automatically on
  injection, no menu or keybind required.

### Notes
- Targets game build **1491.50** specifically -- struct offsets are
  hardcoded from that build's decompiled scripts and are not
  guaranteed to hold on any other build.
