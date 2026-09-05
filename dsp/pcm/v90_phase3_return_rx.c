// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_return_rx.h"

#include "v34_constellation.h"

#include <stddef.h>

static bool compact_equalizer_state_is_valid(
	const struct v90_phase3_return_rx *rx)
{
	if (!rx->compact_equalizer_active)
		return rx->compact_equalizer.tap_count == 0;
	return rx->semantic.second_return &&
	       rx->semantic.stage != V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END &&
	       rx->equalizer.tap_count == QAM_EQUALIZER_MAX_TAP_COUNT &&
	       rx->compact_equalizer.tap_count == V34_EQUALIZER_TAP_COUNT &&
	       rx->compact_equalizer.decision_delay ==
		       V34_EQUALIZER_DECISION_DELAY &&
	       rx->compact_equalizer.fill == rx->equalizer.fill &&
	       rx->compact_equalizer.carrier_tracking ==
		       rx->equalizer.carrier_tracking &&
	       qam_equalizer_is_valid(&rx->compact_equalizer) &&
	       !rx->compact_equalizer.can_adapt;
}

bool v90_phase3_return_rx_is_valid(
	const struct v90_phase3_return_rx *rx)
{
	if (rx == NULL || !qam_adaptation_is_valid(rx->adaptation) ||
	    !v34_precoder_recommendation_is_valid(&rx->precoder) ||
	    (rx->equalizer_reacquiring ?
		     !qam_equalizer_is_valid(&rx->equalizer) :
		     !qam_equalizer_is_handoff_ready(&rx->equalizer)) ||
	    !v90_phase3_return_marker_is_valid(&rx->semantic) ||
	    rx->semantic.confirm_onset != rx->equalizer.carrier_tracking ||
	    !compact_equalizer_state_is_valid(rx))
		return false;
	return !rx->equalizer_reacquiring ||
	       (rx->semantic.second_return && rx->equalizer.carrier_tracking &&
		rx->semantic.stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S &&
		rx->semantic.confirmed_s_symbols == 0 &&
		rx->semantic.buffered_onset_decisions == 0);
}

bool v90_phase3_return_rx_is_complete(
	const struct v90_phase3_return_rx *rx)
{
	return v90_phase3_return_rx_is_valid(rx) &&
	       rx->semantic.stage == V90_PHASE3_RETURN_RX_STAGE_COMPLETE;
}

enum v90_phase3_return_rx_result v90_phase3_return_rx_init(
	struct v90_phase3_return_rx *rx,
	const struct v34_equalizer_handoff *equalizer,
	double adaptation)
{
	const struct v34_precoder_recommendation no_precoder = { 0 };

	return v90_phase3_return_rx_init_with_precoder(
		rx, equalizer, &no_precoder, adaptation);
}

enum v90_phase3_return_rx_result v90_phase3_return_rx_init_with_precoder(
	struct v90_phase3_return_rx *rx,
	const struct v34_equalizer_handoff *equalizer,
	const struct v34_precoder_recommendation *precoder,
	double adaptation)
{
	struct v90_phase3_return_rx initialized = {
		.adaptation = adaptation,
	};

	if (rx == NULL || !v34_equalizer_handoff_is_valid(equalizer) ||
	    !v34_precoder_recommendation_is_valid(precoder) ||
	    !qam_adaptation_is_valid(adaptation))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	initialized.equalizer = equalizer->equalizer;
	initialized.precoder = *precoder;
	if (v90_phase3_return_marker_init(
		    &initialized.semantic,
		    initialized.equalizer.carrier_tracking) !=
		    V90_PHASE3_RETURN_RX_OK ||
	    !v90_phase3_return_rx_is_valid(&initialized))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_PHASE3_RETURN_RX_OK;
}

enum v90_phase3_return_rx_result
v90_phase3_return_rx_allow_implicit_s_end(
	struct v90_phase3_return_rx *rx)
{
	if (!v90_phase3_return_rx_is_valid(rx))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	return v90_phase3_return_marker_allow_implicit_s_end(&rx->semantic);
}

enum v90_phase3_return_rx_result v90_phase3_return_rx_restart(
	struct v90_phase3_return_rx *rx)
{
	struct v90_phase3_return_rx restarted;
	enum v90_phase3_return_rx_result result;

	if (!v90_phase3_return_rx_is_valid(rx))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	restarted = *rx;
	result = v90_phase3_return_marker_restart(&restarted.semantic);
	if (result != V90_PHASE3_RETURN_RX_OK)
		return result;
	restarted.compact_equalizer = (struct qam_equalizer){ 0 };
	restarted.compact_equalizer_active = false;
	restarted.equalizer_reacquiring = false;
	if (!v90_phase3_return_rx_is_valid(&restarted))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	*rx = restarted;
	return V90_PHASE3_RETURN_RX_OK;
}

enum v90_phase3_return_rx_result v90_phase3_return_rx_reacquire_s(
	struct v90_phase3_return_rx *rx,
	const struct qam_point *received)
{
	struct v90_phase3_return_rx next;
	struct qam_point target = v34_rotated_point_zero(0);
	double proportional_gain;
	double integral_gain;

	if (!v90_phase3_return_rx_is_valid(rx) ||
	    !point_is_finite(received))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (!rx->semantic.second_return ||
	    rx->semantic.stage != V90_PHASE3_RETURN_RX_STAGE_SEARCH_S ||
	    !rx->equalizer.carrier_tracking || rx->equalizer_reacquiring)
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	next = *rx;
	proportional_gain = next.equalizer.carrier_proportional_gain;
	integral_gain = next.equalizer.carrier_integral_gain;
	if (qam_equalizer_init_geometry(
		    &next.equalizer, received, &target,
		    QAM_EQUALIZER_MAX_TAP_COUNT,
		    V34_EQUALIZER_DECISION_DELAY) != QAM_EQUALIZER_OK ||
	    qam_equalizer_enable_carrier_tracking(
		    &next.equalizer, proportional_gain,
		    integral_gain) != QAM_EQUALIZER_OK ||
	    qam_equalizer_init_geometry(
		    &next.compact_equalizer, received, &target,
		    V34_EQUALIZER_TAP_COUNT,
		    V34_EQUALIZER_DECISION_DELAY) != QAM_EQUALIZER_OK ||
	    qam_equalizer_enable_carrier_tracking(
		    &next.compact_equalizer, proportional_gain,
		    integral_gain) != QAM_EQUALIZER_OK ||
	    v90_phase3_return_marker_reacquire_s(&next.semantic) !=
		    V90_PHASE3_RETURN_RX_OK)
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	next.equalizer_reacquiring = true;
	next.compact_equalizer_active = true;
	if (!v90_phase3_return_rx_is_valid(&next))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	*rx = next;
	return V90_PHASE3_RETURN_RX_OK;
}

static bool adapt_equalizer_decision(
	struct qam_equalizer *equalizer,
	struct qam_equalizer_observation *observation,
	uint8_t rotation, double adaptation)
{
	struct qam_point target;

	return v34_training_constellation_point(
		       V34_CONSTELLATION_4_POINT, 0, rotation, &target) ==
		       V34_CONSTELLATION_OK &&
	       qam_equalizer_adapt_observation(
		       equalizer, observation, &target,
		       adaptation) == QAM_EQUALIZER_OK;
}

static bool adapt_decision(
	struct v90_phase3_return_rx *rx,
	struct qam_equalizer_observation *observation,
	uint8_t rotation)
{
	return adapt_equalizer_decision(
		&rx->equalizer, observation, rotation, rx->adaptation);
}

static bool result_commits(enum v90_phase3_return_rx_result result)
{
	return result == V90_PHASE3_RETURN_RX_OK ||
	       result == V90_PHASE3_RETURN_RX_S_DETECTED ||
	       result == V90_PHASE3_RETURN_RX_S_ENDED ||
	       result == V90_PHASE3_RETURN_RX_S_BAR_DETECTED;
}

enum v90_phase3_return_rx_result v90_phase3_return_rx_point(
	struct v90_phase3_return_rx *rx,
	const struct qam_point *received)
{
	struct v90_phase3_return_rx next;
	struct qam_equalizer_observation observation = { 0 };
	struct qam_equalizer_observation compact_observation = { 0 };
	uint8_t quarter_point;
	uint8_t rotation;
	bool ready;
	bool compact_ready = false;
	bool adapt;
	enum v90_phase3_return_rx_result result;

	if (!v90_phase3_return_rx_is_valid(rx) ||
	    !point_is_finite(received))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (rx->semantic.stage == V90_PHASE3_RETURN_RX_STAGE_COMPLETE)
		return V90_PHASE3_RETURN_RX_COMPLETE;
	if (rx->semantic.stage == V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END)
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	next = *rx;
	if (qam_equalizer_observe(
		    &next.equalizer, received, &observation, &ready) !=
		    QAM_EQUALIZER_OK ||
	    (next.compact_equalizer_active &&
	     (qam_equalizer_observe(
		      &next.compact_equalizer, received,
		      &compact_observation, &compact_ready) !=
		      QAM_EQUALIZER_OK ||
	      compact_ready != ready)))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	if (!ready) {
		if (!next.equalizer_reacquiring ||
		    !v90_phase3_return_rx_is_valid(&next))
			return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
		*rx = next;
		return V90_PHASE3_RETURN_RX_OK;
	}
	next.equalizer_reacquiring = false;
	if (v34_training_constellation_slice(
		    V34_CONSTELLATION_4_POINT, &observation.output,
		    &quarter_point, &rotation) != V34_CONSTELLATION_OK ||
	    quarter_point != 0)
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	result = v90_phase3_return_marker_observe(&next.semantic, rotation, &adapt);
	if (!result_commits(result))
		return result;
	/* The marker and both equalizers commit together, including failures
	 * after a decision has qualified the semantic S/S-bar boundary. */
	if (adapt && !adapt_decision(&next, &observation, rotation))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	if (next.compact_equalizer_active &&
	    !qam_equalizer_observation_is_pending(&observation) &&
	    !adapt_equalizer_decision(
		    &next.compact_equalizer, &compact_observation,
		    rotation, next.adaptation))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	if (!v90_phase3_return_rx_is_valid(&next))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	*rx = next;
	return result;
}

bool v90_phase3_return_handoff_is_valid(
	const struct v90_phase3_return_handoff *handoff)
{
	if (handoff == NULL ||
	    !v34_phase3_s_detection_is_valid(&handoff->marker) ||
	    !v34_equalizer_handoff_is_valid(&handoff->equalizer) ||
	    !v34_precoder_recommendation_is_valid(&handoff->precoder))
		return false;
	if (!handoff->compact_equalizer_active)
		return handoff->compact_equalizer.tap_count == 0;
	return handoff->equalizer.equalizer.tap_count ==
		       QAM_EQUALIZER_MAX_TAP_COUNT &&
	       handoff->compact_equalizer.tap_count ==
		       V34_EQUALIZER_TAP_COUNT &&
	       handoff->compact_equalizer.decision_delay ==
		       V34_EQUALIZER_DECISION_DELAY &&
	       handoff->compact_equalizer.fill ==
		       handoff->equalizer.equalizer.fill &&
	       handoff->compact_equalizer.carrier_tracking ==
		       handoff->equalizer.equalizer.carrier_tracking &&
	       qam_equalizer_is_handoff_ready(
		       &handoff->compact_equalizer);
}

enum v90_phase3_return_rx_result v90_phase3_return_rx_handoff(
	const struct v90_phase3_return_rx *rx,
	struct v90_phase3_return_handoff *handoff)
{
	struct v90_phase3_return_handoff generated = { 0 };

	if (!v90_phase3_return_rx_is_valid(rx) || handoff == NULL)
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (!v90_phase3_return_rx_is_complete(rx))
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	generated.equalizer.equalizer = rx->equalizer;
	generated.compact_equalizer = rx->compact_equalizer;
	generated.compact_equalizer_active =
		rx->compact_equalizer_active;
	generated.precoder = rx->precoder;
	if (!v90_phase3_return_marker_get_detection(
		    &rx->semantic, &generated.marker) ||
	    !v90_phase3_return_handoff_is_valid(&generated))
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	*handoff = generated;
	return V90_PHASE3_RETURN_RX_OK;
}

const char *v90_phase3_return_rx_result_name(
	enum v90_phase3_return_rx_result result)
{
	switch (result) {
	case V90_PHASE3_RETURN_RX_OK:
		return "ok";
	case V90_PHASE3_RETURN_RX_S_DETECTED:
		return "S detected";
	case V90_PHASE3_RETURN_RX_S_ENDED:
		return "S ended without S-bar";
	case V90_PHASE3_RETURN_RX_S_BAR_DETECTED:
		return "S-bar detected";
	case V90_PHASE3_RETURN_RX_COMPLETE:
		return "complete";
	case V90_PHASE3_RETURN_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_RETURN_RX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_RETURN_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
