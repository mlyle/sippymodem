// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_return_line_rx.h"

#include "memory_region.h"
#include "v34_constellation.h"
#include "v90_phase3.h"

#include <stddef.h>

bool v90_phase3_return_line_rx_is_valid(
	const struct v90_phase3_return_line_rx *rx)
{
	return rx != NULL &&
	       v90_phase3_analog_profile_is_valid(
		       &rx->upstream.transmitting_profile) &&
	       v90_upstream_training_rx_is_tracking(&rx->upstream) &&
	       v34_receive_timing_is_acquired(&rx->upstream.line.timing) &&
	       !rx->upstream.line.equalizer.decision_pending &&
	       !rx->upstream.line.equalizer.retained.pending &&
	       v90_phase3_return_marker_is_valid(&rx->marker) &&
	       v34_precoder_recommendation_is_valid(&rx->precoder) &&
	       (!rx->handoff_draining ||
		v90_phase3_return_marker_is_complete(&rx->marker));
}

bool v90_phase3_return_line_rx_has_failed(
	const struct v90_phase3_return_line_rx *rx)
{
	return v90_phase3_return_line_rx_is_valid(rx) && rx->failed;
}

enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_init(
	struct v90_phase3_return_line_rx *rx,
	const struct v90_phase3_analog_line_handoff *handoff)
{
	struct v90_phase3_return_line_rx initialized = { 0 };

	if (rx == NULL ||
	    !v90_phase3_analog_line_handoff_is_valid(handoff) ||
	    memory_regions_overlap(rx, sizeof(*rx), handoff, sizeof(*handoff)))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	initialized.upstream = handoff->upstream;
	initialized.precoder = handoff->ja.precoder;
	if (v90_phase3_return_marker_init(&initialized.marker, true) !=
		    V90_PHASE3_RETURN_RX_OK ||
	    !v90_phase3_return_line_rx_is_valid(&initialized))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_PHASE3_RETURN_LINE_RX_OK;
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
	result = v90_phase3_return_marker_restart(&restarted.marker);
	if (result == V90_PHASE3_RETURN_RX_INVALID_STATE)
		return V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	if (result != V90_PHASE3_RETURN_RX_OK ||
	    !v90_phase3_return_line_rx_is_valid(&restarted))
		return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
	*rx = restarted;
	return V90_PHASE3_RETURN_LINE_RX_OK;
}

static enum v90_phase3_return_line_rx_result marker_result(
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

static enum v90_phase3_return_line_rx_result observe_return(
	struct v90_phase3_return_marker *marker,
	struct v34_analytic_sample decision)
{
	struct v34_fixed_training_decision hard;

	/* Slicing near-silence can produce an alternating rotation alias. Break
	 * its unpublished onset history before S can end Jd. Once S is announced,
	 * preserve the irreversible transmit handoff and ordinary S-bar rules. */
	if (marker->stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S &&
	    !v34_training_constellation_has_signal(
		    V34_CONSTELLATION_4_POINT, decision)) {
		if (marker->buffered_onset_decisions != 0) {
			struct v90_phase3_return_marker cleared;

			if (v90_phase3_return_marker_init(
				    &cleared, marker->confirm_onset) !=
			    V90_PHASE3_RETURN_RX_OK)
				return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
			cleared.allow_implicit_s_end = marker->allow_implicit_s_end;
			cleared.second_return = marker->second_return;
			*marker = cleared;
		}
		return V90_PHASE3_RETURN_LINE_RX_OK;
	}
	if (v34_training_constellation_fixed_slice(
		    V34_CONSTELLATION_4_POINT, decision, &hard) !=
	    V34_CONSTELLATION_OK)
		return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
	return marker_result(v90_phase3_return_marker_observe(
		marker, hard.clockwise_quarter_turns));
}

enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_sample(
	struct v90_phase3_return_line_rx *rx, int16_t sample)
{
	struct v90_phase3_return_line_rx next;
	struct v34_analytic_sample decision;
	enum v90_upstream_training_rx_result upstream_result;
	enum v90_phase3_return_line_rx_result result =
		V90_PHASE3_RETURN_LINE_RX_OK;

	if (!v90_phase3_return_line_rx_is_valid(rx))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE3_RETURN_LINE_RX_FAILED;
	if (v90_phase3_return_marker_is_complete(&rx->marker))
		return V90_PHASE3_RETURN_LINE_RX_COMPLETE;
	if (rx->marker.stage == V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END)
		return V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	next = *rx;
	upstream_result = v90_upstream_training_rx_sample(
		&next.upstream, sample, &decision);
	if (upstream_result == V90_UPSTREAM_TRAINING_RX_DECISION_READY)
		result = observe_return(&next.marker, decision);
	else if (upstream_result != V90_UPSTREAM_TRAINING_RX_OK) {
		result = V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
	}
	/* SCR can use Jd's sixteen-point constellation; S/S-bar always use
	 * four points. Resume their adaptation after the qualified S onset. */
	if (result == V90_PHASE3_RETURN_LINE_RX_S_DETECTED)
		next.upstream.tracking_constellation = V34_CONSTELLATION_4_POINT;
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
	if (!v90_phase3_return_marker_is_complete(&rx->marker))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	next = *rx;
	next.handoff_draining = true;
	upstream_result = v90_upstream_training_rx_advance(
		&next.upstream, sample);
	if (upstream_result != V90_UPSTREAM_TRAINING_RX_OK) {
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
	if (rx->failed || !v90_phase3_return_marker_is_complete(&rx->marker))
		return rx->failed ? V90_PHASE3_RETURN_LINE_RX_FAILED :
				    V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	generated.upstream = rx->upstream;
	generated.precoder = rx->precoder;
	if (!v90_phase3_return_marker_get_detection(
		    &rx->marker, &generated.marker) ||
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
