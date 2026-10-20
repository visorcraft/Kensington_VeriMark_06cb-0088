# Matcher and print formats

The sensor does not match fingerprints itself, so the driver does it on the
host. Code: `validity-sift.c` (keypoints and matching, self-contained, no
OpenCV) and `validity-match.c` (glue: quality gate, print data, scoring).

## Why not minutiae

A scan is 56 × 144 pixels, about 7 × 3 mm of finger, and holds only 3–9
minutiae (ridge endings and splits). libfprint's NBIS minutiae matcher cannot
work with that. An image-correlation matcher used before failed beyond about
10° of rotation and let other fingers come close to the threshold. SIFT
keypoints describe local ridge texture and work on small, rotated images.

## Keypoints

| Step | Setting |
|---|---|
| Pre-processing | 2× bilinear upscale, local mean/std normalisation (radius 8) |
| Scale space | 3 octaves × 3 intervals, σ = 1.6, DoG extrema with sub-pixel refinement |
| Rejection | contrast < 0.04, edge ratio > 20, 6-pixel border |
| Orientation | 36-bin gradient histogram, extra keypoints for peaks ≥ 80 % |
| Descriptor | 4 × 4 × 8 = 128 values, normalised, clipped at 0.2 |
| Kept | the 250 strongest keypoints per scan |

## Score

1. Nearest-neighbour descriptor matching with Lowe's ratio test (0.8).
2. RANSAC (600 iterations) over a similarity transform: rotation up to ±25°,
   scale within ±15 % (log scale), tolerance 2 px of the original image.
3. **Score = number of geometrically consistent matches.**

A scan is compared with every tap of a print; the print's score is the best
one. **Match threshold: 12** (`VALIDITY0088_SIFT_THRESHOLD`, 4–10000).

Field test with all 10 fingers of one person (see
[evaluation.md](evaluation.md)): wrong fingers never scored above 7 in 1800
comparisons; the enrolled finger scored 46 in the median. Any threshold
between 8 and 15 gives the same result; 12 sits in the middle of that gap.

## Quality gate

Before matching or storing, a scan must have enough ridge contrast: the RMS
of the image after subtracting a 13 × 13 box mean must be at least 15.
Otherwise the tap is asked for again ("Not enough ridge detail"). A reply
without an image (`0x51` status `0x0007`) is also a retry.

## Print formats (`fpi-data`)

The driver stores no images, only keypoints. One serialised keypoint set
(per tap):

```
u32 magic "VSF1", u32 n
n × (u16 x·16, u16 y·16)          positions, fixed point
n × 128 B                          descriptor values × 512, capped at 255
```

≈ 33 KB per tap with 250 keypoints.

| Format | `fpi-data` | Written | Read |
|---|---|---|---|
| 2 (default) | `(u ay ay ay)` = (2, key id, nonce, AES-256-GCM ciphertext of the format-1 `aay`) | yes | yes |
| 1 | `(u aay)` = (1, one keypoint set per tap) | with `VALIDITY0088_PRINT_ENCRYPTION=0` | yes |
| legacy | `aay` of raw 56 × 144 scans (correlation matcher era) | no | yes, keypoints computed on the fly |

Encryption: [print-encryption.md](print-encryption.md).

## Verify

The scan's keypoints are extracted once and scored against the print. Result
MATCH if the score ≥ threshold; the LED shows the result.

## Identify

The same state machine as verify. The scan is scored against every gallery
print; the best print at or above the threshold is reported. Prints that
cannot be read (another driver's, damaged, key missing) are skipped, so one
bad print does not block the others. fprintd uses identify for its duplicate
check when enrolling.

## Enroll and print updates

10 accepted taps form a print. Coverage matters more than the number of
taps: a scan of a finger area that no enrolled tap covers scores like a
different finger (0–5).

The driver supports libfprint's **print update**
(`FP_DEVICE_FEATURE_UPDATE_PRINT`): when an enrollment receives an existing
print as template, its taps are kept and the 10 new ones added, up to 30 taps
(`VALIDITY_PRINT_MAX_TAPS`, oldest dropped). A second enrollment on another
occasion covers more of the finger; in a test, three verify scans that the
original 10 taps scored 2, 2 and 4 matched the added taps with 55, 78 and 17.

Clients: libfprint's `examples/enroll` asks whether to update. **fprintd
(up to 1.94.5) does not use print updates** — enrolling a finger again
replaces its print.

Findings on enrollment (details in [evaluation.md](evaluation.md)): more than
10 taps per enrollment bring no gain; placement prompts cannot be shown
through fprintd (fixed retry statuses); a "too similar tap" rejection was
tried and dropped.
