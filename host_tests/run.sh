#!/bin/sh
# Builds and runs the host-based unit tests (Phase 9) with any C99 compiler --
# no ESP-IDF toolchain needed. These only exercise code that has zero
# hardware dependency (bot.c, pentax_scsi.c's pure helpers, pentax_status.c);
# everything that actually touches USB/SCSI is driven through a scripted mock
# instead of real hardware.
#
# Usage: sh host_tests/run.sh   (from the repo root, or from host_tests/)
set -e

CC="${CC:-cc}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
MAIN="$ROOT/main"
OUT="$HERE/.out"
mkdir -p "$OUT"

CFLAGS="-std=c99 -Wall -Wextra -g -O1"

echo "== pentax_status =="
"$CC" $CFLAGS -I"$HERE" -I"$MAIN" \
    "$HERE/test_pentax_status.c" "$MAIN/pentax_status.c" \
    -o "$OUT/test_pentax_status"
"$OUT/test_pentax_status"

echo
echo "== bot (BOT transport) =="
"$CC" $CFLAGS -I"$HERE" -I"$HERE/stubs" -I"$MAIN" \
    "$HERE/test_bot.c" "$MAIN/bot.c" \
    -o "$OUT/test_bot"
"$OUT/test_bot"

echo
echo "== pentax_scsi (incl. pentax_scsi_exec integration via bot.c) =="
"$CC" $CFLAGS -I"$HERE" -I"$HERE/stubs" -I"$MAIN" \
    "$HERE/test_pentax_scsi.c" "$HERE/fake_clock.c" "$MAIN/pentax_scsi.c" "$MAIN/bot.c" \
    -o "$OUT/test_pentax_scsi"
"$OUT/test_pentax_scsi"

echo
echo "All host test suites passed."
