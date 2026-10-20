# Evaluating match quality

How well the matcher separates the right finger from wrong ones can be
measured offline: save the scans of a test session, then score them with
`tools/scan-eval.c` for any threshold or enrollment size, without new taps.

> The saved scans are **images of fingerprints** (biometric data). Keep them
> in a private directory, never commit them (`*.pgm` is git-ignored), and
> delete them when done.

## 1. Record a session

With `VALIDITY0088_DUMP_SCANS=<dir>`, the driver saves every scan that passes
the quality gate as a 56 × 144 PGM image (mode 600):

```
<dir>/<YYYYmmdd-HHMMSS.mmm>-<action>-f<finger>.pgm
action = enroll | verify | identify;  finger = libfprint FpFinger (1 = left thumb … 10 = right little)
```

A complete session with the libfprint examples (B = `build/libfprint/builddir`),
run from the dump directory so its `test-storage.variant` stays there:

```bash
D=~/.local/share/libfprint/verimark-scans/$(date +%F); mkdir -p -m 700 "$D"; cd "$D"
# enroll each finger: menu numbers 0-9 (0 = left thumb … 9 = right little)
VALIDITY0088_DUMP_SCANS=$D LD_LIBRARY_PATH=$B/libfprint $B/examples/enroll
# probes: several verify taps per finger, ideally later or on another day
VALIDITY0088_DUMP_SCANS=$D LD_LIBRARY_PATH=$B/libfprint $B/examples/verify
```

Use the finger you selected: a wrong finger during a verify run is recorded
under the claimed finger. `scan-eval` reports probes that match another
finger clearly better than their own.

## 2. Score it

Build (from the repository root):

```bash
cc -O2 -o scan-eval tools/scan-eval.c drivers/validity-0088/validity-sift.c \
   -Idrivers/validity-0088 $(pkg-config --cflags --libs glib-2.0) -lm
```

**Thresholds:** `./scan-eval <dir> [threshold…]`. One print per finger is
built from its enroll scans (in time order). Every verify scan is scored
against its own finger (genuine) and the other fingers (impostor); every
enroll scan is scored against the other fingers' prints and, leaving itself
out, against its own (genuine, same session). Output: score distributions
and false rejects / false accepts per threshold.

**Enrollment size:** `./scan-eval <dir> --stages 5,10,15 [threshold] [runs]`.
For each size N, N scans of every finger (enroll and verify alike) are drawn
at random as the print and the rest are probes, repeated `runs` times
(default 200). Shows how false rejects and false accepts change with the
number of taps.

## Results (2026-10-20, one person, all 10 fingers)

10 enroll taps + 10 verify taps per finger, verify right after enrolling.

| Comparison | n | min | p5 | median | p95 | max |
|---|---|---|---|---|---|---|
| impostor (verify probes + enroll scans vs other fingers) | 1800 | 0 | 2 | 2 | 4 | **7** |
| genuine, verify probes | 100 | 2 | 20 | 46 | 96 | 112 |
| genuine, enroll leave-one-out | 100 | 3 | 61 | 118 | 166 | 176 |

| Threshold | False rejects | False accepts |
|---|---|---|
| 6 | 2 % | 7 / 1800 |
| 8 – 15 | 3 % | 0 / 1800 |
| 20 | 5 % | 0 |
| 30 | 12 % | 0 |

- No false accept at the default threshold 12; with 0 in 1800 comparisons the
  false-accept rate is below about 0.17 % (95 % confidence) for this person.
- The three false rejects scored 2, 5 and 6 — noise level, not borderline:
  those taps landed on finger areas none of the enroll taps covered.

Enrollment size (`--stages`, 300 random draws per size, threshold 12):

| Taps per print | 5 | 8 | 10 | 12 | 15 | 18 |
|---|---|---|---|---|---|---|
| False rejects | 1.17 % | 0.61 % | 0.51 % | 0.52 % | 0.47 % | 0.58 % |
| Max impostor score | 7 | 7 | 7 | 7 | 7 | 7 |

No gain above 10 taps. Ten taps drawn from two sittings (0.5 %) did much
better than the ten consecutive enroll taps (3 %): placement variety, not
the number of taps, reduces false rejects — hence print updates (see
[matcher.md](matcher.md)).

Earlier findings on enrollment: taps of one enrollment placed on the same
spot score 130–180 against each other, varied taps mostly below 125 (often
0–30). A gate rejecting taps ≥ 130 against an earlier tap was tried and
dropped: real enrollments never triggered it.

Limits: one person, probes right after enrollment. More people and a later
probe session would tighten the numbers.
