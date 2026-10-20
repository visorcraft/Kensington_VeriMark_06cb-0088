# USB protocol

What the driver sends to and receives from the VeriMark (USB `06cb:0088`,
Synaptics/Validity "Windsor" sensor, sensor id `0x2449`, ROM build 154,
version 6.20, product `0x33`). Everything here was established by the
driver's implementation and by tests on a real device; where the meaning of a
value is not known, this is said.

## Endpoints

The device has one vendor-specific interface (class `0xff`).

| Endpoint | Direction | Use |
|---|---|---|
| `0x01` | bulk OUT | Commands (plaintext before TLS, TLS records afterwards) |
| `0x81` | bulk IN | Replies |
| `0x82` | bulk IN | Raw line stream during calibration |
| `0x83` | interrupt IN | Finger events while a capture program waits for a tap |
| `0x84` | interrupt IN | Not used (no use found in the Windows driver either) |

## Framing

**Plaintext phase** (right after open): a command is one opcode byte
followed by its payload; the reply starts with a 2-byte status word.

**TLS phase**: a Synaptics-modified TLS 1.2 session. Commands and replies are
TLS application-data records (content type `0x17`, version `0x0303`) on the
same endpoints. The client's handshake messages (ClientHello, and the second
flight with all its records concatenated) are preceded by a 4-byte prefix
`44 00 00 00`; application data has no prefix. Cipher suite `0xc005`
(`TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA`): ECDH on P-256, AES-256-CBC with an
explicit IV, HMAC-SHA-256 (32 bytes).

The ClientHello differs from RFC 5246 in four places (a standard one gets a
fatal `illegal_parameter` alert): session id of 7 zero bytes,
compression-methods length 0 with no methods, the declared extensions length
is 2 less than the real one, and extension `0x0004` (normally
`truncated_hmac`) with value `0x0017` advertises P-256.

**Status words** are little-endian: the first two reply bytes, low byte
first (confirmed in the Windows driver, which reads the first 16-bit word of
the reply on x86).

| Status | Meaning |
|---|---|
| `0x0000` | OK |
| `0x0007` | `0x51`: no image (tap too short or too light) |
| `0x0401` | Not supported / rejected: on-chip match opcodes `0x5e`/`0x69`; `0x4f` when the device already holds a certificate ("already paired") |
| `0x04af` | `get_fw_info(fwext)`: firmware extension not loaded — returned on every open of a working unit, harmless; `0x3f`: partition cannot be erased |

## Opening the device

1. **Plaintext init**: `0x06` + 660-byte INIT_MSG4 payload, `0x5b`
   (meaning unknown), `0x01` (ROM info: build, version, product), `0x19`,
   and `0x43 0x02` (firmware info of the fwext partition; diagnostic only).
2. **Pairing**, only when the storage directory has no pairing data: a
   pre-TLS ceremony (clean-slate INIT_MSG4 variant, a certificate request
   signed with a key derived from a fixed Synaptics password, `0x4f` returns
   the device-signed certificate, `0x50` the device's ECDH key). The results
   are stored and reused on later opens. A device that is already paired with
   another host can be paired again; several hosts can pair the same device.
3. **TLS handshake** with the stored certificate and keys.

If the handshake fails, open fails; there is no automatic re-pair.
`VALIDITY0088_FORCE_REPAIR=1` deletes the pairing data so the next open pairs
from scratch.

## Session preparation

At the start of every enroll, verify and identify:

```
1a           enroll session
06 <660 B>   INIT_MSG4
5b
01           ROM info
19
75           (meaning unknown)
39 <…>       LED / scan setup
1a, 06 <660 B>, 5b
06 <10500 B> host part
```

Then, if `calibration.bin` is missing or `VALIDITY0088_RECALIBRATE=1`, the
calibration sequence runs (see [capture-program.md](capture-program.md)) and
the tail of the preparation is repeated.

## Capturing a scan

1. **`0x02` + capture program** (18869 bytes for this sensor, built from the
   generic skeleton and the calibration; see
   [capture-program.md](capture-program.md)). The program contains a mode
   byte: `0x02` ("identify", used for verify, identify and the first enroll
   tap) or `0x23` ("enroll", later enroll taps).
2. **Wait for a finger** on interrupt endpoint `0x83`.
3. **`0x51`** (read image): reply of 8082 bytes = 2-byte status + 16-byte
   header + 8064 pixels. Header fields: type 0, payload length 8076, flags 0,
   width 56, height 144, a constant `0x014d` (meaning unknown), 8 bits per
   pixel. With status `0x0007` the reply is only 18 bytes: no image, the
   driver asks for the tap again.

The pixel stream is **column-major**: 56 columns of 144 samples. The driver
transposes it into a 56 × 144 row-major image (about 7 × 3 mm of finger).

The capture response before the image (`0x02` reply, 2154 bytes) contains
fields named score / minutiae / threshold in older notes; the sensor does
**not** match fingerprints (opcodes `0x5e`/`0x69` return `0x0401`), so the
driver ignores them and matches on the host.

## LED

`0x39` with a 125-byte payload selects LED patterns: the scan-setup glow
while waiting for a tap, a success pulse and a red failure flash after a
verify or identify (`VALIDITY0088_LED_RESULT=0` disables the result flash).

## Flash

`0x3f` (erase), `0x41` (write) and `0x42` (sign) address the device's flash
partitions (partition 1 = certificate store). They are only used by the
pairing ceremony and are **dry-run by default**: frames are built and logged
but not sent unless `VALIDITY0088_FLASH_LIVE=1`.

## Other observations

- The USB device supports runtime suspend; on Linux the kernel suspends it
  automatically while no program has it open (`power/control=auto`).
- The Windows package also contains a software U2F (FIDO) authenticator
  (`synaWudfU2fhid.dll`, a *virtual* HID device). It is not part of the USB
  device and is not implemented here.
