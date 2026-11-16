#ifndef USBPORT_H
#define USBPORT_H

#include <stdbool.h>

// Who owns the USB-C port, decided from what the port can see.
//
// The decision is kept here, away from the files under /sys, so that it can be
// run on a host: the rules are about seconds and about states that take ten
// seconds of cable work to reach on the device, and a table of inputs is the
// only honest way to try them all. usbaudio.c reads the port into one of these
// structs and carries out the single action it gets back. Nothing here opens a
// file, reads the clock or keeps state.
//
// Two opposite mistakes are easy, and the rules exist for both. Treating "no
// sound card after three seconds" as "the other end is a host" is right for a
// phone and wrong for a bus-powered DAC: the DAC cannot enumerate until this
// player puts 5 V on the port, so the old arbitration killed the very thing it
// was waiting for. And after an overcurrent the port can be left with the
// boost on and a stale host role, which survives a reboot because the boost is
// set on an always-powered charger. So an empty port is always given up, and a
// partner that never enumerates gets its boost cycled a few times before the
// port is handed over.

typedef enum {
	USB_PORT_R1,		// TCS1421: the R1's own logic stays in usbaudio.c
	USB_PORT_TYPEC,		// R3 Pro II: FUSB302/TCPM and the MP2731 boost
} usb_port_kind_t;

// The Type-C power role as the port negotiated it. UNKNOWN is a missing or
// unreadable attribute, and is treated as the old code treated every partner.
typedef enum {
	USB_ROLE_UNKNOWN = -1,
	USB_ROLE_SINK = 0,
	USB_ROLE_SOURCE = 1,
} usb_port_role_t;

// And the data role, which says whether the controller's host side is up.
typedef enum {
	USB_DATA_UNKNOWN = -1,
	USB_DATA_DEVICE = 0,
	USB_DATA_HOST = 1,
} usb_port_data_t;

// How long a non-audio partner is given to turn into a sound card before the
// port is handed over. Enumerating a USB audio device takes well under a
// second; three is generous and still short enough that a phone connects a beat
// late rather than not at all.
#define USB_PORT_HANDOVER_SECS 3

// How long a source-role partner is given to enumerate before its boost is
// cycled. A bus-powered device draws almost nothing until VBUS is there and
// then boots its own firmware; four seconds covers a slow one without leaving
// a dead port powered for long.
#define USB_PORT_RECOVER_SECS 4

// How long a port can sit in host mode with nothing on it before the role and
// the boost are reset. The overcurrent storm in the log lasted seconds, so this
// is long enough to tell a real stuck state from a role write still settling.
#define USB_PORT_STUCK_SECS 6

// How long the port is left alone after that reset, so the other end has time
// to see the role change instead of being flicked between states.
#define USB_PORT_RESET_SECS 2

// How many off/on boost cycles a partner gets before the supervisor stops
// trying and leaves the port sourced. Enough for a device that needs one clean
// VBUS edge, few enough that a shorted cable is not pulsed indefinitely.
#define USB_PORT_MAX_BOOST_CYCLES 3

// A device that appears again after a shorter gap than this is the same device
// coming back from the boost pulse, not a new one: it keeps the pulse budget
// it had. Longer than the one-second pulse and its settle, short enough that a
// real re-plug is recognised as one.
#define USB_PORT_FRESH_PARTNER_SECS 3

typedef struct {
	usb_port_kind_t kind;
	bool dac_mode;			// usb_dac_mode(): the player is a USB sound card
	bool port_dual;			// port_type currently reads "[dual]"
	bool partner;			// /sys/class/typec/port0-partner exists
	bool audio;				// a USB sound card is present
	bool vbus;				// VBUS is arriving into the player
	bool host_device;		// a device is on this player's host bus
	bool otg_on;			// MP2731 usb_otg currently 1
	usb_port_role_t power_role;
	usb_port_data_t data_role;
	unsigned partner_secs;	// seconds since the current partner appeared
	unsigned source_secs;	// seconds since sourcing with no enumeration
	unsigned stuck_secs;	// seconds in the stuck condition
	unsigned reset_secs;	// seconds since a reset was started
	bool reset_pending;
	unsigned boost_cycles;	// boost off/on cycles already tried
} usb_port_state_t;

typedef enum {
	USB_PORT_ACT_NONE = 0,
	USB_PORT_ACT_KEEP_DUAL,		// ensure port_type=dual
	USB_PORT_ACT_BECOME_SINK,	// write port_type=sink (hand over)
	USB_PORT_ACT_BOOST_ON,		// write usb_otg=1
	USB_PORT_ACT_BOOST_OFF,		// write usb_otg=0 (counts as a cycle)
	USB_PORT_ACT_RESET,			// port_type=sink + usb_otg=0, caller arms reset_pending
} usb_port_action_t;

// The one thing to do next. The caller executes it and updates its counters;
// this function only reads `s`. The R1 never gets an action: its TCS1421 rules
// stay in usbaudio.c, unchanged, and this table must not quietly take them over.
usb_port_action_t usb_port_decide(const usb_port_state_t *s);

#endif /* USBPORT_H */
