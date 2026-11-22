#ifndef GUI_BROWSERREMOTE_PAGE_H
#define GUI_BROWSERREMOTE_PAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Wireless tile that turns the browser remote on and sets its PIN. The
// server lives only while this page is open: it starts on load and stops on
// unload, the same rule Wi-Fi transfer follows.

extern lv_obj_t *browserremote_screen;

void browserremote_page_init(gui_config_t *cfg);

#endif /* GUI_BROWSERREMOTE_PAGE_H */
