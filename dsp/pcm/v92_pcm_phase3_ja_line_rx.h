// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_JA_LINE_RX_H
#define V92_PCM_PHASE3_JA_LINE_RX_H

#include "binary_equalizer.h"
#include "v92_ja_decoder.h"
#include "v92_pcm_phase3_trn1u_line_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_pcm_phase3_ja_line_rx_stage {
	V92_PCM_PHASE3_JA_LINE_RX_SEARCHING,
	V92_PCM_PHASE3_JA_LINE_RX_DECODING,
	V92_PCM_PHASE3_JA_LINE_RX_COMPLETE,
};

enum v92_pcm_phase3_ja_line_rx_result {
	V92_PCM_PHASE3_JA_LINE_RX_OK,
	V92_PCM_PHASE3_JA_LINE_RX_SYNCHRONIZED,
	V92_PCM_PHASE3_JA_LINE_RX_REJECTED,
	V92_PCM_PHASE3_JA_LINE_RX_DETECTED,
	V92_PCM_PHASE3_JA_LINE_RX_DONE,
	V92_PCM_PHASE3_JA_LINE_RX_FAILED,
	V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT,
	V92_PCM_PHASE3_JA_LINE_RX_INVALID_STATE,
	V92_PCM_PHASE3_JA_LINE_RX_NUMERIC_FAILURE,
};

union v92_pcm_phase3_ja_line_rx_sequence {
	struct v92_phase3_sync sync;
	struct v92_ja_decoder decoder;
};

/* Continue a locked PCM-upstream line from first TRN1u through Ja. The
 * inherited equalizer owns its delayed samples, so the first new input
 * produces the first sign not delivered by the training handoff. */
struct v92_pcm_phase3_ja_line_rx {
	struct binary_equalizer line;
	union v92_pcm_phase3_ja_line_rx_sequence sequence;
	/* Fixed handoff origin; elapsed input is input_samples minus this. */
	uint64_t starting_input_samples;
	uint64_t input_samples;
	enum v92_pcm_phase3_ja_line_rx_stage stage;
	uint8_t input_advance_symbols;
	bool failed;
};

enum v92_pcm_phase3_ja_line_rx_result v92_pcm_phase3_ja_line_rx_init(
	struct v92_pcm_phase3_ja_line_rx *receiver,
	const struct v92_pcm_phase3_trn1u_line_handoff *handoff);
bool v92_pcm_phase3_ja_line_rx_is_valid(
	const struct v92_pcm_phase3_ja_line_rx *receiver);
bool v92_pcm_phase3_ja_line_rx_is_complete(
	const struct v92_pcm_phase3_ja_line_rx *receiver);
bool v92_pcm_phase3_ja_line_rx_has_failed(
	const struct v92_pcm_phase3_ja_line_rx *receiver);

/* Consume one signed 8 kHz sample. Successful calls always produce one hard
 * sign because the inherited equalizer is full. A terminal DSP failure
 * consumes the sample and latches failure; other rejected calls consume
 * nothing. */
enum v92_pcm_phase3_ja_line_rx_result v92_pcm_phase3_ja_line_rx_sample(
	struct v92_pcm_phase3_ja_line_rx *receiver, int16_t sample);

/* Export the full learning request. Failed calls preserve parameters. */
bool v92_pcm_phase3_ja_line_rx_parameters(
	const struct v92_pcm_phase3_ja_line_rx *receiver,
	struct v92_pcm_dil_parameters *parameters);

const char *v92_pcm_phase3_ja_line_rx_result_name(
	enum v92_pcm_phase3_ja_line_rx_result result);

#endif /* V92_PCM_PHASE3_JA_LINE_RX_H */
