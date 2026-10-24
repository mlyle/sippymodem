// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_CPT_LINE_RX_H
#define V92_PCM_PHASE3_CPT_LINE_RX_H

#include "binary_equalizer.h"
#include "v92_pcm_phase3_cpt_rx.h"
#include "v92_pcm_phase3_trn1u_line_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_pcm_phase3_cpt_line_rx_result {
	V92_PCM_PHASE3_CPT_LINE_RX_OK,
	V92_PCM_PHASE3_CPT_LINE_RX_SIGN,
	V92_PCM_PHASE3_CPT_LINE_RX_EVENT,
	V92_PCM_PHASE3_CPT_LINE_RX_DONE,
	V92_PCM_PHASE3_CPT_LINE_RX_FAILED,
	V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT,
	V92_PCM_PHASE3_CPT_LINE_RX_INVALID_STATE,
	V92_PCM_PHASE3_CPT_LINE_RX_NUMERIC_FAILURE,
};

struct v92_cpt_line_candidate {
	struct binary_equalizer line;
	struct v92_pcm_phase3_cpt_rx signs;
};

/* Continue a locked PCM-upstream line from second TRN1u through repeated CPt
 * records and E1u. The inherited equalizer owns its delayed samples, so the
 * first new input produces the first sign not delivered by the handoff. */
struct v92_pcm_phase3_cpt_line_rx {
	struct binary_equalizer line;
	struct v92_pcm_phase3_cpt_rx signs;
	/* A training-derived DC interpretation competes only until the first
	 * CRC-valid CPt record. Raw wins ties; the selected state owns E1u. */
	struct v92_cpt_line_candidate alternate;
	/* Fixed handoff origin; elapsed input is input_samples minus this. */
	uint64_t starting_input_samples;
	uint64_t input_samples;
	uint8_t input_advance_symbols;
	int16_t dc_offset;
	bool alternate_active;
	bool failed;
};

/* E1u decisions trail the physical line by the equalizer decision delay.
 * Preserve those already-accepted TRN2u symbols with the trained line state.
 * TRN2u starts a fresh GPA scrambler, but its differential sign begins with
 * previous_sign from the final E1u symbol. A centered CPt receiver restores
 * raw input coordinates in the FIR history for known-symbol TRN2u training. */
struct v92_pcm_phase3_cpt_line_handoff {
	struct qam_equalizer equalizer;
	struct v92_cpt_parameters parameters;
	uint64_t input_samples;
	double signal_level;
	double adaptation;
	uint8_t input_advance_symbols;
	uint8_t previous_sign;
	int8_t polarity;
};

enum v92_pcm_phase3_cpt_line_rx_result
v92_pcm_phase3_cpt_line_rx_init(
	struct v92_pcm_phase3_cpt_line_rx *receiver,
	const struct v92_pcm_phase3_trn1u_line_handoff *handoff);
bool v92_pcm_phase3_cpt_line_rx_is_valid(
	const struct v92_pcm_phase3_cpt_line_rx *receiver);
bool v92_pcm_phase3_cpt_line_rx_is_complete(
	const struct v92_pcm_phase3_cpt_line_rx *receiver);
bool v92_pcm_phase3_cpt_line_rx_has_failed(
	const struct v92_pcm_phase3_cpt_line_rx *receiver);

/* Consume one signed 8 kHz sample. Successful calls always produce one hard
 * sign because the inherited equalizer is full. event changes on SIGN and
 * EVENT; invalid, terminal, and failed calls change neither state nor event.
 * A terminal DSP failure consumes the sample and latches failure. */
enum v92_pcm_phase3_cpt_line_rx_result
v92_pcm_phase3_cpt_line_rx_sample(
	struct v92_pcm_phase3_cpt_line_rx *receiver, int16_t sample,
	struct v92_pcm_phase3_cpt_rx_event *event);

bool v92_pcm_phase3_cpt_line_rx_parameters(
	const struct v92_pcm_phase3_cpt_line_rx *receiver,
	struct v92_cpt_parameters *parameters);
bool v92_pcm_phase3_cpt_line_rx_previous_sign(
	const struct v92_pcm_phase3_cpt_line_rx *receiver,
	uint8_t *previous_sign);

bool v92_pcm_phase3_cpt_line_handoff_is_valid(
	const struct v92_pcm_phase3_cpt_line_handoff *handoff);
enum v92_pcm_phase3_cpt_line_rx_result
v92_pcm_phase3_cpt_line_rx_handoff(
	const struct v92_pcm_phase3_cpt_line_rx *receiver,
	struct v92_pcm_phase3_cpt_line_handoff *handoff);

const char *v92_pcm_phase3_cpt_line_rx_result_name(
	enum v92_pcm_phase3_cpt_line_rx_result result);

#endif /* V92_PCM_PHASE3_CPT_LINE_RX_H */
