#!/usr/bin/env bash
# Render captured terminal text to PNG with ImageMagick.
# Usage: ./render_evidence.sh
set -euo pipefail

cd "$(dirname "$0")"

WIDTH=1240
FONT=Adwaita-Mono
POINT=17
LINE=24

render() {
  src="$1"
  dst="$2"
  wrapped="/tmp/opencode/${src%.txt}-wrapped.txt"
  fold -w 108 "$src" > "$wrapped"
  lines=$(wc -l < "$wrapped")
  height=$((lines * LINE + 110))
  magick -size "${WIDTH}x${height}" xc:'#0d1117' \
    -font "$FONT" -pointsize "$POINT" -fill '#c9d1d9' \
    -annotate +40+55 "$(cat "$wrapped")" \
    -bordercolor '#30363d' -border 2 "$dst"
  echo "rendered $dst (${WIDTH}x${height})"
}

render harness-summary.txt harness-summary.png
render release-summary.txt release-summary.png
render review-journey.txt review-journey.png
render host-harness.txt host-harness.png
