#!/bin/bash
#
# run-local.sh — run the CI jobs (.github/workflows/ci.yml) locally in
# containers, before pushing. Needs podman (or docker) and network access.
#
# Usage: .github/ci/run-local.sh [image...]
#        .github/ci/run-local.sh clang
#        (default: the images of ci.yml, plus the Ubuntu clang job; names
#        without a "/" are Docker Hub official images, e.g. fedora:42,
#        others e.g. rockylinux/rockylinux:10)
#
# Each image gets a fresh copy of the repository's committed state plus your
# uncommitted changes (without build/), so your own build/ is not touched.
# Logs and copies go to a temporary directory, printed at the end.
#
# SPDX-License-Identifier: LGPL-2.1-or-later

set -uo pipefail

REPO="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
ENGINE="$(command -v podman || command -v docker || true)"
[ -n "$ENGINE" ] || { echo "run-local: podman or docker is needed" >&2; exit 1; }

WITH_CLANG=0
IMAGES=()
if [ $# -eq 0 ]; then
    IMAGES=(ubuntu:24.04 debian:12 debian:13 fedora:42
            rockylinux/rockylinux:9 rockylinux/rockylinux:10)
    WITH_CLANG=1
else
    for arg in "$@"; do
        if [ "$arg" = clang ]; then
            WITH_CLANG=1
        else
            IMAGES+=("$arg")
        fi
    done
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/verimark-ci-XXXXXX")"
RESULTS=()
FAILED=0

# --- lint: shellcheck in a container, repository rules on the host ------------
if "$ENGINE" run --rm -v "$REPO":/mnt:ro,Z -w /mnt docker.io/koalaman/shellcheck-alpine:stable \
        sh -c 'shellcheck -S warning packaging/*.sh tests/*.sh .github/ci/*.sh' >"$WORK/lint.log" 2>&1 &&
   "$REPO/tests/check-repo.sh" >>"$WORK/lint.log" 2>&1; then
    RESULTS+=("lint                       PASS")
else
    RESULTS+=("lint                       FAIL  ($WORK/lint.log)")
    FAILED=1
fi

# --- build and test per distribution ----------------------------------------
# $1 image, $2 cc (gcc or clang). Clang compiles the driver (C); C++ stays g++.
run_job () {
    local img="$1"
    local cc="$2"
    local name ref src start status setup
    name="$(echo "$img" | tr ':./' '---')"
    setup=""
    if [ "$cc" = clang ]; then
        name="${name}-clang"
        setup="export DEBIAN_FRONTEND=noninteractive; apt-get install -y -q clang; export CC=clang CXX=g++;"
    fi
    case "$img" in
        */*) ref="docker.io/$img" ;;
        *)   ref="docker.io/library/$img" ;;
    esac
    src="$WORK/$name"
    mkdir -p "$src"
    rsync -a --exclude /build/ "$REPO"/ "$src"/
    start=$(date +%s)
    if "$ENGINE" run --rm -v "$src":/src:Z -w /src "$ref" bash -c "
            set -e
            .github/ci/prepare-container.sh
            ${setup}
            packaging/build-libfprint.sh --install-deps
            if grep \"validity-0088/.*warning:\" build/meson-compile.log; then
                echo \"the driver compiles with warnings\"; exit 1
            fi
            tests/run.sh" >"$WORK/$name.log" 2>&1; then
        status=PASS
    else
        status="FAIL  ($WORK/$name.log)"
        FAILED=1
    fi
    if [ "$cc" = clang ]; then
        img="$img clang"
    fi
    RESULTS+=("$(printf '%-26s %s  %ss' "$img" "$status" "$(( $(date +%s) - start ))")")
    echo "run-local: $img done"
}

for img in "${IMAGES[@]}"; do
    run_job "$img" gcc
done
if [ "$WITH_CLANG" -eq 1 ]; then
    run_job ubuntu:24.04 clang
fi

echo
printf '%s\n' "${RESULTS[@]}"
echo "logs: $WORK"
exit "$FAILED"
