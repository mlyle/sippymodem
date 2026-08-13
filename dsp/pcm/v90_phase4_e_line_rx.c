// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_e_line_rx.h"

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

static bool channel_matches(const struct v90_phase4_e_line_rx *rx)
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

static bool cp_is_final(const struct v90_cp_parameters *cp)
{
	return v90_cp_parameters_are_valid(cp) &&
	       cp->kind == V90_CP_DATA && cp->selected_rate_index != 0 &&
	       cp->acknowledges_peer && !cp->request_silence;
}

bool v90_phase4_e_line_rx_is_valid(
	const struct v90_phase4_e_line_rx *rx)
{
	return rx != NULL && profile_is_analog(&rx->transmitting_profile) &&
	       qam_demodulator_is_valid(&rx->demodulator) &&
	       qam_demodulator_measure(&rx->demodulator).timing_locked &&
	       channel_matches(rx) && equalizer_is_at_handoff(&rx->equalizer) &&
	       v34_phase4_e_receiver_is_valid(&rx->symbols) &&
	       rx->symbols.transmitting_role ==
		       rx->transmitting_profile.local_role &&
	       cp_is_final(&rx->cp) && isfinite(rx->adaptation) &&
	       rx->adaptation >= 0.0 && rx->adaptation <= 1.0;
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
	initialized.transmitting_profile =
		completed_cp->transmitting_profile;
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
	enum v90_phase4_e_line_rx_result result)
{
	/* A failed decision no longer has a useful adaptation target. */
	next->equalizer.can_adapt = false;
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
	struct qam_point received;
	struct qam_point target;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;
	bool carrier_ready;
	bool equalizer_ready;
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
		    next.adaptation) != QAM_EQUALIZER_OK)
		return fail_sample(
			rx, &next, V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE);
	symbol_result = v34_phase4_e_receive_decision(
		&next.symbols, quarter_point, clockwise_quarter_turns);
	if (symbol_result == V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH)
		return fail_sample(
			rx, &next,
			V90_PHASE4_E_LINE_RX_SEQUENCE_MISMATCH);
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

enum v90_phase4_e_line_rx_result v90_phase4_e_line_rx_start_data(
	const struct v90_phase4_e_line_rx *rx,
	struct v34_data_line_receiver *data_rx,
	const struct v34_data_line_receiver_configuration *configuration)
{
	struct v34_equalizer_handoff handoff;

	if (!v90_phase4_e_line_rx_is_valid(rx) || data_rx == NULL ||
	    configuration == NULL)
		return V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT;
	if (rx->failed)
		return V90_PHASE4_E_LINE_RX_FAILED;
	if (!v90_phase4_e_line_rx_is_complete(rx))
		return V90_PHASE4_E_LINE_RX_INVALID_STATE;
	handoff.equalizer = rx->equalizer;
	if (!v34_equalizer_handoff_is_valid(&handoff) ||
	    v34_data_line_receiver_init(
		    data_rx, &rx->demodulator, &handoff,
		    configuration) != V34_DATA_LINE_RECEIVER_OK)
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
	case V90_PHASE4_E_LINE_RX_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	}
	return "unknown";
}
