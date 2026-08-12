// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_EXCHANGE_H
#define V34_PHASE4_EXCHANGE_H

#include "v34_data_startup.h"
#include "v34_data_stream.h"
#include "v34_mp.h"
#include "v34_phase3_exchange.h"
#include "v34_training_receiver.h"
#include "v34_training_transmitter.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase4_exchange_result {
	V34_PHASE4_EXCHANGE_OK,
	V34_PHASE4_EXCHANGE_SOURCE_FAILURE,
	V34_PHASE4_EXCHANGE_FAILED,
	V34_PHASE4_EXCHANGE_INVALID_ARGUMENT,
	V34_PHASE4_EXCHANGE_INVALID_STATE,
	V34_PHASE4_EXCHANGE_NO_COMMON_RATE,
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
	V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_E_STARTED = 1u << 9,
	V34_PHASE4_EXCHANGE_EVENT_PEER_E_RECEIVED = 1u << 10,
	V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_B1_STARTED = 1u << 11,
	V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_B1_COMPLETE = 1u << 12,
	V34_PHASE4_EXCHANGE_EVENT_PEER_B1_RECEIVED = 1u << 13,
	V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED = 1u << 14,
	V34_PHASE4_EXCHANGE_EVENT_ONLINE = 1u << 15,
};

struct v34_phase4_exchange_configuration {
	struct v34_training_receiver_configuration receive;
	/* Supply an ordinary MP. The exchange sets the acknowledgment bit only
	 * after receiving a valid peer record. */
	struct v34_mp_record transmit_mp;
	double data_equalizer_adaptation;
};

union v34_phase4_transmit_path {
	struct v34_training_transmitter training;
	struct v34_data_stream data;
};

union v34_phase4_receive_path {
	struct v34_training_receiver training;
	struct v34_data_line_receiver data;
};

/* Own both sample directions from the exact Phase 3 handoff through the
 * reciprocal MP-prime, E, and B1 exchanges and into mapped data. The
 * caller ends J only after receiving the answerer's complete S/S-bar marker
 * and reaching a J-pattern boundary. Receipt of the first valid peer MP pauses
 * the sample path until prepare_data_startup() performs the negotiated mapping
 * calibration outside the real-time callback. A valid final peer MP-prime
 * record arms E reception at that exact record boundary. Transmit and receive
 * enter B1 independently, retaining early mapped data in the sample-call
 * outputs. Calls on one instance must be serialized. Each tagged directional
 * union reuses its training storage for the post-E data owner. */
struct v34_phase4_exchange {
	union v34_phase4_transmit_path transmit;
	union v34_phase4_receive_path receive;
	struct v34_data_startup data_startup;
	struct v34_mp_record transmit_mp;
	struct v34_mp_record peer_mp;
	double data_equalizer_adaptation;
	enum v34_station_role local_role;
	bool finish_j_requested;
	bool transmit_trn_started;
	bool transmit_trn_ready_announced;
	bool transmit_mp_started;
	bool transmit_mp_prime_started;
	bool peer_mp_received;
	bool peer_mp_prime_received;
	bool data_preparation_required;
	bool data_startup_prepared;
	bool transmit_data_active;
	bool receive_data_active;
	bool transmit_b1_complete;
	bool receive_b1_complete;
	bool ready_announced;
	bool online_announced;
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
bool v34_phase4_exchange_needs_data_preparation(
	const struct v34_phase4_exchange *exchange);

/* Resolve the pending first peer MP, selecting and calibrating both negotiated
 * data directions. Rejected calls leave the exchange unchanged. A terminal
 * preparation failure is latched because the paused line sample cannot be
 * skipped or replayed. */
enum v34_phase4_exchange_result v34_phase4_exchange_prepare_data_startup(
	struct v34_phase4_exchange *exchange);

/* True once both acknowledgments were committed and both post-E paths were
 * prepared. The milestone remains observable after E has begun. */
bool v34_phase4_exchange_is_ready_for_e(
	const struct v34_phase4_exchange *exchange);
bool v34_phase4_exchange_is_online(
	const struct v34_phase4_exchange *exchange);

/* Copy the prepared post-E mapping and receive configuration atomically. */
bool v34_phase4_exchange_get_data_startup(
	const struct v34_phase4_exchange *exchange,
	struct v34_data_startup *startup);

/* Copy the first accepted peer record with its acknowledgment bit cleared.
 * A rejected call leaves the destination unchanged. */
bool v34_phase4_exchange_get_peer_mp(
	const struct v34_phase4_exchange *exchange,
	struct v34_mp_record *record);

/* Consume and produce one simultaneous line sample. `source` supplies local
 * post-B1 mapping frames; NULL deliberately emits idle ones. `received` changes
 * only with MAPPING_FRAME_RECEIVED and never exposes the peer's B1 frame.
 * Calls while data preparation is required return INVALID_STATE without
 * consuming or producing anything. Receive events affect the next unconsumed
 * transmit symbol.
 * SOURCE_FAILURE consumes neither the input nor any output. Terminal receive
 * errors still commit the consumed input, generated output, and event bits. */
enum v34_phase4_exchange_result v34_phase4_exchange_sample(
	struct v34_phase4_exchange *exchange, int16_t received_sample,
	v34_mapping_frame_source source, void *source_context,
	int16_t *transmit_sample, uint32_t *events,
	struct v34_received_mapping_frame *received);

const char *v34_phase4_exchange_result_name(
	enum v34_phase4_exchange_result result);

#endif /* V34_PHASE4_EXCHANGE_H */
