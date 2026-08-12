// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_line_fixture.h"

#include <g711.h>
#include <v90_phase3_analog_line_rx.h>
#include <v90_phase3_analog_line_tx.h>
#include <v90_phase3_return_line_tx.h>

enum {
	TRN_SYMBOLS = V34_PHASE3_TRN_MIN_SYMBOLS + 173,
	RETURN_SILENCE_SYMBOLS = 350,
	RETURN_S_FINISH_SYMBOL = V34_PHASE3_S_SYMBOLS + 17,
	FIXTURE_SAMPLE_LIMIT = 50000,
	NOMINAL_AMPLITUDE = 6000,
};

static struct v34_phase3_profile profile(void)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_ANSWERING_STATION,
		.preemphasis_filter_index = 0,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
}

static struct v90_phase3_analog_rx_configuration configuration(void)
{
	return (struct v90_phase3_analog_rx_configuration){
		.s_gain_adaptation = 0.15,
		.pp_equalizer_adaptation = 0.2,
		.tracking_equalizer_adaptation = 0.05,
	};
}

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static bool establish_ja(
	enum g711_law law, struct v90_phase3_analog_line_tx *transmit,
	struct v90_phase3_analog_line_handoff *handoff)
{
	struct v34_phase3_profile selected = profile();
	struct v90_phase3_analog_rx_configuration config = configuration();
	struct v90_phase3_analog_line_rx receive;
	bool started_ja = false;

	if (v90_phase3_analog_line_tx_init(
		    transmit, &selected, NOMINAL_AMPLITUDE) !=
		    V90_PHASE3_ANALOG_LINE_TX_OK ||
	    v90_phase3_analog_line_rx_init(
		    &receive, &selected, &config) !=
		    V90_PHASE3_ANALOG_LINE_RX_OK)
		return false;
	for (uint32_t index = 0;
	     index < FIXTURE_SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v90_phase3_analog_line_rx_result result;

		if (!started_ja &&
		    transmit->symbols.stage == V90_PHASE3_ANALOG_TX_PREFIX &&
		    transmit->symbols.sequence.prefix.stage ==
			    V34_PHASE3_TRANSMITTER_TRN &&
		    transmit->symbols.sequence.prefix.trn_symbols ==
			    TRN_SYMBOLS) {
			if (v90_phase3_analog_line_tx_start_ja(transmit) !=
			    V90_PHASE3_ANALOG_LINE_TX_OK)
				return false;
			started_ja = true;
		}
		if (v90_phase3_analog_line_tx_sample(transmit, &sample) !=
		    V90_PHASE3_ANALOG_LINE_TX_OK)
			return false;
		result = v90_phase3_analog_line_rx_sample(
			&receive, quantize(law, sample));
		if (result == V90_PHASE3_ANALOG_LINE_RX_JA_DETECTED)
			return started_ja &&
			       v90_phase3_analog_line_rx_handoff(
				       &receive, handoff) ==
				       V90_PHASE3_ANALOG_LINE_RX_OK;
		if (result != V90_PHASE3_ANALOG_LINE_RX_OK &&
		    result != V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED &&
		    result != V90_PHASE3_ANALOG_LINE_RX_S_DETECTED &&
		    result != V90_PHASE3_ANALOG_LINE_RX_PP_COMPLETE &&
		    result != V90_PHASE3_ANALOG_LINE_RX_JA_REJECTED)
			return false;
	}
	return false;
}

static bool finish_ja(
	enum g711_law law, struct v90_phase3_analog_line_tx *transmit,
	struct v90_phase3_return_line_rx *receive,
	struct v34_training_line *line)
{
	uint32_t finish_at = transmit->symbols.ja_symbols +
		V90_PHASE3_JA_DESCRIPTOR_SYMBOLS;
	bool finish_requested = false;

	for (uint32_t index = 0;
	     index < FIXTURE_SAMPLE_LIMIT; ++index) {
		int16_t sample;

		if (!finish_requested &&
		    transmit->symbols.ja_symbols >= finish_at) {
			if (v90_phase3_analog_line_tx_finish_ja(transmit) !=
			    V90_PHASE3_ANALOG_LINE_TX_OK)
				return false;
			finish_requested = true;
		}
		if (v90_phase3_analog_line_tx_is_handoff_ready(transmit))
			return finish_requested &&
			       v90_phase3_analog_line_tx_handoff(
				       transmit, line) ==
				       V90_PHASE3_ANALOG_LINE_TX_OK;
		if (v90_phase3_analog_line_tx_sample(transmit, &sample) !=
			    V90_PHASE3_ANALOG_LINE_TX_OK ||
		    v90_phase3_return_line_rx_sample(
			    receive, quantize(law, sample)) !=
			    V90_PHASE3_RETURN_LINE_RX_OK)
			return false;
	}
	return false;
}

static bool finish_return_s(
	enum g711_law law, struct v90_phase3_return_line_tx *transmit,
	struct v90_phase3_return_line_rx *receive,
	struct v34_training_line *line)
{
	bool started_s = false;
	bool finished_s = false;
	unsigned int s_events = 0;

	for (uint32_t index = 0;
	     index < FIXTURE_SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v90_phase3_return_line_rx_result result;

		if (!started_s &&
		    transmit->points.silence_symbols ==
			    RETURN_SILENCE_SYMBOLS) {
			if (v90_phase3_return_line_tx_start_s(transmit) !=
			    V90_PHASE3_RETURN_LINE_TX_OK)
				return false;
			started_s = true;
		}
		if (started_s && !finished_s &&
		    transmit->points.s_symbols == RETURN_S_FINISH_SYMBOL) {
			if (v90_phase3_return_line_tx_finish_s(transmit) !=
			    V90_PHASE3_RETURN_LINE_TX_OK)
				return false;
			finished_s = true;
		}
		if (v90_phase3_return_line_tx_is_handoff_ready(transmit))
			return started_s && finished_s && s_events == 1 &&
			       v90_phase3_return_line_tx_handoff(
				       transmit, line) ==
				       V90_PHASE3_RETURN_LINE_TX_OK;
		if (v90_phase3_return_line_tx_sample(transmit, &sample) !=
		    V90_PHASE3_RETURN_LINE_TX_OK)
			return false;
		result = v90_phase3_return_line_rx_sample(
			receive, quantize(law, sample));
		if (result == V90_PHASE3_RETURN_LINE_RX_S_DETECTED)
			s_events++;
		else if (result != V90_PHASE3_RETURN_LINE_RX_OK)
			return false;
	}
	return false;
}

bool v90_phase4_line_fixture_init(
	enum g711_law law, enum v34_constellation_size constellation,
	const struct v90_cp_parameters *training,
	struct v90_phase4_cp_line_tx *transmit,
	struct v90_phase3_return_line_handoff *receive)
{
	struct v90_phase3_analog_line_tx prefix_transmit;
	struct v90_phase3_analog_line_handoff prefix_handoff;
	struct v90_phase3_return_line_tx return_transmit;
	struct v90_phase3_return_line_rx return_receive;
	struct v34_training_line line;

	if (transmit == NULL || receive == NULL ||
	    !establish_ja(law, &prefix_transmit, &prefix_handoff) ||
	    v90_phase3_return_line_rx_init(
		    &return_receive, &prefix_handoff, 0.05) !=
		    V90_PHASE3_RETURN_LINE_RX_OK ||
	    !finish_ja(law, &prefix_transmit, &return_receive, &line) ||
	    v90_phase3_return_line_tx_init(&return_transmit, &line) !=
		    V90_PHASE3_RETURN_LINE_TX_OK ||
	    !finish_return_s(
		    law, &return_transmit, &return_receive, &line) ||
	    v90_phase4_cp_line_tx_init(
		    transmit, &line, constellation, training) !=
		    V90_PHASE4_CP_LINE_TX_OK)
		return false;
	for (uint32_t index = 0;
	     index < FIXTURE_SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v90_phase3_return_line_rx_result result;

		if (v90_phase4_cp_line_tx_sample(transmit, &sample) !=
		    V90_PHASE4_CP_LINE_TX_OK)
			return false;
		result = v90_phase3_return_line_rx_sample(
			&return_receive, quantize(law, sample));
		if (result == V90_PHASE3_RETURN_LINE_RX_S_BAR_DETECTED)
			return transmit->symbols.bit_index > 0 &&
			       transmit->symbols.bit_index <
				       transmit->symbols.frame_bit_count &&
			       v90_phase3_return_line_rx_handoff(
				       &return_receive, receive) ==
				       V90_PHASE3_RETURN_LINE_RX_OK;
		if (result != V90_PHASE3_RETURN_LINE_RX_OK)
			return false;
	}
	return false;
}
