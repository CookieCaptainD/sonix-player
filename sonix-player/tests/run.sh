#!/bin/sh
# Builds and runs the decoder tests. Invoked from the project root as
# `sh tests/run.sh`; it finds the root itself otherwise.
set -e
cd "$(dirname "$0")/.."

# Task 1: the DST decoder, standalone, no other sources needed.
cc -O2 -Wall -Wextra -Isrc -o /tmp/dst_decoder_test \
   tests/dst_decoder_test.c src/system/decode/dst_decoder.c
/tmp/dst_decoder_test
