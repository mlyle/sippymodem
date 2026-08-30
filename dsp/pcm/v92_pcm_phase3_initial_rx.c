// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_initial_rx.h"

#include <stddef.h>

static bool stage_is_valid(enum v92_pcm_phase3_initial_rx_stage stage)
{
	return stage >= V92_PCM_PHASE3_INITIAL_RX_TRN1U &&
	       stage <= V92_PCM_PHASE3_INITIAL_RX_COMPLETE;
}

bool v92_pcm_phase3_initial_rx_is_valid(
	const struct v92_pcm_phase3_initial_rx *receiver)
{
	if (receiver == NULL || !stage_is_valid(receiver->stage))
		return false;
	switch (receiver->stage) {
	case V92_PCM_PHASE3_INITIAL_RX_TRN1U:
		return v92_pcm_phase3_trn1u_line_rx_is_valid(
			&receiver->sequence.trn1u);
	case V92_PCM_PHASE3_INITIAL_RX_JA:
		return v92_pcm_phase3_ja_line_rx_is_valid(
			       &receiver->sequence.ja) &&
		       !v92_pcm_phase3_ja_line_rx_is_complete(
			       &receiver->sequence.ja);
	case V92_PCM_PHASE3_INITIAL_RX_COMPLETE:
		return v92_pcm_phase3_ja_line_rx_is_complete(
			&receiver->sequence.ja);
	}
	return false;
}

enum v92_pcm_phase3_initial_rx_result v92_pcm_phase3_initial_rx_init(
	struct v92_pcm_phase3_initial_rx *receiver, double adaptation)
{
	struct v92_pcm_phase3_initial_rx initialized = { 0 };

	if (receiver == NULL || !qam_adaptation_is_valid(adaptation) ||
	    v92_pcm_phase3_trn1u_line_rx_init(
		    &initialized.sequence.trn1u, adaptation) !=
		    V92_PCM_PHASE3_TRN1U_LINE_RX_OK ||
	    !v92_pcm_phase3_initial_rx_is_valid(&initialized))
		return V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT;
	*receiver = initialized;
	return V92_PCM_PHASE3_INITIAL_RX_OK;
}

bool v92_pcm_phase3_initial_rx_is_complete(
	const struct v92_pcm_phase3_initial_rx *receiver)
{
	return v92_pcm_phase3_initial_rx_is_valid(receiver) &&
	       receiver->stage == V92_PCM_PHASE3_INITIAL_RX_COMPLETE;
}

bool v92_pcm_phase3_initial_rx_has_failed(
	const struct v92_pcm_phase3_initial_rx *receiver)
{
	if (!v92_pcm_phase3_initial_rx_is_valid(receiver))
		return false;
	if (receiver->stage == V92_PCM_PHASE3_INITIAL_RX_TRN1U)
		return receiver->sequence.trn1u.stage ==
		       V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_FAILED;
	return v92_pcm_phase3_ja_line_rx_has_failed(
		&receiver->sequence.ja);
}

static enum v92_pcm_phase3_initial_rx_result receive_trn1u(
	struct v92_pcm_phase3_initial_rx *receiver, int16_t sample)
{
	struct v92_pcm_phase3_trn1u_line_handoff handoff;
	enum v92_pcm_phase3_trn1u_line_rx_result result =
		v92_pcm_phase3_trn1u_line_rx_sample(
			&receiver->sequence.trn1u, sample);

	switch (result) {
	case V92_PCM_PHASE3_TRN1U_LINE_RX_OK:
		return V92_PCM_PHASE3_INITIAL_RX_OK;
	case V92_PCM_PHASE3_TRN1U_LINE_RX_ALIGNED:
		return V92_PCM_PHASE3_INITIAL_RX_ALIGNED;
	case V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED:
		if (v92_pcm_phase3_trn1u_line_rx_handoff(
			    &receiver->sequence.trn1u, &handoff) !=
			    V92_PCM_PHASE3_TRN1U_LINE_RX_OK)
			break;
		{
			struct v92_pcm_phase3_ja_line_rx ja;

			if (v92_pcm_phase3_ja_line_rx_init(&ja, &handoff) !=
			    V92_PCM_PHASE3_JA_LINE_RX_OK)
				break;
			receiver->sequence =
				(union v92_pcm_phase3_initial_rx_sequence){
					.ja = ja,
				};
		}
		receiver->stage = V92_PCM_PHASE3_INITIAL_RX_JA;
		return V92_PCM_PHASE3_INITIAL_RX_TRAINED;
	case V92_PCM_PHASE3_TRN1U_LINE_RX_FAILED:
		return V92_PCM_PHASE3_INITIAL_RX_FAILED;
	case V92_PCM_PHASE3_TRN1U_LINE_RX_NUMERIC_FAILURE:
		return V92_PCM_PHASE3_INITIAL_RX_INTERNAL_ERROR;
	case V92_PCM_PHASE3_TRN1U_LINE_RX_DONE:
	case V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT:
	case V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_STATE:
		break;
	}
	return V92_PCM_PHASE3_INITIAL_RX_INTERNAL_ERROR;
}

static enum v92_pcm_phase3_initial_rx_result receive_ja(
	struct v92_pcm_phase3_initial_rx *receiver, int16_t sample)
{
	enum v92_pcm_phase3_ja_line_rx_result result =
		v92_pcm_phase3_ja_line_rx_sample(&receiver->sequence.ja, sample);

	switch (result) {
	case V92_PCM_PHASE3_JA_LINE_RX_OK:
		return V92_PCM_PHASE3_INITIAL_RX_OK;
	case V92_PCM_PHASE3_JA_LINE_RX_SYNCHRONIZED:
		return V92_PCM_PHASE3_INITIAL_RX_SYNCHRONIZED;
	case V92_PCM_PHASE3_JA_LINE_RX_REJECTED:
		return V92_PCM_PHASE3_INITIAL_RX_REJECTED;
	case V92_PCM_PHASE3_JA_LINE_RX_DETECTED:
		receiver->stage = V92_PCM_PHASE3_INITIAL_RX_COMPLETE;
		return V92_PCM_PHASE3_INITIAL_RX_DETECTED;
	case V92_PCM_PHASE3_JA_LINE_RX_FAILED:
		return V92_PCM_PHASE3_INITIAL_RX_FAILED;
	case V92_PCM_PHASE3_JA_LINE_RX_NUMERIC_FAILURE:
		return V92_PCM_PHASE3_INITIAL_RX_INTERNAL_ERROR;
	case V92_PCM_PHASE3_JA_LINE_RX_DONE:
	case V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT:
	case V92_PCM_PHASE3_JA_LINE_RX_INVALID_STATE:
		break;
	}
	return V92_PCM_PHASE3_INITIAL_RX_INTERNAL_ERROR;
}

enum v92_pcm_phase3_initial_rx_result v92_pcm_phase3_initial_rx_sample(
	struct v92_pcm_phase3_initial_rx *receiver, int16_t sample)
{
	struct v92_pcm_phase3_initial_rx next;
	enum v92_pcm_phase3_initial_rx_result result;

	if (!v92_pcm_phase3_initial_rx_is_valid(receiver))
		return V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT;
	if (v92_pcm_phase3_initial_rx_has_failed(receiver))
		return V92_PCM_PHASE3_INITIAL_RX_FAILED;
	if (receiver->stage == V92_PCM_PHASE3_INITIAL_RX_COMPLETE)
		return V92_PCM_PHASE3_INITIAL_RX_DONE;
	next = *receiver;
	result = next.stage == V92_PCM_PHASE3_INITIAL_RX_TRN1U ?
			 receive_trn1u(&next, sample) : receive_ja(&next, sample);
	if (!v92_pcm_phase3_initial_rx_is_valid(&next))
		return V92_PCM_PHASE3_INITIAL_RX_INTERNAL_ERROR;
	*receiver = next;
	return result;
}

bool v92_pcm_phase3_initial_rx_rate_mask(
	const struct v92_pcm_phase3_initial_rx *receiver,
	uint32_t *rate_mask)
{
	return v92_pcm_phase3_initial_rx_is_complete(receiver) &&
	       v92_pcm_phase3_ja_line_rx_rate_mask(
		       &receiver->sequence.ja, rate_mask);
}

const char *v92_pcm_phase3_initial_rx_stage_name(
	enum v92_pcm_phase3_initial_rx_stage stage)
{
	switch (stage) {
	case V92_PCM_PHASE3_INITIAL_RX_TRN1U:
		return "TRN1u";
	case V92_PCM_PHASE3_INITIAL_RX_JA:
		return "Ja";
	case V92_PCM_PHASE3_INITIAL_RX_COMPLETE:
		return "complete";
	}
	return "unknown";
}

const char *v92_pcm_phase3_initial_rx_result_name(
	enum v92_pcm_phase3_initial_rx_result result)
{
	switch (result) {
	case V92_PCM_PHASE3_INITIAL_RX_OK:
		return "ok";
	case V92_PCM_PHASE3_INITIAL_RX_ALIGNED:
		return "aligned";
	case V92_PCM_PHASE3_INITIAL_RX_TRAINED:
		return "trained";
	case V92_PCM_PHASE3_INITIAL_RX_SYNCHRONIZED:
		return "synchronized";
	case V92_PCM_PHASE3_INITIAL_RX_REJECTED:
		return "rejected";
	case V92_PCM_PHASE3_INITIAL_RX_DETECTED:
		return "detected";
	case V92_PCM_PHASE3_INITIAL_RX_DONE:
		return "done";
	case V92_PCM_PHASE3_INITIAL_RX_FAILED:
		return "failed";
	case V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE3_INITIAL_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
