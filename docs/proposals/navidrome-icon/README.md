# Navidrome tile icon proposal

The Navidrome tile in the Streaming menu reuses the Streaming icon.

This proposal gives it a dedicated tile in the same family as the Music section icons: two gradient rounded squares, a translucent front, a soft shadow, and a white glyph at 60% opacity. The front square sits level, like the Radio tile. The glyph is a vinyl record after Navidrome's mark, on Navidrome blue.

- `navidrome-140.png`: source size, matching the section tiles.
- `navidrome-112.png`: size drawn on the 480x800 panel.
- `before-after.png`: current icon and proposal.
- `streaming-row.png`: the proposal beside the other Streaming tiles.

To ship it, add `navidrome.png` to `sonix-player/assets/icons/`, add `("navidrome.png", "menu_navidrome", SECTION_ICON_SIZE)` to `tools/svg_to_lvgl.py`, and use `&icon_menu_navidrome` in `streaming.c`.
