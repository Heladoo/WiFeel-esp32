#!/usr/bin/env bash
# Host-side tests for components/wifeel_csi. No ESP-IDF, no hardware, no board:
# plain gcc against stub ESP headers (test/host/stubs). Seconds to run.
#
#   ./test/host/run.sh              # test the working tree
#   ./test/host/run.sh HEAD~1       # ...and compare against an older revision
#
# These exist because this project otherwise has no build-time tests at all
# (see CLAUDE.md's Verification section) and the CSI feature pipeline is pure
# arithmetic over a timestamped frame sequence -- which is exactly the kind of
# thing that can be checked on a laptop, and which hid a real packet-rate bug
# for weeks precisely because it never was. See docs/boards.md.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

build_and_run() {  # <label> <wifeel_csi.c> <include dir> [extra cflags]
  local label="$1" src="$2" inc="$3"; shift 3
  echo "########## $label ##########"
  for t in rate_independence false_positives; do
    gcc -O1 -Wall -Wextra -o "$work/$t" "$here/$t.c" "$src" \
        -I "$inc" -I "$here/stubs" -I "$root/components/wifeel_proto/include" \
        "$@" -lm
    "$work/$t"
    echo
  done
}

if [ $# -ge 1 ]; then
  rev="$1"
  mkdir -p "$work/old"
  git -C "$root" show "$rev:components/wifeel_csi/wifeel_csi.c" > "$work/old/wifeel_csi.c"
  git -C "$root" show "$rev:components/wifeel_csi/include/wifeel_csi.h" > "$work/old/wifeel_csi.h"
  # The older header predates the sampling-diagnostic accessors, so build it
  # without them rather than patching the revision under test.
  build_and_run "$rev (baseline)" "$work/old/wifeel_csi.c" "$work/old"
fi

build_and_run "working tree" \
  "$root/components/wifeel_csi/wifeel_csi.c" \
  "$root/components/wifeel_csi/include" \
  -DHAVE_SAMPLING_DIAGS
