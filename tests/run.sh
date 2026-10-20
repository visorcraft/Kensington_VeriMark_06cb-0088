#!/bin/bash
#
# tests/run.sh — build and run the driver's tests. No device needed.
#
# Usage: tests/run.sh [test-name...]      (default: all)
#
# The tests that use the driver's libfprint-dependent code take their
# compiler flags from build/libfprint/builddir/compile_commands.json, so run
# packaging/build-libfprint.sh first. test-matcher needs only GLib.
#
# SPDX-License-Identifier: LGPL-2.1-or-later

set -uo pipefail

REPO="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
DRV="$REPO/drivers/validity-0088"
T="$REPO/tests"
OUT="$REPO/build/tests"
CC="${CC:-cc}"
mkdir -p "$OUT"
# temporary storage directories of the tests; cleared on every run
export TMPDIR="$OUT/tmp"
chmod -R u+w "$TMPDIR" 2>/dev/null
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"

GLIB="$(pkg-config --cflags --libs glib-2.0 gio-2.0)"
CDB="$REPO/build/libfprint/builddir/compile_commands.json"

# -I/-D/-std flags the driver is compiled with inside libfprint
libfprint_flags () {
    [ -f "$CDB" ] || return 1
    python3 - "$CDB" <<'PYEOF'
import json, os, shlex, sys
cdb = sys.argv[1]
for e in json.load(open(cdb)):
    if e['file'].endswith('/validity-printkey.c'):
        out = []
        for a in shlex.split(e['command']):
            if a.startswith(('-I', '-D', '-std')):
                if a.startswith('-I') and not os.path.isabs(a[2:]):
                    a = '-I' + os.path.normpath(os.path.join(e['directory'], a[2:]))
                out.append(a)
        print(' '.join(out))
        break
PYEOF
}

PASS=0
FAIL=0
SKIP=0
RESULTS=()

report () {   # name status detail
    RESULTS+=("$(printf '%-24s %s %s' "$1" "$2" "$3")")
    case "$2" in
        PASS) PASS=$((PASS + 1)) ;;
        FAIL) FAIL=$((FAIL + 1)) ;;
        SKIP) SKIP=$((SKIP + 1)) ;;
    esac
}

run_binary () {   # name binary [args]
    local name="$1"; shift
    local log="$OUT/$name.log"
    if "$@" >"$log" 2>&1; then
        report "$name" PASS "($(grep -c '^ok' "$log") checks)"
    else
        report "$name" FAIL "(see $log)"
        grep -E "^FAIL" "$log" | sed 's/^/    /'
    fi
}

build () {   # name sources... -- extra flags
    local name="$1"; shift
    local srcs=() flags=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do srcs+=("$1"); shift; done
    [ "${1:-}" = "--" ] && shift
    flags=("$@")
    # word splitting of $GLIB and the pkg-config output is intended
    # shellcheck disable=SC2086,SC2046
    if ! $CC -O2 -g -Wall -Wno-unused-parameter -Wno-deprecated-declarations \
            -I"$T" -I"$DRV" "${flags[@]}" -o "$OUT/$name" "${srcs[@]}" \
            $GLIB $(pkg-config --libs libcrypto) -lm 2>"$OUT/$name.build.log"; then
        report "$name" FAIL "(build failed, see $OUT/$name.build.log)"
        return 1
    fi
}

want () {   # is test $1 selected?
    [ ${#SELECT[@]} -eq 0 ] && return 0
    local s; for s in "${SELECT[@]}"; do [ "$s" = "$1" ] && return 0; done
    return 1
}
SELECT=("$@")

# --- test-matcher: synthetic fingerprints, matcher only -------------------
if want test-matcher; then
    build test-matcher "$T/test-matcher.c" "$T/synth-finger.c" "$DRV/validity-sift.c" \
        && run_binary test-matcher "$OUT/test-matcher"
fi

LF="$(libfprint_flags || true)"
for t in test-print test-capture-builder test-pairing-key; do
    want "$t" || continue
    if [ -z "$LF" ]; then
        report "$t" SKIP "(no libfprint build: run packaging/build-libfprint.sh)"
        continue
    fi
    # shellcheck disable=SC2086
    case "$t" in
        test-print)
            build "$t" "$T/test-print.c" "$T/stubs.c" "$T/synth-finger.c" \
                "$DRV/validity-printkey.c" "$DRV/validity-match.c" "$DRV/validity-sift.c" -- $LF \
                && run_binary "$t" "$OUT/$t" ;;
        test-capture-builder)
            build "$t" "$T/test-capture-builder.c" "$T/stubs.c" \
                "$DRV/validity-capture-builder.c" -- $LF \
                && run_binary "$t" "$OUT/$t" ;;
        test-pairing-key)
            build "$t" "$T/test-pairing-key.c" "$T/stubs.c" "$T/stubs-usb.c" \
                "$DRV/validity-init-clean-slate.c" -- $LF \
                && run_binary "$t" "$OUT/$t" ;;
    esac
done

# --- setup-fprintd.sh in a sandbox -----------------------------------------
if want test-setup-fprintd; then
    run_binary test-setup-fprintd bash "$T/test-setup-fprintd.sh"
    grep -q "^skip" "$OUT/test-setup-fprintd.log" && \
        RESULTS[-1]="$(printf '%-24s %s %s' test-setup-fprintd SKIP "($(grep -m1 '^skip' "$OUT/test-setup-fprintd.log"))")" && \
        PASS=$((PASS - 1)) && SKIP=$((SKIP + 1))
fi

# --- repository rules -------------------------------------------------------
if want check-repo; then
    run_binary check-repo bash "$T/check-repo.sh"
fi

# --- shell scripts -----------------------------------------------------------
if want shellcheck; then
    if command -v shellcheck >/dev/null; then
        if shellcheck -S warning "$REPO"/packaging/*.sh "$T"/*.sh >"$OUT/shellcheck.log" 2>&1; then
            report shellcheck PASS ""
        else
            report shellcheck FAIL "(see $OUT/shellcheck.log)"
        fi
    else
        report shellcheck SKIP "(shellcheck not installed)"
    fi
fi

echo
printf '%s\n' "${RESULTS[@]}"
echo "passed $PASS, failed $FAIL, skipped $SKIP"
[ "$FAIL" -eq 0 ]
