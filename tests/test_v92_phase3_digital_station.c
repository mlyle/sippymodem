// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase4_trn2u.h>
#include <v92_phase3_analog_station.h>
#include <v92_phase3_digital_station.h>

#include "test.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_UCODE = 91,
	TEST_SU_THRESHOLD = 256,
	SAMPLE_LIMIT = 20000,
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpt_parameters cpt_parameters(enum g711_law law)
{
	struct v92_cpt_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x4a35),
		.shaping_a1_q1_6 = 12,
		.shaping_a2_q1_6 = -7,
		.shaping_b1_q1_6 = 3,
		.shaping_b2_q1_6 = -2,
		.selected_rate_index = 9,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
		.acknowledges_peer = true,
	};

	add_ucode(&parameters.transmit_constellations[0], 24u);
	add_ucode(&parameters.transmit_constellations[0], 80u);
	return parameters;
}

static struct v92_phase3_analog_station_configuration analog_configuration(
	enum g711_law law, uint32_t rate_mask)
{
	return (struct v92_phase3_analog_station_configuration){
		.local_info1a = {
			.filter_sections = V92_PCM_FILTER_P1_Z2,
			.maximum_total_coefficients = 192,
			.maximum_section_coefficients = 128,
			.local_md_length_276_symbol_units = 0,
			.training_ucode = TEST_UCODE,
		},
		.cpt = cpt_parameters(law),
		.upstream_rate_mask = rate_mask,
		.initial_trn1u_symbols =
			V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS,
		.law = law,
		.upstream_lu_ucode = TEST_UCODE,
	};
}

static bool parameters_match(
	const struct v92_cpt_parameters *left,
	const struct v92_cpt_parameters *right)
{
	uint8_t left_frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	uint8_t right_frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t left_bits = 0u;
	size_t right_bits = 0u;

	return v92_cpt_encode(
		       left, left_frame, sizeof(left_frame), &left_bits) ==
		       V92_CPT_OK &&
	       v92_cpt_encode(
		       right, right_frame, sizeof(right_frame),
		       &right_bits) == V92_CPT_OK &&
	       left_bits == right_bits &&
	       memcmp(left_frame, right_frame, sizeof(left_frame)) == 0;
}

static int16_t media_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static bool run_exchange(enum g711_law law, bool delayed)
{
	const enum v92_constellation_size training_constellation = delayed ?
		V92_CONSTELLATION_4_POINT : V92_CONSTELLATION_8_POINT;
	struct v92_jd jd = {
		.data_rate_capability_mask = UINT32_C(0x002a1555),
		.maximum_spectral_shaping_lookahead = 3,
	};
	struct v92_jp jp = {
		.training_constellation = training_constellation,
		.rate_renegotiation_constellation =
			V92_CONSTELLATION_4_POINT,
	};
	const uint32_t rate_mask = UINT32_C(0x52491);
	const struct v92_phase3_analog_station_configuration configuration =
		analog_configuration(law, rate_mask);
	struct v92_phase3_analog_station analog;
	struct v92_phase3_digital_station digital;
	struct v92_pcm_phase4_trn2u trn2u;
	struct v92_pcm_phase3_cpt_line_handoff receive_handoff;
	struct v92_phase3_analog_transmit_handoff transmit_handoff;
	struct v92_cpt_parameters analog_downstream;
	struct v92_cpt_parameters digital_downstream;
	uint32_t received_mask = 0u;
	uint8_t downstream_delay = law == G711_LAW_PCMA ?
		g711_encode_pcma(0) : g711_encode_pcmu(0);
	int16_t upstream_delay = 0;
	bool trn2u_started = false;
	bool finished = false;

	CHECK(v92_phase3_analog_station_init(&analog, &configuration) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      v92_phase3_digital_station_init(
		      &digital, law, TEST_UCODE, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_OK,
	      "V.92 station pair did not initialize for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < SAMPLE_LIMIT && !finished; ++index) {
		uint8_t octet = law == G711_LAW_PCMA ?
			g711_encode_pcma(0) : g711_encode_pcmu(0);
		int16_t sample;

		if (!v92_phase3_digital_station_transmit_complete(&digital)) {
			enum v92_phase3_digital_station_result tx_result =
				v92_phase3_digital_station_transmit(
					&digital, &octet);

			CHECK(tx_result == V92_PHASE3_DIGITAL_STATION_OK,
			      "downstream transmit failed at %zu: %s",
			      index,
			      v92_phase3_digital_station_result_name(tx_result));
		}
		if (!v92_phase3_analog_station_receive_complete(&analog)) {
			uint8_t delivered = delayed ? downstream_delay : octet;
			enum v92_phase3_analog_station_result result;

			downstream_delay = octet;
			result = v92_phase3_analog_station_receive(
				&analog, delivered);
			CHECK(result == V92_PHASE3_ANALOG_STATION_OK ||
			      result == V92_PHASE3_ANALOG_STATION_COMPLETE,
			      "downstream receive failed at %zu: %s", index,
			      v92_phase3_analog_station_result_name(result));
		}

		if (!v92_phase3_analog_station_transmit_complete(&analog)) {
			enum v92_phase3_analog_station_result result =
				v92_phase3_analog_station_transmit(
					&analog, NULL, &sample);

			CHECK(result == V92_PHASE3_ANALOG_STATION_OK,
			      "analogue transmitter returned %s at %zu",
			      v92_phase3_analog_station_result_name(result),
			      index);
		} else {
			if (!trn2u_started) {
				CHECK(v92_phase3_analog_station_transmit_handoff(
					      &analog, &transmit_handoff) ==
					      V92_PHASE3_ANALOG_STATION_OK &&
				      v92_pcm_phase4_trn2u_init(
					      &trn2u,
					      transmit_handoff
						      .training_constellation,
					      transmit_handoff
						      .previous_encoded_sign,
					      transmit_handoff.lu) ==
					      V92_PCM_PHASE4_TRN2U_OK,
				      "analogue Phase 4 handoff failed");
				trn2u_started = true;
			}
			CHECK(v92_pcm_phase4_trn2u_next(&trn2u, &sample) ==
				      V92_PCM_PHASE4_TRN2U_OK,
			      "TRN2u generation failed at %zu", index);
		}
		sample = media_sample(law, sample);
		if (!v92_phase3_digital_station_receive_complete(&digital)) {
			int16_t delivered = delayed ? upstream_delay : sample;
			enum v92_phase3_digital_station_result result =
				v92_phase3_digital_station_receive(
					&digital, delivered);

			upstream_delay = sample;
			CHECK(result == V92_PHASE3_DIGITAL_STATION_OK ||
			      result == V92_PHASE3_DIGITAL_STATION_COMPLETE,
			      "digital receiver returned %s at %zu",
			      v92_phase3_digital_station_result_name(result),
			      index);
		}
		finished =
			v92_phase3_analog_station_transmit_complete(&analog) &&
			v92_phase3_analog_station_receive_complete(&analog) &&
			v92_phase3_digital_station_transmit_complete(&digital) &&
			v92_phase3_digital_station_receive_complete(&digital);
	}
	CHECK(finished && trn2u_started &&
	      !v92_phase3_analog_station_has_failed(&analog) &&
	      !v92_phase3_digital_station_has_failed(&digital) &&
	      analog.transmit.cpt.finish_requested &&
	      analog.transmit.cpt.stage == V92_PCM_PHASE3_CPT_TX_COMPLETE &&
	      analog.receive.ri.stage == V90_PHASE4_RI_SEARCH_COMPLETE &&
	      digital.downstream.stage == PCM_PHASE3_DIGITAL_TX_COMPLETE &&
	      digital.downstream.scr_symbols >= 6u &&
	      digital.downstream.scr_symbols % 6u == 0u &&
	      digital.ri.finish_requested &&
	      digital.ri.stage == V90_PHASE4_RI_COMPLETE &&
	      digital.jp.su_phase_extension_q16 != 0u,
	      "V.92 station pair did not reach both Phase 4 boundaries");
	CHECK(v92_phase3_digital_station_upstream_rate_mask(
		      &digital, &received_mask) &&
	      received_mask == rate_mask &&
	      v92_phase3_analog_station_downstream_handoff(
		      &analog, &analog_downstream) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      v92_phase3_digital_station_downstream_handoff(
		      &digital, &digital_downstream) ==
		      V92_PHASE3_DIGITAL_STATION_OK &&
	      parameters_match(&analog_downstream, &configuration.cpt) &&
	      parameters_match(&digital_downstream, &configuration.cpt) &&
	      v92_phase3_digital_station_receive_handoff(
		      &digital, &receive_handoff) ==
		      V92_PHASE3_DIGITAL_STATION_OK &&
	      v92_pcm_phase3_cpt_line_handoff_is_valid(&receive_handoff) &&
	      parameters_match(
		      &receive_handoff.parameters, &configuration.cpt) &&
	      v92_phase3_analog_station_transmit_handoff(
		      &analog, &transmit_handoff) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      transmit_handoff.lu > 0 &&
	      transmit_handoff.training_constellation ==
		      training_constellation &&
	      transmit_handoff.previous_encoded_sign ==
		      analog.transmit.cpt.coding.previous_sign,
	      "V.92 Phase 4 handoff changed negotiated state");
	return true;
}

static bool test_analog_contracts(void)
{
	struct v92_phase3_analog_station_configuration configuration =
		analog_configuration(G711_LAW_PCMU, 1u);
	struct v92_phase3_analog_station station;
	struct v92_phase3_analog_station before;
	struct v92_phase3_analog_transmit_handoff handoff = {
		.lu = 123,
		.training_constellation = (enum v92_constellation_size)16,
		.previous_encoded_sign = 1,
	};
	struct v92_cpt_parameters downstream;
	struct v92_cpt_parameters downstream_before;
	int16_t sample = 1234;
	static const char *const names[] = {
		"ok", "complete", "need MD symbol", "failed",
		"invalid argument", "invalid state", "sequence mismatch",
		"numeric failure",
	};

	memset(&station, 0xa5, sizeof(station));
	before = station;
	CHECK(v92_phase3_analog_station_init(NULL, &configuration) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v92_phase3_analog_station_init(&station, NULL) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT,
	      "null analogue initialization returned the wrong result");
	configuration.upstream_lu_ucode = 0u;
	CHECK(!v92_phase3_analog_station_configuration_is_valid(
		      &configuration) &&
	      v92_phase3_analog_station_init(&station, &configuration) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      memcmp(&station, &before, sizeof(station)) == 0,
	      "invalid analogue initialization changed its destination");
	configuration = analog_configuration(G711_LAW_PCMU, 1u);
	CHECK(v92_phase3_analog_station_configuration_is_valid(
		      &configuration) &&
	      !v92_phase3_analog_station_configuration_is_valid(NULL) &&
	      v92_phase3_analog_station_init(&station, &configuration) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      v92_phase3_analog_station_is_valid(&station) &&
	      !v92_phase3_analog_station_has_failed(&station) &&
	      !v92_phase3_analog_station_transmit_complete(&station) &&
	      !v92_phase3_analog_station_receive_complete(&station),
	      "fresh analogue station contracts differed");
	before = station;
	memset(&downstream, 0xa5, sizeof(downstream));
	downstream_before = downstream;
	CHECK(v92_phase3_analog_station_transmit_handoff(
		      &station, &handoff) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_STATE &&
	      v92_phase3_analog_station_downstream_handoff(
		      &station, &downstream) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_STATE &&
	      memcmp(&downstream, &downstream_before,
		     sizeof(downstream)) == 0 &&
	      handoff.lu == 123 &&
	      handoff.training_constellation ==
		      (enum v92_constellation_size)16 &&
	      handoff.previous_encoded_sign == 1u &&
	      v92_phase3_analog_station_transmit(&station, NULL, &sample) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      sample == 0,
	      "fresh analogue transmission contracts differed");
	before = station;
	CHECK(v92_phase3_analog_station_transmit(NULL, NULL, &sample) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v92_phase3_analog_station_transmit(&station, NULL, NULL) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v92_phase3_analog_station_receive(NULL, 0u) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v92_phase3_analog_station_transmit_handoff(
		      &station, NULL) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v92_phase3_analog_station_downstream_handoff(
		      &station, NULL) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      memcmp(&station, &before, sizeof(station)) == 0,
	      "invalid analogue calls returned the wrong result");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_phase3_analog_station_result_name(
				      (enum v92_phase3_analog_station_result)
					      index),
			      names[index]) == 0,
		      "analogue result name %zu differed", index);
	CHECK(strcmp(
		      v92_phase3_analog_station_result_name(
			      (enum v92_phase3_analog_station_result)99),
		      "unknown") == 0,
	      "unknown analogue result name differed");
	return true;
}

static bool test_md_backpressure(void)
{
	struct v92_phase3_analog_station_configuration configuration =
		analog_configuration(G711_LAW_PCMU, 1u);
	struct v92_phase3_analog_station station;
	struct v92_phase3_analog_station before;
	struct v90_pcm_symbol md_symbol;
	int16_t sample = 0;
	bool reached_md = false;

	configuration.local_info1a.local_md_length_276_symbol_units = 1u;
	CHECK(v92_phase3_analog_station_init(&station, &configuration) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      v90_pcm_symbol_make(
		      &md_symbol, G711_LAW_PCMU, 47u, true),
	      "MD station fixture did not initialize");
	before = station;
	CHECK(v92_phase3_analog_station_transmit(
		      &station, &md_symbol, &sample) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      memcmp(&station, &before, sizeof(station)) == 0,
	      "early MD symbol changed analogue state");
	for (size_t index = 0; index < 5000u && !reached_md; ++index) {
		sample = 1234;
		before = station;
		enum v92_phase3_analog_station_result result =
			v92_phase3_analog_station_transmit(
				&station, NULL, &sample);

		if (result == V92_PHASE3_ANALOG_STATION_NEED_MD_SYMBOL) {
			reached_md = sample == 1234 &&
				     memcmp(&station, &before, sizeof(station)) == 0;
			break;
		}
		CHECK(result == V92_PHASE3_ANALOG_STATION_OK,
		      "MD opening returned %s",
		      v92_phase3_analog_station_result_name(result));
	}
	CHECK(reached_md &&
	      v92_phase3_analog_station_transmit(
		      &station, &md_symbol, &sample) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      v92_phase3_analog_station_is_valid(&station),
	      "MD backpressure did not resume transactionally");
	return true;
}

static bool test_digital_contracts(void)
{
	struct v92_jd jd = {
		.data_rate_capability_mask = 1u,
		.maximum_spectral_shaping_lookahead = 1,
	};
	struct v92_jp jp = {
		.training_constellation = V92_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation =
			V92_CONSTELLATION_4_POINT,
	};
	struct v92_phase3_digital_station station;
	struct v92_phase3_digital_station before;
	struct v92_cpt_parameters downstream;
	struct v92_cpt_parameters downstream_before;
	uint8_t octet = 0u;
	uint32_t mask = UINT32_C(0xa5a5a5a5);
	static const char *const names[] = {
		"ok", "complete", "failed", "invalid argument",
		"invalid state", "sequence mismatch", "unsupported",
		"numeric failure",
	};

	memset(&station, 0xa5, sizeof(station));
	before = station;
	CHECK(v92_phase3_digital_station_parameters_are_valid(
		      G711_LAW_PCMU, TEST_UCODE, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) &&
	      !v92_phase3_digital_station_parameters_are_valid(
		      G711_LAW_PCMU, TEST_UCODE, NAN,
		      TEST_SU_THRESHOLD, &jd, &jp) &&
	      v92_phase3_digital_station_init(
		      NULL, G711_LAW_PCMU, TEST_UCODE, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase3_digital_station_init(
		      &station, G711_LAW_PCMU, TEST_UCODE, NAN,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      memcmp(&station, &before, sizeof(station)) == 0,
	      "invalid digital initialization changed its destination");
	CHECK(v92_phase3_digital_station_init(
		      &station, G711_LAW_PCMU, TEST_UCODE, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_OK &&
	      v92_phase3_digital_station_is_valid(&station) &&
	      !v92_phase3_digital_station_has_failed(&station) &&
	      !v92_phase3_digital_station_upstream_rate_mask(
		      &station, &mask) &&
	      mask == UINT32_C(0xa5a5a5a5),
	      "fresh digital station contracts differed");
	before = station;
	memset(&downstream, 0xa5, sizeof(downstream));
	downstream_before = downstream;
	CHECK(v92_phase3_digital_station_transmit(&station, &octet) ==
		      V92_PHASE3_DIGITAL_STATION_OK &&
	      memcmp(&station, &before, sizeof(station)) == 0 &&
	      octet == g711_encode_pcmu(0) &&
	      v92_phase3_digital_station_downstream_handoff(
		      &station, &downstream) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_STATE &&
	      memcmp(&downstream, &downstream_before,
		     sizeof(downstream)) == 0,
	      "pre-Ja silence changed digital state");
	CHECK(v92_phase3_digital_station_transmit(NULL, &octet) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase3_digital_station_receive(NULL, 0) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase3_digital_station_receive_handoff(
		      &station, NULL) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase3_digital_station_downstream_handoff(
		      &station, NULL) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT,
	      "invalid digital calls returned the wrong result");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_phase3_digital_station_result_name(
				      (enum v92_phase3_digital_station_result)
					      index),
			      names[index]) == 0,
		      "digital result name %zu differed", index);
	CHECK(strcmp(
		      v92_phase3_digital_station_result_name(
			      (enum v92_phase3_digital_station_result)99),
		      "unknown") == 0,
	      "unknown digital result name differed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "PCMU zero-DIL station exchange", NULL },
		{ "PCMA zero-DIL station exchange", NULL },
		{ "PCMU delayed station exchange", NULL },
		{ "PCMA delayed station exchange", NULL },
		{ "analogue station contracts", test_analog_contracts },
		{ "manufacturer interval backpressure", test_md_backpressure },
		{ "digital station contracts", test_digital_contracts },
	};
	unsigned int failures = 0u;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (index == 0u)
			passed = run_exchange(G711_LAW_PCMU, false);
		else if (index == 1u)
			passed = run_exchange(G711_LAW_PCMA, false);
		else if (index == 2u)
			passed = run_exchange(G711_LAW_PCMU, true);
		else if (index == 3u)
			passed = run_exchange(G711_LAW_PCMA, true);
		else
			passed = tests[index].run();
		if (passed)
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0u ? 0 : 1;
}
