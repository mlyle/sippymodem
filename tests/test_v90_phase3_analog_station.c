// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase3_analog_line_rx.h>
#include <v90_phase3_analog_station.h>
#include <v90_phase3_digital_tx.h>
#include <v90_phase3_return_line_rx.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	SAMPLE_LIMIT = 50000,
	NOMINAL_AMPLITUDE = 6000,
	POST_POINT_LIMIT =
		2 * V34_TRAINING_LINE_PULSE_SPAN_SYMBOLS +
		QAM_EQUALIZER_DELAY,
};

enum digital_receive_stage {
	DIGITAL_RECEIVE_INITIAL,
	DIGITAL_RECEIVE_RETURN,
	DIGITAL_RECEIVE_COMPLETE,
};

union digital_receive_state {
	struct v90_phase3_analog_line_rx initial;
	struct v90_phase3_return_line_rx returning;
};

struct digital_endpoint {
	union digital_receive_state receive;
	struct v90_phase3_digital_tx transmit;
	enum digital_receive_stage receive_stage;
	bool transmit_started;
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

static struct v90_phase3_analog_rx_configuration receive_configuration(void)
{
	return (struct v90_phase3_analog_rx_configuration){
		.s_gain_adaptation = 0.15,
		.pp_equalizer_adaptation = 0.2,
		.tracking_equalizer_adaptation = 0.05,
	};
}

static struct v90_jd_parameters jd_parameters(void)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = V90_JD_RATE_CAPABILITY_MASK,
		.training_constellation = V34_CONSTELLATION_16_POINT,
		.rate_renegotiation_constellation = V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 2,
	};
}

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static uint8_t silence_octet(enum g711_law law)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(0) : g711_encode_pcmu(0);
}

static bool jd_equal(
	const struct v90_jd_parameters *left,
	const struct v90_jd_parameters *right)
{
	return left->data_rate_capability_mask ==
		       right->data_rate_capability_mask &&
	       left->training_constellation == right->training_constellation &&
	       left->rate_renegotiation_constellation ==
		       right->rate_renegotiation_constellation &&
	       left->maximum_spectral_shaping_lookahead ==
		       right->maximum_spectral_shaping_lookahead;
}

static bool digital_init(
	struct digital_endpoint *digital,
	const struct v34_phase3_profile *selected)
{
	struct v90_phase3_analog_rx_configuration configuration =
		receive_configuration();

	*digital = (struct digital_endpoint){ 0 };
	return v90_phase3_analog_line_rx_init(
		       &digital->receive.initial, selected, &configuration) ==
	       V90_PHASE3_ANALOG_LINE_RX_OK;
}

static bool digital_start_transmit(
	struct digital_endpoint *digital, enum g711_law law,
	uint8_t training_ucode,
	const struct v90_jd_parameters *parameters)
{
	return v90_phase3_digital_tx_init(
		       &digital->transmit, law, training_ucode, parameters) ==
		       V90_PHASE3_DIGITAL_TX_OK &&
	       (digital->transmit_started = true);
}

static bool digital_receive(
	struct digital_endpoint *digital, int16_t sample, enum g711_law law,
	uint8_t training_ucode,
	const struct v90_jd_parameters *parameters)
{
	if (digital->receive_stage == DIGITAL_RECEIVE_INITIAL) {
		struct v90_phase3_analog_line_handoff handoff;
		enum v90_phase3_analog_line_rx_result result =
			v90_phase3_analog_line_rx_sample(
				&digital->receive.initial, sample);

		if (result == V90_PHASE3_ANALOG_LINE_RX_JA_DETECTED) {
			struct v90_phase3_return_line_rx returning;

			if (v90_phase3_analog_line_rx_handoff(
				    &digital->receive.initial, &handoff) !=
				    V90_PHASE3_ANALOG_LINE_RX_OK ||
			    v90_phase3_return_line_rx_init(
				    &returning, &handoff, 0.05) !=
				    V90_PHASE3_RETURN_LINE_RX_OK ||
			    !digital_start_transmit(
				    digital, law, training_ucode, parameters))
				return false;
			digital->receive = (union digital_receive_state){ 0 };
			digital->receive.returning = returning;
			digital->receive_stage = DIGITAL_RECEIVE_RETURN;
			return true;
		}
		return result == V90_PHASE3_ANALOG_LINE_RX_OK ||
		       result == V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED ||
		       result == V90_PHASE3_ANALOG_LINE_RX_S_DETECTED ||
		       result == V90_PHASE3_ANALOG_LINE_RX_PP_COMPLETE ||
		       result == V90_PHASE3_ANALOG_LINE_RX_JA_REJECTED;
	}
	if (digital->receive_stage == DIGITAL_RECEIVE_RETURN) {
		enum v90_phase3_return_line_rx_result result =
			v90_phase3_return_line_rx_sample(
				&digital->receive.returning, sample);

		if (result == V90_PHASE3_RETURN_LINE_RX_S_DETECTED)
			return digital->transmit_started &&
			       v90_phase3_digital_tx_finish_jd(
				       &digital->transmit) ==
				       V90_PHASE3_DIGITAL_TX_OK;
		if (result == V90_PHASE3_RETURN_LINE_RX_S_BAR_DETECTED) {
			digital->receive_stage = DIGITAL_RECEIVE_COMPLETE;
			return true;
		}
		return result == V90_PHASE3_RETURN_LINE_RX_OK;
	}
	return true;
}

static bool digital_transmit(
	struct digital_endpoint *digital, enum g711_law law,
	uint8_t *network_octet)
{
	struct v90_pcm_symbol symbol;

	if (!digital->transmit_started ||
	    digital->transmit.stage == V90_PHASE3_DIGITAL_TX_COMPLETE) {
		*network_octet = silence_octet(law);
		return true;
	}
	if (v90_phase3_digital_tx_next(&digital->transmit, &symbol) !=
	    V90_PHASE3_DIGITAL_TX_OK)
		return false;
	*network_octet = symbol.octet;
	return true;
}

static bool continue_phase4_line(
	struct v34_training_line *line, uint32_t *points, int16_t *sample)
{
	struct qam_point point;
	const struct qam_point *supplied = NULL;
	bool accepting = v34_training_line_needs_point(line);
	bool accepted;

	if (accepting) {
		if (*points >= POST_POINT_LIMIT)
			return false;
		point = v34_rotated_point_zero((int)((*points * 3u) & 3u));
		supplied = &point;
	}
	if (v34_training_line_sample(
		    line, supplied, sample, &accepted) != V34_TRAINING_LINE_OK ||
	    accepted != accepting)
		return false;
	if (accepted)
		(*points)++;
	return true;
}

static bool run_exchange(enum g711_law law)
{
	struct v34_phase3_profile selected = profile();
	struct v90_jd_parameters parameters = jd_parameters();
	struct v90_phase3_analog_station analog;
	struct digital_endpoint digital;
	struct v34_training_line phase4_line;
	struct v90_phase3_analog_receive_handoff receive_handoff;
	struct v90_phase3_return_line_handoff return_handoff;
	uint8_t training_ucode = 91;
	uint32_t phase4_points = 0;
	uint32_t receive_complete_at = UINT32_MAX;
	uint32_t transmit_complete_at = UINT32_MAX;
	bool phase4_line_active = false;

	CHECK(v90_phase3_analog_station_init(
		      &analog, &selected, law, training_ucode,
		      NOMINAL_AMPLITUDE) == V90_PHASE3_ANALOG_STATION_OK &&
	      digital_init(&digital, &selected),
	      "law %u endpoints did not initialize", (unsigned int)law);
	for (uint32_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t analog_sample;
		uint8_t digital_octet;

		if (!phase4_line_active) {
			CHECK(v90_phase3_analog_station_transmit(
				      &analog, &analog_sample) ==
				      V90_PHASE3_ANALOG_STATION_OK,
			      "law %u analogue transmit failed at %u",
			      (unsigned int)law, index);
			if (v90_phase3_analog_station_transmit_complete(&analog)) {
				CHECK(v90_phase3_analog_station_transmit_handoff(
					      &analog, &phase4_line) ==
					      V90_PHASE3_ANALOG_STATION_OK,
				      "law %u transmit handoff failed",
				      (unsigned int)law);
				phase4_line_active = true;
				transmit_complete_at = index;
			}
		} else {
			CHECK(continue_phase4_line(
				      &phase4_line, &phase4_points,
				      &analog_sample),
			      "law %u Phase 4 continuation failed at %u",
			      (unsigned int)law, index);
		}
		CHECK(digital_transmit(&digital, law, &digital_octet),
		      "law %u digital transmit failed at %u",
		      (unsigned int)law, index);
		if (!v90_phase3_analog_station_receive_complete(&analog)) {
			CHECK(v90_phase3_analog_station_receive(
				      &analog, digital_octet) ==
				      V90_PHASE3_ANALOG_STATION_OK,
			      "law %u analogue receive failed at %u",
			      (unsigned int)law, index);
			if (v90_phase3_analog_station_receive_complete(&analog))
				receive_complete_at = index;
		}
		CHECK(digital_receive(
			      &digital, quantize(law, analog_sample), law,
			      training_ucode, &parameters),
		      "law %u digital receive failed at %u",
		      (unsigned int)law, index);
		if (phase4_line_active &&
		    digital.receive_stage == DIGITAL_RECEIVE_COMPLETE)
			break;
	}
	CHECK(receive_complete_at < transmit_complete_at &&
	      phase4_line_active &&
	      digital.receive_stage == DIGITAL_RECEIVE_COMPLETE &&
	      digital.transmit.stage == V90_PHASE3_DIGITAL_TX_COMPLETE &&
	      phase4_points >= QAM_EQUALIZER_DELAY &&
	      v90_phase3_analog_station_receive_handoff(
		      &analog, &receive_handoff) ==
		      V90_PHASE3_ANALOG_STATION_OK &&
	      v90_phase3_return_line_rx_handoff(
		      &digital.receive.returning, &return_handoff) ==
		      V90_PHASE3_RETURN_LINE_RX_OK &&
	      v90_phase3_return_line_handoff_is_valid(&return_handoff) &&
	      jd_equal(&receive_handoff.jd, &parameters) &&
	      receive_handoff.descrambler.history ==
		      digital.transmit.sequence.signs.scrambler.history &&
	      receive_handoff.last_sign ==
		      digital.transmit.sequence.signs.last_sign &&
	      receive_handoff.trn1d_symbols == V90_PHASE3_TRN1D_MIN_SYMBOLS &&
	      analog.transmit.returning.points.s_symbols >=
		      V34_PHASE3_S_SYMBOLS &&
	      !v90_phase3_analog_station_has_failed(&analog),
	      "law %u did not reach matching independent handoffs",
	      (unsigned int)law);
	{
		struct v90_phase3_analog_station saved = analog;
		int16_t sample = 1234;

		CHECK(v90_phase3_analog_station_transmit(&analog, &sample) ==
			      V90_PHASE3_ANALOG_STATION_COMPLETE &&
		      v90_phase3_analog_station_receive(&analog, 0xa5) ==
			      V90_PHASE3_ANALOG_STATION_COMPLETE &&
		      sample == 1234 &&
		      memcmp(&analog, &saved, sizeof(analog)) == 0,
		      "law %u complete station consumed media",
		      (unsigned int)law);
	}
	return true;
}

static bool test_generated_exchange(void)
{
	CHECK(run_exchange(G711_LAW_PCMU) && run_exchange(G711_LAW_PCMA),
	      "generated analogue-station exchanges failed");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase3_profile selected = profile();
	struct v90_phase3_analog_station station;
	struct v90_phase3_analog_station saved;
	struct v34_training_line line;
	struct v34_training_line saved_line;
	struct v90_phase3_analog_receive_handoff handoff;
	struct v90_phase3_analog_receive_handoff saved_handoff;
	int16_t sample = 777;

	memset(&station, 0xa5, sizeof(station));
	saved = station;
	CHECK(v90_phase3_analog_station_init(
		      NULL, &selected, G711_LAW_PCMU, 91,
		      NOMINAL_AMPLITUDE) ==
		      V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v90_phase3_analog_station_init(
		      &station, &selected, (enum g711_law)99, 91,
		      NOMINAL_AMPLITUDE) ==
		      V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      memcmp(&station, &saved, sizeof(station)) == 0,
	      "invalid initialization changed station state");
	CHECK(v90_phase3_analog_station_init(
		      &station, &selected, G711_LAW_PCMU, 91,
		      NOMINAL_AMPLITUDE) == V90_PHASE3_ANALOG_STATION_OK,
	      "validation station did not initialize");
	saved = station;
	memset(&line, 0xa5, sizeof(line));
	saved_line = line;
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v90_phase3_analog_station_transmit(NULL, &sample) ==
		      V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v90_phase3_analog_station_transmit(&station, NULL) ==
		      V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v90_phase3_analog_station_transmit_handoff(&station, &line) ==
		      V90_PHASE3_ANALOG_STATION_INVALID_STATE &&
	      v90_phase3_analog_station_receive_handoff(&station, &handoff) ==
		      V90_PHASE3_ANALOG_STATION_INVALID_STATE &&
	      memcmp(&station, &saved, sizeof(station)) == 0 && sample == 777 &&
	      memcmp(&line, &saved_line, sizeof(line)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "invalid station calls were not transactional");
	CHECK(v90_phase3_sign_receiver_init(
		      &station.receive.signs, G711_LAW_PCMU, 91) ==
		      V90_PHASE3_SIGN_RECEIVER_OK,
	      "could not make mismatch fixture");
	station.receive_stage = V90_PHASE3_ANALOG_RECEIVE_SIGNS;
	CHECK(v90_phase3_analog_station_is_valid(&station) &&
	      v90_phase3_analog_station_receive(&station, 0) ==
		      V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH &&
	      v90_phase3_analog_station_has_failed(&station) &&
	      v90_phase3_analog_station_transmit(&station, &sample) ==
		      V90_PHASE3_ANALOG_STATION_FAILED &&
	      !v90_phase3_analog_receive_handoff_is_valid(NULL) &&
	      strcmp(v90_phase3_analog_station_result_name(
		     V90_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH),
		     "sequence mismatch") == 0 &&
	      strcmp(v90_phase3_analog_station_result_name(
		     (enum v90_phase3_analog_station_result)99),
		     "unknown") == 0,
	      "failure latch or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated zero-DIL exchange", test_generated_exchange },
		{ "validation and failure", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
