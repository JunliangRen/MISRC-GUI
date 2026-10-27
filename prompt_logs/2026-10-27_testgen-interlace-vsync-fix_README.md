# 2026-10-27 — Test Gen simulated CVBS: V-sync rolling fix (interlaced half-line model)

Status: PAL SYNC FIX USER-VERIFIED (2026-10-27, GUI: Simulated device + Gen,
waveform panel, trigger mode - rolling gone). NTSC/SECAM stability and colour
correctness still to be confirmed on screen.

## Symptom

The simulated device's hacktv-ported CVBS bars signal (Gen mode) rolls
vertically in the Video panel. Horizontal is stable. Amplitude/contrast were
already matched to the real EBU HackDAC reference in the previous session
(+0.7 / -0.3 envelope, 2047x scale in the 12-bit domain).

## Root cause (from the decoder's gates, not theory)

`gui_cvbs.c` `cvbs_vbi_first_eq()` derives FIELD PARITY from the offset of
the first EQ pulse of each VBI train relative to the last H sync:

- half-line offset   -> PAL parity 0 (boundary = first_eq - 0.5 lines)
- integer-line offset -> PAL parity 1 (boundary = first_eq - 1.0 lines)

and requires parity to ALTERNATE between consecutive candidates, plus a
312.5-line cadence (+/-2.0).

The old `gui_testgen.c` classified whole 64us lines (trains at integer line
positions). Measured pulse grid of the old generator:

- field 1 first EQ at frame slot 0, last H of the previous frame at -1.0
  lines -> integer offset -> parity 1
- field 2 first EQ at line 314 (t=313 lines), last H at line 313 (t=312)
  -> integer offset -> parity 1

BOTH fields derived as parity 1 -> every second candidate rejected with
"vertical fields repeat parity" (`reject_parity`), no field ever anchored,
the weave never stabilised -> vertical rolling.

## Fix

Rewrote `misrc_tools/misrc_gui/signal/gui_testgen.c` around a HALF-LINE
(32us) SLOT GRID - the structure a real interlaced signal has:

PAL (1250 slots/frame):
- field 1 train: EQ x5 slots 0-4, broad x5 slots 5-9, EQ x5 slots 10-14
- H grid: ODD slots 15..623 (field 1), 641..1249 (field 2)
- field 2 train: EQ x5 slots 625-629, broad x5 slots 630-634, EQ x5 635-639
- blank slots: 624, 640
NTSC (1050 slots): same shape, 6 pulses per train, H grid on EVEN slots.

The two H grids sit 625 slots (312.5 lines) apart - that IS the interlace.
The slot start samples are spread over the frame (need/1250 +-1), so the
line period is exact at any rate and the old short-last-line stream jump
every frame is gone.

### Decoder-gate arithmetic (verified against the generated buffers)

PAL:
- recent-H gate: f1 first EQ at slot 0 vs last H (prev frame slot 1249) =
  0.5 lines (< 2.1 OK); f2 first EQ at slot 625 vs last H1 (slot 623) =
  1.0 line (OK)
- parity gate: f1 half -> parity 0; f2 integer -> parity 1 (alternating OK)
- cadence gate: field starts -0.5 / 312.0 lines -> 312.5 apart (OK)
NTSC mirrors it (parity 0 = integer offset, boundary 1.0; cadence 262/263).

PAL V-switch and SECAM D'R'/D'B' parity now key off a per-field H-line
index (reset at each field boundary, even index -> +V/+135deg/D'R'), which
reproduces the previously-verified colour phase mapping: field 1's first
ACTIVE line carries +V (old line_1 23 odd -> +1), field 2's first ACTIVE
line -V (old line_1 336 even -> -1), alternating line by line, with the
same-phase H pair across each frame boundary (the real PAL 4-field wrap).

## Verification done

- `meson compile -C build-local`: all 58 targets link clean.
- `meson test -C build-local`: 44/45. The one failure (`update_cli`) is
  PRE-EXISTING and unrelated (updater version-compare semantics from the
  previous session: compare returns 0 for a dev current version, while
  update_cli_test.py:172 expects v9.9.9 to compare newer than dev-...).
  Left untouched deliberately - it contradicts the user-requested "dev
  versions must not show as outdated" change; needs a user decision.
- Structural pulse-grid check (throwaway harness in %TEMP%\testgen_check,
  generated buffers scanned at 40 MSPS): PAL EQ/broad/H slot positions and
  widths as designed (EQ 2.35us @ slots 0-4/10-14/625-629/635-639 at the
  -0.25 FS threshold reading 2.25; broad 27.3; H 4.7 @ odd slots; blank
  slots 624/640 empty; last H at slot 1249). NTSC even H grid + EQ x6 at
  slots 525-530 confirmed. Burst dips to -0.30 FS (3/7 x 0.7) are the
  accepted burst amplitude, not a defect.

## Awaiting user verification

1. GUI: Simulated device + Gen (PAL) - vertical rolling gone, bars stable.
2. Gen NTSC and SECAM cycle - same stability.
3. Colour still correct (burst/V-switch parity preserved by construction,
   but confirm on screen).

## Files touched

- misrc_tools/misrc_gui/signal/gui_testgen.c (rewritten generator)

## Restore point

Pre-change state: git HEAD (main) before this edit - gui_testgen.c was the
only file modified in this session.
