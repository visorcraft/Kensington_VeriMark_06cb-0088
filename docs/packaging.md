# Building and installing

Two scripts in `packaging/`: `build-libfprint.sh` builds libfprint with the
driver, `setup-fprintd.sh` makes the system's fprintd use that build. Nothing
is installed over distribution files; fprintd loads the library from the
build directory.

## `build-libfprint.sh`

```
packaging/build-libfprint.sh [--ref <tag|branch>] [--install-deps] [--no-update] [--clean]
```

Run as a normal user (sudo is only used with `--install-deps`); root works
too, e.g. in a container.

1. **Dependencies.** Detects the distribution family from `/etc/os-release`
   (`ID`/`ID_LIKE`) and checks:

   | Needed | Debian / Ubuntu / Mint | RHEL / Fedora / Alma / Rocky |
   |---|---|---|
   | git, meson ≥ the version libfprint asks for, ninja, C and C++ compiler (libfprint's meson project enables C++), pkg-config, python3 | `git meson ninja-build build-essential pkg-config python3` | `git meson ninja-build gcc gcc-c++ pkgconf-pkg-config python3` |
   | glib-2.0, gio-unix-2.0, gobject-2.0, gmodule-2.0 | `libglib2.0-dev` | `glib2-devel` |
   | gusb | `libgusb-dev` | `libgusb-devel` (RHEL 9/10 clones: CRB repository, like meson, ninja-build and gobject-introspection-devel) |
   | OpenSSL ≥ 3.0 | `libssl-dev` | `openssl-devel` |
   | gobject-introspection (`g-ir-scanner`, `.pc`) | `gobject-introspection libgirepository1.0-dev` | `gobject-introspection-devel` |

   Missing items are listed with the exact `apt-get`/`dnf` command, or
   installed with `--install-deps`. Introspection is required because some
   libfprint versions fail to configure without it.
2. **libfprint.** Clones `https://gitlab.freedesktop.org/libfprint/libfprint.git`
   into `build/libfprint` and checks out the latest release tag (`v*`), or
   `--ref` (a tag or branch, e.g. `master`). `--no-update` keeps the checkout
   as it is.
3. **Driver.** Copies `drivers/validity-0088/*.c *.h` into the checkout,
   resets libfprint's two meson files and registers the driver: all `.c`
   files in `driver_sources` of `libfprint/meson.build`, and the OpenSSL
   helper in the root `meson.build` — `drivers_info` (libfprint ≥ 1.94.100)
   or `driver_helper_mapping` (older). With GObject introspection older than
   1.70 (e.g. Rocky/RHEL 9: 1.68), which does not know the `(getter)`
   annotation libfprint uses, it also makes libfprint's introspection
   warnings non-fatal (`fatal_warnings: false`); otherwise libfprint itself
   does not build there.
4. **Build** into `build/libfprint/builddir` with
   `-Ddrivers=validity_0088 -Ddoc=false -Dgtk-examples=false -Dinstalled-tests=false -Dudev_rules=disabled -Dudev_hwdb=disabled`,
   and check that the library contains the driver. The build directory is
   recreated when the libfprint version changes or with `--clean`.

Logs: `build/meson-setup.log`, `build/meson-compile.log`. `build/` is
git-ignored. Tested with v1.94.100, v1.94.10 and master (2026-10).

### Other distributions

The script detects the Debian and RHEL families. Elsewhere, install the same
tools and libraries yourself, then run it (it only checks what is present).
Arch / CachyOS:

```bash
sudo pacman -S git meson ninja base-devel glib2 libgusb openssl gobject-introspection pkgconf python
```

## Building by hand

What the script does, step by step, in any libfprint checkout:

1. Clone libfprint: `git clone https://gitlab.freedesktop.org/libfprint/libfprint.git`
2. Put the driver into it, as a copy or a symlink (with a symlink, a rebuild
   always uses what this repository has checked out):

   ```bash
   cp -r drivers/validity-0088 libfprint/libfprint/drivers/
   # or: ln -s "$PWD/drivers/validity-0088" libfprint/libfprint/drivers/validity-0088
   ```
3. Register it in libfprint's meson files (both snippets are also in
   `drivers/validity-0088/meson-fragment.txt`):
   - `libfprint/libfprint/meson.build`, dictionary `driver_sources`: add
     `'validity_0088' : files(...)` (or `[ ... ]`) listing every `.c` file of
     `drivers/validity-0088/` as `'drivers/validity-0088/<file>.c'`.
   - `libfprint/meson.build` (project root): in `drivers_info` add
     `'validity_0088' : { 'helper': ['openssl'] },` — older trees have a
     `driver_helper_mapping` dictionary instead: `'validity_0088' : [ 'openssl' ],`.

   The key must be `validity_0088` (underscore): meson pastes it into a C
   identifier in the generated `fpi-drivers.c`. Directory and files stay
   `validity-0088`.
4. Build:

   ```bash
   cd libfprint
   meson setup builddir -Ddrivers=validity_0088 -Ddoc=false
   meson compile -C builddir
   ```

   The result is `builddir/libfprint/libfprint-2.so.2`.
5. Try it without installing (finger menu: 6 = right index; the examples keep
   their prints in `test-storage.variant` in the current directory):

   ```bash
   LD_LIBRARY_PATH=$PWD/builddir/libfprint ./builddir/examples/enroll
   LD_LIBRARY_PATH=$PWD/builddir/libfprint ./builddir/examples/verify
   ```

## `setup-fprintd.sh`

Run as root (`sudo`), from a normal terminal — sandboxed shells that set the
"no new privileges" flag cannot use sudo.

| Command | Effect |
|---|---|
| `<builddir>` | Write `fprintd.service.d/override.conf`: bind-mount the build's library directory to `/run/libfprint-validity-0088` and point `LD_LIBRARY_PATH` at it; `RequiresMountsFor=` the build directory. Keeps a `.bak` copy of an existing drop-in, restarts fprintd and checks that it mapped the library from the build. Symlinked paths are resolved. |
| `--status` | Library in use, encryption on/off, key source, number of encrypted and unencrypted prints of this driver |
| `--encrypt-prints` | Store the print key as systemd credential `/etc/credstore.encrypted/validity0088-print-key` (host+tpm2 when the TPM is usable, else host; no PCR binding), importing an existing `print-key` file; drop-in `validity0088-print-key.conf` with `ImportCredential=` and `DeviceAllow=/dev/tpmrm0 rw` |
| `--drop-file-key` | Delete the `print-key` file once the credential holds the same key |
| `--encryption-off` / `--encryption-on` | Drop-in `validity0088-encryption.conf` with `VALIDITY0088_PRINT_ENCRYPTION=0` / remove it; encrypted prints stay readable while the key exists |
| `--delete-key [--yes]` | Delete credential, key file and key drop-in after confirmation; encrypted prints become unreadable |
| `--remove` | Remove all drop-ins of this script; the credential is kept |

After rebuilding the library: `sudo systemctl restart fprintd`.

### Without the script

`sudo systemctl edit fprintd.service` and add (real path of the build, no
symlinks):

```ini
[Unit]
RequiresMountsFor=/path/to/libfprint/builddir/libfprint

[Service]
Environment=LD_LIBRARY_PATH=/run/libfprint-validity-0088
BindReadOnlyPaths=/path/to/libfprint/builddir/libfprint:/run/libfprint-validity-0088
```

then `sudo systemctl daemon-reload && sudo systemctl restart fprintd`. For print
encryption with a systemd credential, use the script: it also writes the
matching drop-ins.

### Why it is done this way

- **`ProtectHome=yes`** in fprintd's unit hides `/home` from the service, so
  the library directory is bind-mounted to a path under `/run`
  (`BindReadOnlyPaths=`).
- **`RequiresMountsFor=`** makes fprintd wait at boot for the disk that holds
  the build.
- **`DeviceAllow=/dev/tpmrm0 rw`**: systemd decrypts imported credentials
  inside the unit's device policy, and fprintd's unit only allows the
  fingerprint device classes. Without the line the TPM cannot be opened and
  fprintd exits with status 243.
- **Undecryptable credential → fprintd does not start** (systemd 255, tested
  with `ImportCredential=` and `LoadCredentialEncrypted=`; a *missing*
  credential is fine). Recovery:
  [print-encryption.md](print-encryption.md#recovery); a helper-unit design
  that avoids the problem is described there too.

## Storage under fprintd

`/var/lib/fprint/validity-0088-pairing/` (fprintd's `StateDirectory`) holds
the pairing data, `calibration.bin` and, without a credential, `print-key`.
It is separate from the examples' `~/.local/share/libfprint/verimark-06cb-0088/`,
so fprintd calibrates once on its first enroll or verify. Prints:
`/var/lib/fprint/<user>/validity-0088/<device id>/<finger>`.

Without systemd (OpenRC, runit …) the driver uses root's
`~/.local/share/libfprint/verimark-06cb-0088/` and the `print-key` file;
`setup-fprintd.sh` itself needs systemd.

## PAM

**Debian / Ubuntu / Mint:** `sudo pam-auth-update --enable fprintd`.

**Arch / CachyOS** (by hand). First open a second TTY (Ctrl+Alt+F3) and log
in there, so you have a way back if something goes wrong. Then:

```bash
sudo cp /etc/pam.d/system-auth /etc/pam.d/system-auth.bak
sudo $EDITOR /etc/pam.d/system-auth
```

and add one line at the very top of the `auth` section, before
`auth required pam_unix.so`:

```
auth       sufficient pam_fprintd.so
```

This covers the login manager (e.g. SDDM, Plasma Login Manager) and the
screen locker. For `sudo`, add the same line at the top of `/etc/pam.d/sudo`.

With `sufficient`, a fingerprint that is not recognised or not available
falls back to the password, so you cannot lock yourself out as long as the
password works.

## udev

`packaging/70-vmark.rules` gives the logged-in user access to the device
(`uaccess`), for running the libfprint examples without sudo. fprintd does
not need it.
