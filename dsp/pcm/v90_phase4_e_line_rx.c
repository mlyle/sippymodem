// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_e_line_rx.h"

#include "memory_region.h"

#include <stddef.h>

static bool cp_is_final(const struct v90_cp_parameters *cp)
{
	return v90_cp_parameters_are_valid(cp) &&
	       cp->kind == V90_CP_DATA && cp->selected_rate_index != 0 &&
	       cp->acknowledges_peer && !cp->request_silence;
}

bool v90_phase4_e_line_rx_is_valid(
	const struct v90_phase4_e_line_rx *rx)
{
	return rx != NULL &&
	       v34_phase3_profile_is_valid(&rx->transmitting_profile) &&
	       v34_line_receiver_matches_profile(
		       &rx->line, &rx->transmitting_profile) &&
	       v34_receive_timing_is_acquired(&rx->line.timing) &&
	       rx->line.equalizer.feedback_mode ==
		       V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
	       !rx->line.equalizer.decision_pending &&
	       v34_phase4_e_receiver_is_valid(&rx->symbols) &&
	       rx->symbols.transmitting_role ==
		       rx->transmitting_profile.local_role &&
	       cp_is_final(&rx->cp) && rx->decoder_power_q15 != 0u &&
	       rx->decoder_power_q15 <= INT16_MAX &&
	       (rx->adaptation == V34_ADAPTIVE_EQUALIZER_FULL ||
		rx->adaptation == V34_ADAPTIVE_EQUALIZER_CARRIER);
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
	    memory_regions_overlap(
		    rx, sizeof(*rx), completed_cp,
		    completed_cp == NULL ? 0 : sizeof(*completed_cp)) ||
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
	initialized.line = completed_cp->line;
	initialized.transmitting_profile =
		completed_cp->transmitting_profile;
	initialized.decoder_power_q15 = completed_cp->decoder_power_q15;
	initialized.adaptation = completed_cp->adaptation;
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
	const struct v34_adaptive_equalizer_observation *observation,
	enum v90_phase4_e_line_rx_result result)
{
	if (next->line.equalizer.decision_pending && observation != NULL &&
	    v34_adaptive_equalizer_discard(
		    &next->line.equalizer, observation) !=
		    V34_ADAPTIVE_EQUALIZER_OK) {
		rx->failed = true;
		return V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE;
	}
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
	struct v34_adaptive_equalizer_observation observation;
	struct v34_fixed_training_decision hard;
	struct v34_receive_action action;
	enum v34_line_receiver_result line_result;
	enum v34_phase4_e_receiver_result symbol_result;

	if (!v90_phase4_e_line_rx_is_valid(rx))
		return V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE4_E_LINE_RX_FAILED;
	if (v90_phase4_e_line_rx_is_complete(rx))
		return V90_PHASE4_E_LINE_RX_COMPLETE;
	next = *rx;
	line_result = v34_line_receiver_sample(
		&next.line, sample, &observation);
	if (line_result == V34_LINE_RECEIVER_OK) {
		if (!v90_phase4_e_line_rx_is_valid(&next))
			return fail_sample(
				rx, &next, NULL,
				V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
		*rx = next;
		return V90_PHASE4_E_LINE_RX_OK;
	}
	if (line_result != V34_LINE_RECEIVER_OBSERVATION_READY)
		return fail_sample(
			rx, &next, NULL,
			V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
	if (v34_training_constellation_fixed_slice(
		    next.symbols.constellation, observation.decision,
		    &hard) != V34_CONSTELLATION_OK)
		return fail_sample(
			rx, &next, &observation,
			V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
	symbol_result = v34_phase4_e_receive_decision(
		&next.symbols, hard.quarter_point,
		hard.clockwise_quarter_turns);
	if (symbol_result != V34_PHASE4_E_RECEIVER_OK &&
	    symbol_result != V34_PHASE4_E_RECEIVER_COMPLETE)
		return fail_sample(
			rx, &next, &observation,
			V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
	action = v34_receive_action_commit(
		&observation, hard.target, next.decoder_power_q15,
		next.adaptation);
	if (v34_line_receiver_resolve(
		    &next.line, &observation, &action,
		    next.decoder_power_q15) != V34_LINE_RECEIVER_OK ||
	    !v90_phase4_e_line_rx_is_valid(&next))
		return fail_sample(
			rx, &next, &observation,
			V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
	*rx = next;
	return symbol_result == V34_PHASE4_E_RECEIVER_COMPLETE ?
		       V90_PHASE4_E_LINE_RX_E_COMPLETE :
		       V90_PHASE4_E_LINE_RX_SYMBOL;
}

enum v90_phase4_e_line_rx_result v90_phase4_e_line_rx_start_data(
	const struct v90_phase4_e_line_rx *rx,
	struct v34_data_line_receiver *data_rx,
	const struct v34_data_line_receiver_configuration *configuration)
{
	struct v34_line_receiver line;

	if (!v90_phase4_e_line_rx_is_valid(rx) || data_rx == NULL ||
	    configuration == NULL ||
	    memory_regions_overlap(rx, sizeof(*rx), data_rx, sizeof(*data_rx)) ||
	    memory_regions_overlap(
		    rx, sizeof(*rx), configuration, sizeof(*configuration)) ||
	    memory_regions_overlap(
		    data_rx, sizeof(*data_rx), configuration,
		    sizeof(*configuration)))
		return V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE4_E_LINE_RX_FAILED;
	if (!v90_phase4_e_line_rx_is_complete(rx))
		return V90_PHASE4_E_LINE_RX_INVALID_STATE;
	line = rx->line;
	if (v34_adaptive_equalizer_set_data_carrier_tracking(
		    &line.equalizer, true) != V34_ADAPTIVE_EQUALIZER_OK)
		return V90_PHASE4_E_LINE_RX_INVALID_STATE;
	if (v34_data_line_receiver_init(
		    data_rx, &line, configuration) !=
	    V34_DATA_RECEIVE_OK)
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
