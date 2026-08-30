// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_station_fixture.h"

#include "v90_phase4_line_fixture.h"

#include <g711.h>

enum { FIXTURE_SAMPLE_LIMIT = 50000 };

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

bool v90_phase4_station_fixture_init(
	enum g711_law law, const struct v90_jd_parameters *jd,
	uint8_t training_ucode,
	const struct v90_cp_parameters *training,
	struct v90_phase4_analog_station *analog,
	struct v90_phase3_return_line_handoff *receive)
{
	struct v90_phase4_analog_station generated_analog;
	struct v90_phase3_return_line_handoff generated_receive;
	struct v90_phase3_return_line_rx return_receive;
	struct v34_training_line line;

	if (analog == NULL || receive == NULL ||
	    !v90_phase4_fixture_prepare_boundary(
		    law, &line, &return_receive) ||
	    v90_phase4_analog_station_init(
		    &generated_analog, &line, jd, law, training_ucode,
		    0u, training) != V90_PHASE4_ANALOG_STATION_OK)
		return false;
	for (uint32_t index = 0;
	     index < FIXTURE_SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v90_phase3_return_line_rx_result result;

		if (v90_phase4_analog_station_transmit(
			    &generated_analog, &sample) !=
		    V90_PHASE4_ANALOG_STATION_OK)
			return false;
		result = v90_phase3_return_line_rx_sample(
			&return_receive, quantize(law, sample));
		if (result == V90_PHASE3_RETURN_LINE_RX_S_BAR_DETECTED) {
			if (generated_analog.transmit.symbols.bit_index == 0 ||
			    generated_analog.transmit.symbols.bit_index >=
				    generated_analog.transmit.symbols.frame_bit_count ||
			    v90_phase3_return_line_rx_handoff(
				    &return_receive, &generated_receive) !=
				    V90_PHASE3_RETURN_LINE_RX_OK)
				return false;
			*analog = generated_analog;
			*receive = generated_receive;
			return true;
		}
		if (result != V90_PHASE3_RETURN_LINE_RX_OK)
			return false;
	}
	return false;
}
