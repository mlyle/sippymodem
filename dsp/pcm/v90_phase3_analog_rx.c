// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_analog_rx.h"

#include "v34_constellation.h"
#include "v34_training_sequence.h"
#include "v34_training_channel.h"

#include <math.h>
#include <stddef.h>

static bool adaptation_is_valid(double adaptation)
{
	return isfinite(adaptation) && adaptation >= 0.0 &&
	       adaptation <= 1.0;
}

bool v90_phase3_analog_rx_configuration_is_valid(
	const struct v90_phase3_analog_rx_configuration *configuration)
{
	return configuration != NULL &&
	       adaptation_is_valid(configuration->s_gain_adaptation) &&
	       adaptation_is_valid(configuration->pp_equalizer_adaptation) &&
	       adaptation_is_valid(configuration->tracking_equalizer_adaptation);
}

static bool detection_is_clear(
	const struct v34_phase3_s_detection *detection)
{
	return detection->clockwise_quarter_turn_offset == 0 &&
	       detection->s_errors == 0 && detection->s_bar_errors == 0;
}

static bool s_receiver_is_valid(
	const struct v90_phase3_s_point_receiver *receiver,
	double adaptation)
{
	if (!v34_phase3_s_receiver_is_valid(&receiver->detector) ||
	    v34_phase3_s_receiver_is_complete(&receiver->detector))
		return false;
	if (receiver->gain_seeded)
		return qam_gain_is_valid(&receiver->gain) &&
		       receiver->gain.adaptation == adaptation;
	return receiver->gain.channel_gain.in_phase == 0.0 &&
	       receiver->gain.channel_gain.quadrature == 0.0 &&
	       receiver->gain.adaptation == 0.0;
}

static bool search_is_initial(const struct v90_ja_search *search)
{
	return search->descrambler.history == 0 &&
	       search->descrambler.polynomial == V34_POLYNOMIAL_GPA &&
	       search->bit_index == 0 && search->warmup_bits == 0 &&
	       search->consecutive_ones == 0 &&
	       !search->has_previous_rotation && !search->collecting &&
	       !search->complete;
}

static bool ja_receiver_is_valid(
	const struct v90_phase3_ja_point_receiver *receiver, bool complete)
{
	if (receiver == NULL ||
	    !qam_equalizer_is_valid(&receiver->equalizer) ||
	    receiver->equalizer.can_adapt ||
	    !adaptation_is_valid(receiver->adaptation) ||
	    !v90_ja_search_is_valid(&receiver->search) ||
	    v90_ja_search_is_complete(&receiver->search) != complete)
		return false;
	if (receiver->decisions <= V34_TRAINING_SEQUENCE_MIN_SYMBOLS)
		return search_is_initial(&receiver->search);
	return receiver->search.has_previous_rotation;
}

bool v90_phase3_analog_rx_is_valid(
	const struct v90_phase3_analog_rx *rx)
{
	if (rx == NULL ||
	    !v90_phase3_analog_rx_configuration_is_valid(&rx->configuration))
		return false;
	switch (rx->stage) {
	case V90_PHASE3_ANALOG_RX_STAGE_S:
		return detection_is_clear(&rx->s_detection) &&
		       s_receiver_is_valid(
			       &rx->state.s,
			       rx->configuration.s_gain_adaptation);
	case V90_PHASE3_ANALOG_RX_STAGE_PP:
		return v34_phase3_s_detection_is_valid(&rx->s_detection) &&
		       v34_phase3_pp_receiver_is_valid(&rx->state.pp) &&
		       !v34_phase3_pp_receiver_is_complete(&rx->state.pp) &&
		       rx->state.pp.adaptation ==
			       rx->configuration.pp_equalizer_adaptation;
	case V90_PHASE3_ANALOG_RX_STAGE_JA:
		return v34_phase3_s_detection_is_valid(&rx->s_detection) &&
		       ja_receiver_is_valid(&rx->state.ja, false) &&
		       rx->state.ja.adaptation ==
			       rx->configuration.tracking_equalizer_adaptation;
	case V90_PHASE3_ANALOG_RX_STAGE_COMPLETE:
		return v34_phase3_s_detection_is_valid(&rx->s_detection) &&
		       ja_receiver_is_valid(&rx->state.ja, true) &&
		       rx->state.ja.adaptation ==
			       rx->configuration.tracking_equalizer_adaptation;
	}
	return false;
}

bool v90_phase3_analog_rx_is_complete(
	const struct v90_phase3_analog_rx *rx)
{
	return v90_phase3_analog_rx_is_valid(rx) &&
	       rx->stage == V90_PHASE3_ANALOG_RX_STAGE_COMPLETE;
}

enum v90_phase3_analog_rx_result v90_phase3_analog_rx_init(
	struct v90_phase3_analog_rx *rx,
	const struct v90_phase3_analog_rx_configuration *configuration)
{
	return v90_phase3_analog_rx_init_suffix(
		rx, configuration, V34_PHASE3_S_SYMBOLS);
}

enum v90_phase3_analog_rx_result v90_phase3_analog_rx_init_suffix(
	struct v90_phase3_analog_rx *rx,
	const struct v90_phase3_analog_rx_configuration *configuration,
	uint8_t s_symbols)
{
	struct v90_phase3_analog_rx initialized = {
		.stage = V90_PHASE3_ANALOG_RX_STAGE_S,
	};

	if (rx == NULL ||
	    !v90_phase3_analog_rx_configuration_is_valid(configuration))
		return V90_PHASE3_ANALOG_RX_INVALID_ARGUMENT;
	initialized.configuration = *configuration;
	if (v34_phase3_s_receiver_init_suffix(
		    &initialized.state.s.detector, s_symbols) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    !v90_phase3_analog_rx_is_valid(&initialized))
		return V90_PHASE3_ANALOG_RX_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_PHASE3_ANALOG_RX_OK;
}

static enum v90_phase3_analog_rx_result receive_s(
	struct v90_phase3_analog_rx *rx, const struct qam_point *received)
{
	struct v90_phase3_s_point_receiver *s = &rx->state.s;
	struct qam_point reference = v34_rotated_point_zero(0);
	struct v34_phase3_s_detection detection;
	struct v34_phase3_pp_receiver pp;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;
	enum v34_phase3_s_receiver_result result;

	if (!s->gain_seeded) {
		if (qam_gain_init_from_reference(
			    &s->gain, received, &reference,
			    rx->configuration.s_gain_adaptation) != QAM_GAIN_OK)
			return V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE;
		s->gain_seeded = true;
	}
	if (v34_training_channel_decide(
		    &s->gain, V34_CONSTELLATION_4_POINT, received,
		    &quarter_point, &clockwise_quarter_turns) !=
	    V34_TRAINING_CHANNEL_OK)
		return V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE;
	result = v34_phase3_s_receiver_receive(
		&s->detector, quarter_point, clockwise_quarter_turns);
	if (result == V34_PHASE3_S_RECEIVER_OK)
		return V90_PHASE3_ANALOG_RX_OK;
	if (result != V34_PHASE3_S_RECEIVER_DETECTED ||
	    !v34_phase3_s_receiver_get_detection(&s->detector, &detection) ||
	    v34_phase3_pp_receiver_init(
		    &pp, rx->configuration.pp_equalizer_adaptation) !=
		    V34_PHASE3_PP_RECEIVER_OK)
		return V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE;
	rx->state = (union v90_phase3_analog_rx_state){ .pp = pp };
	rx->s_detection = detection;
	rx->stage = V90_PHASE3_ANALOG_RX_STAGE_PP;
	return V90_PHASE3_ANALOG_RX_S_DETECTED;
}

static enum v90_phase3_analog_rx_result receive_pp(
	struct v90_phase3_analog_rx *rx, const struct qam_point *received)
{
	struct v34_equalizer_handoff handoff;
	struct v90_phase3_ja_point_receiver ja = { 0 };
	enum v34_phase3_pp_receiver_result result =
		v34_phase3_pp_receive_point(&rx->state.pp, received);

	if (result != V34_PHASE3_PP_RECEIVER_OK)
		return V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE;
	if (!v34_phase3_pp_receiver_is_complete(&rx->state.pp))
		return V90_PHASE3_ANALOG_RX_OK;
	if (v34_phase3_pp_receiver_handoff(&rx->state.pp, &handoff) !=
	    V34_PHASE3_PP_RECEIVER_OK)
		return V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE;
	ja.equalizer = handoff.equalizer;
	ja.adaptation = rx->configuration.tracking_equalizer_adaptation;
	if (v90_ja_search_init(&ja.search) != V90_JA_SEARCH_OK)
		return V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE;
	rx->state = (union v90_phase3_analog_rx_state){ .ja = ja };
	rx->stage = V90_PHASE3_ANALOG_RX_STAGE_JA;
	return V90_PHASE3_ANALOG_RX_PP_COMPLETE;
}

static enum v90_phase3_analog_rx_result search_result(
	enum v90_ja_search_result result)
{
	switch (result) {
	case V90_JA_SEARCH_OK:
		return V90_PHASE3_ANALOG_RX_OK;
	case V90_JA_SEARCH_DETECTED:
		return V90_PHASE3_ANALOG_RX_JA_DETECTED;
	case V90_JA_SEARCH_REJECTED:
		return V90_PHASE3_ANALOG_RX_JA_REJECTED;
	case V90_JA_SEARCH_COMPLETE:
	case V90_JA_SEARCH_INVALID_ARGUMENT:
		break;
	}
	return V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE;
}

static enum v90_phase3_analog_rx_result receive_ja(
	struct v90_phase3_analog_rx *rx, const struct qam_point *received)
{
	struct qam_equalizer_observation observation = { 0 };
	struct qam_point target;
	uint8_t quarter_point;
	uint8_t rotation;
	bool ready;

	if (rx->state.ja.decisions == UINT32_MAX)
		return V90_PHASE3_ANALOG_RX_OUT_OF_RANGE;
	if (qam_equalizer_observe(
		    &rx->state.ja.equalizer, received, &observation, &ready) !=
		    QAM_EQUALIZER_OK ||
	    !ready ||
	    v34_training_constellation_slice(
		    V34_CONSTELLATION_4_POINT, &observation.output,
		    &quarter_point, &rotation) != V34_CONSTELLATION_OK ||
	    v34_training_constellation_point(
		    V34_CONSTELLATION_4_POINT, quarter_point, rotation,
		    &target) != V34_CONSTELLATION_OK ||
	    qam_equalizer_adapt_observation(
		    &rx->state.ja.equalizer, &observation, &target,
		    rx->state.ja.adaptation) != QAM_EQUALIZER_OK)
		return V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE;
	rx->state.ja.decisions++;
	if (rx->state.ja.decisions <= V34_TRAINING_SEQUENCE_MIN_SYMBOLS)
		return V90_PHASE3_ANALOG_RX_OK;
	return search_result(v90_ja_search_receive(
		&rx->state.ja.search, rotation));
}

static bool result_commits(enum v90_phase3_analog_rx_result result)
{
	return result == V90_PHASE3_ANALOG_RX_OK ||
	       result == V90_PHASE3_ANALOG_RX_S_DETECTED ||
	       result == V90_PHASE3_ANALOG_RX_PP_COMPLETE ||
	       result == V90_PHASE3_ANALOG_RX_JA_REJECTED ||
	       result == V90_PHASE3_ANALOG_RX_JA_DETECTED;
}

enum v90_phase3_analog_rx_result v90_phase3_analog_rx_point(
	struct v90_phase3_analog_rx *rx,
	const struct qam_point *received)
{
	struct v90_phase3_analog_rx next;
	enum v90_phase3_analog_rx_result result;

	if (!v90_phase3_analog_rx_is_valid(rx) || received == NULL ||
	    !isfinite(received->in_phase) ||
	    !isfinite(received->quadrature))
		return V90_PHASE3_ANALOG_RX_INVALID_ARGUMENT;
	if (rx->stage == V90_PHASE3_ANALOG_RX_STAGE_COMPLETE)
		return V90_PHASE3_ANALOG_RX_COMPLETE;
	next = *rx;
	switch (next.stage) {
	case V90_PHASE3_ANALOG_RX_STAGE_S:
		result = receive_s(&next, received);
		break;
	case V90_PHASE3_ANALOG_RX_STAGE_PP:
		result = receive_pp(&next, received);
		break;
	case V90_PHASE3_ANALOG_RX_STAGE_JA:
		result = receive_ja(&next, received);
		break;
	case V90_PHASE3_ANALOG_RX_STAGE_COMPLETE:
		return V90_PHASE3_ANALOG_RX_COMPLETE;
	default:
		return V90_PHASE3_ANALOG_RX_INVALID_STATE;
	}
	if (!result_commits(result))
		return result;
	if (result == V90_PHASE3_ANALOG_RX_JA_DETECTED)
		next.stage = V90_PHASE3_ANALOG_RX_STAGE_COMPLETE;
	if (!v90_phase3_analog_rx_is_valid(&next))
		return V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE;
	*rx = next;
	return result;
}

bool v90_phase3_analog_handoff_is_valid(
	const struct v90_phase3_analog_handoff *handoff)
{
	return handoff != NULL &&
	       v34_phase3_s_detection_is_valid(&handoff->s) &&
	       v34_equalizer_handoff_is_valid(&handoff->equalizer) &&
	       v90_dil_parameters_are_valid(&handoff->dil);
}

enum v90_phase3_analog_rx_result v90_phase3_analog_rx_handoff(
	const struct v90_phase3_analog_rx *rx,
	struct v90_phase3_analog_handoff *handoff)
{
	struct v90_phase3_analog_handoff generated;

	if (!v90_phase3_analog_rx_is_valid(rx) || handoff == NULL)
		return V90_PHASE3_ANALOG_RX_INVALID_ARGUMENT;
	if (!v90_phase3_analog_rx_is_complete(rx))
		return V90_PHASE3_ANALOG_RX_INVALID_STATE;
	generated.s = rx->s_detection;
	generated.equalizer.equalizer = rx->state.ja.equalizer;
	if (!v90_ja_search_parameters(
		    &rx->state.ja.search, &generated.dil) ||
	    !v90_phase3_analog_handoff_is_valid(&generated))
		return V90_PHASE3_ANALOG_RX_INVALID_STATE;
	*handoff = generated;
	return V90_PHASE3_ANALOG_RX_OK;
}

const char *v90_phase3_analog_rx_result_name(
	enum v90_phase3_analog_rx_result result)
{
	switch (result) {
	case V90_PHASE3_ANALOG_RX_OK:
		return "ok";
	case V90_PHASE3_ANALOG_RX_S_DETECTED:
		return "S detected";
	case V90_PHASE3_ANALOG_RX_PP_COMPLETE:
		return "PP complete";
	case V90_PHASE3_ANALOG_RX_JA_REJECTED:
		return "Ja rejected";
	case V90_PHASE3_ANALOG_RX_JA_DETECTED:
		return "Ja detected";
	case V90_PHASE3_ANALOG_RX_COMPLETE:
		return "complete";
	case V90_PHASE3_ANALOG_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_ANALOG_RX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_ANALOG_RX_OUT_OF_RANGE:
		return "out of range";
	case V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
