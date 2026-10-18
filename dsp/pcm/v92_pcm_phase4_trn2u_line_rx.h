// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE4_TRN2U_LINE_RX_H
#define V92_PCM_PHASE4_TRN2U_LINE_RX_H

#include "v92_pcm_phase3_cpt_line_rx.h"
#include "v92_pcm_phase4_trn2u.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS = 61,
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

/* Continue the analogue-modem line from E1u or Ru-bar into TRN2u. The reference
 * is kept one equalizer delay ahead of trained_symbols because those physical
 * inputs have already entered the inherited FIR. Initial Phase 4 adds zeroed
 * postcursor taps; renegotiation retains the full trained filter. Neither
 * changes the decision point. */
struct v92_pcm_phase4_trn2u_line_rx {
	struct qam_equalizer equalizer;
	struct v92_pcm_phase4_trn2u reference;
	struct v92_phase4_demodulator demodulator;
	int16_t expected[
		V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY];
	uint64_t starting_input_samples;
	uint64_t input_samples;
	uint32_t trained_symbols;
	/* Keep physical decisions distinct from temporary descrambler fallout. */
	uint32_t symbol_errors;
	uint32_t coding_errors;
	double squared_error_sum;
	double adaptation;
	/* Add after filtering; retained FIR history stays in physical units. */
	double output_bias;
	enum g711_law law;
	uint8_t expected_index;
	int8_t polarity;
	bool renegotiation;
	bool failed;
};

/* Exact physical-boundary continuation into upstream control records. The
 * equalizer still owns decision_delay accepted TRN2u symbols; their known
 * targets are linearized here so a successor can finish training adaptation
 * before switching to decision-directed control reception. */
struct v92_pcm_phase4_trn2u_line_handoff {
	struct qam_equalizer equalizer;
	struct v92_phase4_demodulator demodulator;
	int16_t pending_training_targets[
		V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY];
	uint64_t input_samples;
	uint32_t training_symbols;
	double adaptation;
	double output_bias;
	enum g711_law law;
	int8_t polarity;
	bool renegotiation;
};

/* The receive-side LU scale is the measured TRN1u level retained by handoff.
 * constellation is the initial training choice from Jp; law is the local
 * network encoding, not the peer downstream codec field in CPt. */
enum v92_pcm_phase4_trn2u_line_rx_result
v92_pcm_phase4_trn2u_line_rx_init(
	struct v92_pcm_phase4_trn2u_line_rx *receiver,
	const struct v92_pcm_phase3_cpt_line_handoff *handoff,
	enum v92_constellation_size constellation, enum g711_law law);

struct v92_ru_line_rx;
/* First TRN2u after aligned Ru-bar (V.92 Amendment 1, 8.7.6). Retain the
 * trained FIR and its accepted input tail, but reset scrambling/differential
 * memory. Control hunting may start at the first 12-symbol boundary: the
 * peer may end TRN2u upon receiving SUVd (9.8.2.1.2). This is receive policy,
 * not permission for a transmitter to shorten its training arbitrarily. */
enum v92_pcm_phase4_trn2u_line_rx_result
v92_pcm_phase4_trn2u_line_rx_init_after_ru(
	struct v92_pcm_phase4_trn2u_line_rx *receiver,
	const struct v92_ru_line_rx *handoff);
bool v92_pcm_phase4_trn2u_line_rx_is_valid(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver);
bool v92_pcm_phase4_trn2u_line_rx_can_handoff(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver);
bool v92_pcm_phase4_trn2u_line_rx_has_failed(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver);

bool v92_pcm_phase4_trn2u_line_handoff_is_valid(
	const struct v92_pcm_phase4_trn2u_line_handoff *handoff);
enum v92_pcm_phase4_trn2u_line_rx_result
v92_pcm_phase4_trn2u_line_rx_handoff(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver,
	struct v92_pcm_phase4_trn2u_line_handoff *handoff);

/* Consume one signed 8 kHz sample. ALIGNED marks each physical twelve-symbol
 * boundary at or after the 12000-symbol initial minimum, or the first such
 * boundary of an explicitly initialized renegotiation. The equalizer's
 * delayed outputs have not yet exposed the last decision_delay symbols at
 * that boundary; a successor must retain them. Internal DSP failure latches
 * without consuming the sample; all rejected calls are transactional. */
enum v92_pcm_phase4_trn2u_line_rx_result
v92_pcm_phase4_trn2u_line_rx_sample(
	struct v92_pcm_phase4_trn2u_line_rx *receiver, int16_t sample);

const char *v92_pcm_phase4_trn2u_line_rx_result_name(
	enum v92_pcm_phase4_trn2u_line_rx_result result);

#endif /* V92_PCM_PHASE4_TRN2U_LINE_RX_H */
