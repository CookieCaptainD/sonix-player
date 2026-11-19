#include "browserremote_page.h"

#include <ctype.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/lang.h"
#include "src/system/net/browserremote.h"
#include "src/system/net/browserremote_http.h"
#include "src/system/net/wifi.h"

lv_obj_t *browserremote_screen;

#define BR_POLL_MS 1000

static lv_obj_t *toggle;
static lv_obj_t *status_label;
static lv_obj_t *url_label;
static lv_obj_t *pin_row;
static lv_obj_t *wifi_row;
static lv_timer_t *poll_timer;

static lv_obj_t *pin_layer;
static lv_obj_t *pin_field;
static keyboard_t *pin_keyboard;

static bool switch_is_on(void) { return lv_obj_has_state(toggle, LV_STATE_CHECKED); }
static void hide(lv_obj_t *obj) { lv_obj_set_hidden(obj, true); }
static void show(lv_obj_t *obj) { lv_obj_set_hidden(obj, false); }

static void set_text(lv_obj_t *label, const char *text) {
	const char *shown = lv_label_get_text(label);
	if (shown && strcmp(shown, text) == 0) {
		return;
	}
	lv_label_set_text(label, text);
}

static bool wifi_is_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

static bool pin_valid(const char *pin) {
	size_t len = strlen(pin);
	if (len < 4 || len > BR_PIN_MAX) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (!isdigit((unsigned char)pin[i])) {
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// PIN entry: the same full-screen keyboard layer the playlist name uses.
// ---------------------------------------------------------------------------

static void pin_layer_hide(void) {
	hide(pin_layer);
	lv_textarea_set_text(pin_field, "");
	keyboard_set_visible(pin_keyboard, false);
}

static void pin_cancel_cb(lv_event_t *e) {
	(void)e;
	pin_layer_hide();
}

static void pin_accept_cb(lv_event_t *e) {
	(void)e;
	const char *pin = lv_textarea_get_text(pin_field);
	if (!pin_valid(pin)) {
		lv_label_set_text(status_label, tr("browser_remote_pin_invalid"));
		return;
	}
	browserremote_http_set_pin(pin);
	pin_layer_hide();
	gui_notify_popup("browser_remote_pin_saved");
}

static void pin_row_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(pin_field, "");
	keyboard_set_visible(pin_keyboard, true);
	keyboard_reset(pin_keyboard);
	show(pin_layer);
	lv_obj_move_foreground(pin_layer);
}

static void build_pin_layer(gui_config_t *cfg) {
	pin_layer = lv_obj_create(browserremote_screen);
	lv_obj_set_size(pin_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(pin_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(pin_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(pin_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(pin_layer, 0, 0);
	lv_obj_set_style_radius(pin_layer, 0, 0);
	lv_obj_set_style_pad_all(pin_layer, 0, 0);
	lv_obj_set_scrollable(pin_layer, false);
	hide(pin_layer);

	lv_obj_t *heading = lv_label_create(pin_layer);
	lv_label_set_text(heading, tr("browser_remote_set_pin"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding + 56 + 14, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(pin_layer);
	lv_obj_set_size(cancel, 56, 56);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, pin_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	pin_field = lv_textarea_create(pin_layer);
	lv_textarea_set_one_line(pin_field, true);
	lv_textarea_set_max_length(pin_field, BR_PIN_MAX);
	lv_textarea_set_placeholder_text(pin_field, tr("browser_remote_pin_placeholder"));
	lv_obj_set_size(pin_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(pin_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(pin_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 60);
	lv_obj_add_style(pin_field, &theme_style_card, 0);
	lv_obj_set_style_radius(pin_field, 12, 0);
	lv_obj_set_style_border_width(pin_field, 0, 0);
	lv_obj_set_style_shadow_width(pin_field, 0, 0);
	lv_obj_set_style_pad_all(pin_field, 14, 0);
	lv_obj_set_style_text_font(pin_field, &font_ui_24, 0);
	keyboard_style_caret(pin_field);
	keyboard_style_password(pin_field, 0);

	pin_keyboard = keyboard_create(pin_layer, cfg->screen_width, 316, pin_field, NULL, "ok", pin_accept_cb, NULL);
	keyboard_set_visible(pin_keyboard, false);
}

// ---------------------------------------------------------------------------
// Page state
// ---------------------------------------------------------------------------

static void refresh(void) {
	if (!browserremote_http_has_pin()) {
		set_text(status_label, tr("browser_remote_set_pin_first"));
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		hide(url_label);
		show(wifi_row);
		return;
	}
	if (!wifi_is_connected()) {
		set_text(status_label, tr("enable_wi_fi_first"));
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		hide(url_label);
		show(wifi_row);
		return;
	}

	lv_obj_remove_state(toggle, LV_STATE_DISABLED);
	hide(wifi_row);

	if (!switch_is_on()) {
		set_text(status_label, tr("browser_remote_off_note"));
		hide(url_label);
		return;
	}

	char url[128];
	browserremote_http_url(url, sizeof(url));
	if (url[0]) {
		set_text(status_label, tr("browser_remote_on_note"));
		set_text(url_label, url);
		show(url_label);
	} else {
		set_text(status_label, tr("enable_wi_fi_first"));
		hide(url_label);
	}
}

static void toggle_changed_cb(lv_event_t *e) {
	(void)e;
	if (switch_is_on() && !browserremote_http_has_pin()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup("browser_remote_set_pin_first");
		return;
	}
	if (switch_is_on() && !wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup("enable_wi_fi_first");
		return;
	}
	browserremote_http_set_enabled(switch_is_on());
	refresh();
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	browserremote_http_watchdog();

	// The idle watchdog may have taken the server down while the page stayed
	// open; the switch should say what is really running.
	if (switch_is_on() && !browserremote_http_running()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}
	refresh();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// The preference says what the switch was left on. Starting here, rather
	// than at boot, keeps the server tied to an open page.
	if (browserremote_http_get_enabled() && browserremote_http_has_pin() && wifi_is_connected()) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
		browserremote_http_start();
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}
	refresh();
	lv_timer_resume(poll_timer);
	lv_timer_ready(poll_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll_timer);
	// Leaving the page is the explicit stop, whatever route was taken.
	browserremote_http_stop();
	pin_layer_hide();
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

void browserremote_page_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(browserremote_screen, cfg, "browser_remote");

	settingsrow_toggle(container, "browser_remote", &toggle, toggle_changed_cb);
	pin_row = settingsrow_add(container, "browser_remote_set_pin", NULL, pin_row_cb, NULL);
	(void)pin_row;

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(status_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_top(status_label, 14, 0);

	url_label = lv_label_create(container);
	lv_obj_set_width(url_label, lv_pct(100));
	lv_label_set_long_mode(url_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(url_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(url_label, &font_ui_32, 0);
	lv_obj_set_style_text_color(url_label, theme()->accent, 0);
	lv_obj_set_style_text_align(url_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_ver(url_label, 16, 0);
	hide(url_label);

	wifi_row = settingsrow_add(container, "wi_fi_settings", NULL, wifi_row_cb, NULL);
	lv_obj_set_style_margin_top(wifi_row, 16, 0);

	poll_timer = lv_timer_create(poll_cb, BR_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(browserremote_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(browserremote_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	build_pin_layer(cfg);
	refresh();
}
