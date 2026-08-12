// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_EXCHANGE_H
#define V34_PHASE4_EXCHANGE_H

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
};

struct v34_phase4_exchange_configuration {
	struct v34_training_receiver_configuration receive;
};

/* Own both sample directions from the exact Phase 3 handoff through the
 * minimum Phase 4 TRN interval. The caller ends J only after receiving the
 * answerer's complete S/S-bar marker and reaching a J-pattern boundary.
 * Calls on one instance must be serialized. */
struct v34_phase4_exchange {
	struct v34_training_transmitter transmit;
	struct v34_training_receiver receive;
	enum v34_station_role local_role;
	bool finish_j_requested;
	bool transmit_trn_started;
	bool transmit_trn_ready_announced;
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

/* Consume and produce one simultaneous line sample. Receive events affect the
 * next unconsumed transmit symbol. Terminal receive errors still commit the
 * consumed input, generated output, and event bits. */
enum v34_phase4_exchange_result v34_phase4_exchange_sample(
	struct v34_phase4_exchange *exchange, int16_t received_sample,
	int16_t *transmit_sample, uint32_t *events);

const char *v34_phase4_exchange_result_name(
	enum v34_phase4_exchange_result result);

#endif /* V34_PHASE4_EXCHANGE_H */
