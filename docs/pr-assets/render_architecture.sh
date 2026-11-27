#!/usr/bin/env bash
# Draw the architecture PNG with ImageMagick.
# graphviz dot is not installed on this host, so the PNG is drawn from the same
# nodes and edges recorded in architecture.dot. The DOT source is kept as the
# canonical graph description.
set -euo pipefail

cd "$(dirname "$0")"

OUT=architecture.png
W=1400
H=860
FONT=DejaVu-Sans

# box x1 y1 x2 y2 title line1 line2
box() {
  shape_args+=("roundrectangle $1,$2 $3,$4 12,12")
  cx=$(( ($1 + $3) / 2 ))
  if [ -n "${7:-}" ]; then
    ty=$(( $2 + 22 ))
    step=17
  elif [ -n "${6:-}" ]; then
    ty=$(( $2 + 25 ))
    step=17
  else
    ty=$(( $2 + 30 ))
    step=17
  fi
  text_args+=("text $cx,$ty '$5'")
  if [ -n "${6:-}" ]; then
    text_args+=("text $cx,$((ty + step)) '$6'")
  fi
  if [ -n "${7:-}" ]; then
    text_args+=("text $cx,$((ty + 2 * step)) '$7'")
  fi
}

# arrow x1 y1 x2 y2
arrow() {
  shape_args+=("line $1,$2 $3,$4")
  if [ "$3" -gt "$1" ]; then
    shape_args+=("polygon $3,$4 $(( $3 - 10 )),$(( $4 - 5 )) $(( $3 - 10 )),$(( $4 + 5 ))")
  elif [ "$3" -lt "$1" ]; then
    shape_args+=("polygon $3,$4 $(( $3 + 10 )),$(( $4 - 5 )) $(( $3 + 10 )),$(( $4 + 5 ))")
  elif [ "$4" -gt "$2" ]; then
    shape_args+=("polygon $3,$4 $(( $3 - 5 )),$(( $4 - 10 )) $(( $3 + 5 )),$(( $4 - 10 ))")
  else
    shape_args+=("polygon $3,$4 $(( $3 - 5 )),$(( $4 + 10 )) $(( $3 + 5 )),$(( $4 + 10 ))")
  fi
}

shape_args=()
text_args=()

# Flow across the top row.
arrow 250 150 360 150
arrow 600 150 710 150
arrow 960 150 1070 150

# Library feeds the tree from below.
arrow 835 330 1070 170
# Tree answers back to the bridge, drawn on a lower line.
arrow 1070 205 960 205

# PlayItem path.
arrow 835 300 835 430
arrow 960 480 1070 480

# Unchanged v1.3.3 paths at the bottom.
arrow 300 660 150 180
arrow 620 660 150 180

# Nodes.
box 60 105 250 195 'Car head unit' 'AVRCP controller' ''
box 360 105 600 195 'Patched bluetoothd' 'AVRCP browse PDUs' ''
box 710 105 960 195 'D-Bus browser bridge' 'serials, deadlines' 'cancel on disconnect'
box 1070 105 1310 195 'Browse tree' 'stable 64-bit UIDs' ''
box 710 290 960 335 'Scanned library' 'folders and tracks' ''
box 710 430 960 505 'Playback bridge' 'UID to file' ''
box 1070 430 1310 505 'Existing playback' 'queue, output' ''
box 60 640 300 685 'v1.3.3 MPRIS player' 'metadata, keys' ''
box 360 640 620 685 'Stock bluetoothd' 'flag off' ''

draw_shapes=()
for arg in "${shape_args[@]}"; do
  draw_shapes+=(-draw "$arg")
done
draw_text=()
for arg in "${text_args[@]}"; do
  draw_text+=(-draw "$arg")
done

magick -size "${W}x${H}" xc:'#f7f9fc' \
  -font "$FONT" -pointsize 14 -fill '#eef3fa' -stroke '#33527a' -strokewidth 2 \
  "${draw_shapes[@]}" \
  -stroke none -fill '#101820' -pointsize 11 \
  "${draw_text[@]}" \
  -draw "text 60,50 'AVRCP target browsing, drawn from architecture.dot'" \
  -draw "text 60,74 'graphviz dot unavailable on this host; ImageMagick rendered the same nodes and edges'" \
  -draw "text 60,780 'Solid arrows: browse request and answer. Lower arrows: unchanged MPRIS path and stock fallback.'" \
  -bordercolor '#c9d2df' -border 2 "$OUT"

identify "$OUT"
