#!/bin/sh
# Builds and runs the decoder tests. Invoked from the project root as
# `sh tests/run.sh`; it finds the root itself otherwise.
set -e
cd "$(dirname "$0")/.."

# Task 1: the DST decoder, standalone, no other sources needed.
cc -O2 -Wall -Wextra -Isrc -o /tmp/dst_decoder_test \
   tests/dst_decoder_test.c src/system/decode/dst_decoder.c
/tmp/dst_decoder_test

# Task 2: the SACD ISO reader, against synthetic images.
ISO_DIR=/tmp/sacd-tests
rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR"
python3 tests/make_sacd_iso.py "$ISO_DIR"
cc -O2 -Wall -Wextra -Isrc -o /tmp/sacd_iso_test \
   tests/sacd_iso_test.c src/system/decode/sacd_iso.c \
   src/system/decode/dst_decoder.c
for mode in dsd14 dsd16 dst bad; do
	/tmp/sacd_iso_test "$ISO_DIR/$mode.iso" "$mode"
done
