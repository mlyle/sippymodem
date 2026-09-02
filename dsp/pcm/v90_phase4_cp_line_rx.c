// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_cp_line_rx.h"

#include "memory_region.h"
#include "v90_phase3.h"

#include <stddef.h>

bool v90_phase4_cp_line_rx_is_valid(
	const struct v90_phase4_cp_line_rx *rx)
{
	return rx != NULL &&
	       v90_phase3_demodulator_is_locked(
		       &rx->demodulator, &rx->transmitting_profile) &&
	       qam_equalizer_is_handoff_ready(&rx->equalizer) &&
	       v90_phase4_cp_rx_is_valid(&rx->symbols) &&
	       rx->symbols.transmitting_role ==
		       rx->transmitting_profile.local_role &&
	       qam_adaptation_is_valid(rx->adaptation) &&
	       (!rx->pending_record_is_valid ||
		(rx->record_pending &&
		 v90_cp_parameters_are_valid(&rx->pending_parameters)));
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
	enum v34_constellation_size constellation, double adaptation)
{
	struct v90_phase4_cp_line_rx initialized = {
		.adaptation = adaptation,
	};

	if (rx == NULL ||
	    !v90_phase3_return_line_handoff_is_valid(handoff) ||
	    !qam_adaptation_is_valid(adaptation))
		return V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT;
	initialized.demodulator = handoff->demodulator;
	initialized.equalizer = handoff->phase3.equalizer.equalizer;
	initialized.transmitting_profile = handoff->transmitting_profile;
	if (v90_phase4_cp_rx_init(
		    &initialized.symbols,
		    initialized.transmitting_profile.local_role,
		    constellation) != V90_CP_OK)
		return V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT;
	/* The equalizer intentionally preserves the fixed quadrant ambiguity
	 * measured from S/S-bar. Seed the differential CP decoder with that
	 * received phase so the first CPt dibit is independent of line delay. */
	initialized.symbols.previous_clockwise_quarter_turns =
		handoff->phase3.marker.clockwise_quarter_turn_offset;
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
	const struct v34_data_rate_renegotiation_monitor *monitor;

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
	monitor = &online->equalizer_receiver.rate_renegotiation;
	if (!monitor->rate_renegotiation_enabled ||
	    !v34_phase3_s_receiver_is_complete(
		    &monitor->rate_renegotiation_marker) ||
	    !qam_equalizer_is_handoff_ready(
		    &online->equalizer_receiver.equalizer))
		return V90_PHASE4_CP_LINE_RX_INVALID_STATE;

	resumed.demodulator = online->line;
	resumed.equalizer = online->equalizer_receiver.equalizer;
	resumed.symbols = *control_symbols;
	resumed.symbols.constellation = constellation;
	if (v90_cp_receiver_init(&resumed.symbols.records) != V90_CP_OK)
		return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
	resumed.transmitting_profile = online->transmitting_profile;
	resumed.adaptation = online->equalizer_receiver.adaptation;
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
	struct v90_phase4_cp_line_rx *next)
{
	next->failed = true;
	if (v90_phase4_cp_line_rx_is_valid(next))
		*rx = *next;
	else
		rx->failed = true;
	return V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE;
}

enum v90_phase4_cp_line_rx_result v90_phase4_cp_line_rx_sample(
	struct v90_phase4_cp_line_rx *rx, int16_t sample,
	struct v90_cp_receive_event *event)
{
	struct v90_phase4_cp_line_rx next;
	struct qam_equalizer_observation observation = { 0 };
	struct v90_cp_receive_event emitted = { 0 };
	struct qam_point received;
	struct qam_point target;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;
	bool carrier_ready;
	bool equalizer_ready;

	if (!v90_phase4_cp_line_rx_is_valid(rx) || event == NULL)
		return V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE4_CP_LINE_RX_FAILED;
	if (rx->record_pending)
		return V90_PHASE4_CP_LINE_RX_INVALID_STATE;
	next = *rx;
	if (qam_demodulator_sample(
		    &next.demodulator, sample, &received,
		    &carrier_ready) != QAM_OK)
		return fail_sample(rx, &next);
	if (!carrier_ready) {
		if (!v90_phase4_cp_line_rx_is_valid(&next))
			return fail_sample(rx, &next);
		*rx = next;
		return V90_PHASE4_CP_LINE_RX_OK;
	}
	if (qam_equalizer_observe(
		    &next.equalizer, &received, &observation,
		    &equalizer_ready) != QAM_EQUALIZER_OK ||
	    !equalizer_ready ||
	    v34_training_constellation_slice(
		    next.symbols.constellation, &observation.output,
		    &quarter_point, &clockwise_quarter_turns) !=
		    V34_CONSTELLATION_OK ||
	    v34_training_constellation_point(
		    next.symbols.constellation, quarter_point,
		    clockwise_quarter_turns, &target) !=
		    V34_CONSTELLATION_OK ||
	    v90_phase4_cp_rx_receive_decision(
		    &next.symbols, quarter_point,
		    clockwise_quarter_turns, &emitted) != V90_CP_OK ||
	    qam_equalizer_adapt_observation(
		    &next.equalizer, &observation, &target,
		    next.adaptation) != QAM_EQUALIZER_OK)
		return fail_sample(rx, &next);
	if (emitted.type != V90_CP_RECEIVE_NO_EVENT) {
		next.record_pending = true;
		next.pending_record_is_valid =
			emitted.type == V90_CP_RECEIVE_FRAME &&
			emitted.frame_result == V90_CP_OK;
		next.pending_parameters = (struct v90_cp_parameters){ 0 };
		if (next.pending_record_is_valid)
			next.pending_parameters = emitted.parameters;
	}
	if (!v90_phase4_cp_line_rx_is_valid(&next))
		return fail_sample(rx, &next);
	*rx = next;
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
