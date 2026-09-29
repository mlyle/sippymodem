// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase4_trn2u.h>
#include <v92_pcm_phase4_trn2u_line_rx.h>
#include <v92_phase3_analog_station.h>
#include <v92_phase3_digital_station.h>
#include <v92_pcm_phase3_ja.h>

#include "test.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_UCODE = 91,
	TEST_SU_THRESHOLD = 256,
	SAMPLE_LIMIT = V90_PHASE3_TRN1D_SYMBOLS + 18000,
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
		.restore_codec_grid = true,
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

static bool check_jp_prime_deadline(
	const struct v92_phase3_analog_station *before, uint8_t octet)
{
	const uint32_t last = (40000u - V92_SU_JP_BAR_INPUT_SYMBOLS -
		V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS - 1u) / 12u * 12u;
	const uint32_t counts[] = { last - 1u, last, 40000u, UINT32_MAX };

	for (size_t index = 0; index < ARRAY_LENGTH(counts); ++index) {
		struct v92_phase3_analog_station late = *before;
		late.transmit.cpt.trn1u.emitted_symbols = counts[index];
		CHECK(v92_phase3_analog_station_is_valid(&late), "invalid late Jp-prime fixture");
		enum v92_phase3_analog_station_result result =
			v92_phase3_analog_station_receive(&late, octet, octet);

		CHECK(index == 0 ? result == V92_PHASE3_ANALOG_STATION_OK &&
			      late.second_trn1u_symbols == last :
			      result == V92_PHASE3_ANALOG_STATION_SEQUENCE_MISMATCH &&
			      v92_phase3_analog_station_has_failed(&late),
		      "Jp-prime deadline mishandled %u training symbols: %s",
		      counts[index], v92_phase3_analog_station_result_name(result));
	}
	return true;
}

static bool check_su_phase(const struct v92_phase3_analog_station *before)
{
	for (unsigned exact = 0; exact < 2u; ++exact) {
		struct v92_phase3_analog_station station = *before;
		struct fractional_delay expected = before->transmit_delay;
		struct v92_su su = before->transmit.su;
		int16_t sample, expected_sample;
		int32_t phase = exact ? 0 : su.post_jp_phase_q16;

		station.restore_codec_grid = exact != 0;
		CHECK(v92_su_next(&su, &expected, &expected_sample) == V92_SU_REACHED_TRN1U_READY &&
		      fractional_delay_set_phase(&expected, phase) == FRACTIONAL_DELAY_OK,
		      "invalid expected Su handoff");
		CHECK(v92_phase3_analog_station_transmit(&station, NULL, &sample) ==
			      V92_PHASE3_ANALOG_STATION_OK &&
		      station.transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_CPT &&
		      station.transmit.cpt.trn1u.emitted_symbols == 0u &&
		      station.transmit.cpt.trn1u.scrambler.history == 0u &&
		      sample == expected_sample &&
		      memcmp(&station.transmit_delay, &expected, sizeof(expected)) == 0,
		      "Su did not hand off directly to zero-seeded TRN1u at the selected phase");
	}
	return true;
}

static bool check_early_jp_prime(
	const struct v92_phase3_analog_station *before, uint8_t octet)
{
	struct v92_phase3_analog_station station = *before;
	struct v92_su su;
	int16_t sample;

	/* A very short round trip can return Jp-prime while the 24T
	 * response is still being sent. Receive DIL without truncating Su. */
	fractional_delay_init(&station.transmit_delay);
	CHECK(v92_su_init(&su, station.upstream_lu) == V92_SU_OK,
	      "early Jp-prime fixture failed");
	while (su.stage != V92_SU_JP_WAIT)
		CHECK(v92_su_next(&su, &station.transmit_delay, &sample) <= V92_SU_REACHED_JP_WAIT,
		      "early Jp-prime Su setup failed");
	CHECK(v92_su_accept_jp(&su, &station.receive.j.parameters.v92.jp) == V92_SU_OK,
	      "early Jp-prime Jp setup failed");
	station.transmit = (union v92_phase3_analog_transmit){ .su = su };
	station.transmit_stage = V92_PHASE3_ANALOG_TRANSMIT_SU;
	CHECK(v92_phase3_analog_station_receive(&station, octet, octet) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      station.receive_stage == V92_PHASE3_ANALOG_RECEIVE_RI &&
	      station.transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU,
	      "early Jp-prime truncated Su or rejected DIL reception");
	for (unsigned i = 0; i < V92_SU_JP_BAR_INPUT_SYMBOLS; ++i)
		CHECK(v92_phase3_analog_station_transmit(&station, NULL, &sample) ==
			      V92_PHASE3_ANALOG_STATION_OK &&
		      (i + 1u == V92_SU_JP_BAR_INPUT_SYMBOLS ?
			      station.transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_CPT &&
			      station.transmit.cpt.trn1u.emitted_symbols == 0u :
			      station.transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU),
		      "early Jp-prime changed the Su response length");
	return true;
}

static bool check_trn1u_without_su_reversal(
	const struct v92_phase3_digital_station *boundary)
{
	struct v92_phase3_digital_station digital = *boundary;
	struct v92_pcm_phase3_trn1u training;
	const unsigned symbols = V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS +
		V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY;

	/* Let the peer receive a complete Jp before supplying its response. */
	for (unsigned n = 0; n < 512u && !digital.downstream.jp_records; ++n) {
		uint8_t octet;
		CHECK(v92_phase3_digital_station_transmit(&digital, &octet) ==
			      V92_PHASE3_DIGITAL_STATION_OK, "Jp transmission failed");
	}
	CHECK(digital.downstream.jp_records, "Jp did not finish a record");
	/* Model an obscured Su response. Acquisition must retain TRN1u's first
	 * symbol even when the reversal detector has not established Su. */
	CHECK(v92_pcm_phase3_trn1u_init(&training, digital.law, TEST_UCODE) ==
		      V92_PCM_PHASE3_TRN1U_OK, "TRN1u fixture initialization failed");
	for (unsigned n = 0; n < symbols; ++n) {
		struct v90_pcm_symbol symbol;

		CHECK(v92_pcm_phase3_trn1u_next(&training, &symbol) == V92_PCM_PHASE3_TRN1U_OK &&
		      v92_phase3_digital_station_receive(&digital, symbol.sample) ==
			      V92_PHASE3_DIGITAL_STATION_OK,
		      "obscured Su response lost training at symbol %u", n);
	}
	CHECK(digital.receive_stage == V92_PHASE3_DIGITAL_RECEIVE_SECOND_TRN1U &&
	      digital.downstream.finish_jp_requested &&
	      digital.receive.trn1u.stage == V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_TRAINING &&
	      digital.receive.trn1u.output_symbols == V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS &&
	      digital.receive.trn1u.training_errors == 0u,
	      "obscured Su response discarded the zero-seeded TRN1u opening");
	return true;
}

static bool run_exchange(enum g711_law law, unsigned delay,
	enum v90_dil_profile dil_profile, int32_t channel_phase)
{
	const enum v92_constellation_size training_constellation = delay ?
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
	struct v92_phase3_analog_station_configuration configuration =
		analog_configuration(law, rate_mask);
	struct v90_dil_parameters dil;
	uint32_t dil_symbols;
	struct v92_phase3_analog_station analog;
	struct v92_phase3_digital_station digital;
	struct v92_pcm_phase4_trn2u trn2u;
	struct fractional_delay trn2u_delay;
	struct v92_pcm_phase3_cpt_line_handoff receive_handoff;
	struct v92_pcm_phase4_trn2u transmit_handoff;
	struct v92_cpt_parameters analog_downstream;
	struct v92_cpt_parameters digital_downstream;
	uint32_t received_mask = 0u;
	uint8_t downstream_delay[4096];
	int16_t upstream_delay[4096] = { 0 };
	unsigned cursor = 0;
	int16_t previous_channel_sample = 0;
	size_t su_started = SIZE_MAX;
	bool cpt_started = false;
	bool trn2u_started = false;
	bool finished = false;

	configuration.dil_profile = dil_profile;
	configuration.restore_codec_grid = channel_phase == 0;
	CHECK(delay <= ARRAY_LENGTH(downstream_delay), "delay exceeds test wire");
	memset(downstream_delay, law == G711_LAW_PCMA ?
		g711_encode_pcma(0) : g711_encode_pcmu(0), sizeof(downstream_delay));
	CHECK(v90_dil_profile_init(&dil, dil_profile) == V90_DIL_OK &&
	      v90_dil_training_symbol_count(&dil, &dil_symbols) &&
	      v92_phase3_analog_station_init(&analog, &configuration) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      v92_phase3_digital_station_init(
		      &digital, law, TEST_UCODE, V90_PHASE3_TRN1D_SYMBOLS, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_OK,
	      "V.92 station pair did not initialize for law %u",
	      (unsigned int)law);
	/* Leave room for the final CPt/Ri/E1u/Ri-bar round trips after a full
	 * DIL traversal; the actual five-second CPt deadline is checked below. */
	for (size_t index = 0; index < SAMPLE_LIMIT + dil_symbols + 10u * delay && !finished; ++index) {
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
			uint8_t delivered = delay ? downstream_delay[cursor] : octet;
			enum v92_phase3_analog_station_result result;
			bool waiting_jp_prime =
				analog.receive_stage == V92_PHASE3_ANALOG_RECEIVE_J &&
				analog.transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_CPT;
			struct v92_phase3_analog_station before;

			if (waiting_jp_prime)
				before = analog;
			downstream_delay[cursor] = octet;
			result = v92_phase3_analog_station_receive(
				&analog, delivered, delivered);
			CHECK(result == V92_PHASE3_ANALOG_STATION_OK ||
			      result == V92_PHASE3_ANALOG_STATION_COMPLETE,
			      "downstream receive failed at %zu: %s", index,
			      v92_phase3_analog_station_result_name(result));
			if (waiting_jp_prime && analog.receive_stage == V92_PHASE3_ANALOG_RECEIVE_RI)
				CHECK(memcmp(&analog.transmit, &before.transmit, sizeof(analog.transmit)) == 0 &&
				      memcmp(&analog.transmit_delay, &before.transmit_delay,
					     sizeof(analog.transmit_delay)) == 0 &&
				      check_early_jp_prime(&before, delivered) &&
				      (!dil_symbols || check_jp_prime_deadline(&before, delivered)),
				      "Jp-prime reset training or mishandled its deadline");
		}

		if (!v92_phase3_analog_station_transmit_complete(&analog)) {
			if (analog.transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU &&
			    analog.transmit.su.stage == V92_SU_JP_BAR_RESPONSE &&
			    su_started == SIZE_MAX)
				su_started = index;
			if (dil_symbols && !cpt_started &&
			    analog.transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_CPT &&
			    analog.transmit.cpt.stage == V92_PCM_PHASE3_CPT_TX_RECORD) {
				CHECK(su_started != SIZE_MAX && index - su_started <= 40000u,
				      "CPt missed five-second Su deadline: %zu samples", index - su_started);
				uint32_t trained = analog.transmit.cpt.trn1u.emitted_symbols;

				CHECK(trained >= V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS &&
				      trained % V92_PCM_PHASE3_TRN1U_ALIGNMENT == 0u &&
				      (delay < 4096u ? trained >= dil_symbols :
					      trained == 39948u && index - su_started >= 39980u),
				      "DIL training interval was %u symbols at delay %u",
				      trained, delay);
				cpt_started = true;
			}
			if (analog.transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU &&
			    analog.transmit.su.stage == V92_SU_JP_BAR_RESPONSE &&
			    analog.transmit.su.stage_symbols + 1u == V92_SU_JP_BAR_INPUT_SYMBOLS)
				CHECK(check_su_phase(&analog), "Su handoff failed");
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
					      V92_PHASE3_ANALOG_STATION_OK,
				      "analogue Phase 4 handoff failed");
				trn2u = transmit_handoff;
				trn2u_delay = analog.transmit_delay;
				trn2u_started = true;
			}
			CHECK(v92_pcm_phase4_trn2u_next(&trn2u, &sample) ==
				      V92_PCM_PHASE4_TRN2U_OK &&
			      fractional_delay_process(&trn2u_delay, sample, &sample) == FRACTIONAL_DELAY_OK,
			      "TRN2u generation failed at %zu", index);
		}
		/* An independent central-office sampling phase. Linear interpolation
		 * gives a causal physical channel with a quarter-symbol offset. */
		int16_t current = sample;
		sample = (int16_t)(((int32_t)sample * (65536 - channel_phase) +
			(int32_t)previous_channel_sample * channel_phase) / 65536);
		previous_channel_sample = current;
		sample = media_sample(law, sample);
		if (!v92_phase3_digital_station_receive_complete(&digital)) {
			int16_t delivered = delay ? upstream_delay[cursor] : sample;
			bool measuring_su = digital.receive_stage == V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE;
			bool receiving_ja =
				digital.receive_stage == V92_PHASE3_DIGITAL_RECEIVE_INITIAL &&
				digital.receive.initial.stage == V92_PCM_PHASE3_INITIAL_RX_JA;
			uint16_t expected_phase_minimum = receiving_ja ?
				(uint16_t)fmax(8.0,
					digital.receive.initial.sequence.ja.line.magnitude / 256.0) : 0u;
			enum v92_phase3_digital_station_result result =
				v92_phase3_digital_station_receive(
					&digital, delivered);

			upstream_delay[cursor] = sample;
			CHECK(result == V92_PHASE3_DIGITAL_STATION_OK ||
			      result == V92_PHASE3_DIGITAL_STATION_COMPLETE,
			      "digital receiver returned %s at %zu",
			      v92_phase3_digital_station_result_name(result),
			      index);
			if (measuring_su && digital.receive_stage == V92_PHASE3_DIGITAL_RECEIVE_SU_RESPONSE)
				CHECK(check_trn1u_without_su_reversal(&digital), "TRN1u acquisition failed");
			if (receiving_ja && digital.receive_stage ==
					    V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE)
				CHECK(digital.receive.su.minimum_magnitude == TEST_SU_THRESHOLD &&
				      digital.receive.su.phase_minimum_magnitude ==
					      expected_phase_minimum,
				      "Su did not retain separate sign and measured-level phase thresholds");
		}
		cursor = delay ? (cursor + 1u) % delay : 0u;
		finished =
			v92_phase3_analog_station_transmit_complete(&analog) &&
			v92_phase3_analog_station_receive_complete(&analog) &&
			v92_phase3_digital_station_transmit_complete(&digital) &&
			v92_phase3_digital_station_receive_complete(&digital);
	}
	CHECK(finished && trn2u_started &&
	      !v92_phase3_analog_station_has_failed(&analog) &&
	      !v92_phase3_digital_station_has_failed(&digital) &&
	      analog.transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_TRN2U &&
	      v92_pcm_phase4_trn2u_is_valid(&analog.transmit.trn2u) &&
	      analog.receive.ri.stage == V90_PHASE4_RI_SEARCH_COMPLETE &&
	      digital.downstream.stage == PCM_PHASE3_DIGITAL_TX_COMPLETE &&
	      (dil_symbols ?
		(cpt_started && digital.downstream.dil.parameters.segment_count == dil.segment_count &&
		 analog.transmit.trn2u.emitted_symbols != 0u) :
		(digital.downstream.scr_symbols >= 6u &&
		 digital.downstream.scr_symbols % 6u == 0u)) &&
	      digital.ri.finish_requested &&
	      digital.ri.stage == V90_PHASE4_RI_COMPLETE &&
	      digital.jp.su_phase_extension_q16 != 0u,
	      "V.92 station pair did not reach both Phase 4 boundaries: law=%u delay=%u "
	      "finished=%u trn2u=%u cpt=%u analog=%u/%u digital=%u/%u dil=%u/%u",
	      law, delay, finished, trn2u_started, cpt_started,
	      analog.transmit_stage, analog.receive_stage,
	      digital.downstream.stage, digital.receive_stage,
	      digital.downstream.dil.parameters.segment_count, dil.segment_count);
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
	      transmit_handoff.modulator.lu > 0 &&
	      transmit_handoff.modulator.constellation ==
		      training_constellation &&
	      transmit_handoff.emitted_symbols == analog.transmit.trn2u.emitted_symbols,
	      "V.92 Phase 4 handoff changed negotiated state");
	struct v92_pcm_phase4_trn2u expected = analog.transmit.trn2u;
	for (unsigned i = 0; i < 24; ++i) {
		int16_t actual_sample, expected_sample;

		CHECK(v92_pcm_phase4_trn2u_next(&transmit_handoff, &actual_sample) == V92_PCM_PHASE4_TRN2U_OK &&
		      v92_pcm_phase4_trn2u_next(&expected, &expected_sample) == V92_PCM_PHASE4_TRN2U_OK &&
		      actual_sample == expected_sample, "TRN2u handoff changed sample %u", i);
	}
	{
		struct v92_phase3_analog_station saved;
		int16_t sample = 1234;

		analog.downstream_training_ucode = 0;
		memcpy(&saved, &analog, sizeof(saved));
		CHECK(!v92_phase3_analog_station_transmit_complete(&analog) &&
		      !v92_phase3_analog_station_receive_complete(&analog) &&
		      v92_phase3_analog_station_transmit(&analog, NULL, &sample) ==
			      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
		      v92_phase3_analog_station_receive(&analog, 0xa5, 0xa5) ==
			      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
		      sample == 1234 &&
		      memcmp(&analog, &saved, sizeof(analog)) == 0,
		      "invalid completed analogue state bypassed validation");
	}
	{
		struct v92_phase3_digital_station saved;
		uint8_t octet = 0xa5;

		digital.training_ucode = 0;
		memcpy(&saved, &digital, sizeof(saved));
		CHECK(!v92_phase3_digital_station_transmit_complete(&digital) &&
		      !v92_phase3_digital_station_receive_complete(&digital) &&
		      v92_phase3_digital_station_transmit(&digital, &octet) ==
			      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
		      v92_phase3_digital_station_receive(&digital, 1234) ==
			      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
		      octet == 0xa5 &&
		      memcmp(&digital, &saved, sizeof(digital)) == 0,
		      "invalid completed digital state bypassed validation");
	}
	return true;
}

static bool test_nonzero_station_pair(void)
{
	return run_exchange(G711_LAW_PCMU, true, V90_DIL_PROFILE_CONVENTIONAL, 0) &&
	       run_exchange(G711_LAW_PCMA, true, V90_DIL_PROFILE_CONVENTIONAL, 0) &&
	       run_exchange(G711_LAW_PCMU, false, V90_DIL_PROFILE_CONVENTIONAL, 0) &&
	       run_exchange(G711_LAW_PCMA, false, V90_DIL_PROFILE_CONVENTIONAL, 0) &&
	       run_exchange(G711_LAW_PCMU, 4096u, V90_DIL_PROFILE_CONVENTIONAL, 0) &&
	       run_exchange(G711_LAW_PCMA, 4096u, V90_DIL_PROFILE_CONVENTIONAL, 0);
}

static bool run_delayed_ri(enum g711_law law)
{
	const struct v92_phase3_analog_station_configuration configuration =
		analog_configuration(law, 0xf);
	const struct v92_jd jd = { .data_rate_capability_mask = 1,
		.maximum_spectral_shaping_lookahead = 3 };
	const struct v92_jp jp = { .training_constellation = V92_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation = V92_CONSTELLATION_4_POINT };
	struct v92_phase3_analog_station analog;
	struct v92_phase3_digital_station digital;
	struct pcm_phase3_digital_tx scr = { 0 };
	struct v92_pcm_phase3_trn1u training;
	struct v90_phase4_ri_tx ri;
	bool studying = false;

	CHECK(v92_phase3_analog_station_init(&analog, &configuration) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      v92_phase3_digital_station_init(&digital, law, TEST_UCODE,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, 0.0, TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_OK, "delayed-Ri pair did not initialize");
	for (size_t i = 0; i < SAMPLE_LIMIT && !studying; ++i) {
		uint8_t octet;
		int16_t sample;

		CHECK(v92_phase3_digital_station_transmit(&digital, &octet) ==
			      V92_PHASE3_DIGITAL_STATION_OK &&
		      v92_phase3_analog_station_receive(&analog, octet, octet) ==
			      V92_PHASE3_ANALOG_STATION_OK &&
		      v92_phase3_analog_station_transmit(&analog, NULL, &sample) ==
			      V92_PHASE3_ANALOG_STATION_OK &&
		      v92_phase3_digital_station_receive(&digital, media_sample(law, sample)) ==
			      V92_PHASE3_DIGITAL_STATION_OK, "delayed-Ri startup failed at %zu", i);
		studying = analog.receive_stage == V92_PHASE3_ANALOG_RECEIVE_RI &&
			digital.downstream.stage == PCM_PHASE3_DIGITAL_TX_SCR;
		if (studying)
			scr = digital.downstream;
	}
	CHECK(studying && !scr.finish_scr_requested, "no open SCR boundary");
	training = analog.transmit.cpt.trn1u;
	struct fractional_delay training_delay = analog.transmit_delay;
	/* Continue the actual SCR coding history as a peer still training its
	 * receiver. Drive only wire input; do not inject analogue receive state. */
	for (size_t i = 0; i < 6000; ++i) {
		struct v90_pcm_symbol downstream, expected;
		int16_t sample, expected_sample;

		CHECK(pcm_phase3_digital_tx_next(&scr, &downstream) == PCM_PHASE3_DIGITAL_TX_OK &&
		      v92_phase3_analog_station_receive(&analog, downstream.octet, downstream.octet) ==
			      V92_PHASE3_ANALOG_STATION_OK &&
		      v92_phase3_analog_station_transmit(&analog, NULL, &sample) ==
			      V92_PHASE3_ANALOG_STATION_OK &&
		      v92_pcm_phase3_trn1u_next(&training, &expected) == V92_PCM_PHASE3_TRN1U_OK &&
		      fractional_delay_process(&training_delay, expected.sample, &expected_sample) == FRACTIONAL_DELAY_OK,
		      "extended SCR failed at %zu", i);
		CHECK(!v90_phase4_ri_search_is_acquired(&analog.receive.ri) &&
		      analog.transmit.cpt.stage == V92_PCM_PHASE3_CPT_TX_TRN1U &&
		      sample == expected_sample, "analogue left TRN1u or lost Su history before Ri at %zu", i);
	}
	CHECK(v90_phase4_ri_tx_init(&ri, law, TEST_UCODE) == V90_PHASE4_RI_OK,
	      "delayed Ri did not initialize");
	for (size_t i = 0; i < 192; ++i) {
		uint8_t octet;
		int16_t sample;

		CHECK(v90_phase4_ri_tx_next(&ri, &octet) == V90_PHASE4_RI_OK &&
		      v92_phase3_analog_station_receive(&analog, octet, octet) == V92_PHASE3_ANALOG_STATION_OK &&
		      v92_phase3_analog_station_transmit(&analog, NULL, &sample) ==
			      V92_PHASE3_ANALOG_STATION_OK, "delayed Ri delivery failed at %zu", i);
		CHECK(v90_phase4_ri_search_is_acquired(&analog.receive.ri) ||
		      analog.transmit.cpt.stage == V92_PCM_PHASE3_CPT_TX_TRN1U,
		      "unqualified Ri permitted CPt");
	}
	CHECK(v90_phase4_ri_search_is_acquired(&analog.receive.ri) &&
	      analog.transmit.cpt.stage == V92_PCM_PHASE3_CPT_TX_RECORD &&
	      analog.transmit.cpt.trn1u.emitted_symbols % V92_PCM_PHASE3_TRN1U_ALIGNMENT == 0 &&
	      !analog.transmit.cpt.finish_requested,
	      "Ri did not start aligned CPt, or finished it before Ri-bar");
	return true;
}

static bool test_delayed_ri(void)
{
	return run_delayed_ri(G711_LAW_PCMU) && run_delayed_ri(G711_LAW_PCMA);
}

static bool run_nonzero_dil(enum g711_law law)
{
	/* A scripted peer requests DIL, then sends CPt while DIL is active.
	 * Hold CPt after Ri to distinguish that acknowledgement from E1u. */
	enum { PEER_TRN1U, PEER_JA, PEER_SU, PEER_CPT } peer = PEER_TRN1U;
	struct v92_phase3_digital_station digital;
	struct v92_jd jd = { .data_rate_capability_mask = 1,
		.maximum_spectral_shaping_lookahead = 1 };
	struct v92_jp jp = { .training_constellation = V92_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation = V92_CONSTELLATION_4_POINT };
	struct v92_pcm_dil_parameters request = { .upstream_rate_mask = 0xf };
	struct v92_pcm_phase3_trn1u initial;
	struct v92_pcm_phase3_ja_tx ja;
	struct v92_su su;
	struct fractional_delay delay;
	struct v92_pcm_phase3_cpt_tx cpt;
	struct v92_pcm_phase4_trn2u trn2u;
	struct v92_cpt_parameters parameters = cpt_parameters(law), received;
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v90_pcm_symbol level;
	size_t ri_symbols = 0;
	bool mid_segment_finish = false, e1u_started = false, finished = false;
	bool trn2u_started = false;
	bool trn1u_overlapped_jp = false;

	parameters.spectral_lookahead = jd.maximum_spectral_shaping_lookahead;
	fractional_delay_init(&delay);
	CHECK(v90_dil_profile_init(&request.downstream,
		      V90_DIL_PROFILE_CONVENTIONAL) == V90_DIL_OK &&
	      v92_phase3_digital_station_init(&digital, law, TEST_UCODE, V90_PHASE3_TRN1D_SYMBOLS, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) == V92_PHASE3_DIGITAL_STATION_OK &&
	      v92_pcm_phase3_trn1u_init(&initial, law, TEST_UCODE) ==
		      V92_PCM_PHASE3_TRN1U_OK &&
	      v90_pcm_symbol_make(&level, law, TEST_UCODE, true),
	      "nonzero-DIL peer did not initialize");
	for (size_t index = 0; index < 60000 && !finished; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t octet;
		int16_t sample;

		if (!v92_phase3_digital_station_transmit_complete(&digital))
			CHECK(v92_phase3_digital_station_transmit(&digital, &octet) ==
				      V92_PHASE3_DIGITAL_STATION_OK,
			      "nonzero DIL transmit failed at %zu", index);
		if (peer == PEER_TRN1U && initial.emitted_symbols == 2040) {
			CHECK(v92_pcm_phase3_ja_tx_init_dil(&ja, &initial, &request) ==
				      V92_PCM_PHASE3_JA_OK, "Ja coding handoff failed");
			peer = PEER_JA;
		}
		if (peer == PEER_JA && digital.downstream.jd_records != 0 &&
		    v92_pcm_phase3_ja_tx_can_stop(&ja)) {
			CHECK(v92_su_init(&su, level.sample) == V92_SU_OK &&
			      memcmp(&digital.downstream.dil.parameters, &request.downstream,
				     sizeof(request.downstream)) == 0 &&
			      digital.upstream_rate_mask == request.upstream_rate_mask,
			      "Ja handoff lost the learning request");
			peer = PEER_SU;
		}
		if (peer == PEER_SU) {
			if (su.stage == V92_SU_JP_WAIT && digital.downstream.jp_records != 0)
				CHECK(v92_su_accept_jp(&su, &digital.jp) == V92_SU_OK,
				      "peer could not acknowledge Jp");
			if (su.stage == V92_SU_TRN1U_READY) {
				CHECK(v92_su_finish(&su, &delay, true) == V92_SU_COMPLETE &&
				      v92_pcm_phase3_cpt_tx_init(&cpt, law, TEST_UCODE, &parameters) ==
					      V92_PCM_PHASE3_CPT_TX_OK,
				      "peer could not enter DIL study");
				peer = PEER_CPT;
				trn1u_overlapped_jp = digital.downstream.stage <
					PCM_PHASE3_DIGITAL_TX_DIL;
			}
		}
		if (peer == PEER_TRN1U) {
			CHECK(v92_pcm_phase3_trn1u_next(&initial, &symbol) ==
				      V92_PCM_PHASE3_TRN1U_OK, "initial TRN1u failed");
			sample = symbol.sample;
		} else if (peer == PEER_JA) {
			CHECK(v92_pcm_phase3_ja_tx_next(&ja, &symbol) == V92_PCM_PHASE3_JA_OK,
			      "nonzero Ja generation failed");
			sample = symbol.sample;
		} else if (peer == PEER_SU) {
			enum v92_su_result result = v92_su_next(&su, &delay, &sample);
			CHECK(result == V92_SU_OK || result == V92_SU_REACHED_JP_WAIT ||
			      result == V92_SU_REACHED_TRN1U_READY, "Su generation failed");
		} else {
			if (cpt.stage == V92_PCM_PHASE3_CPT_TX_TRN1U &&
			    cpt.trn1u.emitted_symbols == 6000) {
				CHECK(digital.downstream.stage == PCM_PHASE3_DIGITAL_TX_DIL &&
				      !digital.downstream.dil.finish_requested &&
				      digital.receive_stage == V92_PHASE3_DIGITAL_RECEIVE_CPT &&
				      v92_pcm_phase3_cpt_tx_start(&cpt) == V92_PCM_PHASE3_CPT_TX_OK,
				      "DIL did not wait for CPt: rx=%u tx=%u overlap=%u",
				      digital.receive_stage, digital.downstream.stage,
				      trn1u_overlapped_jp);
			}
			if (digital.transmit_stage == V92_PHASE3_DIGITAL_TRANSMIT_RI &&
			    !e1u_started) {
				CHECK(!digital.ri.finish_requested, "Ri-bar preceded peer E1u");
				if (++ri_symbols == 3000) {
					CHECK(v92_pcm_phase3_cpt_tx_request_finish(&cpt) ==
						      V92_PCM_PHASE3_CPT_TX_OK,
					      "peer could not acknowledge Ri with E1u");
					e1u_started = true;
				}
			}
			if (!v92_pcm_phase3_cpt_tx_is_complete(&cpt)) {
				CHECK(v92_pcm_phase3_cpt_tx_next(&cpt, &symbol) ==
					      V92_PCM_PHASE3_CPT_TX_OK, "CPt/E1u generation failed");
				sample = symbol.sample;
			} else {
				if (!trn2u_started) {
					CHECK(v92_pcm_phase4_trn2u_init(&trn2u, jp.training_constellation,
						      cpt.coding.previous_sign, level.sample) ==
						      V92_PCM_PHASE4_TRN2U_OK, "TRN2u handoff failed");
					trn2u_started = true;
				}
				CHECK(v92_pcm_phase4_trn2u_next(&trn2u, &sample) ==
					      V92_PCM_PHASE4_TRN2U_OK, "TRN2u generation failed");
			}
		}
		if (peer != PEER_SU)
			CHECK(fractional_delay_process(&delay, sample, &sample) ==
				      FRACTIONAL_DELAY_OK, "peer sample delay failed");
		{
			enum v92_phase3_digital_station_result result =
				v92_phase3_digital_station_receive(&digital, media_sample(law, sample));
			CHECK(result == V92_PHASE3_DIGITAL_STATION_OK ||
			      result == V92_PHASE3_DIGITAL_STATION_COMPLETE,
			      "nonzero-DIL receive failed at %zu: %s", index,
			      v92_phase3_digital_station_result_name(result));
		}
		mid_segment_finish |= digital.downstream.stage == PCM_PHASE3_DIGITAL_TX_DIL &&
			digital.downstream.dil.finish_requested;
		finished = v92_phase3_digital_station_receive_complete(&digital) &&
			v92_phase3_digital_station_transmit_complete(&digital);
	}
	CHECK(finished && trn1u_overlapped_jp &&
	      mid_segment_finish && e1u_started && ri_symbols == 3000 &&
	      digital.downstream.scr_symbols == 0 && !digital.downstream.finish_scr_requested &&
	      digital.downstream.dil.complete && digital.ri.finish_requested &&
	      v92_phase3_digital_station_downstream_handoff(&digital, &received) ==
		      V92_PHASE3_DIGITAL_STATION_OK && parameters_match(&received, &parameters) &&
	      v92_phase3_digital_station_receive_handoff(&digital, &handoff) ==
		      V92_PHASE3_DIGITAL_STATION_OK &&
	      v92_pcm_phase3_cpt_line_handoff_is_valid(&handoff),
	      "nonzero-DIL exchange did not complete the negotiated handoffs");
	{
		struct v92_pcm_phase4_trn2u_line_rx receiver;

		CHECK(digital.early_trn2u_samples >= V90_PHASE4_RI_BAR_SYMBOLS &&
		      v92_pcm_phase4_trn2u_line_rx_init(
			      &receiver, &handoff, jp.training_constellation, handoff.parameters.law) ==
			      V92_PCM_PHASE4_TRN2U_LINE_RX_OK,
		      "Ri-bar overlap did not retain early TRN2u");
		for (size_t index = 0; index < digital.early_trn2u_samples; ++index)
			CHECK(v92_pcm_phase4_trn2u_line_rx_sample(&receiver,
				      digital.early_trn2u[index]) == V92_PCM_PHASE4_TRN2U_LINE_RX_OK,
			      "buffered TRN2u handoff failed at %zu", index);
		for (size_t index = 0; index < 240; ++index) {
			int16_t sample;
			CHECK(v92_pcm_phase4_trn2u_next(&trn2u, &sample) == V92_PCM_PHASE4_TRN2U_OK &&
			      fractional_delay_process(&delay, sample, &sample) == FRACTIONAL_DELAY_OK &&
			      v92_pcm_phase4_trn2u_line_rx_sample(&receiver, media_sample(law, sample)) ==
				      V92_PCM_PHASE4_TRN2U_LINE_RX_OK,
			      "live TRN2u continuation failed at %zu", index);
		}
		CHECK(receiver.symbol_errors == 0 && receiver.coding_errors == 0,
		      "Ri-bar overlap lost the TRN2u epoch: %u symbol / %u coding errors",
		      receiver.symbol_errors, receiver.coding_errors);
		while (digital.early_trn2u_samples < ARRAY_LENGTH(digital.early_trn2u))
			CHECK(v92_phase3_digital_station_receive(&digital, 0) ==
				      V92_PHASE3_DIGITAL_STATION_COMPLETE,
			      "bounded overlap rejected a permitted sample");
		CHECK(v92_phase3_digital_station_receive(&digital, 0) ==
			      V92_PHASE3_DIGITAL_STATION_SEQUENCE_MISMATCH &&
		      v92_phase3_digital_station_has_failed(&digital),
		      "unpaced overlap exceeded its bound");
	}
	return true;
}

static bool test_nonzero_dil(void)
{
	return run_nonzero_dil(G711_LAW_PCMU) &&
	       run_nonzero_dil(G711_LAW_PCMA);
}

static bool test_analog_contracts(void)
{
	struct v92_phase3_analog_station_configuration configuration =
		analog_configuration(G711_LAW_PCMU, 1u);
	struct v92_phase3_analog_station station;
	struct v92_phase3_analog_station before;
	struct v92_pcm_phase4_trn2u handoff = {
		.modulator = {
			.lu = 123,
			.constellation = (enum v92_constellation_size)16,
			.previous_encoded_sign = 1,
		},
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
	{
		struct v92_phase3_analog_station checking = station;

		checking.receive_stage = V92_PHASE3_ANALOG_RECEIVE_J;
		CHECK(pcm_phase3_j_receiver_init(&checking.receive.j,
			      PCM_PROTOCOL_V92, checking.law,
			      checking.downstream_training_ucode) ==
			      PCM_PHASE3_J_RECEIVER_OK &&
		      v92_phase3_analog_station_is_valid(&checking),
		      "V.92 station rejected its selected J receiver");
		checking.receive.j.protocol = PCM_PROTOCOL_V90;
		CHECK(pcm_phase3_j_receiver_is_valid(&checking.receive.j) &&
		      !v92_phase3_analog_station_is_valid(&checking),
		      "V.92 station accepted a V.90 receiver");
	}
	station.dil_profile = V90_DIL_PROFILE_CONVENTIONAL;
	before = station;
	CHECK(!v92_phase3_analog_station_is_valid(&station) &&
	      v92_phase3_analog_station_transmit(&station, NULL, &sample) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      memcmp(&station, &before, sizeof(station)) == 0 && sample == 1234,
	      "station accepted a DIL selection different from its opening");
	station.dil_profile = configuration.dil_profile;
	station.post_ja_silence_samples = 1u;
	before = station;
	CHECK(!v92_phase3_analog_station_is_valid(&station) &&
	      v92_phase3_analog_station_transmit(&station, NULL, &sample) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      memcmp(&station, &before, sizeof(station)) == 0,
	      "silence clock before Ja ended was accepted");
	station.post_ja_silence_samples = 0u;
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
	      handoff.modulator.lu == 123 &&
	      handoff.modulator.constellation ==
		      (enum v92_constellation_size)16 &&
	      handoff.modulator.previous_encoded_sign == 1u &&
	      v92_phase3_analog_station_transmit(&station, NULL, &sample) ==
		      V92_PHASE3_ANALOG_STATION_OK &&
	      sample == 0,
	      "fresh analogue transmission contracts differed");
	before = station;
	CHECK(v92_phase3_analog_station_transmit(NULL, NULL, &sample) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v92_phase3_analog_station_transmit(&station, NULL, NULL) ==
		      V92_PHASE3_ANALOG_STATION_INVALID_ARGUMENT &&
	      v92_phase3_analog_station_receive(NULL, 0u, 0u) ==
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
		      G711_LAW_PCMU, TEST_UCODE, V90_PHASE3_TRN1D_SYMBOLS, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) &&
	      !v92_phase3_digital_station_parameters_are_valid(
		      G711_LAW_PCMU, TEST_UCODE, V90_PHASE3_TRN1D_SYMBOLS, NAN,
		      TEST_SU_THRESHOLD, &jd, &jp) &&
	      v92_phase3_digital_station_init(
		      NULL, G711_LAW_PCMU, TEST_UCODE, V90_PHASE3_TRN1D_SYMBOLS, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase3_digital_station_init(
		      &station, G711_LAW_PCMU, TEST_UCODE, V90_PHASE3_TRN1D_SYMBOLS, NAN,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      memcmp(&station, &before, sizeof(station)) == 0,
	      "invalid digital initialization changed its destination");
	CHECK(v92_phase3_digital_station_init(
		      &station, G711_LAW_PCMU, TEST_UCODE, V90_PHASE3_TRN1D_SYMBOLS, 0.0,
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

static bool test_fractional_channel(void)
{
	return run_exchange(G711_LAW_PCMU, 3u, V90_DIL_PROFILE_NONE, 16384) &&
	       run_exchange(G711_LAW_PCMA, 3u, V90_DIL_PROFILE_NONE, 16384);
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
		{ "zero-DIL waits for delayed Ri", test_delayed_ri },
		{ "nonzero-DIL station exchange", test_nonzero_dil },
		{ "native nonzero-DIL station pair", test_nonzero_station_pair },
		{ "analogue station contracts", test_analog_contracts },
		{ "manufacturer interval backpressure", test_md_backpressure },
		{ "digital station contracts", test_digital_contracts },
		{ "independent fractional sampling phase", test_fractional_channel },
	};
	unsigned int failures = 0u;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (index == 0u)
			passed = run_exchange(G711_LAW_PCMU, false, V90_DIL_PROFILE_NONE, 0);
		else if (index == 1u)
			passed = run_exchange(G711_LAW_PCMA, false, V90_DIL_PROFILE_NONE, 0);
		else if (index == 2u)
			passed = run_exchange(G711_LAW_PCMU, true, V90_DIL_PROFILE_NONE, 0);
		else if (index == 3u)
			passed = run_exchange(G711_LAW_PCMA, true, V90_DIL_PROFILE_NONE, 0);
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
