# Debug notes: real enrollment protocol + a foundational status-parsing bug

This branch is **not ready to merge** — it's a debug/reference branch documenting a full
evening's reverse-engineering session against a real `06cb:0088` device (Kensington VeriMark,
already paired and actively used with Windows Hello). Posting it in case it saves the next person
(maintainer or otherwise) from re-deriving the same findings, and because we're stuck on one
specific, well-isolated question we couldn't answer from source alone.

Everything below was found/built by testing against real hardware, cross-referencing
`uunicorn/python-validity` (the working implementation for the sibling `06cb:009a` sensor, which
this driver's own comments already cite as a source), and a real Windows Hello USB capture.

## Two bugs fixed here that are correct and complete on their own

These don't depend on solving anything else below — worth landing regardless.

**1. Status-word byte order was backwards.** `validity_validate_status()` in `validity.c` read the
2-byte status word big-endian (`rsp[0]<<8 | rsp[1]`). The correct order is little-endian —
confirmed against this driver's own `flash_exchange_status()` (`validity-pairing.c`), which
already used little-endian correctly, AND against python-validity's `assert_status()`
(`unpack('<H', b[:2])`). This means **every "unexpected status" this driver has ever logged was
byte-swapped**: e.g. `0x0104` was really `0x0401`, `0x0600` was really `0x0006`, `0xaf04` was
really `0x04af` (which matches `VALIDITY_STATUS_FLASH_ERASE_NOT_ALLOWED`, already defined
elsewhere in the codebase — so that constant was right, just never actually being matched against
correctly-decoded values). Fixed in both `validity_validate_status()` and the equivalent check in
`validity_send_cert_acquisition()`'s `0x4f` handler.

**2. Six wrong bytes in the LED scan-setup payload.** `led_blue_scan_setup_template` in
`validity-commands.c` had `0xbf` at 6 offsets where python-validity's `glow_start_scan()` (which
this driver's own comment claims equivalence to) has `0x99`, plus one `0x00`/`0x00` pair
transposed. Diffed byte-for-byte to confirm; fixed to match the reference exactly.

## The major finding: this driver's enrollment model was built on the wrong opcode

`ENROLL_S_READ_IMAGE` (original code) sent `0x51`/`mode=0x2000` expecting a raw 8082-byte
fingerprint image back directly. **That's not what this opcode does.** In python-validity's
`Sensor.capture()`, the equivalent call (`get_prg_status2()`, byte-for-byte the same wire payload:
`51 00 20 00 00`) only ever returns a small ~16-byte status tuple (`x, y, w1, w2, error`)
confirming the scan succeeded — never image data. The actual per-tap fingerprint template comes
from **three opcodes this driver never implemented at all: `0x69`** (create/end enrollment),
**`0x68`** (enrollment_update_start), and **`0x6b`** (enrollment_update, called twice per tap,
TLV-framed response carrying the accumulating template + a `tid` field that signals when the
device itself considers enrollment complete — not a fixed tap count).

This explains the phantom-finger-detection hang in #2 (see below) and a lot else besides: without
`0x68`/`0x69`/`0x6b`, there's no way for a capture to ever actually contribute to a real
enrollment, so every retry-the-capture-program workaround was chasing a symptom, not the cause.

Also worth noting: the original interrupt-matching condition this driver already had
(`event[0]==0x03 && (event[2]&0x04)`) is **correct** — it matches python-validity's own
`capture()` wait loop exactly (waits on that exact bit, unboundedly, no fallback). We tried
loosening it during debugging and that was a mistake — reverted.

## What's implemented in this branch

- New opcodes `0x68`/`0x69`/`0x6b`/`0x04` (`0x04` = capture-stop, always sent after a capture
  attempt whether it succeeded or not, matching python's `finally: tls.app(unhexlify('04'))`).
- A TLV response parser (`validity_parse_enroll_update_response`) for `0x6b`'s response —
  ported faithfully from `append_new_image()`'s parsing logic, including its slightly odd
  fixed-0x38-byte-prefix-per-record framing (kept as-is rather than "cleaned up", since it's
  reverse-engineered from a real DLL and the exact byte layout matters).
- `ENROLL_NUM_STATES` fully restructured to the real per-tap flow: scan-setup → capture-program →
  interrupt-wait (real bit-4 check, not loosened) → confirm-capture (`0x51`/`0x2000`, now
  correctly treated as a small status tuple, not an image) → capture-stop (`0x04`) →
  enrollment-update-start (`0x68`) → two enrollment-update calls (`0x6b`) → toggle-end (`0x69`/0)
  → loop until the device's own `tid` field says enrollment is complete.
- Print storage moved from local NBIS/image-based matching to `FPI_PRINT_RAW` +
  `fpi_print_set_device_stored` (same pattern `egismoc` uses for match-on-chip devices) — the
  device's own template blob becomes the print's `fpi-data`; no local minutiae extraction. This
  device family appears to do its own on-device template building and (per python-validity's
  `match_finger()`, opcode `0x5e`) its own on-device matching too — verify/identify would need the
  same treatment, not yet attempted here.
- A read-only calibration precondition check (`0x40`, partition 6 — confirmed correct for `0088`
  specifically via this driver's own `validity-pairing.c` comment listing partition IDs; NOT
  partition 1, the cert store). **No write path implemented anywhere in this branch, deliberately**
  — if you extend this, please keep it that way until someone's confident about what triggers
  `0x04af` (see below), since a wrong flash write on a device actively paired with Windows Hello
  is exactly the kind of mistake this project can't afford.

## Where we're actually stuck

`enroll-toggle-start` (`0x69`/enable=1 — the very first of the three new opcodes, i.e. the
earliest point at which the missing protocol is actually exercised) gets a real response, but
with status **`0x0401`**, not `0x0000`. This is a clean, real, reproducible rejection — not
something we're misreading (checked against the byte-order fix above), not a stuck-device-state
artifact (`usbreset`'d the device, identical result), and not something python-validity's
`assert_status()` tolerates either (it only special-cases `0x044f` and, in a different function,
`0x0491`). We don't know what `0x0401` means. It's not documented anywhere we found — not in this
repo, not in python-validity, not in any partition/flash access-level docs we could locate.

We also tried, in case it was a precondition issue rather than something specific to `0x69`
itself:
- A `0x40` partition-6 read to check for existing calibration data — rejected with `0x04af`
  ("partition cannot be erased in current state" per this repo's own existing constant, though
  this was a *read*, not an erase, so that gloss may not directly apply). Made this check
  advisory-only to decouple it from the `0x69` question, and **`0x69` still fails independently**
  with its own `0x0401` regardless of what the calibration read returns — so these appear to be
  two separate things, not one root cause.
- A `0x3e` (`get_flash_info`) priming call before the calibration read, per python-validity's
  `init_flash.py` pattern — no change to either status.

If you've seen `0x0401` on this opcode, or have a real USB capture of a working `06cb:0088`
enrollment (ours only has Windows Hello **verify/unlock** traffic, not a fresh enrollment — see
below) — that's exactly the piece we're missing.

## Windows Hello USB capture

Captured `USBPcapCMD.exe` traffic during real Windows Hello unlock cycles on this same physical
device (not included in this branch to keep it small/no unrelated binary noise for reviewers —
happy to share on request, or see the issue for a link). Confirmed useful findings from it:

- The device stays at the same USB address for an entire multi-unlock session — no bus-level
  reset/reconnect between captures, ruling out a "needs a full USB close+reopen between attempts"
  theory we chased for a while.
- Windows sends a bare scan-setup-sized application-data record (no separate cleanup command)
  immediately before each capture-program send, even for a second capture in the same session —
  confirms the *shape* of what we built matches, even though we're still blocked before ever
  getting that far in practice (we fail at `0x69`, before any capture happens).
- This was verify/unlock traffic, not enrollment, so it doesn't show the `0x68`/`0x69`/`0x6b`
  sequence directly — a capture of an actual (re-)enrollment would likely be far more directly
  useful than what we have.

## Build/reproduce

Same as this repo's own README section 2, plus:

```bash
git clone https://gitlab.freedesktop.org/libfprint/libfprint.git
cp -r drivers/validity-0088 libfprint/libfprint/drivers/
# apply this branch's changes to libfprint/libfprint/drivers/validity-0088/{validity.c,validity.h,validity-commands.c}
```

The README's own meson.build snippet is stale for current libfprint — the `driver_sources` dict
now wants `files(...)` wrapping, and the outer `meson.build`'s `driver_helper_mapping` dict entry
shape is `'validity_0088': { 'helper': ['openssl'] }`, not the list-only form the README shows.

Build deps beyond the README's list (also missing there): `libgirepository1.0-dev` on
Debian/Ubuntu (or your distro's equivalent) — needed for `gobject-introspection-1.0`.

Debug logging (to see raw interrupt-event bytes / status words — default fprintd logging is
info/warning only): set `G_MESSAGES_DEBUG=all` and `LIBFPRINT_DEBUG=5` in whatever environment
wraps your `fprintd` process.

`fprintd-enroll`'s own default D-Bus timeout (~25s) can race a slow polkit auth dialog on some
desktop environments — if you hit that, drive the same D-Bus calls manually with a longer
timeout instead of the stock CLI tool.
