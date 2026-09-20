// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_trn1u_line_rx.h"

#include "packed_bits.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	MINIMUM_CORRELATION_SCORE = 40,
};

_Static_assert((int)V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS <=
		       (int)QAM_EQUALIZER_MAX_TAP_COUNT,
	       "TRN1u equalizer must fit shared storage");
_Static_assert(V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY <
		       V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS,
	       "TRN1u decision delay must select a tap");
_Static_assert(V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS <= UINT8_MAX,
	       "TRN1u sync window counters must fit");

static bool stage_is_valid(
	enum v92_pcm_phase3_trn1u_line_rx_stage stage)
{
	return stage >= V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_HUNTING &&
	       stage <= V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_FAILED;
}

static bool geometry_is_valid(const struct qam_equalizer *equalizer)
{
	return qam_equalizer_is_valid(equalizer) &&
	       equalizer->tap_count ==
		       V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS &&
	       equalizer->decision_delay ==
		       V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY;
}

static bool aligned_state_is_valid(
	const struct v92_pcm_phase3_trn1u_line_rx *receiver)
{
	return receiver->window_count ==
		       V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS &&
	       (receiver->polarity == -1 || receiver->polarity == 1) &&
	       isfinite(receiver->signal_level) &&
		       receiver->signal_level >=
			       V92_PCM_PHASE3_TRN1U_LINE_MIN_SIGNAL_LEVEL &&
	       receiver->signal_level <= 32768.0 &&
	       receiver->equalizer.fill ==
		       receiver->equalizer.decision_delay + 1u &&
	       !receiver->equalizer.can_adapt &&
	       receiver->input_samples >=
		       (uint64_t)receiver->output_symbols +
			       receiver->equalizer.decision_delay;
}

bool v92_pcm_phase3_trn1u_line_rx_is_valid(
	const struct v92_pcm_phase3_trn1u_line_rx *receiver)
{
	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    !geometry_is_valid(&receiver->equalizer) ||
	    !v92_phase3_sync_is_valid(&receiver->sync) ||
	    receiver->sync.stage != V92_PHASE3_SYNC_TRAINING ||
	    !v34_scrambler_is_valid(&receiver->reference) ||
	    receiver->reference.polynomial != V34_POLYNOMIAL_GPA ||
	    !unpacked_bits_are_binary(
		    receiver->expected,
		    V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS) ||
	    receiver->window_count >
		    V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS ||
	    receiver->window_index >=
		    V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS ||
	    !qam_adaptation_is_valid(receiver->adaptation) ||
	    !isfinite(receiver->signal_level) || receiver->signal_level < 0.0 ||
	    receiver->signal_level > 32768.0 || receiver->polarity < -1 ||
	    receiver->polarity > 1 ||
	    receiver->output_symbols != receiver->sync.trained_symbols ||
	    receiver->training_errors > receiver->output_symbols ||
	    receiver->input_samples < receiver->window_count)
		return false;
	switch (receiver->stage) {
	case V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_HUNTING:
		return receiver->output_symbols == 0u &&
		       receiver->training_errors == 0u &&
		       receiver->signal_level == 0.0 &&
		       receiver->polarity == 0 &&
		       receiver->window_index ==
			       receiver->input_samples %
				       V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS;
	case V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_TRAINING:
		return aligned_state_is_valid(receiver) &&
		       receiver->output_symbols <
			       V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS;
	case V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_COMPLETE:
		return aligned_state_is_valid(receiver) &&
		       receiver->output_symbols ==
			       V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS;
	case V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_FAILED:
		return receiver->output_symbols <=
		       V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS;
	}
	return false;
}

static bool initialize_equalizer(
	struct qam_equalizer *equalizer, double level)
{
	struct qam_point reference = {
		.in_phase = level,
	};

	return qam_equalizer_init_geometry(
		       equalizer, &reference, &reference,
		       V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS,
		       V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY) ==
	       QAM_EQUALIZER_OK;
}

enum v92_pcm_phase3_trn1u_line_rx_result
v92_pcm_phase3_trn1u_line_rx_init(
	struct v92_pcm_phase3_trn1u_line_rx *receiver,
	double adaptation)
{
	struct v92_pcm_phase3_trn1u_line_rx initialized = { 0 };

	if (receiver == NULL || !qam_adaptation_is_valid(adaptation) ||
	    !initialize_equalizer(&initialized.equalizer, 1.0) ||
	    v92_phase3_sync_init(&initialized.sync) != V92_PHASE3_SYNC_OK ||
	    v34_scrambler_init(
		    &initialized.reference, V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK)
		return V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT;
	initialized.adaptation = adaptation;
	if (!v92_pcm_phase3_trn1u_line_rx_is_valid(&initialized))
		return V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT;
	*receiver = initialized;
	return V92_PCM_PHASE3_TRN1U_LINE_RX_OK;
}

bool v92_pcm_phase3_trn1u_line_rx_is_complete(
	const struct v92_pcm_phase3_trn1u_line_rx *receiver)
{
	return v92_pcm_phase3_trn1u_line_rx_is_valid(receiver) &&
	       receiver->stage == V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_COMPLETE;
}

static int16_t window_sample(
	const struct v92_pcm_phase3_trn1u_line_rx *receiver,
	size_t index)
{
	return receiver->sync_window[
		(receiver->window_index + index) %
		V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS];
}

static bool window_matches(
	const struct v92_pcm_phase3_trn1u_line_rx *receiver,
	int8_t *polarity, double *level)
{
	struct v34_scrambler reference;
	int64_t correlation = 0;
	uint64_t energy = 0u;
	uint64_t magnitude_sum = 0u;
	uint64_t absolute_correlation;

	if (polarity == NULL || level == NULL ||
	    v34_scrambler_init(&reference, V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK)
		return false;
	for (size_t index = 0;
	     index < V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS; ++index) {
		int32_t sample = window_sample(receiver, index);
		uint32_t magnitude = sample < 0 ?
			(uint32_t)-sample : (uint32_t)sample;
		uint8_t sign;

		if (v34_scramble_group(&reference, 1u, 1u, &sign) !=
		    V34_FRAMING_OK)
			return false;
		correlation += sign != 0u ? -sample : sample;
		energy += (uint64_t)magnitude * magnitude;
		magnitude_sum += magnitude;
	}
	absolute_correlation = correlation < 0 ?
		(uint64_t)-correlation : (uint64_t)correlation;
	if (energy <
		    (uint64_t)V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS *
			    V92_PCM_PHASE3_TRN1U_LINE_MIN_SIGNAL_LEVEL *
			    V92_PCM_PHASE3_TRN1U_LINE_MIN_SIGNAL_LEVEL ||
	    absolute_correlation * absolute_correlation <
		    (uint64_t)MINIMUM_CORRELATION_SCORE * energy)
		return false;
	*polarity = correlation < 0 ? -1 : 1;
	*level = (double)magnitude_sum /
		 V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS;
	return true;
}

static bool next_reference_sign(
	struct v34_scrambler *reference, uint8_t *sign)
{
	return v34_scramble_group(reference, 1u, 1u, sign) ==
	       V34_FRAMING_OK;
}

static void push_expected(
	struct v92_pcm_phase3_trn1u_line_rx *receiver, uint8_t sign)
{
	for (size_t index =
		     V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS - 1u;
	     index != 0u; --index)
		receiver->expected[index] = receiver->expected[index - 1u];
	receiver->expected[0] = sign;
}

static bool train_sample(
	struct v92_pcm_phase3_trn1u_line_rx *receiver, int16_t sample)
{
	struct qam_point input = {
		.in_phase = receiver->polarity > 0 ?
				    sample : -(double)sample,
	};
	struct qam_point output;
	struct qam_point target;
	uint8_t expected_sign;
	uint8_t received_sign;
	bool ready;

	if (!next_reference_sign(&receiver->reference, &expected_sign))
		return false;
	push_expected(receiver, expected_sign);
	if (qam_equalizer_push(
		    &receiver->equalizer, &input, &output, &ready) !=
	    QAM_EQUALIZER_OK)
		return false;
	if (!ready)
		return true;
	expected_sign = receiver->expected[
		V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY];
	target = (struct qam_point){
		.in_phase = expected_sign != 0u ?
				    -receiver->signal_level : receiver->signal_level,
	};
	if (qam_equalizer_adapt(
		    &receiver->equalizer, &target,
		    receiver->adaptation) != QAM_EQUALIZER_OK)
		return false;
	received_sign = output.in_phase < 0.0 ? 1u : 0u;
	if (v92_phase3_sync_train(&receiver->sync, received_sign) !=
	    V92_PHASE3_SYNC_OK)
		return false;
	receiver->output_symbols++;
	if (received_sign != expected_sign)
		receiver->training_errors++;
	if (receiver->output_symbols == V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS)
		receiver->stage = V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_COMPLETE;
	return true;
}

static bool align_receiver(
	struct v92_pcm_phase3_trn1u_line_rx *receiver,
	int8_t polarity, double level)
{
	if (!initialize_equalizer(&receiver->equalizer, level) ||
	    v34_scrambler_init(
		    &receiver->reference, V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK)
		return false;
	receiver->polarity = polarity;
	receiver->signal_level = level;
	receiver->stage = V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_TRAINING;
	memset(receiver->expected, 0, sizeof(receiver->expected));
	for (size_t index = 0;
	     index < V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS; ++index) {
		if (!train_sample(receiver, window_sample(receiver, index)))
			return false;
	}
	return true;
}

static bool hunt_sample(
	struct v92_pcm_phase3_trn1u_line_rx *receiver,
	int16_t sample, bool *aligned)
{
	int8_t polarity;
	double level;

	receiver->sync_window[receiver->window_index] = sample;
	receiver->window_index = (uint8_t)(
		(receiver->window_index + 1u) %
		V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS);
	if (receiver->window_count <
	    V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS)
		receiver->window_count++;
	*aligned = receiver->window_count ==
			   V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS &&
		   window_matches(receiver, &polarity, &level);
	return !*aligned || align_receiver(receiver, polarity, level);
}

static enum v92_pcm_phase3_trn1u_line_rx_result fail_sample(
	struct v92_pcm_phase3_trn1u_line_rx *receiver,
	struct v92_pcm_phase3_trn1u_line_rx *next)
{
	next->stage = V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_FAILED;
	if (v92_pcm_phase3_trn1u_line_rx_is_valid(next))
		*receiver = *next;
	else
		receiver->stage = V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_FAILED;
	return V92_PCM_PHASE3_TRN1U_LINE_RX_NUMERIC_FAILURE;
}

enum v92_pcm_phase3_trn1u_line_rx_result
v92_pcm_phase3_trn1u_line_rx_sample(
	struct v92_pcm_phase3_trn1u_line_rx *receiver,
	int16_t sample)
{
	struct v92_pcm_phase3_trn1u_line_rx next;
	bool aligned = false;

	if (!v92_pcm_phase3_trn1u_line_rx_is_valid(receiver))
		return V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT;
	if (receiver->stage == V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_FAILED)
		return V92_PCM_PHASE3_TRN1U_LINE_RX_FAILED;
	if (receiver->stage == V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_COMPLETE)
		return V92_PCM_PHASE3_TRN1U_LINE_RX_DONE;
	if (receiver->input_samples == UINT64_MAX)
		return V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_STATE;
	next = *receiver;
	next.input_samples++;
	if (next.stage == V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_HUNTING) {
		if (!hunt_sample(&next, sample, &aligned))
			return fail_sample(receiver, &next);
	} else if (!train_sample(&next, sample)) {
		return fail_sample(receiver, &next);
	}
	if (!v92_pcm_phase3_trn1u_line_rx_is_valid(&next))
		return fail_sample(receiver, &next);
	*receiver = next;
	if (next.stage == V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_COMPLETE)
		return V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED;
	return aligned ? V92_PCM_PHASE3_TRN1U_LINE_RX_ALIGNED :
			 V92_PCM_PHASE3_TRN1U_LINE_RX_OK;
}

bool v92_pcm_phase3_trn1u_line_handoff_is_valid(
	const struct v92_pcm_phase3_trn1u_line_handoff *handoff)
{
	return handoff != NULL &&
	       qam_equalizer_is_handoff_ready(&handoff->equalizer) &&
	       handoff->equalizer.tap_count ==
		       V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS &&
	       handoff->equalizer.decision_delay ==
		       V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY &&
	       v92_phase3_sync_is_valid(&handoff->sync) &&
	       handoff->sync.stage == V92_PHASE3_SYNC_TRAINING &&
	       handoff->sync.trained_symbols ==
		       V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS &&
	       handoff->training_errors <=
		       V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS &&
	       isfinite(handoff->signal_level) &&
	       handoff->signal_level >=
		       V92_PCM_PHASE3_TRN1U_LINE_MIN_SIGNAL_LEVEL &&
	       handoff->signal_level <= 32768.0 &&
	       qam_adaptation_is_valid(handoff->adaptation) &&
	       handoff->input_advance_symbols ==
		       V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY &&
	       handoff->input_samples >=
		       (uint64_t)V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS +
			       handoff->input_advance_symbols &&
	       (handoff->polarity == -1 || handoff->polarity == 1);
}

enum v92_pcm_phase3_trn1u_line_rx_result
v92_pcm_phase3_trn1u_line_rx_handoff(
	const struct v92_pcm_phase3_trn1u_line_rx *receiver,
	struct v92_pcm_phase3_trn1u_line_handoff *handoff)
{
	struct v92_pcm_phase3_trn1u_line_handoff generated;

	if (!v92_pcm_phase3_trn1u_line_rx_is_valid(receiver) ||
	    handoff == NULL)
		return V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT;
	if (receiver->stage != V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_COMPLETE)
		return receiver->stage == V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_FAILED ?
			       V92_PCM_PHASE3_TRN1U_LINE_RX_FAILED :
			       V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_STATE;
	generated = (struct v92_pcm_phase3_trn1u_line_handoff){
		.equalizer = receiver->equalizer,
		.sync = receiver->sync,
		.input_samples = receiver->input_samples,
		.training_errors = receiver->training_errors,
		.signal_level = receiver->signal_level,
		.adaptation = receiver->adaptation,
		.input_advance_symbols =
			V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY,
		.polarity = receiver->polarity,
	};
	if (!v92_pcm_phase3_trn1u_line_handoff_is_valid(&generated))
		return V92_PCM_PHASE3_TRN1U_LINE_RX_NUMERIC_FAILURE;
	*handoff = generated;
	return V92_PCM_PHASE3_TRN1U_LINE_RX_OK;
}

const char *v92_pcm_phase3_trn1u_line_rx_result_name(
	enum v92_pcm_phase3_trn1u_line_rx_result result)
{
	switch (result) {
	case V92_PCM_PHASE3_TRN1U_LINE_RX_OK:
		return "ok";
	case V92_PCM_PHASE3_TRN1U_LINE_RX_ALIGNED:
		return "aligned";
	case V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED:
		return "trained";
	case V92_PCM_PHASE3_TRN1U_LINE_RX_DONE:
		return "done";
	case V92_PCM_PHASE3_TRN1U_LINE_RX_FAILED:
		return "failed";
	case V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_STATE:
		return "invalid state";
	case V92_PCM_PHASE3_TRN1U_LINE_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
