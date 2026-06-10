# Testing the pad-injection + polled-watchpoint features

New in this revision:

- **DebugServer commands** (C++, port 21512): `pad_set`, `pad_press`, `watch_change`,
  `watch_list`, `watch_clear`
- **MCP tools**: `pcsx2_pad_set`, `pcsx2_pad_press`, `pcsx2_watch_change`,
  `pcsx2_watch_list`, `pcsx2_watch_clear`

These can only be validated end-to-end with a game running. The human starts PCSX2
and loads the game; an agent (or you, with `nc`) then drives it.

## 0. Prerequisites

- The patched PCSX2 build (`/Users/abe/Desktop/pcsx2/build/pcsx2-qt/PCSX2.app`,
  re-signed after build with `xattr -cr` + `codesign --force --deep -s -`).
- Pad port 1 must be a **DualShock 2** (PCSX2 default). Injection is a no-op for
  other controller types.
- The VM must be **running** (not paused) for pad injection — input is applied
  once per frame at vsync, and `pad_press` frame counts only tick while the game runs.

## 1. Protocol smoke test (no MCP, raw TCP)

With a game running:

```sh
# Tap X (CROSS = 0x4000) for 12 frames (~200ms NTSC)
printf '{"cmd":"pad_press","buttons":16384,"frames":12}\n' | nc 127.0.0.1 21512

# Hold START until cleared
printf '{"cmd":"pad_set","buttons":8}\n' | nc 127.0.0.1 21512
printf '{"cmd":"pad_set","clear":true}\n' | nc 127.0.0.1 21512

# Unknown-command listing should now include the 5 new commands
printf '{"cmd":"help"}\n' | nc 127.0.0.1 21512
```

Expected: `{"ok":true,...}` for each; the game visibly reacts (menu advance, pause
screen, etc.). On the PCSX2 stderr console, watch triggers log
`[DebugServer] watch #N: 0x... changed ... VM paused`.

## 2. MCP end-to-end (the ammo-refill scenario)

1. Human: launch PCSX2, boot Extermination, stand at an ammo-refill station.
2. Agent: `pcsx2_connect` → expect "DebugServer: connected".
3. Agent: `pcsx2_pad_press` with `buttons: "cross"` (or `0x4000`), `frames: 12`.
4. Expected: the ammo-refill menu opens in-game, exactly as if X were tapped on a
   real pad. Repeat with `frames: 2` to verify short taps register, and with
   `buttons: "up"` / `"down"` to navigate the menu.
5. Analog check: `pcsx2_pad_set` with `ly: 0` (stick full up) — the character
   should walk forward until `pcsx2_pad_set` with `clear: true`.

Button mask reference (PS2 libpad order; the tools also accept names):

```
SELECT 0x0001  L3 0x0002  R3 0x0004  START  0x0008
UP     0x0010  RIGHT 0x0020  DOWN 0x0040  LEFT 0x0080
L2     0x0100  R2 0x0200  L1 0x0400  R1   0x0800
TRIANGLE 0x1000  CIRCLE 0x2000  CROSS/X 0x4000  SQUARE 0x8000
```

Stick axes are 0..255, 128 = center, `ly: 0` = up.

## 3. watch_change (auto-pause on memory change)

This substitutes for the arm64-recompiler breakpoints that don't fire.

1. Find a known-changing address (e.g. the ammo count found via `pcsx2_find_pattern`
   / `pcsx2_memory_diff`).
2. `pcsx2_watch_change` with `address`, `size: 4`, `interval_ms: 5`.
3. In-game, cause the value to change (fire a shot / pick up ammo).
4. Expected: the VM **pauses by itself** within ~interval_ms of the change.
   `pcsx2_watch_list` shows `CHANGED old → new @ cycle ...`; `pcsx2_status`
   shows `paused=true`.
5. `pcsx2_get_backtrace` / `pcsx2_read_registers` to inspect, `pcsx2_continue`
   to resume (the watch stays armed and re-triggers on the next change),
   `pcsx2_watch_clear` to remove.

Caveat: polling at 5ms catches the *fact* of a change, not the writing
instruction — PC at pause time is near, not at, the store. Bisect with
save states + smaller intervals, or use `pcsx2_memory_diff` to narrow first.

## 4. Semantics summary

- `pad_set` = base hold, applied every vsync, overrides physical input for all
  16 buttons + both sticks while active.
- `pad_press` = OR'd on top of the base hold for N frames; analogs (if given)
  override for the press duration; afterwards the base hold (or neutral) resumes.
- Clearing (or a press ending with no base hold) emits **one neutral release
  frame** before deactivating, so games see a clean button-up edge.
- Watches: max 64, sizes 1/2/4 bytes, interval 1..1000ms, EE or IOP via `cpu`.
