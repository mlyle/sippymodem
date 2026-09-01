// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_dil_sequence.h>
#include <v90_phase3_analog_station.h>
#include <v90_phase3_digital_station.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	SAMPLE_LIMIT = 70000,
	NOMINAL_AMPLITUDE = 6000,
	POST_POINT_LIMIT =
		2 * V34_LINE_PULSE_SPAN_SYMBOLS +
		V34_EQUALIZER_DECISION_DELAY +
		V90_DIL_MAX_SEGMENT_SYMBOLS,
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

static bool run_exchange(
	enum g711_law law, enum v90_dil_profile dil_profile)
{
	struct v34_phase3_profile selected = profile();
	struct v90_phase3_analog_rx_configuration configuration =
		receive_configuration();
	struct v90_jd_parameters parameters = jd_parameters();
	struct v90_phase3_analog_station analog;
	struct v90_phase3_digital_station digital;
	struct v34_training_line phase4_line;
	struct v90_phase3_analog_receive_handoff receive_handoff;
	struct v90_phase3_return_line_handoff return_handoff;
	struct v90_phase3_digital_transmit_handoff transmit_handoff;
	struct v90_dil_parameters expected_dil;
	uint8_t training_ucode = 91;
	uint32_t expected_dil_symbols;
	uint32_t phase4_points = 0;
	uint32_t receive_complete_at = UINT32_MAX;
	uint32_t transmit_complete_at = UINT32_MAX;
	bool phase4_line_active = false;
	bool second_s_seen = false;
	bool dil_completion_seen = false;

	CHECK(v90_dil_profile_init(&expected_dil, dil_profile) == V90_DIL_OK &&
	      v90_dil_training_symbol_count(
		      &expected_dil, &expected_dil_symbols) &&
	      v90_phase3_analog_station_init(
		      &analog, &selected, law, training_ucode,
		      NOMINAL_AMPLITUDE, dil_profile) ==
		      V90_PHASE3_ANALOG_STATION_OK &&
	      v90_phase3_digital_station_init(
		      &digital, &selected, &configuration, law,
		      training_ucode, &parameters) ==
		      V90_PHASE3_DIGITAL_STATION_OK,
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
		if (!v90_phase3_digital_station_transmit_complete(&digital))
			CHECK(v90_phase3_digital_station_transmit(
				      &digital, &digital_octet) ==
				      V90_PHASE3_DIGITAL_STATION_OK,
			      "law %u digital transmit failed at %u",
			      (unsigned int)law, index);
		else
			digital_octet = 0;
		if (!v90_phase3_analog_station_receive_complete(&analog)) {
			CHECK(v90_phase3_analog_station_receive(
				      &analog, digital_octet) ==
				      V90_PHASE3_ANALOG_STATION_OK,
			      "law %u analogue receive failed at %u",
			      (unsigned int)law, index);
			if (v90_phase3_analog_station_receive_complete(&analog))
				receive_complete_at = index;
		}
		if (!v90_phase3_digital_station_receive_complete(&digital))
			CHECK(v90_phase3_digital_station_receive(
				      &digital,
				      quantize(law, analog_sample)) ==
				      V90_PHASE3_DIGITAL_STATION_OK,
			      "law %u digital receive failed at %u",
			      (unsigned int)law, index);
		if (expected_dil_symbols != 0u &&
		    digital.receive_stage ==
			    V90_PHASE3_DIGITAL_RECEIVE_DIL_RETURN) {
			if (!v90_phase3_digital_station_receive_complete(
				    &digital) &&
			    digital.receive.returning.points.stage ==
				    V90_PHASE3_RETURN_RX_STAGE_S) {
				second_s_seen = true;
				CHECK(!digital.transmit.dil.finish_requested,
				      "law %u second S ended DIL before S-bar",
				      (unsigned int)law);
			}
			if (v90_phase3_digital_station_receive_complete(&digital)) {
				dil_completion_seen = true;
				CHECK(digital.transmit.dil.finish_requested,
				      "law %u complete S-bar did not end DIL",
				      (unsigned int)law);
			}
		}
		if (phase4_line_active &&
		    v90_phase3_digital_station_receive_complete(&digital) &&
		    v90_phase3_digital_station_transmit_complete(&digital))
			break;
	}
	CHECK(receive_complete_at < transmit_complete_at &&
	      phase4_line_active &&
	      v90_phase3_digital_station_receive_complete(&digital) &&
	      v90_phase3_digital_station_transmit_complete(&digital) &&
	      phase4_points >= V34_EQUALIZER_DECISION_DELAY &&
	      v90_phase3_analog_station_receive_handoff(
		      &analog, &receive_handoff) ==
		      V90_PHASE3_ANALOG_STATION_OK &&
	      v90_phase3_digital_station_receive_handoff(
		      &digital, &return_handoff) ==
		      V90_PHASE3_DIGITAL_STATION_OK &&
	      v90_phase3_digital_station_transmit_handoff(
		      &digital, &transmit_handoff) ==
		      V90_PHASE3_DIGITAL_STATION_OK &&
	      v90_phase3_return_line_handoff_is_valid(&return_handoff) &&
	      jd_equal(&receive_handoff.jd, &parameters) &&
	      jd_equal(&transmit_handoff.jd, &parameters) &&
	      receive_handoff.descrambler.history ==
		      transmit_handoff.scrambler.history &&
	      receive_handoff.last_sign ==
		      transmit_handoff.last_sign &&
	      receive_handoff.trn1d_symbols == V90_PHASE3_TRN1D_MIN_SYMBOLS &&
	      analog.dil_training_symbols == expected_dil_symbols &&
	      analog.dil_received_symbols ==
		      (expected_dil_symbols == 0u ? 0u :
			      expected_dil_symbols + 1u) &&
	      memcmp(
		      &digital.transmit.dil.parameters, &expected_dil,
		      sizeof(expected_dil)) == 0 &&
	      digital.receive_stage ==
		      (expected_dil_symbols == 0u ?
			      V90_PHASE3_DIGITAL_RECEIVE_RETURN :
			      V90_PHASE3_DIGITAL_RECEIVE_DIL_RETURN) &&
	      analog.transmit_stage ==
		      (expected_dil_symbols == 0u ?
			      V90_PHASE3_ANALOG_TRANSMIT_RETURN :
			      V90_PHASE3_ANALOG_TRANSMIT_DIL_RETURN) &&
	      analog.transmit.returning.scr_during_idle ==
		      (expected_dil_symbols != 0u) &&
	      (expected_dil_symbols == 0u ||
	       (analog.transmit.returning.idle_scr.constellation ==
			parameters.training_constellation &&
		analog.transmit.returning.idle_scr.has_last_symbol)) &&
	      analog.transmit.returning.points.s_symbols >=
		      V34_PHASE3_S_SYMBOLS &&
	      (expected_dil_symbols == 0u ||
	       (second_s_seen && dil_completion_seen)) &&
	      !v90_phase3_analog_station_has_failed(&analog) &&
	      !v90_phase3_digital_station_has_failed(&digital),
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
	{
		struct v90_phase3_digital_station saved = digital;
		uint8_t octet = 0xa5;

		CHECK(v90_phase3_digital_station_transmit(&digital, &octet) ==
			      V90_PHASE3_DIGITAL_STATION_COMPLETE &&
		      v90_phase3_digital_station_receive(&digital, 1234) ==
			      V90_PHASE3_DIGITAL_STATION_COMPLETE &&
		      octet == 0xa5 &&
		      memcmp(&digital, &saved, sizeof(digital)) == 0,
		      "law %u complete digital station consumed media",
		      (unsigned int)law);
	}
	return true;
}

static bool test_generated_exchange(void)
{
	CHECK(run_exchange(G711_LAW_PCMU, V90_DIL_PROFILE_NONE) &&
	      run_exchange(G711_LAW_PCMA, V90_DIL_PROFILE_NONE) &&
	      run_exchange(G711_LAW_PCMU, V90_DIL_PROFILE_CONVENTIONAL) &&
	      run_exchange(G711_LAW_PCMA, V90_DIL_PROFILE_CONVENTIONAL),
	      "generated analogue-station exchanges failed");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase3_profile selected = profile();
	struct v90_phase3_analog_rx_configuration configuration =
		receive_configuration();
	struct v90_jd_parameters parameters = jd_parameters();
	struct v90_phase3_analog_station station;
	struct v90_phase3_analog_station saved;
	struct v90_phase3_digital_station digital;
	struct v90_phase3_digital_station saved_digital;
	struct v34_training_line line;
	struct v34_training_line saved_line;
	struct v90_phase3_analog_receive_handoff handoff;
	struct v90_phase3_analog_receive_handoff saved_handoff;
	struct v90_phase3_digital_transmit_handoff transmit_handoff;
	struct v90_phase3_digital_transmit_handoff saved_transmit_handoff;
	struct v90_phase3_return_line_handoff return_handoff;
	struct v90_phase3_return_line_handoff saved_return_handoff;
	struct v90_pcm_symbol wrong_sd_bar;
	uint8_t octet = 0xa5;
	int16_t sample = 777;

	memset(&station, 0xa5, sizeof(station));
	saved = station;
	CHECK(v90_phase3_analog_station_init(
		      NULL, &selected, G711_LAW_PCMU, 91,
		      NOMINAL_AMPLITUDE, V90_DIL_PROFILE_NONE) ==
		      V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v90_phase3_analog_station_init(
		      &station, &selected, (enum g711_law)99, 91,
		      NOMINAL_AMPLITUDE, V90_DIL_PROFILE_NONE) ==
		      V90_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      memcmp(&station, &saved, sizeof(station)) == 0,
	      "invalid initialization changed station state");
	CHECK(v90_phase3_analog_station_init(
		      &station, &selected, G711_LAW_PCMU, 91,
		      NOMINAL_AMPLITUDE, V90_DIL_PROFILE_NONE) ==
		      V90_PHASE3_ANALOG_STATION_OK,
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
	station.receive.sd.stage = V90_PHASE3_SD_RECEIVER_SD_BAR;
	station.receive.sd.history_count =
		V90_PHASE3_SD_ACQUISITION_SYMBOLS;
	station.receive.sd.bar_symbols =
		V90_PHASE3_SD_BAR_QUALIFY_SYMBOLS;
	station.receive.sd.bar_errors = V90_PHASE3_SD_BAR_MAX_ERRORS;
	CHECK(v90_pcm_symbol_make(
		      &wrong_sd_bar, G711_LAW_PCMU, 91, true),
	      "could not make mismatch fixture");
	CHECK(v90_phase3_analog_station_is_valid(&station) &&
	      v90_phase3_analog_station_receive(
		      &station, wrong_sd_bar.octet) ==
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
	memset(&digital, 0xa5, sizeof(digital));
	saved_digital = digital;
	CHECK(v90_phase3_digital_station_init(
		      NULL, &selected, &configuration, G711_LAW_PCMU, 91,
		      &parameters) ==
		      V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v90_phase3_digital_station_init(
		      &digital, &selected, &configuration,
		      (enum g711_law)99, 91, &parameters) ==
		      V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      memcmp(&digital, &saved_digital, sizeof(digital)) == 0,
	      "invalid digital initialization changed state");
	CHECK(v90_phase3_digital_station_init(
		      &digital, &selected, &configuration, G711_LAW_PCMU, 91,
		      &parameters) == V90_PHASE3_DIGITAL_STATION_OK,
	      "validation digital station did not initialize");
	saved_digital = digital;
	CHECK(v90_phase3_digital_station_transmit(&digital, &octet) ==
		      V90_PHASE3_DIGITAL_STATION_OK &&
	      octet == g711_encode_pcmu(0) &&
	      memcmp(&digital, &saved_digital, sizeof(digital)) == 0,
	      "pre-Ja digital silence changed state");
	memset(&transmit_handoff, 0x5a, sizeof(transmit_handoff));
	saved_transmit_handoff = transmit_handoff;
	memset(&return_handoff, 0xa5, sizeof(return_handoff));
	saved_return_handoff = return_handoff;
	octet = 0x3c;
	CHECK(v90_phase3_digital_station_transmit(NULL, &octet) ==
		      V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v90_phase3_digital_station_transmit(&digital, NULL) ==
		      V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v90_phase3_digital_station_receive(NULL, 0) ==
		      V90_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v90_phase3_digital_station_transmit_handoff(
		      &digital, &transmit_handoff) ==
		      V90_PHASE3_DIGITAL_STATION_INVALID_STATE &&
	      v90_phase3_digital_station_receive_handoff(
		      &digital, &return_handoff) ==
		      V90_PHASE3_DIGITAL_STATION_INVALID_STATE &&
	      octet == 0x3c &&
	      memcmp(&digital, &saved_digital, sizeof(digital)) == 0 &&
	      memcmp(&transmit_handoff, &saved_transmit_handoff,
		     sizeof(transmit_handoff)) == 0 &&
	      memcmp(&return_handoff, &saved_return_handoff,
		     sizeof(return_handoff)) == 0 &&
	      !v90_phase3_digital_transmit_handoff_is_valid(NULL) &&
	      strcmp(v90_phase3_digital_station_result_name(
		     (enum v90_phase3_digital_station_result)99),
		     "unknown") == 0,
	      "invalid digital calls or result names changed");
	digital.receive.initial.failed = true;
	CHECK(!v90_phase3_digital_station_is_valid(&digital) &&
	      !v90_phase3_digital_station_has_failed(&digital),
	      "digital receiver failure escaped the owner invariant");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated DIL exchanges", test_generated_exchange },
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
