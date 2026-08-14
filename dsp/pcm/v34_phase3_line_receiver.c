// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_line_receiver.h"

#include "v34_training_line.h"

#include <math.h>
#include <stddef.h>

static const double V34_TIMING_POWER_HYSTERESIS = 1.01;

static bool adaptation_is_valid(double adaptation)
{
	return isfinite(adaptation) && adaptation >= 0.0 &&
	       adaptation <= 1.0;
}

bool v34_phase3_line_receiver_configuration_is_valid(
	const struct v34_phase3_line_receiver_configuration *configuration)
{
	return configuration != NULL &&
	       adaptation_is_valid(configuration->s_gain_adaptation) &&
	       adaptation_is_valid(configuration->pp_equalizer_adaptation) &&
	       adaptation_is_valid(
		       configuration->tracking_equalizer_adaptation);
}

static bool profile_matches_receiver(
	const struct v34_phase3_line_receiver *receiver)
{
	return receiver->points.configuration.transmitting_role ==
		       receiver->transmitting_profile.local_role &&
	       receiver->points.configuration.md_symbols ==
		       receiver->transmitting_profile.md_symbol_count &&
	       v34_training_line_channel_matches(
		       &receiver->transmitting_profile,
		       &receiver->demodulator.channel);
}

bool v34_phase3_line_receiver_is_valid(
	const struct v34_phase3_line_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_phase3_profile_is_valid(&receiver->transmitting_profile) ||
	    !qam_demodulator_is_valid(&receiver->demodulator) ||
	    !v34_phase3_receiver_is_valid(&receiver->points) ||
	    !profile_matches_receiver(receiver) ||
	    receiver->demodulator.acquisition_window_symbols !=
		    V34_PHASE3_LINE_ACQUISITION_SYMBOLS ||
	    receiver->demodulator.tracking_window_symbols !=
		    V34_PHASE3_LINE_TRACKING_SYMBOLS)
		return false;
	if (receiver->points.stage == V34_PHASE3_RECEIVER_STAGE_S)
		return receiver->points.state.s.detector.s_symbols ==
		       V34_PHASE3_LINE_S_SUFFIX_SYMBOLS;
	return qam_demodulator_measure(&receiver->demodulator).timing_locked;
}

bool v34_phase3_line_receiver_has_failed(
	const struct v34_phase3_line_receiver *receiver)
{
	return v34_phase3_line_receiver_is_valid(receiver) &&
	       receiver->failed;
}

bool v34_phase3_line_handoff_is_valid(
	const struct v34_phase3_line_handoff *handoff)
{
	return handoff != NULL &&
	       v34_phase3_profile_is_valid(&handoff->transmitting_profile) &&
	       qam_demodulator_is_valid(&handoff->demodulator) &&
	       qam_demodulator_measure(&handoff->demodulator).timing_locked &&
	       v34_training_line_channel_matches(
		       &handoff->transmitting_profile,
		       &handoff->demodulator.channel) &&
	       v34_phase3_handoff_is_valid(&handoff->phase3);
}

enum v34_phase3_line_receiver_result v34_phase3_line_receiver_init(
	struct v34_phase3_line_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	uint32_t sample_rate,
	const struct v34_phase3_line_receiver_configuration *configuration)
{
	struct v34_phase3_line_receiver initialized = { 0 };
	struct v34_phase3_receiver_configuration point_configuration;
	struct qam_channel channel;
	enum v34_training_line_result channel_result;

	if (receiver == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile) ||
	    !v34_phase3_line_receiver_configuration_is_valid(configuration))
		return V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT;
	channel_result = v34_training_line_channel(
		transmitting_profile, sample_rate, &channel);
	if (channel_result != V34_TRAINING_LINE_OK)
		return channel_result == V34_TRAINING_LINE_OUT_OF_RANGE ?
			       V34_PHASE3_LINE_RECEIVER_OUT_OF_RANGE :
			       V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT;
	point_configuration = (struct v34_phase3_receiver_configuration){
		.transmitting_role = transmitting_profile->local_role,
		.md_symbols = transmitting_profile->md_symbol_count,
		.s_gain_adaptation = configuration->s_gain_adaptation,
		.pp_equalizer_adaptation =
			configuration->pp_equalizer_adaptation,
		.tracking_equalizer_adaptation =
			configuration->tracking_equalizer_adaptation,
	};
	initialized.transmitting_profile = *transmitting_profile;
	if (qam_demodulator_init_with_timing_windows(
		    &initialized.demodulator, &channel,
		    V34_PHASE3_LINE_ACQUISITION_SYMBOLS,
		    V34_PHASE3_LINE_TRACKING_SYMBOLS) != QAM_OK ||
	    qam_demodulator_set_timing_power_hysteresis(
		    &initialized.demodulator,
		    V34_TIMING_POWER_HYSTERESIS) != QAM_OK ||
	    v34_phase3_receiver_init_suffix(
		    &initialized.points, &point_configuration,
		    V34_PHASE3_LINE_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_RECEIVER_OK ||
	    !v34_phase3_line_receiver_is_valid(&initialized))
		return V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE3_LINE_RECEIVER_OK;
}

static enum v34_phase3_line_receiver_result point_result(
	enum v34_phase3_receiver_result result)
{
	switch (result) {
	case V34_PHASE3_RECEIVER_OK:
		return V34_PHASE3_LINE_RECEIVER_OK;
	case V34_PHASE3_RECEIVER_S_DETECTED:
		return V34_PHASE3_LINE_RECEIVER_S_DETECTED;
	case V34_PHASE3_RECEIVER_MD_COMPLETE:
		return V34_PHASE3_LINE_RECEIVER_MD_COMPLETE;
	case V34_PHASE3_RECEIVER_REPEATED_S_DETECTED:
		return V34_PHASE3_LINE_RECEIVER_REPEATED_S_DETECTED;
	case V34_PHASE3_RECEIVER_PP_COMPLETE:
		return V34_PHASE3_LINE_RECEIVER_PP_COMPLETE;
	case V34_PHASE3_RECEIVER_J_DETECTED:
		return V34_PHASE3_LINE_RECEIVER_J_DETECTED;
	case V34_PHASE3_RECEIVER_OUT_OF_RANGE:
		return V34_PHASE3_LINE_RECEIVER_OUT_OF_RANGE;
	case V34_PHASE3_RECEIVER_COMPLETE:
	case V34_PHASE3_RECEIVER_INVALID_STATE:
	case V34_PHASE3_RECEIVER_INVALID_ARGUMENT:
	case V34_PHASE3_RECEIVER_NUMERIC_FAILURE:
		break;
	}
	return V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE;
}

static bool result_is_terminal(
	enum v34_phase3_line_receiver_result result)
{
	return result == V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE ||
	       result == V34_PHASE3_LINE_RECEIVER_OUT_OF_RANGE;
}

enum v34_phase3_line_receiver_result v34_phase3_line_receiver_sample(
	struct v34_phase3_line_receiver *receiver, int16_t sample)
{
	struct v34_phase3_line_receiver next;
	struct qam_point point;
	enum v34_phase3_line_receiver_result result;
	bool was_locked;
	bool ready;

	if (!v34_phase3_line_receiver_is_valid(receiver))
		return V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_PHASE3_LINE_RECEIVER_FAILED;
	if (v34_phase3_receiver_is_complete(&receiver->points))
		return V34_PHASE3_LINE_RECEIVER_COMPLETE;
	next = *receiver;
	was_locked = qam_demodulator_measure(&next.demodulator).timing_locked;
	if (qam_demodulator_sample(
		    &next.demodulator, sample, &point, &ready) != QAM_OK) {
		receiver->failed = true;
		return V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE;
	}
	if (!ready) {
		if (!v34_phase3_line_receiver_is_valid(&next)) {
			receiver->failed = true;
			return V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE;
		}
		*receiver = next;
		return !was_locked &&
			       qam_demodulator_measure(&next.demodulator).timing_locked ?
			       V34_PHASE3_LINE_RECEIVER_TIMING_ACQUIRED :
			       V34_PHASE3_LINE_RECEIVER_OK;
	}
	result = point_result(v34_phase3_receive_point(&next.points, &point));
	if (result_is_terminal(result)) {
		next.failed = true;
		if (v34_phase3_line_receiver_is_valid(&next))
			*receiver = next;
		else
			receiver->failed = true;
		return result;
	}
	if (!v34_phase3_line_receiver_is_valid(&next)) {
		receiver->failed = true;
		return V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE;
	}
	*receiver = next;
	return result;
}

enum v34_phase3_line_receiver_result
v34_phase3_line_receiver_track_sample(
	struct v34_phase3_line_receiver *receiver, int16_t sample)
{
	struct v34_phase3_line_receiver next;
	struct qam_point discarded;
	bool ready;

	if (!v34_phase3_line_receiver_is_valid(receiver))
		return V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_PHASE3_LINE_RECEIVER_FAILED;
	if (!v34_phase3_receiver_is_complete(&receiver->points))
		return V34_PHASE3_LINE_RECEIVER_INVALID_STATE;
	next = *receiver;
	if (qam_demodulator_sample(
		    &next.demodulator, sample, &discarded, &ready) != QAM_OK ||
	    !v34_phase3_line_receiver_is_valid(&next)) {
		receiver->failed = true;
		return V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE;
	}
	*receiver = next;
	return V34_PHASE3_LINE_RECEIVER_OK;
}

enum v34_phase3_line_receiver_result v34_phase3_line_receiver_handoff(
	const struct v34_phase3_line_receiver *receiver,
	struct v34_phase3_line_handoff *handoff)
{
	struct v34_phase3_line_handoff generated;

	if (!v34_phase3_line_receiver_is_valid(receiver) || handoff == NULL)
		return V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed ||
	    !v34_phase3_receiver_is_complete(&receiver->points))
		return receiver->failed ? V34_PHASE3_LINE_RECEIVER_FAILED :
					  V34_PHASE3_LINE_RECEIVER_INVALID_STATE;
	generated.demodulator = receiver->demodulator;
	generated.transmitting_profile = receiver->transmitting_profile;
	if (v34_phase3_receiver_handoff(
		    &receiver->points, &generated.phase3) !=
		    V34_PHASE3_RECEIVER_OK ||
	    !v34_phase3_line_handoff_is_valid(&generated))
		return V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE;
	*handoff = generated;
	return V34_PHASE3_LINE_RECEIVER_OK;
}

const char *v34_phase3_line_receiver_result_name(
	enum v34_phase3_line_receiver_result result)
{
	switch (result) {
	case V34_PHASE3_LINE_RECEIVER_OK:
		return "ok";
	case V34_PHASE3_LINE_RECEIVER_TIMING_ACQUIRED:
		return "timing acquired";
	case V34_PHASE3_LINE_RECEIVER_S_DETECTED:
		return "S detected";
	case V34_PHASE3_LINE_RECEIVER_MD_COMPLETE:
		return "MD complete";
	case V34_PHASE3_LINE_RECEIVER_REPEATED_S_DETECTED:
		return "repeated S detected";
	case V34_PHASE3_LINE_RECEIVER_PP_COMPLETE:
		return "PP complete";
	case V34_PHASE3_LINE_RECEIVER_J_DETECTED:
		return "J detected";
	case V34_PHASE3_LINE_RECEIVER_COMPLETE:
		return "complete";
	case V34_PHASE3_LINE_RECEIVER_FAILED:
		return "failed";
	case V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE3_LINE_RECEIVER_INVALID_STATE:
		return "invalid state";
	case V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case V34_PHASE3_LINE_RECEIVER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
