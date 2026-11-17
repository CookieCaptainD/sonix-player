#!/bin/sh
#
# Run a development build of sonix_player over ADB, without flashing the
# device and without writing to the read-only root filesystem.
#
# Requested by @sandreas in Jepl4r/sonix-player#147. The launcher kills the
# player process and reboots one second later, so this script stops the
# launcher first, runs the binary from /tmp, and reboots after it exits. A bad
# binary gives one failed run, then a normal boot. Recovery needs no PC.
#
# usage:
#   sonix_devrun.sh --check BIN      validate BIN only, change nothing
#   sonix_devrun.sh BIN              stop the player, run BIN, then reboot
#
# The binary must be a 32-bit little-endian MIPS executable, 1 MiB to 32 MiB.
# That is the same header check sonix_firmware_packer.sh applies to
# sonix_launch, and it keeps an x86 build, a stub or a truncated copy from
# starting.
#
set -eu

fail() {
	echo "sonix_devrun: $*" >&2
	exit 1
}

CHECK=0
if [ "${1:-}" = "--check" ]; then
	CHECK=1
	shift
fi

BIN="${1:-}"
[ -n "$BIN" ] || fail "usage: sonix_devrun.sh [--check] BIN"
[ -f "$BIN" ] || fail "no file at $BIN"

if stat -c%s . >/dev/null 2>&1; then
	SIZE="$(stat -c%s "$BIN")"
else
	SIZE="$(stat -f%z "$BIN")"
fi
[ "$SIZE" -ge 1048576 ] || fail "$SIZE bytes is too small; the minimum is 1 MiB"
[ "$SIZE" -le 33554432 ] || fail "$SIZE bytes is too large; the limit is 32 MiB"

HEAD="$(od -An -tx1 -N20 "$BIN" | tr -d ' \n')"
[ "${HEAD:0:12}" = "7f454c460101" ] || fail "not a 32-bit little-endian ELF"
[ "${HEAD:32:8}" = "02000800" ] || fail "not an ET_EXEC MIPS executable"

if [ "$CHECK" = 1 ]; then
	echo "OK $BIN ($SIZE bytes, ELF32 MIPS)"
	exit 0
fi

# Stop the launcher before the player. Killing the player alone leaves
# sonix_launch waiting to reboot, which would cut the dev run short.
killall sonix_launch 2>/dev/null || true
killall sonix_player 2>/dev/null || true
sleep 1

# Copy through a temporary name so a pulled card or a full /tmp cannot leave
# half a binary at the path we execute.
cp "$BIN" /tmp/.sonix_dev_player.$$
mv -f /tmp/.sonix_dev_player.$$ /tmp/sonix_dev_player
chmod 755 /tmp/sonix_dev_player

echo "sonix_devrun: running /tmp/sonix_dev_player"
set +e
/tmp/sonix_dev_player
STATUS=$?
set -e
echo "sonix_devrun: dev player exited with $STATUS; rebooting into the installed build"

sync
reboot
