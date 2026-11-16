#include "usbaudio.h"
#include "usbport.h"

#include "src/system/audio/audio.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/swvolume.h"
#include "src/system/core/config.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/device/usb.h"

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TYPEC_PORT_TYPE "/sys/class/typec/port0/port_type"
#define TYPEC_PARTNER "/sys/class/typec/port0-partner"
#define TYPEC_POWER_ROLE "/sys/class/typec/port0/power_role"
#define TYPEC_DATA_ROLE "/sys/class/typec/port0/data_role"
#define SOUND_CLASS "/sys/class/sound"

// The charger's 5 V boost is what actually puts VBUS on the port when this
// player is the source. The MP2731 is always powered, so anything left here
// survives a reboot; the supervisor writes it only when the level changes.
#define MP2731_OTG "/sys/class/power_supply/mp2731-charger/usb_otg"

// The R1 has no Type-C class. Its CC controller is a TCS1421 set by two GPIOs,
// and the platform driver takes the mode by name through one attribute and
// reads the current one back: "Sink", "Source", "StrongDRP" or "NormalDRP".
// NormalDRP is what the driver starts in, and what the stock player uses as
// long as its "USB working mode" is left alone; it writes Sink for DAC mode.
#define TCS1421_CFG "/sys/devices/platform/tcs1421/tcs1421_cfg"
#define TCS1421_DUAL "NormalDRP"
#define TCS1421_SINK "Sink"

// And no partner node either. What the R1 does have is the ID line the
// controller hands to the SoC's OTG block: 0 while this player is the host end
// of the cable. The stock player reads the same node.
#define DWC2_OTG_ID "/sys/devices/platform/jz-dwc2/dwc2/otg_id"

// How long something may sit on the port without turning into a sound card
// before it is taken for a host. Enumerating a USB audio device takes well
// under a second; three is generous and still short enough that a phone
// connects a beat late rather than not at all.
#define HANDOVER_SECS 3

// And how long the port must stay empty before dual role is offered again.
// Changing port_type while something is attached makes the kernel drop the
// connection and negotiate afresh, so the partner disappears for a moment after
// each write; without this delay the two states chase each other.
#define RELEASE_SECS 3

// On the R1, how long after writing Sink the ID line is given to follow before
// it is believed. A host end reading after that is an ID line that does not
// mean what this file thinks it means.
#define ID_SETTLE_SECS 3

static int active_card = -1;			// the USB card in use, -1 when none
static char active_name[64];			// its id, for the interface
static char volume_control[64];			// the control the level is written to
static long volume_min, volume_max;
static unsigned int volume_count = 1; // how many channels that control carries
// The control's dB scale, read from its TLV; volume_has_db is false when the
// device publishes none.
static unsigned int volume_tlv[64];
static bool volume_has_db;

// Which of the two controls the port has. Decided at the first question and
// kept: both are platform devices, there from boot or not at all.
typedef enum { PORT_NONE, PORT_TYPEC, PORT_TCS1421 } port_kind_t;

static port_kind_t port_kind(void) {
	static int kind = -1;
	if (kind < 0) {
		if (access(TYPEC_PORT_TYPE, F_OK) == 0) {
			kind = PORT_TYPEC;
		} else if (access(TCS1421_CFG, W_OK) == 0) {
			kind = PORT_TCS1421;
			fprintf(stderr, "usbaudio: the port is a TCS1421 (%s)\n", TCS1421_CFG);
		} else {
			kind = PORT_NONE;
		}
	}
	return (port_kind_t)kind;
}

// The TCS1421's mode, "" when it cannot be read.
static void tcs1421_mode(char *out, size_t out_size) {
	out[0] = '\0';
	FILE *f = fopen(TCS1421_CFG, "r");
	if (!f) {
		return;
	}
	if (fgets(out, (int)out_size, f)) {
		out[strcspn(out, "\r\n")] = '\0';
	}
	fclose(f);
}

// When the TCS1421 was last set to Sink, for the ID line's check.
static time_t sink_written_at;

// Set when the ID line has been caught reading "host" with the port held as a
// sink, which a sink cannot be. From then on it is not trusted, and the R1 does
// what the stock player does: the port stays dual.
static bool otg_id_untrusted;

// Whether the port is set to dual role right now. The Type-C attribute lists
// the types it supports and brackets the one in force, so "[dual] source sink"
// is dual and "dual source [sink]" is not.
static bool port_is_dual(void) {
	if (port_kind() == PORT_TCS1421) {
		char mode[32];
		tcs1421_mode(mode, sizeof(mode));
		return strcmp(mode, TCS1421_DUAL) == 0;
	}

	FILE *f = fopen(TYPEC_PORT_TYPE, "r");
	if (!f) {
		return false;
	}
	char line[96] = {0};
	bool dual = fgets(line, sizeof(line), f) && strstr(line, "[dual]") != NULL;
	fclose(f);
	return dual;
}

// Whether `path` currently holds `token` as its bracketed role, the way the
// Type-C attributes list them: "source sink [dual]" has dual in force and
// "source [sink]" does not. Returns -1 when the file cannot be read, 0 when
// `token` is not the one in force and 1 when it is.
static int typec_role_read(const char *path, const char *token) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	char line[128];
	bool found = false;
	if (fgets(line, sizeof(line), f)) {
		char bracketed[64];
		snprintf(bracketed, sizeof(bracketed), "[%s]", token);
		found = strstr(line, bracketed) != NULL;
	}
	fclose(f);
	return found ? 1 : 0;
}

// The power role the Type-C stack negotiated. A missing attribute is UNKNOWN,
// which the supervisor treats as the old code treated every non-audio partner:
// hand the port over after the settle time rather than leave it pinned.
static usb_port_role_t typec_power_role(void) {
	if (typec_role_read(TYPEC_POWER_ROLE, "source") > 0) {
		return USB_ROLE_SOURCE;
	}
	if (typec_role_read(TYPEC_POWER_ROLE, "sink") > 0) {
		return USB_ROLE_SINK;
	}
	return USB_ROLE_UNKNOWN;
}

// And the data role: whether the controller's host side is up.
static usb_port_data_t typec_data_role(void) {
	if (typec_role_read(TYPEC_DATA_ROLE, "host") > 0) {
		return USB_DATA_HOST;
	}
	if (typec_role_read(TYPEC_DATA_ROLE, "device") > 0) {
		return USB_DATA_DEVICE;
	}
	return USB_DATA_UNKNOWN;
}

// Whether the host side of the controller has enumerated anything: a device
// under /sys/bus/usb/devices other than the root hub ("1-1", "1-1.2"; the
// interfaces, "1-1:1.0", carry a colon). Only possible with this player as the
// host end, so it says the same thing as the ID line, from the bus itself.
//
// On the R1 this is what finds a phone. The ID line cannot be relied on there:
// read the wrong way round once, it is dropped for the rest of the session
// (see partner_present()), and a phone that has won the host role from then on
// is charged and never shown the card.
static bool host_bus_has_device(void) {
	DIR *dir = opendir("/sys/bus/usb/devices");
	if (!dir) {
		return false;
	}
	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(dir)) != NULL) {
		const char *name = de->d_name;
		if (name[0] < '0' || name[0] > '9' || !strchr(name, '-') || strchr(name, ':')) {
			continue;
		}
		found = true;
	}
	closedir(dir);
	return found;
}

// The R1's ID line: true while this player is the host end.
static bool otg_id_says_host(void) {
	FILE *f = fopen(DWC2_OTG_ID, "r");
	if (!f) {
		return false;
	}
	int c = fgetc(f);
	fclose(f);
	return c == '0';
}

// Whether something is on the port that may need the port handed over.
//
// On the R3 that is anything at all: the kernel creates the partner node when
// a cable brings something with it and takes it away when it goes, before any
// role or protocol is worked out.
//
// On the R1 it is narrower, because narrower is all it can see: this player
// being the host end. That is also the only case that needs anything done. A
// charger or a computer that powers the port has already made this player the
// sink; a Mac or a phone, dual-role like this port, may just as well have made
// it the source, and then nothing charges and nothing enumerates.
static bool partner_present(void) {
	if (port_kind() == PORT_TCS1421) {
		if (host_bus_has_device()) {
			return true;
		}
		if (otg_id_untrusted || !otg_id_says_host()) {
			return false;
		}
		// A sink cannot be the host end. Given a moment to follow the last
		// write, an ID line that says it is has been read the wrong way round,
		// and the port goes back to dual for good.
		if (!port_is_dual() && sink_written_at && time(NULL) - sink_written_at >= ID_SETTLE_SECS) {
			otg_id_untrusted = true;
			fprintf(stderr, "usbaudio: %s reads host with the port held as a sink; not trusted from now on\n",
					DWC2_OTG_ID);
			return false;
		}
		return true;
	}

	DIR *dir = opendir(TYPEC_PARTNER);
	if (!dir) {
		return false;
	}
	closedir(dir);
	return true;
}

// Whether the port is empty, so that dual role can be offered again. On the R3
// the partner node says it. On the R1 a cable that powers the port and leaves
// this player the sink is invisible to partner_present(), so the charger input
// is asked as well: no host end and nothing on VBUS is an empty port.
//
// Not in the first seconds after writing Sink either: the other end has to
// notice, become the source and raise VBUS, and the PMIC reports the input
// once a second. Counted as empty in that gap, the port would go straight back
// to dual under a Mac that was about to power it.
static bool port_empty(void) {
	if (port_kind() == PORT_TCS1421) {
		if (sink_written_at && time(NULL) - sink_written_at < RELEASE_SECS + ID_SETTLE_SECS) {
			return false;
		}
		return !partner_present() && !usb_vbus_present();
	}
	return !partner_present();
}

// Writes port_type, and says whether it took. `want` is "dual" or "sink".
static bool write_port_type(const char *want) {
	bool tcs = port_kind() == PORT_TCS1421;
	const char *path = tcs ? TCS1421_CFG : TYPEC_PORT_TYPE;
	const char *text = want;
	if (tcs) {
		text = strcmp(want, "sink") == 0 ? TCS1421_SINK : TCS1421_DUAL;
	}

	FILE *f = fopen(path, "w");
	if (!f) {
		static bool said;
		if (!said) {
			said = true;
			fprintf(stderr, "usbaudio: %s is not there; the port cannot take a peripheral\n", path);
		}
		return false;
	}
	bool ok = fputs(text, f) >= 0 && fputc('\n', f) != EOF;
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok && tcs && strcmp(want, "sink") == 0) {
		sink_written_at = time(NULL);
	}
	return ok;
}

// The MP2731's 5 V boost: 1 on, 0 off, -1 when the node is not there or reads
// neither.
static int otg_boost_get(void) {
	FILE *f = fopen(MP2731_OTG, "r");
	if (!f) {
		return -1;
	}
	int c = fgetc(f);
	fclose(f);
	if (c == '1') {
		return 1;
	}
	if (c == '0') {
		return 0;
	}
	return -1;
}

// Sets the boost, and only when it is not already where it is wanted: this runs
// on the four-a-second poll, and a write per poll would be a write to the
// charger per poll. Logged once per transition for the same reason. A refused
// write is tried again after two seconds, not every quarter second and not
// never: one transient failure must not silently leave the port without VBUS
// for the rest of the session.
static void otg_boost_set(bool on) {
	static int asked = -1;
	static time_t asked_at;
	int want = on ? 1 : 0;
	time_t now = time(NULL);

	if (otg_boost_get() == want) {
		asked = want;
		return;
	}
	if (asked == want && now - asked_at < 2) {
		return;
	}
	asked = want;
	asked_at = now;

	FILE *f = fopen(MP2731_OTG, "w");
	bool ok = false;
	if (f) {
		ok = fputs(on ? "1" : "0", f) >= 0 && fputc('\n', f) != EOF;
		if (fclose(f) != 0) {
			ok = false;
		}
	}
	fprintf(stderr, "usbaudio: 5 V boost %s%s\n", on ? "on" : "off",
			ok ? "" : " REFUSED by the charger driver");
}

// Hands the port back to the other end. See the state machine in
// arbitrate_port(): this is what a phone needs.
static void port_become_sink(void) {
	if (!port_is_dual()) {
		return;
	}
	bool ok = write_port_type("sink");
	fprintf(stderr, "usbaudio: something on the port is not a sound card; sink %s\n",
			ok ? "written, the other end can be the host" : "REFUSED");
}

// Allows the port to take a peripheral, and says whether it stuck.
//
// Written whenever the port is found not to be dual, not once at startup: the
// setting does not stay put, most likely because the gadget is bound and
// unbound as the cable comes and goes. Checking and rewriting costs one read a
// second and a write only when the answer is wrong.
bool usbaudio_ensure_dual(void) {
	if (port_is_dual()) {
		return true;
	}

	bool ok = write_port_type("dual");
	bool stuck = ok && port_is_dual();

	// Said when the answer changes, not once a second: if something out there
	// keeps putting the port back, this runs every poll and a line per second
	// would bury the log it is meant to explain.
	static int said = -1;
	int outcome = stuck ? 0 : (ok ? 1 : 2);
	if (outcome != said) {
		said = outcome;
		fprintf(stderr, "usbaudio: dual role %s\n",
				stuck ? "set" : (ok ? "written but did not stick" : "REFUSED by the driver"));
	}
	return stuck;
}

// At startup the port is offered dual role only when it is empty. Booting with
// a phone or a PC already on the cable and grabbing the host role from it is
// the same failure as doing it later, and here there is no reason to: whatever
// is on the port arrived before this player did.
//
// The Type-C power role says which of those it is. A sink role means a PC or
// charger has taken the cable and is supplying VBUS; that is left alone as
// before. A source role, or a role that cannot be read, may be a DAC that
// booted with the player: usb.c forces the port to sink for the storage
// gadget, and left that way the DAC would never appear, which is the bug this
// change is for.
static bool offer_port_role(bool announce) {
	if (!partner_present()) {
		return usbaudio_ensure_dual();
	}
	if (port_kind() == PORT_TCS1421) {
		if (announce) {
			fprintf(stderr, "usbaudio: something is already on the port at startup; leaving the role alone\n");
		}
		return false;
	}
	if (typec_power_role() == USB_ROLE_SINK) {
		if (announce) {
			fprintf(stderr, "usbaudio: a source is already on the port at startup; leaving the role alone\n");
		}
		return false;
	}
	if (announce) {
		fprintf(stderr, "usbaudio: a peripheral is on the port at startup; offering dual role\n");
	}
	return usbaudio_ensure_dual();
}

bool usbaudio_init(void) { return offer_port_role(true); }

// The same choice, made again after usb_start() has necessarily put the port
// in sink to bind the storage gadget. The stock player does exactly this in
// its set_usb_working_mode(): the gadget is bound first and the saved role is
// written after it, so a bus-powered DAC plugged in at boot is not left with
// an unpowered port until the supervisor next polls.
void usbaudio_reapply_role(void) { offer_port_role(false); }

// Where card `n`'s device really is. Empty when it has none.
//
// realpath and not readlink: the link's own target is relative
// ("../../../1-1:1.0"), so looking for "/usb" in it never matches. Resolved,
// the same link comes out as /sys/devices/platform/jz-dwc2/usb1/1-1/1-1:1.0.
static void card_device_path(int n, char *out, size_t out_size) {
	char link[PATH_MAX];
	char resolved[PATH_MAX];

	out[0] = '\0';
	snprintf(link, sizeof(link), SOUND_CLASS "/card%d/device", n);
	if (realpath(link, resolved)) {
		snprintf(out, out_size, "%s", resolved);
	}
}

// Whether card `n` hangs off the USB bus rather than off the board.
//
// Asked of the bus and not of the name: the built-in card is called whatever
// the machine driver calls it, and a USB device whatever its maker wrote in its
// descriptor. Two ways, because one of them is exact and the other always
// works: a USB device's `subsystem` link resolves to .../bus/usb, and failing
// that the device's own path runs through the controller's usbN directory.
static bool card_is_usb(int n) {
	char link[PATH_MAX];
	char resolved[PATH_MAX];

	snprintf(link, sizeof(link), SOUND_CLASS "/card%d/device/subsystem", n);
	if (realpath(link, resolved)) {
		size_t len = strlen(resolved);
		if (len >= 4 && strcmp(resolved + len - 4, "/usb") == 0) {
			return true;
		}
	}

	card_device_path(n, resolved, sizeof(resolved));
	return strstr(resolved, "/usb") != NULL;
}

// The lowest-numbered USB sound card, or -1. Lowest rather than newest because
// there is only ever one socket: a second would mean a hub, and the first is as
// good a choice as any.
static int find_usb_card(void) {
	DIR *dir = opendir(SOUND_CLASS);
	if (!dir) {
		return -1;
	}

	int found = -1;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "card", 4) != 0) {
			continue;
		}
		char *end = NULL;
		long n = strtol(de->d_name + 4, &end, 10);
		if (!end || *end || n < 0) {
			continue; // pcmC0D0p and the rest of the class
		}
		if (card_is_usb((int)n) && (found < 0 || n < found)) {
			found = (int)n;
		}
	}
	closedir(dir);
	return found;
}

static void read_card_id(int card, char *out, size_t out_size) {
	char path[128];
	snprintf(path, sizeof(path), SOUND_CLASS "/card%d/id", card);

	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	if (fgets(out, (int)out_size, f)) {
		out[strcspn(out, "\r\n")] = '\0';
	}
	fclose(f);
}

// The device's own playback volume, whatever it decided to call it.
//
// USB audio devices name it themselves -- "PCM Playback Volume" is the usual
// one, but headsets ship "Speaker Playback Volume" and "Headphone Playback
// Volume" too -- so the control is found by shape rather than by name: the
// first integer control whose name ends in "Playback Volume". Its range and
// its channel count come with it, because a USB device's are its own and
// nothing like the CS43198's.
//
// Writable is part of the shape. A control that only reports a level is not a
// volume control for this purpose, and taking it for one would stand the
// software attenuation down in favour of something that cannot move.
static void find_volume_control(int card) {
	snd_ctl_t *ctl;
	snd_ctl_elem_list_t *list;
	char name[32];

	volume_control[0] = '\0';
	volume_min = volume_max = 0;
	volume_count = 1;
	volume_has_db = false;

	snprintf(name, sizeof(name), "hw:%d", card);
	if (snd_ctl_open(&ctl, name, 0) < 0) {
		return;
	}

	snd_ctl_elem_list_alloca(&list);
	if (snd_ctl_elem_list(ctl, list) < 0) {
		snd_ctl_close(ctl);
		return;
	}
	unsigned int count = snd_ctl_elem_list_get_count(list);
	if (snd_ctl_elem_list_alloc_space(list, count) < 0 || snd_ctl_elem_list(ctl, list) < 0) {
		snd_ctl_close(ctl);
		return;
	}

	static const char SUFFIX[] = "Playback Volume";
	for (unsigned int i = 0; i < count && !volume_control[0]; i++) {
		snd_ctl_elem_id_t *id;
		snd_ctl_elem_info_t *info;

		snd_ctl_elem_id_alloca(&id);
		snd_ctl_elem_info_alloca(&info);
		snd_ctl_elem_list_get_id(list, i, id);
		snd_ctl_elem_info_set_id(info, id);
		if (snd_ctl_elem_info(ctl, info) < 0 ||
			snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_INTEGER ||
			!snd_ctl_elem_info_is_writable(info)) {
			continue;
		}

		const char *elem = snd_ctl_elem_info_get_name(info);
		size_t elen = strlen(elem);
		size_t slen = sizeof(SUFFIX) - 1;
		if (elen < slen || strcmp(elem + elen - slen, SUFFIX) != 0) {
			continue;
		}

		snprintf(volume_control, sizeof(volume_control), "%s", elem);
		volume_min = snd_ctl_elem_info_get_min(info);
		volume_max = snd_ctl_elem_info_get_max(info);
		volume_count = snd_ctl_elem_info_get_count(info);
		if (volume_count < 1 || volume_count > 8) {
			volume_count = 1;
		}
		long db_min = 0, db_max = 0;
		volume_has_db = snd_ctl_elem_info_is_tlv_readable(info) &&
						snd_ctl_elem_tlv_read(ctl, id, volume_tlv, sizeof(volume_tlv)) >= 0 &&
						snd_tlv_get_dB_range(volume_tlv, volume_min, volume_max, &db_min, &db_max) >= 0;
	}

	snd_ctl_elem_list_free_space(list);
	snd_ctl_close(ctl);

	if (volume_control[0]) {
		long db_min = 0, db_max = 0;
		if (volume_has_db) {
			snd_tlv_get_dB_range(volume_tlv, volume_min, volume_max, &db_min, &db_max);
		}
		fprintf(stderr, "usbaudio: volume goes to '%s' (%ld..%ld, %u ch, %s%.2f..%.2f dB)\n", volume_control,
				volume_min, volume_max, volume_count, volume_has_db ? "" : "no dB scale, ", db_min / 100.0,
				db_max / 100.0);
	} else {
		fprintf(stderr, "usbaudio: the device has no playback volume control\n");
	}
}

// The level on a device on the USB-C port.
//
// The index follows the jacks' own law (alsa_volume_gain_db100): the same
// number is about as loud on a dongle as on the 3.5 mm socket, and 0 is
// silence.
//
// How the level is split between the device and the samples:
//
//   * no volume control: all of it on the samples (swvolume.c);
//   * by default, the device's control is held at its 0 dB step (at its top
//     when it has no dB scale) and all of the level goes on the samples. It is
//     what a phone does. A dongle's own control can be coarser than it
//     declares -- a whole run of indices landing on one step -- or stop well
//     above silence, and neither can be seen from here;
//   * with [usb] hardware_volume = 1 and a dB scale, the control takes the
//     nearest step at or above the target and the samples the difference, so
//     a coarse or shallow control still moves smoothly all the way down.
//
// The value written is read back. A driver is allowed to round it; sitting at
// the top of its range after being asked for well below is a control that does
// nothing, and then it is given up and the samples take the whole level.
void usbaudio_apply_volume(int percent) {
	if (active_card < 0) {
		swvolume_clear_usb_gain();
		return;
	}
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}
	bool mute = percent == 0;
	long target = mute ? 0 : alsa_volume_gain_db100(percent);

	if (!volume_control[0] || volume_max <= volume_min) {
		swvolume_set_usb_gain(target, mute);
		return;
	}

	bool hardware = volume_has_db && config_get_int("usb", "hardware_volume", 0);
	long value = volume_max;
	long hw_db = 0;
	if (mute) {
		value = volume_min;
	} else if (volume_has_db) {
		long want = hardware ? target : 0;
		if (snd_tlv_convert_from_dB(volume_tlv, volume_min, volume_max, want, &value, hardware ? 1 : -1) < 0) {
			value = volume_max;
		}
		if (value < volume_min) {
			value = volume_min;
		}
		if (value > volume_max) {
			value = volume_max;
		}
		if (snd_tlv_convert_to_dB(volume_tlv, volume_min, volume_max, value, &hw_db) < 0 ||
			hw_db <= SND_CTL_TLV_DB_GAIN_MUTE) {
			hw_db = 0;
		}
	}
	long rest = target - hw_db;
	swvolume_set_usb_gain(rest < 0 ? rest : 0, mute);

	snd_ctl_t *ctl;
	char card[32];
	snprintf(card, sizeof(card), "hw:%d", active_card);
	if (snd_ctl_open(&ctl, card, 0) < 0) {
		return;
	}

	snd_ctl_elem_id_t *id;
	snd_ctl_elem_value_t *elem;
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_value_alloca(&elem);

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, volume_control);
	snd_ctl_elem_value_set_id(elem, id);
	// Every channel the control carries, and no more: writing two values to a
	// mono control is harmless but writing one to a stereo control leaves the
	// right ear where it was.
	for (unsigned int c = 0; c < volume_count; c++) {
		snd_ctl_elem_value_set_integer(elem, c, value);
	}

	int wrote = snd_ctl_elem_write(ctl, elem);
	long back = value;
	if (wrote >= 0) {
		snd_ctl_elem_value_t *check;
		snd_ctl_elem_value_alloca(&check);
		snd_ctl_elem_value_set_id(check, id);
		if (snd_ctl_elem_read(ctl, check) >= 0) {
			back = snd_ctl_elem_value_get_integer(check, 0);
		}
	}
	snd_ctl_close(ctl);

	// Asked for something below the top and still sitting at the top. The slack
	// is never zero: on a control with only a few steps, max - 0 would read as
	// "stuck" the moment the level really is the maximum.
	long slack = (volume_max - volume_min) / 20;
	if (slack < 1) {
		slack = 1;
	}
	bool stuck = value <= volume_max - slack && back >= volume_max - slack;
	if (wrote < 0 || stuck) {
		fprintf(stderr,
				"usbaudio: '%s' did not take %d%% (asked %ld, reads %ld); the level goes to the samples "
				"instead\n",
				volume_control, percent, value, back);
		volume_control[0] = '\0';
		swvolume_set_usb_gain(target, mute);
	}
}

// Who the port belongs to.
//
// Dual role is what lets headphones be seen at all -- as a sink this device
// offers Rd, headphones offer Rd, and two Rd's never notice each other -- so
// the port has to be dual while it is empty or there is nothing to detect.
//
// But a phone is dual-role too, and Android implements Try.SNK: left dual, the
// two agree that this device is the host, so it charges the phone and never
// offers it the card or the DAC.
//
// Neither role can be chosen in advance, because the thing that distinguishes
// the two cases only shows up after the port is already connected: headphones
// become a sound card, a phone does not. So the port stays dual while nothing
// is attached; when something attaches it has HANDOVER_SECS to turn into a
// sound card, and if it does not, the port goes sink and the other end gets to
// be the host. It goes back to dual once the port is empty again. The cost is
// that a phone connects a few seconds late rather than instantly.
//
// The R1 runs the same rules on its TCS1421. A computer's USB-C port is often
// dual-role as well, and against one the R1 can come out the host end: then
// the cable neither charges it nor shows anything to the computer.
// What the R1's port looks like, written to the log each time it changes: the
// TCS1421 mode, the ID line, VBUS and the host bus. The four together are what
// says why a cable did or did not end up where it should.
static void tcs1421_trace(void) {
	char mode[32];
	tcs1421_mode(mode, sizeof(mode));
	int id = -1;
	FILE *f = fopen(DWC2_OTG_ID, "r");
	if (f) {
		int c = fgetc(f);
		id = (c == '0' || c == '1') ? c - '0' : -1;
		fclose(f);
	}
	int vbus = usb_vbus_present() ? 1 : 0;
	int bus = host_bus_has_device() ? 1 : 0;

	static char said_mode[32];
	static int said_id = -2, said_vbus = -1, said_bus = -1;
	if (strcmp(mode, said_mode) == 0 && id == said_id && vbus == said_vbus && bus == said_bus) {
		return;
	}
	snprintf(said_mode, sizeof(said_mode), "%s", mode);
	said_id = id;
	said_vbus = vbus;
	said_bus = bus;
	fprintf(stderr, "usbaudio: port %s, otg_id %d%s, vbus %d, device on the host bus %d\n", mode[0] ? mode : "?", id,
			otg_id_untrusted ? " (not trusted)" : "", vbus, bus);
}

// Seconds since a timestamp, 0 when it was never set or the clock went back.
static unsigned secs_since(time_t at, time_t now) {
	if (at == 0 || now < at) {
		return 0;
	}
	return (unsigned)(now - at);
}

// The R3 Pro II's port, one decision per poll.
//
// usbport.h holds the rules and the reasons. This half only reads the port into
// a state, keeps the clocks those rules need and carries out the one action it
// gets back. Time is in whole seconds because the attributes change no faster;
// the poll runs four times a second, so every transition is seen within a
// quarter of one.
static void typec_arbitrate(bool audio_present, time_t now) {
	static time_t partner_at;	// when the current partner appeared
	static time_t source_at;	// when sourcing began, for the boost cycle
	static time_t stuck_at;		// when host mode with nothing attached began
	static time_t reset_at;		// when the last reset was started
	static time_t empty_at;		// when the port last became empty
	static time_t empty_since;	// the longer view, across a boost pulse
	static time_t boost_off_until;	// a pulse must be long enough to be seen
	static bool reset_pending;
	static unsigned boost_cycles;

	bool partner = partner_present();
	bool vbus = usb_vbus_present();
	bool host_device = host_bus_has_device();
	bool empty = !partner && !vbus && !host_device;

	// A sound card, an empty port and a detach all start the story again. The
	// boost cycle budget belongs to the partner: the next device gets its own.
	// `stuck_at` is deliberately not cleared here: "empty" is also where the
	// stuck state lives, and clearing it on every poll would restart the six
	// seconds before they ever elapsed.
	if (audio_present) {
		partner_at = 0;
		source_at = 0;
		stuck_at = 0;
		empty_since = 0;
		boost_cycles = 0;
	} else if (empty) {
		partner_at = 0;
		source_at = 0;
		if (empty_since == 0) {
			empty_since = now;
		}
	} else if (partner && partner_at == 0) {
		// A new partner gets a fresh pulse budget, but only after the port has
		// been quiet for a while: a bus-powered device can lose its CC
		// termination for the quarter second the boost is off, and counting
		// that gap as a new device would pulse it for ever.
		if (empty_since && secs_since(empty_since, now) >= USB_PORT_FRESH_PARTNER_SECS) {
			boost_cycles = 0;
		}
		empty_since = 0;
		partner_at = now;
		source_at = now;
	} else if (!partner) {
		// A charger with no partner node, or a partner that has just gone.
		partner_at = 0;
		source_at = 0;
		empty_since = 0;
	}

	usb_port_role_t power_role = typec_power_role();
	usb_port_data_t data_role = typec_data_role();
	bool otg_on = otg_boost_get() > 0;
	bool stuck = empty && (data_role == USB_DATA_HOST || otg_on);
	if (stuck) {
		if (stuck_at == 0) {
			stuck_at = now;
		}
	} else {
		stuck_at = 0;
	}

	// The existing release delay, and only where the decision would otherwise
	// offer dual role: changing port_type while something is attached makes
	// the kernel negotiate afresh, and the partner disappears for a moment
	// after each write. Without this the two states chase each other. A reset
	// is not delayed, or it would never finish.
	if (!reset_pending) {
		if (empty && !stuck) {
			if (empty_at == 0) {
				empty_at = now;
			}
			if (secs_since(empty_at, now) < RELEASE_SECS) {
				return;
			}
		} else {
			empty_at = 0;
		}
	}

	// A boost pulse has to be long enough for the device on the far end to see
	// it. The poll runs four times a second, and `time()` only moves once, so
	// without this hold the off and the on would land in the same second.
	if (boost_off_until) {
		if (now < boost_off_until) {
			return;
		}
		boost_off_until = 0;
	}

	usb_port_state_t state = {
		.kind = USB_PORT_TYPEC,
		.dac_mode = usb_dac_mode(),
		.port_dual = port_is_dual(),
		.partner = partner,
		.audio = audio_present,
		.vbus = vbus,
		.host_device = host_device,
		.otg_on = otg_on,
		.power_role = power_role,
		.data_role = data_role,
		.partner_secs = secs_since(partner_at, now),
		.source_secs = secs_since(source_at, now),
		.stuck_secs = secs_since(stuck_at, now),
		.reset_secs = secs_since(reset_at, now),
		.reset_pending = reset_pending,
		.boost_cycles = boost_cycles,
	};

	switch (usb_port_decide(&state)) {
	case USB_PORT_ACT_KEEP_DUAL:
		usbaudio_ensure_dual();
		reset_pending = false;
		reset_at = 0;
		break;

	case USB_PORT_ACT_BECOME_SINK:
		port_become_sink();
		break;

	case USB_PORT_ACT_BOOST_ON:
		otg_boost_set(true);
		source_at = now;
		break;

	case USB_PORT_ACT_BOOST_OFF:
		otg_boost_set(false);
		boost_cycles++;
		source_at = now;
		boost_off_until = now + 1;
		break;

	case USB_PORT_ACT_RESET:
		// Both halves in one go: the role back to sink so the controller
		// leaves host mode, and the boost off so the MP2731 stops sourcing.
		// RESET_SECS of quiet then follow before dual is offered again.
		write_port_type("sink");
		otg_boost_set(false);
		reset_pending = true;
		reset_at = now;
		partner_at = 0;
		source_at = 0;
		stuck_at = 0;
		empty_at = 0;
		boost_cycles = 0;
		fprintf(stderr, "usbaudio: port stuck in host mode with nothing on it; role and boost reset\n");
		break;

	case USB_PORT_ACT_NONE:
	default:
		break;
	}
}

static void arbitrate_port(bool audio_present) {
	time_t now = time(NULL);
	port_kind_t kind = port_kind();

	if (kind == PORT_TYPEC) {
		typec_arbitrate(audio_present, now);
		return;
	}
	if (kind != PORT_TCS1421) {
		return;
	}

	// The R1's TCS1421 rules, unchanged: no Type-C class, no partner node and
	// no boost, only the mode attribute, the ID line and VBUS. See usbport.h
	// for why the R3 could not keep them.
	static time_t attached_at;	// when the current non-audio partner appeared
	static time_t empty_at;		// when the port last became empty

	tcs1421_trace();

	if (audio_present) {
		attached_at = 0;
		empty_at = 0;
		usbaudio_ensure_dual();
		return;
	}

	if (partner_present()) {
		empty_at = 0;
		if (attached_at == 0) {
			attached_at = now;
		} else if (now - attached_at >= HANDOVER_SECS) {
			port_become_sink();
		}
		return;
	}

	attached_at = 0;
	if (!port_empty()) {
		empty_at = 0;
		return;
	}
	if (empty_at == 0) {
		empty_at = now;
	}
	// Nothing on the port, and nothing on it for long enough that this is not
	// the gap left by a role write: it can be dual again, ready for the next
	// pair of headphones.
	if (now - empty_at >= RELEASE_SECS) {
		usbaudio_ensure_dual();
	}
}

void usbaudio_poll(void) {
	// Who owns the port comes first, and is asked whatever playback is doing:
	// the arbitration is about the socket, not about where the sound goes.
	int card = find_usb_card();
	arbitrate_port(card >= 0);

	// Bluetooth wins outright over where playback goes. Both would be pointing
	// it somewhere, and the one the listener is wearing is the one that was
	// chosen deliberately; fighting over audio_set_output_device() from two
	// pollers is how the sound ends up somewhere nobody asked for.
	if (bluetooth_audio_active()) {
		return;
	}

	if (card == active_card) {
		return;
	}

	active_card = card;
	if (card >= 0) {
		char pcm[64];
		// plughw and not hw: what comes out of the decoder is whatever the file
		// held, and a USB headset takes the two or three formats it was built
		// for. The plug layer converts; hw would simply refuse to open.
		snprintf(pcm, sizeof(pcm), "plughw:%d,0", card);
		char where[PATH_MAX];
		read_card_id(card, active_name, sizeof(active_name));
		card_device_path(card, where, sizeof(where));
		fprintf(stderr, "usbaudio: card%d '%s' at %s; playback goes to %s\n", card, active_name, where, pcm);

		// The level before the route, and not after it: between the two calls
		// the sound is already leaving over the port, and a device fresh off
		// the bus sits at whatever its own default is -- which for a dongle is
		// the top of its range. The other order puts a full-scale burst into a
		// pair of headphones somebody is wearing.
		find_volume_control(card);
		usbaudio_apply_volume(get_volume_percent());
		audio_set_output_device(pcm);
	} else {
		active_name[0] = '\0';
		volume_control[0] = '\0';
		swvolume_clear_usb_gain();
		fprintf(stderr, "usbaudio: the port is empty again; playback goes back to the jacks\n");
		audio_set_output_device(NULL);
	}
}

bool usbaudio_active(void) { return active_card >= 0; }

bool usbaudio_has_volume_control(void) { return active_card >= 0 && volume_control[0] && volume_max > volume_min; }

void usbaudio_card_name(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	snprintf(out, out_size, "%s", active_name);
}
