// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_analog_line_rx.h"

#include <stddef.h>

bool v90_phase3_analog_line_rx_is_valid(
	const struct v90_phase3_analog_line_rx *rx)
{
	if (rx == NULL ||
	    !v90_phase3_demodulator_matches(
		    &rx->demodulator, &rx->transmitting_profile) ||
	    !v90_phase3_analog_rx_is_valid(&rx->points))
		return false;
	if (rx->points.stage == V90_PHASE3_ANALOG_RX_STAGE_S)
		return rx->points.state.s.detector.s_symbols ==
		       V90_PHASE3_LINE_S_SUFFIX_SYMBOLS;
	return qam_demodulator_has_timing_lock(&rx->demodulator);
}

bool v90_phase3_analog_line_rx_has_failed(
	const struct v90_phase3_analog_line_rx *rx)
{
	return v90_phase3_analog_line_rx_is_valid(rx) && rx->failed;
}

enum v90_phase3_analog_line_rx_result v90_phase3_analog_line_rx_init(
	struct v90_phase3_analog_line_rx *rx,
	const struct v34_phase3_profile *analog_profile,
	const struct v90_phase3_analog_rx_configuration *configuration)
{
	struct v90_phase3_analog_line_rx initialized = { 0 };
	struct qam_channel channel;
	enum v34_line_channel_result channel_result;

	if (rx == NULL ||
	    !v90_phase3_analog_profile_is_valid(analog_profile) ||
	    !v90_phase3_analog_rx_configuration_is_valid(configuration))
		return V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT;
	channel_result = v34_line_channel_init(
		analog_profile, V90_PHASE3_SAMPLE_RATE, &channel);
	if (channel_result != V34_LINE_CHANNEL_OK)
		return channel_result == V34_LINE_CHANNEL_OUT_OF_RANGE ?
			       V90_PHASE3_ANALOG_LINE_RX_OUT_OF_RANGE :
			       V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT;
	initialized.transmitting_profile = *analog_profile;
	if (qam_demodulator_init_with_timing_windows(
		    &initialized.demodulator, &channel,
		    V90_PHASE3_LINE_ACQUISITION_SYMBOLS,
		    V90_PHASE3_LINE_TRACKING_SYMBOLS) != QAM_OK ||
	    v90_phase3_analog_rx_init_suffix(
		    &initialized.points, configuration,
		    V90_PHASE3_LINE_S_SUFFIX_SYMBOLS) !=
		    V90_PHASE3_ANALOG_RX_OK ||
	    !v90_phase3_analog_line_rx_is_valid(&initialized))
		return V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_PHASE3_ANALOG_LINE_RX_OK;
}

static enum v90_phase3_analog_line_rx_result point_result(
	enum v90_phase3_analog_rx_result result)
{
	switch (result) {
	case V90_PHASE3_ANALOG_RX_OK:
		return V90_PHASE3_ANALOG_LINE_RX_OK;
	case V90_PHASE3_ANALOG_RX_S_DETECTED:
		return V90_PHASE3_ANALOG_LINE_RX_S_DETECTED;
	case V90_PHASE3_ANALOG_RX_PP_COMPLETE:
		return V90_PHASE3_ANALOG_LINE_RX_PP_COMPLETE;
	case V90_PHASE3_ANALOG_RX_JA_REJECTED:
		return V90_PHASE3_ANALOG_LINE_RX_JA_REJECTED;
	case V90_PHASE3_ANALOG_RX_JA_DETECTED:
		return V90_PHASE3_ANALOG_LINE_RX_JA_DETECTED;
	case V90_PHASE3_ANALOG_RX_OUT_OF_RANGE:
		return V90_PHASE3_ANALOG_LINE_RX_OUT_OF_RANGE;
	case V90_PHASE3_ANALOG_RX_COMPLETE:
	case V90_PHASE3_ANALOG_RX_INVALID_ARGUMENT:
	case V90_PHASE3_ANALOG_RX_INVALID_STATE:
	case V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE:
		break;
	}
	return V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE;
}

static bool result_is_terminal(
	enum v90_phase3_analog_line_rx_result result)
{
	return result == V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE ||
	       result == V90_PHASE3_ANALOG_LINE_RX_OUT_OF_RANGE;
}

enum v90_phase3_analog_line_rx_result v90_phase3_analog_line_rx_sample(
	struct v90_phase3_analog_line_rx *rx, int16_t sample)
{
	struct v90_phase3_analog_line_rx next;
	struct qam_point point;
	enum v90_phase3_analog_line_rx_result result;
	bool was_locked;
	bool ready;

	if (!v90_phase3_analog_line_rx_is_valid(rx))
		return V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE3_ANALOG_LINE_RX_FAILED;
	if (v90_phase3_analog_rx_is_complete(&rx->points))
		return V90_PHASE3_ANALOG_LINE_RX_COMPLETE;
	next = *rx;
	was_locked = qam_demodulator_has_timing_lock(&next.demodulator);
	if (qam_demodulator_sample(
		    &next.demodulator, sample, &point, &ready) != QAM_OK) {
		rx->failed = true;
		return V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE;
	}
	if (!ready) {
		if (!v90_phase3_analog_line_rx_is_valid(&next)) {
			rx->failed = true;
			return V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE;
		}
		*rx = next;
		return !was_locked &&
			       qam_demodulator_has_timing_lock(
				       &next.demodulator) ?
			       V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED :
			       V90_PHASE3_ANALOG_LINE_RX_OK;
	}
	result = point_result(v90_phase3_analog_rx_point(&next.points, &point));
	if (result_is_terminal(result)) {
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
	       v90_phase3_demodulator_is_locked(
		       &handoff->demodulator,
		       &handoff->transmitting_profile) &&
	       v90_phase3_analog_handoff_is_valid(&handoff->phase3);
}

enum v90_phase3_analog_line_rx_result v90_phase3_analog_line_rx_handoff(
	const struct v90_phase3_analog_line_rx *rx,
	struct v90_phase3_analog_line_handoff *handoff)
{
	struct v90_phase3_analog_line_handoff generated;

	if (!v90_phase3_analog_line_rx_is_valid(rx) || handoff == NULL)
		return V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed || !v90_phase3_analog_rx_is_complete(&rx->points))
		return rx->failed ? V90_PHASE3_ANALOG_LINE_RX_FAILED :
				    V90_PHASE3_ANALOG_LINE_RX_INVALID_STATE;
	generated.demodulator = rx->demodulator;
	generated.transmitting_profile = rx->transmitting_profile;
	if (v90_phase3_analog_rx_handoff(
		    &rx->points, &generated.phase3) != V90_PHASE3_ANALOG_RX_OK ||
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
