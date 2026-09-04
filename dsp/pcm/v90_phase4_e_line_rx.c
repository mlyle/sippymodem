// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_e_line_rx.h"

#include "v90_phase3.h"

#include <stddef.h>

static const double V90_DATA_CARRIER_ERROR_LIMIT = 1.0 / 4.0;

static const double V90_PHASE4_CONTROL_ADAPTATION_SCALE = 0.8;
/* The long second-return estimate must explain at least this fraction of its
 * learned channel power beyond the conventional V.34 span. */
static const double V90_RETURN_EQUALIZER_TAIL_POWER_RATIO = 1.0 / 64.0;
/* B1 is a long, highly structured all-ones sequence.  Its matched-filter
 * energy can give an adjacent timing candidate a tiny repeatable advantage
 * even when the recovered 8 kHz clock is exact.  Require a full one-percent
 * improvement before moving the established Phase 4 timing reference into
 * B1 and online data. */
static const double V90_DATA_TIMING_POWER_HYSTERESIS = 1.01;

static bool cp_is_final(const struct v90_cp_parameters *cp)
{
	return v90_cp_parameters_are_valid(cp) &&
	       cp->kind == V90_CP_DATA && cp->selected_rate_index != 0 &&
	       cp->acknowledges_peer && !cp->request_silence;
}

static bool compact_equalizer_state_is_valid(
	const struct v90_phase4_e_line_rx *rx)
{
	if (!rx->compact_equalizer_active)
		return rx->compact_equalizer.tap_count == 0;
	return rx->equalizer.tap_count == QAM_EQUALIZER_MAX_TAP_COUNT &&
	       rx->compact_equalizer.tap_count == V34_EQUALIZER_TAP_COUNT &&
	       rx->compact_equalizer.decision_delay ==
		       V34_EQUALIZER_DECISION_DELAY &&
	       rx->compact_equalizer.fill == rx->equalizer.fill &&
	       rx->compact_equalizer.carrier_tracking ==
		       rx->equalizer.carrier_tracking &&
	       qam_equalizer_is_handoff_ready(&rx->compact_equalizer);
}

bool v90_phase4_e_line_rx_is_valid(
	const struct v90_phase4_e_line_rx *rx)
{
	return rx != NULL &&
	       v90_phase3_demodulator_is_locked(
		       &rx->demodulator, &rx->transmitting_profile) &&
	       qam_equalizer_is_handoff_ready(&rx->equalizer) &&
	       compact_equalizer_state_is_valid(rx) &&
	       v34_phase4_e_receiver_is_valid(&rx->symbols) &&
	       rx->symbols.transmitting_role ==
		       rx->transmitting_profile.local_role &&
	       cp_is_final(&rx->cp) &&
	       qam_adaptation_is_valid(rx->adaptation);
}

bool v90_phase4_e_line_rx_has_failed(
	const struct v90_phase4_e_line_rx *rx)
{
	return v90_phase4_e_line_rx_is_valid(rx) && rx->failed;
}

bool v90_phase4_e_line_rx_is_complete(
	const struct v90_phase4_e_line_rx *rx)
{
	return v90_phase4_e_line_rx_is_valid(rx) && !rx->failed &&
	       v34_phase4_e_receiver_is_complete(&rx->symbols);
}

enum v90_phase4_e_line_rx_result v90_phase4_e_line_rx_init(
	struct v90_phase4_e_line_rx *rx,
	const struct v90_phase4_cp_line_rx *completed_cp)
{
	struct v90_phase4_e_line_rx initialized = { 0 };
	enum v90_phase4_cp_line_rx_result cp_result;

	if (rx == NULL ||
	    !v90_phase4_cp_line_rx_is_valid(completed_cp))
		return V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT;
	if (v90_phase4_cp_line_rx_has_failed(completed_cp))
		return V90_PHASE4_E_LINE_RX_FAILED;
	cp_result = v90_phase4_cp_line_rx_pending_parameters(
		completed_cp, &initialized.cp);
	if (cp_result != V90_PHASE4_CP_LINE_RX_OK)
		return cp_result == V90_PHASE4_CP_LINE_RX_INVALID_STATE ?
			       V90_PHASE4_E_LINE_RX_INVALID_STATE :
			       V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT;
	if (!cp_is_final(&initialized.cp))
		return V90_PHASE4_E_LINE_RX_INVALID_STATE;
	initialized.demodulator = completed_cp->demodulator;
	initialized.equalizer = completed_cp->equalizer;
	initialized.compact_equalizer =
		completed_cp->compact_equalizer;
	initialized.compact_equalizer_active =
		completed_cp->compact_equalizer_active;
	initialized.transmitting_profile =
		completed_cp->transmitting_profile;
	initialized.adaptation = completed_cp->adaptation;
	initialized.tap_adaptation_held =
		completed_cp->tap_adaptation_held;
	initialized.symbols = (struct v34_phase4_e_receiver){
		.descrambler = completed_cp->symbols.descrambler,
		.transmitting_role =
			completed_cp->symbols.transmitting_role,
		.constellation = completed_cp->symbols.constellation,
		.previous_clockwise_quarter_turns =
			completed_cp->symbols
				.previous_clockwise_quarter_turns,
	};
	if (!v90_phase4_e_line_rx_is_valid(&initialized))
		return V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_PHASE4_E_LINE_RX_OK;
}

static enum v90_phase4_e_line_rx_result fail_sample(
	struct v90_phase4_e_line_rx *rx,
	struct v90_phase4_e_line_rx *next,
	enum v90_phase4_e_line_rx_result result)
{
	/* A failed decision no longer has a useful adaptation target. */
	next->equalizer.can_adapt = false;
	if (next->compact_equalizer_active)
		next->compact_equalizer.can_adapt = false;
	next->failed = true;
	if (v90_phase4_e_line_rx_is_valid(next))
		*rx = *next;
	else
		rx->failed = true;
	return result;
}

enum v90_phase4_e_line_rx_result v90_phase4_e_line_rx_sample(
	struct v90_phase4_e_line_rx *rx, int16_t sample)
{
	struct v90_phase4_e_line_rx next;
	struct qam_equalizer_observation observation = { 0 };
	struct qam_equalizer_observation compact_observation = { 0 };
	struct qam_point received;
	struct qam_point target;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;
	bool carrier_ready;
	bool equalizer_ready;
	bool compact_ready = false;
	enum v34_phase4_e_receiver_result symbol_result;

	if (!v90_phase4_e_line_rx_is_valid(rx))
		return V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE4_E_LINE_RX_FAILED;
	if (v90_phase4_e_line_rx_is_complete(rx))
		return V90_PHASE4_E_LINE_RX_COMPLETE;
	next = *rx;
	if (qam_demodulator_sample(
		    &next.demodulator, sample, &received,
		    &carrier_ready) != QAM_OK)
		return fail_sample(
			rx, &next, V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
	if (!carrier_ready) {
		if (!v90_phase4_e_line_rx_is_valid(&next))
			return fail_sample(
				rx, &next,
				V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
		*rx = next;
		return V90_PHASE4_E_LINE_RX_OK;
	}
	if (qam_equalizer_observe(
		    &next.equalizer, &received, &observation,
		    &equalizer_ready) != QAM_EQUALIZER_OK ||
	    !equalizer_ready ||
	    (next.compact_equalizer_active &&
	     (qam_equalizer_observe(
		      &next.compact_equalizer, &received,
		      &compact_observation, &compact_ready) !=
			      QAM_EQUALIZER_OK ||
	      compact_ready != equalizer_ready)) ||
	    v34_training_constellation_slice(
		    next.symbols.constellation, &observation.output,
		    &quarter_point, &clockwise_quarter_turns) !=
		    V34_CONSTELLATION_OK ||
	    v34_training_constellation_point(
		    next.symbols.constellation, quarter_point,
		    clockwise_quarter_turns, &target) !=
		    V34_CONSTELLATION_OK ||
	    qam_equalizer_adapt_observation(
		    &next.equalizer, &observation, &target,
		    next.tap_adaptation_held ? 0.0 :
			    next.adaptation *
				    V90_PHASE4_CONTROL_ADAPTATION_SCALE) !=
		    QAM_EQUALIZER_OK ||
	    (next.compact_equalizer_active &&
	     qam_equalizer_adapt_observation(
		     &next.compact_equalizer, &compact_observation,
		     &target,
		     next.tap_adaptation_held ? 0.0 :
			     next.adaptation *
				     V90_PHASE4_CONTROL_ADAPTATION_SCALE) !=
		     QAM_EQUALIZER_OK))
		return fail_sample(
			rx, &next, V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
	symbol_result = v34_phase4_e_receive_decision(
		&next.symbols, quarter_point, clockwise_quarter_turns);
	if (symbol_result != V34_PHASE4_E_RECEIVER_OK &&
	    symbol_result != V34_PHASE4_E_RECEIVER_COMPLETE)
		return fail_sample(
			rx, &next, V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
	if (!v90_phase4_e_line_rx_is_valid(&next))
		return fail_sample(
			rx, &next, V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
	*rx = next;
	return symbol_result == V34_PHASE4_E_RECEIVER_COMPLETE ?
		       V90_PHASE4_E_LINE_RX_E_COMPLETE :
		       V90_PHASE4_E_LINE_RX_SYMBOL;
}

static bool extended_equalizer_tail_is_material(
	const struct qam_equalizer *equalizer)
{
	double core_power = 0.0;
	double tail_power = 0.0;

	for (size_t index = 0; index < equalizer->tap_count; ++index) {
		double power =
			equalizer->coefficients[index].in_phase *
				equalizer->coefficients[index].in_phase +
			equalizer->coefficients[index].quadrature *
				equalizer->coefficients[index].quadrature;

		if (index < V34_EQUALIZER_TAP_COUNT)
			core_power += power;
		else
			tail_power += power;
	}
	return tail_power >=
	       core_power * V90_RETURN_EQUALIZER_TAIL_POWER_RATIO;
}

enum v90_phase4_e_line_rx_result v90_phase4_e_line_rx_start_data(
	const struct v90_phase4_e_line_rx *rx,
	struct v34_data_line_receiver *data_rx,
	const struct v34_data_line_receiver_configuration *configuration)
{
	struct v34_equalizer_handoff handoff;
	struct qam_demodulator demodulator;

	if (!v90_phase4_e_line_rx_is_valid(rx) || data_rx == NULL ||
	    configuration == NULL)
		return V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE4_E_LINE_RX_FAILED;
	if (!v90_phase4_e_line_rx_is_complete(rx))
		return V90_PHASE4_E_LINE_RX_INVALID_STATE;
	demodulator = rx->demodulator;
	handoff.equalizer =
		rx->compact_equalizer_active &&
		!extended_equalizer_tail_is_material(&rx->equalizer) ?
			rx->compact_equalizer : rx->equalizer;
	/* E ends the known-symbol carrier-training epoch. Keep its oscillator
	 * estimate for B1 and data, while the proportional loop continues to
	 * follow residual phase without integrating tentative trellis decisions. */
	if (qam_demodulator_set_timing_power_hysteresis(
		    &demodulator, V90_DATA_TIMING_POWER_HYSTERESIS) != QAM_OK ||
	    qam_equalizer_hold_carrier_frequency(&handoff.equalizer) !=
		    QAM_EQUALIZER_OK ||
	    qam_equalizer_set_carrier_error_limit(
		    &handoff.equalizer, V90_DATA_CARRIER_ERROR_LIMIT) !=
		    QAM_EQUALIZER_OK ||
	    !v34_equalizer_handoff_is_valid(&handoff) ||
	    v34_data_line_receiver_init(
		    data_rx, &demodulator, &handoff,
		    configuration) != V34_DATA_RECEIVE_OK)
		return V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT;
	return V90_PHASE4_E_LINE_RX_OK;
}

const char *v90_phase4_e_line_rx_result_name(
	enum v90_phase4_e_line_rx_result result)
{
	switch (result) {
	case V90_PHASE4_E_LINE_RX_OK:
		return "ok";
	case V90_PHASE4_E_LINE_RX_SYMBOL:
		return "symbol";
	case V90_PHASE4_E_LINE_RX_E_COMPLETE:
		return "E complete";
	case V90_PHASE4_E_LINE_RX_COMPLETE:
		return "complete";
	case V90_PHASE4_E_LINE_RX_FAILED:
		return "failed";
	case V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE4_E_LINE_RX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
