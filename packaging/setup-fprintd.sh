#!/bin/bash
#
# setup-fprintd.sh — point fprintd at a freshly-built libfprint
#
# Writes /etc/systemd/system/fprintd.service.d/override.conf so the
# fprintd service loads libfprint-2.so from the user's build instead of
# the distro copy. Uses BindReadOnlyPaths= so fprintd's ProtectHome=yes
# sandbox stays intact — only the build's library directory becomes
# visible to the service.
#
# Usage:
#   sudo ./setup-fprintd.sh /path/to/libfprint/builddir
#   sudo ./setup-fprintd.sh --encrypt-prints
#   sudo ./setup-fprintd.sh --drop-file-key
#   sudo ./setup-fprintd.sh --encryption-off | --encryption-on
#   sudo ./setup-fprintd.sh --delete-key [--yes]
#   sudo ./setup-fprintd.sh --status
#   sudo ./setup-fprintd.sh --remove
#
# Print encryption is on by default (the driver creates a key file at the
# first enrollment). --encryption-off makes fprintd store new prints
# unencrypted; prints that are already encrypted stay readable as long as
# their key exists. --delete-key removes the key (credential and key file),
# which makes encrypted prints unreadable; it asks for confirmation.
#
# --encrypt-prints stores the driver's print encryption key as a systemd
# credential (/etc/credstore.encrypted/validity0088-print-key, sealed with
# the TPM when available, no PCR binding) and passes it to fprintd with
# ImportCredential=. An existing key file of fprintd's storage directory is
# imported, so prints encrypted with it stay readable. --drop-file-key then
# deletes that key file once the credential is known to hold the same key.
# See docs/print-encryption.md.
#
# The argument is the meson build directory (the parent of the
# libfprint/ subdir that contains libfprint-2.so.2.0.0); that subdir
# itself is accepted too.
#
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

die () { echo "setup-fprintd: $*" >&2; exit 1; }
say () { echo "setup-fprintd: $*"; }

OVERRIDE_DIR=/etc/systemd/system/fprintd.service.d
OVERRIDE_FILE="$OVERRIDE_DIR/override.conf"
KEY_DROPIN="$OVERRIDE_DIR/validity0088-print-key.conf"
ENC_DROPIN="$OVERRIDE_DIR/validity0088-encryption.conf"
PRINT_ROOT=/var/lib/fprint
CRED_NAME=validity0088-print-key
CRED_DIR=/etc/credstore.encrypted
CRED_FILE="$CRED_DIR/$CRED_NAME"
# fprintd's StateDirectory=fprint + the driver's storage subdirectory.
KEY_FILE=/var/lib/fprint/validity-0088-pairing/print-key
# Stable destination OUTSIDE /home so it stays reachable under the
# fprintd unit's ProtectHome=yes sandbox.
EXPOSED=/run/libfprint-validity-0088
LIB_NAME=libfprint-2.so.2.0.0

[ "$(id -u)" -eq 0 ] || die "must be run as root (try: sudo $0 ...)"
USAGE="usage: $0 /path/to/libfprint/builddir | --encrypt-prints | --drop-file-key |
       --encryption-off | --encryption-on | --delete-key [--yes] | --status | --remove"
[ $# -eq 1 ] || { [ $# -eq 2 ] && [ "$1" = --delete-key ] && [ "$2" = --yes ]; } || die "$USAGE"

systemctl cat fprintd.service >/dev/null 2>&1 \
    || die "fprintd.service is not installed on this system"

# Keep a copy of an existing drop-in unless it is identical to the new one.
# Never reuse a name, so no earlier backup is overwritten. systemd only reads
# *.conf files from the drop-in directory, so the copies are ignored.
backup_override () {
    if [ -f "$OVERRIDE_FILE" ] && ! cmp -s "$OVERRIDE_FILE" "$1"; then
        local bak="$OVERRIDE_FILE.bak" n=1
        while [ -e "$bak" ]; do
            bak="$OVERRIDE_FILE.bak.$n"
            n=$((n + 1))
        done
        cp -p -- "$OVERRIDE_FILE" "$bak"
        say "previous drop-in saved as $bak"
    fi
}

# Size in bytes of the decrypted credential (the key only goes through a pipe).
cred_size () {
    systemd-creds decrypt --name="$CRED_NAME" "$1" - 2>/dev/null | wc -c
}

# Prints of this driver stored by fprintd: <root>/<user>/validity-0088/<dev>/<finger>.
# Prints with fpi-data type (uayayay) are encrypted.
count_prints () {
    local enc=0 plain=0 f
    while IFS= read -r -d '' f; do
        if grep -qF '(uayayay)' "$f"; then enc=$((enc + 1)); else plain=$((plain + 1)); fi
    done < <(find "$PRINT_ROOT" -mindepth 4 -maxdepth 4 -type f -path "*/validity-0088/*" -print0 2>/dev/null)
    echo "$enc $plain"
}

restart_fprintd () {
    systemctl daemon-reload
    systemctl try-restart fprintd.service \
        || die "fprintd failed to restart; see: journalctl -u fprintd -n 30"
}

if [ "$1" = "--status" ]; then
    read -r ENC PLAIN < <(count_prints)
    if [ -f "$OVERRIDE_FILE" ]; then
        say "library:    $(sed -n 's/^BindReadOnlyPaths=\([^:]*\):.*/\1/p' "$OVERRIDE_FILE")"
    else
        say "library:    distro libfprint (no $OVERRIDE_FILE)"
    fi
    if [ -f "$ENC_DROPIN" ]; then
        say "encryption: OFF for new prints ($ENC_DROPIN)"
    else
        say "encryption: on for new prints"
    fi
    if [ -f "$CRED_FILE" ]; then
        if [ "$(cred_size "$CRED_FILE")" -eq 32 ]; then
            say "credential: $CRED_FILE (decrypts)"
        else
            say "credential: $CRED_FILE (CANNOT be decrypted)"
        fi
        [ -f "$KEY_DROPIN" ] || say "            not passed to fprintd: no $KEY_DROPIN"
    else
        say "credential: none"
    fi
    [ -f "$KEY_FILE" ] && say "key file:   $KEY_FILE" || say "key file:   none"
    if [ "$ENC" -gt 0 ] && [ ! -f "$CRED_FILE" ] && [ ! -f "$KEY_FILE" ]; then
        say "prints:     $ENC encrypted (UNREADABLE: no key), $PLAIN unencrypted"
    else
        say "prints:     $ENC encrypted, $PLAIN unencrypted"
    fi
    exit 0
fi

if [ "$1" = "--encryption-off" ]; then
    mkdir -p "$OVERRIDE_DIR"
    cat > "$ENC_DROPIN" <<EOF
# Generated by Kensington_VeriMark_06cb-0088/packaging/setup-fprintd.sh --encryption-off
# New validity-0088 prints are stored unencrypted. Undo: --encryption-on
[Service]
Environment=VALIDITY0088_PRINT_ENCRYPTION=0
EOF
    chmod 0644 "$ENC_DROPIN"
    restart_fprintd
    read -r ENC PLAIN < <(count_prints)
    say "new prints are now stored unencrypted"
    if [ "$ENC" -gt 0 ]; then
        say "$ENC encrypted print(s) stay readable as long as the key exists;"
        say "enroll those fingers again to store them unencrypted"
    fi
    exit 0
fi

if [ "$1" = "--encryption-on" ]; then
    [ -f "$ENC_DROPIN" ] || { say "encryption is already on"; exit 0; }
    rm -f -- "$ENC_DROPIN"
    restart_fprintd
    say "new prints are stored encrypted again"
    [ -f "$CRED_FILE" ] || [ -f "$KEY_FILE" ] \
        || say "a key file will be created at the next enrollment (or run --encrypt-prints)"
    exit 0
fi

if [ "$1" = "--delete-key" ]; then
    [ -f "$CRED_FILE" ] || [ -f "$KEY_FILE" ] || die "no print key to delete"
    read -r ENC PLAIN < <(count_prints)
    say "this deletes the print encryption key:"
    [ -f "$CRED_FILE" ] && say "  $CRED_FILE"
    [ -f "$KEY_FILE" ] && say "  $KEY_FILE"
    say "$ENC encrypted print(s) will become unreadable ($PLAIN unencrypted print(s) are not affected)"
    [ -f "$ENC_DROPIN" ] \
        || say "encryption is still on: the next enrollment creates a new key (use --encryption-off first to stop that)"
    if [ "${2:-}" != --yes ]; then
        [ -t 0 ] || die "not a terminal: add --yes to confirm"
        read -r -p "setup-fprintd: type 'delete' to continue: " ANSWER
        [ "$ANSWER" = delete ] || die "cancelled, nothing changed"
    fi
    [ -f "$KEY_FILE" ] && { shred -u -- "$KEY_FILE" 2>/dev/null || rm -f -- "$KEY_FILE"; }
    rm -f -- "$CRED_FILE" "$KEY_DROPIN"
    restart_fprintd
    say "print key deleted"
    if [ "$ENC" -gt 0 ]; then
        say "remove the unreadable prints and enroll again, e.g.:"
        say "  fprintd-delete \"\$USER\" && fprintd-enroll \"\$USER\""
    fi
    exit 0
fi

if [ "$1" = "--remove" ]; then
    [ -f "$OVERRIDE_FILE" ] || [ -f "$KEY_DROPIN" ] || [ -f "$ENC_DROPIN" ] || die "no drop-in in $OVERRIDE_DIR"
    if [ -f "$OVERRIDE_FILE" ]; then
        backup_override /dev/null
        rm -f -- "$OVERRIDE_FILE"
        say "removed $OVERRIDE_FILE; fprintd uses the distro libfprint again"
    fi
    for f in "$KEY_DROPIN" "$ENC_DROPIN"; do
        if [ -f "$f" ]; then
            rm -f -- "$f"
            say "removed $f"
        fi
    done
    rmdir --ignore-fail-on-non-empty -- "$OVERRIDE_DIR"
    systemctl daemon-reload
    systemctl try-restart fprintd.service
    [ -f "$CRED_FILE" ] && say "kept the print key credential $CRED_FILE (needed to read encrypted prints)"
    exit 0
fi

if [ "$1" = "--encrypt-prints" ]; then
    command -v systemd-creds >/dev/null \
        || die "systemd-creds not found (needs systemd >= 250); prints stay encrypted with the key file"
    if [ -f "$CRED_FILE" ]; then
        # Never replace an existing key: prints encrypted with it would be lost.
        [ "$(cred_size "$CRED_FILE")" -eq 32 ] \
            || die "$CRED_FILE exists but cannot be decrypted (TPM cleared?); see docs/print-encryption.md, Recovery"
        say "using the existing credential $CRED_FILE"
    else
        WITH=host
        systemd-creds has-tpm2 >/dev/null 2>&1 && WITH=host+tpm2
        mkdir -p "$CRED_DIR"
        chmod 0700 "$CRED_DIR"
        TMP="$(mktemp "$CRED_DIR/.$CRED_NAME.XXXXXX")"
        trap 'rm -f -- "$TMP"' EXIT
        if [ -f "$KEY_FILE" ]; then
            [ "$(stat -c %s "$KEY_FILE")" -eq 32 ] || die "$KEY_FILE is not a 32-byte key"
            systemd-creds encrypt --name="$CRED_NAME" --with-key="$WITH" --tpm2-pcrs= \
                "$KEY_FILE" "$TMP" >/dev/null
            systemd-creds decrypt --name="$CRED_NAME" "$TMP" - | cmp -s - "$KEY_FILE" \
                || die "the new credential does not decrypt to $KEY_FILE; nothing changed"
            say "imported the existing key $KEY_FILE ($WITH)"
        else
            head -c 32 /dev/urandom \
                | systemd-creds encrypt --name="$CRED_NAME" --with-key="$WITH" --tpm2-pcrs= \
                    - "$TMP" >/dev/null
            [ "$(cred_size "$TMP")" -eq 32 ] || die "the new credential cannot be decrypted; nothing changed"
            say "generated a new print key ($WITH)"
        fi
        chmod 0600 "$TMP"
        mv -f -- "$TMP" "$CRED_FILE"
        trap - EXIT
        say "wrote $CRED_FILE"
    fi

    mkdir -p "$OVERRIDE_DIR"
    cat > "$KEY_DROPIN" <<EOF
# Generated by Kensington_VeriMark_06cb-0088/packaging/setup-fprintd.sh --encrypt-prints
# Passes the validity-0088 print encryption key to fprintd.
# Credential: $CRED_FILE (no PCR binding)
# If fprintd fails to start with status 243/CREDENTIALS, the credential can
# no longer be decrypted (e.g. TPM cleared): see docs/print-encryption.md, Recovery.
[Service]
ImportCredential=$CRED_NAME
# systemd decrypts the credential inside fprintd's device policy, which only
# lists the fingerprint devices; the TPM must be allowed too.
DeviceAllow=/dev/tpmrm0 rw
EOF
    chmod 0644 "$KEY_DROPIN"
    systemctl daemon-reload
    systemctl restart fprintd.service \
        || die "fprintd failed to start; see: journalctl -u fprintd -n 30   (undo: rm $KEY_DROPIN)"
    [ -f "/run/credentials/fprintd.service/$CRED_NAME" ] \
        || die "fprintd is running but did not receive the credential; see: journalctl -u fprintd -n 30"
    say "fprintd now gets the print key from $CRED_FILE"
    if [ -f "$KEY_FILE" ]; then
        say "the key file $KEY_FILE is still there: after a successful"
        say "fingerprint verify, remove it with: sudo $0 --drop-file-key"
    fi
    exit 0
fi

if [ "$1" = "--drop-file-key" ]; then
    [ -f "$KEY_FILE" ] || die "no key file at $KEY_FILE"
    [ -f "$CRED_FILE" ] || die "no credential at $CRED_FILE; run --encrypt-prints first"
    systemd-creds decrypt --name="$CRED_NAME" "$CRED_FILE" - | cmp -s - "$KEY_FILE" \
        || die "$CRED_FILE does not hold the same key as $KEY_FILE; keeping the file"
    shred -u -- "$KEY_FILE" 2>/dev/null || rm -f -- "$KEY_FILE"
    say "removed $KEY_FILE; the key is now only in $CRED_FILE"
    exit 0
fi

BUILD_DIR="$1"
[ -d "$BUILD_DIR" ] || die "build dir not found: $BUILD_DIR"

# Resolve to absolute, follow symlinks
BUILD_DIR="$(readlink -f -- "$BUILD_DIR")"
if [ -f "$BUILD_DIR/$LIB_NAME" ]; then
    LIB_DIR="$BUILD_DIR"
else
    LIB_DIR="$BUILD_DIR/libfprint"
fi

# Validate the build actually produced libfprint-2.so with the driver
LIB="$LIB_DIR/$LIB_NAME"
[ -f "$LIB" ] \
    || die "$LIB_NAME not found in $LIB_DIR — did 'meson compile' run?"

# Plain grep on the file: no pipe (a pipe into 'grep -q' fails with SIGPIPE
# under pipefail) and no dependency on binutils. Build-tree libraries are
# not stripped, so the symbol name is present.
grep -qaF fpi_device_validity_0088_get_type "$LIB" \
    || die "the libfprint at $LIB_DIR does not include the validity-0088 driver — \
re-run 'meson setup --wipe builddir -Ddrivers=validity_0088' and rebuild"

NEW_OVERRIDE="$(mktemp)"
trap 'rm -f -- "$NEW_OVERRIDE"' EXIT

cat > "$NEW_OVERRIDE" <<EOF
# Generated by Kensington_VeriMark_06cb-0088/packaging/setup-fprintd.sh
# Points fprintd at the user-built libfprint that contains the validity-0088
# driver. BindReadOnlyPaths bind-mounts the build's lib directory to a path
# outside /home so fprintd's ProtectHome=yes sandbox doesn't hide it.
# Pairing keys and calibration: /var/lib/fprint/validity-0088-pairing/
# After a rebuild: sudo systemctl restart fprintd
# Undo: sudo setup-fprintd.sh --remove
[Unit]
RequiresMountsFor=$LIB_DIR

[Service]
Environment=LD_LIBRARY_PATH=$EXPOSED
BindReadOnlyPaths=$LIB_DIR:$EXPOSED
EOF

backup_override "$NEW_OVERRIDE"
mkdir -p "$OVERRIDE_DIR"
install -m 0644 -- "$NEW_OVERRIDE" "$OVERRIDE_FILE"

systemctl daemon-reload
systemctl restart fprintd.service \
    || die "fprintd failed to start with the new drop-in; see:
  journalctl -u fprintd -n 30   (undo with: $0 --remove)"

# Make sure the service really mapped the library from the build.
PID="$(systemctl show -p MainPID --value fprintd.service)"
if [ "$PID" -gt 0 ] && grep -qF "$EXPOSED/$LIB_NAME" "/proc/$PID/maps"; then
    say "fprintd (pid $PID) is using $LIB"
else
    die "fprintd is running but did not load $EXPOSED/$LIB_NAME; see: journalctl -u fprintd -n 30"
fi

say "wrote $OVERRIDE_FILE"
cat <<EOF
setup-fprintd: next steps
  - Check:   fprintd-list "\$USER"
             expected device: 'Synaptics/Validity (pre-Prometheus)'
  - fprintd keeps its own pairing keys and calibration in
    /var/lib/fprint/validity-0088-pairing/ (separate from the examples'
    ~/.local/share/libfprint/verimark-06cb-0088/). Without a calibration
    there, the next enroll or verify calibrates the sensor first: keep the
    finger off until it asks for a tap.
  - Login with the finger: sudo pam-auth-update --enable fprintd
EOF
