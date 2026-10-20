#!/bin/bash
#
# check-repo.sh — repository consistency rules (see CLAUDE.md):
#   - every driver .c file listed exactly once in meson-fragment.txt,
#   - an SPDX licence line in every source and script,
#   - no device-specific or biometric data tracked,
#   - all relative links in the Markdown documentation resolve.
#
# SPDX-License-Identifier: LGPL-2.1-or-later

set -u
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

FAILS=0
ok ()   { echo "ok   $*"; }
fail () { echo "FAIL $*"; FAILS=$((FAILS + 1)); }

# 1. driver sources registered exactly once
bad=""
for f in drivers/validity-0088/*.c; do
    b="$(basename "$f")"
    n="$(grep -c "validity-0088/$b'" drivers/validity-0088/meson-fragment.txt)"
    [ "$n" -eq 1 ] || bad="$bad $b:$n"
done
[ -z "$bad" ] && ok "every driver source listed once in meson-fragment.txt" \
              || fail "driver sources not listed exactly once:$bad"

# 2. SPDX lines
missing="$(git ls-files '*.c' '*.h' '*.sh' | while read -r f; do
               grep -q "SPDX-License-Identifier: LGPL-2.1-or-later" "$f" || echo "$f"; done)"
[ -z "$missing" ] && ok "SPDX licence line in every source and script" \
                  || fail "no SPDX line: $missing"

# 3. no device or biometric data
private="$(git ls-files | grep -iE '\.(pgm|bin|pcap|pcapng|variant|pem|cred|raw)$|(^|/)(calibration|print-key|test-storage)' || true)"
[ -z "$private" ] && ok "no calibration, keys, prints, scans or captures tracked" \
                  || fail "private data tracked: $private"

# 4. documentation links
broken=""
for f in $(git ls-files '*.md'); do
    d="$(dirname "$f")"
    while read -r l; do
        [ -n "$l" ] && [ ! -e "$d/$l" ] && broken="$broken $f->$l"
    done < <(grep -oE '\]\([^)#]+\)' "$f" | sed 's/^](//; s/)$//' | grep -v '^[a-z]*:')
done
[ -z "$broken" ] && ok "all relative links in the documentation resolve" \
                 || fail "broken links:$broken"

echo "$([ $FAILS -eq 0 ] && echo passed || echo FAILED): $FAILS failure(s)"
[ $FAILS -eq 0 ]
