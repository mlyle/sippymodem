// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_return_line_rx.h"

#include "memory_region.h"
#include "v90_phase3.h"

#include <stddef.h>

bool v90_phase3_return_line_rx_is_valid(
	const struct v90_phase3_return_line_rx *rx)
{
	return rx != NULL &&
	       v90_phase3_demodulator_is_locked(
		       &rx->demodulator, &rx->transmitting_profile) &&
	       v90_phase3_return_rx_is_valid(&rx->points) &&
	       v90_upstream_training_rx_is_tracking(&rx->upstream) &&
	       v34_phase3_profile_is_equal(
		       &rx->upstream.transmitting_profile,
		       &rx->transmitting_profile) &&
	       v34_phase3_s_prefix_is_valid(&rx->s_prefix) &&
	       (!rx->handoff_draining ||
		v90_phase3_return_rx_is_complete(&rx->points)) &&
	       rx->reacquisition_symbols <=
		       V90_PHASE3_RETURN_LINE_RX_REACQUISITION_SYMBOLS &&
	       (rx->reacquiring_equalizer ?
			(rx->points.semantic.second_return && rx->s_prefix.complete &&
			 rx->points.semantic.stage ==
				 V90_PHASE3_RETURN_RX_STAGE_SEARCH_S) :
			(rx->reacquisition_symbols == 0 &&
			 !rx->points.equalizer_reacquiring));
}

bool v90_phase3_return_line_rx_has_failed(
	const struct v90_phase3_return_line_rx *rx)
{
	return v90_phase3_return_line_rx_is_valid(rx) && rx->failed;
}

enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_init(
	struct v90_phase3_return_line_rx *rx,
	const struct v90_phase3_analog_line_handoff *handoff,
	double adaptation)
{
	struct v90_phase3_return_line_rx initialized = { 0 };

	if (rx == NULL ||
	    !v90_phase3_analog_line_handoff_is_valid(handoff))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	initialized.demodulator = handoff->demodulator;
	initialized.upstream = handoff->upstream;
	initialized.transmitting_profile = handoff->transmitting_profile;
	if (v90_phase3_return_rx_init_with_precoder(
		    &initialized.points, &handoff->phase3.equalizer,
		    &handoff->phase3.precoder,
		    adaptation) != V90_PHASE3_RETURN_RX_OK ||
	    v34_phase3_s_prefix_init_recurrent(&initialized.s_prefix) !=
		    V34_PHASE3_S_PREFIX_OK ||
	    !v90_phase3_return_line_rx_is_valid(&initialized))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_PHASE3_RETURN_LINE_RX_OK;
}

enum v90_phase3_return_line_rx_result
v90_phase3_return_line_rx_allow_implicit_s_end(
	struct v90_phase3_return_line_rx *rx)
{
	enum v90_phase3_return_rx_result result;

	if (!v90_phase3_return_line_rx_is_valid(rx))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	result = v90_phase3_return_rx_allow_implicit_s_end(&rx->points);
	if (result == V90_PHASE3_RETURN_RX_OK)
		return V90_PHASE3_RETURN_LINE_RX_OK;
	if (result == V90_PHASE3_RETURN_RX_INVALID_STATE)
		return V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
}

enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_restart(
	struct v90_phase3_return_line_rx *rx)
{
	struct v90_phase3_return_line_rx restarted;
	enum v90_phase3_return_rx_result result;

	if (!v90_phase3_return_line_rx_is_valid(rx))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE3_RETURN_LINE_RX_FAILED;
	if (rx->handoff_draining)
		return V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	restarted = *rx;
	result = v90_phase3_return_rx_restart(&restarted.points);
	if (result == V90_PHASE3_RETURN_RX_INVALID_STATE)
		return V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	if (result != V90_PHASE3_RETURN_RX_OK ||
	    v34_phase3_s_prefix_init_recurrent(&restarted.s_prefix) !=
		    V34_PHASE3_S_PREFIX_OK ||
	    !v90_phase3_return_line_rx_is_valid(&restarted))
		return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
	*rx = restarted;
	return V90_PHASE3_RETURN_LINE_RX_OK;
}

static enum v90_phase3_return_line_rx_result point_result(
	enum v90_phase3_return_rx_result result)
{
	switch (result) {
	case V90_PHASE3_RETURN_RX_OK:
		return V90_PHASE3_RETURN_LINE_RX_OK;
	case V90_PHASE3_RETURN_RX_S_DETECTED:
		return V90_PHASE3_RETURN_LINE_RX_S_DETECTED;
	case V90_PHASE3_RETURN_RX_S_ENDED:
		return V90_PHASE3_RETURN_LINE_RX_S_ENDED;
	case V90_PHASE3_RETURN_RX_S_BAR_DETECTED:
		return V90_PHASE3_RETURN_LINE_RX_S_BAR_DETECTED;
	case V90_PHASE3_RETURN_RX_COMPLETE:
	case V90_PHASE3_RETURN_RX_INVALID_ARGUMENT:
	case V90_PHASE3_RETURN_RX_INVALID_STATE:
	case V90_PHASE3_RETURN_RX_NUMERIC_FAILURE:
		break;
	}
	return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
}

enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_sample(
	struct v90_phase3_return_line_rx *rx, int16_t sample)
{
	struct v90_phase3_return_line_rx next;
	struct qam_point point;
	struct v34_analytic_sample decision;
	enum v90_phase3_return_rx_stage previous_stage;
	bool point_consumed = false;
	bool ready;
	enum v90_upstream_training_rx_result upstream_result;
	enum v90_phase3_return_line_rx_result result;

	if (!v90_phase3_return_line_rx_is_valid(rx))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE3_RETURN_LINE_RX_FAILED;
	if (v90_phase3_return_rx_is_complete(&rx->points))
		return V90_PHASE3_RETURN_LINE_RX_COMPLETE;
	if (rx->points.semantic.stage == V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END)
		return V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	next = *rx;
	upstream_result = v90_upstream_training_rx_sample(
		&next.upstream, sample, &decision);
	if ((upstream_result != V90_UPSTREAM_TRAINING_RX_OK &&
	     upstream_result != V90_UPSTREAM_TRAINING_RX_DECISION_READY) ||
	    qam_demodulator_sample(
		    &next.demodulator, sample, &point, &ready) != QAM_OK) {
		rx->failed = true;
		return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
	}
	result = V90_PHASE3_RETURN_LINE_RX_OK;
	if (ready && next.points.semantic.second_return &&
	    next.points.semantic.stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S &&
	    !next.reacquiring_equalizer) {
		struct qam_measurement measurement =
			qam_demodulator_measure(&next.demodulator);
		enum v34_phase3_s_prefix_result prefix_result =
			v34_phase3_s_prefix_receive(
				&next.s_prefix, &point,
				measurement.symbol_power / 64.0);

		if (prefix_result != V34_PHASE3_S_PREFIX_OK &&
		    prefix_result != V34_PHASE3_S_PREFIX_DETECTED &&
		    prefix_result != V34_PHASE3_S_PREFIX_REBASE_TIMING) {
			result = V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
		} else if (prefix_result == V34_PHASE3_S_PREFIX_DETECTED) {
			if (v90_phase3_return_rx_reacquire_s(
				    &next.points, &point) !=
			    V90_PHASE3_RETURN_RX_OK) {
				result =
					V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
			} else {
				next.reacquiring_equalizer = true;
				next.reacquisition_symbols = 0;
				point_consumed = true;
			}
		}
	}
	previous_stage = next.points.semantic.stage;
	if (ready && result == V90_PHASE3_RETURN_LINE_RX_OK &&
	    !point_consumed)
		result = point_result(
			v90_phase3_return_rx_point(&next.points, &point));
	if (ready && next.reacquiring_equalizer) {
		if (result == V90_PHASE3_RETURN_LINE_RX_S_DETECTED) {
			next.reacquiring_equalizer = false;
			next.reacquisition_symbols = 0;
		} else if (next.reacquisition_symbols <
			   V90_PHASE3_RETURN_LINE_RX_REACQUISITION_SYMBOLS) {
			next.reacquisition_symbols++;
		} else if (v34_phase3_s_prefix_init_recurrent(
				   &next.s_prefix) !=
				   V34_PHASE3_S_PREFIX_OK) {
			result = V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
		} else {
			next.reacquiring_equalizer = false;
			next.reacquisition_symbols = 0;
		}
	}
	if (ready && previous_stage == V90_PHASE3_RETURN_RX_STAGE_S &&
	    next.points.semantic.stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S) {
		if (v34_phase3_s_prefix_init_recurrent(&next.s_prefix) !=
		    V34_PHASE3_S_PREFIX_OK)
			result = V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
		next.reacquiring_equalizer = false;
		next.reacquisition_symbols = 0;
	}
	if (result == V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE) {
		next.failed = true;
		if (v90_phase3_return_line_rx_is_valid(&next))
			*rx = next;
		else
			rx->failed = true;
		return result;
	}
	if (!v90_phase3_return_line_rx_is_valid(&next)) {
		rx->failed = true;
		return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
	}
	*rx = next;
	return result;
}

enum v90_phase3_return_line_rx_result
v90_phase3_return_line_rx_advance_complete(
	struct v90_phase3_return_line_rx *rx, int16_t sample)
{
	struct v90_phase3_return_line_rx next;
	enum v90_upstream_training_rx_result upstream_result;

	if (!v90_phase3_return_line_rx_is_valid(rx))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE3_RETURN_LINE_RX_FAILED;
	if (!v90_phase3_return_rx_is_complete(&rx->points))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	next = *rx;
	next.handoff_draining = true;
	upstream_result = v90_upstream_training_rx_advance(
		&next.upstream, sample);
	if (upstream_result == V90_UPSTREAM_TRAINING_RX_FAILED ||
	    upstream_result == V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT ||
	    upstream_result == V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE) {
		next.failed = true;
		if (v90_phase3_return_line_rx_is_valid(&next))
			*rx = next;
		else
			rx->failed = true;
		return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
	}
	if (!v90_phase3_return_line_rx_is_valid(&next)) {
		rx->failed = true;
		return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
	}
	*rx = next;
	return V90_PHASE3_RETURN_LINE_RX_OK;
}

bool v90_phase3_return_line_handoff_is_valid(
	const struct v90_phase3_return_line_handoff *handoff)
{
	return handoff != NULL &&
	       v90_phase3_analog_profile_is_valid(
		       &handoff->upstream.transmitting_profile) &&
	       v90_upstream_training_rx_is_tracking(&handoff->upstream) &&
	       v34_receive_timing_is_acquired(&handoff->upstream.line.timing) &&
	       !handoff->upstream.line.equalizer.decision_pending &&
	       !handoff->upstream.line.equalizer.retained.pending &&
	       v34_phase3_s_detection_is_valid(&handoff->marker) &&
	       v34_precoder_recommendation_is_valid(&handoff->precoder);
}

enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_handoff(
	const struct v90_phase3_return_line_rx *rx,
	struct v90_phase3_return_line_handoff *handoff)
{
	struct v90_phase3_return_line_handoff generated = { 0 };

	if (!v90_phase3_return_line_rx_is_valid(rx) || handoff == NULL ||
	    memory_regions_overlap(rx, sizeof(*rx), handoff, sizeof(*handoff)))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed || !v90_phase3_return_rx_is_complete(&rx->points))
		return rx->failed ? V90_PHASE3_RETURN_LINE_RX_FAILED :
				    V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	generated.upstream = rx->upstream;
	generated.precoder = rx->points.precoder;
	if (!v90_phase3_return_marker_get_detection(
		    &rx->points.semantic, &generated.marker) ||
	    !v90_phase3_return_line_handoff_is_valid(&generated))
		return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
	*handoff = generated;
	return V90_PHASE3_RETURN_LINE_RX_OK;
}

const char *v90_phase3_return_line_rx_result_name(
	enum v90_phase3_return_line_rx_result result)
{
	switch (result) {
	case V90_PHASE3_RETURN_LINE_RX_OK:
		return "ok";
	case V90_PHASE3_RETURN_LINE_RX_S_DETECTED:
		return "S detected";
	case V90_PHASE3_RETURN_LINE_RX_S_ENDED:
		return "S ended without S-bar";
	case V90_PHASE3_RETURN_LINE_RX_S_BAR_DETECTED:
		return "S-bar detected";
	case V90_PHASE3_RETURN_LINE_RX_COMPLETE:
		return "complete";
	case V90_PHASE3_RETURN_LINE_RX_FAILED:
		return "failed";
	case V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_RETURN_LINE_RX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
