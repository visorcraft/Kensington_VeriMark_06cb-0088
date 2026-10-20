#!/bin/bash
#
# build-libfprint.sh — fetch upstream libfprint, add the validity-0088
# driver from this repository, and build it.
#
# Usage (from anywhere, as a normal user; root works too, e.g. in CI):
#   packaging/build-libfprint.sh [--ref <tag|branch>] [--install-deps]
#                                [--no-update] [--clean]
#
#   --ref <x>       libfprint tag or branch to build (default: the latest
#                   release tag, e.g. v1.94.100; "master" for the newest code)
#   --install-deps  install missing build dependencies with sudo apt/dnf
#   --no-update     build the libfprint checkout as it is (no fetch/checkout)
#   --clean         delete the build directory first (full rebuild)
#
# Everything goes to build/ in this repository (git-ignored):
#   build/libfprint            upstream libfprint checkout + this driver
#   build/libfprint/builddir   meson build directory
#
# The driver files are copied fresh from drivers/validity-0088/ on every
# run, and libfprint's two meson files are reset and patched each time, so
# the script can be re-run after every change. Afterwards:
#   sudo packaging/setup-fprintd.sh build/libfprint/builddir
#
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

die () { echo "build-libfprint: $*" >&2; exit 1; }
say () { echo "build-libfprint: $*"; }

REPO="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
DRIVER="$REPO/drivers/validity-0088"
SRC="$REPO/build/libfprint"
BUILDDIR="$SRC/builddir"
URL=https://gitlab.freedesktop.org/libfprint/libfprint.git

REF=""
INSTALL_DEPS=0
UPDATE=1
CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --ref)          [ $# -ge 2 ] || die "--ref needs a value"; REF="$2"; shift ;;
        --install-deps) INSTALL_DEPS=1 ;;
        --no-update)    UPDATE=0 ;;
        --clean)        CLEAN=1 ;;
        -h|--help)      sed -n '3,26p' "$0"; exit 0 ;;
        *)              die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

# Normally run as a user (sudo only for --install-deps); root is accepted for
# containers and CI, where the build directory then belongs to root.
SUDO=(sudo)
if [ "$(id -u)" -eq 0 ]; then
    SUDO=()
    say "running as root: fine in a container, otherwise prefer a normal user"
fi
[ -d "$DRIVER" ] || die "driver sources not found: $DRIVER"

# ---------------------------------------------------------------------------
# 1. Build dependencies, by distribution family
# ---------------------------------------------------------------------------

FAMILY=unknown
if [ -r /etc/os-release ]; then
    . /etc/os-release
    case " ${ID:-} ${ID_LIKE:-} " in
        *" debian "*|*" ubuntu "*)                     FAMILY=debian ;;
        *" rhel "*|*" fedora "*|*" centos "*)           FAMILY=rhel ;;
    esac
fi

# check -> package, per family. Checks: "cmd:<tool>" or "pc:<pkg-config module>".
declare -A DEB=(
    [cmd:git]=git [cmd:meson]=meson [cmd:ninja]=ninja-build [cmd:cc]=build-essential
    [cmd:c++]=build-essential
    [cmd:pkg-config]=pkg-config [cmd:python3]=python3 [cmd:g-ir-scanner]=gobject-introspection
    [pc:glib-2.0]=libglib2.0-dev [pc:gio-unix-2.0]=libglib2.0-dev
    [pc:gobject-2.0]=libglib2.0-dev [pc:gmodule-2.0]=libglib2.0-dev
    [pc:gusb]=libgusb-dev [pc:openssl]=libssl-dev
    [pc:gobject-introspection-1.0]=libgirepository1.0-dev
)
declare -A RPM=(
    [cmd:git]=git [cmd:meson]=meson [cmd:ninja]=ninja-build [cmd:cc]=gcc
    [cmd:c++]=gcc-c++
    [cmd:pkg-config]=pkgconf-pkg-config [cmd:python3]=python3
    [cmd:g-ir-scanner]=gobject-introspection-devel
    [pc:glib-2.0]=glib2-devel [pc:gio-unix-2.0]=glib2-devel
    [pc:gobject-2.0]=glib2-devel [pc:gmodule-2.0]=glib2-devel
    [pc:gusb]=libgusb-devel [pc:openssl]=openssl-devel
    [pc:gobject-introspection-1.0]=gobject-introspection-devel
)
# libfprint's meson project also enables C++ (for an example), so a C++
# compiler is needed even though the driver is C.
CHECKS=(cmd:git cmd:meson cmd:ninja cmd:cc cmd:c++ cmd:pkg-config cmd:python3 cmd:g-ir-scanner
        pc:glib-2.0 pc:gio-unix-2.0 pc:gobject-2.0 pc:gmodule-2.0 pc:gusb pc:openssl
        pc:gobject-introspection-1.0)

missing_checks () {
    local c
    for c in "${CHECKS[@]}"; do
        case "$c" in
            cmd:*) command -v "${c#cmd:}" >/dev/null 2>&1 || echo "$c" ;;
            pc:*)  command -v pkg-config >/dev/null 2>&1 \
                       && pkg-config --exists "${c#pc:}" || echo "$c" ;;
        esac
    done
}

mapfile -t MISSING < <(missing_checks)
if [ ${#MISSING[@]} -gt 0 ]; then
    say "missing: ${MISSING[*]}"
    PKGS=()
    for c in "${MISSING[@]}"; do
        case "$FAMILY" in
            debian) p="${DEB[$c]}" ;;
            rhel)   p="${RPM[$c]}" ;;
            *)      p="" ;;
        esac
        [ -n "$p" ] && [[ " ${PKGS[*]:-} " != *" $p "* ]] && PKGS+=("$p")
    done
    case "$FAMILY" in
        debian) CMD=("${SUDO[@]}" apt-get install -y "${PKGS[@]}") ;;
        rhel)   CMD=("${SUDO[@]}" dnf install -y "${PKGS[@]}") ;;
        *)      die "unknown distribution (${ID:-?}): install the tools and pkg-config modules above yourself" ;;
    esac
    if [ "$INSTALL_DEPS" -eq 1 ]; then
        say "installing: ${PKGS[*]}"
        [ "$FAMILY" = debian ] && "${SUDO[@]}" apt-get update -q
        "${CMD[@]}"
        mapfile -t MISSING < <(missing_checks)
        [ ${#MISSING[@]} -eq 0 ] || die "still missing after install: ${MISSING[*]}"
    else
        say "install them with:"
        say "  ${CMD[*]}"
        [ "$FAMILY" = rhel ] && say "  (RHEL/Alma/Rocky: libgusb-devel and meson need the CRB repository, e.g. sudo dnf config-manager --set-enabled crb)"
        die "or re-run with --install-deps"
    fi
fi

pkg-config --atleast-version=3.0 openssl || die "OpenSSL >= 3.0 is required (found $(pkg-config --modversion openssl))"
say "dependencies OK ($FAMILY)"

# ---------------------------------------------------------------------------
# 2. libfprint sources
# ---------------------------------------------------------------------------

if [ -z "$REF" ] && [ "$UPDATE" -eq 1 ]; then
    REF="$(git ls-remote --tags --refs "$URL" 'v*' | sed 's|.*refs/tags/||' \
           | grep -E '^v[0-9]+(\.[0-9]+)*$' | sort -V | tail -1)"
    [ -n "$REF" ] || die "cannot determine the latest libfprint release from $URL"
fi

mkdir -p "$REPO/build"
if [ ! -d "$SRC/.git" ]; then
    [ "$UPDATE" -eq 1 ] || die "no checkout at $SRC (run without --no-update)"
    say "cloning libfprint ($REF)"
    git clone -q "$URL" "$SRC"
fi

if [ "$UPDATE" -eq 1 ]; then
    # Undo the previous run's changes before switching versions.
    git -C "$SRC" checkout -q -- meson.build libfprint/meson.build
    rm -rf "$SRC/libfprint/drivers/validity-0088"
    git -C "$SRC" fetch -q --tags origin
    if git -C "$SRC" rev-parse -q --verify "refs/tags/$REF^{commit}" >/dev/null; then
        TARGET="refs/tags/$REF"
    elif git -C "$SRC" rev-parse -q --verify "refs/remotes/origin/$REF" >/dev/null; then
        TARGET="origin/$REF"
    else
        die "libfprint has no tag or branch '$REF'"
    fi
    git -C "$SRC" checkout -q --detach "$TARGET"
fi
DESC="$(git -C "$SRC" describe --tags --always)"
say "libfprint $DESC"

# meson version required by this libfprint
NEED_MESON="$(sed -n "s/.*meson_version: *'>= *\([0-9.]*\)'.*/\1/p" "$SRC/meson.build" | head -1)"
if [ -n "$NEED_MESON" ] && [ "$(printf '%s\n%s\n' "$NEED_MESON" "$(meson --version)" | sort -V | head -1)" != "$NEED_MESON" ]; then
    die "libfprint $DESC needs meson >= $NEED_MESON, found $(meson --version) (pip install --user meson, or use --ref with an older release)"
fi

# ---------------------------------------------------------------------------
# 3. Driver sources and meson registration
# ---------------------------------------------------------------------------

git -C "$SRC" checkout -q -- meson.build libfprint/meson.build
rm -rf "$SRC/libfprint/drivers/validity-0088"
mkdir -p "$SRC/libfprint/drivers/validity-0088"
cp -p "$DRIVER"/*.c "$DRIVER"/*.h "$SRC/libfprint/drivers/validity-0088/"

# GObject introspection < 1.70 does not know the (getter) annotation that
# libfprint uses, and libfprint makes introspection warnings fatal (seen on
# Rocky/RHEL 9 with 1.68). There, make them non-fatal again.
GI_OLD=0
if [ "$(printf '%s\n%s\n' 1.70 "$(pkg-config --modversion gobject-introspection-1.0)" | sort -V | head -1)" != 1.70 ]; then
    GI_OLD=1
fi

python3 - "$SRC" "$GI_OLD" <<'PYEOF'
import glob, os, re, sys
src = sys.argv[1]
gi_old = sys.argv[2] == '1'

files = sorted(os.path.basename(f) for f in glob.glob(
    os.path.join(src, 'libfprint/drivers/validity-0088/*.c')))
inner = os.path.join(src, 'libfprint/meson.build')
s = open(inner).read()
m = re.search(r'^driver_sources = \{\n', s, re.M)
if not m:
    sys.exit('libfprint/meson.build: driver_sources not found')
entry = "    'validity_0088' : files(\n" + ''.join(
    "        'drivers/validity-0088/%s',\n" % f for f in files) + "    ),\n"
s = s[:m.end()] + entry + s[m.end():]
if gi_old and 'fatal_warnings: true' in s:
    s = s.replace('fatal_warnings: true', 'fatal_warnings: false')
    print('build-libfprint: old gobject-introspection: introspection warnings made non-fatal')
open(inner, 'w').write(s)

outer = os.path.join(src, 'meson.build')
s = open(outer).read()
m = re.search(r'^drivers_info = \{\n', s, re.M)
if m:      # libfprint >= 1.94.100
    entry = "    'validity_0088' : { 'helper': ['openssl'] },\n"
else:      # older releases
    m = re.search(r'^driver_helper_mapping = \{\n', s, re.M)
    if not m:
        sys.exit('meson.build: neither drivers_info nor driver_helper_mapping found')
    entry = "    'validity_0088' : [ 'openssl' ],\n"
open(outer, 'w').write(s[:m.end()] + entry + s[m.end():])
print('build-libfprint: registered %d driver sources' % len(files))
PYEOF

# ---------------------------------------------------------------------------
# 4. Configure and build
# ---------------------------------------------------------------------------

STAMP="$BUILDDIR/.validity-0088-libfprint"
if [ "$CLEAN" -eq 1 ] || { [ -d "$BUILDDIR" ] && [ "$(cat "$STAMP" 2>/dev/null)" != "$DESC" ]; }; then
    rm -rf "$BUILDDIR"
fi
if [ ! -f "$BUILDDIR/build.ninja" ]; then
    # Introspection stays on: some libfprint versions fail to configure
    # without it (tests/meson.build).
    meson setup "$BUILDDIR" "$SRC" \
        -Ddrivers=validity_0088 -Ddoc=false -Dgtk-examples=false \
        -Dinstalled-tests=false -Dudev_rules=disabled -Dudev_hwdb=disabled \
        >"$REPO/build/meson-setup.log" 2>&1 \
        || { tail -20 "$REPO/build/meson-setup.log"; die "meson setup failed (log: build/meson-setup.log)"; }
    echo "$DESC" > "$STAMP"
fi
meson compile -C "$BUILDDIR" >"$REPO/build/meson-compile.log" 2>&1 \
    || { grep -E "error|Error" "$REPO/build/meson-compile.log" | head -20; die "build failed (log: build/meson-compile.log)"; }

LIB="$BUILDDIR/libfprint/libfprint-2.so.2.0.0"
grep -qaF fpi_device_validity_0088_get_type "$LIB" || die "built library lacks the validity-0088 driver"
say "built $LIB (libfprint $DESC with validity-0088)"
if grep -q "drivers_validity-0088_" "$REPO/build/meson-compile.log"; then
    WARN="$(grep -c "validity-0088/.*warning:" "$REPO/build/meson-compile.log" || true)"
    if [ "$WARN" -gt 0 ]; then
        say "$WARN compiler warning(s) in the driver; see build/meson-compile.log"
    else
        say "driver compiled without warnings"
    fi
else
    say "driver unchanged since the last build"
fi
cat <<EOF
build-libfprint: next steps
  - Test without installing:
      LD_LIBRARY_PATH=$BUILDDIR/libfprint $BUILDDIR/examples/enroll
  - Use it with fprintd:
      sudo $REPO/packaging/setup-fprintd.sh $BUILDDIR
EOF
