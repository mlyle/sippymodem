// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE4_TRN2U_LINE_RX_H
#define V92_PCM_PHASE4_TRN2U_LINE_RX_H

#include "v92_pcm_phase3_cpt_line_rx.h"
#include "v92_pcm_phase4_trn2u.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS = 61,
	V92_PCM_PHASE4_TRN2U_LINE_MAX_LEVELS = 8,
	V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY =
		V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY,
};

enum v92_pcm_phase4_trn2u_line_rx_result {
	V92_PCM_PHASE4_TRN2U_LINE_RX_OK,
	V92_PCM_PHASE4_TRN2U_LINE_RX_ALIGNED,
	V92_PCM_PHASE4_TRN2U_LINE_RX_FAILED,
	V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT,
	V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_STATE,
	V92_PCM_PHASE4_TRN2U_LINE_RX_NUMERIC_FAILURE,
};

/* Continue the analogue-modem line from E1u into initial TRN2u. The reference
 * is kept one equalizer delay ahead of trained_symbols because those physical
 * inputs have already entered the inherited FIR. The longer Phase-4 span adds
 * zeroed postcursor taps without moving the trained decision point. */
struct v92_pcm_phase4_trn2u_line_rx {
	struct qam_equalizer equalizer;
	struct v92_pcm_phase4_trn2u reference;
	int16_t expected[
		V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY];
	int16_t levels[V92_PCM_PHASE4_TRN2U_LINE_MAX_LEVELS];
	uint64_t starting_input_samples;
	uint64_t input_samples;
	uint32_t trained_symbols;
	uint32_t symbol_errors;
	double squared_error_sum;
	double adaptation;
	enum g711_law law;
	uint8_t expected_index;
	uint8_t level_count;
	int8_t polarity;
	bool failed;
};

/* The receive-side LU scale is the measured TRN1u level retained by handoff.
 * constellation is the initial training choice from Jp. */
enum v92_pcm_phase4_trn2u_line_rx_result
v92_pcm_phase4_trn2u_line_rx_init(
	struct v92_pcm_phase4_trn2u_line_rx *receiver,
	const struct v92_pcm_phase3_cpt_line_handoff *handoff,
	enum v92_constellation_size constellation);
bool v92_pcm_phase4_trn2u_line_rx_is_valid(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver);
bool v92_pcm_phase4_trn2u_line_rx_can_handoff(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver);
bool v92_pcm_phase4_trn2u_line_rx_has_failed(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver);

/* Consume one signed 8 kHz sample. ALIGNED marks each twelve-symbol boundary
 * at or after the 12000-symbol initial minimum. Internal DSP failure latches
 * without consuming the sample; all rejected calls are transactional. */
enum v92_pcm_phase4_trn2u_line_rx_result
v92_pcm_phase4_trn2u_line_rx_sample(
	struct v92_pcm_phase4_trn2u_line_rx *receiver, int16_t sample);

const char *v92_pcm_phase4_trn2u_line_rx_result_name(
	enum v92_pcm_phase4_trn2u_line_rx_result result);

#endif /* V92_PCM_PHASE4_TRN2U_LINE_RX_H */
