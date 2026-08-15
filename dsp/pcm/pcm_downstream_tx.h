// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_DOWNSTREAM_TX_H
#define PCM_DOWNSTREAM_TX_H

#include "v34_framing.h"
#include "v90_mp.h"
#include "v90_pcm_frame_encoder.h"
#include "v90_phase4.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum pcm_downstream_tx_result {
	PCM_DOWNSTREAM_TX_OK,
	PCM_DOWNSTREAM_TX_COMPLETE,
	PCM_DOWNSTREAM_TX_INVALID_ARGUMENT,
	PCM_DOWNSTREAM_TX_INVALID_PARAMETERS,
	PCM_DOWNSTREAM_TX_INVALID_STATE,
	PCM_DOWNSTREAM_TX_OUT_OF_RANGE,
	PCM_DOWNSTREAM_TX_NUMERIC_FAILURE,
	PCM_DOWNSTREAM_TX_INTERNAL_ERROR,
};

/* One recordable 8 ksymbol/s output. A shaping rule is repeated across its
 * complete shaping frame; shaping_frame_start marks the exact boundary. */
struct pcm_downstream_symbol {
	struct v90_pcm_symbol pcm;
	enum v90_spectral_rule shaping_rule;
	bool shaping_rule_valid;
	bool shaping_frame_start;
};

enum pcm_downstream_tx_stage {
	PCM_DOWNSTREAM_TX_TRN2D,
	PCM_DOWNSTREAM_TX_MP,
	PCM_DOWNSTREAM_TX_MP_PRIME,
	PCM_DOWNSTREAM_TX_ED,
};

/* Own continuous TRN2d, MP, and Ed transmission with zero-initialized GPC
 * scrambling, PCM coding, and a bounded six-symbol output queue. Source
 * generation—and therefore stage—may lead network output by the negotiated
 * shaping look-ahead. */
struct pcm_downstream_tx {
	struct v90_pcm_frame_encoder encoder;
	struct v34_scrambler scrambler;
	struct pcm_downstream_symbol queue[V90_CP_FRAME_INTERVALS];
	struct v90_mp_record mp_record;
	uint8_t mp_sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint64_t accepted_frames;
	uint64_t transmitted_symbols;
	size_t mp_sequence_bits;
	size_t mp_bit_index;
	enum pcm_downstream_tx_stage stage;
	uint8_t queue_count;
	uint8_t ed_frames;
	bool mp_configured;
	bool mp_prime_requested;
	bool mp_prime_record_accepted;
	bool ed_requested;
	bool finish_requested;
	bool complete;
};

enum pcm_downstream_tx_result pcm_downstream_tx_init(
	struct pcm_downstream_tx *transmitter,
	const struct v90_pcm_mapping *training_mapping);
bool pcm_downstream_tx_is_valid(const struct pcm_downstream_tx *transmitter);
bool pcm_downstream_tx_is_complete(const struct pcm_downstream_tx *transmitter);

/* Queue immutable MP parameters. Acknowledgement must be clear; MP begins at
 * the first source-frame boundary after at least 2040 TRN2d symbols have been
 * accepted. Repeating an identical request is harmless. */
enum pcm_downstream_tx_result pcm_downstream_tx_start_mp(
	struct pcm_downstream_tx *transmitter,
	const struct v90_mp_record *record);

/* Request MP prime. A request during TRN2d preserves one complete MP record;
 * a request during MP takes effect at its next padded record boundary. */
enum pcm_downstream_tx_result pcm_downstream_tx_request_mp_prime(
	struct pcm_downstream_tx *transmitter);

/* Request Ed after MP prime has been selected. At least one complete MP-prime
 * source record is retained; a mid-record request finishes that record. Ed
 * then contributes exactly two zero-source frames to the continuous training
 * coder before its delayed spectral output is drained. */
enum pcm_downstream_tx_result pcm_downstream_tx_request_ed(
	struct pcm_downstream_tx *transmitter);

/* End after every already accepted six-symbol frame has drained. The request
 * is idempotent and never truncates a shaping look-ahead window. Once MP is
 * configured, the later Ed transition owns termination instead. */
enum pcm_downstream_tx_result pcm_downstream_tx_request_finish(
	struct pcm_downstream_tx *transmitter);

/* Produce exactly one network symbol. COMPLETE consumes no symbol and leaves
 * output unchanged. Rejected calls leave both state and output unchanged. */
enum pcm_downstream_tx_result pcm_downstream_tx_next(
	struct pcm_downstream_tx *transmitter,
	struct pcm_downstream_symbol *symbol);

const char *pcm_downstream_tx_result_name(
	enum pcm_downstream_tx_result result);

#endif /* PCM_DOWNSTREAM_TX_H */
