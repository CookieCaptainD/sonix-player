#!/usr/bin/env bash
# Compile and run the module harnesses that are present under
# tools/bluez-browse/tests, and capture their real output.
# This script only writes under docs/pr-assets and /tmp/opencode/bb-build.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TESTS="$ROOT/tools/bluez-browse/tests"
MODULES="$ROOT/tools/bluez-browse/modules"
BUILD=/tmp/opencode/bb-build
OUT="$ROOT/docs/pr-assets/host-harness.txt"

mkdir -p "$BUILD"
GLIB=$(pkg-config --cflags --libs glib-2.0)
DBUS=$(pkg-config --cflags --libs dbus-1)

build() {
  name=$1
  shift
  gcc -std=c11 -I "$TESTS" -I "$MODULES" \
    "$TESTS/test_$name.c" "$@" -o "$BUILD/test_$name" $GLIB $DBUS
}

build avctp_defer "$MODULES/avctp-defer.c"
build change_count "$MODULES/avrcp-tg-browse.c"
build display_name "$MODULES/avrcp-tg-browse.c"
build item_type "$MODULES/avrcp-tg-browse.c"
build parent "$MODULES/media-browse.c" "$MODULES/avrcp-tg-browse.c"
build tg_pending "$MODULES/tg-pending.c"

# The fallback harness compiles the shipped module, not a copy of its logic.
gcc -std=gnu11 -Wall -Wextra -I "$ROOT/sonix-player" \
  "$ROOT/sonix-player/tests/harness/harness.c" \
  "$ROOT/sonix-player/tests/harness/fallback.c" \
  -o "$BUILD/test_fallback"

# The app harness compiles the shipped btbrowser.c against a dbuslite stub.
gcc -std=gnu11 -Wall -Wextra -Werror -pthread -I "$ROOT/sonix-player" \
  "$ROOT/sonix-player/tests/btbrowser_test.c" \
  "$ROOT/sonix-player/tests/fake_provider.c" \
  "$ROOT/sonix-player/tests/support/dbuslite_stub.c" \
  "$ROOT/sonix-player/src/system/bluetooth/btbrowser.c" \
  -o "$BUILD/test_btbrowser"

{
  echo "AVRCP TARGET BROWSING - MODULE HARNESS OUTPUT"
  echo "captured : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "source   : tools/bluez-browse/tests and sonix-player/tests/harness"
  echo "build    : gcc, glib-2.0 and dbus-1 flags, modules compiled as shipped"
  echo ""
  for name in avctp_defer change_count display_name item_type parent tg_pending; do
    echo "\$ test_$name"
    "$BUILD/test_$name"
    echo ""
  done
  echo "\$ test_fallback"
  "$BUILD/test_fallback"
  echo ""
  echo "\$ test_btbrowser"
  "$BUILD/test_btbrowser"
  echo ""
} > "$OUT"

echo "captured $OUT"
