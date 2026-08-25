// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_receiver.h"

#include "v34_constellation.h"
#include "v34_training_channel.h"

#include <math.h>
#include <stddef.h>

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

static bool adaptation_is_valid(double adaptation)
{
	return isfinite(adaptation) && adaptation >= 0.0 &&
	       adaptation <= 1.0;
}

bool v34_phase3_receiver_configuration_is_valid(
	const struct v34_phase3_receiver_configuration *configuration)
{
	return configuration != NULL &&
	       role_is_valid(configuration->transmitting_role) &&
	       configuration->md_symbols <=
		       V34_PHASE3_RECEIVER_MAX_MD_SYMBOLS &&
	       adaptation_is_valid(configuration->s_gain_adaptation) &&
	       adaptation_is_valid(configuration->pp_equalizer_adaptation) &&
	       adaptation_is_valid(
		       configuration->tracking_equalizer_adaptation);
}

bool v34_phase3_handoff_is_valid(
	const struct v34_phase3_handoff *handoff)
{
	return handoff != NULL &&
	       v34_phase3_s_detection_is_valid(&handoff->s) &&
	       v34_phase3_j_handoff_is_valid(&handoff->j);
}

bool v34_phase3_prefix_handoff_is_valid(
	const struct v34_phase3_prefix_handoff *handoff)
{
	return handoff != NULL &&
	       v34_phase3_s_detection_is_valid(&handoff->s) &&
	       v34_equalizer_handoff_is_valid(&handoff->equalizer);
}

static bool detection_is_clear(
	const struct v34_phase3_s_detection *detection)
{
	return detection->clockwise_quarter_turn_offset == 0 &&
	       detection->s_errors == 0 && detection->s_bar_errors == 0;
}

static bool s_state_is_valid(
	const struct v34_phase3_receiver *receiver, bool repeated)
{
	const struct v34_phase3_s_point_receiver *s = &receiver->state.s;

	if (!v34_phase3_s_receiver_is_valid(&s->detector) ||
	    v34_phase3_s_receiver_is_complete(&s->detector) ||
	    receiver->md_symbols_remaining != 0)
		return false;
	if (repeated) {
		if (receiver->configuration.md_symbols == 0 ||
		    !v34_phase3_s_detection_is_valid(
			    &receiver->s_detection))
			return false;
	} else if (!detection_is_clear(&receiver->s_detection)) {
		return false;
	}
	if (s->gain_seeded)
		return qam_gain_is_valid(&s->gain) &&
		       s->gain.adaptation ==
			       receiver->configuration.s_gain_adaptation;
	return s->gain.channel_gain.in_phase == 0.0 &&
	       s->gain.channel_gain.quadrature == 0.0 &&
	       s->gain.adaptation == 0.0;
}

bool v34_phase3_receiver_is_valid(
	const struct v34_phase3_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_phase3_receiver_configuration_is_valid(
		    &receiver->configuration))
		return false;
	switch (receiver->stage) {
	case V34_PHASE3_RECEIVER_STAGE_S:
		return s_state_is_valid(receiver, false);
	case V34_PHASE3_RECEIVER_STAGE_MD:
		return receiver->configuration.md_symbols != 0 &&
		       receiver->md_symbols_remaining != 0 &&
		       receiver->md_symbols_remaining <=
			       receiver->configuration.md_symbols &&
		       v34_phase3_s_detection_is_valid(
			       &receiver->s_detection);
	case V34_PHASE3_RECEIVER_STAGE_REPEATED_S:
		return s_state_is_valid(receiver, true);
	case V34_PHASE3_RECEIVER_STAGE_PP:
		return receiver->md_symbols_remaining == 0 &&
		       v34_phase3_s_detection_is_valid(
			       &receiver->s_detection) &&
		       v34_phase3_pp_receiver_is_valid(&receiver->state.pp) &&
		       !v34_phase3_pp_receiver_is_complete(
			       &receiver->state.pp) &&
		       receiver->state.pp.adaptation ==
			       receiver->configuration.pp_equalizer_adaptation;
	case V34_PHASE3_RECEIVER_STAGE_J:
		return receiver->md_symbols_remaining == 0 &&
		       v34_phase3_s_detection_is_valid(
			       &receiver->s_detection) &&
		       v34_phase3_j_equalizer_receiver_is_valid(
			       &receiver->state.j) &&
		       !v34_phase3_j_equalizer_receiver_is_complete(
			       &receiver->state.j) &&
		       receiver->state.j.detector.trn.role ==
			       receiver->configuration.transmitting_role &&
		       receiver->state.j.adaptation ==
			       receiver->configuration
				       .tracking_equalizer_adaptation;
	case V34_PHASE3_RECEIVER_STAGE_COMPLETE:
		return receiver->md_symbols_remaining == 0 &&
		       v34_phase3_s_detection_is_valid(
			       &receiver->s_detection) &&
		       v34_phase3_j_equalizer_receiver_is_complete(
			       &receiver->state.j) &&
		       receiver->state.j.detector.trn.role ==
			       receiver->configuration.transmitting_role &&
		       receiver->state.j.adaptation ==
			       receiver->configuration
				       .tracking_equalizer_adaptation;
	}
	return false;
}

bool v34_phase3_receiver_is_complete(
	const struct v34_phase3_receiver *receiver)
{
	return v34_phase3_receiver_is_valid(receiver) &&
	       receiver->stage == V34_PHASE3_RECEIVER_STAGE_COMPLETE;
}

enum v34_phase3_receiver_result v34_phase3_receiver_init(
	struct v34_phase3_receiver *receiver,
	const struct v34_phase3_receiver_configuration *configuration)
{
	return v34_phase3_receiver_init_suffix(
		receiver, configuration, V34_PHASE3_S_SYMBOLS);
}

enum v34_phase3_receiver_result v34_phase3_receiver_init_suffix(
	struct v34_phase3_receiver *receiver,
	const struct v34_phase3_receiver_configuration *configuration,
	uint8_t s_symbols)
{
	struct v34_phase3_receiver initialized = { 0 };

	if (receiver == NULL ||
	    !v34_phase3_receiver_configuration_is_valid(configuration))
		return V34_PHASE3_RECEIVER_INVALID_ARGUMENT;
	initialized.configuration = *configuration;
	initialized.stage = V34_PHASE3_RECEIVER_STAGE_S;
	if (v34_phase3_s_receiver_init_suffix(
		    &initialized.state.s.detector, s_symbols) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    !v34_phase3_receiver_is_valid(&initialized))
		return V34_PHASE3_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE3_RECEIVER_OK;
}

static enum v34_phase3_receiver_result receive_s(
	struct v34_phase3_receiver *receiver,
	const struct qam_point *received)
{
	struct v34_phase3_s_point_receiver *s = &receiver->state.s;
	struct qam_point reference = v34_rotated_point_zero(0);
	struct v34_phase3_s_detection detection;
	struct v34_phase3_pp_receiver pp;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;
	enum v34_phase3_s_receiver_result result;

	if (!s->gain_seeded) {
		if (qam_gain_init_from_reference(
			    &s->gain, received, &reference,
			    receiver->configuration.s_gain_adaptation) !=
		    QAM_GAIN_OK)
			return V34_PHASE3_RECEIVER_NUMERIC_FAILURE;
		s->gain_seeded = true;
	}
	if (v34_training_channel_decide(
		    &s->gain, V34_CONSTELLATION_4_POINT, received,
		    &quarter_point, &clockwise_quarter_turns) !=
	    V34_TRAINING_CHANNEL_OK)
		return V34_PHASE3_RECEIVER_NUMERIC_FAILURE;
	result = v34_phase3_s_receiver_receive(
		&s->detector, quarter_point, clockwise_quarter_turns);
	if (result == V34_PHASE3_S_RECEIVER_OK)
		return V34_PHASE3_RECEIVER_OK;
	if (result != V34_PHASE3_S_RECEIVER_DETECTED ||
	    !v34_phase3_s_receiver_get_detection(&s->detector, &detection))
		return V34_PHASE3_RECEIVER_NUMERIC_FAILURE;
	if (receiver->stage == V34_PHASE3_RECEIVER_STAGE_S &&
	    receiver->configuration.md_symbols != 0) {
		receiver->state = (union v34_phase3_receiver_state){ 0 };
		receiver->s_detection = detection;
		receiver->md_symbols_remaining =
			receiver->configuration.md_symbols;
		receiver->stage = V34_PHASE3_RECEIVER_STAGE_MD;
		return V34_PHASE3_RECEIVER_S_DETECTED;
	}
	if (v34_phase3_pp_receiver_init(
		    &pp,
		    receiver->configuration.pp_equalizer_adaptation) !=
		    V34_PHASE3_PP_RECEIVER_OK)
		return V34_PHASE3_RECEIVER_NUMERIC_FAILURE;
	receiver->state = (union v34_phase3_receiver_state){ 0 };
	receiver->state.pp = pp;
	receiver->s_detection = detection;
	receiver->stage = V34_PHASE3_RECEIVER_STAGE_PP;
	return receiver->configuration.md_symbols != 0 ?
		       V34_PHASE3_RECEIVER_REPEATED_S_DETECTED :
		       V34_PHASE3_RECEIVER_S_DETECTED;
}

static enum v34_phase3_receiver_result receive_md(
	struct v34_phase3_receiver *receiver)
{
	struct v34_phase3_s_point_receiver s = { 0 };

	receiver->md_symbols_remaining--;
	if (receiver->md_symbols_remaining != 0)
		return V34_PHASE3_RECEIVER_OK;
	if (v34_phase3_s_receiver_init(&s.detector) !=
	    V34_PHASE3_S_RECEIVER_OK)
		return V34_PHASE3_RECEIVER_NUMERIC_FAILURE;
	receiver->state = (union v34_phase3_receiver_state){ 0 };
	receiver->state.s = s;
	receiver->stage = V34_PHASE3_RECEIVER_STAGE_REPEATED_S;
	return V34_PHASE3_RECEIVER_MD_COMPLETE;
}

static enum v34_phase3_receiver_result receive_pp(
	struct v34_phase3_receiver *receiver,
	const struct qam_point *received)
{
	struct v34_equalizer_handoff pp_handoff;
	struct v34_phase3_j_equalizer_receiver j;
	enum v34_phase3_pp_receiver_result result =
		v34_phase3_pp_receive_point(&receiver->state.pp, received);

	if (result != V34_PHASE3_PP_RECEIVER_OK)
		return result == V34_PHASE3_PP_RECEIVER_COMPLETE ?
			       V34_PHASE3_RECEIVER_INVALID_STATE :
			       V34_PHASE3_RECEIVER_NUMERIC_FAILURE;
	if (!v34_phase3_pp_receiver_is_complete(&receiver->state.pp))
		return V34_PHASE3_RECEIVER_OK;
	if (v34_phase3_pp_receiver_handoff(
		    &receiver->state.pp, &pp_handoff) !=
		    V34_PHASE3_PP_RECEIVER_OK ||
	    v34_phase3_j_equalizer_receiver_init(
		    &j, &pp_handoff,
		    receiver->configuration.transmitting_role,
		    receiver->configuration.tracking_equalizer_adaptation) !=
		    V34_PHASE3_J_EQUALIZER_RECEIVER_OK)
		return V34_PHASE3_RECEIVER_NUMERIC_FAILURE;
	receiver->state = (union v34_phase3_receiver_state){ 0 };
	receiver->state.j = j;
	receiver->stage = V34_PHASE3_RECEIVER_STAGE_J;
	return V34_PHASE3_RECEIVER_PP_COMPLETE;
}

static enum v34_phase3_receiver_result receive_j(
	struct v34_phase3_receiver *receiver,
	const struct qam_point *received)
{
	enum v34_phase3_j_equalizer_receiver_result result =
		v34_phase3_j_equalizer_receive_point(
			&receiver->state.j, received);

	switch (result) {
	case V34_PHASE3_J_EQUALIZER_RECEIVER_OK:
		return V34_PHASE3_RECEIVER_OK;
	case V34_PHASE3_J_EQUALIZER_RECEIVER_DETECTED:
		receiver->stage = V34_PHASE3_RECEIVER_STAGE_COMPLETE;
		return V34_PHASE3_RECEIVER_J_DETECTED;
	case V34_PHASE3_J_EQUALIZER_RECEIVER_OUT_OF_RANGE:
		return V34_PHASE3_RECEIVER_OUT_OF_RANGE;
	case V34_PHASE3_J_EQUALIZER_RECEIVER_COMPLETE:
	case V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_STATE:
		return V34_PHASE3_RECEIVER_INVALID_STATE;
	case V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT:
	case V34_PHASE3_J_EQUALIZER_RECEIVER_NUMERIC_FAILURE:
		return V34_PHASE3_RECEIVER_NUMERIC_FAILURE;
	}
	return V34_PHASE3_RECEIVER_NUMERIC_FAILURE;
}

static bool result_commits(enum v34_phase3_receiver_result result)
{
	return result == V34_PHASE3_RECEIVER_OK ||
	       result == V34_PHASE3_RECEIVER_S_DETECTED ||
	       result == V34_PHASE3_RECEIVER_MD_COMPLETE ||
	       result == V34_PHASE3_RECEIVER_REPEATED_S_DETECTED ||
	       result == V34_PHASE3_RECEIVER_PP_COMPLETE ||
	       result == V34_PHASE3_RECEIVER_J_DETECTED;
}

enum v34_phase3_receiver_result v34_phase3_receive_point(
	struct v34_phase3_receiver *receiver,
	const struct qam_point *received)
{
	struct v34_phase3_receiver next;
	enum v34_phase3_receiver_result result;

	if (!v34_phase3_receiver_is_valid(receiver) ||
	    !point_is_finite(received))
		return V34_PHASE3_RECEIVER_INVALID_ARGUMENT;
	if (receiver->stage == V34_PHASE3_RECEIVER_STAGE_COMPLETE)
		return V34_PHASE3_RECEIVER_COMPLETE;
	next = *receiver;
	switch (next.stage) {
	case V34_PHASE3_RECEIVER_STAGE_S:
	case V34_PHASE3_RECEIVER_STAGE_REPEATED_S:
		result = receive_s(&next, received);
		break;
	case V34_PHASE3_RECEIVER_STAGE_MD:
		result = receive_md(&next);
		break;
	case V34_PHASE3_RECEIVER_STAGE_PP:
		result = receive_pp(&next, received);
		break;
	case V34_PHASE3_RECEIVER_STAGE_J:
		result = receive_j(&next, received);
		break;
	case V34_PHASE3_RECEIVER_STAGE_COMPLETE:
		return V34_PHASE3_RECEIVER_COMPLETE;
	default:
		return V34_PHASE3_RECEIVER_INVALID_ARGUMENT;
	}
	if (!result_commits(result))
		return result;
	if (!v34_phase3_receiver_is_valid(&next))
		return V34_PHASE3_RECEIVER_NUMERIC_FAILURE;
	*receiver = next;
	return result;
}

bool v34_phase3_receiver_get_s_detection(
	const struct v34_phase3_receiver *receiver,
	struct v34_phase3_s_detection *detection)
{
	if (!v34_phase3_receiver_is_valid(receiver) || detection == NULL ||
	    receiver->stage == V34_PHASE3_RECEIVER_STAGE_S)
		return false;
	*detection = receiver->s_detection;
	return true;
}

enum v34_phase3_receiver_result v34_phase3_receiver_prefix_handoff(
	const struct v34_phase3_receiver *receiver,
	struct v34_phase3_prefix_handoff *handoff)
{
	struct v34_phase3_prefix_handoff generated;

	if (!v34_phase3_receiver_is_valid(receiver) || handoff == NULL)
		return V34_PHASE3_RECEIVER_INVALID_ARGUMENT;
	if (receiver->stage != V34_PHASE3_RECEIVER_STAGE_J ||
	    receiver->state.j.detector.received_symbols != 0)
		return V34_PHASE3_RECEIVER_INVALID_STATE;
	generated.s = receiver->s_detection;
	generated.equalizer.equalizer = receiver->state.j.equalizer;
	if (!v34_phase3_prefix_handoff_is_valid(&generated))
		return V34_PHASE3_RECEIVER_INVALID_STATE;
	*handoff = generated;
	return V34_PHASE3_RECEIVER_OK;
}

enum v34_phase3_receiver_result v34_phase3_receiver_handoff(
	const struct v34_phase3_receiver *receiver,
	struct v34_phase3_handoff *handoff)
{
	struct v34_phase3_handoff generated;

	if (!v34_phase3_receiver_is_valid(receiver) || handoff == NULL)
		return V34_PHASE3_RECEIVER_INVALID_ARGUMENT;
	if (receiver->stage != V34_PHASE3_RECEIVER_STAGE_COMPLETE)
		return V34_PHASE3_RECEIVER_INVALID_STATE;
	generated.s = receiver->s_detection;
	if (v34_phase3_j_equalizer_receiver_handoff(
		    &receiver->state.j, &generated.j) !=
		    V34_PHASE3_J_EQUALIZER_RECEIVER_OK ||
	    !v34_phase3_handoff_is_valid(&generated))
		return V34_PHASE3_RECEIVER_INVALID_STATE;
	*handoff = generated;
	return V34_PHASE3_RECEIVER_OK;
}
