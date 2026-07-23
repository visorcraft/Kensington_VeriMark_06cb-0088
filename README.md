# Kensington VeriMark (06cb:0088) Linux Driver

An open-source `libfprint` driver for the Kensington VeriMark fingerprint key.

> **Install, plug in, works.**
>
> 1. Clone this repo into a fresh `libfprint` source tree (section 2)
> 2. `meson compile -C builddir`
> 3. Plug in your VeriMark
> 4. `fprintd-enroll` (or `examples/enroll` for testing)
>
> The driver auto-pairs with the device on first open and persists everything to `~/.local/share/libfprint/verimark-06cb-0088/`. Works on both unpaired devices and devices that have been paired elsewhere; multiple machines can independently pair the same device.

## 1. Device Information

This driver targets the **Kensington VeriMark Gen 1 USB-A Fingerprint Key** (USB ID `06cb:0088`), Kensington model K67977 (or K67977WW worldwide).

The VeriMark is a small USB-A dongle with a tap (capacitive area) fingerprint sensor, built around a Synaptics / Validity pre-Prometheus chip. It speaks a Synaptics-modified TLS 1.2 protocol over USB bulk transfers. This repository contains a clean-room Linux implementation of that protocol as a `libfprint` driver, packaged for drop-in integration with the upstream `libfprint` source tree.

When everything is wired up the VeriMark works with `fprintd` (and therefore with PAM-based fingerprint authentication on most Linux desktops and login managers).

## 2. Compile the Driver

The driver compiles into `libfprint-2.so` and is loaded by `libfprint` automatically when a `06cb:0088` device is plugged in.

### Prerequisites

| Tool / library    | Notes                                           |
|-------------------|-------------------------------------------------|
| `meson` >= 0.59   | Build system                                    |
| `ninja`           | Backend used by meson                           |
| `gcc` or `clang`  | C compiler                                      |
| `glib2` (dev)     | GLib / GObject runtime + headers                |
| `gusb` (dev)      | USB transport used by libfprint                 |
| `gudev` (dev)     | udev integration                                |
| `nss` (dev)       | Used by libfprint's NBIS minutiae routines      |
| `openssl` >= 3 (dev) | Used by this driver for AES, HMAC, ECDH      |
| `pixman` (dev)    | Image scaling used by libfprint                 |
| `gobject-introspection` (dev) | Hard-required by libfprint's meson (or configure with `-Dintrospection=false`) |
| `udev`            | Provides `udev.pc`, hard-required by libfprint's meson. Preinstalled on desktop systems; must be installed in minimal environments (containers, chroots, WSL) |
| `pkg-config`      | Build introspection                             |

On Arch / CachyOS:

```bash
sudo pacman -S meson ninja base-devel glib2 libgusb libgudev nss openssl pixman pkgconf gobject-introspection
```

On Debian / Ubuntu:

```bash
sudo apt install meson ninja-build build-essential libglib2.0-dev \
    libgusb-dev libgudev-1.0-dev libnss3-dev libssl-dev libpixman-1-dev \
    libgirepository1.0-dev udev pkg-config
```

> **Note:** `libgirepository1.0-dev` (Debian/Ubuntu) / `gobject-introspection`
> (Arch) satisfies libfprint's hard `gobject-introspection-1.0` meson
> dependency; if you don't want introspection support, you can instead add
> `-Dintrospection=false` to the `meson setup` line below. The `udev` package
> provides `udev.pc`, which libfprint's meson also hard-requires — it is
> preinstalled on desktop systems but missing in minimal environments
> (containers, chroots, WSL).

### Build steps

1. **Clone this repository.**

   ```bash
   git clone https://github.com/visorcraft/Kensington_VeriMark_06cb-0088.git
   cd Kensington_VeriMark_06cb-0088
   ```

2. **Clone upstream `libfprint`** somewhere convenient. The driver is meant to live inside its source tree.

   ```bash
   git clone https://gitlab.freedesktop.org/libfprint/libfprint.git
   ```

3. **Copy the driver source into libfprint:**

   ```bash
   cp -r drivers/validity-0088 libfprint/libfprint/drivers/
   ```

4. **Register the driver with libfprint's meson.** Two upstream files need a one-line entry added:

   **(a)** Open `libfprint/libfprint/meson.build` (the inner one) and find the `driver_sources` dictionary (entries like `'elan' :`, `'synaptics' :`). Add:

   ```meson
   'validity_0088' :
       [ 'drivers/validity-0088/validity.c',
         'drivers/validity-0088/validity-capture-builder.c',
         'drivers/validity-0088/validity-commands.c',
         'drivers/validity-0088/validity-crypto.c',
         'drivers/validity-0088/validity-handshake.c',
         'drivers/validity-0088/validity-init-data.c',
         'drivers/validity-0088/validity-init-clean-slate.c',
         'drivers/validity-0088/validity-enroll-data.c',
         'drivers/validity-0088/validity-pairing.c',
         'drivers/validity-0088/validity-pretls-pair.c',
         'drivers/validity-0088/validity-pubkeys.c' ],
   ```

   **(b)** Open `libfprint/meson.build` (the outer one, at the project root) and find the `driver_helper_mapping` dictionary (entries like `'uru4000' : [ 'openssl' ],`). Add:

   ```meson
   'validity_0088' : [ 'openssl' ],
   ```

   This wires the driver up against libcrypto, which it uses for AES, HMAC, ECDH, and ECDSA.

   The dictionary key MUST be `validity_0088` (underscore) — upstream's meson pastes it directly into a generated C identifier in `fpi-drivers.c` and a hyphen there produces invalid C. The directory and file paths stay as `validity-0088`. Both snippets live in `drivers/validity-0088/meson-fragment.txt`.

5. **Configure + build libfprint** with the new driver enabled:

   ```bash
   cd libfprint
   meson setup --wipe builddir -Ddrivers=validity_0088 -Ddoc=false
   meson compile -C builddir
   ```

   A successful build produces `builddir/libfprint/libfprint-2.so.2`.

6. **Install the udev rule** (optional, only needed if you plan to use `libfprint` examples directly without `sudo`):

   ```bash
   sudo cp ../Kensington_VeriMark_06cb-0088/packaging/70-vmark.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   ```

7. **Sanity check the build** by running the bundled `examples/verify` against the live device. Plug the VeriMark in and run:

   ```bash
   LD_LIBRARY_PATH=$(pwd)/builddir/libfprint ./builddir/examples/verify
   ```

   At the "Choose finger" prompt, pick `6` (right index). On first run with no enrolled finger you'll get `Did you remember to enroll your finger first?` — that's expected; run `examples/enroll` instead to enroll, then come back.

### Per-device data

**You don't need to supply any.** The driver auto-pairs on first open: it generates a fresh keypair, exchanges it with the device, and persists `cert-blob`, `host-key.pem`, and `device-ecdh-pubkey.bin` to `~/.local/share/libfprint/verimark-06cb-0088/`. Subsequent opens reuse the on-disk material.

To skip the autonomous ceremony (e.g., for debugging), set `VALIDITY0088_SKIP_AUTO_PAIR=1`. With that set and no on-disk pair material present, `open()` will fail rather than self-pair, and you can drop your own `cert-blob`, `host-key.pem`, and `device-ecdh-pubkey.bin` into the pairing storage directory by hand.

### Environment variables

| Environment variable | Effect |
|---|---|
| `VALIDITY0088_SKIP_AUTO_PAIR=1` | Disable the autonomous pairing on first open. `open()` will fail instead of self-pairing when no on-disk pair material is present. |
| `VALIDITY0088_FLASH_LIVE=1` | Allow the post-handshake pairing ceremony's flash-write step to actually send `0x3f` / `0x41` / `0x42` frames to the device. **The default is dry-run**: the driver computes every frame and hexdumps it via the journal but does NOT send anything. Only set this once you've reviewed the dry-run output and accept the risk of touching partition 1 (the cert store). |
| `VALIDITY0088_FLASH_DRY_RUN=1` | Force dry-run mode even when `VALIDITY0088_FLASH_LIVE=1` is set. Useful as an explicit safety override in scripts. |
| `VALIDITY0088_FORCE_REPAIR=1` | On the next `open()`, wipe the pairing storage dir so the bootstrap runs fresh. The storage dir is `$STATE_DIRECTORY/validity-0088-pairing` when running under `fprintd` (typically `/var/lib/fprint/validity-0088-pairing/`) and `$XDG_DATA_HOME/libfprint/verimark-06cb-0088/` for CLI tools. |
| `VALIDITY0088_STORAGE_DIR=<path>` | Override the pairing-storage directory entirely (testing / non-systemd hosts). |
| `VALIDITY0088_FLASH_SIGNATURE_HEX=<hex>` | When the cert-store flash write needs an attached firmware-extension signature, supply it as a hex string. Not required for the default cert-store write path. |
| `VALIDITY0088_ENROLL_HOSTPART_CMD=<path>` | Override the embedded enroll-time HOSTPART payload with raw bytes at the given path. The file must be 10501 B and start with the `0x06` wire opcode. Debugging aid; not needed in normal operation. |
| `VALIDITY0088_ENROLL_CAPTURE_CMD=<path>` | Override the embedded capture-program command with raw bytes at the given path. The file must be 18869 B and start with the `0x02` wire opcode. Auto-engaged on devices whose ROM family doesn't match the family the builder targets; otherwise intended only for debugging. |

## 3. Quick Setup (use the fingerprint on the login screen)

The steps below are written for **CachyOS / Arch with KDE Plasma 6** and the Plasma Login Manager (`plasmalogin`). They also apply almost verbatim to other Arch-based systems using SDDM or to other distros with similar PAM stacks; just substitute the right PAM service name.

### Step 1 - Make `fprintd` use your built `libfprint`

**Easy path — use the bundled setup script:**

```bash
sudo packaging/setup-fprintd.sh /path/to/libfprint/builddir
```

That writes the systemd drop-in, reloads the daemon, and restarts `fprintd.service`. Skip to step 2.

**Manual path** — if you'd rather edit by hand, run:

```bash
sudo systemctl edit fprintd.service
```

Add (replace the path with your actual location):

```ini
[Service]
Environment=LD_LIBRARY_PATH=/run/libfprint-validity-0088
BindReadOnlyPaths=/path/to/libfprint/builddir/libfprint:/run/libfprint-validity-0088
```

The `BindReadOnlyPaths=src:dst` line bind-mounts the build's lib directory at a stable system path (`/run/libfprint-validity-0088`) inside fprintd's mount namespace, then points `LD_LIBRARY_PATH` at that mount. This is needed because the fprintd unit ships with `ProtectHome=yes`, which hides `/home` from the service — bind-mounting to a `/home`-relative destination wouldn't help (the destination is under the hidden tmpfs), so we bind to a path outside `/home`.

Save, then reload and restart:

```bash
sudo systemctl daemon-reload
sudo systemctl restart fprintd.service
```

### Step 2 - Confirm fprintd sees the device

```bash
fprintd-list "$USER"
```

Expected output names the device as `Synaptics/Validity (pre-Prometheus)`. If you see "No devices available" instead, double-check the `LD_LIBRARY_PATH` and `BindReadOnlyPaths` in the systemd override.

### Step 3 - Enroll a fingerprint

```bash
fprintd-enroll "$USER"
```

You will be prompted to "Place your finger" ten times. Tap a single finger consistently each time. The LED glows blue while the device waits for a tap. After the tenth accepted tap, `fprintd` reports success and the print is stored in its system-managed location.

### Step 4 - Wire PAM for the login screen

Open a second TTY (Ctrl+Alt+F3) and log in there before changing PAM, so you have a fallback if you misconfigure something.

```bash
sudo cp /etc/pam.d/system-auth /etc/pam.d/system-auth.bak
sudo $EDITOR /etc/pam.d/system-auth
```

Add ONE line at the very top of the `auth` section, before the existing `auth required pam_unix.so` line:

```
auth       sufficient pam_fprintd.so
```

`sufficient` means: if the fingerprint scan succeeds, that is enough to authenticate; if it fails or the user cancels, the prompt falls back to your password. This is the safe setting: as long as your password still works, you cannot lock yourself out.

### Step 5 - Log out and tap to log in

Log out of your current Plasma session (or reboot). At the login screen, click your user and tap your enrolled finger. The desktop should load. If the fingerprint is not recognized, the prompt falls back to asking for your password.

The same PAM stack also applies to KDE's screen locker, so unlocking the screen via fingerprint works after this is set up.

To extend fingerprint authentication to `sudo`, add the same `auth sufficient pam_fprintd.so` line to the top of `/etc/pam.d/sudo`.

## 4. Copyright

This project is published under the GNU Lesser General Public License version 2.1 or later (LGPL-2.1-or-later), matching upstream `libfprint`. See the `LICENSE` file for the full text.

**Disclaimer of affiliation.** This is an independent, open-source community project. The maintainers of this repository have no affiliation with, endorsement from, or sponsorship by Kensington Computer Products Group, ACCO Brands Corporation, Synaptics Incorporated, or any other company whose hardware or software is referenced in this work. "Kensington" and "VeriMark" are trademarks of their respective owners; all references to those marks in this repository are descriptive nominative use only, used solely to identify the hardware device this driver supports. Any other product or company names mentioned may be trademarks of their respective owners.

**No warranty.** This driver is provided "as is", without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose, and noninfringement. In no event shall the contributors be liable for any claim, damages, or other liability arising from the use of this software. See the LICENSE file for the full warranty disclaimer.

**Security note.** Fingerprint authentication on a consumer USB device is convenient but not equivalent to a hardware-isolated secure enclave. Treat it as a convenience layer on top of (not a replacement for) strong password authentication and full-disk encryption.
