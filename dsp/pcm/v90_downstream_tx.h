// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DOWNSTREAM_TX_H
#define V90_DOWNSTREAM_TX_H

#include "v34_framing.h"
#include "v90_pcm_frame_encoder.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_downstream_tx_result {
	V90_DOWNSTREAM_TX_OK,
	V90_DOWNSTREAM_TX_COMPLETE,
	V90_DOWNSTREAM_TX_INVALID_ARGUMENT,
	V90_DOWNSTREAM_TX_INVALID_PARAMETERS,
	V90_DOWNSTREAM_TX_INVALID_STATE,
	V90_DOWNSTREAM_TX_OUT_OF_RANGE,
	V90_DOWNSTREAM_TX_NUMERIC_FAILURE,
	V90_DOWNSTREAM_TX_INTERNAL_ERROR,
};

/* One recordable 8 ksymbol/s output. A shaping rule is repeated across its
 * complete shaping frame; shaping_frame_start marks the exact boundary. */
struct v90_downstream_symbol {
	struct v90_pcm_symbol pcm;
	enum v90_spectral_rule shaping_rule;
	bool shaping_rule_valid;
	bool shaping_frame_start;
};

/* TRN2d owns zero-initialized GPC scrambling, mapping, and a bounded six-
 * symbol output queue. accepted_frames may lead transmitted_symbols only by
 * the negotiated look-ahead plus the current queue. */
struct v90_downstream_tx {
	struct v90_pcm_frame_encoder encoder;
	struct v34_scrambler scrambler;
	struct v90_downstream_symbol queue[V90_CP_FRAME_INTERVALS];
	uint64_t accepted_frames;
	uint64_t transmitted_symbols;
	uint8_t queue_count;
	bool finish_requested;
	bool complete;
};

enum v90_downstream_tx_result v90_downstream_tx_init(
	struct v90_downstream_tx *transmitter,
	const struct v90_pcm_mapping *training_mapping);
bool v90_downstream_tx_is_valid(const struct v90_downstream_tx *transmitter);
bool v90_downstream_tx_is_complete(const struct v90_downstream_tx *transmitter);

/* End after every already accepted six-symbol frame has drained. The request
 * is idempotent and never truncates a shaping look-ahead window. */
enum v90_downstream_tx_result v90_downstream_tx_request_finish(
	struct v90_downstream_tx *transmitter);

/* Produce exactly one network symbol. COMPLETE consumes no symbol and leaves
 * output unchanged. Rejected calls leave both state and output unchanged. */
enum v90_downstream_tx_result v90_downstream_tx_next(
	struct v90_downstream_tx *transmitter,
	struct v90_downstream_symbol *symbol);

const char *v90_downstream_tx_result_name(
	enum v90_downstream_tx_result result);

#endif /* V90_DOWNSTREAM_TX_H */
