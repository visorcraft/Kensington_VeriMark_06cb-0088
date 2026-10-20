# Kensington VeriMark (06cb:0088) Linux Driver

[![CI](https://github.com/visorcraft/Kensington_VeriMark_06cb-0088/actions/workflows/ci.yml/badge.svg)](https://github.com/visorcraft/Kensington_VeriMark_06cb-0088/actions/workflows/ci.yml)

An open-source [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint)
driver for the **Kensington VeriMark Gen 1 USB fingerprint key** (USB ID
`06cb:0088`, model K67977 / K67977WW). With it the key works with `fprintd`,
and so for login, screen unlock and `sudo` on most Linux desktops.

No Windows driver, registry data or capture files are needed.

## Supported systems

| | Systems |
|---|---|
| Built and tested in CI | Ubuntu 24.04, Debian 12 and 13, Fedora 42, Rocky Linux 9 and 10 — against the latest libfprint release ([details](docs/testing.md#continuous-integration)) |
| Tested with the device | Linux Mint (Ubuntu 24.04 base) with fprintd 1.94.3 |
| Should work | Other systemd-based distributions with libfprint ≥ 1.94; the build script installs dependencies on the Debian and RHEL families, elsewhere install them yourself ([Arch](docs/packaging.md#other-distributions)) |

The setup script for fprintd needs systemd. The driver itself also works on
other init systems ([details](docs/packaging.md#storage-under-fprintd)).

## How it works

- **Pairing:** on first use the driver pairs with the key (a Synaptics-modified
  TLS 1.2 session) and stores the pairing; a key paired elsewhere works too.
- **Calibration:** the first enroll or verify calibrates the sensor —
  **keep your finger off the sensor until you are asked to tap.**
- **Matching** happens on the computer (the sensor only takes 56 × 144-pixel
  images, about 7 × 3 mm): SIFT keypoints, threshold 12. In a test with all
  ten fingers of one person, wrong fingers never came close to the threshold.
- **Prints** contain keypoints only, no images, and are stored **encrypted**.

Details: [calibration](docs/capture-program.md), [matcher](docs/matcher.md),
[test results](docs/evaluation.md), [print encryption](docs/print-encryption.md).

## Installation

```bash
git clone https://github.com/visorcraft/Kensington_VeriMark_06cb-0088.git
cd Kensington_VeriMark_06cb-0088

# 1. fetch libfprint, add the driver, build (--install-deps installs missing packages)
packaging/build-libfprint.sh --install-deps

# 2. make the system's fprintd use this build
sudo packaging/setup-fprintd.sh build/libfprint/builddir

# 3. check that fprintd sees the key: expect "Synaptics/Validity (pre-Prometheus)"
fprintd-list "$USER"
```

Nothing is installed over your distribution's files: fprintd loads the library
from the build directory through a systemd drop-in. Undo with
`sudo packaging/setup-fprintd.sh --remove`. Run `sudo` commands in a normal
terminal (sandboxed shells that set "no new privileges" cannot use sudo).

**Fingerprint login:** on Debian / Ubuntu / Mint run
`sudo pam-auth-update --enable fprintd`; on other distributions see
[docs/packaging.md#pam](docs/packaging.md#pam). A finger that is not
recognised always falls back to the password.

**Optional, recommended with a TPM:** keep the print encryption key in a
TPM-sealed systemd credential instead of a file:

```bash
sudo packaging/setup-fprintd.sh --encrypt-prints
fprintd-verify "$USER"                              # check it still works
sudo packaging/setup-fprintd.sh --drop-file-key     # then remove the plain key file
```

The TPM itself is not modified, so a dual-boot Windows (BitLocker included) is
not affected. More options (`--status`, `--encryption-off`, …):
[docs/packaging.md](docs/packaging.md#setup-fprintdsh).

**Updating:** `git pull`, `packaging/build-libfprint.sh`, then
`sudo systemctl restart fprintd`.

## Using it

```bash
fprintd-enroll "$USER"      # 10 taps
fprintd-verify "$USER"
```

- **Vary the placement while enrolling** — centre, tip, left and right edge,
  lower part. The sensor sees only a small part of the finger, and a later tap
  only matches areas the enrollment covered.
- Lift the finger completely between taps. A tap that is too short or too
  light is simply asked for again.
- If fingers are already enrolled, fprintd first asks for one tap to check for
  a duplicate.
- Enrolling a finger again with fprintd replaces its print.

## Troubleshooting

Start with the log: `journalctl -u fprintd -n 50`.

| Problem | What to do |
|---|---|
| `fprintd-list` shows no device | Run `sudo packaging/setup-fprintd.sh --status`; after moving or rebuilding the build directory, run `setup-fprintd.sh` again with its path. |
| "device did not return an image" | The tap was too short or too light; tap again. |
| A finger matches only in some placements | Enroll it again with more varied placement. |
| Many failed scans, low scores (e.g. after moving the key to another computer) | Recalibrate: delete `/var/lib/fprint/validity-0088-pairing/calibration.bin` (as root) and restart fprintd; the next scan calibrates (finger off). |
| fprintd does not start, journal shows `243/CREDENTIALS` | The print key credential can no longer be decrypted, usually because the TPM was cleared. Encrypted prints are lost; recovery steps in [docs/print-encryption.md](docs/print-encryption.md#recovery). |
| "Stored print is encrypted with key …, which is not available" | Its key is gone (deleted, other computer). Restore the key from a backup, or delete the print and enroll again. |
| Prints from a much older version (NBIS era) do not verify | `fprintd-delete "$USER"` and enroll again. |

Configuration variables: [docs/configuration.md](docs/configuration.md).

## Development

- Building by hand, the scripts in detail: [docs/packaging.md](docs/packaging.md)
- Tests without a device: `tests/run.sh` ([docs/testing.md](docs/testing.md));
  CI jobs locally: `.github/ci/run-local.sh`
- Protocol, calibration, matcher, encryption, evaluation: [docs/](docs/README.md)

## Licence

LGPL-2.1-or-later, like libfprint ([LICENSE](LICENSE)). Copyright © 2026
VisorCraft LLC (original driver) and Radek Sipka (this fork). Third-party
software and how the protocol was obtained: [docs/licensing.md](docs/licensing.md).

**Not affiliated** with Kensington, ACCO Brands or Synaptics; "Kensington" and
"VeriMark" are trademarks of their owners, used only to name the supported
device. **No warranty:** provided "as is", see `LICENSE`. **Security:**
fingerprint login on a consumer USB device is a convenience on top of — not a
replacement for — a strong password and disk encryption.
