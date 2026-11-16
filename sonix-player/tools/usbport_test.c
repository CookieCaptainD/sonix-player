// Tests for the USB-C port decision in src/system/audio/usbport.c.
//
// Plain C and no framework: this runs on the host with `make usbport-test`, and
// the module under test is pure so the table below is the whole of the code's
// input. Each case names the situation on the port and the one action the
// supervisor is expected to carry out.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "src/system/audio/usbport.h"

static int failures;

static void check(const char *name, usb_port_action_t got, usb_port_action_t want) {
	if (got == want) {
		return;
	}
	fprintf(stderr, "FAIL %s: got %d, want %d\n", name, (int)got, (int)want);
	failures++;
}

// A port on the R3 in its quiet state: empty, dual, no roles worked out yet.
static usb_port_state_t typec(void) {
	usb_port_state_t s;
	memset(&s, 0, sizeof(s));
	s.kind = USB_PORT_TYPEC;
	s.port_dual = true;
	s.power_role = USB_ROLE_UNKNOWN;
	s.data_role = USB_DATA_UNKNOWN;
	return s;
}

static usb_port_state_t r1(void) {
	usb_port_state_t s = typec();
	s.kind = USB_PORT_R1;
	return s;
}

// A source-role partner that is not a sound card and is not a host device:
// the bus-powered DAC, or USB-C headphones, before either has booted.
static usb_port_state_t bus_powered_dac(void) {
	usb_port_state_t s = typec();
	s.partner = true;
	s.power_role = USB_ROLE_SOURCE;
	s.partner_secs = 20;
	s.source_secs = 1;
	return s;
}

// The R1 keeps the TCS1421 rules in usbaudio.c. Every combination of inputs
// must leave the decision alone, including the ones that would reset or boost
// on the R3: sharing the table between the two ports would be the easy way to
// change the R1 without noticing.
static void test_r1_is_always_none(void) {
	static const usb_port_role_t power[] = {USB_ROLE_UNKNOWN, USB_ROLE_SINK, USB_ROLE_SOURCE};
	static const usb_port_data_t data[] = {USB_DATA_UNKNOWN, USB_DATA_DEVICE, USB_DATA_HOST};

	unsigned n = 0;
	for (int flags = 0; flags < 128; flags++) {
		for (int p = 0; p < 3; p++) {
			for (int d = 0; d < 3; d++) {
				usb_port_state_t s = r1();
				s.dac_mode = (flags & 1) != 0;
				s.port_dual = (flags & 2) != 0;
				s.partner = (flags & 4) != 0;
				s.audio = (flags & 8) != 0;
				s.vbus = (flags & 16) != 0;
				s.host_device = (flags & 32) != 0;
				s.otg_on = (flags & 64) != 0;
				s.power_role = power[p];
				s.data_role = data[d];
				s.partner_secs = 20;
				s.source_secs = 20;
				s.stuck_secs = 20;
				s.reset_pending = (flags & 8) != 0;
				s.reset_secs = 1;

				char name[64];
				snprintf(name, sizeof(name), "R1 combination %u", n++);
				check(name, usb_port_decide(&s), USB_PORT_ACT_NONE);
			}
		}
	}
}

// While the player is a USB sound card the cable belongs to the PC, and the
// port must not be handed over or reset underneath the stream.
static void test_dac_mode_keeps_hands_off(void) {
	usb_port_state_t s = typec();
	s.dac_mode = true;
	s.partner = true;
	s.power_role = USB_ROLE_SOURCE;
	s.data_role = USB_DATA_HOST;
	s.partner_secs = 30;
	s.stuck_secs = 30;
	check("dac mode, busy port", usb_port_decide(&s), USB_PORT_ACT_NONE);

	// The role is sacred under a stream; the boost is not. A boost left on by
	// an earlier failure has to go even in DAC mode, or the player keeps 5 V
	// on an empty connector (or pushes it into the PC).
	s.partner = false;
	s.data_role = USB_DATA_HOST;
	s.otg_on = true;
	check("dac mode, boost left on", usb_port_decide(&s), USB_PORT_ACT_BOOST_OFF);

	s = typec();
	s.dac_mode = true;
	s.partner = true;
	s.power_role = USB_ROLE_SINK;
	s.vbus = true;
	s.otg_on = true;
	check("dac mode, boost on into the PC", usb_port_decide(&s), USB_PORT_ACT_BOOST_OFF);

	s.otg_on = false;
	check("dac mode, boost off with the PC", usb_port_decide(&s), USB_PORT_ACT_NONE);
}

// A sound card is on the bus, so the port is doing its job. Keep dual role
// asserted: the kernel can drop it when the gadget is bound and unbound.
static void test_audio_keeps_dual(void) {
	usb_port_state_t s = typec();
	s.partner = true;
	s.audio = true;
	s.power_role = USB_ROLE_SOURCE;
	s.data_role = USB_DATA_HOST;
	s.partner_secs = 30;
	check("audio present", usb_port_decide(&s), USB_PORT_ACT_KEEP_DUAL);

	s.port_dual = false;
	check("audio present, role dropped", usb_port_decide(&s), USB_PORT_ACT_KEEP_DUAL);
}

// The failure this change is for: a bus-powered DAC cannot enumerate until
// VBUS is on the port, so the supervisor must source and then restart the
// boost, never read "no sound card" as "the other end is a host".
static void test_bus_powered_dac_bootstrap(void) {
	usb_port_state_t s = bus_powered_dac();

	s.otg_on = false;
	check("DAC, boost off", usb_port_decide(&s), USB_PORT_ACT_BOOST_ON);

	s.otg_on = true;
	s.source_secs = 1;
	check("DAC, boost on, just sourced", usb_port_decide(&s), USB_PORT_ACT_NONE);

	s.source_secs = USB_PORT_RECOVER_SECS - 1;
	check("DAC, boost on, not yet due", usb_port_decide(&s), USB_PORT_ACT_NONE);

	s.source_secs = USB_PORT_RECOVER_SECS;
	check("DAC, boost on, due a cycle", usb_port_decide(&s), USB_PORT_ACT_BOOST_OFF);

	s.source_secs = 30;
	s.boost_cycles = USB_PORT_MAX_BOOST_CYCLES;
	check("DAC, cycles used up", usb_port_decide(&s), USB_PORT_ACT_NONE);

	// And the regression itself: nothing about a partner with no sound card
	// and no host device may hand the port over, however long it sits there,
	// even once the boost cycles are used up.
	s.boost_cycles = USB_PORT_MAX_BOOST_CYCLES;
	s.partner_secs = 600;
	s.source_secs = 30;
	s.otg_on = true;
	check("DAC, long wait, boost on", usb_port_decide(&s), USB_PORT_ACT_NONE);
}

// A phone is the case the old handover was written for: it enumerates on this
// player's host bus but never becomes a sound card, so the port goes sink and
// the phone is allowed to be the host.
static void test_phone_hands_over(void) {
	usb_port_state_t s = typec();
	s.partner = true;
	s.power_role = USB_ROLE_SOURCE;
	s.host_device = true;
	s.data_role = USB_DATA_HOST;

	s.partner_secs = 1;
	check("phone, still settling", usb_port_decide(&s), USB_PORT_ACT_NONE);

	s.partner_secs = USB_PORT_HANDOVER_SECS;
	check("phone, handover due", usb_port_decide(&s), USB_PORT_ACT_BECOME_SINK);
}

// A charger or a PC is already the source, so this player is the sink and the
// old three-second handover still applies. It must not be turned into a boost
// cycle: there is nothing here to power.
static void test_charger_settles_sink(void) {
	usb_port_state_t s = typec();
	s.partner = true;
	s.power_role = USB_ROLE_SINK;
	s.vbus = true;

	s.partner_secs = 1;
	check("charger, still settling", usb_port_decide(&s), USB_PORT_ACT_NONE);

	s.partner_secs = USB_PORT_HANDOVER_SECS;
	check("charger, handover due", usb_port_decide(&s), USB_PORT_ACT_BECOME_SINK);
}

// A partner whose role cannot be read is treated as the old code treated every
// non-audio partner, so an old kernel or a missing attribute cannot leave the
// port pinned.
static void test_unknown_power_role_falls_back(void) {
	usb_port_state_t s = typec();
	s.partner = true;
	s.power_role = USB_ROLE_UNKNOWN;
	s.data_role = USB_DATA_UNKNOWN;

	s.partner_secs = 1;
	check("unknown role, still settling", usb_port_decide(&s), USB_PORT_ACT_NONE);

	s.partner_secs = USB_PORT_HANDOVER_SECS;
	check("unknown role, handover due", usb_port_decide(&s), USB_PORT_ACT_BECOME_SINK);
}

// The second failure mode: an overcurrent or a mode mismatch leaves host mode
// behind with nothing on the cable, and the boost is set on the always-powered
// MP2731. Nothing else in userspace turns it off, so the supervisor must.
static void test_stuck_host_role_resets(void) {
	usb_port_state_t s = typec();
	s.data_role = USB_DATA_HOST;
	s.port_dual = false;

	s.stuck_secs = 1;
	check("stuck host role, early", usb_port_decide(&s), USB_PORT_ACT_NONE);

	s.stuck_secs = USB_PORT_STUCK_SECS;
	check("stuck host role, due a reset", usb_port_decide(&s), USB_PORT_ACT_RESET);

	// This kernel never clears data_role on detach: after any host session an
	// idle port still reads [host] while it is dual. That is normal and must
	// not be reset, or every idle port would churn sink/dual for ever.
	s = typec();
	s.data_role = USB_DATA_HOST;
	s.port_dual = true;
	s.stuck_secs = 600;
	check("stale host role, port already dual", usb_port_decide(&s), USB_PORT_ACT_NONE);

	// The boost alone is enough: it can be left on by an earlier session even
	// when the data role has gone back to device.
	s = typec();
	s.otg_on = true;
	s.stuck_secs = USB_PORT_STUCK_SECS;
	check("boost left on, due a reset", usb_port_decide(&s), USB_PORT_ACT_RESET);

	// A host powering the port owns it, and a device on the bus is in use.
	s = typec();
	s.data_role = USB_DATA_HOST;
	s.otg_on = true;
	s.stuck_secs = USB_PORT_STUCK_SECS;
	s.vbus = true;
	check("stuck but powered by the host", usb_port_decide(&s), USB_PORT_ACT_NONE);

	s.vbus = false;
	s.host_device = true;
	check("stuck but a device is on the bus", usb_port_decide(&s), USB_PORT_ACT_NONE);
}

// An empty port is offered dual role again, which is what lets the next pair of
// headphones be seen at all. The caller waits RELEASE_SECS first, so the
// decision itself is only about the state.
static void test_empty_port_offers_dual(void) {
	usb_port_state_t s = typec();
	check("empty, already dual", usb_port_decide(&s), USB_PORT_ACT_NONE);

	s.port_dual = false;
	check("empty, role dropped", usb_port_decide(&s), USB_PORT_ACT_KEEP_DUAL);
}

// The reset the supervisor starts ends with the port back in dual role. Until
// RESET_SECS has passed it is left alone, so the other end has time to see the
// role change instead of being flicked between states.
static void test_reset_holds_then_releases(void) {
	usb_port_state_t s = typec();
	s.reset_pending = true;
	s.reset_secs = 0;
	check("reset just started", usb_port_decide(&s), USB_PORT_ACT_NONE);

	s.reset_secs = USB_PORT_RESET_SECS - 1;
	check("reset still settling", usb_port_decide(&s), USB_PORT_ACT_NONE);

	s.reset_secs = USB_PORT_RESET_SECS;
	check("reset finished", usb_port_decide(&s), USB_PORT_ACT_KEEP_DUAL);

	// The reset outranks an audio device appearing in the window: the port was
	// already being taken down and half a role change is worse than a moment
	// more of silence.
	s.audio = true;
	s.partner = true;
	s.reset_secs = 0;
	check("reset outranks audio", usb_port_decide(&s), USB_PORT_ACT_NONE);
}

int main(void) {
	test_r1_is_always_none();
	test_dac_mode_keeps_hands_off();
	test_audio_keeps_dual();
	test_bus_powered_dac_bootstrap();
	test_phone_hands_over();
	test_charger_settles_sink();
	test_unknown_power_role_falls_back();
	test_stuck_host_role_resets();
	test_empty_port_offers_dual();
	test_reset_holds_then_releases();

	if (failures) {
		fprintf(stderr, "%d USB port decision(s) wrong\n", failures);
		return 1;
	}
	printf("usbport: all decisions as designed\n");
	return 0;
}
