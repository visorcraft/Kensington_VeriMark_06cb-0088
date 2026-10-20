# Configuration

The driver needs no configuration. These environment variables change its
behaviour; for fprintd, set them in a systemd drop-in
(`sudo systemctl edit fprintd.service`, `Environment=NAME=value`), for the
libfprint examples on the command line.

## Everyday

| Variable | Effect |
|---|---|
| `VALIDITY0088_RECALIBRATE=1` | Calibrate the sensor again in the next enroll/verify (keep the finger off the sensor). |
| `VALIDITY0088_SIFT_THRESHOLD=<n>` | Match threshold in consistent keypoints (default 12, allowed 4–10000). Higher: fewer false accepts, more false rejects. See [evaluation.md](evaluation.md). |
| `VALIDITY0088_PRINT_ENCRYPTION=0` | Store new prints unencrypted. Encrypted prints stay readable while their key exists. For fprintd: `setup-fprintd.sh --encryption-off`. |
| `VALIDITY0088_LED_RESULT=0` | No LED feedback (success pulse / red flash) after a verify or identify. |
| `VALIDITY0088_STORAGE_DIR=<path>` | Use this storage directory instead of the default (see [architecture.md](architecture.md#stored-data)). |

## Testing and evaluation

| Variable | Effect |
|---|---|
| `VALIDITY0088_DUMP_SCANS=<dir>` | Save every accepted scan (enroll, verify, identify) as a 56 × 144 PGM image `<time>-<action>-f<finger>.pgm`, for [evaluation.md](evaluation.md). These are images of your fingerprints: keep them private and delete them when done. |
| `VALIDITY0088_VERIFY_MODE=enroll` | Capture verify scans with capture mode `0x23` (as enroll taps) instead of `0x02`. |

## Pairing and recovery

| Variable | Effect |
|---|---|
| `VALIDITY0088_FORCE_REPAIR=1` | On the next open, delete the stored pairing data so the device is paired again from scratch. |
| `VALIDITY0088_SKIP_AUTO_PAIR=1` | Do not pair automatically; open fails when no pairing data is stored (you can put `cert-blob`, `host-key.pem` and `device-ecdh-pubkey.bin` into the storage directory by hand). |
| `VALIDITY0088_TRY_HARD_RESET=1` | On the next open, send the device's unpair sequence instead of opening it; then unplug, replug and unset the variable. |
| `VALIDITY0088_FLASH_LIVE=1` | Let the pairing ceremony really send the flash erase/write/sign frames (`0x3f`/`0x41`/`0x42`). **Default is dry-run**: frames are built and logged, not sent. Only after reviewing the dry-run output — this touches the device's certificate store. |
| `VALIDITY0088_FLASH_DRY_RUN=1` | Force dry-run even with `VALIDITY0088_FLASH_LIVE=1`. |
| `VALIDITY0088_FLASH_SIGNATURE_HEX=<hex>` | Signature to attach when the certificate-store write needs one (not needed by default). |

## Debugging overrides

| Variable | Effect |
|---|---|
| `VALIDITY0088_ENROLL_HOSTPART_CMD=<path>` | Send the host-part payload from this file (10501 B, starting with opcode `0x06`) instead of the built-in one. |
| `VALIDITY0088_ENROLL_CAPTURE_CMD=<path>` | Send the capture program from this file (18869 B, starting with `0x02`) as is — no capture mode byte, no calibration. |

Variables of older versions without effect now: `VALIDITY0088_BZ3_THRESHOLD`,
`VALIDITY0088_NCC_THRESHOLD`, `VALIDITY0088_IMAGE_SCALE`,
`VALIDITY0088_IMAGE_PARTIAL`, `_IMAGE_INVERT`, `_IMAGE_H_FLIP`,
`_IMAGE_V_FLIP`.

For log output of the libfprint examples, run them with `G_MESSAGES_DEBUG=all`.
