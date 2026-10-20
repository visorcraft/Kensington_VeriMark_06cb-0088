# Architecture

The driver is an ordinary libfprint driver (`FpDevice` subclass) for one
device, USB `06cb:0088`. It is built into `libfprint-2.so`; fprintd or any
other libfprint client uses it through the libfprint API.

## Source files (`drivers/validity-0088/`)

| File | Role |
|---|---|
| `validity.c` | Device class: probe/open/close, session preparation and calibration states, enroll and verify/identify state machines, LED feedback |
| `validity-transport.c` | USB bulk read/write, encrypted application-data exchange, reply status checks, async exchange and interrupt-wait helpers |
| `validity-handshake.c` | The Synaptics-modified TLS 1.2 handshake |
| `validity-crypto.c` | TLS PRF, record encryption (AES-256-CBC + HMAC-SHA-256), ECDH |
| `validity-pairing.c`, `validity-pretls-pair.c` | Pairing with the device, pairing key storage, first-pair ceremony |
| `validity-commands.c` | Builders for wire commands (ClientHello, capture, read image, LED, flash) |
| `validity-capture-builder.c` | Generic capture-program skeleton, sensor calibration, building the per-sensor capture program |
| `validity-sift.c/.h` | Self-contained SIFT keypoint extractor and matcher |
| `validity-match.c/.h` | Glue between scans and the matcher: quality gate, image extraction, print (de)serialisation, scoring, scan dumps |
| `validity-printkey.c/.h` | AES-256-GCM encryption of stored prints and the key lookup |
| `validity-init-data.c`, `validity-init-clean-slate.c`, `validity-enroll-data.c` | Static protocol payloads (INIT_MSG4 variants, host part) |
| `validity-pubkeys.c` | Zeroed placeholders for a baked-in pairing identity |
| `validity.h` | Shared definitions, protocol constants, byte-order helpers |

## Life of an operation

```
open()                       plaintext init → (pairing if needed) → TLS handshake
  │
enroll / verify / identify   one state machine (SSM) per operation
  │
  ├─ session preparation     init opcodes, host part, LED/scan setup,
  │                          calibration if calibration.bin is missing
  ├─ capture                 0x02 + capture program, wait for finger event (EP 0x83)
  ├─ read image              0x51 → 56 × 144 image (or "no image" → retry)
  ├─ quality gate            high-pass RMS ≥ 15, else retry
  ├─ (enroll) commit tap     keypoints of the tap kept; 10 taps → print
  ├─ (verify) match          best score of the scan over the print's taps ≥ 12
  ├─ (identify) match        best print of the gallery ≥ 12
  └─ LED feedback            success pulse / red flash (verify, identify)
close()
```

Verify and identify share one state machine; only the match step and the
final report differ (`fpi_device_get_current_action`).

### Enroll

10 stages (`VALIDITY_MAX_ENROLL_STAGES`). The first capture uses mode
`0x02`, all later ones `0x23`, as the Windows driver does. A tap without an
image or without enough ridge contrast is reported as a retry and does not
count. When libfprint passes an existing print as template (print update),
its taps are kept and the new ones added, up to 30 taps.

### Verify and identify

The scan's keypoints are extracted once, then matched against every tap of
the print (verify) or of every gallery print (identify). Prints that cannot
be read are skipped in identify and reported as an error in verify.

## Stored data

Directory (first that applies): `$VALIDITY0088_STORAGE_DIR`;
`$STATE_DIRECTORY/validity-0088-pairing` (fprintd:
`/var/lib/fprint/validity-0088-pairing/`);
`~/.local/share/libfprint/verimark-06cb-0088/` (CLI tools).

| File | Contents |
|---|---|
| `cert-blob`, `host-key.pem`, `device-ecdh-pubkey.bin`, host part, OSB | Pairing with this device |
| `calibration.bin` | Sensor calibration (`VCAL` v1), see [capture-program.md](capture-program.md) |
| `cal-*.bin` | Debug dumps of the last calibration |
| `print-key` | Print encryption key when no systemd credential is used, see [print-encryption.md](print-encryption.md) |

All files are mode 600 in a mode-700 directory. The prints themselves are
stored by the libfprint client (fprintd: `/var/lib/fprint/<user>/validity-0088/…`);
the driver only produces their `fpi-data` content.
