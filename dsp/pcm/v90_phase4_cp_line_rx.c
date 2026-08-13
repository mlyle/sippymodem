// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_cp_line_rx.h"

#include "v34_phase3_line_receiver.h"
#include "v34_training_line.h"
#include "v90_phase3.h"

#include <math.h>
#include <stddef.h>

static bool profile_is_analog(
	const struct v34_phase3_profile *profile)
{
	return v34_phase3_profile_is_valid(profile) &&
	       profile->local_role == V34_ANSWERING_STATION &&
	       profile->md_symbol_count == 0;
}

static bool channel_matches(
	const struct v90_phase4_cp_line_rx *rx)
{
	return rx->demodulator.channel.sample_rate == V90_PHASE3_SAMPLE_RATE &&
	       v34_training_line_channel_matches(
		       &rx->transmitting_profile,
		       &rx->demodulator.channel) &&
	       rx->demodulator.acquisition_window_symbols ==
		       V34_PHASE3_LINE_ACQUISITION_SYMBOLS &&
	       rx->demodulator.tracking_window_symbols ==
		       V34_PHASE3_LINE_TRACKING_SYMBOLS;
}

static bool equalizer_is_at_handoff(
	const struct qam_equalizer *equalizer)
{
	return qam_equalizer_is_valid(equalizer) &&
	       equalizer->fill == QAM_EQUALIZER_DELAY + 1u &&
	       !equalizer->can_adapt;
}

bool v90_phase4_cp_line_rx_is_valid(
	const struct v90_phase4_cp_line_rx *rx)
{
	return rx != NULL &&
	       profile_is_analog(&rx->transmitting_profile) &&
	       qam_demodulator_is_valid(&rx->demodulator) &&
	       qam_demodulator_measure(&rx->demodulator).timing_locked &&
	       channel_matches(rx) &&
	       equalizer_is_at_handoff(&rx->equalizer) &&
	       v90_phase4_cp_rx_is_valid(&rx->symbols) &&
	       rx->symbols.transmitting_role ==
		       rx->transmitting_profile.local_role &&
	       isfinite(rx->adaptation) && rx->adaptation >= 0.0 &&
	       rx->adaptation <= 1.0 &&
	       (!rx->pending_record_is_valid || rx->record_pending);
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
	    !isfinite(adaptation) || adaptation < 0.0 || adaptation > 1.0)
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
