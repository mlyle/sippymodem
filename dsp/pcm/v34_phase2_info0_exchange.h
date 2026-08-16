// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_INFO0_EXCHANGE_H
#define V34_PHASE2_INFO0_EXCHANGE_H

#include "phase2_info0_exchange.h"
#include "v34_info0.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE2_POST_V8_SILENCE_SAMPLES =
		PHASE2_INFO0_POST_V8_SILENCE_SAMPLES,
};

enum v34_phase2_info0_exchange_result {
	V34_PHASE2_INFO0_EXCHANGE_OK,
	V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
};

enum v34_phase2_peer_info0_event {
	V34_PHASE2_PEER_INFO0_NO_EVENT,
	V34_PHASE2_PEER_INFO0_RECEIVED,
	V34_PHASE2_PEER_INFO0_REJECTED,
};

struct v34_phase2_info0_exchange_event {
	enum v34_phase2_peer_info0_event peer_info0_event;
	struct v34_info0 peer_info0;
	struct v34_phase2_tone_event tone_event;
	bool recovery_started;
	bool recovery_completed;
};

struct v34_phase2_info0_exchange {
	struct phase2_info0_exchange transport;
	struct v34_info0 local_info0;
	struct v34_info0 peer_info0;
	enum v34_station_role local_role;
	bool peer_info0_received;
	bool recovery_requested;
	bool recovery_active;
	bool transmit_acknowledges_peer;
	bool acknowledgements_complete;
	bool completion_tone_seen;
};

/* Initial INFO0 always clears its acknowledgment bit.  Correct peer INFO0 is
 * acknowledged by phase-continuous recovery repetitions when the
 * peer repeats INFO0 or sends its following tone before a valid frame. */
enum v34_phase2_info0_exchange_result
v34_phase2_info0_exchange_init(
	struct v34_phase2_info0_exchange *exchange,
	enum v34_station_role local_role,
	int16_t nominal_amplitude,
	const struct v34_info0 *local_info0);

/* Full retrain preserves the completed INFO0 agreement and begins directly
 * with the role tone after the caller-owned silence interval. */
enum v34_phase2_info0_exchange_result
v34_phase2_info0_exchange_init_retrain(
	struct v34_phase2_info0_exchange *exchange,
	enum v34_station_role local_role, int16_t nominal_amplitude,
	const struct v34_info0 *local_info0,
	const struct v34_info0 *peer_info0);
bool v34_phase2_info0_exchange_is_valid(
	const struct v34_phase2_info0_exchange *exchange);

/* One call consumes and produces one 8 kHz line sample. INFO0 and tone events
 * are independent and may both be populated by the same sample. */
enum v34_phase2_info0_exchange_result
v34_phase2_info0_exchange_sample(
	struct v34_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_info0_exchange_event *event);

const char *v34_phase2_info0_exchange_result_name(
	enum v34_phase2_info0_exchange_result result);

#endif
