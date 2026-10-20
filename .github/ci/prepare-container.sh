#!/bin/bash
#
# prepare-container.sh — make a fresh distribution container ready for
# packaging/build-libfprint.sh --install-deps (run as root). Used by the CI
# workflow and for running the same jobs locally:
#   podman run --rm -v "$PWD":/src -w /src fedora:42 \
#     bash -c '.github/ci/prepare-container.sh && packaging/build-libfprint.sh --install-deps && tests/run.sh'
#
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail
. /etc/os-release
case " ${ID:-} ${ID_LIKE:-} " in
    *" debian "*|*" ubuntu "*)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -q
        apt-get install -y -q ca-certificates git python3 ;;
    *" rhel "*|*" fedora "*|*" centos "*)
        # Rocky mirrors can briefly lag behind each other (seen: openssl-devel
        # newer than the openssl-libs a mirror offered): bring the base system
        # up to date and let dnf fall back to versions that fit.
        echo "best=False" >> /etc/dnf/dnf.conf
        dnf upgrade -y -q --refresh || true
        dnf install -y -q git python3 diffutils findutils which
        if [ "${ID:-}" != fedora ]; then
            # libgusb-devel and meson come from CRB on RHEL 9 clones
            dnf install -y -q dnf-plugins-core
            dnf config-manager --set-enabled crb
        fi ;;
    *)
        echo "prepare-container: unsupported distribution ${ID:-?}" >&2
        exit 1 ;;
esac
# the checkout may belong to another user than the container's root
git config --global --add safe.directory '*'
