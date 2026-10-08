// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_cp_line_rx.h"

#include "memory_region.h"
#include "v90_phase3.h"

#include <stddef.h>

bool v90_phase4_cp_line_rx_is_valid(
	const struct v90_phase4_cp_line_rx *rx)
{
	return rx != NULL &&
	       v34_phase3_profile_is_valid(&rx->transmitting_profile) &&
	       v34_line_receiver_matches_profile(
		       &rx->line, &rx->transmitting_profile) &&
	       v34_receive_timing_is_acquired(&rx->line.timing) &&
	       rx->line.equalizer.feedback_mode ==
		       V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
	       !rx->line.equalizer.decision_pending &&
	       (rx->boundary_complete ||
		v34_phase3_s_receiver_is_valid(&rx->boundary)) &&
	       rx->boundary_tail_count <=
		       V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS &&
	       (!rx->boundary_complete || rx->boundary_tail_count == 0u) &&
	       v90_phase4_cp_rx_is_valid(&rx->symbols) &&
	       rx->symbols.transmitting_role ==
		       rx->transmitting_profile.local_role &&
	       rx->decoder_power_q15 != 0u &&
	       rx->decoder_power_q15 <= INT16_MAX &&
	       (rx->adaptation == V34_ADAPTIVE_EQUALIZER_FULL ||
		rx->adaptation == V34_ADAPTIVE_EQUALIZER_CARRIER ||
		rx->adaptation == V34_ADAPTIVE_EQUALIZER_CENTER_TAPS) &&
	       (!rx->pending_record_is_valid ||
		(rx->record_pending &&
		 v90_cp_parameters_are_valid(&rx->pending_parameters)));
}

static void clear_boundary_tail(struct v90_phase4_cp_line_rx *rx)
{
	rx->boundary_tail_count = 0u;
	for (size_t index = 0u;
	     index < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS; ++index)
		rx->boundary_tail[index] =
			(struct v34_analytic_sample){ 0 };
}

static bool replay_post_marker_decisions(
	struct v90_phase4_cp_line_rx *rx, uint8_t post_marker_symbols,
	struct v90_cp_receive_event *event)
{
	if (post_marker_symbols > rx->boundary_tail_count)
		return false;
	for (size_t index = rx->boundary_tail_count - post_marker_symbols;
	     index < rx->boundary_tail_count; ++index) {
		struct v34_fixed_training_decision hard;
		struct v90_cp_receive_event emitted = { 0 };

		if (v34_training_constellation_fixed_slice(
			    rx->symbols.constellation,
			    rx->boundary_tail[index], &hard) !=
			    V34_CONSTELLATION_OK ||
		    v90_phase4_cp_rx_receive_decision(
			    &rx->symbols, hard.quarter_point,
			    hard.clockwise_quarter_turns, &emitted) != V90_CP_OK)
			return false;
		if (emitted.type != V90_CP_RECEIVE_NO_EVENT) {
			if (event->type != V90_CP_RECEIVE_NO_EVENT)
				return false;
			*event = emitted;
		}
	}
	clear_boundary_tail(rx);
	return true;
}

static bool receive_fixed_decision(
	struct v90_phase4_cp_line_rx *rx,
	struct v34_analytic_sample decision,
	struct v34_fixed_training_decision *hard,
	struct v34_phase3_s_adaptation *boundary_adaptation,
	struct v90_cp_receive_event *event)
{
	*boundary_adaptation = (struct v34_phase3_s_adaptation){ 0 };
	*event = (struct v90_cp_receive_event){ 0 };
	if (rx->boundary_complete) {
		return v34_training_constellation_fixed_slice(
			       rx->symbols.constellation, decision, hard) ==
			       V34_CONSTELLATION_OK &&
		       v90_phase4_cp_rx_receive_decision(
			       &rx->symbols, hard->quarter_point,
			       hard->clockwise_quarter_turns, event) == V90_CP_OK;
	}
	if (v34_training_constellation_fixed_slice(
		    V34_CONSTELLATION_4_POINT, decision, hard) !=
	    V34_CONSTELLATION_OK)
		return false;
	rx->boundary_tail_count = v34_phase3_s_append_observation(
		rx->boundary_tail, rx->boundary_tail_count, decision);
	{
		enum v34_phase3_s_receiver_result result =
			v34_phase3_s_receiver_receive_adaptive(
				&rx->boundary, hard->quarter_point,
				hard->clockwise_quarter_turns, decision,
				boundary_adaptation);

		if (result == V34_PHASE3_S_RECEIVER_OK)
			return true;
		if (result != V34_PHASE3_S_RECEIVER_DETECTED)
			return false;
	}
	{
		struct v34_phase3_s_detection detection;

		if (!v34_phase3_s_receiver_get_detection(
			    &rx->boundary, &detection))
			return false;
		rx->symbols.previous_clockwise_quarter_turns =
			detection.clockwise_quarter_turn_offset;
		rx->boundary_complete = true;
		if (!replay_post_marker_decisions(
			    rx, rx->boundary.post_marker_symbols, event))
			return false;
	}
	return true;
}

/* Borrow the immutable handoff history only during construction. Validate its
 * owned receiver once; every subsequent lookup still checks scalar bounds. */
struct decision_history_view {
	const struct v34_analytic_sample *decisions;
	size_t count;
	size_t oldest;
};

static bool decision_history_view_init(
	struct decision_history_view *view,
	const struct v90_upstream_training_rx *upstream)
{
	if (!v90_upstream_training_rx_is_valid(upstream))
		return false;
	view->decisions = upstream->recent_decisions;
	view->count = upstream->recent_decision_count;
	view->oldest = (upstream->next_recent_decision +
		V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS - view->count) %
		V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS;
	return true;
}

static bool history_decision(
	const struct decision_history_view *view, size_t index,
	struct v34_analytic_sample *decision)
{
	if (view->count > V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS ||
	    view->oldest >= V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS ||
	    index >= view->count)
		return false;
	*decision = view->decisions[(view->oldest + index) %
		V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS];
	return true;
}

static bool historical_boundary_at(
	const struct decision_history_view *history, size_t first,
	struct v34_phase3_s_detection *detection, size_t *boundary)
{
	struct v34_phase3_s_receiver marker;
	const size_t window = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS +
		V34_PHASE3_S_BAR_SYMBOLS;
	size_t end = first + window +
		V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS;

	if (end > history->count)
		end = history->count;
	if (v34_phase3_s_receiver_init_fixed_suffix(
		    &marker,
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
	    V34_PHASE3_S_RECEIVER_OK)
		return false;
	for (size_t index = first; index < end; ++index) {
		struct v34_analytic_sample decision;
		struct v34_fixed_training_decision hard;
		struct v34_phase3_s_adaptation ignored;
		enum v34_phase3_s_receiver_result result;

		if (!history_decision(
			    history, index, &decision) ||
		    v34_training_constellation_fixed_slice(
			    V34_CONSTELLATION_4_POINT, decision, &hard) !=
			    V34_CONSTELLATION_OK)
			return false;
		result = v34_phase3_s_receiver_receive_adaptive(
			&marker, hard.quarter_point,
			hard.clockwise_quarter_turns, decision, &ignored);
		if (result == V34_PHASE3_S_RECEIVER_OK)
			continue;
		if (result != V34_PHASE3_S_RECEIVER_DETECTED ||
		    !v34_phase3_s_receiver_get_detection(
			    &marker, detection))
			return false;
		*boundary = index + 1u - marker.post_marker_symbols;
		return *boundary == first + window;
	}
	return false;
}

static bool replay_historical_cp(
	struct v90_phase4_cp_line_rx *rx,
	const struct decision_history_view *history, size_t first)
{
	for (size_t index = first;
	     index < history->count; ++index) {
		struct v34_analytic_sample decision;
		struct v34_fixed_training_decision hard;
		struct v90_cp_receive_event event = { 0 };

		if (!history_decision(
			    history, index, &decision) ||
		    v34_training_constellation_fixed_slice(
			    rx->symbols.constellation, decision, &hard) !=
			    V34_CONSTELLATION_OK ||
		    v90_phase4_cp_rx_receive_decision(
			    &rx->symbols, hard.quarter_point,
			    hard.clockwise_quarter_turns, &event) != V90_CP_OK ||
		    event.type != V90_CP_RECEIVE_NO_EVENT)
			return false;
	}
	return true;
}

static bool seed_initial_boundary(
	struct v90_phase4_cp_line_rx *rx,
	const struct v90_upstream_training_rx *upstream)
{
	const size_t window = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS +
		V34_PHASE3_S_BAR_SYMBOLS;
	struct v34_phase3_s_detection latest_detection = { 0 };
	size_t latest_boundary = 0u;
	bool found = false;
	size_t first;
	struct decision_history_view history;

	if (!decision_history_view_init(&history, upstream))
		return false;
	if (history.count < 64u ||
	    history.count < window)
		return false;
	/* The semantic receiver can finish before or after this fixed decision
	 * path. Select the latest complete marker still in bounded history; an
	 * earlier V.90 return must not become the Phase 4 boundary. */
	/* Each accepted candidate ends at first + window. Search newest first
	 * so the first match is exactly the latest boundary, without replaying
	 * older candidates after it. Candidate detectors own no live state. */
	for (first = history.count - window + 1u;
	     first-- != 0u;) {
		struct v34_phase3_s_detection detection;
		size_t boundary;

		if (historical_boundary_at(
			    &history, first, &detection, &boundary)) {
			latest_detection = detection;
			latest_boundary = boundary;
			found = true;
			break;
		}
	}
	if (found) {
		rx->symbols.previous_clockwise_quarter_turns =
			latest_detection.clockwise_quarter_turn_offset;
		rx->boundary_complete = true;
		return replay_historical_cp(rx, &history, latest_boundary);
	}
	/* The fixed path can still be inside S-bar when the semantic path reports
	 * it. Prime one suffix near that known boundary and finish it on live
	 * samples, retaining any decisions consumed after the marker. */
	if (v34_phase3_s_receiver_init_fixed_suffix(
		    &rx->boundary,
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
	    V34_PHASE3_S_RECEIVER_OK)
		return false;
	first = history.count - 64u;
	for (size_t index = first;
	     index < history.count; ++index) {
		struct v34_analytic_sample decision;
		struct v34_fixed_training_decision hard;
		struct v34_phase3_s_adaptation adaptation;
		struct v90_cp_receive_event event;

		if (!history_decision(
			    &history, index, &decision) ||
		    !receive_fixed_decision(
			    rx, decision, &hard, &adaptation, &event) ||
		    event.type != V90_CP_RECEIVE_NO_EVENT)
			return false;
	}
	return true;
}

bool v90_phase4_cp_line_rx_has_failed(
	const struct v90_phase4_cp_line_rx *rx)
{
	return v90_phase4_cp_line_rx_is_valid(rx) && rx->failed;
}

bool v90_phase4_cp_line_rx_has_pending_record(
	const struct v90_phase4_cp_line_rx *rx)
{
	return v90_phase4_cp_line_rx_is_valid(rx) &&
	       !rx->failed && rx->record_pending;
}

bool v90_phase4_cp_line_rx_pending_record_is_valid(
	const struct v90_phase4_cp_line_rx *rx)
{
	return v90_phase4_cp_line_rx_has_pending_record(rx) &&
	       rx->pending_record_is_valid;
}

enum v90_phase4_cp_line_rx_result
v90_phase4_cp_line_rx_pending_parameters(
	const struct v90_phase4_cp_line_rx *rx,
	struct v90_cp_parameters *parameters)
{
	if (!v90_phase4_cp_line_rx_is_valid(rx) || parameters == NULL)
		return V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE4_CP_LINE_RX_FAILED;
	if (!rx->record_pending || !rx->pending_record_is_valid)
		return V90_PHASE4_CP_LINE_RX_INVALID_STATE;
	*parameters = rx->pending_parameters;
	return V90_PHASE4_CP_LINE_RX_OK;
}

enum v90_phase4_cp_line_rx_result v90_phase4_cp_line_rx_init(
	struct v90_phase4_cp_line_rx *rx,
	const struct v90_phase3_return_line_handoff *handoff,
	enum v34_constellation_size constellation)
{
	struct v90_phase4_cp_line_rx initialized = { 0 };

	if (rx == NULL || !v90_phase3_return_line_handoff_is_valid(handoff) ||
	    memory_regions_overlap(rx, sizeof(*rx), handoff, sizeof(*handoff)))
		return V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT;
	initialized.line = handoff->upstream.line;
	initialized.transmitting_profile = handoff->upstream.transmitting_profile;
	initialized.decoder_power_q15 =
		v90_upstream_training_rx_decoder_power(&handoff->upstream);
	initialized.adaptation = V34_ADAPTIVE_EQUALIZER_FULL;
	/* The training owner qualifies quiet return intervals. CP owns live
	 * control symbols, so do not inherit that owner's temporary hold. */
	if (v34_receive_timing_set_feedback_held(
		    &initialized.line.timing, false) != V34_RECEIVE_TIMING_OK)
		return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
	/* Continue conventional-sample level acquisition after Phase 3 training
	 * and any final-return drain. Freeze the gain only at a
	 * verified record, so a frontend gain change cannot poison E/B1. */
	if (v34_gain_control_set_mode(
		    &initialized.line.timing.gain, V34_GAIN_TRACKING) !=
	    V34_GAIN_CONTROL_OK)
		return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
	if (v90_phase4_cp_rx_init(
		    &initialized.symbols,
		    initialized.transmitting_profile.local_role,
		    constellation) != V90_CP_OK)
		return V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT;
	if (!seed_initial_boundary(&initialized, &handoff->upstream))
		return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
	if (!v90_phase4_cp_line_rx_is_valid(&initialized))
		return V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_PHASE4_CP_LINE_RX_OK;
}

enum v90_phase4_cp_line_rx_result v90_phase4_cp_line_rx_resume_online(
	struct v90_phase4_cp_line_rx *rx,
	const struct v34_data_line_receiver *online,
	const struct v90_phase4_cp_rx *control_symbols,
	enum v34_constellation_size constellation)
{
	struct v90_phase4_cp_line_rx resumed = { 0 };
	struct v34_phase3_s_detection detection;
	struct v90_cp_receive_event event = { 0 };

	if (rx == NULL || online == NULL || control_symbols == NULL ||
	    memory_regions_overlap(rx, sizeof(*rx), online, sizeof(*online)) ||
	    memory_regions_overlap(
		    rx, sizeof(*rx), control_symbols,
		    sizeof(*control_symbols)) ||
	    !v34_data_line_receiver_is_valid(online) ||
	    v34_data_line_receiver_has_failed(online) ||
	    !v90_phase4_cp_rx_is_valid(control_symbols) ||
	    !v34_constellation_size_is_valid(constellation) ||
	    control_symbols->transmitting_role !=
		    online->transmitting_profile.local_role)
		return V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT;
	if (!v34_data_line_receiver_detects_rate_renegotiation(online) ||
	    !v34_data_line_receiver_rate_renegotiation_is_complete(online) ||
	    !v34_data_line_receiver_get_rate_renegotiation_detection(
		    online, &detection))
		return V90_PHASE4_CP_LINE_RX_INVALID_STATE;

	resumed.line = online->line;
	if (v34_adaptive_equalizer_set_data_carrier_tracking(
		    &resumed.line.equalizer,
		    constellation == V34_CONSTELLATION_16_POINT) !=
	    V34_ADAPTIVE_EQUALIZER_OK)
		return V90_PHASE4_CP_LINE_RX_INVALID_STATE;
	resumed.symbols = *control_symbols;
	resumed.symbols.constellation = constellation;
	if (v90_cp_receiver_init(&resumed.symbols.records) != V90_CP_OK)
		return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
	/* The request S/S-bar is already complete. Its error-bounded detector
	 * may retain a few following CP decisions; consume them once, without
	 * replaying physical samples or waiting for another request marker. */
	if (!v34_phase3_s_hunt_get_post_marker_symbols(
		    &online->rate_renegotiation.s, &resumed.boundary_tail_count))
		return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
	for (uint8_t index = 0; index < resumed.boundary_tail_count; ++index)
		if (!v34_phase3_s_hunt_get_post_marker_observation(
			    &online->rate_renegotiation.s, index,
			    &resumed.boundary_tail[index]))
			return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
	resumed.boundary_complete = true;
	if (!replay_post_marker_decisions(
		    &resumed, resumed.boundary_tail_count, &event) ||
	    event.type != V90_CP_RECEIVE_NO_EVENT)
		return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
	resumed.transmitting_profile = online->transmitting_profile;
	resumed.decoder_power_q15 = V34_DATA_DECISION_DECODER_POWER_Q15;
	resumed.adaptation = constellation == V34_CONSTELLATION_16_POINT ?
		V34_ADAPTIVE_EQUALIZER_CARRIER : V34_ADAPTIVE_EQUALIZER_FULL;
	if (!v90_phase4_cp_line_rx_is_valid(&resumed))
		return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
	*rx = resumed;
	return V90_PHASE4_CP_LINE_RX_OK;
}

enum v90_phase4_cp_line_rx_result v90_phase4_cp_line_rx_continue(
	struct v90_phase4_cp_line_rx *rx)
{
	struct v90_phase4_cp_line_rx next;

	if (!v90_phase4_cp_line_rx_is_valid(rx))
		return V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE4_CP_LINE_RX_FAILED;
	if (!rx->record_pending)
		return V90_PHASE4_CP_LINE_RX_INVALID_STATE;
	next = *rx;
	next.record_pending = false;
	next.pending_record_is_valid = false;
	next.pending_parameters = (struct v90_cp_parameters){ 0 };
	if (!v90_phase4_cp_line_rx_is_valid(&next))
		return V90_PHASE4_CP_LINE_RX_INVALID_STATE;
	*rx = next;
	return V90_PHASE4_CP_LINE_RX_OK;
}

static enum v90_phase4_cp_line_rx_result fail_sample(
	struct v90_phase4_cp_line_rx *rx,
	struct v90_phase4_cp_line_rx *next,
	const struct v34_adaptive_equalizer_observation *observation)
{
	if (next->line.equalizer.decision_pending && observation != NULL &&
	    v34_adaptive_equalizer_discard(
		    &next->line.equalizer, observation) !=
		    V34_ADAPTIVE_EQUALIZER_OK) {
		rx->failed = true;
		return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
	}
	next->failed = true;
	if (v90_phase4_cp_line_rx_is_valid(next))
		*rx = *next;
	else
		rx->failed = true;
	return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
}

enum v90_phase4_cp_line_rx_result v90_phase4_cp_line_rx_sample(
	struct v90_phase4_cp_line_rx *rx, int16_t sample,
	struct v90_cp_receive_event *event,
	struct v34_fixed_training_decision *decision)
{
	struct v90_phase4_cp_line_rx next;
	struct v34_adaptive_equalizer_observation observation;
	struct v34_fixed_training_decision hard;
	struct v34_phase3_s_adaptation boundary_adaptation;
	struct v34_receive_action action;
	struct v90_cp_receive_event emitted = { 0 };
	enum v34_line_receiver_result line_result;
	bool boundary_was_complete;

	if (!v90_phase4_cp_line_rx_is_valid(rx) || event == NULL ||
	    memory_regions_overlap(rx, sizeof(*rx), event, sizeof(*event)) ||
	    (decision != NULL &&
	     (memory_regions_overlap(rx, sizeof(*rx), decision, sizeof(*decision)) ||
	      memory_regions_overlap(event, sizeof(*event), decision, sizeof(*decision)))))
		return V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE4_CP_LINE_RX_FAILED;
	if (rx->record_pending)
		return V90_PHASE4_CP_LINE_RX_INVALID_STATE;
	next = *rx;
	line_result = v34_line_receiver_sample(
		&next.line, sample, &observation);
	if (line_result == V34_LINE_RECEIVER_OK) {
		if (!v90_phase4_cp_line_rx_is_valid(&next))
			return fail_sample(rx, &next, NULL);
		*rx = next;
		return V90_PHASE4_CP_LINE_RX_OK;
	}
	if (line_result != V34_LINE_RECEIVER_OBSERVATION_READY)
		return fail_sample(rx, &next, NULL);
	boundary_was_complete = next.boundary_complete;
	if (!receive_fixed_decision(
		    &next, observation.decision, &hard,
		    &boundary_adaptation, &emitted))
		return fail_sample(rx, &next, &observation);
	if (!boundary_was_complete) {
		if (boundary_adaptation.adapt)
			action = v34_receive_action_commit(
				&observation, boundary_adaptation.target,
				next.decoder_power_q15,
				V34_ADAPTIVE_EQUALIZER_CENTER);
		else
			action =
				v34_receive_action_only(V34_RECEIVE_DISCARD);
	} else {
		/* A CP dropout must not teach the FIR an arbitrary hard decision.
		 * Keep the carrier and symbol history moving so a later CP or E can
		 * recover, and resume ordinary adaptation as soon as signal returns. */
		action = v34_receive_action_commit(
			&observation, hard.target,
			next.decoder_power_q15,
			next.adaptation == V34_ADAPTIVE_EQUALIZER_FULL &&
			!v34_training_constellation_has_signal(
				next.symbols.constellation, observation.decision) ?
				V34_ADAPTIVE_EQUALIZER_CARRIER : next.adaptation);
	}
	if (v34_line_receiver_resolve(
		    &next.line, &observation, &action,
		    next.decoder_power_q15) != V34_LINE_RECEIVER_OK)
		return fail_sample(rx, &next, &observation);
	if (emitted.type != V90_CP_RECEIVE_NO_EVENT) {
		next.record_pending = true;
		next.pending_record_is_valid =
			emitted.type == V90_CP_RECEIVE_FRAME &&
			emitted.frame_result == V90_CP_OK;
		next.pending_parameters = (struct v90_cp_parameters){ 0 };
		if (next.pending_record_is_valid)
			next.pending_parameters = emitted.parameters;
		/* A valid CP can finish while the AGC is still correcting an
		 * under-level training handoff. Settle that level from the owned
		 * signal history once, then preserve it through CP/E/B1. Rejected
		 * records and already frozen receivers cannot change the gain. */
		if (next.pending_record_is_valid &&
		    next.line.timing.gain.mode == V34_GAIN_TRACKING &&
		    (v34_gain_control_refresh_level(&next.line.timing.gain) !=
			     V34_GAIN_CONTROL_OK ||
		     v34_receive_timing_freeze_gain(&next.line.timing) !=
			     V34_RECEIVE_TIMING_OK))
			return fail_sample(rx, &next, NULL);
	}
	if (!v90_phase4_cp_line_rx_is_valid(&next))
		return fail_sample(rx, &next, NULL);
	*rx = next;
	if (decision != NULL && boundary_was_complete)
		*decision = hard;
	if (emitted.type != V90_CP_RECEIVE_NO_EVENT) {
		*event = emitted;
		return V90_PHASE4_CP_LINE_RX_RECORD_READY;
	}
	return V90_PHASE4_CP_LINE_RX_SYMBOL;
}

const char *v90_phase4_cp_line_rx_result_name(
	enum v90_phase4_cp_line_rx_result result)
{
	switch (result) {
	case V90_PHASE4_CP_LINE_RX_OK:
		return "ok";
	case V90_PHASE4_CP_LINE_RX_SYMBOL:
		return "symbol";
	case V90_PHASE4_CP_LINE_RX_RECORD_READY:
		return "record ready";
	case V90_PHASE4_CP_LINE_RX_FAILED:
		return "failed";
	case V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE4_CP_LINE_RX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
