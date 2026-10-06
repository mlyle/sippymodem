// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_ja_line_rx.h"

#include <stddef.h>
#include <stdint.h>

static bool stage_is_valid(enum v92_pcm_phase3_ja_line_rx_stage stage)
{
	return stage >= V92_PCM_PHASE3_JA_LINE_RX_SEARCHING &&
	       stage <= V92_PCM_PHASE3_JA_LINE_RX_COMPLETE;
}

bool v92_pcm_phase3_ja_line_rx_is_valid(
	const struct v92_pcm_phase3_ja_line_rx *receiver)
{
	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    !v92_pcm_phase3_line_geometry_is_valid(&receiver->line) ||
	    receiver->starting_input_samples > receiver->input_samples ||
	    receiver->input_samples - receiver->starting_input_samples !=
		    receiver->processed_samples ||
	    receiver->input_advance_symbols !=
		    receiver->line.equalizer.decision_delay ||
	    receiver->input_advance_symbols !=
		    V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY ||
	    (receiver->failed &&
	     receiver->stage == V92_PCM_PHASE3_JA_LINE_RX_COMPLETE))
		return false;
	switch (receiver->stage) {
	case V92_PCM_PHASE3_JA_LINE_RX_SEARCHING:
		return v92_phase3_sync_is_valid(&receiver->sequence.sync) &&
		       receiver->sequence.sync.stage ==
			       V92_PHASE3_SYNC_SEARCHING;
	case V92_PCM_PHASE3_JA_LINE_RX_DECODING:
		return v92_ja_decoder_is_valid(&receiver->sequence.decoder) &&
		       !v92_ja_decoder_is_complete(&receiver->sequence.decoder);
	case V92_PCM_PHASE3_JA_LINE_RX_COMPLETE:
		return v92_ja_decoder_is_complete(&receiver->sequence.decoder);
	}
	return false;
}

enum v92_pcm_phase3_ja_line_rx_result v92_pcm_phase3_ja_line_rx_init(
	struct v92_pcm_phase3_ja_line_rx *receiver,
	const struct v92_pcm_phase3_trn1u_line_handoff *handoff)
{
	struct v92_pcm_phase3_ja_line_rx initialized;

	if (receiver == NULL ||
	    !v92_pcm_phase3_trn1u_line_handoff_is_valid(handoff))
		return V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT;
	initialized = (struct v92_pcm_phase3_ja_line_rx){
		.sequence.sync = handoff->sync,
		.starting_input_samples = handoff->input_samples,
		.input_samples = handoff->input_samples,
		.input_advance_symbols = handoff->input_advance_symbols,
	};
	if (binary_equalizer_init(
		    &initialized.line, &handoff->equalizer,
		    handoff->signal_level, handoff->adaptation,
		    handoff->polarity) != BINARY_EQUALIZER_OK ||
	    v92_phase3_sync_begin(&initialized.sequence.sync) !=
		    V92_PHASE3_SYNC_OK ||
	    !v92_pcm_phase3_ja_line_rx_is_valid(&initialized))
		return V92_PCM_PHASE3_JA_LINE_RX_NUMERIC_FAILURE;
	*receiver = initialized;
	return V92_PCM_PHASE3_JA_LINE_RX_OK;
}

bool v92_pcm_phase3_ja_line_rx_is_complete(
	const struct v92_pcm_phase3_ja_line_rx *receiver)
{
	return v92_pcm_phase3_ja_line_rx_is_valid(receiver) &&
	       !receiver->failed &&
	       receiver->stage == V92_PCM_PHASE3_JA_LINE_RX_COMPLETE;
}

bool v92_pcm_phase3_ja_line_rx_has_failed(
	const struct v92_pcm_phase3_ja_line_rx *receiver)
{
	return v92_pcm_phase3_ja_line_rx_is_valid(receiver) &&
	       receiver->failed;
}

static enum v92_pcm_phase3_ja_line_rx_result fail_sample(
	struct v92_pcm_phase3_ja_line_rx *receiver,
	struct v92_pcm_phase3_ja_line_rx *next)
{
	next->failed = true;
	if (v92_pcm_phase3_ja_line_rx_is_valid(next))
		*receiver = *next;
	else
		receiver->failed = true;
	return V92_PCM_PHASE3_JA_LINE_RX_NUMERIC_FAILURE;
}

static enum v92_pcm_phase3_ja_line_rx_result receive_search_sign(
	struct v92_pcm_phase3_ja_line_rx *receiver, uint8_t sign)
{
	struct v92_ja_decoder decoder;
	enum v92_phase3_sync_result result = v92_phase3_sync_receive(
		&receiver->sequence.sync, sign);

	if (result == V92_PHASE3_SYNC_OK)
		return V92_PCM_PHASE3_JA_LINE_RX_OK;
	if (result != V92_PHASE3_SYNC_SYNCHRONIZED ||
	    v92_ja_decoder_init(&decoder, &receiver->sequence.sync) !=
		    V92_JA_DECODER_OK)
		return V92_PCM_PHASE3_JA_LINE_RX_NUMERIC_FAILURE;
	receiver->sequence = (union v92_pcm_phase3_ja_line_rx_sequence){
		.decoder = decoder,
	};
	receiver->stage = V92_PCM_PHASE3_JA_LINE_RX_DECODING;
	return V92_PCM_PHASE3_JA_LINE_RX_SYNCHRONIZED;
}

static enum v92_pcm_phase3_ja_line_rx_result receive_descriptor_sign(
	struct v92_pcm_phase3_ja_line_rx *receiver, uint8_t sign)
{
	enum v92_ja_decoder_result result = v92_ja_decoder_receive(
		&receiver->sequence.decoder, sign);

	if (result == V92_JA_DECODER_OK)
		return V92_PCM_PHASE3_JA_LINE_RX_OK;
	if (result == V92_JA_DECODER_REJECTED)
		return V92_PCM_PHASE3_JA_LINE_RX_REJECTED;
	if (result != V92_JA_DECODER_DETECTED)
		return V92_PCM_PHASE3_JA_LINE_RX_NUMERIC_FAILURE;
	receiver->stage = V92_PCM_PHASE3_JA_LINE_RX_COMPLETE;
	return V92_PCM_PHASE3_JA_LINE_RX_DETECTED;
}

enum v92_pcm_phase3_ja_line_rx_result v92_pcm_phase3_ja_line_rx_sample(
	struct v92_pcm_phase3_ja_line_rx *receiver, int16_t sample)
{
	struct v92_pcm_phase3_ja_line_rx next;
	enum v92_pcm_phase3_ja_line_rx_result result;
	uint8_t sign;

	if (!v92_pcm_phase3_ja_line_rx_is_valid(receiver))
		return V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V92_PCM_PHASE3_JA_LINE_RX_FAILED;
	if (receiver->stage == V92_PCM_PHASE3_JA_LINE_RX_COMPLETE)
		return V92_PCM_PHASE3_JA_LINE_RX_DONE;
	if (receiver->input_samples == UINT64_MAX ||
	    receiver->processed_samples == UINT64_MAX)
		return V92_PCM_PHASE3_JA_LINE_RX_INVALID_STATE;
	next = *receiver;
	next.input_samples++;
	next.processed_samples++;
	if (binary_equalizer_receive(&next.line, sample, &sign) !=
	    BINARY_EQUALIZER_OK)
		return fail_sample(receiver, &next);
	if (next.stage == V92_PCM_PHASE3_JA_LINE_RX_SEARCHING)
		result = receive_search_sign(&next, sign);
	else
		result = receive_descriptor_sign(&next, sign);
	if (result == V92_PCM_PHASE3_JA_LINE_RX_NUMERIC_FAILURE ||
	    !v92_pcm_phase3_ja_line_rx_is_valid(&next))
		return fail_sample(receiver, &next);
	*receiver = next;
	return result;
}

bool v92_pcm_phase3_ja_line_rx_parameters(
	const struct v92_pcm_phase3_ja_line_rx *receiver,
	struct v92_pcm_dil_parameters *parameters)
{
	return v92_pcm_phase3_ja_line_rx_is_complete(receiver) &&
	       v92_ja_decoder_parameters(
		       &receiver->sequence.decoder, parameters);
}

const char *v92_pcm_phase3_ja_line_rx_result_name(
	enum v92_pcm_phase3_ja_line_rx_result result)
{
	switch (result) {
	case V92_PCM_PHASE3_JA_LINE_RX_OK:
		return "ok";
	case V92_PCM_PHASE3_JA_LINE_RX_SYNCHRONIZED:
		return "synchronized";
	case V92_PCM_PHASE3_JA_LINE_RX_REJECTED:
		return "rejected";
	case V92_PCM_PHASE3_JA_LINE_RX_DETECTED:
		return "detected";
	case V92_PCM_PHASE3_JA_LINE_RX_DONE:
		return "done";
	case V92_PCM_PHASE3_JA_LINE_RX_FAILED:
		return "failed";
	case V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE3_JA_LINE_RX_INVALID_STATE:
		return "invalid state";
	case V92_PCM_PHASE3_JA_LINE_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
