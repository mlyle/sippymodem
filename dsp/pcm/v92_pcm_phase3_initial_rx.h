// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_INITIAL_RX_H
#define V92_PCM_PHASE3_INITIAL_RX_H

#include "v92_pcm_phase3_ja_line_rx.h"
#include "v92_pcm_phase3_trn1u_line_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_pcm_phase3_initial_rx_stage {
	V92_PCM_PHASE3_INITIAL_RX_TRN1U,
	V92_PCM_PHASE3_INITIAL_RX_JA,
	V92_PCM_PHASE3_INITIAL_RX_COMPLETE,
};

enum v92_pcm_phase3_initial_rx_result {
	V92_PCM_PHASE3_INITIAL_RX_OK,
	V92_PCM_PHASE3_INITIAL_RX_ALIGNED,
	V92_PCM_PHASE3_INITIAL_RX_TRAINED,
	V92_PCM_PHASE3_INITIAL_RX_SYNCHRONIZED,
	V92_PCM_PHASE3_INITIAL_RX_REJECTED,
	V92_PCM_PHASE3_INITIAL_RX_DETECTED,
	V92_PCM_PHASE3_INITIAL_RX_DONE,
	V92_PCM_PHASE3_INITIAL_RX_FAILED,
	V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT,
	V92_PCM_PHASE3_INITIAL_RX_INTERNAL_ERROR,
};

union v92_pcm_phase3_initial_rx_sequence {
	struct v92_pcm_phase3_trn1u_line_rx trn1u;
	struct v92_pcm_phase3_ja_line_rx ja;
};

/* Own the digital modem's initial PCM-upstream receive procedure. Opening
 * signals are ignored while the first TRN1u is acquired; the trained line is
 * then continued without a sample gap through Ja and its advertised rate
 * mask. */
struct v92_pcm_phase3_initial_rx {
	union v92_pcm_phase3_initial_rx_sequence sequence;
	enum v92_pcm_phase3_initial_rx_stage stage;
};

enum v92_pcm_phase3_initial_rx_result v92_pcm_phase3_initial_rx_init(
	struct v92_pcm_phase3_initial_rx *receiver, double adaptation);
bool v92_pcm_phase3_initial_rx_is_valid(
	const struct v92_pcm_phase3_initial_rx *receiver);
bool v92_pcm_phase3_initial_rx_is_complete(
	const struct v92_pcm_phase3_initial_rx *receiver);
bool v92_pcm_phase3_initial_rx_has_failed(
	const struct v92_pcm_phase3_initial_rx *receiver);

/* Consume one signed 8 kHz sample. A child DSP failure consumes the sample
 * and remains latched; invalid and terminal calls consume nothing. */
enum v92_pcm_phase3_initial_rx_result v92_pcm_phase3_initial_rx_sample(
	struct v92_pcm_phase3_initial_rx *receiver, int16_t sample);

/* Export the full learning request. Failed calls preserve parameters. */
bool v92_pcm_phase3_initial_rx_parameters(
	const struct v92_pcm_phase3_initial_rx *receiver,
	struct v92_pcm_dil_parameters *parameters);

const char *v92_pcm_phase3_initial_rx_stage_name(
	enum v92_pcm_phase3_initial_rx_stage stage);
const char *v92_pcm_phase3_initial_rx_result_name(
	enum v92_pcm_phase3_initial_rx_result result);

#endif /* V92_PCM_PHASE3_INITIAL_RX_H */
