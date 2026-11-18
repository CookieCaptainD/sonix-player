#!/bin/bash
# The packer reassembles the stock kernel to inspect it. With a custom kernel
# in the image that copy must not reach the ISO: nothing references it and it
# costs 3.5 MiB on the SD card.
set -euo pipefail

[ $# -eq 1 ] || { echo "usage: $0 image.upt" >&2; exit 2; }
[ -f "$1" ] || { echo "no such image: $1" >&2; exit 2; }

if 7z l "$1" 2>/dev/null | grep -qi 'xImage\.stock'; then
	echo "FAIL: $1 contains an unreferenced xImage.stock" >&2
	exit 1
fi

echo "ok: $1 has no xImage.stock"
