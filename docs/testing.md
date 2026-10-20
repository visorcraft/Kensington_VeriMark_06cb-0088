# Tests

`tests/run.sh` builds and runs all tests; no device is needed. Run
`packaging/build-libfprint.sh` first: the tests that use the driver's
libfprint-dependent code take their compiler flags from that build.

```bash
packaging/build-libfprint.sh
tests/run.sh                    # all tests
tests/run.sh test-matcher       # one test
```

Output and logs go to `build/tests/` (git-ignored). The whole suite takes
about 15 seconds.

| Test | What it checks |
|---|---|
| `test-matcher` | The SIFT matcher on **artificial fingerprints**: same finger matches and other fingers do not (false-accept rate, 95th percentile, hard maximum), rotation up to ±20°, coverage and print updates, keypoint serialisation, generator determinism |
| `test-print` | Print encryption (round trip, other device, tampered key id / nonce / ciphertext / tag, credential vs. key file, missing and broken keys), the print-data fallback, and all print formats (format 2, format 1, legacy raw scans) scored with the driver's code, plus loading a print for a print update |
| `test-capture-builder` | Capture and calibration programs from fixed synthetic calibrations against SHA-256 values (identical for the old full upstream template and the current skeleton), refusal of an incompatible trim, missing calibration, `calibration.bin` round trip, capture mode byte |
| `test-pairing-key` | The pairing signing key: scalar 1 gives the P-256 base point G (known answer), random scalars, signatures, the derived `hs_key` |
| `test-setup-fprintd` | `packaging/setup-fprintd.sh` in a sandbox with stand-ins for `systemctl` and `systemd-creds`: every option and its error cases. Needs root or unprivileged user namespaces (`unshare -r`), otherwise skipped |
| `shellcheck` | The shell scripts, if `shellcheck` is installed |

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request, manually and
weekly (to notice new libfprint releases):

- **Lint:** shellcheck on all scripts, `tests/check-repo.sh` (driver sources
  listed once in `meson-fragment.txt`, SPDX lines, no device or
  biometric data tracked, documentation links).
- **Build and test** in containers: Ubuntu 24.04, Debian 12 and 13, Fedora
  42, Rocky Linux 9 and 10, each against the latest libfprint release; Ubuntu also
  against libfprint `master` (allowed to fail: an early warning about
  upstream changes). Each job runs `packaging/build-libfprint.sh
  --install-deps`, fails on any compiler warning in the driver, runs
  `tests/run.sh` and uploads the logs when it fails.

The same jobs run locally with podman or docker before pushing:

```bash
.github/ci/run-local.sh                  # all images
.github/ci/run-local.sh fedora:42        # one image
```

Last local run (2026-10-20): lint and all six distributions passed
(Ubuntu 24.04, Debian 12 and 13, Fedora 42, Rocky Linux 9 and 10, libfprint
v1.94.100), 80–150 s per distribution.

`.github/ci/prepare-container.sh` holds the per-distribution preparation
shared by both (certificates and git; on RHEL clones the CRB repository for
`libgusb-devel` and `meson`).

What the tests cannot cover: anything with the sensor itself (USB, TLS
session, capture, calibration on hardware, LED). Before a release, run the
manual checks: enroll and verify with the libfprint examples, one forced
recalibration (`VALIDITY0088_RECALIBRATE=1`), and enroll + verify through
fprintd.

## Artificial fingerprints (`tests/synth-finger.c`)

Real scans are biometric data and stay out of the repository, so the matcher
is tested on generated fingers. A finger is a 320 × 320 ridge pattern grown
from noise by repeated oriented Gabor filtering along an orientation field
with a core and a delta (zero-pole model, sometimes a second core) and a
smooth random perturbation. Per finger it also varies the ridge period
(8.5–11.5 px), the ridge thickness (a field of Gaussian bumps shifting the
ridge/valley threshold) and adds ridge breaks. A tap is a 56 × 144 window at
a given position and rotation with pressure, blur and noise variation,
scaled like the sensor's images (grey mean 128.5, standard deviation 58 —
both measured on real scans, as was the ridge period).

Compared with the real field test ([evaluation.md](evaluation.md)):

| | Synthetic (16 fingers) | Real (10 fingers) |
|---|---|---|
| right finger, median score | 47 | 46 |
| wrong finger, median / p95 / max | 5 / 7 / 13 | 2 / 4 / 7 |

Synthetic wrong-finger scores are somewhat higher: all synthetic loop cores
come from the same model and look more alike than real ones (1 of 2400
synthetic comparisons reached 12). `test-matcher` therefore checks the
false-accept rate (≤ 1 %), the 95th percentile (≤ 8) and a hard maximum
(< 15) rather than "never 12". Real-world tests with several people should
look at taps over the core in particular.

`tests/test-matcher --write-taps <dir>` writes a few synthetic taps as PGM
images.

## Do the tests catch bugs?

Checked by planting one bug at a time; all were caught (2026-10-20): ratio
test off, rotation limit 8°, RANSAC tolerance ×6, device id missing from the
encryption AAD, key file mode 644, broken key falling back to plaintext,
changed timeslot scaling, mode byte offset off by one, public key part not
imported, the old `nm | grep -q` check in `setup-fprintd.sh`, missing
`DeviceAllow` for the TPM, no backup of an existing drop-in.
