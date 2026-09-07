// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_DIGITAL_STARTUP_H
#define V90_PHASE4_DIGITAL_STARTUP_H

#include "v90_phase4_digital_station.h"
#include "v90_phase4_startup.h"

#include <stdbool.h>
#include <stdint.h>

struct v90_phase4_digital_continuation {
	struct v90_phase4_digital_station station;
	struct v90_phase4_cp_line_rx receive;
};

/* Exact continuous owners at the boundary after the initial CPt/Ri exchange.
 * Neither direction is reset or replayed. */
struct v90_phase4_digital_handoff {
	struct v90_phase4_digital_transmit_handoff transmit;
	struct v90_phase4_cp_line_rx receive;
	uint16_t peer_upstream_rate_mask;
};

union v90_phase4_digital_startup_state {
	struct v90_phase4_digital_station initial;
	struct v90_phase4_digital_continuation continuing;
	struct v90_phase4_digital_handoff complete;
};

/* Once CPt is accepted, later samples remain on the continuous CP receiver
 * while the current Ri/Ri-bar record drains on the digital line. */
struct v90_phase4_digital_startup {
	union v90_phase4_digital_startup_state state;
	struct v90_mp_record initial_mp;
	enum v90_phase4_startup_stage stage;
	bool failed;
};

enum v90_phase4_startup_result v90_phase4_digital_startup_init(
	struct v90_phase4_digital_startup *startup,
	const struct v90_phase4_digital_station *station,
	const struct v90_mp_record *initial_mp);
bool v90_phase4_digital_startup_is_valid(
	const struct v90_phase4_digital_startup *startup);
bool v90_phase4_digital_startup_has_failed(
	const struct v90_phase4_digital_startup *startup);
bool v90_phase4_digital_startup_is_complete(
	const struct v90_phase4_digital_startup *startup);

/* Consume one upstream line sample and produce one simultaneous 8 ksymbol/s
 * G.711 character. State transitions apply to the following character. DONE
 * consumes nothing and
 * leaves output unchanged. A terminal receive error still exposes the output
 * generated for the accepted character. */
enum v90_phase4_startup_result v90_phase4_digital_startup_sample(
	struct v90_phase4_digital_startup *startup,
	int16_t received_sample, uint8_t *transmit_octet);

enum v90_phase4_startup_result v90_phase4_digital_startup_handoff(
	const struct v90_phase4_digital_startup *startup,
	struct v90_phase4_digital_handoff *handoff);

#endif /* V90_PHASE4_DIGITAL_STARTUP_H */
