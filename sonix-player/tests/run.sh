#!/bin/sh
# Builds and runs the browser remote tests. Invoked from the project root as
# `sh tests/run.sh`; it finds the root itself otherwise.
set -e
cd "$(dirname "$0")/.."

# PIN, challenge and browser-bound sessions.
cc -O2 -Wall -Wextra -Isrc -o /tmp/browserremote_pin_test \
   tests/browserremote_pin_test.c src/system/net/browserremote.c
/tmp/browserremote_pin_test

# Reading remote state leaves playback alone; only control commands act.
cc -O2 -Wall -Wextra -Isrc -o /tmp/browserremote_playback_test \
   tests/browserremote_playback_test.c src/system/net/browserremote.c
/tmp/browserremote_playback_test
