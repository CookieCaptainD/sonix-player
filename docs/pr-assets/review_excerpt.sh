#!/usr/bin/env bash
# Capture the seven review rounds from the design doc into a text excerpt.
set -euo pipefail
cd "$(dirname "$0")"
sed -n "/^## Review journey/,\$p" \
  ../superpowers/specs/2026-11-27-avrcp-target-browsing-design.md \
  | sed '$d' > review-journey.txt
echo "captured review-journey.txt ($(wc -l < review-journey.txt) lines)"
