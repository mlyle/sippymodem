// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase4_analog_station.h>

#include <v90_phase3.h>
#include <v90_pcm_symbol.h>

#include "test.h"

#include <string.h>

enum {
	NOMINAL_AMPLITUDE = 6000,
	TRAINING_UCODE = 91,
	SAMPLE_LIMIT = 10000,
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

static struct v90_jd_parameters jd_parameters(
	enum v34_constellation_size constellation)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = 0x15555u,
		.training_constellation = constellation,
		.rate_renegotiation_constellation =
			V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 2,
	};
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters training_parameters(enum g711_law law)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = law,
		.upstream_rate_mask = 0x1555u,
		.trn1d_ratio_q3_13 = 0x2000u,
		.shaping_a1_q1_6 = -16,
		.shaping_a2_q1_6 = 8,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = 31,
		.selected_rate_index = 13,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
	};

	for (uint8_t index = 0; index < 8; ++index)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * index));
	return parameters;
}

static bool mapping_matches(
	const struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters)
{
	struct v90_pcm_mapping expected;

	return v90_pcm_mapping_init(&expected, parameters) ==
		       V90_PCM_MAPPING_OK &&
	       memcmp(mapping, &expected, sizeof(expected)) == 0;
}

static struct qam_point warmup_point(size_t point_index)
{
	return (struct qam_point){
		.in_phase = point_index % 2u == 0 ? 0.5 : -0.5,
		.quadrature = point_index % 3u == 0 ? 0.5 : -0.5,
	};
}

static bool prepare_line(struct v34_training_line *line)
{
	struct v34_phase3_profile selected = profile();
	size_t accepted_points = 0;

	if (v34_training_line_init(
		    line, &selected, V90_PHASE3_SAMPLE_RATE,
		    NOMINAL_AMPLITUDE) != V34_TRAINING_LINE_OK)
		return false;
	for (size_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		struct qam_point point = warmup_point(accepted_points);
		bool accepting = v34_training_line_needs_point(line);
		bool accepted;
		int16_t sample;

		if (accepted_points >= 25u && accepting)
			return true;
		if (v34_training_line_sample(
			    line, accepting ? &point : NULL,
			    &sample, &accepted) != V34_TRAINING_LINE_OK ||
		    accepted != accepting)
			return false;
		if (accepted)
			accepted_points++;
	}
	return false;
}

static bool model_sample(
	struct v90_phase4_cp_line_tx *transmit, int16_t *sample)
{
	return v90_phase4_cp_line_tx_sample(transmit, sample) ==
	       V90_PHASE4_CP_LINE_TX_OK;
}

static bool ri_receive_matches(
	const struct v90_phase4_ri_rx *left,
	const struct v90_phase4_ri_rx *right)
{
	return left->stage == right->stage &&
	       left->law == right->law &&
	       left->ri_symbols == right->ri_symbols &&
	       left->ri_bar_symbols == right->ri_bar_symbols &&
	       left->phase == right->phase &&
	       left->training_ucode == right->training_ucode &&
	       left->polarity_known == right->polarity_known &&
	       left->inverted == right->inverted;
}

static bool run_exchange(
	enum g711_law law, enum v34_constellation_size constellation)
{
	struct v90_jd_parameters jd = jd_parameters(constellation);
	struct v90_cp_parameters training = training_parameters(law);
	struct v90_cp_parameters data = training;
	struct v34_training_line line;
	struct v90_phase4_analog_station station;
	struct v90_phase4_cp_line_tx model;
	struct v90_phase4_ri_tx ri;
	struct v90_phase4_analog_receive_handoff receive_handoff;
	unsigned int records_started = 1;
	unsigned int records_at_transition = 0;
	bool finish_requested = false;
	bool ri_finish_requested = false;
	bool transition_seen = false;
	bool receive_handoff_checked = false;
	bool receive_finished_first = false;
	bool complete = false;

	CHECK(prepare_line(&line), "Phase 4 line fixture did not initialize");
	CHECK(v90_phase4_analog_station_init(
		      &station, &line, &jd, law, TRAINING_UCODE,
		      &training) == V90_PHASE4_ANALOG_STATION_OK &&
	      v90_phase4_cp_line_tx_init(
		      &model, &line, constellation, &training) ==
		      V90_PHASE4_CP_LINE_TX_OK &&
	      v90_phase4_ri_tx_init(&ri, law, TRAINING_UCODE) ==
		      V90_PHASE4_RI_OK,
	      "Phase 4 station fixture did not initialize");

	for (size_t clock = 0; clock < SAMPLE_LIMIT; ++clock) {
		if (!ri_finish_requested &&
		    ri.ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS &&
		    records_started >= 2u &&
		    !v90_phase4_cp_line_tx_record_is_complete(&model) &&
		    model.symbols.bit_index >= 64u &&
		    model.symbols.frame_bit_count -
			    model.symbols.bit_index >= 128u) {
			CHECK(v90_phase4_ri_tx_request_finish(&ri) ==
				      V90_PHASE4_RI_OK,
			      "digital side did not accept the CPt event");
			ri_finish_requested = true;
		}

		if (!v90_phase4_ri_tx_is_complete(&ri)) {
			uint8_t octet;
			bool previously_requested =
				station.finish_cpt_requested;

			CHECK(v90_phase4_ri_tx_next(&ri, &octet) ==
				      V90_PHASE4_RI_OK &&
			      v90_phase4_analog_station_receive(
				      &station, octet) ==
				      V90_PHASE4_ANALOG_STATION_OK,
			      "Ri exchange failed at clock %zu", clock);
			if (!previously_requested &&
			    station.finish_cpt_requested) {
				CHECK(!v90_phase4_cp_line_tx_record_is_complete(
					      &model),
				      "Ri-bar arrived at an untested CPt boundary");
				finish_requested = true;
				transition_seen = true;
				records_at_transition = records_started;
			}
		}

		if (v90_phase4_analog_station_receive_complete(&station) &&
		    !receive_handoff_checked) {
			receive_finished_first =
				!v90_phase4_analog_station_transmit_complete(
					&station);
			CHECK(v90_phase4_analog_station_receive_handoff(
				      &station, &receive_handoff) ==
				      V90_PHASE4_ANALOG_STATION_OK &&
			      v90_phase4_analog_receive_handoff_is_valid(
				      &receive_handoff) &&
			      pcm_downstream_rx_is_valid(
				      &receive_handoff.receiver) &&
			      receive_handoff.receiver.stream.decoder
				      .decision_plane ==
				      V90_PCM_MAPPING_TRANSMIT &&
			      mapping_matches(
				      &receive_handoff.receiver.stream.decoder.mapping,
				      &training) &&
			      pcm_downstream_rx_is_fresh(
				      &receive_handoff.receiver),
			      "TRN2d handoff was not fresh at the Ri boundary");
			{
				struct v90_phase4_analog_receive_handoff corrupted =
					receive_handoff;

				corrupted.receiver.stream.descrambler.history = 1;
				CHECK(!v90_phase4_analog_receive_handoff_is_valid(
					      &corrupted),
				      "handoff accepted a used descrambler epoch");
				corrupted = receive_handoff;
				corrupted.receiver.stream.decoder.signs
					.previous_shaping_signs[0] = 1;
				CHECK(!v90_phase4_analog_receive_handoff_is_valid(
					      &corrupted),
				      "handoff accepted used sign history");
				corrupted = receive_handoff;
				corrupted.receiver.control.mp.sync_one_count = 1;
				CHECK(!v90_phase4_analog_receive_handoff_is_valid(
					      &corrupted),
				      "handoff accepted a used MP synchronizer");
			}
			receive_handoff_checked = true;
		}

		if (!v90_phase4_analog_station_transmit_complete(&station)) {
			int16_t actual;
			int16_t expected;

			if (v90_phase4_cp_line_tx_record_is_complete(&model) &&
			    !finish_requested) {
				CHECK(v90_phase4_cp_line_tx_start_next_record(
					      &model, &training) ==
					      V90_PHASE4_CP_LINE_TX_OK,
				      "CPt model did not repeat");
				records_started++;
			}
			CHECK(!(finish_requested &&
				v90_phase4_cp_line_tx_is_handoff_ready(&model)),
			      "model completed before station predicate");
			CHECK(model_sample(&model, &expected) &&
			      v90_phase4_analog_station_transmit(
				      &station, &actual) ==
				      V90_PHASE4_ANALOG_STATION_OK &&
			      actual == expected &&
			      memcmp(&station.transmit, &model,
				     sizeof(model)) == 0,
			      "CPt waveform diverged at clock %zu", clock);
		}

		if (v90_phase4_analog_station_transmit_complete(&station) &&
		    v90_phase4_analog_station_receive_complete(&station)) {
			complete = true;
			break;
		}
	}

	CHECK(complete && ri_finish_requested && transition_seen &&
	      receive_handoff_checked && receive_finished_first &&
	      records_started >= 2u &&
	      records_started == records_at_transition &&
	      v90_phase4_cp_line_tx_is_handoff_ready(&model),
	      "initial Phase 4 exchange did not stop at the current CPt");
	{
		struct v90_phase4_cp_line_tx transmit_handoff;
		struct v90_phase4_analog_station saved = station;
		uint32_t history;
		uint8_t rotation;
		int16_t sample = 1234;

		CHECK(v90_phase4_analog_station_transmit_handoff(
			      &station, &transmit_handoff) ==
			      V90_PHASE4_ANALOG_STATION_OK &&
		      memcmp(&transmit_handoff, &model, sizeof(model)) == 0 &&
		      v90_phase4_analog_station_transmit(
			      &station, &sample) ==
			      V90_PHASE4_ANALOG_STATION_COMPLETE &&
		      memcmp(&station, &saved, sizeof(station)) == 0 &&
		      sample == 1234,
		      "completed CPt direction was not transactional");
		history = transmit_handoff.symbols.scrambler.history;
		rotation = transmit_handoff.symbols
			.previous_clockwise_quarter_turns;
		data.kind = V90_CP_DATA;
		data.selected_rate_index = 1;
		data.acknowledges_peer = true;
		CHECK(v90_phase4_cp_line_tx_start_next_record(
			      &transmit_handoff, &data) ==
			      V90_PHASE4_CP_LINE_TX_OK &&
		      transmit_handoff.symbols.scrambler.history == history &&
		      transmit_handoff.symbols
			      .previous_clockwise_quarter_turns == rotation,
		      "CP handoff reset continuous coding history");
	}
	{
		struct v90_phase4_analog_station saved = station;

		CHECK(v90_phase4_analog_station_receive(&station, 0xa5) ==
			      V90_PHASE4_ANALOG_STATION_COMPLETE &&
		      memcmp(&station, &saved, sizeof(station)) == 0,
		      "completed Ri direction consumed another character");
	}
	return true;
}

static bool resume_after_ri_transition(enum g711_law law)
{
	struct v90_jd_parameters jd =
		jd_parameters(V34_CONSTELLATION_4_POINT);
	struct v90_cp_parameters training = training_parameters(law);
	struct v34_training_line line;
	struct v90_phase4_ri_tx transmit;
	struct v90_phase4_ri_rx receive;
	struct v90_phase4_analog_station station;
	bool transition_seen = false;

	CHECK(prepare_line(&line) &&
	      v90_phase4_ri_tx_init(&transmit, law, TRAINING_UCODE) ==
		      V90_PHASE4_RI_OK &&
	      v90_phase4_ri_rx_init(&receive, law, TRAINING_UCODE) ==
		      V90_PHASE4_RI_OK &&
	      v90_phase4_ri_tx_request_finish(&transmit) ==
		      V90_PHASE4_RI_OK,
	      "law %u resume fixture did not initialize",
	      (unsigned int)law);
	for (size_t index = 0;
	     index < V90_PHASE4_RI_MIN_SYMBOLS + V90_PHASE4_RI_PERIOD;
	     ++index) {
		uint8_t octet;
		enum v90_phase4_ri_result result;

		CHECK(v90_phase4_ri_tx_next(&transmit, &octet) ==
			      V90_PHASE4_RI_OK,
		      "law %u Ri prefix stopped at %zu",
		      (unsigned int)law, index);
		result = v90_phase4_ri_rx_receive(&receive, octet);
		CHECK(result == V90_PHASE4_RI_OK ||
		      result == V90_PHASE4_RI_TRANSITION_DETECTED,
		      "law %u Ri prefix was rejected at %zu",
		      (unsigned int)law, index);
		if (result == V90_PHASE4_RI_TRANSITION_DETECTED) {
			transition_seen = true;
			break;
		}
	}
	CHECK(transition_seen &&
	      v90_phase4_analog_station_resume(
		      &station, &line, &receive, &jd, &training) ==
		      V90_PHASE4_ANALOG_STATION_OK &&
	      station.finish_cpt_requested &&
	      ri_receive_matches(&station.receive, &receive),
	      "law %u advanced Ri receiver was not preserved",
	      (unsigned int)law);
	{
		struct v90_phase4_ri_rx model = receive;
		uint8_t octet;

		CHECK(v90_phase4_ri_tx_next(&transmit, &octet) ==
			      V90_PHASE4_RI_OK &&
		      v90_phase4_ri_rx_receive(&model, octet) ==
			      V90_PHASE4_RI_OK &&
		      v90_phase4_analog_station_receive(&station, octet) ==
			      V90_PHASE4_ANALOG_STATION_OK &&
		      ri_receive_matches(&station.receive, &model),
		      "law %u resumed Ri receiver replayed its prefix",
		      (unsigned int)law);
	}
	return true;
}

static bool test_directional_resume(void)
{
	CHECK(resume_after_ri_transition(G711_LAW_PCMU) &&
	      resume_after_ri_transition(G711_LAW_PCMA),
	      "advanced Ri receiver resume failed");
	return true;
}

static bool test_initial_exchange(void)
{
	CHECK(run_exchange(G711_LAW_PCMU, V34_CONSTELLATION_4_POINT),
	      "PCMU four-point exchange failed");
	CHECK(run_exchange(G711_LAW_PCMA, V34_CONSTELLATION_16_POINT),
	      "PCMA sixteen-point exchange failed");
	return true;
}

static bool test_contracts(void)
{
	struct v90_jd_parameters jd =
		jd_parameters(V34_CONSTELLATION_4_POINT);
	struct v90_jd_parameters bad_jd = jd;
	struct v90_cp_parameters training = training_parameters(G711_LAW_PCMU);
	struct v90_cp_parameters bad = training;
	struct v34_training_line line;
	struct v34_training_line bad_line;
	struct v90_phase4_analog_station station;
	struct v90_phase4_analog_station untouched;
	struct v90_phase4_analog_station untouched_saved;
	struct v90_phase4_cp_line_tx transmit_handoff;
	struct v90_phase4_cp_line_tx transmit_saved;
	struct v90_phase4_analog_receive_handoff receive_handoff;
	struct v90_phase4_analog_receive_handoff receive_saved;
	struct v90_pcm_symbol wrong;
	int16_t sample = 777;

	CHECK(prepare_line(&line), "validation line did not initialize");
	bad_line = line;
	bad_line.profile.local_role = V34_CALLING_STATION;
	bad_jd.maximum_spectral_shaping_lookahead = 0;
	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	CHECK(v90_phase4_analog_station_init(
		      NULL, &line, &jd, G711_LAW_PCMU, TRAINING_UCODE,
		      &training) == V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
	      v90_phase4_analog_station_init(
		      &untouched, &line, NULL, G711_LAW_PCMU,
		      TRAINING_UCODE, &training) ==
		      V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
	      v90_phase4_analog_station_init(
		      &untouched, &line, &bad_jd, G711_LAW_PCMU,
		      TRAINING_UCODE, &training) ==
		      V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
	      v90_phase4_analog_station_init(
		      &untouched, &bad_line, &jd, G711_LAW_PCMU,
		      TRAINING_UCODE, &training) ==
		      V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "rejected station input changed its destination");
	bad.kind = V90_CP_DATA;
	CHECK(v90_phase4_analog_station_init(
		      &untouched, &line, &jd, G711_LAW_PCMU,
		      TRAINING_UCODE, &bad) ==
		      V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS,
	      "station accepted CP in place of CPt");
	bad = training;
	bad.law = G711_LAW_PCMA;
	CHECK(v90_phase4_analog_station_init(
		      &untouched, &line, &jd, G711_LAW_PCMU,
		      TRAINING_UCODE, &bad) ==
		      V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS,
	      "station accepted a mismatched codec law");
	bad = training;
	bad.spectral_lookahead = 3;
	CHECK(v90_phase4_analog_station_init(
		      &untouched, &line, &jd, G711_LAW_PCMU,
		      TRAINING_UCODE, &bad) ==
		      V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "station accepted unsupported spectral lookahead");
	bad = training;
	memset(bad.transmit_constellations[0].ucode_mask, 0,
	       sizeof(bad.transmit_constellations[0].ucode_mask));
	add_ucode(&bad.transmit_constellations[0], 12);
	add_ucode(&bad.transmit_constellations[0], TRAINING_UCODE);
	add_ucode(&bad.transmit_constellations[0], 119);
	CHECK(v90_phase4_analog_station_init(
		      &untouched, &line, &jd, G711_LAW_PCMU,
		      TRAINING_UCODE, &bad) ==
		      V90_PHASE4_ANALOG_STATION_INVALID_PARAMETERS &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "station accepted an undersized CPt mapping");

	CHECK(v90_phase4_analog_station_init(
		      &station, &line, &jd, G711_LAW_PCMU,
		      TRAINING_UCODE, &training) ==
		      V90_PHASE4_ANALOG_STATION_OK,
	      "valid station did not initialize");
	memset(&transmit_handoff, 0x5a, sizeof(transmit_handoff));
	transmit_saved = transmit_handoff;
	memset(&receive_handoff, 0x5a, sizeof(receive_handoff));
	receive_saved = receive_handoff;
	{
		struct v90_phase4_analog_station saved = station;

		CHECK(v90_phase4_analog_station_transmit(NULL, &sample) ==
			      V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
		      v90_phase4_analog_station_transmit(&station, NULL) ==
			      V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
		      v90_phase4_analog_station_receive(NULL, 0) ==
			      V90_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
		      v90_phase4_analog_station_transmit_handoff(
			      &station, &transmit_handoff) ==
			      V90_PHASE4_ANALOG_STATION_INVALID_STATE &&
		      v90_phase4_analog_station_receive_handoff(
			      &station, &receive_handoff) ==
			      V90_PHASE4_ANALOG_STATION_INVALID_STATE &&
		      memcmp(&station, &saved, sizeof(station)) == 0 &&
		      memcmp(&transmit_handoff, &transmit_saved,
			     sizeof(transmit_handoff)) == 0 &&
		      memcmp(&receive_handoff, &receive_saved,
			     sizeof(receive_handoff)) == 0 &&
		      sample == 777,
		      "rejected station operation was not transactional");
	}

	CHECK(v90_pcm_symbol_make(
		      &wrong, G711_LAW_PCMU, TRAINING_UCODE - 1u,
		      true),
	      "invalid-sequence character did not initialize");
	{
		struct v90_phase4_analog_station expected = station;

		expected.failed = true;
		CHECK(v90_phase4_analog_station_receive(
			      &station, wrong.octet) ==
			      V90_PHASE4_ANALOG_STATION_SEQUENCE_MISMATCH &&
		      v90_phase4_analog_station_has_failed(&station) &&
		      memcmp(&station, &expected, sizeof(station)) == 0,
		      "invalid Ri did not latch a transactional failure");
	}
	CHECK(v90_phase4_analog_station_transmit(&station, &sample) ==
		      V90_PHASE4_ANALOG_STATION_FAILED &&
	      v90_phase4_analog_station_receive(&station, 0) ==
		      V90_PHASE4_ANALOG_STATION_FAILED &&
	      v90_phase4_analog_station_transmit_handoff(
		      &station, &transmit_handoff) ==
		      V90_PHASE4_ANALOG_STATION_FAILED &&
	      v90_phase4_analog_station_receive_handoff(
		      &station, &receive_handoff) ==
		      V90_PHASE4_ANALOG_STATION_FAILED &&
	      sample == 777,
	      "failed station accepted more work");
	CHECK(!v90_phase4_analog_station_is_valid(NULL) &&
	      !v90_phase4_analog_station_has_failed(NULL) &&
	      !v90_phase4_analog_receive_handoff_is_valid(NULL) &&
	      strcmp(v90_phase4_analog_station_result_name(
		     V90_PHASE4_ANALOG_STATION_SEQUENCE_MISMATCH),
		     "sequence mismatch") == 0 &&
	      strcmp(v90_phase4_analog_station_result_name(
		     (enum v90_phase4_analog_station_result)99),
		     "unknown") == 0,
	      "station validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "initial exchange", test_initial_exchange },
		{ "advanced Ri receive resume", test_directional_resume },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
