// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_TRN1U_LINE_RX_H
#define V92_PCM_PHASE3_TRN1U_LINE_RX_H

#include "binary_equalizer.h"
#include "qam_equalizer.h"
#include "v92_phase3_sync.h"
#include "v92_pcm_phase3_trn1u.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS = 64,
	V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS = 31,
	V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY = 10,
	V92_PCM_PHASE3_TRN1U_LINE_MIN_SIGNAL_LEVEL = 512,
};

/* Training and its Ja/CPt successors retain one equalizer geometry. */
static inline bool v92_pcm_phase3_equalizer_has_geometry(
	const struct qam_equalizer *equalizer)
{
	return equalizer != NULL &&
	       equalizer->tap_count == V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS &&
	       equalizer->decision_delay == V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY;
}

static inline bool v92_pcm_phase3_line_geometry_is_valid(
	const struct binary_equalizer *line)
{
	return binary_equalizer_is_valid(line) &&
	       v92_pcm_phase3_equalizer_has_geometry(&line->equalizer) &&
	       line->magnitude >= V92_PCM_PHASE3_TRN1U_LINE_MIN_SIGNAL_LEVEL;
}

enum v92_pcm_phase3_trn1u_line_rx_stage {
	V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_HUNTING,
	V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_TRAINING,
	V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_COMPLETE,
	V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_FAILED,
};

enum v92_pcm_phase3_trn1u_line_rx_result {
	V92_PCM_PHASE3_TRN1U_LINE_RX_OK,
	V92_PCM_PHASE3_TRN1U_LINE_RX_ALIGNED,
	V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED,
	V92_PCM_PHASE3_TRN1U_LINE_RX_DONE,
	V92_PCM_PHASE3_TRN1U_LINE_RX_FAILED,
	V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT,
	V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_STATE,
	V92_PCM_PHASE3_TRN1U_LINE_RX_NUMERIC_FAILURE,
};

/* Acquire the scrambled bipolar second-TRN1u sequence directly from one
 * signed 8 kHz PCM sample per symbol. The top-level equalizer is reused with
 * zero quadrature; no sample-rate conversion or private history is hidden. */
struct v92_pcm_phase3_trn1u_line_rx {
	struct qam_equalizer equalizer;
	struct v92_phase3_sync sync;
	struct v34_scrambler reference;
	int16_t sync_window[V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS];
	uint8_t expected[V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS];
	uint64_t input_samples;
	uint32_t output_symbols;
	uint32_t training_errors;
	double signal_level;
	double adaptation;
	enum v92_pcm_phase3_trn1u_line_rx_stage stage;
	uint8_t window_count;
	uint8_t window_index;
	int8_t polarity;
};

/* The equalizer has consumed decision_delay samples beyond its last emitted
 * sign. Keeping its history in the handoff prevents CPt from losing those
 * already accepted samples. */
struct v92_pcm_phase3_trn1u_line_handoff {
	struct qam_equalizer equalizer;
	struct v92_phase3_sync sync;
	uint64_t input_samples;
	uint32_t training_errors;
	double signal_level;
	double adaptation;
	uint8_t input_advance_symbols;
	int8_t polarity;
};

enum v92_pcm_phase3_trn1u_line_rx_result
v92_pcm_phase3_trn1u_line_rx_init(
	struct v92_pcm_phase3_trn1u_line_rx *receiver,
	double adaptation);
bool v92_pcm_phase3_trn1u_line_rx_is_valid(
	const struct v92_pcm_phase3_trn1u_line_rx *receiver);
bool v92_pcm_phase3_trn1u_line_rx_is_complete(
	const struct v92_pcm_phase3_trn1u_line_rx *receiver);

/* Consume exactly one signed sample. Terminal DSP failure consumes the sample
 * and latches failure; invalid, complete, and failed calls consume nothing. */
enum v92_pcm_phase3_trn1u_line_rx_result
v92_pcm_phase3_trn1u_line_rx_sample(
	struct v92_pcm_phase3_trn1u_line_rx *receiver,
	int16_t sample);

bool v92_pcm_phase3_trn1u_line_handoff_is_valid(
	const struct v92_pcm_phase3_trn1u_line_handoff *handoff);
enum v92_pcm_phase3_trn1u_line_rx_result
v92_pcm_phase3_trn1u_line_rx_handoff(
	const struct v92_pcm_phase3_trn1u_line_rx *receiver,
	struct v92_pcm_phase3_trn1u_line_handoff *handoff);

const char *v92_pcm_phase3_trn1u_line_rx_result_name(
	enum v92_pcm_phase3_trn1u_line_rx_result result);

#endif /* V92_PCM_PHASE3_TRN1U_LINE_RX_H */
