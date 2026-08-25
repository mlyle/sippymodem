// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_EXCHANGE_H
#define V34_PHASE3_EXCHANGE_H

#include "v34_phase34_receiver.h"
#include "v34_phase3_line_transmitter.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE3_ANSWER_SILENCE_MS = 70,
};

enum v34_phase3_exchange_result {
	V34_PHASE3_EXCHANGE_OK,
	V34_PHASE3_EXCHANGE_FAILED,
	V34_PHASE3_EXCHANGE_INVALID_ARGUMENT,
	V34_PHASE3_EXCHANGE_INVALID_STATE,
	V34_PHASE3_EXCHANGE_OUT_OF_RANGE,
	V34_PHASE3_EXCHANGE_CLIPPING_RISK,
	V34_PHASE3_EXCHANGE_SEQUENCE_MISMATCH,
	V34_PHASE3_EXCHANGE_NUMERIC_FAILURE,
};

enum v34_phase3_exchange_event {
	V34_PHASE3_EXCHANGE_EVENT_NONE = 0,
	V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STARTED = 1u << 0,
	V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_J_STARTED = 1u << 1,
	V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STOPPED = 1u << 2,
	V34_PHASE3_EXCHANGE_EVENT_PEER_TIMING_ACQUIRED = 1u << 3,
	V34_PHASE3_EXCHANGE_EVENT_PEER_S_DETECTED = 1u << 4,
	V34_PHASE3_EXCHANGE_EVENT_PEER_MD_COMPLETE = 1u << 5,
	V34_PHASE3_EXCHANGE_EVENT_PEER_REPEATED_S_DETECTED = 1u << 6,
	V34_PHASE3_EXCHANGE_EVENT_PEER_PP_COMPLETE = 1u << 7,
	V34_PHASE3_EXCHANGE_EVENT_PEER_J_DETECTED = 1u << 8,
	V34_PHASE3_EXCHANGE_EVENT_HANDOFF_READY = 1u << 9,
};

struct v34_phase3_exchange_configuration {
	enum v34_constellation_size requested_remote_constellation;
	uint32_t sample_rate;
	int16_t nominal_peak_amplitude;
};

/* Exact full-duplex sample boundary after both Phase 3 J selections. The
 * caller's transmitter is still sending J; the answerer's line is advancing
 * through zero points after stopping J. Both directions retain their complete
 * waveform state, and the next owner must continue only its copied
 * instances. */
struct v34_phase3_exchange_handoff {
	struct v34_training_line transmit_line;
	struct v34_phase3_transmitter phase3_transmit;
	struct v34_phase34_receiver receive;
};

bool v34_phase3_exchange_handoff_is_valid(
	const struct v34_phase3_exchange_handoff *handoff);

/* Own both directions of one endpoint's zero-local-MD Phase 3 exchange.
 * Answering transmission begins after the required 70 ms silence and retains
 * TRN through the complete two-second zero-RTD line-training window. Calling
 * transmission begins after peer J detection and uses the required 512T
 * minimum. The following Phase 4 owner decides when the caller may terminate
 * J. Calls on one instance must be serialized. */
struct v34_phase3_exchange {
	struct v34_phase3_line_transmitter transmit;
	struct v34_phase34_receiver receive;
	uint32_t trn_target_symbols;
	uint32_t answer_silence_samples;
	uint32_t silence_samples_remaining;
	enum v34_station_role local_role;
	bool transmit_started;
	bool stop_transmit_requested;
	bool transmit_stopped;
	bool handoff_announced;
	bool failed;
};

enum v34_phase3_exchange_result v34_phase3_exchange_init(
	struct v34_phase3_exchange *exchange,
	const struct v34_phase3_profile *local_transmit,
	const struct v34_phase3_profile *peer_transmit,
	const struct v34_phase3_exchange_configuration *configuration,
	int16_t initial_receive_gain);

bool v34_phase3_exchange_is_valid(
	const struct v34_phase3_exchange *exchange);
bool v34_phase3_exchange_has_failed(
	const struct v34_phase3_exchange *exchange);
bool v34_phase3_exchange_is_handoff_ready(
	const struct v34_phase3_exchange *exchange);

/* Consume and produce one simultaneous line sample. Events are a bitmask so
 * causally independent receive and transmit boundaries cannot hide each
 * other. Successful calls always initialize both outputs. A terminal receive
 * error still commits the consumed sample and generated transmit sample. The
 * two output objects must not overlap. */
enum v34_phase3_exchange_result v34_phase3_exchange_sample(
	struct v34_phase3_exchange *exchange, int16_t received_sample,
	int16_t *transmit_sample, uint32_t *events);

enum v34_phase3_exchange_result v34_phase3_exchange_handoff(
	const struct v34_phase3_exchange *exchange,
	struct v34_phase3_exchange_handoff *handoff);

#endif /* V34_PHASE3_EXCHANGE_H */
