// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_RATE_PAUSE_H
#define V92_RATE_PAUSE_H

#include "pcm_r.h"
#include "v92_phase4_e2u.h"

enum { V92_RATE_PAUSE_TRN2U_SYMBOLS = 8004 };

/* Own only the duplex signal interval after a pause's E2u. The caller keeps
 * the previous CPd/mapping/receive plan; this interval negotiates none of them.
 * Ed drains independently of TRN2u. Each serialized media interval calls
 * transmit then receive; readiness is observed after both. The next control
 * station takes the exact status transmitter/receiver, including partial
 * records. The enclosing call owns its overall timeout and full retrain. */
struct v92_rate_pause_analog {
	union {
		struct v92_pcm_phase4_trn2u training;
		struct v92_phase4_analog_tx status;
	} upstream;
	struct v92_cpu_parameters cpu;
	struct pcm_downstream_rx downstream;
	struct pcm_r_rx rt;
	bool sending_status;
	bool peer_requested;
	bool failed;
};

struct v92_rate_pause_digital {
	struct v92_upstream_control_line_rx upstream;
	struct pcm_downstream_tx downstream;
	struct pcm_r_tx rt;
	/* Retain the last complete status/zero-rate record for the resumed owner;
	 * a partial following record remains in upstream.control. */
	struct v92_control_event event;
	uint32_t silence_symbols;
	bool peer_requested;
	bool sending_rt;
	bool failed;
};

bool v92_rate_pause_analog_init(struct v92_rate_pause_analog *pause,
	const struct v92_phase4_e2u_handoff *handoff, bool peer_requested);
bool v92_rate_pause_digital_init(struct v92_rate_pause_digital *pause,
	const struct v92_phase4_e2u_handoff *handoff, bool peer_requested);
bool v92_rate_pause_analog_is_valid(const struct v92_rate_pause_analog *pause);
bool v92_rate_pause_digital_is_valid(const struct v92_rate_pause_digital *pause);
bool v92_rate_pause_analog_is_ready(const struct v92_rate_pause_analog *pause);
bool v92_rate_pause_digital_is_ready(const struct v92_rate_pause_digital *pause);

/* Rejected input changes nothing. A processing failure latches failed and
 * leaves output untouched. Observe readiness after each transmit/receive pair:
 * digital receive must still consume the interval carrying the last Rt-bar
 * character. After that boundary, clock the retained control objects directly. */
bool v92_rate_pause_analog_transmit(struct v92_rate_pause_analog *pause, int16_t *sample);
bool v92_rate_pause_analog_receive(struct v92_rate_pause_analog *pause, uint8_t octet);
bool v92_rate_pause_digital_transmit(struct v92_rate_pause_digital *pause, uint8_t *octet);
bool v92_rate_pause_digital_receive(struct v92_rate_pause_digital *pause, int16_t sample);

#endif /* V92_RATE_PAUSE_H */
