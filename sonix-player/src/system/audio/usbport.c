#include "usbport.h"

// A partner that is not a sound card is given USB_PORT_HANDOVER_SECS to become
// one, and then the port is handed over: this is the phone case, and also the
// charger and PC case once they have settled the port as sink.
static usb_port_action_t handover(const usb_port_state_t *s) {
	return s->partner_secs >= USB_PORT_HANDOVER_SECS ? USB_PORT_ACT_BECOME_SINK : USB_PORT_ACT_NONE;
}

usb_port_action_t usb_port_decide(const usb_port_state_t *s) {
	// The R1's TCS1421 has no Type-C class and no partner node, and its rules
	// already work there. Taking them over here would change a port with no
	// way to test it, so the whole table belongs to the R3.
	if (s->kind != USB_PORT_TYPEC) {
		return USB_PORT_ACT_NONE;
	}

	// A reset already under way owns the port until it has had time to land.
	if (s->reset_pending) {
		return s->reset_secs < USB_PORT_RESET_SECS ? USB_PORT_ACT_NONE : USB_PORT_ACT_KEEP_DUAL;
	}

	// The player is the USB sound card and the cable belongs to the PC. Do not
	// touch the role underneath the stream. The boost is not the role: one
	// left on by an earlier fault must not keep 5 V on an empty port, or feed
	// it back into the PC that is now on the other end.
	if (s->dac_mode) {
		if (s->otg_on && (s->vbus || (!s->partner && !s->host_device))) {
			return USB_PORT_ACT_BOOST_OFF;
		}
		return USB_PORT_ACT_NONE;
	}

	// A sound card is on the bus, so the port is doing its job. Keep dual role
	// asserted: the kernel drops it when the gadget is bound and unbound.
	if (s->audio) {
		return USB_PORT_ACT_KEEP_DUAL;
	}

	if (s->partner) {
		if (s->power_role == USB_ROLE_SOURCE) {
			// Source with data role device is deliberately not treated as a
			// stuck port: a PD power-role swap and a data-role swap both
			// leave exactly that (this player sources, the partner is the
			// data host), and it is a working session.
			//
			// A non-audio device enumerated on this player's host bus: a phone
			// has taken the host role and must be given the card, which it can
			// only have as the host end of the cable.
			if (s->host_device) {
				return handover(s);
			}

			// The negotiated source role is already right; what may be missing
			// is the 5 V. A bus-powered DAC cannot enumerate without it, so
			// this is not a handover case however long it takes.
			if (!s->otg_on) {
				return USB_PORT_ACT_BOOST_ON;
			}

			// Sourced for a while and still nothing: drop the boost for one
			// poll and let the caller turn it back on, giving the device the
			// power edge it may be waiting for. Bounded, because a shorted
			// cable would otherwise be pulsed every few seconds for ever.
			if (s->source_secs >= USB_PORT_RECOVER_SECS && s->boost_cycles < USB_PORT_MAX_BOOST_CYCLES) {
				return USB_PORT_ACT_BOOST_OFF;
			}
			return USB_PORT_ACT_NONE;
		}

		// SINK: a charger, a PC, or a phone acting as the source. The old rule
		// still fits: this player is the peripheral and the port stays as it
		// is. UNKNOWN takes the same path, so a kernel that does not publish
		// the role cannot leave the port pinned.
		return handover(s);
	}

	// Nothing on the port.
	if (s->vbus || s->host_device) {
		// A host is powering the port, or a device is already on the bus. The
		// gadget path owns this state; do not pull the role out from under it.
		return USB_PORT_ACT_NONE;
	}
	if (s->otg_on || (s->data_role == USB_DATA_HOST && !s->port_dual)) {
		// Host mode with nothing attached. After an overcurrent or a mode
		// mismatch the controller can be left here, and so can the boost on
		// the always-powered MP2731. Nothing else in userspace clears it.
		//
		// A data role of host alone is not that state: this kernel sets it on
		// every source attach and never clears it on detach, so after any USB
		// DAC, headphone or phone session an idle dual port still reads
		// "[host]". Resetting on that would put an otherwise healthy port
		// through sink/dual every eight seconds for ever. Only a port that is
		// still not dual (a sink write that did not get back, or a reset in
		// progress) or a boost left on is genuinely stuck.
		return s->stuck_secs >= USB_PORT_STUCK_SECS ? USB_PORT_ACT_RESET : USB_PORT_ACT_NONE;
	}

	// Empty and quiet: offer dual role again, which is what lets the next pair
	// of headphones be seen at all. The caller has already waited
	// RELEASE_SECS, so the delay is not repeated here.
	return s->port_dual ? USB_PORT_ACT_NONE : USB_PORT_ACT_KEEP_DUAL;
}
