# Capture program and calibration

Before each scan the host sends the sensor a **capture program** (opcode
`0x02`): a list of records that configure the analogue front end, timing,
finger detection and image processing. The program depends on the individual
sensor, so the driver builds it from a generic skeleton and the sensor's
calibration. Code: `drivers/validity-0088/validity-capture-builder.c`.

## Format

```
02                      opcode
u16 line_len            152 (bytes per raw line)
u16 lines               0 for a normal capture, 337 for calibration
records…                u16 type, u16 len, payload[len]  (little-endian)
[4 zero bytes]          only if the total length would be a multiple of 64
                        (avoids a zero-length USB packet)
```

## Record types

Names from the Windows driver's record table (`synaWudfBioUsb66.dll`); the
right column marks the records of the capture program for this sensor.

| Type | Name | Used | | Type | Name | Used |
|---|---|---|---|---|---|---|
| `0x00` | No_Operation | | | `0x25` | SO_Alternate | |
| `0x01` | Swipe | | | `0x26` | Finger_Detect | ✓ |
| `0x02` | Timeslot_Configuration | | | `0x27` | Finger_Detect_Sample_Register | |
| `0x03` | Register | | | `0x28` | Finger_Detect_Scan_Registers | |
| `0x04` | Register_Set_32 | | | `0x29` | Timeslot_Table_Offset | ✓ |
| `0x05` | Register_Operation_32 | | | `0x2a` | ACM_Config | |
| `0x06` | Security | | | `0x2b` | ACM_Control | |
| `0x08` | Motion_1 | | | `0x2c` | CEM_Config | |
| `0x0a` | CPUCLK | | | `0x2d` | CEM_Control | |
| `0x0b` | Motion_2 | | | `0x2e` | Image_Reconstruction (holds the mode byte) | ✓ |
| `0x0c` | Calibration_Block | | | `0x2f` | (unnamed) | ✓ |
| `0x0d` | Sweep | | | `0x30` | Line_Update | ✓ |
| `0x0e` | Zone_Configuration | | | `0x31` | FDetect_Timeslot_Table | |
| `0x0f` | Zones_Per_Sweep | | | `0x32` | Register_List_16 | ✓ |
| `0x10` | Lines_Per_Sweep_Iteration | | | `0x33` | Register_list_32 | ✓ |
| `0x11` | Lines_Per_Sweep | | | `0x34` | Timeslot_Table_2D | ✓ |
| `0x12` | Total_Zones | | | `0x35` | Timeslot_Table_Offset_for_Finger | ✓ |
| `0x13` | CAL_WOE_Ctrl | | | `0x36` | Security_Aligned | |
| `0x14` | Cal_WOE_Mask | | | `0x38` | WOE_WOF | |
| `0x15` | BW_Reduction | | | `0x39` | Navigation | |
| `0x17` | Reply_Configuration | ✓ | | `0x3a` | WOE_WOF2_Version2 | |
| `0x18` | Motion_3 | | | `0x3b` | Cal_WOE_WOF2 | |
| `0x19` | WOVAR | | | `0x3c` | Event_Signal | |
| `0x1a` | Block_Mode | | | `0x3d` | IFS_Frame_Stats | |
| `0x1b` | Bit_Reduction | | | `0x3e` | SNR_Method_4 | |
| `0x1c` | Motion_4 | | | `0x3f` | WOE_WOF2_Version_3 | |
| `0x1d` | Calibration_WOENF | | | `0x40` | Calibrate_WOE_WOF2_Version_3 | |
| `0x1e` | Calibration | | | `0x41` | Finger_Detect_Ratchet | |
| `0x1f` | Zone_Configuration_A | | | `0x42` | Data_Encoder | ✓ |
| `0x20` | Set_Register_32 | ✓ | | `0x43` | Line_Update_Transform | ✓ |
| `0x21` | Register_Operation_32A | | | `0x44` | Line_Update_InterLeave | ✓ |
| `0x22` | Fingerprint_Buffering | | | `0x45` | SO_Table_Values_for_Macros | |
| `0x23` | Reply_Config_Timeslot_Table | ✓ | | `0x46` | Timeslot_Macro_Definitions | |
| `0x24` | Baseline | | | `0x47` | Enable_ASP_Feature | ✓ |
| | | | | `0x48` | Baseline_Frame | ✓ |
| | | | | `0x49` | Rx_Select | |

The records this sensor does not use (wake on finger, frame statistics, SNR,
swipe, navigation …) are not used by the Windows driver for it either: the
capture program the driver builds is byte-identical to the one Windows sends.

## The skeleton and what calibration fills in

The source contains a 2005-byte skeleton of the program. Its sensor-specific
parts are blank; with a calibration the program is 18869 bytes:

| Record | In the skeleton | Filled from the calibration |
|---|---|---|
| `0x34` Timeslot_Table_2D (536 B) | first 144 B zero | `clamp((px − 128) · 10 / 4)` from frame rows 18, 28, 38 (value 5 → 4) |
| `0x30` Line_Update | only the 6 non-frame entries | 36 entries `0xffffffff / 0x85000000 \| 4g`, each 56 rows × 4 pixels of the scaled frame |
| `0x43` Line_Update_Transform (152 B) | 56-byte column map; trim area zero except its bit-width byte | factory trim, bit-packed twice (`[(bits−1)\|8, min, 0, 0]` + LSB-first fields) |
| `0x48` Baseline_Frame | 4-byte prefix only | 8512-byte baseline frame after the prefix |
| `0x2e` Image_Reconstruction | — | byte at offset `0x6d5` of the program = capture mode (`0x02` / `0x23`) |

Everything else (register setup, timing, scan configuration) is the same on
every 06cb:0088. There is no fallback: without a calibration the driver does
not capture.

Checked: built from the Windows calibration data of a device, the program is
byte-identical to what the Windows driver sends; built from random
calibrations, the old full template and the skeleton give identical
programs.

## Calibration

Runs at the end of session preparation when `calibration.bin` is missing, or
once with `VALIDITY0088_RECALIBRATE=1`. **The finger must be off the sensor.**
Takes about a quarter of a second.

1. **Factory trim:** `6f 00 0e 00 00 00 00 00 00` returns the sensor's
   configuration block list; block `0x80000003` holds, after a 4-byte header,
   the 56-byte per-column trim.
2. **Calibration program 1:** the skeleton's records up to and including
   `0x17`, then `0x44`, `0x30` (without frame entries) and `0x43` with the
   trim; `0x34` timeslot head zeroed; header lines = 337. The sensor streams
   337 raw lines of 152 bytes on bulk endpoint `0x82` (3 passes × 112 lines +
   an end line; each line starts with an 8-byte header `01 fe NN pass …`).
   **Frame** = per-row average of the line pair of pass 2.
3. **Calibration program 2** = program 1 plus the frame-derived records; a
   second stream gives the **baseline** frame (same reduction).
4. Save `calibration.bin` and repeat the tail of the session preparation.

Each frame is checked for plausibility (`validity_cal_frame_plausible`);
observed frame means on a working sensor are 125–128.
Two calibrations of the same sensor differ only by noise (about 1 grey level
on average); the trim is identical.

Unverified detail: one instruction byte in record `0x34` of calibration
program 1 may depend on a device flag the Windows driver reads from offset
`+0x4c` of a device structure, which is not readable on Linux. The current
value works.

### `calibration.bin`

```
u32 magic        "VCAL" (0x4c414356)
u32 version      1
u32 have_baseline
u8  trim[56]
u8  frame[8512]      56 rows × 152 bytes
u8  baseline[8512]
```

17092 bytes, little-endian, mode 600. Debug dumps of the last calibration are
written next to it: `cal-config.bin`, `cal-prog-1.bin`, `cal-prog-2.bin`,
`cal-raw-1.bin`, `cal-raw-2.bin`.
