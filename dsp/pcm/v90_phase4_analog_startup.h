// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_ANALOG_STARTUP_H
#define V90_PHASE4_ANALOG_STARTUP_H

#include "v90_phase4_analog_station.h"
#include "v90_phase4_startup.h"

#include <stdbool.h>
#include <stdint.h>

struct v90_phase4_analog_continuation {
	struct v90_phase4_analog_station station;
	struct pcm_downstream_rx receive;
	bool receive_ready;
};

/* Exact continuous owners at the boundary after the initial CPt/Ri exchange.
 * Neither direction is reset or replayed. */
struct v90_phase4_analog_handoff {
	struct v90_phase4_cp_line_tx transmit;
	struct pcm_downstream_rx receive;
	enum v34_constellation_size rate_renegotiation_constellation;
};

union v90_phase4_analog_startup_state {
	struct v90_phase4_analog_station initial;
	struct v90_phase4_analog_continuation continuing;
	struct v90_phase4_analog_handoff complete;
};

/* Whichever side finishes first remains owned here. After CPt, optional SCR
 * keeps the conventional stream continuous until Ri-bar finishes; after
 * Ri-bar, later characters enter the downstream decoder while CPt drains. */
struct v90_phase4_analog_startup {
	union v90_phase4_analog_startup_state state;
	enum v90_phase4_startup_stage stage;
	bool failed;
};

enum v90_phase4_startup_result v90_phase4_analog_startup_init(
	struct v90_phase4_analog_startup *startup,
	const struct v90_phase4_analog_station *station);
bool v90_phase4_analog_startup_is_valid(
	const struct v90_phase4_analog_startup *startup);
bool v90_phase4_analog_startup_has_failed(
	const struct v90_phase4_analog_startup *startup);
bool v90_phase4_analog_startup_is_complete(
	const struct v90_phase4_analog_startup *startup);

/* Consume and produce one simultaneous 8 ksymbol/s G.711 character. State
 * transitions apply to the following character. DONE consumes nothing and
 * leaves output unchanged. A terminal receive error still exposes the output
 * generated for the accepted character. */
enum v90_phase4_startup_result v90_phase4_analog_startup_octet(
	struct v90_phase4_analog_startup *startup,
	uint8_t received_octet, uint8_t *transmit_octet);

enum v90_phase4_startup_result v90_phase4_analog_startup_handoff(
	const struct v90_phase4_analog_startup *startup,
	struct v90_phase4_analog_handoff *handoff);

#endif /* V90_PHASE4_ANALOG_STARTUP_H */
