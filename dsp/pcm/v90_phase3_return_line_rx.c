// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_return_line_rx.h"

#include "v34_phase3_line_receiver.h"
#include "v34_training_line.h"
#include "v90_phase3.h"

#include <stddef.h>

static bool profile_is_analog(
	const struct v34_phase3_profile *profile)
{
	return v34_phase3_profile_is_valid(profile) &&
	       profile->local_role == V34_ANSWERING_STATION &&
	       profile->md_symbol_count == 0;
}

static bool channel_matches(
	const struct qam_demodulator *demodulator,
	const struct v34_phase3_profile *profile)
{
	return demodulator->channel.sample_rate == V90_PHASE3_SAMPLE_RATE &&
	       v34_training_line_channel_matches(
		       profile, &demodulator->channel);
}

bool v90_phase3_return_line_rx_is_valid(
	const struct v90_phase3_return_line_rx *rx)
{
	return rx != NULL && profile_is_analog(&rx->transmitting_profile) &&
	       qam_demodulator_is_valid(&rx->demodulator) &&
	       qam_demodulator_measure(&rx->demodulator).timing_locked &&
	       channel_matches(&rx->demodulator, &rx->transmitting_profile) &&
	       rx->demodulator.acquisition_window_symbols ==
		       V34_PHASE3_LINE_ACQUISITION_SYMBOLS &&
	       rx->demodulator.tracking_window_symbols ==
		       V34_PHASE3_LINE_TRACKING_SYMBOLS &&
	       v90_phase3_return_rx_is_valid(&rx->points);
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
	initialized.transmitting_profile = handoff->transmitting_profile;
	if (v90_phase3_return_rx_init(
		    &initialized.points, &handoff->phase3.equalizer,
		    adaptation) != V90_PHASE3_RETURN_RX_OK ||
	    !v90_phase3_return_line_rx_is_valid(&initialized))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	*rx = initialized;
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
	bool ready;
	enum v90_phase3_return_line_rx_result result;

	if (!v90_phase3_return_line_rx_is_valid(rx))
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE3_RETURN_LINE_RX_FAILED;
	if (v90_phase3_return_rx_is_complete(&rx->points))
		return V90_PHASE3_RETURN_LINE_RX_COMPLETE;
	next = *rx;
	if (qam_demodulator_sample(
		    &next.demodulator, sample, &point, &ready) != QAM_OK) {
		rx->failed = true;
		return V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE;
	}
	result = ready ? point_result(v90_phase3_return_rx_point(
				 &next.points, &point)) :
			 V90_PHASE3_RETURN_LINE_RX_OK;
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

bool v90_phase3_return_line_handoff_is_valid(
	const struct v90_phase3_return_line_handoff *handoff)
{
	return handoff != NULL &&
	       profile_is_analog(&handoff->transmitting_profile) &&
	       qam_demodulator_is_valid(&handoff->demodulator) &&
	       qam_demodulator_measure(&handoff->demodulator).timing_locked &&
	       channel_matches(
		       &handoff->demodulator,
		       &handoff->transmitting_profile) &&
	       handoff->demodulator.acquisition_window_symbols ==
		       V34_PHASE3_LINE_ACQUISITION_SYMBOLS &&
	       handoff->demodulator.tracking_window_symbols ==
		       V34_PHASE3_LINE_TRACKING_SYMBOLS &&
	       v90_phase3_return_handoff_is_valid(&handoff->phase3);
}

enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_handoff(
	const struct v90_phase3_return_line_rx *rx,
	struct v90_phase3_return_line_handoff *handoff)
{
	struct v90_phase3_return_line_handoff generated;

	if (!v90_phase3_return_line_rx_is_valid(rx) || handoff == NULL)
		return V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed || !v90_phase3_return_rx_is_complete(&rx->points))
		return rx->failed ? V90_PHASE3_RETURN_LINE_RX_FAILED :
				    V90_PHASE3_RETURN_LINE_RX_INVALID_STATE;
	generated.demodulator = rx->demodulator;
	generated.transmitting_profile = rx->transmitting_profile;
	if (v90_phase3_return_rx_handoff(
		    &rx->points, &generated.phase3) !=
		    V90_PHASE3_RETURN_RX_OK ||
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
