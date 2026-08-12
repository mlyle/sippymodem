// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_EXCHANGE_H
#define V34_PHASE4_EXCHANGE_H

#include "v34_mp.h"
#include "v34_phase3_exchange.h"
#include "v34_training_receiver.h"
#include "v34_training_transmitter.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase4_exchange_result {
	V34_PHASE4_EXCHANGE_OK,
	V34_PHASE4_EXCHANGE_FAILED,
	V34_PHASE4_EXCHANGE_INVALID_ARGUMENT,
	V34_PHASE4_EXCHANGE_OUT_OF_RANGE,
	V34_PHASE4_EXCHANGE_CLIPPING_RISK,
	V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH,
	V34_PHASE4_EXCHANGE_NUMERIC_FAILURE,
};

enum v34_phase4_exchange_event {
	V34_PHASE4_EXCHANGE_EVENT_NONE = 0,
	V34_PHASE4_EXCHANGE_EVENT_PEER_S_DETECTED = 1u << 0,
	V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_J_FINISHED = 1u << 1,
	V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_STARTED = 1u << 2,
	V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_READY = 1u << 3,
	V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_STARTED = 1u << 4,
	V34_PHASE4_EXCHANGE_EVENT_PEER_MP_RECEIVED = 1u << 5,
	V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_PRIME_STARTED = 1u << 6,
	V34_PHASE4_EXCHANGE_EVENT_PEER_MP_PRIME_RECEIVED = 1u << 7,
	V34_PHASE4_EXCHANGE_EVENT_READY_FOR_E = 1u << 8,
};

struct v34_phase4_exchange_configuration {
	struct v34_training_receiver_configuration receive;
	/* Supply an ordinary MP. The exchange sets the acknowledgment bit only
	 * after receiving a valid peer record. */
	struct v34_mp_record transmit_mp;
};

/* Own both sample directions from the exact Phase 3 handoff through the
 * reciprocal MP-prime handshake. The caller ends J only after receiving the
 * answerer's complete S/S-bar marker and reaching a J-pattern boundary.
 * Calls on one instance must be serialized. */
struct v34_phase4_exchange {
	struct v34_training_transmitter transmit;
	struct v34_training_receiver receive;
	struct v34_mp_record transmit_mp;
	struct v34_mp_record peer_mp;
	enum v34_station_role local_role;
	bool finish_j_requested;
	bool transmit_trn_started;
	bool transmit_trn_ready_announced;
	bool transmit_mp_started;
	bool transmit_mp_prime_started;
	bool peer_mp_received;
	bool peer_mp_prime_received;
	bool ready_announced;
	bool failed;
};

enum v34_phase4_exchange_result v34_phase4_exchange_init(
	struct v34_phase4_exchange *exchange,
	const struct v34_phase3_exchange_handoff *handoff,
	const struct v34_phase4_exchange_configuration *configuration);

bool v34_phase4_exchange_is_valid(
	const struct v34_phase4_exchange *exchange);
bool v34_phase4_exchange_has_failed(
	const struct v34_phase4_exchange *exchange);
bool v34_phase4_exchange_is_trn_ready(
	const struct v34_phase4_exchange *exchange);
/* Both acknowledgments are committed. E must still begin at a complete local
 * MP-prime record boundary, which the following controller slice will own. */
bool v34_phase4_exchange_is_ready_for_e(
	const struct v34_phase4_exchange *exchange);

/* Copy the first accepted peer record with its acknowledgment bit cleared.
 * A rejected call leaves the destination unchanged. */
bool v34_phase4_exchange_get_peer_mp(
	const struct v34_phase4_exchange *exchange,
	struct v34_mp_record *record);

/* Consume and produce one simultaneous line sample. Receive events affect the
 * next unconsumed transmit symbol. Terminal receive errors still commit the
 * consumed input, generated output, and event bits. */
enum v34_phase4_exchange_result v34_phase4_exchange_sample(
	struct v34_phase4_exchange *exchange, int16_t received_sample,
	int16_t *transmit_sample, uint32_t *events);

const char *v34_phase4_exchange_result_name(
	enum v34_phase4_exchange_result result);

#endif /* V34_PHASE4_EXCHANGE_H */
