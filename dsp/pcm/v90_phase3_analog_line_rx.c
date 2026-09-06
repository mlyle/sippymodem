// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_analog_line_rx.h"

#include "memory_region.h"
#include "v34_constellation.h"

#include <stddef.h>

bool v90_phase3_analog_line_rx_is_valid(
	const struct v90_phase3_analog_line_rx *rx)
{
	uint32_t known_symbols;

	if (rx == NULL ||
	    !v90_phase3_analog_profile_is_valid(
		    &rx->upstream.transmitting_profile) ||
	    !v90_upstream_training_rx_is_valid(&rx->upstream) ||
	    !v90_phase3_ja_rx_is_valid(&rx->ja) ||
	    rx->upstream.line.equalizer.decision_pending ||
	    rx->upstream.line.equalizer.retained.pending)
		return false;
	if (rx->upstream.stage == V90_UPSTREAM_TRAINING_RX_INITIAL)
		return rx->ja.precoder.observed_symbols == 0u;
	known_symbols = rx->ja.precoder.observed_symbols;
	if (known_symbols > V34_TRAINING_SEQUENCE_MIN_SYMBOLS)
		known_symbols = V34_TRAINING_SEQUENCE_MIN_SYMBOLS;
	return rx->upstream.initial.j.received_symbols == known_symbols;
}

bool v90_phase3_analog_line_rx_has_failed(
	const struct v90_phase3_analog_line_rx *rx)
{
	return v90_phase3_analog_line_rx_is_valid(rx) && rx->failed;
}

enum v90_phase3_analog_line_rx_result v90_phase3_analog_line_rx_init(
	struct v90_phase3_analog_line_rx *rx,
	const struct v34_phase3_profile *analog_profile,
	int16_t initial_receive_gain)
{
	struct v90_phase3_analog_line_rx initialized = { 0 };

	if (rx == NULL ||
	    !v90_phase3_analog_profile_is_valid(analog_profile) ||
	    initial_receive_gain <= 0 ||
	    v90_upstream_training_rx_init(
		    &initialized.upstream, analog_profile,
		    initial_receive_gain) != V90_UPSTREAM_TRAINING_RX_OK ||
	    v90_phase3_ja_rx_init(&initialized.ja) != V90_PHASE3_JA_RX_OK ||
	    !v90_phase3_analog_line_rx_is_valid(&initialized))
		return V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_PHASE3_ANALOG_LINE_RX_OK;
}

static enum v90_phase3_analog_line_rx_result observe_ja(
	struct v90_phase3_ja_rx *ja, struct v34_analytic_sample decision)
{
	struct v34_analytic_sample fixed_target;
	struct qam_point unit_target = v34_rotated_point_zero(0);
	struct qam_point normalized;
	double scale;
	enum v90_phase3_ja_rx_result result;

	/* Use the generators' matching four-point coordinates. This is a fixed
	 * domain conversion, not another level/timing/equalizer acquisition. */
	if (v34_training_constellation_fixed_point(
		    V34_CONSTELLATION_4_POINT, 0, 0, &fixed_target) !=
		    V34_CONSTELLATION_OK || fixed_target.in_phase == 0)
		return V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE;
	scale = unit_target.in_phase / fixed_target.in_phase;
	normalized = (struct qam_point){
		decision.in_phase * scale, decision.quadrature * scale,
	};
	/* The fixed owner has already resolved this observation exactly once;
	 * Ja owns only the precoder recommendation and descriptor semantics. */
	result = v90_phase3_ja_rx_observe(ja, &normalized);
	switch (result) {
	case V90_PHASE3_JA_RX_OK:
		return V90_PHASE3_ANALOG_LINE_RX_OK;
	case V90_PHASE3_JA_RX_REJECTED:
		return V90_PHASE3_ANALOG_LINE_RX_JA_REJECTED;
	case V90_PHASE3_JA_RX_DETECTED:
		return V90_PHASE3_ANALOG_LINE_RX_JA_DETECTED;
	case V90_PHASE3_JA_RX_OUT_OF_RANGE:
		return V90_PHASE3_ANALOG_LINE_RX_OUT_OF_RANGE;
	case V90_PHASE3_JA_RX_COMPLETE:
	case V90_PHASE3_JA_RX_INVALID_ARGUMENT:
	case V90_PHASE3_JA_RX_INVALID_STATE:
	case V90_PHASE3_JA_RX_NUMERIC_FAILURE:
		break;
	}
	return V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE;
}

enum v90_phase3_analog_line_rx_result v90_phase3_analog_line_rx_sample(
	struct v90_phase3_analog_line_rx *rx, int16_t sample)
{
	struct v90_phase3_analog_line_rx next;
	struct v34_analytic_sample decision;
	enum v34_phase3_decision_receiver_stage previous;
	enum v90_phase3_analog_line_rx_result result =
		V90_PHASE3_ANALOG_LINE_RX_OK;
	enum v90_upstream_training_rx_result upstream_result;
	bool acquired;

	if (!v90_phase3_analog_line_rx_is_valid(rx))
		return V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE3_ANALOG_LINE_RX_FAILED;
	if (v90_phase3_ja_rx_is_complete(&rx->ja))
		return V90_PHASE3_ANALOG_LINE_RX_COMPLETE;
	next = *rx;
	previous = next.upstream.initial.stage;
	acquired = v34_receive_timing_is_acquired(&next.upstream.line.timing);
	upstream_result = v90_upstream_training_rx_sample(
		&next.upstream, sample, &decision);
	if (upstream_result == V90_UPSTREAM_TRAINING_RX_DECISION_READY)
		result = observe_ja(&next.ja, decision);
	else if (upstream_result != V90_UPSTREAM_TRAINING_RX_OK)
		result = V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE;
	else if (previous == V34_PHASE3_DECISION_RECEIVER_PP &&
		 next.upstream.initial.stage == V34_PHASE3_DECISION_RECEIVER_J)
		result = V90_PHASE3_ANALOG_LINE_RX_PP_COMPLETE;
	else if (previous == V34_PHASE3_DECISION_RECEIVER_S &&
		 next.upstream.initial.stage != V34_PHASE3_DECISION_RECEIVER_S)
		result = V90_PHASE3_ANALOG_LINE_RX_S_DETECTED;
	else if (!acquired &&
		 v34_receive_timing_is_acquired(&next.upstream.line.timing))
		result = V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED;
	if (result == V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE ||
	    result == V90_PHASE3_ANALOG_LINE_RX_OUT_OF_RANGE) {
		next.failed = true;
		if (v90_phase3_analog_line_rx_is_valid(&next))
			*rx = next;
		else
			rx->failed = true;
		return result;
	}
	if (!v90_phase3_analog_line_rx_is_valid(&next)) {
		rx->failed = true;
		return V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE;
	}
	*rx = next;
	return result;
}

bool v90_phase3_analog_line_handoff_is_valid(
	const struct v90_phase3_analog_line_handoff *handoff)
{
	return handoff != NULL &&
	       v90_phase3_analog_profile_is_valid(
		       &handoff->upstream.transmitting_profile) &&
	       v90_upstream_training_rx_is_tracking(&handoff->upstream) &&
	       v34_receive_timing_is_acquired(&handoff->upstream.line.timing) &&
	       !handoff->upstream.line.equalizer.decision_pending &&
	       !handoff->upstream.line.equalizer.retained.pending &&
	       handoff->upstream.initial.j.received_symbols ==
		       V34_TRAINING_SEQUENCE_MIN_SYMBOLS &&
	       v34_precoder_recommendation_is_valid(&handoff->ja.precoder) &&
	       v90_dil_parameters_are_valid(&handoff->ja.dil);
}

enum v90_phase3_analog_line_rx_result v90_phase3_analog_line_rx_handoff(
	const struct v90_phase3_analog_line_rx *rx,
	struct v90_phase3_analog_line_handoff *handoff)
{
	struct v90_phase3_analog_line_handoff generated = { 0 };

	if (!v90_phase3_analog_line_rx_is_valid(rx) || handoff == NULL ||
	    memory_regions_overlap(rx, sizeof(*rx), handoff, sizeof(*handoff)))
		return V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed || !v90_phase3_ja_rx_is_complete(&rx->ja))
		return rx->failed ? V90_PHASE3_ANALOG_LINE_RX_FAILED :
				    V90_PHASE3_ANALOG_LINE_RX_INVALID_STATE;
	generated.upstream = rx->upstream;
	if (v90_phase3_ja_rx_handoff(&rx->ja, &generated.ja) !=
		    V90_PHASE3_JA_RX_OK ||
	    !v90_phase3_analog_line_handoff_is_valid(&generated))
		return V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE;
	*handoff = generated;
	return V90_PHASE3_ANALOG_LINE_RX_OK;
}

const char *v90_phase3_analog_line_rx_result_name(
	enum v90_phase3_analog_line_rx_result result)
{
	switch (result) {
	case V90_PHASE3_ANALOG_LINE_RX_OK:
		return "ok";
	case V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED:
		return "timing acquired";
	case V90_PHASE3_ANALOG_LINE_RX_S_DETECTED:
		return "S detected";
	case V90_PHASE3_ANALOG_LINE_RX_PP_COMPLETE:
		return "PP complete";
	case V90_PHASE3_ANALOG_LINE_RX_JA_REJECTED:
		return "Ja rejected";
	case V90_PHASE3_ANALOG_LINE_RX_JA_DETECTED:
		return "Ja detected";
	case V90_PHASE3_ANALOG_LINE_RX_COMPLETE:
		return "complete";
	case V90_PHASE3_ANALOG_LINE_RX_FAILED:
		return "failed";
	case V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_ANALOG_LINE_RX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE:
		return "numeric failure";
	case V90_PHASE3_ANALOG_LINE_RX_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
