// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_cpt_line_rx.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static bool equalizer_is_continuous(
	const struct qam_equalizer *equalizer)
{
	return qam_equalizer_is_valid(equalizer) &&
	       equalizer->tap_count ==
		       V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS &&
	       equalizer->decision_delay ==
		       V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY &&
	       equalizer->fill == equalizer->decision_delay + 1u &&
	       !equalizer->can_adapt;
}

bool v92_pcm_phase3_cpt_line_rx_is_valid(
	const struct v92_pcm_phase3_cpt_line_rx *receiver)
{
	return receiver != NULL &&
	       equalizer_is_continuous(&receiver->equalizer) &&
	       v92_pcm_phase3_cpt_rx_is_valid(&receiver->signs) &&
	       receiver->signs.stage != V92_PCM_PHASE3_CPT_RX_TRAINING &&
	       receiver->starting_input_samples <= receiver->input_samples &&
	       receiver->input_samples - receiver->starting_input_samples ==
		       receiver->processed_samples &&
	       isfinite(receiver->signal_level) &&
	       receiver->signal_level >=
		       V92_PCM_PHASE3_TRN1U_LINE_MIN_SIGNAL_LEVEL &&
	       receiver->signal_level <= 32768.0 &&
	       isfinite(receiver->adaptation) && receiver->adaptation >= 0.0 &&
	       receiver->adaptation <= 1.0 &&
	       receiver->input_advance_symbols ==
		       receiver->equalizer.decision_delay &&
	       (receiver->polarity == -1 || receiver->polarity == 1);
}

bool v92_pcm_phase3_cpt_line_rx_is_complete(
	const struct v92_pcm_phase3_cpt_line_rx *receiver)
{
	return v92_pcm_phase3_cpt_line_rx_is_valid(receiver) &&
	       !receiver->failed &&
	       v92_pcm_phase3_cpt_rx_is_complete(&receiver->signs);
}

bool v92_pcm_phase3_cpt_line_rx_has_failed(
	const struct v92_pcm_phase3_cpt_line_rx *receiver)
{
	return v92_pcm_phase3_cpt_line_rx_is_valid(receiver) &&
	       receiver->failed;
}

enum v92_pcm_phase3_cpt_line_rx_result
v92_pcm_phase3_cpt_line_rx_init(
	struct v92_pcm_phase3_cpt_line_rx *receiver,
	const struct v92_pcm_phase3_trn1u_line_handoff *handoff)
{
	struct v92_pcm_phase3_cpt_line_rx initialized;

	if (receiver == NULL ||
	    !v92_pcm_phase3_trn1u_line_handoff_is_valid(handoff))
		return V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT;
	initialized = (struct v92_pcm_phase3_cpt_line_rx){
		.equalizer = handoff->equalizer,
		.starting_input_samples = handoff->input_samples,
		.input_samples = handoff->input_samples,
		.signal_level = handoff->signal_level,
		.adaptation = handoff->adaptation,
		.input_advance_symbols = handoff->input_advance_symbols,
		.polarity = handoff->polarity,
	};
	if (v92_pcm_phase3_cpt_rx_init_from_training(
		    &initialized.signs, &handoff->sync) !=
		    V92_PCM_PHASE3_CPT_RX_OK ||
	    !v92_pcm_phase3_cpt_line_rx_is_valid(&initialized))
		return V92_PCM_PHASE3_CPT_LINE_RX_NUMERIC_FAILURE;
	*receiver = initialized;
	return V92_PCM_PHASE3_CPT_LINE_RX_OK;
}

static enum v92_pcm_phase3_cpt_line_rx_result fail_sample(
	struct v92_pcm_phase3_cpt_line_rx *receiver,
	struct v92_pcm_phase3_cpt_line_rx *next)
{
	next->failed = true;
	if (v92_pcm_phase3_cpt_line_rx_is_valid(next))
		*receiver = *next;
	else
		receiver->failed = true;
	return V92_PCM_PHASE3_CPT_LINE_RX_NUMERIC_FAILURE;
}

enum v92_pcm_phase3_cpt_line_rx_result
v92_pcm_phase3_cpt_line_rx_sample(
	struct v92_pcm_phase3_cpt_line_rx *receiver, int16_t sample,
	struct v92_pcm_phase3_cpt_rx_event *event)
{
	struct v92_pcm_phase3_cpt_line_rx next;
	struct qam_equalizer_observation observation = { 0 };
	struct v92_pcm_phase3_cpt_rx_event emitted = { 0 };
	struct qam_point input;
	struct qam_point target;
	uint8_t sign;
	bool ready;

	if (!v92_pcm_phase3_cpt_line_rx_is_valid(receiver) || event == NULL)
		return V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V92_PCM_PHASE3_CPT_LINE_RX_FAILED;
	if (v92_pcm_phase3_cpt_rx_is_complete(&receiver->signs))
		return V92_PCM_PHASE3_CPT_LINE_RX_DONE;
	if (receiver->input_samples == UINT64_MAX ||
	    receiver->processed_samples == UINT64_MAX)
		return V92_PCM_PHASE3_CPT_LINE_RX_INVALID_STATE;
	next = *receiver;
	next.input_samples++;
	next.processed_samples++;
	input = (struct qam_point){
		.in_phase = next.polarity > 0 ? sample : -(double)sample,
	};
	if (qam_equalizer_observe(
		    &next.equalizer, &input, &observation, &ready) !=
		    QAM_EQUALIZER_OK ||
	    !ready)
		return fail_sample(receiver, &next);
	sign = observation.output.in_phase < 0.0 ? 1u : 0u;
	target = (struct qam_point){
		.in_phase = sign != 0u ? -next.signal_level : next.signal_level,
	};
	if (v92_pcm_phase3_cpt_rx_receive(
		    &next.signs, sign, &emitted) != V92_PCM_PHASE3_CPT_RX_OK ||
	    qam_equalizer_adapt_observation(
		    &next.equalizer, &observation, &target,
		    next.adaptation) != QAM_EQUALIZER_OK ||
	    !v92_pcm_phase3_cpt_line_rx_is_valid(&next))
		return fail_sample(receiver, &next);
	*receiver = next;
	*event = emitted;
	return emitted.type == V92_PCM_PHASE3_CPT_RX_NO_EVENT ?
		       V92_PCM_PHASE3_CPT_LINE_RX_SIGN :
		       V92_PCM_PHASE3_CPT_LINE_RX_EVENT;
}

bool v92_pcm_phase3_cpt_line_rx_parameters(
	const struct v92_pcm_phase3_cpt_line_rx *receiver,
	struct v92_cpt_parameters *parameters)
{
	return v92_pcm_phase3_cpt_line_rx_is_valid(receiver) &&
	       !receiver->failed &&
	       v92_pcm_phase3_cpt_rx_parameters(&receiver->signs, parameters);
}

bool v92_pcm_phase3_cpt_line_rx_previous_sign(
	const struct v92_pcm_phase3_cpt_line_rx *receiver,
	uint8_t *previous_sign)
{
	return v92_pcm_phase3_cpt_line_rx_is_complete(receiver) &&
	       v92_pcm_phase3_cpt_rx_previous_sign(
		       &receiver->signs, previous_sign);
}

bool v92_pcm_phase3_cpt_line_handoff_is_valid(
	const struct v92_pcm_phase3_cpt_line_handoff *handoff)
{
	return handoff != NULL &&
	       equalizer_is_continuous(&handoff->equalizer) &&
	       v92_cpt_parameters_are_valid(&handoff->parameters) &&
	       handoff->input_advance_symbols ==
		       V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY &&
	       handoff->input_samples >= handoff->input_advance_symbols &&
	       isfinite(handoff->signal_level) &&
	       handoff->signal_level >=
		       V92_PCM_PHASE3_TRN1U_LINE_MIN_SIGNAL_LEVEL &&
	       handoff->signal_level <= 32768.0 &&
	       isfinite(handoff->adaptation) && handoff->adaptation >= 0.0 &&
	       handoff->adaptation <= 1.0 && handoff->previous_sign <= 1u &&
	       (handoff->polarity == -1 || handoff->polarity == 1);
}

enum v92_pcm_phase3_cpt_line_rx_result
v92_pcm_phase3_cpt_line_rx_handoff(
	const struct v92_pcm_phase3_cpt_line_rx *receiver,
	struct v92_pcm_phase3_cpt_line_handoff *handoff)
{
	struct v92_pcm_phase3_cpt_line_handoff generated;

	if (!v92_pcm_phase3_cpt_line_rx_is_valid(receiver) || handoff == NULL)
		return V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V92_PCM_PHASE3_CPT_LINE_RX_FAILED;
	if (!v92_pcm_phase3_cpt_rx_is_complete(&receiver->signs))
		return V92_PCM_PHASE3_CPT_LINE_RX_INVALID_STATE;
	generated = (struct v92_pcm_phase3_cpt_line_handoff){
		.equalizer = receiver->equalizer,
		.input_samples = receiver->input_samples,
		.signal_level = receiver->signal_level,
		.adaptation = receiver->adaptation,
		.input_advance_symbols = receiver->input_advance_symbols,
		.polarity = receiver->polarity,
	};
	if (!v92_pcm_phase3_cpt_rx_parameters(
		    &receiver->signs, &generated.parameters) ||
	    !v92_pcm_phase3_cpt_rx_previous_sign(
		    &receiver->signs, &generated.previous_sign) ||
	    !v92_pcm_phase3_cpt_line_handoff_is_valid(&generated))
		return V92_PCM_PHASE3_CPT_LINE_RX_NUMERIC_FAILURE;
	*handoff = generated;
	return V92_PCM_PHASE3_CPT_LINE_RX_OK;
}

const char *v92_pcm_phase3_cpt_line_rx_result_name(
	enum v92_pcm_phase3_cpt_line_rx_result result)
{
	switch (result) {
	case V92_PCM_PHASE3_CPT_LINE_RX_OK:
		return "ok";
	case V92_PCM_PHASE3_CPT_LINE_RX_SIGN:
		return "sign";
	case V92_PCM_PHASE3_CPT_LINE_RX_EVENT:
		return "event";
	case V92_PCM_PHASE3_CPT_LINE_RX_DONE:
		return "done";
	case V92_PCM_PHASE3_CPT_LINE_RX_FAILED:
		return "failed";
	case V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE3_CPT_LINE_RX_INVALID_STATE:
		return "invalid state";
	case V92_PCM_PHASE3_CPT_LINE_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
